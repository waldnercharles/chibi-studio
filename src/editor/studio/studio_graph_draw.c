#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
/* Graph canvas rendering: nodes, pins, wires and comments as kit elements. */
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_members_internal.h"
#include "editor/ui/highlight.h"
#include "editor/ui/labels.h"
#include "graph/authoring/describe.h"
#include "graph/model/local.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/conv_nodes.h"
#include "support/array.h"
#include "ui/input_feel.h"
#include "ui/rml.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float GRID = 32;
/* Below this zoom wires drop their labels and pulses (Rivet). */
static const float FLOW_ZOOM_MIN = 0.15f;

/* ---------- Palette ----------
   Colors drawn as geometry follow the theme; graph.rcss themes the elements. */

typedef struct
{
    uint32_t node;      /* the node body: a hollow socket's fill */
    uint32_t edge;      /* around a filled socket */
    uint32_t exec;      /* execution sockets and wires */
    uint32_t exec_idle; /* an unwired execution socket */
    uint32_t hot;       /* a hovered or offered socket's ring */
    uint32_t selected;  /* a selected wire, as graph.rcss's --selected-object */
    uint32_t invalid;   /* a refused connection, and wires of a muted node */
    uint32_t grid, grid_major;
    float shade; /* data type colors darken on light canvases */
} canvas_palette;

static const canvas_palette DARK = {
    0x2b2b2fffu, 0x111113ffu, 0xe8e8eaffu, 0x8a8a90ffu, 0xffffffffu,
    0xe0701cffu, 0xe5534bffu, 0xffffff0au, 0xffffff14u, 1
};
static const canvas_palette LIGHT = { 0xfbfbfcffu, 0xfbfbfcffu, 0x52525bffu,
                                      0xa1a1aaffu, 0x18181bffu, 0xe0701cffu,
                                      0xc93c37ffu, 0x0000000du, 0x0000001au,
                                      0.8f };

static const canvas_palette *palette(const studio *s)
{
    return s->light ? &LIGHT : &DARK;
}

/* Data wires and sockets take their pin type's color. */
static const float PIN_COLORS[][3] = {
    [PIN_FLOW] = { 1.0f, 1.0f, 1.0f },
    [PIN_BOOL] = { 0.95f, 0.43f, 0.48f },
    [PIN_I8] = { 0.34f, 0.76f, 0.65f },
    [PIN_U8] = { 0.34f, 0.76f, 0.65f },
    [PIN_I16] = { 0.013575f, 0.770f, 0.429609f },
    [PIN_U16] = { 0.013575f, 0.770f, 0.429609f },
    [PIN_I32] = { 0.013575f, 0.770f, 0.429609f },
    [PIN_U32] = { 0.013575f, 0.770f, 0.429609f },
    [PIN_I64] = { 0.413575f, 0.770f, 0.429609f },
    [PIN_U64] = { 0.413575f, 0.770f, 0.429609f },
    [PIN_F32] = { 0.65f, 0.85f, 0.39f },
    [PIN_F64] = { 0.42f, 0.68f, 0.22f },
    [PIN_NAME] = { 0.73f, 0.57f, 0.90f },
    [PIN_HANDLE] = { 0.36f, 0.77f, 0.85f },
    [PIN_STR] = { 0.91f, 0.48f, 0.73f },
    [PIN_STRUCT] = { 0.40f, 0.58f, 0.92f },
    [PIN_ENUM] = { 0.34f, 0.76f, 0.65f },
};

/* A pin's color as 0xRRGGBBAA. */
static uint32_t pin_color(const studio *s, const pin_spec *spec, float alpha)
{
    const canvas_palette *p = palette(s);
    uint32_t a = (uint32_t)lroundf(fminf(alpha, 1) * 255);
    if (spec->kind == PIN_EXEC) {
        return (p->exec & ~255u) | a;
    }
    assert(spec->type >= PIN_FLOW && spec->type <= PIN_ENUM);
    const float *c = PIN_COLORS[spec->type];
    return ((uint32_t)lroundf(c[0] * p->shade * 255) << 24) |
           ((uint32_t)lroundf(c[1] * p->shade * 255) << 16) |
           ((uint32_t)lroundf(c[2] * p->shade * 255) << 8) | a;
}

uint32_t sg_pin_color(const studio *s, const pin_spec *spec)
{
    return pin_color(s, spec, 1);
}

/* ---------- Emission ---------- */

/* View eligibility is separate from semantic inline-node visibility. Keep an
   overscan band so panning does not churn elements at the canvas edge. */
static bool in_view(const studio *s, float x, float y, float w, float h)
{
    const studio_graph *g = &s->graph;
    float pad = 128 / sg_zoom(s);
    return x + w >= g->x - pad && y + h >= g->y - pad &&
           x <= g->x + g->in.width + pad && y <= g->y + g->in.height + pad;
}

static bool keep_node(const studio *s, node *n, uint64_t focused)
{
    const studio_graph *g = &s->graph;
    if ((n->comment && !n->comment_hidden) ||
        ed_node_selected(sg_graph_editor(s), n) || n->id == focused ||
        (g->gesture != STUDIO_GRAPH_IDLE && n->id == g->press_node) ||
        (g->gesture == STUDIO_GRAPH_LINK && n->id == g->link_node) ||
        n->id == g->hover_node || n->id == g->reveal || n->id == g->bubble_edit ||
        (g->value_editing && n->id == g->value_edit_node_id) ||
        ((g->inline_popup_pending || g->input_edit_pin || g->inline_edit_pin) &&
         n->id == g->inline_anchor_node)) {
        return true;
    }
    /* Folded expression/source fields belong to their consumer; keep it while
       editing even when the source node itself is semantically inline. */
    for (int p = 0; p < n->input_count; p++) {
        uint64_t id = n->input_pins[p].id;
        if (id == g->input_edit_pin || id == g->inline_edit_pin || id == g->pin_edit) {
            return true;
        }
    }
    float box[4];
    sg_node_box(s, n, box);
    return in_view(s, box[0], box[1], box[2], box[3]);
}

static uint64_t focused_node(const studio *s)
{
    rml_element *el = rml_context_focus_element(rml_backend_context(s->graph.backend));
    if (!el) {
        return 0;
    }
    /* RmlUi Blur moves focus to the input's parent. Passive field wrappers
       should not pin a node forever after its keyboard focus is released. */
    char tab[16];
    rml_element_property(el, "tab-index", tab, sizeof tab);
    if (strcmp(tab, "auto") && !kit_wants_text(s->graph.kit, NULL)) {
        return 0;
    }
    char path[256];
    unsigned long long id = 0;
    if (kit_element_path(s->graph.kit, el, path, sizeof path)) {
        sscanf(path, "world/stage/n%llu", &id);
    }
    return id;
}

static void emit_value(studio *s, node *n, int output, int p)
{
    sg_emit_value_in(s, s->graph.kit, n, output, p);
}

/* Sockets are drawn as geometry: filled when wired, a ring when not, and ringed
   by the hot color while hovered or offered as a target. Execution pins are
   arrows and many-valued pins squares. */
static kit_dot_desc socket_dot(const studio *s, const pin_spec *spec, bool connected, bool hot, float alpha)
{
    const canvas_palette *p = palette(s);
    bool exec = spec->kind == PIN_EXEC;
    uint32_t color = pin_color(s, spec, 1);
    kit_dot_desc d = { exec ? KIT_DOT_ARROW
                       : spec->container != PIN_CONTAINER_SINGLE ? KIT_DOT_SQUARE
                                                                 : KIT_DOT_CIRCLE,
                       connected ? color : p->node,
                       hot         ? p->hot
                       : connected ? p->edge
                       : exec      ? p->exec_idle
                                   : color,
                       hot         ? 2.0f
                       : connected ? (exec ? 0.0f : 1.0f)
                                   : 1.5f };
    uint32_t a = (uint32_t)lroundf(255 * alpha);
    d.fill = (d.fill & ~255u) | (((d.fill & 255u) * a) / 255);
    d.ring = (d.ring & ~255u) | (((d.ring & 255u) * a) / 255);
    return d;
}

/* Watched pins, and the pin under the pointer, show their value in a bubble
   beside the socket unless an editor already shows it (Unreal). While the
   debug target runs, values are live, and with Show Live Values every data
   output shows its value in a quieter chip (Unreal's watches everywhere).
   A hovered pin's bubble waits as a tooltip does, so sweeping across a
   node doesn't flicker, except while values are live. */
static void emit_watch(studio *s, node *n, bool input, int p, const pin_parts *parts, bool hovered)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    editor *e = sg_graph_editor(s);
    bool live = eval_view_live(e->ev);
    bool settled = g->in.time - g->hover_pin_since >= FEEL_TOOLTIP_DELAY;
    bool bubble = (parts->spec->flags & PIN_FLAG_WATCH) ||
                  (hovered && (live || settled));
    bool chip = !input && live && s->prefs.live_values && !sg_far(s);
    bool edited = parts->value != VALUE_NONE && parts->value != VALUE_CHIP;
    /* Previews can't read bindings, so a bound input shows only a run's value. */
    bool bound = input && parts->value == VALUE_CHIP && !parts->inline_source;
    if (parts->spec->kind != PIN_DATA || edited || (bound && !live) ||
        !(bubble || chip)) {
        return;
    }
    studio_live_value value = { 0 };
    if (live && !sg_live_value(s, n, !input, p, &value)) {
        return;
    }
    if (!live) {
        const char *text = input ? eval_view_input(e->ev, sg_active_graph(s), n, p)
                                 : eval_view_output(e->ev, n, p);
        if (!text || !text[0]) {
            return;
        }
        snprintf(value.text, sizeof value.text, "%s", text);
    }
    kit_push(k, "w", NULL, "gwatch");
    kit_flag(k, "data-live", !bubble);
    if (value.color) {
        char color[16];
        snprintf(color, sizeof color, "#%08x", value.swatch);
        kit_leaf(k, "c", NULL, "gwatch-swatch");
        kit_style(k, "background-color", color);
    }
    kit_label(k, "t", NULL, value.text);
    kit_pop(k);
}

static const studio_node_problem *problem_of(const studio_graph *g, uint64_t id)
{
    for (int i = 0; i < acount(g->problems); i++) {
        if (g->problems[i].node == id) {
            return &g->problems[i];
        }
    }
    return NULL;
}

/* An unused node recedes unless a problem draws attention to it or it is being
   worked on: selected, or offered to a dragged wire. */
static bool node_dim(studio *s, node *n)
{
    studio_graph *g = &s->graph;
    return sg_is_unused(g, n->id) && !problem_of(g, n->id) &&
           !ed_node_selected(sg_graph_editor(s), n) && !sg_link_dragging(s);
}

/* One pin: a row, half an execution row, or a socket in the title. Input
   labels share one width per node so the editors line up; compact nodes and
   title pins go unlabeled. */
static void emit_pin(studio *s, node *n, int kind, int p, const char *cls, float label_width)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    node_layout l = *sg_layout_of(s, n);
    bool input = kind == ED_PIN_INPUT;
    pin_parts parts;
    sg_pin_parts_of(s, n, kind, p, &parts);
    char key[16], width[24];
    snprintf(key, sizeof key, input ? "i%d" : "o%d", p);
    bool hovered = g->hover_node == n->id && g->hover_kind == kind &&
                   g->hover_pin == p;
    node *link_from = sg_link_dragging(s) ? sg_node_by_id(s, g->link_node) : NULL;
    bool source = link_from == n && g->link_kind == kind && g->link_pin == p;
    bool fits = link_from &&
                sg_pins_compatible(link_from, g->link_kind, g->link_pin, n, kind, p);
    /* While a wire is dragged, sockets it can't reach recede (imgui-node-editor).
       Sockets are geometry, so a faded node's opacity misses them. */
    float alpha = link_from && !source && !fits ? 0.2f
                : node_dim(s, n) || n->disabled ? 0.45f
                                                : 1;
    kit_push(k, key, NULL, cls);
    kit_flag(k, "data-hover", hovered);
    /* Colors alone don't tell types apart, and outputs go unlabeled: a
       socket and its label tip the pin's type (Unreal). */
    char type[96];
    sm_type_label(parts.spec, type, sizeof type);
    if (!input || !(parts.spec->flags & PIN_FLAG_SETTING)) {
        kit_push(k, "s", "span", "socket");
        kit_tip(k, parts.label, NULL, type);
        kit_dot_desc dot = socket_dot(s, parts.spec, parts.connected || parts.inline_source, hovered, alpha);
        kit_dot(k, "d", NULL, &dot);
        kit_pop(k);
    }
    if (!input && parts.value != VALUE_NONE) {
        emit_value(s, n, 1, p);
    }
    if (input && g->pin_edit && g->pin_edit == parts.spec->id) {
        sg_emit_pin_edit(s, n, p);
    } else if (parts.show_label && l.form == FORM_FULL && !sg_in_head(&l, kind, p)) {
        kit_label(k, "l", "gpin-label", parts.label);
        char *expression = n->desc == &NODE_BehaviorExpression
                             ? studio_code_expression(s, n)
                             : NULL;
        char about[512];
        snprintf(about, sizeof about, "%s%s%s", type, expression ? " · " : "", expression ? expression : "");
        kit_tip(k, parts.label, NULL, about);
        free(expression);
        /* Zoomed out, hinted text can draw a little wider than it measured
           at 100%: a label whose column fits spills into the gap rather
           than truncating. */
        bool column = input && label_width > 0 && !sg_pin_exec(n, kind, p);
        if (column) {
            snprintf(width, sizeof width, "%.0fdp", label_width);
            kit_style(k, "width", width);
        }
        kit_flag(k, "data-fits", column && !l.label_clipped);
    }
    emit_watch(s, n, input, p, &parts, hovered);
    if (input && parts.spec->kind == PIN_DATA &&
        (parts.source_select || parts.value != VALUE_NONE || parts.connected)) {
        sg_emit_graph_input(s, k, n, p);
    }
    kit_pop(k);
}

static void emit_dp(kit *k, const char *property, float value)
{
    char text[32];
    snprintf(text, sizeof text, "%.1fdp", value);
    kit_style(k, property, text);
}

/* How often the debug target ran a node, and when last: "Ran 1204 times,
   last 0.3 s ago.", or "Running, ..." while it keeps running. Empty when it
   never did. */
static void describe_runs(const editor_node_activity *activity, char *buf, size_t size)
{
    buf[0] = '\0';
    if (!activity->runs) {
        return;
    }
    char runs[48] = "once";
    if (activity->runs > 1) {
        snprintf(runs, sizeof runs, "%llu times", (unsigned long long)activity->runs);
    }
    if (activity->pulse >= 1) {
        snprintf(buf, size, "Running, %s so far.", runs);
    } else if (isfinite(activity->since)) {
        snprintf(buf, size, "Ran %s, last %.1f s ago.", runs, activity->since);
    } else {
        snprintf(buf, size, "Ran %s.", runs);
    }
}

/* Hovering a node's title tells what it does, under its full title, and how
   the debug target ran it. The tip is set while the node is hovered, so idle
   nodes cost nothing. */
static void emit_about(studio *s, node *n)
{
    const editor *e = sg_graph_editor(s);
    if (s->graph.hover_node != n->id) {
        return;
    }
    node_subject subject = node_subject_of(n, e->asset_catalog, acount(e->asset_catalog));
    char about[256] = "", runs[96] = "", tip[360], title[256];
    node_subject_describe(&subject, about, sizeof about);
    editor_node_activity activity;
    editor_query_node_activity(e, n->id, &activity);
    describe_runs(&activity, runs, sizeof runs);
    snprintf(tip, sizeof tip, "%s%s%s", about, about[0] && runs[0] ? " " : "", runs);
    if (tip[0]) {
        kit_tip(s->graph.kit, sg_node_title(s, n, FORM_FULL, title, sizeof title), NULL, tip);
    }
}

/* Value controls may atomically replace the asset. Draw traversal owns IDs,
 * and rebinds its borrowed nodes after each control that can publish an edit. */
static node *current_node(studio *s, uint64_t id)
{
    node *n = graph_node_by_id(sg_active_graph(s), id);
    assert(n);
    return n;
}

static void emit_head(studio *s, node *n, const node_layout *l)
{
    uint64_t id = n->id;
    kit *k = s->graph.kit;
    char title[256];
    kit_push(k, "h", NULL, "gnode-head");
    if (l->head_in >= 0) {
        emit_pin(s, n, ED_PIN_INPUT, l->head_in, "hpin in", 0);
        n = current_node(s, id);
    }
    const char *symbol = ed_node_symbol(n->desc);
    uint32_t icon = sg_node_icon(s, n);
    if (symbol) {
        kit_label(k, "sym", "symbol", symbol);
    } else if (icon) {
        kit_icon(k, "i", NULL, icon);
    }
    kit_label(k, "t", "title", sg_node_title(s, n, l->form, title, sizeof title));
    emit_about(s, n);
    if (l->head_out >= 0) {
        emit_pin(s, n, ED_PIN_OUTPUT, l->head_out, "hpin out", 0);
    }
    kit_pop(k);
}

/* An expression's source, highlighted, on the lines measure_node wrapped. */
static void emit_code(studio *s, node *n, const node_layout *l)
{
    kit *k = s->graph.kit;
    if (sg_expression_editing(s, k, n, 0)) {
        sg_emit_expression_editor(s, k, n, 0);
        return;
    }
    char *code = studio_code_expression(s, n);
    int starts[CODE_LINES], length = (int)strlen(code);
    int count = sg_code_lines(s, code, sg_code_room(l), starts);
    kit_push(k, "code", NULL, "gcode");
    bool clipped = false; /* a line ran past its room as last laid out */
    for (int i = 0; i < count; i++) {
        int end = i + 1 < count ? starts[i + 1] : length;
        while (end > starts[i] && code[end - 1] == ' ') {
            end--;
        }
        char key[8], line[512], markup[2048];
        snprintf(key, sizeof key, "l%d", i);
        snprintf(line, sizeof line, "%.*s", end - starts[i], code + starts[i]);
        highlight_markup(HIGHLIGHT_EXPRESSION, line, markup, sizeof markup);
        kit_markup(k, key, "gcode-line", markup);
        rml_element *el = kit_item_element(k);
        clipped |= rml_element_scroll_width(el) > rml_element_client_width(el) + 0.5f;
    }
    kit_button_desc edit = { .icon = KIT_ICON_EDIT,
                             .variant = KIT_GHOST,
                             .size = KIT_SM,
                             .cls = "gcode-edit",
                             .tip = "Edit expression",
                             .flags = sg_graph_readonly(s) ? KIT_DISABLED : 0 };
    if (kit_button(k, "edit", &edit)) {
        sg_begin_expression_edit(s, k, n, 0);
    }
    kit_pop(k);
    /* Zoomed out past OPEN_ZOOM_MIN a line can clip: its tip shows it all. */
    kit_tip(k, clipped ? "Expression" : NULL, NULL, clipped ? code : NULL);
    free(code);
    if (!sg_graph_readonly(s) && kit_item_clicked(k)) {
        sg_begin_expression_edit(s, k, n, 0);
    }
}

static void emit_rows(studio *s, node *n, const node_layout *l)
{
    uint64_t id = n->id;
    kit *k = s->graph.kit;
    graph *gr = sg_active_graph(s);
    char key[16];
    kit_push(k, "b", NULL, "gnode-body");
    if (l->code_lines) {
        emit_code(s, n, l);
        n = current_node(s, id);
        gr = sg_active_graph(s);
    }
    for (int row = 0; row < sg_exec_rows(gr, n, l); row++) {
        snprintf(key, sizeof key, "e%d", row);
        kit_push(k, key, NULL, "grow exec");
        for (int kind = ED_PIN_INPUT; kind <= ED_PIN_OUTPUT; kind++) {
            int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
            for (int p = 0; p < count; p++) {
                if (sg_pin_in_rows(gr, n, l, kind, p) && sg_pin_exec(n, kind, p) &&
                    sg_pin_order(gr, n, l, kind, p) == row) {
                    emit_pin(s, n, kind, p, kind == ED_PIN_INPUT ? "ghalf in" : "ghalf out", 0);
                    n = current_node(s, id);
                    gr = sg_active_graph(s);
                }
            }
        }
        kit_pop(k);
    }
    for (int p = 0; p < n->output_count; p++) {
        if (sg_pin_in_rows(gr, n, l, ED_PIN_OUTPUT, p) &&
            !sg_pin_exec(n, ED_PIN_OUTPUT, p)) {
            emit_pin(s, n, ED_PIN_OUTPUT, p, "grow out", 0);
            n = current_node(s, id);
            gr = sg_active_graph(s);
        }
    }
    for (int p = 0; p < n->input_count; p++) {
        if (sg_pin_in_rows(gr, n, l, ED_PIN_INPUT, p) &&
            !sg_pin_exec(n, ED_PIN_INPUT, p)) {
            emit_pin(s, n, ED_PIN_INPUT, p, "grow in", l->label);
            n = current_node(s, id);
            gr = sg_active_graph(s);
        }
    }
    if (l->add_pin >= 0) {
        kit_push(k, "add", NULL, l->add_pin == ED_PIN_OUTPUT ? "grow gadd out" : "grow gadd in");
        kit_button_desc add = { .label = "Add Pin",
                                .icon = KIT_ICON_ADD,
                                .size = KIT_SM,
                                .flags = sg_graph_readonly(s) ? KIT_DISABLED : 0 };
        if (kit_button(k, "b", &add)) {
            sg_add_run_pin(s, n);
        }
        kit_pop(k);
    }
    kit_pop(k);
}

/* Inputs stack on the left, the output centers on the right, and the symbol
   takes the space between, so none of them overlap. */
static void emit_operator(studio *s, node *n, const node_layout *l)
{
    uint64_t id = n->id;
    kit *k = s->graph.kit;
    graph *gr = sg_active_graph(s);
    float body = l->h - 2 * EDGE;
    kit_push(k, "b", NULL, "gnode-body");
    for (int p = 0; p < n->input_count; p++) {
        if (ed_pin_visible(gr, n, ED_PIN_INPUT, p)) {
            emit_pin(s, n, ED_PIN_INPUT, p, "grow in", 0);
            n = current_node(s, id);
            gr = sg_active_graph(s);
            emit_dp(k, "width", l->column);
        }
    }
    kit_label(k, "sym", "gsymbol", ed_node_symbol(n->desc));
    emit_about(s, n);
    emit_dp(k, "left", l->column + PART_GAP);
    emit_dp(k, "right", SOCKET_PAD);
    emit_dp(k, "line-height", body);
    for (int p = 0; p < n->output_count; p++) {
        if (ed_pin_visible(gr, n, ED_PIN_OUTPUT, p)) {
            emit_pin(s, n, ED_PIN_OUTPUT, p, "gout out", 0);
            emit_dp(k, "top", (body - ROW) / 2);
        }
    }
    kit_pop(k);
}

/* ---------- Problems and the paused node ---------- */

/* One problem per node, the worst: an error replaces warnings, and messages
   of equal weight join. A folded source's problem shows on its consumer. */
static void add_problem(studio_graph *g, graph *gr, uint64_t id, bool error, const char *text)
{
    node *n = id ? graph_node_by_id(gr, id) : NULL;
    if (!n) {
        return;
    }
    uint64_t owner = graph_inline_owner(gr, n)->id;
    studio_node_problem *p = NULL;
    for (int i = 0; i < acount(g->problems); i++) {
        p = g->problems[i].node == owner ? &g->problems[i] : p;
    }
    if (!p) {
        apush(g->problems, ((studio_node_problem){ .node = owner }));
        p = &alast(g->problems);
    } else if (p->error == error) {
        size_t used = strlen(p->text);
        if (!strstr(p->text, text)) {
            snprintf(p->text + used, sizeof p->text - used, " \xc2\xb7 %s", text);
        }
        return;
    } else if (p->error) {
        return;
    }
    p->error = error;
    snprintf(p->text, sizeof p->text, "%s", text);
}

/* The open behavior's compile diagnostics and, while the scene runs, the runs
   that failed in this graph. */
static void collect_problems(studio *s)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    asetlen(g->problems, 0);
    for (int i = 0; i < acount(e->compile.items); i++) {
        const compile_diagnostic *d = &e->compile.items[i];
        if ((d->severity == CMPL_ERROR || d->severity == CMPL_WARNING) &&
            behavior_asset_find_graph(e->asset, d->graph_id) == gr) {
            add_problem(g, gr, d->node_id, d->severity == CMPL_ERROR, d->message);
        }
    }
    if (!runtime_session(s->scene)) {
        return;
    }
    for (int i = 0; i < acount(s->runtime_errors); i++) {
        const studio_runtime_error *error = &s->runtime_errors[i];
        if (!strcmp(error->at.asset, s->scene->graph_document_id) &&
            behavior_asset_find_graph(e->asset, error->at.graph_id) == gr) {
            add_problem(g, gr, error->at.node_id, true, error->message);
        }
    }
}

static bool paused_at(const studio *s, const node *n)
{
    const studio_node_ref *at = &s->pause.at;
    return s->pause.active && at->node_id == n->id &&
           !strcmp(at->asset, s->scene->graph_document_id) &&
           behavior_asset_find_graph(sg_graph_editor(s)->asset, at->graph_id) ==
               sg_active_graph(s);
}

/* A problem badge on the node's corner, its message on hover (Blender's node
   warnings), a ring around the node a paused run stopped at, and a rim on a
   node the debug target is running, which fades once it stops (its
   presence's exit transition, immediate with Reduce Motion). */
static void emit_marks(studio *s, const studio_node_problem *problem, bool paused, bool running)
{
    kit *k = s->graph.kit;
    if (running) {
        kit_leaf(k, "running", NULL, "grunning");
        kit_presence(k);
    }
    if (problem) {
        kit_push(k, "problem", NULL, "gproblem");
        kit_tip(k, problem->error ? "Error" : "Warning", NULL, problem->text);
        kit_icon(k, "i", NULL, problem->error ? KIT_ICON_ERROR : KIT_ICON_WARNING);
        kit_pop(k);
    }
    if (paused) {
        kit_leaf(k, "paused", NULL, "gpaused");
        kit_presence(k);
    }
}

static void emit_node(studio *s, node *n)
{
    static const char *const FORMS[] = { "full", "operator", "getter" };
    uint64_t id = n->id;
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    editor *e = sg_graph_editor(s);
    node_layout l = *sg_layout_of(s, n);
    const studio_node_problem *problem = problem_of(g, n->id);
    bool paused = paused_at(s, n);
    char key[32];
    snprintf(key, sizeof key, "n%llu", (unsigned long long)n->id);
    kit_push(k, key, NULL, "gnode");
    emit_dp(k, "left", n->x);
    emit_dp(k, "top", n->y);
    emit_dp(k, "width", l.w);
    kit_attr(k, "data-kind", sg_node_kind(s, n));
    kit_attr(k, "data-form", FORMS[l.form]);
    kit_flag(k, "data-selected", ed_node_selected(e, n));
    kit_flag(k, "data-active", e->front_node == n && ed_node_selected(e, n) && acount(e->selected) > 1);
    kit_flag(k, "data-disabled", n->disabled != 0);
    kit_flag(k, "data-dim", node_dim(s, n));
    kit_flag(k, "data-breakpoint", n->breakpoint != 0);
    kit_flag(k, "data-error", problem && problem->error);
    kit_flag(k, "data-warning", problem && !problem->error);
    kit_flag(k, "data-paused", paused);
    sg_emit_bubble(s, n);
    if (l.form == FORM_OPERATOR) {
        emit_operator(s, n, &l);
    } else {
        emit_head(s, n, &l);
    }
    n = current_node(s, id);
    if (l.form == FORM_FULL &&
        (l.code_lines || sg_row_count(sg_active_graph(s), n, &l))) {
        emit_rows(s, n, &l);
    }
    n = current_node(s, id);
    editor_node_activity activity;
    editor_query_node_activity(e, n->id, &activity);
    emit_marks(s, problem, paused, activity.pulse >= 1);
    kit_pop(k);
}

/* While the debug target runs, a wire that carried execution or a value
   pulses: bubbles flow at full size while it keeps firing and shrink to
   nothing once it stops (Unreal). Execution wires also glow, fading for two
   seconds; Reduce Motion trades the bubbles for that glow. Far out, where
   wires lose their labels, bubbles go too (Rivet). */
static void emit_activity(studio *s, const edge *ed, const pin_spec *spec, kit_wire *wire)
{
    editor_edge_activity activity;
    editor_query_edge_activity(sg_graph_editor(s), ed, &activity);
    bool still = s->prefs.reduced_motion || sg_zoom(s) < FLOW_ZOOM_MIN;
    float glow = spec->kind == PIN_EXEC ? activity.heat : 0;
    glow = still ? fmaxf(glow, activity.pulse) : glow;
    if (glow <= 0 && (still || activity.pulse <= 0)) {
        return;
    }
    wire->flow_color = pin_color(s, spec, 1);
    if (glow > 0) {
        /* Eighths of a dp, so a fading glow rebuilds a few times, not every frame. */
        wire->flow_width = wire->width + roundf(12 * glow) / 8;
    }
    if (!still && activity.pulse > 0) {
        wire->flags |= KIT_WIRE_FLOW;
        wire->flow = (float)s->time;
        wire->pulse = activity.pulse;
    }
}

/* Wires follow the selection: attached wires brighten, others recede. */
static void emit_wires(studio *s)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    kit_wire *wires = NULL;
    float *via = NULL;
    int *via_first = NULL;
    asetlen(g->wire_edges, 0);
    sg_update_routes(s);
    bool focus = acount(e->selected) > 0 || g->hover_node || g->hover_edge >= 0;
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *ed = &gr->edges[i];
        if (g->gesture == STUDIO_GRAPH_LINK && ed->to_id == g->link_pickup_node &&
            ed->to_pin_id == g->link_pickup_pin) {
            continue;
        }
        node *from = graph_node_by_id(gr, ed->from_id),
             *to = graph_node_by_id(gr, ed->to_id);
        int fo = 0, to_out = 0, fp = -1, tp = -1;
        /* A use of a named value reads the name instead of a wire. */
        if (!sg_node_visible(gr, from) || !sg_node_visible(gr, to) ||
            !graph_pin_by_id(from, ed->from_pin_id, &fo, &fp) || !fo ||
            !graph_pin_by_id(to, ed->to_pin_id, &to_out, &tp) || to_out ||
            graph_edge_is_local(gr, ed)) {
            continue;
        }
        apush(g->wire_edges, i);
        const pin_spec *spec = &from->output_pins[fp];
        bool exec = spec->kind == PIN_EXEC;
        ed_link_ref ref;
        sg_link_ref_of(ed, &ref);
        bool attached = ed_node_selected(e, from) || ed_node_selected(e, to) ||
                        g->hover_node == from->id || g->hover_node == to->id ||
                        g->hover_edge == i || ed_link_selected(e, &ref);
        float alpha = !focus || attached ? 1 : 0.35f;
        if (sg_is_unused(g, from->id) || sg_is_unused(g, to->id)) {
            alpha *= 0.5f;
        }
        bool muted = from->disabled || to->disabled;
        kit_wire wire = { .color = muted ? (palette(s)->invalid & ~255u) |
                                               (uint32_t)lroundf(alpha * 255)
                                         : pin_color(s, spec, alpha),
                          .width = (exec ? 2.5f : 1.5f) + (attached && focus ? 0.5f : 0),
                          .flags = ed->conversion ? KIT_WIRE_DASHED : 0 };
        /* Model anchors remain available when endpoint nodes are culled. */
        sg_socket_at(s, from, ED_PIN_OUTPUT, fp, &wire.from_x, &wire.from_y);
        sg_socket_at(s, to, ED_PIN_INPUT, tp, &wire.to_x, &wire.to_y);
        emit_activity(s, ed, spec, &wire);
        if (ed_link_selected(e, &ref)) {
            wire.color = palette(s)->selected;
            wire.flow_color = wire.flow_color ? wire.color : 0;
        }
        if (g->gesture == STUDIO_GRAPH_MOVE && g->insert_edge == i) {
            wire.color = palette(s)->hot;
            wire.width += 1;
            wire.flow_color = wire.flow_color ? wire.color : 0;
            if (wire.flow_width > 0) {
                wire.flow_width += 1;
            }
        }
        /* Authored reroutes win; otherwise the lanes arrange routes through. */
        int first = acount(via) / 2, count = 0;
        if (ed->reroute_count) {
            for (int r = 0; r < ed->reroute_count; r++) {
                apush(via, ed->reroutes[r].x);
                apush(via, ed->reroutes[r].y);
            }
            count = ed->reroute_count;
        } else if (g->edge_route && i < acount(g->edge_route) && g->edge_route[i] >= 0) {
            int link = g->edge_route[i];
            for (int p = g->routes.start[link]; p < g->routes.start[link + 1]; p++) {
                apush(via, g->routes.points[p].x);
                apush(via, g->routes.points[p].y);
                count++;
            }
        }
        wire.via_count = count;
        apush(via_first, first);
        apush(wires, wire);
    }
    /* Pointers into the grown arrays are taken once they stop growing. */
    for (int w = 0; w < acount(wires); w++) {
        wires[w].via = wires[w].via_count ? via + via_first[w] * 2 : NULL;
    }
    kit_wires_desc desc = { .wires = wires,
                            .count = acount(wires),
                            .style = s->prefs.wires,
                            .camera_x = g->x,
                            .camera_y = g->y,
                            .grid = GRID,
                            .grid_x = g->x,
                            .grid_y = g->y,
                            .grid_color = palette(s)->grid,
                            .grid_major_color = palette(s)->grid_major,
                            .flow_scale = studio_pixel_ratio(s) };
    kit_wires(k, "wires", NULL, &desc);
    afree(wires);
    afree(via);
    afree(via_first);
}

/* A snapped preview that will need a conversion dashes, as the wire it
   makes will. */
static unsigned converts(const pin_spec *out, const pin_spec *in)
{
    return out->kind == PIN_DATA && conversion_for_pins(out, in) ? KIT_WIRE_DASHED : 0;
}

static void emit_front(studio *s)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    char value[32];
    if (g->gesture == STUDIO_GRAPH_BOX) {
        float x0 = fminf(g->box[0], g->box[2]) - g->x,
              y0 = fminf(g->box[1], g->box[3]) - g->y;
        kit_leaf(k, "marquee", NULL, "marquee");
        snprintf(value, sizeof value, "%.1fdp", x0);
        kit_style(k, "left", value);
        snprintf(value, sizeof value, "%.1fdp", y0);
        kit_style(k, "top", value);
        snprintf(value, sizeof value, "%.1fdp", fabsf(g->box[2] - g->box[0]));
        kit_style(k, "width", value);
        snprintf(value, sizeof value, "%.1fdp", fabsf(g->box[3] - g->box[1]));
        kit_style(k, "height", value);
    }
    node *from = g->gesture == STUDIO_GRAPH_LINK ? sg_node_by_id(s, g->link_node)
                                                 : NULL;
    if (!from) {
        return;
    }
    float anchor_x, anchor_y;
    sg_socket_at(s, from, g->link_kind, g->link_pin, &anchor_x, &anchor_y);
    const pin_spec *spec = g->link_kind == ED_PIN_OUTPUT
                             ? &from->output_pins[g->link_pin]
                             : &from->input_pins[g->link_pin];
    node *over = sg_node_by_id(s, g->hover_node);
    bool snap = over && g->hover_kind >= 0 &&
                sg_pins_compatible(from, g->link_kind, g->link_pin, over, g->hover_kind, g->hover_pin);
    bool invalid = over && g->hover_kind >= 0 && !snap;
    kit_wire wire = { .color = invalid ? palette(s)->invalid : pin_color(s, spec, 1),
                      .width = spec->kind == PIN_EXEC ? 2.5f : 2,
                      .to_x = g->world_x,
                      .to_y = g->world_y,
                      .from_x = g->world_x,
                      .from_y = g->world_y };
    /* Wires always run output to input, whichever end is being dragged. */
    if (g->link_kind == ED_PIN_OUTPUT) {
        wire.from_x = anchor_x;
        wire.from_y = anchor_y;
        if (snap) {
            sg_socket_at(s, over, g->hover_kind, g->hover_pin, &wire.to_x, &wire.to_y);
            wire.flags = converts(spec, &over->input_pins[g->hover_pin]);
        }
    } else {
        wire.to_x = anchor_x;
        wire.to_y = anchor_y;
        if (snap) {
            sg_socket_at(s, over, g->hover_kind, g->hover_pin, &wire.from_x, &wire.from_y);
            wire.flags = converts(&over->output_pins[g->hover_pin], spec);
        }
    }
    kit_wires(
        k,
        "link",
        NULL,
        &(kit_wires_desc){ .wires = &wire,
                           .count = 1,
                           .style = s->prefs.wires,
                           .camera_x = g->x,
                           .camera_y = g->y }
    );
}

/* A wire that converts shows what it converts at its middle, so implicit
   work stays visible without a node (Unreal's conversion nodes, folded). */
static void emit_conversions(studio *s)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    graph *gr = sg_active_graph(s);
    if (sg_far(s)) {
        return;
    }
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *ed = &gr->edges[i];
        node *from = graph_node_by_id(gr, ed->from_id),
             *to = graph_node_by_id(gr, ed->to_id);
        int fo = 0, to_out = 0, fp = -1, tp = -1;
        /* A named value's use draws no wire to label. */
        if (!ed->conversion || !sg_node_visible(gr, from) || !sg_node_visible(gr, to) ||
            !graph_pin_by_id(from, ed->from_pin_id, &fo, &fp) || !fo ||
            !graph_pin_by_id(to, ed->to_pin_id, &to_out, &tp) || to_out ||
            graph_edge_is_local(gr, ed)) {
            continue;
        }
        float ax, ay, bx, by;
        sg_socket_at(s, from, ED_PIN_OUTPUT, fp, &ax, &ay);
        sg_socket_at(s, to, ED_PIN_INPUT, tp, &bx, &by);
        /* On the wire as drawn: its authored reroutes, else its lane route. */
        float mx = (ax + bx) / 2, my = (ay + by) / 2;
        int link = g->edge_route && i < acount(g->edge_route) ? g->edge_route[i] : -1;
        if (ed->reroute_count) {
            mx = ed->reroutes[ed->reroute_count / 2].x;
            my = ed->reroutes[ed->reroute_count / 2].y;
        } else if (link >= 0 && g->routes.start[link + 1] > g->routes.start[link]) {
            int middle = (g->routes.start[link] + g->routes.start[link + 1]) / 2;
            mx = g->routes.points[middle].x;
            my = g->routes.points[middle].y;
        }
        char key[32], text[96];
        const node_desc *c = ed->conversion;
        if (c->conversion_from && c->conversion_to) {
            snprintf(text, sizeof text, "%s \xe2\x86\x92 %s", c->conversion_from, c->conversion_to);
        } else {
            editor_node_label(c, text, sizeof text);
        }
        float width = ceilf(sg_text_width(s, text, 10, 400)) + 14;
        if (!in_view(s, mx - width / 2, my, width, ROW)) {
            continue;
        }
        snprintf(key, sizeof key, "x%d", i);
        kit_label(k, key, "gconversion", text);
        emit_dp(k, "left", mx);
        emit_dp(k, "top", my);
        emit_dp(k, "width", width);
        emit_dp(k, "margin-left", -width / 2);
    }
}

/* A wire's reroute points, drawn as dots in its color (Blender). */
static void emit_reroutes(studio *s)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    graph *gr = sg_active_graph(s);
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *ed = &gr->edges[i];
        node *from, *to;
        int from_pin, to_pin;
        if (!ed->reroute_count ||
            !sg_edge_ends(gr, ed, &from, &from_pin, &to, &to_pin) ||
            !sg_node_visible(gr, from) || !sg_node_visible(gr, to)) {
            continue;
        }
        uint32_t color = pin_color(s, &from->output_pins[from_pin], 1);
        for (int r = 0; r < ed->reroute_count; r++) {
            char key[32];
            snprintf(key, sizeof key, "r%d_%d", i, r);
            bool hot = g->hover_reroute == i * GRAPH_EDGE_MAX_REROUTES + r ||
                       (g->gesture == STUDIO_GRAPH_REROUTE &&
                        g->reroute_edge == i && g->reroute_point == r);
            if (!hot && !in_view(s, ed->reroutes[r].x, ed->reroutes[r].y, 0, 0)) {
                continue;
            }
            kit_push(k, key, NULL, "greroute");
            emit_dp(k, "left", ed->reroutes[r].x);
            emit_dp(k, "top", ed->reroutes[r].y);
            kit_dot_desc dot = { KIT_DOT_CIRCLE,
                                 color,
                                 hot ? palette(s)->hot : palette(s)->edge,
                                 hot ? 2.0f : 1.0f };
            kit_dot(k, "d", NULL, &dot);
            kit_pop(k);
        }
    }
}

void sg_emit_canvas(studio *s)
{
    studio_graph *g = &s->graph;
    kit *k = g->kit;
    graph *gr = sg_active_graph(s);
    collect_problems(s);
    kit_root(k, "back");
    emit_wires(s);
    kit_root(k, "world");
    kit_push(k, "stage", NULL, "stage");
    char value[32];
    snprintf(value, sizeof value, "%.2fdp", -g->x);
    kit_style(k, "left", value);
    snprintf(value, sizeof value, "%.2fdp", -g->y);
    kit_style(k, "top", value);
    for (int i = 0; i < acount(gr->comment_boxes); i++) {
        const graph_comment_box *box = &gr->comment_boxes[i];
        if (box->id == g->press_comment || box->id == g->title_edit ||
            box->id == g->comment ||
            in_view(s, box->x, box->y, box->width, box->height)) {
            sg_emit_comment(s, &gr->comment_boxes[i]);
        }
    }
    emit_conversions(s);
    emit_reroutes(s);
    uint64_t focused = focused_node(s);
    for (int i = 0; i < acount(gr->nodes); i++) {
        if (sg_node_visible(gr, gr->nodes[i]) && keep_node(s, gr->nodes[i], focused)) {
            emit_node(s, gr->nodes[i]);
            gr = sg_active_graph(s);
        }
    }
    kit_pop(k);
    kit_root(k, "front");
    emit_front(s);
}
