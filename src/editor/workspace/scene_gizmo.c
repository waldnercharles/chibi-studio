/* The Scene's transform tools: gizmos over the selection in a 2D view
 * (Unity's Move, Rotate and Scale, with Godot's Y arrow pointing down, the
 * way gameplay Y runs), a dragged object's move, Blender's modal move (G),
 * snapping to the grid, and the grid under the scene. Gizmos keep their size
 * on screen. A gesture writes the fields it changes live and records them as
 * one Undo step when it ends. */
#include "editor/collab/actions_scene.h"
#include "editor/workspace/scene_internal.h"
#include "editor/workspace/viewport.h"
#include "platform/cute.h"
#include "render/grid_2d.h"
#include "runtime/component_catalog.h"
#include "runtime/transform_propagation.h"
#include "support/array.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sizes on screen, in pixels. */
static const float ARROW_LENGTH = 72;
static const float ARROW_HEAD = 8;
static const float SQUARE = 14;
static const float RING_RADIUS = 64;
static const float HANDLE = 9;
static const float LINE = 2;
/* How far past a part a press still takes it: the arrows and handles, and
   either side of the ring, which is thinner. */
static const float SLOP = 8;
static const float RING_SLOP = 10;
/* The smallest size a resize leaves, in world units. */
static const float MIN_SIZE = 1;

static const unsigned FREE_COLOR = 0x82B5FFFF;
static const unsigned HANDLE_FILL = 0xFFFFFFFF;

/* Each resize handle's side of the box, counterclockwise from the bottom left. */
static const signed char HANDLE_SIDES[8][2] = { { -1, -1 }, { 0, -1 },
                                                { 1, -1 },  { 1, 0 },
                                                { 1, 1 },   { 0, 1 },
                                                { -1, 1 },  { -1, 0 } };

/* The fields a gesture may write, per target. */
enum
{
    GIZMO_FIELD_X,
    GIZMO_FIELD_Y,
    FIELD_RADIANS,
    FIELD_WIDTH,
    FIELD_HEIGHT,
    FIELD_COLLIDER_WIDTH,
    FIELD_COLLIDER_HEIGHT,
    FIELD_COLLIDER_RADIUS,
    FIELD_COUNT,
};

/* A selected object as the gesture began, in canvas units (Y up). */
struct scene_gizmo_target
{
    ecs_entity entity;
    CF_V2 position; /* in the world, as the canvas shows it */
    float angle;    /* canvas radians: gameplay's negated */
    bool oriented;
    GlobalTransform2D parent; /* where its parent places it; zero for a root */
    CF_V2 size; /* its visual's width and height; zero without one */
    int field[FIELD_COUNT]; /* index in the gesture's fields, or -1 */
};

/* A reflected field the gesture writes: its value before and now, which
   own their storage. */
struct scene_gizmo_field
{
    ecs_entity entity;
    ecs_component_id component;
    int field;
    pin_value before;
    pin_value after;
};

/* Where the selection's gizmo sits, in canvas units. */
typedef struct
{
    CF_V2 pivot; /* the median of the objects' positions (Blender's Median Point) */
    CF_V2 center; /* the resize box: one object's own, else around them all */
    CF_V2 half;
    float angle;
    bool oriented; /* something to rotate */
    bool sized;    /* something to resize */
} gizmo_frame;

bool scene_gizmo_spatial(int part)
{
    return part >= SCENE_GIZMO_ARROW_X && part < SCENE_GIZMO_HANDLE;
}

float scene_snap(float value, float increment)
{
    assert(increment > 0);
    return roundf(value / increment) * increment;
}

static CF_V2 rotated(CF_V2 v, float angle)
{
    float c = cosf(angle), s = sinf(angle);
    return (CF_V2){ v.x * c - v.y * s, v.x * s + v.y * c };
}

static CF_Color packed(unsigned color)
{
    return cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color);
}

static int field_named(const ecs_component_desc *component, const char *name)
{
    for (int f = 0; f < component->type->field_count; f++) {
        if (!strcmp(component->type->fields[f].name, name)) {
            return f;
        }
    }
    assert(!"a reflected field of the component");
    return -1;
}

static pin_value read_field(
    const ecs_world *world,
    ecs_entity entity,
    const ecs_component_desc *component,
    const char *name
)
{
    pin_value value = { 0 };
    int read = ecs_component_read_field(
        NULL,
        world,
        entity,
        component->id,
        field_named(component, name),
        &value
    );
    assert(read == ECS_OK && value.type == PIN_F32);
    return value;
}

/* The component whose width and height size the entity's visual, with the
   name of its height: a mesh surface's geometry, which is drawn from above,
   or its appearance. */
static const ecs_component_desc *sized_component(
    const ecs_world *world,
    ecs_entity entity,
    const char **height
)
{
    const MeshSurface2D *mesh = MeshSurface2D_get_const(world, entity);
    if (mesh && mesh->enabled && Geometry3D_has(world, entity)) {
        *height = "depth";
        return &COMPONENT_Geometry3D;
    }
    *height = "height";
    return ecs_component_has(world, entity, COMPONENT_Appearance.id)
             ? &COMPONENT_Appearance
             : NULL;
}

static scene_gizmo_target target_of(const ecs_world *world, ecs_entity entity)
{
    GlobalTransform2D pose = transform_world_2d(world, entity);
    ecs_entity parent = transform_parent(world, entity);
    scene_gizmo_target target = {
        .entity = entity,
        .position = { pose.position.x, -pose.position.y },
        .angle = -pose.radians,
        .oriented = Orientation_get_const(world, entity) != NULL,
        .parent = parent.slot ? transform_world_2d(world, parent)
                              : (GlobalTransform2D){ 0 },
    };
    const char *height;
    const ecs_component_desc *size = sized_component(world, entity, &height);
    if (size) {
        CF_V2 extent = { read_field(world, entity, size, "width").f,
                         read_field(world, entity, size, height).f };
        if (extent.x > 0 && extent.y > 0 && isfinite(extent.x) &&
            isfinite(extent.y)) {
            target.size = extent;
        }
    }
    for (int f = 0; f < FIELD_COUNT; f++) {
        target.field[f] = -1;
    }
    return target;
}

/* Whether a selected object places `entity`, which then moves with it. */
static bool placed_by_selection(const app_scene_authoring *scene, ecs_entity entity)
{
    for (ecs_entity at = transform_parent(scene->world, entity); at.slot;
         at = transform_parent(scene->world, at)) {
        for (int i = 0; i < acount(scene->actors); i++) {
            if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(at) &&
                scene_instance_selected(scene, i)) {
                return true;
            }
        }
    }
    return false;
}

/* The selected objects with a position, as they are now, leaving out those a
   selected parent carries. The caller frees the array. */
static scene_gizmo_target *selection_targets(const app_scene_authoring *scene)
{
    scene_gizmo_target *targets = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        ecs_entity entity = scene->actors[i].entity;
        if (scene_instance_selected(scene, i) &&
            Transform_get_const(scene->world, entity) &&
            !placed_by_selection(scene, entity)) {
            apush(targets, target_of(scene->world, entity));
        }
    }
    return targets;
}

static bool frame_of(const scene_gizmo_target *targets, gizmo_frame *out)
{
    int count = acount(targets);
    if (!count) {
        return false;
    }
    *out = (gizmo_frame){ 0 };
    CF_V2 min = { INFINITY, INFINITY }, max = { -INFINITY, -INFINITY };
    bool any_size = false;
    for (int i = 0; i < count; i++) {
        const scene_gizmo_target *t = &targets[i];
        out->pivot.x += t->position.x / (float)count;
        out->pivot.y += t->position.y / (float)count;
        out->oriented |= t->oriented;
        any_size |= t->size.x > 0;
        for (int corner = 0; corner < 4; corner++) {
            CF_V2 local = { (corner & 1 ? 0.5f : -0.5f) * t->size.x,
                            (corner & 2 ? 0.5f : -0.5f) * t->size.y };
            CF_V2 at = cf_add(t->position, rotated(local, t->angle));
            min = (CF_V2){ fminf(min.x, at.x), fminf(min.y, at.y) };
            max = (CF_V2){ fmaxf(max.x, at.x), fmaxf(max.y, at.y) };
        }
    }
    if (count == 1) {
        out->center = targets[0].position;
        out->half = cf_mul_v2_f(targets[0].size, 0.5f);
        out->angle = targets[0].angle;
    } else {
        out->center = cf_mul_v2_f(cf_add(min, max), 0.5f);
        out->half = cf_mul_v2_f(cf_sub(max, min), 0.5f);
    }
    out->sized = any_size && out->half.x > 0 && out->half.y > 0;
    return true;
}

/* Where resize handle `handle` shows: on the box, which spreads to keep its
   handles apart on screen while the box is small. */
static CF_V2 handle_at(const gizmo_frame *frame, int handle, float pixel)
{
    float least = 1.5f * HANDLE * pixel;
    CF_V2 local = { HANDLE_SIDES[handle][0] * fmaxf(frame->half.x, least),
                    HANDLE_SIDES[handle][1] * fmaxf(frame->half.y, least) };
    return cf_add(frame->center, rotated(local, frame->angle));
}

static bool tool_enabled(scene_tool tool, const gizmo_frame *frame)
{
    switch (tool) {
        case SCENE_TOOL_ROTATE: return frame->oriented;
        case SCENE_TOOL_SCALE: return frame->sized;
        default: return true;
    }
}

bool scene_gizmo_enabled(app_scene_authoring *scene)
{
    if (scene->render_viewport && engine_viewport_perspective(scene->render_viewport)) {
        return scene_gizmo3d_enabled(scene);
    }
    scene_gizmo_target *targets = selection_targets(scene);
    gizmo_frame frame;
    bool enabled = !frame_of(targets, &frame) ||
                   tool_enabled(scene->gizmo.in.tool, &frame);
    afree(targets);
    return enabled;
}

/* The resize handle nearest canvas point `at` within its slop, which on a
   small box overlaps its neighbours'. */
static int handle_near(const gizmo_frame *frame, CF_V2 at, float pixel)
{
    int part = SCENE_GIZMO_NONE;
    float best = HANDLE / 2 + SLOP;
    for (int h = 0; frame->sized && h < 8; h++) {
        float distance = cf_len(cf_sub(at, handle_at(frame, h, pixel))) / pixel;
        if (distance <= best) {
            best = distance;
            part = SCENE_GIZMO_HANDLE + h;
        }
    }
    return part;
}

bool scene_gizmo_hint(app_scene_authoring *scene, scene_tool_hint *hint)
{
    scene_tool tool = scene->gizmo.in.tool;
    engine_viewport *viewport = scene->render_viewport;
    if ((tool != SCENE_TOOL_ROTATE && tool != SCENE_TOOL_SCALE) || !viewport ||
        engine_viewport_perspective(viewport) || !scene_can_move(scene)) {
        return false;
    }
    scene_gizmo_target *targets = selection_targets(scene);
    gizmo_frame frame;
    bool grey = frame_of(targets, &frame) && !tool_enabled(tool, &frame);
    afree(targets);
    if (!grey) {
        return false;
    }
    engine_viewport_camera camera;
    engine_viewport_get_camera(viewport, &camera);
    *hint = (scene_tool_hint){
        .missing = tool == SCENE_TOOL_ROTATE ? &COMPONENT_Orientation : &COMPONENT_Appearance,
        .x = (frame.pivot.x - camera.center_x) / camera.span_width + 0.5f,
        .y = (camera.center_y - frame.pivot.y) / camera.span_height + 0.5f,
    };
    return true;
}

/* The gizmo's part at canvas point `at` for a canvas of `pixel` units per
   pixel. */
static int part_at(scene_tool tool, const gizmo_frame *frame, CF_V2 at, float pixel)
{
    CF_V2 d = cf_mul_v2_f(cf_sub(at, frame->pivot), 1 / pixel);
    switch (tool) {
        case SCENE_TOOL_MOVE:
            if (fabsf(d.x) <= SQUARE / 2 + 1 && fabsf(d.y) <= SQUARE / 2 + 1) {
                return SCENE_GIZMO_FREE;
            }
            if (d.x >= 0 && d.x <= ARROW_LENGTH + SLOP && fabsf(d.y) <= SLOP) {
                return SCENE_GIZMO_AXIS_X;
            }
            if (d.y <= 0 && -d.y <= ARROW_LENGTH + SLOP && fabsf(d.x) <= SLOP) {
                return SCENE_GIZMO_AXIS_Y;
            }
            return SCENE_GIZMO_NONE;
        case SCENE_TOOL_ROTATE:
            return frame->oriented && fabsf(cf_len(d) - RING_RADIUS) <= RING_SLOP
                     ? SCENE_GIZMO_RING
                     : SCENE_GIZMO_NONE;
        case SCENE_TOOL_SCALE: return handle_near(frame, at, pixel);
        default: return SCENE_GIZMO_NONE;
    }
}

/* ---------- Gestures ---------- */

int scene_gizmo_record(
    app_scene_authoring *scene,
    ecs_entity entity,
    const ecs_component_desc *component,
    const char *name
)
{
    if (!ecs_component_has(scene->world, entity, component->id)) {
        return -1;
    }
    scene_gizmo_field field = { .entity = entity,
                                .component = component->id,
                                .field = field_named(component, name) };
    pin_value value = { 0 };
    int read = ecs_component_read_field(
        NULL,
        scene->world,
        entity,
        component->id,
        field.field,
        &value
    );
    bool copied = read == ECS_OK && scene_property_value_copy(&field.before, value) &&
                  scene_property_value_copy(&field.after, value);
    assert(copied);
    apush(scene->gizmo.fields, field);
    return acount(scene->gizmo.fields) - 1;
}

pin_value scene_gizmo_started(const app_scene_authoring *scene, int index)
{
    return scene->gizmo.fields[index].before;
}

void scene_gizmo_write(app_scene_authoring *scene, int index, pin_value value)
{
    scene_gizmo_field *field = &scene->gizmo.fields[index];
    assert(value.type == field->after.type);
    if (value.type == PIN_STRUCT) {
        memcpy(
            field->after.ptr,
            value.ptr,
            type_catalog_find(field->after.type_name)->size
        );
    } else {
        field->after.f = value.f;
    }
    if (scene_collab_can_write_locally()) {
        ecs_component_write_field(
            scene->world,
            field->entity,
            field->component,
            field->field,
            field->after
        );
        /* A paused run's objects change only its world, which Stop replaces. */
        scene->scene_dirty |= !scene->runtime_snapshot_valid;
    }
}

static bool field_changed(const scene_gizmo_field *field)
{
    if (field->after.type != PIN_STRUCT) {
        return field->after.f != field->before.f;
    }
    size_t size = type_catalog_find(field->after.type_name)->size;
    return memcmp(field->after.ptr, field->before.ptr, size) != 0;
}

static void free_fields(scene_gizmo_field *fields)
{
    for (int i = 0; i < acount(fields); i++) {
        scene_property_value_free(&fields[i].before);
        scene_property_value_free(&fields[i].after);
    }
    afree(fields);
}

static void add_field(
    app_scene_authoring *scene,
    scene_gizmo_target *target,
    int slot,
    const ecs_component_desc *component,
    const char *name
)
{
    target->field[slot] = scene_gizmo_record(scene, target->entity, component, name);
}

/* The fields dragging `part` writes on `target`. */
static void add_fields(app_scene_authoring *scene, scene_gizmo_target *target, int part)
{
    add_field(scene, target, GIZMO_FIELD_X, &COMPONENT_Transform, "x");
    add_field(scene, target, GIZMO_FIELD_Y, &COMPONENT_Transform, "y");
    if (part == SCENE_GIZMO_RING) {
        add_field(scene, target, FIELD_RADIANS, &COMPONENT_Orientation, "radians");
    }
    if (part < SCENE_GIZMO_HANDLE || target->size.x <= 0) {
        return;
    }
    const char *height;
    const ecs_component_desc *size = sized_component(scene->world, target->entity, &height);
    add_field(scene, target, FIELD_WIDTH, size, "width");
    add_field(scene, target, FIELD_HEIGHT, size, height);
    /* Collision follows the visual it was made for. */
    add_field(scene, target, FIELD_COLLIDER_WIDTH, &COMPONENT_Collider, "width");
    add_field(scene, target, FIELD_COLLIDER_HEIGHT, &COMPONENT_Collider, "height");
    add_field(scene, target, FIELD_COLLIDER_RADIUS, &COMPONENT_Collider, "radius");
}

static float started(const scene_gizmo *g, const scene_gizmo_target *target, int slot)
{
    return g->fields[target->field[slot]].before.f;
}

static void set(app_scene_authoring *scene, const scene_gizmo_target *target, int slot, float value)
{
    int index = target->field[slot];
    if (index >= 0 && isfinite(value)) {
        scene_gizmo_write(scene, index, (pin_value){ .type = PIN_F32, .f = value });
    }
}

/* Moves the target to canvas point `at`, written in its parent's frame. */
static void place(app_scene_authoring *scene, const scene_gizmo_target *target, CF_V2 at)
{
    CF_V2 from_parent = { at.x - target->parent.position.x,
                          -at.y - target->parent.position.y };
    float c = cosf(target->parent.radians), s = sinf(target->parent.radians);
    set(scene, target, GIZMO_FIELD_X, c * from_parent.x + s * from_parent.y);
    set(scene, target, GIZMO_FIELD_Y, c * from_parent.y - s * from_parent.x);
}

bool scene_gizmo_snapping(const scene_gizmo *g)
{
    return g->in.snap != g->in.ctrl && g->in.increment > 0;
}

/* Numbers as the status bar shows them: at most two decimals, no zeros. */
static float shown(float value)
{
    float rounded = roundf(value * 100) / 100;
    return rounded == 0 ? 0 : rounded;
}

/* The status bar's line for the change so far: what the gesture does and
   its delta, which the label by the pointer shows alone. */
void scene_gizmo_describe(scene_gizmo *g, const char *verb)
{
    snprintf(g->status, sizeof g->status, "%s %s", verb, g->delta);
}

/* The gesture reports nothing more until it changes something again. */
static void quiet(scene_gizmo *g)
{
    g->status[0] = g->delta[0] = '\0';
}

void scene_gizmo_begin(app_scene_authoring *scene, scene_gizmo_part part, CF_V2 at)
{
    finish_transform_edit(scene, false);
    scene_gizmo *g = &scene->gizmo;
    scene_gizmo_target *targets = selection_targets(scene);
    gizmo_frame frame;
    if (!frame_of(targets, &frame)) {
        afree(targets);
        return;
    }
    g->targets = targets;
    g->active = part;
    g->modal = g->moved = false;
    g->axis = SCENE_GIZMO_FREE;
    g->reselect = -1;
    g->primary = 0;
    ecs_entity active = selected_scene_entity(scene);
    for (int i = 0; i < acount(targets); i++) {
        g->primary = ecs_entity_to_u64(targets[i].entity) == ecs_entity_to_u64(active)
                       ? i
                       : g->primary;
    }
    g->origin = at;
    g->pivot = frame.pivot;
    g->center = frame.center;
    g->half = frame.half;
    g->angle = frame.angle;
    g->turn = 0;
    g->last = atan2f(at.y - frame.pivot.y, at.x - frame.pivot.x);
    g->grip = (CF_V2){ 0 };
    if (part >= SCENE_GIZMO_HANDLE) {
        int handle = part - SCENE_GIZMO_HANDLE;
        CF_V2 side = { HANDLE_SIDES[handle][0] * frame.half.x,
                       HANDLE_SIDES[handle][1] * frame.half.y };
        g->grip = cf_sub(side, rotated(cf_sub(at, frame.center), -frame.angle));
    }
    g->dirty = scene->scene_dirty;
    quiet(g);
    for (int i = 0; i < acount(targets); i++) {
        add_fields(scene, &targets[i], part);
    }
    scene->dragging_actor = true;
}

static void move_selection(app_scene_authoring *scene, CF_V2 at)
{
    scene_gizmo *g = &scene->gizmo;
    int axis = g->active == SCENE_GIZMO_FREE ? g->axis : g->active;
    CF_V2 d = cf_sub(at, g->origin);
    d.y = axis == SCENE_GIZMO_AXIS_X ? 0 : d.y;
    d.x = axis == SCENE_GIZMO_AXIS_Y ? 0 : d.x;
    /* The dragged object lands on the grid, and the rest keep their places
       around it. */
    if (scene_gizmo_snapping(g)) {
        CF_V2 from = g->targets[g->primary].position;
        d.x = axis == SCENE_GIZMO_AXIS_Y
                ? 0
                : scene_snap(from.x + d.x, g->in.increment) - from.x;
        d.y = axis == SCENE_GIZMO_AXIS_X
                ? 0
                : scene_snap(from.y + d.y, g->in.increment) - from.y;
    }
    for (int i = 0; i < acount(g->targets); i++) {
        place(scene, &g->targets[i], cf_add(g->targets[i].position, d));
    }
    snprintf(g->delta, sizeof g->delta, "\xce\x94x %g  \xce\x94y %g", shown(d.x), shown(-d.y));
    scene_gizmo_describe(g, "Move");
}

static void rotate_selection(app_scene_authoring *scene, CF_V2 at)
{
    scene_gizmo *g = &scene->gizmo;
    CF_V2 from_pivot = cf_sub(at, g->pivot);
    if (cf_len(from_pivot) > 0) {
        float angle = atan2f(from_pivot.y, from_pivot.x);
        float step = remainderf(angle - g->last, 2 * CF_PI);
        g->turn += step;
        g->last = angle;
    }
    /* Gameplay radians run from +X toward +Y, clockwise on screen. */
    float delta = -g->turn;
    const scene_gizmo_target *primary = &g->targets[g->primary];
    if (scene_gizmo_snapping(g)) {
        float from = primary->oriented ? -primary->angle : 0;
        delta = scene_snap(from + delta, SCENE_ROTATION_STEP) - from;
    }
    for (int i = 0; i < acount(g->targets); i++) {
        const scene_gizmo_target *t = &g->targets[i];
        place(scene, t, cf_add(g->pivot, rotated(cf_sub(t->position, g->pivot), -delta)));
        set(scene, t, FIELD_RADIANS, -t->angle + delta - t->parent.radians);
    }
    snprintf(g->delta, sizeof g->delta, "%g\xc2\xb0", roundf(delta * 1800 / CF_PI) / 10);
    scene_gizmo_describe(g, "Rotate");
}

/* The box's new size along one axis for the handle at `edge` on `side`
   (-1 or 1), from the opposite side or, keeping the center, both. */
static float resized(const scene_gizmo *g, int axis, int side, float edge, float anchor)
{
    bool center = g->in.alt;
    bool aligned = fabsf(g->angle) < 1e-4f;
    float origin = axis ? g->center.y : g->center.x;
    if (scene_gizmo_snapping(g) && aligned && !center) {
        edge = scene_snap(origin + edge, g->in.increment) - origin;
    }
    float size = center ? 2 * edge * (float)side : (edge - anchor) * (float)side;
    if (scene_gizmo_snapping(g) && (!aligned || center)) {
        size = scene_snap(size, g->in.increment);
    }
    return fmaxf(size, MIN_SIZE);
}

static void resize_selection(app_scene_authoring *scene, CF_V2 at)
{
    scene_gizmo *g = &scene->gizmo;
    int handle = g->active - SCENE_GIZMO_HANDLE;
    int side_x = HANDLE_SIDES[handle][0], side_y = HANDLE_SIDES[handle][1];
    CF_V2 edge = cf_add(rotated(cf_sub(at, g->center), -g->angle), g->grip);
    CF_V2 anchor = g->in.alt ? (CF_V2){ 0 }
                             : (CF_V2){ -side_x * g->half.x, -side_y * g->half.y };
    CF_V2 factor = { 1, 1 };
    if (side_x) {
        factor.x = resized(g, 0, side_x, edge.x, anchor.x) / (2 * g->half.x);
    }
    if (side_y) {
        factor.y = resized(g, 1, side_y, edge.y, anchor.y) / (2 * g->half.y);
    }
    /* Shift keeps the aspect ratio, by the axis that changed most. */
    if (g->in.shift) {
        float uniform = side_x && side_y
                          ? (fabsf(factor.x - 1) > fabsf(factor.y - 1) ? factor.x
                                                                       : factor.y)
                          : (side_x ? factor.x : factor.y);
        factor = (CF_V2){ uniform, uniform };
    }
    for (int i = 0; i < acount(g->targets); i++) {
        const scene_gizmo_target *t = &g->targets[i];
        CF_V2 local = rotated(cf_sub(t->position, g->center), -g->angle);
        local = cf_add(anchor, cf_mul_v2(cf_sub(local, anchor), factor));
        place(scene, t, cf_add(g->center, rotated(local, g->angle)));
        set(scene, t, FIELD_WIDTH, t->size.x * factor.x);
        set(scene, t, FIELD_HEIGHT, t->size.y * factor.y);
        if (t->field[FIELD_COLLIDER_WIDTH] >= 0) {
            set(scene,
                t,
                FIELD_COLLIDER_WIDTH,
                started(g, t, FIELD_COLLIDER_WIDTH) * factor.x);
            set(scene,
                t,
                FIELD_COLLIDER_HEIGHT,
                started(g, t, FIELD_COLLIDER_HEIGHT) * factor.y);
            /* A circle grows with the larger factor, as Unity scales one. */
            set(scene,
                t,
                FIELD_COLLIDER_RADIUS,
                started(g, t, FIELD_COLLIDER_RADIUS) * fmaxf(factor.x, factor.y));
        }
    }
    if (acount(g->targets) == 1) {
        const scene_gizmo_target *t = &g->targets[0];
        snprintf(
            g->delta,
            sizeof g->delta,
            "%g \xc3\x97 %g",
            shown(t->size.x * factor.x),
            shown(t->size.y * factor.y)
        );
        scene_gizmo_describe(g, "Size");
        return;
    }
    snprintf(
        g->delta,
        sizeof g->delta,
        "%g%% \xc3\x97 %g%%",
        shown(factor.x * 100),
        shown(factor.y * 100)
    );
    scene_gizmo_describe(g, "Scale");
}

void scene_gizmo_update(app_scene_authoring *scene, CF_V2 at)
{
    scene_gizmo *g = &scene->gizmo;
    switch (g->active) {
        case SCENE_GIZMO_NONE: return;
        case SCENE_GIZMO_FREE:
        case SCENE_GIZMO_AXIS_X:
        case SCENE_GIZMO_AXIS_Y: move_selection(scene, at); return;
        case SCENE_GIZMO_RING: rotate_selection(scene, at); return;
        default: resize_selection(scene, at); return;
    }
}

/* Records one changed field as Properties does, collaboration included. */
static void commit_field(app_scene_authoring *scene, const scene_gizmo_field *field, bool dirty)
{
    scene->property_actor = field->entity;
    scene->property_edit = (scene_field_edit){ .component = field->component,
                                               .field = field->field };
    bool copied = scene_property_value_copy(&scene->property_edit.before, field->before) &&
                  scene_property_value_copy(&scene->property_edit.after, field->after);
    assert(copied);
    scene->property_dirty = dirty;
    finish_property_edit(scene);
}

void scene_gizmo_finish(app_scene_authoring *scene, bool cancel)
{
    scene_gizmo *g = &scene->gizmo;
    if (!g->active) {
        return;
    }
    scene_gizmo_field *fields = g->fields;
    g->fields = NULL;
    afree(g->targets);
    g->targets = NULL;
    afree(g->space.targets);
    g->active = SCENE_GIZMO_NONE;
    g->modal = false;
    scene->dragging_actor = false;
    int changed = 0;
    for (int i = 0; i < acount(fields); i++) {
        changed += field_changed(&fields[i]);
    }
    if (cancel || !changed) {
        for (int i = 0; scene_collab_can_write_locally() && i < acount(fields); i++) {
            ecs_component_write_field(
                scene->world,
                fields[i].entity,
                fields[i].component,
                fields[i].field,
                fields[i].before
            );
        }
        scene->scene_dirty = g->dirty;
        quiet(g);
        free_fields(fields);
        return;
    }
    /* A paused run's objects keep their new values in its world; nothing
       records them, as Properties' live edits record nothing. */
    if (scene->runtime_snapshot_valid) {
        snprintf(g->report, sizeof g->report, "%s", g->status);
        quiet(g);
        free_fields(fields);
        return;
    }
    if (changed > 1) {
        scene_history_begin_group(scene->history);
    }
    for (int i = 0; i < acount(fields); i++) {
        if (field_changed(&fields[i])) {
            commit_field(scene, &fields[i], g->dirty);
        }
    }
    if (changed > 1) {
        scene_history_end_group(scene->history);
    }
    snprintf(g->report, sizeof g->report, "%s", g->status);
    quiet(g);
    free_fields(fields);
}

/* ---------- Pointer ---------- */

static void drag_frame(app_scene_authoring *scene, const engine_viewport_pointer *pointer)
{
    scene_gizmo *g = &scene->gizmo;
    if (pointer->dragging && pointer->grounded) {
        g->moved = true;
        scene_gizmo_update(scene, (CF_V2){ pointer->world_x, pointer->world_y });
    }
    if (!pointer->released) {
        return;
    }
    int reselect = g->moved ? -1 : g->reselect;
    scene_gizmo_finish(scene, false);
    if (reselect >= 0) {
        select_instance(scene, reselect);
    }
}

void scene_gizmo_drag_object(app_scene_authoring *scene, int index, const engine_viewport_pointer *pointer)
{
    /* Unity's: dragging one of several selected objects moves them all, and
       a click without a drag selects it alone. */
    bool joined = scene_instance_selected(scene, index) &&
                  scene_selection_count(scene) > 1;
    if (joined) {
        finish_transform_edit(scene, false);
        scene->selected = index;
    } else {
        select_instance(scene, index);
    }
    scene_gizmo_begin(
        scene,
        SCENE_GIZMO_FREE,
        (CF_V2){ pointer->world_x, pointer->world_y }
    );
    scene->gizmo.reselect = joined ? index : -1;
    /* A click may press and release within one frame. */
    drag_frame(scene, pointer);
}

/* The part under canvas point `at`, with the box's angle for its cursor. */
static int hovered_part(app_scene_authoring *scene, CF_V2 at, float pixel)
{
    if (scene->gizmo.in.tool == SCENE_TOOL_SELECT) {
        return SCENE_GIZMO_NONE;
    }
    scene_gizmo_target *targets = selection_targets(scene);
    gizmo_frame frame;
    int part = SCENE_GIZMO_NONE;
    if (frame_of(targets, &frame)) {
        part = part_at(scene->gizmo.in.tool, &frame, at, pixel);
        scene->gizmo.tilt = frame.angle;
    }
    afree(targets);
    return part;
}

/* A modal move follows the pointer until a click or Enter; X and Y
   constrain it, and pressing one again frees it. */
static void modal_frame(app_scene_authoring *scene, const engine_viewport_pointer *pointer)
{
    scene_gizmo *g = &scene->gizmo;
    if (g->in.axis_x) {
        g->axis = g->axis == SCENE_GIZMO_AXIS_X ? SCENE_GIZMO_FREE : SCENE_GIZMO_AXIS_X;
    }
    if (g->in.axis_y) {
        g->axis = g->axis == SCENE_GIZMO_AXIS_Y ? SCENE_GIZMO_FREE : SCENE_GIZMO_AXIS_Y;
    }
    if (pointer->world_per_pixel > 0 && pointer->grounded) {
        g->moved = true;
        scene_gizmo_update(scene, (CF_V2){ pointer->world_x, pointer->world_y });
    }
    if (pointer->pressed || g->in.confirm) {
        scene_gizmo_finish(scene, false);
    }
}

bool scene_gizmo_pointer(app_scene_authoring *scene, engine_viewport *viewport, const engine_viewport_pointer *pointer)
{
    scene_gizmo *g = &scene->gizmo;
    bool grab = g->grab;
    g->grab = false;
    CF_V2 at = { pointer->world_x, pointer->world_y };
    if (g->active && g->in.cancel) {
        scene_gizmo_finish(scene, true);
        return true;
    }
    if (g->active) {
        if (g->modal) {
            modal_frame(scene, pointer);
        } else if (scene_gizmo_spatial(g->active)) {
            scene_gizmo3d_pointer(scene, pointer);
        } else {
            drag_frame(scene, pointer);
        }
        return true;
    }
    bool valid = pointer->world_per_pixel > 0 && pointer->grounded;
    if (grab && valid) {
        scene_gizmo_begin(scene, SCENE_GIZMO_FREE, at);
        g->modal = g->active != SCENE_GIZMO_NONE;
        return true;
    }
    if (engine_viewport_perspective(viewport)) {
        return scene_gizmo3d_pointer(scene, pointer);
    }
    g->hover = valid ? hovered_part(scene, at, pointer->world_per_pixel)
                     : SCENE_GIZMO_NONE;
    if (!pointer->pressed || g->hover == SCENE_GIZMO_NONE) {
        return false;
    }
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    scene_gizmo_begin(scene, g->hover, at);
    drag_frame(scene, pointer);
    return true;
}

const char *scene_gizmo_part_cursor(int part, float angle)
{
    switch (part) {
        case SCENE_GIZMO_NONE: return NULL;
        case SCENE_GIZMO_FREE: return "move";
        case SCENE_GIZMO_AXIS_X: return "ew-resize";
        case SCENE_GIZMO_AXIS_Y: return "ns-resize";
        case SCENE_GIZMO_RING: return "pointer";
        default: break;
    }
    if (scene_gizmo_spatial(part)) {
        return part < SCENE_GIZMO_RING_X ? "move" : "pointer";
    }
    /* The handle's side of the box, on screen, to the nearest 45 degrees. */
    static const char *const RESIZE[] = { "ew-resize", "nesw-resize", "ns-resize", "nwse-resize" };
    int handle = part - SCENE_GIZMO_HANDLE;
    assert(handle >= 0 && handle < 8);
    CF_V2 out = rotated((CF_V2){ HANDLE_SIDES[handle][0], HANDLE_SIDES[handle][1] }, angle);
    int octant = (int)lroundf(atan2f(out.y, out.x) / (CF_PI / 4));
    return RESIZE[(octant % 4 + 4) % 4];
}

const char *scene_gizmo_cursor(const app_scene_authoring *scene)
{
    const scene_gizmo *g = &scene->gizmo;
    return g->active != SCENE_GIZMO_NONE
             ? scene_gizmo_part_cursor(g->active, g->angle)
             : scene_gizmo_part_cursor(g->hover, g->tilt);
}

/* ---------- Drawing ---------- */

CF_Color scene_gizmo_color(const scene_gizmo *g, int part, unsigned color)
{
    if (g->active == part) {
        return packed(SCENE_GIZMO_DRAGGED_COLOR);
    }
    CF_Color base = packed(color);
    return g->hover == part && !g->active
             ? cf_color_lerp(base, cf_color_white(), 0.45f)
             : base;
}

static void draw_move(const scene_gizmo *g, CF_V2 pivot, float pixel)
{
    float inner = SQUARE / 2 * pixel, length = ARROW_LENGTH * pixel;
    cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_AXIS_X, SCENE_GIZMO_X_COLOR));
    cf_draw_arrow(
        (CF_V2){ pivot.x + inner, pivot.y },
        (CF_V2){ pivot.x + length, pivot.y },
        LINE * pixel,
        ARROW_HEAD * pixel
    );
    cf_draw_pop_color();
    /* Gameplay +Y points down the screen. */
    cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_AXIS_Y, SCENE_GIZMO_Y_COLOR));
    cf_draw_arrow(
        (CF_V2){ pivot.x, pivot.y - inner },
        (CF_V2){ pivot.x, pivot.y - length },
        LINE * pixel,
        ARROW_HEAD * pixel
    );
    cf_draw_pop_color();
    CF_Aabb square = cf_make_aabb_center_half_extents(pivot, (CF_V2){ inner, inner });
    CF_Color free = scene_gizmo_color(g, SCENE_GIZMO_FREE, FREE_COLOR);
    CF_Color fill = free;
    fill.a *= 0.35f;
    cf_draw_push_color(fill);
    cf_draw_quad_fill(square, 0);
    cf_draw_pop_color();
    cf_draw_push_color(free);
    cf_draw_quad(square, pixel, 0);
    cf_draw_pop_color();
}

static void draw_ring(const scene_gizmo *g, const gizmo_frame *frame, float pixel)
{
    float radius = RING_RADIUS * pixel;
    if (!frame->oriented) {
        cf_draw_push_color(packed(SCENE_GIZMO_DISABLED_COLOR));
        cf_draw_circle2(frame->pivot, radius, LINE * pixel);
        cf_draw_pop_color();
        return;
    }
    cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_RING, FREE_COLOR));
    cf_draw_circle2(frame->pivot, radius, (g->hover == SCENE_GIZMO_RING ? 3 : LINE) * pixel);
    cf_draw_pop_color();
    if (g->active != SCENE_GIZMO_RING) {
        return;
    }
    /* Spokes from where the turn began to where it is. */
    cf_draw_push_color(packed(SCENE_GIZMO_DRAGGED_COLOR));
    for (int spoke = 0; spoke < 2; spoke++) {
        float angle = g->last - (spoke ? 0 : g->turn);
        CF_V2 end = cf_add(frame->pivot, rotated((CF_V2){ radius, 0 }, angle));
        cf_draw_line(frame->pivot, end, pixel);
    }
    cf_draw_pop_color();
}

static void draw_box(const scene_gizmo *g, const gizmo_frame *frame, float pixel)
{
    CF_V2 corners[4];
    for (int c = 0; c < 4; c++) {
        corners[c] = handle_at(frame, 2 * c, pixel);
    }
    cf_draw_push_color(packed(frame->sized ? SCENE_ACTIVE_COLOR : SCENE_GIZMO_DISABLED_COLOR));
    cf_draw_polyline(corners, 4, pixel, true);
    cf_draw_pop_color();
    for (int h = 0; frame->sized && h < 8; h++) {
        CF_V2 at = handle_at(frame, h, pixel);
        CF_V2 quad[4];
        for (int c = 0; c < 4; c++) {
            CF_V2 corner = { (c == 1 || c == 2 ? 0.5f : -0.5f) * HANDLE * pixel,
                             (c >= 2 ? 0.5f : -0.5f) * HANDLE * pixel };
            quad[c] = cf_add(at, rotated(corner, frame->angle));
        }
        cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_HANDLE + h, HANDLE_FILL));
        cf_draw_quad_fill2(quad[0], quad[1], quad[2], quad[3], 0);
        cf_draw_pop_color();
        cf_draw_push_color(packed(SCENE_ACTIVE_COLOR));
        cf_draw_quad2(quad[0], quad[1], quad[2], quad[3], pixel, 0);
        cf_draw_pop_color();
    }
}

/* A modal move's constraint as a line through the selection across the view. */
static void draw_constraint(const app_scene_authoring *scene, float pixel)
{
    const scene_gizmo *g = &scene->gizmo;
    if (!g->modal || g->axis == SCENE_GIZMO_FREE) {
        return;
    }
    engine_viewport_camera camera;
    engine_viewport_get_camera(scene->render_viewport, &camera);
    float reach = camera.span_width + camera.span_height;
    CF_V2 at = g->targets[g->primary].position;
    bool x = g->axis == SCENE_GIZMO_AXIS_X;
    CF_V2 along = x ? (CF_V2){ reach, 0 } : (CF_V2){ 0, reach };
    cf_draw_push_color(packed(x ? SCENE_GIZMO_X_COLOR : SCENE_GIZMO_Y_COLOR));
    cf_draw_line(cf_sub(at, along), cf_add(at, along), pixel);
    cf_draw_pop_color();
}

void scene_gizmo_draw(app_scene_authoring *scene)
{
    const scene_gizmo *g = &scene->gizmo;
    float pixel = scene->selection_scale > 0 ? scene->selection_scale : 1;
    draw_constraint(scene, pixel);
    if (g->in.tool == SCENE_TOOL_SELECT) {
        return;
    }
    scene_gizmo_target *targets = selection_targets(scene);
    gizmo_frame frame;
    if (frame_of(targets, &frame)) {
        switch (g->in.tool) {
            case SCENE_TOOL_MOVE: draw_move(g, frame.pivot, pixel); break;
            case SCENE_TOOL_ROTATE: draw_ring(g, &frame, pixel); break;
            case SCENE_TOOL_SCALE: draw_box(g, &frame, pixel); break;
            default: break;
        }
    }
    afree(targets);
}

float scene_gizmo_grid(const app_scene_authoring *scene)
{
    const scene_tool_input *in = &scene->gizmo.in;
    bool shown = in->grid && in->increment > 0 && scene_can_move(scene) &&
                 !scene->runtime_snapshot_valid && scene->render_viewport &&
                 !engine_viewport_perspective(scene->render_viewport);
    return shown ? in->increment : 0;
}
