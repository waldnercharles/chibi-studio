#include "editor/documents/prototype_source_input.h"

#define AUTHORITY_SOURCE(x, y)                                                 \
    "package asset\nimport data \"game:assets\"\n"                             \
    "pub Build :: () -> byte[] { return data.Encode(data.Object(["             \
    "data.Field.Named(\"name\",data.Value.Text(\"AuthoritySource\")),"         \
    "data.Field.Named(\"components\",data.Object([data.Field.Named("           \
    "\"Transform\",data.Object(["                                              \
    "data.Field.Named(\"x\",data.Value.Real(" #x ")),"                         \
    "data.Field.Named(\"y\",data.Value.Real(" #y "))]))]))])) }\n"

/* One tiny file, one move_fixture, no actors, and one real same-scene
 * checkpoint restoration. The dedicated selector owns the two UI frames. */
typedef struct
{
    studio *s;
    const char *path;
    char *action;
    char **owned;
    int received, sent, proposals, accepted, acks, snapshots;
    int applies, restores, restored;
    uint64_t peer;
    int compositions;
    bool valid, source_guard, code_authority, property_authority;
    int retry_held; /* -1 means this phase was not reached. */
    int frame_held, frame_live, disk_conflict;
} authority_fixture;

static char *authority_keep(authority_fixture *test, char *text)
{
    if (text) {
        apush(test->owned, text);
    }
    return text;
}

static scene_prototype_asset *authority_asset(authority_fixture *test)
{
    return find_prototype(test->s->scene, "AuthoritySource");
}

/* Read the retained model even when canonical source has a diagnostic. */
static bool authority_model_x(authority_fixture *test, float expected)
{
    pin_value value = { 0 };
    scene_prototype_asset *asset = authority_asset(test);
    REQUIRE(
        asset &&
        !prototype_read_field(&acceptance_field_reads, asset->prototype, "Transform", "x", &value)
    );
    REQUIRE(value.type == PIN_F32 && value.f == expected);
    return true;
}

static void authority_send(void *context, const char *json)
{
    authority_fixture *test = context;
    collab_message message;
    if (!collab_message_read(json, &message)) {
        test->valid = false;
        return;
    }
    test->sent++;
    if (!strcmp(message.kind, "proposal") || !strcmp(message.kind, "accepted")) {
        test->proposals += !strcmp(message.kind, "proposal");
        test->accepted += !strcmp(message.kind, "accepted");
        free(test->action);
        test->action = yyjson_val_write(message.payload, 0, NULL);
    } else if (!strcmp(message.kind, "ack")) {
        test->acks++;
    } else if (!strcmp(message.kind, "snapshot_ack")) {
        test->snapshots++;
    } else {
        test->valid = false;
    }
    collab_message_free(&message);
}

static bool authority_apply(void *context, const char *action, char *error, size_t size)
{
    authority_fixture *test = context;
    test->applies++;
    bool ok = scene_collab_apply_action(test->s->scene, action, error, size);
    test->valid &= ok;
    return ok;
}

static bool authority_restore(void *context, const char *payload, char *error, size_t size)
{
    authority_fixture *test = context;
    test->restores++;
    yyjson_doc *doc = yyjson_read(payload, strlen(payload), 0);
    const char *workspace = yyjson_get_str(
        yyjson_obj_get(yyjson_doc_get_root(doc), "workspace")
    );
    bool ok = workspace != NULL;
    if (workspace && *workspace) {
        test->restored++;
        ok = !app_scene_authoring_restore_checkpoint(test->s->scene, workspace);
    }
    yyjson_doc_free(doc);
    if (!ok) {
        snprintf(error, size, "Authority fixture checkpoint failed");
    }
    test->valid &= ok;
    return ok;
}

static bool authority_queue(
    authority_fixture *test,
    const char *client,
    uint64_t seq,
    uint64_t revision,
    const char *kind,
    const char *payload
)
{
    char *json = collab_message_write("authority-room", client, seq, revision, kind, payload);
    REQUIRE(json);
    test->received++;
    collab_receive(json);
    free(json);
    return true;
}

static bool authority_pump(authority_fixture *test)
{
    collab_tick();
    scene_source_input_tick(test->s->scene);
    REQUIRE(test->valid);
    return true;
}

static bool authority_snapshot(authority_fixture *test, const char *workspace)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "checkpoint_version", 1);
    yyjson_mut_obj_add_strcpy(doc, root, "workspace", workspace);
    char *payload = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    bool queued = authority_queue(test, "host", 0, collab_revision(), "snapshot", payload);
    free(payload);
    REQUIRE(queued);
    return authority_pump(test);
}

static bool authority_start(authority_fixture *test, const char *role)
{
    REQUIRE(!scene_source_input_any(test->s->scene));
    collab_shutdown();
    collab_init((collab_config){ .context = test,
                                 .send = authority_send,
                                 .apply = authority_apply,
                                 .restore = authority_restore });
    collab_start("authority-room", "local", role);
    REQUIRE(authority_snapshot(test, ""));
    REQUIRE(collab_state_current() == COLLAB_READY);
    return true;
}

static bool authority_echo(authority_fixture *test)
{
    collab_action_identity pending;
    REQUIRE(test->action && collab_pending_action(&pending));
    REQUIRE(
        authority_queue(test, "local", pending.seq, collab_revision() + 1, "accepted", test->action)
    );
    REQUIRE(authority_pump(test));
    collab_action_outcome outcome;
    REQUIRE(collab_latest_outcome(&outcome) && outcome.action.id == pending.id);
    REQUIRE(outcome.kind == COLLAB_ACTION_ACCEPTED);
    return true;
}

static bool authority_reject(authority_fixture *test)
{
    collab_action_identity pending;
    REQUIRE(collab_pending_action(&pending));
    REQUIRE(
        authority_queue(test, "local", pending.seq, collab_revision(), "rejected", "{\"reason\":\"Stale source proposal\"}")
    );
    return authority_pump(test);
}

static char *authority_action(
    authority_fixture *test,
    const char *before,
    const char *after,
    uint64_t revision,
    int history,
    bool live
)
{
    yyjson_mut_doc *doc;
    yyjson_mut_val *root = scene_action_root(test->s->scene, "prototype.source", &doc);
    yyjson_mut_obj_add_str(doc, root, "prototype", "AuthoritySource");
    yyjson_mut_obj_add_str(doc, root, "path", "prototypes/AuthoritySource.chibi");
    yyjson_mut_obj_add_uint(doc, root, "base_revision", revision);
    yyjson_mut_obj_add_strcpy(doc, root, "before", before);
    yyjson_mut_obj_add_strcpy(doc, root, "after", after);
    yyjson_mut_obj_add_str(doc, root, "input_kind", "raw");
    yyjson_mut_obj_add_str(doc, root, "input_key", "peer");
    char group[64];
    snprintf(group, sizeof group, "peer-%llu", (unsigned long long)++test->peer);
    yyjson_mut_obj_add_strcpy(doc, root, "input_group", group);
    yyjson_mut_obj_add_int(doc, root, "history", history);
    yyjson_mut_obj_add_bool(doc, root, "live", live);
    yyjson_mut_obj_add_bool(doc, root, "coalesce", false);
    return authority_keep(test, scene_action_write(doc));
}

static bool authority_peer(authority_fixture *test, const char *text, int history, bool live)
{
    const char *before = asset_code_text(authority_asset(test)->source);
    char *action = authority_action(test, before, text, collab_revision(), history, live);
    REQUIRE(authority_queue(test, "peer", test->peer, collab_revision() + 1, "accepted", action));
    return authority_pump(test);
}

static bool authority_raw(authority_fixture *test, const char *text)
{
    const char *before = scene_source_input_text(test->s->scene, authority_asset(test));
    return studio_code_replace(test->s, test->path, before, text);
}

/* These are composition passes on the existing kit, not host/GPU frames. */
static void authority_code_draw(authority_fixture *test)
{
    studio *s = test->s;
    test->compositions++;
    kit_begin(s->kit, &s->in);
    kit_root(s->kit, "app");
    studio_code_body(s, STUDIO_AREA_TOP);
    kit_end(s->kit);
}

static void authority_code_paste(authority_fixture *test, const char *text)
{
    kit_code_editor *editor = asset_code_editor(authority_asset(test)->source);
    kit_code_select(editor, 0, (int)strlen(kit_code_text(editor, NULL)));
    kit_code_paste(editor, text);
}

static bool authority_text(authority_fixture *test, const char *canonical, const char *local)
{
    scene_prototype_asset *asset = authority_asset(test);
    REQUIRE(asset && asset->source && !strcmp(asset_code_text(asset->source), canonical));
    REQUIRE(!strcmp(scene_source_input_text(test->s->scene, asset), local ? local : canonical));
    return true;
}

static bool authority_history(authority_fixture *test, bool redo, const char *expected)
{
    char *before = authority_keep(
        test,
        xstrdup(asset_code_text(authority_asset(test)->source))
    );
    REQUIRE(!studio_code_history(test->s, redo, true));
    REQUIRE(authority_text(test, before, expected));
    REQUIRE(authority_echo(test));
    return authority_text(test, expected, NULL);
}

static bool authority_unopened_rejection(authority_fixture *test)
{
    app_scene_authoring *scene = test->s->scene;
    scene_prototype_asset *asset = authority_asset(test);
    REQUIRE(asset && !asset->source);
    const prototype *model = asset->prototype;
    int tabs = acount(scene->tabs), active = scene->active_tab;
    int undo = acount(asset->undo), redo = acount(asset->redo);
    uint64_t step = scene_history_step_id(scene->history, false);
    char *raw = authority_action(
        test,
        AUTHORITY_SOURCE(1, 2),
        "package asset\n"
        "pub Build :: () -> byte[] {",
        collab_revision(),
        0,
        true
    );
    const char *kind = strstr(raw, "\"input_kind\":\"raw");
    REQUIRE(kind);
    size_t prefix = (size_t)(kind - raw) + strlen("\"input_kind\":\"");
    char *invalid = authority_keep(test, xmalloc(strlen(raw) + 16));
    memcpy(invalid, raw, prefix);
    strcpy(invalid + prefix, "properties");
    strcpy(invalid + prefix + strlen("properties"), raw + prefix + strlen("raw"));
    char error[256] = "";
    REQUIRE(!scene_collab_apply_action(scene, invalid, error, sizeof error));
    REQUIRE(*error && !asset->source && asset->prototype == model);
    REQUIRE(acount(scene->tabs) == tabs && scene->active_tab == active);
    REQUIRE(acount(asset->undo) == undo && acount(asset->redo) == redo);
    REQUIRE(scene_history_step_id(scene->history, false) == step);
    return bridge_field(scene, "AuthoritySource", "x", 1);
}

static bool authority_probe_reset(authority_fixture *test)
{
    collab_shutdown();
    scene_source_input_tick(test->s->scene);
    bool shared = scene_source_input_use_shared(test->s->scene, test->path);
    int reloaded = scene_source_input_reload(test->s->scene, authority_asset(test));
    REQUIRE(shared && !reloaded);
    return authority_text(test, AUTHORITY_SOURCE(1, 2), NULL);
}

/* The common optimistic-commit control must reach both real entry points.
 * Store the results independently; assert them only after outer cleanup. */
static bool authority_entry_probes(authority_fixture *test)
{
    REQUIRE(authority_start(test, "editor"));
    const void *step = asset_code_step(authority_asset(test)->source);
    authority_code_paste(test, AUTHORITY_SOURCE(91, 2));
    authority_code_draw(test);
    pin_value value = { 0 };
    bool model = !prototype_read_field(
                     &acceptance_field_reads,
                     authority_asset(test)->prototype,
                     "Transform",
                     "x",
                     &value
                 ) &&
                 value.type == PIN_F32 && value.f == 1;
    test->code_authority = model &&
                           !strcmp(
                               asset_code_text(authority_asset(test)->source),
                               AUTHORITY_SOURCE(1, 2)
                           ) &&
                           asset_code_step(authority_asset(test)->source) == step;
    REQUIRE(authority_probe_reset(test));

    const ecs_component_desc *transform = component_descriptor(test->s->scene, "Transform");
    REQUIRE(transform);
    int scalar = -1;
    for (int i = 0; i < transform->type->field_count; i++) {
        if (!strcmp(transform->type->fields[i].name, "x")) {
            scalar = i;
        }
    }
    REQUIRE(scalar >= 0 && authority_start(test, "editor"));
    step = asset_code_step(authority_asset(test)->source);
    REQUIRE(bridge_property(test->s, transform, scalar, "AuthoritySource", "", "92", false));
    model = !prototype_read_field(
                &acceptance_field_reads,
                authority_asset(test)->prototype,
                "Transform",
                "x",
                &value
            ) &&
            value.type == PIN_F32 && value.f == 1;
    test->property_authority = model &&
                               !strcmp(
                                   asset_code_text(authority_asset(test)->source),
                                   AUTHORITY_SOURCE(1, 2)
                               ) &&
                               asset_code_step(authority_asset(test)->source) == step;
    return authority_probe_reset(test);
}

static bool authority_decoder_rejections(authority_fixture *test)
{
    char *valid = authority_action(
        test,
        AUTHORITY_SOURCE(2, 2),
        AUTHORITY_SOURCE(9, 9),
        collab_revision(),
        0,
        true
    );
    size_t length = strlen(valid);
    char *oversize = authority_keep(test, xmalloc(COLLAB_MAX_ACTION + length + 2));
    memset(oversize, ' ', COLLAB_MAX_ACTION + 1);
    memcpy(oversize + COLLAB_MAX_ACTION + 1, valid, length + 1);
    char *nul = authority_keep(test, xmalloc(length + 32));
    const char *name = strstr(valid, "\"prototype\":\"AuthoritySource");
    REQUIRE(name);
    size_t prefix = (size_t)(name - valid) + strlen("\"prototype\":\"AuthoritySource");
    memcpy(nul, valid, prefix);
    strcpy(nul + prefix, "\\u0000Hidden");
    strcpy(nul + prefix + strlen("\\u0000Hidden"), valid + prefix);
    char *empty = authority_keep(test, xstrdup(valid));
    char *group = strstr(empty, "\"input_group\":\"");
    REQUIRE(group);
    group += strlen("\"input_group\":\"");
    char *end = strchr(group, '"');
    REQUIRE(end);
    memmove(group, end, strlen(end) + 1);
    const char *invalid[] = { oversize, nul, empty };
    const void *step = asset_code_step(authority_asset(test)->source);
    for (int i = 0; i < 3; i++) {
        char error[256] = "";
        REQUIRE(!scene_collab_apply_action(test->s->scene, invalid[i], error, sizeof error));
        REQUIRE(*error);
        REQUIRE(authority_text(test, AUTHORITY_SOURCE(2, 2), NULL));
        REQUIRE(asset_code_step(authority_asset(test)->source) == step);
        REQUIRE(bridge_field(test->s->scene, "AuthoritySource", "x", 2));
    }
    return true;
}

static bool authority_pending_and_history(authority_fixture *test)
{
    app_scene_authoring *scene = test->s->scene;
    REQUIRE(authority_start(test, "host"));
    REQUIRE(authority_raw(test, AUTHORITY_SOURCE(2, 2)));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(2, 2), NULL));
    const void *host_step = asset_code_step(authority_asset(test)->source);
    REQUIRE(authority_echo(test));
    REQUIRE(test->applies == 0 && asset_code_step(authority_asset(test)->source) == host_step);
    REQUIRE(!scene_source_input_any(scene));

    REQUIRE(authority_start(test, "editor"));
    REQUIRE(authority_decoder_rejections(test));
    const void *before_step = asset_code_step(authority_asset(test)->source);
    authority_code_paste(test, AUTHORITY_SOURCE(3, 2));
    authority_code_draw(test);
    /* Keep this exact canonical/history guard as well as the independent
     * entry probes. Its final REQUIRE runs after fixture/file cleanup. */
    test->source_guard = !strcmp(
                             asset_code_text(authority_asset(test)->source),
                             AUTHORITY_SOURCE(2, 2)
                         ) &&
                         asset_code_step(authority_asset(test)->source) == before_step;
    if (!test->source_guard) {
        return false;
    }
    authority_code_paste(test, AUTHORITY_SOURCE(4, 2));
    authority_code_draw(test);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(2, 2), AUTHORITY_SOURCE(4, 2)));
    REQUIRE(bridge_field(scene, "AuthoritySource", "x", 2));
    const prototype *preview = scene_source_input_preview(scene, authority_asset(test));
    pin_value preview_x = { 0 };
    REQUIRE(
        preview &&
        !prototype_read_field(&acceptance_field_reads, preview, "Transform", "x", &preview_x)
    );
    REQUIRE(preview_x.type == PIN_F32 && preview_x.f == 4);
    REQUIRE(scene_source_input_save(scene, authority_asset(test)) == APP_PROTOTYPE_EDIT_PENDING);
    REQUIRE(!studio_code_history(test->s, false, true));
    REQUIRE(scene_source_input_reload(scene, authority_asset(test)) == APP_PROTOTYPE_EDIT_PENDING);
    REQUIRE(asset_code_step(authority_asset(test)->source) == before_step);
    char *disk = authority_keep(test, file_read_text(test->path));
    REQUIRE(disk && !strcmp(disk, AUTHORITY_SOURCE(1, 2)));

    /* A real paste exists only in the widget when the echo arrives. It must
     * survive replay and be reported exactly once by the scoped Code input. */
    authority_code_paste(test, AUTHORITY_SOURCE(5, 2));
    REQUIRE(authority_echo(test));
    REQUIRE(!strcmp(
        kit_code_text(asset_code_editor(authority_asset(test)->source), NULL),
        AUTHORITY_SOURCE(5, 2)
    ));
    authority_code_draw(test);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(3, 2), AUTHORITY_SOURCE(5, 2)));
    collab_action_identity pending;
    REQUIRE(collab_pending_action(&pending));
    uint64_t revision = collab_revision();
    REQUIRE(authority_queue(test, "local", pending.seq, revision + 1, "accepted", test->action));
    char *remote = authority_action(
        test,
        AUTHORITY_SOURCE(4, 2),
        AUTHORITY_SOURCE(4, 9),
        revision + 1,
        0,
        true
    );
    REQUIRE(authority_queue(test, "peer", test->peer, revision + 2, "accepted", remote));
    REQUIRE(authority_pump(test));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(4, 9), AUTHORITY_SOURCE(5, 9)));
    REQUIRE(authority_echo(test));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(5, 9), NULL));
    REQUIRE(!scene_source_input_any(scene));
    REQUIRE(!asset_code_runtime_held(authority_asset(test)->source));
    test->s->prefs.code_apply_live = false;
    REQUIRE(authority_history(test, false, AUTHORITY_SOURCE(4, 9)));
    REQUIRE(asset_code_runtime_held(authority_asset(test)->source));
    test->s->prefs.code_apply_live = true;
    REQUIRE(authority_history(test, true, AUTHORITY_SOURCE(5, 9)));
    REQUIRE(!asset_code_runtime_held(authority_asset(test)->source));
    REQUIRE(authority_peer(test, AUTHORITY_SOURCE(4, 9), -1, true));
    char *target = authority_keep(
        test,
        asset_code_history_target(authority_asset(test)->source, false)
    );
    REQUIRE(target && !strcmp(target, AUTHORITY_SOURCE(5, 9)));
    REQUIRE(authority_history(test, false, AUTHORITY_SOURCE(5, 9)));
    return true;
}

static bool authority_conflicts(authority_fixture *test)
{
    app_scene_authoring *scene = test->s->scene;
    char message[256];
    REQUIRE(!asset_code_runtime_held(authority_asset(test)->source));
    test->s->prefs.code_apply_live = false;
    authority_code_paste(test, AUTHORITY_SOURCE(6, 9));
    authority_code_draw(test);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(5, 9), AUTHORITY_SOURCE(6, 9)));
    REQUIRE(!asset_code_runtime_held(authority_asset(test)->source));
    REQUIRE(authority_reject(test));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(5, 9), AUTHORITY_SOURCE(6, 9)));
    REQUIRE(scene_source_input_conflict(scene, message, sizeof message));
    int sent = test->sent;
    /* The conflict tail arrives before the next draw. Harvest must keep the
     * managed widget's held intent, rather than the still-live canonical flag. */
    authority_code_paste(test, AUTHORITY_SOURCE(7, 9));
    scene_source_input_harvest(scene);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(5, 9), AUTHORITY_SOURCE(7, 9)));
    REQUIRE(test->sent == sent);
    REQUIRE(scene_source_input_retry(scene, test->path) == APP_PROTOTYPE_EDIT_PENDING);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(5, 9), AUTHORITY_SOURCE(7, 9)));
    yyjson_doc *retry = yyjson_read(test->action, strlen(test->action), 0);
    yyjson_val *retry_live = yyjson_obj_get(yyjson_doc_get_root(retry), "live");
    test->retry_held = yyjson_is_bool(retry_live) && !yyjson_get_bool(retry_live);
    yyjson_doc_free(retry);
    if (!test->retry_held) {
        return false;
    }
    REQUIRE(authority_echo(test));
    REQUIRE(asset_code_runtime_held(authority_asset(test)->source));
    test->s->prefs.code_apply_live = true;
    REQUIRE(!authority_raw(test, AUTHORITY_SOURCE(8, 9)));
    REQUIRE(authority_reject(test));
    REQUIRE(scene_source_input_use_shared(scene, test->path));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(7, 9), NULL));

    /* Another caller occupies the one transport slot. This opaque proposal
     * is rejected, never passed to the production action decoder. */
    REQUIRE(collab_propose("{}"));
    REQUIRE(!authority_raw(test, AUTHORITY_SOURCE(8, 9)));
    REQUIRE(authority_peer(test, AUTHORITY_SOURCE(7, 10), 0, true));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(7, 10), AUTHORITY_SOURCE(8, 10)));
    REQUIRE(authority_reject(test));
    REQUIRE(authority_echo(test));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(8, 10), NULL));
    REQUIRE(collab_propose("{}"));
    REQUIRE(!authority_raw(test, AUTHORITY_SOURCE(9, 10)));
    REQUIRE(authority_peer(test, AUTHORITY_SOURCE(10, 10), 0, true));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(10, 10), AUTHORITY_SOURCE(9, 10)));
    REQUIRE(scene_source_input_conflict(scene, message, sizeof message));
    REQUIRE(authority_reject(test));
    REQUIRE(scene_source_input_use_shared(scene, test->path));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(10, 10), NULL));

    const char *invalid[] = {
        "package asset\n"
        "pub Build :: () -> byte[] {",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"DifferentIdentity\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(99)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(99))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    for (int i = 0; i < 2; i++) {
        REQUIRE(!authority_raw(test, invalid[i]));
        REQUIRE(authority_echo(test));
        REQUIRE(authority_text(test, invalid[i], NULL));
        REQUIRE(asset_code_error(authority_asset(test)->source)[0]);
        pin_value rejected = { 0 };
        REQUIRE(
            app_scene_authoring_read_prototype_field(
                &acceptance_field_reads,
                scene,
                "AuthoritySource",
                "Transform",
                "x",
                &rejected
            ) < 0
        );
        REQUIRE(authority_model_x(test, 10));
        REQUIRE(
            text_overlay_text(test->path) &&
            !strcmp(text_overlay_text(test->path), AUTHORITY_SOURCE(10, 10))
        );
        REQUIRE(authority_history(test, false, AUTHORITY_SOURCE(10, 10)));
    }
    return true;
}

static bool authority_properties(authority_fixture *test)
{
    app_scene_authoring *scene = test->s->scene;
    REQUIRE(!scene_source_input_any(scene));
    collab_shutdown();
    REQUIRE(!app_scene_authoring_add_prototype_component(scene, "AuthoritySource", "Aim"));
    char *before = authority_keep(
        test,
        xstrdup(asset_code_text(authority_asset(test)->source))
    );
    REQUIRE(authority_start(test, "editor"));
    const ecs_component_desc *transform = component_descriptor(scene, "Transform");
    const ecs_component_desc *visual = component_descriptor(scene, "Aim");
    REQUIRE(transform && visual);
    int scalar = -1, vector_field = -1;
    for (int i = 0; i < transform->type->field_count; i++) {
        if (!strcmp(transform->type->fields[i].name, "x")) {
            scalar = i;
        }
    }
    pin_value vector = { 0 };
    const type_desc *vector_type = NULL;
    for (int i = 0; i < visual->type->field_count && !vector_type; i++) {
        REQUIRE(!prototype_read_field(
            &acceptance_field_reads,
            authority_asset(test)->prototype,
            visual->name,
            visual->type->fields[i].name,
            &vector
        ));
        vector_type = properties_vector(&vector);
        if (vector_type) {
            vector_field = i;
        }
    }
    REQUIRE(scalar >= 0 && vector_type && vector_type->field_count >= 2);
    const char *axis0 = vector_type->fields[0].name,
               *axis1 = vector_type->fields[1].name;
    pin_value before0 = { 0 }, before1 = { 0 };
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[0], &before0)
    );
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[1], &before1)
    );
    REQUIRE(before0.type == PIN_F32 && before1.type == PIN_F32);
    REQUIRE(bridge_property(test->s, transform, scalar, "AuthoritySource", "", "11", true));
    REQUIRE(bridge_property(test->s, transform, scalar, "AuthoritySource", "", "12", false));
    char *scalar_text = authority_keep(
        test,
        xstrdup(scene_source_input_text(scene, authority_asset(test)))
    );
    REQUIRE(bridge_property(test->s, visual, vector_field, "AuthoritySource", axis0, "6", true));
    REQUIRE(bridge_property(test->s, visual, vector_field, "AuthoritySource", axis0, "8", false));
    char *axis0_text = authority_keep(
        test,
        xstrdup(scene_source_input_text(scene, authority_asset(test)))
    );
    REQUIRE(bridge_property(test->s, visual, vector_field, "AuthoritySource", axis1, "7", false));
    char *both_axes = authority_keep(
        test,
        xstrdup(scene_source_input_text(scene, authority_asset(test)))
    );
    test->source_guard &= !strcmp(asset_code_text(authority_asset(test)->source), before);
    if (!test->source_guard) {
        return false;
    }
    REQUIRE(bridge_field(scene, "AuthoritySource", "x", 10));
    pin_value first = { 0 }, second = { 0 };
    REQUIRE(!prototype_read_field(
        &acceptance_field_reads,
        authority_asset(test)->prototype,
        visual->name,
        visual->type->fields[vector_field].name,
        &vector
    ));
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[0], &first)
    );
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[1], &second)
    );
    REQUIRE(
        first.type == PIN_F32 && first.f == before0.f &&
        second.type == PIN_F32 && second.f == before1.f
    );
    const prototype *preview = scene_source_input_preview(scene, authority_asset(test));
    pin_value preview_x = { 0 };
    REQUIRE(
        preview &&
        !prototype_read_field(&acceptance_field_reads, preview, "Transform", "x", &preview_x)
    );
    REQUIRE(preview_x.type == PIN_F32 && preview_x.f == 12);
    REQUIRE(!prototype_read_field(
        &acceptance_field_reads,
        preview,
        visual->name,
        visual->type->fields[vector_field].name,
        &vector
    ));
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[0], &first)
    );
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[1], &second)
    );
    REQUIRE(first.type == PIN_F32 && first.f == 8 && second.type == PIN_F32 && second.f == 7);
    REQUIRE(authority_echo(test));
    REQUIRE(authority_echo(test));
    char *scalar_undo = authority_keep(
        test,
        asset_code_history_target(authority_asset(test)->source, false)
    );
    REQUIRE(scalar_undo && !strcmp(scalar_undo, before));
    REQUIRE(authority_text(test, scalar_text, both_axes));
    REQUIRE(authority_echo(test));
    REQUIRE(authority_text(test, axis0_text, both_axes));
    REQUIRE(authority_echo(test));
    REQUIRE(authority_text(test, both_axes, NULL));
    REQUIRE(bridge_field(scene, "AuthoritySource", "x", 12));
    REQUIRE(!prototype_read_field(
        &acceptance_field_reads,
        authority_asset(test)->prototype,
        visual->name,
        visual->type->fields[vector_field].name,
        &vector
    ));
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[0], &first)
    );
    REQUIRE(
        !pin_read_field(&acceptance_field_reads, vector.ptr, &vector_type->fields[1], &second)
    );
    REQUIRE(first.type == PIN_F32 && first.f == 8 && second.type == PIN_F32 && second.f == 7);
    REQUIRE(authority_history(test, false, axis0_text));
    REQUIRE(authority_history(test, false, scalar_text));
    REQUIRE(authority_history(test, false, before));
    return true;
}

static bool authority_checkpoint_and_limits(authority_fixture *test)
{
    app_scene_authoring *scene = test->s->scene;
    char *canonical = authority_keep(
        test,
        xstrdup(asset_code_text(authority_asset(test)->source))
    );
    char *undo = authority_keep(
        test,
        asset_code_history_target(authority_asset(test)->source, false)
    );
    size_t size = strlen(canonical) + 128;
    char *draft = authority_keep(test, xmalloc(size));
    snprintf(draft, size, "// LOCAL_DRAFT_NOT_IN_CHECKPOINT\n%s", canonical);
    REQUIRE(!authority_raw(test, draft));
    char *checkpoint = authority_keep(test, app_scene_authoring_checkpoint(scene));
    REQUIRE(checkpoint && !strstr(checkpoint, "LOCAL_DRAFT_NOT_IN_CHECKPOINT"));
    REQUIRE(authority_snapshot(test, checkpoint));
    REQUIRE(authority_text(test, canonical, draft));
    REQUIRE(
        scene_source_input_any(scene) && asset_code_dirty(authority_asset(test)->source)
    );
    char message[256];
    REQUIRE(scene_source_input_conflict(scene, message, sizeof message));
    char *after_undo = authority_keep(
        test,
        asset_code_history_target(authority_asset(test)->source, false)
    );
    REQUIRE(undo && after_undo && !strcmp(undo, after_undo));
    REQUIRE(scene_source_input_use_shared(scene, test->path));
    REQUIRE(authority_text(test, canonical, NULL));

    REQUIRE(collab_propose("{}"));
    for (int i = 0; i < 33; i++) {
        snprintf(draft, size, "// BOUNDED_LOCAL_GROUP_%d\n%s", i, canonical);
        REQUIRE(!authority_raw(test, draft));
    }
    REQUIRE(authority_text(test, canonical, draft));
    REQUIRE(scene_source_input_conflict(scene, message, sizeof message));
    REQUIRE(strstr(message, "limit"));
    REQUIRE(authority_reject(test));
    REQUIRE(scene_source_input_use_shared(scene, test->path));
    REQUIRE(authority_start(test, "viewer"));
    int sent = test->sent;
    REQUIRE(!authority_raw(test, draft));
    REQUIRE(authority_text(test, canonical, draft));
    REQUIRE(scene_source_input_conflict(scene, message, sizeof message));
    REQUIRE(scene_source_input_retry(scene, test->path) == APP_PROTOTYPE_EDIT_PENDING);
    REQUIRE(test->sent == sent && !collab_can_edit());
    REQUIRE(scene_source_input_use_shared(scene, test->path));
    REQUIRE(authority_text(test, canonical, NULL));
    char *disk = authority_keep(test, file_read_text(test->path));
    REQUIRE(disk && !strcmp(disk, AUTHORITY_SOURCE(1, 2)));
    REQUIRE(test->received == 36 && test->sent == 59);
    REQUIRE(
        test->proposals == 27 && test->accepted == 1 && test->acks == 24 &&
        test->snapshots == 7
    );
    REQUIRE(test->applies == 23 && test->restores == 7 && test->restored == 1);
    REQUIRE(test->compositions == 5);
    REQUIRE(acount(scene->actors) == 0 && acount(scene->prototypes) == 1);
    return true;
}

static bool authority_frame_and_disk(authority_fixture *test)
{
    studio *s = test->s;
    app_scene_authoring *scene = s->scene;
    collab_shutdown();
    REQUIRE(!app_scene_authoring_open_prototype(scene, "AuthoritySource"));
    REQUIRE(authority_start(test, "editor"));
    s->prefs.code_apply_live = true;
    REQUIRE(authority_peer(test, AUTHORITY_SOURCE(20, 10), 0, false));
    asset_code *code = authority_asset(test)->source;
    REQUIRE(asset_code_runtime_held(code));
    const void *step = asset_code_step(code);
    studio_code_frame(s);
    test->frame_held = asset_code_runtime_held(code);
    REQUIRE(asset_code_step(code) == step);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(20, 10), NULL));

    s->prefs.code_apply_live = false;
    REQUIRE(authority_peer(test, AUTHORITY_SOURCE(21, 10), 0, true));
    REQUIRE(!asset_code_runtime_held(code));
    step = asset_code_step(code);
    studio_code_frame(s);
    test->frame_live = !asset_code_runtime_held(code);
    REQUIRE(asset_code_step(code) == step);
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(21, 10), NULL));
    REQUIRE(!asset_code_error(code)[0] && asset_code_canonical_dirty(code));

    /* Overlap the unsaved canonical edit. Managed polling must keep its
     * text/model/history and expose the disk diagnostic until explicit Reload. */
    REQUIRE(!write_file_atomic(test->path, AUTHORITY_SOURCE(30, 2)));
    s->time += 1;
    studio_code_frame(s);
    test->disk_conflict = strstr(asset_code_error(code), "conflicts with a disk edit") !=
                          NULL;
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(21, 10), NULL));
    REQUIRE(asset_code_step(code) == step && asset_code_canonical_dirty(code));
    REQUIRE(authority_model_x(test, 21));
    REQUIRE(!scene_source_input_any(scene));
    s->time += 1;
    studio_code_frame(s);
    test->disk_conflict &= strstr(asset_code_error(code), "conflicts with a disk edit") !=
                           NULL;
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(21, 10), NULL));
    REQUIRE(asset_code_step(code) == step && asset_code_canonical_dirty(code));
    REQUIRE(scene_source_input_reload(scene, authority_asset(test)) == APP_PROTOTYPE_EDIT_PENDING);
    REQUIRE(authority_echo(test));
    REQUIRE(authority_text(test, AUTHORITY_SOURCE(30, 2), NULL));
    REQUIRE(!asset_code_error(code)[0] && !asset_code_dirty(code));
    REQUIRE(bridge_field(scene, "AuthoritySource", "x", 30));
    REQUIRE(!scene_source_input_any(scene));
    REQUIRE(test->received == 40 && test->sent == 64);
    REQUIRE(
        test->proposals == 28 && test->accepted == 1 && test->acks == 27 &&
        test->snapshots == 8
    );
    REQUIRE(test->applies == 26 && test->restores == 8 && test->restored == 1);
    REQUIRE(test->compositions == 5);
    REQUIRE(acount(scene->actors) == 0 && acount(scene->prototypes) == 1);
    return true;
}

static bool prototype_source_authority_contract(studio *host)
{
    REQUIRE(host && host->kit && !collab_active());
    char root[] = "/tmp/chibi-source-authority-XXXXXX", directory[256], path[256];
    REQUIRE(mkdtemp(root));
    snprintf(directory, sizeof directory, "%s/prototypes", root);
    bool made = !mkdir(directory, 0700);
    snprintf(path, sizeof path, "%s/AuthoritySource.chibi", directory);
    bool written = made && !write_file_atomic(path, AUTHORITY_SOURCE(1, 2));
    if (!written) {
        if (made) {
            rmdir(directory);
        }
        rmdir(root);
    }
    REQUIRE(written);
    move_fixture fixture = move_fixture_new_with_root(root);
    studio *s = xcalloc(1, sizeof *s);
    s->scene = fixture.scene;
    s->kit = host->kit;
    s->backend = host->backend;
    s->in = host->in;
    s->prefs = host->prefs;
    s->key_area = -1;
    s->focus_area = STUDIO_AREA_TOP;
    s->graph.area = -1;
    s->prefs.layouts[0].editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_CODE;
    s->prefs.code_apply_live = true;
    authority_fixture test = { .s = s,
                               .path = path,
                               .valid = true,
                               .source_guard = true,
                               .retry_held = -1,
                               .frame_held = -1,
                               .frame_live = -1,
                               .disk_conflict = -1 };
    bool ok = !app_scene_authoring_register_prototype(fixture.scene, AUTHORITY_SOURCE(1, 2), path) &&
              authority_unopened_rejection(&test) &&
              !app_scene_authoring_open_prototype(fixture.scene, "AuthoritySource");
    if (ok) {
        ok = authority_entry_probes(&test) && authority_pending_and_history(&test) &&
             authority_conflicts(&test) && authority_properties(&test) &&
             authority_checkpoint_and_limits(&test) &&
             authority_frame_and_disk(&test);
    }
    collab_shutdown();
    studio_code_free(s);
    afree(s->records);
    free(s);
    move_fixture_free(&fixture);
    free(test.action);
    for (int i = 0; i < acount(test.owned); i++) {
        free(test.owned[i]);
    }
    afree(test.owned);
    bool clean = !unlink(path);
    clean &= !rmdir(directory);
    clean &= !rmdir(root);
    printf("Prototype source authority: Code unchanged=%d, Properties unchanged=%d\n", test.code_authority, test.property_authority);
    printf("Prototype source intent: retry_held=%d\n", test.retry_held);
    printf("Prototype source frame: held=%d, live=%d\n", test.frame_held, test.frame_live);
    printf("Prototype source disk: conflict=%d\n", test.disk_conflict);
    REQUIRE(clean);
    REQUIRE(test.code_authority && test.property_authority);
    REQUIRE(test.source_guard);
    REQUIRE(test.retry_held == 1);
    REQUIRE(test.frame_held == 1 && test.frame_live == 1);
    REQUIRE(test.disk_conflict == 1);
    REQUIRE(ok);
    return true;
}

#undef AUTHORITY_SOURCE
