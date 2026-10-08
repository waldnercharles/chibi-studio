#ifndef CHIBI_COLLAB_ACTIONS_SCENE_H
#define CHIBI_COLLAB_ACTIONS_SCENE_H

#include "editor/collab/actions_internal.h"
#include "editor/workspace/scene_internal.h"

#include <yyjson.h>

/* Private scene action builders shared by authoring call sites. */
yyjson_mut_val *scene_action_root(app_scene_authoring *scene, const char *operation, yyjson_mut_doc **doc);
char *scene_action_write(yyjson_mut_doc *doc);
collab_action_gate scene_action_begin(app_scene_authoring *scene, const char *json);
bool scene_action_finish(app_scene_authoring *scene, const char *json, bool success);
bool scene_collab_is_editor(void);
bool scene_collab_can_write_locally(void);
uint64_t scene_actor_next_identity(app_scene_authoring *scene);
uint64_t scene_actor_allocate_identity(app_scene_authoring *scene);
uint64_t scene_actor_identity(const app_scene_authoring *scene, ecs_entity entity);
scene_actor *scene_actor_by_identity(app_scene_authoring *scene, uint64_t identity);
void scene_actor_set_identity(app_scene_authoring *scene, scene_actor *actor, uint64_t identity);

#endif
