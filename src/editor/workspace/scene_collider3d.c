/* A 3D view's colliders, as Unity's: the selected objects' Collider3D
 * boxes, spheres and capsules outlined in green, and with the Select tool a
 * handle on each side of the active object's that drags its extent along
 * that axis. The collider stays centered on its object, so its opposite
 * side follows. A drag writes the one field it changes live and records it
 * as one Undo step when it ends (scene_gizmo.c). With the Colliders view
 * option, as Unreal's Collision show flag, every object's collider is
 * outlined fainter. Hulls and meshes show no outline. */
#include "editor/workspace/scene_collider3d.h"

#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "runtime/transform_propagation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Sizes on screen, in pixels. */
static const float HANDLE = 8;
static const float LINE = 1.5f;
/* How far past a handle a press still takes it. */
static const float SLOP = 5;
static const int CIRCLE_SEGMENTS = 48;
/* How far behind the center, as the cosine from the eye, a handle shows. */
static const float BACK = 0.05f;
/* The least extent a drag leaves, in world units. */
static const float LEAST = 0.01f;
/* A scale this small along any axis leaves nothing to drag. */
static const float FLAT = 1e-6f;

static const unsigned COLLIDER_COLOR = 0x8CE07AFFu;
/* Opaque, since overlapping segments would show their joins. */
static const unsigned UNSELECTED_COLOR = 0x5E9652FFu;
static const CF_V3 UNIT_AXES[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

/* An object's collider as it stands in the world. */
typedef struct
{
    const Collider3D *collider;
    CF_V3 center;
    CF_Quat turn;
    CF_V3 scale; /* its world scale's magnitude along each axis */
} collider3d;

static float component(CF_V3 v, int k)
{
    return k == 0 ? v.x : k == 1 ? v.y : v.z;
}

/* Numbers as the status bar shows them: at most two decimals. */
static float shown(float value)
{
    return roundf(value * 100) / 100;
}

static CF_Color packed(unsigned color)
{
    return cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color);
}

static bool outlined(const Collider3D *collider)
{
    switch (collider->shape) {
        case COLLIDER3D_BOX:
            return isfinite(collider->size.x) && isfinite(collider->size.y) &&
                   isfinite(collider->size.z);
        case COLLIDER3D_SPHERE: return isfinite(collider->radius);
        case COLLIDER3D_CAPSULE:
            return isfinite(collider->radius) && isfinite(collider->height);
        default: return false;
    }
}

/* `entity`'s box, sphere or capsule, placed as physics places it: centered
   on the entity and scaled by its world scale. */
static bool collider_of(const ecs_world *world, ecs_entity entity, collider3d *out)
{
    const Collider3D *collider = Collider3D_get_const(world, entity);
    if (!collider || !Transform3D_has(world, entity) || !outlined(collider)) {
        return false;
    }
    GlobalTransform3D global = transform_world_3d(world, entity);
    CF_V3 scale;
    *out = (collider3d){ .collider = collider };
    cf_m4_decompose(transform_matrix_3d(&global), &out->center, &out->turn, &scale);
    out->scale = cf_abs_v3(scale);
    if (!isfinite(cf_len_v3(out->center)) || !isfinite(cf_len_v3(out->scale))) {
        return false;
    }
    return out->scale.x > FLAT && out->scale.y > FLAT && out->scale.z > FLAT;
}

static float sphere_scale(const collider3d *c)
{
    return fmaxf(c->scale.x, fmaxf(c->scale.y, c->scale.z));
}

static float capsule_scale(const collider3d *c)
{
    return fmaxf(c->scale.x, c->scale.z);
}

/* How far the collider reaches from its center along each of its axes. */
static CF_V3 reach_of(const collider3d *c)
{
    const Collider3D *collider = c->collider;
    switch (collider->shape) {
        case COLLIDER3D_BOX:
            return cf_mul_v3_f(cf_mul_v3(collider->size, c->scale), 0.5f);
        case COLLIDER3D_SPHERE: {
            float r = collider->radius * sphere_scale(c);
            return (CF_V3){ r, r, r };
        }
        default: {
            float r = collider->radius * capsule_scale(c);
            return (CF_V3){ r, fmaxf(collider->height * c->scale.y / 2, r), r };
        }
    }
}

static CF_V3 axis_of(const collider3d *c, int k)
{
    return cf_mul_q_v3(c->turn, UNIT_AXES[k]);
}

/* Handle `h`'s axis, 0 to 2, and its side, 1 or -1. */
static int handle_axis(int h)
{
    return h / 2;
}

static float handle_side(int h)
{
    return h % 2 ? -1.0f : 1.0f;
}

static CF_V3 handle_direction(const collider3d *c, int h)
{
    return cf_mul_v3_f(axis_of(c, handle_axis(h)), handle_side(h));
}

static CF_V3 handle_point(const collider3d *c, int h)
{
    float reach = component(reach_of(c), handle_axis(h));
    return cf_add_v3(c->center, cf_mul_v3_f(handle_direction(c, h), reach));
}

/* Whether handle `h` faces the eye, or nearly: the far side's hide. */
static bool handle_shown(const scene_view3d *view, const collider3d *c, int h)
{
    CF_V3 out = handle_direction(c, h);
    CF_V3 to_eye = cf_sub_v3(view->camera.eye, c->center);
    CF_V3 point = handle_point(c, h);
    if (cf_dot_v3(out, to_eye) < -BACK * cf_len_v3(to_eye) ||
        scene_view3d_world_per_pixel(view, point) <= 0) {
        return false;
    }
    CF_Ray3 ray = { .p = view->camera.eye,
                    .d = cf_norm_v3(cf_sub_v3(point, view->camera.eye)) };
    float along;
    return scene_view3d_line_param(c->center, out, ray, &along);
}

/* The collider the handles drag: the active authored object's, when the
   view shows it. A paused run's spawned object shows its outline but, as
   with the transform gizmo, takes no edits. */
static bool active_collider(app_scene_authoring *scene, render_skip hidden, collider3d *out)
{
    ecs_entity active = selected_scene_entity(scene);
    return scene->gizmo.in.tool == SCENE_TOOL_SELECT && scene_can_move(scene) &&
           !scene->runtime_selected.slot && active.slot &&
           !render_skip_has(hidden, active) &&
           collider_of(scene->world, active, out);
}

static render_skip hidden_now(app_scene_authoring *scene)
{
    return scene_view_skip(scene, APP_SCENE_VIEW_HIDDEN, &scene->view_hidden);
}

int scene_collider3d_part(app_scene_authoring *scene, const scene_view3d *view, CF_V2 at)
{
    collider3d c;
    if (!active_collider(scene, hidden_now(scene), &c)) {
        return SCENE_GIZMO_NONE;
    }
    int part = SCENE_GIZMO_NONE;
    float best = (HANDLE / 2 + SLOP) * view->pixel;
    for (int h = 0; h < 6; h++) {
        float distance = cf_len(
            cf_sub(at, scene_view3d_on_screen(view, handle_point(&c, h)))
        );
        if (handle_shown(view, &c, h) && distance <= best) {
            best = distance;
            part = SCENE_GIZMO_FACE + h;
        }
    }
    return part;
}

/* The field handle `h` drags: a box's size, a capsule's height along its
   Y, else the radius. */
static const char *dragged_field(const Collider3D *collider, int h)
{
    if (collider->shape == COLLIDER3D_BOX) {
        return "size";
    }
    return collider->shape == COLLIDER3D_CAPSULE && handle_axis(h) == 1
             ? "height"
             : "radius";
}

bool scene_collider3d_begin(app_scene_authoring *scene, int part, const scene_view3d *view, CF_Ray3 ray)
{
    collider3d c;
    if (!active_collider(scene, hidden_now(scene), &c)) {
        return false;
    }
    int h = part - SCENE_GIZMO_FACE;
    scene_gizmo *g = &scene->gizmo;
    CF_V3 direction = handle_direction(&c, h);
    float along = 0;
    if (!scene_view3d_line_param(c.center, direction, ray, &along)) {
        return false;
    }
    const char *name = dragged_field(c.collider, h);
    float reach = component(reach_of(&c), handle_axis(h));
    CF_V2 origin = scene_view3d_on_screen(view, handle_point(&c, h));
    finish_transform_edit(scene, false);
    scene_gizmo3d_start(scene, part, origin);
    g->space = (scene_gizmo_space){ .pivot = c.center,
                                    .axes = { direction },
                                    .along = along,
                                    .length = reach };
    /* The gesture writes one field, its first. */
    int field = scene_gizmo_record(scene, selected_scene_entity(scene), &COMPONENT_Collider3D, name);
    assert(field == 0);
    return true;
}

/* `reach` from the center as the field holds it. */
static float field_value(const collider3d *c, int h, float reach)
{
    const Collider3D *collider = c->collider;
    int k = handle_axis(h);
    switch (collider->shape) {
        case COLLIDER3D_BOX: return 2 * reach / component(c->scale, k);
        case COLLIDER3D_SPHERE: return reach / sphere_scale(c);
        default: break;
    }
    if (k == 1) {
        /* A capsule's caps are part of its height, which keeps them whole. */
        float radius = collider->radius * capsule_scale(c);
        return 2 * fmaxf(reach, radius) / c->scale.y;
    }
    float most = collider->height * c->scale.y / 2;
    return fminf(reach, most) / capsule_scale(c);
}

void scene_collider3d_drag(app_scene_authoring *scene, CF_Ray3 ray)
{
    scene_gizmo *g = &scene->gizmo;
    const scene_gizmo_space *space = &g->space;
    collider3d c;
    float along;
    if (!active_collider(scene, hidden_now(scene), &c)) {
        scene_gizmo_finish(scene, true);
        return;
    }
    if (!scene_view3d_line_param(space->pivot, space->axes[0], ray, &along)) {
        return;
    }
    float reach = space->length + along - space->along;
    if (scene_gizmo_snapping(g)) {
        /* Snapping puts the collider's whole extent on the grid. */
        reach = scene_snap(2 * reach, g->in.increment) / 2;
    }
    int h = g->active - SCENE_GIZMO_FACE;
    float value = field_value(&c, h, fmaxf(reach, LEAST));
    const char *field = dragged_field(c.collider, h);
    if (c.collider->shape != COLLIDER3D_BOX) {
        scene_gizmo_write(scene, 0, (pin_value){ .type = PIN_F32, .f = value });
        snprintf(g->delta, sizeof g->delta, "%s %g", field, shown(value));
        scene_gizmo_describe(g, "Collider");
        return;
    }
    pin_value written = scene_gizmo_started(scene, 0);
    CF_V3 size = *(const CF_V3 *)written.ptr;
    int k = handle_axis(h);
    size = (CF_V3){ k == 0 ? value : size.x,
                    k == 1 ? value : size.y,
                    k == 2 ? value : size.z };
    written.ptr = &size;
    scene_gizmo_write(scene, 0, written);
    snprintf(
        g->delta,
        sizeof g->delta,
        "%g \xc3\x97 %g \xc3\x97 %g",
        shown(size.x),
        shown(size.y),
        shown(size.z)
    );
    scene_gizmo_describe(g, "Collider");
}

/* ---------- Drawing ---------- */

static void draw_segment(const scene_view3d *view, CF_V3 a, CF_V3 b, float width)
{
    if (scene_view3d_world_per_pixel(view, a) > 0 &&
        scene_view3d_world_per_pixel(view, b) > 0) {
        cf_draw_line(scene_view3d_on_screen(view, a), scene_view3d_on_screen(view, b), width);
    }
}

/* The arc about `center` from `u` toward `v`, both of length `radius`, over
   `turns` of a full circle. */
static void draw_arc(const scene_view3d *view, CF_V3 center, CF_V3 u, CF_V3 v, float turns, float width)
{
    int segments = (int)ceilf(CIRCLE_SEGMENTS * turns);
    CF_V3 last = cf_add_v3(center, u);
    for (int s = 1; s <= segments; s++) {
        float angle = 2 * CF_PI * turns * (float)s / (float)segments;
        CF_V3 offset = cf_add_v3(cf_mul_v3_f(u, cosf(angle)), cf_mul_v3_f(v, sinf(angle)));
        CF_V3 next = cf_add_v3(center, offset);
        draw_segment(view, last, next, width);
        last = next;
    }
}

static void draw_box(const scene_view3d *view, const collider3d *c, float width)
{
    CF_V3 reach = reach_of(c);
    CF_V3 axes[3];
    for (int k = 0; k < 3; k++) {
        axes[k] = cf_mul_v3_f(axis_of(c, k), ((const float *)&reach)[k]);
    }
    /* Each edge runs along one axis, at one of four corners across the
       other two. */
    for (int k = 0; k < 3; k++) {
        CF_V3 u = axes[(k + 1) % 3], v = axes[(k + 2) % 3];
        for (int corner = 0; corner < 4; corner++) {
            CF_V3 across = cf_add_v3(
                cf_mul_v3_f(u, corner & 1 ? 1 : -1),
                cf_mul_v3_f(v, corner & 2 ? 1 : -1)
            );
            CF_V3 mid = cf_add_v3(c->center, across);
            draw_segment(view, cf_sub_v3(mid, axes[k]), cf_add_v3(mid, axes[k]), width);
        }
    }
}

static void draw_sphere(const scene_view3d *view, const collider3d *c, float width)
{
    float r = reach_of(c).x;
    for (int k = 0; k < 3; k++) {
        CF_V3 u = cf_mul_v3_f(axis_of(c, (k + 1) % 3), r);
        CF_V3 v = cf_mul_v3_f(axis_of(c, (k + 2) % 3), r);
        draw_arc(view, c->center, u, v, 1, width);
    }
}

/* Rings where the caps meet the side, four lines along it, and half
   circles over each cap. */
static void draw_capsule(const scene_view3d *view, const collider3d *c, float width)
{
    CF_V3 reach = reach_of(c);
    float r = reach.x;
    CF_V3 x = cf_mul_v3_f(axis_of(c, 0), r), z = cf_mul_v3_f(axis_of(c, 2), r);
    CF_V3 up = axis_of(c, 1);
    CF_V3 half = cf_mul_v3_f(up, reach.y - r);
    CF_V3 top = cf_add_v3(c->center, half), bottom = cf_sub_v3(c->center, half);
    CF_V3 cap = cf_mul_v3_f(up, r);
    for (int end = 0; end < 2; end++) {
        CF_V3 at = end ? bottom : top;
        CF_V3 outward = end ? cf_neg_v3(cap) : cap;
        draw_arc(view, at, x, z, 1, width);
        draw_arc(view, at, x, outward, 0.5f, width);
        draw_arc(view, at, z, outward, 0.5f, width);
    }
    CF_V3 sides[4] = { x, cf_neg_v3(x), z, cf_neg_v3(z) };
    for (int i = 0; i < 4; i++) {
        draw_segment(view, cf_add_v3(top, sides[i]), cf_add_v3(bottom, sides[i]), width);
    }
}

static void draw_handles(const scene_gizmo *g, const scene_view3d *view, const collider3d *c)
{
    float half = HANDLE / 2 * view->pixel;
    for (int h = 0; h < 6; h++) {
        if (!handle_shown(view, c, h)) {
            continue;
        }
        CF_V2 at = scene_view3d_on_screen(view, handle_point(c, h));
        cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_FACE + h, COLLIDER_COLOR));
        cf_draw_quad_fill(cf_make_aabb_center_half_extents(at, (CF_V2){ half, half }), 0);
        cf_draw_pop_color();
    }
}

static void draw_collider(const scene_view3d *view, const collider3d *c, float width)
{
    switch (c->collider->shape) {
        case COLLIDER3D_BOX: draw_box(view, c, width); break;
        case COLLIDER3D_SPHERE: draw_sphere(view, c, width); break;
        default: draw_capsule(view, c, width); break;
    }
}

/* The Colliders view option: every shown object's collider, the game's
   spawned ones too, under the selection's brighter outlines. */
static void draw_every_collider(
    app_scene_authoring *scene,
    const scene_view3d *view,
    render_skip hidden,
    float width
)
{
    ecs_component_id required[] = { COMPONENT_Collider3D.id,
                                    COMPONENT_Transform3D.id };
    ecs_query query;
    if (ecs_query_begin(scene->world, &query, required, 2) != ECS_OK) {
        return;
    }
    cf_draw_push_color(packed(UNSELECTED_COLOR));
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        collider3d c;
        if (!render_skip_has(hidden, entity) && collider_of(scene->world, entity, &c)) {
            draw_collider(view, &c, width);
        }
    }
    cf_draw_pop_color();
    ecs_query_end(&query);
}

void scene_collider3d_draw(app_scene_authoring *scene, render_skip hidden)
{
    scene_view3d view = scene_view3d_of(scene);
    float width = LINE * view.pixel;
    if (scene->view_options.colliders) {
        draw_every_collider(scene, &view, hidden, width);
    }
    cf_draw_push_color(packed(COLLIDER_COLOR));
    for (int i = -1; i < acount(scene->actors); i++) {
        ecs_entity entity = i < 0 ? scene->runtime_selected : scene->actors[i].entity;
        bool selected = i < 0 ? entity.slot != 0 : scene_instance_selected(scene, i);
        collider3d c;
        if (selected && !render_skip_has(hidden, entity) &&
            collider_of(scene->world, entity, &c)) {
            draw_collider(&view, &c, width);
        }
    }
    cf_draw_pop_color();
    collider3d active;
    if (!scene->gizmo.modal && active_collider(scene, hidden, &active)) {
        draw_handles(&scene->gizmo, &view, &active);
    }
}
