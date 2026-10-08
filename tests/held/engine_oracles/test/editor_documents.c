#include "graph/authoring/asset_edit.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"
#include "graph/nodes/behavior_expression.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reload_keeps_document_identity()
{
    behavior_asset *initial = behavior_asset_new("Reload");
    behavior_asset *disk = behavior_asset_new("Reload");
    behavior_asset *other = behavior_asset_new("Other");
    behavior_asset_set_description(disk, "Saved graph");
    editor *e = editor_new(initial);
    assert(editor_spawn(e, registry_node_by_name("Integer"), 0, 0));
    uint64_t graph_id = disk->graph->graph_id;
    assert(editor_reload_behavior_asset(e, disk, graph_id, "Reload") == 0);
    assert(strcmp(editor_behavior_asset(e)->description, "Saved graph") == 0);
    editor_undo(e);
    assert(acount(editor_active_graph(e)->nodes) == 0);
    assert(editor_spawn(e, registry_node_by_name("Integer"), 0, 0));
    assert(editor_visit_behavior_asset(e, other, other->graph->graph_id, "Other") == 0);
    assert(editor_visit_behavior_asset(e, disk, graph_id, "Reload") == 0);
    assert(acount(editor_active_graph(e)->nodes) == 1);
    editor_undo(e);
    assert(acount(editor_active_graph(e)->nodes) == 0);
    editor_redo(e);
    assert(acount(editor_active_graph(e)->nodes) == 1);
    behavior_asset_set_description((behavior_asset *)editor_behavior_asset(e), "Unsaved replacement");
    assert(editor_reload_behavior_asset(e, disk, graph_id, "Reload") == 0);
    assert(strcmp(editor_behavior_asset(e)->description, "Saved graph") == 0);
    assert(acount(editor_active_graph(e)->nodes) == 0);
    editor_free(e);
    behavior_asset_free(disk);
    behavior_asset_free(other);
    puts("editor documents PASS: explicit Reload replaces cached data and subsequent edits retain independent Undo/Redo");
}

static void nested_callers_survive_document_visits()
{
    behavior_asset *asset = behavior_asset_new("Callers");
    graph *outer = behavior_asset_add_function(asset, "Outer", 0);
    graph *inner = behavior_asset_add_function(asset, "Inner", 0);
    node *root_call = behavior_asset_add_function_call_node(asset, outer);
    node *inner_call = behavior_asset_add_function_call_node_to_graph(asset, outer, inner);
    node *value = graph_add_node(inner, registry_node_by_name("Integer"));
    assert(root_call && inner_call && value);
    uint64_t root_id = asset->graph->graph_id;
    uint64_t outer_id = outer->graph_id;
    uint64_t inner_id = inner->graph_id;
    uint64_t root_call_id = root_call->id;
    uint64_t inner_call_id = inner_call->id;
    uint64_t value_id = value->id;
    char *json = behavior_asset_save(asset);
    behavior_asset *source = behavior_asset_load(json);
    free(json);
    editor *e = editor_new(asset);
    assert(editor_open_graph_call(e, root_call_id) == 0);
    assert(editor_open_graph_call(e, inner_call_id) == 0);
    ed_select_node(e, graph_node_by_id(e->active_graph, value_id), 0);
    behavior_asset *other = behavior_asset_new("OtherCallers");
    assert(editor_visit_behavior_asset(e, other, other->graph->graph_id, "OtherCallers") == 0);
    assert(editor_graph_history_count(e) == 0);
    assert(editor_visit_behavior_asset(e, source, inner_id, "Callers") == 0);
    assert(editor_active_graph(e)->graph_id == inner_id);
    assert(editor_graph_history_count(e) == 2);
    assert(acount(e->selected) == 1 && e->selected[0]->id == value_id);
    assert(editor_graph_back(e) == 0);
    assert(editor_active_graph(e)->graph_id == outer_id);
    assert(acount(e->selected) == 1 && e->selected[0]->id == inner_call_id);
    assert(editor_graph_back(e) == 0);
    assert(editor_active_graph(e)->graph_id == root_id);
    assert(editor_graph_back(e) == -1);
    editor_free(e);
    behavior_asset_free(source);
    behavior_asset_free(other);
    puts("editor documents PASS: nested caller ancestry and selection survive document visits");
}

/* Studio compiles each document it shows, refreshing expressions whose
   reference inputs are bound. A current expression keeps its IDs, so the
   compile leaves an unedited document as it was saved. */
static void compile_keeps_unedited_document()
{
    behavior_asset *authored = behavior_asset_new("Compiled");
    pin_value initial = { .type = PIN_F64 };
    behavior_variable *position = behavior_asset_add_variable(
        authored,
        "position",
        PIN_F64,
        "double",
        PIN_CONTAINER_SINGLE,
        &initial,
        NULL
    );
    node *call = behavior_expression_add(authored, authored->graph, "position += 1", NULL);
    assert(position && call);
    int target = 0;
    while (target < call->input_count &&
           !(call->input_pins[target].flags & PIN_FLAG_REFERENCE)) {
        target++;
    }
    assert(target < call->input_count);
    pin_binding binding = { .kind = PIN_BINDING_VARIABLE,
                            .variable_id = position->id,
                            .source_asset = authored->name };
    assert(graph_set_pin_binding(authored->graph, call, 0, target, &binding) == 0);
    assert(behavior_expression_refresh(authored, call, NULL) == 0);
    char *saved = behavior_asset_save(authored);
    behavior_asset_free(authored);
    editor *e = editor_new(behavior_asset_load(saved));
    assert(editor_command_apply(e, (editor_command){ .kind = EDITOR_COMMAND_COMPILE }) == 0);
    char *compiled = behavior_asset_save(editor_behavior_asset(e));
    assert(!strcmp(compiled, saved));
    free(compiled);
    free(saved);
    editor_free(e);
    puts("editor documents PASS: compiling an unedited document leaves it as saved");
}

/* Reordering pins preserves the selected document and scene context, and
   closing a protected tab cannot discard a dirty source buffer. */
static void document_tab_pins()
{
    app_scene_authoring scene = { .active_tab = 2, .current_scene_tab_key = "scene:a" };
    apush(scene.tabs, ((scene_document_tab){ .kind = APP_EDITOR_DOCUMENT_SCENE, .key = "scene:a" }));
    apush(scene.tabs, ((scene_document_tab){ .kind = APP_EDITOR_DOCUMENT_UI, .key = "ui:b" }));
    apush(
        scene.tabs,
        ((scene_document_tab){ .kind = APP_EDITOR_DOCUMENT_UI, .key = "ui:c", .source_dirty = true })
    );
    scene_tabs_toggle_pin(&scene, 2);
    assert(scene.active_tab == 0 && !strcmp(scene.tabs[0].key, "ui:c"));
    assert(!strcmp(scene_tabs_current_scene_key(&scene), "scene:a"));
    assert(scene_tabs_discard(&scene, 0) == -1 && scene.tabs[0].source_dirty);
    scene_tabs_toggle_pin(&scene, 2);
    assert(!strcmp(scene.tabs[0].key, "ui:c") && !strcmp(scene.tabs[1].key, "ui:b"));
    assert(scene.active_tab == 0);
    scene_tabs_toggle_pin(&scene, 0);
    assert(!strcmp(scene.tabs[0].key, "ui:b") && !strcmp(scene.tabs[1].key, "ui:c"));
    assert(scene.active_tab == 1 && scene_tabs_can_close(&scene, 1));
    assert(!scene_tabs_can_close(&scene, 2));
    apush(scene.tabs, ((scene_document_tab){ .kind = APP_EDITOR_DOCUMENT_SCENE, .key = "scene:d" }));
    assert(scene_tabs_can_close(&scene, 2) && scene_tabs_can_close(&scene, 3));
    afree(scene.tabs);
    puts("editor documents PASS: pins retain document identity and prevent dirty or last-scene closure");
}

/* An asset disappearing invalidates its tab even when ordinary close is
   protected by a pin. The background removal keeps the scene selected. */
static void disappearing_asset_forgets_pinned_tab()
{
    app_scene_authoring scene = { .active_tab = 0,
                                  .current_scene_tab_key = "scene:root" };
    apush(scene.tabs, ((scene_document_tab){ .kind = APP_EDITOR_DOCUMENT_SCENE, .key = "scene:root" }));
    const app_editor_document_kind kinds[] = { APP_EDITOR_DOCUMENT_BEHAVIOR,
                                               APP_EDITOR_DOCUMENT_PROTOTYPE };
    const char *keys[] = { "behavior:Deleted", "prototype:Deleted" };
    for (int i = 0; i < 2; i++) {
        apush(
            scene.tabs,
            ((scene_document_tab){ .kind = kinds[i],
                                   .key = xstrdup(keys[i]),
                                   .label = xstrdup("Deleted"),
                                   .pinned = true })
        );
        scene_tabs_close(&scene, kinds[i], "Deleted");
        assert(acount(scene.tabs) == 2 && scene.tabs[1].pinned);
        scene_tabs_forget(&scene, kinds[i], "Deleted");
        assert(acount(scene.tabs) == 1 && scene.active_tab == 0);
        assert(!strcmp(scene_tabs_current_scene_key(&scene), "scene:root"));
    }
    afree(scene.tabs);
    puts("editor documents PASS: disappearing behavior and prototype assets remove pinned tabs");
}

void test_editor_documents()
{
    document_tab_pins();
    disappearing_asset_forgets_pinned_tab();
    reload_keeps_document_identity();
    nested_callers_survive_document_visits();
    compile_keeps_unedited_document();
}
