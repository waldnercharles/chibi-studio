#include "graph/compiler/validation.h"
#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/graph/eval_view.h"
#include "editor/graph/search.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/serialize.h"
#include "graph/shader/shader_graph.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int editor_command_apply_direct(editor *e, editor_command command)
{
    if (!e || !e->asset ||
        (e->active_graph->math_expression && command.kind != EDITOR_COMMAND_OPEN_GRAPH &&
         command.kind != EDITOR_COMMAND_RENAME_GRAPH)) {
        return -1;
    }
    if (e->presentation && command.kind == EDITOR_COMMAND_CREATE_GRAPH) {
        int kind = command.data.create_graph.graph_kind;
        if (kind != GRAPH_FUNCTION && kind != GRAPH_MACRO) {
            ed_show_error(e, "Create a source function or macro declaration");
            return -1;
        }
        authoring_edit_request request = {
            .kind = kind == GRAPH_MACRO ? AUTHORING_EDIT_CREATE_MACRO : AUTHORING_EDIT_CREATE_FUNCTION,
            .generation = authoring_source_edit_generation(e->presentation),
            .name = command.data.create_graph.name
        };
        return ed_source_edit(e, &request) ? 0 : -1;
    }
    if (e->presentation && command.kind == EDITOR_COMMAND_RENAME_GRAPH) {
        uint64_t identity = 0;
        for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation); i++) {
            const source_presentation_origin *origin = authoring_source_presentation_origin_at(e->presentation, i);
            if (origin->graph == command.data.rename_graph.graph_id && !origin->pin &&
                origin->kind == SOURCE_PRESENTATION_FUNCTION) {
                identity = origin->identity;
                break;
            }
        }
        authoring_edit_request request = {
            .kind = AUTHORING_EDIT_RENAME_DECLARATION,
            .generation = authoring_source_edit_generation(e->presentation),
            .origin = identity,
            .name = command.data.rename_graph.name
        };
        return ed_source_edit(e, &request) ? 0 : -1;
    }
    if (e->presentation && command.kind == EDITOR_COMMAND_SPAWN_NODE) {
        return ed_source_spawn_match(e, &(ed_search_match){ .desc = command.data.spawn_node.desc }) ? 0 : -1;
    }
    switch (command.kind) {
        case EDITOR_COMMAND_CREATE_GRAPH: {
            if (!command.data.create_graph.name || !command.data.create_graph.name[0] ||
                !behavior_asset_mutable(e->asset) ||
                behavior_asset_find_graph_by_name(
                    e->asset,
                    command.data.create_graph.name
                )) {
                return -1;
            }
            ed_snapshot_before(e);
            graph *created = command.data.create_graph.graph_kind == GRAPH_FUNCTION
                               ? behavior_asset_add_function(
                                     e->asset,
                                     command.data.create_graph.name,
                                     behavior_domain_pure(e->asset->domain)
                                 )
                           : command.data.create_graph.graph_kind == GRAPH_MACRO
                               ? behavior_asset_add_macro(
                                     e->asset,
                                     command.data.create_graph.name
                                 )
                               : behavior_asset_add_graph(
                                     e->asset,
                                     command.data.create_graph.graph_kind,
                                     command.data.create_graph.name
                                 );
            return created ? 0 : -1;
        }
        case EDITOR_COMMAND_RENAME_GRAPH: {
            const char *name = command.data.rename_graph.name;
            graph *target = behavior_asset_find_graph(
                e->asset,
                command.data.rename_graph.graph_id
            );
            graph *same = behavior_asset_find_graph_by_name(e->asset, name);
            if (!name || !name[0] || !behavior_asset_mutable(e->asset) || !target ||
                command.data.rename_graph.graph_id == 0 || (same && same != target)) {
                return -1;
            }
            ed_snapshot_before(e);
            return behavior_asset_rename_graph(
                e->asset,
                command.data.rename_graph.graph_id,
                name
            );
        }
        case EDITOR_COMMAND_OPEN_GRAPH:
            return editor_open_graph(e, command.data.open_graph.graph_id);
        case EDITOR_COMMAND_SPAWN_NODE:
            if (!command.data.spawn_node.desc || !e->active_graph ||
                !behavior_asset_mutable(e->asset)) {
                return -1;
            }
            return editor_spawn(
                       e,
                       command.data.spawn_node.desc,
                       command.data.spawn_node.x,
                       command.data.spawn_node.y
                   )
                     ? 0
                     : -1;
        case EDITOR_COMMAND_SET_PIN_DEFAULT: {
            node *n = graph_node_by_id(
                e->active_graph,
                command.data.set_pin_default.node_id
            );
            const pin_value *value = command.data.set_pin_default.value;
            if (e->presentation) {
                if (command.data.set_pin_default.output != 0 &&
                    command.data.set_pin_default.output != 1) {
                    return -1;
                }
                bool output = command.data.set_pin_default.output != 0;
                int pin = command.data.set_pin_default.pin_index;
                int count = n ? (output ? n->output_count : n->input_count) : 0;
                if (!n || !value || pin < 0 || pin >= count) {
                    return -1;
                }
                const pin_spec *spec = output ? &n->output_pins[pin] : &n->input_pins[pin];
                const source_presentation_origin *origin = authoring_source_presentation_origin(
                    e->presentation, e->active_graph->graph_id, n->id, spec->id
                );
                if (!origin || origin->flow) {
                    ed_show_error(e, "Select a source value to edit this input");
                    return -1;
                }
                authoring_edit_request request = {
                    .kind = AUTHORING_EDIT_VALUE,
                    .generation = authoring_source_edit_generation(e->presentation),
                    .origin = origin->identity,
                    .value = value
                };
                return ed_source_edit_coalesced(
                           e, &request, command.data.set_pin_default.coalesce_history
                       ) ? 0 : -1;
            }
            if (!n || !value ||
                (command.data.set_pin_default.output != 0 &&
                 command.data.set_pin_default.output != 1) ||
                !behavior_asset_mutable(e->asset)) {
                return -1;
            }
            const pin_spec *pins = command.data.set_pin_default.output
                                     ? n->output_pins
                                     : n->input_pins;
            int pin_count = command.data.set_pin_default.output ? n->output_count
                                                                : n->input_count;
            int pin_index = command.data.set_pin_default.pin_index;
            if (pin_index < 0 || pin_index >= pin_count ||
                pins[pin_index].kind != PIN_DATA ||
                value->type != pins[pin_index].type) {
                return -1;
            }
            if (!command.data.set_pin_default.coalesce_history ||
                !collab_active() || collab_replaying() || !collab_is_host()) {
                ed_snapshot_before(e);
            }
            return graph_set_pin_default(
                e->active_graph,
                n,
                command.data.set_pin_default.output,
                pin_index,
                value
            );
        }
        case EDITOR_COMMAND_CONNECT_DATA:
        case EDITOR_COMMAND_CONNECT_EXEC: {
            node *from = graph_node_by_id(e->active_graph, command.data.connect.from_node);
            node *to = graph_node_by_id(e->active_graph, command.data.connect.to_node);
            if (!from || !to) {
                return -1;
            }
            if (command.data.connect.from_pin < 0 ||
                command.data.connect.from_pin >= from->output_count ||
                command.data.connect.to_pin < 0 ||
                command.data.connect.to_pin >= to->input_count) {
                return -1;
            }
            if (e->presentation) {
                return ed_connect(
                           e,
                           &(ed_pin_view){ from, ED_PIN_OUTPUT, command.data.connect.from_pin },
                           &(ed_pin_view){ to, ED_PIN_INPUT, command.data.connect.to_pin }
                       ) ? 0 : -1;
            }
            pin_kind expected_kind = command.kind == EDITOR_COMMAND_CONNECT_DATA
                                       ? PIN_DATA
                                       : PIN_EXEC;
            if (!behavior_asset_mutable(e->asset) ||
                !ed_connection_specs_valid(
                    &from->output_pins[command.data.connect.from_pin],
                    &to->input_pins[command.data.connect.to_pin],
                    expected_kind
                )) {
                return -1;
            }
            behavior_asset *snapshot = ed_snapshot(e);
            int pending = e->edit_pending;
            ed_pin_view output = { from, ED_PIN_OUTPUT, command.data.connect.from_pin };
            ed_pin_view input = { to, ED_PIN_INPUT, command.data.connect.to_pin };
            int result = ed_connect(e, &output, &input) ? 0 : -1;
            if (result == 0) {
                e->edit_pending = 1;
                if (collab_record_history()) {
                    if (snapshot) {
                        ed_push_asset(&e->undo, snapshot);
                    }
                    ed_free_asset_stack(&e->redo);
                } else {
                    behavior_asset_free(snapshot);
                }
                return 0;
            }
            behavior_asset *current = ed_snapshot(e);
            if (snapshot && current && !behavior_asset_equal(current, snapshot)) {
                if (ed_adopt_behavior_asset(e, snapshot) != 0) {
                    abort();
                }
                av_value_free(e->edit_revision_snapshot);
                e->edit_revision_snapshot = ed_document_snapshot(e);
                e->edit_revision_asset = e->asset;
                e->edit_revision_initialized = 1;
            }
            behavior_asset_free(current);
            behavior_asset_free(snapshot);
            e->edit_pending = pending;
            return result;
        }
        case EDITOR_COMMAND_COMPILE:
            ed_compile(e);
            return compile_report_blocked(&e->compile) ? -1 : 0;
    }
    return -1;
}

int editor_command_apply(editor *e, editor_command command)
{
    if (e && e->presentation) {
        return editor_command_apply_direct(e, command);
    }
    bool captures = command.kind == EDITOR_COMMAND_CREATE_GRAPH ||
                    command.kind == EDITOR_COMMAND_RENAME_GRAPH ||
                    command.kind == EDITOR_COMMAND_SET_PIN_DEFAULT ||
                    command.kind == EDITOR_COMMAND_CONNECT_DATA ||
                    command.kind == EDITOR_COMMAND_CONNECT_EXEC;
    if (!captures) {
        return editor_command_apply_direct(e, command);
    }
    char *action = graph_collab_action_for_command(e, command);
    if (!action) {
        return collab_active() && !collab_replaying()
                 ? -1
                 : editor_command_apply_direct(e, command);
    }
    collab_action_gate gate = collab_actions_begin(action);
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = editor_command_apply_direct(e, command);
    bool published = collab_actions_finish(action, result == 0);
    free(action);
    return published ? result : -1;
}

void editor_query_state(const editor *e, editor_query *out)
{
    assert(out);
    *out = (editor_query){ 0 };
    if (!e || !e->asset) {
        return;
    }
    out->active_graph = e->active_graph;
    out->graph_count = acount(e->asset->graphs);
    out->compile = &e->compile;
    out->inspector = e->inspector;
    out->inspection_blocked = ed_inspection_blocked(e);
    out->preview_error = eval_view_error(e->ev);
    if (e->edit_revision_initialized && !e->edit_pending &&
        e->edit_revision_asset == e->asset && e->edit_revision_snapshot) {
        out->dirty = !e->saved_snapshot ||
                     !av_equals_ordered(e->edit_revision_snapshot, e->saved_snapshot);
        return;
    }
    av_value *current = ed_document_snapshot(e);
    out->dirty = !av_equals_ordered(current, e->saved_snapshot);
    av_value_free(current);
}

static void ed_record_math_edit(editor *e, behavior_asset *before)
{
    e->edit_pending = 1;
    ed_push_asset(&e->undo, before);
    ed_free_asset_stack(&e->redo);
}

node *editor_add_behavior_expression(editor *e, const char *source, float x, float y, behavior_expression_error *error)
{
    if (!e || e->active_graph->math_expression) {
        return NULL;
    }
    behavior_asset *before = ed_snapshot(e);
    node *n = behavior_expression_add(e->asset, e->active_graph, source, error);
    if (!n) {
        behavior_asset_free(before);
        return NULL;
    }
    n->x = x;
    n->y = y;
    ed_record_math_edit(e, before);
    ed_select_node(e, n, 0);
    return n;
}

int editor_set_behavior_expression(editor *e, node *call, const char *source, behavior_expression_error *error)
{
    if (!e || e->active_graph->math_expression) {
        return -1;
    }
    const char *current = behavior_expression_source(e->asset, call);
    if (source && current && !strcmp(source, current)) {
        return 0;
    }
    behavior_asset *before = ed_snapshot(e);
    if (behavior_expression_set(e->asset, call, source, error) != 0 ||
        behavior_expression_refresh_mode(e->asset, call, GRAPH_EXPRESSION_NONE, error) !=
            0) {
        ed_adopt_behavior_asset(e, before);
        behavior_asset_free(before);
        return -1;
    }
    ed_record_math_edit(e, before);
    return 0;
}

int editor_rename_behavior_expression_input(
    editor *e,
    node *call,
    const char *from,
    const char *to,
    behavior_expression_error *error
)
{
    behavior_asset *before = ed_snapshot(e);
    if (behavior_expression_rename_input(e->asset, call, from, to, error) != 0) {
        behavior_asset_free(before);
        return -1;
    }
    ed_record_math_edit(e, before);
    return 0;
}

int editor_rewire_behavior_expression(
    editor *e,
    node *call,
    const char *source,
    const char *from,
    const char *to,
    behavior_expression_error *error
)
{
    behavior_asset *before = ed_snapshot(e);
    if (behavior_expression_rename_input(e->asset, call, from, to, error) != 0 ||
        behavior_expression_set(e->asset, call, source, error) != 0 ||
        behavior_expression_refresh_mode(e->asset, call, GRAPH_EXPRESSION_NONE, error) !=
            0) {
        ed_adopt_behavior_asset(e, before);
        behavior_asset_free(before);
        return -1;
    }
    ed_record_math_edit(e, before);
    return 0;
}

static node *editor_spawn_direct(editor *e, const node_desc *desc, float x, float y)
{
    if (e->active_graph->math_expression) {
        return NULL;
    }
    ed_snapshot_before(e);
    node *n = graph_add_node(e->active_graph, desc);
    if (!n) {
        return NULL;
    }
    if (!shader_graph_init_node(e->active_graph, n) ||
        ed_name_spawned_event(e, n) != 0) {
        graph_remove_node(e->active_graph, n);
        ed_cancel_snapshot(e);
        return NULL;
    }
    n->x = x;
    n->y = y;
    return n;
}

node *editor_spawn(editor *e, const node_desc *desc, float x, float y)
{
    if (e && e->presentation) {
        ed_source_spawn_match(e, &(ed_search_match){ .desc = desc });
        return NULL;
    }
    uint64_t expected_id = e && e->asset ? e->asset->graph->id_counter : 0;
    if (!expected_id) {
        expected_id = 1;
    }
    char *action = graph_collab_action_spawn(e, desc, x, y);
    if (!action) {
        return collab_active() && !collab_replaying()
                 ? NULL
                 : editor_spawn_direct(e, desc, x, y);
    }
    collab_action_gate gate = collab_actions_begin(action);
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return NULL;
    }
    node *made = editor_spawn_direct(e, desc, x, y);
    if (made) {
        assert(made->id == expected_id);
    }
    collab_actions_finish(action, made != NULL);
    free(action);
    return made;
}

int editor_delete_graph(editor *e, uint64_t graph_id)
{
    graph *target = behavior_asset_find_graph(e->asset, graph_id);
    if (!target || target == e->asset->graph || !behavior_asset_mutable(e->asset)) {
        return -1;
    }
    ed_snapshot_before(e);
    if (target == e->active_graph &&
        editor_open_graph(e, e->asset->graph->graph_id) != 0) {
        return -1;
    }
    if (target->math_expression) {
        ed_reset_node_interaction(e);
        for (int i = 0; i < acount(e->asset->graphs); i++) {
            graph *g = e->asset->graphs[i];
            for (int j = acount(g->nodes) - 1; j >= 0; j--) {
                node *n = g->nodes[j];
                if ((n->desc == &NODE_CallPureFunction || n->desc == &NODE_CallFunction ||
                     n->desc == &NODE_BehaviorExpression) &&
                    !n->reference_asset && n->reference_id == graph_id) {
                    int removed = graph_remove_node_tree(g, n);
                    assert(removed == 0);
                    (void)removed;
                    j = acount(g->nodes);
                }
            }
        }
    }
    return behavior_asset_delete_graph(e->asset, graph_id);
}
