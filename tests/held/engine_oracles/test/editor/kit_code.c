/* kit_code without a GPU: typing with the standard keys, a table of Vim
 * commands, text_edits and their undo, :w, chords for the host, completions,
 * and renders through a renderer that only counts what it is handed. */
#include "authoring/source_presentation.h"
#include "editor/documents/asset_code.h"
#include "editor/ui/kit.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/text_edit.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct counts
{
    int compiled, rendered, textured, textures;
    bool red, green; /* a mesh was drawn in #ff0000 or #00ff00 */
} counts;

static bool same(rml_color a, rml_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static uintptr_t compile_geometry(void *user, rml_mesh mesh)
{
    counts *c = user;
    for (size_t i = 0; i < mesh.vertex_count; i++) {
        c->red |= same(mesh.vertices[i].color, (rml_color){ 255, 0, 0, 255 });
        c->green |= same(mesh.vertices[i].color, (rml_color){ 0, 255, 0, 255 });
    }
    return (uintptr_t)++c->compiled;
}

static void render_geometry(void *user, uintptr_t, rml_vec2, uintptr_t texture)
{
    counts *c = user;
    c->rendered++;
    c->textured += texture != 0;
}

static void release_geometry(void *, uintptr_t) {}

static uintptr_t load_texture(void *, const char *, int *width, int *height)
{
    *width = *height = 1;
    return 1;
}

static uintptr_t generate_texture(void *user, const uint8_t *, int, int)
{
    counts *c = user;
    return (uintptr_t)++c->textures;
}

static void release_texture(void *, uintptr_t) {}
static void enable_scissor(void *, bool) {}
static void set_scissor(void *, rml_rect) {}
static void set_transform(void *, const float *) {}

static const rml_render_interface COUNTING = {
    compile_geometry, render_geometry,  release_geometry,
    load_texture,     generate_texture, release_texture,
    enable_scissor,   set_scissor,      set_transform,
};

#define CONTEXT "kit_code test"

typedef struct fixture
{
    rml_backend *backend; /* starts RmlUi and loads the faces */
    rml_renderer *renderer;
    rml_context *context;
    counts counts;
    kit *k;
    kit_frame_input in;
    kit_code_editor *e;
    kit_code_desc desc;
    bool focus;
    unsigned events; /* since the fixture last cleared them */
    char chord[32];
    int completion;
} fixture;

static void frame(fixture *f)
{
    kit_begin(f->k, &f->in);
    kit_root(f->k, "app");
    kit_code_result r = kit_code(f->k, "code", f->e, &f->desc);
    kit_style(f->k, "width", "480dp");
    kit_style(f->k, "height", "240dp");
    if (f->focus) {
        kit_focus(f->k);
        f->focus = false;
    }
    kit_end(f->k);
    rml_context_update(f->context);
    rml_context_render(f->context);
    f->in.time += 1.0 / 60.0;
    f->events |= r.events;
    if (r.events & KIT_CODE_CHORD) {
        snprintf(f->chord, sizeof f->chord, "%s", r.chord);
    }
    if (r.events & KIT_CODE_COMPLETED) {
        f->completion = r.completion;
    }
}

static void settle(fixture *f)
{
    for (int i = 0; i < 3; i++) {
        frame(f);
    }
}

static void clear_events(fixture *f)
{
    frame(f);
    f->events = 0;
    f->chord[0] = '\0';
    f->completion = -1;
}

static void press(fixture *f, rml_key key, unsigned modifiers)
{
    rml_context_process_key_down(f->context, key, modifiers);
    rml_context_process_key_up(f->context, key, modifiers);
}

static void type(fixture *f, const char *text)
{
    rml_context_process_text_input(f->context, text, strlen(text));
}

/* Keys in Vim's notation: characters as text, and <Esc>, <CR>, <BS>, <Tab>
   and <C-x> (Control and a letter) as keys. */
static void keys(fixture *f, const char *sequence)
{
    for (const char *s = sequence; *s;) {
        if (*s != '<') {
            type(f, (char[]){ *s, 0 });
            s++;
            continue;
        }
        const char *end = strchr(s, '>');
        size_t length = (size_t)(end - s - 1);
        const char *name = s + 1;
        if (!strncmp(name, "Esc", length)) {
            press(f, RML_KEY_ESCAPE, 0);
        } else if (!strncmp(name, "CR", length)) {
            press(f, RML_KEY_RETURN, 0);
        } else if (!strncmp(name, "BS", length)) {
            press(f, RML_KEY_BACK, 0);
        } else if (!strncmp(name, "Tab", length)) {
            press(f, RML_KEY_TAB, 0);
        } else if (length == 3 && name[0] == 'C' && name[1] == '-') {
            press(f, (rml_key)(RML_KEY_A + (name[2] - 'a')), RML_KM_CTRL);
        }
        s = end + 1;
    }
}

static const char *text(fixture *f)
{
    return kit_code_text(f->e, NULL);
}

static bool check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "  kit_code: %s\n", what);
    }
    return ok;
}

/* A Vim command on a fresh buffer, cursor at the start. */
typedef struct vim_case
{
    const char *name, *text, *keys, *expected;
} vim_case;

static const vim_case VIM_CASES[] = {
    /* Motions. */
    { "w moves to the next word", "one two", "wx", "one wo" },
    { "e moves to the word's end", "one two", "ex", "on two" },
    { "$ and 0 reach the line's ends", "one two", "$x0x", "ne tw" },
    { "b moves back a word", "one two", "$bx", "one wo" },
    { "j moves down a line", "ab\ncd", "jx", "ab\nd" },
    { "G and gg reach the last and first lines", "a\nb\nc", "Gxggx", "\nb\n" },
    { "f finds a character", "one two", "ftx", "one wo" },
    /* Operators. */
    { "dw deletes a word", "one two three", "dw", "two three" },
    { "dd deletes a line", "one\ntwo", "dd", "two" },
    { "cw changes a word", "one two three", "cwabc<Esc>", "abc two three" },
    { "yy and p copy a line", "one\ntwo", "yyp", "one\none\ntwo" },
    { "r replaces a character", "one", "lrx", "oxe" },
    { "J joins lines", "one\ntwo", "J", "one two" },
    { "D deletes to the line's end", "one two", "wD", "one " },
    { "o opens a line below", "one", "otwo<Esc>", "one\ntwo" },
    /* Counts. */
    { "3x deletes three characters", "four two", "3x", "r two" },
    { "2dw deletes two words", "one two three", "2dw", "three" },
    { "d2w deletes two words", "one two three", "d2w", "three" },
    { "2dd deletes two lines", "a\nb\nc", "2dd", "c" },
    /* Text objects. */
    { "diw deletes the inner word", "one two three", "ldiw", " two three" },
    { "ciw changes the inner word", "one two three", "ciwabc<Esc>", "abc two three" },
    { "daw deletes a word and its space", "(one) two three", "llllldaw", "(one) three" },
    { "ci( changes inside parentheses", "f(a, b)", "llci(x<Esc>", "f(x)" },
    /* Undo, redo and repeat. */
    { "u undoes", "one two", "dwu", "one two" },
    { "Ctrl+R redoes", "one two", "dwu<C-r>", "two" },
    { ". repeats the last change", "one two three four", "daw..", "four" },
    { "insert mode types text", "one three", "wiTwo <Esc>", "one Two three" },
};

static bool vim_cases(fixture *f)
{
    bool ok = true;
    f->desc.vim = true;
    for (size_t i = 0; i < sizeof VIM_CASES / sizeof *VIM_CASES; i++) {
        const vim_case *c = &VIM_CASES[i];
        kit_code_set_text(f->e, c->text);
        frame(f);
        press(f, RML_KEY_ESCAPE, 0);
        keys(f, c->keys);
        frame(f);
        if (strcmp(text(f), c->expected) != 0) {
            fprintf(stderr, "  kit_code vim: %s: \"%s\", expected \"%s\"\n", c->name, text(f), c->expected);
            ok = false;
        }
    }
    return ok;
}

static bool indentation(fixture *f)
{
    bool ok = true;
    static const struct
    {
        const char *source;
        int cursor;
        const char *expected;
    } cases[] = {
        { "    elapsed := 0", 16, "    elapsed := 0\n    " },
        { "    on Begin {", 14, "    on Begin {\n        " },
        { "    {}", 5, "    {\n        \n    }" },
        { "\ton Begin {", 11, "\ton Begin {\n\t\t" },
        { "    text := \"{\"", 15, "    text := \"{\"\n    " },
        { "    on Begin { // note", 22, "    on Begin { // note\n        " },
        { "    // {", 8, "    // {\n    " },
        { "    /* {", 8, "    /* {\n    " },
        { "    a\n    b", 11, "    a\n    b\n    " },
    };
    for (int vim = 0; vim < 2; vim++) {
        f->desc.vim = vim;
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            kit_code_set_text(f->e, cases[i].source);
            frame(f);
            if (vim) {
                press(f, RML_KEY_ESCAPE, 0);
                type(f, "i");
            }
            kit_code_set_cursor(f->e, cases[i].cursor);
            press(f, RML_KEY_RETURN, 0);
            frame(f);
            ok &= check(!strcmp(text(f), cases[i].expected), "newline inherits indentation and handles code braces");
            kit_code_undo(f->e);
            ok &= check(!strcmp(text(f), cases[i].source), "newline and indentation undo together");
            kit_code_redo(f->e);
            ok &= check(!strcmp(text(f), cases[i].expected), "redo restores the entire indented newline");
        }
    }
    f->desc.vim = false;
    kit_code_set_text(f->e, "    remove");
    frame(f);
    kit_code_select(f->e, 4, 10);
    press(f, RML_KEY_RETURN, 0);
    frame(f);
    ok &= check(!strcmp(text(f), "    \n    "), "indented newline replaces the selection");
    kit_code_undo(f->e);
    ok &= check(!strcmp(text(f), "    remove"), "selection replacement and indentation share one undo");
    f->desc.vim = true;
    kit_code_set_text(f->e, "    on Begin {}");
    frame(f);
    press(f, RML_KEY_ESCAPE, 0);
    keys(f, "oelapsed := 0<Esc>");
    frame(f);
    ok &= check(!strcmp(text(f), "    on Begin {}\n    elapsed := 0"), "Vim o inherits indentation");
    keys(f, "u");
    frame(f);
    ok &= check(!strcmp(text(f), "    on Begin {}"), "Vim open-line insertion undoes as one change");
    keys(f, "Oelapsed := 0<Esc>");
    frame(f);
    ok &= check(!strcmp(text(f), "    elapsed := 0\n    on Begin {}"), "Vim O inherits indentation above");
    return ok;
}

static bool standard_keys(fixture *f)
{
    bool ok = true;
    f->desc.vim = false;
    kit_code_set_text(f->e, "hello\nworld");
    clear_events(f);
    type(f, "abc");
    press(f, RML_KEY_RETURN, 0);
    frame(f);
    ok &= check(!strcmp(text(f), "abc\nhello\nworld"), "the standard keys type and break lines");
    ok &= check((f->events & KIT_CODE_CHANGED) && (f->events & KIT_CODE_CURSOR), "typing reports changes and cursor moves");
    ok &= check(kit_code_cursor(f->e) == 4, "the cursor follows the typing");
    press(f, RML_KEY_BACK, 0);
    press(f, RML_KEY_TAB, 0);
    frame(f);
    ok &= check(
        !strncmp(text(f), "abc", 3) && strlen(text(f)) > strlen("abchello\nworld"),
        "Tab types instead of moving focus"
    );
    kit_rect caret;
    ok &= check(
        kit_wants_keyboard(f->k) && kit_wants_text(f->k, &caret) && caret.h > 0,
        "the editor keeps the keyboard and places the IME at its cursor"
    );
    press(f, RML_KEY_Z, RML_KM_CTRL);
    press(f, RML_KEY_Z, RML_KM_CTRL);
    frame(f);
    ok &= check(!strcmp(text(f), "abc\nhello\nworld"), "the primary modifier and Z undo");
    clear_events(f);
    press(f, RML_KEY_S, RML_KM_CTRL);
    frame(f);
    ok &= check((f->events & KIT_CODE_CHORD) && !strcmp(f->chord, "Mod+S"), "Save goes back to the host");
    ok &= check(!strcmp(text(f), "abc\nhello\nworld"), "a host chord leaves the text alone");
    kit_code_set_text(f->e, "top: 1;");
    frame(f);
    press(f, RML_KEY_RIGHT, RML_KM_SHIFT);
    press(f, RML_KEY_RIGHT, RML_KM_SHIFT);
    press(f, RML_KEY_RIGHT, RML_KM_SHIFT);
    type(f, "bottom");
    frame(f);
    ok &= check(!strcmp(text(f), "bottom: 1;"), "typing replaces the selection, as in a text field");
    kit_code_set_text(f->e, "one\ntwo\nthree");
    frame(f);
    press(f, RML_KEY_END, RML_KM_CTRL);
    frame(f);
    ok &= check(kit_code_cursor(f->e) == (long)strlen("one\ntwo\nthree"), "Control+End goes to the end of the text");
    press(f, RML_KEY_HOME, RML_KM_CTRL);
    frame(f);
    ok &= check(kit_code_cursor(f->e) == 0, "Control+Home goes to its start");
    return ok;
}

static bool clipboard_paste(fixture *f)
{
    bool ok = true;
    const char *before = "before target after";
    const char *pasted = "soup\r\nstew\rfinish";
    const char *expected = "before soup\nstew\nfinish after";
    f->desc.vim = false;
    f->desc.history = false;
    kit_code_set_text(f->e, before);
    frame(f);
    int start = (int)(strstr(before, "target") - before);
    kit_code_select(f->e, start, start + (int)strlen("target"));
    clear_events(f);
    kit_code_paste(f->e, pasted);
    frame(f);
    ok &= check(!strcmp(text(f), expected), "paste replaces the selection and normalizes line endings");
    ok &= check(
        (f->events & KIT_CODE_CHANGED) &&
            kit_code_cursor(f->e) == start + (int)strlen("soup\nstew\nfinish"),
        "paste reports the edit and leaves the cursor after inserted text"
    );
    kit_code_undo(f->e);
    frame(f);
    ok &= check(!strcmp(text(f), before), "one undo restores the text replaced by a paste");
    return ok;
}

static bool mac_keys(fixture *f)
{
    bool ok = true;
    f->desc.vim = true;
    f->desc.mac = true;
    kit_code_set_text(f->e, "one two");
    frame(f);
    press(f, RML_KEY_ESCAPE, 0);
    keys(f, "dwu");
    /* Studio trades them, so a physical Control reads as Meta. */
    press(f, RML_KEY_R, RML_KM_META);
    frame(f);
    ok &= check(!strcmp(text(f), "two"), "Vim reads the physical Control on a Mac");
    clear_events(f);
    press(f, RML_KEY_LMETA, RML_KM_CTRL);
    frame(f);
    ok &= check(!(f->events & KIT_CODE_CHORD), "Command alone is no chord");
    press(f, RML_KEY_K, RML_KM_CTRL);
    frame(f);
    ok &= check(!strcmp(f->chord, "Mod+K"), "Command chords in Vim go to the host on a Mac");
    /* Zep's own Control+P would open a search window. */
    press(f, RML_KEY_P, RML_KM_META);
    press(f, RML_KEY_Z, RML_KM_CTRL);
    frame(f);
    ok &= check(!strcmp(text(f), "one two"), "Command+Z undoes in Vim on a Mac");
    keys(f, "x");
    frame(f);
    ok &= check(!strcmp(text(f), "ne two"), "Control+P leaves Vim in the editor's window");
    f->desc.mac = false;
    return ok;
}

static bool edits(fixture *f)
{
    bool ok = true;
    const char *original = "<div class=\"a\">\n  <p>hi</p>\n</div>\n";
    kit_code_set_text(f->e, original);
    kit_code_set_cursor(f->e, 22); /* inside "hi" */
    clear_events(f);
    const char *comment = "<!-- x -->\n";
    text_edit changes[] = {
        { 12, 13, "b" },
        { 21, 23, "hello" },
        { 0, 0, comment },
    };
    char *expected = text_edits_apply(original, changes, 3);
    kit_code_apply(f->e, changes, 3);
    frame(f);
    ok &= check(!strcmp(text(f), expected), "text_edits apply as the contract applies them");
    ok &= check(!(f->events & KIT_CODE_CHANGED), "the caller's edits report no change");
    ok &= check(kit_code_cursor(f->e) == (int)strlen(comment) + 21 + 5, "a cursor inside a replacement lands after it");
    kit_code_undo(f->e);
    frame(f);
    ok &= check(!strcmp(text(f), original), "one undo takes back every edit");
    kit_code_redo(f->e);
    frame(f);
    ok &= check(!strcmp(text(f), expected), "redo applies them again");
    free(expected);
    return ok;
}

static bool commands(fixture *f)
{
    bool ok = true;
    f->desc.vim = true;
    kit_code_set_text(f->e, "one\ntwo\nthree\n");
    frame(f);
    press(f, RML_KEY_ESCAPE, 0);
    clear_events(f);
    keys(f, ":w<CR>");
    frame(f);
    ok &= check(f->events & KIT_CODE_SAVED, ":w saves through the host");
    keys(f, ":3<CR>");
    frame(f);
    ok &= check(kit_code_cursor(f->e) == 8, ":3 goes to the third line");
    keys(f, ":e other<CR>");
    frame(f);
    ok &= check(!strcmp(text(f), "one\ntwo\nthree\n"), "commands that open files do nothing");
    return ok;
}

static bool completions(fixture *f)
{
    bool ok = true;
    f->desc.vim = false;
    kit_code_set_text(f->e, "");
    frame(f);
    type(f, "wo");
    frame(f);
    const kit_code_completion items[] = {
        { "word", "noun", NULL, CODE_COMPLETE_FIELD },
        { "world", "noun", "world()", CODE_COMPLETE_FUNCTION },
    };
    f->desc.completions = items;
    f->desc.completion_count = 2;
    f->desc.completion_start = 0;
    clear_events(f);
    kit_rect list;
    ok &= check(kit_find(f->k, "layer/@code:app/code", &list), "completions show under the cursor");
    rml_element *popup = test_query(f->context, ".code-completions");
    ok &= check(
        popup && rml_element_scroll_height(popup) <= rml_element_client_height(popup),
        "a short completion list fits without scrolling"
    );
    rml_element *label = rml_element_query_selector(popup, ".label");
    char markup[128];
    rml_element_inner_rml(label, markup, sizeof markup);
    ok &= check(strstr(markup, "<b>wo</b>") != NULL, "completion labels highlight the matching prefix");
    press(f, RML_KEY_DOWN, 0);
    press(f, RML_KEY_RETURN, 0);
    frame(f);
    ok &= check(!strcmp(text(f), "world()") && f->completion == 1, "Down and Enter accept the second item");
    ok &= check(f->events & KIT_CODE_COMPLETED, "accepting reports the completion");
    f->desc.completions = NULL;
    f->desc.completion_count = 0;
    frame(f);
    ok &= check(!kit_find(f->k, "layer/@code:app/code", &list), "no items, no list");
    kit_code_set_text(f->e, "wl");
    kit_code_set_cursor(f->e, 2);
    f->desc.completions = &items[1];
    f->desc.completion_count = 1;
    settle(f);
    popup = test_query(f->context, ".code-completions");
    label = rml_element_query_selector(popup, ".label");
    rml_element_inner_rml(label, markup, sizeof markup);
    ok &= check(strstr(markup, "<b>w</b>") && strstr(markup, "<b>l</b>"), "completion labels highlight fuzzy match positions");
    clear_events(f);
    press(f, RML_KEY_TAB, RML_KM_CTRL);
    frame(f);
    ok &= check(
        !strcmp(f->chord, "Ctrl+Tab") && !strcmp(text(f), "wl") &&
            !(f->events & KIT_CODE_COMPLETED),
        "Control+Tab switches documents without accepting a completion"
    );
    f->desc.mac = true;
    frame(f);
    clear_events(f);
    press(f, RML_KEY_TAB, RML_KM_META | RML_KM_SHIFT);
    frame(f);
    ok &= check(
        !strcmp(f->chord, "Ctrl+Shift+Tab") && !strcmp(text(f), "wl") &&
            !(f->events & KIT_CODE_COMPLETED),
        "physical Control+Shift+Tab switches documents on a Mac without "
        "editing text"
    );
    f->desc.mac = false;
    frame(f);
    press(f, RML_KEY_TAB, 0);
    frame(f);
    ok &= check(!strcmp(text(f), "world()"), "Tab accepts a fuzzy completion immediately");
    kit_code_completion many[24] = { 0 };
    char names[24][16];
    for (int i = 0; i < 24; i++) {
        snprintf(names[i], sizeof names[i], "entry%d", i);
        many[i] = (kit_code_completion){ .label = names[i],
                                         .detail = "number",
                                         .kind = CODE_COMPLETE_FIELD };
    }
    f->desc.completions = many;
    f->desc.completion_count = 24;
    settle(f);
    popup = test_query(f->context, ".code-completions");
    ok &= check(
        rml_element_scroll_height(popup) > rml_element_client_height(popup),
        "a long completion list scrolls only when rows overflow"
    );
    for (int i = 0; i < 20; i++) {
        press(f, RML_KEY_DOWN, 0);
        frame(f);
    }
    settle(f);
    kit_rect selected;
    kit_find(f->k, "layer/@code:app/code", &list);
    bool visible = kit_find(f->k, "layer/@code:app/code/i20", &selected) &&
                   selected.y >= list.y &&
                   selected.y + selected.h <= list.y + list.h;
    ok &= check(
        rml_element_scroll_top(popup) > 0 && visible,
        "keyboard navigation keeps a selected overflowing completion visible"
    );
    press(f, RML_KEY_ESCAPE, 0);
    settle(f);
    ok &= check(!kit_find(f->k, "layer/@code:app/code", &list), "Escape dismisses completion without changing the buffer");
    f->desc.completions = NULL;
    f->desc.completion_count = 0;
    frame(f);

    const kit_code_completion edge_items[] = {
        { "first", "property", NULL },    { "second", "property", NULL },
        { "third", "property", NULL },    { "fourth", "property", NULL },
        { "fifth", "property", NULL },    { "sixth", "property", NULL },
        { "seventh", "property", NULL },  { "eighth", "property", NULL },
        { "ninth", "property", NULL },    { "tenth", "property", NULL },
        { "eleventh", "property", NULL }, { "last", "property", NULL },
    };
    const char *lines = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n"
                        "ten\neleven\ntwelve\n";
    kit_code_set_text(f->e, lines);
    kit_code_set_cursor(f->e, strlen(lines));
    f->desc.completions = edge_items;
    f->desc.completion_count = 12;
    f->desc.completion_start = strlen(lines);
    f->in.height = 260;
    rml_context_set_dimensions(f->context, 800, 260);
    settle(f);
    kit_rect caret = { 0 }, last;
    kit_wants_text(f->k, &caret);
    ok &= check(
        kit_find(f->k, "layer/@code:app/code", &list) && list.y >= 4 &&
            list.y + list.h <= 256 && list.y + list.h <= caret.y + 0.5f,
        "completion near the window bottom opens above the caret within the "
        "viewport"
    );
    ok &= check(list.h <= 148.5f, "code completion contains at most seven compact rows");
    press(f, RML_KEY_UP, 0);
    settle(f);
    ok &= check(
        kit_find(f->k, "layer/@code:app/code/i11", &last) && last.y >= list.y &&
            last.y + last.h <= list.y + list.h,
        "keyboard wrap makes the final selected completion visible"
    );
    press(f, RML_KEY_RETURN, 0);
    frame(f);
    ok &= check(f->completion == 11 && strstr(text(f), "twelve\nlast"), "Enter accepts the revealed final completion");
    f->desc.completions = NULL;
    f->desc.completion_count = 0;
    f->in.height = 600;
    rml_context_set_dimensions(f->context, 800, 600);
    settle(f);
    return ok;
}

/* Workstream I: the undo keys ask a host that keeps the history, and change
   nothing themselves; F12, Shift+F12 and Control+Space ask the host too. */
static bool host_keys(fixture *f)
{
    bool ok = true;
    f->desc.vim = false;
    f->desc.history = true;
    kit_code_set_text(f->e, "one two");
    frame(f);
    type(f, "x");
    clear_events(f);
    press(f, RML_KEY_Z, RML_KM_CTRL);
    frame(f);
    ok &= check((f->events & KIT_CODE_UNDO) && !strcmp(text(f), "xone two"), "with the host keeping the history, the primary modifier and Z ask it to undo");
    clear_events(f);
    press(f, RML_KEY_Y, RML_KM_CTRL);
    frame(f);
    ok &= check(f->events & KIT_CODE_REDO, "and Y asks it to redo");
    kit_code_undo(f->e);
    frame(f);
    ok &= check(!strcmp(text(f), "xone two"), "the editor keeps no history of its own for such a host");
    f->desc.vim = true;
    frame(f);
    press(f, RML_KEY_ESCAPE, 0);
    clear_events(f);
    keys(f, "u");
    frame(f);
    ok &= check((f->events & KIT_CODE_UNDO) && !strcmp(text(f), "xone two"), "Vim's u asks the host to undo");
    clear_events(f);
    keys(f, "<C-r>");
    frame(f);
    ok &= check(f->events & KIT_CODE_REDO, "Vim's Control+R asks it to redo");
    keys(f, "i");
    press(f, RML_KEY_OEM_4, RML_KM_CTRL);
    keys(f, "x");
    frame(f);
    ok &= check(!strcmp(text(f), "one two") && !strcmp(kit_code_mode(f->e), "NORMAL"), "Control+[ leaves insert mode in Vim");
    f->desc.history = false;
    clear_events(f);
    press(f, RML_KEY_F12, 0);
    frame(f);
    ok &= check(f->events & KIT_CODE_DEFINITION, "F12 asks for the definition");
    clear_events(f);
    press(f, RML_KEY_F12, RML_KM_SHIFT);
    frame(f);
    ok &= check(f->events & KIT_CODE_REFERENCES, "Shift+F12 asks for the references");
    clear_events(f);
    press(f, RML_KEY_F2, 0);
    frame(f);
    ok &= check(f->events & KIT_CODE_RENAME, "F2 requests semantic rename");
    clear_events(f);
    press(f, RML_KEY_H, RML_KM_SHIFT | RML_KM_ALT);
    frame(f);
    ok &= check(f->events & KIT_CODE_CALLS, "Shift+Alt+H requests callable hierarchy");
    clear_events(f);
    press(f, RML_KEY_SPACE, RML_KM_CTRL);
    frame(f);
    ok &= check((f->events & KIT_CODE_SUGGEST) && !strcmp(text(f), "one two"), "Control+Space asks for completions");
    clear_events(f);
    press(f, RML_KEY_OEM_COMMA, RML_KM_CTRL);
    frame(f);
    ok &= check(!strcmp(f->chord, "Mod+,"), "Mod+, goes to the host, as Settings");
    /* Command copies and cuts Vim's selection on a Mac. */
    f->desc.mac = true;
    kit_code_set_text(f->e, "one two");
    frame(f);
    press(f, RML_KEY_ESCAPE, 0);
    keys(f, "ve");
    press(f, RML_KEY_C, RML_KM_CTRL);
    frame(f);
    ok &= check(!strcmp(text(f), "one two") && !strcmp(kit_code_mode(f->e), "NORMAL"), "Command+C copies Vim's selection");
    keys(f, "ve");
    press(f, RML_KEY_X, RML_KM_CTRL);
    frame(f);
    ok &= check(!strcmp(text(f), " two"), "Command+X cuts it");
    f->desc.mac = false;
    f->desc.vim = false;
    frame(f);
    return ok;
}

/* The caret of `byte` in the context's pixels. */
static rml_vec2 caret_at(fixture *f, int byte)
{
    kit_code_set_cursor(f->e, byte);
    settle(f);
    kit_rect caret;
    kit_wants_text(f->k, &caret);
    return (rml_vec2){ caret.x + caret.w / 2 + 1, caret.y + caret.h / 2 };
}

/* Workstream I: a double click selects a word; a resting pointer names its
   byte and shows the host's card with its line's diagnostics; long lines
   scroll sideways instead of wrapping. */
static bool pointer_features(fixture *f)
{
    bool ok = true;
    kit_code_set_text(f->e, "alpha beta-gamma delta\nsecond line");
    f->focus = true;
    settle(f);
    rml_vec2 at = caret_at(f, 8);
    rml_context_process_mouse_move(f->context, (int)at.x, (int)at.y, 0);
    rml_context_process_mouse_button_down(f->context, 0, 0);
    rml_context_process_mouse_button_up(f->context, 0, 0);
    rml_context_process_mouse_button_down(f->context, 0, 0);
    rml_context_process_mouse_button_up(f->context, 0, 0);
    frame(f);
    press(f, RML_KEY_BACK, 0);
    frame(f);
    ok &= check(!strcmp(text(f), "alpha  delta\nsecond line"), "a double click selects a word, - and all");
    const kit_code_marker markers[] = { { 1, KIT_CODE_WARNING, "unknown class" } };
    f->desc.markers = markers;
    f->desc.marker_count = 1;
    at = caret_at(f, 2);
    rml_context_process_mouse_move(f->context, (int)at.x + 1, (int)at.y, 0);
    for (int i = 0; i < 40 && kit_code_hover(f->e) < 0; i++) {
        frame(f);
    }
    ok &= check(kit_code_hover(f->e) >= 0 && kit_code_hover(f->e) <= 3, "a resting pointer names the byte under it");
    f->desc.hover = "Word alpha\nits detail";
    settle(f);
    kit_rect card;
    ok &= check(kit_find(f->k, "layer/@hover:app/code", &card) && card.h > 0, "the host's card shows under the pointer");
    ok &= check(kit_find(f->k, "layer/@hover:app/code/m0", &card), "with the diagnostics of the line");
    press(f, RML_KEY_RIGHT, 0);
    settle(f);
    ok &= check(kit_code_hover(f->e) < 0, "a key hides the card until the pointer moves");
    f->desc.hover = NULL;
    f->desc.markers = NULL;
    f->desc.marker_count = 0;
    char line[400] = "long";
    for (int i = 0; i < 70; i++) {
        strcat(line, " word");
    }
    kit_rect code;
    kit_find(f->k, "app/code", &code);
    f->desc.no_wrap = true;
    kit_code_set_text(f->e, "short");
    rml_vec2 top = caret_at(f, 0);
    ok &= check(top.y < code.y + 20 && top.x < code.x + 80, "without wrapping, short text still starts at the top left");
    kit_code_set_text(f->e, line);
    at = caret_at(f, (int)strlen(line));
    ok &= check(at.x > code.x && at.x < code.x + code.w, "without wrapping, lines scroll sideways to the cursor");
    rml_vec2 start = caret_at(f, 0);
    ok &= check(start.x > code.x && start.x < at.x, "and back");
    f->desc.no_wrap = false;
    return ok;
}

static bool rendering(fixture *f)
{
    bool ok = true;
    enum
    {
        LINES = 200,
        LINE = 48,
    };
    char *lines = calloc(LINES, LINE);
    for (int i = 0, at = 0; i < LINES; i++) {
        at += snprintf(
            lines + at,
            (size_t)(LINES * LINE - at),
            "line %d <span class=\"x\">%d</span>\n",
            i,
            i * i
        );
    }
    kit_code_set_text(f->e, lines);
    free(lines);
    /* A span takes its class's color; a second class refines the first. */
    kit_style_sheet(f->k, "spans", "kit-code .tok-test { color: #ff0000; } kit-code .tok-refined { color: #00ff00; }");
    const kit_code_span spans[] = { { 0, 4, "tok-test" },
                                    { 5, 6, "tok-test tok-refined" } };
    const kit_code_marker markers[] = { { 2, KIT_CODE_ERROR, "unclosed tag" } };
    f->desc.spans = spans;
    f->desc.span_count = 2;
    f->desc.markers = markers;
    f->desc.marker_count = 1;
    f->counts.rendered = f->counts.textured = 0;
    settle(f);
    ok &= check(f->counts.red, "a span draws in its class's color");
    ok &= check(f->counts.green, "a span's second class refines its color");
    ok &= check(f->counts.rendered > f->counts.textured, "rendering draws rectangles");
    ok &= check(f->counts.textured > 0 && f->counts.textures > 0, "text draws from the face's glyph textures");
    kit_blur(f->k);
    settle(f);
    int compiled = f->counts.compiled;
    frame(f);
    ok &= check(f->counts.compiled == compiled, "an unchanged frame reuses its meshes");
    kit_rect code;
    ok &= check(kit_find(f->k, "app/code", &code), "the editor is laid out");
    rml_context_process_mouse_move(
        f->context,
        (int)(code.x + code.w / 2),
        (int)(code.y + code.h / 2),
        0
    );
    rml_context_process_mouse_wheel(f->context, (rml_vec2){ 0, 10 }, 0);
    settle(f);
    ok &= check(kit_code_cursor(f->e) > 0, "the wheel scrolls, keeping the cursor in view");
    int before = kit_code_cursor(f->e);
    /* A press in the same frame as the move that reached it. */
    int x = (int)(code.x + code.w / 2), y = (int)(code.y + code.h / 3);
    rml_context_process_mouse_move(f->context, x, y, 0);
    rml_context_process_mouse_button_down(f->context, 0, 0);
    rml_context_process_mouse_button_up(f->context, 0, 0);
    settle(f);
    ok &= check(kit_code_cursor(f->e) != before, "a click places the cursor");
    /* Dragging selects, and Backspace deletes the selection. */
    size_t length;
    kit_code_text(f->e, &length);
    rml_context_process_mouse_move(f->context, x, y, 0);
    rml_context_process_mouse_button_down(f->context, 0, 0);
    for (int step = 1; step <= 4; step++) {
        rml_context_process_mouse_move(f->context, x + step * 12, y, 0);
        frame(f);
    }
    rml_context_process_mouse_button_up(f->context, 0, 0);
    press(f, RML_KEY_BACK, 0);
    frame(f);
    size_t after;
    kit_code_text(f->e, &after);
    ok &= check(after + 3 < length, "dragging selects text");
    f->desc.spans = NULL;
    f->desc.span_count = 0;
    kit_style_sheet(f->k, "spans", NULL);
    f->desc.markers = NULL;
    f->desc.marker_count = 0;
    return ok;
}

static void source_frame(fixture *f, asset_code *code)
{
    kit_begin(f->k, &f->in);
    kit_root(f->k, "app");
    asset_code_draw(code, f->k, "code", &f->desc, false);
    kit_style(f->k, "width", "480dp");
    kit_style(f->k, "height", "240dp");
    if (f->focus) {
        kit_focus(f->k);
        f->focus = false;
    }
    kit_end(f->k);
    rml_context_update(f->context);
    rml_context_render(f->context);
    f->in.time += 1.0 / 60.0;
}

static bool source_typing_history(fixture *f)
{
    const char *source = "behavior Typing { count:int=1; }";
    char root[] = "/tmp/chibi-kit-source-history-XXXXXX", folder[160],
         path[192], error[256];
    assert(mkdtemp(root));
    snprintf(folder, sizeof folder, "%s/behaviors", root);
    assert(!mkdir(folder, 0700));
    snprintf(path, sizeof path, "%s/Typing.chibi", folder);
    FILE *file = fopen(path, "w");
    assert(file && fputs(source, file) >= 0 && !fclose(file));
    ecs_world *world = ecs_world_new();
    game_preview_world *preview = game_preview_world_new(world);
    asset_code *code = asset_code_open(path, error, sizeof error);
    assert(code);
    asset_code_set_source_world(code, preview, root);
    asset_code_time(code, 1);
    assert(asset_code_validate(code, NULL));
    authoring_source_presentation *p = asset_code_presentation(code);
    assert(p);
    const source_presentation_origin *origin = NULL;
    for (int i = 0; i < authoring_source_presentation_origin_count(p); i++) {
        const source_presentation_origin
            *candidate = authoring_source_presentation_origin_at(p, i);
        if (candidate->kind == SOURCE_PRESENTATION_SYMBOL && candidate->node) {
            origin = candidate;
            break;
        }
    }
    assert(origin);
    uint64_t id = origin->node;
    node *tracked = graph_node_by_id(authoring_source_presentation_view(p)->graph, id);
    assert(tracked);
    tracked->x = 137;
    tracked->placed = true;
    tracked->breakpoint = true;
    char *metadata = authoring_source_presentation_metadata(p);
    assert(asset_code_replace_with_metadata(code, source, source, metadata));
    free(metadata);
    editor *graph_editor = editor_new(behavior_asset_new("Typing"));
    assert(!editor_source_presentation(graph_editor, asset_code_presentation(code)));
    graph_editor->arrange_pending = false;
    kit_code_editor *previous = f->e;
    f->e = asset_code_editor(code);
    f->desc.vim = false;
    f->focus = true;
    for (int i = 0; i < 3; i++) {
        source_frame(f, code);
    }
    kit_code_set_cursor(f->e, 0);
    type(f, "// authored comment");
    source_frame(f, code);
    press(f, RML_KEY_RETURN, 0);
    source_frame(f, code);
    asset_code_time(code, 2);
    asset_code_sync_graph(code, graph_editor, false);
    bool ok = check(!asset_code_error(code)[0], "typing stages a valid canonical source edit");
    av_value *checkpoint = asset_code_capture(code);
    const av_value *history = av_obj_get(checkpoint, "history");
    int step = av_get_int(av_obj_get(checkpoint, "step"));
    const av_value *snapshot = av_arr_get(history, (size_t)step);
    const char *edited = av_get_str(av_obj_get(snapshot, "text"));
    const authoring_source *current = asset_code_source(code);
    script_syntax *syntax = script_parse(edited);
    script_analysis *analysis = script_analyze_document(
        syntax,
        asset_code_catalog(code),
        current->analysis->identity
    );
    authoring_source_presentation *independent = authoring_source_presentation_new(
        analysis,
        asset_code_catalog(code),
        NULL
    );
    ok &= check(
        independent && authoring_source_presentation_restore(
                           independent,
                           av_get_str(av_obj_get(snapshot, "metadata"))
                       ),
        "typed history metadata anchors belong to the exact canonical source"
    );
    node *reconstructed = independent
                            ? graph_node_by_id(
                                  authoring_source_presentation_view(independent)
                                      ->graph,
                                  id
                              )
                            : NULL;
    ok &= check(
        reconstructed && reconstructed->x == 137 && reconstructed->placed &&
            reconstructed->breakpoint,
        "typed history independently reconstructs source presentation identity "
        "and layout"
    );
    authoring_source_presentation_free(independent);
    script_analysis_free(analysis);
    script_syntax_free(syntax);
    av_value_free(checkpoint);
    for (int i = 0; i < 2; i++) {
        press(f, RML_KEY_Z, RML_KM_CTRL);
        source_frame(f, code);
    }
    ok &= check(!strcmp(asset_code_text(code), source), "source undo restores the original executable text");
    node *restored = graph_node_by_id(
        authoring_source_presentation_view(asset_code_presentation(code))->graph,
        id
    );
    ok &= check(restored && restored->x == 137 && restored->placed && restored->breakpoint, "source undo preserves semantic presentation identity, layout and breakpoint");
    for (int i = 0; i < 2; i++) {
        press(f, RML_KEY_Y, RML_KM_CTRL);
        source_frame(f, code);
    }
    ok &= check(
        strstr(asset_code_text(code), "// authored comment\n") == asset_code_text(code),
        "source redo restores the valid typed comment"
    );
    restored = graph_node_by_id(
        authoring_source_presentation_view(asset_code_presentation(code))->graph,
        id
    );
    ok &= check(restored && restored->x == 137 && restored->placed && restored->breakpoint, "source redo preserves semantic presentation identity, layout and breakpoint");
    f->e = previous;
    editor_free(graph_editor);
    asset_code_free(code);
    game_preview_world_free(preview);
    ecs_world_free(world);
    assert(!unlink(path));
    snprintf(path, sizeof path, "%s/Typing.chibi.meta", folder);
    unlink(path);
    assert(!rmdir(folder) && !rmdir(root));
    f->focus = true;
    settle(f);
    return ok;
}

bool test_kit_code()
{
    fixture f = { .completion = -1 };
    f.backend = rml_backend_create(64, 64, 1);
    f.renderer = rml_renderer_create(&COUNTING, &f.counts);
    f.context = rml_create_context(CONTEXT, 800, 600, f.renderer);
    f.k = test_kit_create(f.context, "studio.rml");
    f.in = (kit_frame_input){ .dp_ratio = 1, .width = 800, .height = 600 };
    f.e = kit_code_editor_create("hello\nworld");
    bool ok = check(!strcmp(text(&f), "hello\nworld"), "an editor starts with its text");
    f.focus = true;
    settle(&f);
    ok &= check(kit_wants_keyboard(f.k), "a focused editor takes the keyboard");
    ok &= standard_keys(&f);
    ok &= indentation(&f);
    ok &= clipboard_paste(&f);
    ok &= vim_cases(&f);
    ok &= mac_keys(&f);
    ok &= edits(&f);
    ok &= commands(&f);
    ok &= completions(&f);
    ok &= host_keys(&f);
    ok &= rendering(&f);
    ok &= pointer_features(&f);
    ok &= source_typing_history(&f);
    /* The element outlives its editor until the next frame drops it. */
    kit_code_editor_destroy(f.e);
    rml_context_render(f.context);
    kit_destroy(f.k);
    rml_remove_context(CONTEXT);
    rml_release_render_managers();
    rml_renderer_destroy(f.renderer);
    rml_backend_destroy(f.backend);
    return ok;
}
