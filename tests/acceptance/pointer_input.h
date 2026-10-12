/* Keep the native window outside this CPU test; the real gameplay ingress and
 * engine recording still consume the host's projected pointer and controls. */
static struct
{
    bool focused, typing, capture, ray_valid;
    CF_Ray3 ray;
} pointer_host;

static int pointer_focused(const engine_viewport *)
{
    return pointer_host.focused;
}
static bool pointer_typing(const engine_viewport *)
{
    return pointer_host.typing;
}
static bool pointer_capture(const engine_viewport *)
{
    return pointer_host.capture;
}
static bool pointer_world(const engine_viewport *, float *, float *)
{
    return false;
}
static bool pointer_ray(const engine_viewport *, CF_Ray3 *ray)
{
    *ray = pointer_host.ray;
    return pointer_host.ray_valid;
}
static uint64_t pointer_controls()
{
    return UINT64_C(1) << CONTROL_MOUSE_LEFT;
}

#define engine_viewport_focused pointer_focused
#define engine_viewport_ui_typing pointer_typing
#define engine_viewport_ui_capture pointer_capture
#define engine_viewport_mouse_world pointer_world
#define engine_viewport_mouse_ray pointer_ray
#define app_input_controls pointer_controls
#define app_gameplay_input acceptance_gameplay_input
#define app_gameplay_sample acceptance_gameplay_sample
#include "../../src/editor/workspace/gameplay_input.c"
#undef app_gameplay_sample
#undef app_gameplay_input
#undef app_input_controls
#undef engine_viewport_mouse_ray
#undef engine_viewport_mouse_world
#undef engine_viewport_ui_capture
#undef engine_viewport_ui_typing
#undef engine_viewport_focused

static ecs_world *pointer_entities;
static engine *pointer_runtime;

static void pointer_cleanup()
{
    engine_free(pointer_runtime);
    ecs_world_free(pointer_entities);
    pointer_runtime = NULL;
    pointer_entities = NULL;
    editor_set_retained_keyboard(false);
}

TEST_CASE(editor_play_forwards_and_clears_pointer_rays)
{
    pointer_entities = ecs_world_new();
    for (int i = 0; i < REGISTRY_COMPONENT_COUNT; i++) {
        REQUIRE(
            ecs_world_register_component(pointer_entities, REGISTRY_COMPONENTS[i], NULL) == ECS_OK
        );
    }
    pointer_runtime = engine_new(pointer_entities, &(engine_modules){ .input = true, .rendering = true });
    REQUIRE(pointer_runtime);
    app_scene_authoring scene = { .runtime = pointer_runtime,
                                  .runtime_playing = true,
                                  .active_runtime_target = APP_RUNTIME_SCENE };
    REQUIRE(engine_set_playing(pointer_runtime, true) == 0);
    engine_record_input(pointer_runtime, 1);
    REQUIRE(engine_frame(pointer_runtime, 0) == 0);
    pointer_host.ray = (CF_Ray3){ .p = { 2, 3, 4 }, .d = { 0, 0, -1 } };
    for (int i = 0; i < 8; i++) {
        pointer_host.focused = i != 2;
        pointer_host.ray_valid = i != 1;
        pointer_host.typing = i == 3;
        pointer_host.capture = i == 4;
        editor_set_retained_keyboard(i == 5);
        REQUIRE(engine_set_playing(pointer_runtime, i != 6) == 0);
        acceptance_gameplay_input(&scene, pointer_runtime, NULL);
        REQUIRE(engine_set_playing(pointer_runtime, true) == 0);
        REQUIRE(scene.message[0] == '\0');
        REQUIRE(engine_frame(pointer_runtime, (i + 1) * 0.02) == 0);
        int count;
        const input_keyframe *keys = engine_recorded_input(pointer_runtime, &count);
        REQUIRE(count > 0);
        input_keyframe held = keys[count - 1];
        bool active = i == 0 || i == 7;
        REQUIRE(held.pointer.active == active);
        REQUIRE(held.controls == (i < 2 || i == 7 ? UINT64_C(1) << CONTROL_MOUSE_LEFT : 0));
        if (active) {
            REQUIRE(
                held.pointer.origin.x == 2 && held.pointer.origin.y == 3 &&
                held.pointer.origin.z == 4
            );
            REQUIRE(
                held.pointer.direction.x == 0 &&
                held.pointer.direction.y == 0 && held.pointer.direction.z == -1
            );
        }
    }
    return true;
}
