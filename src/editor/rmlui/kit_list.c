/* Virtualized lists (Virtuoso): a kit node over ui_list_window (ui/list.h),
 * whose window of rows the caller emits into the list's content. */
#include "editor/rmlui/kit_internal.h"

#include <assert.h>

bool kit_list_begin(kit *k, const char *key, const char *cls, const kit_list_desc *d, int *first, int *last)
{
    const kit_class list[] = { { cls, cls != NULL } };
    node *n = emit(k, key, KIND_LIST, "div", kit_classes(k, "vlist", list, 1));
    widget *w = &n->w;
    if (!w->body) {
        w->body = raw(n->el, "div", "vlist-content", NULL);
        n->container = w->body;
        w->list = (ui_list_state)UI_LIST_STATE_INIT;
    }
    ui_list window = { .count = d->count,
                       .row = d->row_height,
                       .reveal = d->reveal,
                       .overscan = d->overscan,
                       .follow = d->follow };
    int length = ui_list_window(n->el, &window, &w->list, first);
    *last = *first + length - 1;
    push_node(k, n);
    return length > 0;
}

void kit_list_end(kit *k)
{
    assert(alast(k->stack)->kind == KIND_LIST);
    pop_node(k);
}
