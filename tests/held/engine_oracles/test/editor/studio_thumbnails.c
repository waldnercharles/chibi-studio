#include "authoring/engine_preview.h"
/* Asset thumbnails: mini-maps fit their square; in Studio over Neon Swarm
 * every kind of asset renders, with its cost reported; large previews past
 * their cap release their textures; the grid's keys move the selection; a
 * grid of a thousand assets stays windowed; an unsaved edit renders once it
 * rests, and its Undo too; Sandbox streams its terrain before its picture,
 * showing what it has at the cap; a changed file renders again; and the disk
 * cache keeps pictures between sessions until their content changes. */
#include "editor/workspace/viewport.h"
#include "editor/studio/studio_thumbnails.h"

#include "assets/new_asset.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/input.h"
#include "graph/document/serialize.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/file_io.h"
#include "support/text_file.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define FRAME_SECONDS (1.0 / 60.0)
#define ARENA "projects/arena2d"
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

/* ---------- Mini-maps ---------- */

static bool box_inside(const studio_minimap_box *b, float low, float high)
{
    const float slack = 0.01f;
    return b->x >= low - slack && b->y >= low - slack && b->x + b->w <= high + slack &&
           b->y + b->h <= high + slack && b->w > 0 && b->h > 0;
}

void test_studio_minimap()
{
    char *text = text_file_read(ARENA "/content/behaviors/Movement.chibi");
    behavior_asset *asset = text ? behavior_asset_load(text) : NULL;
    free(text);
    assert(asset);
    const float side = 128, margin = side * 0.1f;
    studio_minimap map;
    studio_minimap_build(asset->graph, side, &map);
    bool inside = acount(map.boxes) > 0;
    float left = side, right = 0, top = side, bottom = 0;
    for (int i = 0; i < acount(map.boxes); i++) {
        const studio_minimap_box *b = &map.boxes[i];
        inside &= box_inside(b, margin, side - margin);
        left = fminf(left, b->x);
        right = fmaxf(right, b->x + b->w);
        top = fminf(top, b->y);
        bottom = fmaxf(bottom, b->y + b->h);
    }
    for (int i = 0; i < acount(map.wires); i++) {
        const studio_minimap_wire *w = &map.wires[i];
        inside &= w->x0 >= margin - 0.01f && w->x1 <= side - margin + 0.01f &&
                  w->y0 >= margin - 0.01f && w->y1 <= side - margin + 0.01f;
    }
    /* The longer side spans the inner square; both are centered. */
    float span = fmaxf(right - left, bottom - top);
    bool fitted = fabsf(span - (side - 2 * margin)) < 0.5f &&
                  fabsf((left + right) / 2 - side / 2) < 0.5f &&
                  fabsf((top + bottom) / 2 - side / 2) < 0.5f;
    test_check(inside && fitted && acount(map.wires) > 0, "a graph's mini-map fits its boxes and wires centered in the thumbnail's square, inside its margin");
    studio_minimap_free(&map);
    graph empty;
    graph_init(&empty);
    studio_minimap_build(&empty, side, &map);
    test_check(!acount(map.boxes) && !acount(map.wires), "an empty graph's mini-map is empty, so its tile keeps the kind's icon");
    graph_free(&empty);
    behavior_asset_free(asset);
}

/* ---------- Studio ---------- */

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    engine_viewport *viewport;
    app_scene_authoring *scene;
    studio *studio;
    double time;
} thumbs_fixture;

static thumbs_fixture fixture_open(const char *root, const char *scene)
{
    thumbs_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Editing", behavior_asset_new("Editing"));
    assert(definition && game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Editing"));
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    f.viewport = engine_viewport_new(640, 360);
    f.scene = app_scene_authoring_new(
        f.world,
        f.live,
        definition,
        editor_behavior_asset(f.graph),
        f.graph,
        NULL,
        NULL,
        f.runtime
    );
    char content[PATH_MAX], path[PATH_MAX + 64];
    snprintf(content, sizeof content, "%s/content", root);
    snprintf(path, sizeof path, "%s/%s", content, scene);
    assert(app_scene_authoring_set_content_root(f.scene, content) == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(app_scene_authoring_load(f.scene, path) == 0);
    f.studio = studio_new(f.scene, f.viewport, root);
    studio *s = f.studio;
    studio_prefs defaults = studio_prefs_default();
    s->prefs.ui_scale = 1;
    s->prefs.assets_tile = STUDIO_ASSETS_TILE;
    s->prefs.workspace = defaults.workspace;
    memcpy(s->prefs.layouts, defaults.layouts, sizeof defaults.layouts);
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
    s->applied = s->prefs;
    s->maximized = STUDIO_AREA_TOP;
    return f;
}

static void fixture_free(thumbs_fixture *f)
{
    studio_free(f->studio);
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
    engine_viewport_free(f->viewport);
}

static double now()
{
    return (double)cf_get_ticks() / (double)cf_get_tick_frequency();
}

/* One frame; returns its milliseconds. */
static double frame(thumbs_fixture *f, double seconds)
{
    double start = now();
    f->time += seconds;
    cf_app_update(NULL);
    app_input_frame();
    editor_tick(f->graph, f->time);
    assert(app_scene_authoring_frame(f->scene, f->time) == FLOW_OK);
    studio_frame(f->studio, seconds);
    cf_app_draw_onto_screen(true);
    f->studio->prefs_changed_at = 0;
    return (now() - start) * 1000;
}

/* Pictures shown so far: rendered, or read from the disk cache. */
static int rendered(const studio_thumb_stats *stats)
{
    int total = stats->loaded;
    for (int k = 0; k < REFERENCE_ASSET_KIND_COUNT; k++) {
        total += stats->count[k];
    }
    return total;
}

/* Frames until no thumbnail shows or waits for half a second, at most
   `limit`; the slowest frame that showed one in *worst. */
static int settle_within(thumbs_fixture *f, double *worst, int limit)
{
    int frames = 0, quiet = 0;
    for (; frames < limit && quiet < 30; frames++) {
        studio_thumb_stats stats = studio_thumbs_stats(f->studio);
        int before = rendered(&stats);
        double ms = frame(f, FRAME_SECONDS);
        stats = studio_thumbs_stats(f->studio);
        bool shown = rendered(&stats) != before;
        quiet = shown || studio_thumbs_pending(f->studio) ? 0 : quiet + 1;
        if (shown && worst && ms > *worst) {
            *worst = ms;
        }
    }
    return frames;
}

static int settle(thumbs_fixture *f, double *worst)
{
    return settle_within(f, worst, 1200);
}

static bool has_kind(const studio *s, int kind)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    for (int i = 0; i < reference_index_asset_count(index); i++) {
        const reference_asset *a = reference_index_asset_at(index, i);
        if ((int)a->kind == kind && a->path && !a->unresolved) {
            return true;
        }
    }
    return false;
}

/* Each kind through the kind filter, with its render times. */
static void every_kind_renders(thumbs_fixture *f)
{
    studio *s = f->studio;
    bool all = true;
    double worst = 0;
    double idle[3];
    for (int i = 0; i < 3; i++) {
        idle[i] = frame(f, FRAME_SECONDS);
    }
    for (int kind = 0; kind < REFERENCE_ASSET_KIND_COUNT; kind++) {
        s->assets_kind = kind;
        s->ui_schedule.wake = true;
        settle(f, &worst);
        studio_thumb_stats stats = studio_thumbs_stats(s);
        bool present = has_kind(s, kind);
        all &= !present || stats.count[kind] > 0;
        if (present) {
            printf(
                "  thumbnails %-10s %3d rendered, %7.2f ms mean, %7.2f ms "
                "max\n",
                studio_asset_kind_label(kind),
                stats.count[kind],
                stats.count[kind] ? stats.total_ms[kind] / stats.count[kind] : 0,
                stats.max_ms[kind]
            );
        }
    }
    studio_thumb_stats stats = studio_thumbs_stats(s);
    printf(
        "  thumbnails: %d without a picture, %d bound, slowest streaming frame "
        "%.1f ms (idle %.1f ms)\n",
        stats.blank,
        stats.bound,
        worst,
        fmin(idle[0], fmin(idle[1], idle[2]))
    );
    test_check(all && stats.bound > 0, "every kind of asset in Neon Swarm renders a thumbnail through the Assets kind filter");
}

/* Large previews past their cap of four give their textures back. */
static void large_previews_evict(thumbs_fixture *f)
{
    studio *s = f->studio;
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int asked = 0;
    studio_thumb_stats before = studio_thumbs_stats(s);
    for (int i = 0; i < reference_index_asset_count(index) && asked < 7; i++) {
        const reference_asset *a = reference_index_asset_at(index, i);
        if (a->kind != REFERENCE_ASSET_BEHAVIOR) {
            continue;
        }
        bool loading;
        studio_thumb(s, i, STUDIO_THUMB_LARGE, &loading);
        frame(f, FRAME_SECONDS);
        asked++;
    }
    studio_thumb_stats after = studio_thumbs_stats(s);
    bool evicted = after.evicted - before.evicted == asked - 4;
    bool released = after.bound - before.bound == 4;
    if (!(evicted && released)) {
        printf(
            "  evicted %d, bound %d -> %d\n",
            after.evicted - before.evicted,
            before.bound,
            after.bound
        );
    }
    test_check(asked == 7 && evicted && released, "large previews past their cap evict the least recent and release its texture");
}

/* The grid's keys: End selects the last asset, Left the one before, Up the
   one a row above, and Home the first. */
static void grid_keys_move_selection(thumbs_fixture *f)
{
    studio *s = f->studio;
    s->assets_kind = -1;
    s->assets_filter[0] = '\0';
    s->ui_schedule.wake = true;
    frame(f, FRAME_SECONDS);
    frame(f, FRAME_SECONDS);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    char last[128];
    assert(studio_assets_key(s, SDLK_END, SDL_KMOD_NONE));
    snprintf(last, sizeof last, "%s", s->assets_selected);
    assert(studio_assets_key(s, SDLK_LEFT, SDL_KMOD_NONE));
    bool before = strcmp(last, s->assets_selected) != 0;
    assert(studio_assets_key(s, SDLK_HOME, SDL_KMOD_NONE));
    int first = reference_index_find_name(index, (reference_asset_kind)s->assets_selected_kind, s->assets_selected);
    bool scene = first >= 0 && reference_index_asset_at(index, first)->kind ==
                                   REFERENCE_ASSET_SCENE;
    test_check(last[0] && before && scene && !strcmp(last, "PauseMenu"), "the grid's End, Left and Home keys move the selection through the assets in their order");
}

/* Tiles the grid shows, past data-for's templates. */
static int grid_tiles(studio *s)
{
    rml_element *tiles[512];
    int count = test_query_all(rml_backend_context(s->backend), "#assets-list [data-ui=Tile]", tiles, 512);
    int shown = 0;
    for (int i = 0; i < count && i < 512; i++) {
        shown += rml_element_is_visible(tiles[i], true);
    }
    return shown;
}

/* A grid of more than a thousand assets makes only the tiles in view. */
static void large_grid_stays_windowed(thumbs_fixture *f)
{
    studio *s = f->studio;
    for (int i = 0; i < 1200; i++) {
        char name[32];
        snprintf(name, sizeof name, "QaGrid%04d", i);
        reference_index_add_asset(f->scene->asset_index, REFERENCE_ASSET_BEHAVIOR, name, NULL, name);
    }
    /* Inserted directly: start the cached model afresh, as a rebuild would. */
    studio_assets_free(s);
    s->assets_kind = REFERENCE_ASSET_BEHAVIOR;
    s->ui_schedule.wake = true;
    frame(f, FRAME_SECONDS);
    double ms = 0;
    for (int i = 0; i < 4; i++) {
        s->ui_schedule.wake = true;
        ms = fmax(ms, frame(f, FRAME_SECONDS));
    }
    int tiles = grid_tiles(s);
    printf("  a grid of %d behaviors makes %d tiles; slowest frame %.1f ms\n", 1200 + 20, tiles, ms);
    test_check(tiles > 0 && tiles < 200, "a grid of more than a thousand assets makes only the tiles in view and overscan");
}

/* Frames until the kind's count passes `count`, at most `limit`; Studio's
   time it took. */
static double until_rendered(thumbs_fixture *f, int kind, int count, int limit)
{
    double start = f->studio->time;
    for (int i = 0; i < limit && studio_thumbs_stats(f->studio).count[kind] <= count;
         i++) {
        frame(f, FRAME_SECONDS);
    }
    return f->studio->time - start;
}

/* An unsaved prototype edit renders once it rests, not before, and never
   reaches the disk; its Undo renders too. Edits every frame, as a scrub
   makes, cost no thumbnail work until they stop. */
static void unsaved_edit_renders_after_rest(thumbs_fixture *f)
{
    studio *s = f->studio;
    app_scene_authoring *scene = f->scene;
    s->assets_kind = REFERENCE_ASSET_PROTOTYPE;
    s->ui_schedule.wake = true;
    settle(f, NULL);
    studio_thumb_stats before = studio_thumbs_stats(s);
    /* Opening it reveals it in the Scene area; Assets comes back. */
    assert(app_scene_authoring_open_prototype(scene, "Crystal") == 0);
    frame(f, FRAME_SECONDS);
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
    s->applied = s->prefs;
    double scrub = 0, work = 0;
    for (int i = 0; i < 30; i++) {
        pin_value color = { .type = PIN_U32, .u = 0xff4fa3ffu - (unsigned)i * 0x100u };
        assert(
            app_scene_authoring_set_prototype_field(scene, "Crystal", "GameAppearance", "color", color) == 0
        );
        scrub = fmax(scrub, frame(f, FRAME_SECONDS));
        work = fmax(work, studio_thumbs_stats(s).frame_ms);
    }
    int early = studio_thumbs_stats(s).count[REFERENCE_ASSET_PROTOTYPE] -
                before.count[REFERENCE_ASSET_PROTOTYPE];
    double latency = until_rendered(f, REFERENCE_ASSET_PROTOTYPE, before.count[REFERENCE_ASSET_PROTOTYPE], 240);
    studio_thumb_stats edited = studio_thumbs_stats(s);
    assert(prototype_undo(scene) == 0);
    double undone = until_rendered(f, REFERENCE_ASSET_PROTOTYPE, edited.count[REFERENCE_ASSET_PROTOTYPE], 240);
    settle(f, NULL);
    studio_thumb_stats after = studio_thumbs_stats(s);
    printf(
        "  unsaved edit: picture %.2f s after the last of 30 edits, Undo's "
        "%.2f s after it; while editing, frames took at most %.1f ms, %.2f ms "
        "of it thumbnails\n",
        latency,
        undone,
        scrub,
        work
    );
    bool rendered = edited.count[REFERENCE_ASSET_PROTOTYPE] >
                        before.count[REFERENCE_ASSET_PROTOTYPE] &&
                    after.count[REFERENCE_ASSET_PROTOTYPE] >
                        edited.count[REFERENCE_ASSET_PROTOTYPE];
    test_check(early == 0 && rendered && latency < 1.0 && undone < 1.0, "an unsaved prototype edit and its Undo render once they rest, not while edits keep coming");
    test_check(after.stored == before.stored, "pictures of unsaved edits stay off the disk cache");
}

/* Asks for one thumbnail each frame, as the grid would, until it is drawn
   in full; Studio's time to its partial picture, or -1, in *partial. */
static double until_drawn(thumbs_fixture *f, int asset, int limit, double step, double *partial)
{
    studio *s = f->studio;
    double start = s->time;
    *partial = -1;
    int before = studio_thumbs_stats(s)
                     .count[reference_index_asset_at(app_scene_authoring_asset_index(s->scene), asset)
                                ->kind];
    for (int i = 0; i < limit; i++) {
        bool loading;
        studio_thumb(s, asset, STUDIO_THUMB_TILE, &loading);
        s->ui_schedule.wake = true;
        frame(f, step);
        studio_thumb_stats stats = studio_thumbs_stats(s);
        if (*partial < 0 && stats.partial) {
            *partial = s->time - start;
        }
        if (stats.count[REFERENCE_ASSET_SCENE] > before) {
            break;
        }
    }
    return s->time - start;
}

#ifndef CHIBI_SKIP_PERFORMANCE_TESTS
/* Sandbox frames its cave and streams the terrain there before drawing. */
static void sandbox_streams_terrain(thumbs_fixture *f)
{
    studio *s = f->studio;
    studio_thumbs_free(s);
    s->assets_kind = REFERENCE_ASSET_SYSTEM;
    int sandbox = reference_index_find_name(
        app_scene_authoring_asset_index(s->scene),
        REFERENCE_ASSET_SCENE,
        "Sandbox"
    );
    double wall = now(), partial;
    double done = until_drawn(f, sandbox, 3000, FRAME_SECONDS, &partial);
    studio_thumb_stats stats = studio_thumbs_stats(s);
    printf(
        "  Sandbox: partial picture at %.2f s, complete at %.2f s (%.2f s "
        "wall), %.0f ms of work, slowest step %.1f ms\n",
        partial,
        done,
        now() - wall,
        stats.total_ms[REFERENCE_ASSET_SCENE],
        stats.max_ms[REFERENCE_ASSET_SCENE]
    );
    test_check(stats.count[REFERENCE_ASSET_SCENE] == 1 && (partial < 0 || partial >= 2.0), "Sandbox's thumbnail streams its terrain first, showing what it has only past the cap");
}

#endif

static void arena_thumbnails()
{
    static char root[PATH_MAX];
    assert(realpath(ARENA, root));
    thumbs_fixture f = fixture_open(root, "scenes/Sandbox.kdl");
    frame(&f, FRAME_SECONDS);
    every_kind_renders(&f);
    large_previews_evict(&f);
    grid_keys_move_selection(&f);
    large_grid_stays_windowed(&f);
#ifndef CHIBI_SKIP_PERFORMANCE_TESTS
    sandbox_streams_terrain(&f);
#endif
    unsaved_edit_renders_after_rest(&f);
    fixture_free(&f);
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static char *disk_text(new_asset_kind kind, const char *name)
{
    char error[128];
    char *json = new_asset_text(kind, name);
    if (kind == NEW_ASSET_BEHAVIOR || kind == NEW_ASSET_LIBRARY ||
        kind == NEW_ASSET_SYSTEM) {
        return json;
    }
    char *disk = asset_text_to_disk(json, error, sizeof error);
    free(json);
    return disk;
}

/* Removes a folder and everything in it. */
static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *item;
    while (dir && (item = readdir(dir))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) {
            continue;
        }
        char child[PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", path, item->d_name);
        struct stat info;
        if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) {
            remove_tree(child);
        } else {
            remove(child);
        }
    }
    if (dir) {
        closedir(dir);
    }
    rmdir(path);
}

static void write_in(const char *root, const char *file, const char *text)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", root, file);
    write_text(path, text);
}

#define DOT_PROTOTYPE                                                          \
    "name \"Dot\"\ncomponents {\n    GAME.GameAppearance color=0xff8800ff "    \
    "height=40.0 "                                                             \
    "shape=\"Circle\" width=40.0\n    (object)GAME.GameTransform\n}\n"
#define MAIN_SCENE                                                             \
    "version 1\nname \"Main\"\n(array)entities {\n    - name=\"Dot\" "         \
    "prototype=\"Dot\"\n}\n"

/* A project of a scene showing one prototype, and a behavior. */
static void make_project(char *root)
{
    assert(mkdtemp(root));
    const char *folders[] = { "content", "content/scenes", "content/behaviors", "content/prototypes" };
    for (int i = 0; i < 4; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", root, folders[i]);
        assert(mkdir(path, 0755) == 0);
    }
    write_in(
        root,
        CHIBI_PROJECT_MANIFEST,
        "version " STRINGIFY(
            CHIBI_PROJECT_VERSION
        ) "\n"
          "name \"Thumbs\"\n"
          "content \"content\"\n"
          "start_scene \"scenes/Main.kdl\"\n"
    );
    write_in(root, "content/scenes/Main.kdl", MAIN_SCENE);
    write_in(root, "content/prototypes/Dot.kdl", DOT_PROTOTYPE);
    char *behavior = disk_text(NEW_ASSET_BEHAVIOR, "Spin");
    write_in(root, "content/behaviors/Spin.chibi", behavior);
    free(behavior);
}

/* A behavior renders again once its file's content changes; a newer
   modification time alone, as a checkout leaves, changes nothing. */
static void imported_graph_renders(const char *root)
{
    char libraries[PATH_MAX], library[PATH_MAX + 32], caller[PATH_MAX];
    snprintf(libraries, sizeof libraries, "%s/content/libraries", root);
    assert(mkdir(libraries, 0755) == 0);
    snprintf(library, sizeof library, "%s/Shared.chibi", libraries);
    snprintf(caller, sizeof caller, "%s/content/behaviors/Imported.chibi", root);
    write_text(library, "library Shared { pure function Seven(Input: float = 7) -> (Result: float) { result Result: float = entry.Input; } }");
    const char *source = "behavior Imported { state float count = 0; value "
                         "chosen = Shared.Seven(Input: 7); on Tick() { count "
                         "+= chosen.Result; } }";
    write_text(caller, source);
    thumbs_fixture f = fixture_open(root, "scenes/Main.kdl");
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    const reference_index *index = app_scene_authoring_asset_index(f.scene);
    int asset = -1;
    for (int i = 0; i < reference_index_asset_count(index); i++) {
        const reference_asset *item = reference_index_asset_at(index, i);
        if (item->kind == REFERENCE_ASSET_BEHAVIOR && !strcmp(item->name, "Imported")) {
            asset = i;
            break;
        }
    }
    assert(asset >= 0);
    bool drawn = false;
    for (int i = 0; i < 120 && !drawn; i++) {
        bool loading;
        drawn = studio_thumb(f.studio, asset, STUDIO_THUMB_TILE, &loading) != NULL;
        frame(&f, FRAME_SECONDS);
    }
    char *saved = file_read_text(caller);
    test_check(drawn && saved && !strcmp(saved, source), "a saved behavior thumbnail resolves project library calls without modifying its source");
    free(saved);
    fixture_free(&f);
    assert(remove(caller) == 0 && remove(library) == 0 && rmdir(libraries) == 0);
}

static void changed_file_renders_again(const char *root)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/content/behaviors/Spin.chibi", root);
    thumbs_fixture f = fixture_open(root, "scenes/Main.kdl");
    studio *s = f.studio;
    s->assets_kind = REFERENCE_ASSET_BEHAVIOR;
    settle(&f, NULL);
    int first = studio_thumbs_stats(s).count[REFERENCE_ASSET_BEHAVIOR];
    struct timeval times[2];
    gettimeofday(&times[0], NULL);
    times[0].tv_sec += 5;
    times[1] = times[0];
    assert(utimes(path, times) == 0);
    frame(&f, 1.5);
    settle(&f, NULL);
    int touched = studio_thumbs_stats(s).count[REFERENCE_ASSET_BEHAVIOR];
    char *text = text_file_read(path);
    size_t size = strlen(text) + 32;
    char *edited = xmalloc(size);
    snprintf(edited, size, "%s// edited\n", text);
    write_text(path, edited);
    free(text);
    free(edited);
    frame(&f, 1.5);
    settle(&f, NULL);
    int changed = studio_thumbs_stats(s).count[REFERENCE_ASSET_BEHAVIOR];
    test_check(first == 1 && touched == 1 && changed == 2, "a thumbnail renders again once its asset's content changes, not for a touched file");
    fixture_free(&f);
}

/* The open scene's thumbnail shows its unsaved edits once they rest, as it
   does an edit of a prototype it uses. */
static void unsaved_scene_renders(const char *root)
{
    thumbs_fixture f = fixture_open(root, "scenes/Main.kdl");
    studio *s = f.studio;
    app_scene_authoring *scene = f.scene;
    s->assets_kind = REFERENCE_ASSET_SCENE;
    settle(&f, NULL);
    int first = studio_thumbs_stats(s).count[REFERENCE_ASSET_SCENE];
    ecs_entity dot = scene->actors[0].entity;
    begin_transform_edit(scene, dot);
    write_transform_edit(scene, dot, (GameTransform){ .x = 120, .y = 40 });
    finish_transform_edit(scene, false);
    frame(&f, FRAME_SECONDS);
    bool dirty = scene->scene_dirty;
    double moved = until_rendered(&f, REFERENCE_ASSET_SCENE, first, 240);
    int second = studio_thumbs_stats(s).count[REFERENCE_ASSET_SCENE];
    pin_value color = { .type = PIN_U32, .u = 0x3366ffffu };
    assert(
        app_scene_authoring_set_prototype_field(scene, "Dot", "GameAppearance", "color", color) == 0
    );
    double recolored = until_rendered(&f, REFERENCE_ASSET_SCENE, second, 240);
    int third = studio_thumbs_stats(s).count[REFERENCE_ASSET_SCENE];
    printf("  open scene: a move shows %.2f s after it, a prototype's color %.2f s after it\n", moved, recolored);
    test_check(dirty && first == 1 && second == 2 && third == 3, "the open scene's thumbnail follows its unsaved edits and its prototypes'");
    fixture_free(&f);
}

/* Pictures persist between sessions: the next one reads them back instead
   of rendering, until the asset's content changes. */
static void pictures_persist(const char *root)
{
    char cache[64] = "/tmp/chibi-thumb-cache-XXXXXX";
    assert(mkdtemp(cache));
    setenv("CHIBI_THUMBNAIL_CACHE", cache, 1);
    thumbs_fixture f = fixture_open(root, "scenes/Main.kdl");
    f.studio->assets_kind = -1;
    settle(&f, NULL);
    studio_thumb_stats first = studio_thumbs_stats(f.studio);
    fixture_free(&f);
    f = fixture_open(root, "scenes/Main.kdl");
    f.studio->assets_kind = -1;
    settle(&f, NULL);
    studio_thumb_stats second = studio_thumbs_stats(f.studio);
    fixture_free(&f);
    write_in(root, "content/prototypes/Dot.kdl", DOT_PROTOTYPE "// edited\n");
    f = fixture_open(root, "scenes/Main.kdl");
    f.studio->assets_kind = -1;
    settle(&f, NULL);
    studio_thumb_stats third = studio_thumbs_stats(f.studio);
    fixture_free(&f);
    int drawn = rendered(&first) - first.blank;
    printf(
        "  disk cache: %d stored, then %d read back with %d rendered; after an "
        "edit %d read back, %d rendered\n",
        first.stored,
        second.loaded,
        rendered(&second) - second.loaded,
        third.loaded,
        rendered(&third) - third.loaded
    );
    test_check(
        drawn > 0 && first.stored == drawn && second.loaded == drawn &&
            rendered(&second) - second.loaded == second.blank,
        "a later session reads every picture back from the disk cache instead "
        "of rendering it"
    );
    /* Dot, and Main, which shows it, render again; the rest read back. */
    test_check(
        third.count[REFERENCE_ASSET_PROTOTYPE] == 1 &&
            third.count[REFERENCE_ASSET_SCENE] == 1 && third.loaded == drawn - 2,
        "a picture whose content changed renders again rather than reading back"
    );
    setenv("CHIBI_THUMBNAIL_CACHE", "", 1);
    remove_tree(cache);
}

static void read_canvas(CF_Canvas canvas, CF_Pixel *pixels, int count)
{
    CF_Readback read = cf_canvas_readback(canvas);
    while (!cf_readback_ready(read)) {
        cf_sleep(1);
    }
    int bytes = count * (int)sizeof *pixels;
    assert(cf_readback_data(read, pixels, bytes) == bytes);
    cf_destroy_readback(read);
}

/* The disk cache's pictures load back pixel for pixel in a later session,
   one of another size never loads, and past the cap the least recently used
   goes. */
static void disk_round_trip()
{
    enum
    {
        SIDE = 32
    };
    char directory[64] = "/tmp/chibi-thumb-disk-XXXXXX";
    assert(mkdtemp(directory));
    CF_Pixel pixels[SIDE * SIDE], back[SIDE * SIDE];
    for (int i = 0; i < SIDE * SIDE; i++) {
        pixels[i].val = (uint32_t)i * 2654435761u | 0xff000000u;
    }
    studio_thumb_disk *d = studio_thumb_disk_open(directory, (size_t)1 << 20);
    studio_thumb_disk_store(d, 1, pixels, SIDE);
    size_t one = studio_thumb_disk_size(d);
    studio_thumb_disk_close(d);
    d = studio_thumb_disk_open(directory, one * 5 / 2);
    CF_Canvas canvas = cf_make_canvas(cf_canvas_defaults(SIDE, SIDE));
    CF_Canvas small = cf_make_canvas(cf_canvas_defaults(SIDE / 2, SIDE / 2));
    bool loaded = studio_thumb_disk_has(d, 1) &&
                  studio_thumb_disk_load(d, 1, canvas, SIDE);
    cf_app_draw_onto_screen(true);
    read_canvas(canvas, back, SIDE * SIDE);
    bool same = !memcmp(pixels, back, sizeof pixels);
    struct timespec pause = { 0, 20000000 };
    nanosleep(&pause, NULL);
    studio_thumb_disk_store(d, 2, pixels, SIDE);
    nanosleep(&pause, NULL);
    assert(studio_thumb_disk_load(d, 1, canvas, SIDE));
    nanosleep(&pause, NULL);
    studio_thumb_disk_store(d, 3, pixels, SIDE);
    bool lru = studio_thumb_disk_has(d, 1) && !studio_thumb_disk_has(d, 2) &&
               studio_thumb_disk_has(d, 3) &&
               studio_thumb_disk_size(d) <= one * 5 / 2;
    bool other = !studio_thumb_disk_load(d, 3, small, SIDE / 2) &&
                 !studio_thumb_disk_has(d, 3);
    test_check(loaded && same, "a picture in the disk cache loads back pixel for pixel in a later session");
    test_check(lru && other, "the disk cache drops the least recently used past its cap, and a picture of another size");
    cf_destroy_canvas(canvas);
    cf_destroy_canvas(small);
    studio_thumb_disk_close(d);
    remove_tree(directory);
}

/* A scene whose camera sits on endless streamed terrain, with an object
   far off: its picture waits for the terrain between them, and past the
   cap shows what has streamed, marked loading, until the rest has. Each
   frame here passes half a second, so the cap comes within a few steps. */
static void streamed_terrain_waits(const char *root)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/content/fields", root);
    assert(mkdir(path, 0755) == 0);
    char *field = disk_text(NEW_ASSET_FIELD, "Ground");
    write_in(root, "content/fields/Ground.kdl", field);
    free(field);
    write_in(root, "content/prototypes/Ground.kdl", "name \"Ground\"\ncomponents {\n    (object)GAME.GameTransform\n    GAME.MaterialTerrain category=8 cell_size=2.0 field=\"Ground\" island_cells=4000 mask=15 min_fragment_cells=24 seed=7 stream_budget=1 stream_radius=64.0\n}\n");
    write_in(root, "content/prototypes/Eye.kdl", "name \"Eye\"\ncomponents {\n    GAME.GameCamera height=200.0 width=200.0\n    (object)GAME.GameTransform\n}\n");
    write_in(root, "content/scenes/Cave.kdl", "version 1\nname \"Cave\"\n(array)entities {\n    - name=\"Ground\" prototype=\"Ground\"\n    - name=\"Eye\" prototype=\"Eye\"\n    - {\n        name \"Dot\"\n        prototype \"Dot\"\n        components {\n            GAME.GameTransform x=2400.0\n        }\n    }\n}\n");
    thumbs_fixture f = fixture_open(root, "scenes/Cave.kdl");
    studio *s = f.studio;
    s->assets_kind = REFERENCE_ASSET_SYSTEM;
    frame(&f, FRAME_SECONDS);
    int cave = reference_index_find_name(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_SCENE, "Cave");
    assert(cave >= 0);
    double partial;
    double done = until_drawn(&f, cave, 2000, 0.5, &partial);
    bool loading;
    const char *image = studio_thumb(s, cave, STUDIO_THUMB_TILE, &loading);
    studio_thumb_stats stats = studio_thumbs_stats(s);
    printf("  streamed terrain: partial picture at %.1f s, complete at %.1f s\n", partial, done);
    test_check(
        stats.partial == 1 && partial >= 2.0 && done > partial && image &&
            !loading && stats.count[REFERENCE_ASSET_SCENE] == 1,
        "a scene's thumbnail waits for its streamed terrain, then past the cap "
        "shows what it has, loading, until the rest has streamed"
    );
    fixture_free(&f);
}

#ifndef CHIBI_SKIP_PERFORMANCE_TESTS
/* How long a cold start takes to show Neon Swarm's thumbnails, without and
   then with the disk cache. */
static void cold_start(const char *root)
{
    char cache[64] = "/tmp/chibi-thumb-cold-XXXXXX";
    assert(mkdtemp(cache));
    setenv("CHIBI_THUMBNAIL_CACHE", cache, 1);
    double took[2];
    int frames[2];
    for (int run = 0; run < 2; run++) {
        thumbs_fixture f = fixture_open(root, "scenes/Sandbox.kdl");
        f.studio->assets_kind = -1;
        double wall = now();
        frames[run] = settle_within(&f, NULL, 6000);
        took[run] = now() - wall;
        fixture_free(&f);
    }
    printf(
        "  cold start, every visible thumbnail: %.2f s (%d frames) rendering, "
        "%.2f s (%d frames) from the disk cache\n",
        took[0],
        frames[0],
        took[1],
        frames[1]
    );
    test_check(took[1] < took[0], "a cold start shows Neon Swarm's thumbnails sooner from the disk cache");
    setenv("CHIBI_THUMBNAIL_CACHE", "", 1);
    remove_tree(cache);
}
#endif

/* Projects of their own: content changes, unsaved scenes, the disk cache. */
static void project_thumbnails()
{
    char root[64] = "/tmp/chibi-thumbs-XXXXXX";
    make_project(root);
    imported_graph_renders(root);
    changed_file_renders_again(root);
    unsaved_scene_renders(root);
    pictures_persist(root);
    streamed_terrain_waits(root);
    remove_tree(root);
    disk_round_trip();
}

int studio_thumbnails_run(const char *program)
{
    CF_Result result = cf_make_app(
        "Studio thumbnails",
        0,
        0,
        0,
        1280,
        800,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "Studio thumbnails app: %s\n", result.details);
        return 1;
    }
    app_scene_renderer_initialize();
    app_input_initialize();
    arena_thumbnails();
    project_thumbnails();
#ifndef CHIBI_SKIP_PERFORMANCE_TESTS
    static char arena[PATH_MAX];
    assert(realpath(ARENA, arena));
    cold_start(arena);
#endif
    app_input_shutdown();
    cf_destroy_app();
    return 0;
}
