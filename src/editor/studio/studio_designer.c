#include "assets/ui_document.h"
/* The visual designer (workstream H). While a ui asset's UI document is
 * active, the Outliner is its Hierarchy, as Unity's UI Builder and Unreal's
 * UMG Designer list a widget's tree. The Canvas in the Scene area outlines
 * the selection and the element under the pointer, selects with a click and
 * takes Library drops, marking where between siblings they land. Selecting
 * in the Hierarchy, in the Canvas or with the Text Editor's cursor selects
 * in all of them. This file keeps the document, the selection, its changes
 * and the Canvas; every change is a text edit of the document
 * (editor/documents/ui_design.h): one undo step that keeps the author's
 * formatting. The Hierarchy, Details and the Library are
 * studio_designer_hierarchy.c, studio_designer_details.c and
 * studio_designer_library.c. */
#include "editor/workspace/viewport.h"
#include "editor/studio/studio_designer_internal.h"
#include "support/array.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The share of a Hierarchy row, at its top and at its bottom, where a drop
 * goes before or after the element rather than inside it, as dnd-kit's
 * sortable trees split a row. */
#define DROP_EDGE 0.25f

/* ---------- The document ---------- */

static void forget(studio_designer *d)
{
    ui_source_free(d->rml);
    ui_source_free(d->rcss);
    designer_hierarchy_forget(d);
    designer_details_forget(d);
}

void studio_designer_free(studio *s)
{
    if (!s->designer) {
        return;
    }
    forget(s->designer);
    free(s->designer);
    s->designer = NULL;
}

bool studio_designer_active(studio *s)
{
    ui_buffer *b = studio_ui_buffer(s);
    return b && ui_buffer_text(b, UI_PART_RML);
}

/* Another document starts with nothing selected or folded. Its asset's
 * name, as data-src gives it, is its file's. */
static void open_document(studio_designer *d, const char *path)
{
    forget(d);
    *d = (studio_designer){ .selected = -1, .hovered = -1, .menu = -1, .reveal_cursor = -1 };
    for (int area = 0; area < STUDIO_AREA_COUNT; area++) {
        d->lists[area] = (ui_list_state)UI_LIST_STATE_INIT;
    }
    snprintf(d->path, sizeof d->path, "%s", path);
    const char *base = content_basename(path), *dot = strrchr(base, '.');
    snprintf(d->asset, sizeof d->asset, "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
}

studio_designer *designer_state(studio *s, ui_buffer **out)
{
    ui_buffer *b = studio_ui_buffer(s);
    if (!b || !ui_buffer_text(b, UI_PART_RML)) {
        return NULL;
    }
    if (!s->designer) {
        s->designer = xcalloc(1, sizeof *s->designer);
    }
    studio_designer *d = s->designer;
    if (strcmp(d->path, b->path)) {
        open_document(d, b->path);
    }
    if (!d->rml || d->parsed != b->revision) {
        /* The selection follows its element through an undo or typing. */
        ui_source *before = d->rml;
        ui_source_free(d->rcss);
        d->rml = ui_design_parse(b, UI_PART_RML);
        d->rcss = ui_design_parse(b, UI_PART_RCSS);
        d->parsed = b->revision;
        d->selected = before ? ui_design_follow(before, d->rml, d->selected) : -1;
        ui_source_free(before);
    }
    *out = b;
    return d;
}

void designer_select(studio *s, int element)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    assert(d && element < acount(d->rml->elements));
    d->selected = element;
    designer_reveal(d);
    s->ui_schedule.wake = true;
    if (element >= 0) {
        d->reveal_cursor = d->rml->elements[element].start;
        b->part = UI_PART_RML;
    }
}

void designer_warn(studio *s, const char *title, const char *error)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title, .description = error });
    studio_record(s, KIT_TOAST_WARNING, error);
}

/* ---------- Elements ---------- */

void designer_element_name(const ui_source *rml, int element, char *out, size_t size)
{
    const ui_source_element *el = &rml->elements[element];
    char *widget = strcmp(el->tag, "use")
                     ? NULL
                     : ui_source_attribute_value(rml, element, "ui");
    snprintf(out, size, "%s", widget && widget[0] ? widget : el->tag);
    free(widget);
}

typedef struct
{
    const char *tag;
    uint32_t icon;
} tag_icon;

static const tag_icon TAG_ICONS[] = {
    { "use", KIT_ICON_WIDGET },
    { "slot", KIT_ICON_SLOT },
    { "button", KIT_ICON_BUTTON },
    { "select", KIT_ICON_INPUT },
    { "textarea", KIT_ICON_INPUT },
    { "img", KIT_ICON_IMAGE },
    { "progress", KIT_ICON_PROGRESS },
    { "span", KIT_ICON_TEXT },
    { "p", KIT_ICON_TEXT },
    { "label", KIT_ICON_TEXT },
    { "h1", KIT_ICON_TEXT },
    { "h2", KIT_ICON_TEXT },
    { "h3", KIT_ICON_TEXT },
    { "morph-text", KIT_ICON_TEXT },
    { "number-flow", KIT_ICON_NUMBER },
};

/* An input by its type: a checkbox or radio toggles, a range slides. */
static uint32_t input_icon(const ui_source *rml, int element)
{
    char *type = ui_source_attribute_value(rml, element, "type");
    uint32_t icon = !type ? KIT_ICON_INPUT
                  : !strcmp(type, "checkbox") || !strcmp(type, "radio") ? KIT_ICON_CHECKBOX
                  : !strcmp(type, "range") ? KIT_ICON_SLIDER
                                           : KIT_ICON_INPUT;
    free(type);
    return icon;
}

uint32_t designer_element_icon(const ui_source *rml, int element)
{
    const char *tag = rml->elements[element].tag;
    if (!strcmp(tag, "input")) {
        return input_icon(rml, element);
    }
    for (size_t i = 0; i < sizeof TAG_ICONS / sizeof *TAG_ICONS; i++) {
        if (!strcmp(TAG_ICONS[i].tag, tag)) {
            return TAG_ICONS[i].icon;
        }
    }
    return KIT_ICON_ELEMENT;
}

/* ---------- Changes ---------- */

/* After a structural change: selects `selected` and reports it with
 * `verb`, or warns why the change failed. */
static void changed(studio *s, int selected, const char *error, const char *failed, const char *verb)
{
    if (error[0]) {
        designer_warn(s, failed, error);
        return;
    }
    designer_select(s, selected);
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    char name[96] = "", message[160];
    if (selected >= 0) {
        designer_element_name(d->rml, selected, name, sizeof name);
    }
    snprintf(message, sizeof message, "%s %s", verb, name);
    studio_report(s, message);
}

void designer_add(studio *s, int target, const char *markup, const char *label)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    ui_source_place place = target < 0 || ui_design_container(d->rml, target)
                              ? UI_SOURCE_INSIDE
                              : UI_SOURCE_AFTER;
    char error[192] = "", failed[128];
    snprintf(failed, sizeof failed, "Could not add %s", label);
    changed(s, ui_design_insert(b, target, place, markup, error, sizeof error), error, failed, "Added");
}

void designer_duplicate(studio *s)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    char error[192] = "";
    if (d && d->selected >= 0) {
        int copy = ui_design_duplicate(b, d->selected, error, sizeof error);
        changed(s, copy, error, "Could not duplicate", "Duplicated");
    }
}

void designer_wrap(studio *s)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    char error[192] = "";
    if (d && d->selected >= 0) {
        int wrapper = ui_design_wrap(b, d->selected, "div", error, sizeof error);
        changed(s, wrapper, error, "Could not wrap", "Wrapped in");
    }
}

/* Deletes the selection, offering Undo while the delete is the latest edit. */
void designer_delete(studio *s)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    if (!d || d->selected < 0) {
        return;
    }
    char name[96], error[192] = "", title[160];
    designer_element_name(d->rml, d->selected, name, sizeof name);
    int next = ui_design_delete(b, d->selected, error, sizeof error);
    if (error[0]) {
        designer_warn(s, "Could not delete", error);
        return;
    }
    snprintf(title, sizeof title, "Deleted %s", name);
    kit_toast_dismiss(s->kit, d->undo.toast);
    d->undo.toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .title = title, .action = "Undo", .duration = 6 }
    );
    d->undo.revision = b->revision;
    studio_record(s, KIT_TOAST_DEFAULT, title);
    designer_select(s, next);
}

static void undo_delete(studio *s, studio_designer *d, ui_buffer *b)
{
    if (!d->undo.toast || !kit_toast_action(s->kit, d->undo.toast)) {
        return;
    }
    d->undo.toast = 0;
    if (b->revision == d->undo.revision && ui_buffer_can_undo(b)) {
        ui_buffer_undo(b);
        studio_report(s, "Undo");
    }
}

void designer_move(studio *s, ui_buffer *b, int element, int target, ui_source_place place)
{
    char error[192] = "";
    int moved = ui_design_move(b, element, target, place, error, sizeof error);
    changed(s, moved, error, "Could not move", "Moved");
}

/* A Hierarchy row's drop ("#<element>") moves the element, a Library item's
 * adds its markup. A row dropped on itself stays. */
static void drop(studio *s, studio_designer *d, ui_buffer *b, const char *payload, int target, ui_source_place place)
{
    char error[192] = "";
    if (payload[0] != '#') {
        int added = ui_design_insert(b, target, place, payload, error, sizeof error);
        changed(s, added, error, "Could not add", "Added");
        return;
    }
    int element = atoi(payload + 1);
    bool valid = element >= 0 && element < acount(d->rml->elements);
    if (!valid || (target >= element &&
                   target < element + ui_design_extent(d->rml, element))) {
        return;
    }
    designer_move(s, b, element, target, place);
}

void designer_drop(studio *s, const char *payload, int target, ui_source_place place)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    if (d) {
        drop(s, d, b, payload, target, place);
    }
}

/* Opens the widget a <use> element shows, beside the document. */
void designer_open_widget(studio *s, int element)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    char *widget = d ? ui_source_attribute_value(d->rml, element, "ui") : NULL;
    if (!widget) {
        return;
    }
    char path[sizeof d->path + 160];
    const char *base = content_basename(d->path);
    snprintf(path, sizeof path, "%.*s%s" UI_MARKUP_EXTENSION, (int)(base - d->path), d->path, widget);
    free(widget);
    studio_open_ui_source(s, path, 1);
}

/* ---------- Text Editor ---------- */

/* The Text Editor's cursor selects the element it is in, and a selection
 * made elsewhere puts the cursor at its element, while the editor holds the
 * document's markup as it is. A change to the text moves the cursor without
 * selecting. */
static void follow_cursor(studio *s, studio_designer *d, ui_buffer *b)
{
    kit_code_editor *e = s->code.editor;
    if (!e || b->part != UI_PART_RML || s->code.editor_part != UI_PART_RML ||
        strcmp(s->code.editor_path, b->path) ||
        strcmp(kit_code_text(e, NULL), ui_buffer_text(b, UI_PART_RML))) {
        return;
    }
    if (d->reveal_cursor >= 0) {
        kit_code_set_cursor(e, d->reveal_cursor);
        d->reveal_cursor = -1;
    } else if (d->cursor_seen == b->revision && kit_code_cursor(e) != d->cursor) {
        int element = ui_design_element_at(d->rml, kit_code_cursor(e));
        if (element >= 0 && element != d->selected) {
            d->selected = element;
            designer_reveal(d);
        }
    }
    d->cursor = kit_code_cursor(e);
    d->cursor_seen = b->revision;
}

/* ---------- Shown elements ---------- */

static int written_line(const rml_element *el, const char *asset)
{
    char src[192];
    return rml_element_attribute(el, "data-src", src, sizeof src)
             ? ui_design_source_line(src, asset)
             : 0;
}

typedef struct
{
    int line, count;
} line_count;

/* How many children of one parent on `line` came before, counting this one. */
static int count_on(line_count **counts, int line)
{
    for (int i = 0; i < acount(*counts); i++) {
        if ((*counts)[i].line == line) {
            return (*counts)[i].count++;
        }
    }
    apush(*counts, ((line_count){ line, 1 }));
    return 0;
}

typedef struct
{
    const ui_source *rml;
    const char *asset;
    const rml_element *element; /* whose source to find, or NULL */
    int source;                 /* whose element to find, or -1 */
    rml_element *found;
    int found_source;
} shown_walk;

/* Walks the shown elements below `el` whose nearest ancestor the asset
 * wrote is element `parent` (-1 for none): an element is the nth child of
 * its parent on its line, as the markup has them. Elements of other assets,
 * such as a widget's, pass through to the use's children in its slot. */
static bool walk(shown_walk *w, rml_element *el, int parent, line_count **counts)
{
    int count = rml_element_child_count(el);
    for (int i = 0; i < count; i++) {
        rml_element *child = rml_element_child(el, i);
        int line = written_line(child, w->asset);
        if (!line) {
            if (walk(w, child, parent, counts)) {
                return true;
            }
            continue;
        }
        int source = ui_design_child_on_line(w->rml, parent, line, count_on(counts, line));
        if (source >= 0 && (child == w->element || source == w->source)) {
            w->found = child;
            w->found_source = source;
            return true;
        }
        line_count *inner = NULL;
        bool found = source >= 0 && walk(w, child, source, &inner);
        afree(inner);
        if (found) {
            return true;
        }
    }
    return false;
}

static void walk_documents(rml_context *context, shown_walk *w)
{
    for (int i = 0; i < rml_context_document_count(context) && !w->found; i++) {
        line_count *counts = NULL;
        walk(w, rml_context_document(context, i), -1, &counts);
        afree(counts);
    }
}

int studio_designer_shown_source(
    rml_context *context,
    const ui_source *rml,
    const char *asset,
    rml_element *element
)
{
    while (element && !written_line(element, asset)) {
        element = rml_element_parent(element);
    }
    shown_walk w = { .rml = rml,
                     .asset = asset,
                     .element = element,
                     .source = -1,
                     .found_source = -1 };
    if (element) {
        walk_documents(context, &w);
    }
    return w.found_source;
}

rml_element *studio_designer_shown(rml_context *context, const ui_source *rml, const char *asset, int source)
{
    shown_walk w = { .rml = rml, .asset = asset, .source = source, .found_source = -1 };
    if (source >= 0) {
        walk_documents(context, &w);
    }
    return w.found;
}

/* ---------- Canvas ---------- */

/* How the Canvas's pixels map to the kit's: the view zoom crops the frame
 * shown in the Scene's surface, `clip`. */
typedef struct
{
    float x, y, sx, sy;
    kit_rect clip;
} canvas_map;

static bool scene_shown(const studio *s)
{
    const studio_layout *layout = &s->prefs.layouts[s->prefs.workspace];
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        if (layout->editors[i] == STUDIO_EDITOR_SCENE && s->areas[i].rect.w > 0) {
            return true;
        }
    }
    return false;
}

static bool shows_document(const studio *s, const studio_designer *d)
{
    const char *shown = studio_previewed_ui(s);
    return shown && !strcmp(shown, d->asset) && scene_shown(s);
}

/* The Canvas's documents while they show the document as it is. */
static rml_context *canvas(studio *s, studio_designer *d, const ui_buffer *b, canvas_map *m)
{
    engine_viewport *viewport = shows_document(s, d) ? s->canvas->viewport : NULL;
    render_ui *ui = viewport ? engine_viewport_ui(viewport) : NULL;
    kit_rect r = s->surface.rect;
    int w = 0, h = 0;
    if (ui) {
        engine_viewport_pixel_size(viewport, &w, &h);
    }
    if (!ui || r.w <= 0 || r.h <= 0 || w <= 0 || h <= 0 || !ui_buffer_shown(b)) {
        return NULL;
    }
    float zoom = s->view.zoom;
    *m = (canvas_map){ .x = r.x + (0.5f - s->view.x * zoom) * r.w,
                       .y = r.y + (0.5f - s->view.y * zoom) * r.h,
                       .sx = r.w * zoom / (float)w,
                       .sy = r.h * zoom / (float)h,
                       .clip = r };
    return render_ui_context(ui);
}

static bool contains(rml_element *el, rml_vec2 p)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    return p.x >= at.x && p.y >= at.y && p.x < at.x + size.x && p.y < at.y + size.y;
}

/* Expanded widgets keep a source-marked data-ui wrapper. Its inline box can
   have no width while its rendered roots carry the widget's actual size. */
kit_rect studio_designer_element_box(rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    kit_rect box = { at.x, at.y, size.x, size.y };
    char widget[128];
    if (!rml_element_attribute(el, "data-ui", widget, sizeof widget)) {
        return box;
    }
    bool bounds = box.w > 0 && box.h > 0;
    for (int i = 0; i < rml_element_child_count(el); i++) {
        rml_element *child = rml_element_child(el, i);
        if (!rml_element_is_visible(child, true)) {
            continue;
        }
        kit_rect root = studio_designer_element_box(child);
        if (root.w <= 0 || root.h <= 0) {
            continue;
        }
        if (!bounds) {
            box = root;
            bounds = true;
            continue;
        }
        float x = fminf(box.x, root.x), y = fminf(box.y, root.y);
        float right = fmaxf(box.x + box.w, root.x + root.w);
        float bottom = fmaxf(box.y + box.h, root.y + root.h);
        box = (kit_rect){ x, y, right - x, bottom - y };
    }
    return box;
}

/* The last, innermost shown element at or below `el` holding `p`, text and
 * pointer-events: none included, as the Inspector picks. */
static rml_element *deepest(rml_element *el, rml_vec2 p)
{
    rml_element *found = NULL;
    int count = rml_element_child_count(el);
    for (int i = 0; i < count; i++) {
        rml_element *child = rml_element_child(el, i);
        if (strcmp(rml_element_tag_name(child), "#text") &&
            rml_element_is_visible(child, false)) {
            rml_element *inner = deepest(child, p);
            found = inner ? inner : found;
        }
    }
    return found ? found : contains(el, p) ? el : NULL;
}

/* The document's element under the pointer, from the topmost document. */
static int source_under(const studio *s, const studio_designer *d, rml_context *context, canvas_map m)
{
    rml_vec2 p = { (s->in.mouse_x - m.x) / m.sx, (s->in.mouse_y - m.y) / m.sy };
    for (int i = rml_context_document_count(context) - 1; i >= 0; i--) {
        rml_element *el = deepest(rml_context_document(context, i), p);
        while (el && !written_line(el, d->asset)) {
            el = rml_element_parent(el);
        }
        if (el) {
            return studio_designer_shown_source(context, d->rml, d->asset, el);
        }
    }
    return -1;
}

static bool pointer_in(const studio *s, kit_rect r)
{
    float x = s->in.mouse_x, y = s->in.mouse_y;
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void place(kit *k, kit_rect r)
{
    char value[32];
    snprintf(value, sizeof value, "%.2fdp", r.x);
    kit_style(k, "left", value);
    snprintf(value, sizeof value, "%.2fdp", r.y);
    kit_style(k, "top", value);
    snprintf(value, sizeof value, "%.2fdp", r.w);
    kit_style(k, "width", value);
    snprintf(value, sizeof value, "%.2fdp", r.h);
    kit_style(k, "height", value);
}

/* An element's border box outlined, labeled with its name and size in dp
 * for the selection, as UMG labels its selected widget. */
static void outline(
    studio *s,
    const studio_designer *d,
    rml_context *context,
    canvas_map m,
    int element,
    const char *cls
)
{
    rml_element *el = studio_designer_shown(context, d->rml, d->asset, element);
    if (!el || !rml_element_is_visible(el, true)) {
        return;
    }
    kit_rect bounds = studio_designer_element_box(el);
    kit_rect box = { m.x + bounds.x * m.sx,
                     m.y + bounds.y * m.sy,
                     bounds.w * m.sx,
                     bounds.h * m.sy };
    char name[96], label[160] = "";
    if (!strcmp(cls, "selected")) {
        float ratio = rml_context_dp_ratio(context);
        designer_element_name(d->rml, element, name, sizeof name);
        snprintf(
            label,
            sizeof label,
            "%s  %g \xc3\x97 %g",
            name,
            roundf(bounds.w / ratio),
            roundf(bounds.h / ratio)
        );
    }
    kit_highlight_desc desc = { .margin = box,
                                .border = box,
                                .padding = box,
                                .content = box,
                                .clip = m.clip,
                                .label = label };
    kit_highlight(s->kit, cls, &desc, cls);
}

/* Where a drop on the Canvas lands (workstream Q), as a Hierarchy row
 * splits: along the flow of the element's parent, its edges go before and
 * after it and its middle inside a container; on nothing, into the
 * document. */
typedef struct
{
    int target;
    ui_source_place place;
    kit_rect box;
    bool across; /* its siblings run across, as in a flex row */
} canvas_target;

/* Whether siblings lay out across: a flex row's items, or inline boxes in
 * a block. */
static bool flows_across(rml_element *el)
{
    rml_element *parent = rml_element_parent(el);
    char display[32] = "", direction[32] = "";
    if (parent) {
        rml_element_property(parent, "display", display, sizeof display);
        rml_element_property(parent, "flex-direction", direction, sizeof direction);
    }
    if (!strcmp(display, "flex") || !strcmp(display, "inline-flex")) {
        return !strncmp(direction, "row", 3);
    }
    rml_element_property(el, "display", display, sizeof display);
    return !strncmp(display, "inline", 6);
}

static canvas_target canvas_target_at(
    const studio *s,
    const studio_designer *d,
    rml_context *context,
    canvas_map m
)
{
    canvas_target t = { .target = source_under(s, d, context, m),
                        .place = UI_SOURCE_INSIDE };
    rml_element *el = t.target >= 0
                        ? studio_designer_shown(context, d->rml, d->asset, t.target)
                        : NULL;
    if (!el) {
        return t;
    }
    kit_rect bounds = studio_designer_element_box(el);
    t.box = (kit_rect){ m.x + bounds.x * m.sx,
                        m.y + bounds.y * m.sy,
                        bounds.w * m.sx,
                        bounds.h * m.sy };
    t.across = flows_across(el);
    float u = t.across ? (s->in.mouse_x - t.box.x) / fmaxf(t.box.w, 1)
                       : (s->in.mouse_y - t.box.y) / fmaxf(t.box.h, 1);
    bool container = ui_design_container(d->rml, t.target);
    t.place = u < DROP_EDGE || (!container && u < 0.5f) ? UI_SOURCE_BEFORE
            : u > 1 - DROP_EDGE || !container           ? UI_SOURCE_AFTER
                                                        : UI_SOURCE_INSIDE;
    return t;
}

/* The line between siblings where a drop before or after lands, across
 * the element's edge, kept within the Canvas. */
static void drop_line(kit *k, const canvas_target *t, kit_rect clip)
{
    enum
    {
        THICK = 2,
    };
    kit_rect r = t->box;
    bool after = t->place == UI_SOURCE_AFTER;
    if (t->across) {
        r = (kit_rect){ r.x + (after ? r.w : 0) - THICK / 2.0f, r.y, THICK, r.h };
    } else {
        r = (kit_rect){ r.x, r.y + (after ? r.h : 0) - THICK / 2.0f, r.w, THICK };
    }
    float x = fmaxf(r.x, clip.x), y = fmaxf(r.y, clip.y);
    float right = fminf(r.x + r.w, clip.x + clip.w),
          bottom = fminf(r.y + r.h, clip.y + clip.h);
    kit_leaf(k, "drop-line", NULL, "designer-drop-line");
    place(k, (kit_rect){ x, y, fmaxf(right - x, 0), fmaxf(bottom - y, 0) });
}

/* ---------- Resize handles ---------- */

/* A handle's side, in kit dp; an edge's handle shows once the edge is three
   handles long. */
#define HANDLE 8.0f

static const struct
{
    const char *key, *cls;
    unsigned axes;
    float u, v; /* where on the box, as fractions of its width and height */
} HANDLES[] = {
    { "resize-e", "designer-handle e", DESIGNER_RESIZE_WIDTH, 1, 0.5f },
    { "resize-s", "designer-handle s", DESIGNER_RESIZE_HEIGHT, 0.5f, 1 },
    { "resize-se", "designer-handle se", DESIGNER_RESIZE_WIDTH | DESIGNER_RESIZE_HEIGHT, 1, 1 },
};

static const char *const DIMENSIONS[] = { "width", "height" };

void designer_resize_css(
    const designer_box *box,
    unsigned axes,
    float dx,
    float dy,
    bool aspect,
    float css[2]
)
{
    float size[2] = { box->size[0] + dx, box->size[1] + dy };
    bool both = axes == (DESIGNER_RESIZE_WIDTH | DESIGNER_RESIZE_HEIGHT);
    if (aspect && both && box->size[0] > 0 && box->size[1] > 0) {
        float ratio = box->size[0] / box->size[1];
        if (fabsf(dx) / box->size[0] >= fabsf(dy) / box->size[1]) {
            size[1] = size[0] / ratio;
        } else {
            size[0] = size[1] * ratio;
        }
    }
    for (int i = 0; i < 2; i++) {
        float border = axes & (1u << i) ? size[i] : box->size[i];
        float content = box->border_box ? fmaxf(border, box->extra[i])
                                        : fmaxf(border - box->extra[i], 0);
        css[i] = roundf(content);
    }
}

/* The element's inline `property`, as written, in `out`; "" for none. */
static void inline_style(const ui_source *rml, int element, const char *property, char *out, size_t size)
{
    out[0] = '\0';
    char *style = ui_source_attribute_value(rml, element, "style");
    size_t length = strlen(property);
    for (const char *at = style; at && *at;) {
        at += strspn(at, " \t\n;");
        const char *colon = strchr(at, ':'), *end = at + strcspn(at, ";");
        if (colon && colon < end && (size_t)(colon - at) >= length &&
            !strncmp(at, property, length) &&
            strspn(at + length, " \t") == (size_t)(colon - at - length)) {
            const char *value = colon + 1 + strspn(colon + 1, " \t");
            int written = (int)(end - value);
            while (written > 0 && isspace((unsigned char)value[written - 1])) {
                written--;
            }
            snprintf(out, size, "%.*s", written, value);
        }
        at = end;
    }
    free(style);
}

/* The drag runs from the handle's center, wherever on it the press was and
   however far the pointer went in the frame that took it. */
static void resize_begin(
    studio *s,
    studio_designer *d,
    ui_buffer *b,
    rml_element *el,
    rml_context *context,
    kit_rect handle
)
{
    float ratio = rml_context_dp_ratio(context);
    rml_vec2 border = rml_element_box_size(el, RML_BOX_BORDER);
    rml_vec2 content = rml_element_box_size(el, RML_BOX_CONTENT);
    char sizing[32] = "";
    rml_element_property(el, "box-sizing", sizing, sizeof sizing);
    d->resize.active = true;
    d->resize.moved = false;
    d->resize.element = d->selected;
    d->resize.x0 = handle.x + handle.w / 2;
    d->resize.y0 = handle.y + handle.h / 2;
    d->resize.revision = b->revision;
    d->resize.box = (designer_box){ { border.x / ratio, border.y / ratio },
                                    { (border.x - content.x) / ratio,
                                      (border.y - content.y) / ratio },
                                    !strcmp(sizing, "border-box") };
    for (int i = 0; i < 2; i++) {
        inline_style(
            d->rml,
            d->selected,
            DIMENSIONS[i],
            d->resize.before[i],
            sizeof d->resize.before[i]
        );
    }
}

/* Puts back what the drag showed on the element. */
static void resize_restore(studio_designer *d, rml_element *el)
{
    for (int i = 0; el && i < 2; i++) {
        if (!(d->resize.axes & (1u << i))) {
            continue;
        }
        if (d->resize.before[i][0]) {
            rml_element_set_property(el, DIMENSIONS[i], d->resize.before[i]);
        } else {
            rml_element_remove_property(el, DIMENSIONS[i]);
        }
    }
}

/* Release writes the size into the element's inline style, one undo step;
   a press that never moved writes nothing. */
static void resize_commit(studio *s, studio_designer *d, ui_buffer *b, rml_element *el)
{
    d->resize.active = false;
    if (!d->resize.moved) {
        return;
    }
    ui_design_style styles[2];
    char values[2][32];
    int count = 0;
    for (int i = 0; i < 2; i++) {
        if (d->resize.axes & (1u << i)) {
            snprintf(values[count], sizeof values[count], "%gdp", d->resize.css[i]);
            styles[count] = (ui_design_style){ DIMENSIONS[i], values[count] };
            count++;
        }
    }
    char error[192] = "";
    if (!ui_design_set_styles(b, d->resize.element, styles, count, error, sizeof error)) {
        resize_restore(d, el);
        designer_warn(s, "Could not resize", error);
    }
}

/* While the button is held the Canvas shows the new size, and the
   selection's label reads it; Shift keeps the aspect at the corner. */
static void resize_frame(studio *s, studio_designer *d, ui_buffer *b, rml_context *context, canvas_map m)
{
    if (!d->resize.active) {
        return;
    }
    rml_element *el = studio_designer_shown(context, d->rml, d->asset, d->resize.element);
    if (!el || b->revision != d->resize.revision) {
        resize_restore(d, el);
        d->resize.active = false;
        return;
    }
    float ratio = rml_context_dp_ratio(context);
    float dx = (s->in.mouse_x - d->resize.x0) / (m.sx * ratio);
    float dy = (s->in.mouse_y - d->resize.y0) / (m.sy * ratio);
    /* A press anywhere on the handle that never left it changes nothing. */
    d->resize.moved |= fabsf(s->in.mouse_x - d->resize.x0) > HANDLE / 2 + 1 ||
                       fabsf(s->in.mouse_y - d->resize.y0) > HANDLE / 2 + 1;
    if (d->resize.moved) {
        bool aspect = (s->mods & SDL_KMOD_SHIFT) != 0;
        designer_resize_css(
            &d->resize.box,
            d->resize.axes,
            dx,
            dy,
            aspect,
            d->resize.css
        );
        for (int i = 0; i < 2; i++) {
            char value[32];
            snprintf(value, sizeof value, "%gdp", d->resize.css[i]);
            if (d->resize.axes & (1u << i)) {
                rml_element_set_property(el, DIMENSIONS[i], value);
            }
        }
    }
    if (!s->in.down[0]) {
        resize_commit(s, d, b, el);
    }
}

static bool inside(kit_rect outer, kit_rect r)
{
    return r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w &&
           r.y + r.h <= outer.y + outer.h;
}

/* The selection's handles on its right and bottom edges and corner: only
   its size changes, so wherever its layout puts it, it stays. A widget's use
   has none, its widget deciding its size. */
static void resize_handles(studio *s, studio_designer *d, ui_buffer *b, rml_context *context, canvas_map m)
{
    kit *k = s->kit;
    rml_element *el = studio_designer_shown(context, d->rml, d->asset, d->selected);
    if (!el || !rml_element_is_visible(el, true) ||
        rml_element_has_attribute(el, "data-ui")) {
        return;
    }
    kit_rect bounds = studio_designer_element_box(el);
    kit_rect box = { m.x + bounds.x * m.sx,
                     m.y + bounds.y * m.sy,
                     bounds.w * m.sx,
                     bounds.h * m.sy };
    for (size_t i = 0; i < sizeof HANDLES / sizeof *HANDLES; i++) {
        kit_rect r = { box.x + box.w * HANDLES[i].u - HANDLE / 2,
                       box.y + box.h * HANDLES[i].v - HANDLE / 2,
                       HANDLE,
                       HANDLE };
        float edge = HANDLES[i].axes == DESIGNER_RESIZE_WIDTH ? box.h : box.w;
        bool corner = HANDLES[i].u == 1 && HANDLES[i].v == 1;
        if (!inside(m.clip, r) || (!corner && edge < 3 * HANDLE)) {
            continue;
        }
        kit_leaf(k, HANDLES[i].key, NULL, HANDLES[i].cls);
        place(k, r);
        kit_flag(
            k,
            "data-active",
            d->resize.active && d->resize.axes == HANDLES[i].axes
        );
        if (kit_item_pressed(k) && !d->resize.active) {
            d->resize.axes = HANDLES[i].axes;
            resize_begin(s, d, b, el, context, r);
        }
    }
}

static void resize_cancel(studio *s, studio_designer *d, ui_buffer *b)
{
    canvas_map m;
    rml_context *context = canvas(s, d, b, &m);
    resize_restore(
        d,
        context ? studio_designer_shown(context, d->rml, d->asset, d->resize.element) : NULL
    );
    d->resize.active = false;
    studio_report(s, "Resize canceled");
}

bool studio_designer_resizing(const studio *s)
{
    return s->designer && s->designer->resize.active;
}

/* Outlines the selection and the element under the pointer or its row,
 * selects with a click, and takes drops through a zone over the frame that
 * only takes the pointer while a Hierarchy row or a Library item drags.
 * Over the Canvas a drag marks where it lands: the element it goes into,
 * or the line between siblings it goes between. */
static void canvas_frame(studio *s, studio_designer *d, ui_buffer *b)
{
    canvas_map m;
    rml_context *context = canvas(s, d, b, &m);
    if (!context) {
        d->resize.active = false;
        return;
    }
    kit *k = s->kit;
    resize_frame(s, d, b, context, m);
    bool dragging = kit_dragging(k, DESIGNER_DRAG);
    bool over = dragging && pointer_in(s, m.clip) && d->hovered < 0;
    bool pointed = s->surface.hovered || over;
    canvas_target t = over
                        ? canvas_target_at(s, d, context, m)
                        : (canvas_target){
                              .target = pointed ? source_under(s, d, context, m) : -1
                          };
    kit_guides_begin(k, "designer");
    kit_leaf(k, "drop", NULL, "designer-drop");
    place(k, m.clip);
    kit_flag(k, "data-active", dragging);
    char payload[512];
    bool dropped = kit_drop_target(k, DESIGNER_DRAG, payload, sizeof payload);
    int hover = d->hovered >= 0 ? d->hovered : t.target;
    bool between = over && t.target >= 0 && t.place != UI_SOURCE_INSIDE;
    if (hover >= 0 && hover != d->selected) {
        outline(s, d, context, m, hover, dragging && !between ? "designer-target" : "designer-hover");
    }
    if (between) {
        drop_line(k, &t, m.clip);
    }
    if (d->selected >= 0) {
        outline(s, d, context, m, d->selected, "selected");
        resize_handles(s, d, b, context, m);
    }
    kit_guides_end(k);
    /* A press selects, unless it took a resize handle. */
    if (s->surface.hovered && s->surface.pressed[0] && !d->resize.active) {
        designer_select(s, t.target);
    }
    if (dropped) {
        t = canvas_target_at(s, d, context, m);
        drop(s, d, b, payload, t.target, t.place);
    }
}

/* ---------- Frame and keys ---------- */

void studio_designer_frame(studio *s)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    if (!d) {
        if (s->designer) {
            s->designer->resize.active = false;
        }
        return;
    }
    undo_delete(s, d, b);
    designer_keep_carry(s, d, b);
    follow_cursor(s, d, b);
    d->hovered = designer_hovered_row(s);
    canvas_frame(s, d, b);
}

uint64_t studio_designer_stamp(studio *s)
{
    ui_buffer *b = studio_ui_buffer(s);
    if (!s->designer || !b || !ui_buffer_text(b, UI_PART_RML)) {
        return 0;
    }
    const studio_designer *d = s->designer;
    return ((uint64_t)b->revision << 32) ^ (uint64_t)(d->selected + 1) ^
           ((uint64_t)d->folds << 16);
}

bool studio_designer_key(studio *s, studio_editor editor, SDL_Keycode key, SDL_Keymod mod)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    /* A resize owns the keys while it runs: Escape cancels it. */
    if (d && d->resize.active) {
        if (key == SDLK_ESCAPE) {
            resize_cancel(s, d, b);
        }
        return true;
    }
    bool hierarchy = editor == STUDIO_EDITOR_OUTLINER;
    if (!d ||
        (!hierarchy && !(editor == STUDIO_EDITOR_SCENE && shows_document(s, d))) ||
        (mod & SDL_KMOD_ALT)) {
        return false;
    }
    /* Mod+D duplicates, as for objects. */
    if (studio_primary(s, mod)) {
        if (key == SDLK_D && !d->carry.active) {
            designer_duplicate(s);
        }
        return key == SDLK_D;
    }
    if (hierarchy && d->carry.active) {
        designer_carry_key(s, d, b, key);
        return true;
    }
    if (hierarchy && !(mod & SDL_KMOD_SHIFT) &&
        (key == SDLK_SPACE || key == SDLK_RETURN)) {
        designer_pick_up(s, d, b);
        return true;
    }
    if (mod & SDL_KMOD_SHIFT) {
        if (key == SDLK_D) {
            designer_duplicate(s);
        }
        return key == SDLK_D;
    }
    switch (key) {
        case SDLK_X:
        case SDLK_DELETE:
        case SDLK_BACKSPACE: designer_delete(s); return true;
        case SDLK_UP:
        case SDLK_DOWN:
            if (hierarchy) {
                designer_step(s, d, key == SDLK_DOWN ? 1 : -1);
            }
            return hierarchy;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            if (hierarchy) {
                designer_fold_selected(d, key == SDLK_LEFT);
            }
            return hierarchy;
        default: return false;
    }
}
