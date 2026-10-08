/* Output (workstream P4) in Studio: its views and header controls are
 * authored (Output.rml, OutputHeader.rml); the Log follows new lines while
 * at its end and writes each line's markup once, escaped; Problems' places
 * and Find's rows navigate; Calls is a tree that folds, selects and opens
 * with the pointer and the keys; the filter and Clear act on the view. */

#include "editor/workspace/editor_internal.h"
#include "support/array.h"
#include "support/text_overlay.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/motion.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS_MAX 256

enum
{
    VIEW_LOG,
    VIEW_PROBLEMS,
    VIEW_RUNTIME,
    VIEW_INFO,
    VIEW_FIND,
    VIEW_CALLS,
};

/* Output in the drawer, showing `view`, once the drawer has eased open. */
static void show_output(studio_fixture *f, int view)
{
    studio *s = f->studio;
    studio_show_editor(s, STUDIO_EDITOR_OUTPUT);
    s->output_view = view;
    s->ui_schedule.wake = true;
    frames(f, 2);
    rml_element *drawer = studio_element(s, ".drawer[data-open]");
    assert(drawer);
    frames(f, (int)ceil(motion_transition_time(drawer) / FRAME_SECONDS) + 2);
}

/* The shown rows `selector` matches in Output, in document order, but its
   data-for templates. */
static int shown(studio *s, const char *selector, rml_element **rows)
{
    rml_element *found[ROWS_MAX];
    char query[128];
    snprintf(query, sizeof query, "[data-ui=Output] %s", selector);
    int count = test_query_all(rml_backend_context(s->backend), query, found, ROWS_MAX);
    int kept = 0;
    for (int i = 0; i < count && i < ROWS_MAX; i++) {
        if (!rml_element_has_attribute(found[i], "data-for") &&
            rml_element_is_visible(found[i], true)) {
            rows[kept++] = found[i];
        }
    }
    return kept;
}

static rml_element *list(studio *s)
{
    rml_element *found = studio_element(s, "#output-list");
    assert(found);
    return found;
}

static bool at_end(studio *s)
{
    rml_element *l = list(s);
    return rml_element_scroll_top(l) + rml_element_client_height(l) >=
           rml_element_scroll_height(l) - 1;
}

/* Two UI refreshes, which an idle Studio makes at its next refresh: a
   printed line is no input, and a following List pins its end in the
   refresh after the grown rows lay out. */
static void take_lines(studio_fixture *f)
{
    for (int i = 0; i < 2; i++) {
        f->studio->ui_schedule.wake = true;
        frames(f, 1);
    }
}

static void print(studio_fixture *f, int count, const char *word)
{
    static int printed;
    for (int i = 0; i < count; i++) {
        char text[96];
        snprintf(text, sizeof text, "%s line %d", word, printed++);
        editor_log_print(f->graph, "Printer", text);
    }
    take_lines(f);
}

/* The element's text, its markup unparsed. */
static void text_of(rml_element *el, char *out, size_t size)
{
    out[0] = '\0';
    rml_element_inner_rml(el, out, size);
}

/* The header's segment labelled `label`. */
static rml_element *segment(studio *s, const char *label)
{
    rml_element *found[16];
    int count = test_query_all(rml_backend_context(s->backend), "[data-ui=OutputHeader] .seg", found, 16);
    for (int i = 0; i < count && i < 16; i++) {
        char text[64];
        text_of(found[i], text, sizeof text);
        if (!strncmp(text, label, strlen(label))) {
            return found[i];
        }
    }
    return NULL;
}

/* The header switches views and Problems' scope, whose Refresh shows with
   the project's problems. */
static void views_switch()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    show_output(&f, VIEW_LOG);
    rml_element *found[16];
    int segments = test_query_all(
        rml_backend_context(s->backend),
        "[data-ui=OutputHeader] .output-views .seg",
        found,
        16
    );
    rml_element *problems = segment(s, "Problems");
    bool log = segment(s, "Log") &&
               rml_element_has_attribute(segment(s, "Log"), "data-pressed");
    if (problems) {
        press(s, problems);
    }
    frames(&f, 3);
    bool switched = s->output_view == VIEW_PROBLEMS &&
                    rml_element_has_attribute(segment(s, "Problems"), "data-pressed") &&
                    studio_element(s, "[data-ui=OutputHeader] .output-scope");
    rml_element *project = segment(s, "Project");
    if (project) {
        press(s, project);
    }
    frames(&f, 3);
    rml_element *refresh = studio_element(s, "[data-ui=OutputHeader] .btn[data-tip=Refresh]");
    bool scoped = s->problems_scope == 1 && refresh &&
                  rml_element_is_visible(refresh, true);
    if (!(segments == 6 && log && switched && scoped)) {
        printf("  %d %d %d %d\n", segments, log, switched, scoped);
    }
    test_check(
        segments == 6 && log && switched && scoped,
        "Output's header switches its six views and Problems' scope, "
        "with Refresh for the project's problems"
    );
    fixture_free(&f);
}

/* The Log follows new lines while its end shows, stops when scrolled up,
   and follows again once scrolled back to its end. */
static void log_follows()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    show_output(&f, VIEW_LOG);
    print(&f, 300, "Follow");
    frames(&f, 3);
    bool followed = at_end(s);
    print(&f, 40, "Follow");
    frames(&f, 3);
    bool still = at_end(s);
    rml_element_set_scroll_top(list(s), 0);
    frames(&f, 3);
    print(&f, 40, "Follow");
    frames(&f, 3);
    bool stayed = rml_element_scroll_top(list(s)) < 1 && !at_end(s);
    rml_element_set_scroll_top(list(s), rml_element_scroll_height(list(s)));
    frames(&f, 3);
    print(&f, 40, "Follow");
    frames(&f, 3);
    bool resumed = at_end(s);
    if (!(followed && still && stayed && resumed)) {
        printf("  %d %d %d %d\n", followed, still, stayed, resumed);
    }
    test_check(
        followed && still && stayed && resumed,
        "Output's Log follows new lines at its end, stays where it was "
        "scrolled up to, and follows again at its end"
    );
    fixture_free(&f);
}

/* A new line into a full log changes one row's markup: the rest keep
   theirs and move up a row, as the oldest line drops. */
static void log_rows_kept()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    show_output(&f, VIEW_LOG);
    print(&f, ED_LOG_MAX, "Full");
    frames(&f, 3);
    rml_element *rows[ROWS_MAX];
    static char before[ROWS_MAX][512], after[ROWS_MAX][512];
    int count = shown(s, ".output-log", rows);
    for (int i = 0; i < count; i++) {
        text_of(rows[i], before[i], sizeof before[i]);
    }
    print(&f, 1, "Full");
    frames(&f, 3);
    int again = shown(s, ".output-log", rows), changed = 0;
    for (int i = 0; i < again && i < count; i++) {
        text_of(rows[i], after[i], sizeof after[i]);
        changed += strcmp(before[i], after[i]) != 0;
    }
    bool ok = count > 10 && again == count && changed == 1 && at_end(s);
    if (!ok) {
        printf("  rows %d then %d, %d changed\n", count, again, changed);
    }
    test_check(ok, "a line into Output's full Log rewrites one row's markup, and the others keep theirs");
    fixture_free(&f);
}

/* Markup in a line shows as text: no element, and braces open no data
   expression. */
static void log_escaped()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    show_output(&f, VIEW_LOG);
    editor_log_print(f.graph, "<i>Origin</i>", "say {{ output.view }} <b>bold</b> & more");
    take_lines(&f);
    frames(&f, 3);
    rml_element *rows[ROWS_MAX];
    int count = shown(s, ".output-log", rows);
    char text[1024] = "";
    if (count) {
        text_of(rows[count - 1], text, sizeof text);
    }
    rml_element *bold = studio_element(s, "[data-ui=Output] .output-log b");
    rml_element *italic = studio_element(s, "[data-ui=Output] .output-log i");
    /* A data expression would show the view's number in place of its text. */
    bool ok = count && !bold && !italic && strstr(text, "&lt;i&gt;Origin") &&
              strstr(text, "{{") && strstr(text, "output");
    if (!ok) {
        printf("  %d %p %p: %s\n", count, (void *)bold, (void *)italic, text);
    }
    test_check(ok, "Output escapes a line's markup and braces");
    fixture_free(&f);
}

/* The game UI's error row in Problems opens its asset's source, whose
   preview the Scene shows. */
static void problem_opens()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    text_overlay_set(BANK, "");
    frames(&f, 3);
    show_output(&f, VIEW_PROBLEMS);
    rml_element *rows[ROWS_MAX];
    int count = shown(s, ".output-row.located", rows);
    char place[256] = "";
    rml_element *at = count ? rml_element_query_selector(rows[0], ".output-at") : NULL;
    if (at) {
        text_of(at, place, sizeof place);
        press(s, rows[0]);
    }
    frames(&f, 4);
    const char *previewed = studio_previewed_ui(s);
    bool ok = count == 1 && !strcmp(place, "ui/Bank.rml") && previewed &&
              !strcmp(previewed, "Bank");
    if (!ok) {
        printf("  %d %s %s\n", count, place, previewed ? previewed : "(none)");
    }
    test_check(ok, "a Problems row names its place and opens it");
    text_overlay_set(BANK, NULL);
    fixture_free(&f);
}

/* Find's rows open their asset. */
static void find_opens()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    studio_reference ref = { .kind = REFERENCE_ASSET_LIBRARY, .use = "Call" };
    snprintf(ref.asset, sizeof ref.asset, "Weapons");
    snprintf(ref.place, sizeof ref.place, "Ignore Shooter");
    apush(s->references, ref);
    snprintf(s->references_title, sizeof s->references_title, "Ignore Shooter");
    show_output(&f, VIEW_FIND);
    rml_element *rows[ROWS_MAX];
    int count = shown(s, ".output-item", rows);
    char label[256] = "", title[64] = "";
    rml_element *name = count ? rml_element_query_selector(rows[0], ".name") : NULL;
    rml_element *head = studio_element(s, "[data-ui=Output] .find-title");
    if (head) {
        text_of(head, title, sizeof title);
    }
    if (name) {
        text_of(name, label, sizeof label);
        press(s, rows[0]);
    }
    frames(&f, 3);
    const char *open = s->scene->graph_document_id;
    bool ok = count == 1 && !strcmp(title, "Ignore Shooter") &&
              !strcmp(label, "Weapons \xe2\x80\xba Ignore Shooter") && open &&
              !strcmp(open, "Weapons");
    if (!ok) {
        printf("  %d '%s' '%s' %s\n", count, title, label, open ? open : "(none)");
    }
    test_check(ok, "Find's head names what it lists, and a row opens its asset");
    fixture_free(&f);
}

static studio_call call(const char *label, int depth, bool parent, bool open)
{
    studio_call c = { .icon = KIT_ICON_FUNCTION,
                      .depth = depth,
                      .parent = parent,
                      .open = open };
    snprintf(c.label, sizeof c.label, "%s", label);
    snprintf(c.meta, sizeof c.meta, "Weapons");
    c.at = (studio_reference){ .kind = REFERENCE_ASSET_LIBRARY, .use = "Call" };
    snprintf(c.at.asset, sizeof c.at.asset, "Weapons");
    return c;
}

/* How far down Output moves a row, in pixels: rows keep one place in the
   layout and Output places them by translateY. */
static float row_shift(rml_element *row)
{
    char value[96] = "";
    rml_element_property(row, "transform", value, sizeof value);
    const char *at = strstr(value, "translateY(");
    rml_context *context = rml_element_context(row);
    return at ? strtof(at + 11, NULL) * rml_context_dp_ratio(context) : 0;
}

static float row_y(rml_element *row)
{
    return rml_element_absolute_offset(row, RML_BOX_BORDER).y + row_shift(row);
}

static int by_top(const void *a, const void *b)
{
    float ya = row_y(*(rml_element *const *)a);
    float yb = row_y(*(rml_element *const *)b);
    return (ya > yb) - (ya < yb);
}

/* A press on `el`, inside the placed `row`, where the pointer sees it. */
static void press_in(studio *s, rml_element *row, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    rml_input input = { .x = (at.x + size.x * 0.5f) / ratio,
                        .y = (at.y + row_shift(row) + size.y * 0.5f) / ratio };
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        input.type = types[i];
        rml_backend_process_input(s->backend, &input);
    }
}

/* The Calls rows' labels from the top, joined by spaces, and the selected
   one's. Rows keep their elements, so the document's order is not the
   rows'. */
static int call_labels(studio *s, char *out, size_t size, char *selected, size_t selected_size)
{
    rml_element *rows[ROWS_MAX];
    int count = shown(s, ".output-item", rows);
    qsort(rows, (size_t)count, sizeof *rows, by_top);
    out[0] = selected[0] = '\0';
    for (int i = 0; i < count; i++) {
        char label[128];
        text_of(rml_element_query_selector(rows[i], ".name"), label, sizeof label);
        snprintf(out + strlen(out), size - strlen(out), "%s%s", i ? " " : "", label);
        if (rml_element_query_selector(rows[i], ".tree-row[data-selected]")) {
            snprintf(selected, selected_size, "%s", label);
        }
    }
    return count;
}

static rml_element *call_row(studio *s, const char *label)
{
    rml_element *rows[ROWS_MAX];
    int count = shown(s, ".output-item", rows);
    for (int i = 0; i < count; i++) {
        char text[128];
        text_of(rml_element_query_selector(rows[i], ".name"), text, sizeof text);
        if (!strcmp(text, label)) {
            return rows[i];
        }
    }
    return NULL;
}

/* A key over Output, then the selected row's label. */
static const char *key_over(studio_fixture *f, SDL_Scancode key)
{
    static char labels[512], selected[128];
    studio *s = f->studio;
    rml_element *l = list(s);
    rml_vec2 at = rml_element_absolute_offset(l, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(l, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    mouse_move(s, (at.x + size.x * 0.5f) / ratio, (at.y + size.y * 0.5f) / ratio);
    frames(f, 1);
    tap(key);
    frames(f, 3);
    call_labels(s, labels, sizeof labels, selected, sizeof selected);
    return selected;
}

/* Calls is a tree: a twisty folds a row, a click selects one, the arrows
   walk and fold it, and a double click opens a call. */
static void calls_tree()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    apush(s->calls.rows, call("Fire", 0, true, true));
    apush(s->calls.rows, call("Aim", 1, true, false));
    apush(s->calls.rows, call("Track", 2, false, false));
    apush(s->calls.rows, call("Reload", 0, false, false));
    s->calls.rows[0].more = true;
    snprintf(s->calls.root.title, sizeof s->calls.root.title, "Fire");
    s->calls.selected = -1;
    show_output(&f, VIEW_CALLS);
    char labels[512], selected[128];
    call_labels(s, labels, sizeof labels, selected, sizeof selected);
    bool listed = !strcmp(labels, "Fire Aim More Reload");
    rml_element *aim = call_row(s, "Aim");
    if (aim) {
        press_in(s, aim, rml_element_query_selector(aim, ".twisty"));
    }
    frames(&f, 3);
    call_labels(s, labels, sizeof labels, selected, sizeof selected);
    bool unfolded = !strcmp(labels, "Fire Aim Track More Reload") &&
                    s->calls.rows[1].open;
    rml_element *reload = call_row(s, "Reload");
    if (reload) {
        press_in(s, reload, reload);
    }
    frames(&f, 3);
    call_labels(s, labels, sizeof labels, selected, sizeof selected);
    bool clicked = !strcmp(selected, "Reload") && s->calls.selected == 3;
    bool up = !strcmp(key_over(&f, SDL_SCANCODE_UP), "Track");
    bool parent = !strcmp(key_over(&f, SDL_SCANCODE_LEFT), "Aim");
    bool folded = !strcmp(key_over(&f, SDL_SCANCODE_LEFT), "Aim") &&
                  !s->calls.rows[1].open;
    bool opened = !strcmp(key_over(&f, SDL_SCANCODE_RIGHT), "Aim") &&
                  s->calls.rows[1].open;
    bool child = !strcmp(key_over(&f, SDL_SCANCODE_RIGHT), "Track");
    /* The keys so far came with focus in the clicked row; with nothing
       focused, the pointer over Output gives it them as well. */
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    if (focus) {
        rml_element_blur(focus);
    }
    bool home = !strcmp(key_over(&f, SDL_SCANCODE_HOME), "Fire");
    bool end = !strcmp(key_over(&f, SDL_SCANCODE_END), "Reload");
    bool kept = !strcmp(s->scene->graph_document_id, "Teller");
    reload = call_row(s, "Reload");
    if (reload) {
        press_in(s, reload, reload);
        press_in(s, reload, reload);
    }
    frames(&f, 3);
    const char *open = s->scene->graph_document_id;
    bool double_click = kept && open && !strcmp(open, "Weapons");
    bool ok = listed && unfolded && clicked && up && parent && folded &&
              opened && child && home && end && double_click;
    if (!ok) {
        printf(
            "  %d %d %d keys %d %d %d %d %d %d %d, open %d\n",
            listed,
            unfolded,
            clicked,
            up,
            parent,
            folded,
            opened,
            child,
            home,
            end,
            double_click
        );
    }
    test_check(
        ok,
        "Output's Calls tree folds by its twisties, selects by a click, walks "
        "and folds with the arrows, and opens a call by a double click"
    );
    fixture_free(&f);
}

/* The filter keeps the lines it matches; Clear empties the Log. */
static void filter_and_clear()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    show_output(&f, VIEW_LOG);
    print(&f, 3, "alpha");
    print(&f, 2, "beta");
    frames(&f, 3);
    rml_element *rows[ROWS_MAX];
    int all = shown(s, ".output-log", rows);
    rml_element *field = studio_element(s, "#output-filter");
    assert(field);
    rml_element_focus(field, true);
    text_event("beta");
    frames(&f, 4);
    int matching = shown(s, ".output-log", rows);
    bool filtered = matching == 2 && !strcmp(s->output_filter, "beta");
    rml_element_blur(field);
    rml_element *clear = studio_element(s, "[data-ui=OutputHeader] .btn[data-tip=Clear]");
    if (clear) {
        press(s, clear);
    }
    frames(&f, 3);
    rml_element *empty = studio_element(s, "[data-ui=Output] .output-empty");
    char text[64] = "";
    if (empty) {
        text_of(empty, text, sizeof text);
    }
    bool cleared = editor_log_count(f.graph) == 0 &&
                   shown(s, ".output-log", rows) == 0 && !strcmp(text, "No output");
    if (!(all == 5 && filtered && cleared)) {
        printf("  %d %d '%s' %d '%s'\n", all, matching, s->output_filter, cleared, text);
    }
    test_check(all == 5 && filtered && cleared, "Output's filter keeps the lines it matches and Clear empties the Log");
    fixture_free(&f);
}

/* Output's own RCSS sizes its rows by the density the kit sets on the area's
   document, and follows a change. */
static void density_follows()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    print(&f, 3, "Density");
    show_output(&f, VIEW_LOG);
    float heights[3];
    for (int i = 0; i < 3; i++) {
        s->prefs.comfortable = i == 1;
        s->ui_schedule.wake = true;
        frames(&f, 4);
        rml_element *rows[ROWS_MAX];
        heights[i] = shown(s, ".output-log", rows)
                       ? rml_element_box_size(rows[0], RML_BOX_BORDER).y
                       : 0;
    }
    /* In pixels, at the window's scale: 20dp compact, 24dp comfortable. */
    test_check(heights[0] > 0 && heights[1] * 20 == heights[0] * 24 && heights[2] == heights[0], "Output's rows are 20dp compact and 24dp comfortable, and follow a change of density");
    fixture_free(&f);
}

void studio_documents_output_checks()
{
    views_switch();
    log_follows();
    log_rows_kept();
    log_escaped();
    problem_opens();
    find_opens();
    calls_tree();
    filter_and_clear();
    density_follows();
}
