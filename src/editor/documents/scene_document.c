#include "assets/scene_document.h"

#include "authoring/application_catalog.h"
#include "authoring/behavior_loader.h"
#include "authoring/preview_source.h"
#include "authoring/source_asset.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "runtime/declared_components.h"
#include "support/asset_text.h"
#include "support/xalloc.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
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

static bool same_names(const char **names, char **graphs)
{
    if (acount(names) != acount(graphs)) {
        return false;
    }
    for (int i = 0; i < acount(names); i++) {
        if (strcmp(names[i], graphs[i])) {
            return false;
        }
    }
    return true;
}

void scene_override_path(av_doc *doc, av_value *overrides, const prototype_link *link)
{
    const char *key = link->component->key ? link->component->key
                                           : link->component->name;
    av_value *fields = av_obj_get(overrides, key);
    if (!fields) {
        fields = av_obj_add_obj(doc, overrides, key);
    }
    av_obj_set(doc, fields, link->component->type->fields[link->field].name, av_str(doc, link->path));
}

const char *scene_link_path(const prototype_link *links, ecs_component_id component, int field)
{
    for (int i = 0; i < acount(links); i++) {
        if (links[i].component->id == component && links[i].field == field) {
            return links[i].path;
        }
    }
    return NULL;
}

/* Whether the actor's authored state has the component: while the scene
   runs, its snapshot's. */
static bool authored_component(
    app_scene_authoring *scene,
    const scene_actor *actor,
    const ecs_component_desc *desc
)
{
    if (!scene->runtime_snapshot_valid) {
        return ecs_component_has(scene->world, actor->entity, desc->id);
    }
    for (int i = 0; i < acount(actor->runtime_snapshot->components); i++) {
        if (actor->runtime_snapshot->components[i].desc == desc) {
            return true;
        }
    }
    return false;
}

/* Adds the entity paths the actor's fields name, for the components it
   still has: its live handles never save. */
static void add_paths(av_doc *doc, av_value *overrides, app_scene_authoring *scene, const scene_actor *actor)
{
    for (int i = 0; i < acount(actor->paths); i++) {
        if (authored_component(scene, actor, actor->paths[i].component)) {
            scene_override_path(doc, overrides, &actor->paths[i]);
        }
    }
}

static av_value *with_paths(app_scene_authoring *scene, const scene_actor *actor, av_value *overrides)
{
    av_doc *doc = av_doc_new();
    av_value *root = overrides ? av_clone(doc, overrides) : av_obj(doc);
    av_doc_set_root(doc, root);
    add_paths(doc, root, scene, actor);
    av_value *result = av_value_copy(root);
    av_doc_free(doc);
    return result;
}

av_value *scene_actor_paths(app_scene_authoring *scene, const scene_actor *actor)
{
    return acount(actor->paths) ? with_paths(scene, actor, NULL) : NULL;
}

/* An actor keeps its prototype's behaviors until the scene lists its own or
   its behaviors differ from the prototype's. */
static int add_actor_entity(
    app_scene_authoring *scene,
    scene_document *document,
    const scene_actor *actor,
    int parent
)
{
    preview_runtime *runtime = scene->live->runtime;
    source_registry *behaviors = runtime->sources;
    const prototype *snapshot = scene->runtime_snapshot_valid ? actor->runtime_snapshot
                                                              : NULL;
    const char **names = NULL;
    if (snapshot) {
        for (int j = 0; j < acount(snapshot->graphs); j++) {
            apush(names, snapshot->graphs[j]);
        }
    }
    if (!snapshot) {
        for (int j = 0; j < source_attachment_count(behaviors); j++) {
            source_attachment_info info = source_attachment_at(behaviors, j);
            if (ecs_entity_to_u64(actor->entity) == ecs_entity_to_u64(info.entity)) {
                apush(names, info.name);
            }
        }
    }
    int index = acount(document->entities);
    int result = scene_document_add_entity(document, parent, actor->name, actor->prototype_name);
    if (result == 0 && (actor->own_behaviors || !same_names(names, actor->base->graphs))) {
        result = scene_document_set_entity_behaviors(document, index, names, acount(names));
    }
    afree(names);
    if (result == 0) {
        av_value *components = snapshot
                                 ? prototype_overrides_value(actor->base, snapshot)
                                 : prototype_capture_overrides_value(
                                       actor->base,
                                       scene->world,
                                       actor->entity
                                   );
        av_value *named = with_paths(scene, actor, components);
        result = scene_document_set_entity_components(document, index, named);
        av_value_free(named);
        av_value_free(components);
    }
    if (result == 0) {
        av_value *retained = scene_retention_save(actor->retained);
        result = scene_document_set_entity_retained(document, index, retained);
        av_value_free(retained);
    }
    return result;
}

/* The position in `actors` of the actor's parent, or -1. */
static int listed_parent(app_scene_authoring *scene, const int *actors, const scene_actor *actor)
{
    uint64_t parent = scene_actor_parent(scene, actor);
    for (int i = 0; parent && i < acount(actors); i++) {
        if (scene->actors[actors[i]].identity == parent) {
            return i;
        }
    }
    return -1;
}

int scene_document_add_actors(app_scene_authoring *scene, scene_document *document, const int *actors)
{
    for (int i = 0; i < acount(actors); i++) {
        const scene_actor *actor = &scene->actors[actors[i]];
        if (add_actor_entity(scene, document, actor, listed_parent(scene, actors, actor)) !=
            0) {
            return -1;
        }
    }
    return 0;
}

scene_document *scene_authoring_document(app_scene_authoring *scene)
{
    char name[256];
    scene_document_name(scene, name, sizeof name);
    scene_document *document = scene_document_new(name);
    if (!document) {
        return NULL;
    }
    document->perspective = scene->perspective;
    scene_document_set_ui(document, scene->ui);
    document->physics = physics_configuration(scene->live->runtime->physics);
    int *actors = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        apush(actors, i);
    }
    int result = scene_document_add_actors(scene, document, actors);
    afree(actors);
    if (result != 0) {
        scene_document_free(document);
        return NULL;
    }
    return document;
}

av_value *scene_authoring_scene_value(app_scene_authoring *scene)
{
    finish_transform_edit(scene, false);
    scene_document *document = scene_authoring_document(scene);
    av_value *value = document
                        ? scene_document_to_value(document, project_components())
                        : NULL;
    scene_document_free(document);
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

static char *read_scene_file_limit(const char *path, size_t limit)
{
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) {
            fclose(file);
        }
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || (uintmax_t)size > limit || fseek(file, 0, SEEK_SET)) {
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
    if (read != (size_t)size || memchr(json, 0, read)) {
        free(json);
        return NULL;
    }
    json[read] = '\0';
    return json;
}

char *read_scene_file(const char *path)
{
    return read_scene_file_limit(path, SIZE_MAX - 1);
}

static const char *prototype_source_include(const char *name, void *user)
{
    const app_scene_authoring *scene = user;
    for (int i = 0; i < acount(scene->prototype_sources); i++) {
        if (!strcmp(scene->prototype_sources[i].name, name)) {
            return scene->prototype_sources[i].text;
        }
    }
    return NULL;
}
bool scene_prototype_sources_refresh(app_scene_authoring *scene)
{
    scene_prototype_source *sources = NULL;
    size_t bytes = 0;
    bool valid = true;
    for (int i = 0; valid && i < acount(scene->content); i++) {
        const scene_content_entry *entry = &scene->content[i];
        if (entry->directory ||
            !content_path_in_folder(scene, entry->path, "prototypes")) {
            continue;
        }
        const char *extension = strrchr(entry->path, '.');
        if (!extension || (strcmp(extension, ".chibi") && strcmp(extension, ".kdl"))) {
            continue;
        }
        char *text = read_scene_file_limit(entry->path, PROTOTYPE_READ_MAX - bytes);
        if (!text || acount(sources) >= PROTOTYPE_EXPANSION_MAX) {
            free(text);
            snprintf(
                scene->message,
                sizeof scene->message,
                "Prototype sources exceed their read limits: %s",
                entry->path
            );
            valid = false;
            break;
        }
        bytes += strlen(text);
        av_doc *doc = authoring_prototype_document_read(
            text,
            &scene->registry,
            scene->message,
            sizeof scene->message
        );
        const char *name = doc ? av_get_str(av_obj_get(av_doc_root(doc), "name"))
                               : NULL;
        if (!name || !*name) {
            free(text);
            av_doc_free(doc);
            valid = false;
            break;
        }
        for (int j = 0; j < acount(sources); j++) {
            if (!strcmp(name, sources[j].name)) {
                snprintf(scene->message, sizeof scene->message, "Duplicate prototype name: %s", name);
                valid = false;
                break;
            }
        }
        if (valid) {
            apush(
                sources,
                ((scene_prototype_source){ xstrdup(name), text, xstrdup(entry->path) })
            );
        } else {
            free(text);
        }
        av_doc_free(doc);
    }
    if (!valid) {
        for (int i = 0; i < acount(sources); i++) {
            free(sources[i].name);
            free(sources[i].text);
            free(sources[i].path);
        }
        afree(sources);
        return false;
    }
    for (int i = 0; i < acount(scene->prototype_sources); i++) {
        free(scene->prototype_sources[i].name);
        free(scene->prototype_sources[i].text);
        free(scene->prototype_sources[i].path);
    }
    afree(scene->prototype_sources);
    scene->prototype_sources = sources;
    scene->registry.include = prototype_source_include;
    scene->registry.include_user = scene;
    return true;
}
static int load_content_prototype(app_scene_authoring *scene, const char *path)
{
    for (int i = 0; i < acount(scene->prototype_sources); i++) {
        if (!strcmp(scene->prototype_sources[i].path, path)) {
            if (app_scene_authoring_register_prototype(
                    scene,
                    scene->prototype_sources[i].text,
                    path
                )) {
                snprintf(scene->message, sizeof scene->message, "Could not load prototype: %s", path);
                return -1;
            }
            return 1;
        }
    }
    return 0;
}

int scene_attach_studio_nodes(app_scene_authoring *scene, preview_world *world, char *error, size_t size)
{
    if (!scene->studio_project || preview_sources_working(world)) {
        return 0;
    }
    int bound = source_bind_service(world->runtime->sources, "studio", &scene->studio_nodes);
    assert(!bound);
    (void)bound;
    native_capabilities *catalog = authoring_application_catalog_new(true, error, size);
    if (!catalog) {
        return -1;
    }
    int status = preview_sources_prepare_with_catalog(
        world,
        scene->content_root,
        behavior_default_backend(),
        catalog,
        error,
        size
    );
    native_capabilities_free(catalog);
    return status;
}

int app_scene_authoring_load_content(app_scene_authoring *scene)
{
    if (!scene) {
        return -1;
    }
    content_refresh(scene);
    if (!scene_prototype_sources_refresh(scene)) {
        return -1;
    }
    preview_world *worlds[] = {
        scene->live,
        prototype_preview_live(scene->prototype_preview),
    };
    for (int i = 0; i < 2; i++) {
        if (scene_attach_studio_nodes(
                scene,
                worlds[i],
                scene->message,
                sizeof scene->message
            )) {
            return -1;
        }
    }
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
            int count = load_content_prototype(scene, entry->path);
            if (count < 0) {
                return -1;
            }
            loaded += count;
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
    prototype_registry registry = app_component_registry();
    if (prototype_apply_components_value(
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
    source_registry *world = scene->live->runtime->sources;
    const authoring_sources *applied = preview_sources_applied(scene->live);
    for (int i = 0; i < acount(names); i++) {
        if (!source_registered(world, names[i])) {
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

int scene_validate_entity(app_scene_authoring *scene, const scene_entity *entity)
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
        result = prototype_spawn_components(asset->prototype, world, &actor, error, sizeof error);
    }
    if (result == 0 && entity->components) {
        result = prototype_apply_components_value(
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
    prototype_owned_strings_free(component_strings);
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

int scene_spawn_entity(app_scene_authoring *scene, const scene_entity *entity, const char *name, ecs_entity parent)
{
    if (add_actor(scene, name, entity->prototype) != 0) {
        return -1;
    }
    scene_actor *actor = &alast(scene->actors);
    if (parent.slot &&
        (ecs_world_record_add_pair(scene->world, actor->entity, ECS_CHILD_OF, parent, 0) ||
         ecs_world_apply_commands(scene->world))) {
        snprintf(scene->message, sizeof scene->message, "Could not place %s under its parent.", name);
        return -1;
    }
    if (scene_retention_load(entity->retained, &actor->retained) != 0 ||
        apply_components_value(scene, actor->entity, entity->components) != 0 ||
        restore_retained_components(scene, actor, actor->base) != 0) {
        return -1;
    }
    actor->paths = entity->components
                     ? prototype_components_links(&scene->registry, entity->components)
                     : NULL;
    actor->own_behaviors = entity->own_behaviors;
    if (!entity->own_behaviors) {
        return 0;
    }
    preview_runtime *runtime = scene->live->runtime;
    source_registry *behaviors = runtime->sources;
    source_detach_entity(behaviors, actor->entity);
    for (int j = 0; j < acount(entity->behaviors); j++) {
        if (source_attach(
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

/* The actor at the '.'-separated path of names from the scene's roots. */
static const scene_actor *actor_at_path(app_scene_authoring *scene, const char *path)
{
    ecs_entity parent = { 0 };
    for (;;) {
        size_t length = strcspn(path, ".");
        const scene_actor *found = NULL;
        for (int i = 0; i < acount(scene->actors) && !found; i++) {
            const scene_actor *actor = &scene->actors[i];
            if (ecs_entity_to_u64(ecs_parent(scene->world, actor->entity)) ==
                    ecs_entity_to_u64(parent) &&
                strlen(actor->name) == length && !strncmp(actor->name, path, length)) {
                found = actor;
            }
        }
        if (!found || !path[length]) {
            return found;
        }
        parent = found->entity;
        path += length + 1;
    }
}

static void link_path(app_scene_authoring *scene, const scene_actor *actor, const prototype_link *link)
{
    const ecs_component_desc *desc = link->component;
    pin_value held = { 0 };
    if (ecs_component_read_field(NULL, scene->world, actor->entity, desc->id, link->field, &held) !=
        ECS_OK) {
        return;
    }
    const scene_actor *target = actor_at_path(scene, link->path);
    uint64_t handle = target ? ecs_entity_to_u64(target->entity) : 0;
    if (held.u64 == handle) {
        return;
    }
    pin_value value = { .type = PIN_HANDLE,
                        .type_name = desc->type->fields[link->field].type_name,
                        .u64 = handle };
    int written = ecs_component_write_field(scene->world, actor->entity, desc->id, link->field, value);
    assert(written == ECS_OK);
}

void scene_link_paths(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        for (int j = 0; j < acount(scene->actors[i].paths); j++) {
            link_path(scene, &scene->actors[i], &scene->actors[i].paths[j]);
        }
    }
}

/* The path that names the actor from the scene's roots, were it named
   `name`; the caller frees it. */
static char *actor_path(app_scene_authoring *scene, const scene_actor *actor, const char *name)
{
    uint64_t parent = scene_actor_parent(scene, actor);
    if (!parent) {
        return xstrdup(name);
    }
    const scene_actor *holder = scene_actor_by_identity(scene, parent);
    assert(holder);
    char *above = actor_path(scene, holder, holder->name);
    size_t size = strlen(above) + strlen(name) + 2;
    char *path = xmalloc(size);
    snprintf(path, size, "%s.%s", above, name);
    free(above);
    return path;
}

/* Whether the path names the object at `from` or one below it. */
static bool names_below(const char *path, const char *from)
{
    size_t length = strlen(from);
    return !strncmp(path, from, length) && (!path[length] || path[length] == '.');
}

/* Moves the link's path, which names the object at `from` or one below it,
   to name that object at `to`. */
static void move_link(prototype_link *link, const char *from, const char *to)
{
    const char *rest = link->path + strlen(from);
    size_t size = strlen(to) + strlen(rest) + 1;
    char *path = xmalloc(size);
    snprintf(path, size, "%s%s", to, rest);
    free(link->path);
    link->path = path;
}

static prototype_link *ref_link(app_scene_authoring *scene, scene_path_ref ref)
{
    scene_actor *actor = scene_actor_by_identity(scene, ref.actor);
    for (int i = 0; actor && i < acount(actor->paths); i++) {
        prototype_link *link = &actor->paths[i];
        if (link->component->id == ref.component && link->field == ref.field) {
            return link;
        }
    }
    return NULL;
}

scene_path_ref *scene_rename_paths(app_scene_authoring *scene, const scene_actor *actor, const char *name)
{
    char *from = actor_path(scene, actor, actor->name);
    char *to = actor_path(scene, actor, name);
    scene_path_ref *renamed = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *holder = &scene->actors[i];
        for (int j = 0; j < acount(holder->paths); j++) {
            prototype_link *link = &holder->paths[j];
            if (names_below(link->path, from)) {
                move_link(link, from, to);
                apush(
                    renamed,
                    ((scene_path_ref){ holder->identity, link->component->id, link->field })
                );
            }
        }
    }
    free(from);
    free(to);
    return renamed;
}

bool scene_rename_recorded_paths(
    app_scene_authoring *scene,
    const scene_actor *actor,
    const char *name,
    const scene_path_ref *paths
)
{
    char *from = actor_path(scene, actor, actor->name);
    char *to = actor_path(scene, actor, name);
    bool found = true;
    for (int i = 0; found && i < acount(paths); i++) {
        prototype_link *link = ref_link(scene, paths[i]);
        found = link && names_below(link->path, from);
    }
    for (int i = 0; found && i < acount(paths); i++) {
        move_link(ref_link(scene, paths[i]), from, to);
    }
    free(from);
    free(to);
    return found;
}

int scene_authoring_load_value(app_scene_authoring *scene, av_value *value, const char *path)
{
    char error[256];
    scene_document *document = scene_document_load_value(value, error, sizeof error);
    if (!document) {
        snprintf(scene->message, sizeof scene->message, "Could not load scene: %s", error);
        return -1;
    }
    for (int i = 0; i < acount(document->entities); i++) {
        if (scene_validate_entity(scene, &document->entities[i]) != 0) {
            scene_document_free(document);
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
    entities_reset(scene->live->runtime->entities);
    physics_reset(scene->live->runtime->physics);
    physics3d_reset(scene->live->runtime->physics3d);
    int configured = physics_configure(scene->live->runtime->physics, document->physics);
    assert(configured == 0);
    configured = physics3d_configure(scene->live->runtime->physics3d, document->physics);
    assert(configured == 0);
    particles_clear(scene->live->runtime->particles);
    app_scene_renderer_reset(scene->renderer);
    while (acount(scene->actors)) {
        scene->selected = acount(scene->actors) - 1;
        remove_selected(scene);
    }
    for (int i = 0; i < acount(document->entities); i++) {
        scene_entity *entity = &document->entities[i];
        ecs_entity parent = entity->parent >= 0 ? scene->actors[entity->parent].entity
                                                : (ecs_entity){ 0 };
        if (scene_spawn_entity(scene, entity, entity->name, parent) != 0) {
            scene_document_free(document);
            return -1;
        }
    }
    scene_link_paths(scene);
    if (entities_rebuild_global_bindings(scene->live->runtime->entities) != 0) {
        scene_document_free(document);
        snprintf(scene->message, sizeof scene->message, "Global binding names must be unique.");
        return -1;
    }
    scene->perspective = document->perspective;
    free(scene->ui);
    scene->ui = document->ui ? xstrdup(document->ui) : NULL;
    scene_document_free(document);
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
    scene_notice(scene);
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

void scene_notice(app_scene_authoring *scene)
{
    snprintf(scene->notice, sizeof scene->notice, "%s", scene->message);
}

app_editor_document_context app_scene_authoring_document(const app_scene_authoring *scene)
{
    return scene ? scene->document
                 : (app_editor_document_context){ APP_EDITOR_DOCUMENT_SCENE, NULL };
}
