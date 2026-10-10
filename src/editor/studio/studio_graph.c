#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
/* The behavior graph canvas: nodes are RmlUi elements in a context of their own
 * whose dp ratio includes graph zoom and standard-font scale, so text is
 * rasterized at its displayed size and clipping keeps working (a transformed
 * element would not clip). The canvas is a texture shown by a surface in the
 * Behavior area; window input is replayed into it in graph units. Interaction
 * follows docs/editor-graph-references.md; edits go through the graph editor's
 * mutation helpers and history. */
#include "diagnostics/tracy.h"
#include "editor/studio/studio_graph_internal.h"
#include "graph/authoring/expand.h"
#include "graph/authoring/pin_edit.h"
#include "graph/document/extract.h"
#include "graph/model/reactive_presentation.h"
#include "graph/nodes/behavior_expression.h"
#include "platform/cute.h"
#include "support/array.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Invalidation follows authored model revisions, never the camera. Pending
 * drafts have not received an edit revision yet, so frame boundaries explicitly
 * invalidate them before geometry is consumed. */
void sg_calculations_invalidate(studio *s)
{
    studio_graph *g = &s->graph;
    asetlen(g->layouts, 0);
    if (g->calculations) {
        cf_map_clear(g->calculations->layout_index);
        g->calculations->routes_dirty = true;
        g->calculations->unused_dirty = true;
    }
}

void sg_calculations_sync(const studio *s)
{
    studio_graph *g = (studio_graph *)&s->graph;
    if (!g->calculations) {
        g->calculations = xcalloc(1, sizeof *g->calculations);
    }
    struct studio_graph_calculations *c = g->calculations;
    graph *gr = sg_active_graph(s);
    editor *e = sg_graph_editor(s);
    int forms = s->prefs.short_bindings | s->prefs.title_flow << 1 |
                s->prefs.compact_nodes << 2;
    unsigned edit = editor_edit_revision(e), assets = s->scene->asset_index_generation;
    unsigned fonts = rml_font_generation();
    float ratio = studio_pixel_ratio(s);
    if (c->lifetime != gr->lifetime_id || c->topology != graph_topology_revision(gr) ||
        c->edit != edit || c->asset != e->asset || c->assets != assets ||
        c->fonts != fonts || c->forms != forms ||
        c->choices != e->choices_resolver || c->choices_user != e->choices_user ||
        c->standard_family != s->prefs.standard_font_family ||
        c->code_family != s->prefs.code_font_family ||
        c->standard_size != s->prefs.standard_font_size ||
        c->code_size != s->prefs.code_font_size || c->pixel_ratio != ratio ||
        c->comfortable != s->prefs.comfortable) {
        sg_calculations_invalidate((studio *)s);
        c->lifetime = gr->lifetime_id;
        c->topology = graph_topology_revision(gr);
        c->edit = edit;
        c->assets = assets;
        c->fonts = fonts;
        c->asset = e->asset;
        c->choices = e->choices_resolver;
        c->choices_user = e->choices_user;
        c->forms = forms;
        c->standard_family = s->prefs.standard_font_family;
        c->code_family = s->prefs.code_font_family;
        c->standard_size = s->prefs.standard_font_size;
        c->code_size = s->prefs.code_font_size;
        c->pixel_ratio = ratio;
        c->comfortable = s->prefs.comfortable;
    }
}

/* A referenced local definition can change without changing the caller's
 * topology. Compare its version, once per frame, rather than its nodes/strings. */
static void calculations_definitions(studio *s)
{
    struct studio_graph_calculations *c = s->graph.calculations;
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    bool changed = acount(c->definitions) != acount(asset->graphs) * 2;
    if (!changed) {
        for (int i = 0; i < acount(asset->graphs); i++) {
            const graph *gr = asset->graphs[i];
            changed |= c->definitions[i * 2] != gr->lifetime_id ||
                       c->definitions[i * 2 + 1] != graph_topology_revision(gr);
        }
    }
    if (!changed) {
        return;
    }
    sg_calculations_invalidate(s);
    asetlen(c->definitions, 0);
    for (int i = 0; i < acount(asset->graphs); i++) {
        apush(c->definitions, asset->graphs[i]->lifetime_id);
        apush(c->definitions, graph_topology_revision(asset->graphs[i]));
    }
}

void sg_calculations_free(studio *s)
{
    struct studio_graph_calculations *c = s->graph.calculations;
    if (!c) {
        return;
    }
    cf_map_free(c->layout_index);
    afree(c->definitions);
    afree(c->routing.nodes);
    afree(c->routing.boxes);
    afree(c->routing.pos);
    afree(c->routing.links);
    afree(c->routing.edge_link);
    free(c);
    s->graph.calculations = NULL;
}

/* ---------- Model access ---------- */

editor *sg_graph_editor(const studio *s)
{
    return s->scene->graph_editor;
}

bool studio_graph_active(const studio *s)
{
    const editor *e = sg_graph_editor(s);
    return e && s->scene->graph_document_id && e->active_graph;
}

graph *sg_active_graph(const studio *s)
{
    return studio_graph_active(s) ? sg_graph_editor(s)->active_graph : NULL;
}

bool sg_graph_readonly(const studio *s)
{
    const graph *g = sg_active_graph(s);
    return !g || g->math_expression || studio_code_graph_readonly(s) ||
           !behavior_asset_mutable(sg_graph_editor(s)->asset);
}

const char *sg_readonly_reason(const studio *s)
{
    const graph *g = sg_active_graph(s);
    if (!g) {
        return NULL;
    }
    if (g->math_expression) {
        return "This graph shows a math expression. Edit the expression in its "
               "node.";
    }
    if (studio_code_graph_readonly(s)) {
        return "The code has errors or is being checked. The graph edits again "
               "once it's valid.";
    }
    if (!behavior_asset_mutable(sg_graph_editor(s)->asset)) {
        return "This asset can't be edited here.";
    }
    return NULL;
}

void sg_refuse_edit(studio *s)
{
    const char *reason = sg_readonly_reason(s);
    assert(reason);
    s->graph.readonly_toast = kit_toast(
        s->kit,
        s->graph.readonly_toast,
        &(kit_toast_desc){ .type = KIT_TOAST_INFO, .title = "Read-only graph", .description = reason }
    );
}

bool sg_node_visible(const graph *g, const node *n)
{
    return n && n->desc && behavior_reactive_authored_node(g, n) == n &&
           !graph_node_is_inline(g, n);
}

node *sg_node_by_id(const studio *s, uint64_t id)
{
    graph *gr = sg_active_graph(s);
    node *n = gr && id ? graph_node_by_id(gr, id) : NULL;
    return n ? (node *)behavior_reactive_authored_node(gr, n) : NULL;
}

graph_comment_box *sg_comment_by_id(const studio *s, uint64_t id)
{
    graph *gr = sg_active_graph(s);
    for (int i = 0; gr && id && i < acount(gr->comment_boxes); i++) {
        if (gr->comment_boxes[i].id == id) {
            return &gr->comment_boxes[i];
        }
    }
    return NULL;
}

void sg_link_ref_of(const edge *ed, ed_link_ref *out)
{
    *out = (ed_link_ref){ ed->from_id, ed->from_pin_id, ed->to_id, ed->to_pin_id };
}

bool sg_edge_ends(const graph *gr, const edge *ed, node **from, int *from_pin, node **to, int *to_pin)
{
    int from_out = 0, to_out = 0;
    *from = graph_node_by_id(gr, ed->from_id);
    *to = graph_node_by_id(gr, ed->to_id);
    return *from && *to &&
           graph_pin_by_id(*from, ed->from_pin_id, &from_out, from_pin) && from_out &&
           graph_pin_by_id(*to, ed->to_pin_id, &to_out, to_pin) && !to_out;
}

/* ---------- Menus ---------- */

/* 1 when any data pin is watched, 0 when none is, -1 without data pins. */
static int node_watched(const node *n)
{
    int watched = -1;
    for (int p = 0; p < n->input_count + n->output_count; p++) {
        const pin_spec *spec = p < n->input_count
                                 ? &n->input_pins[p]
                                 : &n->output_pins[p - n->input_count];
        if (spec->kind == PIN_DATA) {
            watched = watched > 0 || (spec->flags & PIN_FLAG_WATCH) ? 1 : 0;
        }
    }
    return watched;
}

static bool folded_inputs(const graph *gr, node *n)
{
    node *source = NULL;
    int pin = -1;
    for (int p = 0; p < n->input_count; p++) {
        if (graph_inline_source(gr, n, p, &source, &pin)) {
            return true;
        }
    }
    return false;
}

/* Draws the sources folded into n's inputs as their own nodes; automatic
   arrangement places them clear of their neighbours. */
static void expand_inputs(editor *e, node *n)
{
    graph *gr = e->active_graph;
    node **sources = NULL;
    for (int p = 0; p < n->input_count; p++) {
        node *source = NULL;
        int pin = -1;
        if (graph_inline_source(gr, n, p, &source, &pin)) {
            apush(sources, source);
        }
    }
    for (int i = 0; i < acount(sources); i++) {
        (void)graph_set_node_presentation(gr, sources[i], NODE_PRESENTATION_EXPANDED);
        sources[i]->placed = false;
    }
    afree(sources);
    e->arrange_pending = true;
}

/* Alignment for two or more selected nodes, and straightening (Unreal). */
static bool selection_items(studio *s, sg_menu *m)
{
    static const struct
    {
        const char *key, *label;
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
    editor *e = sg_graph_editor(s);
    bool readonly = sg_graph_readonly(s);
    int count = acount(e->selected);
    bool ran = false;
    if (count > 1) {
        sg_submenu_begin(m, "align", "Alignment", KIT_ICON_LAYOUT);
        for (int i = 0; i < (int)(sizeof ALIGN / sizeof *ALIGN) && !ran; i++) {
            if (sg_menu_item(m, ALIGN[i].key, ALIGN[i].label, ALIGN[i].icon, NULL, readonly)) {
                sg_align(s, ALIGN[i].edge);
                ran = true;
            }
        }
        sg_menu_separator(m);
        if (!ran && sg_menu_item(
                        m,
                        "horizontal",
                        "Distribute Horizontally",
                        KIT_ICON_DISTRIBUTE_X,
                        NULL,
                        readonly || count < 3
                    )) {
            sg_distribute(s, false);
            ran = true;
        }
        if (!ran && sg_menu_item(
                        m,
                        "vertical",
                        "Distribute Vertically",
                        KIT_ICON_DISTRIBUTE_Y,
                        NULL,
                        readonly || count < 3
                    )) {
            sg_distribute(s, true);
            ran = true;
        }
        sg_submenu_end(m);
    }
    if (!ran &&
        sg_menu_item(m, "straighten", "Straighten Connections", KIT_ICON_MOVE, "Q", readonly || !count)) {
        sg_straighten(s);
        ran = true;
    }
    return ran;
}

/* Grows the selection along its wires: one step (Blender's Select Linked From
   and To) or to the end (Unreal's Select All Input and Output Nodes). */
static bool select_items(studio *s, sg_menu *m)
{
    static const struct
    {
        const char *key, *label, *chord;
        bool upstream, all;
    } LINKED[] = {
        { "inputs", "Input Nodes", "L", true, false },
        { "outputs", "Output Nodes", "Alt+L", false, false },
        { "all-inputs", "All Input Nodes", NULL, true, true },
        { "all-outputs", "All Output Nodes", NULL, false, true },
    };
    sg_submenu_begin(m, "select", "Select", KIT_ICON_SELECT);
    bool ran = false;
    for (int i = 0; i < (int)(sizeof LINKED / sizeof *LINKED) && !ran; i++) {
        if (i == 2) {
            sg_menu_separator(m);
        }
        const char *chord = LINKED[i].chord ? studio_chord(s, LINKED[i].chord) : NULL;
        if (sg_menu_item(m, LINKED[i].key, LINKED[i].label, 0, chord, false)) {
            sg_select_linked(s, LINKED[i].upstream, LINKED[i].all);
            ran = true;
        }
    }
    sg_submenu_end(m);
    return ran;
}

/* An action may free the node, its graph or the whole asset, so the menu stops
   after one runs; the click closes it. */
void sg_node_menu(studio *s, sg_menu *m, node *n)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    char title[256];
    sg_menu_title(m, ed_node_title(e, n, title, sizeof title));
    const char *definition = studio_chord(s, "Alt+G");
    if (ed_is_graph_call(n) &&
        sg_menu_item(m, "open", "Open Graph", KIT_ICON_EXTERNAL, definition, false)) {
        editor_open_graph_call(e, n->id);
        return;
    }
    if (!ed_is_graph_call(n) && studio_node_definable(s, n) &&
        sg_menu_item(m, "definition", "Go to Definition", KIT_ICON_EXTERNAL, definition, false)) {
        studio_go_to_definition(s, n);
        return;
    }
    if (studio_node_referable(s, n) &&
        sg_menu_item(m, "refs", "Find References", KIT_ICON_REFERENCES, studio_chord(s, "Shift+Alt+F"), false)) {
        studio_find_node_references(s, n);
        return;
    }
    if (studio_node_traceable(s, n) &&
        sg_menu_item(m, "calls", "Call Hierarchy", KIT_ICON_CALLERS, studio_chord(s, "Shift+Alt+H"), false)) {
        studio_calls_node(s, n);
        return;
    }
    if (studio_event_renameable(s, n) &&
        sg_menu_item(m, "rename-event", "Rename Event", KIT_ICON_EDIT, "F2", readonly)) {
        studio_refactor_rename_event(s, studio_event_renameable(s, n));
        return;
    }
    graph_pin_run run;
    if (graph_pin_run_of(n, &run) &&
        sg_menu_item(m, "addpin", "Add Pin", KIT_ICON_ADD, NULL, readonly)) {
        sg_add_run_pin(s, n);
        return;
    }
    if (sg_menu_item(m, "dup", "Duplicate", KIT_ICON_COPY, studio_chord(s, "Shift+D"), readonly)) {
        ed_duplicate_selected(e);
        return;
    }
    if (sg_menu_item(m, "delete", "Delete", KIT_ICON_TRASH, "X", readonly)) {
        sg_delete_selection(s);
        return;
    }
    sg_menu_separator(m);
    int count = acount(e->selected);
    bool locked = readonly || e->ui_context == EDITOR_UI_CONTEXT_SYSTEM;
    if (sg_menu_item(
            m,
            "collapse",
            "Collapse to Function",
            KIT_ICON_FUNCTION,
            studio_chord(s, "Mod+G"),
            locked || behavior_asset_can_extract_function(e->asset, gr, e->selected, count)
        )) {
        sg_collapse(s, GRAPH_FUNCTION);
        return;
    }
    if (sg_menu_item(
            m,
            "macro",
            "Collapse to Macro",
            KIT_ICON_EXPRESSION,
            NULL,
            locked || behavior_asset_can_extract_macro(e->asset, gr, e->selected, count)
        )) {
        sg_collapse(s, GRAPH_MACRO);
        return;
    }
    if (ed_is_graph_call(n) && n->desc != &NODE_BehaviorExpression &&
        sg_menu_item(
            m,
            "expand-node",
            "Expand Node",
            KIT_ICON_UNFOLD,
            NULL,
            readonly || behavior_asset_can_expand_call(e->asset, gr, n)
        )) {
        sg_expand_node(s, n);
        return;
    }
    if (sg_refreshable(s) &&
        sg_menu_item(m, "refresh", "Refresh Nodes", KIT_ICON_REFRESH, NULL, readonly)) {
        sg_refresh_nodes(s);
        return;
    }
    const char *expression = behavior_expression_can_convert_selection(
        e->asset,
        gr,
        e->selected,
        acount(e->selected)
    );
    if (sg_menu_item(
            m,
            "expression",
            "Convert to Expression",
            KIT_ICON_EXPRESSION,
            NULL,
            readonly || expression || e->ui_context == EDITOR_UI_CONTEXT_SYSTEM
        )) {
        sg_convert_to_expression(s);
        return;
    }
    sg_menu_separator(m);
    if (select_items(s, m) || selection_items(s, m)) {
        return;
    }
    sg_menu_separator(m);
    if (sg_menu_item(
            m,
            "mute",
            n->disabled ? "Enable" : "Disable",
            n->disabled ? KIT_ICON_EYE : KIT_ICON_EYE_OFF,
            "M",
            readonly
        )) {
        sg_set_node_disabled(s, n, !n->disabled);
        return;
    }
    if (node_behavior_for(n) != NODE_BEHAVIOR_PURE &&
        sg_menu_item(m, "break", n->breakpoint ? "Clear Breakpoint" : "Set Breakpoint", KIT_ICON_CIRCLE, "F9", readonly)) {
        ed_snapshot_before(e);
        graph_set_breakpoint(gr, n, !n->breakpoint);
        return;
    }
    int watch = node_watched(n);
    if (watch >= 0 &&
        sg_menu_item(m, "watch", watch ? "Stop Watching" : "Watch Values", KIT_ICON_TARGET, NULL, readonly)) {
        ed_snapshot_before(e);
        for (int kind = ED_PIN_INPUT; kind <= ED_PIN_OUTPUT; kind++) {
            int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
            for (int p = 0; p < count; p++) {
                const pin_spec *spec = kind == ED_PIN_INPUT ? &n->input_pins[p]
                                                            : &n->output_pins[p];
                if (spec->kind == PIN_DATA) {
                    graph_set_pin_watch(gr, n, kind == ED_PIN_OUTPUT, p, !watch);
                }
            }
        }
        return;
    }
    if (sg_node_comment_items(s, m, n)) {
        return;
    }
    if (ed_source_scope_available(e) && graph_node_can_inline(gr, n) &&
        sg_menu_item(m, "inline", "Inline", KIT_ICON_EXPRESSION, NULL, readonly)) {
        ed_snapshot_before(e);
        (void)graph_inline_node(gr, n);
        ed_select_node(e, graph_inline_owner(gr, n), false);
        return;
    }
    if (folded_inputs(gr, n) &&
        sg_menu_item(m, "expand", "Expand Inputs", KIT_ICON_MAXIMIZE, NULL, readonly)) {
        ed_snapshot_before(e);
        expand_inputs(e, n);
    }
}

/* ---------- Host ---------- */

static float canvas_w(const studio *s)
{
    return s->graph.surface.rect.w / sg_zoom(s);
}

static float canvas_h(const studio *s)
{
    return s->graph.surface.rect.h / sg_zoom(s);
}

static bool ensure_canvas(studio *s)
{
    studio_graph *g = &s->graph;
    if (g->kit) {
        return true;
    }
    float scale = studio_pixel_ratio(s);
    g->backend = rml_backend_create(
        (int)fmaxf(g->surface.rect.w, 1),
        (int)fmaxf(g->surface.rect.h, 1),
        scale
    );
    if (!g->backend) {
        return false;
    }
    g->kit = kit_create(rml_backend_context(g->backend), "graph.rml");
    kit_theme(g->kit, s->page.document.rcss ? s->page.document.rcss : "");
    studio_apply_graph_font_settings(g->kit, &s->prefs);
    g->in.mouse_x = g->in.mouse_y = -1e9f;
    if (g->zoom <= 0) {
        g->zoom = 1;
    }
    return g->kit != NULL;
}

static bool graph_shown(const studio *s)
{
    const studio_graph *g = &s->graph;
    const studio_layout *layout = &s->prefs.layouts[s->prefs.workspace];
    return g->area >= 0 && (s->maximized < 0 || s->maximized == g->area) &&
           layout->editors[g->area] == STUDIO_EDITOR_BEHAVIOR &&
           s->areas[g->area].rect.w >= 1 && s->areas[g->area].rect.h >= 1 &&
           g->surface.rect.w >= 1 && g->surface.rect.h >= 1 &&
           studio_graph_active(s);
}

void studio_graph_frame(studio *s, double delta_seconds)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    kit_rect r = g->surface.rect;
    bool shown = graph_shown(s);
    /* Nothing traces the debug target while no graph shows it. */
    app_scene_authoring_set_debug_hidden(s->scene, !shown);
    if (!shown || !ensure_canvas(s)) {
        asetlen(g->events, 0);
        g->captured = false;
        g->area = -1;
        return;
    }
    sg_follow_graph(s);
    /* Node settings change node sizes and the wire style changes column gaps,
       so automatic nodes arrange again. */
    int forms = s->prefs.short_bindings | s->prefs.title_flow << 1 |
                s->prefs.compact_nodes << 2 | (int)s->prefs.wires << 3;
    e->arrange_pending |= forms != g->forms;
    g->forms = forms;
    float scale = studio_pixel_ratio(s);
    float graph_ratio = studio_graph_pixel_ratio(s);
    rml_backend_resize_pixels(g->backend, (int)lroundf(r.w * scale), (int)lroundf(r.h * scale), graph_ratio);
    sg_calculations_sync(s);
    calculations_definitions(s);
    if (editor_has_pending_edits(e)) {
        sg_calculations_invalidate(s);
    }
    sg_replay(s);
    /* Source/debug navigation may select a maintenance node. Present its
       semantic Set before gestures or mutation commands consume selection. */
    int selected = 0;
    for (int i = 0; i < acount(e->selected); i++) {
        node *n = (node *)behavior_reactive_authored_node(e->active_graph, e->selected[i]);
        bool duplicate = false;
        for (int k = 0; k < selected; k++) {
            duplicate |= e->selected[k] == n;
        }
        if (!duplicate) {
            e->selected[selected++] = n;
        }
    }
    asetlen(e->selected, selected);
    if (e->front_node) {
        e->front_node = (node *)behavior_reactive_authored_node(e->active_graph, e->front_node);
    }
    sg_pointer(s);
    sg_shortcuts(s);
    sg_nav_keys(s);
    sg_calculations_sync(s);
    calculations_definitions(s);
    if (editor_has_pending_edits(e)) {
        sg_calculations_invalidate(s);
    } /* gestures and field drafts may have edited the graph */
    sg_camera_step(s);
    if (e->arrange_pending) {
        sg_arrange(s, true);
        e->arrange_pending = false;
    }
    if (!g->framed) {
        sg_frame_open(s);
        g->framed = true;
    }
    node *reveal = sg_node_by_id(s, g->reveal);
    if (reveal) {
        sg_show_node(s, reveal, g->reveal_frame);
    }
    g->reveal = 0;
    graph_ratio = studio_graph_pixel_ratio(s);
    rml_backend_resize_pixels(g->backend, (int)lroundf(r.w * scale), (int)lroundf(r.h * scale), graph_ratio);
    g->in.time = s->time;
    g->in.dp_ratio = graph_ratio;
    g->in.zoom = sg_zoom(s);
    g->in.width = canvas_w(s);
    g->in.height = canvas_h(s);
    kit_set_body_class(g->kit, "far", sg_far(s));
    kit_set_body_class(g->kit, "comfortable", s->prefs.comfortable);
    bool light = studio_theme_is_light(s->prefs.theme, s->system_light);
    bool theme_changed = g->theme_initialized &&
                         (g->theme != s->prefs.theme || g->theme_light != light);
    kit_set_body_class(g->kit, "theme-switching", theme_changed);
    if (!g->theme_initialized || theme_changed) {
        studio_theme_apply(g->kit, s->prefs.theme, s->system_light);
        g->theme = s->prefs.theme;
        g->theme_light = light;
        g->theme_initialized = true;
    }
    kit_set_theme(g->kit, "reduced-motion", s->prefs.reduced_motion);
    sg_find_unused(s);
    CHIBI_TRACE_ZONE_BEGIN(values_zone, "Studio graph values");
    ed_refresh_values(e);
    CHIBI_TRACE_ZONE_END(values_zone);
    CHIBI_TRACE_ZONE_BEGIN(emit_zone, "Studio graph emit");
    kit_begin(g->kit, &g->in);
    sg_emit_canvas(s);
    kit_end(g->kit);
    CHIBI_TRACE_ZONE_END(emit_zone);
    CHIBI_TRACE_ZONE_BEGIN(update_zone, "Studio graph update");
    rml_backend_update(g->backend, delta_seconds);
    CHIBI_TRACE_ZONE_END(update_zone);
    CHIBI_TRACE_ZONE_BEGIN(render_zone, "Studio graph render");
    rml_backend_render(g->backend);
    CHIBI_TRACE_ZONE_END(render_zone);
    rml_backend_set_texture_revision(
        s->backend,
        "graph",
        rml_backend_texture(g->backend),
        rml_backend_texture_revision(g->backend)
    );
}

void studio_graph_free(studio *s)
{
    studio_graph *g = &s->graph;
    kit_destroy(g->kit);
    rml_backend_destroy(g->backend);
    arrange_routes_free(&g->routes);
    afree(g->edge_route);
    afree(g->wire_edges);
    afree(g->events);
    afree(g->views);
    afree(g->drag);
    afree(g->box_base);
    studio_graph_details_free(s);
    sg_calculations_free(s);
    afree(g->layouts);
    afree(g->unused);
    afree(g->problems);
    afree(g->find.hits);
    afree(g->visits);
    afree(g->shown);
    afree(g->live.values);
    *g = (studio_graph){ 0 };
}

bool studio_graph_wants_keyboard(const studio *s)
{
    return graph_shown(s) && s->graph.kit && kit_wants_keyboard(s->graph.kit);
}

bool studio_graph_accepts_text(const studio *s)
{
    return graph_shown(s) && s->graph.kit && kit_accepts_text(s->graph.kit);
}

const char *studio_graph_cursor(const studio *s)
{
    const studio_graph *g = &s->graph;
    if (!graph_shown(s) || !g->kit || !(g->surface.hovered || g->captured)) {
        return NULL;
    }
    if (g->gesture == STUDIO_GRAPH_PAN || g->gesture == STUDIO_GRAPH_MOVE) {
        return "move";
    }
    if (g->gesture == STUDIO_GRAPH_RESIZE) {
        return sg_resize_cursor(g->resize_edges);
    }
    if (g->gesture == STUDIO_GRAPH_LINK || g->hover_pin >= 0) {
        return "crosshair";
    }
    return rml_backend_cursor(g->backend);
}

bool studio_graph_can_replay(studio *s, bool redo)
{
    if (s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
        s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        return studio_code_history(s, redo, false);
    }
    const editor *e = sg_graph_editor(s);
    return studio_graph_active(s) &&
           (redo ? editor_can_redo(e) : editor_can_undo(e));
}

void studio_graph_history(studio *s, bool redo)
{
    editor *e = sg_graph_editor(s);
    sg_cancel_gesture(s);
    if (s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
        s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        studio_code_history(s, redo, true);
        return;
    }
    if (redo) {
        editor_redo(e);
    } else {
        editor_undo(e);
    }
}

/* ---------- Area ---------- */

static int visible_nodes(graph *gr)
{
    int count = 0;
    for (int i = 0; i < acount(gr->nodes); i++) {
        count += sg_node_visible(gr, gr->nodes[i]);
    }
    return count;
}

/* With no behavior open, the area offers the selected object's behaviors,
   then a new one or search: an empty editor says how to fill it. */
static void behavior_empty(studio *s)
{
    kit *k = s->kit;
    kit_push(k, "empty", NULL, "pending-editor behavior-empty");
    kit_icon(k, "icon", "glyph", KIT_ICON_GRAPH);
    kit_label(k, "title", "title", "No behavior open");
    scene_actor *actor = selected_actor(s->scene);
    char **names = actor ? instance_behavior_names(s->scene, actor->entity) : NULL;
    char heading[160] = "Select an object to see its behaviors";
    if (actor) {
        snprintf(
            heading,
            sizeof heading,
            acount(names) ? "Behaviors on %s" : "%s has no behaviors",
            actor->name
        );
    }
    kit_label(k, "on", "dim", heading);
    kit_push(k, "choices", NULL, "behavior-choices");
    for (int i = 0; i < acount(names); i++) {
        kit_button_desc open = { .label = names[i],
                                 .icon = KIT_ICON_GRAPH,
                                 .variant = KIT_SUBTLE };
        if (kit_button(k, names[i], &open)) {
            app_scene_authoring_open_behavior(s->scene, names[i]);
        }
    }
    kit_pop(k);
    free_behavior_names(names);
    kit_push(k, "actions", NULL, "behavior-choices");
    kit_button_desc create = { .label = "New Behavior", .icon = KIT_ICON_ADD };
    if (kit_button(k, "new", &create)) {
        studio_command_run(s, STUDIO_COMMAND_NEW_BEHAVIOR);
    }
    kit_button_desc search = { .label = "Search",
                               .icon = KIT_ICON_SEARCH,
                               .shortcut = studio_chord(s, "Mod+K") };
    if (kit_button(k, "search", &search)) {
        studio_command_run(s, STUDIO_COMMAND_SEARCH);
    }
    kit_pop(k);
    kit_pop(k);
}

void studio_behavior_body(studio *s, int area)
{
    kit *k = s->kit;
    studio_graph *g = &s->graph;
    if (!studio_graph_active(s)) {
        behavior_empty(s);
        g->area = -1;
        return;
    }
    g->area = area;
    kit_surface(k, "graph", "graph-surface", "graph", &g->surface);
    studio_members_drop(s);
    /* An empty graph says how to begin, where a node will go. */
    if (!visible_nodes(sg_active_graph(s))) {
        char hint[96];
        snprintf(hint, sizeof hint, "Add a node with %s or a right click", studio_chord(s, "Shift+A"));
        const char *reason = sg_readonly_reason(s);
        kit_label(k, "empty", "graph-empty", reason ? reason : hint);
    }
    studio_debug_call_stack(s);
    sg_find_panel(s);
    studio_definition_peek(s);
}
