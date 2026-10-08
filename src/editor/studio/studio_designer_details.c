/* Details (workstream H, authored in D1): the selected element's text,
 * attributes, data bindings and inline style, then the rules of the
 * document's RCSS that match it, as UI Builder's Inspector and UMG's
 * Details panel show a widget's. A value edits in the control its
 * property's type calls for: a length scrubs with its unit, a color picks,
 * a keyword chooses, and anything else is text. Each commit is a text edit
 * of the document, and a scrub or a picker session is one undo step. A
 * rule's declarations are added and removed in place, and Extract to Class
 * moves the inline style into a new rule (workstream Q). With nothing
 * selected, Details lists every rule of the document's RCSS.
 *
 * This file shapes data for the panel and turns its controls into calls;
 * what a value is, what the document holds and each change to it are
 * capabilities in plain C (studio_designer_edit.h), with no decisions here.
 *
 * Details.rml and DetailsHeader.rml bind two sources. `details` holds the
 * panels, each a head and rows (the shape the panel builds one control from):
 *
 *   details: { query, hint, note,
 *     identity: { icon, title, kind, source },
 *     sections: [{ key, title, kind, open, origin, within, none, rule,
 *                  addLabel, addKind, addTarget, extract, rows: [ROW] }] }
 *   ROW: { id, label, control, text, placeholder, remove,
 *          number, unit, step, min, max, precision, integer, slider,
 *          color, value, options: [{ label, pressed }],
 *          table: TABLE, chips: [{ name }] }
 *
 * `control` is text, number, color, choice (an EnumField over `table`),
 * toggle (a segmented control over `options`), asset (the widget a <use>
 * shows) or chips (a class list). A row's id names what it edits: "a<i>"
 * an attribute, "id", "text", "widget", "s:<property>" an inline style
 * property, "d<rule>.<declaration>" a declaration. `details_add` holds the
 * Add popover shared by every Add button: its title, hint, the text typed
 * and the suggestions.
 *
 * Intents: details.input and .change take a row's id and value (and
 * whether Escape canceled); .text a text field's commit (read from the
 * field); .remove a row; .removeClass a chip; .add opens the popover for a
 * kind and a target; .addPick and .addEnter add what it names; .fold a
 * panel; .source and .openRule open the Text Editor; .choices a keyword
 * list; .cancel puts a text field back. */
#include "editor/studio/studio_designer_edit.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "ui/popover.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What an intent works on. */
typedef struct
{
    studio *s;
    studio_designer *d;
    ui_buffer *b;
} details;

/* ---------- Add popovers ---------- */

static const char *const ADD_TITLES[] = {
    "Add Class",    "Add Attribute",    "Add Property",
    "Add Property", "Extract to Class",
};
static const char *const ADD_HINTS[] = {
    "Class name",      "name=value",     "property: value",
    "property: value", "New class name",
};

/* ---------- Data ---------- */

static yyjson_mut_val *member(yyjson_mut_val *object, const char *name)
{
    yyjson_mut_val *value = yyjson_mut_obj_get(object, name);
    assert(value);
    return value;
}

static void add_members(
    yyjson_mut_doc *doc,
    yyjson_mut_val *object,
    const char *const *texts,
    const char *const *flags,
    const char *const *ints
)
{
    for (int i = 0; texts && texts[i]; i++) {
        yyjson_mut_obj_add_strcpy(doc, object, texts[i], "");
    }
    for (int i = 0; flags && flags[i]; i++) {
        yyjson_mut_obj_add_bool(doc, object, flags[i], false);
    }
    for (int i = 0; ints && ints[i]; i++) {
        yyjson_mut_obj_add_int(doc, object, ints[i], 0);
    }
}

typedef enum
{
    ITEM_SECTION,
    ITEM_ROW,
    ITEM_CHIP,
    ITEM_OPTION,
    ITEM_SUGGESTION,
} item_kind;

static yyjson_mut_val *table_new(yyjson_mut_doc *doc, yyjson_mut_val *object)
{
    static const char *const TEXTS[] = { "label", "icon", "none", "key", NULL };
    yyjson_mut_val *table = yyjson_mut_obj_add_obj(doc, object, "table");
    add_members(doc, table, TEXTS, NULL, NULL);
    yyjson_mut_obj_add_arr(doc, table, "groups");
    return table;
}

static yyjson_mut_val *item_new(yyjson_mut_doc *doc, item_kind kind)
{
    static const char *const SECTION_TEXTS[] = { "key",      "title",   "kind",
                                                 "origin",   "within",  "none",
                                                 "addLabel", "addKind", NULL };
    static const char *const SECTION_FLAGS[] = { "open", "extract", NULL };
    static const char *const SECTION_INTS[] = { "rule", "addTarget", NULL };
    static const char *const ROW_TEXTS[] = { "id",          "label",   "tip",
                                             "about",       "control", "text",
                                             "placeholder", "number",  "unit",
                                             "step",        "min",     "max",
                                             "color",       "value",   NULL };
    static const char *const ROW_FLAGS[] = { "remove", "overridden", "integer", "slider", NULL };
    static const char *const ROW_INTS[] = { "precision", NULL };
    static const char *const CHIP_TEXTS[] = { "name", "label", NULL };
    static const char *const OPTION_FLAGS[] = { "pressed", "last", NULL };
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    switch (kind) {
        case ITEM_SECTION:
            add_members(doc, item, SECTION_TEXTS, SECTION_FLAGS, SECTION_INTS);
            yyjson_mut_obj_add_arr(doc, item, "rows");
            break;
        case ITEM_ROW:
            add_members(doc, item, ROW_TEXTS, ROW_FLAGS, ROW_INTS);
            yyjson_mut_obj_add_arr(doc, item, "options");
            yyjson_mut_obj_add_arr(doc, item, "chips");
            table_new(doc, item);
            break;
        case ITEM_CHIP: add_members(doc, item, CHIP_TEXTS, NULL, NULL); break;
        case ITEM_OPTION:
            add_members(doc, item, CHIP_TEXTS, OPTION_FLAGS, NULL);
            break;
        case ITEM_SUGGESTION:
            add_members(doc, item, CHIP_TEXTS, NULL, NULL);
            break;
    }
    return item;
}

void studio_details_add(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "query", "hint", "note", NULL };
    static const char *const IDENTITY_TEXTS[] = { "icon", "title", "kind", "source", NULL };
    yyjson_mut_val *details = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "details");
    add_members(doc, details, TEXTS, NULL, NULL);
    add_members(doc, yyjson_mut_obj_add_obj(doc, details, "identity"), IDENTITY_TEXTS, NULL, NULL);
    yyjson_mut_obj_add_arr(doc, details, "sections");
}

void studio_details_popover_add(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "title", "hint", "text", NULL };
    yyjson_mut_val *add = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "details_add");
    add_members(doc, add, TEXTS, NULL, NULL);
    yyjson_mut_obj_add_arr(doc, add, "suggestions");
}

/* ---------- Data, written in place ---------- */

/* The panel being written: the lists keep their items, so data-for keeps
 * their elements, and every write is in place. */
typedef struct
{
    studio *s;
    studio_designer *d;
    ui_buffer *b;
    yyjson_mut_doc *doc;
    yyjson_mut_val *sections;
    size_t count; /* sections written */
    bool changed;
} view;

static void put_text(view *v, yyjson_mut_val *object, const char *key, const char *text)
{
    v->changed |= ui_data_put_text(v->doc, object, key, text);
}

static void put_flag(view *v, yyjson_mut_val *object, const char *key, bool on)
{
    v->changed |= ui_data_put_bool(object, key, on);
}

static void put_int(view *v, yyjson_mut_val *object, const char *key, int number)
{
    v->changed |= ui_data_put_int(object, key, number);
}

/* The item at `i`, added when the list is shorter. */
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

static void trim(view *v, yyjson_mut_val *list, size_t count)
{
    while (yyjson_mut_arr_size(list) > count) {
        yyjson_mut_arr_remove_last(list);
        v->changed = true;
    }
}

static bool is_shut(const studio_designer *d, const char *key)
{
    for (int i = 0; i < acount(d->shut); i++) {
        if (!strcmp(d->shut[i], key)) {
            return true;
        }
    }
    return false;
}

static void fold_panel(studio_designer *d, const char *key)
{
    for (int i = 0; i < acount(d->shut); i++) {
        if (!strcmp(d->shut[i], key)) {
            free(d->shut[i]);
            d->shut[i] = d->shut[acount(d->shut) - 1];
            asetlen(d->shut, acount(d->shut) - 1);
            d->shuts++;
            return;
        }
    }
    apush(d->shut, xstrdup(key));
    d->shuts++;
}

void designer_details_forget(studio_designer *d)
{
    for (int i = 0; i < acount(d->shut); i++) {
        free(d->shut[i]);
    }
    afree(d->shut);
}

/* A section being written: its rows go to `list`. */
typedef struct
{
    view *v;
    yyjson_mut_val *section, *list;
    size_t count;
} rows;

static rows begin_section(view *v, const char *key, const char *title, const char *kind)
{
    yyjson_mut_val *section = item_at(v, v->sections, v->count++, ITEM_SECTION);
    put_text(v, section, "key", key);
    put_text(v, section, "title", title);
    put_text(v, section, "kind", kind);
    put_flag(v, section, "open", !is_shut(v->d, key));
    static const char *const CLEARED[] = { "origin", "within", "none", "addLabel", "addKind" };
    for (size_t i = 0; i < sizeof CLEARED / sizeof *CLEARED; i++) {
        put_text(v, section, CLEARED[i], "");
    }
    put_flag(v, section, "extract", false);
    put_int(v, section, "rule", 0);
    put_int(v, section, "addTarget", 0);
    return (rows){ v, section, member(section, "rows"), 0 };
}

static void end_section(rows *r)
{
    trim(r->v, r->list, r->count);
}

static yyjson_mut_val *next_row(rows *r, const char *id, const char *label, const char *control)
{
    view *v = r->v;
    yyjson_mut_val *row = item_at(v, r->list, r->count++, ITEM_ROW);
    put_text(v, row, "id", id);
    put_text(v, row, "label", label);
    put_text(v, row, "control", control);
    put_flag(v, row, "remove", false);
    return row;
}

static void limit_text(char out[24], double limit)
{
    if (isinf(limit)) {
        snprintf(out, 24, "%s", limit < 0 ? "-inf" : "inf");
        return;
    }
    snprintf(out, 24, "%.9g", limit);
}

/* A keyword's choices: segments for up to three, else a list that holds only
 * the current one until it is opened (details.choices), so closed lists
 * build no elements. */
static void keyword_row(rows *r, yyjson_mut_val *row, const char *id, const designer_value_shape *shape)
{
    view *v = r->v;
    const designer_property_type *type = shape->type;
    put_text(v, row, "value", type->keywords[shape->keyword]);
    if (type->count <= 3) {
        yyjson_mut_val *options = member(row, "options");
        for (int i = 0; i < type->count; i++) {
            yyjson_mut_val *option = item_at(v, options, (size_t)i, ITEM_OPTION);
            put_text(v, option, "label", type->keywords[i]);
            put_flag(v, option, "pressed", i == shape->keyword);
            put_flag(v, option, "last", i == type->count - 1);
        }
        trim(v, options, (size_t)type->count);
        return;
    }
    bool open = !strcmp(v->d->choosing, id);
    char key[160];
    snprintf(key, sizeof key, "%s:%s", id, open ? "*" : type->keywords[shape->keyword]);
    yyjson_mut_val *table = member(row, "table");
    if (!strcmp(yyjson_mut_get_str(member(table, "key")), key)) {
        return;
    }
    put_text(v, table, "key", key);
    yyjson_mut_val *groups = member(table, "groups");
    yyjson_mut_arr_clear(groups);
    yyjson_mut_val *group = yyjson_mut_arr_add_obj(v->doc, groups);
    yyjson_mut_obj_add_str(v->doc, group, "label", "");
    yyjson_mut_val *list = yyjson_mut_obj_add_arr(v->doc, group, "options");
    for (int i = 0; i < type->count; i++) {
        if (!open && i != shape->keyword) {
            continue;
        }
        yyjson_mut_val *option = yyjson_mut_arr_add_obj(v->doc, list);
        yyjson_mut_obj_add_strcpy(v->doc, option, "value", type->keywords[i]);
        yyjson_mut_obj_add_strcpy(v->doc, option, "label", type->keywords[i]);
        yyjson_mut_obj_add_str(v->doc, option, "icon", "");
    }
    v->changed = true;
}

/* A property's row: its value in the control its type calls for, and a
 * button removing it. */
static void value_row(rows *r, const char *id, const char *property, const char *value)
{
    view *v = r->v;
    designer_value_shape shape = designer_value_shape_of(property, value);
    yyjson_mut_val *row = next_row(r, id, property, shape.control);
    put_flag(v, row, "remove", true);
    put_text(v, row, "text", value);
    put_text(v, row, "placeholder", "");
    if (!strcmp(shape.control, "number")) {
        char step[24], min[24], max[24];
        limit_text(step, shape.step);
        limit_text(min, shape.min);
        limit_text(max, shape.max);
        put_text(v, row, "number", shape.number);
        put_text(v, row, "unit", shape.unit);
        put_text(v, row, "step", step);
        put_text(v, row, "min", min);
        put_text(v, row, "max", max);
        put_int(v, row, "precision", shape.precision);
        put_flag(v, row, "integer", shape.integer);
        put_flag(v, row, "slider", shape.slider);
    } else if (!strcmp(shape.control, "color")) {
        put_text(v, row, "color", shape.color);
    } else if (shape.type) {
        keyword_row(r, row, id, &shape);
    }
}

/* ---------- Attributes ---------- */

static void text_row(
    rows *r,
    const char *id,
    const char *label,
    const char *value,
    const char *placeholder,
    bool remove
)
{
    view *v = r->v;
    yyjson_mut_val *row = next_row(r, id, label, "text");
    put_flag(v, row, "remove", remove);
    put_text(v, row, "text", value);
    put_text(v, row, "placeholder", placeholder);
}

static bool listed(const view *v, const char *name)
{
    return studio_matches(v->s->property_search, name);
}

/* A <use>'s widget, chosen among the project's ui assets. */
static void widget_row(rows *r, int element)
{
    view *v = r->v;
    if (!listed(v, "widget")) {
        return;
    }
    char value[128];
    designer_attribute_text(v->d->rml, designer_attribute(v->d->rml, element, "ui"), value, sizeof value);
    yyjson_mut_val *row = next_row(r, "widget", "widget", "asset");
    put_text(v, row, "text", value);
}

static void element_text_row(rows *r, int element)
{
    view *v = r->v;
    if (!listed(v, "text")) {
        return;
    }
    char text[512];
    designer_element_text(v->d->rml, element, text, sizeof text);
    text_row(r, "text", "text", text, "None", false);
}

static void id_row(rows *r, int element)
{
    view *v = r->v;
    if (!listed(v, "id")) {
        return;
    }
    char value[128];
    designer_attribute_text(v->d->rml, designer_attribute(v->d->rml, element, "id"), value, sizeof value);
    text_row(r, "id", "id", value, "None", false);
}

/* Classes as chips, each removable. */
static void class_row(rows *r, int element)
{
    view *v = r->v;
    if (!listed(v, "class")) {
        return;
    }
    yyjson_mut_val *row = next_row(r, "class", "class", "chips");
    yyjson_mut_val *chips = member(row, "chips");
    size_t count = 0;
    const char *classes = v->d->rml->elements[element].classes;
    for (const char *c = classes ? classes : ""; *c;) {
        size_t n = strcspn(c, " \t\n");
        if (n) {
            char name[128];
            snprintf(name, sizeof name, "%.*s", (int)n, c);
            put_text(v, item_at(v, chips, count++, ITEM_CHIP), "name", name);
        }
        c += n + (c[n] != '\0');
    }
    trim(v, chips, count);
}

static void attribute_row(rows *r, int index, const char *name, const char *value)
{
    if (!listed(r->v, name)) {
        return;
    }
    char id[16];
    snprintf(id, sizeof id, "a%d", index);
    text_row(r, id, name, value, "", true);
}

static void attributes_section(view *v, int element)
{
    const ui_source *rml = v->d->rml;
    const ui_source_element *el = &rml->elements[element];
    rows r = begin_section(v, "attributes", "Attributes", "attributes");
    if (!strcmp(el->tag, "use")) {
        widget_row(&r, element);
    }
    if (ui_design_text_only(rml, element)) {
        element_text_row(&r, element);
    }
    id_row(&r, element);
    class_row(&r, element);
    for (int i = 0; i < acount(el->attributes); i++) {
        const ui_source_attribute *a = &el->attributes[i];
        char name[128], value[512];
        designer_attribute_name(rml, a, name, sizeof name);
        designer_attribute_text(rml, a, value, sizeof value);
        if (!designer_own_attribute(el->tag, name) &&
            !ui_source_is_binding(name, (int)strlen(name))) {
            attribute_row(&r, i, name, value);
        }
    }
    put_text(v, r.section, "addLabel", "Add Attribute");
    put_text(v, r.section, "addKind", "attribute");
    put_int(v, r.section, "addTarget", element);
    end_section(&r);
}

/* Data bindings, as RmlUi's data-* attributes write them, in code. */
static void bindings_section(view *v, int element)
{
    const ui_source *rml = v->d->rml;
    const ui_source_element *el = &rml->elements[element];
    int count = 0;
    for (int i = 0; i < acount(el->attributes); i++) {
        const ui_source_attribute *a = &el->attributes[i];
        count += ui_source_is_binding(rml->text + a->name_start, a->name_end - a->name_start);
    }
    if (!count) {
        return;
    }
    rows r = begin_section(v, "bindings", "Bindings", "bindings");
    for (int i = 0; i < acount(el->attributes); i++) {
        const ui_source_attribute *a = &el->attributes[i];
        char name[128], value[512];
        designer_attribute_name(rml, a, name, sizeof name);
        designer_attribute_text(rml, a, value, sizeof value);
        if (ui_source_is_binding(name, (int)strlen(name))) {
            attribute_row(&r, i, name, value);
        }
    }
    end_section(&r);
}

/* ---------- Inline style ---------- */

static void style_section(view *v, int element)
{
    designer_declaration *styles = designer_style_declarations(v->d->rml, element);
    rows r = begin_section(v, "style", "Inline Style", "style");
    for (int i = 0; i < acount(styles); i++) {
        if (listed(v, styles[i].property)) {
            char id[sizeof styles[i].property + 4];
            snprintf(id, sizeof id, "s:%s", styles[i].property);
            value_row(&r, id, styles[i].property, styles[i].value);
        }
    }
    put_text(v, r.section, "none", acount(styles) ? "" : "No inline style");
    put_text(v, r.section, "addLabel", "Add Property");
    put_text(v, r.section, "addKind", "style");
    put_int(v, r.section, "addTarget", element);
    put_flag(v, r.section, "extract", acount(styles) > 0);
    end_section(&r);
    afree(styles);
}

/* ---------- Rules ---------- */

static void declaration_row(rows *r, int rule, int index)
{
    const ui_source *rcss = r->v->d->rcss;
    const ui_source_declaration *d = &rcss->rules[rule].declarations[index];
    char property[128], value[256], id[24];
    snprintf(property, sizeof property, "%.*s", d->property_end - d->property_start, rcss->text + d->property_start);
    snprintf(value, sizeof value, "%.*s", d->value_end - d->value_start, rcss->text + d->value_start);
    if (!listed(r->v, property)) {
        return;
    }
    snprintf(id, sizeof id, "d%d.%d", rule, index);
    value_row(r, id, property, value);
}

/* A rule by its selector, its file and line opening it in the Text Editor. */
static void rule_section(view *v, int rule)
{
    const ui_source_rule *rl = &v->d->rcss->rules[rule];
    char key[16], origin[160];
    snprintf(key, sizeof key, "rule%d", rule);
    snprintf(origin, sizeof origin, "%s:%d", content_basename(v->b->files[UI_PART_RCSS]), rl->line);
    rows r = begin_section(v, key, rl->selector, "rule");
    put_text(v, r.section, "origin", origin);
    put_text(
        v,
        r.section,
        "within",
        rl->parent >= 0 ? v->d->rcss->rules[rl->parent].selector : ""
    );
    put_int(v, r.section, "rule", rule);
    for (int i = 0; i < acount(rl->declarations); i++) {
        declaration_row(&r, rule, i);
    }
    put_text(v, r.section, "addLabel", "Add Property");
    put_text(v, r.section, "addKind", "declaration");
    put_int(v, r.section, "addTarget", rule);
    end_section(&r);
}

/* The document's rules that match the element, or all of them for -1. */
static int rules(view *v, int element)
{
    const ui_source *rcss = v->d->rcss;
    int count = 0;
    for (int r = 0; r < acount(rcss->rules); r++) {
        const char *selector = rcss->rules[r].selector;
        if (selector[0] != '@' &&
            (element < 0 || designer_rule_matches(v->d->rml, selector, element))) {
            rule_section(v, r);
            count++;
        }
    }
    return count;
}

/* ---------- The panel ---------- */

/* The element's glyph, name and kind, with its source line as a link to
 * the Text Editor when it has one. */
static void identity(
    view *v,
    yyjson_mut_val *details,
    uint32_t icon,
    const char *title,
    const char *kind,
    const char *source
)
{
    char glyph[5];
    studio_glyph(glyph, icon);
    yyjson_mut_val *identity = member(details, "identity");
    put_text(v, identity, "icon", glyph);
    put_text(v, identity, "title", title);
    put_text(v, identity, "kind", kind);
    put_text(v, identity, "source", source);
}

static void element_view(view *v, yyjson_mut_val *details, int element)
{
    const ui_source *rml = v->d->rml;
    const ui_source_element *el = &rml->elements[element];
    char name[96], title[256], kind[80], source[160];
    designer_element_name(rml, element, name, sizeof name);
    snprintf(title, sizeof title, "%s%s%s", name, el->id ? "#" : "", el->id ? el->id : "");
    if (strcmp(el->tag, "use")) {
        snprintf(kind, sizeof kind, "<%s>", el->tag);
    } else {
        snprintf(kind, sizeof kind, "Widget");
    }
    snprintf(source, sizeof source, "%s:%d", v->d->asset, el->line);
    identity(v, details, designer_element_icon(rml, element), title, kind, source);
    put_text(v, details, "hint", "");
    attributes_section(v, element);
    bindings_section(v, element);
    style_section(v, element);
    bool none = !rules(v, element) && !v->s->property_search[0];
    put_text(v, details, "note", none ? "No rule of the document's RCSS matches it" : "");
}

static void document_view(view *v, yyjson_mut_val *details)
{
    char kind[64];
    snprintf(
        kind,
        sizeof kind,
        "UI document \xc2\xb7 %d elements",
        acount(v->d->rml->elements)
    );
    identity(v, details, KIT_ICON_LAYOUT, v->d->asset, kind, "");
    put_text(v, details, "hint", "Select an element to edit it. The document's rules:");
    put_text(v, details, "note", rules(v, -1) ? "" : "The document has no RCSS rules");
}

bool studio_details_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = at->preview ? NULL : designer_state(s, &b);
    if (!d) {
        return false;
    }
    yyjson_mut_val *details = at->variable;
    yyjson_mut_val *query = member(details, "query");
    if (at->wrote && strcmp(yyjson_mut_get_str(query), s->property_search)) {
        snprintf(s->property_search, sizeof s->property_search, "%s", yyjson_mut_get_str(query));
    }
    struct designer_details_seen *seen = &d->details_seen;
    if (seen->built && seen->parsed == d->parsed && seen->selected == d->selected &&
        seen->shuts == d->shuts && !strcmp(seen->search, s->property_search) &&
        !strcmp(seen->choosing, d->choosing)) {
        return false;
    }
    *seen = (struct designer_details_seen){ .built = true,
                                            .parsed = d->parsed,
                                            .selected = d->selected,
                                            .shuts = d->shuts };
    snprintf(seen->search, sizeof seen->search, "%s", s->property_search);
    snprintf(seen->choosing, sizeof seen->choosing, "%s", d->choosing);
    view v = { .s = s, .d = d, .b = b, .doc = at->doc, .sections = member(details, "sections") };
    put_text(&v, details, "query", s->property_search);
    if (d->selected >= 0) {
        element_view(&v, details, d->selected);
    } else {
        document_view(&v, details);
    }
    trim(&v, v.sections, v.count);
    return v.changed;
}

/* ---------- The Add popover ---------- */

static const char *const ADD_NAMES[] = { "class", "attribute", "style", "declaration", "extract" };

static rml_element *by_id(studio *s, const char *id)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* Frames an Add popover's field keeps asking for the focus, since the
 * popover places itself after it opens. */
#define FOCUS_FRAMES 30

bool studio_details_popover_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = at->preview ? NULL : designer_state(s, &b);
    if (!d) {
        return false;
    }
    yyjson_mut_val *add = at->variable;
    yyjson_mut_val *typed = member(add, "text");
    if (at->wrote && strcmp(yyjson_mut_get_str(typed), d->add.text)) {
        snprintf(d->add.text, sizeof d->add.text, "%s", yyjson_mut_get_str(typed));
    }
    if (d->add.focus > 0) {
        rml_element *field = by_id(s, "details-add-field");
        if (field && rml_element_is_visible(field, true) &&
            rml_element_focus(field, true)) {
            rml_element_select(field);
            d->add.focus = 0;
        } else {
            d->add.focus--;
            s->ui_schedule.wake = true;
        }
    }
    bool changed = ui_data_put_text(at->doc, add, "title", ADD_TITLES[d->add.kind]);
    changed |= ui_data_put_text(at->doc, add, "hint", ADD_HINTS[d->add.kind]);
    changed |= ui_data_put_text(at->doc, add, "text", d->add.text);
    struct designer_add_seen *seen = &d->add_seen;
    if (seen->built && seen->parsed == d->parsed && seen->kind == d->add.kind &&
        seen->target == d->add.target && !strcmp(seen->text, d->add.text)) {
        return changed;
    }
    *seen = (struct designer_add_seen){ .built = true,
                                        .parsed = d->parsed,
                                        .kind = d->add.kind,
                                        .target = d->add.target };
    snprintf(seen->text, sizeof seen->text, "%s", d->add.text);
    char **choices = designer_suggestions(
        d,
        d->add.target,
        (designer_add_kind)d->add.kind,
        d->add.text
    );
    yyjson_mut_val *list = member(add, "suggestions");
    view v = { .s = s, .d = d, .b = b, .doc = at->doc };
    for (int i = 0; i < acount(choices); i++) {
        put_text(&v, item_at(&v, list, (size_t)i, ITEM_SUGGESTION), "label", choices[i]);
        free(choices[i]);
    }
    trim(&v, list, (size_t)acount(choices));
    afree(choices);
    return v.changed || changed;
}

/* ---------- Intents ---------- */

/* The target a row's id names: "a<i>" an attribute, "id", "text", "widget",
 * "s:<property>" an inline property, "d<rule>.<declaration>". */
static bool parse_ref(const char *id, designer_target *t)
{
    *t = (designer_target){ 0 };
    if (!strcmp(id, "id")) {
        t->kind = DESIGNER_TARGET_ID;
    } else if (!strcmp(id, "text")) {
        t->kind = DESIGNER_TARGET_TEXT;
    } else if (!strcmp(id, "widget")) {
        t->kind = DESIGNER_TARGET_WIDGET;
    } else if (id[0] == 'a' && isdigit((unsigned char)id[1])) {
        t->kind = DESIGNER_TARGET_ATTRIBUTE;
        t->index = atoi(id + 1);
    } else if (!strncmp(id, "s:", 2)) {
        t->kind = DESIGNER_TARGET_STYLE;
        snprintf(t->property, sizeof t->property, "%s", id + 2);
    } else if (id[0] == 'd' && isdigit((unsigned char)id[1])) {
        t->kind = sscanf(id + 1, "%d.%d", &t->rule, &t->index) == 2
                    ? DESIGNER_TARGET_DECLARATION
                    : DESIGNER_TARGET_NONE;
    }
    return t->kind != DESIGNER_TARGET_NONE;
}

static bool init(studio *s, details *x)
{
    x->s = s;
    x->d = designer_state(s, &x->b);
    return x->d != NULL;
}

/* What a refused change warns of. */
static const char *change_failure(designer_target_kind kind)
{
    switch (kind) {
        case DESIGNER_TARGET_ATTRIBUTE: return "Could not change the attribute";
        case DESIGNER_TARGET_ID: return "Could not change the id";
        case DESIGNER_TARGET_TEXT: return "Could not change the text";
        case DESIGNER_TARGET_WIDGET: return "Could not change the widget";
        default: return "Could not change the value";
    }
}

static const char *removal_failure(designer_target_kind kind)
{
    return kind == DESIGNER_TARGET_STYLE ? "Could not remove the property"
         : kind == DESIGNER_TARGET_DECLARATION
             ? "Could not remove the declaration"
             : "Could not change the attribute";
}

static void show(const details *x, designer_change result, const char *failure, const char *error)
{
    if (result == DESIGNER_REFUSED) {
        designer_warn(x->s, failure, error);
    }
}

/* A control's input or change: emit('details.input', row, value) and
 * emit('details.change', row, value[, canceled]). Input writes as the
 * control moves; change ends the gesture, and Escape puts the text back by
 * undoing it. */
static void gesture(studio *s, const ui_activation *a, bool active)
{
    details x;
    designer_target t;
    char error[192] = "";
    if (a->argc < 2 || !init(s, &x) || !parse_ref(a->args[0], &t)) {
        return;
    }
    bool canceled = !active && a->argc > 2 && !strcmp(a->args[2], "1");
    designer_change result = designer_set_value(x.d, x.b, &t, a->args[1], active, canceled, error, sizeof error);
    show(&x, result, change_failure(t.kind), error);
}

/* A text field puts the document's text back and lets go. */
static void settle(const details *x, rml_element *field, const designer_target *t)
{
    char text[512];
    if (field && designer_target_text(x->d, t, text, sizeof text)) {
        rml_element_set_value(field, text);
    }
    if (field) {
        rml_element_blur(field);
    }
}

/* A text field's commit: emit('details.text', row); the field holds the
 * text, which can be longer than an argument. A name or text the document
 * refuses goes back to what it was; a value stays beside its warning. */
static void commit_text(studio *s, const ui_activation *a)
{
    details x;
    designer_target t;
    rml_element *field = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    if (!field || !a->argc || !init(s, &x) || !parse_ref(a->args[0], &t)) {
        return;
    }
    char text[4096], error[192] = "";
    rml_element_value(field, text, sizeof text);
    designer_change result = designer_set_text(x.d, x.b, &t, text, error, sizeof error);
    show(&x, result, change_failure(t.kind), error);
    bool value = t.kind == DESIGNER_TARGET_STYLE || t.kind == DESIGNER_TARGET_DECLARATION;
    if (result == DESIGNER_REFUSED && !value) {
        settle(&x, field, &t);
    }
}

/* Removes a row: an attribute, an inline property or a declaration. */
static void remove_row(studio *s, const ui_activation *a)
{
    details x;
    designer_target t;
    char error[192] = "";
    if (!a->argc || !init(s, &x) || !parse_ref(a->args[0], &t)) {
        return;
    }
    show(&x, designer_remove_target(x.d, x.b, &t, error, sizeof error), removal_failure(t.kind), error);
}

/* The Add popover for `kind` and `target` (an element, or a rule for a
 * declaration), under the button that asked. */
static void open_add(studio *s, const ui_activation *a)
{
    details x;
    if (a->argc != 2 || !init(s, &x)) {
        return;
    }
    int kind = -1;
    for (size_t i = 0; i < sizeof ADD_NAMES / sizeof *ADD_NAMES; i++) {
        kind = !strcmp(ADD_NAMES[i], a->args[0]) ? (int)i : kind;
    }
    rml_element *button = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    rml_element *popover = button
                             ? rml_element_by_id(rml_element_owner_document(button), "details-add")
                             : NULL;
    if (kind < 0 || !popover) {
        return;
    }
    x.d->add.kind = kind;
    x.d->add.target = atoi(a->args[1]);
    x.d->add.text[0] = '\0';
    x.d->add.focus = FOCUS_FRAMES;
    studio_ui_resync(s, "details_add");
    popover_show(popover, &(popover_anchor){ .side = POPOVER_BELOW, .element = button, .trigger = button });
}

/* Adds what the popover's text names; whether it did. */
static bool add_value(const details *x, int target, designer_add_kind kind, const char *text)
{
    char error[192] = "";
    designer_change result = designer_add_value(x->d, x->b, target, kind, text, error, sizeof error);
    show(x, result, ADD_TITLES[kind], error);
    return result == DESIGNER_CHANGED;
}

void studio_details_action(studio *s, const char *action, const ui_activation *a)
{
    details x;
    if (!init(s, &x)) {
        return;
    }
    const char *arg = a->argc ? a->args[0] : "";
    if (!strcmp(action, "input") || !strcmp(action, "change")) {
        gesture(s, a, !strcmp(action, "input"));
    } else if (!strcmp(action, "text")) {
        commit_text(s, a);
    } else if (!strcmp(action, "remove")) {
        remove_row(s, a);
    } else if (!strcmp(action, "removeClass") && x.d->selected >= 0) {
        char error[192] = "";
        bool ok = ui_design_class(x.b, x.d->selected, arg, false, error, sizeof error);
        show(&x, ok ? DESIGNER_CHANGED : DESIGNER_REFUSED, "Could not remove the class", error);
    } else if (!strcmp(action, "add")) {
        open_add(s, a);
    } else if (!strcmp(action, "addPick") || !strcmp(action, "addEnter")) {
        rml_element *field = !strcmp(action, "addEnter") && a->anchor
                               ? rml_element_ref_get(a->anchor)
                               : NULL;
        char typed[192] = "";
        if (field) {
            rml_element_value(field, typed, sizeof typed);
        }
        const char *text = field ? typed : arg;
        if (text[0] &&
            add_value(&x, x.d->add.target, (designer_add_kind)x.d->add.kind, text) &&
            field) {
            rml_element *popover = rml_element_by_id(rml_element_owner_document(field), "details-add");
            if (popover) {
                popover_hide(popover);
            }
        }
    } else if (!strcmp(action, "fold")) {
        fold_panel(x.d, arg);
    } else if (!strcmp(action, "source") && x.d->selected >= 0) {
        studio_open_ui_source(s, x.d->path, x.d->rml->elements[x.d->selected].line);
    } else if (
        !strcmp(action, "openRule") && a->argc && atoi(arg) < acount(x.d->rcss->rules)
    ) {
        studio_open_ui_part(
            s,
            x.d->path,
            UI_PART_RCSS,
            x.d->rcss->rules[atoi(arg)].line
        );
    } else if (!strcmp(action, "choices")) {
        snprintf(x.d->choosing, sizeof x.d->choosing, "%s", arg);
    } else if (!strcmp(action, "cancel")) {
        designer_target t;
        rml_element *field = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
        if (parse_ref(a->id, &t)) {
            settle(&x, field, &t);
        }
    }
}

/* ---------- Panel ---------- */

void studio_designer_details_header(studio *s, int area)
{
    rml_element *host = studio_ui_area_controls(s, area, "details", "DetailsHeader");
    rml_element *field = host && s->find_request == STUDIO_EDITOR_PROPERTIES + 1
                           ? rml_element_query_selector(host, "#details-search")
                           : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        s->find_request = 0;
    }
}

void studio_designer_details(studio *s, int area)
{
    studio_ui_area(s, area, "Details");
}
