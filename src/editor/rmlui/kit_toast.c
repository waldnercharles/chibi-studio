/* Toasts after Sonner: a bottom-right stack of at most three visible toasts that
 * live four seconds. Collapsed, older toasts sit behind the front one, lifted by
 * the gap and scaled down 5% per step; hovering expands the stack to full
 * heights and pauses every timer. A drag to the right dismisses past 45 dp or
 * 0.11 dp/ms. Each toast may carry one action, such as Undo. Motion uses the
 * kit's springs, so a toast retargets from wherever it is. */
#include "editor/rmlui/kit_internal.h"
#include "editor/ui/kit_icons.h"
#include "support/math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VISIBLE 3
#define LIFETIME 4.0
#define GAP 14.0f
#define SWIPE_DISTANCE 45.0f
#define SWIPE_VELOCITY (0.11f * 1000) /* dp per second */

/* Where a toast is headed this frame. */
typedef struct toast_goal
{
    double y, scale, opacity;
} toast_goal;

static uint32_t type_icon(kit_toast_type type)
{
    switch (type) {
        case KIT_TOAST_SUCCESS: return KIT_ICON_PASS;
        case KIT_TOAST_INFO: return KIT_ICON_INFO;
        case KIT_TOAST_WARNING: return KIT_ICON_WARNING;
        case KIT_TOAST_ERROR: return KIT_ICON_ERROR;
        case KIT_TOAST_LOADING: return KIT_ICON_LOADING;
        default: return 0;
    }
}

static const char *type_name(kit_toast_type type)
{
    static const char *const names[] = { "default", "success", "info",
                                         "warning", "error",   "loading" };
    return names[type];
}

/* Springs track displacement from a target; move the target, keep the path. */
static void retarget(spring *s, double *target, double next)
{
    s->x += *target - next;
    *target = next;
}

static void step(const kit *k, spring *s, spring_params p, double dt)
{
    if (k->reduced_motion) {
        *s = (spring){ 0 };
        return;
    }
    spring_step(s, p, dt);
}

static void toast_free(toast *t)
{
    free(t->title);
    free(t->description);
    free(t->action);
}

static void expire_actions(kit *k)
{
    for (int i = 0; i < cf_map_size(k->toast_actions);) {
        if (k->frame - k->toast_actions[i] <= 2) {
            i++;
            continue;
        }
        uint64_t key = cf_map_keys(k->toast_actions)[i];
        cf_map_del(k->toast_actions, key);
    }
}

/* The pointer expands the stack while it is over the stack's current extent. */
static void update_expanded(kit *k, node *root)
{
    kit_rect area = element_rect(k, root->el);
    float top = 0;
    int shown = 0;
    bool swiping = false;
    for (int i = 0; i < acount(k->toasts); i++) {
        const toast *t = &k->toasts[i];
        swiping |= t->swiping;
        if (!t->removing && shown < VISIBLE) {
            top = math_min((float)(t->y_target - t->height), top);
            shown++;
        }
    }
    float x = k->in.mouse_x, y = k->in.mouse_y;
    bool over = x >= area.x && x <= area.x + area.w && y <= area.y &&
                y >= area.y + top - GAP;
    k->toasts_expanded = shown > 1 && (over || swiping);
}

static node *toast_node(kit *k, const toast *t)
{
    char key[24];
    snprintf(key, sizeof key, "t%d", t->id);
    node *n = emit(k, key, KIND_BOX, "div", "toast");
    widget *w = &n->w;
    if (!w->label_el) {
        w->icon_el = raw_icon(n->el, "lead", 0);
        rml_element *content = raw(n->el, "div", "content", NULL);
        w->label_el = raw(content, "span", "title", NULL);
        w->meta_el = raw(content, "span", "description", NULL);
    }
    set_attr(n, "data-type", type_name(t->type));
    uint32_t icon = type_icon(t->type);
    if (w->icon != icon) {
        w->icon = icon;
        char text[5];
        utf8(text, icon);
        rml_element_set_inner_rml(w->icon_el, text);
        rml_element_set_property(w->icon_el, "display", icon ? "inline-block" : "none");
    }
    set_element_text(w->label_el, t->title);
    set_element_text(w->meta_el, t->description);
    rml_element_set_property(w->meta_el, "display", str_eq(t->description, "") ? "none" : "block");
    return n;
}

static void toast_buttons(kit *k, node *n, toast *t)
{
    push_node(k, n);
    if (!str_eq(t->action, "")) {
        kit_button_desc action = {
            .label = t->action,
            .variant = KIT_SUBTLE,
            .size = KIT_SM,
            .cls = "toast-action",
        };
        if (kit_button(k, "action", &action) && !t->removing) {
            cf_map_set(k->toast_actions, (uint64_t)t->id, k->frame);
            t->removing = true;
        }
    }
    if (t->duration < 0) {
        kit_button_desc close = {
            .icon = KIT_ICON_CLOSE,
            .size = KIT_SM,
            .cls = "toast-close",
            .tip = "Close",
        };
        if (kit_button(k, "close", &close)) {
            t->removing = true;
        }
    }
    pop_node(k);
}

static bool pointer_on_button(kit *k, node *n)
{
    rml_element *hover = rml_context_hover_element(k->context);
    for (rml_element *el = hover; el && el != n->el; el = rml_element_parent(el)) {
        if (strcmp(rml_element_tag_name(el), "button") == 0) {
            return true;
        }
    }
    return false;
}

/* Swipe: press anywhere but a button, drag right. */
static void toast_swipe(kit *k, node *n, toast *t, double dt)
{
    if ((n->events & EV_PRESS) && !pointer_on_button(k, n) && !t->removing) {
        t->swiping = true;
        t->press_x = k->in.mouse_x;
        t->swipe_velocity = 0;
    }
    if (!t->swiping) {
        return;
    }
    float d = k->in.mouse_x - t->press_x;
    float offset = d >= 0 ? d : d / (1.5f + fabsf(d) / 20);
    if (dt > 0 && (k->in.down[0] || offset != t->x.x)) {
        t->swipe_velocity = (float)((offset - t->x.x) / dt);
    }
    t->x = (spring){ offset, 0 };
    if (k->in.down[0]) {
        return;
    }
    /* A stationary pointer-up is not a zero-velocity sample. Hand the last
       measured drag velocity to the spring that takes over from the pointer. */
    t->x.v = t->swipe_velocity;
    t->swiping = false;
    if (offset >= SWIPE_DISTANCE || t->swipe_velocity >= SWIPE_VELOCITY) {
        t->removing = true;
        t->swiped = true;
    }
}

static toast_goal goal_for(const kit *k, const toast *t, int index, float lift)
{
    double opacity = index < VISIBLE ? 1 : 0;
    if (t->removing) {
        double y = t->swiped ? t->y_target : t->y_target + t->height * 0.4;
        return (toast_goal){ y, t->scale_target, 0 };
    }
    if (k->toasts_expanded) {
        return (toast_goal){ -lift, 1, opacity };
    }
    return (toast_goal){ -GAP * index, 1 - 0.05 * index, opacity };
}

static void toast_move(const kit *k, toast *t, toast_goal g, double dt)
{
    if (!t->placed) {
        /* Enter from one height below, as Sonner's translateY(100%). */
        t->placed = true;
        t->y_target = g.y;
        t->y = (spring){ t->height > 0 ? t->height : 48, 0 };
        t->scale_target = g.scale;
        t->opacity_target = g.opacity;
        t->opacity = (spring){ -g.opacity, 0 };
    }
    retarget(&t->y, &t->y_target, g.y);
    retarget(&t->scale, &t->scale_target, g.scale);
    retarget(&t->opacity, &t->opacity_target, g.opacity);
    if (t->removing && t->swiped) {
        retarget(&t->x, &t->x_target, 360);
    } else if (!t->swiping) {
        retarget(&t->x, &t->x_target, 0);
    }
    spring_params p = spring_for(0.4, 0);
    step(k, &t->y, p, dt);
    step(k, &t->scale, p, dt);
    step(k, &t->opacity, p, dt);
    if (!t->swiping) {
        step(k, &t->x, p, dt);
    }
}

/* Collapsed toasts behind the front take its height and hide their content. */
static void toast_style(kit *k, node *n, toast *t, int index)
{
    bool front = index == 0;
    bool behind = !front && !k->toasts_expanded && !t->removing;
    t->forced_height = behind && k->front_height > 0;
    char buffer[96];
    if (t->forced_height) {
        snprintf(buffer, sizeof buffer, "%.1fdp", k->front_height);
        rml_element_set_property(n->el, "height", buffer);
    } else {
        rml_element_remove_property(n->el, "height");
    }
    set_flag(n, "data-behind", behind);
    set_flag(n, "data-front", front);
    snprintf(
        buffer,
        sizeof buffer,
        "translate(%.2fdp, %.2fdp) scale(%.4f)",
        t->x_target + t->x.x,
        t->y_target + t->y.x,
        t->scale_target + t->scale.x
    );
    rml_element_set_property(n->el, "transform", buffer);
    snprintf(
        buffer,
        sizeof buffer,
        "%.3f",
        math_clamp(t->opacity_target + t->opacity.x, 0.0, 1.0)
    );
    rml_element_set_property(n->el, "opacity", buffer);
    bool interactive = index < VISIBLE && !t->removing;
    rml_element_set_property(n->el, "pointer-events", interactive ? "auto" : "none");
    snprintf(buffer, sizeof buffer, "%d", 100 - index);
    rml_element_set_property(n->el, "z-index", buffer);
}

/* Timers pause while the stack is expanded or the pointer is swiping. */
static void toast_timer(const kit *k, toast *t, double dt)
{
    if (t->removing || k->toasts_expanded || t->swiping || t->duration < 0 ||
        t->type == KIT_TOAST_LOADING) {
        return;
    }
    t->remaining -= dt;
    if (t->remaining <= 0) {
        t->removing = true;
    }
}

/* Emits one toast at stack position *index, below *lift dp of expanded
 * toasts, and advances both past it unless it is leaving. */
static void toast_frame(kit *k, toast *t, int *index, float *lift, double dt)
{
    node *n = toast_node(k, t);
    toast_buttons(k, n, t);
    toast_swipe(k, n, t, dt);
    kit_rect r = element_rect(k, n->el);
    if (!t->forced_height && r.h > 0) {
        t->height = r.h;
    }
    toast_move(k, t, goal_for(k, t, *index, *lift), dt);
    toast_style(k, n, t, *index);
    if (*index == 0 && !t->removing) {
        k->front_height = t->height;
    }
    if (!t->removing) {
        *lift += t->height + GAP;
        ++*index;
    }
    toast_timer(k, t, dt);
}

static void remove_finished(kit *k)
{
    int kept = 0;
    for (int i = 0; i < acount(k->toasts); i++) {
        toast t = k->toasts[i];
        bool faded = k->reduced_motion || (t.opacity_target + t.opacity.x) <= 0.01;
        if (t.removing && faded) {
            toast_free(&t);
            continue;
        }
        k->toasts[kept++] = t;
    }
    asetlen(k->toasts, kept);
}

void toasts_frame(kit *k)
{
    double now = k->in.time;
    double dt = math_clamp(now - k->toast_time, 0.0, 0.1);
    k->toast_time = now;
    expire_actions(k);
    /* Faded toasts go before the toaster emits, so the last one's node
       leaves in the frame its fade ends asked for. */
    remove_finished(k);
    if (!acount(k->toasts)) {
        return;
    }
    node *root = emit_in(k, k->layer, "@toaster", KIND_BOX, "section", "toaster");
    update_expanded(k, root);
    apush(k->stack, k->layer); /* emitted after the root closed; keep the layer's cursor */
    push_node(k, root);
    /* Newest first: index 0 is the front toast. */
    float lift = 0;
    int index = 0;
    for (int i = acount(k->toasts) - 1; i >= 0; i--) {
        toast_frame(k, &k->toasts[i], &index, &lift, dt);
    }
    pop_node(k);
    (void)apop(k->stack);
}

/* ---------- API ---------- */

/* The last live toast with this id, or NULL. */
static toast *live_toast(kit *k, int id)
{
    for (int i = acount(k->toasts) - 1; i >= 0; i--) {
        if (k->toasts[i].id == id && !k->toasts[i].removing) {
            return &k->toasts[i];
        }
    }
    return NULL;
}

int kit_toast(kit *k, int id, const kit_toast_desc *d)
{
    toast *t = live_toast(k, id);
    if (!t) {
        apush(
            k->toasts,
            (toast){
                .id = ++k->toast_next,
                .duration = LIFETIME,
                .remaining = LIFETIME,
                .scale_target = 1,
                .opacity_target = 1,
            }
        );
        t = &alast(k->toasts);
    }
    t->type = d->type;
    str_set(&t->title, d->title);
    str_set(&t->description, d->description);
    str_set(&t->action, d->action);
    t->duration = d->duration < 0 ? -1
                : d->duration > 0 ? d->duration
                                  : LIFETIME;
    t->remaining = t->duration;
    return t->id;
}

void kit_toast_dismiss(kit *k, int id)
{
    for (int i = 0; i < acount(k->toasts); i++) {
        if (k->toasts[i].id == id) {
            k->toasts[i].removing = true;
        }
    }
}

bool kit_toast_action(kit *k, int id)
{
    return cf_map_del(k->toast_actions, (uint64_t)id) != 0;
}
