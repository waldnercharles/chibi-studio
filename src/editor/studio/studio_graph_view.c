/* The graph canvas camera, per-graph views and arrangement. */
#include "editor/studio/studio_graph_internal.h"
#include "graph/authoring/layout.h"
#include "support/array.h"

#include <math.h>

static const float ZOOM_MIN = 0.1f, ZOOM_MAX = 2.0f;
static const double FIT_SECONDS = 0.3;

/* ---------- Camera ---------- */

/* Window px to graph units. */
float sg_zoom(const studio *s)
{
    return s->graph.zoom * s->prefs.standard_font_size / 12.0f;
}

float studio_graph_pixel_ratio(const studio *s)
{
    return sg_zoom(s) * studio_pixel_ratio(s);
}

void sg_to_world(const studio *s, float wx, float wy, float *x, float *y)
{
    const studio_graph *g = &s->graph;
    float zoom = sg_zoom(s);
    *x = g->x + (wx - g->surface.rect.x) / zoom;
    *y = g->y + (wy - g->surface.rect.y) / zoom;
}

void sg_zoom_at(studio *s, float zoom, float wx, float wy)
{
    studio_graph *g = &s->graph;
    zoom = fminf(fmaxf(zoom, ZOOM_MIN), ZOOM_MAX);
    float fx, fy;
    sg_to_world(s, wx, wy, &fx, &fy);
    g->zoom = zoom;
    float effective_zoom = sg_zoom(s);
    g->x = fx - (wx - g->surface.rect.x) / effective_zoom;
    g->y = fy - (wy - g->surface.rect.y) / effective_zoom;
    g->camera.active = false;
    g->wheel_zoomed = s->time;
}

bool sg_far(const studio *s)
{
    return studio_graph_pixel_ratio(s) < FAR_PIXELS;
}

static void camera_to(studio *s, float zoom, float x, float y, bool animate)
{
    studio_graph *g = &s->graph;
    if (!animate || s->prefs.reduced_motion) {
        g->zoom = zoom;
        g->x = x;
        g->y = y;
        g->camera.active = false;
        return;
    }
    g->camera.from_zoom = g->zoom;
    g->camera.from_x = g->x;
    g->camera.from_y = g->y;
    g->camera.zoom = zoom;
    g->camera.x = x;
    g->camera.y = y;
    g->camera.start = s->time;
    g->camera.pan = false;
    g->camera.active = true;
}

/* A reveal keeps the current scale and carries its velocity to a new target. */
static void pan_to(studio *s, float x, float y)
{
    studio_graph *g = &s->graph;
    if (s->prefs.reduced_motion) {
        camera_to(s, g->zoom, x, y, false);
        return;
    }
    bool continuing = g->camera.active && g->camera.pan;
    g->camera.pan_x = (spring){ g->x - x, continuing ? g->camera.pan_x.v : 0 };
    g->camera.pan_y = (spring){ g->y - y, continuing ? g->camera.pan_y.v : 0 };
    g->camera.zoom = g->zoom;
    g->camera.x = x;
    g->camera.y = y;
    g->camera.updated = s->time;
    g->camera.pan = true;
    g->camera.active = g->camera.pan_x.x || g->camera.pan_y.x ||
                       g->camera.pan_x.v || g->camera.pan_y.v;
}

void sg_zoom_actual(studio *s)
{
    studio_graph *g = &s->graph;
    float zoom = sg_zoom(s), w = g->surface.rect.w, h = g->surface.rect.h;
    float cx = g->x + w / 2 / zoom, cy = g->y + h / 2 / zoom;
    camera_to(s, 12.0f / s->prefs.standard_font_size, cx - w / 2, cy - h / 2, true);
}

float sg_zoom_readout(const studio *s)
{
    const studio_graph *g = &s->graph;
    float zoom = g->camera.active ? g->camera.zoom : g->zoom;
    return zoom * s->prefs.standard_font_size / 12.0f;
}

bool sg_wheel_zooming(const studio *s)
{
    return s->time - s->graph.wheel_zoomed < WHEEL_SETTLE;
}

/* Interpolates in screen space so the path neither swoops nor drifts: the
   visible center moves linearly while zoom eases (easeInOutCubic). */
void sg_camera_step(studio *s)
{
    studio_graph *g = &s->graph;
    if (!g->camera.active) {
        return;
    }
    if (s->prefs.reduced_motion) {
        camera_to(s, g->camera.zoom, g->camera.x, g->camera.y, false);
        return;
    }
    if (g->camera.pan) {
        double dt = fmax(0, fmin(s->time - g->camera.updated, 0.1));
        g->camera.updated = s->time;
        spring_params p = spring_for(0.22, 0);
        spring_step(&g->camera.pan_x, p, dt);
        spring_step(&g->camera.pan_y, p, dt);
        g->x = g->camera.x + (float)g->camera.pan_x.x;
        g->y = g->camera.y + (float)g->camera.pan_y.x;
        g->camera.active = g->camera.pan_x.x || g->camera.pan_y.x ||
                           g->camera.pan_x.v || g->camera.pan_y.v;
        return;
    }
    double t = fmin((s->time - g->camera.start) / FIT_SECONDS, 1);
    float u = (float)(t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2);
    float w = g->surface.rect.w, h = g->surface.rect.h;
    float font_scale = s->prefs.standard_font_size / 12.0f;
    float from_zoom = g->camera.from_zoom * font_scale;
    float to_zoom = g->camera.zoom * font_scale;
    float from_cx = g->camera.from_x + w / 2 / from_zoom;
    float from_cy = g->camera.from_y + h / 2 / from_zoom;
    float to_cx = g->camera.x + w / 2 / to_zoom;
    float to_cy = g->camera.y + h / 2 / to_zoom;
    g->zoom = g->camera.from_zoom + (g->camera.zoom - g->camera.from_zoom) * u;
    float zoom = sg_zoom(s);
    g->x = from_cx + (to_cx - from_cx) * u - w / 2 / zoom;
    g->y = from_cy + (to_cy - from_cy) * u - h / 2 / zoom;
    g->camera.active = t < 1;
}

/* The bounds of the selection's nodes, or of every node; false when there
   are none. */
static bool nodes_bounds(studio *s, bool selection, float lo[2], float hi[2])
{
    graph *gr = sg_active_graph(s);
    editor *e = sg_graph_editor(s);
    lo[0] = lo[1] = INFINITY;
    hi[0] = hi[1] = -INFINITY;
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        float box[4];
        if (!sg_node_visible(gr, n) || (selection && !ed_node_selected(e, n)) ||
            !sg_node_box(s, n, box)) {
            continue;
        }
        lo[0] = fminf(lo[0], box[0]);
        lo[1] = fminf(lo[1], box[1]);
        hi[0] = fmaxf(hi[0], box[0] + box[2]);
        hi[1] = fmaxf(hi[1], box[1] + box[3]);
    }
    return lo[0] <= hi[0];
}

/* The zoom fitting the bounds with 10% padding, never past 1:1. */
static float fit_zoom(const studio *s, const float lo[2], const float hi[2])
{
    const studio_graph *g = &s->graph;
    float w = g->surface.rect.w, h = g->surface.rect.h;
    float zoom = fminf(fminf(w / ((hi[0] - lo[0]) * 1.2f), h / ((hi[1] - lo[1]) * 1.2f)), 1);
    return fminf(fmaxf(zoom * 12.0f / s->prefs.standard_font_size, ZOOM_MIN), ZOOM_MAX);
}

/* The camera fitting the selection, or everything; false with none. */
static bool frame_target(studio *s, bool selection, float camera[3])
{
    studio_graph *g = &s->graph;
    float lo[2], hi[2];
    if (!nodes_bounds(s, selection, lo, hi)) {
        return false;
    }
    float w = g->surface.rect.w, h = g->surface.rect.h, zoom = fit_zoom(s, lo, hi);
    float effective_zoom = zoom * s->prefs.standard_font_size / 12.0f;
    camera[0] = zoom;
    camera[1] = (lo[0] + hi[0]) / 2 - w / 2 / effective_zoom;
    camera[2] = (lo[1] + hi[1]) / 2 - h / 2 / effective_zoom;
    return true;
}

/* Fits the selection, or everything, with 10% padding and never past 1:1. */
void sg_frame_nodes(studio *s, bool selection, bool animate)
{
    studio_graph *g = &s->graph;
    float camera[3];
    if (!sg_active_graph(s) || g->surface.rect.w <= 0) {
        return;
    }
    if (!frame_target(s, selection, camera)) {
        if (selection) {
            sg_frame_nodes(s, false, animate);
        }
        return;
    }
    camera_to(s, camera[0], camera[1], camera[2], animate);
}

void sg_frame_again(studio *s)
{
    studio_graph *g = &s->graph;
    bool selection = acount(sg_graph_editor(s)->selected) > 0;
    float camera[3];
    if (selection && sg_active_graph(s) && !g->camera.active &&
        frame_target(s, true, camera) && fabsf(g->zoom - camera[0]) < 1e-3f &&
        fabsf(g->x - camera[1]) < 1 && fabsf(g->y - camera[2]) < 1) {
        selection = false;
    }
    sg_frame_nodes(s, selection, false);
}

/* The room left above and beside a graph's start when it opens at
   OPEN_ZOOM_MIN. */
static const float OPEN_MARGIN = 32;

/* A graph opens fitted when legible, otherwise at its entry. A local value
   may sit above or to the left of the execution flow. */
void sg_frame_open(studio *s)
{
    studio_graph *g = &s->graph;
    float lo[2], hi[2];
    if (!sg_active_graph(s) || g->surface.rect.w <= 0 ||
        !nodes_bounds(s, false, lo, hi) ||
        fit_zoom(s, lo, hi) * s->prefs.standard_font_size / 12.0f >= OPEN_ZOOM_MIN) {
        sg_frame_nodes(s, false, false);
        return;
    }
    float zoom = OPEN_ZOOM_MIN * 12.0f / s->prefs.standard_font_size;
    graph *gr = sg_active_graph(s);
    node *entry = graph_node_by_id(gr, gr->entry_id);
    float box[4];
    if (entry && sg_node_box(s, entry, box)) {
        lo[0] = box[0];
        lo[1] = box[1];
    }
    camera_to(s, zoom, lo[0] - OPEN_MARGIN, lo[1] - OPEN_MARGIN, false);
}

/* Move only far enough to reveal the node with room around it. A node wider
   than the comfortable region is centered rather than chased edge to edge. */
static float reveal_axis(float at, float span, float start, float size)
{
    float inset = span * 0.2f;
    if (size > span - 2 * inset) {
        return start + size / 2 - span / 2;
    }
    if (start < at + inset) {
        return start - inset;
    }
    if (start + size > at + span - inset) {
        return start + size - span + inset;
    }
    return at;
}

/* Source and keyboard navigation move the receiving view without changing
   its zoom. Repeated caret moves in one node keep the current transition. */
void sg_show_node(studio *s, node *n, bool frame)
{
    studio_graph *g = &s->graph;
    if (frame) {
        sg_frame_nodes(s, true, true);
        return;
    }
    /* A previous fit must not carry this new selection away after it is
       already visible. Direct reveals keep the scale currently on screen. */
    if (g->camera.active && !g->camera.pan) {
        g->camera.active = false;
    }
    float zoom = sg_zoom(s);
    float box[4], w = g->surface.rect.w / zoom, h = g->surface.rect.h / zoom;
    sg_node_box(s, n, box);
    float x = g->camera.active && g->camera.pan ? g->camera.x : g->x;
    float y = g->camera.active && g->camera.pan ? g->camera.y : g->y;
    float to_x = reveal_axis(x, w, box[0], box[2]);
    float to_y = reveal_axis(y, h, box[1], box[3]);
    if (fabsf(to_x - x) < 0.5f / zoom && fabsf(to_y - y) < 0.5f / zoom) {
        return;
    }
    pan_to(s, to_x, to_y);
}

static studio_graph_view *view_for(studio_graph *g, uint64_t graph_id)
{
    for (int i = 0; i < acount(g->views); i++) {
        if (g->views[i].graph_id == graph_id) {
            return &g->views[i];
        }
    }
    return NULL;
}

/* Undo replaces the asset and its graphs, so views key on the behavior's name
   and the graph's saved ID rather than on memory. */
static uint64_t graph_key(const studio *s)
{
    graph *gr = sg_active_graph(s);
    if (!gr) {
        return 0;
    }
    uint64_t h = 14695981039346656037ull;
    for (const char *c = s->scene->graph_document_id; *c; c++) {
        h = (h ^ (unsigned char)*c) * 1099511628211ull;
    }
    return (h ^ gr->graph_id) * 1099511628211ull | 1;
}

void sg_keep_graph_view(studio *s)
{
    s->graph.graph_id = graph_key(s);
}

/* Each graph keeps its own camera; one seen for the first time is framed. */
void sg_follow_graph(studio *s)
{
    studio_graph *g = &s->graph;
    uint64_t id = graph_key(s);
    if (id == g->graph_id) {
        return;
    }
    studio_graph_view *old = view_for(g, g->graph_id);
    if (g->graph_id && !old) {
        apush(g->views, ((studio_graph_view){ g->graph_id }));
        old = &alast(g->views);
    }
    if (old) {
        *old = (studio_graph_view){ g->graph_id, g->zoom, g->x, g->y };
    }
    g->graph_id = id;
    /* What the gesture held belongs to the graph that left, so nothing moves
       back; only its pending snapshot goes. */
    if (g->gesture == STUDIO_GRAPH_MOVE || g->gesture == STUDIO_GRAPH_REROUTE ||
        g->gesture == STUDIO_GRAPH_RESIZE) {
        ed_cancel_snapshot(sg_graph_editor(s));
    }
    g->title_edit = g->bubble_edit = g->comment = 0;
    /* Bind to Source named a pin of the graph that left, and pin ids repeat
       across graphs. */
    g->bind_pin = 0;
    g->bind_kit = NULL;
    g->bind_focus = false;
    g->details.bind[0] = '\0';
    g->gesture = STUDIO_GRAPH_IDLE;
    /* The retained fields still belong to the old graph until the next emit.
       Node IDs can repeat, so release their focus before following the new graph. */
    kit_blur(g->kit);
    g->focused_node = 0;
    g->camera.active = false;
    studio_graph_view *view = view_for(g, id);
    g->framed = view != NULL;
    if (view) {
        g->zoom = view->zoom;
        g->x = view->x;
        g->y = view->y;
    } else {
        g->zoom = 1;
        g->x = -40;
        g->y = -40;
    }
}

/* ---------- Arrange ---------- */

typedef studio_graph_route_input arrange_input;

static int input_index(node *const *nodes, const node *n)
{
    for (int i = 0; i < acount(nodes); i++) {
        if (nodes[i] == n) {
            return i;
        }
    }
    return -1;
}

static void arrange_input_build(const studio *s, arrange_input *in)
{
    graph *gr = sg_active_graph(s);
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        float box[4];
        if (!sg_node_visible(gr, n) || !sg_node_box(s, n, box)) {
            continue;
        }
        apush(in->nodes, n);
        apush(in->boxes, ((arrange_box){ box[2], box[3], graph_layout_band(n) }));
        apush(in->pos, ((arrange_point){ n->x, n->y }));
    }
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *ed = &gr->edges[i];
        apush(in->edge_link, -1);
        node *from = graph_node_by_id(gr, ed->from_id),
             *to = graph_node_by_id(gr, ed->to_id);
        int from_out = 0, to_out = 0, from_pin = -1, to_pin = -1;
        if (!from || !to ||
            !graph_pin_by_id(from, ed->from_pin_id, &from_out, &from_pin) || !from_out ||
            !graph_pin_by_id(to, ed->to_pin_id, &to_out, &to_pin) || to_out) {
            continue;
        }
        int a = input_index(in->nodes, from), b = input_index(in->nodes, to);
        if (a < 0 || b < 0) {
            continue;
        }
        in->edge_link[i] = acount(in->links);
        apush(
            in->links,
            ((arrange_link){ .from = a,
                             .to = b,
                             .from_y = sg_pin_y(s, from, ED_PIN_OUTPUT, from_pin),
                             .to_y = sg_pin_y(s, to, ED_PIN_INPUT, to_pin),
                             .flow = sg_pin_exec(from, ED_PIN_OUTPUT, from_pin) })
        );
    }
}

static void arrange_input_free(arrange_input *in)
{
    afree(in->nodes);
    afree(in->boxes);
    afree(in->pos);
    afree(in->links);
    afree(in->edge_link);
}

static arrange_graph arrange_of(const arrange_input *in)
{
    return (arrange_graph){ in->boxes, acount(in->boxes), in->links, acount(in->links) };
}

/* Retain the expensive boxes, socket offsets and edge-to-link mapping. Node
 * positions may change during a gesture before a committed model revision. */
void sg_update_routes(studio *s)
{
    sg_calculations_sync(s);
    studio_graph *g = &s->graph;
    struct studio_graph_calculations *c = g->calculations;
    arrange_input *in = &c->routing;
    bool changed = c->routes_dirty;
    if (changed) {
        arrange_input_free(in);
        *in = (arrange_input){ 0 };
        arrange_input_build(s, in);
        afree(g->edge_route);
        for (int i = 0; i < acount(in->edge_link); i++) {
            apush(g->edge_route, in->edge_link[i]);
        }
        c->routes_dirty = false;
        c->route_preparations++;
    }
    for (int i = 0; i < acount(in->nodes); i++) {
        arrange_point pos = { in->nodes[i]->x, in->nodes[i]->y };
        changed |= in->pos[i].x != pos.x || in->pos[i].y != pos.y;
        in->pos[i] = pos;
    }
    changed |= g->measured_key != (uint64_t)s->prefs.wires + 1;
    if (changed) {
        arrange_graph layout = arrange_of(in);
        arrange_route(&layout, in->pos, (arrange_route_style)s->prefs.wires, &g->routes);
        g->measured_key = (uint64_t)s->prefs.wires + 1;
    }
}

/* A user's Arrange moves every node as one undo step. Opening a graph lays
   out only its automatic nodes, whose positions documents don't keep, so that
   is neither an edit nor history. */
void sg_arrange(studio *s, bool automatic_only)
{
    editor *e = sg_graph_editor(s);
    arrange_input in = { 0 };
    arrange_input_build(s, &in);
    arrange_graph layout = arrange_of(&in);
    arrange_layout(&layout, (arrange_route_style)s->prefs.wires, in.pos);
    bool moved = false;
    for (int i = 0; i < acount(in.nodes) && !moved; i++) {
        moved = !(automatic_only && in.nodes[i]->placed) &&
                (in.nodes[i]->x != in.pos[i].x || in.nodes[i]->y != in.pos[i].y ||
                 (!automatic_only && !in.nodes[i]->placed));
    }
    if (moved && !automatic_only) {
        ed_snapshot_before(e);
    }
    for (int i = 0; moved && i < acount(in.nodes); i++) {
        if (!(automatic_only && in.nodes[i]->placed)) {
            in.nodes[i]->x = in.pos[i].x;
            in.nodes[i]->y = in.pos[i].y;
            in.nodes[i]->placed = !automatic_only;
        }
    }
    arrange_input_free(&in);
}
