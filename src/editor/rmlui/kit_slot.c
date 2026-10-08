/* Authored chrome's kit pieces (kit.h). Native slots, as Unity's
 * IMGUIContainer: kit content inside the authored content of a kit_host. A
 * slot adopts the element a selector finds in the host and emits into it as
 * into a root.
 *
 * Hosted content is rebuilt behind the kit's back: a mount reinstalls its
 * markup, data-for recreates its items, and RmlUi may give a freed element's
 * address to a new one. So the kit marks what it adopts with data-kit-slot
 * and finds it again before each frame's input and emission. A slot whose
 * element is gone loses its nodes without touching their elements, which
 * went with it, and the next kit_slot_begin builds them anew. */
#include "editor/rmlui/kit_internal.h"

#include <assert.h>
#include <string.h>

#define SLOT_MARK "data-kit-slot"

static int slot_index(const kit *k, const char *path)
{
    for (int i = 0; i < acount(k->slots); i++) {
        if (str_eq(k->slots[i]->path, path)) {
            return i;
        }
    }
    return -1;
}

/* The element the slot adopted, while it is still in its host; NULL once the
 * host or the element is gone. Only the adopted element carries its mark. */
static rml_element *adopted(kit *k, const node *slot)
{
    int host_length = (int)(strlen(slot->path) - strlen(slot->key) - 1);
    node *host = path_node(k, frame_format(k, "%.*s", host_length, slot->path));
    if (!host || !host->hosted) {
        return NULL;
    }
    const char *mark = frame_format(k, "[" SLOT_MARK "=\"%s\"]", slot->key);
    rml_element *el = rml_element_query_selector(host->el, mark);
    assert(!el || el == slot->el);
    return el;
}

static bool descends(const node *n, const node *ancestor)
{
    for (; n; n = n->parent) {
        if (n == ancestor) {
            return true;
        }
    }
    return false;
}

/* Frees the slot's exiting nodes, removing their elements only when the
 * adopted element is still there to hold them. */
static void release_exiting(kit *k, const node *slot, bool alive)
{
    int kept = 0;
    for (int i = 0; i < acount(k->exiting); i++) {
        exiting e = k->exiting[i];
        if (!descends(e.node, slot)) {
            k->exiting[kept++] = e;
            continue;
        }
        rml_element *parent = alive ? rml_element_parent(e.node->el) : NULL;
        if (parent) {
            rml_element_remove_child(parent, e.node->el);
        }
        node_free(e.node);
    }
    asetlen(k->exiting, kept);
}

/* Drops slot `i`. Its content leaves the adopted element when that is still
 * in its host; otherwise it went with the element. */
static void drop(kit *k, int i)
{
    node *slot = k->slots[i];
    rml_element *el = adopted(k, slot);
    release_exiting(k, slot, el != NULL);
    unmap(k, slot);
    if (el) {
        for (int c = 0; c < acount(slot->children); c++) {
            rml_element_remove_child(el, slot->children[c]->el);
        }
        rml_element_remove_attribute(el, SLOT_MARK);
    }
    node_free(slot);
    int count = acount(k->slots);
    memmove(&k->slots[i], &k->slots[i + 1], (size_t)(count - 1 - i) * sizeof *k->slots);
    asetlen(k->slots, count - 1);
}

static node *adopt(kit *k, const node *host, const char *key, rml_element *el)
{
    assert(!rml_element_has_attribute(el, SLOT_MARK));
    rml_element_set_attribute(el, SLOT_MARK, key);
    node *n = node_new(KIND_ROOT);
    n->el = n->container = el;
    str_set(&n->key, key);
    str_catf(&n->path, "%s/%s", host->path, key);
    n->created = k->frame;
    /* By path only: the element itself stays the host's content. */
    assert(!path_node(k, n->path));
    cf_map_set(k->by_path, str_hash(n->path), n);
    apush(k->slots, n);
    return n;
}

bool kit_slot_begin(kit *k, const char *key, rml_element *host, const char *selector)
{
    node *h = node_for(k, host);
    assert(h && h->el == host && h->hosted && key[0]);
    rml_element *el = rml_element_query_selector(host, selector);
    int i = slot_index(k, frame_format(k, "%s/%s", h->path, key));
    if (i >= 0 &&
        (k->slots[i]->el != el || !rml_element_has_attribute(el, SLOT_MARK))) {
        drop(k, i);
        i = -1;
    }
    if (!el) {
        return false;
    }
    node *n = i >= 0 ? k->slots[i] : adopt(k, h, key, el);
    n->frame = k->frame;
    push_node(k, n);
    return true;
}

void kit_slot_end(kit *k)
{
    assert(nodes_index(k->slots, alast(k->stack)) >= 0);
    pop_node(k);
}

/* Outer slots come first, so a slot inside another's content is checked
 * only while its host still stands. */
void check_slots(kit *k)
{
    for (int i = 0; i < acount(k->slots);) {
        if (adopted(k, k->slots[i])) {
            i++;
            continue;
        }
        drop(k, i);
    }
}

void end_slots(kit *k)
{
    for (int i = 0; i < acount(k->slots);) {
        if (k->slots[i]->frame == k->frame) {
            i++;
            continue;
        }
        drop(k, i);
    }
}

void free_slots(kit *k)
{
    for (int i = 0; i < acount(k->slots); i++) {
        node_free(k->slots[i]);
    }
    afree(k->slots);
}

/* ---------- Chrome hosts ---------- */

/* Kit content in the host's slots keeps focus, as a field there takes
 * typing; only authored content's nearest node is the host. */
void kit_release_focus(kit *k, rml_element *host)
{
    rml_element *focus = rml_context_focus_element(k->context);
    node *n = focus ? node_for(k, focus) : NULL;
    if (n && n->el == host) {
        rml_element_focus(rml_element_parent(host), false);
    }
}
