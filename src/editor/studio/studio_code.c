#include "assets/ui_document.h"
#include "graph/nodes/core_nodes.h"
/* The Text Editor area (Blender's): code asset source, a UI document's RML
 * and RCSS, or a stylesheet's RCSS. Code follows the active asset; UI editing
 * keeps its per-part language service and history. Focusing its text
 * makes it active again, so Save and Undo follow it. Edits apply across Studio
 * as they are typed (the code_apply_live preference), or when saved.
 *
 * Code intelligence (ui_code.h), after VS Code's editor: syntax errors as the
 * part parses and RmlUi's warnings once its text rests, as gutter markers and
 * a count in the header that steps through them; completion as you type and
 * on Control+Space; a card describing what the pointer rests on; Go to
 * Definition (F12 or a primary click) and Find References (Shift+F12) into
 * Output's Find view; F8 and Shift+F8 through the problems, and Mod+/ to
 * comment lines. These are Text Editor commands (studio_commands.c), so
 * Search, the text's context menu and Settings ▸ Keyboard reach them.
 *
 * One history: the document's buffer (ui_buffer.h) keeps it and the code
 * editors keep none. Typing reaches the buffer as typed text, an accepted
 * completion as a step of its own, and the undo keys (Z with the primary
 * modifier, Vim's u and Control+R) step the buffer as Studio's Undo does.
 * Whatever changes the buffer comes back to the editor as an edit, so its
 * cursor keeps its place; after an undo it lands on the change. Each open
 * document part keeps its own code editor, with its cursor, scroll and Vim
 * state. */
#include "authoring/graph_expression.h"
#include "authoring/preview_source.h"
#include "authoring/source_navigation.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/text_file.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"
#include "ui/popover.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Frames before a revealed line is laid out and can scroll into view. */
#define REVEAL_FRAMES 2
/* Seconds text rests before RmlUi checks it. */
#define CHECK_DELAY 0.3
/* Seconds a scan of the project serves before completion, hover and
   navigation scan again. */
#define PROJECT_STALE 2.0
/* Characters that start completion when typed, as '<' starts a tag. */
#define COMPLETION_TRIGGERS "<.\"("
/* Seconds a moved caret rests before its tab keeps it. */
#define CAPTURE_REST 0.5

static const char *const PART_NAMES[UI_PART_COUNT] = { "rml", "rcss" };

static scene_document_tab *active_asset(studio *s)
{
    int at = s->scene->active_tab;
    if (at < 0 || at >= acount(s->scene->tabs)) {
        return NULL;
    }
    scene_document_tab *tab = &s->scene->tabs[at];
    if (tab->kind == APP_EDITOR_DOCUMENT_UI || !tab->path) {
        return NULL;
    }
    const char *extension = strrchr(tab->path, '.');
    return extension && (!strcmp(extension, CODE_ASSET_EXTENSION) ||
                         !strcmp(extension, DECLARATIVE_ASSET_EXTENSION))
             ? tab
             : NULL;
}

static void capture_asset(studio *s, asset_code *code)
{
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        scene_document_tab *tab = &s->scene->tabs[i];
        if (!tab->path || strcmp(tab->path, asset_code_path(code))) {
            continue;
        }
        av_value_free(tab->source_state);
        tab->source_state = asset_code_capture(code);
        tab->source_dirty = asset_code_dirty(code);
    }
}

bool studio_code_replace(studio *s, const char *path, const char *before, const char *text, const char *metadata)
{
    bool open = false;
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        open |= s->scene->tabs[i].path && !strcmp(s->scene->tabs[i].path, path);
    }
    if (!open) {
        return false;
    }
    for (int i = 0; i < acount(s->code.assets); i++) {
        asset_code *code = s->code.assets[i];
        if (!strcmp(asset_code_path(code), path) &&
            asset_code_replace_with_metadata(code, before, text, metadata)) {
            capture_asset(s, code);
            asset_index_invalidate(s->scene);
            return true;
        }
    }
    return false;
}

static asset_code *asset_context(studio *s, asset_code *code)
{
    asset_code_set_source_world(code, s->scene->live, s->scene->content_root);
    return code;
}

static const authoring_reference *source_reference(const asset_code *code, int offset)
{
    const authoring_navigation *index = asset_code_navigation(code);
    return index ? authoring_navigation_at(index, asset_code_document(code), offset)
                 : NULL;
}

static asset_code *asset_editor(studio *s, scene_document_tab *tab)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        asset_code *code = s->code.assets[i];
        if (strcmp(asset_code_path(code), tab->path)) {
            continue;
        }
        if (tab->source_state && !asset_code_has_state(code, tab->source_state)) {
            asset_code
                *restored = asset_code_restore(tab->source_state, &s->scene->registry);
            if (!restored) {
                studio_warn(s, "Could not restore code source draft");
                return NULL;
            }
            asset_code_free(code);
            s->code.assets[i] = restored;
            return asset_context(s, restored);
        }
        return asset_context(s, code);
    }
    char error[256];
    asset_code *code = tab->source_state
                         ? asset_code_restore(tab->source_state, &s->scene->registry)
                         : asset_code_open(tab->path, error, sizeof error);
    if (!code) {
        studio_warn(s, tab->source_state ? "Could not restore code source draft" : error);
        return NULL;
    }
    asset_code_set_registry(code, &s->scene->registry);
    apush(s->code.assets, code);
    asset_context(s, code);
    capture_asset(s, code);
    return code;
}

/* A retained source session belongs to its document, not a temporary graph
   opened in the same editor (for example a material preview). */
static bool source_graph_active(const studio *s)
{
    const behavior_asset *asset = editor_behavior_asset(s->scene->graph_editor);
    return asset && s->scene->graph_document_id &&
           !strcmp(asset->name, s->scene->graph_document_id);
}

static bool sync_asset_graph(studio *s, asset_code *code)
{
    scene_document_tab *tab = active_asset(s);
    if ((tab && tab->source_only) || !source_graph_active(s)) {
        return false;
    }
    bool pending = asset_code_pending(code);
    bool changed = asset_code_sync_graph(code, s->scene->graph_editor,
                                         s->prefs.code_apply_live);
    bool settled = pending && !asset_code_pending(code) &&
                   !asset_code_error(code)[0];
    if (settled || changed) {
        asset_code_hold_runtime(code, !s->prefs.code_apply_live && asset_code_dirty(code));
    }
    snprintf(
        s->scene->source_preview_hold,
        sizeof s->scene->source_preview_hold,
        "%s",
        asset_code_runtime_held(code) && !s->prefs.code_apply_live
            ? asset_code_path(code)
            : ""
    );
    return changed;
}

static void save_asset(studio *s, asset_code *code)
{
    scene_document_tab *saving = active_asset(s);
    if (saving && saving->source_only) {
        asset_code_time(code, s->time + 0.2);
        asset_code_validate(code, s->scene->graph_editor);
    }
    game_preview_definition *definition = s->scene->definition;
    if (definition && game_preview_definition_path(definition) &&
        !strcmp(asset_code_path(code), game_preview_definition_path(definition))) {
        asset_code_time(code, s->time + 0.2);
        sync_asset_graph(s, code);
    }
    if (s->scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM && s->scene->system_path &&
        !strcmp(asset_code_path(code), s->scene->system_path)) {
        asset_code_time(code, s->time + 0.2);
        sync_asset_graph(s, code);
    }
    char error[256];
    if (!asset_code_save(code, error, sizeof error)) {
        studio_error(s, "Could not save", error);
        return;
    }
    capture_asset(s, code);
    scene_document_tab *tab = active_asset(s);
    if (tab && tab->source_only) {
        int result;
        if (tab->kind == APP_EDITOR_DOCUMENT_SYSTEM) {
            result = open_system_asset(s->scene, tab->path);
        } else {
            const authoring_source *source = asset_code_source(code);
            if (!source || game_preview_source_apply(
                               s->scene->live,
                               tab->path,
                               asset_code_text(code),
                               false,
                               error,
                               sizeof error
                           )) {
                studio_error(s, "Could not open repaired graph", error);
                return;
            }
            const char *name = source->analysis->name;
            refresh_asset_catalog(s->scene);
            result = open_behavior_asset(s->scene, name);
        }
        if (result != 0) {
            studio_error(s, "Could not open repaired graph", app_scene_authoring_message(s->scene));
            return;
        }
        tab = active_asset(s);
        tab->source_only = false;
        scene_tabs_mark_document_saved(s->scene, tab->kind, s->scene->document.id);
        asset_index_invalidate(s->scene);
        studio_behavior_show(s, 2);
    }
    if (s->scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM) {
        s->scene->source_preview_hold[0] = 0;
        scene_tabs_sync_current(s->scene);
        scene_tabs_mark_document_saved(
            s->scene,
            APP_EDITOR_DOCUMENT_SYSTEM,
            s->scene->document.id
        );
    }
    if (definition && game_preview_definition_path(definition) &&
        !strcmp(asset_code_path(code), game_preview_definition_path(definition))) {
        definition->unsaved = false;
        s->scene->source_preview_hold[0] = 0;
        editor *e = s->scene->graph_editor;
        av_value_free(e->saved_snapshot);
        e->saved_snapshot = ed_value_snapshot(e->asset);
    }
    char title[160];
    snprintf(title, sizeof title, "Saved %s", content_basename(asset_code_path(code)));
    studio_saved(s, title, NULL);
}

static scene_document_tab *active_ui(studio *s)
{
    app_scene_authoring *scene = s->scene;
    scene_document_tab *tab = scene->active_tab >= 0 &&
                                      scene->active_tab < acount(scene->tabs)
                                ? &scene->tabs[scene->active_tab]
                                : NULL;
    return tab && tab->kind == APP_EDITOR_DOCUMENT_UI ? tab : NULL;
}

static scene_document_tab *ui_tab(studio *s, const char *path)
{
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        scene_document_tab *tab = &s->scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_UI && !strcmp(tab->path, path)) {
            return tab;
        }
    }
    return NULL;
}

/* The document the Text Editor shows: the active one, else the last shown
 * while it is open, else the last opened. */
static scene_document_tab *shown(studio *s)
{
    scene_document_tab *tab = active_ui(s);
    tab = tab ? tab : ui_tab(s, s->code.path);
    for (int i = acount(s->scene->tabs) - 1; !tab && i >= 0; i--) {
        if (s->scene->tabs[i].kind == APP_EDITOR_DOCUMENT_UI) {
            tab = &s->scene->tabs[i];
        }
    }
    return tab;
}

ui_buffer *studio_ui_buffer(studio *s)
{
    scene_document_tab *tab = active_ui(s);
    return tab ? tab->ui : NULL;
}

/* ---------- Saving and closing ---------- */

/* The workspace must read disk without a source overlay. Keep the draft's
   checkpoint until the reload succeeds so an unreadable file loses no edits. */
app_disk_result studio_code_reload_path(studio *s, const char *path)
{
    const char *current = text_overlay_text(path);
    char *overlay = current ? xstrdup(current) : NULL;
    if (overlay) {
        text_overlay_set(path, NULL);
    }
    app_disk_result result = app_scene_authoring_reload_path(s->scene, path);
    if (result != APP_DISK_RELOADED) {
        if (overlay || text_overlay_text(path)) {
            text_overlay_set(path, overlay);
        }
        for (int i = 0; i < acount(s->code.assets); i++) {
            asset_code *code = s->code.assets[i];
            if (!strcmp(asset_code_path(code), path)) {
                asset_code_adopt_overlay(code);
            }
        }
    }
    free(overlay);
    if (result != APP_DISK_RELOADED) {
        return result;
    }
    if (text_overlay_text(path)) {
        text_overlay_set(path, NULL);
    }
    for (int i = acount(s->code.assets) - 1; i >= 0; i--) {
        if (!strcmp(asset_code_path(s->code.assets[i]), path)) {
            asset_code_free(s->code.assets[i]);
            adel(s->code.assets, i);
        }
    }
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        scene_document_tab *tab = &s->scene->tabs[i];
        if (!tab->path || strcmp(tab->path, path)) {
            continue;
        }
        av_value_free(tab->source_state);
        tab->source_state = NULL;
        tab->source_dirty = false;
    }
    return result;
}

static void save(studio *s, scene_document_tab *tab)
{
    if (save_ui_document(s->scene, tab) != 0) {
        studio_error(s, "Could not save", app_scene_authoring_message(s->scene));
        return;
    }
    char title[160];
    snprintf(title, sizeof title, "Saved %s", tab->label);
    studio_saved(s, title, NULL);
}

/* A file another program changed since it loaded is saved over only when
 * asked, as editors ask before overwriting. */
void studio_code_save(studio *s)
{
    scene_document_tab *asset = active_asset(s);
    if (asset) {
        asset_code *code = asset_editor(s, asset);
        if (code) {
            save_asset(s, code);
        }
        return;
    }
    scene_document_tab *tab = active_ui(s);
    assert(tab);
    if (ui_buffer_on_disk(tab->ui)) {
        save(s, tab);
        return;
    }
    kit_toast_dismiss(s->kit, s->code.overwrite_toast);
    snprintf(s->code.overwrite, sizeof s->code.overwrite, "%s", tab->path);
    s->code.overwrite_toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                           .title = "Changed on disk",
                           .description = tab->label,
                           .action = "Save Anyway",
                           .duration = -1 }
    );
}

/* A behavior shown from its source keeps no snapshot of its saved graph:
   its file is the saved state, so discarding its edits reloads the file.
   False, with an error toast, when that failed. */
static bool reload_source_behavior(studio *s, const scene_document_tab *tab)
{
    if (tab->kind != APP_EDITOR_DOCUMENT_BEHAVIOR || tab->asset_saved ||
        !tab->path || !scene_tabs_dirty(s->scene, tab)) {
        return true;
    }
    char *path = xstrdup(tab->path);
    app_disk_result result = studio_code_reload_path(s, path);
    free(path);
    if (result == APP_DISK_RELOADED) {
        return true;
    }
    studio_error(s, "Could not discard changes", app_scene_authoring_message(s->scene));
    return false;
}

/* A UI document's tab closes with its text; another discards what its
   document has unsaved, as reverting it would. */
void studio_discard_tab(studio *s, int index)
{
    if (!scene_tabs_can_close(s->scene, index)) {
        return;
    }
    scene_document_tab *tab = &s->scene->tabs[index];
    if (tab->kind == APP_EDITOR_DOCUMENT_UI) {
        scene_tabs_close(s->scene, APP_EDITOR_DOCUMENT_UI, tab->path);
        return;
    }
    if (!reload_source_behavior(s, tab)) {
        return;
    }
    tab = &s->scene->tabs[index];
    for (int i = acount(s->code.assets) - 1; i >= 0; i--) {
        if (tab->path && !strcmp(asset_code_path(s->code.assets[i]), tab->path)) {
            /* Remove the live overlay before the tab reloads its disk state. */
            asset_code_free(s->code.assets[i]);
            adel(s->code.assets, i);
        }
    }
    if (scene_tabs_discard(s->scene, index) != 0) {
        studio_error(s, "Could not close the tab", app_scene_authoring_message(s->scene));
    }
}

static void toast_actions(studio *s)
{
    kit *k = s->kit;
    if (s->code.overwrite_toast && kit_toast_action(k, s->code.overwrite_toast)) {
        s->code.overwrite_toast = 0;
        scene_document_tab *tab = ui_tab(s, s->code.overwrite);
        if (tab) {
            save(s, tab);
        }
    }
}

static int line_offset(const char *text, int line)
{
    int at = 0;
    for (int n = 1; n < line && text[at]; n++) {
        at += (int)strcspn(text + at, "\n");
        at += text[at] == '\n';
    }
    return at;
}

/* ---------- Code editors ---------- */

/* The code editor of the tab's shown part, made on first show. */
static studio_code_editor *part_editor(studio *s, const scene_document_tab *tab)
{
    studio_code *c = &s->code;
    const ui_buffer *b = tab->ui;
    for (int i = 0; i < acount(c->editors); i++) {
        if (!strcmp(c->editors[i].path, tab->path) && c->editors[i].part == b->part) {
            return &c->editors[i];
        }
    }
    studio_code_editor made = {
        .part = b->part,
        .editor = kit_code_editor_create(ui_buffer_text(b, b->part)),
        .synced = xstrdup(ui_buffer_text(b, b->part)),
    };
    snprintf(made.path, sizeof made.path, "%s", tab->path);
    apush(c->editors, made);
    return &alast(c->editors);
}

/* Drops the editors of documents that closed. */
static void prune_editors(studio *s)
{
    studio_code *c = &s->code;
    for (int i = acount(c->assets) - 1; i >= 0; i--) {
        bool open = false;
        for (int t = 0; t < acount(s->scene->tabs); t++) {
            scene_document_tab *tab = &s->scene->tabs[t];
            if (tab->path && !strcmp(tab->path, asset_code_path(c->assets[i]))) {
                tab->source_dirty = asset_code_dirty(c->assets[i]);
                open = true;
            }
        }
        if (!open) {
            if (c->editor == asset_code_editor(c->assets[i])) {
                c->editor = NULL;
            }
            asset_code_free(c->assets[i]);
            adel(c->assets, i);
        }
    }
    for (int i = acount(c->editors) - 1; i >= 0; i--) {
        if (ui_tab(s, c->editors[i].path)) {
            continue;
        }
        if (c->editor == c->editors[i].editor) {
            c->editor = NULL;
        }
        kit_code_editor_destroy(c->editors[i].editor);
        free(c->editors[i].synced);
        adel(c->editors, i);
    }
}

void studio_code_free(studio *s)
{
    studio_code *c = &s->code;
    for (int i = 0; i < acount(c->assets); i++) {
        asset_code_free(c->assets[i]);
    }
    afree(c->assets);
    for (int i = 0; i < acount(c->editors); i++) {
        kit_code_editor_destroy(c->editors[i].editor);
        free(c->editors[i].synced);
    }
    afree(c->editors);
    c->editor = NULL;
    ui_source_free(c->source);
    c->source = NULL;
    afree(c->spans);
    for (int p = 0; p < UI_PART_COUNT; p++) {
        ui_code_markers_free(c->markers[p]);
    }
    ui_check_messages_free(c->checked);
    rml_backend_destroy(c->checker);
    ui_source_project_free(&c->project);
    ui_code_completions_free(&c->completions);
    free(c->hover);
    afree(c->problems.rows);
    afree(s->ui_project_problems);
}

/* ---------- Checking ---------- */

/* RmlUi checks the shown document once its text has rested. */
static void check_shown(studio *s)
{
    studio_code *c = &s->code;
    scene_document_tab *tab = shown(s);
    if (!tab) {
        return;
    }
    ui_buffer *b = tab->ui;
    if (!strcmp(c->checked_path, tab->path) && c->checked_revision == b->revision) {
        return;
    }
    if (b->revision != c->check_seen) {
        c->check_seen = b->revision;
        c->check_due = s->time + CHECK_DELAY;
    }
    if (s->time < c->check_due) {
        return;
    }
    if (!c->checker) {
        c->checker = rml_backend_create(1280, 720, 1);
    }
    ui_check_messages_free(c->checked);
    c->checked = NULL;
    c->check_failure[0] = '\0';
    ui_code_check(
        b,
        rml_backend_context(c->checker),
        &c->checked,
        c->check_failure,
        sizeof c->check_failure
    );
    snprintf(c->checked_path, sizeof c->checked_path, "%s", tab->path);
    c->checked_revision = b->revision;
    c->markers_stale = true;
}

/* The Text Editor shows the active UI document when it is assigned to an area. */
static void follow_active(studio *s, const scene_document_tab *active)
{
    if (active) {
        snprintf(s->code.path, sizeof s->code.path, "%s", active->path);
    }
}

/* A moved caret is kept in its tab once it rests, or once another source
   shows; edits are kept as they happen. */
static void capture_caret(studio *s)
{
    studio_code *c = &s->code;
    for (int i = 0; i < acount(c->assets); i++) {
        if (!strcmp(asset_code_path(c->assets[i]), c->capture_path)) {
            capture_asset(s, c->assets[i]);
        }
    }
    c->capture_due = 0;
}

void studio_code_frame(studio *s)
{
    scene_document_tab *tab = active_asset(s);
    bool behavior = tab && tab->kind == APP_EDITOR_DOCUMENT_BEHAVIOR &&
                    s->scene->definition &&
                    game_preview_definition_path(s->scene->definition) &&
                    !strcmp(tab->path, game_preview_definition_path(s->scene->definition));
    bool system = tab && tab->kind == APP_EDITOR_DOCUMENT_SYSTEM &&
                  s->scene->system_path && !strcmp(tab->path, s->scene->system_path);
    if (behavior || system || (tab && tab->source_only)) {
        asset_code *code = asset_editor(s, tab);
        if (code) {
            asset_code_time(code, s->time);
        }
        if (code) {
            bool pending = asset_code_pending(code);
            bool changed = false;
            if (tab->source_only) {
                asset_code_validate(code, s->scene->graph_editor);
            } else {
                changed = sync_asset_graph(s, code);
            }
            if (changed || (pending && !asset_code_pending(code))) {
                capture_asset(s, code);
                asset_index_invalidate(s->scene);
            }
        }
        if (code && asset_code_pending(code)) {
            s->ui_schedule.wake = true;
        }
    }
    if (s->code.capture_due && s->time >= s->code.capture_due) {
        capture_caret(s);
    }
    bool applied = false;
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        scene_document_tab *tab = &s->scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_UI) {
            applied |= ui_buffer_update(tab->ui, s->time, s->prefs.code_apply_live);
        }
    }
    /* References follow the text that stands in for the files. */
    if (applied) {
        asset_index_invalidate(s->scene);
    }
    follow_active(s, active_ui(s));
    prune_editors(s);
    check_shown(s);
    toast_actions(s);
    /* Mouse buttons 4 and 5 over the Text Editor step Back and Forward. */
    int over = s->hover_area;
    if (s->nav_button && over >= 0 &&
        studio_active_layout(s)->editors[over] == STUDIO_EDITOR_CODE) {
        studio_nav_step(s, s->nav_button);
        s->nav_button = 0;
    }
}

bool studio_code_history(studio *s, bool redo, bool apply)
{
    scene_document_tab *tab = active_asset(s);
    if (!tab || (tab->kind != APP_EDITOR_DOCUMENT_BEHAVIOR &&
                 tab->kind != APP_EDITOR_DOCUMENT_SYSTEM)) {
        return false;
    }
    asset_code *code = asset_editor(s, tab);
    if (!code) {
        return false;
    }
    sync_asset_graph(s, code);
    if (!apply) {
        return asset_code_can_replay(code, redo);
    }
    if (!asset_code_replay(code, redo, s->prefs.code_apply_live)) {
        return false;
    }
    sync_asset_graph(s, code);
    capture_asset(s, code);
    return true;
}

static asset_code *graph_code(const studio *s)
{
    if (!source_graph_active(s)) {
        return NULL;
    }
    const char *path = s->scene->document.kind == APP_EDITOR_DOCUMENT_SYSTEM
                         ? s->scene->system_path
                     : s->scene->definition
                         ? game_preview_definition_path(s->scene->definition)
                         : NULL;
    for (int i = 0; path && i < acount(s->code.assets); i++) {
        asset_code *code = s->code.assets[i];
        if (!strcmp(path, asset_code_path(code))) {
            return code;
        }
    }
    return NULL;
}

bool studio_code_graph_readonly(const studio *s)
{
    const asset_code *code = graph_code(s);
    return code && asset_code_graph_blocked(code);
}

bool studio_code_edit_source(void *user, const authoring_edit_request *request, bool coalesce, char *error, size_t size)
{
    studio *s = user;
    editor *e = sg_graph_editor(s);
    const authoring_source_presentation *view = editor_presentation(e);
    scene_document_tab *tab = active_asset(s);
    asset_code *code = tab && !tab->source_only ? asset_editor(s, tab) : NULL;
    const authoring_source *source = code ? asset_code_source(code) : NULL;
    if (!view || !source || !source->valid ||
        strcmp(authoring_source_presentation_text(view), asset_code_text(code))) {
        snprintf(error, size, "Apply or repair the source draft before editing this graph");
        return false;
    }
    authoring_edit_result result = { 0 };
    if (!authoring_source_edit(source->analysis, asset_code_catalog(code), view, request, &result, error, size)) {
        return false;
    }
    source_catalog_edit edit = { .path = asset_code_path(code), .text = result.text };
    bool accepted = !game_preview_sources_validate(s->scene->live, &edit, 1, error, size);
    if (accepted) {
        accepted = asset_code_edit_source(
            code,
            authoring_source_presentation_text(view),
            result.text,
            result.metadata,
            coalesce
        );
        if (accepted) {
            capture_asset(s, code);
            asset_index_invalidate(s->scene);
            s->ui_schedule.wake = true;
        } else {
            snprintf(error, size, "Source changed during the edit");
        }
    }
    authoring_edit_result_free(&result);
    return accepted;
}

char *studio_code_expression(const studio *s, const node *n)
{
    const editor *e = sg_graph_editor(s);
    const authoring_source_presentation *view = editor_presentation(e);
    if (view) {
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            view,
            e->active_graph->graph_id,
            n->id,
            0
        );
        if (!origin || origin->kind != SOURCE_PRESENTATION_EXPRESSION ||
            origin->range.start < 0 || origin->range.end <= origin->range.start) {
            return NULL;
        }
        const char *text = authoring_source_presentation_text(view);
        return strndup(
            text + origin->range.start,
            (size_t)(origin->range.end - origin->range.start)
        );
    }
    return graph_expression_display(e->asset, n);
}

bool studio_code_expression_replace(studio *s, const node *n, const char *text, char *error, size_t size)
{
    editor *e = sg_graph_editor(s);
    const authoring_source_presentation *view = editor_presentation(e);
    const source_presentation_origin *origin = view
                                                 ? authoring_source_presentation_origin(
                                                       view,
                                                       e->active_graph->graph_id,
                                                       n->id,
                                                       0
                                                   )
                                                 : NULL;
    if (!origin || origin->kind != SOURCE_PRESENTATION_EXPRESSION) {
        snprintf(error, size, "Expression has no source location");
        return false;
    }
    authoring_edit_request request = {
        .kind = AUTHORING_EDIT_EXPRESSION,
        .generation = authoring_source_edit_generation(view),
        .origin = origin->identity,
        .expression = text,
    };
    bool accepted = ed_source_edit(e, &request);
    snprintf(error, size, "%s", e->connect_error);
    return accepted;
}

void studio_code_pin_label(const studio *s, const node *n, const pin_spec *pin, char *out, size_t size)
{
    if (n->desc == &NODE_BehaviorExpression &&
        (!pin->display_name || !pin->display_name[0])) {
        for (int i = 0; i < n->input_count; i++) {
            if (pin != &n->input_pins[i]) {
                continue;
            }
            graph_expression_input_name(sg_graph_editor(s)->asset, n, i, out, size);
            return;
        }
    }
    if (n->desc == &NODE_FunctionEntry &&
        (!pin->display_name || !pin->display_name[0])) {
        const behavior_asset *asset = sg_graph_editor(s)->asset;
        for (int g = 0; g < acount(asset->graphs); g++) {
            const graph *generated = asset->graphs[g];
            if (!generated->math_expression || graph_node_by_id(generated, n->id) != n) {
                continue;
            }
            for (int owner = 0; owner < acount(asset->graphs); owner++) {
                const graph *caller = asset->graphs[owner];
                for (int at = 0; at < acount(caller->nodes); at++) {
                    const node *expression = caller->nodes[at];
                    if (expression->desc != &NODE_BehaviorExpression ||
                        expression->reference_asset ||
                        expression->reference_id != generated->graph_id) {
                        continue;
                    }
                    for (int input = 0; input < expression->input_count; input++) {
                        if (expression->input_pins[input].declaration_id != pin->id) {
                            continue;
                        }
                        graph_expression_input_name(asset, expression, input, out, size);
                        return;
                    }
                }
            }
        }
    }
    ed_pin_label(n, pin, out, size);
}

void studio_code_graph_label(const studio *s, const graph *g, char *out, size_t size)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    for (int i = 0; g->math_expression && i < acount(asset->graphs); i++) {
        const graph *owner = asset->graphs[i];
        for (int j = 0; j < acount(owner->nodes); j++) {
            const node *n = owner->nodes[j];
            if (n->reference_id != g->graph_id || n->reference_asset) {
                continue;
            }
            char *expression = studio_code_expression(s, n);
            if (expression) {
                snprintf(out, size, "%s", expression);
                free(expression);
                return;
            }
        }
    }
    snprintf(out, size, "%s", g->name && g->name[0] ? g->name : "Event Graph");
}

char *studio_code_expression_resolve(const studio *s, const node *n, const char *text, char *error, size_t size)
{
    return graph_expression_resolve(sg_graph_editor(s)->asset, n, text, error, size);
}

int studio_behavior_view(const studio *s)
{
    const studio_layout *layout = &s->prefs.layouts[s->prefs.workspace];
    if (s->maximized >= 0) {
        return layout->editors[s->maximized] == STUDIO_EDITOR_CODE ? 1 : 0;
    }
    bool graph = false, code = false;
    for (int i = STUDIO_AREA_TOP; i <= STUDIO_AREA_BOTTOM; i++) {
        graph |= layout->editors[i] == STUDIO_EDITOR_BEHAVIOR;
        code |= layout->editors[i] == STUDIO_EDITOR_CODE;
    }
    return graph && code ? 2 : code ? 1 : 0;
}

void studio_behavior_show(studio *s, int view)
{
    char *action = NULL;
    /* Both views describe the graph's document, even when a scene source was
       selected while the graph stayed open. Activate its existing tab so the
       other document retains its source draft and history. */
    if (view == 2 && studio_graph_active(s)) {
        const char *path = s->scene->graph_document_kind == APP_EDITOR_DOCUMENT_SYSTEM
                             ? s->scene->system_path
                         : s->scene->definition
                             ? game_preview_definition_path(s->scene->definition)
                             : NULL;
        for (int i = 0; path && i < acount(s->scene->tabs); i++) {
            const scene_document_tab *candidate = &s->scene->tabs[i];
            if (candidate->path && !strcmp(candidate->path, path)) {
                if (i != s->scene->active_tab && collab_active() &&
                    !collab_replaying()) {
                    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
                    yyjson_mut_val *root = yyjson_mut_obj(doc);
                    yyjson_mut_doc_set_root(doc, root);
                    yyjson_mut_obj_add_str(doc, root, "family", "command");
                    yyjson_mut_obj_add_str(doc, root, "id", "behavior.view");
                    yyjson_mut_obj_add_strcpy(doc, root, "tab", candidate->key);
                    action = yyjson_mut_write(doc, 0, NULL);
                    yyjson_mut_doc_free(doc);
                    if (collab_actions_begin(action) != COLLAB_ACTION_APPLY) {
                        free(action);
                        return;
                    }
                    collab_begin_replay();
                }
                int result = scene_tabs_activate(s->scene, i);
                if (action) {
                    collab_end_replay();
                }
                if (result != 0) {
                    free(action);
                    return;
                }
                break;
            }
        }
    }
    scene_document_tab *tab = active_asset(s);
    if (tab && tab->source_only) {
        view = 1;
    }
    studio_layout *layout = studio_active_layout(s);
    int graph_area = -1, code_area = -1;
    for (int i = STUDIO_AREA_TOP; i <= STUDIO_AREA_BOTTOM; i++) {
        graph_area = layout->editors[i] == STUDIO_EDITOR_BEHAVIOR ? i : graph_area;
        code_area = layout->editors[i] == STUDIO_EDITOR_CODE ? i : code_area;
    }
    if (view == 2) {
        graph_area = graph_area >= 0              ? graph_area
                   : code_area == STUDIO_AREA_TOP ? STUDIO_AREA_BOTTOM
                                                  : STUDIO_AREA_TOP;
        code_area = graph_area == STUDIO_AREA_TOP ? STUDIO_AREA_BOTTOM : STUDIO_AREA_TOP;
        layout->editors[graph_area] = STUDIO_EDITOR_BEHAVIOR;
        layout->editors[code_area] = STUDIO_EDITOR_CODE;
        s->maximized = -1;
        s->focus_area = code_area;
    } else {
        int area = view == 1 ? code_area : graph_area;
        area = area >= 0 ? area : STUDIO_AREA_TOP;
        layout->editors[area] = view == 1 ? STUDIO_EDITOR_CODE : STUDIO_EDITOR_BEHAVIOR;
        s->maximized = area;
        s->focus_area = area;
    }
    s->ui_schedule.wake = true;
    if (action) {
        collab_actions_finish(action, true);
        free(action);
    }
}

/* A command moves the caret: the next frame reports the move as it does
   the keys', so it ends a typing burst as a click would. */
static void move_caret(kit_code_editor *e, int byte)
{
    kit_code_set_cursor(e, byte);
    kit_code_post(e, KIT_CODE_CURSOR);
}

void studio_code_reveal_node(studio *s, const studio_node_ref *at)
{
    scene_document_tab *tab = active_asset(s);
    if (!tab || !s->scene->graph_document_id ||
        strcmp(at->asset, s->scene->graph_document_id)) {
        return;
    }
    bool visible = false;
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        visible |= studio_active_layout(s)->editors[i] == STUDIO_EDITOR_CODE;
    }
    if (!visible) {
        return;
    }
    asset_code *code = asset_editor(s, tab);
    int start, end;
    if (code && asset_code_location(code, at->graph_id, at->node_id, &start, &end)) {
        move_caret(asset_code_editor(code), start);
    }
}

/* ---------- Opening ---------- */

bool studio_open_ui_part(studio *s, const char *path, ui_part part, int line)
{
    finish_transform_edit(s->scene, false);
    if (scene_tabs_open_ui(s->scene, path) != 0) {
        studio_error(s, content_basename(path), app_scene_authoring_message(s->scene));
        return false;
    }
    ui_buffer *b = studio_ui_buffer(s);
    b->part = ui_buffer_text(b, part) ? part : UI_PART_RCSS;
    s->code.reveal = line;
    s->code.reveal_offset = -1;
    s->code.reveal_wait = REVEAL_FRAMES;
    studio_show_editor(s, STUDIO_EDITOR_CODE);
    return true;
}

bool studio_open_ui_source(studio *s, const char *path, int line)
{
    size_t length = strlen(path), extension = strlen(UI_STYLE_EXTENSION);
    bool styles = length > extension &&
                  !strcmp(path + length - extension, UI_STYLE_EXTENSION);
    return studio_open_ui_part(s, path, styles ? UI_PART_RCSS : UI_PART_RML, line);
}

bool studio_code_open_place(studio *s, const ui_code_place *place)
{
    bool authored = false;
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    for (int i = 0; index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (asset->path && !strcmp(asset->path, place->path) &&
            asset->kind != REFERENCE_ASSET_UI) {
            authored = true;
            if (app_scene_authoring_open_asset(s->scene, i)) {
                return false;
            }
            break;
        }
    }
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        scene_document_tab *tab = &s->scene->tabs[i];
        if (tab->kind != APP_EDITOR_DOCUMENT_UI && tab->path &&
            !strcmp(tab->path, place->path)) {
            scene_tabs_activate(s->scene, i);
            asset_code *code = asset_editor(s, tab);
            if (!code) {
                return false;
            }
            move_caret(asset_code_editor(code), place->offset);
            studio_show_editor(s, STUDIO_EDITOR_CODE);
            return true;
        }
    }
    if (authored || !studio_open_ui_part(s, place->path, place->part, place->line)) {
        return false;
    }
    s->code.reveal_offset = place->offset;
    return true;
}

/* ---------- Problems (workstream Q) ---------- */

/* Each part's markers, as the Text Editor marks them, as Output's rows: a
   row opens the start of what its marker spans, or its line. */
static void add_problems(
    const ui_buffer *b,
    const ui_check_message *messages,
    const char *failure,
    studio_ui_problem **out
)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        const char *text = ui_buffer_text(b, (ui_part)p);
        if (!text) {
            continue;
        }
        ui_source *source = ui_source_parse(text, ui_code_language((ui_part)p));
        kit_code_marker *markers = NULL;
        ui_code_markers(source, (ui_part)p, messages, failure, &markers);
        for (int i = 0; i < acount(markers); i++) {
            const kit_code_marker *m = &markers[i];
            studio_ui_problem row = { .at = { .part = (ui_part)p,
                                              .offset = m->end > m->start ? m->start : -1,
                                              .line = m->line },
                                      .error = m->severity == KIT_CODE_ERROR };
            snprintf(row.at.path, sizeof row.at.path, "%s", b->path);
            snprintf(row.message, sizeof row.message, "%s", m->message);
            apush(*out, row);
        }
        ui_code_markers_free(markers);
        ui_source_free(source);
    }
}

const studio_ui_problem *studio_code_problems(studio *s)
{
    studio_code *c = &s->code;
    scene_document_tab *asset = active_asset(s);
    if (asset) {
        asetlen(c->problems.rows, 0);
        asset_code *code = asset_editor(s, asset);
        const kit_code_marker *markers = code ? asset_code_markers(code) : NULL;
        for (int i = 0; i < acount(markers); i++) {
            const kit_code_marker *m = &markers[i];
            studio_ui_problem row = { .at = { .offset = m->start, .line = m->line },
                                      .error = m->severity == KIT_CODE_ERROR };
            snprintf(row.at.path, sizeof row.at.path, "%s", asset->path);
            snprintf(row.message, sizeof row.message, "%s", m->message);
            apush(c->problems.rows, row);
        }
        return c->problems.rows;
    }
    scene_document_tab *tab = shown(s);
    if (!tab) {
        asetlen(c->problems.rows, 0);
        c->problems.path[0] = '\0';
        return c->problems.rows;
    }
    bool checked = !strcmp(c->checked_path, tab->path);
    unsigned check = checked ? c->checked_revision : 0;
    if (!strcmp(c->problems.path, tab->path) &&
        c->problems.revision == tab->ui->revision && c->problems.checked == check) {
        return c->problems.rows;
    }
    asetlen(c->problems.rows, 0);
    add_problems(
        tab->ui,
        checked ? c->checked : NULL,
        checked ? c->check_failure : NULL,
        &c->problems.rows
    );
    snprintf(c->problems.path, sizeof c->problems.path, "%s", tab->path);
    c->problems.revision = tab->ui->revision;
    c->problems.checked = check;
    return c->problems.rows;
}

/* One file of the project: an open document as it stands, else as read. */
static void file_problems(studio *s, const char *path, studio_ui_problem **out)
{
    scene_document_tab *tab = ui_tab(s, path);
    char error[256] = "";
    ui_buffer *b = tab ? tab->ui : ui_buffer_open(path, error, sizeof error);
    if (!b) {
        studio_ui_problem row = { .at = { .offset = -1, .line = 1 }, .error = true };
        snprintf(row.at.path, sizeof row.at.path, "%s", path);
        snprintf(row.message, sizeof row.message, "%s", error);
        apush(*out, row);
        return;
    }
    ui_check_message *messages = NULL;
    ui_code_check(b, rml_backend_context(s->code.checker), &messages, error, sizeof error);
    add_problems(b, messages, error, out);
    ui_check_messages_free(messages);
    if (!tab) {
        ui_buffer_free(b);
    }
}

static bool scanned(const ui_source_project *project, const char *path)
{
    for (int i = 0; i < acount(project->files); i++) {
        if (!strcmp(project->files[i].path, path)) {
            return true;
        }
    }
    return false;
}

/* The project's ui assets and stylesheets, then the open documents that
   lie elsewhere. */
void studio_code_project_problems(studio *s, studio_ui_problem **out)
{
    if (!s->code.checker) {
        s->code.checker = rml_backend_create(1280, 720, 1);
    }
    char root[PATH_MAX];
    ui_source_project project = { 0 };
    if (realpath(s->scene->content_root, root)) {
        ui_source_project_scan(&project, root);
    }
    for (int i = 0; i < acount(project.files); i++) {
        /* A ui asset's .rcss is a part of the document its .rml opens. */
        char *document = ui_buffer_document(project.files[i].path);
        if (!strcmp(document, project.files[i].path)) {
            file_problems(s, document, out);
        }
        free(document);
    }
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        const scene_document_tab *tab = &s->scene->tabs[i];
        if (tab->kind == APP_EDITOR_DOCUMENT_UI && !scanned(&project, tab->path)) {
            file_problems(s, tab->path, out);
        }
    }
    ui_source_project_free(&project);
}

/* ---------- Header ---------- */

/* A problem's first byte: its range's, else its line's. */
static int problem_byte(const char *text, const kit_code_marker *marker)
{
    return marker->end > marker->start ? marker->start
                                       : line_offset(text, marker->line);
}

/* The index of the problem F8 last went to in a document's part while the
   caret stays where it went, else -1. */
static int problem_kept(const studio_code *c, const void *document, int part, int cursor)
{
    const studio_code_problem *p = &c->problem;
    bool kept = p->document == document && p->part == part && p->at == cursor;
    return kept ? p->index : -1;
}

/* The nearest problem after the byte, or before it going back, or -1.
   Problems on one byte take turns in their order, from `kept` when it is
   one of them. */
static int problem_from(const char *text, const kit_code_marker *markers, int from, int kept, int direction)
{
    bool on = kept >= 0 && kept < acount(markers) &&
              problem_byte(text, &markers[kept]) == from;
    int tie = on ? kept : direction > 0 ? INT_MAX : -1;
    int found = -1, best = 0;
    for (int i = 0; i < acount(markers); i++) {
        int at = problem_byte(text, &markers[i]);
        bool beyond = direction > 0 ? at > from || (at == from && i > tie)
                                    : at < from || (at == from && i < tie);
        bool nearer = found < 0 || (direction > 0 ? at < best : at >= best);
        if (beyond && nearer) {
            found = i;
            best = at;
        }
    }
    return found;
}

/* The first problem past the text's start going on, or the last going
   back. */
static int problem_edge(const char *text, const kit_code_marker *markers, int direction)
{
    return problem_from(text, markers, direction > 0 ? -1 : INT_MAX, -1, direction);
}

/* F8 in an asset's source: the caret on the next problem, its message
   beside it, wrapping past the text's end. */
static void step_asset_problem(studio_code *c, asset_code *code, int direction)
{
    kit_code_editor *e = asset_code_editor(code);
    const kit_code_marker *markers = asset_code_markers(code);
    const char *text = kit_code_text(e, NULL);
    int cursor = kit_code_cursor(e);
    int found = problem_from(text, markers, cursor, problem_kept(c, code, -1, cursor), direction);
    found = found >= 0 ? found : problem_edge(text, markers, direction);
    if (found < 0) {
        kit_code_notice(e, "No problems");
        return;
    }
    int at = problem_byte(text, &markers[found]);
    c->problem = (studio_code_problem){ code, -1, found, at };
    move_caret(e, at);
    kit_code_notice(e, markers[found].message);
}

/* F8 in a UI document: the next problem in the part, then the other
   part's first, wrapping; reveal places the caret and the message once the
   part shows. */
static void step_ui_problem(studio *s, ui_buffer *b, int direction)
{
    studio_code *c = &s->code;
    ui_part part = b->part, other = part == UI_PART_RML ? UI_PART_RCSS : UI_PART_RML;
    int cursor = kit_code_cursor(c->editor);
    int found = problem_from(
        kit_code_text(c->editor, NULL),
        c->markers[part],
        cursor,
        problem_kept(c, b, part, cursor),
        direction
    );
    if (found < 0 && acount(c->markers[other])) {
        part = other;
    }
    const char *text = ui_buffer_text(b, part);
    found = found >= 0 ? found : problem_edge(text, c->markers[part], direction);
    if (found < 0) {
        kit_code_notice(c->editor, "No problems");
        return;
    }
    const kit_code_marker *marker = &c->markers[part][found];
    c->reveal = marker->line;
    c->reveal_offset = problem_byte(text, marker);
    c->problem = (studio_code_problem){ b, part, found, c->reveal_offset };
    c->reveal_wait = part != b->part ? REVEAL_FRAMES : 0;
    snprintf(c->reveal_notice, sizeof c->reveal_notice, "%s", marker->message);
    b->part = part;
}

enum
{
    HEADER_EMPTY,
    HEADER_ASSET, /* an asset's code source */
    HEADER_DOCUMENT,
};

void studio_code_header_add(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = {
        "title", "backKbd",     "forwardKbd", "icon",    "name",         "path",
        "vim",   "problemsTip", "error",      "runtime", "runtimeLabel", NULL
    };
    static const char *const FLAGS[] = { "dirty",      "live",   "back",
                                         "forward",    "parts",  "find",
                                         "behavior",   "rename", "calls",
                                         "sourceOnly", NULL };
    static const char *const NUMBERS[] = { "mode",   "part", "problems",
                                           "errors", "view", NULL };
    yyjson_mut_val *h = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "code_header");
    for (const char *const *text = TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, h, *text, "");
    }
    for (const char *const *flag = FLAGS; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, h, *flag, false);
    }
    for (const char *const *number = NUMBERS; *number; number++) {
        yyjson_mut_obj_add_int(doc, h, *number, 0);
    }
}

/* The document's problems: a count that steps to the next, its tip naming
   the errors and warnings. */
static bool show_problems(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *h)
{
    const studio_code *c = &s->code;
    int errors = 0, count = 0;
    for (int p = 0; c->editor && p < UI_PART_COUNT; p++) {
        for (int i = 0; i < acount(c->markers[p]); i++) {
            errors += c->markers[p][i].severity == KIT_CODE_ERROR;
            count++;
        }
    }
    char tip[64];
    snprintf(
        tip,
        sizeof tip,
        "%d %s, %d %s \xc2\xb7 Next problem",
        errors,
        errors == 1 ? "error" : "errors",
        count - errors,
        count - errors == 1 ? "warning" : "warnings"
    );
    bool changed = ui_data_put_int(h, "problems", count);
    changed |= ui_data_put_int(h, "errors", errors);
    return ui_data_put_text(doc, h, "problemsTip", count ? tip : "No problems") ||
           changed;
}

/* A UI document: Back and Forward, its part, its file, Find, Vim's mode,
   its problems and whether it reads. */
static bool show_document(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *h, const scene_document_tab *tab)
{
    const ui_buffer *b = tab->ui;
    const char *file = b->files[b->part];
    char glyph[5];
    studio_glyph(glyph, b->stylesheet ? KIT_ICON_FILE : KIT_ICON_LAYOUT);
    const char *mode = s->code.editor ? kit_code_mode(s->code.editor) : NULL;
    bool changed = ui_data_put_bool(h, "back", studio_nav_can_step(s, -1));
    changed |= ui_data_put_bool(h, "forward", studio_nav_can_step(s, 1));
    changed |= ui_data_put_text(doc, h, "backKbd", studio_chord(s, "Mod+["));
    changed |= ui_data_put_text(doc, h, "forwardKbd", studio_chord(s, "Mod+]"));
    changed |= ui_data_put_bool(h, "parts", !b->stylesheet);
    changed |= ui_data_put_int(h, "part", b->part);
    changed |= ui_data_put_text(doc, h, "icon", glyph);
    changed |= ui_data_put_text(doc, h, "name", content_basename(file));
    changed |= ui_data_put_text(doc, h, "path", file);
    changed |= ui_data_put_bool(h, "dirty", ui_buffer_dirty(b));
    changed |= ui_data_put_bool(h, "find", s->code.find.open);
    changed |= ui_data_put_text(doc, h, "vim", mode ? mode : "");
    changed |= ui_data_put_text(doc, h, "error", b->error);
    return show_problems(s, doc, h) || changed;
}

/* The shown document's header (CodeHeader.rml), or an asset's source's: its
   name, whether it is unsaved, and the apply mode. */
bool studio_code_header_sync(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *h = at->variable;
    /* The header shows with the text, so the graph decides Code or Both. */
    bool view_changed = ui_data_put_int(h, "view", studio_editor_shown(s, STUDIO_EDITOR_BEHAVIOR) ? 2 : 1);
    scene_document_tab *asset = active_asset(s);
    view_changed |= ui_data_put_bool(h, "sourceOnly", asset && asset->source_only);
    scene_document_tab *tab = asset ? NULL : shown(s);
    int mode = asset ? HEADER_ASSET : tab ? HEADER_DOCUMENT : HEADER_EMPTY;
    bool changed = ui_data_put_int(h, "mode", mode) || view_changed;
    changed |= ui_data_put_bool(h, "live", s->prefs.code_apply_live);
    if (asset) {
        asset_code *code = asset_editor(s, asset);
        changed |= ui_data_put_text(at->doc, h, "title", asset->label);
        const authoring_reference *
            reference = code
                          ? source_reference(code, kit_code_cursor(asset_code_editor(code)))
                          : NULL;
        const authoring_navigation *navigation = code ? asset_code_navigation(code)
                                                      : NULL;
        changed |= ui_data_put_bool(
            h,
            "rename",
            reference && authoring_navigation_definition(navigation, reference->target)
        );
        changed |= ui_data_put_bool(
            h,
            "calls",
            reference &&
                authoring_navigation_callable(navigation, reference->target).id != 0
        );
        changed |= ui_data_put_bool(
            h,
            "behavior",
            asset->kind == APP_EDITOR_DOCUMENT_BEHAVIOR || asset->kind == APP_EDITOR_DOCUMENT_SYSTEM
        );
        changed |= ui_data_put_bool(h, "back", studio_nav_can_step(s, -1));
        changed |= ui_data_put_bool(h, "forward", studio_nav_can_step(s, 1));
        changed |= ui_data_put_text(at->doc, h, "backKbd", studio_chord(s, "Mod+["));
        changed |= ui_data_put_text(at->doc, h, "forwardKbd", studio_chord(s, "Mod+]"));
        changed |= ui_data_put_bool(h, "find", s->code.find.open);
        const char *vim = code ? kit_code_mode(asset_code_editor(code)) : NULL;
        changed |= ui_data_put_text(at->doc, h, "vim", vim ? vim : "");
        changed |= ui_data_put_text(at->doc, h, "error", code ? asset_code_error(code) : "");
        const char *runtime = "", *runtime_label = "";
        if (code && (asset->source_only || asset_code_source(code) != NULL)) {
            if (asset->source_only) {
                runtime_label = "Source only";
                runtime = "Fix source errors and Save to open its graph. "
                          "Existing scene runtime stays unchanged";
            } else if (asset_code_error(code)[0]) {
                runtime_label = "Last valid";
                runtime = "Source errors retain the last valid graph and "
                          "runtime";
            } else if (asset_code_pending(code)) {
                runtime_label = "Compiling";
                runtime = "The current runtime stays active until this draft "
                          "compiles";
            } else if (asset_code_runtime_held(code)) {
                runtime_label = "On Save";
                runtime = "Valid source edits wait for Save before updating "
                          "the runtime";
            } else if (
                s->scene->definition && s->scene->definition->dirty &&
                game_preview_definition_path(s->scene->definition) &&
                !strcmp(asset->path, game_preview_definition_path(s->scene->definition))
            ) {
                runtime_label = "Preview waiting";
                runtime = s->scene->preview_message[0] ? s->scene->preview_message : "The current runtime stays active until the edited behavior can safely reload";
            }
        }
        changed |= ui_data_put_text(at->doc, h, "runtime", runtime);
        changed |= ui_data_put_text(at->doc, h, "runtimeLabel", runtime_label);
        const kit_code_marker *markers = code ? asset_code_markers(code) : NULL;
        int errors = 0, warnings = 0, notes = 0;
        for (int i = 0; i < acount(markers); i++) {
            errors += markers[i].severity == KIT_CODE_ERROR;
            warnings += markers[i].severity == KIT_CODE_WARNING;
            notes += markers[i].severity == KIT_CODE_INFO;
        }
        char tip[96];
        snprintf(
            tip,
            sizeof tip,
            "%d error%s, %d warning%s, %d note%s · Next problem",
            errors,
            errors == 1 ? "" : "s",
            warnings,
            warnings == 1 ? "" : "s",
            notes,
            notes == 1 ? "" : "s"
        );
        changed |= ui_data_put_int(h, "problems", acount(markers));
        changed |= ui_data_put_int(h, "errors", errors);
        changed |= ui_data_put_text(at->doc, h, "problemsTip", acount(markers) ? tip : "No problems");
        return ui_data_put_bool(h, "dirty", code && asset_code_dirty(code)) || changed;
    }
    return tab ? show_document(s, at->doc, h, tab) || changed : changed;
}

static void open_find(studio_code *c);

void studio_code_header_action(studio *s, const char *action, const ui_activation *a)
{
    scene_document_tab *asset = active_asset(s);
    scene_document_tab *tab = asset ? NULL : shown(s);
    int value = a->argc ? atoi(a->args[0]) : 0;
    if (!strcmp(action, "view")) {
        studio_behavior_show(s, value);
    } else if (!strcmp(action, "apply")) {
        s->prefs.code_apply_live = value == 0;
        if (s->prefs.code_apply_live) {
            s->scene->source_preview_hold[0] = 0;
        }
    } else if (!strcmp(action, "save") && asset) {
        asset_code *code = asset_editor(s, asset);
        if (code) {
            save_asset(s, code);
        }
    } else if (!strcmp(action, "back") || !strcmp(action, "forward")) {
        studio_nav_step(s, !strcmp(action, "back") ? -1 : 1);
    } else if (!strcmp(action, "part") && tab && !tab->ui->stylesheet) {
        tab->ui->part = value ? UI_PART_RCSS : UI_PART_RML;
    } else if (!strcmp(action, "find") && (tab || asset)) {
        open_find(&s->code);
    }
}

void studio_code_header(studio *s, int area)
{
    studio_ui_area_controls(s, area, "code", "CodeHeader");
}

/* The editor shows its part as the buffer holds it, the text they then
   share; after an undo or redo from the editor, its cursor lands on the
   change. */
static void follow_text(studio_code *c, studio_code_editor *shown, const char *text)
{
    if (ui_code_follow(shown->editor, text, c->replayed)) {
        c->replayed = false;
    }
    if (strcmp(shown->synced, text)) {
        free(shown->synced);
        shown->synced = xstrdup(text);
    }
}

bool studio_code_collab_apply(
    studio *s,
    const char *relative_path,
    ui_part part,
    const text_edit *edits,
    int count,
    uint64_t base_revision,
    const char *history_op,
    bool history
)
{
    if (!s || !relative_path || relative_path[0] == '/' ||
        base_revision != collab_revision() || part < 0 || part >= UI_PART_COUNT) {
        return false;
    }
    for (const char *p = relative_path; *p;) {
        const char *end = strchr(p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (!length || (length == 1 && p[0] == '.') ||
            (length == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        p = end ? end + 1 : p + length;
    }
    char path[2048];
    int length = snprintf(path, sizeof path, "%s/%s", s->project_root, relative_path);
    if (length < 0 || (size_t)length >= sizeof path) {
        return false;
    }
    scene_document_tab *tab = ui_tab(s, path);
    if (!tab || !tab->ui) {
        return false;
    }

    if (history_op && strcmp(history_op, "type") && strcmp(history_op, "step") &&
        strcmp(history_op, "undo") && strcmp(history_op, "redo")) {
        return false;
    }
    ui_buffer *b = tab->ui;
    bool own_echo = history && collab_apply_client() && collab_client() &&
                    !strcmp(collab_apply_client(), collab_client());
    if (own_echo && history_op &&
        (!strcmp(history_op, "undo") || !strcmp(history_op, "redo"))) {
        bool redo = !strcmp(history_op, "redo");
        if (!ui_buffer_history_edit_matches(b, redo, part, edits, count)) {
            return false;
        }
        if (redo) {
            ui_buffer_redo(b);
        } else {
            ui_buffer_undo(b);
        }
        s->code.replayed = true;
    } else if (history && history_op && !strcmp(history_op, "type")) {
        if (!ui_buffer_collab_type(b, part, edits, count, s->time)) {
            return false;
        }
    } else if (!ui_buffer_collab_edit(b, part, edits, count, history)) {
        return false;
    }

    for (int i = 0; i < acount(s->code.editors); i++) {
        studio_code_editor *editor = &s->code.editors[i];
        if (editor->part == part && !strcmp(editor->path, tab->path)) {
            follow_text(&s->code, editor, ui_buffer_text(b, part));
        }
    }
    return true;
}

/* ---------- Language ---------- */

/* The scan of the project around `path`, made again once stale; `keep`
   serves the last scan of the same project however old, as completion does
   between keystrokes. */
static const ui_source_project *project(studio *s, const char *path, bool keep)
{
    studio_code *c = &s->code;
    char root[1024];
    if (!ui_code_content_root(path, root, sizeof root)) {
        root[0] = '\0';
    }
    bool fresh = !strcmp(root, c->project_root) &&
                 (keep || s->time - c->project_time < PROJECT_STALE);
    if (!fresh) {
        ui_source_project_free(&c->project);
        if (root[0]) {
            ui_code_scan(&c->project, root);
        }
        snprintf(c->project_root, sizeof c->project_root, "%s", root);
        c->project_time = s->time;
    }
    return &c->project;
}

/* Parses the editor's text as its part's language when either changed, and
   highlights it with the parse's spans; true when it parsed. */
static bool parse_part(studio_code *c, kit_code_editor *e, ui_part part)
{
    ui_source_language language = ui_code_language(part);
    const char *text = kit_code_text(e, NULL);
    if (c->source && c->source->language == language && !strcmp(c->source->text, text)) {
        return false;
    }
    ui_source_free(c->source);
    c->source = ui_source_parse(text, language);
    asetlen(c->spans, 0);
    for (int i = 0; i < acount(c->source->spans); i++) {
        const ui_source_span *span = &c->source->spans[i];
        const char *cls = ui_source_kind_class(span->kind);
        if (cls) {
            apush(c->spans, ((kit_code_span){ span->start, span->end, cls }));
        }
    }
    return true;
}

/* Each part's problems: its syntax errors and, for the document RmlUi last
   checked, RmlUi's warnings about the part. The shown part is the editor's
   text, parsed; the other, the buffer's. */
static void mark_problems(studio_code *c, const scene_document_tab *tab, ui_part shown)
{
    bool checked = !strcmp(c->checked_path, tab->path);
    for (int p = 0; p < UI_PART_COUNT; p++) {
        const char *text = ui_buffer_text(tab->ui, (ui_part)p);
        ui_source *other = p != (int)shown && text
                             ? ui_source_parse(text, ui_code_language((ui_part)p))
                             : NULL;
        ui_code_markers_free(c->markers[p]);
        c->markers[p] = NULL;
        if (p == (int)shown || other) {
            ui_code_markers(
                p == (int)shown ? c->source : other,
                (ui_part)p,
                checked ? c->checked : NULL,
                checked ? c->check_failure : NULL,
                &c->markers[p]
            );
        }
        ui_source_free(other);
    }
    c->markers_stale = false;
}

/* What the pointer rests on, described once per place. */
static void describe_hover(studio *s, const ui_buffer *b, kit_code_editor *e)
{
    studio_code *c = &s->code;
    int at = kit_code_hover(e);
    if (at == c->hover_at && c->source == c->hover_source) {
        return;
    }
    free(c->hover);
    c->hover = at >= 0 ? ui_code_hover(c->source, at, project(s, b->path, false))
                       : NULL;
    c->hover_at = at;
    c->hover_source = c->source;
}

/* Parse, problems and hover for the part the editor shows. */
static void understand(studio *s, const scene_document_tab *tab, kit_code_editor *e, ui_part part)
{
    studio_code *c = &s->code;
    if (parse_part(c, e, part) || c->markers_stale) {
        mark_problems(c, tab, part);
    }
    describe_hover(s, tab->ui, e);
}

/* The completions at the cursor; the list closes where there are none, or
   where nothing was typed that asked for them. */
static void complete(studio *s, const ui_buffer *b, kit_code_editor *e, ui_part part, bool asked)
{
    studio_code *c = &s->code;
    parse_part(c, e, part);
    ui_code_completions_free(&c->completions);
    int cursor = kit_code_cursor(e);
    ui_code_complete(c->source, cursor, project(s, b->path, !asked), &c->completions);
    int typed = cursor - c->completions.start, count = acount(c->completions.items);
    bool done = count == 1 && (int)strlen(c->completions.items[0].label) == typed &&
                !strncmp(
                    c->source->text + c->completions.start,
                    c->completions.items[0].label,
                    (size_t)typed
                );
    c->completing = count > 0 && (asked || typed > 0) && !done;
}

/* A typed character that starts completion: a word's first, or a trigger. */
static bool starts_completion(const char *text, ui_code_change d)
{
    if (d.end != d.start || d.inserted != 1) {
        return false;
    }
    char typed = text[d.start];
    return isalnum((unsigned char)typed) || typed == '-' ||
           strchr(COMPLETION_TRIGGERS, typed);
}

/* ---------- Navigation ---------- */

/* A jump records where it left and where it landed, for Back and Forward. */
static void go_to_definition(studio *s, const ui_buffer *b, kit_code_editor *e)
{
    studio_code *c = &s->code;
    parse_part(c, e, b->part);
    int at = kit_code_cursor(e);
    ui_code_place place;
    if (ui_code_definition(b, c->source, at, project(s, b->path, false), &place)) {
        ui_code_place from = { .part = b->part, .offset = at, .line = 1 };
        snprintf(from.path, sizeof from.path, "%s", b->path);
        for (int i = 0; i < at; i++) {
            from.line += c->source->text[i] == '\n';
        }
        if (studio_code_open_place(s, &place)) {
            studio_nav_jump(s, &from, &place);
        }
        return;
    }
    bool named = ui_source_symbol_at(c->source, at).kind != UI_SYMBOL_NONE;
    kit_code_notice(e, named ? "No other definition" : "No symbol to go to here");
}

static const char *symbol_title(const ui_source_symbol *symbol, char *out, size_t size)
{
    static const char *const KINDS[] = {
        [UI_SYMBOL_NONE] = "",          [UI_SYMBOL_WIDGET] = "Widget ",
        [UI_SYMBOL_VARIABLE] = "",      [UI_SYMBOL_CLASS] = "Class .",
        [UI_SYMBOL_SAVE_KEY] = "save.",
    };
    snprintf(out, size, "%s%s", KINDS[symbol->kind], symbol->name);
    return out;
}

/* A Find row for a place: its file and line. */
static studio_reference place_row(const ui_code_place *place)
{
    studio_reference row = { .kind = REFERENCE_ASSET_UI,
                             .use = place->definition ? "Define" : "Use",
                             .ui = *place };
    char *file = ui_buffer_part_file(place->path, place->part);
    snprintf(row.asset, sizeof row.asset, "%s", content_basename(file));
    snprintf(row.place, sizeof row.place, "line %d", place->line);
    free(file);
    return row;
}

static void find_references(studio *s, const ui_buffer *b, kit_code_editor *e)
{
    studio_code *c = &s->code;
    parse_part(c, e, b->part);
    ui_code_place *places = NULL;
    ui_source_symbol symbol = ui_code_places(
        b,
        c->source,
        kit_code_cursor(e),
        project(s, b->path, false),
        &places
    );
    if (symbol.kind == UI_SYMBOL_NONE) {
        kit_code_notice(e, "No symbol to find here");
        return;
    }
    asetlen(s->references, 0);
    for (int i = 0; i < acount(places); i++) {
        apush(s->references, place_row(&places[i]));
    }
    afree(places);
    symbol_title(&symbol, s->references_title, sizeof s->references_title);
    studio_output_find(s);
}

/* ---------- Body ---------- */

/* Puts the cursor where studio_open_ui_part or a place asked, once laid
   out, and focuses the editor. Call right after kit_code. */
static void reveal(studio *s, kit_code_editor *e, const ui_buffer *b)
{
    if (!s->code.reveal || s->code.reveal_wait-- > 0) {
        return;
    }
    const char *text = ui_buffer_text(b, b->part);
    int at = s->code.reveal_offset >= 0 ? s->code.reveal_offset
                                        : line_offset(text, s->code.reveal);
    kit_code_set_cursor(e, math_min(at, (int)strlen(text)));
    kit_focus(s->kit);
    s->code.reveal = 0;
    s->code.reveal_offset = -1;
    if (s->code.reveal_notice[0]) {
        kit_code_notice(e, s->code.reveal_notice);
        s->code.reveal_notice[0] = '\0';
    }
}

/* ---------- Find (VS Code's find widget) ---------- */

/* Where the match of `query` after the one at `at` (the first, for -1)
   starts, or -1 past the last. Each starts after the last one ends, as
   VS Code counts them, so Find steps through exactly the matches it
   counts. Case is ignored, as VS Code's default. */
static int following_match(const char *text, const char *query, int at)
{
    size_t size = strlen(query);
    for (const char *p = at < 0 ? text : text + at + size; size && *p; p++) {
        if (!strncasecmp(p, query, size)) {
            return (int)(p - text);
        }
    }
    return -1;
}

/* Where the first match at or after `from` starts, wrapping past the end,
   or -1 without one. */
static int next_match(const char *text, const char *query, int from)
{
    int first = following_match(text, query, -1);
    for (int at = first; at >= 0; at = following_match(text, query, at)) {
        if (at >= from) {
            return at;
        }
    }
    return first;
}

/* Where the last match before `from` starts, wrapping past the start. */
static int previous_match(const char *text, const char *query, int from)
{
    int last = -1, before = -1;
    for (int at = following_match(text, query, -1); at >= 0;
         at = following_match(text, query, at)) {
        before = at < from ? at : before;
        last = at;
    }
    return before >= 0 ? before : last;
}

/* Counts the matches and places the shown one among them. */
static void count_matches(studio_code *c, const char *text)
{
    c->find.count = 0;
    c->find.index = -1;
    for (int at = following_match(text, c->find.query, -1); at >= 0;
         at = following_match(text, c->find.query, at)) {
        c->find.index = at == c->find.at ? c->find.count : c->find.index;
        c->find.count++;
    }
}

static void show_match(studio_code *c, kit_code_editor *e, int at)
{
    c->find.at = at;
    if (at >= 0) {
        kit_code_select(e, at, at + (int)strlen(c->find.query));
        kit_code_post(e, KIT_CODE_CURSOR); /* as move_caret */
    }
}

static void step_find(studio_code *c, kit_code_editor *e, const char *text, int direction)
{
    if (!c->find.query[0]) {
        return;
    }
    int from = c->find.at >= 0 ? c->find.at : kit_code_cursor(e);
    show_match(
        c,
        e,
        direction > 0 ? next_match(text, c->find.query, from + (c->find.at >= 0))
                      : previous_match(text, c->find.query, from)
    );
}

/* Find starts from a selection within one line, as VS Code seeds its
   query. */
static void open_find(studio_code *c)
{
    c->find.open = c->find.focus = true;
    c->find.at = -1;
    int start, end;
    if (!c->editor || !kit_code_selection(c->editor, &start, &end) ||
        end <= start || (size_t)(end - start) >= sizeof c->find.query) {
        return;
    }
    const char *text = kit_code_text(c->editor, NULL);
    if (memchr(text + start, '\n', (size_t)(end - start))) {
        return;
    }
    snprintf(c->find.query, sizeof c->find.query, "%.*s", end - start, text + start);
    snprintf(c->find.seen, sizeof c->find.seen, "%s", c->find.query);
    c->find.at = start;
}

static void close_find(studio_code *c)
{
    c->find.open = false;
    c->find.refocus = true;
}

/* A new query shows its first match from the shown one or the cursor, so
   typing narrows in place; the count follows the text. */
static void follow_query(studio_code *c, kit_code_editor *e)
{
    const char *text = kit_code_text(e, NULL);
    if (strcmp(c->find.seen, c->find.query)) {
        snprintf(c->find.seen, sizeof c->find.seen, "%s", c->find.query);
        int from = c->find.at >= 0 ? c->find.at : kit_code_cursor(e);
        show_match(c, e, c->find.query[0] ? next_match(text, c->find.query, from) : -1);
    }
    count_matches(c, text);
}

void studio_code_find_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *f = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "code_find");
    yyjson_mut_obj_add_strcpy(doc, f, "query", "");
    yyjson_mut_obj_add_strcpy(doc, f, "count", "");
    yyjson_mut_obj_add_int(doc, f, "total", 0);
}

/* The query is view state the field binds; C keeps it too, so the matches
   follow it and it outlasts the field. */
bool studio_code_find_sync(studio *s, const studio_panel_sync *at)
{
    studio_code *c = &s->code;
    if (!c->find.open) {
        return false;
    }
    yyjson_mut_val *query = yyjson_mut_obj_get(at->variable, "query");
    if (at->wrote && !at->preview && strcmp(yyjson_mut_get_str(query), c->find.query)) {
        snprintf(c->find.query, sizeof c->find.query, "%s", yyjson_mut_get_str(query));
    }
    if (c->editor && !at->preview) {
        follow_query(c, c->editor);
    }
    char label[32] = "";
    if (c->find.query[0] && !c->find.count) {
        snprintf(label, sizeof label, "No results");
    } else if (c->find.index >= 0) {
        snprintf(label, sizeof label, "%d/%d", c->find.index + 1, c->find.count);
    } else if (c->find.count) {
        snprintf(label, sizeof label, "%d", c->find.count);
    }
    bool changed = ui_data_put_text(at->doc, at->variable, "query", c->find.query);
    changed |= ui_data_put_text(at->doc, at->variable, "count", label);
    return ui_data_put_int(at->variable, "total", c->find.count) || changed;
}

/* Enter and Down show the next match, Shift+Enter and Up the previous one,
   and Escape closes the widget, returning to the text. */
void studio_code_find_action(studio *s, const char *action, const ui_activation *a)
{
    studio_code *c = &s->code;
    kit_code_editor *e = c->editor;
    if (!c->find.open || !e) {
        return;
    }
    const char *text = kit_code_text(e, NULL);
    bool shift = a->modifiers & RML_KM_SHIFT;
    if (!strcmp(action, "next") || !strcmp(action, "previous")) {
        step_find(c, e, text, !strcmp(action, "next") ? 1 : -1);
    } else if (!strcmp(action, "enter")) {
        step_find(c, e, text, shift ? -1 : 1);
    } else if (!strcmp(action, "close")) {
        close_find(c);
    }
}

/* The widget over the text's top right (CodeFind.rml). Find (Mod+F) focuses
   its field, selecting the query, once the field can take focus. */
static void find_widget(studio *s)
{
    studio_code *c = &s->code;
    kit *k = s->kit;
    kit_push(k, "find", NULL, "find-widget");
    rml_element *host = studio_ui_panel(s, "bar", "CodeFind");
    kit_pop(k);
    rml_element *field = host && c->find.focus
                           ? rml_element_by_id(host, "code-find-query")
                           : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        c->find.focus = false;
    }
}

/* A chord the editor leaves to Studio runs the command it names, as the
   shortcuts would if typing had not turned them off; Mod+[ and Mod+] step
   Back and Forward, as in JetBrains' editors, and Find finds here. */
static int chord_command(const studio *s, const char *chord)
{
    studio_key key;
    if (!studio_chord_parse(chord, s->mac, &key)) {
        return -1;
    }
    return studio_key_command(s, key, STUDIO_EDITOR_CODE);
}

/* F3 and Shift+F3 step through the last query's matches from the text,
   or open Find without one. */
static void find_again(studio_code *c, int direction)
{
    if (!c->find.query[0]) {
        open_find(c);
        return;
    }
    int start, end;
    if (!kit_code_selection(c->editor, &start, &end) || start != c->find.at) {
        c->find.at = -1;
    }
    const char *text = kit_code_text(c->editor, NULL);
    step_find(c, c->editor, text, direction);
    count_matches(c, text);
}

/* F3 and Shift+F3 find again from Find's field too, whose data-keys bind
   no function key. */
bool studio_code_find_key(studio *s, studio_key key)
{
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    if (key.key != SDLK_F3 ||
        (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) ||
        !s->code.find.open || !s->code.editor || !focus ||
        strcmp(rml_element_id(focus), "code-find-query")) {
        return false;
    }
    find_again(&s->code, key.mod & SDL_KMOD_SHIFT ? -1 : 1);
    return true;
}

static bool language_command(int command)
{
    return command >= STUDIO_COMMAND_GO_TO_DEFINITION &&
           command <= STUDIO_COMMAND_TOGGLE_LINE_COMMENT;
}

/* A language command's key in the text always answers: where it cannot
   act, the card at the caret says why. */
static void run_chord(studio *s, const char *chord)
{
    if (!strcmp(chord, "Mod+[") || !strcmp(chord, "Mod+]")) {
        studio_nav_step(s, chord[4] == ']' ? 1 : -1);
        return;
    }
    if (!strcmp(chord, "F3") || !strcmp(chord, "Shift+F3")) {
        find_again(&s->code, chord[0] == 'S' ? -1 : 1);
        return;
    }
    int command = chord_command(s, chord);
    if (command == STUDIO_COMMAND_FIND) {
        open_find(&s->code);
    } else if (language_command(command)) {
        studio_code_command(s, command);
    } else if (command >= 0) {
        studio_command_run(s, command);
    }
}

/* ---------- Language commands ---------- */

/* The symbol at an asset's caret: one with a definition, which Rename
   renames, or a callable, whose calls Call Hierarchy shows. */
static bool asset_symbol(asset_code *code, bool callable)
{
    const authoring_reference
        *reference = source_reference(code, kit_code_cursor(asset_code_editor(code)));
    const authoring_navigation *navigation = asset_code_navigation(code);
    if (!reference) {
        return false;
    }
    return callable
             ? authoring_navigation_callable(navigation, reference->target).id != 0
             : authoring_navigation_definition(navigation, reference->target) != NULL;
}

static int problem_count(studio *s, asset_code *code)
{
    if (code) {
        return acount(asset_code_markers(code));
    }
    int count = 0;
    for (int p = 0; p < UI_PART_COUNT; p++) {
        count += acount(s->code.markers[p]);
    }
    return count;
}

/* The editor the Text Editor shows, and its asset's source when it shows
   one, or NULL. */
static kit_code_editor *language_editor(studio *s, asset_code **code)
{
    scene_document_tab *asset = active_asset(s);
    *code = asset ? asset_editor(s, asset) : NULL;
    if (asset) {
        return *code ? asset_code_editor(*code) : NULL;
    }
    return shown(s) ? s->code.editor : NULL;
}

bool studio_code_command_enabled(studio *s, studio_command command)
{
    asset_code *code;
    if (!language_editor(s, &code) || !studio_editor_shown(s, STUDIO_EDITOR_CODE)) {
        return false;
    }
    switch (command) {
        case STUDIO_COMMAND_RENAME_SYMBOL:
        case STUDIO_COMMAND_CALL_HIERARCHY:
            return code &&
                   asset_symbol(code, command == STUDIO_COMMAND_CALL_HIERARCHY);
        case STUDIO_COMMAND_NEXT_PROBLEM:
        case STUDIO_COMMAND_PREVIOUS_PROBLEM: return problem_count(s, code) > 0;
        default: return true;
    }
}

/* Comments the selected lines: // in sources and KDL, block comments in
   RML and RCSS, each line or the lines as one block. */
static void toggle_comment(studio *s, kit_code_editor *e, asset_code *code)
{
    if (code) {
        kit_code_toggle_comment(e, "//", NULL, NULL);
        return;
    }
    bool rml = s->code.editor_part == UI_PART_RML;
    kit_code_toggle_comment(e, NULL, rml ? "<!--" : "/*", rml ? "-->" : "*/");
}

/* Navigation goes to the editor as the request its keys used to make, so
   the next frame answers it with the editor's own text and caret. */
void studio_code_command(studio *s, studio_command command)
{
    asset_code *code;
    kit_code_editor *e = language_editor(s, &code);
    if (!e) {
        return;
    }
    switch (command) {
        case STUDIO_COMMAND_GO_TO_DEFINITION:
            kit_code_post(e, KIT_CODE_DEFINITION);
            break;
        case STUDIO_COMMAND_FIND_REFERENCES:
            kit_code_post(e, KIT_CODE_REFERENCES);
            break;
        case STUDIO_COMMAND_RENAME_SYMBOL:
            kit_code_post(e, KIT_CODE_RENAME);
            break;
        case STUDIO_COMMAND_CALL_HIERARCHY:
            kit_code_post(e, KIT_CODE_CALLS);
            break;
        case STUDIO_COMMAND_NEXT_PROBLEM:
        case STUDIO_COMMAND_PREVIOUS_PROBLEM: {
            int direction = command == STUDIO_COMMAND_NEXT_PROBLEM ? 1 : -1;
            if (code) {
                step_asset_problem(&s->code, code, direction);
            } else {
                step_ui_problem(s, shown(s)->ui, direction);
            }
            break;
        }
        case STUDIO_COMMAND_TOGGLE_LINE_COMMENT:
            toggle_comment(s, e, code);
            break;
        default: assert(!"a Text Editor command"); break;
    }
    s->code.menu.open = false;
    s->code.find.refocus = true;
    s->ui_schedule.wake = true;
}

/* ---------- Context menu ---------- */

/* The text's secondary click opens its menu at the pointer (CodeMenu.rml),
   its items the commands above; its document lives while it shows. */
static void code_menu(studio *s)
{
    studio_code *c = &s->code;
    if (!c->menu.open) {
        if (c->menu.mounted) {
            studio_ui_panel_detach(s, "CodeMenu");
            c->menu.mounted = false;
        }
        return;
    }
    kit_document_begin(s->kit, "code-menu", "bar-place", true);
    rml_element *host = studio_ui_panel(s, "code-menu", "CodeMenu");
    kit_document_end(s->kit);
    c->menu.mounted = true;
    rml_element *popover = host ? rml_element_by_id(host, "code-menu") : NULL;
    if (popover && c->menu.opening) {
        /* The menu reads its keys wherever the focus is, but a focused
           text would also type the letters its typeahead reads, so the text
           lets go of the focus, as the graph's menu has it. */
        kit_blur(s->kit);
        float ratio = studio_pixel_ratio(s);
        popover_show(
            popover,
            &(popover_anchor){
                .side = POPOVER_POINT,
                .box = { s->in.mouse_x * ratio, s->in.mouse_y * ratio, 0, 0 } }
        );
        c->menu.opening = false;
    } else if (popover && !popover_open(popover)) {
        /* Closed without a choice. Opening left the focus on the text's
           editor; while it is still there, nowhere or in the menu, as after
           Escape, it returns to the text, as a kit popup's returns to its
           trigger. A press on another control keeps the focus it took, and
           a choice refocuses the text as it runs. */
        c->menu.open = false;
        rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
        c->find.refocus |= !focus ||
                           rml_element_owner_document(focus) ==
                               rml_element_owner_document(popover) ||
                           rml_element_query_selector(focus, "kit-code");
    }
}

static void open_menu(studio_code *c)
{
    c->menu.open = c->menu.opening = true;
}

/* Opening shows the bar at the next UI frame, whatever asked for it. */
void studio_code_find(studio *s)
{
    open_find(&s->code);
    s->ui_schedule.wake = true;
}

/* The editor's text into the buffer, the undo keys through it, and the
   buffer back to the editor: one history. Typing composes with what
   changed the buffer since they last shared its text, as a designer edit
   in the same frame. Returns what typing changed. */
static bool project_relative_path(const studio *s, const char *path, char *relative, size_t size)
{
    size_t root = strlen(s->project_root);
    size_t path_length = strlen(path);
    if (path_length <= root || strncmp(path, s->project_root, root) ||
        path[root] != '/') {
        return false;
    }
    const char *inside = path + root + 1;
    if (!inside[0] || strlen(inside) >= size) {
        return false;
    }
    snprintf(relative, size, "%s", inside);
    return true;
}

static char *text_action(
    const studio *s,
    const ui_buffer *b,
    ui_part part,
    const text_edit *edit,
    const char *history_op
)
{
    char path[1024];
    if (part < 0 || part >= UI_PART_COUNT ||
        !project_relative_path(s, b->path, path, sizeof path)) {
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "family", "text");
    yyjson_mut_obj_add_strcpy(doc, root, "path", path);
    yyjson_mut_obj_add_strcpy(doc, root, "part", PART_NAMES[part]);
    yyjson_mut_obj_add_uint(doc, root, "base_revision", collab_revision());
    if (history_op) {
        yyjson_mut_obj_add_strcpy(doc, root, "history_op", history_op);
    }
    yyjson_mut_val *edits = yyjson_mut_arr(doc);
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, edits);
    yyjson_mut_obj_add_int(doc, item, "start", edit->start);
    yyjson_mut_obj_add_int(doc, item, "end", edit->end);
    yyjson_mut_obj_add_strcpy(doc, item, "text", edit->text);
    yyjson_mut_obj_add_val(doc, root, "edits", edits);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static collab_action_gate text_action_begin(
    studio *s,
    ui_buffer *b,
    ui_part part,
    const text_edit *edit,
    const char *history_op,
    char **action
)
{
    *action = NULL;
    if (!collab_active() || collab_replaying()) {
        return COLLAB_ACTION_APPLY;
    }
    *action = text_action(s, b, part, edit, history_op);
    if (!*action) {
        return COLLAB_ACTION_REJECT;
    }
    return collab_actions_begin(*action);
}

/* The user's text reaches the buffer only after a proposal has been sent.
   An editor participant's buffer catches up from the accepted echo. */
static bool take_edits(studio *s, ui_buffer *b, studio_code_editor *shown, ui_part part, unsigned events)
{
    studio_code *c = &s->code;
    c->replayed = false;
    bool completed = events & (KIT_CODE_COMPLETED | KIT_CODE_STEP);
    bool typed_completion = false;
    if (events & (KIT_CODE_CHANGED | KIT_CODE_COMPLETED)) {
        text_edit edit;
        ui_code_change change;
        if (ui_code_prepare(b, part, shown->editor, shown->synced, &change, &edit)) {
            char *action;
            const char *history_op = completed ? "step" : "type";
            collab_action_gate gate = text_action_begin(s, b, part, &edit, history_op, &action);
            if (gate == COLLAB_ACTION_APPLY) {
                /* The diff addresses the editor before rebasing onto the buffer.
                   Following a shorter external edit below moves those bytes. */
                typed_completion = !completed &&
                                   starts_completion(kit_code_text(shown->editor, NULL), change);
                ui_code_record(b, part, shown->editor, shown->synced, s->time, completed);
                if (action) {
                    (void)collab_actions_finish(action, true);
                }
            } else {
                follow_text(c, shown, ui_buffer_text(b, part));
            }
            free(action);
            free((char *)edit.text);
        }
    }
    bool redo = events & KIT_CODE_REDO;
    if (events & (KIT_CODE_UNDO | KIT_CODE_REDO)) {
        ui_part target_part;
        const char *target;
        if (ui_buffer_history_target(b, redo, &target_part, &target)) {
            text_edit edit;
            if (ui_code_text_diff(ui_buffer_text(b, target_part), target, &edit)) {
                const char *history_op = redo ? "redo" : "undo";
                bool shareable = !collab_active() || collab_replaying() ||
                                 ui_buffer_history_edit_matches(b, redo, target_part, &edit, 1);
                if (shareable) {
                    char *action;
                    collab_action_gate gate = text_action_begin(s, b, target_part, &edit, history_op, &action);
                    if (gate == COLLAB_ACTION_APPLY && ui_code_replay(b, redo)) {
                        c->replayed = true;
                        if (action) {
                            (void)collab_actions_finish(action, true);
                        }
                        studio_report(s, redo ? "Redo" : "Undo");
                    } else if (gate != COLLAB_ACTION_APPLY) {
                        follow_text(c, shown, ui_buffer_text(b, part));
                    }
                    free(action);
                } else {
                    studio_report(s, "This text history step changes multiple parts");
                    follow_text(c, shown, ui_buffer_text(b, part));
                }
                free((char *)edit.text);
            }
        }
    }
    follow_text(c, shown, ui_buffer_text(b, part));
    return typed_completion;
}

/* Completion starts where typing or Control+Space asks, follows the text
   and the cursor while it runs, and ends with an accepted item, Escape or
   an undo. */
static void follow_completion(
    studio *s,
    const ui_buffer *b,
    kit_code_editor *e,
    ui_part part,
    unsigned events,
    bool typed_completion
)
{
    studio_code *c = &s->code;
    bool asked = (events & KIT_CODE_SUGGEST) || typed_completion;
    if (events &
        (KIT_CODE_COMPLETED | KIT_CODE_DISMISSED | KIT_CODE_UNDO | KIT_CODE_REDO)) {
        c->completing = false;
    } else if (asked || (c->completing && (events & (KIT_CODE_CHANGED | KIT_CODE_CURSOR)))) {
        complete(s, b, e, part, asked);
    }
}

static void respond(
    studio *s,
    scene_document_tab *tab,
    studio_code_editor *shown,
    ui_part part,
    const kit_code_result *r
)
{
    ui_buffer *b = tab->ui;
    kit_code_editor *e = shown->editor;
    unsigned events = r->events;
    /* Its text in focus makes the document active, so Save and Undo reach it. */
    if ((r->focused || (events & KIT_CODE_CHANGED)) && tab != active_ui(s)) {
        scene_tabs_activate(s->scene, (int)(tab - s->scene->tabs));
    }
    bool typed_completion = take_edits(s, b, shown, part, events);
    follow_completion(s, b, e, part, events, typed_completion);
    if (events & KIT_CODE_SAVED) {
        studio_save(s);
    }
    if (events & KIT_CODE_DEFINITION) {
        go_to_definition(s, b, e);
    }
    if (events & KIT_CODE_REFERENCES) {
        find_references(s, b, e);
    }
    if (events & KIT_CODE_RENAME) {
        kit_code_notice(e, "Rename isn't available in UI documents");
    }
    if (events & KIT_CODE_CALLS) {
        kit_code_notice(e, "Call hierarchy isn't available in UI documents");
    }
    if (events & KIT_CODE_MENU) {
        open_menu(&s->code);
    }
    if (events & KIT_CODE_CHORD) {
        run_chord(s, r->chord);
    }
}

/* With nothing open, the area says what it edits and offers Search, as the
   Behavior editor's empty state does. */
static void empty_state(studio *s)
{
    kit *k = s->kit;
    kit_push(k, "empty", NULL, "pending-editor behavior-empty");
    kit_icon(k, "icon", "glyph", KIT_ICON_EXPRESSION);
    kit_label(k, "title", "title", "No file open");
    kit_label(k, "hint", "dim", "Open a source, KDL asset, UI document or stylesheet from Assets");
    kit_push(k, "actions", NULL, "behavior-choices");
    kit_button_desc search = { .label = "Search",
                               .icon = KIT_ICON_SEARCH,
                               .shortcut = studio_chord(s, "Mod+K") };
    if (kit_button(k, "search", &search)) {
        studio_command_run(s, STUDIO_COMMAND_SEARCH);
    }
    kit_pop(k);
    kit_pop(k);
}

static ui_code_place asset_place(const asset_code *code, int offset)
{
    ui_code_place place = { .line = 1, .offset = offset };
    snprintf(place.path, sizeof place.path, "%s", asset_code_path(code));
    const char *text = kit_code_text(asset_code_editor((asset_code *)code), NULL);
    for (int i = 0; i < offset && text[i]; i++) {
        place.line += text[i] == '\n';
    }
    return place;
}

static void asset_calls(studio *s, asset_code *code, int cursor)
{
    studio_calls_source(s, asset_code_path(code), cursor, false);
}

static ui_code_place source_location(const authoring_navigation *index, authoring_location location)
{
    const authoring_document
        *document = authoring_navigation_document_at(index, location.document);
    ui_code_place place = { .line = 1, .offset = location.range.start };
    snprintf(place.path, sizeof place.path, "%s", document->path);
    const char *text = authoring_navigation_document_text(index, location.document);
    for (int i = 0; i < place.offset && text[i]; i++) {
        place.line += text[i] == '\n';
    }
    return place;
}

static bool asset_symbol_definition(studio *s, asset_code *code, int cursor)
{
    const authoring_navigation *index = asset_code_navigation(code);
    const authoring_reference *reference = source_reference(code, cursor);
    const authoring_reference
        *definition = reference
                        ? authoring_navigation_definition(index, reference->target)
                        : NULL;
    if (!definition) {
        return false;
    }
    ui_code_place from = asset_place(code, cursor),
                  to = source_location(index, definition->location);
    if (!studio_code_open_place(s, &to)) {
        return false;
    }
    studio_nav_jump(s, &from, &to);
    return true;
}

/* Output ▸ Find lists the uses of the symbol at the cursor, titled by its name. */
static void asset_references(studio *s, asset_code *code, int cursor)
{
    const authoring_navigation *index = asset_code_navigation(code);
    const authoring_reference *reference = source_reference(code, cursor);
    if (!reference) {
        kit_code_notice(asset_code_editor(code), "No symbol to find here");
        return;
    }
    asetlen(s->references, 0);
    for (int i = 0; i < authoring_navigation_reference_count(index); i++) {
        const authoring_reference *found = authoring_navigation_reference_at(index, i);
        if (found->kind == AUTHORING_DEFINE ||
            !authoring_target_equal(found->target, reference->target)) {
            continue;
        }
        ui_code_place place = source_location(index, found->location);
        apush(s->references, place_row(&place));
    }
    const char *name = authoring_navigation_name(index, reference->target);
    script_range range = reference->location.range;
    snprintf(
        s->references_title,
        sizeof s->references_title,
        "%.*s",
        name ? (int)strlen(name) : range.end - range.start,
        name ? name
             : authoring_navigation_document_text(index, reference->location.document) +
                   range.start
    );
    studio_output_find(s);
}

static void asset_requests(studio *s, asset_code *code, unsigned events)
{
    kit_code_editor *e = asset_code_editor(code);
    int cursor = kit_code_cursor(e), definition;
    if (events & KIT_CODE_RENAME) {
        studio_refactor_source(s, asset_code_path(code), cursor);
    }
    if (events & KIT_CODE_CALLS) {
        asset_calls(s, code, cursor);
    }
    if (events & KIT_CODE_DEFINITION) {
        if (asset_code_definition(code, cursor, &definition)) {
            ui_code_place from = asset_place(code, cursor),
                          to = asset_place(code, definition);
            move_caret(e, definition);
            studio_nav_jump(s, &from, &to);
        } else if (!asset_symbol_definition(s, code, cursor)) {
            kit_code_notice(e, "No definition found");
        }
    }
    if (events & KIT_CODE_REFERENCES) {
        asset_references(s, code, cursor);
    }
    if (events & KIT_CODE_MENU) {
        open_menu(&s->code);
    }
    if ((events & KIT_CODE_BREAKPOINT) && !asset_code_error(code)[0] &&
        !active_asset(s)->source_only) {
        asset_code_time(code, s->time + 0.2);
        sync_asset_graph(s, code);
        if (asset_code_error(code)[0]) {
            return;
        }
        uint64_t graph_id, node_id;
        if (asset_code_breakpoint_at(code, cursor, &graph_id, &node_id)) {
            editor *editor = s->scene->graph_editor;
            graph *g = behavior_asset_find_graph(editor->asset, graph_id);
            node *n = g ? graph_node_by_id(g, node_id) : NULL;
            if (n && node_behavior_for(n) != NODE_BEHAVIOR_PURE) {
                ed_snapshot_before(editor);
                graph_set_breakpoint(g, n, !n->breakpoint);
                sync_asset_graph(s, code);
                capture_asset(s, code);
            }
        }
    }
}

/* The symbol under the pointer while the primary modifier is held, when
   Go to Definition has somewhere else to go from it: Mod+click's
   underline, as the asset sources' (asset_code.c). */
static void link_symbol(studio *s, const ui_buffer *b, const kit_code_editor *e, kit_code_desc *desc)
{
    studio_code *c = &s->code;
    int at = kit_code_link(e);
    ui_source_symbol symbol = at >= 0 ? ui_source_symbol_at(c->source, at)
                                      : (ui_source_symbol){ .kind = UI_SYMBOL_NONE };
    ui_code_place place;
    if (symbol.kind != UI_SYMBOL_NONE &&
        ui_code_definition(b, c->source, at, project(s, b->path, false), &place)) {
        desc->link_start = symbol.start;
        desc->link_end = symbol.end;
    }
}

/* The language commands' chords, each command's first CHORDS_PER_COMMAND
   (studio_command_bindings). */
#define CHORDS_PER_COMMAND 5
#define CODE_CHORDS                                                               \
    ((STUDIO_COMMAND_TOGGLE_LINE_COMMENT - STUDIO_COMMAND_GO_TO_DEFINITION + 1) * \
     CHORDS_PER_COMMAND)
typedef struct code_chords
{
    char names[CODE_CHORDS][STUDIO_CHORD_SIZE];
    const char *list[CODE_CHORDS];
    int count;
} code_chords;

/* The language commands' chords as the text names them, which its keys
   send back to Studio before taking them (kit_code_desc.chords), so a
   command runs on whatever chord it is bound to. */
static void bound_chords(const studio *s, code_chords *out)
{
    out->count = 0;
    for (int command = STUDIO_COMMAND_GO_TO_DEFINITION; language_command(command);
         command++) {
        const char *bindings[CHORDS_PER_COMMAND];
        int count = studio_command_bindings(&s->prefs, (studio_command)command, bindings, CHORDS_PER_COMMAND);
        for (int i = 0; i < count; i++) {
            studio_key key;
            if (studio_chord_parse(bindings[i], s->mac, &key)) {
                studio_chord_name(key, s->mac, out->names[out->count]);
                out->list[out->count] = out->names[out->count];
                out->count++;
            }
        }
    }
}

void studio_code_body(studio *s, int)
{
    kit *k = s->kit;
    scene_document_tab *asset = active_asset(s);
    code_chords chords;
    bound_chords(s, &chords);
    if (asset) {
        asset_code *code = asset_editor(s, asset);
        if (!code) {
            empty_state(s);
            return;
        }
        kit_push(k, "code", NULL, "code-editor");
        kit_code_desc preferences = { .vim = s->prefs.code_vim,
                                      .font_size = s->prefs.code_font_size,
                                      .font_family = studio_code_font(&s->prefs),
                                      .mac = s->mac,
                                      .no_wrap = !s->prefs.code_wrap,
                                      .chords = chords.list,
                                      .chord_count = chords.count };
        const reference_index *index = app_scene_authoring_asset_index(s->scene);
        const char **names = NULL;
        for (int i = 0; index && i < reference_index_asset_count(index); i++) {
            const reference_asset *reference = reference_index_asset_at(index, i);
            if (reference->name) {
                apush(names, reference->name);
            }
        }
        asset_code_set_names(code, names, acount(names));
        afree(names);
        s->code.editor = asset_code_editor(code);
        snprintf(s->code.editor_path, sizeof s->code.editor_path, "%s", asset->path);
        bool paused_here = s->pause.active && s->scene->graph_document_id &&
                           !strcmp(s->pause.at.asset, s->scene->graph_document_id);
        asset_code_execution(
            code,
            paused_here ? s->pause.at.graph_id : 0,
            paused_here ? s->pause.at.node_id : 0
        );
        asset_code_time(code, s->time);
        kit_code_result
            r = asset_code_draw(code, k, "text", &preferences, s->prefs.code_apply_live);
        if (s->code.find.refocus) {
            kit_focus(k);
            s->code.find.refocus = false;
        }
        asset_requests(s, code, r.events);
        if (s->code.find.open) {
            find_widget(s);
        }
        code_menu(s);
        if (s->code.capture_due && strcmp(s->code.capture_path, asset->path)) {
            capture_caret(s);
        }
        if (r.events & (KIT_CODE_CHANGED | KIT_CODE_UNDO | KIT_CODE_REDO)) {
            capture_asset(s, code);
            s->code.capture_due = 0;
        } else if (r.events & KIT_CODE_CURSOR) {
            s->code.capture_due = s->time + CAPTURE_REST;
            snprintf(
                s->code.capture_path,
                sizeof s->code.capture_path,
                "%s",
                asset->path
            );
        }
        bool chord = r.events & KIT_CODE_CHORD;
        if ((r.events & KIT_CODE_SAVED) ||
            (chord && chord_command(s, r.chord) == STUDIO_COMMAND_SAVE)) {
            save_asset(s, code);
        }
        kit_pop(k);
        if (chord && chord_command(s, r.chord) != STUDIO_COMMAND_SAVE) {
            run_chord(s, r.chord);
        }
        return;
    }
    studio_code *c = &s->code;
    scene_document_tab *tab = shown(s);
    if (!tab) {
        c->editor = NULL;
        empty_state(s);
        return;
    }
    ui_buffer *b = tab->ui;
    kit_push(k, "code", NULL, "code-editor");
    ui_part part = b->part;
    studio_code_editor *shown_editor = part_editor(s, tab);
    kit_code_editor *e = shown_editor->editor;
    /* An editor coming into view catches up with its part; the one in view
       may hold typing that kit_code has yet to report. */
    if (e != c->editor) {
        follow_text(c, shown_editor, ui_buffer_text(b, part));
        c->editor = e;
        snprintf(c->editor_path, sizeof c->editor_path, "%s", tab->path);
        c->editor_part = part;
    }
    understand(s, tab, e, part);
    kit_code_desc desc = {
        .vim = s->prefs.code_vim,
        .font_size = s->prefs.code_font_size,
        .font_family = studio_code_font(&s->prefs),
        .mac = s->mac,
        .spans = c->spans,
        .span_count = acount(c->spans),
        .markers = c->markers[part],
        .marker_count = acount(c->markers[part]),
        .completions = c->completing ? c->completions.items : NULL,
        .completion_count = c->completing ? acount(c->completions.items) : 0,
        .completion_start = c->completions.start,
        .history = true,
        .no_wrap = !s->prefs.code_wrap,
        .hover = c->hover,
        .word_chars = "-", /* RML's and RCSS's names take hyphens */
        .chords = chords.list,
        .chord_count = chords.count,
    };
    link_symbol(s, b, e, &desc);
    kit_code_result r = kit_code(k, "text", e, &desc);
    if (c->find.refocus) {
        kit_focus(k);
        c->find.refocus = false;
    }
    reveal(s, e, b);
    if (c->find.open) {
        find_widget(s);
    }
    code_menu(s);
    respond(s, tab, shown_editor, part, &r);
    kit_pop(k);
}
