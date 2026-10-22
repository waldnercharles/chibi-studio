#include "assets/scene_document.h"
#include "authoring/engine_preview.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/scene_internal.h"
#include "graph/authoring/graph_edit.h"
#include "graph/document/serialize_internal.h"
#include "graph/model/registry.h"
#include "runtime/component_catalog.h"
#include "runtime/save.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/asset_json.h"
#include "support/text_file.h"
#include "types/value_document.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Undo and Redo for the open document, as Studio's Edit menu applies them. */
static int history(app_scene_authoring *scene, bool redo)
{
    if (scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE) {
        return redo ? prototype_redo(scene) : prototype_undo(scene);
    }
    return replay_scene_edit(scene, redo);
}

static void prototype_component_gain(app_scene_authoring *scene, ecs_world *world)
{
    assert(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"ComponentGain\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"GameTransform\", "
            "data.Object([\n"
            "                    data.Field.Named(\"x\", data.Value.Int(0)),\n"
            "                    data.Field.Named(\"y\", data.Value.Int(0))\n"
            "                ]))\n"
            "            ])),\n"
            "            data.Field.Named(\"behaviors\", data.List([]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(app_scene_authoring_add_instance(scene, "ComponentGain") == 0);
    ecs_entity entity = app_scene_authoring_runtime_state(scene).selected_entity;
    assert(!GAME_GameHealth_has(world, entity));
    assert(app_scene_authoring_add_prototype_component(scene, "ComponentGain", "GameHealth") == 0);
    assert(GAME_GameHealth_has(world, entity));
    assert(app_scene_authoring_open_prototype(scene, "ComponentGain") == 0);
    assert(history(scene, false) == 0);
    assert(!GAME_GameHealth_has(world, entity));
    assert(history(scene, true) == 0);
    assert(GAME_GameHealth_has(world, entity));
    puts("prototype components PASS: an added prototype component reaches instances and follows Undo and Redo");
}

static ecs_entity spawn_registered_child(game_preview_world *live, ecs_entity owner)
{
    assert(
        game_behavior_dispatch(live->runtime->behaviors, owner, "SpawnRegistered", (flow_inputs){ 0 }, NULL) ==
        1
    );
    assert(game_entities_flush_initializers(live->runtime->entities, 0) == 0);
    ecs_entity *spawned = NULL;
    game_entities_copy_live(live->runtime->entities, &spawned);
    assert(acount(spawned) > 0);
    ecs_entity entity = spawned[acount(spawned) - 1];
    afree(spawned);
    return entity;
}

static void prototype_spawn_refresh(app_scene_authoring *scene, game_preview_world *live, ecs_world *world)
{
    behavior_asset *asset = behavior_asset_new("PrototypeRefreshSpawner");
    assert(behavior_asset_set_owner_type(asset, "ecs.entity") == 0);
    graph *g = asset->graph;
    node *spawn = graph_add_node(g, registry_node_by_name("Spawn Prototype"));
    assert(
        spawn &&
        graph_set_pin_default(g, spawn, 0, 1, &(pin_value){ .type = PIN_NAME, .str = "RuntimeRefreshChild" }) ==
            0
    );
    const char *events[] = { "Begin", "SpawnRegistered" };
    for (int i = 0; i < 2; i++) {
        node *event = graph_add_node(g, registry_node_by_name("Event"));
        assert(event && graph_set_event_name(g, event, events[i]) == 0);
        assert(graph_connect_exec(g, event, 0, spawn, 0, 0) == 0);
    }
    assert(
        game_preview_add_definition(live, game_preview_definition_new("PrototypeRefreshSpawner", asset)) == 0
    );
    behavior_asset *child_behavior = behavior_asset_new("RefreshedChild");
    assert(behavior_asset_set_owner_type(child_behavior, "ecs.entity") == 0);
    assert(
        game_preview_add_definition(live, game_preview_definition_new("RefreshedChild", child_behavior)) == 0
    );
    assert(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"RuntimeRefreshChild\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"GameHealth\", data.Object([\n"
            "                    data.Field.Named(\"current\", "
            "data.Value.Int(73)),\n"
            "                    data.Field.Named(\"maximum\", "
            "data.Value.Int(100))\n"
            "                ]))\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"RuntimeRefreshSpawner\")),\n"
            "            data.Field.Named(\"components\", data.Object([])),\n"
            "            data.Field.Named(\"behaviors\", data.List([\n"
            "                data.Value.Text(\"PrototypeRefreshSpawner\")\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    ecs_entity owner = ecs_world_create_entity(world);
    assert(game_behavior_attach(live->runtime->behaviors, owner, "PrototypeRefreshSpawner") == 0);
    ecs_entity original = spawn_registered_child(live, owner);
    assert(GAME_GameHealth_get_const(world, original)->current == 73);
    for (int pass = 0; pass < 6; pass++) {
        int expected = pass == 0 || pass == 2 ? 73 : 19;
        if (pass == 1) {
            assert(
                app_scene_authoring_set_prototype_field(
                    scene,
                    "RuntimeRefreshChild",
                    "GameHealth",
                    "current",
                    (pin_value){ .type = PIN_I32, .i = expected }
                ) == 0
            );
        }
        if (pass == 2) {
            assert(app_scene_authoring_open_prototype(scene, "RuntimeRefreshChild") == 0);
            assert(history(scene, false) == 0);
        }
        if (pass == 3) {
            assert(app_scene_authoring_open_prototype(scene, "RuntimeRefreshChild") == 0);
            assert(history(scene, true) == 0);
        }
        if (pass == 4) {
            assert(
                app_scene_authoring_attach_prototype_behavior(scene, "RuntimeRefreshChild", "RefreshedChild") ==
                0
            );
        }
        if (pass == 5) {
            assert(
                app_scene_authoring_detach_prototype_behavior(scene, "RuntimeRefreshChild", "RefreshedChild") ==
                0
            );
        }
        /* Spawn before any editor frame: mutations update the runtime snapshot immediately. */
        ecs_entity child = spawn_registered_child(live, owner);
        assert(GAME_GameHealth_get_const(world, child)->current == expected);
        assert(
            (game_behavior_instance(live->runtime->behaviors, child, "RefreshedChild") !=
             NULL) == (pass == 4)
        );
        assert(GAME_GameHealth_get_const(world, original)->current == 73);
        assert(app_scene_authoring_preview_prototype(scene, "RuntimeRefreshSpawner") == 0);
        assert(app_scene_authoring_frame(scene, (double)pass) == FLOW_OK);
        const ecs_world *preview = app_scene_authoring_preview_world(scene);
        ecs_component_id health = GAME_COMPONENT_GameHealth.id;
        ecs_query query;
        assert(ecs_query_begin((ecs_world *)preview, &query, &health, 1) == ECS_OK);
        assert(ecs_query_next(&query, &child));
        assert(GAME_GameHealth_get_const(preview, child)->current == expected);
        assert(!ecs_query_next(&query, &child));
        ecs_query_end(&query);
        app_scene_authoring_stop_prototype_preview(scene);
    }
    assert(game_behavior_destroy_entity(live->runtime->behaviors, owner) == 0);
    game_entities_reset(live->runtime->entities);
    puts("prototype spawn refresh PASS: initial registration, edits, Undo/Redo and behavior attachment changes reach the next scene/preview spawn without changing earlier instances");
}

static void duplicate_instance_preserves_edits(app_scene_authoring *scene, ecs_world *world)
{
    assert(app_scene_authoring_add_instance(scene, "First") == 0);
    ecs_entity original = app_scene_authoring_runtime_state(scene).selected_entity;
    GAME_GameTransform_get(world, original)->x = 123;
    GAME_GameHealth_get(world, original)->current = 42;
    assert(app_scene_authoring_duplicate_instance(scene) == 0);
    ecs_entity copy = app_scene_authoring_runtime_state(scene).selected_entity;
    assert(ecs_entity_to_u64(copy) != ecs_entity_to_u64(original));
    assert(GAME_GameTransform_get_const(world, copy)->x == 123);
    assert(GAME_GameHealth_get_const(world, copy)->current == 42);
    assert(history(scene, false) == 0);
    assert(!ecs_entity_is_alive(world, copy));
    assert(ecs_entity_is_alive(world, original));
    assert(history(scene, true) == 0);
    copy = app_scene_authoring_runtime_state(scene).selected_entity;
    assert(GAME_GameHealth_get_const(world, copy)->current == 42);
    GAME_GameHealth_get(world, copy)->current = 10;
    assert(GAME_GameHealth_get_const(world, original)->current == 42);
}

static void prototype_vectors_and_text(app_scene_authoring *scene, ecs_world *world)
{
    const char *
        json = "package asset\n"
               "\n"
               "import data \"game:assets\"\n"
               "\n"
               "pub Build :: () -> byte[] {\n"
               "    return data.Encode(\n"
               "        data.Object([\n"
               "            data.Field.Named(\"name\", "
               "data.Value.Text(\"GuideTest\")),\n"
               "            data.Field.Named(\"components\", data.Object([\n"
               "                data.Field.Named(\"GameTransform\", "
               "data.Object([])),\n"
               "                data.Field.Named(\"GameAim\", "
               "data.Object([])),\n"
               "                data.Field.Named(\"GameUi\", data.Object([]))\n"
               "            ])),\n"
               "            data.Field.Named(\"behaviors\", data.List([]))\n"
               "        ])\n"
               "    )\n"
               "}\n";
    assert(app_scene_authoring_register_prototype(scene, json, NULL) == 0);
    assert(app_scene_authoring_add_instance(scene, "GuideTest") == 0);
    ecs_entity entity = app_scene_authoring_runtime_state(scene).selected_entity;
    CF_V2 direction = { 0.5f, 1.0f };
    assert(
        app_scene_authoring_set_prototype_field(
            scene,
            "GuideTest",
            "GameAim",
            "direction",
            (pin_value){ .type = PIN_STRUCT, .type_name = "CF_V2", .ptr = &direction }
        ) == 0
    );
    assert(GAME_GameAim_get_const(world, entity)->direction.x == 0.5f);
    assert(app_scene_authoring_open_prototype(scene, "GuideTest") == 0);
    assert(history(scene, false) == 0);
    assert(GAME_GameAim_get_const(world, entity)->direction.x == 0);
    assert(history(scene, true) == 0);
    assert(GAME_GameAim_get_const(world, entity)->direction.y == 1);
    char text[] = "Guide";
    assert(
        app_scene_authoring_set_prototype_field(
            scene,
            "GuideTest",
            "GameUi",
            "document",
            (pin_value){ .type = PIN_STR, .str = text }
        ) == 0
    );
    text[0] = 'X';
    assert(!strcmp(GAME_GameUi_get_const(world, entity)->document, "Guide"));

    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    scene_actor *actor = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            actor = &scene->actors[i];
            break;
        }
    }
    assert(actor);
    CF_V2 replayed = { -4.5f, 7.25f };
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "property.set", &doc);
    yyjson_mut_obj_add_uint(doc, root, "actor", actor->identity);
    yyjson_mut_obj_add_strcpy(doc, root, "component", "GameAim");
    yyjson_mut_obj_add_strcpy(doc, root, "field", "direction");
    av_doc *value_doc = av_doc_new();
    av_value *value = pin_value_to_document(
        value_doc,
        &(pin_value){ .type = PIN_STRUCT, .type_name = "CF_V2", .ptr = &replayed }
    );
    yyjson_mut_obj_add_val(doc, root, "value", av_to_json(doc, value));
    av_doc_free(value_doc);
    char *action = scene_action_write(doc);
    char error[256] = { 0 };
    assert(action && scene_collab_apply_action(scene, action, error, sizeof error));
    free(action);
    assert(GAME_GameAim_get_const(world, entity)->direction.x == replayed.x);
    assert(GAME_GameAim_get_const(world, entity)->direction.y == replayed.y);
    root = scene_action_root(scene, "property.set", &doc);
    yyjson_mut_obj_add_uint(doc, root, "actor", actor->identity);
    yyjson_mut_obj_add_strcpy(doc, root, "component", "GameAim");
    yyjson_mut_obj_add_strcpy(doc, root, "field", "direction");
    yyjson_mut_val *invalid = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "value", invalid);
    yyjson_mut_obj_add_real(doc, invalid, "x", 1e39);
    yyjson_mut_obj_add_real(doc, invalid, "y", 2);
    action = scene_action_write(doc);
    assert(action && !scene_collab_apply_action(scene, action, error, sizeof error));
    free(action);
    assert(GAME_GameAim_get_const(world, entity)->direction.x == replayed.x);
    assert(GAME_GameAim_get_const(world, entity)->direction.y == replayed.y);
    puts("prototype and scene vectors PASS: prototype Undo/Redo, collab replay, and owned text");
}

/* The file at `path`, which `canonical` turns into the text
   chibi_ctl --format writes for its authored module. */
static char *saved_canonically(const char *path, char *(*canonical)(const char *json))
{
    char *text = text_file_read(path);
    assert(text);
    char *json = canonical(text);
    char error[256];
    char *disk = asset_document_to_disk(json, error, sizeof error);
    assert(disk && !strcmp(disk, text));
    free(json);
    free(disk);
    return text;
}

static char *canonical_scene(const char *text)
{
    char error[256];
    game_scene_document *document = game_scene_document_load(text, error, sizeof error);
    assert(document);
    char *json = game_scene_document_save(
        document,
        (ecs_component_catalog){ GAME_REGISTRY_COMPONENTS, GAME_REGISTRY_COMPONENT_COUNT }
    );
    game_scene_document_free(document);
    return json;
}

static char *canonical_prototype(const char *text)
{
    game_prototype_registry registry = app_component_registry();
    char error[256];
    game_prototype *prototype = game_prototype_load(text, &registry, error, sizeof error);
    assert(prototype);
    char *json = game_prototype_save(prototype);
    game_prototype_free(prototype);
    return json;
}

static const char *actor_name(app_scene_authoring *scene, ecs_entity entity)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            return scene->actors[i].name;
        }
    }
    return NULL;
}

/* A rename is one Scene Undo step, refuses a name in use or an empty one,
   survives a history checkpoint, and saves as the entity's name. */
static void rename_instance(app_scene_authoring *scene, const char *folder)
{
    assert(app_scene_authoring_add_instance(scene, "First") == 0);
    ecs_entity other = app_scene_authoring_runtime_state(scene).selected_entity;
    assert(app_scene_authoring_add_instance(scene, "First") == 0);
    ecs_entity entity = app_scene_authoring_runtime_state(scene).selected_entity;
    uint64_t identity = scene_actor_identity(scene, entity);
    assert(identity);
    char *before = strdup(actor_name(scene, entity));
    scene->scene_dirty = false;
    assert(app_scene_authoring_rename_instance(scene, entity, "Hero") == 0);
    assert(!strcmp(actor_name(scene, entity), "Hero") && scene->scene_dirty);
    assert(app_scene_authoring_rename_instance(scene, other, "Hero") == -1);
    assert(strstr(scene->message, "Hero") && strcmp(actor_name(scene, other), "Hero"));
    assert(app_scene_authoring_rename_instance(scene, other, "") == -1);
    /* The same name changes nothing and records nothing. */
    assert(app_scene_authoring_rename_instance(scene, entity, "Hero") == 0);
    uint64_t id = ecs_entity_to_u64(entity);
    char *checkpoint = scene_history_checkpoint(scene->history);
    scene_history *restored = scene_history_new();
    assert(scene_history_restore_checkpoint(restored, checkpoint, &id, &id, 1));
    ecs_entity at;
    const char *undone = scene_history_peek_rename(restored, false, &at);
    assert(undone && !strcmp(undone, before) && ecs_entity_to_u64(at) == id);
    scene_history_free(restored);
    free(checkpoint);
    assert(history(scene, false) == 0);
    assert(!strcmp(actor_name(scene, entity), before));
    assert(scene_actor_identity(scene, entity) == identity);
    assert(history(scene, true) == 0);
    assert(!strcmp(actor_name(scene, entity), "Hero"));
    assert(scene_actor_identity(scene, entity) == identity);
    char path[1024];
    snprintf(path, sizeof path, "%s/Renamed.chibi", folder);
    assert(app_scene_authoring_save(scene, path) == 0);
    char *text = saved_canonically(path, canonical_scene);
    char error[256];
    game_scene_document *document = game_scene_document_load(text, error, sizeof error);
    assert(document);
    int renamed = game_scene_document_entity_index(document, "Hero");
    assert(
        renamed >= 0 && !strcmp(document->entities[renamed].prototype, "First") &&
        game_scene_document_entity_index(document, before) < 0
    );
    assert(scene_actor_identity(scene, entity) == identity);
    game_scene_document_free(document);
    free(text);
    free(before);
    puts("rename instance PASS: a rename is one Undo step, refuses names in use, survives checkpoints and saves");
}

/* Whether the object keeps its values of a component its prototype lost. */
static bool retains(app_scene_authoring *scene, ecs_entity entity, const char *component)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        const scene_actor *actor = &scene->actors[i];
        for (int j = 0; ecs_entity_to_u64(actor->entity) == ecs_entity_to_u64(entity) &&
                        j < acount(actor->retained);
             j++) {
            if (!strcmp(actor->retained[j].name, component)) {
                return true;
            }
        }
    }
    return false;
}

static bool has_component(app_scene_authoring *scene, const char *prototype, ecs_component_id id)
{
    return prototype_has_component_id(find_prototype(scene, prototype)->prototype, id);
}

/* Adding and removing a prototype's component is one step of its Undo each;
   an instance's override of a removed component returns with it, and the
   prototype saves in canonical form. */
static void prototype_component_removal(app_scene_authoring *scene, ecs_world *world, const char *folder)
{
    assert(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Removable\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"GameTransform\", "
            "data.Object([\n"
            "                    data.Field.Named(\"x\", data.Value.Int(0)),\n"
            "                    data.Field.Named(\"y\", data.Value.Int(0))\n"
            "                ])),\n"
            "                data.Field.Named(\"GameHealth\", data.Object([\n"
            "                    data.Field.Named(\"current\", "
            "data.Value.Int(73)),\n"
            "                    data.Field.Named(\"maximum\", "
            "data.Value.Int(100))\n"
            "                ]))\n"
            "            ])),\n"
            "            data.Field.Named(\"behaviors\", data.List([]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    assert(app_scene_authoring_add_instance(scene, "Removable") == 0);
    ecs_entity entity = app_scene_authoring_runtime_state(scene).selected_entity;
    GAME_GameHealth_get(world, entity)->current = 42;
    scene_prototype_asset *asset = find_prototype(scene, "Removable");
    int steps = acount(asset->undo);
    assert(app_scene_authoring_add_prototype_component(scene, "Removable", "GameHealth") == -1);
    assert(app_scene_authoring_remove_prototype_component(scene, "Removable", "GameAim") == -1);
    assert(acount(asset->undo) == steps);
    assert(app_scene_authoring_remove_prototype_component(scene, "Removable", "GameHealth") == 0);
    assert(acount(asset->undo) == steps + 1 && asset->dirty);
    assert(!has_component(scene, "Removable", GAME_COMPONENT_GameHealth.id));
    assert(!GAME_GameHealth_has(world, entity) && retains(scene, entity, "GameHealth"));
    assert(app_scene_authoring_add_prototype_component(scene, "Removable", "GameAim") == 0);
    assert(acount(asset->undo) == steps + 2 && GAME_GameAim_has(world, entity));
    assert(app_scene_authoring_open_prototype(scene, "Removable") == 0);
    assert(history(scene, false) == 0);
    /* The prototype's own values leave nothing behind in the scene. */
    assert(!GAME_GameAim_has(world, entity) && !retains(scene, entity, "GameAim"));
    assert(history(scene, false) == 0);
    assert(has_component(scene, "Removable", GAME_COMPONENT_GameHealth.id));
    assert(GAME_GameHealth_get_const(world, entity)->current == 42);
    assert(!retains(scene, entity, "GameHealth"));
    assert(history(scene, true) == 0);
    assert(!GAME_GameHealth_has(world, entity));
    char path[1024];
    snprintf(path, sizeof path, "%s/Removable.chibi", folder);
    assert(app_scene_authoring_save_prototype(scene, "Removable", path) == 0);
    char *text = saved_canonically(path, canonical_prototype);
    assert(!strstr(text, "GameHealth") && strstr(text, "GameTransform"));
    free(text);
    assert(history(scene, true) == 0);
    assert(GAME_GameAim_has(world, entity));
    assert(app_scene_authoring_save_prototype(scene, "Removable", path) == 0);
    text = saved_canonically(path, canonical_prototype);
    assert(strstr(text, "GameAim"));
    free(text);
    puts("prototype component removal PASS: add and remove are one Undo step each, instances keep their overrides, and the prototype saves canonically");
}

static float position_x(ecs_world *world, const scene_actor *actor)
{
    return GAME_GameTransform_get_const(world, actor->entity)->x;
}

static int health(ecs_world *world, const scene_actor *actor)
{
    return GAME_GameHealth_get_const(world, actor->entity)->current;
}

static void write_health(app_scene_authoring *scene, const scene_actor *actor, int current)
{
    pin_value value = { .type = PIN_I32, .i = current };
    assert(
        ecs_component_write_field(
            scene->world,
            actor->entity,
            GAME_COMPONENT_GameHealth.id,
            0,
            value
        ) == ECS_OK
    );
}

/* While the scene runs, moves and writes change only its world: no Undo step
   and no unsaved mark. Keep Changes waits for Stop, which applies the values
   each object's latest record decides as one Undo step. */
static void live_edits_keep_changes(app_scene_authoring *scene, ecs_world *world)
{
    assert(app_scene_authoring_add_instance(scene, "First") == 0);
    int index = acount(scene->actors) - 1;
    select_instance(scene, index);
    float authored_x = position_x(world, &scene->actors[index]);
    int authored_health = health(world, &scene->actors[index]);
    scene_history_clear(scene->history);
    assert(
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0 &&
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PAUSE) == 0
    );
    scene_actor *actor = &scene->actors[index];
    scene->scene_dirty = false;
    GameTransform moved = *GAME_GameTransform_get_const(world, actor->entity);
    moved.x = authored_x + 9;
    begin_transform_edit(scene, actor->entity);
    write_transform_edit(scene, actor->entity, moved);
    finish_transform_edit(scene, false);
    write_health(scene, actor, 50);
    assert(position_x(world, actor) == moved.x && health(world, actor) == 50);
    assert(!scene->scene_dirty && !scene_history_can_undo(scene->history));
    assert(scene_keep_changes(scene, NULL) == 2);
    /* Undo in its toast forgets the latest record only. */
    write_health(scene, actor, 40);
    assert(scene_keep_changes(scene, &GAME_COMPONENT_GameHealth) == 1);
    scene_kept_drop(scene);
    /* A later record of one component decides that component's values. */
    write_health(scene, actor, 30);
    assert(scene_keep_changes(scene, &GAME_COMPONENT_GameHealth) == 1);
    int objects;
    const char *name;
    assert(scene_kept_values(scene, &objects, &name) == 2);
    assert(objects == 1 && !strcmp(name, actor->name));
    assert(!scene->scene_dirty);
    assert(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    actor = &scene->actors[index];
    assert(position_x(world, actor) == moved.x && health(world, actor) == 30);
    assert(scene->scene_dirty && !acount(scene->kept));
    assert(history(scene, false) == 0);
    assert(position_x(world, actor) == authored_x && health(world, actor) == authored_health);
    assert(!scene_history_can_undo(scene->history));
    assert(history(scene, true) == 0);
    assert(position_x(world, actor) == moved.x && health(world, actor) == 30);
    puts("live edits PASS: running edits leave the scene clean, and Keep Changes applies at Stop as one Undo step");
}

static int actor_named(app_scene_authoring *scene, const char *name)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (!strcmp(scene->actors[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static bool same_entity(ecs_entity a, ecs_entity b)
{
    return ecs_entity_to_u64(a) == ecs_entity_to_u64(b);
}

/* Hide and lock are the editor's view of a scene: picks leave hidden and
   locked objects out and draws hidden ones; the flags follow a rename, its
   Undo, a Save As and a reload by name, a delete forgets them, and neither
   the scene, its Undo nor its file holds them. */
static void view_state(app_scene_authoring *scene, const char *folder)
{
    const char *pickable =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"Pickable\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"GameAppearance\", data.Object([\n"
        "                    data.Field.Named(\"height\", "
        "data.Value.Int(20)),\n"
        "                    data.Field.Named(\"width\", data.Value.Int(20))\n"
        "                ])),\n"
        "                data.Field.Named(\"GameTransform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.Int(0)),\n"
        "                    data.Field.Named(\"y\", data.Value.Int(0))\n"
        "                ]))\n"
        "            ])),\n"
        "            data.Field.Named(\"behaviors\", data.List([]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    assert(app_scene_authoring_register_prototype(scene, pickable, NULL) == 0);
    assert(app_scene_authoring_add_instance(scene, "Pickable") == 0);
    int below = scene->selected;
    assert(app_scene_authoring_add_instance(scene, "Pickable") == 0);
    int above = scene->selected;
    ecs_entity under = scene->actors[below].entity, top = scene->actors[above].entity;
    char shown[1024], path[1024];
    snprintf(shown, sizeof shown, "%s/Shown.chibi", folder);
    snprintf(path, sizeof path, "%s/View.chibi", folder);
    /* A file names the scene after the file it last saved to, so the second
       save names it Shown, as the copy saved as View will be. */
    assert(app_scene_authoring_save(scene, shown) == 0);
    assert(app_scene_authoring_save(scene, shown) == 0);
    assert(same_entity(pick_scene_entity(scene, 0, 0, (render_skip){ 0 }), top));
    char *before = scene_history_checkpoint(scene->history);
    app_scene_authoring_set_actor_view(scene, above, APP_SCENE_VIEW_HIDDEN);
    app_scene_authoring_set_actor_view(scene, below, APP_SCENE_VIEW_LOCKED);
    render_skip hidden = scene_view_skip(scene, APP_SCENE_VIEW_HIDDEN, &scene->view_hidden);
    assert(hidden.count == 1 && render_skip_has(hidden, top));
    unsigned unpickable = APP_SCENE_VIEW_HIDDEN | APP_SCENE_VIEW_LOCKED;
    render_skip skip = scene_view_skip(scene, unpickable, &scene->view_unpickable);
    assert(!pick_scene_entity(scene, 0, 0, skip).slot);
    app_scene_authoring_set_actor_view(scene, below, 0);
    skip = scene_view_skip(scene, unpickable, &scene->view_unpickable);
    assert(same_entity(pick_scene_entity(scene, 0, 0, skip), under));
    char *after = scene_history_checkpoint(scene->history);
    assert(!scene->scene_dirty && !strcmp(before, after));
    free(before);
    free(after);
    assert(app_scene_authoring_rename_instance(scene, top, "Ghost") == 0);
    assert(app_scene_authoring_actor_view(scene, above) == APP_SCENE_VIEW_HIDDEN);
    assert(history(scene, false) == 0);
    assert(app_scene_authoring_actor_view(scene, above) == APP_SCENE_VIEW_HIDDEN);
    char *name = strdup(scene->actors[above].name);
    assert(app_scene_authoring_save(scene, path) == 0);
    char *saved = text_file_read(path), *plain = text_file_read(shown);
    assert(saved && plain && !strcmp(saved, plain));
    free(saved);
    free(plain);
    game_save *store = app_scene_authoring_view_store(scene);
    assert(game_save_get(store, path) && game_save_get(store, shown));
    assert(app_scene_authoring_load(scene, path) == 0);
    int reloaded = actor_named(scene, name);
    assert(reloaded >= 0);
    assert(app_scene_authoring_actor_view(scene, reloaded) == APP_SCENE_VIEW_HIDDEN);
    select_instance(scene, reloaded);
    assert(app_scene_authoring_delete_instance(scene) == 0);
    assert(!game_save_get(store, path));
    free(name);
    puts("view state PASS: hidden and locked objects leave picks, follow renames, Undo, Save As and reloads by name, and never reach the scene or its Undo");
}

static int maximum(ecs_world *world, const scene_actor *actor)
{
    return GAME_GameHealth_get_const(world, actor->entity)->maximum;
}

static int prototype_current(app_scene_authoring *scene, const char *prototype)
{
    pin_value value = { 0 };
    assert(
        app_scene_authoring_read_prototype_field(scene, prototype, "GameHealth", "current", &value) == 0
    );
    return value.i;
}

static const char *const APPLIED =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Applied\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"GameTransform\", data.Object([\n"
    "                    data.Field.Named(\"x\", data.Value.Int(0)),\n"
    "                    data.Field.Named(\"y\", data.Value.Int(0))\n"
    "                ])),\n"
    "                data.Field.Named(\"GameHealth\", data.Object([\n"
    "                    data.Field.Named(\"current\", data.Value.Int(10)),\n"
    "                    data.Field.Named(\"maximum\", data.Value.Int(100))\n"
    "                ]))\n"
    "            ])),\n"
    "            data.Field.Named(\"behaviors\", data.List([]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

/* Apply Overrides to Prototype: a chosen field becomes the prototype's, the
   instances that inherit it follow, one that overrides it keeps its own, and
   one Undo puts the prototype and them back. */
static void apply_overrides(app_scene_authoring *scene, ecs_world *world)
{
    assert(app_scene_authoring_register_prototype(scene, APPLIED, NULL) == 0);
    int at[3];
    for (int i = 0; i < 3; i++) {
        assert(app_scene_authoring_add_instance(scene, "Applied") == 0);
        at[i] = scene->selected;
    }
    scene_actor *source = &scene->actors[at[0]];
    write_health(scene, source, 40);
    pin_value top = { .type = PIN_I32, .i = 120 };
    assert(
        ecs_component_write_field(
            world,
            source->entity,
            GAME_COMPONENT_GameHealth.id,
            1,
            top
        ) == ECS_OK
    );
    write_health(scene, &scene->actors[at[2]], 25);
    scene_history_clear(scene->history);
    game_prototype_field *fields = game_prototype_instance_overrides(
        source->base,
        scene->world,
        source->entity
    );
    assert(
        acount(fields) == 2 &&
        fields[0].component->id == GAME_COMPONENT_GameHealth.id
    );
    assert(scene_override_reach(scene, source, fields, 1) == 1);
    assert(scene_override_reach(scene, source, fields, 2) == 2);
    assert(scene_apply_overrides(scene, source, fields, 1) == 0);
    afree(fields);
    source = &scene->actors[at[0]];
    assert(prototype_current(scene, "Applied") == 40);
    assert(health(world, source) == 40 && health(world, &scene->actors[at[1]]) == 40);
    assert(health(world, &scene->actors[at[2]]) == 25 && maximum(world, source) == 120);
    fields = game_prototype_instance_overrides(
        source->base,
        scene->world,
        source->entity
    );
    assert(acount(fields) == 1 && fields[0].field == 1);
    afree(fields);
    assert(find_prototype(scene, "Applied")->dirty);
    assert(history(scene, false) == 0 && !scene_history_can_undo(scene->history));
    assert(prototype_current(scene, "Applied") == 10);
    assert(
        health(world, &scene->actors[at[0]]) == 40 &&
        health(world, &scene->actors[at[1]]) == 10
    );
    assert(health(world, &scene->actors[at[2]]) == 25);
    fields = game_prototype_instance_overrides(
        scene->actors[at[0]].base,
        scene->world,
        scene->actors[at[0]].entity
    );
    assert(acount(fields) == 2);
    afree(fields);
    assert(history(scene, true) == 0);
    assert(
        prototype_current(scene, "Applied") == 40 &&
        health(world, &scene->actors[at[1]]) == 40
    );
    /* Its own document's Undo takes it back too. */
    assert(app_scene_authoring_open_prototype(scene, "Applied") == 0);
    assert(history(scene, false) == 0 && prototype_current(scene, "Applied") == 10);
    scene_authoring_select_document(scene, APP_EDITOR_DOCUMENT_SCENE, APP_SCENE_DOCUMENT_ID);
    puts("apply overrides PASS: a chosen field becomes the prototype's, inheritors follow, overriders keep theirs, one Undo step");
}

/* Save as New Prototype: a free name saves the tuned instance as an asset
   and makes the instance one of it, with no overrides left; a taken or
   invalid name changes nothing; Undo makes it its old prototype's again and
   keeps the asset. */
static void save_as_prototype(app_scene_authoring *scene, ecs_world *world, const char *folder)
{
    assert(app_scene_authoring_set_content_root(scene, folder) == 0);
    assert(app_scene_authoring_add_instance(scene, "Applied") == 0);
    int index = scene->selected;
    write_health(scene, &scene->actors[index], 55);
    char *name = strdup(scene->actors[index].name);
    char error[192];
    int count = acount(scene->actors);
    assert(
        scene_save_as_prototype(scene, &scene->actors[index], "Applied", error, sizeof error) != 0
    );
    assert(strstr(error, "already exists"));
    assert(
        scene_save_as_prototype(scene, &scene->actors[index], "no good", error, sizeof error) != 0
    );
    assert(
        acount(scene->actors) == count &&
        !strcmp(scene->actors[index].prototype_name, "Applied")
    );
    scene_history_clear(scene->history);
    assert(scene_save_as_prototype(scene, &scene->actors[index], "Tuned", error, sizeof error) == 0);
    index = actor_named(scene, name);
    scene_actor *actor = &scene->actors[index];
    assert(!strcmp(actor->prototype_name, "Tuned") && health(world, actor) == 55);
    game_prototype_field *fields = game_prototype_instance_overrides(
        actor->base,
        scene->world,
        actor->entity
    );
    assert(!acount(fields));
    afree(fields);
    const ecs_component_desc *tag = game_prototype_tag_desc(
        find_prototype(scene, "Tuned")->prototype
    );
    assert(ecs_component_has(world, actor->entity, tag->id));
    assert(prototype_current(scene, "Tuned") == 55);
    char path[1024];
    snprintf(path, sizeof path, "%s/prototypes/Tuned.chibi", folder);
    assert(access(path, F_OK) == 0);
    assert(history(scene, false) == 0 && !scene_history_can_undo(scene->history));
    actor = &scene->actors[actor_named(scene, name)];
    assert(!strcmp(actor->prototype_name, "Applied") && health(world, actor) == 55);
    assert(!ecs_component_has(world, actor->entity, tag->id));
    assert(find_prototype(scene, "Tuned") && access(path, F_OK) == 0);
    assert(history(scene, true) == 0);
    assert(!strcmp(scene->actors[actor_named(scene, name)].prototype_name, "Tuned"));
    unlink(path);
    snprintf(path, sizeof path, "%s/prototypes", folder);
    rmdir(path);
    free(name);
    puts("save as prototype PASS: the tuned instance becomes an asset it is an instance of, names are checked, Undo relinks");
}

/* An asset dragged over an object shows in the field that names its kind
   for one draw; clearing puts the authored value back without an Undo step
   or an unsaved mark. */
static void field_preview(app_scene_authoring *scene, ecs_world *world)
{
    const char *shaded =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Shaded\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"GameShader\", data.Object([\n"
        "                    data.Field.Named(\"material\", "
        "data.Value.Text(\"Base\"))\n"
        "                ]))\n"
        "            ])),\n"
        "            data.Field.Named(\"behaviors\", data.List([]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    assert(app_scene_authoring_register_prototype(scene, shaded, NULL) == 0);
    assert(app_scene_authoring_add_instance(scene, "Shaded") == 0);
    ecs_entity entity = scene->actors[scene->selected].entity;
    ecs_component_id component;
    int field = scene_asset_field(scene, entity, REFERENCE_ASSET_MATERIAL, &component);
    assert(field >= 0 && component == GAME_COMPONENT_GameShader.id);
    assert(scene_asset_field(scene, entity, REFERENCE_ASSET_UI, &component) < 0);
    scene->scene_dirty = false;
    scene_history_clear(scene->history);
    scene_preview_field(scene, entity, GAME_COMPONENT_GameShader.id, field, "Glow");
    scene_preview_field(scene, entity, GAME_COMPONENT_GameShader.id, field, "Glass");
    scene_field_previews_apply(scene);
    assert(!strcmp(GAME_GameShader_get_const(world, entity)->material, "Glass"));
    scene_field_previews_clear(scene);
    assert(!strcmp(GAME_GameShader_get_const(world, entity)->material, "Base"));
    assert(
        !scene->scene_dirty && !scene_history_can_undo(scene->history) && !scene->field_previews
    );
    puts("field preview PASS: a dragged asset shows for one draw and leaves the authored value, history and save alone");
}

void test_scene_instances()
{
    ecs_world *world = ecs_world_new();
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    behavior_asset *asset = behavior_asset_new("SceneInstances");
    editor *e = editor_new(asset);
    game_preview_world *live = game_preview_world_new(world);
    game_preview_definition *definition = game_preview_definition_new(
        "SceneInstances",
        behavior_asset_new("SceneInstances")
    );
    assert(game_preview_add_definition(live, definition) == 0);
    engine *runtime = authoring_engine_new(world, live->runtime);
    app_scene_authoring *
        scene = app_scene_authoring_new(world, live, definition, asset, e, NULL, NULL, runtime);
    assert(scene);
    const char *first =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"version\", data.Value.Int(1)),\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"First\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"GameTransform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.Int(0)),\n"
        "                    data.Field.Named(\"y\", data.Value.Int(0))\n"
        "                ])),\n"
        "                data.Field.Named(\"GameHealth\", data.Object([\n"
        "                    data.Field.Named(\"current\", "
        "data.Value.Int(73)),\n"
        "                    data.Field.Named(\"maximum\", "
        "data.Value.Int(100))\n"
        "                ]))\n"
        "            ])),\n"
        "            data.Field.Named(\"behaviors\", data.List([]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    assert(app_scene_authoring_register_prototype(scene, first, NULL) == 0);
    prototype_component_gain(scene, world);
    prototype_spawn_refresh(scene, live, world);
    duplicate_instance_preserves_edits(scene, world);
    prototype_vectors_and_text(scene, world);
    live_edits_keep_changes(scene, world);
    char folder[] = "/tmp/chibi-scene-instances-XXXXXX";
    assert(mkdtemp(folder));
    rename_instance(scene, folder);
    view_state(scene, folder);
    prototype_component_removal(scene, world, folder);
    apply_overrides(scene, world);
    field_preview(scene, world);
    save_as_prototype(scene, world, folder);
    const char *const saved[] = { "Renamed.chibi", "Removable.chibi", "Shown.chibi", "View.chibi" };
    for (size_t i = 0; i < sizeof saved / sizeof *saved; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", folder, saved[i]);
        unlink(path);
    }
    rmdir(folder);
    app_scene_authoring_free(scene);
    engine_free(runtime);
    game_preview_world_free(live);
    editor_free(e);
    ecs_world_free(world);
}
