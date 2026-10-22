#include "editor/workspace/editor_internal.h"
#include "execution/ecs_reader.h"
#include "graph/authoring/asset_edit.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/local_edit.h"
#include "graph/document/extract.h"
#include "graph/model/local.h"
#include "graph/nodes/core_nodes.h"
#include "graph/runtime/flow_internal.h"
#include "support/asset_document.h"
#include "support/cos_sin.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <math.h>

static node *typed_reroute(graph *g, pin_type type, const char *type_name)
{
    node *n = graph_add_node(g, registry_node_by_name("Reroute Data"));
    assert(n);
    assert(graph_set_pin_type(g, n, 0, 0, type, type_name) == 0);
    return n;
}

static int eval_pin_value(
    behavior_asset *asset,
    node *n,
    behavior_instance *instance,
    const service_binding *services,
    double delta,
    arena *storage,
    pin_value *value,
    char *message,
    size_t message_size
)
{
    eval_context context;
    eval_context_init(&context);
    context.instance = instance;
    context.services = services;
    context.delta_seconds = delta;
    int status = graph_eval_output(asset->graph, n, 0, storage, value, &context);
    if (message && message_size) {
        const eval_status *error = eval_context_status(&context);
        snprintf(message, message_size, "%s", error && error->message ? error->message : "");
    }
    eval_context_free(&context);
    return status;
}

static int eval_f32(
    behavior_asset *asset,
    node *n,
    behavior_instance *instance,
    const service_binding *services,
    double delta,
    float *value,
    char *message,
    size_t message_size
)
{
    arena storage;
    arena_init(&storage, 4096);
    eval_context context;
    eval_context_init(&context);
    context.instance = instance;
    context.services = services;
    context.delta_seconds = delta;
    pin_value result = { 0 };
    int status = graph_eval_output(asset->graph, n, 0, &storage, &result, &context);
    if (status == 0 && value) {
        if (result.type == PIN_F32) {
            *value = result.f;
        } else if (result.type == PIN_F64) {
            *value = (float)result.d;
        } else {
            status = -1;
        }
    }
    if (message && message_size) {
        const eval_status *error = eval_context_status(&context);
        snprintf(message, message_size, "%s", error && error->message ? error->message : "");
    }
    eval_context_free(&context);
    arena_free(&storage);
    return status;
}

static behavior_instance *instance_for(behavior_asset *asset, ecs_world *world, ecs_entity owner)
{
    pin_value handle = { .type = PIN_HANDLE,
                         .type_name = "ecs.entity",
                         .u64 = ecs_entity_to_u64(owner) };
    behavior_instance *instance = behavior_instance_create(asset, &handle, NULL, NULL);
    assert(instance);
    if (world) {
        assert(behavior_instance_bind_service(instance, "ecs.world", world) == 0);
    }
    return instance;
}

static flow_run *dispatch(behavior_instance *instance, const char *event)
{
    flow_error error = { 0 };
    flow_run *run = behavior_instance_dispatch(instance, event, (flow_inputs){ 0 }, &error);
    assert(run);
    return run;
}

static node *find_function_node(graph *g, const node_desc *desc, uint64_t reference_id);

static void variable_binding_and_precedence()
{
    behavior_asset *asset = behavior_asset_new("BindingBehavior");
    pin_value initial = { .type = PIN_F32, .type_name = "float", .f = 2 };
    behavior_variable *variable = behavior_asset_add_variable(
        asset,
        "Value",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &initial,
        "Inputs"
    );
    assert(variable);
    node *target = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_default(
            asset->graph,
            target,
            0,
            0,
            &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 3 }
        ) == 0
    );
    assert(
        graph_set_pin_binding(
            asset->graph,
            target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE,
                            .variable_id = variable->id }
        ) == 0
    );
    const pin_binding *binding = graph_pin_binding(asset->graph, target, 0, 0);
    test_check(
        binding && binding->kind == PIN_BINDING_VARIABLE &&
            binding->variable_id == variable->id && binding->source_asset &&
            strcmp(binding->source_asset, "BindingBehavior") == 0,
        "variable input bindings retain stable source identity"
    );

    behavior_instance *instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    float result = 0;
    char message[192];
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 2) < 0.0001f,
        "direct evaluation reads the current bound variable"
    );
    assert(
        behavior_instance_set_variable(
            instance,
            variable->id,
            &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 7 }
        ) == 0
    );
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 7) < 0.0001f,
        "direct evaluation observes a variable write without stale literal data"
    );
    behavior_instance_destroy(instance);

    assert(behavior_asset_rename_variable(asset, variable->id, "RenamedValue") == 0);
    assert(behavior_asset_set_name(asset, "RenamedBinding") == 0);
    binding = graph_pin_binding(asset->graph, target, 0, 0);
    test_check(
        binding && binding->source_asset &&
            strcmp(binding->source_asset, "RenamedBinding") == 0,
        "renaming a behavior updates local binding ownership without changing "
        "the variable ID"
    );
    instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 2) < 0.0001f,
        "renamed variables continue to resolve by stable ID"
    );
    behavior_instance_destroy(instance);

    assert(behavior_asset_retype_variable(asset, variable->id, PIN_I32, "int") == 0);
    test_check(target->input_pins[0].type == PIN_F32, "retyping a variable does not silently retype a fixed consumer pin");
    instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    assert(
        behavior_instance_set_variable(
            instance,
            variable->id,
            &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 13 }
        ) == 0
    );
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 13) < .0001f,
        "a retyped integer binding uses the registered real conversion"
    );
    behavior_instance_destroy(instance);
    assert(behavior_asset_retype_variable(asset, variable->id, PIN_BOOL, "bool") == 0);
    instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    assert(
        behavior_instance_set_variable(
            instance,
            variable->id,
            &(pin_value){ .type = PIN_BOOL, .type_name = "bool", .b = true }
        ) == 0
    );
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 1) < .0001f,
        "a bool binding reads as 1 or 0 through the registered real conversion"
    );
    behavior_instance_destroy(instance);
    assert(behavior_asset_retype_variable(asset, variable->id, PIN_STR, "const char *") == 0);
    instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) != 0 &&
            strstr(message, "incompatible"),
        "a type-mismatched active binding reports an error"
    );
    behavior_instance_destroy(instance);

    assert(behavior_asset_delete_variable(asset, variable->id) == 0);
    test_check(
        graph_pin_binding(asset->graph, target, 0, 0) != NULL,
        "deleting a variable preserves an explicit unresolved binding"
    );
    instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) != 0 &&
            strstr(message, "lost variable"),
        "a deleted variable binding fails visibly instead of falling back"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(asset);

    behavior_asset *wire_asset = behavior_asset_new("WirePrecedence");
    behavior_variable *wire_variable = behavior_asset_add_variable(
        wire_asset,
        "Fallback",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 1 },
        NULL
    );
    node *source = typed_reroute(wire_asset->graph, PIN_F32, "float");
    target = typed_reroute(wire_asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_default(
            wire_asset->graph,
            source,
            0,
            0,
            &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 9 }
        ) == 0
    );
    assert(
        graph_set_pin_binding(
            wire_asset->graph,
            target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE,
                            .variable_id = wire_variable->id }
        ) == 0
    );
    assert(graph_connect_data(wire_asset->graph, source, 0, target, 0) == 0);
    instance = instance_for(wire_asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(wire_asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 9) < 0.0001f,
        "a connected wire takes precedence over a dormant input binding"
    );
    behavior_instance_destroy(instance);
    assert(graph_remove_edge(wire_asset->graph, source, 0, target, 0) == 0);
    instance = instance_for(wire_asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(wire_asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 1) < 0.0001f,
        "disconnecting a wire restores its dormant binding"
    );
    behavior_instance_destroy(instance);
    assert(graph_set_pin_binding(wire_asset->graph, target, 0, 0, NULL) == 0);
    instance = instance_for(wire_asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(wire_asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 0) < 0.0001f,
        "clearing a binding returns an input to its authored literal"
    );
    behavior_instance_destroy(instance);

    assert(
        graph_set_pin_binding(
            wire_asset->graph,
            target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = UINT64_MAX }
        ) == 0
    );
    assert(graph_connect_data(wire_asset->graph, source, 0, target, 0) == 0);
    compile_report report;
    compile_report_init(&report);
    test_check(
        behavior_asset_compile(wire_asset, &report) == 0 && report.error_count == 0,
        "a valid wire ignores an unresolved dormant binding during compilation"
    );
    compile_report_free(&report);
    instance = instance_for(wire_asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(wire_asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 9) < 0.0001f,
        "a valid wire evaluates despite an unresolved dormant binding"
    );
    behavior_instance_destroy(instance);
    assert(graph_remove_edge(wire_asset->graph, source, 0, target, 0) == 0);
    instance = instance_for(wire_asset, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(wire_asset, target, instance, NULL, 0, &result, message, sizeof message) != 0 &&
            strstr(message, "lost variable"),
        "disconnecting a wire exposes an unresolved dormant binding"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(wire_asset);
}

static void flow_timing_and_delta()
{
    behavior_asset *asset = behavior_asset_new("FlowBindings");
    behavior_variable *written_variable = behavior_asset_add_variable(
        asset,
        "Written",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    behavior_variable *observed_variable = behavior_asset_add_variable(
        asset,
        "Observed",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    assert(written_variable && observed_variable);
    uint64_t written_id = written_variable->id;
    uint64_t observed_id = observed_variable->id;
    node *event = graph_add_node(asset->graph, &NODE_Event);
    node *write = behavior_asset_add_set_node(asset, written_id);
    node *read = behavior_asset_add_set_node(asset, observed_id);
    assert(event && write && read);
    assert(graph_set_event_name(asset->graph, event, "Go") == 0);
    assert(
        graph_set_pin_default(
            asset->graph,
            write,
            0,
            1,
            &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 7 }
        ) == 0
    );
    assert(
        graph_set_pin_binding(
            asset->graph,
            read,
            0,
            1,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = written_id }
        ) == 0
    );
    assert(graph_connect_exec(asset->graph, event, 0, write, 0, 0) == 0);
    assert(graph_connect_exec(asset->graph, write, 0, read, 0, 0) == 0);
    behavior_instance *instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    flow_run *run = dispatch(instance, "Go");
    pin_value value;
    test_check(
        flow_run_status(run) == FLOW_COMPLETED &&
            behavior_instance_get_variable(instance, observed_id, &value) == 0 &&
            value.i == 7,
        "flow bindings read a variable after an earlier activation writes it"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(asset);

    asset = behavior_asset_new("DeltaBindings");
    behavior_variable *delta = behavior_asset_add_variable(
        asset,
        "Delta",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 0 },
        NULL
    );
    event = graph_add_node(asset->graph, &NODE_Event);
    node *store = behavior_asset_add_set_node(asset, delta->id);
    assert(event && store);
    assert(graph_set_event_name(asset->graph, event, "Tick") == 0);
    assert(
        graph_set_pin_binding(asset->graph, store, 0, 1, &(pin_binding){ .kind = PIN_BINDING_DELTA_TIME }) == 0
    );
    assert(graph_connect_exec(asset->graph, event, 0, store, 0, 0) == 0);
    instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    behavior_instance_set_delta_seconds(instance, .125);
    run = dispatch(instance, "Tick");
    assert(behavior_instance_get_variable(instance, delta->id, &value) == 0);
    test_check(
        flow_run_status(run) == FLOW_COMPLETED && fabsf(value.f - .125f) < .0001f,
        "flow Delta Time resolves from the current activation"
    );
    behavior_instance_set_delta_seconds(instance, .75);
    run = dispatch(instance, "Tick");
    assert(behavior_instance_get_variable(instance, delta->id, &value) == 0);
    test_check(
        flow_run_status(run) == FLOW_COMPLETED && fabsf(value.f - .75f) < .0001f,
        "Delta Time is sampled again for each flow activation"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(asset);
}

static void event_context_sources()
{
    behavior_asset *asset = behavior_asset_new("EventContextSources");
    behavior_variable *observed_delta = behavior_asset_add_variable(
        asset,
        "ObservedDelta",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 0 },
        NULL
    );
    behavior_variable *observed_value = behavior_asset_add_variable(
        asset,
        "ObservedValue",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    node *event = graph_add_node(asset->graph, &NODE_Event);
    node *delta = graph_add_node(asset->graph, &NODE_DeltaTime);
    node *store_delta = behavior_asset_add_set_node(asset, observed_delta->id);
    node *store_value = behavior_asset_add_set_node(asset, observed_value->id);
    pin_spec delta_seconds = { "DeltaSeconds", PIN_F32, "float", PIN_DATA };
    pin_spec value = { "Value", PIN_I32, "int", PIN_DATA };
    assert(observed_delta && observed_value && event && delta && store_delta && store_value);
    assert(graph_set_event_name(asset->graph, event, "Tick") == 0);
    assert(graph_add_pin(asset->graph, event, 1, &delta_seconds) == 1);
    assert(graph_add_pin(asset->graph, event, 1, &value) == 2);
    assert(graph_connect_exec(asset->graph, event, 0, store_delta, 0, 0) == 0);
    assert(graph_connect_exec(asset->graph, store_delta, 0, store_value, 0, 0) == 0);
    assert(graph_connect_data(asset->graph, delta, 0, store_delta, 1) == 0);
    assert(
        graph_set_pin_binding(
            asset->graph,
            store_value,
            0,
            1,
            &(pin_binding){ .kind = PIN_BINDING_EVENT_OUTPUT,
                            .event_node_id = event->id,
                            .event_pin_id = event->output_pins[2].id }
        ) == 0
    );
    const pin_binding *binding = graph_pin_binding(asset->graph, store_value, 0, 1);
    test_check(
        binding && binding->kind == PIN_BINDING_EVENT_OUTPUT &&
            binding->event_node_id == event->id &&
            binding->event_pin_id == event->output_pins[2].id,
        "event output bindings retain stable event and pin identity"
    );

    char *json = behavior_asset_save(asset);
    behavior_asset *roundtrip = json ? behavior_asset_load(json) : NULL;
    graph *roundtrip_graph = roundtrip ? roundtrip->graph : NULL;
    node *roundtrip_store = roundtrip_graph
                              ? graph_node_by_id(roundtrip_graph, store_value->id)
                              : NULL;
    binding = roundtrip_store
                ? graph_pin_binding(roundtrip_graph, roundtrip_store, 0, 1)
                : NULL;
    test_check(
        roundtrip && binding && binding->kind == PIN_BINDING_EVENT_OUTPUT &&
            binding->event_node_id == event->id &&
            binding->event_pin_id == event->output_pins[2].id,
        "event output bindings survive asset serialization"
    );
    free(json);
    behavior_asset_free(roundtrip);

    node *subset[] = { store_value };
    char *clipboard = graph_save_nodes(asset->graph, subset, 1);
    graph partial;
    graph_init(&partial);
    int appended = clipboard ? graph_append(&partial, clipboard, 0, 0) : -1;
    node *pasted = acount(partial.nodes) == 1 ? partial.nodes[0] : NULL;
    test_check(
        appended == 0 && pasted && !graph_pin_binding(&partial, pasted, 0, 1),
        "partial clipboard drops an event binding without its source event"
    );
    graph_free(&partial);
    free(clipboard);

    for (int backend = 0; backend < 2; backend++) {
        behavior_instance *instance = instance_for(asset, NULL, (ecs_entity){ 0 });
        if (backend) {
            assert(behavior_instance_set_backend(instance, BEHAVIOR_ORACLE) == 0);
        }
        behavior_instance_set_delta_seconds(instance, .75);
        pin_value inputs[] = {
            { .type = PIN_F32, .type_name = "float", .f = .25f },
            { .type = PIN_I32, .type_name = "int", .i = 17 },
        };
        flow_error error = { 0 };
        flow_run *run = behavior_instance_dispatch(
            instance,
            "Tick",
            (flow_inputs){ inputs, 2, NULL },
            &error
        );
        pin_value observed;
        test_check(
            run && flow_run_status(run) == FLOW_COMPLETED &&
                behavior_instance_get_variable(instance, observed_delta->id, &observed) == 0 &&
                fabsf(observed.f - .25f) < .0001f &&
                behavior_instance_get_variable(instance, observed_value->id, &observed) == 0 &&
                observed.i == 17,
            backend ? "event outputs and Delta Time agree in the interpreter"
                    : "event outputs and Delta Time agree in compiled execution"
        );
        behavior_instance_destroy(instance);
    }
    behavior_asset_free(asset);
}

static void executable_event_binding_extraction()
{
    behavior_asset *asset = behavior_asset_new("ExecutableEventBindingExtraction");
    behavior_variable *observed = behavior_asset_add_variable(
        asset,
        "Observed",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    behavior_variable *tail = behavior_asset_add_variable(
        asset,
        "Tail",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    node *event = graph_add_node(asset->graph, &NODE_Event);
    node *write = observed ? behavior_asset_add_set_node(asset, observed->id) : NULL;
    node *after = tail ? behavior_asset_add_set_node(asset, tail->id) : NULL;
    pin_spec value = { "Value", PIN_I32, "int", PIN_DATA };
    assert(observed && tail && event && write && after);
    assert(graph_set_event_name(asset->graph, event, "Input Event") == 0);
    assert(graph_add_pin(asset->graph, event, 1, &value) == 1);
    assert(graph_connect_exec(asset->graph, event, 0, write, 0, 0) == 0);
    assert(graph_connect_exec(asset->graph, write, 0, after, 0, 0) == 0);
    assert(
        graph_set_pin_binding(
            asset->graph,
            write,
            0,
            1,
            &(pin_binding){ .kind = PIN_BINDING_EVENT_OUTPUT,
                            .event_node_id = event->id,
                            .event_pin_id = event->output_pins[1].id }
        ) == 0
    );

    node *selection[] = { write };
    behavior_asset *extracted = NULL;
    uint64_t call_id = 0;
    const char *error = behavior_asset_extract_function(
        asset,
        asset->graph,
        selection,
        1,
        "EventValue",
        &extracted,
        &call_id
    );
    graph *function = extracted
                        ? behavior_asset_find_graph_by_name(extracted, "EventValue")
                        : NULL;
    node *call = extracted ? graph_node_by_id(extracted->graph, call_id) : NULL;
    node *copy = function
                   ? find_function_node(function, &NODE_SetVariable, observed->id)
                   : NULL;
    const pin_binding *call_binding = call
                                        ? graph_pin_binding(extracted->graph, call, 0, 1)
                                        : NULL;
    test_check(
        !error && function && call && copy &&
            !graph_pin_binding(function, copy, 0, 1) && call_binding &&
            call_binding->kind == PIN_BINDING_EVENT_OUTPUT,
        "executable extraction moves event output bindings to the call input"
    );

    for (int backend = 0; backend < 2 && extracted; backend++) {
        behavior_instance *instance = instance_for(extracted, NULL, (ecs_entity){ 0 });
        if (backend) {
            assert(behavior_instance_set_backend(instance, BEHAVIOR_ORACLE) == 0);
        }
        pin_value input = { .type = PIN_I32, .type_name = "int", .i = 17 };
        flow_error flow_error = { 0 };
        flow_run *run = behavior_instance_dispatch(
            instance,
            "Input Event",
            (flow_inputs){ &input, 1, NULL },
            &flow_error
        );
        pin_value result;
        test_check(
            run && flow_run_status(run) == FLOW_COMPLETED &&
                behavior_instance_get_variable(instance, observed->id, &result) == 0 &&
                result.i == 17,
            backend ? "extracted event output reaches the interpreter"
                    : "extracted event output reaches compiled execution"
        );
        behavior_instance_destroy(instance);
    }
    behavior_asset_free(extracted);
    behavior_asset_free(asset);
}

static void component_and_context_bindings()
{
    ecs_world *world = ecs_world_new();
    assert(world);
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameTransform, NULL) == ECS_OK);
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameMovement, NULL) == ECS_OK);
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameAim, NULL) == ECS_OK);
    ecs_entity owner = ecs_world_create_entity(world);
    GameTransform transform = { 4, -2 };
    GameMovement movement = { .speed = 90 };
    GameAim aim = { .direction = { 1.25f, -2.5f }, .firing = true };
    assert(
        ecs_world_record_add(world, owner, GAME_COMPONENT_GameTransform.id, &transform, 0) == ECS_OK
    );
    assert(
        ecs_world_record_add(world, owner, GAME_COMPONENT_GameMovement.id, &movement, 0) == ECS_OK
    );
    assert(ecs_world_record_add(world, owner, GAME_COMPONENT_GameAim.id, &aim, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);

    behavior_asset *asset = behavior_asset_new("ComponentBindings");
    assert(behavior_asset_set_owner_type(asset, "ecs.entity") == 0);
    node *target = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            asset->graph,
            target,
            0,
            0,
            &(pin_binding){
                .kind = PIN_BINDING_COMPONENT_FIELD,
                .component_key = GAME_COMPONENT_GameTransform.key,
                .field_name = "x",
            }
        ) == 0
    );
    compile_report report;
    compile_report_init(&report);
    behavior_asset_compile(asset, &report);
    test_check(
        (report.effects.flags & NODE_EFFECT_WORLD_READ) && !report.effects.parallel_eligible,
        "component input bindings contribute a world read effect"
    );
    compile_report_free(&report);

    behavior_asset *nested = behavior_asset_new("NestedComponentBindings");
    assert(behavior_asset_set_owner_type(nested, "ecs.entity") == 0);
    node *nested_target = typed_reroute(nested->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            nested->graph,
            nested_target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_COMPONENT_FIELD,
                            .component_key = GAME_COMPONENT_GameAim.key,
                            .field_name = "direction.x" }
        ) == 0
    );
    const pin_binding *nested_binding = graph_pin_binding(nested->graph, nested_target, 0, 0);
    compile_report nested_report;
    compile_report_init(&nested_report);
    test_check(
        nested_binding && nested_binding->field_name &&
            !strcmp(nested_binding->field_name, "direction.x") &&
            behavior_asset_compile(nested, &nested_report) == 0 &&
            nested_report.error_count == 0,
        "nested component field bindings validate as scalar inputs"
    );
    compile_report_free(&nested_report);

    behavior_asset *whole = behavior_asset_new("WholeComponentBindings");
    assert(behavior_asset_set_owner_type(whole, "ecs.entity") == 0);
    node *whole_target = typed_reroute(whole->graph, PIN_STRUCT, "GameMovement");
    assert(
        graph_set_pin_binding(
            whole->graph,
            whole_target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_COMPONENT,
                            .component_key = GAME_COMPONENT_GameMovement.key }
        ) == 0
    );
    const pin_binding *whole_binding = graph_pin_binding(whole->graph, whole_target, 0, 0);
    test_check(
        whole_binding && whole_binding->kind == PIN_BINDING_COMPONENT &&
            whole_binding->component_key &&
            strcmp(whole_binding->component_key, GAME_COMPONENT_GameMovement.key) == 0 &&
            !whole_binding->field_name,
        "whole-component bindings retain a stable component key without a field"
    );
    compile_report whole_report;
    compile_report_init(&whole_report);
    test_check(
        behavior_asset_compile(whole, &whole_report) == 0 &&
            whole_report.error_count == 0 &&
            (whole_report.effects.flags & NODE_EFFECT_WORLD_READ) &&
            !whole_report.effects.parallel_eligible,
        "whole-component input bindings contribute a world read effect"
    );
    compile_report_free(&whole_report);

    behavior_asset *mismatch = behavior_asset_new("WholeComponentMismatch");
    assert(behavior_asset_set_owner_type(mismatch, "ecs.entity") == 0);
    node *mismatch_target = typed_reroute(mismatch->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            mismatch->graph,
            mismatch_target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_COMPONENT,
                            .component_key = GAME_COMPONENT_GameTransform.key }
        ) == 0
    );
    compile_report mismatch_report;
    compile_report_init(&mismatch_report);
    test_check(
        behavior_asset_compile(mismatch, &mismatch_report) != 0 &&
            mismatch_report.error_count &&
            mismatch_report.items[0].rule == CMPL_RULE_INPUT_BINDING &&
            strstr(mismatch_report.items[0].message, "incompatible"),
        "the compiler rejects a whole-component binding with a scalar input"
    );
    compile_report_free(&mismatch_report);

    behavior_instance *instance = instance_for(asset, world, owner);
    eval_component_reader reader;
    ecs_component_reader_init(&reader, world);
    service_binding *services = NULL;
    apush(services, ((service_binding){ EVAL_COMPONENT_READER_SERVICE, &reader }));
    float result = 0;
    char message[192];
    behavior_instance *nested_instance = instance_for(nested, world, owner);
    float nested_result = 0;
    test_check(
        eval_f32(nested, nested_target, nested_instance, services, 0, &nested_result, message, sizeof message) ==
                0 &&
            fabsf(nested_result - 1.25f) < .0001f,
        "nested component field bindings read through the reflected path"
    );
    GAME_GameAim_get(world, owner)->direction.x = 3.5f;
    test_check(
        eval_f32(nested, nested_target, nested_instance, services, 0, &nested_result, message, sizeof message) ==
                0 &&
            fabsf(nested_result - 3.5f) < .0001f,
        "nested component field bindings observe owner mutations"
    );
    behavior_instance_destroy(nested_instance);
    test_check(
        eval_f32(asset, target, instance, services, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 4) < .0001f,
        "component field bindings read the owner component"
    );
    GAME_GameTransform_get(world, owner)->x = 11;
    test_check(
        eval_f32(asset, target, instance, services, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 11) < .0001f,
        "component field bindings observe mutations made before evaluation"
    );
    behavior_instance_destroy(instance);

    instance = instance_for(whole, world, owner);
    arena whole_storage;
    arena_init(&whole_storage, 256);
    pin_value whole_value = { 0 };
    test_check(
        eval_pin_value(whole, whole_target, instance, services, 0, &whole_storage, &whole_value, message, sizeof message) ==
                0 &&
            whole_value.type == PIN_STRUCT &&
            whole_value.container == PIN_CONTAINER_SINGLE && whole_value.type_name &&
            strcmp(whole_value.type_name, "GameMovement") == 0 && whole_value.ptr &&
            ((const GameMovement *)whole_value.ptr)->speed == 90 &&
            whole_value.ptr != GAME_GameMovement_get(world, owner),
        "whole-component bindings read a copied struct from the owner component"
    );
    GAME_GameMovement_get(world, owner)->speed = 120;
    test_check(
        ((const GameMovement *)whole_value.ptr)->speed == 90,
        "whole-component snapshots outlive mutations to the ECS storage"
    );
    arena_free(&whole_storage);
    behavior_instance_destroy(instance);

    instance = instance_for(mismatch, world, owner);
    arena mismatch_storage;
    arena_init(&mismatch_storage, 256);
    pin_value mismatch_value = { 0 };
    test_check(
        eval_pin_value(
            mismatch,
            mismatch_target,
            instance,
            services,
            0,
            &mismatch_storage,
            &mismatch_value,
            message,
            sizeof message
        ) != 0 &&
            strstr(message, "incompatible"),
        "runtime rejects a whole-component value incompatible with the input"
    );
    arena_free(&mismatch_storage);
    behavior_instance_destroy(instance);

    node *whole_source = typed_reroute(whole->graph, PIN_STRUCT, "GameMovement");
    GameMovement authored_movement = { .speed = 33 };
    assert(
        graph_set_pin_default(
            whole->graph,
            whole_source,
            0,
            0,
            &(pin_value){ .type = PIN_STRUCT, .type_name = "GameMovement", .ptr = &authored_movement }
        ) == 0
    );
    assert(graph_connect_data(whole->graph, whole_source, 0, whole_target, 0) == 0);
    uint64_t whole_target_id = whole_target->id;
    uint64_t whole_source_id = whole_source->id;
    char *whole_json = behavior_asset_save(whole);
    assert(whole_json);
    behavior_asset_free(whole);
    whole = behavior_asset_load(whole_json);
    free(whole_json);
    assert(whole);
    whole_target = graph_node_by_id(whole->graph, whole_target_id);
    whole_source = graph_node_by_id(whole->graph, whole_source_id);
    whole_binding = graph_pin_binding(whole->graph, whole_target, 0, 0);
    test_check(
        whole_binding && whole_binding->kind == PIN_BINDING_COMPONENT &&
            whole_binding->component_key &&
            strcmp(whole_binding->component_key, GAME_COMPONENT_GameMovement.key) == 0 &&
            !whole_binding->field_name,
        "JSON roundtrip preserves whole-component binding identity"
    );
    instance = instance_for(whole, world, owner);
    arena whole_roundtrip_storage;
    arena_init(&whole_roundtrip_storage, 256);
    pin_value whole_roundtrip_value = { 0 };
    test_check(
        eval_pin_value(
            whole,
            whole_target,
            instance,
            services,
            0,
            &whole_roundtrip_storage,
            &whole_roundtrip_value,
            message,
            sizeof message
        ) == 0 &&
            whole_roundtrip_value.ptr &&
            ((const GameMovement *)whole_roundtrip_value.ptr)->speed == 33,
        "a reloaded whole-component binding retains wire precedence"
    );
    behavior_instance_destroy(instance);
    assert(graph_remove_edge(whole->graph, whole_source, 0, whole_target, 0) == 0);
    instance = instance_for(whole, world, owner);
    arena_reset(&whole_roundtrip_storage);
    test_check(
        eval_pin_value(
            whole,
            whole_target,
            instance,
            services,
            0,
            &whole_roundtrip_storage,
            &whole_roundtrip_value,
            message,
            sizeof message
        ) == 0 &&
            whole_roundtrip_value.ptr &&
            ((const GameMovement *)whole_roundtrip_value.ptr)->speed == 120 &&
            whole_roundtrip_value.ptr != GAME_GameMovement_get(world, owner),
        "disconnecting a reloaded wire restores the whole-component binding"
    );
    GAME_GameMovement_get(world, owner)->speed = 240;
    test_check(
        ((const GameMovement *)whole_roundtrip_value.ptr)->speed == 120,
        "reloaded whole-component snapshots retain their copied value"
    );
    arena_free(&whole_roundtrip_storage);
    behavior_instance_destroy(instance);

    ecs_entity missing = ecs_world_create_entity(world);
    instance = instance_for(whole, world, missing);
    arena missing_storage;
    arena_init(&missing_storage, 256);
    pin_value missing_value = { 0 };
    test_check(
        eval_pin_value(whole, whole_target, instance, services, 0, &missing_storage, &missing_value, message, sizeof message) !=
                0 &&
            strstr(message, "does not have"),
        "whole-component bindings report a missing owner component"
    );
    arena_free(&missing_storage);
    behavior_instance_destroy(instance);
    behavior_asset_free(whole);
    behavior_asset_free(mismatch);
    afree(services);

    missing = ecs_world_create_entity(world);
    instance = instance_for(asset, world, missing);
    apush(services, ((service_binding){ EVAL_COMPONENT_READER_SERVICE, &reader }));
    test_check(
        eval_f32(asset, target, instance, services, 0, &result, message, sizeof message) != 0 &&
            strstr(message, "does not have"),
        "component bindings report a missing owner component"
    );
    behavior_instance_destroy(instance);
    afree(services);
    behavior_asset_free(asset);
    behavior_asset_free(nested);
    ecs_world_free(world);

    asset = behavior_asset_new("ContextBinding");
    target = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(asset->graph, target, 0, 0, &(pin_binding){ .kind = PIN_BINDING_DELTA_TIME }) == 0
    );
    test_check(
        eval_f32(asset, target, NULL, NULL, .03125, &result, message, sizeof message) == 0 &&
            fabsf(result - .03125f) < .0001f,
        "direct Delta Time bindings use the evaluation context"
    );
    behavior_asset_free(asset);
}

static void converted_component_field_binding()
{
    ecs_world *world = ecs_world_new();
    assert(world);
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameHealth, NULL) == ECS_OK);
    assert(ecs_world_register_component(world, &GAME_COMPONENT_GameMovement, NULL) == ECS_OK);
    ecs_entity owner = ecs_world_create_entity(world);
    GameHealth health = { .current = 17, .maximum = 100 };
    assert(ecs_world_record_add(world, owner, GAME_COMPONENT_GameHealth.id, &health, 0) == ECS_OK);
    GameMovement movement = { .speed = 90 };
    assert(
        ecs_world_record_add(world, owner, GAME_COMPONENT_GameMovement.id, &movement, 0) == ECS_OK
    );
    assert(ecs_world_apply_commands(world) == ECS_OK);

    behavior_asset *asset = behavior_asset_new("ConvertedComponentBinding");
    assert(behavior_asset_set_owner_type(asset, "ecs.entity") == 0);
    node *target = typed_reroute(asset->graph, PIN_F64, "double");
    assert(
        graph_set_pin_binding(
            asset->graph,
            target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_COMPONENT_FIELD,
                            .component_key = GAME_COMPONENT_GameHealth.key,
                            .field_name = "current" }
        ) == 0
    );
    compile_report report;
    compile_report_init(&report);
    test_check(
        behavior_asset_compile(asset, &report) == 0 && report.error_count == 0,
        "component field bindings accept registered scalar conversions"
    );
    compile_report_free(&report);

    eval_component_reader reader;
    ecs_component_reader_init(&reader, world);
    service_binding *services = NULL;
    apush(services, ((service_binding){ EVAL_COMPONENT_READER_SERVICE, &reader }));
    behavior_instance *instance = instance_for(asset, world, owner);
    float result = 0;
    char message[192];
    test_check(
        eval_f32(asset, target, instance, services, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 17) < .0001f,
        "direct component field evaluation applies the registered conversion"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(asset);

    behavior_asset *auto_asset = behavior_asset_new("ConvertedGetterSimplification");
    assert(behavior_asset_set_owner_type(auto_asset, "ecs.entity") == 0);
    editor e = { .asset = auto_asset,
                 .active_graph = auto_asset->graph,
                 .ui_context = EDITOR_UI_CONTEXT_BEHAVIOR };
    node *getter = graph_add_node(auto_asset->graph, registry_node_by_name("Get_GameHealth"));
    assert(getter && graph_expose_output_fields(auto_asset->graph, getter, 0) == 0);
    node *converted = graph_add_node(auto_asset->graph, registry_node_by_name("Sine"));
    ed_pin_view output = { getter, ED_PIN_OUTPUT, 1 };
    ed_pin_view input = { converted, ED_PIN_INPUT, 0 };
    bool connected = getter && converted && ed_connect(&e, &output, &input);
    const pin_binding *binding = connected
                                   ? graph_pin_binding(auto_asset->graph, converted, 0, 0)
                                   : NULL;
    test_check(
        connected && binding && binding->kind == PIN_BINDING_COMPONENT_FIELD &&
            binding->component_key &&
            !strcmp(binding->component_key, GAME_COMPONENT_GameHealth.key) &&
            binding->field_name && !strcmp(binding->field_name, "current") &&
            acount(auto_asset->graph->nodes) == 1,
        "inlining a converted component getter stores the field binding"
    );
    instance = instance_for(auto_asset, world, owner);
    int inline_status = eval_f32(auto_asset, converted, instance, services, 0, &result, message, sizeof message);
    test_check(
        inline_status == 0 && result == (float)cos_sin_sine(17.0),
        "an inlined converted getter evaluates through the field binding"
    );
    behavior_instance_destroy(instance);
    ed_free_asset_stack(&e.undo);
    behavior_asset_free(auto_asset);

    behavior_asset *flow_asset = behavior_asset_new("CompiledConvertedComponentBinding");
    assert(behavior_asset_set_owner_type(flow_asset, "ecs.entity") == 0);
    behavior_variable *observed = behavior_asset_add_variable(
        flow_asset,
        "Observed",
        PIN_F64,
        "double",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 0 },
        NULL
    );
    node *event = graph_add_node(flow_asset->graph, &NODE_Event);
    node *set = observed ? behavior_asset_add_set_node(flow_asset, observed->id) : NULL;
    assert(observed && event && set);
    assert(graph_set_event_name(flow_asset->graph, event, "Read") == 0);
    assert(graph_connect_exec(flow_asset->graph, event, 0, set, 0, 0) == 0);
    const pin_binding bindings[] = {
        { .kind = PIN_BINDING_COMPONENT_FIELD,
          .component_key = GAME_COMPONENT_GameHealth.key,
          .field_name = "current" },
        { .kind = PIN_BINDING_COMPONENT,
          .component_key = GAME_COMPONENT_GameMovement.key },
    };
    const double expected[] = { 17, 90 };
    const behavior_backend backends[] = { BEHAVIOR_SLJIT, BEHAVIOR_BYTECODE, BEHAVIOR_ORACLE };
    for (int source = 0; source < 2; source++) {
        assert(graph_set_pin_binding(flow_asset->graph, set, 0, 1, &bindings[source]) == 0);
        compile_report flow_report;
        compile_report_init(&flow_report);
        test_check(
            behavior_asset_compile(flow_asset, &flow_report) == 0 &&
                flow_report.error_count == 0,
            source
                ? "whole-component bindings accept registered unwrapping"
                : "compiled component field bindings accept the same conversion"
        );
        compile_report_free(&flow_report);
        for (int backend = 0; backend < 3; backend++) {
            instance = instance_for(flow_asset, world, owner);
            assert(
                behavior_instance_bind_service(instance, EVAL_COMPONENT_READER_SERVICE, &reader) == 0
            );
            assert(behavior_instance_set_backend(instance, backends[backend]) == 0);
            flow_error error = { 0 };
            flow_run *run = behavior_instance_dispatch(instance, "Read", (flow_inputs){ 0 }, &error);
            pin_value value;
            test_check(
                run && flow_run_status(run) == FLOW_COMPLETED &&
                    behavior_instance_get_variable(instance, observed->id, &value) == 0 &&
                    value.type == PIN_F64 && fabs(value.d - expected[source]) < .0001,
                source ? "whole-component unwrapping agrees across execution "
                         "backends"
                       : "converted component fields agree across execution "
                         "backends"
            );
            behavior_instance_destroy(instance);
        }
    }
    behavior_asset_free(flow_asset);
    afree(services);
    ecs_world_free(world);
}

static void named_disk_references()
{
    behavior_asset *asset = behavior_asset_new("NamedReferences");
    behavior_variable *variable = behavior_asset_add_variable(
        asset,
        "Speed",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .f = 4 },
        NULL
    );
    uint64_t variable_id = variable->id;
    node *bound = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            asset->graph,
            bound,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = variable_id }
        ) == 0
    );
    node *source = typed_reroute(asset->graph, PIN_F32, "float");
    node *local_use = typed_reroute(asset->graph, PIN_F32, "float");
    assert(graph_connect_data(asset->graph, source, 0, local_use, 0) == 0);
    uint64_t local_id = graph_create_local(asset->graph, source, 0, "CurrentSpeed");
    assert(local_id);
    graph *function = behavior_asset_add_function(asset, "ApplySpeed", 0);
    assert(function);
    uint64_t function_id = function->graph_id;
    node *call = graph_add_node(asset->graph, &NODE_CallFunction);
    assert(graph_set_node_reference(asset->graph, call, function_id, function->name) == 0);
    uint64_t bound_id = bound->id, local_use_id = local_use->id, call_id = call->id;
    assert(behavior_asset_rename_variable(asset, variable_id, "Velocity") == 0);
    assert(graph_rename_local(asset->graph, local_id, "CurrentVelocity") == 0);
    assert(behavior_asset_rename_graph(asset, function_id, "ApplyVelocity") == 0);
    node *setter = behavior_asset_add_set_node(asset, variable_id);
    assert(setter && graph_set_node_reference(asset->graph, setter, variable_id, NULL) == 0);
    assert(
        graph_set_pin_default(asset->graph, setter, 0, 1, &(pin_value){ .type = PIN_F32, .f = 9 }) == 0
    );
    char *text = behavior_asset_save(asset);
    test_check(
        text && strstr(text, "Velocity") && strstr(text, "CurrentVelocity") &&
            strstr(text, "ApplyVelocity") && !strstr(text, "variable_id") &&
            !strstr(text, "local_id") && !strstr(text, "reference_id"),
        "asset files use current unique names for bindings and forward "
        "function references"
    );
    behavior_asset *loaded = behavior_asset_load(text);
    test_check(
        loaded &&
            graph_node_by_id(loaded->graph, bound_id)->input_pins[0].binding->variable_id ==
                variable_id &&
            graph_node_by_id(loaded->graph, local_use_id)
                    ->input_pins[0]
                    .binding->local_id == local_id &&
            graph_node_by_id(loaded->graph, call_id)->reference_id == function_id,
        "named references restore stable IDs after loading all function "
        "declarations"
    );
    if (loaded) {
        char *again = behavior_asset_save(loaded);
        test_check(again && !strcmp(text, again), "named reference files keep stable field order when numeric setters originally have no reference name");
        free(again);
        behavior_asset_free(loaded);
    }
    av_doc *doc = av_doc_new();
    av_value *root = behavior_asset_to_value(doc, asset);
    char *internal = av_value_write(root);
    test_check(
        strstr(internal, "variable_id") && strstr(internal, "local_id") &&
            strstr(internal, "reference_id"),
        "in-memory asset snapshots retain numeric reference identities"
    );
    free(internal);
    av_doc_free(doc);
    node *selected[] = { bound };
    char *clipboard = graph_save_nodes(asset->graph, selected, 1);
    test_check(
        clipboard && strstr(clipboard, "variable_id") &&
            graph_append(asset->graph, clipboard, 0, 0) == 0,
        "clipboard copies retain owner-checked numeric variable references"
    );
    free(clipboard);
    doc = av_doc_new();
    root = behavior_asset_to_disk_value(doc, asset);
    av_value *nodes = av_obj_get(av_arr_get(av_obj_get(root, "graphs"), 0), "nodes");
    av_value *call_value = NULL;
    for (size_t i = 0; i < av_arr_size(nodes); i++) {
        av_value *value = av_arr_get(nodes, i);
        if (av_get_uint(av_obj_get(value, "id")) == call_id) {
            call_value = value;
        }
    }
    assert(call_value);
    av_obj_set(doc, call_value, "reference_name", av_str(doc, "MissingFunction"));
    loaded = behavior_asset_from_value(root);
    test_check(!loaded && strstr(graph_last_error(), "unknown function"), "missing named function references fail during loading");
    behavior_asset_free(loaded);
    av_obj_set(doc, call_value, "reference_name", av_str(doc, "ApplyVelocity"));
    uint64_t targets[] = { bound_id, local_use_id };
    const char *names[] = { "Velocity", "CurrentVelocity" };
    for (int at = 0; at < 2; at++) {
        av_value *binding = NULL;
        for (size_t i = 0; i < av_arr_size(nodes); i++) {
            av_value *value = av_arr_get(nodes, i);
            if (av_get_uint(av_obj_get(value, "id")) != targets[at]) {
                continue;
            }
            av_value *inputs = av_obj_get(value, "inputs");
            for (size_t j = 0; j < av_obj_size(inputs); j++) {
                binding = av_obj_get(av_obj_value_at(inputs, j), "binding");
                if (binding) {
                    break;
                }
            }
        }
        assert(binding);
        av_obj_set(doc, binding, "name", av_str(doc, "MissingBinding"));
        loaded = behavior_asset_from_value(root);
        test_check(
            !loaded && strstr(graph_last_error(), "unknown name"),
            "missing named variable and local bindings fail during loading"
        );
        behavior_asset_free(loaded);
        av_obj_set(doc, binding, "name", av_str(doc, names[at]));
    }
    av_doc_free(doc);
    const char *lists[] = { "variables", "graphs", "locals" };
    for (int at = 0; at < 3; at++) {
        doc = av_doc_new();
        root = behavior_asset_to_disk_value(doc, asset);
        av_value *written_graph = av_arr_get(av_obj_get(root, "graphs"), 0);
        av_value *list = av_obj_get(at == 2 ? written_graph : root, lists[at]);
        av_value *duplicate = av_clone(doc, av_arr_get(list, at == 1 ? 1 : 0));
        av_obj_set(doc, duplicate, "id", av_uint(doc, asset->graph->id_counter + 1));
        if (at == 2) {
            char from[SERIALIZE_KEY_SIZE + 32];
            assert(serialize_pin_ref(bound, 1, 0, from, sizeof from));
            av_obj_set(doc, duplicate, "from", av_str(doc, from));
        }
        av_arr_append(list, duplicate);
        loaded = behavior_asset_from_value(root);
        test_check(!loaded, "loaded duplicate variable, graph and local names cannot make symbolic references ambiguous");
        behavior_asset_free(loaded);
        av_doc_free(doc);
    }
    free(text);
    behavior_asset_free(asset);
}

static void serialization_and_clipboard_ownership()
{
    behavior_asset *asset = behavior_asset_new("SerializedBinding");
    behavior_variable *variable = behavior_asset_add_variable(
        asset,
        "Value",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 4 },
        NULL
    );
    node *target = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            asset->graph,
            target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE,
                            .variable_id = variable->id }
        ) == 0
    );
    uint64_t target_id = target->id;
    char *json = behavior_asset_save(asset);
    assert(json);
    behavior_asset_free(asset);
    asset = behavior_asset_load(json);
    free(json);
    assert(asset);
    target = graph_node_by_id(asset->graph, target_id);
    const pin_binding *binding = graph_pin_binding(asset->graph, target, 0, 0);
    test_check(
        binding && binding->source_asset &&
            strcmp(binding->source_asset, "SerializedBinding") == 0,
        "serialized bindings restore owned identity strings"
    );
    behavior_instance *instance = instance_for(asset, NULL, (ecs_entity){ 0 });
    float result = 0;
    char message[192];
    test_check(
        eval_f32(asset, target, instance, NULL, 0, &result, message, sizeof message) == 0 &&
            fabsf(result - 4) < .0001f,
        "a reloaded binding still resolves after its source asset was freed"
    );
    behavior_instance_destroy(instance);

    node *selection[] = { target };
    json = graph_save_nodes(asset->graph, selection, 1);
    assert(json);
    test_check(graph_append(asset->graph, json, 20, 20) == 0, "clipboard copies preserve bindings within the owning behavior");
    free(json);
    behavior_asset_free(asset);

    behavior_asset *source = behavior_asset_new("SameName");
    variable = behavior_asset_add_variable(
        source,
        "Value",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 1 },
        NULL
    );
    target = typed_reroute(source->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            source->graph,
            target,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE,
                            .variable_id = variable->id }
        ) == 0
    );
    selection[0] = target;
    json = graph_save_nodes(source->graph, selection, 1);
    assert(json);
    behavior_asset *same_name = behavior_asset_new("SameName");
    test_check(graph_append(same_name->graph, json, 0, 0) != 0, "clipboard copies reject variable bindings from another owner with the same name");
    free(json);
    behavior_asset_free(same_name);
    behavior_asset_free(source);

    graph standalone = { 0 };
    graph_init(&standalone);
    target = typed_reroute(&standalone, PIN_F32, "float");
    char component_name[] = "GameTransform";
    char field_name[] = "x";
    pin_binding owned = { .kind = PIN_BINDING_COMPONENT_FIELD,
                          .component_key = component_name,
                          .field_name = field_name };
    pin_spec extra = { .name = "Owned",
                       .type = PIN_F32,
                       .type_name = "float",
                       .kind = PIN_DATA,
                       .binding = &owned };
    assert(graph_add_pin(&standalone, target, 0, &extra) == 1);
    binding = graph_pin_binding(&standalone, target, 0, 1);
    test_check(
        binding && binding->component_key &&
            strcmp(binding->component_key, "GameTransform") == 0 &&
            binding->field_name && strcmp(binding->field_name, "x") == 0,
        "duplicating a pin deep-copies binding identity strings"
    );
    graph_free(&standalone);
}

static void inline_source_graphs()
{
    graph g = { 0 };
    graph_init(&g);
    node *target = graph_add_node(&g, registry_node_by_name("Add"));
    node *source = graph_add_node(&g, registry_node_by_name("Integer"));
    assert(source && target);
    assert(graph_connect_data_conversion(&g, source, 0, target, 0, &NODE_To_Real) == 0);
    assert(graph_set_node_presentation(&g, source, NODE_PRESENTATION_INLINE) == 0);
    node *inline_source = NULL;
    int inline_pin = -1;
    test_check(
        graph_node_is_inline(&g, source) &&
            graph_inline_source(&g, target, 0, &inline_source, &inline_pin) &&
            inline_source == source && inline_pin == 0 &&
            graph_input_conversion(&g, target, 0) == &NODE_To_Real,
        "inline sources expose a valid converted ordinary edge"
    );

    uint64_t source_id = source->id;
    uint64_t target_id = target->id;
    char *json = graph_save(&g);
    graph *roundtrip = json ? graph_load(json) : NULL;
    node *roundtrip_source = roundtrip ? graph_node_by_id(roundtrip, source_id) : NULL;
    node *roundtrip_target = roundtrip ? graph_node_by_id(roundtrip, target_id) : NULL;
    inline_source = NULL;
    inline_pin = -1;
    test_check(
        roundtrip && roundtrip_source && roundtrip_target &&
            graph_node_is_inline(roundtrip, roundtrip_source) &&
            graph_inline_source(roundtrip, roundtrip_target, 0, &inline_source, &inline_pin) &&
            inline_source == roundtrip_source && inline_pin == 0 &&
            graph_input_conversion(roundtrip, roundtrip_target, 0) == &NODE_To_Real,
        "inline metadata and conversion edges survive graph serialization"
    );
    free(json);
    if (roundtrip) {
        graph_free(roundtrip);
        free(roundtrip);
    }

    node *selection[] = { target };
    json = graph_save_nodes(&g, selection, 1);
    graph pasted = { 0 };
    graph_init(&pasted);
    test_check(
        json && graph_append(&pasted, json, 10, 10) == 0 && acount(pasted.nodes) == 2,
        "copying a folded target includes its inline source"
    );
    node *pasted_source = NULL;
    node *pasted_target = NULL;
    for (int i = 0; i < acount(pasted.nodes); i++) {
        if (pasted.nodes[i]->presentation == NODE_PRESENTATION_INLINE) {
            pasted_source = pasted.nodes[i];
        } else {
            pasted_target = pasted.nodes[i];
        }
    }
    inline_source = NULL;
    inline_pin = -1;
    test_check(
        pasted_source && pasted_target &&
            graph_inline_source(&pasted, pasted_target, 0, &inline_source, &inline_pin) &&
            inline_source == pasted_source && inline_pin == 0 &&
            graph_input_conversion(&pasted, pasted_target, 0) == &NODE_To_Real,
        "pasted inline sources retain their remapped ordinary edge"
    );
    free(json);
    graph_free(&pasted);

    test_check(
        graph_remove_node_tree(&g, target) == 0 && acount(g.nodes) == 0,
        "removing an inline target removes its owned source"
    );
    graph_free(&g);

    graph direct = { 0 };
    graph_init(&direct);
    source = graph_add_node(&direct, registry_node_by_name("Integer"));
    target = graph_add_node(&direct, registry_node_by_name("Add"));
    assert(source && target);
    assert(graph_connect_data_conversion(&direct, source, 0, target, 0, &NODE_To_Real) == 0);
    assert(graph_set_node_presentation(&direct, source, NODE_PRESENTATION_INLINE) == 0);
    test_check(
        graph_remove_node(&direct, source) == 0 && acount(direct.nodes) == 1 &&
            !graph_node_is_inline(&direct, target),
        "removing an inline source leaves its target graph valid"
    );
    graph_free(&direct);

    graph shared = { 0 };
    graph_init(&shared);
    source = graph_add_node(&shared, registry_node_by_name("Integer"));
    target = graph_add_node(&shared, registry_node_by_name("Add"));
    node *other = graph_add_node(&shared, registry_node_by_name("Add"));
    assert(source && target && other);
    assert(graph_connect_data_conversion(&shared, source, 0, target, 0, &NODE_To_Real) == 0);
    assert(graph_connect_data_conversion(&shared, source, 0, other, 0, &NODE_To_Real) == 0);
    assert(graph_set_node_presentation(&shared, source, NODE_PRESENTATION_INLINE) == 0);
    test_check(!graph_node_is_inline(&shared, source), "shared sources expand instead of hiding a fanout");
    graph_free(&shared);
}

static void component_getter_optimization()
{
    behavior_asset *asset = behavior_asset_new("GetterOptimization");
    editor e = { .asset = asset,
                 .active_graph = asset ? asset->graph : NULL,
                 .ui_context = EDITOR_UI_CONTEXT_BEHAVIOR };
    node *getter = asset
                     ? graph_add_node(asset->graph, registry_node_by_name("Get_GameMovement"))
                     : NULL;
    node *target = asset
                     ? graph_add_node(asset->graph, registry_node_by_name("Set_GameMovement"))
                     : NULL;
    if (getter) {
        ed_add_selected(&e, getter);
        e.front_node = getter;
        e.spawn_link_pin.n = getter;
    }
    bool connected = getter && target &&
                     graph_connect_data(asset->graph, getter, 0, target, 1) == 0;
    bool optimized = connected && ed_optimize_graph(&e) == 1;
    const pin_binding *binding = optimized
                                   ? graph_pin_binding(asset->graph, target, 0, 1)
                                   : NULL;
    node *remaining_source = NULL;
    int remaining_pin = -1;
    test_check(
        optimized && binding && binding->kind == PIN_BINDING_COMPONENT &&
            binding->component_key &&
            !strcmp(binding->component_key, GAME_COMPONENT_GameMovement.key) &&
            !binding->field_name && acount(asset->graph->nodes) == 1 &&
            graph_input_source(asset->graph, target, 1, &remaining_source, &remaining_pin) ==
                0 &&
            !remaining_source && remaining_pin == -1 &&
            acount(e.selected) == 0 && !e.front_node && !e.spawn_link_pin.n,
        "component getter optimization replaces a single consumer with a Self "
        "binding"
    );
    ed_free_asset_stack(&e.undo);
    afree(e.selected);
    behavior_asset_free(asset);
}

static void component_getter_auto_simplification()
{
    behavior_asset *asset = behavior_asset_new("GetterAutoSimplification");
    editor e = { .asset = asset,
                 .active_graph = asset ? asset->graph : NULL,
                 .ui_context = EDITOR_UI_CONTEXT_BEHAVIOR };
    node *getter = asset
                     ? graph_add_node(asset->graph, registry_node_by_name("Get_GameMovement"))
                     : NULL;
    node *target = asset
                     ? graph_add_node(asset->graph, registry_node_by_name("Set_GameMovement"))
                     : NULL;
    ed_pin_view output = { getter, ED_PIN_OUTPUT, 0 };
    ed_pin_view input = { target, ED_PIN_INPUT, 1 };
    bool connected = getter && target && ed_connect(&e, &output, &input);
    const pin_binding *binding = connected
                                   ? graph_pin_binding(asset->graph, target, 0, 1)
                                   : NULL;
    test_check(
        connected && binding && binding->kind == PIN_BINDING_COMPONENT &&
            binding->component_key &&
            !strcmp(binding->component_key, GAME_COMPONENT_GameMovement.key) &&
            acount(asset->graph->nodes) == 1,
        "connecting a single-consumer component getter stores a Self binding"
    );
    behavior_asset_free(asset);
}

static void inline_source_extraction()
{
    behavior_asset *asset = behavior_asset_new("InlineExtraction");
    node *target = graph_add_node(asset->graph, registry_node_by_name("Add"));
    node *source = graph_add_node(asset->graph, registry_node_by_name("Integer"));
    node *sink = typed_reroute(asset->graph, PIN_F32, "float");
    assert(source && target && sink);
    assert(
        graph_set_pin_default(
            asset->graph,
            source,
            0,
            0,
            &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 7 }
        ) == 0
    );
    assert(graph_connect_data_conversion(asset->graph, source, 0, target, 0, &NODE_To_Real) == 0);
    assert(graph_connect_data(asset->graph, target, 0, sink, 0) == 0);
    assert(graph_set_node_presentation(asset->graph, source, NODE_PRESENTATION_INLINE) == 0);
    float before = 0;
    char message[192];
    test_check(
        eval_f32(asset, target, NULL, NULL, 0, &before, message, sizeof message) == 0 &&
            fabsf(before - 7) < 0.0001f,
        "inline source evaluates through its normal converted edge"
    );
    uint64_t source_id = source->id;
    uint64_t target_id = target->id;
    node *selection[] = { target };
    behavior_asset *extracted = NULL;
    uint64_t call_id = 0;
    const char *error = behavior_asset_extract_function(
        asset,
        asset->graph,
        selection,
        1,
        "InlineValue",
        &extracted,
        &call_id
    );
    graph *function = extracted
                        ? behavior_asset_find_graph_by_name(extracted, "InlineValue")
                        : NULL;
    bool found_inline = false;
    bool found_input = false;
    if (function) {
        for (int i = 0; i < acount(function->nodes); i++) {
            node *candidate = function->nodes[i];
            if (!graph_node_is_inline(function, candidate)) {
                continue;
            }
            found_inline = true;
            for (int j = 0; j < acount(function->nodes); j++) {
                node *consumer = function->nodes[j];
                for (int pin = 0; pin < consumer->input_count; pin++) {
                    node *inline_source = NULL;
                    int inline_pin = -1;
                    if (graph_inline_source(function, consumer, pin, &inline_source, &inline_pin) &&
                        inline_source == candidate) {
                        found_input = true;
                    }
                }
            }
        }
    }
    node *call = extracted ? graph_node_by_id(extracted->graph, call_id) : NULL;
    behavior_instance *instance = extracted
                                    ? behavior_instance_create(extracted, NULL, NULL, NULL)
                                    : NULL;
    float after = 0;
    test_check(
        !error && extracted && function && call_id &&
            !graph_node_by_id(extracted->graph, source_id) &&
            !graph_node_by_id(extracted->graph, target_id) && found_inline &&
            found_input && call && call->input_count == 0 &&
            eval_f32(extracted, call, instance, NULL, 0, &after, message, sizeof message) == 0 &&
            fabsf(after - before) < 0.0001f,
        "function extraction keeps an inline source inside the copied graph"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(extracted);
    behavior_asset_free(asset);
}

static node *find_function_node(graph *g, const node_desc *desc, uint64_t reference_id)
{
    for (int i = 0; i < acount(g->nodes); i++) {
        node *n = g->nodes[i];
        if (n->desc == desc && (!reference_id || n->reference_id == reference_id)) {
            return n;
        }
    }
    return NULL;
}

static void extraction_preserves_bindings()
{
    behavior_asset *asset = behavior_asset_new("PureBindingExtraction");
    behavior_variable *variable = behavior_asset_add_variable(
        asset,
        "Input",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 6 },
        NULL
    );
    node *selected = typed_reroute(asset->graph, PIN_F32, "float");
    node *sink = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            asset->graph,
            selected,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE,
                            .variable_id = variable->id }
        ) == 0
    );
    assert(graph_connect_data(asset->graph, selected, 0, sink, 0) == 0);
    node *selection[] = { selected };
    behavior_asset *extracted = NULL;
    uint64_t call_id = 0;
    const char *error = behavior_asset_extract_function(
        asset,
        asset->graph,
        selection,
        1,
        "BoundValue",
        &extracted,
        &call_id
    );
    assert(!error && extracted);
    graph *function = behavior_asset_find_graph_by_name(extracted, "BoundValue");
    node *call = graph_node_by_id(extracted->graph, call_id);
    node *extracted_sink = graph_node_by_id(extracted->graph, sink->id);
    assert(function && call && extracted_sink);
    const pin_binding *call_binding = graph_pin_binding(extracted->graph, call, 0, 0);
    node *copy = function->nodes[2];
    test_check(
        call_binding && call_binding->variable_id == variable->id &&
            graph_pin_binding(function, copy, 0, 0) == NULL,
        "pure extraction moves an active binding to the replacement call input"
    );
    behavior_instance *instance = instance_for(extracted, NULL, (ecs_entity){ 0 });
    float result = 0;
    char message[192];
    test_check(
        eval_f32(extracted, extracted_sink, instance, NULL, 0, &result, message, sizeof message) ==
                0 &&
            fabsf(result - 6) < .0001f,
        "a pure extracted function still evaluates its bound input"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(extracted);
    behavior_asset_free(asset);

    asset = behavior_asset_new("DormantBindingExtraction");
    variable = behavior_asset_add_variable(
        asset,
        "Fallback",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 8 },
        NULL
    );
    node *producer = typed_reroute(asset->graph, PIN_F32, "float");
    selected = typed_reroute(asset->graph, PIN_F32, "float");
    sink = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_default(
            asset->graph,
            producer,
            0,
            0,
            &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 3 }
        ) == 0
    );
    assert(
        graph_set_pin_binding(
            asset->graph,
            selected,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE,
                            .variable_id = variable->id }
        ) == 0
    );
    assert(graph_connect_data(asset->graph, producer, 0, selected, 0) == 0);
    assert(graph_connect_data(asset->graph, selected, 0, sink, 0) == 0);
    selection[0] = selected;
    extracted = NULL;
    call_id = 0;
    error = behavior_asset_extract_function(asset, asset->graph, selection, 1, "DormantValue", &extracted, &call_id);
    assert(!error && extracted);
    call = graph_node_by_id(extracted->graph, call_id);
    extracted_sink = graph_node_by_id(extracted->graph, sink->id);
    function = behavior_asset_find_graph_by_name(extracted, "DormantValue");
    assert(call && extracted_sink && function);
    copy = function->nodes[2];
    test_check(
        graph_pin_binding(extracted->graph, call, 0, 0) &&
            graph_pin_binding(function, copy, 0, 0) == NULL,
        "pure extraction moves a crossing wire's dormant binding to the call"
    );
    node *producer_copy = graph_node_by_id(extracted->graph, producer->id);
    assert(
        producer_copy &&
        graph_remove_edge(extracted->graph, producer_copy, 0, call, 0) == 0
    );
    instance = instance_for(extracted, NULL, (ecs_entity){ 0 });
    test_check(
        eval_f32(extracted, extracted_sink, instance, NULL, 0, &result, message, sizeof message) ==
                0 &&
            fabsf(result - 8) < .0001f,
        "disconnecting an extracted crossing wire restores its dormant binding"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(extracted);
    behavior_asset_free(asset);

    asset = behavior_asset_new("BindingFanout");
    behavior_variable *left_variable = behavior_asset_add_variable(
        asset,
        "Left",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 1 },
        NULL
    );
    behavior_variable *right_variable = behavior_asset_add_variable(
        asset,
        "Right",
        PIN_F32,
        "float",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_F32, .type_name = "float", .f = 2 },
        NULL
    );
    assert(left_variable && right_variable);
    uint64_t left_id = left_variable->id;
    uint64_t right_id = right_variable->id;
    producer = typed_reroute(asset->graph, PIN_F32, "float");
    node *first = typed_reroute(asset->graph, PIN_F32, "float");
    node *second = typed_reroute(asset->graph, PIN_F32, "float");
    sink = typed_reroute(asset->graph, PIN_F32, "float");
    node *sink2 = typed_reroute(asset->graph, PIN_F32, "float");
    assert(
        graph_set_pin_binding(
            asset->graph,
            first,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = left_id }
        ) == 0
    );
    assert(
        graph_set_pin_binding(
            asset->graph,
            second,
            0,
            0,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = right_id }
        ) == 0
    );
    assert(graph_connect_data(asset->graph, producer, 0, first, 0) == 0);
    assert(graph_connect_data(asset->graph, producer, 0, second, 0) == 0);
    assert(graph_connect_data(asset->graph, first, 0, sink, 0) == 0);
    assert(graph_connect_data(asset->graph, second, 0, sink2, 0) == 0);
    node *fanout[] = { first, second };
    char *before = behavior_asset_save(asset);
    extracted = NULL;
    error = behavior_asset_extract_function(asset, asset->graph, fanout, 2, "AmbiguousFanout", &extracted, &call_id);
    char *after = behavior_asset_save(asset);
    test_check(error && !extracted && before && after && strcmp(before, after) == 0, "pure extraction rejects fanout consumers with different dormant bindings");
    free(before);
    free(after);
    behavior_asset_free(extracted);
    behavior_asset_free(asset);
}

static void executable_extraction_preserves_timing()
{
    behavior_asset *asset = behavior_asset_new("ExecutableBindingExtraction");
    behavior_variable *written_variable = behavior_asset_add_variable(
        asset,
        "Written",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    behavior_variable *observed_variable = behavior_asset_add_variable(
        asset,
        "Observed",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    behavior_variable *tail_variable = behavior_asset_add_variable(
        asset,
        "Tail",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 0 },
        NULL
    );
    assert(written_variable && observed_variable && tail_variable);
    uint64_t written_id = written_variable->id;
    uint64_t observed_id = observed_variable->id;
    uint64_t tail_id = tail_variable->id;
    node *event = graph_add_node(asset->graph, &NODE_Event);
    node *write = behavior_asset_add_set_node(asset, written_id);
    node *read = behavior_asset_add_set_node(asset, observed_id);
    node *after = behavior_asset_add_set_node(asset, tail_id);
    assert(event && write && read && after);
    assert(graph_set_event_name(asset->graph, event, "Go") == 0);
    assert(
        graph_set_pin_default(
            asset->graph,
            write,
            0,
            1,
            &(pin_value){ .type = PIN_I32, .type_name = "int", .i = 7 }
        ) == 0
    );
    assert(
        graph_set_pin_binding(
            asset->graph,
            read,
            0,
            1,
            &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = written_id }
        ) == 0
    );
    assert(graph_connect_exec(asset->graph, event, 0, write, 0, 0) == 0);
    assert(graph_connect_exec(asset->graph, write, 0, read, 0, 0) == 0);
    assert(graph_connect_exec(asset->graph, read, 0, after, 0, 0) == 0);
    node *selection[] = { write, read };
    behavior_asset *extracted = NULL;
    uint64_t call_id = 0;
    const char *error = behavior_asset_extract_function(
        asset,
        asset->graph,
        selection,
        2,
        "WriteThenRead",
        &extracted,
        &call_id
    );
    assert(!error && extracted);
    graph *function = behavior_asset_find_graph_by_name(extracted, "WriteThenRead");
    node *call = graph_node_by_id(extracted->graph, call_id);
    node *copy = find_function_node(function, &NODE_SetVariable, observed_id);
    assert(function && call && copy);
    const pin_binding *binding = graph_pin_binding(function, copy, 0, 1);
    bool call_has_binding = false;
    for (int i = 0; i < call->input_count; i++) {
        call_has_binding |= graph_pin_binding(extracted->graph, call, 0, i) != NULL;
    }
    test_check(
        binding && binding->kind == PIN_BINDING_VARIABLE &&
            binding->variable_id == written_id && !call_has_binding,
        "executable extraction retains bound reads inside the copied execution "
        "chain"
    );

    behavior_instance *instance = instance_for(extracted, NULL, (ecs_entity){ 0 });
    flow_run *run = dispatch(instance, "Go");
    pin_value value;
    test_check(
        flow_run_status(run) == FLOW_COMPLETED &&
            behavior_instance_get_variable(instance, observed_id, &value) == 0 &&
            value.i == 7,
        "executable extraction preserves same-activation write then bound read "
        "timing"
    );
    behavior_instance_destroy(instance);
    behavior_asset_free(extracted);
    behavior_asset_free(asset);
}

static void compiled_numeric_variable_binding()
{
    for (int wide = 0; wide < 2; wide++) {
        behavior_asset *asset = behavior_asset_new("NumericBinding");
        pin_value initial = wide ? (pin_value){ .type = PIN_F64,
                                                .type_name = "double",
                                                .d = 2.5 }
                                 : (pin_value){ .type = PIN_F32,
                                                .type_name = "float",
                                                .f = 2.5f };
        uint64_t source = behavior_asset_add_variable(
                              asset,
                              "Source",
                              initial.type,
                              initial.type_name,
                              PIN_CONTAINER_SINGLE,
                              &initial,
                              NULL
        )
                              ->id;
        uint64_t result = behavior_asset_add_variable(
                              asset,
                              "Result",
                              wide ? PIN_F32 : PIN_F64,
                              wide ? "float" : "double",
                              PIN_CONTAINER_SINGLE,
                              NULL,
                              NULL
        )
                              ->id;
        node *event = graph_add_node(asset->graph, &NODE_Event);
        node *set = behavior_asset_add_set_node(asset, result);
        assert(graph_set_event_name(asset->graph, event, "Go") == 0);
        assert(graph_connect_exec(asset->graph, event, 0, set, 0, 0) == 0);
        assert(
            graph_set_pin_binding(
                asset->graph,
                set,
                0,
                1,
                &(pin_binding){ .kind = PIN_BINDING_VARIABLE, .variable_id = source }
            ) == 0
        );
        behavior_instance *instance = instance_for(asset, NULL, (ecs_entity){ 0 });
        behavior_instance_set_trace_enabled(instance, false);
        flow_run *run = dispatch(instance, "Go");
        pin_value value;
        test_check(
            flow_run_status(run) == FLOW_COMPLETED && run->whole_function_executed &&
                behavior_instance_get_variable(instance, result, &value) == 0 &&
                (wide ? value.f : value.d) == 2.5,
            "compiled variable bindings convert float and double using the "
            "source storage type"
        );
        behavior_instance_destroy(instance);
        behavior_asset_free(asset);
    }
}

static void numeric_output_to_text()
{
    behavior_asset *asset = behavior_asset_new("NumericOutputToText");
    editor e = { .asset = asset, .active_graph = asset->graph };
    node *add = graph_add_node(asset->graph, registry_node_by_name("Add"));
    node *number = graph_add_node(asset->graph, registry_node_by_name("To Integer"));
    node *text = graph_add_node(asset->graph, registry_node_by_name("To String"));
    assert(graph_connect_data(asset->graph, add, 0, number, 0) == 0);
    ed_pin_view output = { add, ED_PIN_OUTPUT, 0 };
    ed_pin_view input = { text, ED_PIN_INPUT, 0 };
    test_check(
        ed_connect(&e, &output, &input) && add->output_pins[0].type == PIN_F64 &&
            add->input_pins[0].type == PIN_F64 &&
            graph_input_conversion(asset->graph, text, 0) ==
                registry_node_by_name("To String"),
        "displaying arithmetic as text preserves its existing numeric "
        "connections"
    );
    behavior_asset_free(asset);
}

void test_input_bindings()
{
    compiled_numeric_variable_binding();
    numeric_output_to_text();
    variable_binding_and_precedence();
    flow_timing_and_delta();
    event_context_sources();
    executable_event_binding_extraction();
    component_and_context_bindings();
    converted_component_field_binding();
    named_disk_references();
    serialization_and_clipboard_ownership();
    inline_source_graphs();
    component_getter_optimization();
    component_getter_auto_simplification();
    inline_source_extraction();
    extraction_preserves_bindings();
    executable_extraction_preserves_timing();
    puts("PASS: input bindings resolve variables, components and context across evaluation, flow, persistence and extraction");
}
