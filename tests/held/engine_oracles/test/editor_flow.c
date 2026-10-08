#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "editor/graph/eval_view.h"
#include "editor/workspace/editor_internal.h"
#include "graph/model/behavior_asset.h"
#include "graph/model/registry.h"
#include "graph/runtime/instance.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int finish(node *, struct flow_context *)
{
    return FLOW_OK;
}

static pin_spec exec[] = { { "Exec", PIN_FLOW, NULL, PIN_EXEC } };
static node_desc sink = {
    .name = "Activity sink",
    .inputs = exec,
    .input_count = 1,
    .flow_kind = NODE_FLOW_CUSTOM,
    .flow = finish,
};

static editor_edge_activity activity(editor *editor, const edge *edge)
{
    editor_edge_activity result;
    editor_query_edge_activity(editor, edge, &result);
    return result;
}

static editor_node_activity ran(editor *editor, const node *n)
{
    editor_node_activity result;
    editor_query_node_activity(editor, n->id, &result);
    return result;
}

static void fire(behavior_instance *instance)
{
    flow_error error;
    flow_run *run = behavior_instance_dispatch(instance, "Input", (flow_inputs){ 0 }, &error);
    assert(run && flow_run_status(run) == FLOW_COMPLETED);
    assert(behavior_instance_release_activation(instance, run) == 0);
}

/* The log keeps the newest ED_LOG_MAX lines; each keeps its serial and the
   label it printed with, and clearing keeps counting. */
static void log_ring()
{
    editor *e = editor_new(behavior_asset_new("Log"));
    char text[32];
    for (int i = 0; i < ED_LOG_MAX + 6; i++) {
        snprintf(text, sizeof text, "line %d", i);
        editor_log_print(e, i % 2 ? "Odd" : "Even", text);
    }
    editor_log_line first = editor_log_at(e, 0);
    editor_log_line last = editor_log_at(e, ED_LOG_MAX - 1);
    snprintf(text, sizeof text, "line %d", ED_LOG_MAX + 5);
    assert(
        editor_log_count(e) == ED_LOG_MAX && first.serial == 6 &&
        !strcmp(first.message, "line 6") && !strcmp(first.origin, "Even") &&
        last.serial == ED_LOG_MAX + 5 && !strcmp(last.message, text) &&
        !strcmp(last.origin, "Odd")
    );
    editor_clear_log(e);
    editor_log_print(e, "Again", "after");
    assert(editor_log_count(e) == 1 && editor_log_at(e, 0).serial == ED_LOG_MAX + 6);
    editor_free(e);
}

/* Pulses follow the inspected instance's wires on the editor's clock: whole
   while a wire keeps firing, easing to nothing within half a second once it
   stops, while execution wires keep a longer glow. Nodes count their runs,
   and the instance keeps its last completed run's values. */
void test_editor_flow()
{
    behavior_asset *asset = behavior_asset_new("Flow activity");
    graph *g = asset->graph;
    node *event = graph_add_node(g, &NODE_Event);
    node *branch = graph_add_node(g, &NODE_Branch);
    node *condition = graph_add_node(g, registry_node_by_name("Bool"));
    node *yes = graph_add_node(g, &sink);
    node *no = graph_add_node(g, &sink);
    assert(graph_set_event_name(g, event, "Input") == 0);
    assert(
        graph_set_pin_default(g, condition, 0, 0, &(pin_value){ .type = PIN_BOOL, .b = true }) == 0
    );
    assert(graph_connect_exec(g, event, 0, branch, 0, 0) == 0);
    assert(graph_connect_data(g, condition, 0, branch, 1) == 0);
    assert(graph_connect_exec(g, branch, 0, yes, 0, 0) == 0);
    assert(graph_connect_exec(g, branch, 1, no, 0, 0) == 0);
    const edge *data = &g->edges[1];
    const edge *taken = &g->edges[2];
    const edge *untaken = &g->edges[3];
    editor *editor = editor_new(asset);
    behavior_instance *instance = behavior_instance_create(asset, NULL, NULL, NULL);
    assert(behavior_instance_set_activation_pool(instance, 1) == 0);
    behavior_instance_set_debug_target(instance, true);
    editor_set_inspection_instance(editor, instance);
    double now = 1;
    editor_tick(editor, now);
    assert(!activity(editor, taken).count);

    fire(instance);
    editor_tick(editor, now += 1.0 / 60);
    editor_edge_activity result = activity(editor, taken);
    assert(result.count == 1 && result.pulse == 1 && result.heat > 0);
    result = activity(editor, untaken);
    assert(!result.count && result.pulse == 0 && result.heat == 0);
    /* A data wire carried its value when its consumer pulled it; only
       execution wires glow. */
    result = activity(editor, data);
    assert(result.count >= 1 && result.pulse == 1 && result.heat == 0);
    assert(ran(editor, yes).runs == 1 && ran(editor, yes).pulse == 1);
    assert(ran(editor, event).runs == 1 && ran(editor, branch).runs == 1);
    assert(!ran(editor, no).runs && isinf(ran(editor, no).since));

    /* The completed run stays readable, so pins show what they last carried. */
    int retained = 0;
    flow_run *const *runs = behavior_instance_retained_runs(instance, &retained);
    pin_value value;
    assert(
        retained == 1 &&
        flow_run_watch_input(runs[0], branch, 1, &value) == 0 && value.b
    );
    ed_refresh_values(editor);
    assert(eval_view_live(editor->ev));
    /* A pure output reads as what its consumer received. */
    assert(
        eval_view_live_value(editor->ev, g, condition, 1, 0, &value) == 0 &&
        value.b
    );

    /* Firing every frame keeps the bubbles whole. */
    for (int frame = 0; frame < 30; frame++) {
        fire(instance);
        editor_tick(editor, now += 1.0 / 60);
        assert(activity(editor, taken).pulse == 1);
    }
    assert(activity(editor, taken).count == 31 && ran(editor, yes).runs == 31);
    behavior_instance_retained_runs(instance, &retained);
    assert(retained == 1);

    /* Stopped, they shrink to nothing within half a second; the glow stays. */
    editor_tick(editor, now + 0.3);
    result = activity(editor, taken);
    assert(result.pulse > 0 && result.pulse < 1);
    assert(ran(editor, yes).pulse < 1 && fabs(ran(editor, yes).since - 0.3) < 1e-9);
    editor_tick(editor, now + 0.6);
    result = activity(editor, taken);
    assert(result.pulse == 0 && result.heat > 0 && activity(editor, data).pulse == 0);
    editor_tick(editor, now + 4);
    assert(activity(editor, taken).heat == 0 && activity(editor, taken).count == 31);
    now += 4;

    /* A target chosen after it ran shows what ran before only as counts. */
    editor_set_inspection_instance(editor, NULL);
    editor_tick(editor, now += 0.1);
    assert(!activity(editor, taken).count);
    editor_set_inspection_instance(editor, instance);
    editor_tick(editor, now += 0.1);
    result = activity(editor, taken);
    assert(result.count == 31 && result.pulse == 0);
    fire(instance);
    editor_tick(editor, now += 0.1);
    assert(activity(editor, taken).pulse == 1);

    editor_set_inspection_blocked(editor, true);
    assert(!activity(editor, taken).count && !ran(editor, yes).runs);
    editor_set_inspection_blocked(editor, false);
    editor_set_inspection_instance(editor, instance);

    /* An instance nobody inspects records nothing and keeps no runs. */
    behavior_instance_set_trace_enabled(instance, 0);
    fire(instance);
    int capacity = 0;
    uint64_t generation = 0;
    const behavior_instance_activity
        *table = behavior_instance_activity_table(instance, &capacity, &generation);
    for (int i = 0; i < capacity; i++) {
        assert(!table[i].count);
    }
    behavior_instance_retained_runs(instance, &retained);
    assert(!retained);
    editor_tick(editor, now += 0.1);
    assert(!activity(editor, taken).count);

    editor_set_inspection_instance(editor, NULL);
    behavior_instance_destroy(instance);
    editor_free(editor);
    log_ring();
    puts("editor flow PASS: pulses hold while wires fire and ease out after; node runs; retained values; untraced instances record nothing; the log keeps its newest lines");
}
