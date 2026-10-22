/* Assets: every indexed asset, filtered by kind and name, as a grid of
 * thumbnails (Unity's Project window, Unreal's Content Browser) or as a
 * list (Unity's one-column view), either grouped by kind under headers that
 * fold. projects/studio's Assets shows either as a window of one List over
 * the `assets` source, and AssetsHeader the filters, the view, the size and
 * New. A click selects, Shift and Ctrl (Cmd) extend and toggle the
 * selection as the Outliner's do, the arrows move it, Enter or a
 * double-click opens the asset, and F2 renames it and Delete deletes every
 * asset selected. Prototypes drag into the Scene or the Outliner to place
 * an instance; behaviors drag onto an Outliner row to attach, and assets
 * fields name onto those fields (studio_drop.c). Resting on a tile shows a
 * large preview, as Unreal's Content Browser does. New makes an asset of
 * any kind Studio indexes and opens it, asking for its name in NewAsset
 * (the `asset_menus` source, with the row menu and New's kinds). */
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_thumbnails.h"
#include "editor/workspace/asset_index.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/xalloc.h"
#include "ui/popover.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *const KIND_LABELS[REFERENCE_ASSET_KIND_COUNT] = {
    "Scenes", "Prototypes", "Behaviors", "Events", "Materials",     "Shapes",
    "Fields", "Libraries",  "Systems",   "UI",     "Relationships",
};
static const char *const KIND_NAMES[REFERENCE_ASSET_KIND_COUNT] = {
    "scene", "prototype", "behavior", "event", "material",     "shape",
    "field", "library",   "system",   "ui",    "relationship",
};
static const uint32_t KIND_ICONS[REFERENCE_ASSET_KIND_COUNT] = {
    KIT_ICON_SCENE,    KIT_ICON_PROTOTYPE, KIT_ICON_GRAPH, KIT_ICON_EVENT,
    KIT_ICON_MATERIAL, KIT_ICON_HEXAGON,   KIT_ICON_GRID,  KIT_ICON_LIBRARY,
    KIT_ICON_SYSTEM,   KIT_ICON_LAYOUT,    KIT_ICON_EVENT,
};

/* A tile's padding around its picture, its label's line, the gap between
   tiles and between lines (theme/assets.rcss, Assets.rcss), and the List's
   own padding across, in dp. */
#define TILE_PAD 4.0f
#define TILE_LABEL 18.0f
#define TILE_GAP 4.0f
#define LIST_PAD 8.0f
/* The pointer rests on a tile this long before its large preview renders,
   just ahead of its tooltip's 500 ms. */
#define REST_SECONDS 0.3
/* Rows, or lines of tiles, shown before the List is laid out. */
#define UNLAID_ROWS 40
#define UNLAID_LINES 8
/* A list row's height, in dp, until the List has the theme's --row. */
#define UNLAID_ROW 26.0f
/* Elements of Assets.rml, AssetsHeader.rml and NewAsset.rml. */
#define LIST_ID "assets-list"
#define MENU_ID "asset-menu"
#define NEW_BUTTON_ID "asset-new-button"
#define NAME_ID "new-asset-name"

const char *studio_asset_kind_name(int kind)
{
    return KIND_NAMES[kind];
}

uint32_t studio_asset_kind_icon(int kind)
{
    return KIND_ICONS[kind];
}

const char *studio_asset_kind_label(int kind)
{
    return KIND_LABELS[kind];
}

static bool grid_view(const studio *s)
{
    return !s->prefs.assets_list;
}

/* Rows of the list: a group header (asset < 0, kind set) or an asset. */
typedef struct
{
    int asset;
    int kind;
} asset_row;

/* Lines of the grid: a kind's header (count < 0), as the list heads each
   kind's rows, or up to a line of its tiles from `first` of the tiles. */
typedef struct
{
    int kind;
    int first, count;
} grid_line;

static const reference_index *sort_index;

static int by_name(const void *a, const void *b)
{
    const reference_asset *x = reference_index_asset_at(sort_index, *(const int *)a);
    const reference_asset *y = reference_index_asset_at(sort_index, *(const int *)b);
    return x->kind != y->kind ? (int)x->kind - (int)y->kind
                              : strcasecmp(x->name, y->name);
}

struct studio_assets_cache
{
    bool built;
    const reference_index *index;
    uint64_t revision;
    int kind;
    unsigned collapsed;
    char filter[128];
    int *uses;
    int *sorted;
    asset_row *rows;
    int *tiles;  /* the grid's assets: every match, in the list's order */
    int columns; /* the grid's, as the List last laid out */
    grid_line *lines; /* the grid's, for those columns */
    float *tops;      /* each line's top in dp, then the end of the last */
    int totals[REFERENCE_ASSET_KIND_COUNT];
    ui_list_state window;  /* the List's */
    char typed[128];       /* the filter as the header's field last showed it */
    rml_element_ref *menu; /* a row's menu to open at menu_x, menu_y, or NULL */
    float menu_x, menu_y;
};

void studio_assets_free(studio *s)
{
    struct studio_assets_cache *cache = s->assets_cache;
    afree(s->assets_marked);
    if (!cache) {
        return;
    }
    afree(cache->uses);
    afree(cache->sorted);
    afree(cache->rows);
    afree(cache->tiles);
    afree(cache->lines);
    afree(cache->tops);
    rml_element_ref_destroy(cache->menu);
    free(cache);
    s->assets_cache = NULL;
}

static struct studio_assets_cache *cache_of(studio *s)
{
    if (!s->assets_cache) {
        s->assets_cache = xcalloc(1, sizeof *s->assets_cache);
        s->assets_cache->columns = 1;
        s->assets_cache->window = (ui_list_state)UI_LIST_STATE_INIT;
    }
    return s->assets_cache;
}

/* Whether a kind's group shows its assets: unless folded, and always while
   a name filter narrows them. */
static bool kind_open(const studio *s, int kind)
{
    return !(s->assets_collapsed & (1u << kind)) || s->assets_filter[0];
}

/* Virtualized rows still need a sorted model. Reuse it during scrolling and
 * unrelated edits; a completed index rebuild invalidates the numeric asset IDs. */
static struct studio_assets_cache *asset_rows(studio *s, const reference_index *index)
{
    struct studio_assets_cache *cache = cache_of(s);
    bool rebuild_index = !cache->built || cache->index != index ||
                         cache->revision != s->scene->asset_index_revision;
    if (!rebuild_index && cache->kind == s->assets_kind &&
        cache->collapsed == s->assets_collapsed &&
        !strcmp(cache->filter, s->assets_filter)) {
        return cache;
    }
    s->panel_work.asset_rows++;
    cache->built = true;
    cache->index = index;
    cache->revision = s->scene->asset_index_revision;
    cache->kind = s->assets_kind;
    cache->collapsed = s->assets_collapsed;
    snprintf(cache->filter, sizeof cache->filter, "%s", s->assets_filter);
    if (rebuild_index) {
        int count = index ? reference_index_asset_count(index) : 0;
        asetlen(cache->uses, count);
        if (count) {
            memset(cache->uses, 0, (size_t)count * sizeof *cache->uses);
        }
        for (int r = 0; index && r < reference_index_count(index); r++) {
            int used = reference_dependency(reference_index_at(index, r));
            if (used >= 0) {
                cache->uses[used]++;
            }
        }
        aclear(cache->sorted);
        for (int i = 0; i < count; i++) {
            const reference_asset *asset = reference_index_asset_at(index, i);
            if (asset->name && asset->kind >= 0 &&
                asset->kind < REFERENCE_ASSET_KIND_COUNT) {
                apush(cache->sorted, i);
            }
        }
        sort_index = index;
        if (acount(cache->sorted)) {
            qsort(cache->sorted, (size_t)acount(cache->sorted), sizeof *cache->sorted, by_name);
        }
    }
    bool grouped = s->assets_kind < 0;
    int previous_kind = -1;
    memset(cache->totals, 0, sizeof cache->totals);
    aclear(cache->rows);
    aclear(cache->tiles);
    aclear(cache->lines); /* until the grid lays the new tiles out */
    for (int m = 0; m < acount(cache->sorted); m++) {
        int id = cache->sorted[m];
        const reference_asset *asset = reference_index_asset_at(index, id);
        int kind = (int)asset->kind;
        if ((s->assets_kind >= 0 && kind != s->assets_kind) ||
            !studio_matches(s->assets_filter, asset->name)) {
            continue;
        }
        cache->totals[kind]++;
        apush(cache->tiles, id);
        if (grouped && kind != previous_kind) {
            apush(cache->rows, ((asset_row){ -1, kind }));
        }
        previous_kind = kind;
        if (!grouped || kind_open(s, kind)) {
            apush(cache->rows, ((asset_row){ id, kind }));
        }
    }
    return cache;
}

/* ---------- Selection ---------- */

/* The selection is the primary asset (assets_selected), which F2, Enter and
   the arrows act on, and the others marked with it, as the Outliner selects
   objects: a click selects one, Shift extends from the anchor and Ctrl (Cmd
   on a Mac) toggles one. */

static bool same(const studio_asset_ref *ref, const reference_asset *asset)
{
    return (int)asset->kind == ref->kind && !strcmp(asset->name, ref->name);
}

static studio_asset_ref ref_of(const reference_asset *asset)
{
    studio_asset_ref ref = { .kind = (int)asset->kind };
    snprintf(ref.name, sizeof ref.name, "%s", asset->name);
    return ref;
}

static bool is_selected(const studio *s, const reference_asset *asset)
{
    return s->assets_selected[0] && (int)asset->kind == s->assets_selected_kind &&
           !strcmp(asset->name, s->assets_selected);
}

static int marked_at(const studio *s, const reference_asset *asset)
{
    for (int i = 0; i < acount(s->assets_marked); i++) {
        if (same(&s->assets_marked[i], asset)) {
            return i;
        }
    }
    return -1;
}

/* Selected with the primary, which is selected itself. */
static bool is_marked(const studio *s, const reference_asset *asset)
{
    return marked_at(s, asset) >= 0;
}

static void set_primary(studio *s, const studio_asset_ref *ref)
{
    s->assets_selected_kind = ref->kind;
    snprintf(s->assets_selected, sizeof s->assets_selected, "%s", ref->name);
}

static studio_asset_ref primary(const studio *s)
{
    studio_asset_ref ref = { .kind = s->assets_selected_kind };
    snprintf(ref.name, sizeof ref.name, "%s", s->assets_selected);
    return ref;
}

/* Selects the asset alone, where Shift next extends from. */
static void select_asset(studio *s, const reference_asset *asset)
{
    aclear(s->assets_marked);
    s->assets_anchor = ref_of(asset);
    set_primary(s, &s->assets_anchor);
}

/* Ctrl's click: adds the asset to the selection as its primary, or takes it
   out, the last one marked becoming the primary (Unity's toggle). */
static void toggle_asset(studio *s, const reference_asset *asset)
{
    int at = marked_at(s, asset);
    if (at >= 0) {
        adel(s->assets_marked, at);
        return;
    }
    if (is_selected(s, asset)) {
        studio_asset_ref next = { .kind = -1 };
        if (acount(s->assets_marked)) {
            next = apop(s->assets_marked);
        }
        set_primary(s, &next);
        return;
    }
    if (s->assets_selected[0]) {
        apush(s->assets_marked, primary(s));
    }
    s->assets_anchor = ref_of(asset);
    set_primary(s, &s->assets_anchor);
}

/* The assets in the order the view shows them: the grid's tiles, but for
   those of a collapsed kind, or the list's asset rows. */
static int *shown_assets(const studio *s, const struct studio_assets_cache *cache)
{
    int *shown = NULL;
    for (int l = 0; grid_view(s) && l < acount(cache->lines); l++) {
        const grid_line *line = &cache->lines[l];
        for (int i = 0; i < line->count; i++) {
            apush(shown, cache->tiles[line->first + i]);
        }
    }
    for (int i = 0; !grid_view(s) && i < acount(cache->rows); i++) {
        if (cache->rows[i].asset >= 0) {
            apush(shown, cache->rows[i].asset);
        }
    }
    return shown;
}

static int position_of(const reference_index *index, const int *shown, const studio_asset_ref *ref)
{
    for (int i = 0; ref->name[0] && i < acount(shown); i++) {
        if (same(ref, reference_index_asset_at(index, shown[i]))) {
            return i;
        }
    }
    return -1;
}

static int selected_position(const studio *s, const reference_index *index, const int *shown)
{
    studio_asset_ref selected = primary(s);
    return position_of(index, shown, &selected);
}

/* Shift's click or arrow: selects the shown assets from the anchor to
   `to`, a place in `shown`, which becomes the primary. */
static void extend_to(studio *s, const reference_index *index, const int *shown, int to)
{
    int from = position_of(index, shown, &s->assets_anchor);
    from = from < 0 ? to : from;
    aclear(s->assets_marked);
    int step = from <= to ? 1 : -1;
    for (int i = from; i != to; i += step) {
        apush(s->assets_marked, ref_of(reference_index_asset_at(index, shown[i])));
    }
    studio_asset_ref last = ref_of(reference_index_asset_at(index, shown[to]));
    set_primary(s, &last);
}

/* The grid line holding place `at` of the grid's shown tiles, and its
   column there; -1 when none does. */
static int line_of(const struct studio_assets_cache *cache, int at, int *column)
{
    for (int l = 0, seen = 0; at >= 0 && l < acount(cache->lines); l++) {
        int count = cache->lines[l].count;
        if (count > 0 && at < seen + count) {
            *column = at - seen;
            return l;
        }
        seen += count > 0 ? count : 0;
    }
    return -1;
}

/* The list row or grid line to scroll into view: the selection's, after the
   keyboard moved it; -1 otherwise. */
static int reveal_row(studio *s, const struct studio_assets_cache *cache)
{
    const reference_index *index = cache->index;
    if (!s->assets_reveal) {
        return -1;
    }
    s->assets_reveal = false;
    if (grid_view(s)) {
        int *shown = shown_assets(s, cache), column;
        int line = line_of(cache, selected_position(s, index, shown), &column);
        afree(shown);
        return line;
    }
    for (int i = 0; i < acount(cache->rows); i++) {
        if (cache->rows[i].asset >= 0 &&
            is_selected(s, reference_index_asset_at(index, cache->rows[i].asset))) {
            return i;
        }
    }
    return -1;
}

static uint32_t asset_icon(studio *s, const reference_asset *asset)
{
    return asset->unresolved ? KIT_ICON_WARNING
         : asset->kind == REFERENCE_ASSET_PROTOTYPE
             ? studio_prototype_icon(s, asset->name)
             : KIND_ICONS[asset->kind];
}

/* "3 uses" or "unused" in `out`; false for scenes, entry points nothing
   references, whose usage says nothing. */
static bool usage(
    const struct studio_assets_cache *cache,
    int id,
    const reference_asset *asset,
    char *out,
    size_t size
)
{
    int used = cache->uses[id];
    out[0] = '\0';
    if (asset->kind == REFERENCE_ASSET_SCENE) {
        return false;
    }
    if (!used) {
        snprintf(out, size, "unused");
        return true;
    }
    snprintf(out, size, "%d %s", used, used == 1 ? "use" : "uses");
    return true;
}

/* Where a step of the arrows lands among the shown assets: `step` places
   on, or with `lines` a line of the grid up or down, in the same column or
   the last of a shorter line. Without a selection, a step back or down
   lands on the first asset and End on the last. */
static int stepped(const struct studio_assets_cache *cache, int at, int count, int step, int lines)
{
    if (at < 0) {
        return step >= count ? count - 1 : 0;
    }
    if (!lines) {
        at += step;
        return at < 0 ? 0 : at >= count ? count - 1 : at;
    }
    int column, line = line_of(cache, at, &column);
    int next = line + lines;
    while (next >= 0 && next < acount(cache->lines) && cache->lines[next].count <= 0) {
        next += lines;
    }
    if (next < 0 || next >= acount(cache->lines)) {
        return at;
    }
    int seen = 0;
    for (int l = 0; l < next; l++) {
        seen += cache->lines[l].count > 0 ? cache->lines[l].count : 0;
    }
    int last = cache->lines[next].count - 1;
    return seen + (column < last ? column : last);
}

/* Moves the selection, alone or, with Shift, extending it from the anchor. */
static void move_selection(studio *s, int step, int lines, bool extend)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int *shown = s->assets_cache ? shown_assets(s, s->assets_cache) : NULL;
    int count = acount(shown);
    if (count) {
        int at = stepped(s->assets_cache, selected_position(s, index, shown), count, step, lines);
        if (extend) {
            extend_to(s, index, shown, at);
        } else {
            select_asset(s, reference_index_asset_at(index, shown[at]));
        }
        s->assets_reveal = true;
        s->ui_schedule.wake = true;
    }
    afree(shown);
}

static void open_asset(studio *s, int asset)
{
    finish_transform_edit(s->scene, false);
    if (app_scene_authoring_open_asset(s->scene, asset) == 0 &&
        s->scene->active_tab >= 0 && s->scene->tabs[s->scene->active_tab].source_only) {
        studio_behavior_show(s, 1);
    }
}

/* The cache when it shows the current index, else NULL. */
static const struct studio_assets_cache *current_cache(studio *s)
{
    const struct studio_assets_cache *cache = s->assets_cache;
    bool current = cache &&
                   cache->index == app_scene_authoring_asset_index(s->scene) &&
                   cache->revision == s->scene->asset_index_revision;
    return current ? cache : NULL;
}

/* The selected asset while the view shows it, or -1: a filter or a
   collapsed group hides the selection from the keys too. */
static int selected_asset(studio *s)
{
    const struct studio_assets_cache *cache = current_cache(s);
    if (!cache) {
        return -1;
    }
    int *shown = shown_assets(s, cache);
    int at = selected_position(s, cache->index, shown);
    int asset = at < 0 ? -1 : shown[at];
    afree(shown);
    return asset;
}

/* Every selected asset the view shows, in its order. */
static int *selected_assets(studio *s)
{
    const struct studio_assets_cache *cache = current_cache(s);
    int *shown = cache ? shown_assets(s, cache) : NULL, *selected = NULL;
    for (int i = 0; i < acount(shown); i++) {
        const reference_asset *asset = reference_index_asset_at(cache->index, shown[i]);
        if (is_selected(s, asset) || is_marked(s, asset)) {
            apush(selected, shown[i]);
        }
    }
    afree(shown);
    return selected;
}

static bool open_selection(studio *s)
{
    int asset = selected_asset(s);
    if (asset < 0) {
        return false;
    }
    open_asset(s, asset);
    return true;
}

/* The arrows move the selection (Left and Right only in the grid, Up and
   Down a line of it), Home and End go to the ends, and Enter opens it; Shift
   extends the selection. */
static bool navigation_key(studio *s, SDL_Keycode key, bool extend)
{
    bool grid = grid_view(s);
    int far = s->assets_cache ? acount(s->assets_cache->tiles) : 0;
    switch (key) {
        case SDLK_UP: move_selection(s, -1, grid ? -1 : 0, extend); return true;
        case SDLK_DOWN: move_selection(s, 1, grid ? 1 : 0, extend); return true;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            if (grid) {
                move_selection(s, key == SDLK_LEFT ? -1 : 1, 0, extend);
            }
            return grid;
        case SDLK_HOME: move_selection(s, -far, 0, extend); return true;
        case SDLK_END: move_selection(s, far, 0, extend); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return !extend && open_selection(s);
        default: return false;
    }
}

/* The element with `id` in Studio's documents, or NULL. */
static rml_element *element_by_id(studio *s, const char *id)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* Hidden slots keep the asset they last showed, so the one shown is the
   match with a size. */
#define MAX_SLOTS 8

/* What F2 or Delete asks opens under the asset's tile or row, which the
   keys acted on, rather than at the pointer; at the pointer when the asset
   is scrolled out of view. */
static void anchor_popover(studio *s, int asset)
{
    rml_element *list = element_by_id(s, LIST_ID), *slots[MAX_SLOTS];
    char selector[32];
    snprintf(selector, sizeof selector, "[data-asset='%d']", asset);
    int count = list ? rml_element_query_selector_all(list, selector, slots, MAX_SLOTS)
                     : 0;
    for (int i = 0; i < count && i < MAX_SLOTS; i++) {
        rml_vec2 at = rml_element_absolute_offset(slots[i], RML_BOX_BORDER);
        rml_vec2 size = rml_element_box_size(slots[i], RML_BOX_BORDER);
        if (size.x > 0) {
            float *box = s->refactor.anchor;
            box[0] = at.x;
            box[1] = at.y;
            box[2] = size.x;
            box[3] = size.y;
            return;
        }
    }
}

/* Deletes every selected asset the view shows, as one change. The assets go
   by kind and name, since deleting rebuilds the index that numbers them. */
static void delete_selection(studio *s)
{
    const struct studio_assets_cache *cache = current_cache(s);
    int *assets = selected_assets(s);
    studio_asset_ref *refs = NULL;
    for (int i = 0; i < acount(assets); i++) {
        apush(refs, ref_of(reference_index_asset_at(cache->index, assets[i])));
    }
    if (acount(refs)) {
        studio_refactor_delete_assets(s, refs, acount(refs));
    }
    afree(refs);
    afree(assets);
}

/* Keys act on the selection, as they do in the Outliner and the graph, so
   where the pointer rests never picks what F2 renames or Delete removes. */
bool studio_assets_key(studio *s, SDL_Keycode key, SDL_Keymod mod)
{
    if (studio_designer_active(s) || studio_primary(s, mod) || (mod & SDL_KMOD_ALT)) {
        return false;
    }
    bool shift = mod & SDL_KMOD_SHIFT;
    if (navigation_key(s, key, shift)) {
        return true;
    }
    int asset = shift ? -1 : selected_asset(s);
    if (asset < 0) {
        return false;
    }
    switch (key) {
        case SDLK_F2: studio_refactor_rename_asset(s, asset); break;
        case SDLK_X:
        case SDLK_DELETE:
        case SDLK_BACKSPACE: delete_selection(s); break;
        default: return false;
    }
    anchor_popover(s, asset);
    return true;
}

/* ---------- New assets ---------- */

/* Each kind New makes, in the order of new_asset_kind and then ui_new_kind,
   grouped as Blender's Add and Unity's Create menus group theirs. Its New …
   command gives its label and icon. */
static const struct
{
    const char *group;
    const char *name;          /* its default name, numbered when taken */
    reference_asset_kind kind; /* the Assets filter that lists it first */
} NEW_KINDS[STUDIO_NEW_KIND_COUNT] = {
    { "Scene", "NewScene", REFERENCE_ASSET_SCENE },
    { "Scene", "NewPrototype", REFERENCE_ASSET_PROTOTYPE },
    { "Logic", "NewRelationship", REFERENCE_ASSET_RELATIONSHIP },
    { "Logic", "NewBehavior", REFERENCE_ASSET_BEHAVIOR },
    { "Logic", "NewEvent", REFERENCE_ASSET_EVENT },
    { "Logic", "NewSystem", REFERENCE_ASSET_SYSTEM },
    { "Logic", "NewLibrary", REFERENCE_ASSET_LIBRARY },
    { "Rendering", "NewMaterial", REFERENCE_ASSET_MATERIAL },
    { "Rendering", "NewShape", REFERENCE_ASSET_SHAPE },
    { "Rendering", "NewField", REFERENCE_ASSET_FIELD },
    { "Rendering", "NewShaderLibrary", REFERENCE_ASSET_LIBRARY },
    { "Rendering", "NewModel", REFERENCE_ASSET_SHAPE },
    { "UI", "NewWidget", REFERENCE_ASSET_UI },
    { "UI", "NewDocument", REFERENCE_ASSET_UI },
    { "UI", "NewStylesheet", REFERENCE_ASSET_UI },
};
static const mode_t FOLDER_MODE = 0777;

static bool is_ui(int kind)
{
    return kind >= NEW_ASSET_KIND_COUNT;
}

static studio_command new_command(int kind)
{
    return kind == NEW_ASSET_RELATIONSHIP
             ? STUDIO_COMMAND_NEW_RELATIONSHIP
             : (studio_command)(STUDIO_COMMAND_NEW_SCENE + kind -
                                (kind > NEW_ASSET_RELATIONSHIP));
}

/* Whether an asset of the project has the name; a name only referenced is
   free, so making the asset resolves its references. */
static bool named(studio *s, const char *name)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    for (int i = 0; i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (!asset->unresolved && asset->name && !strcmp(asset->name, name)) {
            return true;
        }
    }
    return false;
}

static char *asset_path(const char *folder, const char *name, const char *extension, char *error, size_t size)
{
    size_t length = strlen(folder) + strlen(name) + strlen(extension) + 2;
    char *path = xmalloc(length);
    snprintf(path, length, "%s/%s%s", folder, name, extension);
    struct stat info;
    if (stat(path, &info) != 0) {
        return path;
    }
    snprintf(error, size, "%s already exists", content_basename(path));
    free(path);
    return NULL;
}

/* The new asset's file uses its kind's extension, a ui asset's .rml or a
   stylesheet's .rcss; NULL with why in `error` when the name is invalid or
   taken. */
static char *new_path(studio *s, int kind, const char *name, char *error, size_t size)
{
    if (!new_asset_name_valid(name, error, size)) {
        return NULL;
    }
    if (named(s, name)) {
        snprintf(error, size, "%s already exists", name);
        return NULL;
    }
    const char *folder_name = is_ui(kind) ? "ui"
                                          : new_asset_folder((new_asset_kind)kind);
    char *folder = content_path_join(s->scene->content_root, folder_name);
    char *path = is_ui(kind)
                   ? ui_buffer_new_path(folder, (ui_new_kind)(kind - NEW_ASSET_KIND_COUNT), name, error, size)
                   : asset_path(folder, name, new_asset_extension((new_asset_kind)kind), error, size);
    free(folder);
    return path;
}

/* The kind's name, numbered from 2 until no asset or file has it. */
static void default_name(studio *s, int kind, char *out, size_t size)
{
    const char *base = NEW_KINDS[kind].name;
    snprintf(out, size, "%s", base);
    for (int i = 2;; i++) {
        char error[160];
        char *path = new_path(s, kind, out, error, sizeof error);
        bool free_name = path != NULL;
        free(path);
        if (free_name) {
            return;
        }
        snprintf(out, size, "%s%d", base, i);
    }
}

/* Opens the name popover once a sync shows the name, so its field has the
   name when it takes the focus and selects it. */
static void ask_next(studio *s)
{
    s->new_asset.asking = STUDIO_NAME_ASKED;
    s->ui_schedule.wake = true;
}

/* Asks for the name of a new asset of `kind`, offering a free one. What the
   name field last showed stays, so the field's next edit is told apart from
   the name offered. */
void studio_new_asset_start(studio *s, int kind)
{
    studio_new_asset *n = &s->new_asset;
    n->kind = kind;
    n->closing = false;
    n->error[0] = '\0';
    app_scene_authoring_refresh_asset_index(s->scene);
    default_name(s, kind, n->name, sizeof n->name);
    ask_next(s);
}

static bool write_ui(ui_new_kind kind, const char *path)
{
    int result = 0;
    for (int p = 0; p < UI_PART_COUNT && !result; p++) {
        const char *text = ui_buffer_template(kind, (ui_part)p);
        char *file = text ? ui_buffer_part_file(path, (ui_part)p) : NULL;
        result = file ? write_file_atomic(file, text) : 0;
        free(file);
    }
    return result == 0;
}

/* The asset as its model writes it (assets/new_asset.h), which loads back
   unchanged and is what --format writes. */
static bool write_code(new_asset_kind kind, const char *name, const char *path)
{
    char *text = new_asset_text(kind, name);
    bool written = text && write_file_atomic(path, text) == 0;
    free(text);
    return written;
}

/* Studio takes the new file up, as it takes up any file that appears, and
   opens it as Assets does: a UI document in the Text Editor. */
static void open_new(studio *s, int kind, const char *path)
{
    if (is_ui(kind)) {
        content_refresh(s->scene);
        app_scene_authoring_refresh_asset_index(s->scene);
        studio_open_ui_source(s, path, 1);
        return;
    }
    scene_disk_follow(s->scene, &(scene_move){ .to = path }, 1);
    app_scene_authoring_refresh_asset_index(s->scene);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    finish_transform_edit(s->scene, false);
    int asset = reference_index_find_asset(index, NEW_KINDS[kind].kind, path, NULL);
    if (app_scene_authoring_open_asset(s->scene, asset) != 0) {
        studio_error(s, "Could not open", app_scene_authoring_message(s->scene));
    } else if (s->scene->active_tab >= 0 && s->scene->tabs[s->scene->active_tab].source_only) {
        studio_behavior_show(s, 1);
    }
}

/* Writes the new asset in its kind's folder and opens it; false when the
   name is invalid or taken. */
static bool create(studio *s)
{
    studio_new_asset *n = &s->new_asset;
    app_scene_authoring_refresh_asset_index(s->scene);
    char *path = new_path(s, n->kind, n->name, n->error, sizeof n->error);
    if (!path) {
        return false;
    }
    char *folder = xstrdup(path);
    *strrchr(folder, '/') = '\0';
    mkdir(folder, FOLDER_MODE);
    free(folder);
    bool written = is_ui(n->kind)
                     ? write_ui((ui_new_kind)(n->kind - NEW_ASSET_KIND_COUNT), path)
                     : write_code((new_asset_kind)n->kind, n->name, path);
    if (written) {
        open_new(s, n->kind, path);
    } else {
        studio_error(s, "Could not create", content_basename(path));
    }
    free(path);
    return true;
}

/* ---------- Data, written in place ---------- */

static yyjson_mut_val *member(yyjson_mut_val *object, const char *name)
{
    yyjson_mut_val *value = yyjson_mut_obj_get(object, name);
    assert(value);
    return value;
}

/* What a text field shows for NULL: nothing. */
static const char *or_empty(const char *text)
{
    return text ? text : "";
}

static bool put_glyph(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *name, uint32_t icon)
{
    char text[5] = "";
    if (icon) {
        studio_glyph(text, icon);
    }
    return ui_data_put_text(doc, object, name, text);
}

/* The shapes of the items arrays hold: a row or tile, a line of tiles and
   a menu entry. */
typedef enum
{
    SHAPE_ITEM,
    SHAPE_LINE,
    SHAPE_ENTRY,
} item_shape;

static const char *const ITEM_TEXTS[] = { "label", "icon",  "meta",  "drag",
                                          "image", "badge", "about", "top" };
static const char *const ITEM_FLAGS[] = { "parent",  "open",       "selected",
                                          "active",  "marked",     "hidden",
                                          "loading", "unresolved", "unused",
                                          "shown" };
static const char *const ITEM_NUMBERS[] = { "key", "kind", "depth" };
static const char *const ENTRY_TEXTS[] = { "id", "action", "label", "icon", "shortcut" };
static const char *const ENTRY_FLAGS[] = { "checked", "disabled", "separator" };
static const char *const HEADER_TEXTS[] = { "filter", "tileWidth", "tileHeight", "empty" };
static const char *const NAMING_TEXTS[] = { "label", "name", "error" };

#define COUNT(a) (sizeof(a) / sizeof *(a))

static void add_texts(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_obj_add_strcpy(doc, object, names[i], "");
    }
}

static void add_flags(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_obj_add_bool(doc, object, names[i], false);
    }
}

static yyjson_mut_val *made(yyjson_mut_doc *doc, item_shape shape)
{
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    switch (shape) {
        case SHAPE_ITEM:
            add_texts(doc, item, ITEM_TEXTS, COUNT(ITEM_TEXTS));
            add_flags(doc, item, ITEM_FLAGS, COUNT(ITEM_FLAGS));
            for (size_t i = 0; i < COUNT(ITEM_NUMBERS); i++) {
                yyjson_mut_obj_add_int(doc, item, ITEM_NUMBERS[i], -1);
            }
            break;
        case SHAPE_LINE:
            yyjson_mut_obj_add_bool(doc, item, "shown", false);
            yyjson_mut_obj_add_strcpy(doc, item, "top", "");
            yyjson_mut_obj_add_val(doc, item, "head", made(doc, SHAPE_ITEM));
            yyjson_mut_obj_add_arr(doc, item, "tiles");
            break;
        case SHAPE_ENTRY:
            add_texts(doc, item, ENTRY_TEXTS, COUNT(ENTRY_TEXTS));
            add_flags(doc, item, ENTRY_FLAGS, COUNT(ENTRY_FLAGS));
            yyjson_mut_obj_add_strcpy(doc, yyjson_mut_obj_add_obj(doc, item, "heading"), "label", "");
            break;
    }
    return item;
}

/* Gives the array `length` items, adding and removing at its end, so items
 * already shown keep their elements; true when its length changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *items, int length, item_shape shape)
{
    int had = (int)yyjson_mut_arr_size(items);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(items, made(doc, shape));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(items);
    }
    return had != length;
}

/* What a row or tile shows, and where in the List's spacer, in dp; NULL
   texts show nothing. */
typedef struct
{
    int id, kind, depth;
    const char *label, *meta, *drag, *image, *about;
    uint32_t icon, badge;
    float top; /* a row's */
    bool parent, open, loading, unresolved, unused;
    bool marked; /* selected with the primary, whose key the view compares */
} item_view;

static void dp_text(char out[32], float value)
{
    snprintf(out, 32, "%.0fdp", value);
}

static bool show_item(yyjson_mut_doc *doc, yyjson_mut_val *item, const item_view *v)
{
    bool changed = ui_data_put_int(item, "key", v->id);
    changed |= ui_data_put_int(item, "kind", v->kind);
    changed |= ui_data_put_int(item, "depth", v->depth);
    changed |= ui_data_put_text(doc, item, "label", or_empty(v->label));
    changed |= ui_data_put_text(doc, item, "meta", or_empty(v->meta));
    changed |= ui_data_put_text(doc, item, "drag", or_empty(v->drag));
    changed |= ui_data_put_text(doc, item, "image", or_empty(v->image));
    changed |= ui_data_put_text(doc, item, "about", or_empty(v->about));
    changed |= put_glyph(doc, item, "icon", v->icon);
    changed |= put_glyph(doc, item, "badge", v->badge);
    changed |= ui_data_put_bool(item, "parent", v->parent);
    changed |= ui_data_put_bool(item, "open", v->open);
    changed |= ui_data_put_bool(item, "loading", v->loading);
    changed |= ui_data_put_bool(item, "unresolved", v->unresolved);
    changed |= ui_data_put_bool(item, "unused", v->unused);
    changed |= ui_data_put_bool(item, "marked", v->marked);
    changed |= ui_data_put_bool(item, "selected", v->marked);
    char top[32];
    dp_text(top, v->top);
    changed |= ui_data_put_text(doc, item, "top", top);
    return ui_data_put_bool(item, "shown", true) || changed;
}

/* A menu entry: a Studio command's (id) or an asset menu's (action). */
typedef struct
{
    const char *id, *action, *label, *shortcut, *heading;
    uint32_t icon;
    bool checked, disabled, separator;
} entry_view;

static bool show_entries(yyjson_mut_doc *doc, yyjson_mut_val *items, const entry_view *entries, int count)
{
    bool changed = fit(doc, items, count, SHAPE_ENTRY);
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(items);
    for (const entry_view *e = entries; (item = yyjson_mut_arr_iter_next(&it)); e++) {
        changed |= ui_data_put_text(doc, item, "id", or_empty(e->id));
        changed |= ui_data_put_text(doc, item, "action", or_empty(e->action));
        changed |= ui_data_put_text(doc, item, "label", or_empty(e->label));
        changed |= ui_data_put_text(doc, item, "shortcut", or_empty(e->shortcut));
        changed |= ui_data_put_text(doc, member(item, "heading"), "label", or_empty(e->heading));
        changed |= put_glyph(doc, item, "icon", e->icon);
        changed |= ui_data_put_bool(item, "checked", e->checked);
        changed |= ui_data_put_bool(item, "disabled", e->disabled);
        changed |= ui_data_put_bool(item, "separator", e->separator);
    }
    return changed;
}

/* ---------- The `assets` source: Assets and AssetsHeader ---------- */

/* The kind filter's choices, a table as EnumField lists one: All, then
   each kind. */
static void add_kinds(yyjson_mut_doc *doc, yyjson_mut_val *assets)
{
    yyjson_mut_val *kinds = yyjson_mut_obj_add_obj(doc, assets, "kinds");
    yyjson_mut_val *groups = yyjson_mut_obj_add_arr(doc, kinds, "groups");
    yyjson_mut_val *group = yyjson_mut_arr_add_obj(doc, groups);
    yyjson_mut_obj_add_str(doc, group, "label", "");
    yyjson_mut_val *options = yyjson_mut_obj_add_arr(doc, group, "options");
    for (int kind = -1; kind < REFERENCE_ASSET_KIND_COUNT; kind++) {
        yyjson_mut_val *option = yyjson_mut_arr_add_obj(doc, options);
        yyjson_mut_obj_add_int(doc, option, "value", kind);
        yyjson_mut_obj_add_str(doc, option, "label", kind < 0 ? "All" : KIND_LABELS[kind]);
        char icon[5] = "";
        studio_glyph(icon, kind < 0 ? KIT_ICON_ASSETS : KIND_ICONS[kind]);
        yyjson_mut_obj_add_strcpy(doc, option, "icon", icon);
    }
}

void studio_assets_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *assets = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "assets");
    yyjson_mut_obj_add_int(doc, assets, "kind", -1);
    add_kinds(doc, assets);
    yyjson_mut_obj_add_int(doc, assets, "tile", STUDIO_ASSETS_TILE);
    yyjson_mut_obj_add_bool(doc, assets, "grid", true);
    add_texts(doc, assets, HEADER_TEXTS, COUNT(HEADER_TEXTS));
    yyjson_mut_obj_add_arr(doc, assets, "rows");
    yyjson_mut_obj_add_arr(doc, assets, "lines");
}

/* The filter as the header's field typed it, or as Studio changed it. */
static bool take_filter(studio *s, const studio_panel_sync *at, struct studio_assets_cache *cache)
{
    yyjson_mut_val *filter = member(at->variable, "filter");
    const char *shown = yyjson_mut_get_str(filter);
    if (!at->preview && strcmp(shown, cache->typed)) {
        snprintf(s->assets_filter, sizeof s->assets_filter, "%s", shown);
        snprintf(cache->typed, sizeof cache->typed, "%s", shown);
        return false;
    }
    if (!strcmp(shown, s->assets_filter)) {
        return false;
    }
    ui_data_set_text(at->doc, filter, s->assets_filter);
    if (!at->preview) {
        snprintf(cache->typed, sizeof cache->typed, "%s", s->assets_filter);
    }
    return true;
}

/* The filters, the size and the tile size it gives. */
static bool show_header(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *assets)
{
    float art = (float)s->prefs.assets_tile;
    char width_text[32], height_text[32];
    dp_text(width_text, art + 2 * TILE_PAD);
    dp_text(height_text, art + 2 * TILE_PAD + TILE_LABEL);
    bool changed = ui_data_put_int(assets, "kind", s->assets_kind);
    changed |= ui_data_put_int(assets, "tile", s->prefs.assets_tile);
    changed |= ui_data_put_bool(assets, "grid", grid_view(s));
    changed |= ui_data_put_text(doc, assets, "tileWidth", width_text);
    return ui_data_put_text(doc, assets, "tileHeight", height_text) || changed;
}

/* The asset under the pointer in an Assets list, or -1. */
static int hovered_asset(studio *s)
{
    rml_element *el = rml_context_hover_element(rml_backend_context(s->backend));
    for (; el; el = rml_element_parent(el)) {
        char id[16];
        if (rml_element_attribute(el, "data-asset", id, sizeof id) > 0) {
            return atoi(id);
        }
    }
    return -1;
}

/* The tile the pointer rests on, once it rested long enough to want its
   large preview, which renders while the tooltip waits to show; else -1. */
static int resting(studio *s)
{
    if (s->assets_hover != s->assets_rest) {
        s->assets_rest = s->assets_hover;
        s->assets_rest_since = s->time;
    }
    return s->assets_rest >= 0 && s->time - s->assets_rest_since >= REST_SECONDS
             ? s->assets_rest
             : -1;
}

/* A kind's group header, in the list or the grid: its name, how many of
   it match and a twisty that folds it. */
static bool show_header_row(
    studio *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *item,
    const struct studio_assets_cache *cache,
    int kind,
    float top
)
{
    char total[16];
    snprintf(total, sizeof total, "%d", cache->totals[kind]);
    item_view header = { .id = -1,
                         .kind = kind,
                         .label = KIND_LABELS[kind],
                         .icon = KIND_ICONS[kind],
                         .meta = total,
                         .top = top,
                         .parent = true,
                         .open = kind_open(s, kind) };
    return show_item(doc, item, &header);
}

/* A kind's group header, or an asset, as a list row. */
static bool show_row(
    studio *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *item,
    const struct studio_assets_cache *cache,
    asset_row row,
    float top
)
{
    if (row.asset < 0) {
        return show_header_row(s, doc, item, cache, row.kind, top);
    }
    const reference_asset *asset = reference_index_asset_at(
        app_scene_authoring_asset_index(s->scene),
        row.asset
    );
    bool grouped = s->assets_kind < 0;
    char meta[24];
    bool counted = usage(cache, row.asset, asset, meta, sizeof meta);
    item_view v = {
        .id = row.asset,
        .kind = row.kind,
        .depth = grouped,
        .label = asset->name,
        .icon = asset_icon(s, asset),
        .meta = grouped ? meta : KIND_NAMES[row.kind],
        .drag = studio_asset_draggable(asset) ? KIND_NAMES[row.kind] : "",
        .unresolved = asset->unresolved,
        .top = top,
        .unused = counted && !cache->uses[row.asset],
        .marked = is_marked(s, asset),
    };
    return show_item(doc, item, &v);
}

/* The window's rows, or lines of tiles, in slots that keep one while it
   stays in view, as Virtuoso keys its items: row `i` takes slot i % slots
   and its place in the spacer, so a scroll changes only the slots of the
   rows that enter. Slots only grow, so a narrower filter hides them rather
   than dropping their elements. A slot's attributes restyle its siblings
   (RmlUi dirties siblings for sibling combinators), so tiles sit in lines
   of their own rather than all in the spacer. */
typedef struct
{
    yyjson_mut_val *items;
    int count;      /* the slots */
    int start, end; /* the items in view */
} slot_window;

static slot_window slots(yyjson_mut_doc *doc, yyjson_mut_val *items, int needed, item_shape shape, bool *changed)
{
    if ((int)yyjson_mut_arr_size(items) < needed) {
        *changed |= fit(doc, items, needed, shape);
    }
    return (slot_window){ .items = items, .count = (int)yyjson_mut_arr_size(items) };
}

/* The item slot `k` shows, or -1 when it shows none. */
static int slot_item(const slot_window *w, int k)
{
    int i = w->start + ((k - w->start % w->count) + w->count) % w->count;
    return i < w->end ? i : -1;
}

/* The theme's --row on the List, in dp; 0 before the theme applies. */
static float row_height(rml_element *list)
{
    char value[32] = "";
    rml_element_property(list, "--row", value, sizeof value);
    return strtof(value, NULL);
}

/* All kinds show as collapsible groups sorted by name, each asset with how
   often it is used, so unused and unresolved assets stand out. One kind is
   a flat list. */
static bool show_list(
    studio *s,
    const studio_panel_sync *at,
    struct studio_assets_cache *cache,
    rml_element *list
)
{
    bool changed = fit(at->doc, member(at->variable, "lines"), 0, SHAPE_LINE);
    int reveal = at->preview ? -1 : reveal_row(s, cache);
    int count = acount(cache->rows), first = 0;
    int length = count < UNLAID_ROWS ? count : UNLAID_ROWS;
    float row = list ? row_height(list) : 0;
    row = row > 0 ? row : UNLAID_ROW;
    if (list) {
        ui_list window = { .count = count, .row = row, .reveal = reveal };
        length = ui_list_window(list, &window, &cache->window, &first);
    }
    slot_window w = slots(at->doc, member(at->variable, "rows"), length, SHAPE_ITEM, &changed);
    w.start = first;
    w.end = first + length;
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(w.items);
    for (int k = 0; (item = yyjson_mut_arr_iter_next(&it)); k++) {
        int r = slot_item(&w, k);
        changed |= r < 0
                     ? ui_data_put_bool(item, "shown", false)
                     : show_row(s, at->doc, item, cache, cache->rows[r], (float)r * row);
    }
    return changed;
}

/* A tile names its asset in full in its tooltip and says what it is and
   how often it is used. Its thumbnail is asked for once the List shows it
   (`ask`); before, it shows one already made. */
static bool show_tile(
    studio *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *item,
    const struct studio_assets_cache *cache,
    int i,
    bool ask
)
{
    int id = cache->tiles[i];
    const reference_asset *
        asset = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), id);
    bool loading = false;
    const char *image = ask ? studio_thumb(s, id, STUDIO_THUMB_TILE, &loading)
                            : studio_thumb_cached(s, id);
    char meta[24], about[64];
    bool counted = usage(cache, id, asset, meta, sizeof meta);
    snprintf(about, sizeof about, "%s%s%s", KIND_NAMES[asset->kind], counted ? " \xc2\xb7 " : "", meta);
    item_view v = {
        .id = id,
        .kind = (int)asset->kind,
        .label = asset->name,
        .icon = asset_icon(s, asset),
        .badge = image ? KIND_ICONS[asset->kind] : 0,
        .image = image,
        .loading = image && loading,
        .drag = studio_asset_draggable(asset) ? KIND_NAMES[asset->kind] : "",
        .about = about,
        .unresolved = asset->unresolved,
        .marked = is_marked(s, asset),
    };
    return show_item(doc, item, &v);
}

/* The columns of tiles that fit the List as it last laid out. */
static int columns_fit(studio *s, rml_element *list, float tile_width)
{
    float laid = rml_element_box_size(list, RML_BOX_BORDER).x / studio_pixel_ratio(s);
    int fit = (int)((laid - LIST_PAD + TILE_GAP) / (tile_width + TILE_GAP));
    return fit < 1 ? 1 : fit;
}

/* The grid's lines for its columns: with every kind shown, each kind's
   header and then, unless it is folded, its tiles in lines of their own, as
   the list groups its rows (Unity's and Unreal's browsers group by kind);
   one kind is lines of tiles alone. A header is a list row tall, a line a
   tile and its gap. */
static void layout_lines(const studio *s, struct studio_assets_cache *cache, float row)
{
    const reference_index *index = cache->index;
    bool grouped = s->assets_kind < 0;
    int count = acount(cache->tiles), columns = cache->columns;
    float tile = (float)s->prefs.assets_tile + 2 * TILE_PAD + TILE_LABEL + TILE_GAP;
    aclear(cache->lines);
    aclear(cache->tops);
    float top = 0;
    for (int i = 0; i < count;) {
        int kind = (int)reference_index_asset_at(index, cache->tiles[i])->kind;
        int end = i;
        while (end < count &&
               (int)reference_index_asset_at(index, cache->tiles[end])->kind == kind) {
            end++;
        }
        if (grouped) {
            apush(cache->lines, ((grid_line){ kind, i, -1 }));
            apush(cache->tops, top);
            top += row;
        }
        for (int j = i; (!grouped || kind_open(s, kind)) && j < end; j += columns) {
            apush(cache->lines, ((grid_line){ kind, j, end - j < columns ? end - j : columns }));
            apush(cache->tops, top);
            top += tile;
        }
        i = end;
    }
    apush(cache->tops, top);
}

/* Line `l` of the grid: its place in the spacer and its kind's header or
   its tiles. A line keeps its header and tiles' slots, shown or hidden, so
   a scroll that turns one into the other instances nothing. */
static bool show_line(
    studio *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *line,
    const struct studio_assets_cache *cache,
    int l,
    bool ask
)
{
    const grid_line *g = &cache->lines[l];
    char top[32];
    dp_text(top, cache->tops[l]);
    yyjson_mut_val *tiles = member(line, "tiles"), *head = member(line, "head");
    bool changed = fit(doc, tiles, cache->columns, SHAPE_ITEM);
    changed |= ui_data_put_bool(line, "shown", true);
    changed |= ui_data_put_text(doc, line, "top", top);
    changed |= g->count < 0 ? show_header_row(s, doc, head, cache, g->kind, 0)
                            : ui_data_put_bool(head, "shown", false);
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(tiles);
    for (int i = 0; (item = yyjson_mut_arr_iter_next(&it)); i++) {
        changed |= i < g->count ? show_tile(s, doc, item, cache, g->first + i, ask)
                                : ui_data_put_bool(item, "shown", false);
    }
    return changed;
}

/* Unity's grid: tiles of one size in lines as wide as the area. */
static bool show_grid(
    studio *s,
    const studio_panel_sync *at,
    struct studio_assets_cache *cache,
    rml_element *list
)
{
    bool changed = fit(at->doc, member(at->variable, "rows"), 0, SHAPE_ITEM);
    float row = list ? row_height(list) : 0;
    if (list) {
        cache->columns = columns_fit(s, list, (float)s->prefs.assets_tile + 2 * TILE_PAD);
    }
    layout_lines(s, cache, row > 0 ? row : UNLAID_ROW);
    /* The selection's line among those just laid out. */
    int reveal = at->preview ? -1 : reveal_row(s, cache);
    int lines = acount(cache->lines), first = 0;
    int length = lines < UNLAID_LINES ? lines : UNLAID_LINES;
    if (list) {
        ui_list window = { .count = lines,
                           .row = row > 0 ? row : UNLAID_ROW,
                           .tops = cache->tops,
                           .reveal = reveal };
        length = ui_list_window(list, &window, &cache->window, &first);
    }
    slot_window w = slots(at->doc, member(at->variable, "lines"), length, SHAPE_LINE, &changed);
    w.start = first;
    w.end = first + length;
    yyjson_mut_val *line;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(w.items);
    for (int k = 0; (line = yyjson_mut_arr_iter_next(&it)); k++) {
        int l = slot_item(&w, k);
        changed |= l < 0 ? ui_data_put_bool(line, "shown", false)
                         : show_line(s, at->doc, line, cache, l, list);
    }
    return changed;
}

/* A row's menu opens the asset or lists what references it (Find
   References). */
static bool show_menu(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *menu)
{
    const reference_asset *asset = reference_index_asset_at(
        app_scene_authoring_asset_index(s->scene),
        s->assets_menu
    );
    entry_view entries[6];
    int count = 0;
    if (asset) {
        entries[count++] = (entry_view){ .action = "open",
                                         .label = "Open",
                                         .icon = KIT_ICON_EXTERNAL };
        if (studio_asset_nameable(asset, REFERENCE_ASSET_UI)) {
            entries[count++] = (entry_view){ .action = "preview",
                                             .label = "Preview",
                                             .icon = KIT_ICON_LAYOUT };
        }
        entries[count++] = (entry_view){ .action = "references",
                                         .label = "Find References",
                                         .icon = KIT_ICON_REFERENCES };
    }
    if (asset && asset->kind == REFERENCE_ASSET_PROTOTYPE) {
        entries[count++] = (entry_view){
            .action = "spawnBreak",
            .label = "Break When Spawned",
            .checked = app_scene_authoring_spawn_break(s->scene, asset->name),
        };
    }
    int *selected = selected_assets(s);
    char remove[32] = "Delete";
    if (acount(selected) > 1) {
        snprintf(remove, sizeof remove, "Delete %d Assets", acount(selected));
    }
    afree(selected);
    if (studio_asset_refactorable(asset)) {
        entries[count++] = (entry_view){ .action = "rename",
                                         .label = "Rename",
                                         .icon = KIT_ICON_EDIT,
                                         .shortcut = "F2" };
    }
    if (studio_asset_deletable(s, asset)) {
        entries[count++] = (entry_view){ .action = "delete",
                                         .label = remove,
                                         .icon = KIT_ICON_TRASH,
                                         .shortcut = "X",
                                         .separator = true };
    }
    bool changed = ui_data_put_int(menu, "id", asset ? s->assets_menu : -1);
    changed |= ui_data_put_text(doc, menu, "label", asset ? asset->name : "");
    return show_entries(doc, member(menu, "items"), entries, count) || changed;
}

static entry_view new_entry(studio *s, int kind, const char *heading, bool separator)
{
    studio_command command = new_command(kind);
    studio_command_info info = studio_command_describe(s, command);
    return (entry_view){ .id = info.id,
                         .label = info.menu,
                         .shortcut = info.shortcut,
                         .heading = heading,
                         .icon = info.icon,
                         .checked = studio_command_checked(s, command) > 0,
                         .disabled = !studio_command_enabled(s, command),
                         .separator = separator };
}

/* New: every kind in its group; with the filter on a kind, that kind's
   first. */
static bool show_create(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *items)
{
    entry_view entries[STUDIO_NEW_KIND_COUNT];
    int count = 0;
    for (int i = 0; i < STUDIO_NEW_KIND_COUNT; i++) {
        if ((int)NEW_KINDS[i].kind == s->assets_kind) {
            entries[count++] = new_entry(s, i, NULL, false);
        }
    }
    bool primary = count > 0;
    const char *group = NULL;
    for (int i = 0; i < STUDIO_NEW_KIND_COUNT; i++) {
        if ((int)NEW_KINDS[i].kind == s->assets_kind) {
            continue;
        }
        bool heads = !group || strcmp(group, NEW_KINDS[i].group);
        group = NEW_KINDS[i].group;
        entries[count] = new_entry(s, i, heads ? group : NULL, primary);
        primary = false;
        count++;
    }
    return show_entries(doc, items, entries, count);
}

bool studio_assets_sync(studio *s, const studio_panel_sync *at)
{
    struct studio_assets_cache *cache = cache_of(s);
    bool changed = take_filter(s, at, cache);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    asset_rows(s, index);
    rml_element *list = at->preview ? NULL : element_by_id(s, LIST_ID);
    /* Before the List lays out, its rows are a guess: window them once it
       has. */
    s->ui_schedule.wake |= !at->preview && !list;
    changed |= show_header(s, at->doc, at->variable);
    changed |= grid_view(s) ? show_grid(s, at, cache, list)
                            : show_list(s, at, cache, list);
    /* A project always has its start scene, so only filters empty the view. */
    bool filtered = s->assets_filter[0] || s->assets_kind >= 0;
    const char *empty = filtered && !acount(cache->tiles) ? "No assets match" : "";
    return ui_data_put_text(at->doc, at->variable, "empty", empty) || changed;
}

/* ---------- The `asset_view` source: what changes apart from the rows ---------- */

/* asset_view's id for no asset, which no row has: a group header's is -1. */
#define NO_ASSET -2

void studio_asset_view_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *view = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "asset_view");
    yyjson_mut_obj_add_int(doc, view, "selected", NO_ASSET);
    yyjson_mut_val *preview = yyjson_mut_obj_add_obj(doc, view, "preview");
    yyjson_mut_obj_add_int(doc, preview, "id", NO_ASSET);
    yyjson_mut_obj_add_strcpy(doc, preview, "image", "");
}

/* The large preview of the tile the pointer rests on, or its tile's
   picture until the large one is ready. */
static bool show_preview(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *preview, int rested)
{
    bool loading;
    const char *image = rested >= 0
                          ? studio_thumb(s, rested, STUDIO_THUMB_LARGE, &loading)
                          : NULL;
    image = image || rested < 0
              ? image
              : studio_thumb(s, rested, STUDIO_THUMB_TILE, &loading);
    bool changed = ui_data_put_int(preview, "id", rested < 0 ? NO_ASSET : rested);
    return ui_data_put_text(doc, preview, "image", or_empty(image)) || changed;
}

bool studio_asset_view_sync(studio *s, const studio_panel_sync *at)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int selected = index && s->assets_selected[0]
                     ? reference_index_find_name(
                           index,
                           (reference_asset_kind)s->assets_selected_kind,
                           s->assets_selected
                       )
                     : -1;
    if (!at->preview) {
        s->assets_hover = hovered_asset(s);
    }
    bool changed = ui_data_put_int(at->variable, "selected", selected < 0 ? NO_ASSET : selected);
    return show_preview(s, at->doc, member(at->variable, "preview"), at->preview ? -1 : resting(s)) ||
           changed;
}

/* Queues the asset menu of the area the press was in, at the press, for
   once its source shows the asset. */
static void queue_menu(studio *s, const ui_activation *a)
{
    rml_element *row = rml_element_ref_get(a->anchor);
    rml_element *menu = row ? rml_element_by_id(rml_element_owner_document(row), MENU_ID)
                            : NULL;
    if (!menu) {
        return;
    }
    struct studio_assets_cache *cache = cache_of(s);
    rml_element_ref_destroy(cache->menu);
    cache->menu = rml_element_ref_create(menu);
    cache->menu_x = a->x;
    cache->menu_y = a->y;
}

static void open_menu(studio *s)
{
    struct studio_assets_cache *cache = s->assets_cache;
    rml_element *menu = cache && cache->menu ? rml_element_ref_get(cache->menu) : NULL;
    if (menu) {
        popover_show(
            menu,
            &(popover_anchor){ .side = POPOVER_POINT,
                               .box = { cache->menu_x, cache->menu_y, 0, 0 } }
        );
    }
    if (cache) {
        rml_element_ref_destroy(cache->menu);
        cache->menu = NULL;
    }
}

/* A click selects an asset, Shift's extends the selection to it and Ctrl's
   (Cmd's on a Mac) toggles it; a click on a kind's header folds its group. */
static void click(studio *s, const ui_activation *a, const reference_asset *asset, int kind)
{
    const struct studio_assets_cache *cache = current_cache(s);
    if (asset && (a->modifiers & RML_KM_SHIFT) && cache) {
        int *shown = shown_assets(s, cache);
        studio_asset_ref clicked = ref_of(asset);
        int at = position_of(cache->index, shown, &clicked);
        if (at >= 0) {
            extend_to(s, cache->index, shown, at);
        }
        afree(shown);
        return;
    }
    if (asset && (a->modifiers & (s->mac ? RML_KM_META : RML_KM_CTRL))) {
        toggle_asset(s, asset);
        return;
    }
    if (asset) {
        select_asset(s, asset);
        return;
    }
    if (kind >= 0 && kind < REFERENCE_ASSET_KIND_COUNT) {
        s->assets_collapsed ^= 1u << kind;
    }
}

/* What the panel's rows, tiles, menus and header controls ask for:
 * click (asset, kind), open, menu, preview, references, spawnBreak, rename
 * and delete (asset), kind (-1 for all), tile (the size), view (0 for the
 * grid, 1 for the list), clearFilter and scroll. */
void studio_assets_action(studio *s, const char *action, const ui_activation *a)
{
    int id = a->argc ? atoi(a->args[0]) : -1;
    const reference_asset *
        asset = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), id);
    if (!strcmp(action, "kind")) {
        s->assets_kind = id >= -1 && id < REFERENCE_ASSET_KIND_COUNT ? id : -1;
    } else if (!strcmp(action, "tile") && a->has_value) {
        int tile = (int)lroundf(a->value);
        s->prefs.assets_tile = tile < STUDIO_ASSETS_TILE_MIN ? STUDIO_ASSETS_TILE_MIN
                             : tile > STUDIO_ASSETS_TILE_MAX ? STUDIO_ASSETS_TILE_MAX
                                                             : tile;
    } else if (!strcmp(action, "view")) {
        s->prefs.assets_list = id == 1;
        s->assets_reveal = true;
    } else if (!strcmp(action, "click")) {
        click(s, a, asset, a->argc > 1 ? atoi(a->args[1]) : -1);
    } else if (!strcmp(action, "clearFilter")) {
        s->assets_filter[0] = '\0';
        s->assets_kind = -1;
    }
    if (!asset) {
        return;
    }
    if (!strcmp(action, "open")) {
        open_asset(s, id);
    } else if (!strcmp(action, "menu")) {
        /* A right press keeps a selection it lands in, as Explorer's does. */
        if (!is_selected(s, asset) && !is_marked(s, asset)) {
            select_asset(s, asset);
        }
        s->assets_menu = id;
        queue_menu(s, a);
    } else if (!strcmp(action, "preview")) {
        studio_preview_ui(s, asset->name);
    } else if (!strcmp(action, "references")) {
        studio_find_asset_references(s, id);
    } else if (!strcmp(action, "spawnBreak")) {
        studio_toggle_spawn_break(s, asset->name);
    } else if (!strcmp(action, "rename")) {
        studio_refactor_rename_asset(s, id);
    } else if (!strcmp(action, "delete")) {
        delete_selection(s);
    }
}

void studio_assets_header(studio *s, int area)
{
    /* Visual designer (workstream H): a UI document's Library. */
    if (studio_designer_active(s)) {
        studio_designer_library_header(s, area);
        return;
    }
    rml_element *host = studio_ui_area_controls(s, area, "assets", "AssetsHeader");
    rml_element *field = host ? rml_element_query_selector(host, "#assets-filter")
                              : NULL;
    /* Find (Mod+F) types in the filter. */
    if (field && s->find_request == STUDIO_EDITOR_ASSETS + 1) {
        rml_element_focus(field, true);
        rml_element_select(field);
        s->find_request = 0;
    }
}

void studio_assets_body(studio *s, int area)
{
    /* Visual designer (workstream H): a UI document's Library. */
    if (studio_designer_active(s)) {
        studio_designer_library(s, area);
        return;
    }
    studio_ui_area(s, area, "Assets");
    open_menu(s);
}

/* ---------- The `asset_menus` source: menus and NewAsset ---------- */

void studio_asset_menus_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *menus = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "asset_menus");
    yyjson_mut_obj_add_arr(doc, menus, "create");
    yyjson_mut_val *row = yyjson_mut_obj_add_obj(doc, menus, "row");
    yyjson_mut_obj_add_int(doc, row, "id", -1);
    yyjson_mut_obj_add_strcpy(doc, row, "label", "");
    yyjson_mut_obj_add_arr(doc, row, "items");
    yyjson_mut_val *naming = yyjson_mut_obj_add_obj(doc, menus, "naming");
    add_texts(doc, naming, NAMING_TEXTS, COUNT(NAMING_TEXTS));
    yyjson_mut_val *button = yyjson_mut_obj_add_obj(doc, naming, "button");
    char icon[5];
    studio_glyph(icon, KIT_ICON_ADD);
    yyjson_mut_obj_add_str(doc, button, "label", "Create");
    yyjson_mut_obj_add_strcpy(doc, button, "icon", icon);
    yyjson_mut_obj_add_str(doc, button, "shortcut", "");
    yyjson_mut_obj_add_bool(doc, button, "checked", false);
    yyjson_mut_obj_add_bool(doc, button, "disabled", false);
}

/* Takes the name the field typed, then shows the kind, the name and why the
   last one was refused. */
static bool show_naming(studio *s, const studio_panel_sync *at, yyjson_mut_val *naming)
{
    studio_new_asset *n = &s->new_asset;
    yyjson_mut_val *name = member(naming, "name");
    const char *shown = yyjson_mut_get_str(name);
    if (!at->preview && strcmp(shown, n->typed)) {
        snprintf(n->name, sizeof n->name, "%s", shown);
        snprintf(n->typed, sizeof n->typed, "%s", shown);
    }
    char title[64];
    snprintf(
        title,
        sizeof title,
        "New %s",
        studio_command_describe(s, new_command(n->kind)).menu
    );
    bool changed = ui_data_put_text(at->doc, naming, "label", title);
    changed |= ui_data_put_text(at->doc, naming, "error", n->error);
    if (strcmp(shown, n->name)) {
        ui_data_set_text(at->doc, name, n->name);
        changed = true;
    }
    if (!at->preview) {
        snprintf(n->typed, sizeof n->typed, "%s", n->name);
        n->asking = n->asking == STUDIO_NAME_ASKED ? STUDIO_NAME_SHOWN : n->asking;
    }
    return changed;
}

bool studio_asset_menus_sync(studio *s, const studio_panel_sync *at)
{
    bool changed = show_create(s, at->doc, member(at->variable, "create"));
    changed |= show_menu(s, at->doc, member(at->variable, "row"));
    return show_naming(s, at, member(at->variable, "naming")) || changed;
}

/* Create, or Enter in the name, makes the asset, or says why not and asks
   again, even after Create closed the popover; the name field sends select
   as it takes the focus. What the field sent selects its name, so typing
   replaces it. */
void studio_asset_menus_action(studio *s, const char *action, const ui_activation *a)
{
    studio_new_asset *n = &s->new_asset;
    rml_element *sender = rml_element_ref_get(a->anchor);
    if (!strcmp(action, "create")) {
        if (create(s)) {
            n->closing = true;
            return;
        }
        ask_next(s);
    }
    if (sender && !strcmp(rml_element_tag_name(sender), "input")) {
        rml_element_select(sender);
    }
}

/* Under Assets' New button, or at the pointer when no Assets header shows. */
static void ask(studio *s, rml_element *popover)
{
    rml_element *button = element_by_id(s, NEW_BUTTON_ID);
    float r = studio_pixel_ratio(s);
    popover_anchor at = { .side = POPOVER_POINT,
                          .box = { s->in.mouse_x * r, s->in.mouse_y * r, 0, 0 } };
    if (button) {
        at = (popover_anchor){ .side = POPOVER_BELOW, .element = button, .trigger = button };
    }
    popover_show(popover, &at);
}

static bool within(rml_element *el, const rml_element *ancestor)
{
    for (; el; el = rml_element_parent(el)) {
        if (el == ancestor) {
            return true;
        }
    }
    return false;
}

/* Closes the popover, its field giving up the focus, so keys reach Studio
   again and the next popover's field takes the focus anew. */
static void close_name(studio *s, rml_element *popover)
{
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    if (focus && within(focus, popover)) {
        rml_element_blur(focus);
    }
    popover_hide(popover);
}

void studio_new_asset_frame(studio *s)
{
    studio_new_asset *n = &s->new_asset;
    /* Its document lives while it asks, from a New command until the
       popover closes, and costs nothing otherwise. */
    if (n->asking == STUDIO_NAME_IDLE && !n->open) {
        return;
    }
    kit *k = s->kit;
    /* A document of its own, as tall as nothing, whose popover shows over
       every area. */
    kit_document_begin(k, "new-asset", "bar-place", true);
    rml_element *host = studio_ui_panel(s, "new-asset", "NewAsset");
    kit_document_end(k);
    rml_element *popover = host ? rml_element_query_selector(host, "#" NAME_ID) : NULL;
    if (!popover) {
        return;
    }
    if (n->closing) {
        close_name(s, popover);
        n->closing = false;
    }
    if (n->asking == STUDIO_NAME_SHOWN) {
        if (!popover_open(popover)) {
            ask(s, popover);
        }
        n->asking = STUDIO_NAME_IDLE;
    }
    n->open = popover_open(popover);
}
