#include "authoring/engine_preview.h"
#include "editor/collab/actions_scene.h"
#include "runtime/component_catalog.h"
#include "runtime/transform_propagation.h"

/* A second world exercises checkpoint remapping independently of the open UI. */
typedef struct
{
    ecs_world *world;
    preview_world *live;
    engine *runtime;
    editor *graph;
    preview_definition *placeholder;
    app_scene_authoring *scene;
} move_fixture;

static move_fixture move_fixture_new_with_root(const char *content_root)
{
    move_fixture fixture = { .world = ecs_world_new() };
    fixture.live = preview_world_new(fixture.world);
    /* Match the app's source-backed placeholder. It is never registered as
       executable behavior; these fixtures only edit scene data. */
    const script_catalog catalog = { 0 };
    const script_lower_bindings bindings = { 0 };
    authoring_source *source = authoring_source_prepare_document(
        "MoveFixture.chibi",
        "behaviors/MoveFixture.chibi",
        "MoveFixture :: behavior {}",
        &catalog,
        &bindings,
        BEHAVIOR_BYTECODE
    );
    assert(source && source->valid);
    authoring_source_presentation
        *presentation = authoring_source_presentation_new(source->analysis, &catalog, NULL);
    assert(presentation);
    fixture.placeholder = preview_projection_new("MoveFixture", presentation);
    fixture.graph = editor_new(authoring_source_presentation_take_view(
        authoring_source_presentation_clone(presentation)
    ));
    int adopted = editor_source_presentation(fixture.graph, presentation);
    assert(adopted == 0);
    authoring_source_presentation_free(presentation);
    authoring_source_free(source);
    fixture.runtime = authoring_engine_new(fixture.world, fixture.live->runtime);
    fixture.scene = app_scene_authoring_new_with_content_root(
        fixture.world,
        fixture.live,
        fixture.placeholder,
        editor_behavior_asset(fixture.graph),
        fixture.graph,
        NULL,
        NULL,
        fixture.runtime,
        content_root
    );
    assert(fixture.scene);
    return fixture;
}

static move_fixture move_fixture_new()
{
    return move_fixture_new_with_root(project);
}

static void move_fixture_free(move_fixture *fixture)
{
    app_scene_authoring_free(fixture->scene);
    engine_free(fixture->runtime);
    preview_world_free(fixture->live);
    editor_free(fixture->graph);
    preview_definition_free(fixture->placeholder);
    ecs_world_free(fixture->world);
}

/* The prototype collection budget must not become every asset reader's budget.
 * One file, at most 65 MiB, and one owned read; no large document is parsed. */
static bool large_source_read_limits(void)
{
    char root[] = "/tmp/chibi-studio-large-read-XXXXXX", directory[256], path[256];
    REQUIRE(mkdtemp(root));
    snprintf(directory, sizeof directory, "%s/prototypes", root);
    REQUIRE(!mkdir(directory, 0700));
    snprintf(path, sizeof path, "%s/Large.chibi", directory);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    char chunk[8192];
    memset(chunk, ' ', sizeof chunk);
    size_t padding = (size_t)PROTOTYPE_READ_MAX + 1;
    for (size_t written = 0; written < padding;) {
        size_t bytes = padding - written;
        if (bytes > sizeof chunk) {
            bytes = sizeof chunk;
        }
        REQUIRE(fwrite(chunk, 1, bytes, file) == bytes);
        written += bytes;
    }
    const char *suffix = "Large :: prototype {}";
    REQUIRE(fputs(suffix, file) >= 0);
    REQUIRE(!fclose(file));
    char *text = read_scene_file(path);
    bool read = text && strlen(text) == padding + strlen(suffix) &&
                !strcmp(text + padding, suffix);
    free(text);
    move_fixture fixture = move_fixture_new_with_root(root);
    app_scene_authoring_refresh_content(fixture.scene);
    bool bounded = !scene_prototype_sources_refresh(fixture.scene) &&
                   strstr(fixture.scene->message, "read limits");
    move_fixture_free(&fixture);
    REQUIRE(!unlink(path));
    REQUIRE(!rmdir(directory));
    REQUIRE(!rmdir(root));
    REQUIRE(read);
    REQUIRE(bounded);
    return true;
}

/* Reach the same document/focus dispatch as the Undo command, not the buffer's
 * replay API. Static source tabs must not consume scene/prototype history. */
static bool source_history_reopens(
    asset_code *code,
    const av_value *checkpoint,
    const prototype_registry *registry,
    editor *graph,
    bool relationship,
    bool changed
)
{
    asset_code *restored = checkpoint ? asset_code_restore(checkpoint, registry) : NULL;
    bool valid = restored && asset_code_static(restored) &&
                 asset_code_validate(restored, graph) &&
                 !strcmp(asset_code_text(restored), asset_code_text(code));
    char error[256];
    if (valid && relationship) {
        ecs_relationship_desc *value = authoring_relationship_source_read(
            asset_code_text(restored),
            error,
            sizeof error
        );
        valid = value && ((value->traits & ECS_REL_EXCLUSIVE) != 0) == !changed;
        authoring_relationship_source_free(value);
    } else if (valid) {
        av_doc *document = authoring_prototype_document_read(
            asset_code_text(restored),
            registry,
            error,
            sizeof error
        );
        prototype *value = document
                             ? prototype_load_value(av_doc_root(document), registry, error, sizeof error)
                             : NULL;
        pin_value field;
        valid = value && !prototype_read_field(value, "Transform", "x", &field) &&
                field.type == PIN_F32 && field.f == (changed ? 2 : 1);
        prototype_free(value);
        av_doc_free(document);
    }
    asset_code_free(restored);
    return valid;
}

static bool static_source_dispatch_history(void)
{
    char root[] = "/tmp/chibi-studio-source-undo-XXXXXX", directory[256],
         path[256], error[256];
    REQUIRE(mkdtemp(root));
    const char *folders[] = { "relationships", "prototypes" };
    const char *names[] = { "UndoTargets", "UndoEnemy" };
    const char *before[] = { "UndoTargets :: relationship {exclusive:true}",
                             "UndoEnemy :: prototype {Transform {x:1,y:0}}" };
    const char *after[] = { "UndoTargets :: relationship {exclusive:false}",
                            "UndoEnemy :: prototype {Transform {x:2,y:0}}" };
    const app_editor_document_kind kinds[] = { APP_EDITOR_DOCUMENT_RELATIONSHIP,
                                               APP_EDITOR_DOCUMENT_PROTOTYPE };
    bool all_valid = true;
    for (int i = 0; i < 2; i++) {
        snprintf(directory, sizeof directory, "%s/%s", root, folders[i]);
        REQUIRE(!mkdir(directory, 0700));
        snprintf(path, sizeof path, "%s/%s.chibi", directory, names[i]);
        REQUIRE(!write_file_atomic(path, before[i]));
        move_fixture fixture = move_fixture_new_with_root(root);
        studio *s = xcalloc(1, sizeof *s);
        s->scene = fixture.scene;
        s->key_area = -1;
        s->focus_area = STUDIO_AREA_TOP;
        s->graph.area = -1;
        s->prefs.layouts[0].editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_CODE;
        s->prefs.code_apply_live = true;
        REQUIRE(!scene_tabs_open_source(fixture.scene, kinds[i], names[i], path));
        asset_code *code = asset_code_open(path, error, sizeof error);
        REQUIRE(code && asset_code_static(code));
        asset_code_set_registry(code, &fixture.scene->registry);
        apush(s->code.assets, code);
        REQUIRE(asset_code_replace(code, before[i], after[i]));
        REQUIRE(asset_code_validate(code, fixture.graph));
        bool undo_enabled = studio_can_replay(s, false);
        studio_history(s, false);
        bool undone = !strcmp(asset_code_text(code), before[i]) &&
                      !asset_code_dirty(code);
        undone &= source_history_reopens(
            code,
            fixture.scene->tabs[fixture.scene->active_tab].source_state,
            &fixture.scene->registry,
            fixture.graph,
            i == 0,
            false
        );
        bool redo_enabled = studio_can_replay(s, true);
        studio_history(s, true);
        bool redone = !strcmp(asset_code_text(code), after[i]) &&
                      asset_code_dirty(code);
        redone &= source_history_reopens(
            code,
            fixture.scene->tabs[fixture.scene->active_tab].source_state,
            &fixture.scene->registry,
            fixture.graph,
            i == 0,
            true
        );
        for (int j = 0; j < acount(s->code.assets); j++) {
            asset_code_free(s->code.assets[j]);
        }
        afree(s->code.assets);
        afree(s->records);
        free(s);
        move_fixture_free(&fixture);
        REQUIRE(!unlink(path));
        REQUIRE(!rmdir(directory));
        if (!undo_enabled || !undone || !redo_enabled || !redone) {
            fprintf(
                stderr,
                "Static source history %s: undo=%d/%d redo=%d/%d\n",
                names[i],
                undo_enabled,
                undone,
                redo_enabled,
                redone
            );
        }
        all_valid &= undo_enabled && undone && redo_enabled && redone;
    }
    REQUIRE(!rmdir(root));
    REQUIRE(all_valid);
    return true;
}

static bool static_relationship_refresh(void)
{
    char root[] = "/tmp/chibi-studio-relation-XXXXXX", directory[256],
         path[256], probe[256], error[256];
    REQUIRE(mkdtemp(root));
    move_fixture fixture = move_fixture_new_with_root(root);
    snprintf(probe, sizeof probe, "%s/RegistryProbe.chibi", root);
    REQUIRE(!write_file_atomic(probe, "RegistryProbe :: prototype { links:{AcceptanceTargets:[]} }"));
    asset_code *code = asset_code_open(probe, error, sizeof error);
    REQUIRE(code && asset_code_static(code));
    asset_code_set_registry(code, &fixture.scene->registry);
    REQUIRE(asset_code_error(code)[0]);
    snprintf(directory, sizeof directory, "%s/relationships", root);
    REQUIRE(!mkdir(directory, 0700));
    snprintf(path, sizeof path, "%s/AcceptanceTargets.chibi", directory);
    REQUIRE(!write_file_atomic(path, "AcceptanceTargets :: relationship { exclusive:true }"));
    app_scene_authoring_refresh_content(fixture.scene);
    REQUIRE(fixture.scene->registry.relationship_count == 1);
    asset_code_set_registry(code, &fixture.scene->registry);
    REQUIRE(!asset_code_error(code)[0]);
    REQUIRE(asset_code_validate(code, fixture.graph));
    const ecs_relationship_desc *schema = fixture.scene->registry.relationships[0];
    ecs_world *worlds[] = { fixture.world,
                            prototype_preview_world(fixture.scene->prototype_preview) };
    for (int i = 0; i < 2; i++) {
        const ecs_relationship_desc
            *live = ecs_world_relationship(worlds[i], schema->id);
        REQUIRE(
            live && (live->traits & ECS_REL_EXCLUSIVE) &&
            !strcmp(live->name, "AcceptanceTargets")
        );
    }
    REQUIRE(!write_file_atomic(path, "AcceptanceTargets :: relationship { exclusive:1 }"));
    app_scene_authoring_refresh_content(fixture.scene);
    REQUIRE(fixture.scene->registry.relationship_count == 1);
    REQUIRE(fixture.scene->registry.relationships[0] == schema);
    for (int i = 0; i < 2; i++) {
        const ecs_relationship_desc
            *live = ecs_world_relationship(worlds[i], schema->id);
        REQUIRE(live && (live->traits & ECS_REL_EXCLUSIVE));
    }
    asset_code_free(code);
    move_fixture_free(&fixture);
    REQUIRE(!unlink(probe));
    REQUIRE(!unlink(path));
    REQUIRE(!rmdir(directory));
    REQUIRE(!rmdir(root));
    return true;
}

static bool static_prototype_refresh(void)
{
    char root[] = "/tmp/chibi-studio-prototype-XXXXXX", directory[256], paths[4][256];
    REQUIRE(mkdtemp(root));
    move_fixture fixture = move_fixture_new_with_root(root);
    app_scene_authoring *scene = fixture.scene;
    snprintf(directory, sizeof directory, "%s/prototypes", root);
    REQUIRE(!mkdir(directory, 0700));
    const char *names[] = { "AcceptanceBase",
                            "AcceptanceMiddle",
                            "AcceptanceLeaf",
                            "AcceptanceNest" };
    const char *sources[] = {
        "AcceptanceBase :: prototype { Transform {x:1, y:0} }",
        "AcceptanceMiddle :: prototype(AcceptanceBase) {}",
        "AcceptanceLeaf :: prototype(AcceptanceMiddle) {}",
        "AcceptanceNest :: prototype { children:[{name:\"child\", "
        "prototype:AcceptanceBase}] }"
    };
    for (int i = 0; i < 4; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/%s.chibi", directory, names[i]);
        REQUIRE(!write_file_atomic(paths[i], sources[i]));
    }
    app_scene_authoring_refresh_content(scene);
    for (int i = 0; i < 4; i++) {
        REQUIRE(!app_scene_authoring_register_prototype(scene, sources[i], paths[i]));
    }
    pin_value value;
    REQUIRE(
        !app_scene_authoring_read_prototype_field(scene, names[2], "Transform", "x", &value)
    );
    REQUIRE(value.type == PIN_F32 && value.f == 1);
    REQUIRE(!write_file_atomic(paths[0], "AcceptanceBase :: prototype { Transform {x:2, y:0} }"));
    REQUIRE(app_scene_authoring_reload_path(scene, paths[0]) == APP_DISK_RELOADED);
    for (int i = 0; i < 3; i++) {
        REQUIRE(
            !app_scene_authoring_read_prototype_field(scene, names[i], "Transform", "x", &value)
        );
        REQUIRE(value.type == PIN_F32 && value.f == 2);
    }
    scene_prototype_asset *nest = find_prototype(scene, names[3]);
    REQUIRE(nest && acount(nest->prototype->child_prototypes) == 1);
    REQUIRE(
        !prototype_read_field(nest->prototype->child_prototypes[0], "Transform", "x", &value)
    );
    REQUIRE(value.type == PIN_F32 && value.f == 2);

    /* Dirty dependents retain their working values. Clean transitive users
     * still reload from the saved dependency graph. */
    scene_prototype_asset *middle = find_prototype(scene, names[1]);
    pin_value edited = { .type = PIN_F32, .f = 9 };
    REQUIRE(!write_prototype_field(scene, middle, "Transform", "x", edited));
    REQUIRE(middle->dirty);
    REQUIRE(!write_file_atomic(paths[0], "AcceptanceBase :: prototype { Transform {x:3, y:0} }"));
    REQUIRE(app_scene_authoring_reload_path(scene, paths[0]) == APP_DISK_UNSAVED);
    REQUIRE(
        !app_scene_authoring_read_prototype_field(scene, names[1], "Transform", "x", &value)
    );
    REQUIRE(value.f == 9 && middle->dirty);
    REQUIRE(
        !app_scene_authoring_read_prototype_field(scene, names[2], "Transform", "x", &value)
    );
    REQUIRE(value.f == 3);
    REQUIRE(strstr(app_scene_authoring_message(scene), "unsaved edits"));
    move_fixture_free(&fixture);
    for (int i = 0; i < 4; i++) {
        REQUIRE(!unlink(paths[i]));
    }
    REQUIRE(!rmdir(directory));
    REQUIRE(!rmdir(root));
    return true;
}

static bool move_same_entity(ecs_entity a, ecs_entity b)
{
    return ecs_entity_to_u64(a) == ecs_entity_to_u64(b);
}

static bool same_saved_scene(app_scene_authoring *scene, const char *expected)
{
    char *actual = scene_authoring_scene_text(scene);
    bool equal = actual && !strcmp(actual, expected);
    if (!equal) {
        fprintf(stderr, "Expected hierarchy:\n%s\nActual hierarchy:\n%s\n", expected, actual ? actual : "(none)");
    }
    free(actual);
    REQUIRE(equal);
    return true;
}

static bool same_world_pose(GlobalTransform2D a, GlobalTransform2D b)
{
    REQUIRE(fabsf(a.position.x - b.position.x) < .0001f);
    REQUIRE(fabsf(a.position.y - b.position.y) < .0001f);
    REQUIRE(fabsf(a.radians - b.radians) < .0001f);
    return true;
}

static char *move_action(
    app_scene_authoring *scene,
    const uint64_t *ids,
    int count,
    uint64_t active,
    uint64_t parent,
    uint64_t before
)
{
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "hierarchy.move", &doc);
    yyjson_mut_val *actors = yyjson_mut_obj_add_arr(doc, root, "actors");
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_uint(doc, actors, ids[i]);
    }
    yyjson_mut_obj_add_uint(doc, root, "active", active);
    yyjson_mut_obj_add_uint(doc, root, "parent", parent);
    yyjson_mut_obj_add_uint(doc, root, "before", before);
    return scene_action_write(doc);
}

static bool scene_move_contract()
{
    move_fixture source = move_fixture_new();
    app_scene_authoring *scene = source.scene;
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "name \"Mover\"\ncomponents { Transform x=0 y=0; "
            "Orientation radians=0; Joint; "
            "Appearance shape=\"Box\" width=4.0 height=4.0; }\n",
            NULL
        ) == 0
    );
    REQUIRE(
        scene_authoring_load_text(
            scene,
            "version 1\nname \"Scene\"\n(array)entities {\n"
            " - name=\"Left\" prototype=\"Mover\" { components { Transform "
            "x=100.0 y=0.0; "
            "Orientation radians=1.5707964; Joint other=\"Left.A\"; }; "
            "(array)children {\n"
            "  - name=\"A\" prototype=\"Mover\" { components { Transform "
            "x=10.0 y=0.0; }; }\n"
            "  - name=\"B\" prototype=\"Mover\" { components { Transform "
            "x=20.0 y=0.0; }; }\n"
            "  - name=\"Clash\" prototype=\"Mover\"\n } }\n"
            " - name=\"Right\" prototype=\"Mover\" { components { "
            "Transform x=200.0 y=0.0; "
            "Orientation radians=-1.5707964; Joint other=\"Right.A\"; "
            "}; (array)children { - "
            "name=\"Clash\" prototype=\"Mover\"; }; }\n"
            " - name=\"C\" prototype=\"Mover\"\n}\n",
            NULL
        ) == 0
    );
    REQUIRE(acount(scene->actors) == 7);
    ecs_entity left = scene->actors[0].entity, a = scene->actors[1].entity,
               b = scene->actors[2].entity, clash = scene->actors[3].entity,
               right = scene->actors[4].entity;
    uint64_t left_id = scene->actors[0].identity, right_id = scene->actors[4].identity;
    uint64_t selected[] = { scene->actors[1].identity, scene->actors[2].identity };
    REQUIRE(app_scene_authoring_select_entity(scene, a) == 0);
    scene->actors[2].marked = true;
    GlobalTransform2D a_world = transform_world_2d(scene->world, a);
    GlobalTransform2D b_world = transform_world_2d(scene->world, b);
    REQUIRE(move_same_entity(pick_scene_entity(scene, 100, -10, (render_skip){ 0 }), a));
    REQUIRE(Joint_get_const(scene->world, left)->other == ecs_entity_to_u64(a));
    REQUIRE(Joint_get_const(scene->world, right)->other == 0);
    char *initial = scene_authoring_scene_text(scene);
    char *initial_checkpoint = app_scene_authoring_checkpoint(scene);
    REQUIRE(initial && initial_checkpoint);
    move_fixture peer = move_fixture_new();
    /* Force remapped handles; the scene's stable actor identities must suffice. */
    REQUIRE(ecs_world_create_entity(peer.world).slot);
    REQUIRE(app_scene_authoring_restore_checkpoint(peer.scene, initial_checkpoint) == 0);
    free(initial_checkpoint);
    REQUIRE(app_scene_authoring_can_drop(scene, right, APP_SCENE_DROP_INSIDE));
    REQUIRE(app_scene_authoring_drop_selection(scene, right, APP_SCENE_DROP_INSIDE) == 0);
    uint64_t step = scene_history_step_id(scene->history, false);
    REQUIRE(
        step && scene_selection_count(scene) == 2 &&
        selected_actor(scene)->identity == selected[0]
    );
    REQUIRE(move_same_entity(ecs_parent(scene->world, a), right));
    REQUIRE(move_same_entity(ecs_parent(scene->world, b), right));
    /* Authored strings stay intact while their live handles follow the new paths. */
    REQUIRE(Joint_get_const(scene->world, left)->other == 0);
    REQUIRE(Joint_get_const(scene->world, right)->other == ecs_entity_to_u64(a));
    REQUIRE(same_world_pose(a_world, transform_world_2d(scene->world, a)));
    REQUIRE(same_world_pose(b_world, transform_world_2d(scene->world, b)));
    REQUIRE(move_same_entity(pick_scene_entity(scene, 100, -10, (render_skip){ 0 }), a));
    REQUIRE(
        scene->actors[4].identity == selected[0] &&
        scene->actors[5].identity == selected[1]
    );
    REQUIRE(move_same_entity(ecs_pair_source(scene->world, ECS_CHILD_OF, right, 1), a));
    REQUIRE(move_same_entity(ecs_pair_source(scene->world, ECS_CHILD_OF, right, 2), b));
    char *moved = scene_authoring_scene_text(scene);
    REQUIRE(moved && strcmp(initial, moved));
    REQUIRE(app_scene_authoring_drop_selection(scene, right, APP_SCENE_DROP_INSIDE) == 0);
    REQUIRE(scene_history_step_id(scene->history, false) == step);
    char *accepted = move_action(scene, selected, 2, selected[0], right_id, 0);
    char error[256];
    REQUIRE(scene_collab_apply_action(peer.scene, accepted, error, sizeof error));
    REQUIRE(same_saved_scene(peer.scene, moved));
    REQUIRE(replay_scene_edit(peer.scene, false) == 0);
    char *peer_history = scene_history_checkpoint(peer.scene->history);
    collab_start("move-fixture", "peer", "editor");
    collab_begin_replay();
    bool replayed = scene_collab_apply_action(peer.scene, accepted, error, sizeof error);
    collab_end_replay();
    collab_shutdown();
    REQUIRE(replayed);
    char *replayed_history = scene_history_checkpoint(peer.scene->history);
    REQUIRE(!strcmp(peer_history, replayed_history));
    free(peer_history);
    free(replayed_history);
    free(accepted);
    REQUIRE(same_saved_scene(peer.scene, moved));
    REQUIRE(
        scene_selection_count(peer.scene) == 2 &&
        selected_actor(peer.scene)->identity == selected[0]
    );
    REQUIRE(replay_scene_edit(scene, false) == 0);
    REQUIRE(same_saved_scene(scene, initial));
    REQUIRE(Joint_get_const(scene->world, left)->other == ecs_entity_to_u64(a));
    REQUIRE(Joint_get_const(scene->world, right)->other == 0);
    REQUIRE(
        scene_selection_count(scene) == 2 &&
        selected_actor(scene)->identity == selected[0]
    );
    REQUIRE(move_same_entity(scene_actor_by_identity(scene, selected[0])->entity, a));
    REQUIRE(replay_scene_edit(scene, true) == 0 && same_saved_scene(scene, moved));
    REQUIRE(Joint_get_const(scene->world, left)->other == 0);
    REQUIRE(Joint_get_const(scene->world, right)->other == ecs_entity_to_u64(a));

    /* Checkpointed history restores selection without frame-local caches. */
    char *checkpoint = app_scene_authoring_checkpoint(scene);
    move_fixture restored = move_fixture_new();
    REQUIRE(ecs_world_create_entity(restored.world).slot);
    REQUIRE(app_scene_authoring_restore_checkpoint(restored.scene, checkpoint) == 0);
    free(checkpoint);
    REQUIRE(
        replay_scene_edit(restored.scene, false) == 0 &&
        same_saved_scene(restored.scene, initial)
    );
    REQUIRE(
        scene_selection_count(restored.scene) == 2 &&
        selected_actor(restored.scene)->identity == selected[0]
    );
    REQUIRE(
        replay_scene_edit(restored.scene, true) == 0 &&
        same_saved_scene(restored.scene, moved)
    );
    REQUIRE(
        scene_selection_count(restored.scene) == 2 &&
        selected_actor(restored.scene)->identity == selected[0]
    );

    /* Deleting a moved child and checkpointing must retain its parent in the
       lifecycle history too, before the older move can be undone. */
    scene_actor *restored_a = scene_actor_by_identity(restored.scene, selected[0]);
    REQUIRE(app_scene_authoring_select_entity(restored.scene, restored_a->entity) == 0);
    REQUIRE(app_scene_authoring_delete_instance(restored.scene) == 0);
    char *deleted_checkpoint = app_scene_authoring_checkpoint(restored.scene);
    move_fixture deleted = move_fixture_new();
    REQUIRE(app_scene_authoring_restore_checkpoint(deleted.scene, deleted_checkpoint) == 0);
    free(deleted_checkpoint);
    REQUIRE(
        replay_scene_edit(deleted.scene, false) == 0 &&
        same_saved_scene(deleted.scene, moved)
    );
    REQUIRE(
        replay_scene_edit(deleted.scene, false) == 0 &&
        same_saved_scene(deleted.scene, initial)
    );
    REQUIRE(scene_selection_count(deleted.scene) == 2);
    move_fixture_free(&deleted);

    /* A parent plus its selected descendant moves once and reorders roots. */
    REQUIRE(app_scene_authoring_select_entity(scene, right) == 0);
    scene_actor_by_identity(scene, selected[0])->marked = true;
    REQUIRE(app_scene_authoring_drop_selection(scene, left, APP_SCENE_DROP_BEFORE) == 0);
    REQUIRE(scene->actors[0].identity == right_id && scene->actors[4].identity == left_id);
    REQUIRE(move_same_entity(ecs_parent(scene->world, a), right));
    REQUIRE(scene_selection_count(scene) == 2 && selected_actor(scene)->identity == right_id);
    REQUIRE(replay_scene_edit(scene, false) == 0 && same_saved_scene(scene, moved));
    REQUIRE(scene_history_can_redo(scene->history));

    /* Rejected moves preserve order, local poses, selection and both stacks. */
    REQUIRE(app_scene_authoring_select_entity(scene, right) == 0);
    char *history = scene_history_checkpoint(scene->history);
    REQUIRE(!app_scene_authoring_can_drop(scene, a, APP_SCENE_DROP_INSIDE));
    REQUIRE(app_scene_authoring_drop_selection(scene, a, APP_SCENE_DROP_INSIDE) == -1);
    REQUIRE(same_saved_scene(scene, moved) && selected_actor(scene)->identity == right_id);
    REQUIRE(app_scene_authoring_select_entity(scene, clash) == 0);
    REQUIRE(!app_scene_authoring_can_drop(scene, right, APP_SCENE_DROP_INSIDE));
    REQUIRE(app_scene_authoring_drop_selection(scene, right, APP_SCENE_DROP_INSIDE) == -1);
    REQUIRE(same_saved_scene(scene, moved));
    uint64_t stale[] = { selected[0], UINT64_MAX };
    REQUIRE(scene_move_actors(scene, stale, 2, selected[0], right_id, 0) == -1);
    REQUIRE(same_saved_scene(scene, moved));
    scene->runtime_snapshot_valid = true;
    REQUIRE(!app_scene_authoring_can_drop(scene, (ecs_entity){ 0 }, APP_SCENE_DROP_ROOT));
    REQUIRE(
        app_scene_authoring_drop_selection(scene, (ecs_entity){ 0 }, APP_SCENE_DROP_ROOT) == -1
    );
    scene->runtime_snapshot_valid = false;
    char *after_history = scene_history_checkpoint(scene->history);
    REQUIRE(!strcmp(history, after_history));
    free(history);
    free(after_history);
    REQUIRE(replay_scene_edit(scene, true) == 0);
    REQUIRE(scene->actors[0].identity == right_id && scene_selection_count(scene) == 2);

    free(initial);
    free(moved);
    move_fixture_free(&restored);
    move_fixture_free(&peer);
    move_fixture_free(&source);
    return true;
}

static bool same_world_spatial(GlobalTransform3D a, GlobalTransform3D b)
{
    CF_V3 before[] = { a.position, a.x_axis, a.y_axis, a.z_axis };
    CF_V3 after[] = { b.position, b.x_axis, b.y_axis, b.z_axis };
    for (int i = 0; i < 4; i++) {
        REQUIRE(fabsf(before[i].x - after[i].x) < .0001f);
        REQUIRE(fabsf(before[i].y - after[i].y) < .0001f);
        REQUIRE(fabsf(before[i].z - after[i].z) < .0001f);
    }
    return true;
}

static bool scene_move_spatial_contract()
{
    move_fixture source = move_fixture_new();
    app_scene_authoring *scene = source.scene;
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "name \"Spatial\"\ncomponents { Transform3D { "
            "rotation x=0.0 y=0.0 z=0.0 w=1.0; scale x=1.0 y=1.0 z=1.0; }; }\n",
            NULL
        ) == 0
    );
    REQUIRE(
        scene_authoring_load_text(
            scene,
            "version 1\nname \"Scene\"\n(array)entities {\n"
            " - name=\"Parent\" prototype=\"Spatial\" { components { "
            "Transform3D { "
            "position x=10.0 y=20.0 z=30.0; rotation x=0.0 y=0.0 z=0.70710677 "
            "w=0.70710677; "
            "scale x=2.0 y=2.0 z=2.0; }; }; }\n"
            " - name=\"Object\" prototype=\"Spatial\" { components { "
            "Transform3D { "
            "position x=4.0 y=5.0 z=6.0; rotation x=0.18257418 y=0.36514837 "
            "z=0.5477226 w=0.73029673; "
            "scale x=1.0 y=2.0 z=3.0; }; }; }\n}\n",
            NULL
        ) == 0
    );
    REQUIRE(acount(scene->actors) == 2);
    ecs_entity parent = scene->actors[0].entity, object = scene->actors[1].entity;
    uint64_t object_id = scene->actors[1].identity;
    REQUIRE(app_scene_authoring_select_entity(scene, object) == 0);
    GlobalTransform3D world = transform_world_3d(scene->world, object);
    char *initial = scene_authoring_scene_text(scene);
    REQUIRE(initial);
    REQUIRE(app_scene_authoring_drop_selection(scene, parent, APP_SCENE_DROP_INSIDE) == 0);
    REQUIRE(move_same_entity(ecs_parent(scene->world, object), parent));
    REQUIRE(same_world_spatial(world, transform_world_3d(scene->world, object)));
    const Transform3D *local = Transform3D_get_const(scene->world, object);
    REQUIRE(
        fabsf(local->scale.x - .5f) < .0001f &&
        fabsf(local->scale.y - 1) < .0001f && fabsf(local->scale.z - 1.5f) < .0001f
    );
    char *moved = scene_authoring_scene_text(scene);
    REQUIRE(moved && strcmp(initial, moved));
    REQUIRE(replay_scene_edit(scene, false) == 0 && same_saved_scene(scene, initial));
    REQUIRE(replay_scene_edit(scene, true) == 0 && same_saved_scene(scene, moved));
    REQUIRE(same_world_spatial(world, transform_world_3d(scene->world, object)));
    char *checkpoint = app_scene_authoring_checkpoint(scene);
    move_fixture restored = move_fixture_new();
    REQUIRE(
        checkpoint &&
        app_scene_authoring_restore_checkpoint(restored.scene, checkpoint) == 0
    );
    free(checkpoint);
    REQUIRE(
        replay_scene_edit(restored.scene, false) == 0 &&
        same_saved_scene(restored.scene, initial)
    );
    REQUIRE(
        replay_scene_edit(restored.scene, true) == 0 &&
        same_saved_scene(restored.scene, moved)
    );
    scene_actor *restored_object = scene_actor_by_identity(restored.scene, object_id);
    REQUIRE(restored_object && selected_actor(restored.scene)->identity == object_id);
    REQUIRE(same_world_spatial(
        world,
        transform_world_3d(restored.world, restored_object->entity)
    ));
    free(initial);
    free(moved);
    move_fixture_free(&restored);
    move_fixture_free(&source);
    return true;
}

static bool scene_move_frame(void *state)
{
    REQUIRE(studio_acceptance_studio(state));
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 1) {
        REQUIRE(scene_move_contract());
        REQUIRE(scene_move_spatial_contract());
    }
    return true;
}

TEST_CASE(scene_move_contracts)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 2);
    printf("\nStudio scene move accepted: frames=2\n");
    return true;
}
