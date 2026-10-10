/* Asset thumbnails (studio_thumbnails.h): a fixed table of slots, each a
 * canvas bound as the still image "kit:thumb:<slot>". An emission asks for
 * the thumbnails it shows; after it, queued ones render within a frame
 * budget: pictures the disk cache holds first, then large previews, then the
 * cheapest kinds. A thumbnail keeps its slot while emissions ask for it; a
 * new one takes a free slot or the one asked for least recently.
 *
 * Each knows the stamp of the content it shows: a hash of the asset's text
 * and of the texts it uses, directly or through one asset, each an open
 * document's unsaved edits when it has them, else its file. It renders
 * again, keeping its old picture meanwhile, once a check finds another
 * stamp. Checks run a second apart, or as soon as edits have rested for
 * EDIT_REST_SECONDS with no mouse button held, so a drag or a scrub never
 * renders mid-gesture. A finished picture of saved content goes to the disk
 * cache under its stamp, read back from the GPU a frame after it was drawn. */
#include "editor/studio/studio_thumbnails.h"

#include "editor/documents/ui_buffer.h"
#include "editor/studio/studio_internal.h"
#include "render/shader_catalog.h"
#include "runtime/material_field.h"
#include "support/array.h"
#include "support/file_io.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TILE_SLOTS 256
#define LARGE_SLOTS 4
#define SLOT_COUNT (TILE_SLOTS + LARGE_SLOTS)
/* Rendering stops for the frame once it has taken this long; one thumbnail
   always takes a step, however long it takes. */
#define BUDGET_SECONDS 0.004
#define CHECK_SECONDS 1.0
/* Unsaved edits show once they have rested this long. */
#define EDIT_REST_SECONDS 0.6
/* Changes with what any kind's picture looks like, so the disk's pictures
   from older renderers go unused. */
#define RENDERER_VERSION 4
/* What the disk cache holds at most, for every project together. */
#ifdef __EMSCRIPTEN__
#define DISK_CAP ((size_t)16 << 20)
#else
#define DISK_CAP ((size_t)64 << 20)
#endif
#define DISK_FOLDER "thumbnails"
#define SEED 1469598103934665603ull
/* How far a pixel's channel may stray from the first's in a flat picture. */
#define FLAT_TOLERANCE 2

static const float SIZE_DP[STUDIO_THUMB_SIZE_COUNT] = { 128, 256 };
static const int SIZE_MAX_PIXELS[STUDIO_THUMB_SIZE_COUNT] = { 256, 512 };

typedef enum
{
    THUMB_FREE,
    THUMB_QUEUED,
    THUMB_READY,
    THUMB_FAILED,
} thumb_state;

typedef struct
{
    thumb_state state;
    reference_asset_kind kind;
    studio_thumb_size size;
    char *name, *path;
    uint64_t stamp; /* of the content its next or last picture shows */
    bool unsaved; /* the stamp holds unsaved edits, whose pictures stay off disk */
    uint64_t asked; /* the emission that last asked for it */
    uint64_t order; /* when, among that emission's requests */
    int side;       /* the canvas's pixels */
    CF_Canvas canvas;
    bool shown;     /* the canvas holds a picture, maybe of an older stamp */
    bool loading;   /* more of the picture is coming */
    double ms;      /* spent on the picture in progress */
    uint64_t drawn; /* the frame its picture to read back was drawn in, or 0;
                       that picture shows once it is seen to show something */
    CF_Readback readback; /* that picture on its way back from the GPU */
} thumb;

/* A file's content hash, kept while its modification time and size stay. */
typedef struct
{
    char *path;
    uint64_t stat, hash;
} file_content;

/* An asset's content hash, as the frame `frame` - 1 found it. */
typedef struct
{
    uint64_t hash, frame;
    bool unsaved;
} asset_content;

struct studio_thumbs
{
    thumb slots[SLOT_COUNT];
    char sources[SLOT_COUNT][24];
    char names[SLOT_COUNT][24];
    uint64_t requests;
    uint64_t frame; /* frames rendered */
    /* Direct dependencies and contents by asset position, as of an index
       revision. */
    const reference_index *index;
    uint64_t index_revision;
    int *dependency_start, *dependencies;
    asset_content *contents;
    file_content *files; /* sorted by path */
    uint64_t edits;      /* the generation of edits last seen */
    double edited;       /* when it changed */
    uint64_t validated;  /* the index revision stamps were last checked at */
    uint64_t validated_edits; /* and the generation of edits */
    double checked;           /* and when */
    int busy; /* the slot + 1 of a job that continues next frame, or 0 */
    studio_thumb_renderers *renderers;
    studio_thumb_disk *disk; /* NULL when pictures do not persist */
    studio_thumb_stats stats;
};

/* Where pictures persist: $CHIBI_THUMBNAIL_CACHE when set, empty for
   nowhere; else beside Studio's preferences, or in the browser's IndexedDB
   (tools/web_cache.js). Owned. */
static char *disk_directory()
{
    const char *chosen = getenv("CHIBI_THUMBNAIL_CACHE");
    if (chosen) {
        return chosen[0] ? xstrdup(chosen) : NULL;
    }
#ifdef __EMSCRIPTEN__
    return xstrdup("/" DISK_FOLDER);
#else
    const char *user = cf_fs_get_user_directory("chibi", "editor");
    if (!user) {
        return NULL;
    }
    size_t size = strlen(user) + sizeof DISK_FOLDER;
    char *path = xmalloc(size);
    snprintf(path, size, "%s" DISK_FOLDER, user);
    return path;
#endif
}

static studio_thumbs *thumbs_new()
{
    studio_thumbs *t = xcalloc(1, sizeof *t);
    for (int i = 0; i < SLOT_COUNT; i++) {
        snprintf(t->names[i], sizeof t->names[i], "thumb:%d", i);
        snprintf(t->sources[i], sizeof t->sources[i], "kit:thumb:%d", i);
    }
    char *directory = disk_directory();
    t->disk = directory ? studio_thumb_disk_open(directory, DISK_CAP) : NULL;
    free(directory);
    return t;
}

/* ---------- Stamps ---------- */

static uint64_t mix(uint64_t hash, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        hash = (hash ^ (value >> (8 * i) & 0xff)) * 1099511628211ull;
    }
    return hash;
}

static uint64_t hash_text(const char *text)
{
    uint64_t hash = SEED;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        hash = (hash ^ *c) * 1099511628211ull;
    }
    return hash;
}

/* What changes when the file does: its modification time and size; 0 for
   no file. */
static uint64_t file_stat(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        return 0;
    }
#if defined(_WIN32)
    uint64_t nanoseconds = 0; /* the Windows runtime stats whole seconds */
#elif defined(__APPLE__)
    uint64_t nanoseconds = (uint64_t)info.st_mtimespec.tv_nsec;
#else
    uint64_t nanoseconds = (uint64_t)info.st_mtim.tv_nsec;
#endif
    return mix(
        mix((uint64_t)info.st_mtime * 1000000000ull + nanoseconds, 0),
        (uint64_t)info.st_size
    );
}

static int compare_files(const void *a, const void *b)
{
    return strcmp(((const file_content *)a)->path, ((const file_content *)b)->path);
}

/* The hash of what the file holds, read again only when it changes; its
   text overlay stands in for it. 0 for no file. */
static uint64_t file_hash(studio_thumbs *t, const char *path)
{
    const char *overlay = text_overlay_text(path);
    if (overlay) {
        return hash_text(overlay);
    }
    uint64_t stat = file_stat(path);
    file_content probe = { .path = (char *)path };
    file_content *known = acount(t->files)
                            ? bsearch(&probe, t->files, (size_t)acount(t->files), sizeof probe, compare_files)
                            : NULL;
    if (known && known->stat == stat) {
        return known->hash;
    }
    char *text = stat ? file_read_text(path) : NULL;
    uint64_t hash = text ? hash_text(text) : 0;
    free(text);
    if (known) {
        known->stat = stat;
        known->hash = hash;
        return hash;
    }
    apush(t->files, ((file_content){ xstrdup(path), stat, hash }));
    qsort(t->files, (size_t)acount(t->files), sizeof *t->files, compare_files);
    return hash;
}

static void index_free(studio_thumbs *t)
{
    afree(t->dependency_start);
    afree(t->dependencies);
    afree(t->contents);
}

/* Each asset's direct dependencies, from the references it holds, and room
   for its content. */
static void index_dependencies(studio_thumbs *t, const reference_index *index, uint64_t revision)
{
    if (t->index == index && t->index_revision == revision && t->dependency_start) {
        return;
    }
    index_free(t);
    t->index = index;
    t->index_revision = revision;
    int assets = reference_index_asset_count(index);
    asetlen(t->dependency_start, assets + 1);
    memset(t->dependency_start, 0, (size_t)(assets + 1) * sizeof *t->dependency_start);
    asetlen(t->contents, assets);
    memset(t->contents, 0, (size_t)assets * sizeof *t->contents);
    for (int i = 0; i < reference_index_count(index); i++) {
        const reference *r = reference_index_at(index, i);
        if (reference_dependency(r) >= 0 && r->source >= 0) {
            t->dependency_start[r->source + 1]++;
        }
    }
    for (int i = 0; i < assets; i++) {
        t->dependency_start[i + 1] += t->dependency_start[i];
    }
    asetlen(t->dependencies, t->dependency_start[assets]);
    int *fill = xcalloc((size_t)assets + 1, sizeof *fill);
    for (int i = 0; i < reference_index_count(index); i++) {
        const reference *r = reference_index_at(index, i);
        int used = reference_dependency(r);
        if (used >= 0 && r->source >= 0) {
            t->dependencies[t->dependency_start[r->source] + fill[r->source]++] = used;
        }
    }
    free(fill);
}

/* The asset's content: its unsaved edits' text, else its file's; found
   once a frame. A ui asset's styles beside it count as its own. */
static uint64_t content_of(studio *s, studio_thumbs *t, int asset, bool *unsaved)
{
    asset_content *c = &t->contents[asset];
    uint64_t frame = s->ui_schedule.frames + 1;
    if (c->frame != frame) {
        const reference_asset *a = reference_index_asset_at(t->index, asset);
        char *text = a->path ? studio_thumb_unsaved(s, a) : NULL;
        c->unsaved = text != NULL;
        c->hash = text ? hash_text(text) : a->path ? file_hash(t, a->path) : 0;
        free(text);
        char *styles = a->kind == REFERENCE_ASSET_UI && a->path
                         ? ui_buffer_part_file(a->path, UI_PART_RCSS)
                         : NULL;
        if (styles && strcmp(styles, a->path)) {
            c->hash = mix(c->hash, file_hash(t, styles));
        }
        free(styles);
        c->frame = frame;
    }
    *unsaved |= c->unsaved;
    return c->hash;
}

/* What the asset's picture shows: its content and that of the assets it
   uses, directly or through one asset, with what its kind also draws on. */
static uint64_t stamp_of(studio *s, studio_thumbs *t, int asset, bool *unsaved)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    index_dependencies(t, index, s->scene->asset_index_revision);
    const reference_asset *a = reference_index_asset_at(index, asset);
    *unsaved = false;
    uint64_t stamp = mix(mix(SEED, a->kind), content_of(s, t, asset, unsaved));
    for (int i = t->dependency_start[asset]; i < t->dependency_start[asset + 1]; i++) {
        int used = t->dependencies[i];
        stamp = mix(stamp, content_of(s, t, used, unsaved));
        for (int j = t->dependency_start[used]; j < t->dependency_start[used + 1]; j++) {
            stamp = mix(stamp, content_of(s, t, t->dependencies[j], unsaved));
        }
    }
    const char *stage = studio_thumb_stage(s);
    int staged = stage
                   ? reference_index_find_name(index, REFERENCE_ASSET_PROTOTYPE, stage)
                   : -1;
    switch (a->kind) {
        case REFERENCE_ASSET_MATERIAL:
            stamp = mix(stamp, s->scene->perspective);
            /* fall through: a material's swatch stands on the stage */
        case REFERENCE_ASSET_PROTOTYPE:
            stamp = staged >= 0 ? mix(stamp, content_of(s, t, staged, unsaved)) : stamp;
            break;
        case REFERENCE_ASSET_UI: stamp = mix(stamp, s->light); break;
        default: break;
    }
    /* Edits reach the shader catalog and the fields as they compile, so an
       unsaved picture waits for them too. */
    bool shader = a->kind == REFERENCE_ASSET_MATERIAL || a->kind == REFERENCE_ASSET_SHAPE;
    if (*unsaved && shader) {
        stamp = mix(stamp, render_shader_revision());
    }
    if (*unsaved && a->kind == REFERENCE_ASSET_FIELD) {
        material_fields *fields = game_materials_fields(s->scene->live->runtime->materials);
        stamp = mix(stamp, material_field_revision(fields, a->name));
    }
    return stamp;
}

/* Changes with every authored edit, its Undo and its Redo: the asset index
   is invalidated by each, the open scene's history steps, and ui documents
   publish their text as overlays. */
static uint64_t edit_generation(const studio *s)
{
    const app_scene_authoring *scene = s->scene;
    uint64_t generation = mix(SEED, scene->asset_index_generation);
    generation = mix(generation, text_overlay_generation());
    generation = mix(generation, scene_history_step_id(scene->history, false));
    return mix(generation, scene_history_step_id(scene->history, true));
}

/* A drag or a scrub holds a button down. */
static bool gesture()
{
    return cf_mouse_down(CF_MOUSE_BUTTON_LEFT) ||
           cf_mouse_down(CF_MOUSE_BUTTON_RIGHT) ||
           cf_mouse_down(CF_MOUSE_BUTTON_MIDDLE);
}

/* The disk's name for the slot's picture at `side` pixels. */
static uint64_t disk_key(const thumb *e, int side)
{
    uint64_t key = mix(mix(SEED, RENDERER_VERSION), e->stamp);
    return mix(mix(mix(key, (uint64_t)side), e->size), hash_text(e->name));
}

/* ---------- Slots ---------- */

static void drop_readback(thumb *e)
{
    if (e->readback.id) {
        cf_destroy_readback(e->readback);
    }
    e->readback = (CF_Readback){ 0 };
    e->drawn = 0;
}

static void release(studio *s, studio_thumbs *t, int slot)
{
    thumb *e = &t->slots[slot];
    if (t->busy == slot + 1) {
        studio_thumb_cancel(t->renderers);
        t->busy = 0;
    }
    drop_readback(e);
    if (e->canvas.id) {
        rml_backend_set_image(s->backend, t->names[slot], 0);
        cf_destroy_canvas(e->canvas);
        t->stats.bound--;
    }
    free(e->name);
    free(e->path);
    *e = (thumb){ 0 };
}

/* Queues the slot to show `stamp`, dropping work toward the one before. */
static void restamp(studio_thumbs *t, int slot, uint64_t stamp, bool unsaved)
{
    thumb *e = &t->slots[slot];
    if (t->busy == slot + 1) {
        studio_thumb_cancel(t->renderers);
        t->busy = 0;
    }
    drop_readback(e);
    e->stamp = stamp;
    e->unsaved = unsaved;
    e->state = THUMB_QUEUED;
    e->loading = false;
    e->ms = 0;
}

static int find(const studio_thumbs *t, const reference_asset *a, studio_thumb_size size)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        const thumb *e = &t->slots[i];
        if (e->state != THUMB_FREE && e->kind == a->kind && e->size == size &&
            !strcmp(e->name, a->name)) {
            return i;
        }
    }
    return -1;
}

/* A free slot of the size, or the one asked for least recently that this
   emission has not asked for; -1 when this emission asks for them all. */
static int claim(studio *s, studio_thumbs *t, studio_thumb_size size)
{
    int first = size == STUDIO_THUMB_LARGE ? TILE_SLOTS : 0;
    int end = size == STUDIO_THUMB_LARGE ? SLOT_COUNT : TILE_SLOTS;
    int oldest = -1;
    for (int i = first; i < end; i++) {
        const thumb *e = &t->slots[i];
        if (e->state == THUMB_FREE) {
            return i;
        }
        if (e->asked != s->ui_schedule.frames &&
            (oldest < 0 || e->asked < t->slots[oldest].asked)) {
            oldest = i;
        }
    }
    if (oldest >= 0) {
        release(s, t, oldest);
        t->stats.evicted++;
    }
    return oldest;
}

static int side_of(const studio *s, studio_thumb_size size)
{
    int side = (int)(SIZE_DP[size] * studio_pixel_ratio(s) + 0.5f);
    return side < SIZE_MAX_PIXELS[size] ? side : SIZE_MAX_PIXELS[size];
}

/* The canvas side the slot's kind renders at. */
static int canvas_side(const studio *s, const thumb *e)
{
    return studio_thumb_side(e->kind, side_of(s, e->size));
}

const char *studio_thumb(studio *s, int asset, studio_thumb_size size, bool *loading)
{
    *loading = false;
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    const reference_asset *a = reference_index_asset_at(index, asset);
    if (!a || !a->name || !a->path || a->unresolved) {
        return NULL;
    }
    if (!s->thumbs) {
        s->thumbs = thumbs_new();
    }
    studio_thumbs *t = s->thumbs;
    int slot = find(t, a, size);
    if (slot < 0 && (slot = claim(s, t, size)) >= 0) {
        thumb *e = &t->slots[slot];
        *e = (thumb){ .state = THUMB_QUEUED,
                      .kind = a->kind,
                      .size = size,
                      .name = xstrdup(a->name),
                      .path = xstrdup(a->path) };
        e->stamp = stamp_of(s, t, asset, &e->unsaved);
    }
    if (slot < 0) {
        return NULL;
    }
    thumb *e = &t->slots[slot];
    if (strcmp(e->path, a->path)) {
        free(e->path);
        e->path = xstrdup(a->path);
        bool unsaved;
        uint64_t stamp = stamp_of(s, t, asset, &unsaved);
        restamp(t, slot, stamp, unsaved);
    }
    e->asked = s->ui_schedule.frames;
    e->order = t->requests++;
    *loading = e->shown && e->loading;
    return e->shown ? t->sources[slot] : NULL;
}

const char *studio_thumb_cached(studio *s, int asset)
{
    const reference_asset
        *a = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), asset);
    int slot = s->thumbs && a && a->name ? find(s->thumbs, a, STUDIO_THUMB_TILE) : -1;
    if (slot < 0 || !s->thumbs->slots[slot].shown) {
        return NULL;
    }
    s->thumbs->slots[slot].asked = s->ui_schedule.frames;
    return s->thumbs->sources[slot];
}

/* ---------- Rendering ---------- */

/* Whether to check the stamps now: once edits have rested with no button
   held, a second after the last check or when the index or the edits
   changed since. */
static bool due(studio *s, studio_thumbs *t)
{
    uint64_t edits = edit_generation(s);
    if (edits != t->edits) {
        t->edits = edits;
        t->edited = s->time;
    }
    if (s->time - t->edited < EDIT_REST_SECONDS || gesture()) {
        return false;
    }
    return t->validated != s->scene->asset_index_revision ||
           t->validated_edits != edits || s->time - t->checked >= CHECK_SECONDS;
}

/* Thumbnails the latest emission asked for whose content changed. */
static void validate(studio *s, studio_thumbs *t, uint64_t latest)
{
    t->validated = s->scene->asset_index_revision;
    t->validated_edits = t->edits;
    t->checked = s->time;
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    for (int i = 0; i < SLOT_COUNT; i++) {
        thumb *e = &t->slots[i];
        int asset = e->state != THUMB_FREE && e->asked == latest
                      ? reference_index_find_name(index, e->kind, e->name)
                      : -1;
        bool unsaved;
        uint64_t stamp = asset >= 0 ? stamp_of(s, t, asset, &unsaved) : e->stamp;
        if (stamp != e->stamp) {
            restamp(t, i, stamp, unsaved);
        }
    }
}

static bool on_disk(const studio *s, const studio_thumbs *t, const thumb *e)
{
    return t->disk && !e->unsaved &&
           studio_thumb_disk_has(t->disk, disk_key(e, canvas_side(s, e)));
}

/* Pictures on disk first, then large previews, then the cheapest kinds,
   then in the order asked. While a job is busy, kinds that need its world
   wait. */
static int next_job(const studio *s, const studio_thumbs *t, uint64_t latest)
{
    int best = -1, best_rank = 0;
    for (int i = 0; i < SLOT_COUNT; i++) {
        const thumb *e = &t->slots[i];
        if (e->state != THUMB_QUEUED || e->asked != latest || t->busy == i + 1) {
            continue;
        }
        bool disk = on_disk(s, t, e);
        if (t->busy && !disk && studio_thumb_needs_world(e->kind)) {
            continue;
        }
        int rank = disk ? -1 : studio_thumb_cost(e->kind);
        const thumb *b = best >= 0 ? &t->slots[best] : NULL;
        bool better = !b || (rank != best_rank    ? rank < best_rank
                             : e->size != b->size ? e->size > b->size
                                                  : e->order < b->order);
        if (better) {
            best = i;
            best_rank = rank;
        }
    }
    return best;
}

static double seconds()
{
    return (double)cf_get_ticks() / (double)cf_get_tick_frequency();
}

/* The slot's canvas, made again when its side changed. */
static void canvas_for(studio *s, studio_thumbs *t, int slot)
{
    thumb *e = &t->slots[slot];
    int side = canvas_side(s, e);
    if (e->canvas.id && e->side != side) {
        drop_readback(e);
        rml_backend_set_image(s->backend, t->names[slot], 0);
        cf_destroy_canvas(e->canvas);
        e->canvas = (CF_Canvas){ 0 };
        e->shown = false;
        t->stats.bound--;
    }
    if (!e->canvas.id) {
        e->canvas = cf_make_canvas(cf_canvas_defaults(side, side));
        e->side = side;
        t->stats.bound++;
    }
}

static void show(studio *s, studio_thumbs *t, int slot)
{
    thumb *e = &t->slots[slot];
    e->shown = true;
    rml_backend_set_image(
        s->backend,
        t->names[slot],
        cf_canvas_get_target(e->canvas).id
    );
    s->ui_schedule.wake = true;
}

/* The job ends with a picture or without one. */
static void finish(studio *s, studio_thumbs *t, int slot, bool drawn)
{
    thumb *e = &t->slots[slot];
    t->stats.count[e->kind]++;
    t->stats.total_ms[e->kind] += e->ms;
    t->stats.blank += !drawn;
    e->ms = 0;
    e->loading = false;
    e->state = drawn ? THUMB_READY : THUMB_FAILED;
    if (t->busy == slot + 1) {
        t->busy = 0;
    }
    if (drawn) {
        /* A picture shows once it is read back, and a world's once seen to
           hold more than one color (take_picture). */
        e->drawn = t->frame;
    } else if (e->shown) {
        /* An older picture no longer shows the asset, as an emptied graph. */
        rml_backend_set_image(s->backend, t->names[slot], 0);
        e->shown = false;
    }
    s->ui_schedule.wake = true;
}

/* One step of the slot's job: its picture from disk, or a step of its
   rendering. */
static void step(studio *s, studio_thumbs *t, int slot)
{
    thumb *e = &t->slots[slot];
    canvas_for(s, t, slot);
    if (t->busy != slot + 1 && on_disk(s, t, e) &&
        studio_thumb_disk_load(t->disk, disk_key(e, e->side), e->canvas, e->side)) {
        t->stats.loaded++;
        e->state = THUMB_READY;
        e->loading = false;
        show(s, t, slot);
        return;
    }
    studio_thumb_job job = { e->kind, e->size, e->name,
                             e->path, e->side, s->light };
    double start = seconds();
    studio_thumb_step result = studio_thumb_render(s, &t->renderers, &job, e->canvas);
    double ms = (seconds() - start) * 1000;
    e->ms += ms;
    if (ms > t->stats.max_ms[e->kind]) {
        t->stats.max_ms[e->kind] = ms;
    }
    switch (result) {
        case STUDIO_THUMB_BUSY: t->busy = slot + 1; break;
        case STUDIO_THUMB_PARTIAL:
            t->busy = slot + 1;
            t->stats.partial++;
            e->loading = true;
            show(s, t, slot);
            break;
        case STUDIO_THUMB_DRAWN: finish(s, t, slot, true); break;
        case STUDIO_THUMB_BLANK: finish(s, t, slot, false); break;
    }
}

/* Whether every pixel has one color, within a step of rounding. */
static bool flat(const CF_Pixel *pixels, int count)
{
    const CF_Pixel first = pixels[0];
    for (int i = 1; i < count; i++) {
        const CF_Pixel p = pixels[i];
        if (abs(p.r - first.r) > FLAT_TOLERANCE || abs(p.g - first.g) > FLAT_TOLERANCE ||
            abs(p.b - first.b) > FLAT_TOLERANCE || abs(p.a - first.a) > FLAT_TOLERANCE) {
            return false;
        }
    }
    return true;
}

/* A picture read back, which shows and goes to disk when it shows saved
   content. A world's picture that came out flat, as a scene with no
   lighting yet renders, says less than its kind's icon and fails instead:
   its renderer cannot tell whether what it drew shows. Other kinds say
   when they draw nothing, so their flat picture, as a constant field's,
   shows. False when it failed. */
static bool take_picture(studio *s, studio_thumbs *t, int slot, const CF_Pixel *pixels)
{
    thumb *e = &t->slots[slot];
    if (studio_thumb_needs_world(e->kind) && flat(pixels, e->side * e->side)) {
        e->state = THUMB_FAILED;
        t->stats.blank++;
        if (e->shown) {
            rml_backend_set_image(s->backend, t->names[slot], 0);
            e->shown = false;
        }
        s->ui_schedule.wake = true;
        return false;
    }
    show(s, t, slot);
    return true;
}

/* A picture no read can check shows as drawn. */
static void show_unread(studio *s, studio_thumbs *t, int slot)
{
    show(s, t, slot);
    drop_readback(&t->slots[slot]);
}

/* Pictures are read back a frame after they were drawn, once the GPU has
   them, and taken when the read lands, one a frame: shown once seen to show
   something, and written to disk when they show saved content. */
static void read_pictures(studio *s, studio_thumbs *t)
{
    bool took = false;
    for (int i = 0; i < SLOT_COUNT; i++) {
        thumb *e = &t->slots[i];
        if (!e->drawn) {
            continue;
        }
        if (!e->readback.id) {
            e->readback = t->frame > e->drawn ? cf_canvas_readback(e->canvas)
                                              : e->readback;
            /* A target the backend cannot read gives no readback. */
            if (t->frame > e->drawn && !e->readback.id) {
                show_unread(s, t, i);
            }
            continue;
        }
        if (took || !cf_readback_ready(e->readback)) {
            continue;
        }
        int bytes = e->side * e->side * (int)sizeof(CF_Pixel);
        CF_Pixel *pixels = xmalloc((size_t)bytes);
        bool read = cf_readback_data(e->readback, pixels, bytes) == bytes;
        if (!read) {
            show_unread(s, t, i);
        }
        if (read && take_picture(s, t, i, pixels) && t->disk && !e->unsaved) {
            studio_thumb_disk_store(t->disk, disk_key(e, e->side), pixels, e->side);
            t->stats.stored++;
        }
        free(pixels);
        drop_readback(e);
        took = true;
    }
}

void studio_thumbs_frame(studio *s)
{
    studio_thumbs *t = s->thumbs;
    if (!t || !s->ui_schedule.frames) {
        return;
    }
    t->frame++;
    uint64_t latest = s->ui_schedule.frames - 1;
    double start = seconds();
    if (due(s, t)) {
        validate(s, t, latest);
    }
    /* A job no one looks at any more stops, freeing the world for those
       shown; it starts again when asked. */
    if (t->busy && t->slots[t->busy - 1].asked != latest) {
        studio_thumb_cancel(t->renderers);
        t->slots[t->busy - 1].loading = false;
        t->busy = 0;
    }
    read_pictures(s, t);
    bool stepped = t->busy != 0;
    if (t->busy) {
        step(s, t, t->busy - 1);
    }
    while (!stepped || seconds() - start < BUDGET_SECONDS) {
        int slot = next_job(s, t, latest);
        if (slot < 0) {
            break;
        }
        step(s, t, slot);
        stepped = true;
    }
    t->stats.frame_ms = (seconds() - start) * 1000;
}

bool studio_thumbs_pending(const studio *s)
{
    const studio_thumbs *t = s->thumbs;
    if (!t || !s->ui_schedule.frames) {
        return false;
    }
    uint64_t latest = s->ui_schedule.frames - 1;
    bool asked = false;
    for (int i = 0; i < SLOT_COUNT; i++) {
        const thumb *e = &t->slots[i];
        if (e->drawn) {
            return true;
        }
        asked |= e->state != THUMB_FREE && e->asked == latest;
    }
    /* Edits since the last check show once they rest. */
    bool resting = asked && t->validated_edits != edit_generation(s);
    return t->busy || resting || next_job(s, t, latest) >= 0;
}

studio_thumb_stats studio_thumbs_stats(const studio *s)
{
    return s->thumbs ? s->thumbs->stats : (studio_thumb_stats){ 0 };
}

void studio_thumbs_free(studio *s)
{
    studio_thumbs *t = s->thumbs;
    if (!t) {
        return;
    }
    for (int i = 0; i < SLOT_COUNT; i++) {
        release(s, t, i);
    }
    studio_thumb_renderers_free(t->renderers);
    studio_thumb_disk_close(t->disk);
    index_free(t);
    for (int i = 0; i < acount(t->files); i++) {
        free(t->files[i].path);
    }
    afree(t->files);
    free(t);
    s->thumbs = NULL;
}
