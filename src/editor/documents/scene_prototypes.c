#include "authoring/native_prototype.h"
#include "authoring/prototype_prepare.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "support/asset_json.h"
#include "types/value_document.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int register_prototype_hosts(app_scene_authoring *scene, const prototype *native_prototype)
{
    if (entities_register(scene->live->runtime->entities, native_prototype) != 0 ||
        prototype_preview_register_prototype(scene->prototype_preview, native_prototype) !=
            0) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "Could not update runtime prototype %s.",
            native_prototype->name
        );
        return -1;
    }
    return 0;
}

static int detach_prototype_behavior(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *asset_name
)
{
    if (!scene || !prototype_name || !asset_name) {
        return -1;
    }
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    if (!asset || !prototype_has_graph(asset->prototype, asset_name)) {
        return -1;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        return -1;
    }
    prototype_history_before(scene, asset->prototype);
    if (prototype_remove_graph(asset->prototype, asset_name) != 0) {
        return -1;
    }
    source_registry *behaviors = scene->live->runtime->sources;
    char error[256];
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (strcmp(actor->prototype_name, prototype_name) != 0) {
            continue;
        }
        source_detach(behaviors, actor->entity, asset_name);
        prototype *base = prototype_clone(asset->prototype, error, sizeof error);
        if (!base) {
            snprintf(
                scene->prototype_message,
                sizeof scene->prototype_message,
                "Could not update %s: %s",
                actor->name,
                error
            );
            return -1;
        }
        prototype_free(actor->base);
        actor->base = base;
    }
    asset->dirty = 1;
    asset_index_invalidate(scene);
    if (register_prototype_hosts(scene, asset->prototype) != 0) {
        return -1;
    }
    snprintf(scene->prototype_message, sizeof scene->prototype_message, "Detached %s from %s.", asset_name, prototype_name);
    return 0;
}

int app_scene_authoring_detach_prototype_behavior(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *asset_name
)
{
    if (!scene || !prototype_name || !asset_name) {
        return -1;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "prototype.behavior_detach", &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "prototype", prototype_name);
    yyjson_mut_obj_add_strcpy(doc, root, "behavior", asset_name);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = detach_prototype_behavior(scene, prototype_name, asset_name);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

bool prototype_has_component_id(const prototype *native_prototype, ecs_component_id id)
{
    for (int i = 0; i < acount(native_prototype->components); i++) {
        if (native_prototype->components[i].desc->id == id) {
            return true;
        }
    }
    return false;
}

static int refresh_prototype_actor(
    app_scene_authoring *scene,
    const prototype *native_prototype,
    scene_actor *actor
)
{
    char error[256];
    av_value *overrides = prototype_capture_overrides_value(
        actor->base,
        scene->world,
        actor->entity
    );
    if (!overrides) {
        return -1;
    }
    for (int i = 0; i < acount(actor->base->components); i++) {
        const ecs_component_desc *desc = actor->base->components[i].desc;
        if (prototype_has_component_id(native_prototype, desc->id) ||
            !ecs_component_has(scene->world, actor->entity, desc->id)) {
            continue;
        }
        scene_retention_remove(&actor->retained, desc->name);
        av_value *values = prototype_entity_component_value(
            scene->world,
            actor->entity,
            desc,
            &scene->registry
        );
        av_value *defaults = prototype_component_value(actor->base, desc->name);
        assert(values && defaults);
        /* Only the instance's own values are worth keeping for the
           component's return; the prototype's come back with it. */
        if (av_equals(values, defaults)) {
            av_value_free(values);
            av_value_free(defaults);
            continue;
        }
        scene_retained_component retained = { .name = xstrdup(desc->name),
                                              .values = values,
                                              .defaults = defaults,
                                              .automatic = true };
        apush(actor->retained, retained);
        scene->scene_dirty = true;
    }
    for (int i = 0; i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        if (desc && prototype_has_component_id(actor->base, desc->id) &&
            !prototype_has_component_id(native_prototype, desc->id) &&
            ecs_component_has(scene->world, actor->entity, desc->id) &&
            ecs_world_record_remove(scene->world, actor->entity, desc->id, (uint64_t)i + 1) !=
                ECS_OK) {
            av_value_free(overrides);
            return -1;
        }
    }
    for (int i = 0; i < acount(native_prototype->components); i++) {
        const prototype_component *component = &native_prototype->components[i];
        if (!ecs_component_has(scene->world, actor->entity, component->desc->id) &&
            ecs_world_record_add(scene->world, actor->entity, component->desc->id, NULL, (uint64_t)i + 1) !=
                ECS_OK) {
            av_value_free(overrides);
            return -1;
        }
    }
    if (ecs_world_apply_commands(scene->world) != ECS_OK) {
        av_value_free(overrides);
        return -1;
    }
    for (int i = 0; i < acount(native_prototype->components); i++) {
        const prototype_component *component = &native_prototype->components[i];
        for (int field = 0; field < component->desc->type->field_count; field++) {
            pin_value value;
            pin_read_field(component->data, &component->desc->type->fields[field], &value);
            if (ecs_component_write_field(
                    scene->world,
                    actor->entity,
                    component->desc->id,
                    field,
                    value
                ) != ECS_OK) {
                av_value_free(overrides);
                return -1;
            }
        }
    }
    if (prototype_apply_components_value(
            scene->world,
            actor->entity,
            overrides,
            &scene->registry,
            &scene->component_strings,
            error,
            sizeof error
        ) != 0) {
        av_value_free(overrides);
        return -1;
    }
    for (int i = 0; i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        if (desc && prototype_has_component_id(actor->base, desc->id) &&
            !prototype_has_component_id(native_prototype, desc->id) &&
            ecs_component_has(scene->world, actor->entity, desc->id) &&
            ecs_world_record_remove(scene->world, actor->entity, desc->id, (uint64_t)i + 1) !=
                ECS_OK) {
            av_value_free(overrides);
            return -1;
        }
    }
    if (ecs_world_apply_commands(scene->world) != ECS_OK) {
        av_value_free(overrides);
        return -1;
    }
    if (restore_retained_components(scene, actor, native_prototype) != 0) {
        av_value_free(overrides);
        return -1;
    }
    prototype *base = prototype_clone(native_prototype, error, sizeof error);
    if (!base) {
        av_value_free(overrides);
        return -1;
    }
    prototype_free(actor->base);
    actor->base = base;
    av_value_free(overrides);
    return 0;
}

int refresh_prototype_instances(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        return -1;
    }
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (strcmp(actor->prototype_name, asset->prototype->name) != 0) {
            continue;
        }
        if (refresh_prototype_actor(scene, asset->prototype, actor) != 0) {
            snprintf(
                scene->prototype_message,
                sizeof scene->prototype_message,
                "Could not update instance %s.",
                actor->name
            );
            return -1;
        }
    }
    /* Refreshing rewrote the instances' handles from the prototype. */
    scene_link_paths(scene);
    return register_prototype_hosts(scene, asset->prototype);
}

int write_prototype_field(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *component,
    const char *field,
    pin_value value
)
{
    if (prototype_write_field(asset->prototype, component, field, value) != 0) {
        return -1;
    }
    asset->dirty = 1;
    asset_index_invalidate(scene);
    return refresh_prototype_instances(scene, asset);
}

int app_scene_authoring_read_prototype_field(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component,
    const char *field,
    pin_value *value
)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    return asset ? prototype_read_field(asset->prototype, component, field, value)
                 : -1;
}

static int set_prototype_field(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name,
    const char *field_name,
    pin_value value
)
{
    if (!scene || !prototype_name || !component_name || !field_name) {
        return -1;
    }
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    if (!asset) {
        return -1;
    }
    prototype_history_before(scene, asset->prototype);
    return write_prototype_field(scene, asset, component_name, field_name, value);
}

int app_scene_authoring_set_prototype_field(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name,
    const char *field_name,
    pin_value value
)
{
    if (!scene || !prototype_name || !component_name || !field_name) {
        return -1;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "prototype.field", &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "prototype", prototype_name);
    yyjson_mut_obj_add_strcpy(doc, root, "component", component_name);
    yyjson_mut_obj_add_strcpy(doc, root, "field", field_name);
    av_doc *native = av_doc_new();
    av_value *native_root = av_obj(native);
    pin_value_write_document(native, native_root, "value", value.type, value.type_name, &value);
    yyjson_mut_obj_add_val(doc, root, "value", av_to_json(doc, av_obj_get(native_root, "value")));
    av_doc_free(native);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = set_prototype_field(scene, prototype_name, component_name, field_name, value);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

const ecs_component_desc *component_descriptor(const app_scene_authoring *scene, const char *name)
{
    for (int i = 0; i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        if (desc && strcmp(desc->name, name) == 0) {
            return desc;
        }
    }
    return NULL;
}

int restore_retained_components(app_scene_authoring *scene, scene_actor *actor, const prototype *native_prototype)
{
    for (int i = 0; i < acount(actor->retained); i++) {
        scene_retained_component *retained = &actor->retained[i];
        const ecs_component_desc *desc = component_descriptor(scene, retained->name);
        assert(desc);
        if (!retained->automatic) {
            if (ecs_component_has(scene->world, actor->entity, desc->id) &&
                (ecs_world_record_remove(scene->world, actor->entity, desc->id, 0) != ECS_OK ||
                 ecs_world_apply_commands(scene->world) != ECS_OK)) {
                return -1;
            }
            continue;
        }
        if (!prototype_has_component_id(native_prototype, desc->id)) {
            continue;
        }
        av_value *defaults = prototype_component_value(native_prototype, desc->name);
        av_value *values = prototype_rebase_component_value(
            retained->values,
            retained->defaults,
            defaults
        );
        av_value_free(defaults);
        int result = values ? apply_components_value(scene, actor->entity, values)
                            : -1;
        av_value_free(values);
        if (result != 0) {
            return result;
        }
        scene_retention_remove(&actor->retained, desc->name);
        i--;
        scene->scene_dirty = true;
    }
    return 0;
}

/* Adds or removes a component as one step of the prototype's Undo; its
   instances follow. */
static int edit_components(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const ecs_component_desc *desc,
    bool add
)
{
    prototype_history_before(scene, asset->prototype);
    int result = add ? prototype_add_native_component(asset->prototype, desc)
                     : prototype_remove_component(asset->prototype, desc->name);
    assert(result == 0);
    asset->dirty = 1;
    asset_index_invalidate(scene);
    return refresh_prototype_instances(scene, asset);
}

static int add_prototype_component(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name
)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    const ecs_component_desc *desc = component_descriptor(scene, component_name);
    if (!asset || !desc || prototype_has_component_id(asset->prototype, desc->id)) {
        return -1;
    }
    return edit_components(scene, asset, desc, true);
}

static int remove_prototype_component(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name
)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    const ecs_component_desc *desc = component_descriptor(scene, component_name);
    if (!asset || !desc || !prototype_has_component_id(asset->prototype, desc->id)) {
        return -1;
    }
    return edit_components(scene, asset, desc, false);
}

static int collab_component_edit(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name,
    const char *operation,
    bool add
)
{
    if (!scene || !prototype_name || !component_name) {
        return -1;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, operation, &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "prototype", prototype_name);
    yyjson_mut_obj_add_strcpy(doc, root, "component", component_name);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = add ? add_prototype_component(scene, prototype_name, component_name)
                     : remove_prototype_component(scene, prototype_name, component_name);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

int app_scene_authoring_add_prototype_component(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name
)
{
    return collab_component_edit(scene, prototype_name, component_name, "prototype.component_add", true);
}

int app_scene_authoring_remove_prototype_component(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name
)
{
    return collab_component_edit(scene, prototype_name, component_name, "prototype.component_remove", false);
}

static int restore_prototype(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    av_value ***from,
    av_value ***to
)
{
    int last = acount(*from) - 1;
    if (last < 0) {
        return 0;
    }
    char error[256];
    av_value *current = prototype_to_value(asset->prototype);
    av_value *snapshot = (*from)[last];
    prototype *native_prototype = prototype_load_value(snapshot, &scene->registry, error, sizeof error);
    if (!native_prototype) {
        av_value_free(current);
        return -1;
    }
    adel(*from, last);
    apush(*to, current);
    prototype_free(asset->prototype);
    asset->prototype = native_prototype;
    asset->dirty = 1;
    asset_index_invalidate(scene);
    if (refresh_prototype_instances(scene, asset) != 0) {
        av_value_free(snapshot);
        return -1;
    }
    av_value_free(snapshot);
    return 0;
}

int prototype_undo(app_scene_authoring *scene)
{
    scene_prototype_asset *asset = selected_prototype(scene);
    return asset ? restore_prototype(scene, asset, &asset->undo, &asset->redo) : -1;
}

int prototype_redo(app_scene_authoring *scene)
{
    scene_prototype_asset *asset = selected_prototype(scene);
    return asset ? restore_prototype(scene, asset, &asset->redo, &asset->undo) : -1;
}

static int append_prototype_asset(app_scene_authoring *scene, prototype *native_prototype, const char *path)
{
    if (!scene || !native_prototype || !native_prototype->name ||
        find_prototype(scene, native_prototype->name)) {
        prototype_free(native_prototype);
        return -1;
    }
    scene_prototype_asset asset = {
        .path = path ? xstrdup(path) : NULL,
        .prototype = native_prototype,
        .dirty = path ? 0 : 1,
    };
    if (register_prototype_hosts(scene, native_prototype) != 0) {
        free(asset.path);
        prototype_free(native_prototype);
        return -1;
    }
    apush(scene->prototypes, asset);
    if (scene->selected_prototype < 0) {
        select_prototype(scene, 0);
    }
    return 0;
}

int app_scene_authoring_register_prototype(app_scene_authoring *scene, const char *json, const char *path)
{
    if (!scene || !json) {
        return -1;
    }
    char error[256];
    prototype *native_prototype = prototype_load(json, &scene->registry, error, sizeof error);
    return append_prototype_asset(scene, native_prototype, path);
}

int app_scene_authoring_save_prototype(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *path
)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    if (!asset || !asset->prototype || !path) {
        return -1;
    }
    if (scene_tabs_path_in_use(scene, APP_EDITOR_DOCUMENT_PROTOTYPE, path)) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Prototype path is already open.");
        return -1;
    }
    char *json = prototype_save(asset->prototype);
    if (!json) {
        return -1;
    }
    int result = save_content_file(scene, path, json);
    free(json);
    if (result == 0) {
        char *saved_path = xstrdup(path);
        free(asset->path);
        asset->path = saved_path;
        asset->dirty = 0;
        content_refresh(scene);
        scene_tabs_mark_document_saved(
            scene,
            APP_EDITOR_DOCUMENT_PROTOTYPE,
            asset->prototype->name
        );
    }
    return result;
}

static int prototype_attach_behavior(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *asset_name
)
{
    if (!scene || !prototype_name || !asset_name) {
        return -1;
    }
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    source_registry *behaviors = scene->live->runtime->sources;
    if (!asset || !asset->prototype) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Prototype not found.");
        return -1;
    }
    if (!source_registered(behaviors, asset_name)) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "Behavior asset %s is not registered.",
            asset_name
        );
        return -1;
    }
    if (prototype_has_graph(asset->prototype, asset_name)) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "%s is already attached to %s.",
            asset_name,
            prototype_name
        );
        return 0;
    }
    if (scene->runtime_snapshot_valid &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) !=
            0) {
        return -1;
    }
    prototype_history_before(scene, asset->prototype);
    if (prototype_add_graph(asset->prototype, asset_name) != 0) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "Could not attach %s to %s.",
            asset_name,
            prototype_name
        );
        return -1;
    }

    scene_actor **actors = NULL;
    prototype **snapshots = NULL;
    scene_actor **attached = NULL;
    char error[256];
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (strcmp(actor->prototype_name, prototype_name) != 0) {
            continue;
        }
        prototype *snapshot = prototype_clone(asset->prototype, error, sizeof error);
        if (!snapshot) {
            snprintf(
                scene->prototype_message,
                sizeof scene->prototype_message,
                "Could not update %s: %s",
                prototype_name,
                error
            );
            free_behavior_snapshots(snapshots);
            prototype_remove_graph(asset->prototype, asset_name);
            afree(actors);
            afree(attached);
            return -1;
        }
        apush(actors, actor);
        apush(snapshots, snapshot);
    }
    for (int i = 0; i < acount(actors); i++) {
        if (actor_has_behavior(behaviors, actors[i], asset_name)) {
            continue;
        }
        if (source_attach(
                behaviors,
                actors[i]->entity,
                asset_name,
                scene->live->runtime->deferred,
                scene->live->runtime->now,
                scene->live->runtime->delta
            ) != 0) {
            snprintf(
                scene->prototype_message,
                sizeof scene->prototype_message,
                "Could not attach %s to spawned actors.",
                asset_name
            );
            for (int j = 0; j < acount(attached); j++) {
                source_detach(behaviors, attached[j]->entity, asset_name);
            }
            free_behavior_snapshots(snapshots);
            prototype_remove_graph(asset->prototype, asset_name);
            afree(actors);
            afree(attached);
            return -1;
        }
        apush(attached, actors[i]);
    }
    for (int i = 0; i < acount(actors); i++) {
        prototype_free(actors[i]->base);
        actors[i]->base = snapshots[i];
        snapshots[i] = NULL;
    }
    afree(actors);
    afree(attached);
    afree(snapshots);
    asset->dirty = 1;
    asset_index_invalidate(scene);
    if (register_prototype_hosts(scene, asset->prototype) != 0) {
        return -1;
    }
    snprintf(scene->prototype_message, sizeof scene->prototype_message, "Attached %s to %s.", asset_name, prototype_name);
    return 0;
}

int app_scene_authoring_attach_prototype_behavior(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *asset_name
)
{
    if (!scene || !prototype_name || !asset_name) {
        return -1;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "prototype.behavior_attach", &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "prototype", prototype_name);
    yyjson_mut_obj_add_strcpy(doc, root, "behavior", asset_name);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = prototype_attach_behavior(scene, prototype_name, asset_name);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}
