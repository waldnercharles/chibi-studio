/* The Scene view's Generated World: the scene's own materials generate its
 * terrain, clouds and liquids while it is edited, as the game's first frame
 * does, without Begin, behaviors or a simulation step (Unreal's Realtime
 * viewport shows procedural content the same way). The generated entities
 * live in the runtime's materials, never in the authored objects; Play
 * discards them so the game generates its own. */
#include "editor/workspace/viewport.h"
#include "editor/workspace/scene_internal.h"
#include "platform/cute.h"
#include "runtime/component_catalog.h"

#include <math.h>
#include <stdio.h>

/* Edits that change what generation reads wait this long for the next one. */
#define PREVIEW_DEBOUNCE_SECONDS 0.15
/* Streamed terrain fills the view for at most this long each frame. */
#define PREVIEW_FRAME_SECONDS 0.006
/* The farthest from the view's center terrain streams for the editor. */
#define PREVIEW_STREAM_RADIUS 4096.0f

static double now_seconds()
{
    return (double)cf_get_ticks() / (double)cf_get_tick_frequency();
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t count)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < count; i++) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

/* Whether generation reads the entity: the components materials prepare. */
static bool generation_source(const ecs_world *world, ecs_entity entity)
{
    const ecs_component_id sources[] = {
        COMPONENT_Destructible.id,
        COMPONENT_FluidVolume.id,
        COMPONENT_MaterialFluidWorld.id,
        COMPONENT_MaterialDestructionWorld.id,
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        if (ecs_component_has(world, entity, sources[i])) {
            return true;
        }
    }
    return false;
}

/* Every component of the entity. Strings are interned, so their pointers
 * name their text. */
static uint64_t hash_entity(uint64_t hash, const ecs_world *world, ecs_entity entity)
{
    uint64_t id = ecs_entity_to_u64(entity);
    hash = hash_bytes(hash, &id, sizeof id);
    for (int c = 0; c < ecs_world_component_count(world); c++) {
        const ecs_component_desc *desc = ecs_world_component_at(world, c);
        if (ecs_component_has(world, entity, desc->id)) {
            hash = hash_bytes(hash, &desc->id, sizeof desc->id);
            hash = hash_bytes(hash, ecs_component_data_const(world, entity, desc->id), desc->element_size);
        }
    }
    return hash;
}

/* What generation reads: terrain from its sources and the field graphs'
 * revisions, and everything else from the other sources. */
static scene_world_inputs generation_inputs(app_scene_authoring *scene)
{
    ecs_world *world = scene->world;
    scene_world_inputs inputs = { UINT64_C(14695981039346656037),
                                  UINT64_C(14695981039346656037) };
    for (int i = 0; i < acount(scene->actors); i++) {
        ecs_entity entity = scene->actors[i].entity;
        if (MaterialTerrain_get_const(world, entity)) {
            inputs.terrain = hash_entity(inputs.terrain, world, entity);
        } else if (generation_source(world, entity)) {
            inputs.sources = hash_entity(inputs.sources, world, entity);
        }
    }
    material_fields *fields = materials_fields(scene->live->runtime->materials);
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        if (definition->working->domain == BEHAVIOR_DOMAIN_FIELD) {
            unsigned revision = material_field_revision(fields, definition->name);
            inputs.terrain = hash_bytes(inputs.terrain, &revision, sizeof revision);
        }
    }
    return inputs;
}

void scene_world_preview_discard(app_scene_authoring *scene)
{
    scene_world_preview *preview = &scene->world_preview;
    if (preview->built) {
        materials_reset(scene->live->runtime->materials);
    }
    *preview = (scene_world_preview){ 0 };
}

/* Terrain streams around the editor's view as well as the scene's cameras. */
static void stream_view(materials *materials, const engine_viewport_camera *camera)
{
    float radius = 0.5f * hypotf(camera->span_width, camera->span_height);
    material_stream_view view = { camera->center_x,
                                  -camera->center_y,
                                  fminf(radius, PREVIEW_STREAM_RADIUS),
                                  0 };
    materials_set_stream_views(materials, &view, 1);
}

/* Prepares the materials, and keeps streaming terrain into the view for a
 * share of the frame. */
static void generate(app_scene_authoring *scene, const engine_viewport_camera *view)
{
    scene_world_preview *preview = &scene->world_preview;
    materials *materials = scene->live->runtime->materials;
    stream_view(materials, view);
    double start = now_seconds();
    const char *error = NULL;
    do {
        error = materials_prepare(materials);
    } while (!error && materials_streaming(materials) &&
             now_seconds() - start < PREVIEW_FRAME_SECONDS);
    snprintf(preview->error, sizeof preview->error, "%s", error ? error : "");
}

void scene_world_preview_update(app_scene_authoring *scene, const engine_viewport_camera *view)
{
    scene_world_preview *preview = &scene->world_preview;
    if (scene->runtime_snapshot_valid) {
        /* The game owns the materials until Stop resets them. */
        *preview = (scene_world_preview){ 0 };
        return;
    }
    if (!scene->view_options.generated_world) {
        scene_world_preview_discard(scene);
        return;
    }
    double now = now_seconds();
    scene_world_inputs inputs = generation_inputs(scene);
    bool terrain = inputs.terrain != preview->inputs.terrain;
    if (preview->built && (terrain || inputs.sources != preview->inputs.sources)) {
        preview->dirty = true;
        preview->terrain_dirty |= terrain;
        preview->changed_at = now;
    }
    preview->inputs = inputs;
    /* Terrain streams again only when its own sources change. */
    if (preview->dirty && now - preview->changed_at >= PREVIEW_DEBOUNCE_SECONDS) {
        if (preview->terrain_dirty) {
            materials_reset(scene->live->runtime->materials);
        } else {
            materials_reset_except_terrain(scene->live->runtime->materials);
        }
        preview->dirty = preview->terrain_dirty = false;
    }
    if (!preview->dirty) {
        preview->built = true;
        generate(scene, view);
    }
    bool waiting = materials_streaming(scene->live->runtime->materials);
    preview->waiting_since = !waiting               ? 0
                           : preview->waiting_since ? preview->waiting_since
                                                    : now;
}

bool app_scene_authoring_world_pending(const app_scene_authoring *scene)
{
    const scene_world_preview *preview = &scene->world_preview;
    return preview->waiting_since > 0 &&
           now_seconds() - preview->waiting_since >= SCENE_WORLD_PENDING_SECONDS;
}

const char *app_scene_authoring_world_error(const app_scene_authoring *scene)
{
    return scene->world_preview.error;
}

void app_scene_authoring_set_view_options(app_scene_authoring *scene, app_scene_view_options options)
{
    scene->view_options = options;
}
