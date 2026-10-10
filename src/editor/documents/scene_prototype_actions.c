/* Prototype actions on a scene instance. Apply Overrides to Prototype copies
 * chosen overridden fields into the instance's prototype, as Unity's
 * Overrides dropdown applies them and Unreal's Apply Instance Changes to
 * Blueprint does: the other instances that inherit those fields follow, and
 * one scene Undo step puts the prototype and them back. Save as New
 * Prototype makes a prototype asset of the tuned instance and makes the
 * instance one of it, as Unity's new prefab from an object and Godot's Save
 * Branch as Scene do; Undo makes it an instance of its old prototype again
 * and keeps the new asset, as both editors keep theirs. */
#include "assets/new_asset.h"
#include "editor/collab/collab.h"
#include "editor/workspace/scene_internal.h"
#include "types/array.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FOLDER_MODE 0755

static const game_prototype_component *base_component(const scene_actor *actor, ecs_component_id id)
{
    for (int i = 0; i < acount(actor->base->components); i++) {
        if (actor->base->components[i].desc->id == id) {
            return &actor->base->components[i];
        }
    }
    return NULL;
}

/* Whether the instance's field differs from its prototype's. */
static bool overrides(const app_scene_authoring *scene, const scene_actor *actor, game_prototype_field o)
{
    const game_prototype_component
        *component = base_component(actor, o.component->id);
    pin_value value = { 0 }, inherited = { 0 };
    if (!component ||
        ecs_component_read_field(scene->world, actor->entity, o.component->id, o.field, &value) !=
            ECS_OK) {
        return false;
    }
    pin_read_field(component->data, &o.component->type->fields[o.field], &inherited);
    return !pin_value_equal(&value, &inherited);
}

/* Whether `actor` inherits one of the fields: has its component and keeps
   its prototype's value. */
static bool inherits(
    const app_scene_authoring *scene,
    const scene_actor *actor,
    const game_prototype_field *fields,
    int count
)
{
    for (int i = 0; i < count; i++) {
        if (base_component(actor, fields[i].component->id) &&
            ecs_component_has(scene->world, actor->entity, fields[i].component->id) &&
            !overrides(scene, actor, fields[i])) {
            return true;
        }
    }
    return false;
}

int scene_override_reach(
    const app_scene_authoring *scene,
    const scene_actor *source,
    const game_prototype_field *fields,
    int count
)
{
    int reach = 0;
    for (int i = 0; i < acount(scene->actors); i++) {
        const scene_actor *actor = &scene->actors[i];
        reach += actor != source &&
                 !strcmp(actor->prototype_name, source->prototype_name) &&
                 inherits(scene, actor, fields, count);
    }
    return reach;
}

/* Every instance of the prototype takes it as its base again, which keeps
   their values and makes the ones equal to it inherited. */
static int rebase_instances(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    char error[256];
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (strcmp(actor->prototype_name, asset->prototype->name)) {
            continue;
        }
        game_prototype *base = game_prototype_clone(asset->prototype, error, sizeof error);
        assert(base);
        game_prototype_free(actor->base);
        actor->base = base;
    }
    asset->dirty = 1;
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
    return register_prototype_hosts(scene, asset->prototype);
}

int scene_restore_prototype(app_scene_authoring *scene, const char *name, av_value *value)
{
    scene_prototype_asset *asset = find_prototype(scene, name);
    char error[256];
    game_prototype *prototype = asset
                                  ? game_prototype_load_value(value, &scene->registry, error, sizeof error)
                                  : NULL;
    if (!prototype) {
        return -1;
    }
    game_prototype_free(asset->prototype);
    asset->prototype = prototype;
    return rebase_instances(scene, asset);
}

/* The fields of `actor` that inherit from the prototype take its new values,
   recorded as one step of the open group. */
static void follow_prototype(
    app_scene_authoring *scene,
    scene_actor *actor,
    const game_prototype *prototype,
    const game_prototype_field *fields,
    int count
)
{
    scene_field_edit *edits = NULL;
    for (int i = 0; i < count; i++) {
        game_prototype_field o = fields[i];
        const game_prototype_component *after = NULL;
        for (int c = 0; c < acount(prototype->components); c++) {
            after = prototype->components[c].desc->id == o.component->id
                      ? &prototype->components[c]
                      : after;
        }
        if (!after || !base_component(actor, o.component->id) ||
            !ecs_component_has(scene->world, actor->entity, o.component->id) ||
            overrides(scene, actor, o)) {
            continue;
        }
        pin_value now = { 0 }, next = { 0 };
        ecs_component_read_field(scene->world, actor->entity, o.component->id, o.field, &now);
        pin_read_field(after->data, &o.component->type->fields[o.field], &next);
        scene_field_edit edit = { .component = o.component->id, .field = o.field };
        if (pin_value_equal(&now, &next) ||
            !scene_property_value_copy(&edit.before, now) ||
            !scene_property_value_copy(&edit.after, next)) {
            scene_property_value_free(&edit.before);
            continue;
        }
        ecs_component_write_field(scene->world, actor->entity, o.component->id, o.field, next);
        apush(edits, edit);
    }
    if (acount(edits)) {
        bool recorded = scene_history_push_fields(scene->history, actor->entity, edits, acount(edits));
        assert(recorded);
    }
    for (int i = 0; i < acount(edits); i++) {
        scene_property_value_free(&edits[i].before);
        scene_property_value_free(&edits[i].after);
    }
    afree(edits);
}

int scene_apply_overrides(
    app_scene_authoring *scene,
    scene_actor *source,
    const game_prototype_field *fields,
    int count
)
{
    scene_prototype_asset *asset = find_prototype(scene, source->prototype_name);
    if (!asset || count <= 0 || !scene_editing(scene) || collab_active()) {
        return -1;
    }
    finish_transform_edit(scene, false);
    finish_property_edit(scene);
    av_value *before = game_prototype_to_value(asset->prototype);
    prototype_history_before(scene, asset->prototype);
    game_prototype_apply_instance(asset->prototype, scene->world, source->entity, fields, count);
    av_value *after = game_prototype_to_value(asset->prototype);
    scene_history_begin_group(scene->history);
    scene_history_push_prototype(
        scene->history,
        &(scene_prototype_edit){ asset->prototype->name, before, after }
    );
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (actor != source && !strcmp(actor->prototype_name, source->prototype_name)) {
            follow_prototype(scene, actor, asset->prototype, fields, count);
        }
    }
    scene_history_end_group(scene->history);
    av_value_free(before);
    av_value_free(after);
    return rebase_instances(scene, asset);
}

/* ---------- Save as New Prototype ---------- */

static bool asset_named(const app_scene_authoring *scene, const char *name)
{
    const reference_index *index = app_scene_authoring_asset_index(scene);
    for (int i = 0; index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (!asset->unresolved && asset->name && !strcmp(asset->name, name)) {
            return true;
        }
    }
    return false;
}

static char *prototype_folder(const app_scene_authoring *scene)
{
    return content_path_join(scene->content_root, new_asset_folder(NEW_ASSET_PROTOTYPE));
}

static char *prototype_path(const app_scene_authoring *scene, const char *name)
{
    char *folder = prototype_folder(scene);
    size_t length = strlen(folder) + strlen(name) + sizeof "/" DECLARATIVE_ASSET_EXTENSION;
    char *path = xmalloc(length);
    snprintf(path, length, "%s/%s" DECLARATIVE_ASSET_EXTENSION, folder, name);
    free(folder);
    return path;
}

bool scene_prototype_name_free(app_scene_authoring *scene, const char *name, char *error, size_t size)
{
    if (!new_asset_name_valid(name, error, size)) {
        return false;
    }
    char *path = prototype_path(scene, name);
    struct stat info;
    bool taken = find_prototype(scene, name) || asset_named(scene, name) ||
                 stat(path, &info) == 0;
    free(path);
    if (taken) {
        snprintf(error, size, "%s already exists", name);
    }
    return !taken;
}

/* The tuned instance as a prototype named `name`, as text. */
static char *instance_prototype(app_scene_authoring *scene, const scene_actor *actor, const char *name)
{
    char **behaviors = instance_behavior_names(scene, actor->entity);
    game_prototype *prototype = game_prototype_from_instance(
        name,
        actor->base,
        scene->world,
        actor->entity,
        (const char *const *)behaviors,
        acount(behaviors)
    );
    free_behavior_names(behaviors);
    char *text = prototype ? game_prototype_save(prototype) : NULL;
    game_prototype_free(prototype);
    return text;
}

int scene_save_as_prototype(
    app_scene_authoring *scene,
    scene_actor *actor,
    const char *name,
    char *error,
    size_t size
)
{
    if (!scene_editing(scene) || collab_active()) {
        snprintf(error, size, "Stop the scene and end the collaboration first");
        return -1;
    }
    if (!scene_prototype_name_free(scene, name, error, size)) {
        return -1;
    }
    if (scene_actor_has_children(scene, actor)) {
        snprintf(
            error,
            size,
            "%s has children, so it can't become a prototype yet",
            actor->name
        );
        return -1;
    }
    char *folder = prototype_folder(scene);
    mkdir(folder, FOLDER_MODE);
    free(folder);
    char *path = prototype_path(scene, name);
    finish_transform_edit(scene, false);
    finish_property_edit(scene);
    char *text = instance_prototype(scene, actor, name);
    int index = (int)(actor - scene->actors);
    int result = text && save_content_file(scene, path, text) == 0 &&
                         app_scene_authoring_register_prototype(scene, text, path) == 0
                   ? scene_repoint_actor(scene, index, name)
                   : -1;
    if (result != 0) {
        snprintf(error, size, "Could not save %s", content_basename(path));
    }
    content_refresh(scene);
    asset_index_invalidate(scene);
    free(text);
    free(path);
    return result;
}
