#include "graph/nodes/core_nodes.h"
/* Find References (Unreal): every node and object across the project that
 * uses a function, variable, event, component or asset, from the reference
 * index, listed in Output's Find view (studio_output.c). A row opens its
 * asset and reveals its node. */
#include "assets/reference_adapters.h"
#include "editor/studio/studio_graph_internal.h"
#include "graph/model/registry.h"
#include "graph/nodes/component_nodes.h"
#include "support/array.h"

#include <stdio.h>
#include <string.h>

static const char *use_label(reference_role role)
{
    switch (role) {
        case REFERENCE_CALL: return "Call";
        case REFERENCE_ARGUMENT: return "Argument";
        case REFERENCE_SPAWN: return "Spawn";
        case REFERENCE_HANDLE: return "Handle";
        case REFERENCE_SEND: return "Send";
        case REFERENCE_ATTACH: return "Attach";
        case REFERENCE_BIND: return "Bind";
        case REFERENCE_READ: return "Read";
        case REFERENCE_WRITE: return "Write";
        case REFERENCE_DEFINE: return "Define";
        case REFERENCE_USE:
        case REFERENCE_ROLE_COUNT: break;
    }
    return "Use";
}

/* A node reference names its graph; another, the object or field holding it. */
studio_reference studio_reference_of(const reference_index *index, const reference *use)
{
    const reference_asset *source = reference_index_asset_at(index, use->source);
    studio_reference row = { .kind = (int)source->kind,
                             .graph_id = use->graph_id,
                             .node_id = use->node_id,
                             .object = !use->node_id && use->object,
                             .use = use_label(use->role) };
    snprintf(row.asset, sizeof row.asset, "%s", source->name);
    const char *place = use->node_id ? (use->graph ? use->graph : "Event Graph")
                      : use->object  ? use->object
                                     : use->component;
    snprintf(row.place, sizeof row.place, "%s", place ? place : "");
    return row;
}

void studio_find_references(studio *s, const char *title, reference_symbol symbol)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    const reference **uses = NULL;
    reference_index_find_uses(index, symbol, &uses);
    asetlen(s->references, 0);
    for (int i = 0; i < acount(uses); i++) {
        apush(s->references, studio_reference_of(index, uses[i]));
    }
    afree(uses);
    snprintf(s->references_title, sizeof s->references_title, "%s", title);
    studio_output_find(s);
}

int studio_document_asset(const studio *s)
{
    const app_scene_authoring *scene = s->scene;
    const reference_index *index = app_scene_authoring_asset_index(scene);
    reference_asset_kind kind = scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM
                                  ? REFERENCE_ASSET_SYSTEM
                                  : reference_domain_kind(
                                        sg_graph_editor(s)->asset->domain
                                    );
    return reference_index_find_name(index, kind, scene->graph_document_id);
}

int studio_refresh_document_asset(studio *s)
{
    app_scene_authoring_refresh_asset_index(s->scene);
    return studio_document_asset(s);
}

static reference_symbol asset_symbol(int asset)
{
    return (reference_symbol){ .kind = REFERENCE_SYMBOL_ASSET, .asset = asset };
}

/* A target the index lacks lists no references rather than nothing at all. */
void studio_find_document_references(studio *s)
{
    studio_find_references(s, s->scene->graph_document_id, asset_symbol(studio_refresh_document_asset(s)));
}

void studio_find_asset_references(studio *s, int asset)
{
    app_scene_authoring_refresh_asset_index(s->scene);
    const reference_asset *
        found = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), asset);
    if (found) {
        studio_find_references(s, found->name, asset_symbol(asset));
    }
}

void studio_find_graph_references(studio *s, const graph *gr)
{
    int asset = studio_refresh_document_asset(s);
    studio_find_references(
        s,
        gr->name,
        (reference_symbol){ .kind = REFERENCE_SYMBOL_GRAPH, .asset = asset, .id = gr->graph_id }
    );
}

void studio_find_variable_references(studio *s, uint64_t variable_id)
{
    const behavior_variable
        *v = behavior_asset_find_variable_by_id(sg_graph_editor(s)->asset, variable_id);
    int asset = studio_refresh_document_asset(s);
    studio_find_references(
        s,
        v ? v->name : "Variable",
        (reference_symbol){ .kind = REFERENCE_SYMBOL_VARIABLE, .asset = asset, .id = variable_id }
    );
}

static bool graph_call(const node *n)
{
    return n->desc == &NODE_CallFunction || n->desc == &NODE_CallPureFunction ||
           n->desc == &NODE_CallMacro;
}

static bool event_node(const node *n)
{
    const char *name = n->desc->name;
    return n->reference_name && n->reference_name[0] &&
           (!strcmp(name, "Event") || !strcmp(name, "Call Event") ||
            !strcmp(name, "Send Event"));
}

static const char *spawned_prototype(const node *n)
{
    if (strcmp(n->desc->name, "Spawn Prototype")) {
        return NULL;
    }
    for (int p = 0; p < n->input_count; p++) {
        const pin_spec *pin = &n->input_pins[p];
        if (!strcmp(pin->name, "Prototype") && pin->default_value.str &&
            pin->default_value.str[0]) {
            return pin->default_value.str;
        }
    }
    return NULL;
}

/* The component a component node gets, sets, tests, adds or removes. */
static const char *accessed_component(const node *n)
{
    const char *verb, *name;
    return component_node_access(n, &verb, &name) && strcmp(verb, "Make") &&
                   strcmp(verb, "Break")
             ? name
             : NULL;
}

bool studio_node_reactive(const studio *s, const node *n)
{
    const editor *e = sg_graph_editor(s);
    const authoring_source_presentation *presentation = editor_presentation(e);
    const source_presentation_origin *origin = presentation && n && e->active_graph
                                                 ? authoring_source_presentation_origin(
                                                       presentation,
                                                       e->active_graph->graph_id,
                                                       n->id,
                                                       0
                                                   )
                                                 : NULL;
    return origin && origin->kind == SOURCE_PRESENTATION_BINDING;
}

bool studio_node_referable(const studio *s, const node *n)
{
    return !studio_node_reactive(s, n) &&
           (graph_call(n) || n->desc == &NODE_GetVariable || n->desc == &NODE_SetVariable ||
            event_node(n) || spawned_prototype(n) || accessed_component(n));
}

bool studio_node_traceable(const studio *s, const node *n)
{
    return !studio_node_reactive(s, n) && (graph_call(n) || event_node(n));
}

/* What a node references: the function it calls, the variable it reads or
   writes, the event it sends or handles, the prototype it spawns, or the
   component it accesses. */
reference_symbol studio_node_symbol(studio *s, const node *n, const char **title)
{
    int document = studio_refresh_document_asset(s);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    *title = n->reference_name;
    reference_symbol symbol = asset_symbol(-1);
    if (graph_call(n)) {
        bool library = n->reference_asset && n->reference_asset[0];
        symbol = (reference_symbol){
            .kind = REFERENCE_SYMBOL_GRAPH,
            .asset = library
                       ? reference_index_find_name(index, REFERENCE_ASSET_LIBRARY, n->reference_asset)
                       : document,
            .id = n->reference_id,
        };
    } else if (n->desc == &NODE_GetVariable || n->desc == &NODE_SetVariable) {
        symbol = (reference_symbol){ .kind = REFERENCE_SYMBOL_VARIABLE,
                                     .asset = document,
                                     .id = n->reference_id };
    } else if (event_node(n)) {
        symbol = (reference_symbol){ .kind = REFERENCE_SYMBOL_EVENT,
                                     .asset = -1,
                                     .name = n->reference_name };
    } else if (spawned_prototype(n)) {
        *title = spawned_prototype(n);
        symbol = asset_symbol(
            reference_index_find_name(index, REFERENCE_ASSET_PROTOTYPE, *title)
        );
    } else if (accessed_component(n)) {
        *title = accessed_component(n);
        symbol = (reference_symbol){ .kind = REFERENCE_SYMBOL_COMPONENT,
                                     .asset = -1,
                                     .name = *title };
    }
    *title = *title ? *title : n->desc->name;
    return symbol;
}

void studio_find_node_references(studio *s, const node *n)
{
    const char *title;
    reference_symbol symbol = studio_node_symbol(s, n, &title);
    studio_find_references(s, title, symbol);
}

void studio_find_clear(studio *s)
{
    asetlen(s->references, 0);
    s->references_title[0] = '\0';
}

static bool matches(const studio_reference *row, const char *filter)
{
    return studio_matches(filter, row->asset) ||
           studio_matches(filter, row->place) || studio_matches(filter, row->use);
}

/* The open document stays open, keeping its history; another opens first.
   A node reference reveals its node, and a scene's its object. */
void studio_open_reference(studio *s, const studio_reference *row)
{
    /* Workstream I: a place in a UI document opens in the Text Editor. */
    if (row->ui.path[0]) {
        studio_code_open_place(s, &row->ui);
        return;
    }
    app_scene_authoring *scene = s->scene;
    bool open = scene->graph_document_id &&
                !strcmp(scene->graph_document_id, row->asset) &&
                (row->kind == REFERENCE_ASSET_SYSTEM) ==
                    (scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM);
    if (!open) {
        int asset = reference_index_find_name(
            app_scene_authoring_asset_index(scene),
            (reference_asset_kind)row->kind,
            row->asset
        );
        finish_transform_edit(scene, false);
        if (asset < 0 || app_scene_authoring_open_asset(scene, asset) != 0) {
            studio_warn(s, "Could not open the reference");
            return;
        }
    }
    if (row->node_id) {
        studio_reveal_node(s, row->graph_id, row->node_id);
    }
    for (int i = 0; row->object && i < acount(scene->actors); i++) {
        if (scene->actors[i].name && !strcmp(scene->actors[i].name, row->place)) {
            select_instance(scene, i);
            studio_reveal_selection(s);
        }
    }
}

int *studio_find_rows(const studio *s)
{
    int *shown = NULL;
    for (int i = 0; i < acount(s->references); i++) {
        if (matches(&s->references[i], s->output_filter)) {
            apush(shown, i);
        }
    }
    return shown;
}
