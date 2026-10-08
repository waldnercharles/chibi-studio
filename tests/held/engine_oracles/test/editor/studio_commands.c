#include "editor/documents/asset_code.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/input.h"
#include "graph/document/serialize.h"
#include "platform/cute.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"
#include "ui/rml.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* A camelCase name a data binding reads, as commands.<id>.enabled. */
static bool binding_name(const char *id)
{
    bool ok = id[0] >= 'a' && id[0] <= 'z';
    for (const char *c = id; *c; c++) {
        ok &= isalnum((unsigned char)*c) != 0;
    }
    return ok;
}

/* The member `field` of `commands.<id>` in a data model's JSON. */
static yyjson_mut_val *command_field(yyjson_mut_doc *doc, studio_command command, const char *field)
{
    yyjson_mut_val *commands = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "commands");
    return yyjson_mut_obj_get(yyjson_mut_obj_get(commands, studio_command_id(command)), field);
}

/* `commands` holds each command's state by id, and shows a change only when
   something differs. */
static void commands_data()
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    studio_commands_add(doc);
    studio_command_state states[STUDIO_COMMAND_COUNT];
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const char *id = studio_command_id((studio_command)c);
        states[c] = (studio_command_state){ .info = { .id = id, .label = id, .menu = id } };
    }
    states[STUDIO_COMMAND_PLAY].info = (studio_command_info){
        .label = "Pause",
        .menu = "Pause",
        .shortcut = "Space",
        .icon = KIT_ICON_PAUSE
    };
    states[STUDIO_COMMAND_SAVE].enabled = true;
    states[STUDIO_COMMAND_TOGGLE_ASSETS].checked = true;
    bool shown = studio_commands_show(doc, states);
    bool same = !studio_commands_show(doc, states);
    const char *pause = "\xf3\xb0\x8f\xa4"; /* KIT_ICON_PAUSE as UTF-8 */
    bool held = !strcmp(yyjson_mut_get_str(command_field(doc, STUDIO_COMMAND_PLAY, "label")), "Pause") &&
                !strcmp(yyjson_mut_get_str(command_field(doc, STUDIO_COMMAND_PLAY, "shortcut")), "Space") &&
                !strcmp(yyjson_mut_get_str(command_field(doc, STUDIO_COMMAND_PLAY, "icon")), pause) &&
                !strcmp(yyjson_mut_get_str(command_field(doc, STUDIO_COMMAND_STOP, "shortcut")), "") &&
                yyjson_mut_get_bool(command_field(doc, STUDIO_COMMAND_SAVE, "enabled")) &&
                !yyjson_mut_get_bool(command_field(doc, STUDIO_COMMAND_UNDO, "enabled")) &&
                yyjson_mut_get_bool(
                    command_field(doc, STUDIO_COMMAND_TOGGLE_ASSETS, "checked")
                );
    states[STUDIO_COMMAND_SAVE].enabled = false;
    bool changed = studio_commands_show(doc, states) &&
                   !yyjson_mut_get_bool(command_field(doc, STUDIO_COMMAND_SAVE, "enabled"));
    test_check(shown && same && held && changed, "the commands variable holds each command's label, shortcut, icon, enabled and checked by id");
    yyjson_mut_doc_free(doc);
}

/* Every command has an id that finds it again, so authored UI can run it by
 * emit('studio.<id>') and bind it as commands.<id>, and no two share one. */
void test_studio_commands()
{
    bool found = true, unique = true, bindable = true;
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        found &= studio_command_named(studio_command_id((studio_command)c)) == c;
        bindable &= binding_name(studio_command_id((studio_command)c));
        for (int d = 0; d < c; d++) {
            unique &= strcmp(
                          studio_command_id((studio_command)c),
                          studio_command_id((studio_command)d)
                      ) != 0;
        }
    }
    test_check(found && unique && studio_command_named("missing") < 0, "each Studio command has its own id that names it");
    test_check(bindable, "each Studio command's id is a camelCase name data bindings read");
    commands_data();
}

/* Zoom In and Zoom Out step UI Scale through the settings table's presets,
 * as the Settings page's select offers them, and Reset Zoom restores its
 * default. */
void test_studio_zoom()
{
    const studio_setting *setting = NULL;
    for (int i = 0; i < studio_setting_count; i++) {
        setting = !strcmp(studio_settings[i].key, "uiScale") ? &studio_settings[i]
                                                             : setting;
    }
    studio *s = xcalloc(1, sizeof *s);
    s->prefs.ui_scale = setting->min;
    bool stepped = true;
    for (int i = 1; i < setting->choice_count; i++) {
        studio_zoom(s, 1);
        stepped &= s->prefs.ui_scale == strtof(setting->choices[i].value, NULL);
    }
    studio_zoom(s, 1);
    bool top = s->prefs.ui_scale == setting->max;
    studio_zoom(s, -1);
    bool down = s->prefs.ui_scale ==
                strtof(setting->choices[setting->choice_count - 2].value, NULL);
    studio_zoom(s, 0);
    test_check(
        stepped && top && down && s->prefs.ui_scale == setting->fallback,
        "Zoom In and Out step UI Scale through the settings table's presets"
    );
    free(s);
}

/* A real SDL click runs through Studio's pointer tracking, unlike a direct
   command call, and catches menubar presses losing their document context. */
static void menu_frame(studio *s)
{
    cf_app_update(NULL);
    app_input_frame();
    editor_tick(sg_graph_editor(s), s->time + 1.0 / 60);
    studio_frame(s, 1.0 / 60);
    cf_app_draw_onto_screen(true);
}

/* The element `selector` matches whose .label reads `label`: an authored
   menu's trigger or item. */
static rml_element *labeled(studio *s, const char *selector, const char *label)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int d = 0; d < rml_context_document_count(context); d++) {
        rml_element *items[64];
        int count = rml_element_query_selector_all(rml_context_document(context, d), selector, items, 64);
        for (int i = 0; i < count && i < 64; i++) {
            rml_element *text = rml_element_query_selector(items[i], ".label");
            rml_element *node = text ? rml_element_first_child(text) : NULL;
            char found[64] = "";
            if (node && !strcmp(rml_element_tag_name(node), "#text")) {
                rml_element_text(node, found, sizeof found);
            }
            if (!strcmp(found, label)) {
                return items[i];
            }
        }
    }
    return NULL;
}

static bool menu_click(studio *s, rml_element *el)
{
    if (!el) {
        return false;
    }
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float scale = cf_app_get_pixel_scale();
    float x = (at.x + size.x / 2) / scale, y = (at.y + size.y / 2) / scale;
    SDL_Event move = { .type = SDL_EVENT_MOUSE_MOTION };
    move.motion.windowID = SDL_GetWindowID(cf_app_get_window());
    move.motion.x = x;
    move.motion.y = y;
    assert(SDL_PushEvent(&move));
    menu_frame(s);
    for (int down = 1; down >= 0; down--) {
        SDL_Event click = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                         : SDL_EVENT_MOUSE_BUTTON_UP };
        click.button.windowID = SDL_GetWindowID(cf_app_get_window());
        click.button.button = SDL_BUTTON_LEFT;
        click.button.down = down;
        click.button.clicks = 1;
        click.button.x = x;
        click.button.y = y;
        assert(SDL_PushEvent(&click));
        menu_frame(s);
    }
    return true;
}

static void save_context(studio *s, int area)
{
    int active = s->scene->active_tab, scene_tab = -1;
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        if (s->scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE) {
            scene_tab = i;
            break;
        }
    }
    assert(scene_tab >= 0);
    scene_tabs_activate(s->scene, scene_tab);
    game_preview_definition
        *definition = game_preview_find_definition(s->scene->live, s->scene->graph_document_id);
    assert(definition);
    bool dirty = definition->unsaved;
    bool scene_dirty = s->scene->scene_dirty;
    s->scene->scene_dirty = false;
    definition->unsaved = true;
    s->focus_area = area;
    bool graph_save = studio_command_enabled(s, STUDIO_COMMAND_SAVE);
    definition->unsaved = false;
    s->scene->scene_dirty = true;
    bool graph_clean = !studio_command_enabled(s, STUDIO_COMMAND_SAVE);
    s->focus_area = STUDIO_AREA_TOP;
    bool scene_save = studio_command_enabled(s, STUDIO_COMMAND_SAVE);
    test_check(
        graph_save && graph_clean && scene_save,
        "Save follows the focused behavior graph or scene even when their "
        "dirty states differ"
    );
    definition->unsaved = dirty;
    s->scene->scene_dirty = scene_dirty;
    scene_tabs_activate(s->scene, active);
    s->focus_area = area;
}

static void document_visibility(studio *s, int graph_area)
{
    int active = s->scene->active_tab;
    studio_layout layout = *studio_active_layout(s);
    studio_workspace workspace = s->prefs.workspace;
    studio_maximize_graph(s);
    menu_frame(s);
    char path[512];
    snprintf(path, sizeof path, "%s/ui/Bank.rml", s->scene->content_root);
    assert(scene_tabs_open_ui(s->scene, path) == 0);
    int ui_tab = s->scene->active_tab;
    menu_frame(s);
    menu_frame(s);
    bool revealed = s->maximized < 0 && studio_editor_shown(s, STUDIO_EDITOR_SCENE) &&
                    studio_previewed_ui(s) &&
                    !strcmp(studio_previewed_ui(s), "Bank");
    bool preserved = s->prefs.workspace == workspace &&
                     !memcmp(studio_active_layout(s), &layout, sizeof layout);
    studio_toggle_maximized(s, graph_area);
    menu_frame(s);
    bool deliberate = s->maximized == graph_area;
    assert(scene_tabs_activate(s->scene, active) == 0);
    menu_frame(s);
    bool compatible = s->maximized == graph_area;
    s->collaboration.following = true;
    assert(scene_tabs_activate(s->scene, ui_tab) == 0);
    menu_frame(s);
    bool remote = s->maximized == graph_area;
    s->collaboration.following = false;
    assert(scene_tabs_activate(s->scene, active) == 0);
    menu_frame(s);
    studio_toggle_maximized(s, graph_area);
    studio_toggle_maximized(s, STUDIO_AREA_TOP);
    assert(scene_tabs_activate(s->scene, ui_tab) == 0);
    menu_frame(s);
    bool canvas = s->maximized == STUDIO_AREA_TOP;
    test_check(
        revealed && preserved && deliberate && compatible && remote && canvas,
        "opening a UI document reveals its Canvas from a maximized graph, "
        "preserves split layouts and retains compatible, deliberate or "
        "followed maximize"
    );
    scene_tabs_close(s->scene, APP_EDITOR_DOCUMENT_UI, path);
    scene_tabs_activate(s->scene, active);
    *studio_active_layout(s) = layout;
    s->maximized = graph_area;
    s->focus_area = graph_area;
    menu_frame(s);
}

static void reload_context(studio *s, int area)
{
    int active = s->scene->active_tab, scene_tab = -1;
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        if (s->scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE) {
            scene_tab = i;
            break;
        }
    }
    assert(scene_tab >= 0);
    char path[512];
    snprintf(path, sizeof path, "%s/ui/Bank.rml", s->scene->content_root);
    assert(scene_tabs_open_ui(s->scene, path) == 0);
    scene_document_tab *tab = &s->scene->tabs[s->scene->active_tab];
    ui_buffer *buffer = tab->ui;
    char *saved = xstrdup(ui_buffer_text(buffer, UI_PART_RML));
    text_edit edit = { 0, 0, "<!-- unsaved reload regression -->" };
    ui_buffer_edit(buffer, UI_PART_RML, &edit, 1);
    studio_command_run(s, STUDIO_COMMAND_RELOAD);
    bool retained = ui_buffer_dirty(buffer) &&
                    strstr(ui_buffer_text(buffer, UI_PART_RML), "unsaved reload regression") &&
                    acount(s->disk_toasts) == 1;
    int toast = retained ? s->disk_toasts[0].toast : 0;
    scene_tabs_activate(s->scene, scene_tab);
    for (int i = 0; i < 20; i++) {
        menu_frame(s);
    }
    rml_context *context = rml_backend_context(s->backend);
    rml_element *action = NULL;
    char suffix[48];
    snprintf(suffix, sizeof suffix, "/t%d/action", toast);
    for (int d = 0; d < rml_context_document_count(context); d++) {
        rml_element *items[32];
        int count = rml_element_query_selector_all(
            rml_context_document(context, d),
            ".toast-action",
            items,
            32
        );
        for (int i = 0; i < count && i < 32; i++) {
            char element_path[256];
            if (kit_element_path(s->kit, items[i], element_path, sizeof element_path) &&
                strstr(element_path, suffix)) {
                action = items[i];
            }
        }
    }
    bool confirmed = action && menu_click(s, action);
    /* Toast buttons emit in kit_end, after studio_disk_frame consumed this
       frame's actions. Let the next editor frame apply the recorded click. */
    menu_frame(s);
    if (!(retained && confirmed && !ui_buffer_dirty(buffer) &&
          !strcmp(ui_buffer_text(buffer, UI_PART_RML), saved) &&
          s->scene->active_tab == scene_tab && !acount(s->disk_toasts))) {
        printf(
            "  Reload checks: retained %d, action %d, confirmed %d, dirty %d, "
            "restored %d, active %d/%d, pending %d\n",
            retained,
            action != NULL,
            confirmed,
            ui_buffer_dirty(buffer),
            !strcmp(ui_buffer_text(buffer, UI_PART_RML), saved),
            s->scene->active_tab,
            scene_tab,
            acount(s->disk_toasts)
        );
    }
    test_check(
        retained && confirmed && !ui_buffer_dirty(buffer) &&
            !strcmp(ui_buffer_text(buffer, UI_PART_RML), saved) &&
            s->scene->active_tab == scene_tab && !acount(s->disk_toasts),
        "Reload retains unsaved text until confirmation and reloads the "
        "requested document after switching tabs"
    );
    free(saved);
    scene_tabs_close(s->scene, APP_EDITOR_DOCUMENT_UI, path);
    scene_tabs_activate(s->scene, active);
    s->focus_area = area;
}

void test_studio_menu_context(studio *s)
{
    editor *e = sg_graph_editor(s);
    if (!editor_can_undo(e) && editor_can_redo(e)) {
        editor_redo(e);
    }
    assert(editor_can_undo(e));
    /* Edit's popup overlaps the Scene pane; it still acts on the graph that
       was clicked last, rather than the area geometrically beneath its row. */
    studio_set_workspace(s, STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
    *studio_active_layout(s) = studio_layout_default(STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
    studio_active_layout(s)->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_BEHAVIOR;
    s->maximized = -1;
    menu_frame(s);
    menu_frame(s);
    int area = s->graph.area;
    assert(area >= 0);
    s->focus_area = area;
    behavior_asset *before = ed_snapshot(e);
    bool opened = menu_click(s, labeled(s, "menubar .trigger", "Edit"));
    rml_element *undo = labeled(s, "#menu-edit .menu-item", "Undo");
    bool enabled = opened && undo &&
                   !rml_element_has_attribute(undo, "data-disabled") &&
                   s->focus_area == area &&
                   studio_command_enabled(s, STUDIO_COMMAND_UNDO);
    bool undo_clicked = enabled && menu_click(s, undo);
    behavior_asset *undone = ed_snapshot(e);
    bool changed = undo_clicked && !behavior_asset_equal(before, undone) &&
                   studio_command_enabled(s, STUDIO_COMMAND_REDO);
    opened = menu_click(s, labeled(s, "menubar .trigger", "Edit"));
    rml_element *redo = labeled(s, "#menu-edit .menu-item", "Redo");
    bool redo_enabled = opened && redo &&
                        !rml_element_has_attribute(redo, "data-disabled") &&
                        s->focus_area == area &&
                        studio_command_enabled(s, STUDIO_COMMAND_REDO);
    bool redo_clicked = redo_enabled && menu_click(s, redo);
    behavior_asset *restored = ed_snapshot(e);
    if (!enabled || !changed || !redo_enabled || !redo_clicked ||
        !behavior_asset_equal(before, restored)) {
        printf(
            "  Graph menu history: enabled=%d undo=%d changed=%d "
            "redo_enabled=%d redo=%d restored=%d\n",
            enabled,
            undo_clicked,
            changed,
            redo_enabled,
            redo_clicked,
            behavior_asset_equal(before, restored)
        );
        printf(
            "  Graph identity: working=%s document=%s active=%d "
            "source_sessions=%d\n",
            editor_behavior_asset(e)->name,
            s->scene->graph_document_id ? s->scene->graph_document_id : "(none)",
            s->scene->document.kind,
            acount(s->code.assets)
        );
        for (int i = 0; i < acount(s->code.assets); i++) {
            asset_code *code = s->code.assets[i];
            printf(
                "  Source history: %s undo=%d redo=%d pending=%d error=%s\n",
                asset_code_path(code),
                asset_code_can_replay(code, false),
                asset_code_can_replay(code, true),
                asset_code_pending(code),
                asset_code_error(code)
            );
        }
    }
    test_check(
        enabled && changed && redo_enabled && redo_clicked &&
            behavior_asset_equal(before, restored),
        "Edit menu preserves graph command context and applies Undo/Redo over "
        "the Scene pane"
    );
    behavior_asset_free(before);
    behavior_asset_free(undone);
    behavior_asset_free(restored);

    save_context(s, area);
    reload_context(s, area);
    document_visibility(s, area);
}
