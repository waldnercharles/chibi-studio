#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/local_edit.h"
/* The Node tab in Properties (workstream G1): the selected node's pins and
 * settings, every pin, advanced ones included, with what feeds it, or a
 * comment frame's title and color, as rows of graph_details
 * (studio_graph_details.c). The canvas shows the common case, this shows
 * everything (Unreal's Details). A row's id names what it edits:
 * n<node>.<part><index>, with a compound value's place after a slash, or
 * c<frame>.<part>. */
#include "authoring/reference_index.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/ui/choices.h"
#include "editor/ui/labels.h"
#include "graph/model/local.h"
#include "graph/model/pin_conv.h"
#include "graph/model/registry.h"
#include "graph/modeling/model_authoring.h"
#include "graph/nodes/behavior_expression.h"
#include "support/arena.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "types/array.h"
#include "types/text.h"
#include "ui/popover.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t studio_graph_node(const studio *s)
{
    const editor *e = sg_graph_editor(s);
    return studio_graph_active(s) && acount(e->selected) > 0 ? e->selected[0]->id : 0;
}

uint64_t studio_graph_comment(const studio *s)
{
    const graph_comment_box *box = studio_graph_node(s)
                                     ? NULL
                                     : sg_comment_by_id(s, s->graph.comment);
    return box ? box->id : 0;
}

/* ---------- Row ids ---------- */

/* What a row edits: owner n (a node) or c (a comment frame), its id, the
   part (i input, o output, m an output's name, x the expression, d
   Disabled, b Breakpoint, k the comment, h Show Comment, a Advanced Inputs;
   t a frame's title, c its color), the pin, and a compound value's place:
   f<field> and a<item> steps joined by slashes. */
typedef struct
{
    char owner, part;
    uint64_t id;
    int index;
    const char *path;
} target;

static void row_id(char out[GD_ROW_ID], char owner, uint64_t id, char part, int index)
{
    snprintf(out, GD_ROW_ID, "%c%" PRIu64 ".%c%d", owner, id, part, index);
}

static bool target_of(const char *id, target *out)
{
    char *end;
    *out = (target){ .owner = id[0], .index = -1 };
    out->id = strtoull(id + 1, &end, 10);
    if (*end != '.' || !end[1]) {
        return false;
    }
    out->part = end[1];
    out->index = (int)strtol(end + 2, &end, 10);
    out->path = *end == '/' ? end + 1 : "";
    return true;
}

/* ---------- Values ---------- */

static bool pin_is_string(pin_type type)
{
    return type == PIN_STR || type == PIN_NAME;
}

static const pin_spec *pin_of(const node *n, int output, int p)
{
    return output ? &n->output_pins[p] : &n->input_pins[p];
}

/* The pin's choices, the place of its value among them or -1. */
static int choice_index(const pin_spec *spec, const pin_value *value, const editor_choices *choices)
{
    for (int i = 0; i < editor_choices_count(choices); i++) {
        editor_choice choice = editor_choice_at(choices, i);
        bool same = spec->type == PIN_NAME
                      ? value->str && choice.name && !strcmp(value->str, choice.name)
                  : spec->type == PIN_ENUM
                      ? choice.value == value->e
                      : choice.value == sg_literal_number(value);
        if (same) {
            return i;
        }
    }
    return -1;
}

static gd_number_shape number_shape(pin_type type)
{
    bool integer = type < PIN_F32;
    gd_number_shape shape = { .integer = integer,
                              .single = type == PIN_F32,
                              .precision = integer ? 0 : 2,
                              .step = integer ? 1 : 0.1,
                              .min = -INFINITY,
                              .max = INFINITY };
    sg_literal_bounds(type, &shape.min, &shape.max);
    return shape;
}

/* An 8-byte integer edits as text, as the canvas's field does. */
static void integer64_text(const pin_value *value, char text[32])
{
    if (value->type == PIN_I64) {
        snprintf(text, 32, "%" PRId64, value->i64);
        return;
    }
    snprintf(text, 32, "%" PRIu64, value->u64);
}

static bool parse_integer64(pin_value *value, const char *text)
{
    char *end;
    errno = 0;
    if (value->type == PIN_I64) {
        long long parsed = strtoll(text, &end, 10);
        value->i64 = parsed;
    } else {
        if (strchr(text, '-')) {
            return false;
        }
        unsigned long long parsed = strtoull(text, &end, 10);
        value->u64 = parsed;
    }
    return text[0] && !*end && errno != ERANGE;
}

/* ---------- Compound values ---------- */

/* The value at `path` in `value`, which a compound pin's default holds in
   `scratch`: f<i> steps into a struct's field, a<i> into an array's item.
   The leaf is read into `out`, with its shape; false when the path names
   none. */
typedef struct
{
    pin_value value;
    pin_spec shape;
    const type_desc *owner; /* the struct holding it, or NULL */
    const type_field *field;
} leaf;

static bool leaf_at(
    arena *scratch,
    const pin_value *value,
    const pin_spec *shape,
    const char *path,
    leaf *out
)
{
    *out = (leaf){ .value = *value, .shape = *shape };
    for (const char *at = path; at && *at;) {
        char *end;
        int index = (int)strtol(at + 1, &end, 10);
        if (at[0] == 'a') {
            const pin_array *array = out->value.ptr;
            if ((out->shape.container != PIN_CONTAINER_ARRAY &&
                 out->shape.container != PIN_CONTAINER_SET) ||
                !array || !pin_array_valid(array) || index < 0 ||
                index >= array->count) {
                return false;
            }
            out->value = pin_array_get(array, index);
            out->shape.container = PIN_CONTAINER_SINGLE;
            out->owner = NULL;
            out->field = NULL;
        } else {
            const type_desc *type = type_catalog_find(out->shape.type_name);
            if (!type || !out->value.ptr || index < 0 || index >= type->field_count) {
                return false;
            }
            const type_field *field = &type->fields[index];
            if (pin_read_field(scratch, out->value.ptr, field, &out->value)) {
                return false;
            }
            out->value.type_name = out->value.type_name ? out->value.type_name
                                                        : field->type_name;
            out->shape = (pin_spec){ .type = field->type,
                                     .type_name = field->type_name,
                                     .kind = PIN_DATA,
                                     .container = PIN_CONTAINER_SINGLE,
                                     .flags = !strcmp(type->name, "ShaderRampStop") &&
                                                      !strcmp(field->name, "Color")
                                                ? PIN_FLAG_COLOR
                                                : 0 };
            out->owner = type;
            out->field = field;
        }
        at = *end == '/' ? end + 1 : NULL;
    }
    return true;
}

/* Writes `with` at `path` in `value`, in place. */
static bool write_at(
    arena *scratch,
    pin_value *value,
    const pin_spec *shape,
    const char *path,
    const pin_value *with
)
{
    if (!path || !*path) {
        *value = *with;
        return true;
    }
    char *end;
    int index = (int)strtol(path + 1, &end, 10);
    const char *rest = *end == '/' ? end + 1 : "";
    if (path[0] == 'a') {
        pin_array *array = value->ptr;
        pin_spec item = *shape;
        item.container = PIN_CONTAINER_SINGLE;
        if ((shape->container != PIN_CONTAINER_ARRAY &&
             shape->container != PIN_CONTAINER_SET) ||
            !array || !pin_array_valid(array) || index < 0 || index >= array->count) {
            return false;
        }
        pin_value entry = pin_array_get(array, index);
        return write_at(scratch, &entry, &item, rest, with) &&
               !pin_array_set(array, index, &entry);
    }
    const type_desc *type = type_catalog_find(shape->type_name);
    if (!type || !value->ptr || index < 0 || index >= type->field_count) {
        return false;
    }
    const type_field *field = &type->fields[index];
    pin_value inner;
    if (pin_read_field(scratch, value->ptr, field, &inner)) {
        return false;
    }
    inner.type_name = inner.type_name ? inner.type_name : field->type_name;
    pin_spec inner_shape = { .type = field->type,
                             .type_name = field->type_name,
                             .kind = PIN_DATA,
                             .container = PIN_CONTAINER_SINGLE };
    if (!write_at(scratch, &inner, &inner_shape, rest, with)) {
        return false;
    }
    return !pin_write_field(scratch, value->ptr, field, inner);
}

static const char *arena_text(arena *scratch, const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = arena_alloc(scratch, length);
    memcpy(copy, text, length);
    return copy;
}

/* ---------- Writing rows ---------- */

typedef struct
{
    studio *s;
    node *n;
    int output, p;
    const char *id; /* the row's */
    bool readonly;
} pin_row;

static void leaf_row(
    gd_rows *r,
    const pin_row *pr,
    const gd_row *d,
    const leaf *l,
    const char *path,
    const double range[2]
);

/* A color, with its alpha beside it when it has one. */
static void color_row(gd_rows *r, const gd_row *d, const pin_spec *shape, const pin_value *value)
{
    gd_row row = *d;
    row.control = "color";
    yyjson_mut_val *item = gd_next_row(r, &row);
    char hex[16];
    snprintf(hex, sizeof hex, "#%08" PRIx32, sg_literal_color(shape, value));
    gd_put_text(r, item, "value", hex);
    const type_desc *type = shape->type == PIN_STRUCT
                              ? type_catalog_find(shape->type_name)
                              : NULL;
    bool alpha = type && type->field_count == 4;
    gd_put_flag(r, item, "alpha", alpha);
    if (alpha) {
        gd_put_alpha(r, item, sg_literal_alpha(shape, value));
    }
}

static void vector_row(gd_rows *r, const gd_row *d, const pin_value *value, const pin_spec *shape)
{
    const type_desc *type = type_catalog_find(shape->type_name);
    int width = pin_vector_width(shape);
    gd_row row = *d;
    row.control = "vector";
    yyjson_mut_val *item = gd_next_row(r, &row);
    double values[4] = { 0 };
    gd_number_shape axis = number_shape(PIN_F32);
    for (int i = 0; type && value->ptr && i < width; i++) {
        pin_value component;
        if (pin_read_field(NULL, value->ptr, &type->fields[i], &component)) {
            continue;
        }
        values[i] = sg_literal_number(&component);
        axis = number_shape(component.type);
    }
    gd_put_axes(r, item, values, width, &axis);
}

/* `path` joined with one more step, from a pin's root. */
static void step_path(char out[GD_ROW_ID], const char *path, char kind, int index)
{
    snprintf(out, GD_ROW_ID, "%s%s%c%d", path, path[0] ? "/" : "", kind, index);
}

static void leaf_id(char out[GD_ROW_ID], const pin_row *pr, const char *path)
{
    snprintf(out, GD_ROW_ID, "%s/%s", pr->id, path);
}

/* A struct's fields, each a row, in the order its type declares them. */
static void field_rows(gd_rows *r, const pin_row *pr, const leaf *l, const char *path, const double range[2])
{
    const type_desc *type = type_catalog_find(l->shape.type_name);
    for (int i = 0; type && l->value.ptr && i < type->field_count; i++) {
        char step[16], at[GD_ROW_ID], id[GD_ROW_ID], label[128];
        snprintf(step, sizeof step, "f%d", i);
        step_path(at, path, 'f', i);
        leaf_id(id, pr, at);
        editor_identifier_label(type->fields[i].name, label, sizeof label);
        arena scratch = { 0 };
        leaf field;
        if (!leaf_at(&scratch, &l->value, &l->shape, step, &field)) {
            arena_free(&scratch);
            continue;
        }
        /* A ramp stop's Position stays between its neighbours'. */
        bool position = range && !strcmp(type->fields[i].name, "Position");
        gd_row row = { .id = id,
                       .label = label,
                       .readonly = pr->readonly || (position && range[0] > range[1]) };
        leaf_row(r, pr, &row, &field, at, position ? range : NULL);
        arena_free(&scratch);
    }
}

/* An array's items, each a row, and a struct's beneath its heading; every
   item has its remove button, but a ramp's last two stops. */
static void item_rows(gd_rows *r, const pin_row *pr, const leaf *l)
{
    const pin_array *array = l->value.ptr;
    bool ramp = sg_ramp_stops(&l->shape);
    for (int i = 0; array && i < array->count; i++) {
        char step[16], id[GD_ROW_ID], label[32];
        snprintf(step, sizeof step, "a%d", i);
        leaf_id(id, pr, step);
        snprintf(label, sizeof label, "Item %d", i + 1);
        leaf item;
        if (!leaf_at(NULL, &l->value, &l->shape, step, &item)) {
            continue;
        }
        double range[2] = { 1, 0 };
        if (ramp) {
            sg_ramp_range(array, i, &range[0], &range[1]);
        }
        gd_row row = { .id = id,
                       .label = label,
                       .readonly = pr->readonly,
                       .removable = !(ramp && array->count <= 2) };
        leaf_row(r, pr, &row, &item, step, ramp ? range : NULL);
    }
}

/* One value inside a compound one, or a pin's single value; a struct inside
   one is its heading and its fields' rows. */
static void leaf_row(
    gd_rows *r,
    const pin_row *pr,
    const gd_row *d,
    const leaf *l,
    const char *path,
    const double range[2]
)
{
    const pin_spec *shape = &l->shape;
    const pin_value *value = &l->value;
    gd_row row = *d;
    yyjson_mut_val *item;
    if ((shape->flags & PIN_FLAG_COLOR) &&
        (shape->type == PIN_U32 || shape->type == PIN_I32 || shape->type == PIN_STRUCT)) {
        color_row(r, &row, shape, value);
        return;
    }
    if (shape->type == PIN_STRUCT && pin_vector_width(shape) > 0) {
        vector_row(r, &row, value, shape);
        return;
    }
    if (shape->type == PIN_STRUCT) {
        row.control = "heading";
        row.heading = true;
        gd_next_row(r, &row);
        field_rows(r, pr, l, path, range);
        return;
    }
    if (shape->type == PIN_ENUM) {
        const editor_choices *choices = editor_choices_for_enum(shape->type_name);
        bool listed = choices && editor_choices_count(choices) > 0;
        row.control = listed ? "choice" : "label";
        item = gd_next_row(r, &row);
        if (listed) {
            gd_put_choices(r, item, choices, choice_index(shape, value, choices));
        } else {
            gd_put_text(r, item, "text", "Missing enum values");
        }
        return;
    }
    if (shape->type == PIN_BOOL) {
        row.control = "bool";
        gd_put_flag(r, gd_next_row(r, &row), "checked", value->b);
        return;
    }
    if (pin_is_string(shape->type)) {
        reference_asset_kind
            asset = l->owner && l->field
                      ? reference_named_field_kind(l->owner->name, l->field->name)
                      : REFERENCE_ASSET_KIND_COUNT;
        char problem[192];
        arena scratch = { 0 };
        const char *text;
        const char *error = pin_text_cstring(&scratch, *value, &text, NULL);
        if (error) {
            row.control = "label";
            row.readonly = true;
            gd_put_text(r, gd_next_row(r, &row), "text", error);
            arena_free(&scratch);
            return;
        }
        row.control = asset != REFERENCE_ASSET_KIND_COUNT ? "asset" : "text";
        row.invalid = asset != REFERENCE_ASSET_KIND_COUNT &&
                      gd_asset_problem(pr->s, asset, text, problem);
        item = gd_next_row(r, &row);
        gd_put_text(r, item, "text", text);
        if (asset != REFERENCE_ASSET_KIND_COUNT) {
            gd_put_asset(r, item, asset, text);
        }
        arena_free(&scratch);
        return;
    }
    if (shape->type == PIN_I64 || shape->type == PIN_U64) {
        char text[32];
        integer64_text(value, text);
        row.control = "text";
        gd_put_text(r, gd_next_row(r, &row), "text", text);
        return;
    }
    if (shape->type >= PIN_I8 && shape->type <= PIN_F64) {
        gd_number_shape number = number_shape(shape->type);
        if (range && range[0] <= range[1]) {
            number.min = fmax(number.min, range[0]);
            number.max = fmin(number.max, range[1]);
        }
        row.control = "number";
        gd_put_number(r, gd_next_row(r, &row), sg_literal_number(value), &number);
        return;
    }
    row.control = "label";
    const char *text = ed_pin_type_name(shape);
    if (shape->type == PIN_HANDLE) {
        text = value->u64
                 ? runtime_entity_label(pr->s->scene, ecs_entity_from_u64(value->u64))
                 : "None";
    }
    gd_put_text(r, gd_next_row(r, &row), "text", text);
}

/* A pin's value: as the canvas edits pin values. */
static void value_row(gd_rows *r, const pin_row *pr, const gd_row *d)
{
    studio *s = pr->s;
    node *n = pr->n;
    const pin_spec *spec = pin_of(n, pr->output, pr->p);
    gd_row row = *d;
    row.readonly = pr->readonly;
    int width = pin_vector_width(spec);
    bool color = sg_value_kind_of(s, n, pr->output, pr->p) == VALUE_COLOR;
    leaf l = { .value = spec->default_value, .shape = *spec };
    if (color) {
        l.shape.flags |= PIN_FLAG_COLOR;
        color_row(r, &row, &l.shape, &l.value);
        return;
    }
    if (spec->container == PIN_CONTAINER_MAP) {
        row.control = "label";
        gd_put_text(r, gd_next_row(r, &row), "text", "Map values are not editable yet");
        return;
    }
    if (spec->container != PIN_CONTAINER_SINGLE ||
        (spec->type == PIN_STRUCT && width == 0)) {
        /* A compound value edits in a popover, as on the canvas. */
        bool array = spec->container != PIN_CONTAINER_SINGLE;
        const type_desc *type = type_catalog_find(spec->type_name);
        if (!array && !type) {
            row.control = "label";
            gd_put_text(r, gd_next_row(r, &row), "text", spec->type_name ? spec->type_name : "Struct");
            return;
        }
        row.control = array ? "array" : "struct";
        /* An array's Add item, which a full ramp disables. */
        row.removable = array && sg_array_can_add(&spec->default_value, spec);
        yyjson_mut_val *item = gd_next_row(r, &row);
        const pin_array *items = spec->default_value.ptr;
        int count = items ? items->count : 0;
        char text[64], title[128] = "";
        snprintf(text, sizeof text, "%d item%s", count, count == 1 ? "" : "s");
        if (type) {
            editor_identifier_label(type->name, title, sizeof title);
        }
        gd_put_text(r, item, "text", array ? text : "Edit");
        gd_put_text(r, item, "name", title);
        gd_rows sub = gd_sub(r, item);
        if (array) {
            item_rows(&sub, pr, &l);
        } else {
            field_rows(&sub, pr, &l, "", NULL);
        }
        gd_sub_end(&sub);
        return;
    }
    const editor_choices *choices = ed_pin_choices(sg_graph_editor(s), n, pr->output, pr->p);
    if (choices && editor_choices_count(choices) > 0) {
        row.control = "choice";
        yyjson_mut_val *item = gd_next_row(r, &row);
        gd_put_choices(r, item, choices, choice_index(spec, &spec->default_value, choices));
        return;
    }
    if (spec->type == PIN_STR && spec->type_name &&
        !strcmp(spec->type_name, "MaterialAsset")) {
        reference_asset_kind kind = REFERENCE_ASSET_MATERIAL;
        arena scratch = { 0 };
        const char *asset;
        const char *error = pin_text_cstring(&scratch, spec->default_value, &asset, NULL);
        if (error) {
            row.control = "label";
            row.readonly = true;
            gd_put_text(r, gd_next_row(r, &row), "text", error);
            arena_free(&scratch);
            return;
        }
        char problem[192];
        row.control = "asset";
        row.invalid = gd_asset_problem(s, kind, asset, problem);
        yyjson_mut_val *item = gd_next_row(r, &row);
        gd_put_text(r, item, "text", asset);
        gd_put_asset(r, item, kind, asset);
        arena_free(&scratch);
        return;
    }
    if (spec->type == PIN_BOOL || (width > 0 && !(spec->flags & PIN_FLAG_COLOR)) ||
        pin_is_string(spec->type) || (spec->type >= PIN_I8 && spec->type <= PIN_F64)) {
        leaf_row(r, pr, &row, &l, "", NULL);
        return;
    }
    row.control = "";
    gd_next_row(r, &row);
}

/* ---------- The Inputs panel ---------- */

static node *expression_editing(studio *s, const node *n, int p)
{
    studio_graph *g = &s->graph;
    if (g->input_edit_kit || !g->input_edit_pin ||
        g->input_edit_pin != n->input_pins[p].id) {
        return NULL;
    }
    return sg_node_by_id(s, g->input_edit_node);
}

/* A folded source's inputs, as its popover lists them: each part's title,
   then its inputs and editable outputs. */
static void inline_rows(gd_rows *r, studio *s, node *target, int p)
{
    graph *gr = sg_active_graph(s);
    editor *e = sg_graph_editor(s);
    node *source = NULL;
    int source_pin = -1;
    if (!graph_input_source(gr, target, p, &source, &source_pin) ||
        !graph_node_is_inline(gr, source)) {
        return;
    }
    uint64_t *ids = NULL;
    graph_inline_tree(gr, source, &ids);
    for (int i = 0; i < acount(ids); i++) {
        node *part = graph_node_by_id(gr, ids[i]);
        if (!part) {
            continue;
        }
        char title[128], id[GD_ROW_ID];
        row_id(id, 'n', part->id, 'p', 0);
        gd_row heading = { .id = id,
                           .label = ed_node_title(e, part, title, sizeof title),
                           .control = "title",
                           .heading = true };
        gd_next_row(r, &heading);
        for (int k = 0; k < part->input_count; k++) {
            if (part->input_pins[k].kind != PIN_DATA ||
                !ed_pin_visible(gr, part, ED_PIN_INPUT, k)) {
                continue;
            }
            sg_input_row(r, s, part, k);
        }
        for (int k = 0; k < part->output_count; k++) {
            if (part->output_pins[k].kind != PIN_DATA ||
                !ed_pin_editable(gr, part, ED_PIN_OUTPUT, k)) {
                continue;
            }
            char label[128];
            row_id(id, 'n', part->id, 'o', k);
            studio_code_pin_label(s, part, &part->output_pins[k], label, sizeof label);
            pin_row pr = { s, part, 1, k, id, sg_graph_readonly(s) };
            value_row(r, &pr, &(gd_row){ .id = id, .label = label });
        }
    }
    afree(ids);
}

void sg_input_row(gd_rows *r, studio *s, node *n, int p)
{
    const pin_spec *spec = &n->input_pins[p];
    char id[GD_ROW_ID], label[160], text[256];
    row_id(id, 'n', n->id, 'i', p);
    studio_code_pin_label(s, n, spec, label, sizeof label);
    gd_row d = { .id = id, .label = label, .readonly = sg_graph_readonly(s) };
    if (expression_editing(s, n, p)) {
        /* Edit Expression: the folded expression's source, in place. */
        d.control = "edit";
        yyjson_mut_val *item = gd_next_row(r, &d);
        gd_put_text(r, item, "text", s->graph.input_edit);
        gd_put_text(r, item, "placeholder", "a + b");
        return;
    }
    sg_input_kind kind = sg_input_view(s, n, p, text, sizeof text);
    if (kind == SG_INPUT_EXEC || kind == SG_INPUT_CHIP) {
        d.control = kind == SG_INPUT_EXEC ? "label" : "chip";
        gd_put_text(r, gd_next_row(r, &d), "text", kind == SG_INPUT_EXEC ? "Execution" : text);
        return;
    }
    if (kind == SG_INPUT_VALUE) {
        pin_row pr = { s, n, 0, p, id, d.readonly };
        d.bindable = sg_input_bindable(s, n, p);
        value_row(r, &pr, &d);
        return;
    }
    d.control = kind == SG_INPUT_SOURCE ? "source" : "";
    yyjson_mut_val *item = gd_next_row(r, &d);
    gd_put_text(r, item, "text", text);
    gd_put_text(r, item, "placeholder", "Value or source");
    gd_rows sub = gd_sub(r, item);
    studio_graph *g = &s->graph;
    if (kind == SG_INPUT_SOURCE && !g->inline_edit_kit && g->inline_edit_node &&
        g->inline_edit_pin == spec->id) {
        inline_rows(&sub, s, n, p);
    }
    gd_sub_end(&sub);
}

/* ---------- The panels ---------- */

static bool used(const graph *gr, const node *n, int p)
{
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *e = &gr->edges[i];
        if (e->from_id == n->id && e->from_pin_id == n->output_pins[p].id) {
            return true;
        }
    }
    return false;
}

/* The expression is the node: edit it here, see errors in place. A rejected
   draft stays until the source changes, as undo does. */
static void expression_panel(gd_view *v, studio *s, node *n, const char *source)
{
    studio_graph *g = &s->graph;
    gd_rows r = gd_panel(v, "expression", "Expression");
    if (r.open) {
        char id[GD_ROW_ID];
        row_id(id, 'n', n->id, 'x', 0);
        bool rejected = g->details.error_node == n->id &&
                        !strncmp(
                            g->details.error_source,
                            source,
                            sizeof g->details.error_source - 1
                        );
        yyjson_mut_val *item = gd_next_row(
            &r,
            &(gd_row){ .id = id, .control = "expression", .readonly = sg_graph_readonly(s) }
        );
        gd_put_text(&r, item, "text", source);
        gd_put_text(&r, item, "placeholder", "a + b");
        gd_put_text(&r, item, "error", rejected ? g->details.error : "");
    }
    gd_panel_end(&r);
}

static void outputs_panel(gd_view *v, studio *s, node *n)
{
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    gd_rows r = gd_panel(v, "outputs", "Outputs");
    for (int p = 0; r.open && p < n->output_count; p++) {
        const pin_spec *spec = &n->output_pins[p];
        char id[GD_ROW_ID], label[160];
        studio_code_pin_label(s, n, spec, label, sizeof label);
        if (ed_pin_editable(gr, n, ED_PIN_OUTPUT, p) && !readonly &&
            sg_value_kind_of(s, n, 1, p) != VALUE_NONE) {
            row_id(id, 'n', n->id, 'o', p);
            pin_row pr = { s, n, 1, p, id, readonly };
            value_row(&r, &pr, &(gd_row){ .id = id, .label = label });
        } else if (spec->kind == PIN_DATA && used(gr, n, p)) {
            /* A named value (Unreal's named reroute): uses read the name
               instead of drawing a wire; clearing it brings wires back. */
            const graph_local *local = graph_output_local(gr, n, p);
            row_id(id, 'n', n->id, 'm', p);
            yyjson_mut_val *item = gd_next_row(
                &r,
                &(gd_row){ .id = id, .label = label, .control = "text", .readonly = readonly }
            );
            gd_put_text(&r, item, "text", local ? local->name : "");
            gd_put_text(&r, item, "placeholder", ed_pin_type_name(spec));
        } else {
            row_id(id, 'n', n->id, 'o', p);
            yyjson_mut_val *item = gd_next_row(
                &r,
                &(gd_row){ .id = id, .label = label, .control = "label" }
            );
            gd_put_text(&r, item, "text", spec->kind == PIN_EXEC ? "Execution" : ed_pin_type_name(spec));
        }
    }
    gd_panel_end(&r);
}

static void check_row(
    gd_rows *r,
    node *n,
    char part,
    const char *label,
    bool checked,
    bool disabled,
    const char *check
)
{
    char id[GD_ROW_ID];
    row_id(id, 'n', n->id, part, 0);
    yyjson_mut_val *item = gd_next_row(
        r,
        &(gd_row){ .id = id, .label = label, .control = "bool", .readonly = disabled }
    );
    gd_put_flag(r, item, "checked", checked);
    gd_put_text(r, item, "check", check);
}

static void node_panel(gd_view *v, studio *s, node *n)
{
    bool readonly = sg_graph_readonly(s);
    gd_rows r = gd_panel(v, "node", "Node");
    if (r.open) {
        check_row(&r, n, 'd', "Disabled", n->disabled, readonly, "");
        if (node_behavior_for(n) != NODE_BEHAVIOR_PURE) {
            check_row(&r, n, 'b', "Breakpoint", n->breakpoint, readonly, "");
        }
        char id[GD_ROW_ID];
        row_id(id, 'n', n->id, 'k', 0);
        yyjson_mut_val *item = gd_next_row(
            &r,
            &(gd_row){ .id = id, .label = "Comment", .control = "area", .readonly = readonly }
        );
        const char *comment = n->comment ? n->comment : "";
        gd_put_text(&r, item, "text", comment);
        gd_put_text(&r, item, "placeholder", "");
        /* A text area's rows, as many as its lines, eight at most. */
        int rows = 1;
        for (const char *c = comment; *c && rows < 8; c++) {
            rows += *c == '\n';
        }
        gd_put_int(&r, item, "precision", rows);
        if (n->comment) {
            check_row(&r, n, 'h', "Show Comment", !n->comment_hidden, readonly, "");
        }
        if (ed_node_has_advanced_inputs(n)) {
            check_row(&r, n, 'a', "Advanced Inputs", n->show_advanced, readonly, "Show on node");
        }
    }
    gd_panel_end(&r);
}

/* Unreal's comment Details: the title, and a color from the full picker as
   well as the menu's palette. */
static void frame_details(gd_view *v, studio *s, graph_comment_box *box)
{
    const char *title = box->title ? box->title : "";
    bool readonly = sg_graph_readonly(s);
    gd_identity(v, KIT_ICON_COMMENT, title[0] ? title : "Comment", "Comment");
    gd_rows r = gd_panel(v, "frame", "Comment");
    if (r.open) {
        char id[GD_ROW_ID];
        row_id(id, 'c', box->id, 't', 0);
        yyjson_mut_val *item = gd_next_row(
            &r,
            &(gd_row){ .id = id, .label = "Title", .control = "text", .readonly = readonly }
        );
        gd_put_text(&r, item, "text", title);
        gd_put_text(&r, item, "placeholder", "Comment");
        row_id(id, 'c', box->id, 'c', 0);
        item = gd_next_row(
            &r,
            &(gd_row){ .id = id, .label = "Color", .control = "color", .readonly = readonly }
        );
        char hex[16];
        snprintf(hex, sizeof hex, "#%08" PRIx32, sg_frame_color(box));
        gd_put_text(&r, item, "value", hex);
        gd_put_flag(&r, item, "alpha", false);
    }
    gd_panel_end(&r);
}

static const behavior_asset *model_target(const editor *e, const node *n)
{
    for (int i = -1; i < acount(e->asset_catalog); i++) {
        const behavior_asset *asset = i < 0 ? e->asset : e->asset_catalog[i];
        if (asset->domain == BEHAVIOR_DOMAIN_MODEL && n->reference_asset &&
            !strcmp(asset->name, n->reference_asset)) {
            return asset;
        }
    }
    return NULL;
}

static void model_node_properties(studio *s, editor *e, graph *gr, node *n, bool readonly)
{
    bool parameter = n->desc == &NODE_ModelParameter;
    bool part = n->desc == &NODE_ModelPart;
    bool instance = n->desc == &NODE_ModelInstance;
    bool anchor = !strcmp(n->desc->name, "Model Anchor");
    bool material_slot = !strcmp(n->desc->name, "Model Material Slot");
    if (!parameter && !part && !instance && !material_slot && !anchor) {
        return;
    }
    kit *k = s->kit;
    unsigned flags = readonly ? KIT_READONLY : 0;
    if (kit_panel_begin(k, "model", instance ? "Model Instance" : "Definition", NULL, true)) {
        if (parameter || part || material_slot || anchor) {
            char name[128];
            snprintf(name, sizeof name, "%s", n->reference_name ? n->reference_name : "");
            kit_prop_begin(k, "name", "Name");
            if (kit_text_field(
                    k,
                    "value",
                    name,
                    sizeof name,
                    parameter       ? "Parameter"
                    : material_slot ? "Material slot"
                    : anchor        ? "Anchor"
                                    : "Part",
                    flags
                )) {
                ed_snapshot_before(e);
                graph_set_node_reference(gr, n, n->reference_id, name);
            }
            kit_prop_end(k);
        }
        if (material_slot) {
            kit_prop_begin(k, "material", "Default material");
            const char *picked = sg_material_picker(s, k, n->reference_asset, flags);
            if (picked && (!n->reference_asset || strcmp(picked, n->reference_asset))) {
                ed_snapshot_before(e);
                graph_set_node_reference_asset(gr, n, picked);
            }
            kit_prop_end(k);
        }
        if (parameter) {
            static const char *const names[] = { "Float", "Vector 2", "Vector 3", "Vector 4" };
            static const char *const types[] = { "float", "ShaderVec2", "ShaderVec3", "ShaderVec4" };
            model_parameter_decl declaration;
            shader_error error = { 0 };
            bool valid = model_parameter_get(n, &declaration, &error);
            int width = valid ? declaration.width : 1;
            kit_prop_begin(k, "type", "Type");
            int selected = width - 1;
            kit_choice(k, "value", &selected, names, 4, flags);
            if (selected >= 0 && selected < 4 && selected + 1 != width) {
                ed_snapshot_before(e);
                pin_type type = selected ? PIN_STRUCT : PIN_F32;
                graph_set_pin_type(gr, n, false, 0, type, types[selected]);
                const float defaults[] = { 1, .001f, 1000 };
                for (int p = 0; p < 3; p++) {
                    const float *old = !valid ? NULL
                                     : p == 0 ? declaration.defaults
                                     : p == 1 ? declaration.minimum
                                              : declaration.maximum;
                    float values[4];
                    for (int i = 0; i < selected + 1; i++) {
                        values[i] = old ? old[i < width ? i : 0] : defaults[p];
                    }
                    pin_value value = { .type = type, .type_name = types[selected] };
                    if (selected) {
                        value.ptr = values;
                    } else {
                        value.f = values[0];
                    }
                    graph_set_pin_default(gr, n, false, p, &value);
                }
            }
            kit_prop_end(k);
        }
        if (instance) {
            kit_prop_begin(k, "reference", "Model");
            kit_command_item *items = NULL;
            const behavior_asset **models = NULL;
            for (int i = 0; i < acount(e->asset_catalog); i++) {
                const behavior_asset *asset = e->asset_catalog[i];
                if (asset->domain != BEHAVIOR_DOMAIN_MODEL ||
                    !strcmp(asset->name, e->asset->name)) {
                    continue;
                }
                apush(models, asset);
                apush(
                    items,
                    ((kit_command_item){
                        .label = asset->name,
                        .icon = KIT_ICON_HEXAGON,
                        .current = n->reference_asset &&
                                   !strcmp(n->reference_asset, asset->name) })
                );
            }
            kit_autocomplete_result result = kit_autocomplete(
                k,
                "asset",
                n->reference_asset,
                items,
                acount(items),
                "Choose model",
                flags
            );
            int picked = result.selected;
            if (picked >= 0 && picked < acount(models) &&
                (!n->reference_asset ||
                 strcmp(n->reference_asset, models[picked]->name))) {
                ed_snapshot_before(e);
                while (n->output_count > 1) {
                    graph_remove_pin(gr, n, true, n->output_count - 1);
                }
                while (n->input_count) {
                    graph_remove_pin(gr, n, false, n->input_count - 1);
                }
                graph_set_node_reference_asset(gr, n, models[picked]->name);
            }
            afree(items);
            afree(models);
            kit_prop_end(k);
            const behavior_asset *target = model_target(e, n);
            for (int i = 0; target && i < acount(target->graph->nodes); i++) {
                const node *anchor = target->graph->nodes[i];
                if (strcmp(anchor->desc->name, "Model Anchor") ||
                    !anchor->reference_name) {
                    continue;
                }
                int output = -1;
                for (int p = 1; p < n->output_count; p++) {
                    if (n->output_pins[p].declaration_id == anchor->id) {
                        output = p;
                    }
                }
                char key[32];
                snprintf(
                    key,
                    sizeof key,
                    "anchor-%llu",
                    (unsigned long long)anchor->id
                );
                bool exposed = output >= 0;
                kit_prop_begin(k, key, anchor->reference_name);
                if (kit_checkbox(k, "expose", &exposed, "Expose anchor", flags)) {
                    ed_snapshot_before(e);
                    if (exposed) {
                        shader_error error = { 0 };
                        if (model_instance_anchor(gr, n, target, anchor->reference_name, &error) <
                            0) {
                            sg_show_error(s, "Can't expose the anchor", error.message);
                        }
                    } else {
                        graph_remove_pin(gr, n, true, output);
                    }
                }
                kit_prop_end(k);
            }
            for (int g = 0; target && g < acount(target->graphs); g++) {
                for (int i = 0; i < acount(target->graphs[g]->nodes); i++) {
                    const node *declaration = target->graphs[g]->nodes[i];
                    bool material = !strcmp(declaration->desc->name, "Model Material Slot");
                    if (declaration->desc != &NODE_ModelParameter && !material) {
                        continue;
                    }
                    model_parameter_decl parameter = { .id = declaration->id,
                                                       .name = declaration->reference_name };
                    shader_error error = { 0 };
                    if ((material && (!parameter.name || !parameter.name[0])) ||
                        (!material &&
                         !model_parameter_get(declaration, &parameter, &error))) {
                        continue;
                    }
                    int pin = -1;
                    for (int p = 0; p < n->input_count; p++) {
                        if (n->input_pins[p].declaration_id == parameter.id) {
                            pin = p;
                        }
                    }
                    bool override = pin >= 0;
                    char key[32];
                    snprintf(
                        key,
                        sizeof key,
                        "parameter-%llu",
                        (unsigned long long)parameter.id
                    );
                    kit_prop_begin(k, key, parameter.name);
                    if (kit_checkbox(k, "override", &override, "Override", readonly ? KIT_DISABLED : 0)) {
                        ed_snapshot_before(e);
                        if (override) {
                            if (model_instance_override(gr, n, target, parameter.name, &error) <
                                0) {
                                sg_show_error(
                                    s,
                                    "Can't override the parameter",
                                    error.message
                                );
                            }
                        } else {
                            graph_remove_pin(gr, n, false, pin);
                        }
                    }
                    kit_prop_end(k);
                }
            }
        }
    }
    kit_panel_end(k);
}

/* Model graphs retain their authored model controls alongside the RML graph
   details: model declarations, instances and preview tools remain reachable
   while the shared graph inspector uses its migrated rows. */
void studio_graph_model_controls(studio *s)
{
    if (!studio_graph_active(s)) {
        return;
    }
    editor *e = sg_graph_editor(s);
    if (!e || !e->asset || e->asset->domain != BEHAVIOR_DOMAIN_MODEL) {
        return;
    }
    graph *gr = sg_active_graph(s);
    node *n = sg_node_by_id(s, studio_graph_node(s));
    if (n) {
        model_node_properties(s, e, gr, n, sg_graph_readonly(s));
    }
}

void studio_node_details(studio *s, gd_view *v)
{
    editor *e = sg_graph_editor(s);
    node *n = sg_node_by_id(s, studio_graph_node(s));
    graph_comment_box *box = sg_comment_by_id(s, studio_graph_comment(s));
    if (box) {
        frame_details(v, s, box);
        return;
    }
    if (!n) {
        gd_empty(v, "No node selected");
        return;
    }
    char title[256];
    char *source = n->desc == &NODE_BehaviorExpression
                     ? studio_code_expression(s, n)
                     : NULL;
    gd_identity(
        v,
        sg_node_icon(s, n) ? sg_node_icon(s, n) : KIT_ICON_HEXAGON,
        source ? source : ed_node_title(e, n, title, sizeof title),
        n->desc->category ? n->desc->category : n->desc->name
    );
    if (source) {
        expression_panel(v, s, n, source);
    }
    free(source);
    if (n->input_count) {
        gd_rows r = gd_panel(v, "inputs", "Inputs");
        for (int p = 0; r.open && p < n->input_count; p++) {
            const pin_spec *spec = &n->input_pins[p];
            if (!(spec->flags & PIN_FLAG_SETTING) || spec->kind == PIN_DATA) {
                sg_input_row(&r, s, n, p);
            }
        }
        gd_panel_end(&r);
    }
    if (n->output_count) {
        outputs_panel(v, s, n);
    }
    node_panel(v, s, n);
}

/* ---------- Capabilities ----------
 * What the tab's edits do to the graph, over nodes, pins and values: no
 * row ids, events or JSON. Each is one undo step; false: refused. Literals
 * are sg_set_literal's, expressions sg_set_expression_source's and
 * comments sg_set_node_comment's. */

/* Names output `p`, renames its value or, with an empty name, unnames it. */
static bool name_value(editor *e, graph *gr, node *n, int p, const char *name)
{
    if (p < 0 || p >= n->output_count) {
        ed_show_error(e, "Value output no longer exists");
        return false;
    }
    const authoring_source_presentation *view = editor_presentation(e);
    if (view) {
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            view,
            gr->graph_id,
            n->id,
            n->output_pins[p].id
        );
        if (!origin) {
            ed_show_error(e, "Value has no source declaration");
            return false;
        }
        return ed_source_edit(
            e,
            &(authoring_edit_request){ .kind = AUTHORING_EDIT_NAME_VALUE,
                                       .generation = authoring_source_edit_generation(view),
                                       .origin = origin->identity,
                                       .producer_result = origin->result_index,
                                       .name = name }
        );
    }
    const graph_local *local = graph_output_local(gr, n, p);
    if ((!local && !name[0]) || (local && !strcmp(local->name, name))) {
        return true;
    }
    ed_snapshot_before(e);
    return !name[0] ? graph_expand_local(gr, local->id) == 0
         : local    ? graph_rename_local(gr, local->id, name) == 0
                    : graph_create_local(gr, n, p, name) != 0;
}

typedef enum
{
    FLAG_DISABLED,
    FLAG_BREAKPOINT,
    FLAG_COMMENT_SHOWN,
    FLAG_ADVANCED_SHOWN,
} node_flag;

bool sg_set_node_disabled(studio *s, node *n, bool disabled)
{
    if (n->disabled == disabled) {
        return true;
    }
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    const authoring_source_presentation *view = editor_presentation(e);
    if (view) {
        const source_presentation_origin *
            origin = authoring_source_presentation_origin(view, gr->graph_id, n->id, 0);
        if (!origin) {
            sg_show_error(s, disabled ? "Can't disable the node" : "Can't enable the node", "Node has no source declaration");
            return false;
        }
        bool changed = ed_source_edit(
            e,
            &(authoring_edit_request){ .kind = AUTHORING_EDIT_SET_DISABLED,
                                       .generation = authoring_source_edit_generation(view),
                                       .origin = origin->identity,
                                       .disabled = disabled }
        );
        if (!changed) {
            sg_show_error(s, disabled ? "Can't disable the node" : "Can't enable the node", e->connect_error);
            s->graph.details.redraw = true;
        }
        return changed;
    }
    ed_snapshot_before(e);
    graph_set_node_disabled(gr, n, disabled);
    return true;
}

static bool flag_of(const node *n, node_flag flag)
{
    switch (flag) {
        case FLAG_DISABLED: return n->disabled;
        case FLAG_BREAKPOINT: return n->breakpoint;
        case FLAG_COMMENT_SHOWN: return !n->comment_hidden;
        case FLAG_ADVANCED_SHOWN: return n->show_advanced;
    }
    return false;
}

/* A state the node holds already changes nothing. */
static void set_flag(studio *s, node *n, node_flag flag, bool on)
{
    graph *gr = sg_active_graph(s);
    if (flag_of(n, flag) == on) {
        return;
    }
    if (flag == FLAG_DISABLED) {
        sg_set_node_disabled(s, n, on);
        return;
    }
    ed_snapshot_before(sg_graph_editor(s));
    switch (flag) {
        case FLAG_DISABLED: break;
        case FLAG_BREAKPOINT: graph_set_breakpoint(gr, n, on); break;
        case FLAG_COMMENT_SHOWN:
            sg_set_node_comment(s, n, n->comment ? n->comment : "", !on);
            break;
        case FLAG_ADVANCED_SHOWN: n->show_advanced = on; break;
    }
}

static void set_frame_title(editor *e, graph *gr, graph_comment_box *box, const char *title)
{
    if (!strcmp(box->title ? box->title : "", title)) {
        return;
    }
    ed_snapshot_before(e);
    graph_set_comment_box(gr, box, title, box->color);
}

/* A color gesture (live, then its commit) is one undo step. */
static void set_frame_color(studio *s, graph_comment_box *box, uint32_t color, sg_step step)
{
    studio_graph *g = &s->graph;
    if (step == SG_STEP_ONCE || !g->details.gesture) {
        ed_snapshot_before(sg_graph_editor(s));
    }
    graph_set_comment_box(sg_active_graph(s), box, box->title ? box->title : "", color);
    g->details.gesture = step == SG_STEP_LIVE;
}

static bool listed(char **names, const char *name)
{
    for (int i = 0; i < acount(names); i++) {
        if (!strcmp(names[i], name)) {
            return true;
        }
    }
    return false;
}

/* The one input an edit added that nothing feeds, or NULL. */
static const char *added_input(const graph *gr, const node *n, char **before)
{
    char **fed = behavior_expression_fed_inputs(gr, n);
    const char *added = NULL;
    int count = 0;
    for (int p = 0; p < n->input_count; p++) {
        const char *name = n->input_pins[p].name;
        if (n->input_pins[p].kind == PIN_DATA && !listed(before, name) &&
            !listed(fed, name)) {
            added = name;
            count++;
        }
    }
    behavior_expression_free_names(fed);
    return count == 1 ? added : NULL;
}

/* Inputs match by name, so a rename drops its wire, as in Unreal's Math
   Expression; the toast keeps it, or undoes an edit that dropped several. */
static void offer_rewire(studio *s, const node *n, char **before, char **lost, const char *source)
{
    studio_graph *g = &s->graph;
    const char *to = acount(lost) == 1 ? added_input(sg_active_graph(s), n, before)
                                       : NULL;
    char title[160];
    snprintf(title, sizeof title, "Unwired %s%s", lost[0], acount(lost) > 1 ? " and more" : "");
    snprintf(g->rewire.from, sizeof g->rewire.from, "%s", to ? lost[0] : "");
    snprintf(g->rewire.to, sizeof g->rewire.to, "%s", to ? to : "");
    snprintf(g->rewire.source, sizeof g->rewire.source, "%s", source);
    g->rewire.node = n->id;
    g->rewire.history = acount(sg_graph_editor(s)->undo);
    kit_toast_dismiss(s->kit, g->rewire.toast);
    g->rewire.toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                           .title = title,
                           .action = to ? "Keep Wire" : "Undo",
                           .duration = 6 }
    );
}

static char **input_names(const node *n)
{
    char **names = NULL;
    for (int p = 0; p < n->input_count; p++) {
        apush(names, xstrdup(n->input_pins[p].name));
    }
    return names;
}

int sg_set_expression_source(studio *s, node *n, const char *source)
{
    studio_graph *g = &s->graph;
    if (editor_presentation(sg_graph_editor(s))) {
        return studio_code_expression_replace(
                   s,
                   n,
                   source,
                   g->expression_error,
                   sizeof g->expression_error
               )
                 ? 0
                 : -1;
    }
    graph *gr = sg_active_graph(s);
    uint64_t id = n->id;
    char **before = input_names(n), **lost = behavior_expression_fed_inputs(gr, n);
    behavior_expression_error error = { 0 };
    char *resolved = studio_code_expression_resolve(
        s,
        n,
        source,
        error.message,
        sizeof error.message
    );
    int status = resolved
                   ? editor_set_behavior_expression(sg_graph_editor(s), n, resolved, &error)
                   : -1;
    free(resolved);
    if (status != 0) {
        snprintf(
            g->expression_error,
            sizeof g->expression_error,
            "%s",
            error.message[0] ? error.message : "Invalid expression"
        );
    } else {
        g->expression_error[0] = '\0';
        gr = sg_active_graph(s);
        n = graph_node_by_id(gr, id);
        assert(n);
        behavior_expression_keep_unfed(gr, n, &lost);
        if (acount(lost)) {
            offer_rewire(s, n, before, lost, source);
        }
    }
    behavior_expression_free_names(before);
    behavior_expression_free_names(lost);
    return status != 0 ? -1 : 0;
}

void studio_rewire_toast(studio *s)
{
    studio_graph *g = &s->graph;
    if (!g->rewire.toast) {
        return;
    }
    editor *e = s->scene->graph_editor;
    bool current = e && acount(e->undo) == g->rewire.history;
    if (!kit_toast_action(s->kit, g->rewire.toast)) {
        if (!current) {
            kit_toast_dismiss(s->kit, g->rewire.toast);
            g->rewire.toast = 0;
        }
        return;
    }
    g->rewire.toast = 0;
    if (!current) {
        return;
    }
    editor_undo(e);
    node *n = sg_node_by_id(s, g->rewire.node);
    if (!n) {
        return;
    }
    sg_select_only(e, n);
    behavior_expression_error error = { 0 };
    if (g->rewire.from[0] && editor_rewire_behavior_expression(
                                 e,
                                 n,
                                 g->rewire.source,
                                 g->rewire.from,
                                 g->rewire.to,
                                 &error
                             )) {
        kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = "Could not keep the wire" });
    }
}

/* ---------- Intents ----------
 * The rows' events read as the capabilities above. */

/* A refused edit's field shows the graph's text again. */
static void refused(studio *s, const char *title)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title });
    s->graph.details.redraw = true;
}

/* What a control's event carries: its row id, its value as text, whether
   it was canceled and the part of a control with several (a vector's axis
   key, or alpha). */
static const char *value_of(const ui_activation *a)
{
    return a->argc > 1 ? a->args[1] : "";
}

static const char *part_of(const ui_activation *a)
{
    return a->argc > 3 ? a->args[3] : "";
}

/* The step a control's event takes: input runs a gesture, change ends one;
   a checkbox's or a choice's is one of its own. */
static sg_step step_of(const char *action)
{
    return !strcmp(action, "input")  ? SG_STEP_LIVE
         : !strcmp(action, "change") ? SG_STEP_COMMIT
                                     : SG_STEP_ONCE;
}

/* The choices a row offers, or NULL: a pin's catalog, or an enum's. */
static const editor_choices *choices_of(studio *s, node *n, int output, int p, const char *path, const leaf *l)
{
    const editor_choices *choices = path[0]
                                      ? NULL
                                      : ed_pin_choices(sg_graph_editor(s), n, output, p);
    if (choices && editor_choices_count(choices) > 0) {
        return choices;
    }
    return l->shape.type == PIN_ENUM ? editor_choices_for_enum(l->shape.type_name)
                                     : NULL;
}

/* The leaf's value with what the control sent; false when it reads as
   none. */
static bool edited_leaf(
    const leaf *l,
    const editor_choices *choices,
    const ui_activation *a,
    arena *scratch,
    pin_value *out
)
{
    const char *text = value_of(a);
    *out = l->value;
    if (choices) {
        int index = atoi(text);
        if (index < 0 || index >= editor_choices_count(choices)) {
            return false;
        }
        editor_choice choice = editor_choice_at(choices, index);
        if (l->shape.type == PIN_NAME) {
            out->str = choice.name;
        } else if (l->shape.type == PIN_ENUM) {
            out->e = choice.value;
        } else {
            sg_set_literal_number(out, choice.value);
        }
        return true;
    }
    if (l->shape.flags & PIN_FLAG_COLOR) {
        if (!strcmp(part_of(a), "alpha")) {
            sg_write_literal_alpha(&l->shape, out, scratch, strtod(text, NULL));
            return true;
        }
        return sg_write_literal_color(
            &l->shape,
            out,
            scratch,
            (uint32_t)strtoul(text + (text[0] == '#'), NULL, 16)
        );
    }
    if (l->shape.type == PIN_STRUCT && pin_vector_width(&l->shape) > 0) {
        /* An axis of a vector, which its key names. */
        const type_desc *type = type_catalog_find(l->shape.type_name);
        const char *key = strchr("xyzw", part_of(a)[0]);
        int axis = key && part_of(a)[0] ? (int)(key - "xyzw") : -1;
        if (!type || axis < 0 || axis >= type->field_count) {
            return false;
        }
        if (!out->ptr) {
            out->ptr = arena_alloc(scratch, type->size);
            memset(out->ptr, 0, type->size);
            out->type_name = type->name;
        }
        pin_value component;
        if (pin_read_field(scratch, out->ptr, &type->fields[axis], &component)) {
            return false;
        }
        sg_set_literal_number(&component, strtod(text, NULL));
        return !pin_write_field(scratch, out->ptr, &type->fields[axis], component);
    }
    if (l->shape.type == PIN_BOOL) {
        out->b = text[0] && strcmp(text, "0") && strcmp(text, "false");
        return true;
    }
    if (l->shape.type == PIN_STR) {
        if (pin_value_text(scratch, out, text, strlen(text))) {
            return false;
        }
        out->type_name = l->value.type_name;
        return true;
    }
    if (l->shape.type == PIN_NAME) {
        out->str = arena_text(scratch, text);
        return true;
    }
    if (l->shape.type == PIN_I64 || l->shape.type == PIN_U64) {
        return parse_integer64(out, text);
    }
    char *end;
    double number = strtod(text, &end);
    if (end == text) {
        return false;
    }
    sg_set_literal_number(out, number);
    return true;
}

/* An array's Add item, or an item's remove button, as one step. */
static bool edit_items(const target *t, const char *action, arena *scratch, pin_value *value, const pin_spec *shape)
{
    char parent[GD_ROW_ID] = "";
    int item = -1;
    if (!strcmp(action, "remove")) {
        const char *slash = strrchr(t->path, '/');
        const char *step = slash ? slash + 1 : t->path;
        snprintf(parent, sizeof parent, "%.*s", slash ? (int)(slash - t->path) : 0, t->path);
        item = atoi(step + 1);
    }
    leaf l;
    if (!leaf_at(scratch, value, shape, parent, &l)) {
        return false;
    }
    pin_value edited = l.value;
    edited.ptr = sg_array_edit(scratch, &l.value, &l.shape, item, item < 0);
    return edited.ptr && write_at(scratch, value, shape, parent, &edited);
}

/* The default in `value` with the control's edit written in; false when it
   reads as none or changes nothing. `step` becomes one of its own for a
   discrete edit. */
static bool edited_value(
    studio *s,
    node *n,
    const target *t,
    const char *action,
    const ui_activation *a,
    arena *scratch,
    pin_value *value,
    const pin_spec *shape,
    sg_step *step
)
{
    if (!strcmp(action, "add") || !strcmp(action, "remove")) {
        *step = SG_STEP_ONCE;
        return edit_items(t, action, scratch, value, shape);
    }
    leaf l;
    if (!leaf_at(scratch, value, shape, t->path, &l)) {
        return false;
    }
    int output = t->part == 'o';
    const editor_choices *choices = choices_of(s, n, output, t->index, t->path, &l);
    if (choices || pin_is_string(l.shape.type) || l.shape.type == PIN_I64 ||
        l.shape.type == PIN_U64) {
        *step = SG_STEP_ONCE;
    }
    pin_value with;
    if (!edited_leaf(&l, choices, a, scratch, &with)) {
        return false;
    }
    /* RmlUi sends a checkbox's change as a binding sets it too. */
    if (l.shape.type == PIN_BOOL && !choices && with.b == l.value.b) {
        return false;
    }
    return write_at(scratch, value, shape, t->path, &with);
}

/* An edit of pin `t`'s default, or of a place inside it: the control's
   gesture is one Undo step, a discrete edit one of its own. A typed text
   the pin refuses shows the pin's again. */
static void edit_value(studio *s, node *n, const target *t, const char *action, const ui_activation *a)
{
    int output = t->part == 'o', p = t->index;
    if (p < 0 || p >= (output ? n->output_count : n->input_count)) {
        return;
    }
    const pin_spec *spec = pin_of(n, output, p);
    arena scratch = { 0 };
    pin_value value;
    if (pin_value_snapshot_to_arena(&scratch, &value, &spec->default_value)) {
        arena_free(&scratch);
        return;
    }
    pin_spec shape = *spec;
    if (sg_value_kind_of(s, n, output, p) == VALUE_COLOR) {
        shape.flags |= PIN_FLAG_COLOR;
    }
    sg_step step = step_of(action);
    if (!edited_value(s, n, t, action, a, &scratch, &value, &shape, &step) ||
        !sg_set_literal(s, n, output, p, &value, step)) {
        s->graph.details.redraw = true;
    }
    arena_free(&scratch);
}

/* A Node panel checkbox's state, which RmlUi also sends as a binding sets
   it (set_flag ignores a state the node holds). */
static void check_flag(studio *s, node *n, char part, bool on)
{
    switch (part) {
        case 'd': set_flag(s, n, FLAG_DISABLED, on); break;
        case 'b': set_flag(s, n, FLAG_BREAKPOINT, on); break;
        case 'h': set_flag(s, n, FLAG_COMMENT_SHOWN, on); break;
        case 'a': set_flag(s, n, FLAG_ADVANCED_SHOWN, on); break;
        default: break;
    }
}

/* A folded expression's source as Edit Expression edits it in place. */
static void edit_inline_expression(studio *s, const ui_activation *a)
{
    studio_graph *g = &s->graph;
    node *expression = sg_node_by_id(s, g->input_edit_node);
    if (!expression || a->argc < 2) {
        return;
    }
    if (sg_set_expression_source(s, expression, a->args[1]) == 0) {
        g->input_edit_pin = g->input_edit_node = 0;
    } else if (g->expression_error[0]) {
        sg_show_error(s, "Can't use this expression", g->expression_error);
    }
}

/* Bind to Source: the row shows its source field, which the details frame
   focuses, opening its list (studio_graph_details.c). */
static void bind_input(studio *s, node *n, int p, const char *row)
{
    studio_graph *g = &s->graph;
    g->bind_pin = n->input_pins[p].id;
    g->bind_kit = NULL;
    g->bind_focus = true;
    g->details.bind_wait = 0;
    snprintf(g->details.bind, sizeof g->details.bind, "%s", row);
}

static void edit_expression(studio *s, node *n, const ui_activation *a)
{
    studio_graph *g = &s->graph;
    const char *source = a->argc > 1 ? a->args[1] : "";
    if (sg_set_expression_source(s, n, source) == 0) {
        g->details.error_node = 0;
        return;
    }
    g->details.error_node = n->id;
    snprintf(g->details.error, sizeof g->details.error, "%s", g->expression_error);
    char *was = studio_code_expression(s, n);
    snprintf(g->details.error_source, sizeof g->details.error_source, "%s", was ? was : "");
    free(was);
}

static void frame_edit(studio *s, graph_comment_box *box, char part, const char *action, const ui_activation *a)
{
    if (a->argc < 2) {
        return;
    }
    if (part == 't' && !strcmp(action, "change")) {
        set_frame_title(sg_graph_editor(s), sg_active_graph(s), box, a->args[1]);
        return;
    }
    if (part != 'c' || (strcmp(action, "input") && strcmp(action, "change"))) {
        return;
    }
    uint32_t color = (uint32_t)strtoul(a->args[1] + (a->args[1][0] == '#'), NULL, 16);
    bool canceled = a->argc > 2 && !strcmp(a->args[2], "1");
    set_frame_color(s, box, canceled ? box->color : color, step_of(action));
}

void studio_node_details_action(studio *s, const char *action, const ui_activation *a)
{
    target t;
    if (!a->argc || !target_of(a->args[0], &t) || sg_graph_readonly(s)) {
        return;
    }
    if (t.owner == 'c') {
        graph_comment_box *box = sg_comment_by_id(s, t.id);
        if (box) {
            frame_edit(s, box, t.part, action, a);
        }
        return;
    }
    node *n = sg_node_by_id(s, t.id);
    if (!n) {
        return;
    }
    bool value = !strcmp(action, "input") || !strcmp(action, "change") ||
                 !strcmp(action, "check") || !strcmp(action, "add") ||
                 !strcmp(action, "remove");
    switch (t.part) {
        case 'i':
            if (!strcmp(action, "change") && expression_editing(s, n, t.index)) {
                edit_inline_expression(s, a);
            } else if (!strcmp(action, "source")) {
                sg_source_open(s, n, t.index, a->args[0], a->anchor ? rml_element_ref_get(a->anchor) : NULL);
            } else if (!strcmp(action, "bind")) {
                bind_input(s, n, t.index, a->args[0]);
            } else if (!strcmp(action, "cancel")) {
                s->graph.input_edit_pin = s->graph.input_edit_node = 0;
            } else if (value) {
                edit_value(s, n, &t, action, a);
            }
            return;
        case 'o':
            if (value) {
                edit_value(s, n, &t, action, a);
            }
            return;
        case 'm':
            if (!strcmp(action, "change") && a->argc > 1) {
                graph *gr = sg_active_graph(s);
                bool named = graph_output_local(gr, n, t.index) != NULL;
                if (!name_value(sg_graph_editor(s), gr, n, t.index, a->args[1])) {
                    const char *error = sg_graph_editor(s)->connect_error;
                    refused(
                        s,
                        error[0] ? error
                        : named  ? "A value has that name"
                                 : "Can't name this value"
                    );
                }
            }
            return;
        case 'x':
            if (!strcmp(action, "change")) {
                edit_expression(s, n, a);
            }
            return;
        case 'k':
            if (!strcmp(action, "change") && a->argc > 1) {
                sg_set_node_comment(s, n, a->args[1], n->comment_hidden);
            }
            return;
        default:
            if (!strcmp(action, "check")) {
                check_flag(s, n, t.part, a->argc > 1 && a->args[1][0]);
            }
            return;
    }
}
