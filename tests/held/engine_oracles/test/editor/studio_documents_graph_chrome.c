#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
/* Graph chrome (workstream G1) in Studio: the canvas's context menus
 * (GraphMenu.rml over the `graph_menu` source), each item's action one Undo
 * step, written once and run by key. */

#include "editor/studio/studio_graph_internal.h"
#include "graph/model/registry.h"
#include "support/array.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/popover.h"

#include <SDL3/SDL_keyboard.h>
#include <stdio.h>
#include <string.h>

/* The Vault with Teller open in the Behavior area below the Scene and
   Properties beside them. */
static studio_fixture graph_open()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_SCENE;
    layout->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_BEHAVIOR;
    layout->editors[STUDIO_AREA_PROPERTIES] = STUDIO_EDITOR_PROPERTIES;
    frames(&f, 6);
    return f;
}

static bool shows_text(rml_element *el, const char *expected)
{
    char text[256] = "";
    if (el) {
        rml_element_inner_rml(el, text, sizeof text);
    }
    return el && !strcmp(text, expected);
}

/* The items of `menu` shown, data-when's templates left out. */
static int shown_items(rml_element *menu, const char *selector, rml_element **out, int max)
{
    rml_element *all[64];
    int count = menu ? rml_element_query_selector_all(menu, selector, all, 64) : 0,
        shown = 0;
    for (int i = 0; i < count && i < 64; i++) {
        if (rml_element_is_visible(all[i], true) && shown < max) {
            out[shown++] = all[i];
        }
    }
    return shown;
}

/* The item of `menu` whose label is `label`, or NULL. */
static rml_element *item_labeled(rml_element *menu, const char *label)
{
    rml_element *items[64];
    int count = shown_items(menu, ".menu-item", items, 64);
    for (int i = 0; i < count; i++) {
        if (shows_text(rml_element_query_selector(items[i], ".label"), label)) {
            return items[i];
        }
    }
    return NULL;
}

static bool disabled(rml_element *item)
{
    return item && rml_element_has_attribute(item, "data-disabled");
}

/* Opens the canvas menu on a node, one of its pins, a comment frame or a
   wire, at a point over the graph, as a right click there does; the open
   menu or NULL. */
static rml_element *open_menu(studio_fixture *f, uint64_t node, uint64_t pin, uint64_t comment, int edge)
{
    studio_graph *g = &f->studio->graph;
    g->menu_node = node;
    g->menu_pin = pin;
    g->menu_comment = comment;
    g->menu_edge = edge;
    g->menu_x = g->surface.rect.x + 80;
    g->menu_y = g->surface.rect.y + 40;
    g->menu_request = true;
    frames(f, 3);
    rml_element *menu = studio_element(f->studio, "#graph-menu");
    return menu && popover_open(menu) ? menu : NULL;
}

/* Clicks a submenu's row, which opens it beside the row; its menu. */
static rml_element *open_submenu(studio_fixture *f, rml_element *row)
{
    if (!row) {
        return NULL;
    }
    press(f->studio, row);
    frames(f, 2);
    rml_element *sub = rml_element_parent(row);
    sub = sub ? rml_element_query_selector(sub, ".popup.menu") : NULL;
    return sub && popover_open(sub) ? sub : NULL;
}

static bool menu_closed(studio_fixture *f)
{
    frames(f, 2);
    return !f->studio->graph.menu_open && !studio_element(f->studio, "#graph-menu");
}

/* The node menu names its node, lists its actions with their keys and
   opens Select beside its row; Disable runs as one Undo step and closes
   the menu, and Escape closes it. */
static void node_menu()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    node *branch = editor_spawn(f.graph, registry_node_by_name("Branch"), 120, 80);
    uint64_t id = branch->id;
    frames(&f, 3);
    int undo = acount(f.graph->undo);
    rml_element *menu = open_menu(&f, id, 0, 0, -1);
    rml_element *heading = menu ? rml_element_query_selector(menu, ".menu-group")
                                : NULL;
    rml_element *remove = item_labeled(menu, "Delete");
    bool shown = menu && shows_text(heading, "Branch") &&
                 item_labeled(menu, "Duplicate") && remove &&
                 shows_text(rml_element_query_selector(remove, ".kbd"), "X");
    rml_element *select = open_submenu(&f, item_labeled(menu, "Select"));
    rml_element *items[8];
    int linked = shown_items(select, ".menu-item", items, 8);
    bool submenu = select && linked == 4 && item_labeled(select, "All Output Nodes");
    tap(SDL_SCANCODE_ESCAPE);
    tap(SDL_SCANCODE_ESCAPE);
    bool escaped = menu_closed(&f);
    menu = open_menu(&f, id, 0, 0, -1);
    press(s, item_labeled(menu, "Disable"));
    bool closed = menu_closed(&f);
    branch = graph_node_by_id(sg_active_graph(s), id);
    bool ran = branch && branch->disabled && acount(f.graph->undo) == undo + 1;
    menu = open_menu(&f, id, 0, 0, -1);
    bool enable = item_labeled(menu, "Enable") != NULL;
    if (!(shown && submenu && escaped && ran && closed && enable)) {
        printf(
            "  node menu: shown %d submenu %d (%d items) escaped %d ran %d "
            "closed %d enable %d\n",
            shown,
            submenu,
            linked,
            escaped,
            ran,
            closed,
            enable
        );
    }
    test_check(
        shown && submenu && escaped && ran && closed && enable,
        "the graph's node menu names the node, shows its actions' keys, opens "
        "Select beside its row, closes on Escape and runs Disable as one Undo "
        "step, closing"
    );
    fixture_free(&f);
}

/* A pin's menu: Break Link stays disabled without a wire and its click does
   nothing; Reset to Default puts a changed input back as one Undo step. */
static void pin_menu()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    node *branch = editor_spawn(f.graph, registry_node_by_name("Branch"), 120, 80);
    uint64_t id = branch->id, condition = branch->input_pins[1].id;
    pin_value on = { .type = PIN_BOOL, .b = true };
    assert(graph_set_pin_default(sg_active_graph(s), branch, 0, 1, &on) == 0);
    frames(&f, 3);
    int undo = acount(f.graph->undo);
    rml_element *menu = open_menu(&f, id, condition, 0, -1);
    rml_element *heading = menu ? rml_element_query_selector(menu, ".menu-group")
                                : NULL;
    rml_element *unlink = item_labeled(menu, "Break Link");
    bool shown = menu && shows_text(heading, "Condition") && disabled(unlink) &&
                 item_labeled(menu, "Reset to Default") &&
                 !disabled(item_labeled(menu, "Reset to Default"));
    press(s, unlink);
    frames(&f, 2);
    bool inert = popover_open(menu) && acount(f.graph->undo) == undo;
    press(s, item_labeled(menu, "Reset to Default"));
    bool closed = menu_closed(&f);
    branch = graph_node_by_id(sg_active_graph(s), id);
    bool reset = branch && !branch->input_pins[1].default_value.b &&
                 acount(f.graph->undo) == undo + 1;
    test_check(
        shown && inert && reset && closed,
        "a pin's menu names the pin, keeps Break Link disabled without a wire "
        "and runs Reset to Default as one Undo step"
    );
    fixture_free(&f);
}

/* A wire's menu deletes it; a comment frame's Color submenu shows each
   color's dot, the frame's own current, and sets another as one step. */
static void wire_and_comment_menus()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    graph *gr = sg_active_graph(s);
    node *add = editor_spawn(f.graph, registry_node_by_name("Add"), 80, 80);
    node *atan = editor_spawn(f.graph, registry_node_by_name("Atan2"), 360, 80);
    assert(graph_connect_data(gr, add, 0, atan, 0) == 0);
    graph_comment_box *box = graph_add_comment_box(gr, "Aim", 40, 260, 240, 120);
    uint64_t comment = box->id;
    frames(&f, 3);
    int undo = acount(f.graph->undo), edges = acount(gr->edges);
    rml_element *menu = open_menu(&f, 0, 0, 0, 0);
    bool wire = item_labeled(menu, "Straighten Connection") &&
                item_labeled(menu, "Delete Wire");
    press(s, item_labeled(menu, "Delete Wire"));
    bool deleted = menu_closed(&f) && acount(gr->edges) == edges - 1 &&
                   acount(f.graph->undo) == undo + 1;
    menu = open_menu(&f, 0, 0, comment, -1);
    rml_element *colors = open_submenu(&f, item_labeled(menu, "Color"));
    rml_element *dots[16];
    int count = shown_items(colors, ".menu-item .lead", dots, 16);
    char tint[32] = "";
    rml_element *red = item_labeled(colors, "Red");
    rml_element *dot = red ? rml_element_query_selector(red, ".lead") : NULL;
    if (dot) {
        rml_element_property(dot, "color", tint, sizeof tint);
    }
    bool listed = count == 9 &&
                  rml_element_has_attribute(item_labeled(colors, "Default"), "data-current") &&
                  tint[0];
    press(s, red);
    bool colored = menu_closed(&f) && sg_comment_by_id(s, comment) &&
                   sg_comment_by_id(s, comment)->color == 0xe5534bffu &&
                   acount(f.graph->undo) == undo + 2;
    if (!(wire && deleted && listed && colored)) {
        printf(
            "  wire %d deleted %d listed %d (%d dots, tint %s) colored %d\n",
            wire,
            deleted,
            listed,
            count,
            tint,
            colored
        );
    }
    test_check(
        wire && deleted && listed && colored,
        "a wire's menu deletes it and a comment frame's Color submenu shows "
        "each color as a tinted dot, the frame's own current, each choice one "
        "Undo step"
    );
    fixture_free(&f);
}

/* ---------- Properties' Node and Member tabs ---------- */

static rml_element *shown_in(rml_element *root, const char *selector)
{
    rml_element *found = NULL;
    return shown_items(root, selector, &found, 1) ? found : NULL;
}

/* The control `selector` finds in a shown row of the graph section labeled
   `label`, or NULL. */
static rml_element *details_control(studio *s, const char *label, const char *selector)
{
    rml_element *rows[64];
    rml_element *section = studio_element(s, "[data-ui=Properties]");
    int count = shown_items(section, ".graph-row .prop", rows, 64);
    for (int i = 0; i < count; i++) {
        rml_element *control = shown_in(rows[i], selector);
        if (control &&
            shows_text(rml_element_query_selector(rows[i], ".label span"), label)) {
            return control;
        }
    }
    return NULL;
}

/* The graph section's shown row labeled `label`, or NULL. */
static rml_element *details_row(studio *s, const char *label)
{
    rml_element *rows[128];
    rml_element *section = studio_element(s, "[data-ui=Properties]");
    int count = shown_items(section, ".graph-row .prop", rows, 64);
    for (int i = 0; i < count; i++) {
        if (shows_text(rml_element_query_selector(rows[i], ".label span"), label)) {
            return rows[i];
        }
    }
    return NULL;
}

static bool identity_is(studio *s, const char *name)
{
    rml_element *section = studio_element(s, "[data-ui=Properties]");
    return shows_text(shown_in(section, ".identity .title"), name);
}

/* Scrolls `el` into view and presses it; nothing without one. */
static void press_in_view(studio_fixture *f, rml_element *el)
{
    if (!el) {
        return;
    }
    rml_element_scroll_into_view(el, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
    frames(f, 2);
    press(f->studio, el);
}

/* Types `text` into the field, replacing what it holds, and presses Enter. */
static void type_in(studio_fixture *f, rml_element *field, const char *text)
{
    if (!field) {
        return;
    }
    press_in_view(f, field);
    frames(f, 2);
    rml_element *input = rml_context_focus_element(rml_backend_context(f->studio->backend));
    if (input) {
        rml_element_select(input);
    }
    text_event(text);
    tap(SDL_SCANCODE_RETURN);
    frames(f, 3);
}

/* A vector input's axis edits as one Undo step; the Node panel's Disabled
   and Comment edit the node; selecting another node rebinds the rows to
   it. */
static void node_tab()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    node *real = editor_spawn(f.graph, registry_node_by_name("Direction To"), 120, 80);
    node *atan = editor_spawn(f.graph, registry_node_by_name("Atan2"), 360, 80);
    uint64_t real_id = real->id, atan_id = atan->id;
    sg_select_only(f.graph, real);
    frames(&f, 6);
    bool tab = s->tab == STUDIO_TAB_NODE && identity_is(s, "Direction To");
    int undo = acount(f.graph->undo);
    rml_element *field = details_control(s, "Target", ".field.number");
    type_in(&f, field, "3.5");
    if (!field) {
        printf("  node tab: no number field\n");
    }
    real = sg_node_by_id(s, real_id);
    int target = -1;
    for (int p = 0; real && p < real->input_count; p++) {
        target = !strcmp(real->input_pins[p].name, "Target") ? p : target;
    }
    const float *vector = real && target >= 0
                            ? real->input_pins[target].default_value.ptr
                            : NULL;
    double literal = vector ? vector[0] : 0;
    bool number = literal == 3.5 && acount(f.graph->undo) == undo + 1;
    press_in_view(&f, details_control(s, "Disabled", "input"));
    frames(&f, 3);
    real = sg_node_by_id(s, real_id);
    bool disabled = real && real->disabled && acount(f.graph->undo) == undo + 2;
    rml_element *comment = shown_in(details_row(s, "Comment"), "textarea");
    if (comment) {
        rml_element_dispatch_value(comment, "change", "Tuned", false);
    }
    frames(&f, 3);
    real = sg_node_by_id(s, real_id);
    bool commented = real && real->comment && !strcmp(real->comment, "Tuned") &&
                     details_row(s, "Show Comment");
    sg_select_only(f.graph, sg_node_by_id(s, atan_id));
    frames(&f, 4);
    bool rebound = identity_is(s, "Atan2") &&
                   shown_in(details_row(s, "Y"), ".source-field input") &&
                   details_row(s, "Disabled");
    if (!(tab && number && disabled && commented && rebound)) {
        printf(
            "  node tab: tab %d number %d (%g, %d steps) disabled %d commented "
            "%d rebound %d\n",
            tab,
            number,
            literal,
            acount(f.graph->undo) - undo,
            disabled,
            commented,
            rebound
        );
    }
    test_check(
        tab && number && disabled && commented && rebound,
        "the Node tab edits a literal as one Undo step, sets Disabled and the "
        "comment, and rebinds its rows to the next node selected"
    );
    fixture_free(&f);
}

/* A comment frame's tab: its title as one Undo step, and a color dragged
   then committed as one more. */
static void frame_tab()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    graph *gr = sg_active_graph(s);
    uint64_t id = graph_add_comment_box(gr, "Aim", 40, 260, 240, 120)->id;
    ed_clear_selection(f.graph);
    s->graph.comment = id;
    frames(&f, 6);
    bool tab = identity_is(s, "Aim");
    int undo = acount(f.graph->undo);
    type_in(&f, details_control(s, "Title", "input"), "Aim at");
    graph_comment_box *box = sg_comment_by_id(s, id);
    bool titled = box && !strcmp(box->title, "Aim at") &&
                  acount(f.graph->undo) == undo + 1;
    rml_element *color = details_control(s, "Color", "color-field");
    if (color) {
        rml_element_dispatch_value(color, "input", "#336699ff", false);
        frames(&f, 2);
        rml_element_dispatch_value(color, "input", "#e5534bff", false);
        frames(&f, 2);
        rml_element_dispatch_value(color, "change", "#e5534bff", false);
    }
    frames(&f, 3);
    box = sg_comment_by_id(s, id);
    bool colored = box && box->color == 0xe5534bffu &&
                   acount(f.graph->undo) == undo + 2;
    if (!(tab && titled && colored)) {
        printf(
            "  frame tab: tab %d titled %d color %d colored %d (%d steps)\n",
            tab,
            titled,
            color != NULL,
            colored,
            acount(f.graph->undo) - undo
        );
    }
    test_check(
        tab && titled && colored,
        "a comment frame's tab sets its title and, as one Undo step, a color "
        "dragged then committed"
    );
    fixture_free(&f);
}

/* An input's source field lists what fits under it as it takes focus, the
   current value highlighted; a typed literal is offered first and Enter
   sets it as one Undo step; Escape shuts the list and keeps the source. */
static void source_field()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    node *atan = editor_spawn(f.graph, registry_node_by_name("Atan2"), 360, 80);
    uint64_t id = atan->id;
    sg_select_only(f.graph, atan);
    frames(&f, 6);
    rml_element *input = shown_in(details_row(s, "Y"), ".source-field input");
    press_in_view(&f, input);
    frames(&f, 4);
    rml_element *list = shown_in(details_row(s, "Y"), ".popup.autocomplete");
    rml_element *rows[64];
    int listed = shown_items(list, ".menu-item", rows, 64);
    bool opened = list && popover_open(list) && listed > 0;
    int undo = acount(f.graph->undo);
    rml_element_select(input);
    text_event("2.5");
    frames(&f, 4);
    rml_element *lit = shown_in(list, ".menu-item[data-highlighted] .label");
    char label[128] = "";
    if (lit) {
        rml_element_inner_rml(lit, label, sizeof label);
    }
    bool offered = strstr(label, "2.5") != NULL;
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    atan = sg_node_by_id(s, id);
    double y = atan ? sg_literal_number(&atan->input_pins[0].default_value) : 0;
    bool set = y == 2.5 && acount(f.graph->undo) == undo + 1 && !popover_open(list);
    if (!set) {
        printf("  source set: y %g steps %d open %d\n", y, acount(f.graph->undo) - undo, popover_open(list));
    }
    /* Typed text stays in the field as the list follows it. */
    input = shown_in(details_row(s, "Y"), ".source-field input");
    press_in_view(&f, input);
    frames(&f, 3);
    char typed[64] = "";
    for (const char *c = "zz"; *c; c++) {
        text_event((char[]){ *c, 0 });
        frames(&f, 2);
    }
    if (input) {
        rml_element_value(input, typed, sizeof typed);
    }
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 4);
    char shown[64] = "";
    if (input) {
        rml_element_value(input, shown, sizeof shown);
    }
    bool kept = strstr(typed, "zz") && !popover_open(list) &&
                !strcmp(shown, "2.5") && acount(f.graph->undo) == undo + 1;
    /* A tab with nothing changing evaluates none of its views. */
    frames(&f, 4);
    rml_data_reads before = studio_ui_data_reads(s);
    frames(&f, 6);
    rml_data_reads after = studio_ui_data_reads(s);
    bool idle = after.values == before.values;
    if (!(opened && offered && set && kept && idle)) {
        printf(
            "  source: opened %d (%d rows) offered %d ('%s') set %d kept %d "
            "('%s', typed '%s') idle %d (%llu views)\n",
            opened,
            listed,
            offered,
            label,
            set,
            kept,
            shown,
            typed,
            idle,
            (unsigned long long)(after.values - before.values)
        );
    }
    test_check(
        opened && offered && set && kept,
        "an input's source field lists what fits under it, offers a typed "
        "literal first and sets it with Enter as one Undo step; typed text "
        "stays as the list follows it, and Escape shuts the list and keeps the "
        "source"
    );
    test_check(idle, "the Node tab evaluates no view while nothing changes");
    fixture_free(&f);
}

/* The index of function `id`'s input named `name`, or -1. */
static int signature_at(editor *e, uint64_t id, const char *name)
{
    graph *g = behavior_asset_find_graph(e->asset, id);
    node *entry = g ? graph_node_by_id(g, g->entry_id) : NULL;
    for (int p = 0; entry && p < entry->output_count; p++) {
        if (entry->output_pins[p].name && !strcmp(entry->output_pins[p].name, name)) {
            return p;
        }
    }
    return -1;
}

/* Presses `from`, carries it past the drag threshold onto the middle of
   `to` and releases there. */
static void carry(studio_fixture *f, rml_element *from, rml_element *to)
{
    studio *s = f->studio;
    float ratio = studio_pixel_ratio(s);
    rml_vec2 at = rml_element_absolute_offset(from, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(from, RML_BOX_BORDER);
    float x = (at.x + size.x * 0.5f) / ratio, y = (at.y + size.y * 0.5f) / ratio;
    at = rml_element_absolute_offset(to, RML_BOX_BORDER);
    size = rml_element_box_size(to, RML_BOX_BORDER);
    float tx = (at.x + size.x * 0.5f) / ratio, ty = (at.y + size.y * 0.5f) / ratio;
    mouse_move(s, x, y);
    frames(f, 1);
    graph_drag_button(s, x, y, true);
    frames(f, 1);
    mouse_move(s, x + 12, y);
    frames(f, 2);
    mouse_move(s, tx, ty);
    frames(f, 3);
    graph_drag_button(s, tx, ty, false);
    frames(f, 4);
}

/* The Member tab: a variable's name, type (the palette's picker), container
   and default, each one Undo step; a function's Add Input, a pin's rename
   and its remove. */
static void member_tab()
{
    studio_fixture f = graph_open();
    studio *s = f.studio;
    behavior_asset *asset = f.graph->asset;
    behavior_asset_add_variable(asset, "Speed", PIN_F32, "float", PIN_CONTAINER_SINGLE, NULL, NULL);
    behavior_asset_add_function(asset, "Aim", 0);
    frames(&f, 2);
    behavior_variable *speed = behavior_asset_find_variable_by_name(asset, "Speed");
    uint64_t speed_id = speed->id;
    s->members.kind = STUDIO_MEMBER_VARIABLE;
    s->members.id = speed_id;
    frames(&f, 6);
    bool tab = s->tab == STUDIO_TAB_MEMBER && identity_is(s, "Speed");
    int undo = acount(f.graph->undo);
    type_in(&f, details_control(s, "Name", "input"), "Velocity");
    speed = behavior_asset_find_variable_by_id(f.graph->asset, speed_id);
    bool renamed = speed && !strcmp(speed->name, "Velocity") &&
                   acount(f.graph->undo) == undo + 1;
    type_in(&f, details_control(s, "Value", ".field.number"), "7");
    speed = behavior_asset_find_variable_by_id(f.graph->asset, speed_id);
    bool defaulted = speed && speed->default_value.f == 7.0f &&
                     acount(f.graph->undo) == undo + 2;
    press_in_view(&f, details_control(s, "Type", ".type-button"));
    frames(&f, 4);
    bool picking = s->palette.client == STUDIO_PALETTE_TYPES;
    text_event("Integer");
    frames(&f, 3);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    speed = behavior_asset_find_variable_by_id(f.graph->asset, speed_id);
    bool retyped = picking && speed && speed->type == PIN_I32 &&
                   !s->palette.client && acount(f.graph->undo) == undo + 3;
    graph *aim = NULL;
    for (int i = 0; i < acount(f.graph->asset->graphs); i++) {
        aim = !strcmp(f.graph->asset->graphs[i]->name, "Aim")
                ? f.graph->asset->graphs[i]
                : aim;
    }
    uint64_t aim_id = aim->graph_id;
    s->members.kind = STUDIO_MEMBER_GRAPH;
    s->members.id = aim_id;
    frames(&f, 6);
    rml_element *section = studio_element(s, "[data-ui=Properties]");
    rml_element *adds[4];
    int buttons = shown_items(section, ".add-row .btn", adds, 4);
    press_in_view(&f, buttons ? adds[0] : NULL);
    frames(&f, 4);
    press_in_view(&f, buttons ? shown_in(section, ".add-row .btn") : NULL);
    frames(&f, 4);
    rml_element *params[4];
    int listed = shown_items(section, ".param", params, 4);
    bool added = signature_at(f.graph, aim_id, "Value") >= 0 &&
                 signature_at(f.graph, aim_id, "Value 2") >= 0 && listed == 2;
    /* The second input's grip dropped on the first's row moves it there. */
    undo = acount(f.graph->undo);
    if (listed == 2) {
        carry(&f, shown_in(params[1], ".param-grip"), params[0]);
    }
    bool reordered = signature_at(f.graph, aim_id, "Value 2") >= 0 &&
                     signature_at(f.graph, aim_id, "Value 2") <
                         signature_at(f.graph, aim_id, "Value") &&
                     acount(f.graph->undo) == undo + 1;
    rml_element *param = shown_in(section, ".param");
    type_in(&f, shown_in(param, ".param-name input"), "Range");
    bool pin_named = signature_at(f.graph, aim_id, "Range") >= 0 &&
                     signature_at(f.graph, aim_id, "Value 2") < 0;
    press_in_view(&f, shown_in(shown_in(section, ".param"), ".param-name .btn"));
    frames(&f, 4);
    bool removed = signature_at(f.graph, aim_id, "Range") < 0 &&
                   shown_items(section, ".param", params, 4) == 1;
    if (!(tab && renamed && defaulted && retyped && added && reordered &&
          pin_named && removed)) {
        printf(
            "  member tab: tab %d renamed %d defaulted %d picking %d retyped "
            "%d added %d (%d buttons, %d rows) reordered %d named %d removed "
            "%d\n",
            tab,
            renamed,
            defaulted,
            picking,
            retyped,
            added,
            buttons,
            listed,
            reordered,
            pin_named,
            removed
        );
    }
    test_check(
        tab && renamed && defaulted && retyped,
        "the Member tab renames a variable, sets its default and its type "
        "from the palette's picker, each one Undo step"
    );
    test_check(
        added && reordered && pin_named && removed,
        "a function's Add Input adds pins whose rows reorder by their grips "
        "as one Undo step, rename and remove them"
    );
    fixture_free(&f);
}

void studio_documents_graph_chrome_checks()
{
    node_menu();
    pin_menu();
    wire_and_comment_menus();
    node_tab();
    frame_tab();
    source_field();
    member_tab();
}
