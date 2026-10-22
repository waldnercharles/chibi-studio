/* The visual designer in Studio (workstream D1): the Hierarchy, Details and
 * Library are authored (Hierarchy.rml, Details.rml, Library.rml) over the
 * `hierarchy`, `details` and `library` sources, with each change a text edit
 * of the open UI document, one Undo step. These checks open the fixture's HUD
 * document with its text replaced in memory (nothing is saved) and drive the
 * panels as a user does: rows select, fold and drag between drop zones; the
 * row menu and the header's Add change the document; Details edits every
 * kind of value in its control, through gestures that join one step; the
 * Library's tiles add and drag. */

#include "editor/studio/studio_designer_internal.h"
#include "support/array.h"
#include "support/text_edit.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/popover.h"

#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The elements of DOCUMENT, in outline order. */
enum
{
    ROOT,
    TITLE,
    ROW,
    OK,
    CANCEL,
    CHECK,
    PROGRESS,
    METER,
    ELEMENTS,
};

static const char DOCUMENT
    [] = "<div id=\"root\" class=\"panel\" style=\"width: 280dp; opacity: "
         "0.85; display: flex; "
         "box-sizing: border-box; background-color: #22334488\">\n"
         "  <span class=\"title big\" data-if=\"shown\">Title</span>\n"
         "  <div class=\"row\">\n"
         "    <button id=\"ok\" data-event-click=\"emit('Ok')\">OK</button>\n"
         "    <button id=\"cancel\">Cancel</button>\n"
         "    <input type=\"checkbox\"/>\n"
         "  </div>\n"
         "  <progress value=\"0.5\"/>\n"
         "  <use ui=\"Meter\" id=\"meter\"/>\n"
         "</div>\n";

static const char
    STYLES[] = ".panel { display: block; width: 240dp; color: #ffffff; }\n"
               ".row button { color: #ff0000; flex-grow: 1; }\n"
               ".title { font-weight: bold; }\n";

#define HUD "test/fixtures/studio-workspace/content/ui/HUD.rml"

/* Studio over the synthetic workspace with its HUD document open as the designer's, its
   markup and styles replaced in memory, and Hierarchy, Details and the
   Library in view beside the Canvas. */
static studio_fixture designer_open()
{
    studio_fixture f = scene_open(
        "test/fixtures/studio-workspace",
        "test/fixtures/studio-workspace/content/scenes/Sandbox.chibi",
        "Movement"
    );
    studio *s = f.studio;
    s->maximized = -1;
    s->prefs.code_apply_live = false;
    s->prefs.reduced_motion = true;
    char path[PATH_MAX];
    assert(realpath(HUD, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 4);
    ui_buffer *buffer = studio_ui_buffer(s);
    text_edit rml = { 0, (int)strlen(ui_buffer_text(buffer, UI_PART_RML)), DOCUMENT };
    ui_buffer_edit(buffer, UI_PART_RML, &rml, 1);
    text_edit rcss = { 0, (int)strlen(ui_buffer_text(buffer, UI_PART_RCSS)), STYLES };
    ui_buffer_edit(buffer, UI_PART_RCSS, &rcss, 1);
    studio_show_editor(s, STUDIO_EDITOR_ASSETS);
    frames(&f, 8);
    return f;
}

static const char *markup_of(studio *s)
{
    return ui_buffer_text(studio_ui_buffer(s), UI_PART_RML);
}

static int undo_steps(studio *s)
{
    return acount(studio_ui_buffer(s)->undo);
}

static studio_designer *designer(studio *s)
{
    ui_buffer *buffer;
    studio_designer *d = designer_state(s, &buffer);
    assert(d);
    return d;
}

static int element_count(studio *s)
{
    return acount(designer(s)->rml->elements);
}

static bool has_text(studio *s, const char *needle)
{
    return strstr(markup_of(s), needle) != NULL;
}

/* ---------- The Hierarchy ---------- */

/* The row of element `e`: the use TreeRow marks with data-element. */
static rml_element *hrow(studio *s, int e)
{
    char selector[64];
    snprintf(selector, sizeof selector, "#hierarchy-list [data-element=e%d]", e);
    return studio_element(s, selector);
}

static rml_element *tree_row_of(rml_element *use)
{
    return use ? rml_element_query_selector(use, ".tree-row") : NULL;
}

static int visible_rows(studio *s)
{
    rml_element *rows[128];
    int count = test_query_all(rml_backend_context(s->backend), "#hierarchy-list [data-element]", rows, 128);
    int shown = 0;
    for (int i = 0; i < count; i++) {
        shown += rml_element_is_visible(rows[i], true);
    }
    return shown;
}

static const char *inner(rml_element *el, char *out, size_t size)
{
    out[0] = '\0';
    if (el) {
        rml_element_inner_rml(el, out, size);
    }
    return out;
}

/* The pointer, `dy` of `el`'s height from its middle, in dp. */
static void at(studio *s, rml_element *el, float dy, float *x, float *y)
{
    rml_vec2 at_ = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    *x = (at_.x + size.x * 0.5f) / ratio;
    *y = (at_.y + size.y * (0.5f + dy)) / ratio;
}

/* A right press at the middle of `el`. */
static void right_press(studio *s, rml_element *el)
{
    float x, y;
    at(s, el, 0, &x, &y);
    rml_input input = { .x = x, .y = y, .button = 1 };
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        input.type = types[i];
        rml_backend_process_input(s->backend, &input);
    }
}

/* Presses `from` and carries it past the drag threshold to `to`, `dy` of its
   height from its middle, and holds it there, at (*tx, *ty). */
static void carry_to(studio_fixture *f, rml_element *from, rml_element *to, float dy, float *tx, float *ty)
{
    studio *s = f->studio;
    float x, y;
    at(s, from, 0, &x, &y);
    at(s, to, dy, tx, ty);
    mouse_move(s, x, y);
    frames(f, 1);
    graph_drag_button(s, x, y, true);
    frames(f, 1);
    mouse_move(s, x + 12, y);
    frames(f, 2);
    mouse_move(s, *tx, *ty);
    frames(f, 3);
}

static void release(studio_fixture *f, float x, float y)
{
    graph_drag_button(f->studio, x, y, false);
    frames(f, 4);
}

static void click_row(studio_fixture *f, int e)
{
    press(f->studio, hrow(f->studio, e));
    frames(f, 3);
}

/* Rows, selection, the twisty and the filter. */
static void hierarchy_rows_and_selection()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    rml_element *root = studio_element(s, "#hierarchy-list .hierarchy-root");
    char text[64];
    bool listed = root && visible_rows(s) == ELEMENTS + 1;
    rml_element *meta = root ? rml_element_query_selector(root, ".meta") : NULL;
    bool document = meta && !strcmp(inner(meta, text, sizeof text), "8") &&
                    strstr(inner(rml_element_query_selector(root, ".name"), text, sizeof text), "HUD");
    test_check(
        listed && document,
        "Hierarchy lists the document as a row with its element count, then a "
        "row for each element"
    );
    click_row(&f, OK);
    rml_element *name = studio_element(s, "#hierarchy-list .tree-row[data-selected] .name");
    bool selected = designer(s)->selected == OK && name &&
                    !strcmp(inner(name, text, sizeof text), "button");
    rml_element *title = studio_element(s, ".identity .title");
    bool shown = title && !strcmp(inner(title, text, sizeof text), "button#ok");
    click_row(&f, TITLE);
    rml_element *twisty = rml_element_query_selector(hrow(s, ROW), ".twisty");
    int before = visible_rows(s);
    press(s, twisty);
    frames(&f, 3);
    bool folded = visible_rows(s) == before - 3 && designer(s)->selected == TITLE &&
                  acount(designer(s)->folded) == 1;
    press(s, rml_element_query_selector(hrow(s, ROW), ".twisty"));
    frames(&f, 3);
    bool unfolded = visible_rows(s) == before && !acount(designer(s)->folded);
    test_check(
        selected && shown && folded && unfolded,
        "a Hierarchy row selects on click and Details names it; a twisty folds "
        "its row without selecting it and unfolds it again"
    );
    click_row(&f, ROOT);
    rml_element *filter = studio_element(s, "#hierarchy-filter");
    if (filter) {
        rml_element_focus(filter, true);
        text_event("cancel");
    }
    frames(&f, 4);
    bool narrowed = visible_rows(s) <= 4 && hrow(s, CANCEL) &&
                    rml_element_is_visible(hrow(s, CANCEL), true);
    if (filter) {
        for (int i = 0; i < 6; i++) {
            tap(SDL_SCANCODE_BACKSPACE);
        }
        text_event("zzz");
    }
    frames(&f, 4);
    rml_element *empty = studio_element(s, ".outliner-empty");
    bool none = empty && rml_element_is_visible(empty, true) &&
                !strcmp(inner(empty, text, sizeof text), "No matches");
    test_check(
        narrowed && none,
        "the Hierarchy's filter keeps the rows that match or hold a match, and "
        "says No matches when none does"
    );
    fixture_free(&f);
}

/* Drops: a row onto the middle or the edge of another, the insertion marks,
   Escape, a Library tile, and a row carried with the keyboard. */
static void hierarchy_drops()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    click_row(&f, ROOT);
    int steps = undo_steps(s);
    float px, py;
    carry_to(&f, hrow(s, TITLE), hrow(s, ROW), 0, &px, &py);
    char zone[16] = "";
    rml_element_attribute(hrow(s, ROW), "data-drop-zone", zone, sizeof zone);
    bool inside = rml_element_has_attribute(hrow(s, ROW), "data-drop-over") &&
                  !strcmp(zone, "inside");
    float x, y;
    at(s, hrow(s, ROW), -0.45f, &x, &y);
    mouse_move(s, x, y);
    frames(&f, 3);
    rml_element_attribute(hrow(s, ROW), "data-drop-zone", zone, sizeof zone);
    bool before = !strcmp(zone, "before");
    at(s, hrow(s, OK), 0.4f, &x, &y);
    mouse_move(s, x, y);
    frames(&f, 3);
    char leaf[16] = "";
    rml_element_attribute(hrow(s, OK), "data-drop-zone", leaf, sizeof leaf);
    bool halves = !strcmp(leaf, "after");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    release(&f, x, y);
    bool canceled = undo_steps(s) == steps && !strcmp(markup_of(s), DOCUMENT);
    test_check(
        inside && before && halves && canceled,
        "a Hierarchy row dragged over another marks where it drops: inside a "
        "container's middle, before it at its top edge, and after a leaf's "
        "lower half; Escape drops nothing"
    );
    carry_to(&f, hrow(s, TITLE), hrow(s, ROW), 0, &px, &py);
    release(&f, px, py);
    ui_buffer *buffer = studio_ui_buffer(s);
    const char *
        moved = "  <div class=\"row\">\n"
                "    <button id=\"ok\" "
                "data-event-click=\"emit('Ok')\">OK</button>\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "    <input type=\"checkbox\"/>\n"
                "    <span class=\"title big\" data-if=\"shown\">Title</span>\n"
                "  </div>\n";
    bool reparented = has_text(s, moved) && undo_steps(s) == steps + 1 &&
                      designer(s)->selected >= 0 &&
                      !strcmp(buffer->path, designer(s)->path);
    studio_history(s, false);
    frames(&f, 3);
    bool undone = !strcmp(markup_of(s), DOCUMENT);
    carry_to(&f, hrow(s, CANCEL), hrow(s, OK), -0.4f, &px, &py);
    release(&f, px, py);
    bool reordered = has_text(s, "<button id=\"cancel\">Cancel</button>\n    <button id=\"ok\"") &&
                     undo_steps(s) == steps + 1;
    studio_history(s, false);
    frames(&f, 3);
    test_check(
        reparented && undone && reordered,
        "dropping a Hierarchy row inside a container moves it to the end of "
        "its children, and above a leaf puts it before it, each one Undo step"
    );
    rml_element *tile = studio_element(s, ".designer-tile");
    int count = element_count(s);
    carry_to(&f, tile, hrow(s, ROW), 0, &px, &py);
    release(&f, px, py);
    bool added = element_count(s) == count + 1 && has_text(s, "<div/>") &&
                 undo_steps(s) == steps + 1;
    studio_history(s, false);
    frames(&f, 3);
    test_check(
        added && !strcmp(markup_of(s), DOCUMENT),
        "a Library tile dragged onto a Hierarchy row adds its element there, "
        "one Undo step"
    );
    click_row(&f, CANCEL);
    mouse_move(
        s,
        s->areas[STUDIO_AREA_OUTLINER].rect.x + 40,
        s->areas[STUDIO_AREA_OUTLINER].rect.y + 60
    );
    frames(&f, 2);
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_SPACE, 0);
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_UP, 0);
    s->ui_schedule.wake = true;
    frames(&f, 4);
    rml_element *marked = studio_element(s, "#hierarchy-list [data-carry-zone=before], #hierarchy-list [data-carry-zone=after], #hierarchy-list [data-carry-zone=inside]");
    rml_element *dim = studio_element(s, "#hierarchy-list [data-dragging]");
    bool carrying = marked && dim && designer(s)->carry.active;
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_ESCAPE, 0);
    s->ui_schedule.wake = true;
    frames(&f, 4);
    bool put_down = !designer(s)->carry.active &&
                    !studio_element(s, "#hierarchy-list [data-carry-zone=before], #hierarchy-list [data-carry-zone=after], #hierarchy-list [data-carry-zone=inside]") &&
                    !strcmp(markup_of(s), DOCUMENT);
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_SPACE, 0);
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_UP, 0);
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_UP, 0);
    studio_designer_key(s, STUDIO_EDITOR_OUTLINER, SDLK_SPACE, 0);
    frames(&f, 3);
    bool kept = has_text(s, "<button id=\"cancel\">Cancel</button>\n    <button id=\"ok\"") &&
                undo_steps(s) == steps + 1;
    if (!(carrying && put_down && kept)) {
        printf(
            "  carry: marked %d, dim %d, active %d, put down %d, kept %d, "
            "steps %d from %d\n%s\n",
            marked != NULL,
            dim != NULL,
            designer(s)->carry.active,
            put_down,
            kept,
            undo_steps(s),
            steps,
            markup_of(s)
        );
    }
    test_check(
        carrying && put_down && kept,
        "a row carried with the keyboard dims and marks the slot its arrow "
        "chose, Escape puts it down, and Space drops it as one Undo step"
    );
    fixture_free(&f);
}

/* The row menu and the header's Add. */
static void hierarchy_menus()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    int steps = undo_steps(s), count = element_count(s);
    right_press(s, hrow(s, OK));
    frames(&f, 4);
    rml_element *menu = studio_element(s, "#hierarchy-menu");
    char text[64];
    rml_element *group = menu ? rml_element_query_selector(menu, ".menu-group") : NULL;
    bool opened = menu && popover_open(menu) && designer(s)->selected == OK &&
                  group && !strcmp(inner(group, text, sizeof text), "button");
    rml_element *items[64];
    int shown = menu ? rml_element_query_selector_all(menu, ".menu-item", items, 64)
                     : 0;
    rml_element *duplicate = NULL, *widget = NULL;
    for (int i = 0; i < shown; i++) {
        rml_element *label = rml_element_query_selector(items[i], ".label");
        inner(label, text, sizeof text);
        duplicate = !strcmp(text, "Duplicate") && rml_element_is_visible(items[i], true)
                      ? items[i]
                      : duplicate;
        widget = !strcmp(text, "Open Widget") && rml_element_is_visible(items[i], true)
                   ? items[i]
                   : widget;
    }
    test_check(
        opened && duplicate && !widget,
        "a right press on a Hierarchy row selects it and opens the row menu "
        "at the pointer, headed by its element, offering Duplicate but Open "
        "Widget only for a widget"
    );
    if (duplicate) {
        press(s, duplicate);
    }
    frames(&f, 4);
    bool duplicated = element_count(s) == count + 1 &&
                      undo_steps(s) == steps + 1 && !popover_open(menu) &&
                      has_text(s, "<button id=\"ok\" data-event-click=\"emit('Ok')\">OK</button>\n    <button id=\"ok\"");
    studio_history(s, false);
    frames(&f, 3);
    right_press(s, hrow(s, METER));
    frames(&f, 4);
    int open_items = rml_element_query_selector_all(menu, ".menu-item", items, 64);
    bool widget_item = false;
    for (int i = 0; i < open_items; i++) {
        inner(rml_element_query_selector(items[i], ".label"), text, sizeof text);
        widget_item |= !strcmp(text, "Open Widget") &&
                       rml_element_is_visible(items[i], true);
    }
    popover_hide_all(rml_backend_context(s->backend));
    frames(&f, 3);
    click_row(&f, CANCEL);
    mouse_move(
        s,
        s->areas[STUDIO_AREA_OUTLINER].rect.x + 40,
        s->areas[STUDIO_AREA_OUTLINER].rect.y + 60
    );
    frames(&f, 2);
    tap(SDL_SCANCODE_X);
    frames(&f, 4);
    bool deleted = element_count(s) == count - 1 &&
                   undo_steps(s) == steps + 1 && !has_text(s, "id=\"cancel\"");
    studio_history(s, false);
    frames(&f, 3);
    test_check(
        duplicated && widget_item && deleted && !strcmp(markup_of(s), DOCUMENT),
        "the row menu's Duplicate copies the element and X deletes it, each "
        "one Undo step, and a widget's menu offers Open Widget"
    );
    click_row(&f, ROW);
    rml_element *add = studio_element(s, "[data-tip=Add]");
    press(s, add);
    frames(&f, 4);
    rml_element *list = studio_element(s, "#hierarchy-add");
    shown = list ? rml_element_query_selector_all(list, ".menu-item", items, 64) : 0;
    rml_element *container = NULL;
    for (int i = 0; i < shown; i++) {
        inner(rml_element_query_selector(items[i], ".label"), text, sizeof text);
        container = !strcmp(text, "Container") ? items[i] : container;
    }
    bool menu_open = list && popover_open(list) && container;
    if (container) {
        press(s, container);
    }
    frames(&f, 4);
    bool inside = element_count(s) == count + 1 && undo_steps(s) == steps + 1 &&
                  has_text(s, "    <input type=\"checkbox\"/>\n    <div/>\n  </div>");
    studio_history(s, false);
    frames(&f, 3);
    test_check(
        menu_open && inside && !strcmp(markup_of(s), DOCUMENT),
        "the header's Add lists the Library's elements and adds the chosen one "
        "inside the selected container, one Undo step"
    );
    fixture_free(&f);
}

/* ---------- Details ---------- */

/* The row of Details labeled `label`, the first in the panel. */
static rml_element *prop(studio *s, const char *label)
{
    rml_element *rows[64];
    int count = test_query_all(rml_backend_context(s->backend), ".designer-details .prop", rows, 64);
    for (int i = 0; i < count; i++) {
        char text[64];
        rml_element *name = rml_element_query_selector(rows[i], ".label span");
        if (!strcmp(inner(name, text, sizeof text), label) &&
            rml_element_is_visible(rows[i], true)) {
            return rows[i];
        }
    }
    return NULL;
}

static rml_element *control(studio *s, const char *label, const char *selector)
{
    rml_element *row = prop(s, label);
    return row ? rml_element_query_selector(row, selector) : NULL;
}

/* A text field's commit, as typing and Enter or leaving the field sends it. */
static void commit(studio_fixture *f, rml_element *field, const char *value)
{
    assert(field);
    rml_element_set_value(field, value);
    rml_element_dispatch_value(field, "change", value, false);
    frames(f, 4);
}

static void choose(studio_fixture *f, int e)
{
    designer_select(f->studio, e);
    f->studio->ui_schedule.wake = true;
    frames(f, 4);
}

/* Text fields, class chips and the Add popover. */
static void details_attributes_and_classes()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    choose(&f, ROOT);
    int steps = undo_steps(s);
    commit(&f, control(s, "id", "input"), "main");
    bool renamed = has_text(s, "id=\"main\"") && undo_steps(s) == steps + 1;
    commit(&f, control(s, "id", "input"), "");
    bool cleared = !has_text(s, "id=\"main\"") && !has_text(s, "id=\"\"") &&
                   undo_steps(s) == steps + 2;
    choose(&f, OK);
    commit(&f, control(s, "text", "input"), "Okay");
    bool retyped = has_text(s, ">Okay</button>") && undo_steps(s) == steps + 3;
    commit(&f, control(s, "data-event-click", "input"), "emit('Fine')");
    bool bound = has_text(s, "data-event-click=\"emit('Fine')\"") &&
                 undo_steps(s) == steps + 4;
    press(s, control(s, "data-event-click", ".remove"));
    frames(&f, 4);
    bool removed = !has_text(s, "data-event-click") && undo_steps(s) == steps + 5;
    test_check(
        renamed && cleared && retyped && bound && removed,
        "Details' text fields commit the id, an element's text and an "
        "attribute or binding on Enter or blur, each one Undo step, and an "
        "attribute's remove button takes it out"
    );
    while (undo_steps(s) > steps) {
        studio_history(s, false);
    }
    frames(&f, 3);
    choose(&f, ROOT);
    rml_element *plus = control(s, "class", ".designer-add");
    press(s, plus);
    frames(&f, 6);
    rml_element *popover = studio_element(s, "#details-add");
    rml_element *field = studio_element(s, "#details-add-field");
    bool open = popover && popover_open(popover) && field &&
                rml_context_focus_element(rml_backend_context(s->backend)) == field;
    text_event("wide");
    frames(&f, 3);
    rml_element *suggestion = NULL;
    char text[64];
    rml_element *items[64];
    int shown = popover
                  ? rml_element_query_selector_all(popover, ".menu-item", items, 64)
                  : 0;
    for (int i = 0; i < shown; i++) {
        inner(rml_element_query_selector(items[i], ".label"), text, sizeof text);
        suggestion = rml_element_is_visible(items[i], true) ? items[i] : suggestion;
    }
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    bool added = has_text(s, "class=\"panel wide\"") &&
                 !popover_open(popover) && undo_steps(s) == steps + 1;
    press(s, plus);
    frames(&f, 6);
    shown = rml_element_query_selector_all(popover, ".menu-item", items, 64);
    rml_element *pick = NULL;
    for (int i = 0; i < shown; i++) {
        inner(rml_element_query_selector(items[i], ".label"), text, sizeof text);
        pick = !strcmp(text, "row") && rml_element_is_visible(items[i], true)
                 ? items[i]
                 : pick;
    }
    if (pick) {
        press(s, pick);
    }
    frames(&f, 4);
    bool picked = has_text(s, "class=\"panel wide row\"") &&
                  undo_steps(s) == steps + 2;
    rml_element *chip = studio_element(s, ".designer-chip .btn");
    if (chip) {
        press(s, chip);
    }
    frames(&f, 4);
    bool dropped = has_text(s, "class=\"wide row\"") && undo_steps(s) == steps + 3;
    if (!(open && added && picked && dropped && !suggestion)) {
        printf(
            "  class: open %d, added %d, picked %d, dropped %d, suggestion "
            "%d\n%s\n",
            open,
            added,
            picked,
            dropped,
            suggestion != NULL,
            markup_of(s)
        );
    }
    test_check(
        open && added && picked && dropped && !suggestion,
        "Details' class row adds a class from the Add popover, typed with "
        "Enter or picked from the document's suggestions, and a chip's "
        "button removes it, each one Undo step"
    );
    fixture_free(&f);
}

/* Values in their controls: a scrub joins one step, Escape undoes it, and a
   color, a keyword and a toggle commit. */
static void details_values()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    choose(&f, ROOT);
    int steps = undo_steps(s);
    rml_element *width = control(s, "width", "number-field");
    char unit[16] = "";
    if (width) {
        rml_element_attribute(width, "unit", unit, sizeof unit);
    }
    bool lengths = !strcmp(unit, "dp");
    rml_element_dispatch_value(width, "input", "290", false);
    frames(&f, 2);
    rml_element_dispatch_value(width, "input", "300", false);
    frames(&f, 2);
    rml_element_dispatch_value(width, "change", "300", false);
    frames(&f, 4);
    bool scrubbed = has_text(s, "width: 300dp") && undo_steps(s) == steps + 1;
    studio_history(s, false);
    frames(&f, 4);
    bool restored = has_text(s, "width: 280dp") && undo_steps(s) == steps;
    width = control(s, "width", "number-field");
    rml_element_dispatch_value(width, "input", "330", false);
    frames(&f, 2);
    rml_element_dispatch_value(width, "change", "280", true);
    frames(&f, 4);
    bool escaped = has_text(s, "width: 280dp") && undo_steps(s) == steps;
    test_check(
        lengths && scrubbed && restored && escaped,
        "a length scrubs with its unit and one Undo step joins every change "
        "of the gesture; Escape puts the value back and leaves no step"
    );
    rml_element *opacity = control(s, "opacity", "number-field");
    bool slider = opacity && rml_element_has_attribute(opacity, "slider");
    rml_element_dispatch_value(opacity, "change", "0.5", false);
    frames(&f, 4);
    bool faded = has_text(s, "opacity: 0.5") && undo_steps(s) == steps + 1;
    rml_element *color = control(s, "background-color", "color-field");
    rml_element_dispatch_value(color, "input", "#ff0000ff", false);
    frames(&f, 2);
    rml_element_dispatch_value(color, "change", "#ff0000ff", false);
    frames(&f, 4);
    bool painted = has_text(s, "background-color: #ff0000") &&
                   undo_steps(s) == steps + 2;
    rml_element *display = control(s, "display", ".enum-field");
    rml_element_dispatch_value(display, "change", "inline-block", false);
    frames(&f, 4);
    bool chosen = has_text(s, "display: inline-block") && undo_steps(s) == steps + 3;
    rml_element *segments[8];
    int found = rml_element_query_selector_all(prop(s, "box-sizing"), ".seg", segments, 8),
        count = 0;
    rml_element *first = NULL;
    for (int i = 0; i < found && i < 8; i++) {
        if (rml_element_is_visible(segments[i], true)) {
            first = first ? first : segments[i];
            count++;
        }
    }
    if (first) {
        press(s, first);
    }
    frames(&f, 4);
    bool toggled = count == 2 && has_text(s, "box-sizing: content-box") &&
                   undo_steps(s) == steps + 4;
    if (!(slider && faded && painted && chosen && toggled)) {
        printf(
            "  values: slider %d, faded %d, painted %d, chosen %d, toggled %d "
            "(%d segments)\n%s\n",
            slider,
            faded,
            painted,
            chosen,
            toggled,
            count,
            markup_of(s)
        );
    }
    test_check(
        slider && faded && painted && chosen && toggled,
        "Details edits opacity as a slider, a color in its picker, a keyword "
        "from its list and two keywords as segments, each one Undo step"
    );
    press(s, control(s, "opacity", ".remove"));
    frames(&f, 4);
    bool removed = !has_text(s, "opacity") && undo_steps(s) == steps + 5;
    if (!removed) {
        printf("  remove: steps %d from %d\n%s\n", undo_steps(s), steps, markup_of(s));
    }
    test_check(removed, "an inline property's remove button takes it out as one Undo step");
    fixture_free(&f);
}

/* An Add popover that refuses what it is given warns, and the rules list. */
static void details_rules_and_refusals()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    choose(&f, -1);
    int steps = undo_steps(s);
    rml_element *found[8], *panels[8];
    int all = test_query_all(rml_backend_context(s->backend), ".designer-details .panel", found, 8),
        count = 0;
    for (int i = 0; i < all && i < 8; i++) {
        if (rml_element_is_visible(found[i], true)) {
            panels[count++] = found[i];
        }
    }
    char text[64];
    bool listed = count == 3 &&
                  !strcmp(inner(rml_element_query_selector(panels[0], ".title"), text, sizeof text), ".panel");
    rml_element *width = control(s, "width", "number-field");
    rml_element_dispatch_value(width, "change", "260", false);
    frames(&f, 4);
    bool declaration = strstr(ui_buffer_text(studio_ui_buffer(s), UI_PART_RCSS), "width: 260dp") &&
                       undo_steps(s) == steps + 1;
    if (!(listed && declaration)) {
        printf(
            "  rules: %d panels, listed %d, declaration %d, steps %d from %d\n",
            count,
            listed,
            declaration,
            undo_steps(s),
            steps
        );
    }
    test_check(
        listed && declaration,
        "with nothing selected Details lists every rule of the document's RCSS "
        "and edits a declaration's value as one Undo step"
    );
    studio_history(s, false);
    choose(&f, ROOT);
    press(s, studio_element(s, ".designer-more .designer-add"));
    frames(&f, 6);
    int records = acount(s->records);
    text_event("width: 1dp; color: red");
    frames(&f, 2);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    bool refused = acount(s->records) > records && undo_steps(s) == steps &&
                   alast(s->records).kind == KIT_TOAST_WARNING;
    test_check(
        refused,
        "an Add popover that is given what the document would not take warns "
        "with the reason and changes nothing"
    );
    rml_element *fold = studio_element(s, ".panel-fold");
    rml_element *body = fold ? rml_element_query_selector(
                                   rml_element_parent(rml_element_parent(fold)),
                                   ".panel-body"
                               )
                             : NULL;
    bool open = body && rml_element_is_visible(body, true);
    press(s, fold);
    frames(&f, 3);
    rml_element *attributes = studio_element(s, ".panel[data-key=attributes] .panel-body");
    bool shut = !attributes || !rml_element_is_visible(attributes, true);
    press(s, studio_element(s, ".panel[data-key=attributes] .panel-fold"));
    frames(&f, 3);
    attributes = studio_element(s, ".panel[data-key=attributes] .panel-body");
    bool again = attributes && rml_element_is_visible(attributes, true);
    if (!(open && shut && again)) {
        printf("  fold: open %d, shut %d, again %d\n", open, shut, again);
    }
    test_check(open && shut && again, "a Details panel's head folds and unfolds it");
    fixture_free(&f);
}

/* ---------- Library ---------- */

static void library_tiles()
{
    studio_fixture f = designer_open();
    studio *s = f.studio;
    choose(&f, ROOT);
    rml_element *tiles[32];
    int count = test_query_all(rml_backend_context(s->backend), ".designer-tile", tiles, 32);
    int visible = 0;
    for (int i = 0; i < count; i++) {
        visible += rml_element_is_visible(tiles[i], true);
    }
    test_check(
        visible == 10 + 3,
        "the Library lists the ten elements and the project's other ui assets "
        "as widgets"
    );
    rml_element *filter = studio_element(s, "#library-filter");
    if (filter) {
        rml_element_focus(filter, true);
        text_event("Meter");
    }
    frames(&f, 4);
    count = test_query_all(rml_backend_context(s->backend), ".designer-tile", tiles, 32);
    visible = 0;
    for (int i = 0; i < count; i++) {
        visible += rml_element_is_visible(tiles[i], true);
    }
    for (int i = 0; filter && i < 5; i++) {
        tap(SDL_SCANCODE_BACKSPACE);
    }
    frames(&f, 4);
    test_check(visible == 1, "the Library's filter keeps the tiles whose names match");
    int steps = undo_steps(s), elements = element_count(s);
    rml_element *button = NULL;
    count = test_query_all(rml_backend_context(s->backend), ".designer-tile", tiles, 32);
    for (int i = 0; i < count; i++) {
        char text[64];
        if (!strcmp(inner(rml_element_query_selector(tiles[i], ".label"), text, sizeof text), "Button")) {
            button = tiles[i];
        }
    }
    press(s, button);
    frames(&f, 4);
    bool added = element_count(s) == elements + 1 && undo_steps(s) == steps + 1 &&
                 has_text(s, "  <button>Button</button>\n</div>");
    studio_history(s, false);
    frames(&f, 3);
    test_check(
        added && !strcmp(markup_of(s), DOCUMENT),
        "clicking a Library tile adds its element to the selection, one Undo "
        "step"
    );
    fixture_free(&f);
}

void studio_documents_designer_checks()
{
    hierarchy_rows_and_selection();
    hierarchy_drops();
    hierarchy_menus();
    details_attributes_and_classes();
    details_values();
    details_rules_and_refusals();
    library_tiles();
}
