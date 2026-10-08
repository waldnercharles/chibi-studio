#include "authoring/engine_preview.h"
#include "editor/workspace/asset_refactor.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/text_file.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Studio renames and deletes across the files of an open project. */
typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    app_scene_authoring *scene;
    char root[64];
} refactor_fixture;

static const char PULSE[] = "behavior Pulse { on Tick() {} }\n";
static const char DOT[] = "name \"Dot\"\n"
                          "components {\n"
                          "    GAME.GameTransform x=1.0\n"
                          "}\n"
                          "(array)behaviors \"Pulse\"\n";
static const char MAIN[] = "version 1\n"
                           "name \"Main\"\n"
                           "(array)entities {\n"
                           "    - {\n"
                           "        name \"Dot_1\"\n"
                           "        prototype \"Dot\"\n"
                           "        (array)behaviors \"Pulse\"\n"
                           "    }\n"
                           "    - {\n"
                           "        name \"Dot_2\"\n"
                           "        prototype \"Dot\"\n"
                           "        (array)behaviors \"Pulse\"\n"
                           "    }\n"
                           "}\n";

static const char *const FOLDERS[] = { "behaviors", "libraries", "prototypes", "scenes" };
#define FOLDER_COUNT ((int)(sizeof FOLDERS / sizeof *FOLDERS))
#define WEAPONS "test/fixtures/language/content/libraries/WeaponActions.chibi"

static void path_to(const refactor_fixture *f, const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", f->root, name);
}

static void write_asset(const refactor_fixture *f, const char *name, const char *text)
{
    char path[128];
    path_to(f, name, path, sizeof path);
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static bool exists(const refactor_fixture *f, const char *name)
{
    char path[128];
    path_to(f, name, path, sizeof path);
    struct stat info;
    return stat(path, &info) == 0;
}

static bool holds(const refactor_fixture *f, const char *name, const char *text)
{
    char path[128];
    path_to(f, name, path, sizeof path);
    char *contents = text_file_read(path);
    bool found = contents && strstr(contents, text);
    free(contents);
    return found;
}

static refactor_fixture fixture_new()
{
    refactor_fixture f = { .world = ecs_world_new() };
    snprintf(f.root, sizeof f.root, "/tmp/chibi-refactor-XXXXXX");
    assert(mkdtemp(f.root));
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    for (int i = 0; i < FOLDER_COUNT; i++) {
        char folder[128];
        path_to(&f, FOLDERS[i], folder, sizeof folder);
        assert(mkdir(folder, 0755) == 0);
    }
    char *library = text_file_read(WEAPONS);
    assert(library);
    write_asset(&f, "libraries/WeaponActions.chibi", library);
    free(library);
    library = text_file_read("test/fixtures/language/content/libraries/DirectionMath.chibi");
    assert(library);
    write_asset(&f, "libraries/DirectionMath.chibi", library);
    free(library);
    write_asset(&f, "behaviors/Pulse.chibi", PULSE);
    write_asset(&f, "prototypes/Dot.kdl", DOT);
    write_asset(&f, "scenes/Main.kdl", MAIN);
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
    char scene[128];
    path_to(&f, "scenes/Main.kdl", scene, sizeof scene);
    assert(app_scene_authoring_set_content_root(f.scene, f.root) == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(game_preview_find_definition(f.live, "WeaponActions"));
    assert(game_preview_find_definition(f.live, "DirectionMath"));
    assert(app_scene_authoring_load(f.scene, scene) == 0);
    return f;
}

static void fixture_free(refactor_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
    for (int i = 0; i < FOLDER_COUNT; i++) {
        char folder[128];
        path_to(f, FOLDERS[i], folder, sizeof folder);
        DIR *directory = opendir(folder);
        struct dirent *item;
        while (directory && (item = readdir(directory))) {
            char file[512];
            snprintf(file, sizeof file, "%s/%s", folder, item->d_name);
            if (item->d_name[0] != '.') {
                assert(remove(file) == 0);
            }
        }
        if (directory) {
            closedir(directory);
        }
        assert(rmdir(folder) == 0);
    }
    assert(rmdir(f->root) == 0);
}

static reference_symbol asset_named(refactor_fixture *f, reference_asset_kind kind, const char *name)
{
    app_scene_authoring_refresh_asset_index(f->scene);
    int asset = reference_index_find_name(app_scene_authoring_asset_index(f->scene), kind, name);
    assert(asset >= 0);
    return (reference_symbol){ .kind = REFERENCE_SYMBOL_ASSET, .asset = asset };
}

static app_refactor *rename_to(
    refactor_fixture *f,
    reference_symbol symbol,
    const char *name,
    char *error,
    size_t size
)
{
    app_refactor *plan = NULL;
    app_scene_authoring_plan_rename(f->scene, symbol, name, &plan, error, size);
    return plan;
}

/* A behavior's file and name follow its rename, as do the prototype and
 * objects attaching it; Undo puts every file back. The open behavior stays
 * open and saved under its new name. */
static void behaviors()
{
    refactor_fixture f = fixture_new();
    char error[256] = "";
    assert(
        app_scene_authoring_open_asset(
            f.scene,
            asset_named(&f, REFERENCE_ASSET_BEHAVIOR, "Pulse").asset
        ) == 0
    );
    app_refactor *plan = rename_to(
        &f,
        asset_named(&f, REFERENCE_ASSET_BEHAVIOR, "Pulse"),
        "Beat",
        error,
        sizeof error
    );
    bool listed = plan && app_refactor_count(plan) == 3;
    app_refactor *undo = NULL;
    bool applied = plan &&
                   app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) ==
                       0;
    bool moved = applied && exists(&f, "behaviors/Beat.chibi") &&
                 !exists(&f, "behaviors/Pulse.chibi") &&
                 holds(&f, "prototypes/Dot.kdl", "Beat") &&
                 holds(&f, "scenes/Main.kdl", "Beat");
    bool loaded = game_preview_find_definition(f.live, "Beat") &&
                  !game_preview_find_definition(f.live, "Pulse") &&
                  game_prototype_has_graph(find_prototype(f.scene, "Dot")->prototype, "Beat");
    sync_behavior_working(f.scene);
    bool saved = loaded && !game_preview_find_definition(f.live, "Beat")->unsaved &&
                 !strcmp(f.scene->document_id, "Beat");
    if (!saved) {
        printf("  the open behavior is unsaved after its rename\n");
    }
    app_refactor *redo = NULL;
    bool undone = undo &&
                  app_scene_authoring_apply_refactor(f.scene, undo, &redo, error, sizeof error) ==
                      0 &&
                  exists(&f, "behaviors/Pulse.chibi") &&
                  !exists(&f, "behaviors/Beat.chibi") &&
                  game_preview_find_definition(f.live, "Pulse") &&
                  !game_preview_find_definition(f.live, "Beat");
    if (!(listed && moved && loaded && undone)) {
        printf("  behavior rename: %s\n", error);
    }
    assert(listed && moved && loaded && saved && undone);
    app_refactor_free(plan);
    app_refactor_free(undo);
    app_refactor_free(redo);
    fixture_free(&f);
}

/* An open library keeps its function graph open and saved when renamed. */
static void libraries()
{
    refactor_fixture f = fixture_new();
    char error[256] = "";
    assert(
        app_scene_authoring_open_asset(
            f.scene,
            asset_named(&f, REFERENCE_ASSET_LIBRARY, "WeaponActions").asset
        ) == 0
    );
    sync_behavior_working(f.scene);
    assert(!game_preview_find_definition(f.live, "WeaponActions")->unsaved);
    app_refactor *plan = rename_to(
        &f,
        asset_named(&f, REFERENCE_ASSET_LIBRARY, "WeaponActions"),
        "Weapons",
        error,
        sizeof error
    );
    app_refactor *undo = NULL;
    assert(
        plan &&
        app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) == 0
    );
    sync_behavior_working(f.scene);
    game_preview_definition *renamed = game_preview_find_definition(f.live, "Weapons");
    bool saved = renamed && !renamed->unsaved &&
                 !strcmp(f.scene->document_id, "Weapons");
    if (!saved) {
        printf("  the open library is unsaved after its rename: %s\n", error);
    }
    assert(saved);
    app_refactor_free(plan);
    app_refactor_free(undo);
    fixture_free(&f);
}

/* A prototype keeps its place and instances while renamed. */
static void prototypes()
{
    refactor_fixture f = fixture_new();
    char error[256] = "";
    app_refactor *plan = rename_to(
        &f,
        asset_named(&f, REFERENCE_ASSET_PROTOTYPE, "Dot"),
        "Point",
        error,
        sizeof error
    );
    app_refactor *undo = NULL;
    bool applied = plan &&
                   app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) ==
                       0;
    bool renamed = applied && find_prototype(f.scene, "Point") &&
                   !find_prototype(f.scene, "Dot") && acount(f.scene->actors) == 2 &&
                   !strcmp(f.scene->actors[0].prototype_name, "Point") &&
                   holds(&f, "scenes/Main.kdl", "prototype \"Point\"");
    if (!renamed) {
        printf("  prototype rename: %s\n", error);
    }
    assert(renamed);
    app_refactor_free(plan);
    app_refactor_free(undo);
    fixture_free(&f);
}

/* Unsaved edits keep a plan from rewriting their file; a changed file keeps
 * a plan from applying. */
static void refusals()
{
    refactor_fixture f = fixture_new();
    char error[256] = "";
    reference_symbol pulse = asset_named(&f, REFERENCE_ASSET_BEHAVIOR, "Pulse");
    find_prototype(f.scene, "Dot")->dirty = 1;
    app_refactor *plan = rename_to(&f, pulse, "Beat", error, sizeof error);
    assert(!plan && strstr(error, "Save Dot"));
    find_prototype(f.scene, "Dot")->dirty = 0;
    plan = rename_to(&f, pulse, "pulse", error, sizeof error);
    assert(!plan && strstr(error, "only in case"));
    plan = rename_to(&f, pulse, "Beat", error, sizeof error);
    assert(plan);
    write_asset(
        &f,
        "prototypes/Dot.kdl",
        "name \"Dot\"\n"
        "(array)behaviors \"Pulse\"\n"
    );
    app_refactor *undo = NULL;
    assert(app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) != 0);
    assert(!undo && exists(&f, "behaviors/Pulse.chibi") && !exists(&f, "behaviors/Beat.chibi"));
    app_refactor_free(plan);

    /* Edits made after a rename keep its Undo from reloading over them. */
    write_asset(&f, "prototypes/Dot.kdl", DOT);
    char dot[128];
    path_to(&f, "prototypes/Dot.kdl", dot, sizeof dot);
    app_scene_authoring_reload_path(f.scene, dot);
    plan = rename_to(&f, asset_named(&f, REFERENCE_ASSET_BEHAVIOR, "Pulse"), "Beat", error, sizeof error);
    assert(
        plan &&
        app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) == 0
    );
    find_prototype(f.scene, "Dot")->dirty = 1;
    app_refactor *redo = NULL;
    bool kept = app_scene_authoring_apply_refactor(f.scene, undo, &redo, error, sizeof error) !=
                    0 &&
                strstr(error, "Save Dot") && exists(&f, "behaviors/Beat.chibi") &&
                find_prototype(f.scene, "Dot")->dirty;
    if (!kept) {
        printf("  undo over unsaved edits: %s\n", error);
    }
    assert(kept);
    app_refactor_free(plan);
    app_refactor_free(undo);
    app_refactor_free(redo);
    fixture_free(&f);
}

/* A deleted behavior's file goes, and Undo brings it back. */
static void deletes()
{
    refactor_fixture f = fixture_new();
    char error[256] = "";
    app_refactor *plan = NULL;
    reference_symbol pulse = asset_named(&f, REFERENCE_ASSET_BEHAVIOR, "Pulse");
    assert(app_scene_authoring_plan_delete(f.scene, pulse, &plan, error, sizeof error) == 0);
    assert(app_refactor_count(plan) == 1);
    app_refactor *undo = NULL;
    assert(app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) == 0);
    assert(!exists(&f, "behaviors/Pulse.chibi"));
    app_refactor *redo = NULL;
    assert(app_scene_authoring_apply_refactor(f.scene, undo, &redo, error, sizeof error) == 0);
    assert(exists(&f, "behaviors/Pulse.chibi") && game_preview_find_definition(f.live, "Pulse"));
    app_refactor_free(plan);
    app_refactor_free(undo);
    app_refactor_free(redo);

    /* A deleted prototype the scene still places stays in memory without a
       file; Undo gives it its file back. */
    reference_symbol dot = asset_named(&f, REFERENCE_ASSET_PROTOTYPE, "Dot");
    assert(app_scene_authoring_plan_delete(f.scene, dot, &plan, error, sizeof error) == 0);
    assert(app_scene_authoring_apply_refactor(f.scene, plan, &undo, error, sizeof error) == 0);
    scene_prototype_asset *kept = find_prototype(f.scene, "Dot");
    assert(kept && !kept->path && kept->dirty);
    assert(app_scene_authoring_apply_refactor(f.scene, undo, &redo, error, sizeof error) == 0);
    int copies = 0;
    for (int i = 0; i < acount(f.scene->prototypes); i++) {
        copies += !strcmp(f.scene->prototypes[i].prototype->name, "Dot");
    }
    kept = find_prototype(f.scene, "Dot");
    bool adopted = copies == 1 && kept->path && !kept->dirty &&
                   exists(&f, "prototypes/Dot.kdl");
    if (!adopted) {
        printf(
            "  undo of a placed prototype's delete: %d copies, path %s, dirty "
            "%d\n",
            copies,
            kept->path ? kept->path : "none",
            kept->dirty
        );
    }
    assert(adopted);
    app_refactor_free(plan);
    app_refactor_free(undo);
    app_refactor_free(redo);
    fixture_free(&f);
}

void test_scene_refactor()
{
    behaviors();
    libraries();
    prototypes();
    refusals();
    deletes();
    puts("scene refactor PASS: renames and deletes rewrite files all or none, Studio follows them, and Undo restores every file");
}
