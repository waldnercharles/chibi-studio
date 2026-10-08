/* Controls after Base UI: buttons, checkboxes, toggle groups and selects, text
 * and search fields, panels, property and tree rows, and surfaces; color
 * fields and splitters wrap the native controls (ui/controls/controls.h).
 * Number fields are in kit_number.c. */
#include "editor/rmlui/kit_internal.h"
#include "editor/ui/kit_icons.h"
#include "support/math.h"
#include "support/xalloc.h"
#include "ui/controls/controls.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool locked(const node *n)
{
    return n->w.desc.flags & (KIT_READONLY | KIT_DISABLED);
}

static int int_attribute(const rml_element *el, const char *name)
{
    char *text = element_attribute(el, name);
    int value = atoi(text);
    free(text);
    return value;
}

static void take_value(char **s, const rml_element *el)
{
    char *value = element_value(el);
    str_set(s, value);
    free(value);
}

static rml_element *text_input(rml_element *parent, const char *cls)
{
    rml_element *input = rml_element_create_child(parent, "input", NULL);
    rml_element_set_attribute(input, "type", "text");
    rml_element_set_class_names(input, cls);
    return input;
}

/* ---------- Events ---------- */

static void toggle_click(node *n, rml_element *target)
{
    if (locked(n)) {
        return;
    }
    for (rml_element *el = target; el && el != n->el; el = rml_element_parent(el)) {
        if (rml_element_has_attribute(el, "data-index")) {
            n->w.pending_index = int_attribute(el, "data-index");
            n->w.pending = true;
            return;
        }
    }
}

void control_click(kit *k, node *n, rml_element *target)
{
    switch (n->kind) {
        case KIND_BUTTON:
            if (!rml_element_has_attribute(n->el, "data-disabled")) {
                n->events |= EV_CLICK;
            }
            return;
        case KIND_CHECK:
            if (!locked(n)) {
                n->w.pending = true;
            }
            return;
        case KIND_TOGGLE: toggle_click(n, target); return;
        case KIND_SELECT:
            if (!locked(n)) {
                const char *key = frame_format(k, "select:%s", n->path);
                if (kit_dropdown_is_open(k, key)) {
                    kit_popup_close(k, key);
                } else {
                    dropdown_open_on(k, key, "Search options", n);
                }
            }
            return;
        case KIND_AUTOCOMPLETE:
            if (!locked(n)) {
                autocomplete_open_on(k, frame_format(k, "autocomplete:%s", n->path), n);
            }
            return;
        case KIND_PANEL:
            if (n->w.head_el && within(target, n->w.head_el)) {
                n->w.open = !n->w.open;
            }
            return;
        case KIND_ROW:
            n->events |= n->w.twisty_el && within(target, n->w.twisty_el)
                           ? EV_TWISTY
                           : EV_CLICK;
            return;
        default: return;
    }
}

static void field_key(kit *k, node *n, const rml_event *e)
{
    widget *w = &n->w;
    if (locked(n)) {
        return;
    }
    bool enter = e->key == RML_KEY_RETURN || e->key == RML_KEY_NUMPADENTER;
    if (enter && w->area && (e->modifiers & RML_KM_SHIFT)) {
        rml_context_process_text_input(k->context, "\n", 1);
        rml_event_stop_propagation(e);
        return;
    }
    if (enter) {
        if (n->kind == KIND_TEXT) {
            take_value(&w->draft, w->input_el);
            w->pending = true;
            rml_element_blur(w->input_el);
        }
        rml_event_stop_propagation(e);
        return;
    }
    if (e->key != RML_KEY_ESCAPE) {
        return;
    }
    if (n->kind == KIND_SEARCH) {
        rml_element_set_value(w->input_el, "");
        str_set(&w->draft, "");
        w->pending = true;
    } else {
        rml_element_set_value(w->input_el, w->applied ? w->applied : "");
        str_set(&w->draft, w->applied);
    }
    rml_element_blur(w->input_el);
    rml_event_stop_propagation(e);
}

/* Space and Return press a checkbox or button. */
static void press_key(node *n, const rml_event *e)
{
    if (locked(n) || rml_element_has_attribute(n->el, "data-disabled") ||
        (e->key != RML_KEY_SPACE && e->key != RML_KEY_RETURN &&
         e->key != RML_KEY_NUMPADENTER)) {
        return;
    }
    if (n->kind == KIND_CHECK) {
        n->w.pending = true;
    }
    if (n->kind == KIND_BUTTON) {
        n->events |= EV_CLICK;
    }
    rml_event_stop_propagation(e);
}

static void select_key(kit *k, node *n, const rml_event *e)
{
    if (locked(n)) {
        return;
    }
    switch (e->key) {
        case RML_KEY_SPACE:
        case RML_KEY_RETURN:
        case RML_KEY_NUMPADENTER:
        case RML_KEY_DOWN:
        case RML_KEY_UP:
            control_click(k, n, n->el);
            rml_event_stop_propagation(e);
            return;
        default: return;
    }
}

static void toggle_key(node *n, const rml_event *e)
{
    if (locked(n)) {
        return;
    }
    int count = acount(n->w.labels);
    if (!count || !rml_element_has_attribute(e->target, "data-index")) {
        return;
    }
    int at = int_attribute(e->target, "data-index");
    switch (e->key) {
        case RML_KEY_SPACE:
        case RML_KEY_RETURN:
        case RML_KEY_NUMPADENTER: break;
        case RML_KEY_LEFT:
        case RML_KEY_UP: at = (at + count - 1) % count; break;
        case RML_KEY_RIGHT:
        case RML_KEY_DOWN: at = (at + 1) % count; break;
        case RML_KEY_HOME: at = 0; break;
        case RML_KEY_END: at = count - 1; break;
        default: return;
    }
    n->w.pending_index = at;
    n->w.pending = true;
    rml_element_focus(rml_element_child(n->el, at), true);
    rml_event_stop_propagation(e);
}

void control_key(kit *k, node *n, const rml_event *e)
{
    switch (n->kind) {
        case KIND_TEXT:
        case KIND_SEARCH: field_key(k, n, e); return;
        case KIND_SELECT: select_key(k, n, e); return;
        case KIND_TOGGLE: toggle_key(n, e); return;
        case KIND_AUTOCOMPLETE: return;
        case KIND_CHECK:
        case KIND_BUTTON: press_key(n, e); return;
        default: return;
    }
}

static bool field_input(const node *n, const rml_element *target)
{
    return (n->kind == KIND_TEXT || n->kind == KIND_SEARCH ||
            n->kind == KIND_AUTOCOMPLETE) &&
           target == n->w.input_el;
}

void control_change(kit *k, node *n, rml_element *target)
{
    if (field_input(n, target)) {
        take_value(&n->w.draft, target);
        if (n->kind == KIND_SEARCH) {
            n->w.pending = true;
        }
        if (n->kind == KIND_AUTOCOMPLETE) {
            n->w.autocomplete_query_active = true;
            n->w.autocomplete_submitted = false;
            n->w.autocomplete_canceled = false;
            autocomplete_open_on(k, frame_format(k, "autocomplete:%s", n->path), n);
        }
    }
}

void control_focus(kit *k, node *n, rml_element *target)
{
    if (!field_input(n, target)) {
        return;
    }
    n->w.focused = true;
    set_flag(n, "data-editing", true);
    if (n->kind == KIND_AUTOCOMPLETE && !locked(n)) {
        autocomplete_open_on(k, frame_format(k, "autocomplete:%s", n->path), n);
    }
}

void control_blur(node *n, rml_element *target)
{
    if (!field_input(n, target)) {
        return;
    }
    n->w.focused = false;
    set_flag(n, "data-editing", false);
    if (n->kind == KIND_TEXT && !str_eq(n->w.draft, n->w.applied)) {
        n->w.pending = true;
    }
}

/* ---------- Controls ---------- */

/* Button variants and sizes (cva): the only place their classes are named. */
static const char *const BUTTON_VARIANTS[] = { "", "subtle", "primary", "outline", "danger" };
static const char *const BUTTON_SIZES[] = { "", "sm" };

bool kit_button(kit *k, const char *key, const kit_button_desc *d)
{
    bool square = d->icon && !(d->label && d->label[0]);
    const kit_class list[] = {
        { BUTTON_VARIANTS[d->variant], true },
        { BUTTON_SIZES[d->size], true },
        { "icon-only", square },
        { d->cls, d->cls != NULL },
    };
    const char *cls = kit_classes(k, "btn", list, 4);
    node *n = emit(k, key, KIND_BUTTON, "button", cls);
    widget *w = &n->w;
    if (!w->label_el) {
        w->icon_el = raw_icon(n->el, "", 0);
        w->label_el = raw(n->el, d->morph ? "morph-text" : "span", "label", NULL);
    }
    if (w->icon != d->icon) {
        w->icon = d->icon;
        char icon[5];
        utf8(icon, d->icon);
        rml_element_set_inner_rml(w->icon_el, icon);
        rml_element_set_property(w->icon_el, "display", d->icon ? "inline-block" : "none");
    }
    if (str_set(&w->label, d->label)) {
        bool morph = !strcmp(rml_element_tag_name(w->label_el), "morph-text");
        assert(morph == d->morph);
        if (morph) {
            morph_text_set(w->label_el, w->label);
        } else {
            set_inner_text(w->label_el, w->label);
        }
        rml_element_set_property(w->label_el, "display", w->label[0] ? "inline-block" : "none");
    }
    set_flag(n, "data-disabled", d->flags & KIT_DISABLED);
    if (d->flags & KIT_ON) {
        set_attr(n, "data-state", "on");
    } else {
        set_flag(n, "data-state", false);
    }
    set_tip(n, d->tip, d->shortcut, d->description);
    return (n->events & EV_CLICK) && !(d->flags & KIT_DISABLED);
}

bool kit_checkbox(kit *k, const char *key, bool *value, const char *label, unsigned flags)
{
    node *n = emit(k, key, KIND_CHECK, "div", "check");
    widget *w = &n->w;
    if (!w->label_el) {
        rml_element *box = raw(n->el, "span", "box", NULL);
        raw_icon(box, "", KIT_ICON_CHECK);
        raw_icon(box, "dash", KIT_ICON_DASH);
        w->label_el = raw(n->el, "span", "label", NULL);
    }
    w->desc.flags = flags;
    if (str_set(&w->label, label)) {
        set_inner_text(w->label_el, w->label);
        rml_element_set_property(w->label_el, "display", w->label[0] ? "inline" : "none");
    }
    bool changed = false;
    if (w->pending && !locked(n)) {
        w->pending = false;
        *value = !*value;
        changed = true;
    }
    w->pending = false;
    set_flag(n, "data-checked", *value);
    set_flag(n, "data-mixed", (flags & KIT_MIXED) && !changed);
    set_flag(n, "data-disabled", flags & KIT_DISABLED);
    set_flag(n, "data-readonly", flags & KIT_READONLY);
    return changed;
}

static bool labels_equal(char **have, const char *const *labels, int count)
{
    if (acount(have) != count) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!str_eq(have[i], labels[i])) {
            return false;
        }
    }
    return true;
}

static void set_segments(node *n, const char *const *labels, int count)
{
    widget *w = &n->w;
    for (int i = 0; i < acount(w->labels); i++) {
        free(w->labels[i]);
    }
    aclear(w->labels);
    for (int i = 0; i < count; i++) {
        apush(w->labels, xstrdup(labels[i]));
    }
    while (rml_element_child_count(n->el)) {
        rml_element_remove_child(n->el, rml_element_child(n->el, 0));
    }
    for (int i = 0; i < count; i++) {
        rml_element *seg = raw(n->el, "button", "seg", labels[i]);
        char index[16];
        snprintf(index, sizeof index, "%d", i);
        rml_element_set_attribute(seg, "data-index", index);
    }
}

static bool choice_toggle(kit *k, const char *key, int *value, const char *const *labels, int count, unsigned flags)
{
    node *n = emit(k, key, KIND_TOGGLE, "div", "segmented");
    widget *w = &n->w;
    w->desc.flags = flags;
    if (!labels_equal(w->labels, labels, count)) {
        set_segments(n, labels, count);
    }
    bool changed = false;
    if (w->pending && !locked(n) && w->pending_index >= 0 && w->pending_index < count) {
        changed = *value != w->pending_index;
        *value = w->pending_index;
    }
    w->pending = false;
    for (int i = 0; i < count; i++) {
        rml_element *segment = rml_element_child(n->el, i);
        flag_element(segment, "data-pressed", i == *value);
        flag_element(segment, "disabled", flags & KIT_DISABLED);
        rml_element_set_property(segment, "tab-index", flags & KIT_DISABLED ? "none" : "auto");
    }
    set_flag(n, "data-readonly", flags & KIT_READONLY);
    set_flag(n, "data-disabled", flags & KIT_DISABLED);
    return changed;
}

static bool choice_select_items(
    kit *k,
    const char *key,
    int *value,
    const kit_command_item *items,
    int count,
    unsigned flags,
    const char *closed_label
)
{
    node *n = emit(k, key, KIND_SELECT, "div", "field select");
    widget *w = &n->w;
    w->desc.flags = flags;
    if (!w->value_el) {
        w->value_el = raw(n->el, "span", "part value", NULL);
        raw_icon(n->el, "caret", KIT_ICON_DOWN);
    }
    set_element_text(w->value_el, flags & KIT_MIXED ? "\xe2\x80\x94" : closed_label);
    set_flag(n, "data-mixed", flags & KIT_MIXED);
    set_flag(n, "data-readonly", flags & KIT_READONLY);
    set_flag(n, "data-disabled", flags & KIT_DISABLED);
    rml_element_set_property(n->el, "tab-index", flags & KIT_DISABLED ? "none" : "auto");
    const char *menu = frame_format(k, "select:%s", n->path);
    if (locked(n)) {
        kit_popup_close(k, menu);
    }
    bool open = kit_dropdown_is_open(k, menu);
    set_flag(n, "data-open", open);
    bool changed = false;
    int selected = -1;
    if (open) {
        selected = dropdown_items(k, menu, items, count);
    }
    if (!locked(n) && selected >= 0 && selected < count) {
        changed = *value != selected;
        *value = selected;
    }
    k->last = n;
    return changed;
}

static bool choice_select(kit *k, const char *key, int *value, const char *const *labels, int count, unsigned flags)
{
    node *parent = alast(k->stack);
    const char *path = str_eq(parent->path, "")
                         ? key
                         : frame_format(k, "%s/%s", parent->path, key);
    bool open = kit_dropdown_is_open(k, frame_format(k, "select:%s", path));
    kit_command_item *items = NULL;
    if (open) {
        for (int i = 0; i < count; i++) {
            apush(items, ((kit_command_item){ .label = labels[i], .current = i == *value }));
        }
    }
    const char *current = *value >= 0 && *value < count ? labels[*value] : "-";
    bool changed = choice_select_items(k, key, value, items, open ? count : 0, flags, current);
    afree(items);
    return changed;
}

bool kit_toggle_group(kit *k, const char *key, int *value, const char *const *labels, int count, unsigned flags)
{
    return choice_toggle(k, key, value, labels, count, flags);
}

bool kit_choice(kit *k, const char *key, int *value, const char *const *labels, int count, unsigned flags)
{
    if (count <= 3) {
        return kit_toggle_group(k, key, value, labels, count, flags);
    }
    return choice_select(k, key, value, labels, count, flags);
}

node *emit_input(kit *k, const char *key, node_kind kind, const char *cls, const char *placeholder)
{
    node *n = emit(k, key, kind, "div", cls);
    widget *w = &n->w;
    if (!w->input_el) {
        if (kind == KIND_SEARCH) {
            rml_element *addon = raw(n->el, "span", "part addon", NULL);
            raw_icon(addon, "sm", KIT_ICON_SEARCH);
        }
        w->area = !strcmp(cls, "field text area");
        w->input_el = w->area ? raw(n->el, "textarea", "part value", NULL)
                              : text_input(n->el, "part value");
    }
    if (!str_eq(w->placeholder, placeholder)) {
        str_set(&w->placeholder, placeholder);
        rml_element_set_attribute(w->input_el, "placeholder", w->placeholder);
    }
    return n;
}

static bool edit_text(node *n, char *buffer, size_t size, unsigned flags)
{
    widget *w = &n->w;
    bool secret = flags & KIT_SECRET;
    if (w->secret != secret) {
        rml_element_set_attribute(w->input_el, "type", secret ? "password" : "text");
        w->secret = secret;
    }
    w->desc.flags = flags;
    set_flag(n, "data-readonly", flags & KIT_READONLY);
    set_flag(n, "data-disabled", flags & KIT_DISABLED);
    set_flag(n, "data-mixed", flags & KIT_MIXED);
    flag_element(w->input_el, "disabled", flags & KIT_DISABLED);
    rml_element_set_property(w->input_el, "tab-index", flags & KIT_DISABLED ? "none" : "auto");
    if (locked(n)) {
        rml_element_set_attribute(w->input_el, "readonly", "");
    } else {
        rml_element_remove_attribute(w->input_el, "readonly");
    }
    bool committed = false;
    if (w->pending) {
        w->pending = false;
        committed = !locked(n);
        w->committed = committed;
        if (committed && !str_eq(w->draft, buffer)) {
            snprintf(buffer, size, "%s", w->draft ? w->draft : "");
        }
    }
    if (!w->focused && !str_eq(w->applied, buffer)) {
        str_set(&w->applied, buffer);
        str_set(&w->draft, buffer);
        rml_element_set_value(w->input_el, buffer);
    }
    return committed;
}

bool kit_text_field(kit *k, const char *key, char *buffer, size_t size, const char *placeholder, unsigned flags)
{
    return edit_text(emit_input(k, key, KIND_TEXT, "field text", placeholder), buffer, size, flags);
}

bool kit_text_area(kit *k, const char *key, char *buffer, size_t size, const char *placeholder, unsigned flags)
{
    enum
    {
        MAX_ROWS = 8,
    };
    node *n = emit_input(k, key, KIND_TEXT, "field text area", placeholder);
    bool committed = edit_text(n, buffer, size, flags);
    int rows = 1;
    for (const char *c = n->w.draft ? n->w.draft : ""; *c && rows < MAX_ROWS; c++) {
        rows += *c == '\n';
    }
    if (int_attribute(n->w.input_el, "rows") != rows) {
        char text[8];
        snprintf(text, sizeof text, "%d", rows);
        rml_element_set_attribute(n->w.input_el, "rows", text);
    }
    return committed;
}

bool kit_search(kit *k, const char *key, char *buffer, size_t size, const char *placeholder)
{
    node *n = emit_input(k, key, KIND_SEARCH, "field search", placeholder);
    widget *w = &n->w;
    bool changed = false;
    if (w->pending) {
        w->pending = false;
        if (!str_eq(w->draft, buffer)) {
            snprintf(buffer, size, "%s", w->draft ? w->draft : "");
            changed = true;
        }
    }
    if (w->focused) {
        return changed;
    }
    char *shown = element_value(w->input_el);
    if (strcmp(shown, buffer) != 0) {
        rml_element_set_value(w->input_el, buffer);
        str_set(&w->draft, buffer);
    }
    free(shown);
    return changed;
}

/* The native <color-field>: its swatch opens its own picker. */
bool kit_color(kit *k, const char *key, uint32_t *rgba, unsigned flags)
{
    node *n = emit(k, key, KIND_COLOR, "color-field", "field color");
    n->w.desc.flags = flags;
    uint32_t color = *rgba;
    bool committed;
    bool changed = color_field_take(n->el, &color, &committed) && color != *rgba;
    *rgba = color;
    n->w.committed = committed;
    char hex[16];
    snprintf(hex, sizeof hex, "#%08x", *rgba);
    set_attr(n, "value", hex);
    /* The caller writes a changed color everywhere this frame. */
    flag_element(n->el, "mixed", (flags & KIT_MIXED) && !changed);
    set_flag(n, "readonly", flags & KIT_READONLY);
    set_flag(n, "disabled", flags & KIT_DISABLED);
    set_flag(n, "data-overridden", flags & KIT_OVERRIDDEN);
    const node *property = n->parent;
    while (property && property->kind != KIND_PROP) {
        property = property->parent;
    }
    set_attr(n, "aria-label", property ? property->w.label : "Color picker");
    return changed;
}

bool kit_panel_begin(kit *k, const char *key, const char *title, const char *badge, bool default_open)
{
    node *n = emit(k, key, KIND_PANEL, "section", "panel");
    widget *w = &n->w;
    if (!w->head_el) {
        w->head_el = raw(n->el, "div", "panel-head", NULL);
        raw_icon(w->head_el, "chev", KIT_ICON_DOWN);
        w->label_el = raw(w->head_el, "span", "title", NULL);
        w->badge_el = raw(w->head_el, "span", "badge override", NULL);
        raw(w->head_el, "span", "spacer", NULL);
        raw_icon(w->head_el, "grip", KIT_ICON_GRIP);
        w->open = default_open;
    }
    if (str_set(&w->label, title)) {
        set_inner_text(w->label_el, w->label);
    }
    if (str_set(&w->badge, badge)) {
        set_inner_text(w->badge_el, w->badge);
        rml_element_set_property(w->badge_el, "display", w->badge[0] ? "inline-block" : "none");
    }
    set_flag(n, "data-open", w->open);
    push_node(k, n);
    if (!w->open) {
        return false;
    }
    push_node(k, emit(k, "@body", KIND_BODY, "div", "panel-body"));
    return true;
}

void kit_panel_end(kit *k)
{
    if (alast(k->stack)->kind == KIND_BODY) {
        pop_node(k);
    }
    assert(alast(k->stack)->kind == KIND_PANEL);
    pop_node(k);
}

void kit_prop_begin(kit *k, const char *key, const char *label)
{
    node *n = emit(k, key, KIND_PROP, "div", "prop");
    if (!n->w.label_el) {
        rml_element *l = raw(n->el, "label", "label", NULL);
        n->w.label_el = raw(l, "span", "", NULL);
    }
    if (str_set(&n->w.label, label)) {
        set_inner_text(n->w.label_el, n->w.label);
    }
    push_node(k, n);
    push_node(k, emit(k, "@control", KIND_BODY, "div", "control"));
}

static rml_element *label_controls(kit *k, node *n, const char *label)
{
    if (n->kind == KIND_PROP) {
        label = n->w.label;
    }
    switch (n->kind) {
        case KIND_NUMBER:
        case KIND_CHECK:
        case KIND_TOGGLE:
        case KIND_SELECT:
        case KIND_TEXT:
        case KIND_SEARCH:
        case KIND_AUTOCOMPLETE:
        case KIND_COLOR: {
            const char *name = n->w.addon && n->w.addon[0]
                                 ? frame_format(k, "%s %s", label, n->w.addon)
                                 : label;
            rml_element_set_attribute(n->el, "aria-label", name);
            rml_element *target = n->w.input_el ? n->w.input_el : n->el;
            if (n->kind == KIND_TOGGLE && rml_element_child_count(n->el)) {
                target = rml_element_child(n->el, 0);
            }
            if (n->kind != KIND_TOGGLE) {
                rml_element_set_attribute(target, "aria-label", name);
            }
            if (locked(n)) {
                return NULL;
            }
            if (!rml_element_has_attribute(target, "id")) {
                rml_element_set_attribute(
                    target,
                    "id",
                    frame_format(k, "%s:%s", target == n->el ? "kit-field" : "kit-input", n->path)
                );
            }
            return target;
        }
        default: break;
    }
    rml_element *first = NULL;
    for (int i = 0; i < acount(n->children); i++) {
        rml_element *target = label_controls(k, n->children[i], label);
        if (!first) {
            first = target;
        }
    }
    return first;
}

void kit_prop_end(kit *k)
{
    pop_node(k);
    node *n = alast(k->stack);
    rml_element *label = rml_element_parent(n->w.label_el);
    rml_element *target = label_controls(k, n, n->w.label);
    if (target) {
        char *id = element_attribute(target, "id");
        rml_element_set_attribute(label, "for", id);
        free(id);
    } else {
        rml_element_remove_attribute(label, "for");
    }
    pop_node(k);
}

static void set_twisty(rml_element *el, int twisty)
{
    uint32_t cp = twisty < 0 ? 0 : twisty ? KIT_ICON_DOWN : KIT_ICON_RIGHT;
    char text[5];
    utf8(text, cp);
    char *shown = element_inner_rml(el);
    if (strcmp(shown, text) != 0) {
        rml_element_set_inner_rml(el, text);
    }
    free(shown);
}

static unsigned row_events(const node *n)
{
    return (n->events & EV_CLICK ? KIT_ROW_CLICK : 0u) |
           (n->events & EV_DOUBLE ? KIT_ROW_DOUBLE_CLICK : 0u) |
           (n->events & EV_CONTEXT ? KIT_ROW_CONTEXT : 0u) |
           (n->events & EV_TWISTY ? KIT_ROW_TWISTY : 0u);
}

unsigned kit_row_begin(kit *k, const char *key, const kit_row_desc *d)
{
    node *n = emit(k, key, KIND_ROW, "div", "tree-row");
    widget *w = &n->w;
    if (!w->label_el) {
        w->twisty_el = raw_icon(n->el, "twisty sm", 0);
        w->icon_el = raw_icon(n->el, "", 0);
        w->label_el = raw(n->el, "span", "name", NULL);
        w->meta_el = raw(n->el, "span", "meta", NULL);
    }
    set_twisty(w->twisty_el, d->twisty);
    if (w->icon != d->icon) {
        w->icon = d->icon;
        char icon[5];
        utf8(icon, d->icon);
        rml_element_set_inner_rml(w->icon_el, icon);
    }
    if (str_set(&w->label, d->label)) {
        set_inner_text(w->label_el, w->label);
    }
    if (str_set(&w->meta, d->meta)) {
        set_inner_text(w->meta_el, w->meta);
    }
    char pad[32];
    snprintf(pad, sizeof pad, "%ddp", 6 + d->depth * 14);
    rml_element_set_property(n->el, "padding-left", pad);
    set_flag(n, "data-selected", d->flags & KIT_ON);
    set_flag(n, "data-active", d->flags & KIT_ACTIVE);
    unsigned result = row_events(n);
    push_node(k, n);
    push_node(k, emit(k, "@trail", KIND_BODY, "span", "trail"));
    return result;
}

void kit_row_end(kit *k)
{
    pop_node(k);
    pop_node(k);
}

/* ---------- Surfaces ---------- */

rml_element *kit_surface(kit *k, const char *key, const char *cls, const char *texture, kit_surface_input *input)
{
    node *n = emit(k, key, KIND_SURFACE, "img", cls);
    set_attr(n, "src", frame_format(k, "kit:%s", texture));
    kit_surface_input out = { .rect = element_rect(k, n->el) };
    out.x = k->in.mouse_x - out.rect.x;
    out.y = k->in.mouse_y - out.rect.y;
    out.hovered = rml_context_hover_element(k->context) == n->el &&
                  rect_contains(out.rect, k->in.mouse_x, k->in.mouse_y);
    bool captured = str_eq(k->surface_capture, n->path);
    /* A surface holding a press still hears the other buttons' presses, as a
       right click cancels a drag. */
    for (int b = 0; b < 3; b++) {
        if (!k->in.pressed[b]) {
            continue;
        }
        float press_x = k->in.press_position[b] ? k->in.press_x[b] : k->in.mouse_x;
        float press_y = k->in.press_position[b] ? k->in.press_y[b] : k->in.mouse_y;
        bool press_inside = rect_contains(out.rect, press_x, press_y);
        bool press_target = k->in.press_position[b]
                              ? rml_context_element_at_point(
                                    k->context,
                                    (rml_vec2){ press_x * dp(k), press_y * dp(k) },
                                    NULL
                                ) == n->el
                              : out.hovered;
        /* The first button must have started on this surface. Once it owns
           capture, later buttons can still cancel or modify that gesture. */
        if (!captured && (!press_inside || !press_target)) {
            continue;
        }
        out.pressed[b] = true;
        out.press_position[b] = k->in.press_position[b];
        out.press_x[b] = press_x - out.rect.x;
        out.press_y[b] = press_y - out.rect.y;
        if (!captured) {
            str_set(&k->surface_capture, n->path);
            k->surface_button = b;
            captured = true;
        }
    }
    if (captured) {
        int b = k->surface_button;
        out.down[b] = k->in.down[b];
        out.released[b] = k->in.released[b];
        if (out.released[b] || !k->in.down[b]) {
            str_set(&k->surface_capture, "");
            k->surface_button = -1;
        }
    }
    out.wheel = out.hovered ? k->in.wheel : 0;
    *input = out;
    return n->el;
}

/* ---------- UI wiring (workstream K) ---------- */

bool kit_picker(kit *k, const char *key, const kit_picker_desc *d)
{
    node *n = emit(k, key, KIND_BUTTON, "div", "field select picker");
    widget *w = &n->w;
    if (!w->value_el) {
        w->icon_el = raw_icon(n->el, "lead", 0);
        w->value_el = raw(n->el, "span", "part value", NULL);
        raw_icon(n->el, "caret", KIT_ICON_DOWN);
    }
    if (w->icon != d->icon) {
        w->icon = d->icon;
        char icon[5];
        utf8(icon, d->icon);
        rml_element_set_inner_rml(w->icon_el, icon);
        rml_element_set_property(w->icon_el, "display", d->icon ? "inline-block" : "none");
    }
    bool empty = !d->value || !d->value[0];
    set_element_text(w->value_el, empty ? d->placeholder : d->value);
    set_flag(n, "data-placeholder", empty);
    set_flag(n, "data-readonly", d->flags & KIT_READONLY);
    set_flag(n, "data-disabled", d->flags & KIT_DISABLED);
    set_flag(n, "data-invalid", d->flags & KIT_INVALID);
    set_tip(n, d->tip, NULL, NULL);
    return (n->events & EV_CLICK) && !(d->flags & (KIT_READONLY | KIT_DISABLED));
}
