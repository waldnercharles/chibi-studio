#include "authoring/engine_preview.h"
#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/asset_edit.h"
#include "authoring/prototype_prepare.h"
#include "editor/workspace/viewport.h"
#include "assets/physics_settings.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/game_presentation.h"
#include "editor/workspace/gameplay_input.h"
#include "editor/workspace/input.h"
#include "editor/workspace/scene_internal.h"
#include "graph/compiler/behavior_program.h"
#include "graph/model/registry.h"
#include "platform/cute.h"
#include "render/arena3d.h"
#include "render/arena_geometry.h"
#include "render/arena_parts.h"
#include "render/arena_visual.h"
#include "render/scene_renderer.h"
#include "render/screenshot.h"
#include "render/ui.h"
#include "runtime/component_catalog.h"
#include "support/asset_json.h"
#include "support/text_file.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"
#include "test/physics_executor.h"
#include "test/suite_internal.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <assert.h>
#include <imgui/dcimgui.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int test_arena_input();

/* CPU scopes of one editor frame, in milliseconds. */
typedef enum
{
    ARENA_STAGE_INPUT,
    ARENA_STAGE_DOCUMENTS,
    ARENA_STAGE_SIMULATION,
    ARENA_STAGE_SCENE,
    ARENA_STAGE_PRESENT,
    ARENA_STAGE_COUNT,
} arena_stage;

/* An editor session without Studio: the Scene area fills the window and reads
 * the mouse and keyboard as Studio reports them to the viewport. */
typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    editor *graph;
    engine *runtime;
    engine_viewport *viewport;
    app_scene_authoring *scene;
    double time;
    bool sample_controls;
    bool profile_behaviors;
    bool focused;
    double stages[ARENA_STAGE_COUNT];
} arena_fixture;

static void arena_app_start()
{
    app_scene_renderer_initialize();
    cf_app_init_imgui();
    ImGui_GetIO()->IniFilename = NULL;
    cf_app_set_imgui_render_target(cf_app_get_canvas());
    app_input_initialize();
}

static void arena_app_stop()
{
    app_input_shutdown();
    cf_destroy_app();
}

static double elapsed_ms(uint64_t *since)
{
    uint64_t now = cf_get_ticks();
    double ms = 1000.0 * (double)(now - *since) / (double)cf_get_tick_frequency();
    *since = now;
    return ms;
}

static void viewport_input(arena_fixture *f)
{
    engine_viewport_host_input in = {
        .width = (float)cf_app_get_width(),
        .height = (float)cf_app_get_height(),
        .x = cf_mouse_x(),
        .y = cf_mouse_y(),
        .hovered = true,
        .focused = f->focused,
        .wheel = cf_mouse_wheel_motion(),
        .alt = cf_key_alt(),
        .escape = cf_key_just_pressed(CF_KEY_ESCAPE),
    };
    for (int b = 0; b < 3; b++) {
        in.down[b] = cf_mouse_down((CF_MouseButton)b);
        in.pressed[b] = cf_mouse_just_pressed((CF_MouseButton)b);
        in.released[b] = cf_mouse_just_released((CF_MouseButton)b);
    }
    engine_viewport_set_host_input(f->viewport, &in);
}

/* Shows the viewport texture over the window, as Studio composites it, so
 * captures and pixel reads include the Scene. */
static void present_viewport(arena_fixture *f)
{
    ImGuiIO *io = ImGui_GetIO();
    ImVec2 size = { (float)cf_app_get_width(), (float)cf_app_get_height() };
    io->DisplaySize = size;
    ImGuiViewport *view = ImGui_GetMainViewport();
    view->Pos = view->WorkPos = (ImVec2){ 0 };
    view->Size = view->WorkSize = size;
    ImGui_SetNextWindowPos((ImVec2){ 0 }, ImGuiCond_Always);
    ImGui_SetNextWindowSize(size, ImGuiCond_Always);
    ImGui_PushStyleVarImVec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0 });
    ImGui_PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui_Begin(
        "Scene",
        NULL,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus
    );
    CF_Texture texture = { engine_viewport_texture(f->viewport) };
    ImGui_Image((ImTextureRef){ ._TexID = (ImTextureID)(uintptr_t)cf_texture_handle(texture) }, size);
    ImGui_End();
    ImGui_PopStyleVarEx(2);
}

static void draw_frame(arena_fixture *f)
{
    uint64_t since = cf_get_ticks();
    f->time += 1.0 / 60.0;
    editor_tick(f->graph, f->time);
    if (f->sample_controls) {
        app_gameplay_input(f->scene, f->runtime, f->viewport);
    }
    f->stages[ARENA_STAGE_DOCUMENTS] = elapsed_ms(&since);
    engine_set_behavior_profiling(f->runtime, f->profile_behaviors);
    assert(app_scene_authoring_frame(f->scene, f->time) == FLOW_OK);
    f->stages[ARENA_STAGE_SIMULATION] = elapsed_ms(&since);
    viewport_input(f);
    assert(app_scene_authoring_draw_viewport(f->scene, f->viewport) == 0);
    present_viewport(f);
    f->stages[ARENA_STAGE_SCENE] = elapsed_ms(&since);
    cf_app_draw_onto_screen(true);
    f->stages[ARENA_STAGE_PRESENT] = elapsed_ms(&since);
}

static void frame(arena_fixture *f)
{
    uint64_t since = cf_get_ticks();
    cf_app_update(NULL);
    app_input_frame();
    f->stages[ARENA_STAGE_INPUT] = elapsed_ms(&since);
    draw_frame(f);
}

static arena_fixture fixture_new()
{
    arena_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Arena UI", behavior_asset_new("Arena UI"));
    assert(game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Arena UI"));
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    f.viewport = engine_viewport_new(800, 600);
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
    assert(app_scene_authoring_load_content(f.scene) == 0);
    const char
        *preview_path = "test/fixtures/content/prototypes/Editor2DPreview.kdl";
    char *preview_json = text_file_read(preview_path);
    assert(preview_json);
    assert(app_scene_authoring_register_prototype(f.scene, preview_json, preview_path) == 0);
    free(preview_json);
    int loaded = app_scene_authoring_load(f.scene, "content/scenes/Arena3D.kdl");
    if (loaded != 0) {
        fprintf(stderr, "Arena scene load: %s\n", app_scene_authoring_message(f.scene));
    }
    assert(loaded == 0);
    assert(app_scene_authoring_open_behavior(f.scene, "TwinStickWeaponBehavior") == 0);
    assert(app_scene_authoring_set_runtime_target(f.scene, APP_RUNTIME_SCENE) == 0);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    engine_viewport_set_render_callback(f.viewport, app_scene_authoring_render, f.scene);
    return f;
}

static void fixture_free(arena_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_viewport_free(f->viewport);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    test_ui_probes_free();
    editor_free(f->graph);
    ecs_world_free(f->world);
}

static void key_event(SDL_Scancode key, bool down)
{
    SDL_Event event = { .type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP };
    event.key.windowID = SDL_GetWindowID(cf_app_get_window());
    event.key.scancode = key;
    event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, true);
    event.key.down = down;
    assert(SDL_PushEvent(&event));
}

static void mouse_button_event(bool down)
{
    SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                     : SDL_EVENT_MOUSE_BUTTON_UP };
    event.button.windowID = SDL_GetWindowID(cf_app_get_window());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = cf_mouse_x();
    event.button.y = cf_mouse_y();
    assert(SDL_PushEvent(&event));
}

static void window_focus_event(bool focused)
{
    SDL_Event event = { .type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED
                                        : SDL_EVENT_WINDOW_FOCUS_LOST };
    event.window.windowID = SDL_GetWindowID(cf_app_get_window());
    assert(SDL_PushEvent(&event));
}

static void mouse_move_event(CF_V2 position)
{
    SDL_Event event = { .type = SDL_EVENT_MOUSE_MOTION };
    event.motion.windowID = SDL_GetWindowID(cf_app_get_window());
    event.motion.x = position.x;
    event.motion.y = position.y;
    assert(SDL_PushEvent(&event));
}

static int player_index(arena_fixture *f)
{
    for (int i = 0; i < acount(f->scene->actors); i++) {
        if (GAME_GameAim_has(f->world, f->scene->actors[i].entity)) {
            return i;
        }
    }
    assert(!"Arena scene requires a player with aiming input");
    return -1;
}

static render_arena_camera camera(arena_fixture *f)
{
    render_arena_camera view;
    engine_viewport_get_arena_camera(f->viewport, &view);
    return view;
}

/* Window coordinates of a viewport NDC position. */
static CF_V2 screen_ndc(arena_fixture *, CF_V2 ndc)
{
    return (CF_V2){ (ndc.x * 0.5f + 0.5f) * (float)cf_app_get_width(),
                    (0.5f - ndc.y * 0.5f) * (float)cf_app_get_height() };
}

static CF_V2 screen_point(arena_fixture *f, CF_V3 point)
{
    render_arena_camera view = camera(f);
    CF_V3 ndc = render_arena_project(&view, point);
    return screen_ndc(f, (CF_V2){ ndc.x, ndc.y });
}

static void assert_framed(arena_fixture *f)
{
    render_arena_camera view = camera(f);
    for (int i = 0; i < acount(f->scene->actors); i++) {
        ecs_entity entity = f->scene->actors[i].entity;
        CF_V3 corners[8];
        if (!render_arena_entity_bounds(f->world, entity, corners)) {
            continue;
        }
        for (int j = 0; j < 8; j++) {
            CF_V3 ndc = render_arena_project(&view, corners[j]);
            assert(fabsf(ndc.x) < 1 && fabsf(ndc.y) < 1 && ndc.z > 0 && ndc.z < 1);
        }
    }
}

static CF_V2 player_ribbon_point(arena_fixture *f, ecs_entity entity)
{
    arena_visual visuals[ARENA_PART_LIMIT];
    char error[160];
    assert(render_arena_visuals(f->world, entity, NULL, visuals, error, sizeof error) == 1);
    CF_M4x4 model = render_arena_visual_model(visuals);
    CF_V3 triangles[ARENA_MESH_TRIANGLE_LIMIT][3];
    int count = render_arena_mesh_triangles(visuals[0].part.primitive, triangles);
    assert(count > 0);
    CF_V2 best = { 0 };
    float margin = 0;
    for (int i = 0; i < count; i++) {
        CF_V2 p[3];
        for (int j = 0; j < 3; j++) {
            p[j] = screen_point(f, cf_m4_transform_point(model, triangles[i][j]));
        }
        float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) -
                     (p[1].y - p[0].y) * (p[2].x - p[0].x);
        if (fabsf(area) < .0001f) {
            continue;
        }
        int left = (int)ceilf(fminf(p[0].x, fminf(p[1].x, p[2].x)));
        int right = (int)floorf(fmaxf(p[0].x, fmaxf(p[1].x, p[2].x)));
        int top = (int)ceilf(fminf(p[0].y, fminf(p[1].y, p[2].y)));
        int bottom = (int)floorf(fmaxf(p[0].y, fmaxf(p[1].y, p[2].y)));
        /* The window reports pointer positions in pixels. Choose an interior pixel
         * with the most edge clearance, rather than a hollow bounding center. */
        for (int y = top; y <= bottom; y++) {
            for (int x = left; x <= right; x++) {
                float clearance = INFINITY;
                for (int j = 0; j < 3; j++) {
                    CF_V2 a = p[j], b = p[(j + 1) % 3];
                    float dx = b.x - a.x, dy = b.y - a.y;
                    float edge = (dx * (y - a.y) - dy * (x - a.x)) / hypotf(dx, dy);
                    clearance = fminf(clearance, area > 0 ? edge : -edge);
                }
                if (clearance > margin) {
                    margin = clearance;
                    best = (CF_V2){ x, y };
                }
            }
        }
    }
    assert(margin > 0);
    return best;
}

static void drag_player(arena_fixture *f)
{
    int index = player_index(f);
    ecs_entity entity = f->scene->actors[index].entity;
    GameTransform before = *GAME_GameTransform_get_const(f->world, entity);
    CF_V2 mouse = player_ribbon_point(f, entity);
    mouse_move_event(mouse);
    frame(f);
    mouse_button_event(true);
    frame(f);
    assert(f->scene->selected == index);
    engine_viewport_pointer start;
    engine_viewport_get_pointer(f->viewport, &start);
    assert(start.pressed && start.hovered);
    mouse_move_event((CF_V2){ mouse.x + 32, mouse.y + 18 });
    frame(f);
    engine_viewport_pointer moved;
    engine_viewport_get_pointer(f->viewport, &moved);
    assert(moved.dragging);
    mouse_button_event(false);
    frame(f);
    GameTransform after = *GAME_GameTransform_get_const(f->world, entity);
    /* Perspective and pane size determine world units per screen pixel. */
    assert(hypotf(after.x - before.x, after.y - before.y) > 0.01f);
    assert(fabsf(after.x - before.x - moved.world_x + start.world_x) < 0.01f);
    assert(fabsf(after.y - before.y + moved.world_y - start.world_y) < 0.01f);
    assert(replay_scene_edit(f->scene, false) == 0);
    GameTransform restored = *GAME_GameTransform_get_const(f->world, entity);
    assert(fabsf(restored.x - before.x) < 0.01f && fabsf(restored.y - before.y) < 0.01f);
    frame(f);
}

static void persistence(arena_fixture *f)
{
    char path[] = "/tmp/arena-ui-scene-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    close(descriptor);
    assert(app_scene_authoring_save(f->scene, path) == 0);
    f->scene->perspective = false;
    assert(app_scene_authoring_load(f->scene, path) == 0);
    assert(f->scene->perspective);
    frame(f);
    assert(engine_viewport_perspective(f->viewport));
    assert_framed(f);
    unlink(path);
}

static void preview_projection(arena_fixture *f)
{
    assert(app_scene_authoring_preview_prototype(f->scene, "Editor2DPreview") == 0);
    frame(f);
    assert(!engine_viewport_perspective(f->viewport));
    app_scene_authoring_stop_prototype_preview(f->scene);
    assert(app_scene_authoring_set_runtime_target(f->scene, APP_RUNTIME_SCENE) == 0);
    frame(f);
    assert(engine_viewport_perspective(f->viewport));
    assert(app_scene_authoring_load(f->scene, "test/fixtures/content/scenes/Editor2D.kdl") == 0);
    frame(f);
    assert(!engine_viewport_perspective(f->viewport));
    assert(app_scene_authoring_preview_prototype(f->scene, "ArenaPlayer") == 0);
    frame(f);
    assert(engine_viewport_perspective(f->viewport));
    app_scene_authoring_stop_prototype_preview(f->scene);
    assert(app_scene_authoring_set_runtime_target(f->scene, APP_RUNTIME_SCENE) == 0);
    frame(f);
    assert(!engine_viewport_perspective(f->viewport));
    assert(app_scene_authoring_load(f->scene, "content/scenes/Arena3D.kdl") == 0);
    frame(f);
    assert(engine_viewport_perspective(f->viewport));
}

static void current_mouse_aim(arena_fixture *f)
{
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    GameTransform position = *GAME_GameTransform_get_const(f->world, player);
    CF_V2 target = { position.x + 120, position.y - 60 };
    CF_V2 mouse = screen_point(f, (CF_V3){ target.x, 0, target.y });
    engine_viewport_pointer previous;
    engine_viewport_get_pointer(f->viewport, &previous);
    mouse_move_event(mouse);
    cf_app_update(NULL);
    app_input_frame();
    viewport_input(f);
    float x, y;
    assert(engine_viewport_mouse_world(f->viewport, &x, &y));
    CF_V2 projected = screen_point(f, (CF_V3){ x, 0, -y });
    /* Events arrive in whole pixels; the world footprint grows with zoom. */
    assert(fabsf(projected.x - mouse.x) < 1.5f && fabsf(projected.y - mouse.y) < 1.5f);
    assert(hypotf(x - previous.world_x, y - previous.world_y) > 10);
    assert(app_scene_authoring_aim_at(f->scene, (CF_V2){ x, -y }, true) == 0);
    draw_frame(f);
    const GameAim *aim = GAME_GameAim_get_const(f->world, player);
    float dx = x - position.x, dy = -y - position.y;
    float length = hypotf(dx, dy);
    assert(
        aim->firing && fabsf(aim->direction.x - dx / length) < 0.0001f &&
        fabsf(aim->direction.y - dy / length) < 0.0001f
    );
}

static int projectiles(arena_fixture *f)
{
    ecs_component_id component = GAME_COMPONENT_GameProjectile.id;
    ecs_query query;
    assert(ecs_query_begin(f->world, &query, &component, 1) == ECS_OK);
    int count = 0;
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        count++;
    }
    ecs_query_end(&query);
    return count;
}

static CF_Pixel *canvas_pixels()
{
    CF_Readback readback = cf_canvas_readback(cf_app_get_canvas());
    assert(readback.id);
    int width = cf_app_get_canvas_width(), height = cf_app_get_canvas_height();
    int size = cf_readback_size(readback);
    assert(size == width * height * (int)sizeof(CF_Pixel));
    CF_Pixel *pixels = malloc((size_t)size);
    assert(pixels);
    uint64_t start = cf_get_ticks();
    while (!cf_readback_ready(readback)) {
        assert(cf_get_ticks() - start < cf_get_tick_frequency() * 10);
        cf_sleep(1);
    }
    assert(cf_readback_data(readback, pixels, size) == size);
    cf_destroy_readback(readback);
    return pixels;
}

static void write_fixture(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static int screen_light(const CF_Pixel *pixels, CF_V2 point)
{
    int width = cf_app_get_canvas_width(), height = cf_app_get_canvas_height();
    int x = (int)(point.x * width / cf_app_get_width());
    int y = (int)(point.y * height / cf_app_get_height());
    assert(x > 0 && x < width - 1 && y > 0 && y < height - 1);
    int light = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            CF_Pixel p = pixels[(y + dy) * width + x + dx];
            light += p.colors.r + p.colors.g + p.colors.b;
        }
    }
    return light / 27;
}

static void planar_orientation(arena_fixture *f)
{
    const char *json =
        "version 1\n"
        "name \"Planar orientation\"\n"
        "presentation \"2d\"\n"
        "(array)entities {\n"
        "    - {\n"
        "        name \"Player\"\n"
        "        prototype \"ArenaPlayer\"\n"
        "        components {\n"
        "            GameTransform x=0 y=0\n"
        "            GameAppearance shape=3 width=80 height=60 "
        "color=4294967295 glow_radius=0 border_width=0 stroke_width=0\n"
        "            GameShader material=\"\" shape=\"\" parameters=\"\"\n"
        "            GameMaterial scene=4294967295 emission_color=4294967295 "
        "emission_intensity=0.1 emission_multiplier=1 absorption=1\n"
        "        }\n"
        "        (array)behaviors \"TwinStickInputBehavior\"\n"
        "    }\n"
        "}\n";
    char path[] = "/tmp/arena-ui-orientation-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0 && close(descriptor) == 0);
    write_fixture(path, json);
    assert(app_scene_authoring_load(f->scene, path) == 0);
    frame(f);
    engine_viewport_set_view(f->viewport, &(engine_viewport_view){ 0, 0, 200, 200 });
    frame(f);
    assert(!engine_viewport_perspective(f->viewport));
    const CF_V2 directions[] = { { 1, -1 }, { -1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        CF_V2 aim = directions[i];
        float length = hypotf(aim.x, aim.y);
        aim.x /= length;
        aim.y /= length;
        assert(
            app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0
        );
        frame(f); /* Establish the resumed wall clock before its first fixed tick. */
        assert(app_scene_authoring_aim(f->scene, aim, false) == FLOW_OK);
        frame(f);
        assert(
            app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_PAUSE) == 0
        );
        f->scene->selected = -1;
        f->scene->actors[0].marked = false;
        frame(f);
        engine_viewport_camera view;
        engine_viewport_get_camera(f->viewport, &view);
        CF_V2 tip = { 24 * aim.x, 24 * aim.y };
        CF_V2 empty = { tip.x - 18 * aim.y, tip.y + 18 * aim.x };
        CF_V2 points[] = { empty, tip };
        CF_V2 screen[2];
        for (int j = 0; j < 2; j++) {
            screen[j] = screen_ndc(
                f,
                (CF_V2){ 2 * (points[j].x - view.center_x) / view.span_width,
                         2 * (-points[j].y - view.center_y) / view.span_height }
            );
        }
        CF_Pixel *pixels = canvas_pixels();
        int outside = screen_light(pixels, screen[0]);
        int inside = screen_light(pixels, screen[1]);
        free(pixels);
        if (inside < 100 || outside > inside - 50) {
            fprintf(stderr, "Planar heading %d: tip brightness %d, empty corner %d\n", i, inside, outside);
        }
        assert(inside >= 100 && outside <= inside - 50);
        for (int j = 0; j < 2; j++) {
            mouse_move_event(screen[j]);
            frame(f);
            mouse_button_event(true);
            frame(f);
            mouse_button_event(false);
            frame(f);
            assert(f->scene->selected == (j ? 0 : -1));
        }
    }
    assert(app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    assert(unlink(path) == 0);
    assert(app_scene_authoring_load(f->scene, "content/scenes/Arena3D.kdl") == 0);
    frame(f);
    puts("planar orientation PASS: authored aim, rendered tips and empty-corner picking in four quadrants");
}

static int standalone_frame(arena_fixture *f, app_game_presentation *game)
{
    cf_app_update(NULL);
    app_input_frame();
    f->time += 1.0 / 60;
    int result = app_game_presentation_frame(game, f->time);
    cf_app_draw_onto_screen(true);
    return result;
}

static void editor_mouse_play(arena_fixture *f)
{
    /* Studio gives the Scene area the keyboard once it is clicked; a playing
     * game then receives bare keys. */
    f->focused = true;
    f->sample_controls = true;
    window_focus_event(true);
    assert(app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    /* Exercise the real editor/player route without wave initialization or
     * enemy contacts interrupting the measured movement frames. */
    for (int i = 0; i < acount(f->scene->actors); i++) {
        ecs_entity entity = f->scene->actors[i].entity;
        if (GAME_GameEncounter_has(f->world, entity)) {
            assert(game_behavior_detach_asset(f->live->runtime->behaviors, entity, "ArenaWaveBehavior") == 1);
        }
    }
    engine_set_focus(f->runtime, ENGINE_FOCUS_VIEWPORT);
    frame(f);
    assert(app_scene_authoring_runtime_state(f->scene).playing);
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    GameTransform initial = *GAME_GameTransform_get_const(f->world, player);
    CF_V2 target = screen_point(f, (CF_V3){ initial.x + 150, 0, initial.y });
    mouse_move_event(target);
    frame(f);
    key_event(SDL_SCANCODE_D, true);
    for (int i = 0; i < 4; i++) {
        frame(f);
    }
    assert(GAME_GameTransform_get_const(f->world, player)->x > initial.x + 8);
    mouse_button_event(true);
    GameTransform before = *GAME_GameTransform_get_const(f->world, player);
    const SDL_Scancode keys[] = { SDL_SCANCODE_D, SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S };
    const CF_V2 directions[] = { { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 } };
    for (int i = 0; i < 32; i++) {
        int direction = i / 8;
        if (i && i % 8 == 0) {
            key_event(keys[direction - 1], false);
            key_event(keys[direction], true);
        }
        if (i == 5) {
            mouse_move_event((CF_V2){ target.x - 35, target.y + 25 });
        }
        frame(f);
        assert(cf_mouse_down(CF_MOUSE_BUTTON_LEFT));
        assert(engine_viewport_focused(f->viewport) && !editor_host_captures_keyboard());
        assert(app_scene_authoring_runtime_state(f->scene).playing);
        assert(engine_get_focus(f->runtime) == ENGINE_FOCUS_VIEWPORT);
        CF_V2 move = GAME_GameInput_get_const(f->world, player)->move;
        assert(
            move.x == directions[direction].x && move.y == directions[direction].y
        );
        assert(GAME_GameAim_get_const(f->world, player)->firing);
        GameTransform after = *GAME_GameTransform_get_const(f->world, player);
        float distance = (after.x - before.x) * move.x + (after.y - before.y) * move.y;
        assert(distance > 2);
        before = after;
    }
    assert(projectiles(f) >= 3);
    engine_viewport_pointer pointer;
    engine_viewport_get_pointer(f->viewport, &pointer);
    assert(pointer.dragging);
    mouse_button_event(false);
    for (int i = 0; i < 4; i++) {
        frame(f);
        assert(!GAME_GameAim_get_const(f->world, player)->firing);
        assert(GAME_GameTransform_get_const(f->world, player)->y > before.y + 2);
        before = *GAME_GameTransform_get_const(f->world, player);
    }
    key_event(SDL_SCANCODE_S, false);
    frame(f);
    CF_V2 move = GAME_GameInput_get_const(f->world, player)->move;
    assert(move.x == 0 && move.y == 0);
    assert(app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    assert(!app_scene_authoring_runtime_state(f->scene).playing);
    f->sample_controls = false;
    puts("editor viewport input PASS: editor Play, native held mouse firing with movement, aim drag, repeat shots and release");
}

static int standalone_fire(arena_fixture *f, app_game_presentation *game, bool mouse)
{
    assert(app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_RESTART) == 0);
    assert(standalone_frame(f, game) == 0);
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    /* The shipped scene maps Space to Dash. Give this input-parity fixture
     * an explicit keyboard Fire binding instead of assuming game controls. */
    bool keyboard_binding = false;
    for (int i = 0; i < acount(f->scene->actors); i++) {
        GameActionBinding
            *binding = GAME_GameActionBinding_get(f->world, f->scene->actors[i].entity);
        if (binding && binding->source == GAME_CONTROL_SPACE) {
            binding->action = "Fire";
            binding->firing = true;
            keyboard_binding = true;
        }
    }
    assert(keyboard_binding);
    if (mouse) {
        mouse_button_event(true);
    } else {
        key_event(SDL_SCANCODE_SPACE, true);
    }
    for (int i = 0; i < 17; i++) {
        assert(standalone_frame(f, game) == 0);
        assert(GAME_GameAim_get_const(f->world, player)->firing);
    }
    int shots = projectiles(f);
    assert(shots >= 2);
    if (mouse) {
        mouse_button_event(false);
    } else {
        key_event(SDL_SCANCODE_SPACE, false);
    }
    for (int i = 0; i < 6; i++) {
        assert(standalone_frame(f, game) == 0);
        assert(!GAME_GameAim_get_const(f->world, player)->firing);
    }
    assert(projectiles(f) == shots);
    return shots;
}

static int standalone_key(arena_fixture *f, app_game_presentation *game, SDL_Scancode key, bool down)
{
    key_event(key, down);
    /* Cute polls SDL after ImGui NewFrame. The first frame delivers the native
     * event; the second processes the pause menu's ImGui shortcut. */
    assert(standalone_frame(f, game) == 0);
    return standalone_frame(f, game);
}

static void standalone_play(arena_fixture *f)
{
    assert(app_scene_authoring_load(f->scene, "content/scenes/Arena3D.kdl") == 0);
    /* Projectile lifetime is part of the firing comparison below. */
    for (int i = 0; i < acount(f->scene->actors); i++) {
        if (!strcmp(f->scene->actors[i].prototype_name, "PhaseWarden")) {
            f->scene->selected = i;
            assert(app_scene_authoring_delete_instance(f->scene) == 0);
            break;
        }
    }
    cf_app_set_size(1280, 960);
    app_game_presentation *game = app_game_presentation_new(f->scene, f->viewport, true);
    assert(game);
    window_focus_event(true);
    cf_draw3d_stats();
    for (int i = 0; i < 4; i++) {
        assert(standalone_frame(f, game) == 0);
    }
    assert_framed(f);
    assert(cf_draw3d_stats().instances > 0);
    assert(app_scene_authoring_runtime_state(f->scene).playing);
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    GameTransform authored = *GAME_GameTransform_get_const(f->world, player);
    render_arena_camera view = camera(f);
    CF_V3
    aim = render_arena_project(&view, (CF_V3){ authored.x + 100, 0, authored.y });
    ImVec2 display = ImGui_GetIO()->DisplaySize;
    ImGuiIO_AddMousePosEvent(
        ImGui_GetIO(),
        (aim.x * 0.5f + 0.5f) * display.x,
        (0.5f - aim.y * 0.5f) * display.y
    );
    assert(standalone_frame(f, game) == 0);
    const GameAim *direction = GAME_GameAim_get_const(f->world, player);
    assert(direction->direction.x > 0.99f && fabsf(direction->direction.y) < 0.02f);
    int keyboard_shots = standalone_fire(f, game, false);
    assert(standalone_fire(f, game, true) == keyboard_shots);
    player = f->scene->actors[player_index(f)].entity;
    mouse_button_event(true);
    assert(standalone_frame(f, game) == 0);
    assert(GAME_GameAim_get_const(f->world, player)->firing);
    assert(standalone_key(f, game, SDL_SCANCODE_ESCAPE, true) == 0);
    assert(!app_scene_authoring_runtime_state(f->scene).playing);
    assert(engine_get_focus(f->runtime) == ENGINE_FOCUS_NONE);
    assert(!GAME_GameAim_get_const(f->world, player)->firing);
    const GameInput *input = GAME_GameInput_get_const(f->world, player);
    assert(input->move.x == 0 && input->move.y == 0);
    double paused = engine_last_timing(f->runtime)->simulation_time;
    mouse_button_event(false);
    assert(standalone_key(f, game, SDL_SCANCODE_ESCAPE, false) == 0);
    for (int i = 0; i < 4; i++) {
        assert(standalone_frame(f, game) == 0);
    }
    assert(engine_last_timing(f->runtime)->simulation_time == paused);
    assert(standalone_key(f, game, SDL_SCANCODE_RETURN, true) == 0);
    assert(app_scene_authoring_runtime_state(f->scene).playing);
    assert(standalone_key(f, game, SDL_SCANCODE_RETURN, false) == 0);
    assert(!GAME_GameAim_get_const(f->world, player)->firing);
    mouse_button_event(true);
    assert(standalone_frame(f, game) == 0);
    assert(GAME_GameAim_get_const(f->world, player)->firing);
    window_focus_event(false);
    assert(standalone_frame(f, game) == 0);
    assert(!app_scene_authoring_runtime_state(f->scene).playing);
    assert(!GAME_GameAim_get_const(f->world, player)->firing);
    assert(engine_get_focus(f->runtime) == ENGINE_FOCUS_NONE);
    mouse_button_event(false);
    window_focus_event(true);
    assert(standalone_key(f, game, SDL_SCANCODE_RETURN, true) == 0);
    assert(standalone_key(f, game, SDL_SCANCODE_RETURN, false) == 0);
    assert(app_scene_authoring_runtime_state(f->scene).playing);
    assert(!GAME_GameAim_get_const(f->world, player)->firing);
    GAME_GameTransform_get(f->world, player)->x += 50;
    assert(standalone_key(f, game, SDL_SCANCODE_R, true) == 0);
    assert(!ecs_entity_is_alive(f->world, player));
    player = f->scene->actors[player_index(f)].entity;
    const GameTransform *restored = GAME_GameTransform_get_const(f->world, player);
    assert(restored->x == authored.x && restored->y == authored.y);
    assert(app_scene_authoring_runtime_state(f->scene).playing);
    assert(standalone_key(f, game, SDL_SCANCODE_R, false) == 0);
    assert(standalone_key(f, game, SDL_SCANCODE_ESCAPE, true) == 0);
    assert(!app_scene_authoring_runtime_state(f->scene).playing);
    assert(standalone_key(f, game, SDL_SCANCODE_ESCAPE, false) == 0);
    assert(standalone_key(f, game, SDL_SCANCODE_Q, true) == 1);
    key_event(SDL_SCANCODE_Q, false);
    app_game_presentation_free(game);
    puts("standalone game PASS: Surface rendering, mouse aim, held mouse/Space cadence and release, window focus loss, Escape pause/release, paused time, Enter resume, authored restart and quit");
}

/* ---------- arena2d end to end (wave 3, workstream X) ---------- */

static void text_event(const char *text)
{
    SDL_Event event = { .type = SDL_EVENT_TEXT_INPUT };
    event.text.windowID = SDL_GetWindowID(cf_app_get_window());
    event.text.text = text;
    assert(SDL_PushEvent(&event));
}

/* Whether an activation reached `probe`'s entity within a few frames: a key
   reaches ImGui a frame after SDL has it, and its activation reaches the
   entity's behaviors at the next document update. */
static bool activated(arena_fixture *f, app_game_presentation *game, const ui_probe *probe)
{
    int before = probe->count;
    bool seen = false;
    for (int i = 0; i < 4; i++) {
        assert(standalone_frame(f, game) == 0);
        seen |= probe->count > before;
    }
    return seen;
}

/* The standalone player gives its game documents the keyboard while the
   game plays: a data-keys binding hears its key, and a focused text field
   takes typed text and the letters the player's shortcuts and gameplay
   would, while Escape still pauses. */
static void standalone_ui_keys(arena_fixture *f)
{
    char path[1024], styles[1024];
    snprintf(path, sizeof path, "%s/ui/KeyProbe.rml", f->scene->content_root);
    snprintf(styles, sizeof styles, "%s/ui/KeyProbe.rcss", f->scene->content_root);
    text_overlay_set(
        path,
        "<div id=\"probe\" autofocus data-keys=\"K:Knock\">\n"
        "  <input id=\"name\" type=\"text\"/>\n"
        "</div>\n"
    );
    text_overlay_set(styles, "#probe { display: block; tab-index: auto; }\n");
    app_game_presentation *game = app_game_presentation_new(f->scene, f->viewport, true);
    assert(game);
    window_focus_event(true);
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    assert(GAME_GameUi_add(f->world, player, 0) == ECS_OK);
    assert(ecs_world_apply_commands(f->world) == ECS_OK);
    *GAME_GameUi_get(f->world, player) = (GameUi){ .document = "KeyProbe" };
    ui_probe heard = { 0 };
    const char *const knock[] = { "Knock" };
    test_ui_probe_events(f->live, player, knock, 1, &heard);
    bool quiet = !activated(f, game, &heard);
    key_event(SDL_SCANCODE_K, true);
    key_event(SDL_SCANCODE_K, false);
    bool knocked = activated(f, game, &heard);
    assert(quiet && knocked);

    game_ui_focus(engine_ui(f->runtime), ecs_entity_to_u64(player), "#name");
    for (int i = 0; i < 2; i++) {
        assert(standalone_frame(f, game) == 0);
    }
    render_ui *ui = engine_viewport_ui(f->viewport);
    bool typing = engine_viewport_ui_typing(f->viewport) && render_ui_wants_text(ui);
    key_event(SDL_SCANCODE_R, true);
    text_event("r");
    key_event(SDL_SCANCODE_R, false);
    key_event(SDL_SCANCODE_D, true);
    text_event("d");
    for (int i = 0; i < 3; i++) {
        assert(standalone_frame(f, game) == 0);
    }
    rml_element *focus = rml_context_focus_element(render_ui_context(ui));
    char value[16] = "";
    rml_element_value(focus, value, sizeof value);
    bool kept = ecs_entity_is_alive(f->world, player) &&
                app_scene_authoring_runtime_state(f->scene).playing &&
                GAME_GameInput_get_const(f->world, player)->move.x == 0;
    key_event(SDL_SCANCODE_D, false);
    assert(standalone_key(f, game, SDL_SCANCODE_ESCAPE, true) == 0);
    bool paused = !app_scene_authoring_runtime_state(f->scene).playing;
    assert(standalone_key(f, game, SDL_SCANCODE_ESCAPE, false) == 0);
    if (!(typing && !strcmp(value, "rd") && kept && paused)) {
        fprintf(stderr, "typing %d value '%s' kept %d paused %d\n", typing, value, kept, paused);
    }
    assert(typing && !strcmp(value, "rd") && kept && paused);
    app_game_presentation_free(game);
    text_overlay_set(path, NULL);
    text_overlay_set(styles, NULL);
    puts("standalone game UI keys PASS: data-keys while playing, a focused text field takes typed text, R and movement, Escape still pauses");
}

static bool review_target(arena_fixture *f, GameTransform *player, GameTransform *target)
{
    ecs_component_id required[] = { GAME_COMPONENT_GameInput.id,
                                    GAME_COMPONENT_GameTransform.id };
    ecs_query query;
    assert(ecs_query_begin(f->world, &query, required, 2) == ECS_OK);
    ecs_entity entity;
    bool found = ecs_query_next(&query, &entity);
    if (found) {
        *player = *GAME_GameTransform_get_const(f->world, entity);
    }
    ecs_query_end(&query);
    if (!found) {
        return false;
    }
    *target = (GameTransform){ player->x + 80, player->y };
    required[0] = GAME_COMPONENT_GameEncounterMember.id;
    assert(ecs_query_begin(f->world, &query, required, 2) == ECS_OK);
    float nearest = INFINITY;
    while (ecs_query_next(&query, &entity)) {
        if (GAME_GameEncounterMember_get_const(f->world, entity)->role != GAME_ENCOUNTER_ENEMY ||
            GAME_GameSpawnRequest_has(f->world, entity) ||
            ecs_entity_is_retiring(f->world, entity)) {
            continue;
        }
        GameTransform position = *GAME_GameTransform_get_const(f->world, entity);
        float distance = hypotf(position.x - player->x, position.y - player->y);
        if (distance < nearest) {
            nearest = distance;
            *target = position;
            const GameVelocity *velocity = GAME_GameVelocity_get_const(f->world, entity);
            if (velocity) {
                float lead = fminf(distance / 480, .25f);
                target->x += velocity->x * lead;
                target->y += velocity->y * lead;
            }
        }
    }
    ecs_query_end(&query);
    return true;
}

static void review_input(arena_fixture *f, int *waypoint, bool held[5], int review_frame, bool shooting)
{
    static const GameTransform route[] = { { 520, -286 }, { 575, -176 },
                                           { 520, -66 },  { 323, -46 },
                                           { 125, -66 },  { 70, -176 },
                                           { 125, -286 }, { 323, -306 } };
    static const SDL_Scancode keys[] = { SDL_SCANCODE_A,
                                         SDL_SCANCODE_D,
                                         SDL_SCANCODE_W,
                                         SDL_SCANCODE_S,
                                         SDL_SCANCODE_SPACE };
    bool desired[5] = { 0 };
    GameTransform player, target;
    if (review_target(f, &player, &target)) {
        GameTransform goal = route[*waypoint];
        if (hypotf(goal.x - player.x, goal.y - player.y) < 35) {
            *waypoint = (*waypoint + 1) % 8;
            goal = route[*waypoint];
        }
        CF_V2 movement = cf_norm_v2((CF_V2){ goal.x - player.x, goal.y - player.y });
        ecs_component_id required[] = { GAME_COMPONENT_GameTransform.id,
                                        GAME_COMPONENT_GameEncounterMember.id };
        ecs_query query;
        assert(ecs_query_begin(f->world, &query, required, 2) == ECS_OK);
        ecs_entity entity;
        while (ecs_query_next(&query, &entity)) {
            if (GAME_GameEncounterMember_get_const(f->world, entity)->role !=
                    GAME_ENCOUNTER_ENEMY ||
                ecs_entity_is_retiring(f->world, entity)) {
                continue;
            }
            const GameTransform *enemy = GAME_GameTransform_get_const(f->world, entity);
            CF_V2 away = { player.x - enemy->x, player.y - enemy->y };
            float distance = cf_len_v2(away);
            if (distance > 0 && distance < 105) {
                float proximity = 1 - distance / 105;
                movement = cf_add_v2(movement, cf_mul_v2_f(away, 5 * proximity * proximity / distance));
            }
        }
        ecs_query_end(&query);
        /* Keep avoidance inside the authored boundary, including its sloping sides. */
        for (int i = 0; i < acount(f->scene->actors); i++) {
            entity = f->scene->actors[i].entity;
            const GameBody *body = GAME_GameBody_get_const(f->world, entity);
            const GameCollider *wall = GAME_GameCollider_get_const(f->world, entity);
            if (!body || body->type != GAME_BODY_STATIC || !wall ||
                wall->shape != GAME_COLLIDER_BOX) {
                continue;
            }
            const GameTransform *center = GAME_GameTransform_get_const(f->world, entity);
            float angle = GAME_GameOrientation_get_const(f->world, entity)->radians;
            CF_V2 inward = { -sinf(angle), cosf(angle) };
            if ((323 - center->x) * inward.x + (-176 - center->y) * inward.y < 0) {
                inward = cf_mul_v2_f(inward, -1);
            }
            float clearance = (player.x - center->x) * inward.x +
                              (player.y - center->y) * inward.y - wall->height * .5f;
            if (clearance < 45) {
                movement = cf_add_v2(movement, cf_mul_v2_f(inward, 4 * (1 - clearance / 45)));
            }
        }
        movement = cf_safe_norm_v2(movement);
        desired[0] = movement.x < -.22f;
        desired[1] = movement.x > .22f;
        desired[2] = movement.y < -.22f;
        desired[3] = movement.y > .22f;
        /* Give the authored wave time to gather while the player keeps moving. */
        desired[4] = shooting && review_frame >= 360;
        render_arena_camera view = camera(f);
        CF_V3 aim = render_arena_project(&view, (CF_V3){ target.x, 0, target.y });
        mouse_move_event(screen_ndc(f, (CF_V2){ aim.x, aim.y }));
    }
    for (int i = 0; i < 5; i++) {
        if (desired[i] != held[i]) {
            key_event(keys[i], desired[i]);
            held[i] = desired[i];
        }
    }
}

static GameEncounter review_encounter(ecs_world *world)
{
    ecs_component_id component = GAME_COMPONENT_GameEncounter.id;
    ecs_query query;
    assert(ecs_query_begin(world, &query, &component, 1) == ECS_OK);
    ecs_entity entity;
    assert(ecs_query_next(&query, &entity));
    GameEncounter encounter = *GAME_GameEncounter_get_const(world, entity);
    ecs_query_end(&query);
    return encounter;
}

static int review_shards(arena_fixture *f)
{
    ecs_component_id component = GAME_COMPONENT_GameVisualTransform.id;
    ecs_query query;
    assert(ecs_query_begin(f->world, &query, &component, 1) == ECS_OK);
    ecs_entity entity;
    int count = 0;
    while (ecs_query_next(&query, &entity)) {
        for (int i = 0;
             i < game_behavior_attachment_count_for_entity(f->live->runtime->behaviors, entity);
             i++) {
            game_behavior_attachment_info info;
            assert(game_behavior_attachment_info_at(f->live->runtime->behaviors, entity, i, &info) == 0);
            if (!strcmp(info.asset_name, "ArenaShardBehavior")) {
                count++;
                break;
            }
        }
    }
    ecs_query_end(&query);
    return count;
}

static int compare_frame_time(const void *a, const void *b)
{
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

static int compare_behavior_time(const void *a, const void *b)
{
    double left = ((const game_behavior_profile_row *)a)->cpu_ms;
    double right = ((const game_behavior_profile_row *)b)->cpu_ms;
    return (left < right) - (left > right);
}

static void review_behavior_totals(game_behavior_profile_frame *total, const game_behavior_profile_frame *frame)
{
    for (int i = 0; i < frame->count; i++) {
        const game_behavior_profile_row *source = &frame->rows[i];
        int index = 0;
        while (index < total->count &&
               (strcmp(total->rows[index].behavior, source->behavior) ||
                strcmp(total->rows[index].event, source->event))) {
            index++;
        }
        assert(index < GAME_BEHAVIOR_PROFILE_MAX);
        if (index == total->count) {
            total->rows[index] = (game_behavior_profile_row){
                .behavior = source->behavior,
                .event = source->event,
            };
            total->count++;
        }
        total->rows[index].cpu_ms += source->cpu_ms;
        total->rows[index].setup_ms += source->setup_ms;
        total->rows[index].dispatches += source->dispatches;
        total->rows[index].starts += source->starts;
        total->rows[index].resumes += source->resumes;
    }
    total->cpu_ms += frame->cpu_ms;
    total->setup_ms += frame->setup_ms;
    total->overflow.cpu_ms += frame->overflow.cpu_ms;
    total->overflow.setup_ms += frame->overflow.setup_ms;
}

int arena_play_review_run(const char *program, bool editor_mode, bool crowd)
{
    enum
    {
        REVIEW_FRAMES = 1800,
        WARMUP_FRAMES = 180
    };
    CF_Result result = cf_make_app(
        "Arena play review",
        0,
        0,
        0,
        1920,
        1080,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "Arena review app: %s\n", result.details);
        return 1;
    }
    bool immediate = cf_app_set_present_mode(CF_PRESENT_MODE_IMMEDIATE);
    printf(
        "Arena review present mode: %s (immediate accepted: %s)\n",
        cf_present_mode_string(cf_app_get_present_mode()),
        immediate ? "yes" : "no"
    );
    arena_app_start();
    arena_fixture f = fixture_new();
    f.profile_behaviors = crowd;
    game_behavior_set_profiling(f.live->runtime->behaviors, crowd);
    app_game_presentation *game = NULL;
    if (editor_mode) {
        for (int i = 0; i < 30; i++) {
            frame(&f);
        }
        f.sample_controls = true;
        window_focus_event(true);
        assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
        /* Inspect the player's open weapon graph, as selecting it in Studio does. */
        select_instance(f.scene, player_index(&f));
        f.focused = true;
        frame(&f);
        frame(&f);
        mouse_move_event(screen_ndc(&f, (CF_V2){ 0, 0 }));
        frame(&f);
        mouse_button_event(true);
        frame(&f);
        frame(&f);
        mouse_button_event(false);
        frame(&f);
        frame(&f);
        assert(engine_viewport_focused(f.viewport) && !editor_host_captures_keyboard());
    } else {
        game = app_game_presentation_new(f.scene, f.viewport, false);
        assert(game);
        assert(standalone_frame(&f, game) == 0);
    }
    const char *capture = getenv("CHIBI_ARENA_PLAY_CAPTURE");
    const char *impact_capture = getenv("CHIBI_ARENA_IMPACT_CAPTURE");
    const char *video_directory = getenv("CHIBI_ARENA_VIDEO_FRAMES");
    int video_frames = 0;
    double times[REVIEW_FRAMES];
    double stages[ARENA_STAGE_COUNT] = { 0 }, runtime[3] = { 0 };
    double crowd_simulation[6] = { 0 }, crowd_behavior[6] = { 0 },
           crowd_physics[6] = { 0 };
    int crowd_samples[6] = { 0 };
    game_behavior_profile_frame behavior_totals = { 0 };
    int samples = 0, waypoint = 0, peak_enemies = 0, peak_shots = 0,
        peak_particles = 0, peak_shards = 0;
    int peak_kills = 0, best_score = 0, best_impact = 0, peak_draws = 0;
    int previous_shards = 0, impact_frames = 0;
    bool held[5] = { 0 };
    bool captured = false, impact_captured = false;
    GameTransform initial, target;
    assert(review_target(&f, &initial, &target));
    float travel = 0;
    GameTransform previous = initial;
    GameEncounter encounter = { 0 };
    cf_draw3d_stats();
    for (int i = 0; i < REVIEW_FRAMES; i++) {
        uint64_t start = cf_get_ticks();
        review_input(&f, &waypoint, held, i, !crowd);
        if (editor_mode && i == 360) {
            float x, y;
            assert(engine_viewport_focused(f.viewport) && !editor_host_captures_keyboard());
            assert(engine_viewport_mouse_world(f.viewport, &x, &y));
        }
        if (editor_mode) {
            frame(&f);
        } else {
            assert(standalone_frame(&f, game) == 0);
        }
        double elapsed = 1000.0 * (double)(cf_get_ticks() - start) /
                         cf_get_tick_frequency();
        encounter = review_encounter(f.world);
        if (i >= WARMUP_FRAMES && encounter.status == GAME_ENCOUNTER_ACTIVE) {
            times[samples++] = elapsed;
            if (crowd && encounter.alive <= 40) {
                int bucket = encounter.alive / 8;
                const engine_frame_timing *timing = engine_last_timing(f.runtime);
                crowd_simulation[bucket] += timing->behavior_ms +
                                            timing->physics_ms + timing->systems_ms;
                crowd_behavior[bucket] += timing->behavior_ms;
                crowd_physics[bucket] += timing->physics_ms;
                crowd_samples[bucket]++;
                if (encounter.alive == 40) {
                    review_behavior_totals(&behavior_totals, engine_behavior_profile(f.runtime));
                }
            }
            if (editor_mode) {
                for (int stage = 0; stage < ARENA_STAGE_COUNT; stage++) {
                    stages[stage] += f.stages[stage];
                }
                const engine_frame_timing *timing = engine_last_timing(f.runtime);
                runtime[0] += timing->behavior_ms;
                runtime[1] += timing->physics_ms;
                runtime[2] += timing->systems_ms;
            }
        }
        CF_DrawStats3d stats = cf_draw3d_stats();
        if (stats.commands > peak_draws) {
            peak_draws = stats.commands;
        }
        int enemies = encounter.alive;
        int shots = projectiles(&f);
        int particles = app_scene_renderer_particle_count(f.scene->renderer);
        int shards = review_shards(&f);
        int kills = encounter.spawned - enemies;
        if (shards > previous_shards) {
            impact_frames = 6;
        } else if (impact_frames > 0) {
            impact_frames--;
        }
        previous_shards = shards;
        if (enemies > peak_enemies) {
            peak_enemies = enemies;
        }
        if (shots > peak_shots && (!crowd || i >= WARMUP_FRAMES)) {
            peak_shots = shots;
        }
        if (particles > peak_particles) {
            peak_particles = particles;
        }
        if (shards > peak_shards) {
            peak_shards = shards;
        }
        if (kills > peak_kills) {
            peak_kills = kills;
        }
        GameTransform player;
        if (review_target(&f, &player, &target)) {
            travel += hypotf(player.x - previous.x, player.y - previous.y);
            previous = player;
        }
        /* Capture real played states only. File writes and readback are outside
         * the timing sample, and no capture happens without an explicit path. */
        bool active = encounter.status == GAME_ENCOUNTER_ACTIVE &&
                      enemies >= 3 && shots > 0;
        int score = enemies * 8 + shots + (particles < 120 ? particles : 120) / 12;
        if (capture && *capture && active && i >= WARMUP_FRAMES && score > best_score) {
            assert(app_save_screenshot(capture) == 0);
            best_score = score;
            captured = true;
        }
        int impact_score = shards * 8 + (particles < 120 ? particles : 120);
        if (impact_capture && *impact_capture && active && impact_frames > 0 &&
            (!impact_captured || impact_score > best_impact)) {
            assert(app_save_screenshot(impact_capture) == 0);
            best_impact = impact_score;
            impact_captured = true;
        }
        /* Ten seconds of the same played simulation, sampled at 30 fps. */
        if (video_directory && *video_directory && i >= 300 && i < 900 && i % 2 == 0) {
            char path[1024];
            int length = snprintf(path, sizeof path, "%s/%04d.png", video_directory, video_frames++);
            assert(length > 0 && (size_t)length < sizeof path);
            assert(app_save_screenshot(path) == 0);
        }
    }
    assert(samples > 0);
    qsort(times, (size_t)samples, sizeof *times, compare_frame_time);
    bool ok = peak_enemies > 0 && peak_shots > 0 && peak_kills > 0 && travel > 50;
    if (crowd) {
        ok = peak_enemies == 40 && !peak_shots && !peak_kills &&
             crowd_samples[5] >= 300;
        for (int i = 0; i < 6; i++) {
            if (crowd_samples[i]) {
                printf(
                    "Crowd %d..%d enemies: %d samples, simulation %.3f ms, "
                    "behaviors %.3f ms, physics %.3f ms\n",
                    i * 8,
                    i == 5 ? 40 : i * 8 + 7,
                    crowd_samples[i],
                    crowd_simulation[i] / crowd_samples[i],
                    crowd_behavior[i] / crowd_samples[i],
                    crowd_physics[i] / crowd_samples[i]
                );
            }
        }
        qsort(
            behavior_totals.rows,
            (size_t)behavior_totals.count,
            sizeof *behavior_totals.rows,
            compare_behavior_time
        );
        for (int i = 0; i < behavior_totals.count; i++) {
            const game_behavior_profile_row *row = &behavior_totals.rows[i];
            printf(
                "Crowd behavior %s / %s: %.3f exec ms, %.3f setup ms per "
                "frame, %.1f starts, %.1f "
                "resumes\n",
                row->behavior,
                row->event,
                row->cpu_ms / crowd_samples[5],
                row->setup_ms / crowd_samples[5],
                (double)row->starts / crowd_samples[5],
                (double)row->resumes / crowd_samples[5]
            );
        }
        printf(
            "Crowd behavior unassigned overhead: %.3f ms per frame; overflow "
            "%.3f "
            "ms\n",
            (crowd_behavior[5] - behavior_totals.cpu_ms - behavior_totals.setup_ms) /
                crowd_samples[5],
            behavior_totals.overflow.cpu_ms / crowd_samples[5]
        );
    }
    ok &= !capture || !*capture || captured;
    ok &= !impact_capture || !*impact_capture || impact_captured;
    printf(
        "arena play review %s: 1920x1080, %d fixed 1/60 frames, %d warm-up "
        "frames, "
        "%d active timing samples; host wall frame p50 %.3f ms, p95 %.3f ms, "
        "p99 %.3f ms "
        "(capture excluded, not GPU pass timing); peaks %d enemies, %d "
        "projectiles, "
        "%d particles, %d beam shards, %d draws; %d kills, player travel %.1f, "
        "final encounter "
        "%d\n",
        ok ? "PASS" : "FAIL",
        REVIEW_FRAMES,
        WARMUP_FRAMES,
        samples,
        times[samples / 2],
        times[(samples - 1) * 95 / 100],
        times[(samples - 1) * 99 / 100],
        peak_enemies,
        peak_shots,
        peak_particles,
        peak_shards,
        peak_draws,
        peak_kills,
        travel,
        encounter.status
    );
    if (editor_mode) {
        printf(
            "Editor CPU scopes (mean ms): input %.3f, documents %.3f, "
            "simulation %.3f, scene %.3f, present %.3f; behavior "
            "%.3f, physics %.3f, other systems %.3f\n",
            stages[ARENA_STAGE_INPUT] / samples,
            stages[ARENA_STAGE_DOCUMENTS] / samples,
            stages[ARENA_STAGE_SIMULATION] / samples,
            stages[ARENA_STAGE_SCENE] / samples,
            stages[ARENA_STAGE_PRESENT] / samples,
            runtime[0] / samples,
            runtime[1] / samples,
            runtime[2] / samples
        );
    }
    if (video_frames) {
        printf("arena review video: %d frames at 30 fps, simulation seconds 5 through 15\n", video_frames);
    }
    app_game_presentation_free(game);
    fixture_free(&f);
    arena_app_stop();
    return !ok;
}

static void ship_motion_review(arena_fixture *f, const char *directory)
{
    const char *json = "version 1\n"
                       "name \"Ship motion review\"\n"
                       "presentation \"3d\"\n"
                       "(array)entities {\n"
                       "    - {\n"
                       "        name \"Player\"\n"
                       "        prototype \"ArenaPlayer\"\n"
                       "        (object)components\n"
                       "        (array)behaviors \"TwinStickInputBehavior\" "
                       "\"ArenaShipMotionBehavior\"\n"
                       "    }\n"
                       "    - {\n"
                       "        name \"Environment\"\n"
                       "        prototype \"ArenaEnvironment\"\n"
                       "        (object)components\n"
                       "        (array)behaviors\n"
                       "    }\n"
                       "}\n";
    char path[] = "/tmp/arena-motion-review-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0 && close(descriptor) == 0);
    write_fixture(path, json);
    assert(app_scene_authoring_load(f->scene, path) == 0);
    assert(unlink(path) == 0);
    assert(app_scene_authoring_runtime_command(f->scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    frame(f);
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    float previous = 0;
    float max_step = 0;
    for (int i = 0; i < 720; i++) {
        CF_V2 movement = i < 210 ? (CF_V2){ 0, 1 }
                       : i < 300 ? (CF_V2){ 0, -1 }
                       : i < 420 ? (CF_V2){ 0 }
                       : i < 540 ? (CF_V2){ -1, 0 }
                       : i < 660 ? (CF_V2){ 1, 0 }
                                 : (CF_V2){ 0 };
        assert(app_scene_authoring_move(f->scene, movement.x, movement.y) == 0);
        assert(app_scene_authoring_aim(f->scene, (CF_V2){ 1, 0 }, false) == 0);
        const GameTransform *position = GAME_GameTransform_get_const(f->world, player);
        engine_viewport_set_view(
            f->viewport,
            &(engine_viewport_view){
                position->x,
                -position->y + 12,
                140,
                100,
            }
        );
        frame(f);
        const GameVisualTransform *visual = GAME_GameVisualTransform_get_const(f->world, player);
        max_step = fmaxf(max_step, fabsf(visual->roll - previous));
        previous = visual->roll;
        if (i == 209 || i == 299 || i == 419 || i == 539 || i == 659) {
            printf(
                "Ship review %.2fs: roll %.2f degrees, pitch %.2f degrees\n",
                (i + 1) / 60.0,
                visual->roll * 180 / CF_PI,
                visual->pitch * 180 / CF_PI
            );
        }
        if (i % 2 == 0) {
            char capture[1024];
            int length = snprintf(capture, sizeof capture, "%s/%04d.png", directory, i / 2);
            assert(length > 0 && (size_t)length < sizeof capture);
            assert(app_save_screenshot(capture) == 0);
        }
    }
    assert(max_step < .04f);
    printf("Ship rendered review PASS: 12 seconds, 30 fps capture; largest fixed-step roll %.3f degrees\n", max_step * 180 / CF_PI);
}

int arena_play_run(const char *program)
{
    CF_Result result = cf_make_app(
        "Arena 3D play regression",
        0,
        0,
        0,
        1920,
        1080,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT | CF_APP_OPTIONS_RESIZABLE_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "Arena play app: %s\n", result.details);
        return 1;
    }
    arena_app_start();
    bool input_only = getenv("CHIBI_ARENA_INPUT_ONLY") != NULL;
    if (!input_only) {
        assert(test_arena_input() == 0);
    }
    fflush(stdout);
    arena_fixture f = fixture_new();
    for (int i = 0; i < 30; i++) {
        frame(&f);
    }
    key_event(SDL_SCANCODE_SPACE, true);
    key_event(SDL_SCANCODE_SPACE, false);
    key_event(SDL_SCANCODE_DELETE, true);
    key_event(SDL_SCANCODE_DELETE, false);
    frame(&f);
    assert(app_input_controls() & (UINT64_C(1) << GAME_CONTROL_SPACE));
    assert(app_input_pressed(APP_INPUT_DELETE));
    frame(&f);
    assert(!(app_input_controls() & (UINT64_C(1) << GAME_CONTROL_SPACE)));
    assert(!app_input_pressed(APP_INPUT_DELETE));
    puts("gameplay quick tap PASS: press and release in one frame survives for exactly one sample");
    const char *motion_frames = getenv("CHIBI_ARENA_MOTION_FRAMES");
    if (motion_frames && *motion_frames) {
        ship_motion_review(&f, motion_frames);
        goto cleanup;
    }
    editor_mouse_play(&f);
    if (input_only) {
        goto cleanup;
    }
    assert(f.scene->perspective && engine_viewport_perspective(f.viewport));
    assert_framed(&f);
    drag_player(&f);
    persistence(&f);
    preview_projection(&f);
    planar_orientation(&f);
    cf_app_set_size(1280, 960);
    for (int i = 0; i < 20; i++) {
        frame(&f);
    }
    assert_framed(&f);
    drag_player(&f);
    cf_app_set_size(1920, 1080);
    for (int i = 0; i < 20; i++) {
        frame(&f);
    }
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    frame(&f); /* Play establishes a fresh wall-clock baseline. */
    current_mouse_aim(&f);
    for (int i = 0; i < 150; i++) {
        float angle = i * 0.018f;
        assert(app_scene_authoring_aim(f.scene, (CF_V2){ cosf(angle), sinf(angle) }, true) == 0);
        frame(&f);
    }
    assert(projectiles(&f) > 0);
    const char *capture = getenv("CHIBI_ARENA_CAPTURE");
    if (capture && *capture) {
        assert(app_save_screenshot(capture) == 0);
    }
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    assert(projectiles(&f) == 0);
    assert(f.scene->perspective);
    standalone_play(&f);
    /* ---------- arena2d end to end (wave 3, workstream X) ---------- */
    standalone_ui_keys(&f);
cleanup:
    fixture_free(&f);
    arena_app_stop();
    if (!input_only) {
        puts("arena play PASS: scene save/reload, independent preview projection, pointer drag/Undo, resize, current-frame mouse aim, firing and Stop, standalone game");
    }
    return 0;
}

static int compare_milliseconds(const void *a, const void *b)
{
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

static ecs_entity scene_scale_spawner(arena_fixture *f)
{
    behavior_asset *asset = behavior_asset_new("Scene scale spawn");
    assert(!behavior_asset_set_owner_type(asset, "ecs.entity"));
    pin_value zero = { .type = PIN_HANDLE, .type_name = "ecs.entity" };
    behavior_variable *created = behavior_asset_add_variable(
        asset,
        "Created",
        PIN_HANDLE,
        "ecs.entity",
        PIN_CONTAINER_SINGLE,
        &zero,
        "Test"
    );
    graph *g = asset->graph;
    node *event = graph_add_node(g, registry_node_by_name("Event"));
    assert(!graph_set_event_name(g, event, "Fire"));
    const char *names[] = { "X", "Y" };
    for (int i = 0; i < 2; i++) {
        pin_spec parameter = { .name = names[i],
                               .type = PIN_F32,
                               .type_name = "float",
                               .kind = PIN_DATA };
        assert(graph_add_pin(g, event, 1, &parameter) == i + 1);
    }
    node *spawn = graph_add_node(g, registry_node_by_name("Spawn Prototype"));
    pin_value prototype = { .type = PIN_NAME, .str = "ArenaEnemy" };
    assert(!graph_set_pin_default(g, spawn, 0, 1, &prototype));
    node *make = graph_add_node(g, registry_node_by_name("Make_GameTransform"));
    node *initialize = graph_add_node(g, registry_node_by_name("SetEntity_GameTransform"));
    node *remember = behavior_asset_add_set_node(asset, created->id);
    assert(!graph_connect_exec(g, event, 0, spawn, 0, 0));
    assert(!graph_connect_exec(g, spawn, 0, initialize, 0, 0));
    assert(!graph_connect_exec(g, initialize, 0, remember, 0, 0));
    assert(!graph_connect_data(g, event, 1, make, 0));
    assert(!graph_connect_data(g, event, 2, make, 1));
    assert(!graph_connect_data(g, spawn, 1, initialize, 1));
    assert(!graph_connect_data(g, make, 0, initialize, 2));
    assert(!graph_connect_data(g, spawn, 1, remember, 1));
    assert(!game_preview_add_definition(f->live, game_preview_definition_new("Scene scale spawn", asset)));
    ecs_entity owner = ecs_world_create_entity(f->world);
    assert(!game_behavior_attach(f->live->runtime->behaviors, owner, "Scene scale spawn"));
    return owner;
}

static ecs_entity *scene_scale_enemies(arena_fixture *f, int count, bool combat)
{
    ecs_entity player = f->scene->actors[player_index(f)].entity;
    GameTransform center = *GAME_GameTransform_get_const(f->world, player);
    int authored = 0;
    ecs_query query;
    ecs_component_id member = GAME_COMPONENT_GameEncounterMember.id;
    assert(ecs_query_begin(f->world, &query, &member, 1) == ECS_OK);
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        authored += GAME_GameEncounterMember_get_const(f->world, entity)->role ==
                    GAME_ENCOUNTER_ENEMY;
    }
    ecs_query_end(&query);
    assert(authored <= count);
    printf("scene_scale_population authored=%d spawned=%d total=%d\n", authored, count - authored, count);
    /* Keep the measurement window alive under real contact damage. The
     * authored wave and every enemy attachment still execute normally. */
    *GAME_GameHealth_get(f->world, player) = (GameHealth){ 1000000000, 1000000000 };
    for (int i = 0; i < acount(f->scene->actors); i++) {
        GameEncounter
            *encounter = GAME_GameEncounter_get(f->world, f->scene->actors[i].entity);
        if (encounter) {
            encounter->total = count;
            encounter->spawned = count;
            encounter->alive = count;
        }
    }
    char *json = text_file_read("content/prototypes/ArenaEnemy.kdl"), error[256];
    assert(json);
    game_prototype *prototype = game_prototype_load(json, &f->scene->registry, error, sizeof error);
    free(json);
    assert(prototype);
    ecs_entity *enemies = NULL;
    ecs_entity spawner = combat ? scene_scale_spawner(f) : (ecs_entity){ 0 };
    int side = (int)ceil(sqrt(count));
    for (int i = 0; i < count - authored; i++) {
        ecs_entity entity;
        GameTransform position = {
            center.x + (i % side - (side - 1) * .5f) * 22,
            center.y + (i / side - (side - 1) * .5f) * 22,
        };
        if (combat) {
            /* Use compiled host spawning so restart owns this population and
             * initialization still precedes each enemy's Begin handlers. */
            pin_value inputs[] = { { .type = PIN_F32, .f = position.x },
                                   { .type = PIN_F32, .f = position.y } };
            assert(
                game_behavior_dispatch(f->live->runtime->behaviors, spawner, "Fire", (flow_inputs){ inputs, 2 }, NULL) ==
                1
            );
            assert(!game_entities_flush_initializers(f->live->runtime->entities, f->time));
            behavior_instance *instance = game_behavior_instance(f->live->runtime->behaviors, spawner, "Scene scale spawn");
            pin_value value;
            assert(!behavior_instance_get_variable(
                instance,
                instance->asset->variables[0].id,
                &value
            ));
            entity = ecs_entity_from_u64(value.u64);
            assert(ecs_entity_is_alive(f->world, entity));
        } else {
            assert(
                !game_prototype_spawn_components(prototype, f->world, &entity, error, sizeof error)
            );
            *GAME_GameTransform_get(f->world, entity) = position;
            assert(!game_behavior_set_attachments(
                f->live->runtime->behaviors,
                entity,
                (const char *const *)prototype->graphs,
                acount(prototype->graphs)
            ));
        }
        apush(enemies, entity);
    }
    if (combat) {
        assert(!game_behavior_destroy_entity(f->live->runtime->behaviors, spawner));
        assert(!game_preview_remove_definition(f->live, "Scene scale spawn"));
    }
    game_prototype_free(prototype);
    return enemies;
}

static void scene_scale_aim(arena_fixture *f)
{
    GameTransform player, target;
    assert(review_target(f, &player, &target));
    render_arena_camera view = camera(f);
    CF_V3 aim = render_arena_project(&view, (CF_V3){ target.x, 0, target.y });
    mouse_move_event(screen_ndc(f, (CF_V2){ aim.x, aim.y }));
}

static void scene_scale_verify_encounter(arena_fixture *f)
{
    for (int i = 0; i < acount(f->scene->actors); i++) {
        ecs_entity entity = f->scene->actors[i].entity;
        if (!GAME_GameEncounter_has(f->world, entity)) {
            continue;
        }
        behavior_instance *instance = game_behavior_instance(f->live->runtime->behaviors, entity, "ArenaWaveBehavior");
        const behavior_instance_failure *failure = behavior_instance_last_failure(instance);
        if (failure) {
            fprintf(
                stderr,
                "Scene scale retained failure: %s node=%llu pin=%d\n",
                failure->message,
                (unsigned long long)failure->node_id,
                failure->pin
            );
        }
        assert(!failure);
        flow_run *run = behavior_instance_dispatch(instance, "RefreshEncounter", (flow_inputs){ 0 }, NULL);
        assert(run);
        if (flow_run_status(run) == FLOW_FAILED) {
            const flow_error *error = flow_run_error(run);
            fprintf(
                stderr,
                "Scene scale encounter failed: %s, code=%d node=%llu pin=%d "
                "steps=%lu\n",
                error->message,
                error->code,
                (unsigned long long)error->node->id,
                error->pin,
                error->steps
            );
        }
        assert(flow_run_status(run) == FLOW_COMPLETED);
        assert(!behavior_instance_release_activation(instance, run));
        return;
    }
    assert(!"Missing encounter behavior");
}

static void scene_scale_disable_sensor_sweeps(arena_fixture *f)
{
    int changed = 0;
    for (int i = 0; i < acount(f->scene->prototypes); i++) {
        game_prototype *prototype = f->scene->prototypes[i].prototype;
        GameBody *body = NULL;
        const GameCollider *collider = NULL;
        for (int j = 0; j < acount(prototype->components); j++) {
            game_prototype_component *component = &prototype->components[j];
            if (component->desc->id == GAME_COMPONENT_GameBody.id) {
                body = component->data;
            }
            if (component->desc->id == GAME_COMPONENT_GameCollider.id) {
                collider = component->data;
            }
        }
        if (body && collider && collider->sensor && body->continuous) {
            body->continuous = false;
            assert(game_entities_register(f->live->runtime->entities, prototype) == 0);
            changed++;
        }
    }
    assert(changed > 0);
    printf("scene_scale_sensor_sweeps disabled_prototypes=%d\n", changed);
}

int arena_scene_scale_run(const char *program, int count, int frames, bool profile)
{
    assert(count > 0 && count <= 20000 && frames >= 30 && frames <= 1800);
    CF_Result result = cf_make_app(
        "Arena Scene scale",
        0,
        0,
        0,
        1280,
        720,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "Arena scale app: %s\n", result.details);
        return 1;
    }
    cf_app_set_present_mode(CF_PRESENT_MODE_IMMEDIATE);
    arena_app_start();
    behavior_backend previous = behavior_debug_backend(BEHAVIOR_SLJIT);
    srand(12345);
    arena_fixture f = fixture_new();
    app_game_presentation *game = app_game_presentation_new(f.scene, f.viewport, false);
    assert(game);
    game_behavior_set_profiling(f.live->runtime->behaviors, profile);
    game_behavior_set_parallel_updates(f.live->runtime->behaviors, !getenv("CBLUEPRINT_BENCH_SERIAL_BEHAVIORS"));
    app_scene_renderer_set_parallel(f.scene->renderer, !getenv("CBLUEPRINT_BENCH_SERIAL_RENDER"));
    const char *physics_json = getenv("CBLUEPRINT_BENCH_PHYSICS_SETTINGS");
    if (physics_json) {
        yyjson_doc *doc = yyjson_read(physics_json, strlen(physics_json), 0);
        game_physics_settings settings;
        av_doc *values = av_doc_new();
        av_value *value = doc ? av_from_json(values, yyjson_doc_get_root(doc)) : NULL;
        assert(value && game_physics_settings_read(value, &settings));
        av_doc_free(values);
        assert(game_physics_configure(f.live->runtime->physics, settings) == 0);
        yyjson_doc_free(doc);
    }
    if (getenv("CBLUEPRINT_BENCH_NO_SENSOR_SWEEPS")) {
        scene_scale_disable_sensor_sweeps(&f);
    }
    game_physics_set_profiling(f.live->runtime->physics, profile || getenv("CBLUEPRINT_BENCH_PHYSICS_PROFILE"));
    test_physics_executor executor = test_physics_executor_open();
    if (executor.library) {
        game_physics_set_tasks(f.live->runtime->physics, &executor.tasks);
    } else if (getenv("CBLUEPRINT_BENCH_SERIAL_PHYSICS")) {
        game_physics_set_tasks(f.live->runtime->physics, NULL);
    }
    uint64_t eval = eval_oracle_calls(), flow = flow_oracle_calls();
    bool combat = getenv("CBLUEPRINT_BENCH_COMBAT") != NULL, cold = true;
    ecs_entity *enemies = scene_scale_enemies(&f, count, combat);
    ecs_entity player = f.scene->actors[player_index(&f)].entity;
    uint64_t trace = UINT64_C(14695981039346656037);
    int peak_backlog = 0, backlog_frames = 0, peak_shots = 0, firing_frames = 0;
    double peak_physics = 0, peak_frame = 0;
    enum
    {
        WALL,
        BEHAVIOR,
        PHYSICS,
        SYSTEMS,
        RENDER_HOST,
        PRESENT,
        METRICS
    };
    const char *names[] = { "wall",    "behavior",    "physics",
                            "systems", "render_host", "present" };
    double *samples[METRICS] = { 0 };
    game_physics_profile phases = { 0 };
    int peak_draws = 0;
    CF_DrawStats3d draw_totals = { 0 };
    uint64_t prepared_ticks = 0, system_ticks = 0, parallel_ticks = 0,
             render_tasks = 0;
    int behavior_workers = 0, render_workers = 0;
    cf_draw3d_stats();
    for (int i = -30; i < frames; i++) {
        if (combat && i >= 0) {
            /* Aim selection is fixture work, outside the frame timer. Held
             * input still enters through the normal presentation controls. */
            scene_scale_aim(&f);
            if (!i) {
                mouse_button_event(true);
            }
        }
        double start = cf_get_ticks() * 1000.0 / cf_get_tick_frequency();
        cf_app_update(NULL);
        app_input_frame();
        f.time += 1.0 / 60.0;
        double begin = cf_get_ticks() * 1000.0 / cf_get_tick_frequency();
        assert(!app_game_presentation_frame(game, f.time));
        double drawn = cf_get_ticks() * 1000.0 / cf_get_tick_frequency();
        cf_app_draw_onto_screen(true);
        double end = cf_get_ticks() * 1000.0 / cf_get_tick_frequency();
        CF_DrawStats3d draws = cf_draw3d_stats();
        if (draws.instances > peak_draws) {
            peak_draws = draws.instances;
        }
        const engine_frame_timing *timing = engine_last_timing(f.runtime);
        if (cold && timing->steps) {
            printf("scene_scale_cold physics_ms=%.6f frame_ms=%.6f\n", timing->physics_ms, end - start);
            cold = false;
        }
        if (i < 0) {
            continue;
        }
        draw_totals.instances += draws.instances;
        draw_totals.commands += draws.commands;
        for (int j = 0; j < CF_DRAW_SPLIT_3D_COUNT; j++) {
            draw_totals.splits[j] += draws.splits[j];
        }
        game_behavior_update_stats ticks = game_behavior_last_update(f.live->runtime->behaviors);
        prepared_ticks += ticks.prepared;
        system_ticks += ticks.system_loop;
        parallel_ticks += ticks.parallel;
        if (ticks.workers > behavior_workers) {
            behavior_workers = ticks.workers;
        }
        render_arena_work work = app_scene_renderer_last_work(f.scene->renderer);
        render_tasks += work.tasks;
        if (work.workers > render_workers) {
            render_workers = work.workers;
        }
        GameEncounter encounter = review_encounter(f.world);
        if (timing->steps != 1 || encounter.alive > count) {
            fprintf(
                stderr,
                "scene_scale_invalid frame=%d steps=%d alive=%d requested=%d\n",
                i,
                timing->steps,
                encounter.alive,
                count
            );
        }
        assert(timing->steps == 1 && encounter.alive <= count);
        assert(combat || encounter.alive == count);
        peak_physics = fmax(peak_physics, timing->physics_ms);
        peak_frame = fmax(peak_frame, end - start);
        if (timing->event_backlog > peak_backlog) {
            peak_backlog = timing->event_backlog;
        }
        backlog_frames += timing->event_backlog > 0;
        if (combat) {
            bool firing = GAME_GameAim_get_const(f.world, player)->firing;
            if (firing && !firing_frames) {
                printf(
                    "scene_scale_first_fire physics_ms=%.6f behavior_ms=%.6f "
                    "frame_ms=%.6f\n",
                    timing->physics_ms,
                    timing->behavior_ms,
                    end - start
                );
            }
            firing_frames += firing;
            int shots = projectiles(&f);
            if (shots > peak_shots) {
                peak_shots = shots;
            }
            int state[] = { encounter.alive,
                            encounter.status,
                            shots,
                            GAME_GameHealth_get_const(f.world, player)->current,
                            GAME_GameExperience_get_const(f.world, player)->current,
                            timing->event_backlog };
            for (int j = 0; j < (int)(sizeof state / sizeof *state); j++) {
                trace = (trace ^ (uint64_t)state[j]) * UINT64_C(1099511628211);
            }
        }
        double values[] = { end - start,
                            timing->behavior_ms,
                            timing->physics_ms,
                            timing->systems_ms,
                            fmax(
                                0,
                                drawn - begin - timing->behavior_ms -
                                    timing->physics_ms - timing->systems_ms
                            ),
                            end - drawn };
        for (int j = 0; j < METRICS; j++) {
            apush(samples[j], values[j]);
        }
        const game_physics_profile *p = game_physics_last_profile(f.live->runtime->physics);
        phases.synchronize_ms += p->synchronize_ms;
        phases.solve_ms += p->solve_ms;
        phases.sweeps_ms += p->sweeps_ms;
        phases.pairs_ms += p->pairs_ms;
        phases.writeback_ms += p->writeback_ms;
        phases.dispatch_ms += p->dispatch_ms;
        phases.bodies = p->bodies;
        phases.pairs = p->pairs;
    }
    scene_scale_verify_encounter(&f);
    double checksum = 0;
    for (int i = 0; i < acount(enemies); i++) {
        if (combat && !ecs_entity_is_alive(f.world, enemies[i])) {
            continue;
        }
        const GameTransform *position = GAME_GameTransform_get_const(f.world, enemies[i]);
        assert(position && isfinite(position->x) && isfinite(position->y));
        checksum += position->x + position->y;
        int attached = game_behavior_attachment_count_for_entity(f.live->runtime->behaviors, enemies[i]);
        assert(attached == 5);
        for (int j = 0; j < attached; j++) {
            game_behavior_attachment_info info;
            assert(!game_behavior_attachment_info_at(f.live->runtime->behaviors, enemies[i], j, &info));
            assert(behavior_instance_backend(info.instance) == BEHAVIOR_SLJIT);
            assert(!behavior_instance_last_failure(info.instance));
        }
    }
    assert(peak_draws > 0 && eval == eval_oracle_calls() && flow == flow_oracle_calls());
    printf(
        "scene_scale_parallel ticks=%llu behavior_workers=%d render_tasks=%llu "
        "render_workers=%d\n",
        (unsigned long long)parallel_ticks,
        behavior_workers,
        (unsigned long long)render_tasks,
        render_workers
    );
    if (getenv("CBLUEPRINT_BENCH_REQUIRE_PARALLEL")) {
        assert(
            parallel_ticks > 0 && behavior_workers > 1 && render_tasks > 0 &&
            render_workers > 1
        );
    }
    printf(
        "scene_scale_executor workers=%d external=%d\n",
        game_physics_workers(f.live->runtime->physics),
        executor.library != NULL
    );
    printf(
        "scene_scale_spikes physics_max_ms=%.6f frame_max_ms=%.6f "
        "backlog_max=%d backlog_frames=%d backlog_final=%d\n",
        peak_physics,
        peak_frame,
        peak_backlog,
        backlog_frames,
        engine_last_timing(f.runtime)->event_backlog
    );
    for (int j = 0; j < METRICS; j++) {
        qsort(samples[j], (size_t)frames, sizeof(double), compare_milliseconds);
        printf(
            "scene_scale_phase name=%s p50_ms=%.6f p95_ms=%.6f\n",
            names[j],
            samples[j][(frames - 1) / 2],
            samples[j][(frames - 1) * 95 / 100]
        );
        afree(samples[j]);
    }
    printf(
        "scene_scale_physics sync_ms=%.6f solve_ms=%.6f sweeps_ms=%.6f "
        "pairs_ms=%.6f "
        "writeback_ms=%.6f dispatch_ms=%.6f bodies=%d pair_candidates=%d\n",
        phases.synchronize_ms / frames,
        phases.solve_ms / frames,
        phases.sweeps_ms / frames,
        phases.pairs_ms / frames,
        phases.writeback_ms / frames,
        phases.dispatch_ms / frames,
        phases.bodies,
        phases.pairs
    );
    printf(
        "scene_scale_render instances=%.3f commands=%.3f\n",
        (double)draw_totals.instances / frames,
        (double)draw_totals.commands / frames
    );
    for (int i = 0; i < CF_DRAW_SPLIT_3D_COUNT; i++) {
        if (draw_totals.splits[i]) {
            printf(
                "scene_scale_render_split reason=%s per_frame=%.3f\n",
                cf_draw_split_3d_to_string(i),
                (double)draw_totals.splits[i] / frames
            );
        }
    }
    printf(
        "scene_scale_ticks prepared=%llu system=%llu\n",
        (unsigned long long)prepared_ticks,
        (unsigned long long)system_ticks
    );
    printf(
        "scene_scale backend=SLJIT entities=%d frames=%d profile=%d draws=%d "
        "checksum=%.9f oracle_calls=0\n",
        count,
        frames,
        profile,
        peak_draws,
        checksum
    );
    if (combat) {
        GameEncounter encounter = review_encounter(f.world);
        int experience = GAME_GameExperience_get_const(f.world, player)->current;
        int health = GAME_GameHealth_get_const(f.world, player)->current;
        printf(
            "scene_scale_combat alive=%d kills=%d experience=%d health=%d "
            "peak_shots=%d firing_frames=%d trace=%llu\n",
            encounter.alive,
            count - encounter.alive,
            experience,
            health,
            peak_shots,
            firing_frames,
            (unsigned long long)trace
        );
        fflush(stdout);
        assert(encounter.alive < count && experience > 0 && firing_frames > 0);
        mouse_button_event(false);
        double start = cf_get_ticks() * 1000.0 / cf_get_tick_frequency();
        assert(!app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_RESTART));
        double restart_ms = cf_get_ticks() * 1000.0 / cf_get_tick_frequency() - start;
        for (int i = 0; i < acount(enemies); i++) {
            assert(!ecs_entity_is_alive(f.world, enemies[i]));
        }
        player = f.scene->actors[player_index(&f)].entity;
        assert(!GAME_GameExperience_get_const(f.world, player)->current);
        assert(!projectiles(&f) && !game_entities_pending_events(f.live->runtime->entities));
        for (int i = 0; i < 3; i++) {
            assert(!standalone_frame(&f, game));
        }
        assert(engine_last_timing(f.runtime)->steps == 1);
        assert(review_encounter(f.world).total == 40);
        assert(
            game_physics_workers(f.live->runtime->physics) ==
            (executor.library ? executor.tasks.workers : 1)
        );
        assert(eval == eval_oracle_calls() && flow == flow_oracle_calls());
        printf(
            "scene_scale_restart reset_ms=%.6f workers=%d oracle_calls=0\n",
            restart_ms,
            game_physics_workers(f.live->runtime->physics)
        );
    }
    fflush(stdout);
    afree(enemies);
    app_game_presentation_free(game);
    fixture_free(&f);
    test_physics_executor_close(&executor);
    behavior_debug_backend(previous);
    arena_app_stop();
    return 0;
}

int compiled_arena_run(const char *program)
{
    CF_Result result = cf_make_app(
        "Compiled Arena smoke",
        0,
        0,
        0,
        800,
        600,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        return 1;
    }
    arena_app_start();
    behavior_backend modes[] = { BEHAVIOR_SLJIT, BEHAVIOR_BYTECODE };
    for (int mode = 0; mode < 2; mode++) {
        behavior_backend previous = behavior_debug_backend(modes[mode]);
        srand(12345);
        arena_fixture f = fixture_new();
        assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
        game_behavior_set_profiling(f.live->runtime->behaviors, !getenv("CBLUEPRINT_BENCH_NO_PROFILE"));
        bool batch = !getenv("CBLUEPRINT_BENCH_NO_BATCH");
        game_behavior_set_batch_enabled(f.live->runtime->behaviors, batch);
        uint64_t batches = behavior_batch_dispatches(), prepared = 0, native = 0;
        uint64_t eval_before = eval_oracle_calls(), flow_before = flow_oracle_calls();
        double behavior_ms[150], simulation_ms[150];
        game_behavior_profile_frame profile = { 0 };
        for (int i = 0; i < 180; i++) {
            assert(app_scene_authoring_aim(f.scene, (CF_V2){ 1, 0 }, true) == 0);
            f.time += 1.0 / 60.0;
            assert(app_scene_authoring_frame(f.scene, f.time) == FLOW_OK);
            game_behavior_update_stats updates = game_behavior_last_update(
                f.live->runtime->behaviors
            );
            prepared += updates.prepared;
            native += updates.native_loop;
            if (i >= 30) {
                const engine_frame_timing *timing = app_scene_authoring_runtime_timing(
                    f.scene
                );
                review_behavior_totals(&profile, game_behavior_profile(f.live->runtime->behaviors));
                behavior_ms[i - 30] = timing->behavior_ms;
                simulation_ms[i - 30] = timing->behavior_ms +
                                        timing->physics_ms + timing->systems_ms;
            }
        }
        qsort(behavior_ms, 150, sizeof(double), compare_milliseconds);
        batches = behavior_batch_dispatches() - batches;
        assert(batch ? prepared > 0 : !batches && !prepared);
        assert(native == 0 || modes[mode] == BEHAVIOR_SLJIT);
        if (batch && modes[mode] == BEHAVIOR_SLJIT &&
            getenv("CBLUEPRINT_BENCH_NO_PROFILE")) {
            assert(native > 0);
        }
        printf(
            "%s Arena batch_dispatches=%llu prepared_updates=%llu "
            "native_loop=%llu\n",
            behavior_backend_name(modes[mode]),
            (unsigned long long)batches,
            (unsigned long long)prepared,
            (unsigned long long)native
        );
        qsort(simulation_ms, 150, sizeof(double), compare_milliseconds);
        printf(
            "%s Arena behavior_p50_ms=%.6f behavior_p95_ms=%.6f "
            "simulation_p50_ms=%.6f simulation_p95_ms=%.6f\n",
            behavior_backend_name(modes[mode]),
            behavior_ms[74],
            behavior_ms[142],
            simulation_ms[74],
            simulation_ms[142]
        );
        qsort(profile.rows, (size_t)profile.count, sizeof *profile.rows, compare_behavior_time);
        for (int j = 0; j < profile.count; j++) {
            printf(
                "profile backend=%s behavior=%s event=%s exec_ms=%.6f "
                "setup_ms=%.6f dispatches=%llu starts=%llu resumes=%llu\n",
                behavior_backend_name(modes[mode]),
                profile.rows[j].behavior,
                profile.rows[j].event,
                profile.rows[j].cpu_ms / 150,
                profile.rows[j].setup_ms / 150,
                (unsigned long long)profile.rows[j].dispatches,
                (unsigned long long)profile.rows[j].starts,
                (unsigned long long)profile.rows[j].resumes
            );
        }
        assert(projectiles(&f) > 0);
        ecs_query query;
        assert(ecs_query_begin(f.world, &query, NULL, 0) == ECS_OK);
        ecs_entity entity;
        int instances = 0, entries = 0, whole = 0;
        const behavior_asset **seen = NULL;
        while (ecs_query_next(&query, &entity) > 0) {
            int count = game_behavior_attachment_count_for_entity(f.live->runtime->behaviors, entity);
            for (int i = 0; i < count; i++) {
                game_behavior_attachment_info info;
                assert(
                    game_behavior_attachment_info_at(f.live->runtime->behaviors, entity, i, &info) == 0
                );
                assert(behavior_instance_backend(info.instance) == modes[mode]);
                instances++;
                const behavior_asset *asset = info.instance->asset;
                bool known = false;
                for (int j = 0; j < acount(seen); j++) {
                    known |= seen[j] == asset;
                }
                if (known) {
                    continue;
                }
                apush(seen, asset);
                for (int j = 0; j < acount(asset->graph->nodes); j++) {
                    node *entry = asset->graph->nodes[j];
                    if (entry->desc != &NODE_Event) {
                        continue;
                    }
                    entries++;
                    whole += behavior_program_function(
                                 behavior_program_graph(behavior_program_cached(asset), asset->graph),
                                 entry
                             ) != NULL;
                    if (mode == 0) {
                        const behavior_function_diagnostic *d = behavior_program_function_diagnostic(
                            behavior_program_graph(behavior_program_cached(asset), asset->graph),
                            entry
                        );
                        if (d) {
                            printf(
                                "whole_exclusion asset=%s event=%s graph=%llu "
                                "node=%llu type=%s pin=%d output=%d "
                                "reason=%s\n",
                                asset->name,
                                entry->reference_name,
                                (unsigned long long)d->graph->graph_id,
                                (unsigned long long)d->node->id,
                                d->node->desc->name,
                                d->pin,
                                d->output,
                                d->reason
                            );
                        }
                    }
                }
            }
        }
        ecs_query_end(&query);
        printf(
            "%s Arena whole_function_entries=%d/%d unique_assets=%d\n",
            behavior_backend_name(modes[mode]),
            whole,
            entries,
            acount(seen)
        );
        afree(seen);
        assert(instances > 0);
        assert(eval_oracle_calls() == eval_before && flow_oracle_calls() == flow_before);
        assert(
            app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_RESTART) == 0
        );
        assert(app_scene_authoring_frame(f.scene, f.time + 1.0 / 60.0) == FLOW_OK);
        assert(eval_oracle_calls() == eval_before && flow_oracle_calls() == flow_before);
        printf(
            "%s: Arena3D firing, simulation and restart; %d instances; zero "
            "oracle calls\n",
            behavior_backend_name(modes[mode]),
            instances
        );
        fixture_free(&f);
        behavior_debug_backend(previous);
    }
    arena_app_stop();
    return 0;
}
