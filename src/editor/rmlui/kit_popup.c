/* Popups, menus and tooltips after Base UI. The kit emits a popup's content
 * while it is open; ui/popover.c stacks it with authored popovers, places it
 * below its anchor, beside its parent menu's row or at a point, closes it on
 * an outside press, Escape or Tab or an item's activation, and runs menus:
 * highlight, typeahead, submenus after a 100 ms hover and the menubar. A
 * dropdown's or an autocomplete's search field keeps its own highlight here.
 * A tooltip shows after a 500 ms hover, switches instantly within 400 ms of
 * the last one, and hides while a button is held or a popup or the palette
 * is open. It holds a label, a shortcut and an optional description. */
#include "editor/rmlui/kit_internal.h"
#include "editor/ui/kit_icons.h"
#include "support/math.h"
#include "support/xalloc.h"
#include "ui/popover.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A menu is as wide as its widest row and the popup's padding, in bounds. */
static const float popup_chrome = 10, menu_min = 180, menu_max = 480;
static const float autocomplete_chrome = 8;
/* Tooltip text in dp on screen, and its distance from the target. */
static const float tip_size = 11, tip_gap = 6;

/* ---------- Opening and closing ---------- */

static bool shown_open(const popup *p)
{
    return p->node && popover_open(p->node->el);
}

void close_popup(kit *k, const char *key)
{
    popup *p = popup_find(k, key);
    if (!p || !p->open) {
        return;
    }
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *child = k->popups[i];
        if (child->open && str_eq(child->parent, key)) {
            close_popup(k, child->key);
        }
    }
    p->open = false;
    p->shown = false;
    p->highlight = -1;
    /* Activation closes after emission; stop hit testing before the next frame
       retires the popup so another click can reach the control beneath it. */
    if (p->node) {
        popover_hide(p->node->el);
        rml_element_set_property(p->node->el, "pointer-events", "none");
    }
}

/* Closes the kit's popups; popover.c closes what a new one did not open
   from as it shows. */
static void close_kit_popups(kit *k)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        close_popup(k, k->popups[i]->key);
    }
}

void close_all_popups(kit *k)
{
    close_kit_popups(k);
    popover_hide_all(k->context);
}

/* Popups popover.c closed, by an outside press, Escape, Tab or an item's
   activation, close here once their last frame emitted. */
void forget_closed_popups(kit *k)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        if (p->open && p->shown && !shown_open(p)) {
            close_popup(k, p->key);
        }
    }
}

popup *open_popup(kit *k, const char *key, const char *parent)
{
    popup *p = popup_at(k, key);
    if (p->open) {
        close_popup(k, p->key);
    }
    free(p->parent);
    free(p->anchor_path);
    free(p->dropdown_placeholder);
    free(p->dropdown_trigger_path);
    free(p->trigger);
    afree(p->items);
    rml_element *focus = rml_context_focus_element(k->context);
    node *trigger = node_for(k, focus);
    *p = (popup){
        .key = p->key,
        .open = true,
        .keyboard = k->keyboard_edit,
        .highlight = -1,
    };
    str_set(&p->trigger, trigger ? trigger->path : "");
    str_set(&p->parent, parent);
    return p;
}

/* The open popup holding `trigger`; the kit's popups are siblings in its
   layer, so at most one does. */
static popup *popup_for_trigger(const kit *k, const node *trigger)
{
    for (int i = 0; trigger && i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        if (p->open && p->node && within(trigger->el, p->node->el)) {
            return p;
        }
    }
    return NULL;
}

static void close_children(kit *k, const popup *owner)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *child = k->popups[i];
        if (child->open && str_eq(child->parent, owner->key)) {
            close_popup(k, child->key);
        }
    }
}

popup *popup_open_on(kit *k, const char *key, const node *anchor, const node *trigger)
{
    popup *owner = popup_for_trigger(k, trigger);
    if (owner) {
        close_children(k, owner);
    } else {
        close_kit_popups(k);
    }
    char *parent = owner ? xstrdup(owner->key) : NULL;
    popup *p = open_popup(k, key, parent);
    free(parent);
    str_set(&p->anchor_path, anchor ? anchor->path : NULL);
    if (anchor) {
        p->anchor = element_rect(k, anchor->el);
    }
    return p;
}

popup *dropdown_open_on(kit *k, const char *key, const char *placeholder, const node *trigger)
{
    popup *existing = popup_find(k, key);
    if (existing && existing->open && existing->dropdown && trigger &&
        str_eq(existing->dropdown_trigger_path, trigger->path)) {
        close_popup(k, key);
        rml_element_focus(trigger->el, true);
        return NULL;
    }
    popup *p = popup_open_on(k, key, trigger, trigger);
    p->dropdown = true;
    p->dropdown_focus = true;
    str_set(&p->dropdown_placeholder, placeholder ? placeholder : "Search…");
    if (trigger) {
        str_set(&p->dropdown_trigger_path, trigger->path);
    }
    return p;
}

void autocomplete_open_on(kit *k, const char *key, node *trigger)
{
    popup *existing = popup_find(k, key);
    if (existing && existing->open) {
        return;
    }
    popup *p = popup_open_on(k, key, trigger, trigger);
    p->dropdown = true;
    p->autocomplete = true;
    p->autocomplete_opening = true;
    str_set(&p->dropdown_trigger_path, trigger->path);
}

/* ---------- Showing ---------- */

/* The role that tells popover.c how it behaves: a menu, a list whose search
   field keeps the focus, or a popover that takes it. */
static const char *popup_role(const popup *p, rml_element *el)
{
    if (p->dropdown) {
        return "listbox";
    }
    bool menu = rml_element_is_class_set(el, "menu") &&
                !rml_element_is_class_set(el, "popover");
    return menu ? "menu" : "dialog";
}

/* Stacks the popup with popover.c as it first emits. */
static void show_popup(kit *k, popup *p, node *n)
{
    node *anchor = p->anchor_path ? path_node(k, p->anchor_path) : NULL;
    node *trigger = path_node(k, p->dropdown_trigger_path ? p->dropdown_trigger_path : p->trigger);
    float r = dp(k);
    popover_anchor at = {
        .side = p->at_point ? POPOVER_POINT
              : p->beside   ? POPOVER_BESIDE
                            : POPOVER_BELOW,
        .element = anchor ? anchor->el : NULL,
        .box = { p->anchor.x * r, p->anchor.y * r, p->anchor.w * r, p->anchor.h * r },
        .trigger = trigger ? trigger->el : NULL,
        .keyboard = p->keyboard,
    };
    if (p->at_point) {
        at.box[0] = p->x * r;
        at.box[1] = p->y * r;
    }
    popover_show(n->el, &at);
    p->shown = true;
}

/* Field choices retain their row limit; action menus only stop at the window. */
static void limit_popup_height(kit *k, popup *p)
{
    rml_element *list = NULL;
    float cap = 0;
    if (p->dropdown || p->autocomplete) {
        list = rml_element_query_selector(p->node->el, ".dropdown-results, .autocomplete-results");
        float chrome = p->autocomplete ? autocomplete_chrome
                                       : popup_chrome + kit_metric(k, "--h") + 8;
        float rows = p->autocomplete ? 7 * kit_metric(k, "--autocomplete-row-h")
                                     : kit_metric(k, "--dropdown-list-h");
        cap = math_min(rows, math_max(1.0f, k->in.height - 8 - chrome));
    } else if (
        rml_element_is_class_set(p->node->el, "menu") &&
        !rml_element_is_class_set(p->node->el, "struct-editor")
    ) {
        list = p->node->el;
        cap = math_max(1.0f, k->in.height - 8);
    }
    if (list) {
        char value[32];
        snprintf(value, sizeof value, "%.0fdp", cap);
        rml_element_set_property(list, "max-height", value);
    }
}

void size_popups(kit *k)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        if (!p->open || !p->node) {
            continue;
        }
        limit_popup_height(k, p);
        if (p->dropdown_reveal && p->highlight >= 0 && p->highlight < acount(p->items)) {
            rml_element_scroll_into_view(p->items[p->highlight]->el, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
            p->dropdown_reveal = false;
        }
    }
}

/* ---------- A dropdown's highlight ---------- */

static void highlight_item(popup *p, int index)
{
    p->highlight = index;
    for (int i = 0; i < acount(p->items); i++) {
        if (p->items[i]) {
            flag_element(p->items[i]->el, "data-highlighted", i == index);
        }
    }
}

static bool item_enabled(const node *n)
{
    return n && !rml_element_has_attribute(n->el, "data-disabled");
}

static node *highlighted(const popup *p)
{
    bool valid = p->highlight >= 0 && p->highlight < acount(p->items);
    return valid ? p->items[p->highlight] : NULL;
}

/* Highlights the next enabled item in dir, wrapping. */
static void move_highlight(popup *p, int dir)
{
    int count = acount(p->items);
    for (int step = 1; step <= count; step++) {
        int i = ((p->highlight + dir * step) % count + count) % count;
        if (item_enabled(p->items[i])) {
            highlight_item(p, i);
            rml_element_scroll_into_view(p->items[i]->el, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
            return;
        }
    }
}

/* Activation is a click, as popover.c closes the popup on one. */
static void activate_item(node *item)
{
    if (item_enabled(item)) {
        rml_element_click(item->el);
    }
}

/* ---------- Events ---------- */

/* A press outside an autocomplete's popup and field ends its editing. */
void popup_press(kit *k, rml_element *target)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        node *field = p->open && p->autocomplete
                        ? path_node(k, p->dropdown_trigger_path)
                        : NULL;
        bool outside = field && !within(target, field->el) &&
                       !(p->node && within(target, p->node->el));
        if (outside) {
            rml_element_blur(field->w.input_el);
        }
    }
}

/* A click on an item reports it; on a submenu's row it opens the submenu. */
void menu_click(kit *k, node *n)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        if (!p->open || nodes_index(p->items, n) < 0 || !item_enabled(n)) {
            continue;
        }
        if (n->kind != KIND_SUBMENU) {
            n->w.pending = true;
            return;
        }
        const char *sub = frame_format(k, "sub:%s", n->path);
        if (!kit_popup_is_open(k, sub)) {
            close_children(k, p);
            popup *child = open_popup(k, sub, p->key);
            child->beside = true;
            str_set(&child->anchor_path, n->path);
            str_set(&child->trigger, n->path);
        }
        return;
    }
}

/* The pointer highlights a dropdown's rows, popover.c a menu's; a disabled
   row never highlights. */
void menu_hover(kit *k, node *n)
{
    if (n->kind != KIND_MENU_ITEM) {
        return;
    }
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        int at = p->open && p->dropdown ? nodes_index(p->items, n) : -1;
        if (at >= 0) {
            highlight_item(p, item_enabled(n) ? at : -1);
        }
    }
}

static popup *open_dropdown(const kit *k, const char *field_path)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        if (p->open && p->dropdown && str_eq(p->dropdown_trigger_path, field_path)) {
            return p;
        }
    }
    return NULL;
}

/* The search field owns ordinary typing. Its dropdown still owns navigation
 * and selection keys, so Up/Down and Enter work without leaving the field. */
bool dropdown_key(kit *k, const rml_event *e, node *n)
{
    rml_element *top = popover_top(k->context);
    node *shown = top ? node_for(k, top) : NULL;
    popup *p = NULL;
    for (int i = 0; shown && i < cf_map_size(k->popups) && !p; i++) {
        p = k->popups[i]->node == shown && k->popups[i]->dropdown ? k->popups[i] : NULL;
    }
    if (!p || !n || n->kind != KIND_SEARCH || e->target != p->dropdown_search) {
        return false;
    }
    switch (e->key) {
        case RML_KEY_DOWN: move_highlight(p, 1); break;
        case RML_KEY_UP: move_highlight(p, -1); break;
        case RML_KEY_RETURN:
        case RML_KEY_NUMPADENTER: activate_item(highlighted(p)); break;
        default: return false;
    }
    rml_event_stop_propagation(e);
    return true;
}

bool autocomplete_key(kit *k, const rml_event *e, node *n)
{
    if (!n || n->kind != KIND_AUTOCOMPLETE || e->target != n->w.input_el) {
        return false;
    }
    if (n->w.desc.flags & (KIT_READONLY | KIT_DISABLED)) {
        return false;
    }
    const char *key = frame_format(k, "autocomplete:%s", n->path);
    popup *p = open_dropdown(k, n->path);
    const char *query = n->w.autocomplete_query_active ? n->w.draft : "";
    bool listed = p && str_eq(p->dropdown_query, query) && highlighted(p) &&
                  item_enabled(highlighted(p));
    switch (e->key) {
        case RML_KEY_DOWN:
        case RML_KEY_UP: {
            if (!p) {
                autocomplete_open_on(k, key, n);
                p = popup_find(k, key);
            }
            p->keyboard = true;
            if (str_eq(p->dropdown_query, query)) {
                move_highlight(p, e->key == RML_KEY_DOWN ? 1 : -1);
            } else {
                p->highlight = -1;
            }
            node *item = highlighted(p);
            if (item) {
                rml_element_set_attribute(n->w.input_el, "aria-activedescendant", item->path);
            } else {
                rml_element_remove_attribute(n->w.input_el, "aria-activedescendant");
            }
            rml_event_stop_propagation(e);
            return true;
        }
        case RML_KEY_RETURN:
        case RML_KEY_NUMPADENTER:
            if (listed) {
                activate_item(highlighted(p));
            } else {
                n->w.autocomplete_submitted = true;
            }
            rml_event_stop_propagation(e);
            return true;
        case RML_KEY_ESCAPE:
            close_popup(k, key);
            str_set(&n->w.draft, n->w.applied);
            n->w.autocomplete_query_active = false;
            n->w.autocomplete_canceled = true;
            rml_element_set_value(n->w.input_el, n->w.applied ? n->w.applied : "");
            rml_element_focus(n->w.input_el, true);
            rml_event_stop_propagation(e);
            return true;
        case RML_KEY_TAB:
            if (!(event_modifiers(e) & KIT_MOD_SHIFT) && listed) {
                activate_item(highlighted(p));
                rml_event_stop_propagation(e);
                return true;
            }
            close_popup(k, key);
            str_set(&n->w.draft, n->w.applied);
            n->w.autocomplete_query_active = false;
            rml_element_set_value(n->w.input_el, n->w.applied ? n->w.applied : "");
            return true;
        default: return false;
    }
}

/* ---------- Popups and menus ---------- */

void kit_popup_open(kit *k, const char *key)
{
    node *trigger = k->last && k->last->kind == KIND_BUTTON &&
                            (k->last->events & EV_CLICK)
                      ? k->last
                      : NULL;
    popup *p = popup_open_on(k, key, k->last, trigger);
    str_set(&p->trigger, k->last ? k->last->path : "");
}

bool kit_dropdown_is_open(const kit *k, const char *key)
{
    popup *p = popup_find(k, key);
    return p && p->open && p->dropdown;
}

void kit_popup_open_at(kit *k, const char *key, float x, float y)
{
    close_kit_popups(k);
    popup *p = open_popup(k, key, "");
    p->at_point = true;
    p->x = x;
    p->y = y;
}

void kit_popup_close(kit *k, const char *key)
{
    close_popup(k, key);
}

/* What popover.c closes stays open here until the frame ends, so a closing
   popup emits once more and reports the item that closed it. */
bool kit_popup_is_open(const kit *k, const char *key)
{
    popup *p = popup_find(k, key);
    return p && p->open;
}

bool kit_has_popup(const kit *k)
{
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        if (k->popups[i]->open) {
            return true;
        }
    }
    return popover_top(k->context) != NULL;
}

bool kit_popup_begin(kit *k, const char *key, const char *cls)
{
    popup *p = popup_find(k, key);
    if (!p || !p->open) {
        return false;
    }
    const char *c = frame_format(k, "popup %s", cls ? cls : "");
    node *n = emit_in(k, k->layer, frame_format(k, "@%s", key), KIND_POPUP, "div", c);
    rml_element_remove_property(n->el, "pointer-events");
    presence(k, n);
    set_attr(n, "role", popup_role(p, n->el));
    p->node = n;
    if (!p->shown) {
        show_popup(k, p, n);
    }
    aclear(p->items);
    p->content = 0;
    apush(k->emitting, p);
    push_node(k, n);
    return true;
}

static popup *emitting_popup(const kit *k)
{
    return acount(k->emitting) ? alast(k->emitting) : NULL;
}

static float dp_ratio(const kit *k)
{
    return rml_context_dp_ratio(k->context);
}

/* RmlUi sizes an absolutely positioned box from its block content and not from
   flex rows, so a menu is widened here to its widest measured row. */
static void fit_row(kit *k, float width)
{
    popup *p = emitting_popup(k);
    if (p) {
        p->content = math_max(p->content, width);
    }
}

void kit_popup_end(kit *k)
{
    assert(acount(k->emitting));
    popup *p = apop(k->emitting);
    float width = math_clamp(p->content + popup_chrome, menu_min, menu_max);
    if (p->content > 0 && p->node && width != p->content_shown) {
        char value[32];
        snprintf(value, sizeof value, "%.0fdp", width);
        rml_element_set_property(p->node->el, "min-width", value);
        p->content_shown = width;
    }
    pop_node(k);
}

/* The shortcut, or a submenu's chevron. */
static void set_row_meta(widget *w, bool sub, const char *shortcut)
{
    flag_element(w->kbd_el, "data-empty", !sub && (!shortcut || !shortcut[0]));
    char *rml = NULL;
    if (sub) {
        char chevron[5];
        utf8(chevron, KIT_ICON_RIGHT);
        str_cat(&rml, chevron);
    } else {
        escape(&rml, shortcut);
    }
    if (str_set(&w->meta, rml)) {
        rml_element_set_inner_rml(w->kbd_el, w->meta);
        rml_element_set_class_names(w->kbd_el, sub ? "icon chev" : "kbd");
    }
    free(rml);
}

/* The chevron, or the gap and the shortcut. */
static float trail_width(kit *k, bool sub, const char *shortcut)
{
    if (sub) {
        return 20;
    }
    if (!shortcut || !shortcut[0]) {
        return 0;
    }
    return 8 + 24 + kit_text_width(k, shortcut, 11, 400, dp_ratio(k));
}

/* Padding, lead icon and gaps, then the label and the shortcut or chevron. */
/* Menu labels draw at the body's size, `--studio-font-13` when the font
   preference sets it (theme/base.rcss), else 13 dp. */
static float label_size(kit *k)
{
    char value[32] = "";
    rml_element_property(k->doc, "--studio-font-13", value, sizeof value);
    float size = strtof(value, NULL);
    return isfinite(size) && size > 0 ? size : 13;
}

static float row_width(kit *k, bool sub, const char *label, const char *shortcut)
{
    return 6 + 16 + 8 + kit_text_width(k, label, label_size(k), 400, dp_ratio(k)) +
           8 + trail_width(k, sub, shortcut);
}

/* A swatch tints the lead glyph; none restores the theme's icon color. */
static void set_swatch(rml_element *lead, uint32_t rgba)
{
    if (!rgba) {
        rml_element_remove_property(lead, "color");
        return;
    }
    char color[16];
    snprintf(color, sizeof color, "#%08x", rgba);
    rml_element_set_property(lead, "color", color);
}

static node *menu_row(kit *k, const char *key, node_kind kind, const kit_menu_item_desc *d)
{
    bool sub = kind == KIND_SUBMENU;
    node *n = emit(k, key, kind, "div", sub ? "menu-item sub" : "menu-item");
    widget *w = &n->w;
    if (!w->label_el) {
        w->icon_el = raw_icon(n->el, "lead", 0);
        w->head_el = raw(n->el, "div", "text", NULL);
        w->label_el = raw(w->head_el, "span", "label", NULL);
        w->about_el = raw(w->head_el, "span", "about", NULL);
        w->kbd_el = raw(n->el, "span", "kbd", NULL);
    }
    uint32_t lead = d->checked >= 0 ? (d->checked ? KIT_ICON_CHECK : 0)
                  : d->swatch       ? KIT_ICON_DOT
                                    : d->icon;
    if (w->icon != lead) {
        w->icon = lead;
        char text[5];
        utf8(text, lead);
        rml_element_set_inner_rml(w->icon_el, text);
    }
    if (w->color != d->swatch) {
        w->color = d->swatch;
        set_swatch(w->icon_el, d->swatch);
    }
    if (str_set(&w->label, d->label)) {
        set_inner_text(w->label_el, w->label);
    }
    set_element_text(w->about_el, d->description);
    set_flag(n, "data-described", d->description && d->description[0]);
    set_row_meta(w, sub, d->shortcut);
    set_flag(n, "data-disabled", d->disabled);
    set_flag(n, "data-checked", d->checked > 0);
    float width = row_width(k, sub, w->label, d->shortcut);
    if (d->description && d->description[0]) {
        width = math_max(
            width,
            6 + 16 + 8 + kit_text_width(k, d->description, 11, 400, dp_ratio(k)) +
                8 + trail_width(k, sub, d->shortcut)
        );
    }
    fit_row(k, width);
    popup *p = emitting_popup(k);
    set_attr(n, "role", p && p->dropdown ? "option" : "menuitem");
    if (sub) {
        set_attr(n, "aria-haspopup", "menu");
    }
    if (p) {
        apush(p->items, n);
    }
    /* popover.c highlights a menu's rows; a dropdown's follow its field. */
    if (p && p->dropdown) {
        flag_element(n->el, "data-highlighted", p->highlight == acount(p->items) - 1);
    }
    return n;
}

bool kit_menu_item(kit *k, const char *key, const kit_menu_item_desc *desc)
{
    node *n = menu_row(k, key, KIND_MENU_ITEM, desc);
    set_flag(n, "data-current", desc->current);
    bool activated = n->w.pending;
    n->w.pending = false;
    return activated && !desc->disabled;
}

void kit_menu_separator(kit *k, const char *key)
{
    emit(k, key, KIND_BOX, "div", "menu-sep");
}

void kit_menu_group(kit *k, const char *key, const char *label)
{
    node *n = emit(k, key, KIND_BOX, "div", "menu-group");
    set_text(n, label);
    fit_row(k, 12 + kit_text_width(k, label, 11, 400, dp_ratio(k)));
}

bool kit_submenu_begin(kit *k, const char *key, const char *label, uint32_t icon)
{
    kit_menu_item_desc d = { .label = label, .icon = icon, .checked = -1 };
    node *n = menu_row(k, key, KIND_SUBMENU, &d);
    return kit_popup_begin(k, frame_format(k, "sub:%s", n->path), "menu");
}

void kit_submenu_end(kit *k)
{
    kit_popup_end(k);
}

/* ---------- Tooltips ---------- */

static void set_position(rml_element *el, float x, float y)
{
    char value[32];
    snprintf(value, sizeof value, "%.1fdp", x);
    rml_element_set_property(el, "left", value);
    snprintf(value, sizeof value, "%.1fdp", y);
    rml_element_set_property(el, "top", value);
}

static void set_optional_attr(node *n, const char *name, const char *value)
{
    if (value && value[0]) {
        set_attr(n, name, value);
        return;
    }
    set_flag(n, name, false);
}

void set_tip(node *n, const char *label, const char *shortcut, const char *description)
{
    set_optional_attr(n, "data-tip", label);
    set_optional_attr(n, "data-kbd", shortcut);
    set_optional_attr(n, "data-tip-about", description);
}

/* The hovered element with a tip, unless a held button or a popup blocks
 * tooltips. */
static rml_element *tip_owner(kit *k)
{
    if (k->in.down[0] || k->in.down[1] || popover_top(k->context)) {
        return NULL;
    }
    /* An empty tip, as a binding leaves one, shows nothing. */
    rml_element *el = rml_context_hover_element(k->context);
    for (; el; el = rml_element_parent(el)) {
        char tip[2];
        if (rml_element_attribute(el, "data-tip", tip, sizeof tip) > 0) {
            return el;
        }
    }
    return NULL;
}

/* Tooltips keep their size on screen in a zoomed canvas. */
static float tip_scale(const kit *k)
{
    return k->in.zoom > 0 ? 1 / k->in.zoom : 1;
}

/* A picture above, then the label and shortcut on one line; a description
 * wraps beneath them. */
static void tip_content(kit *k, rml_element *target)
{
    char *tip = element_attribute(target, "data-tip");
    char *shortcut = element_attribute(target, "data-kbd");
    char *about = element_attribute(target, "data-tip-about");
    char *image = element_attribute(target, "data-tip-image");
    char *rml = NULL;
    if (image[0]) {
        str_cat(&rml, "<img class=\"tip-image\" src=\"");
        escape(&rml, image);
        str_cat(&rml, "\"/>");
    }
    str_set(&k->tip_image, image);
    flag_element(k->tooltip, "data-image", image[0] != '\0');
    free(image);
    str_cat(&rml, "<div class=\"tip-head\"><span>");
    escape(&rml, tip);
    str_cat(&rml, "</span>");
    if (shortcut[0]) {
        str_cat(&rml, "<span class=\"kbd\">");
        escape(&rml, shortcut);
        str_cat(&rml, "</span>");
    }
    str_cat(&rml, "</div>");
    if (about[0]) {
        str_cat(&rml, "<div class=\"tip-about\">");
        escape(&rml, about);
        str_cat(&rml, "</div>");
    }
    rml_element_set_inner_rml(k->tooltip, rml);
    free(rml);
    free(about);
    free(shortcut);
    free(tip);
}

static void show_tip(kit *k, rml_element *target)
{
    tip_content(k, target);
    char size[32];
    snprintf(size, sizeof size, "%.2fdp", tip_size * tip_scale(k));
    rml_element_set_property(k->tooltip, "font-size", size);
    rml_element_set_property(k->tooltip, "display", "block");
    /* Warm tooltips in a group switch instantly (Base UI); a cold one fades in.
     */
    flag_element(k->tooltip, "data-instant", k->in.time < k->tip_warm_until);
    flag_element(k->tooltip, "data-starting-style", !k->reduced_motion);
    k->tip_shown_frame = k->frame;
    k->tip_visible = true;
}

/* Below the target, or above when it would leave the window; beside it for
 * data-tip-side="right". */
static void place_tip(kit *k, rml_element *target)
{
    kit_rect r = element_rect(k, target);
    kit_rect size = element_rect(k, k->tooltip);
    char *side = element_attribute(target, "data-tip-side");
    bool right = strcmp(side, "right") == 0;
    free(side);
    float gap = tip_gap * tip_scale(k), edge = 4 * tip_scale(k);
    float x = right ? r.x + r.w + gap : r.x + r.w / 2 - size.w / 2;
    float y = right ? r.y + r.h / 2 - size.h / 2 : r.y + r.h + gap;
    if (!right && y + size.h > k->in.height - edge) {
        y = r.y - size.h - gap;
    }
    x = math_clamp(x, edge, math_max(edge, k->in.width - size.w - edge));
    set_position(k->tooltip, x, y);
}

void update_tooltip(kit *k)
{
    rml_element *target = tip_owner(k);
    if (target != k->tip_target) {
        if (k->tip_visible) {
            k->tip_warm_until = k->in.time + FEEL_TOOLTIP_GROUP;
        }
        k->tip_visible = false;
        k->tip_target = target;
        k->tip_since = k->in.time;
    }
    if (k->tip_visible && k->frame > k->tip_shown_frame) {
        flag_element(k->tooltip, "data-starting-style", false);
    }
    bool show = target && (k->tip_visible ||
                           k->in.time - k->tip_since >= FEEL_TOOLTIP_DELAY ||
                           k->in.time < k->tip_warm_until);
    if (!show) {
        rml_element_set_property(k->tooltip, "display", "none");
        return;
    }
    if (!k->tip_visible) {
        show_tip(k, target);
    } else {
        char *image = element_attribute(target, "data-tip-image");
        if (!str_eq(image, k->tip_image ? k->tip_image : "")) {
            tip_content(k, target);
        }
        free(image);
    }
    place_tip(k, target);
}

void kit_tip(kit *k, const char *label, const char *shortcut, const char *description)
{
    assert(k->last);
    set_tip(k->last, label, shortcut, description);
}
