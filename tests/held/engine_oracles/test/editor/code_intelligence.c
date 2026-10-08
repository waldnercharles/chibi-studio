/* Code intelligence in the Text Editor without Studio (workstream I): a
 * part's problems as markers, completion accepted into the buffer, Go to
 * Definition's targets, hover descriptions, and one undo history over a code
 * editor and its buffer, in a scratch project on disk. */
#define _GNU_SOURCE
#include "editor/documents/ui_code.h"
#include "support/array.h"
#include "support/text_edit.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONTEXT "code intelligence test"

static const char *const BASE = ".panel { padding: var(--gap); }\n"
                                "body { --gap: 4dp; }\n";
static const char *const OTHER_RCSS = ".shared { color: #f00; }\n"
                                      ".shared .x { margin: 0dp; }\n";
static const char *const
    DOC_RML = "<use ui=\"Dialog\">\n"
              "  <div class=\"panel own shared\">{{ save.volume }}</div>\n"
              "</use>\n";
static const char *const DOC_RCSS = "@import \"theme/base.rcss\";\n"
                                    ".own { margin: var(--gap); }\n"
                                    ".own { colour: red; }\n";

typedef struct scratch
{
    char root[PATH_MAX]; /* the content root */
    char doc[PATH_MAX], dialog[PATH_MAX], base[PATH_MAX], other[PATH_MAX];
    ui_source_project project;
    ui_buffer *b;
} scratch;

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static int remove_entry(const char *path, const struct stat *, int, struct FTW *)
{
    return remove(path);
}

static scratch scratch_new()
{
    scratch s = { 0 };
    char pattern[] = "/tmp/chibi-code-XXXXXX", folder[PATH_MAX];
    assert(mkdtemp(pattern) && realpath(pattern, s.root));
    snprintf(folder, sizeof folder, "%s/ui", s.root);
    assert(mkdir(folder, 0777) == 0);
    snprintf(folder, sizeof folder, "%s/ui/theme", s.root);
    assert(mkdir(folder, 0777) == 0);
    snprintf(s.doc, sizeof s.doc, "%s/ui/Doc.rml", s.root);
    snprintf(s.dialog, sizeof s.dialog, "%s/ui/Dialog.rml", s.root);
    snprintf(s.base, sizeof s.base, "%s/ui/theme/base.rcss", s.root);
    snprintf(s.other, sizeof s.other, "%s/ui/Other.rml", s.root);
    snprintf(folder, sizeof folder, "%s/ui", s.root);
    test_write_ui(folder, "Doc", DOC_RML, DOC_RCSS);
    test_write_ui(folder, "Dialog", "<div class=\"dialog\"><slot/></div>\n", NULL);
    test_write_ui(folder, "Other", "<div class=\"shared\">x</div>\n", OTHER_RCSS);
    write_file(s.base, BASE);
    ui_code_scan(&s.project, s.root);
    char error[256];
    s.b = ui_buffer_open(s.doc, error, sizeof error);
    assert(s.b);
    return s;
}

static void scratch_free(scratch *s)
{
    ui_buffer_free(s->b);
    ui_source_project_free(&s->project);
    assert(nftw(s->root, remove_entry, 8, FTW_DEPTH | FTW_PHYS) == 0);
}

/* The offset of the `nth` `needle` in the text, from 0. */
static int offset_of(const char *text, const char *needle, int nth)
{
    const char *at = strstr(text, needle);
    for (; at && nth > 0; nth--) {
        at = strstr(at + 1, needle);
    }
    assert(at);
    return (int)(at - text);
}

/* ---------- Problems ---------- */

static const kit_code_marker *find_marker(
    const kit_code_marker *markers,
    int line,
    kit_code_severity severity,
    const char *text
)
{
    for (int i = 0; i < acount(markers); i++) {
        if (markers[i].line == line && markers[i].severity == severity &&
            strstr(markers[i].message, text)) {
            return &markers[i];
        }
    }
    return NULL;
}

static bool has_marker(const kit_code_marker *markers, int line, kit_code_severity severity, const char *text)
{
    return find_marker(markers, line, severity, text) != NULL;
}

/* Workstream Q: whether the marker underlines exactly `needle` in `text`. */
static bool underlines(const kit_code_marker *m, const char *text, const char *needle)
{
    return m && m->end - m->start == (int)strlen(needle) &&
           !strncmp(text + m->start, needle, strlen(needle));
}

static void problems(scratch *s)
{
    ui_source *broken = ui_source_parse("<div class=\"a\">\n  <span class=></span>\n", UI_SOURCE_RML);
    kit_code_marker *markers = NULL;
    const ui_check_message messages[] = {
        { UI_CHECK_RML, NULL, 1, "the first line\n  \"expr\"\n  ^" },
        { UI_CHECK_RCSS, NULL, 1, "about the rcss" },
        { UI_CHECK_WIDGETS, "Dialog", 3, "inside the widget" },
    };
    ui_check_message *listed = NULL;
    for (size_t i = 0; i < sizeof messages / sizeof *messages; i++) {
        apush(listed, messages[i]);
    }
    const char *markup = ui_buffer_text(s->b, UI_PART_RML);
    ui_source *rml = ui_source_parse(markup, UI_SOURCE_RML);
    ui_code_markers(broken, UI_PART_RML, NULL, NULL, &markers);
    test_check(acount(markers) > 0 && markers[0].severity == KIT_CODE_ERROR, "a syntax error marks its line as an error");
    test_check(
        acount(markers) > 0 && underlines(&markers[0], broken->text, "<div class=\"a\">"),
        "a syntax error underlines the range tree-sitter gives it"
    );
    ui_code_markers_free(markers);
    markers = NULL;
    ui_code_markers(rml, UI_PART_RML, listed, "no widget \"Dialog\" here", &markers);
    test_check(
        has_marker(markers, 1, KIT_CODE_WARNING, "the first line") &&
            !has_marker(markers, 1, KIT_CODE_WARNING, "expr") &&
            !has_marker(markers, 1, KIT_CODE_WARNING, "about the rcss"),
        "RmlUi's warnings mark the part's lines, their first line each"
    );
    test_check(
        has_marker(markers, 1, KIT_CODE_WARNING, "Dialog: inside the widget") &&
            has_marker(markers, 1, KIT_CODE_ERROR, "no widget"),
        "a widget's warning, and why the asset cannot be checked, mark where "
        "the part names the widget"
    );
    test_check(
        underlines(find_marker(markers, 1, KIT_CODE_WARNING, "Dialog: inside"), markup, "ui=\"Dialog\"") &&
            underlines(find_marker(markers, 1, KIT_CODE_ERROR, "no widget"), markup, "Dialog"),
        "they underline the text naming the widget, not the whole line"
    );
    ui_code_markers_free(markers);
    afree(listed);
    ui_source_free(rml);
    ui_source_free(broken);
    /* RmlUi itself, on the document as it stands. */
    ui_check_message *checked = NULL;
    char error[256] = "";
    rml_backend *checker = rml_backend_create(64, 64, 1);
    test_check(
        ui_code_check(s->b, rml_backend_context(checker), &checked, error, sizeof error),
        "RmlUi checks a ui document's text"
    );
    rml_backend_destroy(checker);
    ui_source *rcss = ui_source_parse(ui_buffer_text(s->b, UI_PART_RCSS), UI_SOURCE_RCSS);
    markers = NULL;
    ui_code_markers(rcss, UI_PART_RCSS, checked, NULL, &markers);
    test_check(has_marker(markers, 3, KIT_CODE_WARNING, "colour"), "an unknown property marks its line of the RCSS");
    test_check(
        underlines(find_marker(markers, 3, KIT_CODE_WARNING, "colour"), rcss->text, "colour: red;"),
        "RmlUi's warning underlines the declaration it quotes"
    );
    ui_code_markers_free(markers);
    ui_source_free(rcss);
    ui_check_messages_free(checked);
}

/* Workstream Q: a stylesheet gets RmlUi's check too, its own lines and,
   at the import naming it, what an import it reads logs. */
static void stylesheet_problems(scratch *s)
{
    char sheet[PATH_MAX], imported[PATH_MAX], error[256] = "";
    snprintf(sheet, sizeof sheet, "%s/ui/theme/sheet.rcss", s->root);
    snprintf(imported, sizeof imported, "%s/ui/theme/loud.rcss", s->root);
    write_file(imported, ".loud { colour: blue; }\n");
    write_file(sheet, "@import \"loud.rcss\";\n.a {\n  colour: red;\n}\n");
    ui_buffer *b = ui_buffer_open(sheet, error, sizeof error);
    assert(b);
    ui_check_message *checked = NULL;
    rml_backend *checker = rml_backend_create(64, 64, 1);
    bool ok = ui_code_check(b, rml_backend_context(checker), &checked, error, sizeof error);
    rml_backend_destroy(checker);
    const char *text = ui_buffer_text(b, UI_PART_RCSS);
    ui_source *rcss = ui_source_parse(text, UI_SOURCE_RCSS);
    kit_code_marker *markers = NULL;
    ui_code_markers(rcss, UI_PART_RCSS, checked, NULL, &markers);
    test_check(ok && underlines(find_marker(markers, 3, KIT_CODE_WARNING, "colour"), text, "colour: red;"), "a stylesheet's unknown property marks its line, underlining the declaration");
    test_check(
        underlines(find_marker(markers, 1, KIT_CODE_WARNING, "loud.rcss: "), text, "loud.rcss"),
        "an imported stylesheet's warning marks the import"
    );
    ui_code_markers_free(markers);
    ui_source_free(rcss);
    ui_check_messages_free(checked);
    ui_buffer_free(b);
    remove(sheet);
    remove(imported);
}

/* ---------- Navigation ---------- */

static bool at_place(const ui_code_place *p, const char *path, ui_part part, int offset)
{
    bool ok = !strcmp(p->path, path) && p->part == part && p->offset == offset;
    if (!ok) {
        fprintf(
            stderr,
            "  place %s part %d offset %d, expected %s part %d offset %d\n",
            p->path,
            p->part,
            p->offset,
            path,
            part,
            offset
        );
    }
    return ok;
}

static void definitions(scratch *s)
{
    const char *markup = ui_buffer_text(s->b, UI_PART_RML);
    const char *style = ui_buffer_text(s->b, UI_PART_RCSS);
    ui_source *rml = ui_source_parse(markup, UI_SOURCE_RML);
    ui_source *rcss = ui_source_parse(style, UI_SOURCE_RCSS);
    ui_code_place place;
    s->b->part = UI_PART_RML;
    test_check(
        ui_code_definition(s->b, rml, offset_of(markup, "Dialog", 0) + 1, &s->project, &place) &&
            at_place(&place, s->dialog, UI_PART_RML, 0),
        "a <use ui> widget goes to its ui asset"
    );
    test_check(
        ui_code_definition(s->b, rml, offset_of(markup, "panel", 0), &s->project, &place) &&
            at_place(&place, s->base, UI_PART_RCSS, 1) && place.line == 1,
        "a class goes to the stylesheet rule that selects it"
    );
    test_check(
        ui_code_definition(s->b, rml, offset_of(markup, "own", 0), &s->project, &place) &&
            at_place(&place, s->doc, UI_PART_RCSS, offset_of(style, "own", 0)),
        "a class goes to the document's own RCSS, as it stands"
    );
    test_check(
        ui_code_definition(s->b, rml, offset_of(markup, "shared", 0), &s->project, &place) &&
            at_place(&place, s->other, UI_PART_RCSS, 1),
        "a class goes to another asset's RCSS, in that file's text"
    );
    s->b->part = UI_PART_RCSS;
    test_check(
        ui_code_definition(s->b, rcss, offset_of(style, "--gap", 0) + 2, &s->project, &place) &&
            at_place(&place, s->base, UI_PART_RCSS, offset_of(BASE, "--gap", 1)),
        "var(--token) goes to the token's declaration"
    );
    test_check(
        !ui_code_definition(s->b, rcss, offset_of(style, "margin", 0), &s->project, &place),
        "a property has no definition to go to"
    );
    /* Find References: definitions first, each in its part's text. */
    s->b->part = UI_PART_RML;
    ui_code_place *places = NULL;
    ui_source_symbol
        symbol = ui_code_places(s->b, rml, offset_of(markup, "shared", 0), &s->project, &places);
    int definitions = 0;
    for (int i = 0; i < acount(places); i++) {
        definitions += places[i].definition;
    }
    test_check(
        symbol.kind == UI_SYMBOL_CLASS && acount(places) == 4 &&
            definitions == 2 && places[0].definition && places[1].definition &&
            at_place(&places[1], s->other, UI_PART_RCSS, offset_of(OTHER_RCSS, "shared", 1)) &&
            places[1].line == 2,
        "Find References lists every place, definitions first, each in its "
        "file's text"
    );
    afree(places);
    ui_source_free(rcss);
    ui_source_free(rml);
}

static void hovers(scratch *s)
{
    const char *markup = ui_buffer_text(s->b, UI_PART_RML);
    const char *style = ui_buffer_text(s->b, UI_PART_RCSS);
    ui_source *rml = ui_source_parse(markup, UI_SOURCE_RML);
    ui_source *rcss = ui_source_parse(style, UI_SOURCE_RCSS);
    char *panel = ui_source_hover(rml, offset_of(markup, "panel", 0), &s->project);
    char *widget = ui_source_hover(rml, offset_of(markup, "Dialog", 0), &s->project);
    char *token = ui_source_hover(rcss, offset_of(style, "--gap", 0), &s->project);
    char *property = ui_code_hover(rcss, offset_of(style, "margin", 0), &s->project);
    ui_source *inherits = ui_source_parse(".a { color: red; }", UI_SOURCE_RCSS);
    char *inherited = ui_code_hover(inherits, 5, &s->project);
    test_check(
        inherited && strstr(inherited, "Property color\nRCSS property\nInitial") &&
            strstr(inherited, "inherited"),
        "a property's card adds RmlUi's initial value and whether it inherits"
    );
    free(inherited);
    ui_source_free(inherits);
    char *unknown = ui_source_hover(rcss, offset_of(style, "colour", 0), &s->project);
    char *nothing = ui_source_hover(rcss, offset_of(style, "{", 0), &s->project);
    test_check(
        panel && !strncmp(panel, "Class .panel\n", 13) &&
            strstr(panel, "ui/theme/base.rcss:1  .panel"),
        "a class describes the rules that select it"
    );
    test_check(
        widget && !strncmp(widget, "Widget Dialog\n", 14) &&
            strstr(widget, "ui/Dialog.rml"),
        "a widget names its asset"
    );
    test_check(
        token && !strncmp(token, "Custom property --gap\n", 22) &&
            strstr(token, "--gap: 4dp;"),
        "a token quotes its declaration"
    );
    test_check(
        property && strstr(property, "Property margin\nRCSS property\nA shorthand"),
        "a property RmlUi knows says so, a shorthand too"
    );
    test_check(unknown && strstr(unknown, "Not a property"), "and one it does not, too");
    test_check(!nothing, "punctuation names nothing");
    free(panel);
    free(widget);
    free(token);
    free(property);
    free(unknown);
    free(nothing);
    ui_source_free(rcss);
    ui_source_free(rml);
}

/* ---------- The editor over its buffer ---------- */

static uintptr_t compile_geometry(void *, rml_mesh)
{
    return 1;
}

static void render_geometry(void *, uintptr_t, rml_vec2, uintptr_t) {}
static void release_geometry(void *, uintptr_t) {}

static uintptr_t load_texture(void *, const char *, int *width, int *height)
{
    *width = *height = 1;
    return 1;
}

static uintptr_t generate_texture(void *, const uint8_t *, int, int)
{
    return 1;
}

static void release_texture(void *, uintptr_t) {}
static void enable_scissor(void *, bool) {}
static void set_scissor(void *, rml_rect) {}
static void set_transform(void *, const float *) {}

static const rml_render_interface NOTHING = {
    compile_geometry, render_geometry,  release_geometry,
    load_texture,     generate_texture, release_texture,
    enable_scissor,   set_scissor,      set_transform,
};

typedef struct editing
{
    rml_renderer *renderer;
    rml_context *context;
    kit *k;
    kit_frame_input in;
    kit_code_editor *e;
    char *synced; /* the text the editor and the buffer last shared */
    kit_code_desc desc;
    unsigned events;
} editing;

static void frame(editing *f)
{
    kit_begin(f->k, &f->in);
    kit_root(f->k, "app");
    kit_code_result r = kit_code(f->k, "code", f->e, &f->desc);
    kit_style(f->k, "width", "480dp");
    kit_style(f->k, "height", "240dp");
    kit_focus(f->k);
    kit_end(f->k);
    rml_context_update(f->context);
    rml_context_render(f->context);
    f->in.time += 1.0 / 60.0;
    f->events = r.events;
}

static void press(editing *f, rml_key key, unsigned modifiers)
{
    rml_context_process_key_down(f->context, key, modifiers);
    rml_context_process_key_up(f->context, key, modifiers);
}

static void type(editing *f, const char *text)
{
    rml_context_process_text_input(f->context, text, strlen(text));
}

/* As the Text Editor answers a frame: typing and completions into the
   buffer, the undo keys through it, and the buffer back to the editor. */
static void answer(editing *f, ui_buffer *b)
{
    if (f->events & (KIT_CODE_CHANGED | KIT_CODE_COMPLETED)) {
        ui_code_record(b, b->part, f->e, f->synced, f->in.time, f->events & KIT_CODE_COMPLETED);
    }
    bool replayed = (f->events & (KIT_CODE_UNDO | KIT_CODE_REDO)) &&
                    ui_code_replay(b, f->events & KIT_CODE_REDO);
    ui_code_follow(f->e, ui_buffer_text(b, b->part), replayed);
    free(f->synced);
    f->synced = xstrdup(ui_buffer_text(b, b->part));
}

static void step(editing *f, ui_buffer *b)
{
    frame(f);
    answer(f, b);
}

/* Workstream Q: a designer edit that reaches the buffer in the frame the
   editor was typed in, before the Text Editor records the typing, composes
   with it instead of being overwritten. */
static void same_frame(editing *f, ui_buffer *b)
{
    f->desc.vim = false;
    step(f, b);
    const char *text = ui_buffer_text(b, UI_PART_RML);
    kit_code_set_cursor(f->e, offset_of(text, "</use>", 0) + 6);
    step(f, b);
    type(f, "<p/>");
    frame(f);
    text_edit designer = { 4, 4, " id=\"d\"" };
    ui_buffer_edit(b, UI_PART_RML, &designer, 1);
    answer(f, b);
    const char *both = ui_buffer_text(b, UI_PART_RML);
    test_check(
        !strncmp(both, "<use id=\"d\" ui=\"Dialog\">", 24) &&
            strstr(both, "</use><p/>") && !strcmp(kit_code_text(f->e, NULL), both),
        "typing and a designer edit in the same frame compose, and the editor "
        "shows both"
    );
    ui_buffer_undo(b);
    step(f, b);
    both = ui_buffer_text(b, UI_PART_RML);
    test_check(
        !strncmp(both, "<use id=\"d\"", 11) && !strstr(both, "<p/>") &&
            !strcmp(kit_code_text(f->e, NULL), both),
        "the typing stays a step of its own after the designer's"
    );
}

static void formatting_keeps_editor_positions()
{
    static const char
        SOURCE[] = "<button data-event-click=\"emit('Cook', save.recipe)\" "
                   "data-tip=\"Serve the simmering moon mushrooms with the "
                   "last of the lamp oil\">"
                   "A <b>{{ save.dish }}</b> C</button>";
    char *formatted = ui_source_format(SOURCE, UI_SOURCE_RML);
    const char *token = strstr(SOURCE, "save.recipe");
    const char *formatted_token = strstr(formatted, "save.recipe");

    kit_code_editor *caret = kit_code_editor_create(SOURCE);
    int caret_offset = (int)(token - SOURCE) + 4;
    kit_code_set_cursor(caret, caret_offset);
    ui_code_follow(caret, formatted, false);
    bool caret_kept = kit_code_cursor(caret) == (int)(formatted_token - formatted) + 4;
    kit_code_editor_destroy(caret);

    kit_code_editor *selection = kit_code_editor_create(SOURCE);
    int selection_start = (int)(token - SOURCE) + 2;
    int selection_end = (int)(token - SOURCE) + 7;
    kit_code_select(selection, selection_start, selection_end);
    ui_code_follow(selection, formatted, false);
    int selected_start = -1, selected_end = -1;
    bool selection_kept = kit_code_selection(selection, &selected_start, &selected_end) &&
                          selected_start == (int)(formatted_token - formatted) + 2 &&
                          selected_end == (int)(formatted_token - formatted) + 7 &&
                          kit_code_cursor(selection) == selected_end;
    kit_code_editor_destroy(selection);
    test_check(caret_kept && selection_kept, "formatting maps the caret and selected bytes inside a quoted binding value");

    kit_code_editor *changed = kit_code_editor_create("<p>old</p>");
    ui_code_follow(changed, "<p>new</p>", false);
    bool token_change = !strcmp(kit_code_text(changed, NULL), "<p>new</p>");
    kit_code_editor_destroy(changed);
    test_check(token_change, "following a source token edit replaces the changed text");
    free(formatted);
}

static void one_history(scratch *s)
{
    ui_buffer *b = s->b;
    b->part = UI_PART_RML;
    editing f = { .in = { .dp_ratio = 1, .width = 800, .height = 600 } };
    f.renderer = rml_renderer_create(&NOTHING, NULL);
    f.context = rml_create_context(CONTEXT, 800, 600, f.renderer);
    f.k = test_kit_create(f.context, "studio.rml");
    const char *original = ui_buffer_text(b, UI_PART_RML);
    char *before = xstrdup(original);
    f.e = kit_code_editor_create(original);
    f.synced = xstrdup(original);
    f.desc.history = true;
    int end = offset_of(original, "</use>", 0) + 6;
    kit_code_set_cursor(f.e, end);
    step(&f, b);
    type(&f, "<use ui=\"Di");
    step(&f, b);
    test_check(strstr(ui_buffer_text(b, UI_PART_RML), "</use><use ui=\"Di") != NULL, "typing reaches the buffer as typed text");
    ui_source *parsed = ui_source_parse(kit_code_text(f.e, NULL), UI_SOURCE_RML);
    ui_code_completions found;
    ui_code_complete(parsed, kit_code_cursor(f.e), &s->project, &found);
    bool offered = acount(found.items) == 1 &&
                   !strcmp(found.items[0].label, "Dialog") &&
                   !strcmp(found.items[0].detail, "widget");
    test_check(offered, "completion offers the widgets a <use ui> can name");
    f.desc.completions = found.items;
    f.desc.completion_count = acount(found.items);
    f.desc.completion_start = found.start;
    frame(&f);
    press(&f, RML_KEY_RETURN, 0);
    step(&f, b);
    f.desc.completions = NULL;
    f.desc.completion_count = 0;
    test_check(
        strstr(ui_buffer_text(b, UI_PART_RML), "<use ui=\"Dialog") &&
            !strcmp(kit_code_text(f.e, NULL), ui_buffer_text(b, UI_PART_RML)),
        "an accepted completion reaches the buffer"
    );
    ui_code_completions_free(&found);
    ui_source_free(parsed);
    press(&f, RML_KEY_Z, RML_KM_CTRL);
    step(&f, b);
    test_check(
        strstr(ui_buffer_text(b, UI_PART_RML), "</use><use ui=\"Di\n") &&
            !strcmp(kit_code_text(f.e, NULL), ui_buffer_text(b, UI_PART_RML)) &&
            kit_code_cursor(f.e) == end + (int)strlen("<use ui=\"Di"),
        "the primary modifier and Z undo the completion alone, in the buffer, "
        "and the editor follows to the change"
    );
    press(&f, RML_KEY_Z, RML_KM_CTRL);
    step(&f, b);
    test_check(
        !strcmp(ui_buffer_text(b, UI_PART_RML), before) &&
            !strcmp(kit_code_text(f.e, NULL), before),
        "then the typing, in one history"
    );
    f.desc.vim = true;
    frame(&f);
    press(&f, RML_KEY_ESCAPE, 0);
    press(&f, RML_KEY_R, RML_KM_CTRL);
    step(&f, b);
    test_check(
        strstr(ui_buffer_text(b, UI_PART_RML), "<use ui=\"Di") && ui_buffer_dirty(b),
        "Vim's Control+R redoes from the same history"
    );
    type(&f, "gd");
    frame(&f);
    test_check(f.events & KIT_CODE_DEFINITION, "Vim gd asks the shared language service for a definition");
    type(&f, "gr");
    frame(&f);
    test_check(f.events & KIT_CODE_REFERENCES, "Vim gr asks for source references");
    type(&f, "gR");
    frame(&f);
    test_check(f.events & KIT_CODE_RENAME, "Vim gR requests semantic rename");
    type(&f, "gc");
    frame(&f);
    test_check(f.events & KIT_CODE_CALLS, "Vim gc requests callable hierarchy");
    press(&f, RML_KEY_O, RML_KM_CTRL);
    frame(&f);
    test_check(f.events & KIT_CODE_CHORD, "Vim Control+O delegates navigation history to Studio");
    press(&f, RML_KEY_I, RML_KM_CTRL);
    frame(&f);
    test_check(f.events & KIT_CODE_CHORD, "Vim Control+I delegates forward navigation to Studio");
    press(&f, RML_KEY_F9, 0);
    frame(&f);
    test_check(f.events & KIT_CODE_BREAKPOINT, "F9 requests a breakpoint at the source cursor");
    ui_buffer_undo(b);
    step(&f, b);
    test_check(!strcmp(kit_code_text(f.e, NULL), before), "Studio's Undo, stepping the buffer, reaches the editor");
    same_frame(&f, b);
    free(before);
    free(f.synced);
    kit_code_editor_destroy(f.e);
    rml_context_render(f.context);
    kit_destroy(f.k);
    rml_remove_context(CONTEXT);
    rml_release_render_managers();
    rml_renderer_destroy(f.renderer);
}

void test_code_intelligence()
{
    rml_backend *backend = rml_backend_create(64, 64, 1);
    scratch s = scratch_new();
    problems(&s);
    stylesheet_problems(&s);
    definitions(&s);
    hovers(&s);
    formatting_keeps_editor_positions();
    one_history(&s);
    scratch_free(&s);
    rml_backend_destroy(backend);
}
