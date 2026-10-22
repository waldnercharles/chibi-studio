#include "editor/documents/asset_code.h"

#include "authoring/source_intelligence.h"
#include "editor/workspace/editor.h"
#include "support/array.h"
#include "support/file_io.h"
#include "support/text_overlay.h"
#include "third_party/pico_headers/pico_unit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[128], behavior_path[192], library_path[192], error[256];
static ecs_world *world;
static game_preview_world *preview;
static asset_code *code, *restored;
static editor *view;
static av_value *checkpoint;
static char *owned;
static authoring_sources *previous;

static void cleanup()
{
    authoring_sources_free(previous);
    previous = NULL;
    free(owned);
    owned = NULL;
    av_value_free(checkpoint);
    checkpoint = NULL;
    asset_code_free(restored);
    asset_code_free(code);
    restored = code = NULL;
    editor_free(view);
    view = NULL;
    game_preview_world_free(preview);
    preview = NULL;
    ecs_world_free(world);
    world = NULL;
    if (root[0]) {
        char path[224];
        snprintf(path, sizeof path, "%s.meta", behavior_path);
        unlink(path);
        rmdir(path);
        unlink(behavior_path);
        unlink(library_path);
        snprintf(path, sizeof path, "%s/behaviors", root);
        rmdir(path);
        snprintf(path, sizeof path, "%s/libraries", root);
        rmdir(path);
        rmdir(root);
    }
    root[0] = 0;
}
static bool write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        return false;
    }
    bool ok = fputs(text, file) >= 0;
    return !fclose(file) && ok;
}
static bool open_project(const char *text, const char *library)
{
    strcpy(root, "/tmp/chibi-source-session-XXXXXX");
    if (!mkdtemp(root)) {
        return false;
    }
    char folder[192];
    snprintf(folder, sizeof folder, "%s/behaviors", root);
    if (mkdir(folder, 0700)) {
        return false;
    }
    snprintf(folder, sizeof folder, "%s/libraries", root);
    if (mkdir(folder, 0700)) {
        return false;
    }
    snprintf(behavior_path, sizeof behavior_path, "%s/behaviors/Consumer.chibi", root);
    snprintf(library_path, sizeof library_path, "%s/libraries/Shared.chibi", root);
    if (!write_file(behavior_path, text) ||
        (library && !write_file(library_path, library))) {
        return false;
    }
    world = ecs_world_new();
    preview = game_preview_world_new(world);
    code = asset_code_open(behavior_path, error, sizeof error);
    if (!code) {
        return false;
    }
    asset_code_set_source_world(code, preview, root);
    asset_code_time(code, 1);
    if (!asset_code_validate(code, NULL)) {
        fprintf(stderr, "source session: %s\n", asset_code_error(code));
        return false;
    }
    return true;
}
static const script_symbol *state(const authoring_source *source, const char *name)
{
    for (int i = 0; source && i < source->analysis->symbol_count; i++) {
        const script_symbol *s = &source->analysis->symbols[i];
        if (s->kind == SCRIPT_STATE && !strcmp(s->name, name)) {
            return s;
        }
    }
    return NULL;
}
TEST_CASE(source_session_draft_history_save_and_conflict)
{
    fprintf(stderr, "source-session: source_session_draft_history_save_and_conflict\n");
    const char *saved = "behavior Consumer { count:int=1; }";
    const char *invalid = "behavior Consumer { count:int=; }";
    const char *changed = "// authored\nbehavior Consumer { count:int=2; }";
    REQUIRE(open_project(saved, NULL));
    REQUIRE(!asset_code_dirty(code) && state(asset_code_source(code), "count"));
    REQUIRE(asset_code_replace(code, saved, invalid));
    asset_code_time(code, 2);
    REQUIRE(!asset_code_validate(code, NULL) && asset_code_error(code)[0]);
    const authoring_source *last_valid = asset_code_graph_source(code, NULL);
    REQUIRE(
        last_valid && last_valid->analysis->valid && last_valid->syntax &&
        !strcmp(last_valid->syntax->text, saved)
    );
    REQUIRE(!asset_code_save(code, error, sizeof error) && error[0]);
    checkpoint = asset_code_capture(code);
    restored = asset_code_restore(checkpoint, NULL);
    REQUIRE(restored);
    asset_code_set_source_world(restored, preview, root);
    REQUIRE(!strcmp(asset_code_text(restored), invalid));
    REQUIRE(asset_code_can_replay(restored, false));
    REQUIRE(asset_code_replay(restored, false, false));
    asset_code_time(restored, 3);
    REQUIRE(asset_code_validate(restored, NULL));
    REQUIRE(!strcmp(asset_code_text(restored), saved));
    REQUIRE(asset_code_replay(restored, true, false));
    REQUIRE(!strcmp(asset_code_text(restored), invalid));
    REQUIRE(!asset_code_replace(restored, saved, changed));
    REQUIRE(asset_code_replace(restored, invalid, changed));
    asset_code_time(restored, 4);
    REQUIRE(asset_code_validate(restored, NULL));
    REQUIRE(asset_code_save(restored, error, sizeof error));
    REQUIRE(!asset_code_dirty(restored));
    owned = file_read_text(behavior_path);
    REQUIRE(owned && !strcmp(owned, changed));
    REQUIRE(write_file(behavior_path, "behavior Consumer { count:int=9; }"));
    REQUIRE(asset_code_replace(restored, changed, "behavior Consumer { count:int=3; }"));
    asset_code_time(restored, 5);
    REQUIRE(asset_code_validate(restored, NULL));
    REQUIRE(!asset_code_save(restored, error, sizeof error) && strstr(error, "disk"));
    REQUIRE(strstr(asset_code_text(restored), "count:int=3"));
    return true;
}
TEST_CASE(source_session_navigation_and_catalog_generation)
{
    fprintf(stderr, "source-session: source_session_navigation_and_catalog_generation\n");
    const char
        *library = "library Shared { function Seven(extra:int=4)->(answer:int) "
                   "{ return(extra); } }";
    const char *text = "behavior Consumer { count:int=1; on Tick { "
                       "chosen:=Shared.Seven(); "
                       "authored:=Shared.Seven(extra:4); } }";
    REQUIRE(open_project(text, library));
    const authoring_source *source = asset_code_source(code);
    REQUIRE(source && source->valid);
    const authoring_navigation *nav = asset_code_navigation(code);
    int document = asset_code_document(code);
    int call = (int)(strstr(text, "Seven") - text);
    const authoring_reference *ref = authoring_navigation_at(nav, document, call);
    REQUIRE(ref && ref->target.kind == AUTHORING_CALLABLE);
    uint64_t callable_id = ref->target.id;
    const authoring_reference *definition = authoring_navigation_definition(nav, ref->target);
    REQUIRE(definition && definition->location.document != document);
    owned = authoring_source_hover(nav, document, call);
    REQUIRE(owned && strstr(owned, "extra: int = 4"));
    authoring_source_presentation *presentation = asset_code_presentation(code);
    const script_symbol *counter = state(source, "count");
    node *placed = NULL;
    uint64_t node_id = 0;
    for (int i = 0; i < authoring_source_presentation_origin_count(presentation); i++) {
        const source_presentation_origin
            *o = authoring_source_presentation_origin_at(presentation, i);
        if (o->semantic == counter->id && o->node) {
            placed = graph_node_by_id(
                authoring_source_presentation_view(presentation)->graph,
                o->node
            );
            node_id = o->node;
            break;
        }
    }
    REQUIRE(placed);
    placed->x = 137;
    placed->y = 251;
    placed->placed = true;
    previous = authoring_sources_retain(game_preview_sources_working(preview));
    const char *next = "library Shared { function "
                       "Seven(extra:int=6)->(answer:int) { return(extra); } }";
    int status = game_preview_source_edit(preview, library_path, next, error, sizeof error);
    if (status) {
        fprintf(stderr, "catalog edit: %s\n", error);
    }
    REQUIRE(status == 0);
    asset_code_set_source_world(code, preview, root);
    asset_code_time(code, 2);
    REQUIRE(asset_code_validate(code, NULL));
    REQUIRE(!asset_code_dirty(code) && !strcmp(asset_code_text(code), text));
    char *fresh = authoring_source_hover(asset_code_navigation(code), asset_code_document(code), call);
    bool changed_default = fresh && strstr(fresh, "extra: int = 6");
    free(fresh);
    authoring_sources_free(previous);
    previous = NULL;
    REQUIRE(changed_default);
    const script_analysis *analysis = asset_code_source(code)->analysis;
    int inherited = 0, authored = 0;
    for (int i = 0; i < analysis->expression_count; i++) {
        const script_expression *call_expression = &analysis->expressions[i];
        if (call_expression->kind != SCRIPT_EXPR_CALL ||
            call_expression->capability != callable_id) {
            continue;
        }
        REQUIRE(call_expression->argument_count == 1);
        const script_expression
            *argument = &analysis->expressions[call_expression->arguments[0].value];
        inherited += argument->constant.type == PIN_I32 && argument->constant.i == 6;
        authored += argument->constant.type == PIN_I32 && argument->constant.i == 4;
    }
    REQUIRE(inherited == 1 && authored == 1);
    placed = graph_node_by_id(
        authoring_source_presentation_view(asset_code_presentation(code))->graph,
        node_id
    );
    REQUIRE(placed && placed->x == 137 && placed->y == 251 && placed->placed);
    REQUIRE(strstr(owned, "extra: int = 4"));
    return true;
}
TEST_CASE(source_session_presentation_history_and_paused_generation)
{
    fprintf(stderr, "source-session: source_session_presentation_history_and_paused_generation\n");
    const char *text = "behavior Consumer { count:int=1; }";
    REQUIRE(open_project(text, NULL));
    authoring_source_presentation *presentation = asset_code_presentation(code);
    REQUIRE(presentation);
    view = editor_new(behavior_asset_new("SourceView"));
    REQUIRE(editor_source_presentation(view, presentation) == 0);
    const source_presentation_origin *origin = NULL;
    for (int i = 0; i < authoring_source_presentation_origin_count(presentation); i++) {
        const source_presentation_origin
            *candidate = authoring_source_presentation_origin_at(presentation, i);
        if (candidate->kind == SOURCE_PRESENTATION_SYMBOL && candidate->node) {
            origin = candidate;
            break;
        }
    }
    REQUIRE(origin);
    uint64_t graph = origin->graph, id = origin->node;
    node *n = graph_node_by_id(
        authoring_source_presentation_view(editor_presentation(view))->graph,
        id
    );
    REQUIRE(n);
    n->x = 137;
    n->y = 251;
    n->placed = true;
    n->breakpoint = true;
    REQUIRE(asset_code_sync_graph(code, view, false));
    asset_code_execution(code, graph, id);
    int start, end;
    REQUIRE(asset_code_execution_location(code, &start, &end));
    REQUIRE(start < end && end <= (int)strlen(text));
    const char *draft = "behavior Consumer { count:int=; }";
    REQUIRE(asset_code_replace(code, text, draft));
    REQUIRE(!asset_code_execution_location(code, &start, &end));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!strcmp(asset_code_text(code), text));
    presentation = asset_code_presentation(code);
    n = graph_node_by_id(authoring_source_presentation_view(presentation)->graph, id);
    REQUIRE(n && n->x == 137 && n->y == 251 && n->placed && n->breakpoint);
    REQUIRE(asset_code_replay(code, true, false));
    REQUIRE(!strcmp(asset_code_text(code), draft));
    const char *two = "behavior Consumer { count:int=2; }";
    const char *three = "behavior Consumer { count:int=3; }";
    const char *four = "behavior Consumer { count:int=4; }";
    const char *five = "behavior Consumer { count:int=5; }";
    const char *six = "behavior Consumer { count:int=6; }";
    REQUIRE(asset_code_replace(code, draft, text));
    REQUIRE(asset_code_edit_source(code, text, two, NULL, false));
    REQUIRE(asset_code_edit_source(code, two, three, NULL, true));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!strcmp(asset_code_text(code), text));
    REQUIRE(asset_code_replay(code, true, false));
    REQUIRE(!strcmp(asset_code_text(code), three));
    REQUIRE(!asset_code_edit_source(code, two, four, NULL, true));
    REQUIRE(!strcmp(asset_code_text(code), three));
    REQUIRE(asset_code_replace(code, three, four));
    REQUIRE(asset_code_edit_source(code, four, five, NULL, true));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!strcmp(asset_code_text(code), four));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!strcmp(asset_code_text(code), three));
    REQUIRE(asset_code_replay(code, true, false));
    REQUIRE(!strcmp(asset_code_text(code), four));
    REQUIRE(asset_code_edit_source(code, four, six, NULL, true));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!strcmp(asset_code_text(code), four));
    REQUIRE(asset_code_replay(code, true, false));
    REQUIRE(!strcmp(asset_code_text(code), six));
    return true;
}
TEST_CASE(source_session_sidecar_conflict_and_partial_save_retry)
{
    fprintf(stderr, "source-session: source_session_sidecar_conflict_and_partial_save_retry\n");
    const char *text = "behavior Consumer { count:int=1; }";
    const char *changed = "behavior Consumer { count:int=2; }";
    REQUIRE(open_project(text, NULL));
    REQUIRE(asset_code_replace(code, text, changed));
    asset_code_time(code, 2);
    REQUIRE(asset_code_validate(code, NULL));
    char sidecar[224];
    snprintf(sidecar, sizeof sidecar, "%s.meta", behavior_path);
    REQUIRE(!mkdir(sidecar, 0700));
    REQUIRE(!asset_code_save(code, error, sizeof error));
    owned = file_read_text(behavior_path);
    REQUIRE(owned && !strcmp(owned, changed));
    REQUIRE(!rmdir(sidecar));
    REQUIRE(asset_code_save(code, error, sizeof error));
    REQUIRE(!asset_code_dirty(code));
    REQUIRE(write_file(sidecar, "{\"external\":true}"));
    const char *draft = "behavior Consumer { count:int=3; }";
    REQUIRE(asset_code_replace(code, changed, draft));
    asset_code_time(code, 3);
    REQUIRE(asset_code_validate(code, NULL));
    REQUIRE(!asset_code_save(code, error, sizeof error) && strstr(error, "metadata"));
    REQUIRE(!strcmp(asset_code_text(code), draft));
    asset_code_refresh(code);
    REQUIRE(strstr(asset_code_error(code), "conflict"));
    REQUIRE(!strcmp(asset_code_text(code), draft));
    return true;
}

static void declarative_checkpoint()
{
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_doc_set_root(doc, root);
    const char *path = "/tmp/chibi-source-checkpoint.chibi";
    const char *valid = "package asset\n"
                        "\n"
                        "import data \"game:assets\"\n"
                        "\n"
                        "pub Build :: () -> byte[] {\n"
                        "    return data.Encode(\n"
                        "        data.Object([\n"
                        "            data.Field.Named(\"name\", "
                        "data.Value.Text(\"Last good\"))\n"
                        "        ])\n"
                        "    )\n"
                        "}\n";
    av_obj_add_str(doc, root, "path", path);
    av_obj_add_str(
        doc,
        root,
        "disk",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Saved\"))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    av_obj_add_str(doc, root, "text", "package asset\n\npub Build :: () -> byte[] {");
    av_obj_add_str(doc, root, "valid", valid);
    av_obj_add_bool(doc, root, "overlay", true);
    av_obj_add_int(doc, root, "step", 1);
    av_obj_add_int(doc, root, "cursor", 3);
    av_obj_add_int(doc, root, "anchor", 12);
    av_value *history = av_obj_add_arr(doc, root, "history");
    av_obj_add_str(
        doc,
        av_arr_add_obj(doc, history),
        "text",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Saved\"))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    av_obj_add_str(doc, av_arr_add_obj(doc, history), "text", "package asset\n\npub Build :: () -> byte[] {");
    av_obj_add_str(
        doc,
        av_arr_add_obj(doc, history),
        "text",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"Finished\"))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );

    /* An invalid draft and the redo branch survive replacement independently
     * of the last valid preview and saved disk baseline. */
    asset_code *code = asset_code_restore(root, NULL);
    assert(code && asset_code_dirty(code) && asset_code_error(code)[0]);
    assert(!strcmp(text_overlay_text(path), valid));
    assert(kit_code_cursor(asset_code_editor(code)) == 3);
    int start, end;
    assert(kit_code_selection(asset_code_editor(code), &start, &end));
    assert(start == 3 && end == 12);
    av_value *snapshot = asset_code_capture(code);
    assert(av_equals(snapshot, root));
    assert(asset_code_has_state(code, snapshot));
    char error[256];
    assert(!asset_code_save(code, error, sizeof error));
    assert(error[0]);
    asset_code *replacement = asset_code_restore(snapshot, NULL);
    assert(replacement);
    asset_code_free(code);
    assert(!strcmp(text_overlay_text(path), valid));
    asset_code_free(replacement);
    assert(!text_overlay_text(path));

    code = asset_code_restore(snapshot, NULL);
    assert(
        code && !strcmp(kit_code_text(asset_code_editor(code), NULL), "package asset\n\npub Build :: () -> byte[] {")
    );
    av_value *again = asset_code_capture(code);
    assert(av_equals(snapshot, again));
    asset_code_free(code);
    av_value_free(again);
    av_value_free(snapshot);
    av_doc_free(doc);

    /* Independent visual changes merge into an unsaved source draft without
     * canonicalizing its comments or overwriting the typed name. */
    char file_path[] = "/tmp/chibi-source-refresh-XXXXXX";
    int fd = mkstemp(file_path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    fputs(
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Saved\")),\n"
        "            data.Field.Named(\"value\", data.Value.Int(2))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        file
    );
    assert(!fclose(file));
    doc = av_doc_new();
    root = av_obj(doc);
    av_doc_set_root(doc, root);
    av_obj_add_str(doc, root, "path", file_path);
    av_obj_add_str(
        doc,
        root,
        "disk",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Saved\")),\n"
        "            data.Field.Named(\"value\", data.Value.Int(1))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    av_obj_add_str(
        doc,
        root,
        "text",
        "// draft\n"
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\",  data.Value.Text(\"Draft\")),\n"
        "            data.Field.Named(\"value\", data.Value.Int(1))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    av_obj_add_str(
        doc,
        root,
        "valid",
        "// draft\n"
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\",  data.Value.Text(\"Draft\")),\n"
        "            data.Field.Named(\"value\", data.Value.Int(1))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    av_obj_add_bool(doc, root, "overlay", true);
    av_obj_add_int(doc, root, "step", 0);
    history = av_obj_add_arr(doc, root, "history");
    av_obj_add_str(
        doc,
        av_arr_add_obj(doc, history),
        "text",
        "// draft\n"
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\",  data.Value.Text(\"Draft\")),\n"
        "            data.Field.Named(\"value\", data.Value.Int(1))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    code = asset_code_restore(root, NULL);
    assert(code);
    asset_code_refresh(code);
    assert(!strcmp(
        kit_code_text(asset_code_editor(code), NULL),
        "// draft\n"
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\",  data.Value.Text(\"Draft\")),\n"
        "            data.Field.Named(\"value\", data.Value.Int(2))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    assert(asset_code_dirty(code));
    assert(asset_code_save(code, error, sizeof error));
    assert(!asset_code_dirty(code));
    asset_code_free(code);
    av_doc_free(doc);
    assert(!unlink(file_path));
}
void test_asset_code_checkpoint()
{
    fprintf(stderr, "source-session: declarative checkpoint\n");
    declarative_checkpoint();
    pu_setup(NULL, cleanup);
    RUN_TEST_CASE(source_session_draft_history_save_and_conflict);
    RUN_TEST_CASE(source_session_navigation_and_catalog_generation);
    RUN_TEST_CASE(source_session_presentation_history_and_paused_generation);
    RUN_TEST_CASE(source_session_sidecar_conflict_and_partial_save_retry);
    pu_clear_setup();
}
