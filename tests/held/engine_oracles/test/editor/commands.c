#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/collab/protocol.h"
#include "editor/graph/search.h"
#include "editor/studio/studio_members_internal.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/events.h"
#include "graph/document/library.h"
#include "graph/nodes/component_nodes.h"
#include "types/array.h"
#include "graph/shader/shader_graph.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"

static pin_spec g_editor_float_outputs[] = {
    { "Value", PIN_F32, "float", PIN_DATA },
};
static pin_spec g_editor_float_inputs[] = {
    { "Value", PIN_F32, "float", PIN_DATA },
};
static pin_spec g_editor_int_inputs[] = {
    { "Value", PIN_I32, "int", PIN_DATA },
    { "Condition", PIN_BOOL, "bool", PIN_DATA },
};
static pin_spec g_editor_name_inputs[] = {
    { "Name", PIN_NAME, "name", PIN_DATA },
};
static node_desc g_editor_float_sink_desc = {
    .name = "editor_float_sink",
    .input_count = 1,
    .inputs = g_editor_float_inputs,
};
static node_desc g_editor_float_desc = {
    .name = "editor_float_source",
    .output_count = 1,
    .outputs = g_editor_float_outputs,
};
static node_desc g_editor_float_relay_desc = {
    .name = "editor_float_relay",
    .input_count = 1,
    .output_count = 1,
    .inputs = g_editor_float_inputs,
    .outputs = g_editor_float_outputs,
};
static node_desc g_editor_name_relay_desc = {
    .name = "editor_name_relay",
    .input_count = 1,
    .output_count = 1,
    .inputs = g_editor_name_inputs,
    .outputs = g_editor_float_outputs,
};
static node_desc g_editor_int_desc = {
    .name = "editor_int_sink",
    .input_count = 2,
    .inputs = g_editor_int_inputs,
};

typedef struct
{
    int published;
} streamed_literal_fixture;

static void streamed_literal_send(void *context, const char *json)
{
    streamed_literal_fixture *f = context;
    collab_message message = { 0 };
    assert(collab_message_read(json, &message));
    if (!strcmp(message.kind, "accepted")) {
        f->published++;
    }
    collab_message_free(&message);
}

static bool streamed_literal_apply(void *, const char *, char *, size_t)
{
    return true;
}

static char *streamed_literal_checkpoint(void *)
{
    return xstrdup("{}");
}

static bool streamed_literal_restore(void *, const char *, char *, size_t)
{
    return true;
}

static void streamed_literal_receive(
    const char *kind,
    const char *client,
    uint64_t seq,
    uint64_t revision,
    const char *payload
)
{
    char *json = collab_message_write("streamed-literal", client, seq, revision, kind, payload);
    assert(json);
    collab_receive(json);
    free(json);
    collab_tick();
}

/* Every host preview is published, while a streamed value's undo state is
   captured only once for the gesture. */
static void check_streamed_literal_undo()
{
    collab_shutdown();
    streamed_literal_fixture f = { 0 };
    collab_init((collab_config){ .context = &f,
                                 .send = streamed_literal_send,
                                 .apply = streamed_literal_apply,
                                 .checkpoint = streamed_literal_checkpoint,
                                 .restore = streamed_literal_restore });
    collab_start("streamed-literal", "self", "host");
    collab_seed("{\"checkpoint_version\":1,\"workspace\":\"{}\",\"project_root\":\"/tmp\"}");
    collab_tick();
    streamed_literal_receive("joined", "self", 0, 0, "{\"role\":\"host\",\"next_seq\":1,\"snapshot_revision\":null}");
    streamed_literal_receive("snapshot_ack", "server", 0, 0, "{}");
    assert(collab_can_edit());

    behavior_asset *asset = behavior_asset_new("Streamed literal undo");
    node *source = graph_add_node(asset->graph, registry_node_by_name("Real"));
    assert(source && source->output_count == 1);
    uint64_t source_id = source->id;
    pin_value initial = { .type = PIN_F64, .d = 0.25 };
    assert(graph_set_pin_default(asset->graph, source, 1, 0, &initial) == 0);
    editor *e = editor_new(asset);
    pin_value preview = { .type = PIN_F64, .d = 1.0 };
    assert(
        editor_command_apply(
            e,
            (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                              .data.set_pin_default = { source->id, 1, 0, &preview, false } }
        ) == 0
    );
    assert(acount(e->undo) == 1 && f.published == 1);
    streamed_literal_receive("accepted", "self", 1, 1, "{}");

    preview.d = 2.0;
    assert(
        editor_command_apply(
            e,
            (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                              .data.set_pin_default = { .node_id = source->id,
                                                        .output = 1,
                                                        .pin_index = 0,
                                                        .value = &preview,
                                                        .coalesce_history = true } }
        ) == 0
    );
    assert(acount(e->undo) == 1 && f.published == 2);
    streamed_literal_receive("accepted", "self", 2, 2, "{}");

    ed_undo(e);
    const graph *restored = editor_active_graph(e);
    node *restored_source = graph_node_by_id(restored, source_id);
    test_check(
        restored_source &&
            restored_source->output_pins[0].default_value.d == initial.d,
        "one Undo restores the pre-gesture literal after all previews were "
        "published"
    );
    ed_redo(e);
    const graph *redone = editor_active_graph(e);
    node *redone_source = graph_node_by_id(redone, source_id);
    test_check(
        redone_source &&
            redone_source->output_pins[0].default_value.d == preview.d,
        "Redo restores the final streamed literal value"
    );

    preview.d = 3.0;
    assert(
        editor_command_apply(
            e,
            (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                              .data.set_pin_default = { .node_id = source_id,
                                                        .output = 1,
                                                        .pin_index = 0,
                                                        .value = &preview } }
        ) == 0
    );
    assert(acount(e->undo) == 2 && f.published == 3);
    streamed_literal_receive("accepted", "self", 3, 3, "{}");
    ed_undo(e);
    const graph *after_separate_undo = editor_active_graph(e);
    node *after_separate_undo_source = graph_node_by_id(after_separate_undo, source_id);
    test_check(
        after_separate_undo_source &&
            after_separate_undo_source->output_pins[0].default_value.d == 2.0,
        "a later gesture keeps its own undo step"
    );
    ed_undo(e);
    const graph *after_group_undo = editor_active_graph(e);
    node *after_group_undo_source = graph_node_by_id(after_group_undo, source_id);
    test_check(
        after_group_undo_source &&
            after_group_undo_source->output_pins[0].default_value.d == initial.d,
        "Undo then crosses the earlier streamed gesture as one step"
    );
    editor_free(e);
    collab_shutdown();
}

/* Inserts the top search result, as choosing the first command result does. */
static void search_insert(editor *e, const char *query)
{
    ed_search_match *matches = ed_search_matches(e, query);
    assert(acount(matches));
    ed_spawn_match_at(e, &matches[0], 0, 0);
    afree(matches);
}

static void check_linked_preview()
{
    behavior_asset *library = behavior_asset_new("PreviewMath");
    library->domain = BEHAVIOR_DOMAIN_LIBRARY;
    graph *identity = behavior_asset_add_function(library, "Identity", 1);
    pin_spec value = { .name = "Value", .type = PIN_F64, .type_name = "double", .kind = PIN_DATA };
    node *entry = graph_node_by_id(identity, identity->entry_id);
    node *ret = graph_node_by_id(identity, identity->return_id);
    assert(graph_add_pin(identity, entry, 1, &value) == 0);
    assert(graph_add_pin(identity, ret, 0, &value) == 0);
    assert(graph_connect_data(identity, entry, 0, ret, 0) == 0);

    behavior_asset *asset = behavior_asset_new("Linked preview");
    node *number = graph_add_node(asset->graph, registry_node_by_name("Real"));
    assert(
        graph_set_pin_default(asset->graph, number, 0, 0, &(pin_value){ .type = PIN_F64, .d = 7 }) == 0
    );
    node *call = behavior_asset_add_function_call_node_to_graph(asset, asset->graph, identity);
    assert(call->reference_asset);
    assert(graph_connect_data(asset->graph, number, 0, call, 0) == 0);
    const behavior_asset *catalog[] = { library };
    eval_view *view = eval_view_new();
    eval_view_set_catalog(view, catalog, 1);
    eval_view_refresh(view, asset->graph);
    test_check(!eval_view_error(view)[0], "preview links authored library references before compilation");
    test_check(!strcmp(eval_view_output(view, number, 0), "7"), "linked preview maps values back to authored nodes");

    assert(
        graph_set_pin_default(asset->graph, number, 0, 0, &(pin_value){ .type = PIN_F64, .d = 11 }) == 0
    );
    eval_view_refresh(view, asset->graph);
    test_check(
        !eval_view_error(view)[0] && !strcmp(eval_view_output(view, number, 0), "11"),
        "preview replaces its linked generation after graph edits"
    );
    eval_view_set_catalog(view, NULL, 0);
    eval_view_refresh(view, asset->graph);
    test_check(
        strstr(eval_view_error(view), "PreviewMath") != NULL,
        "preview reports a missing library instead of hiding compilation errors"
    );
    eval_view_set_catalog(view, catalog, 1);
    eval_view_refresh(view, asset->graph);
    test_check(
        !eval_view_error(view)[0] && !strcmp(eval_view_output(view, number, 0), "11"),
        "preview recovers when its library returns to the catalog"
    );
    eval_view_free(view);
    behavior_asset_free(asset);
    behavior_asset_free(library);
}

/* A behavior compiles with copies of the libraries it calls. A problem in one
   names the library's graph and node, so Problems can open it there. */
static void check_library_problem_source()
{
    behavior_asset *library = behavior_asset_new("ProblemMath");
    library->domain = BEHAVIOR_DOMAIN_LIBRARY;
    graph *value = behavior_asset_add_function(library, "Value", 1);
    node *muted = graph_add_node(value, registry_node_by_name("Real"));
    assert(muted);
    graph_set_node_disabled(value, muted, true);
    behavior_asset *asset = behavior_asset_new("Library problems");
    assert(behavior_asset_add_function_call_node(asset, value));
    editor *e = editor_new(asset);
    const behavior_asset *catalog[] = { library };
    editor_set_asset_catalog(e, catalog, 1);
    assert(editor_command_apply(e, (editor_command){ .kind = EDITOR_COMMAND_COMPILE }) == 0);
    graph_node_source source = { 0 };
    for (int i = 0; i < acount(e->compile.items); i++) {
        const compile_diagnostic *d = &e->compile.items[i];
        if (d->rule == CMPL_RULE_DISABLED) {
            source = graph_node_authored(behavior_asset_find_graph(e->compiled, d->graph_id), d->node_id);
        }
    }
    test_check(
        source.asset && !strcmp(source.asset, "ProblemMath") &&
            source.graph_id == value->graph_id && source.node_id == muted->id,
        "a problem in a called library names the library's graph and node"
    );
    editor_free(e);
    behavior_asset_free(library);
}

/* A node dropped on a wire joins it in one step, and one that can't take
   the wire's value leaves the wire as it was. */
static void check_insert_on_wire()
{
    behavior_asset *asset = behavior_asset_new("Insert on wire");
    editor *e = editor_new(asset);
    graph *g = asset->graph;
    node *source = graph_add_node(g, &g_editor_float_desc);
    node *sink = graph_add_node(g, &g_editor_float_sink_desc);
    node *relay = graph_add_node(g, &g_editor_float_relay_desc);
    node *named = graph_add_node(g, &g_editor_name_relay_desc);
    assert(e && graph_connect_data(g, source, 0, sink, 0) == 0);
    node *into_sink = NULL, *into_relay = NULL;
    int pin = -1;
    bool refused = !ed_insert_on_wire(e, 0, named, 0, 0);
    test_check(
        refused && acount(g->edges) == 1 &&
            graph_input_source(g, sink, 0, &into_sink, &pin) && into_sink == source,
        "a node that can't take a wire's value leaves the wire unchanged"
    );
    bool inserted = ed_insert_on_wire(e, 0, relay, 0, 0);
    test_check(
        inserted && acount(g->edges) == 2 &&
            graph_input_source(g, relay, 0, &into_relay, &pin) && into_relay == source &&
            graph_input_source(g, sink, 0, &into_sink, &pin) && into_sink == relay,
        "inserting a node on a wire routes the wire through it"
    );
    editor_free(e);
}

/* Wires shared into two new Adds; the first also folds its own Real. */
static node *add_shared_consumers(graph *g, node *shared, node **second)
{
    node *folded = graph_add_node(g, registry_node_by_name("Real"));
    node *first = graph_add_node(g, registry_node_by_name("Add"));
    *second = graph_add_node(g, registry_node_by_name("Add"));
    assert(graph_connect_data(g, shared, 0, first, 0) == 0);
    assert(graph_connect_data(g, shared, 0, *second, 0) == 0);
    assert(graph_connect_data(g, folded, 0, first, 1) == 0);
    return first;
}

static void check_delete_shared_source()
{
    behavior_asset *asset = behavior_asset_new("Delete shared source");
    graph *g = asset->graph;
    node *shared = graph_add_node(g, registry_node_by_name("Real"));
    editor *e = editor_new(asset);
    node *second = NULL;
    node *first = add_shared_consumers(g, shared, &second);
    ed_select_node(e, first, false);
    ed_select_node(e, second, true);
    ed_delete_selected(e);
    test_check(acount(g->nodes) == 1 && g->nodes[0] == shared, "deleting consumers keeps the source they shared");

    first = add_shared_consumers(g, shared, &second);
    ed_link_ref link = { shared->id,
                         shared->output_pins[0].id,
                         second->id,
                         second->input_pins[0].id };
    ed_select_node(e, first, false);
    ed_add_selected_link(e, &link);
    ed_delete_selected(e);
    test_check(
        acount(g->nodes) == 2 && g->nodes[0] == shared && g->nodes[1] == second,
        "deleting a consumer and a wire keeps the source they shared"
    );
    editor_free(e);
}

static void check_search_catalog()
{
    behavior_asset *asset = behavior_asset_new("Search catalog");
    graph *function = behavior_asset_add_function(asset, "ComputeVelocity", 1);
    graph *macro = behavior_asset_add_macro(asset, "RepeatStep");
    behavior_variable *variable = behavior_asset_add_variable(
        asset,
        "TrailBudget",
        PIN_F64,
        NULL,
        PIN_CONTAINER_SINGLE,
        NULL,
        NULL
    );
    behavior_asset *library = behavior_asset_new("MotionLibrary");
    library->domain = BEHAVIOR_DOMAIN_LIBRARY;
    graph *export = behavior_asset_add_function(library, "LimitSpeed", 1);
    behavior_asset *event_definition = behavior_asset_new("Impact");
    event_definition->domain = BEHAVIOR_DOMAIN_EVENT_DEFINITION;
    behavior_asset_add_variable(event_definition, "Strength", PIN_F64, NULL, PIN_CONTAINER_SINGLE, NULL, NULL);
    editor *e = editor_new(asset);
    const behavior_asset *assets[] = { library, event_definition };
    const char *prototypes[] = { "TrailParticle" };
    editor_set_asset_catalog(e, assets, 2);
    editor_set_prototype_catalog(e, prototypes, 1);
    const char *queries[] = { "Compute Velocity",
                              "Repeat Step",
                              "Get Trail Budget",
                              "Set Trail Budget",
                              "MotionLibrary / LimitSpeed",
                              "Spawn TrailParticle",
                              "On Impact",
                              "Call Impact",
                              "Send Impact" };
    const char *fuzzy_queries[] = { "velocity compute",
                                    "CV",
                                    "compute velcoity",
                                    "budget trail get",
                                    "math formula" };
    const char *fuzzy_names[] = { "Compute Velocity",
                                  "Compute Velocity",
                                  "Compute Velocity",
                                  "Get Trail Budget",
                                  "Behavior Expression" };
    for (int i = 0; i < 5; i++) {
        ed_search_match *matches = ed_search_matches(e, fuzzy_queries[i]);
        test_check(acount(matches) > 0 && !strcmp(matches[0].name, fuzzy_names[i]), "ranked node search handles reordered words, initials and transposed letters");
        afree(matches);
    }
    node *expected[9] = { 0 };
    for (int i = 0; i < 9; i++) {
        ed_search_match *matches = ed_search_matches(e, queries[i]);
        test_check(
            acount(matches) > 0 && strcmp(matches[0].name, queries[i]) == 0,
            "search ranks an exact authored member ahead of fuzzy node matches"
        );
        node *n = acount(matches) ? ed_spawn_match_at(e, &matches[0], 0, 0) : NULL;
        expected[i] = n;
        test_check(n != NULL, "search results create configured authored nodes");
        if (n && i < 2) {
            test_check(
                n->reference_id == (i ? macro->graph_id : function->graph_id),
                "local search calls preserve graph identity"
            );
        }
        if (n && (i == 2 || i == 3)) {
            test_check(n->reference_id == variable->id, "variable search binds the chosen variable");
        }
        if (n && i == 4) {
            test_check(
                n->reference_id == export->graph_id && n->reference_asset &&
                    !strcmp(n->reference_asset, library->name),
                "library search binds the library function"
            );
        }
        if (n && i == 5) {
            test_check(
                n->input_pins[1].default_value.str &&
                    !strcmp(n->input_pins[1].default_value.str, prototypes[0]),
                "prototype search preselects the spawn asset"
            );
        }
        if (n && i >= 6) {
            test_check(graph_event_matches(n, event_definition), "event search binds the typed payload signature");
        }
        afree(matches);
    }
    for (int i = 0; i < 9; i++) {
        int count = acount(asset->graph->nodes);
        search_insert(e, queries[i]);
        node *inserted = acount(asset->graph->nodes) == count + 1
                           ? asset->graph->nodes[count]
                           : NULL;
        test_check(
            inserted && expected[i] && inserted->desc == expected[i]->desc &&
                inserted->reference_id == expected[i]->reference_id,
            "choosing an authored member from node search inserts the selected "
            "symbol once"
        );
    }
    int comment_count = acount(asset->graph->comment_boxes);
    search_insert(e, "Comment Box");
    test_check(acount(asset->graph->comment_boxes) == comment_count + 1, "choosing Comment Box from node search inserts a comment box from the standard results");
    node *event = graph_add_node(asset->graph, &NODE_Event);
    ed_snapshot_before(e);
    e->spawn_link_pending = true;
    e->spawn_link_pin = (ed_pin_view){ event, ED_PIN_OUTPUT, 0 };
    ed_search_match *pure_matches = ed_search_matches(e, "Compute Velocity");
    bool has_pure = false;
    for (int i = 0; i < acount(pure_matches); i++) {
        has_pure |= pure_matches[i].function == function;
    }
    test_check(!has_pure, "exec pin search excludes a pure function with no exec input");
    afree(pure_matches);
    int edge_count = acount(asset->graph->edges);
    search_insert(e, "Repeat Step");
    test_check(acount(asset->graph->edges) == edge_count + 1, "pin-drag search finds macros and connects the compatible execution input");
    const game_event_field input_params[] = { { .name = "Move", .type = PIN_F64 } };
    const game_host_event_desc host_events[] = {
        { .name = "Input", .payload = input_params, .payload_count = 1 },
    };
    editor_set_event_catalog(e, host_events, 1);
    int host_count = acount(asset->graph->nodes);
    search_insert(e, "On Input");
    node *input = acount(asset->graph->nodes) == host_count + 1
                    ? asset->graph->nodes[host_count]
                    : NULL;
    test_check(
        input && input->desc == &NODE_Event && input->reference_name &&
            !strcmp(input->reference_name, "Input") && input->output_count == 2 &&
            !strcmp(input->output_pins[1].name, "Move"),
        "node search inserts named engine events with the declared payload"
    );
    behavior_asset *shader_library = behavior_asset_new("ShaderMath");
    shader_library->domain = BEHAVIOR_DOMAIN_SHADER_LIBRARY;
    graph *pulse = behavior_asset_add_function(shader_library, "Pulse", 1);
    asset->domain = BEHAVIOR_DOMAIN_MATERIAL;
    const behavior_asset *shader_assets[] = { shader_library };
    editor_set_asset_catalog(e, shader_assets, 1);
    ed_search_match *shader_matches = ed_search_matches(e, "ShaderMath / Pulse");
    node *shader_call = acount(shader_matches)
                          ? ed_spawn_match_at(e, &shader_matches[0], 0, 0)
                          : NULL;
    test_check(
        shader_call && shader_call->desc == &NODE_CallPureFunction &&
            shader_call->reference_id == pulse->graph_id && shader_call->reference_asset &&
            !strcmp(shader_call->reference_asset, "ShaderMath"),
        "shader search finds and inserts pure library functions by their "
        "qualified names"
    );
    afree(shader_matches);
    behavior_asset_free(shader_library);
    behavior_asset *fields = behavior_asset_new("ScalarFields");
    fields->domain = BEHAVIOR_DOMAIN_LIBRARY;
    graph *disc = behavior_asset_add_function(fields, "Disc", true);
    node *disc_return = graph_node_by_id(disc, disc->return_id);
    graph_add_pin(disc, disc_return, 0, &(pin_spec){ .name = "Value", .type = PIN_F32 });
    graph_add_node(disc, registry_node_by_name("Field Circle"));
    asset->domain = BEHAVIOR_DOMAIN_FIELD;
    const behavior_asset *field_assets[] = { fields };
    editor_set_asset_catalog(e, field_assets, 1);
    ed_search_match *field_matches = ed_search_matches(e, "ScalarFields / Disc");
    node *field_call = acount(field_matches)
                         ? ed_spawn_match_at(e, &field_matches[0], 0, 0)
                         : NULL;
    test_check(
        field_call && field_call->desc == &NODE_CallPureFunction &&
            field_call->reference_asset &&
            !strcmp(field_call->reference_asset, "ScalarFields"),
        "field search inserts pure scalar library functions"
    );
    afree(field_matches);
    field_matches = ed_search_matches(e, "Print String");
    test_check(!acount(field_matches), "field search excludes host effects");
    afree(field_matches);
    behavior_asset_free(fields);
    editor_free(e);
    behavior_asset_free(library);
    behavior_asset_free(event_definition);
}

static void check_library_search_snapshot()
{
    behavior_asset *catalog = behavior_asset_new("LibrarySnapshot");
    catalog->domain = BEHAVIOR_DOMAIN_LIBRARY;
    behavior_asset_add_function(catalog, "CurrentFunction", 1);
    behavior_asset_add_function(catalog, "SiblingFunction", 1);
    char *json = behavior_asset_save(catalog);
    behavior_asset *snapshot = behavior_asset_load(json);
    free(json);
    test_check(snapshot != NULL, "library search fixture loads an independent working snapshot");
    if (!snapshot) {
        behavior_asset_free(catalog);
        return;
    }
    editor *e = editor_new(snapshot);
    const behavior_asset *assets[] = { catalog };
    editor_set_asset_catalog(e, assets, 1);
    graph *current = behavior_asset_find_graph_by_name(snapshot, "CurrentFunction");
    graph *sibling = behavior_asset_find_graph_by_name(snapshot, "SiblingFunction");
    editor_open_graph(e, current->graph_id);
    ed_search_match *matches = ed_search_matches(e, "");
    int functions = 0;
    const ed_search_match *local = NULL;
    for (int i = 0; i < acount(matches); i++) {
        if (!matches[i].function) {
            continue;
        }
        functions++;
        if (matches[i].function == sibling) {
            local = &matches[i];
        }
    }
    test_check(functions == 1 && local, "library search excludes the current function and duplicate catalog snapshot functions");
    node *call = local ? ed_spawn_match_at(e, local, 0, 0) : NULL;
    test_check(call && call->reference_id == sibling->graph_id && !call->reference_asset, "library search inserts the sibling from the working document as a local call");
    afree(matches);
    editor_free(e);
    behavior_asset_free(catalog);
}

/* Behavior graphs offer Get Dash as a Get Component preset. */
static bool get_dash(const ed_search_match *match)
{
    return match->desc == &NODE_GetComponent && match->setting &&
           !strcmp(match->setting, "GAME.GameDash") && !match->field;
}

static void check_typed_wire_search()
{
    behavior_asset *asset = behavior_asset_new("Typed wire search");
    editor *e = editor_new(asset);
    node *target = asset ? graph_add_node(asset->graph, &g_editor_float_sink_desc)
                         : NULL;
    const node_desc *dash = registry_node_by_name("Get_GameDash");
    bool behavior_match = false;
    bool system_match = false;
    if (e && target && dash) {
        e->spawn_link_pending = true;
        e->spawn_link_pin = (ed_pin_view){ target, ED_PIN_INPUT, 0 };
        ed_search_match *matches = ed_search_matches(e, "Get Dash");
        for (int i = 0; i < acount(matches); i++) {
            behavior_match |= get_dash(&matches[i]);
        }
        afree(matches);
        editor_set_ui_context(e, EDITOR_UI_CONTEXT_SYSTEM);
        matches = ed_search_matches(e, "Get Dash");
        for (int i = 0; i < acount(matches); i++) {
            system_match |= matches[i].desc == dash;
        }
        afree(matches);
    }
    test_check(behavior_match, "a numeric input suggests Get Dash in the behavior graph");
    test_check(system_match, "a numeric input suggests Get Dash in the system graph");

    bool connected = false;
    if (e && target && dash) {
        editor_set_ui_context(e, EDITOR_UI_CONTEXT_BEHAVIOR);
        asset->graph->dataflow = false;
        e->spawn_link_pending = true;
        e->spawn_link_pin = (ed_pin_view){ target, ED_PIN_INPUT, 0 };
        uint64_t expected_id = asset->graph->id_counter;
        search_insert(e, "Get Dash");
        const pin_binding *binding = graph_pin_binding(asset->graph, target, 0, 0);
        connected = binding && binding->kind == PIN_BINDING_COMPONENT_FIELD &&
                    !graph_node_by_id(asset->graph, expected_id) &&
                    binding->component_key &&
                    !strcmp(binding->component_key, "GAME.GameDash") &&
                    binding->field_name && !strcmp(binding->field_name, "speed");
    }
    test_check(connected, "selecting Get Dash from a numeric input binds a compatible field output");

    behavior_asset *direct_asset = behavior_asset_new("Direct wire replaces inline source");
    editor *direct_editor = editor_new(direct_asset);
    graph *direct = direct_asset->graph;
    node *old_source = graph_add_node(direct, registry_node_by_name("Real"));
    node *new_source = graph_add_node(direct, registry_node_by_name("Real"));
    node *direct_target = graph_add_node(direct, &g_editor_float_sink_desc);
    assert(direct_editor && old_source && new_source && direct_target);
    assert(graph_connect(direct, old_source, 0, direct_target, 0) == 0);
    uint64_t old_source_id = old_source->id;
    bool replaced = graph_node_is_inline(direct, old_source) &&
                    ed_connect(
                        direct_editor,
                        &(ed_pin_view){ new_source, ED_PIN_OUTPUT, 0 },
                        &(ed_pin_view){ direct_target, ED_PIN_INPUT, 0 }
                    );
    node *linked_source = NULL;
    int linked_pin = -1;
    test_check(
        replaced && !graph_node_by_id(direct, old_source_id) &&
            graph_inline_source(direct, direct_target, 0, &linked_source, &linked_pin) &&
            linked_source == new_source,
        "a wire dropped on an input replaces its inline source"
    );
    editor_free(direct_editor);

    behavior_asset *system_asset = behavior_asset_new("System typed wire search");
    editor *system_editor = editor_new(system_asset);
    node *system_target = system_asset
                            ? graph_add_node(system_asset->graph, &g_editor_float_sink_desc)
                            : NULL;
    bool system_connected = false;
    if (system_editor && system_target && dash) {
        editor_set_ui_context(system_editor, EDITOR_UI_CONTEXT_SYSTEM);
        system_editor->spawn_link_pending = true;
        system_editor->spawn_link_pin = (ed_pin_view){ system_target, ED_PIN_INPUT, 0 };
        search_insert(system_editor, "Get Dash");
        node *source = NULL;
        int source_pin = -1;
        system_connected = graph_input_source(system_asset->graph, system_target, 0, &source, &source_pin) &&
                           source && source->desc == dash && source_pin >= 0 &&
                           source_pin < source->output_count &&
                           source->output_pins[source_pin].type == PIN_F32 &&
                           source->output_pins[source_pin].name &&
                           !strcmp(source->output_pins[source_pin].name, "speed");
    }
    test_check(system_connected, "system popup autowires a compatible Get Dash field output");
    editor_free(system_editor);
    editor_free(e);
}

static void check_graph_reconnect_collab_replay()
{
    behavior_asset *asset = behavior_asset_new("Graph reconnect replay");
    editor *e = editor_new(asset);
    node *source = graph_add_node(asset->graph, registry_node_by_name("Sine"));
    node *old_to = graph_add_node(asset->graph, registry_node_by_name("Sine"));
    node *new_to = graph_add_node(asset->graph, registry_node_by_name("Sine"));
    assert(graph_connect_data(asset->graph, source, 0, old_to, 0) == 0);
    /* Undo snapshots round-trip descriptors through the registry. */
    behavior_asset *snapshot = ed_snapshot(e);
    assert(snapshot);
    behavior_asset_free(snapshot);
    uint64_t source_id = source->id, old_id = old_to->id, new_id = new_to->id;
    char *action = graph_collab_action_reconnect(e, source, 0, old_to, 0, new_to, 0);
    char error[256];
    char *message_json = action
                           ? collab_message_write("room", "host", 1, 1, "accepted", action)
                           : NULL;
    collab_message message = { 0 };
    bool transported = message_json && collab_message_read(message_json, &message) &&
                       !strcmp(yyjson_get_str(yyjson_obj_get(message.payload, "op")), "reconnect") &&
                       yyjson_get_uint(yyjson_obj_get(message.payload, "old_node")) ==
                           old_id;
    test_check(transported, "the collaboration protocol transports the original and replacement wire in one accepted message");
    collab_message_free(&message);
    free(message_json);
    int history = acount(e->undo);
    bool moved = action &&
                 graph_collab_apply_action(e, action, error, sizeof error) &&
                 acount(e->active_graph->edges) == 1 &&
                 e->active_graph->edges[0].to_id == new_id &&
                 acount(e->undo) == history + 1;
    bool stale_refused = action &&
                         !graph_collab_apply_action(e, action, error, sizeof error) &&
                         acount(e->undo) == history + 1 &&
                         e->active_graph->edges[0].to_id == new_id;
    ed_undo(e);
    bool restored = acount(e->active_graph->edges) == 1 &&
                    e->active_graph->edges[0].to_id == old_id;
    ed_redo(e);
    bool redone = acount(e->active_graph->edges) == 1 &&
                  e->active_graph->edges[0].to_id == new_id;
    test_check(moved && restored && redone, "collaboration reconnect replaces the old wire in one Undo/Redo transaction");
    test_check(stale_refused, "collaboration rejects a stale reconnect without changing the graph or history");
    free(action);
    source = graph_node_by_id(e->active_graph, source_id);
    new_to = graph_node_by_id(e->active_graph, new_id);
    history = acount(e->undo);
    action = graph_collab_action_reconnect(e, source, 0, new_to, 0, new_to, 0);
    bool same = action && graph_collab_apply_action(e, action, error, sizeof error) &&
                acount(e->undo) == history;
    free(action);
    action = graph_collab_action_reconnect(e, source, 0, new_to, 0, NULL, 0);
    bool disconnected = action &&
                        graph_collab_apply_action(e, action, error, sizeof error) &&
                        !acount(e->active_graph->edges) &&
                        acount(e->undo) == history + 1;
    ed_undo(e);
    bool disconnect_undo = acount(e->active_graph->edges) == 1 &&
                           e->active_graph->edges[0].to_id == new_id;
    test_check(same, "replaying a wire drop on its original input adds no history");
    test_check(disconnected && disconnect_undo, "collaboration replays empty-canvas wire disconnection and Undo restores it");
    free(action);
    source = graph_node_by_id(e->active_graph, source_id);
    new_to = graph_node_by_id(e->active_graph, new_id);
    history = acount(e->undo);
    const char *invalid = "{\"op\":\"reconnect\",\"document\":\"Graph "
                          "reconnect replay\",\"graph\":1,"
                          "\"from_node\":99999,\"from_pin\":0,\"old_node\":"
                          "99998,\"old_pin\":0}";
    bool refused = !graph_collab_apply_action(e, invalid, error, sizeof error) &&
                   !graph_collab_action_reconnect(e, source, -1, new_to, 0, NULL, 0) &&
                   acount(e->active_graph->edges) == 1 && acount(e->undo) == history;
    test_check(refused, "malformed reconnect endpoints are refused before authored state or history changes");
    node *incompatible = graph_add_node(e->active_graph, registry_node_by_name("Branch"));
    bool mismatch = !graph_collab_action_reconnect(e, source, 0, new_to, 0, incompatible, 0) &&
                    !graph_collab_reconnect_apply(e, source, 0, new_to, 0, incompatible, 0) &&
                    acount(e->active_graph->edges) == 1 && acount(e->undo) == history;
    test_check(mismatch, "incompatible reconnect keeps its original wire and history");
    editor_free(e);

    asset = behavior_asset_new("Exec reconnect replay");
    e = editor_new(asset);
    source = graph_add_node(asset->graph, registry_node_by_name("Branch"));
    old_to = graph_add_node(asset->graph, registry_node_by_name("Branch"));
    new_to = graph_add_node(asset->graph, registry_node_by_name("Branch"));
    old_id = old_to->id;
    new_id = new_to->id;
    assert(graph_connect_exec(asset->graph, source, 0, old_to, 0, 0) == 0);
    snapshot = ed_snapshot(e);
    assert(snapshot);
    behavior_asset_free(snapshot);
    action = graph_collab_action_reconnect(e, source, 0, old_to, 0, new_to, 0);
    bool exec_moved = action &&
                      graph_collab_apply_action(e, action, error, sizeof error) &&
                      acount(e->active_graph->edges) == 1 &&
                      e->active_graph->edges[0].to_id == new_id;
    ed_undo(e);
    bool exec_restored = acount(e->active_graph->edges) == 1 &&
                         e->active_graph->edges[0].to_id == old_id;
    test_check(exec_moved && exec_restored, "collaboration reconnect preserves exec output exclusivity and atomic Undo");
    free(action);
    editor_free(e);
}

static void check_graph_binding_collab_replay()
{
    behavior_asset *asset = behavior_asset_new("Graph typed binding replay");
    editor *e = editor_new(asset);
    node *target = graph_add_node(asset->graph, registry_node_by_name("Atan2"));
    assert(e && target && target->input_count == 2);
    pin_binding x = { .kind = PIN_BINDING_COMPONENT_FIELD,
                      .component_key = "GAME.GameAim",
                      .field_name = "direction.x" };
    pin_binding y = { .kind = PIN_BINDING_COMPONENT_FIELD,
                      .component_key = "GAME.GameAim",
                      .field_name = "direction.y" };
    uint64_t x_pin_id = target->input_pins[0].id;
    uint64_t y_pin_id = target->input_pins[1].id;
    char *set_x = graph_collab_action_set_binding(e, target, 0, &x);
    char *set_y = graph_collab_action_set_binding(e, target, 1, &y);
    assert(set_x && set_y);
    assert(graph_move_pin(asset->graph, target, 0, 1, 0) == 0);

    char error[256] = { 0 };
    bool replayed_y = graph_collab_apply_action(e, set_y, error, sizeof error);
    error[0] = '\0';
    bool replayed_x = graph_collab_apply_action(e, set_x, error, sizeof error);
    int x_index = -1, y_index = -1;
    int x_is_output = 1, y_is_output = 1;
    graph_pin_by_id(target, x_pin_id, &x_is_output, &x_index);
    graph_pin_by_id(target, y_pin_id, &y_is_output, &y_index);
    const pin_binding *bound_x = graph_pin_binding(asset->graph, target, 0, x_index);
    const pin_binding *bound_y = graph_pin_binding(asset->graph, target, 0, y_index);
    bool stable_pins = !x_is_output && !y_is_output && x_index == 1 && y_index == 0;
    bool xy_bindings = bound_x && bound_x->kind == PIN_BINDING_COMPONENT_FIELD &&
                       bound_x->field_name &&
                       !strcmp(bound_x->field_name, "direction.x") && bound_y &&
                       bound_y->kind == PIN_BINDING_COMPONENT_FIELD &&
                       bound_y->field_name &&
                       !strcmp(bound_y->field_name, "direction.y");
    test_check(replayed_x && replayed_y && stable_pins && xy_bindings, "binding actions replay X and Y against stable pin IDs after input order changes");
    free(set_x);
    free(set_y);
    editor_free(e);

    behavior_asset *mismatch_asset = behavior_asset_new("Graph mismatched pin replay");
    editor *mismatch_editor = editor_new(mismatch_asset);
    node *mismatch_target = graph_add_node(mismatch_asset->graph, registry_node_by_name("Atan2"));
    assert(mismatch_editor && mismatch_target);
    char invalid[512];
    snprintf(
        invalid,
        sizeof invalid,
        "{\"version\":1,\"family\":\"graph\",\"op\":\"set_binding\","
        "\"document\":\"Graph mismatched pin replay\",\"graph\":%llu,"
        "\"node\":%llu,\"pin_id\":%llu,\"binding\":null}",
        (unsigned long long)mismatch_asset->graph->graph_id,
        (unsigned long long)mismatch_target->id,
        (unsigned long long)mismatch_target->output_pins[0].id
    );
    error[0] = '\0';
    bool rejected_mismatch = !graph_collab_apply_action(mismatch_editor, invalid, error, sizeof error);
    test_check(
        rejected_mismatch &&
            !graph_pin_binding(mismatch_asset->graph, mismatch_target, 0, 0),
        "binding replay rejects an output pin ID used as an input"
    );
    editor_free(mismatch_editor);

    behavior_asset *folded_asset = behavior_asset_new("Clear folded binding replay");
    editor *folded_editor = editor_new(folded_asset);
    node *source = graph_add_node(folded_asset->graph, registry_node_by_name("Add"));
    node *consumer = graph_add_node(folded_asset->graph, registry_node_by_name("Atan2"));
    assert(folded_editor && source && consumer);
    assert(graph_connect_data(folded_asset->graph, source, 0, consumer, 0) == 0);
    uint64_t source_id = source->id;
    assert(graph_node_is_inline(folded_asset->graph, source));
    char *clear = graph_collab_action_set_binding(folded_editor, consumer, 0, NULL);
    assert(clear);
    error[0] = '\0';
    bool cleared = graph_collab_apply_action(folded_editor, clear, error, sizeof error);
    node *remaining = graph_node_by_id(folded_asset->graph, consumer->id);
    node *remaining_source = NULL;
    int remaining_source_pin = -1;
    test_check(
        cleared && remaining && !graph_node_by_id(folded_asset->graph, source_id) &&
            !graph_input_source(folded_asset->graph, remaining, 0, &remaining_source, &remaining_source_pin) &&
            !graph_pin_binding(folded_asset->graph, remaining, 0, 0) &&
            remaining->input_pins[0].default_value.type == PIN_F64,
        "clearing a folded input source through replay leaves its typed "
        "constant input"
    );
    free(clear);
    editor_free(folded_editor);

    behavior_asset *literal_asset = behavior_asset_new("Folded source to constant");
    node *literal_source = graph_add_node(literal_asset->graph, registry_node_by_name("Byte"));
    node *literal_target = graph_add_node(literal_asset->graph, registry_node_by_name("Reroute Data"));
    assert(literal_source && literal_target);
    assert(graph_connect_data(literal_asset->graph, literal_source, 0, literal_target, 0) == 0);
    pin_value original = { .type = PIN_U8, .u = 41 };
    assert(graph_set_pin_default(literal_asset->graph, literal_target, 0, 0, &original) == 0);
    assert(graph_node_is_inline(literal_asset->graph, literal_source));
    uint64_t literal_source_id = literal_source->id;
    uint64_t literal_target_id = literal_target->id;
    uint64_t literal_pin_id = literal_target->input_pins[0].id;
    editor *literal_editor = editor_new(literal_asset);
    assert(literal_editor);
    char invalid_default[512];
    snprintf(
        invalid_default,
        sizeof invalid_default,
        "{\"version\":1,\"family\":\"graph\",\"op\":\"set_binding\","
        "\"document\":\"Folded source to constant\",\"graph\":%llu,"
        "\"node\":%llu,\"pin_id\":%llu,\"binding\":null,"
        "\"default\":{\"type\":%d,\"data\":256}}",
        (unsigned long long)literal_asset->graph->graph_id,
        (unsigned long long)literal_target_id,
        (unsigned long long)literal_pin_id,
        PIN_U8
    );
    error[0] = '\0';
    bool rejected_range = !graph_collab_apply_action(literal_editor, invalid_default, error, sizeof error);
    node *unchanged_source = graph_node_by_id(literal_editor->active_graph, literal_source_id);
    node *unchanged_target = graph_node_by_id(literal_editor->active_graph, literal_target_id);
    node *unchanged_input_source = NULL;
    int unchanged_source_pin = -1;
    test_check(
        rejected_range && !acount(literal_editor->undo) && unchanged_source &&
            unchanged_target &&
            graph_node_is_inline(literal_editor->active_graph, unchanged_source) &&
            graph_input_source(literal_editor->active_graph, unchanged_target, 0, &unchanged_input_source, &unchanged_source_pin) &&
            unchanged_input_source == unchanged_source &&
            unchanged_target->input_pins[0].default_value.u == original.u,
        "an out-of-range typed constant is rejected without removing its "
        "inline source"
    );

    pin_value constant = { .type = PIN_U8, .u = 7 };
    char *set_constant = graph_collab_action_set_input_default(literal_editor, unchanged_target, 0, &constant);
    assert(set_constant);
    error[0] = '\0';
    bool constant_applied = graph_collab_apply_action(literal_editor, set_constant, error, sizeof error);
    node *constant_target = graph_node_by_id(literal_editor->active_graph, literal_target_id);
    test_check(
        constant_applied &&
            !graph_node_by_id(literal_editor->active_graph, literal_source_id) &&
            constant_target &&
            !graph_input_source(literal_editor->active_graph, constant_target, 0, &unchanged_input_source, &unchanged_source_pin) &&
            !graph_pin_binding(literal_editor->active_graph, constant_target, 0, 0) &&
            constant_target->input_pins[0].default_value.u == constant.u &&
            acount(literal_editor->undo) == 1,
        "one typed constant action removes a folded source and sets the "
        "default in one Undo step"
    );
    free(set_constant);
    ed_undo(literal_editor);
    const graph *restored_graph = editor_active_graph(literal_editor);
    node *restored_source = graph_node_by_id(restored_graph, literal_source_id);
    node *restored_target = graph_node_by_id(restored_graph, literal_target_id);
    node *restored_input_source = NULL;
    int restored_source_pin = -1;
    test_check(
        restored_source && restored_target &&
            graph_node_is_inline(restored_graph, restored_source) &&
            graph_input_source(restored_graph, restored_target, 0, &restored_input_source, &restored_source_pin) &&
            restored_input_source == restored_source &&
            restored_target->input_pins[0].default_value.u == original.u,
        "Undo restores the folded source and its previous typed default"
    );
    editor_free(literal_editor);

    const enum_desc *enumeration = type_catalog_enum_at(0);
    assert(enumeration && enumeration->value_count > 0);
    int invalid_enum = 0;
    while (type_catalog_enum_label(enumeration->name, invalid_enum)) {
        invalid_enum++;
    }
    pin_spec enum_input = {
        .name = "Selection",
        .type = PIN_ENUM,
        .type_name = enumeration->name,
        .kind = PIN_DATA,
        .default_value = { .type = PIN_ENUM, .e = enumeration->values[0].value },
    };
    node_desc enum_target_desc = {
        .name = "Enum typed default target",
        .input_count = 1,
        .inputs = &enum_input,
    };
    behavior_asset *enum_asset = behavior_asset_new("Enum typed default");
    node *enum_target = graph_add_node(enum_asset->graph, &enum_target_desc);
    editor *enum_editor = editor_new(enum_asset);
    assert(enum_target && enum_editor && invalid_enum <= enumeration->value_count);
    pin_value unlisted = {
        .type = PIN_ENUM,
        .type_name = enumeration->name,
        .e = invalid_enum,
    };
    bool builder_rejected_enum = !graph_collab_action_set_input_default(enum_editor, enum_target, 0, &unlisted);
    char invalid_enum_action[512];
    snprintf(
        invalid_enum_action,
        sizeof invalid_enum_action,
        "{\"version\":1,\"family\":\"graph\",\"op\":\"set_binding\","
        "\"document\":\"Enum typed default\",\"graph\":%llu,"
        "\"node\":%llu,\"pin_id\":%llu,\"binding\":null,"
        "\"default\":{\"type\":%d,\"type_name\":\"%s\",\"data\":%d}}",
        (unsigned long long)enum_asset->graph->graph_id,
        (unsigned long long)enum_target->id,
        (unsigned long long)enum_target->input_pins[0].id,
        PIN_ENUM,
        enumeration->name,
        invalid_enum
    );
    int enum_default = enum_target->input_pins[0].default_value.e;
    error[0] = '\0';
    bool replay_rejected_enum = !graph_collab_apply_action(enum_editor, invalid_enum_action, error, sizeof error);
    test_check(
        builder_rejected_enum && replay_rejected_enum && !acount(enum_editor->undo) &&
            enum_target->input_pins[0].default_value.e == enum_default,
        "typed enum constants reject values outside the reflected enum"
    );
    editor_free(enum_editor);

    behavior_asset *expand_asset = behavior_asset_new("Expand inline replay");
    editor *expand_editor = editor_new(expand_asset);
    node *inner = graph_add_node(expand_asset->graph, registry_node_by_name("Add"));
    node *outer = graph_add_node(expand_asset->graph, registry_node_by_name("Add"));
    node *expand_target = graph_add_node(expand_asset->graph, registry_node_by_name("Atan2"));
    assert(expand_editor && inner && outer && expand_target);
    assert(graph_connect_data(expand_asset->graph, inner, 0, outer, 0) == 0);
    assert(graph_connect_data(expand_asset->graph, outer, 0, expand_target, 0) == 0);
    assert(
        graph_node_is_inline(expand_asset->graph, inner) &&
        graph_node_is_inline(expand_asset->graph, outer)
    );
    char *expand = graph_collab_action_expand_inline(expand_editor, expand_target, 0);
    assert(expand);
    error[0] = '\0';
    bool expanded = graph_collab_apply_action(expand_editor, expand, error, sizeof error);
    test_check(
        expanded && inner->presentation == NODE_PRESENTATION_EXPANDED &&
            outer->presentation == NODE_PRESENTATION_EXPANDED &&
            !inner->placed && !outer->placed && expand_editor->arrange_pending,
        "inline expansion replay expands the full source tree and schedules "
        "arrangement"
    );
    free(expand);
    editor_free(expand_editor);
}

static void check_event_catalog_search()
{
    behavior_asset *asset = behavior_asset_new("Event catalog search");
    editor *e = editor_new(asset);
    editor_set_event_catalog(e, GAME_HOST_EVENTS, GAME_HOST_EVENT_COUNT);
    search_insert(e, "On Begin");
    node *begin = asset->graph->nodes[acount(asset->graph->nodes) - 1];
    test_check(begin->reference_name && !strcmp(begin->reference_name, "Begin"), "the application event catalog exposes behavior initialization through node search");
    editor_free(e);
}

/* Search results that are undescribed, printing each. */
static int undescribed(const editor *e, const char *query)
{
    ed_search_match *matches = ed_search_matches(e, query);
    int missing = 0;
    for (int i = 0; i < acount(matches); i++) {
        node_subject subject = ed_search_match_subject(e, &matches[i]);
        char about[256];
        if (!node_subject_describe(&subject, about, sizeof about)) {
            printf("  %s has no description\n", matches[i].name);
            missing++;
        }
    }
    afree(matches);
    return missing;
}

/* The add-node palette describes each result: node types, component presets,
   engine events and event definitions, sending one names it. */
static void check_search_descriptions()
{
    behavior_asset *asset = behavior_asset_new("Described search");
    behavior_asset *hit = behavior_asset_new("Hit");
    hit->domain = BEHAVIOR_DOMAIN_EVENT_DEFINITION;
    editor *e = editor_new(asset);
    editor_set_event_catalog(e, GAME_HOST_EVENTS, GAME_HOST_EVENT_COUNT);
    const behavior_asset *catalog[] = { hit };
    editor_set_asset_catalog(e, catalog, 1);
    test_check(!undescribed(e, "") && !undescribed(e, "health"), "every add-node search result has a description");
    ed_search_match *matches = ed_search_matches(e, "Send Hit");
    node_subject subject = acount(matches) ? ed_search_match_subject(e, &matches[0])
                                           : (node_subject){ 0 };
    char about[256] = "";
    test_check(
        subject.desc && node_subject_describe(&subject, about, sizeof about) &&
            !strcmp(about, "Sends Hit to Entity."),
        "a Send result names the event it sends"
    );
    afree(matches);
    editor_free(e);
    behavior_asset_free(hit);
}

/* Browsing the catalog with no query: entries by kind. */
static void component_catalog_counts(const editor *e, const char *label)
{
    ed_search_match *matches = ed_search_matches(e, "");
    int access = 0, fields = 0, structural = 0, make_break = 0;
    for (int i = 0; i < acount(matches); i++) {
        const node_desc *d = matches[i].desc;
        fields += d->component_kind == NODE_COMPONENT_SET_FIELD;
        access += d->component_kind != NODE_COMPONENT_NONE &&
                  d->component_kind != NODE_COMPONENT_SET_FIELD;
        structural += !strncmp(d->name, "Add_", 4) || !strncmp(d->name, "Remove_", 7);
        make_break += d->pure_kind == NODE_PURE_MAKE || d->pure_kind == NODE_PURE_BREAK;
    }
    printf(
        "node catalog %s: %d entries; %d component access, %d field setters, "
        "%d add/remove, %d make/break\n",
        label,
        acount(matches),
        access,
        fields,
        structural,
        make_break
    );
    afree(matches);
}

static node *spawn_first(editor *e, const char *query, const char *name)
{
    ed_search_match *matches = ed_search_matches(e, query);
    bool found = acount(matches) && !strcmp(matches[0].name, name);
    node *n = found ? ed_spawn_match_at(e, &matches[0], 0, 0) : NULL;
    afree(matches);
    return n;
}

/* The catalog lists component nodes instead of generated per-component and
 * per-struct nodes; naming a component or struct spawns a configured node. */
static void check_component_search()
{
    behavior_asset *asset = behavior_asset_new("Component search");
    assert(behavior_asset_set_owner_type(asset, "ecs.entity") == 0);
    editor *e = editor_new(asset);
    component_catalog_counts(e, "behavior");
    ed_search_match *matches = ed_search_matches(e, "");
    int component_nodes = 0, generated = 0;
    for (int i = 0; i < acount(matches); i++) {
        component_nodes += component_node_kind_of(matches[i].desc) &&
                           !matches[i].setting;
        generated += component_generated_kind(matches[i].desc) != COMPONENT_NODE_NONE;
    }
    afree(matches);
    test_check(component_nodes == 7 && !generated, "browsing lists seven component nodes and no generated access, structural, Make or Break nodes");
    node *get = spawn_first(e, "Get Component", "Get Component");
    test_check(
        get && get->desc == &NODE_GetComponent && !component_node_component(get),
        "the generic node spawns without a component"
    );
    node *velocity = spawn_first(e, "Set Velocity", "Set Velocity");
    test_check(
        velocity && velocity->desc == &NODE_SetComponent &&
            !strcmp(component_node_key(velocity), "GAME.GameVelocity") &&
            !component_node_split(velocity) &&
            !strcmp(velocity->input_pins[3].type_name, "GameVelocity") &&
            !strcmp(velocity->output_pins[1].type_name, "GameVelocity"),
        "Set Velocity spawns Set Component writing Velocity"
    );
    node *x = spawn_first(e, "Set Velocity X", "Set Velocity X");
    test_check(
        x && component_node_split(x) && x->input_count == 4 &&
            !strcmp(x->input_pins[3].name, "x") && x->input_pins[3].type == PIN_F32,
        "Set Velocity X spawns Set Component showing only X"
    );
    node *health = spawn_first(e, "has health", "Has Health");
    test_check(
        health && health->desc == &NODE_HasComponent &&
            !strcmp(component_node_key(health), "GAME.GameHealth"),
        "Has Health spawns Has Component for Health"
    );
    node *transform = spawn_first(e, "get transform", "Get Transform");
    test_check(
        transform && !strcmp(transform->output_pins[0].type_name, "GameTransform") &&
            (transform->output_pins[0].flags & PIN_FLAG_REFERENCE),
        "Get Transform spawns Get Component reading Transform"
    );
    node *add = spawn_first(e, "add health", "Add Health");
    node *remove = spawn_first(e, "remove health", "Remove Health");
    test_check(
        add && add->desc == &NODE_AddComponent && remove &&
            remove->desc == &NODE_RemoveComponent &&
            !strcmp(component_node_key(remove), "GAME.GameHealth") &&
            add->output_count == 2 && add->output_pins[1].type == PIN_BOOL,
        "Add and Remove Health spawn structural component nodes"
    );
    node *make = spawn_first(e, "make transform", "Make Transform");
    node *split = spawn_first(e, "break transform", "Break Transform");
    test_check(
        make && make->desc == &NODE_MakeStruct && make->input_count == 3 &&
            !strcmp(make->input_pins[0].name, "x") &&
            !strcmp(make->output_pins[0].type_name, "GameTransform") && split &&
            split->desc == &NODE_BreakStruct && split->output_count == 2 &&
            !strcmp(split->input_pins[0].type_name, "GameTransform"),
        "Make and Break Transform spawn struct nodes with the Transform fields"
    );
    char title[256];
    test_check(
        transform &&
            !strcmp(ed_node_title(e, transform, title, sizeof title), "Get Transform") &&
            x && !strcmp(ed_node_title(e, x, title, sizeof title), "Set Velocity") && get &&
            !strcmp(ed_node_title(e, get, title, sizeof title), "Get Component") && add &&
            !strcmp(ed_node_title(e, add, title, sizeof title), "Add Health") && make &&
            !strcmp(ed_node_title(e, make, title, sizeof title), "Make Transform"),
        "component nodes are titled by access and component"
    );
    editor_free(e);
}

/* Dropping an input's grip on another row in Member properties. */
static void check_signature_drag()
{
    behavior_asset *asset = behavior_asset_new("Signature drag");
    graph *fn = behavior_asset_add_function(asset, "Mix", 0);
    const char *const names[] = { "A", "B", "C" };
    for (int i = 0; i < 3; i++) {
        pin_spec pin = { .name = names[i], .type = PIN_F32, .type_name = "float" };
        graph_signature_add(asset, fn, false, &pin);
    }
    behavior_asset_add_function_call_node(asset, fn);
    editor *e = editor_new(asset);
    node *entry = graph_node_by_id(fn, fn->entry_id);
    editor_tick(e, 0.0);
    unsigned revision = editor_edit_revision(e);
    uint64_t dragged = entry->output_pins[3].id;
    test_check(
        !sm_move_signature_pin(e, fn, false, dragged, 3) && acount(e->undo) == 0,
        "an input dropped on its own row changes nothing"
    );
    test_check(
        sm_move_signature_pin(e, fn, false, dragged, 1) &&
            acount(e->undo) == 1 && !strcmp(entry->output_pins[1].name, "C"),
        "an input dropped on another row moves there as one undo step"
    );
    editor_query state;
    editor_query_state(e, &state);
    test_check(state.dirty, "reordering keyed function parameters marks the document dirty");
    editor_tick(e, 1.0);
    test_check(editor_edit_revision(e) == revision + 1, "reordering keyed function parameters advances the edit revision");
    ed_undo(e);
    fn = behavior_asset_find_graph_by_name(e->asset, "Mix");
    entry = graph_node_by_id(fn, fn->entry_id);
    test_check(!strcmp(entry->output_pins[3].name, "C"), "Undo puts a moved input back");
    editor_query_state(e, &state);
    test_check(!state.dirty, "undoing keyed parameter order restores the clean baseline");
    editor_free(e);
}

static void check_compound_graph_collab_replay()
{
    behavior_asset *asset = behavior_asset_new("Compound graph replay");
    asset->domain = BEHAVIOR_DOMAIN_MATERIAL;
    node *surface = shader_graph_add_output(asset->graph, false);
    node *ramp = graph_add_node(asset->graph, registry_node_by_name("Shader Color Ramp"));
    assert(surface && ramp && shader_graph_init_node(asset->graph, ramp));
    uint64_t surface_id = surface->id, ramp_id = ramp->id;
    editor *e = editor_new(asset);
    float rgb[] = { 0.2f, 0.4f, 0.6f };
    pin_value color = { .type = PIN_STRUCT, .type_name = "ShaderVec3", .ptr = rgb };
    editor_command color_command = {
        .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
        .data.set_pin_default = { surface_id, 0, 0, &color, false }
    };
    char *color_action = graph_collab_action_for_command(e, color_command);
    assert(color_action);
    char error[256] = { 0 };
    bool color_replayed = graph_collab_apply_action(e, color_action, error, sizeof error);
    rgb[0] = 0.9f;
    surface = graph_node_by_id(e->active_graph, surface_id);
    const float *stored_rgb = surface->input_pins[0].default_value.ptr;
    bool color_owned = stored_rgb && fabsf(stored_rgb[0] - 0.2f) < 0.00001f &&
                       fabsf(stored_rgb[2] - 0.6f) < 0.00001f;
    shader_ramp_stop stops[] = {
        { .position = 0, .color = { 0.2f, 0.4f, 0.6f, 0.375f } },
        { .position = 0.5f, .color = { 0.3f, 0.5f, 0.7f, 0.625f } },
        { .position = 1, .color = { 0.4f, 0.6f, 0.8f, 0.875f } },
    };
    pin_value items[3];
    for (int i = 0; i < 3; i++) {
        items[i] = (pin_value){ .type = PIN_STRUCT,
                                .type_name = "ShaderRampStop",
                                .ptr = &stops[i] };
    }
    pin_array array = { .element_type = PIN_STRUCT,
                        .type_name = "ShaderRampStop",
                        .count = 3,
                        .capacity = 3,
                        .items = items };
    pin_value collection = { .type = PIN_STRUCT,
                             .type_name = "ShaderRampStop",
                             .container = PIN_CONTAINER_ARRAY,
                             .ptr = &array };
    editor_command collection_command = {
        .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
        .data.set_pin_default = { ramp_id, 0, 1, &collection, false }
    };
    char *collection_action = graph_collab_action_for_command(e, collection_command);
    assert(collection_action);
    bool collection_replayed = graph_collab_apply_action(e, collection_action, error, sizeof error);
    stops[1].color[3] = 0.1f;
    shader_ramp_stop stored[SHADER_RAMP_MAX];
    ramp = graph_node_by_id(e->active_graph, ramp_id);
    bool collection_owned = shader_node_ramp_get(ramp, stored) == 3 &&
                            fabsf(stored[1].color[3] - 0.625f) < 0.00001f;
    editor_undo(e);
    ramp = graph_node_by_id(e->active_graph, ramp_id);
    bool undo = shader_node_ramp_get(ramp, stored) == 2;
    editor_redo(e);
    ramp = graph_node_by_id(e->active_graph, ramp_id);
    bool redo = shader_node_ramp_get(ramp, stored) == 3 &&
                fabsf(stored[0].color[3] - 0.375f) < 0.00001f;
    float invalid_rgb[] = { INFINITY, 0, 0 };
    color.ptr = invalid_rgb;
    bool invalid_rejected = !graph_collab_action_for_command(e, color_command);
    color.ptr = rgb;
    color.type_name = "ShaderVec2";
    bool wrong_builder_rejected = !graph_collab_action_for_command(e, color_command);
    char *wrong_shape = xstrdup(color_action);
    char *nominal = strstr(wrong_shape, "ShaderVec3");
    assert(nominal);
    nominal[strlen("ShaderVec")] = '2';
    int undo_count = acount(e->undo);
    bool mismatch_rejected = !graph_collab_apply_action(e, wrong_shape, error, sizeof error) &&
                             acount(e->undo) == undo_count;
    test_check(color_replayed && color_owned, "collaboration replays vector/color defaults with independent owned storage");
    test_check(collection_replayed && collection_owned && undo && redo, "collaboration replays nested ramp arrays with precise alpha and Undo/Redo after temporary decode storage is freed");
    test_check(invalid_rejected && wrong_builder_rejected && mismatch_rejected, "collaboration rejects nonfinite compound values and wrong nominal pin shapes before history mutation");
    free(wrong_shape);
    free(color_action);
    free(collection_action);
    editor_free(e);
}

static void check_duplicate_selection()
{
    behavior_asset *asset = behavior_asset_new("Native duplicate");
    graph *g = asset->graph;
    node *source = graph_add_node(g, registry_node_by_name("Real"));
    node *target = graph_add_node(g, registry_node_by_name("Add"));
    assert(source && target && graph_connect(g, source, 0, target, 0) == 0);
    pin_value value = { .type = PIN_F64, .type_name = "double", .d = 7.0 };
    assert(graph_set_pin_default(g, source, 0, 0, &value) == 0);
    graph_set_node_presentation(g, source, NODE_PRESENTATION_EXPANDED);
    source->x = 10;
    source->y = 20;
    target->x = 60;
    target->y = 80;
    editor *e = editor_new(asset);
    ed_add_selected(e, source);
    ed_add_selected(e, target);
    ed_duplicate_selected(e);
    graph *duplicated = e->active_graph;
    node *source_copy = acount(duplicated->nodes) == 4 ? duplicated->nodes[2] : NULL;
    node *target_copy = acount(duplicated->nodes) == 4 ? duplicated->nodes[3] : NULL;
    test_check(
        source_copy && target_copy && acount(duplicated->edges) == 2 &&
            source_copy->id != source->id && target_copy->id != target->id &&
            source_copy->x == 34 && source_copy->y == 44 && target_copy->x == 84 &&
            target_copy->y == 104 && source_copy->input_pins[0].default_value.d == 7.0,
        "Duplicate copies connected nodes with fresh IDs, typed defaults and "
        "offset positions"
    );
    uint64_t copied_id = source_copy ? source_copy->id : 0;
    value.d = 9.0;
    if (source_copy) {
        graph_set_pin_default(duplicated, source_copy, 0, 0, &value);
    }
    test_check(source->input_pins[0].default_value.d == 7.0, "Duplicated nodes own their default values independently");
    ed_undo(e);
    test_check(
        acount(editor_active_graph(e)->nodes) == 2 &&
            acount(editor_active_graph(e)->edges) == 1,
        "One Undo removes a duplicated connected selection"
    );
    ed_redo(e);
    node *restored = graph_node_by_id(editor_active_graph(e), copied_id);
    test_check(
        acount(editor_active_graph(e)->nodes) == 4 &&
            acount(editor_active_graph(e)->edges) == 2 && restored &&
            restored->input_pins[0].default_value.d == 9.0,
        "Redo restores duplicated nodes and their independently edited values"
    );
    editor_free(e);
}

void run_editor_command_checks(void)
{
    check_duplicate_selection();
    check_streamed_literal_undo();
    check_compound_graph_collab_replay();
    check_signature_drag();
    check_linked_preview();
    check_library_problem_source();
    check_search_catalog();
    check_component_search();
    check_library_search_snapshot();
    check_typed_wire_search();
    check_graph_binding_collab_replay();
    check_graph_reconnect_collab_replay();
    check_event_catalog_search();
    check_search_descriptions();
    check_insert_on_wire();
    behavior_asset *asset = behavior_asset_new("Editor command test");
    node *source = graph_add_node(asset->graph, &g_editor_float_desc);
    node *sink = graph_add_node(asset->graph, &g_editor_int_desc);
    node *float_sink = graph_add_node(asset->graph, &g_editor_float_sink_desc);
    editor *e = editor_new(asset);
    test_check(e != NULL, "editor command seam constructs a headless editor");
    if (!e) {
        behavior_asset_free(asset);
        return;
    }

    editor_query state;
    editor_query_state(e, &state);
    test_check(!state.dirty, "a new editor starts with a clean saved baseline");
    editor_tick(e, 0.0);
    editor_command invalid_connect = {
        .kind = EDITOR_COMMAND_CONNECT_DATA,
        .data.connect = { source->id, 0, sink->id, 1 },
    };
    test_check(editor_command_apply(e, invalid_connect) == -1, "an invalid connection is rejected by the command seam");
    editor_query_state(e, &state);
    test_check(!state.dirty && acount(asset->graph->edges) == 0, "a rejected connection leaves no dirty or history edit");
    unsigned revision = editor_edit_revision(e);
    editor_undo(e);
    editor_tick(e, 1.0);
    test_check(editor_edit_revision(e) == revision, "a rejected connection does not add an undo revision");
    unsigned revision_before_invalid_spawn = editor_edit_revision(e);
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_SPAWN_NODE,
                .data.spawn_node = { NULL, 0.0f, 0.0f },
            }
        ) == -1,
        "a spawn command rejects a missing node description"
    );
    test_check(
        editor_edit_revision(e) == revision_before_invalid_spawn &&
            acount(asset->graph->nodes) == 3,
        "a rejected spawn leaves no undo revision or node"
    );
    pin_value float_value = { .type = PIN_F32, .f = 1.0f };
    editor_query_state(e, &state);
    int dirty_before_invalid_pin = state.dirty;
    unsigned revision_before_invalid_pin = editor_edit_revision(e);
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                .data.set_pin_default = { source->id, 2, 0, &float_value, false },
            }
        ) == -1,
        "a pin default command rejects a non-boolean output selector"
    );
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                .data.set_pin_default = { source->id, 0, 1, &float_value, false },
            }
        ) == -1,
        "a pin default command rejects an out-of-range pin"
    );
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                .data.set_pin_default = { sink->id, 0, 0, &float_value, false },
            }
        ) == -1,
        "a pin default command rejects a mismatched value type"
    );
    editor_query_state(e, &state);
    test_check(
        state.dirty == dirty_before_invalid_pin &&
            editor_edit_revision(e) == revision_before_invalid_pin,
        "rejected pin defaults leave no dirty or history edit"
    );
    editor_command valid_connect = {
        .kind = EDITOR_COMMAND_CONNECT_DATA,
        .data.connect = { source->id, 0, float_sink->id, 0 },
    };
    test_check(editor_command_apply(e, valid_connect) == 0, "a valid typed connection uses the editor mutation path");
    test_check(acount(asset->graph->edges) == 1, "valid command connections update graph topology");

    editor_command create_graph = {
        .kind = EDITOR_COMMAND_CREATE_GRAPH,
        .data.create_graph = { GRAPH_FUNCTION, "Worker" },
    };
    editor_query_state(e, &state);
    int dirty_before_create = state.dirty;
    unsigned revision_before_create = editor_edit_revision(e);
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_CREATE_GRAPH,
                .data.create_graph = { GRAPH_FUNCTION, "" },
            }
        ) == -1,
        "graph creation rejects an empty name"
    );
    editor_query_state(e, &state);
    test_check(
        state.dirty == dirty_before_create &&
            editor_edit_revision(e) == revision_before_create,
        "a rejected graph creation leaves no dirty or history edit"
    );
    test_check(editor_command_apply(e, create_graph) == 0, "the command seam creates a graph");
    graph *worker = behavior_asset_find_graph_by_name(asset, "Worker");
    test_check(worker != NULL, "created graphs remain discoverable by name");
    test_check(
        worker && acount(worker->nodes) == 2 &&
            worker->nodes[0]->desc == &NODE_FunctionEntry &&
            worker->nodes[1]->desc == &NODE_Return,
        "command-created functions receive entry and return nodes"
    );
    test_check(
        editor_open_graph(e, worker->graph_id) == 0 && editor_active_graph(e) == worker,
        "the command seam can open the created graph"
    );

    editor_query_state(e, &state);
    int dirty_before_rename = state.dirty;
    unsigned revision_before_rename = editor_edit_revision(e);
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_RENAME_GRAPH,
                .data.rename_graph = { worker->graph_id, "" },
            }
        ) == -1,
        "graph renaming rejects an empty name"
    );
    editor_query_state(e, &state);
    test_check(
        state.dirty == dirty_before_rename &&
            editor_edit_revision(e) == revision_before_rename,
        "a rejected graph rename leaves no dirty or history edit"
    );

    editor_command rename_graph = {
        .kind = EDITOR_COMMAND_RENAME_GRAPH,
        .data.rename_graph = { worker->graph_id, "Renamed" },
    };
    test_check(editor_command_apply(e, rename_graph) == 0, "the command seam renames a graph");
    editor_query_state(e, &state);
    test_check(state.dirty, "authored graph changes are observable as dirty");
    behavior_asset *other = behavior_asset_new("Other asset");
    test_check(
        other && editor_open_behavior_asset(e, other, other->graph->graph_id) == 0,
        "opening a different behavior asset succeeds"
    );
    editor_query_state(e, &state);
    test_check(state.graph_count == 1 && !state.dirty, "opening a different clean behavior asset resets the dirty baseline");
    test_check(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_CREATE_GRAPH,
                .data.create_graph = { GRAPH_FUNCTION, "OtherWorker" },
            }
        ) == 0,
        "the newly opened behavior asset remains editable"
    );
    editor_query_state(e, &state);
    test_check(state.dirty, "edits after opening a different asset are observable as dirty");
    behavior_asset_free(other);
    editor_free(e);
    check_delete_shared_source();
}
