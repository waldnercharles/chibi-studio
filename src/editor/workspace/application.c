#include "chibi/application.h"

#include "assets/project.h"
#include "assets/prototype.h"
#include "authoring/application_catalog.h"
#include "authoring/component_declarations.h"
#include "authoring/engine_preview.h"
#include "authoring/image_prepare.h"
#include "authoring/mesh_prepare.h"
#include "authoring/preview.h"
#include "authoring/shader_prepare.h"
#include "authoring/source_prepare.h"
#include "authoring/source_presentation.h"
#include "authoring/studio_project.h"
#include "authoring/studio_schema.h"
#include "diagnostics/frame_log.h"
#include "diagnostics/tracy.h"
#include "editor/collab/actions.h"
#include "editor/collab/collab.h"
#include "editor/collab/workspace.h"
#include "editor/studio/studio.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/game_presentation.h"
#include "editor/workspace/gameplay_input.h"
#include "editor/workspace/input.h"
#include "editor/workspace/scenario_playback.h"
#include "editor/workspace/scene_authoring.h"
#include "editor/workspace/viewport.h"
#include "execution/status.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "render/screenshot.h"
#include "render/shader_images.h"
#include "runtime/component_catalog.h"
#include "runtime/engine.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#define APP_DEFAULT_SCREENSHOT_PATH "/tmp/chibi-screenshot.png"
/* The longest an idle editor goes without a frame, so file watches, toasts
   and notices still come round. */
#define APP_IDLE_FRAME 0.25
#define APP_DEFAULT_SCREENSHOT_FRAMES 3

/* Closing the window, or the platform's Quit, reaches Studio as a request
   rather than ending the loop, so it can ask about unsaved edits first. Set
   where SDL queues the event, read by the frame. */
static SDL_AtomicInt quit_requested;

static bool SDLCALL hold_quit(void *, SDL_Event *event)
{
    if (event->type != SDL_EVENT_QUIT) {
        return true;
    }
    SDL_SetAtomicInt(&quit_requested, 1);
    return false;
}

typedef struct
{
    editor *graph_editor;
    engine *runtime;
    ecs_world *world;
    game_preview_world *live;
    game_preview_definition *placeholder; /* owned, never registered as execution */
    engine_viewport *viewport;
    app_scene_authoring *authoring;
    declared_components *declared; /* the open project's, or NULL */
    app_game_presentation *game;
    studio *studio;
    int frames;
    int frame;
    const char *screenshot_path;
    bool show_graph; /* --open with --screenshot: the graph canvas fills the window */
    int capture_error;
    int headless;
    double monotonic_time;
    double fixed_dt;
    uint64_t next_frame_ms; /* an idle editor waits for input until then */
    char *project_root;     /* the open project, as opened */
    char *local_project_root;
    char *local_workspace;
    bool collab_initialized;
    bool destroying;
    bool keeps_workspace; /* --checkpoint restores unsaved edits, so closing never asks */
    double log_start_ms, log_last_ms, log_dropped_seconds;
    uint64_t log_simulation_steps;
} app_loop;

static void app_collab_prepare(app_loop *loop);

/* Frees what open_workspace built; the loop itself remains. */
static void close_workspace_state(app_loop *loop, bool clear_renderer)
{
    app_game_presentation_free(loop->game);
    app_scene_authoring_free(loop->authoring);
    engine_free(loop->runtime);
    game_preview_world_free(loop->live);
    editor_free(loop->graph_editor);
    game_preview_definition_free(loop->placeholder);
    if (clear_renderer) {
        authoring_shader_catalog_clear();
    }
    ecs_world_free(loop->world);
    declared_components_free(loop->declared);
    engine_viewport_free(loop->viewport);
    free(loop->project_root);
    loop->game = NULL;
    loop->authoring = NULL;
    loop->runtime = NULL;
    loop->live = NULL;
    loop->graph_editor = NULL;
    loop->placeholder = NULL;
    loop->world = NULL;
    loop->declared = NULL;
    loop->viewport = NULL;
    loop->project_root = NULL;
}

static void close_workspace(app_loop *loop)
{
    close_workspace_state(loop, true);
}

static void app_cleanup(app_loop *loop)
{
    loop->destroying = true;
    if (loop->collab_initialized) {
        collab_shutdown();
        loop->collab_initialized = false;
    }
    studio_free(loop->studio);
    loop->studio = NULL;
    app_input_shutdown();
    close_workspace(loop);
    free(loop->local_project_root);
    free(loop->local_workspace);
    free(loop);
}

/* Whether an idle editor waits instead of running this frame: until input
 * arrives or its next frame is due, blocking natively and skipping the
 * browser's animation frame on the web. */
static bool app_idle(const app_loop *loop)
{
    uint64_t now = SDL_GetTicks();
    if (now >= loop->next_frame_ms || SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
        return false;
    }
#ifdef __EMSCRIPTEN__
    return true;
#else
    SDL_WaitEventTimeout(NULL, (Sint32)(loop->next_frame_ms - now));
    return false;
#endif
}

/* An editor without anything moving waits for input, up to APP_IDLE_FRAME;
 * games, captures and fixed frame counts always run. */
static void schedule_frame(app_loop *loop)
{
    double idle = loop->game || loop->frames || loop->fixed_dt
                    ? 0
                    : studio_idle_time(loop->studio);
    loop->next_frame_ms = SDL_GetTicks() +
                          (uint64_t)(fmin(idle, APP_IDLE_FRAME) * 1000);
}

/* The scene runtime's frame timing, for the browser's frame log. */
static void log_frame(app_loop *loop)
{
    double now = (double)SDL_GetTicksNS() / 1e6;
    if (!loop->log_start_ms) {
        loop->log_start_ms = loop->log_last_ms = now;
    }
    const engine_frame_timing *timing = engine_last_timing(loop->runtime);
    double dropped = timing->dropped_seconds - loop->log_dropped_seconds;
    if (dropped < 0 || timing->simulation_steps < loop->log_simulation_steps) {
        /* The game may replace the scene engine while the app log keeps running. */
        dropped = timing->dropped_seconds;
    }
    frame_log_push((frame_log_entry){
        .time_ms = now - loop->log_start_ms,
        .interval_ms = now - loop->log_last_ms,
        .steps = (int)timing->steps,
        .dropped_ms = dropped * 1000,
        .behavior_ms = timing->behavior_ms,
        .physics_ms = timing->physics_ms,
        .systems_ms = timing->systems_ms,
        .rendering_ms = timing->rendering_ms,
    });
    loop->log_last_ms = now;
    loop->log_dropped_seconds = timing->dropped_seconds;
    loop->log_simulation_steps = timing->simulation_steps;
}

static int app_frame(app_loop *loop)
{
    if (!cf_app_is_running()) {
        return 1;
    }
    app_collab_prepare(loop);
    if (loop->collab_initialized) {
        collab_tick();
    }
    if (app_idle(loop)) {
        return 0;
    }
    CHIBI_TRACE_ZONE_BEGIN(frame_zone, "Application frame");
    CHIBI_TRACE_ZONE_BEGIN(window_zone, "Window update");
    cf_app_update(NULL);
    CHIBI_TRACE_ZONE_END(window_zone);
    CHIBI_TRACE_ZONE_BEGIN(input_zone, "Input");
    app_input_frame();
    CHIBI_TRACE_ZONE_END(input_zone);
    double dt = loop->fixed_dt ? loop->fixed_dt : CF_DELTA_TIME;
    loop->monotonic_time += dt;
    int status = 0;
    if (loop->game) {
        CHIBI_TRACE_ZONE_BEGIN(game_zone, "Game presentation");
        status = app_game_presentation_frame(loop->game, loop->monotonic_time);
        CHIBI_TRACE_ZONE_END(game_zone);
    } else {
        CHIBI_TRACE_ZONE_BEGIN(editor_zone, "Editor");
        editor_tick(loop->graph_editor, loop->monotonic_time);
        app_gameplay_input(loop->authoring, loop->runtime, loop->viewport);
        if (app_scene_authoring_frame(loop->authoring, loop->monotonic_time) != FLOW_OK ||
            app_scenario_follow(loop->authoring) != 0) {
            fprintf(stderr, "runtime frame failed\n");
        }
        studio_frame(loop->studio, dt);
        CHIBI_TRACE_ZONE_END(editor_zone);
    }
    log_frame(loop);
    CHIBI_TRACE_ZONE_BEGIN(present_zone, "Present / GPU wait");
    cf_app_draw_onto_screen(true);
    CHIBI_TRACE_ZONE_END(present_zone);
    schedule_frame(loop);
    CHIBI_TRACE_FRAME_MARK("Application frame");
    if (status) {
        CHIBI_TRACE_ZONE_END(frame_zone);
        return status;
    }

    int finished = loop->frames > 0 && ++loop->frame >= loop->frames;
    if (finished && loop->screenshot_path) {
        CHIBI_TRACE_ZONE_BEGIN(screenshot_zone, "Screenshot");
        loop->capture_error = app_save_screenshot(loop->screenshot_path);
        CHIBI_TRACE_ZONE_END(screenshot_zone);
        CHIBI_TRACE_ZONE_END(frame_zone);
        return 1;
    }
    CHIBI_TRACE_ZONE_END(frame_zone);
    return finished;
}

/* --open names a behavior, event, library, material, shape or field asset;
   --graph selects one of its graphs by name. */
static int open_graph(
    app_scene_authoring *authoring,
    editor *graph_editor,
    const char *asset,
    const char *graph_name,
    char *error,
    size_t error_size
)
{
    if (app_scene_authoring_open_behavior(authoring, asset) != 0) {
        snprintf(error, error_size, "--open: no graph asset named %s", asset);
        return -1;
    }
    if (!graph_name) {
        return 0;
    }
    const graph *target = behavior_asset_find_graph_by_name(editor_behavior_asset(graph_editor), graph_name);
    if (!target || editor_open_graph(graph_editor, target->graph_id) != 0) {
        snprintf(error, error_size, "--graph: %s has no graph named %s", asset, graph_name);
        return -1;
    }
    return 0;
}

/* Installs the components the project declares in content/components. */
static bool load_declared(const char *content, declared_components **out, char *error, size_t size)
{
    native_capabilities *native = authoring_application_catalog_new(true, error, size);
    bool loaded = native && authoring_component_declarations_load(
                                content,
                                native_capabilities_catalog(native),
                                out,
                                error,
                                size
                            );
    native_capabilities_free(native);
    return loaded;
}

/* Opens `project_path` at `scene_path` (NULL for its start scene) into the
 * loop's world, runtime, authoring and viewport. */
static bool open_workspace(
    app_loop *loop,
    const char *project_path,
    const char *scene_path,
    bool,
    char *error,
    size_t error_size
)
{
    chibi_project project = { 0 };
    declared_components *declared = NULL;
    if (chibi_project_open(&project, project_path, error, error_size) != 0) {
        return false;
    }
    char *resolved_scene = chibi_project_scene_path(&project, scene_path);
    if (!resolved_scene) {
        snprintf(
            error,
            error_size,
            "Project does not define a start scene: %s",
            project.root
        );
        chibi_project_free(&project);
        return false;
    }

    /* The host owns shader setup; image sources must precede graph registration. */
    if (!render_image_mount_content(project.content, error, error_size)
        || !authoring_mesh_mount_content(project.content, error, error_size)
        || !load_declared(project.content, &declared, error, error_size)) {
        free(resolved_scene);
        declared_components_free(declared);
        chibi_project_free(&project);
        return false;
    }
    registry_validate();
    const script_catalog placeholder_catalog = { 0 };
    const script_lower_bindings placeholder_bindings = { 0 };
    authoring_source *source = authoring_source_prepare_document(
        "AppBehavior.chibi", "behaviors/AppBehavior.chibi", "behavior AppBehavior {}",
        &placeholder_catalog, &placeholder_bindings, BEHAVIOR_BYTECODE
    );
    if (!source->valid) {
        snprintf(error, error_size, "Could not prepare workspace presentation: %s",
                 source->diagnostic_count ? source->diagnostics[0].message : "invalid source");
        authoring_source_free(source);
        free(resolved_scene);
        declared_components_free(declared);
        chibi_project_free(&project);
        return false;
    }
    authoring_source_presentation *presentation = authoring_source_presentation_new(
        source->analysis, &placeholder_catalog, NULL
    );
    assert(presentation);
    game_preview_definition *definition = game_preview_projection_new("AppBehavior", presentation);
    editor *graph_editor = editor_new(authoring_source_presentation_take_view(
        authoring_source_presentation_clone(presentation)
    ));
    int adopted = editor_source_presentation(graph_editor, presentation);
    assert(adopted == 0);
    (void)adopted;
    authoring_source_presentation_free(presentation);
    authoring_source_free(source);
    ecs_world *world = ecs_world_new();
    engine_viewport *viewport = engine_viewport_new(640, 360);
    game_preview_world *live = game_preview_world_new(world);
    engine *runtime = authoring_engine_new(world, live->runtime);
    app_scene_authoring *authoring = NULL;
    /* The preview runtime owns registration of its GAME component schemas. */
    if (!world || !viewport || !graph_editor || !live || !definition || !runtime) {
        snprintf(error, error_size, "Could not initialize workspace runtime");
        app_scene_authoring_free(authoring);
        engine_free(runtime);
        game_preview_world_free(live);
        editor_free(graph_editor);
        game_preview_definition_free(definition);
        ecs_world_free(world);
        engine_viewport_free(viewport);
        free(resolved_scene);
        declared_components_free(declared);
        chibi_project_free(&project);
        return false;
    }
    editor_set_studio_nodes(graph_editor, studio_project_is(project.root));
    authoring = app_scene_authoring_new_with_content_root(
        world,
        live,
        definition,
        editor_behavior_asset(graph_editor),
        graph_editor,
        NULL,
        NULL,
        runtime,
        project.content
    );
    if (authoring) {
        app_scene_authoring_set_default_ui(authoring, project.ui);
        if (studio_project_is(project.root)) {
            app_scene_authoring_set_studio_project(authoring);
        }
    }
    bool scene_ready = authoring &&
                       app_scene_authoring_load_content(authoring) == 0 &&
                       app_scene_authoring_load(authoring, resolved_scene) == 0;
    if (!scene_ready) {
        snprintf(
            error,
            error_size,
            "Could not initialize scene: %s",
            authoring ? app_scene_authoring_message(authoring) : "authoring unavailable"
        );
        app_scene_authoring_free(authoring);
        engine_free(runtime);
        game_preview_world_free(live);
        editor_free(graph_editor);
        game_preview_definition_free(definition);
        ecs_world_free(world);
        engine_viewport_free(viewport);
        free(resolved_scene);
        declared_components_free(declared);
        chibi_project_free(&project);
        return false;
    }
    free(resolved_scene);
    char *root = xstrdup(project.root);
    chibi_project_free(&project);
    editor_set_event_catalog(graph_editor, GAME_HOST_EVENTS, GAME_HOST_EVENT_COUNT);
    editor_set_ui_context(graph_editor, EDITOR_UI_CONTEXT_BEHAVIOR);
    engine_set_focus(runtime, ENGINE_FOCUS_VIEWPORT);
    engine_viewport_set_render_callback(viewport, app_scene_authoring_render, authoring);

    loop->graph_editor = graph_editor;
    loop->runtime = runtime;
    loop->world = world;
    loop->live = live;
    loop->placeholder = definition;
    loop->viewport = viewport;
    loop->authoring = authoring;
    loop->declared = declared;
    loop->project_root = root;
    return true;
}

static void app_workspace_take(app_loop *loop, app_loop *candidate)
{
    loop->graph_editor = candidate->graph_editor;
    loop->runtime = candidate->runtime;
    loop->world = candidate->world;
    loop->live = candidate->live;
    loop->placeholder = candidate->placeholder;
    loop->viewport = candidate->viewport;
    loop->authoring = candidate->authoring;
    loop->declared = candidate->declared;
    loop->project_root = candidate->project_root;
    loop->studio = candidate->studio;
    candidate->graph_editor = NULL;
    candidate->runtime = NULL;
    candidate->world = NULL;
    candidate->live = NULL;
    candidate->placeholder = NULL;
    candidate->viewport = NULL;
    candidate->authoring = NULL;
    candidate->declared = NULL;
    candidate->project_root = NULL;
    candidate->studio = NULL;
}

static bool app_workspace_build(
    app_loop *candidate,
    const char *project_root,
    const char *workspace,
    char *error,
    size_t error_size
)
{
    if (!open_workspace(candidate, project_root, NULL, false, error, error_size)) {
        close_workspace(candidate);
        return false;
    }
    if (!collab_workspace_restore_model(candidate->authoring, candidate->graph_editor, workspace, error, error_size)) {
        close_workspace(candidate);
        return false;
    }
    candidate->studio = studio_new(
        candidate->authoring,
        candidate->viewport,
        candidate->project_root
    );
    if (!candidate->studio ||
        !collab_workspace_restore_presentation(candidate->studio, workspace, error, error_size)) {
        studio_free(candidate->studio);
        candidate->studio = NULL;
        close_workspace(candidate);
        if (error_size && !error[0]) {
            snprintf(error, error_size, "Could not create the candidate Studio view");
        }
        return false;
    }
    return true;
}

/* Build the entire remote workspace before replacing the current one. The
 * old models stay allocated until the candidate has restored successfully. */
static bool app_replace_workspace(
    app_loop *loop,
    const char *project_root,
    const char *workspace,
    char *error,
    size_t error_size
)
{
    if (error_size) {
        error[0] = '\0';
    }
    if (!loop->studio || loop->game || !loop->project_root) {
        snprintf(error, error_size, "Studio workspace is unavailable");
        return false;
    }
    char *rollback = collab_workspace_checkpoint(
        loop->authoring,
        loop->graph_editor,
        loop->studio,
        loop->project_root
    );
    if (!rollback) {
        snprintf(error, error_size, "Could not preserve the current workspace");
        return false;
    }

    char *previous_root = xstrdup(loop->project_root);
    studio_free(loop->studio);
    loop->studio = NULL;
    /* Images, shader definitions, and mount precedence are process-global. A
     * candidate must start from a clean catalog and mount its own project. */
    authoring_shader_catalog_clear();

    app_loop candidate = { 0 };
    bool candidate_ready = app_workspace_build(&candidate, project_root, workspace, error, error_size);
    if (!candidate_ready) {
        app_loop previous = { 0 };
        char recovery_error[512] = { 0 };
        bool recovered = app_workspace_build(&previous, previous_root, rollback, recovery_error, sizeof recovery_error);
        if (recovered) {
            close_workspace_state(loop, false);
            app_workspace_take(loop, &previous);
        } else {
            studio_free(previous.studio);
            close_workspace(&previous);
            loop->studio = studio_new(loop->authoring, loop->viewport, loop->project_root);
            if (loop->studio) {
                char ignored[256];
                (void)collab_workspace_restore_presentation(loop->studio, rollback, ignored, sizeof ignored);
            }
            if (error_size) {
                snprintf(error, error_size, "Could not restore previous workspace: %s", recovery_error);
            }
        }
        free(previous_root);
        free(rollback);
        return false;
    }

    close_workspace_state(loop, false);
    app_workspace_take(loop, &candidate);
    free(previous_root);
    free(rollback);
    return true;
}

static bool app_collab_save_local(app_loop *loop)
{
    if (loop->local_workspace) {
        return true;
    }
    if (loop->game || !loop->studio || !loop->project_root) {
        return false;
    }
    char *workspace = collab_workspace_checkpoint(
        loop->authoring,
        loop->graph_editor,
        loop->studio,
        loop->project_root
    );
    if (!workspace) {
        return false;
    }
    loop->local_project_root = xstrdup(loop->project_root);
    loop->local_workspace = workspace;
    return true;
}

static void app_collab_prepare(app_loop *loop)
{
    if (loop->collab_initialized && collab_active()) {
        (void)app_collab_save_local(loop);
    }
}

static char *app_collab_checkpoint(void *context)
{
    app_loop *loop = context;
    return collab_workspace_checkpoint(loop->authoring, loop->graph_editor, loop->studio, loop->project_root);
}

static bool app_collab_apply(void *context, const char *action, char *error, size_t error_size)
{
    app_loop *loop = context;
    yyjson_doc *doc = action ? yyjson_read(action, strlen(action), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *family = yyjson_get_str(yyjson_obj_get(root, "family"));
    bool applied = false;
    if (family && (!strcmp(family, "graph") || !strcmp(family, "scene"))) {
        applied = collab_actions_apply(loop->authoring, loop->graph_editor, action, error, error_size);
    } else if (family && (!strcmp(family, "text") || !strcmp(family, "command"))) {
        applied = studio_collab_apply(loop->studio, action, error, error_size);
    } else if (error_size) {
        snprintf(error, error_size, "Unsupported collaboration action family");
    }
    if (applied) {
        loop->next_frame_ms = 0;
    }
    yyjson_doc_free(doc);
    return applied;
}

static void app_collab_presence(
    void *context,
    const char *client,
    const char *name,
    uint32_t color,
    const char *json
)
{
    app_loop *loop = context;
    if (loop->studio) {
        studio_collab_receive(loop->studio, client, name, color, json);
        loop->next_frame_ms = 0;
    }
}

static bool app_collab_restore(void *context, const char *payload, char *error, size_t error_size)
{
    app_loop *loop = context;
    yyjson_doc *doc = payload ? yyjson_read(payload, strlen(payload), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *workspace_value = yyjson_is_obj(root)
                                    ? yyjson_obj_get(root, "workspace")
                                    : NULL;
    yyjson_val *project_value = yyjson_is_obj(root)
                                  ? yyjson_obj_get(root, "project_root")
                                  : NULL;
    const char *workspace = yyjson_get_str(workspace_value);
    const char *project_root = yyjson_get_str(project_value);
    bool valid = yyjson_is_obj(root) &&
                 yyjson_get_int(yyjson_obj_get(root, "checkpoint_version")) == 1 &&
                 yyjson_is_str(workspace_value) && workspace &&
                 yyjson_is_str(project_value) && project_root;
    if (!valid) {
        yyjson_doc_free(doc);
        snprintf(error, error_size, "Invalid collaboration snapshot payload");
        return false;
    }
    bool had_local = loop->local_workspace != NULL;
    if (!app_collab_save_local(loop)) {
        yyjson_doc_free(doc);
        snprintf(error, error_size, "Could not preserve the local workspace");
        return false;
    }
    bool restored = app_replace_workspace(loop, project_root, workspace, error, error_size);
    if (restored) {
        loop->next_frame_ms = 0;
    }
    yyjson_doc_free(doc);
    if (!restored && !had_local) {
        free(loop->local_project_root);
        free(loop->local_workspace);
        loop->local_project_root = NULL;
        loop->local_workspace = NULL;
    }
    return restored;
}

static void app_collab_closed(void *context)
{
    app_loop *loop = context;
    if (loop->destroying || !loop->local_workspace || !loop->local_project_root) {
        return;
    }
    char error[512] = { 0 };
    bool restored = app_replace_workspace(
        loop,
        loop->local_project_root,
        loop->local_workspace,
        error,
        sizeof error
    );
    if (restored) {
        loop->next_frame_ms = 0;
        free(loop->local_project_root);
        free(loop->local_workspace);
        loop->local_project_root = NULL;
        loop->local_workspace = NULL;
        return;
    }
    if (loop->studio) {
        studio_error(loop->studio, "Could not restore local workspace", error);
    }
}

/* Replaces the open project with the one at `path`, rebuilding the editor as
 * a launch would. A project that fails to open reopens the current one. */
static void open_project(app_loop *loop, const char *path)
{
    char error[512];
    char *previous = xstrdup(loop->project_root);
    studio_free(loop->studio);
    close_workspace(loop);
    bool opened = open_workspace(loop, path, NULL, false, error, sizeof error);
#ifndef __EMSCRIPTEN__
    if (opened) {
        studio_remember_project(loop->project_root);
    }
#endif
    if (!opened) {
        char unused[512];
        bool restored = open_workspace(loop, previous, NULL, false, unused, sizeof unused);
        assert(restored);
        (void)restored;
    }
    free(previous);
    loop->studio = studio_new(loop->authoring, loop->viewport, loop->project_root);
    if (!opened) {
        studio_error(loop->studio, "Could not open project", error);
    }
}

#ifndef __EMSCRIPTEN__
/* The most recent of `roots` that still opens, or NULL. */
static const char *openable(char **roots)
{
    for (int i = 0; i < acount(roots); i++) {
        chibi_project project = { 0 };
        char error[256];
        if (chibi_project_open(&project, roots[i], error, sizeof error) == 0) {
            chibi_project_free(&project);
            return roots[i];
        }
    }
    return NULL;
}
#endif

void *chibi_application_create(int argc, char **argv, const char *checkpoint, char *error, size_t error_size)
{
    snprintf(error, error_size, "Could not initialize application");
    int frames = 0;
    int headless = 0;
    bool play = false;
    bool keeps_workspace = false;
    double fixed_dt = 0;
    const char *scene_path = NULL;
    const char *project_path = NULL;
    const char *screenshot_path = NULL;
    const char *open = NULL;
    const char *graph_name = NULL;
    const char *scenario = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);

        } else if (strcmp(argv[i], "--screenshot") == 0) {
            screenshot_path = APP_DEFAULT_SCREENSHOT_PATH;
            headless = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                screenshot_path = argv[++i];
            }
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
        } else if (strcmp(argv[i], "--play") == 0) {
            play = true;
        } else if (strcmp(argv[i], "--scenario") == 0) {
            if (i + 1 == argc || argv[i + 1][0] == '-') {
                snprintf(error, error_size, "--scenario requires a scenarios asset name");
                return NULL;
            }
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--scene") == 0) {
            if (i + 1 == argc || argv[i + 1][0] == '-') {
                snprintf(error, error_size, "--scene requires an authored scene path");
                return NULL;
            }
            scene_path = argv[++i];
        } else if ((strcmp(argv[i], "--project") == 0 || strcmp(argv[i], "--project-root") == 0)) {
            if (i + 1 == argc || argv[i + 1][0] == '-') {
                snprintf(error, error_size, "%s requires a project directory or project.kdl", argv[i]);
                return NULL;
            }
            project_path = argv[++i];
        } else if (strcmp(argv[i], "--open") == 0 || strcmp(argv[i], "--graph") == 0) {
            if (i + 1 == argc || argv[i + 1][0] == '-') {
                snprintf(error, error_size, "%s requires a name", argv[i]);
                return NULL;
            }
            *(argv[i][2] == 'o' ? &open : &graph_name) = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--fixed-dt") == 0) {
            if (i + 1 == argc) {
                snprintf(error, error_size, "--fixed-dt requires a positive number of seconds");
                return NULL;
            }
            char *end;
            fixed_dt = strtod(argv[++i], &end);
            if (*end || !isfinite(fixed_dt) || fixed_dt <= 0) {
                snprintf(error, error_size, "--fixed-dt requires a positive number of seconds");
                return NULL;
            }
        } else if (strcmp(argv[i], "--checkpoint") == 0) {
            keeps_workspace = true;
            i++;
        } else if (strcmp(argv[i], "--fps") == 0) {
            i++;
        } else if (argv[i][0] != '-' && !project_path) {
            project_path = argv[i];
        }
    }
    if (screenshot_path && frames == 0) {
        frames = APP_DEFAULT_SCREENSHOT_FRAMES;
    }

    /* A saved workspace reopens the project Studio had open at shutdown. */
    yyjson_doc *saved = checkpoint ? yyjson_read(checkpoint, strlen(checkpoint), 0)
                                   : NULL;
    const char *saved_project = yyjson_get_str(
        yyjson_obj_get(yyjson_doc_get_root(saved), "project")
    );
    if (saved_project) {
        project_path = saved_project;
        scene_path = NULL;
    }
    /* Without a project, open the one the working directory is inside, else
       the one opened last. */
    char *found = project_path ? NULL : chibi_project_find(".");
    if (found) {
        project_path = found;
    }
#ifndef __EMSCRIPTEN__
    /* Only a person's launch reopens or remembers a project; captures and
       bounded runs leave both alone. */
    bool interactive = !headless && !frames && !screenshot_path;
    char **recent = project_path || !interactive ? NULL : studio_recent_projects();
    project_path = project_path ? project_path : openable(recent);
    if (!project_path) {
        snprintf(error, error_size, "No project to open. Start Studio inside a project folder, or pass one: chibi-dev PATH");
        yyjson_doc_free(saved);
        return NULL;
    }
#endif
    app_loop *loop = xmalloc(sizeof *loop);
    *loop = (app_loop){ .frames = frames,
                        .screenshot_path = screenshot_path,
                        .show_graph = open && screenshot_path,
                        .headless = headless,
                        .fixed_dt = fixed_dt,
                        .keeps_workspace = keeps_workspace };
    bool opened = open_workspace(loop, project_path, scene_path, play, error, error_size);
    yyjson_doc_free(saved);
    free(found);
#ifndef __EMSCRIPTEN__
    studio_recent_projects_free(recent);
    if (opened && interactive) {
        studio_remember_project(loop->project_root);
    }
#endif
    if (!opened) {
        free(loop);
        return NULL;
    }
    app_scene_authoring *authoring = loop->authoring;
    editor *graph_editor = loop->graph_editor;
    engine_viewport *viewport = loop->viewport;
    if (open &&
        open_graph(authoring, graph_editor, open, graph_name, error, error_size) != 0) {
        app_cleanup(loop);
        return NULL;
    }
    if (play) {
        loop->game = app_game_presentation_new(authoring, viewport, !headless);
        if (!loop->game ||
            (scenario && app_scenario_replay(authoring, scenario) != 0)) {
            snprintf(error, error_size, "Could not start game: %s", app_scene_authoring_message(authoring));
            app_cleanup(loop);
            return NULL;
        }
        return loop;
    }
    if (!checkpoint) {
        return loop;
    }
    yyjson_doc *doc = yyjson_read(checkpoint, strlen(checkpoint), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *scene_json = yyjson_get_str(yyjson_obj_get(root, "scene"));
    const char *graph_json = yyjson_get_str(yyjson_obj_get(root, "graph"));
    bool valid = yyjson_get_int(yyjson_obj_get(root, "version")) == 1 &&
                 scene_json && graph_json &&
                 app_scene_authoring_restore_checkpoint(authoring, scene_json) == 0 &&
                 editor_restore_checkpoint(graph_editor, graph_json);
    if (valid) {
        app_scene_authoring_set_editor_working(authoring, editor_behavior_asset(graph_editor));
    }
    yyjson_doc_free(doc);
    if (valid) {
        return loop;
    }
    snprintf(error, error_size, "Workspace checkpoint is incompatible or invalid");
    app_cleanup(loop);
    return NULL;
}

char *chibi_application_checkpoint(void *state)
{
    app_loop *loop = state;
    if (loop->game) {
        /* Play restarts from authored content with the current launch arguments. */
        const char saved[] = "{\"version\":1,\"mode\":\"play\"}";
        return xstrdup(saved);
    }
    char *scene = app_scene_authoring_checkpoint(loop->authoring);
    char *graph = editor_checkpoint(loop->graph_editor);
    char *json = NULL;
    if (scene && graph) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_int(doc, root, "version", 1);
        yyjson_mut_obj_add_str(doc, root, "project", loop->project_root);
        yyjson_mut_obj_add_str(doc, root, "scene", scene);
        yyjson_mut_obj_add_str(doc, root, "graph", graph);
        json = yyjson_mut_write(doc, 0, NULL);
        yyjson_mut_doc_free(doc);
    }
    free(scene);
    free(graph);
    return json;
}

void chibi_application_activate(void *state)
{
    app_loop *loop = state;
#ifndef __EMSCRIPTEN__
    if (loop->game) {
        cf_app_set_imgui_render_target(cf_app_get_canvas());
    }
#endif
    app_input_initialize();
    if (!loop->game) {
        loop->studio = studio_new(loop->authoring, loop->viewport, loop->project_root);
        if (loop->show_graph) {
            studio_maximize_graph(loop->studio);
        }
        collab_init((collab_config){ .context = loop,
                                     .apply = app_collab_apply,
                                     .checkpoint = app_collab_checkpoint,
                                     .restore = app_collab_restore,
                                     .presence = app_collab_presence,
                                     .closed = app_collab_closed });
        loop->collab_initialized = true;
        if (!loop->keeps_workspace) {
            SDL_SetEventFilter(hold_quit, NULL);
        }
    }
}

int chibi_application_frame(void *state)
{
    app_loop *loop = state;
    int finished = app_frame(loop);
    if (finished < 0) {
        return -1;
    }
    bool quit = SDL_SetAtomicInt(&quit_requested, 0);
    if (quit) {
        loop->next_frame_ms = 0; /* its confirmation shows at once */
    }
    if (!loop->game && studio_quit(loop->studio, quit)) {
        cf_app_signal_shutdown();
    }
    char *project = loop->game ? NULL : studio_take_project(loop->studio);
    if (project) {
        if (loop->collab_initialized && collab_active()) {
            collab_leave();
            app_collab_closed(loop);
        }
        open_project(loop, project);
        free(project);
    }
    return finished ? (loop->capture_error ? -1 : 1) : 0;
}

void chibi_application_destroy(void *state)
{
    app_cleanup(state);
}
