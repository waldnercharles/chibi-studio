#include "runtime/component_catalog.h"
#include "runtime/declared_components.h"

/* Independent native worlds exercise editor history without scene actors,
 * authored compilation, project files or rendering. */
static struct
{
    arena input, reads;
    declared_components *components;
    ecs_world *world;
    scene_history *history, *restored;
    char *checkpoint, *previous, *current;
} compiled_history;

static void compiled_history_cleanup()
{
    scene_history_free(compiled_history.history);
    scene_history_free(compiled_history.restored);
    ecs_world_free(compiled_history.world);
    declared_components_free(compiled_history.components);
    arena_free(&compiled_history.input);
    arena_free(&compiled_history.reads);
    free(compiled_history.checkpoint);
    free(compiled_history.previous);
    free(compiled_history.current);
    memset(&compiled_history, 0, sizeof compiled_history);
}

static bool compiled_history_equal(pin_value value, const char *bytes, size_t length)
{
    REQUIRE(value.type == PIN_STR);
    pin_string text = pin_text(value);
    REQUIRE(text.length == length && !memcmp(text.bytes, bytes, length));
    return true;
}

static bool compiled_history_round_trip(const char *bytes, size_t length)
{
    const declared_field field = { .name = "text", .type = "string" };
    const declared_component component = { .name = "HistoryText",
                                           .fields = &field,
                                           .field_count = 1 };
    char error[256];
    compiled_history.components = declared_components_new(&component, 1, error, sizeof error);
    REQUIRE(compiled_history.components);
    ecs_component_catalog catalog = declared_components_ecs(compiled_history.components);
    REQUIRE(catalog.component_count == 1);
    const ecs_component_desc *desc = catalog.components[0];
    compiled_history.world = ecs_world_new();
    REQUIRE(ecs_world_register_component(compiled_history.world, desc, NULL) == ECS_OK);
    ecs_entity entity = ecs_world_create_entity(compiled_history.world);
    REQUIRE(ecs_world_record_add(compiled_history.world, entity, desc->id, NULL, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(compiled_history.world) == ECS_OK);
    pin_value before, after;
    REQUIRE(!pin_value_text(&compiled_history.input, &before, "before", 6));
    REQUIRE(!pin_value_text(&compiled_history.input, &after, bytes, length));
    REQUIRE(
        ecs_component_write_field(compiled_history.world, entity, desc->id, 0, after) == ECS_OK
    );
    compiled_history.history = scene_history_new();
    REQUIRE(
        scene_history_push_field(compiled_history.history, entity, desc->id, 0, before, after)
    );
    compiled_history.checkpoint = scene_history_checkpoint(compiled_history.history);
    REQUIRE(compiled_history.checkpoint);
    scene_history_free(compiled_history.history);
    compiled_history.history = NULL;
    arena_free(&compiled_history.input);
    compiled_history.restored = scene_history_new();
    uint64_t identity = ecs_entity_to_u64(entity);
    REQUIRE(scene_history_restore_checkpoint(
        compiled_history.restored,
        compiled_history.checkpoint,
        &identity,
        &identity,
        1
    ));
    free(compiled_history.checkpoint);
    compiled_history.checkpoint = NULL;
    REQUIRE(scene_history_undo(compiled_history.restored, compiled_history.world, NULL) == 1);
    pin_value actual;
    REQUIRE(
        ecs_component_read_field(&compiled_history.reads, compiled_history.world, entity, desc->id, 0, &actual) ==
        ECS_OK
    );
    REQUIRE(compiled_history_equal(actual, "before", 6));
    REQUIRE(scene_history_redo(compiled_history.restored, compiled_history.world, NULL) == 1);
    REQUIRE(
        ecs_component_read_field(&compiled_history.reads, compiled_history.world, entity, desc->id, 0, &actual) ==
        ECS_OK
    );
    REQUIRE(compiled_history_equal(actual, bytes, length));
    return true;
}

static bool compiled_history_keeps_utf8_text()
{
    static const char bytes[] = "Cr\xc3\xaapes";
    REQUIRE(compiled_history_round_trip(bytes, sizeof bytes - 1));
    return true;
}

static bool compiled_history_keeps_non_utf8_bytes()
{
    static const char bytes[] = { 'a', (char)0xff, 'b' };
    REQUIRE(compiled_history_round_trip(bytes, sizeof bytes));
    return true;
}

static bool compiled_history_keeps_descriptor_nul_bytes()
{
    static const char bytes[] = { 'a', 0, (char)0xff, 'b' };
    static const type_field field = { .name = "text",
                                      .type = PIN_STR,
                                      .size = sizeof(const pin_string *),
                                      .text_descriptor = true };
    static const type_desc type = { .name = "HistoryDescriptor",
                                    .size = sizeof(const pin_string *),
                                    .fields = &field,
                                    .field_count = 1 };
    static const ecs_component_desc desc = { .id = UINT64_C(0x9e7a8c83212e1400),
                                             .key = "HistoryDescriptor",
                                             .name = "HistoryDescriptor",
                                             .type = &type,
                                             .element_size = sizeof(const pin_string *),
                                             .index = ECS_INDEX_DENSE };
    compiled_history.world = ecs_world_new();
    REQUIRE(ecs_world_register_component(compiled_history.world, &desc, NULL) == ECS_OK);
    ecs_entity entity = ecs_world_create_entity(compiled_history.world);
    REQUIRE(ecs_world_record_add(compiled_history.world, entity, desc.id, NULL, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(compiled_history.world) == ECS_OK);
    pin_value before, after;
    REQUIRE(!pin_value_text(&compiled_history.input, &before, "before", 6));
    REQUIRE(!pin_value_text(&compiled_history.input, &after, bytes, sizeof bytes));
    REQUIRE(ecs_component_write_field(compiled_history.world, entity, desc.id, 0, after) == ECS_OK);
    compiled_history.history = scene_history_new();
    REQUIRE(scene_history_push_field(compiled_history.history, entity, desc.id, 0, before, after));
    arena_free(&compiled_history.input);
    REQUIRE(scene_history_undo(compiled_history.history, compiled_history.world, NULL) == 1);
    pin_value actual;
    REQUIRE(
        ecs_component_read_field(&compiled_history.reads, compiled_history.world, entity, desc.id, 0, &actual) ==
        ECS_OK
    );
    REQUIRE(compiled_history_equal(actual, "before", 6));
    REQUIRE(scene_history_redo(compiled_history.history, compiled_history.world, NULL) == 1);
    REQUIRE(
        ecs_component_read_field(&compiled_history.reads, compiled_history.world, entity, desc.id, 0, &actual) ==
        ECS_OK
    );
    REQUIRE(compiled_history_equal(actual, bytes, sizeof bytes));
    return true;
}

static bool compiled_native_history(ecs_entity *entity, bool invalid_after)
{
    compiled_history.world = ecs_world_new();
    REQUIRE(
        ecs_world_register_component(compiled_history.world, &COMPONENT_Transform, NULL) == ECS_OK
    );
    REQUIRE(
        ecs_world_register_component(compiled_history.world, &COMPONENT_GlobalBinding, NULL) == ECS_OK
    );
    *entity = ecs_world_create_entity(compiled_history.world);
    Transform transform = { 20, 30 };
    GlobalBinding binding = { "after" };
    REQUIRE(
        ecs_world_record_add(
            compiled_history.world,
            *entity,
            COMPONENT_Transform.id,
            &transform,
            sizeof transform
        ) == ECS_OK
    );
    REQUIRE(
        ecs_world_record_add(
            compiled_history.world,
            *entity,
            COMPONENT_GlobalBinding.id,
            &binding,
            sizeof binding
        ) == ECS_OK
    );
    REQUIRE(ecs_world_apply_commands(compiled_history.world) == ECS_OK);
    REQUIRE(!COMPONENT_GlobalBinding.type->fields[0].text_descriptor);
    pin_value bad, valid;
    static const char bytes[] = { 'A', 0, 'B' };
    REQUIRE(!pin_value_text(&compiled_history.input, &bad, bytes, sizeof bytes));
    REQUIRE(!pin_value_text(&compiled_history.input, &valid, "after", 5));
    scene_field_edit fields[] = {
        { COMPONENT_Transform.id, 0, { .type = PIN_F32, .f = 10 }, { .type = PIN_F32, .f = 20 } },
        { COMPONENT_GlobalBinding.id, 0, invalid_after ? valid : bad, invalid_after ? bad : valid },
    };
    compiled_history.history = scene_history_new();
    REQUIRE(scene_history_push_fields(compiled_history.history, *entity, fields, 2));
    return true;
}

static bool compiled_native_checkpoint_rejects_nul(bool invalid_after)
{
    ecs_entity entity;
    REQUIRE(compiled_native_history(&entity, invalid_after));
    compiled_history.checkpoint = scene_history_checkpoint(compiled_history.history);
    REQUIRE(compiled_history.checkpoint);
    scene_history_free(compiled_history.history);
    compiled_history.history = NULL;
    arena_free(&compiled_history.input);
    compiled_history.restored = scene_history_new();
    REQUIRE(scene_history_push_field(
        compiled_history.restored,
        entity,
        COMPONENT_Transform.id,
        0,
        (pin_value){ .type = PIN_F32, .f = 15 },
        (pin_value){ .type = PIN_F32, .f = 20 }
    ));
    compiled_history.previous = scene_history_checkpoint(compiled_history.restored);
    REQUIRE(compiled_history.previous);
    uint64_t identity = ecs_entity_to_u64(entity);
    REQUIRE(!scene_history_restore_checkpoint(
        compiled_history.restored,
        compiled_history.checkpoint,
        &identity,
        &identity,
        1
    ));
    compiled_history.current = scene_history_checkpoint(compiled_history.restored);
    REQUIRE(
        compiled_history.current &&
        !strcmp(compiled_history.current, compiled_history.previous)
    );
    REQUIRE(scene_history_can_undo(compiled_history.restored));
    REQUIRE(!scene_history_can_redo(compiled_history.restored));
    REQUIRE(Transform_get_const(compiled_history.world, entity)->x == 20);
    REQUIRE(!strcmp(GlobalBinding_get_const(compiled_history.world, entity)->name, "after"));
    REQUIRE(scene_history_undo(compiled_history.restored, compiled_history.world, NULL) == 1);
    REQUIRE(Transform_get_const(compiled_history.world, entity)->x == 15);
    return true;
}

static bool compiled_checkpoint_rejects_native_nul_before()
{
    REQUIRE(compiled_native_checkpoint_rejects_nul(false));
    return true;
}

static bool compiled_checkpoint_rejects_native_nul_after()
{
    REQUIRE(compiled_native_checkpoint_rejects_nul(true));
    return true;
}

static bool compiled_history_preflights_the_complete_field_batch()
{
    ecs_entity entity;
    REQUIRE(compiled_native_history(&entity, false));
    compiled_history.previous = scene_history_checkpoint(compiled_history.history);
    REQUIRE(compiled_history.previous);
    ecs_entity output = { .slot = UINT32_MAX, .generation = UINT32_MAX };
    int result = scene_history_undo(compiled_history.history, compiled_history.world, &output);
    REQUIRE(Transform_get_const(compiled_history.world, entity)->x == 20);
    REQUIRE(result == -1);
    REQUIRE(output.slot == UINT32_MAX && output.generation == UINT32_MAX);
    compiled_history.current = scene_history_checkpoint(compiled_history.history);
    REQUIRE(
        compiled_history.current &&
        !strcmp(compiled_history.current, compiled_history.previous)
    );
    REQUIRE(!strcmp(GlobalBinding_get_const(compiled_history.world, entity)->name, "after"));
    REQUIRE(scene_history_can_undo(compiled_history.history));
    REQUIRE(!scene_history_can_redo(compiled_history.history));
    return true;
}

static bool compiled_model_history()
{
    REQUIRE(compiled_history_keeps_utf8_text());
    compiled_history_cleanup();
    REQUIRE(compiled_history_keeps_non_utf8_bytes());
    compiled_history_cleanup();
    REQUIRE(compiled_history_keeps_descriptor_nul_bytes());
    compiled_history_cleanup();
    REQUIRE(compiled_checkpoint_rejects_native_nul_before());
    compiled_history_cleanup();
    REQUIRE(compiled_checkpoint_rejects_native_nul_after());
    compiled_history_cleanup();
    REQUIRE(compiled_history_preflights_the_complete_field_batch());
    compiled_history_cleanup();
    return true;
}
