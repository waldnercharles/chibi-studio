/* Drag and drop for kit nodes over the one drag every document shares
 * (ui/drag.h): a source and a target are the attributes drag.c reads,
 * set on the node each frame it is emitted as one, and a drop is taken in
 * the frame after its release. */
#include "editor/rmlui/kit_internal.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void drop_types_clear(node *n)
{
    for (int i = 0; i < acount(n->drop_types); i++) {
        free(n->drop_types[i]);
    }
    afree(n->drop_types);
    n->drop_types = NULL;
}

static void set_or_remove(node *n, const char *name, const char *value)
{
    if (value) {
        set_attr(n, name, value);
        return;
    }
    set_flag(n, name, false);
}

/* What the node was emitted as this frame: a source, a target of the types
 * it named, or neither. */
void drag_flush(node *n)
{
    char *types = NULL;
    for (int i = 0; i < acount(n->drop_types); i++) {
        str_catf(&types, "%s%s", i ? " " : "", n->drop_types[i]);
    }
    set_or_remove(n, "data-drop", types);
    free(types);
    if (!n->drag_source) {
        static const char *const SOURCE[] = { "data-drag",
                                              "data-drag-payload",
                                              "data-drag-label",
                                              "data-drag-icon" };
        for (size_t i = 0; i < sizeof SOURCE / sizeof *SOURCE; i++) {
            set_flag(n, SOURCE[i], false);
        }
    }
}

void drag_frame(kit *k)
{
    bool dragging = ui_drag_active(k->context, NULL);
    /* A release the context missed, outside every element. */
    if (dragging && !k->in.down[0]) {
        ui_drag_cancel(k->context);
        dragging = false;
    }
    if (dragging && !k->dragging) {
        close_all_popups(k);
    }
    k->dragging = dragging;
}

/* ---------- API ---------- */

void kit_drag_source(kit *k, const kit_drag_desc *desc)
{
    node *n = k->last;
    assert(n && desc->type);
    n->drag_source = true;
    set_attr(n, "data-drag", desc->type);
    set_attr(n, "data-drag-payload", desc->payload ? desc->payload : "");
    set_or_remove(n, "data-drag-label", desc->label);
    char icon[5] = "";
    if (desc->icon) {
        utf8(icon, desc->icon);
    }
    set_attr(n, "data-drag-icon", icon);
}

bool kit_drop_target(kit *k, const char *type, char *payload, size_t size)
{
    node *n = k->last;
    assert(n && type);
    apush(n->drop_types, xstrdup(type));
    ui_drag_item item;
    if (!ui_drag_dropped(n->el, type, &item)) {
        return false;
    }
    snprintf(payload, size, "%s", item.payload);
    return true;
}

bool kit_drop_hover(kit *k, char *type, size_t type_size, char *payload, size_t size)
{
    ui_drag_item item;
    if (!k->last || !ui_drag_over(k->last->el, &item)) {
        return false;
    }
    snprintf(type, type_size, "%s", item.type);
    snprintf(payload, size, "%s", item.payload);
    return true;
}

void kit_drag_hint(kit *k, const char *text)
{
    ui_drag_hint(k->context, text);
}

bool kit_dragging(const kit *k, const char *type)
{
    ui_drag_item item;
    return ui_drag_active(k->context, &item) && (!type || str_eq(item.type, type));
}
