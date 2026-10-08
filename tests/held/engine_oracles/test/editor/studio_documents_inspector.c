/* The UI Inspector (workstream I1) in Studio: the Elements tree is a List of
 * TreeRows over a window of every element of every document, without a row
 * limit; a row selects, its twisty folds, a pick reveals its element, and
 * the row under the pointer highlights its element in the app. */

#include "editor/studio/studio_internal.h"
#include "support/array.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROW_ATTRIBUTE "data-inspect-row"
#define ITEMS 10000

/* Studio over the Vault with the Inspector in the Properties area. */
static studio_fixture inspector_open()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_SCENE;
    studio_area_show(s, STUDIO_AREA_PROPERTIES, STUDIO_EDITOR_PANEL, "Inspector");
    frames(&f, 8);
    return f;
}

static rml_element *tree(studio *s)
{
    return studio_element(s, "#inspector-list");
}

/* The tree rows the document holds, but the data-for template. */
static int shown_rows(studio *s)
{
    rml_element *list = tree(s);
    return list ? rml_element_query_selector_all(
                      rml_element_owner_document(list),
                      "#inspector-list [" ROW_ATTRIBUTE "]",
                      NULL,
                      0
                  )
                : 0;
}

static int elements_in_panel(studio *s)
{
    rml_element *list = tree(s);
    return list
             ? rml_element_query_selector_all(rml_element_owner_document(list), "*", NULL, 0)
             : 0;
}

/* The model the panel shows now. */
static const studio_inspect_model *model(studio *s)
{
    return studio_inspect_shown(s);
}

/* The shown row that `key` names, or NULL. */
static rml_element *row_shown(studio *s, const char *key)
{
    char selector[96];
    snprintf(selector, sizeof selector, "#inspector-list [" ROW_ATTRIBUTE "=\"%s\"]", key);
    return studio_element(s, selector);
}

/* The shown row of window row `i` of the model. */
static rml_element *window_row(studio *s, int i)
{
    const studio_inspect_model *m = model(s);
    return m && i >= 0 && i < acount(m->rows) ? row_shown(s, m->rows[i].key) : NULL;
}

/* The shown row marked selected: TreeRow marks its inner row, whose parent
   carries the key. */
static rml_element *selected_row_element(studio *s)
{
    rml_element *marked = studio_element(s, "#inspector-list [data-selected]");
    return marked ? rml_element_parent(marked) : NULL;
}

static void press_twisty(studio *s, rml_element *row)
{
    rml_element *twisty = row ? rml_element_query_selector(row, ".twisty") : NULL;
    if (twisty) {
        press(s, twisty);
    }
}

static void center_dp(studio *s, rml_element *el, float *x, float *y)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    *x = (at.x + size.x * 0.5f) / ratio;
    *y = (at.y + size.y * 0.5f) / ratio;
}

/* Whether `row` lies inside the visible part of `list`. */
static bool in_view(rml_element *list, rml_element *row)
{
    if (!list || !row) {
        return false;
    }
    rml_vec2 at = rml_element_absolute_offset(list, RML_BOX_PADDING);
    rml_vec2 size = rml_element_box_size(list, RML_BOX_PADDING);
    rml_vec2 top = rml_element_absolute_offset(row, RML_BOX_BORDER);
    rml_vec2 height = rml_element_box_size(row, RML_BOX_BORDER);
    return top.y >= at.y - 1 && top.y + height.y <= at.y + size.y + 1;
}

/* Selects `el` as a pick would, and lets the panel show it. */
static void select_element(studio_fixture *f, rml_element *el)
{
    studio_inspect_select(f->studio->inspect, el);
    f->studio->ui_schedule.wake = true;
    frames(f, 5);
}

/* A document of ten thousand items, shown but not displayed, so it lays out
   nothing; returns the items' container. */
static rml_element *big_document(studio *s)
{
    size_t size = ITEMS * 40 + 256, n = 0;
    char *rml = malloc(size);
    assert(rml);
    n += (size_t)snprintf(rml + n, size - n, "<rml><body style=\"display: none; font-family: Geist;\"><div id=\"items\">");
    for (int i = 0; i < ITEMS; i++) {
        n += (size_t)snprintf(rml + n, size - n, "<div class=\"item\">Item %d</div>", i);
    }
    snprintf(rml + n, size - n, "</div></body></rml>");
    rml_context *context = rml_backend_context(s->backend);
    rml_element *doc = rml_context_load_document_from_memory(context, rml, "[big]");
    free(rml);
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    return rml_element_by_id(doc, "items");
}

/* The window row listing the element with id `id`, or -1. */
static int row_with_id(const studio_inspect_model *m, const char *id)
{
    for (int i = 0; i < acount(m->rows); i++) {
        if (!strcmp(m->rows[i].id, id)) {
            return i;
        }
    }
    return -1;
}

/* The documents are the tree's roots; a row selects on a click and its
   twisty folds without selecting. */
static void rows_select_and_fold()
{
    studio_fixture f = inspector_open();
    studio *s = f.studio;
    const studio_inspect_model *m = model(s);
    int roots = m ? m->count : 0, window = m ? acount(m->rows) : 0;
    bool listed = m && roots > 3 && window == roots && shown_rows(s) == roots &&
                  m->rows[0].parent && !m->rows[0].open && m->selected_row < 0;
    rml_element *row = window_row(s, 1);
    rml_element *name = row ? rml_element_query_selector(row, ".name") : NULL;
    press(s, name ? name : row);
    frames(&f, 3);
    m = model(s);
    bool selects = m && m->selection && m->selected_row == 1 &&
                   m->count == roots && row && selected_row_element(s) == row;
    /* The twisty of a parent row opens it, and the selection stays. */
    bool parents = m && acount(m->rows) > 2 && m->rows[2].parent;
    press_twisty(s, window_row(s, 2));
    frames(&f, 3);
    m = model(s);
    bool opened = m && m->count > roots && m->rows[2].open && m->selected_row == 1 &&
                  selected_row_element(s) == window_row(s, 1);
    int grown = m ? m->count : 0;
    press_twisty(s, window_row(s, 2));
    frames(&f, 3);
    m = model(s);
    bool closed = m && m->count == roots && !m->rows[2].open && m->selected_row == 1;
    if (!(listed && selects && parents && opened && closed)) {
        printf(
            "  roots %d rows %d shown %d; selects %d (selected row %d); "
            "parents %d; "
            "opened %d (%d rows); closed %d\n",
            roots,
            window,
            shown_rows(s),
            selects,
            m ? m->selected_row : -2,
            parents,
            opened,
            grown,
            closed
        );
    }
    test_check(
        listed && selects && parents && opened && closed,
        "the Inspector lists each document as a root row; a click selects a "
        "row and its twisty folds it without selecting"
    );
    fixture_free(&f);
}

/* Ten thousand elements list in full, as a window of a few dozen rows that
   keeps its length and its elements as the tree scrolls and folds. */
static void tree_windows_thousands()
{
    studio_fixture f = inspector_open();
    studio *s = f.studio;
    rml_element *items = big_document(s);
    select_element(&f, rml_element_child(items, 0));
    const studio_inspect_model *m = model(s);
    int count = m ? m->count : 0, length = m ? acount(m->rows) : 0;
    int elements = elements_in_panel(s);
    bool all = m && count > ITEMS && m->selection && !m->detached;
    bool windowed = length > 8 && length < 120 && shown_rows(s) == length;
    /* Scrolled to the middle: other rows, the same elements. */
    rml_element *list = tree(s);
    char value[32] = "";
    rml_element_property(list, "--row", value, sizeof value);
    float row = (float)atof(value) * studio_pixel_ratio(s);
    rml_element_set_scroll_top(list, row * (count / 2));
    frames(&f, 5);
    m = model(s);
    bool scrolled = m && m->first > count / 2 - 120 && m->first < count / 2 + 120 &&
                    acount(m->rows) == length && shown_rows(s) == length &&
                    elements_in_panel(s) == elements && window_row(s, 3);
    rml_element_set_scroll_top(list, row * count);
    frames(&f, 5);
    m = model(s);
    bool end = m && m->first + acount(m->rows) == count &&
               acount(m->rows) == length && window_row(s, length - 1);
    /* Selecting the last item reveals its row from the top. */
    rml_element_set_scroll_top(list, 0);
    frames(&f, 4);
    select_element(&f, rml_element_child(items, ITEMS - 1));
    m = model(s);
    rml_element *selected = selected_row_element(s);
    rml_element *text = selected ? rml_element_query_selector(selected, ".text") : NULL;
    char shown_text[64] = "";
    if (text) {
        rml_element_inner_rml(text, shown_text, sizeof shown_text);
    }
    bool revealed = m && m->selected_row == count - 1 &&
                    m->first + acount(m->rows) == count && selected &&
                    strstr(shown_text, "Item 9999");
    /* Folding the items leaves a few rows. */
    rml_element_set_scroll_top(list, 0);
    frames(&f, 4);
    m = model(s);
    int at = row_with_id(m, "#items");
    bool folds = false;
    if (at >= 0) {
        /* Its row at the top of the view, where a press reaches it. */
        rml_element_set_scroll_top(list, row * (m->first + at));
        frames(&f, 4);
        m = model(s);
        press_twisty(s, window_row(s, row_with_id(m, "#items")));
        frames(&f, 4);
        m = model(s);
        folds = m->count < 100 && acount(m->rows) == m->count &&
                shown_rows(s) == m->count;
    }
    if (!(all && windowed && scrolled && end && revealed && folds)) {
        printf(
            "  count %d window %d elements %d; scrolled %d end %d revealed %d "
            "(row %d, '%s'); "
            "folded at %d: %d\n",
            count,
            length,
            elements,
            scrolled,
            end,
            revealed,
            m ? m->selected_row : -2,
            shown_text,
            at,
            folds
        );
    }
    test_check(
        all && windowed && scrolled && end && revealed && folds,
        "the Inspector lists ten thousand elements without a row limit, as a "
        "window of rows that keeps its length and its elements as the tree "
        "scrolls, reveals a selected element and folds"
    );
    fixture_free(&f);
}

/* The row under the pointer highlights its element in the app. */
static void hover_highlights_element()
{
    studio_fixture f = inspector_open();
    studio *s = f.studio;
    rml_element *row = window_row(s, 0);
    float x, y;
    kit_rect rect;
    bool none = !kit_find(s->kit, "layer/inspect/hover/content", &rect);
    if (row) {
        center_dp(s, row, &x, &y);
        mouse_move(s, x, y);
    }
    frames(&f, 3);
    bool over = kit_find(s->kit, "layer/inspect/hover/content", &rect);
    kit_rect area = s->areas[STUDIO_AREA_TOP].rect;
    mouse_move(s, area.x + area.w * 0.5f, area.y + area.h * 0.5f);
    frames(&f, 3);
    bool left = !kit_find(s->kit, "layer/inspect/hover/content", &rect);
    if (!(none && over && left)) {
        printf("  highlight: none %d over %d left %d\n", none, over, left);
    }
    test_check(
        none && over && left,
        "the Inspector highlights the element of the row under the pointer and "
        "stops when the pointer leaves"
    );
    fixture_free(&f);
}

/* While picking, the row of the element under the pointer is marked, and a
   click selects it and ends the pick. */
static void picking_marks_and_selects()
{
    studio_fixture f = inspector_open();
    studio *s = f.studio;
    rml_element *pick = studio_element(s, "[data-ui=Inspector] .toolbar .pick .label");
    select_element(&f, pick);
    /* A deep element selected in a short tree scrolls into view at once. */
    bool revealed = model(s)->selected_row > 6 &&
                    in_view(tree(s), selected_row_element(s));
    studio_inspect_toggle(s);
    frames(&f, 2);
    float x, y;
    center_dp(s, pick, &x, &y);
    mouse_move(s, x, y);
    s->ui_schedule.wake = true;
    frames(&f, 3);
    const studio_inspect_model *m = model(s);
    bool marked = m && m->picking && m->picked_row >= 0 &&
                  studio_element(s, "#inspector-list [data-picked]") != NULL;
    graph_drag_button(s, x, y, true);
    frames(&f, 1);
    graph_drag_button(s, x, y, false);
    frames(&f, 4);
    m = model(s);
    bool selected = m && !m->picking && m->selection && m->selected_row >= 0 &&
                    selected_row_element(s) && !strncmp(m->title, "span", 4);
    if (!(revealed && marked && selected)) {
        printf(
            "  pick: revealed %d marked %d (row %d) selected %d (row %d, "
            "'%s')\n",
            revealed,
            marked,
            m ? m->picked_row : -2,
            selected,
            m ? m->selected_row : -2,
            m ? m->title : ""
        );
    }
    test_check(
        revealed && marked && selected,
        "an element selected deep in the tree scrolls its row into view; while "
        "picking, the Inspector marks the row of the element under the "
        "pointer, and a click selects it and ends the pick"
    );
    fixture_free(&f);
}

void studio_documents_inspector_checks()
{
    rows_select_and_fold();
    tree_windows_thousands();
    hover_highlights_element();
    picking_marks_and_selects();
}
