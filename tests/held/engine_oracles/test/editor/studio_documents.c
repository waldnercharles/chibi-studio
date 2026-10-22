#include "authoring/engine_preview.h"
/* Studio frames over a project on disk, in a hidden window: the fixtures and
 * input every surface's checks share, and the runner. Each surface's checks
 * live in studio_documents_<surface>.c (studio_documents.h). */
#include "editor/workspace/viewport.h"
#include "test/editor/studio_documents.h"

#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/input.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "runtime/component_catalog.h"
#include "test/suite_internal.h"
#include "ui/input_feel.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static studio_test_sample measured;

static bool timing_enabled()
{
    const char *value = getenv("CHIBI_TEST_TIMINGS");
    return value && !strcmp(value, "1");
}

static double wall_seconds()
{
    return timing_enabled()
             ? (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency()
             : 0;
}

studio_test_sample studio_test_begin(const char *name)
{
    studio_test_sample before = measured;
    before.wall = wall_seconds();
    if (timing_enabled()) {
        printf("STUDIO_BEGIN: %s\n", name);
        fflush(stdout);
    }
    return before;
}

void studio_test_end(const char *name, studio_test_sample before)
{
    if (!timing_enabled()) {
        return;
    }
    printf(
        "STUDIO_TIMING: %s wall=%.3fs fixtures=%d setup=%.3fs teardown=%.3fs "
        "frames=%d pump=%.3fs scene=%.3fs studio=%.3fs present=%.3fs "
        "batch_frames=%d batches=%.3fs\n",
        name,
        wall_seconds() - before.wall,
        measured.fixtures - before.fixtures,
        measured.setup - before.setup,
        measured.teardown - before.teardown,
        measured.frames - before.frames,
        measured.pump - before.pump,
        measured.scene - before.scene,
        measured.studio - before.studio,
        measured.present - before.present,
        measured.batch_frames - before.batch_frames,
        measured.batches - before.batches
    );
    fflush(stdout);
}

/* Studio over scene `scene` of the project at `project` with `behavior`
   open. */
studio_fixture fixture_open(const char *project, const char *scene, const char *behavior)
{
    double started = wall_seconds();
    studio_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Editing", behavior_asset_new("Editing"));
    assert(definition && game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Editing"));
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    f.viewport = engine_viewport_new(640, 360);
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
    char content[PATH_MAX];
    snprintf(content, sizeof content, "%s/content", project);
    assert(app_scene_authoring_set_content_root(f.scene, content) == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(app_scene_authoring_load(f.scene, scene) == 0);
    assert(app_scene_authoring_open_behavior(f.scene, behavior) == 0);
    measured.setup += wall_seconds() - started;
    measured.fixtures++;
    return f;
}

studio_fixture fixture_new()
{
    studio_fixture f = fixture_open(PROJECT, PROJECT "/content/scenes/Main.chibi", "Weapons");
    double started = wall_seconds();
    assert(editor_open_graph(f.graph, FUNCTION_GRAPH) == 0);
    f.studio = studio_new(f.scene, f.viewport, PROJECT);
    studio_maximize_graph(f.studio);
    measured.setup += wall_seconds() - started;
    return f;
}

void fixture_free(studio_fixture *f)
{
    double started = wall_seconds();
    studio_free(f->studio);
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
    engine_viewport_free(f->viewport);
    measured.teardown += wall_seconds() - started;
}

/* One application frame. File > Reload runs inside Studio's frame, before the
   canvas draws, so a reload lands between the scene and Studio frames. */
void frame_delta(studio_fixture *f, bool reload, double delta_seconds)
{
    double started = wall_seconds();
    f->time += delta_seconds;
    cf_app_update(NULL);
    app_input_frame();
    double now = wall_seconds();
    measured.pump += now - started;
    started = now;
    editor_tick(f->graph, f->time);
    assert(app_scene_authoring_frame(f->scene, f->time) == FLOW_OK);
    if (reload) {
        assert(app_scene_authoring_reload(f->scene) == 0);
    }
    now = wall_seconds();
    measured.scene += now - started;
    started = now;
    studio_frame(f->studio, delta_seconds);
    now = wall_seconds();
    measured.studio += now - started;
    started = now;
    cf_app_draw_onto_screen(true);
    measured.present += wall_seconds() - started;
    measured.frames++;
}

void frame(studio_fixture *f, bool reload)
{
    frame_delta(f, reload, FRAME_SECONDS);
}

void graph_drag_button(const studio *s, float x, float y, bool down)
{
    SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                     : SDL_EVENT_MOUSE_BUTTON_UP };
    event.button.windowID = SDL_GetWindowID(cf_app_get_window());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x * s->prefs.ui_scale;
    event.button.y = y * s->prefs.ui_scale;
    assert(SDL_PushEvent(&event));
}

/* Studio over `project`'s scene `scene` with `behavior` open and the Scene
   filling the workspace. Preferences stay as saved but for the layout: UI
   Scale 1 and the default split modes, so the areas the checks click are the
   same whatever the user's. */
studio_fixture scene_open(const char *project, const char *scene, const char *behavior)
{
    studio_fixture f = fixture_open(project, scene, behavior);
    double started = wall_seconds();
    f.studio = studio_new(f.scene, f.viewport, project);
    studio_prefs defaults = studio_prefs_default();
    studio_prefs *p = &f.studio->prefs;
    p->ui_scale = 1;
    p->workspace = defaults.workspace;
    memcpy(p->layouts, defaults.layouts, sizeof defaults.layouts);
    f.studio->applied = *p;
    f.studio->maximized = STUDIO_AREA_TOP;
    measured.setup += wall_seconds() - started;
    return f;
}

/* Studio over the Vault scene, whose Vault shows the Bank document and
   attaches Teller. */
studio_fixture vault_open()
{
    return scene_open(PROJECT, VAULT, "Teller");
}

void frames(studio_fixture *f, int count)
{
    double started = wall_seconds();
    for (int i = 0; i < count; i++) {
        frame(f, false);
        /* The user's preferences stay as they are. */
        f->studio->prefs_changed_at = 0;
    }
    measured.batch_frames += count;
    measured.batches += wall_seconds() - started;
}

/* Studio over the Studio project's scene, whose root document is its
   manifest's, as the application opens it. */
studio_fixture studio_project_open()
{
    static char root[PATH_MAX];
    char scene[PATH_MAX + 32];
    assert(realpath("projects/studio", root));
    snprintf(scene, sizeof scene, "%s/content/scenes/Studio.chibi", root);
    studio_fixture f = scene_open(root, scene, "Dismiss");
    app_scene_authoring_set_default_ui(f.scene, "StudioShell");
    return f;
}

void key_event_mod(SDL_Scancode key, bool down, SDL_Keymod mod, bool repeat)
{
    SDL_Event event = { .type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP };
    event.key.windowID = SDL_GetWindowID(cf_app_get_window());
    event.key.scancode = key;
    event.key.key = SDL_GetKeyFromScancode(key, mod, true);
    event.key.mod = mod;
    event.key.down = down;
    event.key.repeat = repeat;
    assert(SDL_PushEvent(&event));
}

void key_event(SDL_Scancode key, bool down)
{
    key_event_mod(key, down, SDL_KMOD_NONE, false);
}

/* The first element in Studio's documents that `selector` matches. */
rml_element *studio_element(studio *s, const char *selector)
{
    rml_context *context = rml_backend_context(s->backend);
    rml_element *found = NULL;
    for (int i = 0; !found && i < rml_context_document_count(context); i++) {
        found = rml_element_query_selector(rml_context_document(context, i), selector);
    }
    return found;
}

/* The rows data-for made in the element at `selector`, leaving out its
   template, and the first one's text. */
int list_rows(studio *s, const char *selector, char *first, size_t size)
{
    first[0] = '\0';
    rml_element *window = studio_element(s, selector);
    int rows = 0;
    for (int i = 0; window && i < rml_element_child_count(window); i++) {
        rml_element *row = rml_element_child(window, i);
        if (rml_element_has_attribute(row, "data-for") ||
            !rml_element_is_visible(row, false)) {
            continue;
        }
        if (!rows++) {
            rml_element_inner_rml(row, first, size);
        }
    }
    return rows;
}

/* The elements of Studio's `panel` that `selector` matches, up to 8. */
int panel_elements(studio *s, const char *panel, const char *selector, rml_element **found)
{
    char query[128];
    snprintf(query, sizeof query, "[data-ui=%s] %s", panel, selector);
    return test_query_all(rml_backend_context(s->backend), query, found, 8);
}

/* A press and release on `el`, which focuses it as a click does. Input is
   in dp, and the element's box in pixels. */
void press(studio *s, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    rml_input input = { .x = (at.x + size.x * 0.5f) / ratio,
                        .y = (at.y + size.y * 0.5f) / ratio };
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        input.type = types[i];
        rml_backend_process_input(s->backend, &input);
    }
}

int tab_named(const app_scene_authoring *scene, const char *label)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].label && !strcmp(scene->tabs[i].label, label)) {
            return i;
        }
    }
    return -1;
}

void text_event(const char *text)
{
    SDL_Event event = { .type = SDL_EVENT_TEXT_INPUT };
    event.text.windowID = SDL_GetWindowID(cf_app_get_window());
    event.text.text = text;
    assert(SDL_PushEvent(&event));
}

void tap(SDL_Scancode key)
{
    key_event(key, true);
    key_event(key, false);
}

/* The pointer to (x, y) in dp, as SDL reports it in window points. */
void mouse_move(const studio *s, float x, float y)
{
    SDL_Event move = { .type = SDL_EVENT_MOUSE_MOTION };
    move.motion.windowID = SDL_GetWindowID(cf_app_get_window());
    move.motion.x = x * s->prefs.ui_scale;
    move.motion.y = y * s->prefs.ui_scale;
    assert(SDL_PushEvent(&move));
}

/* Studio over the Vault scene with the Outliner and Properties beside the
   Scene, whatever layout was saved. */
studio_fixture side_open()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    s->focus_area = STUDIO_AREA_OUTLINER;
    frames(&f, 3);
    return f;
}

/* UI documents as tabs and in the Text Editor (test/editor/ui_documents.c). */
void test_ui_document_tabs();
/* Assets ▸ New and Settings ▸ Project (test/editor/studio_new_assets.c). */
void test_studio_new_assets();
/* Model authoring in Studio (test/editor/studio_models.c). */
void test_studio_models();
/* Workstream I: the Text Editor's code intelligence
   (test/editor/code_intelligence_studio.c). */
void test_code_intelligence_studio();

/* Preserve the aggregate's original order, including the complete Properties
   group (the older --studio-properties selector covers only its fields). */
static bool document_cases(const char *only, bool run)
{
    bool matched = false;
    STUDIO_CASE("graph", studio_documents_graph_checks);
    if (!run || only) {
        STUDIO_CASE("expression-geometry", studio_documents_expression_geometry_checks);
    }
    STUDIO_CASE("tabs", test_ui_document_tabs);
    STUDIO_CASE("new-assets", test_studio_new_assets);
    STUDIO_CASE("models", test_studio_models);
    STUDIO_CASE("code-intelligence", test_code_intelligence_studio);
    STUDIO_CASE("panels", studio_documents_panel_checks);
    STUDIO_CASE("shell", studio_documents_shell_checks);
    STUDIO_CASE("assets", studio_documents_assets_checks);
    STUDIO_CASE("outliner", studio_documents_outliner_checks);
    STUDIO_CASE("properties", studio_documents_properties_checks);
    STUDIO_CASE("lists", studio_documents_list_checks);
    STUDIO_CASE("output", studio_documents_output_checks);
    STUDIO_CASE("members", studio_documents_members_checks);
    STUDIO_CASE("scene", studio_documents_scene_checks);
    STUDIO_CASE("chrome", studio_documents_chrome_checks);
    STUDIO_CASE("inspector", studio_documents_inspector_checks);
    STUDIO_CASE("designer", studio_documents_designer_checks);
    STUDIO_CASE("palette", studio_documents_palette_checks);
    STUDIO_CASE("graph-chrome", studio_documents_graph_chrome_checks);
    return matched;
}

static int studio_documents_app(const char *program, const char *only, const char *shell_case)
{
    studio_test_sample setup = studio_test_begin("app-setup");
    CF_Result result = cf_make_app(
        "Studio documents",
        0,
        0,
        0,
        1280,
        800,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "Studio documents app: %s\n", result.details);
        return 1;
    }
    app_scene_renderer_initialize();
    app_input_initialize();
    studio_test_end("app-setup", setup);
    if (shell_case) {
        studio_shell_cases(shell_case, true);
    } else if (only && !strcmp(only, "property-fields")) {
        studio_test_sample before = studio_test_begin(only);
        studio_documents_property_field_checks();
        studio_test_end(only, before);
    } else {
        document_cases(only, true);
    }
    studio_test_sample teardown = studio_test_begin("app-teardown");
    app_input_shutdown();
    cf_destroy_app();
    studio_test_end("app-teardown", teardown);
    return 0;
}

int studio_documents_run(const char *program)
{
    return studio_documents_app(program, NULL, NULL);
}

int studio_properties_run(const char *program)
{
    return studio_documents_app(program, "property-fields", NULL);
}

int studio_models_run(const char *program)
{
    return studio_documents_app(program, "models", NULL);
}

/* Lists and drag (workstream F4) alone. */
int studio_lists_run(const char *program)
{
    return studio_documents_app(program, "lists", NULL);
}

/* The Outliner (workstream P2) alone. */
int studio_outliner_run(const char *program)
{
    return studio_documents_app(program, "outliner", NULL);
}

/* Output (workstream P4) alone. */
int studio_output_run(const char *program)
{
    return studio_documents_app(program, "output", NULL);
}

/* Members (workstream P5) alone. */
int studio_members_run(const char *program)
{
    return studio_documents_app(program, "members", NULL);
}

/* The shell (workstream C3) alone. */
int studio_shell_run(const char *program)
{
    return studio_documents_app(program, "shell", NULL);
}

/* The Scene area (workstream C1) alone. */
int studio_scene_run(const char *program)
{
    return studio_documents_app(program, "scene", NULL);
}

/* The editor chrome (workstream C2) alone. */
int studio_chrome_run(const char *program)
{
    return studio_documents_app(program, "chrome", NULL);
}

/* The authored panel checks alone. */
int studio_panels_run(const char *program)
{
    return studio_documents_app(program, "panels", NULL);
}

/* The UI Inspector (workstream I1) alone. */
int studio_inspector_run(const char *program)
{
    return studio_documents_app(program, "inspector", NULL);
}

/* The visual designer (workstream D1) alone. */
int studio_designer_run(const char *program)
{
    return studio_documents_app(program, "designer", NULL);
}

/* The palette, pickers and refactor popover (workstream C4) alone. */
int studio_palette_run(const char *program)
{
    return studio_documents_app(program, "palette", NULL);
}

/* The graph's menus and Properties' Node and Member tabs (workstream G1)
   alone. */
int studio_graph_chrome_run(const char *program)
{
    return studio_documents_app(program, "graph-chrome", NULL);
}

int studio_documents_validate(const char *only)
{
    if (!document_cases(only, false)) {
        fprintf(stderr, "unknown Studio documents group: %s (use --studio-documents list)\n", only);
        return 2;
    }
    return 0;
}

int studio_shell_validate(const char *only)
{
    if (!studio_shell_cases(only, false)) {
        fprintf(stderr, "unknown Studio shell case: %s (use --studio-shell list)\n", only);
        return 2;
    }
    return 0;
}

int studio_documents_select(const char *program, const char *only)
{
    int result = studio_documents_validate(only);
    if (result || !strcmp(only, "list")) {
        return result;
    }
    return studio_documents_app(program, only, NULL);
}

int studio_shell_select(const char *program, const char *only)
{
    int result = studio_shell_validate(only);
    if (result || !strcmp(only, "list")) {
        return result;
    }
    return studio_documents_app(program, "shell", only);
}
