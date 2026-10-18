/* Include after outliner_live.h and outliner_live_guards.h. This standalone
 * route reuses their real adapter/mount/guard setup without running or changing
 * either existing scenario list. Per backend: four collection preparations
 * (empty, shipped, two behavior-only edits), four new modules, 22 constants,
 * two empty Begin roots, five adapter sends, and zero pumps/resumes.
 * One shipped Refresh captures 4096 flat actors and publishes 32 literal rows.
 * Two one-actor Reveal edits exhaust at a bounded loop before Project and after
 * accepted Publish. Each loop starts at 1 and stops below 32768 (<=32767 bodies).
 * No inspection, layout, update, rendering, app frames, source writes, child
 * processes, test hooks, token interception or relaxed producer validation.
 * Exact shared spend/grant accounting belongs to the engine authority fixture;
 * this route observes actual backend entries, failures and committed UI data.
 * K249 is qualified by the final-ABI census; installation and run admission
 * remain separate owner-controlled gates. */
enum
{
    OUTLINER_LIVE_FUEL_ACTORS = 4096,
    OUTLINER_LIVE_FUEL_ROWS = 32,
    /* Direct Project's 8585-entry bound plus handler entry/reentry. */
    OUTLINER_LIVE_FUEL_REFRESH_ENTRIES = 8587,
    /* One-actor Project has <=12 starts, hence <=23 entries, plus its handler. */
    OUTLINER_LIVE_FUEL_SMALL_ENTRIES = 25,
};
static char *outliner_live_fuel_original;
static execution_program_failure outliner_live_fuel_failures[2][2];

static void outliner_live_fuel_cleanup()
{
    outliner_live_cleanup();
    free(outliner_live_fuel_original);
    outliner_live_fuel_original = NULL;
}

static uint64_t outliner_live_fuel_fingerprint(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    }
    return hash;
}

static bool outliner_live_fuel_sources()
{
    /* Same exact lengths/FNV identities as the reviewed canonical census.
     * SHA-256 provenance is retained in the admission note, not computed here. */
    REQUIRE(strlen(outliner_live.behavior) == 1441);
    REQUIRE(outliner_live_fuel_fingerprint(outliner_live.behavior) == UINT64_C(0x8c5ca50476106b0b));
    REQUIRE(strlen(outliner_live.library) == 11668);
    REQUIRE(outliner_live_fuel_fingerprint(outliner_live.library) == UINT64_C(0x14e0b3fe818523ed));
    REQUIRE(studio_outliner_module_valid(
        outliner_live.modules[0],
        outliner_live.error,
        sizeof outliner_live.error
    ));
    return true;
}

static bool outliner_live_fuel_actors()
{
    studio_outliner_projection_invalidate(outliner_live.projection);
    app_scene_authoring *scene = outliner_live.scenes[0];
    REQUIRE(acount(scene->actors) == 1);
    for (int i = 0; i < OUTLINER_LIVE_FUEL_ACTORS; i++) {
        char name[16];
        snprintf(name, sizeof name, "Actor%04d", i);
        if (i) {
            ecs_entity entity = ecs_world_create_entity(scene->world);
            REQUIRE(entity.slot);
            apush(
                scene->actors,
                ((scene_actor){ .identity = (uint64_t)i + 1,
                                .entity = entity,
                                .name = xstrdup(name) })
            );
        } else {
            free(scene->actors[0].name);
            scene->actors[0].name = xstrdup(name);
        }
    }
    REQUIRE(acount(scene->actors) == OUTLINER_LIVE_FUEL_ACTORS);
    outliner_live_guards_bind();
    return true;
}

static bool outliner_live_fuel_header(int count)
{
    REQUIRE(!strcmp(av_get_str(outliner_live_value(0, "root.label")), "Live"));
    REQUIRE(!strcmp(av_get_str(outliner_live_value(0, "root.icon")), "\xee\xaf\x92"));
    REQUIRE(av_get_bool(outliner_live_value(0, "root.parent")));
    REQUIRE(av_get_int(outliner_live_value(0, "root.depth")) == 0);
    REQUIRE(av_get_bool(outliner_live_value(0, "root.open")));
    REQUIRE(!av_get_bool(outliner_live_value(0, "rootShown")));
    REQUIRE(av_get_int(outliner_live_value(0, "count")) == count);
    REQUIRE(av_get_int(outliner_live_value(0, "total")) == count + 1);
    REQUIRE(!av_get_bool(outliner_live_value(0, "filtered")));
    REQUIRE(!strcmp(av_get_str(outliner_live_value(0, "notice")), ""));
    return true;
}

static bool outliner_live_fuel_large_rows(const av_value *rows)
{
    REQUIRE(rows && av_is_arr(rows) && av_arr_size(rows) == OUTLINER_LIVE_FUEL_ROWS);
    static const char *const flags[] = { "active", "marked", "hidden", "locked", "renaming" };
    for (int i = 0; i < OUTLINER_LIVE_FUEL_ROWS; i++) {
        const av_value *row = av_arr_get(rows, i);
        REQUIRE(av_is_obj(row) && av_obj_size(row) == 12);
        char key[32], label[16];
        snprintf(
            key,
            sizeof key,
            "%llu",
            (unsigned long long)
                ecs_entity_to_u64(outliner_live.scenes[0]->actors[i].entity)
        );
        snprintf(label, sizeof label, "Actor%04d", i);
        REQUIRE(!strcmp(av_get_str(av_obj_get(row, "key")), key));
        REQUIRE(!strcmp(av_get_str(av_obj_get(row, "label")), label));
        REQUIRE(!strcmp(av_get_str(av_obj_get(row, "icon")), "\xf3\xb0\x86\xa7"));
        REQUIRE(!strcmp(av_get_str(av_obj_get(row, "prototype")), ""));
        REQUIRE(!strcmp(av_get_str(av_obj_get(row, "drop")), "object behavior"));
        REQUIRE(av_get_int(av_obj_get(row, "depth")) == 1);
        REQUIRE(av_get_int(av_obj_get(row, "behaviors")) == 0);
        for (int f = 0; f < 5; f++) {
            const av_value *value = av_obj_get(row, flags[f]);
            REQUIRE(value && av_is_bool(value) && !av_get_bool(value));
        }
    }
    return true;
}

static bool outliner_live_fuel_before_source()
{
    static const char
        before[] = "        fuelWork: int = length(captured.Actors)\n"
                   "        while fuelWork < 32768 {\n"
                   "            fuelWork = fuelWork + 1\n"
                   "        }\n"
                   "        projected := OutlinerRowPolicies.Project(\n";
    char *loop = outliner_live_guards_edit(
        outliner_live_fuel_original,
        "        projected := OutlinerRowPolicies.Project(\n",
        before
    );
    REQUIRE(loop);
    /* The result remains live in Project's argument. When the finite loop
     * completes normally, fuelWork-32768 is zero and the original window wins. */
    char *text = outliner_live_guards_edit(loop, "            First, Count, Collapsed, event.Object\n", "            First + (fuelWork - 32768), Count, Collapsed, event.Object\n");
    free(loop);
    REQUIRE(outliner_live_guards_publish(1, text));
    REQUIRE(studio_outliner_module_valid(
        outliner_live.modules[1],
        outliner_live.error,
        sizeof outliner_live.error
    ));
    outliner_live_guards_bind();
    return true;
}

static bool outliner_live_fuel_after_source()
{
    char *text = outliner_live_guards_edit(
        outliner_live_fuel_original,
        "            ShownOpen = !Collapsed\n",
        "            fuelWork: int = length(captured.Actors)\n"
        "            while fuelWork < 32768 {\n"
        "                fuelWork = fuelWork + 1\n"
        "            }\n"
        "            ShownOpen = (fuelWork == 32768) && !Collapsed\n"
    );
    /* Publish and its Accepted branch precede this loop. The original state
     * assignment remains after Publish and has the same value if it completes. */
    REQUIRE(outliner_live_guards_publish(2, text));
    REQUIRE(studio_outliner_module_valid(
        outliner_live.modules[2],
        outliner_live.error,
        sizeof outliner_live.error
    ));
    outliner_live_guards_bind();
    return true;
}

static bool outliner_live_fuel_exhaust(int backend, int phase)
{
    const char *args[] = { outliner_live.actor_key };
    outliner_live_enqueue(0, "RevealObject", 1, args);
    ui_activation activation;
    REQUIRE(ui_mount_take(outliner_live.mounts[0], &activation));
    REQUIRE(!strcmp(activation.event, "RevealObject"));
    uint64_t before = outliner_live.invocations;
    bool carried = outliner_live_send(0, 0, &activation, false);
    /* The UI boolean reports payload carryability, including when dispatch
     * returns a budget error. The exact execution failure is checked below. */
    REQUIRE(carried && outliner_live.backend_ok);
    REQUIRE(
        outliner_live.invocations > before &&
        outliner_live.invocations - before <=
            (phase ? OUTLINER_LIVE_FUEL_SMALL_ENTRIES : 1)
    );
    execution_program *program = source_program(outliner_live.sources, "OutlinerRows");
    const execution_program_failure *failure = execution_program_error(program);
    REQUIRE(
        failure && failure->status == FLOW_ERR_STEP_BUDGET && failure->kind == BEHAVIOR_FUNCTION_BUDGET
    );
    REQUIRE(!strcmp(failure->asset_name, "OutlinerRows"));
    REQUIRE(failure->owner == ecs_entity_to_u64(outliner_live.controllers[0]));
    const char *start = strstr(outliner_live.behavior, "while fuelWork < 32768 {");
    const char *end = start ? strchr(start, '}') : NULL;
    REQUIRE(start && end);
    REQUIRE(
        failure->start >= start - outliner_live.behavior &&
        failure->end <= end + 1 - outliner_live.behavior &&
        failure->end > failure->start
    );
    REQUIRE(source_error(outliner_live.sources)[0]);
    outliner_live_fuel_failures[backend][phase] = *failure;
    printf(
        "Outliner live fuel: backend=%s phase=%s status=%d kind=%d "
        "source=[%d,%d) backend_entries=%llu shared_spent=not_observed\n",
        behavior_backend_name(outliner_live.execution_backend),
        phase ? "after-publish" : "before-project",
        failure->status,
        failure->kind,
        failure->start,
        failure->end,
        (unsigned long long)(outliner_live.invocations - before)
    );
    execution_program_clear_error(program);
    return true;
}

TEST_CASE(outliner_live_fuel_shipped_request_and_publication_errors)
{
    static const behavior_backend backends[] = { BEHAVIOR_BYTECODE, BEHAVIOR_SLJIT };
    const char *window[] = { "1", "32" };
    for (int b = 0; b < 2; b++) {
        REQUIRE(outliner_live_setup_backend(backends[b]));
        REQUIRE(outliner_live_fuel_sources());
        outliner_live_fuel_original = xstrdup(outliner_live.behavior);
        REQUIRE(outliner_live_fuel_actors());
        REQUIRE(outliner_live_event(0, 0, "ShowRows", 2, window));
        uint64_t before = outliner_live.invocations;
        REQUIRE(outliner_live_event(0, 0, "Refresh", 0, NULL));
        REQUIRE(
            outliner_live.backend_ok && outliner_live.invocations > before &&
            outliner_live.invocations - before <= OUTLINER_LIVE_FUEL_REFRESH_ENTRIES
        );
        REQUIRE(!source_error(outliner_live.sources)[0]);
        REQUIRE(!execution_program_error(source_program(outliner_live.sources, "OutlinerRows")));
        REQUIRE(outliner_live_fuel_header(4096));
        const av_value *owned_rows = outliner_live_value(0, "rows");
        REQUIRE(outliner_live_fuel_large_rows(owned_rows));
        REQUIRE(
            ui_revision(outliner_live.model) == 11 &&
            ui_element_count(outliner_live.model) == 0
        );
        printf(
            "Outliner live fuel: backend=%s phase=shipped-refresh actors=4096 "
            "viewport=32 derived_limit_at_K249=3143914 backend_entries=%llu "
            "shared_spent=not_observed\n",
            behavior_backend_name(backends[b]),
            (unsigned long long)(outliner_live.invocations - before)
        );

        studio_outliner_projection_invalidate(outliner_live.projection);
        outliner_live.s->scene = outliner_live.scenes[1];
        REQUIRE(outliner_live_fuel_before_source());
        REQUIRE(outliner_live_event(0, 0, "FoldRoot", 0, NULL));
        REQUIRE(outliner_live_collapsed(0, true));
        REQUIRE(outliner_live_fuel_exhaust(b, 0));
        REQUIRE(
            ui_revision(outliner_live.model) == 11 &&
            ui_element_count(outliner_live.model) == 0
        );
        REQUIRE(outliner_live_value(0, "rows") == owned_rows);
        REQUIRE(outliner_live_fuel_large_rows(owned_rows));
        REQUIRE(outliner_live_fuel_header(4096));

        REQUIRE(outliner_live_fuel_after_source());
        REQUIRE(outliner_live_fuel_exhaust(b, 1));
        REQUIRE(outliner_live_fuel_header(1));
        REQUIRE(outliner_live_rows(outliner_live_value(0, "rows"), "Beta"));
        REQUIRE(ui_revision(outliner_live.model) == 15);
        REQUIRE(
            ui_element_count(outliner_live.model) == 1 &&
            ui_element_at(outliner_live.model, 0)->reveal == 1
        );
        REQUIRE(
            outliner_live.preparations == 4 && outliner_live.sends == 5 &&
            outliner_live.pumps == 0
        );
        REQUIRE(outliner_live.backend_ok && outliner_live.invocations <= 8615);
        outliner_live_fuel_cleanup();
    }
    for (int phase = 0; phase < 2; phase++) {
        const execution_program_failure *a = &outliner_live_fuel_failures[0][phase];
        const execution_program_failure *b = &outliner_live_fuel_failures[1][phase];
        REQUIRE(a->status == b->status && a->kind == b->kind && a->function_id == b->function_id);
        REQUIRE(
            a->start == b->start && a->end == b->end && a->line == b->line &&
            a->column == b->column
        );
    }
    return true;
}

static int outliner_live_fuel_main(const char *content_root)
{
    outliner_live_content = content_root;
    memset(outliner_live_fuel_failures, 0, sizeof outliner_live_fuel_failures);
    pu_setup(NULL, outliner_live_fuel_cleanup);
    RUN_TEST_CASE(outliner_live_fuel_shipped_request_and_publication_errors);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
