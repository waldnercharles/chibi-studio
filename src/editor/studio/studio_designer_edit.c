/* The visual designer's editing capabilities (workstream D1), declared in
 * studio_designer_edit.h: what the document holds and each change a Details
 * row can make to it. Plain C over the parsed document and `ui_design`'s
 * undoable edits; the panel's glue (studio_designer_details.c) shapes data
 * and reads controls, and decides nothing about the document. */
#include "editor/studio/studio_designer_edit.h"

#include "authoring/ui_check.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Suggestions an Add popover lists at once. */
#define SUGGESTIONS 12

/* ---------- The document, read ---------- */

const ui_source_attribute *designer_attribute(const ui_source *rml, int element, const char *name)
{
    const ui_source_element *el = &rml->elements[element];
    for (int i = 0; i < acount(el->attributes); i++) {
        const ui_source_attribute *a = &el->attributes[i];
        if ((size_t)(a->name_end - a->name_start) == strlen(name) &&
            !strncmp(rml->text + a->name_start, name, strlen(name))) {
            return a;
        }
    }
    return NULL;
}

/* An attribute's value as written, which writes back unchanged. */
void designer_attribute_text(const ui_source *rml, const ui_source_attribute *a, char *out, size_t size)
{
    int length = a && a->value_start >= 0 ? a->value_end - a->value_start : 0;
    snprintf(out, size, "%.*s", length, length ? rml->text + a->value_start : "");
}

void designer_attribute_name(const ui_source *rml, const ui_source_attribute *a, char *out, size_t size)
{
    snprintf(out, size, "%.*s", a->name_end - a->name_start, rml->text + a->name_start);
}

/* Rows of their own show these. */
bool designer_own_attribute(const char *tag, const char *name)
{
    return !strcmp(name, "id") || !strcmp(name, "class") ||
           !strcmp(name, "style") || (!strcmp(tag, "use") && !strcmp(name, "ui"));
}

/* The text of an element holding text alone, without the space around it. */
void designer_element_text(const ui_source *rml, int element, char *out, size_t size)
{
    const ui_source_element *el = &rml->elements[element];
    int start = el->open_end, end = el->close_start;
    while (start < end && isspace((unsigned char)rml->text[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)rml->text[end - 1])) {
        end--;
    }
    snprintf(out, size, "%.*s", end - start, rml->text + start);
}

/* Attributes RmlUi documents take besides id, class and style. */
static const char *const ATTRIBUTES[] = {
    "data-if",      "data-when", "data-for",  "data-event-click", "data-value",
    "data-checked", "data-keys", "autofocus", "placeholder",      "type",
    "value",        "src",       "disabled",
};

static bool has_class(const char *classes, const char *name, size_t length)
{
    for (const char *c = classes ? classes : ""; *c;) {
        size_t n = strcspn(c, " \t\n");
        if (n == length && !strncmp(c, name, n)) {
            return true;
        }
        c += n + (c[n] != '\0');
    }
    return false;
}

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_';
}

/* The classes the document's selectors name that the element lacks. */
static void class_choices(const studio_designer *d, int element, char ***out)
{
    const ui_source_element *el = &d->rml->elements[element];
    for (int r = 0; r < acount(d->rcss->rules); r++) {
        for (const char *c = strchr(d->rcss->rules[r].selector, '.'); c;
             c = strchr(c + 1, '.')) {
            size_t n = 0;
            while (name_char(c[1 + n])) {
                n++;
            }
            char name[128];
            snprintf(name, sizeof name, "%.*s", (int)n, c + 1);
            if (n && !has_class(el->classes, name, n)) {
                ui_source_list_add(out, name);
            }
        }
    }
}

/* What an Add popover suggests for what is typed so far. */
char **designer_suggestions(const studio_designer *d, int target, designer_add_kind kind, const char *typed)
{
    char **all = NULL, **matches = NULL;
    bool element_target = kind != DESIGNER_ADD_DECLARATION;
    if (target < 0 || target >= (element_target ? acount(d->rml->elements)
                                                : acount(d->rcss->rules))) {
        return NULL;
    }
    if (kind == DESIGNER_ADD_CLASS) {
        class_choices(d, target, &all);
    } else if (kind == DESIGNER_ADD_ATTRIBUTE) {
        for (size_t i = 0; i < sizeof ATTRIBUTES / sizeof *ATTRIBUTES; i++) {
            if (!designer_attribute(d->rml, target, ATTRIBUTES[i])) {
                ui_source_list_add(&all, ATTRIBUTES[i]);
            }
        }
    } else if (kind != DESIGNER_ADD_EXTRACT) {
        ui_check_property_names(&all);
    }
    for (int i = 0; i < acount(all); i++) {
        if (studio_matches(typed, all[i]) && acount(matches) < SUGGESTIONS) {
            apush(matches, all[i]);
        } else {
            free(all[i]);
        }
    }
    afree(all);
    return matches;
}

/* "name=value", "property: value" or a name alone, split at `separator`. */
static void split(const char *text, char separator, char *name, size_t name_size, char *value, size_t value_size)
{
    const char *at = strchr(text, separator);
    int length = at ? (int)(at - text) : (int)strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        length--;
    }
    snprintf(name, name_size, "%.*s", length, text);
    const char *rest = at ? at + 1 : "";
    while (isspace((unsigned char)*rest) || *rest == '"') {
        rest++;
    }
    snprintf(value, value_size, "%s", rest);
    for (size_t n = strlen(value); n && (isspace((unsigned char)value[n - 1]) ||
                                         value[n - 1] == '"' || value[n - 1] == ';');) {
        value[--n] = '\0';
    }
}

/* The inline style's declarations, in order, as `property: value` text. The
 * caller afrees the array. */
designer_declaration *designer_style_declarations(const ui_source *rml, int element)
{
    char style[1024];
    designer_attribute_text(rml, designer_attribute(rml, element, "style"), style, sizeof style);
    designer_declaration *found = NULL;
    for (const char *at = style; *at;) {
        size_t length = strcspn(at, ";");
        char text[512];
        snprintf(text, sizeof text, "%.*s", (int)length, at);
        at += length + (at[length] == ';');
        designer_declaration d;
        split(text, ':', d.property, sizeof d.property, d.value, sizeof d.value);
        const char *name = d.property;
        while (isspace((unsigned char)*name)) {
            name++;
        }
        if (name[0]) {
            memmove(d.property, name, strlen(name) + 1);
            apush(found, d);
        }
    }
    return found;
}

/* Past a pseudo class, as :hover or :not(...), or an attribute test. */
static int skip_test(const char *text, int at, int end, char kind)
{
    if (kind == '[') {
        while (at < end && text[at] != ']') {
            at++;
        }
        return at + (at < end);
    }
    at += at < end && text[at] == ':';
    while (at < end && name_char(text[at])) {
        at++;
    }
    for (int depth = 0; at < end && (depth || text[at] == '(');) {
        depth += (text[at] == '(') - (text[at] == ')');
        at++;
    }
    return at;
}

/* Whether a compound selector's tag, id and classes all hold for the
 * element. Pseudo classes and attribute tests are left out, so a rule for
 * a state, as :hover, shows too. */
static bool compound_matches(const char *text, int start, int end, const ui_source_element *el)
{
    int at = start;
    while (at < end && name_char(text[at])) {
        at++;
    }
    bool named = at > start;
    if (named && ((size_t)(at - start) != strlen(el->tag) ||
                  strncasecmp(text + start, el->tag, (size_t)(at - start)))) {
        return false;
    }
    if (!named && at < end && text[at] == '*') {
        named = true;
        at++;
    }
    while (at < end) {
        char kind = text[at++];
        if (kind == ':' || kind == '[') {
            at = skip_test(text, at, end, kind);
            continue;
        }
        int name = at;
        while (at < end && name_char(text[at])) {
            at++;
        }
        size_t length = (size_t)(at - name);
        bool holds = kind == '.' ? has_class(el->classes, text + name, length)
                   : kind == '#' ? el->id && strlen(el->id) == length &&
                                       !strncmp(el->id, text + name, length)
                                 : false;
        if (!holds || !length) {
            return false;
        }
        named = true;
    }
    return named;
}

/* One compound of a selector and the combinator before it: ' ', '>', '+',
 * '~', or 0 for the first. */
typedef struct
{
    int start, end;
    char combinator;
} compound;

enum
{
    COMPOUNDS = 16,
};

static int split_compounds(const char *text, int start, int end, compound *out)
{
    int count = 0;
    char combinator = 0;
    for (int at = start; at < end && count < COMPOUNDS;) {
        if (text[at] == ' ' || strchr(">+~", text[at])) {
            combinator = text[at] == ' ' && combinator ? combinator : text[at];
            at++;
            continue;
        }
        int from = at;
        for (int depth = 0; at < end && (depth || !strchr(" >+~", text[at])); at++) {
            depth += (text[at] == '(' || text[at] == '[') -
                     (text[at] == ')' || text[at] == ']');
        }
        out[count] = (compound){ from, at, count ? combinator : 0 };
        count++;
        combinator = 0;
    }
    return count;
}

/* `body` in a document's RCSS names its host, above all its elements. */
static bool host(const char *text, const compound *c)
{
    return c->end - c->start >= 4 && !strncmp(text + c->start, "body", 4) &&
           !name_char(text[c->start + 4]);
}

/* Whether compounds [0, last] match `element` and its ancestors, right to
 * left. Sibling combinators are not checked, and a widget's own elements
 * between a <use> and its slot's content are not in the markup. */
static bool matches_at(const ui_source *rml, const char *text, const compound *c, int last, int element)
{
    if (host(text, &c[last])) {
        return true;
    }
    if (element < 0 ||
        !compound_matches(text, c[last].start, c[last].end, &rml->elements[element])) {
        return false;
    }
    if (last == 0 || c[last].combinator == '+' || c[last].combinator == '~') {
        return true;
    }
    int parent = rml->elements[element].parent;
    if (c[last].combinator == '>') {
        return matches_at(rml, text, c, last - 1, parent);
    }
    for (int a = parent; a >= 0; a = rml->elements[a].parent) {
        if (matches_at(rml, text, c, last - 1, a)) {
            return true;
        }
    }
    return host(text, &c[last - 1]);
}

/* Whether one of the selector's comma-separated selectors matches the
 * element in the markup. */
bool designer_rule_matches(const ui_source *rml, const char *selector, int element)
{
    int length = (int)strlen(selector);
    for (int start = 0; start < length;) {
        int end = start;
        for (int depth = 0; end < length && (selector[end] != ',' || depth); end++) {
            depth += (selector[end] == '(') - (selector[end] == ')');
        }
        compound c[COMPOUNDS];
        int count = split_compounds(selector, start, end, c);
        if (count && !host(selector, &c[count - 1]) &&
            matches_at(rml, selector, c, count - 1, element)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

/* ---------- Targets ---------- */

/* The declaration of a rule `t` names, or NULL. */
static const ui_source_declaration *declaration_of(const studio_designer *d, const designer_target *t)
{
    const ui_source_rule *rules = d->rcss->rules;
    if (t->rule < 0 || t->rule >= acount(d->rcss->rules) || t->index < 0 ||
        t->index >= acount(rules[t->rule].declarations)) {
        return NULL;
    }
    return &rules[t->rule].declarations[t->index];
}

bool designer_target_text(const studio_designer *d, const designer_target *t, char *out, size_t size)
{
    const ui_source *rml = d->rml;
    int element = d->selected;
    out[0] = '\0';
    if (t->kind == DESIGNER_TARGET_DECLARATION) {
        const ui_source_declaration *decl = declaration_of(d, t);
        snprintf(
            out,
            size,
            "%.*s",
            decl ? decl->value_end - decl->value_start : 0,
            decl ? d->rcss->text + decl->value_start : ""
        );
        return decl != NULL;
    }
    if (element < 0 || element >= acount(rml->elements)) {
        return false;
    }
    const ui_source_element *el = &rml->elements[element];
    switch (t->kind) {
        case DESIGNER_TARGET_ATTRIBUTE:
            if (t->index >= acount(el->attributes)) {
                return false;
            }
            designer_attribute_text(rml, &el->attributes[t->index], out, size);
            return true;
        case DESIGNER_TARGET_ID:
            designer_attribute_text(rml, designer_attribute(rml, element, "id"), out, size);
            return true;
        case DESIGNER_TARGET_WIDGET:
            designer_attribute_text(rml, designer_attribute(rml, element, "ui"), out, size);
            return true;
        case DESIGNER_TARGET_TEXT:
            designer_element_text(rml, element, out, size);
            return true;
        case DESIGNER_TARGET_STYLE: {
            designer_declaration *styles = designer_style_declarations(rml, element);
            bool found = false;
            for (int i = 0; i < acount(styles); i++) {
                if (!strcmp(styles[i].property, t->property)) {
                    snprintf(out, size, "%s", styles[i].value);
                    found = true;
                }
            }
            afree(styles);
            return found;
        }
        default: return false;
    }
}

/* ---------- Changes ---------- */

/* The value a gesture changes: an element's inline property (`element`), or
 * a declaration of a rule. */
typedef struct
{
    int element; /* -1 for a rule */
    int rule, declaration;
    char property[128];
} value_target;

/* The value a style or declaration target names. */
static bool value_target_of(const studio_designer *d, const designer_target *t, value_target *v)
{
    if (t->kind == DESIGNER_TARGET_STYLE && d->selected >= 0) {
        *v = (value_target){ d->selected, -1, -1, "" };
        snprintf(v->property, sizeof v->property, "%s", t->property);
        return true;
    }
    const ui_source_declaration *decl = t->kind == DESIGNER_TARGET_DECLARATION
                                          ? declaration_of(d, t)
                                          : NULL;
    if (!decl) {
        return false;
    }
    *v = (value_target){ -1, t->rule, t->index, "" };
    snprintf(
        v->property,
        sizeof v->property,
        "%.*s",
        decl->property_end - decl->property_start,
        d->rcss->text + decl->property_start
    );
    return true;
}

static void gesture_key(const value_target *v, char *key, size_t size)
{
    snprintf(key, size, "%d:%d:%d:%s", v->element, v->rule, v->declaration, v->property);
}

/* A scrub's or a picker's changes to one value join one undo step while
 * nothing else changed the text between them. */
static bool write_value(
    studio_designer *d,
    ui_buffer *b,
    const value_target *v,
    const char *text,
    bool committed,
    char *error,
    size_t size
)
{
    char key[sizeof d->gesture.key];
    gesture_key(v, key, sizeof key);
    bool mine = !strcmp(d->gesture.key, key);
    bool join = d->gesture.active && mine && d->gesture.revision == b->revision;
    unsigned before = b->revision;
    bool ok = v->element >= 0
                ? ui_design_set_style(b, v->element, v->property, text, join, error, size)
                : ui_design_set_declaration(b, v->rule, v->declaration, text, join, error, size);
    d->gesture.active = ok && !committed && (join || b->revision != before);
    snprintf(d->gesture.key, sizeof d->gesture.key, "%s", key);
    d->gesture.revision = b->revision;
    return ok;
}

/* A <use>'s widget, chosen among the project's ui assets; none is no
 * widget, which a <use> cannot do without. */
static designer_change set_widget(studio_designer *d, ui_buffer *b, const char *name, char *error, size_t size)
{
    char current[128];
    int element = d->selected;
    if (element < 0 || !name[0]) {
        return DESIGNER_UNCHANGED;
    }
    designer_attribute_text(d->rml, designer_attribute(d->rml, element, "ui"), current, sizeof current);
    if (!strcmp(name, current)) {
        return DESIGNER_UNCHANGED;
    }
    return ui_design_set_attribute(b, element, "ui", name, error, size)
             ? DESIGNER_CHANGED
             : DESIGNER_REFUSED;
}

designer_change designer_set_value(
    studio_designer *d,
    ui_buffer *b,
    const designer_target *t,
    const char *argument,
    bool active,
    bool canceled,
    char *error,
    size_t size
)
{
    if (t->kind == DESIGNER_TARGET_WIDGET) {
        return set_widget(d, b, argument, error, size);
    }
    value_target v;
    char current[256], text[256];
    if (!value_target_of(d, t, &v) ||
        !designer_target_text(d, t, current, sizeof current)) {
        return DESIGNER_UNCHANGED;
    }
    char key[sizeof d->gesture.key];
    gesture_key(&v, key, sizeof key);
    bool mine = d->gesture.active && !strcmp(d->gesture.key, key);
    if (canceled) {
        if (mine && d->gesture.revision == b->revision && ui_buffer_can_undo(b)) {
            ui_buffer_undo(b);
        }
        d->gesture.active = false;
        return DESIGNER_UNCHANGED;
    }
    if (!designer_value_text(v.property, current, argument, text, sizeof text)) {
        return DESIGNER_UNCHANGED;
    }
    if (!strcmp(text, current)) {
        d->gesture.active &= !(mine && !active);
        return DESIGNER_UNCHANGED;
    }
    return write_value(d, b, &v, text, !active, error, size) ? DESIGNER_CHANGED
                                                             : DESIGNER_REFUSED;
}

designer_change designer_set_text(
    studio_designer *d,
    ui_buffer *b,
    const designer_target *t,
    const char *text,
    char *error,
    size_t size
)
{
    char current[4096];
    if (!designer_target_text(d, t, current, sizeof current) || !strcmp(text, current)) {
        return DESIGNER_UNCHANGED;
    }
    int element = d->selected;
    bool ok;
    switch (t->kind) {
        case DESIGNER_TARGET_ATTRIBUTE: {
            char name[128];
            designer_attribute_name(
                d->rml,
                &d->rml->elements[element].attributes[t->index],
                name,
                sizeof name
            );
            ok = ui_design_set_attribute(b, element, name, text, error, size);
            break;
        }
        case DESIGNER_TARGET_ID:
            ok = ui_design_set_attribute(b, element, "id", text[0] ? text : NULL, error, size);
            break;
        case DESIGNER_TARGET_TEXT:
            ok = ui_design_set_text(b, element, text, error, size);
            break;
        case DESIGNER_TARGET_STYLE:
        case DESIGNER_TARGET_DECLARATION: {
            value_target v;
            if (!value_target_of(d, t, &v)) {
                return DESIGNER_UNCHANGED;
            }
            ok = write_value(d, b, &v, text, true, error, size);
            break;
        }
        default: return DESIGNER_UNCHANGED;
    }
    return ok ? DESIGNER_CHANGED : DESIGNER_REFUSED;
}

designer_change designer_remove_target(
    studio_designer *d,
    ui_buffer *b,
    const designer_target *t,
    char *error,
    size_t size
)
{
    int element = d->selected;
    bool ok;
    switch (t->kind) {
        case DESIGNER_TARGET_ATTRIBUTE: {
            char name[128];
            if (element < 0 ||
                t->index >= acount(d->rml->elements[element].attributes)) {
                return DESIGNER_UNCHANGED;
            }
            designer_attribute_name(
                d->rml,
                &d->rml->elements[element].attributes[t->index],
                name,
                sizeof name
            );
            ok = ui_design_set_attribute(b, element, name, NULL, error, size);
            break;
        }
        case DESIGNER_TARGET_STYLE:
            ok = ui_design_set_style(b, element, t->property, NULL, false, error, size);
            break;
        case DESIGNER_TARGET_DECLARATION:
            if (!declaration_of(d, t)) {
                return DESIGNER_UNCHANGED;
            }
            ok = ui_design_remove_declaration(b, t->rule, t->index, error, size);
            break;
        default: return DESIGNER_UNCHANGED;
    }
    return ok ? DESIGNER_CHANGED : DESIGNER_REFUSED;
}

designer_change designer_add_value(
    studio_designer *d,
    ui_buffer *b,
    int target,
    designer_add_kind kind,
    const char *text,
    char *error,
    size_t size
)
{
    char name[128], value[256];
    bool element_target = kind != DESIGNER_ADD_DECLARATION;
    if (target < 0 || target >= (element_target ? acount(d->rml->elements)
                                                : acount(d->rcss->rules))) {
        return DESIGNER_UNCHANGED;
    }
    bool ok = false;
    switch (kind) {
        case DESIGNER_ADD_CLASS:
            ok = ui_design_class(b, target, text, true, error, size);
            break;
        case DESIGNER_ADD_ATTRIBUTE:
            split(text, '=', name, sizeof name, value, sizeof value);
            ok = ui_design_set_attribute(b, target, name, value, error, size);
            break;
        case DESIGNER_ADD_STYLE:
        case DESIGNER_ADD_DECLARATION:
            split(text, ':', name, sizeof name, value, sizeof value);
            ok = kind == DESIGNER_ADD_STYLE
                   ? ui_design_set_style(
                         b,
                         target,
                         name,
                         value[0] ? value : designer_default_value(name),
                         false,
                         error,
                         size
                     )
                   : ui_design_add_declaration(
                         b,
                         target,
                         name,
                         value[0] ? value : designer_default_value(name),
                         error,
                         size
                     );
            break;
        case DESIGNER_ADD_EXTRACT:
            ok = ui_design_extract_class(b, target, text, error, size);
            break;
    }
    return ok ? DESIGNER_CHANGED : DESIGNER_REFUSED;
}
