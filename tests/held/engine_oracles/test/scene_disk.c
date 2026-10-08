#include "authoring/engine_preview.h"
#include "assets/code_source.h"
#include "editor/workspace/asset_index.h"
#include "editor/workspace/scene_internal.h"
#include "graph/document/serialize.h"
#include "graph/model/registry.h"
#include "graph/runtime/instance.h"
#include "runtime/component_catalog.h"
#include "support/array.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Another program edits a project the editor has open. */
typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    app_scene_authoring *scene;
    char root[64];
    double now;
} disk_fixture;

static const char
    DOT[] = "name \"Dot\"\ncomponents { GAME.GameTransform { x %s; }; }\n";
static const char PULSE[] = "behavior Pulse { %s on Tick() {} }\n";

static void path_to(const disk_fixture *f, const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", f->root, name);
}

static void write_asset(const disk_fixture *f, const char *name, const char *format, const char *value)
{
    char path[128];
    path_to(f, name, path, sizeof path);
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, format, value);
    fclose(file);
}

static disk_fixture fixture_new()
{
    disk_fixture f = { .world = ecs_world_new() };
    snprintf(f.root, sizeof f.root, "/tmp/chibi-disk-XXXXXX");
    assert(mkdtemp(f.root));
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    const char *folders[] = { "behaviors", "prototypes", "scenes" };
    for (int i = 0; i < 3; i++) {
        char folder[128];
        path_to(&f, folders[i], folder, sizeof folder);
        assert(mkdir(folder, 0755) == 0);
    }
    write_asset(&f, "prototypes/Dot.kdl", DOT, "1.0");
    write_asset(&f, "scenes/Main.kdl", "version 1\nname \"Main\"\n(array)entities { %s }\n", "- { name \"Dot_1\"; prototype \"Dot\"; }; - { name \"Dot_2\"; prototype \"Dot\"; };");
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Editing", behavior_asset_new("Editing"));
    assert(definition && game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Editing"));
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
    char scene_path[128];
    path_to(&f, "scenes/Main.kdl", scene_path, sizeof scene_path);
    assert(app_scene_authoring_set_content_root(f.scene, f.root) == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(app_scene_authoring_load(f.scene, scene_path) == 0);
    assert(!app_scene_authoring_sync_disk(f.scene, f.now));
    return f;
}

static void fixture_free(disk_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
    const char *files[] = { "prototypes/Dot.kdl", "behaviors/Pulse.chibi",
                            "scenes/Main.kdl",    "prototypes",
                            "behaviors",          "scenes" };
    for (int i = 0; i < 6; i++) {
        char path[128];
        path_to(f, files[i], path, sizeof path);
        remove(path);
    }
    rmdir(f->root);
}

/* The one change the next poll finds, a second later. */
static app_disk_result poll_disk(disk_fixture *f, const char *name)
{
    f->now += 2;
    app_disk_change *changes = app_scene_authoring_sync_disk(f->scene, f->now);
    app_disk_result result = acount(changes) == 1 ? changes[0].result : APP_DISK_NONE;
    assert(!changes || (acount(changes) == 1 && strstr(changes[0].path, name)));
    app_disk_changes_free(changes);
    return result;
}

static float dot_x(disk_fixture *f, int actor)
{
    return GAME_GameTransform_get_const(f->world, f->scene->actors[actor].entity)
        ->x;
}

static void set_dot_x(disk_fixture *f, float x)
{
    pin_value value = { .type = PIN_F32, .f = x };
    assert(
        app_scene_authoring_set_prototype_field(f->scene, "Dot", "GameTransform", "x", value) == 0
    );
}

static void prototypes_follow_disk_unless_edited()
{
    disk_fixture f = fixture_new();
    char path[128];
    path_to(&f, "prototypes/Dot.kdl", path, sizeof path);
    write_asset(&f, "prototypes/Dot.kdl", DOT, "5.0");
    assert(
        poll_disk(&f, "Dot.kdl") == APP_DISK_RELOADED && dot_x(&f, 0) == 5 &&
        dot_x(&f, 1) == 5
    );

    set_dot_x(&f, 7);
    assert(app_scene_authoring_save_prototype(f.scene, "Dot", path) == 0);
    assert(poll_disk(&f, "Dot.kdl") == APP_DISK_NONE && dot_x(&f, 0) == 7);

    set_dot_x(&f, 9);
    write_asset(&f, "prototypes/Dot.kdl", DOT, "3.0");
    assert(poll_disk(&f, "Dot.kdl") == APP_DISK_UNSAVED && dot_x(&f, 0) == 9);
    assert(
        app_scene_authoring_reload_path(f.scene, path) == APP_DISK_RELOADED &&
        dot_x(&f, 0) == 3
    );

    write_asset(&f, "prototypes/Dot.kdl", "%s", "{");
    assert(poll_disk(&f, "Dot.kdl") == APP_DISK_FAILED && dot_x(&f, 0) == 3);
    fixture_free(&f);
}

static void behaviors_appear_and_reload()
{
    disk_fixture f = fixture_new();
    write_asset(&f, "behaviors/Pulse.chibi", PULSE, "");
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    game_preview_definition *pulse = game_preview_find_definition(f.live, "Pulse");
    assert(pulse && acount(pulse->working->graph->nodes) == 1);
    write_asset(&f, "behaviors/Pulse.chibi", PULSE, "value number = integer(Value: 7);");
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    const graph *updated = game_preview_find_definition(f.live, "Pulse")->working->graph;
    assert(acount(updated->nodes) == 2);
    const node *number = NULL;
    for (int i = 0; i < acount(updated->nodes); i++) {
        if (!strcmp(updated->nodes[i]->desc->name, "Integer")) {
            number = updated->nodes[i];
        }
    }
    assert(number && number->input_pins[0].default_value.i == 7);
    fixture_free(&f);
}

static void imported_behavior_reload_preserves_runtime()
{
    disk_fixture f = fixture_new();
    behavior_asset *library = behavior_asset_load(
        "library Shared { pure function Seven() -> (Result: int) { "
        "result Result: int = 7; } }"
    );
    game_preview_definition *shared = game_preview_definition_new("Shared", library);
    assert(shared && !game_preview_add_definition(f.live, shared));
    assert(!game_prototype_preview_register_behavior(
        f.scene->prototype_preview,
        "Shared",
        shared->working
    ));
    behavior_asset *event = behavior_asset_new("Signal");
    event->domain = BEHAVIOR_DOMAIN_EVENT_DEFINITION;
    assert(behavior_asset_add_variable(
        event,
        "Amount",
        PIN_I32,
        "int",
        PIN_CONTAINER_SINGLE,
        &(pin_value){ .type = PIN_I32, .i = 3 },
        NULL
    ));
    game_preview_definition *signal = game_preview_definition_new("Signal", event);
    assert(signal && !game_preview_add_definition(f.live, signal));
    assert(!game_prototype_preview_register_behavior(
        f.scene->prototype_preview,
        "Signal",
        signal->working
    ));
    const char *source = "behavior Pulse { state count: int = 0; "
                         "on Tick() { count = Shared.Seven().Result; } "
                         "on Signal() { count += event.Amount; } }";
    write_asset(&f, "behaviors/Pulse.chibi", "%s", source);
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    ecs_entity entity = f.scene->actors[0].entity;
    assert(!game_behavior_attach(f.live->runtime->behaviors, entity, "Pulse"));
    flow_error error = { 0 };
    assert(
        game_behavior_dispatch(f.live->runtime->behaviors, entity, "Tick", (flow_inputs){ 0 }, &error) == 1
    );
    behavior_instance *instance = game_behavior_instance(f.live->runtime->behaviors, entity, "Pulse");
    assert(instance && instance->execution.variables[0].i == 7);
    pin_value amount = { .type = PIN_I32, .i = 3 };
    assert(
        game_behavior_dispatch(f.live->runtime->behaviors, entity, "Signal", (flow_inputs){ &amount, 1 }, &error) == 1
    );
    assert(instance->execution.variables[0].i == 10);
    source = "behavior Pulse { state count: int = 0; "
             "on Tick() { count += Shared.Seven().Result; } "
             "on Signal() { count += event.Amount; } }";
    write_asset(&f, "behaviors/Pulse.chibi", "%s", source);
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    assert(game_behavior_instance(f.live->runtime->behaviors, entity, "Pulse") == instance);
    assert(
        game_behavior_dispatch(f.live->runtime->behaviors, entity, "Tick", (flow_inputs){ 0 }, &error) == 1
    );
    assert(instance->execution.variables[0].i == 17);
    game_preview_definition *pulse = game_preview_find_definition(f.live, "Pulse");
    const behavior_asset *working = pulse->working, *applied = pulse->applied;
    write_asset(&f, "behaviors/Pulse.chibi", "%s", "behavior Pulse { on Tick() { await Missing.Seven(); } }");
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_FAILED);
    assert(
        pulse->working == working && pulse->applied == applied &&
        game_behavior_instance(f.live->runtime->behaviors, entity, "Pulse") == instance
    );
    assert(
        game_behavior_dispatch(f.live->runtime->behaviors, entity, "Tick", (flow_inputs){ 0 }, &error) == 1
    );
    assert(instance->execution.variables[0].i == 24);
    fixture_free(&f);
}

static void the_scene_reloads_after_the_runtime_stops()
{
    disk_fixture f = fixture_new();
    select_instance(f.scene, 1);
    f.scene->camera = (scene_camera){ .view = { 10, 20, 300, 200 }, .saved = true };
    f.scene->runtime_snapshot_valid = true;
    write_asset(&f, "scenes/Main.kdl", "version 1\nname \"Main\"\n(array)entities { %s }\n", "- { name \"Dot_2\"; prototype \"Dot\"; }; - { name \"Dot_3\"; prototype \"Dot\"; };");
    assert(poll_disk(&f, "Main.kdl") == APP_DISK_NONE && acount(f.scene->actors) == 2);
    f.scene->runtime_snapshot_valid = false;
    assert(poll_disk(&f, "Main.kdl") == APP_DISK_RELOADED);
    assert(!strcmp(f.scene->actors[1].name, "Dot_3"));
    assert(!strcmp(selected_actor(f.scene)->name, "Dot_2"));
    assert(f.scene->camera.saved && f.scene->camera.view.center_x == 10);
    fixture_free(&f);
}

static void metadata_changes_reload_the_paired_source()
{
    disk_fixture f = fixture_new();
    const char *text = "behavior Pulse { on Tick() {} }\n";
    write_asset(&f, "behaviors/Pulse.chibi", "%s", text);
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    char path[128], sidecar[128];
    path_to(&f, "behaviors/Pulse.chibi", path, sizeof path);
    path_to(&f, "behaviors/Pulse.chibi.meta", sidecar, sizeof sidecar);
    code_source *source = code_source_parse(text);
    assert(source->asset && acount(source->asset->graph->nodes));
    node *tick = source->asset->graph->nodes[0];
    uint64_t id = tick->id;
    tick->x = 123;
    tick->y = 456;
    tick->placed = true;
    assert(code_source_metadata_save(source, path));
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    game_preview_definition *pulse = game_preview_find_definition(f.live, "Pulse");
    node *loaded = graph_node_by_id(pulse->working->graph, id);
    assert(loaded && loaded->placed && loaded->x == 123 && loaded->y == 456);
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_NONE);

    pulse->unsaved = true;
    tick->x = 789;
    assert(code_source_metadata_save(source, path));
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_UNSAVED);
    loaded = graph_node_by_id(pulse->working->graph, id);
    assert(loaded && loaded->x == 123);
    pulse->unsaved = false;
    assert(app_scene_authoring_reload_path(f.scene, path) == APP_DISK_RELOADED);
    loaded = graph_node_by_id(pulse->working->graph, id);
    assert(loaded && loaded->x == 789);

    /* A paired source and metadata save produces one document change. */
    write_asset(&f, "behaviors/Pulse.chibi", "%s", text);
    tick->x = 321;
    assert(code_source_metadata_save(source, path));
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    loaded = graph_node_by_id(pulse->working->graph, id);
    assert(loaded && loaded->x == 321);
    tick->x = 654;
    assert(code_source_metadata_save(source, path));
    scene_disk_note(f.scene, path);
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_NONE);
    assert(remove(sidecar) == 0);
    assert(poll_disk(&f, "Pulse.chibi") == APP_DISK_RELOADED);
    code_source_free(source);
    fixture_free(&f);
}

static void sidecars_are_not_assets()
{
    disk_fixture f = fixture_new();
    write_asset(&f, "behaviors/Pulse.chibi", PULSE, "");
    write_asset(&f, "behaviors/Pulse.chibi.meta", "%s", "{}");
    content_refresh(f.scene);
    for (int i = 0; i < acount(f.scene->content); i++) {
        assert(!strstr(f.scene->content[i].path, "Pulse.chibi.meta"));
    }
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    const reference_index *index = app_scene_authoring_asset_index(f.scene);
    assert(reference_index_find_name(index, REFERENCE_ASSET_BEHAVIOR, "Pulse") >= 0);
    assert(reference_index_find_name(index, REFERENCE_ASSET_BEHAVIOR, "Pulse.chibi.meta") < 0);
    char sidecar[128];
    path_to(&f, "behaviors/Pulse.chibi.meta", sidecar, sizeof sidecar);
    assert(remove(sidecar) == 0);
    fixture_free(&f);
}

void test_scene_metadata_disk()
{
    sidecars_are_not_assets();
    metadata_changes_reload_the_paired_source();
}

void test_scene_disk()
{
    test_scene_metadata_disk();
    prototypes_follow_disk_unless_edited();
    behaviors_appear_and_reload();
    imported_behavior_reload_preserves_runtime();
    the_scene_reloads_after_the_runtime_stops();
    puts(
        "scene disk PASS: external edits reload prototypes, behaviors and the "
        "scene; own saves, unsaved edits and "
        "a running scene wait"
    );
}
