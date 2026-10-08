/* Properties (workstream P1): its authored panel over the `properties`
 * source. Prototype components, enum, vector and live fields, the field
 * menu, a scrub Escape cancels, several objects' mixed values, and an asset
 * dropped onto a field. */

#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/input.h"
#include "platform/cute.h"
#include "render/screenshot.h"
#include "authoring/ui_check.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/text_file.h"
#include "test/editor/studio_documents.h"
#include "test/editor/ui_bench.h"
#include "test/suite_internal.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The elements `selector` matches under `root` that show, up to `max`:
   data-for and data-when keep their templates hidden. */
static int shown_all(rml_element *root, const char *selector, rml_element **out, int max)
{
    rml_element *found[512];
    int count = root ? rml_element_query_selector_all(root, selector, found, 512) : 0,
        shown = 0;
    for (int i = 0; i < count && shown < max; i++) {
        if (rml_element_is_visible(found[i], true)) {
            out[shown++] = found[i];
        }
    }
    return shown;
}

static rml_element *shown_in(rml_element *root, const char *selector)
{
    rml_element *found = NULL;
    return shown_all(root, selector, &found, 1) ? found : NULL;
}

/* The Properties panel's root, whichever document holds it. */
static rml_element *properties(studio *s)
{
    return studio_element(s, "[data-ui=Properties]");
}

/* A Properties row by its visible label, so the checks follow the authored
   panel rather than how it is built. */
static rml_element *property_row(studio *s, const char *label)
{
    rml_element *rows[128];
    int count = shown_all(properties(s), ".prop", rows, 128);
    for (int i = 0; i < count; i++) {
        rml_element *text = rml_element_query_selector(rows[i], ".label span");
        char shown[128] = "";
        if (text && rml_element_inner_rml(text, shown, sizeof shown) &&
            !strcmp(shown, label)) {
            return rows[i];
        }
    }
    return NULL;
}

/* The row labeled `label`, scrolled into view. */
static rml_element *row_in_view(studio_fixture *f, const char *label)
{
    rml_element *row = property_row(f->studio, label);
    if (row) {
        rml_element_scroll_into_view(row, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
        frames(f, 2);
    }
    return property_row(f->studio, label);
}

/* The tab button whose tip is `label`, or NULL when the tab is not shown. */
static rml_element *tab_button(studio *s, const char *label)
{
    char selector[96];
    snprintf(selector, sizeof selector, ".prop-tabs .btn[data-tip=%s]", label);
    return shown_in(properties(s), selector);
}

/* Keep the selected actor's Object fields visible when this fixture also has
   an active graph node, which normally drives the inspector to Node. */
static void show_object_properties(studio *s)
{
    studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
    s->shown_node = studio_graph_node(s) ? studio_graph_node(s)
                                         : studio_graph_comment(s);
    s->shown_member = studio_member_shown(s);
    s->shown_actor = s->scene->selected;
    s->tab = STUDIO_TAB_OBJECT;
}

/* Optional visual evidence for the focused Properties run. */
static void capture_properties_screenshot(studio_fixture *f, const char *name)
{
    const char *directory = getenv("CHIBI_PROPERTIES_CAPTURE_DIR");
    if (!directory || !directory[0]) {
        return;
    }
    frames(f, 12);
    char path[PATH_MAX];
    int length = snprintf(path, sizeof path, "%s/%s.png", directory, name);
    assert(length > 0 && (size_t)length < sizeof path);
    assert(app_save_screenshot(path) == 0);
}

/* Compute a line's baseline from the same face metrics RmlUi uses to draw it.
   Comparing a property row's outer boxes misses differences in line-height. */
static float text_baseline(rml_element *line)
{
    rml_vec2 at = rml_element_absolute_offset(line, RML_BOX_CONTENT);
    rml_vec2 size = rml_element_box_size(line, RML_BOX_CONTENT);
    uintptr_t face = rml_font_face("inter", RML_FONT_NORMAL, 400, 12);
    assert(face);
    rml_font_metrics metrics = rml_font_face_metrics(face);
    return at.y + (size.y - metrics.line_spacing) * 0.5f + metrics.ascent;
}

/* The center of `el` in dp, as the pointer reaches it. */
static void element_center(studio *s, rml_element *el, float *x, float *y)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    *x = (at.x + size.x * 0.5f) / ratio;
    *y = (at.y + size.y * 0.5f) / ratio;
}

static void mouse_click(studio_fixture *f, float x, float y, Uint8 button)
{
    const studio *s = f->studio;
    mouse_move(s, x, y);
    for (int down = 1; down >= 0; down--) {
        SDL_Event press = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                         : SDL_EVENT_MOUSE_BUTTON_UP };
        press.button.windowID = SDL_GetWindowID(cf_app_get_window());
        press.button.button = button;
        press.button.down = down;
        press.button.clicks = 1;
        press.button.x = x * s->prefs.ui_scale;
        press.button.y = y * s->prefs.ui_scale;
        assert(SDL_PushEvent(&press));
    }
    frames(f, 2);
}

static bool click(studio_fixture *f, rml_element *el, Uint8 button)
{
    if (!el) {
        return false;
    }
    float x, y;
    element_center(f->studio, el, &x, &y);
    mouse_click(f, x, y, button);
    return true;
}

static bool prototype_has(app_scene_authoring *scene, const char *component)
{
    const game_prototype *prototype = find_prototype(scene, "Vault")->prototype;
    for (int i = 0; i < acount(prototype->components); i++) {
        if (!strcmp(prototype->components[i].desc->name, component)) {
            return true;
        }
    }
    return false;
}

static float acceleration(studio_fixture *f, int actor)
{
    return GAME_GameDrive_get_const(f->scene->world, f->scene->actors[actor].entity)
        ->acceleration;
}

static void set_acceleration(studio_fixture *f, int actor, float value)
{
    GAME_GameDrive_get(f->scene->world, f->scene->actors[actor].entity)->acceleration = value;
}

/* The Binding scene with its object's Object tab in Properties. */
static studio_fixture binding_open(const char *search)
{
    studio_fixture f = fixture_open(PROJECT, PROJECT "/content/scenes/Binding.kdl", "Weapons");
    f.studio = studio_new(f.scene, f.viewport, "projects/studio");
    select_instance(f.scene, 0);
    show_object_properties(f.studio);
    snprintf(f.studio->property_search, sizeof f.studio->property_search, "%s", search);
    scene_history_clear(f.scene->history);
    frames(&f, 4);
    return f;
}

/* Prototype Properties opens on its own context and keeps a valid component
   tab selected while that document remains active. */
static void prototype_properties_follow_document()
{
    studio_fixture f = side_open();
    studio *s = f.studio;
    s->maximized = s->focus_area = STUDIO_AREA_PROPERTIES;
    s->tab = STUDIO_TAB_SCENE;
    char error[256];
    assert(component_presentation_read(
        "components { GAME.GameUi { tab \"Gameplay\"; }; }\n",
        (ecs_component_catalog){ f.scene->registry.components,
                                 f.scene->registry.component_count },
        &s->presentation.hints,
        error,
        sizeof error
    ));
    assert(app_scene_authoring_open_prototype(f.scene, "Vault") == 0);
    frames(&f, 3);
    bool object = s->tab == STUDIO_TAB_OBJECT && !tab_button(s, "Scene") &&
                  tab_button(s, "Object");
    s->tab = STUDIO_TAB_GAMEPLAY;
    frames(&f, 2);
    rml_element *gameplay = tab_button(s, "Gameplay");
    bool retained = s->tab == STUDIO_TAB_GAMEPLAY && gameplay &&
                    rml_element_has_attribute(gameplay, "data-state");
    char state[8] = "";
    if (gameplay) {
        rml_element_attribute(gameplay, "data-state", state, sizeof state);
    }
    test_check(object, "opening a prototype leaves the unrelated Scene tab and selects Object");
    test_check(retained && !strcmp(state, "on"), "Properties keeps a valid component tab selected for the active prototype");
    fixture_free(&f);
}

/* Add Component searches the components the prototype lacks and adds the
   chosen one; its panel's menu removes it, and the toast's Undo returns it. */
static void prototype_components()
{
    studio_fixture f = side_open();
    studio *s = f.studio;
    /* Properties fills the workspace. Scroll its final action into view. */
    s->maximized = s->focus_area = STUDIO_AREA_PROPERTIES;
    assert(app_scene_authoring_open_prototype(f.scene, "Vault") == 0);
    frames(&f, 3);
    rml_element *add = shown_in(properties(s), ".prop-scroll .add-row .btn");
    if (add) {
        rml_element_scroll_into_view(add, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
        frames(&f, 2);
    }
    bool button = click(&f, add, SDL_BUTTON_LEFT);
    bool palette = s->palette.client == STUDIO_PALETTE_COMPONENTS;
    text_event("Health");
    frames(&f, 2);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    char added[128], selector[192];
    snprintf(added, sizeof added, "%s", s->components.added);
    bool gained = added[0] && prototype_has(f.scene, added) && !s->palette.client;
    const char *dot = strrchr(added, '.');
    snprintf(selector, sizeof selector, "section.panel[data-key=%s]", dot ? dot + 1 : added);
    rml_element *panel = shown_in(properties(s), selector);
    bool shown = panel && rml_element_has_attribute(panel, "data-open");
    if (panel) {
        rml_element_scroll_into_view(panel, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
        frames(&f, 2);
    }
    click(&f, panel ? rml_element_query_selector(panel, ".panel-head .title") : NULL, SDL_BUTTON_RIGHT);
    frames(&f, 2);
    rml_element *menu = studio_element(s, "#component-menu[data-open]");
    click(&f, menu ? shown_in(menu, ".menu-item") : NULL, SDL_BUTTON_LEFT);
    bool removed = !prototype_has(f.scene, added);
    rml_element *undo = studio_element(s, ".toast-action");
    if (undo) {
        rml_element_click(undo);
    }
    frames(&f, 2);
    bool restored = prototype_has(f.scene, added);
    studio_history(s, false);
    bool undone = !prototype_has(f.scene, added);
    studio_history(s, true);
    bool redone = prototype_has(f.scene, added);
    if (!(button && palette && gained && shown && menu && removed && undo &&
          restored && undone && redone)) {
        printf(
            "  %d %d %d %d %d %d %d %d %d %d\n",
            button,
            palette,
            gained,
            shown,
            menu != NULL,
            removed,
            undo != NULL,
            restored,
            undone,
            redone
        );
    }
    test_check(button && palette && gained, "Add Component searches the components a prototype lacks and adds the one chosen");
    test_check(shown && menu && removed && undo && restored && undone && redone, "an added component's panel opens, its menu (a right press on its head) removes it with Undo in a toast, and Undo and Redo step both");
    fixture_free(&f);
}

/* The label of the option an EnumField's field shows. */
static void enum_shown(rml_element *row, char *out, size_t size)
{
    out[0] = '\0';
    rml_element *value = row ? shown_in(row, ".field.select .current span") : NULL;
    if (value) {
        rml_element_inner_rml(value, out, size);
    }
}

/* Whether the shown groups of `list` are the three device groups. */
static bool device_groups(rml_element *list, int *count)
{
    static const char *const EXPECTED[] = { "Mouse", "Keyboard", "Controller" };
    rml_element *groups[8];
    *count = shown_all(list, ".menu-group", groups, 8);
    bool known = *count == 3;
    for (int i = 0; known && i < *count; i++) {
        char group[32] = "";
        rml_element_inner_rml(groups[i], group, sizeof group);
        known &= !strcmp(group, EXPECTED[i]);
    }
    return known;
}

/* A large reflected enum keeps its name in Properties and can still be
   changed from the control, searchable and grouped. GameControl's Aim Stick
   is past the old cap. Its list loads once it opens. */
static void large_enum_property()
{
    studio_fixture f = binding_open("Source");
    studio *s = f.studio;
    rml_element *tabs = studio_element(s, "[data-ui=Properties] .prop-tabs");
    rml_element *row = row_in_view(&f, "Source");
    rml_element *label = row ? rml_element_query_selector(row, ".label span") : NULL;
    rml_element *field = row ? shown_in(row, ".field.select") : NULL;
    rml_element *value = row ? shown_in(row, ".field.select .current span") : NULL;
    char shown[64];
    enum_shown(row, shown, sizeof shown);
    rml_element *closed[4];
    bool lazy = shown_all(row, ".popup.dropdown .menu-item", closed, 4) == 0 &&
                rml_element_query_selector_all(row, ".popup.dropdown [role=option]", NULL, 0) <=
                    4;
    bool readable = tabs && row && field && !strcmp(shown, "Aim Stick");
    float label_baseline = label ? text_baseline(label) : 0;
    float value_baseline = value ? text_baseline(value) : 0;
    bool aligned = label && value && fabsf(label_baseline - value_baseline) <= 1.5f;
    if (field) {
        press(s, field);
        frames(&f, 2);
    }
    rml_element *dropdown = studio_element(s, ".popup.dropdown[data-open]");
    rml_element *icons[128];
    int group_count = 0;
    bool grouped = dropdown && device_groups(dropdown, &group_count);
    int icon_count = shown_all(dropdown, ".menu-item .lead", icons, 128);
    for (int i = 0; i < icon_count; i++) {
        char icon[16] = "";
        rml_element_inner_rml(icons[i], icon, sizeof icon);
        grouped &= icon[0] != '\0';
    }
    rml_element *current = shown_in(dropdown, ".menu-item[data-current]");
    bool highlighted = current &&
                       rml_element_has_attribute(current, "data-highlighted");
    grouped &= icon_count > 32;
    capture_properties_screenshot(&f, "properties-enum-dropdown");
    text_event("L");
    frames(&f, 2);
    char query[16] = "";
    rml_element *search = dropdown
                            ? rml_element_query_selector(dropdown, ".field.search .value")
                            : NULL;
    if (search) {
        rml_element_value(search, query, sizeof query);
    }
    int search_groups = 0;
    device_groups(dropdown, &search_groups);
    bool searched = !strcmp(query, "L") && search_groups >= 1 &&
                    shown_all(dropdown, ".menu-item .lead", icons, 128) > 2;
    capture_properties_screenshot(&f, "properties-grouped-search");
    tap(SDL_SCANCODE_BACKSPACE);
    frames(&f, 1);
    text_event("Mouse Left");
    frames(&f, 2);
    rml_element *first = shown_in(dropdown, ".menu-item .label");
    char option[64] = "";
    if (first) {
        rml_element_inner_rml(first, option, sizeof option);
    }
    bool filtered = !strcmp(option, "Mouse Left");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    pin_value source = { 0 };
    bool editable = filtered &&
                    ecs_component_read_field(
                        f.scene->world,
                        f.scene->actors[0].entity,
                        GAME_COMPONENT_GameActionBinding.id,
                        0,
                        &source
                    ) == ECS_OK &&
                    source.type == PIN_ENUM && source.e == GAME_CONTROL_MOUSE_LEFT &&
                    scene_history_can_undo(f.scene->history);
    enum_shown(property_row(s, "Source"), shown, sizeof shown);
    bool follows = !strcmp(shown, "Mouse Left");
    if (!(readable && lazy && aligned && dropdown && filtered && editable &&
          grouped && searched && highlighted && follows)) {
        printf(
            "  %d %d %d %d %d %d %d %d %d %d (baselines %g / %g, groups %d, "
            "icons %d, search groups %d, query '%s', shown '%s')\n",
            readable,
            lazy,
            aligned,
            dropdown != NULL,
            filtered,
            editable,
            grouped,
            searched,
            highlighted,
            follows,
            label_baseline,
            value_baseline,
            group_count,
            icon_count,
            search_groups,
            query,
            shown
        );
    }
    test_check(readable && lazy, "Properties shows a reflected enum's choice by name and builds its list only once it opens");
    test_check(aligned, "Properties label and value text baselines align");
    test_check(
        dropdown && filtered && editable && grouped && searched && highlighted && follows,
        "a reflected enum beyond 32 options opens grouped, iconed and "
        "searchable on its current value, and a choice is one Undo step"
    );
    fixture_free(&f);
}

/* A reflected vector component uses one joined number control and keeps the
   rest of the vector intact through Undo and Redo. */
static void component_vector_property()
{
    studio_fixture f = binding_open("Target");
    studio *s = f.studio;
    rml_element *row = row_in_view(&f, "Target");
    rml_element *axes[4];
    int axis_count = shown_all(row, ".field.number", axes, 4);
    bool controls = axis_count == 2;
    bool clicked = false;
    if (controls) {
        press(s, axes[0]);
        clicked = rml_element_has_attribute(axes[0], "data-editing");
    }
    text_event("9");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    const GameDrive
        *drive = GAME_GameDrive_get_const(f.scene->world, f.scene->actors[0].entity);
    bool edited = drive && drive->target.x == 9.0f && drive->target.y == 4.0f;
    capture_properties_screenshot(&f, "properties-vector-edited");
    studio_history(s, false);
    drive = GAME_GameDrive_get_const(f.scene->world, f.scene->actors[0].entity);
    bool undone = drive && drive->target.x == 3.0f && drive->target.y == 4.0f;
    studio_history(s, true);
    drive = GAME_GameDrive_get_const(f.scene->world, f.scene->actors[0].entity);
    bool redone = drive && drive->target.x == 9.0f && drive->target.y == 4.0f;
    if (!(controls && clicked && edited && undone && redone)) {
        printf(
            "  %d %d %d %d %d (%g, %g)\n",
            controls,
            clicked,
            edited,
            undone,
            redone,
            drive ? drive->target.x : -1.0f,
            drive ? drive->target.y : -1.0f
        );
    }
    test_check(controls && clicked && edited, "a reflected component vector is editable by axis in Properties");
    test_check(undone && redone, "component vector edits retain owned before and after values for Undo and Redo");
    fixture_free(&f);
}

/* While the scene runs, a Properties field edits the running world without
   marking the scene unsaved or writing history, and the header says Live;
   Keep Changes (K) applies the running value when the scene stops, as one
   Undo step. A Play refresh that only moves values lays nothing out. */
static void live_property_edit()
{
    studio_fixture f = binding_open("Target");
    studio *s = f.studio;
    studio_runtime(s, APP_RUNTIME_PLAY);
    f.scene->scene_dirty = false;
    frames(&f, 3);
    app_runtime_state run = app_scene_authoring_runtime_state(f.scene);
    bool live = studio_live(s) && run.playing && run.target == APP_RUNTIME_SCENE &&
                studio_element(s, ".area-head.live") &&
                studio_element(s, "[data-ui=PropertiesHeader] .badge.accent") &&
                shown_in(studio_element(s, "[data-ui=PropertiesHeader]"), ".btn");
    rml_element *row = row_in_view(&f, "Target");
    rml_element *axes[4];
    int axis_count = shown_all(row, ".field.number", axes, 4);
    bool editable = axis_count == 2 &&
                    !rml_element_has_attribute(axes[0], "data-readonly");
    if (editable) {
        press(s, axes[0]);
    }
    text_event("9");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    const GameDrive
        *drive = GAME_GameDrive_get_const(f.scene->world, f.scene->actors[0].entity);
    bool written = drive && drive->target.x == 9.0f;
    bool clean = !f.scene->scene_dirty && !scene_history_can_undo(f.scene->history);
    /* A value the game moves shows without laying out Properties. */
    frames(&f, 2);
    rml_element *y = shown_all(row_in_view(&f, "Target"), ".field.number", axes, 4) == 2
                       ? axes[1]
                       : NULL;
    GAME_GameDrive_get(f.scene->world, f.scene->actors[0].entity)->target.y = 21.5f;
    bench_probe(rml_element_owner_document(properties(s)));
    frames(&f, 1);
    uint64_t layouts = bench_layouts();
    frames(&f, 6);
    char shown[32] = "";
    rml_element *text = y ? rml_element_query_selector(y, ".value") : NULL;
    if (text) {
        rml_element_inner_rml(text, shown, sizeof shown);
    }
    uint64_t laid_out = bench_layouts() - layouts;
    bool refreshed = strstr(shown, "21.5") != NULL && !laid_out;
    bool chord = studio_command_for_key(&s->prefs, SDLK_K, 0, STUDIO_EDITOR_PROPERTIES, s->mac) ==
                     STUDIO_COMMAND_KEEP_CHANGES &&
                 studio_command_for_key(&s->prefs, SDLK_K, 0, STUDIO_EDITOR_BEHAVIOR, s->mac) !=
                     STUDIO_COMMAND_KEEP_CHANGES;
    studio_command_run(s, STUDIO_COMMAND_KEEP_CHANGES);
    frames(&f, 2);
    bool kept = acount(f.scene->kept) == 1 && s->keep.toast;
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 2);
    drive = GAME_GameDrive_get_const(f.scene->world, f.scene->actors[0].entity);
    bool applied = drive && drive->target.x == 9.0f && f.scene->scene_dirty &&
                   s->undo_toast && !studio_live(s);
    studio_history(s, false);
    drive = GAME_GameDrive_get_const(f.scene->world, f.scene->actors[0].entity);
    bool undone = drive && drive->target.x == 3.0f &&
                  !scene_history_can_undo(f.scene->history);
    if (!(live && editable && written && clean && refreshed && chord && kept &&
          applied && undone)) {
        printf(
            "  %d %d %d %d %d ('%s', %llu layouts) %d %d %d %d\n",
            live,
            editable,
            written,
            clean,
            refreshed,
            shown,
            (unsigned long long)laid_out,
            chord,
            kept,
            applied,
            undone
        );
    }
    test_check(live && editable && written && clean, "while the scene runs, Properties edits the running world without an unsaved mark or Undo step, and its header says Live and offers Keep");
    test_check(refreshed, "a value the running game moves refreshes its field without laying out Properties");
    test_check(chord && kept && applied && undone, "Keep Changes (K) applies a running value when the scene stops, as one Undo step");
    fixture_free(&f);
}

/* A right press on an overridden field opens its menu, whose Reset to
   Prototype puts the prototype's value back as one Undo step. */
static void field_menu_resets()
{
    studio_fixture f = binding_open("Acceleration");
    studio *s = f.studio;
    set_acceleration(&f, 0, 9);
    frames(&f, 3);
    rml_element *row = row_in_view(&f, "Acceleration");
    bool marked = row && rml_element_has_attribute(row, "data-overridden");
    click(&f, row ? shown_in(row, ".field.number") : NULL, SDL_BUTTON_RIGHT);
    frames(&f, 2);
    rml_element *menu = studio_element(s, "#field-menu[data-open]");
    rml_element *items[2];
    int count = shown_all(menu, ".menu-item", items, 2);
    bool offered = count == 2 &&
                   !rml_element_has_attribute(items[0], "data-disabled") &&
                   !rml_element_has_attribute(items[1], "data-disabled");
    click(&f, count ? items[0] : NULL, SDL_BUTTON_LEFT);
    /* The menu fades out as it closes. */
    rml_element *open = studio_element(s, "#field-menu[data-open]");
    bool reset = acceleration(&f, 0) == 5 &&
                 scene_history_can_undo(f.scene->history) &&
                 (!open || rml_element_has_attribute(open, "data-ending-style"));
    row = property_row(s, "Acceleration");
    bool unmarked = row && !rml_element_has_attribute(row, "data-overridden");
    studio_history(s, false);
    bool undone = acceleration(&f, 0) == 9;
    if (!(marked && menu && offered && reset && unmarked && undone)) {
        printf(
            "  %d %d %d %d %d %d (%g)\n",
            marked,
            menu != NULL,
            offered,
            reset,
            unmarked,
            undone,
            acceleration(&f, 0)
        );
    }
    test_check(marked && menu && offered && reset && unmarked && undone, "a right press on an overridden field opens its menu, and Reset to Prototype is one Undo step that clears its mark");
    fixture_free(&f);
}

/* Scrubbing a number writes the value as it moves, without history, and
   Escape puts its first value back, writing nothing. */
static void scrub_escape_cancels()
{
    studio_fixture f = binding_open("Acceleration");
    studio *s = f.studio;
    f.scene->scene_dirty = false;
    rml_element *row = row_in_view(&f, "Acceleration");
    rml_element *field = row ? shown_in(row, ".field.number") : NULL;
    float x = 0, y = 0;
    if (field) {
        element_center(s, field, &x, &y);
    }
    mouse_move(s, x, y);
    frames(&f, 1);
    graph_drag_button(s, x, y, true);
    frames(&f, 1);
    for (int i = 1; i <= 4; i++) {
        mouse_move(s, x + 10 * i, y);
        frames(&f, 1);
    }
    bool live = field && acceleration(&f, 0) != 5 &&
                rml_element_has_attribute(field, "data-scrubbing") &&
                !scene_history_can_undo(f.scene->history);
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 1);
    graph_drag_button(s, x + 40, y, false);
    frames(&f, 3);
    bool restored = acceleration(&f, 0) == 5 &&
                    !scene_history_can_undo(f.scene->history) &&
                    !f.scene->scene_dirty;
    if (!(live && restored)) {
        printf("  %d %d (%g)\n", live, restored, acceleration(&f, 0));
    }
    test_check(live && restored, "a scrub writes as it moves without history, and Escape puts the value back");
    fixture_free(&f);
}

/* Several objects selected show their shared fields, a value they differ in
   mixed; typing one writes it to each as one Undo step. */
static void mixed_selection()
{
    studio_fixture f = binding_open("Acceleration");
    studio *s = f.studio;
    studio_add_object(s, "Binding");
    frames(&f, 2);
    app_scene_authoring *scene = f.scene;
    set_acceleration(&f, 1, 7);
    scene_history_clear(scene->history);
    for (int i = 0; i < acount(scene->actors); i++) {
        scene->actors[i].marked = true;
    }
    scene->selected = 0;
    show_object_properties(s);
    frames(&f, 3);
    rml_element *title = shown_in(properties(s), ".identity .title");
    char heading[32] = "";
    if (title) {
        rml_element_inner_rml(title, heading, sizeof heading);
    }
    rml_element *chips[4];
    bool counted = !strcmp(heading, "2 objects") &&
                   shown_all(properties(s), ".identity .lineage .btn", chips, 4) == 1;
    rml_element *row = row_in_view(&f, "Acceleration");
    rml_element *field = row ? shown_in(row, ".field.number") : NULL;
    bool mixed = field && rml_element_has_attribute(field, "data-mixed");
    if (field) {
        press(s, field);
    }
    text_event("8");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool written = acceleration(&f, 0) == 8 && acceleration(&f, 1) == 8;
    field = shown_in(property_row(s, "Acceleration"), ".field.number");
    bool settled = field && !rml_element_has_attribute(field, "data-mixed");
    studio_history(s, false);
    bool undone = acceleration(&f, 0) == 5 && acceleration(&f, 1) == 7 &&
                  !scene_history_can_undo(scene->history);
    if (!(counted && mixed && written && settled && undone)) {
        printf(
            "  %d ('%s') %d %d %d %d (%g, %g)\n",
            counted,
            heading,
            mixed,
            written,
            settled,
            undone,
            acceleration(&f, 0),
            acceleration(&f, 1)
        );
    }
    test_check(counted && mixed, "several selected objects show their count, a chip per prototype and a value they differ in as mixed");
    test_check(written && settled && undone, "a value typed into a mixed field writes every selected object as one Undo step");
    fixture_free(&f);
}

/* Find focuses the header's search, typing filters the rows to the fields
   it names, and Escape clears it and lets go. */
static void search_filters()
{
    studio_fixture f = binding_open("");
    studio *s = f.studio;
    rml_element *rows[64];
    int before = shown_all(properties(s), ".prop", rows, 64);
    s->find_request = STUDIO_EDITOR_PROPERTIES + 1;
    frames(&f, 2);
    rml_element *search = studio_element(s, "[data-ui=PropertiesHeader] #search");
    rml_context *context = rml_backend_context(s->backend);
    bool focused = search && rml_context_focus_element(context) == search &&
                   !s->find_request;
    text_event("Accel");
    frames(&f, 3);
    int filtered = shown_all(properties(s), ".prop", rows, 64);
    bool found = filtered == 1 && property_row(s, "Acceleration") &&
                 !strcmp(s->property_search, "Accel");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    bool cleared = !s->property_search[0] &&
                   rml_context_focus_element(context) != search &&
                   shown_all(properties(s), ".prop", rows, 64) == before;
    if (!(focused && found && cleared)) {
        printf("  %d %d (%d of %d rows) %d\n", focused, found, filtered, before, cleared);
    }
    test_check(focused && found && cleared, "Find focuses the property search, typing filters the fields, and Escape clears it");
    fixture_free(&f);
}

/* The Vault's ui document, as its GameUi names it. */
static const char *vault_document(studio_fixture *f)
{
    return GAME_GameUi_get_const(f->scene->world, f->scene->actors[0].entity)->document;
}

/* Carries the Bank asset's row onto Properties' Document field, the pointer
   held, Escape pressed there when `cancel`; whether the field took the drag
   and showed Bank while over it. */
static bool carry_bank(studio_fixture *f, bool cancel)
{
    studio *s = f->studio;
    char first[256];
    list_rows(s, "#assets-list > .list-spacer", first, sizeof first);
    rml_element *window = studio_element(s, "#assets-list > .list-spacer");
    rml_element *row = NULL;
    for (int i = 0; window && i < rml_element_child_count(window); i++) {
        char text[4096] = "";
        rml_element *child = rml_element_child(window, i);
        rml_element_inner_rml(child, text, sizeof text);
        row = strstr(text, "Bank") && !strstr(text, ">UI<") ? child : row;
    }
    rml_element *prop = property_row(s, "Document");
    rml_element *picker = prop ? shown_in(prop, ".field.picker") : NULL;
    if (!row || !picker) {
        printf("  carry: row=%d picker=%d\n", row != NULL, picker != NULL);
        return false;
    }
    float x, y, px, py;
    element_center(s, row, &x, &y);
    element_center(s, picker, &px, &py);
    mouse_move(s, x, y);
    frames(f, 1);
    graph_drag_button(s, x, y, true);
    frames(f, 1);
    mouse_move(s, x + 12, y);
    frames(f, 2);
    mouse_move(s, px, py);
    frames(f, 3);
    rml_element *target = shown_in(property_row(s, "Document"), ".asset-field");
    char shown[64] = "";
    rml_element *value = target ? rml_element_query_selector(target, ".field.picker .value")
                                : NULL;
    if (value) {
        rml_element_inner_rml(value, shown, sizeof shown);
    }
    bool over = target && rml_element_has_attribute(target, "data-drop-over") &&
                rml_element_has_attribute(target, "data-drop-preview") &&
                !strcmp(shown, "Bank");
    if (cancel) {
        tap(SDL_SCANCODE_ESCAPE);
        frames(f, 1);
    }
    graph_drag_button(s, px, py, false);
    frames(f, 3);
    return over;
}

/* Typed asset assignment (studio_drop.c): a ui asset dragged from Assets
   onto Properties' Document field shows there while over it, the authored
   value untouched, and lands as one Undo step; Escape puts the drag back. */
static void asset_drop_assigns()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_ASSETS;
    s->prefs.assets_tile = STUDIO_ASSETS_LIST;
    s->applied = s->prefs;
    snprintf(s->assets_filter, sizeof s->assets_filter, "Bank");
    select_instance(f.scene, 0);
    show_object_properties(s);
    snprintf(s->property_search, sizeof s->property_search, "Document");
    pin_value elsewhere = { .type = PIN_STR, .str = "Elsewhere" };
    ecs_component_write_field(
        f.scene->world,
        f.scene->actors[0].entity,
        GAME_COMPONENT_GameUi.id,
        0,
        elsewhere
    );
    scene_history_clear(f.scene->history);
    frames(&f, 6);
    bool cancelled = carry_bank(&f, true) &&
                     !strcmp(vault_document(&f), "Elsewhere") &&
                     !scene_history_can_undo(f.scene->history);
    bool over = carry_bank(&f, false);
    bool assigned = !strcmp(vault_document(&f), "Bank") &&
                    scene_history_can_undo(f.scene->history);
    studio_history(s, false);
    bool undone = !strcmp(vault_document(&f), "Elsewhere");
    if (!(cancelled && over && assigned && undone)) {
        printf("  asset drop: %d %d %d %d (%s)\n", cancelled, over, assigned, undone, vault_document(&f));
    }
    test_check(cancelled, "Escape puts a dragged asset back: the field keeps its value and history is untouched");
    test_check(over && assigned && undone, "a ui asset dragged onto its Properties field shows there, lands as one Undo step and Undo restores it");
    fixture_free(&f);
}

/* Properties' panel and the widgets it builds rows with read without a
   warning from RmlUi. */
static void properties_assets_check()
{
    static const char *const NAMES[] = {
        "Properties",  "PropertiesHeader", "PropertyRow",
        "VectorField", "EnumField",        "AssetField",
    };
    int clean = 0, count = (int)(sizeof NAMES / sizeof *NAMES);
    for (int i = 0; i < count; i++) {
        char path[256], error[200] = "";
        snprintf(path, sizeof path, "projects/studio/content/ui/%s.rml", NAMES[i]);
        char *rml = text_file_read(path);
        snprintf(path, sizeof path, "projects/studio/content/ui/%s.rcss", NAMES[i]);
        char *rcss = text_file_read(path);
        ui_check_message *messages = NULL;
        bool ok = rml &&
                  ui_check("projects/studio/content", NAMES[i], rml, rcss, &messages, error, sizeof error);
        for (int m = 0; m < acount(messages); m++) {
            fprintf(
                stderr,
                "  %s:%d %s\n",
                NAMES[i],
                messages[m].line,
                messages[m].text
            );
        }
        clean += ok && !acount(messages);
        ui_check_messages_free(messages);
        free(rml);
        free(rcss);
    }
    test_check(clean == count, "Properties and the widgets of its rows check clean");
}

static rml_element *sleep_box(studio *s)
{
    rml_element *row = property_row(s, "Allow sleeping");
    return row ? rml_element_query_selector(row, ".check input") : NULL;
}

/* RmlUi sends change when a binding checks a checkbox too, so a checkbox
   shown checked keeps its field, frame after frame, and a click sets it
   once, as one Undo step. */
static void bound_checkbox_holds()
{
    studio_fixture f = binding_open("");
    studio *s = f.studio;
    bool sleep = game_physics_configuration(f.scene->live->runtime->physics).sleep;
    press(s, tab_button(s, "Scene"));
    bool held = true;
    for (int i = 0; i < 4; i++) {
        frames(&f, 1);
        rml_element *box = sleep_box(s);
        held &= box && rml_element_has_attribute(box, "checked") == sleep &&
                game_physics_configuration(f.scene->live->runtime->physics).sleep == sleep;
    }
    held &= !scene_history_can_undo(f.scene->history);
    rml_element *box = sleep_box(s);
    if (box) {
        press(s, box);
    }
    frames(&f, 4);
    box = sleep_box(s);
    bool clicked = box && rml_element_has_attribute(box, "checked") == !sleep &&
                   game_physics_configuration(f.scene->live->runtime->physics).sleep == !sleep &&
                   scene_history_can_undo(f.scene->history);
    studio_history(s, false);
    frames(&f, 2);
    bool undone = game_physics_configuration(f.scene->live->runtime->physics).sleep == sleep &&
                  !scene_history_can_undo(f.scene->history);
    if (!(held && clicked && undone)) {
        printf("  %d %d %d\n", held, clicked, undone);
    }
    test_check(held && clicked && undone, "a checkbox its binding checks keeps its field, and a click sets it once, as one Undo step");
    fixture_free(&f);
}

void studio_documents_property_field_checks()
{
    bound_checkbox_holds();
    properties_assets_check();
    large_enum_property();
    component_vector_property();
    live_property_edit();
    field_menu_resets();
    scrub_escape_cancels();
    mixed_selection();
    search_filters();
}

void studio_documents_properties_checks()
{
    prototype_properties_follow_document();
    prototype_components();
    studio_documents_property_field_checks();
    asset_drop_assigns();
}
