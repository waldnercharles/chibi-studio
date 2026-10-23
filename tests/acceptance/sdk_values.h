#include "authoring/application_catalog.h"
#include "authoring/language_catalog.h"
#include "runtime/declared_components.h"
#include "types/text.h"

/* Renderer-free ownership and checkpoint checks for the installed SDK's
 * length-carrying strings. No project loading or compiler preparation. */
static struct
{
    arena input, reads;
    declared_components *components;
    ecs_world *world;
    scene_history *history, *restored;
    native_capabilities *native[2];
    authoring_language_catalog *language[2];
    pin_value copy;
    char *checkpoint;
} sdk_values;

static void sdk_values_cleanup()
{
    for (int i = 0; i < 2; i++) {
        authoring_language_catalog_free(sdk_values.language[i]);
        native_capabilities_free(sdk_values.native[i]);
    }
    scene_property_value_free(&sdk_values.copy);
    scene_history_free(sdk_values.history);
    scene_history_free(sdk_values.restored);
    ecs_world_free(sdk_values.world);
    declared_components_free(sdk_values.components);
    arena_free(&sdk_values.input);
    arena_free(&sdk_values.reads);
    free(sdk_values.checkpoint);
    memset(&sdk_values, 0, sizeof sdk_values);
}

static bool sdk_values_equal(pin_value value, const char *bytes, size_t length)
{
    REQUIRE(value.type == PIN_STR);
    pin_string text = pin_text(value);
    REQUIRE(text.length == length && !memcmp(text.bytes, bytes, length));
    return true;
}

TEST_CASE(sdk_property_copy_keeps_string_bytes)
{
    static const char bytes[] = { 'a', 0, (char)0xff, 'b' };
    pin_value source;
    REQUIRE(!pin_value_text(&sdk_values.input, &source, bytes, sizeof bytes));
    source.type_name = "SdkText";
    REQUIRE(scene_property_value_copy(&sdk_values.copy, source));
    REQUIRE(sdk_values.copy.text != source.text);
    REQUIRE(sdk_values.copy.type_name != source.type_name);
    arena_free(&sdk_values.input);
    REQUIRE(sdk_values_equal(sdk_values.copy, bytes, sizeof bytes));
    REQUIRE(!strcmp(sdk_values.copy.type_name, "SdkText"));
    return true;
}

static bool sdk_history_round_trip(const char *bytes, size_t length)
{
    const declared_field field = { .name = "text", .type = "string" };
    const declared_component component = { .name = "SdkText",
                                           .fields = &field,
                                           .field_count = 1 };
    char error[256];
    sdk_values.components = declared_components_new(&component, 1, error, sizeof error);
    REQUIRE(sdk_values.components);
    ecs_component_catalog catalog = declared_components_ecs(sdk_values.components);
    REQUIRE(catalog.component_count == 1);
    const ecs_component_desc *desc = catalog.components[0];
    sdk_values.world = ecs_world_new();
    REQUIRE(ecs_world_register_component(sdk_values.world, desc, NULL) == ECS_OK);
    ecs_entity entity = ecs_world_create_entity(sdk_values.world);
    REQUIRE(ecs_world_record_add(sdk_values.world, entity, desc->id, NULL, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(sdk_values.world) == ECS_OK);
    pin_value before, after;
    REQUIRE(!pin_value_text(&sdk_values.input, &before, "before", 6));
    REQUIRE(!pin_value_text(&sdk_values.input, &after, bytes, length));
    REQUIRE(ecs_component_write_field(sdk_values.world, entity, desc->id, 0, after) == ECS_OK);
    sdk_values.history = scene_history_new();
    REQUIRE(scene_history_push_field(sdk_values.history, entity, desc->id, 0, before, after));
    sdk_values.checkpoint = scene_history_checkpoint(sdk_values.history);
    REQUIRE(sdk_values.checkpoint);
    scene_history_free(sdk_values.history);
    sdk_values.history = NULL;
    arena_free(&sdk_values.input);
    sdk_values.restored = scene_history_new();
    uint64_t identity = ecs_entity_to_u64(entity);
    REQUIRE(
        scene_history_restore_checkpoint(sdk_values.restored, sdk_values.checkpoint, &identity, &identity, 1)
    );
    REQUIRE(scene_history_undo(sdk_values.restored, sdk_values.world, NULL) == 1);
    pin_value actual;
    REQUIRE(
        ecs_component_read_field(&sdk_values.reads, sdk_values.world, entity, desc->id, 0, &actual) == ECS_OK
    );
    REQUIRE(sdk_values_equal(actual, "before", 6));
    REQUIRE(scene_history_redo(sdk_values.restored, sdk_values.world, NULL) == 1);
    REQUIRE(
        ecs_component_read_field(&sdk_values.reads, sdk_values.world, entity, desc->id, 0, &actual) == ECS_OK
    );
    REQUIRE(sdk_values_equal(actual, bytes, length));
    return true;
}

TEST_CASE(sdk_history_keeps_utf8_text)
{
    static const char bytes[] = "Cr\xc3\xaapes";
    REQUIRE(sdk_history_round_trip(bytes, sizeof bytes - 1));
    return true;
}

TEST_CASE(sdk_history_keeps_non_utf8_bytes)
{
    static const char bytes[] = { 'a', (char)0xff, 'b' };
    REQUIRE(sdk_history_round_trip(bytes, sizeof bytes));
    return true;
}

TEST_CASE(sdk_application_catalog_has_optional_studio_actions)
{
    char error[256];
    for (int studio = 0; studio < 2; studio++) {
        sdk_values.native[studio] = authoring_application_catalog_new(studio != 0, error, sizeof error);
        REQUIRE(sdk_values.native[studio]);
        authoring_language_catalog_input input = { .native = sdk_values.native[studio] };
        sdk_values.language[studio] = authoring_language_catalog_new(&input, error, sizeof error);
        REQUIRE(sdk_values.language[studio]);
        const script_catalog *catalog = authoring_language_catalog_view(
            sdk_values.language[studio]
        );
        int found = script_catalog_find_capability(catalog, "prototype_choices", SCRIPT_CAPABILITY_ACTION);
        REQUIRE((found >= 0) == (studio != 0));
        if (found >= 0) {
            const script_catalog_capability *capability = &catalog->capabilities[found];
            const native_capability_binding *binding = native_capabilities_find(
                sdk_values.native[studio],
                capability->id,
                script_catalog_capability_abi(catalog, capability)
            );
            REQUIRE(binding && binding->call);
        }
        printf(
            "SDK application catalog: studio=%d capabilities=%d "
            "prototype_choices=%d\n",
            studio,
            catalog->capability_count,
            found >= 0
        );
    }
    return true;
}

static int sdk_values_main()
{
    pu_setup(NULL, sdk_values_cleanup);
    RUN_TEST_CASE(sdk_property_copy_keeps_string_bytes);
    RUN_TEST_CASE(sdk_history_keeps_utf8_text);
    RUN_TEST_CASE(sdk_history_keeps_non_utf8_bytes);
    RUN_TEST_CASE(sdk_application_catalog_has_optional_studio_actions);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
