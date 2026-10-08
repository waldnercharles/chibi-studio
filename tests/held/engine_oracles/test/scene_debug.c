#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "editor/workspace/scene_debug.h"

#include "assets/code_catalog.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"
#include "graph/nodes/component_nodes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static behavior_asset *debug_copy_context(const behavior_asset *source, const code_source_context *context)
{
    char *json = behavior_asset_save(source);
    assert(json);
    behavior_asset *copy = code_catalog_load_with_context(context, json, NULL, 0);
    free(json);
    assert(copy);
    return copy;
}

static behavior_asset *debug_copy(const behavior_asset *source)
{
    return debug_copy_context(source, NULL);
}

static void linked_pin_metadata()
{
    behavior_asset *working = behavior_asset_new("LinkedPinMetadata");
    node *value = graph_add_node(working->graph, registry_node_by_name("Normalize Vector 2"));
    assert(value);
    value->input_pins[0].type_name = "GameVelocity";
    value->output_pins[0].type_name = "GameVelocity";
    behavior_asset *applied = debug_copy(working);
    node *linked = graph_node_by_id(applied->graph, value->id);
    linked->input_pins[0].type_name = "CF_V2";
    linked->output_pins[0].type_name = "CF_V2";
    linked->input_pins[0].group = 2;
    linked->output_pins[0].group = 3;
    assert(scene_debug_matches(working, applied));
    linked->output_pins[0].type_name = "CF_V3";
    assert(!scene_debug_matches(working, applied));
    behavior_asset_free(working);
    behavior_asset_free(applied);
}

/* The runtime runs a linked snapshot: library functions imported, and a Set
 * writing two fields lowered into two setters. The unedited graph opened from
 * that definition runs it; an edit does not. */
static void linked_runtime()
{
    ecs_world *world = ecs_world_new();
    game_preview_world *live = game_preview_world_new(world);
    behavior_asset *library = behavior_asset_new("DebugLibrary");
    library->domain = BEHAVIOR_DOMAIN_LIBRARY;
    graph *value = behavior_asset_add_function(library, "Value", true);
    game_preview_definition *imported = game_preview_definition_new("DebugLibrary", library);
    assert(value && imported && game_preview_add_definition(live, imported) == 0);
    behavior_asset *working = behavior_asset_new("DebugLinked");
    assert(behavior_asset_set_owner_type(working, "ecs.entity") == 0);
    graph *g = working->graph;
    node *begin = graph_add_node(g, &NODE_Event);
    node *set = graph_add_node(g, &NODE_SetComponent);
    int setting = component_node_setting(set);
    pin_value transform = { .type = PIN_NAME, .str = "GameTransform" };
    assert(begin && set && graph_set_event_name(g, begin, "Begin") == 0);
    assert(graph_set_pin_default(g, set, 0, setting, &transform) == 0);
    assert(component_node_split_value(g, set, true) == 0);
    assert(graph_connect_exec(g, begin, 0, set, 0, 0) == 0);
    assert(behavior_asset_add_function_call_node(working, value));
    game_preview_definition *definition = game_preview_definition_new("DebugLinked", working);
    assert(definition && game_preview_add_definition(live, definition) == 0);
    const behavior_asset *libraries[] = { imported->working };
    code_catalog *catalog = code_catalog_from_assets(libraries, 1);
    behavior_asset *open = debug_copy_context(definition->working, code_catalog_context(catalog));
    code_catalog_free(catalog);
    assert(!scene_debug_matches(open, definition->applied));
    assert(scene_debug_runs(live, open, definition->applied));
    node *edited = graph_node_by_id(open->graph, set->id);
    pin_value x = { .type = PIN_F32, .f = 3 };
    assert(graph_set_pin_default(open->graph, edited, 0, edited->input_count - 2, &x) == 0);
    assert(!scene_debug_runs(live, open, definition->applied));
    behavior_asset_free(open);
    game_preview_world_free(live);
    ecs_world_free(world);
}

static void compatibility_cache()
{
    behavior_asset *working = behavior_asset_new("DebugCache");
    assert(behavior_asset_set_owner_type(working, "ecs.entity") == 0);
    graph *g = working->graph;
    node *value = graph_add_node(g, registry_node_by_name("Integer"));
    node *begin = graph_add_node(g, registry_node_by_name("Event"));
    node *delay = graph_add_node(g, registry_node_by_name("Delay"));
    assert(value && begin && delay);
    uint64_t value_id = value->id;
    assert(graph_set_event_name(g, begin, "Begin") == 0);
    assert(graph_set_pin_default(g, delay, 0, 1, &(pin_value){ .type = PIN_F64, .d = 0.1 }) == 0);
    assert(graph_connect_exec(g, begin, 0, delay, 0, 0) == 0);
    behavior_asset *applied = debug_copy(working);
    ecs_world *world = ecs_world_new();
    game_behavior_world *behaviors = game_behavior_world_new(world);
    assert(game_behavior_register_asset(behaviors, "DebugCache", applied) == 0);
    ecs_entity entity = ecs_world_create_entity(world);
    assert(game_behavior_attach(behaviors, entity, "DebugCache") == 0);
    game_behavior_attachment_info info;
    assert(game_behavior_attachment_info_at(behaviors, entity, 0, &info) == 0);
    editor *e = editor_new(working);
    editor_tick(e, 0);
    game_live_world runtime_live = { .behaviors = behaviors };
    game_preview_world live = { .runtime = &runtime_live };
    app_scene_authoring scene = {
        .world = world,
        .live = &live,
        .graph_editor = e,
        .graph_document_kind = APP_EDITOR_DOCUMENT_BEHAVIOR,
        .graph_document_id = "DebugCache",
        .debug_target = APP_RUNTIME_SCENE,
        .debug_sessions = { [APP_RUNTIME_SCENE] = 7 },
    };
    scene_debug_select(&scene.debug, &info, 7);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE && e->inspection_run);

    /* The host pump releases the completed run, which the traced target
     * retains for its values. Inspection must refresh even while semantic
     * compatibility is cached. */
    assert(game_behavior_pump(behaviors, 0.2) >= FLOW_OK);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE && !e->inspection_run);
    int retained = 0;
    behavior_instance_retained_runs(info.instance, &retained);
    assert(retained == 1);

    /* A UI mutation happens before editor_tick advances the revision. */
    unsigned revision = editor_edit_revision(e);
    pin_value changed = { .type = PIN_I32, .i = 17 };
    assert(
        editor_command_apply(
            e,
            (editor_command){
                .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                .data.set_pin_default = { .node_id = value_id, .value = &changed },
            }
        ) == 0
    );
    assert(editor_edit_revision(e) == revision && editor_has_pending_edits(e));
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_STALE && !e->inspection_instance);
    editor_tick(e, 0.2);
    assert(editor_edit_revision(e) != revision && !editor_has_pending_edits(e));
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_STALE);

    editor_undo(e);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);
    editor_tick(e, 0.3);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);

    /* The same snapshot path used by node dragging records an Undoable layout edit. */
    ed_snapshot_before(e);
    value = graph_node_by_id(editor_active_graph(e), value_id);
    value->x += 140;
    value->y -= 80;
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);
    editor_tick(e, 0.4);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);
    editor_undo(e);
    editor_tick(e, 0.5);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);

    /* Hot apply preserves the attachment while replacing its immutable definition. */
    behavior_asset *replacement = debug_copy(editor_behavior_asset(e));
    value = graph_node_by_id(replacement->graph, value_id);
    assert(graph_set_pin_default(replacement->graph, value, 0, 0, &changed) == 0);
    behavior_asset *old = NULL;
    assert(
        game_behavior_preview_asset(behaviors, "DebugCache", replacement, &old) == INSTANCE_UPDATE_READY
    );
    behavior_asset_free(old);
    applied = replacement;
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_STALE);
    replacement = debug_copy(editor_behavior_asset(e));
    assert(
        game_behavior_preview_asset(behaviors, "DebugCache", replacement, &old) == INSTANCE_UPDATE_READY
    );
    behavior_asset_free(old);
    applied = replacement;
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);

    scene.debug_sessions[APP_RUNTIME_SCENE] = 8;
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_UNAVAILABLE && !e->inspection_instance);
    assert(game_behavior_attachment_info_at(behaviors, entity, 0, &info) == 0);
    scene_debug_select(&scene.debug, &info, 8);
    assert(!scene.debug.comparison.valid);
    sync_editor_inspection(&scene);
    assert(scene.debug_status == APP_DEBUG_AVAILABLE);

    scene_debug_clear(&scene.debug);
    editor_set_inspection_blocked(e, true);
    editor_free(e);
    game_behavior_world_free(behaviors);
    behavior_asset_free(applied);
    ecs_world_free(world);
}

void test_scene_debug()
{
    linked_pin_metadata();
    linked_runtime();
    compatibility_cache();
    ecs_world *world = ecs_world_new();
    game_behavior_world *behaviors = game_behavior_world_new(world);
    behavior_asset *asset = behavior_asset_new("DebugIdentity");
    assert(behavior_asset_set_owner_type(asset, "ecs.entity") == 0);
    assert(game_behavior_register_asset(behaviors, "DebugIdentity", asset) == 0);
    ecs_entity first = ecs_world_create_entity(world);
    ecs_entity second = ecs_world_create_entity(world);
    assert(game_behavior_attach(behaviors, first, "DebugIdentity") == 0);
    assert(game_behavior_attach(behaviors, second, "DebugIdentity") == 0);
    game_behavior_attachment_info info, resolved;
    assert(game_behavior_attachment_info_at(behaviors, first, 0, &info) == 0);
    scene_debug debug = { 0 };
    scene_debug_select(&debug, &info, 7);
    assert(scene_debug_resolve(&debug, world, behaviors, 7, &resolved));
    assert(ecs_entity_to_u64(resolved.entity) == ecs_entity_to_u64(first));
    assert(!scene_debug_resolve(&debug, world, behaviors, 8, &resolved));
    assert(game_behavior_detach_asset(behaviors, first, "DebugIdentity") == 1);
    assert(!scene_debug_resolve(&debug, world, behaviors, 7, &resolved));
    assert(game_behavior_attach(behaviors, first, "DebugIdentity") == 0);
    assert(!scene_debug_resolve(&debug, world, behaviors, 7, &resolved));
    assert(game_behavior_attachment_info_at(behaviors, first, 0, &info) == 0);
    scene_debug_select(&debug, &info, 7);
    assert(scene_debug_resolve(&debug, world, behaviors, 7, &resolved));
    assert(game_behavior_detach_asset(behaviors, first, "DebugIdentity") == 1);
    assert(ecs_world_destroy_entity(world, first) == ECS_OK);
    ecs_entity replacement = ecs_world_create_entity(world);
    assert(ecs_entity_to_u64(replacement) != ecs_entity_to_u64(first));
    assert(game_behavior_attach(behaviors, replacement, "DebugIdentity") == 0);
    assert(!scene_debug_resolve(&debug, world, behaviors, 7, &resolved));
    scene_debug_clear(&debug);
    game_behavior_world_free(behaviors);
    behavior_asset_free(asset);
    ecs_world_free(world);

    behavior_asset *working = behavior_asset_new("DebugRevision");
    node *value = graph_add_node(working->graph, registry_node_by_name("Integer"));
    assert(value);
    char *json = behavior_asset_save(working);
    behavior_asset *applied = behavior_asset_load(json);
    free(json);
    assert(applied && scene_debug_matches(working, applied));
    node *grouped = graph_node_by_id(applied->graph, value->id);
    grouped->output_pins[0].group = 3;
    assert(scene_debug_matches(working, applied));
    value->x = 140;
    value->y = -80;
    assert(scene_debug_matches(working, applied));
    pin_value changed = { .type = PIN_I32, .i = 17 };
    assert(graph_set_pin_default(working->graph, value, 0, 0, &changed) == 0);
    assert(!scene_debug_matches(working, applied));
    node *runtime_value = graph_node_by_id(applied->graph, value->id);
    assert(graph_set_pin_default(applied->graph, runtime_value, 0, 0, &changed) == 0);
    assert(scene_debug_matches(working, applied));
    changed.i = 18;
    assert(graph_set_pin_default(applied->graph, runtime_value, 0, 0, &changed) == 0);
    assert(!scene_debug_matches(working, applied));
    behavior_asset_free(working);
    behavior_asset_free(applied);
    scene_debug_clear(&debug);
    puts("scene debug PASS: instance identity, session, detach/reattach and entity reuse; semantic edits invalidate while layout changes remain compatible");
}
