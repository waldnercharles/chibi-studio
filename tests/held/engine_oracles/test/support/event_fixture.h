#include "test/support/code_fixture.h"
#ifndef CHIBI_TEST_EVENT_FIXTURE_H
#define CHIBI_TEST_EVENT_FIXTURE_H
#include "authoring/preview.h"
#include "authoring/prototype_preview.h"
#include "graph/document/library.h"
#include "graph/document/serialize.h"
#include "support/text_file.h"

#include <assert.h>
#include <stdlib.h>
static inline void register_damage_event(game_preview_world *world)
{
    const char *paths[] = {
        "test/fixtures/behavior-catalog/content/events/Damage.kdl",
        "test/fixtures/behavior-catalog/content/events/Damaged.kdl",
        "test/fixtures/behavior-catalog/content/events/Died.kdl",
        "test/fixtures/behavior-catalog/content/events/RefreshEncounter.kdl",
        "test/fixtures/behavior-catalog/content/events/CollectExperience.kdl"
    };
    for (int i = 0; i < (int)(sizeof paths / sizeof *paths); i++) {
        char *json = text_file_read(paths[i]);
        assert(json);
        behavior_asset *asset = test_project_asset(paths[i], json);
        free(json);
        assert(asset);
        assert(
            game_preview_add_definition(world, game_preview_definition_new(asset->name, asset)) == 0
        );
    }
}
static inline behavior_asset *link_test_libraries(behavior_asset *asset)
{
    const char *paths[] = {
        "test/fixtures/behavior-catalog/content/libraries/DirectionMath.chibi",
        "test/fixtures/behavior-catalog/content/libraries/"
        "MovementActions.chibi",
        "test/fixtures/behavior-catalog/content/libraries/WeaponActions.chibi",
        "test/fixtures/behavior-catalog/content/shader-libraries/"
        "ShaderMath.kdl",
        "test/fixtures/behavior-catalog/content/events/Damage.kdl",
        "test/fixtures/behavior-catalog/content/events/Damaged.kdl",
        "test/fixtures/behavior-catalog/content/events/Died.kdl",
        "test/fixtures/behavior-catalog/content/events/RefreshEncounter.kdl",
        "test/fixtures/behavior-catalog/content/events/CollectExperience.kdl"
    };
    enum
    {
        count = sizeof paths / sizeof *paths
    };
    behavior_asset *catalog[count];
    for (int i = 0; i < count; i++) {
        char *json = text_file_read(paths[i]);
        assert(json);
        catalog[i] = test_project_asset(paths[i], json);
        free(json);
        assert(catalog[i]);
    }
    char error[256];
    behavior_asset *linked = behavior_asset_link_libraries(
        asset,
        (const behavior_asset *const *)catalog,
        count,
        error,
        sizeof error
    );
    assert(linked);
    behavior_asset_free(asset);
    for (int i = 0; i < count; i++) {
        behavior_asset_free(catalog[i]);
    }
    return linked;
}
static inline void register_test_preview_catalog(game_prototype_preview *preview)
{
    if (!preview) {
        return;
    }
    const char *path = "test/fixtures/behavior-catalog/content/libraries/"
                       "DirectionMath.chibi";
    char *text = text_file_read(path);
    assert(text);
    test_source_fixture fixture = test_source_prepare(path, text);
    free(text);
    test_source_register(game_prototype_preview_live(preview)->runtime, &fixture);
    test_source_free(&fixture);
}
#endif
