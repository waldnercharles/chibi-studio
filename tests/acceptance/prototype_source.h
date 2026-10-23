#include "authoring/native_prototype.h"
#include "editor/collab/protocol.h"

/* A separate two-frame selector: four tiny prototype sources, three actors,
 * one workspace checkpoint restore. It does not enlarge --source-edit. */
static bool bridge_field(app_scene_authoring *scene, const char *name, const char *field, float expected)
{
    pin_value value = { 0 };
    REQUIRE(
        !app_scene_authoring_read_prototype_field(NULL, scene, name, "Transform", field, &value)
    );
    REQUIRE(value.type == PIN_F32 && value.f == expected);
    return true;
}

static bool bridge_property(
    studio *s,
    const ecs_component_desc *component,
    int field,
    const char *name,
    const char *axis,
    const char *text,
    bool active
)
{
    properties_row row = { .kind = PROPERTIES_PROTOTYPE,
                           .component = component->id,
                           .field = field };
    snprintf(row.prototype, sizeof row.prototype, "%s", name);
    ui_activation edit = { .argc = active ? 3 : 4 };
    properties_row_id(&row, edit.args[0]);
    snprintf(edit.args[1], sizeof edit.args[1], "%s", text);
    snprintf(edit.args[active ? 2 : 3], sizeof edit.args[0], "%s", axis);
    studio_properties_action(s, active ? "input" : "change", &edit);
    return true;
}

/* Drive the existing collaboration gate entirely in process. The transport
 * callback counts messages and never opens a socket or changes the workspace. */
static void bridge_collab_send(void *context, const char *json)
{
    (void)json;
    (*(int *)context)++;
}

static bool bridge_collab_restore(void *context, const char *payload, char *error, size_t size)
{
    (void)context;
    (void)payload;
    (void)error;
    (void)size;
    return true;
}

static bool bridge_collab_ready(int *sent)
{
    collab_shutdown();
    *sent = 0;
    collab_init((collab_config){ .context = sent,
                                 .send = bridge_collab_send,
                                 .restore = bridge_collab_restore });
    collab_start("source-bridge", "peer", "editor");
    char *snapshot = collab_message_write("source-bridge", "host", 0, 0, "snapshot", "{\"checkpoint_version\":1,\"workspace\":\"\"}");
    REQUIRE(snapshot);
    collab_receive(snapshot);
    free(snapshot);
    collab_tick();
    REQUIRE(collab_can_edit() && collab_state_current() == COLLAB_READY && *sent == 1);
    return true;
}

/* The caller owns the counter and always shuts collaboration down before
 * checking this helper's result, including every REQUIRE failure path. */
static bool bridge_queued_commands(
    studio *s,
    scene_prototype_asset *applied,
    const ecs_component_desc *visual,
    const ecs_component_desc *transform,
    const char *unapplied,
    int *sent,
    bool *quiet
)
{
    app_scene_authoring *scene = s->scene;
    const char *name = applied->prototype->name;
    const void *pending_step = asset_code_step(applied->source);
    int records = acount(s->records), component_toast = s->components.toast;
    const void *component_step = s->components.step;
    s->components.object = 0;
    snprintf(s->components.prototype, sizeof s->components.prototype, "%s", name);
    s->components.added[0] = 0;
    int visual_index = -1;
    for (int i = 0; i < scene->registry.component_count; i++) {
        if (scene->registry.components[i] == visual) {
            visual_index = i;
        }
    }
    REQUIRE(visual_index >= 0);
    REQUIRE(bridge_collab_ready(sent));
    studio_component_choose(s, &(studio_palette_entry){ .index = visual_index });
    bool queued_add_quiet = *sent == 2 && acount(s->records) == records &&
                            !s->components.added[0] &&
                            !prototype_has_component_id(
                                applied->prototype,
                                visual->id
                            );
    REQUIRE(bridge_collab_ready(sent));
    snprintf(s->components.menu, sizeof s->components.menu, "Transform");
    studio_properties_action(s, "componentAction", &(ui_activation){ 0 });
    bool queued_remove_quiet = *sent == 2 && acount(s->records) == records &&
                               s->components.toast == component_toast &&
                               s->components.step == component_step &&
                               prototype_has_component_id(
                                   applied->prototype,
                                   transform->id
                               );
    REQUIRE(bridge_collab_ready(sent));
    REQUIRE(
        app_scene_authoring_set_prototype_field(
            scene,
            name,
            "Transform",
            "x",
            (pin_value){ .type = PIN_F32, .f = 21 }
        ) == APP_PROTOTYPE_EDIT_PENDING &&
        *sent == 2
    );
    REQUIRE(bridge_collab_ready(sent));
    REQUIRE(
        app_scene_authoring_attach_prototype_behavior(scene, name, "Counter") ==
            APP_PROTOTYPE_EDIT_PENDING &&
        *sent == 2
    );
    REQUIRE(bridge_collab_ready(sent));
    REQUIRE(
        app_scene_authoring_detach_prototype_behavior(scene, name, "Counter") ==
            APP_PROTOTYPE_EDIT_PENDING &&
        *sent == 2
    );
    bool queued_source_unchanged = asset_code_step(applied->source) == pending_step &&
                                   !strcmp(asset_code_text(applied->source), unapplied) &&
                                   applied->source_not_applied;
    *quiet = queued_add_quiet && queued_remove_quiet && queued_source_unchanged;
    return true;
}

static bool prototype_source_contract(studio *s)
{
    app_scene_authoring *scene = s->scene;
    char paths[4][1024], directory[1024], error[256];
    int directory_length = snprintf(
        directory,
        sizeof directory,
        "%s/prototypes",
        scene->content_root
    );
    REQUIRE(directory_length > 0 && (size_t)directory_length < sizeof directory);
    bool created_directory = mkdir(directory, 0700) == 0;
    if (!created_directory) {
        REQUIRE(errno == EEXIST);
        struct stat info;
        REQUIRE(!stat(directory, &info) && S_ISDIR(info.st_mode));
    }
    const char *names[] = { "SourceBridgeBase",
                            "SourceBridge",
                            "SourceBridgeApply",
                            "SourceBridgeReverse" };
    const char *sources[] = {
        "SourceBridgeBase :: prototype { Transform {x:10,y:7} }\n",
        "// keep this comment\nSourceBridge :: prototype(SourceBridgeBase) { "
        "Transform {x:1} }\n",
        "SourceBridgeApply :: prototype { Transform {x:1,y:2} }\n",
        "SourceBridgeReverse :: prototype { Transform {x:4,y:5} }\n"
    };
    for (int i = 0; i < 4; i++) {
        int n = snprintf(
            paths[i],
            sizeof paths[i],
            "%s/prototypes/%s.chibi",
            scene->content_root,
            names[i]
        );
        REQUIRE(n > 0 && (size_t)n < sizeof paths[i]);
        struct stat info;
        REQUIRE(stat(paths[i], &info) != 0 && errno == ENOENT);
        REQUIRE(!write_file_atomic(paths[i], sources[i]));
    }
    REQUIRE(content_refresh(scene));
    for (int i = 0; i < 4; i++) {
        REQUIRE(!app_scene_authoring_register_prototype(scene, sources[i], paths[i]));
    }
    const ecs_component_desc *transform = component_descriptor(scene, "Transform");
    REQUIRE(transform && transform->type->field_count >= 2);
    int x = -1;
    for (int i = 0; i < transform->type->field_count; i++) {
        if (!strcmp(transform->type->fields[i].name, "x")) {
            x = i;
        }
    }
    REQUIRE(x >= 0);

    /* Ordinary open and the existing Properties gesture share Assets' code
       session. Two input events and the commit are exactly one source step. */
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[1]));
    scene_prototype_asset *asset = find_prototype(scene, names[1]);
    asset_code *code = asset->source;
    REQUIRE(code && !asset_code_dirty(code));
    REQUIRE(bridge_property(s, transform, x, names[1], "", "2", true));
    REQUIRE(bridge_property(s, transform, x, names[1], "", "3", true));
    REQUIRE(bridge_property(s, transform, x, names[1], "", "3", false));
    REQUIRE(bridge_field(scene, names[1], "x", 3));
    bool source_owned_gesture = asset_code_dirty(code) &&
                                strstr(asset_code_text(code), "keep this comment") &&
                                strcmp(asset_code_text(code), sources[1]);
    if (!source_owned_gesture) {
        for (int i = 0; i < 4; i++) {
            REQUIRE(!unlink(paths[i]));
        }
        if (created_directory) {
            REQUIRE(!rmdir(directory));
        }
    }
    REQUIRE(source_owned_gesture); /* Old model-only Properties must fail after file cleanup. */
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!strcmp(asset_code_text(code), sources[1]) && !asset_code_can_replay(code, false));
    REQUIRE(!prototype_redo(scene));
    REQUIRE(bridge_field(scene, names[1], "x", 3));
    REQUIRE(!app_scene_authoring_refresh_asset_index(scene));
    int indexed = reference_index_find_asset(
        scene->asset_index,
        REFERENCE_ASSET_PROTOTYPE,
        paths[1],
        NULL
    );
    REQUIRE(indexed >= 0 && !app_scene_authoring_open_asset(scene, indexed));
    REQUIRE(asset->source == code && scene_tabs_dirty(scene, &scene->tabs[scene->active_tab]));

    /* Omitted/inherited fields become local without flattening normal edits. */
    REQUIRE(!app_scene_authoring_set_prototype_field(
        scene,
        names[1],
        "Transform",
        "y",
        (pin_value){ .type = PIN_F32, .f = 9 }
    ));
    REQUIRE(bridge_field(scene, names[1], "y", 9));
    REQUIRE(strstr(asset_code_text(code), "SourceBridgeBase"));
    char *base = file_read_text(paths[0]);
    REQUIRE(base && !strcmp(base, sources[0]));
    free(base);
    REQUIRE(!app_scene_authoring_set_prototype_field(
        scene,
        names[0],
        "Transform",
        "y",
        (pin_value){ .type = PIN_F32, .f = 11 }
    ));
    REQUIRE(!scene_prototype_source_save(scene, find_prototype(scene, names[0])));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(bridge_field(scene, names[1], "y", 11));
    REQUIRE(!prototype_redo(scene));
    REQUIRE(bridge_field(scene, names[1], "y", 9));

    /* Native component defaults and the actual axis control use the source
       writer. Undo removes the authored component and restores exact text. */
    char *before_component = xstrdup(asset_code_text(code));
    REQUIRE(!app_scene_authoring_add_prototype_component(scene, names[1], "Aim"));
    const ecs_component_desc *visual = component_descriptor(scene, "Aim");
    REQUIRE(visual);
    const prototype_component *component = NULL;
    for (int i = 0; i < acount(asset->prototype->components); i++) {
        if (asset->prototype->components[i].desc->id == visual->id) {
            component = &asset->prototype->components[i];
        }
    }
    REQUIRE(component);
    int vector_field = -1;
    arena storage = { 0 };
    pin_value vector = { 0 };
    const type_desc *vector_type = NULL;
    for (int i = 0; i < visual->type->field_count && !vector_type; i++) {
        REQUIRE(!pin_read_field(&storage, component->data, &visual->type->fields[i], &vector));
        vector_type = properties_vector(&vector);
        if (vector_type) {
            vector_field = i;
        }
    }
    REQUIRE(vector_type && vector_type->field_count >= 2);
    pin_value sibling = { 0 };
    REQUIRE(!pin_read_field(&storage, vector.ptr, &vector_type->fields[1], &sibling));
    const char *axis = vector_type->fields[0].name;
    REQUIRE(bridge_property(s, visual, vector_field, names[1], axis, "6", true));
    REQUIRE(bridge_property(s, visual, vector_field, names[1], axis, "8", false));
    REQUIRE(!prototype_read_field(
        &storage,
        asset->prototype,
        visual->name,
        visual->type->fields[vector_field].name,
        &vector
    ));
    pin_value after_sibling = { 0 };
    REQUIRE(!pin_read_field(&storage, vector.ptr, &vector_type->fields[1], &after_sibling));
    REQUIRE(pin_value_equal(&sibling, &after_sibling));
    const ecs_component_desc *motion = component_descriptor(scene, "VisualMotion");
    REQUIRE(motion);
    prototype *native = prototype_clone(asset->prototype, error, sizeof error);
    REQUIRE(native && !prototype_add_native_component(native, motion));
    REQUIRE(!app_scene_authoring_add_prototype_component(scene, names[1], motion->name));
    for (int i = 0; i < motion->type->field_count; i++) {
        pin_value expected = { 0 }, actual = { 0 };
        const char *field = motion->type->fields[i].name;
        REQUIRE(!prototype_read_field(&storage, native, motion->name, field, &expected));
        REQUIRE(!prototype_read_field(&storage, asset->prototype, motion->name, field, &actual));
        REQUIRE(pin_value_equal(&expected, &actual));
    }
    arena_free(&storage);
    prototype_free(native);
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!strcmp(asset_code_text(code), before_component));
    free(before_component);
    char *before_remove = xstrdup(asset_code_text(code));
    REQUIRE(!app_scene_authoring_remove_prototype_component(scene, names[1], "Transform"));
    REQUIRE(!prototype_has_component_id(asset->prototype, transform->id));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!strcmp(asset_code_text(code), before_remove));
    free(before_remove);

    /* Public behavior commands share the same source history; Counter is the
       host fixture's registered executable and no actor uses this prototype. */
    REQUIRE(!app_scene_authoring_attach_prototype_behavior(scene, names[1], "Counter"));
    REQUIRE(prototype_has_graph(asset->prototype, "Counter"));
    REQUIRE(!app_scene_authoring_detach_prototype_behavior(scene, names[1], "Counter"));
    REQUIRE(!prototype_has_graph(asset->prototype, "Counter"));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(prototype_has_graph(asset->prototype, "Counter"));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!prototype_has_graph(asset->prototype, "Counter"));

    /* Text and Properties see the same draft. Invalid syntax and a valid
       identity-changing draft retain the last accepted model and overlay. */
    char *accepted = xstrdup(asset_code_text(code));
    REQUIRE(studio_code_replace(s, paths[1], accepted, "SourceBridge :: prototype {"));
    const void *invalid_step = asset_code_step(code);
    REQUIRE(
        app_scene_authoring_set_prototype_field(
            scene,
            names[1],
            "Transform",
            "x",
            (pin_value){ .type = PIN_F32, .f = 12 }
        ) < 0
    );
    REQUIRE(
        asset_code_step(code) == invalid_step &&
        !strcmp(asset_code_text(code), "SourceBridge :: prototype {")
    );
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!strcmp(asset_code_text(code), accepted));
    REQUIRE(
        studio_code_replace(s, paths[1], accepted, "WrongBridgeName :: prototype {Transform{x:99}}")
    );
    REQUIRE(asset_code_error(code)[0]);
    REQUIRE(text_overlay_text(paths[1]) && !strcmp(text_overlay_text(paths[1]), accepted));
    REQUIRE(!prototype_undo(scene));
    free(accepted);

    /* A held raw draft is accepted for Properties without applying on open;
       explicit Save performs the pending application and clears its hold. */
    s->prefs.code_apply_live = false;
    char *before_held = xstrdup(asset_code_text(code));
    const char *held_text = "SourceBridge :: prototype(SourceBridgeBase) { "
                            "Transform {x:18,y:9} }";
    REQUIRE(studio_code_replace(s, paths[1], before_held, held_text));
    REQUIRE(asset_code_runtime_held(code));
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[1]));
    REQUIRE(asset_code_runtime_held(code));
    REQUIRE(bridge_field(scene, names[1], "x", 18));
    free(before_held);

    /* Save preserves Undo, and Discard restores disk, never a model snapshot. */
    studio_save(s);
    REQUIRE(!asset_code_runtime_held(code));
    s->prefs.code_apply_live = true;
    char *saved = file_read_text(paths[1]);
    REQUIRE(saved && strstr(saved, ":: prototype") && !asset_code_dirty(code));
    REQUIRE(asset_code_can_replay(code, false));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(asset_code_dirty(code));
    int source_tab = scene->active_tab;
    studio_discard_tab(s, source_tab);
    asset = find_prototype(scene, names[1]);
    REQUIRE(asset->source && !asset_code_dirty(asset->source));
    REQUIRE(!strcmp(asset_code_text(asset->source), saved));
    free(saved);

    /* Assets-first and an already-dirty model transition keep the edited
       values, add a source step, and retain the original disk baseline. */
    REQUIRE(!app_scene_authoring_refresh_asset_index(scene));
    indexed = reference_index_find_asset(scene->asset_index, REFERENCE_ASSET_PROTOTYPE, paths[3], NULL);
    REQUIRE(indexed >= 0);
    scene_prototype_asset *reverse = find_prototype(scene, names[3]);
    REQUIRE(reverse && !reverse->source);
    REQUIRE(!prototype_write_field(
        reverse->prototype,
        "Transform",
        "x",
        (pin_value){ .type = PIN_F32, .f = 15 }
    ));
    reverse->dirty = 1;
    REQUIRE(!app_scene_authoring_open_asset(scene, indexed));
    /* Assets selects the tab; Code composition acquires its shared session. */
    kit_begin(s->kit, &s->in);
    kit_root(s->kit, "app");
    studio_code_body(s, STUDIO_AREA_TOP);
    kit_end(s->kit);
    asset_code *reverse_source = reverse->source;
    REQUIRE(reverse_source && asset_code_dirty(reverse_source));
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[3]));
    REQUIRE(reverse->source == reverse_source);
    REQUIRE(reverse->source && asset_code_dirty(reverse->source));
    REQUIRE(bridge_field(scene, names[3], "x", 15));
    REQUIRE(!prototype_undo(scene));
    REQUIRE(!strcmp(asset_code_text(reverse->source), sources[3]));

    /* A never-opened prototype edited by background Apply has a document and
       source history. Scene Undo restores source using a recoverable step. */
    REQUIRE(!scene_authoring_load_text(
        scene,
        "version 1\nname \"Scene\"\n"
        "(array)entities { - name=\"Source\" prototype=\"SourceBridgeApply\" { "
        "components {Transform x=20.0;}; }\n"
        "- name=\"Peer\" prototype=\"SourceBridgeApply\"\n"
        "- name=\"Override\" prototype=\"SourceBridgeApply\" { components "
        "{Transform x=30.0;}; } }",
        NULL
    ));
    REQUIRE(acount(scene->actors) == 3);
    scene_prototype_asset *applied = find_prototype(scene, names[2]);
    REQUIRE(applied && !applied->source);
    int active = scene->active_tab;
    prototype_field field = { transform, x };
    REQUIRE(!scene_apply_overrides(scene, &scene->actors[0], &field, 1));
    REQUIRE(
        scene->active_tab == active && applied->source &&
        asset_code_dirty(applied->source)
    );
    REQUIRE(Transform_get_const(scene->world, scene->actors[0].entity)->x == 20);
    REQUIRE(Transform_get_const(scene->world, scene->actors[1].entity)->x == 20);
    REQUIRE(Transform_get_const(scene->world, scene->actors[2].entity)->x == 30);
    char *apply_text = xstrdup(asset_code_text(applied->source));
    /* A notified deletion retains the canonical owner, including a clean
       source's history. Close and Save All must see the missing-file dirty
       state; failed Discard cannot replace it with a pathless model draft. */
    REQUIRE(!scene_prototype_source_save(scene, applied));
    char *deleted_text = file_read_text(paths[2]);
    REQUIRE(deleted_text && !asset_code_dirty(applied->source));
    asset_code *deleted_source = applied->source;
    const void *deleted_step = asset_code_step(deleted_source);
    int deleted_tab = -1;
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].path && !strcmp(scene->tabs[i].path, paths[2])) {
            deleted_tab = i;
        }
    }
    REQUIRE(deleted_tab >= 0);
    REQUIRE(!unlink(paths[2]));
    scene_disk_follow(scene, &(scene_move){ .from = paths[2] }, 1);
    bool missing_source_dirty = applied->source == deleted_source &&
                                applied->path && !strcmp(applied->path, paths[2]) &&
                                asset_code_file_missing(deleted_source) &&
                                scene_prototype_dirty(applied) &&
                                scene_tabs_dirty(scene, &scene->tabs[deleted_tab]);
    REQUIRE(
        applied->source == deleted_source && applied->path &&
        !strcmp(applied->path, paths[2]) && asset_code_file_missing(deleted_source)
    );
    REQUIRE(asset_code_step(deleted_source) == deleted_step);
    REQUIRE(!strcmp(asset_code_text(deleted_source), deleted_text));
    int tab_count = acount(scene->tabs);
    studio_close_tab(s, deleted_tab);
    bool missing_close_prompt = acount(scene->tabs) == tab_count &&
                                s->unsaved_prompt.action == STUDIO_UNSAVED_CLOSE_TAB &&
                                !strcmp(
                                    s->unsaved_prompt.tab,
                                    scene->tabs[deleted_tab].key
                                );
    REQUIRE(acount(scene->tabs) == tab_count);
    kit_toast_dismiss(s->kit, s->unsaved_prompt.toast);
    s->unsaved_prompt = (studio_unsaved_prompt){ 0 };
    bool missing_save_all = !studio_save_all(s);
    REQUIRE(access(paths[2], F_OK) != 0 && errno == ENOENT);
    REQUIRE(
        asset_code_file_missing(deleted_source) && asset_code_step(deleted_source) == deleted_step
    );
    /* Match Code's retained cache reference; failed Discard releases only
       that reference, never the workspace owner or its recoverable history. */
    bool cached_deleted_source = false;
    for (int i = 0; i < acount(s->code.assets); i++) {
        cached_deleted_source |= s->code.assets[i] == deleted_source;
    }
    if (!cached_deleted_source) {
        apush(s->code.assets, asset_code_retain(deleted_source));
    }
    studio_discard_tab(s, deleted_tab);
    for (int i = 0; i < acount(s->code.assets); i++) {
        REQUIRE(s->code.assets[i] != deleted_source);
    }
    REQUIRE(
        acount(scene->tabs) == tab_count && applied->source == deleted_source &&
        asset_code_step(deleted_source) == deleted_step &&
        !strcmp(asset_code_text(deleted_source), deleted_text)
    );
    bool missing_discard_dirty = scene_tabs_dirty(scene, &scene->tabs[deleted_tab]);
    REQUIRE(
        text_overlay_text(paths[2]) &&
        !strcmp(text_overlay_text(paths[2]), applied->source_accepted)
    );
    const char *invalid_apply = "SourceBridgeApply :: prototype {";
    REQUIRE(studio_code_replace(s, paths[2], deleted_text, invalid_apply));
    const void *missing_invalid_step = asset_code_step(deleted_source);
    /* One checkpoint covers both a rejected identity draft and the populated
       scene source-before/source-after payload, including rebase-only replay. */
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[1]));
    asset = find_prototype(scene, names[1]);
    code = asset->source;
    accepted = xstrdup(asset_code_text(code));
    REQUIRE(
        studio_code_replace(s, paths[1], accepted, "WrongBridgeName :: prototype {Transform{x:99}}")
    );
    char *checkpoint = app_scene_authoring_checkpoint(scene);
    REQUIRE(checkpoint);
    move_fixture restored = move_fixture_new_with_root(scene->content_root);
    REQUIRE(
        !preview_sources_prepare(restored.live, scene->content_root, behavior_default_backend(), error, sizeof error)
    );
    REQUIRE(!app_scene_authoring_restore_checkpoint(restored.scene, checkpoint));
    scene_prototype_asset *restored_asset = find_prototype(restored.scene, names[1]);
    REQUIRE(
        restored_asset && restored_asset->source &&
        asset_code_error(restored_asset->source)[0]
    );
    REQUIRE(
        !strcmp(asset_code_text(restored_asset->source), "WrongBridgeName :: prototype {Transform{x:99}}")
    );
    REQUIRE(!strcmp(restored_asset->source_accepted, accepted));
    scene_prototype_asset *restored_apply = find_prototype(restored.scene, names[2]);
    REQUIRE(restored_apply && restored_apply->source);
    REQUIRE(
        asset_code_file_missing(restored_apply->source) &&
        asset_code_dirty(restored_apply->source) &&
        !strcmp(asset_code_text(restored_apply->source), invalid_apply)
    );
    REQUIRE(!replay_scene_edit(restored.scene, false));
    REQUIRE(!strcmp(asset_code_text(restored_apply->source), sources[2]));
    REQUIRE(
        Transform_get_const(restored.scene->world, restored.scene->actors[0].entity)
            ->x == 20
    );
    REQUIRE(
        Transform_get_const(restored.scene->world, restored.scene->actors[1].entity)
            ->x == 1
    );
    REQUIRE(!replay_scene_edit(restored.scene, true));
    REQUIRE(!strcmp(asset_code_text(restored_apply->source), apply_text));
    REQUIRE(asset_code_file_missing(restored_apply->source));
    move_fixture_free(&restored);
    free(checkpoint);
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!scene_prototype_source_sync(scene, asset, true));
    free(accepted);

    /* The returning file and ordinary code refresh must not consume a raw
       draft or its history. Source Undo recovers the saved bytes; explicit
       Save then resolves the existing disk-conflict policy without Save As. */
    REQUIRE(!write_file_atomic(paths[2], deleted_text));
    scene_disk_follow(scene, &(scene_move){ .to = paths[2] }, 1);
    asset_code_refresh(deleted_source);
    REQUIRE(
        asset_code_file_missing(deleted_source) &&
        asset_code_step(deleted_source) == missing_invalid_step &&
        !strcmp(asset_code_text(deleted_source), invalid_apply)
    );
    REQUIRE(asset_code_replay(deleted_source, false, false));
    REQUIRE(!scene_prototype_source_sync(scene, applied, true));
    REQUIRE(
        !strcmp(asset_code_text(deleted_source), deleted_text) &&
        asset_code_file_missing(deleted_source)
    );
    bool missing_undo_dirty = asset_code_dirty(deleted_source);
    REQUIRE(!scene_prototype_source_save(scene, applied));
    REQUIRE(!asset_code_file_missing(deleted_source) && !asset_code_dirty(deleted_source));
    REQUIRE(asset_code_can_replay(deleted_source, true));
    REQUIRE(!scene_prototype_source_accept(
        scene,
        applied,
        asset_code_text(deleted_source),
        apply_text,
        false,
        true,
        error,
        sizeof error
    ));
    free(deleted_text);

    /* Scene Undo may replace a later invalid draft, but source Undo recovers
       its exact bytes rather than discarding that independent history. */
    REQUIRE(studio_code_replace(s, paths[2], apply_text, invalid_apply));
    REQUIRE(!replay_scene_edit(scene, false));
    REQUIRE(!strcmp(asset_code_text(applied->source), sources[2]));
    REQUIRE(asset_code_replay(applied->source, false, false));
    REQUIRE(!strcmp(asset_code_text(applied->source), invalid_apply));
    REQUIRE(scene_prototype_source_sync(scene, applied, true) < 0);
    REQUIRE(asset_code_replay(applied->source, true, false));
    REQUIRE(!scene_prototype_source_sync(scene, applied, true));
    REQUIRE(!strcmp(asset_code_text(applied->source), sources[2]));
    REQUIRE(Transform_get_const(scene->world, scene->actors[0].entity)->x == 20);
    REQUIRE(Transform_get_const(scene->world, scene->actors[1].entity)->x == 1);
    REQUIRE(!replay_scene_edit(scene, true));
    REQUIRE(!strcmp(asset_code_text(applied->source), apply_text));
    REQUIRE(asset_code_can_replay(applied->source, false));
    free(apply_text);

    /* A stale/invalid source batch is a no-op across text, model and history. */
    const void *step = asset_code_step(applied->source);
    char *text = xstrdup(asset_code_text(applied->source));
    REQUIRE(
        scene_prototype_source_accept(scene, applied, "stale", text, false, true, error, sizeof error) < 0
    );
    REQUIRE(
        scene_prototype_source_accept(scene, applied, text, "broken", false, true, error, sizeof error) < 0
    );
    REQUIRE(
        asset_code_step(applied->source) == step &&
        !strcmp(asset_code_text(applied->source), text)
    );
    REQUIRE(bridge_field(scene, names[2], "x", 20));

    /* Existing runtime failure stays an accepted, undoable source edit. A
       missing behavior can follow a successful attach on an earlier actor;
       reversing the source reconciles every touched identity, not just the
       last completely applied model. */
    const char
        *unapplied = "SourceBridgeApply :: prototype { Transform {x:20,y:2} "
                     "behaviors:[Counter,UnavailableBridgeBehavior]; }";
    REQUIRE(
        scene_prototype_source_accept(scene, applied, text, unapplied, false, true, error, sizeof error) > 0
    );
    REQUIRE(applied->source_not_applied && asset_code_dirty(applied->source));
    REQUIRE(actor_has_behavior(scene->live->runtime->sources, &scene->actors[0], "Counter"));
    REQUIRE(!strcmp(asset_code_text(applied->source), unapplied));
    REQUIRE(prototype_has_graph(applied->prototype, "Counter"));
    REQUIRE(prototype_has_graph(applied->prototype, "UnavailableBridgeBehavior"));
    REQUIRE(scene_apply_overrides(scene, &scene->actors[0], &field, 1) > 0);
    /* Queued collaboration proposals are not accepted edits even when this
       asset already has a pending runtime failure. Exercise the real palette
       and removal action, then the public field/behavior command gates. */
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[2]));
    const void *pending_step = asset_code_step(applied->source);
    int sent = 0;
    bool queued_commands_quiet = false;
    bool queued_valid = bridge_queued_commands(s, applied, visual, transform, unapplied, &sent, &queued_commands_quiet);
    collab_shutdown();
    REQUIRE(queued_valid);
    /* An actual accepted command has a distinct result, and replay accepts
       that result without rejecting a source edit the editor already owns. */
    REQUIRE(
        app_scene_authoring_set_prototype_field(
            scene,
            names[2],
            "Transform",
            "x",
            (pin_value){ .type = PIN_F32, .f = 20 }
        ) == APP_PROTOTYPE_EDIT_NOT_APPLIED
    );
    yyjson_mut_doc *action_doc = NULL;
    yyjson_mut_val *action = scene_action_root(scene, "prototype.field", &action_doc);
    yyjson_mut_obj_add_str(action_doc, action, "prototype", names[2]);
    yyjson_mut_obj_add_str(action_doc, action, "component", "Transform");
    yyjson_mut_obj_add_str(action_doc, action, "field", "x");
    yyjson_mut_obj_add_real(action_doc, action, "value", 20.0);
    char *action_json = scene_action_write(action_doc);
    REQUIRE(action_json && scene_collab_apply_action(scene, action_json, error, sizeof error));
    free(action_json);
    REQUIRE(asset_code_step(applied->source) == pending_step);
    REQUIRE(asset_code_replay(applied->source, false, false));
    REQUIRE(!scene_prototype_source_sync(scene, applied, true));
    REQUIRE(!applied->source_not_applied);
    REQUIRE(!strcmp(asset_code_text(applied->source), text));
    for (int i = 0; i < acount(scene->actors); i++) {
        REQUIRE(!actor_has_behavior(scene->live->runtime->sources, &scene->actors[i], "Counter"));
        REQUIRE(
            !actor_has_behavior(scene->live->runtime->sources, &scene->actors[i], "UnavailableBridgeBehavior")
        );
    }
    free(text);
    for (int i = 0; i < 4; i++) {
        REQUIRE(!unlink(paths[i]));
    }
    if (created_directory) {
        REQUIRE(!rmdir(directory));
    }
    bool deleted_source_retained = missing_source_dirty &&
                                   missing_close_prompt && missing_save_all &&
                                   missing_discard_dirty && missing_undo_dirty;
    fprintf(stderr, "Prototype source edge controls: deleted_source_retained=%d queued_commands_quiet=%d\n", deleted_source_retained, queued_commands_quiet);
    REQUIRE(deleted_source_retained && queued_commands_quiet); /* Both controls fail only after cleanup. */
    return true;
}

static bool prototype_source_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s);
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 1) {
        REQUIRE(prototype_source_contract(s));
    }
    return true;
}

TEST_CASE(prototype_source_ownership)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 2);
    printf("\nStudio prototype source ownership accepted: frames=2 prototypes=4 actors=3 checkpoints=1\n");
    return true;
}
