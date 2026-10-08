#include "editor/studio/studio_graph_internal.h"
#include "platform/cute.h"
#include "support/array.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_graph_culling(studio *s, node *n);

void test_studio_graph_zoom(studio *s, node *n)
{
    assert(s->graph.kit && n && n->input_count > 0 && n->output_count > 0);
    float old_zoom = s->graph.zoom;
    float baseline[4] = { 0 }, input[2] = { 0 }, output[2] = { 0 };
    bool stable = true;
    const float zooms[] = { 0.72f, 0.83f, 1.0f, 1.17f, 1.4f, 1.8f };
    for (int i = 0; i < (int)(sizeof zooms / sizeof *zooms); i++) {
        s->graph.zoom = zooms[i];
        sg_calculations_invalidate(s);
        float box[4], input_at[2], output_at[2];
        sg_node_box(s, n, box);
        sg_socket_at(s, n, ED_PIN_INPUT, 0, &input_at[0], &input_at[1]);
        sg_socket_at(s, n, ED_PIN_OUTPUT, 0, &output_at[0], &output_at[1]);
        if (i == 0) {
            memcpy(baseline, box, sizeof box);
            memcpy(input, input_at, sizeof input_at);
            memcpy(output, output_at, sizeof output_at);
        }
        for (int axis = 0; axis < 4; axis++) {
            stable &= fabsf(box[axis] - baseline[axis]) < 0.001f;
        }
        for (int axis = 0; axis < 2; axis++) {
            stable &= fabsf(input_at[axis] - input[axis]) < 0.001f &&
                      fabsf(output_at[axis] - output[axis]) < 0.001f;
        }
    }
    s->graph.zoom = old_zoom;
    sg_calculations_invalidate(s);
    test_check(stable, "node bounds and wire anchors stay fixed in graph units while zoom changes");
    test_graph_culling(s, n);
}

static void emit_graph(studio *s)
{
    cf_app_update(NULL);
    kit_begin(s->graph.kit, &s->graph.in);
    sg_emit_canvas(s);
    kit_end(s->graph.kit);
    rml_backend_update(s->graph.backend, 1.0 / 60);
    rml_backend_render(s->graph.backend);
    cf_app_draw_onto_screen(true);
}

static int emitted_nodes(studio *s)
{
    graph *gr = sg_active_graph(s);
    int count = 0;
    for (int i = 0; i < acount(gr->nodes); i++) {
        char path[96];
        kit_rect rect;
        snprintf(
            path,
            sizeof path,
            "world/stage/n%llu",
            (unsigned long long)gr->nodes[i]->id
        );
        count += kit_find(s->graph.kit, path, &rect);
    }
    return count;
}

static void test_graph_culling(studio *s, node *n)
{
    studio_graph *g = &s->graph;
    float old_x = g->x, old_y = g->y;
    editor *e = sg_graph_editor(s);
    ed_clear_selection(e);
    g->hover_node = g->press_node = g->link_node = 0;
    g->x = g->y = 100000;
    emit_graph(s);
    test_check(emitted_nodes(s) == 0, "offscreen graph nodes are omitted from the emitted canvas");
    bool anchored = acount(g->wire_edges) > 0;
    for (int w = 0; w < acount(g->wire_edges); w++) {
        float points[4];
        anchored &= kit_wire_points(g->kit, "back/wires", w, points, 2) >= 2;
    }
    test_check(anchored, "wire paths retain model anchors when both endpoint nodes are unmounted");
    ed_select_node(e, n, false);
    emit_graph(s);
    test_check(emitted_nodes(s) == 1, "offscreen selected node remains emitted for keyboard editing");
    ed_clear_selection(e);
    g->value_editing = true;
    g->value_edit_node_id = n->id;
    emit_graph(s);
    test_check(emitted_nodes(s) == 1, "offscreen edited node remains mounted until editing ends");
    g->value_editing = false;
    g->value_edit_node_id = 0;
    float box[4];
    sg_node_box(s, n, box);
    g->x = box[0] + box[2] + 64 / sg_zoom(s);
    g->y = box[1];
    emit_graph(s);
    char path[96];
    kit_rect rect;
    snprintf(path, sizeof path, "world/stage/n%llu", (unsigned long long)n->id);
    test_check(kit_find(g->kit, path, &rect), "graph overscan retains nodes just outside the viewport");
    /* Exercise retention through real DOM focus, without selected/edit flags. */
    g->x = box[0] - 32;
    g->y = box[1] - 32;
    emit_graph(s);
    rml_context *context = rml_backend_context(g->backend);
    rml_element *field = NULL;
    for (int d = 0; d < rml_context_document_count(context) && !field; d++) {
        rml_element *inputs[32];
        int count = rml_element_query_selector_all(
            rml_context_document(context, d),
            ".gnode input",
            inputs,
            32
        );
        for (int i = 0; i < count && i < 32; i++) {
            char input_path[256];
            if (kit_element_path(g->kit, inputs[i], input_path, sizeof input_path) &&
                !strncmp(input_path, path, strlen(path)) &&
                input_path[strlen(path)] == '/') {
                field = inputs[i];
                break;
            }
        }
    }
    bool focused = field && rml_element_focus(field, true) &&
                   rml_context_focus_element(context) == field;
    test_check(focused, "graph culling fixture focuses a real shown node input");
    ed_clear_selection(e);
    g->hover_node = g->press_node = g->link_node = 0;
    rml_context_process_mouse_leave(context);
    g->x = g->y = 100000;
    emit_graph(s);
    char focused_path[256];
    bool mounted = focused && kit_find(g->kit, path, &rect) &&
                   rml_context_focus_element(context) == field &&
                   kit_element_path(g->kit, field, focused_path, sizeof focused_path);
    test_check(mounted, "offscreen unselected node input remains mounted with actual DOM focus");
    kit_blur(g->kit);
    test_check(rml_context_focus_element(context) != field, "blur releases actual input focus before offscreen culling");
    emit_graph(s);
    test_check(!kit_find(g->kit, path, &rect), "blur releases the offscreen focused node for culling");

    int edge_index = g->wire_edges[0];
    graph *gr = sg_active_graph(s);
    edge *ed = &gr->edges[edge_index];
    node *from, *to;
    int from_pin, to_pin;
    assert(sg_edge_ends(gr, ed, &from, &from_pin, &to, &to_pin));
    float from_x = from->x, from_y = from->y, to_x = to->x, to_y = to->y;
    float width = g->in.width, height = g->in.height;
    int reroutes = ed->reroute_count;
    typeof(ed->reroutes) old_reroutes;
    memcpy(old_reroutes, ed->reroutes, sizeof old_reroutes);
    kit_wire_style style = s->prefs.wires;
    /* Make this an explicit horizontal crossing, independent of automatic
       lanes around the fixture's other nodes and fan-out wires. */
    ed->reroute_count = 2;
    ed->reroutes[0] = (typeof(*ed->reroutes)){ 0, 64 };
    ed->reroutes[1] = (typeof(*ed->reroutes)){ 400, 64 };
    s->prefs.wires = KIT_WIRE_STRAIGHT;
    from->x = -2000;
    to->x = 2000;
    from->y = 64 - sg_pin_y(s, from, ED_PIN_OUTPUT, from_pin);
    to->y = 64 - sg_pin_y(s, to, ED_PIN_INPUT, to_pin);
    g->x = g->y = 0;
    g->in.width = 400;
    g->in.height = 128;
    rml_backend_resize_pixels(
        g->backend,
        (int)lroundf(400 * g->in.dp_ratio),
        (int)lroundf(128 * g->in.dp_ratio),
        g->in.dp_ratio
    );
    emit_graph(s);
    char from_path[96], to_path[96];
    snprintf(
        from_path,
        sizeof from_path,
        "world/stage/n%llu",
        (unsigned long long)from->id
    );
    snprintf(
        to_path,
        sizeof to_path,
        "world/stage/n%llu",
        (unsigned long long)to->id
    );
    test_check(
        !kit_find(g->kit, from_path, &rect) && !kit_find(g->kit, to_path, &rect),
        "crossing-wire endpoints can both be outside the emitted viewport"
    );
    int crossing_wire = -1;
    for (int w = 0; w < acount(g->wire_edges); w++) {
        if (g->wire_edges[w] == edge_index) {
            crossing_wire = w;
        }
    }
    float crossing_points[512];
    int crossing_count = kit_wire_points(g->kit, "back/wires", crossing_wire, crossing_points, 256);
    bool crosses = false;
    for (int p = 1; p < crossing_count && p < 256; p++) {
        float ax = crossing_points[(p - 1) * 2], bx = crossing_points[p * 2];
        float ay = crossing_points[(p - 1) * 2 + 1], by = crossing_points[p * 2 + 1];
        crosses |= fminf(ax, bx) <= 200 && fmaxf(ax, bx) >= 200 &&
                   fabsf(ay - 64) < 0.01f && fabsf(by - 64) < 0.01f;
    }
    test_check(crosses, "offscreen-endpoint wire has a rendered horizontal crossing inside the viewport");
    g->in.mouse_x = 200;
    g->in.mouse_y = 64;
    /* kit_hovered_path reads the actual RmlUi hover element. Updating only
       frame-input coordinates can leave an earlier node as the hover target. */
    rml_context_process_mouse_move(
        rml_backend_context(g->backend),
        (int)lroundf(200 * g->in.dp_ratio),
        (int)lroundf(64 * g->in.dp_ratio),
        0
    );
    g->press_button = -1;
    g->gesture = STUDIO_GRAPH_IDLE;
    sg_pointer(s);
    test_check(g->hover_edge == edge_index, "wire crossing the viewport remains hit-testable with both endpoint nodes culled");
    g->press_button = 0;
    g->press_path[0] = '\0';
    g->press_control = false;
    g->double_click = false;
    g->in.modifiers = 0;
    sg_pointer(s);
    ed_link_ref link;
    sg_link_ref_of(ed, &link);
    test_check(ed_link_selected(e, &link), "crossing wire remains selectable without endpoint DOM");
    ed_clear_selection(e);
    g->press_button = -1;
    g->gesture = STUDIO_GRAPH_LINK;
    g->x = 35;
    g->y = 19;
    g->link_node = from->id;
    g->link_kind = ED_PIN_OUTPUT;
    g->link_pin = from_pin;
    g->hover_node = to->id;
    g->hover_kind = ED_PIN_INPUT;
    g->hover_pin = to_pin;
    g->world_x = 120;
    g->world_y = 80;
    emit_graph(s);
    float points[512], a[2], b[2];
    sg_socket_at(s, from, ED_PIN_OUTPUT, from_pin, &a[0], &a[1]);
    sg_socket_at(s, to, ED_PIN_INPUT, to_pin, &b[0], &b[1]);
    int count = kit_wire_points(g->kit, "front/link", 0, points, 256);
    test_check(
        count >= 2 && count <= 256 && fabsf(points[0] - a[0] + g->x) < 0.01f &&
            fabsf(points[1] - a[1] + g->y) < 0.01f &&
            fabsf(points[(count - 1) * 2] - b[0] + g->x) < 0.01f &&
            fabsf(points[(count - 1) * 2 + 1] - b[1] + g->y) < 0.01f,
        "draft connection snaps to numeric model anchors under a panned camera"
    );
    g->link_node = to->id;
    g->link_kind = ED_PIN_INPUT;
    g->link_pin = to_pin;
    g->hover_node = from->id;
    g->hover_kind = ED_PIN_OUTPUT;
    g->hover_pin = from_pin;
    emit_graph(s);
    count = kit_wire_points(g->kit, "front/link", 0, points, 256);
    test_check(
        count >= 2 && count <= 256 && fabsf(points[0] - a[0] + g->x) < 0.01f &&
            fabsf(points[(count - 1) * 2] - b[0] + g->x) < 0.01f,
        "dragging from an input preserves output-to-input draft wire direction"
    );
    g->hover_kind = ED_PIN_INPUT;
    g->hover_node = to->id;
    g->hover_pin = to_pin;
    emit_graph(s);
    count = kit_wire_points(g->kit, "front/link", 0, points, 256);
    test_check(
        count >= 2 && count <= 256 && fabsf(points[0] - g->world_x + g->x) < 0.01f &&
            fabsf(points[1] - g->world_y + g->y) < 0.01f,
        "incompatible draft target follows the world pointer instead of "
        "snapping"
    );
    g->gesture = STUDIO_GRAPH_IDLE;
    g->link_node = 0;
    g->hover_node = 0;
    g->hover_kind = g->hover_pin = -1;
    from->x = from_x;
    from->y = from_y;
    to->x = to_x;
    to->y = to_y;
    ed->reroute_count = reroutes;
    memcpy(ed->reroutes, old_reroutes, sizeof old_reroutes);
    s->prefs.wires = style;
    g->in.width = width;
    g->in.height = height;
    rml_backend_resize_pixels(
        g->backend,
        (int)lroundf(width * g->in.dp_ratio),
        (int)lroundf(height * g->in.dp_ratio),
        g->in.dp_ratio
    );
    g->in.mouse_x = g->in.mouse_y = -1e9f;
    g->hover_node = 0;
    g->x = old_x;
    g->y = old_y;
    emit_graph(s);
}
