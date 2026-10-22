#include "graph/authoring/graph_edit.h"
/* The Behavior area: drawing a graph never changes what its asset saves,
 * so reloading a viewed asset leaves it saved; layout, calculations, drags,
 * value controls and completion follow edits and Undo. */

#include "assets/code_source.h"
#include "assets/ui_events.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/input.h"
#include "graph/document/serialize.h"
#include "graph/model/reactive.h"
#include "graph/model/reactive_presentation.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/compare_nodes.h"
#include "graph/nodes/component_nodes.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "render/screenshot.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/text_file.h"
#include "support/text_overlay.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/input_feel.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pin_spec VALUE_CONTROL_INPUTS[] = {
    { .name = "Packed Color",
      .type = PIN_U32,
      .type_name = "unsigned int",
      .flags = PIN_FLAG_COLOR,
      .default_value = { .type = PIN_U32, .u = 0xf0cd69ff } },
    { .name = "Vector Color",
      .type = PIN_STRUCT,
      .type_name = "CF_V3",
      .flags = PIN_FLAG_COLOR,
      .default_value = { .type = PIN_STRUCT, .type_name = "CF_V3" } },
    { .name = "Shape",
      .type = PIN_ENUM,
      .type_name = "GameShape",
      .default_value = { .type = PIN_ENUM, .type_name = "GameShape", .e = 1 } },
};
static node_desc VALUE_CONTROL_NODE = { .name = "Value Control Fixture",
                                        .input_count = 3,
                                        .inputs = VALUE_CONTROL_INPUTS };

void test_studio_graph_zoom(studio *s, node *n);
void test_studio_menu_context(studio *s);

static void graph_key(studio *s, rml_input_key key)
{
    (void)s;
    assert(key == RML_INPUT_KEY_ENTER);
    key_event(SDL_SCANCODE_RETURN, true);
    key_event(SDL_SCANCODE_RETURN, false);
}

static void graph_text(studio *s, const char *text)
{
    (void)s;
    text_event(text);
}

static void graph_press_rect(studio *s, kit_rect rect)
{
    studio_graph *g = &s->graph;
    float x = g->surface.rect.x + (rect.x + rect.w * 0.5f) * g->in.zoom;
    float y = g->surface.rect.y + (rect.y + rect.h * 0.5f) * g->in.zoom;
    mouse_move(s, x, y);
    for (int down = 1; down >= 0; down--) {
        SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                         : SDL_EVENT_MOUSE_BUTTON_UP };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = x * s->prefs.ui_scale;
        event.button.y = y * s->prefs.ui_scale;
        assert(SDL_PushEvent(&event));
    }
}

static bool graph_value_control_shown(studio_fixture *f, node *n, int pin)
{
    char path[128];
    kit_rect rect;
    const pin_spec *spec = &n->input_pins[pin];
    char control[32];
    if (sg_value_kind_of(f->studio, n, 0, pin) == VALUE_COLOR) {
        snprintf(control, sizeof control, "pin-%llu", (unsigned long long)spec->id);
    } else {
        snprintf(control, sizeof control, "%s", spec->flags & PIN_FLAG_SETTING ? "v" : "source-edit");
    }
    snprintf(
        path,
        sizeof path,
        "world/stage/n%llu/b/i%d/%s",
        (unsigned long long)n->id,
        pin,
        control
    );
    return kit_find(f->studio->graph.kit, path, &rect) && rect.w > 0 && rect.h > 0;
}

/* Color presentation reaches the graph canvas for declared pins and reflected
   component fields; enum metadata continues to select its catalog control. */
static void graph_value_controls_use_metadata()
{
    studio_fixture f = fixture_new();
    graph *gr = f.graph->active_graph;
    node *values = graph_add_node(gr, &VALUE_CONTROL_NODE);
    assert(values);
    values->x = 420;
    values->y = 90;
    values->placed = true;
    node *set = graph_add_node(gr, registry_node_by_name("Set Component"));
    assert(set && component_node_retarget(gr, set, "GameAppearance") == 0);
    assert(component_node_split_value(gr, set, true) == 0);
    set->x = 120;
    set->y = 90;
    set->placed = true;
    int color_pin = -1;
    for (int i = 0; i < set->input_count; i++) {
        if (set->input_pins[i].name && !strcmp(set->input_pins[i].name, "color")) {
            color_pin = i;
            break;
        }
    }
    assert(color_pin >= 0);
    bool kinds = sg_value_kind_of(f.studio, values, 0, 0) == VALUE_COLOR &&
                 sg_value_kind_of(f.studio, values, 0, 1) == VALUE_COLOR &&
                 sg_value_kind_of(f.studio, values, 0, 2) == VALUE_CHOICE &&
                 sg_value_kind_of(f.studio, set, 0, color_pin) == VALUE_COLOR;
    frames(&f, 3);
    bool controls = graph_value_control_shown(&f, values, 0) &&
                    graph_value_control_shown(&f, values, 1) &&
                    graph_value_control_shown(&f, values, 2) &&
                    graph_value_control_shown(&f, set, color_pin);
    test_check(kinds && controls, "graph pins draw color pickers from pin and reflected metadata and enums use choices");

    fixture_free(&f);
}

static void graph_color_session_saves_undoes_and_replays()
{
    studio_fixture f = fixture_new();
    graph *gr = f.graph->active_graph;
    node *set = graph_add_node(gr, registry_node_by_name("Set Component"));
    assert(set && component_node_retarget(gr, set, "GameAppearance") == 0);
    assert(component_node_split_value(gr, set, true) == 0);
    set->x = 120;
    set->y = 90;
    set->placed = true;
    int color_pin = -1;
    for (int i = 0; i < set->input_count; i++) {
        if (set->input_pins[i].name && !strcmp(set->input_pins[i].name, "color")) {
            color_pin = i;
            break;
        }
    }
    assert(color_pin >= 0);
    uint64_t set_id = set->id;
    uint32_t initial = set->input_pins[color_pin].default_value.u;
    bool can_undo_before = editor_can_undo(f.graph);
    char path[128];
    snprintf(
        path,
        sizeof path,
        "world/stage/n%llu/b/i%d/pin-%llu",
        (unsigned long long)set_id,
        color_pin,
        (unsigned long long)set->input_pins[color_pin].id
    );
    frames(&f, 3);
    kit_rect control;
    assert(kit_find(f.studio->graph.kit, path, &control));
    graph_press_rect(f.studio, control);
    frames(&f, 3);
    rml_element *hex = test_query(rml_backend_context(f.studio->graph.backend), ".popup.picker .hex");
    if (hex) {
        rml_element_focus(hex, true);
        rml_element_select(hex);
        graph_text(f.studio, "482b86ff");
        graph_key(f.studio, RML_INPUT_KEY_ENTER);
        frames(&f, 2);
    }
    const graph *active = editor_active_graph(f.graph);
    node *edited = graph_node_by_id(active, set_id);
    bool changed = hex && edited &&
                   edited->input_pins[color_pin].default_value.u == 0x482b86ff;
    if (kit_find(f.studio->graph.kit, path, &control)) {
        graph_press_rect(f.studio, control);
    }
    frames(&f, 3);

    char *saved_document = behavior_asset_save(editor_behavior_asset(f.graph));
    behavior_asset *saved = saved_document ? behavior_asset_load(saved_document) : NULL;
    free(saved_document);
    graph *saved_graph = saved ? behavior_asset_find_graph(saved, active->graph_id)
                               : NULL;
    node *saved_set = saved_graph ? graph_node_by_id(saved_graph, set_id) : NULL;
    bool persisted = saved_set &&
                     saved_set->input_pins[color_pin].default_value.u == 0x482b86ff;
    if (saved) {
        behavior_asset_free(saved);
    }

    sg_update_routes(f.studio);
    unsigned before_undo = f.studio->graph.calculations->route_preparations;
    editor_undo(f.graph);
    active = editor_active_graph(f.graph);
    node *undone_set = graph_node_by_id(active, set_id);
    bool undone = undone_set &&
                  undone_set->input_pins[color_pin].default_value.u == initial &&
                  editor_can_undo(f.graph) == can_undo_before &&
                  editor_can_redo(f.graph);
    sg_update_routes(f.studio);
    sg_find_unused(f.studio);
    bool undo_routes = f.studio->graph.calculations->route_preparations > before_undo;
    editor_redo(f.graph);
    active = editor_active_graph(f.graph);
    node *redone_set = graph_node_by_id(active, set_id);
    bool redone = redone_set &&
                  redone_set->input_pins[color_pin].default_value.u == 0x482b86ff;
    sg_update_routes(f.studio);
    sg_find_unused(f.studio);
    bool current_routes = true;
    for (int i = 0; i < acount(f.studio->graph.calculations->routing.nodes); i++) {
        node *routed = f.studio->graph.calculations->routing.nodes[i];
        current_routes &= graph_node_by_id(active, routed->id) == routed;
    }
    test_check(undo_routes && current_routes, "undo and redo replace cached routing inputs with the restored graph");
    test_check(changed && persisted && undone && redone, "a graph color picker saves its RGBA value and streams one undoable session that replays");
    fixture_free(&f);
}

static void reload_keeps_viewed_graph_saved()
{
    studio_fixture f = fixture_new();
    for (int i = 0; i < 3; i++) {
        frame(&f, false);
    }
    frame(&f, true);
    for (int i = 0; i < 3; i++) {
        frame(&f, false);
    }
    const node *setter = graph_node_by_id(editor_active_graph(f.graph), SETTER);
    test_check(setter && setter->x != 0, "Studio arranges the reloaded graph it shows");
    const game_preview_definition *weapons = game_preview_find_definition(f.live, "Weapons");
    test_check(!weapons->unsaved, "reloading a library Studio drew leaves it saved");
    fixture_free(&f);
}

static void graph_layout_does_not_wiggle()
{
    studio_fixture f = fixture_new();
    for (int i = 0; i < 3; i++) {
        frame(&f, false);
    }
    test_studio_graph_zoom(f.studio, graph_node_by_id(editor_active_graph(f.graph), SETTER));
    fixture_free(&f);
}

static void graph_calculations_follow_edits()
{
    studio_fixture f = fixture_new();
    frames(&f, 3);
    studio *s = f.studio;
    graph *gr = sg_active_graph(s);
    node *n = graph_add_node(gr, &VALUE_CONTROL_NODE);
    assert(n);
    node_layout before = *sg_layout_of(s, n);
    sg_update_routes(s);
    sg_find_unused(s);
    struct studio_graph_calculations *c = s->graph.calculations;
    unsigned measured = c->measurements, prepared = c->route_preparations;
    unsigned reachable = c->reachability_builds;
    s->graph.x += 150;
    s->graph.y -= 70;
    s->graph.zoom *= 0.9f;
    node_layout panned = *sg_layout_of(s, n);
    sg_update_routes(s);
    sg_find_unused(s);
    test_check(
        c->measurements == measured && c->route_preparations == prepared &&
            c->reachability_builds == reachable && before.w == panned.w &&
            before.h == panned.h,
        "camera movement retains intrinsic measurements, routing input and "
        "reachability"
    );

    n->x += 37;
    n->y -= 19;
    sg_update_routes(s);
    bool moved = false;
    for (int i = 0; i < acount(c->routing.nodes); i++) {
        if (c->routing.nodes[i] == n) {
            moved = c->routing.pos[i].x == n->x && c->routing.pos[i].y == n->y;
        }
    }
    test_check(moved && c->route_preparations == prepared, "uncommitted node movement updates routing positions without preparing topology again");

    assert(graph_set_pin_name(gr, n, 0, 0, "An intentionally much longer reflected input label") == 0);
    node_layout renamed = *sg_layout_of(s, n);
    sg_update_routes(s);
    sg_find_unused(s);
    test_check(renamed.w > before.w && c->route_preparations > prepared && c->reachability_builds > reachable, "pin label edits invalidate measured geometry and dependent calculations");
    unsigned after_label = c->measurements;
    s->prefs.standard_font_size = 18;
    (void)sg_layout_of(s, n);
    test_check(c->measurements > after_label, "font presentation changes invalidate intrinsic measurements");

    static pin_spec choice_output = { .name = "Shape",
                                      .type = PIN_ENUM,
                                      .type_name = "GameShape" };
    static node_desc choice_source = { .name = "Cache Choice Source",
                                       .output_count = 1,
                                       .outputs = &choice_output };
    node *source = graph_add_node(gr, &choice_source);
    assert(source && graph_connect_data(gr, source, 0, n, 2) == 0);
    assert(graph_set_node_presentation(gr, source, NODE_PRESENTATION_EXPANDED) == 0);
    sg_update_routes(s);
    int expanded_count = acount(c->routing.nodes);
    assert(graph_set_node_presentation(gr, source, NODE_PRESENTATION_INLINE) == 0);
    sg_update_routes(s);
    sg_find_unused(s);
    test_check(!sg_node_visible(gr, source) && acount(c->routing.nodes) == expanded_count - 1, "folding removes the source box and its route input while retaining its editable model");
    assert(graph_remove_node(gr, source) == 0);

    uint64_t id = n->id;
    assert(graph_remove_node(gr, n) == 0);
    sg_update_routes(s);
    sg_find_unused(s);
    bool removed = !sg_is_unused(&s->graph, id);
    for (int i = 0; i < acount(c->routing.nodes); i++) {
        removed &= c->routing.nodes[i]->id != id;
    }
    for (int i = 0; i < acount(s->graph.layouts); i++) {
        removed &= s->graph.layouts[i].id != id;
    }
    test_check(removed, "removed nodes leave measurement, route and unused caches together");
    fixture_free(&f);
}

static void graph_drag_retains_calculations_and_undo()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    studio_graph *g = &s->graph;
    node *n = graph_node_by_id(sg_active_graph(s), SETTER);
    assert(n);
    g->camera.active = false;
    g->framed = true;
    g->zoom = 1;
    g->x = n->x - 80;
    g->y = n->y - 80;
    frames(&f, 3);
    float old_x = n->x, old_y = n->y;
    int undo_before = acount(f.graph->undo);
    kit_rect title;
    assert(kit_find(g->kit, "world/stage/n10247/h/t", &title));
    float x = g->surface.rect.x + (title.x + title.w / 2) * sg_zoom(s);
    float y = g->surface.rect.y + (title.y + title.h / 2) * sg_zoom(s);
    mouse_move(s, x, y);
    graph_drag_button(s, x, y, true);
    frame(&f, false);
    mouse_move(s, x + 20, y + 12);
    frame(&f, false);
    bool moving = g->gesture == STUDIO_GRAPH_MOVE && f.graph->pending_snapshot &&
                  !editor_has_pending_edits(f.graph);
    struct studio_graph_calculations *c = g->calculations;
    unsigned measured = c->measurements, prepared = c->route_preparations;
    unsigned reachable = c->reachability_builds;
    bool cached = moving, follows = moving, anchored = moving;
    for (int step = 1; step <= 3; step++) {
        float dx = 20 + (float)step * 10, dy = 12 + (float)step * 8;
        mouse_move(s, x + dx, y + dy);
        frame(&f, false);
        cached &= g->gesture == STUDIO_GRAPH_MOVE && f.graph->pending_snapshot &&
                  !editor_has_pending_edits(f.graph) &&
                  c->measurements == measured && c->route_preparations == prepared &&
                  c->reachability_builds == reachable;
        follows &= fabsf(n->x - old_x - dx / sg_zoom(s)) < 0.01f &&
                   fabsf(n->y - old_y - dy / sg_zoom(s)) < 0.01f &&
                   g->hover_node == SETTER;
        bool wire = false;
        graph *gr = sg_active_graph(s);
        for (int w = 0; w < acount(g->wire_edges); w++) {
            const edge *ed = &gr->edges[g->wire_edges[w]];
            if (ed->from_id != SETTER) {
                continue;
            }
            int output, pin;
            assert(graph_pin_by_id(n, ed->from_pin_id, &output, &pin) && output);
            float sx, sy, points[4];
            sg_socket_at(s, n, ED_PIN_OUTPUT, pin, &sx, &sy);
            wire |= kit_wire_points(g->kit, "back/wires", w, points, 2) >= 2 &&
                    fabsf(points[0] - (sx - g->x)) < 0.5f &&
                    fabsf(points[1] - (sy - g->y)) < 0.5f;
        }
        anchored &= wire;
    }
    test_check(cached, "real held node dragging retains intrinsic measurements, route preparation and reachability while its undo snapshot is pending");
    test_check(follows && anchored, "real node dragging updates pointer picking and rendered wire anchors on each movement");
    graph_drag_button(s, x + 50, y + 36, false);
    frame(&f, false);
    bool committed = g->gesture == STUDIO_GRAPH_IDLE && !f.graph->pending_snapshot &&
                     acount(f.graph->undo) == undo_before + 1;
    editor_undo(f.graph);
    frames(&f, 3);
    n = graph_node_by_id(sg_active_graph(s), SETTER);
    test_check(
        committed && n && fabsf(n->x - old_x) < 0.01f &&
            fabsf(n->y - old_y) < 0.01f && acount(f.graph->undo) == undo_before,
        "releasing a real drag commits one undo step that restores the "
        "original node position"
    );
    test_studio_menu_context(s);
    fixture_free(&f);
}

/* Physical Tab commits the semantic row marked as selected after filtering,
   rather than relying on the visual resemblance of adjacent completion rows. */
static void graph_completion_tab_matches_highlight()
{
    studio_fixture f = fixture_open(
        "test/fixtures/studio-workspace",
        "test/fixtures/studio-workspace/content/scenes/Sandbox.chibi",
        "Weapon"
    );
    f.studio = studio_new(f.scene, f.viewport, "test/fixtures/studio-workspace");
    studio *s = f.studio;
    studio_maximize_graph(s);
    frames(&f, 3);
    node *expression = graph_node_by_id(sg_active_graph(s), 48);
    assert(expression);
    int pin = -1;
    for (int i = 0; i < expression->input_count; i++) {
        if (!strcmp(expression->input_pins[i].name, "elapsed")) {
            pin = i;
        }
    }
    assert(pin >= 0);
    char path[128];
    snprintf(path, sizeof path, "world/stage/n48/b/i%d/source-edit", pin);
    kit_rect rect;
    assert(kit_find(s->graph.kit, path, &rect));
    graph_press_rect(s, rect);
    frames(&f, 3);
    rml_context *context = rml_backend_context(s->graph.backend);
    rml_element *input = rml_context_focus_element(context);
    assert(input && !strcmp(rml_element_tag_name(input), "input"));
    rml_element_select(input);
    text_event("Tr");
    frames(&f, 3);
    mouse_move(
        s,
        s->graph.surface.rect.x + 20,
        s->graph.surface.rect.y + s->graph.surface.rect.h - 20
    );
    frames(&f, 24);
    rml_element *selected = test_query(context, ".popup.autocomplete .menu-item[data-highlighted] .label");
    char label[256] = "";
    if (selected) {
        rml_element_inner_rml(selected, label, sizeof label);
    }
    char plain[256] = "";
    size_t length = 0;
    bool tag = false;
    for (const char *at = label; *at; at++) {
        if (*at == '<') {
            tag = true;
        } else if (*at == '>') {
            tag = false;
        } else if (!tag) {
            plain[length++] = *at;
        }
    }
    char expected[2] = { 0 };
    if (!strcmp(plain, "Transform \xE2\x80\xBA X")) {
        expected[0] = 'x';
    } else if (!strcmp(plain, "Transform \xE2\x80\xBA Y")) {
        expected[0] = 'y';
    }
    key_event(SDL_SCANCODE_TAB, true);
    frames(&f, 8);
    key_event(SDL_SCANCODE_TAB, false);
    frames(&f, 8);
    expression = graph_node_by_id(sg_active_graph(s), 48);
    const pin_binding *binding = graph_pin_binding(sg_active_graph(s), expression, 0, pin);
    bool same = selected && expected[0] && binding &&
                binding->kind == PIN_BINDING_COMPONENT_FIELD &&
                binding->component_key &&
                !strcmp(binding->component_key, "GAME.GameTransform") &&
                binding->field_name && !strcmp(binding->field_name, expected);
    if (!same) {
        fprintf(
            stderr,
            "completion Tab: highlighted=%s expected_field=%s "
            "accepted_component=%s accepted_field=%s\n",
            label,
            expected,
            binding && binding->component_key ? binding->component_key : "none",
            binding && binding->field_name ? binding->field_name : "none"
        );
    }
    test_check(same, "settled graph completion Tab accepts the displayed highlighted semantic field");
    fixture_free(&f);
}

static void replay_close_stop_reopen_keeps_graph_layout()
{
    studio_fixture f = fixture_open(
        "test/fixtures/studio-workspace",
        "test/fixtures/studio-workspace/content/scenes/Sandbox.chibi",
        "Weapon"
    );
    f.studio = studio_new(f.scene, f.viewport, "test/fixtures/studio-workspace");
    studio *s = f.studio;
    studio_maximize_graph(s);
    frames(&f, 3);
    graph *gr = sg_active_graph(s);
    uint64_t lifetime = gr->lifetime_id;
    uint64_t *ids = NULL;
    float *positions = NULL;
    float lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        float box[4];
        if (!sg_node_visible(gr, n) || !sg_node_box(s, n, box)) {
            continue;
        }
        apush(ids, n->id);
        apush(positions, n->x);
        apush(positions, n->y);
        lo[0] = fminf(lo[0], box[0]);
        lo[1] = fminf(lo[1], box[1]);
        hi[0] = fmaxf(hi[0], box[0] + box[2]);
        hi[1] = fmaxf(hi[1], box[1] + box[3]);
    }
    assert(acount(ids) > 1 && hi[0] - lo[0] > 360);
    behavior_asset *before = behavior_asset_clone(editor_behavior_asset(f.graph));
    assert(before);
    assert(app_scene_authoring_open_behavior(f.scene, "Movement") == 0);
    bool pending = f.graph->arrange_pending;
    char *checkpoint = editor_checkpoint(f.graph);
    assert(checkpoint);
    bool checkpoint_restored = editor_restore_checkpoint(f.graph, checkpoint);
    free(checkpoint);
    test_check(pending && checkpoint_restored && f.graph->arrange_pending, "a native checkpoint taken before the first graph frame retains pending automatic arrangement");
    frames(&f, 3);
    assert(app_scenario_replay(f.scene, "Firefight") == 0);
    frames(&f, 3);
    int weapon = tab_named(f.scene, "Weapon");
    assert(weapon >= 0 && !scene_tabs_dirty(f.scene, &f.scene->tabs[weapon]));
    studio_close_tab(s, weapon);
    frames(&f, 2);
    int movement = tab_named(f.scene, "Movement");
    assert(movement >= 0 && !scene_tabs_dirty(f.scene, &f.scene->tabs[movement]));
    studio_close_tab(s, movement);
    frames(&f, 2);
    bool hidden = tab_named(f.scene, "Weapon") < 0 &&
                  tab_named(f.scene, "Movement") < 0 && !studio_graph_active(s);
    studio_runtime(s, APP_RUNTIME_STOP);
    frame(&f, false);
    assert(app_scene_authoring_open_behavior(f.scene, "Weapon") == 0);
    studio_maximize_graph(s);
    frames(&f, 3);
    gr = sg_active_graph(s);
    bool positions_equal = gr != NULL;
    for (int i = 0; i < acount(ids); i++) {
        node *n = gr ? graph_node_by_id(gr, ids[i]) : NULL;
        positions_equal &= n && fabsf(n->x - positions[i * 2]) < 0.5f &&
                           fabsf(n->y - positions[i * 2 + 1]) < 0.5f;
    }
    bool restored = hidden && gr && gr->lifetime_id != lifetime && positions_equal;
    char *before_text = behavior_asset_save(before);
    char *after_text = behavior_asset_save(editor_behavior_asset(f.graph));
    bool authored_equal = before_text && after_text &&
                          !strcmp(before_text, after_text);
    if (!restored || !authored_equal) {
        size_t different = 0;
        if (before_text && after_text) {
            while (before_text[different] &&
                   before_text[different] == after_text[different]) {
                different++;
            }
        }
        printf(
            "  replay layout: hidden %d, fresh %d, positions %d, authored %d, "
            "byte %zu\n",
            hidden,
            gr && gr->lifetime_id != lifetime,
            positions_equal,
            authored_equal,
            different
        );
        printf(
            "  before: %.160s\n  after: %.160s\n",
            before_text ? before_text + different : "missing",
            after_text ? after_text + different : "missing"
        );
    }
    free(before_text);
    free(after_text);
    test_check(
        restored && authored_equal,
        "Firefight replay, closing both graph tabs, Stop and reopening Weapon "
        "restore its automatic positions without authoring or saving an edit"
    );
    behavior_asset_free(before);

    sg_frame_nodes(s, false, false);
    frame(&f, false);
    bool fitted = true;
    for (int i = 0; i < acount(ids); i++) {
        float box[4];
        node *n = graph_node_by_id(gr, ids[i]);
        fitted &= sg_node_box(s, n, box) && box[0] >= s->graph.x - 1 &&
                  box[1] >= s->graph.y - 1 &&
                  box[0] + box[2] <= s->graph.x + s->graph.in.width + 1 &&
                  box[1] + box[3] <= s->graph.y + s->graph.in.height + 1;
    }
    bool wires = acount(s->graph.wire_edges) > 0;
    for (int w = 0; w < acount(s->graph.wire_edges); w++) {
        const edge *ed = &gr->edges[s->graph.wire_edges[w]];
        node *from, *to;
        int from_pin, to_pin;
        assert(sg_edge_ends(gr, ed, &from, &from_pin, &to, &to_pin));
        int count = kit_wire_points(s->graph.kit, "back/wires", w, NULL, 0);
        float *points = xmalloc((size_t)(count ? count : 1) * 2 * sizeof *points);
        kit_wire_points(s->graph.kit, "back/wires", w, points, count);
        float ax, ay, bx, by;
        sg_socket_at(s, from, ED_PIN_OUTPUT, from_pin, &ax, &ay);
        sg_socket_at(s, to, ED_PIN_INPUT, to_pin, &bx, &by);
        wires &= count >= 2 && fabsf(points[0] - ax + s->graph.x) < 0.5f &&
                 fabsf(points[1] - ay + s->graph.y) < 0.5f &&
                 fabsf(points[(count - 1) * 2] - bx + s->graph.x) < 0.5f &&
                 fabsf(points[(count - 1) * 2 + 1] - by + s->graph.y) < 0.5f;
        free(points);
    }
    test_check(fitted && wires, "Frame fits the restored graph spread and every rendered wire keeps its restored socket anchors");
    afree(ids);
    afree(positions);
    fixture_free(&f);
}

/* A press on a node moves it only once it travels FEEL_DRAG_DP. */
static void graph_drag_threshold()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    studio_graph *g = &s->graph;
    node *n = graph_node_by_id(sg_active_graph(s), SETTER);
    assert(n);
    g->camera.active = false;
    g->framed = true;
    g->zoom = 1;
    g->x = n->x - 80;
    g->y = n->y - 80;
    frames(&f, 3);
    float old_x = n->x;
    kit_rect title;
    assert(kit_find(g->kit, "world/stage/n10247/h/t", &title));
    float x = g->surface.rect.x + (title.x + title.w / 2) * sg_zoom(s);
    float y = g->surface.rect.y + (title.y + title.h / 2) * sg_zoom(s);
    mouse_move(s, x, y);
    graph_drag_button(s, x, y, true);
    frame(&f, false);
    mouse_move(s, x + FEEL_DRAG_DP - 1, y);
    frame(&f, false);
    bool still = g->gesture != STUDIO_GRAPH_MOVE && n->x == old_x;
    mouse_move(s, x + FEEL_DRAG_DP + 1, y);
    frame(&f, false);
    bool moved = g->gesture == STUDIO_GRAPH_MOVE && n->x != old_x;
    graph_drag_button(s, x + FEEL_DRAG_DP + 1, y, false);
    frame(&f, false);
    editor_undo(f.graph);
    frames(&f, 2);
    test_check(still && moved, "a node drags once the press travels the drag threshold, and not before");
    fixture_free(&f);
}

/* A font-size preference scales the graph as a whole. The DOM hit boxes,
   native socket positions and pointer conversion must still describe one
   coordinate system, while the saved canvas zoom stays user-controlled. */
static bool graph_hit_geometry(studio *s)
{
    studio_graph *canvas = &s->graph;
    graph *gr = sg_active_graph(s);
    bool found_socket = false;
    if (!canvas->kit || !gr || fabsf(canvas->in.zoom - sg_zoom(s)) > 0.001f ||
        fabsf(canvas->in.dp_ratio - studio_graph_pixel_ratio(s)) > 0.001f) {
        return false;
    }
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        if (!sg_node_visible(gr, n)) {
            continue;
        }
        float box[4];
        sg_node_box(s, n, box);
        char path[64];
        snprintf(path, sizeof path, "world/stage/n%llu", (unsigned long long)n->id);
        kit_rect rendered;
        bool found = kit_find(canvas->kit, path, &rendered);
        if (!found || fabsf(rendered.x - (box[0] - canvas->x)) > 0.5f ||
            fabsf(rendered.y - (box[1] - canvas->y)) > 0.5f ||
            fabsf(rendered.w - box[2]) > 0.5f || fabsf(rendered.h - box[3]) > 0.5f) {
            return false;
        }
        const node_layout *layout = sg_layout_of(s, n);
        for (int kind = ED_PIN_INPUT; kind <= ED_PIN_OUTPUT; kind++) {
            int count = kind == ED_PIN_INPUT ? n->input_count : n->output_count;
            char side = kind == ED_PIN_INPUT ? 'i' : 'o';
            for (int pin = 0; pin < count; pin++) {
                bool in_head = sg_in_head(layout, kind, pin);
                if (!in_head && !sg_pin_in_rows(gr, n, layout, kind, pin)) {
                    continue;
                }
                if (in_head) {
                    snprintf(
                        path,
                        sizeof path,
                        "world/stage/n%llu/h/%c%d/s",
                        (unsigned long long)n->id,
                        side,
                        pin
                    );
                } else if (sg_pin_exec(n, kind, pin)) {
                    snprintf(
                        path,
                        sizeof path,
                        "world/stage/n%llu/b/e%d/%c%d/s",
                        (unsigned long long)n->id,
                        sg_pin_order(gr, n, layout, kind, pin),
                        side,
                        pin
                    );
                } else {
                    snprintf(
                        path,
                        sizeof path,
                        "world/stage/n%llu/b/%c%d/s",
                        (unsigned long long)n->id,
                        side,
                        pin
                    );
                }
                kit_rect socket;
                float x, y, world_x, world_y;
                sg_socket_at(s, n, kind, pin, &x, &y);
                found = kit_find(canvas->kit, path, &socket);
                if (!found || fabsf(socket.x + socket.w / 2 - (x - canvas->x)) > 0.5f ||
                    fabsf(socket.y + socket.h / 2 - (y - canvas->y)) > 0.5f) {
                    return false;
                }
                found_socket = true;
                float screen_x = canvas->surface.rect.x +
                                 (x - canvas->x) * sg_zoom(s);
                float screen_y = canvas->surface.rect.y +
                                 (y - canvas->y) * sg_zoom(s);
                sg_to_world(s, screen_x, screen_y, &world_x, &world_y);
                if (fabsf(world_x - x) >= 0.01f || fabsf(world_y - y) >= 0.01f) {
                    return false;
                }
            }
        }
    }
    return found_socket;
}

static void expression_edit_keeps_socket_geometry()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    behavior_expression_error error = { 0 };
    node *n = editor_add_behavior_expression(
        f.graph,
        "first_value + second_value + third_value + fourth_value + fifth_value "
        "+ sixth_value",
        120,
        90,
        &error
    );
    assert(n);
    uint64_t id = n->id;
    assert(
        !graph_set_pin_display_name(f.graph->active_graph, n, 0, 0, "normalize_vector_2(Value: &component(\"GAME.GameVelocity\"))")
    );
    n->placed = true;
    frames(&f, 3);
    n = graph_node_by_id(f.graph->active_graph, id);
    assert(n);
    node_layout before = *sg_layout_of(s, n);
    bool bounded = before.code_lines > 1 && before.label < before.w - 22;
    char path[96];
    snprintf(path, sizeof path, "world/stage/n%llu/b/i0/l", (unsigned long long)id);
    kit_rect label;
    bounded &= kit_find(s->graph.kit, path, &label) &&
               label.x + label.w <= n->x - s->graph.x + before.w - 1;
    bool static_geometry = graph_hit_geometry(s);
    sg_begin_expression_edit(s, s->graph.kit, n, 0);
    frames(&f, 3);
    n = graph_node_by_id(f.graph->active_graph, id);
    node_layout editing = *sg_layout_of(s, n);
    bool editing_geometry = graph_hit_geometry(s) &&
                            before.top == editing.top && before.h == editing.h;
    s->graph.input_edit_node = 0;
    s->graph.input_edit_kit = NULL;
    frames(&f, 3);
    test_check(bounded && static_geometry && editing_geometry && graph_hit_geometry(s), "long expression labels remain inside their node and editing preserves rendered socket and wire geometry");
    fixture_free(&f);
}

static void graph_font_size_preserves_hit_geometry()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    studio_graph *g = &s->graph;
    g->camera.active = false;
    g->zoom = 0.8f;
    g->x = -40;
    g->y = -40;
    g->framed = true;
    frames(&f, 2);
    bool standard = fabsf(g->zoom - 0.8f) < 0.001f && graph_hit_geometry(s);
    s->prefs.standard_font_size = 18;
    frames(&f, 2);
    bool enlarged = fabsf(g->zoom - 0.8f) < 0.001f &&
                    fabsf(g->in.zoom - 1.2f) < 0.001f && graph_hit_geometry(s);
    test_check(
        standard && enlarged,
        "graph font scaling keeps rendered node and socket hit geometry "
        "aligned without changing the stored zoom"
    );
    fixture_free(&f);
}

/* No window is needed for generated binding presentation rules. */
void studio_documents_reactive_presentation_checks()
{
    code_source *source = code_source_parse(
        "behavior Binding { bind self.ParticleEmitter.Active = "
        "self.ParticleEmitter.Parent.ActionBinding.Pressed; }"
    );
    assert(source->asset && acount(source->asset->reactives) == 1);
    behavior_asset *asset = behavior_asset_clone(source->asset);
    code_source_free(source);
    const behavior_reactive *r = &asset->reactives[0];
    graph *g = behavior_asset_find_graph(asset, r->graph_id);
    node *set = graph_node_by_id(g, r->statement_node_id);
    node *event = graph_node_by_id(g, r->event_id);
    node *read = graph_node_by_id(g, r->source_node_id);
    node *branch = NULL, *guard = NULL;
    int pin;
    assert(graph_exec_output_target(g, event, 0, 0, &branch, &pin));
    for (int i = 0; i < branch->input_count; i++) {
        if (!strcmp(branch->input_pins[i].name, "Condition")) {
            assert(graph_input_source(g, branch, i, &guard, &pin));
        }
    }
    assert(guard);
    test_check(
        sg_node_visible(g, set) && !sg_node_visible(g, event) &&
            !sg_node_visible(g, branch) && !sg_node_visible(g, guard) &&
            behavior_reactive_authored_node(g, event) == set &&
            behavior_reactive_authored_node(g, guard) == set,
        "binding guards stay hidden and source/debug targets present the "
        "editable Set"
    );
    node *authored = graph_add_node(g, &NODE_Not_Equal);
    int output = behavior_reactive_output_index(read, r->source_pin_id);
    assert(output >= 0);
    for (int i = 0; i < authored->input_count; i++) {
        assert(
            graph_set_pin_type(
                g,
                authored,
                false,
                i,
                PIN_BOOL,
                read->output_pins[output].type_name
            ) == 0
        );
    }
    assert(output >= 0 && graph_connect_data(g, read, output, authored, 0) == 0);
    test_check(sg_node_visible(g, authored) && behavior_reactive_authored_node(g, read) == read, "binding presentation preserves an authored comparison and its shared read-back source");
    behavior_asset_free(asset);
}

void studio_documents_expression_geometry_checks()
{
    expression_edit_keeps_socket_geometry();
}

void studio_documents_graph_checks()
{
    studio_documents_reactive_presentation_checks();
    graph_layout_does_not_wiggle();
    graph_calculations_follow_edits();
    graph_drag_retains_calculations_and_undo();
    graph_completion_tab_matches_highlight();
    replay_close_stop_reopen_keeps_graph_layout();
    graph_value_controls_use_metadata();
    graph_color_session_saves_undoes_and_replays();
    reload_keeps_viewed_graph_saved();
    graph_drag_threshold();
    graph_font_size_preserves_hit_geometry();
    expression_edit_keeps_socket_geometry();
}
