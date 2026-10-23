#include "authoring/application_catalog.h"
#include "authoring/language_catalog.h"
#include "editor/collab/actions_internal.h"
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
    editor *graph_editor;
    char *action;
} sdk_values;

static void sdk_values_cleanup()
{
    for (int i = 0; i < 2; i++) {
        authoring_language_catalog_free(sdk_values.language[i]);
        native_capabilities_free(sdk_values.native[i]);
    }
    scene_property_value_free(&sdk_values.copy);
    editor_free(sdk_values.graph_editor);
    free(sdk_values.action);
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

TEST_CASE(sdk_history_keeps_descriptor_nul_bytes)
{
    static const char bytes[] = { 'a', 0, (char)0xff, 'b' };
    static const type_field field = { .name = "text",
                                      .type = PIN_STR,
                                      .size = sizeof(const pin_string *),
                                      .text_descriptor = true };
    static const type_desc type = { .name = "SdkDescriptor",
                                    .size = sizeof(const pin_string *),
                                    .fields = &field,
                                    .field_count = 1 };
    ecs_component_desc desc = { .id = UINT64_C(0x9e7a8c83212e1400),
                                .key = "SdkDescriptor",
                                .name = "SdkDescriptor",
                                .type = &type,
                                .element_size = type.size,
                                .layout_hash = type_layout_hash(&type),
                                .index = ECS_INDEX_DENSE };
    sdk_values.world = ecs_world_new();
    REQUIRE(ecs_world_register_component(sdk_values.world, &desc, NULL) == ECS_OK);
    ecs_entity entity = ecs_world_create_entity(sdk_values.world);
    REQUIRE(ecs_world_record_add(sdk_values.world, entity, desc.id, NULL, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(sdk_values.world) == ECS_OK);
    pin_value before, after;
    REQUIRE(!pin_value_text(&sdk_values.input, &before, "before", 6));
    REQUIRE(!pin_value_text(&sdk_values.input, &after, bytes, sizeof bytes));
    REQUIRE(ecs_component_write_field(sdk_values.world, entity, desc.id, 0, after) == ECS_OK);
    sdk_values.history = scene_history_new();
    REQUIRE(scene_history_push_field(sdk_values.history, entity, desc.id, 0, before, after));
    arena_free(&sdk_values.input);
    REQUIRE(scene_history_undo(sdk_values.history, sdk_values.world, NULL) == 1);
    pin_value actual;
    REQUIRE(
        ecs_component_read_field(&sdk_values.reads, sdk_values.world, entity, desc.id, 0, &actual) == ECS_OK
    );
    REQUIRE(sdk_values_equal(actual, "before", 6));
    REQUIRE(scene_history_redo(sdk_values.history, sdk_values.world, NULL) == 1);
    REQUIRE(
        ecs_component_read_field(&sdk_values.reads, sdk_values.world, entity, desc.id, 0, &actual) == ECS_OK
    );
    REQUIRE(sdk_values_equal(actual, bytes, sizeof bytes));
    return true;
}

static bool sdk_native_history(ecs_entity *entity, bool invalid_after)
{
    sdk_values.world = ecs_world_new();
    REQUIRE(ecs_world_register_component(sdk_values.world, &COMPONENT_Transform, NULL) == ECS_OK);
    REQUIRE(
        ecs_world_register_component(sdk_values.world, &COMPONENT_GlobalBinding, NULL) == ECS_OK
    );
    *entity = ecs_world_create_entity(sdk_values.world);
    Transform transform = { 20, 30 };
    GlobalBinding binding = { "after" };
    REQUIRE(
        ecs_world_record_add(sdk_values.world, *entity, COMPONENT_Transform.id, &transform, sizeof transform) ==
        ECS_OK
    );
    REQUIRE(
        ecs_world_record_add(sdk_values.world, *entity, COMPONENT_GlobalBinding.id, &binding, sizeof binding) ==
        ECS_OK
    );
    REQUIRE(ecs_world_apply_commands(sdk_values.world) == ECS_OK);
    REQUIRE(!COMPONENT_GlobalBinding.type->fields[0].text_descriptor);
    pin_value bad, valid;
    static const char bytes[] = { 'A', 0, 'B' };
    REQUIRE(!pin_value_text(&sdk_values.input, &bad, bytes, sizeof bytes));
    REQUIRE(!pin_value_text(&sdk_values.input, &valid, "after", 5));
    scene_field_edit fields[] = {
        { COMPONENT_Transform.id, 0, { .type = PIN_F32, .f = 10 }, { .type = PIN_F32, .f = 20 } },
        { COMPONENT_GlobalBinding.id, 0, invalid_after ? valid : bad, invalid_after ? bad : valid },
    };
    sdk_values.history = scene_history_new();
    REQUIRE(scene_history_push_fields(sdk_values.history, *entity, fields, 2));
    return true;
}

static bool sdk_native_checkpoint_rejects_nul(bool invalid_after)
{
    ecs_entity entity;
    REQUIRE(sdk_native_history(&entity, invalid_after));
    sdk_values.checkpoint = scene_history_checkpoint(sdk_values.history);
    REQUIRE(sdk_values.checkpoint);
    sdk_values.restored = scene_history_new();
    REQUIRE(scene_history_push_field(
        sdk_values.restored,
        entity,
        COMPONENT_Transform.id,
        0,
        (pin_value){ .type = PIN_F32, .f = 15 },
        (pin_value){ .type = PIN_F32, .f = 20 }
    ));
    uint64_t identity = ecs_entity_to_u64(entity);
    REQUIRE(
        !scene_history_restore_checkpoint(sdk_values.restored, sdk_values.checkpoint, &identity, &identity, 1)
    );
    REQUIRE(scene_history_can_undo(sdk_values.restored));
    REQUIRE(!scene_history_can_redo(sdk_values.restored));
    REQUIRE(Transform_get_const(sdk_values.world, entity)->x == 20);
    REQUIRE(!strcmp(GlobalBinding_get_const(sdk_values.world, entity)->name, "after"));
    REQUIRE(scene_history_undo(sdk_values.restored, sdk_values.world, NULL) == 1);
    REQUIRE(Transform_get_const(sdk_values.world, entity)->x == 15);
    return true;
}

TEST_CASE(sdk_checkpoint_rejects_native_nul_before)
{
    REQUIRE(sdk_native_checkpoint_rejects_nul(false));
    return true;
}

TEST_CASE(sdk_checkpoint_rejects_native_nul_after)
{
    REQUIRE(sdk_native_checkpoint_rejects_nul(true));
    return true;
}

TEST_CASE(sdk_history_preflights_the_complete_field_batch)
{
    ecs_entity entity;
    REQUIRE(sdk_native_history(&entity, false));
    ecs_entity output = { 0 };
    int result = scene_history_undo(sdk_values.history, sdk_values.world, &output);
    REQUIRE(Transform_get_const(sdk_values.world, entity)->x == 20);
    REQUIRE(result == -1);
    REQUIRE(!output.slot);
    REQUIRE(!strcmp(GlobalBinding_get_const(sdk_values.world, entity)->name, "after"));
    REQUIRE(scene_history_can_undo(sdk_values.history));
    REQUIRE(!scene_history_can_redo(sdk_values.history));
    return true;
}

static bool sdk_graph_string_round_trip(const char *bytes, size_t length)
{
    behavior_asset *asset = behavior_asset_new("SdkValues");
    sdk_values.graph_editor = editor_new(asset);
    node *target = graph_add_node(asset->graph, &NODE_PrintString);
    REQUIRE(target && target->input_count == 2 && target->input_pins[1].type == PIN_STR);
    pin_value value;
    REQUIRE(!pin_value_text(&sdk_values.input, &value, bytes, length));
    sdk_values.action = graph_collab_action_for_command(
        sdk_values.graph_editor,
        (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                          .data.set_pin_default = { target->id, 0, 1, &value, false } }
    );
    REQUIRE(sdk_values.action);
    arena_free(&sdk_values.input);
    char error[256];
    REQUIRE(
        graph_collab_apply_action(sdk_values.graph_editor, sdk_values.action, error, sizeof error)
    );
    free(sdk_values.action);
    sdk_values.action = NULL;
    REQUIRE(sdk_values_equal(target->input_pins[1].default_value, bytes, length));
    REQUIRE(!strcmp(
        target->input_pins[1].default_value.type_name,
        target->input_pins[1].type_name
    ));
    return true;
}

TEST_CASE(sdk_graph_action_keeps_utf8_text)
{
    static const char bytes[] = "Cr\xc3\xaapes";
    REQUIRE(sdk_graph_string_round_trip(bytes, sizeof bytes - 1));
    return true;
}

TEST_CASE(sdk_graph_action_keeps_nul_bytes)
{
    static const char bytes[] = { 'A', 0, 'B' };
    REQUIRE(sdk_graph_string_round_trip(bytes, sizeof bytes));
    return true;
}

TEST_CASE(sdk_graph_action_keeps_non_utf8_bytes)
{
    static const char bytes[] = { 'A', (char)0xff, 'B' };
    REQUIRE(sdk_graph_string_round_trip(bytes, sizeof bytes));
    return true;
}

TEST_CASE(sdk_graph_action_refuses_codec_rejection)
{
    behavior_asset *asset = behavior_asset_new("SdkValues");
    sdk_values.graph_editor = editor_new(asset);
    node *target = graph_add_node(asset->graph, &NODE_PrintString);
    REQUIRE(target && target->input_count == 2 && target->input_pins[1].type == PIN_STR);
    /* The shared document codec refuses a byte envelope beyond its value
     * budget. That must refuse the action, rather than omit its payload. */
    const size_t length = 1000000;
    char *bytes = arena_alloc(&sdk_values.input, length);
    memset(bytes, 0xff, length);
    pin_value value;
    REQUIRE(!pin_value_text(&sdk_values.input, &value, bytes, length));
    sdk_values.action = graph_collab_action_for_command(
        sdk_values.graph_editor,
        (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                          .data.set_pin_default = { target->id, 0, 1, &value, false } }
    );
    REQUIRE(!sdk_values.action);
    REQUIRE(pin_text(target->input_pins[1].default_value).length == 0);
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
    RUN_TEST_CASE(sdk_history_keeps_descriptor_nul_bytes);
    RUN_TEST_CASE(sdk_checkpoint_rejects_native_nul_before);
    RUN_TEST_CASE(sdk_checkpoint_rejects_native_nul_after);
    RUN_TEST_CASE(sdk_history_preflights_the_complete_field_batch);
    RUN_TEST_CASE(sdk_graph_action_keeps_utf8_text);
    RUN_TEST_CASE(sdk_graph_action_keeps_nul_bytes);
    RUN_TEST_CASE(sdk_graph_action_keeps_non_utf8_bytes);
    RUN_TEST_CASE(sdk_graph_action_refuses_codec_rejection);
    RUN_TEST_CASE(sdk_application_catalog_has_optional_studio_actions);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
