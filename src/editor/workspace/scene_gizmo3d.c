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
#include "editor/workspace/scene_internal.h"
#include "editor/workspace/scene_view3d.h"
#include "platform/cute.h"
#include "render/scene3d.h"
#include "runtime/component_catalog.h"
#include "runtime/transform_propagation.h"
#include "support/array.h"
#include "types/type.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Sizes on screen, in pixels. */
static const float ARROW_LENGTH = 80;
static const float ARROW_HEAD = 9;
static const float CENTER = 14;
static const float CUBE = 9;
static const float LINE = 2;
/* How far past a part a press still takes it. */
static const float SLOP = 7;
/* The plane squares' span along each of their axes, as a fraction of an
   arrow. */
static const float PLANE_NEAR = 0.2f;
static const float PLANE_FAR = 0.45f;
static const int RING_SEGMENTS = 64;
/* How far behind the pivot, as the cosine from the eye, a ring still shows. */
static const float RING_BACK = 0.05f;
/* An arrow pointing this close to the eye shows too short to take, and a
   square seen this close to edge-on too thin. */
static const float FORESHORTENED = 0.2f;
/* The smallest factor a scale leaves, and the step it snaps to. */
static const float MIN_FACTOR = 0.01f;
static const float SCALE_STEP = 0.1f;
/* A ray this close to parallel to a square's plane meets it nowhere it
   could be dragged to. */
static const float PARALLEL = 1e-3f;

static const unsigned CENTER_COLOR = 0xE8E8E8FF;
static const unsigned AXIS_COLORS[3] = { SCENE_GIZMO_X_COLOR,
                                         SCENE_GIZMO_Y_COLOR,
                                         SCENE_GIZMO_Z_COLOR };
static const char AXIS_NAMES[3] = { 'X', 'Y', 'Z' };
static const CF_V3 UNIT_AXES[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

/* The fields a gesture may write, per target. */
enum
{
    FIELD_POSITION, /* a Transform3D's */
    FIELD_ROTATION,
    FIELD_SCALE,
    FIELD_TRANSFORM_X, /* a Transform's, its Geometry3D's and its Orientation's */
    FIELD_TRANSFORM_Y,
    FIELD_ELEVATION,
    FIELD_RADIANS,
    FIELD_WIDTH,
    FIELD_HEIGHT,
    FIELD_DEPTH,
    FIELD_COLLIDER_WIDTH,
    FIELD_COLLIDER_HEIGHT,
    FIELD_COLLIDER_RADIUS,
    FIELD_COUNT,
};

/* A selected object as the gesture began, in the world. */
struct scene_gizmo_target3d
{
    ecs_entity entity;
    bool spatial; /* a Transform3D places it */
    bool turns;   /* it has a rotation: a Transform3D or an Orientation */
    CF_V3 position;
    CF_Quat rotation;
    CF_M4x4 from_world;           /* the world into its parent's space */
    CF_Quat parent_turn;          /* its parent's rotation */
    bool mirrored;                /* its parent mirrors its X */
    GameGlobalTransform2D parent; /* without a Transform3D, where its parent
                                     places it; zero for a root */
    int field[FIELD_COUNT];       /* index in the gesture's fields, or -1 */
};

/* Where the selection's gizmo stands. */
typedef struct
{
    CF_V3 pivot; /* the median of the objects' positions */
    CF_V3 axes[3];
    float length;     /* an arrow's, in world units */
    bool local;       /* the axes are the active object's */
    bool turnable[3]; /* every target can turn about the axis, and one does */
} gizmo3d;

static CF_Color packed(unsigned color)
{
    return cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color);
}

/* A zero quaternion reads as no turn, as the engine reads one. */
static CF_Quat rotation_of(CF_Quat q)
{
    return q.x == 0 && q.y == 0 && q.z == 0 && q.w == 0 ? cf_quat_identity()
                                                        : cf_quat_norm(q);
}

/* Whether `axis` points toward the eye from `point`. */
static bool facing(const scene_view3d *view, CF_V3 point, CF_V3 axis)
{
    return cf_dot_v3(axis, cf_sub_v3(view->camera.eye, point)) >= 0;
}

/* ---------- Targets ---------- */

/* What places `entity`: its nearest ancestor with the same transform. */
static ecs_entity placer(const ecs_world *world, ecs_entity entity)
{
    return GAME_GameTransform3D_has(world, entity)
             ? game_transform_parent_3d(world, entity)
             : game_transform_parent(world, entity);
}

static bool placeable(const ecs_world *world, ecs_entity entity)
{
    return GAME_GameTransform3D_has(world, entity) ||
           (GAME_GameTransform_has(world, entity) &&
            GAME_GameGeometry3D_has(world, entity));
}

static bool selected_entity(const app_scene_authoring *scene, ecs_entity entity)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity) &&
            scene_instance_selected(scene, i)) {
            return true;
        }
    }
    return false;
}

/* Whether a selected object places `entity`, which then moves with it. */
static bool carried(const app_scene_authoring *scene, ecs_entity entity)
{
    for (ecs_entity at = placer(scene->world, entity); at.slot;
         at = placer(scene->world, at)) {
        if (selected_entity(scene, at)) {
            return true;
        }
    }
    return false;
}

static scene_gizmo_target3d target_of(const ecs_world *world, ecs_entity entity)
{
    scene_gizmo_target3d target = { .entity = entity,
                                    .spatial = GAME_GameTransform3D_has(world, entity),
                                    .from_world = cf_m4_identity(),
                                    .parent_turn = cf_quat_identity() };
    for (int f = 0; f < FIELD_COUNT; f++) {
        target.field[f] = -1;
    }
    ecs_entity parent = placer(world, entity);
    if (target.spatial) {
        GameGlobalTransform3D global = game_transform_world_3d(world, entity);
        CF_V3 scale;
        cf_m4_decompose(
            game_transform_matrix_3d(&global),
            &target.position,
            &target.rotation,
            &scale
        );
        target.turns = true;
        if (parent.slot) {
            GameGlobalTransform3D placed = game_transform_world_3d(world, parent);
            CF_M4x4 matrix = game_transform_matrix_3d(&placed);
            target.from_world = cf_m4_invert(matrix);
            CF_V3 at, parent_scale;
            /* Decomposing folds a mirrored parent's flip into its X. */
            cf_m4_decompose(matrix, &at, &target.parent_turn, &parent_scale);
            target.mirrored = parent_scale.x < 0;
        }
        return target;
    }
    GameGlobalTransform2D pose = game_transform_world_2d(world, entity);
    float elevation = GAME_GameGeometry3D_get_const(world, entity)->elevation;
    target.position = (CF_V3){ pose.position.x, elevation, pose.position.y };
    /* Gameplay radians turn from +X toward +Z, clockwise seen from above. */
    target.rotation = cf_quat_from_axis_angle(UNIT_AXES[1], -pose.radians);
    target.turns = GAME_GameOrientation_has(world, entity);
    target.parent = parent.slot ? game_transform_world_2d(world, parent)
                                : (GameGlobalTransform2D){ 0 };
    return target;
}

/* The selected objects the gizmo places, leaving out those a selected
   parent carries. The caller frees the array. */
static scene_gizmo_target3d *selection_targets(const app_scene_authoring *scene)
{
    scene_gizmo_target3d *targets = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        ecs_entity entity = scene->actors[i].entity;
        if (scene_instance_selected(scene, i) &&
            placeable(scene->world, entity) && !carried(scene, entity)) {
            apush(targets, target_of(scene->world, entity));
        }
    }
    return targets;
}

/* The target the gizmo's axes follow and whose position snaps: the active
   object, else the first. */
static int primary_of(const app_scene_authoring *scene, const scene_gizmo_target3d *targets)
{
    ecs_entity active = selected_scene_entity(scene);
    for (int i = 0; i < acount(targets); i++) {
        if (ecs_entity_to_u64(targets[i].entity) == ecs_entity_to_u64(active)) {
            return i;
        }
    }
    return 0;
}

/* Whether every target can turn about `axis`: an object without a
   Transform3D turns only about Y. */
static bool turnable(const scene_gizmo_target3d *targets, CF_V3 axis)
{
    bool any = false;
    for (int i = 0; i < acount(targets); i++) {
        if (!targets[i].spatial && fabsf(axis.y) < 1 - 1e-4f) {
            return false;
        }
        any |= targets[i].turns;
    }
    return any;
}

static bool recordable(const ecs_world *world, const scene_gizmo_target3d *t)
{
    return !t->spatial || type_value_is_finite(
                              GAME_COMPONENT_GameTransform3D.type,
                              GAME_GameTransform3D_get_const(world, t->entity)
                          );
}

static bool gizmo_of(
    const app_scene_authoring *scene,
    const scene_view3d *view,
    const scene_gizmo_target3d *targets,
    gizmo3d *out
)
{
    int count = acount(targets);
    if (!count) {
        return false;
    }
    *out = (gizmo3d){ 0 };
    for (int i = 0; i < count; i++) {
        /* Values the gesture could not record, such as an infinite scale,
           offer no gizmo until the object is fixed. */
        if (!recordable(scene->world, &targets[i])) {
            return false;
        }
        out->pivot = cf_add_v3(out->pivot, cf_mul_v3_f(targets[i].position, 1 / (float)count));
    }
    /* Scale always follows the active object's axes, as Unity's does. */
    out->local = scene->gizmo.in.local || scene->gizmo.in.tool == SCENE_TOOL_SCALE;
    CF_Quat turn = targets[primary_of(scene, targets)].rotation;
    for (int k = 0; k < 3; k++) {
        out->axes[k] = out->local ? cf_mul_q_v3(turn, UNIT_AXES[k]) : UNIT_AXES[k];
        out->turnable[k] = turnable(targets, out->axes[k]);
    }
    out->length = ARROW_LENGTH * scene_view3d_world_per_pixel(view, out->pivot);
    return out->length > 0;
}

/* Rotate needs an object that turns; rings it cannot turn about show grey. */
bool scene_gizmo3d_enabled(app_scene_authoring *scene)
{
    if (scene->gizmo.in.tool != SCENE_TOOL_ROTATE) {
        return true;
    }
    scene_gizmo_target3d *targets = selection_targets(scene);
    bool enabled = !acount(targets);
    for (int i = 0; i < acount(targets); i++) {
        enabled |= targets[i].turns;
    }
    afree(targets);
    return enabled;
}

/* ---------- Parts ---------- */

static CF_V3 along(const gizmo3d *gizmo, int k, float fraction)
{
    return cf_add_v3(gizmo->pivot, cf_mul_v3_f(gizmo->axes[k], gizmo->length * fraction));
}

/* Whether arrow or handle `k` shows long enough on screen to take. */
static bool arrow_shown(const scene_view3d *view, const gizmo3d *gizmo, int k)
{
    CF_V2 a = scene_view3d_on_screen(view, gizmo->pivot),
          b = scene_view3d_on_screen(view, along(gizmo, k, 1));
    return cf_len(cf_sub(b, a)) >= FORESHORTENED * ARROW_LENGTH * view->pixel;
}

/* The corners of the square across the axes other than `k`, on their sides
   toward the eye. */
static void plane_corners(const scene_view3d *view, const gizmo3d *gizmo, int k, CF_V2 corners[4])
{
    int i = (k + 1) % 3, j = (k + 2) % 3;
    float si = facing(view, gizmo->pivot, gizmo->axes[i]) ? 1 : -1;
    float sj = facing(view, gizmo->pivot, gizmo->axes[j]) ? 1 : -1;
    const float spans[4][2] = { { PLANE_NEAR, PLANE_NEAR },
                                { PLANE_FAR, PLANE_NEAR },
                                { PLANE_FAR, PLANE_FAR },
                                { PLANE_NEAR, PLANE_FAR } };
    for (int c = 0; c < 4; c++) {
        CF_V3 offset = cf_add_v3(
            cf_mul_v3_f(gizmo->axes[i], si * spans[c][0] * gizmo->length),
            cf_mul_v3_f(gizmo->axes[j], sj * spans[c][1] * gizmo->length)
        );
        corners[c] = scene_view3d_on_screen(view, cf_add_v3(gizmo->pivot, offset));
    }
}

static bool plane_shown(const scene_view3d *view, const gizmo3d *gizmo, int k)
{
    CF_V3 to_eye = cf_norm_v3(cf_sub_v3(view->camera.eye, gizmo->pivot));
    return fabsf(cf_dot_v3(gizmo->axes[k], to_eye)) >= FORESHORTENED;
}

static bool inside(CF_V2 p, const CF_V2 corners[4])
{
    int sign = 0;
    for (int c = 0; c < 4; c++) {
        CF_V2 edge = cf_sub(corners[(c + 1) % 4], corners[c]);
        float side = cf_cross(edge, cf_sub(p, corners[c]));
        int s = side > 0 ? 1 : (side < 0 ? -1 : 0);
        if (s && sign && s != sign) {
            return false;
        }
        sign = s ? s : sign;
    }
    return true;
}

/* Ring `k`'s point at `angle`, in the world. */
static CF_V3 ring_point(const gizmo3d *gizmo, int k, float angle)
{
    CF_V3 u = gizmo->axes[(k + 1) % 3], v = gizmo->axes[(k + 2) % 3];
    CF_V3 offset = cf_add_v3(cf_mul_v3_f(u, cosf(angle)), cf_mul_v3_f(v, sinf(angle)));
    return cf_add_v3(gizmo->pivot, cf_mul_v3_f(offset, gizmo->length));
}

/* Whether ring `k` shows at `angle`: a ring shows its half toward the eye,
   as Unity's, and all of itself while it faces the eye. */
static bool ring_front(const scene_view3d *view, const gizmo3d *gizmo, int k, float angle)
{
    CF_V3 out = cf_sub_v3(ring_point(gizmo, k, angle), gizmo->pivot);
    CF_V3 to_eye = cf_sub_v3(view->camera.eye, gizmo->pivot);
    return cf_dot_v3(out, to_eye) >= -RING_BACK * cf_len_v3(out) * cf_len_v3(to_eye);
}

static float segment_angle(int segment)
{
    return 2 * CF_PI * (float)segment / (float)RING_SEGMENTS;
}

/* How far `at` is from ring `k` on screen, and the angle on the ring of
   the segment nearest it. */
static float ring_distance(const scene_view3d *view, const gizmo3d *gizmo, int k, CF_V2 at, float *angle)
{
    float best = INFINITY;
    for (int s = 0; s < RING_SEGMENTS; s++) {
        float a = segment_angle(s), b = segment_angle(s + 1);
        if (!ring_front(view, gizmo, k, (a + b) / 2)) {
            continue;
        }
        CF_V2 from = scene_view3d_on_screen(view, ring_point(gizmo, k, a));
        CF_V2 to = scene_view3d_on_screen(view, ring_point(gizmo, k, b));
        float distance = scene_view3d_segment_distance(at, from, to);
        if (distance < best) {
            best = distance;
            *angle = (a + b) / 2;
        }
    }
    return best;
}

/* Whether ring `k` shows too thin to follow the pointer's angle around it. */
static bool ring_edge_on(const scene_view3d *view, const gizmo3d *gizmo, int k)
{
    return !plane_shown(view, gizmo, k);
}

/* The direction a positive turn moves ring `k`'s point at `angle` on
   screen. A ring's points advance with their angle about its axis. */
static CF_V2 ring_tangent(const scene_view3d *view, const gizmo3d *gizmo, int k, float angle)
{
    const float step = CF_PI / RING_SEGMENTS;
    CF_V2 ahead = scene_view3d_on_screen(view, ring_point(gizmo, k, angle + step));
    CF_V2 behind = scene_view3d_on_screen(view, ring_point(gizmo, k, angle - step));
    return cf_safe_norm(cf_sub(ahead, behind));
}

static bool in_center(const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at, float size)
{
    CF_V2 d = cf_sub(at, scene_view3d_on_screen(view, gizmo->pivot));
    float half = (size / 2 + 1) * view->pixel;
    return fabsf(d.x) <= half && fabsf(d.y) <= half;
}

/* The arrow, or scale handle, of `first` + k nearest `at` within its slop. */
static int nearest_arrow(const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at, int first)
{
    int part = SCENE_GIZMO_NONE;
    float best = SLOP * view->pixel;
    CF_V2 pivot = scene_view3d_on_screen(view, gizmo->pivot);
    for (int k = 0; k < 3; k++) {
        float distance = scene_view3d_segment_distance(
            at,
            pivot,
            scene_view3d_on_screen(view, along(gizmo, k, 1))
        );
        if (arrow_shown(view, gizmo, k) && distance <= best) {
            best = distance;
            part = first + k;
        }
    }
    return part;
}

static int part_at(scene_tool tool, const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at)
{
    switch (tool) {
        case SCENE_TOOL_MOVE:
            if (in_center(view, gizmo, at, CENTER)) {
                return SCENE_GIZMO_SCREEN;
            }
            for (int k = 0; k < 3; k++) {
                CF_V2 corners[4];
                plane_corners(view, gizmo, k, corners);
                if (plane_shown(view, gizmo, k) && inside(at, corners)) {
                    return SCENE_GIZMO_PLANE_X + k;
                }
            }
            return nearest_arrow(view, gizmo, at, SCENE_GIZMO_ARROW_X);
        case SCENE_TOOL_ROTATE: {
            int part = SCENE_GIZMO_NONE;
            float best = SLOP * view->pixel;
            for (int k = 0; k < 3; k++) {
                float angle;
                float distance = ring_distance(view, gizmo, k, at, &angle);
                if (gizmo->turnable[k] && distance <= best) {
                    best = distance;
                    part = SCENE_GIZMO_RING_X + k;
                }
            }
            return part;
        }
        case SCENE_TOOL_SCALE:
            if (in_center(view, gizmo, at, CUBE)) {
                return SCENE_GIZMO_SCALE_ALL;
            }
            return nearest_arrow(view, gizmo, at, SCENE_GIZMO_SCALE_X);
        default: return SCENE_GIZMO_NONE;
    }
}

/* ---------- Gestures ---------- */

static void add_fields(app_scene_authoring *scene, scene_gizmo_target3d *t, scene_tool tool)
{
    ecs_entity e = t->entity;
    if (t->spatial) {
        t->field[FIELD_POSITION] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameTransform3D, "position");
        if (tool == SCENE_TOOL_ROTATE) {
            t->field[FIELD_ROTATION] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameTransform3D, "rotation");
        }
        if (tool == SCENE_TOOL_SCALE) {
            t->field[FIELD_SCALE] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameTransform3D, "scale");
        }
        return;
    }
    t->field[FIELD_TRANSFORM_X] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameTransform, "x");
    t->field[FIELD_TRANSFORM_Y] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameTransform, "y");
    t->field[FIELD_ELEVATION] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameGeometry3D, "elevation");
    if (tool == SCENE_TOOL_ROTATE) {
        t->field[FIELD_RADIANS] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameOrientation, "radians");
    }
    if (tool != SCENE_TOOL_SCALE) {
        return;
    }
    t->field[FIELD_WIDTH] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameGeometry3D, "width");
    t->field[FIELD_HEIGHT] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameGeometry3D, "height");
    t->field[FIELD_DEPTH] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameGeometry3D, "depth");
    /* Collision follows the visual it was made for, as the 2D gizmo's. */
    t->field[FIELD_COLLIDER_WIDTH] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameCollider, "width");
    t->field[FIELD_COLLIDER_HEIGHT] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameCollider, "height");
    t->field[FIELD_COLLIDER_RADIUS] = scene_gizmo_record(scene, e, &GAME_COMPONENT_GameCollider, "radius");
}

static float started(const app_scene_authoring *scene, const scene_gizmo_target3d *t, int slot)
{
    return scene_gizmo_started(scene, t->field[slot]).f;
}

static void set_float(app_scene_authoring *scene, const scene_gizmo_target3d *t, int slot, float value)
{
    int index = t->field[slot];
    if (index >= 0 && isfinite(value)) {
        scene_gizmo_write(scene, index, (pin_value){ .type = PIN_F32, .f = value });
    }
}

/* Writes a Transform3D's vector or quaternion field. */
static void set_struct(app_scene_authoring *scene, const scene_gizmo_target3d *t, int slot, void *value)
{
    int index = t->field[slot];
    pin_value written = scene_gizmo_started(scene, index);
    written.ptr = value;
    scene_gizmo_write(scene, index, written);
}

/* Moves the target's center to world point `at`, written in its parent's
   space. */
static void place(app_scene_authoring *scene, const scene_gizmo_target3d *t, CF_V3 at)
{
    if (t->spatial) {
        CF_V3 local = cf_m4_transform_point(t->from_world, at);
        set_struct(scene, t, FIELD_POSITION, &local);
        return;
    }
    CF_V2 from_parent = { at.x - t->parent.position.x, at.z - t->parent.position.y };
    float c = cosf(t->parent.radians), s = sinf(t->parent.radians);
    set_float(scene, t, FIELD_TRANSFORM_X, c * from_parent.x + s * from_parent.y);
    set_float(scene, t, FIELD_TRANSFORM_Y, c * from_parent.y - s * from_parent.x);
    set_float(scene, t, FIELD_ELEVATION, at.y);
}

/* Turns the target by `radians` about `axis`, a direction in the world. */
static void turn_target(app_scene_authoring *scene, const scene_gizmo_target3d *t, CF_V3 axis, float radians)
{
    if (t->spatial) {
        if (t->field[FIELD_ROTATION] < 0) {
            return;
        }
        CF_Quat local = rotation_of(
            *(const CF_Quat *)scene_gizmo_started(scene, t->field[FIELD_ROTATION])
                 .ptr
        );
        CF_V3 in_parent = cf_mul_q_v3(cf_quat_conjugate(t->parent_turn), axis);
        if (t->mirrored) {
            /* A turn seen through a mirror turns the other way, about the
               mirrored axis. */
            in_parent.x = -in_parent.x;
            radians = -radians;
        }
        CF_Quat turn = cf_quat_from_axis_angle(in_parent, radians);
        CF_Quat turned = cf_quat_norm(cf_mul_q(turn, local));
        set_struct(scene, t, FIELD_ROTATION, &turned);
        return;
    }
    if (t->field[FIELD_RADIANS] >= 0) {
        /* An object without a Transform3D turns about Y, against its radians. */
        set_float(scene, t, FIELD_RADIANS, started(scene, t, FIELD_RADIANS) - radians * axis.y);
    }
}

/* Scales the target along its own axes by `factor`. */
static void scale_target(app_scene_authoring *scene, const scene_gizmo_target3d *t, CF_V3 factor)
{
    if (t->spatial) {
        if (t->field[FIELD_SCALE] >= 0) {
            CF_V3 scale = cf_mul_v3(
                *(const CF_V3 *)scene_gizmo_started(scene, t->field[FIELD_SCALE])
                     .ptr,
                factor
            );
            set_struct(scene, t, FIELD_SCALE, &scale);
        }
        return;
    }
    set_float(scene, t, FIELD_WIDTH, started(scene, t, FIELD_WIDTH) * factor.x);
    set_float(scene, t, FIELD_HEIGHT, started(scene, t, FIELD_HEIGHT) * factor.y);
    set_float(scene, t, FIELD_DEPTH, started(scene, t, FIELD_DEPTH) * factor.z);
    if (t->field[FIELD_COLLIDER_WIDTH] >= 0) {
        /* The collider lies across X and gameplay Y, the world's Z. */
        set_float(
            scene,
            t,
            FIELD_COLLIDER_WIDTH,
            started(scene, t, FIELD_COLLIDER_WIDTH) * factor.x
        );
        set_float(
            scene,
            t,
            FIELD_COLLIDER_HEIGHT,
            started(scene, t, FIELD_COLLIDER_HEIGHT) * factor.z
        );
        set_float(
            scene,
            t,
            FIELD_COLLIDER_RADIUS,
            started(scene, t, FIELD_COLLIDER_RADIUS) * fmaxf(factor.x, factor.z)
        );
    }
}

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

/* The pointer's angle about the pivot on screen. */
static float screen_angle(const scene_view3d *view, CF_V3 pivot, CF_V2 at)
{
    CF_V2 d = cf_sub(at, scene_view3d_on_screen(view, pivot));
    return atan2f(d.y, d.x);
}

void scene_gizmo3d_start(app_scene_authoring *scene, int part, CF_V2 origin)
{
    scene_gizmo *g = &scene->gizmo;
    g->active = part;
    g->modal = g->moved = false;
    g->reselect = -1;
    g->primary = 0;
    g->origin = origin;
    g->turn = 0;
    g->dirty = scene->scene_dirty;
    g->status[0] = g->delta[0] = '\0';
    scene->dragging_actor = true;
}

static bool begin(app_scene_authoring *scene, int part, const scene_view3d *view, const engine_viewport_pointer *pointer)
{
    finish_transform_edit(scene, false);
    scene_gizmo *g = &scene->gizmo;
    scene_gizmo_target3d *targets = selection_targets(scene);
    gizmo3d gizmo;
    if (!gizmo_of(scene, view, targets, &gizmo)) {
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
    scene_gizmo3d_start(scene, part, scene_view3d_pointer_at(view, pointer));
    g->primary = primary_of(scene, targets);
    int ring = part - SCENE_GIZMO_RING_X;
    if (ring >= 0 && ring < 3 && ring_edge_on(view, &gizmo, ring)) {
        float angle = 0;
        ring_distance(view, &gizmo, ring, g->origin, &angle);
        g->space.tangent = ring_tangent(view, &gizmo, ring, angle);
    }
    g->last = screen_angle(view, gizmo.pivot, g->origin);
    for (int i = 0; i < acount(targets); i++) {
        add_fields(scene, &targets[i], g->in.tool);
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
        place(scene, &space->targets[i], cf_add_v3(space->targets[i].position, snapped));
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

static void rotate_selection(app_scene_authoring *scene, const scene_view3d *view, CF_V2 at)
{
    scene_gizmo *g = &scene->gizmo;
    const scene_gizmo_space *space = &g->space;
    int k = g->active - SCENE_GIZMO_RING_X;
    CF_V3 axis = space->axes[k];
    float radians;
    if (space->tangent.x || space->tangent.y) {
        /* A ring's radius along its tangent on screen turns it a radian. */
        radians = cf_dot(cf_sub(at, g->origin), space->tangent) /
                  (ARROW_LENGTH * view->pixel);
    } else {
        float angle = screen_angle(view, space->pivot, at);
        g->turn += remainderf(angle - g->last, 2 * CF_PI);
        g->last = angle;
        /* Counterclockwise on screen turns about an axis toward the eye. */
        radians = facing(view, space->pivot, axis) ? g->turn : -g->turn;
    }
    if (scene_gizmo_snapping(g)) {
        radians = scene_snap(radians, SCENE_ROTATION_STEP);
    }
    CF_Quat turn = cf_quat_from_axis_angle(axis, radians);
    for (int i = 0; i < acount(space->targets); i++) {
        const scene_gizmo_target3d *t = &space->targets[i];
        CF_V3 offset = cf_sub_v3(t->position, space->pivot);
        place(scene, t, cf_add_v3(space->pivot, cf_mul_q_v3(turn, offset)));
        turn_target(scene, t, axis, radians);
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
        reach = ARROW_LENGTH * view->pixel;
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
        place(scene, t, at_scale);
        scale_target(scene, t, (CF_V3){ f[0], f[1], f[2] });
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
            rotate_selection(scene, &view, at);
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
    scene_gizmo_target3d *targets = selection_targets(scene);
    gizmo3d gizmo;
    int part = gizmo_of(scene, view, targets, &gizmo)
                 ? part_at(scene->gizmo.in.tool, view, &gizmo, at)
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
    /* Select shows the active object's collider handles; the rest their gizmos. */
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

/* ---------- Drawing ---------- */

static void draw_arrows(const scene_gizmo *g, const scene_view3d *view, const gizmo3d *gizmo)
{
    CF_V2 pivot = scene_view3d_on_screen(view, gizmo->pivot);
    for (int k = 0; k < 3; k++) {
        if (!arrow_shown(view, gizmo, k)) {
            continue;
        }
        cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_ARROW_X + k, AXIS_COLORS[k]));
        cf_draw_arrow(
            pivot,
            scene_view3d_on_screen(view, along(gizmo, k, 1)),
            LINE * view->pixel,
            ARROW_HEAD * view->pixel
        );
        cf_draw_pop_color();
    }
    for (int k = 0; k < 3; k++) {
        if (!plane_shown(view, gizmo, k)) {
            continue;
        }
        CF_V2 c[4];
        plane_corners(view, gizmo, k, c);
        CF_Color color = scene_gizmo_color(g, SCENE_GIZMO_PLANE_X + k, AXIS_COLORS[k]);
        CF_Color fill = color;
        fill.a *= 0.35f;
        cf_draw_push_color(fill);
        cf_draw_quad_fill2(c[0], c[1], c[2], c[3], 0);
        cf_draw_pop_color();
        cf_draw_push_color(color);
        cf_draw_quad2(c[0], c[1], c[2], c[3], view->pixel, 0);
        cf_draw_pop_color();
    }
    float half = CENTER / 2 * view->pixel;
    CF_Aabb square = cf_make_aabb_center_half_extents(pivot, (CF_V2){ half, half });
    cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_SCREEN, CENTER_COLOR));
    cf_draw_quad(square, view->pixel, 0);
    cf_draw_pop_color();
}

static void draw_rings(const scene_gizmo *g, const scene_view3d *view, const gizmo3d *gizmo)
{
    for (int k = 0; k < 3; k++) {
        int part = SCENE_GIZMO_RING_X + k;
        cf_draw_push_color(
            gizmo->turnable[k] ? scene_gizmo_color(g, part, AXIS_COLORS[k])
                               : packed(SCENE_GIZMO_DISABLED_COLOR)
        );
        float width = (g->hover == part && !g->active ? 3 : LINE) * view->pixel;
        for (int s = 0; s < RING_SEGMENTS; s++) {
            float a = segment_angle(s), b = segment_angle(s + 1);
            if (ring_front(view, gizmo, k, (a + b) / 2)) {
                cf_draw_line(
                    scene_view3d_on_screen(view, ring_point(gizmo, k, a)),
                    scene_view3d_on_screen(view, ring_point(gizmo, k, b)),
                    width
                );
            }
        }
        cf_draw_pop_color();
    }
}

static void draw_cube(CF_V2 at, float size, CF_Color color)
{
    CF_Aabb cube = cf_make_aabb_center_half_extents(at, (CF_V2){ size / 2, size / 2 });
    cf_draw_push_color(color);
    cf_draw_quad_fill(cube, 0);
    cf_draw_pop_color();
}

static void draw_scale(const scene_gizmo *g, const scene_view3d *view, const gizmo3d *gizmo)
{
    CF_V2 pivot = scene_view3d_on_screen(view, gizmo->pivot);
    for (int k = 0; k < 3; k++) {
        if (!arrow_shown(view, gizmo, k)) {
            continue;
        }
        CF_Color color = scene_gizmo_color(g, SCENE_GIZMO_SCALE_X + k, AXIS_COLORS[k]);
        CF_V2 tip = scene_view3d_on_screen(view, along(gizmo, k, 1));
        cf_draw_push_color(color);
        cf_draw_line(pivot, tip, LINE * view->pixel);
        cf_draw_pop_color();
        draw_cube(tip, CUBE * view->pixel, color);
    }
    draw_cube(pivot, CUBE * view->pixel, scene_gizmo_color(g, SCENE_GIZMO_SCALE_ALL, CENTER_COLOR));
}

void scene_gizmo3d_draw(app_scene_authoring *scene)
{
    const scene_gizmo *g = &scene->gizmo;
    if (g->in.tool == SCENE_TOOL_SELECT || g->modal) {
        return;
    }
    scene_view3d view = scene_view3d_of(scene);
    scene_gizmo_target3d *targets = selection_targets(scene);
    gizmo3d gizmo;
    if (gizmo_of(scene, &view, targets, &gizmo)) {
        switch (g->in.tool) {
            case SCENE_TOOL_MOVE: draw_arrows(g, &view, &gizmo); break;
            case SCENE_TOOL_ROTATE: draw_rings(g, &view, &gizmo); break;
            case SCENE_TOOL_SCALE: draw_scale(g, &view, &gizmo); break;
            default: break;
        }
    }
    afree(targets);
}
