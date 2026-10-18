static bool discard_overlay_is(const preview_world *world, const char *path, const char *text)
{
    for (int i = 0; i < preview_sources_overlay_count(world); i++) {
        const source_catalog_edit *edit = preview_sources_overlay_at(world, i);
        if (!strcmp(edit->path, path)) {
            return edit->text && !strcmp(edit->text, text);
        }
    }
    return false;
}

/* Source-only Discard owns the text draft, not a valid executable model.
 * Reuse the host's worlds and first source-edit frame; do not publish the
 * malformed disk sources or add a rendering pass. */
static bool executable_source_discard(studio *s)
{
    app_scene_authoring *scene = s->scene;
    const node *literal = NULL;
    REQUIRE(check_counter(s, 1, false, &literal));
    char error[256];
    REQUIRE(studio_code_expression_replace(s, literal, "2", error, sizeof error));
    asset_code *counter = counter_source(s);
    REQUIRE(counter && asset_code_sync_graph(counter, scene->graph_editor, true));
    REQUIRE(check_counter(s, 2, true, NULL));
    char *counter_text = xstrdup(asset_code_text(counter));
    char *counter_path = xstrdup(asset_code_path(counter));
    REQUIRE(discard_overlay_is(scene->live, counter_path, counter_text));
    int tab_count = acount(scene->tabs), code_count = acount(s->code.assets);
    int active = scene->active_tab;
    REQUIRE(active == tab_count - 1);
    char **keys = NULL;
    for (int i = 0; i < tab_count; i++) {
        apush(keys, xstrdup(scene->tabs[i].key));
    }
    const authoring_sources *applied = preview_sources_applied(scene->live);
    int source_count = authoring_sources_count(preview_sources_working(scene->live));
    const char *folders[] = { "behaviors", "libraries", "systems" };
    const char *names[] = { "DiscardBehavior", "DiscardLibrary", "DiscardSystem" };
    const char *disk[] = { "DiscardBehavior :: behavior {",
                           "DiscardLibrary :: library {",
                           "DiscardSystem :: system {" };
    const char *edited[] = { "DiscardBehavior :: behavior {\n",
                             "DiscardLibrary :: library {}",
                             "DiscardSystem :: system {\n" };
    const app_editor_document_kind kinds[] = { APP_EDITOR_DOCUMENT_BEHAVIOR,
                                               APP_EDITOR_DOCUMENT_BEHAVIOR,
                                               APP_EDITOR_DOCUMENT_SYSTEM };
    bool all_valid = true;
    int cases = 0;
    for (int kind = 0; kind < 3; kind++) {
        char directory[1024], path[1024];
        int length = snprintf(directory, sizeof directory, "%s/%s", scene->content_root, folders[kind]);
        REQUIRE(length > 0 && (size_t)length < sizeof directory);
        bool made_directory = mkdir(directory, 0700) == 0;
        REQUIRE(made_directory || errno == EEXIST);
        length = snprintf(path, sizeof path, "%s/%s.chibi", directory, names[kind]);
        REQUIRE(length > 0 && (size_t)length < sizeof path);
        struct stat info;
        REQUIRE(stat(path, &info) != 0 && errno == ENOENT);
        REQUIRE(!write_file_atomic(path, disk[kind]));
        for (int background = 0; background < 2; background++) {
            REQUIRE(!scene_tabs_open_source(scene, kinds[kind], names[kind], path));
            REQUIRE(scene->active_tab == tab_count);
            asset_code *code = asset_code_open(path, error, sizeof error);
            REQUIRE(code && !asset_code_static(code));
            asset_code_set_registry(code, &scene->registry);
            asset_code_set_source_world(code, scene->live, scene->content_root);
            apush(s->code.assets, code);
            const authoring_source *original = preview_source_find(scene->live, path, false);
            REQUIRE(
                original && !original->valid &&
                !strcmp(original->syntax->text, disk[kind])
            );
            REQUIRE(studio_code_replace(s, path, disk[kind], edited[kind], NULL));
            scene_document_tab *tab = &scene->tabs[tab_count];
            REQUIRE(tab->source_only && tab->source_state && tab->source_dirty && !tab->asset_saved);
            const authoring_source *draft = preview_source_find(scene->live, path, false);
            REQUIRE(draft && !strcmp(draft->syntax->text, edited[kind]));
            REQUIRE(draft->valid == (kind == 1));
            if (background) {
                REQUIRE(!scene_tabs_activate(scene, active));
            }
            if (kind == 0 && background) {
                const authoring_sources *working = preview_sources_working(scene->live);
                const av_value *checkpoint = scene->tabs[tab_count].source_state;
                REQUIRE(!unlink(path));
                studio_discard_tab(s, tab_count);
                bool held = acount(scene->tabs) == tab_count + 1 &&
                            acount(s->code.assets) == code_count + 1 &&
                            scene->tabs[tab_count].source_state == checkpoint &&
                            scene->tabs[tab_count].source_dirty &&
                            preview_sources_working(scene->live) == working &&
                            preview_sources_applied(scene->live) == applied &&
                            s->code.assets[code_count] == code &&
                            !strcmp(asset_code_text(code), edited[kind]);
                REQUIRE(!write_file_atomic(path, disk[kind]));
                all_valid &= held;
                if (!held) {
                    fprintf(stderr, "Source-only executable discard lost unreadable draft\n");
                }
            }
            int records = acount(s->records);
            studio_discard_tab(s, tab_count);
            const authoring_source *restored = preview_source_find(scene->live, path, false);
            bool closed = acount(scene->tabs) == tab_count &&
                          acount(s->code.assets) == code_count;
            bool reset = restored && !restored->valid &&
                         !strcmp(restored->syntax->text, disk[kind]) &&
                         discard_overlay_is(scene->live, path, disk[kind]) &&
                         !text_overlay_text(path);
            bool kept = scene->active_tab == active &&
                        preview_sources_applied(scene->live) == applied &&
                        counter_source(s) == counter && asset_code_dirty(counter) &&
                        !strcmp(asset_code_text(counter), counter_text) &&
                        discard_overlay_is(scene->live, counter_path, counter_text) &&
                        acount(s->records) == records;
            for (int i = 0; i < tab_count; i++) {
                kept &= !strcmp(scene->tabs[i].key, keys[i]);
            }
            char *saved = file_read_text(path);
            bool unchanged = saved && !strcmp(saved, disk[kind]);
            free(saved);
            if (!closed || !reset || !kept || !unchanged) {
                fprintf(
                    stderr,
                    "Source-only executable discard %s background=%d: "
                    "closed=%d reset=%d kept=%d disk=%d\n",
                    names[kind],
                    background,
                    closed,
                    reset,
                    kept,
                    unchanged
                );
            }
            all_valid &= closed && reset && kept && unchanged;
            cases++;
            /* The unfixed control must clean up before its final assertion. */
            for (int i = acount(s->code.assets) - 1; i >= 0; i--) {
                if (!strcmp(asset_code_path(s->code.assets[i]), path)) {
                    asset_code_free(s->code.assets[i]);
                    adel(s->code.assets, i);
                }
            }
            if (acount(scene->tabs) > tab_count) {
                REQUIRE(!scene_tabs_discard(scene, tab_count));
            }
            REQUIRE(scene->active_tab == active);
        }
        REQUIRE(!unlink(path));
        REQUIRE(!preview_source_edit(scene->live, path, NULL, error, sizeof error));
        refresh_asset_catalog(scene);
        asset_index_invalidate(scene);
        if (made_directory) {
            REQUIRE(!rmdir(directory));
        }
    }
    all_valid &= preview_sources_applied(scene->live) == applied &&
                 authoring_sources_count(preview_sources_working(scene->live)) ==
                     source_count;
    /* A normal source-backed graph still reloads its saved model on Discard. */
    REQUIRE(!scene->tabs[active].source_only && !scene->tabs[active].asset_saved);
    studio_discard_tab(s, active);
    bool ordinary_closed = acount(scene->tabs) == tab_count - 1;
    REQUIRE(!app_scene_authoring_open_behavior(scene, "Counter"));
    (void)studio_code_history(s, false, false);
    REQUIRE(show_state_graph(s));
    bool ordinary_reset = check_counter(s, 1, false, NULL) && unchanged_source();
    all_valid &= ordinary_closed && ordinary_reset;
    for (int i = 0; i < acount(keys); i++) {
        free(keys[i]);
    }
    afree(keys);
    free(counter_text);
    free(counter_path);
    fprintf(
        stderr,
        "Source-only executable discard cleanup complete: cases=%d passed=%d "
        "ordinary=%d/%d\n",
        cases,
        all_valid,
        ordinary_closed,
        ordinary_reset
    );
    REQUIRE(all_valid);
    return true;
}
