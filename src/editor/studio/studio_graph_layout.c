#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
/* Node sizes, rows and socket positions for the graph canvas. */
#include "editor/documents/component_properties.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/ui/labels.h"
#include "graph/authoring/pin_edit.h"
#include "graph/model/local.h"
#include "graph/model/pin_conv.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/component_nodes.h"
#include "graph/nodes/fmt_nodes.h"
#include "graph/shader/shader_graph.h"
#include "platform/cute.h"
#include "support/array.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float HEAD = 24, PAD = 4;

/* ---------- Node layout ----------
   Nodes are sized here rather than by RmlUi, from measured text and the
   graph.rcss metrics, so a node's box and sockets are known before it is laid
   out: arrange, framing and hit tests never wait a frame.

   A full node's rows follow Blender: execution pins pair
   up first, then every data output, then every data input, so inputs always
   sit below outputs. Unlabeled execution pins may sit in the title instead
   (Unity Visual Scripting). Operators draw as a symbol between their inputs
   and output, and one-output reads as pills (Unreal's compact nodes). */

bool sg_pin_exec(const node *n, int kind, int pin)
{
    return (kind == ED_PIN_INPUT ? n->input_pins : n->output_pins)[pin].kind == PIN_EXEC;
}

bool sg_in_head(const node_layout *l, int kind, int pin)
{
    return pin == (kind == ED_PIN_INPUT ? l->head_in : l->head_out);
}

/* Whether a pin has a row: visible and not drawn in the title. */
bool sg_pin_in_rows(const graph *g, const node *n, const node_layout *l, int kind, int pin)
{
    return ed_pin_visible(g, n, kind, pin) && !sg_in_head(l, kind, pin);
}

/* Index among the row pins on the same side of the same kind (exec or data). */
int sg_pin_order(const graph *g, const node *n, const node_layout *l, int kind, int pin)
{
    int order = 0;
    for (int p = 0; p < pin; p++) {
        order += sg_pin_in_rows(g, n, l, kind, p) &&
                 sg_pin_exec(n, kind, p) == sg_pin_exec(n, kind, pin);
    }
    return order;
}

static int pin_count(const graph *g, const node *n, const node_layout *l, int kind, bool exec)
{
    int count = 0;
    for (int p = 0; p < (kind == ED_PIN_INPUT ? n->input_count : n->output_count); p++) {
        count += sg_pin_in_rows(g, n, l, kind, p) && sg_pin_exec(n, kind, p) == exec;
    }
    return count;
}

int sg_exec_rows(const graph *g, const node *n, const node_layout *l)
{
    int in = pin_count(g, n, l, ED_PIN_INPUT, true),
        out = pin_count(g, n, l, ED_PIN_OUTPUT, true);
    return in > out ? in : out;
}

int sg_row_count(const graph *g, const node *n, const node_layout *l)
{
    return sg_exec_rows(g, n, l) + pin_count(g, n, l, ED_PIN_OUTPUT, false) +
           pin_count(g, n, l, ED_PIN_INPUT, false);
}

static int pin_row(const graph *g, const node *n, const node_layout *l, int kind, int pin)
{
    int order = sg_pin_order(g, n, l, kind, pin);
    if (sg_pin_exec(n, kind, pin) || l->form == FORM_OPERATOR) {
        return order;
    }
    int outputs = kind == ED_PIN_INPUT ? pin_count(g, n, l, ED_PIN_OUTPUT, false) : 0;
    return sg_exec_rows(g, n, l) + outputs + order;
}

static bool is_expression(const studio *s, const node *n)
{
    return behavior_expression_source(sg_graph_editor(s)->asset, n) != NULL;
}

const char *sg_node_kind(const studio *s, const node *n)
{
    if (is_expression(s, n)) {
        return "expression";
    }
    if (ed_is_graph_call(n)) {
        return "call";
    }
    switch (n->desc->display_class) {
        case NODE_DISPLAY_EVENT: return "event";
        case NODE_DISPLAY_FUNCTION: return "function";
        case NODE_DISPLAY_FLOW: return "flow";
        case NODE_DISPLAY_VARIABLE: return "variable";
        case NODE_DISPLAY_LITERAL: return "literal";
        default: return "pure";
    }
}

uint32_t sg_node_icon(const studio *s, const node *n)
{
    if (is_expression(s, n)) {
        return KIT_ICON_EXPRESSION;
    }
    if (ed_is_graph_call(n)) {
        return KIT_ICON_FUNCTION;
    }
    switch (n->desc->display_class) {
        case NODE_DISPLAY_EVENT: return KIT_ICON_EVENT;
        case NODE_DISPLAY_FLOW: return KIT_ICON_BRANCH;
        case NODE_DISPLAY_FUNCTION: return KIT_ICON_FUNCTION;
        case NODE_DISPLAY_VARIABLE: return KIT_ICON_FIELD;
        default: return 0;
    }
}

/* ---------- Measurement ---------- */

static const float COLUMN_GAP = 12, HEAD_PAD = 10;
static const float NODE_MIN = 120, NODE_MAX = 360, BOTTOM = 6;
static const float OPERATOR_MIN = 56, OPERATOR_PAD = 4, SYMBOL_SIZE = 18;

/* graph.rcss .gcode: 16 dp lines, 8 dp of padding and 4 below; 28 dp across. */
static const float CODE_SIZE = 11, CODE_LINE = 16, CODE_PAD = 12, CODE_INSET = 48;

/* Color presentation belongs to the explicit pin flag or the reflected field
   catalog, not to a guess based on names like "Color" or the C type `unsigned`. */
bool sg_color_pin(const node *n, const pin_spec *spec)
{
    int width = pin_vector_width(spec);
    if ((spec->flags & PIN_FLAG_COLOR) &&
        (spec->type == PIN_U32 || spec->type == PIN_I32 || width == 3 || width == 4)) {
        return true;
    }
    if (spec->type != PIN_U32) {
        return false;
    }
    const ecs_component_desc *component = component_node_component(n);
    const node_desc *structure = component_node_struct(n);
    const type_desc *type = component ? component->type
                          : structure ? structure->type
                                      : NULL;
    for (int i = 0; type && i < type->field_count; i++) {
        const type_field *field = &type->fields[i];
        if (field->name && spec->name && !strcmp(field->name, spec->name) &&
            appearance_color_field(type, field)) {
            return true;
        }
    }
    return false;
}

value_kind sg_value_kind_of(const studio *s, node *n, int output, int p)
{
    const pin_spec *spec = output ? &n->output_pins[p] : &n->input_pins[p];
    if (output && !ed_pin_editable(sg_active_graph(s), n, ED_PIN_OUTPUT, p)) {
        return VALUE_NONE;
    }
    int width = pin_vector_width(spec);
    const editor_choices *choices = ed_pin_choices(sg_graph_editor(s), n, output, p);
    if (spec->container != PIN_CONTAINER_SINGLE) {
        return VALUE_COMPOUND;
    }
    if (choices && editor_choices_count(choices) > 0) {
        return VALUE_CHOICE;
    }
    if (sg_color_pin(n, spec)) {
        return VALUE_COLOR;
    }
    if (spec->type == PIN_BOOL) {
        return VALUE_BOOL;
    }
    if (width > 0 && !(spec->flags & PIN_FLAG_COLOR)) {
        return VALUE_VECTOR;
    }
    if (spec->type == PIN_STR || spec->type == PIN_NAME) {
        return VALUE_TEXT;
    }
    if (spec->type == PIN_STRUCT) {
        return VALUE_COMPOUND;
    }
    return spec->type >= PIN_I8 && spec->type <= PIN_F64 ? VALUE_NUMBER : VALUE_NONE;
}

static bool exec_label(const pin_spec *spec, const char *label)
{
    return spec->kind == PIN_EXEC &&
           (!strcmp(label, "Exec") || !strcmp(label, "Then") ||
            !strcmp(label, "In") || !strcmp(label, "Out"));
}

/* "transform.position" reads as "Transform › Position". */
static void path_label(const char *path, char *out, size_t size)
{
    size_t used = 0;
    out[0] = '\0';
    for (const char *at = path; *at && used < size;) {
        const char *end = strchr(at, '.');
        int length = end ? (int)(end - at) : (int)strlen(at);
        char part[128], word[128];
        snprintf(part, sizeof part, "%.*s", length, at);
        editor_identifier_label(part, word, sizeof word);
        used += (size_t)
            snprintf(out + used, size - used, "%s%s", used ? " \xe2\x80\xba " : "", word);
        at += length + (end != NULL);
    }
}

/* A bound value's chip: what it reads, and an icon for where it comes from.
   Component values always belong to the node's owner, so "Self" is implied. */
static uint32_t binding_chip(const studio *s, const pin_binding *b, char *out, size_t size)
{
    editor *e = sg_graph_editor(s);
    char first[128], second[160];
    if (s->prefs.short_bindings) {
        switch (b->kind) {
            case PIN_BINDING_COMPONENT:
            case PIN_BINDING_COMPONENT_FIELD: {
                const ecs_component_desc *c = ed_component_by_key(b->component_key);
                const char *name = c && c->name ? c->name : b->component_key;
                if (!name) {
                    break;
                }
                editor_component_label(name, first, sizeof first);
                if (b->kind == PIN_BINDING_COMPONENT || !b->field_name) {
                    snprintf(out, size, "%s", first);
                    return KIT_ICON_COMPONENT;
                }
                path_label(b->field_name, second, sizeof second);
                snprintf(out, size, "%s \xe2\x80\xba %s", first, second);
                return KIT_ICON_COMPONENT;
            }
            case PIN_BINDING_VARIABLE: {
                const behavior_variable *v = behavior_asset_find_variable_by_id(e->asset, b->variable_id);
                bool here = !b->source_asset || !e->asset->name ||
                            !strcmp(b->source_asset, e->asset->name);
                if (!here || !v || !v->name) {
                    break;
                }
                editor_identifier_label(v->name, out, size);
                return KIT_ICON_VARIABLE;
            }
            case PIN_BINDING_LOCAL: {
                const graph_local *local = graph_local_by_id(e->active_graph, b->local_id);
                if (!local || !local->name) {
                    break;
                }
                editor_identifier_label(local->name, out, size);
                return KIT_ICON_LOCAL;
            }
            case PIN_BINDING_EVENT_OUTPUT: {
                node *event = graph_node_by_id(e->active_graph, b->event_node_id);
                int output = 0;
                const pin_spec *pin = event
                                        ? graph_pin_by_id(event, b->event_pin_id, &output, NULL)
                                        : NULL;
                if (!pin || !output || !pin->name) {
                    break;
                }
                editor_identifier_label(pin->name, second, sizeof second);
                if (event->reference_name) {
                    const char *name = event->reference_name;
                    snprintf(out, size, "%s \xe2\x80\xba %s", name, second);
                } else {
                    snprintf(out, size, "%s", second);
                }
                return KIT_ICON_EVENT;
            }
            case PIN_BINDING_DELTA_TIME:
                snprintf(out, size, "Delta Time");
                return KIT_ICON_TIMER;
            default: break;
        }
    }
    /* Full paths, and missing sources, which say what is missing. */
    snprintf(out, size, "%s", ed_binding_label(e, b, second, sizeof second));
    return 0;
}

/* A folded source's chip: the expression the input receives. With Short
   Bindings each value it reads is named as its binding chip names it, and a
   chip that is one read takes that read's icon. */
static uint32_t folded_chip(const studio *s, node *n, int p, char *out, size_t size)
{
    node *source = NULL;
    int source_pin;
    if (graph_input_source(sg_active_graph(s), n, p, &source, &source_pin) &&
        source->desc == &NODE_BehaviorExpression) {
        char *readable = studio_code_expression(s, source);
        snprintf(out, size, "%s", readable ? readable : "");
        free(readable);
        return 0;
    }
    char text[256];
    behavior_expression_read *reads = NULL;
    behavior_expression_format(
        sg_active_graph(s),
        n,
        p,
        text,
        sizeof text,
        s->prefs.short_bindings ? &reads : NULL
    );
    uint32_t icon = 0;
    size_t used = 0;
    int at = 0;
    out[0] = '\0';
    for (int i = 0; i < acount(reads); i++) {
        char name[160];
        uint32_t read_icon = binding_chip(s, &reads[i].read, name, sizeof name);
        bool whole = acount(reads) == 1 && reads[i].start == 0 &&
                     reads[i].length == (int)strlen(text);
        icon = whole ? read_icon : 0;
        used += (size_t)
            snprintf(out + used, size - used, "%.*s%s", reads[i].start - at, text + at, name);
        used = used < size ? used : size - 1;
        at = reads[i].start + reads[i].length;
    }
    snprintf(out + used, size - used, "%s", text + at);
    afree(reads);
    return icon;
}

void sg_pin_parts_of(const studio *s, node *n, int kind, int p, pin_parts *out)
{
    graph *gr = sg_active_graph(s);
    bool input = kind == ED_PIN_INPUT;
    const pin_spec *spec = input ? &n->input_pins[p] : &n->output_pins[p];
    *out = (pin_parts){ .spec = spec };
    node *src = NULL;
    int src_pin = -1;
    if (input) {
        out->connected = graph_input_source(gr, n, p, &src, &src_pin) != 0;
        out->inline_source = out->connected && graph_node_is_inline(gr, src);
    } else {
        out->connected = graph_output_local(gr, n, p) != NULL;
    }
    const graph_local *local = input ? NULL : graph_output_local(gr, n, p);
    /* Execution pins, and outputs, are wired when any edge touches them. */
    for (int i = 0; i < acount(gr->edges) && !out->connected; i++) {
        const edge *ed = &gr->edges[i];
        out->connected = input ? ed->to_id == n->id && ed->to_pin_id == spec->id
                               : ed->from_id == n->id && ed->from_pin_id == spec->id;
    }
    studio_code_pin_label(s, n, spec, out->label, sizeof out->label);
    if (local) {
        editor_identifier_label(local->name, out->label, sizeof out->label);
    }
    out->show_label = !exec_label(spec, out->label);
    out->source_select = input && !sg_graph_readonly(s) && spec->kind == PIN_DATA &&
                         !(spec->flags & PIN_FLAG_SETTING) &&
                         (out->connected || ed_pin_editable(gr, n, kind, p));
    /* An unwired shader input that samples its surface says what it reads
       (Fresnel's Normal reads the Surface Normal), not a zero vector. */
    const char *context = NULL;
    if (input && !out->connected && !spec->binding && !spec->default_value.ptr &&
        shader_node_input_context(n, p, &context) >= 0 && context) {
        snprintf(out->chip, sizeof out->chip, "%s", context);
        out->value = VALUE_CHIP;
        return;
    }
    if (out->inline_source) {
        out->chip_icon = folded_chip(s, n, p, out->chip, sizeof out->chip);
        out->value = VALUE_CHIP;
    } else if (
        input && out->connected && src && src_pin >= 0 &&
        (!spec->binding || spec->binding->kind != PIN_BINDING_LOCAL)
    ) {
        char title[128], pin_label[96];
        ed_node_title(sg_graph_editor(s), src, title, sizeof title);
        studio_code_pin_label(s, src, &src->output_pins[src_pin], pin_label, sizeof pin_label);
        snprintf(out->chip, sizeof out->chip, "%s \xe2\x80\xba %s", title, pin_label);
        out->chip_icon = sg_node_icon(s, src);
        out->value = VALUE_CHIP;
    } else if (input && spec->binding) {
        out->chip_icon = binding_chip(s, spec->binding, out->chip, sizeof out->chip);
        out->value = VALUE_CHIP;
    } else if ((!input || !out->connected) && ed_pin_editable(gr, n, kind, p)) {
        out->value = sg_value_kind_of(s, n, !input, p);
    }
}

/* Layout uses the same text metrics at every camera zoom. Measuring at the
   live zoom makes hinted glyph advances change node widths and socket
   positions as the camera moves. The canvas still rasterizes text at its
   displayed zoom. */
float sg_text_width(const studio *s, const char *text, float size, int weight)
{
    float ratio = fmaxf(studio_pixel_ratio(s) * s->prefs.standard_font_size / 12.0f, FAR_PIXELS);
    return s->graph.kit ? kit_text_width(s->graph.kit, text, size, weight, ratio)
                        : strlen(text) * size * 0.55f;
}

/* The least width of a pin's editor or chip; editors then fill the row. */
static float value_width(const studio *s, const pin_parts *parts)
{
    float width = 0;
    switch (parts->value) {
        case VALUE_CHOICE: width = 96; break;
        case VALUE_BOOL: width = 16; break;
        case VALUE_VECTOR:
            width = 58.0f * (float)pin_vector_width(parts->spec);
            break;
        case VALUE_COLOR: width = 58; break;
        case VALUE_TEXT: width = 96; break;
        case VALUE_NUMBER: width = 64; break;
        case VALUE_COMPOUND: width = 96; break;
        case VALUE_CHIP:
            width = sg_text_width(s, parts->chip, 11, 400) + 12 +
                    (parts->chip_icon ? 16 : 0) +
                    (parts->connected || parts->spec->binding ? 2 : 0);
            if (parts->inline_source || (!parts->connected && !parts->spec->binding)) {
                width = fminf(width, 200);
            }
            break;
        default: break;
    }
    /* An unbound compound value keeps room for Bind to Source beside it. */
    bool compound = parts->value == VALUE_VECTOR || parts->value == VALUE_COLOR ||
                    parts->value == VALUE_COMPOUND;
    return width + (compound && parts->source_select ? BIND_WIDTH : 0);
}

/* Operators and one-output reads compact (Unreal); anything with execution
   pins, several outputs or an editable output stays a full node. */
static node_form form_of(const studio *s, node *n)
{
    graph *gr = sg_active_graph(s);
    if (!s->prefs.compact_nodes || ed_is_graph_call(n)) {
        return FORM_FULL;
    }
    int inputs = 0, outputs = 0, output = -1;
    for (int kind = ED_PIN_INPUT; kind <= ED_PIN_OUTPUT; kind++) {
        int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
        for (int p = 0; p < count; p++) {
            if (!ed_pin_visible(gr, n, kind, p)) {
                continue;
            }
            if (sg_pin_exec(n, kind, p)) {
                return FORM_FULL;
            }
            inputs += kind == ED_PIN_INPUT;
            outputs += kind == ED_PIN_OUTPUT;
            output = kind == ED_PIN_OUTPUT ? p : output;
        }
    }
    if (outputs != 1) {
        return FORM_FULL;
    }
    pin_parts parts;
    sg_pin_parts_of(s, n, ED_PIN_OUTPUT, output, &parts);
    /* A named value shows its name, so its node keeps a labeled row. */
    if (parts.value != VALUE_NONE || graph_output_local(sg_active_graph(s), n, output)) {
        return FORM_FULL;
    }
    if (!inputs) {
        return FORM_GETTER;
    }
    return ed_node_symbol(n->desc) ? FORM_OPERATOR : FORM_FULL;
}

/* The first execution pin on a side moves into the title when unlabeled. */
static int title_pin(const graph *gr, node *n, int kind)
{
    int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
    for (int p = 0; p < count; p++) {
        if (!ed_pin_visible(gr, n, kind, p) || !sg_pin_exec(n, kind, p)) {
            continue;
        }
        const pin_spec *spec = kind == ED_PIN_INPUT ? &n->input_pins[p]
                                                    : &n->output_pins[p];
        char label[128];
        ed_pin_label(n, spec, label, sizeof label);
        return exec_label(spec, label) ? p : -1;
    }
    return -1;
}

/* Expressions show their source in the body, so the title carries a name the
   author gave instead; pills drop the "Get" every read would repeat. */
const char *sg_node_title(const studio *s, node *n, node_form form, char *buf, size_t size)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    const char *source = behavior_expression_source(asset, n);
    if (source) {
        const graph *generated = behavior_asset_find_graph(asset, n->reference_id);
        const char *name = generated->name && generated->name[0] ? generated->name
                                                                 : source;
        snprintf(buf, size, "%s", strcmp(name, source) ? name : "Expression");
        return buf;
    }
    ed_node_title(sg_graph_editor(s), n, buf, size);
    if (form == FORM_GETTER && !strncmp(buf, "Get ", 4)) {
        memmove(buf, buf + 4, strlen(buf + 4) + 1);
    }
    return buf;
}

/* Breaks source at spaces into lines no wider than `width`, filling starts
   with each line's offset; returns the line count. A word wider than a line
   keeps a line of its own, and the last line takes whatever is left. */
int sg_code_lines(const studio *s, const char *code, float width, int starts[CODE_LINES])
{
    int count = 1, start = 0, fits = -1; /* fits: the end of the text that fits */
    starts[0] = 0;
    for (int i = 0; count < CODE_LINES;) {
        /* Lines break at spaces, and after operators and commas so that
           unspaced source wraps too. */
        while (code[i] && !strchr(" ,+-*/%^&|<>=", code[i])) {
            i++;
        }
        int end = code[i] && code[i] != ' ' ? i + 1 : i;
        char line[512];
        snprintf(line, sizeof line, "%.*s", end - start, code + start);
        if (fits > start && sg_text_width(s, line, CODE_SIZE, 400) > width) {
            start = fits + (code[fits] == ' ');
            starts[count++] = start;
            fits = -1;
            continue;
        }
        fits = end;
        if (!code[i]) {
            break;
        }
        i++;
    }
    return count;
}

/* Nodes are border-box: rows and code sit inside the node's one dp border. */
float sg_code_room(const node_layout *l)
{
    return l->w - 2 * EDGE - CODE_INSET;
}

float sg_code_height(const node_layout *l)
{
    return l->code_lines ? CODE_PAD + (float)l->code_lines * CODE_LINE : 0;
}

static node_layout measure_operator(const studio *s, node *n, node_layout l, const char *symbol)
{
    graph *gr = sg_active_graph(s);
    float values = 0;
    int inputs = 0;
    for (int p = 0; p < n->input_count; p++) {
        if (!ed_pin_visible(gr, n, ED_PIN_INPUT, p)) {
            continue;
        }
        pin_parts parts;
        sg_pin_parts_of(s, n, ED_PIN_INPUT, p, &parts);
        values = fmaxf(values, value_width(s, &parts));
        inputs++;
    }
    l.column = SOCKET_PAD + values;
    l.w = ceilf(fmaxf(
        2 * EDGE + l.column + 2 * PART_GAP +
            sg_text_width(s, symbol, SYMBOL_SIZE, 700) + SOCKET_PAD,
        OPERATOR_MIN
    ));
    l.top = EDGE + OPERATOR_PAD;
    l.h = 2 * (EDGE + OPERATOR_PAD) + (float)(inputs > 1 ? inputs : 1) * ROW;
    return l;
}

static node_layout measure_node(const studio *s, node *n)
{
    graph *gr = sg_active_graph(s);
    node_layout l = { .id = n->id,
                      .form = form_of(s, n),
                      .head_in = -1,
                      .head_out = -1,
                      .add_pin = -1 };
    char title[256];
    sg_node_title(s, n, l.form, title, sizeof title);
    const char *symbol = ed_node_symbol(n->desc);
    if (l.form == FORM_OPERATOR) {
        return measure_operator(s, n, l, symbol);
    }
    float width = sg_text_width(s, title, 12, 600) + 2 * HEAD_PAD +
                  (symbol || sg_node_icon(s, n) ? 20 : 0);
    if (l.form == FORM_GETTER) {
        for (int p = 0; p < n->output_count; p++) {
            l.head_out = ed_pin_visible(gr, n, ED_PIN_OUTPUT, p) ? p : l.head_out;
        }
        l.w = ceilf(2 * EDGE + width + PART_GAP);
        l.h = 2 * EDGE + HEAD;
        return l;
    }
    if (s->prefs.title_flow) {
        l.head_in = title_pin(gr, n, ED_PIN_INPUT);
        l.head_out = title_pin(gr, n, ED_PIN_OUTPUT);
    }
    float values = 0, exec_in = 0, exec_out = 0;
    for (int kind = ED_PIN_INPUT; kind <= ED_PIN_OUTPUT; kind++) {
        int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
        for (int p = 0; p < count; p++) {
            if (!sg_pin_in_rows(gr, n, &l, kind, p)) {
                continue;
            }
            pin_parts parts;
            sg_pin_parts_of(s, n, kind, p, &parts);
            float label = parts.show_label ? sg_text_width(s, parts.label, 12, 400)
                                           : 0;
            float value = value_width(s, &parts);
            if (sg_pin_exec(n, kind, p)) {
                float *side = kind == ED_PIN_INPUT ? &exec_in : &exec_out;
                *side = fmaxf(*side, SOCKET_PAD + label);
            } else if (kind == ED_PIN_INPUT) {
                l.label = fmaxf(l.label, label);
                values = fmaxf(values, value);
            } else {
                width = fmaxf(width, 2 * SOCKET_PAD + label + (value > 0 ? PART_GAP + value : 0));
            }
        }
    }
    width = fmaxf(width, exec_in + exec_out + COLUMN_GAP);
    l.label = ceilf(l.label);
    if (l.label > 0 || values > 0) {
        width = fmaxf(
            width,
            2 * SOCKET_PAD + l.label + (l.label > 0 && values > 0 ? PART_GAP : 0) + values
        );
    }
    char *code = studio_code_expression(s, n);
    if (code) {
        width = fmaxf(width, sg_text_width(s, code, CODE_SIZE, 400) + CODE_INSET);
    }
    l.w = ceilf(fminf(fmaxf(2 * EDGE + width, NODE_MIN), NODE_MAX));
    float room = l.w - 2 * EDGE - 2 * SOCKET_PAD;
    float value_room = fminf(values, room * 0.6f);
    l.label = fminf(l.label, fmaxf(0, room - value_room - (values > 0 ? PART_GAP : 0)));
    if (code) {
        int starts[CODE_LINES];
        l.code_lines = sg_code_lines(s, code, sg_code_room(&l), starts);
    }
    free(code);
    l.top = EDGE + HEAD + PAD + sg_code_height(&l);
    /* Variadic nodes end with a row that adds a pin (Unreal); Format Text
       keeps a spare argument itself. */
    graph_pin_run run;
    if (graph_pin_run_of(n, &run) && n->desc != &NODE_Format_Text) {
        l.add_pin = run.output ? ED_PIN_OUTPUT : ED_PIN_INPUT;
    }
    int rows = sg_row_count(gr, n, &l) + (l.add_pin >= 0);
    l.h = rows || l.code_lines ? l.top + (float)rows * ROW + BOTTOM + EDGE
                               : 2 * EDGE + HEAD;
    return l;
}

/* Intrinsic geometry is independent of the viewport and camera. */
const node_layout *sg_layout_of(const studio *s, node *n)
{
    sg_calculations_sync(s);
    studio_graph *g = (studio_graph *)&s->graph;
    struct studio_graph_calculations *c = g->calculations;
    int index = cf_map_get(c->layout_index, n->id);
    if (index) {
        return &g->layouts[index - 1];
    }
    node_layout l = measure_node(s, n);
    apush(g->layouts, l);
    cf_map_set(c->layout_index, n->id, acount(g->layouts));
    c->measurements++;
    return &alast(g->layouts);
}

/* Where a socket sits below its node's top. */
float sg_pin_y(const studio *s, node *n, int kind, int pin)
{
    node_layout l = *sg_layout_of(s, n);
    if (sg_in_head(&l, kind, pin)) {
        return EDGE + HEAD / 2;
    }
    if (l.form == FORM_OPERATOR && kind == ED_PIN_OUTPUT) {
        return l.h / 2;
    }
    return l.top + ((float)pin_row(sg_active_graph(s), n, &l, kind, pin) + 0.5f) * ROW;
}

/* A node's box in graph units. */
bool sg_node_box(const studio *s, node *n, float box[4])
{
    const node_layout *l = sg_layout_of(s, n);
    box[0] = n->x;
    box[1] = n->y;
    box[2] = l->w;
    box[3] = l->h;
    return true;
}

/* Where a socket sits, from the same metrics that size nodes. */
void sg_socket_at(const studio *s, node *n, int kind, int pin, float *x, float *y)
{
    *x = kind == ED_PIN_INPUT ? n->x : n->x + sg_layout_of(s, n)->w;
    *y = n->y + sg_pin_y(s, n, kind, pin);
}
