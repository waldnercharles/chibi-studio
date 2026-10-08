/* Whole hosted documents retire their conditional and list binding views. */
#include "editor/ui/kit.h"
#include "third_party/pico_headers/pico_unit.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define REPLACEMENTS 16

static struct
{
    rml_backend *backend;
    rml_context *context;
    kit *k;
    yyjson_mut_doc *json;
    yyjson_mut_val *label, *shown, *rows;
    rml_data_model *model;
    rml_element_ref *documents[REPLACEMENTS];
    int base_documents;
} fixture;

static void setup()
{
    memset(&fixture, 0, sizeof fixture);
    fixture.backend = rml_backend_create(640, 480, 1);
    fixture.context = rml_backend_context(fixture.backend);
    fixture.k = kit_create(fixture.context, "studio.rml");
    kit_theme(fixture.k, "body, div, span { display: block; }");
    fixture.json = yyjson_mut_doc_new(NULL);
    assert(fixture.json);
    yyjson_mut_val *root = yyjson_mut_obj(fixture.json);
    yyjson_mut_doc_set_root(fixture.json, root);
    fixture.label = yyjson_mut_str(fixture.json, "Initial");
    fixture.shown = yyjson_mut_bool(fixture.json, false);
    fixture.rows = yyjson_mut_arr(fixture.json);
    yyjson_mut_obj_add_val(fixture.json, root, "label", fixture.label);
    yyjson_mut_obj_add_val(fixture.json, root, "shown", fixture.shown);
    yyjson_mut_obj_add_val(fixture.json, root, "rows", fixture.rows);
    fixture.model = rml_data_model_create(
        fixture.context,
        "iframe_bindings",
        fixture.json,
        NULL,
        NULL
    );
    assert(fixture.model);
    fixture.base_documents = rml_context_document_count(fixture.context);
}

static bool teardown()
{
    if (fixture.k) {
        kit_destroy(fixture.k);
    }
    rml_context_update(fixture.context);
    bool released = rml_context_document_count(fixture.context) == 0;
    for (int i = 0; i < REPLACEMENTS; i++) {
        if (fixture.documents[i]) {
            released = released && !rml_element_ref_get(fixture.documents[i]);
            rml_element_ref_destroy(fixture.documents[i]);
        }
    }
    rml_data_model_dirty_all(fixture.model);
    rml_data_reads before = rml_data_model_reads(fixture.model);
    rml_context_update(fixture.context);
    rml_data_reads after = rml_data_model_reads(fixture.model);
    released = released && before.values == after.values &&
               before.lookups == after.lookups;
    rml_data_model_destroy(fixture.model);
    yyjson_mut_doc_free(fixture.json);
    rml_backend_destroy(fixture.backend);
    return released;
}

static bool title_is(rml_element *element, const char *expected)
{
    if (!element) {
        return false;
    }
    char title[64];
    rml_element_attribute(element, "title", title, sizeof title);
    return !strcmp(title, expected);
}

static void begin_frame(int frame)
{
    kit_frame_input input = {
        .width = 640,
        .height = 480,
        .dp_ratio = 1,
        .time = frame / 60.0,
    };
    kit_begin(fixture.k, &input);
    kit_root(fixture.k, "app");
}

TEST_CASE(replaces_bound_documents)
{
    rml_data_reads topology_reads[2] = { 0 };
    for (int i = 0; i < REPLACEMENTS; i++) {
        bool shown = i & 1;
        int rows = shown ? 2 : 1;
        /* All JSON strings outlive the update; changing them cannot leave a
         * retired document's view pointing at a stack buffer. */
        const char *label = shown ? "Odd" : "Even";
        yyjson_mut_set_str(fixture.label, label);
        yyjson_mut_set_bool(fixture.shown, shown);
        yyjson_mut_arr_clear(fixture.rows);
        for (int row = 0; row < rows; row++) {
            yyjson_mut_val *value = yyjson_mut_arr_add_obj(fixture.json, fixture.rows);
            yyjson_mut_obj_add_str(fixture.json, value, "name", row ? "Second" : label);
        }
        rml_data_model_dirty_all(fixture.model);

        begin_frame(i);
        char key[32];
        snprintf(key, sizeof key, "bound-%d", i);
        kit_document_begin(fixture.k, key, NULL, shown);
        rml_element *host = kit_host(fixture.k, "content", NULL);
        rml_element_set_data_model(host, "iframe_bindings");
        rml_element_set_inner_rml(
            host,
            "<div class='value' data-attr-title='label'/>"
            "<div data-when='shown'><span class='conditional' "
            "data-attr-title='label'/></div>"
            "<div class='row' data-for='row : rows' "
            "data-attr-title='row.name'><span/></div>"
        );
        rml_element *document = rml_element_owner_document(host);
        fixture.documents[i] = rml_element_ref_create(document);
        kit_document_end(fixture.k);
        kit_end(fixture.k);
        rml_backend_update(fixture.backend, 1.0 / 60.0);

        REQUIRE(rml_context_document_count(fixture.context) == fixture.base_documents + 1);
        REQUIRE(rml_element_ref_get(fixture.documents[i]) == document);
        for (int retired = 0; retired < i; retired++) {
            REQUIRE(!rml_element_ref_get(fixture.documents[retired]));
        }
        REQUIRE(title_is(rml_element_query_selector(document, ".value"), label));
        rml_element *conditional = rml_element_query_selector(document, ".conditional");
        REQUIRE(shown ? title_is(conditional, label) : !conditional);
        rml_element *items[3];
        /* The data-for source survives as one hidden template. */
        REQUIRE(rml_element_query_selector_all(document, ".row", items, 3) == rows + 1);
        int first = 0, second = 0;
        for (int row = 0; row < rows + 1; row++) {
            first += title_is(items[row], label);
            second += title_is(items[row], "Second");
        }
        REQUIRE(first == 1);
        REQUIRE(second == rows - 1);

        /* Count an already-mounted topology, excluding initial binding setup.
         * Replacing more documents must not accumulate evaluated views. */
        rml_data_model_dirty_all(fixture.model);
        rml_data_reads before = rml_data_model_reads(fixture.model);
        rml_backend_update(fixture.backend, 1.0 / 60.0);
        rml_data_reads after = rml_data_model_reads(fixture.model);
        rml_data_reads reads = { .values = after.values - before.values,
                                 .lookups = after.lookups - before.lookups };
        REQUIRE(reads.values > 0);
        if (i < 2) {
            topology_reads[shown] = reads;
        } else {
            REQUIRE(reads.values == topology_reads[shown].values);
            REQUIRE(reads.lookups == topology_reads[shown].lookups);
        }

        yyjson_mut_set_str(fixture.label, "Changed");
        rml_data_model_dirty(fixture.model, "label");
        before = rml_data_model_reads(fixture.model);
        rml_backend_update(fixture.backend, 1.0 / 60.0);
        after = rml_data_model_reads(fixture.model);
        /* One direct label plus the optional structural conditional. No row
         * view depends on label, and no retired document may evaluate it. */
        REQUIRE(after.values - before.values == (uint64_t)(1 + shown));
        REQUIRE(title_is(rml_element_query_selector(document, ".value"), "Changed"));
        REQUIRE(
            !shown || title_is(rml_element_query_selector(document, ".conditional"), "Changed")
        );
    }

    begin_frame(REPLACEMENTS);
    kit_end(fixture.k);
    rml_backend_update(fixture.backend, 1.0 / 60.0);
    REQUIRE(rml_context_document_count(fixture.context) == fixture.base_documents);
    REQUIRE(!rml_element_ref_get(fixture.documents[REPLACEMENTS - 1]));
    rml_data_model_dirty_all(fixture.model);
    rml_data_reads before = rml_data_model_reads(fixture.model);
    rml_backend_update(fixture.backend, 1.0 / 60.0);
    rml_data_reads after = rml_data_model_reads(fixture.model);
    REQUIRE(after.values == before.values);
    REQUIRE(after.lookups == before.lookups);
    rml_backend_stats stats = rml_backend_get_stats(fixture.backend);
    REQUIRE(stats.render_captures == 0);
    REQUIRE(stats.render_passes == 0);
    return true;
}

bool iframe_bound_documents()
{
    int warnings = rml_backend_warning_count();
    setup();
    bool passed = replaces_bound_documents();
    bool released = teardown();
    bool cleanup = pu_require(
        released,
        "all hosted binding documents and views released",
        __FILE__,
        __LINE__,
        NULL
    );
    bool quiet = pu_require(
        rml_backend_warning_count() == warnings,
        "bound document replacement emits no RmlUi warnings",
        __FILE__,
        __LINE__,
        NULL
    );
    return passed && cleanup && quiet;
}
