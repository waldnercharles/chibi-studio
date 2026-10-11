#ifndef CHIBI_SCENE_DEBUG_H
#define CHIBI_SCENE_DEBUG_H

#include "authoring/preview_source.h"
#include "execution/inspection.h"
#include "runtime/behaviors/source.h"

typedef struct
{
    ecs_entity entity;
    uint64_t sequence;
    const char *asset_name;
    const execution_module *module;
    execution_program *program;
    execution_instance *instance;
} scene_debug_attachment;

typedef struct
{
    ecs_entity entity;
    uint64_t sequence, session;
    char *asset_name, *prototype;
    execution_inspector *inspector;
    execution_module *module;
    const authoring_sources *sources;
} scene_debug;

scene_debug_attachment scene_debug_attachment_index(source_registry *, int);
int scene_debug_attachment_count_for_entity(source_registry *, ecs_entity);
int scene_debug_attachment_at(source_registry *, ecs_entity, int, scene_debug_attachment *);
bool scene_debug_owner_attachment(source_registry *, const char *name, ecs_entity, scene_debug_attachment *);
void scene_debug_clear(scene_debug *);
/* Retains inspection and applied semantic generations independently of target lifetime. */
void scene_debug_select(scene_debug *, const scene_debug_attachment *, uint64_t session, const authoring_sources *);
bool scene_debug_resolve(scene_debug *, ecs_world *, source_registry *, uint64_t session, scene_debug_attachment *);
bool scene_debug_retired(const scene_debug *, uint64_t session, scene_debug_attachment *);

#endif
