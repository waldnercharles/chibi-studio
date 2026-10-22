#include "authoring/engine_preview.h"
/* The Scene's transform tools edit the scene as gestures: each writes live,
 * records one Undo step, snaps the dragged object to the grid and restores
 * everything when cancelled. */
#include "editor/workspace/input.h"
#include "editor/workspace/scene_internal.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "runtime/component_catalog.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <math.h>

static const char *const BOX =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Box\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"GAME.GameAppearance\", data.Object([\n"
    "                    data.Field.Named(\"height\", data.Value.Real(10.0)),\n"
    "                    data.Field.Named(\"width\", data.Value.Real(20.0))\n"
    "                ])),\n"
    "                data.Field.Named(\"GAME.GameCollider\", data.Object([\n"
    "                    data.Field.Named(\"height\", data.Value.Real(10.0)),\n"
    "                    data.Field.Named(\"radius\", data.Value.Real(10.0)),\n"
    "                    data.Field.Named(\"shape\", "
    "data.Value.Text(\"Box\")),\n"
    "                    data.Field.Named(\"width\", data.Value.Real(20.0))\n"
    "                ])),\n"
    "                data.Field.Named(\"GAME.GameOrientation\", "
    "data.Object([])),\n"
    "                data.Field.Named(\"GAME.GameTransform\", "
    "data.Object([]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

/* Two boxes, A at (10, 20) and B at (50, 20). */
static const char *const SCENE =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Gizmo\")),\n"
    "            data.Field.Named(\"entities\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"A\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Box\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"GAME.GameTransform\", "
    "data.Object([\n"
    "                            data.Field.Named(\"x\", "
    "data.Value.Real(10.0)),\n"
    "                            data.Field.Named(\"y\", "
    "data.Value.Real(20.0))\n"
    "                        ]))\n"
    "                    ]))\n"
    "                ]),\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"B\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Box\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"GAME.GameTransform\", "
    "data.Object([\n"
    "                            data.Field.Named(\"x\", "
    "data.Value.Real(50.0)),\n"
    "                            data.Field.Named(\"y\", "
    "data.Value.Real(20.0))\n"
    "                        ]))\n"
    "                    ]))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    app_scene_authoring *scene;
} gizmo_fixture;

static gizmo_fixture fixture_new()
{
    gizmo_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Gizmo", behavior_asset_new("Gizmo"));
    assert(definition && game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Gizmo"));
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
    assert(f.scene);
    assert(app_scene_authoring_register_prototype(f.scene, BOX, NULL) == 0);
    assert(scene_authoring_load_text(f.scene, SCENE, NULL) == 0);
    assert(acount(f.scene->actors) == 2);
    return f;
}

static void fixture_free(gizmo_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-3f;
}

static const GameTransform *position_of(const gizmo_fixture *f, int actor)
{
    return GAME_GameTransform_get_const(f->world, f->scene->actors[actor].entity);
}

static const GameAppearance *appearance_of(const gizmo_fixture *f, int actor)
{
    return GAME_GameAppearance_get_const(f->world, f->scene->actors[actor].entity);
}

static const GameCollider *collider_of(const gizmo_fixture *f, int actor)
{
    return GAME_GameCollider_get_const(f->world, f->scene->actors[actor].entity);
}

static void use_tool(gizmo_fixture *f, scene_tool tool, bool snap)
{
    f->scene->gizmo.in = (scene_tool_input){ .tool = tool, .snap = snap, .increment = 8 };
}

/* Dragging a box's right handle resizes it and its collider from the left
   edge, and Undo restores them all in one step. */
static void resize_is_one_step()
{
    gizmo_fixture f = fixture_new();
    select_instance(f.scene, 0);
    use_tool(&f, SCENE_TOOL_SCALE, false);
    /* Canvas units run Y up: A's right edge is at (20, -20). */
    scene_gizmo_begin(f.scene, SCENE_GIZMO_HANDLE + 3, (CF_V2){ 20, -20 });
    scene_gizmo_update(f.scene, (CF_V2){ 30, -21 });
    scene_gizmo_finish(f.scene, false);
    const GameAppearance *size = appearance_of(&f, 0);
    const GameCollider *collider = collider_of(&f, 0);
    bool resized = near(size->width, 30) && near(size->height, 10) &&
                   near(position_of(&f, 0)->x, 15) &&
                   near(position_of(&f, 0)->y, 20) && near(collider->width, 30) &&
                   near(collider->height, 10) && near(collider->radius, 15) &&
                   !strcmp(f.scene->gizmo.report, "Size 30 \xc3\x97 10");
    bool undone = replay_scene_edit(f.scene, false) == 0 &&
                  near(size->width, 20) && near(position_of(&f, 0)->x, 10) &&
                  near(collider->width, 20) && near(collider->radius, 10) &&
                  !scene_history_can_undo(f.scene->history);
    test_check(resized && undone, "a resize changes the size, the collider and the position, as one undo step");
    fixture_free(&f);
}

/* Snapping lands the dragged object on the grid, not the pointer's delta,
   and Ctrl lifts it during the gesture. */
static void move_snaps_position()
{
    gizmo_fixture f = fixture_new();
    select_instance(f.scene, 0);
    use_tool(&f, SCENE_TOOL_MOVE, true);
    scene_gizmo_begin(f.scene, SCENE_GIZMO_FREE, (CF_V2){ 10, -20 });
    scene_gizmo_update(f.scene, (CF_V2){ 23, -25 });
    bool snapped = near(position_of(&f, 0)->x, 24) && near(position_of(&f, 0)->y, 24);
    f.scene->gizmo.in.ctrl = true;
    scene_gizmo_update(f.scene, (CF_V2){ 23, -25 });
    bool free = near(position_of(&f, 0)->x, 23) && near(position_of(&f, 0)->y, 25);
    scene_gizmo_update(f.scene, (CF_V2){ 30, -20 });
    f.scene->gizmo.in.ctrl = false;
    scene_gizmo_begin(f.scene, SCENE_GIZMO_AXIS_X, (CF_V2){ 30, -20 });
    scene_gizmo_update(f.scene, (CF_V2){ 37, -40 });
    bool constrained = near(position_of(&f, 0)->x, 40) &&
                       near(position_of(&f, 0)->y, 20);
    scene_gizmo_finish(f.scene, true);
    bool cancelled = near(position_of(&f, 0)->x, 30);
    test_check(snapped && free && constrained && cancelled, "a move snaps the object to the grid, Ctrl frees it, an axis constrains it and Escape restores it");
    fixture_free(&f);
}

/* A move of several selected objects keeps their layout and is one step. */
static void selection_moves_together()
{
    gizmo_fixture f = fixture_new();
    f.scene->actors[0].marked = f.scene->actors[1].marked = true;
    f.scene->selected = 1;
    use_tool(&f, SCENE_TOOL_MOVE, false);
    scene_gizmo_begin(f.scene, SCENE_GIZMO_FREE, (CF_V2){ 0, 0 });
    scene_gizmo_update(f.scene, (CF_V2){ 16, 4 });
    scene_gizmo_finish(f.scene, false);
    bool moved = near(position_of(&f, 0)->x, 26) && near(position_of(&f, 0)->y, 16) &&
                 near(position_of(&f, 1)->x, 66) && near(position_of(&f, 1)->y, 16);
    bool undone = replay_scene_edit(f.scene, false) == 0 &&
                  near(position_of(&f, 0)->x, 10) &&
                  near(position_of(&f, 1)->x, 50) &&
                  !scene_history_can_undo(f.scene->history);
    test_check(moved && undone, "a selection moves together as one undo step");
    fixture_free(&f);
}

/* Rotation turns the orientation by the pointer's angle about the pivot,
   in 15 degree steps while snapping. */
static void rotation_snaps_steps()
{
    gizmo_fixture f = fixture_new();
    select_instance(f.scene, 0);
    use_tool(&f, SCENE_TOOL_ROTATE, true);
    scene_gizmo_begin(f.scene, SCENE_GIZMO_RING, (CF_V2){ 20, -20 });
    /* 80 degrees clockwise on screen is +80 degrees of gameplay heading. */
    float angle = -80 * CF_PI / 180;
    scene_gizmo_update(f.scene, (CF_V2){ 10 + 10 * cosf(angle), -20 + 10 * sinf(angle) });
    scene_gizmo_finish(f.scene, false);
    const GameOrientation *
        orientation = GAME_GameOrientation_get_const(f.world, f.scene->actors[0].entity);
    test_check(
        near(orientation->radians, 75 * CF_PI / 180) && near(position_of(&f, 0)->x, 10),
        "a rotation turns the orientation in 15 degree steps while snapping"
    );
    fixture_free(&f);
}

void test_scene_gizmo(const char *program)
{
    CF_Result result = cf_make_app(
        "Scene gizmo",
        0,
        0,
        0,
        256,
        256,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    assert(!cf_is_error(result));
    app_scene_renderer_initialize();
    app_input_initialize();
    test_check(near(scene_snap(13, 8), 16) && near(scene_snap(-11, 8), -8), "snapping rounds to the nearest increment");
    resize_is_one_step();
    move_snaps_position();
    selection_moves_together();
    rotation_snaps_steps();
    app_input_shutdown();
    cf_destroy_app();
}
