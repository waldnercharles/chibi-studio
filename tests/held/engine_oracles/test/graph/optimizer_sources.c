#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "graph/authoring/local_edit.h"
#include "editor/workspace/editor_internal.h"
#include "graph/authoring/optimize.h"
#include "graph/model/local.h"
#include "support/text_file.h"
#include "test/suite_internal.h"
#include "test/support/code_fixture.h"

#include <assert.h>

static uint64_t variable(behavior_asset *asset, const char *name)
{
    behavior_variable *v = behavior_asset_add_variable(
        asset,
        name,
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32 },
        NULL
    );
    return v->id;
}

static void connect_exec(graph *g, node *from, int output, node *to)
{
    assert(!graph_connect_exec(g, from, output, to, 0, 0));
}

static void set_value(graph *g, node *n, float value)
{
    assert(!graph_set_pin_default(g, n, 0, 1, &(pin_value){ .type = PIN_F32, .f = value }));
}

static void setter_outputs()
{
    enum
    {
        STRAIGHT,
        WRITE,
        DELAY,
        TRUE_BRANCH,
        CALL,
        SEQUENCE,
        LOCAL,
        BYPASS,
        LOOP,
        FALSE_BRANCH,
        CASE_COUNT
    };
    const node_desc *between_desc[CASE_COUNT] = {
        [WRITE] = &NODE_SetVariable,  [DELAY] = &NODE_Delay,
        [TRUE_BRANCH] = &NODE_Branch, [FALSE_BRANCH] = &NODE_Branch,
        [SEQUENCE] = &NODE_Sequence,  [LOOP] = &NODE_ForLoop
    };
    for (int mode = 0; mode < CASE_COUNT; mode++) {
        behavior_asset *asset = behavior_asset_new("Set output folding");
        graph *g = asset->graph;
        uint64_t id = variable(asset, "Value");
        uint64_t first = variable(asset, "First"), second = variable(asset, "Second");
        node *source = behavior_asset_add_set_node(asset, id);
        set_value(g, source, 7);
        node *event = graph_add_node(g, &NODE_Event);
        graph_set_event_name(g, event, "Go");
        connect_exec(g, event, 0, source);
        node *uses[2] = { behavior_asset_add_set_node(asset, first),
                          behavior_asset_add_set_node(asset, second) };
        connect_exec(g, source, 0, uses[0]);
        connect_exec(g, uses[0], 0, uses[1]);
        for (int i = 0; i < 2; i++) {
            assert(!graph_connect_data(g, source, 1, uses[i], 1));
        }
        if (between_desc[mode]) {
            node *between = mode == WRITE ? behavior_asset_add_set_node(asset, id)
                                          : graph_add_node(g, between_desc[mode]);
            assert(!graph_remove_edge(g, source, 0, uses[0], 0));
            connect_exec(g, source, 0, between);
            connect_exec(g, between, 0, uses[0]);
            if (mode == WRITE) {
                set_value(g, between, 9);
            }
            if (mode == DELAY) {
                graph_set_pin_default(g, between, 0, 1, &(pin_value){ .type = PIN_F64, .d = .1 });
            }
            if (mode == TRUE_BRANCH || mode == FALSE_BRANCH) {
                graph_set_pin_default(
                    g,
                    between,
                    0,
                    1,
                    &(pin_value){ .type = PIN_BOOL, .b = mode == TRUE_BRANCH }
                );
                connect_exec(g, between, 1, uses[1]);
            }
        }
        if (mode == CALL) {
            graph *function = behavior_asset_add_function(asset, "Unproven call", false);
            connect_exec(
                function,
                graph_node_by_id(function, function->entry_id),
                0,
                graph_node_by_id(function, function->return_id)
            );
            node *call = behavior_asset_add_function_call_node(asset, function);
            assert(!graph_remove_edge(g, source, 0, uses[0], 0));
            connect_exec(g, source, 0, call);
            connect_exec(g, call, 0, uses[0]);
        }
        if (mode == LOCAL) {
            assert(graph_create_local(g, source, 1, "Value"));
        }
        if (mode == BYPASS) {
            node *other = graph_add_node(g, &NODE_Event);
            graph_set_event_name(g, other, "Other");
            connect_exec(g, other, 0, uses[1]);
        }
        char *json = behavior_asset_save(asset);
        behavior_asset *original = behavior_asset_load(json);
        free(json);
        assert(original);
        graph_optimize_report report = { 0 };
        graph_optimize_options options = graph_optimize_defaults();
        options.local_aliases = false;
        int changed = graph_optimize(g, &options, &report);
        bool expected = mode == STRAIGHT || mode == TRUE_BRANCH ||
                        mode == LOCAL || mode == FALSE_BRANCH;
        bool folded = true;
        for (int i = 0; i < 2; i++) {
            const pin_binding *binding = graph_pin_binding(g, uses[i], 0, 1);
            node *driver = NULL;
            int pin = -1;
            folded &= binding && binding->kind == PIN_BINDING_VARIABLE &&
                      binding->variable_id == id &&
                      !graph_input_source(g, uses[i], 1, &driver, &pin);
        }
        test_check(
            folded == expected && (changed > 0) == expected &&
                graph_node_by_id(g, source->id) == source,
            expected ? "Set outputs become variable inputs across a proven "
                       "execution path; the assignment remains"
                     : "later writes, suspension, siblings, and bypass paths "
                       "retain Set output snapshots"
        );
        if (mode == LOCAL) {
            test_check(!acount(g->locals), "Optimize upgrades an existing Set output local to variable inputs");
        }
        behavior_instance *instances[] = {
            behavior_instance_create(original, NULL, NULL, NULL),
            behavior_instance_create(asset, NULL, NULL, NULL)
        };
        for (int i = 0; i < 2; i++) {
            flow_error error = { 0 };
            flow_run *run = behavior_instance_dispatch(instances[i], "Go", (flow_inputs){ 0 }, &error);
            assert(run);
            if (mode == DELAY) {
                test_check(flow_run_status(run) == FLOW_SUSPENDED, "snapshot regression reaches a suspended flow");
                assert(!behavior_instance_set_variable(
                    instances[i],
                    id,
                    &(pin_value){ .type = PIN_F32, .f = 99 }
                ));
                assert(flow_run_tick_at(run, 1) >= 0);
            }
            test_check(flow_run_status(run) == FLOW_COMPLETED, "original and optimized Set-output flows complete");
            pin_value a, b;
            assert(!behavior_instance_get_variable(instances[i], first, &a));
            assert(!behavior_instance_get_variable(instances[i], second, &b));
            test_check(a.f == (mode == FALSE_BRANCH ? 0 : 7) && b.f == 7, "consumers retain the assigned value even after later writes or external updates during a delay");
            behavior_instance_destroy(instances[i]);
        }
        char *before = behavior_asset_save(asset);
        graph_optimize_report_free(&report);
        test_check(graph_optimize(g, &options, &report) == 0, "repeating Set-output optimization is a no-op");
        char *after = behavior_asset_save(asset);
        test_check(!strcmp(before, after), "repeated Set-output optimization preserves IDs and serialized bytes");
        free(before);
        free(after);
        graph_optimize_report_free(&report);
        behavior_asset_free(original);
        behavior_asset_free(asset);
    }
}

static void explicit_self()
{
    for (int mode = 0; mode < 4; mode++) {
        behavior_asset *asset = behavior_asset_new("Explicit Self folding");
        behavior_asset_set_owner_type(asset, "ecs.entity");
        graph *g = asset->graph;
        node *source = behavior_asset_add_self_node(asset);
        node *uses[3];
        for (int i = 0; i < 3; i++) {
            uses[i] = graph_add_node(g, registry_node_by_name("Reroute Data"));
            assert(!graph_set_pin_type(g, uses[i], 0, 0, PIN_HANDLE, "ecs.entity"));
            assert(
                !graph_set_pin_default(g, uses[i], 0, 0, &(pin_value){ .type = PIN_HANDLE, .u64 = 99 })
            );
            assert(!graph_connect_data(g, source, 0, uses[i], 0));
        }
        if (mode == 1) {
            graph_set_pin_watch(g, source, 1, 0, true);
        }
        if (mode == 2) {
            graph_set_node_disabled(g, uses[1], true);
        }
        if (mode == 3) {
            assert(graph_create_local(g, source, 0, "Self"));
        }
        graph_optimize_report report = { 0 };
        int changed = graph_optimize(g, NULL, &report);
        bool all_inline = true;
        for (int i = 0; i < 3; i++) {
            node *self = NULL;
            int pin = -1;
            all_inline &= graph_inline_source(g, uses[i], 0, &self, &pin) &&
                          self->desc == &NODE_Self;
        }
        test_check(all_inline == (mode == 0 || mode == 3) && (changed > 0) == all_inline, "Self fanout becomes inline sources while preserving watched and disabled groups");
        if (all_inline) {
            for (int i = 0; i < 3; i++) {
                test_check(uses[i]->input_pins[0].default_value.u64 == 99, "inlining Self preserves dormant entity input values");
            }
            char *json = behavior_asset_save(asset);
            behavior_asset *copy = behavior_asset_load(json);
            free(json);
            assert(copy);
            arena storage;
            arena_init(&storage, 4096);
            pin_value owner = { .type = PIN_HANDLE, .type_name = "ecs.entity", .u64 = 42 };
            behavior_instance *instance = behavior_instance_create(copy, &owner, NULL, NULL);
            eval_context context;
            eval_context_init(&context);
            context.instance = instance;
            for (int i = 0; i < 3; i++) {
                node *use = graph_node_by_id(copy->graph, uses[i]->id);
                pin_value value;
                assert(!graph_eval_output(copy->graph, use, 0, &storage, &value, &context));
                test_check(value.u64 == 42, "serialized inline Self evaluates the instance owner");
            }
            eval_context_free(&context);
            behavior_instance_destroy(instance);
            arena_free(&storage);
            behavior_asset_free(copy);
            char *before = behavior_asset_save(asset);
            graph_optimize_report_free(&report);
            test_check(graph_optimize(g, NULL, &report) == 0, "already-inline Self is a no-op");
            char *after = behavior_asset_save(asset);
            test_check(!strcmp(before, after), "Self optimization is idempotent without allocating more nodes");
            free(before);
            free(after);
        }
        graph_optimize_report_free(&report);
        behavior_asset_free(asset);
    }
}

static void arena_shard()
{
    char *json = text_file_read("test/fixtures/content/behaviors/ArenaShardBehavior.chibi");
    behavior_asset *asset = test_project_asset("test/fixtures/content/behaviors/ArenaShardBehavior.chibi", json);
    free(json);
    assert(asset);
    editor e = { .asset = asset,
                 .active_graph = asset->graph,
                 .ui_context = EDITOR_UI_CONTEXT_BEHAVIOR };
    behavior_variable *age = behavior_asset_find_variable_by_name(asset, "Age");
    assert(age);
    node *set = NULL;
    for (int i = 0; i < acount(asset->graph->nodes); i++) {
        node *n = asset->graph->nodes[i];
        if (n->desc == &NODE_SetVariable && n->reference_id == age->id) {
            set = n;
        }
    }
    assert(set);
    behavior_asset *before = ed_snapshot(&e);
    test_check(ed_optimize_graph(&e) > 0 && acount(e.undo) == 1, "ArenaShard optimization creates one history transaction");
    int age_uses = 0;
    for (int i = 0; i < acount(asset->graph->nodes); i++) {
        node *n = asset->graph->nodes[i];
        for (int p = 0; p < n->input_count; p++) {
            const pin_binding *binding = n->input_pins[p].binding;
            age_uses += binding && binding->kind == PIN_BINDING_VARIABLE &&
                        binding->variable_id == age->id;
        }
        if (n->desc == &NODE_Self) {
            test_check(graph_node_is_inline(asset->graph, n), "ArenaShard Self is presented inline on its entity input");
        }
    }
    bool wired = false;
    for (int i = 0; i < acount(asset->graph->edges); i++) {
        wired |= asset->graph->edges[i].from_id == set->id &&
                 asset->graph->edges[i].kind == GRAPH_EDGE_DATA;
    }
    test_check(!wired && age_uses >= 3, "ArenaShard's two Set Age consumers and getter use inline variable inputs");
    ed_undo(&e);
    behavior_asset *undone = ed_snapshot(&e);
    test_check(behavior_asset_equal(before, undone), "Undo restores ArenaShard's Set output and Self topology and layout exactly");
    behavior_asset_free(before);
    behavior_asset_free(undone);
    ed_free_asset_stack(&e.undo);
    ed_free_asset_stack(&e.redo);
    eval_view_free(e.ev);
    behavior_asset_free(e.asset);
}

void test_optimizer_sources()
{
    setter_outputs();
    explicit_self();
    arena_shard();
}
