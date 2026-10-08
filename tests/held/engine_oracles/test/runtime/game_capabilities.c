#include "authoring/engine_preview.h"
#include "editor/workspace/viewport.h"
#include "authoring/preview.h"
#include "runtime/camera.h"
#include "runtime/component_catalog.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void saved_progress()
{
    char path[] = "/tmp/chibi-save-test-XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    close(file);
    unlink(path);
    game_save *save = game_save_new();
    assert(game_save_open(save, path));
    assert(game_save_set(save, "blueprint", "ricochet"));
    assert(game_save_commit(save));
    assert(game_save_set(save, "unsecured", "payload"));
    game_save_discard(save);
    assert(!game_save_get(save, "unsecured"));
    assert(!game_save_import(save, "{\"blueprint\":1}"));
    assert(!strcmp(game_save_get(save, "blueprint"), "ricochet"));
    assert(!game_save_import(save, "{\"a\":\"x\",\"a\":\"y\"}"));
    game_save_free(save);
    save = game_save_new();
    assert(game_save_open(save, path));
    assert(!strcmp(game_save_get(save, "blueprint"), "ricochet"));
    assert(game_save_remove(save, "blueprint"));
    assert(game_save_commit(save));
    game_save_free(save);
    unlink(path);
    char directory[] = "/tmp/chibi-save-failure-XXXXXX";
    assert(mkdtemp(directory));
    char storage[128], moved[128];
    snprintf(storage, sizeof storage, "%s/profile.json", directory);
    snprintf(moved, sizeof moved, "%s-moved", directory);
    save = game_save_new();
    assert(game_save_open(save, storage));
    assert(game_save_set(save, "blueprint", "secured"));
    assert(game_save_commit(save));
    assert(rename(directory, moved) == 0);
    assert(game_save_set(save, "blueprint", "pending"));
    assert(!game_save_commit(save));
    game_save_discard(save);
    assert(!strcmp(game_save_get(save, "blueprint"), "secured"));
    assert(rename(moved, directory) == 0);
    assert(game_save_open(save, storage));
    assert(!strcmp(game_save_get(save, "blueprint"), "secured"));
    game_save_free(save);
    assert(unlink(storage) == 0 && rmdir(directory) == 0);
}

static ecs_entity entity_at(ecs_world *world, CF_V2 point)
{
    ecs_entity entity = ecs_world_create_entity(world);
    assert(GAME_GameTransform_add(world, entity, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    *GAME_GameTransform_get(world, entity) = point;
    return entity;
}

static void sound_feedback()
{
    game_audio *audio = game_audio_new();
    game_sound sound = { 440, 180, 0.2f, 0.5f, 0.5f, 0.1f };
    assert(game_audio_emit(audio, sound));
    uint64_t hash = game_audio_hash(audio);
    assert(!game_audio_emit(audio, (game_sound){ 0 }));
    assert(game_audio_count(audio) == 1 && game_audio_hash(audio) == hash);
    game_sound played;
    assert(game_audio_take(audio, &played, 1) == 1 && game_audio_take(audio, &played, 1) == 0);
    int size;
    unsigned char *wav = game_sound_wav(sound, &size);
    assert(wav && size == 17684 && !memcmp(wav, "RIFF", 4));
    long energy = 0;
    for (int i = 44; i < size; i += 2) {
        int16_t sample = (int16_t)((unsigned)wav[i] | (unsigned)wav[i + 1] << 8);
        energy += abs(sample);
    }
    assert(energy > 1000000 && !wav[44] && !wav[45] && !wav[size - 1] && !wav[size - 2]);
    free(wav);
    game_audio_free(audio);
}

static void visibility_benchmark(ecs_world *world, game_preview_world *live, ecs_entity settings, ecs_entity player)
{
    if (!getenv("CHIBI_VISIBILITY_BENCH")) {
        return;
    }
    *GAME_GameFog_get(world, settings) = (GameFog){ .origin = { -512, -512 },
                                                    .columns = 128,
                                                    .rows = 128,
                                                    .cell_size = 8 };
    *GAME_GameVision_get(world, player) = (GameVision){ .radius = 220,
                                                        .occluders = 8,
                                                        .remember = true };
    for (int i = 0; i < 64; i++) {
        ecs_entity wall = entity_at(world, (CF_V2){ (i % 8) * 120 - 420, (i / 8) * 120 - 420 });
        assert(GAME_GameCollider_add(world, wall, 0) == ECS_OK);
        assert(ecs_world_apply_commands(world) == ECS_OK);
        *GAME_GameCollider_get(world, wall) = (GameCollider){ .shape = GAME_COLLIDER_BOX,
                                                              .width = 12,
                                                              .height = 60,
                                                              .category = 8 };
    }
    double total = 0, maximum = 0;
    for (int i = 0; i < 120; i++) {
        *GAME_GameTransform_get(world, player) = (CF_V2){ sinf(i * 0.05f) * 60,
                                                          cosf(i * 0.05f) * 60 };
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        assert(!game_visibility_update(live->runtime->visibility, 1.0f / 60));
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) * 1000 +
                         (end.tv_nsec - start.tv_nsec) / 1000000.0;
        total += elapsed;
        if (elapsed > maximum) {
            maximum = elapsed;
        }
    }
    printf(
        "Visibility benchmark: 128x128 grid, 65 walls, radius 220, 120 moving "
        "ticks: mean %.3f ms, max %.3f ms\n",
        total / 120,
        maximum
    );
}

void test_game_capabilities()
{
    saved_progress();
    sound_feedback();
    ecs_world *world = ecs_world_new();
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    game_preview_world *live = game_preview_world_new(world);
    engine *runtime = authoring_engine_new(world, live->runtime);
    ecs_entity settings = ecs_world_create_entity(world);
    assert(GAME_GameFog_add(world, settings, 0) == ECS_OK);
    ecs_entity player = entity_at(world, (CF_V2){ 5, 5 });
    assert(GAME_GameVision_add(world, player, 0) == ECS_OK);
    assert(GAME_GameCamera_add(world, player, 0) == ECS_OK);
    ecs_entity wall = entity_at(world, (CF_V2){ 35, 5 });
    assert(GAME_GameCollider_add(world, wall, 0) == ECS_OK);
    ecs_entity contact = ecs_world_create_entity(world);
    assert(GAME_GameSensorContact_add(world, contact, 0) == ECS_OK);
    assert(ecs_world_apply_commands(world) == ECS_OK);
    *GAME_GameFog_get(world, settings) = (GameFog){ .origin = { -50, -50 },
                                                    .columns = 30,
                                                    .rows = 20,
                                                    .cell_size = 10 };
    *GAME_GameVision_get(world, player) = (GameVision){ .radius = 80,
                                                        .occluders = 8,
                                                        .remember = true };
    *GAME_GameCamera_get(world, player) = (GameCamera){ .width = 320,
                                                        .height = 180,
                                                        .offset = { 2, 3 } };
    *GAME_GameCollider_get(world, wall) = (GameCollider){ .shape = GAME_COLLIDER_BOX,
                                                          .width = 10,
                                                          .height = 120,
                                                          .category = 8 };
    *GAME_GameSensorContact_get(world, contact) = (GameSensorContact){
        .position = { 75, 5 },
        .remaining = 0.1f
    };
    engine_viewport_view view;
    assert(game_camera_view(world, NULL, &view) == 1);
    assert(view.center_x == 7 && view.center_y == -8 && view.width == 320);
    assert(view.focal_distance == 0);
    GAME_GameCamera_get(world, player)->focal_distance = 900;
    assert(game_camera_view(world, NULL, &view) == 1 && view.focal_distance == 900);
    GAME_GameCamera_get(world, player)->yaw = 1.2f;
    GAME_GameCamera_get(world, player)->pitch = .8f;
    GAME_GameCamera_get(world, player)->target_elevation = 60;
    assert(game_camera_view(world, NULL, &view) == 1);
    assert(view.yaw == 1.2f && view.pitch == .8f && view.target_elevation == 60);
    GAME_GameCamera_get(world, player)->yaw = NAN;
    assert(game_camera_view(world, NULL, &view) == -1);
    GAME_GameCamera_get(world, player)->yaw = 0;
    GAME_GameCamera_get(world, player)->focal_distance = 1;
    assert(game_camera_view(world, NULL, &view) == -1);
    GAME_GameCamera_get(world, player)->focal_distance = 0;
    assert(!game_visibility_update(live->runtime->visibility, 0));
    assert(game_visibility_at(live->runtime->visibility, (CF_V2){ 15, 5 }).visible);
    assert(!game_visibility_at(live->runtime->visibility, (CF_V2){ 65, 5 }).visible);
    assert(!game_visibility_at(live->runtime->visibility, (CF_V2){ 65, 5 }).explored);
    *GAME_GameTransform_get(world, player) = (CF_V2){ -40, -40 };
    GAME_GameVision_get(world, player)->radius = 15;
    assert(!game_visibility_update(live->runtime->visibility, 0.2f));
    game_visibility_sample old = game_visibility_at(live->runtime->visibility, (CF_V2){ 15, 5 });
    assert(!old.visible && old.explored);
    assert(GAME_GameSensorContact_get_const(world, contact)->remaining == 0);
    GameFog config;
    const unsigned char *cells = game_visibility_cells(live->runtime->visibility, &config);
    assert(cells && config.columns == 30);
    game_visibility_reset(live->runtime->visibility);
    assert(!game_visibility_update(live->runtime->visibility, 0));
    assert(!game_visibility_at(live->runtime->visibility, (CF_V2){ 15, 5 }).explored);
    visibility_benchmark(world, live, settings, player);
    engine_free(runtime);
    game_preview_world_free(live);
    ecs_world_free(world);
    puts("Game capability checks passed: committed saves, camera framing, occlusion, terrain memory and sensor expiry");
}
