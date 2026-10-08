#include "graph/authoring/graph_edit.h"
/* Graph mini-maps for asset thumbnails: the node boxes graph_layout
 * arranges, in the canvas's category colors, and their wires, as Blender's
 * node-group and Unreal's material-function thumbnails draw a graph whole. */
#include "editor/studio/studio_thumbnails.h"
#include "graph/authoring/layout.h"
#include "platform/cute.h"
#include "support/array.h"

#include <math.h>

#define MARGIN_FRACTION 0.1f
/* Wires leave and enter their boxes level, for this share of their run. */
#define WIRE_REACH 0.5f

/* The canvas's node categories (theme/graph.rcss), lifted to read on the
 * dark stage at a tile's size. */
static uint32_t category_color(const graph *g, const node *n)
{
    if (graph_node_is_inline(g, n)) {
        return 0;
    }
    switch (n->desc->display_class) {
        case NODE_DISPLAY_EVENT: return 0xc4566fffu;
        case NODE_DISPLAY_FUNCTION: return 0x4f86c9ffu;
        case NODE_DISPLAY_FLOW: return 0x8d8d99ffu;
        case NODE_DISPLAY_VARIABLE: return 0xb9764fffu;
        case NODE_DISPLAY_LITERAL: return 0xc6a24affu;
        default: return 0x4f9a63ffu;
    }
}

static int box_index(const graph *g, uint64_t id)
{
    for (int i = 0; i < acount(g->nodes); i++) {
        if (g->nodes[i]->id == id) {
            return i;
        }
    }
    return -1;
}

/* Boxes in graph units, NULL entries for inline nodes, which have none. */
static void graph_boxes(graph *g, studio_minimap_box *boxes)
{
    for (int i = 0; i < acount(g->nodes); i++) {
        const node *n = g->nodes[i];
        arrange_box box = graph_layout_box(g, n);
        boxes[i] = (studio_minimap_box){ n->x,
                                         n->y,
                                         box.width,
                                         box.height,
                                         category_color(g, n) };
    }
}

static void add_wires(const graph *g, const studio_minimap_box *boxes, studio_minimap *out)
{
    for (int i = 0; i < acount(g->edges); i++) {
        const edge *e = &g->edges[i];
        int from = box_index(g, e->from_id), to = box_index(g, e->to_id);
        int from_pin, to_pin;
        if (from < 0 || to < 0 || !boxes[from].color || !boxes[to].color ||
            !graph_pin_by_id(g->nodes[from], e->from_pin_id, NULL, &from_pin) ||
            !graph_pin_by_id(g->nodes[to], e->to_pin_id, NULL, &to_pin)) {
            continue;
        }
        const studio_minimap_box *a = &boxes[from], *b = &boxes[to];
        apush(
            out->wires,
            ((studio_minimap_wire){
                a->x + a->w,
                a->y + graph_layout_pin_y(g, g->nodes[from], true, from_pin),
                b->x,
                b->y + graph_layout_pin_y(g, g->nodes[to], false, to_pin),
                e->kind == GRAPH_EDGE_EXEC })
        );
    }
}

void studio_minimap_build(graph *g, float side, studio_minimap *out)
{
    *out = (studio_minimap){ 0 };
    int count = acount(g->nodes);
    if (!count) {
        return;
    }
    graph_place_unplaced(g);
    studio_minimap_box *boxes = NULL;
    asetlen(boxes, count);
    graph_boxes(g, boxes);
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (int i = 0; i < count; i++) {
        if (!boxes[i].color) {
            continue;
        }
        min_x = fminf(min_x, boxes[i].x);
        min_y = fminf(min_y, boxes[i].y);
        max_x = fmaxf(max_x, boxes[i].x + boxes[i].w);
        max_y = fmaxf(max_y, boxes[i].y + boxes[i].h);
    }
    if (min_x > max_x) {
        afree(boxes);
        return;
    }
    add_wires(g, boxes, out);
    float inner = side * (1 - 2 * MARGIN_FRACTION);
    float width = max_x - min_x, height = max_y - min_y;
    float scale = inner / fmaxf(width, height);
    float left = (side - width * scale) * 0.5f, top = (side - height * scale) * 0.5f;
    for (int i = 0; i < count; i++) {
        studio_minimap_box b = boxes[i];
        if (!b.color) {
            continue;
        }
        apush(
            out->boxes,
            ((studio_minimap_box){ left + (b.x - min_x) * scale,
                                   top + (b.y - min_y) * scale,
                                   b.w * scale,
                                   b.h * scale,
                                   b.color })
        );
    }
    for (int i = 0; i < acount(out->wires); i++) {
        studio_minimap_wire *w = &out->wires[i];
        *w = (studio_minimap_wire){ left + (w->x0 - min_x) * scale,
                                    top + (w->y0 - min_y) * scale,
                                    left + (w->x1 - min_x) * scale,
                                    top + (w->y1 - min_y) * scale,
                                    w->flow };
    }
    afree(boxes);
}

void studio_minimap_free(studio_minimap *m)
{
    afree(m->boxes);
    afree(m->wires);
    *m = (studio_minimap){ 0 };
}

static CF_Color color_of(uint32_t rgba, float alpha)
{
    return cf_make_color_rgba(
        rgba >> 24,
        rgba >> 16 & 255,
        rgba >> 8 & 255,
        (uint8_t)((rgba & 255) * alpha)
    );
}

/* The square's y runs down; the target's runs up. */
static CF_V2 at(float side, float x, float y)
{
    return (CF_V2){ x, side - y };
}

void studio_minimap_draw(const studio_minimap *m, float side)
{
    float stroke = fmaxf(1.0f, side / 160);
    for (int i = 0; i < acount(m->wires); i++) {
        const studio_minimap_wire *w = &m->wires[i];
        cf_draw_push_color(
            color_of(w->flow ? 0xe6e6eaffu : 0x9aa3b2ffu, w->flow ? 0.85f : 0.6f)
        );
        CF_V2 a = at(side, w->x0, w->y0), b = at(side, w->x1, w->y1);
        float reach = fmaxf(fabsf(b.x - a.x) * WIRE_REACH, stroke * 4);
        cf_draw_bezier_line2(a, (CF_V2){ a.x + reach, a.y }, (CF_V2){ b.x - reach, b.y }, b, 12, stroke);
        cf_draw_pop_color();
    }
    for (int i = 0; i < acount(m->boxes); i++) {
        const studio_minimap_box *b = &m->boxes[i];
        float head = fminf(b->h, fmaxf(stroke * 2, b->h * 0.2f));
        float radius = fminf(stroke * 2, b->w * 0.1f);
        CF_Aabb body = { at(side, b->x, b->y + b->h), at(side, b->x + b->w, b->y) };
        CF_Aabb title = { at(side, b->x, b->y + head), at(side, b->x + b->w, b->y) };
        cf_draw_push_color(color_of(0x2a2c33ffu, 1));
        cf_draw_box_rounded_fill(body, radius);
        cf_draw_pop_color();
        cf_draw_push_color(color_of(b->color, 1));
        cf_draw_box_rounded_fill(title, radius);
        cf_draw_pop_color();
        cf_draw_push_color(color_of(0x5a5e6aff, 1));
        cf_draw_box_rounded(body, stroke * 0.5f, radius);
        cf_draw_pop_color();
    }
}
