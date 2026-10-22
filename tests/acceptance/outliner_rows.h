/* Freeze the native row producer before a live authored-policy cutover. This
 * runs in the existing acceptance host; it does not install a second producer. */
#include "editor/studio/studio_nodes.h"
#include "types/array.h"

#include <inttypes.h>

#define OUTLINER_ROWS_FRAMES 2
#define OUTLINER_ROWS_ACTORS 4

static arena outliner_rows_storage[2];
static pin_value outliner_rows_retained[2][2];
static char outliner_rows_keys[OUTLINER_ROWS_ACTORS][24];
static int outliner_rows_queries, outliner_rows_materialized;

/* Literal expectations, independent of the production selectors. Glyph bytes
 * are U+F01A7 (prototype), U+F0D5D (camera) and U+F0336 (light). */
static const studio_object_row OUTLINER_ROWS_EXPECTED[] = {
    { .label = "Alpha",
      .icon = "\xf3\xb0\x86\xa7",
      .prototype = "Plain",
      .drop = "object behavior",
      .depth = 1 },
    { .label = "Bolt",
      .icon = "\xf3\xb0\xb5\x9d",
      .prototype = "CameraRig",
      .drop = "object behavior material shape ui field system",
      .depth = 2,
      .behaviors = 1,
      .marked = true,
      .hidden = true,
      .renaming = true },
    { .label = "Élan",
      .icon = "\xf3\xb0\x8c\xb6",
      .prototype = "Lamp",
      .drop = "object behavior",
      .depth = 3,
      .active = true,
      .locked = true },
    { .label = "Delta",
      .icon = "\xf3\xb0\x86\xa7",
      .prototype = "Plain",
      .drop = "object behavior",
      .depth = 1,
      .marked = true,
      .hidden = true,
      .locked = true },
};

typedef struct
{
    int first, count;
    const char *filter;
    int shown, count_out, actors[OUTLINER_ROWS_ACTORS];
} outliner_rows_request;

static const outliner_rows_request OUTLINER_ROWS_ALL = { 0, 4, "",
                                                         4, 4, { 0, 1, 2, 3 } };
static const outliner_rows_request OUTLINER_ROWS_EMPTY = { 0, 4, "", 0, 0, { 0 } };
static const outliner_rows_request OUTLINER_ROWS_REQUESTS[] = {
    { 1, 2, "", 4, 2, { 1, 2 } },    { 3, 9, "", 4, 1, { 3 } },
    { -2, 2, "", 4, 2, { 0, 1 } },   { 0, -1, "", 4, 0, { 0 } },
    { 99, 4, "", 4, 0, { 0 } },      { 0, 0, "", 4, 0, { 0 } },
    { 0, 4, "ALP", 1, 1, { 0 } },    { 0, 4, "camera", 1, 1, { 1 } },
    { 0, 4, "absent", 0, 0, { 0 } }, { 0, 4, "É", 1, 1, { 2 } },
};

static bool outliner_rows_check(
    const arena *storage,
    const pin_value *out,
    const outliner_rows_request *request,
    bool editable,
    bool renaming,
    bool renamed
)
{
    REQUIRE(out[0].type == PIN_STRUCT && out[0].container == PIN_CONTAINER_ARRAY);
    REQUIRE(out[1].type == PIN_I32 && out[1].container == PIN_CONTAINER_SINGLE);
    REQUIRE(!out[0].record && !out[1].record);
    REQUIRE(out[1].type_name && !strcmp(out[1].type_name, "int"));
    REQUIRE(out[1].i == request->shown);
    const pin_array *array = out[0].ptr;
    REQUIRE(array && arena_contains(storage, array));
    REQUIRE(array->element_type == PIN_STRUCT);
    REQUIRE(!array->record);
    REQUIRE(array->count == request->count_out && array->capacity == request->count_out);
    REQUIRE(array->type_name && arena_contains(storage, array->type_name));
    REQUIRE(!strcmp(array->type_name, STUDIO_OBJECT_ROW_TYPE));
    REQUIRE(out[0].type_name == array->type_name);
    for (int i = 0; i < array->count; i++) {
        REQUIRE(arena_contains(storage, &array->items[i]));
        const pin_value *value = &array->items[i];
        REQUIRE(value->type == PIN_STRUCT && value->container == PIN_CONTAINER_SINGLE);
        REQUIRE(!value->record);
        REQUIRE(value->type_name == array->type_name);
        const studio_object_row *row = value->ptr;
        REQUIRE(row && arena_contains(storage, row));
        const char *strings[] = { row->key, row->label, row->icon, row->prototype, row->drop };
        for (size_t j = 0; j < sizeof strings / sizeof *strings; j++) {
            REQUIRE(strings[j] && arena_contains(storage, strings[j]));
        }
        int actor = request->actors[i];
        const studio_object_row *expected = &OUTLINER_ROWS_EXPECTED[actor];
        REQUIRE(!strcmp(row->key, outliner_rows_keys[actor]));
        REQUIRE(
            !strcmp(row->label, renamed && actor == 1 ? "Bolt-new" : expected->label)
        );
        REQUIRE(!strcmp(row->icon, expected->icon));
        REQUIRE(!strcmp(row->prototype, expected->prototype));
        REQUIRE(!strcmp(row->drop, editable ? expected->drop : ""));
        REQUIRE(row->depth == expected->depth && row->behaviors == expected->behaviors);
        REQUIRE(row->active == expected->active && row->marked == expected->marked);
        REQUIRE(row->hidden == expected->hidden && row->locked == expected->locked);
        REQUIRE(row->renaming == (renaming && expected->renaming));
    }
    return true;
}

static bool outliner_rows_read(studio *s, arena *storage, pin_value *out, const outliner_rows_request *request)
{
    snprintf(s->outliner_filter, sizeof s->outliner_filter, "%s", request->filter);
    pin_value in[] = {
        { .type = PIN_I32, .i = request->first },
        { .type = PIN_I32, .i = request->count },
    };
    REQUIRE(!studio_node_run(s, STUDIO_NODE_SCENE_OBJECTS, in, storage, out));
    outliner_rows_queries++;
    REQUIRE(out[0].ptr);
    outliner_rows_materialized += ((const pin_array *)out[0].ptr)->count;
    return true;
}

static bool outliner_rows_once(studio *s, const outliner_rows_request *request, bool editable, bool renaming)
{
    arena storage = { 0 };
    pin_value out[2] = { 0 };
    bool ok = outliner_rows_read(s, &storage, out, request) &&
              outliner_rows_check(&storage, out, request, editable, renaming, false);
    arena_free(&storage);
    REQUIRE(ok);
    return true;
}

/* No forged handles enter a live world. This isolates the actual command-key
 * resolver and distinguishes entity bits from persistent actor identities. */
static bool outliner_rows_wide_keys()
{
    static const uint64_t ids[] = {
        UINT64_C(0x8000000100000007),
        UINT64_C(0x8000000200000007),
        UINT64_C(0xfedcba9800000009),
        UINT64_C(0x0020000000000001),
    };
    static const char *const keys[] = {
        "9223372041149743111",
        "9223372045444710407",
        "18364758542507835401",
        "9007199254740993",
    };
    app_scene_authoring *scene = xcalloc(1, sizeof *scene);
    studio *s = xcalloc(1, sizeof *s);
    s->scene = scene;
    for (int i = 0; i < OUTLINER_ROWS_ACTORS; i++) {
        scene_actor actor = { .identity = (uint64_t)i + 11,
                              .entity = ecs_entity_from_u64(ids[i]) };
        apush(scene->actors, actor);
    }
    bool ok = true;
    for (int i = 0; i < OUTLINER_ROWS_ACTORS; i++) {
        ok &= studio_object_index(s, keys[i]) == i;
    }
    ok &= studio_object_index(s, "11") == -1 && studio_object_index(s, NULL) == -1;
    scene_actor first = scene->actors[0];
    scene->actors[0] = scene->actors[1];
    scene->actors[1] = first;
    ok &= studio_object_index(s, keys[0]) == 1;
    asetlen(scene->actors, 0);
    ok &= studio_object_index(s, keys[0]) == -1;
    afree(scene->actors);
    free(scene);
    free(s);
    REQUIRE(ok);
    return true;
}

static bool outliner_rows_fixture(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(acount(scene->actors) == 0);
    REQUIRE(outliner_rows_once(s, &OUTLINER_ROWS_EMPTY, true, true));
    static const char *const prototypes[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Plain\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"CameraRig\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ])),\n"
        "                data.Field.Named(\"DirectionalLight\", "
        "data.Object([])),\n"
        "                data.Field.Named(\"Camera\", data.Object([\n"
        "                    data.Field.Named(\"width\", "
        "data.Value.UInt(64)),\n"
        "                    data.Field.Named(\"height\", "
        "data.Value.UInt(64))\n"
        "                ])),\n"
        "                data.Field.Named(\"Shader\", data.Object([])),\n"
        "                data.Field.Named(\"Ui\", data.Object([])),\n"
        "                data.Field.Named(\"EngineSystem\", data.Object([]))\n"
        "            ])),\n"
        "            data.Field.Named(\"behaviors\", data.List([\n"
        "                data.Value.Text(\"Counter\")\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Lamp\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([\n"
        "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
        "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
        "                ])),\n"
        "                data.Field.Named(\"DirectionalLight\", "
        "data.Object([]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
    };
    for (size_t i = 0; i < sizeof prototypes / sizeof *prototypes; i++) {
        REQUIRE(app_scene_authoring_register_prototype(scene, prototypes[i], NULL) == 0);
    }
    REQUIRE(
        scene_authoring_load_text(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Rows\")),\n"
            "            data.Field.Named(\"entities\", data.List([\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Alpha\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Plain\")),\n"
            "                    data.Field.Named(\"children\", data.List([\n"
            "                        data.Object([\n"
            "                            data.Field.Named(\"name\", "
            "data.Value.Text(\"Bolt\")),\n"
            "                            data.Field.Named(\"prototype\", "
            "data.Value.Text(\"CameraRig\")),\n"
            "                            data.Field.Named(\"components\", "
            "data.Object([\n"
            "                                "
            "data.Field.Named(\"MaterialTerrain\", data.Object([]))\n"
            "                            ])),\n"
            "                            data.Field.Named(\"children\", "
            "data.List([\n"
            "                                data.Object([\n"
            "                                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Élan\")),\n"
            "                                    "
            "data.Field.Named(\"prototype\", data.Value.Text(\"Lamp\"))\n"
            "                                ])\n"
            "                            ]))\n"
            "                        ])\n"
            "                    ]))\n"
            "                ]),\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"name\", "
            "data.Value.Text(\"Delta\")),\n"
            "                    data.Field.Named(\"prototype\", "
            "data.Value.Text(\"Plain\"))\n"
            "                ])\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    REQUIRE(acount(scene->actors) == OUTLINER_ROWS_ACTORS && studio_editable(s));
    select_instance(scene, 1);
    scene_toggle_instance(scene, 3);
    scene_toggle_instance(scene, 2);
    const unsigned views[] = { 0,
                               APP_SCENE_VIEW_HIDDEN,
                               APP_SCENE_VIEW_LOCKED,
                               APP_SCENE_VIEW_HIDDEN | APP_SCENE_VIEW_LOCKED };
    for (int i = 0; i < OUTLINER_ROWS_ACTORS; i++) {
        app_scene_authoring_set_actor_view(scene, i, views[i]);
        snprintf(
            outliner_rows_keys[i],
            sizeof outliner_rows_keys[i],
            "%" PRIu64,
            ecs_entity_to_u64(scene->actors[i].entity)
        );
        REQUIRE(studio_object_index(s, outliner_rows_keys[i]) == i);
    }
    s->rename.outliner = true;
    s->rename.entity = ecs_entity_to_u64(scene->actors[1].entity);
    REQUIRE(
        outliner_rows_read(s, &outliner_rows_storage[0], outliner_rows_retained[0], &OUTLINER_ROWS_ALL)
    );
    REQUIRE(
        outliner_rows_check(&outliner_rows_storage[0], outliner_rows_retained[0], &OUTLINER_ROWS_ALL, true, true, false)
    );
    for (size_t i = 0; i < sizeof OUTLINER_ROWS_REQUESTS / sizeof *OUTLINER_ROWS_REQUESTS;
         i++) {
        REQUIRE(outliner_rows_once(s, &OUTLINER_ROWS_REQUESTS[i], true, true));
    }
    /* Read-only view state, with no runtime transition or host frame inside. */
    scene->runtime_snapshot_valid = true;
    bool readonly = outliner_rows_once(s, &OUTLINER_ROWS_ALL, false, true);
    scene->runtime_snapshot_valid = false;
    REQUIRE(readonly);
    s->rename.outliner = false;
    REQUIRE(outliner_rows_once(s, &OUTLINER_ROWS_ALL, true, false));
    s->rename.outliner = true;
    REQUIRE(app_scene_authoring_rename_instance(scene, scene->actors[1].entity, "Bolt-new") == 0);
    REQUIRE(
        outliner_rows_read(s, &outliner_rows_storage[1], outliner_rows_retained[1], &OUTLINER_ROWS_ALL)
    );
    REQUIRE(
        outliner_rows_check(&outliner_rows_storage[1], outliner_rows_retained[1], &OUTLINER_ROWS_ALL, true, true, true)
    );
    REQUIRE(
        outliner_rows_check(&outliner_rows_storage[0], outliner_rows_retained[0], &OUTLINER_ROWS_ALL, true, true, false)
    );
    s->rename.entity = 0;
    s->rename.outliner = false;
    REQUIRE(
        scene_authoring_load_text(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Replacement\")),\n"
            "            data.Field.Named(\"entities\", data.List([]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    REQUIRE(outliner_rows_once(s, &OUTLINER_ROWS_EMPTY, true, false));
    for (int i = 0; i < OUTLINER_ROWS_ACTORS; i++) {
        REQUIRE(studio_object_index(s, outliner_rows_keys[i]) == -1);
    }
    REQUIRE(
        outliner_rows_check(&outliner_rows_storage[0], outliner_rows_retained[0], &OUTLINER_ROWS_ALL, true, true, false)
    );
    REQUIRE(
        outliner_rows_check(&outliner_rows_storage[1], outliner_rows_retained[1], &OUTLINER_ROWS_ALL, true, true, true)
    );
    return true;
}

static bool outliner_rows_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->scene);
    frames++;
    if (frames == 1) {
        return outliner_rows_fixture(s);
    }
    REQUIRE(frames == OUTLINER_ROWS_FRAMES && acount(s->scene->actors) == 0);
    return true;
}

TEST_CASE(outliner_rows)
{
    REQUIRE(outliner_rows_wide_keys());
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(OUTLINER_ROWS_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    int status = studio_acceptance_host_main(sizeof args / sizeof *args, args);
    /* Read both original result arenas after normal host/scene/world shutdown.
     * No test-side retaining copy can hide a borrowed production string. */
    bool retained = status == 0 && callback_ok && frames == OUTLINER_ROWS_FRAMES &&
                    outliner_rows_check(
                        &outliner_rows_storage[0],
                        outliner_rows_retained[0],
                        &OUTLINER_ROWS_ALL,
                        true,
                        true,
                        false
                    ) &&
                    outliner_rows_check(
                        &outliner_rows_storage[1],
                        outliner_rows_retained[1],
                        &OUTLINER_ROWS_ALL,
                        true,
                        true,
                        true
                    );
    arena_free(&outliner_rows_storage[0]);
    arena_free(&outliner_rows_storage[1]);
    REQUIRE(retained);
    REQUIRE(outliner_rows_queries == 16 && outliner_rows_materialized == 24);
    REQUIRE(unchanged_source());
    printf(
        "\nStudio Outliner rows accepted: frames=%d queries=%d rows=%d\n",
        frames,
        outliner_rows_queries,
        outliner_rows_materialized
    );
    return true;
}
