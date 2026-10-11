#include "graph/compiler/validation.h"
#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "editor/collab/collab.h"
#include "editor/graph/eval_view.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/serialize.h"
#include "graph/model/pin_conv.h"
#include "graph/model/registry.h"
#include "graph/nodes/conv_nodes.h"

#include <assert.h>

static void ed_format_text_repair(editor *e, node *n);

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ed_select_appended_visible(editor *e, int first)
{
    for (int i = first; i < acount(e->active_graph->nodes); i++) {
        node *n = e->active_graph->nodes[i];
        if (!graph_node_is_inline(e->active_graph, n)) {
            ed_add_selected(e, n);
        }
    }
}

behavior_asset *ed_snapshot(const editor *e)
{
    if (e->presentation) {
        return NULL; /* Source and layout history belong to the source document. */
    }
    return behavior_asset_clone(e->asset);
}

void ed_commit_snapshot(editor *e)
{
    if (!e->pending_snapshot) {
        return;
    }
    e->edit_pending = 1;
    if (!collab_record_history()) {
        behavior_asset_free(e->pending_snapshot);
        e->pending_snapshot = NULL;
        return;
    }
    ed_push_asset(&e->undo, e->pending_snapshot);
    e->pending_snapshot = NULL;
    ed_free_asset_stack(&e->redo);
}

void ed_snapshot_before(editor *e)
{
    e->edit_pending = 1;
    if (!collab_record_history()) {
        return;
    }
    behavior_asset *snapshot = ed_snapshot(e);
    if (!snapshot) {
        return;
    }
    ed_push_asset(&e->undo, snapshot);
    ed_free_asset_stack(&e->redo);
}

void ed_cancel_snapshot(editor *e)
{
    behavior_asset_free(e->pending_snapshot);
    e->pending_snapshot = NULL;
}

void ed_show_error(editor *e, const char *message)
{
    snprintf(e->connect_error, sizeof e->connect_error, "%s", message);
}

void ed_clear_connect_error(editor *e)
{
    e->connect_error[0] = '\0';
}

void ed_destroy_preview(editor *e)
{
    editor_set_inspector(e, NULL, 0);
}

void ed_release_document(editor *e)
{
    if (e->presentation) {
        authoring_source_presentation_free(e->presentation);
    } else {
        behavior_asset_free(e->asset);
    }
    e->presentation = NULL;
    e->asset = NULL;
}

/* Every document swap releases previews before retiring its graph storage. */
static void adopt_owned(editor *e, behavior_asset *fresh, authoring_source_presentation *presentation)
{
    ed_destroy_preview(e);
    uint64_t active_graph_id = e->active_graph ? e->active_graph->graph_id : 0;
    /* The value preview borrows nodes from the old asset. */
    eval_view_free(e->ev);
    e->ev = eval_view_new();
    eval_view_set_formatter(e->ev, e->value_formatter, e->value_formatter_user);
    ed_release_document(e);
    e->asset = fresh;
    e->presentation = presentation;
    e->active_graph = behavior_asset_find_graph(e->asset, active_graph_id);
    if (!e->active_graph) {
        e->active_graph = e->asset->graph;
    }
    e->arrange_pending = false;
    compile_report_free(&e->compile);
    compile_report_init(&e->compile);
    behavior_asset_free(e->compiled);
    e->compiled = NULL;
    ed_clear_selection(e);
    e->front_node = NULL;
    e->spawn_link_pending = 0;
    e->log_count = 0;
    e->edit_pending = 1;
}

int ed_adopt_behavior_asset_with_identity(editor *e, const behavior_asset *source, uint64_t owner_identity)
{
    behavior_asset *fresh = behavior_asset_clone(source);
    if (!fresh) {
        return -1;
    }
    if (owner_identity) {
        fresh->identity = owner_identity;
    }
    adopt_owned(e, fresh, NULL);
    return 0;
}

int ed_adopt_source_presentation(editor *e, const authoring_source_presentation *source)
{
    authoring_source_presentation *copy = authoring_source_presentation_clone(source);
    adopt_owned(e, authoring_source_presentation_view(copy), copy);
    ed_cancel_snapshot(e);
    ed_free_asset_stack(&e->undo);
    ed_free_asset_stack(&e->redo);
    return 0;
}

int ed_adopt_behavior_asset(editor *e, const behavior_asset *source)
{
    return ed_adopt_behavior_asset_with_identity(
        e,
        source,
        e && e->asset ? e->asset->identity : 0
    );
}

void ed_undo(editor *e)
{
    if (acount(e->undo) == 0) {
        return;
    }
    behavior_asset *target = apop(e->undo);
    behavior_asset *current = behavior_asset_clone(e->asset);
    if (current) {
        ed_push_asset(&e->redo, current);
    }
    ed_adopt_behavior_asset(e, target);
    behavior_asset_free(target);
}

void ed_redo(editor *e)
{
    if (acount(e->redo) == 0) {
        return;
    }
    behavior_asset *target = apop(e->redo);
    behavior_asset *current = behavior_asset_clone(e->asset);
    if (current) {
        ed_push_asset(&e->undo, current);
    }
    ed_adopt_behavior_asset(e, target);
    behavior_asset_free(target);
}

bool editor_can_undo(const editor *e)
{
    return e && acount(e->undo) > 0;
}

bool editor_can_redo(const editor *e)
{
    return e && acount(e->redo) > 0;
}

uint64_t *ed_source_selected_origins(const editor *e)
{
    assert(e->presentation);
    uint64_t *ids = NULL;
    for (int i = 0; i < acount(e->selected); i++) {
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, e->selected[i]->id, 0
        );
        if (!origin) {
            afree(ids);
            return NULL;
        }
        apush(ids, origin->identity);
    }
    return ids;
}
uint64_t ed_source_scope_origin(const editor *e)
{
    assert(e->presentation);
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation); i++) {
        const source_presentation_origin *origin = authoring_source_presentation_origin_at(
            e->presentation, i
        );
        if (origin->graph == e->active_graph->graph_id && !origin->pin &&
            origin->kind == SOURCE_PRESENTATION_FUNCTION) {
            return origin->identity;
        }
    }
    return 0;
}
static void source_selection_edit(editor *e, authoring_edit_kind kind)
{
    uint64_t *ids = ed_source_selected_origins(e);
    if (!ids && acount(e->selected)) {
        ed_show_error(e, "Selection has no current editable source origins");
        return;
    }
    for (int i = 0; i < acount(e->selected_links); i++) {
        const ed_link_ref *link = &e->selected_links[i];
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, link->to_id, link->to_pin_id
        );
        if (!origin || origin->flow || kind != AUTHORING_EDIT_DELETE_SELECTION) {
            afree(ids);
            ed_show_error(e, "Select whole statements or value inputs for this gesture");
            return;
        }
        bool found = false;
        for (int j = 0; j < acount(ids); j++) {
            found |= ids[j] == origin->identity;
        }
        if (!found) {
            apush(ids, origin->identity);
        }
    }
    if (!acount(ids)) {
        ed_show_error(e, "Select source statements or value inputs to edit");
        return;
    }
    authoring_edit_request request = {
        .kind = kind,
        .generation = authoring_source_edit_generation(e->presentation),
        .origin = ids[0],
        .origins = ids,
        .origin_count = acount(ids)
    };
    ed_source_edit(e, &request);
    afree(ids);
}

void ed_delete_selected(editor *e)
{
    if (e->presentation) {
        source_selection_edit(e, AUTHORING_EDIT_DELETE_SELECTION);
        return;
    }
    if (acount(e->selected) == 0 && acount(e->selected_links) == 0) {
        return;
    }
    /* Before any removal, which can fold a shared source into a survivor. */
    uint64_t *ids = NULL;
    for (int i = 0; i < acount(e->selected); i++) {
        graph_inline_tree(e->active_graph, e->selected[i], &ids);
    }
    ed_snapshot_before(e);
    for (int i = acount(e->selected_links) - 1; i >= 0; i--) {
        int index = ed_edge_index(e, &e->selected_links[i]);
        if (index >= 0) {
            edge ed = e->active_graph->edges[index];
            node *from = graph_node_by_id(e->active_graph, ed.from_id);
            node *to = graph_node_by_id(e->active_graph, ed.to_id);
            int from_pin = -1, to_pin = -1, fo = 0, to_out = 0;
            if (from && to &&
                graph_pin_by_id(from, ed.from_pin_id, &fo, &from_pin) && fo &&
                graph_pin_by_id(to, ed.to_pin_id, &to_out, &to_pin) && !to_out) {
                if (graph_remove_edge(e->active_graph, from, from_pin, to, to_pin) == 0) {
                    ed_format_text_repair(e, to);
                }
            }
        }
    }

    for (int i = acount(ids) - 1; i >= 0; i--) {
        node *target = graph_node_by_id(e->active_graph, ids[i]);
        if (target) {
            int removed = graph_remove_node(e->active_graph, target);
            assert(removed == 0);
            (void)removed;
        }
    }
    ed_reset_node_interaction(e);
    afree(ids);
}

void ed_duplicate_selected(editor *e)
{
    if (e->presentation) {
        source_selection_edit(e, AUTHORING_EDIT_DUPLICATE_SELECTION);
        return;
    }
    if (acount(e->selected) == 0) {
        return;
    }
    av_doc *doc = av_doc_new();
    av_value *selection = graph_nodes_to_value(doc, e->active_graph, e->selected, acount(e->selected));
    if (!selection) {
        av_doc_free(doc);
        return;
    }
    ed_snapshot_before(e);
    int first = acount(e->active_graph->nodes);
    if (graph_append_value(e->active_graph, selection, 24.0f, 24.0f) == 0) {
        ed_clear_selection(e);
        ed_select_appended_visible(e, first);
    }
    av_doc_free(doc);
}

/* CAT-38: a Format Text node always carries one trailing Generic argument
 * pin; connecting the last pin adopts its type and reveals the next spare.
 * Capped so the group cannot grow without bound. */
#define ED_FMT_TEXT_MAX_ARGS 12

static int ed_format_text_generic(const node *n, int index)
{
    if (!n->desc || strcmp(n->desc->name, "Format Text") != 0 || index < 1 ||
        index >= n->input_count) {
        return 0;
    }
    const pin_spec *pin = &n->input_pins[index];
    return pin->kind == PIN_DATA && pin->type == PIN_FLOW &&
           (pin->flags & PIN_FLAG_WILDCARD);
}

static void ed_append_generic_arg(editor *e, node *n)
{
    if (!n->desc || strcmp(n->desc->name, "Format Text") != 0) {
        return;
    }
    if (n->input_count - 1 >= ED_FMT_TEXT_MAX_ARGS) {
        return;
    }
    char name[32];
    snprintf(name, sizeof name, "Arg%d", n->input_count - 1);
    pin_spec pin = { .name = name, .type = PIN_FLOW, .kind = PIN_DATA, .flags = PIN_FLAG_WILDCARD };
    graph_add_pin(e->active_graph, n, 0, &pin);
}

static void ed_format_text_repair(editor *e, node *n)
{
    if (!n->desc || strcmp(n->desc->name, "Format Text") != 0) {
        return;
    }
    int generic_count = 0;
    for (int i = 1; i < n->input_count; i++) {
        generic_count += ed_format_text_generic(n, i);
    }
    for (int i = n->input_count - 1; i > 0 && generic_count > 1; i--) {
        if (!ed_format_text_generic(n, i)) {
            continue;
        }
        if (graph_remove_pin(e->active_graph, n, 0, i) == 0) {
            generic_count--;
        }
    }
}

static void ed_format_text_ensure_spare(editor *e, node *n)
{
    if (!n->desc || strcmp(n->desc->name, "Format Text") != 0) {
        return;
    }
    for (int i = 1; i < n->input_count; i++) {
        if (ed_format_text_generic(n, i)) {
            return;
        }
    }
    ed_append_generic_arg(e, n);
}

int ed_connection_specs_valid(const pin_spec *output, const pin_spec *input, pin_kind expected_kind)
{
    if ((output->flags | input->flags) & PIN_FLAG_SETTING) {
        return 0;
    }
    if (expected_kind == PIN_DATA && (input->flags & PIN_FLAG_REFERENCE)) {
        if (!(output->flags & PIN_FLAG_REFERENCE) ||
            (!(input->flags & PIN_FLAG_WILDCARD) &&
             (output->type != input->type || output->container != input->container))) {
            return 0;
        }
    }
    if (output->kind != expected_kind || input->kind != expected_kind) {
        return 0;
    }
    if (expected_kind != PIN_DATA || (output->flags & PIN_FLAG_WILDCARD) ||
        (input->flags & PIN_FLAG_WILDCARD) || pin_types_compatible(output, input)) {
        return 1;
    }
    return conversion_for_pins(output, input) != NULL;
}

/* Drops the inline tree a new wire replaces, as it would a literal. */
static void ed_drop_replaced_source(editor *e, const ed_pin_view *output, const ed_pin_view *input, uint64_t replaced)
{
    node *source = NULL;
    int source_pin = -1;
    node *old = graph_node_by_id(e->active_graph, replaced);
    if (old &&
        graph_input_source(e->active_graph, input->n, input->index, &source, &source_pin) &&
        source == output->n && old != output->n) {
        int removed = graph_remove_node_tree(e->active_graph, old);
        assert(removed == 0);
        (void)removed;
    }
}

static const source_presentation_origin *source_statement(editor *e, const node *n)
{
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation);
         i++) {
        const source_presentation_origin
            *origin = authoring_source_presentation_origin_at(e->presentation, i);
        if (!origin->pin && origin->graph == e->active_graph->graph_id &&
            origin->node == n->id && origin->kind == SOURCE_PRESENTATION_STATEMENT) {
            return origin;
        }
    }
    return NULL;
}
static bool source_connect(editor *e, const ed_pin_view *output, const ed_pin_view *input)
{
    const pin_spec *out = &output->n->output_pins[output->index];
    const pin_spec *in = &input->n->input_pins[input->index];
    if (out->kind != in->kind) {
        ed_show_error(e, "Connect matching value or flow ports");
        return false;
    }
    authoring_edit_request request = {
        .generation = authoring_source_edit_generation(e->presentation)
    };
    if (out->kind == PIN_EXEC) {
        const source_presentation_origin *producer = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, output->n->id, out->id
        );
        if (producer && producer->kind == SOURCE_PRESENTATION_EXPRESSION) {
            producer = source_statement(e, output->n);
        }
        const source_presentation_origin *consumer = source_statement(e, input->n);
        if (!producer || !consumer || consumer->operation == SCRIPT_STMT_BLOCK) {
            ed_show_error(e, "Move a whole statement to a source flow position or body");
            return false;
        }
        if (producer->kind == SOURCE_PRESENTATION_FUNCTION ||
            (producer->kind == SOURCE_PRESENTATION_STATEMENT && producer->operation == SCRIPT_STMT_BLOCK)) {
            request.kind = AUTHORING_EDIT_MOVE_INTO_BODY;
            request.origin = producer->identity;
            request.producer = consumer->identity;
        } else if (producer->kind == SOURCE_PRESENTATION_STATEMENT) {
            request.kind = AUTHORING_EDIT_MOVE_AFTER;
            request.origin = consumer->identity;
            request.producer = producer->identity;
        } else {
            ed_show_error(e, "Flow output has no editable source position");
            return false;
        }
    } else {
        const source_presentation_origin *producer = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, output->n->id, out->id
        );
        const source_presentation_origin *consumer = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, input->n->id, in->id
        );
        if (!producer || !consumer || !producer->output || consumer->output) {
            ed_show_error(e, "Connection has no current source expression");
            return false;
        }
        request.kind = AUTHORING_EDIT_CONNECT;
        request.origin = consumer->identity;
        request.producer = producer->identity;
        request.producer_result = producer->result_index;
    }
    return ed_source_edit(e, &request);
}

int ed_connect(editor *e, const ed_pin_view *a, const ed_pin_view *b)
{
    ed_clear_connect_error(e);
    const ed_pin_view *output = a->kind == ED_PIN_OUTPUT ? a : b;
    const ed_pin_view *input = a->kind == ED_PIN_INPUT ? a : b;
    if (output->kind != ED_PIN_OUTPUT || input->kind != ED_PIN_INPUT) {
        return 0;
    }
    if (e->presentation) {
        return source_connect(e, output, input);
    }
    const pin_spec *out_spec = &output->n->output_pins[output->index];
    const pin_spec *in_spec = &input->n->input_pins[input->index];
    node *replaced = NULL;
    int replaced_pin = -1;
    uint64_t replaced_id = ed_inline_source_for(e, input->n, input->index, &replaced, &replaced_pin)
                             ? replaced->id
                             : 0;
    const char *out_name = ed_pin_type_name(out_spec);
    const char *in_name = ed_pin_type_name(in_spec);
    if (!ed_connection_specs_valid(out_spec, in_spec, out_spec->kind)) {
        snprintf(e->connect_error, sizeof e->connect_error, "Cannot connect %s output to %s input", out_name, in_name);
        return 0;
    }
    /* PIN-11: a Generic data input accepts any data peer and adopts its
     * type in core; it is not a type mismatch and never toasts. */
    int generic_target = out_spec->kind == PIN_DATA &&
                         (in_spec->flags & PIN_FLAG_WILDCARD);
    if (out_spec->kind == PIN_DATA && !generic_target &&
        (!(out_spec->flags & PIN_FLAG_WILDCARD) ||
         (in_spec->type == PIN_STR && conversion_for_pins(out_spec, in_spec))) &&
        !pin_types_compatible(out_spec, in_spec)) {
        const node_desc *conv = conversion_for_pins(out_spec, in_spec);
        assert(conv);
        int connected = graph_connect_data_conversion(
                            e->active_graph,
                            output->n,
                            output->index,
                            input->n,
                            input->index,
                            conv
                        ) == 0;
        if (connected) {
            ed_drop_replaced_source(e, output, input, replaced_id);
            (void)ed_simplify_component_getter(
                e,
                output->n,
                output->index,
                input->n,
                input->index,
                conv
            );
        }
        return connected;
    }
    if (out_spec->kind == PIN_EXEC) {
        int order = output->n->desc->flow_kind == NODE_FLOW_SEQUENCE ? output->index
                                                                     : 0;
        return graph_connect_exec(
                   e->active_graph,
                   output->n,
                   output->index,
                   input->n,
                   input->index,
                   order
               ) == 0;
    }
    node *old_source = NULL;
    int old_source_pin = -1;
    const pin_binding *old_binding = NULL;
    pin_binding old_binding_copy = { 0 };
    const node_desc *old_conversion = NULL;
    bool reference_input = in_spec->flags & PIN_FLAG_REFERENCE;
    behavior_asset *reference_before = reference_input ? ed_snapshot(e) : NULL;
    if (reference_input) {
        old_binding = graph_pin_binding(e->active_graph, input->n, 0, input->index);
        if (old_binding) {
            old_binding_copy = *old_binding;
        }
        if (graph_input_source(e->active_graph, input->n, input->index, &old_source, &old_source_pin)) {
            old_conversion = graph_input_conversion(
                e->active_graph,
                input->n,
                input->index
            );
        }
    }
    int ok = graph_connect_data(
                 e->active_graph,
                 output->n,
                 output->index,
                 input->n,
                 input->index
             ) == 0;
    if (!ok) {
        if (reference_before) {
            (void)ed_adopt_behavior_asset(e, reference_before);
            behavior_asset_free(reference_before);
        }
        return 0;
    }
    if (ok && generic_target) {
        ed_format_text_ensure_spare(e, input->n);
    }
    if (ok) {
        /* The wired input changed type, so its expression regenerates first;
           the source's result, then, below. */
        node *expression = (input->n->desc == &NODE_CallPureFunction ||
                            input->n->desc == &NODE_BehaviorExpression)
                             ? input->n
                             : NULL;
        if (!expression && (output->n->desc == &NODE_CallPureFunction ||
                            output->n->desc == &NODE_BehaviorExpression)) {
            expression = output->n;
        }
        if (expression &&
            behavior_asset_find_graph(e->asset, expression->reference_id) &&
            behavior_asset_find_graph(e->asset, expression->reference_id)->math_expression) {
            behavior_expression_error error;
            if (behavior_expression_refresh_mode(e->asset, expression, GRAPH_EXPRESSION_NONE, &error) !=
                0) {
                if (reference_before) {
                    (void)ed_adopt_behavior_asset(e, reference_before);
                } else if (reference_input) {
                    (void)graph_remove_edge(
                        e->active_graph,
                        output->n,
                        output->index,
                        input->n,
                        input->index
                    );
                    if (old_source) {
                        (void)graph_connect_data_conversion(
                            e->active_graph,
                            old_source,
                            old_source_pin,
                            input->n,
                            input->index,
                            old_conversion
                        );
                    }
                    if (old_binding) {
                        (void)graph_set_pin_binding(
                            e->active_graph,
                            input->n,
                            0,
                            input->index,
                            &old_binding_copy
                        );
                    }
                }
                ed_show_error(e, error.message);
                behavior_asset_free(reference_before);
                return 0;
            }
        }
        graph *source = output->n != expression &&
                                (output->n->desc == &NODE_CallPureFunction ||
                                 output->n->desc == &NODE_BehaviorExpression)
                          ? behavior_asset_find_graph(e->asset, output->n->reference_id)
                          : NULL;
        if (source && source->math_expression) {
            behavior_expression_error error;
            (void)behavior_expression_refresh_mode(e->asset, output->n, GRAPH_EXPRESSION_NONE, &error);
        }
        ed_drop_replaced_source(e, output, input, replaced_id);
        if (!reference_input) {
            (void)ed_simplify_component_getter(
                e,
                output->n,
                output->index,
                input->n,
                input->index,
                NULL
            );
        }
    }
    behavior_asset_free(reference_before);
    return ok;
}

/* Replaces a wire with two through n: the wire's source into n's input `in`
 * and n's output `out` into its target. Both connections are validated
 * before the wire is removed; a refusal leaves the graph as it was. Returns
 * whether n was inserted. */
bool ed_insert_on_wire(editor *e, int edge_index, node *n, int in, int out)
{
    if (e->presentation) {
        ed_show_error(e, "Insert this statement through the source editor");
        return false;
    }
    graph *g = e->active_graph;
    const edge *ed = &g->edges[edge_index];
    node *from = graph_node_by_id(g, ed->from_id),
         *to = graph_node_by_id(g, ed->to_id);
    int from_pin = -1, to_pin = -1, from_out = 0, to_out = 0;
    bool resolved = from && to &&
                    graph_pin_by_id(from, ed->from_pin_id, &from_out, &from_pin) &&
                    graph_pin_by_id(to, ed->to_pin_id, &to_out, &to_pin);
    assert(resolved && from_out && !to_out);
    (void)resolved;
    pin_kind kind = ed->kind == GRAPH_EDGE_EXEC ? PIN_EXEC : PIN_DATA;
    ed_clear_connect_error(e);
    if (!behavior_asset_mutable(e->asset) ||
        !ed_connection_specs_valid(&from->output_pins[from_pin], &n->input_pins[in], kind) ||
        !ed_connection_specs_valid(&n->output_pins[out], &to->input_pins[to_pin], kind)) {
        return false;
    }
    behavior_asset *before = ed_snapshot(e);
    uint64_t id = n->id;
    graph_remove_edge(g, from, from_pin, to, to_pin);
    ed_pin_view a = { from, ED_PIN_OUTPUT, from_pin };
    ed_pin_view b = { n, ED_PIN_INPUT, in };
    ed_pin_view c = { n, ED_PIN_OUTPUT, out };
    ed_pin_view d = { to, ED_PIN_INPUT, to_pin };
    bool inserted = ed_connect(e, &a, &b) && ed_connect(e, &c, &d);
    if (!inserted) {
        /* Only a check made while connecting gets here, such as an
           expression refusing its new input's type. */
        int restored = ed_adopt_behavior_asset(e, before);
        assert(restored == 0);
        (void)restored;
        ed_select_node(e, graph_node_by_id(e->active_graph, id), false);
    }
    behavior_asset_free(before);
    return inserted;
}

static bool ed_expression_call_stale(const graph *caller, const node *call)
{
    for (int i = 0; i < call->input_count; i++) {
        if (call->input_pins[i].kind != PIN_DATA) {
            continue;
        }
        node *source;
        int source_pin;
        if (!graph_input_source(caller, call, i, &source, &source_pin)) {
            continue;
        }
        const pin_spec *output = &source->output_pins[source_pin];
        const pin_spec *input = &call->input_pins[i];
        if (input->flags & PIN_FLAG_REFERENCE) {
            if (!(output->flags & PIN_FLAG_REFERENCE) ||
                (!(input->flags & PIN_FLAG_WILDCARD) &&
                 !pin_types_compatible(output, input))) {
                return true;
            }
            pin_binding binding = { 0 };
            if (graph_resolve_reference((graph *)caller, source, source_pin, &binding) !=
                0) {
                return true;
            }
            continue;
        }
        const node_desc *conversion = graph_input_conversion(caller, call, i);
        if (conversion ? conversion_matches_pins(output, input, conversion)
                       : pin_types_compatible(output, input)) {
            continue;
        }
        return true;
    }
    /* A bound reference is an established capture, not a stale direct edge.
       Rebuilding it during compilation changes authored expression contracts. */
    return false;
}

void ed_refresh_stale_behavior_expressions(editor *e)
{
    if (e->presentation) {
        return;
    }
    /* Loading or an older edit can leave a direct edge on a generic expression
     * input after its public pin has fallen back to double. Repair that
     * boundary before the read-only compiler pass reports it. */
    for (int i = 0; i < acount(e->asset->graphs); i++) {
        graph *caller = e->asset->graphs[i];
        for (int j = 0; j < acount(caller->nodes); j++) {
            node *call = caller->nodes[j];
            if (call->desc != &NODE_CallPureFunction &&
                call->desc != &NODE_BehaviorExpression) {
                continue;
            }
            graph *generated = behavior_asset_find_graph(e->asset, call->reference_id);
            if (!generated || !generated->math_expression ||
                !ed_expression_call_stale(caller, call)) {
                continue;
            }
            behavior_expression_error error = { 0 };
            (void)behavior_expression_refresh_mode(e->asset, call, generated->expression_mode, &error);
        }
    }
}
