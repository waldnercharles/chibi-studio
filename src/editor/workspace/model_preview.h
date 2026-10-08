#ifndef CHIBI_EDITOR_MODEL_PREVIEW_H
#define CHIBI_EDITOR_MODEL_PREVIEW_H
#include "editor/workspace/viewport.h"
#include "assets/model_export.h"
#include "editor/workspace/scene_authoring.h"
#include "render/shader_catalog.h"

typedef struct scene_model_preview scene_model_preview;
typedef enum
{
    MODEL_VIEW_ORBIT,
    MODEL_VIEW_FRONT,
    MODEL_VIEW_SIDE,
    MODEL_VIEW_TOP
} model_preview_view;
typedef struct
{
    char destination[1024];
    float cell_size;
    int max_triangles;
    uint64_t max_samples, max_work;
} model_preview_bake_settings;
model_preview_bake_settings scene_model_preview_bake_settings(const app_scene_authoring *scene);
void scene_model_preview_set_bake_settings(
    app_scene_authoring *scene,
    const model_preview_bake_settings *settings
);
bool scene_model_preview_bake(
    app_scene_authoring *scene,
    model_export_summary *summary,
    char *error,
    size_t size
);
typedef struct
{
    int part, axis;
    bool section;
    float offset;
} model_preview_inspection;
model_preview_inspection scene_model_preview_inspection(const app_scene_authoring *scene);
void scene_model_preview_set_inspection(app_scene_authoring *scene, model_preview_inspection value);
bool scene_model_preview_anchor(
    app_scene_authoring *scene,
    int slot,
    model_anchor *anchor,
    float position[3],
    float angles[3]
);
bool scene_model_preview_dimensions(const app_scene_authoring *scene, float dimensions[3]);
bool scene_model_preview_reference(const app_scene_authoring *scene, float *opacity);
void scene_model_preview_set_reference(app_scene_authoring *scene, bool visible, float opacity);
model_preview_view scene_model_preview_view(const app_scene_authoring *scene);
void scene_model_preview_set_view(app_scene_authoring *scene, model_preview_view view);
int scene_model_preview_start(app_scene_authoring *scene, const char *name);
void scene_model_preview_free(app_scene_authoring *scene);
const char *scene_model_preview_name(const app_scene_authoring *scene);
ecs_entity scene_model_preview_entity(const app_scene_authoring *scene);
void scene_model_preview_update(app_scene_authoring *scene);
void scene_model_preview_pointer(app_scene_authoring *scene, engine_viewport *viewport);
void scene_model_preview_frame(app_scene_authoring *scene);
bool scene_model_preview_parameter(
    app_scene_authoring *scene,
    int slot,
    model_parameter *parameter,
    float values[4]
);
bool scene_model_preview_set_parameter(app_scene_authoring *scene, int slot, const float values[4]);
void scene_model_preview_reset_parameters(app_scene_authoring *scene);
const char *scene_model_preview_error(const app_scene_authoring *scene);
#endif
