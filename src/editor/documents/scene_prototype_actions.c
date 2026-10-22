#include "editor/documents/prototype_source_input.h"
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
#include "authoring/source_prototype_edit.h"
#include "editor/collab/collab.h"
#include "editor/documents/asset_code.h"
#include "editor/workspace/scene_internal.h"
#include "support/arena.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/xalloc.h"
#include "types/array.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FOLDER_MODE 0755

static const prototype_component *base_component(const scene_actor *actor, ecs_component_id id)
{
    for (int i = 0; i < acount(actor->base->components); i++) {
        if (actor->base->components[i].desc->id == id) {
            return &actor->base->components[i];
        }
    }
    return NULL;
}

/* Whether the instance's field differs from its prototype's. */
static bool overrides(const app_scene_authoring *scene, const scene_actor *actor, prototype_field o)
{
    const prototype_component *component = base_component(actor, o.component->id);
    arena storage = { 0 };
    pin_value value = { 0 }, inherited = { 0 };
    if (!component || ecs_component_read_field(
                          &storage,
                          scene->world,
                          actor->entity,
                          o.component->id,
                          o.field,
                          &value
                      ) != ECS_OK) {
        arena_free(&storage);
        return false;
    }
    bool changed = !pin_read_field(
                       &storage,
                       component->data,
                       &o.component->type->fields[o.field],
                       &inherited
                   ) &&
                   !pin_value_equal(&value, &inherited);
    arena_free(&storage);
    return changed;
}

/* Whether `actor` inherits one of the fields: has its component and keeps
   its prototype's value. */
static bool inherits(
    const app_scene_authoring *scene,
    const scene_actor *actor,
    const prototype_field *fields,
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
    const prototype_field *fields,
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
        prototype *base = prototype_clone(asset->prototype, error, sizeof error);
        assert(base);
        prototype_free(actor->base);
        actor->base = base;
    }
    if (!scene_prototype_source_owned(asset)) {
        asset->dirty = 1;
    }
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
    int result = register_prototype_hosts(scene, asset->prototype);
    if (!scene_prototype_source_owned(asset)) {
        return result;
    }
    if (result != 0) {
        asset->source_not_applied = true;
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "Edited, but not applied: could not update runtime prototype %s.",
            asset->prototype->name
        );
    }
    return asset->source_not_applied ? 1 : 0;
}

int scene_restore_prototype(app_scene_authoring *scene, const char *name, av_value *value)
{
    scene_prototype_asset *asset = find_prototype(scene, name);
    if (asset && scene_prototype_source_owned(asset)) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "Cannot restore %s without its original source text.",
            name
        );
        return -1;
    }
    char error[256];
    prototype *prototype = asset
                             ? prototype_load_value(value, &scene->registry, error, sizeof error)
                             : NULL;
    if (!prototype) {
        return -1;
    }
    prototype_free(asset->prototype);
    asset->prototype = prototype;
    return rebase_instances(scene, asset);
}

int scene_restore_prototype_edit(app_scene_authoring *scene, const scene_prototype_edit *edit, bool redo)
{
    if (!edit->source_before && !edit->source_after) {
        return scene_restore_prototype(scene, edit->name, redo ? edit->after : edit->before);
    }
    scene_prototype_asset *asset = find_prototype(scene, edit->name);
    if (!asset || !scene_prototype_source_owned(asset) ||
        !edit->source_before || !edit->source_after) {
        return -1;
    }
    asset_code *code = scene_prototype_source_session(
        scene,
        asset,
        scene->prototype_message,
        sizeof scene->prototype_message
    );
    if (!code) {
        return -1;
    }
    /* Keep an intervening draft in the prototype's own Undo history. Never
       replace its checkpoint or run full instance refresh: this scene group
       already owns the affected instance field entries and their replay order. */
    int accepted = scene_prototype_source_accept(
        scene,
        asset,
        asset_code_text(code),
        redo ? edit->source_after : edit->source_before,
        false,
        false,
        scene->prototype_message,
        sizeof scene->prototype_message
    );
    if (accepted < 0) {
        return -1;
    }
    return rebase_instances(scene, asset);
}

/* The fields of `actor` that inherit from the prototype take its new values,
   recorded as one step of the open group. */
static bool follow_prototype(
    app_scene_authoring *scene,
    scene_actor *actor,
    const prototype *prototype,
    const prototype_field *fields,
    int count
)
{
    arena storage = { 0 };
    scene_field_edit *edits = NULL;
    bool complete = true;
    for (int i = 0; i < count; i++) {
        prototype_field o = fields[i];
        const prototype_component *after = NULL;
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
        if (ecs_component_read_field(
                &storage,
                scene->world,
                actor->entity,
                o.component->id,
                o.field,
                &now
            ) != ECS_OK) {
            complete = false;
            continue;
        }
        if (pin_read_field(&storage, after->data, &o.component->type->fields[o.field], &next)) {
            complete = false;
            continue;
        }
        if (pin_value_equal(&now, &next)) {
            continue;
        }
        scene_field_edit edit = { .component = o.component->id, .field = o.field };
        if (!scene_property_value_copy(&edit.before, now) ||
            !scene_property_value_copy(&edit.after, next)) {
            scene_property_value_free(&edit.before);
            scene_property_value_free(&edit.after);
            complete = false;
            continue;
        }
        if (ecs_component_write_field(scene->world, actor->entity, o.component->id, o.field, next) !=
            ECS_OK) {
            scene_property_value_free(&edit.before);
            scene_property_value_free(&edit.after);
            complete = false;
            continue;
        }
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
    arena_free(&storage);
    return complete;
}

/* Source-owned Apply is one immutable batch. Only after validation and source
   acceptance do selected inheriting peers follow; the source actor is untouched. */
static int apply_source_overrides(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    scene_actor *source,
    const prototype_field *fields,
    int count
)
{
    asset_code *code = scene_prototype_source_session(
        scene,
        asset,
        scene->prototype_message,
        sizeof scene->prototype_message
    );
    if (!code) {
        return -1;
    }
    if (scene_prototype_source_sync(scene, asset, false) < 0) {
        return -1;
    }
    arena storage = { 0 };
    authoring_prototype_edit *edits = NULL;
    for (int i = 0; i < count; i++) {
        const ecs_component_desc *component = fields[i].component;
        int field = fields[i].field;
        pin_value value = { 0 };
        if (!component || !component->type || field < 0 ||
            field >= component->type->field_count ||
            ecs_component_read_field(
                &storage,
                scene->world,
                source->entity,
                component->id,
                field,
                &value
            ) != ECS_OK) {
            snprintf(scene->prototype_message, sizeof scene->prototype_message, "Could not read the selected instance fields.");
            afree(edits);
            arena_free(&storage);
            return -1;
        }
        apush(
            edits,
            ((authoring_prototype_edit){ .kind = AUTHORING_PROTOTYPE_SET_FIELD,
                                         .component = component->name,
                                         .field = component->type->fields[field].name,
                                         .value = value })
        );
    }
    char *before_text = xstrdup(asset_code_text(code));
    authoring_prototype_edit_result candidate = { 0 };
    bool valid = authoring_prototype_source_edit(
        before_text,
        &scene->registry,
        asset_code_catalog(code),
        edits,
        acount(edits),
        &candidate,
        scene->prototype_message,
        sizeof scene->prototype_message
    );
    afree(edits);
    arena_free(&storage);
    if (!valid || !candidate.changed) {
        authoring_prototype_edit_result_free(&candidate);
        free(before_text);
        return valid ? (asset->source_not_applied ? 1 : 0) : -1;
    }
    av_value *before = prototype_to_value(asset->prototype);
    int accepted = scene_prototype_source_accept(
        scene,
        asset,
        before_text,
        candidate.source,
        false,
        false,
        scene->prototype_message,
        sizeof scene->prototype_message
    );
    if (accepted < 0) {
        av_value_free(before);
        authoring_prototype_edit_result_free(&candidate);
        free(before_text);
        return -1;
    }
    av_value *after = prototype_to_value(asset->prototype);
    scene_history_begin_group(scene->history);
    bool recorded = scene_history_push_prototype(
        scene->history,
        &(scene_prototype_edit){ .name = asset->prototype->name,
                                 .before = before,
                                 .after = after,
                                 .source_before = before_text,
                                 .source_after = candidate.source }
    );
    assert(recorded);
    bool followed = true;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (actor != source && !strcmp(actor->prototype_name, source->prototype_name)) {
            followed &= follow_prototype(scene, actor, asset->prototype, fields, count);
        }
    }
    scene_history_end_group(scene->history);
    av_value_free(before);
    av_value_free(after);
    authoring_prototype_edit_result_free(&candidate);
    free(before_text);
    if (!followed) {
        asset->source_not_applied = true;
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Edited, but not applied: some inheriting instance fields could not be updated.");
    }
    return rebase_instances(scene, asset);
}

int scene_apply_overrides(
    app_scene_authoring *scene,
    scene_actor *source,
    const prototype_field *fields,
    int count
)
{
    scene_source_input_harvest(scene);
    scene_prototype_asset *asset = find_prototype(scene, source->prototype_name);
    if (asset && asset->path && scene_source_input_blocked(scene, asset->path)) {
        return -1;
    }
    if (!asset || count <= 0 || !scene_editing(scene) || collab_active()) {
        return -1;
    }
    finish_transform_edit(scene, false);
    finish_property_edit(scene);
    if (scene_prototype_source_owned(asset)) {
        return apply_source_overrides(scene, asset, source, fields, count);
    }
    av_value *before = prototype_to_value(asset->prototype);
    prototype_history_before(scene, asset->prototype);
    prototype_apply_instance(asset->prototype, scene->world, source->entity, fields, count);
    av_value *after = prototype_to_value(asset->prototype);
    scene_history_begin_group(scene->history);
    scene_history_push_prototype(
        scene->history,
        &(scene_prototype_edit){ .name = asset->prototype->name, .before = before, .after = after }
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
    size_t length = strlen(folder) + strlen(name) + sizeof "/" CODE_ASSET_EXTENSION;
    char *path = xmalloc(length);
    snprintf(path, length, "%s/%s" CODE_ASSET_EXTENSION, folder, name);
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
    prototype *prototype = prototype_from_instance(
        name,
        actor->base,
        scene->world,
        actor->entity,
        (const char *const *)behaviors,
        acount(behaviors)
    );
    free_behavior_names(behaviors);
    char *text = prototype ? prototype_save(prototype) : NULL;
    prototype_free(prototype);
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
