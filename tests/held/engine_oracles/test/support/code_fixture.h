#ifndef CHIBI_TEST_CODE_FIXTURE_H
#define CHIBI_TEST_CODE_FIXTURE_H

#include "authoring/application_catalog.h"
#include "authoring/language_catalog.h"
#include "authoring/preview_runtime.h"
#include "authoring/source_collection.h"
#include "authoring/source_presentation.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    authoring_language_catalog *native;
    authoring_sources *sources;
    const authoring_source *source;
} test_source_fixture;
static inline void test_source_free(test_source_fixture *fixture)
{
    authoring_sources_free(fixture->sources);
    authoring_language_catalog_free(fixture->native);
    *fixture = (test_source_fixture){ 0 };
}
/* One owned canonical declaration/source generation; text is an exact overlay.
 * Execution consumes source.module. Presentation graphs never authorize execution. */
static inline test_source_fixture test_source_prepare(const char *path, const char *text)
{
    test_source_fixture fixture = { 0 };
    const char *content = strstr(path, "/content/");
    size_t length = content ? (size_t)(content - path) + strlen("/content")
                            : strlen("content");
    assert(content || !strncmp(path, "content/", strlen("content/")));
    char *root = xmalloc(length + 1);
    memcpy(root, path, length);
    root[length] = 0;
    char error[256] = { 0 };
    native_capabilities *table = authoring_application_catalog_new(false, error, sizeof error);
    assert(table);
    const authoring_language_catalog_input input = { .native = table };
    fixture.native = authoring_language_catalog_new(&input, error, sizeof error);
    native_capabilities_free(table);
    assert(fixture.native);
    const source_catalog_edit edit = { .path = path, .text = text };
    source_catalog *declarations = source_catalog_edited(
        root,
        authoring_language_catalog_view(fixture.native),
        &edit,
        1
    );
    free(root);
    script_lower_bindings bindings = authoring_language_bindings(fixture.native);
    fixture.sources = authoring_sources_prepare_catalog(declarations, &bindings, behavior_default_backend());
    for (int i = 0; i < authoring_sources_count(fixture.sources); i++) {
        const authoring_source *source = authoring_sources_at(fixture.sources, i);
        if (!strcmp(source->path, path)) {
            fixture.source = source;
            break;
        }
    }
    if (!fixture.source || !fixture.source->valid) {
        fprintf(stderr, "%s: canonical source preparation failed\n", path);
        for (int i = 0; fixture.source && i < fixture.source->diagnostic_count; i++) {
            fprintf(stderr, "%s\n", fixture.source->diagnostics[i].message);
        }
    }
    assert(
        fixture.source && fixture.source->valid &&
        authoring_sources_valid(fixture.sources)
    );
    return fixture;
}
static inline void test_source_register(game_preview_runtime *world, const test_source_fixture *fixture)
{
    char error[256];
    const authoring_source *source = fixture->source;
    game_source_publication entry = {
        .name = source->analysis->name,
        .previous_name = game_source_registered(world->sources, source->analysis->name)
                           ? source->analysis->name
                           : NULL,
        .module = source->module,
    };
    assert(!game_source_publish(world->sources, &entry, 1, NULL, error, sizeof error));
}
/* Owned authoring view for structural assertions only. Executable fixtures
 * must register the canonical module via test_source_register instead. */
static inline behavior_asset *test_project_asset(const char *path, const char *text)
{
    if (!strstr(path, ".chibi")) {
        return behavior_asset_load(text);
    }
    test_source_fixture fixture = test_source_prepare(path, text);
    authoring_source_presentation *presentation = authoring_source_presentation_new(
        fixture.source->analysis,
        authoring_sources_catalog(fixture.sources),
        NULL
    );
    assert(presentation);
    behavior_asset *view = authoring_source_presentation_take_view(presentation);
    test_source_free(&fixture);
    return view;
}
#endif
