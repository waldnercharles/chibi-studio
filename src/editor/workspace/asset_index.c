#include "editor/workspace/asset_index.h"

#include "assets/prototype.h"
#include "assets/reference_adapters.h"
#include "assets/scene_document.h"
#include "assets/ui_document.h"
#include "authoring/preview_source.h"
#include "authoring/source_navigation.h"
#include "authoring/source_references.h"
#include "editor/documents/ui_buffer.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/text_file.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An unsaved scene or system is keyed by its tab. */
static const char *tab_key(const scene_document_tab *tab)
{
    return tab->path ? NULL : tab->key;
}

static const char *tab_name(const scene_document_tab *tab, const char *fallback)
{
    return tab->label && tab->label[0] ? tab->label : fallback;
}

static bool current_scene_tab(const app_scene_authoring *scene, const scene_document_tab *tab)
{
    return scene->current_scene_tab_key && tab->key &&
           !strcmp(scene->current_scene_tab_key, tab->key);
}

static bool has_extension(const char *path, const char *extension)
{
    const char *dot = strrchr(content_basename(path), '.');
    return dot && !strcmp(dot, extension);
}

/* The ui folder lists each ui asset by its .rml, and each stylesheet; an
   asset's .rcss opens with its .rml. */
static bool ui_listed(const char *path)
{
    if (!has_extension(path, UI_STYLE_EXTENSION)) {
        return has_extension(path, UI_MARKUP_EXTENSION);
    }
    char *document = ui_buffer_document(path);
    bool own = !strcmp(document, path);
    free(document);
    return own;
}

static reference_asset_kind content_asset_kind(const app_scene_authoring *scene, const char *path)
{
    static const struct
    {
        const char *folder;
        reference_asset_kind kind;
    } folders[] = {
        { "scenes", REFERENCE_ASSET_SCENE },
        { "prototypes", REFERENCE_ASSET_PROTOTYPE },
        { "relationships", REFERENCE_ASSET_RELATIONSHIP },
        { "behaviors", REFERENCE_ASSET_BEHAVIOR },
        { "events", REFERENCE_ASSET_EVENT },
        { "materials", REFERENCE_ASSET_MATERIAL },
        { "shapes", REFERENCE_ASSET_SHAPE },
        { "models", REFERENCE_ASSET_SHAPE },
        { "fields", REFERENCE_ASSET_FIELD },
        { "libraries", REFERENCE_ASSET_LIBRARY },
        { "shader-libraries", REFERENCE_ASSET_LIBRARY },
        { "systems", REFERENCE_ASSET_SYSTEM },
        { "ui", REFERENCE_ASSET_UI },
    };
    for (int i = 0; i < (int)(sizeof folders / sizeof *folders); i++) {
        if (content_path_in_folder(scene, path, folders[i].folder)) {
            return folders[i].kind != REFERENCE_ASSET_UI || ui_listed(path)
                     ? folders[i].kind
                     : REFERENCE_ASSET_KIND_COUNT;
        }
    }
    return REFERENCE_ASSET_KIND_COUNT;
}

static const char *content_asset_name(const char *path)
{
    const char *base = content_basename(path);
    static char name[256];
    snprintf(name, sizeof name, "%s", base);
    char *extension = strrchr(name, '.');
    if (extension && (!strcmp(extension, DECLARATIVE_ASSET_EXTENSION) ||
                      !strcmp(extension, CODE_ASSET_EXTENSION) ||
                      !strcmp(extension, UI_MARKUP_EXTENSION))) {
        *extension = '\0';
    }
    return name;
}

/* ---------- Assets ---------- */

static void add_content_assets(reference_index *index, app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->content); i++) {
        scene_content_entry *entry = &scene->content[i];
        reference_asset_kind kind = entry->directory || entry->image
                                      ? REFERENCE_ASSET_KIND_COUNT
                                      : content_asset_kind(scene, entry->path);
        if (kind != REFERENCE_ASSET_KIND_COUNT) {
            reference_index_add_asset(
                index,
                kind,
                content_asset_name(entry->path),
                entry->path,
                NULL
            );
        }
    }
}

static void add_tab_assets(reference_index *index, app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_SCENE) {
            reference_index_add_asset(
                index,
                REFERENCE_ASSET_SCENE,
                tab_name(tab, "Scene"),
                tab->path,
                tab_key(tab)
            );
        }
        if (tab->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
            reference_index_add_asset(
                index,
                REFERENCE_ASSET_SYSTEM,
                tab_name(tab, "System"),
                tab->path,
                tab_key(tab)
            );
        }
    }
}

static int definition_asset(reference_index *index, app_scene_authoring *scene, preview_definition *definition)
{
    return reference_index_add_asset(
        index,
        definition->working_presentation && definition->path
            ? content_asset_kind(scene, definition->path)
            : reference_domain_kind(definition->working->domain),
        definition->name,
        definition->path,
        definition->path ? NULL : definition->name
    );
}

static int prototype_asset(reference_index *index, scene_prototype_asset *asset)
{
    return reference_index_add_asset(
        index,
        REFERENCE_ASSET_PROTOTYPE,
        asset->prototype->name,
        asset->path,
        asset->path ? NULL : prototype_identity(asset->prototype)
    );
}

/* ---------- References ---------- */

static void add_ui_references(reference_index *index, app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->content); i++) {
        scene_content_entry *entry = &scene->content[i];
        if (entry->directory || entry->image ||
            content_asset_kind(scene, entry->path) != REFERENCE_ASSET_UI ||
            !has_extension(entry->path, UI_MARKUP_EXTENSION)) {
            continue;
        }
        char *text = text_file_read(entry->path);
        if (text) {
            int source = reference_index_find_asset(
                index,
                REFERENCE_ASSET_UI,
                entry->path,
                NULL
            );
            reference_index_add_ui(index, source, text);
        }
        free(text);
    }
}

/* A saved scene's objects, each also holding what its prototype names. */
static void add_scene_document(app_scene_authoring *scene, reference_index *index, int source, av_value *value)
{
    char error[256];
    scene_document *document = value
                                 ? scene_document_load_value(value, error, sizeof error)
                                 : NULL;
    for (int i = 0; document && i < acount(document->entities); i++) {
        scene_entity *entity = &document->entities[i];
        scene_prototype_asset *base = find_prototype(scene, entity->prototype);
        char *path = scene_document_entity_path(document, i);
        reference_object object = {
            .name = path,
            .prototype = entity->prototype,
            .base = base ? base->prototype : NULL,
            .overrides = entity->components,
            .behaviors = (const char *const *)entity->behaviors,
            .behavior_count = acount(entity->behaviors),
        };
        reference_index_add_object(index, source, &object);
        free(path);
    }
    if (document) {
        reference_index_add_page(index, source, document->ui, scene->default_ui);
    }
    scene_document_free(document);
}

/* Scene tabs other than the current one, whose objects are live actors. */
static void add_scene_tab_references(reference_index *index, app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_SCENE && !current_scene_tab(scene, tab)) {
            int source = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, tab->path, tab_key(tab));
            add_scene_document(scene, index, source, tab->scene_value);
        }
    }
}

/* The open graph's working copy holds edits live apply has not reached. */
static const behavior_asset *definition_working(app_scene_authoring *scene, preview_definition *definition)
{
    const behavior_asset *working = definition->working;
    const behavior_asset *current = definition == scene->definition && scene->graph_editor
                                      ? editor_behavior_asset(scene->graph_editor)
                                      : NULL;
    bool same = current && current->name && working->name &&
                current->domain == working->domain &&
                !strcmp(current->name, working->name);
    return same ? current : working;
}

static void add_definitions(reference_index *index, app_scene_authoring *scene, bool references)
{
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        int source = definition_asset(index, scene, definition);
        if (references && !definition->working_presentation) {
            reference_index_add_graphs(index, source, definition_working(scene, definition));
        }
    }
}

static void add_prototypes(reference_index *index, app_scene_authoring *scene, bool references)
{
    for (int i = 0; i < acount(scene->prototypes); i++) {
        int source = prototype_asset(index, &scene->prototypes[i]);
        if (references) {
            reference_index_add_prototype(index, source, scene->prototypes[i].prototype);
        }
    }
}

/* A live actor holds its behaviors and overrides as they are now: from the
 * runtime snapshot while playing, otherwise from the world. */
static void add_actor_references(
    app_scene_authoring *scene,
    reference_index *index,
    int source,
    scene_actor *actor
)
{
    const prototype *snapshot = scene->runtime_snapshot_valid ? actor->runtime_snapshot
                                                              : NULL;
    assert(!scene->runtime_snapshot_valid || snapshot);
    const char **behaviors = NULL;
    for (int i = 0; snapshot && i < acount(snapshot->graphs); i++) {
        apush(behaviors, snapshot->graphs[i]);
    }
    const source_registry *sources = scene->live->runtime->sources;
    int count = snapshot ? 0 : source_attachment_count(sources);
    for (int i = 0; i < count; i++) {
        source_attachment_info info = source_attachment_at(sources, i);
        if (ecs_entity_to_u64(info.entity) == ecs_entity_to_u64(actor->entity)) {
            apush(behaviors, info.name);
        }
    }
    av_value *overrides = snapshot ? prototype_overrides_value(actor->base, snapshot)
                                   : prototype_capture_overrides_value(
                                         actor->base,
                                         scene->world,
                                         actor->entity
                                     );
    reference_object object = {
        .name = actor->name,
        .prototype = actor->prototype_name,
        .base = actor->base,
        .overrides = overrides,
        .behaviors = behaviors,
        .behavior_count = acount(behaviors),
    };
    reference_index_add_object(index, source, &object);
    av_value_free(overrides);
    afree(behaviors);
}

static void add_active_scene_references(reference_index *index, app_scene_authoring *scene)
{
    int source = -1;
    for (int i = 0; source < 0 && i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_SCENE && current_scene_tab(scene, tab)) {
            source = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, tab->path, tab_key(tab));
        }
    }
    for (int i = 0; source >= 0 && i < acount(scene->actors); i++) {
        add_actor_references(scene, index, source, &scene->actors[i]);
    }
    if (source >= 0) {
        reference_index_add_page(index, source, scene->ui, scene->default_ui);
    }
}

static void add_source_references(reference_index *index, app_scene_authoring *scene)
{
    const authoring_sources *sources = preview_sources_working(scene->live);
    if (!sources) {
        return;
    }
    authoring_document *documents = NULL;
    for (int i = 0; i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        apush(
            documents,
            ((authoring_document){ source->path, source->syntax, source->analysis })
        );
    }
    const script_catalog *catalog = authoring_sources_catalog(sources);
    authoring_navigation *navigation = authoring_navigation_new_project(
        documents,
        acount(documents),
        catalog,
        authoring_sources_definitions(sources),
        authoring_sources_definition_count(sources),
        authoring_sources_references(sources),
        authoring_sources_reference_count(sources)
    );
    authoring_source_references_add(index, navigation);
    authoring_navigation_free(navigation);
    afree(documents);
}

int app_scene_authoring_refresh_asset_index(app_scene_authoring *scene)
{
    if (!scene) {
        return -1;
    }
    if (!scene->asset_index) {
        scene->asset_index = reference_index_new();
    }
    reference_index *index = scene->asset_index;
    reference_index_clear(index);
    /* Every asset first, so references find the event definitions they name. */
    add_content_assets(index, scene);
    add_tab_assets(index, scene);
    add_definitions(index, scene, false);
    add_prototypes(index, scene, false);
    add_ui_references(index, scene);
    add_scene_tab_references(index, scene);
    add_definitions(index, scene, true);
    add_prototypes(index, scene, true);
    add_active_scene_references(index, scene);
    add_source_references(index, scene);
    scene->asset_index_built_generation = scene->asset_index_generation;
    scene->asset_index_revision++;
    return 0;
}

const reference_index *app_scene_authoring_asset_index(const app_scene_authoring *scene)
{
    return scene ? scene->asset_index : NULL;
}

/* A scene or system opens its tab, or its file when no tab shows it. */
static int open_tab_asset(app_scene_authoring *scene, const reference_asset *asset, int position)
{
    app_editor_document_kind kind = asset->kind == REFERENCE_ASSET_SCENE
                                      ? APP_EDITOR_DOCUMENT_SCENE
                                      : APP_EDITOR_DOCUMENT_SYSTEM;
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind == kind &&
            reference_index_find_asset(scene->asset_index, asset->kind, tab->path, tab_key(tab)) ==
                position) {
            return scene_tabs_activate(scene, i);
        }
    }
    if (!asset->path) {
        return -1;
    }
    return asset->kind == REFERENCE_ASSET_SCENE
             ? scene_tabs_open_scene(scene, asset->path)
             : open_system_asset(scene, asset->path);
}

int app_scene_authoring_open_asset(app_scene_authoring *scene, int position)
{
    const reference_asset *asset = scene && scene->asset_index
                                     ? reference_index_asset_at(scene->asset_index, position)
                                     : NULL;
    if (!asset) {
        return -1;
    }
    bool executable = asset->kind == REFERENCE_ASSET_BEHAVIOR ||
                      asset->kind == REFERENCE_ASSET_LIBRARY ||
                      asset->kind == REFERENCE_ASSET_SYSTEM;
    if (asset->unresolved && executable && asset->path) {
        return scene_tabs_open_source(
            scene,
            asset->kind == REFERENCE_ASSET_SYSTEM ? APP_EDITOR_DOCUMENT_SYSTEM
                                                  : APP_EDITOR_DOCUMENT_BEHAVIOR,
            asset->name,
            asset->path
        );
    }
    if (asset->unresolved) {
        return -1;
    }
    switch (asset->kind) {
        case REFERENCE_ASSET_SCENE:
            return open_tab_asset(scene, asset, position);
        case REFERENCE_ASSET_SYSTEM: {
            int result = open_tab_asset(scene, asset, position);
            return result == 0 || !asset->path ? result
                                               : scene_tabs_open_source(
                                                     scene,
                                                     APP_EDITOR_DOCUMENT_SYSTEM,
                                                     asset->name,
                                                     asset->path
                                                 );
        }
        case REFERENCE_ASSET_PROTOTYPE:
            if (asset->path && strrchr(asset->path, '.') &&
                !strcmp(strrchr(asset->path, '.'), CODE_ASSET_EXTENSION)) {
                return scene_tabs_open_source(
                    scene,
                    APP_EDITOR_DOCUMENT_PROTOTYPE,
                    asset->name,
                    asset->path
                );
            }
            return app_scene_authoring_open_prototype(scene, asset->name);
        case REFERENCE_ASSET_RELATIONSHIP:
            return asset->path ? scene_tabs_open_source(
                                     scene,
                                     APP_EDITOR_DOCUMENT_RELATIONSHIP,
                                     asset->name,
                                     asset->path
                                 )
                               : -1;
        case REFERENCE_ASSET_BEHAVIOR:
        case REFERENCE_ASSET_EVENT:
        case REFERENCE_ASSET_MATERIAL:
        case REFERENCE_ASSET_SHAPE:
        case REFERENCE_ASSET_FIELD:
        case REFERENCE_ASSET_LIBRARY: {
            int result = app_scene_authoring_open_behavior(scene, asset->name);
            return result == 0 || !executable || !asset->path
                     ? result
                     : scene_tabs_open_source(
                           scene,
                           APP_EDITOR_DOCUMENT_BEHAVIOR,
                           asset->name,
                           asset->path
                       );
        }
        case REFERENCE_ASSET_UI:
            return asset->path ? scene_tabs_open_ui(scene, asset->path) : -1;
        case REFERENCE_ASSET_KIND_COUNT: break;
    }
    return -1;
}
