/* Call Hierarchy (VS Code) in Output's Calls view: a function's or event's
 * callers or callees as a tree, from the shared call hierarchy. A handler's
 * row stands for its event among callers. A row opens the call, send or
 * Event node that joins it to its parent; a row whose children the trace
 * left out ends with More, which traces again from it. Output shows the
 * rows (studio_output.c): a click selects a row, a double click or Enter
 * opens it, and the arrows walk and fold the tree. */
#include "assets/call_hierarchy.h"
#include "authoring/source_navigation.h"
#include "editor/studio/studio_graph_internal.h"
#include "graph/document/events.h"
#include "support/array.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Among callers a handler and its event take two levels, so this shows six
   hops through events. */
enum
{
    CALLS_DEPTH = 12,
};

static const char *asset_name(const reference_index *index, int asset)
{
    const reference_asset *found = reference_index_asset_at(index, asset);
    return found ? found->name : "";
}

static const char *item_label(const reference_index *index, const call_item *item)
{
    switch (item->kind) {
        case CALL_FUNCTION: return item->name ? item->name : "Unresolved";
        case CALL_GRAPH: return item->name ? item->name : "Event Graph";
        case CALL_PLACE:
            return item->name ? item->name : asset_name(index, item->asset);
        case CALL_HANDLER:
        case CALL_EVENT: return item->name;
    }
    return "";
}

static uint32_t item_icon(const reference_index *index, const call_item *item)
{
    switch (item->kind) {
        case CALL_FUNCTION: return KIT_ICON_FUNCTION;
        case CALL_GRAPH: return KIT_ICON_GRAPH;
        case CALL_PLACE:
            return studio_asset_kind_icon(
                (int)reference_index_asset_at(index, item->asset)->kind
            );
        case CALL_HANDLER:
        case CALL_EVENT: return KIT_ICON_EVENT;
    }
    return KIT_ICON_GRAPH;
}

/* Events are named, host events marked; everything else sits in an asset. */
static const char *item_meta(const reference_index *index, const call_item *item)
{
    if (item->kind != CALL_EVENT) {
        return asset_name(index, item->asset);
    }
    return host_event_reserved(item->name) ? "Host" : "";
}

/* A function or an event as a root to trace, by name; a handler traces as
   its event. */
static studio_call_root root_of(const reference_index *index, const call_item *item)
{
    assert(item->kind == CALL_FUNCTION || item->kind == CALL_HANDLER || item->kind == CALL_EVENT);
    bool function = item->kind == CALL_FUNCTION;
    const reference_asset *asset = function
                                     ? reference_index_asset_at(index, item->asset)
                                     : NULL;
    studio_call_root root = { .kind = function ? REFERENCE_SYMBOL_GRAPH
                                               : REFERENCE_SYMBOL_EVENT,
                              .asset_kind = asset ? (int)asset->kind : 0,
                              .graph_id = item->graph_id };
    snprintf(root.title, sizeof root.title, "%s", item_label(index, item));
    snprintf(root.asset, sizeof root.asset, "%s", asset ? asset->name : "");
    return root;
}

/* Appends at's children as rows at depth, each followed by its own. Among
   callers a handler's only child is its event, so the handler row stands for
   it and shows the event's senders. */
static void add_rows(studio *s, const reference_index *index, const call_item *items, int at, int depth)
{
    for (int i = items[at].first; i < items[at].first + items[at].count; i++) {
        const call_item *item = &items[i];
        int below = !s->calls.callees && item->kind == CALL_HANDLER && item->count == 1
                      ? item->first
                      : i;
        studio_call row = { .icon = item_icon(index, item),
                            .at = studio_reference_of(index, item->sites[0]),
                            .depth = depth,
                            .open = depth == 0,
                            .parent = items[below].count > 0 || items[below].more,
                            .cycle = item->cycle || items[below].cycle,
                            .more = items[below].more };
        if (row.more) {
            row.from = root_of(index, &items[below]);
        }
        snprintf(row.label, sizeof row.label, "%s", item_label(index, item));
        snprintf(row.meta, sizeof row.meta, "%s", item_meta(index, item));
        apush(s->calls.rows, row);
        add_rows(s, index, items, below, depth + 1);
    }
}

static reference_symbol root_symbol(const studio_call_root *root, const reference_index *index)
{
    if (root->kind == REFERENCE_SYMBOL_EVENT) {
        return (reference_symbol){ .kind = REFERENCE_SYMBOL_EVENT,
                                   .asset = -1,
                                   .name = root->title };
    }
    return (reference_symbol){
        .kind = REFERENCE_SYMBOL_GRAPH,
        .asset = reference_index_find_name(index, (reference_asset_kind)root->asset_kind, root->asset),
        .id = root->graph_id,
    };
}

static void trace(studio *s)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    call_item *items = call_hierarchy(
        index,
        root_symbol(&s->calls.root, index),
        s->calls.callees,
        CALLS_DEPTH
    );
    asetlen(s->calls.rows, 0);
    add_rows(s, index, items, 0, 0);
    call_hierarchy_free(items);
    s->calls.selected = -1;
    studio_output_calls(s);
}

/* Traces the callers of a function or event the index names. */
static void trace_symbol(studio *s, reference_symbol symbol, const char *title)
{
    const reference_asset *asset = reference_index_asset_at(
        app_scene_authoring_asset_index(s->scene),
        symbol.asset
    );
    if (symbol.kind != REFERENCE_SYMBOL_EVENT &&
        (symbol.kind != REFERENCE_SYMBOL_GRAPH || !asset)) {
        studio_report(s, "Not in the project");
        return;
    }
    s->calls.source_path[0] = 0;
    studio_call_root *root = &s->calls.root;
    *root = (studio_call_root){ .kind = symbol.kind,
                                .asset_kind = asset ? (int)asset->kind : 0,
                                .graph_id = symbol.id };
    snprintf(root->asset, sizeof root->asset, "%s", asset ? asset->name : "");
    snprintf(root->title, sizeof root->title, "%s", title);
    s->calls.callees = 0;
    trace(s);
}

static asset_code *source_code(studio *s, const char *path)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        if (!strcmp(path, asset_code_path(s->code.assets[i]))) {
            return s->code.assets[i];
        }
    }
    return NULL;
}

static studio_reference source_place(const authoring_navigation *index, authoring_location at)
{
    const authoring_document *document = authoring_navigation_document_at(index, at.document);
    studio_reference place = { .ui = { .line = 1, .offset = at.range.start } };
    snprintf(place.ui.path, sizeof place.ui.path, "%s", document->path);
    const char *text = authoring_navigation_document_text(index, at.document);
    for (int i = 0; i < at.range.start && text[i]; i++) {
        place.ui.line += text[i] == '\n';
    }
    return place;
}

static void source_rows(
    studio *s,
    const authoring_navigation *index,
    authoring_target target,
    authoring_location at,
    authoring_target *ancestors,
    int depth
)
{
    if (acount(s->calls.rows) >= 512) {
        return;
    }
    bool cycle = false;
    for (int i = 0; i < depth; i++) {
        cycle |= authoring_target_equal(ancestors[i], target);
    }
    studio_call row = { .icon = KIT_ICON_FUNCTION,
                        .at = source_place(index, at),
                        .depth = depth,
                        .open = depth == 0,
                        .cycle = cycle };
    const char *name = authoring_navigation_name(index, target);
    snprintf(row.label, sizeof row.label, "%s", name ? name : "Handler");
    int row_index = acount(s->calls.rows);
    apush(s->calls.rows, row);
    if (cycle || depth == CALLS_DEPTH) {
        return;
    }
    ancestors[depth] = target;
    for (int i = 0; i < authoring_navigation_call_count(index); i++) {
        const authoring_call *call = authoring_navigation_call_at(index, i);
        authoring_target caller = authoring_navigation_caller(index, call);
        if (!authoring_target_equal(s->calls.callees ? caller : call->callee, target)) {
            continue;
        }
        s->calls.rows[row_index].parent = true;
        source_rows(s, index, s->calls.callees ? call->callee : caller, call->location, ancestors, depth + 1);
    }
}

void studio_calls_source(studio *s, const char *path, int offset, bool callees)
{
    asset_code *code = source_code(s, path);
    const authoring_navigation *index = code ? asset_code_navigation(code) : NULL;
    const authoring_reference
        *reference = index
                       ? authoring_navigation_at(index, asset_code_document(code), offset)
                       : NULL;
    authoring_target target = reference
                                ? authoring_navigation_callable(index, reference->target)
                                : (authoring_target){ 0 };
    if (!target.id) {
        studio_report(s, "No callable symbol at this position");
        return;
    }
    if (path != s->calls.source_path) {
        snprintf(s->calls.source_path, sizeof s->calls.source_path, "%s", path);
    }
    s->calls.source_offset = offset;
    s->calls.callees = callees;
    const char *name = authoring_navigation_name(index, target);
    snprintf(s->calls.root.title, sizeof s->calls.root.title, "%s", name ? name : "Handler");
    asetlen(s->calls.rows, 0);
    authoring_target ancestors[CALLS_DEPTH + 1];
    source_rows(s, index, target, reference->location, ancestors, 0);
    s->calls.selected = -1;
    studio_output_calls(s);
}

void studio_calls_node(studio *s, const node *n)
{
    const char *title;
    reference_symbol symbol = studio_node_symbol(s, n, &title);
    trace_symbol(s, symbol, title);
}

void studio_calls_library(studio *s, const char *asset, uint64_t graph_id, const char *title)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    reference_symbol symbol = {
        .kind = REFERENCE_SYMBOL_GRAPH,
        .asset = reference_index_find_name(index, REFERENCE_ASSET_LIBRARY, asset),
        .id = graph_id,
    };
    trace_symbol(s, symbol, title);
}

void studio_calls_graph(studio *s, const graph *gr)
{
    reference_symbol symbol = { .kind = REFERENCE_SYMBOL_GRAPH,
                                .asset = studio_refresh_document_asset(s),
                                .id = gr->graph_id };
    trace_symbol(s, symbol, gr->name);
}

void studio_calls_clear(studio *s)
{
    asetlen(s->calls.rows, 0);
    s->calls.root.title[0] = '\0';
    s->calls.source_path[0] = '\0';
    s->calls.selected = -1;
}

/* Ends the subtrees of rows at depth or deeper with their More rows,
   written as -1 - row. */
static void end_subtrees(const studio_call *rows, int **more, int depth, int **shown)
{
    while (acount(*more) && rows[alast(*more)].depth >= depth) {
        apush(*shown, -1 - apop(*more));
    }
}

int *studio_calls_rows(const studio *s)
{
    const studio_call *rows = s->calls.rows;
    int *shown = NULL, *more = NULL;
    int closed = -1; /* depth of the closed row being skipped past */
    for (int i = 0; i < acount(rows); i++) {
        if (closed >= 0 && rows[i].depth > closed) {
            continue;
        }
        end_subtrees(rows, &more, rows[i].depth, &shown);
        closed = rows[i].open ? -1 : rows[i].depth;
        if (!studio_matches(s->output_filter, rows[i].label) &&
            !studio_matches(s->output_filter, rows[i].meta)) {
            continue;
        }
        apush(shown, i);
        if (rows[i].open && rows[i].more) {
            apush(more, i);
        }
    }
    end_subtrees(rows, &more, 0, &shown);
    afree(more);
    return shown;
}

/* A More row traces again from the row it ends, listing the children the
   trace left out; that replaces the rows. */
static void more(studio *s, int row)
{
    s->calls.root = s->calls.rows[row].from;
    trace(s);
}

/* Rows may have changed since they were shown: keys past them do nothing. */
void studio_calls_click(studio *s, int key)
{
    if (key < 0 && -1 - key < acount(s->calls.rows)) {
        more(s, -1 - key);
        return;
    }
    if (key < acount(s->calls.rows)) {
        s->calls.selected = key;
    }
}

void studio_calls_open(studio *s, int key)
{
    if (key < 0 || key >= acount(s->calls.rows)) {
        studio_calls_click(s, key);
        return;
    }
    s->calls.selected = key;
    studio_open_reference(s, &s->calls.rows[key].at);
}

void studio_calls_direction(studio *s, bool callees)
{
    if (s->calls.callees != callees && s->calls.root.title[0]) {
        s->calls.callees = callees;
        if (s->calls.source_path[0]) {
            studio_calls_source(s, s->calls.source_path, s->calls.source_offset, callees);
        } else {
            trace(s);
        }
    }
}

/* ---------- Keys ---------- */

/* The shown calls, without their More rows, and where the selection is
   among them, or -1. */
static int *shown_calls(const studio *s, int *at)
{
    int *rows = studio_calls_rows(s), *calls = NULL;
    *at = -1;
    for (int i = 0; i < acount(rows); i++) {
        if (rows[i] >= 0) {
            *at = rows[i] == s->calls.selected ? acount(calls) : *at;
            apush(calls, rows[i]);
        }
    }
    afree(rows);
    return calls;
}

/* The row's parent: the nearest row above it one level up, or -1. */
static int parent_of(const studio *s, int row)
{
    for (int i = row - 1; i >= 0; i--) {
        if (s->calls.rows[i].depth < s->calls.rows[row].depth) {
            return i;
        }
    }
    return -1;
}

/* Right opens a closed row, then steps to its first child; Left closes an
   open row, then steps to its parent. */
static int sideways(studio *s, const int *calls, int at, bool right)
{
    studio_call *row = &s->calls.rows[calls[at]];
    if (right && row->parent && !row->open) {
        row->open = true;
        return calls[at];
    }
    if (right) {
        bool child = at + 1 < acount(calls) &&
                     s->calls.rows[calls[at + 1]].depth > row->depth;
        return child ? calls[at + 1] : calls[at];
    }
    if (row->parent && row->open) {
        row->open = false;
        return calls[at];
    }
    int parent = parent_of(s, calls[at]);
    return parent >= 0 ? parent : calls[at];
}

/* A tree's keys (VS Code, WAI-ARIA's tree view): Up and Down step through
   the shown calls, Home and End go to the ends, Left and Right fold or step
   out and in, and Enter opens the selected call. */
bool studio_calls_key(studio *s, SDL_Keycode key)
{
    int at, *calls = shown_calls(s, &at);
    int count = acount(calls), selected = s->calls.selected;
    bool taken = true;
    switch (key) {
        case SDLK_DOWN:
            selected = count ? calls
                                   [at < 0           ? 0
                                    : at + 1 < count ? at + 1
                                                     : at]
                             : -1;
            break;
        case SDLK_UP:
            selected = count ? calls[at <= 0 ? 0 : at - 1] : -1;
            break;
        case SDLK_HOME: selected = count ? calls[0] : -1; break;
        case SDLK_END: selected = count ? calls[count - 1] : -1; break;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            selected = at >= 0 ? sideways(s, calls, at, key == SDLK_RIGHT) : selected;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (at >= 0) {
                studio_open_reference(s, &s->calls.rows[calls[at]].at);
            }
            break;
        default: taken = false; break;
    }
    afree(calls);
    s->calls.selected = selected;
    s->calls.reveal |= taken;
    s->ui_schedule.wake |= taken;
    return taken;
}
