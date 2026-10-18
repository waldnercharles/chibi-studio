/* Draft standalone arithmetic route: four pico cases, 16 page rows, 10 text/
 * hierarchy examples, 12 invalid captures, and 17 synthetic allowance rows.
 * No source preparation, dispatch, world, UI, renderer, process or filesystem.
 * At most eight actors and one eight-entry temporary depth vector per call.
 * Unit weights 1/2 exercise arithmetic only; neither is a production K. */
#include "editor/studio/studio_outliner_work.h"

#include <limits.h>
#include <stdint.h>

typedef struct
{
    studio_scene_read_facts facts;
    authoring_scene_node nodes[8];
    studio_scene_read_actor actors[8];
    studio_scene_read_prototype prototype;
    studio_scene_read_component components[3];
    studio_scene_read_field fields[4];
    studio_scene_read_attachment attachments[2];
} outliner_work_fixture;

static void outliner_work_four(outliner_work_fixture *f, bool chain)
{
    *f = (outliner_work_fixture){ 0 };
    static const int first[] = { 0, 2, 3, 3 };
    static const int count[] = { 2, 1, 0, 1 };
    for (int i = 0; i < 4; i++) {
        f->nodes[i] = (authoring_scene_node){ .name = "aaaaa",
                                              .parent = chain ? i - 1 : -1 };
        f->actors[i] = (studio_scene_read_actor){
            .prototype_name = "aa",
            .prototype = 0,
            .field_first = first[i],
            .field_count = count[i],
        };
    }
    f->prototype = (studio_scene_read_prototype){ .component_count = 3 };
    /* Both retained attachment entries have the same identity; the workload
     * counts raw entries without deduplication or matching them to a row. */
    f->facts = (studio_scene_read_facts){
        .nodes = f->nodes,
        .actors = f->actors,
        .actor_count = 4,
        .prototypes = &f->prototype,
        .prototype_count = 1,
        .components = f->components,
        .component_count = 3,
        .fields = f->fields,
        .field_count = 4,
        .attachments = f->attachments,
        .attachment_count = 2,
        .filter = "aab",
        .scene_path = "/",
    };
}

TEST_CASE(outliner_work_page_bounds)
{
    static const struct
    {
        int first, count;
        bool collapsed;
        uintptr_t visible, visits;
    } cases[] = {
        { INT_MIN, 4, false, 3, 207 },
        { 0, 4, false, 3, 207 },
        { 1, 4, false, 4, 226 },
        { 2, 4, false, 3, 207 },
        { INT_MAX, 4, false, 0, 142 },
        { 0, 0, false, 0, 142 },
        { 0, -1, false, 0, 142 },
        { 0, 1, false, 0, 142 },
        { 1, 1, false, 1, 165 },
        { 2, 2, false, 2, 188 },
        { INT_MIN, INT_MAX, false, 4, 226 },
        { INT_MAX, INT_MAX, false, 0, 142 },
        { 1, INT_MIN, false, 0, 142 },
        { 0, INT_MAX, false, 4, 226 },
        { 1, 4, true, 0, 142 },
        /* Reveal reads its already-forced-open protected state at Capture. */
        { 1, 4, false, 4, 226 },
    };
    outliner_work_fixture f;
    outliner_work_four(&f, true);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        studio_outliner_work work;
        REQUIRE(!studio_outliner_work_measure(
            &f.facts,
            cases[i].first,
            cases[i].count,
            cases[i].collapsed,
            "",
            &work
        ));
        REQUIRE(
            work.actors == 4 && work.prototypes == 1 && work.components == 3 &&
            work.fields == 4 && work.attachments == 2
        );
        REQUIRE(
            work.visible == cases[i].visible && work.max_depth == 3 &&
            work.max_components == 3 && work.max_fields == 2
        );
        REQUIRE(
            work.match_starts == 12 && work.match_pairs == 36 &&
            work.actor_text_bytes == 28 && work.filter_bytes == 3 && work.path_bytes == 1
        );
        uintptr_t total = 0;
        REQUIRE(studio_outliner_work_limit(&work, 1, &total));
        REQUIRE(total == 10000 + cases[i].visits);
    }
    return true;
}

static bool outliner_work_expect(
    const studio_scene_read_facts *facts,
    const char *reveal,
    uintptr_t visits,
    uintptr_t starts,
    uintptr_t pairs,
    uintptr_t depth
)
{
    studio_outliner_work work;
    REQUIRE(!studio_outliner_work_measure(facts, 1, INT_MAX, false, reveal, &work));
    REQUIRE(work.match_starts == starts && work.match_pairs == pairs && work.max_depth == depth);
    uintptr_t total = 0;
    REQUIRE(studio_outliner_work_limit(&work, 1, &total));
    REQUIRE(total == 10000 + visits);
    return true;
}

TEST_CASE(outliner_work_text_and_hierarchy)
{
    char path[129];
    static const struct
    {
        size_t bytes;
        uintptr_t visits;
    } paths[] = { { 0, 8 }, { 1, 10 }, { 127, 262 }, { 128, 263 } };
    studio_scene_read_facts empty = { .filter = "" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
        memset(path, 'p', sizeof path);
        path[paths[i].bytes] = '\0';
        empty.scene_path = path;
        REQUIRE(outliner_work_expect(&empty, "", paths[i].visits, 0, 0, 0));
    }
    outliner_work_fixture f;
    outliner_work_four(&f, false);
    f.facts.filter = "";
    REQUIRE(outliner_work_expect(&f.facts, "", 94, 0, 0, 0));
    f.facts.filter = "aab123";
    REQUIRE(outliner_work_expect(&f.facts, "", 94, 0, 0, 0));
    f.facts.filter = "ab";
    REQUIRE(outliner_work_expect(&f.facts, "", 234, 20, 40, 0));

    char name[65], query[33];
    memset(name, 'a', sizeof name - 1);
    name[64] = '\0';
    memset(query, 'a', sizeof query - 1);
    query[31] = 'b';
    query[32] = '\0';
    f = (outliner_work_fixture){ 0 };
    f.nodes[0] = (authoring_scene_node){ .name = name, .parent = -1 };
    f.actors[0] = (studio_scene_read_actor){ .prototype_name = "", .prototype = -1 };
    f.facts = (studio_scene_read_facts){
        .nodes = f.nodes,
        .actors = f.actors,
        .actor_count = 1,
        .filter = query,
        .scene_path = "",
    };
    REQUIRE(outliner_work_expect(&f.facts, "18446744073709551615", 3220, 33, 1056, 0));

    for (int i = 0; i < 8; i++) {
        f.nodes[i] = (authoring_scene_node){ .name = "", .parent = i - 1 };
        f.actors[i] = (studio_scene_read_actor){ .prototype_name = "",
                                                 .prototype = -1 };
    }
    f.facts.actor_count = 8;
    f.facts.filter = "";
    REQUIRE(outliner_work_expect(&f.facts, "", 152, 0, 0, 7));
    for (int i = 0; i < 8; i++) {
        f.nodes[i].parent = -1;
    }
    REQUIRE(outliner_work_expect(&f.facts, "", 96, 0, 0, 0));
    return true;
}

TEST_CASE(outliner_work_invalid_facts)
{
    for (int i = 0; i < 12; i++) {
        outliner_work_fixture f;
        outliner_work_four(&f, true);
        switch (i) {
            case 0: f.facts.actor_count = -1; break;
            case 1: f.facts.actor_count = INT_MAX; break;
            case 2: f.prototype.component_first = 1; break;
            case 3: f.prototype.component_count = 4; break;
            case 4: f.nodes[0].parent = 0; break;
            case 5: f.nodes[1].parent = -2; break;
            case 6: f.actors[0].prototype = 1; break;
            case 7: f.actors[1].field_first = 1; break;
            case 8: f.actors[0].field_count = 5; break;
            case 9: f.facts.filter = NULL; break;
            case 10: f.nodes[0].name = NULL; break;
            case 11: f.actors[0].prototype_name = NULL; break;
        }
        studio_outliner_work work;
        memset(&work, 0xa5, sizeof work);
        studio_outliner_work before = work;
        REQUIRE(studio_outliner_work_measure(&f.facts, 1, 4, false, "", &work));
        REQUIRE(!memcmp(&work, &before, sizeof work));
    }
    return true;
}

TEST_CASE(outliner_work_checked_allowance)
{
    const uintptr_t largest_weight = (UINTPTR_MAX - 10000) / 8;
    const uintptr_t largest_total = UINTPTR_MAX - ((UINTPTR_MAX - 10000) % 8);
    const struct
    {
        studio_outliner_work work;
        uintptr_t weight;
        bool accepted;
        uintptr_t total;
    } cases[] = {
        { .weight = 0 },
        { .weight = UINTPTR_MAX },
        { .weight = UINTPTR_MAX / 8 },
        { .work.actors = UINTPTR_MAX, .weight = 1 },
        { .work.visible = UINTPTR_MAX, .weight = 1 },
        { .work.match_starts = UINTPTR_MAX, .weight = 1 },
        { .work.match_pairs = UINTPTR_MAX, .weight = 1 },
        { .work = { .visible = 2, .max_depth = UINTPTR_MAX }, .weight = 1 },
        { .work = { .visible = 2, .max_components = UINTPTR_MAX }, .weight = 1 },
        { .work = { .visible = 2, .max_fields = UINTPTR_MAX, .fields = 1 }, .weight = 1 },
        { .work = { .visible = 2, .attachments = UINTPTR_MAX }, .weight = 1 },
        { .work.path_bytes = UINTPTR_MAX, .weight = 1 },
        { .work.actors = UINTPTR_MAX / 3, .weight = 1 },
        { .weight = largest_weight, .accepted = true, .total = largest_total },
        { .weight = 2, .accepted = true, .total = 10016 },
        { .work = { .visible = 1, .max_components = UINTPTR_MAX / 2 + 1 }, .weight = 1 },
        { .work = { .visible = 1, .max_fields = UINTPTR_MAX, .fields = UINTPTR_MAX },
          .weight = 1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        uintptr_t total = 77;
        REQUIRE(
            studio_outliner_work_limit(&cases[i].work, cases[i].weight, &total) ==
            cases[i].accepted
        );
        REQUIRE(total == (cases[i].accepted ? cases[i].total : 77));
    }
    return true;
}

static int outliner_work_main()
{
    RUN_TEST_CASE(outliner_work_page_bounds);
    RUN_TEST_CASE(outliner_work_text_and_hierarchy);
    RUN_TEST_CASE(outliner_work_invalid_facts);
    RUN_TEST_CASE(outliner_work_checked_allowance);
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
