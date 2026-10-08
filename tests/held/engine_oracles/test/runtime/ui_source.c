/* The ui language service: highlighting, outline, diagnostics, completion,
 * references and edits of ui markup and styles. */
#include "assets/ui_source.h"

#include "editor/documents/ui_buffer.h"
#include "support/array.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char SAMPLE_RML
    [] = "<div id=\"panel\" class=\"card wide\">\n"
         "  <span data-if=\"save.sound\">{{ save.name }}</span>\n"
         "  <use ui=\"MenuButton\" data-event-click=\"emit('Resume')\"/>\n"
         "</div>\n";
static const char SAMPLE_RCSS[] = "// tokens\n"
                                  ".card {\n"
                                  "  color: var(--primary);\n"
                                  "  &:hover { opacity: 0.5; }\n"
                                  "  .dark & { width: 10dp; }\n"
                                  "}\n";

static const char *const KIND_NAMES[UI_SOURCE_KIND_COUNT] = {
    "text",     "number",   "string",   "keyword", "name", "path",
    "punct",    "comment",  "error",    "warning", "tag",  "attribute",
    "selector", "property", "variable", "binding",
};

/* The spans as "kind:text" words, for comparing with a golden. */
static char *spans_text(const ui_source *s)
{
    size_t size = 1;
    for (int i = 0; i < acount(s->spans); i++) {
        size += 24 + (size_t)(s->spans[i].end - s->spans[i].start);
    }
    char *out = malloc(size);
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < acount(s->spans); i++) {
        const ui_source_span *span = &s->spans[i];
        used += (size_t)snprintf(
            out + used,
            size - used,
            "%s%s:%.*s",
            i ? " " : "",
            KIND_NAMES[span->kind],
            span->end - span->start,
            s->text + span->start
        );
    }
    return out;
}

static bool slice_is(const ui_source *s, int start, int end, const char *expected)
{
    return (size_t)(end - start) == strlen(expected) &&
           !strncmp(s->text + start, expected, (size_t)(end - start));
}

static char *slice_copy(const ui_source *source, int start, int end)
{
    char *text = calloc((size_t)(end - start) + 1, 1);
    memcpy(text, source->text + start, (size_t)(end - start));
    return text;
}

static bool same_rml_meaning(const ui_source *before, const ui_source *after)
{
    if (acount(before->elements) != acount(after->elements)) {
        return false;
    }
    for (int i = 0; i < acount(before->elements); i++) {
        const ui_source_element *a = &before->elements[i], *b = &after->elements[i];
        if (strcmp(a->tag, b->tag) || a->parent != b->parent ||
            a->self_closing != b->self_closing ||
            acount(a->attributes) != acount(b->attributes)) {
            return false;
        }
        for (int j = 0; j < acount(a->attributes); j++) {
            const ui_source_attribute *x = &a->attributes[j], *y = &b->attributes[j];
            char *xname = slice_copy(before, x->name_start, x->name_end);
            char *yname = slice_copy(after, y->name_start, y->name_end);
            char *xvalue = ui_source_attribute_value(before, i, xname);
            char *yvalue = ui_source_attribute_value(after, i, yname);
            bool same = !strcmp(xname, yname) && xvalue && yvalue &&
                        !strcmp(xvalue, yvalue);
            free(xname);
            free(yname);
            free(xvalue);
            free(yvalue);
            if (!same) {
                return false;
            }
        }
    }
    return true;
}

static bool same_rcss_meaning(const ui_source *before, const ui_source *after)
{
    if (acount(before->rules) != acount(after->rules)) {
        return false;
    }
    for (int i = 0; i < acount(before->rules); i++) {
        const ui_source_rule *a = &before->rules[i], *b = &after->rules[i];
        if (strcmp(a->selector, b->selector) || a->parent != b->parent ||
            acount(a->declarations) != acount(b->declarations)) {
            return false;
        }
        for (int j = 0; j < acount(a->declarations); j++) {
            const ui_source_declaration *x = &a->declarations[j],
                                        *y = &b->declarations[j];
            char *xp = slice_copy(before, x->property_start, x->property_end);
            char *yp = slice_copy(after, y->property_start, y->property_end);
            char *xv = slice_copy(before, x->value_start, x->value_end);
            char *yv = slice_copy(after, y->value_start, y->value_end);
            bool same = !strcmp(xp, yp) && !strcmp(xv, yv);
            free(xp);
            free(yp);
            free(xv);
            free(yv);
            if (!same) {
                return false;
            }
        }
    }
    return true;
}

static void formatting()
{
    static const char RML[] = "<!-- keep this note -->\n"
                              "<button data-event-click=\"emit('Cook', "
                              "save.recipe)\" data-tip=\"Serve the simmering "
                              "moon mushrooms with the last of the lamp oil\">"
                              "A <b>{{ save.dish }}</b> C</button>\n";
    static const char RCSS
        [] = "/* recipe tokens */.panel{color:var(--cream);padding:calc(1dp + "
             "2dp);--recipe:  \"A,  B\"  ;&:hover{opacity:.5}}";
    char *rml = ui_source_format(RML, UI_SOURCE_RML);
    char *rcss = ui_source_format(RCSS, UI_SOURCE_RCSS);
    ui_source *before_rml = ui_source_parse(RML, UI_SOURCE_RML);
    ui_source *after_rml = ui_source_parse(rml, UI_SOURCE_RML);
    ui_source *before_rcss = ui_source_parse(RCSS, UI_SOURCE_RCSS);
    ui_source *after_rcss = ui_source_parse(rcss, UI_SOURCE_RCSS);
    char *again = ui_source_format(rcss, UI_SOURCE_RCSS);
    bool rml_ok = !acount(before_rml->diagnostics) &&
                  !acount(after_rml->diagnostics) &&
                  same_rml_meaning(before_rml, after_rml) &&
                  strstr(rml, "button\n    data-event-click=") &&
                  strstr(rml, "<!-- keep this note -->") &&
                  strstr(rml, "A <b>{{ save.dish }}</b> C");
    bool rcss_ok = !acount(before_rcss->diagnostics) &&
                   !acount(after_rcss->diagnostics) &&
                   same_rcss_meaning(before_rcss, after_rcss) &&
                   strstr(rcss, "/* recipe tokens */") &&
                   strstr(rcss, "--recipe:  \"A,  B\"  ;") && !strcmp(rcss, again);
    test_check(rml_ok && rcss_ok, "formatting preserves parsed RML and RCSS, comments, bindings and mixed text, and is idempotent");

    static const char BAD_RML[] = "<div><span>unfinished";
    static const char BAD_RCSS[] = ".panel { color: red";
    char *bad_rml = ui_source_format(BAD_RML, UI_SOURCE_RML);
    char *bad_rcss = ui_source_format(BAD_RCSS, UI_SOURCE_RCSS);
    bool invalid_ok = !strcmp(BAD_RML, bad_rml) && !strcmp(BAD_RCSS, bad_rcss);
    ui_buffer *buffer = calloc(1, sizeof *buffer);
    buffer->parts[UI_PART_RML] = strdup(BAD_RML);
    buffer->parts[UI_PART_RCSS] = strdup(BAD_RCSS);
    bool no_edit = !ui_buffer_format(buffer, UI_PART_RML) && !acount(buffer->undo) &&
                   !strcmp(BAD_RML, buffer->parts[UI_PART_RML]);
    ui_buffer_free(buffer);

    buffer = calloc(1, sizeof *buffer);
    buffer->parts[UI_PART_RML] = strdup(RML);
    buffer->parts[UI_PART_RCSS] = strdup(RCSS);
    bool one_undo = ui_buffer_format(buffer, UI_PART_RCSS) &&
                    acount(buffer->undo) == 1;
    ui_buffer_undo(buffer);
    one_undo = one_undo && !strcmp(RCSS, buffer->parts[UI_PART_RCSS]);
    ui_buffer_free(buffer);
    buffer = calloc(1, sizeof *buffer);
    buffer->files[UI_PART_RML] = strdup("test.rml");
    buffer->files[UI_PART_RCSS] = strdup("test.rcss");
    buffer->parts[UI_PART_RML] = strdup(RML);
    buffer->parts[UI_PART_RCSS] = strdup(RCSS);
    bool save_step = ui_buffer_format_for_save(buffer) &&
                     acount(buffer->undo) == 1 && buffer->part == UI_PART_RML;
    ui_buffer_undo(buffer);
    save_step = save_step && !strcmp(RML, buffer->parts[UI_PART_RML]) &&
                !strcmp(RCSS, buffer->parts[UI_PART_RCSS]);
    ui_buffer_free(buffer);
    test_check(invalid_ok && no_edit, "invalid source remains byte-identical and never adds an undo step");
    test_check(one_undo, "formatting a source part is one undoable buffer edit");
    test_check(save_step, "formatting both saved source parts joins one undoable save step");

    free(rml);
    free(rcss);
    free(again);
    free(bad_rml);
    free(bad_rcss);
    ui_source_free(before_rml);
    ui_source_free(after_rml);
    ui_source_free(before_rcss);
    ui_source_free(after_rcss);
}

/* The spans of `text` parsed as `language` match `expected`. */
static bool highlights(const char *text, ui_source_language language, const char *expected)
{
    ui_source *s = ui_source_parse(text, language);
    char *spans = spans_text(s);
    bool ok = !strcmp(spans, expected);
    if (!ok) {
        printf("  spans: %s\n", spans);
    }
    free(spans);
    ui_source_free(s);
    return ok;
}

static void highlighting()
{
    bool ok = highlights(
                  "<p class=\"a\" style=\"width: 2dp\">{{ save.x + 1 }}</p>\n",
                  UI_SOURCE_RML,
                  "punct:< tag:p attribute:class punct:= string:\" "
                  "string:a string:\" attribute:style punct:= string:\" "
                  "property:width "
                  "punct:: number:2dp string:\" punct:> punct:{{ binding:save "
                  "punct:. "
                  "binding:x punct:+ number:1 punct:}} punct:</ tag:p punct:>"
              ) &&
              highlights(
                  ".a { color: #fff; } /* c */\n",
                  UI_SOURCE_RCSS,
                  "selector:. selector:a punct:{ property:color punct:: "
                  "number:#fff punct:; punct:} comment:/* c */"
              );
    test_check(ok, "ui source highlights RML tags, attributes, bindings and inline style, and RCSS, in file offsets");
    static const char ATTRIBUTE_IF[] = "data-attrif-data-disabled";
    test_check(
        ui_source_is_binding(ATTRIBUTE_IF, (int)strlen(ATTRIBUTE_IF)) &&
            !ui_source_is_binding("data-tip", (int)strlen("data-tip")),
        "RmlUi's data-attrif- is a binding and a plain data- attribute is not"
    );
    test_check(
        !strcmp(ui_source_kind_class(UI_SOURCE_TAG), "tok-keyword tok-tag") &&
            !strcmp(ui_source_kind_class(UI_SOURCE_STRING), "tok-string") &&
            !ui_source_kind_class(UI_SOURCE_TEXT),
        "ui source spans name highlight.c's tok-* classes and their own"
    );

    ui_source *markup = ui_source_parse(SAMPLE_RML, UI_SOURCE_RML);
    ui_source *styles = ui_source_parse(SAMPLE_RCSS, UI_SOURCE_RCSS);
    char *rml = spans_text(markup), *rcss = spans_text(styles);
    ok = strstr(rcss, "comment:// tokens") &&
         strstr(rcss, "keyword:& keyword:: keyword:hover") &&
         strstr(rcss, "variable:--primary") &&
         strstr(rml, "name:emit punct:( string:'Resume'") &&
         strstr(rcss, "selector:. selector:dark keyword:&");
    if (!ok) {
        printf("  spans: %s\n  %s\n", rml, rcss);
    }
    test_check(ok, "ui source highlights line comments, nesting, var() and emit()");
    free(rml);
    free(rcss);
    ui_source_free(styles);
    ui_source_free(markup);
}

static void outline()
{
    ui_source *s = ui_source_parse(SAMPLE_RML, UI_SOURCE_RML);
    const ui_source_element *e = s->elements;
    bool elements = acount(s->elements) == 3 && !strcmp(e[0].tag, "div") &&
                    !strcmp(e[0].id, "panel") &&
                    !strcmp(e[0].classes, "card wide") && e[0].parent == -1 &&
                    e[0].line == 1 && !strcmp(e[1].tag, "span") &&
                    e[1].parent == 0 && e[1].line == 2 && !strcmp(e[2].tag, "use") &&
                    e[2].self_closing && e[2].parent == 0 && e[2].line == 3;
    bool ranges = elements &&
                  slice_is(s, e[1].start, e[1].end, "<span data-if=\"save.sound\">{{ save.name }}</span>") &&
                  slice_is(s, e[1].start, e[1].open_end, "<span data-if=\"save.sound\">") &&
                  slice_is(s, e[1].close_start, e[1].end, "</span>") &&
                  slice_is(s, e[0].close_start, e[0].end, "</div>") &&
                  acount(e[2].attributes) == 2 &&
                  slice_is(s, e[2].attributes[1].name_start, e[2].attributes[1].name_end, "data-event-click") &&
                  slice_is(s, e[2].attributes[1].value_start, e[2].attributes[1].value_end, "emit('Resume')");
    test_check(elements && ranges, "ui source outlines elements with tag, id, classes, parent, lines and byte ranges");
    char *value = elements ? ui_source_attribute_value(s, 1, "data-if") : NULL;
    test_check(value && !strcmp(value, "save.sound"), "ui source reads an attribute's value");
    free(value);

    int div = ui_source_element_at(s, e[1].start + 3);
    test_check(div == 1 && ui_source_element_at(s, e[0].start + 1) == 0, "ui source finds the innermost element at an offset");
    int line, column;
    ui_source_position(s, e[1].start, &line, &column);
    test_check(
        line == 2 && column == 3 && ui_source_offset(s, 2, 3) == e[1].start,
        "ui source converts offsets to lines and columns and back"
    );
    test_check(!acount(s->diagnostics), "valid markup has no diagnostics");
    ui_source_free(s);

    s = ui_source_parse(SAMPLE_RCSS, UI_SOURCE_RCSS);
    const ui_source_rule *r = s->rules;
    bool rules = acount(s->rules) == 3 && !strcmp(r[0].selector, ".card") &&
                 r[0].parent == -1 && r[0].line == 2 &&
                 !strcmp(r[1].selector, "&:hover") && r[1].parent == 0 &&
                 !strcmp(r[2].selector, ".dark &") && r[2].parent == 0 &&
                 r[2].line == 5 && acount(r[0].declarations) == 1 &&
                 slice_is(
                     s,
                     r[0].declarations[0].property_start,
                     r[0].declarations[0].property_end,
                     "color"
                 ) &&
                 slice_is(
                     s,
                     r[0].declarations[0].value_start,
                     r[0].declarations[0].value_end,
                     "var(--primary)"
                 ) &&
                 slice_is(s, r[1].start, r[1].end, "&:hover { opacity: 0.5; }");
    test_check(rules, "ui source outlines nested rules with selectors, declarations and ranges");
    test_check(!acount(s->diagnostics), "valid styles have no diagnostics, Sass nesting included");
    ui_source_free(s);

    s = ui_source_parse(":root { --x: 1dp; }\n.a { width: var(--x); }\n", UI_SOURCE_RCSS);
    test_check(
        acount(s->rules) == 2 && s->language == UI_SOURCE_RCSS && !acount(s->diagnostics),
        "ui source parses a whole stylesheet"
    );
    ui_source_free(s);
}

/* The diagnostics of `text` as "line:column message", joined by "; ". */
static bool diagnoses(const char *text, ui_source_language language, const char *expected)
{
    ui_source *s = ui_source_parse(text, language);
    char got[512] = "";
    size_t used = 0;
    for (int i = 0; i < acount(s->diagnostics); i++) {
        const ui_source_diagnostic *d = &s->diagnostics[i];
        used += (size_t)snprintf(
            got + used,
            sizeof got - used,
            "%s%d:%d %s",
            i ? "; " : "",
            d->line,
            d->column,
            d->message
        );
    }
    bool ok = !strcmp(got, expected);
    if (!ok) {
        printf("  diagnostics %s\n", got);
    }
    ui_source_free(s);
    return ok;
}

static void diagnostics()
{
    bool ok = diagnoses(
                  "<div>\n  <span>x</div>\n</span>\n",
                  UI_SOURCE_RML,
                  "2:3 <span> is not closed; 3:1 </span> closes no open element"
              ) &&
              diagnoses(".a { color: red; }\n.b { color: ; \n", UI_SOURCE_RCSS, "2:1 unexpected \".b { color: ;\"");
    test_check(ok, "ui source reports markup and style syntax errors at their lines");

    static const char
        TERNARIES[] = "<div>\n"
                      "  <b>{{ save.ekX > 0 ? save.ekX : 0 }}</b>\n"
                      "  <b>{{ save.ekH > 0 ? save.ekH : 0 }}</b>\n"
                      "</div>\n";
    ui_source *source = ui_source_parse(TERNARIES, UI_SOURCE_RML);
    test_check(!acount(source->diagnostics) && acount(source->elements) == 3, "RML text bindings can contain comparison operators without hiding source diagnostics");
    ui_source_free(source);

    source = ui_source_parse("{{ save.count > 0 ? save.count : 0 }}", UI_SOURCE_RML);
    char *tokens = spans_text(source);
    bool binding = !acount(source->diagnostics) &&
                   strstr(tokens, "binding:save") && strstr(tokens, "punct:>");
    test_check(binding, "a pure text binding remains a highlighted RML text node");
    free(tokens);
    ui_source_free(source);
}

static ui_source_project sample_project()
{
    ui_source_project p = { 0 };
    const char *const widgets[] = { "MenuButton", "MenuCheck", "TextField" };
    const char *const classes[] = { "card", "wide", "title" };
    const char *const variables[] = { "--primary", "--muted" };
    const char *const properties[] = { "color", "opacity", "width", "height" };
    const char *const keys[] = { "sound", "name", "theme" };
    for (int i = 0; i < 3; i++) {
        ui_source_list_add(&p.widgets, widgets[i]);
        ui_source_list_add(&p.classes, classes[i]);
        ui_source_list_add(&p.save_keys, keys[i]);
    }
    for (int i = 0; i < 2; i++) {
        ui_source_list_add(&p.variables, variables[i]);
    }
    for (int i = 0; i < 4; i++) {
        ui_source_list_add(&p.properties, properties[i]);
    }
    return p;
}

/* The labels completion offers where `text` has its '^', joined by spaces,
 * and the word it replaces. */
static void complete_at(
    const char *marked,
    ui_source_language language,
    const ui_source_project *p,
    char *out,
    size_t size
)
{
    const char *bar = strchr(marked, '^');
    assert(bar);
    char text[512];
    snprintf(text, sizeof text, "%.*s%s", (int)(bar - marked), marked, bar + 1);
    ui_source *s = ui_source_parse(text, language);
    ui_source_completions c;
    ui_source_complete(s, (int)(bar - marked), p, &c);
    size_t used = (size_t)snprintf(out, size, "[%.*s]", c.end - c.start, text + c.start);
    for (int i = 0; i < acount(c.items); i++) {
        used += (size_t)snprintf(out + used, size - used, " %s", c.items[i].label);
    }
    ui_source_completions_free(&c);
    ui_source_free(s);
}

static void completion()
{
    ui_source_project p = sample_project();
    static const struct
    {
        const char *text;
        ui_source_language language;
        const char *expected;
    } cases[] = {
        { "<sp^", UI_SOURCE_RML, "[sp] span" },
        { "<div data-i^", UI_SOURCE_RML, "[data-i] data-if data-alias- data-event-click data-event-dblclick data-event-input data-event-submit data-visible" },
        { "<div data-ev^", UI_SOURCE_RML, "[data-ev] data-event-blur data-event-change data-event-click data-event-contextmenu data-event-dblclick data-event-focus data-event-input data-event-keydown data-event-keyup data-event-mousedown data-event-mouseout data-event-mouseover data-event-mouseup data-event-submit" },
        { "<use u^", UI_SOURCE_RML, "[u] ui autofocus data-event-blur data-event-contextmenu data-event-focus data-event-input data-event-keyup data-event-mousedown data-event-mouseout data-event-mouseover data-event-mouseup data-event-submit data-value data-value-lazy grouping suffix value" },
        { "<use ui=\"Menu^\"/>", UI_SOURCE_RML, "[Menu] MenuButton MenuCheck" },
        { "<div class=\"card w^\">", UI_SOURCE_RML, "[w] wide" },
        { "<div data-class-t^", UI_SOURCE_RML, "[t] title" },
        { "<div data-if=\"save.th^\">", UI_SOURCE_RML, "[th] theme" },
        { "<p>{{ save.^", UI_SOURCE_RML, "[] name sound theme" },
        { "<div style=\"wi^\">", UI_SOURCE_RML, "[wi] width" },
        { "<div style=\"color: var(--m^\">", UI_SOURCE_RML, "[--m] --muted --primary" },
        { ".ca^ { }", UI_SOURCE_RCSS, "[ca] card" },
        { ".a {\n  op^\n}", UI_SOURCE_RCSS, "[op] opacity" },
        { ".a { color: var(--p^) }", UI_SOURCE_RCSS, "[--p] --primary" },
        { ".a { &:hover { he^ } }", UI_SOURCE_RCSS, "[he] height" },
        { "sp^ { }", UI_SOURCE_RCSS, "[sp] span" },
        { "<nbrflw^", UI_SOURCE_RML, "[nbrflw] number-flow" },
        { ".a {\n  wdth^\n}", UI_SOURCE_RCSS, "[wdth] width" },
        { "<div class=\"cd^\">", UI_SOURCE_RML, "[cd] card" },
        { "<div style=\"color: red; ^\">", UI_SOURCE_RML, "[] color height opacity width" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char got[512];
        complete_at(cases[i].text, cases[i].language, &p, got, sizeof got);
        if (strcmp(got, cases[i].expected)) {
            printf(
                "  completing %s\n    got      %s\n    expected %s\n",
                cases[i].text,
                got,
                cases[i].expected
            );
            ok = false;
        }
    }
    test_check(ok, "ui source completes tags, attributes, widgets, classes, save keys, properties and custom properties");
    ui_source_project_free(&p);
}

/* `text` after the edits one operation makes. */
static char *edited(const char *text, text_edit *edits)
{
    char *out = text_edits_apply(text, edits, acount(edits));
    ui_source_edits_free(edits);
    return out;
}

static bool expect_text(const char *label, const char *got, const char *expected)
{
    bool ok = got && !strcmp(got, expected);
    if (!ok) {
        printf("  %s\n--- got\n%s--- expected\n%s---\n", label, got ? got : "(null)", expected);
    }
    return ok;
}

static const char EDIT_SAMPLE[] = "<div id=\"panel\" class=\"card\">\n"
                                  "  <span>A</span>\n"
                                  "  <img src=\"a.png\"/>\n"
                                  "</div>\n"
                                  "<p>Tail</p>\n";

typedef enum
{
    SET_ATTRIBUTE,
    REPLACE_ATTRIBUTE,
    REMOVE_ATTRIBUTE,
    ADD_CLASS,
    REMOVE_CLASS,
    SET_STYLE,
    INSERT_CHILD,
    OPEN_ELEMENT,
    INSERT_BEFORE,
    INSERT_AFTER,
    MOVE,
    DELETE,
} edit_case;

static bool apply_case(ui_source *s, edit_case which, text_edit **edits)
{
    char error[160];
    switch (which) {
        case SET_ATTRIBUTE:
            return ui_source_set_attribute(s, 1, "data-if", "save.sound", edits, error, sizeof error);
        case REPLACE_ATTRIBUTE:
            return ui_source_set_attribute(s, 0, "id", "main \"box\"", edits, error, sizeof error);
        case REMOVE_ATTRIBUTE:
            return ui_source_remove_attribute(s, 0, "id", edits, error, sizeof error);
        case ADD_CLASS:
            return ui_source_add_class(s, 0, "wide", edits, error, sizeof error);
        case REMOVE_CLASS:
            return ui_source_remove_class(s, 0, "card", edits, error, sizeof error);
        case SET_STYLE:
            return ui_source_set_style(s, 1, "width", "4dp", edits, error, sizeof error);
        case INSERT_CHILD:
            return ui_source_insert_element(s, 0, UI_SOURCE_INSIDE, "<p>New</p>", edits, error, sizeof error);
        case OPEN_ELEMENT:
            return ui_source_insert_element(s, 2, UI_SOURCE_INSIDE, "<span/>", edits, error, sizeof error);
        case INSERT_BEFORE:
            return ui_source_insert_element(s, 3, UI_SOURCE_BEFORE, "<hr/>", edits, error, sizeof error);
        case INSERT_AFTER:
            return ui_source_insert_element(s, 1, UI_SOURCE_AFTER, "<div>\n  <b/>\n</div>", edits, error, sizeof error);
        case MOVE:
            return ui_source_move_element(s, 3, 0, UI_SOURCE_INSIDE, edits, error, sizeof error);
        case DELETE:
            return ui_source_delete_element(s, 2, edits, error, sizeof error);
    }
    return false;
}

/* Edits within one line: a middle class, style declarations replaced and
 * removed, and elements that share their line. */
static void inline_edits()
{
    static const char SAMPLE_LINE[] = "<p class=\"a b c\" style=\"width: "
                                      "1dp; height: 2dp;\">Text<b>x</b></p>\n"
                                      "<hr/>\n";
    static const struct
    {
        const char *label;
        const char *expected;
    } cases[] = {
        { "remove a middle class", "<p class=\"a c\" style=\"width: 1dp; height: 2dp;\">Text<b>x</b></p>\n<hr/>\n" },
        { "replace a style property", "<p class=\"a b c\" style=\"width: 3dp; height: 2dp;\">Text<b>x</b></p>\n<hr/>\n" },
        { "remove the last style property", "<p class=\"a b c\" style=\"width: 1dp;\">Text<b>x</b></p>\n<hr/>\n" },
        { "remove the first style property", "<p class=\"a b c\" style=\"height: 2dp;\">Text<b>x</b></p>\n<hr/>\n" },
        { "insert a child before an inline close tag", "<p class=\"a b c\" style=\"width: 1dp; height: 2dp;\">Text<b>x</b><i/></p>\n<hr/>\n" },
        { "delete an element that shares its line", "<p class=\"a b c\" style=\"width: 1dp; height: 2dp;\">Text</p>\n<hr/>\n" },
        { "move an element after one on its own line", "<p class=\"a b c\" style=\"width: 1dp; height: 2dp;\">Text</p>\n<hr/>\n<b>x</b>\n" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ui_source *s = ui_source_parse(SAMPLE_LINE, UI_SOURCE_RML);
        text_edit *list = NULL;
        char error[160];
        bool applied = i == 0
                         ? ui_source_remove_class(s, 0, "b", &list, error, sizeof error)
                     : i == 1
                         ? ui_source_set_style(s, 0, "width", "3dp", &list, error, sizeof error)
                     : i == 2
                         ? ui_source_set_style(s, 0, "height", NULL, &list, error, sizeof error)
                     : i == 3
                         ? ui_source_set_style(s, 0, "width", NULL, &list, error, sizeof error)
                     : i == 4
                         ? ui_source_insert_element(s, 0, UI_SOURCE_INSIDE, "<i/>", &list, error, sizeof error)
                     : i == 5
                         ? ui_source_delete_element(s, 1, &list, error, sizeof error)
                         : ui_source_move_element(s, 1, 2, UI_SOURCE_AFTER, &list, error, sizeof error);
        char *text = applied ? edited(SAMPLE_LINE, list) : NULL;
        ok &= expect_text(cases[i].label, text, cases[i].expected);
        free(text);
        ui_source_free(s);
    }
    test_check(ok, "ui source edits classes, style declarations and elements that share a line");
}

/* Markup whose last line has no line break, or which is empty. */
static void edge_edits()
{
    static const struct
    {
        const char *label, *text, *expected;
        bool move;
    } cases[] = {
        { "append to markup without a final line break",
          "<div>\n  <p/>\n</div>",
          "<div>\n  <p/>\n</div>\n<span/>",
          false },
        { "move after an element on the last line",
          "<div>\n  <p/>\n</div>",
          "<div>\n</div>\n<p/>",
          true },
        { "fill empty markup", "", "<span/>\n", false },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ui_source *s = ui_source_parse(cases[i].text, UI_SOURCE_RML);
        text_edit *list = NULL;
        char error[160];
        bool applied = cases[i].move
                         ? ui_source_move_element(s, 1, 0, UI_SOURCE_AFTER, &list, error, sizeof error)
                         : ui_source_insert_element(s, -1, UI_SOURCE_INSIDE, "<span/>", &list, error, sizeof error);
        char *text = applied ? edited(cases[i].text, list) : NULL;
        ok &= expect_text(cases[i].label, text, cases[i].expected);
        free(text);
        ui_source_free(s);
    }
    test_check(ok, "ui source edits markup without a final line break, or empty");
}

static void edits()
{
    static const struct
    {
        edit_case which;
        const char *label;
        const char *expected;
    } cases[] = {
        { SET_ATTRIBUTE, "set a new attribute", "<div id=\"panel\" class=\"card\">\n  <span data-if=\"save.sound\">A</span>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { REPLACE_ATTRIBUTE, "replace an attribute", "<div id=\"main &quot;box&quot;\" class=\"card\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { REMOVE_ATTRIBUTE, "remove an attribute", "<div class=\"card\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { ADD_CLASS, "add a class", "<div id=\"panel\" class=\"card wide\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { REMOVE_CLASS, "remove the last class", "<div id=\"panel\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { SET_STYLE, "set a style property", "<div id=\"panel\" class=\"card\">\n  <span style=\"width: 4dp;\">A</span>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { INSERT_CHILD, "insert a last child", "<div id=\"panel\" class=\"card\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n  <p>New</p>\n</div>\n<p>Tail</p>\n" },
        { OPEN_ELEMENT, "insert into a self-closing element", "<div id=\"panel\" class=\"card\">\n  <span>A</span>\n  <img src=\"a.png\">\n    <span/>\n  </img>\n</div>\n<p>Tail</p>\n" },
        { INSERT_BEFORE, "insert before an element", "<div id=\"panel\" class=\"card\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n</div>\n<hr/>\n<p>Tail</p>\n" },
        { INSERT_AFTER,
          "insert multi-line markup after an element",
          "<div id=\"panel\" class=\"card\">\n  <span>A</span>\n  <div>\n    "
          "<b/>\n  </div>\n  <img src=\"a.png\"/>\n</div>\n<p>Tail</p>\n" },
        { MOVE, "move an element into another", "<div id=\"panel\" class=\"card\">\n  <span>A</span>\n  <img src=\"a.png\"/>\n  <p>Tail</p>\n</div>\n" },
        { DELETE, "delete an element", "<div id=\"panel\" class=\"card\">\n  <span>A</span>\n</div>\n<p>Tail</p>\n" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ui_source *s = ui_source_parse(EDIT_SAMPLE, UI_SOURCE_RML);
        text_edit *list = NULL;
        bool applied = apply_case(s, cases[i].which, &list);
        char *text = applied ? edited(EDIT_SAMPLE, list) : NULL;
        ok &= expect_text(cases[i].label, text, cases[i].expected);
        free(text);
        ui_source_free(s);
    }
    test_check(ok, "ui source edits attributes, classes, inline style and elements, keeping the markup's indentation");
    inline_edits();
    edge_edits();

    ui_source *s = ui_source_parse(EDIT_SAMPLE, UI_SOURCE_RML);
    text_edit *list = NULL;
    char error[160];
    bool refused = !ui_source_move_element(s, 0, 1, UI_SOURCE_INSIDE, &list, error, sizeof error) &&
                   strstr(error, "itself") && !list &&
                   !ui_source_remove_class(s, 0, "missing", &list, error, sizeof error) &&
                   !ui_source_insert_element(s, -1, UI_SOURCE_AFTER, "<b/>", &list, error, sizeof error);
    test_check(refused, "ui source refuses edits it cannot make, with a reason");
    ui_source_free(s);
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs(text, file);
    fclose(file);
}

static void references()
{
    char root[] = "/tmp/chibi-ui-source-XXXXXX", folder[96], path[160];
    assert(mkdtemp(root));
    snprintf(folder, sizeof folder, "%s/ui", root);
    assert(!mkdir(folder, 0700));
    snprintf(path, sizeof path, "%s/theme", folder);
    assert(!mkdir(path, 0700));
    snprintf(path, sizeof path, "%s/empty", folder);
    assert(!mkdir(path, 0700));
    snprintf(path, sizeof path, "%s/theme/tokens.rcss", folder);
    write_file(path, ":root { --primary: #fff; }\n.card { color: var(--primary); }\n");
    snprintf(path, sizeof path, "%s/Button.rml", folder);
    write_file(path, "<button class=\"card\"><slot/></button>\n");
    static const char
        MENU_RML[] = "<div class=\"card\" data-if=\"save.open\">\n"
                     "  <use ui=\"Button\">Go</use>\n"
                     "</div>\n";
    static const char MENU_RCSS[] = ".card { border-color: var(--primary); }\n";
    snprintf(path, sizeof path, "%s/Menu.rml", folder);
    write_file(path, MENU_RML);
    snprintf(path, sizeof path, "%s/Menu.rcss", folder);
    write_file(path, MENU_RCSS);
    ui_source_project p = { 0 };
    ui_source_project_scan(&p, root);
    bool scanned = acount(p.files) == 4 && acount(p.widgets) == 2 &&
                   !strcmp(p.widgets[0], "Button") && acount(p.classes) == 1 &&
                   !strcmp(p.classes[0], "card") && acount(p.variables) == 1 &&
                   !strcmp(p.variables[0], "--primary") &&
                   acount(p.save_keys) == 1 && !strcmp(p.save_keys[0], "open");
    test_check(scanned, "ui source learns a project's widgets, classes, custom properties and save keys");

    ui_source *s = ui_source_parse(MENU_RML, UI_SOURCE_RML);
    ui_source *styles = ui_source_parse(MENU_RCSS, UI_SOURCE_RCSS);
    ui_source_symbol widget = ui_source_symbol_at(s, (int)(strstr(MENU_RML, "Button") - MENU_RML) + 2);
    ui_source_symbol card = ui_source_symbol_at(s, (int)(strstr(MENU_RML, "card") - MENU_RML));
    ui_source_symbol primary = ui_source_symbol_at(
        styles,
        (int)(strstr(MENU_RCSS, "--primary") - MENU_RCSS) + 3
    );
    bool symbols = widget.kind == UI_SYMBOL_WIDGET &&
                   !strcmp(widget.name, "Button") &&
                   card.kind == UI_SYMBOL_CLASS && !strcmp(card.name, "card") &&
                   primary.kind == UI_SYMBOL_VARIABLE &&
                   !strcmp(primary.name, "--primary");
    test_check(symbols, "ui source finds the widget, class or custom property at an offset");

    ui_source_location *found = NULL;
    ui_source_references(&p, &widget, &found);
    bool widgets = acount(found) == 2 && found[0].definition &&
                   strstr(found[0].path, "Button.rml") && !found[1].definition &&
                   strstr(found[1].path, "Menu.rml") && found[1].line == 2;
    ui_source_locations_free(found);
    found = NULL;
    ui_source_references(&p, &primary, &found);
    bool variables = acount(found) == 3 && found[0].definition &&
                     strstr(found[0].path, "tokens.rcss") && found[0].line == 1 &&
                     !found[1].definition && !found[2].definition;
    ui_source_locations_free(found);
    found = NULL;
    ui_source_references(&p, &card, &found);
    int definitions = 0;
    for (int i = 0; i < acount(found); i++) {
        definitions += found[i].definition;
    }
    bool classes = acount(found) == 4 && definitions == 2 && found[0].definition;
    ui_source_locations_free(found);
    test_check(widgets && variables && classes, "ui source lists a symbol's definitions, then its references, across the project");
    ui_source_free(styles);
    ui_source_free(s);
    ui_source_project_free(&p);
    char command[160];
    snprintf(command, sizeof command, "rm -rf %s", root);
    assert(system(command) == 0);
}

/* Every shipped ui file parses without a diagnostic. */
static void shipped()
{
    static const char *const CONTENT[] = { "projects/studio/content" };
    bool ok = true;
    int files = 0;
    for (size_t c = 0; c < sizeof CONTENT / sizeof *CONTENT; c++) {
        ui_source_project p = { 0 };
        ui_source_project_scan(&p, CONTENT[c]);
        for (int i = 0; i < acount(p.files); i++) {
            FILE *file = fopen(p.files[i].path, "rb");
            assert(file);
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            char *text = calloc(1, (size_t)size + 1);
            fread(text, 1, (size_t)size, file);
            fclose(file);
            ui_source *s = ui_source_parse(text, p.files[i].name ? UI_SOURCE_RML : UI_SOURCE_RCSS);
            for (int d = 0; d < acount(s->diagnostics); d++) {
                printf(
                    "  %s:%d:%d: %s\n",
                    p.files[i].path,
                    s->diagnostics[d].line,
                    s->diagnostics[d].column,
                    s->diagnostics[d].message
                );
                ok = false;
            }
            files++;
            ui_source_free(s);
            free(text);
        }
        ui_source_project_free(&p);
    }
    test_check(ok && files > 60, "every shipped ui asset's markup and styles, and stylesheet, parses without a syntax error");
}

void test_ui_source()
{
    highlighting();
    formatting();
    outline();
    diagnostics();
    completion();
    edits();
    references();
    shipped();
}
