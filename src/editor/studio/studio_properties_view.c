#include "editor/documents/prototype_source_input.h"
/* Properties' view: the `properties` variable Properties.rml and
 * PropertiesHeader.rml bind (docs/studio-rml-migration.md, Data):
 *
 *   propertyValues: [text]
 *   properties: { query, live, readonly, keepTip, view, note,
 *     tabs: [{ id, icon, tip, on, sep }],
 *     identity: { kind, icon, name, of, prototype, kbd, why, locked, placed,
 *                 actions, chips: [{ label, icon, name }] },
 *     panels: [{ key, title, badge, open, menu, rows: [ROW] }],
 *     behaviors: { shown, open, locked, why, rows: [{ name, open }],
 *                  assets: [MENU ITEM] },
 *     add: { shown, locked, why },
 *     fieldMenu: { reset, apply }, componentMenu: { title, label, icon,
 *                  about, disabled } }
 *   ROW: { id, label, tip, about, control, slot, value, min, max, step,
 *          precision, unit, text, drop, problem, overridden, mixed, readonly,
 *          invalid, integer, float, slider, checked, preview, axes: [AXIS],
 *          table: { label, icon, none, groups } }
 *   AXIS: { key, label, slot, min, max, step, precision, unit, slider, mixed,
 *           overridden }
 *
 * `control` names the row's one control (number, vector, bool, choice,
 * color, text, asset or label), which Properties.rml builds with data-when.
 * Each refresh writes values in place and only where they differ, so a Play
 * refresh that moves values repaints number fields without laying out; a
 * number's text lives in room its member owns, so rewriting it allocates
 * nothing. A number row's or axis's value is propertyValues[slot]: RmlUi
 * dirties by top-level variable, so a refresh that moves only numbers
 * evaluates only the views that show them. A choice row lists only its current option until it is opened
 * (properties.choices), so closed lists build no elements. */
#include "editor/collab/actions_scene.h"
#include "editor/documents/component_properties.h"
#include "editor/studio/studio_properties_internal.h"
#include "editor/ui/labels.h"
#include "graph/model/registry.h"
#include "runtime/component_catalog.h"
#include "support/arena.h"
#include "support/array.h"
#include "types/text.h"
#include "types/value_format.h"
#include "ui/drag.h"
#include "ui/popover.h"

#include <assert.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bytes a number's text member owns; a refresh rewrites it in place. */
#define ROOM 32

typedef struct view
{
    studio *s;
    yyjson_mut_doc *doc;
    yyjson_mut_val *root;
    yyjson_mut_val *values; /* propertyValues */
    yyjson_mut_val **shown; /* the panel slots written this refresh (array) */
    int slots;              /* the values written this refresh */
    bool changed;           /* properties changed */
    bool values_changed;
    arena storage;
} view;

/* ---------- Members, written in place ---------- */

static yyjson_mut_val *member(yyjson_mut_val *obj, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    assert(v);
    return v;
}

static void put_text(view *v, yyjson_mut_val *obj, const char *key, const char *text)
{
    v->changed |= ui_data_put_text(v->doc, obj, key, text);
}

static void put_flag(view *v, yyjson_mut_val *obj, const char *key, bool on)
{
    v->changed |= ui_data_put_bool(obj, key, on);
}

static void put_int(view *v, yyjson_mut_val *obj, const char *key, int n)
{
    v->changed |= ui_data_put_int(obj, key, n);
}

/* Rewrites the room `m` owns with `text`; true when it differed. Numbers
   are text, not ui_data_put_number's doubles, which RmlUi writes into an
   attribute to three decimals. */
static bool set_room(yyjson_mut_val *m, const char *text)
{
    char *room = (char *)yyjson_mut_get_str(m);
    size_t length = strlen(text);
    assert(length < ROOM);
    if (!strcmp(room, text)) {
        return false;
    }
    memcpy(room, text, length + 1);
    yyjson_mut_set_strn(m, room, length);
    return true;
}

static void put_room(view *v, yyjson_mut_val *obj, const char *key, const char *text)
{
    v->changed |= set_room(member(obj, key), text);
}

/* A number as text a control reads back exactly: RmlUi writes a double into
   an attribute to three decimals. */
static void number_text(char text[ROOM], double number, bool single)
{
    if (isinf(number)) {
        snprintf(text, ROOM, "%s", number < 0 ? "-inf" : "inf");
        return;
    }
    snprintf(text, ROOM, single ? "%.9g" : "%.17g", number);
}

static void put_number(view *v, yyjson_mut_val *obj, const char *key, double number, bool single)
{
    char text[ROOM];
    number_text(text, number, single);
    put_room(v, obj, key, text);
}

static yyjson_mut_val *room_new(yyjson_mut_doc *doc)
{
    static const char BLANK[ROOM];
    yyjson_mut_val *room = yyjson_mut_strncpy(doc, BLANK, ROOM - 1);
    yyjson_mut_set_strn(room, yyjson_mut_get_str(room), 0);
    return room;
}

static void add_room(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key)
{
    yyjson_mut_obj_add_val(doc, obj, key, room_new(doc));
}

/* A number the game moves while it plays: its text in the next slot of
   `propertyValues`, which `obj` names, so a refresh that moves only values
   dirties that variable alone and evaluates only the views that show them. */
static void put_value(view *v, yyjson_mut_val *obj, double number, bool single)
{
    int slot = v->slots++;
    put_int(v, obj, "slot", slot);
    if ((size_t)slot == yyjson_mut_arr_size(v->values)) {
        yyjson_mut_arr_append(v->values, room_new(v->doc));
        v->values_changed = true;
    }
    char text[ROOM];
    number_text(text, number, single);
    v->values_changed |= set_room(yyjson_mut_arr_get(v->values, (size_t)slot), text);
}

static void add_texts(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_obj_add_str(doc, obj, keys[i], "");
    }
}

static void add_flags(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_obj_add_bool(doc, obj, keys[i], false);
    }
}

#define ADD_TEXTS(doc, obj, keys)                                              \
    add_texts(doc, obj, keys, sizeof keys / sizeof *keys)
#define ADD_FLAGS(doc, obj, keys)                                              \
    add_flags(doc, obj, keys, sizeof keys / sizeof *keys)

static void trim(view *v, yyjson_mut_val *list, size_t count)
{
    while (yyjson_mut_arr_size(list) > count) {
        yyjson_mut_arr_remove_last(list);
        v->changed = true;
    }
}

/* Hides the items past `count` and keeps them, with their elements, for
   the next selection to show again: instancing a row costs far more than
   binding one (docs/studio-rml-migration.md, S0 results). */
static void hide_rest(view *v, yyjson_mut_val *list, size_t count)
{
    for (size_t i = count; i < yyjson_mut_arr_size(list); i++) {
        put_flag(v, yyjson_mut_arr_get(list, i), "shown", false);
    }
}

/* The kinds of list item. */
typedef enum
{
    ITEM_ROW,
    ITEM_AXIS,
    ITEM_TAB,
    ITEM_CHIP,
    ITEM_BEHAVIOR,
    ITEM_MENU,
} item_kind;

static void put_glyph(view *v, yyjson_mut_val *obj, const char *key, uint32_t icon)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    put_text(v, obj, key, glyph);
}

/* ---------- Shapes ---------- */

static yyjson_mut_val *table_new(yyjson_mut_doc *doc, yyjson_mut_val *obj)
{
    static const char *const TEXTS[] = { "label", "icon", "none", "key" };
    yyjson_mut_val *table = yyjson_mut_obj_add_obj(doc, obj, "table");
    ADD_TEXTS(doc, table, TEXTS);
    yyjson_mut_obj_add_arr(doc, table, "groups");
    return table;
}

static yyjson_mut_val *axis_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "key", "label", "unit" };
    static const char *const ROOMS[] = { "min", "max", "step" };
    static const char *const FLAGS[] = { "slider", "mixed", "overridden" };
    yyjson_mut_val *axis = yyjson_mut_obj(doc);
    ADD_TEXTS(doc, axis, TEXTS);
    for (size_t i = 0; i < sizeof ROOMS / sizeof *ROOMS; i++) {
        add_room(doc, axis, ROOMS[i]);
    }
    ADD_FLAGS(doc, axis, FLAGS);
    yyjson_mut_obj_add_int(doc, axis, "precision", 0);
    yyjson_mut_obj_add_int(doc, axis, "slot", 0);
    return axis;
}

static yyjson_mut_val *row_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "id",      "label", "tip",  "about",
                                         "control", "unit",  "text", "drop",
                                         "problem", "kind",  "hint" };
    static const char *const ROOMS[] = { "value", "min", "max", "step" };
    static const char *const FLAGS[] = { "shown",    "overridden", "mixed",
                                         "readonly", "invalid",    "integer",
                                         "float",    "slider",     "checked",
                                         "preview" };
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    ADD_TEXTS(doc, row, TEXTS);
    for (size_t i = 0; i < sizeof ROOMS / sizeof *ROOMS; i++) {
        add_room(doc, row, ROOMS[i]);
    }
    ADD_FLAGS(doc, row, FLAGS);
    yyjson_mut_obj_add_int(doc, row, "precision", 0);
    yyjson_mut_obj_add_int(doc, row, "slot", 0);
    yyjson_mut_obj_add_arr(doc, row, "axes");
    table_new(doc, row);
    return row;
}

static yyjson_mut_val *panel_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "key", "title", "badge" };
    static const char *const FLAGS[] = { "shown", "open", "menu" };
    yyjson_mut_val *panel = yyjson_mut_obj(doc);
    ADD_TEXTS(doc, panel, TEXTS);
    ADD_FLAGS(doc, panel, FLAGS);
    yyjson_mut_obj_add_int(doc, panel, "order", 0);
    yyjson_mut_obj_add_arr(doc, panel, "rows");
    return panel;
}

static yyjson_mut_val *tab_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "icon", "tip" };
    static const char *const FLAGS[] = { "on", "sep" };
    yyjson_mut_val *tab = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, tab, "id", 0);
    ADD_TEXTS(doc, tab, TEXTS);
    ADD_FLAGS(doc, tab, FLAGS);
    return tab;
}

static yyjson_mut_val *chip_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "label", "icon", "name" };
    yyjson_mut_val *chip = yyjson_mut_obj(doc);
    ADD_TEXTS(doc, chip, TEXTS);
    return chip;
}

static yyjson_mut_val *behavior_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, row, "name", "");
    yyjson_mut_obj_add_bool(doc, row, "open", false);
    return row;
}

/* A MenuItem's item. */
static yyjson_mut_val *menu_item_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "icon", "label", "shortcut" };
    static const char *const FLAGS[] = { "checked", "disabled" };
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    ADD_TEXTS(doc, item, TEXTS);
    ADD_FLAGS(doc, item, FLAGS);
    return item;
}

static yyjson_mut_val *item_new(yyjson_mut_doc *doc, item_kind kind)
{
    switch (kind) {
        case ITEM_ROW: return row_new(doc);
        case ITEM_AXIS: return axis_new(doc);
        case ITEM_TAB: return tab_new(doc);
        case ITEM_CHIP: return chip_new(doc);
        case ITEM_BEHAVIOR: return behavior_new(doc);
        case ITEM_MENU: return menu_item_new(doc);
    }
    abort();
}

/* Lists keep their items, so data-for keeps their elements: the item at
   `i`, added when the list is shorter. */
static yyjson_mut_val *item_at(view *v, yyjson_mut_val *list, size_t i, item_kind kind)
{
    if (i < yyjson_mut_arr_size(list)) {
        return yyjson_mut_arr_get(list, i);
    }
    yyjson_mut_val *item = item_new(v->doc, kind);
    yyjson_mut_arr_append(list, item);
    v->changed = true;
    return item;
}

static yyjson_mut_val *option_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *option = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, option, "value", 0);
    yyjson_mut_obj_add_str(doc, option, "label", "");
    yyjson_mut_obj_add_str(doc, option, "icon", "");
    return option;
}

void studio_properties_add(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "query", "keepTip", "view", "note" };
    static const char *const FLAGS[] = { "live", "readonly" };
    static const char *const IDENTITY_TEXTS[] = { "kind", "icon", "name",
                                                  "of",   "kbd",  "prototype",
                                                  "why" };
    static const char *const IDENTITY_FLAGS[] = { "locked", "placed", "actions" };
    static const char *const BEHAVIOR_FLAGS[] = { "shown", "open", "locked" };
    static const char *const ADD_FLAGS_[] = { "shown", "locked" };
    static const char *const MENU_TEXTS[] = { "title", "label", "icon", "about" };
    yyjson_mut_obj_add_arr(doc, yyjson_mut_doc_get_root(doc), "propertyValues");
    yyjson_mut_val *root = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "properties");
    ADD_TEXTS(doc, root, TEXTS);
    ADD_FLAGS(doc, root, FLAGS);
    yyjson_mut_obj_add_arr(doc, root, "tabs");
    yyjson_mut_val *identity = yyjson_mut_obj_add_obj(doc, root, "identity");
    ADD_TEXTS(doc, identity, IDENTITY_TEXTS);
    ADD_FLAGS(doc, identity, IDENTITY_FLAGS);
    yyjson_mut_obj_add_arr(doc, identity, "chips");
    yyjson_mut_obj_add_arr(doc, root, "panels");
    yyjson_mut_val *behaviors = yyjson_mut_obj_add_obj(doc, root, "behaviors");
    ADD_FLAGS(doc, behaviors, BEHAVIOR_FLAGS);
    yyjson_mut_obj_add_str(doc, behaviors, "why", "");
    yyjson_mut_obj_add_arr(doc, behaviors, "rows");
    yyjson_mut_obj_add_arr(doc, behaviors, "assets");
    yyjson_mut_val *add = yyjson_mut_obj_add_obj(doc, root, "add");
    ADD_FLAGS(doc, add, ADD_FLAGS_);
    yyjson_mut_obj_add_str(doc, add, "why", "");
    yyjson_mut_val *field_menu = yyjson_mut_obj_add_obj(doc, root, "fieldMenu");
    yyjson_mut_obj_add_val(doc, field_menu, "reset", menu_item_new(doc));
    yyjson_mut_obj_add_val(doc, field_menu, "apply", menu_item_new(doc));
    yyjson_mut_val *component_menu = yyjson_mut_obj_add_obj(doc, root, "componentMenu");
    ADD_TEXTS(doc, component_menu, MENU_TEXTS);
    yyjson_mut_obj_add_bool(doc, component_menu, "disabled", false);
}

/* ---------- Rows ---------- */

/* The rows of one panel, written in order. */
typedef struct rows
{
    view *v;
    yyjson_mut_val *list;
    size_t count;
} rows;

/* What every row shows. */
typedef struct row_desc
{
    const char *id, *label, *about, *control;
    bool overridden, mixed, readonly, invalid;
} row_desc;

static yyjson_mut_val *next_row(rows *r, const row_desc *d)
{
    view *v = r->v;
    v->s->panel_work.properties_fields++;
    yyjson_mut_val *row = item_at(v, r->list, r->count++, ITEM_ROW);
    put_flag(v, row, "shown", true);
    put_text(v, row, "id", d->id);
    put_text(v, row, "label", d->label);
    put_text(v, row, "tip", d->about && d->about[0] ? d->label : "");
    put_text(v, row, "about", d->about ? d->about : "");
    put_text(v, row, "control", d->control);
    put_flag(v, row, "overridden", d->overridden);
    put_flag(v, row, "mixed", d->mixed);
    put_flag(v, row, "readonly", d->readonly);
    put_flag(v, row, "invalid", d->invalid);
    return row;
}

/* How a number edits: Blender's and Base UI's NumberField attributes. */
typedef struct number_shape
{
    double min, max, step;
    int precision;
    bool integer, single, slider;
    const char *unit;
} number_shape;

/* A presented range draws a slider; a presented unit follows the value. */
static void number_hints(
    const studio *s,
    const ecs_component_desc *desc,
    const type_field *field,
    number_shape *d
)
{
    field_hint hint = component_presentation_field(
        &s->presentation.hints,
        desc,
        (int)(field - desc->type->fields)
    );
    if (hint.min < hint.max) {
        d->min = hint.min;
        d->max = hint.max;
        d->slider = true;
    }
    if (hint.unit) {
        d->unit = hint.unit;
    }
}

/* An angle shows in degrees, as Unity's and Blender's rotations do; its
   range turns with it. */
static double to_degrees(const type_field *field, double value, number_shape *d)
{
    if (!properties_angle(field)) {
        return value;
    }
    d->unit = d->unit && strchr(d->unit, '/') ? "\xc2\xb0/s" : "\xc2\xb0";
    d->min *= 180 / M_PI;
    d->max *= 180 / M_PI;
    return value * 180 / M_PI;
}

/* Units of length (world units, pixels and their rates) and of angle step
   by one; other quantities by a tenth. */
static bool whole_steps(const char *unit)
{
    return unit && (!strcmp(unit, "u") || !strcmp(unit, "px") ||
                    !strncmp(unit, "u/", 2) || !strncmp(unit, "\xc2\xb0", 2));
}

/* A float's step, from its range or unit and never its value, so arrows and
   scrubs keep one gain however large the value grows: a presented range
   steps by the power of ten nearest a hundredth of it, with a digit more
   than the step shows. */
static void float_step(number_shape *d)
{
    if (d->slider) {
        int exponent = (int)floor(log10((d->max - d->min) / 100));
        d->step = pow(10, exponent);
        d->precision = exponent >= 1 ? 0 : 1 - exponent;
        return;
    }
    d->step = whole_steps(d->unit) ? 1 : 0.1;
    d->precision = d->unit && !strncmp(d->unit, "\xc2\xb0", 2) ? 1 : 2;
}

/* The largest integer a double holds exactly, which a number field edits;
   64-bit fields edit within it. */
#define EXACT_INTEGER 9007199254740992.0

/* The type's range, within an authored one; unbounded types stay so. */
static void number_bounds(pin_type type, number_shape *d)
{
    d->single = type == PIN_F32;
    double min = -INFINITY, max = INFINITY;
    switch (type) {
        case PIN_I8: min = INT8_MIN, max = INT8_MAX; break;
        case PIN_U8: min = 0, max = UINT8_MAX; break;
        case PIN_I16: min = INT16_MIN, max = INT16_MAX; break;
        case PIN_U16: min = 0, max = UINT16_MAX; break;
        case PIN_I32:
        case PIN_ENUM: min = INT32_MIN, max = INT32_MAX; break;
        case PIN_U32: min = 0, max = UINT32_MAX; break;
        case PIN_I64: min = -EXACT_INTEGER, max = EXACT_INTEGER; break;
        case PIN_U64: min = 0, max = EXACT_INTEGER; break;
        case PIN_F32: min = -FLT_MAX, max = FLT_MAX; break;
        default: break;
    }
    bool bounded = d->min != 0 || d->max != 0;
    d->min = bounded ? fmax(d->min, min) : min;
    d->max = bounded ? fmin(d->max, max) : max;
}

static void put_shape(view *v, yyjson_mut_val *obj, const number_shape *d)
{
    put_number(v, obj, "min", d->min, false);
    put_number(v, obj, "max", d->max, false);
    put_number(v, obj, "step", d->step, false);
    put_int(v, obj, "precision", d->precision);
    put_text(v, obj, "unit", d->unit ? d->unit : "");
    put_flag(v, obj, "slider", d->slider);
}

static void number_row(rows *r, const row_desc *d, const number_shape *shape, double value)
{
    view *v = r->v;
    yyjson_mut_val *row = next_row(r, d);
    put_value(v, row, value, shape->single);
    put_shape(v, row, shape);
    put_flag(v, row, "integer", shape->integer);
    put_flag(v, row, "float", shape->single);
}

static void text_row(rows *r, const row_desc *d, const char *text)
{
    put_text(r->v, next_row(r, d), "text", text);
}

static void bool_row(rows *r, const row_desc *d, bool checked)
{
    put_flag(r->v, next_row(r, d), "checked", checked);
}

static void color_row(rows *r, const row_desc *d, uint32_t rgba)
{
    char hex[16];
    snprintf(hex, sizeof hex, "#%08" PRIx32, rgba);
    put_room(r->v, next_row(r, d), "value", hex);
}

/* One axis of a vector row. */
typedef struct axis_desc
{
    const char *key, *label;
    double value;
    number_shape shape;
    bool mixed, overridden;
} axis_desc;

static void vector_row(rows *r, const row_desc *d, const axis_desc *axes, int count)
{
    view *v = r->v;
    yyjson_mut_val *row = next_row(r, d);
    yyjson_mut_val *list = member(row, "axes");
    bool single = true;
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *axis = item_at(v, list, (size_t)i, ITEM_AXIS);
        put_text(v, axis, "key", axes[i].key);
        put_text(v, axis, "label", axes[i].label);
        put_value(v, axis, axes[i].value, axes[i].shape.single);
        put_shape(v, axis, &axes[i].shape);
        put_flag(v, axis, "mixed", axes[i].mixed);
        put_flag(v, axis, "overridden", axes[i].overridden);
        single &= axes[i].shape.single;
    }
    trim(v, list, (size_t)count);
    put_flag(v, row, "float", single);
}

/* ---------- Choice and asset lists ---------- */

/* Rewrites `table`'s groups when its key, what they list, changed. */
static yyjson_mut_val *table_of(view *v, yyjson_mut_val *row, const char *key, bool *rebuild)
{
    yyjson_mut_val *table = member(row, "table");
    *rebuild = strcmp(yyjson_mut_get_str(member(table, "key")), key) != 0;
    if (*rebuild) {
        put_text(v, table, "key", key);
        yyjson_mut_arr_clear(member(table, "groups"));
    }
    return table;
}

/* The options of group `label`, made when it differs from the last one's. */
static yyjson_mut_val *group_options(view *v, yyjson_mut_val *table, const char *label)
{
    yyjson_mut_val *groups = member(table, "groups");
    size_t count = yyjson_mut_arr_size(groups);
    yyjson_mut_val *last = count ? yyjson_mut_arr_get(groups, count - 1) : NULL;
    if (last && !strcmp(yyjson_mut_get_str(member(last, "label")), label)) {
        return member(last, "options");
    }
    yyjson_mut_val *group = yyjson_mut_arr_add_obj(v->doc, groups);
    yyjson_mut_obj_add_strcpy(v->doc, group, "label", label);
    return yyjson_mut_obj_add_arr(v->doc, group, "options");
}

static void add_option(view *v, yyjson_mut_val *options, const editor_choice *choice)
{
    yyjson_mut_val *option = option_new(v->doc);
    yyjson_mut_set_sint(member(option, "value"), choice->value);
    put_text(v, option, "label", choice->label);
    put_glyph(v, option, "icon", choice->icon);
    yyjson_mut_arr_append(options, option);
}

/* A choice lists every option once opened, else only the current one,
   which its closed field shows. */
static void choice_row(rows *r, const row_desc *d, const editor_choices *choices, int value)
{
    view *v = r->v;
    yyjson_mut_val *row = next_row(r, d);
    bool open = !strcmp(v->s->properties.choosing, d->id);
    char text[16], key[PROPERTIES_ROW_ID + 32];
    snprintf(text, sizeof text, "%d", value);
    put_room(v, row, "value", text);
    snprintf(key, sizeof key, "%s:%s", d->id, open ? "*" : text);
    bool rebuild;
    yyjson_mut_val *table = table_of(v, row, key, &rebuild);
    for (int i = 0; rebuild && i < editor_choices_count(choices); i++) {
        editor_choice choice = editor_choice_at(choices, i);
        if (open || choice.value == value) {
            add_option(v, group_options(v, table, choice.group ? choice.group : ""), &choice);
        }
    }
    v->changed |= rebuild;
}

/* An asset a field names: its kind's assets once opened. */
typedef struct asset_desc
{
    reference_asset_kind kind;
    const char *value, *none;
} asset_desc;

static void asset_row(rows *r, row_desc d, const asset_desc *asset)
{
    view *v = r->v;
    studio *s = v->s;
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    /* Events are names: host, action and ui events have no asset. */
    bool named = asset->value[0] && index && asset->kind != REFERENCE_ASSET_EVENT;
    const reference_asset
        *found = named ? reference_index_asset_at(
                             index,
                             reference_index_find_name(index, asset->kind, asset->value)
                         )
                       : NULL;
    char problem[192] = "", dragged[256];
    if (named && (!found || found->unresolved)) {
        snprintf(
            problem,
            sizeof problem,
            "No %s asset is named %s",
            studio_asset_kind_name(asset->kind),
            asset->value
        );
    }
    char over[PROPERTIES_ROW_ID];
    bool preview = !d.readonly &&
                   properties_drop_over(s, over, dragged, sizeof dragged) &&
                   !strcmp(over, d.id);
    d.invalid = problem[0] && !preview;
    yyjson_mut_val *row = next_row(r, &d);
    put_text(v, row, "text", preview ? dragged : asset->value);
    put_flag(v, row, "preview", preview);
    put_text(v, row, "problem", preview ? "" : problem);
    put_text(
        v,
        row,
        "drop",
        d.readonly || asset->kind == REFERENCE_ASSET_EVENT
            ? ""
            : studio_asset_kind_name(asset->kind)
    );
    /* The field's picker lists the kind's assets (palette.pick). */
    put_text(v, row, "kind", studio_asset_kind_name(asset->kind));
    yyjson_mut_val *table = member(row, "table");
    put_text(v, table, "label", studio_asset_kind_label(asset->kind));
    put_glyph(v, table, "icon", studio_asset_kind_icon(asset->kind));
    put_text(v, table, "none", asset->none);
}

/* ---------- Objects ---------- */

/* The object a handle names: a placed object by its name, a spawned one by
   its prototype's; NULL once it is gone. */
static const char *entity_name(const app_scene_authoring *scene, ecs_entity entity)
{
    if (!ecs_entity_is_alive(scene->world, entity)) {
        return NULL;
    }
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            return scene->actors[i].name;
        }
    }
    return runtime_entity_label(scene, entity);
}

/* Objects that name different objects show a dash, as a mixed text does,
   and select none of them. */
static void mixed_entity_row(rows *r, row_desc d)
{
    d.control = "entity";
    yyjson_mut_val *row = next_row(r, &d);
    put_text(r->v, row, "text", "\xe2\x80\x94");
    put_text(r->v, row, "hint", "");
    put_text(r->v, row, "problem", "The selected objects name different objects");
    yyjson_mut_val *table = member(row, "table");
    put_glyph(r->v, table, "icon", KIT_ICON_TARGET);
    put_text(r->v, table, "none", "None");
}

/* What an entity field's tooltip says about where its object comes from. */
static const char *entity_problem(const char *name, const char *path, bool unresolved, char *text, size_t size)
{
    if (unresolved) {
        snprintf(text, size, "No object has the path %s", path);
    } else if (!name) {
        snprintf(text, size, "The object this field named was removed");
    } else if (path) {
        snprintf(text, size, "Named by the path %s", path);
    } else {
        snprintf(text, size, "Set while the game runs");
    }
    return text;
}

/* An object a field names, which the game sets as it runs or the file names
   by `path`, unresolved when no object has it. Studio can't pick one to save
   yet, so the field shows the object, as Unity's object field does, and a
   click selects it rather than offering a pick that could not be kept. */
static void entity_row(rows *r, row_desc d, uint64_t handle, const char *path, bool unresolved)
{
    view *v = r->v;
    studio *s = v->s;
    if (d.mixed) {
        mixed_entity_row(r, d);
        return;
    }
    ecs_entity entity = ecs_entity_from_u64(handle);
    const char *name = handle ? entity_name(s->scene, entity)
                     : path   ? path
                              : "";
    char hint[160], problem[192];
    snprintf(hint, sizeof hint, "Select %s", name ? name : "");
    d.control = "entity";
    d.invalid = !name || unresolved;
    yyjson_mut_val *row = next_row(r, &d);
    put_text(v, row, "text", name ? name : "Missing object");
    put_text(v, row, "hint", name && name[0] && handle ? hint : "");
    put_text(v, row, "problem", entity_problem(name, path, unresolved, problem, sizeof problem));
    uint32_t icon = d.invalid ? KIT_ICON_WARNING
                  : !handle
                      ? KIT_ICON_TARGET
                      : studio_prototype_icon(s, scene_entity_prototype(s->scene, entity));
    yyjson_mut_val *table = member(row, "table");
    put_glyph(v, table, "icon", icon);
    put_text(v, table, "none", "None");
}

/* ---------- Reflected fields ---------- */

/* Where a field is read and how it is marked. */
typedef struct field_source
{
    const ecs_component_desc *desc;
    const prototype_component *component;
    int index;
    pin_value value;
    row_desc row;
    const char *path; /* the entity path a handle field names, if any */
    bool placed; /* a placed object's field, whose path the scene resolves */
} field_source;

static double integer_of(const pin_value *value)
{
    switch (value->type) {
        case PIN_I8: return value->i8;
        case PIN_U8: return value->u8;
        case PIN_I16: return value->i16;
        case PIN_U16: return value->u16;
        case PIN_U32: return value->u;
        case PIN_ENUM: return value->e;
        case PIN_I64: return (double)value->i64;
        case PIN_U64: return (double)value->u64;
        default: return value->i;
    }
}

static void integer_row(rows *r, const field_source *f)
{
    const type_field *field = &f->desc->type->fields[f->index];
    const editor_choices *choices = property_choices(f->desc->type, field);
    row_desc d = f->row;
    if (choices && editor_choices_count(choices) > 0) {
        d.control = "choice";
        choice_row(
            r,
            &d,
            choices,
            f->value.type == PIN_ENUM ? f->value.e : f->value.i
        );
        return;
    }
    number_shape shape = { .step = 1, .integer = true };
    number_hints(r->v->s, f->desc, field, &shape);
    number_bounds(f->value.type, &shape);
    d.control = "number";
    number_row(r, &d, &shape, integer_of(&f->value));
}

/* A 64-bit integer edits in a number field while a double holds it
   exactly; past that, as its text, which keeps every digit a number field
   would round away or clamp. */
static void integer64_row(rows *r, const field_source *f)
{
    const pin_value *value = &f->value;
    bool exact = value->type == PIN_I64 ? value->i64 >= -(int64_t)EXACT_INTEGER &&
                                              value->i64 <= (int64_t)EXACT_INTEGER
                                        : value->u64 <= (uint64_t)EXACT_INTEGER;
    if (exact) {
        integer_row(r, f);
        return;
    }
    char text[32];
    if (value->type == PIN_I64) {
        snprintf(text, sizeof text, "%" PRId64, value->i64);
    } else {
        snprintf(text, sizeof text, "%" PRIu64, value->u64);
    }
    row_desc d = f->row;
    d.control = "text";
    text_row(r, &d, text);
}

static void float_row(rows *r, const field_source *f)
{
    const type_field *field = &f->desc->type->fields[f->index];
    number_shape shape = { 0 };
    number_hints(r->v->s, f->desc, field, &shape);
    double value = to_degrees(
        field,
        f->value.type == PIN_F32 ? f->value.f : f->value.d,
        &shape
    );
    float_step(&shape);
    number_bounds(f->value.type, &shape);
    row_desc d = f->row;
    d.control = "number";
    number_row(r, &d, &shape, value);
}

static void struct_row(rows *r, const field_source *f)
{
    static const char *const LABELS[] = { "X", "Y", "Z", "W" };
    const type_desc *type = properties_vector(&f->value);
    row_desc d = f->row;
    if (!type) {
        d.control = "";
        next_row(r, &d);
        return;
    }
    axis_desc axes[4];
    for (int i = 0; i < type->field_count; i++) {
        pin_value axis = { 0 };
        pin_read_field(&r->v->storage, f->value.ptr, &type->fields[i], &axis);
        double value = axis.type == PIN_F32 ? axis.f : axis.d;
        axes[i] = (axis_desc){ .key = type->fields[i].name,
                               .label = LABELS[i],
                               .value = value,
                               .mixed = d.mixed,
                               .overridden = d.overridden };
        number_hints(
            r->v->s,
            f->desc,
            &f->desc->type->fields[f->index],
            &axes[i].shape
        );
        float_step(&axes[i].shape);
        number_bounds(axis.type, &axes[i].shape);
    }
    d.control = "vector";
    vector_row(r, &d, axes, type->field_count);
}

static void value_row(rows *r, const field_source *f)
{
    const type_field *field = &f->desc->type->fields[f->index];
    const pin_value *value = &f->value;
    row_desc d = f->row;
    switch (value->type) {
        case PIN_BOOL:
            d.control = "bool";
            /* A mixed checkbox sets every object on its first click. */
            bool_row(r, &d, !d.mixed && value->b);
            return;
        case PIN_U32:
            if (appearance_color_field(f->desc->type, field)) {
                d.control = "color";
                color_row(r, &d, value->u);
                return;
            }
            /* fallthrough */
        case PIN_I32:
        case PIN_ENUM:
        case PIN_I8:
        case PIN_U8:
        case PIN_I16:
        case PIN_U16: integer_row(r, f); return;
        case PIN_I64:
        case PIN_U64: integer64_row(r, f); return;
        case PIN_F32:
        case PIN_F64: float_row(r, f); return;
        case PIN_STRUCT: struct_row(r, f); return;
        case PIN_NAME:
        case PIN_STR: {
            const char *text = "";
            const char *error = pin_text_cstring(&r->v->storage, *value, &text, NULL);
            d.control = "text";
            if (error) {
                d.readonly = true;
                d.invalid = true;
                d.control = "label";
                text = pin_value_to_string(value);
            }
            /* A mixed text stays empty, its placeholder a dash. */
            yyjson_mut_val *row = next_row(r, &d);
            put_text(r->v, row, "text", d.mixed ? "" : text);
            put_text(r->v, row, "problem", error ? error : "");
            return;
        }
        case PIN_HANDLE:
            entity_row(r, d, value->u64, f->path, f->path && f->placed && !value->u64);
            return;
        default:
            d.control = "label";
            text_row(r, &d, pin_value_to_string(value));
            return;
    }
}

static void field_row(rows *r, const field_source *f)
{
    const type_field *field = &f->desc->type->fields[f->index];
    reference_asset_kind named = properties_named(f->desc, field, &f->value);
    if (named == REFERENCE_ASSET_KIND_COUNT) {
        value_row(r, f);
        return;
    }
    row_desc d = f->row;
    d.control = "asset";
    const char *text = "";
    if (pin_text_cstring(&r->v->storage, f->value, &text, NULL)) {
        value_row(r, f);
        return;
    }
    asset_row(r, d, &(asset_desc){ .kind = named, .value = text, .none = "None" });
}

/* An object's field, edited in its world: the running one while the scene
   runs. A gesture's other editor (collaboration) shows what it sent. */
static void object_field(rows *r, scene_actor *actor, const prototype_component *component, int index, bool locked)
{
    studio *s = r->v->s;
    app_scene_authoring *scene = s->scene;
    const ecs_component_desc *desc = component->desc;
    const type_field *field = &desc->type->fields[index];
    char label[128], id[PROPERTIES_ROW_ID];
    pin_value value = { 0 };
    if (ecs_component_read_field(&r->v->storage, scene->world, actor->entity, desc->id, index, &value) !=
            ECS_OK ||
        !properties_label(s, component, field, label, sizeof label)) {
        return;
    }
    bool pending = scene->property_actor.slot &&
                   ecs_entity_to_u64(scene->property_actor) ==
                       ecs_entity_to_u64(actor->entity) &&
                   scene->property_edit.component == desc->id &&
                   scene->property_edit.field == index;
    pin_value shown = pending && scene_collab_is_editor() ? scene->property_edit.after
                                                          : value;
    properties_row_id(
        &(properties_row){ .kind = PROPERTIES_OBJECT,
                           .entity = ecs_entity_to_u64(actor->entity),
                           .component = desc->id,
                           .field = index },
        id
    );
    field_source f = {
        .desc = desc,
        .component = component,
        .index = index,
        .value = shown,
        .row = { .id = id,
                 .label = label,
                 .about = field->description,
                 .overridden = properties_overridden(scene, actor, component, index),
                 .mixed = properties_mixed(s, actor, desc->id, index, &value),
                 .readonly = locked },
        .path = scene_link_path(actor->paths, desc->id, index),
        .placed = true
    };
    field_row(r, &f);
}

/* An object's position, one row of two axes as the Scene moves it. */
static void position_row(rows *r, scene_actor *actor, const prototype_component *component, bool locked)
{
    studio *s = r->v->s;
    const Transform *position = Transform_get_const(s->scene->world, actor->entity);
    if (!studio_matches(s->property_search, "Position")) {
        return;
    }
    static const char *const KEYS[] = { "x", "y" }, *const LABELS[] = { "X", "Y" };
    axis_desc axes[2];
    for (int i = 0; i < 2; i++) {
        pin_value shown = { .type = PIN_F32, .f = i ? position->y : position->x };
        axes[i] = (axis_desc){
            .key = KEYS[i],
            .label = LABELS[i],
            .value = shown.f,
            .shape = { .min = -FLT_MAX, .max = FLT_MAX, .step = 1, .precision = 2, .single = true },
            .mixed = properties_mixed(s, actor, COMPONENT_Transform.id, i, &shown),
            .overridden = properties_overridden(s->scene, actor, component, i)
        };
    }
    char id[PROPERTIES_ROW_ID];
    properties_row_id(
        &(properties_row){ .kind = PROPERTIES_OBJECT,
                           .entity = ecs_entity_to_u64(actor->entity),
                           .component = COMPONENT_Transform.id },
        id
    );
    vector_row(
        r,
        &(row_desc){ .id = id, .label = "Position", .control = "vector", .readonly = locked },
        axes,
        2
    );
}

/* ---------- Panels ---------- */

typedef struct panel_desc
{
    const char *key, *title, *badge;
    int order; /* where its slot goes among the others */
    bool open, menu;
} panel_desc;

/* The panels follow the registry's order; the Scene tab's come after them. */
#define SCENE_PANELS (1 << 20)

static int registry_order(const app_scene_authoring *scene, const ecs_component_desc *desc)
{
    for (int i = 0; i < scene->registry.component_count; i++) {
        if (scene->registry.components[i] == desc) {
            return i;
        }
    }
    return scene->registry.component_count;
}

/* The slot of panel `key`, kept for the session as the kit keyed its panels,
   so a component's rows keep their elements from one selection to the next;
   a slot new to the session goes where its order puts it. */
static yyjson_mut_val *panel_slot(view *v, const panel_desc *d)
{
    yyjson_mut_val *list = member(v->root, "panels");
    size_t at = 0, count = yyjson_mut_arr_size(list);
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_val *panel = yyjson_mut_arr_get(list, i);
        if (!strcmp(yyjson_mut_get_str(member(panel, "key")), d->key)) {
            return panel;
        }
        at += yyjson_mut_get_sint(member(panel, "order")) < d->order;
    }
    yyjson_mut_val *panel = panel_new(v->doc);
    put_text(v, panel, "key", d->key);
    put_int(v, panel, "order", d->order);
    yyjson_mut_arr_insert(list, panel, at);
    v->changed = true;
    return panel;
}

/* The panel's head; its rows follow while it is open. */
static rows next_panel(view *v, const panel_desc *d)
{
    yyjson_mut_val *panel = panel_slot(v, d);
    apush(v->shown, panel);
    put_flag(v, panel, "shown", true);
    put_text(v, panel, "title", d->title);
    put_text(v, panel, "badge", d->badge ? d->badge : "");
    put_flag(v, panel, "open", d->open);
    put_flag(v, panel, "menu", d->menu);
    return (rows){ .v = v, .list = member(panel, "rows") };
}

static void end_panel(rows *r)
{
    hide_rest(r->v, r->list, r->count);
}

/* Whether the search finds the component or any field it shows. */
static bool found(studio *s, const prototype_component *component, const char *title)
{
    const ecs_component_desc *desc = component->desc;
    if (!s->property_search[0] || studio_matches(s->property_search, title)) {
        return true;
    }
    if (desc->id == COMPONENT_Transform.id &&
        studio_matches(s->property_search, "Position")) {
        return true;
    }
    for (int i = 0; i < desc->type->field_count; i++) {
        char label[128];
        if (properties_label(s, component, &desc->type->fields[i], label, sizeof label)) {
            return true;
        }
    }
    return false;
}

static void component_panel(view *v, scene_actor *actor, const ecs_component_desc *desc, bool locked)
{
    studio *s = v->s;
    prototype_component added;
    const prototype_component *component = properties_base(actor, desc, &added);
    char title[128], badge[16] = "";
    editor_component_label(desc->name, title, sizeof title);
    if (!found(s, component, title)) {
        return;
    }
    int overrides = 0;
    for (int i = 0; i < desc->type->field_count; i++) {
        overrides += properties_overridden(s->scene, actor, component, i);
    }
    if (overrides) {
        snprintf(badge, sizeof badge, "%d", overrides);
    }
    /* An authored object's component its prototype lacks is its own, but
       for one the engine derives from another, which it computes. */
    bool derived = desc->derived_from != 0;
    bool own = component == &added && actor->identity && !studio_live(s) && !derived;
    if (own) {
        snprintf(badge, sizeof badge, "Added");
    }
    if (derived) {
        snprintf(badge, sizeof badge, "Computed");
        locked = true;
    }
    const char *key = properties_panel_key(desc->name);
    panel_desc d = { .key = key,
                     .title = title,
                     .badge = badge,
                     .order = registry_order(s->scene, desc),
                     .open = properties_open(s, key),
                     /* While the scene runs, an authored object's component
                        menu keeps its values alone; else it removes a
                        component the object added. */
                     .menu = !derived && ((studio_live(s) && actor->identity) ||
                                          (own && !properties_multi(s, actor))) };
    rows r = next_panel(v, &d);
    const void *data = ecs_component_data_const(s->scene->world, actor->entity, desc->id);
    for (int i = 0; d.open && i < desc->type->field_count; i++) {
        if (desc->id == COMPONENT_Transform.id) {
            position_row(&r, actor, component, locked);
            break;
        }
        if (property_field_visible(desc, &desc->type->fields[i], data)) {
            object_field(&r, actor, component, i, locked);
        }
    }
    end_panel(&r);
}

static void show_prototype_field(
    rows *r,
    scene_prototype_asset *asset,
    const prototype_component *component,
    int index,
    bool locked
)
{
    studio *s = r->v->s;
    const type_field *field = &component->desc->type->fields[index];
    char label[128], id[PROPERTIES_ROW_ID];
    if (!properties_label(s, component, field, label, sizeof label)) {
        return;
    }
    properties_row row = { .kind = PROPERTIES_PROTOTYPE,
                           .component = component->desc->id,
                           .field = index };
    snprintf(row.prototype, sizeof row.prototype, "%s", asset->prototype->name);
    properties_row_id(&row, id);
    field_source f = {
        .desc = component->desc,
        .component = component,
        .index = index,
        .row = { .id = id, .label = label, .about = field->description, .readonly = locked },
        .path = scene_link_path(asset->prototype->links, component->desc->id, index)
    };
    if (pin_read_field(&r->v->storage, component->data, field, &f.value)) {
        return;
    }
    const prototype *shown = scene_source_input_preview(s->scene, asset);
    if (shown != asset->prototype) {
        pin_value pending = { 0 };
        if (prototype_read_field(
                &r->v->storage,
                shown,
                component->desc->name,
                field->name,
                &pending
            ) == 0) {
            f.value = pending;
            f.path = scene_link_path(shown->links, component->desc->id, index);
        }
    }
    field_row(r, &f);
}

static void prototype_panels(view *v, scene_prototype_asset *asset, bool locked)
{
    studio *s = v->s;
    prototype *prototype = asset->prototype;
    for (int i = 0; i < acount(prototype->components); i++) {
        const prototype_component *component = &prototype->components[i];
        const ecs_component_desc *desc = component->desc;
        char title[128];
        editor_component_label(desc->name, title, sizeof title);
        if (properties_tab_of(s, desc) != s->tab || !component->data ||
            !found(s, component, title)) {
            continue;
        }
        const char *key = properties_panel_key(desc->name);
        panel_desc d = { .key = key,
                         .title = title,
                         .order = registry_order(s->scene, desc),
                         .open = properties_open(s, key),
                         .menu = true };
        rows r = next_panel(v, &d);
        for (int f = 0; d.open && f < desc->type->field_count; f++) {
            if (property_field_visible(desc, &desc->type->fields[f], component->data)) {
                show_prototype_field(&r, asset, component, f, locked);
            }
        }
        end_panel(&r);
    }
}

/* ---------- The Scene tab ---------- */

static void scene_row(
    properties_scene_setting setting,
    const char *label,
    bool locked,
    row_desc *d,
    char *id
)
{
    properties_row_id(&(properties_row){ .kind = PROPERTIES_SCENE, .field = setting }, id);
    *d = (row_desc){ .id = id, .label = label, .readonly = locked };
}

static void scene_panels(view *v, bool locked)
{
    studio *s = v->s;
    app_scene_authoring *scene = s->scene;
    char id[PROPERTIES_ROW_ID], project[160];
    row_desc d;
    if (studio_matches(s->property_search, "Root document")) {
        bool open = properties_open(s, "ui");
        rows ui = next_panel(
            v,
            &(panel_desc){ .key = "ui", .title = "UI", .order = SCENE_PANELS, .open = open }
        );
        if (open) {
            snprintf(
                project,
                sizeof project,
                "Project (%s)",
                scene->default_ui ? scene->default_ui : "none"
            );
            scene_row(SCENE_UI, "Root document", locked, &d, id);
            d.control = "asset";
            asset_row(
                &ui,
                d,
                &(asset_desc){ .kind = REFERENCE_ASSET_UI,
                               .value = scene->ui ? scene->ui : "",
                               .none = project }
            );
        }
        end_panel(&ui);
    }
    physics_settings physics = physics_configuration(scene->live->runtime->physics);
    bool open = properties_open(s, "physics");
    rows r = next_panel(
        v,
        &(panel_desc){ .key = "physics", .title = "Physics", .order = SCENE_PANELS + 1, .open = open }
    );
    if (open) {
        static const struct
        {
            properties_scene_setting setting;
            const char *label;
            number_shape shape;
        } NUMBERS[] = {
            { SCENE_WORKERS,
              "Workers",
              { .min = 0, .max = PHYSICS_WORKER_MAX, .step = 1, .integer = true } },
            { SCENE_SUBSTEPS,
              "Substeps",
              { .min = 1, .max = PHYSICS_SUBSTEP_MAX, .step = 1, .integer = true } },
        };
        static const struct
        {
            properties_scene_setting setting;
            const char *label;
        } BOOLS[] = { { SCENE_SLEEP, "Allow sleeping" },
                      { SCENE_CONTINUOUS, "Continuous collision" },
                      { SCENE_FORCE_DRIVE, "Drive with forces" } };
        static const number_shape HERTZ = { .min = 0,
                                            .max = 240,
                                            .step = 1,
                                            .precision = 1,
                                            .unit = "Hz",
                                            .single = true };
        static const number_shape DAMPING = { .min = 0,
                                              .max = 100,
                                              .step = 0.1,
                                              .precision = 2,
                                              .single = true };
        const double integers[] = { physics.workers, physics.substeps };
        const bool flags[] = { physics.sleep, physics.continuous, physics.force_drive };
        for (int i = 0; i < 2; i++) {
            if (studio_matches(s->property_search, NUMBERS[i].label)) {
                scene_row(NUMBERS[i].setting, NUMBERS[i].label, locked, &d, id);
                d.control = "number";
                number_row(&r, &d, &NUMBERS[i].shape, integers[i]);
            }
        }
        for (int i = 0; i < 3; i++) {
            if (studio_matches(s->property_search, BOOLS[i].label)) {
                scene_row(BOOLS[i].setting, BOOLS[i].label, locked, &d, id);
                d.control = "bool";
                bool_row(&r, &d, flags[i]);
            }
        }
        if (studio_matches(s->property_search, "Contact Hz")) {
            scene_row(SCENE_HERTZ, "Contact Hz", locked, &d, id);
            d.control = "number";
            number_row(&r, &d, &HERTZ, physics.contact_hertz);
        }
        if (studio_matches(s->property_search, "Damping")) {
            scene_row(SCENE_DAMPING, "Damping", locked, &d, id);
            d.control = "number";
            number_row(&r, &d, &DAMPING, physics.contact_damping);
        }
    }
    end_panel(&r);
}

/* ---------- Identity ---------- */

/* The object's name edits in place, as Unity's Inspector names one. Authored
   objects have an identity; spawned ones do not. */
static void object_identity(view *v, const properties_subject *subject)
{
    studio *s = v->s;
    scene_actor *actor = subject->actor;
    yyjson_mut_val *id = member(v->root, "identity");
    put_glyph(v, id, "icon", studio_prototype_icon(s, actor->prototype_name));
    put_text(v, id, "name", actor->name);
    put_text(v, id, "of", actor->identity ? "Instance of" : "Spawned from");
    put_text(v, id, "prototype", actor->prototype_name);
    bool locked = subject->locked || !actor->identity;
    const char *why = actor->identity
                        ? properties_while_playing(s, subject->locked, "Stop playing to rename")
                        : "Spawned while playing; Stop removes it";
    put_flag(v, id, "locked", locked);
    put_text(v, id, "kbd", locked ? "" : studio_chord(s, "F2"));
    put_text(v, id, "why", why ? why : "");
    put_flag(v, id, "placed", actor->identity != 0);
    put_flag(v, id, "actions", studio_prototype_action_actor(s) == actor);
}

/* With several objects selected: how many, and a chip per prototype among
   them, with its count, which narrows the selection to its objects. */
static void selection_identity(view *v)
{
    studio *s = v->s;
    app_scene_authoring *scene = s->scene;
    yyjson_mut_val *id = member(v->root, "identity");
    char title[64];
    snprintf(title, sizeof title, "%d objects", scene_selection_count(scene));
    put_text(v, id, "name", title);
    yyjson_mut_val *chips = member(id, "chips");
    size_t count = 0;
    for (int i = 0; i < acount(scene->actors); i++) {
        const char *prototype = scene->actors[i].prototype_name;
        int same = 0, first = i;
        for (int j = 0; j < acount(scene->actors); j++) {
            bool selected = scene_instance_selected(scene, j) &&
                            !strcmp(scene->actors[j].prototype_name, prototype);
            first = selected && j < first ? j : first;
            same += selected;
        }
        if (!scene_instance_selected(scene, i) || first != i) {
            continue;
        }
        char label[160];
        snprintf(label, sizeof label, same > 1 ? "%s \xc3\x97%d" : "%s", prototype, same);
        yyjson_mut_val *chip = item_at(v, chips, count++, ITEM_CHIP);
        put_text(v, chip, "label", label);
        put_glyph(v, chip, "icon", studio_prototype_icon(s, prototype));
        put_text(v, chip, "name", prototype);
    }
    trim(v, chips, count);
}

static void show_prototype_identity(view *v, const prototype *prototype)
{
    studio *s = v->s;
    int instances = 0, components = acount(prototype->components);
    for (int i = 0; i < acount(s->scene->actors); i++) {
        instances += s->scene->actors[i].prototype_name &&
                     !strcmp(s->scene->actors[i].prototype_name, prototype->name);
    }
    char lineage[64];
    snprintf(
        lineage,
        sizeof lineage,
        "%d %s \xc2\xb7 %d %s",
        components,
        components == 1 ? "component" : "components",
        instances,
        instances == 1 ? "instance" : "instances"
    );
    yyjson_mut_val *id = member(v->root, "identity");
    put_glyph(v, id, "icon", studio_prototype_icon(s, prototype->name));
    put_text(v, id, "name", prototype->name);
    put_text(v, id, "of", lineage);
}

/* ---------- Behaviors ---------- */

static void behaviors(view *v, const properties_subject *subject)
{
    studio *s = v->s;
    app_scene_authoring *scene = s->scene;
    yyjson_mut_val *b = member(v->root, "behaviors");
    bool open = properties_open(s, "behaviors");
    const char *why = properties_while_playing(s, subject->locked, "Stop playing to change behaviors");
    put_flag(v, b, "open", open);
    put_flag(v, b, "locked", subject->locked);
    put_text(v, b, "why", why ? why : "");
    s->panel_work.property_choices++;
    char **names = instance_behavior_names(scene, subject->actor->entity);
    yyjson_mut_val *list = member(b, "rows");
    size_t count = 0;
    for (int i = 0; i < acount(names); i++) {
        if (!studio_matches(s->property_search, names[i])) {
            continue;
        }
        yyjson_mut_val *row = item_at(v, list, count++, ITEM_BEHAVIOR);
        put_text(v, row, "name", names[i]);
        put_flag(v, row, "open", scene->graph_document_id && !strcmp(scene->graph_document_id, names[i]));
    }
    trim(v, list, count);
    /* Attach Behavior's menu: every behavior, those attached checked. */
    const reference_index *index = app_scene_authoring_asset_index(scene);
    yyjson_mut_val *assets = member(b, "assets");
    count = 0;
    for (int i = 0; index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (asset->kind != REFERENCE_ASSET_BEHAVIOR || !asset->name) {
            continue;
        }
        bool attached = false;
        for (int j = 0; j < acount(names); j++) {
            attached |= !strcmp(names[j], asset->name);
        }
        yyjson_mut_val *item = item_at(v, assets, count++, ITEM_MENU);
        put_text(v, item, "label", asset->name);
        put_glyph(v, item, "icon", KIT_ICON_GRAPH);
        put_flag(v, item, "checked", attached);
    }
    trim(v, assets, count);
    free_behavior_names(names);
}

/* ---------- Menus ---------- */

static void menu_item(view *v, yyjson_mut_val *item, const char *label, uint32_t icon, bool disabled)
{
    put_text(v, item, "label", label);
    put_glyph(v, item, "icon", icon);
    put_flag(v, item, "disabled", disabled);
}

/* The field menu's Reset to Prototype, which any selected object's
   override enables, and Apply to Prototype (Unity's Apply to Prefab). */
static void field_menu(view *v, const properties_subject *subject)
{
    studio *s = v->s;
    scene_actor *actor = subject->actor;
    const ecs_component_desc
        *desc = actor ? properties_component(s->scene, s->context.component) : NULL;
    bool target = desc && s->context.entity == ecs_entity_to_u64(actor->entity);
    bool locked = subject->locked && !studio_live(s);
    bool overridden = false, apply = false;
    for (int a = 0; target && a < acount(s->scene->actors); a++) {
        scene_actor *other = &s->scene->actors[a];
        bool selected = other == actor ||
                        (subject->multi && scene_instance_selected(s->scene, a) &&
                         ecs_component_has(s->scene->world, other->entity, desc->id));
        prototype_component added;
        const prototype_component *base = properties_base(other, desc, &added);
        /* Position resets both axes. */
        bool position = desc->id == COMPONENT_Transform.id;
        overridden |= selected &&
                      (properties_overridden(s->scene, other, base, s->context.field) ||
                       (position && properties_overridden(s->scene, other, base, 1)));
    }
    s->panel_work.property_overrides += target && actor->identity;
    prototype_field *applicable = target && actor->identity
                                    ? prototype_instance_overrides(
                                          actor->base,
                                          s->scene->world,
                                          actor->entity
                                      )
                                    : NULL;
    for (int i = 0; i < acount(applicable); i++) {
        apply |= applicable[i].component == desc &&
                 applicable[i].field == s->context.field;
    }
    afree(applicable);
    yyjson_mut_val *menu = member(v->root, "fieldMenu");
    menu_item(v, member(menu, "reset"), "Reset to Prototype", KIT_ICON_RESET, locked || !overridden);
    menu_item(
        v,
        member(menu, "apply"),
        "Apply to Prototype",
        KIT_ICON_PROTOTYPE,
        locked || studio_live(s) || !apply
    );
}

/* A component panel's menu: Remove Component on a prototype or a component
   an object added, Keep Changes on a running object. */
static void component_menu(view *v, const properties_subject *subject)
{
    studio *s = v->s;
    yyjson_mut_val *menu = member(v->root, "componentMenu");
    char title[128];
    editor_component_label(s->components.menu, title, sizeof title);
    put_text(v, menu, "title", title);
    bool remove = subject->asset || !studio_live(s);
    const char *why = properties_while_playing(s, subject->locked, "Stop playing to remove");
    put_text(v, menu, "label", remove ? "Remove Component" : "Keep Changes");
    put_glyph(v, menu, "icon", remove ? KIT_ICON_TRASH : KIT_ICON_PIN);
    put_text(v, menu, "about", remove && why ? why : "");
    put_flag(v, menu, "disabled", remove && subject->locked);
}

/* ---------- Tabs and header ---------- */

static void tabs(view *v, const bool available[STUDIO_TAB_COUNT])
{
    studio *s = v->s;
    yyjson_mut_val *list = member(v->root, "tabs");
    size_t count = 0;
    for (int t = 0; t < STUDIO_TAB_COUNT; t++) {
        if (!available[t]) {
            continue;
        }
        bool sep = t == STUDIO_TAB_BEHAVIORS ||
                   (t == STUDIO_TAB_NODE && !available[STUDIO_TAB_BEHAVIORS]) ||
                   (t == STUDIO_TAB_MEMBER && !available[STUDIO_TAB_BEHAVIORS] &&
                    !available[STUDIO_TAB_NODE]);
        yyjson_mut_val *tab = item_at(v, list, count++, ITEM_TAB);
        put_int(v, tab, "id", t);
        put_glyph(v, tab, "icon", properties_tab_icon((studio_tab)t));
        put_text(v, tab, "tip", properties_tab_label((studio_tab)t));
        put_flag(v, tab, "on", s->tab == (studio_tab)t);
        put_flag(v, tab, "sep", sep);
    }
    trim(v, list, count);
}

/* While the scene runs, Properties edits its running world: the header says
   so and offers Keep Changes; a prototype stays read-only. */
static void header(view *v)
{
    studio *s = v->s;
    bool live = studio_properties_live(s);
    put_text(v, v->root, "query", s->property_search);
    put_flag(v, v->root, "live", live);
    put_flag(v, v->root, "readonly", studio_live(s) && !live);
    put_text(
        v,
        v->root,
        "keepTip",
        studio_command_enabled(s, STUDIO_COMMAND_KEEP_CHANGES)
            ? "Copy this object's live values into the scene when you stop"
            : "Select an object placed in the scene"
    );
}

/* Hides the slots this refresh did not show; their rows go, so no view of
   theirs runs at a refresh, and come back from data when the panel does. */
static void hide_panels(view *v)
{
    yyjson_mut_val *panel, *list = member(v->root, "panels");
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(list);
    while ((panel = yyjson_mut_arr_iter_next(&it))) {
        bool written = false;
        for (int i = 0; i < acount(v->shown) && !written; i++) {
            written = v->shown[i] == panel;
        }
        if (!written) {
            put_flag(v, panel, "shown", false);
            trim(v, member(panel, "rows"), 0);
        }
    }
}

/* ---------- The body ---------- */

/* What the body shows; each is written once a refresh, so an unchanged
   one dirties nothing. */
typedef struct shown
{
    const char *view, *identity, *note;
    bool behaviors, add;
} shown;

/* Add Component, below the panels, unless the scene runs. */
static void add_button(view *v, const properties_subject *subject)
{
    yyjson_mut_val *add = member(v->root, "add");
    const char *why = properties_while_playing(v->s, subject->locked, "Stop playing to add components");
    put_flag(v, add, "locked", subject->locked);
    put_text(v, add, "why", why ? why : "");
}

static void object_body(view *v, const properties_subject *subject, shown *out)
{
    studio *s = v->s;
    app_scene_authoring *scene = s->scene;
    scene_actor *actor = subject->actor;
    out->view = "object";
    if (s->tab == STUDIO_TAB_OBJECT) {
        out->identity = subject->multi ? "selection" : "object";
        if (subject->multi) {
            selection_identity(v);
        } else {
            object_identity(v, subject);
        }
    }
    if (s->tab == STUDIO_TAB_BEHAVIORS) {
        out->behaviors = true;
        behaviors(v, subject);
        return;
    }
    out->add = actor->identity && !subject->multi;
    add_button(v, subject);
    for (int i = 0; i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        bool shared = true;
        for (int a = 0; subject->multi && a < acount(scene->actors); a++) {
            shared &= !scene_instance_selected(scene, a) ||
                      ecs_component_has(scene->world, scene->actors[a].entity, desc->id);
        }
        if (shared && ecs_component_has(scene->world, actor->entity, desc->id) &&
            properties_tab_of(s, desc) == s->tab) {
            /* Values edit the running world while the scene runs. */
            component_panel(v, actor, desc, subject->locked && !studio_live(s));
        }
    }
}

static void body(view *v, const properties_subject *subject)
{
    studio *s = v->s;
    shown out = { .view = "empty", .identity = "", .note = "" };
    if (s->tab == STUDIO_TAB_NODE || s->tab == STUDIO_TAB_MEMBER) {
        /* The Node and Member tabs are graph_details' (workstream G1). */
        out.view = "graph";
    } else if (s->tab == STUDIO_TAB_SCENE) {
        const char *why = properties_while_playing(s, subject->locked, "Stop playing to change scene settings");
        out = (shown){ .view = "scene", .identity = "", .note = why ? why : "" };
        scene_panels(v, subject->locked);
    } else if (subject->asset && subject->asset->prototype) {
        out = (shown){ .view = "prototype", .identity = "", .note = "", .add = true };
        if (s->tab == STUDIO_TAB_OBJECT) {
            out.identity = "prototype";
            show_prototype_identity(v, subject->asset->prototype);
        }
        prototype_panels(v, subject->asset, subject->locked);
        add_button(v, subject);
    } else if (subject->actor) {
        object_body(v, subject, &out);
    }
    put_text(v, v->root, "view", out.view);
    put_text(v, member(v->root, "identity"), "kind", out.identity);
    put_text(v, v->root, "note", out.note);
    put_flag(v, member(v->root, "behaviors"), "shown", out.behaviors);
    put_flag(v, member(v->root, "add"), "shown", out.add);
    hide_panels(v);
}

/* ---------- Drops ---------- */

bool properties_drop_over(studio *s, char row[PROPERTIES_ROW_ID], char *asset, size_t size)
{
    rml_context *context = rml_backend_context(s->backend);
    ui_drag_item item;
    if (!context || !ui_drag_active(context, &item)) {
        return false;
    }
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *field = rml_element_query_selector(
            rml_context_document(context, i),
            "[data-ui=Properties] .asset-field[data-drop-over]"
        );
        if (field) {
            rml_element_attribute(field, "data-row", row, PROPERTIES_ROW_ID);
            snprintf(asset, size, "%s", item.payload);
            return true;
        }
    }
    return false;
}

/* ---------- Sync ---------- */

bool studio_properties_sync(studio *s, const studio_panel_sync *at)
{
    view v = { .s = s,
               .doc = at->doc,
               .root = at->variable,
               .values = member(yyjson_mut_doc_get_root(at->doc), "propertyValues") };
    /* The search is the panel's own: its field writes it. */
    if (at->wrote && !at->preview) {
        snprintf(
            s->property_search,
            sizeof s->property_search,
            "%s",
            yyjson_mut_get_str(member(v.root, "query"))
        );
    }
    properties_subject subject;
    properties_subject_of(s, &subject);
    bool available[STUDIO_TAB_COUNT];
    properties_follow_tabs(s, &subject, available);
    header(&v);
    tabs(&v, available);
    body(&v, &subject);
    /* This menu has no submenus. The popover stack owns openness, including
       dismissal before the closing animation has shed data-open. */
    rml_element *popup = at->preview ? NULL
                                     : popover_top(rml_backend_context(s->backend));
    if (at->preview || (popup && !strcmp(rml_element_id(popup), "field-menu"))) {
        field_menu(&v, &subject);
    }
    component_menu(&v, &subject);
    afree(v.shown);
    if (v.values_changed) {
        rml_data_model_dirty(at->model, "propertyValues");
    }
    arena_free(&v.storage);
    return v.changed;
}
