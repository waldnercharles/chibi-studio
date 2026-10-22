/* Studio's editor nodes (authoring/studio_schema.h) run only in Studio's
 * own project: the graph editor's node search offers them there and nowhere
 * else, searchNodes does as well, and a behavior in another project that holds
 * one fails validation. */
#include "authoring/studio_schema.h"
#include "automation/controller.h"
#include "editor/graph/search.h"
#include "editor/workspace/editor_internal.h"
#include "graph/nodes/studio_nodes.h"
#include "support/array.h"
#include "test/suite_internal.h"
#include "third_party/yyjson/yyjson.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MISSING "Editor nodes require Studio"

/* How many of the search's matches are the pack's nodes, and whether `name`
   is among them. */
static int pack_matches(const ed_search_match *matches, const char *name, bool *named)
{
    int count = 0;
    for (int i = 0; i < acount(matches); i++) {
        bool pack = studio_node_is(matches[i].desc);
        count += pack;
        *named |= pack && !strcmp(matches[i].desc->name, name);
    }
    return count;
}

static bool reports_missing(const compile_report *report)
{
    for (int i = 0; i < acount(report->items); i++) {
        const compile_diagnostic *d = &report->items[i];
        if (d->severity == CMPL_ERROR && d->rule == CMPL_RULE_EDITOR_NODE &&
            !strcmp(d->message, MISSING)) {
            return true;
        }
    }
    return false;
}

/* The Add menu's search, the palette's and a dragged wire's all list what
   ed_search_matches does. */
static void editor_search()
{
    behavior_asset *asset = behavior_asset_new("Gate");
    editor *e = editor_new(asset);
    assert(e && graph_add_node(asset->graph, registry_node_by_name("Scene Info")));
    bool named = false;
    ed_search_match *game = ed_search_matches(e, "Scene Objects");
    int hidden = pack_matches(game, "Scene Objects", &named);
    ed_search_match *every = ed_search_matches(e, "");
    bool listed = false;
    int hidden_all = pack_matches(every, "Scene Objects", &listed);
    ed_compile(e);
    bool refused = reports_missing(&e->compile);
    test_check(
        hidden == 0 && !named && hidden_all == 0 && !listed && refused,
        "a game project's node search leaves out Studio's editor nodes, and "
        "its Problems "
        "name one a behavior holds"
    );
    afree(game);
    afree(every);

    editor_set_studio_nodes(e, true);
    named = false;
    ed_search_match *studio = ed_search_matches(e, "Scene Objects");
    pack_matches(studio, "Scene Objects", &named);
    ed_search_match *all = ed_search_matches(e, "");
    listed = false;
    int offered = pack_matches(all, "Scene Objects", &listed);
    ed_compile(e);
    test_check(
        named && listed && offered == STUDIO_PACK.node_count &&
            !reports_missing(&e->compile),
        "the Studio project's node search offers each of Studio's editor "
        "nodes, and its "
        "Problems accept them"
    );
    afree(studio);
    afree(all);
    editor_free(e);
}

static char *search_in(const char *root, const char *query)
{
    char request[256];
    snprintf(request, sizeof request, "{\"contract\":1,\"operation\":\"search_nodes\",\"query\":\"%s\"}", query);
    int exit_code = 0;
    return chibi_automation_handle(request, root, &exit_code);
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs(text, file);
    fclose(file);
}

/* searchNodes lists the pack for the Studio project only, and validateAsset
   refuses a behavior with one in any other. */
static void controller()
{
    char *game = search_in(
        "test/fixtures/language",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"scene\", "
        "data.Value.Text(\"objects\"))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    char *studio = search_in(
        "projects/studio",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"scene\", "
        "data.Value.Text(\"objects\"))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    test_check(
        game && !strstr(game, "\"Scene Objects\"") && studio &&
            strstr(studio, "\"Scene Objects\""),
        "searchNodes offers Studio's editor nodes in the Studio project only"
    );
    free(game);
    free(studio);

    char root[] = "/tmp/chibi-studio-nodes-XXXXXX", content[96], behaviors[96],
         path[128];
    assert(mkdtemp(root));
    snprintf(content, sizeof content, "%s/content", root);
    snprintf(behaviors, sizeof behaviors, "%s/behaviors", content);
    snprintf(path, sizeof path, "%s/Gate.chibi", behaviors);
    assert(!mkdir(content, 0755) && !mkdir(behaviors, 0755));
    behavior_asset *asset = behavior_asset_new("Gate");
    assert(graph_add_node(asset->graph, registry_node_by_name("Scene Info")));
    char *text = behavior_asset_save(asset);
    write_text(path, text);
    free(text);
    behavior_asset_free(asset);
    int exit_code = 0;
    char *response = chibi_automation_handle(
        "{\"contract\":1,\"operation\":\"validate_asset\",\"kind\":"
        "\"behavior\",\"name\":\"Gate\"}",
        root,
        &exit_code
    );
    test_check(exit_code != 0 && strstr(response, MISSING) && strstr(response, "\"CMPL-29\""), "validateAsset fails a behavior holding a Studio editor node in another project");
    free(response);
    unlink(path);
    rmdir(behaviors);
    rmdir(content);
    rmdir(root);
}

void test_studio_nodes_gate()
{
    editor_search();
    controller();
}
