/* The Behavior header (projects/studio's BehaviorHeader.rml), after Unreal's
 * graph editor toolbar: Back and Forward, breadcrumbs of the behavior and
 * the calls entered to reach the open graph, whose crumb picks or renames a
 * graph; while the game runs, the debug target, which instance the graph
 * shows (Unreal's debug object); the zoom, the Graph menu, Arrange, Find,
 * Frame all and the View menu (GraphView). Four sources feed it, by how
 * often they change: `graph_header` (the crumbs and buttons),
 * `graph_menus` (the picker's graphs and the assets of the behavior's
 * kind), `graph_view` (the zoom and what the selection allows) and
 * `graph_targets` (the debug target's choices). Its actions are
 * graph_header.<action>. */
#include "assets/reference_adapters.h"
#include "editor/studio/studio_graph_internal.h"
#include "support/array.h"
#include "ui/popover.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Items ---------- */

/* An item as MenuItem shows it, or a crumb or a choice: `name` is the
   member of its variable that holds it, when one does. */
typedef struct
{
    const char *name, *id, *label, *shortcut, *keywords;
    uint32_t icon;
    bool disabled, current;
} header_item;

static const char *const ITEM_TEXTS[] = { "id",       "icon",     "label",
                                          "shortcut", "keywords", NULL };
static const char *const ITEM_FLAGS[] = { "checked", "disabled", "current", NULL };

typedef enum
{
    SHAPE_ITEM,
    SHAPE_GROUP, /* a label over an array of items, `options` */
} item_shape;

static yyjson_mut_val *item_new(yyjson_mut_doc *doc, item_shape shape)
{
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    if (shape == SHAPE_GROUP) {
        yyjson_mut_obj_add_strcpy(doc, item, "label", "");
        yyjson_mut_obj_add_arr(doc, item, "options");
        return item;
    }
    for (const char *const *text = ITEM_TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, item, *text, "");
    }
    for (const char *const *flag = ITEM_FLAGS; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, item, *flag, false);
    }
    return item;
}

static void add_items(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *const *names)
{
    for (; *names; names++) {
        yyjson_mut_obj_add_val(doc, object, *names, item_new(doc, SHAPE_ITEM));
    }
}

static bool put_glyph(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *name, uint32_t icon)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    return ui_data_put_text(doc, object, name, glyph);
}

static bool put_item(yyjson_mut_doc *doc, yyjson_mut_val *item, const header_item *shown)
{
    bool changed = ui_data_put_text(doc, item, "id", shown->id ? shown->id : "");
    changed |= ui_data_put_text(doc, item, "label", shown->label);
    changed |= put_glyph(doc, item, "icon", shown->icon);
    changed |= ui_data_put_text(doc, item, "shortcut", shown->shortcut ? shown->shortcut : "");
    changed |= ui_data_put_text(doc, item, "keywords", shown->keywords ? shown->keywords : "");
    changed |= ui_data_put_bool(item, "disabled", shown->disabled);
    return ui_data_put_bool(item, "current", shown->current) || changed;
}

/* Puts each item in the member of `object` it names. */
static bool put_named(yyjson_mut_doc *doc, yyjson_mut_val *object, const header_item *items, size_t count)
{
    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        changed |= put_item(doc, yyjson_mut_obj_get(object, items[i].name), &items[i]);
    }
    return changed;
}

/* Gives `array` `length` objects of `shape`, adding and removing at its
   end, so items already shown keep their elements; true when its length
   changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *array, int length, item_shape shape)
{
    int had = (int)yyjson_mut_arr_size(array);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(array, item_new(doc, shape));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(array);
    }
    return had != length;
}

static uint32_t graph_icon(const editor *e, const graph *gr)
{
    if (editor_presentation(e) && gr == e->asset->graph) {
        return KIT_ICON_VARIABLE;
    }
    return gr->kind == GRAPH_FUNCTION ? KIT_ICON_FUNCTION
         : gr->kind == GRAPH_MACRO    ? KIT_ICON_EXPRESSION
                                      : KIT_ICON_EVENT;
}

/* The behavior's kind among the assets, for its crumb and the assets the
   crumb switches to. */
static reference_asset_kind document_kind(const studio *s)
{
    return s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM
             ? REFERENCE_ASSET_SYSTEM
             : reference_domain_kind(sg_graph_editor(s)->asset->domain);
}

/* ---------- The debug target ---------- */

static const char *actor_name(const app_scene_authoring *scene, ecs_entity entity)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            return scene->actors[i].name;
        }
    }
    return NULL;
}

/* An instance as the debug target menu names it: its object's name, or its
   prototype's and its entity's slot when the game spawned it. A destroyed
   object's prototype is the one the target kept. */
static void instance_label(
    app_scene_authoring *scene,
    ecs_entity entity,
    const char *prototype,
    char *buf,
    size_t size
)
{
    const char *name = actor_name(scene, entity);
    if (name) {
        snprintf(buf, size, "%s", name);
        return;
    }
    snprintf(
        buf,
        size,
        "%s %u",
        prototype ? prototype : runtime_entity_label(scene, entity),
        (unsigned)entity.slot
    );
}

static bool targeted(const app_scene_authoring *scene)
{
    return scene->debug_status == APP_DEBUG_AVAILABLE ||
           scene->debug_status == APP_DEBUG_STALE;
}

/* What the target button adds to the instance's name: a destroyed object's
   frozen data, a stale definition, or Follow newest. */
static const char *target_state(const app_scene_authoring *scene)
{
    if (scene->debug_destroyed) {
        return " \xc2\xb7 Destroyed";
    }
    if (scene->debug_status == APP_DEBUG_STALE) {
        return " \xc2\xb7 Edited";
    }
    return scene->debug_newest ? " \xc2\xb7 Newest" : "";
}

static const char *target_description(const app_scene_authoring *scene)
{
    if (scene->debug_destroyed) {
        return "Its object was destroyed. The graph shows what it last did "
               "until another target is chosen.";
    }
    if (scene->debug_status == APP_DEBUG_STALE) {
        return "It runs the saved behavior. Save to run this graph.";
    }
    return scene->debug_newest
             ? "Follows the newest one the game spawned; choose an instance to "
               "stay on one."
             : "The running instance whose wires, nodes and values this graph "
               "shows. Selecting an object shows its instance.";
}

/* The target button's label: the instance, a newest one awaited, or none. */
static void target_label(app_scene_authoring *scene, char *label, size_t size)
{
    snprintf(label, size, "No target");
    if (targeted(scene)) {
        char name[96];
        instance_label(scene, scene->debug.entity, scene->debug.prototype, name, sizeof name);
        snprintf(label, size, "%s%s", name, target_state(scene));
    } else if (scene->debug_newest) {
        snprintf(label, size, "Newest %s", scene->debug_newest);
    }
}

/* Whether the target shows: while the scene or a preview runs a behavior. */
static bool shows_target(const studio *s)
{
    return studio_session(s) &&
           s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_BEHAVIOR;
}

/* ---------- graph_header ---------- */

void studio_graph_header_add(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "backKbd",    "forwardKbd",
                                         "target",     "targetAbout",
                                         "arrangeKbd", "readonlyAbout",
                                         NULL };
    static const char *const FLAGS[] = { "open",     "back",       "forward",
                                         "renaming", "debug",      "readonly",
                                         "find",     "sourceView", NULL };
    yyjson_mut_val *h = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "graph_header");
    yyjson_mut_obj_add_int(doc, h, "view", 0);
    for (const char *const *text = TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, h, *text, "");
    }
    for (const char *const *flag = FLAGS; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, h, *flag, false);
    }
    yyjson_mut_val *asset = yyjson_mut_obj_add_obj(doc, h, "asset");
    yyjson_mut_obj_add_strcpy(doc, asset, "label", "");
    yyjson_mut_obj_add_strcpy(doc, asset, "icon", "");
    yyjson_mut_val *shown = yyjson_mut_obj_add_obj(doc, h, "graph");
    yyjson_mut_obj_add_strcpy(doc, shown, "label", "");
    yyjson_mut_obj_add_strcpy(doc, shown, "icon", "");
    yyjson_mut_obj_add_arr(doc, h, "callers");
}

/* The calls entered to reach the open graph, each returning to its graph:
   its id is its index among them. */
static bool show_callers(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *array)
{
    editor *e = sg_graph_editor(s);
    int count = 0;
    for (int i = 0; i < editor_graph_history_count(e); i++) {
        count += behavior_asset_find_graph(e->asset, e->graph_callers[i].graph_id) != NULL;
    }
    bool changed = fit(doc, array, count, SHAPE_ITEM);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = 0; i < editor_graph_history_count(e); i++) {
        const graph
            *caller = behavior_asset_find_graph(e->asset, e->graph_callers[i].graph_id);
        if (caller) {
            char id[16];
            snprintf(id, sizeof id, "%d", i);
            char label[256];
            studio_code_graph_label(s, caller, label, sizeof label);
            header_item shown = { .id = id, .label = label };
            changed |= put_item(doc, yyjson_mut_arr_iter_next(&it), &shown);
        }
    }
    return changed;
}

static bool show_crumbs(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *h)
{
    const graph *gr = sg_active_graph(s);
    yyjson_mut_val *asset = yyjson_mut_obj_get(h, "asset");
    yyjson_mut_val *shown = yyjson_mut_obj_get(h, "graph");
    bool changed = ui_data_put_text(doc, asset, "label", s->scene->graph_document_id);
    changed |= put_glyph(doc, asset, "icon", studio_asset_kind_icon(document_kind(s)));
    char label[256];
    studio_code_graph_label(s, gr, label, sizeof label);
    changed |= ui_data_put_text(doc, shown, "label", label);
    changed |= put_glyph(doc, shown, "icon", graph_icon(sg_graph_editor(s), gr));
    return show_callers(s, doc, yyjson_mut_obj_get(h, "callers")) || changed;
}

static bool show_target(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *h)
{
    bool debug = shows_target(s);
    char label[128] = "";
    if (debug) {
        target_label(s->scene, label, sizeof label);
    }
    bool changed = ui_data_put_bool(h, "debug", debug);
    changed |= ui_data_put_text(doc, h, "target", label);
    return ui_data_put_text(doc, h, "targetAbout", debug ? target_description(s->scene) : "") ||
           changed;
}

/* The rename ends when its graph is no longer the open one. */
static bool renaming(const studio *s)
{
    const studio_graph *g = &s->graph;
    const graph *gr = sg_active_graph(s);
    return g->renaming && gr && g->rename_graph == gr->graph_id;
}

bool studio_graph_header_sync(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *h = at->variable;
    bool open = studio_graph_active(s);
    bool changed = ui_data_put_bool(h, "open", open);
    changed |= ui_data_put_int(h, "view", studio_behavior_view(s));
    changed |= ui_data_put_bool(
        h,
        "sourceView",
        s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
            s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM
    );
    if (!open) {
        return changed;
    }
    sg_nav_track(s);
    changed |= ui_data_put_bool(h, "back", studio_nav_can_step(s, -1));
    changed |= ui_data_put_bool(h, "forward", studio_nav_can_step(s, 1));
    changed |= ui_data_put_text(at->doc, h, "backKbd", studio_chord(s, "Alt+\xe2\x86\x90"));
    changed |= ui_data_put_text(at->doc, h, "forwardKbd", studio_chord(s, "Alt+\xe2\x86\x92"));
    changed |= ui_data_put_text(at->doc, h, "arrangeKbd", studio_chord(s, "Shift+L"));
    changed |= ui_data_put_bool(h, "readonly", sg_graph_readonly(s));
    const char *reason = sg_readonly_reason(s);
    changed |= ui_data_put_text(at->doc, h, "readonlyAbout", reason ? reason : "");
    changed |= ui_data_put_bool(h, "find", s->graph.find.open);
    s->graph.renaming = renaming(s);
    changed |= ui_data_put_bool(h, "renaming", s->graph.renaming);
    changed |= show_crumbs(s, at->doc, h);
    return show_target(s, at->doc, h) || changed;
}

/* ---------- graph_menus ---------- */

void studio_graph_menus_add(yyjson_mut_doc *doc)
{
    static const char *const ITEMS[] = { "refs",   "newFunction", "newMacro",
                                         "rename", "graphRefs",   "graphCalls",
                                         "remove", NULL };
    yyjson_mut_val *m = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "graph_menus");
    add_items(doc, m, ITEMS);
    yyjson_mut_val *kind = yyjson_mut_obj_add_obj(doc, m, "kind");
    yyjson_mut_obj_add_strcpy(doc, kind, "label", "");
    yyjson_mut_obj_add_strcpy(doc, kind, "icon", "");
    yyjson_mut_obj_add_bool(doc, m, "kinds", false);
    yyjson_mut_obj_add_bool(doc, m, "named", false);
    yyjson_mut_obj_add_arr(doc, m, "assets");
    yyjson_mut_obj_add_arr(doc, m, "graphs");
}

/* The other assets of the behavior's kind, to switch to (the prototype's
   All Behaviors). A System has none. */
static bool show_assets(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *m)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    reference_asset_kind kind = document_kind(s);
    bool listed = kind != REFERENCE_ASSET_SYSTEM;
    yyjson_mut_val *shown = yyjson_mut_obj_get(m, "kind");
    bool changed = ui_data_put_bool(m, "kinds", listed);
    changed |= ui_data_put_text(doc, shown, "label", studio_asset_kind_label(kind));
    changed |= put_glyph(doc, shown, "icon", studio_asset_kind_icon(kind));
    int count = 0;
    for (int i = 0; listed && index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        count += asset->kind == kind && !asset->unresolved;
    }
    yyjson_mut_val *array = yyjson_mut_obj_get(m, "assets");
    changed |= fit(doc, array, count, SHAPE_ITEM);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = 0; count && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (asset->kind != kind || asset->unresolved) {
            continue;
        }
        header_item item = { .label = asset->name,
                             .icon = studio_asset_kind_icon(kind),
                             .current = !strcmp(asset->name, s->scene->graph_document_id) };
        changed |= put_item(doc, yyjson_mut_arr_iter_next(&it), &item);
    }
    return changed;
}

enum
{
    PICKER_EVENTS,
    PICKER_VARIABLES,
    PICKER_FUNCTIONS,
    PICKER_MACROS,
    PICKER_GROUPS,
};

static int graph_group(const editor *e, const graph *g)
{
    if (editor_presentation(e) && g == e->asset->graph) {
        return PICKER_VARIABLES;
    }
    return g->kind == GRAPH_FUNCTION ? PICKER_FUNCTIONS
         : g->kind == GRAPH_MACRO    ? PICKER_MACROS
                                     : PICKER_EVENTS;
}

/* Events stay ahead of helpers regardless of declaration order. Generated
   expression graphs belong to their node; only list one while it is open. */
static bool show_graphs(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *array)
{
    editor *e = sg_graph_editor(s);
    const graph *gr = sg_active_graph(s);
    static const char *const labels[PICKER_GROUPS] = { "Events",
                                                       "Variables",
                                                       "Functions",
                                                       "Macros" };
    int counts[PICKER_GROUPS] = { 0 }, groups = 0;
    for (int i = 0; i < acount(e->asset->graphs); i++) {
        const graph *g = e->asset->graphs[i];
        if (!g->math_expression || g == gr) {
            counts[graph_group(e, g)]++;
        }
    }
    for (int i = 0; i < PICKER_GROUPS; i++) {
        groups += counts[i] != 0;
    }
    bool changed = fit(doc, array, groups, SHAPE_GROUP);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int group = 0; group < PICKER_GROUPS; group++) {
        if (!counts[group]) {
            continue;
        }
        yyjson_mut_val *section = yyjson_mut_arr_iter_next(&it);
        changed |= ui_data_put_text(doc, section, "label", labels[group]);
        yyjson_mut_val *options = yyjson_mut_obj_get(section, "options");
        changed |= fit(doc, options, counts[group], SHAPE_ITEM);
        yyjson_mut_arr_iter choices = yyjson_mut_arr_iter_with(options);
        for (int i = 0; i < acount(e->asset->graphs); i++) {
            const graph *item = e->asset->graphs[i];
            if ((item->math_expression && item != gr) || graph_group(e, item) != group) {
                continue;
            }
            char id[24], label[256];
            snprintf(id, sizeof id, "%llu", (unsigned long long)item->graph_id);
            studio_code_graph_label(s, item, label, sizeof label);
            header_item shown = { .id = id,
                                  .label = label,
                                  .icon = graph_icon(e, item),
                                  .current = item == gr };
            changed |= put_item(doc, yyjson_mut_arr_iter_next(&choices), &shown);
        }
    }
    return changed;
}

/* New Function and New Macro, and for a function or macro, which are named
   where the event graph is the behavior, Rename, its references, its
   callers and Delete. */
static bool show_graph_items(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *m)
{
    const graph *gr = sg_active_graph(s);
    bool locked = !behavior_asset_mutable(sg_graph_editor(s)->asset);
    const char *remove = gr->kind == GRAPH_MACRO ? "Delete Macro" : "Delete Function";
    const header_item items[] = {
        { .name = "refs", .label = "Find References", .icon = KIT_ICON_REFERENCES },
        { .name = "newFunction", .label = "New Function", .icon = KIT_ICON_ADD, .disabled = locked },
        { .name = "newMacro", .label = "New Macro", .icon = KIT_ICON_ADD, .disabled = locked },
        { .name = "rename", .label = "Rename", .icon = KIT_ICON_EDIT, .disabled = locked },
        { .name = "graphRefs", .label = "Find References", .icon = KIT_ICON_REFERENCES },
        { .name = "graphCalls", .label = "Call Hierarchy", .icon = KIT_ICON_CALLERS },
        { .name = "remove", .label = remove, .icon = KIT_ICON_TRASH, .disabled = locked },
    };
    bool changed = ui_data_put_bool(m, "named", gr->kind == GRAPH_FUNCTION || gr->kind == GRAPH_MACRO);
    return put_named(doc, m, items, sizeof items / sizeof *items) || changed;
}

/* What the menus show follows the asset index, the behavior's edits, the
   open graph and whether the asset takes edits; a host's model is its own. */
static uint64_t menus_key(const studio *s, const studio_panel_sync *at)
{
    const editor *e = sg_graph_editor(s);
    const uint64_t parts[] = { s->scene->asset_index_generation,
                               editor_edit_revision(e),
                               sg_active_graph(s)->graph_id,
                               (uintptr_t)e->asset,
                               (uintptr_t)at->doc,
                               behavior_asset_mutable(e->asset) };
    uint64_t key = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        key = (key ^ parts[i]) * 1099511628211ull;
    }
    return key;
}

bool studio_graph_menus_sync(studio *s, const studio_panel_sync *at)
{
    if (!studio_graph_active(s)) {
        return false;
    }
    uint64_t key = menus_key(s, at);
    if (key == s->graph.menus_key) {
        return false;
    }
    s->graph.menus_key = key;
    bool changed = show_assets(s, at->doc, at->variable);
    changed |= show_graphs(s, at->doc, yyjson_mut_obj_get(at->variable, "graphs"));
    return show_graph_items(s, at->doc, at->variable) || changed;
}

/* ---------- graph_view ---------- */

/* Alignment for two or more selected nodes, and straightening (Unreal). */
static const struct
{
    const char *id, *label;
    uint32_t icon;
    align_edge edge;
} ALIGN[] = {
    { "top", "Align Top", KIT_ICON_ALIGN_TOP, ALIGN_TOP },
    { "middle", "Align Middle", KIT_ICON_ALIGN_MIDDLE, ALIGN_MIDDLE },
    { "bottom", "Align Bottom", KIT_ICON_ALIGN_BOTTOM, ALIGN_BOTTOM },
    { "left", "Align Left", KIT_ICON_ALIGN_LEFT, ALIGN_LEFT },
    { "center", "Align Center", KIT_ICON_ALIGN_CENTER, ALIGN_CENTER },
    { "right", "Align Right", KIT_ICON_ALIGN_RIGHT, ALIGN_RIGHT },
};
enum
{
    ALIGN_COUNT = sizeof ALIGN / sizeof *ALIGN
};

void studio_graph_view_add(yyjson_mut_doc *doc)
{
    static const char *const ITEMS[] = { "optimize", "collapse",   "horizontal",
                                         "vertical", "straighten", NULL };
    yyjson_mut_val *v = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "graph_view");
    yyjson_mut_obj_add_real(doc, v, "zoom", 0);
    yyjson_mut_obj_add_bool(doc, v, "zoomShown", false);
    yyjson_mut_obj_add_bool(doc, v, "zooming", false);
    yyjson_mut_obj_add_bool(doc, v, "align", false);
    add_items(doc, v, ITEMS);
    yyjson_mut_val *alignment = yyjson_mut_obj_add_obj(doc, v, "alignment");
    yyjson_mut_obj_add_strcpy(doc, alignment, "label", "Alignment");
    char glyph[5];
    studio_glyph(glyph, KIT_ICON_LAYOUT);
    yyjson_mut_obj_add_strcpy(doc, alignment, "icon", glyph);
    yyjson_mut_val *edges = yyjson_mut_obj_add_arr(doc, v, "edges");
    for (int i = 0; i < ALIGN_COUNT; i++) {
        yyjson_mut_arr_append(edges, item_new(doc, SHAPE_ITEM));
    }
}

/* The Graph menu's items, which the selection enables. */
static bool show_actions(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *v)
{
    bool readonly = sg_graph_readonly(s);
    bool expression = readonly || sg_active_graph(s)->math_expression;
    int count = acount(sg_graph_editor(s)->selected);
    const header_item items[] = {
        { .name = "optimize", .label = "Optimize", .icon = KIT_ICON_EXPRESSION, .disabled = expression },
        { .name = "collapse",
          .label = "Math to Expressions",
          .icon = KIT_ICON_EXPRESSION,
          .disabled = expression },
        { .name = "horizontal",
          .label = "Distribute Horizontally",
          .icon = KIT_ICON_DISTRIBUTE_X,
          .disabled = readonly || count < 3 },
        { .name = "vertical",
          .label = "Distribute Vertically",
          .icon = KIT_ICON_DISTRIBUTE_Y,
          .disabled = readonly || count < 3 },
        { .name = "straighten",
          .label = "Straighten Connections",
          .icon = KIT_ICON_MOVE,
          .shortcut = "Q",
          .disabled = readonly || !count },
    };
    bool changed = ui_data_put_bool(v, "align", count > 1);
    changed |= put_named(doc, v, items, sizeof items / sizeof *items);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(yyjson_mut_obj_get(v, "edges"));
    for (int i = 0; i < ALIGN_COUNT; i++) {
        header_item edge = { .id = ALIGN[i].id,
                             .label = ALIGN[i].label,
                             .icon = ALIGN[i].icon,
                             .disabled = readonly };
        changed |= put_item(doc, yyjson_mut_arr_iter_next(&it), &edge);
    }
    return changed;
}

bool studio_graph_view_sync(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *v = at->variable;
    bool shown = studio_graph_active(s) && s->graph.kit && s->graph.surface.rect.w > 0;
    bool changed = ui_data_put_bool(v, "zoomShown", shown);
    if (shown) {
        changed |= ui_data_put_number(v, "zoom", roundf(sg_zoom_readout(s) * 100));
        changed |= ui_data_put_bool(v, "zooming", sg_wheel_zooming(s));
    }
    return studio_graph_active(s) ? show_actions(s, at->doc, v) || changed : changed;
}

/* ---------- graph_targets ---------- */

/* A choice of the target picker: its value names what choosing does. */
typedef struct
{
    char value[64];
    char label[96];
    const char *keywords;
    uint32_t icon;
    bool current;
} target_choice;

typedef struct
{
    const char *label;
    target_choice *choices;
} target_group;

enum
{
    GROUP_NONE,
    GROUP_NEWEST,
    GROUP_BREAK,
    GROUP_SCENE,
    GROUP_SPAWNED,
    GROUP_COUNT,
};

static const char *const GROUP_LABELS[GROUP_COUNT] = { "",
                                                       "Follow newest",
                                                       "Break when spawned",
                                                       "Scene",
                                                       "Spawned" };

void studio_graph_targets_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *t = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "graph_targets");
    yyjson_mut_obj_add_arr(doc, t, "groups");
}

static bool prototype_attaches(const prototype *prototype, const char *behavior)
{
    for (int i = 0; i < acount(prototype->graphs); i++) {
        if (!strcmp(prototype->graphs[i], behavior)) {
            return true;
        }
    }
    return false;
}

/* Per prototype that attaches the open behavior: Follow newest, which
   follows each one the game spawns (Unreal's debug filter by class), and
   Break when spawned. The scene's game only; a preview runs one object. */
static void prototype_choices(studio *s, target_group *groups)
{
    app_scene_authoring *scene = s->scene;
    if (scene->active_runtime_target != APP_RUNTIME_SCENE) {
        return;
    }
    const char *newest = scene->debug_newest;
    for (int i = 0; i < acount(scene->prototypes); i++) {
        const prototype *prototype = scene->prototypes[i].prototype;
        if (!prototype_attaches(prototype, scene->graph_document_id)) {
            continue;
        }
        target_choice follow = {
            .keywords = "newest latest last spawned follow",
            .icon = studio_prototype_icon(s, prototype->name),
            .current = newest && !strcmp(newest, prototype->name)
        };
        snprintf(follow.value, sizeof follow.value, "newest:%s", prototype->name);
        snprintf(follow.label, sizeof follow.label, "%s", prototype->name);
        apush(groups[GROUP_NEWEST].choices, follow);
        bool on = app_scene_authoring_spawn_break(scene, prototype->name);
        target_choice pause = { .keywords = "break pause spawn breakpoint",
                                .icon = on ? KIT_ICON_DOT : KIT_ICON_CIRCLE,
                                .current = on };
        snprintf(pause.value, sizeof pause.value, "break:%s", prototype->name);
        snprintf(pause.label, sizeof pause.label, "%s", prototype->name);
        apush(groups[GROUP_BREAK].choices, pause);
    }
}

/* The running instances of the open behavior to debug, searchable, after
   No target (Unreal's debug object menu), then Follow newest and Break
   when spawned per prototype, then the scene's and the spawned instances. */
static void target_choices(studio *s, target_group *groups)
{
    s->panel_work.target_choices++;
    app_scene_authoring *scene = s->scene;
    source_registry *behaviors = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                                   ? prototype_preview_sources(scene->prototype_preview)
                                   : scene->live->runtime->sources;
    bool on = targeted(scene);
    target_choice none = { .value = "none",
                           .label = "No target",
                           .icon = KIT_ICON_CLOSE,
                           .current = !on };
    apush(groups[GROUP_NONE].choices, none);
    prototype_choices(s, groups);
    execution_program *program = source_program(behaviors, scene->graph_document_id);
    for (int i = 0; program && i < execution_program_owner_count(program); i++) {
        ecs_entity entity = ecs_entity_from_u64(execution_program_owner_at(program, i));
        target_choice choice = {
            .icon = KIT_ICON_TARGET,
            .current = on && !scene->debug_newest &&
                       ecs_entity_to_u64(entity) ==
                           ecs_entity_to_u64(scene->debug.entity),
        };
        snprintf(
            choice.value,
            sizeof choice.value,
            "entity:%llu",
            (unsigned long long)ecs_entity_to_u64(entity)
        );
        instance_label(scene, entity, NULL, choice.label, sizeof choice.label);
        apush(
            groups[actor_name(scene, entity) ? GROUP_SCENE : GROUP_SPAWNED].choices,
            choice
        );
    }
}

/* A group's options are items whose id is the choice's value. */
static bool show_group(yyjson_mut_doc *doc, yyjson_mut_val *shown, const target_group *group)
{
    yyjson_mut_val *options = yyjson_mut_obj_get(shown, "options");
    bool changed = ui_data_put_text(doc, shown, "label", group->label);
    changed |= fit(doc, options, acount(group->choices), SHAPE_ITEM);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(options);
    for (int i = 0; i < acount(group->choices); i++) {
        const target_choice *choice = &group->choices[i];
        header_item item = { .id = choice->value,
                             .label = choice->label,
                             .keywords = choice->keywords,
                             .icon = choice->icon,
                             .current = choice->current };
        changed |= put_item(doc, yyjson_mut_arr_iter_next(&it), &item);
    }
    return changed;
}

/* Only groups with choices show, as the kit's dropdown headed only the
   groups its items named. */
bool studio_graph_targets_sync(studio *s, const studio_panel_sync *at)
{
    /* The choice list has no submenus. Read the owning popover stack rather
       than the DOM's data-open, which remains during its closing animation.
       Rebuild on every open sync so live owners and their labels stay current. */
    rml_element *popup = at->preview ? NULL
                                     : popover_top(rml_backend_context(s->backend));
    if (!at->preview && (!popup || strcmp(rml_element_id(popup), "debug-targets"))) {
        return false;
    }
    target_group groups[GROUP_COUNT] = { 0 };
    for (int i = 0; i < GROUP_COUNT; i++) {
        groups[i].label = GROUP_LABELS[i];
    }
    if (studio_graph_active(s) && shows_target(s)) {
        target_choices(s, groups);
    }
    int shown = 0;
    for (int i = 0; i < GROUP_COUNT; i++) {
        shown += acount(groups[i].choices) > 0;
    }
    yyjson_mut_val *array = yyjson_mut_obj_get(at->variable, "groups");
    bool changed = fit(at->doc, array, shown, SHAPE_GROUP);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = 0; i < GROUP_COUNT; i++) {
        if (acount(groups[i].choices)) {
            changed |= show_group(at->doc, yyjson_mut_arr_iter_next(&it), &groups[i]);
        }
        afree(groups[i].choices);
    }
    return changed;
}

/* ---------- Actions ---------- */

/* Adds a graph under the first free name and opens it, as one undo step. */
static void create_graph(editor *e, graph_kind kind, const char *base)
{
    char name[128];
    snprintf(name, sizeof name, "%s", base);
    for (int i = 2; behavior_asset_find_graph_by_name(e->asset, name); i++) {
        snprintf(name, sizeof name, "%s %d", base, i);
    }
    editor_command create = { .kind = EDITOR_COMMAND_CREATE_GRAPH,
                              .data.create_graph = { kind, name } };
    if (editor_command_apply(e, create) == 0) {
        graph *made = behavior_asset_find_graph_by_name(e->asset, name);
        if (made) {
            editor_open_graph(e, made->graph_id);
        }
    }
}

/* The name field commits (Blender's F2): a new name renames the graph and
   its uses, as one undo step. */
static void rename_graph(studio *s, const char *name)
{
    graph *gr = sg_active_graph(s);
    s->graph.renaming = false;
    if (!name[0] || (gr->name && !strcmp(gr->name, name)) ||
        studio_members_rename_across(s, STUDIO_MEMBER_GRAPH, gr->graph_id, gr->name, name)) {
        return;
    }
    editor_command rename = { .kind = EDITOR_COMMAND_RENAME_GRAPH,
                              .data.rename_graph = { gr->graph_id, name } };
    if (editor_command_apply(sg_graph_editor(s), rename) != 0) {
        kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = "A graph has that name" });
    }
}

/* Switches to another asset of the behavior's kind, by name. */
static void open_asset(studio *s, const char *name)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    reference_asset_kind kind = document_kind(s);
    for (int i = 0; index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (asset->kind == kind && !strcmp(asset->name, name) &&
            strcmp(name, s->scene->graph_document_id)) {
            finish_transform_edit(s->scene, false);
            app_scene_authoring_open_asset(s->scene, i);
            return;
        }
    }
}

/* A choice of the target picker: none or an instance, Follow newest, or a
   prototype's Break when spawned toggled. */
static void choose_target(studio *s, const char *value)
{
    const char *colon = strchr(value, ':');
    const char *rest = colon ? colon + 1 : "";
    if (!strncmp(value, "newest:", 7)) {
        app_scene_authoring_follow_newest(s->scene, rest);
    } else if (!strncmp(value, "break:", 6)) {
        studio_toggle_spawn_break(s, rest);
    } else if (!strncmp(value, "entity:", 7)) {
        app_scene_authoring_choose_debug_target(
            s->scene,
            ecs_entity_from_u64(strtoull(rest, NULL, 10))
        );
    } else {
        app_scene_authoring_choose_debug_target(s->scene, (ecs_entity){ 0 });
    }
}

static const char *argument(const ui_activation *a)
{
    return a->argc ? a->args[0] : "";
}

/* What the graph's own controls do: navigation, the picker and the Graph
   menu. An action may free the graph, so each returns after one. */
static bool graph_action(studio *s, const char *action, const ui_activation *a)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (!strcmp(action, "graph")) {
        if (editor_open_graph(e, strtoull(argument(a), NULL, 10)) == 0) {
            studio_code_reveal_graph(s);
        }
    } else if (!strcmp(action, "caller")) {
        editor_graph_ancestor(e, atoi(argument(a)));
    } else if (!strcmp(action, "newFunction") || !strcmp(action, "newMacro")) {
        bool function = !strcmp(action, "newFunction");
        create_graph(e, function ? GRAPH_FUNCTION : GRAPH_MACRO, function ? "New Function" : "New Macro");
    } else if (!strcmp(action, "rename")) {
        s->graph.renaming = true;
        s->graph.rename_graph = gr->graph_id;
    } else if (!strcmp(action, "renamed") && s->graph.renaming) {
        rename_graph(s, argument(a));
    } else if (!strcmp(action, "cancel")) {
        s->graph.renaming = false;
    } else if (!strcmp(action, "graphRefs")) {
        studio_find_graph_references(s, gr);
    } else if (!strcmp(action, "graphCalls")) {
        studio_calls_graph(s, gr);
    } else if (!strcmp(action, "remove")) {
        editor_delete_graph(e, gr->graph_id);
    } else {
        return false;
    }
    return true;
}

static bool menu_action(studio *s, const char *action, const ui_activation *a)
{
    if (!strcmp(action, "optimize")) {
        sg_optimize(s);
    } else if (!strcmp(action, "collapse")) {
        sg_collapse_math(s);
    } else if (!strcmp(action, "align")) {
        for (int i = 0; i < ALIGN_COUNT; i++) {
            if (!strcmp(ALIGN[i].id, argument(a))) {
                sg_align(s, ALIGN[i].edge);
            }
        }
    } else if (!strcmp(action, "distribute")) {
        sg_distribute(s, atoi(argument(a)) != 0);
    } else if (!strcmp(action, "straighten")) {
        sg_straighten(s);
    } else if (!strcmp(action, "arrange")) {
        sg_arrange(s, false);
        sg_frame_nodes(s, false, true);
    } else if (!strcmp(action, "frame")) {
        sg_frame_nodes(s, false, true);
    } else if (!strcmp(action, "zoomActual")) {
        sg_zoom_actual(s);
    } else if (!strcmp(action, "find")) {
        studio_graph_find(s);
    } else {
        return false;
    }
    return true;
}

void studio_graph_header_action(studio *s, const char *action, const ui_activation *a)
{
    if (!studio_graph_active(s)) {
        return;
    }
    if (!strcmp(action, "view")) {
        studio_behavior_show(s, atoi(argument(a)));
    } else if (!strcmp(action, "back") || !strcmp(action, "forward")) {
        studio_nav_step(s, !strcmp(action, "back") ? -1 : 1);
    } else if (!strcmp(action, "assetRefs")) {
        studio_find_document_references(s);
    } else if (!strcmp(action, "asset")) {
        open_asset(s, argument(a));
    } else if (!strcmp(action, "targets")) {
        studio_ui_resync(s, "graph_targets");
    } else if (!strcmp(action, "target")) {
        choose_target(s, argument(a));
        studio_ui_resync(s, "graph_targets");
    } else if (!graph_action(s, action, a)) {
        menu_action(s, action, a);
    }
}

/* The controls are BehaviorHeader's, mounted per area as the View menu was
   (studio_ui_area_controls). */
void studio_behavior_header(studio *s, int area)
{
    studio_ui_area_controls(s, area, "graph", "BehaviorHeader");
}
