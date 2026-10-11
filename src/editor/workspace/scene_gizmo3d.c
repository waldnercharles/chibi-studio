/* The Scene's transform tools in a 3D view: Unity's Move arrows, plane
 * squares and center, Rotate rings and Scale handles over the selection,
 * along the world's axes or the active object's, with snapping. Gizmos keep
 * their size on screen. A gesture writes the fields it changes live and
 * records them as one Undo step when it ends (scene_gizmo.c).
 *
 * A Transform3D places an object by its position, rotation and scale. An
 * object without one stands at its Transform, raised to its Geometry3D's
 * elevation and turned about Y by its Orientation, which is all such an
 * object can turn by; its Geometry3D's width, height and depth size it. */
#include "editor/workspace/scene_collider3d.h"
#include "editor/workspace/scene_gizmo3d_internal.h"
#include "support/array.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const float MIN_FACTOR = 0.01f;
static const float SCALE_STEP = 0.1f;
static const float PARALLEL = 1e-3f;
static const char AXIS_NAMES[3] = { 'X', 'Y', 'Z' };

/* Where the ray meets the plane through the pivot across `normal`. */
static bool plane_hit(CF_V3 pivot, CF_V3 normal, CF_Ray3 ray, CF_V3 *hit)
{
    float facing_ray = cf_dot_v3(ray.d, normal);
    if (fabsf(facing_ray) < PARALLEL) {
        return false;
    }
    float t = cf_dot_v3(cf_sub_v3(pivot, ray.p), normal) / facing_ray;
    *hit = cf_add_v3(ray.p, cf_mul_v3_f(ray.d, t));
    return t > 0;
}

/* The plane a part drags across: a square's, or the view's for the center. */
static CF_V3 drag_normal(const scene_gizmo_space *space, const scene_view3d *view, int part)
{
    return part == SCENE_GIZMO_SCREEN ? view->forward
                                      : space->axes[part - SCENE_GIZMO_PLANE_X];
}

void scene_gizmo3d_start(app_scene_authoring *scene, int part, CF_V2 origin)
{
    scene_gizmo *g = &scene->gizmo;
    g->active = part;
    g->modal = g->moved = false;
    g->reselect = -1;
    g->primary = 0;
    g->origin = origin;
    g->turn = g->last = 0;
    g->dirty = scene->scene_dirty;
    g->status[0] = g->delta[0] = '\0';
    scene->dragging_actor = true;
}

static bool begin(app_scene_authoring *scene, int part, const scene_view3d *view, const engine_viewport_pointer *pointer)
{
    finish_transform_edit(scene, false);
    scene_gizmo *g = &scene->gizmo;
    scene_gizmo_target3d *targets = gizmo3d_targets(scene);
    gizmo3d gizmo;
    if (!gizmo3d_shape(scene, view, targets, &gizmo)) {
        afree(targets);
        return false;
    }
    g->space = (scene_gizmo_space){ .pivot = gizmo.pivot,
                                    .length = gizmo.length,
                                    .local = gizmo.local,
                                    .targets = targets };
    memcpy(g->space.axes, gizmo.axes, sizeof gizmo.axes);
    CF_Ray3 ray = scene_view3d_ray(view, pointer);
    if (part >= SCENE_GIZMO_ARROW_X && part <= SCENE_GIZMO_ARROW_Z &&
        !scene_view3d_line_param(
            g->space.pivot,
            g->space.axes[part - SCENE_GIZMO_ARROW_X],
            ray,
            &g->space.along
        )) {
        g->space.along = 0;
    }
    if ((part >= SCENE_GIZMO_PLANE_X && part <= SCENE_GIZMO_SCREEN) &&
        !plane_hit(
            gizmo.pivot,
            drag_normal(&g->space, view, part),
            ray,
            &g->space.grab
        )) {
        g->space.grab = gizmo.pivot;
    }
    CF_V2 origin = scene_view3d_pointer_at(view, pointer);
    int ring = part - SCENE_GIZMO_RING_X;
    if (ring >= 0 && ring < 3) {
        g->space.tangent = gizmo3d_ring_drag(view, &gizmo, ring, origin);
    }
    if (ring >= 0 && ring < 3 && !g->space.tangent.x && !g->space.tangent.y) {
        CF_V3 hit;
        if (!plane_hit(gizmo.pivot, gizmo.axes[ring], ray, &hit) ||
            cf_len_v3(cf_sub_v3(hit, gizmo.pivot)) <= PARALLEL) {
            afree(g->space.targets);
            return false;
        }
        g->space.grab = cf_norm_v3(cf_sub_v3(hit, gizmo.pivot));
    }
    scene_gizmo3d_start(scene, part, origin);
    g->primary = gizmo3d_primary(scene, targets);
    for (int i = 0; i < acount(targets); i++) {
        gizmo3d_record(scene, &targets[i], g->in.tool);
    }
    return true;
}

/* Which of the gizmo's axes `part` moves along. */
static bool moves_along(int part, int k)
{
    if (part == SCENE_GIZMO_SCREEN) {
        return true;
    }
    if (part <= SCENE_GIZMO_ARROW_Z) {
        return part - SCENE_GIZMO_ARROW_X == k;
    }
    return part - SCENE_GIZMO_PLANE_X != k;
}

/* Numbers as the status bar shows them: at most two decimals, no zeros. */
static float shown(float value)
{
    float rounded = roundf(value * 100) / 100;
    return rounded == 0 ? 0 : rounded;
}

static void move_selection(app_scene_authoring *scene, const scene_view3d *view, CF_Ray3 ray)
{
    scene_gizmo *g = &scene->gizmo;
    const scene_gizmo_space *space = &g->space;
    CF_V3 delta;
    if (g->active <= SCENE_GIZMO_ARROW_Z) {
        int k = g->active - SCENE_GIZMO_ARROW_X;
        float t;
        if (!scene_view3d_line_param(space->pivot, space->axes[k], ray, &t)) {
            return;
        }
        delta = cf_mul_v3_f(space->axes[k], t - space->along);
    } else {
        CF_V3 hit;
        if (!plane_hit(space->pivot, drag_normal(space, view, g->active), ray, &hit)) {
            return;
        }
        delta = cf_sub_v3(hit, space->grab);
    }
    /* Along the world's axes the dragged object lands on the grid; along its
       own it moves by whole increments. The rest keep their places around it. */
    float from[3], step[3];
    CF_V3 snapped = { 0 };
    for (int k = 0; k < 3; k++) {
        from[k] = cf_dot_v3(space->targets[g->primary].position, space->axes[k]);
        step[k] = moves_along(g->active, k) ? cf_dot_v3(delta, space->axes[k]) : 0;
        if (scene_gizmo_snapping(g) && moves_along(g->active, k)) {
            step[k] = space->local
                        ? scene_snap(step[k], g->in.increment)
                        : scene_snap(from[k] + step[k], g->in.increment) - from[k];
        }
        snapped = cf_add_v3(snapped, cf_mul_v3_f(space->axes[k], step[k]));
    }
    for (int i = 0; i < acount(space->targets); i++) {
        gizmo3d_place(scene, &space->targets[i], cf_add_v3(space->targets[i].position, snapped));
    }
    snprintf(
        g->delta,
        sizeof g->delta,
        "\xce\x94x %g  \xce\x94y %g  \xce\x94z %g",
        shown(step[0]),
        shown(step[1]),
        shown(step[2])
    );
    scene_gizmo_describe(g, "Move");
}

static void rotate_selection(app_scene_authoring *scene, const scene_view3d *view, CF_V2 at, CF_Ray3 ray)
{
    scene_gizmo *g = &scene->gizmo;
    const scene_gizmo_space *space = &g->space;
    int k = g->active - SCENE_GIZMO_RING_X;
    CF_V3 axis = space->axes[k];
    float radians;
    if (space->tangent.x || space->tangent.y) {
        /* A ring's radius along its tangent on screen turns it a radian. */
        radians = cf_dot(cf_sub(at, g->origin), space->tangent) /
                  (GIZMO3D_ARROW_LENGTH * view->pixel);
    } else {
        CF_V3 hit;
        if (!plane_hit(space->pivot, axis, ray, &hit)) {
            return;
        }
        CF_V3 radial = cf_sub_v3(hit, space->pivot);
        if (cf_len_v3(radial) <= PARALLEL) {
            return;
        }
        radial = cf_norm_v3(radial);
        float angle = atan2f(
            cf_dot_v3(axis, cf_cross_v3(space->grab, radial)),
            cf_dot_v3(space->grab, radial)
        );
        g->turn += remainderf(angle - g->last, 2 * CF_PI);
        g->last = angle;
        radians = g->turn;
    }
    if (scene_gizmo_snapping(g)) {
        radians = scene_snap(radians, SCENE_ROTATION_STEP);
    }
    CF_Quat turn = cf_quat_from_axis_angle(axis, radians);
    for (int i = 0; i < acount(space->targets); i++) {
        const scene_gizmo_target3d *t = &space->targets[i];
        CF_V3 offset = cf_sub_v3(t->position, space->pivot);
        gizmo3d_place(scene, t, cf_add_v3(space->pivot, cf_mul_q_v3(turn, offset)));
        gizmo3d_turn(scene, t, axis, radians);
    }
    snprintf(g->delta, sizeof g->delta, "%c %g\xc2\xb0", AXIS_NAMES[k], roundf(radians * 1800 / CF_PI) / 10);
    scene_gizmo_describe(g, "Rotate");
}

/* How far the pointer has scaled the selection: along the dragged handle
   as it shows on screen, or right and up from the center. */
static float scale_factor(const scene_gizmo *g, const scene_view3d *view, CF_V2 at)
{
    const scene_gizmo_space *space = &g->space;
    CF_V2 d = cf_sub(at, g->origin);
    float reach;
    float moved;
    if (g->active == SCENE_GIZMO_SCALE_ALL) {
        reach = GIZMO3D_ARROW_LENGTH * view->pixel;
        moved = d.x + d.y;
    } else {
        int k = g->active - SCENE_GIZMO_SCALE_X;
        CF_V3 tip = cf_add_v3(space->pivot, cf_mul_v3_f(space->axes[k], space->length));
        CF_V2 handle = cf_sub(
            scene_view3d_on_screen(view, tip),
            scene_view3d_on_screen(view, space->pivot)
        );
        reach = cf_len(handle);
        moved = reach > 0 ? cf_dot(d, handle) / reach : 0;
    }
    float factor = reach > 0 ? 1 + moved / reach : 1;
    if (scene_gizmo_snapping(g)) {
        factor = scene_snap(factor, SCALE_STEP);
    }
    return fmaxf(factor, MIN_FACTOR);
}

static void scale_selection(app_scene_authoring *scene, const scene_view3d *view, CF_V2 at)
{
    scene_gizmo *g = &scene->gizmo;
    const scene_gizmo_space *space = &g->space;
    float factor = scale_factor(g, view, at);
    bool all = g->active == SCENE_GIZMO_SCALE_ALL;
    int k = g->active - SCENE_GIZMO_SCALE_X;
    float f[3];
    for (int i = 0; i < 3; i++) {
        f[i] = all || i == k ? factor : 1;
    }
    for (int i = 0; i < acount(space->targets); i++) {
        const scene_gizmo_target3d *t = &space->targets[i];
        CF_V3 offset = cf_sub_v3(t->position, space->pivot);
        CF_V3 at_scale = space->pivot;
        for (int a = 0; a < 3; a++) {
            float part = cf_dot_v3(offset, space->axes[a]) * f[a];
            at_scale = cf_add_v3(at_scale, cf_mul_v3_f(space->axes[a], part));
        }
        gizmo3d_place(scene, t, at_scale);
        gizmo3d_scale(scene, t, (CF_V3){ f[0], f[1], f[2] });
    }
    if (all) {
        snprintf(g->delta, sizeof g->delta, "%g%%", shown(factor * 100));
    } else {
        snprintf(g->delta, sizeof g->delta, "%c %g%%", AXIS_NAMES[k], shown(factor * 100));
    }
    scene_gizmo_describe(g, "Scale");
}

static void drag(app_scene_authoring *scene, const engine_viewport_pointer *pointer)
{
    scene_gizmo *g = &scene->gizmo;
    scene_view3d view = scene_view3d_of(scene);
    if (pointer->dragging) {
        g->moved = true;
        CF_V2 at = scene_view3d_pointer_at(&view, pointer);
        if (g->active <= SCENE_GIZMO_SCREEN) {
            move_selection(scene, &view, scene_view3d_ray(&view, pointer));
        } else if (g->active <= SCENE_GIZMO_RING_Z) {
            rotate_selection(scene, &view, at, scene_view3d_ray(&view, pointer));
        } else if (g->active <= SCENE_GIZMO_SCALE_ALL) {
            scale_selection(scene, &view, at);
        } else {
            scene_collider3d_drag(scene, scene_view3d_ray(&view, pointer));
        }
    }
    if (pointer->released) {
        scene_gizmo_finish(scene, false);
    }
}

/* The part under the pointer for the selection as it is now. */
static int hovered_part(app_scene_authoring *scene, const scene_view3d *view, CF_V2 at)
{
    scene_gizmo_target3d *targets = gizmo3d_targets(scene);
    gizmo3d gizmo;
    int part = gizmo3d_shape(scene, view, targets, &gizmo)
                 ? gizmo3d_hit(scene->gizmo.in.tool, view, &gizmo, at)
                 : SCENE_GIZMO_NONE;
    afree(targets);
    return part;
}

bool scene_gizmo3d_pointer(app_scene_authoring *scene, const engine_viewport_pointer *pointer)
{
    scene_gizmo *g = &scene->gizmo;
    if (g->active) {
        drag(scene, pointer);
        return true;
    }
    scene_view3d view = scene_view3d_of(scene);
    CF_V2 at = scene_view3d_pointer_at(&view, pointer);
    bool visible = pointer->world_per_pixel > 0 &&
                   (pointer->hovered || pointer->pressed);
    bool select = g->in.tool == SCENE_TOOL_SELECT;
    g->hover = !visible ? SCENE_GIZMO_NONE
             : select   ? scene_collider3d_part(scene, &view, at)
                        : hovered_part(scene, &view, at);
    if (!pointer->pressed || g->hover == SCENE_GIZMO_NONE) {
        return false;
    }
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    bool began = select
                   ? scene_collider3d_begin(scene, g->hover, &view, scene_view3d_ray(&view, pointer))
                   : begin(scene, g->hover, &view, pointer);
    if (began) {
        drag(scene, pointer);
    }
    return true;
}
