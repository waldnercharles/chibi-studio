#include "authoring/engine_preview.h"
#include "graph/authoring/graph_edit.h"
#include "editor/collab/workspace.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/input.h"
#include "graph/model/registry.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "runtime/component_catalog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

static editor *restore_selection(editor *source)
{
    char *json = editor_checkpoint(source);
    editor *candidate = editor_new(behavior_asset_new("Selection candidate"));
    assert(editor_restore_checkpoint(candidate, json));
    free(json);
    return candidate;
}

static void link_checkpoint()
{
    behavior_asset *asset = behavior_asset_new("Selections");
    node *literal = graph_add_node(asset->graph, registry_node_by_name("Integer"));
    node *relay = graph_add_node(asset->graph, registry_node_by_name("Reroute Data"));
    assert(graph_connect_data(asset->graph, literal, 0, relay, 0) == 0);
    const edge *wire = &asset->graph->edges[0];
    ed_link_ref link = { wire->from_id, wire->from_pin_id, wire->to_id, wire->to_pin_id };
    uint64_t graph_id = asset->graph->graph_id;
    editor *source = editor_new(asset);
    ed_add_selected_link(source, &link);
    editor *linked = restore_selection(source);
    assert(acount(linked->selected_links) == 1);
    assert(ed_edge_index(linked, &linked->selected_links[0]) == 0);
    behavior_asset *other = behavior_asset_new("Other selection document");
    assert(editor_visit_behavior_asset(linked, other, other->graph->graph_id, "other") == 0);
    editor *cached = restore_selection(linked);
    assert(editor_visit_behavior_asset(cached, NULL, graph_id, "Selections") == 0);
    assert(
        acount(cached->selected_links) == 1 &&
        ed_edge_index(cached, &cached->selected_links[0]) == 0
    );
    editor_free(cached);
    editor_free(linked);
    editor_free(source);
    behavior_asset_free(other);
}

static uint64_t selected_id(const editor *e)
{
    assert(acount(e->selected) == 1);
    return e->selected[0]->id;
}

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    app_scene_authoring *scene;
} workspace_fixture;

static workspace_fixture workspace_fixture_new()
{
    workspace_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new(
        "CollabWorkspaceFixture",
        behavior_asset_new("CollabWorkspaceFixture")
    );
    assert(definition && game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("CollabWorkspaceFixture"));
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    f.scene = app_scene_authoring_new(
        f.world,
        f.live,
        definition,
        editor_behavior_asset(f.graph),
        f.graph,
        NULL,
        NULL,
        f.runtime
    );
    assert(f.scene);
    return f;
}

static void workspace_fixture_free(workspace_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
}

static char *workspace_json(const char *scene, const char *graph)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", 1);
    yyjson_mut_obj_add_str(doc, root, "project", "fixture-project");
    yyjson_mut_obj_add_str(doc, root, "scene", scene);
    yyjson_mut_obj_add_str(doc, root, "graph", graph);
    yyjson_mut_obj_add_str(doc, root, "presentation", "{}");
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static void workspace_checkpoint_restore()
{
    workspace_fixture source = workspace_fixture_new();
    assert(editor_spawn(source.graph, registry_node_by_name("Integer"), 17, 23));
    char *scene = app_scene_authoring_checkpoint(source.scene);
    char *graph = editor_checkpoint(source.graph);
    assert(scene && graph);
    char *workspace = workspace_json(scene, graph);
    assert(workspace);

    workspace_fixture candidate = workspace_fixture_new();
    char error[128] = { 0 };
    assert(
        !collab_workspace_restore_model(candidate.scene, candidate.graph, "{\"version\":999}", error, sizeof error)
    );
    assert(acount(editor_active_graph(candidate.graph)->nodes) == 0);
    assert(
        collab_workspace_restore_model(candidate.scene, candidate.graph, workspace, error, sizeof error)
    );
    assert(acount(editor_active_graph(candidate.graph)->nodes) == 1);

    free(scene);
    free(graph);
    free(workspace);
    workspace_fixture_free(&source);
    workspace_fixture_free(&candidate);
}

void test_editor_checkpoint()
{
    workspace_checkpoint_restore();
    link_checkpoint();
    editor *source = editor_new(behavior_asset_new("Checkpoint A"));
    const node_desc *integer = registry_node_by_name("Integer");
    node *first = editor_spawn(source, integer, 12, 23);
    assert(first);
    uint64_t first_id = first->id;
    uint64_t graph_a = editor_active_graph(source)->graph_id;
    assert(editor_spawn(source, integer, 50, 60));
    editor_undo(source);
    ed_select_node(source, graph_node_by_id(source->active_graph, first_id), 0);
    behavior_asset *other = behavior_asset_new("Checkpoint B");
    uint64_t graph_b = other->graph->graph_id;
    assert(editor_visit_behavior_asset(source, other, graph_b, "B") == 0);
    node *selected = editor_spawn(source, integer, -10, 14);
    uint64_t second_id = selected->id;
    ed_select_node(source, selected, 0);
    char *checkpoint = editor_checkpoint(source);
    for (int iteration = 0; iteration < 3; iteration++) {
        editor *restored = editor_new(behavior_asset_new("Candidate"));
        assert(!editor_restore_checkpoint(restored, "{broken"));
        assert(!editor_restore_checkpoint(restored, "{\"version\":999}"));
        assert(editor_restore_checkpoint(restored, checkpoint));
        assert(editor_active_graph(restored)->graph_id == graph_b);
        assert(selected_id(restored) == second_id);
        editor_query query;
        editor_query_state(restored, &query);
        assert(query.dirty);
        editor_undo(restored);
        assert(acount(editor_active_graph(restored)->nodes) == 0);
        editor_query_state(restored, &query);
        assert(!query.dirty);
        editor_redo(restored);
        assert(graph_node_by_id(editor_active_graph(restored), second_id));
        assert(editor_visit_behavior_asset(restored, NULL, graph_a, "Checkpoint A") == 0);
        assert(selected_id(restored) == first_id);
        assert(acount(editor_active_graph(restored)->nodes) == 1);
        assert(graph_node_by_id(editor_active_graph(restored), first_id));
        editor_redo(restored);
        assert(acount(editor_active_graph(restored)->nodes) == 2);
        editor_undo(restored);
        editor_undo(restored);
        editor_query_state(restored, &query);
        assert(!query.dirty);
        editor_free(restored);
    }
    free(checkpoint);
    editor_free(source);
    behavior_asset_free(other);
    printf("PASS: editor checkpoints preserve active and cached documents, baselines, Undo/Redo and node/link selection\n");
}

void test_editor_checkpoint_with_app(const char *program)
{
    CF_Result result = cf_make_app(
        "Editor checkpoint",
        0,
        0,
        0,
        256,
        256,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "editor checkpoint app: %s\n", result.details ? result.details : "unknown error");
        assert(false);
    }
    app_scene_renderer_initialize();
    app_input_initialize();
    test_editor_checkpoint();
    app_input_shutdown();
    cf_destroy_app();
}
