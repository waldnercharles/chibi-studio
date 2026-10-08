#include "graph/authoring/asset_edit.h"
/* Members (workstream P5) in Studio: the authored panel (Members.rml) over
 * the open behavior's sections, its rename in place, menu, keys, drag onto
 * the graph canvas, reveal and filter, with the `members` source keeping
 * selection, edits and history in C. */

#include "editor/studio/studio_members_internal.h"
#include "render/screenshot.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/popover.h"

#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Vault with Teller open, Teller holding variables and a function, and
   Members in the Properties area beside the Scene and the graph. */
static studio_fixture members_open()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    behavior_asset *asset = f.graph->asset;
    pin_value hull = { .type = PIN_F32, .f = 100 };
    behavior_asset_add_variable(asset, "Speed", PIN_F32, "float", PIN_CONTAINER_SINGLE, NULL, NULL);
    behavior_asset_add_variable(asset, "Alive", PIN_BOOL, "bool", PIN_CONTAINER_SINGLE, NULL, NULL);
    behavior_asset_add_variable(asset, "Hull Max", PIN_F32, "float", PIN_CONTAINER_SINGLE, &hull, "Combat");
    behavior_asset_add_function(asset, "Aim", 0);
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_SCENE;
    layout->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_BEHAVIOR;
    layout->editors[STUDIO_AREA_PROPERTIES] = STUDIO_EDITOR_MEMBERS;
    frames(&f, 6);
    return f;
}

/* Members' shown row whose name is `name`. */
static rml_element *member_row(studio *s, const char *name)
{
    rml_element *rows[128];
    int count = test_query_all(
        rml_backend_context(s->backend),
        "#members-list .member-row .tree-row",
        rows,
        128
    );
    for (int i = 0; i < count && i < 128; i++) {
        rml_element *label = rml_element_query_selector(rows[i], ".name");
        char text[128] = "";
        if (label) {
            rml_element_inner_rml(label, text, sizeof text);
        }
        if (!strcmp(text, name) && rml_element_is_visible(rows[i], true)) {
            return rows[i];
        }
    }
    return NULL;
}

static bool row_text(rml_element *row, const char *selector, const char *text)
{
    rml_element *part = row ? rml_element_query_selector(row, selector) : NULL;
    char shown[128] = "";
    if (part) {
        rml_element_inner_rml(part, shown, sizeof shown);
    }
    return part && !strcmp(shown, text);
}

static int variable_count(studio_fixture *f)
{
    return acount(f->graph->asset->variables);
}

static void mouse_over_members(studio_fixture *f)
{
    kit_rect r = f->studio->areas[STUDIO_AREA_PROPERTIES].rect;
    mouse_move(f->studio, r.x + r.w * 0.5f, r.y + r.h - 12);
    frames(f, 1);
}

/* A right press on `el`, which opens a context menu. */
static void right_press(studio *s, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    rml_input input = { .type = RML_INPUT_POINTER_MOVE,
                        .x = at.x / ratio + 30,
                        .y = at.y / ratio + 6,
                        .button = 1 };
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        input.type = types[i];
        rml_backend_process_input(s->backend, &input);
    }
}

/* Presses `from`, carries it past the drag threshold to the middle of
   `to` and holds it there; returns where it is held, in dp. */
static void carry_to(studio_fixture *f, rml_element *from, rml_element *to, float *x, float *y)
{
    studio *s = f->studio;
    float ratio = studio_pixel_ratio(s);
    rml_vec2 start = rml_element_absolute_offset(from, RML_BOX_BORDER);
    rml_vec2 at = rml_element_absolute_offset(to, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(to, RML_BOX_BORDER);
    float sx = start.x / ratio + 30, sy = start.y / ratio + 6;
    *x = (at.x + size.x * 0.5f) / ratio;
    *y = (at.y + size.y * 0.5f) / ratio;
    mouse_move(s, sx, sy);
    frames(f, 1);
    graph_drag_button(s, sx, sy, true);
    frames(f, 1);
    mouse_move(s, sx - 12, sy);
    frames(f, 2);
    mouse_move(s, *x, *y);
    frames(f, 4);
}

/* The element with focus, and whether it is the rename field. */
static bool renaming_field(studio *s)
{
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    rml_element *field = studio_element(s, ".member-rename input");
    return focus && focus == field;
}

/* Sections show their counts, members their type, swatch and uses; a
   press selects for Properties and a section's press folds it. */
static void rows_and_selection()
{
    studio_fixture f = members_open();
    studio *s = f.studio;
    rml_element *variables = member_row(s, "Variables");
    rml_element *speed = member_row(s, "Speed");
    rml_element *combat = member_row(s, "Combat");
    bool sections = variables && row_text(variables, ".meta", "3") &&
                    rml_element_has_attribute(rml_element_parent(variables), "data-section") &&
                    member_row(s, "Functions") && member_row(s, "Graphs") &&
                    member_row(s, "Aim") && member_row(s, "Hull Max");
    rml_element *swatch = speed ? rml_element_query_selector(speed, "kit-dot.member-swatch")
                                : NULL;
    char shape[16] = "";
    if (swatch) {
        rml_element_attribute(swatch, "shape", shape, sizeof shape);
    }
    rml_element *uses = speed ? rml_element_query_selector(speed, ".member-uses")
                              : NULL;
    bool look = speed && row_text(speed, ".meta", "Float") &&
                !strcmp(shape, "circle") && combat &&
                row_text(combat, ".meta", "1") && uses &&
                rml_element_is_visible(uses, true) &&
                rml_element_has_attribute(uses, "data-unused") &&
                row_text(uses, ".label", "0");
    if (speed) {
        press(s, speed);
    }
    frames(&f, 2);
    behavior_variable *v = sm_variable(s);
    rml_element *selected = member_row(s, "Speed");
    bool picked = v && !strcmp(v->name, "Speed") && selected &&
                  rml_element_has_attribute(selected, "data-selected") &&
                  rml_element_has_attribute(selected, "data-active");
    press(s, member_row(s, "Variables"));
    frames(&f, 2);
    bool folded = !member_row(s, "Speed") && member_row(s, "Variables");
    press(s, member_row(s, "Variables"));
    frames(&f, 2);
    bool unfolded = member_row(s, "Speed") != NULL;
    /* The use count lists uses in Find; its row's own press waits. */
    rml_element *aim = member_row(s, "Aim");
    rml_element *aim_uses = aim ? rml_element_query_selector(aim, ".member-uses")
                                : NULL;
    if (aim_uses) {
        press(s, aim_uses);
    }
    frames(&f, 2);
    bool listed = aim_uses && !strcmp(s->references_title, "Aim") &&
                  sm_variable(s) && !strcmp(sm_variable(s)->name, "Speed");
    if (!(sections && look && picked && folded && unfolded && listed)) {
        printf(
            "  members rows: sections %d look %d (shape '%s') picked %d folded "
            "%d %d listed %d ('%s')\n",
            sections,
            look,
            shape,
            picked,
            folded,
            unfolded,
            listed,
            s->references_title
        );
    }
    test_check(
        sections && look,
        "Members shows each section's count and add button, and each "
        "variable's type, swatch and use count, as authored rows"
    );
    test_check(
        picked && folded && unfolded,
        "a press on a member selects it for Properties, and one on a "
        "section folds and unfolds it"
    );
    test_check(
        listed,
        "a member's use count lists its uses in Find, and its row keeps "
        "the press (stop() in the count's click)"
    );
    fixture_free(&f);
}

/* F2 puts a focused name field in the row's place: Enter renames as one
   Undo step and Escape keeps the name. */
static void rename_in_place()
{
    studio_fixture f = members_open();
    studio *s = f.studio;
    press(s, member_row(s, "Speed"));
    frames(&f, 2);
    uint64_t id = s->members.id;
    int steps = acount(f.graph->undo);
    mouse_over_members(&f);
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    bool field = renaming_field(s);
    text_event("Velocity");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    behavior_variable *v = behavior_asset_find_variable_by_id(f.graph->asset, id);
    bool renamed = v && !strcmp(v->name, "Velocity") &&
                   acount(f.graph->undo) == steps + 1 &&
                   !s->members.rename_kind && member_row(s, "Velocity") &&
                   !studio_element(s, ".member-rename input");
    tap(SDL_SCANCODE_F2);
    frames(&f, 3);
    bool again = renaming_field(s);
    text_event("Nope");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    v = behavior_asset_find_variable_by_id(f.graph->asset, id);
    bool kept = v && !strcmp(v->name, "Velocity") && !s->members.rename_kind &&
                acount(f.graph->undo) == steps + 1;
    /* A double click renames too, and the new function's add button names
       it at once. */
    rml_element *functions = member_row(s, "Functions");
    rml_element *add = functions ? rml_element_query_selector(functions, ".toggle")
                                 : NULL;
    int graphs = acount(f.graph->asset->graphs);
    if (add) {
        press(s, add);
    }
    frames(&f, 3);
    bool added = acount(f.graph->asset->graphs) == graphs + 1 &&
                 renaming_field(s) && member_row(s, "Functions");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    if (!(field && renamed && again && kept && added)) {
        printf(
            "  members rename: field %d renamed %d again %d kept %d added %d "
            "(%d steps)\n",
            field,
            renamed,
            again,
            kept,
            added,
            acount(f.graph->undo) - steps
        );
    }
    test_check(
        field && renamed && again && kept,
        "F2 over Members puts a focused name field in the row's place: "
        "Enter renames as one Undo step and Escape keeps the name"
    );
    test_check(
        added,
        "a section's add button adds a function and opens its name field "
        "without folding the section"
    );
    fixture_free(&f);
}

/* A right press selects and opens the member's menu at the pointer;
   Duplicate and Delete run, and X deletes with an Undo toast. */
static void menu_and_keys()
{
    studio_fixture f = members_open();
    studio *s = f.studio;
    int count = variable_count(&f);
    right_press(s, member_row(s, "Speed"));
    frames(&f, 3);
    rml_element *menu = studio_element(s, "#member-menu");
    bool open = menu && popover_open(menu) && sm_variable(s) &&
                !strcmp(sm_variable(s)->name, "Speed");
    rml_element *items[8];
    int shown = test_query_all(rml_backend_context(s->backend), "#member-menu .menu-item", items, 8);
    rml_element *duplicate = NULL;
    for (int i = 0; i < shown && i < 8; i++) {
        if (row_text(items[i], ".label", "Duplicate") &&
            rml_element_is_visible(items[i], true)) {
            duplicate = items[i];
        }
    }
    if (duplicate) {
        press(s, duplicate);
    }
    frames(&f, 3);
    bool duplicated = variable_count(&f) == count + 1 &&
                      member_row(s, "Speed 2") && !popover_open(menu);
    mouse_over_members(&f);
    tap(SDL_SCANCODE_X);
    frames(&f, 3);
    bool deleted = variable_count(&f) == count && !member_row(s, "Speed 2") &&
                   s->members.undo.toast;
    studio_graph_history(s, false);
    frames(&f, 3);
    bool undone = variable_count(&f) == count + 1 && member_row(s, "Speed 2");
    if (!(open && duplicated && deleted && undone)) {
        printf("  members menu: open %d duplicated %d deleted %d undone %d\n", open, duplicated, deleted, undone);
    }
    test_check(
        open && duplicated,
        "a right press on a member selects it and opens its menu at the "
        "pointer, whose Duplicate runs"
    );
    test_check(deleted && undone, "X over Members deletes the selected member and offers Undo");
    fixture_free(&f);
}

/* A variable carried onto the graph canvas, a C surface in another area,
   places its Get there, its hint saying so and naming Alt for its Set. */
static void drag_into_graph()
{
    studio_fixture f = members_open();
    studio *s = f.studio;
    graph *gr = f.graph->active_graph;
    int nodes = acount(gr->nodes);
    rml_element *speed = member_row(s, "Speed");
    rml_element *canvas = studio_element(s, "img[src=kit:graph]");
    bool over = false;
    char hint[64] = "";
    if (speed && canvas) {
        float x, y;
        carry_to(&f, speed, canvas, &x, &y);
        over = rml_element_has_attribute(canvas, "data-drop-over");
        rml_element *shown = studio_element(s, ".drag-overlay .hint");
        if (shown) {
            rml_element_inner_rml(shown, hint, sizeof hint);
        }
        graph_drag_button(s, x, y, false);
        frames(&f, 3);
    }
    const node *placed = acount(gr->nodes) == nodes + 1 ? alast(gr->nodes) : NULL;
    bool get = placed && placed->reference_name &&
               !strcmp(placed->reference_name, "Speed") &&
               strstr(placed->desc->name, "Get");
    if (!(over && get && !strcmp(hint, "Get (Alt: Set)"))) {
        printf(
            "  members drag: row %d canvas %d over %d hint '%s' nodes %d\n",
            speed != NULL,
            canvas != NULL,
            over,
            hint,
            acount(gr->nodes) - nodes
        );
    }
    test_check(
        over && get && !strcmp(hint, "Get (Alt: Set)"),
        "a variable dragged from Members onto the graph canvas places its "
        "Get there, its hint naming Get and Alt for Set"
    );
    fixture_free(&f);
}

/* Go to Definition's reveal scrolls a far member into the List's window
   and view; the header's filter narrows the rows. */
static void reveal_and_filter()
{
    studio_fixture f = members_open();
    studio *s = f.studio;
    behavior_asset *asset = f.graph->asset;
    for (int i = 0; i < 80; i++) {
        char name[32];
        snprintf(name, sizeof name, "Extra %02d", i);
        behavior_asset_add_variable(asset, name, PIN_I32, "int", PIN_CONTAINER_SINGLE, NULL, NULL);
    }
    frames(&f, 2);
    bool hidden = member_row(s, "Extra 79") == NULL;
    studio_members_reveal(s, STUDIO_MEMBER_VARIABLE, alast(asset->variables).id);
    frames(&f, 4);
    rml_element *row = member_row(s, "Extra 79");
    rml_element *list = studio_element(s, "#members-list");
    bool shown = false;
    float middle = 0, view = 0, height = 0;
    if (row && list) {
        middle = rml_element_absolute_offset(row, RML_BOX_BORDER).y +
                 rml_element_box_size(row, RML_BOX_BORDER).y * 0.5f;
        view = rml_element_absolute_offset(list, RML_BOX_BORDER).y;
        height = rml_element_client_height(list);
        shown = middle >= view && middle <= view + height;
    }
    bool selected = row && rml_element_has_attribute(row, "data-selected");
    rml_element *filter = studio_element(s, "#members-filter");
    if (filter) {
        press(s, filter);
    }
    frames(&f, 1);
    text_event("H");
    frames(&f, 2);
    /* A preference a command changed while Settings is closed outlasts the
       filter's next binding write. */
    bool comfortable = !s->prefs.comfortable;
    s->prefs.comfortable = comfortable;
    frames(&f, 2);
    text_event("ull");
    frames(&f, 3);
    bool filtered = !strcmp(s->members.filter, "Hull") &&
                    member_row(s, "Hull Max") && !member_row(s, "Speed") &&
                    !member_row(s, "Extra 00") && s->prefs.comfortable == comfortable;
    s->prefs.comfortable = !comfortable;
    frames(&f, 1);
    if (!(hidden && shown && selected && filtered)) {
        printf(
            "  members reveal: hidden %d shown %d (row at %.0f, view "
            "%.0f-%.0f) "
            "selected %d filtered %d ('%s')\n",
            hidden,
            shown,
            middle,
            view,
            view + height,
            selected,
            filtered,
            s->members.filter
        );
    }
    test_check(hidden && shown && selected, "revealing a member scrolls its row into Members' List and selects it");
    test_check(
        filtered,
        "typing in Members' header filter narrows its rows, and its binding "
        "writes leave alone a preference set while Settings is closed"
    );
    fixture_free(&f);
}

/* With CHIBI_MEMBERS_CAPTURE_DIR set, native shots of Members in dark and
   light, compact and comfortable: rows with a selection, the name field,
   the menu and a drag held over the graph. */
static void capture(studio_fixture *f, const char *dir, const char *variant, const char *state)
{
    char path[PATH_MAX];
    frames(f, 8);
    snprintf(path, sizeof path, "%s/native-%s-%s.png", dir, variant, state);
    assert(app_save_screenshot(path) == 0);
}

static void capture_members(const char *dir)
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
        studio_fixture f = members_open();
        studio *s = f.studio;
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        frames(&f, 4);
        press(s, member_row(s, "Speed"));
        capture(&f, dir, VARIANTS[v].name, "rows");
        mouse_over_members(&f);
        tap(SDL_SCANCODE_F2);
        capture(&f, dir, VARIANTS[v].name, "rename");
        tap(SDL_SCANCODE_ESCAPE);
        frames(&f, 2);
        right_press(s, member_row(s, "Speed"));
        capture(&f, dir, VARIANTS[v].name, "menu");
        tap(SDL_SCANCODE_ESCAPE);
        frames(&f, 2);
        float x, y;
        carry_to(&f, member_row(s, "Alive"), studio_element(s, "img[src=kit:graph]"), &x, &y);
        capture(&f, dir, VARIANTS[v].name, "drag");
        tap(SDL_SCANCODE_ESCAPE);
        graph_drag_button(s, x, y, false);
        frames(&f, 2);
        fixture_free(&f);
    }
}

void studio_documents_members_checks()
{
    const char *dir = getenv("CHIBI_MEMBERS_CAPTURE_DIR");
    if (dir && dir[0]) {
        capture_members(dir);
    }
    rows_and_selection();
    rename_in_place();
    menu_and_keys();
    drag_into_graph();
    reveal_and_filter();
}
