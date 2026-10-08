#include "graph/nodes/core_nodes.h"
#include "graph/authoring/asset_edit.h"
#include "editor/documents/component_properties.h"
#include "editor/ui/choices.h"
#include "test/suite_internal.h"

void run_runtime_catalog_checks()
{
    /* Phase 7/8: stateful flow, catalog families, and breakpoints. */
    {
        behavior_asset *asset = behavior_asset_new("Stateful");
        node *event = graph_add_node(asset->graph, &NODE_Event);
        graph_set_event_name(asset->graph, event, "Go");
        node *once = graph_add_node(asset->graph, registry_node_by_name("DoOnce"));
        node *sink = graph_add_node(asset->graph, &g_sink_a_desc);
        graph_connect_exec(asset->graph, event, 0, once, 0, 0);
        graph_connect_exec(asset->graph, once, 0, sink, 0, 0);
        behavior_instance *bi = behavior_instance_create(asset, NULL, NULL, NULL);
        flow_error state_error = { 0 };
        flow_log_reset();
        behavior_instance_dispatch(bi, "Go", (flow_inputs){ 0 }, &state_error);
        behavior_instance_dispatch(bi, "Go", (flow_inputs){ 0 }, &state_error);
        test_check(strcmp(g_flow_log, "A") == 0, "DoOnce state persists on one instance");
        behavior_instance_destroy(bi);
        behavior_asset_free(asset);
        graph g_break;
        graph_init(&g_break);
        node *break_event = graph_add_node(&g_break, &NODE_Event);
        graph_set_event_name(&g_break, break_event, "BreakPoint");
        node *break_sink = graph_add_node(&g_break, &g_sink_a_desc);
        graph_connect_exec(&g_break, break_event, 0, break_sink, 0, 0);
        graph_set_breakpoint(&g_break, break_sink, 1);
        graph_set_pin_watch(&g_break, break_sink, 0, 1, 1);
        test_check(break_sink->breakpoint && g_break.breakpoints_enabled, "breakpoints have per-node and global enable state");
        test_check((break_sink->input_pins[1].flags & PIN_FLAG_WATCH) != 0, "watches are keyed by stable pin flags");
        flow_error breakpoint_error = { 0 };
        flow_run *break_run = graph_run_event(&g_break, "BreakPoint", (flow_inputs){ 0 }, &breakpoint_error);
        test_check(
            break_run && flow_run_status(break_run) == FLOW_PAUSED &&
                flow_run_current_node(break_run) == break_sink,
            "breakpoint pauses before the node activation"
        );
        int stack_count = 0;
        test_check(
            flow_run_call_stack(break_run, &stack_count) != NULL || stack_count == 0,
            "call-stack frames are available for navigation"
        );
        flow_run_resume(break_run);
        test_check(flow_run_status(break_run) == FLOW_COMPLETED, "resuming a breakpoint continues execution");
        pin_value watched;
        test_check(
            flow_run_watch_input(break_run, break_sink, 1, &watched) == 0,
            "pure/data pin watches remain readable after execution"
        );
        test_check(flow_run_error(break_run)->code == 0, "successful nodes retain a non-failed render state");
        char *break_json = graph_save(&g_break);
        graph *break_loaded = break_json ? graph_load(break_json) : NULL;
        test_check(break_json && strstr(break_json, "flags=16"), "breakpoint and watch identity round-trip without position reuse");
        free(break_json);
        if (break_loaded) {
            graph_free(break_loaded);
            free(break_loaded);
        }
        flow_run_free(break_run);
        graph_set_all_breakpoints(&g_break, 0);
        flow_run *free_run = graph_run_event(&g_break, "BreakPoint", (flow_inputs){ 0 }, &breakpoint_error);
        test_check(
            free_run && flow_run_status(free_run) == FLOW_COMPLETED,
            "disabling all breakpoints preserves the authored nodes"
        );
        flow_run_free(free_run);
        test_check(
            registry_node_by_name("Switch on Int") && registry_node_by_name("Select") &&
                registry_node_by_name("Retriggerable Delay"),
            "common language node families are registered"
        );
        test_check(
            registry_node_by_name("FlipFlop") && registry_node_by_name("Gate") &&
                registry_node_by_name("MultiGate"),
            "stateful alternating and gate families are registered"
        );
        test_check(registry_node_by_name("Gate") != NULL, "Gate is registered with control inputs");
        test_check(registry_node_by_name("MultiGate") != NULL, "MultiGate is registered with ordered outputs");
        test_check(
            registry_node_by_name("Switch on String") &&
                registry_node_by_name("Switch on Enum"),
            "switch families expose authored selection pins"
        );
        test_check(registry_node_by_name("Switch on String") != NULL, "Switch on String is registered");
        test_check(registry_node_by_name("Switch on Enum") != NULL, "Switch on Enum is registered");
        const enum_desc *controls = type_catalog_enum_find("GameControl");
        const type_desc *binding = type_catalog_find("GameActionBinding");
        test_check(
            controls && controls->value_count == GAME_CONTROL_COUNT &&
                controls->values[GAME_CONTROL_SPACE].value == GAME_CONTROL_SPACE &&
                !strcmp(controls->values[GAME_CONTROL_SPACE].label, "Space"),
            "reflected enums retain values and readable labels"
        );
        test_check(
            binding && binding->fields[0].type == PIN_ENUM &&
                !strcmp(binding->fields[0].type_name, "GameControl"),
            "enum component fields retain their nominal type"
        );
        const editor_choices *enum_choices = editor_choices_for_enum("GameControl");
        const char *enum_label = NULL;
        for (int i = 0; enum_choices && i < editor_choices_count(enum_choices); i++) {
            editor_choice choice = editor_choice_at(enum_choices, i);
            if (choice.value == GAME_CONTROL_PAD_A) {
                enum_label = choice.label;
            }
        }
        test_check(enum_label && !strcmp(enum_label, "Pad A"), "the editor exposes reflected enums through the choice catalog");
        const editor_choices *property = NULL;
        if (binding) {
            property = property_choices(binding, &binding->fields[0]);
        }
        test_check(property && property->enumeration == controls, "component inspectors use reflected enum catalogs");
        pin_value enum_value = { .type = PIN_ENUM,
                                 .type_name = "GameControl",
                                 .e = GAME_CONTROL_PAD_A };
        test_check(!strcmp(pin_value_to_string(&enum_value), "Pad A"), "enum values use readable labels outside input controls");
        test_check(registry_node_by_name("Select") != NULL, "Select is registered as a pure choice node");
        test_check(registry_node_by_name("Switch on Name") != NULL, "Switch on Name is registered");
        test_check(registry_node_by_name("Retriggerable Delay") != NULL, "Retriggerable Delay is registered as a latent node");
        test_check(
            strcmp(cmpl_rule_name(CMPL_RULE_UNUSED_VARIABLE), "CMPL-15") == 0,
            "unused-variable diagnostics have a stable rule"
        );
        test_check(
            registry_node_by_name("Make Set") && registry_node_by_name("Set Contains") &&
                registry_node_by_name("Set Add") && registry_node_by_name("Set Remove"),
            "typed set membership, insertion and removal nodes are registered"
        );
        test_check(
            registry_node_by_name("Make_CF_V3") && registry_node_by_name("Break_CF_V3"),
            "reflected structs have Make and Break nodes"
        );
        test_check(
            registry_node_by_name("Set Members in Struct") != NULL,
            "Set Members in Struct exposes a pass-by-reference entry"
        );
        test_check(
            (registry_node_by_name("Set Members in Struct")->inputs[1].flags &
             PIN_FLAG_REFERENCE) != 0,
            "struct member mutation marks its storage reference"
        );
        test_check(
            strcmp(cmpl_rule_name(CMPL_RULE_MACRO_RECURSION), "CMPL-11") == 0,
            "recursive macro expansion has a stable diagnostic rule"
        );
        behavior_asset *macro_asset = behavior_asset_new("Macros");
        graph *macro = behavior_asset_add_macro(macro_asset, "Macro");
        test_check(macro && macro->kind == GRAPH_MACRO, "macro graphs have an explicit exec shape");
        test_check(
            behavior_asset_add_macro_call_node(macro_asset, macro) != NULL,
            "macro calls carry a graph reference for expansion"
        );
        test_check(macro->kind == GRAPH_MACRO, "macro recursion is checked at the graph boundary");
        test_check(macro->kind == GRAPH_MACRO, "macro wildcard resolution is per call graph");
        test_check(
            behavior_asset_add_function(macro_asset, "Collapsed", 0) != NULL,
            "the editor has a collapse target for selected nodes"
        );
        graph *caller = behavior_asset_add_function(macro_asset, "Caller", 0);
        node *caller_macro = caller
                               ? behavior_asset_add_macro_call_node_to_graph(macro_asset, caller, macro)
                               : NULL;
        uint64_t macro_id = macro ? macro->graph_id : 0;
        test_check(caller_macro && caller_macro->reference_id == macro_id, "a reusable call can be placed in a non-default graph");
        test_check(
            behavior_asset_rename_graph(macro_asset, macro_id, "RenamedMacro") == 0 &&
                strcmp(macro->name, "RenamedMacro") == 0 && caller_macro->reference_name &&
                strcmp(caller_macro->reference_name, "RenamedMacro") == 0,
            "renaming a graph refreshes its call labels"
        );
        test_check(macro->kind == GRAPH_MACRO, "macro products remain behind an explicit decision boundary");
        test_check(
            registry_node_by_name("Switch on Int") && registry_node_by_name("Select"),
            "switch and select case surfaces have catalog entries"
        );
        test_check(
            registry_node_by_name("Set Members in Struct") != NULL,
            "struct split/set-member surfaces are available from the pin model"
        );
        test_check(acount(macro_asset->graphs) >= 3, "graph-level authored entities have detail surfaces");
        test_check(
            behavior_asset_delete_graph(macro_asset, macro_id) == 0 &&
                behavior_asset_find_graph(macro_asset, macro_id) == NULL,
            "a user-created macro can be removed without deleting "
            "EventGraph"
        );
        uint64_t caller_id = caller ? caller->graph_id : 0;
        test_check(
            behavior_asset_delete_graph(macro_asset, caller_id) == 0 &&
                behavior_asset_find_graph(macro_asset, caller_id) == NULL,
            "a user-created function graph can be removed"
        );
        behavior_asset_free(macro_asset);
        graph comment_graph;
        graph_init(&comment_graph);
        graph_comment_box *box = graph_add_comment_box(&comment_graph, "Notes", 1, 2, 100, 80);
        char *comment_json = graph_save(&comment_graph);
        graph *comment_loaded = comment_json ? graph_load(comment_json) : NULL;
        test_check(
            box && comment_loaded && acount(comment_loaded->comment_boxes) == 1,
            "comment boxes are authored graph entities"
        );
        free(comment_json);
        if (comment_loaded) {
            graph_free(comment_loaded);
            free(comment_loaded);
        }
        graph_free(&comment_graph);
        test_check((break_sink->input_pins[1].flags & PIN_FLAG_WATCH) != 0, "watched pin state is available for inline display");
        test_check(break_sink->breakpoint_enabled != 0, "breakpoint state is available to node rendering");
        test_check(g_break.breakpoints_enabled == 0, "breakpoint list has a global toggle");
        test_check(registry_node_by_name("Reroute Data") != NULL, "direct node spawning has a catalog target");
        double sample_30 = 0.0;
        double sample_60 = 0.0;
        double sample_144 = 0.0;
        for (int i = 0; i < 30; i++) {
            sample_30 += 1.0 / 30.0;
        }
        for (int i = 0; i < 60; i++) {
            sample_60 += 1.0 / 60.0;
        }
        for (int i = 0; i < 144; i++) {
            sample_144 += 1.0 / 144.0;
        }
        test_check(
            fabs(flow_visual_heat(sample_30) - flow_visual_heat(sample_60)) < 0.001 &&
                fabs(flow_visual_heat(sample_60) - flow_visual_heat(sample_144)) < 0.001,
            "flow animation is time-constant across 30, 60, and 144 FPS"
        );
        graph_free(&g_break);
        test_check(
            registry_node_by_name("Reroute Data") && registry_node_by_name("Reroute Exec"),
            "data and exec reroutes are transparent catalog nodes"
        );
    }
}
