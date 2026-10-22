/* Content another program changed on disk reloads in place. Graph assets and
 * systems swap into the running world; prototypes refresh their instances and
 * the open scene reloads, keeping the view and selection, once the runtime
 * stops. A document with unsaved edits keeps them until reloaded by path. */
#include "assets/input_map.h"
#include "authoring/preview_source.h"
#include "authoring/source_presentation.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const GRAPH_FOLDERS[] = {
    "behaviors", "events",           "fields", "libraries",
    "materials", "shader-libraries", "shapes", "models",
};

static bool in_graph_folder(const app_scene_authoring *scene, const char *path)
{
    for (size_t i = 0; i < sizeof GRAPH_FOLDERS / sizeof *GRAPH_FOLDERS; i++) {
        if (content_path_in_folder(scene, path, GRAPH_FOLDERS[i])) {
            return true;
        }
    }
    return false;
}

/* The runtime's world holds prototypes and the scene while it runs. */
static bool waits_for_runtime(const app_scene_authoring *scene, const char *path)
{
    return runtime_session(scene) &&
           (content_path_in_folder(scene, path, "prototypes") ||
            content_path_in_folder(scene, path, "scenes"));
}

static scene_document_tab *tab_at(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *path
)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind == kind && tab->path && !strcmp(tab->path, path)) {
            return tab;
        }
    }
    return NULL;
}

static app_disk_result add_definition(app_scene_authoring *scene, const char *path)
{
    char *json = read_scene_file(path);
    char error[256] = "";
    if (source_catalog_source_path(scene->content_root, path)) {
        source_catalog_edit edit = { .path = path, .text = json };
        int status = json ? scene_publish_source_edits(scene, &edit, 1, error, sizeof error)
                          : -1;
        free(json);
        snprintf(
            scene->message,
            sizeof scene->message,
            status ? "Could not load %s: %s" : "Added %s.",
            content_basename(path),
            error
        );
        return status ? APP_DISK_FAILED : APP_DISK_RELOADED;
    }
    behavior_asset *asset = scene_graph_source_path(scene, json, path, error, sizeof error);
    free(json);
    preview_definition *definition = asset ? preview_definition_new(asset->name, asset)
                                           : NULL;
    if (!definition) {
        behavior_asset_free(asset);
        snprintf(scene->message, sizeof scene->message, "Could not load %s: %s", content_basename(path), error);
        return APP_DISK_FAILED;
    }
    preview_definition_set_path(definition, path);
    if (preview_add_definition(scene->live, definition) != 0) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Could not add %s.",
            definition->name
        );
        preview_definition_free(definition);
        return APP_DISK_FAILED;
    }
    if (scene->prototype_preview) {
        prototype_preview_register_behavior(
            scene->prototype_preview,
            definition->name,
            definition->working
        );
    }
    snprintf(scene->message, sizeof scene->message, "Added %s.", definition->name);
    scene_notice(scene);
    return APP_DISK_RELOADED;
}

static app_disk_result sync_graph(app_scene_authoring *scene, const char *path, bool force)
{
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        const char *at = preview_definition_path(definition);
        if (!at || strcmp(at, path)) {
            continue;
        }
        if (definition->unsaved && !force) {
            return APP_DISK_UNSAVED;
        }
        return reload_behavior_from_disk(scene, definition->name) == 0
                 ? APP_DISK_RELOADED
                 : APP_DISK_FAILED;
    }
    return add_definition(scene, path);
}

/* The runtime takes the new version at once; an open document follows it. */
static app_disk_result sync_system(app_scene_authoring *scene, const char *path, bool force)
{
    scene_document_tab *tab = tab_at(scene, APP_EDITOR_DOCUMENT_SYSTEM, path);
    if (tab && scene_tabs_dirty(scene, tab) && !force) {
        return APP_DISK_UNSAVED;
    }
    char *json = read_scene_file(path);
    char error[256] = "not a system";
    if (source_catalog_source_path(scene->content_root, path)) {
        source_catalog_edit edit = { .path = path, .text = json };
        int status = json ? scene_publish_source_edits(scene, &edit, 1, error, sizeof error)
                          : -1;
        if (!status && scene->system_path && !strcmp(scene->system_path, path)) {
            status = load_system_document_text(scene, json, path, true);
        } else if (!status) {
            authoring_source_presentation *
                presentation = scene_source_projection_path(scene, json, path, error, sizeof error);
            status = presentation
                       ? editor_reload_source_presentation_document(scene->graph_editor, presentation, path)
                       : -1;
            authoring_source_presentation_free(presentation);
        }
        free(json);
        snprintf(
            scene->message,
            sizeof scene->message,
            status ? "Could not reload %s: %s" : "Reloaded %s.",
            content_basename(path),
            error
        );
        if (!status) {
            scene_notice(scene);
        }
        return status ? APP_DISK_FAILED : APP_DISK_RELOADED;
    }
    free(json);
    snprintf(
        scene->message,
        sizeof scene->message,
        "Could not reload %s: systems require canonical source.",
        content_basename(path)
    );
    return APP_DISK_FAILED;
}

static bool prototype_uses(const prototype *asset, const char *name)
{
    for (int i = 0; i < acount(asset->includes); i++) {
        if (!strcmp(asset->includes[i], name)) {
            return true;
        }
    }
    for (int i = 0; asset->children && i < acount(asset->children->entities); i++) {
        const char *child = asset->children->entities[i].prototype;
        if (child && !strcmp(child, name)) {
            return true;
        }
    }
    return false;
}

/* The resolver's new text also changes flattened includes and child prototypes.
 * Collect the transitive users before replacing any borrowed prototype names. */
static app_disk_result reload_prototype_users(app_scene_authoring *scene, int changed)
{
    int *affected = NULL;
    bool *seen = xcalloc((size_t)acount(scene->prototypes), sizeof *seen);
    seen[changed] = true;
    apush(affected, changed);
    for (int next = 0; next < acount(affected); next++) {
        const char *name = scene->prototypes[affected[next]].prototype->name;
        for (int i = 0; i < acount(scene->prototypes); i++) {
            if (!seen[i] && prototype_uses(scene->prototypes[i].prototype, name)) {
                seen[i] = true;
                apush(affected, i);
            }
        }
    }
    free(seen);
    app_disk_result result = APP_DISK_RELOADED;
    char message[256] = "";
    for (int i = 1; i < acount(affected); i++) {
        scene_prototype_asset *asset = &scene->prototypes[affected[i]];
        if (!asset->path || scene_prototype_dirty(asset)) {
            if (result != APP_DISK_FAILED) {
                result = APP_DISK_UNSAVED;
                snprintf(
                    message,
                    sizeof message,
                    "%s has unsaved edits; reload it to apply changed "
                    "prototype dependencies.",
                    asset->prototype->name
                );
            }
        } else if (reload_prototype_asset(scene, asset) < 0) {
            result = APP_DISK_FAILED;
            snprintf(message, sizeof message, "%s", scene->message);
        }
    }
    afree(affected);
    if (message[0]) {
        snprintf(scene->message, sizeof scene->message, "%s", message);
    }
    return result;
}

static app_disk_result sync_prototype(app_scene_authoring *scene, const char *path, bool force)
{
    for (int i = 0; i < acount(scene->prototypes); i++) {
        scene_prototype_asset *asset = &scene->prototypes[i];
        if (!asset->path || strcmp(asset->path, path)) {
            continue;
        }
        if (scene_prototype_dirty(asset) && !force) {
            return APP_DISK_UNSAVED;
        }
        return reload_prototype_asset(scene, asset) >= 0
                 ? reload_prototype_users(scene, i)
                 : APP_DISK_FAILED;
    }
    char *json = read_scene_file(path);
    bool added = json &&
                 app_scene_authoring_register_prototype(scene, json, path) == 0;
    free(json);
    snprintf(
        scene->message,
        sizeof scene->message,
        added ? "Added %s." : "Could not load %s.",
        content_basename(path)
    );
    return added ? APP_DISK_RELOADED : APP_DISK_FAILED;
}

/* Reloading the open scene keeps the camera and the selection by name. */
static int reload_scene(app_scene_authoring *scene)
{
    char *active = scene->active_tab >= 0 && scene->active_tab < acount(scene->tabs)
                     ? xstrdup(scene->tabs[scene->active_tab].key)
                     : NULL;
    scene_camera camera = scene->camera;
    int target = scene->camera_target;
    scene_actor *actor = selected_actor(scene);
    char *selected = actor ? xstrdup(actor->name) : NULL;
    char *path = xstrdup(scene->scene_path);
    int result = app_scene_authoring_load(scene, path);
    free(path);
    if (result == 0) {
        scene->camera = camera;
        scene->camera_target = target;
        scene->selected = -1;
        for (int i = 0; selected && i < acount(scene->actors); i++) {
            if (!strcmp(scene->actors[i].name, selected)) {
                select_instance(scene, i);
            }
        }
    }
    free(selected);
    for (int i = 0; active && i < acount(scene->tabs); i++) {
        if (!strcmp(scene->tabs[i].key, active)) {
            if (scene_tabs_activate(scene, i) != 0) {
                result = -1;
            }
            break;
        }
    }
    free(active);
    return result;
}

/* The open scene reloads; another open scene takes the file's text for when
 * its tab returns. */
static app_disk_result sync_scene(app_scene_authoring *scene, const char *path, bool force)
{
    if (scene->scene_path && !strcmp(scene->scene_path, path)) {
        if (scene->scene_dirty && !force) {
            return APP_DISK_UNSAVED;
        }
        return reload_scene(scene) == 0 ? APP_DISK_RELOADED : APP_DISK_FAILED;
    }
    scene_document_tab *tab = tab_at(scene, APP_EDITOR_DOCUMENT_SCENE, path);
    if (!tab) {
        return APP_DISK_NONE;
    }
    if (tab->scene_dirty && !force) {
        return APP_DISK_UNSAVED;
    }
    char *json = read_scene_file(path);
    if (!json) {
        snprintf(scene->message, sizeof scene->message, "Could not read %s.", content_basename(path));
        return APP_DISK_FAILED;
    }
    char error[256];
    av_doc *native = asset_document_read(json, error, sizeof error);
    free(json);
    if (!native) {
        snprintf(scene->message, sizeof scene->message, "Could not reload %s: %s", content_basename(path), error);
        return APP_DISK_FAILED;
    }
    av_value_free(tab->scene_value);
    tab->scene_value = av_value_copy(av_doc_root(native));
    av_doc_free(native);
    av_value_free(tab->scene_history);
    tab->scene_history = NULL;
    tab->scene_dirty = false;
    afree(tab->scene_entities);
    tab->scene_entity_count = 0;
    afree(tab->scene_actor_ids);
    tab->selected_entity = 0;
    tab->selected = -1;
    snprintf(scene->message, sizeof scene->message, "Reloaded %s.", content_basename(path));
    scene_notice(scene);
    return APP_DISK_RELOADED;
}

/* The input map declares the `input.<Action>` members scripts read, so every
 * source prepares again with its text. */
static app_disk_result sync_input_map(app_scene_authoring *scene, const char *path)
{
    char *text = read_scene_file(path);
    char error[256] = "";
    source_catalog_edit edit = { .path = path, .text = text };
    int status = text ? scene_publish_source_edits(scene, &edit, 1, error, sizeof error) : -1;
    free(text);
    snprintf(
        scene->message,
        sizeof scene->message,
        status ? "Could not reload %s: %s" : "Reloaded %s.",
        content_basename(path),
        error
    );
    if (!status) {
        scene_notice(scene);
    }
    return status ? APP_DISK_FAILED : APP_DISK_RELOADED;
}

static bool input_map_path(const app_scene_authoring *scene, const char *path)
{
    char *map = content_path_join(scene->content_root, INPUT_MAP_FILE);
    bool same = !strcmp(map, path);
    free(map);
    return same;
}

/* A removed file leaves what the editor loaded from it. */
static app_disk_result sync_path(app_scene_authoring *scene, const char *path, bool force)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        return APP_DISK_NONE;
    }
    if (input_map_path(scene, path)) {
        return sync_input_map(scene, path);
    }
    if (in_graph_folder(scene, path)) {
        return sync_graph(scene, path, force);
    }
    if (content_path_in_folder(scene, path, "systems")) {
        return sync_system(scene, path, force);
    }
    if (content_path_in_folder(scene, path, "prototypes")) {
        return sync_prototype(scene, path, force);
    }
    if (content_path_in_folder(scene, path, "scenes")) {
        return sync_scene(scene, path, force);
    }
    return APP_DISK_NONE;
}

static void push_unique(char ***paths, const char *path)
{
    for (int i = 0; i < acount(*paths); i++) {
        if (!strcmp((*paths)[i], path)) {
            return;
        }
    }
    apush(*paths, xstrdup(path));
}

static void add_change(app_disk_change **changes, app_disk_result result, const char *path, const char *message)
{
    app_disk_change change = { .result = result, .path = xstrdup(path) };
    snprintf(change.message, sizeof change.message, "%s", message);
    apush(*changes, change);
}

/* Open UI documents follow their files wherever they are, as a ui asset of
 * another project or a stylesheet: one without edits reloads. */
static app_disk_change *sync_ui_documents(app_scene_authoring *scene, double now)
{
    app_disk_change *changes = NULL;
    for (int i = 0; i < acount(scene->tabs); i++) {
        scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind != APP_EDITOR_DOCUMENT_UI || !ui_buffer_poll_disk(tab->ui, now)) {
            continue;
        }
        app_disk_result result = ui_buffer_dirty(tab->ui) ? APP_DISK_UNSAVED
                               : reload_ui_document(scene, tab) == 0
                                   ? APP_DISK_RELOADED
                                   : APP_DISK_FAILED;
        add_change(&changes, result, tab->path, scene->message);
    }
    return changes;
}

app_disk_change *app_scene_authoring_sync_disk(app_scene_authoring *scene, double now)
{
    app_disk_change *changes = sync_ui_documents(scene, now);
    if (!scene->content_root) {
        return changes;
    }
    char **changed = tree_watch_poll(&scene->disk, scene->content_root, CODE_ASSET_EXTENSION, now);
    if (!changed && (!acount(scene->disk_pending) || runtime_session(scene))) {
        return changes;
    }
    if (changed && !content_refresh(scene)) {
        add_change(&changes, APP_DISK_FAILED, scene->content_root, scene->message);
    }
    char **paths = scene->disk_pending;
    scene->disk_pending = NULL;
    for (int i = 0; i < acount(changed); i++) {
        push_unique(&paths, changed[i]);
    }
    tree_watch_changes_free(changed);
    for (int i = 0; i < acount(paths); i++) {
        if (waits_for_runtime(scene, paths[i])) {
            push_unique(&scene->disk_pending, paths[i]);
            continue;
        }
        app_disk_result result = sync_path(scene, paths[i], false);
        if (result != APP_DISK_NONE) {
            add_change(&changes, result, paths[i], scene->message);
        }
    }
    tree_watch_changes_free(paths);
    return changes;
}

void app_disk_changes_free(app_disk_change *changes)
{
    for (int i = 0; i < acount(changes); i++) {
        free(changes[i].path);
    }
    afree(changes);
}

app_disk_result app_scene_authoring_reload_path(app_scene_authoring *scene, const char *path)
{
    scene_document_tab *ui = tab_at(scene, APP_EDITOR_DOCUMENT_UI, path);
    if (ui) {
        return reload_ui_document(scene, ui) == 0 ? APP_DISK_RELOADED : APP_DISK_FAILED;
    }
    if (!content_refresh(scene)) {
        return APP_DISK_FAILED;
    }
    return sync_path(scene, path, true);
}

/* ---------- Refactors ---------- */

bool scene_same_file(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    char *left = realpath(a, NULL), *right = realpath(b, NULL);
    bool same = left && right ? !strcmp(left, right) : !strcmp(a, b);
    free(left);
    free(right);
    return same;
}

static preview_definition *definition_file(app_scene_authoring *scene, const char *path)
{
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        if (scene_same_file(preview_definition_path(definition), path)) {
            return definition;
        }
    }
    return NULL;
}

static scene_prototype_asset *prototype_file(app_scene_authoring *scene, const char *path)
{
    for (int i = 0; i < acount(scene->prototypes); i++) {
        if (scene_same_file(scene->prototypes[i].path, path)) {
            return &scene->prototypes[i];
        }
    }
    return NULL;
}

static void rename_id(char **id, const char *from, const char *to)
{
    if (*id && !strcmp(*id, from)) {
        free(*id);
        *id = xstrdup(to);
    }
}

/* The open document, the graph editor's and the tabs follow a rename. */
static void rename_document(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *from,
    const char *to,
    const char *path
)
{
    scene_tabs_rename(scene, kind, from, to, path);
    if (scene->document.kind == kind) {
        rename_id(&scene->document_id, from, to);
        scene->document.id = scene->document_id;
    }
    if (scene->graph_document_kind == kind) {
        rename_id(&scene->graph_document_id, from, to);
    }
}

/* A renamed prototype keeps its place, tab and instances. */
static void move_prototype(app_scene_authoring *scene, scene_prototype_asset *asset, const char *to)
{
    char *json = read_scene_file(to);
    char error[256];
    prototype *loaded = json ? prototype_load(json, &scene->registry, error, sizeof error)
                             : NULL;
    free(json);
    if (!loaded) {
        snprintf(scene->message, sizeof scene->message, "Could not load %s.", content_basename(to));
        return;
    }
    char *from = xstrdup(asset->prototype->name);
    for (int i = 0; i < acount(scene->actors); i++) {
        rename_id(&scene->actors[i].prototype_name, from, loaded->name);
    }
    for (int i = 0; i < acount(scene->tabs); i++) {
        rename_id(&scene->tabs[i].selected_prototype_name, from, loaded->name);
    }
    free(asset->prototype->name);
    asset->prototype->name = xstrdup(loaded->name);
    free(asset->path);
    asset->path = xstrdup(to);
    rename_document(scene, APP_EDITOR_DOCUMENT_PROTOTYPE, from, loaded->name, to);
    prototype_free(loaded);
    reload_prototype_asset(scene, asset);
    free(from);
}

/* A renamed definition keeps its place, tab, attachments and history. */
static void move_definition(app_scene_authoring *scene, preview_definition *definition, const char *to)
{
    char *json = read_scene_file(to);
    char error[256] = "";
    if (definition->working_presentation) {
        char *from = xstrdup(definition->name);
        char *old_path = xstrdup(preview_definition_path(definition));
        const authoring_source *old = preview_source_find(scene->live, old_path, false);
        char *identity = old ? xstrdup(old->analysis->identity) : NULL;
        source_catalog_edit edits[] = {
            { .path = old_path },
            { .path = to, .text = json, .identity = identity },
        };
        authoring_source_presentation
            *presentation = scene_source_projection_path(scene, json, to, error, sizeof error);
        const behavior_asset *view = presentation
                                       ? authoring_source_presentation_view(presentation)
                                       : NULL;
        char *name = view ? xstrdup(view->name) : NULL;
        bool active = scene->definition == definition;
        int status = !name ||
                     scene_publish_source_edits(scene, edits, 2, error, sizeof error);
        if (!status) {
            authoring_source_presentation_free(presentation);
            presentation = authoring_source_presentation_clone(
                preview_find_definition(scene->live, name)->working_presentation
            );
            editor_rename_behavior_asset(scene->graph_editor, from, name);
            rename_document(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, from, name, to);
            status = editor_reload_source_presentation_document(scene->graph_editor, presentation, name);
            if (active) {
                scene->definition = preview_find_definition(scene->live, name);
                scene->editor_working = editor_behavior_asset(scene->graph_editor);
                scene->editor_state_seen = false;
            }
        }
        if (status) {
            snprintf(
                scene->message,
                sizeof scene->message,
                "Could not move %s: %s",
                content_basename(to),
                error
            );
        }
        authoring_source_presentation_free(presentation);
        free(name);
        free(old_path);
        free(identity);
        free(from);
        free(json);
        return;
    }
    behavior_asset *loaded = scene_graph_source_path(scene, json, to, error, sizeof error);
    free(json);
    char *from = xstrdup(definition->name);
    char *name = loaded ? xstrdup(loaded->name) : NULL;
    behavior_asset_free(loaded);
    if (!name || preview_rename_definition(scene->live, from, name) != 0) {
        snprintf(scene->message, sizeof scene->message, "Could not load %s: %s", content_basename(to), error);
        free(from);
        free(name);
        return;
    }
    preview_definition_set_path(definition, to);
    editor_rename_behavior_asset(scene->graph_editor, from, name);
    rename_document(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, from, name, to);
    if (scene->prototype_preview) {
        prototype_preview_register_behavior(
            scene->prototype_preview,
            name,
            definition->working
        );
    }
    reload_behavior_from_disk(scene, name);
    free(from);
    free(name);
}

/* Another definition for the graph editor while one goes. */
static preview_definition *other_definition(app_scene_authoring *scene, const preview_definition *leaving)
{
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        if (definition != leaving) {
            return definition;
        }
    }
    return NULL;
}

/* A definition whose file went: its tab closes, the graph editor turns to
   another, and it leaves the live world unless the world still runs it. */
static void forget_definition(app_scene_authoring *scene, preview_definition *leaving)
{
    char *name = xstrdup(leaving->name);
    if (leaving->working_presentation) {
        char *path = xstrdup(preview_definition_path(leaving));
        source_catalog_edit edit = { .path = path };
        char error[256] = "";
        bool active = scene->definition == leaving;
        if (scene_publish_source_edits(scene, &edit, 1, error, sizeof error)) {
            snprintf(scene->message, sizeof scene->message, "Could not remove %s: %s", name, error);
            free(path);
            free(name);
            return;
        }
        scene_tabs_forget(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, name);
        editor_forget_behavior_asset(scene->graph_editor, name);
        if (active) {
            scene->definition = NULL;
            scene->editor_working = NULL;
            if (preview_definition_count(scene->live)) {
                open_behavior_graph(scene, preview_definition_at(scene->live, 0)->name);
            }
        }
        scene->editor_definition_seen = scene->definition;
        scene->editor_state_seen = false;
        free(path);
        free(name);
        return;
    }
    scene_tabs_forget(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, name);
    preview_definition *shown = other_definition(scene, leaving);
    if (scene->definition == leaving && shown) {
        open_behavior_graph(scene, shown->name);
    }
    editor_forget_behavior_asset(scene->graph_editor, name);
    if (scene->definition == leaving) {
        preview_definition_set_path(leaving, NULL);
        free(name);
        return;
    }
    if (scene->editor_definition_seen == leaving) {
        scene->editor_definition_seen = scene->definition;
    }
    /* Attached instances keep it, with its path, until they go. */
    preview_remove_definition(scene->live, name);
    free(name);
}

static void forget_index(int *index, int removed)
{
    *index = *index == removed ? -1 : *index - (*index > removed);
}

/* A deleted prototype goes, unless the open scene still places it; then it
   stays, unsaved. */
static void forget_prototype(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    const char *name = asset->prototype->name;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (!strcmp(scene->actors[i].prototype_name, name)) {
            if (asset->source && scene_prototype_source_owned(asset)) {
                asset_code_file_removed(asset->source);
                scene_prototype_source_capture(scene, asset);
                snprintf(scene->message, sizeof scene->message, "Prototype source was deleted; its draft is retained. Restore the file, then save or reload it.");
                return;
            }
            free(asset->path);
            asset->path = NULL;
            asset->dirty = 1;
            return;
        }
    }
    if (previewed_prototype(scene) == asset) {
        app_scene_authoring_stop_prototype_preview(scene);
    }
    scene_tabs_forget(scene, APP_EDITOR_DOCUMENT_PROTOTYPE, name);
    int removed = (int)(asset - scene->prototypes);
    forget_index(&scene->selected_prototype, removed);
    forget_index(&scene->runtime_preview_prototype, removed);
    for (int i = 0; i < acount(scene->tabs); i++) {
        forget_index(&scene->tabs[i].selected_prototype, removed);
    }
    scene_prototype_source_free(asset);
    prototype_free(asset->prototype);
    free(asset->path);
    clear_prototype_history(asset);
    int count = acount(scene->prototypes);
    memmove(asset, asset + 1, (size_t)(count - removed - 1) * sizeof *asset);
    asetlen(scene->prototypes, count - 1);
}

static bool named_file(const char *path, const char *name)
{
    const char *base = content_basename(path);
    size_t length = strlen(name);
    return !strncmp(base, name, length) &&
           !strcmp(base + length, CODE_ASSET_EXTENSION);
}

/* A file that comes back, as Undo brings back a deleted one, takes up what
   Studio kept of it without a path: a prototype the scene still places, or
   the definition the graph editor still shows. */
static bool adopt_file(app_scene_authoring *scene, const char *path)
{
    bool prototypes = content_path_in_folder(scene, path, "prototypes");
    for (int i = 0; prototypes && i < acount(scene->prototypes); i++) {
        scene_prototype_asset *asset = &scene->prototypes[i];
        if (!asset->path && named_file(path, asset->prototype->name)) {
            asset->path = xstrdup(path);
            asset->dirty = 0;
            reload_prototype_asset(scene, asset);
            return true;
        }
    }
    bool graphs = in_graph_folder(scene, path);
    for (int i = 0; graphs && i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        if (!preview_definition_path(definition) && named_file(path, definition->name)) {
            preview_definition_set_path(definition, path);
            reload_behavior_from_disk(scene, definition->name);
            return true;
        }
    }
    return false;
}

static void forget_file(app_scene_authoring *scene, const char *from)
{
    preview_definition *definition = definition_file(scene, from);
    scene_prototype_asset *prototype = prototype_file(scene, from);
    if (definition) {
        forget_definition(scene, definition);
    }
    if (prototype) {
        forget_prototype(scene, prototype);
    }
}

void scene_disk_note(app_scene_authoring *scene, const char *path)
{
    tree_watch_note(&scene->disk, path);
}

void scene_disk_follow(app_scene_authoring *scene, const scene_move *moves, int count)
{
    for (int i = 0; i < count; i++) {
        if (moves[i].from) {
            scene_disk_note(scene, moves[i].from);
        }
        if (moves[i].to) {
            scene_disk_note(scene, moves[i].to);
        }
    }
    content_refresh(scene);
    /* New and renamed files first, so the files that name them find them. */
    for (int i = 0; i < count; i++) {
        const scene_move *move = &moves[i];
        bool moved = move->to && (!move->from || strcmp(move->from, move->to));
        scene_prototype_asset *prototype = moved ? prototype_file(scene, move->from)
                                                 : NULL;
        preview_definition *definition = moved ? definition_file(scene, move->from)
                                               : NULL;
        scene_prototype_asset *returning = move->to ? prototype_file(scene, move->to)
                                                    : NULL;
        if ((prototype && prototype->source &&
             asset_code_file_missing(prototype->source)) ||
            (returning && returning->source &&
             asset_code_file_missing(returning->source))) {
            snprintf(scene->message, sizeof scene->message, "Prototype source returned; its retained draft is unchanged. Save or reload to resolve it.");
            continue;
        }
        if (prototype) {
            move_prototype(scene, prototype, move->to);
        } else if (definition) {
            move_definition(scene, definition, move->to);
        } else if (moved && (move->from || !adopt_file(scene, move->to))) {
            sync_path(scene, move->to, true);
        }
    }
    for (int i = 0; i < count; i++) {
        if (moves[i].from && moves[i].to && !strcmp(moves[i].from, moves[i].to)) {
            scene_prototype_asset *asset = prototype_file(scene, moves[i].to);
            if (!asset || !asset->source || !asset_code_file_missing(asset->source)) {
                sync_path(scene, moves[i].to, true);
            }
        }
    }
    for (int i = 0; i < count; i++) {
        if (moves[i].from && !moves[i].to) {
            forget_file(scene, moves[i].from);
        }
    }
    refresh_asset_catalog(scene);
    asset_index_invalidate(scene);
}
