#include "authoring/ui_loader.h"
/* Native slots: kit content inside a kit_host's authored content, rebuilt
 * when the content replaces its element; and the authored status bar, top
 * bar and area header, which host them, over Studio's `editor` and
 * `commands` variables. */
#include "editor/studio/studio_internal.h"
#include "render/ui.h"
#include "support/text_overlay.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAME_SECONDS (1.0 / 60.0)

typedef struct
{
    rml_backend *backend;
    kit *k;
    kit_frame_input in;
    ui_mount *mount;
    rml_listener *listener;
    ui_data data;
    rml_element *host;
    bool slot;  /* begin the #slot slot */
    int clicks; /* of the button in it */
} slot_fixture;

static void on_event(void *user, const rml_event *event)
{
    ui_mount_listen(user, event);
}

static void on_emit(void *user, const rml_event *event, const char *name, int argc, const char *const *args)
{
    ui_mount_emit(user, event, name, argc, args);
}

/* A Studio kit whose host shows ui asset `name` under `root`, bound to
 * `doc`. */
static void open_fixture(slot_fixture *f, const char *root, const char *name, yyjson_mut_doc *doc)
{
    f->backend = rml_backend_create(1280, 480, 1);
    f->k = test_kit_create(rml_backend_context(f->backend), "studio.rml");
    f->in = (kit_frame_input){ .dp_ratio = 1, .width = 1280, .height = 480 };
    f->mount = authoring_ui_mount_new(root, name);
    f->listener = rml_listener_create(on_event, f->mount);
    ui_data_init(&f->data, rml_backend_context(f->backend), doc, on_emit, f->mount);
}

static void close_fixture(slot_fixture *f)
{
    kit_destroy(f->k);
    ui_data_free(&f->data);
    rml_backend_destroy(f->backend);
    ui_mount_free(f->mount);
    rml_listener_destroy(f->listener);
}

static void host(slot_fixture *f, const char *key)
{
    f->host = kit_host(f->k, key, NULL);
    if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
        kit_style_sheet(f->k, ui_mount_name(f->mount), ui_mount_style(f->mount));
    }
}

static void end_frame(slot_fixture *f)
{
    kit_end(f->k);
    rml_backend_update(f->backend, FRAME_SECONDS);
    f->in.time += FRAME_SECONDS;
}

/* The Bar asset with a button in #slot and a label in each item's cell. */
static void bar_frame(slot_fixture *f)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    host(f, "bar");
    if (f->slot && kit_slot_begin(k, "slot", f->host, "#slot")) {
        f->clicks += kit_button(k, "go", &(kit_button_desc){ .label = "Go" });
        kit_slot_end(k);
    }
    if (kit_slot_begin(k, "cell", f->host, ".cell")) {
        kit_label(k, "l", "native", "Native");
        kit_slot_end(k);
    }
    end_frame(f);
}

static void bar_frames(slot_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        bar_frame(f);
    }
}

/* The shown elements `selector` matches; data-for keeps its template
   hidden, and data-if hides rather than removes. */
static int count(slot_fixture *f, const char *selector)
{
    rml_element *found[8];
    int matches = rml_element_query_selector_all(f->host, selector, found, 8),
        shown = 0;
    for (int i = 0; i < matches && i < 8; i++) {
        shown += rml_element_is_visible(found[i], true);
    }
    return shown;
}

static void click(slot_fixture *f, rml_element *el)
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

/* Sets `items` to `size` numbers and lets data-for follow. */
static void set_items(slot_fixture *f, int size)
{
    yyjson_mut_val *items = yyjson_mut_obj_get(yyjson_mut_doc_get_root(f->data.doc), "items");
    yyjson_mut_arr_clear(items);
    for (int i = 0; i < size; i++) {
        yyjson_mut_arr_add_int(f->data.doc, items, i);
    }
    rml_data_model_dirty(f->data.model, "items");
}

/* Bar, then the same markup edited. */
static const char BAR[] = "<div class=\"bar\"><span id=\"slot\"/><div "
                          "class=\"item\" data-for=\"item : items\"><span "
                          "class=\"cell\"/></div></div>\n";
static const char EDITED[] = "<div class=\"bar edited\"><span "
                             "id=\"slot\"/><div class=\"item\" data-for=\"item "
                             ": items\"><span class=\"cell\"/></div></div>\n";

static void slot_checks(const char *root)
{
    char folder[128], path[160];
    snprintf(folder, sizeof folder, "%s/ui", root);
    snprintf(path, sizeof path, "%s/Bar.rml", folder);
    test_write_ui(folder, "Bar", BAR, NULL);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *top = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, top);
    yyjson_mut_obj_add_arr(doc, top, "items");
    slot_fixture f = { .slot = true };
    open_fixture(&f, root, "Bar", doc);
    set_items(&f, 1);
    bar_frames(&f, 3);
    kit_rect rect;
    rml_element *slot = rml_element_query_selector(f.host, "#slot");
    char mark[16] = "";
    rml_element_attribute(slot, "data-kit-slot", mark, sizeof mark);
    test_check(
        !ui_mount_error(f.mount)[0] && !strcmp(mark, "slot") &&
            count(&f, "#slot .btn") == 1 && count(&f, ".cell .native") == 1 &&
            kit_find(f.k, "app/bar/slot/go", &rect),
        "a native slot adopts an element of hosted content, marks it and holds "
        "kit content"
    );

    click(&f, rml_element_query_selector(f.host, "#slot .btn"));
    bar_frame(&f);
    test_check(f.clicks == 1, "kit controls in a native slot take their events");

    /* The mount reinstalls its markup within the frame, and RmlUi's pools
       hand the new #slot the old one's address. Neither the pointer nor the
       focus is on what goes, so no event reaches the kit meanwhile. */
    rml_backend_process_input(
        f.backend,
        &(rml_input){ .type = RML_INPUT_POINTER_MOVE, .x = 1, .y = 470 }
    );
    kit_release_focus(f.k, f.host);
    bar_frame(&f);
    text_overlay_set(path, EDITED);
    bar_frame(&f);
    rml_element *button = rml_element_query_selector(f.host, "#slot .btn");
    bool rebuilt = count(&f, ".edited #slot .btn") == 1;
    if (button) {
        click(&f, button);
    }
    /* data-for makes the new items as the document updates, after the frame
       that installed them. */
    bar_frame(&f);
    test_check(
        rebuilt && f.clicks == 2 && count(&f, ".cell .native") == 1,
        "a native slot builds anew when its mount reinstalls the markup"
    );

    set_items(&f, 0);
    bar_frames(&f, 2);
    bool gone = count(&f, ".cell") == 0 && !kit_find(f.k, "app/bar/cell/l", &rect);
    set_items(&f, 1);
    bar_frames(&f, 2);
    bool back = count(&f, ".cell .native") == 1;
    /* data-for drops the item and makes another between frames, where the
       freed elements' addresses may come back, and input arrives before the
       next frame begins. */
    set_items(&f, 0);
    rml_backend_update(f.backend, FRAME_SECONDS);
    set_items(&f, 1);
    rml_backend_update(f.backend, FRAME_SECONDS);
    click(&f, rml_element_query_selector(f.host, ".item"));
    bar_frames(&f, 2);
    test_check(
        gone && back && count(&f, ".cell .native") == 1 &&
            kit_find(f.k, "app/bar/cell/l", &rect),
        "a native slot follows data-for as it drops and recreates its item"
    );

    f.slot = false;
    bar_frame(&f);
    test_check(
        count(&f, "#slot .btn") == 0 &&
            !rml_element_has_attribute(rml_element_query_selector(f.host, "#slot"), "data-kit-slot") &&
            !kit_find(f.k, "app/bar/slot/go", &rect),
        "a native slot not begun in a frame loses its content and its mark"
    );
    close_fixture(&f);
    text_overlay_set(path, NULL);
    test_remove_ui(folder, "Bar");
}

/* ---------- The authored status bar ---------- */

static void status_frame(slot_fixture *f)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    host(f, "status");
    kit_release_focus(k, f->host);
    end_frame(f);
}

static void status_frames(slot_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        status_frame(f);
    }
}

static bool text_is(rml_element *el, const char *text)
{
    char rml[128] = "";
    if (el) {
        rml_element_inner_rml(el, rml, sizeof rml);
    }
    return !strcmp(rml, text);
}

/* A native element's text: <number-flow> and <morph-text> label theirs. */
static bool label_is(rml_element *el, const char *text)
{
    char label[64] = "";
    if (el) {
        rml_element_attribute(el, "aria-label", label, sizeof label);
    }
    return !strcmp(label, text);
}

/* The status bar's counts: selected, then objects. */
static bool counts_are(slot_fixture *f, const char *selected, const char *objects)
{
    rml_element *flows[2];
    return rml_element_query_selector_all(f->host, ".objects number-flow", flows, 2) == 2 &&
           label_is(flows[0], selected) && label_is(flows[1], objects);
}

/* The hidden objects' count, which shows only while any are hidden. */
static bool hidden_is(slot_fixture *f, const char *hidden)
{
    rml_element *count = rml_element_query_selector(f->host, ".hidden-objects");
    if (!hidden) {
        return !count || !rml_element_is_visible(count, false);
    }
    return count && rml_element_is_visible(count, false) &&
           label_is(rml_element_query_selector(count, "number-flow"), hidden);
}

static bool state_is(rml_element *el, const char *state)
{
    char value[16] = "";
    rml_element_attribute(el, "data-state", value, sizeof value);
    return !strcmp(value, state);
}

/* Studio's `editor` and `commands` variables, every command enabled and
   labeled by its id. */
static yyjson_mut_doc *chrome_doc(studio_command_state *commands)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    studio_status_add(doc);
    studio_commands_add(doc);
    studio_menubar_add(doc);
    studio_shell_add(doc);
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const char *id = studio_command_id((studio_command)c);
        commands[c] = (studio_command_state){
            .info = { .id = id, .label = id, .menu = id, .icon = KIT_ICON_FILE },
            .enabled = true,
        };
    }
    return doc;
}

#define ASSETS_KEY "\xe2\x87\xa7\xe2\x8c\x98"

static void status_checks()
{
    studio_command_state commands[STUDIO_COMMAND_COUNT];
    yyjson_mut_doc *doc = chrome_doc(commands);
    commands[STUDIO_COMMAND_TOGGLE_ASSETS].checked = true;
    commands[STUDIO_COMMAND_TOGGLE_ASSETS].info.shortcut = ASSETS_KEY "A";
    studio_status status = {
        .hints = { { KIT_ICON_MOUSE, NULL, "Select" }, { 0, "F", "Frame" } },
        .hint_count = 2,
        .report = "Saved",
        .reporting = true,
        .scene = "Sandbox",
        .selection = "Player",
        .selected = 1,
        .objects = 12,
        .hidden = 3,
        .maximized = -1,
    };
    bool shown = studio_status_show(doc, &status) &&
                 studio_commands_show(doc, commands);
    bool same = !studio_status_show(doc, &status) &&
                !studio_commands_show(doc, commands);
    slot_fixture f = { 0 };
    open_fixture(&f, "projects/studio/content", "StatusBar", doc);
    status_frames(&f, 3);
    rml_element *hints[4], *toggles[4];
    int hint_count = rml_element_query_selector_all(f.host, "[data-ui=KeyHint]", hints, 4) -
                     1;
    int toggle_count = rml_element_query_selector_all(f.host, ".toggle-drawer", toggles, 4);
    char kbd[32] = "";
    rml_element_attribute(toggles[0], "data-kbd", kbd, sizeof kbd);
    rml_element *bar = rml_element_query_selector(f.host, ".statusbar");
    rml_element *report = rml_element_query_selector(f.host, ".report");
    test_check(
        shown && same && !ui_mount_error(f.mount)[0] && hint_count == 2 &&
            text_is(rml_element_query_selector(hints[0], ".label"), "Select") &&
            text_is(rml_element_query_selector(hints[1], ".kbd-key"), "F") &&
            !rml_element_is_visible(rml_element_query_selector(hints[1], ".icon"), false) &&
            rml_element_is_visible(report, false) &&
            label_is(rml_element_query_selector(report, "morph-text"), "Saved") &&
            toggle_count == 3 && state_is(toggles[0], "on") &&
            state_is(toggles[1], "off") && !strcmp(kbd, ASSETS_KEY "A") &&
            text_is(rml_element_query_selector(toggles[1], ".label"), "toggleOutput") &&
            counts_are(&f, "1", "12") && hidden_is(&f, "3") &&
            count(&f, "#ui-scale .ui-scale") == 1 &&
            rml_element_box_size(bar, RML_BOX_BORDER).y == 28,
        "the authored status bar shows the editor and commands variables, "
        "and UI Scale's menu"
    );

    click(&f, toggles[1]);
    status_frame(&f);
    ui_activation a;
    bool emitted = ui_mount_take(f.mount, &a) &&
                   !strcmp(a.event, "studio.toggleOutput") && !a.has_value &&
                   studio_command_named(a.event + strlen("studio.")) ==
                       STUDIO_COMMAND_TOGGLE_OUTPUT;
    test_check(emitted && !kit_wants_keyboard(f.k), "a status bar toggle emits its Studio command and leaves the keyboard to Studio");

    status.hint_count = 1;
    status.reporting = false;
    status.selection = "";
    status.selected = 0;
    status.hidden = 0;
    if (studio_status_show(doc, &status)) {
        rml_data_model_dirty(f.data.model, "editor");
    }
    status_frames(&f, 2);
    test_check(
        count(&f, "[data-ui=KeyHint]") == 1 &&
            rml_element_has_attribute(report, "data-ending-style") &&
            counts_are(&f, "0", "12") && hidden_is(&f, NULL),
        "the status bar follows the editor variable as it changes: the report "
        "leaves, the counts roll and the hidden count goes"
    );
    close_fixture(&f);
}

/* ---------- The authored top bar and area header (workstream T) ---------- */

static void topbar_frame(slot_fixture *f)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    host(f, "topbar");
    kit_release_focus(k, f->host);
    end_frame(f);
}

static void topbar_frames(slot_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        topbar_frame(f);
    }
}

/* The single activation a click made. */
static bool emitted_once(slot_fixture *f, const char *event, int value)
{
    ui_activation a, more;
    bool one = ui_mount_take(f->mount, &a) && !ui_mount_take(f->mount, &more);
    return one && !strcmp(a.event, event) &&
           (value < 0 ? !a.has_value : a.has_value && (int)a.value == value);
}

static const studio_tab_view TABS[] = {
    { "Sandbox", "scenes/Sandbox.kdl", KIT_ICON_SCENE, false, false, false },
    { "Player", "prototypes/Player.kdl", KIT_ICON_PROTOTYPE, true, true, false },
    { "Hud", "ui/Hud.rml", KIT_ICON_LAYOUT, false, false, true },
};

/* The top bar lists `editor.tabs` with their state, runs a tab's commands
   with its index, reads its transport from `commands`, and shows the
   menubar, the play target and a paused run's steps. */
static void topbar_checks()
{
    studio_command_state commands[STUDIO_COMMAND_COUNT];
    yyjson_mut_doc *doc = chrome_doc(commands);
    commands[STUDIO_COMMAND_STOP].enabled = false;
    studio_status status = { .report = "",
                             .scene = "",
                             .selection = "",
                             .playing = true,
                             .tabs = TABS,
                             .tab_count = 3,
                             .maximized = -1 };
    studio_status_show(doc, &status);
    studio_commands_show(doc, commands);
    slot_fixture f = { 0 };
    open_fixture(&f, "projects/studio/content", "TopBar", doc);
    topbar_frames(&f, 3);
    rml_element *tabs[4], *buttons[4];
    int tab_count = rml_element_query_selector_all(f.host, ".doc-tab", tabs, 4);
    int button_count = rml_element_query_selector_all(f.host, ".group > .btn", buttons, 4);
    /* Three tabs and data-for's hidden template; play, stop and step. */
    assert(tab_count == 4 && button_count == 3);
    char tip[64] = "";
    rml_element_attribute(rml_element_query_selector(tabs[0], ".face"), "data-tip", tip, sizeof tip);
    test_check(
        !ui_mount_error(f.mount)[0] && count(&f, ".doc-tab") == 3 &&
            state_is(tabs[1], "on") && state_is(tabs[0], "off") &&
            !strcmp(tip, "scenes/Sandbox.kdl") &&
            text_is(rml_element_query_selector(tabs[1], ".label"), "Player") &&
            count(&f, ".dirty-dot") == 1 && count(&f, ".doc-close") == 1 &&
            rml_element_query_selector(tabs[2], ".doc-close") &&
            state_is(buttons[0], "on") &&
            rml_element_has_attribute(buttons[1], "data-disabled") &&
            !rml_element_has_attribute(buttons[0], "data-disabled") &&
            count(&f, "menubar > .trigger") == 6 &&
            count(&f, "#play-target .target") == 1,
        "the authored top bar shows editor.tabs and the transport's commands, "
        "with the menubar's six menus and the play target"
    );

    click(&f, rml_element_query_selector(tabs[0], ".face"));
    topbar_frame(&f);
    bool selected = emitted_once(&f, "studio.selectTab", 0);
    click(&f, rml_element_query_selector(tabs[2], ".doc-close"));
    topbar_frame(&f);
    bool closed = emitted_once(&f, "studio.closeTab", 2);
    click(&f, buttons[0]);
    topbar_frame(&f);
    bool played = emitted_once(&f, "studio.play", -1);
    test_check(selected && closed && played && !kit_wants_keyboard(f.k), "a tab emits selectTab and its close button closeTab, each with the tab's index, and the transport emits its commands");

    status.paused = true;
    status.tab_count = 1;
    studio_status_show(doc, &status);
    rml_data_model_dirty(f.data.model, "editor");
    topbar_frames(&f, 2);
    rml_element_query_selector_all(f.host, ".group > .btn", buttons, 4);
    test_check(
        count(&f, ".doc-tab") == 1 && count(&f, ".group > .btn") == 2 &&
            count(&f, "#debug-steps > .btn") == 3 && state_is(buttons[0], "off"),
        "the top bar follows editor.tabs, and a paused run leaves the step "
        "to the debugger's steps"
    );
    close_fixture(&f);
}

/* Area `area`'s header, with a filter field in its controls. */
static void header_frame(slot_fixture *f, int area, char *filter, size_t size)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    f->host = kit_host(k, "head", NULL);
    char alias[32];
    snprintf(alias, sizeof alias, "editor.areas[%d]", area);
    kit_attr(k, "data-alias-area", alias);
    if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
        kit_style_sheet(k, ui_mount_name(f->mount), ui_mount_style(f->mount));
    }
    kit_release_focus(k, f->host);
    if (kit_slot_begin(k, "controls", f->host, "#controls")) {
        kit_search(k, "filter", filter, size, "Filter");
        kit_slot_end(k);
    }
    end_frame(f);
}

/* Area `area`'s header over editor.areas as `status` shows them. */
static void open_header(slot_fixture *f, int area, const studio_status *status, char *filter, size_t size)
{
    studio_command_state commands[STUDIO_COMMAND_COUNT];
    yyjson_mut_doc *doc = chrome_doc(commands);
    studio_status_show(doc, status);
    studio_commands_show(doc, commands);
    open_fixture(f, "projects/studio/content", "AreaHeader", doc);
    for (int i = 0; i < 3; i++) {
        header_frame(f, area, filter, size);
    }
}

/* An area header binds its own item of editor.areas: Maximize runs with
   the area's index and turns to Restore, the drawer's alone closes, and a
   field in its controls keeps the keyboard while a click on its chrome
   leaves it to Studio. */
static void header_checks()
{
    studio_status status = { .report = "", .scene = "", .selection = "", .maximized = -1 };
    char filter[32] = "";
    slot_fixture top = { 0 }, drawer = { 0 };
    open_header(&top, STUDIO_AREA_TOP, &status, filter, sizeof filter);
    kit_rect rect;
    bool shown = !ui_mount_error(top.mount)[0] && count(&top, ".close") == 0 &&
                 count(&top, ".editor-type") == 1 &&
                 kit_find(top.k, "app/head/controls/filter", &rect);
    rml_element *input = rml_element_query_selector(top.host, "#controls input");
    rml_element_focus(input, false);
    header_frame(&top, STUDIO_AREA_TOP, filter, sizeof filter);
    bool typing = kit_wants_keyboard(top.k);
    click(&top, rml_element_query_selector(top.host, ".maximize"));
    header_frame(&top, STUDIO_AREA_TOP, filter, sizeof filter);
    bool maximized = emitted_once(&top, "studio.maximize", STUDIO_AREA_TOP) &&
                     !kit_wants_keyboard(top.k);
    status.maximized = STUDIO_AREA_TOP;
    studio_status_show(top.data.doc, &status);
    rml_data_model_dirty(top.data.model, "editor");
    header_frame(&top, STUDIO_AREA_TOP, filter, sizeof filter);
    maximized &= rml_element_is_class_set(rml_element_query_selector(top.host, ".area-head"), "maximized") &&
                 count(&top, ".maximize .icon") == 1;
    close_fixture(&top);

    status.maximized = -1;
    open_header(&drawer, STUDIO_AREA_DRAWER, &status, filter, sizeof filter);
    bool closes = count(&drawer, ".close") == 1;
    click(&drawer, rml_element_query_selector(drawer.host, ".close"));
    header_frame(&drawer, STUDIO_AREA_DRAWER, filter, sizeof filter);
    closes &= emitted_once(&drawer, "studio.closeDrawer", -1);
    click(&drawer, rml_element_query_selector(drawer.host, ".maximize"));
    header_frame(&drawer, STUDIO_AREA_DRAWER, filter, sizeof filter);
    closes &= emitted_once(&drawer, "studio.maximize", STUDIO_AREA_DRAWER);
    close_fixture(&drawer);
    if (!(shown && typing && maximized && closes)) {
        printf("  %d %d %d %d\n", shown, typing, maximized, closes);
    }
    test_check(shown && typing && maximized && closes, "an area header binds its area: Maximize runs on it and turns to Restore, the drawer's closes, and its slots keep a field's keyboard");
}

void test_kit_slot()
{
    char root[] = "/tmp/chibi-kit-slot-XXXXXX", folder[64];
    assert(mkdtemp(root));
    snprintf(folder, sizeof folder, "%s/ui", root);
    assert(mkdir(folder, 0700) == 0);
    slot_checks(root);
    rmdir(folder);
    rmdir(root);
    status_checks();
    /* Authored top bar and area headers (workstream T) */
    topbar_checks();
    header_checks();
}
