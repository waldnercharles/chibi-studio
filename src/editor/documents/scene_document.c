#include "assets/scene_document.h"

#include "authoring/behavior_loader.h"
#include "authoring/preview_source.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "runtime/declared_components.h"
#include "support/asset_text.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The document is named after its file, so saving never renames it. */
void scene_document_name(const app_scene_authoring *scene, char *name, size_t size)
{
    snprintf(name, size, "Scene");
    if (scene->scene_path && scene->scene_path[0]) {
        snprintf(name, size, "%s", content_basename(scene->scene_path));
        char *extension = strrchr(name, '.');
        if (extension && extension != name) {
            *extension = '\0';
        }
    }
}

int scene_document_add_actor(app_scene_authoring *scene, game_scene_document *document, const scene_actor *actor)
{
    game_preview_runtime *runtime = scene->live->runtime;
    game_source_registry *behaviors = runtime->sources;
    const game_prototype *snapshot = scene->runtime_snapshot_valid
                                       ? actor->runtime_snapshot
                                       : NULL;
    const char **names = NULL;
    if (snapshot) {
        for (int j = 0; j < acount(snapshot->graphs); j++) {
            apush(names, snapshot->graphs[j]);
        }
    }
    if (!snapshot) {
        for (int j = 0; j < game_source_attachment_count(behaviors); j++) {
            game_source_attachment_info info = game_source_attachment_at(behaviors, j);
            if (ecs_entity_to_u64(actor->entity) == ecs_entity_to_u64(info.entity)) {
                apush(names, info.name);
            }
        }
    }
    int index = acount(document->entities);
    int result = game_scene_document_add_entity(
        document,
        actor->name,
        actor->prototype_name,
        names,
        acount(names)
    );
    afree(names);
    if (result == 0) {
        av_value *components = snapshot
                                 ? game_prototype_overrides_value(actor->base, snapshot)
                                 : game_prototype_capture_overrides_value(
                                       actor->base,
                                       scene->world,
                                       actor->entity
                                   );
        result = game_scene_document_set_entity_components(document, index, components);
        av_value_free(components);
    }
    if (result == 0) {
        av_value *retained = scene_retention_save(actor->retained);
        result = game_scene_document_set_entity_retained(document, index, retained);
        av_value_free(retained);
    }
    return result;
}

game_scene_document *scene_authoring_document(app_scene_authoring *scene)
{
    char name[256];
    scene_document_name(scene, name, sizeof name);
    game_scene_document *document = game_scene_document_new(name);
    if (!document) {
        return NULL;
    }
    document->perspective = scene->perspective;
    game_scene_document_set_ui(document, scene->ui);
    document->physics = game_physics_configuration(scene->live->runtime->physics);
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_document_add_actor(scene, document, &scene->actors[i]) != 0) {
            game_scene_document_free(document);
            return NULL;
        }
    }
    return document;
}

av_value *scene_authoring_scene_value(app_scene_authoring *scene)
{
    finish_transform_edit(scene, false);
    game_scene_document *document = scene_authoring_document(scene);
    av_value *value = document
                        ? game_scene_document_to_value(
                              document,
                              project_components()
                          )
                        : NULL;
    game_scene_document_free(document);
    return value;
}

char *scene_authoring_scene_text(app_scene_authoring *scene)
{
    av_value *value = scene_authoring_scene_value(scene);
    char *text = value ? asset_text_write(value) : NULL;
    av_value_free(value);
    return text;
}

int app_scene_authoring_save(app_scene_authoring *scene, const char *path)
{
    if (!scene || !path) {
        return -1;
    }
    if (scene_tabs_path_in_use(scene, APP_EDITOR_DOCUMENT_SCENE, path)) {
        snprintf(scene->message, sizeof scene->message, "Scene path is already open.");
        return -1;
    }
    char *json = scene_authoring_scene_text(scene);
    if (!json) {
        return -1;
    }
    int result = save_content_file(scene, path, json);
    free(json);
    if (result == 0) {
        scene_view_save_as(scene, path);
        char *saved_path = xstrdup(path);
        free(scene->scene_path);
        scene->scene_path = saved_path;
        scene->scene_dirty = false;
        scene_tabs_sync_current(scene);
    }
    return result;
}

char *read_scene_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        if (file) {
            fclose(file);
        }
        return NULL;
    }
    long size = ftell(file);
    if (fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    char *json = (char *)malloc((size_t)size + 1);
    if (!json) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(json, 1, (size_t)size, file);
    fclose(file);
    json[read] = '\0';
    return json;
}

static int load_content_prototype(app_scene_authoring *scene, const char *path)
{
    char *json = read_scene_file(path);
    if (!json) {
        return 0;
    }
    int result = app_scene_authoring_register_prototype(scene, json, path) == 0;
    free(json);
    return result;
}

int app_scene_authoring_load_content(app_scene_authoring *scene)
{
    if (!scene) {
        return -1;
    }
    content_refresh(scene);
    int loaded = app_behavior_fixtures_load(
        scene->live,
        scene->prototype_preview,
        scene->content_root
    );
    if (loaded < 0) {
        loaded = 0;
    }
    const char *runtime_folders[] = {
        "libraries", "shader-libraries", "events", "systems",   "fields",
        "materials", "shapes",           "models", "behaviors", "prototypes"
    };
    const char *first_runtime_asset = NULL;
    for (int i = 0; i < acount(scene->content); i++) {
        scene_content_entry *entry = &scene->content[i];
        if (entry->directory) {
            continue;
        }
        for (int j = 0; j < (int)(sizeof runtime_folders / sizeof *runtime_folders);
             j++) {
            if (content_path_in_folder(scene, entry->path, runtime_folders[j])) {
                if (!first_runtime_asset) {
                    first_runtime_asset = entry->path;
                }
                break;
            }
        }
        if (content_path_in_folder(scene, entry->path, "prototypes")) {
            loaded += load_content_prototype(scene, entry->path);
        }
    }
    if (loaded || !first_runtime_asset) {
        return 0;
    }
    snprintf(scene->message, sizeof scene->message, "Could not load runtime content: %s", first_runtime_asset);
    return -1;
}

static scene_prototype_asset *ensure_prototype(app_scene_authoring *scene, const char *name)
{
    scene_prototype_asset *asset = find_prototype(scene, name);
    if (asset || !scene || !scene->resolve_prototype) {
        return asset;
    }
    const char *json = scene->resolve_prototype(name, scene->resolve_user);
    if (!json || app_scene_authoring_register_prototype(scene, json, NULL) != 0) {
        return NULL;
    }
    return find_prototype(scene, name);
}

int apply_components_value(app_scene_authoring *scene, ecs_entity entity, av_value *components)
{
    if (!components) {
        return 0;
    }
    char error[256];
    game_prototype_registry registry = app_component_registry();
    if (game_prototype_apply_components_value(
            scene->world,
            entity,
            components,
            &registry,
            &scene->component_strings,
            error,
            sizeof error
        ) != 0) {
        snprintf(scene->message, sizeof scene->message, "Could not apply overrides: %s", error);
        return -1;
    }
    return 0;
}

static int validate_scene_behaviors(app_scene_authoring *scene, char **names)
{
    game_source_registry *world = scene->live->runtime->sources;
    const authoring_sources *applied = game_preview_sources_applied(scene->live);
    for (int i = 0; i < acount(names); i++) {
        if (!game_source_registered(world, names[i])) {
            snprintf(scene->message, sizeof scene->message, "Unknown behavior asset %s.", names[i]);
            return -1;
        }
        for (int j = 0; applied && j < authoring_sources_count(applied); j++) {
            const authoring_source *source = authoring_sources_at(applied, j);
            if (source->analysis && !strcmp(source->analysis->name, names[i]) &&
                source->analysis->owner_type !=
                    authoring_sources_catalog(applied)->entity_type) {
                snprintf(
                    scene->message,
                    sizeof scene->message,
                    "Behavior %s cannot attach to a scene entity.",
                    names[i]
                );
                return -1;
            }
        }
    }
    return 0;
}

int scene_validate_entity(app_scene_authoring *scene, const game_scene_entity *entity)
{
    scene_retained_component *retained = NULL;
    bool valid = scene_retention_load(entity->retained, &retained) == 0 &&
                 scene_retention_validate(retained, &scene->registry);
    scene_retention_free(retained);
    if (!valid) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Invalid retained components for %s.",
            entity->name
        );
        return -1;
    }
    scene_prototype_asset *asset = ensure_prototype(scene, entity->prototype);
    if (!asset) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Unknown scene prototype %s.",
            entity->prototype
        );
        return -1;
    }
    if (validate_scene_behaviors(scene, asset->prototype->graphs) != 0 ||
        validate_scene_behaviors(scene, entity->behaviors) != 0) {
        return -1;
    }
    /* Exercise the same component parser without mutating the open Scene. */
    ecs_world *world = ecs_world_new();
    int result = 0;
    char error[256] = { 0 };
    char **component_strings = NULL;
    for (int i = 0; i < scene->registry.component_count; i++) {
        if (ecs_world_register_component(world, scene->registry.components[i], NULL) !=
            ECS_OK) {
            snprintf(error, sizeof error, "could not register scene components");
            result = -1;
            break;
        }
    }
    ecs_entity actor;
    if (result == 0) {
        result = game_prototype_spawn_components(asset->prototype, world, &actor, error, sizeof error);
    }
    if (result == 0 && entity->components) {
        result = game_prototype_apply_components_value(
            world,
            actor,
            entity->components,
            &scene->registry,
            &component_strings,
            error,
            sizeof error
        );
    }
    ecs_world_free(world);
    game_prototype_owned_strings_free(component_strings);
    if (result != 0) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Could not load scene entity %s: %s",
            entity->name,
            error
        );
    }
    return result;
}

int scene_spawn_entity(app_scene_authoring *scene, const game_scene_entity *entity, const char *name)
{
    if (add_actor(scene, name, entity->prototype) != 0) {
        return -1;
    }
    scene_actor *actor = &alast(scene->actors);
    if (scene_retention_load(entity->retained, &actor->retained) != 0 ||
        apply_components_value(scene, actor->entity, entity->components) != 0 ||
        restore_retained_components(scene, actor, actor->base) != 0) {
        return -1;
    }
    game_preview_runtime *runtime = scene->live->runtime;
    game_source_registry *behaviors = runtime->sources;
    game_source_detach_entity(behaviors, actor->entity);
    for (int j = 0; j < acount(entity->behaviors); j++) {
        if (game_source_attach(
                behaviors,
                actor->entity,
                entity->behaviors[j],
                runtime->deferred,
                runtime->now,
                runtime->delta
            ) != 0) {
            snprintf(scene->message, sizeof scene->message, "Could not attach scene behavior.");
            return -1;
        }
    }
    return 0;
}

int scene_authoring_load_value(app_scene_authoring *scene, av_value *value, const char *path)
{
    char error[256];
    game_scene_document *document = game_scene_document_load_value(value, error, sizeof error);
    if (!document) {
        snprintf(scene->message, sizeof scene->message, "Could not load scene: %s", error);
        return -1;
    }
    for (int i = 0; i < acount(document->entities); i++) {
        if (scene_validate_entity(scene, &document->entities[i]) != 0) {
            game_scene_document_free(document);
            return -1;
        }
    }
    scene->debug_sessions[APP_RUNTIME_SCENE]++;
    editor_set_inspection_blocked(scene->graph_editor, true);
    engine_set_playing(scene->runtime, false);
    scene->live->runtime->deferred = true;
    scene->runtime_playing = false;
    scene->runtime_step = false;
    app_scene_authoring_stop_prototype_preview(scene);
    invalidate_runtime_snapshot(scene);
    game_entities_reset(scene->live->runtime->entities);
    game_physics_reset(scene->live->runtime->physics);
    int configured = game_physics_configure(
        scene->live->runtime->physics,
        document->physics
    );
    assert(configured == 0);
    game_particles_clear(scene->live->runtime->particles);
    app_scene_renderer_reset(scene->renderer);
    while (acount(scene->actors)) {
        scene->selected = acount(scene->actors) - 1;
        remove_selected(scene);
    }
    for (int i = 0; i < acount(document->entities); i++) {
        game_scene_entity *entity = &document->entities[i];
        if (scene_spawn_entity(scene, entity, entity->name) != 0) {
            game_scene_document_free(document);
            return -1;
        }
    }
    if (game_entities_rebuild_global_bindings(scene->live->runtime->entities) != 0) {
        game_scene_document_free(document);
        snprintf(scene->message, sizeof scene->message, "Global binding names must be unique.");
        return -1;
    }
    scene->perspective = document->perspective;
    free(scene->ui);
    scene->ui = document->ui ? xstrdup(document->ui) : NULL;
    game_scene_document_free(document);
    char *saved_path = path ? xstrdup(path) : NULL;
    free(scene->scene_path);
    scene->scene_path = saved_path;
    scene->scene_dirty = false;
    scene_tabs_document_selected(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    snprintf(
        scene->message,
        sizeof scene->message,
        "Loaded scene from %s.",
        scene->scene_path ? scene->scene_path : "checkpoint"
    );
    return 0;
}

int scene_authoring_load_text(app_scene_authoring *scene, const char *text, const char *path)
{
    char error[256];
    av_doc *doc = text ? asset_text_read(text, error, sizeof error) : NULL;
    if (!doc) {
        snprintf(scene->message, sizeof scene->message, "Could not load scene: %s", text ? error : "missing document");
        return -1;
    }
    int result = scene_authoring_load_value(scene, av_doc_root(doc), path);
    av_doc_free(doc);
    return result;
}

int app_scene_authoring_load(app_scene_authoring *scene, const char *path)
{
    if (!scene || !path) {
        return -1;
    }
    char *json = read_scene_file(path);
    if (!json) {
        snprintf(scene->message, sizeof scene->message, "Could not read %s.", path);
        return -1;
    }
    int result = scene_authoring_load_text(scene, json, path);
    free(json);
    if (result == 0) {
        scene->camera.saved = false;
        scene->camera_target = -1;
    }
    return result;
}

const char *app_scene_authoring_message(const app_scene_authoring *scene)
{
    return scene ? scene->message : NULL;
}

app_editor_document_context app_scene_authoring_document(const app_scene_authoring *scene)
{
    return scene ? scene->document
                 : (app_editor_document_context){ APP_EDITOR_DOCUMENT_SCENE, NULL };
}
