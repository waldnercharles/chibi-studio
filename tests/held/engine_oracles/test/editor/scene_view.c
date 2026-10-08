#include "authoring/engine_preview.h"
/* The Scene view's game preview while a scene is edited: Generated World
 * prepares the scene's materials without dirtying it and Play discards them,
 * and View Through Camera frames a GameCamera's span. */
#include "editor/workspace/viewport.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "test/suite_internal.h"

#include <assert.h>

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    behavior_asset *asset;
    editor *graph;
    engine *runtime;
    app_scene_authoring *scene;
} view_fixture;

static const char *const PROTOTYPES[] = {
    "name \"Rock\"\n"
    "components {\n"
    "    GameTransform {\n"
    "        x 0\n"
    "        y 0\n"
    "    }\n"
    "    GameCollider {\n"
    "        radius 10\n"
    "    }\n"
    "    GameDestructible {\n"
    "        cell_size 2\n"
    "        min_fragment_cells 1\n"
    "    }\n"
    "}\n",
    "name \"OffCamera\"\n"
    "components {\n"
    "    GameTransform {\n"
    "        x 0\n"
    "        y 0\n"
    "    }\n"
    "    GameCamera {\n"
    "        width 100\n"
    "        height 50\n"
    "        disabled #true\n"
    "    }\n"
    "}\n",
    "name \"ViewCamera\"\n"
    "components {\n"
    "    GameTransform {\n"
    "        x 0\n"
    "        y 0\n"
    "    }\n"
    "    GameCamera {\n"
    "        width 320\n"
    "        height 180\n"
    "    }\n"
    "}\n",
};

static view_fixture fixture_new()
{
    view_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.asset = behavior_asset_new("SceneView");
    f.graph = editor_new(f.asset);
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("SceneView", behavior_asset_new("SceneView"));
    assert(game_preview_add_definition(f.live, definition) == 0);
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    engine_set_playing(f.runtime, false);
    f.scene = app_scene_authoring_new(f.world, f.live, definition, f.asset, f.graph, NULL, NULL, f.runtime);
    assert(f.scene);
    for (size_t i = 0; i < sizeof PROTOTYPES / sizeof *PROTOTYPES; i++) {
        assert(app_scene_authoring_register_prototype(f.scene, PROTOTYPES[i], NULL) == 0);
    }
    return f;
}

static void fixture_free(view_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
}

static ecs_entity add(view_fixture *f, const char *prototype, float x, float y)
{
    assert(app_scene_authoring_add_instance(f->scene, prototype) == 0);
    ecs_entity entity = alast(f->scene->actors).entity;
    *GAME_GameTransform_get(f->world, entity) = (GameTransform){ x, y };
    return entity;
}

static void generated_world()
{
    view_fixture f = fixture_new();
    ecs_entity rock = add(&f, "Rock", 40, 10);
    f.scene->scene_dirty = false;
    const game_materials *materials = f.live->runtime->materials;
    engine_viewport_camera view = { 0, 0, 640, 360 };
    bool lit = f.scene->view_options.lighting && !f.scene->view_options.generated_world;
    scene_world_preview_update(f.scene, &view);
    bool off = !game_materials_mask(materials, rock);
    uint64_t history = scene_history_group_id(f.scene->history, false);
    app_scene_authoring_set_view_options(f.scene, (app_scene_view_options){ .generated_world = true });
    scene_world_preview_update(f.scene, &view);
    scene_world_preview_update(f.scene, &view);
    bool made = game_materials_mask(materials, rock) &&
                f.scene->world_preview.built && !f.scene->world_preview.dirty;
    bool untouched = !f.scene->scene_dirty &&
                     scene_history_group_id(f.scene->history, false) == history &&
                     GAME_GameTransform_get_const(f.world, rock)->x == 40 &&
                     acount(f.scene->actors) == 1;
    GAME_GameTransform_get(f.world, rock)->x = 60;
    scene_world_preview_update(f.scene, &view);
    bool waits = f.scene->world_preview.dirty && game_materials_mask(materials, rock);
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    bool played = !game_materials_mask(materials, rock) &&
                  !f.scene->world_preview.built;
    assert(app_scene_authoring_runtime_command(f.scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    rock = f.scene->actors[0].entity;
    scene_world_preview_update(f.scene, &view);
    bool again = game_materials_mask(materials, rock) != NULL;
    app_scene_authoring_set_view_options(f.scene, (app_scene_view_options){ .lighting = true });
    scene_world_preview_update(f.scene, &view);
    bool discarded = !game_materials_mask(materials, rock) &&
                     !f.scene->world_preview.built;
    test_check(lit && off && made && untouched && waits && played && again && discarded, "Generated World prepares an edited scene's materials without dirtying it, keeps them while an edit waits, and Play and turning it off discard them");
    fixture_free(&f);
}

static void camera_framing()
{
    view_fixture f = fixture_new();
    engine_viewport_view view;
    bool none = !app_scene_authoring_camera_framing(f.scene, &view);
    ecs_entity off = add(&f, "OffCamera", -50, 0);
    ecs_entity on = add(&f, "ViewCamera", 100, 50);
    GAME_GameCamera_get(f.world, on)->offset = (CF_V2){ 10, 20 };
    f.scene->selected = -1;
    bool active = app_scene_authoring_camera_framing(f.scene, &view) &&
                  view.center_x == 110 && view.center_y == -70 &&
                  view.width == 320 && view.height == 180;
    select_instance(f.scene, 0);
    bool selected = selected_actor(f.scene)->entity.slot == off.slot &&
                    app_scene_authoring_camera_framing(f.scene, &view) &&
                    view.center_x == -50 && view.width == 100 && view.height == 50;
    bool undrawn = app_scene_authoring_view_through_camera(f.scene, true) == -1 &&
                   !app_scene_authoring_viewing_through_camera(f.scene);
    test_check(none && active && selected && undrawn, "View Through Camera frames the selected camera's span, else the first enabled one's, around its object and offset");
    fixture_free(&f);
}

void test_scene_view()
{
    generated_world();
    camera_framing();
}

/* With a viewport: the view takes the camera's span, follows the camera,
 * leaves when the view moves, and restores the view from before. */
void test_scene_view_camera()
{
    view_fixture f = fixture_new();
    ecs_entity camera = add(&f, "ViewCamera", 100, 50);
    engine_viewport *viewport = engine_viewport_new(64, 36);
    assert(viewport);
    f.scene->render_viewport = viewport;
    engine_viewport_view before, view;
    engine_viewport_get_view(viewport, &before);
    bool entered = app_scene_authoring_view_through_camera(f.scene, true) == 0 &&
                   app_scene_authoring_viewing_through_camera(f.scene);
    engine_viewport_get_view(viewport, &view);
    bool framed = entered && view.center_x == 100 && view.center_y == -50 &&
                  view.width == 320 && view.height == 180;
    GAME_GameTransform_get(f.world, camera)->x = 140;
    scene_camera_view_follow(f.scene, viewport, true);
    engine_viewport_get_view(viewport, &view);
    bool followed = view.center_x == 140 &&
                    app_scene_authoring_viewing_through_camera(f.scene);
    app_scene_authoring_view_through_camera(f.scene, false);
    engine_viewport_get_view(viewport, &view);
    bool restored = !app_scene_authoring_viewing_through_camera(f.scene) &&
                    view.center_x == before.center_x && view.width == before.width;
    app_scene_authoring_view_through_camera(f.scene, true);
    engine_viewport_view panned = { 7, 8, 90, 50, 0 };
    engine_viewport_set_view(viewport, &panned);
    scene_camera_view_follow(f.scene, viewport, true);
    engine_viewport_get_view(viewport, &view);
    bool left = !app_scene_authoring_viewing_through_camera(f.scene) &&
                view.center_x == 7;
    test_check(framed && followed && restored && left, "View Through Camera sets the view to the camera's span, follows the camera, restores the earlier view, and ends when the view moves");
    engine_viewport_free(viewport);
    fixture_free(&f);
}
