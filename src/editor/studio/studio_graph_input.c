#include "graph/authoring/graph_edit.h"
/* Graph canvas input: replayed window events, gestures and shortcuts. */
#include "editor/studio/studio_graph_internal.h"
#include "platform/cute.h"
#include "support/array.h"
#include "ui/input_feel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum
{
    CURVE_POINTS = 256,
};

static const float WIRE_HIT = 10; /* window px: a 20 px wire target */
static const float SNAP = 16;     /* graph units: half a drawn grid cell */

typedef struct
{
    uint64_t node;
    int kind; /* -1 on the node body */
    int pin;
    uint64_t comment;                /* a comment frame */
    int reroute_edge, reroute_point; /* a wire's reroute point, or -1 */
    bool title;                      /* on the frame's title */
    unsigned edges;                  /* the frame's SG_EDGE_* grip, or 0 */
    bool bubble;                     /* on the node's comment bubble */
} graph_hit;

static const graph_hit NO_HIT = { 0, -1, -1, 0, -1, -1 };

/* A frame drags from anywhere inside it (Blender); its grips resize it. */
static graph_hit parse_comment_hit(unsigned long long id, const char *part)
{
    graph_hit hit = NO_HIT;
    hit.comment = id;
    hit.title = !strcmp(part, "/t");
    if (sscanf(part, "/h%u", &hit.edges) != 1) {
        hit.edges = 0;
    }
    return hit;
}

static graph_hit parse_hit(const char *path)
{
    graph_hit hit = NO_HIT;
    unsigned long long id;
    int used = 0;
    if (path && sscanf(path, "world/stage/c%llu%n", &id, &used) == 1) {
        return parse_comment_hit(id, path + used);
    }
    if (path &&
        sscanf(path, "world/stage/r%d_%d", &hit.reroute_edge, &hit.reroute_point) == 2) {
        return hit;
    }
    hit.reroute_edge = hit.reroute_point = -1;
    if (!path || sscanf(path, "world/stage/n%llu%n", &id, &used) != 1) {
        return hit;
    }
    hit.node = id;
    /* Only a pin's socket and label take a wire; the rest of its row
   belongs to the node, so the node stays easy to grab. */
    const char *row = path + used;
    hit.bubble = !strncmp(row, "/cm", 3);
    int exec, pin, end = 0;
    char side;
    if (sscanf(row, "/b/e%d/%c%d%n", &exec, &side, &pin, &end) != 3 &&
        sscanf(row, "/b/%c%d%n", &side, &pin, &end) != 2 &&
        sscanf(row, "/h/%c%d%n", &side, &pin, &end) != 2) {
        return hit;
    }
    const char *part = row + end;
    bool grip = !strncmp(part, "/s", 2) || !strcmp(part, "/l");
    if (grip && (side == 'i' || side == 'o')) {
        hit.kind = side == 'i' ? ED_PIN_INPUT : ED_PIN_OUTPUT;
        hit.pin = pin;
    }
    return hit;
}

static float drag_distance(const studio *s)
{
    const studio_graph *g = &s->graph;
    float zoom = sg_zoom(s);
    return hypotf(
        g->in.mouse_x * zoom + g->surface.rect.x - g->press_x,
        g->in.mouse_y * zoom + g->surface.rect.y - g->press_y
    );
}

bool sg_link_dragging(const studio *s)
{
    return s->graph.gesture == STUDIO_GRAPH_LINK && drag_distance(s) >= FEEL_DRAG_DP;
}

static float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float t = fmaxf(0, fminf(1, ((px - ax) * dx + (py - ay) * dy) / fmaxf(dx * dx + dy * dy, 1e-6f)));
    return hypotf(px - ax - dx * t, py - ay - dy * t);
}

/* The wire under the pointer, from the curves drawn last frame. */
static int hit_wire(const studio *s)
{
    const studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    float best = WIRE_HIT / sg_zoom(s);
    int found = -1;
    float points[CURVE_POINTS * 2];
    for (int w = 0; w < acount(g->wire_edges); w++) {
        int count = kit_wire_points(g->kit, "back/wires", w, points, CURVE_POINTS);
        count = count < CURVE_POINTS ? count : CURVE_POINTS;
        for (int i = 1; i < count; i++) {
            float d = segment_distance(
                g->in.mouse_x,
                g->in.mouse_y,
                points[i * 2 - 2],
                points[i * 2 - 1],
                points[i * 2],
                points[i * 2 + 1]
            );
            if (d < best) {
                best = d;
                found = g->wire_edges[w];
            }
        }
    }
    return found < acount(gr->edges) ? found : -1;
}

/* The wire's segment nearest the pointer, counting its reroutes in order. */
static int reroute_segment(const studio *s, const edge *ed)
{
    const studio_graph *g = &s->graph;
    node *from, *to;
    int from_pin, to_pin;
    if (!ed->reroute_count ||
        !sg_edge_ends(sg_active_graph(s), ed, &from, &from_pin, &to, &to_pin)) {
        return 0;
    }
    float x[GRAPH_EDGE_MAX_REROUTES + 2], y[GRAPH_EDGE_MAX_REROUTES + 2];
    int last = ed->reroute_count + 1;
    sg_socket_at(s, from, ED_PIN_OUTPUT, from_pin, &x[0], &y[0]);
    for (int r = 0; r < ed->reroute_count; r++) {
        x[r + 1] = ed->reroutes[r].x;
        y[r + 1] = ed->reroutes[r].y;
    }
    sg_socket_at(s, to, ED_PIN_INPUT, to_pin, &x[last], &y[last]);
    int best = 0;
    float nearest = INFINITY;
    for (int i = 0; i < last; i++) {
        float d = segment_distance(g->world_x, g->world_y, x[i], y[i], x[i + 1], y[i + 1]);
        best = d < nearest ? i : best;
        nearest = fminf(nearest, d);
    }
    return best;
}

static void remove_reroute(edge *ed, int point)
{
    ed->reroute_count--;
    memmove(
        &ed->reroutes[point],
        &ed->reroutes[point + 1],
        (size_t)(ed->reroute_count - point) * sizeof *ed->reroutes
    );
}

/* Double-clicking a wire adds a reroute where it was clicked and drags it
   (Unreal). A wire on arranged lanes takes it as its only point. */
static bool add_reroute(studio *s, int index)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    edge *ed = &sg_active_graph(s)->edges[index];
    if (ed->reroute_count >= GRAPH_EDGE_MAX_REROUTES) {
        return false;
    }
    int at = reroute_segment(s, ed);
    e->pending_snapshot = ed_snapshot(e);
    memmove(
        &ed->reroutes[at + 1],
        &ed->reroutes[at],
        (size_t)(ed->reroute_count - at) * sizeof *ed->reroutes
    );
    ed->reroutes[at].x = g->reroute_x = g->press_wx;
    ed->reroutes[at].y = g->reroute_y = g->press_wy;
    ed->reroute_count++;
    g->gesture = STUDIO_GRAPH_REROUTE;
    g->reroute_edge = index;
    g->reroute_point = at;
    g->reroute_added = true;
    return true;
}

/* A reroute point drags; Alt+click removes it (Unreal). */
static void press_reroute(studio *s, graph_hit hit)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (sg_graph_readonly(s) || hit.reroute_edge >= acount(gr->edges) ||
        hit.reroute_point >= gr->edges[hit.reroute_edge].reroute_count) {
        return;
    }
    edge *ed = &gr->edges[hit.reroute_edge];
    if (g->in.modifiers & KIT_MOD_ALT) {
        ed_snapshot_before(e);
        remove_reroute(ed, hit.reroute_point);
        return;
    }
    e->pending_snapshot = ed_snapshot(e);
    g->gesture = STUDIO_GRAPH_REROUTE;
    g->reroute_edge = hit.reroute_edge;
    g->reroute_point = hit.reroute_point;
    g->reroute_x = ed->reroutes[hit.reroute_point].x;
    g->reroute_y = ed->reroutes[hit.reroute_point].y;
    g->reroute_added = false;
}

/* The pins n would join wire `index` by, or false when it can't. */
static bool insert_pins(const studio *s, node *n, int index, int *in, int *out)
{
    graph *gr = sg_active_graph(s);
    node *from, *to;
    int from_pin, to_pin;
    if (!sg_edge_ends(gr, &gr->edges[index], &from, &from_pin, &to, &to_pin)) {
        return false;
    }
    *in = sg_body_target(gr, from, ED_PIN_OUTPUT, from_pin, n);
    *out = sg_body_target(gr, to, ED_PIN_INPUT, to_pin, n);
    return *in >= 0 && *out >= 0;
}

/* A lone unwired node dragged over a wire offers to join it (Blender,
   Node-RED); Alt keeps it out, and so does a frame carrying its contents. */
static int insert_target(const studio *s)
{
    const studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    node *n = acount(g->drag) == 1 ? sg_node_by_id(s, g->drag[0].id) : NULL;
    if (!n || g->press_comment || (g->in.modifiers & KIT_MOD_ALT)) {
        return -1;
    }
    for (int i = 0; i < acount(gr->edges); i++) {
        if (gr->edges[i].from_id == n->id || gr->edges[i].to_id == n->id) {
            return -1;
        }
    }
    int wire = hit_wire(s), in, out;
    return wire >= 0 && insert_pins(s, n, wire, &in, &out) ? wire : -1;
}

static void begin_link(studio *s, node *n, int kind, int pin)
{
    studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    node *src = NULL;
    int src_pin = -1;
    g->link_pickup_node = g->link_pickup_pin = 0;
    /* Dragging a connected data input picks its wire up (Blender, Rete). */
    if (kind == ED_PIN_INPUT && n->input_pins[pin].kind == PIN_DATA &&
        graph_input_source(gr, n, pin, &src, &src_pin) &&
        !graph_node_is_inline(gr, src)) {
        /* Keep the authored wire until release, so Escape and a refused
           connection leave the graph and its undo history unchanged. */
        g->link_pickup_node = n->id;
        g->link_pickup_pin = n->input_pins[pin].id;
        n = src;
        kind = ED_PIN_OUTPUT;
        pin = src_pin;
    }
    g->gesture = STUDIO_GRAPH_LINK;
    g->link_node = n->id;
    g->link_kind = kind;
    g->link_pin = pin;
}

/* Node search opens where the node will land, at the pointer, rather than
   across the window (Fitts; Blender's Add menu, Unreal's context menu). */
static void open_node_search(studio *s, const char *placeholder)
{
    studio_graph *g = &s->graph;
    float zoom = sg_zoom(s), scale = studio_pixel_ratio(s);
    float box[4] = { (g->surface.rect.x + (g->world_x - g->x) * zoom) * scale,
                     (g->surface.rect.y + (g->world_y - g->y) * zoom) * scale,
                     0,
                     0 };
    g->spawn_pending = true;
    g->spawn_x = g->world_x;
    g->spawn_y = g->world_y;
    studio_palette_open_box(s, STUDIO_PALETTE_NODES, placeholder, box);
}

static void end_link(studio *s, graph_hit hit)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    node *from = sg_node_by_id(s, g->link_node);
    node *to = sg_node_by_id(s, hit.node);
    g->gesture = STUDIO_GRAPH_IDLE;
    if (!from) {
        return;
    }
    if (to && hit.kind >= 0 && hit.pin >= 0) {
        if (sg_pins_compatible(from, g->link_kind, g->link_pin, to, hit.kind, hit.pin) ||
            hit.kind != g->link_kind) {
            if (g->link_pickup_node) {
                sg_reconnect_pins(s, to, hit.pin);
            } else {
                sg_connect_pins(s, from, g->link_kind, g->link_pin, to, hit.kind, hit.pin);
            }
        } else if (to != from || hit.pin != g->link_pin) {
            sg_show_error(s, "Can't connect", "A wire joins an output to an input");
        }
        return;
    }
    if (to && to != from) {
        int target = sg_body_target(gr, from, g->link_kind, g->link_pin, to);
        if (target >= 0) {
            if (g->link_pickup_node) {
                sg_reconnect_pins(s, to, target);
            } else {
                sg_connect_pins(
                    s,
                    from,
                    g->link_kind,
                    g->link_pin,
                    to,
                    g->link_kind == ED_PIN_OUTPUT ? ED_PIN_INPUT : ED_PIN_OUTPUT,
                    target
                );
            }
        } else {
            sg_show_error(s, "Can't connect", "This node has no pin that fits the wire");
        }
        return;
    }
    if (!to) {
        if (g->link_pickup_node) {
            sg_reconnect_pins(s, NULL, -1);
            return;
        }
        /* Dropped on empty canvas: search for a node that fits, spawned here
           and wired (Blender, Unreal). */
        e->spawn_link_pending = 1;
        e->spawn_link_pin = (ed_pin_view){ from, g->link_kind, g->link_pin };
        e->pending_snapshot = ed_snapshot(e);
        open_node_search(s, "Add a node for this wire");
    }
}

/* The marquee adds what it touches to the selection it began with; with Ctrl
   it removes it (Blender). */
static void finish_box(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    float x0 = fminf(g->box[0], g->box[2]), x1 = fmaxf(g->box[0], g->box[2]);
    float y0 = fminf(g->box[1], g->box[3]), y1 = fmaxf(g->box[1], g->box[3]);
    ed_clear_selection(e);
    for (int i = 0; i < acount(g->box_base); i++) {
        node *n = graph_node_by_id(gr, g->box_base[i]);
        if (n) {
            ed_add_selected(e, n);
        }
    }
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        float b[4];
        if (!sg_node_visible(gr, n) || !sg_node_box(s, n, b) ||
            !(b[0] < x1 && b[0] + b[2] > x0 && b[1] < y1 && b[1] + b[3] > y0)) {
            continue;
        }
        if (g->box_subtract) {
            ed_remove_selected(e, n);
        } else if (!ed_node_selected(e, n)) {
            ed_add_selected(e, n);
        }
    }
}

/* Keys follow the layout, as the key's letter reads. */
static bool key_held(SDL_Keycode key)
{
    return SDL_GetKeyboardState(NULL)[SDL_GetScancodeFromKey(key, NULL)];
}

/* Holding a key and clicking empty canvas places a node there (Unreal). M
   stays free to mute (Blender). */
static const struct
{
    SDL_Keycode key;
    const char *node;
} SPAWN_KEYS[] = {
    { SDLK_B, "Branch" },  { SDLK_S, "Sequence" }, { SDLK_D, "Delay" },
    { SDLK_F, "ForEach" }, { SDLK_N, "DoN" },      { SDLK_O, "DoOnce" },
    { SDLK_G, "Gate" },
};

static bool spawn_held(studio *s)
{
    studio_graph *g = &s->graph;
    for (int i = 0; i < (int)(sizeof SPAWN_KEYS / sizeof *SPAWN_KEYS); i++) {
        if (!key_held(SPAWN_KEYS[i].key)) {
            continue;
        }
        if (SPAWN_KEYS[i].key == SDLK_F) {
            g->frame_pending = false;
        }
        return sg_spawn_node(s, SPAWN_KEYS[i].node, g->press_wx, g->press_wy);
    }
    return false;
}

/* Scrolling zooms at the pointer, from a wheel or a trackpad alike (Blender,
   Unreal), at the rate every canvas zooms; panning is a middle, right or
   Space drag. */
static void wheel(studio *s, const rml_input *ev)
{
    studio_graph *g = &s->graph;
    float factor = feel_zoom_scale(feel_zoom_notches(-ev->wheel_y));
    sg_zoom_at(s, g->zoom * factor, ev->x, ev->y);
}

/* A frame's grip resizes it and a double-click on its title renames it
   (Unreal); true when the press went to one of them. */
static bool press_comment(studio *s, graph_comment_box *box, graph_hit hit)
{
    if (sg_graph_readonly(s)) {
        return false;
    }
    if (hit.edges) {
        sg_begin_resize(s, box, hit.edges);
        return true;
    }
    if (hit.title && s->graph.double_click) {
        sg_edit_title(s, box->id);
        return true;
    }
    return false;
}

static void press(studio *s, int button, graph_hit hit, bool on_control)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    unsigned mods = g->in.modifiers;
    bool subtract = mods & (KIT_MOD_CTRL | KIT_MOD_META);
    bool additive = subtract || (mods & KIT_MOD_SHIFT);
    if (button == 2 || (button == 0 && g->space)) {
        g->gesture = STUDIO_GRAPH_PAN;
        return;
    }
    if (button == 1) {
        g->gesture = STUDIO_GRAPH_RIGHT;
        return;
    }
    if (on_control) {
        return;
    }
    g->press_comment = 0;
    g->comment = 0;
    if (hit.reroute_edge >= 0) {
        press_reroute(s, hit);
        return;
    }
    graph_comment_box *box = sg_comment_by_id(s, hit.comment);
    if (box && press_comment(s, box, hit)) {
        return;
    }
    if (box) {
        /* A click selects the frame; dragging it carries what it frames, and
           Shift moves the frame alone (Unreal, Blender). */
        ed_clear_selection(e);
        g->comment = box->id;
        g->press_comment = box->id;
        g->comment_x = box->x;
        g->comment_y = box->y;
        g->press_node = 0;
        g->press_toggle = false;
        g->gesture = STUDIO_GRAPH_PRESS;
        return;
    }
    node *n = sg_node_by_id(s, hit.node);
    if (n && hit.bubble && g->double_click && !sg_graph_readonly(s)) {
        sg_edit_bubble(s, n->id);
        return;
    }
    if (n && g->double_click && ed_is_graph_call(n)) {
        /* Double-click enters a call (Blender's Tab); Back returns. */
        editor_open_graph_call(e, n->id);
        return;
    }
    if (n && hit.kind >= 0 && hit.pin >= 0 && (mods & KIT_MOD_ALT)) {
        /* Alt+click breaks every wire at the pin (Unreal). */
        sg_break_links(s, n, hit.kind, hit.pin);
        return;
    }
    if (n && hit.kind >= 0 && hit.pin >= 0 && !sg_graph_readonly(s)) {
        begin_link(s, n, hit.kind, hit.pin);
        return;
    }
    if (n) {
        bool selected = ed_node_selected(e, n);
        g->press_node = n->id;
        g->press_toggle = additive && selected;
        if (!selected) {
            if (!additive) {
                ed_clear_selection(e);
            }
            ed_add_selected(e, n);
        }
        e->front_node = n;
        g->gesture = STUDIO_GRAPH_PRESS;
        return;
    }
    if (g->hover_edge >= 0 && g->hover_edge < acount(gr->edges)) {
        if (g->double_click && !sg_graph_readonly(s) && add_reroute(s, g->hover_edge)) {
            return;
        }
        ed_link_ref link;
        sg_link_ref_of(&gr->edges[g->hover_edge], &link);
        if ((mods & KIT_MOD_ALT) && !sg_graph_readonly(s)) {
            ed_clear_selection(e);
            ed_add_selected_link(e, &link);
            ed_delete_selected(e);
            return;
        }
        if (!additive) {
            ed_clear_selection(e);
        }
        ed_add_selected_link(e, &link);
        return;
    }
    if (!mods && !sg_graph_readonly(s) && spawn_held(s)) {
        return;
    }
    g->gesture = STUDIO_GRAPH_BOX;
    g->box[0] = g->press_wx;
    g->box[1] = g->press_wy;
    g->box[2] = g->world_x;
    g->box[3] = g->world_y;
    g->box_subtract = subtract;
    asetlen(g->box_base, 0);
    if (additive) {
        for (int i = 0; i < acount(e->selected); i++) {
            apush(g->box_base, e->selected[i]->id);
        }
    }
    ed_clear_selection(e);
}

/* Where n's pin nearest `y` sits below its top, or 0 without pins. */
static float nearest_pin(const studio *s, node *n, float y)
{
    graph *gr = sg_active_graph(s);
    float best = 0, distance = INFINITY;
    for (int kind = ED_PIN_INPUT; kind <= ED_PIN_OUTPUT; kind++) {
        int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
        for (int p = 0; p < count; p++) {
            float at = ed_pin_visible(gr, n, kind, p) ? sg_pin_y(s, n, kind, p)
                                                      : INFINITY;
            if (fabsf(at - y) < distance) {
                distance = fabsf(at - y);
                best = at;
            }
        }
    }
    return best;
}

/* Ctrl snaps the lead node's pin nearest the press to the grid, so wires
   between snapped nodes run level, and the rest of the selection keeps its
   offset (imgui-node-editor, xyflow). A frame dragged by its title snaps its
   corner. */
static void snap_move(studio *s, float *dx, float *dy)
{
    studio_graph *g = &s->graph;
    int lead = 0;
    for (int i = 0; i < acount(g->drag); i++) {
        lead = g->drag[i].id == g->press_node ? i : lead;
    }
    node *n = g->press_comment || !acount(g->drag)
                ? NULL
                : sg_node_by_id(s, g->drag[lead].id);
    if (!n && !g->press_comment) {
        return;
    }
    float x = n ? g->drag[lead].x : g->comment_x, y = n ? g->drag[lead].y : g->comment_y;
    float pin = n ? nearest_pin(s, n, g->press_wy - y) : 0;
    *dx = roundf((x + *dx) / SNAP) * SNAP - x;
    *dy = roundf((y + *dy + pin) / SNAP) * SNAP - pin - y;
}

static void drag(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    switch (g->gesture) {
        case STUDIO_GRAPH_PRESS:
            if (drag_distance(s) < FEEL_DRAG_DP || sg_graph_readonly(s)) {
                break;
            }
            g->gesture = STUDIO_GRAPH_MOVE;
            asetlen(g->drag, 0);
            if (g->press_comment) {
                sg_comment_contents(s, g->press_comment, g->in.modifiers & KIT_MOD_SHIFT);
            } else {
                for (int i = 0; i < acount(e->selected); i++) {
                    node *n = e->selected[i];
                    apush(g->drag, ((typeof(*g->drag)){ n->id, n->x, n->y }));
                }
            }
            e->pending_snapshot = ed_snapshot(e);
            /* fallthrough */
        case STUDIO_GRAPH_MOVE: {
            float dx = g->world_x - g->press_wx, dy = g->world_y - g->press_wy;
            if (g->in.modifiers & KIT_MOD_CTRL) {
                snap_move(s, &dx, &dy);
            }
            g->insert_edge = insert_target(s);
            graph_comment_box *box = sg_comment_by_id(s, g->press_comment);
            if (box) {
                box->x = g->comment_x + dx;
                box->y = g->comment_y + dy;
            }
            for (int i = 0; i < acount(g->drag); i++) {
                node *n = sg_node_by_id(s, g->drag[i].id);
                if (n) {
                    n->x = g->drag[i].x + dx;
                    n->y = g->drag[i].y + dy;
                    n->placed = true;
                }
            }
            break;
        }
        case STUDIO_GRAPH_BOX:
            g->box[2] = g->world_x;
            g->box[3] = g->world_y;
            finish_box(s);
            break;
        case STUDIO_GRAPH_RIGHT:
            if (drag_distance(s) >= FEEL_DRAG_DP) {
                g->gesture = STUDIO_GRAPH_PAN;
            }
            break;
        case STUDIO_GRAPH_RESIZE: sg_resize(s); break;
        case STUDIO_GRAPH_REROUTE: {
            /* A drag moves the point by the pointer's travel; a click leaves it. */
            if (drag_distance(s) < FEEL_DRAG_DP) {
                break;
            }
            edge *ed = &sg_active_graph(s)->edges[g->reroute_edge];
            ed->reroutes[g->reroute_point].x = g->reroute_x + g->world_x - g->press_wx;
            ed->reroutes[g->reroute_point].y = g->reroute_y + g->world_y - g->press_wy;
            break;
        }
        default: break;
    }
}

static void release(studio *s, graph_hit hit)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    switch (g->gesture) {
        case STUDIO_GRAPH_PRESS: {
            /* A click on a selected node without a drag narrows the selection
               to it; with a modifier it toggles. */
            node *n = sg_node_by_id(s, g->press_node);
            if (n && g->press_toggle) {
                ed_remove_selected(e, n);
            } else if (n && !(g->in.modifiers & (KIT_MOD_SHIFT | KIT_MOD_CTRL | KIT_MOD_META))) {
                sg_select_only(e, n);
            }
            break;
        }
        case STUDIO_GRAPH_MOVE: {
            graph_comment_box *box = sg_comment_by_id(s, g->press_comment);
            bool moved = box && (box->x != g->comment_x || box->y != g->comment_y);
            for (int i = 0; i < acount(g->drag); i++) {
                node *n = sg_node_by_id(s, g->drag[i].id);
                moved |= n && (n->x != g->drag[i].x || n->y != g->drag[i].y);
            }
            node *lone = acount(g->drag) == 1 ? sg_node_by_id(s, g->drag[0].id) : NULL;
            int in, out;
            bool offered = g->insert_edge >= 0 && lone;
            bool inserted = offered &&
                            insert_pins(s, lone, g->insert_edge, &in, &out) &&
                            ed_insert_on_wire(e, g->insert_edge, lone, in, out);
            if (offered && !inserted) {
                sg_show_error(
                    s,
                    "Can't insert the node",
                    e->connect_error[0] ? e->connect_error : "This node does not fit the wire"
                );
            }
            if (moved || inserted) {
                ed_commit_snapshot(e);
            } else {
                ed_cancel_snapshot(e);
            }
            break;
        }
        case STUDIO_GRAPH_REROUTE: {
            const edge *ed = &sg_active_graph(s)->edges[g->reroute_edge];
            bool moved = g->reroute_added ||
                         ed->reroutes[g->reroute_point].x != g->reroute_x ||
                         ed->reroutes[g->reroute_point].y != g->reroute_y;
            if (moved) {
                ed_commit_snapshot(e);
            } else {
                ed_cancel_snapshot(e);
            }
            break;
        }
        case STUDIO_GRAPH_RESIZE: sg_end_resize(s, false); break;
        case STUDIO_GRAPH_LINK: end_link(s, hit); return;
        case STUDIO_GRAPH_RIGHT: {
            /* A right click that didn't move opens the pin, node or wire menu,
               or on empty canvas the add-node search (Unreal's action menu). */
            graph_hit at = parse_hit(g->press_path[0] ? g->press_path : NULL);
            node *n = sg_node_by_id(s, at.node);
            int wire = n || at.comment ? -1 : hit_wire(s);
            bool pin = n && at.kind >= 0 && at.pin >= 0;
            g->menu_comment = at.comment;
            g->menu_pin = !pin                     ? 0
                        : at.kind == ED_PIN_OUTPUT ? n->output_pins[at.pin].id
                                                   : n->input_pins[at.pin].id;
            if (n || wire >= 0 || at.comment) {
                if (n && !pin && !ed_node_selected(e, n)) {
                    sg_select_only(e, n);
                }
                if (at.comment) {
                    ed_clear_selection(e);
                    g->comment = at.comment;
                }
                g->menu_node = n ? n->id : 0;
                g->menu_edge = wire;
                g->menu_request = true;
                g->menu_x = g->press_x;
                g->menu_y = g->press_y;
            } else if (!sg_graph_readonly(s)) {
                open_node_search(s, "Add a node");
            } else {
                sg_refuse_edit(s);
            }
            break;
        }
        default: break;
    }
    g->gesture = STUDIO_GRAPH_IDLE;
}

/* A cancelled move puts the frame and nodes back and records nothing. */
void sg_cancel_gesture(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    if (g->gesture == STUDIO_GRAPH_MOVE) {
        graph_comment_box *box = sg_comment_by_id(s, g->press_comment);
        if (box) {
            box->x = g->comment_x;
            box->y = g->comment_y;
        }
        for (int i = 0; i < acount(g->drag); i++) {
            node *n = sg_node_by_id(s, g->drag[i].id);
            if (n) {
                n->x = g->drag[i].x;
                n->y = g->drag[i].y;
            }
        }
        ed_cancel_snapshot(e);
    }
    if (g->gesture == STUDIO_GRAPH_RESIZE) {
        sg_end_resize(s, true);
    }
    if (g->gesture == STUDIO_GRAPH_REROUTE) {
        edge *ed = &sg_active_graph(s)->edges[g->reroute_edge];
        if (g->reroute_added) {
            remove_reroute(ed, g->reroute_point);
        } else {
            ed->reroutes[g->reroute_point].x = g->reroute_x;
            ed->reroutes[g->reroute_point].y = g->reroute_y;
        }
        ed_cancel_snapshot(e);
    }
    g->gesture = STUDIO_GRAPH_IDLE;
    g->link_pickup_node = g->link_pickup_pin = 0;
}

/* Replays this frame's window input into the canvas context: pointer input
   while the canvas is hovered or holds a press, keys while one of its fields
   has focus. The canvas's own gestures read the replayed state. */
void sg_replay(studio *s)
{
    studio_graph *g = &s->graph;
    kit_frame_input *in = &g->in;
    for (int b = 0; b < 3; b++) {
        in->pressed[b] = in->released[b] = false;
    }
    in->wheel = 0;
    g->press_button = -1;
    bool blocked = studio_palette_is_open(s) ||
                   (kit_wants_pointer(s->kit) && !g->surface.hovered);
    for (int i = 0; i < acount(g->events); i++) {
        rml_input ev = g->events[i];
        bool pointer = ev.type <= RML_INPUT_POINTER_WHEEL;
        if (pointer) {
            float zoom = sg_zoom(s);
            float cx = (ev.x - g->surface.rect.x) / zoom;
            float cy = (ev.y - g->surface.rect.y) / zoom;
            bool inside = ev.x >= g->surface.rect.x && ev.y >= g->surface.rect.y &&
                          ev.x < g->surface.rect.x + g->surface.rect.w &&
                          ev.y < g->surface.rect.y + g->surface.rect.h;
            bool popup_open = kit_has_popup(g->kit);
            bool popup_wheel = ev.type == RML_INPUT_POINTER_WHEEL &&
                               popup_open && !blocked;
            bool take = g->captured || popup_wheel ||
                        (inside && !blocked && g->surface.hovered);
            if (ev.type == RML_INPUT_POINTER_DOWN && !take) {
                kit_blur(g->kit);
                continue;
            }
            if (!take) {
                if (ev.type == RML_INPUT_POINTER_MOVE && in->mouse_x > -1e8f) {
                    rml_input leave = { .type = RML_INPUT_POINTER_LEAVE };
                    rml_backend_process_input(g->backend, &leave);
                    in->mouse_x = in->mouse_y = -1e9f;
                }
                continue;
            }
            in->mouse_x = cx;
            in->mouse_y = cy;
            ev.x = cx;
            ev.y = cy;
            in->modifiers = ((ev.modifiers & RML_INPUT_MOD_CONTROL) ? KIT_MOD_CTRL : 0u) |
                            ((ev.modifiers & RML_INPUT_MOD_SHIFT) ? KIT_MOD_SHIFT : 0u) |
                            ((ev.modifiers & RML_INPUT_MOD_ALT) ? KIT_MOD_ALT : 0u) |
                            ((ev.modifiers & RML_INPUT_MOD_META) ? KIT_MOD_META : 0u);
            if (ev.type == RML_INPUT_POINTER_WHEEL) {
                if (popup_open && !blocked) {
                    /* A popup scrolls the distance the editor's lists do. */
                    float ratio = studio_graph_pixel_ratio(s),
                          scale = cf_app_get_pixel_scale();
                    ev.wheel_x = feel_scroll_units(ev.wheel_x, ratio, scale);
                    ev.wheel_y = feel_scroll_units(ev.wheel_y, ratio, scale);
                    rml_backend_process_input(g->backend, &ev);
                } else if (!popup_open) {
                    wheel(s, &g->events[i]);
                }
                continue;
            }
            if (ev.type == RML_INPUT_POINTER_DOWN && ev.button >= 0 && ev.button < 3) {
                in->down[ev.button] = in->pressed[ev.button] = true;
                g->captured = true;
                s->focus_area = g->area;
                /* Hit-test where the press happened: a quick click or drag can
                   arrive within one frame. */
                const char *path = kit_hovered_path(g->kit);
                snprintf(g->press_path, sizeof g->press_path, "%s", path ? path : "");
                float wx = g->events[i].x, wy = g->events[i].y;
                g->double_click = ev.button == 0 &&
                                  s->time - g->last_press_time <
                                      FEEL_DOUBLE_CLICK_SECONDS &&
                                  hypotf(wx - g->last_press_x, wy - g->last_press_y) <
                                      FEEL_DOUBLE_CLICK_DP;
                g->last_press_time = g->double_click ? 0 : s->time;
                g->last_press_x = wx;
                g->last_press_y = wy;
                g->press_control = kit_pointer_on_control(g->kit);
                g->press_button = ev.button;
                g->press_x = g->events[i].x;
                g->press_y = g->events[i].y;
                sg_to_world(s, g->press_x, g->press_y, &g->press_wx, &g->press_wy);
            }
            if (ev.type == RML_INPUT_POINTER_UP && ev.button >= 0 && ev.button < 3) {
                in->down[ev.button] = false;
                in->released[ev.button] = true;
                g->captured = in->down[0] || in->down[1] || in->down[2];
            }
        } else if (!kit_wants_keyboard(g->kit)) {
            continue;
        }
        rml_backend_process_input(g->backend, &ev);
    }
    asetlen(g->events, 0);
    /* A pinch zooms at the pointer, as scrolling does. */
    if (s->pinch != 0 && !kit_has_popup(g->kit) && g->surface.hovered && !blocked) {
        sg_zoom_at(s, g->zoom * expf(s->pinch), s->in.mouse_x, s->in.mouse_y);
    }
}

static void snap_link(studio *s, graph_hit *hit)
{
    studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    node *from = sg_node_by_id(s, g->link_node);
    int want = g->link_kind == ED_PIN_OUTPUT ? ED_PIN_INPUT : ED_PIN_OUTPUT;
    float best = 20 / sg_zoom(s);
    for (int i = 0; from && i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        int count = want == ED_PIN_INPUT ? n->input_count : n->output_count;
        if (n == from || !sg_node_visible(gr, n)) {
            continue;
        }
        for (int p = 0; p < count; p++) {
            float x, y;
            if (!ed_pin_visible(gr, n, want, p) ||
                !sg_pins_compatible(from, g->link_kind, g->link_pin, n, want, p)) {
                continue;
            }
            sg_socket_at(s, n, want, p, &x, &y);
            float d = hypotf(x - g->world_x, y - g->world_y);
            if (d < best) {
                best = d;
                *hit = NO_HIT;
                hit->node = n->id;
                hit->kind = want;
                hit->pin = p;
            }
        }
    }
}

/* Dragging near an edge scrolls the view, faster closer in: up to 900 px/s
   within 40 px of the edge (xyflow). */
static void auto_pan(studio *s)
{
    studio_graph *g = &s->graph;
    double dt = fmin(s->time - g->pan_time, 0.05);
    g->pan_time = s->time;
    if (g->gesture != STUDIO_GRAPH_MOVE && g->gesture != STUDIO_GRAPH_BOX &&
        g->gesture != STUDIO_GRAPH_LINK && g->gesture != STUDIO_GRAPH_RESIZE) {
        return;
    }
    const float edge = 40, speed = 900;
    float zoom = sg_zoom(s);
    float px = g->in.mouse_x * zoom, py = g->in.mouse_y * zoom;
    float w = g->surface.rect.w, h = g->surface.rect.h;
    float vx = px < edge     ? -(edge - px) / edge
             : px > w - edge ? (px - (w - edge)) / edge
                             : 0;
    float vy = py < edge     ? -(edge - py) / edge
             : py > h - edge ? (py - (h - edge)) / edge
                             : 0;
    vx = fmaxf(-1, fminf(1, vx));
    vy = fmaxf(-1, fminf(1, vy));
    if (vx == 0 && vy == 0) {
        return;
    }
    g->x += vx * speed * (float)dt / zoom;
    g->y += vy * speed * (float)dt / zoom;
    g->world_x += vx * speed * (float)dt / zoom;
    g->world_y += vy * speed * (float)dt / zoom;
    g->camera.active = false;
}

void sg_pointer(studio *s)
{
    studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    float zoom = sg_zoom(s);
    sg_to_world(
        s,
        g->in.mouse_x * zoom + g->surface.rect.x,
        g->in.mouse_y * zoom + g->surface.rect.y,
        &g->world_x,
        &g->world_y
    );
    bool inside = g->in.mouse_x > -1e8f;
    graph_hit hit = inside ? parse_hit(kit_hovered_path(g->kit)) : NO_HIT;
    bool on_control = inside && kit_pointer_on_control(g->kit);
    node *hovered = sg_node_by_id(s, hit.node);
    uint64_t was_node = g->hover_node;
    int was_kind = g->hover_kind, was_pin = g->hover_pin;
    g->hover_node = hovered ? hovered->id : 0;
    g->hover_kind = hovered && !on_control ? hit.kind : -1;
    g->hover_pin = hovered && !on_control ? hit.pin : -1;
    g->hover_reroute = hit.reroute_edge >= 0
                         ? hit.reroute_edge * GRAPH_EDGE_MAX_REROUTES + hit.reroute_point
                         : -1;
    g->hover_edge = inside && !hovered && hit.reroute_edge < 0 && g->gesture == STUDIO_GRAPH_IDLE
                      ? hit_wire(s)
                      : -1;
    if (g->press_button >= 0 && g->gesture == STUDIO_GRAPH_IDLE) {
        press(s, g->press_button, parse_hit(g->press_path[0] ? g->press_path : NULL), g->press_control);
    }
    if (g->gesture == STUDIO_GRAPH_LINK && hit.kind < 0 && inside) {
        /* A wire snaps to the nearest socket it fits within 20 px (xyflow). */
        snap_link(s, &hit);
        g->hover_node = hit.node;
        g->hover_kind = hit.kind;
        g->hover_pin = hit.pin;
    }
    if (g->hover_node != was_node || g->hover_kind != was_kind ||
        g->hover_pin != was_pin) {
        g->hover_pin_since = g->in.time;
    }
    auto_pan(s);
    if (g->gesture == STUDIO_GRAPH_PAN) {
        /* The graph point grabbed at the press stays under the pointer. */
        g->x = g->press_wx - g->in.mouse_x;
        g->y = g->press_wy - g->in.mouse_y;
        g->world_x = g->press_wx;
        g->world_y = g->press_wy;
        g->camera.active = false;
    } else {
        drag(s);
    }
    bool released = g->in.released[0] || g->in.released[1] || g->in.released[2];
    if (released && !(g->in.down[0] || g->in.down[1] || g->in.down[2])) {
        if (g->gesture == STUDIO_GRAPH_PAN) {
            g->gesture = STUDIO_GRAPH_IDLE;
        } else {
            release(s, hit);
        }
        g->press_comment = 0;
    }
    (void)gr;
}

/* Canvas shortcuts act on the area under the pointer or last clicked (Blender). */
/* The event node under the pointer, whose event F2 renames. */
static node *event_hover(studio *s)
{
    node *n = sg_node_by_id(s, s->graph.hover_node);
    return studio_event_renameable(s, n) && !sg_graph_readonly(s) ? n : NULL;
}

/* The expression input under the pointer, which F2 renames. */
static node *renameable_hover(studio *s)
{
    studio_graph *g = &s->graph;
    node *n = g->hover_pin >= 0 ? sg_node_by_id(s, g->hover_node) : NULL;
    return n && sg_pin_renameable(s, n, g->hover_kind, g->hover_pin) ? n : NULL;
}

static bool feeds(const graph *gr, const node *from, const node *to)
{
    for (int i = 0; i < acount(gr->edges); i++) {
        if (gr->edges[i].from_id == from->id && gr->edges[i].to_id == to->id) {
            return true;
        }
    }
    return false;
}

/* Arrow keys walk the selection along wires (n8n): Right to the nearest
   node it feeds, Left to the nearest feeding it, Up and Down to the nearest
   above or below in its column. Shift keeps what was selected. */
static void walk(studio *s, SDL_Keycode key, bool extend)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    node *from = e->front_node && ed_node_selected(e, e->front_node) ? e->front_node
               : acount(e->selected) ? alast(e->selected)
                                     : NULL;
    float at[4], box[4];
    if (!from || !sg_node_box(s, from, at)) {
        return;
    }
    float fx = at[0] + at[2] / 2, fy = at[1] + at[3] / 2, best = INFINITY;
    node *to = NULL;
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        if (n == from || !sg_node_visible(gr, n) || !sg_node_box(s, n, box)) {
            continue;
        }
        float x = box[0] + box[2] / 2, y = box[1] + box[3] / 2;
        bool column = fabsf(x - fx) < (at[2] + box[2]) / 2;
        bool fits = key == SDLK_RIGHT ? feeds(gr, from, n)
                  : key == SDLK_LEFT  ? feeds(gr, n, from)
                  : key == SDLK_UP    ? column && y < fy
                                      : column && y > fy;
        float distance = hypotf(x - fx, y - fy);
        if (fits && distance < best) {
            best = distance;
            to = n;
        }
    }
    if (!to) {
        return;
    }
    if (!extend) {
        ed_clear_selection(e);
    }
    ed_add_selected(e, to);
    e->front_node = to;
    sg_show_node(s, to, false);
}

/* M mutes the selection (Blender), or enables it when all of it is muted. */
static void mute_selection(studio *s)
{
    editor *e = sg_graph_editor(s);
    bool muted = true;
    for (int i = 0; i < acount(e->selected); i++) {
        muted &= e->selected[i]->disabled != 0;
    }
    for (int i = 0; i < acount(e->selected); i++) {
        if (!sg_set_node_disabled(s, e->selected[i], !muted)) {
            return;
        }
    }
}

void sg_shortcuts(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    bool over = s->hover_area == g->area ||
                (s->hover_area < 0 && s->focus_area == g->area);
    g->space = over && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE];
    /* F also places For Each with a click, so it frames once released. */
    if (g->frame_pending && !key_held(SDLK_F)) {
        g->frame_pending = false;
        sg_frame_again(s);
    }
    if (!over || kit_wants_keyboard(s->kit) || kit_wants_keyboard(g->kit) ||
        studio_palette_is_open(s)) {
        return;
    }
    for (int i = 0; i < acount(s->keys); i++) {
        SDL_Keycode key = s->keys[i].key;
        bool mod = studio_primary(s, s->keys[i].mod);
        bool shift = s->keys[i].mod & SDL_KMOD_SHIFT;
        bool bare = !(
            s->keys[i].mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT | SDL_KMOD_SHIFT)
        );
        if (s->keys[i].repeat) {
            continue; /* a held key acts once */
        }
        /* A gesture holds an undo snapshot and edge indices, so edits wait
           for it to end; Escape cancels it. */
        if (g->gesture != STUDIO_GRAPH_IDLE) {
            if (key == SDLK_ESCAPE) {
                sg_cancel_gesture(s);
            }
            continue;
        }
        /* At rest, Escape lets go of the selection, as in the Scene. */
        if (bare && key == SDLK_ESCAPE) {
            ed_clear_selection(e);
            g->comment = 0;
            continue;
        }
        bool alt = s->keys[i].mod & SDL_KMOD_ALT;
        if (!mod && (key == SDLK_DELETE || key == SDLK_BACKSPACE || key == SDLK_X)) {
            sg_delete_selection(s);
        } else if (shift && alt && key == SDLK_F) {
            if (acount(e->selected) && studio_node_referable(s, e->selected[0])) {
                studio_find_node_references(s, e->selected[0]);
            }
        } else if (shift && alt && key == SDLK_H) {
            if (acount(e->selected) && studio_node_traceable(s, e->selected[0])) {
                studio_calls_node(s, e->selected[0]);
            }
        } else if (alt && !mod && !shift && key == SDLK_G) {
            /* Unreal's Go to Definition. */
            if (acount(e->selected) && studio_node_definable(s, e->selected[0])) {
                studio_go_to_definition(s, e->selected[0]);
            }
        } else if (!mod && !shift && key == SDLK_F) {
            g->frame_pending = true;
        } else if (!mod && key == SDLK_F2 && renameable_hover(s)) {
            sg_rename_pin(s, renameable_hover(s), g->hover_pin);
        } else if (!mod && key == SDLK_F2 && event_hover(s)) {
            studio_refactor_rename_event(s, studio_event_renameable(s, event_hover(s)));
        } else if (bare && key == SDLK_M && sg_graph_readonly(s)) {
            sg_refuse_edit(s);
        } else if (bare && key == SDLK_M) {
            mute_selection(s);
        } else if (!mod && !alt && (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN)) {
            walk(s, key, shift);
        } else if (!mod && !shift && key == SDLK_Q) {
            sg_straighten(s);
        } else if (!mod && key == SDLK_HOME) {
            sg_frame_nodes(s, false, true);
        } else if (mod && key == SDLK_A) {
            graph *gr = sg_active_graph(s);
            ed_clear_selection(e);
            for (int n = 0; n < acount(gr->nodes); n++) {
                if (sg_node_visible(gr, gr->nodes[n])) {
                    ed_add_selected(e, gr->nodes[n]);
                }
            }
        } else if ((mod && key == SDLK_D) || (shift && key == SDLK_D)) {
            if (sg_graph_readonly(s)) {
                sg_refuse_edit(s);
            } else {
                ed_duplicate_selected(e);
            }
        } else if (mod && key == SDLK_C) {
            sg_copy_selection(s);
        } else if (mod && key == SDLK_X) {
            sg_copy_selection(s);
            sg_delete_selection(s);
        } else if (mod && key == SDLK_V) {
            sg_paste(s);
        } else if (mod && key == SDLK_G && !sg_graph_readonly(s) && acount(e->selected)) {
            sg_collapse(s, GRAPH_FUNCTION);
        } else if ((shift && key == SDLK_A) || (bare && key == SDLK_TAB)) {
            if (sg_graph_readonly(s)) {
                sg_refuse_edit(s);
            } else {
                open_node_search(s, "Add a node");
            }
        } else if (!mod && !shift && key == SDLK_C && !sg_graph_readonly(s)) {
            sg_frame_selection(s);
        } else if (!mod && !shift && key == SDLK_L) {
            /* Blender's Select Linked From (L) and To; Shift+L arranges. */
            sg_select_linked(s, !alt, false);
        } else if (!mod && shift && key == SDLK_L && !sg_graph_readonly(s)) {
            sg_arrange(s, false);
        } else if (key == SDLK_F9 && !sg_graph_readonly(s)) {
            /* Breakpoints on the selected nodes that run (Unreal). */
            bool snapshot = false;
            for (int n = 0; n < acount(e->selected); n++) {
                node *target = e->selected[n];
                if (node_behavior_for(target) == NODE_BEHAVIOR_PURE) {
                    continue;
                }
                if (!snapshot) {
                    ed_snapshot_before(e);
                    snapshot = true;
                }
                graph_set_breakpoint(e->active_graph, target, !target->breakpoint);
            }
        }
    }
}
