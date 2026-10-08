#include "authoring/ui_loader.h"
#include "authoring/ui_events_adapter.h"
#include "execution/status.h"
#include "editor/workspace/viewport.h"
#include "editor/workspace/game_presentation.h"

#include "assets/ui_document.h"
#include "editor/workspace/gameplay_input.h"
#include "editor/workspace/scenario_playback.h"
#include "editor/workspace/scene_internal.h"
#include "platform/cute.h"
#include "render/audio.h"
#include "render/ui.h"
#include "runtime/camera.h"
#include "runtime/component_catalog.h"
#include "support/xalloc.h"

#include <assert.h>
#include <float.h>
#include <imgui/dcimgui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char editor_ui_font[];
extern const size_t editor_ui_font_size;

struct app_game_presentation
{
    app_scene_authoring *scene;
    engine_viewport *viewport;
    bool interactive;
    bool render_failed;
    render_audio *audio;
    /* Explicit presentation time; it advances while playing and freezes paused. */
    double showcase_time;
    double showcase_last_time;
    bool showcase_time_started;
    char ui_error[256]; /* the last UI asset error reported */
    bool text_input; /* SDL delivers typed text for a focused game text field */
    char lighting_warning[512]; /* the last lighting adjustment reported */
};

static render_arena_camera game_camera(const app_game_presentation *game)
{
    render_arena_camera camera;
    engine_viewport_get_arena_camera(game->viewport, &camera);
    return camera;
}

static void render_game(void *user)
{
    app_game_presentation *game = user;
    app_scene_authoring *scene = game->scene;
    if (scene->perspective) {
        render_arena_camera camera = game_camera(game);
        game->render_failed = !app_scene_renderer_draw3d(
            scene->renderer,
            scene->world,
            scene->live->runtime->particles,
            &camera,
            game->viewport,
            scene->runtime,
            (render_skip){ 0 }
        );
        return;
    }
    app_scene_authoring_render(scene);
}

/* Game UI text uses the embedded Inter face. ImGui's atlas owns a copy. */
static void prepare_ui()
{
    ImGuiIO *io = ImGui_GetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    for (int i = 0; i < io->Fonts->Fonts.Size; i++) {
        if (!strcmp(ImFont_GetDebugName(io->Fonts->Fonts.Data[i]), "Chibi UI")) {
            return;
        }
    }
    ImFontConfig config = { 0 };
    config.FontDataOwnedByAtlas = true;
    strcpy(config.Name, "Chibi UI");
    config.GlyphMaxAdvanceX = FLT_MAX;
    config.RasterizerMultiply = 1.0f;
    config.RasterizerDensity = 1.0f;
    config.ExtraSizeScale = 1.0f;
    void *font = ImGui_MemAlloc(editor_ui_font_size);
    memcpy(font, editor_ui_font, editor_ui_font_size);
    io->FontDefault = ImFontAtlas_AddFontFromMemoryTTF(
        io->Fonts,
        font,
        (int)editor_ui_font_size,
        16.0f,
        &config,
        NULL
    );
    ImGui_GetStyle()->FontSizeBase = 16.0f;
}

/* ImGui lays out in logical pixels over the whole canvas it is drawn into. */
static ImVec2 display_size()
{
    ImGuiIO *io = ImGui_GetIO();
    ImVec2 scale = io->DisplayFramebufferScale;
    ImVec2 size = { cf_app_get_canvas_width() / (scale.x > 0 ? scale.x : 1.0f),
                    cf_app_get_canvas_height() / (scale.y > 0 ? scale.y : 1.0f) };
    io->DisplaySize = size;
    ImGuiViewport *view = ImGui_GetMainViewport();
    view->Pos = view->WorkPos = (ImVec2){ 0 };
    view->Size = view->WorkSize = size;
    return size;
}

app_game_presentation *app_game_presentation_new(
    app_scene_authoring *scene,
    engine_viewport *viewport,
    bool interactive
)
{
    if (interactive) {
        const char *directory = cf_fs_get_user_directory("chibi", "game");
        char save_path[1024];
        int length = snprintf(save_path, sizeof save_path, "%sprofiles.json", directory ? directory : "");
        if (!directory || length < 0 || length >= (int)sizeof save_path ||
            !game_save_open(scene->live->runtime->save, save_path)) {
            fprintf(
                stderr,
                "Could not open game save storage: %s\n",
                game_save_error(scene->live->runtime->save)
            );
            return NULL;
        }
    }
    if (app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) !=
        0) {
        return NULL;
    }
    app_game_presentation *game = xcalloc(1, sizeof *game);
    *game = (app_game_presentation){ .scene = scene,
                                     .viewport = viewport,
                                     .interactive = interactive };
    game->audio = render_audio_new();
    prepare_ui();
    scene->selected = -1;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene->actors[i].marked = false;
    }
    scene->render_viewport = viewport;
    engine_viewport_set_perspective(viewport, scene->perspective);
    engine_viewport_request_resize(viewport, cf_app_get_canvas_width(), cf_app_get_canvas_height());
    fit_scene_view(viewport, scene->world, NULL, 0);
    engine_viewport_set_render_callback(viewport, render_game, game);
    ui_world ui = { .ecs = scene->world,
                    .page = app_scene_authoring_page(scene),
                    .model = engine_ui(scene->runtime),
                    .host = authoring_ui_events(scene->live),
                    .save = scene->live->runtime->save };
    authoring_viewport_set_ui(viewport, &ui, scene->content_root);
    return game;
}

void app_game_presentation_free(app_game_presentation *game)
{
    if (game) {
        if (game->text_input) {
            cf_input_disable_ime();
        }
        engine_viewport_clear_arena_camera(game->viewport);
        engine_viewport_set_render_callback(game->viewport, NULL, NULL);
        render_audio_free(game->audio);
        free(game);
    }
}

static bool mouse_target(const app_game_presentation *game, CF_V2 *target)
{
    ImVec2 size = ImGui_GetIO()->DisplaySize, mouse = ImGui_GetIO()->MousePos;
    if (size.x <= 0 || size.y <= 0 || mouse.x < 0 || mouse.y < 0 ||
        mouse.x >= size.x || mouse.y >= size.y) {
        return false;
    }
    CF_V2 ndc = { mouse.x * 2 / size.x - 1, 1 - mouse.y * 2 / size.y };
    if (game->scene->perspective) {
        render_arena_camera camera = game_camera(game);
        return render_arena_ground(&camera, ndc, target);
    }
    engine_viewport_camera frame;
    engine_viewport_get_camera(game->viewport, &frame);
    *target = (CF_V2){ frame.center_x + ndc.x * frame.span_width * 0.5f,
                       -frame.center_y - ndc.y * frame.span_height * 0.5f };
    return true;
}

static int command(app_game_presentation *game, app_runtime_command command)
{
    int result = app_scene_authoring_runtime_command(game->scene, APP_RUNTIME_SCENE, command);
    if (result == 0 && command == APP_RUNTIME_RESTART) {
        game->showcase_time = 0;
    }
    return result;
}

static int encounter_status(ecs_world *world)
{
    ecs_component_id component = GAME_COMPONENT_GameEncounter.id;
    if (!ecs_world_component(world, component)) {
        return GAME_ENCOUNTER_ACTIVE;
    }
    ecs_query query;
    int result = ecs_query_begin(world, &query, &component, 1);
    assert(result == ECS_OK);
    (void)result;
    ecs_entity entity;
    int status = GAME_ENCOUNTER_ACTIVE;
    while (ecs_query_next(&query, &entity)) {
        if (!ecs_entity_is_retiring(world, entity)) {
            status = GAME_GameEncounter_get_const(world, entity)->status;
            break;
        }
    }
    ecs_query_end(&query);
    return status;
}

static bool finished(int status)
{
    return status == GAME_ENCOUNTER_CLEARED || status == GAME_ENCOUNTER_DEFEATED;
}

/* The pause menu's body class for this status. */
static const char *menu_state(int status)
{
    if (status == GAME_ENCOUNTER_CLEARED) {
        return "cleared";
    }
    return status == GAME_ENCOUNTER_DEFEATED ? "defeated" : "paused";
}

/* Runs what the pause menu's last activation asks for: 1 quits. */
static int menu_command(app_game_presentation *game, int status)
{
    char name[64];
    if (!render_ui_take_overlay_event(engine_viewport_ui(game->viewport), name, sizeof name)) {
        return 0;
    }
    if (!strcmp(name, "Resume")) {
        return finished(status) ? 0 : command(game, APP_RUNTIME_PLAY);
    }
    if (!strcmp(name, "Restart")) {
        return command(game, APP_RUNTIME_RESTART);
    }
    if (!strcmp(name, "Quit")) {
        return 1;
    }
    fprintf(stderr, "PauseMenu event %s is not Resume, Restart, or Quit\n", name);
    return 0;
}

/* SDL delivers typed text only while text input is started: follow the
   game documents' focus, as a browser does a field's. */
static void sync_text_input(app_game_presentation *game)
{
    bool wants = engine_viewport_ui_typing(game->viewport);
    if (wants == game->text_input) {
        return;
    }
    game->text_input = wants;
    if (wants) {
        cf_input_enable_ime();
        return;
    }
    cf_input_disable_ime();
}

/* Prints a message once when it appears or changes. */
static void report_once(char *last, size_t size, const char *message)
{
    if (!strcmp(message, last)) {
        return;
    }
    snprintf(last, size, "%s", message);
    if (message[0]) {
        fprintf(stderr, "%s\n", message);
    }
}

/* UI asset errors and lighting adjustments. */
static void report_ui_error(app_game_presentation *game)
{
    report_once(
        game->ui_error,
        sizeof game->ui_error,
        render_ui_error(engine_viewport_ui(game->viewport))
    );
    report_once(
        game->lighting_warning,
        sizeof game->lighting_warning,
        app_scene_renderer_warning(game->scene->renderer)
    );
}

int app_game_presentation_frame(app_game_presentation *game, double now)
{
    ImVec2 size = display_size();
    int status = encounter_status(game->scene->world);
    bool playing = engine_is_playing(game->scene->runtime);
    /* A replay plays on unattended, restarting when it finishes. */
    bool replaying = app_scenario_replaying(game->scene);
    if (!game->showcase_time_started) {
        game->showcase_time_started = true;
    } else if (playing && now > game->showcase_last_time) {
        game->showcase_time += now - game->showcase_last_time;
    }
    game->showcase_last_time = now;
    if (playing && finished(status) && !replaying) {
        if (command(game, APP_RUNTIME_PAUSE) != 0) {
            return -1;
        }
        playing = false;
    }
    bool focused = !game->interactive || replaying || cf_app_has_focus();
    /* A focused game text field takes the letters the shortcuts use; Escape
       still pauses. */
    bool typing = playing && engine_viewport_ui_typing(game->viewport);
    if ((playing && ImGui_IsKeyPressed(ImGuiKey_Escape)) || (playing && !focused)) {
        if (command(game, APP_RUNTIME_PAUSE) != 0) {
            return -1;
        }
        playing = false;
    }
    if (focused && !typing && ImGui_IsKeyPressed(ImGuiKey_R)) {
        if (command(game, APP_RUNTIME_RESTART) != 0) {
            return -1;
        }
        playing = true;
    }
    if (focused && !playing && ImGui_IsKeyPressed(ImGuiKey_Q)) {
        return 1;
    }
    if (focused && !playing && ImGui_IsKeyPressed(ImGuiKey_Enter)) {
        if (command(game, finished(status) ? APP_RUNTIME_RESTART : APP_RUNTIME_PLAY) != 0) {
            return -1;
        }
        playing = true;
    }
    if (size.x > 0 && size.y > 0) {
        engine_viewport_request_resize(game->viewport, (int)size.x, (int)size.y);
    }
    engine_viewport_view authored_view;
    int authored_camera = game_camera_view(game->scene->world, game->scene->runtime, &authored_view);
    if (authored_camera < 0) {
        fprintf(stderr, "GameCamera requires one enabled camera with positive spans and a Transform.\n");
        return -1;
    }
    if (authored_camera) {
        engine_viewport_set_view(game->viewport, &authored_view);
        engine_viewport_clear_arena_camera(game->viewport);
    } else if (game->scene->perspective) {
        engine_viewport_set_arena_showcase(game->viewport, (float)game->showcase_time);
    } else {
        engine_viewport_clear_arena_camera(game->viewport);
    }
    engine_set_focus(game->scene->runtime, playing && focused ? ENGINE_FOCUS_VIEWPORT : ENGINE_FOCUS_NONE);
    CF_V2 target;
    bool mouse = mouse_target(game, &target);
    bool ui_capture = engine_viewport_ui_capture(game->viewport) || typing;
    app_gameplay_sample(game->scene, playing && focused && !ui_capture, mouse ? &target : NULL);
    if (engine_frame(game->scene->runtime, now) != FLOW_OK ||
        app_scenario_follow(game->scene) != 0) {
        fprintf(stderr, "Game frame failed: %s\n", engine_error(game->scene->runtime));
        return -1;
    }
    app_scene_renderer_set_time(
        game->scene->renderer,
        (float)engine_last_timing(game->scene->runtime)->simulation_time
    );
    status = encounter_status(game->scene->world);
    if (playing && finished(status) && !replaying) {
        if (command(game, APP_RUNTIME_PAUSE) != 0) {
            return -1;
        }
        playing = false;
        engine_set_focus(game->scene->runtime, ENGINE_FOCUS_NONE);
    }
    ImGui_SetNextWindowPos((ImVec2){ 0 }, ImGuiCond_Always);
    ImGui_SetNextWindowSize(size, ImGuiCond_Always);
    ImGui_PushStyleVarImVec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0 });
    ImGui_PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui_PushStyleVar(ImGuiStyleVar_ImageBorderSize, 0);
    ImGui_Begin(
        "Game",
        NULL,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBringToFrontOnFocus
    );
    render_ui_set_overlay(
        engine_viewport_ui(game->viewport),
        UI_DOCUMENT_PAUSE_MENU,
        playing ? NULL : menu_state(status)
    );
    engine_viewport_set_time(game->viewport, now);
    int drawn = engine_viewport_image(game->viewport, size.x, size.y);
    if (game->interactive) {
        render_audio_update(
            game->audio,
            engine_audio(game->scene->runtime),
            engine_is_playing(game->scene->runtime)
        );
    }
    engine_viewport_ui_input(game->viewport, focused);
    engine_viewport_ui_keys(game->viewport, playing && focused);
    sync_text_input(game);
    report_ui_error(game);
    ImGui_End();
    ImGui_PopStyleVarEx(3);
    if (drawn != 0) {
        fprintf(stderr, "Could not create the game viewport target\n");
        return -1;
    }
    if (game->render_failed) {
        fprintf(
            stderr,
            "Game rendering failed: %s\n",
            app_scene_renderer_error(game->scene->renderer)
        );
        return -1;
    }
    return menu_command(game, status);
}
