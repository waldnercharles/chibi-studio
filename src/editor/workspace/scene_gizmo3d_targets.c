#include "editor/workspace/scene_gizmo3d_internal.h"
#include "runtime/component_catalog.h"
#include "runtime/transform_propagation.h"
#include "support/array.h"
#include "types/type.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static CF_Quat rotation_of(CF_Quat q)
{
    return q.x == 0 && q.y == 0 && q.z == 0 && q.w == 0 ? cf_quat_identity()
                                                        : cf_quat_norm(q);
}

_Static_assert(FIELD_COUNT == GIZMO3D_FIELD_COUNT, "gizmo field count");

static const CF_V3 UNIT_AXES[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

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

/* Rotation through a parent is representable without shear only when its
   three axes are perpendicular and equally long. Reflections are allowed. */
static bool conformal(const GameGlobalTransform3D *transform)
{
    CF_V3 x = transform->x_axis, y = transform->y_axis, z = transform->z_axis;
    float xx = cf_dot_v3(x, x), yy = cf_dot_v3(y, y), zz = cf_dot_v3(z, z);
    float tolerance = 1e-5f * fmaxf(xx, fmaxf(yy, zz));
    return fabsf(xx - yy) <= tolerance && fabsf(xx - zz) <= tolerance &&
           fabsf(cf_dot_v3(x, y)) <= tolerance &&
           fabsf(cf_dot_v3(x, z)) <= tolerance && fabsf(cf_dot_v3(y, z)) <= tolerance;
}

/* A collapsed or nearly parallel parent has no stable inverse: a world
   move cannot be written as a local position. */
static bool invertible(const GameGlobalTransform3D *transform)
{
    CF_V3 x = transform->x_axis, y = transform->y_axis, z = transform->z_axis;
    float product = cf_len_v3(x) * cf_len_v3(y) * cf_len_v3(z);
    float determinant = cf_dot_v3(cf_cross_v3(x, y), z);
    return isfinite(product) && isfinite(determinant) && product > 0 &&
           fabsf(determinant) > 1e-5f * product;
}

static scene_gizmo_target3d target_of(const ecs_world *world, ecs_entity entity)
{
    scene_gizmo_target3d target = { .entity = entity,
                                    .spatial = GAME_GameTransform3D_has(world, entity),
                                    .from_world = cf_m4_identity(),
                                    .parent_turn = cf_quat_identity(),
                                    .valid = true };
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
            target.valid = invertible(&placed);
            target.turns = target.valid && conformal(&placed);
            if (!target.valid) {
                return target;
            }
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
scene_gizmo_target3d *gizmo3d_targets(const app_scene_authoring *scene)
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
int gizmo3d_primary(const app_scene_authoring *scene, const scene_gizmo_target3d *targets)
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
        if ((targets[i].spatial && !targets[i].turns) ||
            (!targets[i].spatial && fabsf(axis.y) < 1 - 1e-4f)) {
            return false;
        }
        any |= targets[i].turns;
    }
    return any;
}

static bool recordable(const ecs_world *world, const scene_gizmo_target3d *t)
{
    return t->valid &&
           (!t->spatial || type_value_is_finite(
                               GAME_COMPONENT_GameTransform3D.type,
                               GAME_GameTransform3D_get_const(world, t->entity)
                           ));
}

bool gizmo3d_shape(
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
    CF_Quat turn = targets[gizmo3d_primary(scene, targets)].rotation;
    for (int k = 0; k < 3; k++) {
        out->axes[k] = out->local ? cf_mul_q_v3(turn, UNIT_AXES[k]) : UNIT_AXES[k];
        out->turnable[k] = turnable(targets, out->axes[k]);
    }
    out->length = GIZMO3D_ARROW_LENGTH * scene_view3d_world_per_pixel(view, out->pivot);
    return out->length > 0;
}

/* Invalid placement offers no tool; an unrepresentable world rotation
   offers no enabled ring. */
bool scene_gizmo3d_enabled(app_scene_authoring *scene)
{
    scene_gizmo_target3d *targets = gizmo3d_targets(scene);
    bool enabled = !acount(targets);
    if (acount(targets)) {
        scene_view3d view = scene_view3d_of(scene);
        gizmo3d shape;
        enabled = gizmo3d_shape(scene, &view, targets, &shape) &&
                  (scene->gizmo.in.tool != SCENE_TOOL_ROTATE ||
                   shape.turnable[0] || shape.turnable[1] || shape.turnable[2]);
    }
    afree(targets);
    return enabled;
}

void gizmo3d_record(app_scene_authoring *scene, scene_gizmo_target3d *t, scene_tool tool)
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
void gizmo3d_place(app_scene_authoring *scene, const scene_gizmo_target3d *t, CF_V3 at)
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
void gizmo3d_turn(app_scene_authoring *scene, const scene_gizmo_target3d *t, CF_V3 axis, float radians)
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
void gizmo3d_scale(app_scene_authoring *scene, const scene_gizmo_target3d *t, CF_V3 factor)
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
