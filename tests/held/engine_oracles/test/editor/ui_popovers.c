/* Popovers (src/ui/popover.h) in a bare RmlUi context, as games have them,
 * through projects/studio's menu widgets: a menu opened from one document
 * draws above another whichever has focus; Escape and an outside press
 * close it and return focus; typeahead, a submenu's 100 ms delay, Left and
 * Right; a menubar switching menus on hover; a popover's focus; a color
 * picker escaping a short area; ChoiceList filtering and keys; and
 * EnumField listing every value of every reflected choice field. */
#include "assets/ui_document.h"
#include "editor/documents/component_properties.h"
#include "editor/studio/studio_internal.h"
#include "authoring/ui_check.h"
#include "support/array.h"
#include "support/text_file.h"
#include "test/suite_internal.h"
#include "ui/popover.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SECONDS (1.0 / 60.0)

static const char STYLE[] =
    "body { font-family: Inter; font-size: 13dp; line-height: 18dp; "
    "--menu-row-h: 22px; --faint-foreground: #888888; "
    "position: absolute; top: 0px; height: 480px; overflow: hidden; }"
    "body.a { left: 0px; width: 200px; height: 400px; } body.b { left: 200px; "
    "width: 440px; }"
    "body.short { left: 0px; top: 400px; width: 200px; height: 80px; }"
    ".popup { position: absolute; display: block; width: 160px; padding: 4px; "
    "background-color: #222222; z-index: 20; }"
    "[popover]:not([data-open]) { display: none; }"
    "[popover][data-unplaced] { visibility: hidden; }"
    ".menu-item { display: flex; height: 22px; } .menu-group, .menu-sep { "
    "display: block; height: 8px; }"
    "button { display: block; width: 60px; height: 22px; tab-index: auto; }"
    "menubar { display: flex; } input { display: block; width: 100px; "
    "height: 20px; }"
    "#trigger { position: absolute; left: 140px; top: 10px; }"
    ".popup { opacity: 1; transition: opacity 0.1s cubic-out; }"
    ".popup[data-starting-style], .popup[data-ending-style] { opacity: 0; }"
    ".choice-list [data-filtered] { display: none; }"
    "color-field { display: block; width: 40px; height: 20px; }"
    ".picker { width: 200px; height: 220px; }";

/* Area A: a menu by its right edge, a menubar, a popover with a field and
   an EnumField. */
static const char AREA_A[] =
    "<button id=\"trigger\" popovertarget=\"menu\">Edit</button>"
    "<use ui=\"Menu\" id=\"menu\">"
    "<use ui=\"MenuItem\" data-for=\"entry : items\" data-alias-item=\"entry\" "
    "data-event-click=\"ran = ran + 1; last = entry.label\"/>"
    "<use ui=\"MenuSeparator\"/>"
    "<use ui=\"MenuCheck\" data-checked=\"checked\">Check</use>"
    "<use ui=\"Submenu\" data-alias-item=\"sub\">"
    "<use ui=\"MenuItem\" data-for=\"entry : subitems\" "
    "data-alias-item=\"entry\" "
    "data-event-click=\"ran = ran + 1; last = entry.label\"/>"
    "</use>"
    "</use>"
    "<menubar><button id=\"file\" popovertarget=\"file-menu\">File</button>"
    "<button id=\"view\" popovertarget=\"view-menu\">View</button></menubar>"
    "<use ui=\"Menu\" id=\"file-menu\"><use ui=\"MenuItem\">New</use></use>"
    "<use ui=\"Menu\" id=\"view-menu\"><use ui=\"MenuItem\">Zoom</use></use>"
    "<button id=\"note-trigger\" popovertarget=\"note\">Note</button>"
    "<div id=\"note\" popover class=\"popup\"><input id=\"note-field\" "
    "type=\"text\"/><button id=\"note-ok\">OK</button></div>"
    "<use ui=\"EnumField\" id=\"enum\" data-alias-table=\"bodies\" "
    "data-alias-value=\"current\" "
    "data-event-change=\"chosen = ev.value; changes = changes + 1\"/>";

static const char AREA_B[] = "<button id=\"b-button\">B</button>";

static const char AREA_SHORT[] = "<color-field id=\"color\" "
                                 "value=\"#336699ff\"></color-field>";

static const char DATA[] =
    "{\"items\": [{\"label\": \"Cut\"}, {\"label\": \"Copy\"}, {\"label\": "
    "\"Paste\"}, {\"label\": \"Preferences\"}, {\"label\": \"Delete\", "
    "\"disabled\": true}],"
    "\"sub\": {\"label\": \"More\"}, \"subitems\": [{\"label\": \"First\"}, "
    "{\"label\": \"Second\"}],"
    "\"bodies\": {\"groups\": [{\"label\": \"\", \"options\": ["
    "{\"value\": 0, \"label\": \"Static\"}, {\"value\": 1, \"label\": "
    "\"Dynamic\"}, {\"value\": 2, \"label\": \"Kinematic\"}]}, {\"label\": "
    "\"More\", \"options\": [{\"value\": 3, \"label\": \"Sensor\"}, "
    "{\"value\": 4, \"label\": \"Static Sensor\"}]}]},"
    "\"current\": 1, \"chosen\": \"\", \"changes\": 0, \"ran\": 0, \"last\": "
    "\"\", \"checked\": false}";

typedef struct
{
    rml_backend *backend;
    rml_context *context;
    yyjson_mut_doc *data;
    rml_data_model *model;
    rml_element *a, *b, *low;
} popover_fixture;

static void frames(popover_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        rml_backend_update(f->backend, FRAME_SECONDS);
    }
}

/* Lets `seconds` pass, a frame at a time. */
static void pause_for(popover_fixture *f, double seconds)
{
    frames(f, (int)(seconds / FRAME_SECONDS) + 1);
}

static rml_element *by_id(popover_fixture *f, const char *id)
{
    rml_element *el = test_query(f->context, id);
    assert(el);
    return el;
}

static rml_vec2 center(rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    return (rml_vec2){ at.x + size.x / 2, at.y + size.y / 2 };
}

static void hover(popover_fixture *f, rml_vec2 at)
{
    rml_context_process_mouse_move(f->context, (int)at.x, (int)at.y, 0);
    frames(f, 1);
}

static void press_at(popover_fixture *f, rml_vec2 at)
{
    hover(f, at);
    rml_context_process_mouse_button_down(f->context, 0, 0);
    rml_context_process_mouse_button_up(f->context, 0, 0);
    frames(f, 1);
}

static void click(popover_fixture *f, const char *selector)
{
    press_at(f, center(by_id(f, selector)));
}

static void key(popover_fixture *f, rml_key k)
{
    rml_context_process_key_down(f->context, k, 0);
    rml_context_process_key_up(f->context, k, 0);
    frames(f, 1);
}

static void type(popover_fixture *f, const char *typed)
{
    rml_context_process_text_input(f->context, typed, strlen(typed));
    frames(f, 1);
}

static bool within(rml_element *el, const rml_element *ancestor)
{
    for (; el; el = rml_element_parent(el)) {
        if (el == ancestor) {
            return true;
        }
    }
    return false;
}

static bool open(popover_fixture *f, const char *selector)
{
    return popover_open(by_id(f, selector));
}

static const char *text(popover_fixture *f, const char *name)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(yyjson_mut_doc_get_root(f->data), name);
    return yyjson_mut_is_str(v) ? yyjson_mut_get_str(v) : "";
}

static double number(popover_fixture *f, const char *name)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(yyjson_mut_doc_get_root(f->data), name);
    return yyjson_mut_is_str(v) ? strtod(yyjson_mut_get_str(v), NULL)
                                : yyjson_mut_get_num(v);
}

/* The label of the menu's highlighted item, or "". */
static void lit(popover_fixture *f, const char *menu, char *out, size_t size)
{
    rml_element *item = rml_element_query_selector(by_id(f, menu), "[data-highlighted] .label");
    out[0] = '\0';
    for (int i = 0; item && i < rml_element_child_count(item); i++) {
        rml_element *node = rml_element_child(item, i);
        if (!strcmp(rml_element_tag_name(node), "#text") && !out[0]) {
            rml_element_text(node, out, size);
        }
    }
}

static bool lit_is(popover_fixture *f, const char *menu, const char *label)
{
    char shown[64];
    lit(f, menu, shown, sizeof shown);
    return !strcmp(shown, label);
}

/* `markup` expanded with projects/studio's widgets in a document of class
   `cls`. */
static rml_element *open_document(popover_fixture *f, const char *markup, const char *cls)
{
    ui_document document = { 0 };
    char error[256] = "", **uses = NULL;
    bool expanded = ui_document_read(markup, "", NULL, NULL, &document, error, sizeof error) &&
                    ui_document_expand(
                        &document,
                        "PopoverTest",
                        "projects/studio/content/ui",
                        false,
                        &uses,
                        error,
                        sizeof error
                    );
    for (int i = 0; i < acount(uses); i++) {
        free(uses[i]);
    }
    afree(uses);
    if (!expanded) {
        fprintf(stderr, "  popovers: %s\n", error);
        ui_document_free(&document);
        return NULL;
    }
    size_t size = strlen(document.rml) + strlen(document.rcss) + sizeof STYLE + 256;
    char *rml = malloc(size);
    snprintf(
        rml,
        size,
        "<rml><head><style>%s %s</style></head><body class=\"%s\" "
        "data-model=\"popovers\">%s</body></rml>",
        STYLE,
        document.rcss,
        cls,
        document.rml
    );
    ui_document_free(&document);
    rml_element *doc = rml_context_load_document_from_memory(f->context, rml, "[popovers]");
    free(rml);
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    return doc;
}

static bool open_fixture(popover_fixture *f)
{
    f->backend = rml_backend_create(640, 480, 1);
    f->context = rml_backend_context(f->backend);
    yyjson_doc *parsed = yyjson_read(DATA, strlen(DATA), 0);
    f->data = yyjson_doc_mut_copy(parsed, NULL);
    yyjson_doc_free(parsed);
    f->model = rml_data_model_create(f->context, "popovers", f->data, NULL, NULL);
    f->a = open_document(f, AREA_A, "a");
    f->b = open_document(f, AREA_B, "b");
    f->low = open_document(f, AREA_SHORT, "short");
    frames(f, 3);
    return f->a && f->b && f->low;
}

static void close_fixture(popover_fixture *f)
{
    rml_element *docs[] = { f->a, f->b, f->low };
    for (size_t i = 0; i < sizeof docs / sizeof *docs; i++) {
        if (docs[i]) {
            rml_document_close(docs[i]);
        }
    }
    frames(f, 1);
    rml_data_model_destroy(f->model);
    yyjson_mut_doc_free(f->data);
    rml_backend_destroy(f->backend);
}

/* The elements under `selector`'s element. */
static int built_under(popover_fixture *f, const char *selector)
{
    return rml_element_query_selector_all(by_id(f, selector), "*", NULL, 0);
}

/* The text of the menu's first item shown (not a data-for template). */
static void first_item(popover_fixture *f, char *out, size_t size)
{
    rml_element *labels[16];
    int count = test_query_all(f->context, "#menu .menu-item .label", labels, 16);
    out[0] = '\0';
    for (int i = 0; i < count && i < 16 && !out[0]; i++) {
        rml_element *node = rml_element_first_child(labels[i]);
        if (node && rml_element_is_visible(labels[i], true)) {
            rml_element_text(node, out, size);
        }
    }
}

static void set_first_label(popover_fixture *f, const char *label)
{
    yyjson_mut_val *items = yyjson_mut_obj_get(yyjson_mut_doc_get_root(f->data), "items");
    yyjson_mut_set_str(yyjson_mut_obj_get(yyjson_mut_arr_get(items, 0), "label"), label);
    rml_data_model_dirty(f->model, "items");
    frames(f, 2);
}

/* A popover with data-template (Menu and Submenu) builds its items while it
   shows and holds nothing closed: no elements, no views, so a change to
   what its items show reads no value; opening builds them with the current
   value, closing keeps them through the fade and then removes them. */
static void mounted_while_open(popover_fixture *f)
{
    rml_data_reads before = rml_data_model_reads(f->model);
    set_first_label(f, "Cut!");
    rml_data_reads after = rml_data_model_reads(f->model);
    test_check(
        built_under(f, "#menu") == 0 && built_under(f, "#file-menu") == 0 &&
            after.values == before.values && after.lookups == before.lookups,
        "a closed menu holds no elements and binds nothing: a change to what "
        "its items show evaluates no view"
    );
    click(f, "#trigger");
    char label[32];
    first_item(f, label, sizeof label);
    int built = built_under(f, "#menu");
    test_check(
        open(f, "#menu") && built > 0 && !strcmp(label, "Cut!") &&
            built_under(f, "#menu #submenu") == 0 &&
            rml_data_model_reads(f->model).values > after.values,
        "opening a menu builds its items bound to the current values, and the "
        "submenu in it stays unbuilt"
    );
    key(f, RML_KEY_ESCAPE);
    bool fading = !open(f, "#menu") && built_under(f, "#menu") == built &&
                  rml_element_has_attribute(by_id(f, "#menu"), "data-ending-style");
    click(f, "#trigger");
    first_item(f, label, sizeof label);
    test_check(
        fading && open(f, "#menu") && built_under(f, "#menu") == built &&
            !strcmp(label, "Cut!"),
        "a closed menu keeps its items through its fade, and opening it again "
        "during the fade keeps them"
    );
    key(f, RML_KEY_ESCAPE);
    pause_for(f, 0.3);
    test_check(
        !open(f, "#menu") && built_under(f, "#menu") == 0 &&
            !rml_element_has_attribute(by_id(f, "#menu"), "data-open"),
        "the end of a menu's fade removes its items"
    );
    click(f, "#trigger");
    hover(f, center(by_id(f, "#menu .submenu > .menu-item")));
    pause_for(f, 0.15);
    bool sub = open(f, "#menu #submenu") && built_under(f, "#menu #submenu") > 0;
    key(f, RML_KEY_ESCAPE);
    key(f, RML_KEY_ESCAPE);
    pause_for(f, 0.3);
    test_check(
        sub && !open(f, "#menu") && built_under(f, "#menu") == 0,
        "a submenu builds when it opens, and closing its menu removes both"
    );
    set_first_label(f, "Cut");
}

/* A menu by the edge of area A reaches over area B and stays above it once
   B takes focus; Escape closes it and leaves the focus on its trigger. */
static void above_areas(popover_fixture *f)
{
    click(f, "#trigger");
    rml_element *menu = by_id(f, "#menu");
    rml_vec2 at = rml_element_absolute_offset(menu, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(menu, RML_BOX_BORDER);
    rml_vec2 trigger = rml_element_absolute_offset(by_id(f, "#trigger"), RML_BOX_BORDER);
    rml_vec2 over = { 260, at.y + 10 };
    rml_element_focus(by_id(f, "#b-button"), true);
    frames(f, 2);
    rml_element *hit = rml_context_element_at_point(f->context, over, NULL);
    test_check(
        open(f, "#menu") && at.x + size.x > 260 && at.y >= trigger.y + 22 &&
            hit && rml_element_owner_document(hit) == f->a,
        "a menu opened by an area's edge draws and hit-tests above the next "
        "area, even once that area takes focus"
    );
    rml_element_focus(by_id(f, "#trigger"), true);
    key(f, RML_KEY_ESCAPE);
    test_check(
        !open(f, "#menu") && rml_context_focus_element(f->context) == by_id(f, "#trigger"),
        "Escape closes the menu and the trigger keeps the focus"
    );
    click(f, "#trigger");
    click(f, "#b-button");
    test_check(!open(f, "#menu"), "a press in another area closes the menu");
}

/* A disabled item never highlights: not under the pointer, which leaves
   none highlighted, nor by End or typeahead, which pass it; a click on it
   runs nothing and keeps the menu open (Base UI). */
static void disabled_items(popover_fixture *f)
{
    rml_element *items[8], *removal = NULL;
    int count = test_query_all(f->context, "#menu .menu-item[data-disabled]", items, 8);
    for (int i = 0; i < count && i < 8; i++) {
        removal = rml_element_has_attribute(items[i], "data-for") ? removal
                                                                  : items[i];
    }
    if (!removal) {
        test_check(false, "the menu has a disabled item");
        return;
    }
    hover(f, center(by_id(f, "#menu .menu-item:not([data-for])")));
    hover(f, center(removal));
    bool pointer = !rml_element_has_attribute(removal, "data-highlighted") &&
                   !test_query(f->context, "#menu [data-highlighted]");
    key(f, RML_KEY_END);
    bool end = lit_is(f, "#menu", "More");
    key(f, RML_KEY_UP);
    key(f, RML_KEY_UP);
    end &= lit_is(f, "#menu", "Preferences");
    pause_for(f, 0.6);
    key(f, RML_KEY_D);
    bool typed = !rml_element_has_attribute(removal, "data-highlighted");
    double ran = number(f, "ran");
    click(f, "#menu .menu-item[data-disabled]:not([data-for])");
    test_check(
        pointer && end && typed && number(f, "ran") == ran && open(f, "#menu"),
        "a disabled menu item never highlights, by pointer, End or typeahead, "
        "and a click on it runs nothing and keeps the menu open"
    );
}

/* Typeahead, Enter, a submenu's delay, Right and Left. */
static void menu_keys(popover_fixture *f)
{
    rml_element_focus(by_id(f, "#trigger"), true);
    key(f, RML_KEY_RETURN);
    frames(f, 2);
    bool first = open(f, "#menu") && lit_is(f, "#menu", "Cut");
    key(f, RML_KEY_P);
    bool paste = lit_is(f, "#menu", "Paste");
    key(f, RML_KEY_R);
    bool preferences = lit_is(f, "#menu", "Preferences");
    pause_for(f, 0.6);
    key(f, RML_KEY_C);
    test_check(
        first && paste && preferences && lit_is(f, "#menu", "Cut"),
        "a menu a key opens highlights its first item, and typeahead finds "
        "items by the letters typed within half a second"
    );
    key(f, RML_KEY_DOWN);
    key(f, RML_KEY_RETURN);
    test_check(
        !open(f, "#menu") && number(f, "ran") == 1 && !strcmp(text(f, "last"), "Copy"),
        "Enter runs the highlighted item once and closes its menu"
    );
    click(f, "#trigger");
    rml_element *row = by_id(f, "#menu .submenu > .menu-item");
    hover(f, center(row));
    rml_element *sub = by_id(f, "#menu #submenu");
    bool early = open(f, "#menu #submenu");
    pause_for(f, 0.12);
    rml_vec2 row_at = rml_element_absolute_offset(row, RML_BOX_BORDER);
    rml_vec2 sub_at = rml_element_absolute_offset(sub, RML_BOX_BORDER);
    test_check(
        !early && popover_open(sub) && sub_at.x > row_at.x + 100,
        "a submenu opens beside its row after a 100 ms hover, not before"
    );
    key(f, RML_KEY_LEFT);
    bool left = !popover_open(sub) && open(f, "#menu");
    key(f, RML_KEY_RIGHT);
    frames(f, 2);
    test_check(
        left && popover_open(sub) && lit_is(f, "#menu #submenu", "First"),
        "Left closes a submenu and Right opens it with its first item "
        "highlighted"
    );
    key(f, RML_KEY_DOWN);
    key(f, RML_KEY_RETURN);
    test_check(
        !open(f, "#menu") && !popover_open(sub) && !strcmp(text(f, "last"), "Second"),
        "an item in a submenu runs and closes both menus"
    );
    click(f, "#trigger");
    click(f, "#menu .menu-check");
    test_check(
        open(f, "#menu") &&
            yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(f->data), "checked")),
        "a checkbox item toggles and its menu stays open"
    );
    disabled_items(f);
    key(f, RML_KEY_ESCAPE);
}

/* While a menu a menubar trigger opened is open, hovering another trigger
   opens its menu, and Right steps back. */
static void menubar(popover_fixture *f)
{
    rml_vec2 file = center(by_id(f, "#file"));
    hover(f, file);
    rml_context_process_mouse_button_down(f->context, 0, 0);
    rml_context_process_mouse_button_up(f->context, 0, 0);
    frames(f, 2);
    bool pressed = open(f, "#file-menu");
    hover(f, center(by_id(f, "#view")));
    frames(f, 1);
    bool switched = open(f, "#view-menu") && !open(f, "#file-menu");
    key(f, RML_KEY_RIGHT);
    frames(f, 2);
    test_check(
        pressed && switched && open(f, "#file-menu") &&
            !open(f, "#view-menu") && lit_is(f, "#file-menu", "New"),
        "a menubar opens on press, switches menus on hover, and Right steps "
        "to the next trigger's menu with its first item highlighted"
    );
    key(f, RML_KEY_ESCAPE);
}

/* A popover takes the focus into its first field; Escape gives it back. */
static void focus_return(popover_fixture *f)
{
    click(f, "#note-trigger");
    frames(f, 2);
    bool focused = rml_context_focus_element(f->context) == by_id(f, "#note-field");
    key(f, RML_KEY_ESCAPE);
    test_check(
        focused && !open(f, "#note") &&
            rml_context_focus_element(f->context) == by_id(f, "#note-trigger"),
        "a popover focuses its first field, and Escape closes it and returns "
        "the focus to its trigger"
    );
    click(f, "#note-trigger");
    frames(f, 2);
    key(f, RML_KEY_TAB);
    bool inside = open(f, "#note") &&
                  rml_context_focus_element(f->context) == by_id(f, "#note-ok");
    key(f, RML_KEY_TAB);
    test_check(inside && !open(f, "#note"), "Tab moves within a popover and closes it past its end");
}

/* A color picker opened in a short area at the window's bottom flips above
   its field, fully in the window, and draws above the area over it. */
static void picker_escapes(popover_fixture *f)
{
    click(f, "#color");
    frames(f, 2);
    rml_element *picker = test_query(f->context, "#color .picker");
    rml_vec2 at = picker ? rml_element_absolute_offset(picker, RML_BOX_BORDER)
                         : (rml_vec2){ 0 };
    rml_vec2 size = picker ? rml_element_box_size(picker, RML_BOX_BORDER)
                           : (rml_vec2){ 0 };
    rml_element *hit = picker
                         ? rml_context_element_at_point(f->context, center(picker), NULL)
                         : NULL;
    test_check(
        picker && at.y >= 4 && at.y + size.y <= 400 && at.x >= 4 &&
            at.x + size.x <= 640 - 4 && within(hit, picker),
        "a color picker in a short area flips above its field, inside the "
        "window, and draws above the area over it"
    );
    key(f, RML_KEY_ESCAPE);
}

/* The options a list shows: neither filtered nor a data-for template. */
static int shown_options(rml_element *list)
{
    rml_element *options[64];
    int count = rml_element_query_selector_all(list, "[role=option]", options, 64),
        shown = 0;
    for (int i = 0; i < count && i < 64; i++) {
        shown += !rml_element_has_attribute(options[i], "data-for") &&
                 rml_element_is_visible(options[i], true);
    }
    return shown;
}

/* EnumField: a searchable dropdown of every value, its search filtering
   and its keys choosing. */
static void enum_field(popover_fixture *f)
{
    char value[32] = "";
    rml_element *labels[8];
    int count = test_query_all(f->context, "#enum .part.value span span", labels, 8);
    for (int i = 0; i < count && i < 8; i++) {
        rml_element *node = rml_element_first_child(labels[i]);
        if (node && rml_element_is_visible(labels[i], true)) {
            rml_element_text(node, value, sizeof value);
        }
    }
    click(f, "#enum button");
    frames(f, 2);
    rml_element *list = by_id(f, "#enum choice-list");
    rml_element *search = by_id(f, "#enum choice-list input");
    bool opened = open(f, "#enum #choices") &&
                  rml_context_focus_element(f->context) == search &&
                  shown_options(list) == 5 && lit_is(f, "#enum #choices", "Dynamic");
    type(f, "sta");
    bool filtered = shown_options(list) == 2 && lit_is(f, "#enum #choices", "Static");
    key(f, RML_KEY_DOWN);
    key(f, RML_KEY_RETURN);
    test_check(
        !strcmp(value, "Dynamic") && opened && filtered &&
            !strcmp(text(f, "chosen"), "4") && number(f, "changes") == 1 &&
            !open(f, "#enum #choices") &&
            rml_context_focus_element(f->context) == by_id(f, "#enum button"),
        "EnumField shows its value, lists every option with the current one "
        "highlighted, filters as its search is typed, and Down and Enter "
        "choose, closing it and returning the focus"
    );
    click(f, "#enum button");
    frames(f, 2);
    bool reset = shown_options(list) == 5;
    type(f, "zzz");
    test_check(
        reset && shown_options(list) == 0 && rml_element_has_attribute(list, "data-empty"),
        "a ChoiceList opens again unfiltered and marks itself empty when "
        "nothing matches"
    );
    key(f, RML_KEY_ESCAPE);
}

/* choices.fields lists every value of every reflected choice field. */
static void choice_tables()
{
    yyjson_mut_doc *doc = studio_sources_new();
    yyjson_mut_val *fields = yyjson_mut_obj_get(
        yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "choices"),
        "fields"
    );
    int tables = 0, wrong = 0;
    for (int c = 0; c < GAME_REGISTRY_COMPONENT_COUNT; c++) {
        const ecs_component_desc *desc = GAME_REGISTRY_COMPONENTS[c];
        for (int i = 0; desc->type && i < desc->type->field_count; i++) {
            const editor_choices
                *choices = property_choices(desc->type, &desc->type->fields[i]);
            if (!choices || !editor_choices_count(choices)) {
                continue;
            }
            yyjson_mut_val *table = yyjson_mut_obj_get(
                yyjson_mut_obj_get(fields, desc->name),
                desc->type->fields[i].name
            );
            yyjson_mut_val *groups = yyjson_mut_obj_get(table, "groups");
            size_t listed = 0;
            for (size_t g = 0; g < yyjson_mut_arr_size(groups); g++) {
                listed += yyjson_mut_arr_size(
                    yyjson_mut_obj_get(yyjson_mut_arr_get(groups, g), "options")
                );
            }
            tables++;
            wrong += (int)listed != editor_choices_count(choices);
        }
    }
    yyjson_mut_doc_free(doc);
    test_check(tables > 0 && !wrong, "choices.fields lists every value of every reflected choice field");
}

/* The menu and choice widgets and the panels that use them read without a
   warning from RmlUi. */
static void assets_check()
{
    static const char *const NAMES[] = {
        "Menu",       "MenuItem",  "MenuCommand", "MenuGroup",  "MenuSeparator",
        "Submenu",    "MenuCheck", "MenuOption",  "ChoiceList", "EnumField",
        "AssetField", "GraphView", "AreaHeader",  "Widgets",
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
    test_check(clean == count, "the menu and choice widgets and the panels using them check clean");
}

void test_ui_popovers()
{
    popover_fixture f = { 0 };
    bool opened = open_fixture(&f);
    test_check(opened, "the menu widgets expand");
    if (opened) {
        mounted_while_open(&f);
        above_areas(&f);
        menu_keys(&f);
        menubar(&f);
        focus_return(&f);
        picker_escapes(&f);
        enum_field(&f);
    }
    close_fixture(&f);
    choice_tables();
    assets_check();
}
