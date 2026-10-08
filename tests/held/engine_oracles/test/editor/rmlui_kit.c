/* Studio kit contracts without a GPU: reconciliation, number fields, menus,
 * command. */
#include "editor/ui/highlight.h"
#include "editor/ui/kit.h"
#include "support/number_expr.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"
#include "ui/controls/controls.h"
#include "ui/input_feel.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    rml_backend *backend;
    kit *k;
    kit_frame_input in;
    int clicks;
    double value, second;
    bool single_precision;
    unsigned number_flags;
    double number_max;
    int choice_value, choice_changes;
    int nested_choice, nested_choice_changes;
    int changes, commits;
    uint32_t color;
    int color_changes, color_commits;
    bool focus_color;
    bool controls;
    unsigned control_flags;
    int select, toggle;
    bool control_check;
    char control_text[32];
    bool editing;
    int menu_b;
    int menu_count, menu_action;
    bool menu_submenu;
    const char *order[3];
    int order_count;
    double flow;
    const char *morph;
    int reveal, rows_emitted, hovered_row;
    char dropped[32];
    bool drop_hovered;     /* a "thing" drag is over the target */
    int row_double_clicks; /* on the draggable row "pear" */
    char hovered[32];      /* and its payload */
    char note[64];
    int note_commits;
    bool focus_note;
    char autocomplete_value[64];
    char autocomplete_query[64];
    bool autocomplete_open;
    unsigned autocomplete_flags;
    int autocomplete_selected, autocomplete_submits, autocomplete_cancels;
    bool open_rename;             /* a popover with a name field and one item */
    bool focus_rename, b_renames; /* item B opens the popover focused */
    bool rename_empty;
    char rename[32];
    int rename_commits, rename_applied;
} kit_fixture;

static const char *const CHOICES[] = { "Alpha", "Beta", "Frame Selected", "Gamma" };

static const kit_command_item AUTOCOMPLETE_OPTIONS[] = {
    { .label = "Path 00", .group = "Paths", .keywords = "common", .meta = "float" },
    { .label = "Path 01", .group = "Paths", .keywords = "common" },
    { .label = "Path 02", .group = "Paths", .keywords = "common" },
    { .label = "Path 03", .group = "Paths", .keywords = "common" },
    { .label = "Path 04", .group = "Paths", .keywords = "common", .current = true },
    { .label = "Path 05", .group = "Paths", .keywords = "common" },
    { .label = "Path 06", .group = "Paths", .keywords = "common" },
    { .label = "Path 07", .group = "Paths", .keywords = "common" },
    { .label = "Path 08", .group = "Paths", .keywords = "common" },
    { .label = "Path 09", .group = "Paths", .keywords = "common" },
    { .label = "Path 10", .group = "Paths", .keywords = "common" },
    { .label = "Path 11", .group = "Paths", .keywords = "common" },
};

static const char
    TIP_ABOUT[] = "Fires True when Condition is true, otherwise False, "
                  "and continues from whichever output fired.";

static void action_menu(kit_fixture *f)
{
    for (int i = 0; i < f->menu_count; i++) {
        char key[16], label[32];
        snprintf(key, sizeof key, "action%d", i);
        snprintf(label, sizeof label, "Action %02d", i + 1);
        kit_menu_item_desc item = { .label = label, .checked = -1 };
        if (kit_menu_item(f->k, key, &item)) {
            f->menu_action = i + 1;
        }
    }
}

static void emit(kit_fixture *f)
{
    kit *k = f->k;
    kit_root(k, "app");
    kit_push(k, "bar", NULL, "topbar");
    if (kit_button(k, "file", &(kit_button_desc){ .label = "File" })) {
        kit_popup_open(k, "menu:app/bar/file");
    }
    if (kit_popup_begin(k, "menu:app/bar/file", "menu")) {
        kit_attr(k, "id", "test-file-menu");
        if (f->menu_count && f->menu_submenu) {
            if (kit_submenu_begin(k, "actions", "Actions", 0)) {
                kit_attr(k, "id", "test-action-submenu");
                action_menu(f);
                kit_submenu_end(k);
            }
        } else if (f->menu_count) {
            action_menu(f);
        } else {
            kit_menu_item_desc a = { .label = "A", .checked = -1 };
            kit_menu_item(k, "a", &a);
            kit_menu_item_desc b = { .label = "B", .checked = -1 };
            if (kit_menu_item(k, "b", &b)) {
                f->menu_b++;
                f->open_rename = f->focus_rename = f->b_renames;
            }
        }
        kit_popup_end(k);
    }
    kit_button_desc button = { .label = "Press" };
    if (kit_button(k, "button", &button)) {
        f->clicks++;
    }
    kit_attr(k, "id", "test-press-button");
    kit_button_desc rename = { .label = "Rename" };
    if (kit_button(k, "rename-trigger", &rename)) {
        if (kit_popup_is_open(k, "rename")) {
            kit_popup_close(k, "rename");
        } else {
            kit_popup_open(k, "rename");
        }
    }
    kit_leaf(k, "about", NULL, NULL);
    kit_style(k, "width", "40dp");
    kit_style(k, "height", "20dp");
    kit_tip(k, "Branch", NULL, TIP_ABOUT);
    kit_pop(k);
    kit_push(k, "fields", NULL, "prop-scroll");
    kit_style(k, "width", "300dp");
    kit_prop_begin(k, "first", "First");
    kit_number_desc d = { .step = 1,
                          .precision = 2,
                          .flags = f->number_flags,
                          .single_precision = f->single_precision,
                          .max = f->number_max };
    if (kit_number(k, "value", &f->value, &d)) {
        f->changes++;
    }
    f->commits += kit_item_committed(k);
    f->editing = kit_item_active(k);
    kit_prop_end(k);
    kit_prop_begin(k, "second", "Second");
    kit_number(k, "value", &f->second, &d);
    kit_prop_end(k);
    kit_prop_begin(k, "tint", "Tint");
    if (kit_color(k, "value", &f->color, 0)) {
        f->color_changes++;
    }
    f->color_commits += kit_item_committed(k);
    if (f->focus_color) {
        kit_focus(k);
        f->focus_color = false;
    }
    kit_prop_end(k);
    kit_prop_begin(k, "note", "Note");
    f->note_commits += kit_text_area(k, "value", f->note, sizeof f->note, NULL, 0);
    if (f->focus_note) {
        kit_focus(k);
        f->focus_note = false;
    }
    kit_prop_end(k);
    if (f->controls) {
        const char *const labels[] = { "One", "Two", "Three", "Four" };
        kit_prop_begin(k, "select", "Select");
        kit_choice(k, "value", &f->select, labels, 4, f->control_flags);
        kit_attr(k, "id", "test-select");
        kit_prop_end(k);
        kit_prop_begin(k, "toggle", "Toggle");
        kit_choice(k, "value", &f->toggle, labels, 2, f->control_flags);
        kit_attr(k, "id", "test-toggle");
        kit_prop_end(k);
        kit_prop_begin(k, "control-text", "Text");
        kit_text_field(k, "value", f->control_text, sizeof f->control_text, NULL, f->control_flags);
        kit_attr(k, "id", "test-control-text");
        kit_prop_end(k);
        kit_checkbox(k, "check", &f->control_check, "Check", f->control_flags);
        kit_attr(k, "id", "test-check");
    }
    kit_prop_begin(k, "choice", "Choice");
    f->choice_changes += kit_choice(
        k,
        "value",
        &f->choice_value,
        CHOICES,
        (int)(sizeof CHOICES / sizeof *CHOICES),
        0
    );
    kit_prop_end(k);
    kit_prop_begin(k, "source", "Source");
    kit_autocomplete_result result = kit_autocomplete(
        k,
        "value",
        f->autocomplete_value,
        AUTOCOMPLETE_OPTIONS,
        (int)(sizeof AUTOCOMPLETE_OPTIONS / sizeof *AUTOCOMPLETE_OPTIONS),
        "Type a path or value",
        f->autocomplete_flags
    );
    const char *query = kit_autocomplete_query(k, "value");
    f->autocomplete_open = query != NULL;
    snprintf(f->autocomplete_query, sizeof f->autocomplete_query, "%s", query ? query : "");
    if (result.selected >= 0) {
        snprintf(
            f->autocomplete_value,
            sizeof f->autocomplete_value,
            "%s",
            AUTOCOMPLETE_OPTIONS[result.selected].label
        );
        f->autocomplete_selected = result.selected;
    }
    if (result.submitted && query) {
        snprintf(f->autocomplete_value, sizeof f->autocomplete_value, "%s", query);
        f->autocomplete_submits++;
        kit_autocomplete_close(k, "value");
    }
    f->autocomplete_cancels += result.canceled;
    f->autocomplete_open = kit_autocomplete_query(k, "value") != NULL;
    kit_prop_end(k);
    kit_panel_begin(k, "panel", "Panel", NULL, true);
    kit_panel_end(k);
    kit_pop(k);
    kit_push(k, "list", NULL, NULL);
    for (int i = 0; i < f->order_count; i++) {
        kit_label(k, f->order[i], "row", f->order[i]);
        kit_style(k, "display", "block");
    }
    kit_pop(k);
    kit_push(k, "motion", NULL, NULL);
    const kit_number_flow_desc plain = { 0 };
    kit_number_flow(k, "flow", NULL, f->flow, &plain);
    kit_number_flow(k, "flow_ref", NULL, 10, &plain);
    kit_morph(k, "morph", NULL, f->morph);
    kit_morph(k, "morph_ref", NULL, "Pause");
    kit_pop(k);
    kit_list_desc list = { .count = 1000, .row_height = 20, .reveal = f->reveal };
    int first, last;
    kit_list_begin(k, "big", NULL, &list, &first, &last);
    f->hovered_row = -1;
    kit_style(k, "height", "200dp");
    kit_style(k, "flex", "0 0 auto");
    for (int i = first; i <= last; i++) {
        char row[16];
        snprintf(row, sizeof row, "r%d", i);
        kit_label(k, row, NULL, row);
        kit_style(k, "display", "block");
        kit_style(k, "height", "20dp");
        if (kit_item_hovered(k)) {
            f->hovered_row = i;
        }
    }
    kit_list_end(k);
    f->rows_emitted = last - first + 1;
    kit_push(k, "dnd", NULL, NULL);
    kit_style(k, "display", "flex");
    kit_style(k, "flex", "0 0 auto");
    kit_leaf(k, "source", NULL, NULL);
    kit_style(k, "width", "60dp");
    kit_style(k, "height", "20dp");
    kit_drag_source(k, &(kit_drag_desc){ .type = "thing", .payload = "apple" });
    kit_leaf(k, "target", NULL, NULL);
    kit_style(k, "width", "60dp");
    kit_style(k, "height", "20dp");
    kit_style(k, "margin-left", "40dp");
    kit_drop_target(k, "thing", f->dropped, sizeof f->dropped);
    kit_drop_target(k, "other", f->dropped, sizeof f->dropped);
    char type[16];
    f->drop_hovered = kit_drop_hover(k, type, sizeof type, f->hovered, sizeof f->hovered) &&
                      !strcmp(type, "thing");
    if (f->drop_hovered) {
        kit_drag_hint(k, "Put here");
    }
    unsigned pear = kit_row_begin(k, "pear", &(kit_row_desc){ .label = "Pear", .twisty = -1 });
    kit_row_end(k);
    kit_style(k, "width", "60dp");
    kit_drag_source(k, &(kit_drag_desc){ .type = "thing", .payload = "pear" });
    f->row_double_clicks += (pear & KIT_ROW_DOUBLE_CLICK) != 0;
    kit_pop(k);
    if (f->open_rename) {
        f->open_rename = false;
        kit_popup_open_at(k, "rename", 100, 100);
    }
    if (kit_popup_begin(k, "rename", "popover")) {
        if (f->rename_empty) {
            kit_label(k, "message", NULL, "No controls");
        } else {
            f->rename_commits += kit_text_field(k, "name", f->rename, sizeof f->rename, NULL, 0);
            if (f->focus_rename) {
                kit_focus(k);
                f->focus_rename = false;
            }
            kit_button_desc apply = { .label = "Apply" };
            f->rename_applied += kit_button(k, "apply", &apply);
        }
        if (!f->rename_empty) {
            f->nested_choice_changes += kit_choice(
                k,
                "mode",
                &f->nested_choice,
                CHOICES,
                (int)(sizeof CHOICES / sizeof *CHOICES),
                0
            );
        }
        kit_popup_end(k);
    }
}

static void frame(kit_fixture *f)
{
    kit_begin(f->k, &f->in);
    emit(f);
    kit_end(f->k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    f->in.time += 1.0 / 60.0;
    for (int b = 0; b < 3; b++) {
        f->in.pressed[b] = f->in.released[b] = false;
    }
}

static void settle(kit_fixture *f)
{
    for (int i = 0; i < 4; i++) {
        frame(f);
    }
}

static void pointer(kit_fixture *f, rml_input_type type, float x, float y)
{
    f->in.mouse_x = x;
    f->in.mouse_y = y;
    rml_input input = { .type = type, .x = x, .y = y };
    rml_backend_process_input(f->backend, &input);
}

static bool center(kit_fixture *f, const char *path, float *x, float *y)
{
    kit_rect r;
    if (!kit_find(f->k, path, &r)) {
        return false;
    }
    *x = r.x + r.w * 0.5f;
    *y = r.y + r.h * 0.5f;
    return true;
}

static bool click(kit_fixture *f, const char *path)
{
    float x, y;
    if (!center(f, path, &x, &y)) {
        return false;
    }
    pointer(f, RML_INPUT_POINTER_MOVE, x, y);
    pointer(f, RML_INPUT_POINTER_DOWN, x, y);
    pointer(f, RML_INPUT_POINTER_UP, x, y);
    frame(f);
    return true;
}

static void click_element(kit_fixture *f, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float x = at.x + size.x * 0.5f;
    float y = at.y + size.y * 0.5f;
    pointer(f, RML_INPUT_POINTER_MOVE, x, y);
    pointer(f, RML_INPUT_POINTER_DOWN, x, y);
    pointer(f, RML_INPUT_POINTER_UP, x, y);
    frame(f);
}

static void key(kit_fixture *f, rml_input_key k, unsigned modifiers)
{
    rml_input down = { .type = RML_INPUT_KEY_PRESS, .key = k, .modifiers = modifiers };
    rml_backend_process_input(f->backend, &down);
    rml_input up = { .type = RML_INPUT_KEY_RELEASE, .key = k, .modifiers = modifiers };
    rml_backend_process_input(f->backend, &up);
}

static void type(kit_fixture *f, const char *text)
{
    rml_input input = { .type = RML_INPUT_TEXT_INPUT,
                        .text = text,
                        .text_size = strlen(text) };
    rml_backend_process_input(f->backend, &input);
}

/* The shown tooltip's size, with its description's text. */
static rml_vec2 tip_shown(kit_fixture *f, const char *path, char *about, size_t size)
{
    float x, y;
    center(f, path, &x, &y);
    pointer(f, RML_INPUT_POINTER_MOVE, 1, 1);
    for (int i = 0; i < 10; i++) {
        frame(f);
    }
    pointer(f, RML_INPUT_POINTER_MOVE, x, y);
    for (int i = 0; i < 45; i++) {
        frame(f);
    }
    rml_element *hovered = rml_context_hover_element(rml_backend_context(f->backend));
    rml_element *document = rml_element_owner_document(hovered);
    rml_element *text = test_query(rml_backend_context(f->backend), ".tooltip .tip-about");
    about[0] = '\0';
    if (text) {
        rml_element_inner_rml(text, about, size);
    }
    return rml_element_box_size(test_query(rml_backend_context(f->backend), ".tooltip"), RML_BOX_BORDER);
}

static bool check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "  kit: %s\n", what);
    }
    return ok;
}

/* RmlUi draws its caret at the content origin with the computed line-height.
   Check its text box stays shorter and centered inside the unchanged hit box. */
static bool centered_input_line(rml_element *input)
{
    if (!input) {
        return false;
    }
    char line[32];
    rml_element_property(input, "line-height", line, sizeof line);
    float height = (float)atof(line);
    rml_vec2 hit = rml_element_box_size(input, RML_BOX_BORDER);
    rml_vec2 text = rml_element_box_size(input, RML_BOX_CONTENT);
    rml_vec2 hit_at = rml_element_absolute_offset(input, RML_BOX_BORDER);
    rml_vec2 text_at = rml_element_absolute_offset(input, RML_BOX_CONTENT);
    float top = text_at.y - hit_at.y;
    float bottom = hit.y - top - text.y;
    return height > 0 && height < hit.y && fabsf(text.y - height) <= 1 &&
           top >= 1 && fabsf(top - bottom) <= 1;
}

/* Capture actual font meshes without a GPU. Comparing only DOM text or
 * element identity would miss displaced baselines and stale glyph lines. */
typedef struct
{
    bool seen;
    float left, top, right, bottom;
} chart_bounds;

typedef struct
{
    chart_bounds glyphs[3]; /* badge, ticks, hover */
    float transform[16];
    bool transformed;
    int *unrelated_compiles;
} chart_mesh;

static void chart_bound(chart_bounds *b, float x, float y)
{
    if (!b->seen) {
        *b = (chart_bounds){ true, x, y, x, y };
        return;
    }
    b->left = fminf(b->left, x);
    b->top = fminf(b->top, y);
    b->right = fmaxf(b->right, x);
    b->bottom = fmaxf(b->bottom, y);
}

static uintptr_t chart_compile(void *user, rml_mesh mesh)
{
    chart_mesh *capture = user;
    chart_mesh *saved = xcalloc(1, sizeof *saved);
    bool unrelated = false;
    for (size_t v = 0; v < mesh.vertex_count; v++) {
        rml_vertex vertex = mesh.vertices[v];
        rml_color c = vertex.color;
        unrelated |= c.r == 255 && c.g == 255 && c.b == 0;
        int group = c.r == 255 && c.g == 0 && c.b == 0 ? 0
                  : c.r == 0 && c.g == 255 && c.b == 0 ? 1
                  : c.r == 0 && c.g == 0 && c.b == 255 ? 2
                                                       : -1;
        if (group >= 0) {
            chart_bound(
                &saved->glyphs[group],
                vertex.position.x,
                vertex.position.y
            );
        }
    }
    if (unrelated && capture->unrelated_compiles) {
        (*capture->unrelated_compiles)++;
    }
    return (uintptr_t)saved;
}

static void chart_render_mesh(void *user, uintptr_t geometry, rml_vec2 at, uintptr_t texture)
{
    if (!texture) {
        return;
    }
    chart_mesh *output = user, *mesh = (chart_mesh *)geometry;
    for (int g = 0; g < 3; g++) {
        chart_bounds b = mesh->glyphs[g];
        if (b.seen) {
            for (int corner = 0; corner < 4; corner++) {
                float x = (corner & 1 ? b.right : b.left) + at.x;
                float y = (corner & 2 ? b.bottom : b.top) + at.y;
                const float *m = output->transform;
                chart_bound(
                    &output->glyphs[g],
                    output->transformed ? m[0] * x + m[4] * y + m[12] : x,
                    output->transformed ? m[1] * x + m[5] * y + m[13] : y
                );
            }
        }
    }
}

static void chart_release_mesh(void *, uintptr_t geometry)
{
    free((void *)geometry);
}
static uintptr_t chart_load_texture(void *, const char *, int *width, int *height)
{
    *width = *height = 1;
    return 1;
}
static uintptr_t chart_generate_texture(void *, const uint8_t *, int, int)
{
    return 1;
}
static void chart_release_texture(void *, uintptr_t) {}
static void chart_scissor_enable(void *, bool) {}
static void chart_scissor(void *, rml_rect) {}
static void chart_transform(void *user, const float *transform)
{
    chart_mesh *capture = user;
    capture->transformed = transform != NULL;
    if (transform) {
        memcpy(capture->transform, transform, sizeof capture->transform);
    }
}
static void retained_transform(void *, const float *) {}

static const rml_render_interface CHART_RENDERER = {
    chart_compile,        chart_render_mesh,      chart_release_mesh,
    chart_load_texture,   chart_generate_texture, chart_release_texture,
    chart_scissor_enable, chart_scissor,          chart_transform,
};

/* The renderer boundary is also the geometry upload boundary. Distinct colors
 * identify custom meshes without counting document backgrounds or font meshes. */
typedef struct
{
    int compiles[3], releases[3];
    size_t uploaded[3];
    chart_bounds drawn[3];
} geometry_capture;

static uintptr_t retained_compile(void *user, rml_mesh mesh)
{
    geometry_capture *capture = user;
    chart_mesh *saved = xcalloc(1, sizeof *saved);
    for (size_t i = 0; i < mesh.vertex_count; i++) {
        rml_vertex v = mesh.vertices[i];
        int group = v.color.r == 241 && v.color.g == 17 && v.color.b == 203 ? 0
                  : v.color.r == 19 && v.color.g == 227 && v.color.b == 71  ? 1
                  : (v.color.r == 83 && v.color.g == 149 && v.color.b == 251) ||
                            (v.color.r == 42 && v.color.g == 75 && v.color.b == 126)
                      ? 2
                      : -1;
        if (group >= 0) {
            chart_bound(&saved->glyphs[group], v.position.x, v.position.y);
        }
    }
    for (int g = 0; g < 3; g++) {
        if (saved->glyphs[g].seen) {
            capture->compiles[g]++;
            capture->uploaded[g] += mesh.vertex_count * sizeof *mesh.vertices +
                                    mesh.index_count * sizeof *mesh.indices;
        }
    }
    return (uintptr_t)saved;
}

static void retained_render(void *user, uintptr_t geometry, rml_vec2 at, uintptr_t texture)
{
    if (texture) {
        return;
    }
    geometry_capture *capture = user;
    chart_mesh *saved = (chart_mesh *)geometry;
    for (int g = 0; g < 3; g++) {
        chart_bounds b = saved->glyphs[g];
        if (b.seen) {
            chart_bound(&capture->drawn[g], b.left + at.x, b.top + at.y);
            chart_bound(&capture->drawn[g], b.right + at.x, b.bottom + at.y);
        }
    }
}

static void retained_release(void *user, uintptr_t geometry)
{
    geometry_capture *capture = user;
    chart_mesh *saved = (chart_mesh *)geometry;
    for (int g = 0; g < 3; g++) {
        capture->releases[g] += saved->glyphs[g].seen;
    }
    free(saved);
}

static const rml_render_interface RETAINED_RENDERER = {
    retained_compile,     retained_render,        retained_release,
    chart_load_texture,   chart_generate_texture, chart_release_texture,
    chart_scissor_enable, chart_scissor,          retained_transform,
};

static void wire_geometry_frame(
    kit *k,
    rml_context *context,
    geometry_capture *capture,
    const kit_wires_desc *desc,
    float dp
)
{
    kit_frame_input in = { .width = 640, .height = 400, .dp_ratio = dp };
    rml_context_set_dp_ratio(context, dp);
    kit_begin(k, &in);
    kit_root(k, "app");
    kit_wires(k, "plot", NULL, desc);
    kit_style(k, "width", "320dp");
    kit_style(k, "height", "120dp");
    kit_end(k);
    rml_context_update(context);
    memset(capture->drawn, 0, sizeof capture->drawn);
    rml_context_render(context);
}

/* A <line-chart> of one series from 0 to `max`, published as test:plot, as
   Performance shows its own. */
static void test_line_chart(kit *k, const line_chart_series *series, double max, const char *unit, int precision)
{
    line_chart_publish("test:plot", series);
    kit_leaf(k, "plot", "line-chart", "chart");
    char text[32];
    kit_attr(k, "series", "test:plot");
    kit_attr(k, "min", "0");
    snprintf(text, sizeof text, "%g", max);
    kit_attr(k, "max", text);
    kit_attr(k, "unit", unit ? unit : "");
    snprintf(text, sizeof text, "%d", precision);
    kit_attr(k, "precision", text);
}

static void geometry_frame(kit *k, rml_context *context, geometry_capture *capture, bool chart, float value)
{
    kit_frame_input in = { .width = 640, .height = 400, .dp_ratio = 1 };
    kit_begin(k, &in);
    kit_root(k, "app");
    if (chart) {
        float times[] = { 0, 30 }, values[] = { value, value };
        line_chart_series series = { times, values, 2, 0xf111cbff, 30 };
        test_line_chart(k, &series, 1, NULL, 0);
    } else {
        kit_wire wire = { .from_x = 20,
                          .from_y = 30,
                          .to_x = value,
                          .to_y = 90,
                          .color = 0xf111cbff,
                          .width = 2 };
        kit_wires(
            k,
            "plot",
            NULL,
            &(kit_wires_desc){
                .wires = &wire,
                .count = 1,
                .style = KIT_WIRE_STRAIGHT,
                .grid = 16,
                .grid_color = 0x13e347ff,
                .grid_major_color = 0x13e347ff,
            }
        );
    }
    kit_style(k, "width", "320dp");
    kit_style(k, "height", "120dp");
    kit_end(k);
    rml_context_update(context);
    memset(capture->drawn, 0, sizeof capture->drawn);
    rml_context_render(context);
}

static bool chart_same_bounds(chart_bounds a, chart_bounds b);

static bool retained_geometry()
{
    rml_backend *fonts = rml_backend_create(64, 64, 1);
    geometry_capture capture = { 0 };
    rml_renderer *renderer = rml_renderer_create(&RETAINED_RENDERER, &capture);
    rml_context *context = rml_create_context("retained-geometry", 640, 400, renderer);
    kit *k = test_kit_create(context, "studio.rml");
    for (int i = 0; i < 3; i++) {
        geometry_frame(k, context, &capture, false, 180);
    }
    int wire_uploads = capture.compiles[0], grid_uploads = capture.compiles[1];
    size_t bytes = capture.uploaded[0] + capture.uploaded[1];
    chart_bounds wire_bounds = capture.drawn[0];
    float path[16];
    int count = kit_wire_points(k, "app/plot", 0, path, 8);
    bool ok = check(
        count == 4 && path[0] == 20 && path[1] == 30 && path[2] == 36 &&
            path[3] == 30 && path[4] == 164 && path[5] == 90 && path[6] == 180 &&
            path[7] == 90 && wire_bounds.seen && capture.drawn[1].seen && bytes > 0,
        "wire and grid upload visible geometry with the expected picking path"
    );
    geometry_frame(k, context, &capture, false, 180);
    ok &= check(
        capture.compiles[0] == wire_uploads && capture.compiles[1] == grid_uploads &&
            capture.uploaded[0] + capture.uploaded[1] == bytes &&
            chart_same_bounds(wire_bounds, capture.drawn[0]),
        "unchanged wires and grid render without compiling or uploading again"
    );
    geometry_frame(k, context, &capture, false, 240);
    count = kit_wire_points(k, "app/plot", 0, path, 8);
    ok &= check(
        capture.compiles[0] > wire_uploads && count == 4 && path[6] == 240 &&
            capture.drawn[0].right > wire_bounds.right + 50,
        "changed wire endpoints update the rendered mesh and picking path"
    );
    kit_wire wire = { .from_x = 20,
                      .from_y = 30,
                      .to_x = 240,
                      .to_y = 90,
                      .color = 0xf111cbff,
                      .width = 2 };
    kit_wires_desc wires = { .wires = &wire,
                             .count = 1,
                             .style = KIT_WIRE_STRAIGHT,
                             .grid = 16,
                             .grid_color = 0x13e347ff,
                             .grid_major_color = 0x13e347ff };
    wire_bounds = capture.drawn[0];
    wire_uploads = capture.compiles[0];
    grid_uploads = capture.compiles[1];
    wires.camera_x = wires.grid_x = 10;
    wires.camera_y = wires.grid_y = 5;
    wire_geometry_frame(k, context, &capture, &wires, 1);
    count = kit_wire_points(k, "app/plot", 0, path, 8);
    ok &= check(
        capture.compiles[0] == wire_uploads &&
            capture.compiles[1] == grid_uploads && count == 4 &&
            path[0] == 10 && path[1] == 25 && path[6] == 230 && path[7] == 85 &&
            fabsf(capture.drawn[0].left - (wire_bounds.left - 10)) < 0.01f &&
            fabsf(capture.drawn[0].top - (wire_bounds.top - 5)) < 0.01f,
        "panning translates retained world wire and grid meshes while picking "
        "stays screen-local"
    );
    wire.flags = KIT_WIRE_FLOW;
    wire.flow = 0.1f;
    wire.pulse = 1;
    wire.flow_color = 0x5395fbff;
    wire.flow_width = 8;
    wire_geometry_frame(k, context, &capture, &wires, 1);
    wire_uploads = capture.compiles[0];
    int flow_uploads = capture.compiles[2];
    float previous_path[16];
    memcpy(previous_path, path, (size_t)count * 2 * sizeof *path);
    wire.flow = 0.2f;
    wire_geometry_frame(k, context, &capture, &wires, 1);
    count = kit_wire_points(k, "app/plot", 0, path, 8);
    ok &= check(
        capture.compiles[0] == wire_uploads && capture.compiles[1] == grid_uploads &&
            capture.compiles[2] == flow_uploads + 1 && capture.drawn[2].seen &&
            count == 4 &&
            !memcmp(previous_path, path, (size_t)count * 2 * sizeof *path),
        "flow animation uploads only its bubble layer and retains static wire "
        "paths, glow and grid"
    );
    wire_geometry_frame(k, context, &capture, &wires, 1);
    ok &= check(
        capture.compiles[0] == wire_uploads && capture.compiles[2] == flow_uploads + 1,
        "an unchanged flow phase reuses the bubble mesh too"
    );
    chart_bounds hot_bounds = capture.drawn[2];
    wire.flow_width = 2;
    wire.flow_color = 0x5395fb80;
    wire.pulse = 0; /* stopped firing: the bubbles are gone, the glow fades */
    wire_geometry_frame(k, context, &capture, &wires, 1);
    ok &= check(
        capture.compiles[0] == wire_uploads && capture.compiles[1] == grid_uploads &&
            capture.compiles[2] == flow_uploads + 2 && capture.drawn[2].seen &&
            capture.drawn[2].top > hot_bounds.top,
        "a decaying glow updates only the overlay mesh once bubbles are gone"
    );
    wire_uploads = capture.compiles[0];
    wire_geometry_frame(k, context, &capture, &wires, 1.5f);
    count = kit_wire_points(k, "app/plot", 0, path, 8);
    ok &= check(
        capture.compiles[0] > wire_uploads &&
            capture.compiles[1] > grid_uploads && count == 4 && path[0] == 10 &&
            path[1] == 25 && path[6] == 230 && path[7] == 85,
        "dp changes regenerate pixel meshes without changing screen-local dp "
        "picking"
    );
    rml_context_set_dp_ratio(context, 1);
    float via[] = { 120, 50 };
    wire.via = via;
    wire.via_count = 1;
    wire.flags = KIT_WIRE_DASHED;
    wire_geometry_frame(k, context, &capture, &wires, 1);
    count = kit_wire_points(k, "app/plot", 0, path, 8);
    ok &= check(count == 5 && path[4] == 110 && path[5] == 45, "changing a dashed wire's world reroute preserves its screen-local waypoint");
    wire_uploads = capture.compiles[0];
    wires.style = KIT_WIRE_CURVED;
    wire.width = 3;
    wire_geometry_frame(k, context, &capture, &wires, 1);
    count = kit_wire_points(k, "app/plot", 0, path, 8);
    ok &= check(capture.compiles[0] > wire_uploads && count > 5 && path[0] == 10 && path[1] == 25, "wire style and width changes rebuild the retained mesh and sampled picking curve");
    wires.count = 0;
    wire_geometry_frame(k, context, &capture, &wires, 1);
    ok &= check(kit_wire_points(k, "app/plot", 0, path, 8) == 0, "removing a retained wire removes its picking path");
    for (int i = 0; i < 3; i++) {
        geometry_frame(k, context, &capture, true, 0.2f);
    }
    int chart_uploads = capture.compiles[0];
    bytes = capture.uploaded[0];
    chart_bounds chart_bounds_before = capture.drawn[0];
    geometry_frame(k, context, &capture, true, 0.2f);
    ok &= check(
        chart_bounds_before.seen && capture.compiles[0] == chart_uploads &&
            capture.uploaded[0] == bytes &&
            chart_same_bounds(chart_bounds_before, capture.drawn[0]),
        "unchanged chart samples and range render without compiling or "
        "uploading again"
    );
    geometry_frame(k, context, &capture, true, 0.8f);
    ok &= check(
        capture.compiles[0] > chart_uploads && capture.drawn[0].seen &&
            capture.drawn[0].top < chart_bounds_before.top - 40,
        "changed chart samples upload and render their new height"
    );
    kit_destroy(k);
    line_chart_publish("test:plot", NULL);
    rml_remove_context("retained-geometry");
    rml_release_render_managers();
    ok &= check(
        capture.releases[0] == capture.compiles[0] &&
            capture.releases[1] == capture.compiles[1] &&
            capture.releases[2] == capture.compiles[2],
        "retained custom meshes release their renderer resources on removal"
    );
    rml_renderer_destroy(renderer);
    rml_backend_destroy(fonts);
    return ok;
}

typedef struct
{
    rml_context *context;
    kit *k;
    kit_frame_input in;
    chart_mesh drawn;
    int unrelated_compiles;
    float value, max;
    const char *unit, *typography;
} chart_fixture;

static void chart_clear_capture(chart_fixture *f)
{
    f->drawn = (chart_mesh){ .unrelated_compiles = &f->unrelated_compiles };
}

static void chart_styles(kit *k, const char *typography)
{
    char style[512];
    snprintf(
        style,
        sizeof style,
        ".chart-badge { color: #ff0000; %s } "
        ".chart-tick { color: #00ff00; %s } "
        "body .chart .chart-hover { color: #0000ff; %s }",
        typography,
        typography,
        typography
    );
    kit_style_sheet(k, "chart-test", style);
}

static rml_element *chart_plot(chart_fixture *f)
{
    return test_query(f->context, ".chart");
}

static void chart_frame_test(chart_fixture *f)
{
    float times[] = { 0, 30 }, values[] = { f->value, f->value };
    line_chart_series series = { .times = times, .values = values, .count = 2, .now = 30 };
    kit_begin(f->k, &f->in);
    kit_root(f->k, "app");
    chart_styles(f->k, f->typography);
    kit_label(f->k, "unrelated", NULL, "Unrelated workspace label");
    kit_style(f->k, "color", "#ffff00");
    kit_style(f->k, "position", "absolute");
    kit_style(f->k, "top", "160dp");
    test_line_chart(f->k, &series, f->max, f->unit, 1);
    kit_style(f->k, "width", "320dp");
    kit_style(f->k, "height", "120dp");
    kit_end(f->k);
    rml_context_update(f->context);
    chart_clear_capture(f);
    rml_context_render(f->context);
    f->in.time += 1.0 / 60;
}

static bool chart_same_bounds(chart_bounds a, chart_bounds b)
{
    return a.seen && b.seen && fabsf(a.left - b.left) <= 1 &&
           fabsf(a.top - b.top) <= 1 && fabsf(a.right - b.right) <= 1 &&
           fabsf(a.bottom - b.bottom) <= 1;
}

/* A new ordinary label gets its glyph baseline and intrinsic width from normal
 * RmlUi layout, independently of the chart's local update path. */
static bool chart_fresh_labels(chart_fixture *f, rml_renderer *renderer)
{
    chart_mesh reference = { 0 };
    rml_context *context = rml_create_context("chart-label-reference", 640, 400, renderer);
    rml_context_set_dp_ratio(context, f->in.dp_ratio);
    kit *k = test_kit_create(context, "studio.rml");
    rml_element *plot = chart_plot(f);
    rml_vec2 origin = rml_element_absolute_offset(plot, RML_BOX_CONTENT);
    kit_begin(k, &f->in);
    kit_root(k, "app");
    chart_styles(k, f->typography);
    kit_push(k, "plot", NULL, "chart");
    kit_style(k, "position", "relative");
    kit_style(k, "width", "320dp");
    kit_style(k, "height", "120dp");
    for (int i = 0; i < rml_element_child_count(plot); i++) {
        rml_element *label = rml_element_child(plot, i);
        rml_element *text = rml_element_first_child(label);
        if (!text || !rml_element_is_visible(label, true)) {
            continue;
        }
        char cls[64], content[128], key[24], position[32], transform[96];
        rml_element_class_names(label, cls, sizeof cls);
        rml_element_text(text, content, sizeof content);
        snprintf(key, sizeof key, "label-%d", i);
        kit_label(k, key, cls, content);
        rml_vec2 at = rml_element_absolute_offset(label, RML_BOX_BORDER);
        snprintf(position, sizeof position, "%gpx", at.y - origin.y);
        kit_style(k, "top", position);
        rml_element_property(label, "transform", transform, sizeof transform);
        kit_style(k, "transform", transform);
        if (strstr(cls, "chart-hover")) {
            snprintf(position, sizeof position, "%gpx", at.x - origin.x);
            kit_style(k, "left", position);
        }
    }
    kit_pop(k);
    kit_end(k);
    rml_context_update(context);
    /* Both contexts share this renderer; its capture belongs to f. */
    chart_clear_capture(f);
    rml_context_render(context);
    reference = f->drawn;
    chart_clear_capture(f);
    rml_context_render(f->context);
    rml_element *badge = rml_element_query_selector(plot, ".chart-badge");
    rml_element *ref_badge = test_query(context, ".chart-badge");
    rml_vec2 size = rml_element_box_size(badge, RML_BOX_BORDER);
    rml_vec2 ref_size = rml_element_box_size(ref_badge, RML_BOX_BORDER);
    rml_vec2 at = rml_element_absolute_offset(badge, RML_BOX_BORDER);
    float right = origin.x + rml_element_box_size(plot, RML_BOX_CONTENT).x -
                  4 * f->in.dp_ratio;
    bool same = fabsf(size.x - ref_size.x) <= 1 && fabsf(at.x + size.x - right) <= 1;
    for (int g = 0; g < 3; g++) {
        chart_bounds actual = f->drawn.glyphs[g], fresh = reference.glyphs[g];
        bool glyphs_match = chart_same_bounds(actual, fresh);
        if (!glyphs_match) {
            fprintf(
                stderr,
                "  kit: chart glyph group %d actual=%d [%g,%g,%g,%g] fresh=%d "
                "[%g,%g,%g,%g]\n",
                g,
                actual.seen,
                actual.left,
                actual.top,
                actual.right,
                actual.bottom,
                fresh.seen,
                fresh.left,
                fresh.top,
                fresh.right,
                fresh.bottom
            );
        }
        same &= glyphs_match;
    }
    if (!same) {
        fprintf(
            stderr,
            "  kit: chart badge width=%g fresh=%g right=%g expected=%g "
            "ratio=%g\n",
            size.x,
            ref_size.x,
            at.x + size.x,
            right,
            f->in.dp_ratio
        );
    }
    kit_destroy(k);
    rml_remove_context("chart-label-reference");
    return same;
}

static bool chart_label_updates()
{
    rml_backend *fonts = rml_backend_create(64, 64, 1);
    chart_fixture f = { .value = 9, .max = 10000, .unit = "ms", .typography = "" };
    rml_renderer *renderer = rml_renderer_create(&CHART_RENDERER, &f.drawn);
    f.context = rml_create_context("chart-label-updates", 640, 400, renderer);
    f.k = test_kit_create(f.context, "studio.rml");
    f.in = (kit_frame_input){ .width = 640, .height = 400, .dp_ratio = 1 };
    for (int i = 0; i < 3; i++) {
        chart_frame_test(&f);
    }
    rml_element *plot = chart_plot(&f);
    rml_vec2 at = rml_element_absolute_offset(plot, RML_BOX_CONTENT);
    f.in.mouse_x = at.x + 100;
    f.in.mouse_y = at.y + 50;
    rml_context_process_mouse_move(f.context, (int)f.in.mouse_x, (int)f.in.mouse_y, 0);
    chart_frame_test(&f);
    bool ok = check(chart_fresh_labels(&f, renderer), "initial chart labels match ordinary laid-out glyphs");
    f.typography = "font-family: inconsolata;";
    f.value = 2000;
    chart_frame_test(&f);
    chart_frame_test(&f);
    chart_bounds before_move = f.drawn.glyphs[0];
    int unrelated_before = f.unrelated_compiles;
    float before_width = rml_element_box_size(rml_element_query_selector(plot, ".chart-badge"), RML_BOX_BORDER)
                             .x;
    f.value = 8000;
    chart_frame_test(&f);
    float after_width = rml_element_box_size(rml_element_query_selector(plot, ".chart-badge"), RML_BOX_BORDER)
                            .x;
    ok &= check(
        unrelated_before > 0 && f.unrelated_compiles == unrelated_before &&
            before_width == after_width && before_move.seen && f.drawn.glyphs[0].seen &&
            fabsf(before_move.top - f.drawn.glyphs[0].top - 72) <= 1,
        "moving an equal-width latest-value badge paints its new height "
        "without rebuilding unrelated workspace glyphs"
    );
    ok &= check(chart_fresh_labels(&f, renderer), "translated chart labels retain normal laid-out glyph baselines and right anchoring");
    f.typography = "";
    f.value = 9;
    chart_frame_test(&f);
    chart_frame_test(&f);
    float narrow = rml_element_box_size(rml_element_query_selector(plot, ".chart-badge"), RML_BOX_BORDER)
                       .x;
    f.value = 1234;
    f.unit = "µs <ms>&";
    chart_frame_test(&f);
    char text[128];
    rml_element *badge = rml_element_query_selector(plot, ".chart-badge");
    rml_element_text(rml_element_first_child(badge), text, sizeof text);
    float wide = rml_element_box_size(badge, RML_BOX_BORDER).x;
    ok &= check(
        !strcmp(text, "1234.0 µs <ms>&") && wide > narrow &&
            chart_fresh_labels(&f, renderer),
        "chart glyph updates show Unicode and literal markup units and grow "
        "the right-anchored badge"
    );
    f.value = 2;
    f.unit = "";
    chart_frame_test(&f);
    rml_element_text(rml_element_first_child(badge), text, sizeof text);
    ok &= check(
        !strcmp(text, "2.0") && rml_element_box_size(badge, RML_BOX_BORDER).x < wide &&
            chart_fresh_labels(&f, renderer),
        "chart glyph updates shrink digit counts and remove units without "
        "retaining stale glyphs"
    );
    f.max = 100000;
    for (int i = 0; i < 3; i++) {
        chart_frame_test(&f);
    }
    bool larger_ticks = false;
    for (int i = 0; i < rml_element_child_count(plot); i++) {
        rml_element *label = rml_element_child(plot, i);
        if (rml_element_is_class_set(label, "chart-tick") &&
            rml_element_first_child(label)) {
            rml_element_text(rml_element_first_child(label), text, sizeof text);
            larger_ticks |= atof(text) >= 10000;
        }
    }
    ok &= check(larger_ticks && chart_fresh_labels(&f, renderer), "changing the chart range updates tick text and glyph widths to match fresh layout");
    f.typography = "font-family: inconsolata; font-size: 15dp; font-weight: "
                   "700; line-height: 24dp;";
    chart_frame_test(&f);
    chart_frame_test(&f);
    ok &= check(chart_fresh_labels(&f, renderer), "chart labels match fresh font size, face, weight and line-height layout");
    f.in.dp_ratio = 1.5f;
    rml_context_set_dp_ratio(f.context, f.in.dp_ratio);
    chart_frame_test(&f);
    chart_frame_test(&f);
    ok &= check(chart_fresh_labels(&f, renderer), "chart labels match fresh layout after a dp-ratio change");
    kit_destroy(f.k);
    line_chart_publish("test:plot", NULL);
    rml_remove_context("chart-label-updates");
    rml_release_render_managers();
    rml_renderer_destroy(renderer);
    rml_backend_destroy(fonts);
    return ok;
}

typedef struct
{
    rml_context *context;
    kit *k;
    kit_frame_input in;
    chart_mesh drawn;
    int unrelated_compiles;
    double value, max;
    int width;
    bool slider, changed;
    const char *typography, *unit;
} number_label_fixture;

static void number_label_emit(number_label_fixture *f, kit *k, const char *reference)
{
    kit_begin(k, &f->in);
    kit_root(k, "app");
    char css[512], width[32];
    snprintf(
        css,
        sizeof css,
        ".number .value { color: #ff0000; %s } "
        ".number .addon { color: #00ff00; } "
        ".number .unit { color: #0000ff; } .number-unrelated { color: #ffff00; "
        "}",
        f->typography
    );
    kit_style_sheet(k, "number-label-test", css);
    kit_label(k, "unrelated", "number-unrelated", "Workspace content stays still");
    if (reference) {
        kit_push(k, "number", NULL, "field number");
        kit_label(k, "addon", "part addon", "X");
        kit_label(k, "value", "part value", reference);
        kit_label(k, "unit", "part unit", f->unit);
        kit_pop(k);
    } else {
        f->changed = kit_number(
            k,
            "number",
            &f->value,
            &(kit_number_desc){ .precision = 6,
                                .addon = "X",
                                .unit = f->unit,
                                .max = f->max,
                                .slider = f->slider }
        );
    }
    snprintf(width, sizeof width, "%ddp", f->width);
    kit_style(k, "width", width);
    kit_end(k);
}

static void number_label_frame(number_label_fixture *f)
{
    number_label_emit(f, f->k, NULL);
    rml_context_update(f->context);
    f->drawn = (chart_mesh){ .unrelated_compiles = &f->unrelated_compiles };
    rml_context_render(f->context);
    f->in.time += 1.0 / 60;
}

static bool number_label_reference(number_label_fixture *f, rml_renderer *renderer)
{
    char text[128];
    rml_element *value = test_query(f->context, ".number .value");
    rml_element_text(rml_element_first_child(value), text, sizeof text);
    chart_mesh actual = f->drawn;
    rml_context *context = rml_create_context("number-label-reference", 640, 400, renderer);
    rml_context_set_dp_ratio(context, f->in.dp_ratio);
    kit *k = test_kit_create(context, "studio.rml");
    number_label_emit(f, k, text);
    rml_context_update(context);
    f->drawn = (chart_mesh){ 0 };
    rml_context_render(context);
    bool same = true;
    for (int group = 0; group < 3; group++) {
        chart_bounds a = actual.glyphs[group], b = f->drawn.glyphs[group];
        bool matches = chart_same_bounds(a, b);
        if (!matches) {
            fprintf(
                stderr,
                "  kit: number '%s' group %d glyphs [%g,%g,%g,%g] fresh "
                "[%g,%g,%g,%g]\n",
                text,
                group,
                a.left,
                a.top,
                a.right,
                a.bottom,
                b.left,
                b.top,
                b.right,
                b.bottom
            );
        }
        same &= matches;
    }
    kit_destroy(k);
    rml_remove_context("number-label-reference");
    f->drawn = actual;
    return same;
}

static bool number_label_updates()
{
    rml_backend *fonts = rml_backend_create(64, 64, 1);
    number_label_fixture f = { .value = 9, .width = 180, .typography = "", .unit = "m" };
    rml_renderer *renderer = rml_renderer_create(&CHART_RENDERER, &f.drawn);
    f.context = rml_create_context("number-label-updates", 640, 400, renderer);
    f.k = test_kit_create(f.context, "studio.rml");
    f.in = (kit_frame_input){ .width = 640, .height = 400, .dp_ratio = 1 };
    number_label_frame(&f);
    number_label_frame(&f);
    bool ok = check(number_label_reference(&f, renderer), "initial numeric glyphs match ordinary right-aligned text");
    const double values[] = { 20.125, -1234.5, 0.000001, 9 };
    int compiled = f.unrelated_compiles;
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        f.value = values[i];
        number_label_frame(&f);
        ok &= check(f.unrelated_compiles == compiled, "live fitting numeric values preserve unrelated workspace glyph meshes");
        ok &= check(number_label_reference(&f, renderer), "growing, shrinking, small and negative numeric glyphs match ordinary right alignment");
    }
    f.width = 80;
    f.value = 1e100;
    number_label_frame(&f);
    ok &= check(number_label_reference(&f, renderer), "overlong numeric values retain ordinary ellipsis layout");
    f.value = -2;
    number_label_frame(&f);
    ok &= check(number_label_reference(&f, renderer), "numeric text recovers normal alignment after overflow");
    f.width = 220;
    f.unit = "μm";
    f.typography = "font-family: inconsolata; font-size: 15dp; font-weight: "
                   "700; line-height: 24dp;";
    f.value = -20.125;
    number_label_frame(&f);
    number_label_frame(&f);
    ok &= check(number_label_reference(&f, renderer), "numeric labels match fresh typography, units and resized layout");
    const char *typographies[] = {
        "font-family: inter; font-size: 15dp; font-weight: 700; line-height: "
        "24dp;",
        "font-family: inter; font-size: 18dp; font-weight: 400; line-height: "
        "24dp;",
        "font-family: inter; font-size: 18dp; font-weight: 400; line-height: "
        "32dp;",
    };
    for (size_t i = 0; i < sizeof typographies / sizeof *typographies; i++) {
        f.typography = typographies[i];
        number_label_frame(&f);
        number_label_frame(&f);
        ok &= check(number_label_reference(&f, renderer), "standalone numeric family, size, weight and line-height changes retain ordinary glyph baselines");
    }
    f.in.dp_ratio = 1.5f;
    rml_context_set_dp_ratio(f.context, f.in.dp_ratio);
    number_label_frame(&f);
    number_label_frame(&f);
    ok &= check(number_label_reference(&f, renderer), "unchanged numeric text matches ordinary layout after a dp-ratio change");
    f.value = 3.5;
    number_label_frame(&f);
    ok &= check(number_label_reference(&f, renderer), "numeric glyphs match ordinary layout after a dp-ratio change");
    f.value = 234.25;
    number_label_frame(&f);
    ok &= check(number_label_reference(&f, renderer), "local numeric updates retain the new font and dp baseline");
    kit_destroy(f.k);
    rml_remove_context("number-label-updates");
    rml_release_render_managers();
    rml_renderer_destroy(renderer);
    rml_backend_destroy(fonts);
    return ok;
}

/* A wrapping inline element receives resize notifications for its fragment
 * boxes on every real layout, even when the resulting boxes are unchanged. */
static int variable_font_fragment_resizes;

static void variable_font_resize(rml_element *, void *)
{
    variable_font_fragment_resizes++;
}

/* Equal glyph meshes retain their uploads even through repeated layout.
 * Fragment resize notifications expose the layout work that uploads miss. */
static bool variable_font_quiescence()
{
    rml_backend *fonts = rml_backend_create(64, 64, 1);
    int compiles = 0;
    /* LoadDocument formats before returning, before per-element user data can
       be attached. This test-only instancer counts that initial layout too. */
    variable_font_fragment_resizes = 0;
    chart_mesh capture = { .unrelated_compiles = &compiles };
    static const rml_element_hooks hooks = { .resize = variable_font_resize };
    static rml_instancer *instancer;
    if (!instancer) {
        instancer = rml_instancer_create(&hooks);
        rml_register_element_instancer("font-quiescence-probe", instancer);
    }
    rml_renderer *renderer = rml_renderer_create(&CHART_RENDERER, &capture);
    rml_context *context = rml_create_context("variable-font-quiescence", 640, 400, renderer);
    rml_element *document = rml_context_load_document_from_memory(
        context,
        "<rml><head><style>body { --test-font: 13dp; font-family: Inter; "
        "font-size: var(--test-font); line-height: 1.4; color: #ffff00; }"
        "div { width: 96dp; } font-quiescence-probe { display: inline; "
        "white-space: normal; }"
        "</style></head><body><div><font-quiescence-probe>Variable document "
        "font wraps into several inline fragments and stays still after "
        "layout</font-quiescence-probe></div></body></rml>",
        "variable-font-quiescence.rml"
    );
    rml_element_set_property(document, "font-family", "Inter");
    rml_element_set_property(document, "font-size", "var(--test-font, 13dp)");
    rml_document_show(document, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    bool ok = true;
    for (int phase = 0; phase < 3; phase++) {
        /* Loading and showing can lay out the initial fragments synchronously.
           The mount phase includes that work; later phases measure changes. */
        int before = compiles,
            before_layout = phase == 0 ? 0 : variable_font_fragment_resizes;
        if (phase == 1) {
            rml_context_set_dp_ratio(context, 1.5f);
        } else if (phase == 2) {
            rml_document_add_named_style_sheet(document, "font-change", "body { --test-font: 18dp; }");
        }
        for (int frame = 0; frame < 3; frame++) {
            rml_context_update(context);
            rml_context_render(context);
        }
        ok &= check(compiles > before, "a variable document font renders its initial, rescaled and changed glyphs");
        if (variable_font_fragment_resizes <= before_layout) {
            fprintf(stderr, "  kit: variable document font phase %d fragment layouts before=%d after=%d\n", phase, before_layout, variable_font_fragment_resizes);
        }
        ok &= check(variable_font_fragment_resizes > before_layout, "variable document font changes lay out the wrapping inline fragments");
        int settled = compiles, settled_layout = variable_font_fragment_resizes;
        for (int frame = 0; frame < 12; frame++) {
            rml_context_update(context);
            rml_context_render(context);
        }
        if (compiles != settled) {
            fprintf(
                stderr,
                "  kit: variable document font phase %d compiled %d additional "
                "glyph meshes after settling\n",
                phase,
                compiles - settled
            );
        }
        ok &= check(compiles == settled, "variable document font retains unchanged glyph uploads after mount, dp and font changes");
        if (variable_font_fragment_resizes != settled_layout) {
            fprintf(
                stderr,
                "  kit: variable document font phase %d repeated %d inline "
                "fragment layouts after settling\n",
                phase,
                variable_font_fragment_resizes - settled_layout
            );
        }
        ok &= check(variable_font_fragment_resizes == settled_layout, "a variable-based document font stops relaying out inline fragments after mount, dp and font changes");
    }
    rml_remove_context("variable-font-quiescence");
    rml_release_render_managers();
    rml_renderer_destroy(renderer);
    rml_backend_destroy(fonts);
    return ok;
}

/* An invisible slider fill must not turn local numeric text changes into
 * document layout. The wrapping sibling detects layouts that retain meshes. */
static bool number_hidden_fill_quiescence()
{
    rml_backend *fonts = rml_backend_create(64, 64, 1);
    number_label_fixture f = { .value = 10,
                               .max = 100,
                               .width = 180,
                               .typography = "",
                               .unit = "m" };
    rml_renderer *renderer = rml_renderer_create(&CHART_RENDERER, &f.drawn);
    f.context = rml_create_context("number-hidden-fill", 640, 400, renderer);
    f.k = test_kit_create(f.context, "studio.rml");
    f.in = (kit_frame_input){ .width = 640, .height = 400, .dp_ratio = 1 };
    number_label_frame(&f);
    static const rml_element_hooks hooks = { .resize = variable_font_resize };
    static rml_instancer *instancer;
    if (!instancer) {
        instancer = rml_instancer_create(&hooks);
        rml_register_element_instancer("number-layout-probe", instancer);
    }
    variable_font_fragment_resizes = 0;
    rml_element *document = rml_context_document(f.context, 0);
    rml_element *sibling = rml_element_query_selector(document, ".number-unrelated");
    rml_element_set_property(sibling, "width", "96dp");
    rml_element_set_inner_rml(
        sibling,
        "<number-layout-probe style='display: inline; white-space: normal'>"
        "Workspace content wraps into several fragments and stays still"
        "</number-layout-probe>"
    );
    for (int frame = 0; frame < 3; frame++) {
        number_label_frame(&f);
    }
    int layouts = variable_font_fragment_resizes, compiles = f.unrelated_compiles;
    double deadline = rml_context_next_update_delay(f.context);
    bool ok = check(compiles > 0 && layouts > 0, "bounded numeric layout probe renders wrapping workspace content");
    const double values[] = { 35, 65, 80 };
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        f.value = values[i];
        number_label_emit(&f, f.k, NULL);
        ok &= check(
            rml_context_next_update_delay(f.context) == deadline,
            "bounded non-slider live values preserve the idle context deadline"
        );
        rml_context_update(f.context);
        f.drawn = (chart_mesh){ .unrelated_compiles = &f.unrelated_compiles };
        rml_context_render(f.context);
        rml_element *label = rml_element_query_selector(document, ".number .value");
        char text[32];
        rml_element_text(rml_element_first_child(label), text, sizeof text);
        ok &= check(
            strtod(text, NULL) == f.value && f.drawn.glyphs[0].seen,
            "bounded non-slider live values still update visible numeric text"
        );
        ok &= check(variable_font_fragment_resizes == layouts && f.unrelated_compiles == compiles, "bounded non-slider live values retain sibling layouts and glyph meshes");
    }
    f.slider = true;
    number_label_frame(&f);
    rml_element *fill = rml_element_query_selector(document, ".number .fill");
    char width[32];
    rml_element_property(fill, "width", width, sizeof width);
    ok &= check(
        fabs(strtod(width, NULL) - 80) < 0.001 &&
            rml_element_box_size(fill, RML_BOX_BORDER).x > 0,
        "enabling the slider draws the latest live value percentage"
    );
    kit_rect field;
    bool found = kit_find(f.k, "app/number", &field);
    ok &= check(found, "enabled numeric slider has an interactive field");
    if (found) {
        rml_element *value = rml_element_query_selector(document, ".number .value");
        rml_vec2 at = rml_element_absolute_offset(value, RML_BOX_BORDER);
        rml_vec2 size = rml_element_box_size(value, RML_BOX_BORDER);
        float x = at.x + size.x * 0.5f, y = at.y + size.y * 0.5f;
        f.in.mouse_x = x;
        f.in.mouse_y = y;
        rml_context_process_mouse_move(f.context, (int)x, (int)y, 0);
        rml_context_process_mouse_button_down(f.context, 0, 0);
        f.in.down[0] = true;
        number_label_frame(&f);
        f.in.mouse_x = x + field.w * 0.1f;
        rml_context_process_mouse_move(f.context, (int)f.in.mouse_x, (int)y, 0);
        number_label_frame(&f);
        ok &= check(f.changed && fabs(f.value - 90) < 1, "newly enabled slider scrubs the current value across its bounded range");
        rml_context_process_mouse_button_up(f.context, 0, 0);
        f.in.down[0] = false;
        number_label_frame(&f);
        number_label_frame(&f);
        ok &= check(!kit_wants_text(f.k, NULL) && fabs(f.value - 90) < 1, "slider release retains the scrubbed value without opening text editing");
    }
    kit_destroy(f.k);
    rml_remove_context("number-hidden-fill");
    rml_release_render_managers();
    rml_renderer_destroy(renderer);
    rml_backend_destroy(fonts);
    return ok;
}

bool test_rmlui_kit()
{
    bool ok = retained_geometry();
    ok &= chart_label_updates();
    ok &= number_label_updates();
    ok &= variable_font_quiescence();
    ok &= number_hidden_fill_quiescence();
    kit_fixture f = { .value = 10,
                      .second = 3,
                      .color = 0x33669900,
                      .autocomplete_value = "Path 04",
                      .autocomplete_selected = -1,
                      .order = { "a", "b", "c" },
                      .order_count = 3,
                      .flow = 9,
                      .morph = "Play",
                      .reveal = -1 };
    f.backend = rml_backend_create(1024, 640, 1);
    f.k = test_kit_create(rml_backend_context(f.backend), "studio.rml");
    f.in = (kit_frame_input){ .dp_ratio = 1, .width = 1024, .height = 640 };
    settle(&f);
    ok &= check(rml_font_face("noto sans", RML_FONT_NORMAL, 400, 12) != 0, "Noto Sans regular face");
    ok &= check(rml_font_face("noto sans", RML_FONT_NORMAL, 600, 12) != 0, "Noto Sans semibold face");
    ok &= check(rml_font_face("noto sans", RML_FONT_NORMAL, 700, 12) != 0, "Noto Sans bold face");
    ok &= check(rml_font_face("inconsolata", RML_FONT_NORMAL, 400, 12) != 0, "Inconsolata regular face");

    /* A color field is keyboard reachable, opens from Enter, and routes focus
       to its RGBA editor. Six-digit edits retain alpha; invalid tails do not. */
    f.focus_color = true;
    frame(&f);
    rml_element *color_trigger = test_query(rml_backend_context(f.backend), ".field.color");
    bool color_focus = rml_context_focus_element(rml_backend_context(f.backend)) ==
                       color_trigger;
    key(&f, RML_INPUT_KEY_ENTER, 0);
    settle(&f);
    rml_element *hex = test_query(rml_backend_context(f.backend), ".popup.picker .hex");
    bool picker_focus = hex &&
                        rml_context_focus_element(rml_backend_context(f.backend)) == hex;
    char shown_hex[16] = "";
    if (hex) {
        rml_element_value(hex, shown_hex, sizeof shown_hex);
    }
    ok &= check(color_focus && picker_focus && !strcmp(shown_hex, "33669900"), "color trigger takes focus and Enter opens an RGBA picker focused on its value");

    if (hex) {
        rml_element_focus(hex, true);
        rml_element_select(hex);
        type(&f, "11223344");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
        ok &= check(f.color == 0x11223344, "eight-digit hex changes RGBA");

        rml_element_focus(hex, true);
        rml_element_select(hex);
        type(&f, "aabbcc");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
        ok &= check(f.color == 0xaabbcc44, "six-digit hex retains current alpha");

        rml_element_focus(hex, true);
        rml_element_select(hex);
        type(&f, "12345x");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
        rml_element *hexrow = test_query(rml_backend_context(f.backend), ".popup.picker .hexrow");
        ok &= check(
            f.color == 0xaabbcc44 && rml_element_has_attribute(hexrow, "data-invalid"),
            "partial hexadecimal input is rejected and marked invalid"
        );
    }

    rml_element *opacity = test_query(rml_backend_context(f.backend), ".popup.picker .prop .field.number");
    rml_element *percent = test_query(rml_backend_context(f.backend), ".popup.picker .prop .field.number .unit");
    char percent_text[8] = "";
    if (percent) {
        rml_element_inner_rml(percent, percent_text, sizeof percent_text);
    }
    if (hex && opacity) {
        rml_element_focus(hex, true);
        key(&f, RML_INPUT_KEY_TAB, 0);
        frame(&f);
        ok &= check(
            rml_context_focus_element(rml_backend_context(f.backend)) == opacity,
            "color picker Tab reaches Opacity without closing"
        );
        key(&f, RML_INPUT_KEY_TAB, RML_INPUT_MOD_SHIFT);
        frame(&f);
        ok &= check(rml_context_focus_element(rml_backend_context(f.backend)) == hex, "color picker Shift+Tab returns to hex");
    }
    if (opacity) {
        click_element(&f, opacity);
        type(&f, "50");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
    }
    ok &= check(
        opacity && !strcmp(percent_text, "%") && f.color == 0xaabbcc80,
        "opacity uses the bounded percent number field and updates alpha"
    );
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    settle(&f);
    ok &= check(f.color_commits == 1, "all picker edits commit as one color session");
    ok &= check(
        rml_context_focus_element(rml_backend_context(f.backend)) == color_trigger,
        "color picker Escape restores trigger focus"
    );

    rml_element *number_label = test_query(rml_backend_context(f.backend), ".prop-scroll label");
    double label_before = f.value;
    if (number_label) {
        click_element(&f, number_label);
        type(&f, "7");
        ok &= check(kit_wants_text(f.k, NULL), "number property label starts editing its field");
        rml_element *number_input = rml_context_focus_element(
            rml_backend_context(f.backend)
        );
        ok &= check(centered_input_line(number_input), "numeric caret line is centered within its full input hit box");
        kit_set_body_class(f.k, "comfortable", true);
        settle(&f);
        ok &= check(centered_input_line(number_input), "comfortable numeric input grows without stretching its caret");
        kit_set_body_class(f.k, "comfortable", false);
        settle(&f);
        key(&f, RML_INPUT_KEY_ESCAPE, 0);
        frame(&f);
        ok &= check(f.value == label_before, "number label edit cancels without changing its value");
    } else {
        ok &= check(false, "number property has an interactive label");
    }

    f.controls = true;
    snprintf(f.control_text, sizeof f.control_text, "Original");
    settle(&f);
    rml_element *select = test_query(rml_backend_context(f.backend), "#test-select");
    rml_element *toggle = test_query(rml_backend_context(f.backend), "#test-toggle");
    rml_element *control_text = test_query(rml_backend_context(f.backend), "#test-control-text input");
    rml_element *control_check = test_query(rml_backend_context(f.backend), "#test-check");
    if (select) {
        rml_element_focus(select, true);
        key(&f, RML_INPUT_KEY_ENTER, 0);
        settle(&f);
        key(&f, RML_INPUT_KEY_DOWN, 0);
        key(&f, RML_INPUT_KEY_ENTER, 0);
        settle(&f);
        ok &= check(f.select == 1, "select keyboard opens menu and chooses next label");
        rml_element_focus(select, true);
        key(&f, RML_INPUT_KEY_SPACE, 0);
        settle(&f);
        key(&f, RML_INPUT_KEY_DOWN, 0);
        key(&f, RML_INPUT_KEY_ESCAPE, 0);
        settle(&f);
        ok &= check(f.select == 1, "select Escape cancels highlighted option");
    } else {
        ok &= check(false, "select control is present");
    }
    if (toggle) {
        rml_element_focus(rml_element_child(toggle, 0), true);
        key(&f, RML_INPUT_KEY_RIGHT, 0);
        frame(&f);
        ok &= check(f.toggle == 1, "segmented choice arrow moves and selects");
    }
    if (control_check) {
        rml_element_focus(control_check, true);
        key(&f, RML_INPUT_KEY_NUMPAD_ENTER, 0);
        frame(&f);
        ok &= check(f.control_check, "keypad Enter activates a checkbox");
        key(&f, RML_INPUT_KEY_NUMPAD_ENTER, 0);
        frame(&f);
        ok &= check(!f.control_check, "keypad Enter toggles the checkbox back");
    }
    f.control_flags = KIT_READONLY;
    frame(&f);
    rml_element *text_label = control_text
                                ? rml_element_query_selector(
                                      rml_element_parent(rml_element_parent(
                                          rml_element_parent(control_text)
                                      )),
                                      "label"
                                  )
                                : NULL;
    if (text_label) {
        kit_blur(f.k);
        click_element(&f, text_label);
        ok &= check(
            rml_context_focus_element(rml_backend_context(f.backend)) != control_text,
            "readonly property label does not activate its input"
        );
    }
    if (control_text) {
        rml_element_focus(control_text, true);
        rml_element_select(control_text);
        type(&f, "Changed");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
    }
    if (toggle) {
        rml_element_focus(rml_element_child(toggle, 0), true);
        key(&f, RML_INPUT_KEY_SPACE, 0);
        key(&f, RML_INPUT_KEY_LEFT, 0);
        frame(&f);
    }
    if (select) {
        rml_element_focus(select, true);
        key(&f, RML_INPUT_KEY_ENTER, 0);
        settle(&f);
    }
    if (control_check) {
        rml_element_focus(control_check, true);
        key(&f, RML_INPUT_KEY_SPACE, 0);
        frame(&f);
    }
    ok &= check(
        !strcmp(f.control_text, "Original") && f.toggle == 1 &&
            !f.control_check && f.select == 1 &&
            !kit_dropdown_is_open(f.k, "select:app/fields/select/@control/value"),
        "readonly controls reject typing and keyboard activation"
    );
    f.control_flags = KIT_DISABLED;
    frame(&f);
    if (text_label) {
        kit_blur(f.k);
        click_element(&f, text_label);
        ok &= check(
            rml_context_focus_element(rml_backend_context(f.backend)) != control_text,
            "disabled property label does not activate its input"
        );
    }
    if (control_text) {
        click_element(&f, control_text);
        type(&f, "Changed");
        key(&f, RML_INPUT_KEY_ENTER, 0);
    }
    if (toggle) {
        click_element(&f, rml_element_child(toggle, 0));
    }
    if (select) {
        click_element(&f, select);
    }
    settle(&f);
    ok &= check(
        !strcmp(f.control_text, "Original") && f.toggle == 1 && f.select == 1 &&
            !kit_dropdown_is_open(f.k, "select:app/fields/select/@control/value"),
        "disabled controls reject pointer activation and typing"
    );
    f.control_flags = 0;
    frame(&f);
    if (control_text) {
        rml_element_focus(control_text, true);
        rml_element_select(control_text);
        type(&f, "Enabled");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
    }
    ok &= check(!strcmp(f.control_text, "Enabled"), "text becomes editable when reenabled");
    ok &= check(centered_input_line(control_text), "text caret line is shorter and centered in its input");
    if (control_text) {
        rml_vec2 hit_at = rml_element_absolute_offset(control_text, RML_BOX_BORDER);
        rml_vec2 hit = rml_element_box_size(control_text, RML_BOX_BORDER);
        kit_blur(f.k);
        pointer(&f, RML_INPUT_POINTER_MOVE, hit_at.x + hit.x / 2, hit_at.y + hit.y - 1);
        pointer(&f, RML_INPUT_POINTER_DOWN, hit_at.x + hit.x / 2, hit_at.y + hit.y - 1);
        pointer(&f, RML_INPUT_POINTER_UP, hit_at.x + hit.x / 2, hit_at.y + hit.y - 1);
        frame(&f);
        ok &= check(rml_context_focus_element(rml_backend_context(f.backend)) == control_text, "clicking input padding still focuses its full-height text hit target");
    }
    if (text_label) {
        click_element(&f, text_label);
        ok &= check(
            rml_context_focus_element(rml_backend_context(f.backend)) == control_text,
            "text property label focuses its editable input"
        );
        rml_element_select(control_text);
        type(&f, "Labeled");
        key(&f, RML_INPUT_KEY_ENTER, 0);
        frame(&f);
        ok &= check(!strcmp(f.control_text, "Labeled"), "editing from a property label commits to its value");
    } else {
        ok &= check(false, "text property has an interactive label");
    }
    f.controls = false;
    settle(&f);

    /* Highlighting: JSON keys are names, values keep their token; markup is escaped. */
    char markup[512];
    highlight_markup(HIGHLIGHT_JSON, "{\"x\": -1.5e3, \"on\": true, \"s\": \"a<b\"}", markup, sizeof markup);
    ok &= check(
        strstr(markup, "<span class=\"tok-name\">&quot;x&quot;</span>") &&
            strstr(markup, "tok-number\">-1.5e3<") &&
            strstr(markup, "tok-keyword\">true<") &&
            strstr(markup, "tok-string\">&quot;a&lt;b&quot;<"),
        "json tokens"
    );
    highlight_markup(HIGHLIGHT_LOG, "Could not open content/scenes/Sandbox.json at 42", markup, sizeof markup);
    ok &= check(
        strstr(markup, "tok-error\">Could<") &&
            strstr(markup, "tok-path\">content/scenes/Sandbox.json<") &&
            strstr(markup, "tok-number\">42<"),
        "log tokens"
    );
    /* Markup cut to fit never splits a tag or entity. */
    highlight_markup(HIGHLIGHT_LOG, "x < 1234567", markup, 30);
    ok &= check(strcmp(markup, "x ") == 0, "markup stops before a run whose tags do not fit");
    highlight_markup(HIGHLIGHT_LOG, "x < 1234567", markup, 74);
    const char *partial = "x <span class=\"tok-punct\">&lt;</span> "
                          "<span class=\"tok-number\">123</span>";
    ok &= check(strcmp(markup, partial) == 0, "markup keeps the characters of a run that fit");
    highlight_markup(HIGHLIGHT_LOG, "<<", markup, 36);
    ok &= check(strcmp(markup, "<span class=\"tok-punct\">&lt;</span>") == 0, "markup never cuts an entity");
    /* Braces would open a data expression where the markup is shown. */
    highlight_escape("a {{ b }} <c>", markup, sizeof markup);
    ok &= check(!strcmp(markup, "a &#123;&#123; b &#125;&#125; &lt;c&gt;"), "escaped markup shows braces and tags as text");
    highlight_escape("<<", markup, 8);
    ok &= check(!strcmp(markup, "&lt;"), "escaped markup is cut between whole characters");
    double parsed;
    ok &= check(
        number_expr_eval("=2*pi", 0, &parsed) &&
            fabs(parsed - 6.283185307179586) < 1e-9 &&
            number_expr_eval("*2", 3, &parsed) && parsed == 6 &&
            !number_expr_eval("2 *", 3, &parsed),
        "shared number parsing keeps expressions, relative operators, and "
        "rejects incomplete input"
    );

    ok &= check(click(&f, "app/bar/button") && f.clicks == 1, "button click reports once");
    frame(&f);
    ok &= check(f.clicks == 1, "button click does not repeat");
    rml_element *button = test_query(rml_backend_context(f.backend), "#test-press-button");
    ok &= check(button != NULL, "the keyboard button is present");
    if (button) {
        rml_element_focus(button, true);
        key(&f, RML_INPUT_KEY_NUMPAD_ENTER, 0);
        frame(&f);
        ok &= check(f.clicks == 2, "keypad Enter activates a focused button once");
    }

    /* Scrub: press, move beyond the threshold, release. One commit, no text edit.
     */
    float x, y;
    ok &= check(center(&f, "app/fields/first/@control/value", &x, &y), "number field is laid out");
    pointer(&f, RML_INPUT_POINTER_MOVE, x, y);
    pointer(&f, RML_INPUT_POINTER_DOWN, x, y);
    frame(&f);
    for (int step = 1; step <= 5; step++) {
        pointer(&f, RML_INPUT_POINTER_MOVE, x + step * 8.0f, y);
        frame(&f);
    }
    pointer(&f, RML_INPUT_POINTER_UP, x + 40, y);
    frame(&f);
    frame(&f);
    ok &= check(f.value > 10 && f.changes > 0, "scrub changes the value");
    ok &= check(f.commits == 1, "scrub commits once");
    ok &= check(!kit_wants_text(f.k, NULL), "releasing a scrub does not start editing");

    /* Click without movement edits; arithmetic relative to the value commits
     * once. */
    double before = f.value;
    f.commits = 0;
    ok &= check(click(&f, "app/fields/first/@control/value"), "number field click");
    ok &= check(kit_wants_text(f.k, NULL), "click enters text editing");
    key(&f, RML_INPUT_KEY_END, 0);
    type(&f, "*2");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    frame(&f);
    ok &= check(fabs(f.value - before * 2) < 0.01, "expression doubles the value");
    ok &= check(f.commits == 1, "text entry commits once");
    ok &= check(!kit_wants_text(f.k, NULL), "Enter leaves text editing");

    /* Escape restores the value while editing. */
    before = f.value;
    ok &= check(click(&f, "app/fields/first/@control/value"), "second edit");
    type(&f, "99");
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    ok &= check(f.value == before, "Escape restores the value");

    ok &= check(click(&f, "app/fields/first/@control/value"), "number edit before becoming readonly");
    type(&f, "99");
    f.number_flags = KIT_READONLY;
    frame(&f);
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(f.value == before && !kit_wants_text(f.k, NULL), "a number becoming readonly cancels its active draft");
    f.number_flags = 0;
    settle(&f);

    /* Invalid input keeps editing and does not change the model. */
    ok &= check(click(&f, "app/fields/first/@control/value"), "third edit");
    key(&f, RML_INPUT_KEY_BACKSPACE, 0);
    type(&f, "abc");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(kit_wants_text(f.k, NULL) && f.value == before, "invalid input stays in editing");
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);

    /* Editing preserves the complete value even when display rounds it. */
    f.value = 1.2345678901234567;
    frame(&f);
    before = f.value;
    ok &= check(click(&f, "app/fields/first/@control/value"), "precise value edit");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(f.value == before, "accepting an untouched number preserves precision");

    f.value = 0.1;
    settle(&f);
    ok &= check(click(&f, "app/fields/first/@control/value"), "decimal value edit");
    rml_element *decimal = rml_context_focus_element(rml_backend_context(f.backend));
    char decimal_text[32] = "";
    if (decimal) {
        rml_element_value(decimal, decimal_text, sizeof decimal_text);
    }
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(!strcmp(decimal_text, "0.1") && f.value == 0.1, "decimal editing shows the short value and preserves it on accept");

    f.single_precision = true;
    f.value = (double)0.1f;
    settle(&f);
    int float_changes = f.changes;
    ok &= check(click(&f, "app/fields/first/@control/value"), "promoted float edit");
    decimal = rml_context_focus_element(rml_backend_context(f.backend));
    rml_element_value(decimal, decimal_text, sizeof decimal_text);
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        !strcmp(decimal_text, "0.1") && f.value == (double)0.1f && f.changes == float_changes,
        "float editing shows 0.1 and accepting it does not change the model"
    );
    ok &= check(click(&f, "app/fields/first/@control/value"), "changed float edit");
    type(&f, "0.2");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.value == (double)0.2f && f.changes == float_changes + 1,
        "float edits normalize the changed decimal to its exact stored value"
    );
    ok &= check(click(&f, "app/fields/first/@control/value"), "overflowing float edit");
    type(&f, "1e100");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        kit_wants_text(f.k, NULL) && f.value == (double)0.2f,
        "a nonfinite float conversion stays invalid without changing the model"
    );
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    f.number_max = 1;
    frame(&f);
    ok &= check(click(&f, "app/fields/first/@control/value"), "bounded float edit");
    type(&f, "1e100");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(!kit_wants_text(f.k, NULL) && f.value == 1, "bounded float edits clamp before converting to stored precision");
    f.number_max = 0;
    f.single_precision = false;
    frame(&f);

    /* Tab follows mixed form order and commits the numeric draft once. */
    f.commits = 0;
    ok &= check(click(&f, "app/fields/first/@control/value"), "number edit before Tab");
    type(&f, "7.5");
    key(&f, RML_INPUT_KEY_TAB, 0);
    frame(&f);
    rml_element *second_field = test_query(rml_backend_context(f.backend), ".prop:nth-child(2) .field.number");
    ok &= check(
        f.value == 7.5 && f.commits == 1 &&
            rml_context_focus_element(rml_backend_context(f.backend)) == second_field,
        "Tab commits once and focuses the next field"
    );
    key(&f, RML_INPUT_KEY_ENTER, 0);
    key(&f, RML_INPUT_KEY_TAB, 0);
    frame(&f);
    rml_element *color_field = test_query(rml_backend_context(f.backend), ".field.color");
    ok &= check(
        rml_context_focus_element(rml_backend_context(f.backend)) == color_field,
        "Tab from a number reaches the color control in document order"
    );
    key(&f, RML_INPUT_KEY_TAB, RML_INPUT_MOD_SHIFT);
    frame(&f);
    ok &= check(
        rml_context_focus_element(rml_backend_context(f.backend)) == second_field,
        "Shift+Tab returns to the preceding numeric control"
    );
    ok &= check(click(&f, "app/fields/first/@control/value"), "number edit after Tab");
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);

    /* Keyboard steps: Up +1, Shift+Up +10. */
    before = f.value;
    key(&f, RML_INPUT_KEY_UP, 0);
    frame(&f);
    key(&f, RML_INPUT_KEY_UP, RML_INPUT_MOD_SHIFT);
    frame(&f);
    ok &= check(fabs(f.value - (before + 11)) < 1e-6, "arrow steps with Shift for coarse");

    before = f.value;
    key(&f, RML_INPUT_KEY_PAGE_UP, 0);
    frame(&f);
    ok &= check(f.value == before + 10, "PageUp advances a number by ten steps");
    key(&f, RML_INPUT_KEY_PAGE_DOWN, RML_INPUT_MOD_SHIFT);
    frame(&f);
    ok &= check(f.value == before, "PageDown retreats ten steps even with Shift held");

    /* A text area: Shift+Enter starts a line, Enter commits both once. */
    f.focus_note = true;
    frame(&f);
    ok &= check(kit_wants_text(f.k, NULL), "a text area takes focus");
    type(&f, "a");
    key(&f, RML_INPUT_KEY_ENTER, RML_INPUT_MOD_SHIFT);
    type(&f, "b");
    frame(&f);
    ok &= check(f.note_commits == 0 && kit_wants_text(f.k, NULL), "Shift+Enter keeps editing");
    kit_rect one, two;
    ok &= check(kit_find(f.k, "app/fields/note/@control/value", &two), "text area is laid out");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    frame(&f);
    ok &= check(strcmp(f.note, "a\nb") == 0 && f.note_commits == 1, "Enter commits the lines once");
    ok &= check(!kit_wants_text(f.k, NULL), "Enter leaves the text area");
    f.focus_note = true;
    settle(&f);
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(f.note_commits == 2, "Enter commits an unchanged optional text value");
    snprintf(f.note, sizeof f.note, "a");
    settle(&f);
    ok &= check(
        kit_find(f.k, "app/fields/note/@control/value", &one) && two.h > one.h,
        "the text area grows with its lines"
    );

    /* A choice opens an anchored searchable dropdown. Filtering keeps the
       ranked result keyboard-selectable and commits its original index. */
    kit_rect trigger;
    ok &= check(kit_find(f.k, "app/fields/choice/@control/value", &trigger), "searchable choice trigger is laid out");
    ok &= check(click(&f, "app/fields/choice/@control/value"), "searchable choice opens");
    settle(&f);
    rml_element *dropdown = test_query(rml_backend_context(f.backend), ".popup.dropdown");
    rml_vec2 dropdown_at = dropdown
                             ? rml_element_absolute_offset(dropdown, RML_BOX_BORDER)
                             : (rml_vec2){ 0 };
    rml_vec2 dropdown_size = dropdown ? rml_element_box_size(dropdown, RML_BOX_BORDER)
                                      : (rml_vec2){ 0 };
    ok &= check(
        kit_dropdown_is_open(f.k, "select:app/fields/choice/@control/value") &&
            dropdown && dropdown_size.x >= trigger.w &&
            fabsf(dropdown_at.x - trigger.x) < 1.0f && dropdown_at.y >= trigger.y,
        "choice dropdown is sized and positioned from its trigger"
    );
    ok &= check(kit_wants_text(f.k, NULL), "dropdown search takes focus");
    type(&f, "fs");
    frame(&f);
    rml_element *shown[8];
    int shown_count = dropdown
                        ? rml_element_query_selector_all(dropdown, ".dropdown-results .menu-item", shown, 8)
                        : 0;
    ok &= check(shown_count == 1, "search narrows the dropdown to its matching choice");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.choice_value == 2 && f.choice_changes == 1 &&
            !kit_dropdown_is_open(f.k, "select:app/fields/choice/@control/value"),
        "Enter selects the matching choice and closes its dropdown"
    );
    key(&f, RML_INPUT_KEY_SPACE, 0);
    frame(&f);
    dropdown = test_query(rml_backend_context(f.backend), ".popup.dropdown");
    rml_element *current = dropdown
                             ? rml_element_query_selector(dropdown, ".menu-item[data-highlighted] .label")
                             : NULL;
    char current_text[64] = "";
    if (current) {
        rml_element_inner_rml(current, current_text, sizeof current_text);
    }
    ok &= check(
        kit_dropdown_is_open(f.k, "select:app/fields/choice/@control/value") &&
            strcmp(current_text, "Frame Selected") == 0,
        "keyboard opens the choice and highlights the current value"
    );
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    ok &= check(
        !kit_dropdown_is_open(f.k, "select:app/fields/choice/@control/value") &&
            rml_context_focus_element(rml_backend_context(f.backend)) ==
                test_query(rml_backend_context(f.backend), ".prop-scroll .field.select"),
        "Escape closes the dropdown and restores focus to its field"
    );

    /* Inline autocomplete keeps the typed query in one field, returns source
       indices only on commit, and retains every match in a long group. */
    const char *source_path = "app/fields/source/@control/value";
    ok &= check(click(&f, source_path), "autocomplete opens from its input");
    settle(&f);
    rml_element *autocomplete = test_query(rml_backend_context(f.backend), ".field.autocomplete input.value");
    char shown_value[64] = "";
    if (autocomplete) {
        rml_element_value(autocomplete, shown_value, sizeof shown_value);
    }
    ok &= check(
        f.autocomplete_open && f.autocomplete_query[0] == 0 &&
            strcmp(shown_value, "Path 04") == 0,
        "autocomplete displays the committed value and opens with an empty "
        "query"
    );
    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "Path");
    frame(&f);
    rml_element *autocomplete_popup = test_query(rml_backend_context(f.backend), ".popup.autocomplete");
    rml_element *matches[16];
    int match_count = autocomplete_popup
                        ? rml_element_query_selector_all(
                              autocomplete_popup,
                              ".autocomplete-results .menu-item",
                              matches,
                              16
                          )
                        : 0;
    ok &= check(
        f.autocomplete_open && strcmp(f.autocomplete_query, "Path") == 0 &&
            strcmp(f.autocomplete_value, "Path 04") == 0 && match_count == 12,
        "typing filters without mutating the model or capping same-group "
        "matches"
    );
    rml_element *results = rml_element_query_selector(autocomplete_popup, ".autocomplete-results");
    settle(&f);
    float compact_row = rml_element_box_size(matches[0], RML_BOX_BORDER).y;
    float
        compact_pitch = rml_element_absolute_offset(matches[1], RML_BOX_BORDER).y -
                        rml_element_absolute_offset(matches[0], RML_BOX_BORDER).y;
    float list_height = rml_element_box_size(results, RML_BOX_CONTENT).y;
    ok &= check(
        results && fabsf(compact_pitch - compact_row) <= 1 &&
            fabsf(list_height - compact_pitch * 7) <= 1 &&
            rml_element_scroll_height(results) > list_height,
        "autocomplete keeps twelve choices behind exactly seven compact rows "
        "with scrolling"
    );
    kit_set_body_class(f.k, "comfortable", true);
    settle(&f);
    float comfortable_row = rml_element_box_size(matches[0], RML_BOX_BORDER).y;
    float comfortable_pitch = rml_element_absolute_offset(matches[1], RML_BOX_BORDER)
                                  .y -
                              rml_element_absolute_offset(matches[0], RML_BOX_BORDER)
                                  .y;
    list_height = rml_element_box_size(results, RML_BOX_CONTENT).y;
    ok &= check(
        fabsf(comfortable_row - compact_row) <= 1 &&
            fabsf(comfortable_pitch - compact_pitch) <= 1 &&
            fabsf(list_height - comfortable_pitch * 7) <= 1 &&
            rml_element_scroll_height(results) > list_height,
        "comfortable density retains the same compact autocomplete row pitch "
        "and seven-row viewport"
    );
    kit_set_body_class(f.k, "comfortable", false);
    settle(&f);
    rml_element *name = match_count ? rml_element_query_selector(matches[0], ".label")
                                    : NULL;
    rml_element *detail = match_count
                            ? rml_element_query_selector(matches[0], ".detail")
                            : NULL;
    char name_rml[64] = "", detail_text[32] = "";
    if (name) {
        rml_element_inner_rml(name, name_rml, sizeof name_rml);
    }
    if (detail) {
        rml_element_inner_rml(detail, detail_text, sizeof detail_text);
    }
    rml_vec2 name_at = name ? rml_element_absolute_offset(name, RML_BOX_BORDER)
                            : (rml_vec2){ 0 };
    rml_vec2 name_size = name ? rml_element_box_size(name, RML_BOX_BORDER)
                              : (rml_vec2){ 0 };
    rml_vec2 detail_at = detail ? rml_element_absolute_offset(detail, RML_BOX_BORDER)
                                : (rml_vec2){ 0 };
    ok &= check(
        name && detail && strstr(name_rml, "<b>Path</b>") &&
            !strcmp(detail_text, "float") && fabsf(name_at.y - detail_at.y) <= 1 &&
            detail_at.x >= name_at.x + name_size.x &&
            rml_element_has_attribute(autocomplete, "aria-activedescendant"),
        "completion highlights the match and places its type on the same line "
        "without overlap"
    );
    for (int i = 0; i < 7; i++) {
        key(&f, RML_INPUT_KEY_DOWN, 0);
    }
    settle(&f);
    rml_vec2 list_at = rml_element_absolute_offset(results, RML_BOX_CONTENT);
    rml_vec2 row_at = rml_element_absolute_offset(matches[7], RML_BOX_BORDER);
    list_height = rml_element_box_size(results, RML_BOX_CONTENT).y;
    ok &= check(
        rml_element_scroll_top(results) > 0 && row_at.y >= list_at.y - 1 &&
            row_at.y + compact_row <= list_at.y + list_height + 1,
        "arrow navigation scrolls the eighth dropdown option into view"
    );
    for (int i = 0; i < 4; i++) {
        key(&f, RML_INPUT_KEY_DOWN, 0);
    }
    settle(&f);
    row_at = rml_element_absolute_offset(matches[11], RML_BOX_BORDER);
    ok &= check(
        row_at.y >= list_at.y - 1 &&
            row_at.y + compact_row <= list_at.y + list_height + 1,
        "arrow navigation reveals the final dropdown option"
    );
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.autocomplete_selected == 11 &&
            strcmp(f.autocomplete_value, "Path 11") == 0 && !f.autocomplete_open,
        "keyboard can select a matching autocomplete option beyond the first "
        "eight"
    );

    ok &= check(click(&f, source_path), "autocomplete reopens at the committed value");
    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "unfinished.path.");
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    autocomplete = test_query(rml_backend_context(f.backend), ".field.autocomplete input.value");
    shown_value[0] = 0;
    if (autocomplete) {
        rml_element_value(autocomplete, shown_value, sizeof shown_value);
    }
    ok &= check(
        f.autocomplete_cancels == 1 && !f.autocomplete_open &&
            strcmp(f.autocomplete_value, "Path 11") == 0 &&
            strcmp(shown_value, "Path 11") == 0,
        "Escape cancels the draft and restores the committed display"
    );
    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "Path 02");
    frame(&f);
    ok &= check(
        f.autocomplete_open && strcmp(f.autocomplete_query, "Path 02") == 0,
        "typing after Escape reopens and filters autocomplete"
    );
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.autocomplete_selected == 2 && strcmp(f.autocomplete_value, "Path 02") == 0,
        "Enter commits the active completion"
    );

    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "Path 03");
    frame(&f);
    key(&f, RML_INPUT_KEY_TAB, 0);
    frame(&f);
    ok &= check(
        f.autocomplete_selected == 3 &&
            !strcmp(f.autocomplete_value, "Path 03") && !f.autocomplete_open &&
            rml_context_focus_element(rml_backend_context(f.backend)) == autocomplete,
        "Tab accepts the completion and keeps the caret in its input"
    );
    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "Path 04");
    frame(&f);
    key(&f, RML_INPUT_KEY_TAB, RML_INPUT_MOD_SHIFT);
    frame(&f);
    ok &= check(
        f.autocomplete_selected == 3 &&
            !strcmp(f.autocomplete_value, "Path 03") && !f.autocomplete_open &&
            rml_context_focus_element(rml_backend_context(f.backend)) != autocomplete,
        "Shift+Tab moves focus back without accepting the highlighted "
        "suggestion"
    );
    ok &= check(click(&f, source_path), "autocomplete returns to its input after backward navigation");

    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "0.5");
    frame(&f);
    ok &= check(
        !rml_element_has_attribute(autocomplete, "aria-activedescendant"),
        "an empty completion list clears its active option for assistive "
        "technology"
    );
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.autocomplete_submits == 1 &&
            strcmp(f.autocomplete_value, "0.5") == 0 && !f.autocomplete_open,
        "Enter submits valid free text when the catalog has no match"
    );

    ok &= check(click(&f, source_path), "autocomplete reopens before outside click");
    key(&f, RML_INPUT_KEY_A, RML_INPUT_MOD_CONTROL);
    type(&f, "draft text");
    frame(&f);
    ok &= check(click(&f, "app/bar/button"), "outside click dismisses autocomplete");
    autocomplete = test_query(rml_backend_context(f.backend), ".field.autocomplete input.value");
    shown_value[0] = 0;
    if (autocomplete) {
        rml_element_value(autocomplete, shown_value, sizeof shown_value);
    }
    ok &= check(
        !f.autocomplete_open && strcmp(f.autocomplete_value, "0.5") == 0 &&
            strcmp(shown_value, "0.5") == 0,
        "outside click restores the committed display"
    );

    f.autocomplete_flags = KIT_READONLY;
    settle(&f);
    ok &= check(click(&f, source_path), "read-only autocomplete can receive focus");
    settle(&f);
    ok &= check(!f.autocomplete_open, "read-only autocomplete focus does not open selectable suggestions");
    f.autocomplete_flags = 0;
    kit_blur(f.k);
    settle(&f);

    snprintf(f.autocomplete_value, sizeof f.autocomplete_value, "External.path");
    settle(&f);
    rml_element_value(autocomplete, shown_value, sizeof shown_value);
    ok &= check(strcmp(shown_value, "External.path") == 0, "an unfocused autocomplete refreshes its display after an external value change");

    /* Menubar: press opens, item activation reports once and closes. */
    ok &= check(click(&f, "app/bar/file"), "menu trigger");
    settle(&f);
    ok &= check(kit_popup_is_open(f.k, "menu:app/bar/file"), "trigger opens its menu");
    ok &= check(click(&f, "layer/@menu:app/bar/file/b"), "menu item is laid out");
    frame(&f);
    ok &= check(f.menu_b == 1 && !kit_popup_is_open(f.k, "menu:app/bar/file"), "item activates and closes the menu");
    click(&f, "app/bar/file");
    settle(&f);
    key(&f, RML_INPUT_KEY_TAB, 0);
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "menu:app/bar/file"), "Tab still dismisses a menu");
    click(&f, "app/bar/file");
    settle(&f);
    key(&f, RML_INPUT_KEY_END, 0);
    key(&f, RML_INPUT_KEY_NUMPAD_ENTER, 0);
    frame(&f);
    ok &= check(f.menu_b == 2 && !kit_popup_is_open(f.k, "menu:app/bar/file"), "keypad Enter activates the highlighted menu item and dismisses its menu");

    /* Actions use all available space; only field suggestions keep seven rows. */
    f.menu_count = 12;
    click(&f, "app/bar/file");
    settle(&f);
    rml_element *action_popup = test_query(rml_backend_context(f.backend), "#test-file-menu:not([data-ending-style])");
    rml_element *actions[12];
    int action_count = rml_element_query_selector_all(action_popup, ".menu-item", actions, 12);
    rml_vec2 action_at = rml_element_absolute_offset(action_popup, RML_BOX_CONTENT);
    float action_height = rml_element_box_size(action_popup, RML_BOX_CONTENT).y;
    bool all_actions_visible = action_count == 12;
    for (int i = 0; i < action_count; i++) {
        rml_vec2 at = rml_element_absolute_offset(actions[i], RML_BOX_BORDER);
        float height = rml_element_box_size(actions[i], RML_BOX_BORDER).y;
        all_actions_visible &= at.y >= action_at.y - 1 &&
                               at.y + height <= action_at.y + action_height + 1;
    }
    kit_rect field_rect;
    bool field_found = kit_find(f.k, "app/fields/first/@control/value", &field_rect);
    ok &= check(
        all_actions_visible &&
            rml_element_scroll_height(action_popup) <=
                rml_element_box_size(action_popup, RML_BOX_PADDING).y + 1 &&
            field_found && action_count &&
            rml_element_box_size(actions[0], RML_BOX_BORDER).y < field_rect.h,
        "a tall viewport shows all twelve actions without scrolling, in rows "
        "slimmer than editable fields"
    );
    kit_popup_close(f.k, "menu:app/bar/file");
    frame(&f);
    f.in.height = 180;
    rml_backend_resize(f.backend, f.in.width, f.in.height, 1);
    click(&f, "app/bar/file");
    settle(&f);
    action_popup = test_query(rml_backend_context(f.backend), "#test-file-menu:not([data-ending-style])");
    rml_vec2 action_border_at = rml_element_absolute_offset(action_popup, RML_BOX_BORDER);
    float action_border_height = rml_element_box_size(action_popup, RML_BOX_BORDER)
                                     .y;
    action_height = rml_element_box_size(action_popup, RML_BOX_CONTENT).y;
    ok &= check(
        action_border_at.y >= 4 - 1 &&
            action_border_at.y + action_border_height <= f.in.height - 4 + 1 &&
            rml_element_scroll_height(action_popup) >
                rml_element_box_size(action_popup, RML_BOX_PADDING).y + 1,
        "a short viewport fits the action menu's border and scrolls its "
        "overflow"
    );
    key(&f, RML_INPUT_KEY_END, 0);
    settle(&f);
    action_popup = test_query(rml_backend_context(f.backend), "#test-file-menu:not([data-ending-style])");
    action_count = rml_element_query_selector_all(action_popup, ".menu-item", actions, 12);
    action_at = rml_element_absolute_offset(action_popup, RML_BOX_PADDING);
    action_height = rml_element_box_size(action_popup, RML_BOX_PADDING).y;
    rml_vec2 final_action_at = { 0 };
    float final_action_height = 0;
    if (action_count == 12) {
        final_action_at = rml_element_absolute_offset(actions[11], RML_BOX_BORDER);
        final_action_height = rml_element_box_size(actions[11], RML_BOX_BORDER).y;
    }
    ok &= check(
        action_count == 12 && rml_element_scroll_top(action_popup) > 0 &&
            final_action_at.y >= action_at.y - 1 &&
            final_action_at.y + final_action_height <= action_at.y + action_height + 1,
        "End reveals the final action in a viewport-limited menu"
    );
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.menu_action == 12 && !kit_popup_is_open(f.k, "menu:app/bar/file"),
        "Enter activates the final action and closes the scrolling menu"
    );
    f.menu_action = 0;
    f.menu_submenu = true;
    click(&f, "app/bar/file");
    settle(&f);
    key(&f, RML_INPUT_KEY_END, 0);
    key(&f, RML_INPUT_KEY_RIGHT, 0);
    settle(&f);
    rml_element *action_submenu = test_query(rml_backend_context(f.backend), "#test-action-submenu:not([data-ending-style])");
    ok &= check(
        action_submenu && kit_popup_is_open(f.k, "sub:layer/@menu:app/bar/file/actions"),
        "Right opens the action submenu from its keyboard-selected parent row"
    );
    key(&f, RML_INPUT_KEY_END, 0);
    settle(&f);
    action_submenu = test_query(rml_backend_context(f.backend), "#test-action-submenu:not([data-ending-style])");
    bool submenu_final_visible = false;
    if (action_submenu) {
        action_count = rml_element_query_selector_all(action_submenu, ".menu-item", actions, 12);
        action_at = rml_element_absolute_offset(action_submenu, RML_BOX_PADDING);
        action_height = rml_element_box_size(action_submenu, RML_BOX_PADDING).y;
        if (action_count == 12) {
            final_action_at = rml_element_absolute_offset(actions[11], RML_BOX_BORDER);
            submenu_final_visible = rml_element_scroll_top(action_submenu) > 0 &&
                                    final_action_at.y >= action_at.y - 1 &&
                                    final_action_at.y + final_action_height <=
                                        action_at.y + action_height + 1;
        }
    }
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        submenu_final_visible && f.menu_action == 12 &&
            !kit_popup_is_open(f.k, "menu:app/bar/file") &&
            !kit_popup_is_open(f.k, "sub:layer/@menu:app/bar/file/actions"),
        "End reveals the submenu's last action; Enter activates it and closes "
        "both menu levels"
    );
    f.menu_count = 0;
    f.menu_submenu = false;
    f.in.height = 640;
    rml_backend_resize(f.backend, f.in.width, f.in.height, 1);
    settle(&f);

    /* A field in a popup takes typing and Enter; the popup keeps Escape. */
    click(&f, "app/bar/rename-trigger");
    settle(&f);
    ok &= check(kit_wants_text(f.k, NULL), "opening a popover focuses its first tabbable control");
    click(&f, "app/bar/rename-trigger");
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "rename"), "pressing a popover's ordinary button trigger again closes it");
    click(&f, "app/bar/rename-trigger");
    settle(&f);
    ok &= check(click(&f, "layer/@rename/mode"), "nested choice is laid out in its parent popup");
    settle(&f);
    ok &= check(
        kit_popup_is_open(f.k, "rename") &&
            kit_dropdown_is_open(f.k, "select:layer/@rename/mode"),
        "opening a choice keeps its parent popup open"
    );
    key(&f, RML_INPUT_KEY_DOWN, 0);
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.nested_choice == 1 && f.nested_choice_changes == 1 &&
            kit_popup_is_open(f.k, "rename") &&
            !kit_dropdown_is_open(f.k, "select:layer/@rename/mode") &&
            rml_context_focus_element(rml_backend_context(f.backend)) ==
                test_query(rml_backend_context(f.backend), ".popup .field.select"),
        "choosing a nested option updates its value and returns focus to the "
        "parent field"
    );
    ok &= check(click(&f, "layer/@rename/mode"), "nested choice reopens");
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    ok &= check(
        kit_popup_is_open(f.k, "rename") &&
            !kit_dropdown_is_open(f.k, "select:layer/@rename/mode") &&
            rml_context_focus_element(rml_backend_context(f.backend)) ==
                test_query(rml_backend_context(f.backend), ".popup .field.select"),
        "Escape closes only the nested dropdown and restores focus within its "
        "parent"
    );
    ok &= check(click(&f, "layer/@rename/name"), "popover field is laid out");
    key(&f, RML_INPUT_KEY_A, 0);
    type(&f, "ab");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(
        f.rename_commits == 1 && !strcmp(f.rename, "ab") && !f.rename_applied &&
            kit_popup_is_open(f.k, "rename"),
        "a popover's field takes letters and Enter instead of the menu's "
        "typeahead"
    );
    ok &= check(click(&f, "layer/@rename/name"), "popover field refocuses");
    key(&f, RML_INPUT_KEY_TAB, 0);
    frame(&f);
    rml_element *popover_button = rml_context_focus_element(rml_backend_context(f.backend));
    ok &= check(
        popover_button && !strcmp(rml_element_tag_name(popover_button), "button") &&
            kit_popup_is_open(f.k, "rename"),
        "Tab moves from a popover field to its button without closing"
    );
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(f.rename_applied == 1 && kit_popup_is_open(f.k, "rename"), "Enter activates a focused popover button");
    key(&f, RML_INPUT_KEY_TAB, RML_KM_SHIFT);
    frame(&f);
    ok &= check(
        kit_wants_text(f.k, NULL) && kit_popup_is_open(f.k, "rename"),
        "Shift+Tab moves from a popover button back to its field"
    );
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "rename"), "Escape in a popover's field closes the popover");
    rml_element *restored = rml_context_focus_element(rml_backend_context(f.backend));
    char restored_label[32] = "";
    if (restored) {
        rml_element *label = rml_element_query_selector(restored, ".label");
        if (label) {
            rml_element_inner_rml(label, restored_label, sizeof restored_label);
        }
    }
    ok &= check(!strcmp(restored_label, "Rename"), "Escape restores focus to the popover's trigger");
    click(&f, "app/bar/rename-trigger");
    settle(&f);
    key(&f, RML_INPUT_KEY_TAB, RML_KM_SHIFT);
    frame(&f);
    ok &= check(
        !kit_popup_is_open(f.k, "rename") && !kit_wants_text(f.k, NULL),
        "Shift+Tab from the first popover control exits to the trigger "
        "and closes"
    );
    click(&f, "app/bar/rename-trigger");
    settle(&f);
    key(&f, RML_INPUT_KEY_TAB, 0);
    key(&f, RML_INPUT_KEY_TAB, 0);
    key(&f, RML_INPUT_KEY_TAB, 0);
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "rename"), "Tab from the last popover control exits to the document and closes");
    click(&f, "app/bar/rename-trigger");
    settle(&f);
    rml_element *outside = test_query(rml_backend_context(f.backend), ".topbar > button");
    ok &= check(outside && rml_element_focus(outside, true), "outside document controls remain focusable while a popover is open");
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "rename"), "moving focus to an outside control dismisses a nonmodal popover");
    f.rename_empty = true;
    click(&f, "app/bar/rename-trigger");
    settle(&f);
    rml_element *empty_focus = rml_context_focus_element(rml_backend_context(f.backend));
    ok &= check(
        empty_focus && rml_element_is_class_set(empty_focus, "popover"),
        "an informational popover focuses itself when it has no tabbable "
        "controls"
    );
    rml_element_focus(outside, true);
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "rename"), "an informational popover also closes on outside focus");
    f.rename_empty = false;
    f.b_renames = true;
    ok &= check(click(&f, "app/bar/file"), "menu trigger reopens");
    settle(&f);
    ok &= check(click(&f, "layer/@menu:app/bar/file/b"), "menu item is laid out again");
    settle(&f);
    ok &= check(
        kit_popup_is_open(f.k, "rename") && kit_wants_text(f.k, NULL),
        "a popover a menu item opens keeps its field's focus as the menu closes"
    );
    type(&f, "c");
    key(&f, RML_INPUT_KEY_ENTER, 0);
    frame(&f);
    ok &= check(!strcmp(f.rename, "c"), "typing replaces the selection the move kept");
    kit_popup_close(f.k, "rename");
    frame(&f);
    ok &= check(!kit_popup_is_open(f.k, "rename"), "close closes a popup at once");

    /* Tooltips: the description wraps under the label, and a zoomed canvas
       keeps the tooltip's size on screen. */
    char about[256];
    rml_vec2 tip = tip_shown(&f, "app/bar/about", about, sizeof about);
    ok &= check(!strcmp(about, TIP_ABOUT), "a tooltip shows its description");
    ok &= check(tip.x < 300 && tip.y > 40, "a tooltip's description wraps");
    f.in.zoom = 2;
    rml_vec2 zoomed = tip_shown(&f, "app/bar/about", about, sizeof about);
    f.in.zoom = 0;
    ok &= check(zoomed.x < tip.x * 0.6f, "a zoomed canvas's tooltip undoes the zoom");

    /* Keyed children reorder and prune without recreating survivors. */
    float ay, cy;
    kit_rect removed;
    settle(&f);
    ok &= check(
        center(&f, "app/list/a", &x, &ay) && center(&f, "app/list/c", &x, &cy) && ay < cy,
        "initial order"
    );
    f.order[0] = "c";
    f.order[1] = "a";
    f.order_count = 2;
    settle(&f);
    ok &= check(center(&f, "app/list/a", &x, &ay) && center(&f, "app/list/c", &x, &cy) && cy < ay, "reordered");
    ok &= check(!kit_find(f.k, "app/list/b", &removed), "unemitted child is removed");

    /* Virtualized list: a window of rows, and reveal scrolls a far row into
     * existence. */
    ok &= check(f.rows_emitted > 0 && f.rows_emitted < 40, "list emits only the visible window");
    ok &= check(!kit_find(f.k, "app/big/r999", &removed), "far rows are not emitted");
    f.reveal = 999;
    settle(&f);
    ok &= check(kit_find(f.k, "app/big/r999", &removed) && f.rows_emitted < 40, "reveal scrolls to the row");

    /* Hover finds the row under the pointer, never an overscan row laid out
       past the list's edge and clipped. */
    kit_rect big;
    ok &= check(kit_find(f.k, "app/big", &big), "list is laid out");
    pointer(&f, RML_INPUT_POINTER_MOVE, big.x + 5, big.y + 5);
    settle(&f);
    ok &= check(f.hovered_row >= 0, "the row under the pointer is hovered");
    pointer(&f, RML_INPUT_POINTER_MOVE, big.x + 5, big.y - 5);
    settle(&f);
    ok &= check(f.hovered_row < 0, "an overscan row outside the list is not hovered");

    /* Drag and drop: picked up after FEEL_DRAG_DP of movement, never by a
     * still hold, and dropped on the target under the pointer. */
    float sx = 0, sy = 0, tx = 0, ty = 0;
    ok &= check(
        center(&f, "app/dnd/source", &sx, &sy) &&
            center(&f, "app/dnd/target", &tx, &ty),
        "drag parts are laid out"
    );
    pointer(&f, RML_INPUT_POINTER_MOVE, sx, sy);
    pointer(&f, RML_INPUT_POINTER_DOWN, sx, sy);
    f.in.down[0] = true;
    for (int i = 0; i < 30; i++) {
        frame(&f);
    }
    ok &= check(!kit_dragging(f.k, "thing"), "a press held still for half a second is no drag");
    pointer(&f, RML_INPUT_POINTER_MOVE, sx + FEEL_DRAG_DP - 1, sy);
    frame(&f);
    ok &= check(!kit_dragging(f.k, "thing"), "no drag under the drag threshold");
    pointer(&f, RML_INPUT_POINTER_MOVE, tx, ty);
    frame(&f);
    ok &= check(kit_dragging(f.k, "thing"), "drag starts past the drag threshold");
    frame(&f);
    frame(&f);
    rml_element *over = test_query(rml_backend_context(f.backend), "[data-drop-over]");
    rml_element *hint = test_query(rml_backend_context(f.backend), ".drag-overlay[data-hint] .hint");
    char hint_text[32] = "";
    if (hint) {
        rml_element_inner_rml(hint, hint_text, sizeof hint_text);
    }
    ok &= check(
        f.drop_hovered && !strcmp(f.hovered, "apple") && over &&
            rml_element_has_attribute(over, "data-drop-accept") &&
            !strcmp(hint_text, "Put here"),
        "a target of several types hears the drag over it and names what a "
        "drop does"
    );
    pointer(&f, RML_INPUT_POINTER_UP, tx, ty);
    f.in.down[0] = false;
    frame(&f);
    ok &= check(strcmp(f.dropped, "apple") == 0 && !kit_dragging(f.k, NULL), "drop lands on the target");
    /* Escape puts a drag back: nothing lands, even released over a target. */
    f.dropped[0] = '\0';
    pointer(&f, RML_INPUT_POINTER_MOVE, sx, sy);
    pointer(&f, RML_INPUT_POINTER_DOWN, sx, sy);
    f.in.down[0] = true;
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_MOVE, tx, ty);
    frame(&f);
    key(&f, RML_INPUT_KEY_ESCAPE, 0);
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_UP, tx, ty);
    f.in.down[0] = false;
    frame(&f);
    ok &= check(!f.dropped[0] && !kit_dragging(f.k, NULL), "Escape cancels a drag");
    /* As in browsers, only a press after a click double-clicks: RmlUi
     * counts a press after any press on the same element, so a quick press
     * on a row just dragged from would open it. */
    float px = 0, py = 0;
    ok &= check(center(&f, "app/dnd/pear", &px, &py), "the draggable row is laid out");
    pointer(&f, RML_INPUT_POINTER_MOVE, px, py);
    pointer(&f, RML_INPUT_POINTER_DOWN, px, py);
    f.in.down[0] = true;
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_MOVE, tx, ty);
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_UP, tx, ty);
    f.in.down[0] = false;
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_MOVE, px, py);
    pointer(&f, RML_INPUT_POINTER_DOWN, px, py);
    pointer(&f, RML_INPUT_POINTER_UP, px, py);
    frame(&f);
    ok &= check(f.row_double_clicks == 0, "a quick press on a row just dragged from is a click, not a double click");
    for (int i = 0; i < 2; i++) {
        pointer(&f, RML_INPUT_POINTER_DOWN, px, py);
        pointer(&f, RML_INPUT_POINTER_UP, px, py);
    }
    frame(&f);
    ok &= check(f.row_double_clicks == 1, "a double click on the row after it still double-clicks");

    /* NumberFlow and Torph settle to exactly what a fresh render of the new
     * value lays out. */
    f.flow = 10;
    f.morph = "Pause";
    frame(&f);
    kit_rect flow, flow_ref, morph, morph_ref;
    ok &= check(kit_find(f.k, "app/motion/morph", &morph) && morph.w > 0, "morph lays out while moving");
    for (int i = 0; i < 90; i++) {
        frame(&f);
    }
    ok &= check(
        kit_find(f.k, "app/motion/flow", &flow) &&
            kit_find(f.k, "app/motion/flow_ref", &flow_ref) && flow.w > 0 &&
            fabsf(flow.w - flow_ref.w) < 0.5f,
        "number flow gains a place and settles"
    );
    ok &= check(
        kit_find(f.k, "app/motion/morph", &morph) &&
            kit_find(f.k, "app/motion/morph_ref", &morph_ref) &&
            fabsf(morph.w - morph_ref.w) < 0.5f,
        "morph settles to the new text"
    );

    /* Toasts: an action reports once and dismisses; an untouched toast expires
     * after four seconds. */
    int undo = kit_toast(f.k, 0, &(kit_toast_desc){ .title = "Deleted", .action = "Undo" });
    for (int i = 0; i < 60; i++) {
        frame(&f);
    }
    ok &= check(click(&f, "layer/@toaster/t1/action"), "toast action is laid out");
    frame(&f);
    ok &= check(kit_toast_action(f.k, undo) && !kit_toast_action(f.k, undo), "toast action reports once");
    pointer(&f, RML_INPUT_POINTER_MOVE, 10, 10);
    kit_toast(f.k, 0, &(kit_toast_desc){ .title = "Saved" });
    for (int i = 0; i < 60; i++) {
        frame(&f);
    }
    ok &= check(kit_find(f.k, "layer/@toaster/t2", &removed), "toast shows");
    for (int i = 0; i < 60 * 4; i++) {
        frame(&f);
    }
    ok &= check(!kit_find(f.k, "layer/@toaster/t2", &removed), "toast expires");

    kit_toast(f.k, 0, &(kit_toast_desc){ .title = "Swipe" });
    for (int i = 0; i < 60; i++) {
        frame(&f);
    }
    float toast_x = 0, toast_y = 0;
    ok &= check(center(&f, "layer/@toaster/t3", &toast_x, &toast_y), "swipe toast is laid out");
    pointer(&f, RML_INPUT_POINTER_MOVE, toast_x, toast_y);
    pointer(&f, RML_INPUT_POINTER_DOWN, toast_x, toast_y);
    f.in.down[0] = true;
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_MOVE, toast_x + 20, toast_y);
    frame(&f);
    pointer(&f, RML_INPUT_POINTER_UP, toast_x + 20, toast_y);
    f.in.down[0] = false;
    frame(&f);
    char toast_transform[96] = "";
    float swipe_x = 0;
    rml_element *swipe_toast = test_query(rml_backend_context(f.backend), ".toast");
    bool has_swipe_velocity = swipe_toast &&
                              rml_element_property(swipe_toast, "transform", toast_transform, sizeof toast_transform) <
                                  sizeof toast_transform &&
                              sscanf(toast_transform, "translate(%fdp, %*fdp) scale(%*f)", &swipe_x) ==
                                  1;
    ok &= check(has_swipe_velocity && swipe_x > 35, "toast dismissal carries a quick swipe's velocity into its spring");

    /* Highlighting fills at most the runs it is given, none included. */
    ok &= check(highlight_runs(HIGHLIGHT_EXPRESSION, "a + 1", 5, NULL, 0) == 0, "highlighting without capacity writes nothing");

    /* Text measures in dp, as drawn at a pixel ratio. */
    float hello = kit_text_width(f.k, "Hello", 12, 400, 1);
    ok &= check(hello > 20 && hello < 40, "text measures in dp");
    float large = kit_text_width(f.k, "Hello", 24, 400, 1);
    ok &= check(fabsf(large / (2 * hello) - 1) < 0.1f, "text width follows size");
    ok &= check(kit_text_width(f.k, "Hello", 12, 400, 2) == large / 2, "text measures as drawn at a ratio");
    ok &= check(kit_text_width(f.k, "Hello", 12, 700, 1) > hello, "bold text is wider");

    /* A second context, like the zoomed graph canvas, comes and goes while the
       first keeps working. */
    rml_backend *second = rml_backend_create(200, 100, 2);
    kit *canvas = second ? test_kit_create(rml_backend_context(second), "graph.rml")
                         : NULL;
    ok &= check(canvas != NULL, "a second context loads its document");
    if (canvas) {
        kit_frame_input in = { .dp_ratio = 2, .width = 200, .height = 100 };
        kit_begin(canvas, &in);
        kit_root(canvas, "world");
        kit_label(canvas, "node", NULL, "Node");
        kit_end(canvas);
        rml_backend_update(second, 1.0 / 60);
        kit_rect node;
        ok &= check(kit_find(canvas, "world/node", &node) && node.w > 0, "second context lays out");
        kit_destroy(canvas);
    }
    rml_backend_destroy(second);
    ok &= check(click(&f, "app/bar/button"), "first context works after the second closes");

    kit_destroy(f.k);
    rml_backend_destroy(f.backend);
    return ok;
}
