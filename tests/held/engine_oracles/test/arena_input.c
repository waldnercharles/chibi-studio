#include "authoring/engine_preview.h"
#include "editor/workspace/viewport.h"
#include "editor/workspace/gameplay_input.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "runtime/component_catalog.h"
#include "authoring/prototype_preview.h"
#include "support/text_file.h"
#include "test/support/code_fixture.h"
#include "test/support/simulation_fixture.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    app_scene_authoring *scene;
    simulation_fixture simulation;
} arena_fixture;

static arena_fixture arena_new()
{
    arena_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("ArenaTest", behavior_asset_new("ArenaTest"));
    assert(game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("ArenaTest"));
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
    const char
        *weapon_path = "test/fixtures/content/behaviors/WeaponBehavior.chibi";
    char *weapon_json = text_file_read(weapon_path);
    assert(weapon_json);
    behavior_asset *weapon_asset = test_project_asset(weapon_path, weapon_json);
    free(weapon_json);
    assert(weapon_asset);
    game_preview_definition
        *weapon_definition = game_preview_definition_new(weapon_asset->name, weapon_asset);
    assert(weapon_definition);
    assert(game_preview_add_definition(f.live, weapon_definition) == 0);
    assert(
        game_prototype_preview_register_behavior(
            f.scene->prototype_preview,
            weapon_asset->name,
            weapon_asset
        ) == 0
    );
    const char
        *projectile_path = "test/fixtures/content/prototypes/Projectile.chibi";
    char *projectile_json = text_file_read(projectile_path);
    assert(projectile_json);
    assert(app_scene_authoring_register_prototype(f.scene, projectile_json, projectile_path) == 0);
    free(projectile_json);
    int loaded = app_scene_authoring_load(f.scene, "content/scenes/Arena3D.chibi");
    if (loaded) {
        fprintf(stderr, "Arena load: %s\n", app_scene_authoring_message(f.scene));
    }
    assert(loaded == 0);
    /* Keep the cadence fixture free of targets that consume live projectiles. */
    for (int i = 0; i < acount(f.scene->actors); i++) {
        if (!strcmp(f.scene->actors[i].prototype_name, "PhaseWarden")) {
            f.scene->selected = i;
            assert(app_scene_authoring_delete_instance(f.scene) == 0);
            break;
        }
    }
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    f.simulation = simulation_fixture_new(f.runtime, NULL);
    return f;
}

static ecs_entity arena_player(ecs_world *world)
{
    ecs_query query;
    ecs_component_id required[] = { GAME_COMPONENT_GameInput.id,
                                    GAME_COMPONENT_GameAim.id };
    assert(ecs_query_begin(world, &query, required, 2) == ECS_OK);
    ecs_entity player;
    assert(ecs_query_next(&query, &player));
    ecs_query_end(&query);
    return player;
}

static int arena_shots(ecs_world *world, ecs_entity *latest)
{
    ecs_query query;
    ecs_component_id component = GAME_COMPONENT_GameProjectile.id;
    assert(ecs_query_begin(world, &query, &component, 1) == ECS_OK);
    ecs_entity entity;
    int count = 0;
    *latest = (ecs_entity){ 0 };
    while (ecs_query_next(&query, &entity)) {
        count++;
        if (ecs_entity_to_u64(entity) > ecs_entity_to_u64(*latest)) {
            *latest = entity;
        }
    }
    ecs_query_end(&query);
    return count;
}

static void frame(arena_fixture *f, double time)
{
    int result = app_scene_authoring_frame(f->scene, time);
    if (result < 0) {
        fprintf(
            stderr,
            "Arena frame %.4f failed: %d / %s\n",
            time,
            result,
            app_scene_authoring_message(f->scene)
        );
    }
    assert(result >= 0);
    f->simulation.time = time;
}

/* Keep the authoring host in the loop: it forwards focus and transport to the
 * preview engine as well as advancing the scene runtime. */
static void steps(arena_fixture *f, int count)
{
    frame(f, f->simulation.time);
    for (int i = 0; i < count; i++) {
        frame(f, f->simulation.time + ENGINE_FIXED_SECONDS);
    }
}

static void preview_clock(arena_fixture *f)
{
    double time = f->simulation.time;
    f->simulation = simulation_fixture_new(f->runtime, f->scene->prototype_preview);
    f->simulation.time = time;
    frame(f, time);
}

static void arena_fixture_free(arena_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
}

static void movement_membership(arena_fixture *f, ecs_world *world, ecs_entity player)
{
    float before = GAME_GameTransform_get_const(world, player)->x;
    assert(app_scene_authoring_move(f->scene, 1, 0) == FLOW_OK);
    assert(ecs_world_record_remove(world, player, GAME_COMPONENT_GameInput.id, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    steps(f, 1);
    assert(GAME_GameTransform_get_const(world, player)->x == before);
    assert(GAME_GameVelocity_get_const(world, player)->x == 0);
    assert(app_scene_authoring_move(f->scene, 1, 0) == FLOW_OK);
    steps(f, 1);
    assert(GAME_GameTransform_get_const(world, player)->x == before);
    GameInput input = { 0 };
    assert(ecs_world_record_add(world, player, GAME_COMPONENT_GameInput.id, &input, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    assert(app_scene_authoring_move(f->scene, 1, 0) == FLOW_OK);
    steps(f, 1);
    assert(fabsf(GAME_GameTransform_get_const(world, player)->x - before - 3) < 0.001f);
}

static void fire_membership(bool preview)
{
    arena_fixture f = arena_new();
    if (preview) {
        assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
        assert(app_scene_authoring_preview_prototype(f.scene, "ArenaPlayer") == 0);
        engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
        preview_clock(&f);
    }
    ecs_world *world = preview
                         ? game_prototype_preview_world(f.scene->prototype_preview)
                         : f.world;
    game_behavior_world *behaviors = preview ? game_prototype_preview_behaviors(
                                                   f.scene->prototype_preview
                                               )
                                             : f.live->runtime->behaviors;
    ecs_entity player = arena_player(world), ai = ecs_world_create_entity(world);
    GameTransform position = { 100, -100 };
    GameWeapon weapon = { .damage = 10, .cooldown = 0.125f };
    assert(
        ecs_world_record_add(world, ai, GAME_COMPONENT_GameTransform.id, &position, 0) == ECS_OK
    );
    assert(ecs_world_record_add(world, ai, GAME_COMPONENT_GameWeapon.id, &weapon, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    assert(game_behavior_attach(behaviors, player, "WeaponBehavior") == 0);
    assert(game_behavior_attach(behaviors, ai, "WeaponBehavior") == 0);
    ecs_entity shot;
    assert(app_scene_authoring_input(f.scene, "Fire", 0, 0) == 0);
    steps(&f, 1);
    assert(arena_shots(world, &shot) == 0);
    assert(app_scene_authoring_input(f.scene, "Fire", 1, 1) == 1);
    frame(&f, f.simulation.time);
    assert(arena_shots(world, &shot) == 0);
    steps(&f, 1);
    assert(arena_shots(world, &shot) == 1);
    assert(fabsf(GAME_GameTransform_get_const(world, shot)->x - 327) < 0.001f);

    assert(ecs_world_record_remove(world, player, GAME_COMPONENT_GameInput.id, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    assert(app_scene_authoring_input(f.scene, "Fire", 1, 1) == 0);
    assert(app_scene_authoring_input(f.scene, "Fire", 0, 0) == 1);
    assert(app_scene_authoring_input(f.scene, "Fire", 1, 1) == 1);
    steps(&f, 1);
    assert(arena_shots(world, &shot) == 1);

    /* Local ownership must not filter directed gameplay events to an AI. */
    if (preview) {
        assert(game_behavior_dispatch(behaviors, ai, "Fire", (flow_inputs){ 0 }, NULL) == 1);
    } else {
        assert(
            game_entities_request_event(f.live->runtime->entities, ai, "Fire", (execution_arguments){ 0 }) == FLOW_OK
        );
    }
    steps(&f, 1);
    assert(arena_shots(world, &shot) == 2);
    assert(fabsf(GAME_GameTransform_get_const(world, shot)->x - 107) < 0.001f);

    GameInput input = { 0 };
    assert(ecs_world_record_add(world, player, GAME_COMPONENT_GameInput.id, &input, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    assert(app_scene_authoring_input(f.scene, "Fire", 0, 0) == 1);
    assert(app_scene_authoring_input(f.scene, "Fire", 1, 1) == 1);
    steps(&f, 1);
    assert(arena_shots(world, &shot) == 3);
    assert(fabsf(GAME_GameTransform_get_const(world, shot)->x - 327) < 0.001f);
    arena_fixture_free(&f);
}

static void movement_frame_timing()
{
    arena_fixture f = arena_new();
    ecs_entity player = arena_player(f.world);
    frame(&f, 0);
    float before = GAME_GameTransform_get_const(f.world, player)->x;
    assert(app_scene_authoring_move(f.scene, -1, 0) == FLOW_OK);
    assert(app_scene_authoring_move(f.scene, 1, 0) == FLOW_OK);
    assert(GAME_GameTransform_get_const(f.world, player)->x == before);
    frame(&f, ENGINE_FIXED_SECONDS / 2);
    assert(GAME_GameTransform_get_const(f.world, player)->x == before);
    frame(&f, ENGINE_FIXED_SECONDS);
    assert(fabsf(GAME_GameTransform_get_const(f.world, player)->x - before - 3) < 0.001f);
    assert(engine_sample_movement(f.runtime, 1, 0) == FLOW_OK);
    steps(&f, 5);
    assert(fabsf(GAME_GameTransform_get_const(f.world, player)->x - before - 18) < 0.001f);
    movement_membership(&f, f.world, player);

    float active_velocity = GAME_GameVelocity_get_const(f.world, player)->x;
    assert(GAME_GameInput_get_const(f.world, player)->move.x == 1);
    engine_set_focus(f.runtime, ENGINE_FOCUS_CANVAS);
    assert(GAME_GameInput_get_const(f.world, player)->move.x == 0);
    assert(GAME_GameVelocity_get_const(f.world, player)->x == active_velocity);
    steps(&f, 1);
    assert(GAME_GameInput_get_const(f.world, player)->move.x == 0);
    assert(GAME_GameVelocity_get_const(f.world, player)->x == 0);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    assert(app_scene_authoring_move(f.scene, 1, 0) == FLOW_OK);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    player = arena_player(f.world);
    before = GAME_GameTransform_get_const(f.world, player)->x;
    steps(&f, 1);
    assert(GAME_GameTransform_get_const(f.world, player)->x == before);

    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    assert(app_scene_authoring_preview_prototype(f.scene, "ArenaPlayer") == 0);
    preview_clock(&f);
    ecs_world *preview = game_prototype_preview_world(f.scene->prototype_preview);
    player = arena_player(preview);
    before = GAME_GameTransform_get_const(preview, player)->x;
    assert(app_scene_authoring_move(f.scene, 1, 0) == FLOW_OK);
    steps(&f, 1);
    assert(fabsf(GAME_GameTransform_get_const(preview, player)->x - before - 3) < 0.001f);
    assert(game_prototype_preview_sample_movement(f.scene->prototype_preview, 1, 0) == FLOW_OK);
    steps(&f, 5);
    assert(fabsf(GAME_GameTransform_get_const(preview, player)->x - before - 18) < 0.001f);
    movement_membership(&f, preview, player);
    assert(app_scene_authoring_move(f.scene, 1, 0) == FLOW_OK);
    assert(
        app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_PREVIEW, APP_RUNTIME_RESTART) == 0
    );
    player = arena_player(preview);
    before = GAME_GameTransform_get_const(preview, player)->x;
    steps(&f, 2);
    assert(GAME_GameTransform_get_const(preview, player)->x == before);
    arena_fixture_free(&f);
}

static void wall_blocking()
{
    arena_fixture f = arena_new();
    ecs_entity player = arena_player(f.world), wall = { 0 };
    for (int i = 0; i < acount(f.scene->actors); i++) {
        ecs_entity entity = f.scene->actors[i].entity;
        if (GAME_GameEncounter_has(f.world, entity)) {
            assert(game_behavior_detach_asset(f.live->runtime->behaviors, entity, "ArenaWaveBehavior") == 1);
        }
        const GameCollider *collider = GAME_GameCollider_get_const(f.world, entity);
        const GameBody *body = GAME_GameBody_get_const(f.world, entity);
        const GameOrientation *heading = GAME_GameOrientation_get_const(f.world, entity);
        if (collider && body && heading && !collider->sensor &&
            collider->shape == GAME_COLLIDER_BOX &&
            body->type == GAME_BODY_STATIC && fabsf(sinf(heading->radians)) > .2f &&
            fabsf(cosf(heading->radians)) > .2f) {
            wall = entity;
        }
    }
    assert(wall.slot);
    GameTransform center = *GAME_GameTransform_get_const(f.world, wall);
    GameCollider boundary = *GAME_GameCollider_get_const(f.world, wall);
    float angle = GAME_GameOrientation_get_const(f.world, wall)->radians;
    CF_V2 inward = { -sinf(angle), cosf(angle) };
    GameTransform initial = *GAME_GameTransform_get_const(f.world, player);
    if ((initial.x - center.x) * inward.x + (initial.y - center.y) * inward.y < 0) {
        inward.x = -inward.x;
        inward.y = -inward.y;
    }
    float contact = boundary.height * .5f +
                    GAME_GameCollider_get_const(f.world, player)->radius;
    *GAME_GameTransform_get(f.world, player) = (GameTransform){
        center.x + inward.x * (contact + 18),
        center.y + inward.y * (contact + 18)
    };
    assert(app_scene_authoring_move(f.scene, -inward.x, -inward.y) == FLOW_OK);
    float distance = 0;
    for (int i = 0; i < 32; i++) {
        steps(&f, 1);
        GameTransform position = *GAME_GameTransform_get_const(f.world, player);
        float dx = position.x - center.x, dy = position.y - center.y;
        distance = dx * inward.x + dy * inward.y;
        assert(distance >= contact - .2f);
        assert(fabsf(dx * cosf(angle) + dy * sinf(angle)) < .2f);
    }
    assert(distance < contact + .3f);
    assert(app_scene_authoring_move(f.scene, inward.x, inward.y) == FLOW_OK);
    steps(&f, 1);
    GameTransform position = *GAME_GameTransform_get_const(f.world, player);
    float released = (position.x - center.x) * inward.x +
                     (position.y - center.y) * inward.y;
    assert(released > distance + 2.9f);
    arena_fixture_free(&f);
}

static void aim_normalization_rounding()
{
    arena_fixture f = arena_new();
    ecs_entity player = arena_player(f.world);
    /* This unit-length fan direction rounds to 1 in native hypotf but one ULP
     * below 1 in WASM hypotf, changing the projectile trajectory. */
    CF_V2 direction = { 0x1.c65302p-1f, -0x1.d828a2p-2f };
    assert(app_scene_authoring_aim(f.scene, direction, false) == FLOW_OK);
    steps(&f, 1);
    const GameAim *aim = GAME_GameAim_get_const(f.world, player);
    assert(aim->manual && !aim->firing);
    assert(aim->direction.x == direction.x && aim->direction.y == direction.y);
    arena_fixture_free(&f);
}

int test_arena_input()
{
    fire_membership(false);
    fire_membership(true);
    movement_frame_timing();
    aim_normalization_rounding();
    wall_blocking();
    arena_fixture f = arena_new();
    ecs_entity player = arena_player(f.world);
    ecs_entity ai = ecs_world_create_entity(f.world);
    GameTransform ai_position = { 100, -100 };
    GameAim ai_aim = { .direction = { 0, -1 }, .firing = true };
    assert(
        ecs_world_record_add(f.world, ai, GAME_COMPONENT_GameTransform.id, &ai_position, 0) == ECS_OK
    );
    assert(ecs_world_record_add(f.world, ai, GAME_COMPONENT_GameAim.id, &ai_aim, 0) == ECS_OK);
    assert(ecs_world_apply_commands(f.world) == ECS_OK);
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) == FLOW_OK);
    assert(GAME_GameAim_get_const(f.world, ai)->direction.y == -1);
    ecs_entity shot;
    frame(&f, 0);
    assert(arena_shots(f.world, &shot) == 0);
    steps(&f, 1);
    assert(arena_shots(f.world, &shot) == 1);
    const GameVelocity *velocity = GAME_GameVelocity_get_const(f.world, shot);
    assert(velocity->x == 480 && velocity->y == 0);
    assert(GAME_GameGeometry3D_has(f.world, shot));
    steps(&f, 3);
    assert(arena_shots(f.world, &shot) == 1);
    float before = GAME_GameTransform_get_const(f.world, player)->x;
    assert(app_scene_authoring_move(f.scene, -1, 0) == FLOW_OK);
    assert(GAME_GameTransform_get_const(f.world, player)->x == before);
    steps(&f, 4);
    assert(fabsf(GAME_GameTransform_get_const(f.world, player)->x - before + 12) < 0.001f);
    assert(arena_shots(f.world, &shot) == 2);
    assert(fabsf(GAME_GameOrientation_get_const(f.world, player)->radians) < 0.001f);
    /* A float cooldown can land just below 0.125 at tick 15. Check the
     * next fixed tick, when the third shot is due in either case. */
    steps(&f, 8);
    assert(arena_shots(f.world, &shot) == 3);

    assert(app_scene_authoring_move(f.scene, 0, 0) == FLOW_OK);
    GameTransform position = *GAME_GameTransform_get_const(f.world, player);
    assert(
        app_scene_authoring_aim_at(f.scene, (CF_V2){ position.x, position.y + 100 }, true) == FLOW_OK
    );
    steps(&f, 1);
    const GameAim *aim = GAME_GameAim_get_const(f.world, player);
    assert(fabsf(aim->direction.x) < 0.001f && fabsf(aim->direction.y - 1) < 0.001f);
    aim = GAME_GameAim_get_const(f.world, ai);
    assert(aim->direction.x == 0 && aim->direction.y == -1 && aim->firing);
    /* Cursor aiming stays active on mouse-up, independently of firing. */
    assert(
        app_scene_authoring_aim_at(f.scene, (CF_V2){ position.x, position.y + 100 }, false) == FLOW_OK
    );
    steps(&f, 1);
    aim = GAME_GameAim_get_const(f.world, player);
    assert(aim->manual && !aim->firing && fabsf(aim->direction.y - 1) < .001f);
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 0 }, false) == FLOW_OK);
    steps(&f, 5);
    assert(arena_shots(f.world, &shot) == 3);
    aim = GAME_GameAim_get_const(f.world, player);
    assert(!aim->manual && aim->direction.x == 0 && aim->direction.y == 1);

    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 0, -1 }, true) == FLOW_OK);
    engine_viewport *unfocused = engine_viewport_new(32, 32);
    app_gameplay_input(f.scene, f.runtime, unfocused);
    steps(&f, 1);
    assert(!GAME_GameAim_get_const(f.world, player)->firing);
    assert(GAME_GameAim_get_const(f.world, ai)->firing);
    assert(arena_shots(f.world, &shot) == 3);
    engine_viewport_free(unfocused);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) == FLOW_OK);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PAUSE) == 0);
    double paused_time = engine_last_timing(f.runtime)->simulation_time;
    steps(&f, 5);
    assert(engine_last_timing(f.runtime)->simulation_time == paused_time);
    assert(arena_shots(f.world, &shot) == 3);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    steps(&f, 1);
    assert(!GAME_GameAim_get_const(f.world, player)->firing);
    assert(arena_shots(f.world, &shot) == 3);

    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_RESTART) == 0);
    player = arena_player(f.world);
    assert(arena_shots(f.world, &shot) == 0);
    assert(!GAME_GameAim_get_const(f.world, player)->firing);
    assert(GAME_GameTransform_get_const(f.world, player)->x == 320);
    for (int i = 0; i <= 16; i++) {
        assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, i % 2 == 0) == FLOW_OK);
        steps(&f, 1);
        assert(arena_shots(f.world, &shot) <= 1 + i / 8);
    }
    assert(arena_shots(f.world, &shot) == 3);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    player = arena_player(f.world);
    assert(app_scene_authoring_preview_prototype(f.scene, "ArenaPlayer") == 0);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    preview_clock(&f);
    ecs_world *preview = game_prototype_preview_world(f.scene->prototype_preview);
    ecs_entity preview_player = arena_player(preview);
    assert(app_scene_authoring_aim(f.scene, (CF_V2){ -1, 0 }, true) == FLOW_OK);
    assert(!GAME_GameAim_get_const(preview, preview_player)->firing);
    steps(&f, 1);
    assert(GAME_GameAim_get_const(preview, preview_player)->firing);
    assert(!GAME_GameAim_get_const(f.world, player)->firing);
    assert(arena_shots(preview, &shot) == 1);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_PREVIEW, APP_RUNTIME_PAUSE) == 0);
    paused_time = engine_last_timing(f.simulation.runtime)->simulation_time;
    steps(&f, 5);
    assert(engine_last_timing(f.simulation.runtime)->simulation_time == paused_time);
    assert(arena_shots(preview, &shot) == 1);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_PREVIEW, APP_RUNTIME_PLAY) == 0);
    steps(&f, 1);
    assert(!GAME_GameAim_get_const(preview, preview_player)->firing);
    assert(arena_shots(preview, &shot) == 1);

    arena_fixture_free(&f);
    puts("arena input PASS: local movement/fire membership, directed AI events, fixed-step movement, independent aim, held/tapped cadence, focus/pause release, physical walls, restart and preview isolation");
    return 0;
}
