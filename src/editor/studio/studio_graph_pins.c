#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "graph/authoring/local_edit.h"
/* The pin menu (Unreal's): links, defaults, promotion, names, struct fields
 * and variadic pins. Each action is one undo step; an action may free the
 * node, so the menu stops after one runs. */
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_graph_internal.h"
#include "graph/authoring/optimize.h"
#include "graph/authoring/pin_edit.h"
#include "graph/document/graph_signature.h"
#include "graph/model/local.h"
#include "graph/nodes/component_nodes.h"
#include "support/array.h"
#include "types/array.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const pin_spec *pin_of(const node *n, int kind, int pin)
{
    return kind == ED_PIN_OUTPUT ? &n->output_pins[pin] : &n->input_pins[pin];
}

static bool touches(const edge *ed, const node *n, int kind, uint64_t pin_id)
{
    return kind == ED_PIN_OUTPUT ? ed->from_id == n->id && ed->from_pin_id == pin_id
                                 : ed->to_id == n->id && ed->to_pin_id == pin_id;
}

int sg_pin_links(const graph *gr, const node *n, int kind, int pin)
{
    uint64_t id = pin_of(n, kind, pin)->id;
    int links = 0;
    for (int i = 0; i < acount(gr->edges); i++) {
        links += touches(&gr->edges[i], n, kind, id);
    }
    return links;
}

void sg_break_links(studio *s, node *n, int kind, int pin)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (sg_graph_readonly(s) || !sg_pin_links(gr, n, kind, pin)) {
        return;
    }
    ed_snapshot_before(e);
    graph_break_pin_links(gr, n, kind == ED_PIN_OUTPUT, pin);
}

static void warn(studio *s, const char *title)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title });
}

/* ---------- Links ---------- */

/* One wire of several, named by the pin at its other end. */
static bool link_items(studio *s, sg_menu *m, node *n, int kind, int p)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    int links = sg_pin_links(gr, n, kind, p);
    const char *label = links > 1 ? "Break All Links" : "Break Link";
    const char *chord = studio_chord(s, "Alt+Click");
    if (sg_menu_item(m, "break", label, KIT_ICON_CLOSE, chord, readonly || !links)) {
        sg_break_links(s, n, kind, p);
        return true;
    }
    if (links < 2) {
        return false;
    }
    sg_submenu_begin(m, "links", "Break Link To", 0);
    uint64_t id = pin_of(n, kind, p)->id;
    bool broke = false;
    for (int i = 0; i < acount(gr->edges) && !broke; i++) {
        node *from, *to;
        int from_pin, to_pin;
        if (!touches(&gr->edges[i], n, kind, id) ||
            !sg_edge_ends(gr, &gr->edges[i], &from, &from_pin, &to, &to_pin)) {
            continue;
        }
        node *other = kind == ED_PIN_OUTPUT ? to : from;
        const pin_spec *far = kind == ED_PIN_OUTPUT ? &to->input_pins[to_pin]
                                                    : &from->output_pins[from_pin];
        char title[128], name[128], text[272], key[16];
        ed_node_title(e, other, title, sizeof title);
        studio_code_pin_label(s, other, far, name, sizeof name);
        snprintf(text, sizeof text, "%s \xe2\x80\xba %s", title, name);
        snprintf(key, sizeof key, "l%d", i);
        if (sg_menu_item(m, key, text, 0, NULL, readonly)) {
            ed_snapshot_before(e);
            graph_remove_edge(gr, from, from_pin, to, to_pin);
            broke = true;
        }
    }
    sg_submenu_end(m);
    return broke;
}

/* ---------- Values ---------- */

/* A call's input resets to its function's or macro's declared default. */
static pin_value reset_value(const studio *s, const node *n, int p)
{
    const pin_spec *declared = graph_signature_call_input(sg_graph_editor(s)->asset, n, p);
    return declared ? declared->default_value : graph_pin_reset_value(n, 0, p);
}

/* An unwired input holding a binding or a value other than its default. */
static bool resettable(const studio *s, const node *n, int p)
{
    const pin_spec *spec = &n->input_pins[p];
    node *source = NULL;
    int source_pin = -1;
    if (spec->type == PIN_FLOW ||
        graph_input_source(sg_active_graph(s), n, p, &source, &source_pin)) {
        return false;
    }
    pin_value reset = reset_value(s, n, p);
    return spec->binding || !pin_value_equal(&spec->default_value, &reset);
}

static void reset_pin(studio *s, node *n, int p)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    pin_value reset = reset_value(s, n, p);
    if (editor_presentation(e)) {
        if (editor_command_apply(
                e,
                (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                                  .data.set_pin_default = { .node_id = n->id,
                                                            .pin_index = p,
                                                            .value = &reset } }
            )) {
            sg_show_error(s, "Can't reset the value", e->connect_error);
        }
        return;
    }
    char *action = graph_collab_action_reset_pin(e, n, p, &reset);
    if (!action) {
        if (collab_active() && !collab_replaying()) {
            return;
        }
        ed_snapshot_before(e);
        graph_set_pin_binding(gr, n, 0, p, NULL);
        graph_set_pin_default(gr, n, 0, p, &reset);
        return;
    }
    collab_action_gate gate = collab_actions_begin(action);
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return;
    }
    ed_snapshot_before(e);
    bool success = graph_set_pin_binding(gr, n, 0, p, NULL) == 0 &&
                   graph_set_pin_default(gr, n, 0, p, &reset) == 0;
    collab_actions_finish(action, success);
    free(action);
}

/* Variables belong to event behaviors; a wildcard has no type to give one. */
static bool promotable(const studio *s, const pin_spec *spec)
{
    const editor *e = sg_graph_editor(s);
    return e->asset->domain == BEHAVIOR_DOMAIN_EVENT &&
           e->ui_context != EDITOR_UI_CONTEXT_SYSTEM && spec->type != PIN_FLOW;
}

/* The pin's label, numbered past the variables already named so. */
static void variable_name(const studio *s, const node *n, const pin_spec *spec, char *out, size_t size)
{
    const editor *e = sg_graph_editor(s);
    char label[96];
    studio_code_pin_label(s, n, spec, label, sizeof label);
    snprintf(out, size, "%s", label);
    for (int i = 2; behavior_asset_find_variable_by_name(e->asset, out); i++) {
        snprintf(out, size, "%s %d", label, i);
    }
}

/* The new Get or Set sits a column away with its socket level with the pin. */
static void place_facing(studio *s, node *placed, int placed_kind, int placed_pin, node *n, int kind, int p)
{
    const float gap = 60;
    float px, py, x, y;
    sg_socket_at(s, n, kind, p, &px, &py);
    sg_socket_at(s, placed, placed_kind, placed_pin, &x, &y);
    placed->x += (kind == ED_PIN_INPUT ? px - gap : px + gap) - x;
    placed->y += py - y;
}

/* A Set that runs after the node continues its flow level with it; one left
   unwired sits below the node, clear of the flow (Unreal wires it only to a
   free Then). */
static void place_set(studio *s, node *set, node *n, int p)
{
    const float gap = 30;
    node *source = NULL;
    int then = -1, order = 0;
    if (graph_exec_input_source(sg_active_graph(s), set, 0, &source, &then, &order)) {
        place_facing(s, set, ED_PIN_INPUT, 0, n, ED_PIN_OUTPUT, then);
        return;
    }
    float box[4];
    place_facing(s, set, ED_PIN_INPUT, 1, n, ED_PIN_OUTPUT, p);
    sg_node_box(s, n, box);
    set->y = box[1] + box[3] + gap;
}

static void promote(studio *s, node *n, int kind, int p)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    bool output = kind == ED_PIN_OUTPUT;
    char name[128];
    variable_name(s, n, pin_of(n, kind, p), name, sizeof name);
    int before = acount(gr->nodes);
    e->pending_snapshot = ed_snapshot(e);
    if (!behavior_asset_promote_pin_to_variable(e->asset, n, output, p, name)) {
        ed_cancel_snapshot(e);
        warn(s, "Can't promote this pin");
        return;
    }
    ed_commit_snapshot(e);
    if (acount(gr->nodes) == before) {
        return;
    }
    node *access = alast(gr->nodes);
    sg_calculations_invalidate(s);
    if (output) {
        place_set(s, access, n, p);
    } else {
        place_facing(s, access, ED_PIN_OUTPUT, 0, n, kind, p);
    }
    access->placed = true;
}

/* Names an output as a graph local (Unreal's named reroute): its uses read
   the name instead of drawing wires. Properties renames it. */
static void name_value(studio *s, node *n, int p)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    char name[128];
    graph_local_name(gr, n, p, name, sizeof name);
    e->pending_snapshot = ed_snapshot(e);
    if (!graph_create_local(gr, n, p, name)) {
        ed_cancel_snapshot(e);
        warn(s, "Can't name this value");
        return;
    }
    ed_commit_snapshot(e);
    sg_select_only(e, n);
}

/* Name Value, or Remove Name once named. */
static bool name_items(studio *s, sg_menu *m, node *n, int p)
{
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    const graph_local *local = graph_output_local(gr, n, p);
    if (local) {
        if (!sg_menu_item(m, "unname", "Remove Name", KIT_ICON_LOCAL, NULL, readonly)) {
            return false;
        }
        ed_snapshot_before(sg_graph_editor(s));
        graph_expand_local(gr, local->id);
        return true;
    }
    bool unused = !sg_pin_links(gr, n, ED_PIN_OUTPUT, p);
    if (!sg_menu_item(m, "name", "Name Value", KIT_ICON_LOCAL, NULL, readonly || unused)) {
        return false;
    }
    name_value(s, n, p);
    return true;
}

static bool value_items(studio *s, sg_menu *m, node *n, int kind, int p)
{
    bool readonly = sg_graph_readonly(s);
    if (kind == ED_PIN_INPUT) {
        bool fixed = readonly || !resettable(s, n, p);
        if (sg_menu_item(m, "reset", "Reset to Default", KIT_ICON_RESET, NULL, fixed)) {
            reset_pin(s, n, p);
            return true;
        }
    }
    if (promotable(s, pin_of(n, kind, p)) &&
        sg_menu_item(m, "promote", "Promote to Variable", KIT_ICON_VARIABLE, NULL, readonly)) {
        promote(s, n, kind, p);
        return true;
    }
    return kind == ED_PIN_OUTPUT && name_items(s, m, n, p);
}

/* ---------- Struct fields ----------
   A struct output splits into field outputs beside it, which any node can
   evaluate; recombining hides the unwired ones. Set Component's Value splits
   into its settable field inputs. */

static int exposed_fields(const node *n, int p)
{
    int count = 0;
    for (int i = 0; i < n->output_count; i++) {
        count += n->output_pins[i].field_source == n->output_pins[p].id;
    }
    return count;
}

/* The struct output an exposed field reads, or -1. */
static int field_parent(const node *n, int p)
{
    for (int i = 0; i < n->output_count; i++) {
        if (n->output_pins[p].field_source &&
            n->output_pins[i].id == n->output_pins[p].field_source) {
            return i;
        }
    }
    return -1;
}

static bool splittable(const node *n, int kind, int p)
{
    if (kind == ED_PIN_INPUT) {
        return component_node_kind_of(n->desc) == COMPONENT_NODE_SET &&
               !component_node_split(n) && n->input_pins[p].type == PIN_STRUCT;
    }
    const type_desc *type = graph_output_struct(n, p);
    return type && exposed_fields(n, p) < type->field_count;
}

/* Set Component's field inputs, pins split by automation, and exposed
   fields or the struct output that shows them. */
static bool recombinable(const node *n, int kind, int p)
{
    if (kind == ED_PIN_OUTPUT) {
        return n->output_pins[p].split_parent || exposed_fields(n, p) ||
               field_parent(n, p) >= 0;
    }
    bool field = component_node_kind_of(n->desc) == COMPONENT_NODE_SET &&
                 component_node_split(n) && p > component_node_setting(n) + 1;
    return field || n->input_pins[p].split_parent;
}

/* Recombining exposed fields keeps the wired ones, so it needs an unwired one. */
static bool hides_field(const graph *gr, const node *n, int kind, int p)
{
    if (kind == ED_PIN_INPUT || n->output_pins[p].split_parent) {
        return true;
    }
    int source = exposed_fields(n, p) ? p : field_parent(n, p);
    for (int i = 0; i < n->output_count; i++) {
        if (n->output_pins[i].field_source == n->output_pins[source].id &&
            !sg_pin_links(gr, n, ED_PIN_OUTPUT, i)) {
            return true;
        }
    }
    return false;
}

static void recombine(graph *gr, node *n, int kind, int p)
{
    if (kind == ED_PIN_INPUT || n->output_pins[p].split_parent) {
        graph_recombine_pin(gr, n, kind == ED_PIN_OUTPUT, p);
        return;
    }
    graph_hide_output_fields(gr, n, exposed_fields(n, p) ? p : field_parent(n, p));
}

static bool struct_items(studio *s, sg_menu *m, node *n, int kind, int p)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    if (splittable(n, kind, p) &&
        sg_menu_item(m, "split", "Split Struct Pin", KIT_ICON_MAXIMIZE, NULL, readonly)) {
        ed_snapshot_before(e);
        if (kind == ED_PIN_INPUT) {
            graph_split_pin(gr, n, 0, p);
        } else {
            graph_expose_output_fields(gr, n, p);
        }
        return true;
    }
    bool idle = readonly || !hides_field(gr, n, kind, p);
    if (recombinable(n, kind, p) &&
        sg_menu_item(m, "recombine", "Recombine Struct Pin", KIT_ICON_FRAME, NULL, idle)) {
        ed_snapshot_before(e);
        recombine(gr, n, kind, p);
        return true;
    }
    return false;
}

/* ---------- Variadic pins ---------- */

void sg_add_run_pin(studio *s, node *n)
{
    ed_snapshot_before(sg_graph_editor(s));
    graph_add_run_pin(sg_active_graph(s), n);
}

/* ---------- Renaming expression inputs ---------- */

/* An expression's input is named in its source; renaming rewrites the source
   and keeps the wire (Unreal renames a Math Expression's pins only by
   editing its text, which drops the wire). */
bool sg_pin_renameable(const studio *s, const node *n, int kind, int p)
{
    return n->desc == &NODE_BehaviorExpression && kind == ED_PIN_INPUT &&
           n->input_pins[p].kind == PIN_DATA && !sg_graph_readonly(s);
}

void sg_rename_pin(studio *s, const node *n, int p)
{
    s->graph.pin_edit = n->input_pins[p].id;
    s->graph.pin_focus = true;
}

static void rename_input(studio *s, node *n, int p, const char *name)
{
    char from[128];
    snprintf(from, sizeof from, "%s", n->input_pins[p].name);
    if (!name[0] || !strcmp(from, name)) {
        return;
    }
    behavior_expression_error error = { 0 };
    if (editor_rename_behavior_expression_input(sg_graph_editor(s), n, from, name, &error) !=
        0) {
        warn(s, error.message[0] ? error.message : "Can't rename this input");
    }
}

/* The field that replaces the pin's label while it is renamed; Enter or
   leaving it commits. */
void sg_emit_pin_edit(studio *s, node *n, int p)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    char name[128];
    snprintf(name, sizeof name, "%s", n->input_pins[p].name);
    bool committed = kit_text_field(k, "rename", name, sizeof name, NULL, 0);
    if (g->pin_focus) {
        kit_focus(k);
        g->pin_focus = false;
    } else if (!kit_item_active(k)) {
        g->pin_edit = 0;
    }
    if (committed) {
        g->pin_edit = 0;
        rename_input(s, n, p, name);
    }
}

void sg_pin_menu(studio *s, sg_menu *m, node *n, int kind, int p)
{
    char label[128];
    studio_code_pin_label(s, n, pin_of(n, kind, p), label, sizeof label);
    sg_menu_title(m, label);
    if (sg_pin_renameable(s, n, kind, p) &&
        sg_menu_item(m, "rename", "Rename", KIT_ICON_EDIT, "F2", false)) {
        sg_rename_pin(s, n, p);
        return;
    }
    if (link_items(s, m, n, kind, p)) {
        return;
    }
    if (pin_of(n, kind, p)->kind == PIN_DATA &&
        (value_items(s, m, n, kind, p) || struct_items(s, m, n, kind, p))) {
        return;
    }
    bool output = kind == ED_PIN_OUTPUT;
    if (graph_run_pin_removable(n, output, p) &&
        sg_menu_item(m, "remove", "Remove Pin", KIT_ICON_TRASH, NULL, sg_graph_readonly(s))) {
        ed_snapshot_before(sg_graph_editor(s));
        graph_remove_run_pin(sg_active_graph(s), n, output, p);
    }
}
