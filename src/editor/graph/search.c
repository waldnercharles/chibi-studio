#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "editor/graph/search.h"

#include "assets/ui_events.h"
#include "authoring/studio_schema.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/ui/labels.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/events.h"
#include "graph/document/extract.h"
#include "graph/document/library.h"
#include "graph/document/serialize.h"
#include "graph/model/local.h"
#include "graph/model/registry.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/component_nodes.h"
#include "graph/nodes/conv_nodes.h"
#include "graph/nodes/studio_nodes.h"
#include "graph/shader/shader_graph.h"
#include "support/text_search.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pin_spec ed_math_inputs[] = { { .name = "a", .type = PIN_F64 },
                                     { .name = "b", .type = PIN_F64 } };
static pin_spec ed_math_outputs[] = { { .name = "Result", .type = PIN_F64 } };
static const node_desc ed_math_desc = {
    .name = "Behavior Expression",
    .category = "Math",
    .keywords = "formula expression equation calculate",
    .description = "Evaluates a formula as a value, an action run on Exec, or "
                   "a branch that fires True or False from Result.",
    .inputs = ed_math_inputs,
    .input_count = 2,
    .outputs = ed_math_outputs,
    .output_count = 1
};

static const node_desc ed_comment_box_desc = {
    .name = "Comment Box",
    .category = "Basic",
    .keywords = "comment note annotation",
    .description = "Adds a titled frame for grouping nodes.",
    .display_class = NODE_DISPLAY_PURE
};

static int ed_desc_score(const node_desc *desc, const char *filter)
{
    char label[256], secondary[768];
    editor_node_label(desc, label, sizeof label);
    snprintf(
        secondary,
        sizeof secondary,
        "%s %s",
        desc->category ? desc->category : "",
        desc->keywords ? desc->keywords : ""
    );
    int score = text_search_score(filter, label, secondary);
    int raw = text_search_score(filter, desc->name, secondary);
    return score > raw ? score : raw;
}

static const char *ed_desc_category(const node_desc *d)
{
    return d && d->category && d->category[0] ? d->category : "Basic";
}

static int ed_search_match_cmp(const void *a, const void *b)
{
    const ed_search_match *ma = (const ed_search_match *)a;
    const ed_search_match *mb = (const ed_search_match *)b;
    const char *ca = ma->category;
    const char *cb = mb->category;
    int c = mb->score - ma->score;
    if (c) {
        return c;
    }
    c = strcmp(ca, cb);
    return c ? c : strcmp(ma->name, mb->name);
}

/* PIN-11: a Generic (wildcard) data pin accepts any data peer; the type is
 * adopted in core on connect, so the editor treats it as compatible. */
static int ed_data_connectable(const pin_spec *source, const pin_spec *candidate)
{
    if ((source->flags | candidate->flags) & PIN_FLAG_SETTING) {
        return 0;
    }
    if ((candidate->flags & PIN_FLAG_REFERENCE) &&
        (!(source->flags & PIN_FLAG_REFERENCE) ||
         (!(candidate->flags & PIN_FLAG_WILDCARD) &&
          (source->type != candidate->type || source->container != candidate->container)))) {
        return 0;
    }
    if (source->kind == PIN_EXEC) {
        return 1;
    }
    if ((source->flags | candidate->flags) & PIN_FLAG_WILDCARD) {
        return 1;
    }
    return pin_types_compatible(source, candidate) ||
           conversion_for_pins(source, candidate) != NULL;
}

static const type_desc *ed_struct_output_type(const pin_spec *pin)
{
    if (!pin || pin->kind != PIN_DATA || pin->type != PIN_STRUCT ||
        pin->container != PIN_CONTAINER_SINGLE ||
        (pin->flags & PIN_FLAG_SETTING) || !pin->type_name) {
        return NULL;
    }
    return type_catalog_find(pin->type_name);
}

static bool ed_struct_field_pin(const pin_spec *aggregate, int index, pin_spec *field_pin)
{
    const type_desc *type = ed_struct_output_type(aggregate);
    if (!type || index < 0 || index >= type->field_count || !field_pin) {
        return false;
    }
    const type_field *field = &type->fields[index];
    *field_pin = (pin_spec){ .name = field->name,
                             .type = field->type,
                             .type_name = field->type_name,
                             .kind = PIN_DATA,
                             .container = PIN_CONTAINER_SINGLE };
    return true;
}

static bool ed_struct_fields_connectable(const pin_spec *aggregate, const pin_spec *target)
{
    if (!target || target->kind != PIN_DATA) {
        return false;
    }
    const type_desc *type = ed_struct_output_type(aggregate);
    for (int i = 0; type && i < type->field_count; i++) {
        pin_spec field;
        if (ed_struct_field_pin(aggregate, i, &field) &&
            ed_data_connectable(&field, target)) {
            return true;
        }
    }
    return false;
}

static int ed_compact_system_node(editor *e, node *n)
{
    if (e->ui_context != EDITOR_UI_CONTEXT_SYSTEM || !n || !n->desc) {
        return 0;
    }
    if (n->desc == &NODE_Event || !n->desc->type) {
        return 0;
    }
    int get = strncmp(n->desc->name, "Get_", 4) == 0;
    int set = strncmp(n->desc->name, "Set_", 4) == 0;
    if (!get && !set) {
        return 0;
    }
    if (get) {
        for (int i = n->input_count - 1; i >= 0; i--) {
            if (graph_remove_pin(e->active_graph, n, 0, i) != 0) {
                return -1;
            }
        }
        for (int i = n->output_count - 1; i >= 0; i--) {
            if (graph_remove_pin(e->active_graph, n, 1, i) != 0) {
                return -1;
            }
        }
    } else {
        for (int i = n->input_count - 1; i >= 0; i--) {
            if (graph_remove_pin(e->active_graph, n, 0, i) != 0) {
                return -1;
            }
        }
        pin_spec exec = { .name = "Exec", .type = PIN_FLOW, .kind = PIN_EXEC };
        if (graph_add_pin(e->active_graph, n, 0, &exec) < 0) {
            return -1;
        }
    }
    int output = get;
    if (n->desc->type->alias_of) {
        pin_spec pin = {
            .name = "Value",
            .type = PIN_STRUCT,
            .type_name = n->desc->type->alias_of,
            .kind = PIN_DATA,
        };
        return graph_add_pin(e->active_graph, n, output, &pin) < 0 ? -1 : 0;
    }
    for (int i = 0; i < n->desc->type->field_count; i++) {
        const type_field *field = &n->desc->type->fields[i];
        pin_spec pin = {
            .name = field->name,
            .type = field->type,
            .type_name = field->type_name,
            .kind = PIN_DATA,
        };
        if (graph_add_pin(e->active_graph, n, output, &pin) < 0) {
            return -1;
        }
    }
    return 0;
}

static int ed_spawn_desc_compatible(const editor *e, const node_desc *desc)
{
    if (desc == &ed_comment_box_desc) {
        return e->ui_context != EDITOR_UI_CONTEXT_SYSTEM && !e->spawn_link_pending;
    }
    if ((e->asset->domain == BEHAVIOR_DOMAIN_LIBRARY ||
         e->asset->domain == BEHAVIOR_DOMAIN_SHADER_LIBRARY) &&
        !behavior_library_node_allowed(e->asset->domain, desc)) {
        return 0;
    }
    bool shader = e->asset->domain == BEHAVIOR_DOMAIN_MATERIAL ||
                  e->asset->domain == BEHAVIOR_DOMAIN_SHAPE ||
                  e->asset->domain == BEHAVIOR_DOMAIN_MODEL ||
                  e->asset->domain == BEHAVIOR_DOMAIN_SHADER_LIBRARY;
    bool field_node = desc->category && !strcmp(desc->category, "Procedural Field");
    if (e->asset->domain == BEHAVIOR_DOMAIN_FIELD && !field_node) {
        return 0;
    }
    if (field_node && e->asset->domain != BEHAVIOR_DOMAIN_FIELD &&
        e->asset->domain != BEHAVIOR_DOMAIN_LIBRARY) {
        return 0;
    }
    bool shader_node = desc->category && !strncmp(desc->category, "Shader", 6);
    bool model_node = desc->category && !strncmp(desc->category, "Model", 5);
    if (e->asset->domain == BEHAVIOR_DOMAIN_MODEL) {
        if (!model_node && !shader_node) {
            return 0;
        }
    } else if (model_node || shader != shader_node) {
        return 0;
    }
    if (desc == &NODE_CallFunction || desc == &NODE_CallPureFunction ||
        desc == &NODE_BehaviorExpression || desc == &NODE_CallMacro) {
        return 0;
    }
    if (!e->spawn_link_pending) {
        return 1;
    }
    const pin_spec
        *source = e->spawn_link_pin.kind == ED_PIN_OUTPUT
                    ? &e->spawn_link_pin.n->output_pins[e->spawn_link_pin.index]
                    : &e->spawn_link_pin.n->input_pins[e->spawn_link_pin.index];
    const pin_spec *pins = e->spawn_link_pin.kind == ED_PIN_OUTPUT ? desc->inputs
                                                                   : desc->outputs;
    int count = e->spawn_link_pin.kind == ED_PIN_OUTPUT ? desc->input_count
                                                        : desc->output_count;
    for (int i = 0; i < count; i++) {
        const pin_spec *candidate = &pins[i];
        if (source->kind != candidate->kind) {
            continue;
        }
        if (source->kind == PIN_EXEC ||
            (e->spawn_link_pin.kind == ED_PIN_OUTPUT
                 ? ed_data_connectable(source, candidate)
                 : ed_data_connectable(candidate, source) ||
                       ed_struct_fields_connectable(candidate, source))) {
            return 1;
        }
    }
    return 0;
}

static void ed_add_search_match(
    const editor *e,
    ed_search_match **matches,
    ed_search_match match,
    const node_desc *signature,
    const char *filter
)
{
    if (!ed_spawn_desc_compatible(e, signature)) {
        return;
    }
    node_desc named = *signature;
    named.name = match.name;
    match.score = ed_desc_score(&named, filter);
    if (match.score >= 0) {
        apush(*matches, match);
    }
}

/* A component node configured by its setting, titled by its access and the
 * component or struct it names. A shape is the generated node it becomes. */
static void ed_add_preset(
    const editor *e,
    ed_search_match **matches,
    ed_search_match match,
    const type_desc *type,
    const node_desc *shape,
    const char *filter
)
{
    const node_desc *desc = match.desc;
    char label[128], verb[32];
    editor_component_label(type->name, label, sizeof label);
    snprintf(verb, sizeof verb, "%.*s", (int)strcspn(desc->name, " "), desc->name);
    snprintf(match.name, sizeof match.name, "%s %s", verb, label);
    match.category = desc->category;
    if (shape) {
        ed_add_search_match(e, matches, match, shape, filter);
        return;
    }
    pin_spec inputs[4], outputs[2];
    node_desc signature = *desc;
    memcpy(inputs, desc->inputs, (size_t)desc->input_count * sizeof *inputs);
    memcpy(outputs, desc->outputs, (size_t)desc->output_count * sizeof *outputs);
    for (int i = 0; i < desc->input_count; i++) {
        inputs[i].type_name = inputs[i].type == PIN_STRUCT ? type->name
                                                           : inputs[i].type_name;
    }
    for (int i = 0; i < desc->output_count; i++) {
        outputs[i].type_name = outputs[i].type == PIN_STRUCT ? type->name
                                                             : outputs[i].type_name;
    }
    if (match.field) {
        char name[128];
        editor_identifier_label(match.field, name, sizeof name);
        size_t used = strlen(match.name);
        snprintf(match.name + used, sizeof match.name - used, " %s", name);
        const type_field *f = pin_type_field_path(type, match.field);
        inputs[desc->input_count - 1] = (pin_spec){ .name = f->name,
                                                    .type = f->type,
                                                    .type_name = f->type_name };
    }
    signature.inputs = inputs;
    signature.outputs = outputs;
    ed_add_search_match(e, matches, match, &signature, filter);
}

/* Component nodes replace the generated per-component and per-struct nodes;
 * a query offers their presets. */
static void ed_search_components(const editor *e, ed_search_match **matches, const char *filter)
{
    if (!filter || !filter[0]) {
        return;
    }
    component_node_preset *presets = component_node_presets();
    for (int i = 0; i < acount(presets); i++) {
        const component_node_preset *p = &presets[i];
        ed_search_match match = { .desc = p->desc,
                                  .setting = p->setting,
                                  .field = p->field };
        const node_desc *shape = component_node_shape(p->desc, p->setting);
        ed_add_preset(e, matches, match, p->type, shape, filter);
    }
    afree(presets);
}

/* An Event node of the asset already handles the event. */
static bool ed_event_handled(const editor *e, const char *name)
{
    for (int g = 0; g < acount(e->asset->graphs); g++) {
        const graph *graph = e->asset->graphs[g];
        for (int n = 0; n < acount(graph->nodes); n++) {
            const node *node = graph->nodes[n];
            if (node->desc->flow_kind == NODE_FLOW_EVENT &&
                node->reference_name && !strcmp(node->reference_name, name)) {
                return true;
            }
        }
    }
    return false;
}

static void ed_search_host_events(const editor *e, ed_search_match **matches, const char *filter)
{
    for (int i = 0; i < e->event_catalog_count; i++) {
        const game_host_event_desc *event = &e->event_catalog[i];
        if (ed_event_handled(e, event->name)) {
            continue;
        }
        pin_spec *outputs = NULL;
        apush(outputs, NODE_Event.outputs[0]);
        for (int p = 0; p < event->payload_count; p++) {
            apush(outputs, graph_event_payload_pin(&event->payload[p]));
        }
        node_desc signature = NODE_Event;
        signature.outputs = outputs;
        signature.output_count = acount(outputs);
        signature.keywords = event->keywords;
        ed_search_match match = { .desc = &NODE_Event,
                                  .host_event = i + 1,
                                  .category = "Events" };
        snprintf(match.name, sizeof match.name, "On %s", event->name);
        ed_add_search_match(e, matches, match, &signature, filter);
        afree(outputs);
    }
}

/* Events GameUi documents send, found by their name and by what the player
 * does to send them. */
static void ed_search_ui_events(const editor *e, ed_search_match **matches, const char *filter)
{
    for (int i = 0; i < e->ui_event_catalog_count; i++) {
        const ui_event *event = &e->ui_event_catalog[i];
        if (ed_event_handled(e, event->name)) {
            continue;
        }
        char keywords[384];
        snprintf(keywords, sizeof keywords, "%s %s", event->keywords, event->description);
        node_desc signature = NODE_Event;
        signature.keywords = keywords;
        signature.category = "UI Events";
        ed_search_match match = { .desc = &NODE_Event,
                                  .ui_event = event,
                                  .category = "UI Events" };
        snprintf(match.name, sizeof match.name, "On %s", event->name);
        ed_add_search_match(e, matches, match, &signature, filter);
    }
}

/* The values GameUi documents bind under self, as Set UI Data nodes that set
   them: "Set items" in UI Data, found by the document and its rows' fields. */
static void ed_search_ui_data(const editor *e, ed_search_match **matches, const char *filter)
{
    const node_desc *set = registry_node_by_name("Set UI Data");
    for (int i = 0; set && i < e->ui_data_catalog_count; i++) {
        const ui_data_binding *b = &e->ui_data_catalog[i];
        char keywords[512];
        snprintf(
            keywords,
            sizeof keywords,
            "%s self %s %s %s",
            set->keywords,
            b->document,
            b->binding.list ? "list rows data-for" : "value",
            b->binding.fields
        );
        node_desc signature = *set;
        signature.keywords = keywords;
        signature.category = "UI Data";
        ed_search_match match = { .desc = set, .ui_data = b, .category = "UI Data" };
        snprintf(match.name, sizeof match.name, "Set %s (%s)", b->binding.name, b->document);
        ed_add_search_match(e, matches, match, &signature, filter);
    }
}

static void ed_search_events(const editor *e, ed_search_match **matches, const char *filter)
{
    const node_desc *descs[] = { &NODE_Event,
                                 &NODE_CallEvent,
                                 registry_node_by_name("Send Event") };
    const char *verbs[] = { "On", "Call", "Send" };
    for (int i = 0; i < acount(e->asset_catalog); i++) {
        const behavior_asset *event = e->asset_catalog[i];
        if (event->domain != BEHAVIOR_DOMAIN_EVENT_DEFINITION) {
            continue;
        }
        for (int action = 0; action < 3; action++) {
            const node_desc *desc = descs[action];
            if (!desc) {
                continue;
            }
            node_desc signature = *desc;
            pin_spec *pins = NULL;
            const pin_spec *base = action ? desc->inputs : desc->outputs;
            int count = action ? desc->input_count : desc->output_count;
            for (int p = 0; p < count; p++) {
                apush(pins, base[p]);
            }
            for (int p = 0; p < acount(event->variables); p++) {
                const behavior_variable *v = &event->variables[p];
                apush(
                    pins,
                    ((pin_spec){ .name = v->name,
                                 .type = v->type,
                                 .type_name = v->type_name,
                                 .container = v->container })
                );
            }
            if (action) {
                signature.inputs = pins;
                signature.input_count = acount(pins);
            } else {
                signature.outputs = pins;
                signature.output_count = acount(pins);
            }
            ed_search_match match = { .desc = desc, .event = event, .category = "Events" };
            snprintf(
                match.name,
                sizeof match.name,
                "%s %s",
                verbs[action],
                event->name
            );
            ed_add_search_match(e, matches, match, &signature, filter);
            afree(pins);
        }
    }
}

ed_search_match *ed_search_matches(const editor *e, const char *filter)
{
    ed_search_match *matches = NULL;
    for (int i = -1; i < registry_node_count(); i++) {
        const node_desc *desc = i < 0 ? &ed_math_desc : registry_node(i);
        bool generated_access = component_generated_kind(desc) &&
                                e->ui_context != EDITOR_UI_CONTEXT_SYSTEM;
        bool editor_only = studio_node_is(desc) && !e->studio_nodes;
        if (desc == &NODE_GetVariable || desc == &NODE_SetVariable ||
            generated_access || editor_only) {
            continue;
        }
        ed_search_match match = { .desc = desc, .category = ed_desc_category(desc) };
        editor_node_label(desc, match.name, sizeof match.name);
        if (desc == &NODE_Self) {
            if (!e->asset->owner_type_name) {
                continue;
            }
            pin_spec output = desc->outputs[0];
            output.type_name = e->asset->owner_type_name;
            node_desc signature = *desc;
            signature.outputs = &output;
            ed_add_search_match(e, &matches, match, &signature, filter);
            continue;
        }
        ed_add_search_match(e, &matches, match, desc, filter);
    }
    if (e->ui_context == EDITOR_UI_CONTEXT_SYSTEM) {
        return matches;
    }
    ed_search_match comment = { .name = "Comment Box",
                                .desc = &ed_comment_box_desc,
                                .category = ed_desc_category(&ed_comment_box_desc) };
    ed_add_search_match(e, &matches, comment, &ed_comment_box_desc, filter);
    ed_search_components(e, &matches, filter);
    bool shader = e->asset->domain == BEHAVIOR_DOMAIN_MATERIAL ||
                  e->asset->domain == BEHAVIOR_DOMAIN_SHAPE ||
                  e->asset->domain == BEHAVIOR_DOMAIN_MODEL ||
                  e->asset->domain == BEHAVIOR_DOMAIN_SHADER_LIBRARY;
    for (int a = -1; a < acount(e->asset_catalog); a++) {
        const behavior_asset *asset = a < 0 ? e->asset : e->asset_catalog[a];
        if (a >= 0 && (!strcmp(asset->name, e->asset->name) ||
                       asset->domain != (shader ? BEHAVIOR_DOMAIN_SHADER_LIBRARY
                                                : BEHAVIOR_DOMAIN_LIBRARY))) {
            continue;
        }
        for (int i = 1; i < acount(asset->graphs); i++) {
            graph *function = asset->graphs[i];
            if (function == e->active_graph ||
                (function->math_expression &&
                 !strcmp(function->name, function->math_expression)) ||
                (function->kind != GRAPH_FUNCTION && function->kind != GRAPH_MACRO)) {
                continue;
            }
            if (a >= 0 && function->kind != GRAPH_FUNCTION) {
                continue;
            }
            node *entry = graph_node_by_id(function, function->entry_id);
            node *ret = graph_node_by_id(function, function->return_id);
            if (!entry || !ret) {
                continue;
            }
            bool field = e->asset->domain == BEHAVIOR_DOMAIN_FIELD;
            if (field) {
                bool compatible = ret->input_count == 1;
                for (int side = 0; side < 2; side++) {
                    const pin_spec *pins = side ? ret->input_pins : entry->output_pins;
                    int count = side ? ret->input_count : entry->output_count;
                    for (int p = 0; p < count; p++) {
                        compatible &= pins[p].kind == PIN_DATA &&
                                      pins[p].type == PIN_F32 &&
                                      pins[p].container == PIN_CONTAINER_SINGLE;
                    }
                }
                for (int n = 0; n < acount(function->nodes); n++) {
                    const node_desc *desc = function->nodes[n]->desc;
                    compatible &= desc == &NODE_FunctionEntry ||
                                  desc == &NODE_Return ||
                                  desc == &NODE_CallPureFunction ||
                                  (desc->category &&
                                   !strcmp(desc->category, "Procedural Field"));
                }
                if (!compatible) {
                    continue;
                }
            }
            const char *category = field  ? "Procedural Field"
                                 : shader ? "Shader/Functions"
                                 : function->kind == GRAPH_MACRO ? "Macros"
                                                                 : "Functions";
            bool executable = false;
            for (int p = 0; p < entry->output_count; p++) {
                executable |= entry->output_pins[p].kind == PIN_EXEC;
            }
            for (int p = 0; p < ret->input_count; p++) {
                executable |= ret->input_pins[p].kind == PIN_EXEC;
            }
            ed_search_match match = { .function = function,
                                      .category = category,
                                      .desc = function->kind == GRAPH_MACRO ? &NODE_CallMacro
                                            : executable ? &NODE_CallFunction
                                                         : &NODE_CallPureFunction };
            if (a < 0) {
                editor_identifier_label(function->name, match.name, sizeof match.name);
            } else {
                snprintf(
                    match.name,
                    sizeof match.name,
                    "%s / %s",
                    asset->name,
                    function->name
                );
            }
            node_desc signature = { .name = match.name,
                                    .category = category,
                                    .inputs = entry->output_pins,
                                    .input_count = entry->output_count,
                                    .outputs = ret->input_pins,
                                    .output_count = ret->input_count };
            ed_add_search_match(e, &matches, match, &signature, filter);
        }
    }
    if (!shader && e->asset->domain == BEHAVIOR_DOMAIN_EVENT) {
        ed_search_host_events(e, &matches, filter);
        ed_search_ui_events(e, &matches, filter);
        ed_search_ui_data(e, &matches, filter);
        ed_search_events(e, &matches, filter);
        for (int i = 0; i < acount(e->asset->variables); i++) {
            const behavior_variable *v = &e->asset->variables[i];
            for (int set = 0; set < 2; set++) {
                const node_desc *desc = set ? &NODE_SetVariable : &NODE_GetVariable;
                pin_spec data = { .name = v->name,
                                  .type = v->type,
                                  .type_name = v->type_name,
                                  .container = v->container };
                pin_spec inputs[] = { { .name = "Exec", .type = PIN_FLOW, .kind = PIN_EXEC },
                                      data };
                pin_spec outputs[] = { { .name = "Then", .type = PIN_FLOW, .kind = PIN_EXEC },
                                       data };
                node_desc signature = *desc;
                signature.inputs = set ? inputs : NULL;
                signature.input_count = set ? 2 : 0;
                signature.outputs = set ? outputs : &data;
                signature.output_count = set ? 2 : 1;
                ed_search_match match = { .desc = desc,
                                          .variable = v,
                                          .category = "Variables" };
                char name[256];
                editor_identifier_label(v->name, name, sizeof name);
                snprintf(match.name, sizeof match.name, "%s %s", set ? "Set" : "Get", name);
                ed_add_search_match(e, &matches, match, &signature, filter);
            }
        }
        const node_desc *spawn = registry_node_by_name("Spawn Prototype");
        for (int i = 0; spawn && i < acount(e->prototype_catalog); i++) {
            ed_search_match match = { .desc = spawn,
                                      .prototype = e->prototype_catalog[i],
                                      .category = "Prototypes" };
            snprintf(match.name, sizeof match.name, "Spawn %s", match.prototype);
            ed_add_search_match(e, &matches, match, spawn, filter);
        }
    }
    if (acount(matches) > 1) {
        qsort(matches, (size_t)acount(matches), sizeof *matches, ed_search_match_cmp);
    }
    return matches;
}

node_subject ed_search_match_subject(const editor *e, const ed_search_match *match)
{
    return (node_subject){
        .desc = match->desc,
        .setting = match->setting,
        .field = match->field,
        .host = match->host_event ? &e->event_catalog[match->host_event - 1] : NULL,
        .definition = match->event,
    };
}

static node *spawn_match(editor *e, const ed_search_match *match)
{
    graph *g = e->active_graph;
    if (match->function) {
        return match->function->kind == GRAPH_MACRO
                 ? behavior_asset_add_macro_call_node_to_graph(
                       e->asset,
                       g,
                       match->function
                   )
                 : behavior_asset_add_function_call_node_to_graph(
                       e->asset,
                       g,
                       match->function
                   );
    }
    if (match->desc == &ed_math_desc) {
        return behavior_expression_add(e->asset, g, "a + b", NULL);
    }
    if (!strcmp(match->desc->name, "Shader Surface Output")) {
        return shader_graph_add_output(g, false);
    }
    if (!strcmp(match->desc->name, "Shader Shape Output")) {
        return shader_graph_add_output(g, true);
    }
    node *n = graph_add_node(g, match->desc);
    if (!n) {
        return NULL;
    }
    if (match->desc == &NODE_Self) {
        if (!e->asset->owner_type_name ||
            graph_set_pin_type(g, n, 1, 0, PIN_HANDLE, e->asset->owner_type_name) != 0) {
            graph_remove_node(g, n);
            return NULL;
        }
    }
    if (match->variable) {
        const behavior_variable *v = match->variable;
        bool set = match->desc == &NODE_SetVariable;
        graph_set_node_reference(g, n, v->id, v->name);
        if (set) {
            graph_set_pin_type(g, n, 0, 1, v->type, v->type_name);
            graph_set_pin_container(g, n, 0, 1, v->container);
        }
        graph_set_pin_type(g, n, 1, set ? 1 : 0, v->type, v->type_name);
        graph_set_pin_container(g, n, 1, set ? 1 : 0, v->container);
    }
    if (match->host_event) {
        const game_host_event_desc *signature = &e->event_catalog[match->host_event - 1];
        graph_set_node_reference(g, n, 0, signature->name);
        ed_event_add_declared_params(e, n, signature);
    }
    if (match->ui_event && graph_set_event_name(g, n, match->ui_event->name) != 0) {
        graph_remove_node(g, n);
        return NULL;
    }
    if (match->event && graph_event_apply(g, n, match->event) != 0) {
        graph_remove_node(g, n);
        return NULL;
    }
    if (match->prototype) {
        pin_value value = { .type = PIN_NAME, .str = match->prototype };
        graph_set_pin_default(g, n, 0, 1, &value);
    }
    if (match->ui_data) {
        pin_value value = { .type = PIN_NAME,
                            .type_name = "name",
                            .str = match->ui_data->binding.name };
        graph_set_pin_default(g, n, 0, 2, &value);
    }
    if (match->setting && component_node_configure(g, n, match->setting, match->field)) {
        graph_remove_node(g, n);
        return NULL;
    }
    return n;
}

static int ed_expose_spawned_output_fields(editor *e, node *n)
{
    if (!e || !e->active_graph || !n) {
        return -1;
    }
    int output_count = n->output_count;
    for (int i = 0; i < output_count; i++) {
        if (!graph_output_struct(n, i)) {
            continue;
        }
        if (graph_expose_output_fields(e->active_graph, n, i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ed_autowire_spawn_input(editor *e, node *n)
{
    int connected = 0;
    const pin_spec *input = &e->spawn_link_pin.n->input_pins[e->spawn_link_pin.index];
    for (int p = 0; !connected && p < n->output_count; p++) {
        ed_pin_view target = { n, ED_PIN_OUTPUT, p };
        const pin_spec *output = &n->output_pins[p];
        if (output->kind == input->kind && ed_data_connectable(output, input)) {
            connected = ed_connect(e, &e->spawn_link_pin, &target);
        }
    }
    return connected;
}

static int ed_autowire_spawn(editor *e, node *n)
{
    if (e->spawn_link_pin.kind == ED_PIN_OUTPUT) {
        int connected = 0;
        for (int p = 0; p < n->input_count && !connected; p++) {
            ed_pin_view target = { n, ED_PIN_INPUT, p };
            const pin_spec *a = &e->spawn_link_pin.n
                                     ->output_pins[e->spawn_link_pin.index];
            const pin_spec *b = &n->input_pins[p];
            if (a->kind == b->kind && ed_data_connectable(a, b)) {
                connected = ed_connect(e, &e->spawn_link_pin, &target);
            }
        }
        return connected;
    }
    int connected = ed_autowire_spawn_input(e, n);
    if (connected || ed_expose_spawned_output_fields(e, n) != 0) {
        return connected;
    }
    return ed_autowire_spawn_input(e, n);
}

static node *ed_spawn_match_at_direct(editor *e, const ed_search_match *match, float x, float y, uint64_t *created_id)
{
    if (created_id) {
        *created_id = 0;
    }
    if (match->desc == &ed_comment_box_desc) {
        ed_snapshot_before(e);
        graph_add_comment_box(e->active_graph, "Comment", x, y, 320.0f, 160.0f);
        return NULL;
    }
    if (!e->spawn_link_pending) {
        ed_snapshot_before(e);
    }
    node *n = spawn_match(e, match);
    if (n && !shader_graph_init_node(e->active_graph, n)) {
        graph_remove_node(e->active_graph, n);
        n = NULL;
        ed_show_error(e, "This shader already has 32 parameters.");
    }
    if (n && ed_compact_system_node(e, n) != 0) {
        graph_remove_node(e->active_graph, n);
        n = NULL;
    }
    if (n && !match->event && !match->host_event && !match->ui_event &&
        ed_name_spawned_event(e, n) != 0) {
        graph_remove_node(e->active_graph, n);
        n = NULL;
    }
    if (!n) {
        return NULL;
    }
    uint64_t id = n->id;
    if (created_id) {
        *created_id = id;
    }
    n->x = x;
    n->y = y;
    if (match->desc == &ed_math_desc) {
        ed_select_node(e, n, 0);
    }
    if (e->spawn_link_pending) {
        ed_autowire_spawn(e, n);
        ed_commit_snapshot(e);
        e->spawn_link_pending = 0;
        /* Wiring can fold a spawned getter into the pin it feeds. */
        n = graph_node_by_id(e->active_graph, id);
    }
    return n;
}

bool ed_source_spawn_match(editor *e, const ed_search_match *match)
{
    assert(e->presentation);
    if (!match || !match->desc) {
        return false;
    }
    if (match->host_event < 0 || match->host_event > e->event_catalog_count) {
        return false;
    }
    char qualified[768];
    const char *name = match->desc->name;
    if (match->function) {
        if (match->function->owner == e->asset) {
            name = match->function->name;
        } else {
            int length = snprintf(
                qualified,
                sizeof qualified,
                "%s.%s",
                match->function->owner->name,
                match->function->name
            );
            if (length < 0 || (size_t)length >= sizeof qualified) {
                ed_show_error(e, "Callable name is too long");
                return false;
            }
            name = qualified;
        }
    }
    authoring_edit_request request = {
        .kind = AUTHORING_EDIT_CREATE_CAPABILITY,
        .generation = authoring_source_edit_generation(e->presentation),
        .origin = ed_source_scope_origin(e),
        .capability_name = name,
        .reference = match->setting   ? match->setting
                   : match->prototype ? match->prototype
                   : match->event     ? match->event->name
                   : match->host_event ? e->event_catalog[match->host_event - 1].name
                   : match->ui_event ? match->ui_event->name
                   : match->variable ? match->variable->name
                                     : NULL,
        .field = match->field
    };
    if (match->event || match->host_event || match->ui_event) {
        request.kind = AUTHORING_EDIT_CREATE_EVENT;
    }
    if (match->desc == &ed_math_desc) {
        request.kind = AUTHORING_EDIT_CREATE_EXPRESSION;
    }
    if (e->spawn_link_pending) {
        const ed_pin_view *pin = &e->spawn_link_pin;
        if (!pin->n || pin->index < 0 ||
            (pin->kind != ED_PIN_OUTPUT && pin->kind != ED_PIN_INPUT) ||
            pin->index >= (pin->kind == ED_PIN_OUTPUT ? pin->n->output_count
                                                      : pin->n->input_count)) {
            ed_show_error(e, "Creation connection is no longer available");
            return false;
        }
        const pin_spec *spec = pin->kind == ED_PIN_OUTPUT
                                 ? &pin->n->output_pins[pin->index]
                                 : &pin->n->input_pins[pin->index];
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            e->presentation,
            e->active_graph->graph_id,
            pin->n->id,
            spec->id
        );
        if (!origin) {
            ed_show_error(e, "Creation has no current source connection");
            return false;
        }
        if (pin->kind == ED_PIN_OUTPUT) {
            request.producer = origin->identity;
            request.producer_result = origin->result_index;
        } else {
            request.previous = origin->identity;
        }
    }
    bool changed = ed_source_edit(e, &request);
    if (changed) {
        e->spawn_link_pending = 0;
    }
    return changed;
}

node *ed_spawn_match_at_with_id(editor *e, const ed_search_match *match, float x, float y, uint64_t *created_id)
{
    if (created_id) {
        *created_id = 0;
    }
    if (e->presentation && match->desc != &ed_comment_box_desc) {
        ed_source_spawn_match(e, match);
        return NULL; /* The source owner adopts the new projection after dispatch. */
    }
    /* Comment boxes do not have node IDs and ed_spawn_match_at deliberately
       returns NULL for them, so they cannot use this node action envelope. */
    if (collab_active() && !collab_replaying() && match && match->desc &&
        !strcmp(match->desc->name, "Comment Box")) {
        return NULL;
    }
    char *action = graph_collab_action_spawn_match(e, match, x, y);
    if (!action) {
        if (collab_active() && !collab_replaying()) {
            return NULL;
        }
        return ed_spawn_match_at_direct(e, match, x, y, created_id);
    }
    collab_action_gate gate = collab_actions_begin(action);
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return NULL;
    }
    uint64_t expected_id = e->asset->graph->id_counter;
    if (!expected_id) {
        expected_id = 1;
    }
    uint64_t made_id = 0;
    node *made = ed_spawn_match_at_direct(e, match, x, y, &made_id);
    if (made_id) {
        assert(made_id == expected_id);
    }
    /* A spawn folded into its destination still changed the graph. */
    collab_actions_finish(action, made_id == expected_id);
    free(action);
    if (created_id) {
        *created_id = made_id;
    }
    return made;
}

node *ed_spawn_match_at(editor *e, const ed_search_match *match, float x, float y)
{
    return ed_spawn_match_at_with_id(e, match, x, y, NULL);
}

const char *ed_extract_selection(editor *e)
{
    if (e->presentation) {
        uint64_t *origins = ed_source_selected_origins(e);
        if (!acount(origins)) {
            afree(origins);
            return "Select complete source statements to extract";
        }
        authoring_edit_request request = {
            .kind = AUTHORING_EDIT_EXTRACT_FUNCTION,
            .generation = authoring_source_edit_generation(e->presentation),
            .origins = origins,
            .origin_count = acount(origins),
            .name = e->extract_name
        };
        bool changed = ed_source_edit(e, &request);
        afree(origins);
        return changed ? NULL : "Source extraction was rejected";
    }
    if (e->ui_context == EDITOR_UI_CONTEXT_SYSTEM) {
        return "Systems do not support extracted functions.";
    }
    if (e->active_graph->math_expression) {
        return "Generated math graphs are read-only";
    }
    behavior_asset *result = NULL;
    uint64_t call_id = 0;
    const char *error = behavior_asset_extract_function(
        e->asset,
        e->active_graph,
        e->selected,
        acount(e->selected),
        e->extract_name,
        &result,
        &call_id
    );
    if (error) {
        return error;
    }
    ed_snapshot_before(e);
    int adopted = ed_adopt_behavior_asset(e, result);
    behavior_asset_free(result);
    assert(adopted == 0);
    node *call = graph_node_by_id(e->active_graph, call_id);
    assert(call);
    ed_select_node(e, call, 0);
    return NULL;
}

node *ed_convert_behavior_expression_selection(
    editor *e,
    node *const *selection,
    int count,
    behavior_expression_error *error
)
{
    if (e->presentation) {
        if (error) {
            snprintf(error->message, sizeof error->message, "Convert canonical source through the checked source editor");
        }
        return NULL;
    }
    ed_snapshot_before(e);
    node *call = behavior_expression_convert_selection(e->asset, e->active_graph, selection, count, error);
    if (!call) {
        return NULL;
    }
    ed_select_node(e, call, 0);
    return call;
}
