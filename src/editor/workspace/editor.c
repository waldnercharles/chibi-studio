#include "assets/ui_events.h"
#include "authoring/preview.h"
#include "editor/graph/eval_view.h"
#include "editor/workspace/editor_internal.h"
#include "graph/compiler/validation.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"
#include "support/xalloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int ed_event_name_used(const editor *e, const char *name)
{
    for (int i = 0; i < acount(e->active_graph->nodes); i++) {
        const node *n = e->active_graph->nodes[i];
        if (n->desc->flow_kind == NODE_FLOW_EVENT && n->reference_name &&
            strcmp(n->reference_name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ed_event_param_count(const node *n)
{
    int count = 0;
    for (int i = 0; i < n->output_count; i++) {
        count += n->output_pins[i].kind == PIN_DATA;
    }
    return count;
}

/* Appends only the declared parameters this event is still missing, so no
 * existing pin is retyped or removed and no edge is invalidated under EVT-5. */
int ed_event_add_declared_params(editor *e, node *n, const host_event_desc *signature)
{
    int added = 0;
    for (int i = ed_event_param_count(n); i < signature->payload_count; i++) {
        pin_spec pin = graph_event_payload_pin(&signature->payload[i]);
        added += graph_add_pin(e->active_graph, n, 1, &pin) >= 0;
    }
    return added;
}

int ed_name_spawned_event(editor *e, node *n)
{
    if (!n->desc || n->desc->flow_kind != NODE_FLOW_EVENT || !e->event_catalog_count) {
        return 0;
    }
    for (int i = 0; i < e->event_catalog_count; i++) {
        const host_event_desc *signature = &e->event_catalog[i];
        if (ed_event_name_used(e, signature->name)) {
            continue;
        }
        if (graph_set_node_reference(e->active_graph, n, 0, signature->name) != 0) {
            return -1;
        }
        return ed_event_add_declared_params(e, n, signature) == signature->payload_count
                 ? 0
                 : -1;
    }
    return -1;
}

void ed_free_asset_stack(behavior_asset ***stack)
{
    for (int i = 0; i < acount(*stack); i++) {
        behavior_asset_free((*stack)[i]);
    }
    afree(*stack);
}

void ed_push_asset(behavior_asset ***stack, behavior_asset *value)
{
    if (!value) {
        return;
    }
    if (acount(*stack) == ED_HIST_MAX) {
        behavior_asset_free((*stack)[0]);
        memmove(*stack, *stack + 1, (size_t)(ED_HIST_MAX - 1) * sizeof **stack);
        (*stack)[ED_HIST_MAX - 1] = value;
        return;
    }
    apush(*stack, value);
}

av_value *ed_value_snapshot(const behavior_asset *asset)
{
    av_doc *doc = av_doc_new();
    av_value *snapshot = av_value_copy(behavior_asset_to_value(doc, asset));
    av_doc_free(doc);
    return snapshot;
}

av_value *ed_presentation_snapshot(const authoring_source_presentation *presentation)
{
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_obj_add_strcpy(doc, root, "source", authoring_source_presentation_text(presentation));
    char *metadata = authoring_source_presentation_metadata(presentation);
    av_obj_add_strcpy(doc, root, "layout", metadata ? metadata : "");
    free(metadata);
    av_value *snapshot = av_value_copy(root);
    av_doc_free(doc);
    return snapshot;
}

av_value *ed_document_snapshot(const editor *e)
{
    return e->presentation ? ed_presentation_snapshot(e->presentation)
                           : ed_value_snapshot(e->asset);
}

editor *editor_new(behavior_asset *asset)
{
    registry_validate();
    editor *e = xcalloc(1, sizeof *e);
    e->asset = asset;
    e->document_key = xstrdup(asset->name);
    e->ui_context = EDITOR_UI_CONTEXT_BEHAVIOR;
    e->active_graph = asset->graph;
    e->arrange_pending = true;
    e->edit_revision_asset = asset;
    e->saved_snapshot = ed_value_snapshot(asset);
    compile_report_init(&e->compile);
    e->ev = eval_view_new();
    return e;
}

void editor_set_value_formatter(editor *e, editor_value_formatter formatter, void *user)
{
    e->value_formatter = formatter;
    e->value_formatter_user = user;
    eval_view_set_formatter(e->ev, formatter, user);
}

void editor_set_choices_resolver(editor *e, editor_choices_resolver resolver, void *user)
{
    e->choices_resolver = resolver;
    e->choices_user = user;
}

const graph *editor_active_graph(const editor *e)
{
    return e ? e->active_graph : NULL;
}

static void free_ui_data_choices(editor *e)
{
    for (int i = 0; i < e->ui_data_choices.count; i++) {
        free((void *)e->ui_data_choices.items[i].label);
    }
    free((void *)e->ui_data_choices.items);
    e->ui_data_choices = (editor_choices){ 0 };
}

void editor_free(editor *e)
{
    if (!e) {
        return;
    }
    afree(e->asset_catalog);
    afree(e->prototype_catalog);
    free_ui_data_choices(e);
    ed_free_asset_stack(&e->undo);
    ed_free_asset_stack(&e->redo);
    free(e->document_key);
    for (int i = 0; i < acount(e->documents); i++) {
        ed_document_history *history = &e->documents[i];
        free(history->key);
        if (history->presentation) {
            authoring_source_presentation_free(history->presentation);
        } else {
            behavior_asset_free(history->asset);
        }
        av_value_free(history->saved);
        ed_free_asset_stack(&history->undo);
        ed_free_asset_stack(&history->redo);
        afree(history->links);
        free(history->location.nodes);
    }
    afree(e->documents);
    behavior_asset_free(e->pending_snapshot);
    free(e->clipboard);
    av_value_free(e->edit_revision_snapshot);
    av_value_free(e->saved_snapshot);
    ed_flow_activity_free(e);
    ed_destroy_preview(e);
    compile_report_free(&e->compile);
    behavior_asset_free(e->compiled);
    ed_release_document(e);
    afree(e->graph_callers);
    afree(e->selected);
    afree(e->selected_links);
    eval_view_free(e->ev);
    free(e);
}

void editor_set_ui_context(editor *e, editor_ui_context context)
{
    if (!e || context < EDITOR_UI_CONTEXT_SCENE || context > EDITOR_UI_CONTEXT_SYSTEM) {
        return;
    }
    e->ui_context = context;
}

void editor_set_studio_nodes(editor *e, bool offered)
{
    e->studio_nodes = offered;
}

static bool retained_keyboard;

void editor_set_retained_keyboard(bool captured)
{
    retained_keyboard = captured;
}

int editor_host_captures_keyboard(void)
{
    return retained_keyboard;
}

void editor_set_event_catalog(editor *e, const host_event_desc *events, int count)
{
    if (!e) {
        return;
    }
    e->event_catalog = count > 0 ? events : NULL;
    e->event_catalog_count = count > 0 ? count : 0;
}

void editor_set_ui_event_catalog(editor *e, const struct ui_event *events, int count)
{
    e->ui_event_catalog = count > 0 ? events : NULL;
    e->ui_event_catalog_count = count > 0 ? count : 0;
}

/* Each name once, labeled with its documents: "items (Hud, Shop)". */
void editor_set_ui_data_catalog(editor *e, const ui_data_binding *bindings, int count)
{
    e->ui_data_catalog = count > 0 ? bindings : NULL;
    e->ui_data_catalog_count = count > 0 ? count : 0;
    free_ui_data_choices(e);
    editor_choice *choices = count > 0 ? xmalloc((size_t)count * sizeof *choices)
                                       : NULL;
    int made = 0;
    for (int i = 0; i < count; i++) {
        const char *name = bindings[i].binding.name;
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = !strcmp(bindings[j].binding.name, name);
        }
        if (seen) {
            continue;
        }
        char label[256];
        int used = snprintf(label, sizeof label, "%s (%s", name, bindings[i].document);
        for (int j = i + 1; j < count && used < (int)sizeof label; j++) {
            if (!strcmp(bindings[j].binding.name, name)) {
                used += snprintf(
                    label + used,
                    sizeof label - (size_t)used,
                    ", %s",
                    bindings[j].document
                );
            }
        }
        if (used < (int)sizeof label) {
            snprintf(label + used, sizeof label - (size_t)used, ")");
        }
        choices[made++] = (editor_choice){ .label = xstrdup(label), .name = name };
    }
    e->ui_data_choices = (editor_choices){ choices, made };
}

void editor_set_asset_opener(editor *e, int (*open)(void *, const char *, uint64_t), void *user)
{
    e->open_asset = open;
    e->open_asset_user = user;
}

void editor_set_prototype_catalog(editor *e, const char *const *names, int count)
{
    aclear(e->prototype_catalog);
    for (int i = 0; i < count; i++) {
        apush(e->prototype_catalog, names[i]);
    }
}

void editor_set_asset_catalog(editor *e, const behavior_asset *const *assets, int count)
{
    bool changed = acount(e->asset_catalog) != count;
    for (int i = 0; !changed && i < count; i++) {
        changed = e->asset_catalog[i] != assets[i];
    }
    if (!changed) {
        return;
    }
    aclear(e->asset_catalog);
    for (int i = 0; i < count; i++) {
        apush(e->asset_catalog, assets[i]);
    }
    e->asset_catalog_revision++;
}

void editor_refresh_asset_catalog(editor *e, preview_world *world)
{
    int previous = acount(e->asset_catalog), count = preview_definition_count(world);
    bool changed = previous != count;
    asetlen(e->asset_catalog, count);
    for (int i = 0; i < count; i++) {
        const behavior_asset *asset = preview_definition_at(world, i)->working;
        changed |= i >= previous || e->asset_catalog[i] != asset;
        e->asset_catalog[i] = asset;
    }
    if (changed) {
        e->asset_catalog_revision++;
    }
}

/* Displays owned snapshots of actual execution; never evaluates graph producers. */
void ed_refresh_values(editor *e)
{
    eval_view_set_inspector(e->ev, ed_display_inspector(e), e->inspection_owner, e->presentation);
}
