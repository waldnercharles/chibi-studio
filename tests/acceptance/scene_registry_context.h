/* Reuse the source-only editor through Studio's real history-query dispatch.
 * No setter, text edit, tab restore or extra host frame repairs its context. */
static bool cached_editor_registry(void)
{
    char root[] = "/tmp/chibi-studio-registry-XXXXXX", prototypes[256],
         relationships[256], prototype_path[256], relationship_path[256];
    const char *source =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"CachedProbe\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([])),\n"
        "            data.Field.Named(\"links\", data.Object([\n"
        "                data.Field.Named(\"CachedTargets\", data.List([]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    const char *relationship = "package asset\n"
                               "\n"
                               "import data \"game:assets\"\n"
                               "\n"
                               "pub Build :: () -> byte[] {\n"
                               "    return data.Encode(\n"
                               "        data.Object([\n"
                               "            data.Field.Named(\"name\", "
                               "data.Value.Text(\"CachedTargets\")),\n"
                               "            data.Field.Named(\"exclusive\", "
                               "data.Value.Bool(true))\n"
                               "        ])\n"
                               "    )\n"
                               "}\n";
    REQUIRE(mkdtemp(root));
    move_fixture fixture = move_fixture_new_with_root(root);
    snprintf(prototypes, sizeof prototypes, "%s/prototypes", root);
    snprintf(relationships, sizeof relationships, "%s/relationships", root);
    REQUIRE(!mkdir(prototypes, 0700));
    REQUIRE(!mkdir(relationships, 0700));
    snprintf(prototype_path, sizeof prototype_path, "%s/CachedProbe.chibi", prototypes);
    snprintf(relationship_path, sizeof relationship_path, "%s/CachedTargets.chibi", relationships);
    REQUIRE(!write_file_atomic(prototype_path, source));
    studio *s = xcalloc(1, sizeof *s);
    s->scene = fixture.scene;
    REQUIRE(
        !scene_tabs_open_source(fixture.scene, APP_EDITOR_DOCUMENT_PROTOTYPE, "CachedProbe", prototype_path)
    );
    REQUIRE(!studio_code_history(s, false, false));
    REQUIRE(acount(s->code.assets) == 1);
    asset_code *code = s->code.assets[0];
    scene_document_tab *tab = &fixture.scene->tabs[fixture.scene->active_tab];
    const av_value *checkpoint = tab->source_state;
    REQUIRE(asset_code_static(code) && asset_code_error(code)[0]);
    REQUIRE(checkpoint && asset_code_has_state(code, checkpoint));
    REQUIRE(!asset_code_dirty(code) && !asset_code_can_replay(code, true));
    const script_syntax *syntax = asset_code_syntax(code);
    REQUIRE(syntax && !syntax->diagnostic_count);
    REQUIRE(!studio_code_history(s, false, false));
    REQUIRE(s->code.assets[0] == code && asset_code_syntax(code) == syntax);
    const authoring_sources *working = preview_sources_working(fixture.live);
    REQUIRE(fixture.scene->registry.relationship_count == 0);

    REQUIRE(!write_file_atomic(relationship_path, relationship));
    bool refreshed = app_scene_authoring_refresh_content(fixture.scene) &&
                     fixture.scene->registry.relationship_count == 1;
    bool generation = preview_sources_working(fixture.live) == working;
    bool undo = studio_code_history(s, false, false);
    asset_code *retained = s->code.assets[0];
    bool same = retained == code && tab->source_state == checkpoint &&
                asset_code_has_state(retained, checkpoint);
    bool valid = !asset_code_error(retained)[0] &&
                 asset_code_validate(retained, fixture.graph);
    bool unchanged = !strcmp(asset_code_text(retained), source) &&
                     !asset_code_dirty(retained) && !tab->source_dirty && !undo &&
                     !asset_code_can_replay(retained, true) && !acount(s->records);
    syntax = asset_code_syntax(retained);
    undo = studio_code_history(s, false, false);
    bool stable = s->code.assets[0] == retained &&
                  asset_code_syntax(s->code.assets[0]) == syntax && !undo;

    /* The old product's expected diagnostic failure is reported only after
     * its retained editor, both worlds, tab checkpoint and files are freed. */
    for (int i = 0; i < acount(s->code.assets); i++) {
        asset_code_free(s->code.assets[i]);
    }
    afree(s->code.assets);
    afree(s->records);
    free(s);
    move_fixture_free(&fixture);
    REQUIRE(!unlink(prototype_path));
    REQUIRE(!unlink(relationship_path));
    REQUIRE(!rmdir(prototypes));
    REQUIRE(!rmdir(relationships));
    REQUIRE(!rmdir(root));
    bool cached_registry_current = refreshed && generation && same && valid &&
                                   unchanged && stable;
    if (!cached_registry_current) {
        fprintf(
            stderr,
            "Cached editor registry: refreshed=%d generation=%d same=%d "
            "valid=%d unchanged=%d stable=%d\n",
            refreshed,
            generation,
            same,
            valid,
            unchanged,
            stable
        );
    }
    REQUIRE(cached_registry_current);
    return true;
}
