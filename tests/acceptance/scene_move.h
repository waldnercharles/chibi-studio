#include "authoring/engine_preview.h"
#include "authoring/source_format.h"
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
    const char *suffix =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Large\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
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
        valid = value &&
                !prototype_read_field(&acceptance_field_reads, value, "Transform", "x", &field) &&
                field.type == PIN_F32 && field.f == (changed ? 2 : 1);
        prototype_free(value);
        av_doc_free(document);
    }
    asset_code_free(restored);
    return valid;
}

static bool static_source_dispatch_history(studio *host)
{
    char root[] = "/tmp/chibi-studio-source-undo-XXXXXX", directory[256],
         path[256], scene_path[256], error[256];
    REQUIRE(host && host->kit);
    REQUIRE(mkdtemp(root));
    const char *scene_text = "// Unrelated scene stays unchanged.\n";
    snprintf(scene_path, sizeof scene_path, "%s/Unrelated.chibi", root);
    const char *folders[] = { "relationships", "prototypes" };
    const char *names[] = { "UndoTargets", "UndoEnemy" };
    const char *before[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"UndoTargets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.Bool(true))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"UndoEnemy\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(1)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    const char *after[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"UndoTargets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.Bool(false))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"UndoEnemy\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(2)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    const char *invalid[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"UndoTargets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.UInt(1))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"UndoEnemy\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", "
        "data.Value.Text(\"bad\")),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    const app_editor_document_kind kinds[] = { APP_EDITOR_DOCUMENT_RELATIONSHIP,
                                               APP_EDITOR_DOCUMENT_PROTOTYPE };
    bool all_valid = true, clean_saves = true;
    for (int i = 0; i < 2; i++) {
        const char *unformatted[] = { before[i], after[i] };
        char *text[2];
        for (int j = 0; j < 2; j++) {
            script_syntax *syntax = script_parse(unformatted[j]);
            text[j] = authoring_source_format(syntax, error, sizeof error);
            script_syntax_free(syntax);
            REQUIRE(text[j] && strlen(text[j]) <= 128);
        }
        snprintf(directory, sizeof directory, "%s/%s", root, folders[i]);
        REQUIRE(!mkdir(directory, 0700));
        snprintf(path, sizeof path, "%s/%s.chibi", directory, names[i]);
        REQUIRE(!write_file_atomic(path, text[0]));
        REQUIRE(!write_file_atomic(scene_path, scene_text));
        move_fixture fixture = move_fixture_new_with_root(root);
        fixture.scene->scene_path = xstrdup(scene_path);
        if (i) {
            REQUIRE(!app_scene_authoring_register_prototype(fixture.scene, text[0], path));
        }
        studio *s = xcalloc(1, sizeof *s);
        s->scene = fixture.scene;
        s->kit = host->kit;
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

        /* A clean canonical source owns Save before it has any edits. The
         * background scene's sentinel also catches the relationship fallback. */
        int source_tab = fixture.scene->active_tab;
        studio_save(s);
        code = s->code.assets[0];
        char *disk = file_read_text(path), *background = file_read_text(scene_path);
        bool clean_saved = disk && !strcmp(disk, text[0]) && background &&
                           !strcmp(background, scene_text) &&
                           !strcmp(asset_code_text(code), text[0]) &&
                           !asset_code_dirty(code) &&
                           !scene_tabs_dirty(fixture.scene, &fixture.scene->tabs[source_tab]) &&
                           fixture.scene->active_tab == source_tab &&
                           fixture.scene->document.kind == kinds[i] &&
                           !fixture.scene->scene_dirty && !acount(s->records) &&
                           !studio_can_replay(s, false) &&
                           !studio_can_replay(s, true);
        free(disk);
        free(background);
        /* Keep the old-product control running through cleanup after a bad
         * clean Save, with the same source bytes as the fixed product. */
        REQUIRE(!write_file_atomic(path, text[0]));
        REQUIRE(!write_file_atomic(scene_path, scene_text));
        REQUIRE(!scene_tabs_open_source(fixture.scene, kinds[i], names[i], path));
        REQUIRE(studio_code_replace(s, path, text[0], text[1]));
        REQUIRE(asset_code_validate(code, fixture.graph));
        bool undo_enabled = studio_can_replay(s, false);
        studio_history(s, false);
        bool undone = !strcmp(asset_code_text(code), text[0]) &&
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
        bool redone = !strcmp(asset_code_text(code), text[1]) &&
                      asset_code_dirty(code);
        redone &= source_history_reopens(
            code,
            fixture.scene->tabs[fixture.scene->active_tab].source_state,
            &fixture.scene->registry,
            fixture.graph,
            i == 0,
            true
        );
        int records = acount(s->records);
        studio_save(s);
        code = s->code.assets[0];
        disk = file_read_text(path);
        bool dirty_saved = disk && !strcmp(disk, text[1]) &&
                           !asset_code_dirty(code) &&
                           !scene_tabs_dirty(fixture.scene, &fixture.scene->tabs[source_tab]) &&
                           studio_can_replay(s, false) &&
                           !studio_can_replay(s, true);
        free(disk);
        bool notice = i ? acount(s->records) == records
                        : acount(s->records) == records + 1 &&
                              s->records[records].kind == KIT_TOAST_ERROR &&
                              strstr(s->records[records].text, "Saved, but not applied") &&
                              strstr(s->records[records].text, "Reopen the project");
        studio_history(s, false);
        bool saved_history = !strcmp(asset_code_text(code), text[0]) &&
                             asset_code_dirty(code) && studio_can_replay(s, true);
        bool saved_all = studio_save_all(s);
        code = s->code.assets[0];
        disk = file_read_text(path);
        background = file_read_text(scene_path);
        saved_all &= disk && !strcmp(disk, text[0]) && background &&
                     !strcmp(background, scene_text) &&
                     !asset_code_dirty(code) && !studio_active_dirty(s) &&
                     fixture.scene->active_tab == source_tab &&
                     studio_can_replay(s, true);
        free(disk);
        free(background);
        records = acount(s->records);
        bool clean_all = studio_save_all(s) && acount(s->records) == records;
        REQUIRE(studio_code_replace(s, path, text[0], invalid[i]));
        studio_save(s);
        code = s->code.assets[0];
        disk = file_read_text(path);
        bool invalid_dirty = disk && !strcmp(disk, text[0]) &&
                             !strcmp(asset_code_text(code), invalid[i]) &&
                             asset_code_error(code)[0] &&
                             asset_code_dirty(code) && studio_active_dirty(s) &&
                             acount(s->records) == records + 1 &&
                             s->records[records].kind == KIT_TOAST_ERROR &&
                             strstr(s->records[records].text, "Could not save");
        free(disk);
        studio_discard_tab(s, source_tab);
        disk = file_read_text(path);
        bool discarded = disk && !strcmp(disk, text[0]) &&
                         !text_overlay_text(path) && !acount(s->code.assets) &&
                         acount(fixture.scene->tabs) == 1;
        free(disk);

        /* A malformed file opens clean and can be closed without repairing it.
         * Saving it must report its diagnostic, never serialize a stale model. */
        REQUIRE(!write_file_atomic(path, invalid[i]));
        /* Closing a prototype tab retains its workspace-owned source. Reload
         * the external edit before checking a clean malformed document. */
        if (i) {
            REQUIRE(studio_code_reload_path(s, path) == APP_DISK_RELOADED);
        }
        REQUIRE(!scene_tabs_open_source(fixture.scene, kinds[i], names[i], path));
        records = acount(s->records);
        studio_save(s);
        code = acount(s->code.assets) ? s->code.assets[0] : NULL;
        disk = file_read_text(path);
        background = file_read_text(scene_path);
        bool invalid_clean = code && disk && !strcmp(disk, invalid[i]) &&
                             background && !strcmp(background, scene_text) &&
                             asset_code_error(code)[0] &&
                             !asset_code_dirty(code) && !studio_active_dirty(s) &&
                             acount(s->records) == records + 1 &&
                             s->records[records].kind == KIT_TOAST_ERROR &&
                             strstr(s->records[records].text, "Could not save");
        free(disk);
        free(background);
        studio_discard_tab(s, fixture.scene->active_tab);
        bool closed = !acount(s->code.assets) && acount(fixture.scene->tabs) == 1 &&
                      !text_overlay_text(path);
        for (int j = 0; j < acount(s->code.assets); j++) {
            asset_code_free(s->code.assets[j]);
        }
        afree(s->code.assets);
        afree(s->records);
        free(s);
        move_fixture_free(&fixture);
        free(text[0]);
        free(text[1]);
        REQUIRE(!unlink(path));
        REQUIRE(!rmdir(directory));
        bool valid = clean_saved && undo_enabled && undone && redo_enabled &&
                     redone && dirty_saved && notice && saved_history &&
                     saved_all && clean_all && invalid_dirty && discarded &&
                     invalid_clean && closed;
        if (!valid) {
            fprintf(
                stderr,
                "Static source save %s: clean=%d undo=%d/%d redo=%d/%d "
                "dirty=%d notice=%d history=%d all=%d clean_all=%d "
                "invalid=%d/%d discard=%d close=%d\n",
                names[i],
                clean_saved,
                undo_enabled,
                undone,
                redo_enabled,
                redone,
                dirty_saved,
                notice,
                saved_history,
                saved_all,
                clean_all,
                invalid_dirty,
                invalid_clean,
                discarded,
                closed
            );
        }
        clean_saves &= clean_saved;
        all_valid &= valid;
    }
    REQUIRE(!unlink(scene_path));
    REQUIRE(!rmdir(root));
    REQUIRE(clean_saves);
    REQUIRE(all_valid);
    return true;
}

static bool relationship_schema_kept(const move_fixture *fixture, const ecs_relationship_desc *schema)
{
    if (fixture->scene->registry.relationship_count != 1 ||
        fixture->scene->registry.relationships[0] != schema) {
        return false;
    }
    ecs_world *worlds[] = { fixture->world,
                            prototype_preview_world(fixture->scene->prototype_preview) };
    for (int i = 0; i < 2; i++) {
        const ecs_relationship_desc
            *live = ecs_world_relationship(worlds[i], schema->id);
        if (!live || !(live->traits & ECS_REL_EXCLUSIVE) ||
            strcmp(live->name, "AcceptanceTargets")) {
            return false;
        }
    }
    return true;
}

static bool static_relationship_refresh(studio *host)
{
    char root[] = "/tmp/chibi-studio-relation-XXXXXX", directory[256],
         prototypes[256], path[256], probe[256], error[256];
    const char *before = "package asset\n"
                         "\n"
                         "import data \"game:assets\"\n"
                         "\n"
                         "pub Build :: () -> byte[] {\n"
                         "    return data.Encode(\n"
                         "        data.Object([\n"
                         "            data.Field.Named(\"name\", "
                         "data.Value.Text(\"AcceptanceTargets\")),\n"
                         "            data.Field.Named(\"exclusive\", "
                         "data.Value.Bool(true))\n"
                         "        ])\n"
                         "    )\n"
                         "}\n";
    const char *after = "package asset\n"
                        "\n"
                        "import data \"game:assets\"\n"
                        "\n"
                        "pub Build :: () -> byte[] {\n"
                        "    return data.Encode(\n"
                        "        data.Object([\n"
                        "            data.Field.Named(\"name\", "
                        "data.Value.Text(\"AcceptanceTargets\")),\n"
                        "            data.Field.Named(\"exclusive\", "
                        "data.Value.Bool(false))\n"
                        "        ])\n"
                        "    )\n"
                        "}\n";
    const char *prototype_source =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"RegistryProbe\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([])),\n"
        "            data.Field.Named(\"links\", data.Object([\n"
        "                data.Field.Named(\"AcceptanceTargets\", "
        "data.List([]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    REQUIRE(host && host->kit);
    REQUIRE(mkdtemp(root));
    move_fixture fixture = move_fixture_new_with_root(root);
    snprintf(prototypes, sizeof prototypes, "%s/prototypes", root);
    REQUIRE(!mkdir(prototypes, 0700));
    snprintf(probe, sizeof probe, "%s/RegistryProbe.chibi", prototypes);
    REQUIRE(!write_file_atomic(probe, prototype_source));
    asset_code *code = asset_code_open(probe, error, sizeof error);
    REQUIRE(code && asset_code_static(code));
    asset_code_set_registry(code, &fixture.scene->registry);
    REQUIRE(asset_code_error(code)[0]);
    snprintf(directory, sizeof directory, "%s/relationships", root);
    REQUIRE(!mkdir(directory, 0700));
    snprintf(path, sizeof path, "%s/AcceptanceTargets.chibi", directory);
    REQUIRE(!write_file_atomic(path, before));
    app_scene_authoring_refresh_content(fixture.scene);
    REQUIRE(fixture.scene->registry.relationship_count == 1);
    asset_code_set_registry(code, &fixture.scene->registry);
    REQUIRE(!asset_code_error(code)[0]);
    REQUIRE(asset_code_validate(code, fixture.graph));
    const ecs_relationship_desc *schema = fixture.scene->registry.relationships[0];
    REQUIRE(relationship_schema_kept(&fixture, schema));
    asset_code_free(code);

    /* Use the real save/report path, borrowing only the existing host's kit.
     * The private studio owns its source buffer, tab state and Output records. */
    studio *s = xcalloc(1, sizeof *s);
    s->scene = fixture.scene;
    s->kit = host->kit;
    s->key_area = -1;
    s->focus_area = STUDIO_AREA_TOP;
    s->graph.area = -1;
    s->prefs.layouts[0].editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_CODE;
    s->prefs.code_apply_live = true;
    REQUIRE(
        !scene_tabs_open_source(fixture.scene, APP_EDITOR_DOCUMENT_RELATIONSHIP, "AcceptanceTargets", path)
    );
    code = asset_code_open(path, error, sizeof error);
    REQUIRE(code && asset_code_static(code));
    asset_code_set_registry(code, &fixture.scene->registry);
    apush(s->code.assets, code);
    REQUIRE(asset_code_replace(code, before, after));
    REQUIRE(asset_code_validate(code, fixture.graph));
    studio_save(s);
    /* asset_editor may restore a tab checkpoint, so retrieve its owned buffer. */
    code = s->code.assets[0];
    char *disk = file_read_text(path);
    bool saved = disk && !strcmp(disk, asset_code_text(code)) &&
                 strstr(disk, "false") && strlen(disk) <= 128 &&
                 !asset_code_dirty(code) &&
                 !fixture.scene->tabs[fixture.scene->active_tab].source_dirty;
    free(disk);
    bool saved_notice = acount(s->records) == 1 &&
                        s->records[0].kind == KIT_TOAST_ERROR &&
                        strstr(s->records[0].text, "Saved, but not applied") &&
                        strstr(s->records[0].text, "AcceptanceTargets") &&
                        strstr(s->records[0].text, "Reopen the project");
    bool kept = relationship_schema_kept(&fixture, schema);
    asset_code_free(code);
    afree(s->code.assets);
    s->code.assets = NULL;
    REQUIRE(!text_overlay_text(path) && !text_overlay_text(probe));

    /* Start watching after the save. File sizes change, so no sleep or mtime
     * assumption is needed. The second edit is a successful prototype load
     * that overwrites scene->message after relationship refresh fails. */
    s->time = 0;
    studio_disk_frame(s);
    bool baseline = acount(s->records) == (saved_notice ? 1 : 0) &&
                    acount(fixture.scene->prototypes) == 0;
    REQUIRE(!write_file_atomic(
        path,
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceTargets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.UInt(1))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    REQUIRE(!write_file_atomic(
        probe,
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"RegistryProbe\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([])),\n"
        "            data.Field.Named(\"links\", data.Object([\n"
        "                data.Field.Named(\"AcceptanceTargets\", "
        "data.List([]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    s->time = 1;
    studio_disk_frame(s);
    bool invalid_notice = acount(s->records) == 2 &&
                          s->records[1].kind == KIT_TOAST_ERROR &&
                          strstr(s->records[1].text, "Invalid relationship property or value type");
    bool overwritten = acount(fixture.scene->prototypes) == 1 &&
                       !strcmp(app_scene_authoring_message(fixture.scene), "Added RegistryProbe.chibi.");
    kept &= relationship_schema_kept(&fixture, schema);

    REQUIRE(!unlink(path));
    s->time = 2;
    studio_disk_frame(s);
    bool removed_notice = acount(s->records) == 3 &&
                          s->records[2].kind == KIT_TOAST_ERROR &&
                          strstr(s->records[2].text, "AcceptanceTargets") &&
                          strstr(s->records[2].text, "Reopen the project");
    kept &= relationship_schema_kept(&fixture, schema);
    bool reload_failed = app_scene_authoring_reload_path(fixture.scene, path) ==
                         APP_DISK_FAILED;
    kept &= relationship_schema_kept(&fixture, schema);
    int records = acount(s->records);
    s->time = 3;
    studio_disk_frame(s);
    bool quiet = acount(s->records) == records;

    /* Expected failures on the old product are asserted only after cleanup.
     * Error notices copied into the borrowed kit remain host-owned and are
     * released at its ordinary shutdown; no extra frame or kit is created. */
    studio_disk_free(s);
    afree(s->records);
    free(s);
    move_fixture_free(&fixture);
    REQUIRE(!unlink(probe));
    REQUIRE(!rmdir(prototypes));
    REQUIRE(!rmdir(directory));
    REQUIRE(!rmdir(root));
    bool ok = saved && saved_notice && baseline && invalid_notice &&
              overwritten && removed_notice && reload_failed && kept && quiet;
    if (!ok) {
        fprintf(
            stderr,
            "Relationship notices: saved=%d notice=%d baseline=%d invalid=%d "
            "overwritten=%d removed=%d reload=%d kept=%d quiet=%d\n",
            saved,
            saved_notice,
            baseline,
            invalid_notice,
            overwritten,
            removed_notice,
            reload_failed,
            kept,
            quiet
        );
    }
    REQUIRE(ok);
    return true;
}

/* Match the actual inspector subject, not only the active tab's label. */
static bool source_prototype_subject(studio *s, const char *name)
{
    app_scene_authoring *scene = s->scene;
    properties_subject subject;
    properties_subject_of(s, &subject);
    app_editor_document_context document = app_scene_authoring_document(scene);
    return document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE &&
           !strcmp(document.id, name) && scene->tabs[scene->active_tab].source_only &&
           subject.asset == find_prototype(scene, name) && !subject.actor;
}

static bool static_prototype_refresh(void)
{
    char root[] = "/tmp/chibi-studio-prototype-XXXXXX", directory[256],
         paths[4][256], relationships[256], relationship[256];
    const char *relation = "package asset\n"
                           "\n"
                           "import data \"game:assets\"\n"
                           "\n"
                           "pub Build :: () -> byte[] {\n"
                           "    return data.Encode(\n"
                           "        data.Object([\n"
                           "            data.Field.Named(\"name\", "
                           "data.Value.Text(\"AcceptanceTargets\")),\n"
                           "            data.Field.Named(\"exclusive\", "
                           "data.Value.Bool(true))\n"
                           "        ])\n"
                           "    )\n"
                           "}\n";
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
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceBase\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(1)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceMiddle\")),\n"
        "            data.Field.Named(\"includes\", data.List([\n"
        "                data.Value.Text(\"AcceptanceBase\")\n"
        "            ])),\n"
        "            data.Field.Named(\"components\", data.Object([]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceLeaf\")),\n"
        "            data.Field.Named(\"includes\", data.List([\n"
        "                data.Value.Text(\"AcceptanceMiddle\")\n"
        "            ])),\n"
        "            data.Field.Named(\"components\", data.Object([]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceNest\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([])),\n"
        "            data.Field.Named(\"children\", data.List([\n"
        "                data.Object([\n"
        "                    data.Field.Named(\"name\", "
        "data.Value.Text(\"child\")),\n"
        "                    data.Field.Named(\"prototype\", "
        "data.Value.Text(\"AcceptanceBase\"))\n"
        "                ])\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    for (int i = 0; i < 4; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/%s.chibi", directory, names[i]);
        REQUIRE(!write_file_atomic(paths[i], sources[i]));
    }
    snprintf(relationships, sizeof relationships, "%s/relationships", root);
    REQUIRE(!mkdir(relationships, 0700));
    snprintf(relationship, sizeof relationship, "%s/AcceptanceTargets.chibi", relationships);
    REQUIRE(!write_file_atomic(relationship, relation));
    REQUIRE(app_scene_authoring_refresh_content(scene));
    REQUIRE(scene->registry.relationship_count == 1);
    const ecs_relationship_desc *schema = scene->registry.relationships[0];
    REQUIRE(relationship_schema_kept(&fixture, schema));
    for (int i = 0; i < 4; i++) {
        REQUIRE(!app_scene_authoring_register_prototype(scene, sources[i], paths[i]));
    }
    int passive_tabs = acount(scene->tabs), passive_active = scene->active_tab;
    pin_value value;
    REQUIRE(!app_scene_authoring_read_prototype_field(
        &acceptance_field_reads,
        scene,
        names[2],
        "Transform",
        "x",
        &value
    ));
    REQUIRE(value.type == PIN_F32 && value.f == 1);
    REQUIRE(!write_file_atomic(
        paths[0],
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceBase\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(2)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    REQUIRE(app_scene_authoring_reload_path(scene, paths[0]) == APP_DISK_RELOADED);
    for (int i = 0; i < 3; i++) {
        REQUIRE(!app_scene_authoring_read_prototype_field(
            &acceptance_field_reads,
            scene,
            names[i],
            "Transform",
            "x",
            &value
        ));
        REQUIRE(value.type == PIN_F32 && value.f == 2);
    }
    scene_prototype_asset *nest = find_prototype(scene, names[3]);
    REQUIRE(nest && acount(nest->prototype->child_prototypes) == 1);
    REQUIRE(!prototype_read_field(
        &acceptance_field_reads,
        nest->prototype->child_prototypes[0],
        "Transform",
        "x",
        &value
    ));
    REQUIRE(value.type == PIN_F32 && value.f == 2);
    REQUIRE(acount(scene->tabs) == passive_tabs && scene->active_tab == passive_active);

    /* A relationship error must not leave include sources stale when the
     * same watcher batch reloads a prototype and its transitive users. */
    app_disk_change *changes = app_scene_authoring_sync_disk(scene, 0);
    bool baseline = !acount(changes);
    app_disk_changes_free(changes);
    REQUIRE(!write_file_atomic(
        relationship,
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceTargets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.UInt(1))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    REQUIRE(!write_file_atomic(
        paths[0],
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceBase\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(20)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    changes = app_scene_authoring_sync_disk(scene, 1);
    float refreshed[4] = { 0 };
    bool propagated = true, reloaded = false;
    for (int i = 0; i < 3; i++) {
        bool read = !app_scene_authoring_read_prototype_field(
                        &acceptance_field_reads,
                        scene,
                        names[i],
                        "Transform",
                        "x",
                        &value
                    ) &&
                    value.type == PIN_F32;
        if (read) {
            refreshed[i] = value.f;
        }
        propagated &= read && refreshed[i] == 20;
    }
    nest = find_prototype(scene, names[3]);
    bool child = nest && acount(nest->prototype->child_prototypes) == 1 &&
                 !prototype_read_field(
                     &acceptance_field_reads,
                     nest->prototype->child_prototypes[0],
                     "Transform",
                     "x",
                     &value
                 ) &&
                 value.type == PIN_F32;
    if (child) {
        refreshed[3] = value.f;
    }
    propagated &= child && refreshed[3] == 20;
    bool kept = relationship_schema_kept(&fixture, schema);

    /* If both refreshes fail, the first relationship diagnostic wins. If
     * only prototype refresh fails, keep its error and failure result. */
    REQUIRE(!write_file_atomic(
        paths[3],
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceBase\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    bool combined_failed = !app_scene_authoring_refresh_content(scene) &&
                           strstr(app_scene_authoring_message(scene), "Invalid relationship property or value type");
    REQUIRE(!write_file_atomic(relationship, relation));
    bool prototype_failed = !app_scene_authoring_refresh_content(scene) &&
                            strstr(app_scene_authoring_message(scene), "Duplicate prototype name: AcceptanceBase");
    REQUIRE(!write_file_atomic(paths[3], sources[3]));
    bool restored = app_scene_authoring_refresh_content(scene);
    kept &= relationship_schema_kept(&fixture, schema);

    /* The owned watcher diagnostic survives every later status overwrite. */
    bool notice = false;
    for (int i = 0; i < acount(changes); i++) {
        if (changes[i].result == APP_DISK_FAILED && !strcmp(changes[i].path, root)) {
            notice |= strstr(changes[i].message, "Invalid relationship property or value type") !=
                      NULL;
        }
        if (changes[i].result == APP_DISK_RELOADED &&
            !strcmp(changes[i].path, paths[0])) {
            reloaded = true;
        }
    }
    app_disk_changes_free(changes);

    REQUIRE(acount(scene->tabs) == passive_tabs && scene->active_tab == passive_active);

    /* Dirty dependents retain their working values. Clean transitive users
     * still reload from the saved dependency graph. */
    scene_prototype_asset *middle = find_prototype(scene, names[1]);
    pin_value edited = { .type = PIN_F32, .f = 9 };
    REQUIRE(!write_prototype_field(scene, middle, "Transform", "x", edited));
    REQUIRE(middle->dirty);
    REQUIRE(acount(scene->tabs) == passive_tabs + 1 && scene->active_tab == passive_active);
    REQUIRE(!write_file_atomic(
        paths[0],
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"AcceptanceBase\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(3)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    REQUIRE(app_scene_authoring_reload_path(scene, paths[0]) == APP_DISK_UNSAVED);
    REQUIRE(!app_scene_authoring_read_prototype_field(
        &acceptance_field_reads,
        scene,
        names[1],
        "Transform",
        "x",
        &value
    ));
    REQUIRE(value.f == 9 && middle->dirty);
    REQUIRE(!app_scene_authoring_read_prototype_field(
        &acceptance_field_reads,
        scene,
        names[2],
        "Transform",
        "x",
        &value
    ));
    REQUIRE(value.f == 3);
    REQUIRE(strstr(app_scene_authoring_message(scene), "unsaved edits"));

    /* Close only the intentionally edited view; its workspace draft survives. */
    scene_tabs_close(scene, APP_EDITOR_DOCUMENT_PROTOTYPE, names[1]);
    /* Reuse these registered sources through the real Assets open route. */
    studio *s = xcalloc(1, sizeof *s);
    s->scene = scene;
    int tabs_before = acount(scene->tabs), undo_before = acount(middle->undo);
    REQUIRE(!app_scene_authoring_refresh_asset_index(scene));
    int base = reference_index_find_asset(scene->asset_index, REFERENCE_ASSET_PROTOTYPE, paths[0], NULL);
    int leaf = reference_index_find_asset(scene->asset_index, REFERENCE_ASSET_PROTOTYPE, paths[2], NULL);
    REQUIRE(base >= 0 && leaf >= 0);
    scene->selected_prototype = -1;
    REQUIRE(!app_scene_authoring_open_asset(scene, base));
    bool initial = source_prototype_subject(s, names[0]);
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[1]));
    REQUIRE(!app_scene_authoring_open_asset(scene, leaf));
    bool opened = source_prototype_subject(s, names[2]);
    int leaf_tab = scene->active_tab;
    REQUIRE(!app_scene_authoring_open_prototype(scene, names[1]));
    REQUIRE(!scene_tabs_activate(scene, leaf_tab));
    bool activated = source_prototype_subject(s, names[2]);
    REQUIRE(!app_scene_authoring_open_asset(scene, leaf));
    REQUIRE(!scene_tabs_activate(scene, leaf_tab));
    bool repeated = scene->active_tab == leaf_tab &&
                    acount(scene->tabs) == tabs_before + 3 &&
                    source_prototype_subject(s, names[2]);

    /* Invalid source remains openable without a registered prototype. It
     * must not inherit another asset's inspector or replace its diagnostic. */
    char invalid_path[256], message[sizeof scene->message],
        prototype_message[sizeof scene->prototype_message];
    snprintf(invalid_path, sizeof invalid_path, "%s/Unloaded.chibi", directory);
    REQUIRE(!write_file_atomic(
        invalid_path,
        "package asset\n"
        "pub Build :: () -> byte[] {"
    ));
    REQUIRE(!app_scene_authoring_refresh_content(scene));
    REQUIRE(scene->message[0] && !find_prototype(scene, "Unloaded"));
    REQUIRE(!app_scene_authoring_refresh_asset_index(scene));
    int missing = reference_index_find_asset(scene->asset_index, REFERENCE_ASSET_PROTOTYPE, invalid_path, NULL);
    REQUIRE(missing >= 0);
    snprintf(message, sizeof message, "%s", scene->message);
    snprintf(prototype_message, sizeof prototype_message, "%s", scene->prototype_message);
    REQUIRE(!app_scene_authoring_open_asset(scene, missing));
    bool invalid = source_prototype_subject(s, "Unloaded") &&
                   scene->selected_prototype == -1;
    int invalid_tab = scene->active_tab;
    REQUIRE(!scene_tabs_activate(scene, leaf_tab));
    REQUIRE(!scene_tabs_activate(scene, invalid_tab));
    invalid &= source_prototype_subject(s, "Unloaded") &&
               scene->selected_prototype == -1;
    bool diagnostics = !strcmp(scene->message, message) &&
                       !strcmp(scene->prototype_message, prototype_message);
    bool preserved = acount(scene->tabs) == tabs_before + 4 &&
                     !scene->tabs[leaf_tab].source_dirty &&
                     !scene->tabs[invalid_tab].source_dirty && middle->dirty &&
                     acount(middle->undo) == undo_before &&
                     !scene_history_can_undo(scene->history) &&
                     !scene_history_can_redo(scene->history) &&
                     relationship_schema_kept(&fixture, schema);
    /* A file without its folder's asset extension, such as a sidecar an
     * older Studio left, is not an asset. */
    char stray[256];
    snprintf(stray, sizeof stray, "%s/Unloaded.chibi.meta", directory);
    REQUIRE(!write_file_atomic(stray, "{}"));
    REQUIRE(!app_scene_authoring_refresh_content(scene));
    REQUIRE(!app_scene_authoring_refresh_asset_index(scene));
    REQUIRE(
        reference_index_find_asset(scene->asset_index, REFERENCE_ASSET_PROTOTYPE, stray, NULL) < 0
    );
    REQUIRE(!unlink(stray));
    free(s);
    REQUIRE(!unlink(invalid_path));
    move_fixture_free(&fixture);
    for (int i = 0; i < 4; i++) {
        REQUIRE(!unlink(paths[i]));
    }
    REQUIRE(!rmdir(directory));
    REQUIRE(!unlink(relationship));
    REQUIRE(!rmdir(relationships));
    REQUIRE(!rmdir(root));
    bool ok = baseline && propagated && reloaded && kept && combined_failed &&
              prototype_failed && restored && notice;
    if (!ok) {
        fprintf(
            stderr,
            "Prototype refresh after relationship failure: base=%g middle=%g "
            "leaf=%g child=%g "
            "baseline=%d propagated=%d reloaded=%d kept=%d combined=%d "
            "prototype=%d restored=%d notice=%d\n",
            refreshed[0],
            refreshed[1],
            refreshed[2],
            refreshed[3],
            baseline,
            propagated,
            reloaded,
            kept,
            combined_failed,
            prototype_failed,
            restored,
            notice
        );
    }
    REQUIRE(ok);
    bool source_selection = initial && opened && activated && repeated &&
                            invalid && diagnostics && preserved;
    if (!source_selection) {
        fprintf(
            stderr,
            "Prototype source selection: initial=%d opened=%d activated=%d "
            "repeated=%d invalid=%d diagnostics=%d preserved=%d\n",
            initial,
            opened,
            activated,
            repeated,
            invalid,
            diagnostics,
            preserved
        );
    }
    REQUIRE(source_selection);
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
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Mover\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"Transform\", data.Object([\n"
            "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
            "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
            "                ])),\n"
            "                data.Field.Named(\"Orientation\", data.Object([\n"
            "                    data.Field.Named(\"radians\", "
            "data.Value.UInt(0))\n"
            "                ])),\n"
            "                data.Field.Named(\"Joint\", data.Object([])),\n"
            "                data.Field.Named(\"Appearance\", data.Object([\n"
            "                    data.Field.Named(\"shape\", "
            "data.Value.Text(\"Box\")),\n"
            "                    data.Field.Named(\"width\", "
            "data.Value.Real(4.0)),\n"
            "                    data.Field.Named(\"height\", "
            "data.Value.Real(4.0))\n"
            "                ]))\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    REQUIRE(
        scene_authoring_load_text(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Scene\")),\n"
            "            data.Field.Named(\"entities\", data.List([\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Left\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\")),\n"
            "                    data.Field.Named(\"components\", "
            "data.Object([\n"
            "                        data.Field.Named(\"Transform\", "
            "data.Object([\n"
            "                            data.Field.Named(\"x\", "
            "data.Value.Real(100.0)),\n"
            "                            data.Field.Named(\"y\", "
            "data.Value.Real(0.0))\n"
            "                        ])),\n"
            "                        data.Field.Named(\"Orientation\", "
            "data.Object([\n"
            "                            data.Field.Named(\"radians\", "
            "data.Value.Real(1.5707964))\n"
            "                        ])),\n"
            "                        data.Field.Named(\"Joint\", "
            "data.Object([\n"
            "                            data.Field.Named(\"other\", "
            "data.Value.Text(\"Left.A\"))\n"
            "                        ]))\n"
            "                    ])),\n"
            "                    data.Field.Named(\"children\", data.List([\n"
            "                        data.Object([\n"
            "                            data.Field.Named(\"name\", "
            "data.Value.Text(\"A\")),\n"
            "                            data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\")),\n"
            "                            data.Field.Named(\"components\", "
            "data.Object([\n"
            "                                data.Field.Named(\"Transform\", "
            "data.Object([\n"
            "                                    data.Field.Named(\"x\", "
            "data.Value.Real(10.0)),\n"
            "                                    data.Field.Named(\"y\", "
            "data.Value.Real(0.0))\n"
            "                                ]))\n"
            "                            ]))\n"
            "                        ]),\n"
            "                        data.Object([\n"
            "                            data.Field.Named(\"name\", "
            "data.Value.Text(\"B\")),\n"
            "                            data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\")),\n"
            "                            data.Field.Named(\"components\", "
            "data.Object([\n"
            "                                data.Field.Named(\"Transform\", "
            "data.Object([\n"
            "                                    data.Field.Named(\"x\", "
            "data.Value.Real(20.0)),\n"
            "                                    data.Field.Named(\"y\", "
            "data.Value.Real(0.0))\n"
            "                                ]))\n"
            "                            ]))\n"
            "                        ]),\n"
            "                        data.Object([\n"
            "                            data.Field.Named(\"name\", "
            "data.Value.Text(\"Clash\")),\n"
            "                            data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\"))\n"
            "                        ])\n"
            "                    ]))\n"
            "                ]),\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Right\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\")),\n"
            "                    data.Field.Named(\"components\", "
            "data.Object([\n"
            "                        data.Field.Named(\"Transform\", "
            "data.Object([\n"
            "                            data.Field.Named(\"x\", "
            "data.Value.Real(200.0)),\n"
            "                            data.Field.Named(\"y\", "
            "data.Value.Real(0.0))\n"
            "                        ])),\n"
            "                        data.Field.Named(\"Orientation\", "
            "data.Object([\n"
            "                            data.Field.Named(\"radians\", "
            "data.Value.Real(-1.5707964))\n"
            "                        ])),\n"
            "                        data.Field.Named(\"Joint\", "
            "data.Object([\n"
            "                            data.Field.Named(\"other\", "
            "data.Value.Text(\"Right.A\"))\n"
            "                        ]))\n"
            "                    ])),\n"
            "                    data.Field.Named(\"children\", data.List([\n"
            "                        data.Object([\n"
            "                            data.Field.Named(\"name\", "
            "data.Value.Text(\"Clash\")),\n"
            "                            data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\"))\n"
            "                        ])\n"
            "                    ]))\n"
            "                ]),\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"C\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Mover\"))\n"
            "                ])\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
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
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Spatial\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"Transform3D\", data.Object([\n"
            "                    data.Field.Named(\"rotation\", data.Object([\n"
            "                        data.Field.Named(\"x\", "
            "data.Value.Real(0.0)),\n"
            "                        data.Field.Named(\"y\", "
            "data.Value.Real(0.0)),\n"
            "                        data.Field.Named(\"z\", "
            "data.Value.Real(0.0)),\n"
            "                        data.Field.Named(\"w\", "
            "data.Value.Real(1.0))\n"
            "                    ])),\n"
            "                    data.Field.Named(\"scale\", data.Object([\n"
            "                        data.Field.Named(\"x\", "
            "data.Value.Real(1.0)),\n"
            "                        data.Field.Named(\"y\", "
            "data.Value.Real(1.0)),\n"
            "                        data.Field.Named(\"z\", "
            "data.Value.Real(1.0))\n"
            "                    ]))\n"
            "                ]))\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    REQUIRE(
        scene_authoring_load_text(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Scene\")),\n"
            "            data.Field.Named(\"entities\", data.List([\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Parent\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Spatial\")),\n"
            "                    data.Field.Named(\"components\", "
            "data.Object([\n"
            "                        data.Field.Named(\"Transform3D\", "
            "data.Object([\n"
            "                            data.Field.Named(\"position\", "
            "data.Object([\n"
            "                                data.Field.Named(\"x\", "
            "data.Value.Real(10.0)),\n"
            "                                data.Field.Named(\"y\", "
            "data.Value.Real(20.0)),\n"
            "                                data.Field.Named(\"z\", "
            "data.Value.Real(30.0))\n"
            "                            ])),\n"
            "                            data.Field.Named(\"rotation\", "
            "data.Object([\n"
            "                                data.Field.Named(\"x\", "
            "data.Value.Real(0.0)),\n"
            "                                data.Field.Named(\"y\", "
            "data.Value.Real(0.0)),\n"
            "                                data.Field.Named(\"z\", "
            "data.Value.Real(0.70710677)),\n"
            "                                data.Field.Named(\"w\", "
            "data.Value.Real(0.70710677))\n"
            "                            ])),\n"
            "                            data.Field.Named(\"scale\", "
            "data.Object([\n"
            "                                data.Field.Named(\"x\", "
            "data.Value.Real(2.0)),\n"
            "                                data.Field.Named(\"y\", "
            "data.Value.Real(2.0)),\n"
            "                                data.Field.Named(\"z\", "
            "data.Value.Real(2.0))\n"
            "                            ]))\n"
            "                        ]))\n"
            "                    ]))\n"
            "                ]),\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Object\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Spatial\")),\n"
            "                    data.Field.Named(\"components\", "
            "data.Object([\n"
            "                        data.Field.Named(\"Transform3D\", "
            "data.Object([\n"
            "                            data.Field.Named(\"position\", "
            "data.Object([\n"
            "                                data.Field.Named(\"x\", "
            "data.Value.Real(4.0)),\n"
            "                                data.Field.Named(\"y\", "
            "data.Value.Real(5.0)),\n"
            "                                data.Field.Named(\"z\", "
            "data.Value.Real(6.0))\n"
            "                            ])),\n"
            "                            data.Field.Named(\"rotation\", "
            "data.Object([\n"
            "                                data.Field.Named(\"x\", "
            "data.Value.Real(0.18257418)),\n"
            "                                data.Field.Named(\"y\", "
            "data.Value.Real(0.36514837)),\n"
            "                                data.Field.Named(\"z\", "
            "data.Value.Real(0.5477226)),\n"
            "                                data.Field.Named(\"w\", "
            "data.Value.Real(0.73029673))\n"
            "                            ])),\n"
            "                            data.Field.Named(\"scale\", "
            "data.Object([\n"
            "                                data.Field.Named(\"x\", "
            "data.Value.Real(1.0)),\n"
            "                                data.Field.Named(\"y\", "
            "data.Value.Real(2.0)),\n"
            "                                data.Field.Named(\"z\", "
            "data.Value.Real(3.0))\n"
            "                            ]))\n"
            "                        ]))\n"
            "                    ]))\n"
            "                ])\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
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
