/* The Outliner: renaming objects in place, and the Hierarchy a UI
 * document shows there. */

#include "assets/new_asset.h"
#include "assets/ui_events.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_nodes.h"
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
#include "support/asset_document.h"
#include "support/text_file.h"
#include "support/text_overlay.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/input_feel.h"
#include "ui/motion.h"

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

/* Selecting on Canvas keeps the same widget visible in its Hierarchy. */
static void designer_selection_reveals_hierarchy()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->prefs.code_apply_live = false;
    char path[PATH_MAX];
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 3);
    ui_buffer *buffer = studio_ui_buffer(s);
    const char *markup = "<div id=\"panel\"><button "
                         "id=\"target\">Select</button></div>\n";
    text_edit edit = { 0, (int)strlen(ui_buffer_text(buffer, UI_PART_RML)), markup };
    ui_buffer_edit(buffer, UI_PART_RML, &edit, 1);
    frames(&f, 3);
    studio_designer *d = designer_state(s, &buffer);
    assert(acount(d->rml->elements) == 2);
    designer_select(s, 0);
    assert(studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_LEFT, 0));
    snprintf(d->filter, sizeof d->filter, "absent");
    d->closed = true;
    s->ui_schedule.wake = true;
    frames(&f, 2);
    bool hidden = !studio_element(s, "#hierarchy-list .tree-row[data-selected] .name");
    designer_select(s, 1);
    s->ui_schedule.wake = true;
    frames(&f, 3);
    rml_element *name = studio_element(s, "#hierarchy-list .tree-row[data-selected] .name");
    char text[32] = "";
    if (name) {
        rml_element_inner_rml(name, text, sizeof text);
    }
    if (!(hidden && name && !strcmp(text, "button") && !d->closed &&
          !d->filter[0] && !acount(d->folded))) {
        printf(
            "  Hierarchy hidden %d, row %s, selected %d, closed %d, filter "
            "'%s', folded %d\n",
            hidden,
            name ? text : "missing",
            d->selected,
            d->closed,
            d->filter,
            acount(d->folded)
        );
    }
    test_check(
        hidden && name && !strcmp(text, "button") && !d->closed &&
            !d->filter[0] && !acount(d->folded),
        "Canvas selection reveals its Hierarchy row through collapsed "
        "ancestors and an excluding filter"
    );
    snprintf(d->filter, sizeof d->filter, "target");
    designer_select(s, 1);
    test_check(!strcmp(d->filter, "target"), "selection keeps a Hierarchy filter that already includes its row");
    fixture_free(&f);
}

static void mouse_over(studio_fixture *f, studio_area_slot area)
{
    kit_rect r = f->studio->areas[area].rect;
    mouse_move(f->studio, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    frames(f, 1);
}

static bool named(const app_scene_authoring *scene, const char *name)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (!strcmp(scene->actors[i].name, name)) {
            return true;
        }
    }
    return false;
}

/* Renames one object at a time: F2 over the Outliner edits its row, Enter
   renames as one Undo step, Escape keeps the name and a name in use is
   refused; without the Outliner, F2 edits Properties' name field. */
static void rename_objects()
{
    studio_fixture f = side_open();
    studio *s = f.studio;
    app_scene_authoring *scene = f.scene;
    studio_add_object(s, "Vault");
    select_instance(scene, 0);
    frames(&f, 2);
    mouse_over(&f, STUDIO_AREA_OUTLINER);
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    bool field = s->rename.entity && s->rename.outliner && kit_wants_keyboard(s->kit);
    text_event("Strongroom");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 2);
    bool renamed = named(scene, "Strongroom") && !named(scene, "Vault") &&
                   !s->rename.entity;
    studio_history(s, false);
    bool undone = named(scene, "Vault") && !named(scene, "Strongroom");
    studio_history(s, true);
    bool redone = named(scene, "Strongroom");
    frames(&f, 1);
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    text_event("Nope");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    bool kept = named(scene, "Strongroom") && !named(scene, "Nope") &&
                !s->rename.entity;
    select_instance(scene, 1);
    frames(&f, 1);
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    text_event("Strongroom");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 2);
    bool refused = named(scene, "Vault_1") && strstr(scene->message, "Strongroom");
    studio_active_layout(s)->editors[STUDIO_AREA_OUTLINER] = STUDIO_EDITOR_ASSETS;
    frames(&f, 2);
    mouse_over(&f, STUDIO_AREA_TOP);
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    bool properties = s->tab == STUDIO_TAB_OBJECT && kit_wants_keyboard(s->kit);
    text_event("Annex");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 2);
    bool inspector = named(scene, "Annex") && !named(scene, "Vault_1");
    if (!(field && renamed && undone && redone && kept && refused &&
          properties && inspector)) {
        printf("  %d %d %d %d %d %d %d %d\n", field, renamed, undone, redone, kept, refused, properties, inspector);
    }
    test_check(
        field && renamed && undone && redone && kept,
        "F2 over the Outliner renames its row in place as one Undo step, "
        "and Escape keeps the name"
    );
    test_check(
        refused && properties && inspector,
        "a name in use is refused, and F2 edits Properties' name field "
        "while no area shows the Outliner"
    );
    fixture_free(&f);
}

/* ---------- The authored Outliner ---------- */

/* Actor `index`'s row in the Outliner, or NULL while it is out of the
   window. */
static rml_element *row_of(studio *s, int index)
{
    char selector[64];
    snprintf(
        selector,
        sizeof selector,
        "#outliner-list [data-object=\"%llu\"]",
        (unsigned long long)ecs_entity_to_u64(s->scene->actors[index].entity)
    );
    return studio_element(s, selector);
}

static rml_element *toggle_of(studio *s, int index, int which);

/* The point `fx`, `fy` of `el`'s border box, in dp. */
static void point_in(studio *s, rml_element *el, float fx, float fy, float *x, float *y)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    *x = (at.x + size.x * fx) / ratio;
    *y = (at.y + size.y * fy) / ratio;
}

/* Presses and releases the left button at (x, y) in dp with `mod` held,
   as SDL reports it, and lets the click land. */
static void click_at(studio_fixture *f, float x, float y, SDL_Keymod mod)
{
    studio *s = f->studio;
    SDL_Scancode key = mod & SDL_KMOD_SHIFT ? SDL_SCANCODE_LSHIFT
                     : mod & SDL_KMOD_ALT   ? SDL_SCANCODE_LALT
                     : mod & SDL_KMOD_CTRL  ? SDL_SCANCODE_LCTRL
                                            : SDL_SCANCODE_LGUI;
    if (mod) {
        key_event_mod(key, true, mod, false);
    }
    /* A row shows its eye and lock while hovered, so the press comes once
       the pointer has rested there, as a hand's does. */
    mouse_move(s, x, y - 1);
    frames(f, 2);
    mouse_move(s, x, y);
    frames(f, 1);
    graph_drag_button(s, x, y, true);
    graph_drag_button(s, x, y, false);
    frames(f, 1);
    if (mod) {
        key_event_mod(key, false, SDL_KMOD_NONE, false);
    }
    frames(f, 2);
}

/* Clicks at `fx`, `fy` of `el`. */
static void click_in(studio_fixture *f, rml_element *el, float fx, float fy, SDL_Keymod mod)
{
    float x, y;
    point_in(f->studio, el, fx, fy, &x, &y);
    click_at(f, x, y, mod);
}

/* Clicks in the column of row `index`'s eye (0) or lock (1), `fy` down the
   row's height. */
static void click_toggle(studio_fixture *f, int index, int which, float fy, SDL_Keymod mod)
{
    rml_element *toggle = toggle_of(f->studio, index, which);
    float x, y, unused;
    point_in(f->studio, toggle, 0.5f, 0.5f, &x, &unused);
    point_in(f->studio, row_of(f->studio, index), 0.5f, fy, &unused, &y);
    click_at(f, x, y, mod);
}

/* The platform's primary modifier, as Studio reads it. */
static SDL_Keymod primary(const studio *s)
{
    return s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
}

/* Studio beside the Vault scene with `count` objects, the first selected. */
static studio_fixture objects_open(int count)
{
    studio_fixture f = side_open();
    for (int i = 1; i < count; i++) {
        studio_add_object(f.studio, "Vault");
    }
    /* The view store outlives the run, keyed by scene and object name. */
    for (int i = 0; i < acount(f.scene->actors); i++) {
        app_scene_authoring_set_actor_view(f.scene, i, 0);
    }
    select_instance(f.scene, 0);
    frames(&f, 4);
    return f;
}

/* A click selects its row alone, Mod+click adds a row or takes it out, and
   Shift+click selects from the anchor; the rows show it, the active one a
   step stronger. */
static void rows_select()
{
    studio_fixture f = objects_open(5);
    studio *s = f.studio;
    app_scene_authoring *scene = f.scene;
    rml_element *row = row_of(s, 1);
    if (row) {
        click_in(&f, row, 0.5f, 0.5f, 0);
    }
    bool one = row && scene->selected == 1 && scene_selection_count(scene) == 1;
    click_in(&f, row_of(s, 3), 0.5f, 0.5f, primary(s));
    bool added = scene->selected == 3 && scene_selection_count(scene) == 2 &&
                 scene_instance_selected(scene, 1);
    click_in(&f, row_of(s, 1), 0.5f, 0.5f, primary(s));
    bool removed = !scene_instance_selected(scene, 1) &&
                   scene_selection_count(scene) == 1;
    click_in(&f, row_of(s, 0), 0.5f, 0.5f, SDL_KMOD_SHIFT);
    bool range = scene_selection_count(scene) == 4 && scene->selected == 0;
    frames(&f, 2);
    rml_element *active = studio_element(s, "#outliner-list .tree-row[data-active] .name");
    char name[64] = "";
    if (active) {
        rml_element_inner_rml(active, name, sizeof name);
    }
    bool shown = !strcmp(name, scene->actors[0].name) &&
                 studio_element(s, "#outliner-list .tree-row[data-marked]");
    if (!(one && added && removed && range && shown)) {
        printf("  select: %d %d %d %d %d, active '%s'\n", one, added, removed, range, shown, name);
    }
    test_check(
        one && added && removed && range && shown,
        "a row's click selects it alone, Mod+click adds or removes it, "
        "Shift+click selects from the anchor, and the rows show the selection"
    );
    fixture_free(&f);
}

/* Row `index`'s eye (0) or lock (1). */
static rml_element *toggle_of(studio *s, int index, int which)
{
    rml_element *row = row_of(s, index);
    rml_element *toggles[2] = { NULL, NULL };
    if (row) {
        rml_element_query_selector_all(row, ".view-toggles .toggle", toggles, 2);
    }
    return toggles[which];
}

static unsigned view_of(studio *s, int index)
{
    return app_scene_authoring_actor_view(s->scene, index);
}

/* The eye hides its object and the lock locks it, a press anywhere in their
   column down the row's height, without selecting the row; Alt+click on an
   eye shows that object alone. The hidden row dims. */
static void toggles_hide_and_lock()
{
    studio_fixture f = objects_open(3);
    studio *s = f.studio;
    rml_element *eye = toggle_of(s, 1, 0);
    if (eye) {
        click_toggle(&f, 1, 0, 0.1f, 0);
    }
    bool hidden = eye && (view_of(s, 1) & APP_SCENE_VIEW_HIDDEN) &&
                  f.scene->selected == 0 && scene_selection_count(f.scene) == 1;
    rml_element *row = studio_element(s, "#outliner-list .tree-row[data-hidden]");
    rml_element *lock = toggle_of(s, 2, 1);
    if (lock) {
        click_toggle(&f, 2, 1, 0.9f, 0);
    }
    bool locked = lock && (view_of(s, 2) & APP_SCENE_VIEW_LOCKED) &&
                  f.scene->selected == 0;
    click_toggle(&f, 0, 0, 0.5f, SDL_KMOD_ALT);
    bool alone = !(view_of(s, 0) & APP_SCENE_VIEW_HIDDEN) &&
                 (view_of(s, 1) & APP_SCENE_VIEW_HIDDEN) &&
                 (view_of(s, 2) & APP_SCENE_VIEW_HIDDEN);
    if (!(hidden && row && locked && alone)) {
        printf("  toggles: hidden %d dims %d locked %d alone %d\n", hidden, row != NULL, locked, alone);
    }
    test_check(
        hidden && row && locked && alone,
        "a row's eye hides its object and its lock locks it from the top to "
        "the bottom of the row without selecting it, the hidden row dims, "
        "and Alt+click on an eye shows that object alone"
    );
    fixture_free(&f);
}

/* Presses and releases `button` (SDL's) at (x, y) in dp. */
static void button_at(studio_fixture *f, float x, float y, int button)
{
    studio *s = f->studio;
    mouse_move(s, x, y);
    frames(f, 1);
    for (int down = 1; down >= 0; down--) {
        SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                         : SDL_EVENT_MOUSE_BUTTON_UP };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = (Uint8)button;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = x * s->prefs.ui_scale;
        event.button.y = y * s->prefs.ui_scale;
        assert(SDL_PushEvent(&event));
    }
    frames(f, 3);
}

/* The open menu's heading, or "" while it is closed. */
static void menu_heading(studio *s, char *out, size_t size)
{
    out[0] = '\0';
    rml_element *group = studio_element(s, "#outliner-menu[data-open] .menu-group");
    if (group) {
        rml_element_inner_rml(group, out, size);
    }
}

/* A right click on a row selects it unless the selection holds it, and
   opens the row menu at the pointer, headed by what it acts on; its items
   run Studio's commands. */
static void row_menu()
{
    studio_fixture f = objects_open(3);
    studio *s = f.studio;
    float x, y;
    point_in(s, row_of(s, 2), 0.4f, 0.5f, &x, &y);
    button_at(&f, x, y, SDL_BUTTON_RIGHT);
    char heading[64];
    menu_heading(s, heading, sizeof heading);
    rml_element *menu = studio_element(s, "#outliner-menu[data-open]");
    rml_vec2 at = menu ? rml_element_absolute_offset(menu, RML_BOX_BORDER)
                       : (rml_vec2){ 0 };
    float ratio = studio_pixel_ratio(s);
    bool opened = menu && f.scene->selected == 2 &&
                  scene_selection_count(f.scene) == 1 &&
                  !strcmp(heading, f.scene->actors[2].name) &&
                  fabsf(at.x / ratio - x) < 2 && fabsf(at.y / ratio - y) < 2;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    click_in(&f, row_of(s, 0), 0.5f, 0.5f, primary(s));
    point_in(s, row_of(s, 2), 0.4f, 0.5f, &x, &y);
    button_at(&f, x, y, SDL_BUTTON_RIGHT);
    menu_heading(s, heading, sizeof heading);
    bool kept = scene_selection_count(f.scene) == 2 && !strcmp(heading, "2 objects");
    rml_element *items[24];
    int count = test_query_all(
        rml_backend_context(s->backend),
        "#outliner-menu[data-open] .menu-item",
        items,
        24
    );
    rml_element *duplicate = NULL;
    for (int i = 0; i < count && i < 24; i++) {
        char text[64] = "";
        rml_element *label = rml_element_query_selector(items[i], ".label");
        if (label) {
            rml_element_inner_rml(label, text, sizeof text);
        }
        duplicate = !strcmp(text, "Duplicate") ? items[i] : duplicate;
    }
    int objects = acount(f.scene->actors);
    if (duplicate) {
        click_in(&f, duplicate, 0.5f, 0.5f, 0);
    }
    /* The click closes the menu, which fades out, then hides. */
    rml_element *leaving = studio_element(s, "#outliner-menu[data-open]");
    bool closed = !leaving || rml_element_has_attribute(leaving, "data-ending-style");
    if (leaving) {
        frames(&f, (int)ceil(motion_transition_time(leaving) / FRAME_SECONDS) + 1);
    }
    bool ran = acount(f.scene->actors) == objects + 2 && closed &&
               !studio_element(s, "#outliner-menu[data-open]");
    if (!(opened && kept && ran)) {
        printf("  menu: opened %d kept %d ran %d, '%s', %d items\n", opened, kept, ran, heading, count);
    }
    test_check(
        opened && kept && ran,
        "a right click on a row selects it unless the selection holds it and "
        "opens the row menu at the pointer, headed by what it acts on, whose "
        "items run Studio's commands"
    );
    fixture_free(&f);
}

/* The filter field shows the rows it matches and their count, and a notice
   when none does; the root folds with its twisty, a double click, or Left
   and Right over the Outliner. */
static void filter_and_root()
{
    studio_fixture f = objects_open(3);
    studio *s = f.studio;
    studio_add_object(s, "Binding");
    frames(&f, 3);
    rml_element *field = studio_element(s, "#outliner-filter");
    if (field) {
        click_in(&f, field, 0.5f, 0.5f, 0);
    }
    text_event("Bind");
    frames(&f, 3);
    char count[16] = "", first[128] = "";
    rml_element *meta = studio_element(s, "#outliner-list .outliner-root .trail .meta");
    if (meta) {
        rml_element_inner_rml(meta, count, sizeof count);
    }
    int rows = list_rows(s, "#outliner-list .list-spacer", first, sizeof first);
    bool filtered = !strcmp(s->outliner_filter, "Bind") &&
                    !strcmp(count, "1") && rows == 2;
    text_event("zz");
    frames(&f, 3);
    rml_element *empty = studio_element(s, ".outliner-empty");
    char notice[64] = "";
    if (empty && rml_element_is_visible(empty, true)) {
        rml_element_inner_rml(empty, notice, sizeof notice);
    }
    bool none = !strcmp(notice, "No matches");
    s->outliner_filter[0] = '\0';
    frames(&f, 3);
    rml_element *twisty = studio_element(s, "#outliner-list .outliner-root .twisty");
    if (twisty) {
        click_in(&f, twisty, 0.5f, 0.5f, 0);
    }
    /* The root's fold is the Outliner's own view state (OutlinerRows), so
       the rows under it tell whether it is open. */
    bool folded = twisty && !row_of(s, 0);
    mouse_over(&f, STUDIO_AREA_OUTLINER);
    tap(SDL_SCANCODE_RIGHT);
    frames(&f, 3);
    bool right = row_of(s, 0) != NULL;
    tap(SDL_SCANCODE_LEFT);
    frames(&f, 3);
    bool left = !row_of(s, 0);
    rml_element *root = studio_element(s, "#outliner-list .outliner-root .name");
    float x, y;
    if (root) {
        point_in(s, root, 0.5f, 0.5f, &x, &y);
        mouse_move(s, x, y);
        frames(&f, 1);
        for (int i = 0; i < 2; i++) {
            graph_drag_button(s, x, y, true);
            graph_drag_button(s, x, y, false);
        }
        frames(&f, 3);
    }
    bool doubled = root && row_of(s, 0);
    if (!(filtered && none && folded && right && left && doubled)) {
        printf(
            "  filter: %d ('%s', count '%s', %d rows) none %d ('%s') folded %d "
            "right %d left %d double %d\n",
            filtered,
            s->outliner_filter,
            count,
            rows,
            none,
            notice,
            folded,
            right,
            left,
            doubled
        );
    }
    test_check(
        filtered && none,
        "the Outliner's filter shows the rows it matches and their count, "
        "and No matches when none does"
    );
    test_check(
        folded && right && left && doubled,
        "the scene's root folds with its twisty, a double click, and Left "
        "and Right over the Outliner"
    );
    fixture_free(&f);
}

/* The first row data-for made in the Outliner, past its template. */
static rml_element *first_row(studio *s)
{
    rml_element *rows[2] = { NULL, NULL };
    test_query_all(rml_backend_context(s->backend), "#outliner-list .outliner-row", rows, 2);
    return rows[0] && rml_element_has_attribute(rows[0], "data-for") ? rows[1]
                                                                     : rows[0];
}

/* Thousands of objects show as a window of rows, which keeps its length and
   its elements as the list scrolls, and Down reveals the row it selects. */
static void rows_window()
{
    studio_fixture f = objects_open(1);
    studio *s = f.studio;
    for (int i = 0; i < 2000; i++) {
        studio_add_object(s, "Vault");
    }
    select_instance(f.scene, 0);
    frames(&f, 4);
    rml_element *list = studio_element(s, "#outliner-list");
    rml_element *spacer = studio_element(s, "#outliner-list .list-spacer");
    char first[256] = "", later[256] = "";
    int rows = list_rows(s, "#outliner-list .list-spacer", first, sizeof first);
    rml_element *kept = first_row(s);
    char height[32] = "";
    float row = 0;
    if (spacer) {
        rml_element_property(spacer, "--row", height, sizeof height);
        row = (float)atof(height);
        rml_element_set_scroll_top(list, row * 500 * studio_pixel_ratio(s));
    }
    frames(&f, 4);
    int scrolled = list_rows(s, "#outliner-list .list-spacer", later, sizeof later);
    bool windowed = list && rows > 4 && rows < 80 && scrolled == rows &&
                    strcmp(first, later) && !row_of(s, 3) && row_of(s, 500) &&
                    kept && first_row(s) == kept;
    mouse_over(&f, STUDIO_AREA_OUTLINER);
    select_instance(f.scene, 0);
    tap(SDL_SCANCODE_DOWN);
    frames(&f, 4);
    bool revealed = f.scene->selected == 1 && row_of(s, 1) != NULL;
    if (!(windowed && revealed)) {
        printf("  window: %d rows then %d, revealed %d\n", rows, scrolled, revealed);
    }
    test_check(
        windowed && revealed,
        "the Outliner shows a window of thousands of rows that keeps its "
        "length as it scrolls, and Down reveals the row it selects"
    );
    fixture_free(&f);
}

/* Tab gives the Outliner's list the keyboard, which shows the active row's
   focus ring; then Studio's keys act on the Outliner wherever the pointer
   is: Down steps the selection, F2 renames in place and Enter returns the
   keyboard to the list. */
static void list_takes_tab()
{
    studio_fixture f = objects_open(3);
    studio *s = f.studio;
    rml_element *field = studio_element(s, "#outliner-filter");
    if (field) {
        click_in(&f, field, 0.5f, 0.5f, 0);
    }
    kit_rect scene = s->areas[STUDIO_AREA_TOP].rect;
    mouse_move(s, scene.x + scene.w * 0.5f, scene.y + scene.h * 0.5f);
    frames(&f, 1);
    /* Past Add and Maximize. */
    rml_element *list = studio_element(s, "#outliner-list");
    for (int i = 0; i < 4 && list && !rml_element_is_pseudo_class_set(list, "focus");
         i++) {
        tap(SDL_SCANCODE_TAB);
        frames(&f, 2);
    }
    bool focused = list && rml_element_is_pseudo_class_set(list, "focus-visible") &&
                   studio_ui_keyboard_area(s) == STUDIO_AREA_OUTLINER;
    tap(SDL_SCANCODE_DOWN);
    frames(&f, 3);
    bool stepped = f.scene->selected == 1;
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    bool renaming = s->rename.entity && s->rename.outliner;
    text_event("Kept");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool renamed = named(f.scene, "Kept") && !s->rename.entity && list &&
                   rml_element_is_pseudo_class_set(list, "focus-visible");
    if (!(focused && stepped && renaming && renamed)) {
        printf("  tab: focused %d stepped %d renaming %d renamed %d\n", focused, stepped, renaming, renamed);
    }
    test_check(
        focused && stepped && renaming && renamed,
        "Tab gives the Outliner's list the keyboard, and its keys then act on "
        "the Outliner wherever the pointer is: Down, F2, and Enter back to the "
        "list"
    );
    fixture_free(&f);
}

/* ---------- Refresh follows what the panels read ---------- */

/* The Refresh events the panels' behaviors ran in the next frame. */
static uint64_t refreshes(studio_fixture *f)
{
    game_behavior_world *behaviors = studio_ui_behaviors(f->studio);
    game_behavior_set_profiling(behaviors, true);
    game_behavior_profile_begin(behaviors);
    frames(f, 1);
    const game_behavior_profile_frame *profile = game_behavior_profile(behaviors);
    uint64_t runs = 0;
    for (int i = 0; i < profile->count; i++) {
        runs += !strcmp(profile->rows[i].event, "Refresh") ? profile->rows[i].dispatches
                                                           : 0;
    }
    game_behavior_set_profiling(behaviors, false);
    return runs;
}

/* The name row `index` shows, or "" while it is out of the window. */
static void row_name(studio *s, int index, char *out, size_t size)
{
    rml_element *row = row_of(s, index);
    rml_element *name = row ? rml_element_query_selector(row, ".name") : NULL;
    out[0] = '\0';
    if (name) {
        rml_element_inner_rml(name, out, size);
    }
}

static int prototype_options(studio *s)
{
    rml_element *popup = studio_element(s, "#outliner-add");
    return popup ? rml_element_query_selector_all(popup, ".menu-item", NULL, 0) : -1;
}

/* A panel's behaviors refresh only when what they read changed: nothing
   runs in a frame with nothing changed, even when a UI frame is forced, and
   each thing the Outliner shows reaches its rows by the next frame. */
static void refresh_follows_changes()
{
    studio_fixture f = objects_open(3);
    studio *s = f.studio;
    app_scene_authoring *scene = f.scene;
    uint64_t idle = 0;
    for (int i = 0; i < 4; i++) {
        s->ui_schedule.wake = true;
        idle += refreshes(&f);
    }
    test_check(idle == 0, "a UI frame with nothing changed runs no panel's Refresh");
    /* A name Properties commits, and its Undo. */
    char before[64], name[64];
    row_name(s, 1, before, sizeof before);
    studio_rename_commit(s, ecs_entity_to_u64(scene->actors[1].entity), "Annex");
    uint64_t ran = refreshes(&f);
    row_name(s, 1, name, sizeof name);
    bool renamed = ran > 0 && !strcmp(name, "Annex");
    studio_history(s, false);
    refreshes(&f);
    row_name(s, 1, name, sizeof name);
    bool undone = !strcmp(name, before);
    /* A selection the Scene makes, then a second object. */
    select_instance(scene, 2);
    refreshes(&f);
    row_name(s, 2, name, sizeof name);
    rml_element *active = studio_element(s, "#outliner-list .tree-row[data-active] .name");
    char shown[64] = "";
    if (active) {
        rml_element_inner_rml(active, shown, sizeof shown);
    }
    bool selected = !strcmp(shown, name);
    scene_toggle_instance(scene, 0);
    refreshes(&f);
    bool marked = studio_element(s, "#outliner-list .tree-row[data-marked]") != NULL;
    /* Hide in Editor, a command, and Show All. */
    studio_hide(s, true);
    refreshes(&f);
    bool hidden = studio_element(s, "#outliner-list .tree-row[data-hidden]") != NULL;
    studio_show_all(s);
    refreshes(&f);
    bool shown_again = !studio_element(s, "#outliner-list .tree-row[data-hidden]");
    /* An object added, and the filter Studio sets. */
    studio_add_object(s, "Vault");
    refreshes(&f);
    /* The List windows its new length once it has laid out. */
    frames(&f, 2);
    bool added = row_of(s, 3) != NULL;
    snprintf(s->outliner_filter, sizeof s->outliner_filter, "Vault_1");
    refreshes(&f);
    bool filtered = row_of(s, 1) && !row_of(s, 0) && !row_of(s, 2);
    s->outliner_filter[0] = '\0';
    refreshes(&f);
    frames(&f, 2);
    bool cleared = row_of(s, 0) && row_of(s, 2);
    /* An asset the project gains joins Add's list. */
    int options = prototype_options(s);
    char *json = new_asset_text(NEW_ASSET_PROTOTYPE, "Crate");
    int registered = app_scene_authoring_register_prototype(scene, json, NULL);
    free(json);
    refreshes(&f);
    bool asset = registered == 0 && options >= 0 &&
                 prototype_options(s) == options + 1;
    /* Play shuts Add, and Stop opens it. */
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 3);
    bool playing = studio_element(s, "button[popovertarget=outliner-add][data-disabled]") !=
                   NULL;
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 3);
    bool stopped = !studio_element(s, "button[popovertarget=outliner-add][data-disabled]");
    idle = 0;
    for (int i = 0; i < 3; i++) {
        idle += refreshes(&f);
    }
    if (!(renamed && undone && selected && marked && hidden && shown_again &&
          added && filtered && cleared && asset && playing && stopped && !idle)) {
        printf(
            "  refresh: renamed %d undone %d selected %d marked %d hidden %d "
            "shown %d "
            "added %d filtered %d cleared %d asset %d playing %d stopped %d "
            "idle %d\n",
            renamed,
            undone,
            selected,
            marked,
            hidden,
            shown_again,
            added,
            filtered,
            cleared,
            asset,
            playing,
            stopped,
            (int)idle
        );
    }
    test_check(
        renamed && undone && selected && marked && hidden && shown_again && added,
        "the Outliner follows a name Properties commits and its Undo, the "
        "Scene's selection, Hide in Editor, and an object added"
    );
    test_check(
        filtered && cleared && asset && playing && stopped && !idle,
        "the Outliner follows the filter Studio sets, an asset the project "
        "gains, and Play and Stop, and settles to no Refresh after them"
    );
    fixture_free(&f);
}

/* What the editor nodes read moves the stamp that wakes Refresh: each change
   a panel can show differs from the last, and a frame between them does not. */
static void stamp_follows_reads()
{
    studio_fixture f = objects_open(3);
    studio *s = f.studio;
    app_scene_authoring *scene = f.scene;
    uint64_t at = studio_nodes_stamp(s);
    frames(&f, 3);
    bool still = studio_nodes_stamp(s) == at;
    bool moved[9] = { false };
    int step = 0;
#define MOVES(change)                                                          \
    do {                                                                       \
        change;                                                                \
        frames(&f, 2);                                                         \
        uint64_t now = studio_nodes_stamp(s);                                  \
        moved[step++] = now != at;                                             \
        at = now;                                                              \
    } while (0)
    MOVES(select_instance(scene, 1));
    MOVES(studio_toggle_spawn_break(s, "Vault"));
    MOVES(studio_rename_object(s));
    MOVES(s->rename.entity = 0);
    MOVES(app_scene_authoring_set_instance_behavior(scene, "Teller", false));
    MOVES(app_scene_authoring_set_instance_behavior(scene, "Teller", true));
    MOVES(studio_history(s, false));
    MOVES(studio_runtime(s, APP_RUNTIME_PLAY));
    MOVES(studio_runtime(s, APP_RUNTIME_STOP));
#undef MOVES
    bool all = true;
    for (int i = 0; i < step; i++) {
        all &= moved[i];
    }
    if (!(still && all)) {
        printf(
            "  stamp: still %d, moved %d %d %d %d %d %d %d %d %d\n",
            still,
            moved[0],
            moved[1],
            moved[2],
            moved[3],
            moved[4],
            moved[5],
            moved[6],
            moved[7],
            moved[8]
        );
    }
    test_check(
        still && all,
        "the editor nodes' stamp holds between frames and moves with the "
        "selection, a spawn break, in-place rename, an attached behavior, "
        "Undo, and Play and Stop"
    );
    fixture_free(&f);
}

void studio_documents_outliner_checks()
{
    designer_selection_reveals_hierarchy();
    rename_objects();
    rows_select();
    toggles_hide_and_lock();
    row_menu();
    filter_and_root();
    rows_window();
    list_takes_tab();
    refresh_follows_changes();
    stamp_follows_reads();
}
