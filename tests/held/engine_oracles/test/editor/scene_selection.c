#include "authoring/engine_preview.h"
/* One selection model for the Scene, the Outliner and Properties: toggling
 * and ranges, commands on the whole selection as one Undo step, the object
 * clipboard's scene text, and multi-object field edits. */
#include "editor/workspace/editor.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/component_catalog.h"
#include "support/asset_document.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    "                data.Field.Named(\"GAME.GameTransform\", "
    "data.Object([]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

/* Three boxes, A at (10, 20), B at (50, 20) and C at (90, 60), C wider. */
static const char *const SCENE =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Selection\")),\n"
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
    "                ]),\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"C\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Box\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"GAME.GameAppearance\", "
    "data.Object([\n"
    "                            data.Field.Named(\"width\", "
    "data.Value.Real(44.0))\n"
    "                        ])),\n"
    "                        data.Field.Named(\"GAME.GameTransform\", "
    "data.Object([\n"
    "                            data.Field.Named(\"x\", "
    "data.Value.Real(90.0)),\n"
    "                            data.Field.Named(\"y\", "
    "data.Value.Real(60.0))\n"
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
    editor *graph;
    engine *runtime;
    app_scene_authoring *scene;
} selection_fixture;

static selection_fixture fixture_new(const char *scene)
{
    selection_fixture f = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Selection", behavior_asset_new("Selection"));
    assert(game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Selection"));
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    engine_set_playing(f.runtime, false);
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
    if (scene) {
        assert(scene_authoring_load_text(f.scene, scene, NULL) == 0);
    }
    return f;
}

static void fixture_free(selection_fixture *f)
{
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
}

static const GameTransform *position_of(const selection_fixture *f, int actor)
{
    return GAME_GameTransform_get_const(f->world, f->scene->actors[actor].entity);
}

static bool selection_is(const selection_fixture *f, const char *names, int active)
{
    char selected[16] = "";
    size_t used = 0;
    for (int i = 0; i < acount(f->scene->actors); i++) {
        if (scene_instance_selected(f->scene, i)) {
            selected[used++] = f->scene->actors[i].name[0];
        }
    }
    selected[used] = '\0';
    return !strcmp(selected, names) && f->scene->selected == active;
}

/* An object's overridden fields as scene text writes them. */
static char *overrides_of(const selection_fixture *f, int actor)
{
    const scene_actor *a = &f->scene->actors[actor];
    av_value *value = game_prototype_capture_overrides_value(a->base, f->world, a->entity);
    char *text = av_value_write(value);
    av_value_free(value);
    return text;
}

/* Mod+click toggles a row and Shift+click selects a range from the anchor,
   the clicked row active; taking the active one out leaves another active. */
static void toggle_and_range()
{
    selection_fixture f = fixture_new(SCENE);
    select_instance(f.scene, 0);
    scene_toggle_instance(f.scene, 2);
    bool added = selection_is(&f, "AC", 2) && scene_selection_count(f.scene) == 2;
    scene_toggle_instance(f.scene, 2);
    bool removed = selection_is(&f, "A", 0);
    scene_toggle_instance(f.scene, 1);
    scene_toggle_instance(f.scene, 1);
    bool active_left = selection_is(&f, "A", 0);
    scene_toggle_instance(f.scene, 0);
    bool emptied = selection_is(&f, "", -1);
    int rows[] = { 0, 1, 2 };
    scene_select_range(f.scene, rows, 0, 2);
    bool range = selection_is(&f, "ABC", 2);
    scene_select_range(f.scene, rows, 2, 1);
    bool back = selection_is(&f, "BC", 1);
    test_check(added && removed && active_left && emptied && range && back, "Mod+click toggles objects in the selection and Shift+click selects a range, the clicked one active");
    fixture_free(&f);
}

/* Undo restores the selection a step began with and Redo the one it ended
   with, as Unity and Blender do; Studio notes it as each frame begins. */
static void undo_restores_selection()
{
    selection_fixture f = fixture_new(SCENE);
    select_instance(f.scene, 0);
    scene_toggle_instance(f.scene, 2);
    scene_selection_track(f.scene);
    bool duplicated = scene_duplicate_selection(f.scene, (GameTransform){ 8, 8 }) == 0;
    scene_selection_track(f.scene);
    bool undone = replay_scene_edit(f.scene, false) == 0 && selection_is(&f, "AC", 2);
    bool redone = replay_scene_edit(f.scene, true) == 0 &&
                  scene_selection_count(f.scene) == 2 &&
                  scene_instance_selected(f.scene, 3) &&
                  scene_instance_selected(f.scene, 4) && f.scene->selected == 4;
    select_instance(f.scene, 1);
    scene_selection_track(f.scene);
    bool deleted = app_scene_authoring_delete_instance(f.scene) == 0;
    scene_selection_track(f.scene);
    bool restored = replay_scene_edit(f.scene, false) == 0 &&
                    selection_is(&f, "B", 1);
    test_check(duplicated && undone && redone && deleted && restored, "Undo restores the selection a step began with and Redo the one it ended with");
    fixture_free(&f);
}

/* Duplicate copies every selected object a grid step away as one Undo step,
   and the copies become the selection. */
static void duplicate_is_one_step()
{
    selection_fixture f = fixture_new(SCENE);
    select_instance(f.scene, 0);
    scene_toggle_instance(f.scene, 2);
    bool duplicated = scene_duplicate_selection(f.scene, (GameTransform){ 8, 8 }) == 0 &&
                      acount(f.scene->actors) == 5;
    bool placed = duplicated && position_of(&f, 3)->x == 18 &&
                  position_of(&f, 3)->y == 28 && position_of(&f, 4)->x == 98 &&
                  GAME_GameAppearance_get_const(f.world, f.scene->actors[4].entity)
                          ->width == 44;
    bool selected = duplicated && scene_selection_count(f.scene) == 2 &&
                    scene_instance_selected(f.scene, 3) &&
                    scene_instance_selected(f.scene, 4) && f.scene->selected == 4;
    bool undone = replay_scene_edit(f.scene, false) == 0 &&
                  acount(f.scene->actors) == 3 &&
                  !scene_history_can_undo(f.scene->history);
    bool redone = replay_scene_edit(f.scene, true) == 0 &&
                  acount(f.scene->actors) == 5 && position_of(&f, 4)->x == 98;
    test_check(placed && selected && undone && redone, "Duplicate copies the whole selection a grid step away as one undo step and selects the copies");
    fixture_free(&f);
}

/* Copy writes scene text; Paste in another scene makes objects with the same
   components, names kept unless taken, as one Undo step. */
static void clipboard_round_trip()
{
    selection_fixture f = fixture_new(SCENE);
    select_instance(f.scene, 1);
    scene_toggle_instance(f.scene, 2);
    char *text = scene_copy_selection(f.scene);
    av_doc *clipboard = text ? asset_document_read(text, NULL, 0) : NULL;
    av_value *copied = av_obj_get(av_doc_root(clipboard), "entities");
    av_value *first = av_arr_get(copied, 0);
    av_value *appearance = av_obj_get(av_obj_get(first, "components"), "GAME.GameAppearance");
    bool written = clipboard && av_arr_size(copied) == 2 &&
                   !strcmp(av_get_str(av_obj_get(first, "prototype")), "Box") &&
                   av_get_num(av_obj_get(appearance, "width")) == 44 &&
                   strcmp(av_get_str(av_obj_get(first, "name")), "A");
    av_doc_free(clipboard);
    selection_fixture other = fixture_new(NULL);
    int pasted = scene_paste(other.scene, text, (scene_paste_place){ 0 });
    bool made = pasted == 2 && acount(other.scene->actors) == 2 &&
                !strcmp(other.scene->actors[0].name, "B") &&
                !strcmp(other.scene->actors[1].name, "C") &&
                scene_selection_count(other.scene) == 2;
    char *source = overrides_of(&f, 2), *copy = made ? overrides_of(&other, 1) : NULL;
    bool same = copy && !strcmp(source, copy);
    bool one_step = replay_scene_edit(other.scene, false) == 0 &&
                    acount(other.scene->actors) == 0 &&
                    !scene_history_can_undo(other.scene->history);
    /* Pasting into the scene copied from renames and moves the group. */
    GameTransform center = { 0, 0 };
    pasted = scene_paste(f.scene, text, (scene_paste_place){ .center = &center });
    bool renamed = pasted == 2 && !strcmp(f.scene->actors[3].name, "Box_1") &&
                   !strcmp(f.scene->actors[4].name, "Box_2");
    bool centered = renamed && position_of(&f, 3)->x == -20 &&
                    position_of(&f, 3)->y == -20 &&
                    position_of(&f, 4)->x == 20 && position_of(&f, 4)->y == 20;
    test_check(written && made && same && one_step && centered, "Copy writes the selection as scene text and Paste makes the same objects in another scene, centered where asked, as one undo step");
    free(source);
    free(copy);
    free(text);
    fixture_free(&other);
    fixture_free(&f);
}

/* Text that is not scene text, or names a prototype the project lacks,
   pastes nothing and says why. */
static void invalid_clipboard()
{
    selection_fixture f = fixture_new(SCENE);
    const char *unknown =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Other\")),\n"
        "            data.Field.Named(\"entities\", data.List([\n"
        "                data.Object([\n"
        "                    data.Field.Named(\"name\", "
        "data.Value.Text(\"Ghost\")),\n"
        "                    data.Field.Named(\"prototype\", "
        "data.Value.Text(\"Missing\"))\n"
        "                ])\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    bool text = scene_paste(f.scene, "hello { world", (scene_paste_place){ 0 }) < 0 &&
                strstr(f.scene->message, "no objects");
    bool empty = scene_paste(f.scene, NULL, (scene_paste_place){ 0 }) < 0;
    bool missing = scene_paste(f.scene, unknown, (scene_paste_place){ 0 }) < 0 &&
                   strstr(f.scene->message, "Missing");
    bool untouched = acount(f.scene->actors) == 3 &&
                     !scene_history_can_undo(f.scene->history);
    test_check(text && empty && missing && untouched, "invalid clipboard text pastes nothing and reports why");
    fixture_free(&f);
}

/* A field written to several selected objects records one Undo step. */
static void multi_field_is_one_step()
{
    selection_fixture f = fixture_new(SCENE);
    ecs_component_id appearance = GAME_COMPONENT_GameAppearance.id;
    int width = 0;
    while (strcmp(GAME_COMPONENT_GameAppearance.type->fields[width].name, "width")) {
        width++;
    }
    ecs_entity a = f.scene->actors[0].entity, c = f.scene->actors[2].entity;
    pin_value before_a = { .type = PIN_F32, .f = 20 },
              before_c = { .type = PIN_F32, .f = 44 };
    pin_value after = { .type = PIN_F32, .f = 30 };
    f.scene->property_actor = a;
    f.scene->property_edit = (scene_field_edit){ .component = appearance,
                                                 .field = width };
    assert(scene_property_value_copy(&f.scene->property_edit.before, before_a));
    assert(scene_property_value_copy(&f.scene->property_edit.after, after));
    scene_field_target other = { .entity = c };
    assert(scene_property_value_copy(&other.before, before_c));
    apush(f.scene->property_others, other);
    ecs_component_write_field(f.world, a, appearance, width, after);
    ecs_component_write_field(f.world, c, appearance, width, after);
    finish_property_edit(f.scene);
    bool written = GAME_GameAppearance_get_const(f.world, a)->width == 30 &&
                   GAME_GameAppearance_get_const(f.world, c)->width == 30;
    bool undone = replay_scene_edit(f.scene, false) == 0 &&
                  GAME_GameAppearance_get_const(f.world, a)->width == 20 &&
                  GAME_GameAppearance_get_const(f.world, c)->width == 44 &&
                  !scene_history_can_undo(f.scene->history);
    bool reselected = selection_is(&f, "AC", 0) || selection_is(&f, "AC", 2);
    test_check(written && undone && reselected, "a field written to several selected objects is one undo step, which leaves them selected");
    fixture_free(&f);
}

void test_scene_selection()
{
    toggle_and_range();
    duplicate_is_one_step();
    undo_restores_selection();
    clipboard_round_trip();
    invalid_clipboard();
    multi_field_is_one_step();
}
