#include "graph/authoring/graph_edit.h"
#include "editor/ui/labels.h"
#include "editor/workspace/editor_internal.h"
#include "graph/authoring/layout.h"
#include "graph/authoring/optimize.h"
#include "graph/model/local.h"
#include "runtime/component_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ed_source_scope_available(const editor *e)
{
    if (!e || !e->active_graph || !e->asset || e->active_graph->math_expression ||
        (e->ui_context != EDITOR_UI_CONTEXT_BEHAVIOR &&
         e->ui_context != EDITOR_UI_CONTEXT_SYSTEM)) {
        return false;
    }
    switch (e->asset->domain) {
        case BEHAVIOR_DOMAIN_EVENT:
        case BEHAVIOR_DOMAIN_LIBRARY:
        case BEHAVIOR_DOMAIN_MATERIAL:
        case BEHAVIOR_DOMAIN_SHAPE:
        case BEHAVIOR_DOMAIN_MODEL:
        case BEHAVIOR_DOMAIN_EVENT_DEFINITION:
        case BEHAVIOR_DOMAIN_SHADER_LIBRARY: return true;
        case BEHAVIOR_DOMAIN_FIELD: return false;
    }
    return false;
}

static bool ed_binding_scope_available(const editor *e)
{
    return e && e->ui_context == EDITOR_UI_CONTEXT_BEHAVIOR && e->asset &&
           e->asset->domain == BEHAVIOR_DOMAIN_EVENT;
}

const ecs_component_desc *ed_component_by_key(const char *key)
{
    if (!key) {
        return NULL;
    }
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        const ecs_component_desc *component = GAME_REGISTRY_COMPONENTS[i];
        if (component && component->key && !strcmp(component->key, key)) {
            return component;
        }
    }
    return NULL;
}

const char *ed_binding_label(const editor *e, const pin_binding *binding, char *buf, size_t cap)
{
    if (!binding) {
        return "Literal";
    }
    if (binding->kind == PIN_BINDING_LOCAL) {
        const graph_local *local = graph_local_by_id(e->active_graph, binding->local_id);
        return local ? local->name : "Missing local";
    }
    if (binding->kind == PIN_BINDING_DELTA_TIME) {
        return "Delta Time";
    }
    if (binding->kind == PIN_BINDING_EVENT_OUTPUT) {
        node *event = graph_node_by_id(e->active_graph, binding->event_node_id);
        int output = 0;
        const pin_spec *pin = event
                                ? graph_pin_by_id(event, binding->event_pin_id, &output, NULL)
                                : NULL;
        const char *name = event && event->reference_name ? event->reference_name
                                                          : "Event";
        snprintf(buf, cap, "On %s.%s", name, pin && output && pin->name ? pin->name : "Value");
        return buf;
    }
    if (binding->kind == PIN_BINDING_COMPONENT) {
        const ecs_component_desc *component = ed_component_by_key(binding->component_key);
        char label[128];
        editor_component_label(component ? component->name : binding->component_key, label, sizeof label);
        snprintf(buf, cap, "Self.%s", label);
        return buf;
    }
    if (binding->kind == PIN_BINDING_COMPONENT_FIELD) {
        const ecs_component_desc *component = ed_component_by_key(binding->component_key);
        snprintf(
            buf,
            cap,
            "Self.%s.%s",
            component && component->name
                ? component->name
                : (binding->component_key ? binding->component_key : "?"),
            binding->field_name ? binding->field_name : "?"
        );
        return buf;
    }
    if (binding->kind == PIN_BINDING_VARIABLE) {
        if (binding->source_asset && e->asset->name &&
            strcmp(binding->source_asset, e->asset->name) != 0) {
            return "Missing variable source";
        }
        const behavior_variable
            *variable = behavior_asset_find_variable_by_id(e->asset, binding->variable_id);
        if (!variable) {
            return "Missing variable";
        }
        snprintf(buf, cap, "Variable.%s", variable->name ? variable->name : "?");
        return buf;
    }
    return "Invalid source";
}

bool ed_inline_source_for(const editor *e, const node *target, int index, node **source, int *source_pin)
{
    node *found_source = NULL;
    int found_pin = -1;
    return e && e->active_graph && target &&
           graph_inline_source(
               e->active_graph,
               target,
               index,
               source ? source : &found_source,
               source_pin ? source_pin : &found_pin
           );
}

static void ed_forget_removed_node(editor *e, node *removed, uint64_t id)
{
    ed_remove_selected(e, removed);
    if (e->front_node == removed) {
        e->front_node = NULL;
    }
    if (e->spawn_link_pin.n == removed) {
        e->spawn_link_pin = (ed_pin_view){ 0 };
    }
    for (int i = acount(e->selected_links) - 1; i >= 0; i--) {
        ed_link_ref link = e->selected_links[i];
        if (link.from_id == id || link.to_id == id) {
            adel(e->selected_links, i);
        }
    }
}

bool ed_simplify_component_getter(
    editor *e,
    node *source,
    int source_pin,
    node *target,
    int target_pin,
    const node_desc *conversion
)
{
    if (!ed_binding_scope_available(e) || !e->active_graph || !source || !target) {
        return false;
    }
    node *connected = NULL;
    int connected_pin = -1;
    if (!graph_input_source(e->active_graph, target, target_pin, &connected, &connected_pin) ||
        connected != source || connected_pin != source_pin ||
        graph_input_conversion(e->active_graph, target, target_pin) != conversion) {
        return false;
    }
    uint64_t id = source->id;
    if (!graph_fold_component_getter(e->active_graph, source)) {
        return false;
    }
    ed_forget_removed_node(e, source, id);
    return true;
}

int ed_optimize_graph(editor *e)
{
    if (!e->active_graph || e->active_graph->math_expression ||
        e->active_graph->flow_pause_count) {
        return 0;
    }
    behavior_asset *before = ed_snapshot(e);
    if (!before) {
        return -1;
    }
    typedef struct
    {
        node *pointer;
        uint64_t id;
    } original_node;
    original_node *original = NULL;
    for (int i = 0; i < acount(e->active_graph->nodes); i++) {
        node *n = e->active_graph->nodes[i];
        apush(original, ((original_node){ n, n->id }));
    }
    graph_optimize_report report = { 0 };
    int result = graph_optimize(e->active_graph, NULL, &report);
    snprintf(
        e->optimize_summary,
        sizeof e->optimize_summary,
        "%d inputs folded, %d nodes removed, %d locals created. %d candidates "
        "skipped.",
        report.folded_inputs,
        report.removed_nodes,
        report.created_aliases,
        acount(report.skipped)
    );
    if (result > 0) {
        for (int i = 0; i < acount(original); i++) {
            if (!graph_node_by_id(e->active_graph, original[i].id)) {
                ed_forget_removed_node(e, original[i].pointer, original[i].id);
            }
        }
        asetlen(e->selected_links, 0);
        graph_layout(e->active_graph);
        ed_push_asset(&e->undo, before);
        ed_free_asset_stack(&e->redo);
        e->edit_pending = 1;
    } else {
        behavior_asset_free(before);
    }
    graph_optimize_report_free(&report);
    afree(original);
    return result;
}
