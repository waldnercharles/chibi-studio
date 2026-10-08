#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "graph/authoring/local_edit.h"
#include "execution/ecs_reader.h"
#include "editor/workspace/editor_internal.h"
#include "graph/authoring/layout.h"
#include "graph/authoring/optimize.h"
#include "graph/document/extract.h"
#include "graph/model/local.h"
#include "graph/nodes/math_nodes.h"
#include "graph/shader/shader_graph.h"
#include "runtime/systems/system.h"
#include "test/suite_internal.h"

#include <assert.h>

static node *add(graph *g, const char *name)
{
    const node_desc *d = registry_node_by_name(name);
    assert(d);
    node *n = graph_add_node(g, d);
    assert(n);
    return n;
}

static node *sink(graph *g, pin_type type, const char *name)
{
    node *n = add(g, "Reroute Data");
    assert(graph_set_pin_type(g, n, 0, 0, type, name) == 0);
    return n;
}

static double evaluate(graph *g, node *n, behavior_instance *instance, double delta, const service_binding *services)
{
    arena a;
    arena_init(&a, 4096);
    eval_context ctx;
    eval_context_init(&ctx);
    ctx.instance = instance;
    ctx.delta_seconds = delta;
    ctx.services = services;
    pin_value value = { 0 };
    int result = graph_eval_output(g, n, 0, &a, &value, &ctx);
    if (result) {
        fprintf(stderr, "optimizer eval: %s\n", ctx.status.message);
    }
    assert(!result);
    double number = value.type == PIN_F32 ? value.f
                  : value.type == PIN_F64 ? value.d
                                          : value.i;
    eval_context_free(&ctx);
    arena_free(&a);
    return number;
}

static behavior_asset *copy_asset(const behavior_asset *asset)
{
    char *json = behavior_asset_save(asset);
    assert(json);
    behavior_asset *copy = behavior_asset_load(json);
    if (!copy) {
        fprintf(stderr, "optimizer copy: %s\n", graph_last_error());
    }
    assert(copy);
    free(json);
    return copy;
}

static void component_fanout()
{
    behavior_asset *asset = behavior_asset_new("Optimizer components");
    behavior_asset_set_owner_type(asset, "ecs.entity");
    graph *g = asset->graph;
    node *get = add(g, "Get_GameTransform");
    assert(get->desc->binding_component);
    assert(graph_expose_output_fields(g, get, 0) == 0);
    node *x = sink(g, PIN_F32, "float");
    node *y = sink(g, PIN_F32, "float");
    node *x2 = sink(g, PIN_F32, "float");
    assert(graph_connect_data(g, get, 1, x, 0) == 0);
    assert(graph_connect_data(g, get, 2, y, 0) == 0);
    assert(graph_connect_data(g, get, 1, x2, 0) == 0);
    behavior_variable *snapshot = behavior_asset_add_variable(
        asset,
        "Snapshot",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32 },
        NULL
    );
    uint64_t snapshot_id = snapshot->id;
    node *event = graph_add_node(g, &NODE_Event);
    assert(graph_set_event_name(g, event, "Sample") == 0);
    node *set = behavior_asset_add_set_node(asset, snapshot_id);
    assert(graph_connect_exec(g, event, 0, set, 0, 0) == 0);
    assert(graph_connect_data(g, x, 0, set, 1) == 0);
    behavior_asset *original = copy_asset(asset);
    graph_optimize_report report = { 0 };
    assert(graph_optimize(g, NULL, &report) == 1);
    test_check(
        report.folded_inputs == 3 && report.removed_nodes == 1 && !acount(g->locals),
        "all exposed getter fields fold as one source group"
    );
    graph_optimize_report_free(&report);
    eval_view *preview = eval_view_new();
    eval_view_refresh(preview, g);
    test_check(!eval_view_error(preview)[0], "asset-only preview defers folded context reads without a graph error");
    eval_view_free(preview);
    ecs_world *world = ecs_world_new();
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameTransform, NULL) == ECS_OK);
    ecs_entity entity = ecs_world_create_entity(world);
    GameTransform initial = { 3, 7 };
    assert(
        ecs_world_record_add(world, entity, GAME_COMPONENT_GameTransform.id, &initial, 1) == ECS_OK
    );
    assert(ecs_world_apply_commands(world) == ECS_OK);
    pin_value owner = { .type = PIN_HANDLE,
                        .type_name = "ecs.entity",
                        .u64 = ecs_entity_to_u64(entity) };
    behavior_instance *before = behavior_instance_create(original, &owner, NULL, NULL);
    behavior_instance *after = behavior_instance_create(asset, &owner, NULL, NULL);
    eval_component_reader reader;
    ecs_component_reader_init(&reader, world);
    service_binding *services = NULL;
    apush(services, ((service_binding){ "ecs.world", world }));
    apush(services, ((service_binding){ EVAL_COMPONENT_READER_SERVICE, &reader }));
    assert(behavior_instance_bind_service(before, "ecs.world", world) == 0);
    assert(behavior_instance_bind_service(after, "ecs.world", world) == 0);
    assert(behavior_instance_bind_service(before, EVAL_COMPONENT_READER_SERVICE, &reader) == 0);
    assert(behavior_instance_bind_service(after, EVAL_COMPONENT_READER_SERVICE, &reader) == 0);
    for (int i = 0; i < 3; i++) {
        GAME_GameTransform_get(world, entity)->x = (float)(i * 11 + 3);
        GAME_GameTransform_get(world, entity)->y = (float)(i * 7 + 5);
        test_check(
            evaluate(original->graph, graph_node_by_id(original->graph, x->id), before, 0, services) ==
                    evaluate(g, x, after, 0, services) &&
                evaluate(original->graph, graph_node_by_id(original->graph, y->id), before, 0, services) ==
                    evaluate(g, y, after, 0, services),
            "component folds read changed values at each demand"
        );
        flow_error error = { 0 };
        flow_run *run_before = behavior_instance_dispatch(before, "Sample", (flow_inputs){ 0 }, &error);
        flow_run *run_after = behavior_instance_dispatch(after, "Sample", (flow_inputs){ 0 }, &error);
        pin_value a, b;
        test_check(
            run_before && run_after && flow_run_status(run_before) == FLOW_COMPLETED &&
                flow_run_status(run_after) == FLOW_COMPLETED &&
                !behavior_instance_get_variable(before, snapshot_id, &a) &&
                !behavior_instance_get_variable(after, snapshot_id, &b) &&
                a.f == b.f && b.f == GAME_GameTransform_get(world, entity)->x,
            "component changes between EXEC activations are observed "
            "identically after folding"
        );
    }
    behavior_instance_destroy(before);
    behavior_instance_destroy(after);
    afree(services);
    ecs_world_free(world);
    behavior_asset_free(original);
    behavior_asset_free(asset);
}

static void locals_and_documents()
{
    behavior_asset *asset = behavior_asset_new("Optimizer locals");
    graph *g = asset->graph;
    node *source = add(g, "Integer");
    graph_set_pin_default(g, source, 0, 0, &(pin_value){ .type = PIN_I32, .i = 17 });
    node *uses[3];
    for (int i = 0; i < 3; i++) {
        uses[i] = add(g, "Add");
        assert(graph_connect_data_conversion(g, source, 0, uses[i], 0, &NODE_To_Real) == 0);
    }
    char *before = behavior_asset_save(asset);
    graph_optimize_report dry = { 0 }, report = { 0 };
    graph_optimize_options options = graph_optimize_defaults();
    options.fanout_threshold = 4;
    test_check(
        graph_optimize(g, &options, &report) == 0 && !acount(g->locals),
        "a fanout below the configured threshold remains wired"
    );
    graph_optimize_report_free(&report);
    options.fanout_threshold = 3;
    options.dry_run = true;
    assert(graph_optimize(g, &options, &dry) == 1);
    char *untouched = behavior_asset_save(asset);
    test_check(
        !strcmp(before, untouched) && dry.created_aliases == 1,
        "dry run leaves IDs, serialized asset, and topology unchanged"
    );
    assert(graph_optimize(g, NULL, &report) == 1);
    test_check(
        report.created_aliases == 1 && report.changed_edges == 3 &&
            !report.removed_nodes && !acount(asset->variables),
        "fanout three names one existing value without persistent state"
    );
    test_check(
        !graph_validate_locals(g) && !graph_node_is_inline(g, source) &&
            acount(g->edges) == 3,
        "locals keep all dependency edges and the source visible"
    );
    for (int i = 0; i < 3; i++) {
        test_check(
            graph_input_conversion(g, uses[i], 0) == &NODE_To_Real &&
                evaluate(g, uses[i], NULL, 0, NULL) == 17,
            "each local use retains and evaluates its original conversion"
        );
    }
    behavior_asset *repeat = behavior_asset_load(before);
    graph_optimize_report repeated = { 0 };
    assert(graph_optimize(repeat->graph, NULL, &repeated) == 1);
    char *repeat_json = behavior_asset_save(repeat);
    char *optimized_json = behavior_asset_save(asset);
    test_check(!strcmp(repeat_json, optimized_json), "identical graphs produce identical local IDs, names, and serialized output");
    free(repeat_json);
    free(optimized_json);
    graph_optimize_report_free(&repeated);
    behavior_asset_free(repeat);
    behavior_asset *clone = copy_asset(asset);
    test_check(
        acount(clone->graph->locals) == 1 && !graph_validate_locals(clone->graph),
        "asset clone preserves local identities and dependencies"
    );
    char *clipboard = graph_save_nodes(g, g->nodes, acount(g->nodes));
    assert(graph_append(g, clipboard, 50, 50) == 0);
    test_check(
        acount(g->locals) == 2 && g->locals[0].id != g->locals[1].id &&
            strcmp(g->locals[0].name, g->locals[1].name) && !graph_validate_locals(g),
        "clipboard remaps source, pin, local IDs, and conflicting names"
    );
    free(clipboard);
    clipboard = graph_save_nodes(g, uses, 1);
    graph partial;
    graph_init(&partial);
    assert(graph_append(&partial, clipboard, 0, 0) == 0);
    test_check(
        !partial.nodes[0]->input_pins[0].binding && !acount(partial.locals),
        "partial clipboard does not retain a dangling external local"
    );
    graph_free(&partial);
    free(clipboard);
    uint64_t local_id = g->locals[0].id;
    assert(graph_expand_local(g, local_id) == 0);
    test_check(
        !uses[0]->input_pins[0].binding && evaluate(g, uses[0], NULL, 0, NULL) == 17,
        "expanding a local restores wires without changing values"
    );
    source = graph_node_by_id(clone->graph, clone->graph->locals[0].source_id);
    assert(graph_remove_node(clone->graph, source) == 0);
    test_check(!acount(clone->graph->locals) && !graph_validate_locals(clone->graph), "source deletion clears its local uses");
    behavior_asset_free(clone);
    graph_optimize_report_free(&dry);
    graph_optimize_report_free(&report);
    free(before);
    free(untouched);
    behavior_asset_free(asset);
}

static void skip_groups()
{
    for (int mode = 0; mode < 7; mode++) {
        behavior_asset *asset = behavior_asset_new("Optimizer skips");
        graph *g = asset->graph;
        node *source = add(g, "Get_GameMovement");
        node *a = sink(g, PIN_STRUCT, "GameMovement");
        node *b = sink(g, PIN_STRUCT, "GameMovement");
        assert(graph_connect_data(g, source, 0, a, 0) == 0);
        assert(graph_connect_data(g, source, 0, b, 0) == 0);
        if (mode == 0) {
            graph_set_pin_watch(g, source, 1, 0, 1);
        }
        if (mode == 1) {
            graph_set_breakpoint(g, b, 1);
        }
        if (mode == 2) {
            graph_set_node_disabled(g, b, 1);
        }
        if (mode == 3) {
            graph_set_pin_binding(
                g,
                b,
                0,
                0,
                &(pin_binding){ .kind = PIN_BINDING_COMPONENT, .component_key = "other" }
            );
        }
        if (mode == 4) {
            b->input_pins[0].type = PIN_FLOW;
        }
        if (mode == 5) {
            g->edges[1].custom_handles = true;
        }
        if (mode == 6) {
            g->edges[1].conversion = &NODE_To_Real;
        }
        char *before = behavior_asset_save(asset);
        graph_optimize_report report = { 0 };
        assert(graph_optimize(g, NULL, &report) == 0);
        char *after = behavior_asset_save(asset);
        test_check(
            !strcmp(before, after) && acount(report.skipped) > 0 &&
                acount(g->nodes) == 3,
            "one unsafe consumer leaves the entire getter fanout intact"
        );
        free(before);
        free(after);
        graph_optimize_report_free(&report);
        behavior_asset_free(asset);
    }
}

static void local_validation()
{
    behavior_asset *asset = behavior_asset_new("Invalid locals");
    graph *g = asset->graph;
    node *a = add(g, "Add"), *b = add(g, "Add");
    assert(graph_connect_data(g, a, 0, b, 0) == 0);
    assert(graph_create_local(g, a, 0, "Sum"));
    char *valid = behavior_asset_save(asset);
    g->locals[0].source_id = 999999;
    av_doc *invalid = av_doc_new();
    av_value *invalid_value = behavior_asset_to_value(invalid, asset);
    behavior_asset *loaded = behavior_asset_from_value(invalid_value);
    test_check(!loaded && strstr(graph_last_error(), "local"), "missing local sources fail loading with a local diagnostic");
    av_doc_free(invalid);
    behavior_asset_free(asset);
    asset = behavior_asset_load(valid);
    g = asset->graph;
    a = g->nodes[0];
    b = g->nodes[1];
    assert(graph_connect_data(g, b, 0, a, 0) == 0);
    compile_report report;
    compile_report_init(&report);
    test_check(behavior_asset_compile(asset, &report) > 0, "local dependencies participate in cycle validation");
    compile_report_free(&report);
    g->edges[0].conversion = &NODE_To_Real;
    test_check(graph_validate_locals(g) != NULL, "local validation rejects a conversion incompatible with its source");
    behavior_asset_free(asset);
    free(valid);
}

static void history_and_cleanup()
{
    behavior_asset *asset = behavior_asset_new("Optimizer history");
    editor e = { .asset = asset,
                 .active_graph = asset->graph,
                 .ui_context = EDITOR_UI_CONTEXT_BEHAVIOR };
    node *delta = add(asset->graph, "Delta Time");
    for (int i = 0; i < 3; i++) {
        node *consumer = add(asset->graph, "Add");
        assert(graph_connect_data(asset->graph, delta, 0, consumer, 0) == 0);
        consumer->x = (float)(3000 + i * 1000);
    }
    behavior_asset *before = ed_snapshot(&e);
    test_check(ed_optimize_graph(&e) == 1 && acount(e.undo) == 1, "optimization and arrangement create one undo entry");
    float left = e.active_graph->nodes[0]->x, right = left;
    for (int i = 1; i < acount(e.active_graph->nodes); i++) {
        left = fminf(left, e.active_graph->nodes[i]->x);
        right = fmaxf(right, e.active_graph->nodes[i]->x);
    }
    test_check(right - left < 2000, "optimizer arrangement compacts the original fanout spread");
    behavior_asset *after = ed_snapshot(&e);
    ed_undo(&e);
    behavior_asset *undone = ed_snapshot(&e);
    test_check(behavior_asset_equal(before, undone), "one Undo restores exact topology and layout");
    ed_redo(&e);
    behavior_asset *redone = ed_snapshot(&e);
    test_check(behavior_asset_equal(after, redone), "one Redo restores all optimizer changes");
    int history = acount(e.undo);
    test_check(ed_optimize_graph(&e) == 0 && acount(e.undo) == history, "a no-op optimizer preserves history");
    behavior_asset *noop = ed_snapshot(&e);
    test_check(behavior_asset_equal(noop, after), "a no-op optimizer preserves authored values");
    behavior_asset_free(noop);
    behavior_asset_free(before);
    behavior_asset_free(after);
    behavior_asset_free(undone);
    behavior_asset_free(redone);
    ed_free_asset_stack(&e.undo);
    ed_free_asset_stack(&e.redo);
    eval_view_free(e.ev);
    behavior_asset_free(e.asset);

    asset = behavior_asset_new("Optimizer constants");
    node *sine = graph_add_node(asset->graph, &NODE_Sine);
    graph_set_pin_default(asset->graph, sine, 0, 0, &(pin_value){ .type = PIN_F64, .d = 0.5 });
    node *consumer = sink(asset->graph, PIN_F64, "double");
    graph_connect_data(asset->graph, sine, 0, consumer, 0);
    double expected = evaluate(asset->graph, consumer, NULL, 0, NULL);
    graph_optimize_report report = { 0 };
    assert(graph_optimize(asset->graph, NULL, &report) == 1);
    test_check(evaluate(asset->graph, consumer, NULL, 0, NULL) == expected, "explicitly foldable scalar math preserves the result");
    graph_optimize_report_free(&report);
    behavior_asset_free(asset);
}

static void exact_sources()
{
    for (int kind = 0; kind < 3; kind++) {
        behavior_asset *asset = behavior_asset_new("Exact fold");
        behavior_asset_set_owner_type(asset, "ecs.entity");
        graph *g = asset->graph;
        node *source = NULL;
        if (kind == 0) {
            behavior_variable *v = behavior_asset_add_variable(
                asset,
                "Input",
                PIN_F32,
                "float",
                PIN_CONTAINER_SINGLE,
                &(pin_value){ .type = PIN_F32, .f = 4 },
                NULL
            );
            source = behavior_asset_add_get_node(asset, v->id);
        }
        if (kind == 1) {
            source = behavior_asset_add_self_node(asset);
        }
        if (kind == 2) {
            source = add(g, "Get_GameMovement");
        }
        assert(source);
        node *targets[3];
        for (int i = 0; i < 3; i++) {
            node *target = kind == 1 ? sink(g, PIN_HANDLE, "ecs.entity")
                                     : add(g, "Add");
            targets[i] = target;
            if (kind == 1) {
                target->input_pins[0].flags |= PIN_FLAG_DEFAULT_SELF;
            }
            const node_desc *conversion = kind == 2
                                            ? registry_node_by_name("Unwrap_GameMovement")
                                            : NULL;
            assert(graph_connect_data_conversion(g, source, 0, target, 0, conversion) == 0);
        }
        graph_optimize_report report = { 0 };
        assert(graph_optimize(g, NULL, &report) == 1);
        test_check(report.folded_inputs == 3 && report.removed_nodes == 1 && !acount(g->locals), "exact Variable, default Self, and component unwrap fanouts fold directly");
        if (kind == 1) {
            pin_value owner = { .type = PIN_HANDLE, .type_name = "ecs.entity", .u64 = 42 };
            behavior_instance *instance = behavior_instance_create(asset, &owner, NULL, NULL);
            for (int i = 0; i < 3; i++) {
                test_check(
                    targets[i]->input_pins[0].type == PIN_HANDLE &&
                        evaluate(g, targets[i], instance, 0, NULL) == 42,
                    "default Self folding preserves resolved wildcard type and "
                    "owner value"
                );
            }
            behavior_instance_destroy(instance);
        }
        graph_optimize_report_free(&report);
        behavior_asset_free(asset);
    }
}

static void structural_cleanup()
{
    behavior_asset *asset = behavior_asset_new("Projection cleanup");
    graph *g = asset->graph;
    node *make = add(g, "Make_GameHealth"), *split = add(g, "Break_GameHealth");
    assert(graph_set_pin_default(g, make, 0, 0, &(pin_value){ .type = PIN_I32, .i = 12 }) == 0);
    node *a = sink(g, PIN_I32, "int"), *b = sink(g, PIN_I32, "int");
    assert(graph_connect_data(g, make, 0, split, 0) == 0);
    assert(graph_connect_data(g, split, 0, a, 0) == 0);
    assert(graph_connect_data(g, split, 1, b, 0) == 0);
    graph_optimize_report report = { 0 };
    assert(graph_optimize(g, NULL, &report) == 1);
    test_check(
        report.removed_nodes == 1 && make->output_count == 3 &&
            evaluate(g, a, NULL, 0, NULL) == 12,
        "Break cleanup exposes fields on the original aggregate snapshot"
    );
    graph_optimize_report_free(&report);
    behavior_asset_free(asset);
}

static void event_projection_cleanup()
{
    behavior_asset *asset = behavior_asset_new("Event projection cleanup");
    graph *g = asset->graph;
    pin_value zero = { .type = PIN_F32, .type_name = "float" };
    uint64_t x_id = behavior_asset_add_variable(asset, "X", PIN_F32, "float", PIN_CONTAINER_SINGLE, &zero, NULL)
                        ->id;
    uint64_t y_id = behavior_asset_add_variable(asset, "Y", PIN_F32, "float", PIN_CONTAINER_SINGLE, &zero, NULL)
                        ->id;
    node *event = add(g, "Event");
    assert(graph_set_event_name(g, event, "Input") == 0);
    assert(
        graph_add_pin(g, event, 1, &(pin_spec){ .name = "Direction", .type = PIN_STRUCT, .type_name = "CF_V2" }) ==
        1
    );
    test_check(graph_output_struct(event, 1) == type_catalog_find("CF_V2"), "dynamic event structs expose aggregate fields");
    node *split = add(g, "Break_CF_V2");
    uint64_t split_id = split->id;
    node *store_x = behavior_asset_add_set_node(asset, x_id);
    node *store_y = behavior_asset_add_set_node(asset, y_id);
    assert(graph_connect_data(g, event, 1, split, 0) == 0);
    assert(graph_connect_data(g, split, 0, store_x, 1) == 0);
    assert(graph_connect_data(g, split, 1, store_y, 1) == 0);
    assert(graph_connect_exec(g, event, 0, store_x, 0, 0) == 0);
    assert(graph_connect_exec(g, store_x, 0, store_y, 0, 0) == 0);

    behavior_asset *original = copy_asset(asset);
    graph_optimize_report report = { 0 };
    assert(graph_optimize(g, NULL, &report) == 1);
    int x = -1, y = -1;
    for (int i = 0; i < event->output_count; i++) {
        if (event->output_pins[i].field_source != event->output_pins[1].id) {
            continue;
        }
        if (!strcmp(event->output_pins[i].field_name, "x")) {
            x = i;
        }
        if (!strcmp(event->output_pins[i].field_name, "y")) {
            y = i;
        }
    }
    node *source = NULL;
    int source_pin = -1;
    bool rewired = graph_node_by_id(g, split_id) == NULL &&
                   event->output_count == 4 && x >= 0 && y >= 0 &&
                   graph_input_source(g, store_x, 1, &source, &source_pin) &&
                   source == event && source_pin == x;
    bool y_rewired = graph_input_source(g, store_y, 1, &source, &source_pin) &&
                     source == event && source_pin == y;
    test_check(report.removed_nodes == 1 && rewired && y_rewired, "event struct payload fields replace Break projections");

    pin_value input = { .type = PIN_STRUCT,
                        .type_name = "CF_V2",
                        .ptr = &(CF_V2){ 3, -4 } };
    behavior_instance *before = behavior_instance_create(original, NULL, NULL, NULL);
    behavior_instance *after = behavior_instance_create(asset, NULL, NULL, NULL);
    flow_error error = { 0 };
    flow_run *before_run = behavior_instance_dispatch(
        before,
        "Input",
        (flow_inputs){ .values = &input, .count = 1 },
        &error
    );
    flow_run *after_run = behavior_instance_dispatch(
        after,
        "Input",
        (flow_inputs){ .values = &input, .count = 1 },
        &error
    );
    pin_value before_x, before_y, after_x, after_y;
    bool values = before_run && after_run &&
                  flow_run_status(before_run) == FLOW_COMPLETED &&
                  flow_run_status(after_run) == FLOW_COMPLETED &&
                  behavior_instance_get_variable(before, x_id, &before_x) == 0 &&
                  behavior_instance_get_variable(before, y_id, &before_y) == 0 &&
                  behavior_instance_get_variable(after, x_id, &after_x) == 0 &&
                  behavior_instance_get_variable(after, y_id, &after_y) == 0 &&
                  before_x.f == after_x.f && before_y.f == after_y.f &&
                  before_x.f == 3 && before_y.f == -4;
    test_check(values, "optimized event fields preserve payload values at runtime");
    behavior_instance_destroy(before);
    behavior_instance_destroy(after);
    behavior_asset_free(original);
    graph_optimize_report_free(&report);
    behavior_asset_free(asset);
}

static void nested_projection_cleanup()
{
    behavior_asset *asset = behavior_asset_new("Nested projection cleanup");
    graph *g = asset->graph;
    pin_value zero = { .type = PIN_F32, .type_name = "float" };
    uint64_t x_id = behavior_asset_add_variable(asset, "X", PIN_F32, "float", PIN_CONTAINER_SINGLE, &zero, NULL)
                        ->id;
    uint64_t y_id = behavior_asset_add_variable(asset, "Y", PIN_F32, "float", PIN_CONTAINER_SINGLE, &zero, NULL)
                        ->id;
    node *make = add(g, "Make_GameAim");
    CF_V2 direction = { 3, -4 };
    assert(
        graph_set_pin_default(
            g,
            make,
            0,
            0,
            &(pin_value){ .type = PIN_STRUCT, .type_name = "CF_V2", .ptr = &direction }
        ) == 0
    );
    assert(graph_expose_output_fields(g, make, 0) == 0);
    int direction_output = -1;
    for (int i = 0; i < make->output_count; i++) {
        if (make->output_pins[i].field_source == make->output_pins[0].id &&
            !strcmp(make->output_pins[i].field_name, "direction")) {
            direction_output = i;
            break;
        }
    }
    assert(direction_output >= 0);
    assert(graph_expose_output_fields(g, make, direction_output) == 0);
    int x = -1, y = -1;
    for (int i = 0; i < make->output_count; i++) {
        if (make->output_pins[i].field_source !=
            make->output_pins[direction_output].id) {
            continue;
        }
        if (!strcmp(make->output_pins[i].field_name, "x")) {
            x = i;
        }
        if (!strcmp(make->output_pins[i].field_name, "y")) {
            y = i;
        }
    }
    assert(x >= 0 && y >= 0);
    node *event = add(g, "Event");
    assert(graph_set_event_name(g, event, "Input") == 0);
    node *split = add(g, "Break_CF_V2");
    uint64_t split_id = split->id;
    node *store_x = behavior_asset_add_set_node(asset, x_id);
    node *store_y = behavior_asset_add_set_node(asset, y_id);
    assert(graph_connect_data(g, make, direction_output, split, 0) == 0);
    assert(graph_connect_data(g, split, 0, store_x, 1) == 0);
    assert(graph_connect_data(g, split, 1, store_y, 1) == 0);
    assert(graph_connect_exec(g, event, 0, store_x, 0, 0) == 0);
    assert(graph_connect_exec(g, store_x, 0, store_y, 0, 0) == 0);

    behavior_asset *original = copy_asset(asset);
    graph_optimize_report report = { 0 };
    assert(graph_optimize(g, NULL, &report) == 1);
    node *source = NULL;
    int source_pin = -1;
    bool rewired = graph_node_by_id(g, split_id) == NULL && make->output_count == 6 &&
                   graph_input_source(g, store_x, 1, &source, &source_pin) &&
                   source == make &&
                   !strcmp(make->output_pins[source_pin].field_name, "x") &&
                   graph_input_source(g, store_y, 1, &source, &source_pin) &&
                   source == make &&
                   !strcmp(make->output_pins[source_pin].field_name, "y");
    test_check(report.removed_nodes == 1 && rewired, "nested struct fields replace Break projections");
    assert(graph_hide_output_fields(g, make, 0) == 0);
    test_check(make->output_count == 4, "collapsing an aggregate keeps connected nested fields");

    behavior_instance *before = behavior_instance_create(original, NULL, NULL, NULL);
    behavior_instance *after = behavior_instance_create(asset, NULL, NULL, NULL);
    flow_error error = { 0 };
    flow_run *before_run = behavior_instance_dispatch(before, "Input", (flow_inputs){ 0 }, &error);
    flow_run *after_run = behavior_instance_dispatch(after, "Input", (flow_inputs){ 0 }, &error);
    pin_value before_x, before_y, after_x, after_y;
    bool values = before_run && after_run &&
                  flow_run_status(before_run) == FLOW_COMPLETED &&
                  flow_run_status(after_run) == FLOW_COMPLETED &&
                  behavior_instance_get_variable(before, x_id, &before_x) == 0 &&
                  behavior_instance_get_variable(before, y_id, &before_y) == 0 &&
                  behavior_instance_get_variable(after, x_id, &after_x) == 0 &&
                  behavior_instance_get_variable(after, y_id, &after_y) == 0 &&
                  before_x.f == after_x.f && before_y.f == after_y.f &&
                  before_x.f == 3 && before_y.f == -4;
    test_check(values, "optimized nested fields preserve payload values at runtime");
    behavior_instance_destroy(before);
    behavior_instance_destroy(after);
    behavior_asset_free(original);
    graph_optimize_report_free(&report);
    behavior_asset_free(asset);
}

static void component_nested_binding_cleanup()
{
    behavior_asset *asset = behavior_asset_new("Component nested projection cleanup");
    assert(behavior_asset_set_owner_type(asset, "ecs.entity") == 0);
    graph *g = asset->graph;
    pin_value zero = { .type = PIN_F32, .type_name = "float" };
    uint64_t x_id = behavior_asset_add_variable(asset, "X", PIN_F32, "float", PIN_CONTAINER_SINGLE, &zero, NULL)
                        ->id;
    uint64_t y_id = behavior_asset_add_variable(asset, "Y", PIN_F32, "float", PIN_CONTAINER_SINGLE, &zero, NULL)
                        ->id;
    node *get = add(g, "Get_GameAim");
    uint64_t get_id = get->id;
    assert(graph_expose_output_fields(g, get, 0) == 0);
    int direction = -1;
    for (int i = 0; i < get->output_count; i++) {
        if (get->output_pins[i].field_source == get->output_pins[0].id &&
            !strcmp(get->output_pins[i].field_name, "direction")) {
            direction = i;
            break;
        }
    }
    assert(direction >= 0 && graph_expose_output_fields(g, get, direction) == 0);
    node *event = add(g, "Event");
    assert(graph_set_event_name(g, event, "Input") == 0);
    node *split = add(g, "Break_CF_V2");
    uint64_t split_id = split->id;
    node *store_x = behavior_asset_add_set_node(asset, x_id);
    node *store_y = behavior_asset_add_set_node(asset, y_id);
    assert(graph_connect_data(g, get, direction, split, 0) == 0);
    assert(graph_connect_data(g, split, 0, store_x, 1) == 0);
    assert(graph_connect_data(g, split, 1, store_y, 1) == 0);
    assert(graph_connect_exec(g, event, 0, store_x, 0, 0) == 0);
    assert(graph_connect_exec(g, store_x, 0, store_y, 0, 0) == 0);

    graph_optimize_report report = { 0 };
    assert(graph_optimize(g, NULL, &report) == 2);
    const pin_binding *x_binding = graph_pin_binding(g, store_x, 0, 1);
    const pin_binding *y_binding = graph_pin_binding(g, store_y, 0, 1);
    bool direct = graph_node_by_id(g, split_id) == NULL &&
                  graph_node_by_id(g, get_id) == NULL && x_binding &&
                  x_binding->kind == PIN_BINDING_COMPONENT_FIELD &&
                  x_binding->component_key &&
                  !strcmp(x_binding->component_key, GAME_COMPONENT_GameAim.key) &&
                  x_binding->field_name &&
                  !strcmp(x_binding->field_name, "direction.x") && y_binding &&
                  y_binding->kind == PIN_BINDING_COMPONENT_FIELD &&
                  y_binding->component_key &&
                  !strcmp(y_binding->component_key, GAME_COMPONENT_GameAim.key) &&
                  y_binding->field_name &&
                  !strcmp(y_binding->field_name, "direction.y");
    test_check(
        report.removed_nodes == 2 && direct &&
            !graph_input_source(g, store_x, 1, NULL, NULL) &&
            !graph_input_source(g, store_y, 1, NULL, NULL),
        "component field Break projections become nested component bindings"
    );
    ecs_world *world = ecs_world_new();
    assert(world);
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameAim, NULL) == ECS_OK);
    ecs_entity owner = ecs_world_create_entity(world);
    GameAim aim = { .direction = { 3, -4 } };
    assert(ecs_world_record_add(world, owner, GAME_COMPONENT_GameAim.id, &aim, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    pin_value owner_value = { .type = PIN_HANDLE,
                              .type_name = "ecs.entity",
                              .u64 = ecs_entity_to_u64(owner) };
    behavior_instance *instance = behavior_instance_create(asset, &owner_value, NULL, NULL);
    assert(instance);
    eval_component_reader reader;
    ecs_component_reader_init(&reader, world);
    assert(behavior_instance_bind_service(instance, "ecs.world", world) == 0);
    assert(behavior_instance_bind_service(instance, EVAL_COMPONENT_READER_SERVICE, &reader) == 0);
    flow_error error = { 0 };
    flow_run *run = behavior_instance_dispatch(instance, "Input", (flow_inputs){ 0 }, &error);
    pin_value x_value = { 0 }, y_value = { 0 };
    test_check(
        run && flow_run_status(run) == FLOW_COMPLETED &&
            behavior_instance_get_variable(instance, x_id, &x_value) == 0 &&
            behavior_instance_get_variable(instance, y_id, &y_value) == 0 &&
            x_value.f == 3 && y_value.f == -4,
        "optimized component Break cleanup evaluates nested bindings"
    );
    behavior_instance_destroy(instance);
    ecs_world_free(world);
    graph_optimize_report_free(&report);
    behavior_asset_free(asset);
}

void test_optimizer_runtime();
void test_optimizer_sources();

void test_graph_optimizer()
{
    test_optimizer_sources();
    component_fanout();
    locals_and_documents();
    skip_groups();
    local_validation();
    history_and_cleanup();
    exact_sources();
    structural_cleanup();
    event_projection_cleanup();
    nested_projection_cleanup();
    component_nested_binding_cleanup();
    test_optimizer_runtime();
}
