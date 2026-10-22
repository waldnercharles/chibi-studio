#include "authoring/engine_preview.h"
#include "authoring/native_prototype.h"
#include "authoring/preview.h"
#include "authoring/prototype_preview.h"
#include "editor/workspace/viewport.h"
#include "runtime/camera.h"
#include "runtime/component_catalog.h"
#include "runtime/engine.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct
{
    const char *name;
    pin_value value;
} style_edits[] = {
    { "shape", { .type = PIN_ENUM, .type_name = "GameShape", .e = GAME_SHAPE_BOX } },
    { "stroke_width", { .type = PIN_F32, .type_name = "float", .f = 2.25f } },
    { "border_width", { .type = PIN_F32, .type_name = "float", .f = 3.5f } },
    { "border_color", { .type = PIN_U32, .type_name = "unsigned int", .u = 0x11223344u } },
    { "glow_radius", { .type = PIN_F32, .type_name = "float", .f = 14 } },
    { "glow_color", { .type = PIN_U32, .type_name = "unsigned int", .u = 0xaabbccddu } },
};

static void edit_style(GameAppearance *appearance)
{
    appearance->shape = 0;
    appearance->stroke_width = 2.25f;
    appearance->border_width = 3.5f;
    appearance->border_color = 0x11223344u;
    appearance->glow_radius = 14;
    appearance->glow_color = 0xaabbccddu;
}

static void expect_edited_style(const GameAppearance *appearance)
{
    assert(appearance->shape == 0 && appearance->border_width == 3.5f);
    assert(appearance->stroke_width == 2.25f);
    assert(appearance->border_color == 0x11223344u);
    assert(appearance->glow_radius == 14 && appearance->glow_color == 0xaabbccddu);
}

static game_prototype *read_prototype(const char *name, const game_prototype_registry *registry)
{
    char path[256], error[256];
    snprintf(path, sizeof path, "test/fixtures/content/prototypes/%s.chibi", name);
    FILE *file = fopen(path, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    assert(size >= 0);
    rewind(file);
    char *text = xmalloc((size_t)size + 1);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = 0;
    fclose(file);
    game_prototype *prototype = game_prototype_load(text, registry, error, sizeof error);
    free(text);
    assert(prototype);
    while (acount(prototype->graphs)) {
        assert(
            game_prototype_remove_graph(
                prototype,
                prototype->graphs[acount(prototype->graphs) - 1]
            ) == 0
        );
    }
    return prototype;
}

static void reject_component_values(
    const game_prototype_registry *registry,
    const char *component,
    const char *field,
    int64_t value
)
{
    char error[256];
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_doc_set_root(doc, root);
    av_obj_add_str(doc, root, "name", "Invalid values");
    av_value *components = av_obj_add_obj(doc, root, "components");
    av_value *fields = av_obj_add_obj(doc, components, component);
    av_obj_add_int(doc, fields, field, value);
    char *source = asset_document_write(root);
    assert(source);
    game_prototype *invalid = game_prototype_load(source, registry, error, sizeof error);
    assert(!invalid);
    game_prototype_free(invalid);
    free(source);
    av_doc_free(doc);
}

static void expect_entity(ecs_world *world, ecs_entity entity, float width, float height, unsigned color)
{
    const GameAppearance *appearance = GAME_GameAppearance_get_const(world, entity);
    assert(appearance && appearance->width == width && appearance->height == height);
    assert(appearance->color == color);
    switch (color) {
        case 0xFFFFFFFFu:
            assert(
                appearance->shape == GAME_SHAPE_STROKED_CHEVRON &&
                appearance->border_width == 0
            );
            assert(appearance->border_color == 0 && appearance->stroke_width == 1);
            assert(appearance->glow_radius == 8 && appearance->glow_color == 0xFFFFFF50u);
            break;
        case 0x140F00FFu:
            assert(appearance->shape == 2 && appearance->border_width == 2);
            assert(appearance->border_color == 0xFFCC00FFu && appearance->stroke_width == 0);
            assert(appearance->glow_radius == 6 && appearance->glow_color == 0xFFCC0040u);
            break;
        case 0x00FFF0FFu:
            assert(appearance->shape == GAME_SHAPE_CAPSULE && appearance->border_width == 0);
            assert(appearance->stroke_width == 0 && appearance->border_color == 0);
            assert(appearance->glow_radius == 6 && appearance->glow_color == 0x00FFF060u);
            break;
        default: assert(false);
    }
}

static ecs_entity spawn(const game_prototype *prototype, ecs_world *world)
{
    char error[256];
    ecs_entity entity;
    assert(game_prototype_spawn_components(prototype, world, &entity, error, sizeof error) == 0);
    return entity;
}

static void added_component_defaults(const game_prototype_registry *registry, ecs_world *world)
{
    char error[256];
    game_prototype *appearance = game_prototype_new("AddedAppearance");
    assert(game_prototype_add_native_component(appearance, &GAME_COMPONENT_GameAppearance) == 0);
    pin_value value;
    assert(
        game_prototype_read_field(appearance, "GameAppearance", "width", &value) == 0 &&
        value.f == 32
    );
    assert(
        game_prototype_read_field(appearance, "GameAppearance", "height", &value) == 0 &&
        value.f == 32
    );
    assert(
        game_prototype_read_field(appearance, "GameAppearance", "color", &value) == 0 &&
        value.u == 0xFFFFFFFFu
    );
    assert(
        game_prototype_read_field(appearance, "GameAppearance", "shape", &value) == 0 &&
        value.e == GAME_SHAPE_BOX
    );
    assert(
        game_prototype_read_field(appearance, "GameAppearance", "antialias", &value) == 0 &&
        value.f == 1
    );
    ecs_entity entity = spawn(appearance, world);
    const GameAppearance *spawned = GAME_GameAppearance_get_const(world, entity);
    assert(
        spawned && spawned->width == 32 && spawned->height == 32 &&
        spawned->color == 0xFFFFFFFFu && spawned->shape == GAME_SHAPE_BOX &&
        spawned->antialias == 1
    );
    char *saved = game_prototype_save(appearance);
    game_prototype *loaded = game_prototype_load(saved, registry, error, sizeof error);
    free(saved);
    assert(loaded);
    assert(
        game_prototype_read_field(loaded, "GameAppearance", "width", &value) == 0 &&
        value.f == 32
    );
    game_prototype_free(loaded);
    game_prototype_free(appearance);

    game_prototype *camera = game_prototype_new("AddedCamera");
    assert(game_prototype_add_native_component(camera, &GAME_COMPONENT_GameCamera) == 0);
    assert(
        game_prototype_read_field(camera, "GameCamera", "width", &value) == 0 &&
        value.f == 960
    );
    assert(
        game_prototype_read_field(camera, "GameCamera", "height", &value) == 0 &&
        value.f == 540
    );
    assert(game_prototype_read_field(camera, "GameTransform", "x", &value) == -1);
    assert(game_prototype_add_native_component(camera, &GAME_COMPONENT_GameTransform) == 0);
    (void)spawn(camera, world);
    engine_viewport_view view;
    assert(game_camera_view(world, NULL, &view) == 1 && view.width == 960 && view.height == 540);
    game_prototype_free(camera);
}

static void authored_roundtrip(
    game_prototype *prototype,
    const game_prototype_registry *registry,
    float width,
    float height,
    unsigned color
)
{
    pin_value value;
    assert(game_prototype_read_field(prototype, "GameAppearance", "width", &value) == 0);
    assert(value.type == PIN_F32 && value.f == width);
    assert(game_prototype_read_field(prototype, "GameAppearance", "color", &value) == 0);
    assert(value.type == PIN_U32 && value.u == color);
    char error[256];
    game_prototype *edited = game_prototype_clone(prototype, error, sizeof error);
    assert(edited);
    value = (pin_value){ .type = PIN_F32, .type_name = "float", .f = 45.5f };
    assert(game_prototype_write_field(edited, "GameAppearance", "width", value) == 0);
    value = (pin_value){ .type = PIN_U32, .type_name = "unsigned int", .u = 0x11223344u };
    assert(game_prototype_write_field(edited, "GameAppearance", "color", value) == 0);
    for (int i = 0; i < (int)(sizeof style_edits / sizeof style_edits[0]); i++) {
        assert(
            game_prototype_write_field(
                edited,
                "GameAppearance",
                style_edits[i].name,
                style_edits[i].value
            ) == 0
        );
    }
    char *saved = game_prototype_save(edited);
    game_prototype *loaded = game_prototype_load(saved, registry, error, sizeof error);
    free(saved);
    assert(loaded);
    for (int i = 0; i < (int)(sizeof style_edits / sizeof style_edits[0]); i++) {
        assert(
            game_prototype_read_field(loaded, "GameAppearance", style_edits[i].name, &value) == 0
        );
        pin_value expected = style_edits[i].value;
        assert(value.type == expected.type);
        switch (expected.type) {
            case PIN_I32: assert(value.i == expected.i); break;
            case PIN_ENUM: assert(value.e == expected.e); break;
            case PIN_U32: assert(value.u == expected.u); break;
            case PIN_F32: assert(value.f == expected.f); break;
            default: assert(false);
        }
    }
    assert(
        game_prototype_read_field(loaded, "GameAppearance", "width", &value) == 0 &&
        value.f == 45.5f
    );
    assert(
        game_prototype_read_field(loaded, "GameAppearance", "height", &value) == 0 &&
        value.f == height
    );
    assert(
        game_prototype_read_field(loaded, "GameAppearance", "color", &value) == 0 &&
        value.u == 0x11223344u
    );
    assert(
        game_prototype_read_field(prototype, "GameAppearance", "color", &value) == 0 &&
        value.u == color
    );
    game_prototype_free(loaded);
    game_prototype_free(edited);
}

static void instance_overrides(
    game_prototype *prototype,
    const game_prototype_registry *registry,
    ecs_world *world,
    char ***owned_strings,
    float width,
    float height,
    unsigned color
)
{
    ecs_entity original = spawn(prototype, world);
    expect_entity(world, original, width, height, color);
    GameAppearance *appearance = GAME_GameAppearance_get(world, original);
    appearance->width = 45.5f;
    appearance->color = 0x11223344u;
    edit_style(appearance);
    av_value *overrides = game_prototype_capture_overrides_value(prototype, world, original);
    av_value *changed = av_obj_value_at(overrides, 0);
    assert(av_is_obj(changed) && !av_obj_get(changed, "height"));
    ecs_entity copied = spawn(prototype, world);
    char error[256];
    assert(
        game_prototype_apply_components_value(world, copied, overrides, registry, owned_strings, error, sizeof error) ==
        0
    );
    av_value_free(overrides);
    const GameAppearance *copy = GAME_GameAppearance_get_const(world, copied);
    assert(copy && copy->width == 45.5f && copy->height == height && copy->color == 0x11223344u);
    expect_edited_style(copy);
    ecs_entity fresh = spawn(prototype, world);
    expect_entity(world, fresh, width, height, color);
}

static void preview_reset(
    game_prototype *prototype,
    const game_prototype_registry *registry,
    float width,
    float height,
    unsigned color
)
{
    game_prototype_preview *preview = game_prototype_preview_new(registry);
    char error[256];
    assert(game_prototype_preview_launch(preview, prototype, error, sizeof error) == 0);
    ecs_world *world = game_prototype_preview_world(preview);
    ecs_entity original = game_prototype_preview_entity(preview);
    expect_entity(world, original, width, height, color);
    GAME_GameAppearance_get(world, original)->color = 0x11223344u;
    edit_style(GAME_GameAppearance_get(world, original));
    assert(game_prototype_preview_tick(preview, 0) >= 0);
    assert(GAME_GameAppearance_get_const(world, original)->color == 0x11223344u);
    expect_edited_style(GAME_GameAppearance_get_const(world, original));
    game_prototype_preview_stop(preview);
    assert(game_prototype_preview_launch(preview, prototype, error, sizeof error) == 0);
    ecs_entity fresh = game_prototype_preview_entity(preview);
    assert(ecs_entity_to_u64(fresh) != ecs_entity_to_u64(original));
    expect_entity(world, fresh, width, height, color);
    game_prototype_preview_free(preview);
}

static void legacy_appearance(const game_prototype_registry *registry, ecs_world *world)
{
    const char *json =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Legacy "
        "appearance\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"GameAppearance\", data.Object([\n"
        "                    data.Field.Named(\"width\", data.Value.Int(24)),\n"
        "                    data.Field.Named(\"height\", "
        "data.Value.Int(16)),\n"
        "                    data.Field.Named(\"color\", "
        "data.Value.Int(287454020))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n";
    char error[256];
    game_prototype *prototype = game_prototype_load(json, registry, error, sizeof error);
    assert(prototype);
    ecs_entity entity = spawn(prototype, world);
    const GameAppearance *appearance = GAME_GameAppearance_get_const(world, entity);
    assert(appearance && appearance->width == 24 && appearance->height == 16);
    assert(appearance->color == 0x11223344u);
    assert(
        appearance->shape == 0 && appearance->stroke_width == 0 &&
        appearance->border_width == 0 && appearance->border_color == 0
    );
    assert(appearance->glow_radius == 0 && appearance->glow_color == 0);
    game_prototype_free(prototype);
}

void test_appearance()
{
    game_prototype_registry registry = { GAME_REGISTRY_COMPONENTS,
                                         GAME_REGISTRY_COMPONENT_COUNT };
    ecs_world *world = ecs_world_new();
    char **owned_strings = NULL;
    for (int i = 0; i < registry.component_count; i++) {
        assert(ecs_world_register_component(world, registry.components[i], NULL) == ECS_OK);
    }
    reject_component_values(&registry, "GameAppearance", "shape", INT64_C(2147483648));
    reject_component_values(&registry, "GameAppearance", "color", INT64_C(4294967296));
    reject_component_values(&registry, "GameHealth", "current", INT64_C(2147483648));
    added_component_defaults(&registry, world);
    legacy_appearance(&registry, world);
    game_preview_world *live = game_preview_world_new(world);
    engine *runtime = authoring_engine_new(world, live->runtime);
    assert(engine_set_playing(runtime, true) == 0);
    const char *names[] = { "Player", "Enemy", "Projectile" };
    const float widths[] = { 20, 28, 18 };
    const float heights[] = { 18, 28, 6 };
    const unsigned colors[] = { 0xFFFFFFFFu, 0x140F00FFu, 0x00FFF0FFu };
    for (int i = 0; i < 3; i++) {
        game_prototype *prototype = read_prototype(names[i], &registry);
        authored_roundtrip(prototype, &registry, widths[i], heights[i], colors[i]);
        instance_overrides(prototype, &registry, world, &owned_strings, widths[i], heights[i], colors[i]);
        assert(engine_frame(runtime, 0) >= 0);
        preview_reset(prototype, &registry, widths[i], heights[i], colors[i]);
        game_prototype_free(prototype);
    }
    engine_free(runtime);
    game_preview_world_free(live);
    ecs_world_free(world);
    game_prototype_owned_strings_free(owned_strings);
    puts("appearance PASS: distinct authored styles, sparse instance overrides, persistence and preview reset");
}
