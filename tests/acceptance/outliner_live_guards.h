/* Include after outliner_live.h. This separate route uses its production setup
 * and cleanup, but does not execute or change the original twelve scenarios.
 * Four collection preparations: empty, original, and two behavior-only edits.
 * Four newly prepared modules; the library is reused for both edits, so its
 * 22 named constants are evaluated once (compiler budget 100,000 per frame).
 * Exactly eight adapter sends, one deferred RevealObject dispatch, two empty
 * Begin handlers, no pumps/resumes. Ordinary controls retain 10,000 per frame;
 * Refresh/Reveal share the request allowance granted from captured facts.
 * The same two mounts and one-actor worlds use two initial mount syncs, with
 * no document show, layout, context update, render, frames or file writes.
 * The production backend initializes its sixteen embedded font faces.
 */
static char *outliner_live_guards_original;

static void outliner_live_guards_cleanup()
{
    outliner_live_cleanup();
    free(outliner_live_guards_original);
    outliner_live_guards_original = NULL;
}

/* Replace one literal only inside the final production RevealObject handler.
 * These two fixed edits preserve the guard's exact event/state/module contract. */
static char *outliner_live_guards_edit(const char *source, const char *from, const char *to)
{
    const char *root = strstr(source, "    on RevealObject {");
    const char *found = root ? strstr(root, from) : NULL;
    if (!found || strstr(found + strlen(from), from)) {
        return NULL;
    }
    size_t before = (size_t)(found - source), remove = strlen(from), add = strlen(to);
    char *result = xmalloc(strlen(source) - remove + add + 1);
    memcpy(result, source, before);
    memcpy(result + before, to, add);
    strcpy(result + before + add, found + remove);
    return result;
}

static bool outliner_live_guards_publish(int revision, char *text)
{
    REQUIRE(text);
    free(outliner_live.behavior);
    outliner_live.behavior = text;
    REQUIRE(outliner_live_prepare(revision));
    REQUIRE(authoring_sources_reused(outliner_live.prepared[revision]) == 1);
    REQUIRE(
        preview_sources_publish(
            &outliner_live.ui_live,
            NULL,
            outliner_live.error,
            sizeof outliner_live.error
        ) == 0
    );
    REQUIRE(
        execution_program_module(source_program(outliner_live.sources, "OutlinerRows")) ==
        outliner_live.modules[revision]
    );
    return true;
}

static bool outliner_live_guards_hidden_reveal()
{
    const render_ui_host host = authoring_ui_events(&outliner_live.ui_live);
    ui_activation activation = { .event = "RevealObject", .argc = 1 };
    snprintf(activation.args[0], sizeof activation.args[0], "%s", outliner_live.actor_key);
    behavior_run_scope runs = { 0 };
    behavior_run_scope_begin(&runs);
    outliner_live.sends++;
    bool carried = studio_outliner_projection_send(
        outliner_live.projection,
        &host,
        (ecs_entity){ 0 },
        &activation,
        NULL,
        false,
        0,
        0,
        0
    );
    behavior_run_scope_end(&runs);
    outliner_live_count_runs(&runs);
    REQUIRE(carried && runs.counts.bytecode_entries == 0);
    /* The deferred typed payload must own these bytes after return. */
    memset(activation.args[0], 'x', strlen(activation.args[0]));
    return true;
}

static void outliner_live_guards_bind()
{
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[0],
        ecs_entity_to_u64(outliner_live.controllers[0])
    );
}

static bool outliner_live_guards_protected_state()
{
    static const char *const names[] = { "First", "Count", "Collapsed", "ShownOpen" };
    execution_program *program = source_program(outliner_live.sources, "OutlinerRows");
    execution_instance *instance = execution_program_instance(
        program,
        ecs_entity_to_u64(outliner_live.controllers[0])
    );
    for (int i = 0; i < 4; i++) {
        char identity[96];
        snprintf(identity, sizeof identity, "behaviors/OutlinerRows.chibi:1:%s", names[i]);
        const pin_value *value = execution_instance_state(instance, script_catalog_id("symbol", identity));
        REQUIRE(value && value->container == PIN_CONTAINER_SINGLE);
        if (i < 2) {
            REQUIRE(value->type == PIN_I32 && value->i == (i == 1 ? 3 : 0));
        } else {
            REQUIRE(value->type == PIN_BOOL && value->b == (i == 3));
        }
    }
    return true;
}

TEST_CASE(outliner_live_guards_epoch_replay_and_repeated_calls)
{
    REQUIRE(outliner_live_setup());
    outliner_live_guards_original = xstrdup(outliner_live.behavior);
    app_scene_authoring *scene = outliner_live.s->scene;
    ecs_world *world = scene->world;
    const char *window[] = { "0", "3" };
    const char *reveal[] = { outliner_live.actor_key };
    REQUIRE(outliner_live_event(0, 0, "ShowRows", 2, window));
    REQUIRE(
        ui_revision(outliner_live.model) == 0 &&
        ui_element_count(outliner_live.model) == 0
    );

    /* The real host increments this exposed scene generation when loading a
     * scene (scene_authoring_load_value), even when both allocations survive.
     * Only that test-owned input changes here; no dispatch proof is created. */
    outliner_live_enqueue(0, "RevealObject", 1, reveal);
    uint64_t epoch = scene->debug_sessions[APP_RUNTIME_SCENE];
    scene->debug_sessions[APP_RUNTIME_SCENE]++;
    REQUIRE(outliner_live.s->scene == scene && scene->world == world);
    REQUIRE(outliner_live_deliver(0, 0, "RevealObject", true));
    REQUIRE(
        ui_revision(outliner_live.model) == 0 &&
        ui_element_count(outliner_live.model) == 0
    );

    /* Both old bindings are now stale. Native reveal is retained without
     * authority, then another same-pointer epoch makes that intent obsolete. */
    REQUIRE(outliner_live_guards_hidden_reveal());
    scene->debug_sessions[APP_RUNTIME_SCENE]++;
    REQUIRE(scene->debug_sessions[APP_RUNTIME_SCENE] == epoch + 2);
    REQUIRE(outliner_live.s->scene == scene && scene->world == world);
    outliner_live_guards_bind();
    REQUIRE(outliner_live_event(0, 0, "Refresh", 0, NULL));
    REQUIRE(outliner_live_projection(0, "Alpha"));
    REQUIRE(
        ui_revision(outliner_live.model) == 11 &&
        ui_element_count(outliner_live.model) == 0
    );
    REQUIRE(!studio_outliner_projection_replay_error(outliner_live.projection)[0]);
    const av_value *owned_rows = outliner_live_value(0, "rows");

    /* Both captures remain live in the authored data flow: the first supplies
     * arrays, the second supplies Scene. The second call must fail before any
     * publication, and the already-owned model must remain byte-for-byte equal. */
    char *twice = outliner_live_guards_edit(
        outliner_live_guards_original,
        "        captured := capture_outliner()\n",
        "        captured := capture_outliner()\n"
        "        capturedAgain := capture_outliner()\n"
    );
    REQUIRE(twice);
    char *capture = outliner_live_guards_edit(twice, "captured.Scene", "capturedAgain.Scene");
    free(twice);
    REQUIRE(outliner_live_guards_publish(1, capture));
    studio_outliner_projection_unmount(outliner_live.projection, outliner_live.mounts[0]);
    studio_outliner_projection_unmount(outliner_live.projection, outliner_live.mounts[1]);
    REQUIRE(outliner_live_guards_hidden_reveal());
    outliner_live_guards_bind();
    outliner_live_enqueue(0, "FoldRoot", 0, NULL);
    outliner_live_enqueue(0, "ToggleRoot", 0, NULL);
    uint64_t revision = ui_revision(outliner_live.model);
    REQUIRE(outliner_live_deliver(0, 0, "FoldRoot", true));
    REQUIRE(outliner_live_collapsed(0, true));
    REQUIRE(ui_mount_pending(outliner_live.mounts[0]));
    REQUIRE(
        strstr(studio_outliner_projection_replay_error(outliner_live.projection), "An Outliner request may capture only once.")
    );
    REQUIRE(!source_error(outliner_live.sources)[0]);
    REQUIRE(
        ui_revision(outliner_live.model) == revision &&
        ui_element_count(outliner_live.model) == 0
    );
    REQUIRE(
        outliner_live_value(0, "rows") == owned_rows &&
        outliner_live_rows(owned_rows, "Alpha")
    );
    REQUIRE(outliner_live_deliver(0, 0, "ToggleRoot", true));
    REQUIRE(outliner_live_collapsed(0, false));
    REQUIRE(!ui_mount_pending(outliner_live.mounts[0]));
    REQUIRE(!studio_outliner_projection_replay_error(outliner_live.projection)[0]);
    REQUIRE(ui_revision(outliner_live.model) == revision);

    /* A second Publish has identical protected inputs and a reveal command.
     * If accepted, it would add another model revision and set Collapsed true.
     * The marker makes the returned false result observable without a host
     * service replacement or inspection of the private request record. */
    char *publish = outliner_live_guards_edit(
        outliner_live_guards_original,
        "        if publish_outliner(projected.Projection, projected.Rows) {\n"
        "            ShownOpen = !Collapsed\n"
        "        }",
        "        if publish_outliner(projected.Projection, projected.Rows) {\n"
        "            ShownOpen = !Collapsed\n"
        "        }\n"
        "        acceptedAgain := publish_outliner(projected.Projection, "
        "projected.Rows)\n"
        "        if acceptedAgain {\n"
        "            Collapsed = true\n"
        "        }"
    );
    REQUIRE(outliner_live_guards_publish(2, publish));
    outliner_live_guards_bind();
    REQUIRE(outliner_live_guards_protected_state());
    REQUIRE(outliner_live_event(0, 0, "RevealObject", 1, reveal));
    REQUIRE(outliner_live_guards_protected_state());
    REQUIRE(!source_error(outliner_live.sources)[0]);
    REQUIRE(!studio_outliner_projection_replay_error(outliner_live.projection)[0]);
    REQUIRE(ui_revision(outliner_live.model) == revision + 1);
    REQUIRE(
        ui_element_count(outliner_live.model) == 1 &&
        ui_element_at(outliner_live.model, 0)->reveal == 1
    );
    REQUIRE(outliner_live_projection(0, "Alpha"));
    REQUIRE(
        outliner_live_value(0, "rows") == owned_rows &&
        outliner_live_rows(owned_rows, "Alpha")
    );
    REQUIRE(
        outliner_live.preparations == 4 && outliner_live.sends == 8 &&
        outliner_live.pumps == 0
    );
    REQUIRE(
        outliner_live.backend_ok && outliner_live.invocations > 0 &&
        outliner_live.invocations + 2 < 1000
    );
    return true;
}

static int outliner_live_guards_main(const char *content_root)
{
    outliner_live_content = content_root;
    pu_setup(NULL, outliner_live_guards_cleanup);
    RUN_TEST_CASE(outliner_live_guards_epoch_replay_and_repeated_calls);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
