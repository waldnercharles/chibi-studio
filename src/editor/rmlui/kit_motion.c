/* Motion for the Studio kit, after Motion (motion.dev) and Base UI:
 *
 * - Presence: a node marked with kit_presence mounts with data-starting-style
 *   and, when no longer emitted, stays with data-ending-style until the
 *   transitions RCSS declares for it finish. Styling stays in RCSS.
 * - Layout: a node marked with kit_layout animates changes to its position
 *   within its parent with a spring (FLIP), keeping velocity when retargeted.
 *   Reduced motion settles every animation immediately. */
#include "editor/rmlui/kit_internal.h"
#include "support/math.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Presence ---------- */

void release_exiting_within(kit *k, rml_element *ancestor)
{
    int kept = 0;
    for (int i = 0; i < acount(k->exiting); i++) {
        exiting e = k->exiting[i];
        if (within(e.node->el, ancestor)) {
            node_free(e.node);
            continue;
        }
        k->exiting[kept++] = e;
    }
    asetlen(k->exiting, kept);
}

static void remove_element(kit *k, rml_element *el)
{
    release_exiting_within(k, el);
    rml_element *parent = rml_element_parent(el);
    if (parent) {
        rml_element_remove_child(parent, el);
    }
}

void retire(kit *k, node *n)
{
    unmap(k, n);
    rml_element *el = n->el;
    double duration = n->presence && !k->reduced_motion ? motion_transition_time(el)
                                                        : 0;
    if (duration <= 0 || !rml_element_parent(el)) {
        remove_element(k, el);
        node_free(n);
        return;
    }
    rml_element_remove_attribute(el, "data-starting-style");
    rml_element_set_attribute(el, "data-ending-style", "");
    rml_element_set_property(el, "pointer-events", "none");
    apush(k->exiting, (exiting){ n, k->in.time + duration });
}

void presence(kit *k, node *n)
{
    if (n->presence) {
        return;
    }
    n->presence = true;
    if (n->created != k->frame || k->reduced_motion) {
        return;
    }
    rml_element_set_attribute(n->el, "data-starting-style", "");
    apush(k->entering, n);
}

void forget_motion(kit *k, node *n)
{
    if (n->presence) {
        nodes_remove(k->entering, n);
    }
    if (n->layout) {
        nodes_remove(k->layout_nodes, n);
    }
}

/* ---------- Frame ---------- */

/* Starting styles apply for one styled frame, then transition away. */
static void settle_entering(kit *k)
{
    int kept = 0;
    for (int i = 0; i < acount(k->entering); i++) {
        node *n = k->entering[i];
        if (n->created >= k->frame) {
            k->entering[kept++] = n;
            continue;
        }
        rml_element_remove_attribute(n->el, "data-starting-style");
    }
    asetlen(k->entering, kept);
}

static void finish_exits(kit *k, double now)
{
    for (int i = 0; i < acount(k->exiting);) {
        if (k->exiting[i].until > now) {
            i++;
            continue;
        }
        node *n = k->exiting[i].node;
        int count = acount(k->exiting);
        memmove(
            &k->exiting[i],
            &k->exiting[i + 1],
            (size_t)(count - 1 - i) * sizeof *k->exiting
        );
        asetlen(k->exiting, count - 1);
        rml_element *el = n->el;
        node_free(n);
        remove_element(k, el);
    }
}

static kit_rect relative_rect(const kit *k, rml_element *el)
{
    rml_vec2 offset = rml_element_relative_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float s = dp(k);
    return (kit_rect){ offset.x / s, offset.y / s, size.x / s, size.y / s };
}

static void apply_offset(node *n)
{
    if (n->layout_x.x == 0 && n->layout_y.x == 0) {
        rml_element_remove_property(n->el, "transform");
        return;
    }
    char buffer[64];
    snprintf(
        buffer,
        sizeof buffer,
        "translate(%.2fdp, %.2fdp)",
        n->layout_x.x,
        n->layout_y.x
    );
    rml_element_set_property(n->el, "transform", buffer);
}

/* Offsets a moved node back to where it was, then lets the spring carry it
 * home. */
static void layout_step(kit *k, node *n, spring_params p, double dt)
{
    kit_rect r = relative_rect(k, n->el);
    if (n->layout_known && !k->reduced_motion) {
        n->layout_x.x += n->layout_rect.x - r.x;
        n->layout_y.x += n->layout_rect.y - r.y;
    }
    n->layout_rect = r;
    n->layout_known = true;
    if (k->reduced_motion) {
        n->layout_x = n->layout_y = (spring){ 0 };
    }
    spring_step(&n->layout_x, p, dt);
    spring_step(&n->layout_y, p, dt);
    apply_offset(n);
}

void motion_frame(kit *k)
{
    double now = k->in.time;
    double dt = math_clamp(now - k->motion_time, 0.0, 0.1);
    k->motion_time = now;
    settle_entering(k);
    finish_exits(k, now);
    if (!acount(k->layout_nodes)) {
        return;
    }
    /* FLIP: measure this frame's layout before it renders, laying out the
       documents that hold animated nodes. */
    rml_element **laid_out = NULL;
    for (int i = 0; i < acount(k->layout_nodes); i++) {
        rml_element *document = rml_element_owner_document(k->layout_nodes[i]->el);
        bool seen = false;
        for (int d = 0; d < acount(laid_out); d++) {
            seen |= laid_out[d] == document;
        }
        if (!seen) {
            apush(laid_out, document);
            rml_document_update(document);
        }
    }
    afree(laid_out);
    spring_params p = spring_for(0.35, 0);
    for (int i = 0; i < acount(k->layout_nodes); i++) {
        layout_step(k, k->layout_nodes[i], p, dt);
    }
}

/* ---------- API ---------- */

void kit_presence(kit *k)
{
    assert(k->last);
    presence(k, k->last);
}

void kit_layout(kit *k)
{
    node *n = k->last;
    assert(n);
    if (n->layout) {
        return;
    }
    n->layout = true;
    apush(k->layout_nodes, n);
}

/* ---------- Idle ---------- */

static bool moving(spring s)
{
    return s.x != 0 || s.v != 0;
}

/* Relative to the last emitted frame. The host carries this deadline forward
 * while simulation keeps running; UI-only motion must not follow its cadence. */
double kit_idle_time(const kit *k)
{
    double delay = rml_context_next_update_delay(k->context);
    double now = k->in.time;
    if (k->restyle || acount(k->entering) || ui_drag_active(k->context, NULL)) {
        return 0;
    }
    for (int i = 0; i < acount(k->exiting); i++) {
        delay = fmin(delay, fmax(0, k->exiting[i].until - now));
    }
    for (int i = 0; i < acount(k->layout_nodes); i++) {
        node *n = k->layout_nodes[i];
        if (moving(n->layout_x) || moving(n->layout_y)) {
            return 0;
        }
    }
    for (int i = 0; i < acount(k->toasts); i++) {
        const toast *t = &k->toasts[i];
        if (!t->placed || t->removing || t->swiping || moving(t->x) ||
            moving(t->y) || moving(t->scale) || moving(t->opacity)) {
            return 0;
        }
        if (!k->toasts_expanded && t->duration >= 0 && t->type != KIT_TOAST_LOADING) {
            /* Toast dt is capped at .1; keep its elapsed countdown accurate. */
            delay = fmin(delay, fmax(0, fmin(0.1, t->remaining)));
        }
    }
    if (k->tip_target && !k->tip_visible) {
        delay = fmin(delay, fmax(0, k->tip_since + FEEL_TOOLTIP_DELAY - now));
    }
    /* A popup popover.c opened a submenu for emits at once. */
    for (int i = 0; i < cf_map_size(k->popups); i++) {
        if (k->popups[i]->open && !k->popups[i]->shown) {
            return 0;
        }
    }
    return delay;
}
