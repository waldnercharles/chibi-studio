#include "authoring/preview.h"
#include "authoring/prototype_preview.h"
#include "editor/workspace/movement_input.h"
#include "graph/document/serialize.h"
#include "runtime/component_catalog.h"
#include "runtime/engine.h"
#include "runtime/particles.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "test/support/code_fixture.h"
#include "test/support/event_fixture.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_content(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *text = xmalloc((size_t)size + 1);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = 0;
    fclose(file);
    return text;
}

static test_source_fixture read_behavior(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "test/fixtures/content/behaviors/%s.chibi", name);
    char *text = read_content(path);
    test_source_fixture fixture = test_source_prepare(path, text);
    free(text);
    return fixture;
}

static int attach(ecs_world *, ecs_entity entity, const char *name, void *user)
{
    game_preview_runtime *runtime = user;
    return game_source_attach(
        runtime->sources,
        entity,
        name,
        runtime->deferred,
        runtime->now,
        runtime->delta
    );
}

static void trail_case(bool isolated)
{
    game_prototype_registry registry = { GAME_REGISTRY_COMPONENTS,
                                         GAME_REGISTRY_COMPONENT_COUNT };
    char *text = read_content("test/fixtures/content/prototypes/Player.kdl");
    char error[256];
    game_prototype *prototype = game_prototype_load(text, &registry, error, sizeof error);
    free(text);
    assert(prototype);
    for (int i = acount(prototype->graphs) - 1; i >= 0; i--) {
        const char *name = prototype->graphs[i];
        if (strcmp(name, "InputBehavior") && strcmp(name, "MovementTrailBehavior")) {
            assert(game_prototype_remove_graph(prototype, name) == 0);
        }
    }
    game_prototype_preview *preview = isolated ? game_prototype_preview_new(&registry)
                                               : NULL;
    register_test_preview_catalog(preview);
    ecs_world *world = preview ? game_prototype_preview_world(preview)
                               : ecs_world_new();
    game_preview_world *live = preview ? NULL : game_preview_world_new(world);
    const char *names[] = { "InputBehavior", "MovementTrailBehavior" };
    for (int i = 0; i < 2; i++) {
        test_source_fixture fixture = read_behavior(names[i]);
        test_source_register(
            preview ? game_prototype_preview_live(preview)->runtime : live->runtime,
            &fixture
        );
        test_source_free(&fixture);
    }
    ecs_entity entity;
    if (preview) {
        assert(game_prototype_preview_launch(preview, prototype, error, sizeof error) == 0);
        entity = game_prototype_preview_entity(preview);
    } else {
        for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
            assert(
                ecs_world_register_component(world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK
            );
        }
        assert(
            game_prototype_spawn(
                prototype,
                world,
                &(game_prototype_spawn_hooks){ attach, live->runtime },
                &entity,
                error,
                sizeof error
            ) == 0
        );
    }
    game_preview_runtime *host = preview ? game_prototype_preview_live(preview)->runtime
                                         : live->runtime;
    game_particles *particles = preview
                                  ? (game_particles *)game_prototype_preview_particles(preview)
                                  : live->runtime->particles;
    for (int direction = 0; direction < 4; direction++) {
        game_particles_clear(particles);
        app_movement_input keys = app_movement_from_keys(
            direction == 0,
            direction == 1,
            direction == 2,
            direction == 3
        );
        CF_V2 vector = { keys.x, keys.y };
        pin_value input = { .type = PIN_STRUCT, .type_name = "CF_V2", .ptr = &vector };
        GameTransform before = *GAME_GameTransform_get(world, entity);
        host->delta = 1.0 / 64;
        assert(
            game_source_dispatch(host->sources, entity, "Input", &input, 1, host->now, host->delta) >= 0
        );
        const GameTransform *after = GAME_GameTransform_get_const(world, entity);
        float speed = GAME_GameMovement_get_const(world, entity)->speed;
        const GameVelocity *velocity = GAME_GameVelocity_get_const(world, entity);
        assert(velocity && fabsf(velocity->x - vector.x * speed) < 0.0001f);
        assert(fabsf(velocity->y - vector.y * speed) < 0.0001f);
        assert(after->x == before.x && after->y == before.y);
        assert(game_particles_count(particles) == 1);
        const game_particle *particle = game_particles_data(particles);
        assert(fabsf(particle->position.x - after->x + vector.x * 9) < 0.0001f);
        assert(fabsf(particle->position.y - after->y + vector.y * 9) < 0.0001f);
        assert(particle->velocity.x * vector.x + particle->velocity.y * vector.y < 0);
        assert(particle->lifetime >= 0.2f && particle->lifetime <= 0.4f);
        assert(particle->size >= 2 && particle->size <= 4);
        assert(particle->color == 0xFF991AFFu && particle->end_color == 0x4D00FF80u);
        vector = (CF_V2){ 0 };
        assert(
            game_source_dispatch(host->sources, entity, "Input", &input, 1, host->now, host->delta) >= 0
        );
        assert(game_particles_count(particles) == 1);
    }
    game_particles_clear(particles);
    host->delta = 1.0 / 128;
    CF_V2 vector = { 1, 0 };
    pin_value input = { .type = PIN_STRUCT, .type_name = "CF_V2", .ptr = &vector };
    for (int i = 0; i < 8; i++) {
        assert(
            game_source_dispatch(host->sources, entity, "Input", &input, 1, host->now, host->delta) >= 0
        );
        assert(game_particles_count(particles) == (i + 1) / 2);
    }
    assert(game_particles_tick(particles, 0.5) == 0);
    assert(game_particles_count(particles) == 0);
    if (preview) {
        game_prototype_preview_free(preview);
    } else {
        game_preview_world_free(live);
        ecs_world_free(world);
    }
    game_prototype_free(prototype);
}

void test_movement_trail()
{
    trail_case(false);
    trail_case(true);
    puts("movement trail PASS: keyboard direction sets velocity and rear exhaust, fractional emission and idle in both hosts");
}
