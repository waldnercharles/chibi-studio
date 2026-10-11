#include "authoring/native_prototype.h"
/* A disposable model fixture uses the ordinary preview engine and SDF scene
 * renderer. It never joins the authored scene or prototype registries. */
#include "editor/workspace/viewport.h"
#include "editor/workspace/model_preview.h"

#include "editor/workspace/scene_internal.h"
#include "render/geometry_2d.h"
#include "render/scene_renderer.h"
#include "runtime/component_catalog.h"
#include "runtime/visual_transform.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scene_model_preview
{
    char *name;
    prototype *fixtures[4];
    ecs_entity entity;
    float pitch, yaw, radius;
    model_preview_view view;
    model_preview_bake_settings bake;
    uint64_t selected_node;
    char *selected_asset, *selected_path;
    bool section;
    int section_axis;
    float section_offset;
    bool reference_visible;
    float reference_opacity;
    bool empty;
    float dimensions[3];
    float drag_x, drag_y, drag_pitch, drag_yaw;
    bool dragging, frame;
    float draft[SHADER_PARAMETER_MAX][4], applied[SHADER_PARAMETER_MAX][4];
    struct
    {
        uint64_t node;
        char *asset, *scope;
        pin_type type;
        int width;
        float values[4];
    } rejected[SHADER_PARAMETER_MAX];
    char error[256];
};

static void component(prototype *prototype, const ecs_component_desc *desc, const void *value)
{
    int result = prototype_add_native_component(prototype, desc);
    assert(result == 0);
    (void)result;
    memcpy(prototype->components[acount(prototype->components) - 1].data, value, desc->element_size);
}
#define COMPONENT(prototype, type, ...)                                        \
    component(prototype, &COMPONENT_##type, &(type){ __VA_ARGS__ })

const char *scene_model_preview_name(const app_scene_authoring *scene)
{
    return scene->model_preview ? scene->model_preview->name : NULL;
}
ecs_entity scene_model_preview_entity(const app_scene_authoring *scene)
{
    return scene->model_preview ? scene->model_preview->entity : (ecs_entity){ 0 };
}
model_preview_bake_settings scene_model_preview_bake_settings(const app_scene_authoring *scene)
{
    return scene->model_preview ? scene->model_preview->bake
                                : (model_preview_bake_settings){ 0 };
}
void scene_model_preview_set_bake_settings(app_scene_authoring *scene, const model_preview_bake_settings *settings)
{
    assert(scene->model_preview);
    scene->model_preview->bake = *settings;
}
bool scene_model_preview_bake(
    app_scene_authoring *scene,
    model_export_summary *summary,
    char *error,
    size_t size
)
{
    scene_model_preview *preview = scene->model_preview;
    model_export_override *overrides = NULL;
    for (int slot = 0; slot < SHADER_PARAMETER_MAX; slot++) {
        model_parameter parameter;
        if (!render_shader_model_parameter(render_shader_find(preview->name), slot, &parameter)) {
            continue;
        }
        model_export_override value = { .asset = parameter.asset,
                                        .scope = parameter.scope,
                                        .parameter_id = parameter.node,
                                        .width = parameter.width };
        memcpy(value.value, preview->applied[slot], sizeof value.value);
        apush(overrides, value);
    }
#ifdef __EMSCRIPTEN__
    static unsigned next_bake;
    snprintf(preview->bake.destination, sizeof preview->bake.destination, "/tmp/model-bake-%u", ++next_bake);
#endif
    model_export_options options = { .bake = model_bake_defaults(),
                                     .overrides = overrides,
                                     .override_count = acount(overrides) };
    options.bake.cell_size = preview->bake.cell_size;
    options.bake.max_triangles = preview->bake.max_triangles;
    options.bake.max_samples = preview->bake.max_samples;
    options.bake.max_work = preview->bake.max_work;
    bool ok = model_export_bundle(
        scene->content_root,
        preview->name,
        preview->bake.destination,
        &options,
        summary,
        error,
        size
    );
    afree(overrides);
    return ok;
}

model_preview_inspection scene_model_preview_inspection(const app_scene_authoring *scene)
{
    const scene_model_preview *preview = scene->model_preview;
    model_preview_inspection value = { .part = -1 };
    if (!preview) {
        return value;
    }
    value.axis = preview->section_axis;
    value.section = preview->section;
    value.offset = preview->section_offset;
    const model_inspection *report = render_shader_model_inspection(
        render_shader_find(preview->name)
    );
    for (int i = 0; report && preview->selected_node && i < acount(report->parts); i++) {
        const model_part_inspection *part = &report->parts[i];
        if (part->node == preview->selected_node &&
            !strcmp(part->asset, preview->selected_asset) &&
            !strcmp(part->path, preview->selected_path)) {
            value.part = i;
            break;
        }
    }
    return value;
}
void scene_model_preview_set_inspection(app_scene_authoring *scene, model_preview_inspection value)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview) {
        return;
    }
    const model_inspection *report = render_shader_model_inspection(
        render_shader_find(preview->name)
    );
    assert(value.axis >= 0 && value.axis < 3 && isfinite(value.offset));
    assert(
        value.part == -1 ||
        (report && value.part >= 0 && value.part < acount(report->parts))
    );
    free(preview->selected_asset);
    free(preview->selected_path);
    preview->selected_asset = preview->selected_path = NULL;
    preview->selected_node = 0;
    if (value.part >= 0) {
        const model_part_inspection *part = &report->parts[value.part];
        preview->selected_node = part->node;
        preview->selected_asset = xstrdup(part->asset);
        preview->selected_path = xstrdup(part->path);
    }
    preview->section = value.section;
    preview->section_axis = value.axis;
    preview->section_offset = value.offset;
    preview->frame = true;
}

bool scene_model_preview_anchor(
    app_scene_authoring *scene,
    int slot,
    model_anchor *anchor,
    float position[3],
    float angles[3]
)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview ||
        !render_shader_model_anchor(render_shader_find(preview->name), slot, anchor)) {
        return false;
    }
    shader_error error = { 0 };
    return render_shader_model_anchor_values(
        render_shader_find(preview->name),
        slot,
        preview->applied,
        position,
        angles,
        &error
    );
}

bool scene_model_preview_dimensions(const app_scene_authoring *scene, float dimensions[3])
{
    if (!scene->model_preview || scene->model_preview->empty) {
        return false;
    }
    memcpy(
        dimensions,
        scene->model_preview->dimensions,
        sizeof scene->model_preview->dimensions
    );
    return true;
}

bool scene_model_preview_reference(const app_scene_authoring *scene, float *opacity)
{
    const scene_model_preview *preview = scene->model_preview;
    *opacity = preview ? preview->reference_opacity : .5f;
    return preview && preview->reference_visible;
}
void scene_model_preview_set_reference(app_scene_authoring *scene, bool visible, float opacity)
{
    scene_model_preview *preview = scene->model_preview;
    if (preview) {
        preview->reference_visible = visible;
        preview->reference_opacity = opacity;
    }
}

model_preview_view scene_model_preview_view(const app_scene_authoring *scene)
{
    return scene->model_preview ? scene->model_preview->view : MODEL_VIEW_ORBIT;
}
void scene_model_preview_set_view(app_scene_authoring *scene, model_preview_view view)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview) {
        return;
    }
    preview->view = view;
    preview->pitch = view == MODEL_VIEW_TOP ? -1.57079633f : 0;
    preview->yaw = view == MODEL_VIEW_SIDE ? 1.57079633f : 0;
    if (view == MODEL_VIEW_ORBIT) {
        preview->pitch = -.45f;
        preview->yaw = .45f;
    }
    preview->dragging = false;
    preview->frame = true;
    preview->radius = 0; /* Refresh camera lens even if dimensions did not change. */
}

void scene_model_preview_free(app_scene_authoring *scene)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        prototype_free(preview->fixtures[i]);
    }
    for (int i = 0; i < SHADER_PARAMETER_MAX; i++) {
        free(preview->rejected[i].asset);
        free(preview->rejected[i].scope);
    }
    app_scene_renderer_set_model_inspection(
        scene->renderer,
        (ecs_entity){ 0 },
        -1,
        false,
        (float[3]){ 0, 0, 1 },
        0
    );
    free(preview->selected_asset);
    free(preview->selected_path);
    free(preview->name);
    free(preview);
    scene->model_preview = NULL;
}

int scene_model_preview_start(app_scene_authoring *scene, const char *name)
{
    if (scene->runtime_snapshot_valid) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Stop the scene before previewing a model.");
        return -1;
    }
    preview_definition *definition = preview_find_definition(scene->live, name);
    if (!definition || definition->working->domain != BEHAVIOR_DOMAIN_MODEL) {
        return -1;
    }
    if (scene_model_preview_name(scene) &&
        !strcmp(scene_model_preview_name(scene), name)) {
        scene->active_runtime_target = APP_RUNTIME_PREVIEW;
        return 0;
    }
    if (!render_shader_is_model(render_shader_find(name))) {
        if (preview_update(
                scene->live,
                definition,
                scene->preview_message,
                sizeof scene->preview_message
            ) < 0) {
            return -1;
        }
    }
    app_scene_authoring_stop_prototype_preview(scene);
    scene_model_preview *preview = xcalloc(1, sizeof *preview);
    scene->model_preview = preview;
    preview->name = xstrdup(name);
    model_bake_options defaults = model_bake_defaults();
    preview->bake.max_triangles = defaults.max_triangles;
    preview->bake.max_samples = defaults.max_samples;
    preview->bake.max_work = defaults.max_work;
    preview->reference_opacity = .5f;
#ifndef __EMSCRIPTEN__
    snprintf(
        preview->bake.destination,
        sizeof preview->bake.destination,
        "%s/../%s-mesh",
        scene->content_root,
        name
    );
#endif
    preview->pitch = -.45f;
    preview->yaw = .45f;
    preview->frame = true;
    scene->model_camera = (scene_camera){ 0 };
    for (int i = 0; i < 4; i++) {
        static const char *const names[] = { "ModelPreviewObject",
                                             "ModelPreviewCamera",
                                             "ModelPreviewKey",
                                             "ModelPreviewFill" };
        preview->fixtures[i] = prototype_new(names[i]);
        preview->fixtures[i]->registry_storage = scene->registry;
    }
    prototype *object = preview->fixtures[0];
    COMPONENT(object, Transform, 0);
    COMPONENT(object, Appearance, .width = 1, .height = 1, .color = 0xb7c4d6ff);
    COMPONENT(object, Geometry3D, .primitive = PRIMITIVE_BOX, .width = 1, .height = 1, .depth = 1);
    COMPONENT(object, MeshSurface2D, .enabled = true);
    COMPONENT(object, SdfSurface3D, .enabled = true);
    COMPONENT(object, VisualTransform, .scale = 1);
    COMPONENT(object, Shader, .shape = preview->name);
    prototype *camera = preview->fixtures[1];
    COMPONENT(camera, Camera, .width = 200, .height = 200);
    HRC2D lighting = hrc2d_defaults();
    lighting.intensity = 3;
    lighting.exposure = .6f;
    lighting.ambient = .15f;
    lighting.fog = .08f;
    lighting.bloom = .03f;
    lighting.bounce_intensity = .2f;
    lighting.surface_relief = 0;
    component(camera, &COMPONENT_HRC2D, &lighting);
    for (int i = 2; i < 4; i++) {
        COMPONENT(preview->fixtures[i], Transform, .x = i == 2 ? -90 : 70, .y = i == 2 ? -60 : 40);
        COMPONENT(
            preview->fixtures[i],
            Appearance,
            .shape = SHAPE_CIRCLE,
            .width = 1,
            .height = 1,
            .color = i == 2 ? 0xffe6caff : 0xb7d7ffff
        );
        COMPONENT(
            preview->fixtures[i],
            Material,
            .scene = 0xffffffff,
            .albedo = 0xffffffff,
            .emission_color = i == 2 ? 0xffe6caff : 0xb7d7ffff,
            .emission_intensity = i == 2 ? .45f : .2f,
            .emission_multiplier = 1
        );
    }
    prototype_preview_scene_entity entities[4];
    for (int i = 0; i < 4; i++) {
        entities[i] = (prototype_preview_scene_entity){
            .prototype = preview->fixtures[i],
            .parent = -1,
        };
    }
    if (prototype_preview_launch_scene(
            scene->prototype_preview,
            entities,
            4,
            scene->prototype_message,
            sizeof scene->prototype_message
        ) != ECS_OK) {
        scene_model_preview_free(scene);
        return -1;
    }
    preview->entity = prototype_preview_entity(scene->prototype_preview);
    scene->active_runtime_target = APP_RUNTIME_PREVIEW;
    scene->preview_playing = false;
    scene->runtime_preview_prototype = -1;
    scene->preview_follow = false;
    engine_set_playing(prototype_preview_runtime(scene->prototype_preview), false);
    scene->camera_target = -1;
    scene_model_preview_update(scene);
    return 0;
}

static bool snapshot(
    app_scene_authoring *scene,
    float values[SHADER_PARAMETER_MAX][4],
    float min[3],
    float max[3],
    bool *empty
)
{
    scene_model_preview *preview = scene->model_preview;
    shader_error error = { 0 };
    if (!render_shader_model_snapshot(
            render_shader_find(preview->name),
            prototype_preview_world(scene->prototype_preview),
            preview->entity,
            NULL,
            values,
            min,
            max,
            empty,
            &error
        )) {
        snprintf(preview->error, sizeof preview->error, "%s", error.message);
        return false;
    }
    return true;
}

void scene_model_preview_update(app_scene_authoring *scene)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview || scene->active_runtime_target != APP_RUNTIME_PREVIEW) {
        return;
    }
    float values[SHADER_PARAMETER_MAX][4], min[3], max[3];
    if (!snapshot(scene, values, min, max, &preview->empty)) {
        return;
    }
    model_preview_inspection inspection = scene_model_preview_inspection(scene);
    if (preview->selected_node && inspection.part < 0) {
        snprintf(preview->error, sizeof preview->error, "Isolated part no longer exists; showing complete model.");
        scene_model_preview_set_inspection(scene, inspection);
    }
    float normal[3] = { 0 };
    normal[inspection.axis] = 1;
    app_scene_renderer_set_model_inspection(
        scene->renderer,
        preview->entity,
        inspection.part,
        inspection.section,
        normal,
        inspection.offset
    );
    if (inspection.part >= 0) {
        model_bounds part_bounds;
        shader_error error = { 0 };
        if (!render_shader_model_part_bounds(
                render_shader_find(preview->name),
                values,
                inspection.part,
                &part_bounds,
                &error
            )) {
            snprintf(preview->error, sizeof preview->error, "%s", error.message);
            return;
        }
        memcpy(min, part_bounds.min, sizeof min);
        memcpy(max, part_bounds.max, sizeof max);
        preview->empty = part_bounds.empty;
    }
    for (int i = 0; i < 3; i++) {
        preview->dimensions[i] = preview->empty ? 0 : max[i] - min[i];
    }
    memcpy(preview->applied, values, sizeof values);
    memcpy(preview->draft, values, sizeof values);
    for (int i = 0; i < SHADER_PARAMETER_MAX; i++) {
        if (!preview->rejected[i].asset) {
            continue;
        }
        bool found = false;
        for (int slot = 0; slot < SHADER_PARAMETER_MAX; slot++) {
            model_parameter parameter;
            if (render_shader_model_parameter(render_shader_find(preview->name), slot, &parameter) &&
                parameter.node == preview->rejected[i].node &&
                parameter.type == preview->rejected[i].type &&
                parameter.width == preview->rejected[i].width &&
                !strcmp(parameter.asset, preview->rejected[i].asset) &&
                !strcmp(parameter.scope, preview->rejected[i].scope)) {
                memcpy(preview->draft[slot], preview->rejected[i].values, sizeof values[slot]);
                found = true;
                break;
            }
        }
        if (!found) {
            free(preview->rejected[i].asset);
            free(preview->rejected[i].scope);
            preview->rejected[i].asset = NULL;
            preview->rejected[i].scope = NULL;
            snprintf(preview->error, sizeof preview->error, "Rejected preview value was removed because its declaration changed.");
        }
    }
    if (preview->empty) {
        preview->frame = false;
        return;
    }
    ecs_world *world = prototype_preview_world(scene->prototype_preview);
    float radius = .5f *
                   hypotf(hypotf(max[0] - min[0], max[1] - min[1]), max[2] - min[2]);
    if (!(radius > 0) || !isfinite(radius)) {
        snprintf(preview->error, sizeof preview->error, "Model bounds cannot be framed.");
        return;
    }
    if (radius != preview->radius) {
        preview->radius = radius;
        for (int i = 2; i < 4; i++) {
            ecs_entity light_entity = prototype_preview_entity_at(scene->prototype_preview, i);
            Transform *position = Transform_get(world, light_entity);
            position->x = radius * (i == 2 ? -.9f : .7f);
            position->y = radius * (i == 2 ? -.6f : .4f);
            Appearance *light = Appearance_get(world, light_entity);
            light->width = light->height = radius * .12f;
        }
        ecs_entity camera_entity = prototype_preview_entity_at(scene->prototype_preview, 1);
        Camera *camera = Camera_get(world, camera_entity);
        camera->width = camera->height = radius * 2.5f;
        camera->focal_distance = preview->view == MODEL_VIEW_ORBIT ? radius * 6 : 0;
        HRC2D_get(world, camera_entity)->surface_radius = radius * .24f;
        if (scene->render_viewport) {
            engine_viewport_view view;
            engine_viewport_get_view(scene->render_viewport, &view);
            view.focal_distance = camera->focal_distance;
            engine_viewport_set_view(scene->render_viewport, &view);
        }
    }
    if (scene->render_viewport) {
        engine_viewport_view view;
        engine_viewport_get_view(scene->render_viewport, &view);
        float focal_distance = preview->view == MODEL_VIEW_ORBIT ? radius * 6 : 0;
        if (view.focal_distance != focal_distance) {
            view.focal_distance = focal_distance;
            engine_viewport_set_view(scene->render_viewport, &view);
        }
    }
    VisualTransform *visual = VisualTransform_get(world, preview->entity);
    visual->pitch = preview->pitch;
    visual->yaw = preview->yaw;
    CF_M4x4 rotation = cf_m4_from_trs(
        (CF_V3){ 0 },
        visual_rotation(0, (CF_V3){ preview->pitch, preview->yaw, 0 }),
        (CF_V3){ 1, 1, 1 }
    );
    CF_V4 center = cf_mul_m4_v4(
        rotation,
        (CF_V4){ min[0] * .5f + max[0] * .5f,
                 min[1] * .5f + max[1] * .5f,
                 min[2] * .5f + max[2] * .5f,
                 1 }
    );
    Transform *position = Transform_get(world, preview->entity);
    position->x = -center.x;
    position->y = -center.z;
    Geometry3D_get(world, preview->entity)->elevation = -center.y;
    if (preview->frame && scene->render_viewport) {
        CF_V2 lo, hi;
        if (render_geometry_2d_mesh_bounds(
                world,
                preview->entity,
                NULL,
                &lo,
                &hi,
                preview->error,
                sizeof preview->error
            )) {
            engine_viewport_view view;
            engine_viewport_get_view(scene->render_viewport, &view);
            view.center_x = lo.x * .5f + hi.x * .5f;
            view.center_y = -(lo.y * .5f + hi.y * .5f);
            view.width = fmaxf(hi.x - lo.x, radius * .001f) * 1.25f;
            view.height = fmaxf(hi.y - lo.y, radius * .001f) * 1.25f;
            engine_viewport_set_view(scene->render_viewport, &view);
        }
        preview->frame = false;
    }
}

void scene_model_preview_frame(app_scene_authoring *scene)
{
    if (scene->model_preview) {
        scene->model_preview->frame = true;
    }
}

void scene_model_preview_pointer(app_scene_authoring *scene, engine_viewport *viewport)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview || scene->active_runtime_target != APP_RUNTIME_PREVIEW) {
        return;
    }
    engine_viewport_pointer pointer;
    engine_viewport_get_pointer(viewport, &pointer);
    if (pointer.pressed && preview->view == MODEL_VIEW_ORBIT) {
        preview->dragging = true;
        preview->drag_x = pointer.press_ndc_x;
        preview->drag_y = pointer.press_ndc_y;
        preview->drag_pitch = preview->pitch;
        preview->drag_yaw = preview->yaw;
    }
    if (preview->dragging && pointer.dragging) {
        preview->yaw = preview->drag_yaw + (pointer.ndc_x - preview->drag_x) * 3.14159265f;
        preview->pitch = fmaxf(
            -1.55f,
            fminf(1.55f, preview->drag_pitch - (pointer.ndc_y - preview->drag_y) * 1.57f)
        );
    }
    if (pointer.released || pointer.cancel) {
        preview->dragging = false;
    }
}

bool scene_model_preview_parameter(
    app_scene_authoring *scene,
    int slot,
    model_parameter *parameter,
    float values[4]
)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview ||
        !render_shader_model_parameter(render_shader_find(preview->name), slot, parameter)) {
        return false;
    }
    memcpy(values, preview->draft[slot], sizeof preview->draft[slot]);
    return true;
}

bool scene_model_preview_set_parameter(app_scene_authoring *scene, int slot, const float values[4])
{
    scene_model_preview *preview = scene->model_preview;
    model_parameter parameter;
    if (!preview ||
        !render_shader_model_parameter(render_shader_find(preview->name), slot, &parameter)) {
        return false;
    }
    int rejected = -1;
    for (int i = 0; i < SHADER_PARAMETER_MAX; i++) {
        if (preview->rejected[i].asset && parameter.node == preview->rejected[i].node &&
            !strcmp(parameter.asset, preview->rejected[i].asset) &&
            !strcmp(parameter.scope, preview->rejected[i].scope)) {
            rejected = i;
            break;
        }
    }
    pin_value value = parameter.width == 1
                        ? (pin_value){ .type = PIN_F32, .f = values[0] }
                        : (pin_value){ .type = PIN_STRUCT,
                                       .type_name = parameter.type_name,
                                       .ptr = (void *)values };
    const char *error = render_shader_shape_parameter_set(
        prototype_preview_world(scene->prototype_preview),
        preview->entity,
        parameter.name,
        value
    );
    if (error) {
        if (rejected < 0) {
            for (int i = 0; i < SHADER_PARAMETER_MAX; i++) {
                if (!preview->rejected[i].asset) {
                    rejected = i;
                    break;
                }
            }
            assert(rejected >= 0);
            preview->rejected[rejected].asset = xstrdup(parameter.asset);
            preview->rejected[rejected].scope = xstrdup(parameter.scope);
        }
        preview->rejected[rejected].node = parameter.node;
        preview->rejected[rejected].type = parameter.type;
        preview->rejected[rejected].width = parameter.width;
        memcpy(preview->rejected[rejected].values, values, sizeof preview->draft[slot]);
        snprintf(preview->error, sizeof preview->error, "%s", error);
        memcpy(preview->draft[slot], values, sizeof preview->draft[slot]);
        return false;
    }
    if (rejected >= 0) {
        free(preview->rejected[rejected].asset);
        free(preview->rejected[rejected].scope);
        preview->rejected[rejected].asset = NULL;
        preview->rejected[rejected].scope = NULL;
    }
    preview->error[0] = 0;
    for (int i = 0; i < SHADER_PARAMETER_MAX; i++) {
        if (preview->rejected[i].asset) {
            snprintf(preview->error, sizeof preview->error, "A preview parameter still has a rejected value.");
            break;
        }
    }
    scene_model_preview_update(scene);
    return true;
}

void scene_model_preview_reset_parameters(app_scene_authoring *scene)
{
    scene_model_preview *preview = scene->model_preview;
    if (!preview) {
        return;
    }
    char *name = xstrdup(preview->name);
    model_preview_bake_settings bake = preview->bake;
    float pitch = preview->pitch, yaw = preview->yaw;
    app_scene_authoring_stop_prototype_preview(scene);
    if (scene_model_preview_start(scene, name) == 0) {
        scene->model_preview->pitch = pitch;
        scene->model_preview->yaw = yaw;
        scene_model_preview_set_bake_settings(scene, &bake);
        scene_model_preview_frame(scene);
    }
    free(name);
}
const char *scene_model_preview_error(const app_scene_authoring *scene)
{
    if (!scene->model_preview) {
        return "";
    }
    return scene->model_preview->error[0] ? scene->model_preview->error
         : scene->model_preview->empty    ? "Model has an empty result"
                                          : "";
}
