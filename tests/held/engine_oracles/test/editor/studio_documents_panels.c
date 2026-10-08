/* Authored panels and game UI in Studio: UI wiring (workstream K), the
 * Canvas preview, Settings, modals, the widget gallery and fields that keep
 * their editing keys. */

#include "editor/workspace/viewport.h"
#include "assets/code_source.h"
#include "assets/project.h"
#include "assets/ui_events.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/input.h"
#include "graph/document/serialize.h"
#include "graph/nodes/component_nodes.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "render/screenshot.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/file_io.h"
#include "support/text_overlay.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/input_feel.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BANK_STYLES PROJECT "/content/ui/Bank.rcss"

static int bank_errors(const studio *s)
{
    int count = 0;
    for (int i = 0; i < acount(s->records); i++) {
        count += s->records[i].kind == KIT_TOAST_ERROR &&
                 strstr(s->records[i].text, "ui/Bank.rml") != NULL;
    }
    return count;
}

/* A broken ui asset's error shows in the Scene and Problems, and Output
   records it once per change. */
static void ui_errors_recorded()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    bool clean = !s->ui_error[0];
    text_overlay_set(BANK, "");
    frames(&f, 3);
    bool shown = strstr(s->ui_error, "ui/Bank.rml") != NULL && bank_errors(s) == 1;
    frames(&f, 3);
    bool once = bank_errors(s) == 1;
    text_overlay_set(BANK, "<div><body/></div>\n");
    frames(&f, 3);
    bool changed = bank_errors(s) == 2;
    text_overlay_set(BANK, NULL);
    frames(&f, 3);
    bool fixed = !s->ui_error[0] && bank_errors(s) == 2;
    if (!(clean && shown && once && changed && fixed)) {
        printf("  %d %d %d %d %d: %s\n", clean, shown, once, changed, fixed, s->ui_error);
    }
    test_check(
        clean && shown && once && changed && fixed,
        "a game UI error shows in the Scene and Output records it once "
        "per change"
    );
    fixture_free(&f);
}

/* Teller hears the Bank the Vault shows. */
static void ui_events_offered()
{
    studio_fixture f = vault_open();
    frames(&f, 3);
    ui_event *events = studio_behavior_ui_events(f.studio);
    bool ok = acount(events) == 2 && !strcmp(events[0].name, "Extract") &&
              !strcmp(events[0].description, "Sent when the player clicks #extract in Bank") &&
              !strcmp(events[1].name, "Close");
    test_check(
        ok,
        "the Add menu offers the events of the GameUi documents that "
        "reach the open behavior"
    );
    ui_events_free(events);
    fixture_free(&f);
}

static void click(render_ui *ui, float x, float y)
{
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        render_ui_input(ui, &(rml_input){ .type = types[i], .x = x, .y = y });
    }
}

/* The asset's root in the preview's documents, found from the element under
   the pointer. */
static rml_element *preview_root(render_ui *ui)
{
    rml_element *hover = rml_context_hover_element(render_ui_context(ui));
    return hover ? rml_element_query_selector(rml_element_owner_document(hover), "[data-ui=Bank]")
                 : NULL;
}

/* The preview shows the asset alone with the backdrop's class and logs the
   events it emits, which reach no behavior. */
static void preview_logs_actions()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    studio_preview_ui(s, "Bank");
    /* The stage lays out, then the frame's size holds for two draws. */
    frames(&f, 8);
    studio_canvas *c = s->canvas;
    render_ui *ui = engine_viewport_ui(c->viewport);
    click(ui, 5, 5);
    rml_element *root = preview_root(ui);
    bool alone = root && rml_element_query_selector_all(
                             rml_element_owner_document(root),
                             "[data-ui=Bank]",
                             NULL,
                             0
                         ) == 1;
    bool dark = root && rml_element_is_class_set(root, "dark");
    frames(&f, 1);
    bool logged = acount(c->actions) == 1 &&
                  !strcmp(c->actions[0].event, "Extract") &&
                  !strcmp(c->actions[0].element, "#extract");
    click(ui, 5, 5);
    c->light = true;
    frames(&f, 1);
    render_ui_input(ui, &(rml_input){ .type = RML_INPUT_POINTER_MOVE, .x = 5, .y = 5 });
    root = preview_root(ui);
    bool light = root && rml_element_is_class_set(root, "light") &&
                 !rml_element_is_class_set(root, "dark");
    bool counted = acount(c->actions) == 1 && c->actions[0].count == 2;
    studio_preview_ui(s, NULL);
    bool closed = !studio_previewed_ui(s);
    /* The scene's surface lays out again, then the closed preview frees. */
    frames(&f, 3);
    closed &= !s->canvas;
    studio_command_run(s, STUDIO_COMMAND_PREVIEW_UI);
    bool picking = s->palette.client == STUDIO_PALETTE_ASSETS &&
                   !strcmp(s->pick.key, STUDIO_PICK_PREVIEW) &&
                   studio_palette_is_open(s);
    if (!(picking && alone && dark && logged && light && counted && closed)) {
        printf("  %d %d %d %d %d %d %d\n", picking, alone, dark, logged, light, counted, closed);
    }
    test_check(
        picking && alone && dark && logged && light && counted && closed,
        "the Canvas preview shows a ui asset alone, themed, and logs the "
        "events it emits"
    );
    fixture_free(&f);
}

static bool preview_geometry_matches(studio *s)
{
    rml_element *image = s->canvas->frame;
    assert(image);
    rml_vec2 size = rml_element_box_size(image, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    int width, height;
    engine_viewport_pixel_size(s->canvas->viewport, &width, &height);
    return fabsf(size.x / ratio - s->surface.rect.w) <= 0.5f &&
           fabsf(size.y / ratio - s->surface.rect.h) <= 0.5f &&
           abs(width - (int)(s->surface.rect.w * s->prefs.ui_scale)) <= 1 &&
           abs(height - (int)(s->surface.rect.h * s->prefs.ui_scale)) <= 1;
}

/* Preview measurements must converge even when passive refresh is suspended. */
static void preview_geometry_settles_without_input()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 60);
    studio_preview_ui(s, "Bank");
    bool matched = true;
    const studio_canvas_size sizes[] = {
        STUDIO_CANVAS_FIT,
        STUDIO_CANVAS_PHONE,
        STUDIO_CANVAS_DESKTOP,
    };
    for (int size = 0; size < (int)(sizeof sizes / sizeof *sizes); size++) {
        s->canvas->size = sizes[size];
        s->ui_schedule.wake = true;
        frame(&f, false);
        /* No input or passive inspection should be needed to finish layout. */
        for (int i = 0; i < 10; i++) {
            s->ui_schedule.next = s->time + 10;
            frame(&f, false);
        }
        matched &= preview_geometry_matches(s);
    }
    test_check(matched, "Canvas preview texture and measured surface settle to the rendered aspect without input or passive refresh");
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_SCENE;
    studio_active_layout(s)->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_SCENE;
    frames(&f, 30);
    rml_element *panes[2];
    int count = test_query_all(rml_backend_context(s->backend), ".canvas-frame", panes, 2);
    bool two = count == 2 && rml_element_is_visible(panes[0], false) &&
               rml_element_is_visible(panes[1], false) && panes[0] != panes[1];
    if (two) {
        rml_vec2 first = rml_element_absolute_offset(panes[0], RML_BOX_BORDER);
        rml_vec2 second = rml_element_absolute_offset(panes[1], RML_BOX_BORDER);
        two = fabsf(first.x - second.x) > 0.5f || fabsf(first.y - second.y) > 0.5f;
    }
    test_check(two && preview_geometry_matches(s), "two visible Scene panes reconcile the exact pane that supplies the shared Canvas surface");
    s->active_until = 0;
    s->ui_schedule.next = s->time;
    uint64_t before = s->ui_schedule.frames;
    frames(&f, 60);
    uint64_t emissions = s->ui_schedule.frames - before;
    test_check(
        emissions >= 3 && emissions < 10 && !s->ui_schedule.wake,
        "settled Canvas geometry keeps the idle emission schedule bounded"
    );
    fixture_free(&f);
}

/* The Scene area follows the active document: a ui asset's UI document
   shows its preview, and the scene's document shows the scene again. */
static void preview_follows_document()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    char path[PATH_MAX];
    assert(realpath(BANK, path));
    bool opened = studio_open_ui_source(s, path, 1);
    frames(&f, 2);
    bool shown = opened && studio_previewed_ui(s) &&
                 !strcmp(studio_previewed_ui(s), "Bank");
    int scene = -1;
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        scene = f.scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE ? i : scene;
    }
    assert(scene >= 0 && scene_tabs_activate(f.scene, scene) == 0);
    frames(&f, 2);
    bool back = !studio_previewed_ui(s);
    test_check(
        shown && back,
        "the Scene area previews the active UI document's "
        "asset and returns to the scene with its document"
    );
    fixture_free(&f);
}

static bool logged_last(const studio *s, const char *event)
{
    const studio_canvas *c = s->canvas;
    return acount(c->actions) &&
           !strcmp(c->actions[acount(c->actions) - 1].event, event);
}

/* The elements of the Canvas's asset that `selector` matches, up to 8. */
static int canvas_elements(studio *s, const char *selector, rml_element **found)
{
    char query[192];
    snprintf(query, sizeof query, "[data-ui=%s] %s", s->canvas->asset, selector);
    rml_context *context = render_ui_context(engine_viewport_ui(s->canvas->viewport));
    return rml_element_query_selector_all(rml_context_document(context, 0), query, found, 8);
}

/* The variables the Canvas's documents bind. */
static yyjson_mut_val *canvas_variables(studio *s)
{
    render_ui *ui = engine_viewport_ui(s->canvas->viewport);
    return yyjson_mut_doc_get_root(render_ui_data(ui)->doc);
}

/* With the Studio project open, its ui assets are Studio's chrome, so the
   Canvas previews them over Studio's own data: TopBar lists the document
   tabs and labels its commands, whose studio.* events only reach the
   Actions log. A game's preview binds only save and system. */
static void preview_binds_studio_data()
{
    studio_fixture f = studio_project_open();
    studio *s = f.studio;
    frames(&f, 3);
    studio_preview_ui(s, "TopBar");
    frames(&f, 8);
    rml_element *found[8];
    /* Besides data-for's template. */
    int tabs = canvas_elements(s, ".doc-tab", found) - 1;
    bool listed = s->own_project && tabs > 0 && tabs == acount(f.scene->tabs);
    char label[64] = "";
    if (canvas_elements(s, ".search-trigger .label", found) == 1) {
        rml_element_inner_rml(found[0], label, sizeof label);
    }
    bool labeled = !strcmp(
        label,
        studio_command_describe(s, STUDIO_COMMAND_SEARCH).label
    );
    canvas_elements(s, ".search-trigger", found);
    rml_element_click(found[0]);
    frames(&f, 2);
    bool logged = logged_last(s, "studio.search") && !studio_palette_is_open(s);
    fixture_free(&f);

    f = vault_open();
    frames(&f, 3);
    studio_preview_ui(f.studio, "Bank");
    frames(&f, 3);
    yyjson_mut_val *variables = canvas_variables(f.studio);
    /* Besides save and system, each entity's self data is a variable of
       its own, "e<entity>". */
    int entities = 0;
    bool others = false;
    yyjson_mut_val *key;
    yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(variables);
    while ((key = yyjson_mut_obj_iter_next(&it))) {
        const char *name = yyjson_mut_get_str(key);
        bool entity = name[0] == 'e' && name[1] >= '0' && name[1] <= '9';
        entities += entity;
        others |= !entity && strcmp(name, "save") && strcmp(name, "system");
    }
    bool game = yyjson_mut_obj_get(variables, "save") &&
                yyjson_mut_obj_get(variables, "system") && entities > 0 && !others;
    if (!(listed && labeled && logged && game)) {
        printf("  %d %d %d %d: %d tabs, '%s'\n", listed, labeled, logged, game, tabs, label);
    }
    test_check(
        listed && labeled && logged && game,
        "a Studio-project Canvas preview binds Studio's data and only logs its "
        "commands; a game's binds save, system and entities' self data"
    );
    fixture_free(&f);
}

static void ui_wiring_checks()
{
    ui_errors_recorded();
    ui_events_offered();
    preview_logs_actions();
    preview_geometry_settles_without_input();
    preview_follows_document();
    preview_binds_studio_data();
}

/* The scene object showing a game document, the Vault. */
static ecs_entity vault_entity(const studio_fixture *f)
{
    for (int i = 0; i < acount(f->scene->actors); i++) {
        ecs_entity entity = f->scene->actors[i].entity;
        if (GAME_GameUi_has(f->scene->world, entity)) {
            return entity;
        }
    }
    assert(!"The Vault shows a game document");
    return (ecs_entity){ 0 };
}

/* The Canvas preview takes the keys typed while its area has the keyboard,
   so the asset's data-keys bindings send their events, and not while
   another area has it. */
static void preview_hears_keys()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    text_overlay_set(BANK, "<div id=\"extract\" autofocus data-keys=\"C:Close\">Extract</div>\n");
    text_overlay_set(BANK_STYLES, "#extract { display: block; height: 100%; tab-index: auto; }\n");
    frames(&f, 3);
    studio_preview_ui(s, "Bank");
    frames(&f, 8);
    s->focus_area = STUDIO_AREA_BOTTOM;
    key_event(SDL_SCANCODE_C, true);
    key_event(SDL_SCANCODE_C, false);
    frames(&f, 2);
    bool elsewhere = !logged_last(s, "Close");
    s->focus_area = STUDIO_AREA_TOP;
    key_event(SDL_SCANCODE_C, true);
    key_event(SDL_SCANCODE_C, false);
    frames(&f, 2);
    bool heard = logged_last(s, "Close");
    /* Studio keeps its keys there: Space still plays the scene. */
    key_event(SDL_SCANCODE_SPACE, true);
    key_event(SDL_SCANCODE_SPACE, false);
    frames(&f, 2);
    bool shortcut = app_scene_authoring_runtime_state(f.scene).playing;
    studio_runtime(s, APP_RUNTIME_STOP);
    test_check(
        elsewhere && heard && shortcut,
        "the focused Canvas preview takes keys, so data-keys bindings "
        "fire, and Studio keeps its shortcuts"
    );
    studio_preview_ui(s, NULL);
    frames(&f, 3);
    /* The Vault's behaviors count the Close events its document sends. */
    const char *const closes[] = { "Close" };
    ui_probe editing_heard = { 0 }, playing_heard = { 0 };
    test_ui_probe_events(f.scene->live, vault_entity(&f), closes, 1, &editing_heard);
    key_event(SDL_SCANCODE_C, true);
    key_event(SDL_SCANCODE_C, false);
    frames(&f, 2);
    bool editing = editing_heard.count == 0;
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 3);
    test_ui_probe_events(f.scene->live, vault_entity(&f), closes, 1, &playing_heard);
    key_event(SDL_SCANCODE_C, true);
    key_event(SDL_SCANCODE_C, false);
    /* The key reaches the document, whose activation reaches Vault's
       behaviors in the frame that follows. */
    frames(&f, 2);
    bool playing = playing_heard.count == 1;
    test_check(editing && playing, "the focused Scene's game documents take keys while the scene plays");
    text_overlay_set(BANK, NULL);
    text_overlay_set(BANK_STYLES, NULL);
    fixture_free(&f);
    test_ui_probes_free();
}

/* Recording consumes key and text events before the normal shortcut and
   game-input paths. A repeat must leave the recorder active for the next
   non-repeat keydown. */
static void settings_records_shortcut()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    studio_ui_show(s, "Settings");
    frames(&f, 4);
    rml_element *search_field = studio_element(s, ".search-input");
    key_event(SDL_SCANCODE_SPACE, true);
    key_event(SDL_SCANCODE_SPACE, false);
    frames(&f, 2);
    bool text_field_kept = search_field &&
                           !app_scene_authoring_runtime_state(f.scene).playing;

    /* panel_actions_route covers the Record control. Here, exercise the
       host after that control has requested a recording. */
    s->shortcut_recording = STUDIO_COMMAND_SEARCH + 1;
    bool recording = s->shortcut_recording == STUDIO_COMMAND_SEARCH + 1;
    SDL_Event lost = { .type = SDL_EVENT_WINDOW_FOCUS_LOST };
    lost.window.windowID = SDL_GetWindowID(cf_app_get_window());
    assert(SDL_PushEvent(&lost));
    frames(&f, 1);
    bool focus_canceled = recording && s->shortcut_recording == 0 &&
                          !s->prefs.shortcuts[STUDIO_COMMAND_SEARCH][0];
    lost.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    assert(SDL_PushEvent(&lost));
    frames(&f, 1);
    s->shortcut_recording = STUDIO_COMMAND_SEARCH + 1;
    recording = s->shortcut_recording == STUDIO_COMMAND_SEARCH + 1;

    SDL_Keymod primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    SDL_Scancode primary_key = s->mac ? SDL_SCANCODE_LGUI : SDL_SCANCODE_LCTRL;
    key_event_mod(primary_key, true, primary, false);
    key_event_mod(SDL_SCANCODE_P, true, primary | SDL_KMOD_SHIFT, true);
    key_event_mod(SDL_SCANCODE_P, false, primary | SDL_KMOD_SHIFT, false);
    frames(&f, 2);
    bool repeat_ignored = recording &&
                          s->shortcut_recording == STUDIO_COMMAND_SEARCH + 1;
    text_event("p");
    key_event_mod(SDL_SCANCODE_P, true, primary | SDL_KMOD_SHIFT, false);
    key_event_mod(SDL_SCANCODE_P, false, primary | SDL_KMOD_SHIFT, false);
    key_event_mod(primary_key, false, SDL_KMOD_NONE, false);
    frames(&f, 3);
    bool captured = s->shortcut_recording == 0 &&
                    !strcmp(s->prefs.shortcuts[STUDIO_COMMAND_SEARCH], "Mod+Shift+P");
    if (!(text_field_kept && focus_canceled && repeat_ignored && captured)) {
        printf(
            "  %d %d %d %d (recording %d)\n",
            text_field_kept,
            focus_canceled,
            repeat_ignored,
            captured,
            s->shortcut_recording
        );
    }
    test_check(
        text_field_kept && focus_canceled && repeat_ignored && captured,
        "Settings text focus blocks Play; focus loss cancels recording, which "
        "ignores repeats and captures a chord through the host input queue"
    );
    fixture_free(&f);
}

/* The first element `selector` matches in Studio's documents that shows. */
static rml_element *shown_element(studio *s, const char *selector)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int d = 0; d < rml_context_document_count(context); d++) {
        rml_element *found[512];
        int count = rml_element_query_selector_all(rml_context_document(context, d), selector, found, 512);
        for (int i = 0; i < count && i < 512; i++) {
            if (rml_element_is_visible(found[i], true)) {
                return found[i];
            }
        }
    }
    return NULL;
}

/* A panel's activations route by name (studio_panel_route): studio.<id>
 * runs the command, <source>.<action> the panel source's action, here the
 * Settings page's Record button's emit('settings.record', row.key), and
 * any other event goes to the panel's behaviors. */
static void panel_actions_route()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    studio_ui_show_settings(s, "Frame Selected");
    frames(&f, 4);
    rml_element *record = shown_element(s, ".record-shortcut");
    if (record) {
        press(s, record);
    }
    frames(&f, 3);
    bool recorded = record && s->shortcut_recording == STUDIO_COMMAND_FRAME + 1 &&
                    shown_element(s, ".recording");
    s->shortcut_recording = 0;
    ui_activation a = { .event = "settings.record", .argc = 1, .args = { "command.search" } };
    bool routed = studio_panel_route(s, &a) &&
                  s->shortcut_recording == STUDIO_COMMAND_SEARCH + 1;
    s->shortcut_recording = 0;
    ui_activation command = { .event = "studio.keyboardShortcuts" };
    ui_activation unknown = { .event = "nowhere.select" };
    ui_activation bare = { .event = "Close" };
    bool ran = studio_panel_route(s, &command) && studio_ui_modal_open(s);
    bool passed = !studio_panel_route(s, &unknown) && !studio_panel_route(s, &bare);
    if (!(recorded && routed && ran && passed)) {
        printf("  panel actions: %d %d %d %d\n", recorded, routed, ran, passed);
    }
    test_check(
        recorded && routed && ran && passed,
        "panel actions route by name: Settings' Record emits settings.record "
        "to its source, "
        "studio.<id> runs a command, and other events go to the panel's "
        "behaviors"
    );
    fixture_free(&f);
}

/* The widget gallery's VirtualList, driven by the VirtualList and
   WidgetGallery behaviors, makes only the rows in view of ten thousand, and
   scrolling shows the rows there. */
static void virtual_list_rows()
{
    studio_fixture f = studio_project_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_PANEL;
    snprintf(layout->panels[STUDIO_AREA_TOP], sizeof layout->panels[0], "Widgets");
    frames(&f, 12);
    char first[96];
    int rows = list_rows(s, ".vlist-window", first, sizeof first);
    bool windowed = rows > 10 && rows < 60 && !strcmp(first, "Row 1 of 10000");
    rml_element *view = studio_element(s, ".vlist");
    if (view) {
        rml_element_set_scroll_top(view, 24 * 500 * studio_pixel_ratio(s));
    }
    frames(&f, 6);
    int scrolled_rows = list_rows(s, ".vlist-window", first, sizeof first);
    bool scrolled = scrolled_rows > 10 && scrolled_rows < 60 &&
                    !strcmp(first, "Row 495 of 10000");
    if (!(windowed && scrolled)) {
        printf("  %d %d rows %d then %d, first '%s'\n", windowed, scrolled, rows, scrolled_rows, first);
    }
    test_check(
        windowed && scrolled,
        "a VirtualList of ten thousand rows makes only those in view, and "
        "scrolling shows the rows there"
    );
    fixture_free(&f);
}

static void settings_reopens_after_font_change()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char path[PATH_MAX];
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    studio_set_workspace(s, STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
    s->prefs.code_font_family = 0;
    s->prefs.code_font_size = 13;
    frames(&f, 3);
    bool reopened = true;
    for (int i = 0; i < 3 && reopened; i++) {
        rml_element *gear = studio_element(s, "[data-ui=TopBar] [data-tip=Settings]");
        assert(gear);
        press(s, gear);
        frames(&f, 3);
        rml_element *close = studio_element(s, "[data-ui=Settings] .close");
        reopened = close && rml_element_is_visible(close, true) &&
                   studio_element(s, "[data-ui=Settings] .search-input");
        if (!reopened) {
            break;
        }
        s->prefs.code_font_family = (i + 1) % 2;
        frames(&f, 2);
        press(s, close);
        frames(&f, 3);
        reopened = !studio_element(s, ".modal-host");
    }
    test_check(
        reopened,
        "Settings closes and reopens through the top bar after "
        "changing the monospace font with a code document open"
    );
    fixture_free(&f);
}

static void source_reload_write(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

/* Studio's private project owns its executable catalog, independently of the
 * scene being edited. A failed disk edit retains the running generation. */
static void private_panel_code_reloads()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char root[] = "/tmp/chibi-studio-ui-XXXXXX";
    assert(mkdtemp(root));
    const char *directories[] = { "content",
                                  "content/behaviors",
                                  "content/libraries",
                                  "content/prototypes",
                                  "content/scenes" };
    char path[1024];
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/%s", root, directories[i]);
        assert(!mkdir(path, 0700));
    }
    const char *files[] = { "project.kdl",
                            "content/scenes/Studio.kdl",
                            "content/prototypes/Panel.kdl",
                            "content/libraries/Shared.chibi",
                            "content/behaviors/Panel.chibi" };
    const char *texts[] = {
        NULL,
        "version 1\nname \"Studio\"\n(array)entities {\n - name=\"Panel\" "
        "prototype=\"Panel\" {\n (array)behaviors \"Panel\"\n }\n}\n",
        "name \"Panel\"\ncomponents {}\n",
        "library Shared { pure function Seven() -> (Result: int) { result "
        "Result: int = 7; } }",
        "behavior Panel { state count: int = 0; on Begin() { count += "
        "Shared.Seven().Result; } }"
    };
    char manifest[256];
    snprintf(manifest, sizeof manifest,
             "version %d\nname \"Private Studio\"\ncontent \"content\"\nstart_scene "
             "\"scenes/Studio.kdl\"\n", CHIBI_PROJECT_VERSION);
    texts[0] = manifest;
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        source_reload_write(path, texts[i]);
    }
    studio_ui *original = s->ui;
    rml_backend *original_backend = s->backend;
    s->backend = rml_backend_create(640, 480, 1);
    assert(s->backend);
    char error[256];
    studio_ui *private_ui = studio_ui_new(s, root, error, sizeof error);
    assert(private_ui);
    s->ui = private_ui;
    game_behavior_world *world = studio_ui_behaviors(s);
    assert(game_behavior_attachment_count(world) == 1);
    game_behavior_attachment_info panel = game_behavior_attachment_info_index(world, 0);
    behavior_instance *instance = panel.instance;
    flow_error flow = { 0 };
    assert(game_behavior_dispatch(world, panel.entity, "Begin", (flow_inputs){ 0 }, &flow) == 1);
    assert(acount(instance->activations) > 0);
    const behavior_asset *original_generation = panel.definition;
    int before = instance->execution.variables[0].i;
    snprintf(path, sizeof path, "%s/%s", root, files[4]);
    source_reload_write(path, "behavior Panel { state count: int = 0; on Begin() { count += Shared.Seven().Result * 2; } }");
    s->time += 2;
    studio_ui_tick(s);
    bool held = game_behavior_attachment_info_index(world, 0).definition ==
                original_generation;
    s->time += FRAME_SECONDS;
    studio_ui_tick(s);
    assert(game_behavior_dispatch(world, panel.entity, "Begin", (flow_inputs){ 0 }, &flow) == 1);
    bool reloaded = game_behavior_instance(world, panel.entity, "Panel") == instance &&
                    instance->execution.variables[0].i == before + 14;
    const behavior_asset *generation = game_behavior_attachment_info_index(world, 0)
                                           .definition;
    s->time += FRAME_SECONDS;
    studio_ui_tick(s);
    bool once = game_behavior_attachment_info_index(world, 0).definition == generation;
    source_reload_write(path, "behavior Panel { on Begin() { await Missing.Seven(); } }");
    s->time += 2;
    studio_ui_tick(s);
    assert(game_behavior_dispatch(world, panel.entity, "Begin", (flow_inputs){ 0 }, &flow) == 1);
    bool retained = game_behavior_instance(world, panel.entity, "Panel") == instance &&
                    game_behavior_attachment_info_index(world, 0).definition ==
                        generation &&
                    instance->execution.variables[0].i == before + 28;
    if (!(reloaded && retained)) {
        printf(
            "private reload before=%d current=%d reloaded=%d retained=%d\n",
            before,
            instance->execution.variables[0].i,
            reloaded,
            retained
        );
        for (int i = 0; i < acount(s->records); i++) {
            puts(s->records[i].text);
        }
    }
    test_check(held && reloaded && retained && once, "private Studio .chibi reload waits for active runs, resolves its own library, applies once with retained state, and keeps the last valid runtime after an invalid edit");
    s->ui = original;
    studio_ui_free(private_ui);
    rml_backend_destroy(s->backend);
    s->backend = original_backend;
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        assert(!unlink(path));
    }
    for (int i = 4; i >= 0; i--) {
        snprintf(path, sizeof path, "%s/%s", root, directories[i]);
        assert(!rmdir(path));
    }
    assert(!rmdir(root));
    fixture_free(&f);
}

/* Stored source retains metadata while the editor shows the executable prefix. */
static bool source_view_equals(asset_code *code, const char *text)
{
    int start, end;
    size_t length = strlen(text);
    if (code_source_metadata_range(text, &start, &end)) {
        length = (size_t)start;
    }
    const char *shown = kit_code_text(asset_code_editor(code), NULL);
    return !strcmp(asset_code_text(code), text) && strlen(shown) == length &&
           !memcmp(shown, text, length);
}

/* A failed disk reload keeps the actual editor, including its view and modal
   state; a successful reload removes the draft and its saved checkpoint. */
static void source_reload_preserves_failed_draft()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char folder[] = "/tmp/chibi-source-reload-XXXXXX";
    assert(mkdtemp(folder));
    char directory[PATH_MAX], path[PATH_MAX];
    snprintf(directory, sizeof directory, "%s/behaviors", folder);
    assert(mkdir(directory, 0700) == 0);
    snprintf(path, sizeof path, "%s/Teller.chibi", directory);
    char *saved = file_read_text(PROJECT "/content/behaviors/Teller.chibi");
    assert(saved);
    source_reload_write(path, saved);
    assert(app_scene_authoring_set_content_root(f.scene, folder) == 0);
    game_preview_definition *definition = game_preview_find_definition(f.live, "Teller");
    assert(definition);
    game_preview_definition_set_path(definition, path);
    scene_document_tab *tab = &f.scene->tabs[f.scene->active_tab];
    assert(tab->kind == APP_EDITOR_DOCUMENT_BEHAVIOR);
    free(tab->path);
    tab->path = strdup(path);
    assert(tab->path);
    size_t length = strlen(saved) + 32;
    char *draft = malloc(length);
    assert(draft);
    snprintf(draft, length, "// Unsaved source draft\n%s", saved);
    av_doc *doc = av_doc_new();
    av_value *state = av_obj(doc);
    av_doc_set_root(doc, state);
    av_obj_add_str(doc, state, "path", path);
    av_obj_add_str(doc, state, "disk", saved);
    av_obj_add_str(doc, state, "text", draft);
    av_obj_add_str(doc, state, "valid", draft);
    av_obj_add_bool(doc, state, "overlay", true);
    av_obj_add_int(doc, state, "step", 1);
    av_obj_add_int(doc, state, "cursor", 5);
    av_obj_add_int(doc, state, "anchor", 5);
    av_value *history = av_obj_add_arr(doc, state, "history");
    av_arr_add_str(doc, history, saved);
    av_arr_add_str(doc, history, draft);
    asset_code *code = asset_code_restore(state, &f.scene->registry);
    assert(code);
    apush(s->code.assets, code);
    av_value_free(tab->source_state);
    tab->source_state = asset_code_capture(code);
    tab->source_dirty = true;
    av_value *checkpoint = av_value_copy(tab->source_state);
    kit_code_editor *editor = asset_code_editor(code);
    source_reload_write(path, "name \"unfinished");
    app_disk_result failed = studio_code_reload_path(s, path);
    bool retained = failed == APP_DISK_FAILED && acount(s->code.assets) == 1 &&
                    s->code.assets[0] == code && asset_code_editor(code) == editor &&
                    source_view_equals(code, draft) &&
                    kit_code_cursor(editor) == 5 && asset_code_dirty(code) &&
                    tab->source_dirty && av_equals(checkpoint, tab->source_state) &&
                    text_overlay_text(path) &&
                    !strcmp(text_overlay_text(path), draft);
    test_check(retained, "failed code reload preserves the live editor, draft, history, cursor and preview overlay");
    /* On Save keeps a newer valid draft separate from its last live preview.
       Checkpoint restoration can also replace the overlay's revision. */
    text_overlay_set(path, saved);
    failed = studio_code_reload_path(s, path);
    test_check(
        failed == APP_DISK_FAILED && asset_code_editor(code) == editor &&
            source_view_equals(code, draft) && text_overlay_text(path) &&
            !strcmp(text_overlay_text(path), saved),
        "failed reload restores the actual preview bytes while keeping a newer "
        "deferred source draft"
    );
    source_reload_write(path, saved);
    app_disk_result loaded = studio_code_reload_path(s, path);
    bool cleared = loaded == APP_DISK_RELOADED && !acount(s->code.assets) &&
                   !tab->source_state && !tab->source_dirty &&
                   !text_overlay_text(path);
    s->maximized = STUDIO_AREA_TOP;
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_CODE;
    s->ui_schedule.wake = true;
    frames(&f, 3);
    asset_code *fresh = acount(s->code.assets) ? s->code.assets[0] : NULL;
    test_check(
        cleared && fresh && !asset_code_dirty(fresh) && source_view_equals(fresh, saved),
        "successful code reload clears its checkpoint and redraws exact disk "
        "text without restoring the old draft"
    );
    /* A restored checkpoint publishes its preview before the Text Editor
       opens. Reload must bypass that overlay even with no asset_code object. */
    assert(fresh && acount(s->code.assets) == 1);
    asset_code_free(fresh);
    adel(s->code.assets, 0);
    av_value_free(tab->source_state);
    tab->source_state = av_value_copy(checkpoint);
    tab->source_dirty = true;
    text_overlay_set(path, draft);
    source_reload_write(path, "name \"unfinished");
    failed = studio_code_reload_path(s, path);
    test_check(
        failed == APP_DISK_FAILED && !acount(s->code.assets) &&
            tab->source_dirty && av_equals(checkpoint, tab->source_state) &&
            text_overlay_text(path) && !strcmp(text_overlay_text(path), draft),
        "failed checkpoint-only reload keeps the draft and restores preview "
        "without opening a source editor"
    );
    source_reload_write(path, saved);
    loaded = studio_code_reload_path(s, path);
    cleared = loaded == APP_DISK_RELOADED && !tab->source_state &&
              !tab->source_dirty && !text_overlay_text(path);
    s->ui_schedule.wake = true;
    frames(&f, 3);
    fresh = acount(s->code.assets) ? s->code.assets[0] : NULL;
    test_check(
        cleared && fresh && !asset_code_dirty(fresh) && source_view_equals(fresh, saved),
        "successful checkpoint-only reload reads disk rather than preview and "
        "cannot restore the discarded draft"
    );
    av_value_free(checkpoint);
    av_doc_free(doc);
    free(draft);
    free(saved);
    fixture_free(&f);
    assert(unlink(path) == 0 && rmdir(directory) == 0 && rmdir(folder) == 0);
}

/* The value of the focused element in the Scene's game documents. */
static void game_field(studio_fixture *f, char *value, size_t size)
{
    rml_context *context = render_ui_context(engine_viewport_ui(f->viewport));
    rml_element *focus = rml_context_focus_element(context);
    value[0] = '\0';
    if (focus) {
        rml_element_value(focus, value, size);
    }
}

/* While the scene plays, a focused text field in its game documents takes
   the text typed in the focused Scene, SDL delivering text for it, and
   Studio keeps the keys with the primary modifier. */
static void scene_text_field()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    text_overlay_set(BANK, "<input id=\"name\" type=\"text\" autofocus/>\n");
    s->focus_area = STUDIO_AREA_TOP;
    frames(&f, 3);
    char value[32];
    text_event("x");
    frames(&f, 2);
    game_field(&f, value, sizeof value);
    bool editing = !value[0] && !engine_viewport_ui_typing(f.viewport);
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 3);
    bool started = engine_viewport_ui_typing(f.viewport) && s->text_input;
    key_event(SDL_SCANCODE_A, true);
    text_event("a");
    key_event(SDL_SCANCODE_A, false);
    text_event("b");
    frames(&f, 2);
    game_field(&f, value, sizeof value);
    bool typed = !strcmp(value, "ab");
    SDL_Event select = { .type = SDL_EVENT_KEY_DOWN };
    select.key.windowID = SDL_GetWindowID(cf_app_get_window());
    select.key.scancode = SDL_SCANCODE_A;
    select.key.key = SDLK_A;
    select.key.mod = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    select.key.down = true;
    assert(SDL_PushEvent(&select));
    frames(&f, 1);
    bool kept = !acount(s->game_input);
    key_event(SDL_SCANCODE_A, false);
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 2);
    bool stopped = !engine_viewport_ui_typing(f.viewport) && !s->text_input;
    if (!(editing && started && typed && kept && stopped)) {
        printf("  %d %d %d '%s' %d %d\n", editing, started, typed, value, kept, stopped);
    }
    test_check(
        editing && started && typed && kept && stopped,
        "a focused text field in the playing Scene's game UI takes typed "
        "text, and Studio keeps its chords"
    );
    text_overlay_set(BANK, NULL);
    fixture_free(&f);
}

/* Authored modal events travel through the mount and Dismiss behavior. */
static void authored_fields_keep_editing_keys()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    text_overlay_set(
        BANK,
        "<div data-keys=\"C:Parent Space:Parent Enter:Parent Up:Parent "
        "Down:Parent Escape:Close\">"
        "<input id=\"field\" type=\"text\" autofocus "
        "data-keys=\"Enter:Submit\"/>"
        "<textarea id=\"area\"/>"
        "<select id=\"pick\"><option value=\"a\">A</option><option "
        "value=\"b\">B</option></select>"
        "<input id=\"radio\" type=\"radio\"/>"
        "</div>\n"
    );
    frames(&f, 3);
    studio_preview_ui(s, "Bank");
    frames(&f, 8);
    rml_context *context = render_ui_context(engine_viewport_ui(s->canvas->viewport));
    rml_element *field[8] = { 0 };
    assert(canvas_elements(s, "#field", field) == 1);
    assert(rml_element_focus(field[0], true));
    int before = acount(s->canvas->actions);
    rml_context_process_key_down(context, RML_KEY_C, 0);
    rml_context_process_text_input(context, "c", 1);
    rml_context_process_key_down(context, RML_KEY_SPACE, 0);
    rml_context_process_text_input(context, " ", 1);
    frames(&f, 2);
    char value[32];
    rml_element_value(field[0], value, sizeof value);
    bool typed = !strcmp(value, "c ") && acount(s->canvas->actions) == before;
    rml_context_process_key_down(context, RML_KEY_RETURN, 0);
    frames(&f, 2);
    bool submitted = logged_last(s, "Submit") &&
                     acount(s->canvas->actions) == before + 1;
    assert(canvas_elements(s, "#area", field) == 1);
    assert(rml_element_focus(field[0], true));
    before = acount(s->canvas->actions);
    rml_context_process_text_input(context, "a", 1);
    rml_context_process_key_down(context, RML_KEY_RETURN, 0);
    rml_context_process_text_input(context, "\n", 1);
    frames(&f, 2);
    rml_element_value(field[0], value, sizeof value);
    bool newline = strchr(value, '\n') && acount(s->canvas->actions) == before;
    rml_context_process_key_down(context, RML_KEY_ESCAPE, 0);
    frames(&f, 2);
    bool escaped = logged_last(s, "Close");
    assert(canvas_elements(s, "#pick", field) == 1);
    assert(rml_element_focus(field[0], true));
    rml_element_click(field[0]);
    before = acount(s->canvas->actions);
    rml_context_process_key_down(context, RML_KEY_DOWN, 0);
    rml_context_process_key_down(context, RML_KEY_RETURN, 0);
    rml_context_process_key_down(context, RML_KEY_ESCAPE, 0);
    frames(&f, 2);
    rml_element_value(field[0], value, sizeof value);
    bool selected = !strcmp(value, "b") && acount(s->canvas->actions) == before;
    assert(canvas_elements(s, "#radio", field) == 1);
    assert(rml_element_focus(field[0], true));
    rml_context_process_key_down(context, RML_KEY_UP, 0);
    frames(&f, 2);
    bool radio = logged_last(s, "Parent");
    if (!(typed && submitted && newline && escaped && selected && radio)) {
        printf(
            "  field keys: typed %d, submitted %d, newline %d, Escape %d, "
            "select %d, radio %d\n",
            typed,
            submitted,
            newline,
            escaped,
            selected,
            radio
        );
    }
    test_check(typed && submitted && newline && escaped && selected && radio, "authored input bindings submit explicitly and inherit Escape while text, textarea and select keep editing keys; radios retain ancestor navigation");
    studio_preview_ui(s, NULL);
    fixture_free(&f);
    text_overlay_set(BANK, NULL);
}

static void authored_modals_and_collaboration()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    rml_element *found[8] = { 0 };
    bool toolbar = panel_elements(s, "TopBar", ".collaboration-trigger", found) == 1;
    assert(toolbar);
    press(s, found[0]);
    frames(&f, 4);
    bool opened = panel_elements(s, "Collaboration", ".collab-title", found) == 1 &&
                  !kit_popup_is_open(s->kit, "collaboration");
    bool focused = studio_element(s, "[data-ui=Collaboration] .close:focus") != NULL;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 4);
    bool escape_closed = panel_elements(s, "Collaboration", ".collab-title", found) == 0;
    studio_command_run(s, STUDIO_COMMAND_COLLABORATION);
    frames(&f, 3);
    rml_element *close = studio_element(s, "[data-ui=Collaboration] .close");
    assert(close);
    press(s, close);
    frames(&f, 4);
    bool close_closed = panel_elements(s, "Collaboration", ".collab-title", found) == 0;
    studio_command_run(s, STUDIO_COMMAND_SETTINGS);
    frames(&f, 3);
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 4);
    bool settings_escape = studio_element(s, "[data-ui=Settings] .head-title") == NULL;
    studio_command_run(s, STUDIO_COMMAND_SETTINGS);
    frames(&f, 3);
    close = studio_element(s, "[data-ui=Settings] .close");
    assert(close);
    press(s, close);
    frames(&f, 4);
    bool settings_close = studio_element(s, "[data-ui=Settings] .head-title") == NULL;
    if (!(toolbar && opened && focused && escape_closed && close_closed &&
          settings_escape && settings_close)) {
        printf(
            "  authored modal checks: toolbar %d, opened %d, focused %d, "
            "Escape "
            "%d, X %d, Settings Escape %d, Settings X %d\n",
            toolbar,
            opened,
            focused,
            escape_closed,
            close_closed,
            settings_escape,
            settings_close
        );
    }
    test_check(
        toolbar && opened && focused && escape_closed && close_closed &&
            settings_escape && settings_close,
        "toolbar and command open one authored Collaboration dialog; both "
        "dialogs dismiss with Escape and X"
    );

    collab_start("room", "self", "editor");
    apush(
        s->collaboration.peers,
        ((studio_collab_peer){ .client = strdup("peer"),
                               .name = strdup("Ada"),
                               .presentation = studio_collab_capture(s),
                               .color = 0x7aa2f7ffu })
    );
    studio_command_run(s, STUDIO_COMMAND_COLLABORATION);
    frames(&f, 3);
    bool participant = panel_elements(s, "Collaboration", ".participant-name", found) == 1;
    rml_element *follow = studio_element(s, "[data-ui=Collaboration] .follow");
    assert(follow);
    press(s, follow);
    frames(&f, 3);
    bool following = s->collaboration.following &&
                     !strcmp(s->collaboration.follow_client, "peer");
    rml_element *unfollow = studio_element(s, "[data-ui=Collaboration] .unfollow");
    assert(unfollow);
    press(s, unfollow);
    frames(&f, 3);
    bool stopped = !s->collaboration.following;
    studio_command_run_with(s, STUDIO_COMMAND_COLLABORATION_FOLLOW, 0);
    studio_ui_collaboration_action(s, STUDIO_COMMAND_COLLABORATION_LEAVE);
    collab_tick();
    bool left = !s->collaboration.following && !s->collaboration.saved_prefs &&
                !collab_active();
    s->collaboration.busy = true;
    collab_feedback("Creating room...", 0);
    studio_ui_collaboration_action(s, STUDIO_COMMAND_COLLABORATION_JOIN);
    bool gated = !strcmp(collab_feedback_text(), "Creating room...") &&
                 studio_collab_busy(s);
    collab_feedback("Room service unavailable", 1);
    bool retry = !studio_collab_busy(s);
    if (!(participant && following && stopped && left && gated && retry)) {
        printf(
            "  authored room checks: participant %d, following %d, stopped %d, "
            "left %d, gated %d, retry %d\n",
            participant,
            following,
            stopped,
            left,
            gated,
            retry
        );
    }
    test_check(participant && following && stopped && left && gated && retry, "authored participants follow and stop following; Leave restores local preferences; pending room requests gate retries until failure");
    collab_shutdown();
    fixture_free(&f);
}

/* The editors whose area shows authored panels: the body, header controls
   or both (the Behavior editor's and the Text Editor's headers, the Scene's
   SceneHeader, Performance's header and body). */
static const studio_editor AREA_PANELS[] = {
    STUDIO_EDITOR_ASSETS, STUDIO_EDITOR_OUTLINER, STUDIO_EDITOR_MEMBERS,
    STUDIO_EDITOR_OUTPUT, STUDIO_EDITOR_BEHAVIOR, STUDIO_EDITOR_PROPERTIES,
    STUDIO_EDITOR_SCENE,  STUDIO_EDITOR_CODE,     STUDIO_EDITOR_PERFORMANCE,
};
#define AREA_PANEL_COUNT (int)(sizeof AREA_PANELS / sizeof *AREA_PANELS)
#define AREA_ROUNDS 3

/* Shows `editor` in the top area for three UI frames, the List's window
   following its mount. */
static void show_area_panel(studio_fixture *f, studio_editor editor)
{
    studio_active_layout(f->studio)->editors[STUDIO_AREA_TOP] = editor;
    frames(f, 3);
}

/* The Workspace's drawer and side column (workstream C3): their areas
   change editor types, and each eases open and shut, its areas leaving
   their places; the arrangement ends as it began. */
static void workspace_takes_turns(studio_fixture *f)
{
    static const studio_editor DRAWERS[] = {
        STUDIO_EDITOR_OUTPUT,
        STUDIO_EDITOR_ASSETS,
        STUDIO_EDITOR_PERFORMANCE,
    };
    studio *s = f->studio;
    studio_layout *layout = studio_active_layout(s), before = *layout;
    layout->drawer_open = true;
    for (size_t d = 0; d < sizeof DRAWERS / sizeof *DRAWERS; d++) {
        studio_area_show(s, STUDIO_AREA_DRAWER, DRAWERS[d], "");
        frames(f, 3);
    }
    for (int a = 0; a < AREA_PANEL_COUNT; a++) {
        studio_area_show(s, STUDIO_AREA_OUTLINER, AREA_PANELS[a], "");
        frames(f, 3);
    }
    layout->drawer_open = false;
    studio_toggle_side_pane(s);
    frames(f, 30);
    studio_toggle_side_pane(s);
    frames(f, 30);
    *layout = before;
    frames(f, 30);
}

/* The views of the panels' data model: one frame evaluates each once. */
static uint64_t data_views(studio_fixture *f)
{
    studio *s = f->studio;
    uint64_t before = studio_ui_data_reads(s).values;
    studio_ui_data_dirty_all(s);
    s->ui_schedule.wake = true;
    frames(f, 1);
    return studio_ui_data_reads(s).values - before;
}

/* The palette, a picker and the refactor popover open and close, each
   document mounting while it shows and letting its mount go after. */
static void palettes_take_turns(studio_fixture *f)
{
    studio *s = f->studio;
    studio_command_run(s, STUDIO_COMMAND_SEARCH);
    frames(f, 2);
    text_event("to");
    frames(f, 2);
    tap(SDL_SCANCODE_ESCAPE);
    frames(f, 2);
    studio_pick_open(
        s,
        "turns",
        "",
        &(studio_pick_desc){ .kind = REFERENCE_ASSET_PROTOTYPE, .none = "None" },
        studio_element(s, ".area-header")
    );
    frames(f, 2);
    tap(SDL_SCANCODE_ESCAPE);
    frames(f, 2);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    studio_refactor_rename_asset(s, reference_index_find_name(index, REFERENCE_ASSET_PROTOTYPE, "Player"));
    frames(f, 2);
    tap(SDL_SCANCODE_ESCAPE);
    frames(f, 2);
}

/* One area turned from every authored panel to every other, round after
   round, and the palette, a picker and the refactor popover opened and
   closed: RmlUi warns of no view it lost or could not bind, and the data
   model keeps as many views as after the first round. With `designer`, a UI
   document is open, so the Outliner, Properties and Assets areas show the
   Designer's Hierarchy, Details and Library (workstream D1), and the Scene
   area its Canvas. */
static void area_panels_take_turns(bool designer)
{
    /* A behavior open, so Members lists it and GraphView's menus fill. */
    studio_fixture f = scene_open("test/fixtures/studio-workspace", "test/fixtures/studio-workspace/content/scenes/Sandbox.kdl", "Movement");
    studio *s = f.studio;
    if (designer) {
        char path[PATH_MAX];
        s->prefs.code_apply_live = false;
        assert(
            realpath("test/fixtures/studio-workspace/content/ui/HUD.rml", path) &&
            studio_open_ui_source(s, path, 1)
        );
        frames(&f, 4);
    }
    /* The list, which streams no thumbnails between rounds. */
    s->prefs.assets_tile = STUDIO_ASSETS_LIST;
    s->applied = s->prefs;
    int warnings = rml_backend_warning_count();
    uint64_t views[2] = { 0 };
    for (int round = 0; round < AREA_ROUNDS; round++) {
        for (int a = 0; a < AREA_PANEL_COUNT; a++) {
            for (int b = 0; b < AREA_PANEL_COUNT; b++) {
                if (a != b) {
                    show_area_panel(&f, AREA_PANELS[a]);
                    show_area_panel(&f, AREA_PANELS[b]);
                }
            }
        }
        /* Maximized and back, an area's header and body move in the
           kit's tree. */
        for (int a = 0; a < AREA_PANEL_COUNT; a++) {
            show_area_panel(&f, AREA_PANELS[a]);
            s->maximized = STUDIO_AREA_TOP;
            frames(&f, 3);
            s->maximized = -1;
            frames(&f, 3);
        }
        workspace_takes_turns(&f);
        /* The Scene area turns between the scene and a ui asset's Canvas,
           in its body and its header. */
        show_area_panel(&f, STUDIO_EDITOR_SCENE);
        for (int i = 0; !designer && i < 2; i++) {
            studio_preview_ui(s, "HUD");
            frames(&f, 3);
            studio_preview_ui(s, NULL);
            frames(&f, 3);
        }
        palettes_take_turns(&f);
        show_area_panel(&f, AREA_PANELS[0]);
        views[round > 0] = data_views(&f);
    }
    warnings = rml_backend_warning_count() - warnings;
    bool kept = !warnings && views[1] == views[0];
    if (!kept) {
        printf(
            "  views %llu after the first round, %llu after the last; %d RmlUi "
            "warnings\n",
            (unsigned long long)views[0],
            (unsigned long long)views[1],
            warnings
        );
    }
    test_check(
        kept,
        designer ? "with a UI document open, an area turned between every pair "
                   "of editors shows the Hierarchy, Details, Library and the "
                   "others in turn, binds each one's views anew, none lost or "
                   "piling up"
                 : "an area turned between every pair of authored panels, the "
                   "drawer's and side column's editors changed and each eased "
                   "open and shut, binds each one's views anew, none lost or "
                   "piling up"
    );
    fixture_free(&f);
}

void studio_documents_panel_checks()
{
    area_panels_take_turns(false);
    area_panels_take_turns(true);
    ui_wiring_checks();
    preview_hears_keys();
    settings_records_shortcut();
    panel_actions_route();
    virtual_list_rows();
    settings_reopens_after_font_change();
    source_reload_preserves_failed_draft();
    private_panel_code_reloads();
    scene_text_field();
    authored_modals_and_collaboration();
    authored_fields_keep_editing_keys();
}
