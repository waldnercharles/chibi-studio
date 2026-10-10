#include "graph/authoring/graph_edit.h"
/* Graph canvas edits, refactoring, usage and the add-node palette. */
#include "assets/ui_events.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/graph/search.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/ui/labels.h"
#include "graph/authoring/expand.h"
#include "graph/authoring/linked.h"
#include "graph/document/extract.h"
#include "graph/document/library.h"
#include "graph/document/serialize.h"
#include "graph/model/local.h"
#include "graph/model/registry.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/conv_nodes.h"
#include "platform/cute.h"
#include "support/array.h"
#include "support/text_file.h"
#include "support/xalloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Mutations ---------- */

/* The nodes a frame carries: those wholly inside it when the drag begins. */
void sg_comment_contents(studio *s, uint64_t id, bool frame_only)
{
    studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    graph_comment_box *box = sg_comment_by_id(s, id);
    if (!box || frame_only) {
        return;
    }
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        float b[4];
        if (sg_node_visible(gr, n) && sg_node_box(s, n, b) && b[0] >= box->x &&
            b[1] >= box->y && b[0] + b[2] <= box->x + box->width &&
            b[1] + b[3] <= box->y + box->height) {
            apush(g->drag, ((typeof(*g->drag)){ n->id, n->x, n->y }));
        }
    }
}

/* C frames the selection with a comment (Unreal, Blender's Ctrl J). */
void sg_frame_selection(studio *s)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    float lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };
    for (int i = 0; i < acount(e->selected); i++) {
        float b[4];
        if (sg_node_box(s, e->selected[i], b)) {
            lo[0] = fminf(lo[0], b[0]);
            lo[1] = fminf(lo[1], b[1]);
            hi[0] = fmaxf(hi[0], b[0] + b[2]);
            hi[1] = fmaxf(hi[1], b[1] + b[3]);
        }
    }
    if (lo[0] > hi[0]) {
        return;
    }
    const float pad = 24, title = 32;
    ed_snapshot_before(e);
    graph_add_comment_box(
        gr,
        "Comment",
        lo[0] - pad,
        lo[1] - pad - title,
        hi[0] - lo[0] + 2 * pad,
        hi[1] - lo[1] + 2 * pad + title
    );
}

void sg_show_error(studio *s, const char *title, const char *message)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title, .description = message });
    studio_record(s, KIT_TOAST_WARNING, message);
}

/* Connects two sockets through the editor's rules (conversions, inline
   sources, exec ordering). Reports the refusal the editor gives. */
static bool sg_connect_pins_direct(studio *s, node *a, int a_kind, int a_pin, node *b, int b_kind, int b_pin)
{
    editor *e = sg_graph_editor(s);
    ed_pin_view from = { a, a_kind, a_pin };
    ed_pin_view to = { b, b_kind, b_pin };
    ed_clear_connect_error(e);
    e->pending_snapshot = ed_snapshot(e);
    int connected = ed_connect(e, &from, &to);
    if (connected) {
        ed_commit_snapshot(e);
    } else {
        ed_cancel_snapshot(e);
        if (e->connect_error[0]) {
            sg_show_error(s, "Can't connect", e->connect_error);
        }
    }
    return connected;
}

bool sg_connect_pins(studio *s, node *a, int a_kind, int a_pin, node *b, int b_kind, int b_pin)
{
    editor *e = sg_graph_editor(s);
    if (e->presentation) {
        return sg_connect_pins_direct(s, a, a_kind, a_pin, b, b_kind, b_pin);
    }
    char *action = graph_collab_action_connect(e, a, a_kind, a_pin, b, b_kind, b_pin);
    if (!action) {
        return collab_active() && !collab_replaying()
                 ? false
                 : sg_connect_pins_direct(s, a, a_kind, a_pin, b, b_kind, b_pin);
    }
    collab_action_gate gate = collab_actions_begin(action);
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return false;
    }
    bool connected = sg_connect_pins_direct(s, a, a_kind, a_pin, b, b_kind, b_pin);
    bool published = collab_actions_finish(action, connected);
    free(action);
    return connected && published;
}

/* Moving a connected input's wire is one edit, including in shared rooms. */
bool sg_reconnect_pins(studio *s, node *to, int pin)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    node *old = graph_node_by_id(gr, g->link_pickup_node), *from;
    int output, old_pin, from_pin;
    if (!old || !graph_pin_by_id(old, g->link_pickup_pin, &output, &old_pin) ||
        !graph_input_source(gr, old, old_pin, &from, &from_pin)) {
        return false;
    }
    if (g->link_kind != ED_PIN_OUTPUT || from->id != g->link_node ||
        from_pin != g->link_pin) {
        sg_show_error(s, "Can't connect", "This connection changed while you were dragging");
        return false;
    }
    if (to == old && pin == old_pin) {
        return true;
    }
    if (e->presentation) {
        bool changed = graph_collab_reconnect_apply(e, from, from_pin, old, old_pin, to, pin);
        if (!changed && e->connect_error[0]) {
            sg_show_error(s, "Can't connect", e->connect_error);
        }
        return changed;
    }
    char *action = graph_collab_action_reconnect(e, from, from_pin, old, old_pin, to, pin);
    if (action && collab_actions_begin(action) != COLLAB_ACTION_APPLY) {
        free(action);
        return false;
    }
    if (!action && collab_active() && !collab_replaying()) {
        return false;
    }
    ed_clear_connect_error(e);
    bool changed = graph_collab_reconnect_apply(e, from, from_pin, old, old_pin, to, pin);
    if (!changed) {
        sg_show_error(s, "Can't connect", e->connect_error[0] ? e->connect_error : "These pins cannot be connected");
    }
    bool published = action ? collab_actions_finish(action, changed) : true;
    free(action);
    return changed && published;
}

bool sg_pins_compatible(const node *a, int a_kind, int a_pin, const node *b, int b_kind, int b_pin)
{
    if (a == b || a_kind == b_kind) {
        return false;
    }
    const pin_spec *out = a_kind == ED_PIN_OUTPUT ? &a->output_pins[a_pin]
                                                  : &b->output_pins[b_pin];
    const pin_spec *in = a_kind == ED_PIN_OUTPUT ? &b->input_pins[b_pin]
                                                 : &a->input_pins[a_pin];
    return out->kind == in->kind && ed_connection_specs_valid(out, in, out->kind) != 0;
}

/* Dropped on a node body: the first compatible socket, exact types first. */
int sg_body_target(const graph *gr, const node *from, int kind, int pin, const node *to)
{
    int want = kind == ED_PIN_OUTPUT ? ED_PIN_INPUT : ED_PIN_OUTPUT;
    int count = want == ED_PIN_INPUT ? to->input_count : to->output_count;
    int fallback = -1;
    for (int p = 0; p < count; p++) {
        if (!ed_pin_visible(gr, to, want, p) ||
            !sg_pins_compatible(from, kind, pin, to, want, p)) {
            continue;
        }
        node *src = NULL;
        int src_pin = -1;
        bool taken = want == ED_PIN_INPUT &&
                     graph_input_source(gr, to, p, &src, &src_pin);
        if (!taken) {
            return p;
        }
        if (fallback < 0) {
            fallback = p;
        }
    }
    return fallback;
}

void sg_select_only(editor *e, node *n)
{
    ed_clear_selection(e);
    if (n) {
        ed_add_selected(e, n);
    }
}

void sg_delete_selection(studio *s)
{
    editor *e = sg_graph_editor(s);
    uint64_t frame = studio_graph_comment(s);
    if (sg_graph_readonly(s)) {
        if (frame || acount(e->selected) || acount(e->selected_links)) {
            sg_refuse_edit(s);
        }
        return;
    }
    if (frame && !acount(e->selected_links)) {
        ed_snapshot_before(e);
        graph_remove_comment_box(e->active_graph, frame);
        return;
    }
    if (acount(e->selected) || acount(e->selected_links)) {
        ed_delete_selected(e);
    }
}

void sg_copy_selection(studio *s)
{
    editor *e = sg_graph_editor(s);
    if (!acount(e->selected)) {
        return;
    }
    if (e->presentation) {
        uint64_t *origins = ed_source_selected_origins(e);
        char *fragment = origins
                             ? authoring_source_clipboard(
                                   e->presentation, origins, acount(origins),
                                   e->connect_error, sizeof e->connect_error
                               ) : NULL;
        afree(origins);
        if (fragment) {
            kit_clipboard_set(fragment);
            free(e->clipboard);
            e->clipboard = fragment;
        }
        return;
    }
    char *json = graph_save_nodes(e->active_graph, e->selected, acount(e->selected));
    if (json) {
        kit_clipboard_set(json);
        free(e->clipboard);
        e->clipboard = json;
    }
}

/* Where a paste lands: the pointer over the canvas, else the view's center. */
static void paste_point(const studio *s, float *x, float *y)
{
    const studio_graph *g = &s->graph;
    if (g->in.mouse_x > -1e8f) {
        *x = g->world_x;
        *y = g->world_y;
        return;
    }
    float zoom = sg_zoom(s);
    *x = g->x + g->surface.rect.w / 2 / zoom;
    *y = g->y + g->surface.rect.h / 2 / zoom;
}

/* Pasted nodes center on the pointer (Unreal) and are selected. */
static void place_pasted(studio *s, int first)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    float lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };
    for (int i = first; i < acount(gr->nodes); i++) {
        float b[4];
        if (sg_node_visible(gr, gr->nodes[i]) && sg_node_box(s, gr->nodes[i], b)) {
            lo[0] = fminf(lo[0], b[0]);
            lo[1] = fminf(lo[1], b[1]);
            hi[0] = fmaxf(hi[0], b[0] + b[2]);
            hi[1] = fmaxf(hi[1], b[1] + b[3]);
        }
    }
    float x, y;
    paste_point(s, &x, &y);
    float dx = lo[0] <= hi[0] ? roundf(x - (lo[0] + hi[0]) / 2) : 0;
    float dy = lo[1] <= hi[1] ? roundf(y - (lo[1] + hi[1]) / 2) : 0;
    ed_clear_selection(e);
    for (int i = first; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        n->x += dx;
        n->y += dy;
        if (sg_node_visible(gr, n)) {
            n->placed = true;
            ed_add_selected(e, n);
        }
    }
}

void sg_paste(studio *s)
{
    editor *e = sg_graph_editor(s);
    if (sg_graph_readonly(s)) {
        sg_refuse_edit(s);
        return;
    }
    char *clipboard = cf_clipboard_get();
    const char *text = clipboard && clipboard[0] ? clipboard : e->clipboard;
    if (text && e->presentation) {
        uint64_t origin = ed_source_scope_origin(e);
        authoring_edit_kind kind = origin ? AUTHORING_EDIT_APPEND_BODY
                                          : AUTHORING_EDIT_APPEND_DECLARATIONS;
        if (acount(e->selected)) {
            const node *anchor = e->selected[acount(e->selected) - 1];
            const source_presentation_origin *selected = authoring_source_presentation_origin(
                e->presentation, e->active_graph->graph_id, anchor->id, 0
            );
            if (selected && selected->kind == SOURCE_PRESENTATION_STATEMENT) {
                origin = selected->identity;
                kind = AUTHORING_EDIT_INSERT_AFTER;
            }
        }
        authoring_edit_request request = {
            .kind = kind,
            .generation = authoring_source_edit_generation(e->presentation),
            .origin = origin,
            .fragment = text
        };
        ed_source_edit(e, &request);
    } else if (text) {
        ed_snapshot_before(e);
        int first = acount(e->active_graph->nodes);
        if (graph_append(e->active_graph, text, 0, 0) == 0) {
            place_pasted(s, first);
        }
    }
    if (clipboard) {
        cf_free(clipboard);
    }
}

/* ---------- Selection layout ---------- */

/* Applies moved positions as one undo step; `before` is the asset as it was. */
static void commit_moves(editor *e, behavior_asset *before, bool moved)
{
    if (!moved) {
        behavior_asset_free(before);
        return;
    }
    e->pending_snapshot = before;
    ed_commit_snapshot(e);
}

static void move_node(node *n, float x, float y, bool *moved)
{
    *moved |= n->x != x || n->y != y;
    n->x = x;
    n->y = y;
    n->placed = true;
}

/* The selected nodes that draw a box, with their boxes. */
static align_box *selected_boxes(studio *s, node ***nodes)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    align_box *boxes = NULL;
    for (int i = 0; i < acount(e->selected); i++) {
        float b[4];
        if (sg_node_visible(gr, e->selected[i]) && sg_node_box(s, e->selected[i], b)) {
            apush(*nodes, e->selected[i]);
            apush(boxes, ((align_box){ b[0], b[1], b[2], b[3] }));
        }
    }
    return boxes;
}

/* Moves the nodes to their boxes as one undo step, and frees both. */
static void apply_boxes(studio *s, node **nodes, align_box *boxes)
{
    editor *e = sg_graph_editor(s);
    behavior_asset *before = sg_graph_readonly(s) ? NULL : ed_snapshot(e);
    bool moved = false;
    for (int i = 0; before && i < acount(nodes); i++) {
        move_node(nodes[i], roundf(boxes[i].x), roundf(boxes[i].y), &moved);
    }
    commit_moves(e, before, moved);
    afree(nodes);
    afree(boxes);
}

void sg_align(studio *s, align_edge edge)
{
    node **nodes = NULL;
    align_box *boxes = selected_boxes(s, &nodes);
    align_boxes(boxes, acount(boxes), edge);
    apply_boxes(s, nodes, boxes);
}

void sg_distribute(studio *s, bool vertical)
{
    node **nodes = NULL;
    align_box *boxes = selected_boxes(s, &nodes);
    distribute_boxes(boxes, acount(boxes), vertical);
    apply_boxes(s, nodes, boxes);
}

/* Moves one end of a drawn wire so the wire runs level. */
static void level_wire(studio *s, const edge *ed, bool move_source, bool *moved)
{
    graph *gr = sg_active_graph(s);
    node *from, *to;
    int from_pin, to_pin;
    if (!sg_edge_ends(gr, ed, &from, &from_pin, &to, &to_pin) ||
        !sg_node_visible(gr, from) || !sg_node_visible(gr, to) ||
        graph_edge_is_local(gr, ed)) {
        return;
    }
    float ax, ay, bx, by;
    sg_socket_at(s, from, ED_PIN_OUTPUT, from_pin, &ax, &ay);
    sg_socket_at(s, to, ED_PIN_INPUT, to_pin, &bx, &by);
    node *n = move_source ? from : to;
    move_node(n, n->x, n->y + (move_source ? by - ay : ay - by), moved);
}

/* A node's leading wire: its first wired execution input, else its first
   wired data input; a node without inputs leads with its first output. */
static const edge *leading_wire(const graph *gr, const node *n, bool *source)
{
    const edge *data = NULL, *out = NULL;
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *ed = &gr->edges[i];
        if (ed->to_id == n->id && ed->kind == GRAPH_EDGE_EXEC) {
            *source = false;
            return ed;
        }
        if (ed->to_id == n->id && !data && !graph_edge_is_local(gr, ed) &&
            !graph_node_is_inline(gr, graph_node_by_id(gr, ed->from_id))) {
            data = ed;
        }
        out = !out && ed->from_id == n->id ? ed : out;
    }
    *source = !data;
    return data ? data : out;
}

static int by_x(const void *a, const void *b)
{
    const node *left = *(node *const *)a, *right = *(node *const *)b;
    return (left->x > right->x) - (left->x < right->x);
}

void sg_straighten(studio *s)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (sg_graph_readonly(s)) {
        return;
    }
    behavior_asset *before = ed_snapshot(e);
    bool moved = false;
    for (int i = 0; i < acount(e->selected_links); i++) {
        int w = ed_edge_index(e, &e->selected_links[i]);
        if (w >= 0) {
            level_wire(s, &gr->edges[w], false, &moved);
        }
    }
    node **order = NULL;
    for (int i = 0; i < acount(e->selected); i++) {
        apush(order, e->selected[i]);
    }
    if (acount(order) > 1) {
        qsort(order, (size_t)acount(order), sizeof *order, by_x);
    }
    for (int i = 0; i < acount(order); i++) {
        bool source = false;
        const edge *ed = leading_wire(gr, order[i], &source);
        if (ed) {
            level_wire(s, ed, source, &moved);
        }
    }
    afree(order);
    commit_moves(e, before, moved);
}

void sg_straighten_wire(studio *s, int index)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (sg_graph_readonly(s) || index < 0 || index >= acount(gr->edges)) {
        return;
    }
    behavior_asset *before = ed_snapshot(e);
    bool moved = false;
    level_wire(s, &gr->edges[index], false, &moved);
    commit_moves(e, before, moved);
}

bool sg_spawn_node(studio *s, const char *name, float x, float y)
{
    editor *e = sg_graph_editor(s);
    const node_desc *desc = registry_node_by_name(name);
    if (!desc || sg_graph_readonly(s)) {
        return false;
    }
    if (e->presentation) {
        return ed_source_spawn_match(e, &(ed_search_match){ .desc = desc });
    }
    char label[128];
    editor_node_label(desc, label, sizeof label);
    ed_search_match *found = ed_search_matches(e, label);
    node *n = NULL;
    for (int i = 0; i < acount(found) && !n; i++) {
        if (found[i].desc == desc && !found[i].function && !found[i].variable) {
            n = ed_spawn_match_at(e, &found[i], x, y);
        }
    }
    afree(found);
    if (n) {
        n->placed = true;
        sg_select_only(e, n);
    }
    return n != NULL;
}

/* ---------- Usage ----------
   Nodes no event reaches, and pure nodes whose values nothing used reads, draw
   dimmed: the author sees dead code before the compiler prunes it. */

bool sg_is_unused(const studio_graph *g, uint64_t id)
{
    for (int i = 0; i < acount(g->unused); i++) {
        if (g->unused[i] == id) {
            return true;
        }
    }
    return false;
}

void sg_find_unused(studio *s)
{
    studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    sg_calculations_sync(s);
    if (!g->calculations->unused_dirty) {
        return;
    }
    g->calculations->unused_dirty = false;
    g->calculations->reachability_builds++;
    asetlen(g->unused, 0);
    int count = acount(gr->nodes);
    bool *used = xcalloc((size_t)(count ? count : 1), sizeof *used);
    bool roots = false;
    for (int i = 0; i < count; i++) {
        const node *n = gr->nodes[i];
        bool root = n->desc->flow_kind == NODE_FLOW_EVENT ||
                    n->id == gr->entry_id || n->id == gr->return_id || gr->dataflow;
        used[i] = root;
        roots |= root;
    }
    if (!roots) {
        free(used);
        return;
    }
    /* Exec reach forward, then data demand backward, to a fixed point. */
    for (bool changed = true; changed;) {
        changed = false;
        for (int e = 0; e < acount(gr->edges); e++) {
            const edge *ed = &gr->edges[e];
            int from = -1, to = -1;
            for (int i = 0; i < count && (from < 0 || to < 0); i++) {
                from = gr->nodes[i]->id == ed->from_id ? i : from;
                to = gr->nodes[i]->id == ed->to_id ? i : to;
            }
            if (from < 0 || to < 0) {
                continue;
            }
            if (ed->kind == GRAPH_EDGE_EXEC && used[from] && !used[to]) {
                used[to] = changed = true;
            }
            if (ed->kind == GRAPH_EDGE_DATA && used[to] && !used[from] &&
                node_behavior_for(gr->nodes[from]) == NODE_BEHAVIOR_PURE) {
                used[from] = changed = true;
            }
        }
    }
    for (int i = 0; i < count; i++) {
        if (!used[i] && sg_node_visible(gr, gr->nodes[i])) {
            apush(g->unused, gr->nodes[i]->id);
        }
    }
    free(used);
}

/* ---------- Selection ---------- */

/* Grows the selection along its wires (Blender's L and Alt+L, Unreal's Select
   All Input and Output Nodes); selection never enters history. */
void sg_select_linked(studio *s, bool upstream, bool all)
{
    editor *e = sg_graph_editor(s);
    node **linked = NULL;
    graph_linked_nodes(sg_active_graph(s), e->selected, acount(e->selected), upstream, all, &linked);
    for (int i = 0; i < acount(linked); i++) {
        if (sg_node_visible(e->active_graph, linked[i])) {
            ed_add_selected(e, linked[i]);
        }
    }
    afree(linked);
}

/* ---------- Refactoring ---------- */

/* Takes a refactored copy of the behavior as one undo step. */
static void adopt(editor *e, behavior_asset *result)
{
    ed_snapshot_before(e);
    int adopted = ed_adopt_behavior_asset(e, result);
    behavior_asset_free(result);
    assert(adopted == 0);
    (void)adopted;
}

static void refused(studio *s, const char *title, const char *error)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title, .description = error });
}

static const char *extract(editor *e, graph_kind kind, const char *name, behavior_asset **result, uint64_t *call)
{
    if (e->ui_context == EDITOR_UI_CONTEXT_SYSTEM) {
        return "Systems have no functions or macros.";
    }
    graph *gr = e->active_graph;
    int count = acount(e->selected);
    return kind == GRAPH_MACRO
             ? behavior_asset_extract_macro(e->asset, gr, e->selected, count, name, result, call)
             : behavior_asset_extract_function(e->asset, gr, e->selected, count, name, result, call);
}

/* The selection becomes a call to a new function (Ctrl+G) or macro whose
   interface is the wires crossing its edge (Blender, Unreal). */
void sg_collapse(studio *s, graph_kind kind)
{
    editor *e = sg_graph_editor(s);
    const char *base = kind == GRAPH_MACRO ? "New Macro" : "New Function";
    char name[128];
    snprintf(name, sizeof name, "%s", base);
    for (int i = 2; behavior_asset_find_graph_by_name(e->asset, name); i++) {
        snprintf(name, sizeof name, "%s %d", base, i);
    }
    if (e->presentation) {
        uint64_t *origins = ed_source_selected_origins(e);
        if (!acount(origins)) {
            afree(origins);
            refused(s, "Can't collapse", "Select complete source statements to extract");
            return;
        }
        authoring_edit_request request = {
            .kind = kind == GRAPH_MACRO ? AUTHORING_EDIT_EXTRACT_MACRO : AUTHORING_EDIT_EXTRACT_FUNCTION,
            .generation = authoring_source_edit_generation(e->presentation),
            .origins = origins, .origin_count = acount(origins), .name = name
        };
        ed_source_edit(e, &request);
        afree(origins);
        return;
    }
    behavior_asset *result = NULL;
    uint64_t call = 0;
    const char *error = extract(e, kind, name, &result, &call);
    if (error) {
        refused(s, "Can't collapse", error);
        return;
    }
    adopt(e, result);
    sg_select_only(e, graph_node_by_id(e->active_graph, call));
}

/* The library function a call names, or NULL for any other node. */
static const graph *library_function(const editor *e, const node *n)
{
    for (int i = 0; n->reference_asset && i < acount(e->asset_catalog); i++) {
        const behavior_asset *library = e->asset_catalog[i];
        const graph *target = !strcmp(library->name, n->reference_asset)
                                ? behavior_asset_find_graph(library, n->reference_id)
                                : NULL;
        if (target) {
            return target->kind == GRAPH_FUNCTION ? target : NULL;
        }
    }
    return NULL;
}

bool sg_refreshable(const studio *s)
{
    const editor *e = sg_graph_editor(s);
    for (int i = 0; i < acount(e->selected); i++) {
        if (library_function(e, e->selected[i])) {
            return true;
        }
    }
    return false;
}

/* Unreal's Refresh Nodes: the selection's library calls take their
   functions' current signatures, as one undo step. */
void sg_refresh_nodes(studio *s)
{
    editor *e = sg_graph_editor(s);
    if (e->presentation) {
        return;
    }
    graph *gr = sg_active_graph(s);
    unsigned long long before = graph_topology_revision(gr);
    const char *error = NULL;
    e->pending_snapshot = ed_snapshot(e);
    for (int i = 0; i < acount(e->selected) && !error; i++) {
        const graph *target = library_function(e, e->selected[i]);
        error = target ? graph_library_refresh_call(gr, e->selected[i], target) : NULL;
    }
    if (graph_topology_revision(gr) != before) {
        ed_commit_snapshot(e);
    } else {
        ed_cancel_snapshot(e);
    }
    if (error) {
        refused(s, "Can't refresh", error);
    }
}

/* Unreal's Expand Node: the call becomes the body it calls, selected. */
void sg_expand_node(studio *s, node *call)
{
    editor *e = sg_graph_editor(s);
    if (e->presentation) {
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, call->id, 0
        );
        if (!origin) {
            refused(s, "Can't expand", "Select an authored call with a current source origin");
            return;
        }
        authoring_edit_request request = {
            .kind = AUTHORING_EDIT_INLINE_CALL,
            .generation = authoring_source_edit_generation(e->presentation),
            .origin = origin->identity
        };
        ed_source_edit(e, &request);
        return;
    }
    behavior_asset *result = NULL;
    uint64_t *ids = NULL;
    const char *error = behavior_asset_expand_call(e->asset, e->active_graph, call, &result, &ids);
    if (error) {
        refused(s, "Can't expand", error);
        return;
    }
    adopt(e, result);
    ed_clear_selection(e);
    for (int i = 0; i < acount(ids); i++) {
        ed_add_selected(e, graph_node_by_id(e->active_graph, ids[i]));
    }
    afree(ids);
    e->arrange_pending = true;
}

void sg_convert_to_expression(studio *s)
{
    editor *e = sg_graph_editor(s);
    behavior_expression_error error = { 0 };
    node **selection = NULL;
    for (int i = 0; i < acount(e->selected); i++) {
        apush(selection, e->selected[i]);
    }
    if (!ed_convert_behavior_expression_selection(e, selection, acount(selection), &error)) {
        kit_toast(
            s->kit,
            0,
            &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                               .title = "Can't convert",
                               .description = error.message[0] ? error.message : NULL }
        );
    }
    afree(selection);
}

/* Folds reads into bindings and names values used three or more times. */
void sg_optimize(studio *s)
{
    editor *e = sg_graph_editor(s);
    if (ed_optimize_graph(e) < 0) {
        return;
    }
    studio_report(s, e->optimize_summary);
}

/* Each tree of three or more math nodes becomes one expression. */
void sg_collapse_math(studio *s)
{
    editor *e = sg_graph_editor(s);
    if (e->presentation) {
        refused(s, "Can't convert", "Edit canonical source through the checked source editor");
        return;
    }
    ed_snapshot_before(e);
    behavior_expression_error error;
    int created = behavior_expression_collapse_math(e->asset, e->active_graph, 3, &error);
    if (created < 0) {
        kit_toast(
            s->kit,
            0,
            &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                               .title = "Can't convert",
                               .description = error.message }
        );
        return;
    }
    char message[64];
    snprintf(message, sizeof message, "%d expressions", created);
    studio_report(s, message);
    e->arrange_pending = created > 0;
}

/* ---------- Add node ---------- */

/* How well a search result can take the dragged wire (wire_fit). */
static int link_fit(const editor *e, const ed_search_match *match)
{
    const node_desc *desc = match->desc;
    const ed_pin_view *link = &e->spawn_link_pin;
    if (!desc || match->function || match->variable || !link->n) {
        return 1;
    }
    bool from_output = link->kind == ED_PIN_OUTPUT;
    const pin_spec *source = from_output ? &link->n->output_pins[link->index]
                                         : &link->n->input_pins[link->index];
    const pin_spec *pins = from_output ? desc->inputs : desc->outputs;
    int count = from_output ? desc->input_count : desc->output_count, best = 0;
    for (int i = 0; i < count; i++) {
        int fit = from_output ? wire_fit(source, &pins[i])
                              : wire_fit(&pins[i], source);
        best = fit > best ? fit : best;
    }
    return best;
}

/* A node spawned from a wire lands with its wired socket at the drop point
   (Blender, Unreal). */
static void place_on_wire(studio *s, node *n, const ed_pin_view *from)
{
    graph *gr = sg_active_graph(s);
    bool input = from->kind == ED_PIN_OUTPUT;
    int count = input ? n->input_count : n->output_count;
    for (int p = 0; p < count; p++) {
        bool wired = false;
        if (input) {
            node *src = NULL;
            int src_pin = -1;
            wired = graph_input_source(gr, n, p, &src, &src_pin) && src == from->n;
        } else {
            for (int i = 0; i < acount(gr->edges) && !wired; i++) {
                wired = gr->edges[i].from_id == n->id &&
                        gr->edges[i].from_pin_id == n->output_pins[p].id &&
                        gr->edges[i].to_id == from->n->id;
            }
        }
        if (!wired) {
            continue;
        }
        sg_calculations_invalidate(s);
        n->y -= sg_pin_y(s, n, input ? ED_PIN_INPUT : ED_PIN_OUTPUT, p);
        if (!input) {
            n->x -= sg_layout_of(s, n)->w;
        }
        return;
    }
}

/* What a search result does, as its second line. */
static const char *match_about(studio *s, const editor *e, const ed_search_match *match)
{
    node_subject subject = ed_search_match_subject(e, match);
    char text[256];
    if (match->ui_event) {
        snprintf(text, sizeof text, "%s.", match->ui_event->description);
        return kit_string(s->kit, text);
    }
    const ui_data_binding *b = match->ui_data;
    if (b) {
        snprintf(
            text,
            sizeof text,
            "Gives %s self.%s%s%s.",
            b->document,
            b->binding.name,
            b->binding.list ? ", a list whose rows bind " : "",
            b->binding.list ? b->binding.fields : ""
        );
        return kit_string(s->kit, text);
    }
    return node_subject_describe(&subject, text, sizeof text)
             ? kit_string(s->kit, text)
             : NULL;
}

/* ---------- UI events ---------- */

/* The events a ui asset's markup sends that no host event or event
 * definition names already. */
static void add_document_events(ui_event **events, const reference_index *index, const reference_asset *asset)
{
    char *text = text_file_read(asset->path);
    ui_document_event *found = NULL;
    if (text) {
        ui_document_events(text, &found);
    }
    for (int i = 0; i < acount(found); i++) {
        if (!game_host_event_reserved(found[i].name) &&
            reference_index_event_asset(index, found[i].name) < 0) {
            ui_events_add(events, &found[i], asset->name);
        }
    }
    ui_document_events_free(found);
    free(text);
}

static void add_document_bindings(ui_data_binding **out, const reference_asset *asset)
{
    char *text = text_file_read(asset->path);
    ui_document_binding *found = NULL;
    if (text) {
        ui_document_bindings(text, &found);
    }
    for (int i = 0; i < acount(found); i++) {
        ui_data_binding bound = { .binding = found[i] };
        snprintf(bound.document, sizeof bound.document, "%s", asset->name);
        apush(*out, bound);
    }
    afree(found);
    free(text);
}

void studio_graph_ui_data(studio *s, bool again)
{
    /* A guest shares its host's graph editor, whose catalog the host keeps. */
    if (s->hosted) {
        return;
    }
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int behavior = index ? studio_document_asset(s) : -1;
    if (!again && behavior == s->bound.behavior) {
        return;
    }
    s->bound.behavior = behavior;
    afree(s->bound.bindings);
    int *documents = NULL;
    if (behavior >= 0) {
        reference_index_attached_documents(index, behavior, &documents);
    }
    for (int i = 0; i < acount(documents); i++) {
        const reference_asset *asset = reference_index_asset_at(index, documents[i]);
        if (asset->path) {
            add_document_bindings(&s->bound.bindings, asset);
        }
    }
    afree(documents);
    editor_set_ui_data_catalog(
        sg_graph_editor(s),
        s->bound.bindings,
        acount(s->bound.bindings)
    );
}

void studio_add_menu_close(studio *s)
{
    ui_events_free(s->add_menu.events);
    s->add_menu.events = NULL;
    s->add_menu.read = false;
    afree(s->add_menu.matches);
    s->add_menu.matches = NULL;
}

ui_event *studio_behavior_ui_events(studio *s)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int behavior = index ? studio_document_asset(s) : -1;
    int *documents = NULL;
    if (behavior >= 0) {
        reference_index_attached_documents(index, behavior, &documents);
    }
    ui_event *events = NULL;
    for (int i = 0; i < acount(documents); i++) {
        const reference_asset *asset = reference_index_asset_at(index, documents[i]);
        if (asset->path) {
            add_document_events(&events, index, asset);
        }
    }
    afree(documents);
    return events;
}

/* Offers the open behavior's documents' events to the search, read once
   while it is open. */
static void read_catalog(studio *s)
{
    if (!s->add_menu.read) {
        s->add_menu.events = studio_behavior_ui_events(s);
        s->add_menu.read = true;
        studio_graph_ui_data(s, true);
    }
    editor_set_ui_event_catalog(
        sg_graph_editor(s),
        s->add_menu.events,
        acount(s->add_menu.events)
    );
}

/* With nothing typed, the nodes added last lead under Recent, most recent
   first (Serial Position), instead of the alphabet's first category. */
static void recent_nodes_first(const studio *s, studio_palette_entry **entries)
{
    enum
    {
        RECENT_NODES = 5
    };
    studio_palette_entry *ordered = NULL;
    bool *taken = xcalloc((size_t)acount(*entries) + 1, sizeof *taken);
    const studio_recent *recent = s->prefs.recent;
    for (int r = 0; r < STUDIO_RECENT_COUNT && recent[r].key[0] &&
                    acount(ordered) < RECENT_NODES;
         r++) {
        for (int i = 0; i < acount(*entries); i++) {
            if (!taken[i] && !strcmp((*entries)[i].key, recent[r].key)) {
                apush(ordered, (*entries)[i]);
                snprintf(alast(ordered).group, sizeof alast(ordered).group, "Recent");
                taken[i] = true;
                break;
            }
        }
    }
    for (int i = 0; i < acount(*entries); i++) {
        if (!taken[i]) {
            apush(ordered, (*entries)[i]);
        }
    }
    free(taken);
    afree(*entries);
    *entries = ordered;
}

void studio_graph_palette_entries(studio *s, const char *query, studio_palette_entry **entries)
{
    editor *e = sg_graph_editor(s);
    read_catalog(s);
    ed_search_match *found = ed_search_matches(e, query);
    /* For a dragged wire, nodes whose socket matches its type exactly come
       first, then conversions, then wildcards; each band keeps search order. */
    ed_search_match **matches = (ed_search_match **)&s->add_menu.matches;
    aclear(*matches);
    for (int band = 3; band >= 0 && acount(*matches) < 200; band--) {
        for (int i = 0; i < acount(found) && acount(*matches) < 200; i++) {
            if ((e->spawn_link_pending ? link_fit(e, &found[i]) : 3) == band) {
                apush(*matches, found[i]);
            }
        }
    }
    afree(found);
    /* The palette keeps this order and groups rows where a group first shows,
       so a wire's search groups by band, its category after each name. */
    static const char *const BANDS[] = { "Other nodes", "Any type", "Converts", "Fits the wire" };
    bool linked = e->spawn_link_pending;
    for (int i = 0; i < acount(*matches); i++) {
        const ed_search_match *m = &(*matches)[i];
        const char *group = linked ? BANDS[link_fit(e, m)] : m->category;
        studio_palette_entry *entry = studio_palette_add(entries, group, m->name);
        entry->icon = m->function ? KIT_ICON_FUNCTION
                    : m->host_event || m->event || m->ui_event ? KIT_ICON_EVENT
                    : m->variable                              ? KIT_ICON_FIELD
                                                               : 0;
        const char *about = match_about(s, e, m);
        snprintf(entry->about, sizeof entry->about, "%s", about ? about : "");
        snprintf(entry->meta, sizeof entry->meta, "%s", linked ? m->category : "");
        snprintf(entry->key, sizeof entry->key, "node:%s/%s", m->category, m->name);
        entry->index = i;
    }
    if (!query[0] && !linked) {
        recent_nodes_first(s, entries);
    }
    editor_set_ui_event_catalog(e, NULL, 0);
}

void studio_graph_palette_choose(studio *s, const studio_palette_entry *entry)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    if (!g->spawn_pending || !studio_graph_active(s)) {
        return;
    }
    read_catalog(s);
    studio_palette_remember(&s->prefs, entry->key);
    bool linked = e->spawn_link_pending;
    ed_pin_view from = e->spawn_link_pin;
    node *n = NULL;
    if (e->presentation &&
        strcmp(s->add_menu.matches[entry->index].desc->name, "Comment Box")) {
        if (!ed_source_spawn_match(e, &s->add_menu.matches[entry->index]) && e->connect_error[0]) {
            sg_show_error(s, "Can't add the node", e->connect_error);
        }
    } else {
        n = ed_spawn_match_at(e, &s->add_menu.matches[entry->index], g->spawn_x, g->spawn_y);
    }
    if (n) {
        sg_select_only(e, n);
        n->placed = true;
        if (linked) {
            place_on_wire(s, n, &from);
        }
    }
    editor_set_ui_event_catalog(e, NULL, 0);
    if (e->spawn_link_pending) {
        /* Nothing took the wire: a comment, or a spawn the editor refused. */
        ed_cancel_snapshot(e);
        e->spawn_link_pending = 0;
    }
    g->spawn_pending = false;
    studio_add_menu_close(s);
}

/* A search the palette closed without placing a node ends its spawn and
   lets go of the wire it was dragged from. */
void studio_graph_palette(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    studio_graph_ui_data(s, false);
    bool searching = s->palette.client == STUDIO_PALETTE_NODES;
    if (!g->spawn_pending || (searching && studio_graph_active(s))) {
        return;
    }
    if (searching) {
        studio_palette_close(s);
    }
    studio_add_menu_close(s);
    g->spawn_pending = false;
    if (e && e->spawn_link_pending) {
        ed_cancel_snapshot(e);
        e->spawn_link_pending = 0;
    }
}
