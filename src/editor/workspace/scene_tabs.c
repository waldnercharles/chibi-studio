#include "assets/prototype.h"
#include "assets/ui_document.h"
#include "authoring/behavior_loader.h"
#include "authoring/preview_source.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static scene_document_tab *active_tab(app_scene_authoring *scene)
{
    return scene->active_tab >= 0 && scene->active_tab < acount(scene->tabs)
             ? &scene->tabs[scene->active_tab]
             : NULL;
}

static void free_tab(scene_document_tab *tab)
{
    free(tab->key);
    free(tab->label);
    free(tab->path);
    av_value_free(tab->scene_value);
    av_value_free(tab->scene_history);
    afree(tab->scene_entities);
    afree(tab->scene_actor_ids);
    free(tab->selected_prototype_name);
    av_value_free(tab->asset_saved);
    ui_buffer_free(tab->ui);
    av_value_free(tab->source_state);
    *tab = (scene_document_tab){ 0 };
}

static const char *tab_kind_name(app_editor_document_kind kind)
{
    switch (kind) {
        case APP_EDITOR_DOCUMENT_SCENE: return "scene";
        case APP_EDITOR_DOCUMENT_PROTOTYPE: return "prototype";
        case APP_EDITOR_DOCUMENT_BEHAVIOR: return "behavior";
        case APP_EDITOR_DOCUMENT_SYSTEM: return "system";
        case APP_EDITOR_DOCUMENT_UI: return "ui";
        case APP_EDITOR_DOCUMENT_RELATIONSHIP: return "relationship";
    }
    return "document";
}

static char *tab_key(app_editor_document_kind kind, const char *id, const char *path)
{
    const char *value = path && path[0] ? path : id;
    if (!value || !value[0]) {
        return NULL;
    }
    size_t size = strlen(value) + strlen(tab_kind_name(kind)) + 2;
    char *key = xmalloc(size);
    snprintf(key, size, "%s:%s", tab_kind_name(kind), value);
    return key;
}

static char *tab_label(app_editor_document_kind kind, const char *id, const char *path)
{
    const char *value = path && path[0] ? content_basename(path) : id;
    if (!value || !value[0]) {
        value = kind == APP_EDITOR_DOCUMENT_SCENE ? "Scene" : "Document";
    }
    char *label = xstrdup(value);
    size_t length = strlen(label);
    const char *extension = strrchr(label, '.');
    if (!extension || (strcmp(extension, UI_MARKUP_EXTENSION) &&
                       strcmp(extension, CODE_ASSET_EXTENSION) &&
                       strcmp(extension, DECLARATIVE_ASSET_EXTENSION))) {
        return label;
    }
    size_t extension_length = strlen(extension);
    if (length > extension_length &&
        !strcmp(label + length - extension_length, extension)) {
        label[length - extension_length] = '\0';
    }
    return label;
}

static int tab_find(const app_scene_authoring *scene, const char *key)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].key && strcmp(scene->tabs[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int scene_tab_for_path(const app_scene_authoring *scene, const char *path)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE &&
            scene->tabs[i].path && strcmp(scene->tabs[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int document_tab_for_path(
    const app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *path
)
{
    if (!path) {
        return -1;
    }
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].kind == kind && scene->tabs[i].path &&
            strcmp(scene->tabs[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int scene_tab_for_current_document(const app_scene_authoring *scene)
{
    if (scene->current_scene_tab_key) {
        int index = tab_find(scene, scene->current_scene_tab_key);
        if (index >= 0 && scene->tabs[index].kind == APP_EDITOR_DOCUMENT_SCENE) {
            return index;
        }
    }
    if (scene->scene_path) {
        return scene_tab_for_path(scene, scene->scene_path);
    }
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE && !scene->tabs[i].path) {
            return i;
        }
    }
    return -1;
}

const char *scene_tabs_current_scene_key(const app_scene_authoring *scene)
{
    if (scene->current_scene_tab_key) {
        int index = tab_find(scene, scene->current_scene_tab_key);
        if (index >= 0 && scene->tabs[index].kind == APP_EDITOR_DOCUMENT_SCENE) {
            return scene->tabs[index].key;
        }
    }
    int index = scene_tab_for_current_document(scene);
    return index >= 0 ? scene->tabs[index].key : NULL;
}

static void set_current_scene_tab(app_scene_authoring *scene, const scene_document_tab *tab)
{
    assert(tab->kind == APP_EDITOR_DOCUMENT_SCENE && tab->key);
    free(scene->current_scene_tab_key);
    scene->current_scene_tab_key = xstrdup(tab->key);
}

static void tab_set_path(scene_document_tab *tab, const char *path)
{
    free(tab->path);
    tab->path = path ? xstrdup(path) : NULL;
}

static void capture_scene_tab(app_scene_authoring *scene, scene_document_tab *tab)
{
    assert(tab->kind == APP_EDITOR_DOCUMENT_SCENE);
    if (scene_authoring_sync_document(scene) != 0) {
        return;
    }
    av_value *json = scene_authoring_scene_value(scene);
    av_value *history = scene_history_capture(scene->history);
    if (!json || !history) {
        av_value_free(json);
        av_value_free(history);
        return;
    }
    av_value_free(tab->scene_value);
    tab->scene_value = json;
    av_value_free(tab->scene_history);
    tab->scene_history = history;
    afree(tab->scene_entities);
    afree(tab->scene_actor_ids);
    for (int i = 0; i < acount(scene->actors); i++) {
        apush(tab->scene_entities, ecs_entity_to_u64(scene->actors[i].entity));
        apush(tab->scene_actor_ids, scene->actors[i].identity);
    }
    tab->scene_entity_count = acount(tab->scene_entities);
    tab->selected_entity = scene->selected >= 0 &&
                                   scene->selected < acount(scene->actors)
                             ? ecs_entity_to_u64(scene->actors[scene->selected].entity)
                             : 0;
    tab->camera = scene->camera.view;
    tab->camera_saved = scene->camera.saved;
    tab->scene_dirty = scene->scene_dirty;
    tab->selected = scene->selected;
    tab->selected_prototype = scene->selected_prototype;
    free(tab->selected_prototype_name);
    tab->selected_prototype_name = selected_prototype(scene) &&
                                           selected_prototype(scene)->prototype
                                     ? xstrdup(
                                           selected_prototype(scene)->prototype->name
                                       )
                                     : NULL;
    /* A scene loaded into an untitled tab names it: "scene-1" becomes Sandbox.
       A titled tab keeps its identity through Save As. */
    const char *path = scene->scene_path;
    if (path && path[0] && !tab->path) {
        /* A tab being created has no key yet. */
        bool current = scene->current_scene_tab_key && tab->key &&
                       !strcmp(scene->current_scene_tab_key, tab->key);
        free(tab->key);
        tab->key = tab_key(tab->kind, NULL, path);
        free(tab->label);
        tab->label = tab_label(tab->kind, NULL, path);
        if (current) {
            set_current_scene_tab(scene, tab);
        }
    }
    tab_set_path(tab, path);
}

static void capture_system_tab(app_scene_authoring *scene, scene_document_tab *tab)
{
    assert(tab->kind == APP_EDITOR_DOCUMENT_SYSTEM);
    if (scene_authoring_sync_document(scene) != 0) {
        return;
    }
    tab->system_editor_revision = editor_edit_revision(scene->graph_editor);
    tab_set_path(tab, scene->system_path);
}

static void restore_scene_tab_view(app_scene_authoring *scene, const scene_document_tab *tab)
{
    scene->scene_dirty = tab->scene_dirty;
    scene->camera.view = tab->camera;
    scene->camera.saved = tab->camera_saved;
    scene->camera.framed = 0;
    scene->selected = -1;
    for (int i = 0; i < acount(scene->actors); i++) {
        uint64_t entity = i < tab->scene_entity_count ? tab->scene_entities[i] : 0;
        scene->actors[i].marked = tab->selected_entity && entity == tab->selected_entity;
        if (scene->actors[i].marked) {
            scene->selected = i;
        }
    }
    if (scene->selected < 0 && tab->selected >= 0 &&
        tab->selected < acount(scene->actors)) {
        scene->selected = tab->selected;
    }
    scene->selected_prototype = tab->selected_prototype_name
                                  ? prototype_index(scene, tab->selected_prototype_name)
                                  : tab->selected_prototype;
    if (scene->selected_prototype < 0 ||
        scene->selected_prototype >= acount(scene->prototypes)) {
        scene->selected_prototype = -1;
    }
}

static void capture_asset_baseline(app_scene_authoring *scene, scene_document_tab *tab)
{
    if (tab->source_only) {
        return;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_PROTOTYPE && !tab->asset_saved) {
        scene_prototype_asset *asset = find_prototype(scene, tab->key + strlen("prototype:"));
        if (asset && asset->prototype) {
            av_value *json = prototype_to_value(asset->prototype);
            if (json) {
                av_value_free(tab->asset_saved);
                tab->asset_saved = json;
            }
        }
        return;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_BEHAVIOR && !tab->asset_saved) {
        preview_definition *definition = preview_find_definition(
            scene->live,
            tab->key + strlen("behavior:")
        );
        if (definition && definition->working && !definition->working_presentation) {
            av_doc *native = av_doc_new();
            av_value *json = av_value_copy(
                behavior_asset_to_value(native, definition->working)
            );
            av_doc_free(native);
            if (json) {
                av_value_free(tab->asset_saved);
                tab->asset_saved = json;
            }
        }
    }
}

void scene_tabs_sync_current(app_scene_authoring *scene)
{
    scene_document_tab *tab = active_tab(scene);
    if (!tab || tab->source_only) {
        return;
    }
    int current_scene = scene_tab_for_current_document(scene);
    if (current_scene >= 0) {
        capture_scene_tab(scene, &scene->tabs[current_scene]);
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        capture_system_tab(scene, tab);
    }
}

void scene_tabs_mark_document_saved(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *id
)
{
    const char *path = NULL;
    if (kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        path = scene->system_path;
    } else if (kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        scene_prototype_asset *asset = find_prototype(scene, id);
        path = asset ? asset->path : NULL;
    } else if (kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        preview_definition *definition = preview_find_definition(scene->live, id);
        path = definition ? preview_definition_path(definition) : NULL;
    }
    char *key = tab_key(kind, id, kind == APP_EDITOR_DOCUMENT_SYSTEM ? scene->system_path : NULL);
    int index = document_tab_for_path(scene, kind, path);
    if (index < 0) {
        index = key ? tab_find(scene, key) : -1;
    }
    free(key);
    if (index < 0) {
        return;
    }
    scene_document_tab *tab = &scene->tabs[index];
    tab_set_path(tab, path);
    free(tab->label);
    tab->label = tab_label(kind, id, path);
    av_value_free(tab->asset_saved);
    tab->asset_saved = NULL;
    if (kind != APP_EDITOR_DOCUMENT_SYSTEM) {
        capture_asset_baseline(scene, tab);
    }
}

static scene_document_tab *add_tab(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *id,
    const char *path
)
{
    char *key = tab_key(
        kind,
        id,
        kind == APP_EDITOR_DOCUMENT_SCENE || kind == APP_EDITOR_DOCUMENT_SYSTEM ||
                kind == APP_EDITOR_DOCUMENT_RELATIONSHIP
            ? path
            : NULL
    );
    if (!key) {
        return NULL;
    }
    int index = document_tab_for_path(scene, kind, path);
    if (index < 0) {
        index = tab_find(scene, key);
    }
    if (index >= 0) {
        free(key);
        return &scene->tabs[index];
    }
    scene_document_tab tab = {
        .kind = kind,
        .key = key,
        .label = tab_label(kind, id, path),
        .path = path ? xstrdup(path) : NULL,
    };
    apush(scene->tabs, tab);
    return &alast(scene->tabs);
}

void scene_tabs_initialize(app_scene_authoring *scene)
{
    scene->active_tab = -1;
    scene->next_scene_key = 1;
    scene_document_tab *tab = add_tab(scene, APP_EDITOR_DOCUMENT_SCENE, "scene-1", NULL);
    assert(tab);
    scene->active_tab = 0;
    set_current_scene_tab(scene, tab);
}

void scene_tabs_free(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        free_tab(&scene->tabs[i]);
    }
    afree(scene->tabs);
    scene->active_tab = -1;
    free(scene->current_scene_tab_key);
    scene->current_scene_tab_key = NULL;
}

/* The file behind a document, or NULL for one never saved. */
static const char *document_path(app_scene_authoring *scene, app_editor_document_kind kind, const char *id)
{
    switch (kind) {
        case APP_EDITOR_DOCUMENT_SCENE: return scene->scene_path;
        case APP_EDITOR_DOCUMENT_SYSTEM: return scene->system_path;
        case APP_EDITOR_DOCUMENT_PROTOTYPE: {
            scene_prototype_asset *asset = find_prototype(scene, id);
            return asset ? asset->path : NULL;
        }
        case APP_EDITOR_DOCUMENT_BEHAVIOR: {
            preview_definition *definition = preview_find_definition(scene->live, id);
            return definition ? preview_definition_path(definition) : NULL;
        }
        case APP_EDITOR_DOCUMENT_UI: return id;
        case APP_EDITOR_DOCUMENT_RELATIONSHIP: return id;
    }
    return NULL;
}

/* Documents named by their file rather than an asset name. */
static bool keyed_by_path(app_editor_document_kind kind)
{
    return kind == APP_EDITOR_DOCUMENT_SCENE ||
           kind == APP_EDITOR_DOCUMENT_SYSTEM || kind == APP_EDITOR_DOCUMENT_UI ||
           kind == APP_EDITOR_DOCUMENT_RELATIONSHIP;
}

void scene_tabs_document_selected(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *id
)
{
    /* Loading actors must not navigate back to the previous scene tab. */
    if (scene->scene_tab_restoring) {
        return;
    }
    int previous = scene->active_tab;
    const char *path = document_path(scene, kind, id);
    scene_document_tab *current = active_tab(scene);
    if (current && current->kind == kind && !scene->tabs_transitioning) {
        bool same = keyed_by_path(kind)
                      ? ((!path && !current->path) ||
                         (path && current->path && !strcmp(path, current->path)))
                      : !strcmp(current->key + strlen(tab_kind_name(kind)) + 1, id);
        if (same) {
            return;
        }
    }
    if (previous >= 0 && !scene->tabs_transitioning) {
        scene_tabs_sync_current(scene);
    }
    if (kind == APP_EDITOR_DOCUMENT_SCENE && !path && scene->active_tab >= 0 &&
        scene->active_tab < acount(scene->tabs) &&
        scene->tabs[scene->active_tab].kind == APP_EDITOR_DOCUMENT_SCENE &&
        !scene->tabs[scene->active_tab].path) {
        return;
    }
    char generated[64];
    if (kind == APP_EDITOR_DOCUMENT_SCENE && !path) {
        snprintf(generated, sizeof generated, "scene-%d", scene->next_scene_key);
        id = generated;
    }
    char *key = tab_key(
        kind,
        id,
        kind == APP_EDITOR_DOCUMENT_SCENE || kind == APP_EDITOR_DOCUMENT_SYSTEM ? path : NULL
    );
    if (!key) {
        return;
    }
    int index = document_tab_for_path(scene, kind, path);
    if (index < 0) {
        index = tab_find(scene, key);
    }
    free(key);
    /* Only scene_tabs_open_ui makes a UI document's tab, with its text. */
    if (index < 0 && kind == APP_EDITOR_DOCUMENT_UI) {
        return;
    }
    if (index < 0 && kind == APP_EDITOR_DOCUMENT_SCENE && acount(scene->tabs) == 1 &&
        scene->tabs[0].kind == APP_EDITOR_DOCUMENT_SCENE &&
        !scene->tabs[0].scene_value) {
        index = 0;
        scene_document_tab *tab = &scene->tabs[0];
        free(tab->key);
        tab->key = tab_key(kind, id, path);
        free(tab->label);
        tab->label = tab_label(kind, id, path);
        tab_set_path(tab, path);
        scene->next_scene_key++;
    }
    if (index < 0) {
        scene_document_tab *tab = add_tab(scene, kind, id, path);
        index = tab ? (int)(tab - scene->tabs) : -1;
        if (index >= 0 && kind == APP_EDITOR_DOCUMENT_SCENE && !path) {
            scene->next_scene_key++;
        }
    }
    if (index >= 0) {
        scene->active_tab = index;
        if (kind == APP_EDITOR_DOCUMENT_SCENE) {
            set_current_scene_tab(scene, &scene->tabs[index]);
        }
        if (index != previous && kind == APP_EDITOR_DOCUMENT_SCENE &&
            scene->tabs[index].scene_value) {
            restore_scene_tab_view(scene, &scene->tabs[index]);
        }
        if (kind != APP_EDITOR_DOCUMENT_SCENE || path || scene->tabs[index].scene_value) {
            capture_asset_baseline(scene, &scene->tabs[index]);
        }
    }
}

int scene_tabs_open_source(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *name,
    const char *path
)
{
    assert(
        kind == APP_EDITOR_DOCUMENT_BEHAVIOR || kind == APP_EDITOR_DOCUMENT_SYSTEM ||
        kind == APP_EDITOR_DOCUMENT_PROTOTYPE || kind == APP_EDITOR_DOCUMENT_RELATIONSHIP
    );
    scene_tabs_sync_current(scene);
    scene_document_tab *tab = add_tab(scene, kind, name, path);
    if (!tab) {
        return -1;
    }
    tab->source_only = true;
    scene->active_tab = (int)(tab - scene->tabs);
    scene_authoring_select_document(scene, kind, keyed_by_path(kind) ? path : name);
    free(scene->graph_document_id);
    scene->graph_document_id = NULL;
    scene->definition = NULL;
    scene->behavior_apply_pending = false;
    return 0;
}

static int restore_scene_tab_contents(app_scene_authoring *scene, scene_document_tab *tab)
{
    assert(tab->kind == APP_EDITOR_DOCUMENT_SCENE && tab->scene_value);
    uint64_t *old = tab->scene_entities;
    int count = tab->scene_entity_count;
    assert(!scene->scene_tab_restoring);
    scene->scene_tab_restoring = true;
    int loaded = scene_authoring_load_value(scene, tab->scene_value, tab->path);
    scene->scene_tab_restoring = false;
    if (loaded != 0) {
        return -1;
    }
    if (acount(tab->scene_actor_ids) == acount(scene->actors)) {
        for (int i = 0; i < acount(scene->actors); i++) {
            scene->actors[i].identity = 0;
        }
        for (int i = 0; i < acount(tab->scene_actor_ids); i++) {
            uint64_t identity = tab->scene_actor_ids[i];
            if (!identity || identity >= UINT64_MAX) {
                return -1;
            }
            for (int j = 0; j < i; j++) {
                if (tab->scene_actor_ids[j] == identity) {
                    return -1;
                }
            }
            if (identity >= scene->next_actor_id) {
                scene->next_actor_id = identity + 1;
            }
        }
        for (int i = 0; i < acount(scene->actors); i++) {
            scene_actor_set_identity(scene, &scene->actors[i], tab->scene_actor_ids[i]);
        }
    }
    uint64_t *replacement = NULL;
    int replacement_count = acount(scene->actors);
    for (int i = 0; i < replacement_count; i++) {
        apush(replacement, ecs_entity_to_u64(scene->actors[i].entity));
    }
    int mapped = count < replacement_count ? count : replacement_count;
    if (tab->scene_history &&
        !scene_history_restore_value(scene->history, tab->scene_history, old, replacement, mapped)) {
        afree(replacement);
        return -1;
    }
    if (!tab->scene_history) {
        scene_history_clear(scene->history);
    }
    afree(replacement);
    restore_scene_tab_view(scene, tab);
    return 0;
}

static int restore_system_tab(app_scene_authoring *scene, const scene_document_tab *tab)
{
    const char *text = av_get_str(av_obj_get(tab->source_state, "text"));
    const authoring_source *source = preview_source_find(scene->live, tab->path, false);
    if (!text && source) {
        text = source->syntax->text;
    }
    return text ? load_system_document_text(scene, text, tab->path, false)
                : open_system_asset(scene, tab->path);
}

int scene_tabs_activate(app_scene_authoring *scene, int index)
{
    if (index < 0 || index >= acount(scene->tabs) || index == scene->active_tab) {
        return index == scene->active_tab ? 0 : -1;
    }
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "tab.activate", &action_doc);
    yyjson_mut_obj_add_strcpy(
        action_doc,
        action_root,
        "target_tab",
        scene->tabs[index].key
    );
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int previous = scene->active_tab;
    int previous_scene = scene_tab_for_current_document(scene);
    scene_document_tab rollback = { .kind = APP_EDITOR_DOCUMENT_SCENE };
    scene_tabs_sync_current(scene);
    bool rollback_ready = previous_scene >= 0;
    if (rollback_ready) {
        capture_scene_tab(scene, &rollback);
        rollback_ready = rollback.scene_value != NULL;
    }
    scene->tabs_transitioning = true;
    scene->active_tab = index;
    scene_document_tab *target = &scene->tabs[index];
    int result = 0;
    if (target->source_only) {
        scene_authoring_select_document(
            scene,
            target->kind,
            keyed_by_path(target->kind)
                ? target->path
                : target->key + strlen(tab_kind_name(target->kind)) + 1
        );
        free(scene->graph_document_id);
        scene->graph_document_id = NULL;
        scene->definition = NULL;
        scene->behavior_apply_pending = false;
    } else if (target->kind == APP_EDITOR_DOCUMENT_SCENE) {
        /* Returning from an asset tab keeps the current scene session. */
        if (index != previous_scene) {
            if (target->scene_value) {
                result = restore_scene_tab_contents(scene, target);
            } else {
                capture_scene_tab(scene, target);
            }
        }
        if (result == 0) {
            scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
        }
    } else if (target->kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        result = app_scene_authoring_open_prototype(scene, target->key + strlen("prototype:"));
    } else if (target->kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        result = open_behavior_asset(scene, target->key + strlen("behavior:"));
    } else if (target->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        result = restore_system_tab(scene, target);
    } else if (target->kind == APP_EDITOR_DOCUMENT_UI) {
        scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_UI, target->path);
    }
    if (result == 0) {
        if (target->kind == APP_EDITOR_DOCUMENT_SCENE) {
            set_current_scene_tab(scene, target);
        }
    } else {
        scene->active_tab = previous;
        if (target->kind == APP_EDITOR_DOCUMENT_SCENE && rollback_ready) {
            scene->active_tab = previous_scene;
            int restored = restore_scene_tab_contents(scene, &rollback);
            assert(restored == 0);
            if (restored != 0) {
                scene->active_tab = previous;
                free_tab(&rollback);
                scene->tabs_transitioning = false;
                scene_action_finish(scene, action, false);
                free(action);
                return result;
            }
            scene->active_tab = previous;
            scene_document_tab *prior = &scene->tabs[previous];
            if (prior->source_only) {
                scene_authoring_select_document(
                    scene,
                    prior->kind,
                    keyed_by_path(prior->kind)
                        ? prior->path
                        : prior->key + strlen(tab_kind_name(prior->kind)) + 1
                );
            } else if (prior->kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
                app_scene_authoring_open_prototype(scene, prior->key + strlen("prototype:"));
            } else if (prior->kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
                open_behavior_asset(scene, prior->key + strlen("behavior:"));
            } else if (prior->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
                restore_system_tab(scene, prior);
            } else if (prior->kind == APP_EDITOR_DOCUMENT_UI) {
                scene_authoring_select_document(
                    scene,
                    APP_EDITOR_DOCUMENT_UI,
                    prior->path
                );
            }
            scene->active_tab = previous;
        }
    }
    scene->tabs_transitioning = false;
    free_tab(&rollback);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

int scene_tabs_open_scene(app_scene_authoring *scene, const char *path)
{
    if (!path) {
        return -1;
    }
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action_root = scene_action_root(scene, "tab.open_scene", &action_doc);
    yyjson_mut_obj_add_strcpy(action_doc, action_root, "path", path);
    char *action = scene_action_write(action_doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    char *json = read_scene_file(path);
    if (!json) {
        scene_action_finish(scene, action, false);
        free(action);
        return -1;
    }
    char *key = tab_key(APP_EDITOR_DOCUMENT_SCENE, path, path);
    int index = scene_tab_for_path(scene, path);
    if (index < 0) {
        index = key ? tab_find(scene, key) : -1;
    }
    free(key);
    bool created = false;
    if (index < 0) {
        scene_document_tab *tab = add_tab(scene, APP_EDITOR_DOCUMENT_SCENE, path, path);
        if (!tab) {
            free(json);
            scene_action_finish(scene, action, false);
            free(action);
            return -1;
        }
        char error[256];
        av_doc *native = asset_text_read(json, error, sizeof error);
        free(json);
        if (!native) {
            free_tab(tab);
            adel(scene->tabs, acount(scene->tabs) - 1);
            scene_action_finish(scene, action, false);
            free(action);
            return -1;
        }
        tab->scene_value = av_value_copy(av_doc_root(native));
        av_doc_free(native);
        tab->scene_dirty = false;
        index = (int)(tab - scene->tabs);
        created = true;
    } else {
        free(json);
    }
    int result = scene_tabs_activate(scene, index);
    if (result != 0 && created) {
        free_tab(&scene->tabs[index]);
        adel(scene->tabs, index);
        if (index < scene->active_tab) {
            scene->active_tab--;
        }
    }
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

bool scene_tabs_path_in_use(
    const app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *path
)
{
    if (!scene || !path) {
        return false;
    }
    int current = kind == APP_EDITOR_DOCUMENT_SCENE
                    ? scene_tab_for_current_document(scene)
                    : scene->active_tab;
    for (int i = 0; i < acount(scene->tabs); i++) {
        const scene_document_tab *tab = &scene->tabs[i];
        if (i != current && tab->kind == kind && tab->path &&
            strcmp(tab->path, path) == 0) {
            return true;
        }
    }
    return false;
}

static void omit_automatic_positions(av_value *value)
{
    if (av_is_obj(value)) {
        av_value *placed = av_obj_get(value, "placed");
        if (av_is_bool(placed) && !av_get_bool(placed)) {
            av_obj_remove_key(value, "x");
            av_obj_remove_key(value, "y");
            av_obj_remove_key(value, "placed");
        }
        for (size_t i = 0; i < av_obj_size(value); i++) {
            omit_automatic_positions(av_obj_value_at(value, i));
        }
    }
    if (av_is_arr(value)) {
        for (size_t i = 0; i < av_arr_size(value); i++) {
            omit_automatic_positions(av_arr_get(value, i));
        }
    }
}

static bool authored_values_equal(av_value *a, av_value *b)
{
    av_doc *doc = av_doc_new();
    av_value *left = av_clone(doc, a);
    av_value *right = av_clone(doc, b);
    omit_automatic_positions(left);
    omit_automatic_positions(right);
    bool equal = av_equals(left, right);
    av_doc_free(doc);
    return equal;
}

bool scene_tabs_dirty(app_scene_authoring *scene, const scene_document_tab *tab)
{
    if (tab->source_only || tab->source_dirty) {
        return tab->source_dirty;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_SCENE) {
        int current = scene_tab_for_current_document(scene);
        return current >= 0 && tab == &scene->tabs[current] ? scene->scene_dirty
                                                            : tab->scene_dirty;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        const char *name = tab->key + strlen("prototype:");
        scene_prototype_asset *asset = find_prototype(scene, name);
        return asset && asset->dirty;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        const char *name = tab->key + strlen("behavior:");
        preview_definition *definition = preview_find_definition(scene->live, name);
        return definition && definition->unsaved;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        const authoring_source *source = preview_source_find(scene->live, tab->path, false);
        preview_definition *definition = source ? preview_find_definition(
                                                      scene->live,
                                                      source->analysis->name
                                                  )
                                                : NULL;
        return tab->source_dirty || (definition && definition->unsaved);
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_UI) {
        return ui_buffer_dirty(tab->ui);
    }
    return false;
}

static int discard_prototype(app_scene_authoring *scene, scene_document_tab *tab)
{
    if (!tab->asset_saved) {
        return -1;
    }
    const char *name = tab->key + strlen("prototype:");
    scene_prototype_asset *asset = find_prototype(scene, name);
    if (!asset) {
        return -1;
    }
    char error[256];
    prototype *native_prototype = prototype_load_value(tab->asset_saved, &scene->registry, error, sizeof error);
    if (!native_prototype) {
        return -1;
    }
    prototype *previous = asset->prototype;
    asset->prototype = native_prototype;
    if (refresh_prototype_instances(scene, asset) != 0) {
        asset->prototype = previous;
        prototype_free(native_prototype);
        return -1;
    }
    prototype_free(previous);
    clear_prototype_history(asset);
    asset->dirty = 0;
    return 0;
}

static int discard_behavior(app_scene_authoring *scene, scene_document_tab *tab)
{
    const char *name = tab->key + strlen("behavior:");
    preview_definition *definition = preview_find_definition(scene->live, name);
    if (!tab->asset_saved) {
        /* A behavior shown from its source keeps no graph snapshot; with
           nothing unsaved there is nothing to discard. */
        return definition && !definition->unsaved ? 0 : -1;
    }
    behavior_asset *working = behavior_asset_from_value(tab->asset_saved);
    graph *graph = working ? behavior_asset_find_graph(working, 0) : NULL;
    if (!definition || !working || !graph) {
        behavior_asset_free(working);
        return -1;
    }
    behavior_asset *previous = definition->working;
    int dirty = definition->dirty;
    bool unsaved = definition->unsaved;
    definition->working = working;
    definition->dirty = 1;
    definition->unsaved = false;
    int result = preview_apply(
        scene->live,
        definition,
        scene->message,
        sizeof scene->message
    );
    if (result != 0) {
        definition->working = previous;
        definition->dirty = dirty;
        definition->unsaved = unsaved;
        behavior_asset_free(working);
        return -1;
    }
    result = editor_reload_behavior_asset(scene->graph_editor, working, graph->graph_id, name);
    assert(result == 0);
    refresh_asset_catalog(scene);
    behavior_asset_free(previous);
    scene->definition = definition;
    scene->editor_working = editor_behavior_asset(scene->graph_editor);
    scene->editor_revision_seen = editor_edit_revision(scene->graph_editor);
    scene->editor_behavior_asset_seen = scene->editor_working;
    scene->editor_definition_seen = definition;
    scene->editor_state_seen = true;
    scene->behavior_apply_pending = false;
    return 0;
}

static int discard_system(app_scene_authoring *scene, scene_document_tab *tab)
{
    char *text = read_scene_file(tab->path);
    if (!text) {
        return -1;
    }
    source_catalog_edit edit = { .path = tab->path, .text = text };
    char error[256] = "";
    int result = scene_publish_source_edits(scene, &edit, 1, error, sizeof error);
    if (!result) {
        result = load_system_document_text(scene, text, tab->path, true);
    }
    free(text);
    if (!result) {
        av_value_free(tab->source_state);
        tab->source_state = NULL;
        tab->source_dirty = false;
        scene_tabs_mark_document_saved(
            scene,
            APP_EDITOR_DOCUMENT_SYSTEM,
            scene->document.id
        );
    }
    return result;
}

static int discard_tab(app_scene_authoring *scene, int index)
{
    scene_document_tab *tab = &scene->tabs[index];
    if (tab->source_only) {
        return 0;
    }
    switch (tab->kind) {
        case APP_EDITOR_DOCUMENT_SCENE: return 0;
        case APP_EDITOR_DOCUMENT_PROTOTYPE:
            return discard_prototype(scene, tab);
        case APP_EDITOR_DOCUMENT_BEHAVIOR: return discard_behavior(scene, tab);
        case APP_EDITOR_DOCUMENT_SYSTEM: return discard_system(scene, tab);
        /* The edits go with the tab's buffer. */
        case APP_EDITOR_DOCUMENT_UI: return 0;
        case APP_EDITOR_DOCUMENT_RELATIONSHIP: return 0;
    }
    return -1;
}

bool scene_tabs_can_close(const app_scene_authoring *scene, int index)
{
    if (index < 0 || index >= acount(scene->tabs) ||
        scene->tabs[index].pinned || acount(scene->tabs) <= 1) {
        return false;
    }
    if (scene->tabs[index].kind != APP_EDITOR_DOCUMENT_SCENE) {
        return true;
    }
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (i != index && scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE) {
            return true;
        }
    }
    return false;
}

/* Pins form a stable leading group. Moving a tab only changes navigation;
   the active document and the underlying scene retain their identities. */
void scene_tabs_toggle_pin(app_scene_authoring *scene, int index)
{
    assert(index >= 0 && index < acount(scene->tabs));
    char *active_key = scene->tabs[scene->active_tab].key;
    scene_document_tab moved = scene->tabs[index];
    moved.pinned = !moved.pinned;
    int target = 0;
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (i != index && scene->tabs[i].pinned) {
            target++;
        }
    }
    if (target < index) {
        memmove(
            &scene->tabs[target + 1],
            &scene->tabs[target],
            (size_t)(index - target) * sizeof *scene->tabs
        );
    }
    if (target > index) {
        memmove(
            &scene->tabs[index],
            &scene->tabs[index + 1],
            (size_t)(target - index) * sizeof *scene->tabs
        );
    }
    scene->tabs[target] = moved;
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].key == active_key) {
            scene->active_tab = i;
            break;
        }
    }
}

/* A graph whose tab closes leaves the Behavior area, which shows no graph
   until another opens. */
static void forget_graph(app_scene_authoring *scene, const scene_document_tab *tab)
{
    char *key = scene->graph_document_id
                  ? tab_key(scene->graph_document_kind, scene->graph_document_id, NULL)
                  : NULL;
    if (key && !strcmp(key, tab->key)) {
        free(scene->graph_document_id);
        scene->graph_document_id = NULL;
    }
    free(key);
}

static int remove_tab(app_scene_authoring *scene, int index)
{
    if (!scene_tabs_can_close(scene, index)) {
        return -1;
    }
    const char *scene_key = scene_tabs_current_scene_key(scene);
    bool underlying = scene_key && !strcmp(scene_key, scene->tabs[index].key);
    if (underlying || index == scene->active_tab) {
        int next = -1;
        for (int distance = 1; distance < acount(scene->tabs) && next < 0; distance++) {
            int candidates[] = { index + distance, index - distance };
            for (int c = 0; c < 2; c++) {
                int i = candidates[c];
                if (i >= 0 && i < acount(scene->tabs) &&
                    (!underlying || scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE)) {
                    next = i;
                    break;
                }
            }
        }
        assert(next >= 0);
        if (scene_tabs_activate(scene, next) != 0) {
            return -1;
        }
    }
    forget_graph(scene, &scene->tabs[index]);
    free_tab(&scene->tabs[index]);
    int count = acount(scene->tabs);
    memmove(
        &scene->tabs[index],
        &scene->tabs[index + 1],
        (size_t)(count - index - 1) * sizeof *scene->tabs
    );
    asetlen(scene->tabs, count - 1);
    if (index < scene->active_tab) {
        scene->active_tab--;
    }
    return 0;
}

void scene_tabs_rename(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *from,
    const char *to,
    const char *path
)
{
    char *key = tab_key(kind, from, NULL);
    int index = tab_find(scene, key);
    free(key);
    if (index < 0) {
        return;
    }
    scene_document_tab *tab = &scene->tabs[index];
    free(tab->key);
    tab->key = tab_key(kind, to, NULL);
    free(tab->label);
    tab->label = tab_label(kind, to, path);
    tab_set_path(tab, path);
    capture_asset_baseline(scene, tab);
}

void scene_tabs_close(app_scene_authoring *scene, app_editor_document_kind kind, const char *id)
{
    char *key = tab_key(kind, id, NULL);
    int index = tab_find(scene, key);
    free(key);
    if (index >= 0) {
        remove_tab(scene, index);
    }
}

void scene_tabs_forget(app_scene_authoring *scene, app_editor_document_kind kind, const char *id)
{
    assert(kind == APP_EDITOR_DOCUMENT_BEHAVIOR || kind == APP_EDITOR_DOCUMENT_PROTOTYPE);
    char *key = tab_key(kind, id, NULL);
    int index = tab_find(scene, key);
    free(key);
    if (index >= 0) {
        scene->tabs[index].pinned = false;
        remove_tab(scene, index);
    }
}

int scene_tabs_discard(app_scene_authoring *scene, int index)
{
    if (!scene || index < 0 || index >= acount(scene->tabs)) {
        return -1;
    }
    if (!scene_tabs_can_close(scene, index)) {
        return -1;
    }
    scene_document_tab *tab = &scene->tabs[index];
    bool background = !tab->source_only && tab->kind != APP_EDITOR_DOCUMENT_SCENE &&
                      index != scene->active_tab;
    char *previous = background ? xstrdup(scene->tabs[scene->active_tab].key) : NULL;
    int result = background ? scene_tabs_activate(scene, index) : 0;
    if (result == 0) {
        result = discard_tab(scene, index);
    }
    if (result == 0) {
        result = remove_tab(scene, index);
    }
    /* Discarding a graph or prototype needs its live document, but closing
       a background tab keeps the document the user was working in. */
    if (previous) {
        int active = tab_find(scene, previous);
        assert(active >= 0);
        if (scene_tabs_activate(scene, active) != 0) {
            result = -1;
        }
    }
    free(previous);
    return result;
}

/* ---------- UI documents ---------- */

/* The owned absolute path of the document `path` belongs to
   (ui_buffer_document); NULL when it does not exist. */
static char *ui_tab_path(const char *path)
{
    char *absolute = realpath(path, NULL);
    char *document = absolute ? ui_buffer_document(absolute) : NULL;
    free(absolute);
    return document;
}

scene_document_tab *scene_tabs_ui(app_scene_authoring *scene, const char *path)
{
    char *document = ui_tab_path(path);
    const char *key = document ? document : path;
    scene_document_tab *found = NULL;
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_UI && !strcmp(tab->path, key)) {
            found = tab;
            break;
        }
    }
    free(document);
    return found;
}

/* A new tab holding the document's text, keyed by its absolute path. */
static int add_ui_tab(app_scene_authoring *scene, const char *path)
{
    char error[256] = "no such file";
    char *absolute = ui_tab_path(path);
    ui_buffer *buffer = absolute ? ui_buffer_open(absolute, error, sizeof error) : NULL;
    if (!buffer) {
        snprintf(scene->message, sizeof scene->message, "Could not open %s: %s", content_basename(path), error);
        free(absolute);
        return -1;
    }
    scene_document_tab tab = {
        .kind = APP_EDITOR_DOCUMENT_UI,
        .key = tab_key(APP_EDITOR_DOCUMENT_UI, absolute, absolute),
        .label = tab_label(APP_EDITOR_DOCUMENT_UI, NULL, absolute),
        .path = absolute,
        .ui = buffer,
    };
    apush(scene->tabs, tab);
    return acount(scene->tabs) - 1;
}

int scene_tabs_open_ui(app_scene_authoring *scene, const char *path)
{
    scene_document_tab *open = scene_tabs_ui(scene, path);
    int index = open ? (int)(open - scene->tabs) : add_ui_tab(scene, path);
    return index < 0 ? -1 : scene_tabs_activate(scene, index);
}
