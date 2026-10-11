#include "authoring/preview_source.h"
#include "authoring/source_format.h"
#include "authoring/source_metadata.h"
#include "authoring/source_presentation.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/scene_internal.h"
#include "execution/backend.h"
#include "graph/document/serialize.h"
#include "graph/shader/shader_graph.h"
#include "runtime/component_catalog.h"
#include "support/asset_text.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const mode_t DOCUMENT_DIRECTORY_MODE = 511;

int open_behavior_asset(app_scene_authoring *scene, const char *asset_name);

int scene_stage_source_edits(
    app_scene_authoring *scene,
    const source_catalog_edit *edits,
    int count,
    preview_source_staging **stages,
    const char *restart_path,
    char *error,
    size_t size
)
{
    preview_world *worlds[] = {
        scene->live,
        scene->prototype_preview ? prototype_preview_live(scene->prototype_preview) : NULL,
    };
    for (int i = 0; i < 2; i++) {
        if (!worlds[i]) {
            continue;
        }
        if ((!preview_sources_working(worlds[i]) &&
             preview_sources_prepare(worlds[i], scene->content_root, behavior_default_backend(), error, size)) ||
            (i == 0 && count &&
             preview_sources_edit(worlds[i], edits, count, error, size))) {
            return -1;
        }
        if (i == 1) {
            const authoring_sources *sources = preview_sources_working(scene->live);
            source_catalog_edit *membership = NULL;
            for (int j = 0; j < authoring_sources_count(sources); j++) {
                const authoring_source *source = authoring_sources_at(sources, j);
                apush(
                    membership,
                    ((source_catalog_edit){ .path = source->path,
                                            .text = source->syntax->text,
                                            .identity = source->analysis->identity })
                );
            }
            for (int j = 0; j < preview_sources_overlay_count(scene->live); j++) {
                const source_catalog_edit
                    *overlay = preview_sources_overlay_at(scene->live, j);
                if (!source_catalog_source_path(scene->content_root, overlay->path)) {
                    apush(membership, *overlay);
                }
            }
            int status = preview_sources_replace(worlds[i], membership, acount(membership), error, size);
            afree(membership);
            if (status) {
                return -1;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        if (!worlds[i]) {
            continue;
        }
        stages[i] = preview_sources_stage(worlds[i], restart_path, error, size);
        if (!stages[i]) {
            for (int j = 0; j < i; j++) {
                if (stages[j]) {
                    preview_sources_discard(stages[j]);
                    stages[j] = NULL;
                }
            }
            return -1;
        }
    }
    return 0;
}

void scene_finish_source_edits(app_scene_authoring *scene, preview_source_staging **stages, bool commit)
{
    char *name = scene->definition ? xstrdup(scene->definition->name) : NULL;
    const char *path = scene->definition ? preview_definition_path(scene->definition)
                                         : NULL;
    const authoring_source *source = path ? preview_source_find(scene->live, path, true)
                                          : NULL;
    char *identity = source ? xstrdup(source->analysis->identity) : NULL;
    for (int i = 0; i < 2; i++) {
        if (stages[i]) {
            if (commit) {
                preview_sources_commit(stages[i]);
            } else {
                preview_sources_discard(stages[i]);
            }
            stages[i] = NULL;
        }
    }
    if (commit && name) {
        scene->definition = preview_find_definition(scene->live, name);
        if (!scene->definition && identity) {
            const authoring_sources *sources = preview_sources_working(scene->live);
            for (int i = 0; i < authoring_sources_count(sources); i++) {
                const authoring_source *current = authoring_sources_at(sources, i);
                if (!strcmp(current->analysis->identity, identity)) {
                    scene->definition = preview_find_definition(
                        scene->live,
                        current->analysis->name
                    );
                    break;
                }
            }
        }
        scene->editor_definition_seen = scene->definition;
        scene->editor_state_seen = false;
    }
    free(identity);
    free(name);
    refresh_asset_catalog(scene);
}

int scene_publish_source_edits(
    app_scene_authoring *scene,
    const source_catalog_edit *edits,
    int count,
    char *error,
    size_t size
)
{
    return scene_publish_source_edits_restart(scene, edits, count, NULL, error, size);
}

int scene_publish_sources(app_scene_authoring *scene, const char *restart_path, char *error, size_t size)
{
    return scene_publish_source_edits_restart(scene, NULL, 0, restart_path, error, size);
}

int scene_publish_source_edits_restart(
    app_scene_authoring *scene,
    const source_catalog_edit *edits,
    int count,
    const char *restart_path,
    char *error,
    size_t size
)
{
    preview_source_staging *stages[2] = { 0 };
    if (scene_stage_source_edits(scene, edits, count, stages, restart_path, error, size)) {
        return -1;
    }
    scene_finish_source_edits(scene, stages, true);
    return 0;
}

static int save_source_view(
    app_scene_authoring *scene,
    const authoring_source_presentation *presentation,
    const char *path,
    const char *old_path
)
{
    char error[256] = "";
    script_syntax *before = script_parse(authoring_source_presentation_text(presentation));
    char *text = authoring_source_format(before, error, sizeof error);
    script_syntax *after = text ? script_parse(text) : NULL;
    char *metadata = authoring_source_presentation_metadata(presentation);
    char *remapped = after && metadata
                       ? authoring_source_metadata_remap(metadata, before, after)
                       : NULL;
    free(metadata);
    script_syntax_free(before);
    script_syntax_free(after);
    if (!text || !remapped) {
        free(text);
        free(remapped);
        snprintf(scene->message, sizeof scene->message, "Could not format source: %s", error);
        return -1;
    }
    const authoring_source *old = old_path
                                    ? preview_source_find(scene->live, old_path, false)
                                    : NULL;
    char *identity = old ? xstrdup(old->analysis->identity) : NULL;
    bool moved = old_path && strcmp(path, old_path);
    source_catalog_edit edits[] = {
        { .path = path, .text = text, .identity = identity },
        { .path = old_path },
    };
    preview_source_staging *stages[2] = { 0 };
    int result = scene_stage_source_edits(scene, edits, moved ? 2 : 1, stages, NULL, error, sizeof error);
    bool staged = !result;
    if (!result) {
        size_t size = strlen(path) + strlen(".meta") + 1;
        char *sidecar = xmalloc(size);
        snprintf(sidecar, size, "%s.meta", path);
        result = write_file_atomic(path, text);
        if (!result) {
            scene_disk_note(scene, path);
            result = write_file_atomic(sidecar, remapped);
            if (!result) {
                scene_disk_note(scene, sidecar);
            }
        }
        free(sidecar);
        if (result) {
            snprintf(error, sizeof error, "Could not write source and presentation metadata");
        }
    }
    if (staged) {
        scene_finish_source_edits(scene, stages, !result);
    }
    if (!result) {
        const authoring_source *source = preview_source_find(scene->live, path, false);
        const behavior_asset *current = editor_behavior_asset(scene->graph_editor);
        authoring_source_presentation *formatted = authoring_source_presentation_new(
            source->analysis,
            authoring_sources_catalog(preview_sources_working(scene->live)),
            NULL
        );
        if (formatted) {
            bool restored = authoring_source_presentation_restore(formatted, remapped);
            assert(restored);
            (void)restored;
            if (editor_presentation(scene->graph_editor) && current &&
                !strcmp(current->name, source->analysis->name)) {
                result = editor_source_presentation(scene->graph_editor, formatted);
                scene->editor_working = editor_behavior_asset(scene->graph_editor);
                scene->editor_state_seen = false;
            }
            authoring_source_presentation_free(formatted);
        }
    }
    if (result) {
        snprintf(scene->message, sizeof scene->message, "Could not save %s: %s", path, error);
    }
    free(text);
    free(remapped);
    free(identity);
    return result;
}

static int sync_open_behavior(app_scene_authoring *scene)
{
    if (!scene->definition || !scene->editor_working || !scene->graph_editor) {
        return 0;
    }
    const behavior_asset *current = editor_behavior_asset(scene->graph_editor);
    scene->editor_working = current;
    if (!current || !current->name || !scene->definition->name ||
        strcmp(current->name, scene->definition->name) != 0 ||
        scene->editor_working == scene->definition->working) {
        return 0;
    }
    const authoring_source_presentation *presentation = editor_presentation(
        scene->graph_editor
    );
    int synced = presentation
                   ? preview_projection_set(scene->definition, (authoring_source_presentation *)presentation, false)
                   : preview_sync_working(scene->definition, scene->editor_working);
    if (synced != 0) {
        return -1;
    }
    refresh_asset_catalog(scene);
    return 0;
}

static bool holds_event(const graph *gr)
{
    for (int i = 0; i < acount(gr->nodes); i++) {
        const node *n = gr->nodes[i];
        if (n->desc->display_class == NODE_DISPLAY_EVENT && n->reference_name &&
            n->reference_name[0]) {
            return true;
        }
    }
    return false;
}

/* A behavior opens on the graph it was left on, else on its default graph
   when that holds an event, else on its first handler: Blueprint's event
   graph is where the events are. */
static graph *start_graph(app_scene_authoring *scene, const behavior_asset *asset, const char *key)
{
    uint64_t left;
    graph *shown = editor_document_graph(scene->graph_editor, key, &left)
                     ? behavior_asset_find_graph(asset, left)
                     : NULL;
    if (shown) {
        return shown;
    }
    graph *main = behavior_asset_find_graph(asset, 0);
    if (!main || holds_event(main)) {
        return main;
    }
    for (int i = 0; i < acount(asset->graphs); i++) {
        if (asset->graphs[i]->kind == GRAPH_EVENT && holds_event(asset->graphs[i])) {
            return asset->graphs[i];
        }
    }
    return main;
}

int open_behavior_graph(app_scene_authoring *scene, const char *asset_name)
{
    preview_definition *target = preview_find_definition(scene->live, asset_name);
    if (!target || !scene->graph_editor || sync_open_behavior(scene) != 0) {
        return -1;
    }
    behavior_asset *working = target->working;
    graph *start = start_graph(scene, working, asset_name);
    int opened = !start ? -1
               : target->working_presentation ? editor_visit_source_presentation(
                                                    scene->graph_editor,
                                                    target->working_presentation,
                                                    start->graph_id,
                                                    asset_name
                                                )
                                              : editor_visit_behavior_asset(
                                                    scene->graph_editor,
                                                    working,
                                                    start->graph_id,
                                                    asset_name
                                                );
    if (opened != 0) {
        return -1;
    }
    scene->definition = target;
    scene->editor_working = editor_behavior_asset(scene->graph_editor);
    return 0;
}

int open_behavior_asset(app_scene_authoring *scene, const char *asset_name)
{
    bool was_transitioning = scene->tabs_transitioning;
    if (!was_transitioning) {
        scene_tabs_sync_current(scene);
    }
    scene->tabs_transitioning = true;
    int result = open_behavior_graph(scene, asset_name);
    if (result != 0) {
        scene->tabs_transitioning = was_transitioning;
        return result;
    }
    set_behavior_component_context(scene, asset_name);
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, asset_name);
    scene->tabs_transitioning = was_transitioning;
    snprintf(
        scene->message,
        sizeof scene->message,
        "Editing %s asset %s.",
        behavior_domain_name(scene->definition->working->domain),
        asset_name
    );
    scene_notice(scene);
    return 0;
}

int app_scene_authoring_open_behavior(app_scene_authoring *scene, const char *asset_name)
{
    if (!scene || !asset_name) {
        return -1;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "tab.open_behavior", &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "behavior", asset_name);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = open_behavior_asset(scene, asset_name);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

static int content_behavior_index(const app_scene_authoring *scene, const char *path)
{
    const char *name = content_basename(path);
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        const preview_definition *definition = preview_definition_at(scene->live, i);
        const char *asset_name = definition->name;
        char filename[256];
        snprintf(
            filename,
            sizeof filename,
            "%s%s",
            asset_name,
            behavior_asset_extension(definition->working->domain)
        );
        if (strcmp(name, asset_name) == 0 || strcmp(name, filename) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *behavior_path(const app_scene_authoring *scene, const char *asset_name)
{
    for (int i = 0; i < acount(scene->content); i++) {
        const scene_content_entry *entry = &scene->content[i];
        if (entry->directory || content_behavior_index(scene, entry->path) < 0) {
            continue;
        }
        int index = content_behavior_index(scene, entry->path);
        if (strcmp(preview_definition_at(scene->live, index)->name, asset_name) == 0) {
            return entry->path;
        }
    }
    return NULL;
}

static int save_behavior_definition(
    app_scene_authoring *scene,
    preview_definition *definition,
    const char *requested_path
)
{
    const char *path = requested_path ? requested_path
                                      : preview_definition_path(definition);
    if (!path) {
        path = behavior_path(scene, definition->name);
    }
    if (!path) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Behavior %s has no source path.",
            definition->name
        );
        return -1;
    }
    if (scene_tabs_path_in_use(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, path)) {
        snprintf(scene->message, sizeof scene->message, "Behavior path is already open.");
        return -1;
    }
    if (definition->working_presentation) {
        char *saved_path = xstrdup(path);
        int result = save_source_view(
            scene,
            definition->working_presentation,
            saved_path,
            preview_definition_path(definition)
        );
        if (!result) {
            preview_definition_set_path(definition, saved_path);
            definition->unsaved = false;
            scene_tabs_mark_document_saved(
                scene,
                APP_EDITOR_DOCUMENT_BEHAVIOR,
                definition->name
            );
        }
        free(saved_path);
        return result;
    }
    char *json = behavior_asset_save(definition->working);
    if (!json) {
        return -1;
    }
    int result = save_content_file(scene, path, json);
    if (result == 0) {
        preview_definition_set_path(definition, path);
        definition->unsaved = false;
        scene_tabs_mark_document_saved(
            scene,
            APP_EDITOR_DOCUMENT_BEHAVIOR,
            definition->name
        );
    }
    free(json);
    return result;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, DOCUMENT_DIRECTORY_MODE) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static char *document_default_path(app_scene_authoring *scene)
{
    const char *folder = NULL;
    const char *name = NULL;
    const char *extension = DECLARATIVE_ASSET_EXTENSION;
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        scene_prototype_asset *asset = selected_prototype(scene);
        folder = "prototypes";
        name = asset && asset->prototype ? asset->prototype->name : NULL;
    } else if (scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        const char *folders[] = { "behaviors", "libraries", "materials",
                                  "shapes",    "events",    "shader-libraries",
                                  "fields",    "models" };
        folder = scene->definition ? folders[scene->definition->working->domain]
                                   : "behaviors";
        name = scene->definition ? scene->definition->name : NULL;
        if (scene->definition) {
            extension = behavior_asset_extension(scene->definition->working->domain);
        }
    }
    if (!name || !name[0] || ensure_directory(scene->content_root) != 0) {
        return NULL;
    }
    char *directory = folder ? content_path_join(scene->content_root, folder)
                             : xstrdup(scene->content_root);
    if (folder && ensure_directory(directory) != 0) {
        free(directory);
        return NULL;
    }
    size_t filename_size = strlen(name) + strlen(extension) + 1;
    char *filename = (char *)xmalloc(filename_size);
    snprintf(filename, filename_size, "%s%s", name, extension);
    char *path = content_path_join(directory, filename);
    free(filename);
    free(directory);
    return path;
}

int reload_behavior_from_disk(app_scene_authoring *scene, const char *asset_name)
{
    preview_definition *definition = asset_name
                                       ? preview_find_definition(scene->live, asset_name)
                                       : scene->definition;
    if (!definition) {
        snprintf(scene->message, sizeof scene->message, "No behavior document is active.");
        return -1;
    }
    const char *path = preview_definition_path(definition);
    if (!path) {
        path = behavior_path(scene, definition->name);
    }
    if (!path) {
        snprintf(scene->message, sizeof scene->message, "Behavior requires Save As before Reload.");
        return -1;
    }
    char *json = read_scene_file(path);
    char error[256] = "";
    if (definition->working_presentation) {
        authoring_source_presentation *
            presentation = scene_source_projection_path(scene, json, path, error, sizeof error);
        behavior_asset *view = presentation
                                 ? authoring_source_presentation_view(presentation)
                                 : NULL;
        if (!view || !view->graph || strcmp(view->name, definition->name)) {
            authoring_source_presentation_free(presentation);
            free(json);
            snprintf(scene->message, sizeof scene->message, "Could not reload %s: %s", path, error);
            return -1;
        }
        char *name = xstrdup(definition->name);
        bool active_document = scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR &&
                               scene->document.id &&
                               !strcmp(scene->document.id, name);
        const graph *active = active_document
                                ? editor_active_graph(scene->graph_editor)
                                : NULL;
        uint64_t graph_id = active && behavior_asset_find_graph(view, active->graph_id)
                              ? active->graph_id
                              : view->graph->graph_id;
        char *source_path = xstrdup(path);
        source_catalog_edit edit = { .path = source_path, .text = json };
        int applied = scene_publish_source_edits(scene, &edit, 1, error, sizeof error);
        int opened = active_document
                       ? editor_reload_source_presentation(scene->graph_editor, presentation, graph_id, name)
                       : editor_reload_source_presentation_document(scene->graph_editor, presentation, name);
        authoring_source_presentation_free(presentation);
        free(json);
        if (!opened) {
            definition = preview_find_definition(scene->live, name);
            definition->unsaved = false;
            scene_tabs_mark_document_saved(scene, APP_EDITOR_DOCUMENT_BEHAVIOR, name);
            if (active_document) {
                scene->definition = definition;
                scene->editor_working = editor_behavior_asset(scene->graph_editor);
                scene->editor_state_seen = false;
                scene->behavior_apply_pending = false;
            }
        }
        snprintf(
            scene->message,
            sizeof scene->message,
            applied ? "Reloaded %s; apply blocked: %s" : "Reloaded %s.",
            source_path,
            error
        );
        if (!applied) {
            scene_notice(scene);
        }
        free(source_path);
        free(name);
        return opened;
    }
    behavior_asset *loaded = scene_graph_source_path(scene, json, path, error, sizeof error);
    free(json);
    if (!loaded) {
        snprintf(scene->message, sizeof scene->message, "Could not reload %s: %s", path, error);
        return -1;
    }
    if (!loaded->name || strcmp(loaded->name, definition->name) != 0) {
        behavior_asset_free(loaded);
        snprintf(
            scene->message,
            sizeof scene->message,
            "Reloaded behavior name does not match %s.",
            definition->name
        );
        return -1;
    }
    const bool active_document = scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR &&
                                 scene->document.id &&
                                 !strcmp(scene->document.id, definition->name);
    const graph *active = active_document ? editor_active_graph(scene->graph_editor)
                                          : NULL;
    uint64_t graph_id = active ? active->graph_id : loaded->graph->graph_id;
    if (!behavior_asset_find_graph(loaded, graph_id)) {
        graph_id = loaded->graph->graph_id;
    }
    int opened = active_document ? editor_reload_behavior_asset(
                                       scene->graph_editor,
                                       loaded,
                                       graph_id,
                                       definition->name
                                   )
                                 : editor_reload_behavior_asset_document(
                                       scene->graph_editor,
                                       loaded,
                                       definition->name
                                   );
    if (opened != 0) {
        behavior_asset_free(loaded);
        snprintf(scene->message, sizeof scene->message, "Could not open reloaded %s.", path);
        return -1;
    }
    if (!preview_definition_path(definition)) {
        preview_definition_set_path(definition, path);
    }
    behavior_asset *previous = definition->working;
    definition->working = loaded;
    definition->dirty = 1;
    char apply_message[256];
    int applied = preview_update(scene->live, definition, apply_message, sizeof apply_message);
    definition->unsaved = false;
    scene->preview_retry = true;
    refresh_asset_catalog(scene);
    behavior_asset_free(previous);
    scene_tabs_mark_document_saved(
        scene,
        APP_EDITOR_DOCUMENT_BEHAVIOR,
        definition->name
    );
    if (active_document) {
        scene->definition = definition;
        scene->editor_working = editor_behavior_asset(scene->graph_editor);
        scene->editor_revision_seen = editor_edit_revision(scene->graph_editor);
        scene->editor_behavior_asset_seen = scene->editor_working;
        scene->editor_definition_seen = definition;
        scene->editor_state_seen = true;
        scene->behavior_apply_pending = false;
    }
    if (applied != 0) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Reloaded %s; apply blocked, previous runtime remains active.",
            path
        );
    } else {
        snprintf(scene->message, sizeof scene->message, "Reloaded %s.", path);
        scene_notice(scene);
    }
    return 0;
}

static int reload_behavior_document(app_scene_authoring *scene)
{
    return reload_behavior_from_disk(scene, NULL);
}

int reload_prototype_asset(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    char *json = read_scene_file(asset->path);
    char error[256];
    prototype *loaded = json ? prototype_load(json, &scene->registry, error, sizeof error)
                             : NULL;
    free(json);
    if (!loaded) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Could not reload %s: %s",
            asset->path,
            json ? error : "unreadable"
        );
        return -1;
    }
    if (!loaded->name || strcmp(loaded->name, asset->prototype->name) != 0) {
        prototype_free(loaded);
        snprintf(
            scene->message,
            sizeof scene->message,
            "Reloaded prototype name does not match %s.",
            asset->prototype->name
        );
        return -1;
    }
    prototype *previous = asset->prototype;
    asset->prototype = loaded;
    if (refresh_prototype_instances(scene, asset) != 0) {
        asset->prototype = previous;
        prototype_free(loaded);
        snprintf(
            scene->message,
            sizeof scene->message,
            "Could not apply reloaded %s.",
            asset->path
        );
        return -1;
    }
    prototype_free(previous);
    asset->dirty = 0;
    clear_prototype_history(asset);
    snprintf(scene->message, sizeof scene->message, "Reloaded %s.", asset->path);
    scene_notice(scene);
    return 0;
}

static int reload_prototype_document(app_scene_authoring *scene)
{
    scene_prototype_asset *asset = selected_prototype(scene);
    if (!asset || !asset->path) {
        snprintf(scene->message, sizeof scene->message, "Prototype requires Save As before Reload.");
        return -1;
    }
    return reload_prototype_asset(scene, asset);
}

int reload_ui_document(app_scene_authoring *scene, scene_document_tab *tab)
{
    char error[256];
    if (!ui_buffer_reload(tab->ui, error, sizeof error)) {
        snprintf(scene->message, sizeof scene->message, "Could not reload %s: %s", tab->label, error);
        return -1;
    }
    snprintf(scene->message, sizeof scene->message, "Reloaded %s.", tab->label);
    scene_notice(scene);
    return 0;
}

static int reload_active_document(app_scene_authoring *scene)
{
    if (scene->document.kind == APP_EDITOR_DOCUMENT_UI) {
        return reload_ui_document(scene, scene_tabs_ui(scene, scene->document.id));
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        return scene->system_path ? load_system_asset(scene, scene->system_path, true)
                                  : -1;
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        return reload_prototype_document(scene);
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        return reload_behavior_document(scene);
    }
    if (!scene->scene_path) {
        snprintf(scene->message, sizeof scene->message, "Scene requires Save As before Reload.");
        return -1;
    }
    return app_scene_authoring_load(scene, scene->scene_path);
}

int load_system_document_text(app_scene_authoring *scene, const char *text, const char *path, bool reload)
{
    char error[256] = "";
    if (source_catalog_source_path(scene->content_root, path)) {
        authoring_source_presentation *
            presentation = scene_source_projection_path(scene, text, path, error, sizeof error);
        behavior_asset *view = presentation
                                 ? authoring_source_presentation_view(presentation)
                                 : NULL;
        if (!view || !view->graph) {
            authoring_source_presentation_free(presentation);
            snprintf(scene->message, sizeof scene->message, "Could not open System: %s", error);
            return -1;
        }
        if (!scene->tabs_transitioning) {
            scene_tabs_sync_current(scene);
        }
        int opened = reload ? editor_reload_source_presentation(
                                  scene->graph_editor,
                                  presentation,
                                  view->graph->graph_id,
                                  path
                              )
                            : editor_visit_source_presentation(
                                  scene->graph_editor,
                                  presentation,
                                  view->graph->graph_id,
                                  path
                              );
        char *name = xstrdup(view->name);
        authoring_source_presentation_free(presentation);
        if (opened != 0) {
            free(name);
            return -1;
        }
        bool was_transitioning = scene->tabs_transitioning;
        scene->tabs_transitioning = true;
        char *saved_path = xstrdup(path);
        free(scene->system_path);
        scene->system_path = saved_path;
        scene->editor_working = editor_behavior_asset(scene->graph_editor);
        scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SYSTEM, name);
        scene_tabs_sync_current(scene);
        scene->tabs_transitioning = was_transitioning;
        free(name);
        return 0;
    }
    snprintf(scene->message, sizeof scene->message, "Systems require canonical source.");
    return -1;
}

int load_system_asset(app_scene_authoring *scene, const char *path, bool reload)
{
    char *json = read_scene_file(path);
    if (!json) {
        return -1;
    }
    if (reload && source_catalog_source_path(scene->content_root, path)) {
        source_catalog_edit edit = { .path = path, .text = json };
        char error[256] = "";
        if (scene_publish_source_edits(scene, &edit, 1, error, sizeof error)) {
            snprintf(scene->message, sizeof scene->message, "Could not apply System: %s", error);
            free(json);
            return -1;
        }
    }
    int result = load_system_document_text(scene, json, path, reload);
    free(json);
    return result;
}

int open_system_asset(app_scene_authoring *scene, const char *path)
{
    return load_system_asset(scene, path, false);
}

int app_scene_authoring_open_system(app_scene_authoring *scene, const char *path)
{
    if (!scene || !path) {
        return -1;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "tab.open_system", &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "path", path);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return gate == COLLAB_ACTION_PENDING ? 1 : -1;
    }
    int result = open_system_asset(scene, path);
    scene_action_finish(scene, action, result == 0);
    free(action);
    return result;
}

static int save_system_document(app_scene_authoring *scene, const char *path)
{
    if (scene_tabs_path_in_use(scene, APP_EDITOR_DOCUMENT_SYSTEM, path)) {
        snprintf(scene->message, sizeof scene->message, "System path is already open.");
        return -1;
    }
    const authoring_source_presentation *presentation = editor_presentation(
        scene->graph_editor
    );
    if (presentation) {
        int result = save_source_view(scene, presentation, path, scene->system_path);
        if (!result) {
            char *saved_path = xstrdup(path);
            free(scene->system_path);
            scene->system_path = saved_path;
            scene_tabs_sync_current(scene);
            scene_tabs_mark_document_saved(
                scene,
                APP_EDITOR_DOCUMENT_SYSTEM,
                scene->document.id
            );
        }
        return result;
    }
    snprintf(scene->message, sizeof scene->message, "Systems require canonical source.");
    return -1;
}

static int save_as_active_document(app_scene_authoring *scene)
{
    char *path = document_default_path(scene);
    if (!path) {
        snprintf(scene->message, sizeof scene->message, "Could not choose a Save As path.");
        return -1;
    }
    int result = -1;
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        scene_prototype_asset *asset = selected_prototype(scene);
        result = asset
                   ? app_scene_authoring_save_prototype(scene, asset->prototype->name, path)
                   : -1;
    } else if (scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        preview_definition *definition = scene->definition;
        int synced = definition ? sync_behavior_working(scene) : -1;
        int saved = synced == 0 ? save_behavior_definition(scene, definition, path)
                                : -1;
        int applied = saved == 0 && !definition->working_presentation
                        ? preview_update(
                              scene->live,
                              definition,
                              scene->preview_message,
                              sizeof scene->preview_message
                          )
                        : saved;
        result = saved;
        if (saved == 0 && applied != 0) {
            snprintf(
                scene->message,
                sizeof scene->message,
                "Saved %s; apply blocked, previous runtime remains active.",
                definition->name
            );
        } else if (saved == 0) {
            snprintf(
                scene->message,
                sizeof scene->message,
                "Saved %s.",
                definition->name
            );
            scene_notice(scene);
        }
    }
    free(path);
    return result;
}

/* Saves the behavior open in the graph editor, whichever document is active. */
int save_behavior_document(app_scene_authoring *scene)
{
    if (!scene->definition) {
        return -1;
    }
    if (!preview_definition_path(scene->definition) &&
        !behavior_path(scene, scene->definition->name)) {
        if (scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
            return save_as_active_document(scene);
        }
        snprintf(scene->message, sizeof scene->message, "Behavior requires Save As.");
        return -1;
    }
    int synced = sync_behavior_working(scene);
    int saved = synced == 0 ? save_behavior_definition(scene, scene->definition, NULL)
                            : -1;
    int applied = saved == 0 && !scene->definition->working_presentation
                    ? preview_update(
                          scene->live,
                          scene->definition,
                          scene->preview_message,
                          sizeof scene->preview_message
                      )
                    : saved;
    if (saved == 0 && applied != 0) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Saved %s; apply blocked, previous runtime remains active.",
            scene->definition->name
        );
    } else if (saved == 0) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Saved %s.",
            scene->definition->name
        );
        scene_notice(scene);
    }
    return saved;
}

int save_ui_document(app_scene_authoring *scene, scene_document_tab *tab)
{
    ui_buffer *b = tab->ui;
    int result = 0;
    ui_buffer_format_for_save(b);
    for (int p = 0; p < UI_PART_COUNT && !result; p++) {
        if (ui_buffer_saves(b, (ui_part)p)) {
            result = write_file_atomic(b->files[p], b->parts[p]);
        }
    }
    if (result == 0) {
        ui_buffer_saved(b);
    } else {
        snprintf(scene->message, sizeof scene->message, "Could not write %s.", tab->label);
    }
    return result;
}

/* A UI document saves over its file only while the file is the one it
 * loaded or last saved. */
static int save_active_ui_document(app_scene_authoring *scene)
{
    scene_document_tab *tab = scene_tabs_ui(scene, scene->document.id);
    if (!ui_buffer_on_disk(tab->ui)) {
        snprintf(scene->message, sizeof scene->message, "%s changed on disk.", tab->label);
        return -1;
    }
    return save_ui_document(scene, tab);
}

int save_active_document(app_scene_authoring *scene)
{
    if (scene->document.kind == APP_EDITOR_DOCUMENT_UI) {
        return save_active_ui_document(scene);
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        if (!editor_presentation(scene->graph_editor) || !scene->system_path) {
            snprintf(scene->message, sizeof scene->message, "System requires Save As.");
            return -1;
        }
        return save_system_document(scene, scene->system_path);
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        scene_prototype_asset *asset = selected_prototype(scene);
        if (!asset || !asset->path) {
            return asset ? save_as_active_document(scene) : -1;
        }
        return app_scene_authoring_save_prototype(
            scene,
            asset->prototype->name,
            asset->path
        );
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        return save_behavior_document(scene);
    }
    if (!scene->scene_path) {
        snprintf(scene->message, sizeof scene->message, "Scene requires Save As.");
        return -1;
    }
    return app_scene_authoring_save(scene, scene->scene_path);
}

int app_scene_authoring_reload(app_scene_authoring *scene)
{
    return scene ? reload_active_document(scene) : -1;
}
