#include "authoring/ui_loader.h"
/* Studio's settings table: the preferences file and the saved keys authored
 * panels bind go through it, and the Settings page's `settings` variable
 * shows and edits it. */
#include "editor/studio/studio_internal.h"
#include "render/ui.h"
#include "support/array.h"
#include "support/text_file.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <unistd.h>
#endif

#ifndef __EMSCRIPTEN__
static void prefs_file_override()
{
    const char *before = getenv("CHIBI_STUDIO_PREFS_PATH");
    char *previous = before ? xstrdup(before) : NULL;
    char path[] = "/tmp/chibi-studio-prefs-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && !close(fd));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputs("{\"theme\":\"dark\",\"uiScale\":1.5,\"future\":42}", file) >= 0);
    assert(!fclose(file));
    assert(!setenv("CHIBI_STUDIO_PREFS_PATH", path, 1));
    studio_prefs prefs = studio_prefs_load();
    bool loaded = prefs.theme == STUDIO_THEME_DARK && prefs.ui_scale == 1.5f;
    prefs.ui_scale = 2;
    prefs.workspace = STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    prefs.layouts[STUDIO_WORKSPACE_HORIZONTAL_SPLIT].side_open = false;
    bool saved = studio_prefs_save(&prefs);
    studio_prefs reloaded = studio_prefs_load();
    char *text = text_file_read(path);
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    bool retained = yyjson_get_int(yyjson_obj_get(yyjson_doc_get_root(doc), "future")) ==
                    42;
    test_check(loaded && saved && retained && studio_prefs_equal(&prefs, &reloaded), "an explicit preferences file owns load and save, preserving unrelated settings");
    yyjson_doc_free(doc);
    free(text);
    char invalid[128];
    snprintf(invalid, sizeof invalid, "%s/prefs.json", path);
    assert(!setenv("CHIBI_STUDIO_PREFS_PATH", invalid, 1));
    test_check(!studio_prefs_save(&prefs), "an invalid explicit preferences file fails without falling back to user storage");
    if (previous) {
        assert(!setenv("CHIBI_STUDIO_PREFS_PATH", previous, 1));
    } else {
        assert(!unsetenv("CHIBI_STUDIO_PREFS_PATH"));
    }
    free(previous);
    assert(!unlink(path));
}
#endif

static const char *text_at(yyjson_val *root, const char *pointer)
{
    return yyjson_get_str(yyjson_ptr_get(root, pointer));
}

/* A file the previous build saved still loads, every setting round-trips,
 * and its one arrangement becomes Vertical Split's. */
static void prefs_round_trip()
{
    const char
        *old_file = "{\"version\":1,\"followDocuments\":true,\"theme\":"
                    "\"dark\",\"density\":"
                    "\"comfortable\",\"uiScale\":1.5,"
                    "\"reduceMotion\":true,\"wires\":\"orthogonal\","
                    "\"nodes\":{\"shortBindings\":false,"
                    "\"titleFlow\":false,\"compact\":false},\"code\":{"
                    "\"vim\":true,\"applyLive\":false,"
                    "\"fontSize\":20},\"shortcuts\":{"
                    "\"save\":\"Ctrl+Shift+S\","
                    "\"workspaceLayout\":\"Alt+1\","
                    "\"workspaceUi\":\"Alt+2\"},\"layout\":{\"sideWidth\":400,"
                    "\"drawer\":{\"open\":true},"
                    "\"areas\":[\"scene\",\"behavior\",\"outliner\","
                    "\"properties\",{\"panel\":\"Data\"}]}}";
    studio_prefs p = studio_prefs_read(old_file);
    const studio_layout *layout = &p.layouts[STUDIO_WORKSPACE_VERTICAL_SPLIT];
    studio_prefs defaults = studio_prefs_default();
    studio_prefs with_override = defaults;
    snprintf(with_override.shortcuts[STUDIO_COMMAND_SAVE], STUDIO_CHORD_SIZE, "Ctrl+Shift+S");
    bool loaded = p.theme == STUDIO_THEME_DARK && p.comfortable &&
                  p.ui_scale == 1.5f && p.reduced_motion &&
                  p.wires == KIT_WIRE_ORTHOGONAL && !p.short_bindings &&
                  !p.title_flow && !p.compact_nodes && p.code_vim &&
                  !p.code_apply_live && p.code_font_size == 20 &&
                  p.standard_font_family == 2 && p.standard_font_size == 13 &&
                  p.code_font_family == 0 &&
                  !strcmp(p.shortcuts[STUDIO_COMMAND_SAVE], "Ctrl+Shift+S") &&
                  !strcmp(p.shortcuts[STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT], "Alt+1") &&
                  !strcmp(p.shortcuts[STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT], "Alt+2") &&
                  p.workspace == STUDIO_WORKSPACE_VERTICAL_SPLIT &&
                  layout->side_open && layout->side_width == 400 &&
                  layout->drawer_open &&
                  layout->editors[STUDIO_AREA_DRAWER] == STUDIO_EDITOR_PANEL &&
                  !strcmp(layout->panels[STUDIO_AREA_DRAWER], "Data");
    char *json = studio_prefs_write(&p, old_file);
    studio_prefs back = studio_prefs_read(json);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool keys = !strcmp(text_at(root, "/density"), "comfortable") &&
                !strcmp(text_at(root, "/wires"), "orthogonal") &&
                yyjson_is_false(yyjson_ptr_get(root, "/nodes/compact")) &&
                yyjson_is_false(yyjson_ptr_get(root, "/code/applyLive")) &&
                !strcmp(text_at(root, "/shortcuts/save"), "Ctrl+Shift+S") &&
                !strcmp(text_at(root, "/shortcuts/workspaceVerticalSplit"), "Alt+1") &&
                !strcmp(text_at(root, "/shortcuts/workspaceHorizontalSplit"), "Alt+2") &&
                !yyjson_ptr_get(root, "/shortcuts/workspaceLayout") &&
                !yyjson_ptr_get(root, "/shortcuts/workspaceUi") &&
                yyjson_get_num(yyjson_ptr_get(root, "/code/fontSize")) == 20 &&
                yyjson_get_num(yyjson_ptr_get(root, "/uiScale")) == 1.5 &&
                !yyjson_ptr_get(root, "/followDocuments");
    test_check(
        loaded && keys && !studio_prefs_equal(&with_override, &defaults) &&
            studio_prefs_equal(&p, &back),
        "preferences load a previous build's file, include shortcut overrides "
        "in change detection, and round-trip through the settings table"
    );
    yyjson_doc_free(doc);
    free(json);

    /* A newer build's settings stay; a reset shortcut does not come back. */
    const char
        *newer = "{\"code\":{\"vim\":true,\"minimap\":true},\"future\":1,"
                 "\"shortcuts\":{\"save\":\"Ctrl+Shift+S\",\"later\":\"F9\"}}";
    json = studio_prefs_write(&defaults, newer);
    doc = yyjson_read(json, strlen(json), 0);
    root = yyjson_doc_get_root(doc);
    bool kept = yyjson_is_true(yyjson_ptr_get(root, "/code/minimap")) &&
                yyjson_is_false(yyjson_ptr_get(root, "/code/vim")) &&
                yyjson_get_int(yyjson_ptr_get(root, "/future")) == 1 &&
                !strcmp(text_at(root, "/shortcuts/later"), "F9") &&
                !yyjson_ptr_get(root, "/shortcuts/save");
    test_check(
        kept,
        "saving keeps the settings a newer build wrote and drops a "
        "reset shortcut"
    );
    yyjson_doc_free(doc);
    free(json);
}

/* View › Show's toggles are preferences: Generated World, Lighting and
 * Camera Frames start on and Fog of War off, they save under "scene", and each
 * Show command is checked as its preference is. View Through Camera's Shift+C
 * and Keypad 0 reach it in the Scene area and collide with nothing there. */
static void scene_view_preferences()
{
    studio_prefs p = studio_prefs_default();
    bool defaults = p.scene_world && p.scene_lighting && !p.scene_fog && p.scene_cameras;
    p.scene_world = false;
    p.scene_fog = true;
    char *json = studio_prefs_write(&p, NULL);
    studio_prefs back = studio_prefs_read(json);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool saved = yyjson_is_false(yyjson_ptr_get(root, "/scene/generatedWorld")) &&
                 yyjson_is_true(yyjson_ptr_get(root, "/scene/fog")) &&
                 studio_prefs_equal(&p, &back) && !back.scene_world && back.scene_fog;
    yyjson_doc_free(doc);
    free(json);
    studio *s = xcalloc(1, sizeof *s);
    s->prefs = back;
    bool checked = studio_command_checked(s, STUDIO_COMMAND_SHOW_WORLD) == 0 &&
                   studio_command_checked(s, STUDIO_COMMAND_SHOW_LIGHTING) == 1 &&
                   studio_command_checked(s, STUDIO_COMMAND_SHOW_FOG) == 1 &&
                   studio_command_checked(s, STUDIO_COMMAND_SHOW_CAMERAS) == 1;
    free(s);
    studio_command camera = STUDIO_COMMAND_VIEW_THROUGH_CAMERA;
    bool keys = studio_command_for_key(&p, SDLK_C, SDL_KMOD_SHIFT, STUDIO_EDITOR_SCENE, true) ==
                    (int)camera &&
                studio_command_for_key(&p, SDLK_KP_0, 0, STUDIO_EDITOR_SCENE, false) ==
                    (int)camera &&
                studio_command_conflict(&p, camera, "Shift+C", true) < 0 &&
                studio_command_conflict(&p, camera, "Shift+C", false) < 0;
    test_check(
        defaults && saved && checked && keys,
        "Scene view toggles default, save and check as preferences, and "
        "View Through Camera's chords reach it without collisions"
    );
}

static const studio_setting *setting_named(const char *key)
{
    for (int i = 0; i < studio_setting_count; i++) {
        if (!strcmp(studio_settings[i].key, key)) {
            return &studio_settings[i];
        }
    }
    return NULL;
}

static int choice_named(const studio_setting *setting, const char *name)
{
    for (int i = 0; setting && i < setting->choice_count; i++) {
        if (!strcmp(setting->choices[i].value, name)) {
            return i;
        }
    }
    return -1;
}

static void font_preferences()
{
    studio_prefs p = studio_prefs_default();
    const studio_setting *standard_family = setting_named("font.standardFamily");
    const studio_setting *standard_size = setting_named("font.standardSize");
    const studio_setting *code_family = setting_named("code.fontFamily");
    const studio_setting *code_size = setting_named("code.fontSize");
    bool choices = standard_family && standard_family->choice_count == 3 &&
                   choice_named(standard_family, "Inter") >= 0 &&
                   choice_named(standard_family, "Noto Sans") >= 0 &&
                   choice_named(standard_family, "Geist") >= 0 &&
                   choice_named(code_family, "JetBrains Mono") >= 0 &&
                   choice_named(code_family, "Inconsolata") >= 0;
    bool defaults = p.standard_font_size == 13 && p.code_font_size == 13 &&
                    !strcmp(studio_standard_font(&p), "geist") &&
                    !strcmp(studio_code_font(&p), "jetbrains mono");
    bool changed = choices &&
                   studio_setting_parse(&p, standard_family, "Noto Sans") &&
                   studio_setting_parse(&p, standard_size, "18") &&
                   studio_setting_parse(&p, code_family, "Inconsolata") &&
                   studio_setting_parse(&p, code_size, "16");
    char *json = studio_prefs_write(&p, NULL);
    studio_prefs back = studio_prefs_read(json);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool serialized = !strcmp(text_at(root, "/font/standardFamily"), "Noto Sans") &&
                      yyjson_get_num(yyjson_ptr_get(root, "/font/standardSize")) == 18 &&
                      !strcmp(text_at(root, "/code/fontFamily"), "Inconsolata") &&
                      yyjson_get_num(yyjson_ptr_get(root, "/code/fontSize")) == 16;
    test_check(
        defaults && changed && serialized && studio_prefs_equal(&p, &back) &&
            !strcmp(studio_standard_font(&back), "noto sans") &&
            !strcmp(studio_code_font(&back), "inconsolata"),
        "standard and monospace font family and size settings persist and "
        "resolve to registered faces"
    );
    yyjson_doc_free(doc);
    free(json);
}

static void invalid_values()
{
    studio_prefs defaults = studio_prefs_default();
    const char *invalid = "{\"theme\":\"purple\",\"density\":7,\"uiScale\":1.3,"
                          "\"reduceMotion\":\"yes\",\"font\":{"
                          "\"standardFamily\":\"missing\","
                          "\"standardSize\":99},"
                          "\"wires\":\"zigzag\",\"nodes\":{\"shortBindings\":1,"
                          "\"titleFlow\":null},"
                          "\"code\":{\"vim\":\"true\",\"fontSize\":2,"
                          "\"applyLive\":[]},\"shortcuts\":{"
                          "\"save\":\"Mod+Missing\",\"search\":9}}";
    studio_prefs p = studio_prefs_read(invalid);
    studio_prefs out_of_range = studio_prefs_read("{\"uiScale\":9,\"code\":{\"fontSize\":99}}");
    studio_prefs garbage = studio_prefs_read("not json");
    studio_prefs none = studio_prefs_read(NULL);
    studio_prefs unknown_workspace = studio_prefs_read("{\"layout\":{\"workspace\":\"missing\"}}");
    studio_prefs invalid_layout = studio_prefs_read(
        "{\"layout\":{\"workspace\":\"ui\",\"presets\":{\"ui\":{"
        "\"sideOpen\":\"closed\",\"sideWidth\":99,\"mainSplit\":0.99,"
        "\"sideSplit\":1.5,\"drawer\":{\"open\":\"yes\",\"height\":20},"
        "\"areas\":[\"unknown\"]}}}}"
    );
    studio_prefs on_horizontal = defaults;
    on_horizontal.workspace = STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    studio_prefs legacy_ui = studio_prefs_read("{\"layout\":{\"workspace\":\"ui\"}}");
    studio_prefs legacy_layout = studio_prefs_read("{\"layout\":{\"workspace\":\"layout\"}}");
    test_check(
        studio_prefs_equal(&p, &defaults) &&
            studio_prefs_equal(&out_of_range, &defaults) &&
            studio_prefs_equal(&garbage, &defaults) &&
            studio_prefs_equal(&none, &defaults) &&
            studio_prefs_equal(&unknown_workspace, &defaults) &&
            studio_prefs_equal(&legacy_ui, &on_horizontal) &&
            studio_prefs_equal(&legacy_layout, &defaults) &&
            !p.shortcuts[STUDIO_COMMAND_SAVE][0] &&
            !p.shortcuts[STUDIO_COMMAND_SEARCH][0] &&
            studio_prefs_equal(&invalid_layout, &on_horizontal),
        "a preference of the wrong type, out of range or not a choice keeps "
        "its default, a workspace's arrangement included"
    );
}

static void theme_presets()
{
    studio_prefs prefs = studio_prefs_default();
    bool persisted = true;
    for (int theme = 0; theme < STUDIO_THEME_COUNT; theme++) {
        prefs.theme = (studio_theme)theme;
        char *json = studio_prefs_write(&prefs, NULL);
        studio_prefs restored = studio_prefs_read(json);
        persisted &= restored.theme == prefs.theme;
        free(json);
    }
    test_check(persisted, "every Appearance palette persists through the preferences file");

    rml_backend *backend = rml_backend_create(800, 600, 1);
    rml_context *context = rml_backend_context(backend);
    kit *k = test_kit_create(context, "graph.rml");
    rml_element *body = rml_context_document(context, 0);
    studio_theme_apply(k, STUDIO_THEME_CATPPUCCIN_MOCHA, true);
    bool dark = rml_element_is_class_set(body, "dark") &&
                rml_element_is_class_set(body, "theme-catppuccin-mocha");
    studio_theme_apply(k, STUDIO_THEME_SOLARIZED_LIGHT, false);
    bool light = !rml_element_is_class_set(body, "dark") &&
                 !rml_element_is_class_set(body, "theme-catppuccin-mocha") &&
                 rml_element_is_class_set(body, "theme-solarized-light");
    studio_theme_apply(k, STUDIO_THEME_SYSTEM, false);
    bool system_dark = rml_element_is_class_set(body, "dark") &&
                       !rml_element_is_class_set(body, "theme-solarized-light");
    studio_theme_apply(k, STUDIO_THEME_SYSTEM, true);
    test_check(
        dark && light && system_dark && !rml_element_is_class_set(body, "dark"),
        "the graph switches named dark and light palettes without "
        "retaining an old class, then follows System"
    );
    kit_destroy(k);
    rml_backend_destroy(backend);
}

/* Each split mode keeps its own arrangement through the file, and which one
   is active. */
static void workspace_presets_round_trip()
{
    studio_prefs p = studio_prefs_default();
    p.workspace = STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    studio_layout *vertical = &p.layouts[STUDIO_WORKSPACE_VERTICAL_SPLIT];
    vertical->side_open = false;
    vertical->side_width = 448;
    vertical->main_split = 0.67f;
    vertical->drawer_open = true;
    vertical->editors[STUDIO_AREA_DRAWER] = STUDIO_EDITOR_PANEL;
    snprintf(vertical->panels[STUDIO_AREA_DRAWER], sizeof vertical->panels[0], "%s", "Data");
    studio_layout *horizontal = &p.layouts[STUDIO_WORKSPACE_HORIZONTAL_SPLIT];
    horizontal->side_width = 336;
    horizontal->main_split = 0.56f;
    horizontal->drawer_height = 190;
    horizontal->canvas_actions_open = true;
    char *json = studio_prefs_write(
        &p,
        "{\"followDocuments\":true,\"layout\":{"
        "\"presets\":{\"ui\":{},\"layout\":{}}}}"
    );
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    studio_prefs back = studio_prefs_read(json);
    bool encoded = !strcmp(text_at(root, "/layout/workspace"), "horizontalSplit") &&
                   yyjson_is_false(
                       yyjson_ptr_get(root, "/layout/presets/verticalSplit/sideOpen")
                   ) &&
                   !strcmp(text_at(root, "/layout/presets/verticalSplit/areas/4/panel"), "Data") &&
                   !strcmp(text_at(root, "/layout/presets/horizontalSplit/areas/1"), "code") &&
                   !yyjson_ptr_get(root, "/followDocuments") &&
                   !yyjson_ptr_get(root, "/layout/presets/ui") &&
                   !yyjson_ptr_get(root, "/layout/presets/layout");
    studio_prefs closed_actions = back;
    closed_actions.layouts[STUDIO_WORKSPACE_HORIZONTAL_SPLIT].canvas_actions_open = false;
    bool hidden_by_default = !studio_layout_default(STUDIO_WORKSPACE_HORIZONTAL_SPLIT)
                                  .canvas_actions_open;
    encoded &= yyjson_is_true(
        yyjson_ptr_get(root, "/layout/presets/horizontalSplit/canvasActionsOpen")
    );
    studio_prefs other = back;
    other.layouts[STUDIO_WORKSPACE_VERTICAL_SPLIT].side_open = true;
    test_check(
        hidden_by_default && encoded && studio_prefs_equal(&p, &back) &&
            !studio_prefs_equal(&back, &other) &&
            !studio_prefs_equal(&back, &closed_actions),
        "each workspace keeps its own arrangement through the preferences "
        "file, and the active one stays active"
    );
    yyjson_doc_free(doc);
    free(json);

    const char *
        previous = "{\"followDocuments\":true,\"layout\":{\"workspace\":\"ui\","
                   "\"presets\":{\"layout\":{\"sideWidth\":460},"
                   "\"ui\":{\"sideWidth\":340}}}}";
    studio_prefs migrated = studio_prefs_read(previous);
    bool migrated_values = migrated.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT &&
                           migrated.layouts[STUDIO_WORKSPACE_VERTICAL_SPLIT].side_width ==
                               460 &&
                           migrated.layouts[STUDIO_WORKSPACE_HORIZONTAL_SPLIT].side_width ==
                               340;
    json = studio_prefs_write(&migrated, previous);
    doc = yyjson_read(json, strlen(json), 0);
    root = yyjson_doc_get_root(doc);
    bool migrated_keys = !strcmp(text_at(root, "/layout/workspace"), "horizontalSplit") &&
                         yyjson_ptr_get(root, "/layout/presets/verticalSplit") &&
                         yyjson_ptr_get(root, "/layout/presets/horizontalSplit") &&
                         !yyjson_ptr_get(root, "/layout/presets/layout") &&
                         !yyjson_ptr_get(root, "/layout/presets/ui") &&
                         !yyjson_ptr_get(root, "/followDocuments");
    test_check(migrated_values && migrated_keys, "previous mode names migrate while keeping their saved arrangements");
    yyjson_doc_free(doc);
    free(json);
}

static void shortcut_chords()
{
    studio_prefs prefs = studio_prefs_default();
    bool defaults_valid = true;
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const char *chord = studio_command_default_chord((studio_command)c);
        defaults_valid &= !chord || !chord[0] || studio_chord_valid(chord);
    }
    studio_key key = { 0 };
    bool parsed = studio_chord_parse("Mod+Shift+P", false, &key) &&
                  key.key == SDLK_P && key.mod == (SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    bool mac = studio_chord_parse("Mod+Shift+P", true, &key) &&
               key.key == SDLK_P && key.mod == (SDL_KMOD_GUI | SDL_KMOD_SHIFT);
    bool matching = studio_chord_matches("Mod+Shift+P", SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT, false) &&
                    studio_chord_matches("Mod+Shift+P", SDLK_P, SDL_KMOD_GUI | SDL_KMOD_SHIFT, true) &&
                    !studio_chord_matches("Mod+Shift+P", SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT, true) &&
                    studio_chord_matches("Ctrl+P", SDLK_P, SDL_KMOD_CTRL, true);
    char shown[64];
    studio_chord_format(shown, sizeof shown, "Mod+Shift+P", false);
    bool windows_format = !strcmp(shown, "Ctrl+Shift+P");
    studio_chord_format(shown, sizeof shown, "Mod+Shift+P", true);
    bool mac_format = !strcmp(shown, "\xe2\x87\xa7\xe2\x8c\x98P");
    bool valid = studio_chord_valid("Mod+Shift+P") &&
                 studio_chord_valid("Ctrl+P") && !studio_chord_valid("") &&
                 !studio_chord_valid("Mod+") && !studio_chord_valid("NoSuchKey+P");

    prefs.shortcuts[STUDIO_COMMAND_SETTINGS][0] = '\0';
    int conflict = studio_command_conflict(&prefs, STUDIO_COMMAND_SETTINGS, "Mod+S", false);
    bool global_conflict = conflict == STUDIO_COMMAND_SAVE &&
                           studio_command_conflict(&prefs, STUDIO_COMMAND_SAVE, "Mod+S", false) <
                               0;

    test_check(
        parsed && mac && matching && windows_format && mac_format && valid && defaults_valid,
        "shortcut chords and every command default parse, match and "
        "format on each platform"
    );
    test_check(global_conflict, "a global shortcut conflict names the other command and ignores itself");
}

static void shortcut_dispatch()
{
    studio_prefs prefs = studio_prefs_default();
    SDL_Keymod primary = SDL_KMOD_CTRL;
    int zoom_equals = studio_command_for_key(&prefs, SDLK_EQUALS, primary, STUDIO_EDITOR_SCENE, false);
    int zoom_plus = studio_command_for_key(&prefs, SDLK_PLUS, primary, STUDIO_EDITOR_SCENE, false);
    int zoom_keypad_plus = studio_command_for_key(&prefs, SDLK_KP_PLUS, primary, STUDIO_EDITOR_SCENE, false);
    int zoom_keypad_minus = studio_command_for_key(&prefs, SDLK_KP_MINUS, primary, STUDIO_EDITOR_SCENE, false);
    int zoom_keypad_zero = studio_command_for_key(&prefs, SDLK_KP_0, primary, STUDIO_EDITOR_SCENE, false);
    int scene_frame = studio_command_for_key(&prefs, SDLK_F, 0, STUDIO_EDITOR_SCENE, false);
    int outliner_frame = studio_command_for_key(&prefs, SDLK_F, 0, STUDIO_EDITOR_OUTLINER, false);
    int behavior_frame = studio_command_for_key(&prefs, SDLK_F, 0, STUDIO_EDITOR_BEHAVIOR, false);
    int scene_play = studio_command_for_key(&prefs, SDLK_SPACE, 0, STUDIO_EDITOR_SCENE, false);
    int behavior_play = studio_command_for_key(&prefs, SDLK_SPACE, 0, STUDIO_EDITOR_BEHAVIOR, false);
    int search_alias = studio_command_for_key(&prefs, SDLK_P, SDL_KMOD_CTRL, STUDIO_EDITOR_SCENE, false);
    bool contexts = scene_frame == STUDIO_COMMAND_FRAME &&
                    outliner_frame == STUDIO_COMMAND_FRAME &&
                    behavior_frame < 0 && scene_play == STUDIO_COMMAND_PLAY &&
                    behavior_play < 0 && search_alias == STUDIO_COMMAND_SEARCH &&
                    zoom_equals == STUDIO_COMMAND_ZOOM_IN &&
                    zoom_plus == STUDIO_COMMAND_ZOOM_IN &&
                    zoom_keypad_plus == STUDIO_COMMAND_ZOOM_IN &&
                    zoom_keypad_minus == STUDIO_COMMAND_ZOOM_OUT &&
                    zoom_keypad_zero == STUDIO_COMMAND_ZOOM_RESET;

    snprintf(prefs.shortcuts[STUDIO_COMMAND_SAVE], STUDIO_CHORD_SIZE, "Mod+P");
    bool symmetric = studio_command_conflict(
                         &prefs,
                         STUDIO_COMMAND_SEARCH,
                         studio_command_chord(&prefs, STUDIO_COMMAND_SEARCH),
                         false
                     ) == STUDIO_COMMAND_SAVE &&
                     studio_command_conflict(&prefs, STUDIO_COMMAND_SAVE, "Mod+P", false) ==
                         STUDIO_COMMAND_SEARCH;
    snprintf(prefs.shortcuts[STUDIO_COMMAND_SEARCH], STUDIO_CHORD_SIZE, "Mod+Shift+P");
    bool override_disables_alias = studio_command_for_key(&prefs, SDLK_P, SDL_KMOD_CTRL, STUDIO_EDITOR_SCENE, false) ==
                                       STUDIO_COMMAND_SAVE &&
                                   studio_command_conflict(&prefs, STUDIO_COMMAND_SAVE, "Mod+P", false) <
                                       0;

    test_check(
        contexts,
        "shortcut dispatch respects area contexts and keeps "
        "Search's unmodified platform alias"
    );
    test_check(
        symmetric && override_disables_alias,
        "shortcut conflicts include aliases symmetrically, and an "
        "override replaces its defaults"
    );
}

static void shortcut_recording()
{
    studio_prefs prefs = studio_prefs_default();
    int recording = STUDIO_COMMAND_SEARCH + 1;
    bool modifier = studio_shortcut_record(
        &prefs,
        &recording,
        (studio_key){ .key = SDLK_LCTRL, .mod = SDL_KMOD_CTRL },
        false
    );
    bool repeat = studio_shortcut_record(
        &prefs,
        &recording,
        (studio_key){ .key = SDLK_P, .mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT, .repeat = true },
        false
    );
    bool still_recording = modifier && repeat &&
                           recording == STUDIO_COMMAND_SEARCH + 1 &&
                           !prefs.shortcuts[STUDIO_COMMAND_SEARCH][0];
    bool saved = studio_shortcut_record(
                     &prefs,
                     &recording,
                     (studio_key){ .key = SDLK_P, .mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT },
                     false
                 ) &&
                 recording == 0 &&
                 !strcmp(prefs.shortcuts[STUDIO_COMMAND_SEARCH], "Mod+Shift+P");
    bool matches = studio_chord_matches(
        prefs.shortcuts[STUDIO_COMMAND_SEARCH],
        SDLK_P,
        SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
        false
    );

    recording = STUDIO_COMMAND_SEARCH + 1;
    bool canceled = studio_shortcut_record(
                        &prefs,
                        &recording,
                        (studio_key){ .key = SDLK_ESCAPE, .mod = SDL_KMOD_CTRL },
                        false
                    ) &&
                    recording == 0 &&
                    !strcmp(prefs.shortcuts[STUDIO_COMMAND_SEARCH], "Mod+Shift+P");

    recording = STUDIO_COMMAND_SAVE + 1;
    bool mac_saved = studio_shortcut_record(
                         &prefs,
                         &recording,
                         (studio_key){ .key = SDLK_S,
                                       .mod = SDL_KMOD_GUI | SDL_KMOD_CTRL | SDL_KMOD_SHIFT },
                         true
                     ) &&
                     recording == 0 &&
                     !strcmp(prefs.shortcuts[STUDIO_COMMAND_SAVE], "Mod+Ctrl+Shift+S") &&
                     studio_chord_matches(
                         prefs.shortcuts[STUDIO_COMMAND_SAVE],
                         SDLK_S,
                         SDL_KMOD_GUI | SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
                         true
                     );

    recording = STUDIO_COMMAND_SEARCH + 1;
    bool reset_to_default = studio_shortcut_record(
                                &prefs,
                                &recording,
                                (studio_key){ .key = SDLK_K, .mod = SDL_KMOD_CTRL },
                                false
                            ) &&
                            recording == 0 &&
                            !prefs.shortcuts[STUDIO_COMMAND_SEARCH][0];
    test_check(
        still_recording && saved && matches && canceled && mac_saved && reset_to_default,
        "shortcut recording ignores modifiers and repeats, cancels on Escape, "
        "handles Mac modifiers and stores the default as no override"
    );
}

/* No two commands share a default chord or alias where both can act, on
 * either platform, every binding parses, and Keyboard Shortcuts lists each
 * command's area. */
static void shortcut_defaults()
{
    studio_prefs prefs = studio_prefs_default();
    bool unique = true, valid = true, grouped = true;
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const char *chords[5];
        int count = studio_command_bindings(&prefs, (studio_command)c, chords, 5);
        for (int i = 0; i < count; i++) {
            valid &= studio_chord_valid(chords[i]);
        }
        for (int mac = 0; count && mac < 2; mac++) {
            int other = studio_command_conflict(&prefs, (studio_command)c, chords[0], mac);
            if (other >= 0) {
                printf(
                    "  %s conflicts with %s\n",
                    studio_command_id((studio_command)c),
                    studio_command_id((studio_command)other)
                );
            }
            unique &= other < 0;
        }
        grouped &= studio_keymap_area(studio_command_group((studio_command)c));
    }
    test_check(unique && valid && grouped, "default shortcuts are unique where they act, parse, and have a Keyboard Shortcuts area");
}

/* The areas each object, find and transport key acts in. */
static void shortcut_areas()
{
    studio_prefs p = studio_prefs_default();
    SDL_Keymod ctrl = SDL_KMOD_CTRL, cmd = SDL_KMOD_GUI;
    bool deletes = studio_command_for_key(&p, SDLK_BACKSPACE, 0, STUDIO_EDITOR_OUTLINER, true) ==
                       STUDIO_COMMAND_DELETE &&
                   studio_command_for_key(&p, SDLK_BACKSPACE, 0, STUDIO_EDITOR_SCENE, false) ==
                       STUDIO_COMMAND_DELETE &&
                   studio_command_for_key(&p, SDLK_DELETE, 0, STUDIO_EDITOR_SCENE, false) ==
                       STUDIO_COMMAND_DELETE &&
                   studio_command_for_key(&p, SDLK_X, 0, STUDIO_EDITOR_SCENE, false) ==
                       STUDIO_COMMAND_DELETE &&
                   studio_command_for_key(&p, SDLK_BACKSPACE, 0, STUDIO_EDITOR_ASSETS, false) <
                       0;
    bool duplicates = studio_command_for_key(&p, SDLK_D, cmd, STUDIO_EDITOR_SCENE, true) ==
                          STUDIO_COMMAND_DUPLICATE &&
                      studio_command_for_key(&p, SDLK_D, ctrl, STUDIO_EDITOR_OUTLINER, false) ==
                          STUDIO_COMMAND_DUPLICATE &&
                      studio_command_for_key(&p, SDLK_D, SDL_KMOD_SHIFT, STUDIO_EDITOR_SCENE, false) ==
                          STUDIO_COMMAND_DUPLICATE &&
                      studio_command_for_key(&p, SDLK_D, SDL_KMOD_SHIFT, STUDIO_EDITOR_OUTPUT, false) <
                          0 &&
                      studio_command_for_key(&p, SDLK_D, ctrl, STUDIO_EDITOR_BEHAVIOR, false) <
                          0;
    bool selects = studio_command_for_key(&p, SDLK_A, ctrl, STUDIO_EDITOR_OUTLINER, false) ==
                       STUDIO_COMMAND_SELECT_ALL &&
                   studio_command_for_key(&p, SDLK_A, ctrl, STUDIO_EDITOR_BEHAVIOR, false) <
                       0 &&
                   studio_command_for_key(&p, SDLK_ESCAPE, 0, STUDIO_EDITOR_SCENE, false) ==
                       STUDIO_COMMAND_DESELECT &&
                   studio_command_for_key(&p, SDLK_ESCAPE, 0, STUDIO_EDITOR_BEHAVIOR, false) <
                       0 &&
                   studio_command_for_key(&p, SDLK_HOME, 0, STUDIO_EDITOR_SCENE, false) ==
                       STUDIO_COMMAND_FRAME_ALL &&
                   studio_command_for_key(&p, SDLK_HOME, 0, STUDIO_EDITOR_BEHAVIOR, false) <
                       0;
    bool finds = true;
    static const studio_editor FINDS[] = {
        STUDIO_EDITOR_BEHAVIOR,   STUDIO_EDITOR_CODE,   STUDIO_EDITOR_OUTLINER,
        STUDIO_EDITOR_PROPERTIES, STUDIO_EDITOR_ASSETS, STUDIO_EDITOR_OUTPUT,
        STUDIO_EDITOR_MEMBERS
    };
    for (size_t i = 0; i < sizeof FINDS / sizeof *FINDS; i++) {
        finds &= studio_command_for_key(&p, SDLK_F, cmd, FINDS[i], true) ==
                 STUDIO_COMMAND_FIND;
    }
    finds &= studio_command_for_key(&p, SDLK_F, cmd, STUDIO_EDITOR_SCENE, true) < 0;
    bool transport = studio_command_for_key(&p, SDLK_F5, SDL_KMOD_SHIFT, STUDIO_EDITOR_BEHAVIOR, true) ==
                         STUDIO_COMMAND_STOP &&
                     studio_command_for_key(&p, SDLK_SLASH, cmd, STUDIO_EDITOR_CODE, true) ==
                         STUDIO_COMMAND_KEYBOARD_SHORTCUTS &&
                     studio_command_for_key(&p, SDLK_SLASH, ctrl, STUDIO_EDITOR_COUNT, false) ==
                         STUDIO_COMMAND_KEYBOARD_SHORTCUTS;
    test_check(deletes && duplicates, "Delete, Backspace and X delete, and Shift+D and Mod+D duplicate, objects only over the Scene and Outliner");
    test_check(selects && finds && transport, "Select All, Deselect, Frame All, Find, Stop and Keyboard Shortcuts act in their areas");
}

/* Text fields keep the primary modifier's editing keys; Studio takes the
 * rest. Option's composed characters dispatch as the key's letter. */
static void shortcut_text_keys()
{
    bool text = studio_text_key(SDLK_A) && studio_text_key(SDLK_C) &&
                studio_text_key(SDLK_V) && studio_text_key(SDLK_X) &&
                studio_text_key(SDLK_Z) && studio_text_key(SDLK_Y) &&
                studio_text_key(SDLK_LEFT) && studio_text_key(SDLK_BACKSPACE);
    bool commands = !studio_text_key(SDLK_K) && !studio_text_key(SDLK_S) &&
                    !studio_text_key(SDLK_EQUALS) && !studio_text_key(SDLK_SLASH);
    SDL_KeyboardEvent option_g = { .key = 0xa9,
                                   .scancode = SDL_SCANCODE_G,
                                   .mod = SDL_KMOD_LALT };
    SDL_KeyboardEvent copyright = { .key = 0xa9, .scancode = SDL_SCANCODE_G };
    SDL_KeyboardEvent option_f5 = { .key = SDLK_F5,
                                    .scancode = SDL_SCANCODE_F5,
                                    .mod = SDL_KMOD_LALT };
    bool composed = studio_event_key(&option_g) == SDLK_G &&
                    studio_event_key(&copyright) == 0xa9 &&
                    studio_event_key(&option_f5) == SDLK_F5;
    char shown[64];
    studio_chord_format(shown, sizeof shown, "Backspace", true);
    bool glyphs = !strcmp(shown, "Delete");
    studio_chord_format(shown, sizeof shown, "Backspace", false);
    glyphs &= !strcmp(shown, "Backspace");
    studio_chord_format(shown, sizeof shown, "Mod+Up", true);
    glyphs &= !strcmp(shown, "\xe2\x8c\x98\xe2\x86\x91");
    studio_chord_format(shown, sizeof shown, "Ctrl+Space", true);
    glyphs &= !strcmp(shown, "\xe2\x8c\x83 Space");
    studio_chord_format(shown, sizeof shown, "Escape", false);
    glyphs &= !strcmp(shown, "Esc");
    test_check(text && commands, "a focused field keeps the clipboard, undo and caret chords and passes the rest to Studio");
    test_check(composed && glyphs, "Option-composed keys dispatch as their letter, and menus write arrows as glyphs and a Mac's delete keys by name");
}

/* The `shortcuts` group named `label`, or NULL. */
static yyjson_mut_val *sheet_group(yyjson_mut_doc *doc, const char *label)
{
    yyjson_mut_val *sheet = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "shortcuts");
    yyjson_mut_val *groups = yyjson_mut_obj_get(sheet, "groups");
    for (size_t i = 0; i < yyjson_mut_arr_size(groups); i++) {
        yyjson_mut_val *group = yyjson_mut_arr_get(groups, i);
        if (!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(group, "label")), label)) {
            return group;
        }
    }
    return NULL;
}

/* The keys of the group's row labeled `label`, joined by spaces. */
static void sheet_keys(yyjson_mut_val *group, const char *label, char *out, size_t size)
{
    out[0] = '\0';
    yyjson_mut_val *rows = group ? yyjson_mut_obj_get(group, "rows") : NULL;
    for (size_t i = 0; rows && i < yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, i);
        if (strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(row, "label")), label)) {
            continue;
        }
        yyjson_mut_val *keys = yyjson_mut_obj_get(row, "keys");
        size_t used = 0;
        for (size_t k = 0; k < yyjson_mut_arr_size(keys); k++) {
            used += (size_t)snprintf(
                out + used,
                size - used,
                "%s%s",
                k ? " " : "",
                yyjson_mut_get_str(yyjson_mut_arr_get(keys, k))
            );
        }
    }
}

/* Keyboard Shortcuts lists commands by area with their effective chords,
 * then the keys editors take themselves, and searches all of it. */
static void shortcut_sheet()
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    studio_keymap_add(doc);
    studio_prefs prefs = studio_prefs_default();
    studio_keymap_cache cache = { 0 };
    bool shown = studio_keymap_show(doc, &prefs, false, &cache);
    bool cached = !studio_keymap_show(doc, &prefs, false, &cache);
    char keys[128];
    sheet_keys(sheet_group(doc, "Global"), "Search", keys, sizeof keys);
    bool search = !strcmp(keys, "Ctrl+K Ctrl+P");
    sheet_keys(sheet_group(doc, "Behavior"), "Duplicate nodes", keys, sizeof keys);
    bool fixed = !strcmp(keys, "Shift+D Ctrl+D");
    sheet_keys(sheet_group(doc, "Text Editor"), "Start and end of the text", keys, sizeof keys);
    bool platform = !strcmp(keys, "Ctrl+Home Ctrl+End");
    snprintf(prefs.shortcuts[STUDIO_COMMAND_SAVE], STUDIO_CHORD_SIZE, "Mod+Shift+S");
    bool rebound = studio_keymap_show(doc, &prefs, false, &cache);
    sheet_keys(sheet_group(doc, "Global"), "Save", keys, sizeof keys);
    rebound &= !strcmp(keys, "Ctrl+Shift+S");
    yyjson_mut_val *sheet = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "shortcuts");
    yyjson_mut_set_str(yyjson_mut_obj_get(sheet, "query"), "breakpoint");
    studio_keymap_show(doc, &prefs, true, &cache);
    bool found = yyjson_mut_get_int(yyjson_mut_obj_get(sheet, "matches")) == 1 &&
                 sheet_group(doc, "Behavior") && !sheet_group(doc, "Global");
    yyjson_mut_set_str(yyjson_mut_obj_get(sheet, "query"), "no such key");
    studio_keymap_show(doc, &prefs, true, &cache);
    bool empty = yyjson_mut_get_int(yyjson_mut_obj_get(sheet, "matches")) == 0 &&
                 !yyjson_mut_arr_size(yyjson_mut_obj_get(sheet, "groups"));
    test_check(shown && cached && search && fixed && platform && rebound && found && empty, "Keyboard Shortcuts lists effective chords and editor keys by area and searches them");
    yyjson_mut_doc_free(doc);
}

/* Authored panels bind the preferences as saved keys: a panel's change
 * applies, an invalid one is dropped, and the save shows the preferences. */
static void save_mirror()
{
    game_save *save = game_save_new();
    uint64_t seen = UINT64_MAX;
    studio_prefs p = studio_prefs_default();
    studio_prefs_share(&p, save, &seen);
    bool mirrored = !strcmp(game_save_get(save, "theme"), "system") &&
                    !strcmp(game_save_get(save, "codeVim"), "false") &&
                    !strcmp(game_save_get(save, "uiScale"), "1") &&
                    !strcmp(game_save_get(save, "codeFontSize"), "13") &&
                    !strcmp(game_save_get(save, "density"), "compact");
    game_save_set(save, "codeVim", "true");
    game_save_set(save, "theme", "dark");
    game_save_set(save, "uiScale", "7");
    studio_prefs_share(&p, save, &seen);
    bool applied = p.code_vim && p.theme == STUDIO_THEME_DARK && p.ui_scale == 1 &&
                   !strcmp(game_save_get(save, "uiScale"), "1");
    p.comfortable = true;
    studio_prefs_share(&p, save, &seen);
    test_check(
        mirrored && applied && !strcmp(game_save_get(save, "density"), "comfortable"),
        "preferences mirror to saved keys, and a panel's valid change "
        "applies back"
    );
    game_save_free(save);
}

/* ---------- The page's data ---------- */

typedef struct
{
    yyjson_mut_doc *doc;
    yyjson_mut_val *page;
    studio_prefs prefs;
    studio_command_info commands[4];
    studio_settings_view view;
    studio_settings_cache cache;
} page_fixture;

static void page_open(page_fixture *f)
{
    f->cache = (studio_settings_cache){ 0 };
    f->doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(f->doc, yyjson_mut_obj(f->doc));
    studio_settings_add(f->doc);
    f->page = yyjson_mut_obj_get(yyjson_mut_doc_get_root(f->doc), "settings");
    f->prefs = studio_prefs_default();
    f->commands[0] = (studio_command_info){
        .id = "save",
        .label = "Save",
        .shortcut = studio_command_default_chord(STUDIO_COMMAND_SAVE)
    };
    f->commands[1] = (studio_command_info){
        .id = "search",
        .label = "Search",
        .shortcut = studio_command_default_chord(STUDIO_COMMAND_SEARCH)
    };
    f->commands[2] = (studio_command_info){ .id = "reload",
                                            .label = "Reload",
                                            .keywords = "revert disk" };
    f->commands[3] = (studio_command_info){
        .id = "frame",
        .label = "Frame Selected",
        .shortcut = studio_command_default_chord(STUDIO_COMMAND_FRAME)
    };
    f->view = (studio_settings_view){ .prefs = &f->prefs,
                                      .cache = &f->cache,
                                      .commands = f->commands,
                                      .command_count = 4,
                                      .mac = false,
                                      .project = "Arena" };
    snprintf(f->view.fields[STUDIO_PROJECT_NAME], sizeof f->view.fields[0], "Arena");
    snprintf(f->view.fields[STUDIO_PROJECT_START_SCENE], sizeof f->view.fields[0], "scenes/Main.kdl");
    apush(
        f->view.choices[STUDIO_PROJECT_START_SCENE],
        ((studio_setting_choice){ "scenes/Main.kdl", "Main" })
    );
    apush(
        f->view.choices[STUDIO_PROJECT_START_SCENE],
        ((studio_setting_choice){ "scenes/Boss.kdl", "Boss" })
    );
    apush(f->view.choices[STUDIO_PROJECT_UI], ((studio_setting_choice){ "Hud", "Hud" }));
    apush(f->view.choices[STUDIO_PROJECT_UI], ((studio_setting_choice){ "Shell", "Shell" }));
    studio_settings_sync(&f->view, f->doc, false);
}

static void page_view_free(studio_settings_view *view)
{
    for (int f = 0; f < STUDIO_PROJECT_FIELD_COUNT; f++) {
        afree(view->choices[f]);
    }
}

static void page_close(page_fixture *f)
{
    page_view_free(&f->view);
    yyjson_mut_doc_free(f->doc);
}

static yyjson_mut_val *member(yyjson_mut_val *object, const char *name)
{
    return yyjson_mut_obj_get(object, name);
}

static void set_text(page_fixture *f, yyjson_mut_val *object, const char *name, const char *text)
{
    ui_data_set_text(f->doc, member(object, name), text);
}

static const char *text_of_page(page_fixture *f, const char *name)
{
    return yyjson_mut_get_str(member(f->page, name));
}

static yyjson_mut_val *row_named(page_fixture *f, const char *key)
{
    yyjson_mut_val *rows = member(f->page, "rows");
    for (size_t i = 0; i < yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, i);
        if (!strcmp(yyjson_mut_get_str(member(row, "key")), key)) {
            return row;
        }
    }
    return NULL;
}

/* The keys of the rows shown, joined by spaces. */
static void shown(page_fixture *f, char *out, size_t size)
{
    yyjson_mut_val *rows = member(f->page, "rows");
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, i);
        if (yyjson_mut_get_bool(member(row, "shown"))) {
            used += (size_t)snprintf(
                out + used,
                size - used,
                "%s%s",
                used ? " " : "",
                yyjson_mut_get_str(member(row, "key"))
            );
        }
    }
}

static const char *category_text(page_fixture *f, studio_settings_category c, const char *name)
{
    yyjson_mut_val *item = yyjson_mut_arr_get(member(f->page, "categories"), (size_t)c);
    return yyjson_mut_get_str(member(item, name));
}

static bool category_flag(page_fixture *f, studio_settings_category c, const char *name)
{
    yyjson_mut_val *item = yyjson_mut_arr_get(member(f->page, "categories"), (size_t)c);
    return yyjson_mut_get_bool(member(item, name));
}

static bool keys_are(yyjson_mut_val *row, const char *const *keys, int count)
{
    yyjson_mut_val *list = member(row, "keys");
    bool same = (int)yyjson_mut_arr_size(list) == count;
    for (int i = 0; same && i < count; i++) {
        same = !strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(list, (size_t)i)), keys[i]);
    }
    return same;
}

static void page_rows()
{
    page_fixture f;
    page_open(&f);
    char keys[512];
    shown(&f, keys, sizeof keys);
    int rows = (int)yyjson_mut_arr_size(member(f.page, "rows"));
    test_check(
        rows == studio_setting_count + f.view.command_count + STUDIO_PROJECT_FIELD_COUNT &&
            !strcmp(
                keys,
                "theme density uiScale reduceMotion "
                "font.standardFamily font.standardSize"
            ) &&
            !strcmp(yyjson_mut_get_str(member(f.page, "title")), "Appearance") &&
            category_flag(&f, STUDIO_SETTINGS_PROJECT, "hidden"),
        "the Settings page shows the first category's settings under the "
        "Editor tab"
    );

    set_text(&f, f.page, "query", "vim");
    studio_settings_sync(&f.view, f.doc, true);
    shown(&f, keys, sizeof keys);
    yyjson_mut_val *vim = row_named(&f, "code.vim");
    test_check(
        !strcmp(keys, "code.vim") && yyjson_mut_get_bool(member(vim, "head")) &&
            !strcmp(category_text(&f, STUDIO_SETTINGS_TEXT, "badge"), "1") &&
            category_flag(&f, STUDIO_SETTINGS_APPEARANCE, "dim") &&
            yyjson_mut_get_num(member(f.page, "matches")) == 1,
        "searching shows the matching settings of every category, headed and "
        "counted"
    );
    set_text(&f, f.page, "query", "animation");
    studio_settings_sync(&f.view, f.doc, true);
    shown(&f, keys, sizeof keys);
    bool described = !strcmp(keys, "reduceMotion");
    set_text(&f, f.page, "query", "  graph  pins ");
    studio_settings_sync(&f.view, f.doc, true);
    shown(&f, keys, sizeof keys);
    test_check(described && !strcmp(keys, "nodes.titleFlow"), "search matches keywords and descriptions, every word of the query");

    yyjson_mut_set_bool(member(vim, "value"), true);
    studio_settings_sync(&f.view, f.doc, true);
    bool toggled = f.prefs.code_vim &&
                   !strcmp(yyjson_mut_get_str(member(vim, "value")), "true") &&
                   yyjson_mut_get_bool(member(vim, "modified")) &&
                   category_flag(&f, STUDIO_SETTINGS_TEXT, "dot");
    set_text(&f, vim, "value", yyjson_mut_get_str(member(vim, "default")));
    studio_settings_sync(&f.view, f.doc, true);
    test_check(
        toggled && !f.prefs.code_vim && !yyjson_mut_get_bool(member(vim, "modified")) &&
            !category_flag(&f, STUDIO_SETTINGS_TEXT, "dot"),
        "a row's switch sets its preference and marks it modified; Reset puts "
        "the default back"
    );

    yyjson_mut_val *theme = row_named(&f, "theme"), *scale = row_named(&f, "uiScale");
    set_text(&f, theme, "value", "purple");
    set_text(&f, scale, "value", "1.25");
    studio_settings_sync(&f.view, f.doc, true);
    test_check(
        f.prefs.theme == STUDIO_THEME_SYSTEM &&
            !strcmp(yyjson_mut_get_str(member(theme, "value")), "system") &&
            f.prefs.ui_scale == 1.25f &&
            yyjson_mut_get_bool(member(scale, "modified")),
        "an invalid value is dropped and its row shows the setting again"
    );

    yyjson_mut_val *font = row_named(&f, "code.fontSize");
    bool fraction = !strcmp(yyjson_mut_get_str(member(font, "position")), "0.208333");
    yyjson_mut_set_real(member(font, "position"), 0.2501);
    studio_settings_sync(&f.view, f.doc, true);
    test_check(
        fraction && f.prefs.code_font_size == 14 &&
            !strcmp(yyjson_mut_get_str(member(font, "value")), "14") &&
            !strcmp(yyjson_mut_get_str(member(font, "position")), "0.25"),
        "a slider moves from 0 to 1 and sets its number on a step"
    );
    page_close(&f);
}

/* A shortcut row lists its default's aliases until it is rebound; a chord
 * two commands share counts on the page, and "conflict" finds them. */
static void page_shortcut_aliases()
{
    page_fixture f;
    page_open(&f);
    set_text(&f, f.page, "category", "keyboard");
    studio_settings_sync(&f.view, f.doc, true);
    yyjson_mut_val *search = row_named(&f, "command.search");
    bool aliased = !strcmp(yyjson_mut_get_str(member(search, "aliases")), "Also Ctrl+P") &&
                   !yyjson_mut_get_str(
                       member(row_named(&f, "command.save"), "aliases")
                   )[0] &&
                   yyjson_mut_get_real(member(f.page, "conflicts")) == 0;
    snprintf(f.prefs.shortcuts[STUDIO_COMMAND_SAVE], STUDIO_CHORD_SIZE, "Mod+P");
    studio_settings_sync(&f.view, f.doc, false);
    bool counted = yyjson_mut_get_real(member(f.page, "conflicts")) == 2;
    set_text(&f, f.page, "query", "conflict");
    studio_settings_sync(&f.view, f.doc, true);
    char keys[256];
    shown(&f, keys, sizeof keys);
    test_check(
        aliased && counted && !strcmp(keys, "command.save command.search"),
        "a shortcut row lists its aliases, and the Keyboard page counts and "
        "finds conflicting chords"
    );
    page_close(&f);
}

/* Stable bindings may reuse parsed presentation, while live command state
 * and edits from either the model or shortcut recorder still reach the page. */
static void page_shortcut_refresh()
{
    page_fixture f;
    page_open(&f);
    set_text(&f, f.page, "category", "keyboard");
    studio_settings_sync(&f.view, f.doc, true);
    yyjson_mut_val *save = row_named(&f, "command.save");
    yyjson_mut_val *search = row_named(&f, "command.search");
    static const char *const CTRL_S[] = { "Ctrl", "S" };
    static const char *const CMD_P[] = { "⌘", "P" };
    static const char *const CMD_S[] = { "⌘", "S" };
    test_check(!studio_settings_sync(&f.view, f.doc, false) && keys_are(save, CTRL_S, 2), "unchanged Settings sync retains shortcut keys without dirtying the page");

    set_text(&f, save, "value", "Mod+P");
    studio_settings_sync(&f.view, f.doc, true);
    bool alias = !strcmp(yyjson_mut_get_str(member(save, "conflict")), "Also assigned to Search");
    f.commands[1].label = "Find Commands";
    f.view.recording = STUDIO_COMMAND_SAVE + 1;
    studio_settings_sync(&f.view, f.doc, false);
    bool live = !strcmp(yyjson_mut_get_str(member(search, "label")), "Find Commands") &&
                !strcmp(yyjson_mut_get_str(member(save, "conflict")), "Also assigned to Find Commands") &&
                yyjson_mut_get_bool(member(save, "recording"));
    f.view.recording = 0;
    set_text(&f, search, "value", "Mod+Shift+P");
    studio_settings_sync(&f.view, f.doc, true);
    bool replacement = !yyjson_mut_get_str(member(save, "conflict"))[0];
    set_text(&f, search, "value", "Mod+K");
    studio_settings_sync(&f.view, f.doc, true);
    bool reset = !f.prefs.shortcuts[STUDIO_COMMAND_SEARCH][0] &&
                 !strcmp(yyjson_mut_get_str(member(save, "conflict")), "Also assigned to Find Commands");
    test_check(alias && live && replacement && reset, "Settings refreshes dynamic labels and recording, replacement removes a default alias conflict, and reset restores it");

    f.view.mac = true;
    studio_settings_sync(&f.view, f.doc, false);
    bool platform = keys_are(save, CMD_P, 2);
    /* A captured preference changes outside the row-write path. */
    f.prefs.shortcuts[STUDIO_COMMAND_SAVE][0] = '\0';
    studio_settings_sync(&f.view, f.doc, false);
    test_check(
        platform && keys_are(save, CMD_S, 2) &&
            !yyjson_mut_get_str(member(save, "conflict"))[0] &&
            !studio_settings_sync(&f.view, f.doc, false),
        "platform and external binding changes refresh keys and conflicts, "
        "then settle"
    );

    yyjson_mut_arr_clear(member(f.page, "rows"));
    studio_settings_sync(&f.view, f.doc, false);
    bool rebuilt = keys_are(row_named(&f, "command.save"), CMD_S, 2);
    yyjson_mut_doc *old = f.doc;
    f.doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(f.doc, yyjson_mut_obj(f.doc));
    studio_settings_add(f.doc);
    f.page = member(yyjson_mut_doc_get_root(f.doc), "settings");
    studio_settings_sync(&f.view, f.doc, false);
    test_check(rebuilt && keys_are(row_named(&f, "command.save"), CMD_S, 2), "rebuilt and replacement Settings models receive keys even with unchanged bindings");
    yyjson_mut_doc_free(old);
    page_close(&f);
}

static void page_keyboard_and_project()
{
    page_fixture f;
    page_open(&f);
    set_text(&f, f.page, "category", "keyboard");
    studio_settings_sync(&f.view, f.doc, true);
    char keys[512];
    shown(&f, keys, sizeof keys);
    static const char *const SAVE[] = { "Ctrl", "S" };
    static const char *const SEARCH[] = { "Ctrl", "K" };
    yyjson_mut_val *save = row_named(&f, "command.save");
    yyjson_mut_val *search = row_named(&f, "command.search");
    yyjson_mut_val *frame = row_named(&f, "command.frame");
    bool listed = !strcmp(keys, "command.save command.search command.reload command.frame") &&
                  keys_are(row_named(&f, "command.save"), SAVE, 2) &&
                  keys_are(row_named(&f, "command.search"), SEARCH, 2) &&
                  keys_are(row_named(&f, "command.reload"), NULL, 0) &&
                  !strcmp(yyjson_mut_get_str(member(save, "value")), "Mod+S") &&
                  !strcmp(yyjson_mut_get_str(member(save, "default")), "Mod+S") &&
                  yyjson_mut_get_str(member(frame, "context"))[0] != '\0';
    set_text(&f, f.page, "query", "revert");
    studio_settings_sync(&f.view, f.doc, true);
    shown(&f, keys, sizeof keys);
    test_check(
        listed && !strcmp(keys, "command.reload"),
        "Keyboard lists each command's shortcut as keys, and search finds "
        "commands"
    );

    set_text(&f, f.page, "query", "");
    set_text(&f, search, "value", "Mod+S");
    studio_settings_sync(&f.view, f.doc, true);
    bool override = !strcmp(f.prefs.shortcuts[STUDIO_COMMAND_SEARCH], "Mod+S") &&
                    !strcmp(yyjson_mut_get_str(member(search, "conflict")), "Also assigned to Save");

    /* The Record button's settings.record action (studio_panel_route). */
    f.view.recording = STUDIO_COMMAND_SEARCH + 1;
    studio_settings_sync(&f.view, f.doc, true);
    bool recording = f.view.recording == STUDIO_COMMAND_SEARCH + 1 &&
                     yyjson_mut_get_bool(member(search, "recording"));
    bool recorded = studio_shortcut_record(
        &f.prefs,
        &f.view.recording,
        (studio_key){ .key = SDLK_P, .mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT },
        f.view.mac
    );
    /* A data-model write can still report the pre-recording value. The
       settings row's bound value must prevent it from replacing the capture. */
    studio_settings_sync(&f.view, f.doc, true);
    bool captured = recorded && f.view.recording == 0 &&
                    !strcmp(f.prefs.shortcuts[STUDIO_COMMAND_SEARCH], "Mod+Shift+P") &&
                    !strcmp(yyjson_mut_get_str(member(search, "value")), "Mod+Shift+P") &&
                    !yyjson_mut_get_bool(member(search, "recording"));

    set_text(&f, search, "value", "Mod+K");
    studio_settings_sync(&f.view, f.doc, true);
    bool reset = !f.prefs.shortcuts[STUDIO_COMMAND_SEARCH][0] &&
                 !strcmp(yyjson_mut_get_str(member(search, "value")), "Mod+K");
    test_check(
        override && recording && captured && reset,
        "Keyboard edits a chord, reports a conflict, records a "
        "replacement and stores defaults without an override"
    );

    /* The Record button's settings.record action (studio_panel_route). */
    f.view.recording = STUDIO_COMMAND_SEARCH + 1;
    studio_settings_sync(&f.view, f.doc, true);
    bool started = f.view.recording == STUDIO_COMMAND_SEARCH + 1;
    set_text(&f, f.page, "query", "");
    set_text(&f, f.page, "scope", "project");
    studio_settings_sync(&f.view, f.doc, true);
    shown(&f, keys, sizeof keys);
    yyjson_mut_val *root = row_named(&f, "project.ui");
    test_check(started && !f.view.recording, "leaving a shortcut row cancels recording");
    bool scoped = !strcmp(keys, "project.name project.startScene project.ui") &&
                  !strcmp(yyjson_mut_get_str(member(f.page, "category")), "project") &&
                  category_flag(&f, STUDIO_SETTINGS_APPEARANCE, "hidden") &&
                  !strcmp(text_of_page(&f, "project"), "Arena") &&
                  yyjson_mut_arr_size(member(root, "choices")) == 3;
    set_text(&f, root, "value", "Hud");
    studio_settings_sync(&f.view, f.doc, true);
    bool chosen = !strcmp(f.view.fields[STUDIO_PROJECT_UI], "Hud") &&
                  yyjson_mut_get_bool(member(root, "modified"));
    set_text(&f, root, "value", "Missing");
    studio_settings_sync(&f.view, f.doc, true);
    test_check(
        scoped && chosen && !strcmp(f.view.fields[STUDIO_PROJECT_UI], "Hud") &&
            !strcmp(yyjson_mut_get_str(member(root, "value")), "Hud"),
        "the Project tab sets the root document to one of the project's ui "
        "documents"
    );

    yyjson_mut_val *name = row_named(&f, "project.name");
    yyjson_mut_val *start = row_named(&f, "project.startScene");
    bool shown_name = !strcmp(yyjson_mut_get_str(member(name, "value")), "Arena") &&
                      !strcmp(yyjson_mut_get_str(member(name, "control")), "text") &&
                      !yyjson_mut_get_bool(member(name, "modified"));
    set_text(&f, name, "value", "Neon Swarm");
    set_text(&f, start, "value", "scenes/Boss.kdl");
    studio_settings_sync(&f.view, f.doc, true);
    bool renamed = !strcmp(f.view.fields[STUDIO_PROJECT_NAME], "Neon Swarm") &&
                   !strcmp(f.view.fields[STUDIO_PROJECT_START_SCENE], "scenes/Boss.kdl");
    set_text(&f, name, "value", "");
    set_text(&f, start, "value", "scenes/Missing.kdl");
    studio_settings_sync(&f.view, f.doc, true);
    yyjson_mut_val *first = yyjson_mut_arr_get(member(start, "choices"), 0);
    test_check(
        shown_name && renamed &&
            !strcmp(f.view.fields[STUDIO_PROJECT_NAME], "Neon Swarm") &&
            !strcmp(f.view.fields[STUDIO_PROJECT_START_SCENE], "scenes/Boss.kdl") &&
            yyjson_mut_arr_size(member(start, "choices")) == 2 &&
            !strcmp(yyjson_mut_get_str(member(first, "label")), "Main") &&
            !yyjson_mut_get_bool(member(start, "modified")),
        "the Project tab names the project and picks its start scene, which "
        "neither can leave empty"
    );
    page_close(&f);
}

/* ---------- The authored page ---------- */

typedef struct
{
    rml_backend *backend;
    kit *k;
    kit_frame_input in;
    ui_mount *mount;
    rml_listener *listener;
    ui_data data;
    uint64_t writes, save_seen;
    game_save *save; /* the Studio world's, which panels bind */
    rml_element *host;
    page_fixture page;
} mount_fixture;

static void on_event(void *user, const rml_event *event)
{
    ui_mount_listen(user, event);
}

static void on_emit(void *user, const rml_event *event, const char *name, int argc, const char *const *args)
{
    ui_mount_emit(user, event, name, argc, args);
}

/* A frame in studio_ui_frame's order: the saved keys, the page, then the
 * preferences shared back to the saved keys. */
static void mount_frame(mount_fixture *f)
{
    ui_data_sync(&f->data, f->save);
    uint64_t writes = rml_data_model_writes(f->data.model);
    if (studio_settings_sync(&f->page.view, f->data.doc, writes != f->writes)) {
        rml_data_model_dirty(f->data.model, "settings");
    }
    f->writes = writes;
    studio_prefs_share(&f->page.prefs, f->save, &f->save_seen);
    kit_begin(f->k, &f->in);
    kit_root(f->k, "app");
    f->host = kit_modal_host(f->k, "Settings");
    if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
        kit_style_sheet(f->k, "Settings", ui_mount_style(f->mount));
    }
    kit_end(f->k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    f->in.time += 1.0 / 60.0;
}

/* The visible elements `selector` matches in the page. */
static int visible(mount_fixture *f, const char *selector, rml_element **first)
{
    rml_element *matches[256];
    int count = rml_element_query_selector_all(f->host, selector, matches, 256);
    int shown = 0;
    for (int i = 0; i < count && i < 256; i++) {
        if (rml_element_is_visible(matches[i], true)) {
            *first = shown++ ? *first : matches[i];
        }
    }
    return shown;
}

static void click(mount_fixture *f, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float x = at.x + size.x * 0.5f, y = at.y + size.y * 0.5f;
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        rml_backend_process_input(f->backend, &(rml_input){ .type = types[i], .x = x, .y = y });
    }
}

static bool centered_caret(rml_element *input)
{
    if (!input) {
        return false;
    }
    char line[32];
    rml_element_property(input, "line-height", line, sizeof line);
    float height = (float)atof(line);
    rml_vec2 hit = rml_element_box_size(input, RML_BOX_BORDER);
    rml_vec2 content = rml_element_box_size(input, RML_BOX_CONTENT);
    rml_vec2 hit_at = rml_element_absolute_offset(input, RML_BOX_BORDER);
    rml_vec2 content_at = rml_element_absolute_offset(input, RML_BOX_CONTENT);
    float top = content_at.y - hit_at.y;
    float bottom = hit.y - top - content.y;
    return height > 0 && height < hit.y && fabsf(content.y - height) <= 1 &&
           top >= 1 && fabsf(top - bottom) <= 1;
}

static void page_mount()
{
    mount_fixture f = { .in = { .dp_ratio = 1, .width = 1280, .height = 800 },
                        .save = game_save_new() };
    f.backend = rml_backend_create(1280, 800, 1);
    f.k = test_kit_create(rml_backend_context(f.backend), "studio.rml");
    f.mount = authoring_ui_mount_new("projects/studio/content", "Settings");
    f.listener = rml_listener_create(on_event, f.mount);
    page_open(&f.page);
    ui_data_init(&f.data, rml_backend_context(f.backend), f.page.doc, on_emit, f.mount);
    f.page.page = yyjson_mut_obj_get(yyjson_mut_doc_get_root(f.page.doc), "settings");
    for (int i = 0; i < 4; i++) {
        mount_frame(&f);
    }
    rml_element *first = NULL;
    int rows = visible(&f, ".setting", &first);
    bool loaded = !ui_mount_error(f.mount)[0] && rows == 6 &&
                  f.page.prefs.code_font_size == 13;
    studio_apply_font_settings(f.k, &f.page.prefs);
    mount_frame(&f);
    test_check(centered_caret(rml_element_query_selector(f.host, ".search-input")), "the authored Settings search centers its caret inside the full hit area");
    rml_element *default_body = rml_context_document(rml_backend_context(f.backend), 0);
    char default_family[64], default_size[32], default_line[32];
    rml_element_property(default_body, "font-family", default_family, sizeof default_family);
    rml_element_property(default_body, "font-size", default_size, sizeof default_size);
    rml_element_property(default_body, "line-height", default_line, sizeof default_line);
    rml_element *numeric = rml_element_query_selector(f.host, ".readout");
    char numeric_family[64] = { 0 };
    if (numeric) {
        rml_element_property(numeric, "font-family", numeric_family, sizeof numeric_family);
    }
    test_check(
        !strcmp(default_family, "geist") && !strcmp(default_size, "13dp") &&
            !strcmp(default_line, "18dp") && kit_metric(f.k, "--h") == 26 &&
            kit_metric(f.k, "--row") == 26 && kit_metric(f.k, "--header-h") == 32 &&
            kit_metric(f.k, "--topbar-h") == 40 &&
            kit_metric(f.k, "--status-h") == 28 && kit_metric(f.k, "--panel-h") == 30 &&
            numeric && !strcmp(numeric_family, "GeistNumeric"),
        "default font, numeric face and authored chrome metrics agree with the "
        "compact design"
    );
    rml_element *theme_select = NULL;
    int dracula = choice_named(setting_named("theme"), "dracula");
    bool themes = visible(&f, "select", &theme_select) == 2 && dracula >= 0;
    if (themes) {
        click(&f, theme_select);
        for (int i = 0; i < dracula; i++) {
            rml_backend_process_input(
                f.backend,
                &(rml_input){ .type = RML_INPUT_KEY_PRESS, .key = RML_INPUT_KEY_DOWN }
            );
            rml_backend_process_input(
                f.backend,
                &(rml_input){ .type = RML_INPUT_KEY_RELEASE, .key = RML_INPUT_KEY_DOWN }
            );
        }
        rml_backend_process_input(
            f.backend,
            &(rml_input){ .type = RML_INPUT_KEY_PRESS, .key = RML_INPUT_KEY_ENTER }
        );
        rml_backend_process_input(
            f.backend,
            &(rml_input){ .type = RML_INPUT_KEY_RELEASE, .key = RML_INPUT_KEY_ENTER }
        );
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    test_check(
        themes && f.page.prefs.theme == STUDIO_THEME_DRACULA,
        "the authored Appearance select applies a palette chosen with the "
        "keyboard"
    );
    studio_prefs font_prefs = f.page.prefs;
    studio_setting_parse(&font_prefs, setting_named("font.standardFamily"), "Noto Sans");
    studio_setting_parse(&font_prefs, setting_named("font.standardSize"), "18");
    studio_setting_parse(&font_prefs, setting_named("code.fontFamily"), "Inconsolata");
    studio_setting_parse(&font_prefs, setting_named("code.fontSize"), "16");
    studio_apply_font_settings(f.k, &font_prefs);
    mount_frame(&f);
    rml_element *doc = rml_context_document(rml_backend_context(f.backend), 0);
    rml_element *body = doc;
    rml_element *font_control = rml_element_query_selector(f.host, ".search-input");
    rml_element *mono_probe = rml_element_create_child(f.host, "span", NULL);
    rml_element_set_attribute(mono_probe, "class", "code-completions");
    rml_element_set_inner_rml(mono_probe, "Glyphs");
    /* On every kit document: the dialog is in the layer's. */
    kit_set_body_class(f.k, "dark", true);
    rml_backend_update(f.backend, 1.0 / 60.0);
    char family[64] = { 0 }, size[32] = { 0 }, line[32] = { 0 };
    char mono_family[64] = { 0 }, mono_size[32] = { 0 };
    if (body) {
        rml_element_property(body, "font-family", family, sizeof family);
        rml_element_property(body, "font-size", size, sizeof size);
        rml_element_property(body, "line-height", line, sizeof line);
    }
    if (numeric) {
        rml_element_property(numeric, "font-family", numeric_family, sizeof numeric_family);
    }
    rml_element_property(mono_probe, "font-family", mono_family, sizeof mono_family);
    rml_element_property(mono_probe, "font-size", mono_size, sizeof mono_size);
    uintptr_t standard_face = rml_font_face("noto sans", RML_FONT_NORMAL, 400, 18);
    uintptr_t mono_face = rml_font_face("inconsolata", RML_FONT_NORMAL, 400, 16);
    rml_font_metrics standard_metrics = { 0 }, mono_metrics = { 0 };
    if (standard_face) {
        standard_metrics = rml_font_face_metrics(standard_face);
    }
    if (mono_face) {
        mono_metrics = rml_font_face_metrics(mono_face);
    }
    float compact_height = font_control
                             ? rml_element_box_size(font_control, RML_BOX_BORDER)
                                   .y
                             : 0;
    test_check(centered_caret(font_control), "applied larger font settings keep the search caret shorter and centered");
    bool applied = body && !strcmp(family, "noto sans") &&
                   !strcmp(size, "18dp") && !strcmp(line, "25dp") &&
                   compact_height == 36 && !strcmp(numeric_family, "noto sans") &&
                   !strcmp(mono_family, "inconsolata") &&
                   !strcmp(mono_size, "15dp") && standard_face && mono_face &&
                   rml_font_string_width(standard_face, "Glyphs") > 0 &&
                   rml_font_string_width(mono_face, "Glyphs") > 0 &&
                   standard_metrics.line_spacing <= 25 &&
                   mono_metrics.line_spacing <= 20;
    kit_set_body_class(f.k, "comfortable", true);
    rml_backend_update(f.backend, 1.0 / 60.0);
    float comfortable_height = font_control
                                 ? rml_element_box_size(font_control, RML_BOX_BORDER)
                                       .y
                                 : 0;
    bool comfortable_room = body && comfortable_height == 42;
    test_check(centered_caret(font_control), "comfortable density preserves caret geometry with the larger font");
    studio_apply_graph_font_settings(f.k, &font_prefs);
    mount_frame(&f);
    font_control = rml_element_query_selector(f.host, ".search-input");
    if (body) {
        rml_element_property(body, "font-family", family, sizeof family);
        rml_element_property(body, "font-size", size, sizeof size);
        rml_element_property(body, "line-height", line, sizeof line);
    }
    float graph_control_height = font_control
                                   ? rml_element_box_size(font_control, RML_BOX_BORDER)
                                         .y
                                   : 0;
    bool graph_geometry = body && !strcmp(family, "noto sans") &&
                          !strcmp(size, "12dp") && !strcmp(line, "16dp") &&
                          graph_control_height == 22;
    test_check(centered_caret(font_control), "graph font settings retain a centered caret at their fixed control pitch");
    if (!applied || !comfortable_room || !graph_geometry) {
        fprintf(
            stderr,
            "font diagnostics: body=%p family='%s' size='%s' line='%s' "
            "control=%.2f/%.2f/%.2f mono-family='%s' mono-size='%s' "
            "standard-face=%lu mono-face=%lu "
            "standard-metrics=%.2f mono-metrics=%.2f "
            "standard-width=%d mono-width=%d comfortable=%d graph=%d\n",
            (void *)body,
            family,
            size,
            line,
            compact_height,
            comfortable_height,
            graph_control_height,
            mono_family,
            mono_size,
            (unsigned long)standard_face,
            (unsigned long)mono_face,
            standard_metrics.line_spacing,
            mono_metrics.line_spacing,
            standard_face ? rml_font_string_width(standard_face, "Glyphs") : 0,
            mono_face ? rml_font_string_width(mono_face, "Glyphs") : 0,
            comfortable_room,
            graph_geometry
        );
    }
    test_check(
        loaded && applied && comfortable_room && graph_geometry &&
            !strcmp(studio_code_font(&font_prefs), "inconsolata"),
        "font changes apply to registered faces, scale line and control "
        "geometry, and keep graph metrics at their native size"
    );
    set_text(&f.page, f.page.page, "query", "vim");
    rml_data_model_dirty(f.data.model, "settings");
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    rml_element *vim = NULL;
    bool one = visible(&f, ".switch input", &vim) == 1;
    if (one) {
        click(&f, vim);
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    test_check(loaded, "the Settings page loads its six Appearance controls");
    test_check(one, "searching the Settings page for vim shows one switch");
    test_check(f.page.prefs.code_vim, "the filtered switch sets its preference");

    rml_element *reset = NULL;
    bool offered = visible(&f, ".setting-reset", &reset) == 1;
    if (offered) {
        click(&f, reset);
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    test_check(
        offered && !f.page.prefs.code_vim && !rml_element_has_attribute(vim, "checked"),
        "Reset puts a switched setting back"
    );

    rml_element *items[8];
    int count = rml_element_query_selector_all(f.host, ".nav-item input", items, 8);
    rml_element *graph = NULL;
    for (int i = 0; i < count && i < 8; i++) {
        char value[32];
        if (rml_element_attribute(items[i], "value", value, sizeof value) > 0 &&
            !strcmp(value, "graph")) {
            graph = items[i];
        }
    }
    if (graph) {
        click(&f, graph);
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    rml_element *select = NULL, *radio = NULL;
    bool graph_shown = graph && !strcmp(text_of_page(&f.page, "category"), "graph") &&
                       !text_of_page(&f.page, "query")[0] &&
                       visible(&f, "select", &select) == 1 &&
                       visible(&f, ".switch input", &radio) == 4;
    uint64_t settled = rml_data_model_writes(f.data.model);
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    test_check(
        graph_shown && rml_data_model_writes(f.data.model) == settled,
        "choosing a category leaves search and shows its controls, which "
        "write nothing until used"
    );

    count = rml_element_query_selector_all(f.host, ".nav-item input", items, 8);
    rml_element *keyboard = NULL;
    for (int i = 0; i < count && i < 8; i++) {
        char value[32];
        if (rml_element_attribute(items[i], "value", value, sizeof value) > 0 &&
            !strcmp(value, "keyboard")) {
            keyboard = items[i];
        }
    }
    if (keyboard) {
        click(&f, keyboard);
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    set_text(&f.page, f.page.page, "query", "Search");
    rml_data_model_dirty(f.data.model, "settings");
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    rml_element *record_button = NULL;
    bool record_visible = keyboard &&
                          visible(&f, ".record-shortcut", &record_button) == 1;
    if (record_visible) {
        click(&f, record_button);
    }
    /* The button sends emit('settings.record', row.key), which Studio routes
       to the Settings source (studio_panel_route); here the test does. */
    mount_frame(&f);
    ui_activation a;
    bool emitted = false;
    while (ui_mount_take(f.mount, &a)) {
        if (!strcmp(a.event, "settings.record") && a.argc == 1 &&
            !strcmp(a.args[0], "command.search")) {
            emitted = true;
            f.page.view.recording = STUDIO_COMMAND_SEARCH + 1;
        }
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    bool listening = record_visible && emitted &&
                     f.page.view.recording == STUDIO_COMMAND_SEARCH + 1 &&
                     visible(&f, ".recording", &record_button) == 1 &&
                     visible(&f, ".record-shortcut", &record_button) == 0;
    bool captured = studio_shortcut_record(
        &f.page.prefs,
        &f.page.view.recording,
        (studio_key){ .key = SDLK_P, .mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT },
        f.page.view.mac
    );
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    rml_element *shortcut_reset = NULL;
    bool reset_visible = visible(&f, ".setting-reset", &shortcut_reset) == 1;
    if (reset_visible) {
        click(&f, shortcut_reset);
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    test_check(
        listening && captured && reset_visible &&
            !f.page.prefs.shortcuts[STUDIO_COMMAND_SEARCH][0],
        "the mounted Keyboard row records a chord and Reset restores its "
        "default"
    );

    set_text(&f.page, f.page.page, "query", "");
    set_text(&f.page, f.page.page, "scope", "project");
    rml_data_model_dirty(f.data.model, "settings");
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    rml_element *name = NULL;
    bool project = visible(&f, ".text-control input", &name) == 1 &&
                   visible(&f, "select", &select) == 2;
    if (project) {
        click(&f, name);
        rml_backend_process_input(
            f.backend,
            &(rml_input){ .type = RML_INPUT_TEXT_INPUT, .text = " Lab", .text_size = 4 }
        );
        click(&f, rml_element_query_selector(f.host, ".page-title"));
    }
    for (int i = 0; i < 3; i++) {
        mount_frame(&f);
    }
    test_check(
        project && !strcmp(f.page.view.fields[STUDIO_PROJECT_NAME], "Arena Lab"),
        "the Project tab shows the name as a text field, commits it on blur, "
        "and shows start scene and root document selects"
    );

    kit_destroy(f.k);
    ui_data_free(&f.data);
    rml_backend_destroy(f.backend);
    ui_mount_free(f.mount);
    rml_listener_destroy(f.listener);
    page_view_free(&f.page.view);
    game_save_free(f.save);
}

/* Drive emissions only when the kit asks for them, while its context advances
 * on every simulation frame. Toast expiry must survive that split. */
static void independent_ui_deadlines()
{
    rml_backend *backend = rml_backend_create(800, 600, 1);
    kit *k = test_kit_create(rml_backend_context(backend), "studio.rml");
    kit_frame_input in = { .width = 800,
                           .height = 600,
                           .dp_ratio = 1,
                           .mouse_x = -1000,
                           .mouse_y = -1000 };
    kit_begin(k, &in);
    kit_root(k, "app");
    kit_end(k);
    rml_backend_update(backend, 1.0 / 60.0);

    in.time = 1.0 / 60.0;
    kit_begin(k, &in);
    kit_root(k, "app");
    kit_toast(k, 0, &(kit_toast_desc){ .title = "Scheduled", .duration = 0.2 });
    kit_end(k);
    rml_backend_update(backend, 1.0 / 60.0);
    bool waiting = kit_idle_time(k) < 0.2;
    double next = in.time + kit_idle_time(k);
    for (int i = 2; i <= 180; i++) {
        in.time = i / 60.0;
        if (in.time >= next) {
            kit_begin(k, &in);
            kit_root(k, "app");
            kit_end(k);
            rml_backend_update(backend, 1.0 / 60.0);
            next = in.time + kit_idle_time(k);
        } else {
            rml_backend_update(backend, 1.0 / 60.0);
        }
    }
    test_check(
        waiting && !test_query(rml_backend_context(backend), ".toast"),
        "independent UI deadlines expire a toast while simulation keeps "
        "ticking"
    );
    kit_destroy(k);
    rml_backend_destroy(backend);
}

void test_studio_settings()
{
    independent_ui_deadlines();
    prefs_round_trip();
    scene_view_preferences();
#ifndef __EMSCRIPTEN__
    prefs_file_override();
#endif
    font_preferences();
    invalid_values();
    theme_presets();
    workspace_presets_round_trip();
    shortcut_chords();
    shortcut_dispatch();
    shortcut_recording();
    shortcut_defaults();
    shortcut_areas();
    shortcut_text_keys();
    shortcut_sheet();
    save_mirror();
    page_rows();
    page_keyboard_and_project();
    page_shortcut_refresh();
    page_shortcut_aliases();
    page_mount();
}
