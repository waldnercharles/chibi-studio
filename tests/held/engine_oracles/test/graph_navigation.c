#include "graph/authoring/asset_edit.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "graph/model/behavior_asset.h"
#include "graph/model/registry.h"
#include "support/array.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    behavior_asset *asset;
    uint64_t root;
    uint64_t function;
    uint64_t pure;
    uint64_t macro;
    uint64_t root_call;
    uint64_t nested_call;
    uint64_t macro_call;
    uint64_t ordinary;
} navigation_fixture;

static navigation_fixture fixture()
{
    navigation_fixture f = { .asset = behavior_asset_new("Navigation") };
    graph *function = behavior_asset_add_function(f.asset, "Work", 0);
    graph *pure = behavior_asset_add_function(f.asset, "Value", 1);
    graph *macro = behavior_asset_add_macro(f.asset, "Sequence");
    assert(function && pure && macro);
    f.root = f.asset->graph->graph_id;
    f.function = function->graph_id;
    f.pure = pure->graph_id;
    f.macro = macro->graph_id;
    node *root_call = behavior_asset_add_function_call_node(f.asset, function);
    node *nested = behavior_asset_add_function_call_node_to_graph(f.asset, function, pure);
    node *macro_call = behavior_asset_add_macro_call_node(f.asset, macro);
    node *ordinary = graph_add_node(f.asset->graph, registry_node_by_name("Integer"));
    assert(root_call && nested && macro_call && ordinary);
    root_call->x = 231;
    root_call->y = -83;
    nested->x = -120;
    nested->y = 350;
    f.root_call = root_call->id;
    f.nested_call = nested->id;
    f.macro_call = macro_call->id;
    f.ordinary = ordinary->id;
    return f;
}

static void expect_graph(editor *editor, uint64_t id, bool back)
{
    editor_query state;
    editor_query_state(editor, &state);
    assert(state.active_graph && state.active_graph->graph_id == id);
    assert((editor_graph_history_count(editor) > 0) == back);
}

static void expect_position(editor *editor, uint64_t id, float x, float y)
{
    node *call = graph_node_by_id(editor_active_graph(editor), id);
    assert(call && call->x == x && call->y == y);
}

void test_graph_navigation()
{
    navigation_fixture f = fixture();
    editor *editor = editor_new(f.asset);
    assert(editor);
    expect_graph(editor, f.root, false);
    assert(editor_graph_back(editor) == -1);
    assert(editor_open_graph_call(editor, UINT64_MAX) == -1);
    assert(editor_open_graph_call(editor, f.ordinary) == -1);
    node *root_call = graph_node_by_id(f.asset->graph, f.root_call);
    char title[256];
    assert(graph_set_node_reference(f.asset->graph, root_call, f.function, "Wrong function") == 0);
    assert(strcmp(ed_node_title(editor, root_call, title, sizeof title), "Work") == 0);
    node *macro_call = graph_node_by_id(f.asset->graph, f.macro_call);
    assert(graph_set_node_reference(f.asset->graph, macro_call, f.macro, "Wrong macro") == 0);
    assert(strcmp(ed_node_title(editor, macro_call, title, sizeof title), "Sequence") == 0);
    node *pure_call = graph_node_by_id(behavior_asset_find_graph(f.asset, f.function), f.nested_call);
    assert(
        graph_set_node_reference(behavior_asset_find_graph(f.asset, f.function), pure_call, f.pure, "Wrong pure function") ==
        0
    );
    assert(strcmp(ed_node_title(editor, pure_call, title, sizeof title), "Value") == 0);
    behavior_asset *library = behavior_asset_new("Shared");
    library->domain = BEHAVIOR_DOMAIN_LIBRARY;
    graph *shared = behavior_asset_add_function(library, "Shared Value", 1);
    node *shared_call = behavior_asset_add_function_call_node(f.asset, shared);
    assert(shared_call);
    const behavior_asset *catalog[] = { library };
    editor_set_asset_catalog(editor, catalog, 1);
    assert(behavior_asset_rename_graph(library, shared->graph_id, "Renamed Value") == 0);
    assert(
        strcmp(ed_node_title(editor, shared_call, title, sizeof title), "Shared.Renamed Value") == 0
    );
    graph_remove_node(f.asset->graph, shared_call);
    editor_set_asset_catalog(editor, NULL, 0);
    behavior_asset_free(library);
    root_call->reference_id = UINT64_MAX;
    assert(editor_open_graph_call(editor, f.root_call) == -1);
    root_call->reference_id = f.function;
    expect_graph(editor, f.root, false);
    assert(editor_open_graph_call(editor, f.root_call) == 0);
    expect_graph(editor, f.function, true);
    node *nested = graph_node_by_id(behavior_asset_find_graph(f.asset, f.function), f.nested_call);
    nested->reference_id = f.function;
    assert(editor_open_graph_call(editor, f.nested_call) == -1);
    nested->reference_id = f.pure;
    expect_graph(editor, f.function, true);
    assert(editor_open_graph_call(editor, f.nested_call) == 0);
    expect_graph(editor, f.pure, true);
    assert(editor_open_graph(editor, f.pure) == 0);
    expect_graph(editor, f.pure, true);
    assert(editor_graph_history_count(editor) == 2);
    assert(editor_graph_back(editor) == 0);
    expect_graph(editor, f.function, true);
    assert(editor_graph_history_count(editor) == 1);
    expect_position(editor, f.nested_call, -120, 350);
    assert(editor_graph_back(editor) == 0);
    expect_graph(editor, f.root, false);
    assert(editor_graph_history_count(editor) == 0);
    expect_position(editor, f.root_call, 231, -83);
    assert(editor_open_graph_call(editor, f.root_call) == 0);
    assert(editor_open_graph_call(editor, f.nested_call) == 0);
    assert(editor_graph_back(editor) == 0);
    expect_graph(editor, f.function, true);
    expect_position(editor, f.nested_call, -120, 350);
    assert(editor_graph_back(editor) == 0);
    expect_graph(editor, f.root, false);
    expect_position(editor, f.root_call, 231, -83);
    assert(editor_open_graph_call(editor, f.macro_call) == 0);
    expect_graph(editor, f.macro, true);
    assert(editor_open_graph(editor, f.function) == 0);
    expect_graph(editor, f.function, false);
    assert(editor_graph_back(editor) == -1);

    /* Back and Forward return to a graph with the calls that reached it. */
    assert(editor_open_graph(editor, f.root) == 0);
    assert(editor_open_graph_call(editor, f.root_call) == 0);
    assert(editor_open_graph_call(editor, f.nested_call) == 0);
    ed_graph_caller trail[2];
    assert(acount(editor->graph_callers) == 2);
    memcpy(trail, editor->graph_callers, sizeof trail);
    assert(editor_open_graph(editor, f.root) == 0);
    assert(editor_open_graph(editor, f.pure) == 0);
    expect_graph(editor, f.pure, false);
    ed_restore_callers(editor, trail, 2);
    expect_graph(editor, f.pure, true);
    assert(editor_graph_history_count(editor) == 2);
    assert(editor->graph_callers[0].graph_id == f.root);
    assert(editor->graph_callers[1].graph_id == f.function);
    /* A call that no longer leads on drops out with those before it. */
    trail[0].node_id = f.ordinary;
    ed_restore_callers(editor, trail, 2);
    assert(editor_graph_history_count(editor) == 1);
    assert(editor->graph_callers[0].graph_id == f.function);
    assert(editor_graph_back(editor) == 0);
    expect_graph(editor, f.function, false);

    assert(editor_open_graph(editor, f.root) == 0);
    assert(editor_open_graph_call(editor, f.root_call) == 0);
    assert(editor_open_graph_call(editor, f.nested_call) == 0);
    assert(behavior_asset_delete_graph(f.asset, f.function) == 0);
    assert(editor_graph_back(editor) == 0);
    expect_graph(editor, f.root, false);
    expect_position(editor, f.root_call, 231, -83);

    assert(editor_open_graph_call(editor, f.macro_call) == 0);
    navigation_fixture replacement = fixture();
    assert(replacement.root == f.root && replacement.macro == f.macro);
    assert(editor_open_behavior_asset(editor, replacement.asset, replacement.macro) == 0);
    expect_graph(editor, replacement.macro, false);
    assert(editor_graph_back(editor) == -1);
    behavior_asset_free(replacement.asset);
    editor_free(editor);
    puts("PASS: graph calls support nested Back, reject invalid targets, preserve positions and clear history on document replacement");
}
