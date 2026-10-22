#include "authoring/engine_preview.h"
/* The graph's debug target while the game runs (Unreal's debug object): it
 * follows the selection to that object's instance of the open behavior, a
 * spawned object's included. Only that instance records activity, and none
 * does while no graph shows it or No target is chosen, which lasts until the
 * selection changes. Short-lived spawns: Follow newest, a destroyed target's
 * frozen data, Break when spawned and playback speed. */
#include "authoring/source_prepare.h"
#include "authoring/source_presentation.h"
#include "editor/workspace/scene_internal.h"
#include "platform/cute.h"
#include "render/shader_catalog.h"
#include "runtime/component_catalog.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <stdio.h>

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    game_preview_definition *placeholder;
    app_scene_authoring *scene;
    double time;
    bool owns_file_system; /* content's image materials read through it */
} debug_fixture;

static debug_fixture fixture_new()
{
    debug_fixture f = { .world = ecs_world_new(),
                        .owns_file_system = !cf_fs_get_base_directory() };
    if (f.owns_file_system) {
        assert(!cf_is_error(cf_fs_init(NULL)));
        render_shader_catalog_clear();
    }
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    const script_catalog catalog = { 0 };
    const script_lower_bindings bindings = { 0 };
    authoring_source *source = authoring_source_prepare_document(
        "DebugTarget.chibi",
        "behaviors/DebugTarget.chibi",
        "behavior DebugTarget {}",
        &catalog,
        &bindings,
        BEHAVIOR_BYTECODE
    );
    assert(source->valid);
    authoring_source_presentation
        *presentation = authoring_source_presentation_new(source->analysis, &catalog, NULL);
    assert(presentation);
    game_preview_definition *definition = game_preview_projection_new("DebugTarget", presentation);
    f.placeholder = definition;
    f.graph = editor_new(authoring_source_presentation_take_view(
        authoring_source_presentation_clone(presentation)
    ));
    assert(editor_source_presentation(f.graph, presentation) == 0);
    authoring_source_presentation_free(presentation);
    authoring_source_free(source);
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
    assert(app_scene_authoring_set_content_root(f.scene, "content") == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(app_scene_authoring_load(f.scene, "content/scenes/Arena3D.chibi") == 0);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    return f;
}

static void fixture_free(debug_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    game_preview_definition_free(f->placeholder);
    ecs_world_free(f->world);
    if (f->owns_file_system) {
        render_shader_catalog_clear();
        cf_fs_destroy();
    }
}

static void steps(debug_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        f->time += ENGINE_FIXED_SECONDS;
        assert(app_scene_authoring_frame(f->scene, f->time) >= 0);
    }
}

static bool same_entity(ecs_entity a, ecs_entity b)
{
    return ecs_entity_to_u64(a) == ecs_entity_to_u64(b);
}

static ecs_entity first_with(ecs_world *world, ecs_component_id component)
{
    ecs_query query;
    assert(ecs_query_begin(world, &query, &component, 1) == ECS_OK);
    ecs_entity entity = { 0 };
    ecs_query_next(&query, &entity);
    ecs_query_end(&query);
    return entity;
}

static int traced(game_source_registry *sources)
{
    int count = 0;
    for (int i = 0; i < game_source_attachment_count(sources); i++) {
        game_source_attachment_info info = game_source_attachment_at(sources, i);
        execution_inspector *inspector = execution_program_inspection(info.program);
        count += inspector &&
                 execution_inspector_records(inspector, ecs_entity_to_u64(info.entity));
    }
    return count;
}

static bool recorded(const execution_inspector *inspector, ecs_entity owner)
{
    for (int i = 0; inspector && i < execution_inspector_activity_count(inspector);
         i++) {
        execution_debug_activity row = execution_inspector_activity_at(inspector, i);
        if (row.owner == ecs_entity_to_u64(owner) && row.count) {
            return true;
        }
    }
    return false;
}

/* Runs the game until the target is another instance than `sequence`'s. */
static void until_retargeted(debug_fixture *f, uint64_t sequence)
{
    for (int i = 0; i < 240 && f->scene->debug.sequence == sequence; i++) {
        steps(f, 1);
        sync_editor_inspection(f->scene);
    }
}

static bool projectile(debug_fixture *f, ecs_entity entity)
{
    return ecs_component_has(f->world, entity, GAME_COMPONENT_GameProjectile.id);
}

/* Short-lived spawns: Follow newest (Unreal's debug filter), a destroyed
   target's frozen data, Break when spawned, and playback speed. */
static void spawned_targets()
{
    debug_fixture f = fixture_new();
    game_source_registry *behaviors = f.live->runtime->sources;
    scene_debug_attachment retired;
    assert(app_scene_authoring_open_behavior(f.scene, "ProjectileBehavior") == 0);
    app_scene_authoring_follow_newest(f.scene, "ArenaProjectile");
    test_check(f.scene->debug_status == APP_DEBUG_NONE && f.scene->debug_newest, "Follow newest waits for the first spawn");
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) >= 0);
    until_retargeted(&f, 0);
    uint64_t first = f.scene->debug.sequence;
    assert(
        f.scene->debug_status == APP_DEBUG_AVAILABLE &&
        projectile(&f, f.scene->debug.entity)
    );
    until_retargeted(&f, first);
    test_check(
        f.scene->debug.sequence > first && projectile(&f, f.scene->debug.entity) &&
            f.scene->debug_status == APP_DEBUG_AVAILABLE,
        "Follow newest moves the target to each projectile the game spawns"
    );

    /* The last shot dies; the graph keeps what it recorded. */
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, false) >= 0);
    steps(&f, 2);
    sync_editor_inspection(f.scene);
    ecs_entity last = f.scene->debug.entity;
    execution_inspector *instance = execution_inspector_retain(f.scene->debug.inspector);
    assert(instance && execution_inspector_records(instance, ecs_entity_to_u64(last)));
    for (int i = 0; i < 240 && ecs_entity_is_alive(f.world, last); i++) {
        steps(&f, 1);
    }
    assert(!ecs_entity_is_alive(f.world, last));
    sync_editor_inspection(f.scene);
    test_check(
        f.scene->debug_destroyed && f.scene->debug_status == APP_DEBUG_AVAILABLE &&
            same_entity(f.scene->debug.entity, last) && recorded(instance, last) &&
            scene_debug_retired(&f.scene->debug, f.scene->debug.session, &retired) &&
            ecs_entity_to_u64(retired.entity) == ecs_entity_to_u64(last) &&
            traced(behaviors) == 0 &&
            !strcmp(f.scene->debug.prototype, "ArenaProjectile"),
        "a destroyed target keeps its recorded activity, frozen, and its name"
    );
    steps(&f, 5);
    sync_editor_inspection(f.scene);
    assert(f.scene->debug_destroyed && same_entity(f.scene->debug.entity, last));

    execution_debug_cursor frozen;
    assert(execution_inspector_owner_cursor(instance, ecs_entity_to_u64(last), &frozen));
    assert(frozen.retired);

    /* The next shot replaces it. */
    uint64_t destroyed = f.scene->debug.sequence;
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) >= 0);
    until_retargeted(&f, destroyed);
    test_check(
        !f.scene->debug_destroyed && f.scene->debug.sequence > destroyed &&
            !same_entity(f.scene->debug.entity, last),
        "Follow newest moves on to the next spawn and lets the destroyed one go"
    );

    execution_debug_cursor retained;
    assert(execution_inspector_owner_cursor(instance, ecs_entity_to_u64(last), &retained));
    assert(
        retained.retired && retained.run == frozen.run &&
        retained.step == frozen.step
    );
    execution_inspector_free(instance);

    /* Break when spawned pauses with the new shot selected and targeted. */
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, false) >= 0);
    steps(&f, 2);
    assert(app_scene_authoring_choose_debug_target(f.scene, (ecs_entity){ 0 }) == 0);
    app_scene_authoring_set_spawn_break(f.scene, "ArenaProjectile", true);
    steps(&f, 2);
    test_check(f.scene->runtime_playing, "shots already flying do not break");
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) >= 0);
    for (int i = 0; i < 240 && f.scene->runtime_playing; i++) {
        steps(&f, 1);
    }
    sync_editor_inspection(f.scene);
    ecs_entity selected = selected_scene_entity(f.scene);
    test_check(
        !f.scene->runtime_playing && projectile(&f, selected) &&
            same_entity(f.scene->debug.entity, selected) &&
            f.scene->debug_status == APP_DEBUG_AVAILABLE,
        "Break when spawned pauses on the frame a shot spawns, selected and "
        "targeted"
    );
    app_scene_authoring_set_spawn_break(f.scene, "ArenaProjectile", false);
    assert(!app_scene_authoring_spawn_break(f.scene, "ArenaProjectile"));

    /* Playback speed holds through Pause and returns to 1x on Stop. */
    app_scene_authoring_set_time_scale(f.scene, 0.25);
    assert(engine_time_scale(f.runtime) == 0.25);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    test_check(app_scene_authoring_time_scale(f.scene) == 1, "Stop returns playback to normal speed");
    fixture_free(&f);
}

void test_debug_target()
{
    debug_fixture f = fixture_new();
    game_source_registry *behaviors = f.live->runtime->sources;
    ecs_entity player = first_with(f.world, GAME_COMPONENT_GameInput.id);
    for (int i = 0; i < acount(f.scene->actors); i++) {
        if (same_entity(f.scene->actors[i].entity, player)) {
            f.scene->selected = i;
        }
    }
    assert(app_scene_authoring_open_behavior(f.scene, "TwinStickInputBehavior") == 0);
    sync_editor_inspection(f.scene);
    execution_program *input_program = game_source_program(behaviors, "TwinStickInputBehavior");
    execution_inspector *input = execution_program_inspection(input_program);
    assert(input);
    assert(f.scene->debug_status == APP_DEBUG_AVAILABLE);
    assert(same_entity(f.scene->debug.entity, player));
    assert(
        execution_inspector_records(input, ecs_entity_to_u64(player)) &&
        traced(behaviors) == 1
    );
    steps(&f, 2);
    assert(recorded(input, player));
    for (int i = 0; i < game_source_attachment_count(behaviors); i++) {
        game_source_attachment_info other = game_source_attachment_at(behaviors, i);
        execution_inspector *handle = execution_program_inspection(other.program);
        assert(
            (other.program == input_program && same_entity(other.entity, player)) ||
            !recorded(handle, other.entity)
        );
    }

    /* A shot the game spawned is a target too. */
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) >= 0);
    steps(&f, 2);
    ecs_entity shot = first_with(f.world, GAME_COMPONENT_GameProjectile.id);
    scene_debug_attachment info;
    assert(shot.slot && scene_debug_attachment_at(behaviors, shot, 0, &info) == 0);
    char name[128];
    snprintf(name, sizeof name, "%s", info.asset_name);
    assert(app_scene_authoring_open_behavior(f.scene, name) == 0);
    assert(app_scene_authoring_select_runtime_entity(f.scene, shot) == 0);
    sync_editor_inspection(f.scene);
    assert(f.scene->debug_status == APP_DEBUG_AVAILABLE);
    assert(same_entity(f.scene->debug.entity, shot));
    assert(
        !execution_inspector_records(input, ecs_entity_to_u64(player)) &&
        traced(behaviors) == 1
    );

    /* Nothing traces while no graph shows the target. */
    app_scene_authoring_set_debug_hidden(f.scene, true);
    sync_editor_inspection(f.scene);
    assert(traced(behaviors) == 0);
    app_scene_authoring_set_debug_hidden(f.scene, false);
    sync_editor_inspection(f.scene);
    assert(traced(behaviors) == 1);

    /* No target stays until the selection changes. */
    assert(app_scene_authoring_choose_debug_target(f.scene, (ecs_entity){ 0 }) == 0);
    steps(&f, 1);
    sync_editor_inspection(f.scene);
    assert(f.scene->debug_status == APP_DEBUG_NONE && traced(behaviors) == 0);
    assert(app_scene_authoring_open_behavior(f.scene, "TwinStickInputBehavior") == 0);
    f.scene->runtime_selected = (ecs_entity){ 0 };
    sync_editor_inspection(f.scene);
    assert(
        same_entity(f.scene->debug.entity, player) &&
        execution_inspector_records(input, ecs_entity_to_u64(player))
    );
    fixture_free(&f);
    spawned_targets();
    puts("debug target PASS: follows the selection, spawned objects included; only the target traces; hidden graphs and No target trace nothing");
}
