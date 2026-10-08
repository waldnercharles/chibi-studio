#include "assets/ui_document.h"
/* The UI Inspector, after Unity's UI Toolkit Debugger, Unreal's Widget
 * Reflector and Chrome's Elements and Styles panes. It picks any element
 * Studio draws: its chrome and authored panels, which share the kit's
 * document; the graph canvas's document, through its surface; and the game
 * UI the Scene shows, through the view zoom's crop. The panel that lists the
 * tree and styles is authored (projects/studio's Inspector), so it inspects
 * itself; studio_sources.c binds it to what this reads. The tree has no row
 * limit: the panel's List shows a window of it (ui_list_window), as
 * Virtuoso does, and only that window's rows are described.
 *
 * Elements come and go between frames, so the Inspector tracks each one as
 * an element_ref and dereferences it only once found again. */
#include "editor/workspace/viewport.h"
#include "editor/studio/studio_internal.h"
#include "support/array.h"
#include "support/ui_style.h"
#include "support/xalloc.h"
#include "ui/list.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Studio scene entity whose panel is the Inspector. */
#define INSPECT_PANEL "Inspector"
#define DEPTH_MAX 64
/* Expanded elements remembered; the oldest is forgotten first. */
#define OPENED_MAX 512
/* The panel's List (ui/Inspector.rml), of which the tree shows a window, and
 * the attribute that names a row's tree row. */
#define LIST_ID "inspector-list"
#define ROW_ATTRIBUTE "data-inspect-row"
/* Rows described while the List has no window yet. */
#define UNLAID_ROWS 40
/* Syncs that try to scroll the selected row into view. */
#define REVEAL_SYNCS 8

static const char *const PSEUDO_NAMES[] = { "hover", "active", "focus", "checked" };
_Static_assert(sizeof PSEUDO_NAMES / sizeof *PSEUDO_NAMES == STUDIO_PSEUDO_COUNT, "a name per pseudo class");

/* The values the Computed pane lists: layout, box, then paint and text. */
// clang-format off
static const char *const COMPUTED[] = {
    "display", "position", "left", "top", "right", "bottom",
    "width", "height", "min-width", "max-width", "min-height", "max-height", "box-sizing",
    "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding-top", "padding-right", "padding-bottom", "padding-left",
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "flex-direction", "flex-grow", "flex-shrink", "flex-basis", "align-items", "justify-content",
    "row-gap", "column-gap", "overflow-x", "overflow-y", "z-index",
    "opacity", "visibility", "pointer-events", "color", "background-color", "border-top-color",
    "border-top-left-radius", "font-family", "font-size", "font-weight", "line-height",
    "text-align", "white-space", "transform", "cursor",
};
// clang-format on

/* An element found again by its place in the tree: the pointer is only
 * compared until an element is found at that place, or anywhere. */
typedef struct element_ref
{
    rml_element *element;
    char tag[24]; /* which an element at a reused address rarely shares */
    bool lost;    /* gone once, so never found at a reused address */
    int place[DEPTH_MAX]; /* the document's index, then each child's */
    int depth;
} element_ref;

/* A row of the tree before it is described: its element, depth, the row of
 * its parent (-1 for a document) and its place among the parent's children
 * (a document's among the context's). */
typedef struct tree_row
{
    rml_element *element;
    int depth, parent, child;
} tree_row;

struct studio_inspect
{
    bool picking;
    studio_inspect_target target;
    bool selected;
    element_ref selection;
    bool hovering;
    element_ref hover;   /* the tree row under the pointer */
    rml_element *picked; /* under the pointer while picking; compared only */
    studio_inspect_target picked_target;
    bool focused;
    element_ref focus;   /* Studio's focus while picking, which a pick keeps */
    tree_row *tree;      /* every row of the tree, while it is read */
    element_ref *places; /* each row of the window's element */
    ui_list_state list;  /* the panel's List, which sizes the window */
    bool list_found;     /* list_ref names the List */
    element_ref list_ref;
    rml_element **open;               /* expanded; compared only */
    bool force[STUDIO_PSEUDO_COUNT];  /* asked for by the panel */
    bool forced[STUDIO_PSEUDO_COUNT]; /* set on forced_on */
    element_ref forced_on;
    studio_inspect_target forced_target;
    int reveal; /* syncs left to scroll the selected row into view */
    uint64_t seen, rows_seen, marks_seen; /* the model's hashes at the last sync */
    studio_inspect_model model;
};

/* A declaration as the element's style holds it. */
typedef struct declared
{
    char name[64], value[160], file[256], rule[128];
    int line, specificity;
} declared;

/* How a target's context pixels land in Studio's dp: origin plus pixels
 * times scale, within clip. */
typedef struct mapping
{
    float x, y, sx, sy;
    kit_rect clip;
} mapping;

typedef struct pick
{
    studio_inspect_target target;
    rml_element *element;
} pick;

/* ---------- Elements ---------- */

static bool is_text(const rml_element *el)
{
    const char *tag = rml_element_tag_name(el);
    return tag[0] == '#' && !strcmp(tag, "#text");
}

static int child_index(const rml_element *parent, const rml_element *child)
{
    int count = rml_element_child_count(parent);
    for (int i = 0; i < count; i++) {
        if (rml_element_child(parent, i) == child) {
            return i;
        }
    }
    return -1;
}

static int document_index(rml_context *context, const rml_element *document)
{
    for (int i = 0; i < rml_context_document_count(context); i++) {
        if (rml_context_document(context, i) == document) {
            return i;
        }
    }
    return -1;
}

/* The element itself, or for a scrollbar's parts, the element they scroll. */
static rml_element *in_dom(rml_element *el)
{
    while (rml_element_owner_document(el) != el &&
           child_index(rml_element_parent(el), el) < 0) {
        el = rml_element_parent(el);
    }
    return el;
}

static void aim(element_ref *r, rml_element *el)
{
    r->element = el;
    r->lost = false;
    snprintf(r->tag, sizeof r->tag, "%s", rml_element_tag_name(el));
}

static element_ref ref_to(rml_element *el)
{
    element_ref r;
    aim(&r, el);
    rml_element *document = rml_element_owner_document(el);
    int up[DEPTH_MAX], n = 0;
    for (rml_element *e = el; e != document; e = rml_element_parent(e)) {
        assert(n < DEPTH_MAX - 1);
        up[n++] = child_index(rml_element_parent(e), e);
    }
    up[n++] = document_index(rml_element_context(el), document);
    for (int i = 0; i < n; i++) {
        r.place[i] = up[n - 1 - i];
    }
    r.depth = n;
    return r;
}

static rml_element *follow(rml_context *context, const element_ref *r)
{
    if (r->place[0] < 0 || r->place[0] >= rml_context_document_count(context)) {
        return NULL;
    }
    rml_element *e = rml_context_document(context, r->place[0]);
    for (int i = 1; e && i < r->depth; i++) {
        e = r->place[i] >= 0 && r->place[i] < rml_element_child_count(e)
              ? rml_element_child(e, r->place[i])
              : NULL;
    }
    return e;
}

/* Whether `el` is `from` or below it, writing its place from `depth` on. */
static bool search(rml_element *from, const rml_element *el, element_ref *r, int depth)
{
    if (from == el) {
        r->depth = depth;
        return true;
    }
    int count = depth < DEPTH_MAX ? rml_element_child_count(from) : 0;
    for (int i = 0; i < count; i++) {
        r->place[depth] = i;
        if (search(rml_element_child(from, i), el, r, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool same(const element_ref *r, rml_element *el)
{
    return el == r->element && !strcmp(rml_element_tag_name(el), r->tag);
}

/* The live element `r` tracks: at its place, else anywhere in the context,
 * which moves its place; NULL once it is gone. */
static rml_element *find(rml_context *context, element_ref *r)
{
    rml_element *at = r->lost ? NULL : follow(context, r);
    if (at && same(r, at)) {
        return at;
    }
    bool found = false;
    for (int d = 0; !r->lost && !found && d < rml_context_document_count(context); d++) {
        r->place[0] = d;
        found = search(rml_context_document(context, d), r->element, r, 1);
    }
    r->lost = !found || !same(r, r->element);
    return r->lost ? NULL : r->element;
}

/* Text on one line: runs of white space become one space, and a character
 * that does not fit is left out whole. */
static void one_line(char *out, size_t size, const char *text)
{
    size_t n = 0;
    bool space = false;
    for (const unsigned char *c = (const unsigned char *)text; *c;) {
        if (isspace(*c)) {
            space = n > 0;
            c++;
            continue;
        }
        size_t length = *c < 0x80 ? 1 : *c < 0xe0 ? 2 : *c < 0xf0 ? 3 : 4;
        if (n + space + length >= size) {
            break;
        }
        if (space) {
            out[n++] = ' ';
        }
        space = false;
        for (size_t i = 0; i < length && *c; i++) {
            out[n++] = (char)*c++;
        }
    }
    out[n] = '\0';
}

static void text_of(const rml_element *el, char *out, size_t size)
{
    char text[512];
    rml_element_text(el, text, sizeof text);
    one_line(out, size, text);
}

/* ---------- The tree ---------- */

static int open_index(const studio_inspect *in, const rml_element *el)
{
    for (int i = 0; i < acount(in->open); i++) {
        if (in->open[i] == el) {
            return i;
        }
    }
    return -1;
}

static void set_open(studio_inspect *in, rml_element *el, bool open)
{
    int i = open_index(in, el);
    if (open && i < 0) {
        if (acount(in->open) == OPENED_MAX) {
            memmove(in->open, in->open + 1, (OPENED_MAX - 1) * sizeof *in->open);
            asetlen(in->open, OPENED_MAX - 1);
        }
        apush(in->open, el);
    }
    if (!open && i >= 0) {
        adel(in->open, i);
    }
}

/* Whether a text node holds nothing but white space. */
static bool blank(const rml_element *el)
{
    char text[512];
    rml_element_text(el, text, sizeof text);
    for (const char *c = text; *c; c++) {
        if (!isspace((unsigned char)*c)) {
            return false;
        }
    }
    return true;
}

/* The panel's own List: its rows are the window of this tree, so listing
 * them would show the tree to itself, as a mirror facing a mirror does. */
static bool is_list(const rml_element *el)
{
    return !strcmp(rml_element_id(el), LIST_ID);
}

/* Whether `el` has children to list under it: not an element whose only
 * child is text, which its own row shows. A text node has no children. */
static bool expandable(const rml_element *el)
{
    int count = rml_element_child_count(el);
    bool text = count == 1 && is_text(rml_element_child(el, 0));
    return count > 0 && !text && !is_list(el);
}

/* The key naming `el` in the panel, its address as text: it names the same
 * element while it lives, whatever rows come or go above it, and is only
 * compared, never followed. */
static void key_of(char out[STUDIO_INSPECT_KEY_SIZE], const rml_element *el)
{
    snprintf(out, STUDIO_INSPECT_KEY_SIZE, "%llx", (unsigned long long)(uintptr_t)el);
}

static void describe_row(const studio_inspect *in, studio_inspect_row *row, rml_element *el, int depth)
{
    *row = (studio_inspect_row){ .depth = depth };
    key_of(row->key, el);
    if (is_text(el)) {
        row->node = true;
        text_of(el, row->text, sizeof row->text);
        return;
    }
    snprintf(row->tag, sizeof row->tag, "%s", rml_element_tag_name(el));
    const char *id = rml_element_id(el);
    snprintf(row->id, sizeof row->id, "%s%s", id[0] ? "#" : "", id);
    char classes[256];
    rml_element_class_names(el, classes, sizeof classes);
    size_t n = 0;
    for (const char *c = classes; *c && n + 1 < sizeof row->classes; c++) {
        bool starts = *c != ' ' && (c == classes || c[-1] == ' ');
        if (starts) {
            row->classes[n++] = '.';
        }
        if (*c != ' ' && n + 1 < sizeof row->classes) {
            row->classes[n++] = *c;
        }
    }
    row->classes[n] = '\0';
    rml_element_attribute(el, "data-ui", row->widget, sizeof row->widget);
    row->parent = expandable(el);
    row->open = row->parent && open_index(in, el) >= 0;
    rml_element *only = rml_element_child_count(el) == 1 ? rml_element_child(el, 0)
                                                         : NULL;
    if (only && is_text(only)) {
        text_of(only, row->text, sizeof row->text);
    }
    if (is_list(el)) {
        snprintf(row->text, sizeof row->text, "%d rows", in->model.count);
    }
    row->hidden = !rml_element_is_visible(el, false);
}

/* Lists `el` and, while it is open, what is under it: one pass over what
 * the tree shows, which describes nothing. */
static void walk(studio_inspect *in, rml_element *el, int depth, int parent, int child)
{
    if (is_text(el) && blank(el)) {
        return;
    }
    int row = acount(in->tree);
    apush(in->tree, ((tree_row){ el, depth, parent, child }));
    if (depth + 1 == DEPTH_MAX || !expandable(el) || open_index(in, el) < 0) {
        return;
    }
    int count = rml_element_child_count(el);
    for (int i = 0; i < count; i++) {
        walk(in, rml_element_child(el, i), depth + 1, row, i);
    }
}

static void list_tree(studio_inspect *in, rml_context *context)
{
    asetlen(in->tree, 0);
    for (int d = 0; d < rml_context_document_count(context); d++) {
        walk(in, rml_context_document(context, d), 0, -1, d);
    }
    in->model.count = acount(in->tree);
}

/* The tree row listing `el`, or -1. */
static int row_of(const studio_inspect *in, const rml_element *el)
{
    for (int i = 0; el && i < acount(in->tree); i++) {
        if (in->tree[i].element == el) {
            return i;
        }
    }
    return -1;
}

/* Where tree row `row` is: its document, then the child to follow at each
 * level down. */
static element_ref place_of(const studio_inspect *in, int row)
{
    const tree_row *l = &in->tree[row];
    element_ref r;
    aim(&r, l->element);
    r.depth = l->depth + 1;
    int d = l->depth;
    for (int i = row; i >= 0; i = in->tree[i].parent) {
        r.place[d--] = in->tree[i].child;
    }
    return r;
}

/* Whether tree row `row` lies in the List's view. */
static bool row_in_view(rml_element *list, int row)
{
    char value[32] = "";
    rml_element_property(list, "--row", value, sizeof value);
    float height = strtof(value, NULL);
    float ratio = rml_context_dp_ratio(rml_element_context(list));
    float top = (float)row * height, scroll = rml_element_scroll_top(list) / ratio,
          view = rml_element_client_height(list) / ratio;
    return top >= scroll - 0.5f && top + height <= scroll + view + 0.5f;
}

/* The rows of the tree to describe: `limit` of them, or the List's window
 * when the panel has one. After a selection it scrolls the selected row into
 * view, again at each sync until it is: the first may find the List too
 * short for a row its content has yet to lay out. */
static int window(studio_inspect *in, rml_element *list, int limit, int *first)
{
    int count = in->model.count, row = in->model.selected_row;
    *first = 0;
    if (!list) {
        return count < limit ? count : limit;
    }
    bool laid_out = rml_element_client_height(list) > 0;
    ui_list view = { .count = count, .reveal = in->reveal ? row : -1 };
    in->list.revealed = -1;
    int length = ui_list_window(list, &view, &in->list, first);
    bool done = row < 0 || (laid_out && row_in_view(list, row));
    in->reveal = done ? 0 : in->reveal - (in->reveal > 0 && laid_out);
    return length;
}

static void describe_window(studio_inspect *in, int first, int length)
{
    studio_inspect_model *m = &in->model;
    asetlen(m->rows, 0);
    asetlen(in->places, 0);
    m->first = first;
    for (int r = first; r < first + length; r++) {
        studio_inspect_row row;
        describe_row(in, &row, in->tree[r].element, in->tree[r].depth);
        apush(m->rows, row);
        apush(in->places, place_of(in, r));
    }
}

/* ---------- The selection ---------- */

/* "Asset:line" from the nearest data-src, else the nearest ui asset or
 * widget (data-ui) at its first line, which for content slotted into a
 * widget names the widget; kit chrome is native. */
static void describe_source(studio_inspect_model *m, rml_element *el)
{
    rml_element *document = rml_element_owner_document(el);
    char widget[128] = "";
    for (rml_element *e = el; e; e = e == document ? NULL : rml_element_parent(e)) {
        char src[160];
        if (rml_element_attribute(e, "data-src", src, sizeof src) && src[0]) {
            snprintf(m->source, sizeof m->source, "%s", src);
            m->editable = true;
            return;
        }
        if (!widget[0]) {
            rml_element_attribute(e, "data-ui", widget, sizeof widget);
        }
    }
    m->editable = widget[0] != '\0';
    snprintf(
        m->source,
        sizeof m->source,
        "%s",
        widget[0]   ? widget
        : m->native ? "Native (C)"
                    : ""
    );
}

/* A box edge in dp to a tenth, with a dash for none, as DevTools shows it. */
static void length(char out[12], float value)
{
    float v = roundf(value * 10) / 10;
    if (v == 0) {
        snprintf(out, 12, "\xe2\x80\x93");
        return;
    }
    snprintf(out, 12, "%g", v);
}

/* The widths between two boxes: top, right, bottom and left. */
static void edges(char out[4][12], rml_element *el, rml_box_area outer, rml_box_area inner, float ratio)
{
    rml_vec2 oa = rml_element_absolute_offset(el, outer),
             os = rml_element_box_size(el, outer);
    rml_vec2 ia = rml_element_absolute_offset(el, inner),
             is = rml_element_box_size(el, inner);
    float e[4] = { ia.y - oa.y,
                   oa.x + os.x - ia.x - is.x,
                   oa.y + os.y - ia.y - is.y,
                   ia.x - oa.x };
    for (int i = 0; i < 4; i++) {
        length(out[i], e[i] / ratio);
    }
}

static void describe_box(studio_inspect_model *m, rml_element *el, float ratio)
{
    edges(m->box[0], el, RML_BOX_MARGIN, RML_BOX_BORDER, ratio);
    edges(m->box[1], el, RML_BOX_BORDER, RML_BOX_PADDING, ratio);
    edges(m->box[2], el, RML_BOX_PADDING, RML_BOX_CONTENT, ratio);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_CONTENT);
    snprintf(
        m->size,
        sizeof m->size,
        "%g \xc3\x97 %g",
        roundf(size.x / ratio * 10) / 10,
        roundf(size.y / ratio * 10) / 10
    );
}

static void collect(void *user, const rml_property_info *p)
{
    declared d = { .line = p->line, .specificity = p->specificity };
    snprintf(d.name, sizeof d.name, "%s", p->name);
    snprintf(d.value, sizeof d.value, "%s", p->value);
    snprintf(d.file, sizeof d.file, "%s", p->path);
    snprintf(d.rule, sizeof d.rule, "%s", p->rule);
    apush(*(declared **)user, d);
}

/* Cascade order: inline first, then the more specific rule, then the later
 * one, each rule's declarations together by name. */
static int by_cascade(const void *a, const void *b)
{
    const declared *x = a, *y = b;
    bool inline_x = !x->rule[0], inline_y = !y->rule[0];
    if (inline_x != inline_y) {
        return inline_x ? -1 : 1;
    }
    if (x->specificity != y->specificity) {
        return y->specificity - x->specificity;
    }
    if (x->line != y->line) {
        return y->line - x->line;
    }
    int order = strcmp(x->file, y->file);
    order = order ? order : strcmp(x->rule, y->rule);
    return order ? order : strcmp(x->name, y->name);
}

static bool same_rule(const declared *a, const declared *b)
{
    return a->line == b->line && !strcmp(a->file, b->file) &&
           !strcmp(a->rule, b->rule);
}

/* The file and line a rule was written at, which ui_style_origin maps from
 * the sheet RmlUi names and the rule's line in it; else the line in the
 * sheet, as for a host's built-in style. */
static void describe_origin(studio_inspect_style *s, const declared *d)
{
    int line = d->line;
    s->linked = ui_style_origin(d->file, d->line, s->file, sizeof s->file, &line);
    s->line = line;
    const char *base = strrchr(s->file, '/');
    base = s->linked ? (base ? base + 1 : s->file) : "";
    snprintf(s->origin, sizeof s->origin, s->linked ? "%s:%d" : "%sline %d", base, line);
}

/* Headings and declarations for `list`, sorted. */
static void add_rules(studio_inspect_model *m, declared *list, bool inherited)
{
    if (acount(list) > 1) {
        qsort(list, (size_t)acount(list), sizeof *list, by_cascade);
    }
    for (int i = 0; i < acount(list); i++) {
        const declared *d = &list[i];
        if (!i || !same_rule(d, &list[i - 1])) {
            studio_inspect_style heading = { .heading = true, .inherited = inherited };
            snprintf(heading.selector, sizeof heading.selector, "%s", d->rule[0] ? d->rule : "style");
            if (d->rule[0]) {
                describe_origin(&heading, d);
            }
            apush(m->styles, heading);
        }
        studio_inspect_style line = { .inherited = inherited };
        snprintf(line.name, sizeof line.name, "%s", d->name);
        snprintf(line.value, sizeof line.value, "%s", d->value);
        apush(m->styles, line);
    }
}

/* The element's own declarations, then those it inherits, by ancestor.
 * RmlUi keeps only the declarations that win, so none is struck through as
 * DevTools strikes an overridden one. */
static void describe_styles(studio_inspect_model *m, rml_element *el)
{
    asetlen(m->styles, 0);
    declared *list = NULL;
    rml_element_local_properties(el, NULL, collect, &list);
    add_rules(m, list, false);
    rml_element *document = rml_element_owner_document(el);
    for (rml_element *a = el == document ? NULL : rml_element_parent(el); a;
         a = a == document ? NULL : rml_element_parent(a)) {
        asetlen(list, 0);
        rml_element_local_properties(a, el, collect, &list);
        if (!acount(list)) {
            continue;
        }
        studio_inspect_style from = { .heading = true, .inherited = true, .ancestor = true };
        char name[96];
        kit_element_selector(a, name, sizeof name);
        snprintf(from.selector, sizeof from.selector, "Inherited from %s", name);
        apush(m->styles, from);
        add_rules(m, list, true);
    }
    afree(list);
}

static void describe_computed(studio_inspect_model *m, rml_element *el)
{
    asetlen(m->computed, 0);
    for (size_t i = 0; i < sizeof COMPUTED / sizeof *COMPUTED; i++) {
        studio_inspect_pair p = { 0 };
        snprintf(p.name, sizeof p.name, "%s", COMPUTED[i]);
        if (rml_element_property(el, COMPUTED[i], p.value, sizeof p.value) &&
            p.value[0]) {
            apush(m->computed, p);
        }
    }
}

static void add_pair(studio_inspect_pair **pairs, const char *name, const char *value)
{
    studio_inspect_pair p;
    snprintf(p.name, sizeof p.name, "%s", name);
    snprintf(p.value, sizeof p.value, "%s", value);
    apush(*pairs, p);
}

static void attribute(void *user, const char *name, const char *value)
{
    if (strcmp(name, "id") != 0 && strcmp(name, "class") != 0) {
        add_pair(user, name, value);
    }
}

static int by_name(const void *a, const void *b)
{
    return strcmp(
        ((const studio_inspect_pair *)a)->name,
        ((const studio_inspect_pair *)b)->name
    );
}

/* id and class, then the rest by name, since RmlUi keeps them unordered. */
static void describe_attributes(studio_inspect_model *m, rml_element *el)
{
    asetlen(m->attributes, 0);
    if (rml_element_id(el)[0]) {
        add_pair(&m->attributes, "id", rml_element_id(el));
    }
    char classes[256];
    if (rml_element_class_names(el, classes, sizeof classes)) {
        add_pair(&m->attributes, "class", classes);
    }
    int first = acount(m->attributes);
    rml_element_attributes(el, attribute, &m->attributes);
    int count = acount(m->attributes) - first;
    if (count > 1) {
        qsort(m->attributes + first, (size_t)count, sizeof *m->attributes, by_name);
    }
}

static void describe(studio_inspect_model *m, rml_context *context, const kit *k, rml_element *el)
{
    kit_element_selector(el, m->title, sizeof m->title);
    m->native = kit_element_path(k, el, m->path, sizeof m->path);
    describe_source(m, el);
    rml_element_pseudo_classes(el, m->pseudo, sizeof m->pseudo);
    describe_box(m, el, rml_context_dp_ratio(context));
    describe_styles(m, el);
    describe_computed(m, el);
    describe_attributes(m, el);
}

/* ---------- Model ---------- */

studio_inspect *studio_inspect_new()
{
    studio_inspect *in = xcalloc(1, sizeof(studio_inspect));
    in->list = (ui_list_state)UI_LIST_STATE_INIT;
    return in;
}

void studio_inspect_free(studio_inspect *in)
{
    if (!in) {
        return;
    }
    studio_inspect_model *m = &in->model;
    afree(m->rows);
    afree(m->styles);
    afree(m->computed);
    afree(m->attributes);
    afree(in->tree);
    afree(in->places);
    afree(in->open);
    free(in);
}

void studio_inspect_select(studio_inspect *in, rml_element *el)
{
    in->selected = true;
    in->selection = ref_to(el);
    in->reveal = REVEAL_SYNCS;
    memset(in->force, 0, sizeof in->force);
    rml_element *document = rml_element_owner_document(el);
    for (rml_element *e = el; e != document;) {
        e = rml_element_parent(e);
        set_open(in, e, true);
    }
}

/* Lists the tree, describes `limit` of its rows or the window of `list`'s,
 * and the selection. */
static studio_inspect_model *read_model(
    studio_inspect *in,
    rml_context *context,
    const kit *k,
    rml_element *list,
    int limit
)
{
    studio_inspect_model *m = &in->model;
    rml_element *selected = in->selected ? find(context, &in->selection) : NULL;
    list_tree(in, context);
    m->selected_row = row_of(in, selected);
    m->picked_row = in->picking && in->picked_target == in->target
                      ? row_of(in, in->picked)
                      : -1;
    m->selected_key[0] = m->picked_key[0] = '\0';
    if (m->selected_row >= 0) {
        key_of(m->selected_key, selected);
    }
    if (m->picked_row >= 0) {
        key_of(m->picked_key, in->picked);
    }
    int first;
    int length = window(in, list, limit, &first);
    describe_window(in, first, length);
    m->selection = in->selected;
    m->detached = in->selected && !selected;
    memcpy(m->force, in->force, sizeof m->force);
    if (selected) {
        describe(m, context, k, selected);
    }
    return m;
}

const studio_inspect_model *studio_inspect_read(studio_inspect *in, rml_context *context, const kit *k)
{
    return read_model(in, context, k, NULL, INT_MAX);
}

/* ---------- Change ---------- */

static uint64_t mix(uint64_t h, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    }
    return h;
}

static uint64_t mix_text(uint64_t h, const char *text)
{
    return mix(h, text, strlen(text) + 1);
}

static uint64_t mix_flags(uint64_t h, const bool *flags, int count)
{
    for (int i = 0; i < count; i++) {
        h = mix(h, &(unsigned char){ flags[i] }, 1);
    }
    return h;
}

/* The window of rows the List shows, as another hash, so a scroll or a fold
 * changes the rows alone and a selection the details alone. */
static uint64_t rows_hash(const studio_inspect_model *m)
{
    uint64_t h = mix(0xcbf29ce484222325ull, &m->first, sizeof m->first);
    h = mix(h, &(int){ acount(m->rows) }, sizeof(int));
    for (int i = 0; i < acount(m->rows); i++) {
        const studio_inspect_row *r = &m->rows[i];
        h = mix(h, &r->depth, sizeof r->depth);
        bool flags[] = { r->node, r->parent, r->open, r->hidden };
        h = mix_flags(h, flags, (int)(sizeof flags / sizeof *flags));
        const char *texts[] = { r->key,     r->tag,    r->id,
                                r->classes, r->widget, r->text };
        for (size_t t = 0; t < sizeof texts / sizeof *texts; t++) {
            h = mix_text(h, texts[t]);
        }
    }
    return h;
}

static uint64_t mix_styles(uint64_t h, const studio_inspect_style *styles)
{
    h = mix(h, &(int){ acount(styles) }, sizeof(int));
    for (int i = 0; i < acount(styles); i++) {
        const studio_inspect_style *st = &styles[i];
        bool flags[] = { st->heading, st->ancestor, st->inherited, st->linked };
        h = mix_flags(h, flags, (int)(sizeof flags / sizeof *flags));
        h = mix_text(mix_text(h, st->selector), st->origin);
        h = mix_text(mix_text(h, st->name), st->value);
    }
    return h;
}

static uint64_t mix_pairs(uint64_t h, const studio_inspect_pair *pairs)
{
    h = mix(h, &(int){ acount(pairs) }, sizeof(int));
    for (int i = 0; i < acount(pairs); i++) {
        h = mix_text(mix_text(h, pairs[i].name), pairs[i].value);
    }
    return h;
}

/* The keys of the selected row and the picked one, as a hash of their own, so
 * a selection or a pick dirties only the views that compare them. */
static uint64_t marks_hash(const studio_inspect_model *m)
{
    return mix_text(mix_text(0xcbf29ce484222325ull, m->selected_key), m->picked_key);
}

/* What the panel shows but the rows, as one hash: equal hashes show the
 * same model from the same memory, since lists only move when their counts
 * grow. */
static uint64_t model_hash(const studio_inspect_model *m)
{
    uint64_t h = 0xcbf29ce484222325ull;
    bool flags[] = { m->picking,    m->wide,       m->selection,
                     m->detached,   m->native,     m->editable,
                     m->targets[0], m->targets[1], m->targets[2] };
    h = mix_flags(h, flags, (int)(sizeof flags / sizeof *flags));
    h = mix_flags(h, m->force, STUDIO_PSEUDO_COUNT);
    h = mix(h, &(int){ (int)m->target }, sizeof(int));
    const char *texts[] = { m->shortcut, m->title,  m->path,
                            m->source,   m->pseudo, m->size };
    for (size_t t = 0; t < sizeof texts / sizeof *texts; t++) {
        h = mix_text(h, texts[t]);
    }
    for (int b = 0; b < 3; b++) {
        for (int e = 0; e < 4; e++) {
            h = mix_text(h, m->box[b][e]);
        }
    }
    h = mix_styles(h, m->styles);
    return mix_pairs(mix_pairs(h, m->computed), m->attributes);
}

/* ---------- Targets ---------- */

static rml_context *target_context(const studio *s, studio_inspect_target target)
{
    switch (target) {
        case STUDIO_INSPECT_STUDIO: return rml_backend_context(s->backend);
        case STUDIO_INSPECT_GRAPH:
            return s->graph.kit ? rml_backend_context(s->graph.backend) : NULL;
        case STUDIO_INSPECT_GAME:
            /* The viewport has documents once the Scene drew through it. */
            return s->scene->render_viewport == s->viewport
                     ? render_ui_context(engine_viewport_ui(s->viewport))
                     : NULL;
        case STUDIO_INSPECT_TARGET_COUNT: break;
    }
    return NULL;
}

/* Game documents have no kit of their own; Studio's names them by steps. */
static const kit *target_kit(const studio *s, studio_inspect_target target)
{
    return target == STUDIO_INSPECT_GRAPH ? s->graph.kit : s->kit;
}

static mapping target_mapping(const studio *s, studio_inspect_target target)
{
    if (target == STUDIO_INSPECT_GRAPH) {
        kit_rect r = s->graph.surface.rect;
        float scale = 1 / studio_pixel_ratio(s);
        return (mapping){ r.x, r.y, scale, scale, r };
    }
    if (target == STUDIO_INSPECT_GAME) {
        /* The Scene surface shows the part of the canvas the view zoom crops. */
        kit_rect r = s->surface.rect;
        int w, h;
        engine_viewport_pixel_size(s->viewport, &w, &h);
        float zoom = s->view.zoom;
        return (mapping){ r.x + (0.5f - s->view.x * zoom) * r.w,
                          r.y + (0.5f - s->view.y * zoom) * r.h,
                          r.w * zoom / (float)(w > 0 ? w : 1),
                          r.h * zoom / (float)(h > 0 ? h : 1),
                          r };
    }
    float scale = 1 / s->in.dp_ratio;
    return (mapping){ 0, 0, scale, scale, { 0 } };
}

/* The context a Studio surface shows, by its texture. */
static studio_inspect_target surface_target(const rml_element *el)
{
    char src[32] = "";
    if (el && !strcmp(rml_element_tag_name(el), "img")) {
        rml_element_attribute(el, "src", src, sizeof src);
    }
    return !strcmp(src, "kit:graph")    ? STUDIO_INSPECT_GRAPH
         : !strcmp(src, "kit:viewport") ? STUDIO_INSPECT_GAME
                                        : STUDIO_INSPECT_TARGET_COUNT;
}

/* ---------- Picking ---------- */

static bool contains(rml_element *el, rml_vec2 p)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    return p.x >= at.x && p.y >= at.y && p.x < at.x + size.x && p.y < at.y + size.y;
}

/* The last, innermost shown element at or below `el` whose box holds `p`,
 * pointer-events: none included, as a HUD's text is. */
static rml_element *innermost(rml_element *el, rml_vec2 p, const rml_element *ignore)
{
    rml_element *found = NULL;
    int count = rml_element_child_count(el);
    for (int i = 0; i < count; i++) {
        rml_element *child = rml_element_child(el, i);
        if (child == ignore || is_text(child) || !rml_element_is_visible(child, false)) {
            continue;
        }
        rml_element *inner = innermost(child, p, ignore);
        found = inner ? inner : found;
    }
    return found ? found : contains(el, p) ? el : NULL;
}

/* The element that takes the pointer at `p`, then its innermost part
 * there; where nothing takes it, the innermost of the topmost document. */
static rml_element *element_at(rml_context *context, rml_vec2 p, const rml_element *ignore)
{
    rml_element *hit = rml_context_element_at_point(context, p, ignore);
    if (hit) {
        rml_element *inner = innermost(hit, p, ignore);
        return in_dom(inner ? inner : hit);
    }
    for (int d = rml_context_document_count(context) - 1; d >= 0; d--) {
        rml_element *inner = innermost(rml_context_document(context, d), p, ignore);
        if (inner) {
            return in_dom(inner);
        }
    }
    return NULL;
}

/* The element under the pointer, looking through a surface into the
 * context it shows. */
static pick pick_at(studio *s, const rml_element *cover)
{
    float ratio = s->in.dp_ratio;
    rml_vec2 p = { s->in.mouse_x * ratio, s->in.mouse_y * ratio };
    rml_element *el = element_at(target_context(s, STUDIO_INSPECT_STUDIO), p, cover);
    studio_inspect_target inner = surface_target(el);
    rml_context *context = inner < STUDIO_INSPECT_TARGET_COUNT
                             ? target_context(s, inner)
                             : NULL;
    if (context) {
        mapping m = target_mapping(s, inner);
        rml_vec2 q = { (s->in.mouse_x - m.x) / m.sx, (s->in.mouse_y - m.y) / m.sy };
        rml_element *found = element_at(context, q, NULL);
        if (found) {
            return (pick){ inner, found };
        }
    }
    return (pick){ STUDIO_INSPECT_STUDIO, el };
}

/* ---------- Studio ---------- */

static studio_inspect *inspector(studio *s)
{
    if (!s->inspect) {
        s->inspect = studio_inspect_new();
    }
    return s->inspect;
}

static int panel_area(const studio *s)
{
    const studio_layout *layout = &s->prefs.layouts[s->prefs.workspace];
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        if (layout->editors[i] == STUDIO_EDITOR_PANEL &&
            !strcmp(layout->panels[i], INSPECT_PANEL) && s->areas[i].rect.w > 0) {
            return i;
        }
    }
    return -1;
}

/* The panel's List, found again by its place, else by its id; NULL before
 * it is built. */
static rml_element *panel_list(const studio *s, studio_inspect *in)
{
    rml_context *context = target_context(s, STUDIO_INSPECT_STUDIO);
    rml_element *list = in->list_found ? find(context, &in->list_ref) : NULL;
    for (int d = 0; !list && d < rml_context_document_count(context); d++) {
        list = rml_element_by_id(rml_context_document(context, d), LIST_ID);
        if (list) {
            in->list_ref = ref_to(list);
        }
    }
    in->list_found = list != NULL;
    return list;
}

/* In the drawer, as DevTools docks at the bottom, unless an area shows it. */
static void show_panel(studio *s)
{
    if (panel_area(s) >= 0) {
        return;
    }
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_DRAWER] = STUDIO_EDITOR_PANEL;
    snprintf(layout->panels[STUDIO_AREA_DRAWER], sizeof layout->panels[STUDIO_AREA_DRAWER], "%s", INSPECT_PANEL);
    layout->drawer_open = true;
}

bool studio_inspect_picking(const studio *s)
{
    return s->inspect && s->inspect->picking;
}

void studio_inspect_toggle(studio *s)
{
    studio_inspect *in = inspector(s);
    in->picking = !in->picking;
    if (in->picking) {
        show_panel(s);
    }
}

bool studio_inspect_key(studio *s, const SDL_KeyboardEvent *key)
{
    bool gameplay = !studio_previewed_ui(s) && engine_viewport_focused(s->viewport);
    studio_editor editor = s->hover_area >= 0
                             ? studio_active_layout(s)->editors[s->hover_area]
                             : STUDIO_EDITOR_COUNT;
    bool typing = kit_wants_keyboard(s->kit) || studio_graph_wants_keyboard(s);
    bool chord = (!(gameplay || typing) || studio_primary(s, key->mod)) &&
                 studio_command_for_key(&s->prefs, key->key, key->mod, editor, s->mac) ==
                     STUDIO_COMMAND_INSPECT;
    if (chord) {
        if (!key->repeat) {
            studio_command_run(s, STUDIO_COMMAND_INSPECT);
        }
        return true;
    }
    if (key->key != SDLK_ESCAPE || !studio_inspect_picking(s)) {
        return false;
    }
    s->inspect->picking = false;
    return true;
}

/* In the Text Editor at a line of the part: a ui asset's .rml, whose lines
 * data-src names, or its .rcss, whose lines a rule's origin names; a
 * stylesheet has only its RCSS. */
static void open_source(studio *s, const char *path, ui_part part, int line)
{
    bool opened = part == UI_PART_RCSS ? studio_open_ui_part(s, path, part, line)
                                       : studio_open_ui_source(s, path, line);
    if (opened) {
        return;
    }
    char message[1100];
    snprintf(message, sizeof message, "Could not open %s", path);
    kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = "Edit Source", .description = message }
    );
    studio_record(s, KIT_TOAST_WARNING, message);
}

/* The selection's ui asset at its line: Studio's own panels are in the
 * Studio project, a game's documents in the open project. */
static void edit_source(studio *s, const studio_inspect *in)
{
    const studio_inspect_model *m = &in->model;
    if (!m->selection || !m->editable) {
        return;
    }
    const char *colon = strrchr(m->source, ':');
    int line = colon ? atoi(colon + 1) : 1;
    int asset = colon ? (int)(colon - m->source) : (int)strlen(m->source);
    char path[1400];
    snprintf(
        path,
        sizeof path,
        "%s/ui/%.*s" UI_MARKUP_EXTENSION,
        in->target == STUDIO_INSPECT_GAME ? s->scene->content_root
                                          : resource_studio_content(),
        asset,
        m->source
    );
    open_source(s, path, UI_PART_RML, line > 0 ? line : 1);
}

static void open_rule(studio *s, const studio_inspect *in, int row)
{
    const studio_inspect_style *style = row >= 0 && row < acount(in->model.styles)
                                          ? &in->model.styles[row]
                                          : NULL;
    if (style && style->linked) {
        open_source(s, style->file, UI_PART_RCSS, style->line);
    }
}

/* The element of window row `at`, found again, or NULL. */
static rml_element *row_element(studio_inspect *in, rml_context *context, int at)
{
    return at >= 0 && at < acount(in->places) ? find(context, &in->places[at]) : NULL;
}

/* The window row `key` names, or -1. */
static int window_row(const studio_inspect *in, const char *key)
{
    for (int i = 0; i < acount(in->model.rows); i++) {
        if (!strcmp(in->model.rows[i].key, key)) {
            return i;
        }
    }
    return -1;
}

void studio_inspect_click(studio *s, const char *key, bool open)
{
    studio_inspect *in = inspector(s);
    int at = window_row(in, key);
    rml_element *el = row_element(in, target_context(s, in->target), at);
    if (!el) {
        return;
    }
    /* The twisty is inside its row, so its click also reaches the row. */
    if (open != in->model.rows[at].open) {
        set_open(in, el, open);
        return;
    }
    studio_inspect_select(in, el);
}

static void apply(studio *s, studio_inspect *in, const studio_inspect_input *input)
{
    memcpy(in->force, input->force, sizeof in->force);
    if (input->source) {
        edit_source(s, in);
    }
    open_rule(s, in, input->rule);
    /* The panel asks for a target by changing the one it last showed; a pick
       may have moved on since. */
    bool other = input->target >= 0 && input->target < STUDIO_INSPECT_TARGET_COUNT &&
                 input->target != (int)in->model.target;
    if (other && target_context(s, (studio_inspect_target)input->target)) {
        in->target = (studio_inspect_target)input->target;
        in->selected = in->hovering = false;
    }
}

const studio_inspect_model *studio_inspect_sync(studio *s, const studio_inspect_input *input)
{
    if (panel_area(s) < 0 && !studio_inspect_picking(s)) {
        return NULL;
    }
    studio_inspect *in = inspector(s);
    if (!target_context(s, in->target)) {
        in->target = STUDIO_INSPECT_STUDIO;
        in->selected = in->hovering = false;
    }
    apply(s, in, input);
    studio_inspect_model *m = read_model(
        in,
        target_context(s, in->target),
        target_kit(s, in->target),
        panel_list(s, in),
        UNLAID_ROWS
    );
    m->target = in->target;
    m->picking = in->picking;
    s->ui_schedule.wake |= in->reveal > 0;
    for (int t = 0; t < STUDIO_INSPECT_TARGET_COUNT; t++) {
        m->targets[t] = target_context(s, (studio_inspect_target)t) != NULL;
    }
    int area = panel_area(s);
    m->wide = area >= 0 && s->areas[area].rect.w > s->areas[area].rect.h * 1.5f;
    snprintf(
        m->shortcut,
        sizeof m->shortcut,
        "%s",
        studio_command_describe(s, STUDIO_COMMAND_INSPECT).shortcut
    );
    uint64_t hash = model_hash(m), rows = rows_hash(m), marks = marks_hash(m);
    m->revision += hash != in->seen;
    m->rows_revision += rows != in->rows_seen;
    m->marks_revision += marks != in->marks_seen;
    in->seen = hash;
    in->rows_seen = rows;
    in->marks_seen = marks;
    return m;
}

const studio_inspect_model *studio_inspect_shown(const studio *s)
{
    return s->inspect ? &s->inspect->model : NULL;
}

static kit_rect box_rect(rml_element *el, rml_box_area area, mapping m)
{
    rml_vec2 at = rml_element_absolute_offset(el, area);
    rml_vec2 size = rml_element_box_size(el, area);
    return (kit_rect){ m.x + at.x * m.sx, m.y + at.y * m.sy, size.x * m.sx, size.y * m.sy };
}

static bool clips(rml_element *el)
{
    char x[16], y[16];
    rml_element_property(el, "overflow-x", x, sizeof x);
    rml_element_property(el, "overflow-y", y, sizeof y);
    return strcmp(x, "visible") != 0 || strcmp(y, "visible") != 0;
}

static kit_rect intersect(kit_rect a, kit_rect b)
{
    float x = fmaxf(a.x, b.x), y = fmaxf(a.y, b.y);
    float right = fminf(a.x + a.w, b.x + b.w), bottom = fminf(a.y + a.h, b.y + b.h);
    return (kit_rect){ x, y, fmaxf(right - x, 0), fmaxf(bottom - y, 0) };
}

/* Where the element can show: its surface, less what its scrolling and
 * clipping ancestors hide, as a scrolled-away row is. False when none of
 * it shows. */
static bool shown_area(rml_element *el, mapping m, kit_rect *clip)
{
    *clip = m.clip;
    bool clipped = m.clip.w > 0;
    rml_element *document = rml_element_owner_document(el);
    for (rml_element *a = el; a != document;) {
        a = rml_element_parent(a);
        if (clips(a)) {
            kit_rect r = box_rect(a, RML_BOX_PADDING, m);
            *clip = clipped ? intersect(*clip, r) : r;
            clipped = true;
        }
    }
    return !clipped || (clip->w > 0 && clip->h > 0);
}

/* DevTools' highlight with a label naming the element and its size; an
 * outline alone marks the selection. */
static void draw(studio *s, const char *key, pick p, bool outline)
{
    mapping m = target_mapping(s, p.target);
    rml_element *el = p.element;
    kit_highlight_desc d = { .margin = box_rect(el, RML_BOX_MARGIN, m),
                             .border = box_rect(el, RML_BOX_BORDER, m),
                             .padding = box_rect(el, RML_BOX_PADDING, m),
                             .content = box_rect(el, RML_BOX_CONTENT, m) };
    if (!shown_area(el, m, &d.clip)) {
        return;
    }
    char name[96], label[160];
    if (outline) {
        d.margin = d.padding = d.content = d.border;
    } else {
        kit_element_selector(el, name, sizeof name);
        rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
        float ratio = rml_context_dp_ratio(rml_element_context(el));
        snprintf(
            label,
            sizeof label,
            "%s  %g \xc3\x97 %g",
            name,
            roundf(size.x / ratio),
            roundf(size.y / ratio)
        );
        d.label = label;
    }
    kit_highlight(s->kit, key, &d, outline ? "selected" : "hovered");
}

/* The pick under the pointer, else the tree row under it, else the
 * selection while the panel shows. */
static void highlight(studio *s, studio_inspect *in, pick picked, bool shown)
{
    if (picked.element) {
        draw(s, "hover", picked, false);
        return;
    }
    rml_context *context = target_context(s, in->target);
    if (!context) {
        return;
    }
    rml_element *hovered = in->hovering ? find(context, &in->hover) : NULL;
    if (hovered) {
        draw(s, "hover", (pick){ in->target, hovered }, false);
        return;
    }
    rml_element *selected = shown && in->selected ? find(context, &in->selection)
                                                  : NULL;
    if (selected) {
        draw(s, "selection", (pick){ in->target, selected }, true);
    }
}

static bool any(const bool *flags)
{
    for (int i = 0; i < STUDIO_PSEUDO_COUNT; i++) {
        if (flags[i]) {
            return true;
        }
    }
    return false;
}

/* Forced pseudo classes stay set on the selection while the panel shows,
 * set again each frame since pointer and focus changes clear them. */
static void force_pseudo(studio *s, studio_inspect *in, bool shown)
{
    rml_context *context = target_context(s, in->forced_target);
    rml_element *forced = context && any(in->forced) ? find(context, &in->forced_on)
                                                     : NULL;
    rml_context *target = target_context(s, in->target);
    rml_element *selected = shown && in->selected && target
                              ? find(target, &in->selection)
                              : NULL;
    if (forced && forced != selected) {
        for (int i = 0; i < STUDIO_PSEUDO_COUNT; i++) {
            if (in->forced[i]) {
                rml_element_set_pseudo_class(forced, PSEUDO_NAMES[i], false);
            }
        }
    }
    if (!forced || forced != selected) {
        memset(in->forced, 0, sizeof in->forced);
    }
    if (!selected) {
        return;
    }
    in->forced_on = in->selection;
    in->forced_target = in->target;
    for (int i = 0; i < STUDIO_PSEUDO_COUNT; i++) {
        if (in->force[i] || in->forced[i]) {
            rml_element_set_pseudo_class(selected, PSEUDO_NAMES[i], in->force[i]);
        }
        in->forced[i] = in->force[i];
    }
}

/* A press on the overlay moves RmlUi's focus to its document; the focus
 * picking began with comes back, so a dialog keeps its keys. */
static void keep_focus(studio *s, studio_inspect *in)
{
    rml_context *context = target_context(s, STUDIO_INSPECT_STUDIO);
    rml_element *focus = rml_context_focus_element(context);
    if (!in->picking) {
        rml_element *kept = in->focused ? find(context, &in->focus) : NULL;
        if (kept && kept != focus) {
            rml_element_focus(kept, false);
        }
        in->focused = false;
        return;
    }
    /* A press may come a frame before its click. */
    if (s->in.down[0] || s->in.pressed[0]) {
        return;
    }
    in->focused = focus != NULL;
    if (focus) {
        in->focus = ref_to(focus);
    }
}

/* The tree row under the pointer, by the attribute its row carries, as the
 * element it lists: the app highlights it. */
static void track_hover(studio *s, studio_inspect *in)
{
    in->hovering = false;
    rml_element *over = rml_context_hover_element(target_context(s, STUDIO_INSPECT_STUDIO));
    while (over && !rml_element_has_attribute(over, ROW_ATTRIBUTE)) {
        over = rml_element_parent(over);
    }
    char key[STUDIO_INSPECT_KEY_SIZE] = "";
    if (over) {
        rml_element_attribute(over, ROW_ATTRIBUTE, key, sizeof key);
    }
    int at = over ? window_row(in, key) : -1;
    if (at >= 0) {
        in->hovering = true;
        in->hover = in->places[at];
    }
}

void studio_inspect_emit(studio *s)
{
    studio_inspect *in = s->inspect;
    if (!in) {
        return;
    }
    bool shown = panel_area(s) >= 0;
    if (in->picking || shown) {
        kit *k = s->kit;
        track_hover(s, in);
        rml_element *cover = kit_inspect_begin(k, "inspect", in->picking);
        pick picked = in->picking ? pick_at(s, cover) : (pick){ in->target, NULL };
        in->picked = picked.element;
        in->picked_target = picked.target;
        highlight(s, in, picked, shown);
        kit_inspect_end(k);
        if (picked.element && kit_item_clicked(k)) {
            in->picking = false;
            in->target = picked.target;
            in->hovering = false;
            studio_inspect_select(in, picked.element);
            s->ui_schedule.wake = true;
        }
    }
    keep_focus(s, in);
    force_pseudo(s, in, shown);
}
