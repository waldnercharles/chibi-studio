#define PICO_UNIT_IMPLEMENTATION
#include "vendor/pico_unit.h"
#include "editor/studio/studio_internal.h"
#include "editor/documents/asset_code.h"
#include "authoring/source_presentation.h"
#include "support/array.h"
#include "support/file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

studio *studio_acceptance_studio(void *);
int studio_acceptance_host_main(int, char **);

static const char *project;
static const char *executable;
static int frames, edits, undos, redos;
static bool callback_ok = true;
static const char counter_bytes[] =
    "behavior Counter {\n"
    "    state count: int = 1;\n"
    "    on Tick {\n"
    "        count = count + 1;\n"
    "    }\n"
    "}\n";

static bool unchanged_source()
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Counter.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    char *text = file_read_text(path);
    REQUIRE(text);
    bool same = !strcmp(text, counter_bytes);
    free(text);
    REQUIRE(same);
    return true;
}

static asset_code *counter_source(studio *s)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        asset_code *code = s->code.assets[i];
        const authoring_source *source = asset_code_source(code);
        if (source && source->valid && !strcmp(source->analysis->name, "Counter")) {
            return code;
        }
    }
    return NULL;
}

/* Check exact source, resolved initializer and current owned graph generation. */
static bool check_counter(studio *s, int expected, bool dirty, const node **literal)
{
    asset_code *code = counter_source(s);
    REQUIRE(code);
    const authoring_source *source = asset_code_source(code);
    REQUIRE(source && source->valid);
    const script_analysis *a = source->analysis;
    int expression = -1;
    for (int i = 0; i < a->symbol_count; i++) {
        if (a->symbols[i].kind == SCRIPT_STATE && !strcmp(a->symbols[i].name, "count")) {
            REQUIRE(expression == -1);
            expression = a->symbols[i].initializer;
        }
    }
    REQUIRE(expression >= 0 && expression < a->expression_count);
    const script_expression *value = &a->expressions[expression];
    const char *text = asset_code_text(code);
    bool actual_dirty = asset_code_dirty(code);
    if (actual_dirty != dirty || value->kind != SCRIPT_EXPR_CONSTANT ||
        value->constant.type != PIN_I32 || value->constant.i != expected) {
        av_value *state = asset_code_capture(code);
        const char *disk = av_get_str(av_obj_get(state, "disk"));
        const char *baseline = av_get_str(av_obj_get(state, "disk_metadata"));
        const char *metadata = av_get_str(av_obj_get(state, "metadata"));
        const av_value *history = av_obj_get(state, "history");
        int step = av_get_int(av_obj_get(state, "step"));
        const char *snapshot = av_get_str(av_obj_get(av_arr_get(history, step), "metadata"));
        baseline = baseline ? baseline : "";
        metadata = metadata ? metadata : "";
        snapshot = snapshot ? snapshot : "";
        size_t at = 0;
        while (baseline[at] && metadata[at] && baseline[at] == metadata[at]) {
            at++;
        }
        fprintf(stderr,
                "Studio dirty diagnostic: text_equals_disk=%d step=%d history=%zu "
                "baseline_bytes=%zu metadata_bytes=%zu snapshot_equals_metadata=%d "
                "first_difference=%zu baseline=%.80s metadata=%.80s\n",
                disk && !strcmp(text, disk), step, av_arr_size(history),
                strlen(baseline), strlen(metadata), !strcmp(snapshot, metadata),
                at, baseline + at, metadata + at);
        size_t count = av_arr_size(history);
        for (size_t i = 0; i < count && i < 8; i++) {
            const av_value *entry = av_arr_get(history, i);
            const char *entry_text = av_get_str(av_obj_get(entry, "text"));
            const char *entry_metadata = av_get_str(av_obj_get(entry, "metadata"));
            entry_metadata = entry_metadata ? entry_metadata : "";
            size_t difference = 0;
            while (baseline[difference] && entry_metadata[difference] &&
                   baseline[difference] == entry_metadata[difference]) {
                difference++;
            }
            yyjson_doc *doc = yyjson_read(entry_metadata, strlen(entry_metadata), 0);
            yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
            fprintf(stderr,
                    "Studio history[%zu]: text=%.120s metadata_bytes=%zu keys=%zu "
                    "nodes=%zu watches=%zu boxes=%zu routes=%zu first_difference=%zu "
                    "baseline=%.80s metadata=%.80s\n",
                    i, entry_text ? entry_text : "", strlen(entry_metadata),
                    yyjson_obj_size(root), yyjson_arr_size(yyjson_obj_get(root, "source_layout")),
                    yyjson_arr_size(yyjson_obj_get(root, "source_pin_watches")),
                    yyjson_arr_size(yyjson_obj_get(root, "source_comment_boxes")),
                    yyjson_arr_size(yyjson_obj_get(root, "source_edge_routes")),
                    difference, baseline + difference, entry_metadata + difference);
            if (i > 0) {
                const char *previous = av_get_str(av_obj_get(av_arr_get(history, i - 1), "metadata"));
                previous = previous ? previous : "";
                size_t changed = 0;
                while (previous[changed] && entry_metadata[changed] &&
                       previous[changed] == entry_metadata[changed]) {
                    changed++;
                }
                fprintf(stderr, "Studio history delta[%zu]: at=%zu previous=%.120s current=%.120s\n",
                        i, changed, previous + changed, entry_metadata + changed);
            }
            yyjson_doc_free(doc);
        }
        av_value_free(state);
    }
    REQUIRE(value->kind == SCRIPT_EXPR_CONSTANT);
    REQUIRE(value->constant.type == PIN_I32 && value->constant.i == expected);
    REQUIRE(value->range.end == value->range.start + 1);
    REQUIRE(text[value->range.start] == '0' + expected);
    editor *e = s->scene->graph_editor;
    const authoring_source_presentation *view = editor_presentation(e);
    REQUIRE(view);
    REQUIRE(!strcmp(authoring_source_presentation_text(view), text));
    REQUIRE(authoring_source_presentation_matches(view, a, asset_code_catalog(code)));
    const graph *g = editor_active_graph(e);
    REQUIRE(g);
    const node *found = NULL;
    for (int i = 0; i < authoring_source_presentation_origin_count(view); i++) {
        const source_presentation_origin *o = authoring_source_presentation_origin_at(view, i);
        if (o->pin || o->kind != SOURCE_PRESENTATION_EXPRESSION ||
            o->record != expression || o->graph != g->graph_id) {
            continue;
        }
        for (int j = 0; j < acount(g->nodes); j++) {
            if (g->nodes[j]->id == o->node) {
                REQUIRE(!found);
                found = g->nodes[j];
            }
        }
    }
    REQUIRE(found);
    char *shown = studio_code_expression(s, found);
    REQUIRE(shown);
    bool equal = shown[0] == '0' + expected && !shown[1];
    free(shown);
    REQUIRE(equal);
    REQUIRE(actual_dirty == dirty);
    if (literal) {
        *literal = found;
    }
    return true;
}

static bool source_edit_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 4);
    if (frames == 1) {
        const node *literal = NULL;
        REQUIRE(check_counter(s, 1, false, &literal));
        s->prefs.code_apply_live = true;
        studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
        studio_show_editor(s, STUDIO_EDITOR_CODE);
        char error[256];
        bool changed = studio_code_expression_replace(s, literal, "2", error, sizeof error);
        if (!changed) {
            fprintf(stderr, "Studio edit acceptance: %s\n", error);
        }
        REQUIRE(changed);
        edits++;
    } else if (frames == 2) {
        REQUIRE(check_counter(s, 2, true, NULL));
        REQUIRE(studio_code_history(s, false, true));
        undos++;
        REQUIRE(check_counter(s, 1, false, NULL));
    } else if (frames == 3) {
        REQUIRE(check_counter(s, 1, false, NULL));
        REQUIRE(studio_code_history(s, true, true));
        redos++;
        REQUIRE(check_counter(s, 2, true, NULL));
    } else {
        REQUIRE(check_counter(s, 2, true, NULL));
        REQUIRE(studio_editor_shown(s, STUDIO_EDITOR_BEHAVIOR));
        REQUIRE(studio_editor_shown(s, STUDIO_EDITOR_CODE));
    }
    return true;
}

bool studio_acceptance_frame(void *state)
{
    callback_ok = source_edit_frame(state);
    return callback_ok;
}

TEST_CASE(source_graph_edit_undo_redo)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless", "--frames", "4",
                     "--fixed-dt", "0.016666666666666666", "--project-root",
                     (char *)project, "--open", "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok);
    REQUIRE(unchanged_source());
    REQUIRE(frames == 4 && edits == 1 && undos == 1 && redos == 1);
    printf("Studio source edit accepted: frames=%d edits=%d undo=%d redo=%d\n",
           frames, edits, undos, redos);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "--source-edit") ||
        strcmp(argv[2], "--project-root")) {
        fprintf(stderr, "Usage: studio-acceptance --source-edit --project-root PATH\n");
        return 2;
    }
    executable = argv[0];
    project = argv[3];
    char prefs[1024], cache[1024];
    int p = snprintf(prefs, sizeof prefs, "%s/.acceptance-prefs.json", project);
    int c = snprintf(cache, sizeof cache, "%s/.acceptance-thumbnails", project);
    if (p <= 0 || (size_t)p >= sizeof prefs || c <= 0 || (size_t)c >= sizeof cache ||
        setenv("CHIBI_STUDIO_PREFS_PATH", prefs, 1) ||
        setenv("CHIBI_THUMBNAIL_CACHE", cache, 1)) {
        fprintf(stderr, "Could not isolate Studio acceptance preferences/cache\n");
        return 2;
    }
    pu_display_time(true);
    RUN_TEST_CASE(source_graph_edit_undo_redo);
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
