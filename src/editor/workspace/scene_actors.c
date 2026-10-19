#include "assets/scene_document.h"
#include "authoring/material_prepare.h"
#include "authoring/native_prototype.h"
#include "editor/collab/actions_scene.h"
#include "editor/collab/collab.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "runtime/declared_components.h"
#include "runtime/transform_propagation.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int attach_prototype_behavior(ecs_world *world, ecs_entity entity, const char *asset_name, void *user)
{
    app_scene_authoring *scene = user;
    if (!scene || !source_registered(scene->live->runtime->sources, asset_name)) {
        return -1;
    }
    assert(world == scene->world);
    preview_runtime *runtime = scene->live->runtime;
    return source_attach(
        runtime->sources,
        entity,
        asset_name,
        runtime->deferred,
        runtime->now,
        runtime->delta
    );
}

int actor_has_behavior(source_registry *sources, const scene_actor *actor, const char *asset_name)
{
    for (int i = 0; i < source_attachment_count(sources); i++) {
        source_attachment_info info = source_attachment_at(sources, i);
        if (ecs_entity_to_u64(info.entity) == ecs_entity_to_u64(actor->entity) &&
            !strcmp(info.name, asset_name)) {
            return 1;
        }
    }
    return 0;
}

static int destroy_actor_entity(app_scene_authoring *scene, ecs_entity entity)
{
    if (!ecs_entity_is_alive(scene->world, entity)) {
        return ECS_ERR_ENTITY;
    }
    source_detach_entity(scene->live->runtime->sources, entity);
    return ecs_world_destroy_entity(scene->world, entity);
}

void free_behavior_snapshots(prototype **snapshots)
{
    for (int i = 0; i < acount(snapshots); i++) {
        prototype_free(snapshots[i]);
    }
    afree(snapshots);
}

bool scene_actor_has_children(app_scene_authoring *scene, const scene_actor *actor)
{
    return ecs_pair_source_count(scene->world, ECS_CHILD_OF, actor->entity) != 0;
}

uint64_t scene_actor_parent(app_scene_authoring *scene, const scene_actor *actor)
{
    if (scene->runtime_snapshot_valid) {
        return actor->runtime_parent;
    }
    ecs_entity parent = ecs_parent(scene->world, actor->entity);
    for (int i = 0; parent.slot && i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(parent)) {
            return scene->actors[i].identity;
        }
    }
    return 0;
}

static bool descends_from(const app_scene_authoring *scene, ecs_entity entity, ecs_entity ancestor)
{
    for (ecs_entity at = ecs_parent(scene->world, entity); at.slot;
         at = ecs_parent(scene->world, at)) {
        if (ecs_entity_to_u64(at) == ecs_entity_to_u64(ancestor)) {
            return true;
        }
    }
    return false;
}

Transform scene_local_offset(const ecs_world *world, ecs_entity entity, Transform offset)
{
    ecs_entity parent = transform_parent(world, entity);
    float radians = parent.slot ? transform_world_2d(world, parent).radians : 0;
    float c = cosf(radians), s = sinf(radians);
    return (Transform){ c * offset.x + s * offset.y, c * offset.y - s * offset.x };
}

int scene_actor_subtree_end(const app_scene_authoring *scene, int index)
{
    int end = index + 1;
    while (end < acount(scene->actors) &&
           descends_from(scene, scene->actors[end].entity, scene->actors[index].entity)) {
        end++;
    }
    return end;
}

int add_actor(app_scene_authoring *scene, const char *name, const char *prototype_name)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    if (!scene || !name || !name[0] || !asset) {
        return -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        return -1;
    }
    for (int i = 0; i < acount(asset->prototype->graphs); i++) {
        const char *name = asset->prototype->graphs[i];
        if (!source_registered(scene->live->runtime->sources, name)) {
            snprintf(
                scene->message,
                sizeof scene->message,
                "Prototype %s references missing behavior asset %s.",
                prototype_name,
                name
            );
            return -1;
        }
    }
    const prototype_spawn_hooks hooks = { attach_prototype_behavior, scene };
    char error[256];
    ecs_entity entity = { 0 };
    if (!authoring_prototype_parameters(scene->live->runtime->entities, asset->prototype, error, sizeof error) ||
        prototype_spawn(asset->prototype, scene->world, &hooks, &entity, error, sizeof error) !=
            ECS_OK) {
        snprintf(scene->message, sizeof scene->message, "Could not spawn %s: %s", name, error);
        return -1;
    }
    if (entities_rebuild_global_bindings(scene->live->runtime->entities) != 0) {
        destroy_actor_entity(scene, entity);
        snprintf(scene->message, sizeof scene->message, "Global binding names must be unique.");
        return -1;
    }
    prototype *base = prototype_clone(asset->prototype, error, sizeof error);
    if (!base) {
        destroy_actor_entity(scene, entity);
        snprintf(scene->message, sizeof scene->message, "Could not snapshot %s: %s", name, error);
        return -1;
    }
    scene_actor actor = {
        .identity = scene_actor_allocate_identity(scene),
        .entity = entity,
        .name = xstrdup(name),
        .prototype_name = xstrdup(prototype_name),
        .base = base,
    };
    apush(scene->actors, actor);
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
    scene->selected = acount(scene->actors) - 1;
    select_prototype(scene, prototype_index(scene, prototype_name));
    select_instance(scene, scene->selected);
    return 0;
}

static void free_actor(scene_actor *actor)
{
    free(actor->name);
    free(actor->prototype_name);
    prototype_free(actor->base);
    prototype_free(actor->runtime_snapshot);
    prototype_free(actor->restored_state);
    scene_retention_free(actor->retained);
    prototype_links_free(actor->paths);
}

static int remove_actor_at(app_scene_authoring *scene, int index)
{
    scene_actor *actor = &scene->actors[index];
    if (destroy_actor_entity(scene, actor->entity) != ECS_OK) {
        return -1;
    }
    free_actor(actor);
    int count = acount(scene->actors);
    memmove(
        &scene->actors[index],
        &scene->actors[index + 1],
        (size_t)(count - index - 1) * sizeof *scene->actors
    );
    asetlen(scene->actors, count - 1);
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
    scene->selected = index < count - 1 ? index : count - 2;
    if (scene->selected >= 0) {
        select_instance(scene, scene->selected);
    } else {
        sync_editor_inspection(scene);
    }
    return 0;
}

void remove_selected(app_scene_authoring *scene)
{
    if (!selected_actor(scene)) {
        return;
    }
    finish_transform_edit(scene, true);
    scene_history_clear(scene->history);
    /* Unrecorded teardown/load also removes actors destroyed during simulation. */
    if (!ecs_entity_is_alive(scene->world, scene->actors[scene->selected].entity)) {
        free_actor(&scene->actors[scene->selected]);
        int count = acount(scene->actors);
        memmove(
            &scene->actors[scene->selected],
            &scene->actors[scene->selected + 1],
            (size_t)(count - scene->selected - 1) * sizeof *scene->actors
        );
        asetlen(scene->actors, count - 1);
        scene->selected = count - 2;
        return;
    }
    remove_actor_at(scene, scene->selected);
}

/* The object's components and behaviors, spawning as an instance of the
   prototype whose identity is `identity`, with `change` added with its
   defaults or removed, unless NULL. */
static av_value *actor_state_with(
    app_scene_authoring *scene,
    const scene_actor *actor,
    const char *identity,
    const ecs_component_desc *change,
    bool add
)
{
    prototype *state = prototype_capture_entity(
        actor->name,
        scene->world,
        actor->entity,
        &scene->registry
    );
    if (!state) {
        return NULL;
    }
    free(state->identity);
    state->identity = xstrdup(identity);
    int changed = !change ? 0
                : add     ? prototype_add_native_component(state, change)
                          : prototype_remove_component(state, change->name);
    if (changed != 0) {
        prototype_free(state);
        return NULL;
    }
    source_registry *sources = scene->live->runtime->sources;
    for (int i = 0; i < source_attachment_count(sources); i++) {
        source_attachment_info info = source_attachment_at(sources, i);
        if (ecs_entity_to_u64(info.entity) != ecs_entity_to_u64(actor->entity)) {
            continue;
        }
        if (prototype_add_graph(state, info.name)) {
            prototype_free(state);
            return NULL;
        }
    }
    av_value *value = prototype_to_value(state);
    prototype_free(state);
    return value;
}

static av_value *actor_state_as(app_scene_authoring *scene, const scene_actor *actor, const char *identity)
{
    return actor_state_with(scene, actor, identity, NULL, false);
}

static av_value *capture_actor_state(app_scene_authoring *scene, const scene_actor *actor)
{
    return actor_state_as(scene, actor, actor->base->identity);
}

static bool actor_name_used(const app_scene_authoring *scene, const char *name)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (strcmp(scene->actors[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether another child of `parent`, or another root when it is zero, is
   named `name`. Names are unique only among siblings. */
static bool sibling_named(
    const app_scene_authoring *scene,
    ecs_entity parent,
    ecs_entity except,
    const char *name
)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        ecs_entity entity = scene->actors[i].entity;
        if (ecs_entity_to_u64(entity) != ecs_entity_to_u64(except) &&
            ecs_entity_to_u64(ecs_parent(scene->world, entity)) ==
                ecs_entity_to_u64(parent) &&
            !strcmp(scene->actors[i].name, name)) {
            return true;
        }
    }
    return false;
}

static int next_actor_name(const app_scene_authoring *scene, const char *prototype, char *name, size_t size)
{
    int suffix = 1;
    do {
        if (snprintf(name, size, "%s_%d", prototype, suffix++) >= (int)size) {
            return -1;
        }
    } while (actor_name_used(scene, name));
    return 0;
}

int add_instance_at(app_scene_authoring *scene, const char *prototype_name, const Transform *position)
{
    if (!scene || !prototype_name || !find_prototype(scene, prototype_name)) {
        return -1;
    }
    finish_transform_edit(scene, false);
    char name[256];
    if (next_actor_name(scene, prototype_name, name, sizeof name) != 0) {
        return -1;
    }
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "instance.add", &action_doc);
    uint64_t identity = scene_actor_next_identity(scene);
    yyjson_mut_obj_add_uint(action_doc, action_root, "actor", identity);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "prototype", prototype_name);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "name", name);
    if (position) {
        yyjson_mut_obj_add_real(action_doc, action_root, "x", position->x);
        yyjson_mut_obj_add_real(action_doc, action_root, "y", position->y);
    }
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    bool was_dirty = scene->scene_dirty;
    if (add_actor(scene, name, prototype_name) != 0) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    scene_actor *actor = &alast(scene->actors);
    if (actor->identity != identity) {
        remove_actor_at(scene, acount(scene->actors) - 1);
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    if (position && Transform_has(scene->world, actor->entity)) {
        write_position(scene, actor->entity, *position);
    }
    /* It may be the object a dangling path names. */
    scene_link_paths(scene);
    av_value *state = capture_actor_state(scene, actor);
    av_value *base = prototype_to_value(actor->base);
    scene_actor_edit edit = { .index = acount(scene->actors) - 1,
                              .name = actor->name,
                              .prototype_name = actor->prototype_name,
                              .base = base,
                              .state = state,
                              .created = true };
    edit.identity = actor->identity;
    bool recorded = state && base &&
                    (!collab_record_history() ||
                     scene_history_push_actor(scene->history, actor->entity, &edit));
    av_value_free(state);
    av_value_free(base);
    if (!recorded) {
        remove_actor_at(scene, acount(scene->actors) - 1);
        scene->scene_dirty = was_dirty;
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    scene_action_finish(scene, action, true);
    free(action);
    return 0;
}

int app_scene_authoring_add_instance(app_scene_authoring *scene, const char *prototype_name)
{
    return add_instance_at(scene, prototype_name, NULL);
}

char **instance_behavior_names(app_scene_authoring *scene, ecs_entity entity)
{
    char **names = NULL;
    source_registry *sources = scene->live->runtime->sources;
    for (int i = 0; i < source_attachment_count(sources); i++) {
        source_attachment_info info = source_attachment_at(sources, i);
        if (ecs_entity_to_u64(info.entity) == ecs_entity_to_u64(entity)) {
            apush(names, xstrdup(info.name));
        }
    }
    return names;
}

void free_behavior_names(char **names)
{
    for (int i = 0; i < acount(names); i++) {
        free(names[i]);
    }
    afree(names);
}

int app_scene_authoring_set_instance_behavior(app_scene_authoring *scene, const char *asset_name, bool attached)
{
    if (!scene || !asset_name || !selected_actor(scene)) {
        return -1;
    }
    finish_transform_edit(scene, false);
    scene_actor *actor = selected_actor(scene);
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "instance.behavior", &action_doc);
    yyjson_mut_obj_add_uint(action_doc, action_root, "actor", actor->identity);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "behavior", asset_name);
    yyjson_mut_obj_add_bool(action_doc, action_root, "attached", attached);
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    char **before = instance_behavior_names(scene, actor->entity);
    char **after = NULL;
    bool found = false;
    for (int i = 0; i < acount(before); i++) {
        bool match = strcmp(before[i], asset_name) == 0;
        found |= match;
        if (!match || attached) {
            apush(after, xstrdup(before[i]));
        }
    }
    if (attached && !found) {
        apush(after, xstrdup(asset_name));
    }
    int result = 0;
    if (found != attached) {
        result = source_set_attachments(
            scene->live->runtime->sources,
            actor->entity,
            (const char *const *)after,
            acount(after),
            scene->live->runtime->deferred,
            scene->live->runtime->now,
            scene->live->runtime->delta
        );
        if (result == 0) {
            scene_behavior_edit edit = { .before = (const char *const *)before,
                                         .before_count = acount(before),
                                         .after = (const char *const *)after,
                                         .after_count = acount(after) };
            bool recorded = !collab_record_history() ||
                            scene_history_push_behaviors(scene->history, actor->entity, &edit);
            assert(recorded);
            scene->scene_dirty = true;
            asset_index_invalidate(scene);
            scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
            sync_editor_inspection(scene);
        }
    }
    if (result != 0) {
        snprintf(scene->message, sizeof scene->message, "Could not update behavior %s.", asset_name);
    }
    scene_action_finish(scene, action, result == 0);
    free(action);
    free_behavior_names(before);
    free_behavior_names(after);
    return result;
}

int app_scene_authoring_delete_instance(app_scene_authoring *scene)
{
    if (!scene || !selected_actor(scene)) {
        return -1;
    }
    finish_transform_edit(scene, false);
    scene_actor *actor = selected_actor(scene);
    /* Until the Outliner edits the hierarchy, a parent goes after its children. */
    if (scene_actor_has_children(scene, actor)) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Delete %s's children first.",
            actor->name
        );
        return -1;
    }
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "instance.delete", &action_doc);
    yyjson_mut_obj_add_uint(action_doc, action_root, "actor", actor->identity);
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    actor = selected_actor(scene);
    av_value *state = capture_actor_state(scene, actor);
    av_value *base = prototype_to_value(actor->base);
    if (!state || !base) {
        av_value_free(state);
        av_value_free(base);
        return -1;
    }
    char *name = xstrdup(actor->name), *prototype = xstrdup(actor->prototype_name);
    av_value *retained = scene_retention_save(actor->retained);
    ecs_entity entity = actor->entity;
    scene_actor_edit edit = { .index = scene->selected,
                              .identity = actor->identity,
                              .parent = scene_actor_parent(scene, actor),
                              .name = name,
                              .prototype_name = prototype,
                              .base = base,
                              .state = state,
                              .own_behaviors = actor->own_behaviors,
                              .retained = retained,
                              .paths = scene_actor_paths(scene, actor) };
    int result = remove_actor_at(scene, scene->selected);
    if (!result) {
        /* Paths that named it name nothing now. */
        scene_link_paths(scene);
        /* An object added later under this name starts shown. */
        scene_view_rename(scene, name, NULL);
        bool recorded = !collab_record_history() ||
                        scene_history_push_actor(scene->history, entity, &edit);
        assert(recorded);
        scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    }
    free(name);
    av_value_free(retained);
    av_value_free(edit.paths);
    free(prototype);
    av_value_free(state);
    av_value_free(base);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

static int restore_actor(app_scene_authoring *scene, const scene_actor_edit *edit, ecs_entity *entity)
{
    scene_actor *parent = edit->parent ? scene_actor_by_identity(scene, edit->parent)
                                       : NULL;
    ecs_entity parent_entity = parent ? parent->entity : (ecs_entity){ 0 };
    if ((edit->parent && !parent) || edit->index < 0 ||
        edit->index > acount(scene->actors) ||
        sibling_named(scene, parent_entity, (ecs_entity){ 0 }, edit->name)) {
        return -1;
    }
    char error[256];
    prototype *base = prototype_load_value(edit->base, &scene->registry, error, sizeof error);
    prototype *state = prototype_load_value(edit->state, &scene->registry, error, sizeof error);
    if (!base || !state) {
        prototype_free(base);
        prototype_free(state);
        return -1;
    }
    for (int i = 0; i < acount(state->graphs); i++) {
        if (!source_registered(scene->live->runtime->sources, state->graphs[i])) {
            prototype_free(base);
            prototype_free(state);
            return -1;
        }
    }
    const prototype_spawn_hooks hooks = { attach_prototype_behavior, scene };
    if (!authoring_prototype_parameters(scene->live->runtime->entities, state, error, sizeof error) ||
        prototype_spawn(state, scene->world, &hooks, entity, error, sizeof error) !=
            ECS_OK) {
        prototype_free(base);
        prototype_free(state);
        return -1;
    }
    scene_actor actor = { .entity = *entity,
                          .identity = edit->identity
                                        ? edit->identity
                                        : scene_actor_allocate_identity(scene),
                          .name = xstrdup(edit->name),
                          .prototype_name = xstrdup(edit->prototype_name),
                          .base = base,
                          .restored_state = state,
                          .own_behaviors = edit->own_behaviors };
    if (edit->paths) {
        actor.paths = prototype_components_links(&scene->registry, edit->paths);
    }
    if (scene_retention_load(edit->retained, &actor.retained) != 0 ||
        (parent_entity.slot &&
         (ecs_world_record_add_pair(scene->world, *entity, ECS_CHILD_OF, parent_entity, 0) ||
          ecs_world_apply_commands(scene->world)))) {
        destroy_actor_entity(scene, *entity);
        free_actor(&actor);
        return -1;
    }
    scene_actor_set_identity(scene, &actor, actor.identity);
    int count = acount(scene->actors);
    apush(scene->actors, actor);
    memmove(
        &scene->actors[edit->index + 1],
        &scene->actors[edit->index],
        (size_t)(count - edit->index) * sizeof *scene->actors
    );
    scene->actors[edit->index] = actor;
    scene_link_paths(scene);
    select_instance(scene, edit->index);
    return 0;
}

int app_scene_authoring_duplicate_instance(app_scene_authoring *scene)
{
    if (!selected_actor(scene)) {
        return -1;
    }
    finish_transform_edit(scene, false);
    scene_actor *actor = selected_actor(scene);
    char name[256];
    if (next_actor_name(scene, actor->prototype_name, name, sizeof name) != 0) {
        return -1;
    }
    uint64_t source_identity = actor->identity;
    uint64_t created_identity = scene_actor_next_identity(scene);
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "instance.duplicate", &action_doc);
    yyjson_mut_obj_add_uint(action_doc, action_root, "actor", source_identity);
    yyjson_mut_obj_add_uint(action_doc, action_root, "created_actor", created_identity);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "name", name);
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    actor = selected_actor(scene);
    char *prototype = xstrdup(actor->prototype_name);
    av_value *state = capture_actor_state(scene, actor);
    av_value *base = prototype_to_value(actor->base);
    av_value *retained = scene_retention_save(actor->retained);
    /* The copy is its source's sibling, after the source's children. */
    scene_actor_edit edit = { .index = scene_actor_subtree_end(scene, scene->selected),
                              .identity = created_identity,
                              .parent = scene_actor_parent(scene, actor),
                              .name = name,
                              .prototype_name = prototype,
                              .base = base,
                              .state = state,
                              .retained = retained,
                              .paths = scene_actor_paths(scene, actor),
                              .created = true };
    ecs_entity entity;
    int result = state && base ? restore_actor(scene, &edit, &entity) : -1;
    if (!result) {
        bool recorded = !collab_record_history() ||
                        scene_history_push_actor(scene->history, entity, &edit);
        assert(recorded);
        scene->scene_dirty = true;
        asset_index_invalidate(scene);
        scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    }
    free(prototype);
    av_value_free(state);
    av_value_free(base);
    av_value_free(retained);
    av_value_free(edit.paths);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

/* Removes the object at `index` and adds it back as `state`, an instance of
   `prototype` (`base`), keeping its name, place, identity and retained
   components, and its behaviors list while its prototype stays, as one
   Undo step. Takes `base` and `state`;
   `prototype` may be the object's own, which removing it frees, so it is
   copied first. */
static int replace_actor(
    app_scene_authoring *scene,
    int index,
    const char *prototype,
    av_value *base,
    av_value *state
)
{
    scene_actor *actor = &scene->actors[index];
    /* Removing it would destroy its children. */
    assert(!scene_actor_has_children(scene, actor));
    scene_actor_edit removed = {
        .index = index,
        .identity = actor->identity,
        .parent = scene_actor_parent(scene, actor),
        .name = xstrdup(actor->name),
        .prototype_name = xstrdup(actor->prototype_name),
        .base = prototype_to_value(actor->base),
        .state = capture_actor_state(scene, actor),
        .own_behaviors = actor->own_behaviors,
        .retained = scene_retention_save(actor->retained),
        .paths = scene_actor_paths(scene, actor),
    };
    scene_actor_edit added = removed;
    added.prototype_name = xstrdup(prototype);
    added.base = base;
    added.state = state;
    added.created = true;
    /* A new prototype's instance follows its behaviors. */
    added.own_behaviors = removed.own_behaviors &&
                          !strcmp(added.prototype_name, removed.prototype_name);
    ecs_entity old = actor->entity, entity = { 0 };
    int result = removed.state && added.state ? remove_actor_at(scene, index) : -1;
    if (result == 0 && restore_actor(scene, &added, &entity) != 0) {
        result = restore_actor(scene, &removed, &entity);
        assert(result == 0);
        result = -1;
    }
    if (result == 0 && collab_record_history()) {
        scene_history_begin_group(scene->history);
        scene_history_push_actor(scene->history, old, &removed);
        scene_history_push_actor(scene->history, entity, &added);
        scene_history_end_group(scene->history);
    }
    if (result == 0) {
        scene->scene_dirty = true;
        asset_index_invalidate(scene);
    }
    free((char *)removed.name);
    free((char *)removed.prototype_name);
    free((char *)added.prototype_name);
    av_value_free(removed.base);
    av_value_free(removed.state);
    av_value_free(removed.retained);
    av_value_free(removed.paths);
    av_value_free(added.base);
    av_value_free(added.state);
    return result;
}

int scene_repoint_actor(app_scene_authoring *scene, int index, const char *prototype)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype);
    scene_actor *actor = &scene->actors[index];
    assert(asset && strcmp(actor->prototype_name, prototype));
    return replace_actor(
        scene,
        index,
        prototype,
        prototype_to_value(asset->prototype),
        actor_state_as(scene, actor, asset->prototype->identity)
    );
}

static int edit_instance_component(app_scene_authoring *scene, const ecs_component_desc *desc, bool add)
{
    int index = scene->selected;
    scene_actor *actor = &scene->actors[index];
    return replace_actor(
        scene,
        index,
        actor->prototype_name,
        prototype_to_value(actor->base),
        actor_state_with(scene, actor, actor->base->identity, desc, add)
    );
}

static int collab_instance_component(app_scene_authoring *scene, const char *component_name, bool add)
{
    scene_actor *actor = selected_actor(scene);
    const ecs_component_desc *desc = component_descriptor(scene, component_name);
    if (!actor || !actor->identity || !desc ||
        ecs_component_has(scene->world, actor->entity, desc->id) == add ||
        (!add && prototype_has_component_id(actor->base, desc->id))) {
        return -1;
    }
    if (scene_actor_has_children(scene, actor)) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "%s has children, so its components can't change yet.",
            actor->name
        );
        return -1;
    }
    finish_transform_edit(scene, false);
    finish_property_edit(scene);
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(
        scene,
        add ? "instance.component_add" : "instance.component_remove",
        &action_doc
    );
    yyjson_mut_obj_add_uint(action_doc, action_root, "actor", actor->identity);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "component", component_name);
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = scene->runtime_snapshot_valid
                   ? -1
                   : edit_instance_component(scene, desc, add);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

int app_scene_authoring_add_instance_component(app_scene_authoring *scene, const char *component_name)
{
    return collab_instance_component(scene, component_name, true);
}

int app_scene_authoring_remove_instance_component(app_scene_authoring *scene, const char *component_name)
{
    return collab_instance_component(scene, component_name, false);
}

/* The selected objects' identities, in scene order. The caller frees the
   array. */
static uint64_t *selection_identities(const app_scene_authoring *scene)
{
    uint64_t *identities = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i)) {
            apush(identities, scene->actors[i].identity);
        }
    }
    return identities;
}

/* Selects the objects `identities` names, `active` active. */
static void select_identities(app_scene_authoring *scene, const uint64_t *identities, uint64_t active)
{
    scene->selected = -1;
    for (int i = 0; i < acount(scene->actors); i++) {
        bool marked = false;
        for (int j = 0; j < acount(identities) && !marked; j++) {
            marked = scene->actors[i].identity == identities[j];
        }
        scene->actors[i].marked = marked;
        if (marked && (scene->selected < 0 || scene->actors[i].identity == active)) {
            scene->selected = i;
        }
    }
    scene->runtime_selected = (ecs_entity){ 0 };
}

/* ---------- Selection around history steps ---------- */

/* Unity keeps the selection in its Undo steps; recent steps are enough. */
#define STEP_SELECTIONS_MAX 256

static scene_selection_set selection_now(const app_scene_authoring *scene)
{
    const scene_actor *active = selected_actor((app_scene_authoring *)scene);
    return (scene_selection_set){ .ids = selection_identities(scene),
                                  .active = active ? active->identity : 0 };
}

static void selection_set_free(scene_selection_set *set)
{
    afree(set->ids);
}

static void step_selection_free(scene_step_selection *entry)
{
    selection_set_free(&entry->before);
    selection_set_free(&entry->after);
}

static bool selection_same(const scene_selection_set *a, const scene_selection_set *b)
{
    return a->active == b->active && acount(a->ids) == acount(b->ids) &&
           (!acount(a->ids) ||
            !memcmp(a->ids, b->ids, (size_t)acount(a->ids) * sizeof *a->ids));
}

/* A step the last frame added remembers the selection the frame began with
   and the one it ended with; then this frame's selection is noted, and
   counted when it is not the last frame's. */
void scene_selection_track(app_scene_authoring *scene)
{
    scene_selection_set now = selection_now(scene);
    scene->selection_revision += !selection_same(&scene->frame_selection, &now);
    uint64_t step = scene_history_step_id(scene->history, false);
    if (step > scene->seen_step) {
        if (acount(scene->step_selections) == STEP_SELECTIONS_MAX) {
            step_selection_free(&scene->step_selections[0]);
            memmove(
                scene->step_selections,
                scene->step_selections + 1,
                (STEP_SELECTIONS_MAX - 1) * sizeof *scene->step_selections
            );
            asetlen(scene->step_selections, STEP_SELECTIONS_MAX - 1);
        }
        apush(
            scene->step_selections,
            ((scene_step_selection){ .step = step,
                                     .before = scene->frame_selection,
                                     .after = now })
        );
        scene->frame_selection = (scene_selection_set){ 0 };
        scene->seen_step = step;
        now = selection_now(scene);
    }
    selection_set_free(&scene->frame_selection);
    scene->frame_selection = now;
}

void scene_selection_free(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->step_selections); i++) {
        step_selection_free(&scene->step_selections[i]);
    }
    afree(scene->step_selections);
    selection_set_free(&scene->frame_selection);
}

static const scene_step_selection *step_selection(const app_scene_authoring *scene, uint64_t step)
{
    for (int i = acount(scene->step_selections) - 1; step && i >= 0; i--) {
        if (scene->step_selections[i].step == step) {
            return &scene->step_selections[i];
        }
    }
    return NULL;
}

/* Moves an object by `offset` as one recorded transform edit. */
static void move_actor(app_scene_authoring *scene, ecs_entity entity, Transform offset)
{
    const Transform *position = Transform_get_const(scene->world, entity);
    if (!position) {
        return;
    }
    Transform local = scene_local_offset(scene->world, entity, offset);
    begin_transform_edit(scene, entity);
    write_transform_edit(
        scene,
        entity,
        (Transform){ position->x + local.x, position->y + local.y }
    );
    finish_transform_edit(scene, false);
}

int scene_duplicate_selection(app_scene_authoring *scene, Transform offset)
{
    finish_transform_edit(scene, false);
    uint64_t *sources = selection_identities(scene);
    uint64_t *copies = NULL;
    uint64_t active = selected_actor(scene) ? selected_actor(scene)->identity : 0;
    uint64_t active_copy = 0;
    int result = 0;
    scene_history_begin_group(scene->history);
    for (int i = 0; i < acount(sources) && !result; i++) {
        scene_actor *source = scene_actor_by_identity(scene, sources[i]);
        assert(source);
        scene->selected = (int)(source - scene->actors);
        result = app_scene_authoring_duplicate_instance(scene);
        if (result) {
            break;
        }
        scene_actor *copy = selected_actor(scene);
        move_actor(scene, copy->entity, offset);
        apush(copies, copy->identity);
        active_copy = sources[i] == active ? copy->identity : active_copy;
    }
    scene_history_end_group(scene->history);
    select_identities(scene, result ? sources : copies, result ? active : active_copy);
    afree(sources);
    afree(copies);
    return result;
}

char *scene_copy_selection(app_scene_authoring *scene)
{
    finish_transform_edit(scene, false);
    char name[256];
    scene_document_name(scene, name, sizeof name);
    scene_document *document = scene_document_new(name);
    int *selected = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i)) {
            apush(selected, i);
        }
    }
    int result = scene_document_add_actors(scene, document, selected);
    afree(selected);
    char *text = !result && acount(document->entities)
                   ? scene_document_save(document, project_components())
                   : NULL;
    scene_document_free(document);
    return text;
}

/* A pasted object keeps its name unless another object has it. */
static int paste_name(const app_scene_authoring *scene, const scene_entity *entity, char *name, size_t size)
{
    if (!actor_name_used(scene, entity->name)) {
        snprintf(name, size, "%s", entity->name);
        return 0;
    }
    return next_actor_name(scene, entity->prototype, name, size);
}

/* Spawns the document's objects; on failure none remain. */
static int spawn_pasted(app_scene_authoring *scene, const scene_document *document)
{
    int first = acount(scene->actors);
    for (int i = 0; i < acount(document->entities); i++) {
        const scene_entity *entity = &document->entities[i];
        ecs_entity parent = entity->parent >= 0
                              ? scene->actors[first + entity->parent].entity
                              : (ecs_entity){ 0 };
        char name[256];
        if (paste_name(scene, entity, name, sizeof name) != 0 ||
            scene_spawn_entity(scene, entity, name, parent) != 0) {
            while (acount(scene->actors) > first) {
                remove_actor_at(scene, acount(scene->actors) - 1);
            }
            return -1;
        }
    }
    scene_link_paths(scene);
    return 0;
}

/* The move that lands the objects from `first` on where `place` asks. */
static Transform paste_move(app_scene_authoring *scene, int first, scene_paste_place place)
{
    CF_V2 min = { INFINITY, INFINITY }, max = { -INFINITY, -INFINITY };
    for (int i = first; i < acount(scene->actors); i++) {
        const Transform *at = Transform_get_const(scene->world, scene->actors[i].entity);
        /* Pasted children keep their place under their pasted parents. */
        if (at && !ecs_parent(scene->world, scene->actors[i].entity).slot) {
            min = (CF_V2){ fminf(min.x, at->x), fminf(min.y, at->y) };
            max = (CF_V2){ fmaxf(max.x, at->x), fmaxf(max.y, at->y) };
        }
    }
    Transform move = place.offset;
    if (place.center && min.x <= max.x) {
        move = (Transform){ place.center->x - (min.x + max.x) / 2,
                            place.center->y - (min.y + max.y) / 2 };
    }
    if (place.snap > 0) {
        move.x = roundf(move.x / place.snap) * place.snap;
        move.y = roundf(move.y / place.snap) * place.snap;
    }
    return move;
}

/* Records a pasted object as created, as adding one does. */
static void record_created(app_scene_authoring *scene, int index)
{
    scene_actor *actor = &scene->actors[index];
    av_value *state = capture_actor_state(scene, actor);
    av_value *base = prototype_to_value(actor->base);
    av_value *retained = scene_retention_save(actor->retained);
    av_value *paths = scene_actor_paths(scene, actor);
    scene_actor_edit edit = { .index = index,
                              .identity = actor->identity,
                              .parent = scene_actor_parent(scene, actor),
                              .name = actor->name,
                              .prototype_name = actor->prototype_name,
                              .base = base,
                              .state = state,
                              .retained = retained,
                              .paths = paths,
                              .created = true };
    bool recorded = state && base &&
                    scene_history_push_actor(scene->history, actor->entity, &edit);
    assert(recorded);
    av_value_free(state);
    av_value_free(base);
    av_value_free(retained);
    av_value_free(paths);
}

int scene_paste(app_scene_authoring *scene, const char *text, scene_paste_place place)
{
    char error[256];
    scene_document *document = text && text[0]
                                 ? scene_document_load(text, error, sizeof error)
                                 : NULL;
    if (!document || !acount(document->entities)) {
        snprintf(scene->message, sizeof scene->message, "The clipboard holds no objects.");
        scene_document_free(document);
        return -1;
    }
    /* Collaboration replays named actions, and pasting is not one. */
    if (collab_active()) {
        snprintf(scene->message, sizeof scene->message, "Paste waits until the collaboration ends.");
        scene_document_free(document);
        return -1;
    }
    for (int i = 0; i < acount(document->entities); i++) {
        if (scene_validate_entity(scene, &document->entities[i]) != 0) {
            scene_document_free(document);
            return -1;
        }
    }
    finish_transform_edit(scene, false);
    bool was_dirty = scene->scene_dirty;
    int first = acount(scene->actors);
    uint64_t *selection = selection_identities(scene);
    uint64_t active = selected_actor(scene) ? selected_actor(scene)->identity : 0;
    int spawned = spawn_pasted(scene, document);
    scene_document_free(document);
    if (spawned != 0) {
        select_identities(scene, selection, active);
        scene->scene_dirty = was_dirty;
        afree(selection);
        return -1;
    }
    afree(selection);
    Transform move = paste_move(scene, first, place);
    scene_history_begin_group(scene->history);
    for (int i = first; i < acount(scene->actors); i++) {
        const Transform *at = Transform_get_const(scene->world, scene->actors[i].entity);
        if (at && !ecs_parent(scene->world, scene->actors[i].entity).slot) {
            write_position(
                scene,
                scene->actors[i].entity,
                (Transform){ at->x + move.x, at->y + move.y }
            );
        }
        record_created(scene, i);
        scene->actors[i].marked = true;
    }
    scene_history_end_group(scene->history);
    scene->selected = acount(scene->actors) - 1;
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    return acount(scene->actors) - first;
}

static int actor_index(const app_scene_authoring *scene, ecs_entity entity)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            return i;
        }
    }
    return -1;
}

static void set_actor_name(app_scene_authoring *scene, scene_actor *actor, const char *name)
{
    scene_view_rename(scene, actor->name, name);
    free(actor->name);
    actor->name = xstrdup(name);
    scene_link_paths(scene);
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
}

int app_scene_authoring_rename_instance(app_scene_authoring *scene, ecs_entity entity, const char *name)
{
    int index = actor_index(scene, entity);
    assert(index >= 0);
    if (!strcmp(scene->actors[index].name, name)) {
        return 0;
    }
    if (scene_entity_validate_name(
            name,
            sibling_named(scene, ecs_parent(scene->world, entity), entity, name),
            scene->message,
            sizeof scene->message
        )) {
        return -1;
    }
    finish_transform_edit(scene, false);
    scene_actor *actor = &scene->actors[index];
    uint64_t identity = actor->identity;
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "instance.rename", &action_doc);
    yyjson_mut_obj_add_uint(action_doc, action_root, "actor", identity);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "name", name);
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    /* Stopping respawns the objects, so the index finds it again. */
    actor = scene_actor_by_identity(scene, identity);
    assert(actor);
    scene_path_ref *paths = scene_rename_paths(scene, actor, name);
    bool recorded = !collab_record_history() || scene_history_push_rename(
                                                    scene->history,
                                                    actor->entity,
                                                    actor->name,
                                                    name,
                                                    paths
                                                );
    assert(recorded);
    afree(paths);
    set_actor_name(scene, actor, name);
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    scene_action_finish(scene, action, true);
    free(action);
    return 0;
}

static void apply_physics_settings(app_scene_authoring *scene, physics_settings settings)
{
    preview_runtime *runtime = scene->live->runtime;
    physics_reset(runtime->physics);
    physics3d_reset(runtime->physics3d);
    int result = physics_configure(runtime->physics, settings);
    assert(result == 0);
    result = physics3d_configure(runtime->physics3d, settings);
    assert(result == 0);
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
}

const char *app_scene_authoring_page(const app_scene_authoring *scene)
{
    return scene->ui ? scene->ui : scene->default_ui;
}

static void set_name(char **field, const char *name)
{
    free(*field);
    *field = name && name[0] ? xstrdup(name) : NULL;
}

void app_scene_authoring_set_ui(app_scene_authoring *scene, const char *ui)
{
    set_name(&scene->ui, ui);
    scene->scene_dirty = true;
}

void app_scene_authoring_set_default_ui(app_scene_authoring *scene, const char *ui)
{
    set_name(&scene->default_ui, ui);
}

void app_scene_authoring_set_studio_project(app_scene_authoring *scene)
{
    scene->studio_project = true;
}

int app_scene_authoring_set_physics(app_scene_authoring *scene, physics_settings settings)
{
    if (scene->runtime_snapshot_valid || scene->preview_playing ||
        prototype_preview_entity(scene->prototype_preview).slot ||
        !physics_settings_valid(settings)) {
        return -1;
    }
    finish_transform_edit(scene, false);
    physics_settings previous = physics_configuration(scene->live->runtime->physics);
    if (!scene_history_push_physics(scene->history, previous, settings)) {
        return 0;
    }
    apply_physics_settings(scene, settings);
    return 0;
}

static int replay_scene_edit_one(app_scene_authoring *scene, bool redo)
{
    if (!scene_can_move(scene)) {
        return -1;
    }
    finish_transform_edit(scene, false);
    av_value *hierarchy = scene_history_peek_hierarchy(scene->history, redo);
    if (hierarchy) {
        if (scene_restore_hierarchy(scene, hierarchy) != 0) {
            return -1;
        }
        scene_history_complete_hierarchy(scene->history, redo);
        return 0;
    }
    const physics_settings *physics = scene_history_peek_physics(scene->history, redo);
    if (physics) {
        if (prototype_preview_entity(scene->prototype_preview).slot) {
            return -1;
        }
        apply_physics_settings(scene, *physics);
        scene_history_complete_physics(scene->history, redo);
        return 0;
    }
    const scene_prototype_edit *prototype = scene_history_peek_prototype(scene->history, redo);
    if (prototype) {
        int result = scene_restore_prototype_edit(scene, prototype, redo);
        if (result < 0) {
            return -1;
        }
        scene_history_complete_prototype(scene->history, redo);
        return result;
    }
    ecs_entity entity;
    const char *name = scene_history_peek_rename(scene->history, redo, &entity);
    if (name) {
        int index = actor_index(scene, entity);
        if (index < 0 ||
            sibling_named(scene, ecs_parent(scene->world, entity), entity, name) ||
            !scene_rename_recorded_paths(
                scene,
                &scene->actors[index],
                name,
                scene_history_peek_rename_paths(scene->history, redo)
            )) {
            return -1;
        }
        set_actor_name(scene, &scene->actors[index], name);
        scene_history_complete_rename(scene->history, redo);
        select_instance(scene, index);
        return 0;
    }
    bool present;
    const scene_actor_edit
        *actor_edit = scene_history_peek_actor(scene->history, redo, &entity, &present);
    if (actor_edit) {
        ecs_entity replacement = { 0 };
        int result = -1;
        if (present) {
            if (!ecs_entity_is_alive(scene->world, entity)) {
                result = restore_actor(scene, actor_edit, &replacement);
            }
        } else {
            for (int i = 0; i < acount(scene->actors); i++) {
                if (ecs_entity_to_u64(scene->actors[i].entity) ==
                    ecs_entity_to_u64(entity)) {
                    result = remove_actor_at(scene, i);
                    scene_link_paths(scene);
                    break;
                }
            }
        }
        if (result != 0) {
            return -1;
        }
        scene_history_complete_actor(scene->history, redo, replacement);
        scene->scene_dirty = true;
        asset_index_invalidate(scene);
        return 0;
    }
    const scene_behavior_edit
        *behavior_edit = scene_history_peek_behaviors(scene->history, redo, &entity);
    if (behavior_edit) {
        if (scene->runtime_snapshot_valid) {
            snprintf(scene->message, sizeof scene->message, "Stop the scene before changing behavior attachments.");
            return -1;
        }
        const char *const *names = redo ? behavior_edit->after : behavior_edit->before;
        int count = redo ? behavior_edit->after_count : behavior_edit->before_count;
        if (source_set_attachments(
                scene->live->runtime->sources,
                entity,
                names,
                count,
                scene->live->runtime->deferred,
                scene->live->runtime->now,
                scene->live->runtime->delta
            ) != 0) {
            return -1;
        }
        scene_history_complete_behaviors(scene->history, redo);
        scene->scene_dirty = true;
        asset_index_invalidate(scene);
        for (int i = 0; i < acount(scene->actors); i++) {
            if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
                select_instance(scene, i);
                break;
            }
        }
        sync_editor_inspection(scene);
        return 0;
    }
    int result = redo ? scene_history_redo(scene->history, scene->world, &entity)
                      : scene_history_undo(scene->history, scene->world, &entity);
    if (result > 0) {
        scene->scene_dirty = true;
        asset_index_invalidate(scene);
        for (int i = 0; i < acount(scene->actors); i++) {
            if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
                select_instance(scene, i);
                break;
            }
        }
    }
    return result < 0 ? -1 : 0;
}

int replay_scene_edit(app_scene_authoring *scene, bool redo)
{
    /* Hierarchy snapshots carry their selection through checkpoints too. */
    if (scene_history_peek_hierarchy(scene->history, redo)) {
        return replay_scene_edit_one(scene, redo);
    }
    uint64_t group = scene_history_group_id(scene->history, redo);
    const scene_step_selection
        *recorded = step_selection(scene, scene_history_step_id(scene->history, redo));
    uint64_t *touched = NULL; /* the objects the steps edited or restored */
    int applied = 0;
    bool not_applied = false;
    do {
        int count = acount(scene->actors);
        int result = replay_scene_edit_one(scene, redo);
        if (result < 0) {
            while (applied-- > 0) {
                int restored = replay_scene_edit_one(scene, !redo);
                assert(restored >= 0);
            }
            afree(touched);
            return -1;
        }
        not_applied |= result > 0;
        if (acount(scene->actors) >= count && selected_actor(scene)) {
            apush(touched, selected_actor(scene)->identity);
        }
        applied++;
    } while (group && scene_history_group_id(scene->history, redo) == group);
    /* Undo restores the selection the step began with, Redo the one it
       ended with. */
    if (recorded) {
        const scene_selection_set *set = redo ? &recorded->after : &recorded->before;
        select_identities(scene, set->ids, set->active);
        afree(touched);
        return not_applied ? 1 : 0;
    }
    /* A step of several objects leaves those it kept selected, as it found
       them. */
    int kept = 0;
    for (int i = 0; i < acount(touched); i++) {
        kept += scene_actor_by_identity(scene, touched[i]) != NULL;
    }
    if (kept > 1) {
        select_identities(scene, touched, alast(touched));
    }
    afree(touched);
    return not_applied ? 1 : 0;
}
