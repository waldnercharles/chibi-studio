#include "editor/workspace/editor_internal.h"

#include <string.h>

static int ed_link_equal(const ed_link_ref *a, const ed_link_ref *b)
{
    return a->from_id == b->from_id && a->from_pin_id == b->from_pin_id &&
           a->to_id == b->to_id && a->to_pin_id == b->to_pin_id;
}

int ed_edge_index(const editor *e, const ed_link_ref *link)
{
    for (int i = 0; i < acount(e->active_graph->edges); i++) {
        const edge *ed = &e->active_graph->edges[i];
        if (ed->from_id == link->from_id && ed->from_pin_id == link->from_pin_id &&
            ed->to_id == link->to_id && ed->to_pin_id == link->to_pin_id) {
            return i;
        }
    }
    return -1;
}

void ed_clear_selection(editor *e)
{
    aclear(e->selected);
    aclear(e->selected_links);
}

void ed_reset_node_interaction(editor *e)
{
    ed_clear_selection(e);
    e->front_node = NULL;
    e->spawn_link_pin = (ed_pin_view){ 0 };
    e->spawn_link_pending = 0;
    for (int i = acount(e->graph_callers) - 1; i >= 0; i--) {
        const ed_graph_caller *caller = &e->graph_callers[i];
        graph *g = behavior_asset_find_graph(e->asset, caller->graph_id);
        if (g && graph_node_by_id(g, caller->node_id)) {
            continue;
        }
        adel(e->graph_callers, i);
    }
}

int ed_switch_graph(editor *e, graph *graph)
{
    if (!e || !graph || graph->owner != e->asset) {
        return -1;
    }
    if (e->active_graph == graph) {
        return 0;
    }
    e->active_graph = graph;
    e->arrange_pending = true;
    ed_clear_selection(e);
    e->front_node = NULL;
    e->spawn_link_pending = 0;
    return 0;
}

int ed_link_selected(const editor *e, const ed_link_ref *link)
{
    for (int i = 0; i < acount(e->selected_links); i++) {
        if (ed_link_equal(link, &e->selected_links[i])) {
            return 1;
        }
    }
    return 0;
}

void ed_add_selected_link(editor *e, const ed_link_ref *link)
{
    if (ed_link_selected(e, link)) {
        return;
    }
    apush(e->selected_links, *link);
}

int ed_node_selected(const editor *e, const node *n)
{
    for (int i = 0; i < acount(e->selected); i++) {
        if (e->selected[i] == n) {
            return 1;
        }
    }
    return 0;
}

void ed_add_selected(editor *e, node *n)
{
    if (ed_node_selected(e, n)) {
        return;
    }
    apush(e->selected, n);
}

void ed_remove_selected(editor *e, node *n)
{
    for (int i = 0; i < acount(e->selected); i++) {
        if (e->selected[i] != n) {
            continue;
        }
        adel(e->selected, i);
        return;
    }
}

void ed_select_node(editor *e, node *n, int append)
{
    if (!append) {
        aclear(e->selected);
        aclear(e->selected_links);
    }
    if (n) {
        ed_add_selected(e, n);
        e->front_node = n;
    }
}
