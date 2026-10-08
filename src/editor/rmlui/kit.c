/* Studio kit: immediate-mode calls reconciled onto retained RmlUi documents:
 * the kit's document, its layer and the documents kit_document_begin shows
 * over the boxes it places, which lay out apart (ui/iframe.h).
 * This file keys, updates and retires nodes and routes RmlUi events to the
 * controllers in kit_controls.c, kit_popup.c and kit_command.c; number and
 * color fields and splitters are native controls (ui/controls/controls.h).
 * They implement the Base UI contracts recorded in
 * docs/prototypes/studio/kit.html; the C editor owns every model value. */
#include "editor/rmlui/kit_internal.h"
#include "support/resources.h"
#include "support/ui_style.h"
#include "support/xalloc.h"
#include "ui/iframe.h"
#include "ui/popover.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Strings ---------- */

uint64_t str_hash(const char *s)
{
    return cf_fnv1a(s, (int)strlen(s));
}

bool str_eq(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

bool str_set(char **s, const char *text)
{
    text = text ? text : "";
    if (*s && strcmp(*s, text) == 0) {
        return false;
    }
    char *copy = xstrdup(text);
    free(*s);
    *s = copy;
    return true;
}

/* Builds the result apart from *s, so arguments may point into it. */
static void str_vcatf(char **s, const char *format, va_list args)
{
    va_list measure;
    va_copy(measure, args);
    int add = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    assert(add >= 0);
    size_t have = *s ? strlen(*s) : 0;
    char *out = xmalloc(have + (size_t)add + 1);
    memcpy(out, *s ? *s : "", have);
    vsnprintf(out + have, (size_t)add + 1, format, args);
    free(*s);
    *s = out;
}

void str_catf(char **s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    str_vcatf(s, format, args);
    va_end(args);
}

void str_cat(char **s, const char *text)
{
    str_catf(s, "%s", text ? text : "");
}

static const char *entity(char c)
{
    switch (c) {
        case '&': return "&amp;";
        case '<': return "&lt;";
        case '>': return "&gt;";
        case '"': return "&quot;";
        default: return NULL;
    }
}

void escape(char **s, const char *text)
{
    text = text ? text : "";
    size_t have = *s ? strlen(*s) : 0, add = 0;
    for (const char *c = text; *c; c++) {
        const char *e = entity(*c);
        add += e ? strlen(e) : 1;
    }
    char *out = xmalloc(have + add + 1);
    memcpy(out, *s ? *s : "", have);
    char *at = out + have;
    for (const char *c = text; *c; c++) {
        const char *e = entity(*c);
        if (!e) {
            *at++ = *c;
            continue;
        }
        size_t length = strlen(e);
        memcpy(at, e, length);
        at += length;
    }
    *at = 0;
    free(*s);
    *s = out;
}

void utf8(char out[static 5], uint32_t cp)
{
    static const uint32_t lead[] = { 0, 0, 0xc0, 0xe0, 0xf0 };
    int count = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    out[count] = 0;
    for (int i = count - 1; i > 0; i--) {
        out[i] = (char)(0x80 | (cp & 0x3f));
        cp >>= 6;
    }
    out[0] = (char)(lead[count] | cp);
}

const char *frame_format(kit *k, const char *format, ...)
{
    char *s = NULL;
    va_list args;
    va_start(args, format);
    str_vcatf(&s, format, args);
    va_end(args);
    apush(k->strings, s);
    return s;
}

bool rect_contains(kit_rect r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ---------- Elements ---------- */

char *element_value(const rml_element *el)
{
    size_t size = rml_element_value(el, NULL, 0) + 1;
    char *s = xmalloc(size);
    rml_element_value(el, s, size);
    return s;
}

char *element_attribute(const rml_element *el, const char *name)
{
    size_t size = rml_element_attribute(el, name, NULL, 0) + 1;
    char *s = xmalloc(size);
    rml_element_attribute(el, name, s, size);
    return s;
}

char *element_inner_rml(const rml_element *el)
{
    size_t size = rml_element_inner_rml(el, NULL, 0) + 1;
    char *s = xmalloc(size);
    rml_element_inner_rml(el, s, size);
    return s;
}

float dp(const kit *k)
{
    return k->in.dp_ratio > 0 ? k->in.dp_ratio : 1;
}

kit_rect element_rect(const kit *k, rml_element *el)
{
    rml_vec2 offset = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float s = dp(k);
    return (kit_rect){ offset.x / s, offset.y / s, size.x / s, size.y / s };
}

static pair *pair_find(pair *pairs, const char *name)
{
    for (int i = 0; i < acount(pairs); i++) {
        if (strcmp(pairs[i].name, name) == 0) {
            return &pairs[i];
        }
    }
    return NULL;
}

static void pair_put(pair **pairs, const char *name, const char *value)
{
    pair *p = pair_find(*pairs, name);
    if (!p) {
        apush(*pairs, (pair){ xstrdup(name), NULL });
        p = &alast(*pairs);
    }
    str_set(&p->value, value);
}

static void pairs_free(pair *pairs)
{
    for (int i = 0; i < acount(pairs); i++) {
        free(pairs[i].name);
        free(pairs[i].value);
    }
    afree(pairs);
}

void set_class(node *n, const char *cls)
{
    if (str_eq(n->cls, cls)) {
        return;
    }
    str_set(&n->cls, cls);
    rml_element_set_class_names(n->el, n->cls);
}

void set_attr(node *n, const char *name, const char *value)
{
    pair *p = pair_find(n->attrs, name);
    if (p && str_eq(p->value, value)) {
        return;
    }
    pair_put(&n->attrs, name, value);
    rml_element_set_attribute(n->el, name, value);
}

void set_flag(node *n, const char *name, bool on)
{
    pair *p = pair_find(n->attrs, name);
    if ((p != NULL) == on) {
        return;
    }
    if (on) {
        pair_put(&n->attrs, name, "");
        rml_element_set_attribute(n->el, name, "");
        return;
    }
    free(p->name);
    free(p->value);
    adel(n->attrs, p - n->attrs);
    rml_element_remove_attribute(n->el, name);
}

void flag_element(rml_element *el, const char *name, bool on)
{
    if (on == rml_element_has_attribute(el, name)) {
        return;
    }
    if (on) {
        rml_element_set_attribute(el, name, "");
        return;
    }
    rml_element_remove_attribute(el, name);
}

void set_inner_text(rml_element *el, const char *text)
{
    char *rml = NULL;
    escape(&rml, text);
    rml_element_set_inner_rml(el, rml);
    free(rml);
}

void set_text(node *n, const char *text)
{
    if (str_eq(n->text, text)) {
        return;
    }
    str_set(&n->text, text);
    set_inner_text(n->el, n->text);
}

void set_element_text(rml_element *el, const char *text)
{
    char *rml = NULL;
    escape(&rml, text);
    char *current = element_inner_rml(el);
    if (strcmp(current, rml) != 0) {
        rml_element_set_inner_rml(el, rml);
    }
    free(current);
    free(rml);
}

rml_element *raw(rml_element *parent, const char *tag, const char *cls, const char *text)
{
    rml_element *el = rml_element_create_child(parent, tag, NULL);
    rml_element_set_class_names(el, cls);
    if (text) {
        set_inner_text(el, text);
    }
    return el;
}

rml_element *raw_icon(rml_element *parent, const char *cls, uint32_t cp)
{
    char *c = NULL;
    str_catf(&c, "icon %s", cls ? cls : "");
    rml_element *el = raw(parent, "span", c, NULL);
    free(c);
    if (cp) {
        char text[5];
        utf8(text, cp);
        rml_element_set_inner_rml(el, text);
    }
    return el;
}

/* ---------- Nodes ---------- */

node *node_new(node_kind kind)
{
    node *n = xcalloc(1, sizeof *n);
    n->kind = kind;
    n->w.pending_index = -1;
    n->w.icon = UINT32_MAX;
    return n;
}

static void strings_free(char **strings)
{
    for (int i = 0; i < acount(strings); i++) {
        free(strings[i]);
    }
    afree(strings);
}

static void widget_free(widget *w)
{
    char *owned[] = { w->addon, w->draft, w->applied,    w->label,
                      w->meta,  w->badge, w->placeholder };
    for (size_t i = 0; i < sizeof owned / sizeof *owned; i++) {
        free(owned[i]);
    }
    strings_free(w->labels);
}

void node_free(node *n)
{
    for (int i = 0; i < acount(n->children); i++) {
        node_free(n->children[i]);
    }
    afree(n->children);
    free(n->key);
    free(n->path);
    free(n->cls);
    free(n->text);
    free(n->tag);
    drop_types_clear(n);
    pairs_free(n->attrs);
    pairs_free(n->styles);
    widget_free(&n->w);
    free(n);
}

static uint64_t element_key(const rml_element *el)
{
    return (uint64_t)(uintptr_t)el;
}

/* Indexes a node by element and path; distinct paths must hash apart. */
static void index_node(kit *k, node *n)
{
    cf_map_set(k->by_element, element_key(n->el), n);
    uint64_t h = str_hash(n->path);
    node *same = cf_map_get(k->by_path, h);
    assert(!same || str_eq(same->path, n->path));
    cf_map_set(k->by_path, h, n);
}

static void close_document(kit *k, node *n);

/* An index entry goes only while it is n's: a native slot's nodes outlive
 * their elements, whose addresses may already index new nodes. A node's
 * own document closes with it. */
void unmap(kit *k, node *n)
{
    for (int i = 0; i < acount(n->children); i++) {
        unmap(k, n->children[i]);
    }
    if (n->document) {
        close_document(k, n);
    }
    forget_motion(k, n);
    if (cf_map_get(k->by_element, element_key(n->el)) == n) {
        cf_map_del(k->by_element, element_key(n->el));
    }
    if (cf_map_get(k->by_path, str_hash(n->path)) == n) {
        cf_map_del(k->by_path, str_hash(n->path));
    }
    if (k->last == n) {
        k->last = NULL;
    }
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup *p = k->popups[i];
        if (p->node == n) {
            p->node = NULL;
        }
        nodes_remove(p->items, n);
    }
}

static void prune(kit *k, node *n)
{
    while (acount(n->children) > n->cursor) {
        retire(k, apop(n->children));
    }
}

static void insert_child(node *parent, int i, node *n)
{
    apush(parent->children, n);
    int after = acount(parent->children) - 1 - i;
    memmove(
        &parent->children[i + 1],
        &parent->children[i],
        (size_t)after * sizeof *parent->children
    );
    parent->children[i] = n;
}

static void remove_child(node *parent, int i)
{
    int count = acount(parent->children);
    memmove(
        &parent->children[i],
        &parent->children[i + 1],
        (size_t)(count - 1 - i) * sizeof *parent->children
    );
    asetlen(parent->children, count - 1);
}

/* Moves child j back to i, the ones between moving up one. */
static void rotate_child(node *parent, int i, int j)
{
    node *moved = parent->children[j];
    memmove(
        &parent->children[i + 1],
        &parent->children[i],
        (size_t)(j - i) * sizeof *parent->children
    );
    parent->children[i] = moved;
}

static bool is_input(const rml_element *el)
{
    const char *tag = rml_element_tag_name(el);
    return !strcmp(tag, "input") || !strcmp(tag, "textarea");
}

/* Moving detaches the element, which drops RmlUi focus and text selection
 * inside it; restore both so reordering keyed siblings (popups in the layer)
 * keeps an edit going. */
static void move_element(kit *k, rml_element *parent, rml_element *el, rml_element *before)
{
    rml_element *focus = rml_context_focus_element(k->context);
    if (!focus || !within(focus, el)) {
        rml_element_move_child(parent, el, before);
        return;
    }
    int start = 0, end = 0;
    bool text = is_input(focus);
    if (text) {
        rml_element_selection_range(focus, &start, &end);
    }
    rml_element_move_child(parent, el, before);
    rml_element_focus(focus, true);
    if (text) {
        rml_element_set_selection_range(focus, start, end);
    }
}

/* The first child from the cursor with this key, brought to the cursor. One
 * of another kind or tag is retired so a new one takes its place. */
static node *reuse_child(kit *k, node *parent, const char *key, node_kind kind, const char *tag)
{
    int i = parent->cursor;
    for (int j = i; j < acount(parent->children); j++) {
        node *candidate = parent->children[j];
        if (strcmp(candidate->key, key) != 0) {
            continue;
        }
        if (candidate->kind != kind || strcmp(candidate->tag, tag) != 0) {
            remove_child(parent, j);
            candidate->presence = false;
            retire(k, candidate);
            return NULL;
        }
        if (j != i) {
            move_element(
                k,
                parent->container,
                candidate->el,
                parent->children[i]->el
            );
            rotate_child(parent, i, j);
        }
        return candidate;
    }
    return NULL;
}

static node *create_child(kit *k, node *parent, const char *key, node_kind kind, const char *tag)
{
    int i = parent->cursor;
    node *n = node_new(kind);
    str_set(&n->key, key);
    str_set(&n->tag, tag);
    n->parent = parent;
    if (str_eq(parent->path, "")) {
        str_set(&n->path, key);
    } else {
        str_catf(&n->path, "%s/%s", parent->path, key);
    }
    n->created = k->frame;
    rml_element *before = i < acount(parent->children) ? parent->children[i]->el : NULL;
    n->el = n->container = rml_element_create_child(parent->container, tag, before);
    index_node(k, n);
    insert_child(parent, i, n);
    return n;
}

node *emit_in(kit *k, node *parent, const char *key, node_kind kind, const char *tag, const char *cls)
{
    assert(parent && key && key[0]);
    const char *element_tag = tag && tag[0] ? tag : "div";
    node *n = reuse_child(k, parent, key, kind, element_tag);
    if (!n) {
        n = create_child(k, parent, key, kind, element_tag);
    }
    parent->cursor++;
    n->frame = k->frame;
    drop_types_clear(n);
    n->drag_source = false;
    set_class(n, cls);
    k->last = n;
    return n;
}

node *emit(kit *k, const char *key, node_kind kind, const char *tag, const char *cls)
{
    assert(acount(k->stack));
    return emit_in(k, alast(k->stack), key, kind, tag, cls);
}

void push_node(kit *k, node *n)
{
    n->cursor = 0;
    apush(k->stack, n);
}

void pop_node(kit *k)
{
    assert(acount(k->stack) > 1);
    node *n = alast(k->stack);
    prune(k, n);
    (void)apop(k->stack);
    k->last = n;
}

static node *element_node(const kit *k, const rml_element *el)
{
    return cf_map_get(k->by_element, element_key(el));
}

node *node_for(const kit *k, rml_element *el)
{
    for (; el; el = rml_element_parent(el)) {
        node *n = element_node(k, el);
        if (n) {
            return n;
        }
    }
    return NULL;
}

node *path_node(const kit *k, const char *path)
{
    node *n = cf_map_get(k->by_path, str_hash(path));
    return n && str_eq(n->path, path) ? n : NULL;
}

bool within(rml_element *el, rml_element *ancestor)
{
    for (; el; el = rml_element_parent(el)) {
        if (el == ancestor) {
            return true;
        }
    }
    return false;
}

int nodes_index(node *const *list, const node *n)
{
    for (int i = 0; i < acount(list); i++) {
        if (list[i] == n) {
            return i;
        }
    }
    return -1;
}

void nodes_remove(node **list, const node *n)
{
    int kept = 0;
    for (int i = 0; i < acount(list); i++) {
        if (list[i] != n) {
            list[kept++] = list[i];
        }
    }
    asetlen(list, kept);
}

static int document_index(const kit *k, const rml_element *document)
{
    for (int i = 0; i < acount(k->documents); i++) {
        if (k->documents[i] == document) {
            return i;
        }
    }
    return -1;
}

bool owns_document(const kit *k, const rml_element *document)
{
    return document_index(k, document) >= 0;
}

/* ---------- Popups ---------- */

popup *popup_find(const kit *k, const char *key)
{
    popup *p = cf_map_get(k->popups, str_hash(key));
    return p && str_eq(p->key, key) ? p : NULL;
}

popup *popup_at(kit *k, const char *key)
{
    popup *p = popup_find(k, key);
    if (p) {
        return p;
    }
    uint64_t h = str_hash(key);
    assert(!cf_map_has(k->popups, h));
    p = xcalloc(1, sizeof *p);
    str_set(&p->key, key);
    p->highlight = -1;
    cf_map_set(k->popups, h, p);
    return p;
}

static void popup_free(popup *p)
{
    free(p->key);
    free(p->parent);
    free(p->dropdown_placeholder);
    free(p->dropdown_trigger_path);
    free(p->anchor_path);
    free(p->trigger);
    afree(p->items);
    free(p);
}

/* ---------- Event routing ---------- */

unsigned event_modifiers(const rml_event *e)
{
    return (e->modifiers & RML_KM_CTRL ? KIT_MOD_CTRL : 0u) |
           (e->modifiers & RML_KM_SHIFT ? KIT_MOD_SHIFT : 0u) |
           (e->modifiers & RML_KM_ALT ? KIT_MOD_ALT : 0u) |
           (e->modifiers & RML_KM_META ? KIT_MOD_META : 0u);
}

static void mousedown(kit *k, const rml_event *e, node *n)
{
    /* An outside press closed them in popover.c: they emit no more. */
    forget_closed_popups(k);
    popup_press(k, e->target);
    if (!n) {
        return;
    }
    if (e->button == 1) {
        n->events |= EV_CONTEXT;
        return;
    }
    if (e->button != 0) {
        return;
    }
    n->events |= EV_PRESS;
    k->keyboard_edit = false;
}

static void click(kit *k, const rml_event *e, node *n)
{
    switch (n->kind) {
        case KIND_MENU_ITEM:
        case KIND_SUBMENU: menu_click(k, n); return;
        case KIND_BOX:
            /* A click on a label or icon is also a click on the boxes around
               it, as DOM clicks bubble, so a whole row or tab answers. */
            for (node *box = n; box && box->kind == KIND_BOX; box = box->parent) {
                box->events |= EV_CLICK;
            }
            return;
        default: control_click(k, n, e->target); return;
    }
}

/* The modal host emitted last, or NULL: kit_modal_host's hosts are the
 * layer's only hosted children. */
static rml_element *top_modal(const kit *k)
{
    for (int i = acount(k->layer->children) - 1; i >= 0; i--) {
        if (k->layer->children[i]->hosted) {
            return k->layer->children[i]->el;
        }
    }
    return NULL;
}

static bool trap_focus(kit *k, const rml_event *e)
{
    rml_element *modal = e->key == RML_KEY_TAB ? top_modal(k) : NULL;
    if (!modal) {
        return false;
    }
    focus_step(modal, e->target, e->modifiers & RML_KM_SHIFT ? -1 : 1, true, NULL);
    rml_event_stop_propagation(e);
    return true;
}

/* An open menu takes the keys its trigger would: popover.c runs it. */
static bool menu_open(kit *k)
{
    rml_element *top = popover_top(k->context);
    return top && !rml_element_is_class_set(top, "popover") &&
           !rml_element_has_attribute(top, "popover");
}

static void keydown(kit *k, const rml_event *e, node *n)
{
    k->keyboard_edit = true;
    /* An open color picker takes its own keys, Escape and Tab included. */
    if (n && n->kind == KIND_COLOR && color_field_open(n->el)) {
        return;
    }
    if (trap_focus(k, e)) {
        return;
    }
    /* Hosted content handles its own keys, and RmlUi navigates it. */
    if (n && n->hosted && e->key != RML_KEY_ESCAPE && e->key != RML_KEY_TAB) {
        return;
    }
    /* A field in a popup takes its own keys, as in a Base UI Popover; the
       popup keeps Escape and Tab, so typing never runs a menu's typeahead. */
    if (autocomplete_key(k, e, n)) {
        return;
    }
    bool entry = n && (n->kind == KIND_TEXT || n->kind == KIND_SEARCH ||
                       n->kind == KIND_AUTOCOMPLETE || n->kind == KIND_NUMBER ||
                       (n->kind == KIND_COLOR && e->target == n->el) ||
                       is_input(e->target));
    if (dropdown_key(k, e, n) || !n || (!entry && menu_open(k))) {
        return;
    }
    control_key(k, n, e);
}

static void blur(const rml_event *e, node *n)
{
    if (n) {
        control_blur(n, e->target);
    }
}

static void on_event(void *user, const rml_event *e)
{
    kit *k = user;
    check_slots(k);
    node *n = node_for(k, e->target);
    switch (e->id) {
        case RML_EVENT_MOUSEDOWN: mousedown(k, e, n); break;
        case RML_EVENT_CLICK:
            if (n) {
                click(k, e, n);
            }
            break;
        case RML_EVENT_DBLCLICK:
            if (n) {
                n->events |= EV_DOUBLE;
            }
            break;
        case RML_EVENT_KEYDOWN: keydown(k, e, n); break;
        /* The kit's fields take each edit; they commit on their own keys. */
        case RML_EVENT_INPUT:
            if (n) {
                control_change(k, n, e->target);
            }
            break;
        case RML_EVENT_FOCUS:
            if (n) {
                control_focus(k, n, e->target);
            }
            break;
        case RML_EVENT_BLUR: blur(e, n); break;
        case RML_EVENT_MOUSEMOVE:
            if (n) {
                menu_hover(k, n);
            }
            break;
        default: break;
    }
}

static const rml_event_id kit_events[] = {
    RML_EVENT_MOUSEDOWN, RML_EVENT_CLICK, RML_EVENT_DBLCLICK,
    RML_EVENT_KEYDOWN,   RML_EVENT_INPUT, RML_EVENT_MOUSEOVER,
    RML_EVENT_FOCUS,     RML_EVENT_BLUR,  RML_EVENT_MOUSEMOVE,
};

#define KIT_EVENT_COUNT (sizeof kit_events / sizeof *kit_events)

/* ---------- Stylesheets ---------- */

/* The name RmlUi reports for the kit's sheet, "kit-<document>", or with a
 * key, for that extra sheet, "kit-<document>-<key>" (support/ui_style.h). */
static void sheet_name(const kit *k, const char *key, char *name, size_t size)
{
    int stem = (int)strcspn(k->document, ".");
    snprintf(name, size, "kit-%.*s%s%s", stem, k->document, key ? "-" : "", key ? key : "");
    for (char *c = name; *c; c++) {
        *c = isalnum((unsigned char)*c) || *c == '-' ? *c : '_';
    }
}

/* The theme is the document's stylesheet: the Studio project styles every
 * kit document, so Studio edits its own chrome. Each sheet is recorded with
 * where its lines came from, as its maker recorded them (ui_style_map). The
 * kit's other documents take copies, read once. */
static void restyle(kit *k)
{
    char name[160];
    const char *theme = k->theme ? k->theme : "";
    sheet_name(k, NULL, name, sizeof name);
    if (!rml_document_set_named_style_sheet(k->doc, name, theme) && theme[0]) {
        fprintf(stderr, "RmlUi could not read the theme of %s\n", k->document);
    }
    ui_style_set(name, theme, ui_style_map(theme));
    for (int i = 0; i < acount(k->extra_styles); i++) {
        const char *extra = k->extra_styles[i].value;
        sheet_name(k, k->extra_styles[i].name, name, sizeof name);
        if (extra[0] && !rml_document_add_named_style_sheet(k->doc, name, extra)) {
            fprintf(
                stderr,
                "RmlUi could not read the %s style sheet\n",
                k->extra_styles[i].name
            );
        }
        ui_style_set(name, extra, ui_style_map(extra));
    }
    for (int i = 1; i < acount(k->documents); i++) {
        rml_document_copy_style_sheet(k->documents[i], k->doc);
    }
    k->restyle = false;
}

void kit_theme(kit *k, const char *rcss)
{
    if (k->theme && rcss && !strcmp(k->theme, rcss)) {
        return;
    }
    free(k->theme);
    k->theme = rcss ? xstrdup(rcss) : NULL;
    k->restyle = true;
}

void kit_style_sheet(kit *k, const char *key, const char *rcss)
{
    pair *p = pair_find(k->extra_styles, key);
    if (!rcss && p) {
        free(p->name);
        free(p->value);
        adel(k->extra_styles, p - k->extra_styles);
        k->restyle = true;
        return;
    }
    if (!rcss || (p && str_eq(p->value, rcss))) {
        return;
    }
    pair_put(&k->extra_styles, key, rcss);
    k->restyle = true;
}

void kit_set_standard_font_family(kit *k, const char *family)
{
    assert(family && family[0]);
    str_set(&k->standard_font_family, family);
}

/* ---------- Documents ---------- */

/* Listens to the document, styles it as the kit's are and gives it the
 * body classes the kit set. */
static void add_document(kit *k, rml_element *document)
{
    apush(k->documents, document);
    for (size_t i = 0; i < KIT_EVENT_COUNT; i++) {
        rml_element_add_event_listener(document, kit_events[i], k->listener, true);
    }
    for (int i = 0; i < acount(k->body_classes); i++) {
        rml_element_set_class(document, k->body_classes[i], true);
    }
    if (document != k->doc && !k->restyle) {
        rml_document_copy_style_sheet(document, k->doc);
    }
}

static void stop_listening(kit *k, rml_element *document)
{
    for (size_t e = 0; e < KIT_EVENT_COUNT; e++) {
        rml_element_remove_event_listener(document, kit_events[e], k->listener, true);
    }
}

/* Hosted content holds its owner's listeners, which may be gone by the time
 * the context releases a closed document, so the content goes first. Closing
 * moves the pointer's hover, whose events the kit's other documents hear. */
static void remove_document(kit *k, rml_element *document)
{
    int i = document_index(k, document);
    assert(i >= 0);
    memmove(
        &k->documents[i],
        &k->documents[i + 1],
        (size_t)(acount(k->documents) - 1 - i) * sizeof *k->documents
    );
    asetlen(k->documents, acount(k->documents) - 1);
    rml_element_set_inner_rml(document, "");
    rml_document_close(document);
}

/* A document of the kit's own, at the URL of the kit's document so that
 * relative paths resolve alike; it takes no focus as it shows. */
static rml_element *open_document(kit *k, const char *id)
{
    char *url = NULL;
    str_catf(&url, "%s/%s", resource_ui(), k->document);
    rml_element *document = rml_context_create_document(k->context, url);
    free(url);
    assert(document);
    if (id) {
        rml_element_set_attribute(document, "id", id);
    }
    add_document(k, document);
    rml_document_show(document, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    return document;
}

/* The layer is a document over all the others, so popups, tooltips,
 * toasts, the palette and dialogs show above every area; its body takes the
 * classes of the document's, whose variables its content reads. */
static rml_element *open_layer(kit *k)
{
    rml_element *layer = open_document(k, "layer");
    char classes[256];
    size_t length = rml_element_class_names(k->doc, classes, sizeof classes);
    assert(length < sizeof classes);
    rml_element_set_class_names(layer, classes);
    return layer;
}

/* n's children go into a document of their own, shown over n's element,
 * and keep the paths they would have without it. */
static void open_node_document(kit *k, node *n, bool fit)
{
    assert(!acount(n->children));
    if (cf_map_get(k->by_path, str_hash(n->path)) == n) {
        cf_map_del(k->by_path, str_hash(n->path));
    }
    str_set(&n->path, n->parent->path);
    n->container = open_document(k, NULL);
    n->document = true;
    iframe_attach(n->container, n->el, fit);
}

/* Its nodes are gone, so the hover events closing sends reach none. */
static void close_document(kit *k, node *n)
{
    iframe_detach(n->container);
    release_exiting_within(k, n->container);
    stop_listening(k, n->container);
    remove_document(k, n->container);
    n->container = n->el;
    n->document = false;
}

/* ---------- API ---------- */

kit *kit_create(void *context, const char *document_path)
{
    kit *k = xcalloc(1, sizeof *k);
    k->context = context;
    k->frame = 1;
    k->surface_button = -1;
    char *path = NULL;
    str_catf(&path, "%s/%s", resource_ui(), document_path);
    k->doc = rml_context_load_document(k->context, path);
    free(path);
    assert(k->doc);
    k->document = xstrdup(document_path);
    k->restyle = true; /* on the first frame, after the owner sets a theme */
    /* It stays under the documents shown over it, which RmlUi raises above
       their siblings as they take focus. */
    rml_element_set_property(k->doc, "z-index", "0");
    rml_document_show(k->doc, RML_MODAL_NONE, RML_FOCUS_AUTO, RML_SCROLL_FLAG_AUTO);
    register_wires_element();
    register_dot_element();
    register_code_element();
    k->listener = rml_listener_create(on_event, k);
    add_document(k, k->doc);
    rml_element *layer = open_layer(k);
    k->layer = node_new(KIND_ROOT);
    k->layer->el = k->layer->container = layer;
    str_set(&k->layer->path, "layer");
    cf_map_set(k->by_element, element_key(layer), k->layer);
    apush(k->roots, k->layer);
    k->tooltip = raw(layer, "div", "tooltip", NULL);
    rml_element_set_property(k->tooltip, "display", "none");
    return k;
}

static void kit_free(kit *k)
{
    for (int i = 0; i < acount(k->roots); i++) {
        node_free(k->roots[i]);
    }
    afree(k->roots);
    free_slots(k);
    for (int i = 0; i < acount(k->exiting); i++) {
        node_free(k->exiting[i].node);
    }
    afree(k->exiting);
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        popup_free(k->popups[i]);
    }
    cf_map_free(k->popups);
    for (int i = 0; i < acount(k->toasts); i++) {
        free(k->toasts[i].title);
        free(k->toasts[i].description);
        free(k->toasts[i].action);
    }
    afree(k->toasts);
    free(k->surface_capture);
    free(k->tip_image);
    strings_free(k->strings);
    cf_map_free(k->by_element);
    cf_map_free(k->by_path);
    cf_map_free(k->toast_actions);
    afree(k->stack);
    afree(k->emitting);
    afree(k->entering);
    afree(k->layout_nodes);
    afree(k->documents);
    strings_free(k->body_classes);
    free(k->document);
    free(k->theme);
    free(k->standard_font_family);
    pairs_free(k->extra_styles);
    free(k);
}

void kit_destroy(kit *k)
{
    if (!k) {
        return;
    }
    /* The kit hears nothing while its nodes outlive their elements. */
    for (int i = 0; i < acount(k->documents); i++) {
        stop_listening(k, k->documents[i]);
    }
    /* Those kit_document_begin opened, shown in elements, then the layer
       and the kit's own. */
    while (acount(k->documents)) {
        rml_element *document = alast(k->documents);
        if (acount(k->documents) > 2) {
            iframe_detach(document);
        }
        remove_document(k, document);
    }
    rml_listener_destroy(k->listener);
    kit_free(k);
}

static void close_root(kit *k)
{
    if (!k->root) {
        return;
    }
    while (acount(k->stack) > 1) {
        pop_node(k);
    }
    prune(k, k->root);
    aclear(k->stack);
    k->root = NULL;
}

void kit_begin(kit *k, const kit_frame_input *input)
{
    k->in = *input;
    check_slots(k);
    if (k->restyle) {
        restyle(k);
        /* Resolve authored metrics before native lists read them this frame. */
        rml_context_update(k->context);
    }
    size_popups(k);
    drag_frame(k);
    k->layer->cursor = 0;
    k->layer->frame = k->frame;
}

void kit_end(kit *k)
{
    /* Hosted content installs its markup as it is emitted, so the style
       sheets it added this frame apply before the frame lays out and renders,
       not a frame after it. */
    if (k->restyle) {
        restyle(k);
    }
    close_root(k);
    forget_closed_popups(k);
    toasts_frame(k);
    prune(k, k->layer);
    end_slots(k);
    update_tooltip(k);
    motion_frame(k);
    for (int i = 0; i < cf_map_size(k->by_path); i++) {
        node *n = k->by_path[i];
        if (n->frame == k->frame) {
            drag_flush(n);
        }
        n->events = 0;
        n->w.committed = false;
    }
    strings_free(k->strings);
    k->strings = NULL;
    k->frame++;
}

static node *root_node(const kit *k, const char *element_id)
{
    for (int i = 0; i < acount(k->roots); i++) {
        if (str_eq(k->roots[i]->path, element_id)) {
            return k->roots[i];
        }
    }
    return NULL;
}

static node *add_root(kit *k, const char *element_id)
{
    rml_element *el = rml_element_by_id(k->doc, element_id);
    assert(el && "kit_root element must exist in the document");
    node *root = node_new(KIND_ROOT);
    root->el = root->container = el;
    str_set(&root->path, element_id);
    index_node(k, root);
    apush(k->roots, root);
    return root;
}

void kit_root(kit *k, const char *element_id)
{
    close_root(k);
    node *root = root_node(k, element_id);
    k->root = root ? root : add_root(k, element_id);
    k->root->frame = k->frame;
    push_node(k, k->root);
}

void kit_push(kit *k, const char *key, const char *tag, const char *cls)
{
    push_node(k, emit(k, key, KIND_BOX, tag, cls));
}

void kit_pop(kit *k)
{
    pop_node(k);
}

void kit_leaf(kit *k, const char *key, const char *tag, const char *cls)
{
    emit(k, key, KIND_BOX, tag, cls);
}

void kit_document_begin(kit *k, const char *key, const char *cls, bool fit)
{
    node *n = emit(k, key, KIND_BOX, "div", cls);
    if (!n->document) {
        open_node_document(k, n, fit);
    }
    push_node(k, n);
}

void kit_document_end(kit *k)
{
    node *n = alast(k->stack);
    assert(n->document);
    pop_node(k);
}

rml_element *kit_host(kit *k, const char *key, const char *cls)
{
    node *n = emit(k, key, KIND_BOX, "div", cls);
    n->hosted = true;
    return n->el;
}

rml_element *kit_modal_host(kit *k, const char *key)
{
    node *n = emit_in(k, k->layer, key, KIND_BOX, "div", "modal-host");
    n->hosted = true;
    return n->el;
}

void kit_modal_begin(kit *k, const char *key)
{
    node *n = emit_in(k, k->layer, key, KIND_BOX, "div", "modal-host");
    n->hosted = true;
    push_node(k, n);
}

void kit_modal_end(kit *k)
{
    pop_node(k);
}

void kit_label(kit *k, const char *key, const char *cls, const char *text)
{
    node *n = emit(k, key, KIND_BOX, "span", cls);
    set_text(n, text);
}

void kit_live_label(kit *k, const char *key, const char *cls, const char *text)
{
    node *n = emit(k, key, KIND_BOX, "live-text", cls);
    set_attr(n, "value", text);
}

void kit_icon(kit *k, const char *key, const char *cls, uint32_t codepoint)
{
    const char *c = frame_format(k, "icon %s", cls ? cls : "");
    node *n = emit(k, key, KIND_BOX, "span", c);
    char text[5];
    utf8(text, codepoint);
    set_text(n, text);
}

void kit_markup(kit *k, const char *key, const char *cls, const char *rml)
{
    node *n = emit(k, key, KIND_BOX, "span", cls);
    if (str_eq(n->text, rml)) {
        return;
    }
    str_set(&n->text, rml);
    rml_element_set_inner_rml(n->el, n->text);
}

void kit_attr(kit *k, const char *name, const char *value)
{
    assert(k->last);
    set_attr(k->last, name, value ? value : "");
}

void kit_flag(kit *k, const char *name, bool on)
{
    assert(k->last);
    set_flag(k->last, name, on);
}

void kit_style(kit *k, const char *property, const char *value)
{
    node *n = k->last;
    assert(n);
    pair *p = pair_find(n->styles, property);
    if (p && str_eq(p->value, value)) {
        return;
    }
    pair_put(&n->styles, property, value);
    rml_element_set_property(n->el, property, value);
}

bool kit_item_clicked(kit *k)
{
    return k->last && (k->last->events & EV_CLICK);
}

bool kit_item_pressed(kit *k)
{
    return k->last && (k->last->events & EV_PRESS);
}

bool kit_item_active(kit *k)
{
    node *n = k->last;
    if (!n) {
        return false;
    }
    switch (n->kind) {
        case KIND_NUMBER: return number_field_active(n->el);
        case KIND_COLOR: return color_field_open(n->el);
        default: return n->w.focused;
    }
}

bool kit_item_committed(kit *k)
{
    return k->last && k->last->w.committed;
}

kit_rect kit_item_rect(kit *k)
{
    return k->last ? element_rect(k, k->last->el) : (kit_rect){ 0 };
}

rml_element *kit_item_element(kit *k)
{
    return k->last ? k->last->el : NULL;
}

bool kit_item_hovered(kit *k)
{
    rml_element *hover = rml_context_hover_element(k->context);
    return k->last && hover && within(hover, k->last->el);
}

const char *kit_string(kit *k, const char *text)
{
    apush(k->strings, xstrdup(text ? text : ""));
    return alast(k->strings);
}

const char *kit_classes(kit *k, const char *base, const kit_class *list, int count)
{
    char *cls = NULL;
    str_cat(&cls, base);
    for (int i = 0; i < count; i++) {
        if (!list[i].on || !list[i].name || !list[i].name[0]) {
            continue;
        }
        str_catf(&cls, "%s%s", cls[0] ? " " : "", list[i].name);
    }
    apush(k->strings, cls);
    return cls;
}

/* ---------- Host queries ---------- */

bool kit_wants_keyboard(const kit *k)
{
    if (popover_top(k->context)) {
        return true;
    }
    rml_element *focus = rml_context_focus_element(k->context);
    /* A focused number field handles its own keys during RmlUi dispatch; global
       shortcuts still apply unless text is being edited. A modal's hosted
       content and a code editor own the keyboard while they hold focus; a
       list or button an area or the chrome hosts passes it on, as a page's
       do, so the area under the pointer still takes its keys. */
    node *n = focus ? node_for(k, focus) : NULL;
    bool modal = n && n->hosted && n->parent == k->layer;
    return focus && (is_input(focus) || modal || (n && n->kind == KIND_CODE));
}

bool kit_accepts_text(const kit *k)
{
    rml_element *focus = rml_context_focus_element(k->context);
    if (!focus) {
        return false;
    }
    node *n = element_node(k, focus);
    return is_input(focus) || (n && (n->kind == KIND_NUMBER || n->kind == KIND_CODE));
}

bool kit_input_focused(const kit *k)
{
    rml_element *focus = rml_context_focus_element(k->context);
    return focus && is_input(focus);
}

bool kit_wants_text(const kit *k, kit_rect *caret)
{
    rml_element *focus = rml_context_focus_element(k->context);
    node *n = focus ? element_node(k, focus) : NULL;
    bool code = n && n->kind == KIND_CODE;
    if (!focus || !(is_input(focus) || code)) {
        return false;
    }
    if (caret) {
        *caret = code ? code_caret(k, n) : element_rect(k, focus);
    }
    return true;
}

bool kit_wants_pointer(const kit *k)
{
    if (popover_top(k->context)) {
        return true;
    }
    if (!str_eq(k->surface_capture, "")) {
        return false;
    }
    rml_element *hover = rml_context_hover_element(k->context);
    if (!hover || rml_element_owner_document(hover) == hover) {
        return false;
    }
    node *n = element_node(k, hover);
    return !n || n->kind != KIND_SURFACE;
}

float kit_metric(const kit *k, const char *name)
{
    char value[64];
    rml_element_property(k->doc, name, value, sizeof value);
    float metric = strtof(value, NULL);
    assert(isfinite(metric) && metric > 0);
    return metric;
}

/* Measured with the face RmlUi draws at this ratio: it truncates font sizes to
   whole pixels (ElementStyle) and hints glyph advances. */
float kit_text_width(const kit *k, const char *text, float size, int weight, float ratio)
{
    assert(ratio > 0);
    int pixels = (int)(size * ratio);
    if (!text || !text[0] || pixels < 1) {
        return 0;
    }
    char family[128];
    const char *authored = k->standard_font_family;
    if (!authored) {
        rml_element_property(k->doc, "font-family", family, sizeof family);
        for (char *c = family; *c; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        authored = family;
    }
    uintptr_t face = rml_font_face(authored, RML_FONT_NORMAL, weight > 0 ? weight : 400, pixels);
    if (!face) {
        return 0;
    }
    return (float)rml_font_string_width(face, text) / ratio;
}

void kit_focus(kit *k)
{
    assert(k->last);
    rml_element *el = k->last->w.input_el ? k->last->w.input_el : k->last->el;
    rml_element_focus(el, true);
    if (is_input(el)) {
        rml_element_select(el);
    }
}

void kit_blur(kit *k)
{
    rml_element *focus = rml_context_focus_element(k->context);
    if (focus && rml_element_owner_document(focus) != focus) {
        rml_element_blur(focus);
    }
}

const char *kit_hovered_path(const kit *k)
{
    node *n = node_for(k, rml_context_hover_element(k->context));
    return n && n->kind != KIND_ROOT ? n->path : NULL;
}

bool kit_pointer_on_control(const kit *k)
{
    rml_element *hover = rml_context_hover_element(k->context);
    for (node *n = node_for(k, hover); n; n = n->parent) {
        switch (n->kind) {
            case KIND_BUTTON:
            case KIND_NUMBER:
            case KIND_CHECK:
            case KIND_TOGGLE:
            case KIND_SELECT:
            case KIND_TEXT:
            case KIND_SEARCH:
            case KIND_AUTOCOMPLETE:
            case KIND_COLOR: return true;
            default: break;
        }
    }
    return false;
}

bool kit_find(const kit *k, const char *path, kit_rect *out)
{
    node *n = path_node(k, path);
    if (!n) {
        return false;
    }
    *out = element_rect(k, n->el);
    return out->w > 0 && out->h > 0;
}

void kit_set_theme(kit *k, const char *name, bool active)
{
    rml_context_activate_theme(k->context, name, active);
    if (strcmp(name, "reduced-motion") == 0) {
        k->reduced_motion = active;
    }
}

void kit_set_body_class(kit *k, const char *name, bool on)
{
    int at = -1;
    for (int i = 0; i < acount(k->body_classes); i++) {
        at = strcmp(k->body_classes[i], name) ? at : i;
    }
    if (on && at < 0) {
        apush(k->body_classes, xstrdup(name));
    }
    if (!on && at >= 0) {
        free(k->body_classes[at]);
        adel(k->body_classes, at);
    }
    for (int i = 0; i < acount(k->documents); i++) {
        rml_element_set_class(k->documents[i], name, on);
    }
}
