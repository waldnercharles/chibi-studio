#include "editor/workspace/viewport.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/asset_index.h"
#include "editor/workspace/model_preview.h"
#include "support/array.h"
#include "support/text_file.h"
#include "support/xalloc.h"
#include "ui/popover.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* Present only when the browser keeps projects itself (no local server). */
EM_JS(bool, web_projects_available, (), {
    return !!Module['chibiExportProject'];
})
EM_JS(void, web_export_project, (), { Module['chibiExportProject'](); })
EM_JS(void, web_import_project, (), { Module['chibiImportProject'](); })
#endif

static const char *const EDITOR_LABELS[STUDIO_EDITOR_COUNT] = {
    "Scene",  "Behavior",    "Outliner", "Properties",  "Output",
    "Assets", "Performance", "Members",  "Text Editor", "Panel"
};
static const uint32_t EDITOR_ICONS[STUDIO_EDITOR_COUNT] = {
    KIT_ICON_VIEWPORT,    KIT_ICON_GRAPH,    KIT_ICON_OUTLINER,
    KIT_ICON_PROPERTIES,  KIT_ICON_OUTPUT,   KIT_ICON_ASSETS,
    KIT_ICON_PERFORMANCE, KIT_ICON_VARIABLE, KIT_ICON_EXPRESSION,
    KIT_ICON_LAYOUT,
};

static void report(studio *s, kit_toast_type kind, const char *message)
{
    snprintf(s->status, sizeof s->status, "%s", message);
    s->status_warning = kind == KIT_TOAST_WARNING;
    s->status_time = s->time;
    studio_record(s, kind, message);
}

void studio_report(studio *s, const char *message)
{
    report(s, KIT_TOAST_INFO, message);
}

void studio_warn(studio *s, const char *message)
{
    report(s, KIT_TOAST_WARNING, message);
}

/* Reports why an edit failed: the scene's reason, which follow_messages then
   need not repeat, else `fallback`. */
static void warn_failure(studio *s, const char *fallback)
{
    snprintf(s->seen_message, sizeof s->seen_message, "%s", s->scene->message);
    studio_warn(s, s->seen_message[0] ? s->seen_message : fallback);
}

const char *studio_scene_title(const app_scene_authoring *scene, char *out, size_t size)
{
    const char *path = scene->scene_path;
    const char *base = path && path[0] ? content_basename(path) : NULL;
    snprintf(out, size, "%s", base ? base : "Scene");
    /* The document name without its storage format (.chibi). */
    char *extension = strrchr(out, '.');
    if (extension && extension != out) {
        *extension = '\0';
    }
    return out;
}

/* Kit icons by component_icon. */
static const uint32_t PROTOTYPE_ICONS[COMPONENT_ICON_COUNT] = {
    KIT_ICON_CAMERA,     KIT_ICON_SYSTEM,  KIT_ICON_LIGHT,   KIT_ICON_EMITTER,
    KIT_ICON_WATER,      KIT_ICON_TERRAIN, KIT_ICON_GAMEPAD, KIT_ICON_KEYBOARD,
    KIT_ICON_PROJECTILE, KIT_ICON_HEALTH,  KIT_ICON_DIAMOND, KIT_ICON_SHAPE,
};

/* A prototype's icon follows from what it is made of, never from its name: of
   the icons its components name in editor/Components.kdl, the first in
   component_icon order decides. */
uint32_t studio_prototype_icon(const studio *s, const char *prototype)
{
    scene_prototype_asset *asset = prototype ? find_prototype(s->scene, prototype)
                                             : NULL;
    if (!asset || !asset->prototype) {
        return KIT_ICON_PROTOTYPE;
    }
    const game_prototype *p = asset->prototype;
    component_icon first = COMPONENT_ICON_COUNT;
    for (int c = 0; c < acount(p->components); c++) {
        component_icon icon = component_presentation_icon(
            &s->presentation.hints,
            p->components[c].desc
        );
        if (icon != COMPONENT_ICON_NONE && icon < first) {
            first = icon;
        }
    }
    return first < COMPONENT_ICON_COUNT ? PROTOTYPE_ICONS[first] : KIT_ICON_PROTOTYPE;
}

/* ---------- Commands ---------- */

/* The graph canvas owns history and saving while it has focus (Blender: the
   area under the pointer or last clicked), and so do the editors of its
   members and nodes. */
static bool graph_focused(const studio *s)
{
    int area = studio_command_area(s);
    if (!studio_graph_active(s) || area < 0) {
        return false;
    }
    studio_editor editor = s->prefs.layouts[s->prefs.workspace].editors[area];
    return area == s->graph.area || editor == STUDIO_EDITOR_MEMBERS ||
           (editor == STUDIO_EDITOR_PROPERTIES &&
            (s->tab == STUDIO_TAB_NODE || s->tab == STUDIO_TAB_MEMBER));
}

static bool behavior_history_focused(const studio *s)
{
    if (graph_focused(s)) {
        return true;
    }
    int area = studio_command_area(s);
    return area >= 0 && s->prefs.layouts[s->prefs.workspace].editors[area] ==
                            STUDIO_EDITOR_CODE;
}

static void toast(studio *s, kit_toast_type type, const char *title, const char *description)
{
    char text[256];
    snprintf(text, sizeof text, description ? "%s: %s" : "%s", title, description);
    studio_record(s, type, text);
    /* Save All reports once for every document it saved. */
    if (type == KIT_TOAST_SUCCESS && s->saving_all) {
        return;
    }
    kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = type,
                           .title = title,
                           .description = description,
                           .duration = type == KIT_TOAST_ERROR ? -1 : 0 }
    );
}

void studio_saved(studio *s, const char *title, const char *description)
{
    toast(s, KIT_TOAST_SUCCESS, title, description);
}

void studio_error(studio *s, const char *title, const char *message)
{
    toast(s, KIT_TOAST_ERROR, title, message);
}

/* Saves the active document, or the behavior on the graph canvas while
   `graph` says the canvas owns Save. */
static void save_document(studio *s, bool graph)
{
    app_scene_authoring *scene = s->scene;
    if (scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        studio_code_save(s);
        return;
    }
    if (scene->active_tab >= 0 && scene->active_tab < acount(scene->tabs) &&
        scene->tabs[scene->active_tab].source_dirty) {
        studio_code_save(s);
        return;
    }
    /* Text saves while the scene runs, as it applies live. */
    /* Selecting a document tab changes the active document but does not
       necessarily move focus away from the last editor area. The selected UI
       document owns Save in that state; graph focus owns Save only while a
       different document is active. */
    if (studio_ui_buffer(s) &&
        (!graph || scene->document.kind == APP_EDITOR_DOCUMENT_UI)) {
        studio_code_save(s);
        return;
    }
    if (!studio_editable(s) || runtime_session(scene)) {
        toast(s, KIT_TOAST_WARNING, "Stop the scene to save", NULL);
        return;
    }
    finish_transform_edit(scene, false);
    s->problems_checked = false;
    const char *failure = scene->message[0] ? scene->message : NULL;
    if (graph && scene->graph_document_kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        char title[160];
        snprintf(title, sizeof title, "Saved %s", scene->graph_document_id);
        if (save_behavior_document(scene) == 0) {
            toast(s, KIT_TOAST_SUCCESS, title, NULL);
        } else {
            toast(
                s,
                KIT_TOAST_ERROR,
                "Could not save",
                scene->message[0] ? scene->message : failure
            );
        }
        return;
    }
    if (scene->document.kind != APP_EDITOR_DOCUMENT_SCENE) {
        const char *label = scene->active_tab >= 0 &&
                                    scene->active_tab < acount(scene->tabs)
                              ? scene->tabs[scene->active_tab].label
                              : NULL;
        char title[160];
        snprintf(title, sizeof title, "Saved %s", label ? label : "document");
        if (save_active_document(scene) == 0) {
            toast(s, KIT_TOAST_SUCCESS, title, NULL);
        } else {
            toast(
                s,
                KIT_TOAST_ERROR,
                "Could not save",
                scene->message[0] ? scene->message : failure
            );
        }
        return;
    }
    if (!scene->scene_path || !scene->scene_path[0]) {
        toast(s, KIT_TOAST_WARNING, "Scene has no file", NULL);
        return;
    }
    char name[128];
    char title[160];
    if (app_scene_authoring_save(scene, scene->scene_path) == 0) {
        snprintf(title, sizeof title, "Saved %s", studio_scene_title(scene, name, sizeof name));
        toast(s, KIT_TOAST_SUCCESS, title, content_basename(scene->scene_path));
    } else {
        toast(s, KIT_TOAST_ERROR, "Could not save", scene->message[0] ? scene->message : NULL);
    }
}

void studio_save(studio *s)
{
    save_document(s, graph_focused(s));
}

bool studio_save_tab(studio *s, int index)
{
    app_scene_authoring *scene = s->scene;
    finish_transform_edit(scene, false);
    if (index != scene->active_tab && scene_tabs_activate(scene, index) != 0) {
        return false;
    }
    save_document(s, false);
    return !scene_tabs_dirty(scene, &scene->tabs[index]);
}

int studio_tab_index(const studio *s, const char *key)
{
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        if (!strcmp(s->scene->tabs[i].key, key)) {
            return i;
        }
    }
    return -1;
}

/* Saves each document with unsaved edits as Save would with it active, then
   returns to the tab that was active, and reports once for all of them. */
bool studio_save_all(studio *s)
{
    app_scene_authoring *scene = s->scene;
    if (!studio_editable(s) || runtime_session(scene)) {
        toast(s, KIT_TOAST_WARNING, "Stop the scene to save", NULL);
        return false;
    }
    char *active = scene->active_tab >= 0
                     ? xstrdup(scene->tabs[scene->active_tab].key)
                     : NULL;
    int saved = 0, unsaved = 0;
    char title[192] = "";
    s->saving_all = true;
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (!scene_tabs_dirty(scene, &scene->tabs[i])) {
            continue;
        }
        if (!studio_save_tab(s, i)) {
            unsaved++;
            continue;
        }
        snprintf(title, sizeof title, "Saved %s", scene->tabs[i].label);
        saved++;
    }
    s->saving_all = false;
    int back = active ? studio_tab_index(s, active) : -1;
    if (back >= 0 && back != scene->active_tab) {
        finish_transform_edit(scene, false);
        scene_tabs_activate(scene, back);
    }
    free(active);
    if (saved > 1 && !unsaved) {
        snprintf(title, sizeof title, "Saved %d documents", saved);
    }
    if (saved && !unsaved) {
        toast(s, KIT_TOAST_SUCCESS, title, NULL);
    } else if (saved) {
        snprintf(title, sizeof title, "Saved %d of %d documents", saved, saved + unsaved);
        toast(s, KIT_TOAST_WARNING, title, NULL);
    }
    return !studio_unsaved(s);
}

/* History and dirty state belong to the active document, not always the scene. */
bool studio_can_replay(studio *s, bool redo)
{
    app_scene_authoring *scene = s->scene;
    if ((scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
         scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM) &&
        behavior_history_focused(s)) {
        return studio_code_history(s, redo, false);
    }
    if (graph_focused(s)) {
        return studio_graph_can_replay(s, redo);
    }
    ui_buffer *text = studio_ui_buffer(s);
    if (text) {
        return redo ? ui_buffer_can_redo(text) : ui_buffer_can_undo(text);
    }
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        scene_prototype_asset *asset = selected_prototype(scene);
        return asset && acount(redo ? asset->redo : asset->undo) > 0;
    }
    return redo ? scene_history_can_redo(scene->history)
                : scene_history_can_undo(scene->history);
}

bool studio_active_dirty(studio *s)
{
    app_scene_authoring *scene = s->scene;
    const scene_document_tab *tab = scene->active_tab >= 0 &&
                                            scene->active_tab < acount(scene->tabs)
                                      ? &scene->tabs[scene->active_tab]
                                      : NULL;
    if (tab && tab->source_dirty) {
        return true;
    }
    if (studio_ui_buffer(s) &&
        (!graph_focused(s) || scene->document.kind == APP_EDITOR_DOCUMENT_UI)) {
        return ui_buffer_dirty(studio_ui_buffer(s));
    }
    if (graph_focused(s) && scene->graph_document_kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
        game_preview_definition
            *definition = game_preview_find_definition(scene->live, scene->graph_document_id);
        return definition && definition->unsaved;
    }
    return tab ? scene_tabs_dirty(scene, tab) : scene->scene_dirty;
}

bool studio_unsaved(studio *s)
{
    app_scene_authoring *scene = s->scene;
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene_tabs_dirty(scene, &scene->tabs[i])) {
            return true;
        }
    }
    return scene->scene_dirty;
}

/* The active scene or prototype document's history. */
static void document_history(studio *s, bool redo)
{
    finish_transform_edit(s->scene, false);
    int result = s->scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE
                   ? (redo ? prototype_redo(s->scene) : prototype_undo(s->scene))
                   : replay_scene_edit(s->scene, redo);
    if (result == 0) {
        studio_report(s, redo ? "Redo" : "Undo");
    }
}

void studio_history(studio *s, bool redo)
{
    if ((s->scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
         s->scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM) &&
        behavior_history_focused(s)) {
        if (studio_code_history(s, redo, true)) {
            studio_report(s, redo ? "Redo" : "Undo");
        }
        return;
    }
    ui_buffer *text = graph_focused(s) ? NULL : studio_ui_buffer(s);
    /* Text replays while the scene runs, as it applies live. */
    if (text && studio_can_replay(s, redo)) {
        if (redo) {
            ui_buffer_redo(text);
        } else {
            ui_buffer_undo(text);
        }
        studio_report(s, redo ? "Redo" : "Undo");
        return;
    }
    if (!studio_editable(s) || !studio_can_replay(s, redo)) {
        return;
    }
    if (graph_focused(s)) {
        studio_graph_history(s, redo);
        studio_report(s, redo ? "Redo" : "Undo");
        return;
    }
    document_history(s, redo);
}

bool studio_session(const studio *s)
{
    return runtime_session(s->scene) || s->guest != NULL;
}

/* The Studio project's game is Studio: Play there runs a guest Studio in
   the Scene area, and Play again or Stop ends it, as Unity's Play toggles. */
static bool runs_guest(const studio *s, app_runtime_state state)
{
    return s->own_project && state.target == APP_RUNTIME_SCENE &&
           !state.playing && !runtime_session(s->scene);
}

void studio_record_gameplay(studio *s)
{
    if (!app_scenario_recording(s->scene)) {
        if (app_scenario_record(s->scene) != 0) {
            toast(s, KIT_TOAST_ERROR, "Could not record gameplay", app_scene_authoring_message(s->scene));
        }
        return;
    }
    char name[128];
    if (app_scenario_save(s->scene, name, sizeof name) != 0) {
        toast(s, KIT_TOAST_ERROR, "Could not save the recording", NULL);
        return;
    }
    toast(s, KIT_TOAST_SUCCESS, "Saved gameplay recording", name);
}

void studio_replay_scenario(studio *s, const char *name)
{
    if (app_scenario_replay(s->scene, name) != 0) {
        toast(s, KIT_TOAST_ERROR, "Could not replay scenario", app_scene_authoring_message(s->scene));
    }
}

/* An edit the user did not make directly offers Undo in a toast while it is
   still the latest edit, as Delete does. */
static void offer_undo(studio *s, const char *title)
{
    kit_toast_dismiss(s->kit, s->undo_toast);
    s->undo_toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .title = title, .action = "Undo", .duration = 6 }
    );
    studio_record(s, KIT_TOAST_DEFAULT, title);
    s->undo_toast_group = scene_history_group_id(s->scene->history, false);
}

/* What Stop applies of Keep Changes, or "" for nothing. */
static void kept_title(studio *s, char *title, size_t size)
{
    int objects;
    const char *name;
    int values = scene_kept_values(s->scene, &objects, &name);
    title[0] = '\0';
    if (!values) {
        return;
    }
    char who[160];
    if (name) {
        snprintf(who, sizeof who, "%s", name);
    } else {
        snprintf(who, sizeof who, "%d objects", objects);
    }
    snprintf(title, size, "Applied %d kept %s to %s", values, values == 1 ? "value" : "values", who);
}

void studio_runtime(studio *s, app_runtime_command command)
{
    /* Play resumes a run paused at a breakpoint (Unreal). */
    if (command == APP_RUNTIME_PLAY && studio_debug(s, STUDIO_RESUME)) {
        return;
    }
    /* Stop keeps a recording and ends a replay. */
    if (command == APP_RUNTIME_STOP) {
        if (app_scenario_recording(s->scene)) {
            studio_record_gameplay(s);
        }
        app_scenario_stop(s->scene);
    }
    finish_transform_edit(s->scene, false);
    app_runtime_state state = app_scene_authoring_runtime_state(s->scene);
    if (s->guest && command != APP_RUNTIME_STEP) {
        studio_guest_stop(s);
        return;
    }
    if (command == APP_RUNTIME_PLAY && runs_guest(s, state)) {
        studio_guest_start(s);
        return;
    }
    if (command == APP_RUNTIME_PLAY && state.playing) {
        command = APP_RUNTIME_PAUSE;
    }
    char kept[320];
    kept_title(s, kept, sizeof kept);
    app_scene_authoring_runtime_command(s->scene, state.target, command);
    /* Stop applied Keep Changes as one Undo step. */
    if (kept[0] && !studio_live(s)) {
        offer_undo(s, kept);
    }
}

/* Fits every selected object, a spawned one included, in the view. */
void studio_frame_selection(studio *s)
{
    if (scene_model_preview_name(s->scene)) {
        scene_model_preview_frame(s->scene);
        return;
    }
    app_scene_authoring *scene = s->scene;
    ecs_entity *entities = NULL;
    if (scene->runtime_selected.slot && selected_scene_entity(scene).slot) {
        apush(entities, scene->runtime_selected);
    }
    for (int i = 0; i < acount(scene->actors); i++) {
        ecs_entity entity = scene->actors[i].entity;
        if (scene_instance_selected(scene, i) &&
            ecs_entity_is_alive(scene->world, entity)) {
            apush(entities, entity);
        }
    }
    if (acount(entities)) {
        fit_scene_view(s->viewport, scene->world, entities, acount(entities));
        engine_viewport_get_view(s->viewport, &scene->camera.view);
        scene->camera.saved = true;
        app_scene_authoring_framed(scene);
    }
    afree(entities);
}

/* What an operation on the selection did, for a report or a toast:
   "Copied Player" or "Copied 3 objects". */
static void selection_title(studio *s, const char *verb, int count, char *title, size_t size)
{
    scene_actor *actor = selected_actor(s->scene);
    if (count == 1 && actor) {
        snprintf(title, size, "%s %s", verb, actor->name);
        return;
    }
    snprintf(title, size, "%s %d objects", verb, count);
}

/* Duplicates land one grid increment down and right, as Godot's do, so the
   copies show beside their originals and stay on the grid with them; Unity's
   in place copies hide behind what they copy. */
void studio_duplicate(studio *s)
{
    app_scene_authoring *scene = s->scene;
    if (!studio_editable(s) || !selected_actor(scene)) {
        return;
    }
    float step = s->prefs.scene_increment;
    if (scene_duplicate_selection(scene, (GameTransform){ step, step }) != 0) {
        warn_failure(s, "Could not duplicate");
        return;
    }
    char title[160];
    selection_title(s, "Duplicated", scene_selection_count(scene), title, sizeof title);
    studio_report(s, title);
}

void studio_copy(studio *s)
{
    char *text = scene_copy_selection(s->scene);
    if (!text) {
        return;
    }
    kit_clipboard_set(text);
    free(text);
    char title[160];
    selection_title(s, "Copied", scene_selection_count(s->scene), title, sizeof title);
    studio_report(s, title);
}

/* The pointer's place in the scene while it is over the Scene and the key
   came from there, as gameplay coordinates. */
static bool scene_pointer(studio *s, GameTransform *at)
{
    int area = s->key_area;
    float x, y;
    if (area < 0 || studio_active_layout(s)->editors[area] != STUDIO_EDITOR_SCENE ||
        !s->surface.hovered || studio_previewed_ui(s) ||
        !engine_viewport_mouse_world(s->viewport, &x, &y)) {
        return false;
    }
    *at = (GameTransform){ x, -y };
    return true;
}

void studio_paste(studio *s)
{
    if (!studio_editable(s)) {
        return;
    }
    float step = s->prefs.scene_increment;
    GameTransform pointer;
    scene_paste_place place = { .offset = { step, step },
                                .snap = s->prefs.scene_snap ? step : 0 };
    if (scene_pointer(s, &pointer)) {
        place.center = &pointer;
    }
    char *text = cf_clipboard_get();
    int pasted = scene_paste(s->scene, text, place);
    if (text) {
        cf_free(text);
    }
    if (pasted < 0) {
        kit_toast(
            s->kit,
            0,
            &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                               .title = "Nothing to paste",
                               .description = s->scene->message }
        );
        /* The toast says why; the status bar need not repeat it. */
        s->scene->message[0] = '\0';
        return;
    }
    char title[160];
    selection_title(s, "Pasted", pasted, title, sizeof title);
    studio_report(s, title);
}

void studio_frame_all(studio *s)
{
    fit_scene_view(s->viewport, s->scene->world, NULL, 0);
    engine_viewport_get_view(s->viewport, &s->scene->camera.view);
    s->scene->camera.saved = true;
    app_scene_authoring_framed(s->scene);
}

void studio_select_all(studio *s)
{
    app_scene_authoring *scene = s->scene;
    finish_transform_edit(scene, false);
    for (int i = 0; i < acount(scene->actors); i++) {
        scene->actors[i].marked = true;
    }
    scene->runtime_selected = (ecs_entity){ 0 };
    scene->selected = acount(scene->actors) - 1;
}

void studio_deselect(studio *s)
{
    app_scene_authoring *scene = s->scene;
    finish_transform_edit(scene, false);
    for (int i = 0; i < acount(scene->actors); i++) {
        scene->actors[i].marked = false;
    }
    scene->runtime_selected = (ecs_entity){ 0 };
    scene->selected = -1;
}

void studio_find_field(studio *s, studio_editor editor)
{
    if (s->find_request == (int)editor + 1) {
        kit_focus(s->kit);
        s->find_request = 0;
    }
}

static int actor_of(const app_scene_authoring *scene, ecs_entity entity)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            return i;
        }
    }
    return -1;
}

/* Deletes the actors' entities from last to first; 0 when all went. */
static int delete_entities(app_scene_authoring *scene, const ecs_entity *entities)
{
    int result = 0;
    for (int i = acount(entities) - 1; i >= 0 && !result; i--) {
        scene->selected = actor_of(scene, entities[i]);
        result = app_scene_authoring_delete_instance(scene);
    }
    for (int i = 0; i < acount(scene->actors); i++) {
        scene->actors[i].marked = false;
    }
    return result;
}

/* Deletes the selection as one Undo step, which a toast offers to undo
   while it is still the latest edit, titled "<verb> Player" or "<verb> 3
   objects". */
static void delete_selection(studio *s, const char *verb)
{
    app_scene_authoring *scene = s->scene;
    if (!studio_editable(s) || !selected_actor(scene)) {
        return;
    }
    ecs_entity *entities = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i)) {
            apush(entities, scene->actors[i].entity);
        }
    }
    char title[160];
    selection_title(s, verb, acount(entities), title, sizeof title);
    finish_transform_edit(scene, false);
    scene_history_begin_group(scene->history);
    int result = delete_entities(scene, entities);
    scene_history_end_group(scene->history);
    afree(entities);
    if (result != 0) {
        toast(
            s,
            KIT_TOAST_ERROR,
            "Could not delete",
            s->scene->message[0] ? s->scene->message : NULL
        );
        return;
    }
    offer_undo(s, title);
}

void studio_delete(studio *s)
{
    delete_selection(s, "Deleted");
}

/* Cut copies and deletes, the deletion offering Undo as Delete does. */
void studio_cut(studio *s)
{
    char *text = scene_copy_selection(s->scene);
    if (!text) {
        return;
    }
    kit_clipboard_set(text);
    free(text);
    delete_selection(s, "Cut");
}

static void undo_toast(studio *s)
{
    if (!s->undo_toast) {
        return;
    }
    /* Every document but a prototype replays the scene's history. */
    bool current = s->scene->document.kind != APP_EDITOR_DOCUMENT_PROTOTYPE &&
                   scene_history_group_id(s->scene->history, false) == s->undo_toast_group;
    if (kit_toast_action(s->kit, s->undo_toast)) {
        s->undo_toast = 0;
        /* The deletion, even when a graph under the toast has focus. */
        if (current && studio_editable(s)) {
            document_history(s, false);
        }
        return;
    }
    if (!current) {
        kit_toast_dismiss(s->kit, s->undo_toast);
        s->undo_toast = 0;
    }
}

void studio_add_object(studio *s, const char *prototype)
{
    if (!studio_editable(s)) {
        return;
    }
    finish_transform_edit(s->scene, false);
    if (app_scene_authoring_add_instance(s->scene, prototype) != 0) {
        warn_failure(s, "Could not add object");
    }
}

/* ---------- Layout ---------- */

void studio_toggle_drawer(studio *s, studio_editor editor)
{
    studio_layout *layout = studio_active_layout(s);
    bool showing = layout->drawer_open && layout->editors[STUDIO_AREA_DRAWER] == editor;
    layout->editors[STUDIO_AREA_DRAWER] = editor;
    layout->drawer_open = !showing;
}

void studio_toggle_maximized(studio *s, int area)
{
    s->maximized = s->maximized >= 0 ? -1 : area;
}

/* A new arrangement starts unmaximized, with no area focused or hovered
   until the pointer finds one. */
static void forget_areas(studio *s)
{
    s->maximized = s->focus_area = s->hover_area = -1;
    memset(s->areas, 0, sizeof s->areas);
    s->main_rect = s->side_rect = (kit_rect){ 0 };
}

void studio_reset_layout(studio *s)
{
    *studio_active_layout(s) = studio_layout_default(s->prefs.workspace);
    forget_areas(s);
}

void studio_set_workspace(studio *s, studio_workspace workspace)
{
    assert(workspace >= 0 && workspace < STUDIO_WORKSPACE_COUNT);
    if (s->prefs.workspace == workspace) {
        return;
    }
    s->prefs.workspace = workspace;
    forget_areas(s);
}

void studio_toggle_side_pane(studio *s)
{
    studio_layout *layout = studio_active_layout(s);
    layout->side_open = !layout->side_open;
    s->maximized = -1;
}

/* The menus' UI Scale choices. */
static const float UI_SCALES[] = { 0.5f, 0.75f, 1, 1.25f, 1.5f, 2 };
enum
{
    UI_SCALE_COUNT = sizeof UI_SCALES / sizeof *UI_SCALES,
};

/* The settings table's UI Scale row, whose presets are its steps. */
static const studio_setting *ui_scale_setting()
{
    for (int i = 0; i < studio_setting_count; i++) {
        if (studio_settings[i].offset == offsetof(studio_prefs, ui_scale)) {
            return &studio_settings[i];
        }
    }
    assert(!"the settings table has UI Scale");
    return NULL;
}

void studio_zoom(studio *s, int step)
{
    const studio_setting *setting = ui_scale_setting();
    int count = setting->choice_count;
    float scale = s->prefs.ui_scale, next = step ? scale : setting->fallback;
    /* The last match is the nearest step: up from the top, down from the bottom. */
    for (int i = 0; i < count; i++) {
        float up = strtof(setting->choices[count - 1 - i].value, NULL),
              down = strtof(setting->choices[i].value, NULL);
        if (step > 0 && up > scale + 0.001f) {
            next = up;
        }
        if (step < 0 && down < scale - 0.001f) {
            next = down;
        }
    }
    s->prefs.ui_scale = next;
}

/* ---------- Document tabs ---------- */

static uint32_t tab_icon(const scene_document_tab *tab)
{
    switch (tab->kind) {
        case APP_EDITOR_DOCUMENT_SCENE: return KIT_ICON_SCENE;
        case APP_EDITOR_DOCUMENT_PROTOTYPE: return KIT_ICON_PROTOTYPE;
        case APP_EDITOR_DOCUMENT_BEHAVIOR: return KIT_ICON_GRAPH;
        case APP_EDITOR_DOCUMENT_SYSTEM: return KIT_ICON_SYSTEM;
        case APP_EDITOR_DOCUMENT_UI:
            return tab->ui->stylesheet ? KIT_ICON_FILE : KIT_ICON_LAYOUT;
    }
    return KIT_ICON_FILE;
}

bool studio_tab_closes(const studio *s, int tab)
{
    return scene_tabs_can_close(s->scene, tab);
}

/* The top bar's TopBar lists these from `editor.tabs`; a tab runs
   selectTab and its close button closeTab, with the tab's index. */
studio_tab_view *studio_document_tabs(studio *s)
{
    app_scene_authoring *scene = s->scene;
    studio_tab_view *tabs = NULL;
    for (int i = 0; i < acount(scene->tabs); i++) {
        const scene_document_tab *tab = &scene->tabs[i];
        studio_tab_view shown = {
            .label = tab->label ? tab->label : "Untitled",
            .path = tab->path ? tab->path : "",
            .icon = tab_icon(tab),
            .dirty = scene_tabs_dirty(scene, tab),
            .active = i == scene->active_tab,
            .closable = studio_tab_closes(s, i),
            .pinned = tab->pinned,
            .pinnable = true,
            .pin_label = tab->pinned ? "Unpin Tab" : "Pin Tab",
            .close_reason = tab->pinned             ? "Unpin to close"
                          : studio_tab_closes(s, i) ? "Close Tab"
                                                    : "Keep one scene open",
        };
        apush(tabs, shown);
    }
    return tabs;
}

/* ---------- Top bar ---------- */

/* The top bar is authored: projects/studio's TopBar binds `editor` and
   `commands` (studio_sources.c) for its document tabs, transport, gear and
   search, `menubar` for its menus' lists and `shell` for the play target
   and the playback speed; its buttons and menus run commands and those
   sources' actions. It is a document of its own whose place takes its
   height. Chrome controls retain focus while the host routes global
   shortcuts. */
static void topbar(studio *s)
{
    kit *k = s->kit;
    /* The gear's Settings page is an authored dialog. */
    studio_ui_modal(s, "Settings");
    kit_document_begin(k, "topbar", "bar-place", true);
    studio_ui_panel(s, "topbar", "TopBar");
    kit_document_end(k);
}

/* ---------- Menu items (workstream C3) ---------- */

/* An item MenuItem or Submenu shows: label, icon, shortcut, checked and
   disabled. */
static void item_add(yyjson_mut_doc *doc, yyjson_mut_val *item)
{
    yyjson_mut_obj_add_strcpy(doc, item, "label", "");
    yyjson_mut_obj_add_strcpy(doc, item, "icon", "");
    yyjson_mut_obj_add_strcpy(doc, item, "shortcut", "");
    yyjson_mut_obj_add_bool(doc, item, "checked", false);
    yyjson_mut_obj_add_bool(doc, item, "disabled", false);
}

/* Writes the item in place; true when it changed. */
static bool item_put(
    yyjson_mut_doc *doc,
    yyjson_mut_val *item,
    const char *label,
    uint32_t icon,
    bool checked,
    bool disabled
)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    bool changed = ui_data_put_text(doc, item, "label", label);
    changed |= ui_data_put_text(doc, item, "icon", glyph);
    changed |= ui_data_put_bool(item, "checked", checked);
    return ui_data_put_bool(item, "disabled", disabled) || changed;
}

/* Gives `items` `length` items, adding and removing at its end, so items
   already shown keep their elements; true when its length changed. */
static bool items_fit(yyjson_mut_doc *doc, yyjson_mut_val *items, int length)
{
    int had = (int)yyjson_mut_arr_size(items);
    for (int i = had; i < length; i++) {
        item_add(doc, yyjson_mut_arr_add_obj(doc, items));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(items);
    }
    return had != length;
}

/* ---------- The menubar's lists (`menubar`) ---------- */

/* The rows and submenus the menus name without a command. */
static const struct
{
    const char *key, *label;
    uint32_t icon;
} MENU_ITEMS[] = {
    { "newMenu", "New", 0 },
    /* An ellipsis: the dialog asks for a name before anything is made. */
    { "project", "Project\xe2\x80\xa6", KIT_ICON_FOLDER },
#ifdef __EMSCRIPTEN__
    { "openRecent", "Open Project", KIT_ICON_FOLDER },
#else
    { "openRecent", "Open Recent", KIT_ICON_FOLDER },
#endif
    { "importProject", "Import Project", 0 },
    { "exportProject", "Export Project", 0 },
    { "add", "Add", KIT_ICON_ADD },
    { "transform", "Transform", 0 },
};

void studio_menubar_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *menubar = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "menubar");
    yyjson_mut_val *items = yyjson_mut_obj_add_obj(doc, menubar, "items");
    for (size_t i = 0; i < sizeof MENU_ITEMS / sizeof *MENU_ITEMS; i++) {
        yyjson_mut_val *item = yyjson_mut_obj_add_obj(doc, items, MENU_ITEMS[i].key);
        item_add(doc, item);
        item_put(doc, item, MENU_ITEMS[i].label, MENU_ITEMS[i].icon, false, false);
    }
    yyjson_mut_obj_add_arr(doc, menubar, "projects");
    yyjson_mut_obj_add_arr(doc, menubar, "prototypes");
    yyjson_mut_obj_add_bool(doc, menubar, "web", false);
}

/* Export archives saved files and import reloads the page, so both need
   saved work; only the browser without a local server offers them. */
static bool web_projects()
{
#ifdef __EMSCRIPTEN__
    return web_projects_available();
#else
    return false;
#endif
}

static void web_project(studio *s, bool import)
{
#ifdef __EMSCRIPTEN__
    if (!web_projects_available()) {
        return;
    }
    if (studio_unsaved(s)) {
        toast(s, KIT_TOAST_WARNING, "Save changes first", NULL);
        return;
    }
    if (import) {
        web_import_project();
    } else {
        web_export_project();
    }
#else
    (void)s;
    (void)import;
#endif
}

/* The recent and workspace projects, the open one checked and disabled. */
static bool show_projects(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *projects)
{
    bool changed = items_fit(doc, projects, acount(s->projects));
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(projects);
    for (int i = 0; i < acount(s->projects); i++) {
        bool current = studio_project_current(s, i);
        changed |= item_put(doc, yyjson_mut_arr_iter_next(&it), s->projects[i].name, 0, current, current);
    }
    return changed;
}

/* The scene's named prototypes with their icons, disabled while the scene
   takes no edits. */
static bool show_prototypes(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *prototypes)
{
    const scene_prototype_asset *assets = s->scene->prototypes;
    int count = 0;
    for (int i = 0; i < acount(assets); i++) {
        count += assets[i].prototype && assets[i].prototype->name;
    }
    bool changed = items_fit(doc, prototypes, count);
    bool readonly = !studio_editable(s);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(prototypes);
    for (int i = 0; i < acount(assets); i++) {
        const char *name = assets[i].prototype ? assets[i].prototype->name : NULL;
        if (name) {
            changed |= item_put(
                doc,
                yyjson_mut_arr_iter_next(&it),
                name,
                studio_prototype_icon(s, name),
                false,
                readonly
            );
        }
    }
    return changed;
}

bool studio_menubar_sync(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *menubar = at->variable;
    bool changed = show_projects(s, at->doc, yyjson_mut_obj_get(menubar, "projects"));
    changed |= show_prototypes(s, at->doc, yyjson_mut_obj_get(menubar, "prototypes"));
    return ui_data_put_bool(menubar, "web", web_projects()) || changed;
}

void studio_menubar_action(studio *s, const char *action, const ui_activation *a)
{
    int index = a->argc ? atoi(a->args[0]) : -1;
    if (!strcmp(action, "newProject")) {
        studio_new_project_open(s);
    } else if (!strcmp(action, "openProject") && index >= 0 && index < acount(s->projects) && !studio_project_current(s, index)) {
        studio_open_project(s, index);
    } else if (!strcmp(action, "add") && a->argc) {
        studio_add_object(s, a->args[0]);
    } else if (!strcmp(action, "importProject") || !strcmp(action, "exportProject")) {
        web_project(s, action[0] == 'i');
    }
}

/* ---------- Play target, playback speed and UI Scale (`shell`) ---------- */

void studio_shell_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *shell = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "shell");
    yyjson_mut_val *tab_menu = yyjson_mut_obj_add_obj(doc, shell, "tabMenu");
    item_add(doc, yyjson_mut_obj_add_obj(doc, tab_menu, "pin"));
    item_add(doc, yyjson_mut_obj_add_obj(doc, tab_menu, "close"));
    yyjson_mut_val *target = yyjson_mut_obj_add_obj(doc, shell, "target");
    yyjson_mut_obj_add_strcpy(doc, target, "label", "");
    yyjson_mut_obj_add_strcpy(doc, target, "icon", "");
    item_add(doc, yyjson_mut_obj_add_obj(doc, target, "scene"));
    item_add(doc, yyjson_mut_obj_add_obj(doc, target, "preview"));
    yyjson_mut_val *speed = yyjson_mut_obj_add_obj(doc, shell, "speed");
    yyjson_mut_obj_add_strcpy(doc, speed, "label", "");
    yyjson_mut_obj_add_bool(doc, speed, "on", false);
    items_fit(doc, yyjson_mut_obj_add_arr(doc, speed, "presets"), studio_speed_count);
    yyjson_mut_val *scale = yyjson_mut_obj_add_obj(doc, shell, "scale");
    yyjson_mut_obj_add_strcpy(doc, scale, "label", "");
    items_fit(doc, yyjson_mut_obj_add_arr(doc, scale, "presets"), UI_SCALE_COUNT);
}

/* What Play could preview: the open prototype document, else the selected
   object's prototype. */
static scene_prototype_asset *preview_candidate(studio *s)
{
    app_scene_authoring *scene = s->scene;
    scene_actor *actor = selected_actor(scene);
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        return selected_prototype(scene);
    }
    return actor ? find_prototype(scene, actor->prototype_name) : NULL;
}

/* What Play runs: the scene, or a preview of a prototype; fixed while a
   session runs. */
static bool show_target(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *target)
{
    app_scene_authoring *scene = s->scene;
    scene_prototype_asset *previewed = prototype_at(scene, scene->runtime_preview_prototype);
    scene_prototype_asset *candidate = preview_candidate(s);
    const char *model = scene_model_preview_name(scene);
    bool model_preview = app_scene_authoring_runtime_state(scene).target ==
                             APP_RUNTIME_PREVIEW &&
                         model;
    bool prototype_preview = app_scene_authoring_runtime_state(scene).target ==
                                 APP_RUNTIME_PREVIEW &&
                             previewed;
    bool preview = model_preview || prototype_preview;
    bool session = studio_session(s);
    const scene_prototype_asset *shown = candidate ? candidate : previewed;
    char title[128], scene_label[160], preview_label[160], current[160], icon[5];
    snprintf(
        scene_label,
        sizeof scene_label,
        "Scene \xc2\xb7 %s",
        studio_scene_title(scene, title, sizeof title)
    );
    snprintf(
        preview_label,
        sizeof preview_label,
        "Preview \xc2\xb7 %s",
        shown   ? shown->prototype->name
        : model ? model
                : "Prototype"
    );
    snprintf(
        current,
        sizeof current,
        "Preview \xc2\xb7 %s",
        model_preview       ? model
        : prototype_preview ? previewed->prototype->name
                            : ""
    );
    studio_glyph(icon, preview ? KIT_ICON_PROTOTYPE : KIT_ICON_SCENE);
    bool changed = ui_data_put_text(doc, target, "label", preview ? current : scene_label);
    changed |= ui_data_put_text(doc, target, "icon", icon);
    changed |= item_put(doc, yyjson_mut_obj_get(target, "scene"), scene_label, 0, !preview, session);
    return item_put(
               doc,
               yyjson_mut_obj_get(target, "preview"),
               preview_label,
               0,
               preview && (model_preview || shown == previewed),
               session || (!candidate && !model_preview)
           ) ||
           changed;
}

static void choose_target(studio *s, const char *target)
{
    app_scene_authoring *scene = s->scene;
    scene_prototype_asset *candidate = preview_candidate(s);
    bool model_preview = scene_model_preview_name(scene) != NULL;
    if (studio_session(s)) {
        return;
    }
    if (!strcmp(target, "scene")) {
        if (app_scene_authoring_runtime_state(scene).target == APP_RUNTIME_PREVIEW) {
            app_scene_authoring_set_runtime_target(scene, APP_RUNTIME_SCENE);
        }
    } else if (candidate) {
        scene->selected_prototype = (int)(candidate - scene->prototypes);
        app_scene_authoring_set_runtime_target(scene, APP_RUNTIME_PREVIEW);
    } else if (model_preview) {
        app_scene_authoring_set_runtime_target(scene, APP_RUNTIME_PREVIEW);
    }
}

/* The speed beside the transport, as UI Scale keeps its percentage in the
   status bar: marked while it is not 1x, with a menu of its steps. */
static bool show_speed(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *speed)
{
    double now = app_scene_authoring_time_scale(s->scene);
    char label[16];
    studio_speed_label(now, label, sizeof label);
    bool changed = ui_data_put_text(doc, speed, "label", label);
    changed |= ui_data_put_bool(speed, "on", now != 1);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(yyjson_mut_obj_get(speed, "presets"));
    for (int i = 0; i < studio_speed_count; i++) {
        studio_speed_label(studio_speeds[i], label, sizeof label);
        changed |= item_put(doc, yyjson_mut_arr_iter_next(&it), label, 0, now == studio_speeds[i], false);
    }
    return changed;
}

static void percent(float scale, char *out, size_t size)
{
    snprintf(out, size, "%d%%", (int)lroundf(scale * 100));
}

/* UI Scale stays in reach, as Office keeps its zoom in the status bar. */
static bool show_scale(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *scale)
{
    char label[16];
    percent(s->prefs.ui_scale, label, sizeof label);
    bool changed = ui_data_put_text(doc, scale, "label", label);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(yyjson_mut_obj_get(scale, "presets"));
    for (int i = 0; i < UI_SCALE_COUNT; i++) {
        percent(UI_SCALES[i], label, sizeof label);
        bool checked = fabsf(s->prefs.ui_scale - UI_SCALES[i]) < 0.001f;
        changed |= item_put(doc, yyjson_mut_arr_iter_next(&it), label, 0, checked, false);
    }
    return changed;
}

/* Resolve the document by identity each time: pinning moves it, and another
   action may close it while the menu is open. */
static int tab_menu_index(const studio *s)
{
    for (int i = 0; s->tab_menu_key && i < acount(s->scene->tabs); i++) {
        if (!strcmp(s->scene->tabs[i].key, s->tab_menu_key)) {
            return i;
        }
    }
    return -1;
}

static bool show_tab_menu(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *menu)
{
    int index = tab_menu_index(s);
    bool pinned = index >= 0 && s->scene->tabs[index].pinned;
    bool closable = studio_tab_closes(s, index);
    bool changed = item_put(
        doc,
        yyjson_mut_obj_get(menu, "pin"),
        pinned ? "Unpin Tab" : "Pin Tab",
        KIT_ICON_PIN,
        pinned,
        index < 0
    );
    yyjson_mut_val *close = yyjson_mut_obj_get(menu, "close");
    changed |= item_put(doc, close, "Close Tab", KIT_ICON_CLOSE, false, !closable);
    const char *chord = studio_command_chord(&s->prefs, STUDIO_COMMAND_CLOSE_TAB);
    changed |= ui_data_put_text(doc, close, "shortcut", chord[0] ? studio_chord(s, chord) : "");
    return changed;
}

static void open_tab_menu(studio *s, int index, const ui_activation *a)
{
    if (index < 0 || index >= acount(s->scene->tabs)) {
        return;
    }
    free(s->tab_menu_key);
    s->tab_menu_key = xstrdup(s->scene->tabs[index].key);
    studio_ui_resync(s, "shell");
    rml_element *from = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    rml_element *menu = from ? rml_element_by_id(rml_element_owner_document(from), "tab-menu")
                             : NULL;
    if (menu) {
        popover_show(menu, &(popover_anchor){ .side = POPOVER_POINT, .box = { a->x, a->y, 0, 0 } });
    }
}

bool studio_shell_sync(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *shell = at->variable;
    bool changed = show_tab_menu(s, at->doc, yyjson_mut_obj_get(shell, "tabMenu"));
    changed |= show_target(s, at->doc, yyjson_mut_obj_get(shell, "target"));
    changed |= show_speed(s, at->doc, yyjson_mut_obj_get(shell, "speed"));
    return show_scale(s, at->doc, yyjson_mut_obj_get(shell, "scale")) || changed;
}

void studio_shell_action(studio *s, const char *action, const ui_activation *a)
{
    if (!strcmp(action, "tabPin") || !strcmp(action, "tabClose")) {
        int index = tab_menu_index(s);
        if (index >= 0) {
            studio_command_run_with(
                s,
                !strcmp(action, "tabPin") ? STUDIO_COMMAND_TOGGLE_PIN_TAB
                                          : STUDIO_COMMAND_CLOSE_TAB,
                index
            );
        }
        return;
    }
    if (!a->argc) {
        return;
    }
    int index = atoi(a->args[0]);
    if (!strcmp(action, "tabMenu")) {
        open_tab_menu(s, index, a);
    } else if (!strcmp(action, "target")) {
        choose_target(s, a->args[0]);
    } else if (!strcmp(action, "speed") && index >= 0 && index < studio_speed_count) {
        app_scene_authoring_set_time_scale(s->scene, studio_speeds[index]);
    } else if (!strcmp(action, "scale") && index >= 0 && index < UI_SCALE_COUNT) {
        s->prefs.ui_scale = UI_SCALES[index];
    }
}

/* ---------- Areas ---------- */

/* Drawers hold panels, never a second Scene or graph. */
static bool drawer_editor(studio_editor editor)
{
    return editor == STUDIO_EDITOR_OUTPUT || editor == STUDIO_EDITOR_ASSETS ||
           editor == STUDIO_EDITOR_PERFORMANCE ||
           editor == STUDIO_EDITOR_PANEL || editor == STUDIO_EDITOR_CODE;
}

void studio_maximize_graph(studio *s)
{
    studio_layout *layout = studio_active_layout(s);
    int area = STUDIO_AREA_TOP;
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        area = layout->editors[i] == STUDIO_EDITOR_BEHAVIOR ? i : area;
    }
    layout->editors[area] = STUDIO_EDITOR_BEHAVIOR;
    s->applied.layouts[s->prefs.workspace].editors[area] = STUDIO_EDITOR_BEHAVIOR;
    s->maximized = area;
}

/* ---------- Editor types (workstream F3) ---------- */

/* The Studio project's panels that are tools in any project; the rest are
   pieces of Studio's own chrome, kept in a submenu. */
static const struct
{
    const char *name;
    uint32_t icon;
} TOOL_PANELS[] = {
    { "Inspector", KIT_ICON_TARGET },
    { "Data", KIT_ICON_LIBRARY },
    { "Theme", KIT_ICON_MATERIAL },
};

static int tool_panel(const char *name)
{
    for (size_t i = 0; i < sizeof TOOL_PANELS / sizeof *TOOL_PANELS; i++) {
        if (!strcmp(TOOL_PANELS[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

/* A panel's icon: a tool's own, else the panel glyph. */
static uint32_t panel_icon(const char *name)
{
    int tool = tool_panel(name);
    return tool >= 0 ? TOOL_PANELS[tool].icon : EDITOR_ICONS[STUDIO_EDITOR_PANEL];
}

/* Editor types by task, as Blender's editor type menu groups them. */
typedef struct
{
    const char *label;
    studio_editor editors[4];
    int count;
    bool tools; /* followed by the Studio panels that are tools */
} editor_group;

static const editor_group EDITOR_GROUPS[] = {
    { "General",
      { STUDIO_EDITOR_SCENE, STUDIO_EDITOR_OUTLINER, STUDIO_EDITOR_PROPERTIES, STUDIO_EDITOR_ASSETS },
      4,
      false },
    { "Scripting",
      { STUDIO_EDITOR_BEHAVIOR, STUDIO_EDITOR_MEMBERS, STUDIO_EDITOR_CODE },
      3,
      false },
    { "Diagnostics", { STUDIO_EDITOR_OUTPUT, STUDIO_EDITOR_PERFORMANCE }, 2, true },
};

studio_area_view studio_area_view_of(studio *s, int area)
{
    studio_layout *layout = studio_active_layout(s);
    studio_editor editor = layout->editors[area];
    bool panel = editor == STUDIO_EDITOR_PANEL;
    return (studio_area_view){
        .editor = editor,
        .panel = panel ? layout->panels[area] : "",
        .icon = panel ? panel_icon(layout->panels[area]) : EDITOR_ICONS[editor],
        .title = panel ? layout->panels[area] : EDITOR_LABELS[editor],
    };
}

static void add_glyph(yyjson_mut_doc *doc, yyjson_mut_val *obj, uint32_t icon)
{
    char glyph[5];
    studio_glyph(glyph, icon);
    yyjson_mut_obj_add_strcpy(doc, obj, "icon", glyph);
}

static void add_panel(yyjson_mut_doc *doc, yyjson_mut_val *list, const char *name)
{
    yyjson_mut_val *option = yyjson_mut_arr_add_obj(doc, list);
    yyjson_mut_obj_add_strcpy(doc, option, "value", name);
    yyjson_mut_obj_add_strcpy(doc, option, "label", name);
    add_glyph(doc, option, panel_icon(name));
}

void studio_editor_choices(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *editors, yyjson_mut_val *panels)
{
    yyjson_mut_val *groups = yyjson_mut_obj_get(editors, "groups");
    yyjson_mut_arr_clear(groups);
    for (size_t g = 0; g < sizeof EDITOR_GROUPS / sizeof *EDITOR_GROUPS; g++) {
        const editor_group *group = &EDITOR_GROUPS[g];
        yyjson_mut_val *shown = yyjson_mut_arr_add_obj(doc, groups);
        yyjson_mut_obj_add_str(doc, shown, "label", group->label);
        yyjson_mut_obj_add_bool(doc, shown, "tools", group->tools);
        yyjson_mut_val *options = yyjson_mut_obj_add_arr(doc, shown, "options");
        for (int i = 0; i < group->count; i++) {
            studio_editor e = group->editors[i];
            yyjson_mut_val *option = yyjson_mut_arr_add_obj(doc, options);
            yyjson_mut_obj_add_int(doc, option, "value", e);
            yyjson_mut_obj_add_str(doc, option, "label", EDITOR_LABELS[e]);
            add_glyph(doc, option, EDITOR_ICONS[e]);
            yyjson_mut_obj_add_bool(doc, option, "drawer", drawer_editor(e));
        }
    }
    yyjson_mut_val *tools = yyjson_mut_obj_get(panels, "tools");
    yyjson_mut_val *others = yyjson_mut_obj_get(panels, "others");
    yyjson_mut_arr_clear(tools);
    yyjson_mut_arr_clear(others);
    for (int i = 0; i < studio_ui_panel_count(s); i++) {
        const char *name = studio_ui_panel_name(s, i);
        add_panel(doc, tool_panel(name) >= 0 ? tools : others, name);
    }
}

void studio_area_show(studio *s, int area, int editor, const char *panel)
{
    studio_layout *layout = studio_active_layout(s);
    if (area < 0 || area >= STUDIO_AREA_COUNT || editor < 0 ||
        editor >= STUDIO_EDITOR_COUNT ||
        (area == STUDIO_AREA_DRAWER && !drawer_editor((studio_editor)editor))) {
        return;
    }
    layout->editors[area] = (studio_editor)editor;
    if (editor == STUDIO_EDITOR_PANEL) {
        snprintf(layout->panels[area], sizeof layout->panels[area], "%s", panel);
    }
}

/* The editor's own header controls. */
static void editor_header(studio *s, int index)
{
    switch (studio_active_layout(s)->editors[index]) {
        case STUDIO_EDITOR_SCENE: studio_scene_header(s, index); break;
        case STUDIO_EDITOR_BEHAVIOR: studio_behavior_header(s, index); break;
        case STUDIO_EDITOR_OUTLINER: studio_outliner_header(s, index); break;
        case STUDIO_EDITOR_PROPERTIES:
            studio_properties_header(s, index);
            break;
        case STUDIO_EDITOR_OUTPUT: studio_output_header(s, index); break;
        case STUDIO_EDITOR_ASSETS: studio_assets_header(s, index); break;
        case STUDIO_EDITOR_PERFORMANCE:
            studio_performance_header(s, index);
            break;
        case STUDIO_EDITOR_MEMBERS: studio_members_header(s, index); break;
        case STUDIO_EDITOR_CODE: studio_code_header(s, index); break;
        case STUDIO_EDITOR_PANEL:
            kit_label(s->kit, "title", NULL, studio_active_layout(s)->panels[index]);
            break;
        default: break;
    }
}

/* The area header is authored: projects/studio's AreaHeader, one per area,
   binds its `area` (studio_sources.c) to its editor type menu, to maximize
   it and to close the drawer. The editor's own controls stay native in its
   slot. Fields keep their keys while chrome controls allow global shortcuts
   through the host. */
static void area_header(studio *s, int index)
{
    kit *k = s->kit;
    rml_element *host = studio_ui_area_header(s, index, "AreaHeader");
    if (!host) {
        return;
    }
    if (kit_slot_begin(k, "controls", host, "#controls")) {
        editor_header(s, index);
        kit_slot_end(k);
    }
}

/* An area: its header and its editor's body, in a document of its own over
   its place in the Workspace, so what changes inside it lays out only that
   area. */
static void area(studio *s, int index)
{
    kit *k = s->kit;
    studio_layout *layout = studio_active_layout(s);
    studio_editor editor = layout->editors[index];
    kit_document_begin(k, "doc", "area-doc", false);
    kit_push(k, "area", NULL, "area");
    area_header(s, index);
    kit_push(k, "body", NULL, "area-body");
    switch (editor) {
        case STUDIO_EDITOR_SCENE: studio_scene_body(s, index); break;
        case STUDIO_EDITOR_BEHAVIOR: studio_behavior_body(s, index); break;
        case STUDIO_EDITOR_OUTLINER: studio_outliner_body(s, index); break;
        case STUDIO_EDITOR_PROPERTIES: studio_properties_body(s, index); break;
        case STUDIO_EDITOR_OUTPUT: studio_output_body(s, index); break;
        case STUDIO_EDITOR_ASSETS: studio_assets_body(s, index); break;
        case STUDIO_EDITOR_PERFORMANCE:
            studio_performance_body(s, index);
            break;
        case STUDIO_EDITOR_MEMBERS: studio_members_body(s, index); break;
        case STUDIO_EDITOR_CODE: studio_code_body(s, index); break;
        case STUDIO_EDITOR_PANEL:
            studio_ui_area(s, index, layout->panels[index]);
            break;
        default: break;
    }
    kit_pop(k);
    kit_pop(k);
    s->areas[index].rect = kit_item_rect(k);
    kit_document_end(k);
}

/* ---------- Workspace (workstream C3) ---------- */

/* Each area's place in projects/studio's Workspace (#area-<key>), by
   studio_area_slot. */
static const char *const AREA_KEYS[STUDIO_AREA_COUNT] = {
    "top", "bottom", "outliner", "properties", "drawer",
};

/* What the Workspace lays out, in dp: each splitter's value and bounds, from
   the arrangement and the sizes its columns took in the last layout. The
   top and bottom areas split `size`, stacked or side by side; the Outliner
   and Properties split the side column's `height`. The drawer keeps its
   open height while it eases shut, so its place shrinks from there. */
typedef struct
{
    float size, top, top_min, top_max;
    float height, outliner, outliner_max;
    float drawer, drawer_max, side_max;
} workspace_extents;

static workspace_extents extents(studio *s)
{
    const studio_layout *layout = studio_active_layout(s);
    bool across = s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    float column = s->main_rect.h > 0 ? s->main_rect.h : s->in.height - 60;
    float drawer_max = fmaxf(80, column - 200);
    float drawer = fminf(layout->drawer_height, drawer_max);
    float size = across ? (s->main_rect.w > 0 ? s->main_rect.w : s->in.width)
                        : column - (layout->drawer_open ? drawer + 4 : 0);
    float minimum = across ? 160 : 80;
    float height = s->side_rect.h > 0 ? s->side_rect.h : s->in.height - 60;
    return (workspace_extents){
        .size = size,
        .top = floorf((size - 4) * layout->main_split),
        .top_min = minimum,
        .top_max = fmaxf(minimum, size - minimum - 4),
        .height = height,
        .outliner = floorf((height - 4) * layout->side_split),
        .outliner_max = fmaxf(80, height - 120),
        .drawer = drawer,
        .drawer_max = drawer_max,
        .side_max = fmaxf(260, s->in.width - 320),
    };
}

/* The workspace's values Workspace.rml reads, after `max` (the maximized
   area's key, or ""), `across`, `drawerOpen` and `sideOpen`. */
static const char *const WORKSPACE_NUMBERS[] = {
    "top",    "topMin",       "topMax",    "outliner", "outlinerMax",
    "drawer", "drawerHeight", "drawerMax", "side",     "sideMax",
};
enum
{
    WORKSPACE_NUMBER_COUNT = sizeof WORKSPACE_NUMBERS / sizeof *WORKSPACE_NUMBERS,
};

void studio_workspace_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *w = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "workspace");
    yyjson_mut_obj_add_strcpy(doc, w, "max", "");
    yyjson_mut_obj_add_bool(doc, w, "across", false);
    yyjson_mut_obj_add_bool(doc, w, "drawerOpen", false);
    yyjson_mut_obj_add_bool(doc, w, "sideOpen", false);
    for (int i = 0; i < WORKSPACE_NUMBER_COUNT; i++) {
        yyjson_mut_obj_add_real(doc, w, WORKSPACE_NUMBERS[i], 0);
    }
}

bool studio_workspace_sync(studio *s, const studio_panel_sync *at)
{
    const studio_layout *layout = studio_active_layout(s);
    workspace_extents e = extents(s);
    yyjson_mut_val *w = at->variable;
    const char *max = s->maximized >= 0 ? AREA_KEYS[s->maximized] : "";
    bool changed = ui_data_put_text(at->doc, w, "max", max);
    changed |= ui_data_put_bool(w, "across", s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
    changed |= ui_data_put_bool(w, "drawerOpen", layout->drawer_open);
    changed |= ui_data_put_bool(w, "sideOpen", layout->side_open);
    /* Whole dp, as the kit sized the places. */
    const double numbers[WORKSPACE_NUMBER_COUNT] = {
        e.top,
        e.top_min,
        e.top_max,
        e.outliner,
        e.outliner_max,
        rintf(e.drawer),
        layout->drawer_height,
        e.drawer_max,
        rintf(layout->side_width),
        e.side_max,
    };
    for (int i = 0; i < WORKSPACE_NUMBER_COUNT; i++) {
        changed |= ui_data_put_number(w, WORKSPACE_NUMBERS[i], numbers[i]);
    }
    return changed;
}

/* A splitter moved: emit('workspace.resize', which, value) with which
   `main`, `outliner`, `drawer` or `side`. The arrangement is a preference,
   saved as preferences are, with no Undo step. The places follow in the
   same frame. */
void studio_workspace_action(studio *s, const char *action, const ui_activation *a)
{
    if (strcmp(action, "resize") || a->argc < 2) {
        return;
    }
    studio_layout *layout = studio_active_layout(s);
    workspace_extents e = extents(s);
    const char *which = a->args[0];
    float value = strtof(a->args[1], NULL);
    if (!strcmp(which, "main")) {
        layout->main_split = value / fmaxf(1, e.size - 4);
    } else if (!strcmp(which, "outliner")) {
        layout->side_split = value / fmaxf(1, e.height - 4);
    } else if (!strcmp(which, "drawer")) {
        layout->drawer_height = value;
    } else if (!strcmp(which, "side")) {
        layout->side_width = value;
    }
    studio_ui_resync(s, "workspace");
}

/* The border box `selector` took in the Workspace's last layout, in dp;
   empty while it is hidden. */
static kit_rect laid_out(studio *s, rml_element *host, const char *selector)
{
    rml_element *el = rml_element_query_selector(host, selector);
    if (!el || !rml_element_is_visible(el, false)) {
        return (kit_rect){ 0 };
    }
    float ratio = studio_pixel_ratio(s);
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    return (kit_rect){ at.x / ratio, at.y / ratio, size.x / ratio, size.y / ratio };
}

/* The columns' sizes split their areas; a hidden column keeps its last. */
static void measure(studio *s, rml_element *host)
{
    kit_rect main = laid_out(s, host, ".column.main");
    kit_rect side = laid_out(s, host, ".side .column");
    s->main_rect = main.w > 0 && main.h > 0 ? main : s->main_rect;
    s->side_rect = side.w > 0 && side.h > 0 ? side : s->side_rect;
}

/* Whether area `i` shows: the maximized one alone, else the drawer and the
   side column while open and while their places ease shut. */
static bool area_shown(studio *s, int i, kit_rect place)
{
    const studio_layout *layout = studio_active_layout(s);
    if (s->maximized >= 0) {
        return i == s->maximized;
    }
    if (i == STUDIO_AREA_DRAWER) {
        return layout->drawer_open || place.h > 0.5f;
    }
    if (i == STUDIO_AREA_OUTLINER || i == STUDIO_AREA_PROPERTIES) {
        return layout->side_open || place.w > 0.5f;
    }
    return true;
}

/* Area `i` in its place, a native slot of the Workspace. */
static void place(studio *s, rml_element *host, int i)
{
    char selector[32];
    snprintf(selector, sizeof selector, "#area-%s", AREA_KEYS[i]);
    kit_rect box = laid_out(s, host, selector);
    bool slot = kit_slot_begin(s->kit, AREA_KEYS[i], host, selector);
    bool shown = slot && area_shown(s, i, box);
    if (shown) {
        area(s, i);
    }
    if (slot) {
        kit_slot_end(s->kit);
    }
    if (!shown) {
        studio_ui_area_detach(s, i);
        s->areas[i].rect = (kit_rect){ 0 };
    }
}

bool studio_areas_moved(studio *s)
{
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        char path[48];
        snprintf(path, sizeof path, "app/workspace/%s/area", AREA_KEYS[i]);
        kit_rect now, was = s->areas[i].rect;
        if (kit_find(s->kit, path, &now) &&
            (fabsf(now.x - was.x) > 0.5f || fabsf(now.y - was.y) > 0.5f ||
             fabsf(now.w - was.w) > 0.5f || fabsf(now.h - was.h) > 0.5f)) {
            return true;
        }
    }
    return false;
}

/* The workspace is authored: projects/studio's Workspace lays out the
   areas' places, the splitters between them, the drawer and side column,
   and a maximized area filling it all, from the `workspace` source. It is
   a document of its own over the areas, so a splitter's hit area reaches
   past its gutter onto theirs; everything else in it passes the pointer
   through. */
static void workspace(studio *s)
{
    kit *k = s->kit;
    kit_document_begin(k, "workspace", "workspace-place", false);
    rml_element *host = studio_ui_panel(s, "workspace", "Workspace");
    if (host) {
        rml_element *document = rml_element_owner_document(host);
        rml_element_set_property(document, "z-index", "1");
        rml_element_set_property(document, "pointer-events", "none");
        measure(s, host);
        for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
            place(s, host, i);
        }
    }
    kit_document_end(k);
}

/* ---------- Status bar ---------- */

/* Authoring messages report a success, which the scene marks as its notice,
   or what kept an edit from happening, which shows as a warning. */
static void follow_messages(studio *s)
{
    if (!strcmp(s->scene->message, s->seen_message)) {
        return;
    }
    snprintf(s->seen_message, sizeof s->seen_message, "%s", s->scene->message);
    if (!s->seen_message[0]) {
        return;
    }
    if (!strcmp(s->seen_message, s->scene->notice)) {
        studio_report(s, s->seen_message);
        return;
    }
    studio_warn(s, s->seen_message);
}

/* The status bar is authored: projects/studio's StatusBar binds `editor`,
   `commands` (studio_sources.c) and `shell` and runs commands, in a
   document of its own whose place takes its height. Controls retain focus
   while the host routes global shortcuts. */
static void statusbar(studio *s)
{
    follow_messages(s);
    kit_document_begin(s->kit, "status", "bar-place", true);
    studio_ui_panel(s, "status", "StatusBar");
    kit_document_end(s->kit);
}

/* ---------- Frame ---------- */

/* Whether the pointer is over the kit's overlay layer or an open authored
   popover, such as a menubar menu reaching over an area. */
static bool over_overlay(studio *s)
{
    const char *path = kit_hovered_path(s->kit);
    rml_element *el = rml_context_hover_element(rml_backend_context(s->backend));
    while (el && !rml_element_has_attribute(el, "popover")) {
        el = rml_element_parent(el);
    }
    return el || (path && !strncmp(path, "layer/", 6));
}

static void track_pointer(studio *s)
{
    s->hover_area = -1;
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        kit_rect r = s->areas[i].rect;
        if (s->in.mouse_x >= r.x && s->in.mouse_y >= r.y &&
            s->in.mouse_x < r.x + r.w && s->in.mouse_y < r.y + r.h) {
            s->hover_area = i;
        }
    }
    if (s->in.pressed[0] || s->in.pressed[1] || s->in.pressed[2]) {
        /* Shell controls act on the last editor. A popup can overlap another
           pane geometrically without transferring its command context. */
        if (s->hover_area >= 0 && !over_overlay(s) && !studio_palette_is_open(s)) {
            s->focus_area = s->hover_area;
        }
    }
}

/* editor/Components.kdl applies without a restart. An invalid edit keeps the
   last valid file; the engine's presentation applies while it is missing. */
static void sync_presentation(studio *s)
{
    studio_presentation *p = &s->presentation;
    char path[sizeof p->watch.path];
    snprintf(path, sizeof path, "%s/" COMPONENT_PRESENTATION_PATH, s->scene->content_root);
    if (!file_watch_changed(&p->watch, path, s->time)) {
        return;
    }
    if (!file_watch_exists(&p->watch)) {
        component_presentation_free(&p->hints);
        return;
    }
    char *text = text_file_read(path);
    char error[256] = "could not be read";
    if (!text || !component_presentation_read(
                     text,
                     (ecs_component_catalog){ s->scene->registry.components,
                                              s->scene->registry.component_count },
                     &p->hints,
                     error,
                     sizeof error
                 )) {
        toast(s, KIT_TOAST_ERROR, COMPONENT_PRESENTATION_PATH, error);
    }
    free(text);
}

/* Opening a document must reveal it when a different editor fills the window.
   Consider each tab transition once, so maximizing another pane deliberately
   does not immediately undo that choice. Normal split arrangements stay put. */
static void reveal_document(studio *s)
{
    int active = s->scene->active_tab;
    if (active < 0 || active >= acount(s->scene->tabs)) {
        return;
    }
    const scene_document_tab *tab = &s->scene->tabs[active];
    uint64_t identity = 1469598103934665603ull;
    for (const unsigned char *at = (const unsigned char *)tab->key; *at; at++) {
        identity = (identity ^ *at) * 1099511628211ull;
    }
    if (identity == s->revealed_document) {
        return;
    }
    bool initial = s->revealed_document == 0;
    s->revealed_document = identity;
    if (initial || s->maximized < 0 || s->collaboration.following) {
        return;
    }
    studio_editor target = STUDIO_EDITOR_SCENE;
    if (tab->kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
        tab->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        target = STUDIO_EDITOR_BEHAVIOR;
    }
    if (tab->kind == APP_EDITOR_DOCUMENT_UI) {
        char name[sizeof s->followed];
        if (!studio_ui_document_asset(s, tab->path, name, sizeof name)) {
            target = STUDIO_EDITOR_CODE;
        }
    }
    studio_editor shown = studio_active_layout(s)->editors[s->maximized];
    if (tab->kind == APP_EDITOR_DOCUMENT_BEHAVIOR && shown == STUDIO_EDITOR_CODE) {
        target = STUDIO_EDITOR_CODE;
    }
    if (shown == target || shown == STUDIO_EDITOR_CODE ||
        (shown == STUDIO_EDITOR_PROPERTIES && tab->kind == APP_EDITOR_DOCUMENT_PROTOTYPE)) {
        return;
    }
    studio_show_editor(s, target);
}

/* The window names the active document and the project, the document marked
   while it has unsaved edits as its tab is, as VS Code's title does: "● Level
   — Celeste — Chibi Studio". A guest's window is its host's. */
static void window_title(studio *s)
{
    if (s->hosted) {
        return;
    }
    app_scene_authoring *scene = s->scene;
    const scene_document_tab *tab = scene->active_tab >= 0 &&
                                            scene->active_tab < acount(scene->tabs)
                                      ? &scene->tabs[scene->active_tab]
                                      : NULL;
    char document[128];
    const char *label = tab ? tab->label
                            : studio_scene_title(scene, document, sizeof document);
    bool dirty = tab ? scene_tabs_dirty(scene, tab) : scene->scene_dirty;
    const char *project = s->project.name;
    char title[sizeof s->window_title];
    snprintf(
        title,
        sizeof title,
        "%s%s%s%s \xe2\x80\x94 Chibi Studio",
        dirty ? "\xe2\x97\x8f " : "",
        label,
        project ? " \xe2\x80\x94 " : "",
        project ? project : ""
    );
    if (strcmp(title, s->window_title)) {
        snprintf(s->window_title, sizeof s->window_title, "%s", title);
        cf_app_set_title(title);
    }
}

void studio_emit(studio *s)
{
    sync_presentation(s);
    studio_disk_frame(s);
    studio_code_frame(s);
    studio_canvas_follow(s);
    reveal_document(s);
    track_pointer(s);
    kit_root(s->kit, "app");
    studio_drop_frame(s);
    topbar(s);
    workspace(s);
    /* Visual designer (workstream H): the Text Editor's cursor and the Canvas. */
    studio_designer_frame(s);
    statusbar(s);
    studio_palette(s);
    studio_new_asset_frame(s);
    studio_prototype_actions_frame(s);
    undo_toast(s);
    studio_projects_frame(s);
    studio_rewire_toast(s);
    studio_members_toast(s);
    studio_refactor_frame(s);
    studio_graph_menu_frame(s);
    studio_graph_details_frame(s);
    studio_component_toast(s);
    studio_keep_toast(s);
    studio_new_project_frame(s);
    window_title(s);
}
