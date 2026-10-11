#include "graph/nodes/core_nodes.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/problems.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const behavior_asset *editor_behavior_asset(const editor *e)
{
    return e ? e->asset : NULL;
}

static uint64_t *selected_ids(const editor *e)
{
    uint64_t *selected = NULL;
    for (int i = 0; i < acount(e->selected); i++) {
        apush(selected, e->selected[i]->id);
    }
    return selected;
}

static void restore_selected(editor *e, const uint64_t *selected)
{
    for (int i = 0; i < acount(selected); i++) {
        node *n = graph_node_by_id(e->active_graph, selected[i]);
        if (n) {
            ed_add_selected(e, n);
        }
    }
    e->arrange_pending = true;
}

int editor_source_presentation(editor *e, const authoring_source_presentation *source)
{
    uint64_t *selected = selected_ids(e);
    int result = ed_adopt_source_presentation(e, source);
    if (!result) {
        restore_selected(e, selected);
    }
    afree(selected);
    return result;
}

int editor_declarative_graph(editor *e, const behavior_asset *source)
{
    uint64_t *selected = selected_ids(e);
    int result = ed_adopt_behavior_asset(e, source);
    if (!result) {
        restore_selected(e, selected);
        ed_compile(e);
    }
    afree(selected);
    return result;
}

const authoring_source_presentation *editor_presentation(const editor *e)
{
    return e->presentation;
}

void editor_set_source_edit_handler(editor *e, editor_source_edit_fn edit, void *user)
{
    e->edit_source = edit;
    e->edit_source_user = user;
}

bool ed_source_edit(editor *e, const authoring_edit_request *request)
{
    return ed_source_edit_coalesced(e, request, false);
}

bool ed_source_edit_coalesced(editor *e, const authoring_edit_request *request, bool coalesce)
{
    assert(e->presentation);
    ed_clear_connect_error(e);
    if (!e->edit_source) {
        ed_show_error(e, "Open this source document before editing its graph");
        return false;
    }
    return e->edit_source(
        e->edit_source_user,
        request,
        coalesce,
        e->connect_error,
        sizeof e->connect_error
    );
}

void editor_set_source_resolver(
    editor *e,
    authoring_source_presentation *(*resolve)(void *, const char *, const char *),
    void *user
)
{
    e->resolve_source = resolve;
    e->resolve_source_user = user;
}

int editor_open_behavior_asset(editor *e, const behavior_asset *source, uint64_t graph_id)
{
    if (!e || !source || !behavior_asset_find_graph(source, graph_id)) {
        return -1;
    }
    free(e->document_key);
    e->document_key = NULL;
    int result = ed_adopt_behavior_asset_with_identity(e, source, source->identity);
    if (result != 0) {
        return -1;
    }
    aclear(e->graph_callers);
    av_value_free(e->saved_snapshot);
    e->saved_snapshot = ed_value_snapshot(e->asset);
    ed_free_asset_stack(&e->undo);
    ed_free_asset_stack(&e->redo);
    editor_set_inspector(e, NULL, 0);
    e->arrange_pending = true;
    return editor_open_graph(e, graph_id);
}

static int editor_open_graph_direct(editor *e, uint64_t graph_id)
{
    if (!e || !e->asset) {
        return -1;
    }
    graph *target = behavior_asset_find_graph(e->asset, graph_id);
    if (!target) {
        return -1;
    }
    if (target != e->active_graph) {
        aclear(e->graph_callers);
    }
    return ed_switch_graph(e, target);
}

int editor_open_graph(editor *e, uint64_t graph_id)
{
    if (!e || !e->asset) {
        return -1;
    }
    graph *target = behavior_asset_find_graph(e->asset, graph_id);
    if (!target) {
        return -1;
    }
    if (target == e->active_graph) {
        return editor_open_graph_direct(e, graph_id);
    }
    char *action = graph_collab_action_open(e, graph_id);
    if (!action) {
        return collab_active() && !collab_replaying()
                 ? -1
                 : editor_open_graph_direct(e, graph_id);
    }
    collab_action_gate gate = collab_actions_begin(action);
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = editor_open_graph_direct(e, graph_id);
    bool published = collab_actions_finish(action, result == 0);
    free(action);
    return published ? result : -1;
}

const graph *editor_graph_call_target(const editor *e, const node *call)
{
    if (!e || !call || !e->active_graph) {
        return NULL;
    }
    if (e->presentation) {
        uint64_t target = authoring_source_presentation_call_graph(
            e->presentation,
            e->active_graph->graph_id,
            call->id
        );
        return target ? behavior_asset_find_graph(e->asset, target) : NULL;
    }
    return ed_is_graph_call(call) && !call->reference_asset
             ? behavior_asset_find_graph(e->asset, call->reference_id)
             : NULL;
}

int editor_open_graph_call(editor *e, uint64_t node_id)
{
    if (!e || !e->active_graph) {
        return -1;
    }
    node *call = graph_node_by_id(e->active_graph, node_id);
    if (!call || (!e->presentation && !ed_is_graph_call(call))) {
        return -1;
    }
    if (call->reference_asset) {
        if (!e->open_asset) {
            return -1;
        }
        char *name = strdup(call->reference_asset);
        uint64_t target_id = call->reference_id;
        int result = e->open_asset(e->open_asset_user, name, target_id);
        free(name);
        return result;
    }
    const graph *resolved = editor_graph_call_target(e, call);
    graph *target = resolved ? behavior_asset_find_graph(e->asset, resolved->graph_id)
                             : NULL;
    if (!target || target == e->active_graph ||
        (target->kind != GRAPH_MACRO && target->kind != GRAPH_FUNCTION)) {
        return -1;
    }
    if (acount(e->graph_callers) == ED_CALLER_LIMIT) {
        adel(e->graph_callers, 0);
    }
    apush(e->graph_callers, ((ed_graph_caller){ e->active_graph->graph_id, node_id }));
    ed_cancel_snapshot(e);
    return ed_switch_graph(e, target);
}

int editor_graph_back(editor *e)
{
    if (!e || !e->asset) {
        return -1;
    }
    while (acount(e->graph_callers)) {
        ed_graph_caller caller = apop(e->graph_callers);
        graph *target = behavior_asset_find_graph(e->asset, caller.graph_id);
        if (!target) {
            continue;
        }
        ed_cancel_snapshot(e);
        ed_switch_graph(e, target);
        e->arrange_pending = false;
        node *call = graph_node_by_id(target, caller.node_id);
        if (call) {
            ed_select_node(e, call, 0);
        }
        return 0;
    }
    return -1;
}

int editor_graph_history_count(const editor *e)
{
    return e ? acount(e->graph_callers) : 0;
}

int editor_graph_ancestor(editor *e, int index)
{
    assert(index >= 0 && index < acount(e->graph_callers));
    asetlen(e->graph_callers, index + 1);
    return editor_graph_back(e);
}
