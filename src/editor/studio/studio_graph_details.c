/* Properties' Node and Member tabs (workstream G1): the `graph_details`
 * variable Properties.rml's graph section binds, and the intents its rows
 * send. The Node tab (studio_graph_properties.c) shows the selected node's
 * pins and settings or a comment frame's; the Member tab
 * (studio_member_properties.c) the member Members selected. Each writes
 * panels of rows through the writers below, which keep their items, so a
 * selection rebinds rows rather than building them, as P1's view does.
 *
 *   graph_values: [text]          numbers, by slot (a scrub dirties these alone)
 *   graph_details_ui: { source, popup }  the row whose source list or popover is open
 *   graph_details: { view, empty, containers: TABLE,
 *     identity: { icon, name, of },
 *     panels: [{ key, title, open, shown, add, addId, addOff, rows: [ROW] }] }
 *   ROW: { id, label, tip, about, control, def, text, placeholder, check,
 *          name, type, swatch, color, error, kind, drop, problem, value,
 *          shown, overridden, readonly, invalid, checked, integer, float,
 *          alpha, removable, heading, fixed, bindable, slot, alphaSlot,
 *          precision, container, min, max, step, axes: [AXIS], table: TABLE,
 *          sub: [ROW] }
 *   AXIS: { key, label, slot, min, max, step, precision }
 *   TABLE: { label, icon, none, key, groups: [{ label, options: [{ value, label, icon }] }] }
 *
 * `control` names the row's control (GraphField.rml): label, chip, text,
 * area, bool, number, vector, choice, color, asset, source; the graph section
 * itself builds expression, type, struct, array and param rows. A param row's
 * default is the control `def` names, over the row's own value members.
 * `sub` holds a popover's rows: a struct's fields, an array's items or a
 * folded source's inputs. Intents: graph_details.<action> with a row id,
 * whose first letter names its owner: n (a node's), c (a comment frame's),
 * v, g or l (a variable's, a function's or macro's, a named value's). */
#include "authoring/reference_index.h"
#include "editor/studio/studio_members_internal.h"
#include "editor/ui/choices.h"
#include "support/array.h"
#include "ui/popover.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frames Bind to Source waits for a row to show its source field. */
#define BIND_FRAMES 8

/* Bytes a number's text member owns; a write rewrites it in place. */
#define ROOM 32

struct gd_view
{
    studio *s;
    uint32_t icon; /* the heading, written once the tab has named it */
    char name[256], of[160], empty[64];
    yyjson_mut_doc *doc;
    yyjson_mut_val *root;
    yyjson_mut_val *values; /* graph_values */
    yyjson_mut_val **shown; /* the panels written this sync */
    int slots;              /* the values written this sync */
    bool changed, values_changed;
};

/* ---------- Members, written in place ---------- */

static yyjson_mut_val *member(yyjson_mut_val *obj, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    assert(v);
    return v;
}

static void put_text(gd_view *v, yyjson_mut_val *obj, const char *key, const char *text)
{
    v->changed |= ui_data_put_text(v->doc, obj, key, text ? text : "");
}

static void put_flag(gd_view *v, yyjson_mut_val *obj, const char *key, bool on)
{
    v->changed |= ui_data_put_bool(obj, key, on);
}

static void put_int(gd_view *v, yyjson_mut_val *obj, const char *key, int n)
{
    v->changed |= ui_data_put_int(obj, key, n);
}

static void put_glyph(gd_view *v, yyjson_mut_val *obj, const char *key, uint32_t icon)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    put_text(v, obj, key, glyph);
}

/* Rewrites the room `m` owns with `text`; true when it differed. */
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

static yyjson_mut_val *room_new(yyjson_mut_doc *doc)
{
    static const char BLANK[ROOM];
    yyjson_mut_val *room = yyjson_mut_strncpy(doc, BLANK, ROOM - 1);
    yyjson_mut_set_strn(room, yyjson_mut_get_str(room), 0);
    return room;
}

/* A number as text a control reads back exactly. */
static void number_text(char text[ROOM], double number, bool single)
{
    if (isinf(number)) {
        snprintf(text, ROOM, "%s", number < 0 ? "-inf" : "inf");
        return;
    }
    snprintf(text, ROOM, single ? "%.9g" : "%.17g", number);
}

static void put_number(gd_view *v, yyjson_mut_val *obj, const char *key, double number)
{
    char text[ROOM];
    number_text(text, number, false);
    v->changed |= set_room(member(obj, key), text);
}

/* The number's text in the next slot of graph_values, which `obj` names. */
static void put_value(gd_view *v, yyjson_mut_val *obj, const char *key, double number, bool single)
{
    int slot = v->slots++;
    put_int(v, obj, key, slot);
    if ((size_t)slot == yyjson_mut_arr_size(v->values)) {
        yyjson_mut_arr_append(v->values, room_new(v->doc));
        v->values_changed = true;
    }
    char text[ROOM];
    number_text(text, number, single);
    v->values_changed |= set_room(yyjson_mut_arr_get(v->values, (size_t)slot), text);
}

/* ---------- Shapes ---------- */

static const char *const ROW_TEXTS[] = {
    "id",          "label", "tip",     "about", "control", "def",   "text",
    "placeholder", "check", "name",    "type",  "swatch",  "color", "error",
    "kind",        "drop",  "problem", "value", NULL
};
static const char *const ROW_FLAGS[] = { "shown",   "overridden", "readonly",
                                         "invalid", "checked",    "integer",
                                         "float",   "alpha",      "removable",
                                         "heading", "fixed",      "bindable",
                                         NULL };
static const char *const ROW_INTS[] = { "slot", "alphaSlot", "precision", "container", NULL };
static const char *const ROOMS[] = { "min", "max", "step", NULL };

static void add_texts(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys)
{
    for (; *keys; keys++) {
        yyjson_mut_obj_add_strcpy(doc, obj, *keys, "");
    }
}

static void add_flags(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys)
{
    for (; *keys; keys++) {
        yyjson_mut_obj_add_bool(doc, obj, *keys, false);
    }
}

static void add_ints(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys)
{
    for (; *keys; keys++) {
        yyjson_mut_obj_add_int(doc, obj, *keys, 0);
    }
}

static void add_rooms(yyjson_mut_doc *doc, yyjson_mut_val *obj)
{
    for (const char *const *key = ROOMS; *key; key++) {
        yyjson_mut_obj_add_val(doc, obj, *key, room_new(doc));
    }
}

static yyjson_mut_val *table_new(yyjson_mut_doc *doc, yyjson_mut_val *obj)
{
    static const char *const TEXTS[] = { "label", "icon", "none", "key", NULL };
    yyjson_mut_val *table = yyjson_mut_obj_add_obj(doc, obj, "table");
    add_texts(doc, table, TEXTS);
    yyjson_mut_obj_add_arr(doc, table, "groups");
    return table;
}

static yyjson_mut_val *row_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    add_texts(doc, row, ROW_TEXTS);
    add_flags(doc, row, ROW_FLAGS);
    add_ints(doc, row, ROW_INTS);
    add_rooms(doc, row);
    yyjson_mut_obj_add_arr(doc, row, "axes");
    yyjson_mut_obj_add_arr(doc, row, "sub");
    table_new(doc, row);
    return row;
}

static yyjson_mut_val *axis_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "key", "label", "unit", NULL };
    static const char *const FLAGS[] = { "slider", "mixed", "overridden", NULL };
    static const char *const INTS[] = { "slot", "precision", NULL };
    yyjson_mut_val *axis = yyjson_mut_obj(doc);
    add_texts(doc, axis, TEXTS);
    add_flags(doc, axis, FLAGS);
    add_ints(doc, axis, INTS);
    add_rooms(doc, axis);
    return axis;
}

static yyjson_mut_val *panel_new(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "key", "title", "add", "addId", NULL };
    static const char *const FLAGS[] = { "shown", "open", "addOff", NULL };
    yyjson_mut_val *panel = yyjson_mut_obj(doc);
    add_texts(doc, panel, TEXTS);
    add_flags(doc, panel, FLAGS);
    yyjson_mut_obj_add_arr(doc, panel, "rows");
    return panel;
}

static void add_option(yyjson_mut_doc *doc, yyjson_mut_val *options, int value, const char *label, uint32_t icon)
{
    yyjson_mut_val *option = yyjson_mut_arr_add_obj(doc, options);
    yyjson_mut_obj_add_int(doc, option, "value", value);
    yyjson_mut_obj_add_strcpy(doc, option, "label", label);
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    yyjson_mut_obj_add_strcpy(doc, option, "icon", glyph);
}

void studio_graph_details_add(yyjson_mut_doc *doc)
{
    static const char *const IDENTITY[] = { "icon", "name", "of", NULL };
    static const char *const CONTAINERS[] = { "Single", "Array", "Set", "Map" };
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_obj_add_arr(doc, root, "graph_values");
    yyjson_mut_val *ui = yyjson_mut_obj_add_obj(doc, root, "graph_details_ui");
    yyjson_mut_obj_add_strcpy(doc, ui, "source", "");
    yyjson_mut_obj_add_strcpy(doc, ui, "popup", "");
    yyjson_mut_val *d = yyjson_mut_obj_add_obj(doc, root, "graph_details");
    yyjson_mut_obj_add_strcpy(doc, d, "view", "");
    yyjson_mut_obj_add_strcpy(doc, d, "empty", "");
    add_texts(doc, yyjson_mut_obj_add_obj(doc, d, "identity"), IDENTITY);
    yyjson_mut_obj_add_arr(doc, d, "panels");
    /* A container's choices, which every param row shares. */
    yyjson_mut_val *containers = table_new(doc, yyjson_mut_obj_add_obj(doc, d, "containers"));
    yyjson_mut_val *group = yyjson_mut_arr_add_obj(doc, member(containers, "groups"));
    yyjson_mut_obj_add_strcpy(doc, group, "label", "");
    yyjson_mut_val *options = yyjson_mut_obj_add_arr(doc, group, "options");
    for (int i = 0; i < 4; i++) {
        add_option(doc, options, i, CONTAINERS[i], 0);
    }
}

/* Lists keep their items, so data-for keeps their elements. */
static yyjson_mut_val *item_at(
    gd_view *v,
    yyjson_mut_val *list,
    size_t i,
    yyjson_mut_val *(*make)(yyjson_mut_doc *)
)
{
    if (i < yyjson_mut_arr_size(list)) {
        return yyjson_mut_arr_get(list, i);
    }
    yyjson_mut_val *item = make(v->doc);
    yyjson_mut_arr_append(list, item);
    v->changed = true;
    return item;
}

static void trim(gd_view *v, yyjson_mut_val *list, size_t count)
{
    while (yyjson_mut_arr_size(list) > count) {
        yyjson_mut_arr_remove_last(list);
        v->changed = true;
    }
}

/* ---------- The heading ---------- */

void gd_identity(gd_view *v, uint32_t icon, const char *name, const char *of)
{
    v->icon = icon;
    snprintf(v->name, sizeof v->name, "%s", name);
    snprintf(v->of, sizeof v->of, "%s", of);
}

void gd_empty(gd_view *v, const char *text)
{
    snprintf(v->empty, sizeof v->empty, "%s", text);
}

/* Writes the heading and the empty state, once a sync. */
static void put_heading(gd_view *v)
{
    yyjson_mut_val *id = member(v->root, "identity");
    put_glyph(v, id, "icon", v->icon);
    put_text(v, id, "name", v->name);
    put_text(v, id, "of", v->of);
    put_text(v, v->root, "empty", v->empty);
}

/* ---------- Panels ---------- */

static studio_fold *fold_of(studio *s, const char *key)
{
    for (int i = 0; i < acount(s->graph.details.folds); i++) {
        if (!strcmp(s->graph.details.folds[i].key, key)) {
            return &s->graph.details.folds[i];
        }
    }
    return NULL;
}

/* A panel opens first, as the kit's did; its fold holds for the session. */
static bool panel_open(studio *s, const char *key)
{
    const studio_fold *fold = fold_of(s, key);
    return !fold || fold->open;
}

static void fold(studio *s, const char *key)
{
    studio_fold *f = fold_of(s, key);
    if (!f) {
        apush(s->graph.details.folds, ((studio_fold){ .open = true }));
        f = &alast(s->graph.details.folds);
        snprintf(f->key, sizeof f->key, "%s", key);
    }
    f->open = !f->open;
}

/* The slot of panel `key`, kept for the session so its rows keep their
   elements from one selection to the next. */
static yyjson_mut_val *panel_slot(gd_view *v, const char *key)
{
    yyjson_mut_val *list = member(v->root, "panels");
    for (size_t i = 0; i < yyjson_mut_arr_size(list); i++) {
        yyjson_mut_val *panel = yyjson_mut_arr_get(list, i);
        if (!strcmp(yyjson_mut_get_str(member(panel, "key")), key)) {
            return panel;
        }
    }
    yyjson_mut_val *panel = panel_new(v->doc);
    put_text(v, panel, "key", key);
    yyjson_mut_arr_append(list, panel);
    v->changed = true;
    return panel;
}

gd_rows gd_panel(gd_view *v, const char *key, const char *title)
{
    yyjson_mut_val *panel = panel_slot(v, key);
    /* Panels show in the order written: a slot written again moves last. */
    yyjson_mut_val *list = member(v->root, "panels");
    size_t at = (size_t)acount(v->shown);
    if (yyjson_mut_arr_get(list, at) != panel) {
        for (size_t i = 0; i < yyjson_mut_arr_size(list); i++) {
            if (yyjson_mut_arr_get(list, i) == panel) {
                yyjson_mut_arr_remove(list, i);
                break;
            }
        }
        yyjson_mut_arr_insert(list, panel, at);
        v->changed = true;
    }
    apush(v->shown, panel);
    bool open = panel_open(v->s, key);
    put_flag(v, panel, "shown", true);
    put_text(v, panel, "title", title);
    put_flag(v, panel, "open", open);
    put_text(v, panel, "add", "");
    return (gd_rows){ .v = v, .list = member(panel, "rows"), .panel = panel, .open = open };
}

void gd_panel_add(gd_rows *r, const char *id, const char *label, bool disabled)
{
    put_text(r->v, r->panel, "addId", id);
    put_text(r->v, r->panel, "add", label);
    put_flag(r->v, r->panel, "addOff", disabled);
}

/* Rows past those written hide and keep their elements for the next
   selection to show; a folded panel's go. */
void gd_panel_end(gd_rows *r)
{
    if (!r->open) {
        trim(r->v, r->list, 0);
        return;
    }
    for (size_t i = r->count; i < yyjson_mut_arr_size(r->list); i++) {
        put_flag(r->v, yyjson_mut_arr_get(r->list, i), "shown", false);
    }
}

/* ---------- Rows ---------- */

yyjson_mut_val *gd_next_row(gd_rows *r, const gd_row *d)
{
    gd_view *v = r->v;
    yyjson_mut_val *row = item_at(v, r->list, r->count++, row_new);
    put_flag(v, row, "shown", true);
    put_text(v, row, "id", d->id);
    put_text(v, row, "label", d->label);
    put_text(v, row, "tip", d->about && d->about[0] ? d->label : "");
    put_text(v, row, "about", d->about);
    put_text(v, row, "control", d->control);
    put_text(v, row, "def", d->def);
    put_flag(v, row, "readonly", d->readonly);
    put_flag(v, row, "invalid", d->invalid);
    put_flag(v, row, "heading", d->heading);
    put_flag(v, row, "removable", d->removable);
    put_flag(v, row, "fixed", d->fixed);
    put_flag(v, row, "bindable", d->bindable);
    return row;
}

void gd_put_text(gd_rows *r, yyjson_mut_val *row, const char *key, const char *text)
{
    put_text(r->v, row, key, text);
}

void gd_put_flag(gd_rows *r, yyjson_mut_val *row, const char *key, bool on)
{
    put_flag(r->v, row, key, on);
}

void gd_put_int(gd_rows *r, yyjson_mut_val *row, const char *key, int n)
{
    put_int(r->v, row, key, n);
}

static void put_shape(gd_view *v, yyjson_mut_val *obj, const gd_number_shape *shape)
{
    put_number(v, obj, "min", shape->min);
    put_number(v, obj, "max", shape->max);
    put_number(v, obj, "step", shape->step);
    put_int(v, obj, "precision", shape->precision);
}

void gd_put_number(gd_rows *r, yyjson_mut_val *row, double value, const gd_number_shape *shape)
{
    gd_view *v = r->v;
    put_value(v, row, "slot", value, shape->single);
    put_shape(v, row, shape);
    put_flag(v, row, "integer", shape->integer);
    put_flag(v, row, "float", shape->single);
}

void gd_put_alpha(gd_rows *r, yyjson_mut_val *row, double alpha)
{
    put_value(r->v, row, "alphaSlot", alpha, false);
}

void gd_put_axes(gd_rows *r, yyjson_mut_val *row, const double *values, int count, const gd_number_shape *shape)
{
    static const char *const LABELS[] = { "X", "Y", "Z", "W" };
    static const char *const KEYS[] = { "x", "y", "z", "w" };
    gd_view *v = r->v;
    yyjson_mut_val *list = member(row, "axes");
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *axis = item_at(v, list, (size_t)i, axis_new);
        put_text(v, axis, "key", KEYS[i]);
        put_text(v, axis, "label", LABELS[i]);
        put_value(v, axis, "slot", values[i], shape->single);
        put_shape(v, axis, shape);
    }
    trim(v, list, (size_t)count);
    put_flag(v, row, "float", shape->single);
}

/* Rewrites `table`'s groups when its key, what they list, changed. */
static yyjson_mut_val *table_of(gd_view *v, yyjson_mut_val *row, const char *key, bool *rebuild)
{
    yyjson_mut_val *table = member(row, "table");
    *rebuild = strcmp(yyjson_mut_get_str(member(table, "key")), key) != 0;
    if (*rebuild) {
        put_text(v, table, "key", key);
        yyjson_mut_arr_clear(member(table, "groups"));
        v->changed = true;
    }
    return table;
}

static yyjson_mut_val *group_options(gd_view *v, yyjson_mut_val *table, const char *label)
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

/* A choice lists its options once opened, else only the current one, which
   its closed field shows; each option's value is its place. */
void gd_put_choices(gd_rows *r, yyjson_mut_val *row, const editor_choices *choices, int current)
{
    int count = editor_choices_count(choices);
    gd_view *v = r->v;
    const char *id = yyjson_mut_get_str(member(row, "id"));
    bool open = !strcmp(v->s->graph.details.choosing, id);
    char value[16], key[160];
    snprintf(value, sizeof value, "%d", current);
    put_text(v, row, "value", value);
    snprintf(key, sizeof key, "%s:%s:%d", id, open ? "*" : value, count);
    bool rebuild;
    yyjson_mut_val *table = table_of(v, row, key, &rebuild);
    for (int i = 0; rebuild && i < count; i++) {
        editor_choice choice = editor_choice_at(choices, i);
        if (open || i == current) {
            yyjson_mut_val
                *options = group_options(v, table, choice.group ? choice.group : "");
            add_option(
                v->doc,
                options,
                i,
                choice.label ? choice.label : choice.name,
                choice.icon
            );
        }
    }
}

static void gd_put_table(gd_rows *r, yyjson_mut_val *row, const char *label, uint32_t icon, const char *none)
{
    yyjson_mut_val *table = member(row, "table");
    put_text(r->v, table, "label", label);
    put_glyph(r->v, table, "icon", icon);
    put_text(r->v, table, "none", none);
}

/* An asset field: its kind's picker (palette.pick), drops of its kind, and
   the problem when no asset has its name. Events are names: host, action
   and ui events have no asset. */
bool gd_asset_problem(studio *s, reference_asset_kind kind, const char *value, char problem[192])
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    bool named = value[0] && index && kind != REFERENCE_ASSET_EVENT;
    const reference_asset *found = named
                                     ? reference_index_asset_at(
                                           index,
                                           reference_index_find_name(index, kind, value)
                                       )
                                     : NULL;
    problem[0] = '\0';
    if (named && (!found || found->unresolved)) {
        snprintf(problem, 192, "No %s asset is named %s", studio_asset_kind_name(kind), value);
    }
    return problem[0] != '\0';
}

void gd_put_asset(gd_rows *r, yyjson_mut_val *row, reference_asset_kind kind, const char *value)
{
    gd_view *v = r->v;
    char problem[192];
    gd_asset_problem(v->s, kind, value, problem);
    bool readonly = yyjson_mut_get_bool(member(row, "readonly"));
    put_text(v, row, "problem", problem);
    put_text(v, row, "kind", studio_asset_kind_name(kind));
    put_text(v, row, "drop", readonly || kind == REFERENCE_ASSET_EVENT ? "" : studio_asset_kind_name(kind));
    gd_put_table(r, row, studio_asset_kind_label(kind), studio_asset_kind_icon(kind), "None");
}

/* A type's swatch, a dot colored as the canvas colors its sockets: a
   circle, a square for a container, an arrow for execution. */
void gd_put_type(gd_rows *r, yyjson_mut_val *row, const pin_spec *pin, const char *label)
{
    char color[16];
    snprintf(color, sizeof color, "#%08x", sg_pin_color(r->v->s, pin));
    put_text(
        r->v,
        row,
        "swatch",
        pin->kind == PIN_EXEC                    ? "arrow"
        : pin->container == PIN_CONTAINER_SINGLE ? "circle"
                                                 : "square"
    );
    put_text(r->v, row, "color", color);
    put_text(r->v, row, "type", label);
}

/* A popover's rows: a struct's fields, an array's items or a folded source's
   inputs. */
gd_rows gd_sub(gd_rows *r, yyjson_mut_val *row)
{
    return (gd_rows){ .v = r->v, .list = member(row, "sub"), .open = true };
}

void gd_sub_end(gd_rows *r)
{
    trim(r->v, r->list, r->count);
}

/* ---------- Sync ---------- */

/* What the tabs show follows the tab, the selection, the graph and its
   edits, whether it takes edits, the open choice, source list and popover,
   the folds and what intents changed; a host's model is its own. */
static uint64_t inputs_key(const studio *s, const studio_panel_sync *at)
{
    const studio_graph *g = &s->graph;
    const editor *e = studio_graph_active(s) ? sg_graph_editor(s) : NULL;
    const graph *gr = e ? sg_active_graph(s) : NULL;
    const uint64_t parts[] = {
        s->tab,
        studio_graph_node(s),
        studio_graph_comment(s),
        s->members.kind,
        s->members.id,
        (uintptr_t)(e ? e->asset : NULL),
        e ? editor_edit_revision(e) : 0,
        gr ? gr->lifetime_id : 0,
        gr ? graph_topology_revision(gr) : 0,
        e ? sg_graph_readonly(s) : 0,
        s->scene->asset_index_generation,
        g->details.revision,
        (uintptr_t)at->doc,
    };
    uint64_t key = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        key = (key ^ parts[i]) * 1099511628211ull;
    }
    return key;
}

static void hide_panels(gd_view *v)
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

bool studio_graph_details_sync(studio *s, const studio_panel_sync *at)
{
    studio_graph *g = &s->graph;
    bool node = s->tab == STUDIO_TAB_NODE, shown = node || s->tab == STUDIO_TAB_MEMBER;
    uint64_t key = inputs_key(s, at);
    if (at->preview || !shown || key == g->details.key) {
        return false;
    }
    g->details.key = key;
    gd_view v = { .s = s,
                  .doc = at->doc,
                  .root = at->variable,
                  .values = member(yyjson_mut_doc_get_root(at->doc), "graph_values") };
    if (!studio_graph_active(s)) {
        gd_empty(&v, node ? "No node selected" : "No member selected");
    } else if (node) {
        studio_node_details(s, &v);
    } else {
        studio_member_details(s, &v);
    }
    put_text(&v, v.root, "view", node ? "node" : "member");
    put_heading(&v);
    hide_panels(&v);
    /* A field whose edit was put back, or whose list closed, shows the
       model's text again: views compare with what the field holds. */
    v.changed |= g->details.redraw;
    g->details.redraw = false;
    afree(v.shown);
    yyjson_mut_val *ui = member(yyjson_mut_doc_get_root(at->doc), "graph_details_ui");
    bool opened = ui_data_put_text(at->doc, ui, "source", g->details.source);
    opened |= ui_data_put_text(at->doc, ui, "popup", g->details.popup);
    if (opened) {
        rml_data_model_dirty(at->model, "graph_details_ui");
    }
    if (v.values_changed) {
        rml_data_model_dirty(at->model, "graph_values");
    }
    return v.changed;
}

/* ---------- Intents ---------- */

void gd_changed(studio *s)
{
    s->graph.details.revision++;
}

/* A text field's Escape keeps the model's text, as Enter would have
   committed it. */
static void cancel(studio *s, const ui_activation *a)
{
    rml_element *field = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    s->graph.details.redraw = true;
    if (field) {
        rml_element_blur(field);
    }
}

/* The source list: query, the field's text as typed, which opens the list
   when it is shut; escape shuts it, keeping the source; tab chooses the
   highlighted result, as the canvas's field does. */
static bool source_action(studio *s, const char *action, const char *id, const ui_activation *a)
{
    bool listing = s->palette.client == STUDIO_PALETTE_SOURCES &&
                   !strcmp(s->graph.details.source, id);
    if (!strcmp(action, "query")) {
        if (!listing) {
            studio_node_details_action(s, "source", a);
        }
        studio_palette_set_query(s, a->argc > 1 ? a->args[1] : "");
        return true;
    }
    if (!strcmp(action, "escape")) {
        if (listing) {
            studio_palette_close(s);
        }
        return true;
    }
    if (!strcmp(action, "tab")) {
        if (listing && acount(s->palette.places)) {
            studio_palette_action(
                s,
                "choose",
                &(ui_activation){ .has_value = true,
                                  .value = (float)(s->palette.highlight + 1) }
            );
        }
        return true;
    }
    return false;
}

/* input and change: a control's value as its gesture runs and as it
   commits, with ev.value, ev.canceled and ev.name; check: a checkbox's
   state; choices: a choice opening, which lists its options; fold: a
   panel's head; cancel: Escape in a text field; source, query, escape and
   tab: an input's source field; the rest are the owner's. Keys a field
   binds send its id, the row's. */
void studio_graph_details_action(studio *s, const char *action, const ui_activation *a)
{
    const char *id = a->argc ? a->args[0] : a->id;
    gd_changed(s);
    if (!strcmp(action, "cancel")) {
        cancel(s, a);
    }
    if (source_action(s, action, id, a)) {
        return;
    }
    if (!strcmp(action, "fold")) {
        fold(s, id);
        return;
    }
    if (!strcmp(action, "choices")) {
        snprintf(s->graph.details.choosing, sizeof s->graph.details.choosing, "%s", id);
        return;
    }
    if (!studio_graph_active(s) || !id[0]) {
        return;
    }
    if (!strcmp(action, "addPin")) {
        studio_member_details_action(s, action, a);
        return;
    }
    switch (id[0]) {
        case 'n':
        case 'c': studio_node_details_action(s, action, a); break;
        case 'v':
        case 'g':
        case 'l': studio_member_details_action(s, action, a); break;
        default: break;
    }
}

/* ---------- Popovers ---------- */

/* The element with `id` in Studio's documents, or NULL. */
static rml_element *by_id(studio *s, const char *id)
{
    rml_context *context = rml_backend_context(s->backend);
    rml_element *found = NULL;
    for (int i = 0; !found && i < rml_context_document_count(context); i++) {
        found = rml_element_by_id(rml_context_document(context, i), id);
    }
    return found;
}

/* The popover `selector` names in the row whose field has id `row`. */
static rml_element *row_popover(rml_element *field, const char *selector)
{
    rml_element *row = field;
    while (row && !rml_element_is_class_set(row, "prop")) {
        row = rml_element_parent(row);
    }
    return row ? rml_element_query_selector(row, selector) : NULL;
}

/* Shows the row's popover `selector` names under the row's field once its
   document built it; true while it is open. */
static bool shown(studio *s, const char *selector, const char *row, bool *showing)
{
    rml_element *field = by_id(s, row);
    rml_element *popover = field ? row_popover(field, selector) : NULL;
    if (!*showing) {
        if (popover && field) {
            popover_show(
                popover,
                &(popover_anchor){ .side = POPOVER_BELOW, .element = field, .trigger = field }
            );
            *showing = true;
        }
        return field != NULL;
    }
    return popover && popover_open(popover);
}

/* Bind to Source in Properties ends as its source list closes. */
static void end_bind(studio *s)
{
    studio_graph *g = &s->graph;
    g->bind_pin = 0;
    g->bind_focus = false;
    g->details.bind[0] = '\0';
    g->details.redraw = true;
}

/* The source field Bind to Source swapped into its row takes the focus once
   the tab has built it, which opens its list. The row keeps its value
   editor when the field never shows, and returns to it when the field loses
   the focus without a list. */
static void bind_frame(studio *s)
{
    studio_graph *g = &s->graph;
    rml_element *field = by_id(s, g->details.bind);
    char role[16] = "";
    if (field) {
        rml_element_attribute(field, "role", role, sizeof role);
    }
    field = strcmp(role, "combobox") ? NULL : field;
    if (g->bind_focus && field) {
        g->bind_focus = false;
        rml_element_focus(field, true);
        return;
    }
    if (g->bind_focus && ++g->details.bind_wait >= BIND_FRAMES) {
        end_bind(s);
        return;
    }
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    if (!g->bind_focus && !g->details.source[0] && (!field || focus != field)) {
        end_bind(s);
    }
}

/* The source list shows while the palette lists the field's sources; it
   closes with the palette, a press outside it or its field leaving. */
static void source_frame(studio *s)
{
    studio_graph *g = &s->graph;
    bool listing = s->palette.client == STUDIO_PALETTE_SOURCES;
    if (listing &&
        shown(s, ".popup.autocomplete", g->details.source, &g->details.source_shown)) {
        return;
    }
    rml_element *field = by_id(s, g->details.source);
    rml_element *list = field ? row_popover(field, ".popup.autocomplete") : NULL;
    if (list && popover_open(list)) {
        popover_hide(list);
    }
    if (listing) {
        studio_palette_close(s);
    }
    if (!g->bind_kit && !strcmp(g->details.source, g->details.bind)) {
        end_bind(s);
    }
    g->details.source[0] = '\0';
    g->details.source_shown = false;
    g->details.redraw = true;
    gd_changed(s);
}

/* A folded source's inputs, under its field while Edit Inline Inputs
   holds them open. */
static void popup_frame(studio *s)
{
    studio_graph *g = &s->graph;
    if (shown(s, ".graph-inline-values", g->details.popup, &g->details.popup_shown)) {
        return;
    }
    g->details.popup[0] = '\0';
    g->details.popup_shown = false;
    if (!g->inline_edit_kit) {
        g->inline_edit_node = g->inline_edit_pin = 0;
    }
    gd_changed(s);
}

void studio_graph_details_frame(studio *s)
{
    studio_graph *g = &s->graph;
    if (g->details.bind[0] && !g->bind_kit) {
        bind_frame(s);
    }
    if (g->details.source[0]) {
        source_frame(s);
    }
    if (g->details.popup[0]) {
        popup_frame(s);
    }
}

void studio_graph_details_free(studio *s)
{
    afree(s->graph.details.folds);
    s->graph.details.folds = NULL;
    sg_source_free(s);
}
