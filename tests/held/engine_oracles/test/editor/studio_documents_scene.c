/* The Scene area in Studio (workstream C1): SceneHeader's tools, snapping
 * and grid increment, View menu and View Zoom, and Frame, which run
 * Studio's commands; Scene's overlays over the scene, its camera labels
 * moving without laying out; the Canvas's header and Actions log; a
 * guest Studio's header; and the fixture documents keeping Studio's clock.
 * With CHIBI_SCENE_CAPTURE_DIR set it writes native shots of the area's
 * states in the four variants. */

#include "editor/workspace/viewport.h"
#include "render/screenshot.h"
#include "support/array.h"
#include "test/editor/studio_documents.h"
#include "test/editor/ui_bench.h"
#include "test/suite_internal.h"
#include "ui/popover.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SANDBOX "test/fixtures/studio-workspace"

/* The synthetic Sandbox, its Camera framed in view, with the Scene filling the
   workspace. The generated world stays off, so shots hold still. */
static studio_fixture sandbox_open()
{
    studio_fixture f = scene_open(SANDBOX, SANDBOX "/content/scenes/Sandbox.chibi", "Movement");
    f.studio->prefs.scene_world = false;
    f.studio->prefs.scene_cameras = true;
    frames(&f, 8);
    return f;
}

/* The first element of `panel` that `selector` matches, or NULL. */
static rml_element *in_panel(studio *s, const char *panel, const char *selector)
{
    rml_element *found[8];
    return panel_elements(s, panel, selector, found) ? found[0] : NULL;
}

static bool shown(rml_element *el)
{
    return el && rml_element_is_visible(el, true);
}

/* The markup inside an element, which is its text for a label. */
static void text_of(rml_element *el, char *out, size_t size)
{
    out[0] = '\0';
    if (el) {
        rml_element_inner_rml(el, out, size);
    }
}

static bool text_is(rml_element *el, const char *text)
{
    char shown_text[256];
    text_of(el, shown_text, sizeof shown_text);
    return !strcmp(shown_text, text);
}

/* The shown menu row in `menu` whose label starts with `label`. */
static rml_element *menu_row(rml_element *menu, const char *label)
{
    rml_element *rows[32];
    int count = menu ? rml_element_query_selector_all(menu, ".menu-item", rows, 32)
                     : 0;
    for (int i = 0; i < count && i < 32; i++) {
        char text[64];
        text_of(rml_element_query_selector(rows[i], ".label"), text, sizeof text);
        if (!strncmp(text, label, strlen(label)) && shown(rows[i])) {
            return rows[i];
        }
    }
    return NULL;
}

static void click(studio_fixture *f, rml_element *el)
{
    assert(el);
    press(f->studio, el);
    frames(f, 3);
}

/* Shown, and not fading out as presence leaves it (ui/motion.h). */
static bool staying(rml_element *el)
{
    return shown(el) && !rml_element_has_attribute(el, "data-ending-style");
}

static bool pressed(rml_element *el)
{
    char state[8] = "";
    return el && rml_element_attribute(el, "data-state", state, sizeof state) &&
           !strcmp(state, "on");
}

/* The tools run their commands and show the active one pressed, with
   their names and keys in their tips; the magnet toggles snapping and
   the increment's menu sets it. */
static void header_tools_and_snapping()
{
    studio_fixture f = sandbox_open();
    studio *s = f.studio;
    rml_element *tools[8];
    int count = panel_elements(s, "SceneHeader", ".tool-group .btn", tools);
    assert(count == SCENE_TOOL_COUNT);
    bool select = pressed(tools[0]) && !pressed(tools[1]);
    char tip[32] = "", key[16] = "";
    rml_element_attribute(tools[1], "data-tip", tip, sizeof tip);
    rml_element_attribute(tools[1], "data-kbd", key, sizeof key);
    click(&f, tools[1]);
    panel_elements(s, "SceneHeader", ".tool-group .btn", tools);
    bool move = s->prefs.tool == SCENE_TOOL_MOVE && pressed(tools[1]) &&
                !pressed(tools[0]);
    test_check(
        select && move && !strcmp(tip, "Move") && !strcmp(key, "W"),
        "the Scene header's tools run their commands, the active one "
        "pressed, each tip naming it and its key"
    );

    bool snap = s->prefs.scene_snap;
    click(&f, in_panel(s, "SceneHeader", ".snap-controls .btn.icon-only"));
    bool toggled = s->prefs.scene_snap != snap &&
                   pressed(in_panel(s, "SceneHeader", ".snap-controls .btn.icon-only")) !=
                       snap;
    s->prefs.scene_increment = 32;
    s->ui_schedule.wake = true;
    frames(&f, 2);
    rml_element *increment = in_panel(s, "SceneHeader", ".snap-controls .menu-btn");
    bool labeled = text_is(rml_element_query_selector(increment, ".label"), "32");
    click(&f, increment);
    rml_element *menu = in_panel(s, "SceneHeader", "#scene-increment");
    bool open = popover_open(menu);
    click(&f, menu_row(menu, "16"));
    increment = in_panel(s, "SceneHeader", ".snap-controls .menu-btn");
    test_check(
        toggled && labeled && open && s->prefs.scene_increment == 16 &&
            text_is(rml_element_query_selector(increment, ".label"), "16") &&
            !popover_open(menu),
        "the magnet toggles snapping and the grid increment's menu sets it"
    );
    fixture_free(&f);
}

/* View shows Frame, Show's toggles checked as their preferences are and
   the View Zoom levels; 200% shows its button, which 100% hides again.
   Frame stays off without a selection. */
static void view_menu_and_zoom()
{
    studio_fixture f = sandbox_open();
    studio *s = f.studio;
    s->prefs.scene_grid = true;
    s->ui_schedule.wake = true;
    frames(&f, 2);
    bool hidden = !shown(in_panel(s, "SceneHeader", "[popovertarget=scene-zoom]"));
    click(&f, in_panel(s, "SceneHeader", "[popovertarget=scene-view]"));
    rml_element *menu = in_panel(s, "SceneHeader", "#scene-view");
    rml_element *grid = menu_row(menu, "Grid");
    bool checked = grid && rml_element_has_attribute(grid, "data-checked") &&
                   menu_row(menu, "Frame Selected") &&
                   menu_row(menu, "Camera Frames");
    click(&f, grid);
    bool ungridded = !s->prefs.scene_grid && !popover_open(menu);
    click(&f, in_panel(s, "SceneHeader", "[popovertarget=scene-view]"));
    click(&f, menu_row(menu, "200%"));
    rml_element *zoom = in_panel(s, "SceneHeader", "[popovertarget=scene-zoom]");
    bool magnified = fabsf(s->view.zoom - 2) < 0.01f && shown(zoom) &&
                     text_is(rml_element_query_selector(zoom, ".label"), "200%");
    click(&f, zoom);
    click(&f, menu_row(in_panel(s, "SceneHeader", "#scene-zoom"), "100%"));
    bool back = fabsf(s->view.zoom - 1) < 0.01f &&
                !shown(in_panel(s, "SceneHeader", "[popovertarget=scene-zoom]"));
    studio_command_run(s, STUDIO_COMMAND_DESELECT);
    frames(&f, 3);
    rml_element *frame = in_panel(s, "SceneHeader", ".snap-controls + .btn");
    bool off = frame && rml_element_has_attribute(frame, "data-disabled");
    if (!(hidden && checked && ungridded && magnified && back && off)) {
        printf("  %d %d %d %d %d %d\n", hidden, checked, ungridded, magnified, back, off);
    }
    test_check(
        hidden && checked && ungridded && magnified && back && off,
        "the View menu toggles Show's items and sets View Zoom, whose "
        "button shows only while it magnifies; Frame waits for a selection"
    );
    fixture_free(&f);
}

static void transform_of(studio *s, char *out, size_t size)
{
    rml_element *label = in_panel(s, "Scene", ".camera-label:not([data-for])");
    out[0] = '\0';
    if (label && text_is(label, "Camera")) {
        rml_element_property(label, "transform", out, size);
    }
}

/* A camera frame's label follows the camera by transform, laying out none
   of Studio's documents; the label names the scene and selection; a
   session shows Live instead. */
static void overlays_follow_the_scene()
{
    studio_fixture f = sandbox_open();
    studio *s = f.studio;
    char before[64], after[64];
    transform_of(s, before, sizeof before);
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        bench_probe(rml_context_document(context, i));
    }
    engine_viewport_view view;
    engine_viewport_get_view(f.viewport, &view);
    view.center_y += 16;
    engine_viewport_set_view(f.viewport, &view);
    frames(&f, 1);
    uint64_t layouts = bench_layouts();
    s->ui_schedule.wake = true;
    frames(&f, 1);
    transform_of(s, after, sizeof after);
    bool moved = before[0] && after[0] && strcmp(before, after) &&
                 bench_layouts() == layouts;
    if (!moved) {
        printf(
            "  '%s' to '%s', %llu layouts\n",
            before,
            after,
            (unsigned long long)(bench_layouts() - layouts)
        );
    }
    test_check(moved, "a camera frame's label follows a pan by transform, laying nothing out");

    rml_element *named = in_panel(s, "Scene", ".view-label .dim");
    char text[128];
    text_of(named, text, sizeof text);
    bool names = shown(named) && !strncmp(text, "(Sandbox) ", 10);
    studio_command_run(s, STUDIO_COMMAND_PLAY);
    frames(&f, 30);
    bool live = shown(in_panel(s, "Scene", ".live-badge")) &&
                !staying(in_panel(s, "Scene", ".view-label")) &&
                !in_panel(s, "Scene", ".camera-label:not([data-for])");
    studio_command_run(s, STUDIO_COMMAND_STOP);
    frames(&f, 30);
    bool editing = !shown(in_panel(s, "Scene", ".live-badge")) &&
                   staying(in_panel(s, "Scene", ".view-label"));
    test_check(
        names && live && editing,
        "the Scene names its scene and selection while edited and shows "
        "Live instead while a session runs"
    );
    fixture_free(&f);
}

/* The Canvas's header chooses the screen size, backdrop and Actions log;
   the log lists the asset's events and Clear empties it; the Scene crumb
   returns to the scene. */
static void canvas_header_and_actions()
{
    studio_fixture f = sandbox_open();
    studio *s = f.studio;
    studio_preview_ui(s, "HUD");
    frames(&f, 4);
    bool crumb = text_is(in_panel(s, "SceneHeader", ".crumb.current .label"), "HUD") &&
                 !in_panel(s, "SceneHeader", ".tool-group");
    rml_element *sizes[8];
    int count = panel_elements(s, "SceneHeader", ".canvas-sizes .seg:not([data-for])", sizes);
    assert(count == STUDIO_CANVAS_SIZE_COUNT);
    click(&f, sizes[STUDIO_CANVAS_PHONE]);
    panel_elements(s, "SceneHeader", ".canvas-sizes .seg:not([data-for])", sizes);
    bool phone = s->canvas->size == STUDIO_CANVAS_PHONE &&
                 rml_element_has_attribute(sizes[STUDIO_CANVAS_PHONE], "data-pressed") &&
                 text_is(in_panel(s, "Canvas", ".view-label span"), "Phone \xc2\xb7 390 \xc3\x97 844");
    bool light = s->canvas->light;
    click(&f, in_panel(s, "SceneHeader", ".canvas-sizes + .btn"));
    bool backdrop = s->canvas->light != light;
    studio_active_layout(s)->canvas_actions_open = false;
    frames(&f, 2);
    click(&f, in_panel(s, "SceneHeader", ".canvas-actions-button"));
    bool open = studio_active_layout(s)->canvas_actions_open &&
                shown(in_panel(s, "Canvas", ".canvas-actions-empty"));
    apush(
        s->canvas->actions,
        ((studio_canvas_action){ .count = 2, .event = "Repair", .element = "#repair" })
    );
    /* As the preview's click that logs it would. */
    s->ui_schedule.wake = true;
    frames(&f, 3);
    bool logged = text_is(in_panel(s, "Canvas", ".canvas-action:not([data-for]) .event"), "Repair") &&
                  text_is(
                      in_panel(s, "Canvas", ".canvas-action:not([data-for]) .count"),
                      "\xc3\x97"
                      "2"
                  );
    click(&f, in_panel(s, "Canvas", ".canvas-actions-head .btn"));
    bool cleared = acount(s->canvas->actions) == 0 &&
                   shown(in_panel(s, "Canvas", ".canvas-actions-empty"));
    click(&f, in_panel(s, "SceneHeader", ".crumb:not(.current)"));
    bool back = !studio_previewed_ui(s) && in_panel(s, "SceneHeader", ".tool-group");
    if (!(crumb && phone && backdrop && open && logged && cleared && back)) {
        printf("  %d %d %d %d %d %d %d\n", crumb, phone, backdrop, open, logged, cleared, back);
    }
    test_check(
        crumb && phone && backdrop && open && logged && cleared && back,
        "the Canvas's header sets its size, backdrop and Actions log, which "
        "lists events until cleared, and its Scene crumb returns"
    );
    fixture_free(&f);
}

/* A guest Studio's header names what runs in place of the Scene's tools. */
static void guest_header()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    studio_guest_start(s);
    frames(&f, 8);
    bool guest = s->guest && shown(in_panel(s, "SceneHeader", ".scene-guest")) &&
                 !in_panel(s, "SceneHeader", ".tool-group");
    studio_guest_stop(s);
    /* The next refresh shows the scene's tools again. */
    frames(&f, 20);
    bool tools = in_panel(s, "SceneHeader", ".tool-group") != NULL;
    test_check(guest && !s->guest && tools, "a guest Studio's Scene header names it in place of the tools");
    fixture_free(&f);
}

/* The Scene's and Canvas's own RCSS reads the classes the kit sets on the
   area's document, as the settings change: the viewport keeps dark roles in
   a light theme, and the Canvas's Actions rows follow the density. */
static void panel_rules_follow_body()
{
    studio_fixture f = sandbox_open();
    studio *s = f.studio;
    s->prefs.theme = STUDIO_THEME_DARK;
    frames(&f, 4);
    char dark[32] = "", light[32] = "";
    rml_element *label = in_panel(s, "Scene", ".view-label");
    if (label) {
        rml_element_property(label, "color", dark, sizeof dark);
    }
    s->prefs.theme = STUDIO_THEME_LIGHT;
    frames(&f, 4);
    label = in_panel(s, "Scene", ".view-label");
    if (label) {
        rml_element_property(label, "color", light, sizeof light);
    }
    test_check(
        !strcmp(dark, "#e6e8ec") && !strcmp(light, "#e8edf5"),
        "the Scene's own RCSS keeps the viewport's dark roles in a light theme "
        "and follows a theme change"
    );
    studio_preview_ui(s, "HUD");
    studio_active_layout(s)->canvas_actions_open = true;
    apush(
        s->canvas->actions,
        ((studio_canvas_action){ .count = 1, .event = "Repair", .element = "#repair" })
    );
    s->ui_schedule.wake = true;
    frames(&f, 4);
    float heights[3];
    for (int i = 0; i < 3; i++) {
        s->prefs.comfortable = i == 1;
        s->ui_schedule.wake = true;
        frames(&f, 4);
        rml_element *row = in_panel(s, "Canvas", ".canvas-action:not([data-for])");
        heights[i] = row ? rml_element_box_size(row, RML_BOX_BORDER).y : 0;
    }
    test_check(
        heights[0] > 0 && heights[1] * 20 == heights[0] * 24 &&
            heights[2] == heights[0],
        "the Canvas's own RCSS sizes its Actions rows 20dp compact and 24dp "
        "comfortable, as the density changes"
    );
    fixture_free(&f);
}

static void capture(studio_fixture *f, const char *dir, const char *variant, const char *state)
{
    char path[PATH_MAX];
    frames(f, 12);
    snprintf(path, sizeof path, "%s/native-%s-%s.png", dir, variant, state);
    assert(app_save_screenshot(path) == 0);
}

/* The header and overlays editing the scene, magnified, in the Canvas with
   its Actions log, and in Play. */
static void capture_scene(const char *dir)
{
    static const struct
    {
        const char *name;
        studio_theme theme;
        bool comfortable;
    } VARIANTS[] = { { "dark-compact", STUDIO_THEME_DARK, false },
                     { "dark-comfortable", STUDIO_THEME_DARK, true },
                     { "light-compact", STUDIO_THEME_LIGHT, false },
                     { "light-comfortable", STUDIO_THEME_LIGHT, true } };
    for (size_t v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++) {
        studio_fixture f = sandbox_open();
        studio *s = f.studio;
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        capture(&f, dir, VARIANTS[v].name, "scene");
        s->view.zoom = 2;
        capture(&f, dir, VARIANTS[v].name, "zoom");
        s->view.zoom = 1;
        studio_preview_ui(s, "HUD");
        studio_active_layout(s)->canvas_actions_open = true;
        frames(&f, 4);
        apush(
            s->canvas->actions,
            ((studio_canvas_action){ .count = 2, .event = "Repair", .element = "#repair" })
        );
        capture(&f, dir, VARIANTS[v].name, "canvas");
        studio_preview_ui(s, NULL);
        studio_command_run(s, STUDIO_COMMAND_PLAY);
        capture(&f, dir, VARIANTS[v].name, "play");
        studio_command_run(s, STUDIO_COMMAND_STOP);
        frames(&f, 2);
        fixture_free(&f);
    }
}

#define SLOW_FRAMES 4
#define SLOW_FRAME_MICROSECONDS 100000

/* Whether frames that each take six times the frame time they are given
   step RmlUi's clock by that frame time alone. */
static bool steps_frame_time(studio_fixture *f)
{
    double start = rml_elapsed_time();
    for (int i = 0; i < SLOW_FRAMES; i++) {
        usleep(SLOW_FRAME_MICROSECONDS);
        frames(f, 1);
    }
    double stepped = rml_elapsed_time() - start;
    if (fabs(stepped - SLOW_FRAMES * FRAME_SECONDS) > 1e-6) {
        printf("  stepped %.4f s over %d frames\n", stepped, SLOW_FRAMES);
        return false;
    }
    return true;
}

/* RmlUi's clock, which all documents share, follows the frame time Studio
   is given however long its frames take, while the game's documents show in
   the Scene and in the Canvas, so a fixed timestep replays transitions. */
static void documents_keep_frame_time()
{
    studio_fixture f = vault_open();
    frames(&f, 2);
    bool scene = steps_frame_time(&f);
    studio_preview_ui(f.studio, "Bank");
    frames(&f, 2);
    bool canvas = f.studio->canvas && steps_frame_time(&f);
    test_check(
        scene && canvas,
        "the game's documents in the Scene and the Canvas step RmlUi's "
        "clock by Studio's frame time, not the wall clock"
    );
    fixture_free(&f);
}

void studio_documents_scene_checks()
{
    const char *dir = getenv("CHIBI_SCENE_CAPTURE_DIR");
    if (dir && dir[0]) {
        capture_scene(dir);
        return;
    }
    header_tools_and_snapping();
    view_menu_and_zoom();
    overlays_follow_the_scene();
    canvas_header_and_actions();
    panel_rules_follow_body();
    guest_header();
    documents_keep_frame_time();
}
