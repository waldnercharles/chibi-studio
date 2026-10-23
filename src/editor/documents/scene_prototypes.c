#include "authoring/native_prototype.h"
#include "authoring/prototype_prepare.h"
#include "editor/collab/actions_scene.h"
#include "editor/documents/prototype_source_input.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "support/asset_json.h"
#include "support/asset_text.h"
#include "support/text_overlay.h"
#include "types/value_document.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Canonical prototypes keep source as the authoring document. The resolved
 * model is accepted together with that text; applying it to live actors is a
 * separate, existing fallible operation. */
bool scene_prototype_source_owned(const scene_prototype_asset *asset)
{
    const char *extension = asset && asset->path ? strrchr(asset->path, '.') : NULL;
    return extension && !strcmp(extension, CODE_ASSET_EXTENSION);
}

scene_prototype_asset *scene_prototype_for_path(app_scene_authoring *scene, const char *path)
{
    for (int i = 0; path && i < acount(scene->prototypes); i++) {
        scene_prototype_asset *asset = &scene->prototypes[i];
        if (asset->path && !strcmp(asset->path, path)) {
            return asset;
        }
    }
    return NULL;
}

bool scene_prototype_dirty(const scene_prototype_asset *asset)
{
    return asset->source ? asset_code_dirty(asset->source) : asset->dirty;
}

void scene_prototype_source_free(scene_prototype_asset *asset)
{
    asset_code_free(asset->source);
    asset->source = NULL;
    free(asset->source_accepted);
    asset->source_accepted = NULL;
    asset->source_not_applied = false;
    for (int i = 0; i < acount(asset->source_pending_behaviors); i++) {
        free(asset->source_pending_behaviors[i]);
    }
    afree(asset->source_pending_behaviors);
}

void scene_prototype_source_capture(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    if (!asset->source) {
        return;
    }
    asset->dirty = asset_code_dirty(asset->source);
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind != APP_EDITOR_DOCUMENT_PROTOTYPE || !tab->path ||
            strcmp(tab->path, asset->path)) {
            continue;
        }
        av_value_free(tab->source_state);
        tab->source_state = asset_code_capture(asset->source);
        tab->source_dirty = asset->dirty;
        tab->source_only = true;
    }
    asset_index_invalidate(scene);
}

static bool source_behavior_named(char *const *names, const char *name)
{
    for (int i = 0; i < acount(names); i++) {
        if (!strcmp(names[i], name)) {
            return true;
        }
    }
    return false;
}

static void source_track_behaviors(scene_prototype_asset *asset, const prototype *next)
{
    const prototype *values[] = { asset->prototype, next };
    for (int side = 0; side < 2; side++) {
        for (int i = 0; i < acount(values[side]->graphs); i++) {
            const char *name = values[side]->graphs[i];
            if (!prototype_has_graph(values[1 - side], name) &&
                !source_behavior_named(asset->source_pending_behaviors, name)) {
                apush(asset->source_pending_behaviors, xstrdup(name));
            }
        }
    }
}

static const prototype_component *source_component(const prototype *value, ecs_component_id id)
{
    for (int i = 0; i < acount(value->components); i++) {
        if (value->components[i].desc->id == id) {
            return &value->components[i];
        }
    }
    return NULL;
}

/* A preexisting model draft is converted before ownership changes. Never
 * replace it with clean disk text or serialize KDL into its source file. */
static bool source_import_model(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    asset_code *code,
    char *error,
    size_t size
)
{
    authoring_prototype_edit_result disk = { 0 }, edited = { 0 };
    const char *text = asset_code_text(code);
    if (!authoring_prototype_source_edit(
            text,
            &scene->registry,
            asset_code_catalog(code),
            NULL,
            0,
            &disk,
            error,
            size
        )) {
        return false;
    }
    authoring_prototype_edit *ops = NULL;
    arena values = { 0 };
    for (int i = 0; i < acount(asset->prototype->components); i++) {
        const prototype_component *next = &asset->prototype->components[i];
        const prototype_component
            *prior = source_component(disk.value, next->desc->id);
        if (!prior) {
            apush(
                ops,
                ((authoring_prototype_edit){
                    .kind = AUTHORING_PROTOTYPE_ADD_COMPONENT,
                    .component = next->desc->name,
                    .value = { .type = PIN_STRUCT,
                               .type_name = next->desc->type->name,
                               .ptr = next->data } })
            );
            continue;
        }
        for (int field = 0; field < next->desc->type->field_count; field++) {
            const type_field *desc = &next->desc->type->fields[field];
            pin_value before = { 0 }, after = { 0 };
            const char *read_error = pin_read_field(&values, prior->data, desc, &before);
            if (!read_error) {
                read_error = pin_read_field(&values, next->data, desc, &after);
            }
            if (read_error) {
                snprintf(error, size, "%s", read_error);
                arena_free(&values);
                afree(ops);
                authoring_prototype_edit_result_free(&disk);
                return false;
            }
            if (!pin_value_equal(&before, &after)) {
                apush(
                    ops,
                    ((authoring_prototype_edit){ .kind = AUTHORING_PROTOTYPE_SET_FIELD,
                                                 .component = next->desc->name,
                                                 .field = desc->name,
                                                 .value = after })
                );
            }
        }
    }
    for (int i = 0; i < acount(disk.value->components); i++) {
        const ecs_component_desc *desc = disk.value->components[i].desc;
        if (!source_component(asset->prototype, desc->id)) {
            apush(
                ops,
                ((authoring_prototype_edit){ .kind = AUTHORING_PROTOTYPE_REMOVE_COMPONENT,
                                             .component = desc->name })
            );
        }
    }
    for (int i = 0; i < acount(asset->prototype->graphs); i++) {
        const char *name = asset->prototype->graphs[i];
        if (!prototype_has_graph(disk.value, name)) {
            apush(
                ops,
                ((authoring_prototype_edit){ .kind = AUTHORING_PROTOTYPE_ATTACH_BEHAVIOR,
                                             .behavior = name })
            );
        }
    }
    for (int i = 0; i < acount(disk.value->graphs); i++) {
        const char *name = disk.value->graphs[i];
        if (!prototype_has_graph(asset->prototype, name)) {
            apush(
                ops,
                ((authoring_prototype_edit){ .kind = AUTHORING_PROTOTYPE_DETACH_BEHAVIOR,
                                             .behavior = name })
            );
        }
    }
    bool ok = authoring_prototype_source_edit(
        text,
        &scene->registry,
        asset_code_catalog(code),
        ops,
        acount(ops),
        &edited,
        error,
        size
    );
    if (ok) {
        av_value *expected = prototype_to_value(asset->prototype);
        av_value *actual = prototype_to_value(edited.value);
        ok = expected && actual && av_equals(expected, actual);
        av_value_free(expected);
        av_value_free(actual);
        if (!ok) {
            snprintf(error, size, "The existing prototype draft has composition that cannot be preserved in source");
        }
    }
    if (ok && edited.changed) {
        ok = asset_code_edit_source(code, text, edited.source, NULL, false);
    }
    afree(ops);
    arena_free(&values);
    authoring_prototype_edit_result_free(&disk);
    authoring_prototype_edit_result_free(&edited);
    return ok;
}

asset_code *scene_prototype_source_session(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    char *error,
    size_t size
)
{
    if (!scene_prototype_source_owned(asset)) {
        return NULL;
    }
    if (asset->source) {
        asset_code_set_registry(asset->source, &scene->registry);
        asset_code_set_source_world(asset->source, scene->live, scene->content_root);
        return asset->source;
    }
    asset_code *code = scene_prototype_source_prepare(scene, asset, error, size);
    if (code) {
        scene_prototype_source_adopt_session(scene, asset, code);
    }
    return code;
}

asset_code *scene_prototype_source_prepare(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    char *error,
    size_t size
)
{
    const av_value *state = NULL;
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->path && !strcmp(tab->path, asset->path)) {
            state = tab->source_state;
            break;
        }
    }
    asset_code *code = asset_code_prepare_prototype(asset->path, state, &scene->registry, error, size);
    if (!code) {
        return NULL;
    }
    asset_code_own_prototype(code);
    asset_code_set_registry(code, &scene->registry);
    asset_code_set_source_world(code, scene->live, scene->content_root);
    if (asset->dirty && !state &&
        !source_import_model(scene, asset, code, error, size)) {
        asset_code_free(code);
        return NULL;
    }
    return code;
}

void scene_prototype_source_adopt_session(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    asset_code *code
)
{
    assert(!asset->source && code);
    asset->source = code;
    asset_code_adopt_overlay(code);
    clear_prototype_history(asset);
    scene_prototype_source_capture(scene, asset);
}

static int source_apply(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    if (refresh_prototype_instances(scene, asset) != 0) {
        return -1;
    }
    source_registry *behaviors = scene->live->runtime->sources;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        if (strcmp(actor->prototype_name, asset->prototype->name)) {
            continue;
        }
        for (int j = 0; j < acount(asset->source_pending_behaviors); j++) {
            const char *name = asset->source_pending_behaviors[j];
            if (!prototype_has_graph(asset->prototype, name)) {
                source_detach(behaviors, actor->entity, name);
                continue;
            }
            if (!actor_has_behavior(behaviors, actor, name) &&
                source_attach(
                    behaviors,
                    actor->entity,
                    name,
                    scene->live->runtime->deferred,
                    scene->live->runtime->now,
                    scene->live->runtime->delta
                ) != 0) {
                snprintf(
                    scene->prototype_message,
                    sizeof scene->prototype_message,
                    "Could not attach %s to %s.",
                    name,
                    actor->name
                );
                return -1;
            }
        }
    }
    for (int i = 0; i < acount(asset->source_pending_behaviors); i++) {
        free(asset->source_pending_behaviors[i]);
    }
    aclear(asset->source_pending_behaviors);
    return 0;
}

static int source_adopt(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    authoring_prototype_edit_result *candidate,
    bool refresh
)
{
    prototype *previous = asset->prototype;
    source_track_behaviors(asset, candidate->value);
    asset->prototype = candidate->value;
    candidate->value = NULL;
    free(asset->source_accepted);
    asset->source_accepted = xstrdup(asset_code_text(asset->source));
    asset_code_publish_static(asset->source);
    if (refresh) {
        asset->source_not_applied = source_apply(scene, asset) != 0;
        asset_code_hold_runtime(asset->source, false);
    }
    prototype_free(previous);
    scene_prototype_source_capture(scene, asset);
    if (asset->source_not_applied) {
        char detail[256];
        snprintf(detail, sizeof detail, "%s", scene->prototype_message);
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Edited, but not applied: %.210s", detail);
        return 1;
    }
    return 0;
}

int scene_prototype_source_accept(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *before,
    const char *after,
    bool coalesce,
    bool refresh,
    char *error,
    size_t size
)
{
    asset_code *code = scene_prototype_source_session(scene, asset, error, size);
    if (!code || strcmp(before, asset_code_text(code))) {
        snprintf(error, size, "Prototype source changed before the edit was accepted");
        return -1;
    }
    authoring_prototype_edit_result candidate = { 0 };
    if (!authoring_prototype_source_edit(
            after,
            &scene->registry,
            asset_code_catalog(code),
            NULL,
            0,
            &candidate,
            error,
            size
        )) {
        return -1;
    }
    if (strcmp(candidate.value->name, asset->prototype->name)) {
        snprintf(error, size, "Prototype source identity changed; reopen the asset");
        authoring_prototype_edit_result_free(&candidate);
        return -1;
    }
    /* Preparation is synchronous against this borrowed dependency snapshot. */
    bool source_changed = strcmp(before, after) != 0;
    if (strcmp(before, asset_code_text(code)) ||
        (source_changed &&
         !asset_code_edit_source(code, before, after, NULL, coalesce))) {
        snprintf(error, size, "Prototype source changed before the edit was accepted");
        authoring_prototype_edit_result_free(&candidate);
        return -1;
    }
    asset_code_validate(code, NULL);
    int result = source_adopt(scene, asset, &candidate, refresh);
    if (source_changed) {
        scene_tabs_ensure_prototype(scene, asset);
        scene_prototype_source_capture(scene, asset);
    }
    authoring_prototype_edit_result_free(&candidate);
    return result;
}

int scene_prototype_source_sync(app_scene_authoring *scene, scene_prototype_asset *asset, bool refresh)
{
    char error[256] = "";
    asset_code *code = scene_prototype_source_session(scene, asset, error, sizeof error);
    if (!code || asset_code_error(code)[0]) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "%s",
            code ? asset_code_error(code) : error
        );
        return -1;
    }
    const char *text = asset_code_text(code);
    if (asset->source_accepted && !strcmp(asset->source_accepted, text) &&
        !(refresh && asset_code_runtime_held(code))) {
        return asset->source_not_applied ? 1 : 0;
    }
    int result = scene_prototype_source_accept(scene, asset, text, text, false, refresh, error, sizeof error);
    if (result < 0) {
        asset_code_reject_static(code, error);
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
    }
    return result;
}

int scene_prototype_source_edit(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const authoring_prototype_edit *ops,
    int count,
    bool coalesce
)
{
    char error[256] = "";
    asset_code *code = scene_prototype_source_session(scene, asset, error, sizeof error);
    if (!code || asset_code_error(code)[0]) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "%s",
            code ? asset_code_error(code) : error
        );
        return -1;
    }
    authoring_prototype_edit_result candidate = { 0 };
    const char *before = asset_code_text(code);
    if (!authoring_prototype_source_edit(
            before,
            &scene->registry,
            asset_code_catalog(code),
            ops,
            count,
            &candidate,
            error,
            sizeof error
        )) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
        return -1;
    }
    int result = scene_prototype_source_accept(
        scene,
        asset,
        before,
        candidate.source,
        coalesce,
        true,
        error,
        sizeof error
    );
    if (result < 0) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
    } else if (candidate.materialized) {
        char detail[256];
        snprintf(detail, sizeof detail, "%s", result > 0 ? scene->prototype_message : "Edited prototype.");
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Inherited composition is now local; Undo restores its bases. %.180s", detail);
    }
    authoring_prototype_edit_result_free(&candidate);
    return result;
}

int scene_prototype_source_save(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    scene_source_input_harvest(scene);
    char error[256] = "";
    asset_code *code = scene_prototype_source_session(scene, asset, error, sizeof error);
    if (!code || scene_source_input_blocked(scene, asset->path)) {
        return -1;
    }
    if (scene_prototype_source_sync(scene, asset, true) < 0) {
        return -1;
    }
    int retried = scene_prototype_source_accept(
        scene,
        asset,
        asset_code_text(code),
        asset_code_text(code),
        false,
        true,
        error,
        sizeof error
    );
    if (retried < 0) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
        return -1;
    }
    int saved_result = scene_source_input_save(scene, asset);
    bool saved = saved_result == 0;
    if (!saved) {
        snprintf(error, sizeof error, "%s", scene->prototype_message);
    }
    /* Accepted formatting and saved source use the same canonical model. */
    int applied = scene_prototype_source_sync(scene, asset, true);
    scene_prototype_source_capture(scene, asset);
    if (!saved) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
        return -1;
    }
    scene_tabs_mark_document_saved(
        scene,
        APP_EDITOR_DOCUMENT_PROTOTYPE,
        asset->prototype->name
    );
    /* Includes intentionally resolve from saved project sources. Refresh that
     * snapshot after writing a base without replacing any dirty dependent. */
    bool refreshed = content_refresh(scene);
    if (applied != 0 || !refreshed) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Saved, but not applied. The accepted source and model are retained.");
        return 1;
    }
    return 0;
}

int scene_prototype_source_reload(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    char error[256] = "";
    if (!asset->source &&
        !scene_prototype_source_session(scene, asset, error, sizeof error)) {
        snprintf(scene->message, sizeof scene->message, "%s", error);
        return -1;
    }
    int result = scene_source_input_reload(scene, asset);
    if (result == APP_PROTOTYPE_EDIT_PENDING) {
        snprintf(scene->message, sizeof scene->message, "%s", scene->prototype_message);
        return -1;
    }
    if (result < 0) {
        snprintf(scene->message, sizeof scene->message, "%s", scene->prototype_message);
        return -1;
    }
    scene_prototype_source_capture(scene, asset);
    snprintf(
        scene->message,
        sizeof scene->message,
        "%s",
        result ? "Reloaded source, but not applied to the runtime." : "Reloaded prototype source."
    );
    return result == APP_PROTOTYPE_EDIT_NOT_APPLIED ? 1 : 0;
}

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
    if (asset && scene_prototype_source_owned(asset) &&
        scene_prototype_source_sync(scene, asset, false) < 0) {
        return -1;
    }
    if (!asset || !prototype_has_graph(asset->prototype, asset_name)) {
        return -1;
    }
    if (scene_prototype_source_owned(asset)) {
        authoring_prototype_edit edit = { .kind = AUTHORING_PROTOTYPE_DETACH_BEHAVIOR,
                                          .behavior = asset_name };
        return scene_prototype_source_edit(scene, asset, &edit, 1, false);
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
        return gate == COLLAB_ACTION_PENDING ? APP_PROTOTYPE_EDIT_PENDING : -1;
    }
    int result = detach_prototype_behavior(scene, prototype_name, asset_name);
    scene_action_finish(scene, action, result >= 0);
    free(action);
    return result > 0 ? APP_PROTOTYPE_EDIT_NOT_APPLIED : result;
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
            arena storage = { 0 };
            pin_value value;
            bool written = !pin_read_field(
                               &storage,
                               component->data,
                               &component->desc->type->fields[field],
                               &value
                           ) &&
                           ecs_component_write_field(
                               scene->world,
                               actor->entity,
                               component->desc->id,
                               field,
                               value
                           ) == ECS_OK;
            arena_free(&storage);
            if (!written) {
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
    if (scene_prototype_source_owned(asset)) {
        authoring_prototype_edit edit = { .kind = AUTHORING_PROTOTYPE_SET_FIELD,
                                          .component = component,
                                          .field = field,
                                          .value = value };
        return scene_prototype_source_edit(scene, asset, &edit, 1, false);
    }
    if (prototype_write_field(asset->prototype, component, field, value) != 0) {
        return -1;
    }
    asset->dirty = 1;
    asset_index_invalidate(scene);
    return refresh_prototype_instances(scene, asset);
}

int app_scene_authoring_read_prototype_field(
    arena *storage,
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component,
    const char *field,
    pin_value *value
)
{
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    if (asset && scene_prototype_source_owned(asset) &&
        scene_prototype_source_sync(
            scene,
            asset,
            !asset->source || !asset_code_runtime_held(asset->source)
        ) < 0) {
        return -1;
    }
    return asset
             ? prototype_read_field(storage, asset->prototype, component, field, value)
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
    if (!scene_prototype_source_owned(asset)) {
        prototype_history_before(scene, asset->prototype);
    }
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
        return gate == COLLAB_ACTION_PENDING ? APP_PROTOTYPE_EDIT_PENDING : -1;
    }
    int result = set_prototype_field(scene, prototype_name, component_name, field_name, value);
    scene_action_finish(scene, action, result >= 0);
    free(action);
    return result > 0 ? APP_PROTOTYPE_EDIT_NOT_APPLIED : result;
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
    if (scene_prototype_source_owned(asset)) {
        char error[256];
        prototype *candidate = prototype_clone(asset->prototype, error, sizeof error);
        if (!candidate ||
            (add && prototype_add_native_component(candidate, desc) != 0)) {
            prototype_free(candidate);
            return -1;
        }
        const prototype_component *component = source_component(candidate, desc->id);
        authoring_prototype_edit edit = {
            .kind = add ? AUTHORING_PROTOTYPE_ADD_COMPONENT
                        : AUTHORING_PROTOTYPE_REMOVE_COMPONENT,
            .component = desc->name,
            .value = { .type = PIN_STRUCT,
                       .type_name = desc->type->name,
                       .ptr = component ? component->data : NULL }
        };
        int result = scene_prototype_source_edit(scene, asset, &edit, 1, false);
        prototype_free(candidate);
        return result;
    }
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
    if (asset && scene_prototype_source_owned(asset) &&
        scene_prototype_source_sync(scene, asset, false) < 0) {
        return -1;
    }
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
    if (asset && scene_prototype_source_owned(asset) &&
        scene_prototype_source_sync(scene, asset, false) < 0) {
        return -1;
    }
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
        return gate == COLLAB_ACTION_PENDING ? APP_PROTOTYPE_EDIT_PENDING : -1;
    }
    int result = add ? add_prototype_component(scene, prototype_name, component_name)
                     : remove_prototype_component(scene, prototype_name, component_name);
    scene_action_finish(scene, action, result >= 0);
    free(action);
    return result > 0 ? APP_PROTOTYPE_EDIT_NOT_APPLIED : result;
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

static int replay_prototype_source(app_scene_authoring *scene, scene_prototype_asset *asset, bool redo)
{
    char error[256];
    asset_code *code = scene_prototype_source_session(scene, asset, error, sizeof error);
    return code
             ? scene_source_input_history(scene, asset, redo, !asset_code_runtime_held(code))
             : -1;
}

int prototype_undo(app_scene_authoring *scene)
{
    scene_prototype_asset *asset = selected_prototype(scene);
    return asset && scene_prototype_source_owned(asset)
             ? replay_prototype_source(scene, asset, false)
         : asset ? restore_prototype(scene, asset, &asset->undo, &asset->redo)
                 : -1;
}

int prototype_redo(app_scene_authoring *scene)
{
    scene_prototype_asset *asset = selected_prototype(scene);
    return asset && scene_prototype_source_owned(asset)
             ? replay_prototype_source(scene, asset, true)
         : asset ? restore_prototype(scene, asset, &asset->redo, &asset->undo)
                 : -1;
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
    const char *extension = strrchr(path, '.');
    if (scene_prototype_source_owned(asset)) {
        if (strcmp(asset->path, path)) {
            snprintf(scene->prototype_message, sizeof scene->prototype_message, "Save the canonical prototype at its source path.");
            return -1;
        }
        return scene_prototype_source_save(scene, asset);
    }
    if (extension && !strcmp(extension, CODE_ASSET_EXTENSION)) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "A model-only prototype cannot be saved as canonical source.");
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
    if (asset && scene_prototype_source_owned(asset) &&
        scene_prototype_source_sync(scene, asset, false) < 0) {
        return -1;
    }
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
    if (scene_prototype_source_owned(asset)) {
        authoring_prototype_edit edit = { .kind = AUTHORING_PROTOTYPE_ATTACH_BEHAVIOR,
                                          .behavior = asset_name };
        return scene_prototype_source_edit(scene, asset, &edit, 1, false);
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
        return gate == COLLAB_ACTION_PENDING ? APP_PROTOTYPE_EDIT_PENDING : -1;
    }
    int result = prototype_attach_behavior(scene, prototype_name, asset_name);
    scene_action_finish(scene, action, result >= 0);
    free(action);
    return result > 0 ? APP_PROTOTYPE_EDIT_NOT_APPLIED : result;
}
