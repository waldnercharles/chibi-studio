/* The UI Inspector's kit pieces (kit.h): the overlay that picks, DevTools'
 * box-model highlight and readable element paths. The Inspector itself is
 * studio_inspect.c. */
#include "editor/rmlui/kit_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Steps shown below the nearest emitted ancestor before the path elides. */
#define PATH_STEPS 6
/* The label's height and its gap from the box, in dp. */
#define LABEL_HEIGHT 18.0f
#define LABEL_GAP 2.0f

rml_element *kit_inspect_begin(kit *k, const char *key, bool picking)
{
    node *n = emit_in(k, k->layer, key, KIND_BOX, "div", "inspect-overlay");
    set_flag(n, "data-picking", picking);
    push_node(k, n);
    return n->el;
}

void kit_inspect_end(kit *k)
{
    pop_node(k);
}

void kit_guides_begin(kit *k, const char *key)
{
    push_node(k, emit_in(k, k->layer, key, KIND_BOX, "div", "inspect-overlay guides"));
}

void kit_guides_end(kit *k)
{
    pop_node(k);
}

static const char *length(kit *k, float value)
{
    return frame_format(k, "%.2fdp", fmaxf(value, 0));
}

/* The last item placed at `r`, relative to `origin`. */
static void place(kit *k, kit_rect r, kit_rect origin)
{
    kit_style(k, "left", frame_format(k, "%.2fdp", r.x - origin.x));
    kit_style(k, "top", frame_format(k, "%.2fdp", r.y - origin.y));
    kit_style(k, "width", length(k, r.w));
    kit_style(k, "height", length(k, r.h));
}

/* A band filling `outer` less `inner`, drawn as the border of a box. */
static void band(kit *k, const char *key, kit_rect outer, kit_rect inner, kit_rect origin)
{
    emit(k, key, KIND_BOX, "div", frame_format(k, "band %s", key));
    place(k, outer, origin);
    kit_style(k, "border-top-width", length(k, inner.y - outer.y));
    kit_style(
        k,
        "border-right-width",
        length(k, outer.x + outer.w - inner.x - inner.w)
    );
    kit_style(
        k,
        "border-bottom-width",
        length(k, outer.y + outer.h - inner.y - inner.h)
    );
    kit_style(k, "border-left-width", length(k, inner.x - outer.x));
}

/* The label sits above the border box, or below it where the clip ends. */
static void label(kit *k, const kit_highlight_desc *d, kit_rect origin)
{
    kit_rect b = d->border;
    float above = b.y - LABEL_HEIGHT - LABEL_GAP;
    float top = above >= origin.y ? above : b.y + b.h + LABEL_GAP;
    kit_label(k, "label", "label", d->label);
    kit_style(k, "left", frame_format(k, "%.2fdp", fmaxf(b.x - origin.x, 0)));
    kit_style(k, "top", frame_format(k, "%.2fdp", top - origin.y));
}

void kit_highlight(kit *k, const char *key, const kit_highlight_desc *d, const char *cls)
{
    bool clipped = d->clip.w > 0 && d->clip.h > 0;
    kit_rect origin = clipped ? d->clip
                              : (kit_rect){ 0, 0, k->in.width, k->in.height };
    push_node(k, emit(k, key, KIND_BOX, "div", KIT_CX(k, "inspect-highlight", { cls, true })));
    place(k, origin, (kit_rect){ 0 });
    band(k, "margin", d->margin, d->border, origin);
    band(k, "border", d->border, d->padding, origin);
    band(k, "padding", d->padding, d->content, origin);
    emit(k, "content", KIND_BOX, "div", "band content");
    place(k, d->content, origin);
    if (d->label && d->label[0]) {
        label(k, d, origin);
    }
    pop_node(k);
}

/* Appends tag#id.class, as a selector names the element. */
static void step(char **out, rml_element *el)
{
    str_cat(out, rml_element_tag_name(el));
    const char *id = rml_element_id(el);
    if (id[0]) {
        str_catf(out, "#%s", id);
    }
    char classes[256];
    rml_element_class_names(el, classes, sizeof classes);
    for (char *c = classes; *c;) {
        size_t n = strcspn(c, " ");
        if (n) {
            str_catf(out, ".%.*s", (int)n, c);
        }
        c += n + (c[n] == ' ');
    }
}

void kit_element_selector(rml_element *el, char *out, size_t size)
{
    char *name = NULL;
    step(&name, el);
    snprintf(out, size, "%s", name);
    free(name);
}

bool kit_element_path(const kit *k, rml_element *el, char *out, size_t size)
{
    node *n = node_for(k, el);
    rml_element *document = rml_element_owner_document(el);
    rml_element *steps[PATH_STEPS];
    int count = 0;
    bool elided = false;
    for (rml_element *e = el; e && e != document && (!n || e != n->el);
         e = rml_element_parent(e)) {
        elided = count == PATH_STEPS;
        if (elided) {
            break;
        }
        steps[count++] = e;
    }
    char *path = NULL;
    str_cat(&path, n ? n->path : "");
    if (elided) {
        str_cat(&path, path[0] ? " \xe2\x80\xba \xe2\x80\xa6" : "\xe2\x80\xa6");
    }
    for (int i = count - 1; i >= 0; i--) {
        if (path[0]) {
            str_cat(&path, " \xe2\x80\xba ");
        }
        step(&path, steps[i]);
    }
    if (!path[0]) {
        step(&path, el);
    }
    snprintf(out, size, "%s", path);
    free(path);
    bool hosted = n && n->hosted && el != n->el;
    return owns_document(k, document) && !hosted;
}
