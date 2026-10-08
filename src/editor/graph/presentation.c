#include "graph/nodes/core_nodes.h"
#include "editor/ui/labels.h"
#include "editor/workspace/editor_internal.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/component_nodes.h"
#include "support/array.h"

#include <stdio.h>
#include <string.h>

bool ed_is_graph_call(const node *n)
{
    return n && (n->desc == &NODE_CallFunction || n->desc == &NODE_CallPureFunction ||
                 n->desc == &NODE_BehaviorExpression || n->desc == &NODE_CallMacro);
}

const char *ed_node_symbol(const node_desc *desc)
{
    /* Arithmetic and comparisons draw as their expression operator in every
       graph kind; reflected vector math marks itself through jit_kind. */
    const char *symbol = NULL;
    switch (desc->jit_kind) {
        case NODE_JIT_ADD: symbol = "+"; break;
        case NODE_JIT_SUB: symbol = "−"; break;
        case NODE_JIT_MUL: symbol = "×"; break;
        case NODE_JIT_DIV: symbol = "÷"; break;
        default: break;
    }
    int binding = 0;
    const char *spelling = behavior_expression_spelling(desc, &binding);
    if (spelling && binding >= 3 && binding != BEHAVIOR_EXPRESSION_PREFIX_BINDING) {
        symbol = behavior_expression_symbol(spelling);
    }
    static const char *const TRIG[][2] = { { "sin", "SIN" },
                                           { "cos", "COS" },
                                           { "tan", "TAN" } };
    for (int i = 0; spelling && !binding && i < 3; i++) {
        symbol = !strcmp(spelling, TRIG[i][0]) ? TRIG[i][1] : symbol;
    }
    return symbol;
}

/* Component nodes and the generated nodes they lower to read the same:
 * the access, then the component or struct. */
static bool ed_component_title(const node *n, char *buf, size_t cap)
{
    const char *verb = NULL, *name = NULL;
    if (!component_node_access(n, &verb, &name)) {
        return false;
    }
    char label[128];
    editor_component_label(name, label, sizeof label);
    snprintf(buf, cap, "%s %s", verb, label);
    return true;
}

const char *ed_node_title(const editor *e, const node *n, char *buf, size_t cap)
{
    return ed_node_title_in(e, e->asset, e->active_graph, n, buf, cap);
}

const char *ed_node_title_in(
    const editor *e,
    const behavior_asset *asset,
    const graph *g,
    const node *n,
    char *buf,
    size_t cap
)
{
    const authoring_source_presentation *presentation = editor_presentation(e);
    const source_presentation_origin *
        origin = g && presentation
                   ? authoring_source_presentation_origin(presentation, g->graph_id, n->id, 0)
                   : NULL;
    if (origin && origin->kind == SOURCE_PRESENTATION_FUNCTION && g->entry_id == n->id) {
        snprintf(buf, cap, "%s", g->name);
        return buf;
    }
    if (origin && origin->kind == SOURCE_PRESENTATION_BINDING) {
        const char *text = authoring_source_presentation_text(presentation);
        int length = origin->range.end - origin->range.start;
        snprintf(buf, cap, "%.*s", length, text + origin->range.start);
        return buf;
    }
    if (ed_component_title(n, buf, cap)) {
        return buf;
    }
    if (n->reference_name && (!strcmp(n->desc->name, "Shader Parameter") ||
                              !strcmp(n->desc->name, "Model Parameter") ||
                              !strcmp(n->desc->name, "Model Part") ||
                              !strcmp(n->desc->name, "Model Material Slot"))) {
        snprintf(buf, cap, "%s", n->reference_name);
        return buf;
    }
    if (n->reference_name && (!strcmp(n->desc->name, "Send Event") ||
                              !strcmp(n->desc->name, "Call Event"))) {
        snprintf(buf, cap, "%s %s", !strcmp(n->desc->name, "Send Event") ? "Send" : "Call", n->reference_name);
        return buf;
    }
    if (!strcmp(n->desc->name, "Model Instance") && n->reference_asset) {
        snprintf(buf, cap, "%s", n->reference_asset);
        return buf;
    }
    if (ed_is_graph_call(n)) {
        const behavior_asset *callee = asset;
        if (n->reference_asset) {
            callee = NULL;
            for (int i = 0; i < acount(e->asset_catalog); i++) {
                if (!strcmp(e->asset_catalog[i]->name, n->reference_asset)) {
                    callee = e->asset_catalog[i];
                    break;
                }
            }
        }
        const graph *target = behavior_asset_find_graph(callee, n->reference_id);
        const char *name = target ? target->name : n->reference_name;
        if (n->reference_asset && target) {
            snprintf(buf, cap, "%s.%s", callee->name, name);
            return buf;
        }
        if (!name || !name[0]) {
            editor_node_label(n->desc, buf, cap);
            return buf;
        }
        if (behavior_expression_source(asset, n)) {
            snprintf(buf, cap, "%s", name);
        } else {
            editor_identifier_label(name, buf, cap);
        }
        return buf;
    }
    const char *expression = behavior_expression_source(asset, n);
    if (expression) {
        snprintf(buf, cap, "%s", expression);
        return buf;
    }
    if (n->reference_name && n->reference_name[0] &&
        n->desc->flow_kind == NODE_FLOW_EVENT) {
        snprintf(buf, cap, "On %s", n->reference_name);
        return buf;
    }
    /* A macro's tunnels read as Inputs and Outputs, as in Unreal. */
    if (g && g->kind == GRAPH_MACRO && (n->id == g->entry_id || n->id == g->return_id)) {
        snprintf(buf, cap, "%s", n->id == g->entry_id ? "Inputs" : "Outputs");
        return buf;
    }
    char label[256];
    editor_node_label(n->desc, label, sizeof label);
    if (n->desc == &NODE_GetVariable || n->desc == &NODE_SetVariable) {
        const behavior_variable *variable = behavior_asset_find_variable_by_id(asset, n->reference_id);
        if (variable && variable->name) {
            char name[192];
            editor_identifier_label(variable->name, name, sizeof name);
            snprintf(buf, cap, "%s %s", n->desc == &NODE_GetVariable ? "Get" : "Set", name);
            return buf;
        }
    }
    snprintf(buf, cap, "%s", label);
    return buf;
}
