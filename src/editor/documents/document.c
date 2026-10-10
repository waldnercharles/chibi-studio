#include "editor/workspace/editor_internal.h"
#include "graph/document/serialize.h"
#include "support/asset_json.h"
#include "support/asset_value.h"
#include "support/xalloc.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static ed_location capture_location(const editor *e);
static int restore_location(editor *e, const ed_location *location, bool arrange_pending);

static void release_history_asset(ed_document_history *history)
{
    if (history->presentation) {
        authoring_source_presentation_free(history->presentation);
    } else {
        behavior_asset_free(history->asset);
    }
    history->presentation = NULL;
    history->asset = NULL;
}

static void ed_keep_document(editor *e)
{
    if (!e->document_key) {
        return;
    }
    ed_commit_snapshot(e);
    ed_document_history history = {
        .key = e->document_key,
        .asset = e->presentation ? NULL : behavior_asset_clone(e->asset),
        .presentation = e->presentation
                          ? authoring_source_presentation_clone(e->presentation)
                          : NULL,
        .saved = e->saved_snapshot,
        .owner_identity = e->asset ? e->asset->identity : 0,
        .undo = e->undo,
        .redo = e->redo,
        .location = capture_location(e),
        .arrange_pending = e->arrange_pending,
    };
    if (history.presentation) {
        history.asset = authoring_source_presentation_view(history.presentation);
    }
    for (int i = 0; i < acount(e->selected_links); i++) {
        apush(history.links, e->selected_links[i]);
    }
    apush(e->documents, history);
    e->document_key = NULL;
    e->saved_snapshot = NULL;
    e->undo = NULL;
    e->redo = NULL;
}

static int visit_document(
    editor *e,
    const behavior_asset *source,
    const authoring_source_presentation *presentation,
    uint64_t graph_id,
    const char *key
)
{
    if (e->document_key && !strcmp(e->document_key, key)) {
        return editor_open_graph(e, graph_id);
    }
    ed_document_history history = { 0 };
    int index = -1;
    for (int i = 0; i < acount(e->documents); i++) {
        if (!strcmp(e->documents[i].key, key)) {
            history = e->documents[i];
            index = i;
            break;
        }
    }
    const authoring_source_presentation *view = history.presentation
                                                  ? history.presentation
                                                  : presentation;
    const behavior_asset *target = history.asset ? history.asset : source;
    if (view) {
        target = authoring_source_presentation_view(view);
    }
    if (!target || !behavior_asset_find_graph(target, graph_id)) {
        return -1;
    }
    if (index >= 0) {
        adel(e->documents, index);
    }
    ed_keep_document(e);
    int result;
    if (view) {
        result = editor_source_presentation(e, view);
        if (!result) {
            aclear(e->graph_callers);
            result = editor_open_graph(e, graph_id);
        }
    } else {
        result = editor_open_behavior_asset(e, target, graph_id);
    }
    assert(result == 0);
    ed_free_asset_stack(&e->undo);
    ed_free_asset_stack(&e->redo);
    av_value_free(e->saved_snapshot);
    if (history.key) {
        e->undo = history.undo;
        e->redo = history.redo;
        e->saved_snapshot = history.saved;
        e->document_key = history.key;
        if (history.location.graph_id == graph_id) {
            restore_location(e, &history.location, history.arrange_pending);
            for (int i = 0; i < acount(history.links); i++) {
                apush(e->selected_links, history.links[i]);
            }
        }
        free(history.location.nodes);
        afree(history.links);
        release_history_asset(&history);
    } else {
        e->saved_snapshot = ed_document_snapshot(e);
        e->document_key = xstrdup(key);
        e->arrange_pending = true;
    }
    return 0;
}

uint64_t editor_document_graph(const editor *e, const char *key)
{
    if (e->document_key && !strcmp(e->document_key, key)) {
        return e->active_graph->graph_id;
    }
    for (int i = 0; i < acount(e->documents); i++) {
        if (!strcmp(e->documents[i].key, key)) {
            return e->documents[i].location.graph_id;
        }
    }
    return 0;
}

int editor_visit_behavior_asset(editor *e, const behavior_asset *source, uint64_t graph_id, const char *key)
{
    return visit_document(e, source, NULL, graph_id, key);
}

int editor_visit_source_presentation(
    editor *e,
    const authoring_source_presentation *source,
    uint64_t graph_id,
    const char *key
)
{
    return visit_document(e, NULL, source, graph_id, key);
}

int editor_reload_source_presentation(
    editor *e,
    const authoring_source_presentation *source,
    uint64_t graph_id,
    const char *key
)
{
    if (!behavior_asset_find_graph(authoring_source_presentation_view(source), graph_id)) {
        return -1;
    }
    char *copy = xstrdup(key);
    editor_source_presentation(e, source);
    aclear(e->graph_callers);
    free(e->document_key);
    e->document_key = copy;
    av_value_free(e->saved_snapshot);
    e->saved_snapshot = ed_document_snapshot(e);
    editor_forget_behavior_asset(e, copy);
    e->arrange_pending = true;
    return editor_open_graph(e, graph_id);
}

int editor_reload_source_presentation_document(
    editor *e,
    const authoring_source_presentation *source,
    const char *key
)
{
    if (e->document_key && !strcmp(e->document_key, key)) {
        const behavior_asset *view = authoring_source_presentation_view(source);
        uint64_t graph_id = e->active_graph->graph_id;
        if (!behavior_asset_find_graph(view, graph_id)) {
            graph_id = view->graph->graph_id;
        }
        return editor_reload_source_presentation(e, source, graph_id, key);
    }
    for (int i = 0; i < acount(e->documents); i++) {
        ed_document_history *history = &e->documents[i];
        if (strcmp(history->key, key)) {
            continue;
        }
        authoring_source_presentation *copy = authoring_source_presentation_clone(source);
        release_history_asset(history);
        history->presentation = copy;
        history->asset = authoring_source_presentation_view(copy);
        av_value_free(history->saved);
        history->saved = ed_presentation_snapshot(copy);
        ed_free_asset_stack(&history->undo);
        ed_free_asset_stack(&history->redo);
        free(history->location.nodes);
        history->location = (ed_location){ .graph_id = history->asset->graph->graph_id };
        history->arrange_pending = true;
        afree(history->links);
        history->owner_identity = history->asset->identity;
        return 0;
    }
    return 0;
}

int editor_reload_behavior_asset(editor *e, const behavior_asset *source, uint64_t graph_id, const char *key)
{
    char *copy = xstrdup(key);
    if (editor_open_behavior_asset(e, source, graph_id) != 0) {
        free(copy);
        return -1;
    }
    for (int i = 0; i < acount(e->documents); i++) {
        ed_document_history *history = &e->documents[i];
        if (strcmp(history->key, copy) != 0) {
            continue;
        }
        free(history->key);
        release_history_asset(history);
        av_value_free(history->saved);
        ed_free_asset_stack(&history->undo);
        ed_free_asset_stack(&history->redo);
        afree(history->links);
        free(history->location.nodes);
        adel(e->documents, i);
        break;
    }
    e->document_key = copy;
    return 0;
}

int editor_reload_behavior_asset_document(editor *e, const behavior_asset *source, const char *key)
{
    if (!e || !source || !key) {
        return -1;
    }
    if (e->document_key && !strcmp(e->document_key, key)) {
        const graph *graph = editor_active_graph(e);
        return graph && behavior_asset_find_graph(source, graph->graph_id)
                 ? editor_reload_behavior_asset(e, source, graph->graph_id, key)
                 : -1;
    }
    for (int i = 0; i < acount(e->documents); i++) {
        ed_document_history *history = &e->documents[i];
        if (strcmp(history->key, key) != 0) {
            continue;
        }
        behavior_asset *copy = behavior_asset_clone(source);
        if (!copy) {
            return -1;
        }
        release_history_asset(history);
        history->asset = copy;
        av_value_free(history->saved);
        history->saved = ed_value_snapshot(source);
        ed_free_asset_stack(&history->undo);
        ed_free_asset_stack(&history->redo);
        free(history->location.nodes);
        history->location = (ed_location){ 0 };
        history->arrange_pending = true;
        afree(history->links);
        history->owner_identity = source->identity;
        return 0;
    }
    return 0;
}

void editor_forget_behavior_asset(editor *e, const char *key)
{
    for (int i = 0; i < acount(e->documents); i++) {
        ed_document_history *history = &e->documents[i];
        if (strcmp(history->key, key) != 0) {
            continue;
        }
        free(history->key);
        release_history_asset(history);
        av_value_free(history->saved);
        ed_free_asset_stack(&history->undo);
        ed_free_asset_stack(&history->redo);
        afree(history->links);
        free(history->location.nodes);
        adel(e->documents, i);
        return;
    }
}

void editor_rename_behavior_asset(editor *e, const char *from, const char *to)
{
    if (e->document_key && !strcmp(e->document_key, from)) {
        free(e->document_key);
        e->document_key = xstrdup(to);
    }
    for (int i = 0; i < acount(e->documents); i++) {
        if (!strcmp(e->documents[i].key, from)) {
            free(e->documents[i].key);
            e->documents[i].key = xstrdup(to);
        }
    }
}

static ed_location capture_location(const editor *e)
{
    ed_location location = {
        .graph_id = e->active_graph->graph_id,
        .node_count = acount(e->selected),
        .caller_count = acount(e->graph_callers),
    };
    if (location.node_count) {
        location.nodes = xmalloc((size_t)location.node_count * sizeof *location.nodes);
        for (int i = 0; i < location.node_count; i++) {
            location.nodes[i] = e->selected[i]->id;
        }
    }
    assert(location.caller_count <= ED_CALLER_LIMIT);
    if (location.caller_count) {
        memcpy(
            location.callers,
            e->graph_callers,
            (size_t)location.caller_count * sizeof *location.callers
        );
    }
    return location;
}

void ed_restore_callers(editor *e, const ed_graph_caller *callers, int count)
{
    assert(count >= 0 && count <= ED_CALLER_LIMIT);
    int first = count;
    uint64_t target_id = e->active_graph->graph_id;
    for (int i = count - 1; i >= 0; i--) {
        const ed_graph_caller *caller = &callers[i];
        graph *parent = behavior_asset_find_graph(e->asset, caller->graph_id);
        node *call = parent ? graph_node_by_id(parent, caller->node_id) : NULL;
        if (!ed_is_graph_call(call) || call->reference_id != target_id) {
            break;
        }
        first = i;
        target_id = caller->graph_id;
    }
    aclear(e->graph_callers);
    for (int i = first; i < count; i++) {
        apush(e->graph_callers, callers[i]);
    }
}

static int restore_location(editor *e, const ed_location *location, bool arrange_pending)
{
    graph *target = behavior_asset_find_graph(e->asset, location->graph_id);
    if (!target) {
        return -1;
    }
    for (int i = 0; i < location->node_count; i++) {
        if (!graph_node_by_id(target, location->nodes[i])) {
            return -1;
        }
    }
    if (editor_open_graph(e, location->graph_id) != 0) {
        return -1;
    }
    ed_restore_callers(e, location->callers, location->caller_count);
    ed_clear_selection(e);
    e->front_node = NULL;
    for (int i = 0; i < location->node_count; i++) {
        ed_add_selected(e, graph_node_by_id(target, location->nodes[i]));
    }
    e->arrange_pending = arrange_pending;
    return 0;
}

static av_value *checkpoint_assets(av_doc *doc, behavior_asset **assets)
{
    av_value *array = av_arr(doc);
    for (int i = 0; i < acount(assets); i++) {
        av_arr_append(array, behavior_asset_to_value(doc, assets[i]));
    }
    return array;
}

static av_value *checkpoint_location(av_doc *doc, const ed_location *location)
{
    av_value *value = av_obj(doc);
    av_obj_add_uint(doc, value, "graph", location->graph_id);
    av_value *selection = av_obj_add_arr(doc, value, "selection");
    for (int i = 0; i < location->node_count; i++) {
        av_arr_add_uint(doc, selection, location->nodes[i]);
    }
    av_value *callers = av_obj_add_arr(doc, value, "callers");
    for (int i = 0; i < location->caller_count; i++) {
        av_value *item = av_arr_add_obj(doc, callers);
        av_obj_add_uint(doc, item, "graph", location->callers[i].graph_id);
        av_obj_add_uint(doc, item, "node", location->callers[i].node_id);
    }
    return value;
}

static av_value *checkpoint_document(av_doc *doc, const ed_document_history *history)
{
    av_value *value = av_obj(doc);
    av_obj_add_strcpy(doc, value, "key", history->key ? history->key : "");
    if (history->presentation) {
        av_obj_add_strcpy(
            doc,
            value,
            "source",
            authoring_source_presentation_text(history->presentation)
        );
        char *layout = authoring_source_presentation_metadata(history->presentation);
        av_obj_add_strcpy(doc, value, "layout", layout ? layout : "");
        free(layout);
    } else {
        av_obj_add_val(doc, value, "asset", behavior_asset_to_value(doc, history->asset));
    }
    av_obj_add_val(doc, value, "saved", history->saved ? av_clone(doc, history->saved) : av_null(doc));
    av_obj_add_uint(doc, value, "owner_identity", history->owner_identity);
    av_obj_set(doc, value, "arrange_pending", av_bool(doc, history->arrange_pending));
    av_obj_add_val(doc, value, "undo", checkpoint_assets(doc, history->undo));
    av_obj_add_val(doc, value, "redo", checkpoint_assets(doc, history->redo));
    av_value *links = av_obj_add_arr(doc, value, "links");
    for (int i = 0; i < acount(history->links); i++) {
        const ed_link_ref *link = &history->links[i];
        av_value *item = av_arr_add_obj(doc, links);
        av_obj_add_uint(doc, item, "from", link->from_id);
        av_obj_add_uint(doc, item, "output", link->from_pin_id);
        av_obj_add_uint(doc, item, "to", link->to_id);
        av_obj_add_uint(doc, item, "input", link->to_pin_id);
    }
    av_obj_add_val(doc, value, "location", checkpoint_location(doc, &history->location));
    return value;
}

char *editor_checkpoint(editor *e)
{
    ed_commit_snapshot(e);
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_doc_set_root(doc, root);
    av_obj_add_int(doc, root, "version", 3);
    ed_document_history current = { .key = e->document_key,
                                    .asset = e->asset,
                                    .presentation = e->presentation,
                                    .saved = e->saved_snapshot,
                                    .owner_identity = e->asset->identity,
                                    .undo = e->undo,
                                    .redo = e->redo,
                                    .location = capture_location(e),
                                    .arrange_pending = e->arrange_pending };
    current.links = e->selected_links;
    av_obj_add_val(doc, root, "active", checkpoint_document(doc, &current));
    free(current.location.nodes);
    av_value *documents = av_obj_add_arr(doc, root, "documents");
    for (int i = 0; i < acount(e->documents); i++) {
        av_arr_append(documents, checkpoint_document(doc, &e->documents[i]));
    }
    char *result = av_json_write(av_doc_root(doc));
    av_doc_free(doc);
    return result;
}

static bool checkpoint_read_assets(av_value *value, behavior_asset ***assets)
{
    if (!av_is_arr(value) || av_arr_size(value) > ED_HIST_MAX) {
        return false;
    }
    size_t i, count;
    av_value *item;
    av_arr_foreach(value, i, count, item)
    {
        behavior_asset *asset = behavior_asset_from_value(item);
        if (!asset) {
            return false;
        }
        apush(*assets, asset);
    }
    return true;
}

static void checkpoint_free_document(ed_document_history *history)
{
    free(history->key);
    release_history_asset(history);
    av_value_free(history->saved);
    ed_free_asset_stack(&history->undo);
    ed_free_asset_stack(&history->redo);
    afree(history->links);
    free(history->location.nodes);
}

static bool checkpoint_read_location(av_value *value, ed_location *location)
{
    av_value *graph = av_obj_get(value, "graph");
    av_value *selection = av_obj_get(value, "selection");
    av_value *callers = av_obj_get(value, "callers");
    if (!av_is_uint(graph) || !av_is_arr(selection) || !av_is_arr(callers) ||
        av_arr_size(callers) > ED_CALLER_LIMIT) {
        return false;
    }
    location->graph_id = av_get_uint(graph);
    location->node_count = (int)av_arr_size(selection);
    location->nodes = xmalloc(av_arr_size(selection) * sizeof(uint64_t));
    size_t i, count;
    av_value *item;
    av_arr_foreach(selection, i, count, item)
    {
        if (!av_is_uint(item)) {
            return false;
        }
        location->nodes[i] = av_get_uint(item);
    }
    av_arr_foreach(callers, i, count, item)
    {
        av_value *caller_graph = av_obj_get(item, "graph");
        av_value *node = av_obj_get(item, "node");
        if (!av_is_uint(caller_graph) || !av_is_uint(node)) {
            return false;
        }
        location->callers[location->caller_count++] = (ed_graph_caller){
            av_get_uint(caller_graph),
            av_get_uint(node),
        };
    }
    return true;
}

static bool checkpoint_selection_valid(const ed_document_history *history)
{
    const behavior_asset *asset = history->asset;
    graph *g = behavior_asset_find_graph(asset, history->location.graph_id);
    bool valid = g != NULL;
    for (int i = 0; valid && i < history->location.node_count; i++) {
        valid = graph_node_by_id(g, history->location.nodes[i]) != NULL;
    }
    for (int i = 0; valid && i < acount(history->links); i++) {
        const ed_link_ref *link = &history->links[i];
        bool found = false;
        for (int j = 0; j < acount(g->edges); j++) {
            const edge *wire = &g->edges[j];
            found |= wire->from_id == link->from_id &&
                     wire->from_pin_id == link->from_pin_id &&
                     wire->to_id == link->to_id && wire->to_pin_id == link->to_pin_id;
        }
        valid = found;
    }
    return valid;
}

static bool checkpoint_read_document(editor *e, av_value *value, ed_document_history *history)
{
    const char *key = av_get_str(av_obj_get(value, "key"));
    const char *source = av_get_str(av_obj_get(value, "source"));
    av_value *saved = av_obj_get(value, "saved");
    if (source) {
        const char *layout = av_get_str(av_obj_get(value, "layout"));
        if (!key || !layout || !e->resolve_source || !av_is_obj(saved)) {
            return false;
        }
        history->presentation = e->resolve_source(e->resolve_source_user, key, source);
        if (!history->presentation ||
            !authoring_source_presentation_restore(history->presentation, layout)) {
            return false;
        }
        history->asset = authoring_source_presentation_view(history->presentation);
        history->saved = av_value_copy(saved);
    } else {
        history->asset = behavior_asset_from_value(av_obj_get(value, "asset"));
        behavior_asset *saved_asset = behavior_asset_from_value(saved);
        history->saved = saved_asset ? av_value_copy(saved) : NULL;
        behavior_asset_free(saved_asset);
    }
    if (!key || !history->asset || !history->saved) {
        return false;
    }
    av_value *arrange_pending = av_obj_get(value, "arrange_pending");
    if (arrange_pending && !av_is_bool(arrange_pending)) {
        return false;
    }
    history->arrange_pending = av_get_bool(arrange_pending);
    history->key = xstrdup(key);
    av_value *owner_identity = av_obj_get(value, "owner_identity");
    if (owner_identity &&
        (!av_is_uint(owner_identity) || !av_get_uint(owner_identity))) {
        return false;
    }
    history->owner_identity = owner_identity ? av_get_uint(owner_identity) : 0;
    if (!checkpoint_read_assets(av_obj_get(value, "undo"), &history->undo) ||
        !checkpoint_read_assets(av_obj_get(value, "redo"), &history->redo)) {
        return false;
    }
    av_value *links = av_obj_get(value, "links");
    if (!av_is_arr(links)) {
        return false;
    }
    size_t link_index, link_count;
    av_value *link;
    av_arr_foreach(links, link_index, link_count, link)
    {
        av_value *from = av_obj_get(link, "from");
        av_value *output = av_obj_get(link, "output");
        av_value *to = av_obj_get(link, "to");
        av_value *input = av_obj_get(link, "input");
        if (!av_is_uint(from) || !av_is_uint(output) || !av_is_uint(to) ||
            !av_is_uint(input)) {
            return false;
        }
        apush(
            history->links,
            ((ed_link_ref){ av_get_uint(from),
                            av_get_uint(output),
                            av_get_uint(to),
                            av_get_uint(input) })
        );
    }
    if (!checkpoint_read_location(av_obj_get(value, "location"), &history->location)) {
        return false;
    }
    return checkpoint_selection_valid(history);
}

bool editor_restore_checkpoint(editor *e, const char *json)
{
    av_doc *doc = av_json_read(json);
    av_value *root = av_doc_root(doc);
    ed_document_history current = { 0 }, *documents = NULL;
    behavior_asset *asset = NULL;
    bool ok = false;
    av_value *stored = av_obj_get(root, "documents");
    if (av_get_int(av_obj_get(root, "version")) != 3 || !av_is_arr(stored) ||
        !checkpoint_read_document(e, av_obj_get(root, "active"), &current)) {
        goto done;
    }
    asset = current.presentation ? NULL : behavior_asset_clone(current.asset);
    if (asset && current.owner_identity) {
        asset->identity = current.owner_identity;
    }
    size_t i, count;
    av_value *item;
    av_arr_foreach(stored, i, count, item)
    {
        ed_document_history history = { 0 };
        bool valid = checkpoint_read_document(e, item, &history);
        apush(documents, history);
        if (!valid || !strcmp(history.key, current.key)) {
            goto done;
        }
        for (int j = 0; j < acount(documents) - 1; j++) {
            if (!strcmp(documents[j].key, history.key)) {
                goto done;
            }
        }
    }
    if (current.presentation) {
        if (editor_source_presentation(e, current.presentation) ||
            editor_open_graph(e, current.location.graph_id)) {
            goto done;
        }
    } else if (editor_open_behavior_asset(e, asset, current.location.graph_id) != 0) {
        goto done;
    }
    for (int j = 0; j < acount(e->documents); j++) {
        checkpoint_free_document(&e->documents[j]);
    }
    afree(e->documents);
    e->documents = documents;
    documents = NULL;
    free(e->document_key);
    av_value_free(e->saved_snapshot);
    ed_free_asset_stack(&e->undo);
    ed_free_asset_stack(&e->redo);
    e->document_key = current.key;
    e->saved_snapshot = current.saved;
    e->undo = current.undo;
    e->redo = current.redo;
    current.key = NULL;
    current.saved = NULL;
    current.undo = current.redo = NULL;
    ok = restore_location(e, &current.location, current.arrange_pending) == 0;
    for (int j = 0; j < acount(current.links); j++) {
        apush(e->selected_links, current.links[j]);
    }
done:
    checkpoint_free_document(&current);
    for (int j = 0; j < acount(documents); j++) {
        checkpoint_free_document(&documents[j]);
    }
    afree(documents);
    behavior_asset_free(asset);
    av_doc_free(doc);
    return ok;
}
