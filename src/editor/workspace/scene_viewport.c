#include "authoring/ui_events_adapter.h"
#include "authoring/ui_loader.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/model_preview.h"
#include "editor/workspace/scene_internal.h"
#include "editor/workspace/viewport.h"
#include "platform/cute.h"
#include "render/appearance_geometry.h"
#include "render/geometry_2d.h"
#include "render/grid_2d.h"
#include "render/mesh_surface.h"
#include "render/scene3d.h"
#include "render/shader_catalog.h"
#include "render/ui.h"
#include "runtime/camera.h"
#include "runtime/component_catalog.h"
#include "runtime/tilemap.h"
#include "runtime/transform_propagation.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Where the viewport shows an entity: its exact world placement, since edits
 * and loads move it before any engine frame propagates them. The angle turns
 * the other way, as the viewport's y points down. */
static GameGlobalTransform2D actor_pose(const ecs_world *world, ecs_entity entity)
{
    GameGlobalTransform2D pose = game_transform_world_2d(world, entity);
    pose.radians = -pose.radians;
    return pose;
}

static render_scene3d_camera camera3d(engine_viewport *viewport)
{
    render_scene3d_camera camera;
    engine_viewport_get_camera3d(viewport, &camera);
    return camera;
}

static bool appearance_bounds(
    const GameTransform *transform,
    const GameAppearance *appearance,
    const GameShader *shader,
    float radians,
    bool effects,
    CF_Aabb *out
)
{
    CF_V2 min, max;
    GameAppearance bounded = *appearance;
    float extent[2];
    if (shader && render_shader_bounds(render_shader_find(shader->shape), extent)) {
        bounded.shape = GAME_SHAPE_BOX;
        bounded.width = 2 * (shader->bounds_x > 0 ? shader->bounds_x : extent[0]);
        bounded.height = 2 * (shader->bounds_y > 0 ? shader->bounds_y : extent[1]);
        appearance = &bounded;
    }
    if (!isfinite(transform->x) || !isfinite(transform->y) ||
        !game_appearance_rotated_bounds(appearance, radians, &min, &max)) {
        return false;
    }
    float padding = 0;
    if (effects) {
        if (isfinite(appearance->glow_radius) && appearance->glow_radius > 0) {
            padding += appearance->glow_radius;
        }
    }
    *out = (CF_Aabb){
        { transform->x + min.x - padding, -transform->y + min.y - padding },
        { transform->x + max.x + padding, -transform->y + max.y + padding }
    };
    return isfinite(out->min.x) && isfinite(out->min.y) &&
           isfinite(out->max.x) && isfinite(out->max.y);
}

static bool visual_bounds(const ecs_world *world, ecs_entity entity, bool effects, CF_Aabb *bounds)
{
    const GameTilemap *map = GAME_GameTilemap_get_const(world, entity);
    if (map) {
        GameGlobalTransform2D shown = game_transform_world_2d(world, entity);
        CF_V2 min, max;
        if (!tilemap_bounds(
                map->columns,
                map->rows,
                map->frame_width,
                map->frame_height,
                (CF_V2){ shown.position.x, shown.position.y },
                shown.radians,
                &min,
                &max
            )) {
            return false;
        }
        *bounds = (CF_Aabb){ { min.x, -max.y }, { max.x, -min.y } };
        return true;
    }
    const GameMeshSurface2D *mesh = GAME_GameMeshSurface2D_get_const(world, entity);
    const GameSdfSurface3D *sdf = GAME_GameSdfSurface3D_get_const(world, entity);
    if ((mesh && mesh->enabled) || (sdf && sdf->enabled)) {
        CF_V2 min, max;
        char error[128];
        if (!render_geometry_2d_mesh_bounds(world, entity, NULL, &min, &max, error, sizeof error)) {
            return false;
        }
        *bounds = (CF_Aabb){ { min.x, -max.y }, { max.x, -min.y } };
        return true;
    }
    const GameAppearance *appearance = GAME_GameAppearance_get_const(world, entity);
    if (!appearance || !GAME_GameTransform_get_const(world, entity)) {
        return false;
    }
    GameGlobalTransform2D pose = actor_pose(world, entity);
    return appearance_bounds(
        &pose.position,
        appearance,
        GAME_GameShader_get_const(world, entity),
        pose.radians,
        effects,
        bounds
    );
}

static bool tilemap_hit(const ecs_world *world, ecs_entity entity, CF_V2 point)
{
    const GameTilemap *map = GAME_GameTilemap_get_const(world, entity);
    GameGlobalTransform2D shown = game_transform_world_2d(world, entity);
    CF_V2 min, max;
    if (!tilemap_bounds(
            map->columns,
            map->rows,
            map->frame_width,
            map->frame_height,
            (CF_V2){ shown.position.x, shown.position.y },
            shown.radians,
            &min,
            &max
        )) {
        return false;
    }
    float dx = point.x - shown.position.x, dy = point.y - shown.position.y;
    float c = cosf(shown.radians), s = sinf(shown.radians);
    CF_V2 local = { c * dx + s * dy, c * dy - s * dx };
    return local.x >= 0 && local.y >= 0 &&
           local.x < (float)map->columns * map->frame_width &&
           local.y < (float)map->rows * map->frame_height;
}

/* Whether `entity` is among the `count` in `only`, or `only` is NULL. */
static bool fits(const ecs_entity *only, int count, ecs_entity entity)
{
    for (int i = 0; only && i < count; i++) {
        if (ecs_entity_to_u64(only[i]) == ecs_entity_to_u64(entity)) {
            return true;
        }
    }
    return !only;
}

static bool fit_arena_view(engine_viewport *viewport, ecs_world *world, const ecs_entity *only, int count)
{
    ecs_component_id required[] = { GAME_COMPONENT_GameTransform.id,
                                    GAME_COMPONENT_GameGeometry3D.id };
    ecs_query query;
    if (ecs_query_begin(world, &query, required, 2) != ECS_OK) {
        return false;
    }
    CF_V3 *points = NULL;
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        CF_V3 corners[8];
        if (!fits(only, count, entity) ||
            !render_scene3d_entity_bounds(world, entity, corners)) {
            continue;
        }
        for (int i = 0; i < 8; i++) {
            apush(points, corners[i]);
        }
    }
    ecs_query_end(&query);
    if (acount(points)) {
        engine_viewport_camera camera;
        engine_viewport_get_camera(viewport, &camera);
        CF_Aabb fit = render_scene3d_fit(
            points,
            acount(points),
            camera.span_width / camera.span_height
        );
        engine_viewport_view view = { (fit.min.x + fit.max.x) * 0.5f,
                                      (fit.min.y + fit.max.y) * 0.5f,
                                      fit.max.x - fit.min.x,
                                      fit.max.y - fit.min.y };
        engine_viewport_set_view(viewport, &view);
    }
    bool found = acount(points) > 0;
    afree(points);
    return found;
}

bool fit_scene_view(engine_viewport *viewport, ecs_world *world, const ecs_entity *only, int count)
{
    if (engine_viewport_perspective(viewport)) {
        return fit_arena_view(viewport, world, only, count);
    }
    ecs_component_id required[] = { GAME_COMPONENT_GameTransform.id };
    ecs_query query;
    if (ecs_query_begin(world, &query, required, 1) != ECS_OK) {
        return false;
    }
    CF_Aabb total = { 0 };
    bool found = false;
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        if (!fits(only, count, entity)) {
            continue;
        }
        const GameRenderState *render_state = GAME_GameRenderState_get_const(world, entity);
        if (render_state && render_state->hidden) {
            continue;
        }
        CF_Aabb bounds;
        if (!visual_bounds(world, entity, true, &bounds)) {
            continue;
        }
        if (!found) {
            total = bounds;
        }
        total.min.x = fminf(total.min.x, bounds.min.x);
        total.min.y = fminf(total.min.y, bounds.min.y);
        total.max.x = fmaxf(total.max.x, bounds.max.x);
        total.max.y = fmaxf(total.max.y, bounds.max.y);
        found = true;
    }
    ecs_query_end(&query);
    if (found) {
        engine_viewport_fit(
            viewport,
            total.min.x,
            total.min.y,
            total.max.x,
            total.max.y
        );
    }
    return found;
}

ecs_entity pick_scene_entity(app_scene_authoring *scene, float x, float y, render_skip skip)
{
    ecs_component_id required[] = { GAME_COMPONENT_GameTransform.id };
    ecs_query query;
    if (ecs_query_begin(scene->world, &query, required, 1) != ECS_OK) {
        return (ecs_entity){ 0 };
    }
    ecs_entity picked = { 0 };
    int picked_layer = INT_MIN;
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        const GameRenderState *state = GAME_GameRenderState_get_const(scene->world, entity);
        if ((state && state->hidden) || render_skip_has(skip, entity)) {
            continue;
        }
        const GameMeshSurface2D *mesh = GAME_GameMeshSurface2D_get_const(scene->world, entity);
        const GameAppearance *appearance = GAME_GameAppearance_get_const(scene->world, entity);
        const GameSdfSurface3D *sdf = GAME_GameSdfSurface3D_get_const(scene->world, entity);
        const GameTilemap *map = GAME_GameTilemap_get_const(scene->world, entity);
        if (map) {
            if (!tilemap_hit(scene->world, entity, (CF_V2){ x, -y })) {
                continue;
            }
        } else if ((mesh && mesh->enabled) || (sdf && sdf->enabled)) {
            render_mesh_surface surface = { 0 };
            char error[128];
            float height;
            bool hit = render_mesh_surface_build(scene->world, entity, NULL, &surface, error, sizeof error) &&
                       render_mesh_surface_pick(&surface, (CF_V2){ x, -y }, &height);
            render_mesh_surface_free(&surface);
            if (!hit) {
                continue;
            }
        } else {
            if (!appearance) {
                continue;
            }
            GameGlobalTransform2D pose = actor_pose(scene->world, entity);
            const GameTransform *transform = &pose.position;
            CF_Aabb bounds;
            float radians = pose.radians;
            CF_V2 local;
            const GameShader *shader = GAME_GameShader_get_const(scene->world, entity);
            bool custom_shape = shader &&
                                render_shader_is_shape(render_shader_find(shader->shape));
            GameAppearance selectable = *appearance;
            if (selectable.shape == GAME_SHAPE_STROKED_CHEVRON) {
                selectable.shape = GAME_SHAPE_CHEVRON;
            }
            if (!appearance_bounds(transform, appearance, shader, radians, false, &bounds) ||
                !game_appearance_rotate_point(
                    (CF_V2){ x - transform->x, y + transform->y },
                    -radians,
                    &local
                ) ||
                !(custom_shape ? render_shader_contains(
                                     shader,
                                     scene->world,
                                     entity,
                                     (float[2]){ local.x, local.y },
                                     (float[2]){ x, y },
                                     app_scene_renderer_time(scene->renderer)
                                 )
                               : game_appearance_contains(&selectable, local))) {
                continue;
            }
        }
        bool editable = game_entities_owns(scene->live->runtime->entities, entity);
        for (int i = 0; !editable && i < acount(scene->actors); i++) {
            editable = ecs_entity_to_u64(scene->actors[i].entity) ==
                       ecs_entity_to_u64(entity);
        }
        if (!editable) {
            continue;
        }
        int layer = map ? map->layer : appearance ? appearance->layer : 0;
        /* Match the renderer's authored layer, then stable entity-slot order. */
        if (picked.slot && (layer < picked_layer ||
                            (layer == picked_layer && entity.slot <= picked.slot))) {
            continue;
        }
        picked = entity;
        picked_layer = layer;
    }
    ecs_query_end(&query);
    return picked;
}

static bool selection_bounds(
    app_scene_authoring *scene,
    engine_viewport *viewport,
    ecs_entity entity,
    CF_Aabb *bounds
)
{
    if (engine_viewport_perspective(viewport)) {
        CF_V3 corners[8];
        render_scene3d_camera camera = camera3d(viewport);
        return render_scene3d_entity_bounds(scene->world, entity, corners) &&
               render_scene3d_screen_bounds(&camera, corners, bounds);
    }
    if (!visual_bounds(scene->world, entity, false, bounds)) {
        return false;
    }
    engine_viewport_camera camera;
    engine_viewport_get_camera(viewport, &camera);
    bounds->min.x = 2 * (bounds->min.x - camera.center_x) / camera.span_width;
    bounds->max.x = 2 * (bounds->max.x - camera.center_x) / camera.span_width;
    bounds->min.y = 2 * (bounds->min.y - camera.center_y) / camera.span_height;
    bounds->max.y = 2 * (bounds->max.y - camera.center_y) / camera.span_height;
    return true;
}

/* A press on empty space starts a marquee. It replaces the selection, or
   with Shift (or Mod) held adds to it, as Blender's and Unity's box select
   extend. */
static void begin_marquee(app_scene_authoring *scene, CF_V2 at)
{
    bool extend = scene->gizmo.in.extend;
    afree(scene->marquee_base);
    scene->marquee_base = NULL;
    scene->marquee_active = extend ? scene->selected : -1;
    for (int i = 0; i < acount(scene->actors); i++) {
        bool kept = extend && scene_instance_selected(scene, i);
        if (kept) {
            apush(scene->marquee_base, i);
        }
        scene->actors[i].marked = kept;
    }
    scene->selected = scene->marquee_active;
    scene->runtime_selected = (ecs_entity){ 0 };
    scene->marquee = true;
    scene->marquee_start = scene->marquee_end = at;
}

static bool in_marquee_base(const app_scene_authoring *scene, int actor)
{
    for (int i = 0; i < acount(scene->marquee_base); i++) {
        if (scene->marquee_base[i] == actor) {
            return true;
        }
    }
    return false;
}

/* Marks what the marquee touches, the last object it adds active. */
static void mark_marquee(app_scene_authoring *scene, engine_viewport *viewport, render_skip unpickable)
{
    float min_x = fminf(scene->marquee_start.x, scene->marquee_end.x);
    float max_x = fmaxf(scene->marquee_start.x, scene->marquee_end.x);
    float min_y = fminf(scene->marquee_start.y, scene->marquee_end.y);
    float max_y = fmaxf(scene->marquee_start.y, scene->marquee_end.y);
    scene->selected = scene->marquee_active;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        CF_Aabb bounds;
        bool inside = !render_skip_has(unpickable, actor->entity) &&
                      selection_bounds(scene, viewport, actor->entity, &bounds) &&
                      bounds.max.x >= min_x && bounds.min.x <= max_x &&
                      bounds.max.y >= min_y && bounds.min.y <= max_y;
        bool base = in_marquee_base(scene, i);
        actor->marked = base || inside;
        if (inside && !base) {
            scene->selected = i;
        }
    }
}

static void end_marquee(app_scene_authoring *scene)
{
    scene->marquee = scene->marquee_visible = false;
    afree(scene->marquee_base);
    scene->marquee_base = NULL;
}

/* The object under the pointer in the edit view, perspective or 2D. */
static ecs_entity pick_pointer(
    app_scene_authoring *scene,
    engine_viewport *viewport,
    const engine_viewport_pointer *pointer,
    render_skip unpickable
)
{
    if (!engine_viewport_perspective(viewport)) {
        return pick_scene_entity(scene, pointer->world_x, pointer->world_y, unpickable);
    }
    render_scene3d_camera camera = camera3d(viewport);
    return render_scene3d_pick(
        scene->world,
        &camera,
        (CF_V2){ pointer->ndc_x, pointer->ndc_y },
        unpickable
    );
}

ecs_entity scene_pick_at_pointer(app_scene_authoring *scene, engine_viewport *viewport)
{
    engine_viewport_pointer pointer;
    engine_viewport_get_pointer(viewport, &pointer);
    if (!pointer.hovered) {
        return (ecs_entity){ 0 };
    }
    render_skip unpickable = scene_view_skip(
        scene,
        APP_SCENE_VIEW_HIDDEN | APP_SCENE_VIEW_LOCKED,
        &scene->view_unpickable
    );
    return pick_pointer(scene, viewport, &pointer, unpickable);
}

static void edit_scene_pointer(app_scene_authoring *scene, engine_viewport *viewport)
{
    engine_viewport_pointer pointer;
    engine_viewport_get_pointer(viewport, &pointer);
    if (pointer.world_per_pixel > 0) {
        scene->selection_scale = pointer.world_per_pixel;
    }
    if (pointer.dropped_prototype[0] && scene_can_move(scene) &&
        !scene->runtime_snapshot_valid) {
        GameTransform position = { pointer.world_x, -pointer.world_y };
        if (add_instance_at(scene, pointer.dropped_prototype, &position) == 0) {
            scene->reveal_actor = (ecs_entity){ 0 };
        }
        return;
    }
    if (!scene_can_move(scene) || pointer.cancel) {
        end_marquee(scene);
        if (scene->dragging_actor) {
            finish_transform_edit(scene, true);
        }
        return;
    }
    /* Hidden and locked objects are left to the Outliner. */
    render_skip unpickable = scene_view_skip(
        scene,
        APP_SCENE_VIEW_HIDDEN | APP_SCENE_VIEW_LOCKED,
        &scene->view_unpickable
    );
    if (scene_gizmo_pointer(scene, viewport, &pointer)) {
        return;
    }
    if (pointer.pressed) {
        finish_transform_edit(scene, false);
        ecs_entity picked = pick_pointer(scene, viewport, &pointer, unpickable);
        scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
        if (!picked.slot) {
            begin_marquee(scene, (CF_V2){ pointer.ndc_x, pointer.ndc_y });
            return;
        }
        if (game_entities_owns(scene->live->runtime->entities, picked)) {
            app_scene_authoring_select_runtime_entity(scene, picked);
        }
        for (int i = 0; i < acount(scene->actors); i++) {
            if (ecs_entity_to_u64(scene->actors[i].entity) != ecs_entity_to_u64(picked)) {
                continue;
            }
            /* Shift (or Mod) adds an object to the selection or takes it out,
               as Unity's and Blender's Shift+click do. */
            if (scene->gizmo.in.extend) {
                scene_toggle_instance(scene, i);
            } else {
                scene_gizmo_drag_object(scene, i, &pointer);
            }
            return;
        }
        /* A paused run's spawned objects move in its world. */
        if (!GAME_GameTransform_has(scene->world, picked)) {
            return;
        }
        begin_transform_edit(scene, picked);
        scene->dragging_actor = true;
        scene->drag_origin = (GameTransform){ pointer.world_x, -pointer.world_y };
    }
    /* A paused run's spawned object follows the pointer in its world; the
       gizmo moves placed ones. */
    if (scene->dragging_actor && !scene->gizmo.active) {
        if (pointer.dragging) {
            GameTransform moved = scene_local_offset(
                scene->world,
                scene->edit_actor,
                (GameTransform){ pointer.world_x - scene->drag_origin.x,
                                 -pointer.world_y - scene->drag_origin.y }
            );
            GameTransform position = { scene->edit_before.x + moved.x,
                                       scene->edit_before.y + moved.y };
            write_transform_edit(scene, scene->edit_actor, position);
        }
        if (pointer.released) {
            finish_transform_edit(scene, false);
        }
        return;
    }
    if (!scene->marquee) {
        ecs_entity picked = pointer.hovered && !pointer.down &&
                                    scene->gizmo.hover == SCENE_GIZMO_NONE
                              ? pick_pointer(scene, viewport, &pointer, unpickable)
                              : (ecs_entity){ 0 };
        /* What a press selects or moves (above): an object with a position. */
        if (picked.slot && GAME_GameTransform_has(scene->world, picked)) {
            scene->hovered = picked;
        }
        return;
    }
    scene->marquee_end = (CF_V2){ pointer.ndc_x, pointer.ndc_y };
    scene->marquee_visible = pointer.dragging;
    if (pointer.dragging) {
        mark_marquee(scene, viewport, unpickable);
    }
    if (pointer.released) {
        end_marquee(scene);
    }
}

int app_scene_authoring_draw_viewport(app_scene_authoring *scene, engine_viewport *viewport)
{
    scene->render_viewport = viewport;
    app_runtime_target target = scene->active_runtime_target;
    scene_prototype_asset *prototype = previewed_prototype(scene);
    bool model = target == APP_RUNTIME_PREVIEW && scene->model_preview;
    bool preview = target == APP_RUNTIME_PREVIEW && (prototype || model);
    engine_viewport_set_runtime(
        viewport,
        preview ? game_prototype_preview_runtime(scene->prototype_preview)
                : scene->runtime
    );
    /* While the scene is edited, the selected object shows its UI even when it
       starts hidden; Play shows what the game would. */
    scene_actor *selected = scene->runtime_snapshot_valid ? NULL
                                                          : selected_actor(scene);
    ui_world ui = {
        .ecs = scene->world,
        .page = app_scene_authoring_page(scene),
        .model = engine_ui(scene->runtime),
        .host = authoring_ui_events(scene->live),
        .save = scene->live->runtime->save,
        .revealed = selected ? ecs_entity_to_u64(selected->entity) : 0,
        .hidden = scene_view_skip(scene, APP_SCENE_VIEW_HIDDEN, &scene->view_hidden)
    };
    if (preview) {
        game_prototype_preview *p = scene->prototype_preview;
        ui = (ui_world){ .ecs = game_prototype_preview_world(p),
                         .page = app_scene_authoring_page(scene),
                         .model = engine_ui(game_prototype_preview_runtime(p)),
                         .host = authoring_ui_events(game_prototype_preview_live(p)),
                         .save = game_prototype_preview_live(p)->runtime->save,
                         .preview = true };
    }
    authoring_viewport_set_ui(viewport, &ui, scene->content_root);
    bool perspective = scene->perspective;
    if (preview) {
        ecs_world *world = game_prototype_preview_world(scene->prototype_preview);
        ecs_entity entity = game_prototype_preview_entity(scene->prototype_preview);
        perspective = !model && GAME_GameGeometry3D_has(world, entity);
    }
    engine_viewport_set_perspective(viewport, perspective);
    /* Prototype records are append-only; indices survive array reallocations. */
    int camera_target = model   ? -2
                      : preview ? (int)(prototype - scene->prototypes) + 1
                                : 0;
    scene_camera *camera = model   ? &scene->model_camera
                         : preview ? &prototype->camera
                                   : &scene->camera;
    bool fit = false;
    if (camera_target != scene->camera_target) {
        if (camera->saved) {
            engine_viewport_set_view(viewport, &camera->view);
        } else {
            engine_viewport_reset_camera(viewport);
            fit = true;
        }
        app_scene_renderer_reset(scene->renderer);
        scene->camera_target = camera_target;
    }
    if (model) {
        if (fit) {
            scene_model_preview_frame(scene);
        }
        scene_model_preview_update(scene);
    }
    ecs_entity fit_entity = scene->reveal_actor;
    fit |= fit_entity.slot != 0;
    scene->reveal_actor = (ecs_entity){ 0 };
    /* Before the draw, so the frame that fits the camera shows it. */
    if (fit && !model) {
        fit_scene_view(
            viewport,
            preview ? game_prototype_preview_world(scene->prototype_preview)
                    : scene->world,
            fit_entity.slot ? &fit_entity : NULL,
            1
        );
    }
    if (preview && scene->preview_follow &&
        (scene->preview_playing || scene->preview_step)) {
        ecs_world *world = game_prototype_preview_world(scene->prototype_preview);
        const GameTransform *position = GAME_GameTransform_get_const(
            world,
            game_prototype_preview_entity(scene->prototype_preview)
        );
        if (position) {
            engine_viewport_view view;
            engine_viewport_get_view(viewport, &view);
            view.center_x = position->x;
            view.center_y = -position->y;
            engine_viewport_set_view(viewport, &view);
        }
    }
    if (!preview) {
        scene_camera_view_follow(scene, viewport, scene_editing(scene));
        engine_viewport_camera edited;
        engine_viewport_get_camera(viewport, &edited);
        scene_world_preview_update(scene, &edited);
    }
    engine_viewport_controls controls = {
        .session = target == APP_RUNTIME_PREVIEW ? preview : scene->runtime_snapshot_valid,
        .editing = scene_can_move(scene),
        .playing = target == APP_RUNTIME_PREVIEW ? scene->preview_playing
                                                 : scene->runtime_playing,
    };
    if (controls.session && !controls.editing && !model) {
        engine_viewport_view view;
        int authored = game_camera_view(
            preview ? game_prototype_preview_world(scene->prototype_preview)
                    : scene->world,
            preview ? game_prototype_preview_runtime(scene->prototype_preview)
                    : scene->runtime,
            &view
        );
        if (authored > 0) {
            engine_viewport_set_view(viewport, &view);
        }
        if (authored < 0) {
            snprintf(scene->message, sizeof scene->message, "GameCamera requires one active camera with positive spans.");
        }
    }
    scene_field_previews_apply(scene);
    int drawn = engine_viewport_draw(viewport, &controls);
    scene_field_previews_clear(scene);
    /* The pointer's frame below picks it again for the next draw, if it
       still rests on one. */
    scene->hovered = (ecs_entity){ 0 };
    if (model) {
        scene_model_preview_pointer(scene, viewport);
    } else {
        edit_scene_pointer(scene, viewport);
    }
    engine_viewport_get_view(viewport, &camera->view);
    camera->saved = true;
    return drawn;
}

static CF_Color packed_color(unsigned color)
{
    return cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color);
}

static void draw_particles(const game_particles *particles)
{
    const game_particle *items = game_particles_data(particles);
    for (int i = 0; i < game_particles_count(particles); i++) {
        const game_particle *p = &items[i];
        float remaining = 1.0f - p->age / p->lifetime;
        CF_Color color = cf_make_color_rgb(p->color >> 24, p->color >> 16, p->color >> 8);
        color.a = (float)(p->color & 255) / 255.0f * remaining;
        float size = 1.0f + 3.0f * remaining;
        cf_draw_push_color(color);
        cf_draw_quad_fill(cf_make_aabb_pos_w_h(cf_v2(p->position.x, -p->position.y), size, size), 0.0f);
        cf_draw_pop_color();
    }
}

static void draw_appearance(const GameAppearance *appearance, const GameTransform *transform, float radians)
{
    CF_V2 center = { transform->x, -transform->y };
    float width = appearance->width * 0.5f;
    float height = appearance->height * 0.5f;
    float border = isfinite(appearance->border_width) && appearance->border_width > 0
                     ? appearance->border_width
                     : 0;
    float glow = isfinite(appearance->glow_radius) && appearance->glow_radius > 0
                   ? appearance->glow_radius
                   : 0;
    cf_draw_push_color(packed_color(appearance->color));
    cf_draw_push_interior_border(packed_color(appearance->border_color), border);
    cf_draw_push_glow(packed_color(appearance->glow_color), glow);
    switch (appearance->shape) {
        case GAME_SHAPE_CIRCLE:
            cf_draw_circle_fill2(center, width < height ? width : height);
            break;
        case GAME_SHAPE_CAPSULE: {
            float radius = fminf(width, height);
            CF_V2 offset = width >= height ? (CF_V2){ width - radius, 0 }
                                           : (CF_V2){ 0, height - radius };
            game_appearance_rotate_point(offset, radians, &offset);
            cf_draw_capsule_fill2(
                (CF_V2){ center.x - offset.x, center.y - offset.y },
                (CF_V2){ center.x + offset.x, center.y + offset.y },
                radius
            );
            break;
        }
        case GAME_SHAPE_DIAMOND:
        case GAME_SHAPE_CHEVRON:
        case GAME_SHAPE_STROKED_CHEVRON:
        default: {
            CF_V2 local[4];
            int count = game_appearance_points(appearance, local);
            if (!count) {
                local[0] = (CF_V2){ -width, -height };
                local[1] = (CF_V2){ width, -height };
                local[2] = (CF_V2){ width, height };
                local[3] = (CF_V2){ -width, height };
                count = 4;
            }
            CF_V2 points[4];
            for (int i = 0; i < count; i++) {
                game_appearance_rotate_point(local[i], radians, &local[i]);
                points[i] = (CF_V2){ center.x + local[i].x, center.y + local[i].y };
            }
            if (appearance->shape == GAME_SHAPE_STROKED_CHEVRON) {
                cf_draw_polyline(points, count, appearance->stroke_width, true);
            } else {
                cf_draw_polygon_fill(points, count, 0);
            }
            break;
        }
    }
    cf_draw_pop_glow();
    cf_draw_pop_outline();
    cf_draw_pop_color();
}

/* Whether the object under the pointer shows its hover outline: while the
   scene is edited, not under a dragged asset's drop outline. */
static bool hover_shown(const app_scene_authoring *scene, const ecs_world *world, render_skip hidden)
{
    return scene->hovered.slot && world == scene->world &&
           scene_can_move(scene) && !acount(scene->field_previews) &&
           !render_skip_has(hidden, scene->hovered);
}

static CF_V2 canvas_point(const engine_viewport_camera *frame, CF_V2 ndc)
{
    return (CF_V2){ frame->center_x + ndc.x * frame->span_width * 0.5f,
                    frame->center_y + ndc.y * frame->span_height * 0.5f };
}

static void draw_marquee(app_scene_authoring *scene)
{
    if (!scene->marquee_visible) {
        return;
    }
    engine_viewport_camera frame;
    engine_viewport_get_camera(scene->render_viewport, &frame);
    CF_V2 a = canvas_point(&frame, scene->marquee_start);
    CF_V2 b = canvas_point(&frame, scene->marquee_end);
    CF_Aabb box = { { fminf(a.x, b.x), fminf(a.y, b.y) },
                    { fmaxf(a.x, b.x), fmaxf(a.y, b.y) } };
    cf_draw_push_color(packed_color(0xFFFFFF10));
    cf_draw_quad_fill(box, 0);
    cf_draw_pop_color();
    cf_draw_push_color(packed_color(0xFFFFFFAA));
    cf_draw_quad(box, scene->selection_scale * 0.5f, 0);
    cf_draw_pop_color();
}

/* The outline of selection entry `i`: the active object's, or another
   selected one's; -1 is the selected runtime entity. */
static unsigned selection_color(const app_scene_authoring *scene, int i)
{
    return i < 0 || i == scene->selected ? SCENE_ACTIVE_COLOR : SCENE_SELECTED_COLOR;
}

/* An object's bounding box, projected and drawn as its twelve edges. */
static void draw_arena_box(
    ecs_world *world,
    const render_scene3d_camera *camera,
    const engine_viewport_camera *frame,
    ecs_entity entity,
    float stroke,
    unsigned color
)
{
    CF_V3 bounds[8];
    if (!render_scene3d_entity_bounds(world, entity, bounds)) {
        return;
    }
    CF_V2 corners[8];
    for (int j = 0; j < 8; j++) {
        CF_V3 projected = render_scene3d_project(camera, bounds[j]);
        corners[j] = canvas_point(frame, (CF_V2){ projected.x, projected.y });
    }
    cf_draw_push_color(packed_color(color));
    for (int j = 0; j < 8; j++) {
        for (int bit = 1; bit <= 4; bit *= 2) {
            if (!(j & bit)) {
                cf_draw_line(corners[j], corners[j | bit], stroke);
            }
        }
    }
    cf_draw_pop_color();
}

static void draw_arena_selection(
    app_scene_authoring *scene,
    ecs_world *world,
    const render_scene3d_camera *camera,
    render_skip hidden
)
{
    engine_viewport_camera frame;
    engine_viewport_get_camera(scene->render_viewport, &frame);
    float stroke = scene->selection_scale > 0 ? scene->selection_scale * 0.5f : 0.5f;
    if (hover_shown(scene, world, hidden)) {
        draw_arena_box(world, camera, &frame, scene->hovered, stroke, SCENE_HOVER_COLOR);
    }
    for (int i = -1; i < acount(scene->actors); i++) {
        ecs_entity entity = i < 0 ? scene->runtime_selected : scene->actors[i].entity;
        bool selected = i < 0 ? entity.slot != 0 : scene_instance_selected(scene, i);
        if (selected && entity.slot && !render_skip_has(hidden, entity)) {
            draw_arena_box(world, camera, &frame, entity, stroke, selection_color(scene, i));
        }
    }
    for (int i = 0; i < acount(scene->field_previews); i++) {
        ecs_entity entity = scene->field_previews[i].entity;
        draw_arena_box(world, camera, &frame, entity, stroke, SCENE_DROP_COLOR);
    }
    draw_marquee(scene);
}

/* A box `margin` pixels around an object's appearance, a pixel wide. */
static void outline_object(ecs_world *world, ecs_entity entity, float pixel, float margin, unsigned color)
{
    CF_Aabb bounds;
    if (!visual_bounds(world, entity, false, &bounds)) {
        return;
    }
    bounds.min.x -= margin * pixel;
    bounds.min.y -= margin * pixel;
    bounds.max.x += margin * pixel;
    bounds.max.y += margin * pixel;
    cf_draw_push_color(packed_color(color));
    cf_draw_quad(bounds, pixel, 0);
    cf_draw_pop_color();
}

/* The grid under the scene while it is edited: the renderer's backdrop, or
   drawn here when the scene draws its own shapes. */
static float scene_grid(app_scene_authoring *scene)
{
    float grid = scene_gizmo_grid(scene);
    app_scene_renderer_set_grid(scene->renderer, grid);
    return grid;
}

static void draw_grid(app_scene_authoring *scene, float grid)
{
    engine_viewport_camera camera;
    engine_viewport_get_camera(scene->render_viewport, &camera);
    CF_V2 half = { camera.span_width * 0.5f, camera.span_height * 0.5f };
    CF_Aabb view = cf_make_aabb_center_half_extents(
        (CF_V2){ camera.center_x, camera.center_y },
        half
    );
    render_grid_2d_draw(grid, view, scene->selection_scale > 0 ? scene->selection_scale : 1, 0);
}

void app_scene_authoring_render(void *user)
{
    app_scene_authoring *scene = (app_scene_authoring *)user;
    if (!scene) {
        return;
    }
    ecs_world *world = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                         ? game_prototype_preview_world(scene->prototype_preview)
                         : scene->world;
    const game_particles *particles = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                                        ? game_prototype_preview_particles(scene->prototype_preview)
                                        : scene->live->runtime->particles;
    const engine *runtime = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                              ? game_prototype_preview_runtime(scene->prototype_preview)
                              : scene->runtime;
    /* An edited scene shows the game's state only as its view options ask; a
       paused run keeps its own, its materials, visibility and lighting. */
    bool editing = scene_editing(scene);
    const app_scene_view_options *options = &scene->view_options;
    if (editing) {
        runtime = scene->world_preview.built || options->fog ? scene->runtime : NULL;
    }
    app_scene_renderer_set_view(
        scene->renderer,
        editing && !options->lighting,
        editing && !options->fog
    );
    render_skip hidden = scene_view_skip(scene, APP_SCENE_VIEW_HIDDEN, &scene->view_hidden);
    /* Edits and loads move objects before any engine frame propagates them. */
    if (game_transform_propagate(world) != ECS_OK) {
        return;
    }
    if (scene->render_viewport && engine_viewport_perspective(scene->render_viewport)) {
        render_scene3d_camera camera = camera3d(scene->render_viewport);
        app_scene_renderer_draw3d(
            scene->renderer,
            world,
            particles,
            &camera,
            scene->render_viewport,
            runtime,
            hidden
        );
        if (scene_can_move(scene)) {
            draw_arena_selection(scene, world, &camera, hidden);
        }
        return;
    }
    float grid = scene_grid(scene);
    bool material_drawn = scene->render_viewport && app_scene_renderer_draw(
                                                        scene->renderer,
                                                        world,
                                                        particles,
                                                        scene->render_viewport,
                                                        runtime,
                                                        hidden
                                                    );
    ecs_component_id required[] = { GAME_COMPONENT_GameTransform.id,
                                    GAME_COMPONENT_GameAppearance.id };
    ecs_query query;
    if (ecs_query_begin(world, &query, required, 2) != ECS_OK) {
        return;
    }
    if (!material_drawn && grid > 0) {
        draw_grid(scene, grid);
    }
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        if (render_skip_has(hidden, entity)) {
            continue;
        }
        GameGlobalTransform2D shown = engine_render_transform(runtime, world, entity);
        const GameTransform *transform = &shown.position;
        const GameAppearance *appearance = GAME_GameAppearance_get_const(world, entity);
        CF_Aabb bounds;
        float radians = -shown.radians;
        if (!appearance_bounds(
                transform,
                appearance,
                GAME_GameShader_get_const(world, entity),
                radians,
                false,
                &bounds
            )) {
            continue;
        }
        if (!material_drawn) {
            GameAppearance visible = *appearance;
            const GameMaterial *material = GAME_GameMaterial_get_const(world, entity);
            if (material) {
                visible.color = material->scene;
                visible.border_color = material->border_scene;
                visible.glow_radius = 0;
            }
            draw_appearance(&visible, transform, radians);
        }
    }
    ecs_query_end(&query);
    if (!material_drawn) {
        draw_particles(particles);
    }
    if (scene->active_runtime_target == APP_RUNTIME_PREVIEW && scene->preview_follow) {
        const GameTransform *position = GAME_GameTransform_get_const(
            world,
            game_prototype_preview_entity(scene->prototype_preview)
        );
        if (position) {
            CF_V2 path[] = { { scene->preview_origin.x, -scene->preview_origin.y },
                             { position->x, -position->y } };
            engine_viewport_camera camera;
            engine_viewport_get_camera(scene->render_viewport, &camera);
            int width, height;
            engine_viewport_pixel_size(scene->render_viewport, &width, &height);
            float stroke = camera.span_width / fmaxf(1, width);
            cf_draw_push_color(packed_color(0x82B5FF55));
            cf_draw_polyline(path, 2, stroke, false);
            cf_draw_circle_fill2(path[0], stroke * 3);
            cf_draw_pop_color();
        }
    }
    if (editing) {
        scene_overlays_draw(scene);
    }
    /* Overlays keep a pixel's antialiasing at any camera zoom. */
    float pixel = scene->selection_scale > 0 ? scene->selection_scale : 1;
    cf_draw_push_shape_aa(SCENE_OVERLAY_AA * pixel * cf_app_get_pixel_scale());
    /* Under the selection's outline, which covers it on a selected object. */
    if (hover_shown(scene, world, hidden)) {
        outline_object(world, scene->hovered, pixel, 3, SCENE_HOVER_COLOR);
    }
    for (int selection = -1; selection < acount(scene->actors); selection++) {
        ecs_entity selected = selection < 0 ? scene->runtime_selected
                                            : scene->actors[selection].entity;
        bool marked = selection < 0 ? selected.slot != 0
                                    : scene_instance_selected(scene, selection);
        if (scene_can_move(scene) && selected.slot && marked &&
            !render_skip_has(hidden, selected)) {
            outline_object(world, selected, pixel, 3, selection_color(scene, selection));
        }
    }
    for (int i = 0; i < acount(scene->field_previews); i++) {
        outline_object(world, scene->field_previews[i].entity, pixel, 5, SCENE_DROP_COLOR);
    }
    draw_marquee(scene);
    if (scene_can_move(scene)) {
        scene_gizmo_draw(scene);
    }
    cf_draw_pop_shape_aa();
}
