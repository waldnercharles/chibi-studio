#include "assets/call_hierarchy.h"
#include "authoring/engine_preview.h"
#include "authoring/preview_source.h"
#include "authoring/source_presentation.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/gameplay_input.h"
#include "editor/workspace/input.h"
#include "editor/workspace/scene_internal.h"
#include "editor/workspace/viewport.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "runtime/component_catalog.h"
#include "support/xalloc.h"
#include "test/asset_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

/* Runs after Cute initialization; no rendering or input injection is required
 * to prove that a native generation can reconstruct authoring state. */
typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    app_scene_authoring *scene;
} checkpoint_fixture;

/* Generic graph history fixtures use the retained declarative graph domain;
 * executable source scenarios below prepare and publish canonical modules. */
static behavior_asset *checkpoint_graph(const char *name)
{
    behavior_asset *asset = behavior_asset_new(name);
    asset->domain = BEHAVIOR_DOMAIN_SHADER_LIBRARY;
    return asset;
}

static checkpoint_fixture fixture_new()
{
    checkpoint_fixture fixture = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(
            ecs_world_register_component(fixture.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK
        );
    }
    fixture.live = game_preview_world_new(fixture.world);
    game_preview_definition *definition = game_preview_definition_new(
        "ReloadBehavior",
        checkpoint_graph("ReloadBehavior")
    );
    assert(definition && game_preview_add_definition(fixture.live, definition) == 0);
    fixture.graph = editor_new(checkpoint_graph("ReloadBehavior"));
    fixture.runtime = authoring_engine_new(fixture.world, fixture.live->runtime);
    fixture.scene = app_scene_authoring_new(
        fixture.world,
        fixture.live,
        definition,
        editor_behavior_asset(fixture.graph),
        fixture.graph,
        NULL,
        NULL,
        fixture.runtime
    );
    assert(fixture.scene);
    return fixture;
}

static void fixture_free(checkpoint_fixture *fixture)
{
    app_scene_authoring_free(fixture->scene);
    engine_free(fixture->runtime);
    game_preview_world_free(fixture->live);
    editor_free(fixture->graph);
    ecs_world_free(fixture->world);
}

static checkpoint_fixture roundtrip(checkpoint_fixture *source)
{
    char *scene = app_scene_authoring_checkpoint(source->scene);
    char *graph = editor_checkpoint(source->graph);
    assert(scene && graph);
    yyjson_doc *wire = yyjson_read(scene, strlen(scene), 0);
    assert(wire && yyjson_is_obj(yyjson_obj_get(yyjson_doc_get_root(wire), "scene")));
    yyjson_doc_free(wire);
    bool full_catalog = game_preview_definition_count(source->live) > 1;
    char *source_root = game_preview_sources_working(source->live)
                          ? xstrdup(source->scene->content_root)
                          : NULL;
    behavior_backend source_backend = behavior_default_backend();
    const authoring_sources *applied_sources = game_preview_sources_applied(
        source->live
    );
    if (applied_sources && authoring_sources_count(applied_sources)) {
        source_backend = execution_module_backend(
            authoring_sources_at(applied_sources, 0)->module
        );
    }
    fixture_free(source);
    checkpoint_fixture candidate = fixture_new();
    if (source_root) {
        char error[256];
        assert(app_scene_authoring_set_content_root(candidate.scene, source_root) == 0);
        game_preview_sources_prepare(candidate.live, source_root, source_backend, error, sizeof error);
        assert(game_preview_sources_working(candidate.live));
        free(source_root);
    } else if (full_catalog) {
        assert(app_scene_authoring_set_content_root(candidate.scene, "test/fixtures/content") == 0);
        assert(app_scene_authoring_load_content(candidate.scene) == 0);
    }
    assert(app_scene_authoring_restore_checkpoint(candidate.scene, scene) == 0);
    assert(editor_restore_checkpoint(candidate.graph, graph));
    app_scene_authoring_set_editor_working(
        candidate.scene,
        editor_behavior_asset(candidate.graph)
    );
    free(scene);
    free(graph);
    return candidate;
}

static void unsaved_library_signatures_checkpoint()
{
    const char *applied[] = {
        "library CheckpointCaller { pure function Forward() -> (Result: int) { "
        "value selected = CheckpointLibrary.Old(); result Result: int = "
        "selected.Result; } }",
        "library CheckpointLibrary { pure function Old() -> (Result: int) { "
        "result Result: int = 7; } }",
    };
    const char *working[] = {
        "library CheckpointCaller { pure function Forward() -> (Result: int) { "
        "value selected = CheckpointLibrary.New(); result Result: int = "
        "selected.Result; } }",
        "library CheckpointLibrary { pure function New() -> (Result: int) { "
        "result Result: int = 9; } }",
    };
    const char *root = "test/fixtures/language/content";
    const char *paths[] = {
        "test/fixtures/language/content/libraries/CheckpointCaller.chibi",
        "test/fixtures/language/content/libraries/CheckpointLibrary.chibi"
    };
    checkpoint_fixture f = fixture_new();
    char error[256];
    assert(!app_scene_authoring_set_content_root(f.scene, root));
    assert(!game_preview_sources_prepare(f.live, root, BEHAVIOR_BYTECODE, error, sizeof error));
    source_catalog_edit edits[2];
    for (int i = 0; i < 2; i++) {
        edits[i] = (source_catalog_edit){ .path = paths[i], .text = applied[i] };
    }
    assert(!game_preview_sources_replace(f.live, edits, 2, error, sizeof error));
    assert(!game_preview_sources_publish(f.live, NULL, error, sizeof error));
    for (int i = 0; i < 2; i++) {
        edits[i].text = working[i];
    }
    assert(!game_preview_sources_replace(f.live, edits, 2, error, sizeof error));
    for (int i = 0; i < 2; i++) {
        const authoring_source *before = game_preview_source_find(f.live, paths[i], true);
        const authoring_source *after = game_preview_source_find(f.live, paths[i], false);
        assert(before && after);
        authoring_source_presentation *a = authoring_source_presentation_new(
            before->analysis,
            authoring_sources_catalog(game_preview_sources_applied(f.live)),
            NULL
        );
        authoring_source_presentation *w = authoring_source_presentation_new(
            after->analysis,
            authoring_sources_catalog(game_preview_sources_working(f.live)),
            NULL
        );
        game_preview_definition
            *definition = game_preview_projection_new(before->analysis->name, a);
        assert(definition && !game_preview_add_projection(f.live, definition));
        assert(!game_preview_projection_set(definition, w, false));
        game_preview_definition_set_path(definition, paths[i]);
        definition->dirty = definition->unsaved = true;
        authoring_source_presentation_free(a);
        authoring_source_presentation_free(w);
    }
    f = roundtrip(&f);
    const authoring_source *before = game_preview_source_find(f.live, paths[1], true);
    const authoring_source *after = game_preview_source_find(f.live, paths[1], false);
    game_preview_definition *library = game_preview_find_definition(f.live, "CheckpointLibrary");
    game_preview_definition *caller = game_preview_find_definition(f.live, "CheckpointCaller");
    assert(
        library && caller && library->dirty && library->unsaved &&
        caller->dirty && caller->unsaved
    );
    assert(
        before && after && strstr(before->syntax->text, "Old()") &&
        !strstr(before->syntax->text, "New()")
    );
    assert(strstr(after->syntax->text, "New()") && !strstr(after->syntax->text, "Old()"));
    fixture_free(&f);
    puts("scene checkpoint PASS: forward library calls retain independent applied and unsaved signatures");
}

static char *checkpoint_declaration(const char *text, const char *declaration)
{
    const char *end = strrchr(text, '}');
    assert(end);
    size_t prefix = (size_t)(end - text), added = strlen(declaration);
    char *result = xmalloc(strlen(text) + added + 1);
    memcpy(result, text, prefix);
    memcpy(result + prefix, declaration, added);
    strcpy(result + prefix + added, end);
    return result;
}

static uint64_t checkpoint_state(const authoring_source *source, const char *name)
{
    for (int i = 0; i < source->analysis->symbol_count; i++) {
        const script_symbol *symbol = &source->analysis->symbols[i];
        if (symbol->kind == SCRIPT_STATE && !strcmp(symbol->name, name)) {
            return symbol->id;
        }
    }
    assert(false);
    return 0;
}

static execution_program *checkpoint_input_program(checkpoint_fixture *f)
{
    return game_source_program(
        game_prototype_preview_live(f->scene->prototype_preview)->runtime->sources,
        "InputBehavior"
    );
}

static int checkpoint_input_count(checkpoint_fixture *f, uint64_t state)
{
    execution_program *program = checkpoint_input_program(f);
    assert(program);
    ecs_entity owner = game_prototype_preview_entity(f->scene->prototype_preview);
    execution_instance *instance = execution_program_instance(program, ecs_entity_to_u64(owner));
    const pin_value *value = instance ? execution_instance_state(instance, state)
                                      : NULL;
    assert(value && value->type == PIN_I32);
    return value->i;
}

static void checkpoint_input_edit(checkpoint_fixture *f, const char *path, const char *text)
{
    char error[256];
    assert(!game_preview_source_edit(f->live, path, text, error, sizeof error));
    const authoring_source *source = game_preview_source_find(f->live, path, false);
    assert(source && source->valid);
    game_preview_definition *definition = game_preview_find_definition(f->live, "InputBehavior");
    assert(definition && definition->working_presentation);
    authoring_source_presentation *presentation = authoring_source_presentation_new(
        source->analysis,
        authoring_sources_catalog(game_preview_sources_working(f->live)),
        definition->working_presentation
    );
    assert(presentation && !game_preview_projection_set(definition, presentation, false));
    authoring_source_presentation_free(presentation);
    definition->dirty = definition->unsaved = true;
}

static void shader_scene_load()
{
    const char *root = "test/fixtures/content";
    const char *path = "test/fixtures/content/behaviors/InputBehavior.chibi";
    checkpoint_fixture f = fixture_new();
    assert(!app_scene_authoring_set_content_root(f.scene, root));
    char error[256];
    game_preview_sources_prepare(f.live, root, behavior_default_backend(), error, sizeof error);
    game_preview_world *preview = game_prototype_preview_live(f.scene->prototype_preview);
    game_preview_sources_prepare(preview, root, behavior_default_backend(), error, sizeof error);
    assert(game_preview_sources_working(preview));
    assert(game_preview_sources_working(f.live));
    char *shared[] = { read_scene_file("content/behaviors/HealthBehavior.chibi"),
                       read_scene_file("content/behaviors/PlayerContactBehavior.chibi") };
    char *input = read_scene_file(path);
    assert(shared[0] && shared[1] && input);
    char *baseline = checkpoint_declaration(input, "    state checkpoint_marker: int = 0;\n");
    free(input);
    char *handler = strstr(baseline, "on Input");
    char *body = handler ? strchr(handler, '{') : NULL;
    assert(body);
    const char *increment = "\n        checkpoint_marker += 1;";
    size_t prefix = (size_t)(body + 1 - baseline), added = strlen(increment);
    input = xmalloc(strlen(baseline) + added + 1);
    memcpy(input, baseline, prefix);
    memcpy(input + prefix, increment, added);
    strcpy(input + prefix + added, baseline + prefix);
    free(baseline);
    baseline = input;
    source_catalog_edit edits[] = {
        { .path = "test/fixtures/content/behaviors/HealthBehavior.chibi",
          .text = shared[0] },
        { .path = "test/fixtures/content/behaviors/PlayerContactBehavior.chibi",
          .text = shared[1] },
        { .path = path, .text = baseline }
    };
    assert(!scene_publish_source_edits(f.scene, edits, 3, error, sizeof error));
    free(shared[0]);
    free(shared[1]);
    assert(!app_scene_authoring_load_content(f.scene));
    assert(!app_scene_authoring_load(f.scene, "test/fixtures/content/scenes/ShaderDemo.kdl"));
    assert(acount(f.scene->actors) == 3);
    char *pending = checkpoint_declaration(baseline, "    state checkpoint_pending: int = 1;\n");
    checkpoint_input_edit(&f, path, pending);
    f = roundtrip(&f);
    game_preview_definition *definition = game_preview_find_definition(f.live, "InputBehavior");
    assert(definition && definition->dirty && definition->unsaved);
    const authoring_source *working = game_preview_source_find(f.live, path, false);
    const authoring_source *applied = game_preview_source_find(f.live, path, true);
    assert(working && applied && strstr(working->syntax->text, "checkpoint_pending"));
    assert(!strstr(applied->syntax->text, "checkpoint_pending"));
    free(pending);
    checkpoint_input_edit(&f, path, baseline);
    assert(!scene_publish_sources(f.scene, NULL, error, sizeof error));
    assert(acount(f.scene->actors) == 3);
    assert(!app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY));
    assert(!app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP));
    assert(!app_scene_authoring_open_behavior(f.scene, "InputBehavior"));
    assert(editor_presentation(f.graph));
    assert(!strcmp(authoring_source_presentation_text(editor_presentation(f.graph)), baseline));
    assert(!app_scene_authoring_preview_prototype(f.scene, "Player"));
    applied = game_preview_source_find(f.live, path, true);
    uint64_t marker = checkpoint_state(applied, "checkpoint_marker");
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    int idle = checkpoint_input_count(&f, marker);
    assert(app_scene_authoring_move(f.scene, 0, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 0.00) >= 0);
    assert(checkpoint_input_count(&f, marker) == idle);
    assert(app_scene_authoring_move(f.scene, 1, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, ENGINE_FIXED_SECONDS) >= 0);
    int first = checkpoint_input_count(&f, marker);
    assert(first > idle);
    ecs_world *preview_world = game_prototype_preview_world(f.scene->prototype_preview);
    ecs_entity player = game_prototype_preview_entity(f.scene->prototype_preview);
    const GameVelocity *velocity = GAME_GameVelocity_get_const(preview_world, player);
    assert(velocity && velocity->x > 0);
    assert(app_scene_authoring_move(f.scene, 1, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 2 * ENGINE_FIXED_SECONDS) >= 0);
    int held = checkpoint_input_count(&f, marker);
    assert(held > first);
    assert(app_scene_authoring_move(f.scene, 0, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 3 * ENGINE_FIXED_SECONDS) >= 0);
    int released = checkpoint_input_count(&f, marker);
    assert(released > held);
    assert(GAME_GameVelocity_get_const(preview_world, player)->x == 0);
    assert(app_scene_authoring_move(f.scene, 0, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 4 * ENGINE_FIXED_SECONDS) >= 0);
    assert(checkpoint_input_count(&f, marker) == released);
    assert(app_scene_authoring_move(f.scene, 1, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 5 * ENGINE_FIXED_SECONDS) >= 0);
    int refocused = checkpoint_input_count(&f, marker);
    engine_viewport *unfocused = engine_viewport_new(32, 32);
    app_gameplay_input(f.scene, f.runtime, unfocused);
    assert(app_scene_authoring_frame(f.scene, 6 * ENGINE_FIXED_SECONDS) >= 0);
    int stopped = checkpoint_input_count(&f, marker);
    assert(stopped > refocused);
    assert(GAME_GameVelocity_get_const(preview_world, player)->x == 0);
    app_gameplay_input(f.scene, f.runtime, unfocused);
    assert(app_scene_authoring_frame(f.scene, 7 * ENGINE_FIXED_SECONDS) >= 0);
    assert(checkpoint_input_count(&f, marker) == stopped);
    engine_viewport_free(unfocused);
    engine_set_focus(f.runtime, ENGINE_FOCUS_VIEWPORT);
    assert(app_scene_authoring_move(f.scene, 0, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 8 * ENGINE_FIXED_SECONDS) >= 0);
    assert(checkpoint_input_count(&f, marker) == stopped);
    assert(app_scene_authoring_move(f.scene, 1, 0) >= 0);
    assert(app_scene_authoring_frame(f.scene, 9 * ENGINE_FIXED_SECONDS) >= 0);
    assert(checkpoint_input_count(&f, marker) > stopped);

    assert(!app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_PREVIEW, APP_RUNTIME_PAUSE));
    execution_program *before = checkpoint_input_program(&f);
    int retained = checkpoint_input_count(&f, marker);
    char *changed = xstrdup(baseline);
    char *initial = strstr(changed, "checkpoint_marker: int = 0;");
    assert(initial);
    initial[strlen("checkpoint_marker: int = ")] = '9';
    checkpoint_input_edit(&f, path, changed);
    working = game_preview_source_find(f.live, path, false);
    authoring_source_presentation *view = authoring_source_presentation_new(
        working->analysis,
        authoring_sources_catalog(game_preview_sources_working(f.live)),
        editor_presentation(f.graph)
    );
    assert(view && !editor_source_presentation(f.graph, view));
    authoring_source_presentation_free(view);
    app_scene_authoring_set_editor_working(f.scene, editor_behavior_asset(f.graph));
    preview_behavior_edits(f.scene);
    assert(checkpoint_input_program(&f) == before);
    assert(checkpoint_input_count(&f, marker) == retained);
    applied = game_preview_source_find(f.live, path, true);
    assert(applied && !strcmp(applied->syntax->text, changed));
    assert(!strcmp(authoring_source_presentation_text(editor_presentation(f.graph)), changed));
    free(changed);
    free(baseline);
    fixture_free(&f);
}

static void scene_tabs_retain_authored_state()
{
    checkpoint_fixture f = fixture_new();
    assert(
        app_scene_authoring_register_prototype(
            f.scene,
            "name \"ReloadPrototype\"\n"
            "components {\n"
            "    GameTransform x=1 y=2\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(app_scene_authoring_add_instance(f.scene, "ReloadPrototype") == 0);
    const char *first_path = "/tmp/chibi-39-scene-a.kdl";
    const char *second_path = "/tmp/chibi-39-scene-b.kdl";
    char *first_json = scene_authoring_scene_text(f.scene);
    assert(first_json && write_text_file_atomic(first_path, first_json) == 0);
    free(first_json);
    assert(app_scene_authoring_load(f.scene, first_path) == 0);
    scene_tabs_document_selected(f.scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    int first = f.scene->active_tab;
    /* Loading into the untitled tab names it after the file. */
    assert(!strcmp(f.scene->tabs[first].label, "chibi-39-scene-a"));
    f.scene->camera = (scene_camera){ .view = { 1, 2, 300, 200 }, .saved = true };
    assert(f.scene->selected == 0);
    char *second_json = scene_authoring_scene_text(f.scene);
    assert(second_json && write_text_file_atomic(second_path, second_json) == 0);
    free(second_json);
    assert(scene_tabs_open_scene(f.scene, second_path) == 0);
    int second = f.scene->active_tab;
    assert(first != second && f.scene->active_tab == second);
    GameTransform
        before = *GAME_GameTransform_get_const(f.world, f.scene->actors[0].entity);
    GameTransform after = { 11, 12 };
    write_position(f.scene, f.scene->actors[0].entity, after);
    assert(scene_history_push(f.scene->history, f.scene->actors[0].entity, before, after));
    f.scene->scene_dirty = true;
    f.scene->camera.view.center_x = 42;
    scene_tabs_sync_current(f.scene);
    assert(scene_tabs_activate(f.scene, first) == 0);
    assert(GAME_GameTransform_get_const(f.world, f.scene->actors[0].entity)->x == 1);
    assert(f.scene->camera.view.center_x == 1 && f.scene->selected == 0);
    assert(scene_tabs_activate(f.scene, second) == 0);
    assert(GAME_GameTransform_get_const(f.world, f.scene->actors[0].entity)->x == 11);
    assert(f.scene->camera.view.center_x == 42 && f.scene->selected == 0);
    assert(replay_scene_edit(f.scene, false) == 0);
    assert(GAME_GameTransform_get_const(f.world, f.scene->actors[0].entity)->x == 1);
    assert(replay_scene_edit(f.scene, true) == 0);
    assert(GAME_GameTransform_get_const(f.world, f.scene->actors[0].entity)->x == 11);
    remove(first_path);
    remove(second_path);
    fixture_free(&f);
}

static void scene_tabs_save_as_keeps_identity()
{
    checkpoint_fixture f = fixture_new();
    assert(
        app_scene_authoring_register_prototype(
            f.scene,
            "name \"SaveAsPrototype\"\n"
            "components {\n"
            "    GameTransform x=1 y=2\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(app_scene_authoring_add_instance(f.scene, "SaveAsPrototype") == 0);
    const char *first_path = "/tmp/chibi-39-save-as-a.kdl";
    const char *second_path = "/tmp/chibi-39-save-as-b.kdl";
    char *json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(first_path, json) == 0);
    free(json);
    assert(app_scene_authoring_load(f.scene, first_path) == 0);
    int scene_tab = f.scene->active_tab;
    char *identity = xstrdup(f.scene->tabs[scene_tab].key);
    assert(app_scene_authoring_save(f.scene, second_path) == 0);
    assert(acount(f.scene->tabs) == 1 && f.scene->scene_path);
    assert(!strcmp(f.scene->scene_path, second_path));
    assert(!strcmp(f.scene->tabs[scene_tab].key, identity));
    assert(app_scene_authoring_open_behavior(f.scene, "ReloadBehavior") == 0);
    assert(scene_tabs_activate(f.scene, scene_tab) == 0);
    assert(f.scene->document.kind == APP_EDITOR_DOCUMENT_SCENE);
    assert(!strcmp(f.scene->scene_path, second_path));
    assert(app_scene_authoring_open_behavior(f.scene, "ReloadBehavior") == 0);
    assert(scene_tabs_open_scene(f.scene, second_path) == 0);
    int scene_tabs = 0;
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        scene_tabs += f.scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE;
    }
    assert(scene_tabs == 1);
    free(identity);
    remove(first_path);
    remove(second_path);
    fixture_free(&f);
}

static void scene_tabs_failed_restore_rolls_back()
{
    checkpoint_fixture f = fixture_new();
    assert(
        app_scene_authoring_register_prototype(
            f.scene,
            "name \"RollbackPrototype\"\n"
            "components {\n"
            "    GameTransform x=1 y=2\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(app_scene_authoring_add_instance(f.scene, "RollbackPrototype") == 0);
    const char *first_path = "/tmp/chibi-39-rollback-a.kdl";
    const char *second_path = "/tmp/chibi-39-rollback-b.kdl";
    char *json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(first_path, json) == 0);
    free(json);
    assert(app_scene_authoring_load(f.scene, first_path) == 0);
    json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(second_path, json) == 0);
    free(json);
    assert(scene_tabs_open_scene(f.scene, second_path) == 0);
    int second = f.scene->active_tab;
    int first = -1;
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        if (f.scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE && i != second) {
            first = i;
        }
    }
    assert(first >= 0 && scene_tabs_activate(f.scene, first) == 0);
    write_position(f.scene, f.scene->actors[0].entity, (GameTransform){ 11, 12 });
    f.scene->scene_dirty = true;
    scene_tabs_sync_current(f.scene);
    av_value_free(f.scene->tabs[second].scene_history);
    f.scene->tabs[second].scene_history = test_read_asset_value("");
    assert(scene_tabs_activate(f.scene, second) != 0);
    assert(f.scene->active_tab == first && f.scene->scene_dirty);
    assert(GAME_GameTransform_get_const(f.world, f.scene->actors[0].entity)->x == 11);
    remove(first_path);
    remove(second_path);
    fixture_free(&f);
}

static void scene_tabs_discard_reverts_shared_assets()
{
    checkpoint_fixture f = fixture_new();
    assert(
        app_scene_authoring_register_prototype(
            f.scene,
            "name \"DiscardPrototype\"\n"
            "components {\n"
            "    GameTransform x=1 y=2\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(app_scene_authoring_open_prototype(f.scene, "DiscardPrototype") == 0);
    assert(
        app_scene_authoring_set_prototype_field(
            f.scene,
            "DiscardPrototype",
            "GameTransform",
            "x",
            (pin_value){ .type = PIN_F32, .f = 7 }
        ) == 0
    );
    int prototype_tab = f.scene->active_tab;
    assert(f.scene->prototypes[0].dirty);
    assert(scene_tabs_discard(f.scene, prototype_tab) == 0);
    pin_value value;
    assert(
        app_scene_authoring_read_prototype_field(f.scene, "DiscardPrototype", "GameTransform", "x", &value) ==
            0 &&
        value.f == 1 && !f.scene->prototypes[0].dirty
    );

    assert(app_scene_authoring_open_behavior(f.scene, "ReloadBehavior") == 0);
    int behavior_nodes = acount(f.scene->definition->working->graph->nodes);
    assert(editor_spawn(f.graph, registry_node_by_name("Integer"), 12, 34));
    app_scene_authoring_set_editor_working(f.scene, editor_behavior_asset(f.graph));
    assert(sync_behavior_working(f.scene) == 0);
    int behavior_tab = f.scene->active_tab;
    assert(f.scene->definition->dirty);
    assert(scene_tabs_discard(f.scene, behavior_tab) == 0);
    assert(!f.scene->definition->dirty && !f.scene->definition->unsaved);
    assert(acount(f.scene->definition->working->graph->nodes) == behavior_nodes);
    /* The closed behavior leaves the Behavior area. */
    assert(!f.scene->graph_document_id);
    fixture_free(&f);
}

/* The canvas arranges an opened graph's automatic nodes. Opening another
   behavior keeps both tabs saved. */
static void opening_behaviors_keeps_them_saved()
{
    checkpoint_fixture f = fixture_new();
    behavior_asset *asset = checkpoint_graph("ArrangedBehavior");
    node *automatic = graph_add_node(asset->graph, registry_node_by_name("Integer"));
    automatic->placed = false;
    uint64_t id = automatic->id;
    game_preview_definition *arranged = game_preview_definition_new("ArrangedBehavior", asset);
    assert(arranged && game_preview_add_definition(f.live, arranged) == 0);
    assert(app_scene_authoring_open_behavior(f.scene, "ArrangedBehavior") == 0);
    graph_node_by_id(editor_active_graph(f.graph), id)->x += 300;
    assert(app_scene_authoring_open_behavior(f.scene, "ReloadBehavior") == 0);
    assert(sync_behavior_working(f.scene) == 0);
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        assert(!scene_tabs_dirty(f.scene, &f.scene->tabs[i]));
    }
    assert(!arranged->unsaved && !arranged->dirty);
    fixture_free(&f);
}

/* The graph editor borrows each definition's working asset. Opening a
   behavior copies the edited one it leaves into its definition, freeing the
   working asset the frame's catalog held; the previews read next must not see
   it. */
static void opening_behaviors_refreshes_asset_catalog()
{
    checkpoint_fixture f = fixture_new();
    game_preview_definition *left = game_preview_definition_new("CatalogLeft", checkpoint_graph("CatalogLeft"));
    game_preview_definition *right = game_preview_definition_new(
        "CatalogRight",
        checkpoint_graph("CatalogRight")
    );
    assert(left && game_preview_add_definition(f.live, left) == 0);
    assert(right && game_preview_add_definition(f.live, right) == 0);
    assert(app_scene_authoring_open_behavior(f.scene, "CatalogLeft") == 0);
    assert(editor_spawn(f.graph, registry_node_by_name("Integer"), 12, 34));
    app_scene_authoring_update(f.scene, false);
    assert(app_scene_authoring_open_behavior(f.scene, "CatalogRight") == 0);
    assert(acount(left->working->graph->nodes) == 1);
    assert(acount(f.graph->asset_catalog) == game_preview_definition_count(f.live));
    for (int i = 0; i < acount(f.graph->asset_catalog); i++) {
        assert(
            f.graph->asset_catalog[i] == game_preview_definition_at(f.live, i)->working
        );
    }
    ed_refresh_values(f.graph);
    fixture_free(&f);
}

static void scene_tabs_close_underlying_scene_keeps_context()
{
    checkpoint_fixture f = fixture_new();
    const char *first_path = "/tmp/chibi-39-close-scene-a.kdl";
    const char *second_path = "/tmp/chibi-39-close-scene-c.kdl";
    char *json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(first_path, json) == 0);
    free(json);
    assert(app_scene_authoring_load(f.scene, first_path) == 0);
    json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(second_path, json) == 0);
    free(json);
    assert(scene_tabs_open_scene(f.scene, second_path) == 0);
    int first = -1, second = f.scene->active_tab;
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        if (f.scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE && i != second) {
            first = i;
        }
    }
    assert(first >= 0 && scene_tabs_activate(f.scene, first) == 0);
    assert(app_scene_authoring_open_behavior(f.scene, "ReloadBehavior") == 0);
    assert(scene_tabs_discard(f.scene, first) == 0);
    assert(f.scene->document.kind == APP_EDITOR_DOCUMENT_SCENE);
    assert(!strcmp(f.scene->scene_path, second_path));
    assert(f.scene->tabs[f.scene->active_tab].kind == APP_EDITOR_DOCUMENT_SCENE);
    assert(!strcmp(f.scene->tabs[f.scene->active_tab].path, second_path));
    assert(
        scene_tabs_current_scene_key(f.scene) ==
        f.scene->tabs[f.scene->active_tab].key
    );
    remove(first_path);
    remove(second_path);
    fixture_free(&f);
}

static void scene_tabs_reject_last_scene_close()
{
    checkpoint_fixture f = fixture_new();
    assert(scene_tabs_discard(f.scene, f.scene->active_tab) == -1);
    assert(acount(f.scene->tabs) == 1);
    assert(f.scene->tabs[0].kind == APP_EDITOR_DOCUMENT_SCENE);
    fixture_free(&f);
}

static void scene_tabs_failed_behavior_discard_is_atomic()
{
    checkpoint_fixture f = fixture_new();
    assert(app_scene_authoring_open_behavior(f.scene, "ReloadBehavior") == 0);
    assert(editor_spawn(f.graph, registry_node_by_name("Integer"), 12, 34));
    app_scene_authoring_set_editor_working(f.scene, editor_behavior_asset(f.graph));
    assert(sync_behavior_working(f.scene) == 0);
    int nodes = acount(f.scene->definition->working->graph->nodes);
    behavior_asset *working = f.scene->definition->working;
    const behavior_asset *editor_asset = editor_behavior_asset(f.graph);
    scene_document_tab *tab = &f.scene->tabs[f.scene->active_tab];
    av_value_free(tab->asset_saved);
    tab->asset_saved = test_read_asset_value("");
    assert(scene_tabs_discard(f.scene, f.scene->active_tab) == -1);
    assert(f.scene->document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR);
    assert(f.scene->definition->working == working);
    assert(editor_behavior_asset(f.graph) == editor_asset);
    assert(acount(f.scene->definition->working->graph->nodes) == nodes);
    assert(f.scene->definition->dirty && f.scene->definition->unsaved);
    fixture_free(&f);
}

static void write_system_fixture(const char *path, const char *name)
{
    char text[256];
    snprintf(text, sizeof text, "system %s(position:GameTransform) {}", name);
    assert(!write_text_file_atomic(path, text));
}
static uint64_t system_query_type(const checkpoint_fixture *f)
{
    const authoring_source *source = game_preview_source_find(f->live, f->scene->system_path, false);
    assert(source && source->analysis && source->analysis->function_count);
    const script_function *system = &source->analysis->functions[0];
    assert(system->parameter_count == 1);
    return source->analysis->symbols[system->parameters[0]].type.nominal;
}
static void checkpoint_system_edit(checkpoint_fixture *f, const char *path, const char *text)
{
    char error[256];
    ed_snapshot_before(f->graph);
    assert(!game_preview_source_edit(f->live, path, text, error, sizeof error));
    const authoring_source *source = game_preview_source_find(f->live, path, false);
    assert(source && source->valid);
    authoring_source_presentation *view = authoring_source_presentation_new(
        source->analysis,
        authoring_sources_catalog(game_preview_sources_working(f->live)),
        editor_presentation(f->graph)
    );
    assert(view && !editor_source_presentation(f->graph, view));
    authoring_source_presentation_free(view);
    scene_tabs_sync_current(f->scene);
}

static void scene_tabs_retain_system_state()
{
    checkpoint_fixture f = fixture_new();
    const char *directory = "/tmp/chibi-39-source";
    const char *root = "/tmp/chibi-39-source/content";
    const char *folder = "/tmp/chibi-39-source/content/systems";
    const char
        *first_path = "/tmp/chibi-39-source/content/systems/SystemA.chibi";
    const char
        *second_path = "/tmp/chibi-39-source/content/systems/SystemB.chibi";
    assert(!mkdir(directory, 0755));
    assert(!mkdir(root, 0755));
    assert(!mkdir(folder, 0755));
    write_system_fixture(first_path, "SystemA");
    write_system_fixture(second_path, "SystemB");
    assert(!app_scene_authoring_set_content_root(f.scene, root));
    char error[256];
    assert(!game_preview_sources_prepare(f.live, root, BEHAVIOR_BYTECODE, error, sizeof error));
    assert(app_scene_authoring_open_system(f.scene, first_path) == 0);
    int first = f.scene->active_tab;
    assert(!scene_tabs_dirty(f.scene, &f.scene->tabs[first]));
    checkpoint_system_edit(&f, first_path, "system SystemA(position:GameVelocity) {}");
    int before_marker = acount(editor_active_graph(f.graph)->nodes);
    checkpoint_system_edit(&f, first_path, "system SystemA(position:GameVelocity) { state marker:int=1; }");
    editor_tick(f.graph, 0);
    assert(scene_tabs_dirty(f.scene, &f.scene->tabs[first]));
    int first_nodes = acount(editor_active_graph(f.graph)->nodes);
    assert(app_scene_authoring_open_system(f.scene, second_path) == 0);
    int second = f.scene->active_tab;
    assert(first != second);
    assert(scene_tabs_dirty(f.scene, &f.scene->tabs[first]));
    assert(!scene_tabs_dirty(f.scene, &f.scene->tabs[second]));
    checkpoint_system_edit(&f, second_path, "system SystemB(position:GameOrientation) { state marker:int=2; }");
    int second_nodes = acount(editor_active_graph(f.graph)->nodes);
    assert(scene_tabs_activate(f.scene, first) == 0);
    assert(!strcmp(f.scene->system_path, first_path));
    assert(system_query_type(&f) == script_catalog_id("type", "GameVelocity"));
    assert(acount(editor_active_graph(f.graph)->nodes) == first_nodes);
    assert(scene_tabs_dirty(f.scene, &f.scene->tabs[first]));
    const authoring_source *source = game_preview_source_find(f.live, first_path, false);
    assert(source && strstr(source->syntax->text, "GameVelocity"));
    editor_undo(f.graph);
    assert(acount(editor_active_graph(f.graph)->nodes) == before_marker);
    editor_redo(f.graph);
    assert(acount(editor_active_graph(f.graph)->nodes) == first_nodes);
    assert(scene_tabs_activate(f.scene, second) == 0);
    assert(!strcmp(f.scene->system_path, second_path));
    assert(system_query_type(&f) == script_catalog_id("type", "GameOrientation"));
    assert(acount(editor_active_graph(f.graph)->nodes) == second_nodes);
    assert(save_active_document(f.scene) == 0);
    assert(!scene_tabs_dirty(f.scene, &f.scene->tabs[second]));
    assert(scene_tabs_activate(f.scene, first) == 0);
    assert(scene_tabs_dirty(f.scene, &f.scene->tabs[first]));
    assert(!scene_tabs_dirty(f.scene, &f.scene->tabs[second]));
    assert(!unlink(first_path));
    assert(!unlink(second_path));
    char metadata[256];
    snprintf(metadata, sizeof metadata, "%s.meta", second_path);
    unlink(metadata);
    fixture_free(&f);
    assert(!rmdir(folder));
    assert(!rmdir(root));
    assert(!rmdir(directory));
}

static void checkpoint_restores_next_scene_key()
{
    checkpoint_fixture f = fixture_new();
    const char *first_path = "/tmp/chibi-39-key-a.kdl";
    const char *second_path = "/tmp/chibi-39-key-b.kdl";
    char *json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(first_path, json) == 0);
    free(json);
    assert(app_scene_authoring_load(f.scene, first_path) == 0);
    json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(second_path, json) == 0);
    free(json);
    assert(scene_tabs_open_scene(f.scene, second_path) == 0);
    int first = -1;
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        if (f.scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE &&
            f.scene->tabs[i].path && !strcmp(f.scene->tabs[i].path, first_path)) {
            first = i;
        }
    }
    assert(first >= 0);
    free(f.scene->tabs[first].key);
    f.scene->tabs[first].key = xstrdup("scene:scene-17");
    f.scene->next_scene_key = 2;
    f = roundtrip(&f);
    assert(f.scene->next_scene_key == 18);
    free(f.scene->scene_path);
    f.scene->scene_path = NULL;
    f.scene->tabs_transitioning = true;
    scene_tabs_document_selected(f.scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    f.scene->tabs_transitioning = false;
    assert(f.scene->next_scene_key == 19);
    assert(!strcmp(f.scene->tabs[f.scene->active_tab].key, "scene:scene-18"));
    remove(first_path);
    remove(second_path);
    fixture_free(&f);
}

static int indexed_asset(const reference_index *index, reference_asset_kind kind, const char *name)
{
    int asset = reference_index_find_name(index, kind, name);
    assert(asset >= 0);
    return asset;
}

static const reference *indexed_reference(const reference_index *index, int source, int target, reference_role role)
{
    for (int i = 0; i < reference_index_count(index); i++) {
        const reference *r = reference_index_at(index, i);
        if (r->source == source && reference_dependency(r) == target && r->role == role) {
            return r;
        }
    }
    return NULL;
}

static reference_symbol event_named(const char *name)
{
    return (reference_symbol){ .kind = REFERENCE_SYMBOL_EVENT, .asset = -1, .name = name };
}

/* A handler of the root event in asset, among its callees. */
static const call_item *handler_in(const call_item *items, int asset)
{
    for (int i = items[0].first; i < items[0].first + items[0].count; i++) {
        if (items[i].kind == CALL_HANDLER && items[i].asset == asset) {
            return &items[i];
        }
    }
    return NULL;
}

static void asset_references_follow_authored_edits()
{
    checkpoint_fixture f = fixture_new();
    assert(app_scene_authoring_set_content_root(f.scene, "content") == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    const reference_index *index = app_scene_authoring_asset_index(f.scene);
    int arena = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, "content/scenes/Arena3D.kdl", NULL);
    assert(arena >= 0);
    char *arena_path = realpath("content/scenes/Arena3D.kdl", NULL);
    assert(
        arena_path &&
        reference_index_find_asset(index, REFERENCE_ASSET_SCENE, arena_path, NULL) == arena
    );
    free(arena_path);
    int player = indexed_asset(index, REFERENCE_ASSET_PROTOTYPE, "ArenaPlayer");
    int material = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaPlayer");
    int health = indexed_asset(index, REFERENCE_ASSET_BEHAVIOR, "HealthBehavior");
    int damage = indexed_asset(index, REFERENCE_ASSET_EVENT, "Damage");
    assert(player != material);
    const reference *binding = indexed_reference(index, player, material, REFERENCE_BIND);
    assert(binding && !strcmp(binding->component, "GameShader.material"));
    assert(indexed_reference(index, player, health, REFERENCE_ATTACH));
    const reference *consumer = indexed_reference(index, health, damage, REFERENCE_HANDLE);
    assert(
        consumer && consumer->source_start >= 0 &&
        consumer->source_end > consumer->source_start
    );
    const authoring_source *health_source = game_preview_source_find(
        f.live,
        "content/behaviors/HealthBehavior.chibi",
        false
    );
    assert(
        health_source &&
        !strncmp(health_source->syntax->text + consumer->source_start, "Damage", 6)
    );
    int contact = indexed_asset(index, REFERENCE_ASSET_BEHAVIOR, "PlayerContactBehavior");
    const reference *emitter = indexed_reference(index, contact, damage, REFERENCE_SEND);
    assert(emitter && emitter->source_end > emitter->source_start);
    const authoring_source *contact_source = game_preview_source_find(
        f.live,
        "content/behaviors/PlayerContactBehavior.chibi",
        false
    );
    assert(
        contact_source &&
        !strncmp(contact_source->syntax->text + emitter->source_start, "\"Damage\"", 8)
    );
    call_item *calls = call_hierarchy(index, event_named("Damage"), true, 1);
    assert(
        handler_in(calls, health) &&
        handler_in(calls, health)->graph_id == consumer->graph_id
    );
    call_hierarchy_free(calls);

    /* Exact unsaved source edits update references before publication. */
    assert(app_scene_authoring_open_behavior(f.scene, "HealthBehavior") == 0);
    char *original = xstrdup(health_source->syntax->text);
    int body_end = (int)(strrchr(original, '}') - original);
    char *edited = xmalloc(strlen(original) + 96);
    char error[256];
    snprintf(edited, strlen(original) + 96, "%.*s on PendingReferenceEvent {} }", body_end, original);
    source_catalog_edit edits[] = {
        { .path = "content/events/PendingReferenceEvent.kdl", .text = "name \"PendingReferenceEvent\"\ndomain \"event\"\n(array)variables {}\n" },
        { .path = "content/events/UpdatedReferenceEvent.kdl", .text = "name \"UpdatedReferenceEvent\"\ndomain \"event\"\n(array)variables {}\n" },
        { .path = "content/behaviors/HealthBehavior.chibi", .text = edited },
    };
    assert(!game_preview_sources_edit(f.live, edits, 3, error, sizeof error));
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    health = indexed_asset(index, REFERENCE_ASSET_BEHAVIOR, "HealthBehavior");
    int pending = indexed_asset(index, REFERENCE_ASSET_EVENT, "PendingReferenceEvent");
    consumer = indexed_reference(index, health, pending, REFERENCE_HANDLE);
    assert(consumer && consumer->source_end > consumer->source_start);
    calls = call_hierarchy(index, event_named("PendingReferenceEvent"), true, 1);
    assert(
        handler_in(calls, health) &&
        handler_in(calls, health)->graph_id == consumer->graph_id
    );
    call_hierarchy_free(calls);

    snprintf(edited, strlen(original) + 96, "%.*s on UpdatedReferenceEvent {} }", body_end, original);
    assert(!game_preview_source_edit(f.live, edits[2].path, edited, error, sizeof error));
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    assert(!indexed_reference(index, health, pending, REFERENCE_HANDLE));
    int updated = indexed_asset(index, REFERENCE_ASSET_EVENT, "UpdatedReferenceEvent");
    consumer = indexed_reference(index, health, updated, REFERENCE_HANDLE);
    assert(consumer);
    assert(!game_preview_source_edit(f.live, edits[2].path, original, error, sizeof error));
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    assert(!indexed_reference(index, health, updated, REFERENCE_HANDLE));
    free(edited);
    free(original);

    assert(
        app_scene_authoring_set_prototype_field(
            f.scene,
            "ArenaPlayer",
            "GameShader",
            "material",
            (pin_value){ .type = PIN_STR, .str = "ArenaFloor" }
        ) == 0
    );
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    player = indexed_asset(index, REFERENCE_ASSET_PROTOTYPE, "ArenaPlayer");
    material = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaPlayer");
    int floor = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaFloor");
    assert(!indexed_reference(index, player, material, REFERENCE_BIND));
    assert(indexed_reference(index, player, floor, REFERENCE_BIND));

    assert(
        app_scene_authoring_register_prototype(
            f.scene,
            "name \"ReferencePrototype\"\n"
            "components {\n"
            "    (object)GameTransform\n"
            "    GameShader material=\"ArenaPlayer\"\n"
            "}\n",
            NULL
        ) == 0
    );
    scene_authoring_select_document(f.scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    assert(app_scene_authoring_add_instance(f.scene, "ReferencePrototype") == 0);
    const char *first_path = "/tmp/chibi-40-references-a.kdl";
    const char *second_path = "/tmp/chibi-40-references-b.kdl";
    assert(app_scene_authoring_save(f.scene, first_path) == 0);
    int first_tab = f.scene->active_tab;
    char *json = scene_authoring_scene_text(f.scene);
    assert(json && write_text_file_atomic(second_path, json) == 0);
    free(json);
    assert(scene_tabs_open_scene(f.scene, second_path) == 0);
    int second_tab = f.scene->active_tab;
    const ecs_component_desc *shader = &GAME_COMPONENT_GameShader;
    int field = -1;
    for (int i = 0; i < shader->type->field_count; i++) {
        if (!strcmp(shader->type->fields[i].name, "material")) {
            field = i;
        }
    }
    assert(field >= 0 && acount(f.scene->actors) == 1);
    assert(
        ecs_component_write_field(
            f.world,
            f.scene->actors[0].entity,
            shader->id,
            field,
            (pin_value){ .type = PIN_STR, .str = "ArenaFloor" }
        ) == ECS_OK
    );
    f.scene->scene_dirty = true;
    asset_index_invalidate(f.scene);
    assert(
        !strcmp(GAME_GameShader_get_const(f.world, f.scene->actors[0].entity)->material, "ArenaFloor")
    );
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    assert(
        !strcmp(GAME_GameShader_get_const(f.world, f.scene->actors[0].entity)->material, "ArenaFloor")
    );
    int second_scene = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, second_path, NULL);
    material = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaPlayer");
    floor = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaFloor");
    assert(second_scene >= 0);
    assert(!indexed_reference(index, second_scene, material, REFERENCE_BIND));
    binding = indexed_reference(index, second_scene, floor, REFERENCE_BIND);
    assert(binding && binding->object && binding->object[0]);

    assert(scene_tabs_activate(f.scene, first_tab) == 0);
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    int first_scene = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, first_path, NULL);
    second_scene = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, second_path, NULL);
    material = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaPlayer");
    floor = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaFloor");
    assert(first_scene >= 0 && second_scene >= 0 && first_scene != second_scene);
    assert(indexed_reference(index, first_scene, material, REFERENCE_BIND));
    assert(indexed_reference(index, second_scene, floor, REFERENCE_BIND));
    assert(!indexed_reference(index, second_scene, material, REFERENCE_BIND));
    assert(app_scene_authoring_open_asset(f.scene, second_scene) == 0);
    assert(f.scene->active_tab == second_tab && f.scene->scene_dirty);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    assert(
        ecs_component_write_field(
            f.world,
            f.scene->actors[0].entity,
            shader->id,
            field,
            (pin_value){ .type = PIN_STR, .str = "ArenaEnemy" }
        ) == ECS_OK
    );
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    second_scene = reference_index_find_asset(index, REFERENCE_ASSET_SCENE, second_path, NULL);
    floor = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaFloor");
    int enemy = indexed_asset(index, REFERENCE_ASSET_MATERIAL, "ArenaEnemy");
    assert(indexed_reference(index, second_scene, floor, REFERENCE_BIND));
    assert(!indexed_reference(index, second_scene, enemy, REFERENCE_BIND));
    assert(
        !strcmp(GAME_GameShader_get_const(f.world, f.scene->actors[0].entity)->material, "ArenaEnemy")
    );
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    assert(
        !strcmp(GAME_GameShader_get_const(f.world, f.scene->actors[0].entity)->material, "ArenaFloor")
    );
    remove(first_path);
    remove(second_path);
    fixture_free(&f);
    puts("asset references PASS: typed relationships, pending edits, component changes and retained scene overrides");
}

static void physics_settings_checkpoint()
{
    checkpoint_fixture f = fixture_new();
    game_physics_settings defaults = game_physics_configuration(f.live->runtime->physics);
    game_physics_settings settings = defaults;
    settings.workers = 8;
    settings.substeps = 2;
    settings.sleep = false;
    settings.continuous = false;
    settings.force_drive = true;
    settings.contact_hertz = 15;
    settings.contact_damping = 1;
    assert(app_scene_authoring_set_physics(f.scene, settings) == 0);
    assert(f.scene->scene_dirty);
    f = roundtrip(&f);
    assert(game_physics_settings_equal(
        settings,
        game_physics_configuration(f.live->runtime->physics)
    ));
    assert(replay_scene_edit(f.scene, false) == 0);
    assert(game_physics_settings_equal(
        defaults,
        game_physics_configuration(f.live->runtime->physics)
    ));
    f = roundtrip(&f);
    assert(replay_scene_edit(f.scene, true) == 0);
    assert(game_physics_settings_equal(
        settings,
        game_physics_configuration(f.live->runtime->physics)
    ));
    game_physics_settings invalid = settings;
    invalid.substeps = 0;
    assert(app_scene_authoring_set_physics(f.scene, invalid) < 0);
    assert(game_physics_settings_equal(
        settings,
        game_physics_configuration(f.live->runtime->physics)
    ));
    fixture_free(&f);
}

int test_scene_checkpoint()
{
    physics_settings_checkpoint();
    asset_references_follow_authored_edits();
    unsaved_library_signatures_checkpoint();
    shader_scene_load();
    scene_tabs_retain_authored_state();
    scene_tabs_save_as_keeps_identity();
    scene_tabs_failed_restore_rolls_back();
    scene_tabs_discard_reverts_shared_assets();
    opening_behaviors_keeps_them_saved();
    opening_behaviors_refreshes_asset_catalog();
    scene_tabs_close_underlying_scene_keeps_context();
    scene_tabs_reject_last_scene_close();
    scene_tabs_failed_behavior_discard_is_atomic();
    scene_tabs_retain_system_state();
    checkpoint_restores_next_scene_key();
    checkpoint_fixture fixture = fixture_new();
    assert(
        app_scene_authoring_register_prototype(
            fixture.scene,
            "name \"ReloadPrototype\"\n"
            "components {\n"
            "    GameTransform x=1 y=2\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(
        app_scene_authoring_set_prototype_field(
            fixture.scene,
            "ReloadPrototype",
            "GameTransform",
            "x",
            (pin_value){ .type = PIN_F32, .f = 7 }
        ) == 0
    );
    assert(app_scene_authoring_add_instance(fixture.scene, "ReloadPrototype") == 0);
    assert(app_scene_authoring_open_behavior(fixture.scene, "ReloadBehavior") == 0);
    assert(editor_spawn(fixture.graph, registry_node_by_name("Integer"), 12, 34));
    fixture.scene->camera = (scene_camera){
        .view = { 10, 20, 300, 200, 600, .7f, 1.1f, 50, .4f },
        .saved = true
    };
    fixture = roundtrip(&fixture);
    assert(fixture.scene->scene_dirty && acount(fixture.scene->actors) == 1);
    assert(
        acount(fixture.scene->prototypes) == 1 && fixture.scene->prototypes[0].dirty
    );
    assert(fixture.scene->camera.saved && fixture.scene->camera.view.center_x == 10);
    assert(fixture.scene->camera.view.focal_distance == 600);
    assert(fixture.scene->camera.view.yaw == .7f && fixture.scene->camera.view.pitch == 1.1f);
    assert(fixture.scene->camera.view.target_elevation == 50);
    assert(fixture.scene->camera.view.roll == .4f);
    assert(acount(editor_active_graph(fixture.graph)->nodes) == 1);
    assert(
        GAME_GameTransform_get_const(fixture.world, fixture.scene->actors[0].entity)
            ->x == 7
    );
    editor_undo(fixture.graph);
    assert(acount(editor_active_graph(fixture.graph)->nodes) == 0);
    assert(app_scene_authoring_open_prototype(fixture.scene, "ReloadPrototype") == 0);
    assert(prototype_undo(fixture.scene) == 0);
    pin_value value;
    assert(
        app_scene_authoring_read_prototype_field(fixture.scene, "ReloadPrototype", "GameTransform", "x", &value) ==
            0 &&
        value.f == 1
    );
    scene_authoring_select_document(fixture.scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    assert(replay_scene_edit(fixture.scene, false) == 0);
    assert(acount(fixture.scene->actors) == 0);
    assert(replay_scene_edit(fixture.scene, true) == 0);
    assert(acount(fixture.scene->actors) == 1);
    float authored_x = GAME_GameTransform_get_const(
                           fixture.world,
                           fixture.scene->actors[0].entity
    )
                           ->x;
    assert(
        app_scene_authoring_runtime_command(fixture.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0
    );
    GAME_GameTransform_get(fixture.world, fixture.scene->actors[0].entity)->x = 999;
    assert(
        app_scene_authoring_runtime_command(fixture.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PAUSE) == 0
    );
    fixture = roundtrip(&fixture);
    app_runtime_state state = app_scene_authoring_runtime_state(fixture.scene);
    assert(state.target == APP_RUNTIME_SCENE && !state.playing && fixture.scene->runtime_snapshot_valid);
    assert(
        GAME_GameTransform_get_const(fixture.world, fixture.scene->actors[0].entity)
            ->x == authored_x
    );
    assert(
        app_scene_authoring_runtime_command(fixture.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0
    );
    assert(
        GAME_GameTransform_get_const(fixture.world, fixture.scene->actors[0].entity)
            ->x == authored_x
    );
    fixture_free(&fixture);
    puts("scene checkpoint PASS: unsaved graph/prototype/scene, Undo/Redo, camera, paused restart and authored pre-Play pose");
    return 0;
}

void test_scene_instances();
void test_scene_disk();
void test_scene_refactor();
void test_scene_view_camera();

int scene_documents_run(const char *program)
{
    CF_Result result = cf_make_app(
        "Scene document retention",
        0,
        0,
        0,
        256,
        256,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "scene document app: %s\n", result.details ? result.details : "unknown error");
        return 1;
    }
    app_scene_renderer_initialize();
    app_input_initialize();
    int failed = test_scene_checkpoint();
    test_scene_view_camera();
    test_scene_instances();
    test_scene_disk();
    test_scene_refactor();
    app_input_shutdown();
    cf_destroy_app();
    return failed;
}
