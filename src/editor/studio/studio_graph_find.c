#include "graph/authoring/graph_edit.h"
/* Find in graph (Unreal's Find Results, Blender's Find Node): a field over the
 * canvas lists the nodes of the open behavior whose title, pins, values,
 * bindings, expression or comment contain the query, graph by graph. Up and
 * Down step through them in the graph; Enter or a click jumps to one, and
 * Escape closes the field, giving the keys back to the graph. The field and
 * its matches are projects/studio's GraphFind over the `graph_find` source. */
#include "editor/studio/studio_graph_internal.h"
#include "graph/authoring/find.h"
#include "support/array.h"
#include "ui/list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    VISIBLE_ROWS = 8,
    /* Rows shown before the List is laid out. */
    UNLAID_ROWS = 20,
};

/* GraphFind's query field and List. */
#define QUERY_ID "graph-find-query"
#define LIST_ID "graph-find-list"

/* Opening shows the bar at the next UI frame, whatever asked for it. */
void studio_graph_find(studio *s)
{
    studio_graph *g = &s->graph;
    if (!g->find.open) {
        g->find.list = (ui_list_state)UI_LIST_STATE_INIT;
    }
    g->find.open = true;
    g->find.focus = true;
    s->ui_schedule.wake = true;
}

static void close_find(studio *s)
{
    s->graph.find.open = false;
    asetlen(s->graph.find.hits, 0);
}

/* A match on an inline node points at the node that draws it. */
static void add_hits(studio *s, graph *gr)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        char title[96];
        graph_find_match match;
        if (!n->desc || !graph_find_node(
                            e->asset,
                            gr,
                            n,
                            ed_node_title(e, n, title, sizeof title),
                            g->find.query,
                            &match
                        )) {
            continue;
        }
        node *owner = graph_inline_owner(gr, n);
        uint32_t icon = sg_node_icon(s, owner);
        studio_find_hit hit = { .graph_id = gr->graph_id,
                                .node_id = owner->id,
                                .icon = icon ? icon : KIT_ICON_HEXAGON };
        ed_node_title(e, owner, hit.title, sizeof hit.title);
        snprintf(hit.text, sizeof hit.text, "%s", match.text);
        apush(g->find.hits, hit);
    }
}

static bool same_hit(const studio_find_hit *a, const studio_find_hit *b)
{
    return a->graph_id == b->graph_id && a->node_id == b->node_id &&
           !strcmp(a->text, b->text);
}

/* Results keep the behavior's graph order and follow the graph as it
   changes, the current one by identity; a new query starts before the first
   without moving the view (cmdk). */
static void collect(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    studio_find_hit shown = { 0 };
    bool showing = g->find.current >= 0 && g->find.current < acount(g->find.hits);
    if (showing) {
        shown = g->find.hits[g->find.current];
    }
    asetlen(g->find.hits, 0);
    for (int i = 0; i < acount(e->asset->graphs); i++) {
        if (!e->asset->graphs[i]->math_expression) {
            add_hits(s, e->asset->graphs[i]);
        }
    }
    g->find.current = -1;
    for (int i = 0; showing && i < acount(g->find.hits); i++) {
        g->find.current = same_hit(&g->find.hits[i], &shown) ? i : g->find.current;
    }
    if (strcmp(g->find.seen, g->find.query)) {
        snprintf(g->find.seen, sizeof g->find.seen, "%s", g->find.query);
        g->find.current = -1;
    }
}

static void jump(studio *s, int index)
{
    const studio_find_hit *hit = &s->graph.find.hits[index];
    s->graph.find.current = index;
    studio_reveal_node(s, hit->graph_id, hit->node_id);
}

/* Steps through the results in the graph, wrapping at either end. A step
   keeps the zoom and pans only to bring an off-screen result into view. */
static void step(studio *s, int direction)
{
    int count = acount(s->graph.find.hits), current = s->graph.find.current;
    if (!count) {
        return;
    }
    int next = current < 0 ? (direction > 0 ? 0 : count - 1) : current + direction;
    jump(s, (next + count) % count);
    s->graph.reveal_frame = false;
}

/* Closing gives the keyboard back to the graph's area. */
static void leave(studio *s)
{
    close_find(s);
    s->focus_area = s->graph.area;
}

void studio_graph_find_action(studio *s, const char *action, const ui_activation *a)
{
    studio_graph *g = &s->graph;
    if (!g->find.open || !studio_graph_active(s)) {
        return;
    }
    int count = acount(g->find.hits);
    if (!strcmp(action, "next") || !strcmp(action, "previous")) {
        step(s, !strcmp(action, "next") ? 1 : -1);
    } else if (!strcmp(action, "jump") && a->argc) {
        int index = atoi(a->args[0]);
        if (index >= 0 && index < count) {
            jump(s, index);
        }
    } else if (!strcmp(action, "enter") && count) {
        jump(s, g->find.current < 0 ? 0 : g->find.current);
        leave(s);
    } else if (!strcmp(action, "close")) {
        leave(s);
    }
    /* A scroll only wakes the source, which reads where the List is. */
}

/* ---------- The source ---------- */

static const char *const ROW_TEXTS[] = { "key", "label", "icon", "meta", NULL };
static const char *const ROW_FLAGS[] = { "parent", "open",   "selected",
                                         "active", "marked", "hidden",
                                         NULL };

static yyjson_mut_val *row_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    for (const char *const *text = ROW_TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, row, *text, "");
    }
    for (const char *const *flag = ROW_FLAGS; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, row, *flag, false);
    }
    yyjson_mut_obj_add_int(doc, row, "depth", 0);
    return row;
}

void studio_graph_find_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *f = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "graph_find");
    yyjson_mut_obj_add_strcpy(doc, f, "query", "");
    yyjson_mut_obj_add_strcpy(doc, f, "count", "");
    yyjson_mut_obj_add_strcpy(doc, f, "height", "0dp");
    yyjson_mut_obj_add_strcpy(doc, f, "none", "");
    yyjson_mut_obj_add_int(doc, f, "total", 0);
    yyjson_mut_obj_add_arr(doc, f, "rows");
}

static const char *graph_label(const graph *gr)
{
    return gr->name && gr->name[0] ? gr->name : "Event Graph";
}

/* A match names what matched when that is not the node's title, and the
   graph it is in when that is not the open one. */
static bool show_row(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *row, int index)
{
    const studio_find_hit *hit = &s->graph.find.hits[index];
    const graph *gr = behavior_asset_find_graph(sg_graph_editor(s)->asset, hit->graph_id);
    bool elsewhere = hit->graph_id != sg_active_graph(s)->graph_id;
    bool text = strcmp(hit->text, hit->title) != 0;
    char key[16], meta[256], glyph[5];
    snprintf(key, sizeof key, "%d", index);
    snprintf(
        meta,
        sizeof meta,
        "%s%s%s",
        text ? hit->text : "",
        elsewhere && text ? " \xc2\xb7 " : "",
        elsewhere && gr ? graph_label(gr) : ""
    );
    studio_glyph(glyph, hit->icon);
    bool changed = ui_data_put_text(doc, row, "key", key);
    changed |= ui_data_put_text(doc, row, "label", hit->title);
    changed |= ui_data_put_text(doc, row, "icon", glyph);
    changed |= ui_data_put_text(doc, row, "meta", meta);
    return ui_data_put_bool(row, "selected", index == s->graph.find.current) || changed;
}

/* GraphFind's List, or NULL before it is laid out. */
static rml_element *find_list(studio *s)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), LIST_ID);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* The List's window of matches, as tall as eight rows at most, scrolled to
   show the current one. */
static bool show_rows(studio *s, const studio_panel_sync *at)
{
    studio_graph *g = &s->graph;
    yyjson_mut_val *array = yyjson_mut_obj_get(at->variable, "rows");
    int count = acount(g->find.hits);
    rml_element *list = at->preview ? NULL : find_list(s);
    /* Before the List lays out, its window is a guess: window it once it
       has. */
    s->ui_schedule.wake |= !at->preview && !list && count;
    int first = 0, length = count < UNLAID_ROWS ? count : UNLAID_ROWS;
    float row = 0;
    if (list) {
        ui_list window = { .count = count, .reveal = g->find.current };
        length = ui_list_window(list, &window, &g->find.list, &first);
        char value[32] = "";
        rml_element_property(list, "--row", value, sizeof value);
        row = strtof(value, NULL);
    }
    char height[32];
    snprintf(height, sizeof height, "%.0fdp", row * (count < VISIBLE_ROWS ? count : VISIBLE_ROWS));
    bool changed = ui_data_put_text(at->doc, at->variable, "height", height);
    changed |= ui_data_put_int(at->variable, "total", count);
    int had = (int)yyjson_mut_arr_size(array);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(array, row_new(at->doc));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(array);
    }
    changed |= had != length;
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = first; (item = yyjson_mut_arr_iter_next(&it)); i++) {
        changed |= show_row(s, at->doc, item, i);
    }
    return changed;
}

/* The query is view state the field binds; C keeps it too, so the matches
   follow it and it outlasts the field. */
static bool sync_query(studio *s, const studio_panel_sync *at)
{
    studio_graph *g = &s->graph;
    yyjson_mut_val *query = yyjson_mut_obj_get(at->variable, "query");
    if (at->wrote && !at->preview && strcmp(yyjson_mut_get_str(query), g->find.query)) {
        snprintf(g->find.query, sizeof g->find.query, "%s", yyjson_mut_get_str(query));
    }
    return ui_data_put_text(at->doc, at->variable, "query", g->find.query);
}

bool studio_graph_find_sync(studio *s, const studio_panel_sync *at)
{
    studio_graph *g = &s->graph;
    if (!g->find.open || !studio_graph_active(s)) {
        return false;
    }
    bool changed = sync_query(s, at);
    collect(s);
    int count = acount(g->find.hits);
    char label[32] = "";
    if (g->find.query[0] && !count) {
        snprintf(label, sizeof label, "No results");
    } else if (g->find.current >= 0) {
        snprintf(label, sizeof label, "%d/%d", g->find.current + 1, count);
    } else if (count) {
        snprintf(label, sizeof label, "%d", count);
    }
    changed |= ui_data_put_text(at->doc, at->variable, "count", label);
    return show_rows(s, at) || changed;
}

/* The panel floats over the canvas's top right, in the Studio document so it
   stays unscaled. Find (Mod+F) focuses its field, selecting the query, once
   the field can take focus. */
void sg_find_panel(studio *s)
{
    studio_graph *g = &s->graph;
    if (!g->find.open) {
        return;
    }
    kit *k = s->kit;
    kit_push(k, "find", NULL, "find-widget");
    rml_element *host = studio_ui_panel(s, "bar", "GraphFind");
    kit_pop(k);
    rml_element *field = host && g->find.focus ? rml_element_by_id(host, QUERY_ID)
                                               : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        g->find.focus = false;
    }
}

/* Framing waits for the frame's layout, so a node in a graph just opened
   frames too. */
void studio_reveal_node(studio *s, uint64_t graph_id, uint64_t node_id)
{
    editor *e = sg_graph_editor(s);
    if (!studio_graph_active(s) || editor_open_graph(e, graph_id) != 0) {
        return;
    }
    node *n = sg_node_by_id(s, node_id);
    if (!n) {
        return;
    }
    n = graph_inline_owner(sg_active_graph(s), n);
    sg_select_only(e, n);
    e->front_node = n;
    s->graph.reveal = n->id;
    s->graph.reveal_frame = true;
}
