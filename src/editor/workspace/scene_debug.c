#include "editor/workspace/scene_debug.h"

#include "support/xalloc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

scene_debug_attachment scene_debug_attachment_index(source_registry *registry, int index)
{
    source_attachment_info info = source_attachment_at(registry, index);
    return (scene_debug_attachment){ info.entity, info.sequence, info.name,
                                     info.module, info.program,  info.instance };
}

int scene_debug_attachment_count_for_entity(source_registry *registry, ecs_entity entity)
{
    int count = 0;
    for (int i = 0; i < source_attachment_count(registry); i++) {
        source_attachment_info info = source_attachment_at(registry, i);
        count += ecs_entity_to_u64(info.entity) == ecs_entity_to_u64(entity);
    }
    return count;
}

int scene_debug_attachment_at(source_registry *registry, ecs_entity entity, int index, scene_debug_attachment *out)
{
    for (int i = 0; i < source_attachment_count(registry); i++) {
        scene_debug_attachment info = scene_debug_attachment_index(registry, i);
        if (ecs_entity_to_u64(info.entity) == ecs_entity_to_u64(entity) &&
            index-- == 0) {
            *out = info;
            return 0;
        }
    }
    return -1;
}

bool scene_debug_owner_attachment(
    source_registry *registry,
    const char *name,
    ecs_entity entity,
    scene_debug_attachment *out
)
{
    execution_program *program = source_program(registry, name);
    execution_instance *instance = program
                                     ? execution_program_instance(program, ecs_entity_to_u64(entity))
                                     : NULL;
    if (!instance) {
        return false;
    }
    *out = (scene_debug_attachment){ .entity = entity,
                                     .asset_name = name,
                                     .program = program,
                                     .instance = instance };
    return true;
}

void scene_debug_clear(scene_debug *debug)
{
    free(debug->asset_name);
    free(debug->prototype);
    if (debug->inspector) {
        execution_inspector_recording(debug->inspector, false, 0);
        execution_inspector_select_owner(debug->inspector, 0);
    }
    execution_inspector_free(debug->inspector);
    execution_module_free(debug->module);
    authoring_sources_free((authoring_sources *)debug->sources);
    *debug = (scene_debug){ 0 };
}

void scene_debug_select(
    scene_debug *debug,
    const scene_debug_attachment *info,
    uint64_t session,
    const authoring_sources *sources
)
{
    char *name = xstrdup(info->asset_name);
    execution_inspector *inspector = execution_program_inspector(info->program);
    execution_module *module = (execution_module *)info->module;
    execution_module_retain(module);
    const authoring_sources *generation = authoring_sources_retain(sources);
    scene_debug_clear(debug);
    *debug = (scene_debug){ .entity = info->entity,
                            .sequence = info->sequence,
                            .session = session,
                            .asset_name = name,
                            .inspector = inspector,
                            .module = module,
                            .sources = generation };
    execution_inspector_recording(debug->inspector, false, 0);
    bool selected = execution_inspector_select_owner(debug->inspector, ecs_entity_to_u64(info->entity));
    assert(selected);
    (void)selected;
}

bool scene_debug_resolve(
    scene_debug *debug,
    ecs_world *world,
    source_registry *registry,
    uint64_t session,
    scene_debug_attachment *out
)
{
    if (!debug->asset_name || debug->session != session ||
        !ecs_entity_is_alive(world, debug->entity)) {
        return false;
    }
    if (!debug->sequence) {
        if (!scene_debug_owner_attachment(registry, debug->asset_name, debug->entity, out)) {
            return false;
        }
        out->module = debug->module;
        return true;
    }
    for (int i = 0; i < source_attachment_count(registry); i++) {
        scene_debug_attachment info = scene_debug_attachment_index(registry, i);
        if (info.sequence == debug->sequence &&
            !strcmp(info.asset_name, debug->asset_name)) {
            *out = info;
            return true;
        }
    }
    return false;
}

bool scene_debug_retired(const scene_debug *debug, uint64_t session, scene_debug_attachment *out)
{
    if (!debug->asset_name || debug->session != session || !debug->inspector) {
        return false;
    }
    *out = (scene_debug_attachment){ .entity = debug->entity,
                                     .sequence = debug->sequence,
                                     .asset_name = debug->asset_name,
                                     .module = debug->module };
    return true;
}
