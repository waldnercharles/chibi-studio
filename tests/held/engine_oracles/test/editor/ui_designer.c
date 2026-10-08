#include "authoring/ui_loader.h"
/* The visual designer (workstream H): each Hierarchy and Details change
 * writes the document's text as expected, keeping its formatting, as one
 * undo step; and a selection maps between a shown element's data-src, the
 * outline's indexes and the Text Editor's cursor offsets. */
#include "editor/documents/ui_design.h"
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_internal.h"
#include "render/ui.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "test/suite_internal.h"
#include "ui/rml.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char RML[] = "<div id=\"root\" class=\"panel\">\n"
                          "  <span class=\"title\">Title</span>\n"
                          "  <div class=\"row\">\n"
                          "    <button id=\"ok\">OK</button>\n"
                          "    <button id=\"cancel\">Cancel</button>\n"
                          "  </div>\n"
                          "</div>\n";

static const char RCSS[] = ".panel { display: block; width: 200dp; }\n"
                           ".row button { color: #ff0000; }\n";

enum
{
    ROOT,
    TITLE,
    ROW,
    OK,
    CANCEL,
};

/* The document opened fresh from its markup and styles. */
static ui_buffer *open_document(const char *folder, const char *name, const char *rml, const char *rcss)
{
    char path[PATH_MAX], error[256];
    test_write_ui(folder, name, rml, rcss);
    snprintf(path, sizeof path, "%s/%s.rml", folder, name);
    ui_buffer *b = ui_buffer_open(path, error, sizeof error);
    assert(b);
    return b;
}

/* Whether a change left `rml` and `rcss` as the parts, one undo step deep,
 * and undoing it brings back the text before. */
static bool one_step(ui_buffer *b, const char *rml, const char *rcss, const char *before_rml, const char *before_rcss)
{
    bool written = !strcmp(ui_buffer_text(b, UI_PART_RML), rml) &&
                   !strcmp(ui_buffer_text(b, UI_PART_RCSS), rcss) &&
                   acount(b->undo) == 1;
    if (!written) {
        printf(
            "  rml:\n%s  rcss:\n%s  steps %d\n",
            ui_buffer_text(b, UI_PART_RML),
            ui_buffer_text(b, UI_PART_RCSS),
            acount(b->undo)
        );
    }
    ui_buffer_undo(b);
    return written && !strcmp(ui_buffer_text(b, UI_PART_RML), before_rml) &&
           !strcmp(ui_buffer_text(b, UI_PART_RCSS), before_rcss);
}

static bool markup_step(ui_buffer *b, const char *rml)
{
    return one_step(b, rml, RCSS, RML, RCSS);
}

static void moves(const char *folder)
{
    ui_buffer *b = open_document(folder, "Moves", RML, RCSS);
    char error[192] = "";
    test_check(
        ui_design_move(b, CANCEL, OK, UI_SOURCE_BEFORE, error, sizeof error) == OK &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div class=\"row\">\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "    <button id=\"ok\">OK</button>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Hierarchy: dragging a row above its sibling reorders the markup, "
        "selecting the moved element"
    );
    test_check(
        ui_design_move(b, TITLE, ROW, UI_SOURCE_INSIDE, error, sizeof error) == CANCEL &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <div class=\"row\">\n"
                "    <button id=\"ok\">OK</button>\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "    <span class=\"title\">Title</span>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Hierarchy: dropping a row onto another reparents it after the "
        "target's children, indented"
    );
    test_check(
        ui_design_move(b, OK, ROW, UI_SOURCE_AFTER, error, sizeof error) == CANCEL &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div class=\"row\">\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "  </div>\n"
                "  <button id=\"ok\">OK</button>\n"
                "</div>\n"
            ),
        "Hierarchy: dropping below a parent moves the element out, at the "
        "parent's indentation"
    );
    int before = acount(b->undo);
    test_check(
        ui_design_move(b, ROW, OK, UI_SOURCE_INSIDE, error, sizeof error) == -1 &&
            error[0] && acount(b->undo) == before &&
            !strcmp(ui_buffer_text(b, UI_PART_RML), RML),
        "Hierarchy: an element cannot move into itself, and the refusal leaves "
        "no step"
    );
    ui_buffer_free(b);
}

static void structure(const char *folder)
{
    ui_buffer *b = open_document(folder, "Structure", RML, RCSS);
    char error[192] = "";
    test_check(
        ui_design_insert(b, ROW, UI_SOURCE_INSIDE, "<use ui=\"Chip\"/>", error, sizeof error) ==
                5 &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div class=\"row\">\n"
                "    <button id=\"ok\">OK</button>\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "    <use ui=\"Chip\"/>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Library: a widget added to a container is its last child, on its own "
        "line"
    );
    test_check(
        ui_design_duplicate(b, OK, error, sizeof error) == CANCEL &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div class=\"row\">\n"
                "    <button id=\"ok\">OK</button>\n"
                "    <button id=\"ok\">OK</button>\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Hierarchy: Duplicate copies the element after itself and selects the "
        "copy"
    );
    test_check(
        ui_design_wrap(b, ROW, "div", error, sizeof error) == ROW &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div>\n"
                "    <div class=\"row\">\n"
                "      <button id=\"ok\">OK</button>\n"
                "      <button id=\"cancel\">Cancel</button>\n"
                "    </div>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Hierarchy: Wrap in Div puts the element and its children one level "
        "deeper inside a new div"
    );
    test_check(
        ui_design_delete(b, OK, error, sizeof error) == OK &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div class=\"row\">\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Hierarchy: Delete removes the element's lines and selects its next "
        "sibling"
    );
    test_check(
        ui_design_delete(b, CANCEL, error, sizeof error) == OK &&
            ui_design_delete(b, OK, error, sizeof error) == ROW &&
            acount(b->undo) == 2,
        "Hierarchy: deleting a last child selects its previous sibling, then "
        "its parent"
    );
    ui_buffer_undo(b);
    ui_buffer_undo(b);
    ui_buffer_free(b);
}

static void attributes(const char *folder)
{
    ui_buffer *b = open_document(folder, "Attributes", RML, RCSS);
    char error[192] = "";
    const char *renamed = "<div id=\"root\" class=\"panel\">\n"
                          "  <span class=\"title\">Title</span>\n"
                          "  <div class=\"row\">\n"
                          "    <button id=\"confirm\">OK</button>\n"
                          "    <button id=\"cancel\">Cancel</button>\n"
                          "  </div>\n"
                          "</div>\n";
    test_check(
        ui_design_set_attribute(b, OK, "id", "confirm", error, sizeof error) &&
            markup_step(b, renamed),
        "Details: editing the id rewrites its value alone"
    );
    const char *classed = "<div id=\"root\" class=\"panel\">\n"
                          "  <span class=\"title big\">Title</span>\n"
                          "  <div class=\"row\">\n"
                          "    <button id=\"ok\">OK</button>\n"
                          "    <button id=\"cancel\">Cancel</button>\n"
                          "  </div>\n"
                          "</div>\n";
    test_check(
        ui_design_class(b, TITLE, "big", true, error, sizeof error) &&
            markup_step(b, classed),
        "Details: adding a class chip appends it to the class attribute"
    );
    const char *unclassed = "<div id=\"root\" class=\"panel\">\n"
                            "  <span>Title</span>\n"
                            "  <div class=\"row\">\n"
                            "    <button id=\"ok\">OK</button>\n"
                            "    <button id=\"cancel\">Cancel</button>\n"
                            "  </div>\n"
                            "</div>\n";
    test_check(
        ui_design_class(b, TITLE, "title", false, error, sizeof error) &&
            markup_step(b, unclassed),
        "Details: removing the last class chip removes the attribute"
    );
    const char *bound = "<div id=\"root\" class=\"panel\">\n"
                        "  <span class=\"title\">Title</span>\n"
                        "  <div class=\"row\">\n"
                        "    <button id=\"ok\" "
                        "data-event-click=\"emit('Ok')\">OK</button>\n"
                        "    <button id=\"cancel\">Cancel</button>\n"
                        "  </div>\n"
                        "</div>\n";
    test_check(
        ui_design_set_attribute(b, OK, "data-event-click", "emit('Ok')", error, sizeof error) &&
            markup_step(b, bound),
        "Details: adding a binding writes it after the element's attributes"
    );
    const char *retyped = "<div id=\"root\" class=\"panel\">\n"
                          "  <span class=\"title\">Title</span>\n"
                          "  <div class=\"row\">\n"
                          "    <button id=\"ok\">Okay</button>\n"
                          "    <button id=\"cancel\">Cancel</button>\n"
                          "  </div>\n"
                          "</div>\n";
    test_check(
        ui_design_set_text(b, OK, "Okay", error, sizeof error) &&
            markup_step(b, retyped) &&
            !ui_design_set_text(b, ROW, "No", error, sizeof error) &&
            acount(b->undo) == 0,
        "Details: an element holding text alone is retyped; one holding "
        "elements is not"
    );
    ui_buffer_free(b);
}

/* A scrub's changes, joined, stay one step whose undo is the text before. */
static void styles(const char *folder)
{
    ui_buffer *b = open_document(folder, "Styles", RML, RCSS);
    char error[192] = "";
    const char
        *styled = "<div id=\"root\" class=\"panel\" style=\"width: 130dp;\">\n"
                  "  <span class=\"title\">Title</span>\n"
                  "  <div class=\"row\">\n"
                  "    <button id=\"ok\">OK</button>\n"
                  "    <button id=\"cancel\">Cancel</button>\n"
                  "  </div>\n"
                  "</div>\n";
    bool scrubbed = ui_design_set_style(b, ROOT, "width", "120dp", false, error, sizeof error) &&
                    ui_design_set_style(b, ROOT, "width", "125dp", true, error, sizeof error) &&
                    ui_design_set_style(b, ROOT, "width", "130dp", true, error, sizeof error);
    test_check(scrubbed && markup_step(b, styled), "Details: a scrubbed inline style value is one undo step");
    const char *sized = "<div id=\"root\" class=\"panel\" style=\"width: "
                        "150dp; height: 40dp;\">\n"
                        "  <span class=\"title\">Title</span>\n"
                        "  <div class=\"row\">\n"
                        "    <button id=\"ok\">OK</button>\n"
                        "    <button id=\"cancel\">Cancel</button>\n"
                        "  </div>\n"
                        "</div>\n";
    const ui_design_style size[] = { { "width", "150dp" }, { "height", "40dp" } };
    bool resized = ui_design_set_styles(b, ROOT, size, 2, error, sizeof error);
    test_check(resized && markup_step(b, sized), "Canvas: a resize writes width and height as one undo step");
    const char *rule = ".panel { display: block; width: 240dp; }\n"
                       ".row button { color: #ff0000; }\n";
    bool picked = ui_design_set_declaration(b, 0, 1, "220dp", false, error, sizeof error) &&
                  ui_design_set_declaration(b, 0, 1, "240dp", true, error, sizeof error);
    test_check(picked && one_step(b, RML, rule, RML, RCSS), "Details: a matched rule's declaration value edits in the rcss block as one step");
    int steps = acount(b->undo);
    test_check(
        !ui_design_set_declaration(b, 0, 1, "1dp; color: red", false, error, sizeof error) &&
            acount(b->undo) == steps && !strcmp(ui_buffer_text(b, UI_PART_RCSS), RCSS),
        "Details: a value that would end its declaration is refused"
    );
    ui_buffer_free(b);
}

/* A resize handle grows the moved axes by the pointer's travel in document
   dp, in whole dp, never under the padding and border; content-box sizes
   leave them out; Shift at the corner keeps the aspect, the axis moved
   further leading. */
static void resize_math()
{
    const unsigned both = DESIGNER_RESIZE_WIDTH | DESIGNER_RESIZE_HEIGHT;
    designer_box content = { { 100, 50 }, { 20, 10 }, false };
    designer_box border = { { 100, 50 }, { 20, 10 }, true };
    float a[2], b[2], c[2], d[2];
    designer_resize_css(&content, DESIGNER_RESIZE_WIDTH, 30.4f, 99, false, a);
    designer_resize_css(&content, both, 20, 2, true, b);
    designer_resize_css(&content, DESIGNER_RESIZE_WIDTH, -500, 0, false, c);
    designer_resize_css(&border, DESIGNER_RESIZE_HEIGHT, 0, -100, false, d);
    test_check(
        a[0] == 110 && a[1] == 40 && b[0] == 100 && b[1] == 50 && c[0] == 0 &&
            d[0] == 100 && d[1] == 10,
        "Canvas: resize sizes follow the handle, box sizing, the minimum and "
        "the aspect"
    );
}

/* Insertions indent as the markup around them does, four spaces here. */
static void keeps_indentation(const char *folder)
{
    ui_buffer *b = open_document(folder, "Wide", "<div class=\"a\">\n    <span>x</span>\n</div>\n", NULL);
    char error[192] = "";
    test_check(
        ui_design_insert(b, 0, UI_SOURCE_INSIDE, "<button>Button</button>", error, sizeof error) ==
                2 &&
            one_step(
                b,
                "<div class=\"a\">\n"
                "    <span>x</span>\n"
                "    <button>Button</button>\n"
                "</div>\n",
                "",
                "<div class=\"a\">\n"
                "    <span>x</span>\n"
                "</div>\n",
                ""
            ),
        "Library: an element added keeps the document's own indentation step"
    );
    ui_buffer_free(b);
}

/* Workstream Q: Details adds and removes a matched rule's declarations,
 * written as the rule is, one undo step each. */
static void declarations(const char *folder)
{
    ui_buffer *b = open_document(folder, "Declarations", RML, RCSS);
    char error[192] = "";
    const char
        *added = ".panel { display: block; width: 200dp; height: 40dp; }\n"
                 ".row button { color: #ff0000; }\n";
    test_check(
        ui_design_add_declaration(b, 0, "height", "40dp", error, sizeof error) &&
            one_step(b, RML, added, RML, RCSS),
        "Details: a declaration added to a rule on one line joins its line"
    );
    const char *removed = ".panel { width: 200dp; }\n"
                          ".row button { color: #ff0000; }\n";
    test_check(
        ui_design_remove_declaration(b, 0, 0, error, sizeof error) &&
            one_step(b, RML, removed, RML, RCSS),
        "Details: a declaration removed from a rule on one line leaves the rest"
    );
    test_check(
        !ui_design_add_declaration(b, 0, "height", "1dp; color: red", error, sizeof error) &&
            !ui_design_add_declaration(b, 0, "9lives", "1", error, sizeof error) &&
            !ui_design_remove_declaration(b, 1, 3, error, sizeof error) &&
            !acount(b->undo),
        "Details: a declaration that is not one, or is not there, is refused"
    );
    ui_buffer_free(b);
    b = open_document(folder, "Lines", "<div class=\"card\"/>\n", ".card {\n    color: red;\n}\n.empty {\n}\n");
    const char *rml = "<div class=\"card\"/>\n";
    const char *before = ".card {\n    color: red;\n}\n.empty {\n}\n";
    test_check(
        ui_design_add_declaration(b, 0, "margin", "2dp", error, sizeof error) &&
            one_step(b, rml, ".card {\n    color: red;\n    margin: 2dp;\n}\n.empty {\n}\n", rml, before),
        "Details: a declaration added to a rule of many lines takes a line "
        "indented as its others"
    );
    test_check(
        ui_design_add_declaration(b, 1, "--gap", "4dp", error, sizeof error) &&
            one_step(b, rml, ".card {\n    color: red;\n}\n.empty {\n  --gap: 4dp;\n}\n", rml, before),
        "Details: a custom property added to an empty rule takes a line of its "
        "own"
    );
    test_check(
        ui_design_remove_declaration(b, 0, 0, error, sizeof error) &&
            one_step(b, rml, ".card {\n}\n.empty {\n}\n", rml, before),
        "Details: a declaration removed from its own line takes the line"
    );
    ui_buffer_free(b);
}

/* Extract to class moves the inline style into a rule written as the
 * RCSS's rules are, as one step over both parts. */
static void extraction(const char *folder)
{
    const char *styled_rml = "<div id=\"root\" class=\"panel\" style=\"width: "
                             "130dp; color: red;\">\n"
                             "  <span class=\"title\">Title</span>\n"
                             "</div>\n";
    ui_buffer *b = open_document(folder, "Extract", styled_rml, ".panel { display: block; }\n");
    char error[192] = "";
    const char *classed = "<div id=\"root\" class=\"panel wide\">\n"
                          "  <span class=\"title\">Title</span>\n"
                          "</div>\n";
    test_check(
        ui_design_extract_class(b, 0, "wide", error, sizeof error) &&
            one_step(b, classed, ".panel { display: block; }\n.wide { width: 130dp; color: red; }\n", styled_rml, ".panel { display: block; }\n"),
        "Details: Extract to class moves the inline style into a new rule and "
        "applies the class, one undo step over both parts"
    );
    test_check(
        !ui_design_extract_class(b, 1, "plain", error, sizeof error) &&
            !ui_design_extract_class(b, 0, "2wide", error, sizeof error) &&
            !acount(b->undo),
        "Details: an element without inline style, or a name that is no class, "
        "extracts nothing"
    );
    ui_buffer_free(b);
    b = open_document(folder, "ExtractLines", "<span style=\"color: red\">x</span>\n", ".panel {\n  display: block;\n}\n");
    test_check(
        ui_design_extract_class(b, 0, "alert", error, sizeof error) &&
            one_step(
                b,
                "<span class=\"alert\">x</span>\n",
                ".panel {\n  display: block;\n}\n\n.alert {\n  color: "
                "red;\n}\n",
                "<span style=\"color: red\">x</span>\n",
                ".panel {\n  display: block;\n}\n"
            ),
        "Details: an extracted rule takes lines of its own after rules that do"
    );
    ui_buffer_free(b);
}

/* dnd-kit's keyboard sensor over the sortable tree: the arrows step through
 * each gap between rows at each depth. */
static void keyboard_slots(const char *folder)
{
    ui_source *source = ui_source_parse(RML, UI_SOURCE_RML);
    ui_design_slot *slots = NULL;
    int home = ui_design_slots(source, OK, NULL, &slots);
    const ui_design_slot expected[] = {
        { ROOT, UI_SOURCE_BEFORE },   { TITLE, UI_SOURCE_BEFORE },
        { TITLE, UI_SOURCE_INSIDE },  { ROW, UI_SOURCE_BEFORE },
        { CANCEL, UI_SOURCE_BEFORE }, { CANCEL, UI_SOURCE_INSIDE },
        { CANCEL, UI_SOURCE_AFTER },  { ROW, UI_SOURCE_AFTER },
        { ROOT, UI_SOURCE_AFTER },
    };
    bool listed = acount(slots) == (int)(sizeof expected / sizeof *expected) &&
                  home == 4;
    for (int i = 0; listed && i < acount(slots); i++) {
        listed = slots[i].target == expected[i].target &&
                 slots[i].place == expected[i].place;
    }
    test_check(listed, "Hierarchy: the keyboard steps through every gap between rows, at each depth, leaving out the carried row");
    asetlen(slots, 0);
    bool folded[] = { false, false, true, false, false };
    home = ui_design_slots(source, TITLE, folded, &slots);
    test_check(
        acount(slots) == 4 && home == 1 && slots[2].target == ROW &&
            slots[2].place == UI_SOURCE_AFTER,
        "Hierarchy: a folded row's children are left out"
    );
    asetlen(slots, 0);
    home = ui_design_slots(source, OK, folded, &slots);
    test_check(acount(slots) == 9 && home == 4, "Hierarchy: the carried row's own parent stays open");
    ui_buffer *b = open_document(folder, "Slots", RML, RCSS);
    char error[192] = "";
    const ui_design_slot last = slots[6];
    test_check(
        ui_design_move(b, OK, last.target, last.place, error, sizeof error) == CANCEL &&
            markup_step(
                b,
                "<div id=\"root\" class=\"panel\">\n"
                "  <span class=\"title\">Title</span>\n"
                "  <div class=\"row\">\n"
                "    <button id=\"cancel\">Cancel</button>\n"
                "    <button id=\"ok\">OK</button>\n"
                "  </div>\n"
                "</div>\n"
            ),
        "Hierarchy: a keyboard drop moves the element to its slot as one step"
    );
    ui_buffer_free(b);
    afree(slots);
    ui_source_free(source);
}

/* ---------- Selection ---------- */

static int offset_of(const char *text, const char *needle)
{
    const char *at = strstr(text, needle);
    assert(at);
    return (int)(at - text);
}

static void selection_maps()
{
    ui_source *source = ui_source_parse(RML, UI_SOURCE_RML);
    test_check(
        ui_design_source_line("Card:12", "Card") == 12 &&
            !ui_design_source_line("Cards:3", "Card") &&
            !ui_design_source_line("Other:1", "Card"),
        "Selection: data-src names a line of the document's own asset only"
    );
    test_check(
        ui_design_child_on_line(source, -1, 1, 0) == ROOT &&
            ui_design_child_on_line(source, ROOT, 3, 0) == ROW &&
            ui_design_child_on_line(source, ROW, 4, 0) == OK &&
            ui_design_child_on_line(source, ROOT, 4, 0) == -1 &&
            ui_design_child_on_line(source, ROW, 4, 1) == -1,
        "Selection: a shown element's line and place under its parent name one "
        "outline index"
    );
    test_check(
        ui_design_element_at(source, offset_of(RML, "OK")) == OK &&
            ui_design_element_at(source, offset_of(RML, "    <button id=\"ok\"")) == OK &&
            ui_design_element_at(source, offset_of(RML, "  </div>")) == ROW &&
            ui_design_element_at(source, offset_of(RML, "class=\"title\"")) == TITLE,
        "Selection: the Text Editor's cursor selects the element it is in, or "
        "the one its line's space leads to"
    );
    const char *insert = "  <p>New</p>\n";
    int at = offset_of(RML, "  <div class=\"row\">");
    size_t length = strlen(RML) + strlen(insert) + 1;
    char *typed = malloc(length);
    snprintf(typed, length, "%.*s%s%s", at, RML, insert, RML + at);
    ui_source *after = ui_source_parse(typed, UI_SOURCE_RML);
    test_check(
        ui_design_follow(source, after, TITLE) == TITLE &&
            ui_design_follow(source, after, ROW) == ROW + 1 &&
            ui_design_follow(source, after, CANCEL) == CANCEL + 1 &&
            ui_design_follow(source, after, ROOT) == ROOT,
        "Selection: an undo or typing elsewhere leaves the selection on its "
        "element"
    );
    ui_source_free(after);
    free(typed);
    at = offset_of(RML, "title\"") + 5;
    length = strlen(RML) + 5;
    typed = malloc(length);
    snprintf(typed, length, "%.*s big%s", at, RML, RML + at);
    after = ui_source_parse(typed, UI_SOURCE_RML);
    test_check(
        ui_design_follow(source, after, TITLE) == TITLE &&
            ui_design_follow(source, after, OK) == OK,
        "Selection: a change inside the selected element's start tag keeps it "
        "selected"
    );
    ui_source_free(after);
    free(typed);
    ui_source_free(source);
}

/* A shown document: two elements on one line, a widget whose own element
 * is not the document's, and the use's child in the widget's slot. */
static const char
    CARD_RML[] = "<div id=\"card\">\n"
                 "  <span class=\"a\">A</span><span class=\"b\">B</span>\n"
                 "  <use ui=\"Chip\" id=\"chip\">\n"
                 "    <span class=\"inside\">Slot</span>\n"
                 "  </use>\n"
                 "</div>\n";
static const char CARD_RCSS
    [] = "#card { display: block; width: 300dp; height: 200dp; }\n"
         "[data-ui=Chip] { display: inline; }\n"
         "span { display: inline-block; width: 20dp; height: 20dp; }\n";

static const char CHIP_RML[] = "<div class=\"chip\">\n"
                               "  <span class=\"dot\"/>\n"
                               "  <slot/>\n"
                               "</div>\n";
static const char
    CHIP_RCSS[] = ".chip { display: block; width: 100dp; height: 40dp; }\n";

static rml_element *query(rml_context *context, const char *selector)
{
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_query_selector(rml_context_document(context, i), selector);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static void shown_maps(const char *folder)
{
    char ui[PATH_MAX], path[PATH_MAX];
    snprintf(ui, sizeof ui, "%s/ui", folder);
    assert(mkdir(ui, 0700) == 0);
    test_write_ui(ui, "Card", CARD_RML, CARD_RCSS);
    test_write_ui(ui, "Chip", CHIP_RML, CHIP_RCSS);
    ecs_world *world = ecs_world_new();
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    ui_world w = { .ecs = world };
    render_ui *shown = authoring_ui_new(folder, NULL);
    render_ui_set_overlay(shown, "Card", "dark");
    render_ui_update(shown, &w, 1280, 720, 1.0 / 60);
    rml_context *context = render_ui_context(shown);
    char error[256];
    snprintf(path, sizeof path, "%s/Card.rml", ui);
    ui_buffer *b = ui_buffer_open(path, error, sizeof error);
    assert(b);
    ui_source *rml = ui_design_parse(b, UI_PART_RML);
    bool round_trip = acount(rml->elements) == 5;
    for (int i = 0; i < acount(rml->elements); i++) {
        rml_element *el = studio_designer_shown(context, rml, "Card", i);
        round_trip &= el && studio_designer_shown_source(context, rml, "Card", el) == i;
    }
    test_check(round_trip, "Selection: every element of the document maps to its shown element and back by data-src");
    rml_element *a = query(context, ".a"), *b_span = query(context, ".b"),
                *dot = query(context, ".dot"), *inside = query(context, ".inside");
    test_check(
        studio_designer_shown_source(context, rml, "Card", a) == 1 &&
            studio_designer_shown_source(context, rml, "Card", b_span) == 2 &&
            studio_designer_shown_source(context, rml, "Card", dot) == 3 &&
            studio_designer_shown_source(context, rml, "Card", inside) == 4,
        "Selection: elements on one line stay apart, a widget's own element "
        "picks its use, and slot content its own element"
    );
    rml_element *use = studio_designer_shown(context, rml, "Card", 3);
    rml_element *chip = query(context, ".chip");
    assert(use && chip);
    kit_rect bounds = studio_designer_element_box(use);
    rml_vec2 wrapper_size = rml_element_box_size(use, RML_BOX_BORDER);
    rml_vec2 root_at = rml_element_absolute_offset(chip, RML_BOX_BORDER);
    rml_vec2 root_size = rml_element_box_size(chip, RML_BOX_BORDER);
    test_check(
        wrapper_size.x < root_size.x && bounds.w >= root_size.x &&
            bounds.h >= root_size.y && bounds.x <= root_at.x &&
            bounds.y <= root_at.y &&
            bounds.x + bounds.w >= root_at.x + root_size.x &&
            bounds.y + bounds.h >= root_at.y + root_size.y && root_size.x > 0 &&
            root_size.y > 0,
        "widget selection bounds enclose its rendered root even when the "
        "source wrapper is inline"
    );
    ui_source_free(rml);
    ui_buffer_free(b);
    render_ui_free(shown);
    ecs_world_free(world);
    test_remove_ui(ui, "Card");
    test_remove_ui(ui, "Chip");
    rmdir(ui);
}

void test_ui_designer()
{
    char pattern[] = "/tmp/chibi-ui-designer-XXXXXX", folder[PATH_MAX];
    assert(mkdtemp(pattern) && realpath(pattern, folder));
    moves(folder);
    structure(folder);
    attributes(folder);
    styles(folder);
    resize_math();
    keeps_indentation(folder);
    declarations(folder);
    extraction(folder);
    keyboard_slots(folder);
    selection_maps();
    shown_maps(folder);
    static const char *const NAMES[] = {
        "Moves",        "Structure", "Attributes", "Styles",       "Wide",
        "Declarations", "Lines",     "Extract",    "ExtractLines", "Slots",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++) {
        test_remove_ui(folder, NAMES[i]);
    }
    rmdir(folder);
}
