/* Live values (studio_graph_live.c): what the debug target's pins last
 * carried while it runs, written compactly, as Unreal's watch bubbles and
 * Blender's socket inspection show them. Values are read ten times a second,
 * so numbers that change every frame stay readable. */
#include "editor/studio/studio_graph_internal.h"
#include "graph/model/registry.h"
#include "support/array.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const double LIVE_PERIOD = 0.1;
/* Longer text ends in an ellipsis. */
enum
{
    LIVE_TEXT_MAX = 24
};

/* Drops a fraction's trailing zeros, and a negative zero's sign. */
static void trim(char *text)
{
    char *dot = strchr(text, '.');
    if (dot) {
        char *end = text + strlen(text);
        while (end > dot + 1 && end[-1] == '0') {
            *--end = '\0';
        }
        if (end == dot + 1) {
            *dot = '\0';
        }
    }
    if (!strcmp(text, "-0")) {
        strcpy(text, "0");
    }
}

/* About three significant digits, never in exponent form. */
static void compact_number(double value, char *buf, size_t cap)
{
    double size = fabs(value);
    int decimals = size >= 100 ? 0 : size >= 10 ? 1 : size >= 1 ? 2 : 3;
    snprintf(buf, cap, "%.*f", decimals, value);
    trim(buf);
}

static bool number_of(const pin_value *v, double *out)
{
    switch (v->type) {
        case PIN_F32: *out = v->f; return true;
        case PIN_F64: *out = v->d; return true;
        case PIN_I32: *out = v->i; return true;
        case PIN_U32: *out = v->u; return true;
        case PIN_I64: *out = (double)v->i64; return true;
        default: return false;
    }
}

/* A vector or a small all-number struct as "(1.5, -2)". */
static bool compact_struct(const pin_value *v, const pin_spec *spec, char *buf, size_t cap)
{
    const type_desc *type = type_catalog_find(v->type_name ? v->type_name : spec->type_name);
    if (!type || !v->ptr || type->field_count < 1 || type->field_count > 4) {
        return false;
    }
    size_t used = (size_t)snprintf(buf, cap, "(");
    for (int i = 0; i < type->field_count && used < cap; i++) {
        pin_value field;
        double number;
        if (type->fields[i].type < PIN_I8 || type->fields[i].type > PIN_F64 ||
            pin_read_field(NULL, v->ptr, &type->fields[i], &field)) {
            return false;
        }
        if (!number_of(&field, &number)) {
            return false;
        }
        char part[32];
        compact_number(number, part, sizeof part);
        used += (size_t)snprintf(buf + used, cap - used, "%s%s", i ? ", " : "", part);
    }
    if (used < cap) {
        snprintf(buf + used, cap - used, ")");
    }
    return true;
}

static void ellipsize(char *text)
{
    if (strlen(text) > LIVE_TEXT_MAX) {
        strcpy(text + LIVE_TEXT_MAX - 1, "\xe2\x80\xa6");
    }
}

static void compact(studio *s, const node *n, const pin_spec *spec, const pin_value *v, studio_live_value *out)
{
    double number;
    if (sg_color_pin(n, spec)) {
        out->color = true;
        out->swatch = sg_literal_color(spec, v);
        snprintf(out->text, sizeof out->text, "#%06x", out->swatch >> 8);
        return;
    }
    if (v->container == PIN_CONTAINER_SINGLE &&
        (v->type == PIN_F32 || v->type == PIN_F64) && number_of(v, &number)) {
        compact_number(number, out->text, sizeof out->text);
        return;
    }
    if (v->container == PIN_CONTAINER_SINGLE && v->type == PIN_STRUCT &&
        compact_struct(v, spec, out->text, sizeof out->text)) {
        return;
    }
    snprintf(
        out->text,
        sizeof out->text,
        "%s",
        eval_view_format_value(sg_graph_editor(s)->ev, v)
    );
    ellipsize(out->text);
}

bool sg_live_value(studio *s, node *n, int output, int pin, studio_live_value *out)
{
    studio_graph *g = &s->graph;
    if (s->time - g->live.time >= LIVE_PERIOD || s->time < g->live.time) {
        aclear(g->live.values);
        g->live.time = s->time;
    }
    const pin_spec *spec = output ? &n->output_pins[pin] : &n->input_pins[pin];
    for (int i = 0; i < acount(g->live.values); i++) {
        const studio_live_value *seen = &g->live.values[i];
        if (seen->pin == spec->id && seen->node == n->id && seen->output == output) {
            *out = *seen;
            return out->found;
        }
    }
    *out = (studio_live_value){ .node = n->id, .pin = spec->id, .output = output };
    pin_value value;
    editor *e = sg_graph_editor(s);
    out->found = eval_view_live_value(e->ev, sg_active_graph(s), n, output, pin, &value) ==
                 0;
    if (out->found) {
        compact(s, n, spec, &value, out);
    }
    apush(g->live.values, *out);
    return out->found;
}
