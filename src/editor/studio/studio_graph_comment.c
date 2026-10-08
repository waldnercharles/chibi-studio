/* Comments on the graph canvas: frames around nodes (Unreal's comment boxes,
 * Blender's frames) and the comment bubble a node can carry (Unreal's node
 * comments). Titles and comments edit in place, frames resize from their edges
 * and corners, and every edit is one undo step. */
#include "editor/studio/studio_graph_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum
{
    COMMENT_TEXT = 1024, /* bytes a node comment edits in */
};

static const float FRAME_MIN_W = 96, FRAME_MIN_H = 64;
static const float BUBBLE_MAX = 280;                /* dp */
static const uint32_t DEFAULT_SWATCH = 0x8a8a96ffu; /* graph.rcss .gcomment-tint */

static const struct
{
    const char *label;
    uint32_t color;
} FRAME_COLORS[] = {
    { "Default", 0 },          { "Red", 0xe5534bffu },
    { "Orange", 0xe0823dffu }, { "Yellow", 0xd4b13cffu },
    { "Green", 0x4fae5cffu },  { "Teal", 0x3aa6a0ffu },
    { "Blue", 0x4a8fe0ffu },   { "Purple", 0x9a6fe0ffu },
    { "Pink", 0xd4679fffu },
};

/* Edge and corner grips, keyed h<edges> for the hit test. */
static const struct
{
    unsigned edges;
    const char *cls;
} GRIPS[] = {
    { SG_EDGE_TOP, "grip n" },
    { SG_EDGE_BOTTOM, "grip s" },
    { SG_EDGE_LEFT, "grip w" },
    { SG_EDGE_RIGHT, "grip e" },
    { SG_EDGE_TOP | SG_EDGE_LEFT, "grip nw" },
    { SG_EDGE_TOP | SG_EDGE_RIGHT, "grip ne" },
    { SG_EDGE_BOTTOM | SG_EDGE_LEFT, "grip sw" },
    { SG_EDGE_BOTTOM | SG_EDGE_RIGHT, "grip se" },
};

static void emit_dp(kit *k, const char *property, float value)
{
    char text[32];
    snprintf(text, sizeof text, "%.1fdp", value);
    kit_style(k, property, text);
}

/* A title or, over several lines, a comment edited in place: text holds the
   model's value and, on Enter or when focus leaves, the committed one.
   Editing ends with focus. */
static bool edit_field(studio *s, char *text, size_t size, bool lines, uint64_t *editing)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    bool committed = lines ? kit_text_area(k, "edit", text, size, "Comment", 0)
                           : kit_text_field(k, "edit", text, size, "Comment", 0);
    if (g->comment_focus) {
        kit_focus(k);
        g->comment_focus = false;
    } else if (!kit_item_active(k)) {
        *editing = 0;
    }
    return committed;
}

/* ---------- Frames ---------- */

void sg_edit_title(studio *s, uint64_t comment)
{
    s->graph.title_edit = comment;
    s->graph.bubble_edit = 0;
    s->graph.comment_focus = true;
}

static void emit_title(studio *s, graph_comment_box *box)
{
    studio_graph *g = &s->graph;
    const char *title = box->title && box->title[0] ? box->title : "Comment";
    if (g->title_edit != box->id) {
        kit_label(g->kit, "t", "gcomment-title", title);
        return;
    }
    char text[256];
    snprintf(text, sizeof text, "%s", title);
    kit_push(g->kit, "e", NULL, "gcomment-edit");
    if (edit_field(s, text, sizeof text, false, &g->title_edit) && text[0] &&
        strcmp(text, title)) {
        ed_snapshot_before(sg_graph_editor(s));
        graph_set_comment_box(sg_active_graph(s), box, text, box->color);
    }
    kit_pop(g->kit);
}

/* A frame's tint is keyed by its color, so a new color replaces the layer
   and an uncolored frame keeps the canvas's own. */
static void emit_tint(kit *k, uint32_t color)
{
    char key[16], value[16];
    snprintf(key, sizeof key, "f%08x", color);
    kit_leaf(k, key, NULL, "gcomment-tint");
    if (!color) {
        return;
    }
    snprintf(value, sizeof value, "#%06x1f", color >> 8);
    kit_style(k, "background-color", value);
    snprintf(value, sizeof value, "#%06x80", color >> 8);
    kit_style(k, "border-color", value);
}

/* Frames sit behind nodes; their titles stay readable when zoomed out
   (Unreal). */
void sg_emit_comment(studio *s, graph_comment_box *box)
{
    kit *k = s->graph.kit;
    char key[32];
    snprintf(key, sizeof key, "c%llu", (unsigned long long)box->id);
    kit_push(k, key, NULL, "gcomment");
    kit_flag(k, "data-selected", studio_graph_comment(s) == box->id);
    emit_dp(k, "left", box->x);
    emit_dp(k, "top", box->y);
    emit_dp(k, "width", box->width);
    emit_dp(k, "height", box->height);
    emit_tint(k, box->color);
    emit_title(s, box);
    for (int i = 0; !sg_graph_readonly(s) && i < (int)(sizeof GRIPS / sizeof *GRIPS);
         i++) {
        snprintf(key, sizeof key, "h%u", GRIPS[i].edges);
        kit_leaf(k, key, NULL, GRIPS[i].cls);
    }
    kit_pop(k);
}

void sg_comment_menu(studio *s, sg_menu *m, graph_comment_box *box)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    if (sg_menu_item(m, "rename", "Rename", KIT_ICON_EDIT, NULL, readonly)) {
        sg_edit_title(s, box->id);
        return;
    }
    sg_submenu_begin(m, "color", "Color", KIT_ICON_MATERIAL);
    for (int i = 0; i < (int)(sizeof FRAME_COLORS / sizeof *FRAME_COLORS); i++) {
        char key[16];
        snprintf(key, sizeof key, "c%d", i);
        uint32_t color = FRAME_COLORS[i].color;
        uint32_t swatch = color ? color : DEFAULT_SWATCH;
        if (sg_menu_swatch(m, key, FRAME_COLORS[i].label, swatch, box->color == color, readonly)) {
            if (box->color != color) {
                ed_snapshot_before(e);
                graph_set_comment_box(gr, box, box->title ? box->title : "", color);
            }
            return;
        }
    }
    sg_submenu_end(m);
    sg_menu_separator(m);
    if (sg_menu_item(m, "delete", "Delete Comment", KIT_ICON_TRASH, NULL, readonly)) {
        ed_snapshot_before(e);
        graph_remove_comment_box(gr, box->id);
    }
}

uint32_t sg_frame_color(const graph_comment_box *box)
{
    return box->color ? box->color : DEFAULT_SWATCH;
}

/* ---------- Resizing ---------- */

void sg_begin_resize(studio *s, graph_comment_box *box, unsigned edges)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    g->gesture = STUDIO_GRAPH_RESIZE;
    g->press_comment = box->id;
    g->resize_edges = edges;
    g->comment_x = box->x;
    g->comment_y = box->y;
    g->comment_w = box->width;
    g->comment_h = box->height;
    e->pending_snapshot = ed_snapshot(e);
}

/* The dragged edges follow the pointer; the opposite ones stay put. */
void sg_resize(studio *s)
{
    studio_graph *g = &s->graph;
    graph_comment_box *box = sg_comment_by_id(s, g->press_comment);
    if (!box) {
        return;
    }
    float dx = g->world_x - g->press_wx, dy = g->world_y - g->press_wy;
    float right = g->comment_x + g->comment_w, bottom = g->comment_y + g->comment_h;
    if (g->resize_edges & SG_EDGE_LEFT) {
        box->x = fminf(g->comment_x + dx, right - FRAME_MIN_W);
        box->width = right - box->x;
    }
    if (g->resize_edges & SG_EDGE_RIGHT) {
        box->width = fmaxf(g->comment_w + dx, FRAME_MIN_W);
    }
    if (g->resize_edges & SG_EDGE_TOP) {
        box->y = fminf(g->comment_y + dy, bottom - FRAME_MIN_H);
        box->height = bottom - box->y;
    }
    if (g->resize_edges & SG_EDGE_BOTTOM) {
        box->height = fmaxf(g->comment_h + dy, FRAME_MIN_H);
    }
}

/* Commits a resize that changed the frame; a cancel puts it back. */
void sg_end_resize(studio *s, bool cancel)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph_comment_box *box = sg_comment_by_id(s, g->press_comment);
    bool changed = box && (box->x != g->comment_x || box->y != g->comment_y ||
                           box->width != g->comment_w || box->height != g->comment_h);
    if (box && cancel) {
        box->x = g->comment_x;
        box->y = g->comment_y;
        box->width = g->comment_w;
        box->height = g->comment_h;
    }
    if (changed && !cancel) {
        ed_commit_snapshot(e);
    } else {
        ed_cancel_snapshot(e);
    }
    g->resize_edges = 0;
}

const char *sg_resize_cursor(unsigned edges)
{
    bool x = edges & (SG_EDGE_LEFT | SG_EDGE_RIGHT),
         y = edges & (SG_EDGE_TOP | SG_EDGE_BOTTOM);
    if (x && y) {
        bool falling = edges == (SG_EDGE_TOP | SG_EDGE_LEFT) ||
                       edges == (SG_EDGE_BOTTOM | SG_EDGE_RIGHT);
        return falling ? "nwse-resize" : "nesw-resize";
    }
    return x ? "ew-resize" : "ns-resize";
}

/* ---------- Node comments ---------- */

void sg_edit_bubble(studio *s, uint64_t node_id)
{
    s->graph.bubble_edit = node_id;
    s->graph.title_edit = 0;
    s->graph.comment_focus = true;
}

void sg_set_node_comment(studio *s, node *n, const char *text, bool hidden)
{
    if (!strcmp(text, n->comment ? n->comment : "") && hidden == n->comment_hidden) {
        return;
    }
    ed_snapshot_before(sg_graph_editor(s));
    graph_set_node_comment(sg_active_graph(s), n, text, hidden);
}

/* The widest line of a comment as the bubble draws it. */
static float widest_line(const studio *s, const char *text)
{
    float widest = 0;
    for (const char *at = text; at;) {
        const char *end = strchr(at, '\n');
        char line[COMMENT_TEXT];
        snprintf(line, sizeof line, "%.*s", end ? (int)(end - at) : (int)strlen(at), at);
        widest = fmaxf(widest, sg_text_width(s, line, 12, 400));
        at = end ? end + 1 : NULL;
    }
    return widest;
}

/* A comment bubble above the node, or while hidden only above the hovered
   node (Unreal's unpinned comment). Titles-only zoom hides it. */
void sg_emit_bubble(studio *s, node *n)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    bool editing = g->bubble_edit == n->id;
    if (!editing && (!n->comment || (n->comment_hidden && g->hover_node != n->id))) {
        return;
    }
    /* Sized from the text: an absolute box would shrink to the node's width. */
    float width = editing
                    ? BUBBLE_MAX
                    : fminf(ceilf(widest_line(s, n->comment)) + 18, BUBBLE_MAX);
    kit_push(k, "cm", NULL, "gbubble");
    emit_dp(k, "width", width);
    if (!editing) {
        kit_label(k, "t", "gbubble-text", n->comment);
        kit_pop(k);
        return;
    }
    char text[COMMENT_TEXT];
    snprintf(text, sizeof text, "%s", n->comment ? n->comment : "");
    if (edit_field(s, text, sizeof text, true, &g->bubble_edit)) {
        sg_set_node_comment(s, n, text, n->comment_hidden);
    }
    kit_pop(k);
}

/* The node menu's comment items; true when one ran. */
bool sg_node_comment_items(studio *s, sg_menu *m, node *n)
{
    bool readonly = sg_graph_readonly(s);
    if (sg_menu_item(m, "comment", n->comment ? "Edit Comment" : "Add Comment", KIT_ICON_COMMENT, NULL, readonly)) {
        sg_edit_bubble(s, n->id);
        return true;
    }
    if (n->comment && sg_menu_item(
                          m,
                          "bubble",
                          n->comment_hidden ? "Show Comment" : "Hide Comment",
                          n->comment_hidden ? KIT_ICON_EYE : KIT_ICON_EYE_OFF,
                          NULL,
                          readonly
                      )) {
        sg_set_node_comment(s, n, n->comment, !n->comment_hidden);
        return true;
    }
    return false;
}
