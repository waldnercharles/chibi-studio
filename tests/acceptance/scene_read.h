/* Private capture/lifetime checks in the existing acceptance dispatcher.
 * No renderer, UI service binding, Begin, tick or authored dispatch. Six cases,
 * thirteen captures: eight successes and five rejections. A single bytecode
 * source preparation supplies deferred, pending-only attachments. */
#include "authoring/source_prepare.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_scene_read.h"
#include "runtime/component_catalog.h"
#include "runtime/save.h"

#include <stddef.h>

#define SCENE_READ_ACTORS 4
#define SCENE_READ_CUSTOM 5
#define SCENE_READ_COMPONENTS 8
#define SCENE_READ_CAPTURES 13

typedef struct
{
    const char *a, *b, *c;
    int number;
} scene_read_value;

typedef struct
{
    studio *s, *other;
    app_scene_authoring *scene, *replacement;
    ecs_world *world;
    preview_runtime runtime;
    preview_world live;
    ecs_component_desc custom[SCENE_READ_CUSTOM];
    type_desc types[SCENE_READ_CUSTOM];
    type_field fields[SCENE_READ_CUSTOM][3];
    const ecs_component_desc *components[SCENE_READ_COMPONENTS];
} scene_read_fixture;

static scene_read_fixture *scene_read_fixture_active;
static authoring_source *scene_read_source;
static studio_scene_read_scope scene_read_scope;
static studio_scene_read *scene_read_owned[SCENE_READ_CAPTURES];
static int scene_read_owned_count;
static int scene_read_capture_count, scene_read_successes, scene_read_rejections;
static int scene_read_preparations, scene_read_registrations, scene_read_attachments;
static uint64_t scene_read_entities[SCENE_READ_ACTORS];
static ecs_component_id scene_read_components[SCENE_READ_CUSTOM];
static char scene_read_error[256];
static const char *const SCENE_READ_NAMES[] = { "Alpha", "Bolt", "Élan", "Delta" };
static const char *const SCENE_READ_PROTOTYPES[] = { "", "CameraRig", "Lamp", "Unresolved" };
static const char *const SCENE_READ_SOURCE_NAMES[] = { "ReadAlpha", "ReadBeta" };
static const int SCENE_READ_ATTACHMENT_ACTORS[] = { 0, 1, 1, 2, 3, 3 };
static const int SCENE_READ_ATTACHMENT_SOURCES[] = { 0, 0, 1, 1, 0, 1 };
static const uint64_t SCENE_READ_RENAME = UINT64_C(0xfedcba9800000009);

static void scene_read_fixture_free()
{
    scene_read_fixture *f = scene_read_fixture_active;
    if (!f) {
        return;
    }
    source_registry_free(f->runtime.sources);
    entities_free(f->runtime.entities);
    ecs_world_free(f->world);
    for (int i = 0; i < acount(f->scene->actors); i++) {
        free(f->scene->actors[i].name);
        free(f->scene->actors[i].prototype_name);
    }
    afree(f->scene->actors);
    for (int i = 0; i < acount(f->scene->prototypes); i++) {
        prototype_free(f->scene->prototypes[i].prototype);
        free(f->scene->prototypes[i].path);
    }
    afree(f->scene->prototypes);
    for (int i = 0; i < acount(f->scene->view); i++) {
        free(f->scene->view[i].name);
    }
    afree(f->scene->view);
    free(f->scene->view_path);
    free(f->scene->scene_path);
    save_free(f->scene->view_store);
    component_presentation_free(&f->s->presentation.hints);
    for (int c = 0; c < SCENE_READ_CUSTOM; c++) {
        free((char *)f->custom[c].key);
        free((char *)f->custom[c].name);
        free((char *)f->types[c].name);
        for (int i = 0; i < f->types[c].field_count; i++) {
            free((char *)f->fields[c][i].name);
        }
    }
    free(f->replacement);
    free(f->other);
    free(f->scene);
    free(f->s);
    free(f);
    scene_read_fixture_active = NULL;
}

static void scene_read_cleanup()
{
    if (scene_read_scope.entered) {
        studio_scene_read_end(&scene_read_scope);
    }
    for (int i = 0; i < scene_read_owned_count; i++) {
        studio_scene_read_free(scene_read_owned[i]);
        scene_read_owned[i] = NULL;
    }
    scene_read_owned_count = 0;
    scene_read_fixture_free();
}

static scene_read_fixture *scene_read_fixture_new()
{
    assert(!scene_read_fixture_active);
    scene_read_fixture *f = xcalloc(1, sizeof *f);
    scene_read_fixture_active = f;
    f->s = xcalloc(1, sizeof *f->s);
    f->scene = xcalloc(1, sizeof *f->scene);
    f->world = ecs_world_new();
    f->runtime.sources = source_registry_new(f->world);
    f->runtime.entities = entities_new(f->world, NULL);
    f->runtime.ecs = f->world;
    f->live.runtime = &f->runtime;
    f->scene->world = f->world;
    f->scene->live = &f->live;
    f->scene->view_store = save_new();
    f->scene->scene_path = xstrdup("content/scenes/Read.chibi");
    f->scene->active_runtime_target = APP_RUNTIME_SCENE;
    f->scene->selected = -1;
    f->s->scene = f->scene;
    snprintf(f->s->outliner_filter, sizeof f->s->outliner_filter, "cam");
    f->s->rename.entity = SCENE_READ_RENAME;
    f->s->rename.outliner = true;
    return f;
}

static studio_scene_read *scene_read_take()
{
    assert(scene_read_fixture_active && scene_read_owned_count < SCENE_READ_CAPTURES);
    scene_read_capture_count++;
    studio_scene_read *read = studio_scene_read_capture(
        &scene_read_scope,
        scene_read_fixture_active->s,
        scene_read_error,
        sizeof scene_read_error
    );
    if (read) {
        scene_read_successes++;
        scene_read_owned[scene_read_owned_count++] = read;
    } else {
        scene_read_rejections++;
    }
    return read;
}

static bool scene_read_make_actors(scene_read_fixture *f)
{
    for (int i = 0; i < SCENE_READ_ACTORS; i++) {
        ecs_entity entity = ecs_world_create_entity(f->world);
        REQUIRE(entity.slot);
        scene_read_entities[i] = ecs_entity_to_u64(entity);
        apush(
            f->scene->actors,
            ((scene_actor){
                .identity = UINT64_C(9007199254741001) + (uint64_t)i,
                .entity = entity,
                .name = xstrdup(SCENE_READ_NAMES[i]),
                .prototype_name = i ? xstrdup(SCENE_READ_PROTOTYPES[i]) : NULL,
                .marked = i == 1 || i == 3,
            })
        );
    }
    REQUIRE(
        ecs_world_record_add_pair(
            f->world,
            f->scene->actors[1].entity,
            ECS_CHILD_OF,
            f->scene->actors[0].entity,
            0
        ) == ECS_OK
    );
    REQUIRE(
        ecs_world_record_add_pair(
            f->world,
            f->scene->actors[2].entity,
            ECS_CHILD_OF,
            f->scene->actors[1].entity,
            1
        ) == ECS_OK
    );
    REQUIRE(ecs_world_apply_commands(f->world) == ECS_OK);
    f->scene->selected = 2;
    REQUIRE(save_set(f->scene->view_store, f->scene->scene_path, "{\"Bolt\":1,\"Élan\":2,\"Delta\":3}"));
    f->s->rename.entity = scene_read_entities[1];
    return true;
}

static void scene_read_field(
    scene_read_fixture *f,
    int component,
    int at,
    const char *name,
    pin_type type,
    size_t offset
)
{
    f->fields[component][at] = (type_field){
        .name = xstrdup(name),
        .type = type,
        .offset = offset,
        .size = type == PIN_I32 ? sizeof(int) : sizeof(const char *),
        .type_name = type == PIN_I32 ? "int" : "const char *",
    };
    f->types[component].field_count++;
}

static bool scene_read_make_components(scene_read_fixture *f)
{
    static const char *const types[] = { "Shader", "Shader", "Ui", "MaterialTerrain", "EngineSystem" };
    static const component_icon icons[] = { COMPONENT_ICON_LIGHT,
                                            COMPONENT_ICON_CAMERA,
                                            COMPONENT_ICON_LIGHT,
                                            COMPONENT_ICON_NONE,
                                            COMPONENT_ICON_SYSTEM };
    for (int c = 0; c < SCENE_READ_CUSTOM; c++) {
        char key[48];
        snprintf(key, sizeof key, "test.scene-read.component.%d", c);
        f->types[c] = (type_desc){ .name = xstrdup(types[c]),
                                   .size = sizeof(scene_read_value),
                                   .fields = f->fields[c] };
        f->custom[c] = (ecs_component_desc){
            .id = ecs_component_marker_id(key),
            .key = xstrdup(key),
            .name = xstrdup(key),
            .element_size = sizeof(scene_read_value),
            .type = &f->types[c],
        };
        scene_read_components[c] = f->custom[c].id;
        f->components[c] = &f->custom[c];
        apush(
            f->s->presentation.hints.components,
            ((presented_component){ .id = f->custom[c].id, .icon = icons[c] })
        );
    }
    scene_read_field(f, 0, 0, "material", PIN_STR, offsetof(scene_read_value, a));
    scene_read_field(f, 0, 1, "shape", PIN_STR, offsetof(scene_read_value, b));
    scene_read_field(f, 0, 2, "unclassified", PIN_STR, offsetof(scene_read_value, c));
    scene_read_field(f, 1, 0, "material", PIN_NAME, offsetof(scene_read_value, a));
    scene_read_field(f, 1, 1, "shape", PIN_I32, offsetof(scene_read_value, number));
    scene_read_field(f, 2, 0, "document", PIN_NAME, offsetof(scene_read_value, a));
    scene_read_field(f, 3, 0, "field", PIN_STR, offsetof(scene_read_value, a));
    scene_read_field(f, 4, 0, "asset", PIN_NAME, offsetof(scene_read_value, a));
    f->components[5] = &COMPONENT_Transform;
    f->components[6] = &COMPONENT_Orientation;
    f->components[7] = &COMPONENT_Transform3D;
    f->scene->registry = (prototype_registry){ .components = f->components,
                                               .component_count = SCENE_READ_COMPONENTS };
    for (int c = 0; c < SCENE_READ_COMPONENTS; c++) {
        REQUIRE(ecs_world_register_component(f->world, f->components[c], NULL) == ECS_OK);
    }
    for (int c = 0; c < SCENE_READ_CUSTOM; c++) {
        REQUIRE(
            ecs_world_record_add(
                f->world,
                f->scene->actors[1].entity,
                f->custom[c].id,
                NULL,
                (uint64_t)c
            ) == ECS_OK
        );
    }
    REQUIRE(
        ecs_world_record_add(f->world, f->scene->actors[3].entity, f->custom[2].id, NULL, 5) == ECS_OK
    );
    Transform position = { 1, 2 };
    Orientation orientation = { .radians = 0.5f };
    Transform3D spatial = { .position = { 3, 4, 5 },
                            .rotation = { 0, 0, 0, 1 },
                            .scale = { 1, 2, 3 } };
    REQUIRE(
        ecs_world_record_add(
            f->world,
            f->scene->actors[0].entity,
            COMPONENT_Transform.id,
            &position,
            6
        ) == ECS_OK
    );
    REQUIRE(
        ecs_world_record_add(
            f->world,
            f->scene->actors[1].entity,
            COMPONENT_Orientation.id,
            &orientation,
            7
        ) == ECS_OK
    );
    REQUIRE(
        ecs_world_record_add(
            f->world,
            f->scene->actors[2].entity,
            COMPONENT_Transform3D.id,
            &spatial,
            8
        ) == ECS_OK
    );
    REQUIRE(ecs_world_apply_commands(f->world) == ECS_OK);
    return true;
}

static bool scene_read_make_full(scene_read_fixture *f)
{
    REQUIRE(scene_read_source && scene_read_source->valid && scene_read_source->module);
    REQUIRE(scene_read_make_actors(f));
    REQUIRE(scene_read_make_components(f));
    static const char *const names[] = { "Plain", "CameraRig", "Lamp" };
    for (int p = 0; p < 3; p++) {
        prototype *document = prototype_new(names[p]);
        int count = p == 1 ? 3 : p == 2 ? 1 : 0;
        for (int c = 0; c < count; c++) {
            apush(document->components, ((prototype_component){ .desc = &f->custom[c] }));
        }
        char path[80];
        snprintf(path, sizeof path, "content/prototypes/%s.kdl", names[p]);
        apush(
            f->scene->prototypes,
            ((scene_prototype_asset){ .path = xstrdup(path), .prototype = document })
        );
    }
    for (int i = 0; i < 2; i++) {
        scene_read_registrations++;
        REQUIRE(
            source_register(
                f->runtime.sources,
                SCENE_READ_SOURCE_NAMES[i],
                scene_read_source->module,
                NULL
            ) == 0
        );
    }
    for (int i = 0; i < 6; i++) {
        scene_read_attachments++;
        REQUIRE(
            source_attach(
                f->runtime.sources,
                f->scene->actors[SCENE_READ_ATTACHMENT_ACTORS[i]].entity,
                SCENE_READ_SOURCE_NAMES[SCENE_READ_ATTACHMENT_SOURCES[i]],
                true,
                0,
                0
            ) == 0
        );
    }
    return true;
}

static bool scene_read_empty_facts(const studio_scene_read *read)
{
    const studio_scene_read_facts *facts = studio_scene_read_get(read);
    REQUIRE(facts->actor_count == 0 && !facts->nodes && !facts->actors);
    REQUIRE(facts->prototype_count == 0 && !facts->prototypes);
    REQUIRE(facts->component_count == 0 && !facts->components);
    REQUIRE(facts->field_count == 0 && !facts->fields);
    REQUIRE(facts->attachment_count == 0 && !facts->attachments);
    REQUIRE(facts->active == -1 && facts->editable && !facts->live);
    REQUIRE(ecs_entity_to_u64(facts->rename) == SCENE_READ_RENAME && facts->rename_outliner);
    REQUIRE(!strcmp(facts->filter, "cam") && !strcmp(facts->scene_path, "content/scenes/Read.chibi"));
    return true;
}

static bool scene_read_full_facts(const studio_scene_read *read, bool changed)
{
    const studio_scene_read_facts *facts = studio_scene_read_get(read);
    static const int parents[] = { -1, 0, 1, -1 };
    static const int prototypes[] = { -1, 1, 2, -1 };
    static const int first[] = { 0, 0, 6, 6 }, counts[] = { 0, 6, 0, 1 };
    REQUIRE(facts->actor_count == 4 && facts->prototype_count == 3);
    REQUIRE(facts->component_count == 4 && facts->field_count == 7 && facts->attachment_count == 6);
    for (int i = 0; i < SCENE_READ_ACTORS; i++) {
        const authoring_scene_node *node = &facts->nodes[i];
        const studio_scene_read_actor *actor = &facts->actors[i];
        REQUIRE(ecs_entity_to_u64(node->entity) == scene_read_entities[i]);
        REQUIRE(node->parent == parents[i]);
        REQUIRE(!strcmp(node->name, changed && i == 1 ? "Bolt-new" : SCENE_READ_NAMES[i]));
        REQUIRE(actor->identity == UINT64_C(9007199254741001) + (uint64_t)i);
        REQUIRE(
            !strcmp(actor->prototype_name, SCENE_READ_PROTOTYPES[i]) &&
            actor->prototype == prototypes[i]
        );
        REQUIRE(actor->field_first == first[i] && actor->field_count == counts[i]);
        REQUIRE(
            actor->view_flags == (unsigned)(changed ? (i == 0   ? 1
                                                       : i == 1 ? 2
                                                                : 0)
                                                    : i)
        );
        REQUIRE(actor->selected == (changed ? i == 1 : i != 0));
        REQUIRE(node->has_transform_2d == (i == 0));
        REQUIRE(node->has_orientation == (i == 1));
        REQUIRE(node->has_transform_3d == (i == 2));
    }
    REQUIRE(facts->nodes[0].position.x == 1 && facts->nodes[0].position.y == 2);
    REQUIRE(facts->nodes[1].orientation.radians == 0.5f);
    const Transform3D *spatial = &facts->nodes[2].transform_3d;
    REQUIRE(
        spatial->position.x == 3 && spatial->position.y == 4 &&
        spatial->position.z == 5
    );
    REQUIRE(
        spatial->rotation.x == 0 && spatial->rotation.y == 0 &&
        spatial->rotation.z == 0 && spatial->rotation.w == 1
    );
    REQUIRE(spatial->scale.x == 1 && spatial->scale.y == 2 && spatial->scale.z == 3);
    static const char *const prototype_names[] = { "Plain", "CameraRig", "Lamp" };
    static const int component_first[] = { 0, 0, 3 }, component_count[] = { 0, 3, 1 };
    for (int i = 0; i < 3; i++) {
        const studio_scene_read_prototype *p = &facts->prototypes[i];
        char path[80], identity[32];
        snprintf(path, sizeof path, "content/prototypes/%s.kdl", prototype_names[i]);
        snprintf(
            identity,
            sizeof identity,
            "prototype-%016llx",
            (unsigned long long)ecs_component_marker_id(prototype_names[i])
        );
        REQUIRE(!strcmp(p->name, prototype_names[i]) && !strcmp(p->path, path));
        REQUIRE(!strcmp(p->identity, identity));
        REQUIRE(
            p->component_first == component_first[i] &&
            p->component_count == component_count[i]
        );
    }
    static const int component_indices[] = { 0, 1, 2, 0 };
    static const component_icon icons[] = { COMPONENT_ICON_LIGHT,
                                            COMPONENT_ICON_CAMERA,
                                            COMPONENT_ICON_LIGHT,
                                            COMPONENT_ICON_LIGHT };
    for (int i = 0; i < 4; i++) {
        REQUIRE(facts->components[i].component == scene_read_components[component_indices[i]]);
        REQUIRE(!strcmp(facts->components[i].type_name, i == 2 ? "Ui" : "Shader"));
        REQUIRE(
            facts->components[i].icon ==
            (changed && (i == 0 || i == 3) ? COMPONENT_ICON_NONE : icons[i])
        );
    }
    static const int field_components[] = { 0, 0, 1, 2, 3, 4, 2 };
    static const int field_indices[] = { 0, 1, 0, 0, 0, 0, 0 };
    static const char *const field_types[] = {
        "Shader",          "Shader",       "Shader", "Ui",
        "MaterialTerrain", "EngineSystem", "Ui"
    };
    static const char *const field_names[] = { "material", "shape", "material",
                                               "document", "field", "asset",
                                               "document" };
    static const pin_type pin_types[] = { PIN_STR, PIN_STR,  PIN_NAME, PIN_NAME,
                                          PIN_STR, PIN_NAME, PIN_NAME };
    static const reference_asset_kind kinds[] = {
        REFERENCE_ASSET_MATERIAL, REFERENCE_ASSET_SHAPE,
        REFERENCE_ASSET_MATERIAL, REFERENCE_ASSET_UI,
        REFERENCE_ASSET_FIELD,    REFERENCE_ASSET_SYSTEM,
        REFERENCE_ASSET_UI
    };
    for (int i = 0; i < 7; i++) {
        const studio_scene_read_field *field = &facts->fields[i];
        REQUIRE(
            field->component == scene_read_components[field_components[i]] &&
            field->field == field_indices[i]
        );
        REQUIRE(
            !strcmp(field->component_type, field_types[i]) &&
            !strcmp(field->name, field_names[i])
        );
        REQUIRE(field->type == pin_types[i] && field->kind == kinds[i]);
    }
    for (int i = 0; i < 6; i++) {
        const studio_scene_read_attachment *attachment = &facts->attachments[i];
        REQUIRE(
            ecs_entity_to_u64(attachment->entity) ==
            scene_read_entities[SCENE_READ_ATTACHMENT_ACTORS[i]]
        );
        REQUIRE(attachment->sequence == (uint64_t)i + 1 && attachment->begin == 0);
        REQUIRE(
            !strcmp(attachment->name, SCENE_READ_SOURCE_NAMES[SCENE_READ_ATTACHMENT_SOURCES[i]])
        );
    }
    REQUIRE(facts->active == (changed ? 1 : 2));
    REQUIRE(facts->editable == !changed && facts->live == changed);
    REQUIRE(ecs_entity_to_u64(facts->rename) == scene_read_entities[changed ? 3 : 1]);
    REQUIRE(facts->rename_outliner == !changed);
    REQUIRE(!strcmp(facts->filter, changed ? "new" : "cam"));
    REQUIRE(
        !strcmp(facts->scene_path, changed ? "content/scenes/Changed.chibi" : "content/scenes/Read.chibi")
    );
    return true;
}

/* Check producer aliases before any test-side retaining copy (none is made). */
static bool scene_read_independent(const studio_scene_read *read, const scene_read_fixture *f)
{
    const studio_scene_read_facts *facts = studio_scene_read_get(read);
    REQUIRE((const void *)facts->actors != (const void *)f->scene->actors);
    REQUIRE((const void *)facts->prototypes != (const void *)f->scene->prototypes);
    REQUIRE(
        facts->filter != f->s->outliner_filter && facts->scene_path != f->scene->scene_path
    );
    for (int i = 0; i < facts->actor_count; i++) {
        REQUIRE(facts->nodes[i].name != f->scene->actors[i].name);
        REQUIRE(facts->actors[i].prototype_name != f->scene->actors[i].prototype_name);
    }
    for (int i = 0; i < facts->prototype_count; i++) {
        const prototype *p = f->scene->prototypes[i].prototype;
        REQUIRE(
            facts->prototypes[i].name != p->name &&
            facts->prototypes[i].identity != prototype_identity(p)
        );
        REQUIRE(!strcmp(facts->prototypes[i].identity, prototype_identity(p)));
        REQUIRE(facts->prototypes[i].path != f->scene->prototypes[i].path);
        for (int c = 0; c < facts->prototypes[i].component_count; c++) {
            REQUIRE(
                facts->components[facts->prototypes[i].component_first + c].type_name !=
                p->components[c].desc->type->name
            );
        }
    }
    for (int i = 0; i < facts->field_count; i++) {
        const studio_scene_read_field *field = &facts->fields[i];
        const ecs_component_desc *desc = NULL;
        for (int c = 0; c < SCENE_READ_CUSTOM; c++) {
            if (f->custom[c].id == field->component) {
                desc = &f->custom[c];
            }
        }
        REQUIRE(desc);
        REQUIRE(
            field->component_type != desc->type->name &&
            field->name != desc->type->fields[field->field].name
        );
    }
    for (int i = 0; i < facts->attachment_count; i++) {
        source_attachment_info info = source_attachment_at(f->runtime.sources, i);
        REQUIRE(facts->attachments[i].name != info.name);
    }
    return true;
}

TEST_CASE(scene_read_empty)
{
    scene_read_fixture *f = scene_read_fixture_new();
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *read = scene_read_take();
    REQUIRE(read && !scene_read_error[0]);
    REQUIRE(studio_scene_read_current(&scene_read_scope, f->s, read));
    REQUIRE(scene_read_empty_facts(read));
    studio_scene_read_end(&scene_read_scope);
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, read));
    REQUIRE(scene_read_empty_facts(read));
    return true;
}

TEST_CASE(scene_read_raw_facts)
{
    scene_read_fixture *f = scene_read_fixture_new();
    REQUIRE(scene_read_make_full(f));
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *before = scene_read_take();
    REQUIRE(before && scene_read_full_facts(before, false));
    REQUIRE(scene_read_independent(before, f));
    studio_scene_read_invalidate(&scene_read_scope);
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, before));
    studio_scene_read_end(&scene_read_scope);
    free(f->scene->actors[1].name);
    f->scene->actors[1].name = xstrdup("Bolt-new");
    f->scene->selected = 1;
    f->scene->actors[3].marked = false;
    f->scene->runtime_snapshot_valid = true;
    free(f->scene->scene_path);
    f->scene->scene_path = xstrdup("content/scenes/Changed.chibi");
    REQUIRE(save_set(f->scene->view_store, f->scene->scene_path, "{\"Alpha\":1,\"Bolt-new\":2}"));
    f->s->presentation.hints.components[0].icon = COMPONENT_ICON_NONE;
    snprintf(f->s->outliner_filter, sizeof f->s->outliner_filter, "new");
    f->s->rename.entity = scene_read_entities[3];
    f->s->rename.outliner = false;
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *after = scene_read_take();
    REQUIRE(after && scene_read_full_facts(after, true));
    REQUIRE(scene_read_independent(after, f));
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, before));
    REQUIRE(studio_scene_read_current(&scene_read_scope, f->s, after));
    REQUIRE(scene_read_full_facts(before, false));
    studio_scene_read_end(&scene_read_scope);
    scene_read_fixture_free();
    REQUIRE(scene_read_full_facts(before, false));
    REQUIRE(scene_read_full_facts(after, true));
    return true;
}

TEST_CASE(scene_read_invalid_hierarchy)
{
    scene_read_fixture *f = scene_read_fixture_new();
    REQUIRE(scene_read_make_actors(f));
    ecs_entity saved = f->scene->actors[3].entity;
    f->scene->actors[3].entity = f->scene->actors[0].entity;
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    authoring_scene_owner *owner = scene_read_scope.owner;
    REQUIRE(!scene_read_take());
    REQUIRE(strstr(scene_read_error, "more than once") && scene_read_scope.owner == owner);
    studio_scene_read_end(&scene_read_scope);
    f->scene->actors[3].entity = saved;
    scene_actor first = f->scene->actors[0];
    f->scene->actors[0] = f->scene->actors[1];
    f->scene->actors[1] = first;
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    owner = scene_read_scope.owner;
    REQUIRE(!scene_read_take());
    REQUIRE(strstr(scene_read_error, "parents before") && scene_read_scope.owner == owner);
    studio_scene_read_end(&scene_read_scope);
    first = f->scene->actors[0];
    f->scene->actors[0] = f->scene->actors[1];
    f->scene->actors[1] = first;
    REQUIRE(
        ecs_world_record_remove_pair(
            f->world,
            f->scene->actors[1].entity,
            ECS_CHILD_OF,
            f->scene->actors[0].entity,
            0
        ) == ECS_OK
    );
    REQUIRE(
        ecs_world_record_add_pair(
            f->world,
            f->scene->actors[1].entity,
            ECS_CHILD_OF,
            f->scene->actors[3].entity,
            1
        ) == ECS_OK
    );
    REQUIRE(ecs_world_apply_commands(f->world) == ECS_OK);
    /* Exclude the live parent from authored actors. Keep its allocated record
     * for cleanup, and restore the length before any assertion can return. */
    asetlen(f->scene->actors, SCENE_READ_ACTORS - 1);
    bool entered = studio_scene_read_begin(&scene_read_scope, f->s);
    owner = scene_read_scope.owner;
    studio_scene_read *missing = scene_read_take();
    bool unchanged = scene_read_scope.owner == owner;
    if (entered) {
        studio_scene_read_end(&scene_read_scope);
    }
    asetlen(f->scene->actors, SCENE_READ_ACTORS);
    REQUIRE(entered && !missing);
    REQUIRE(strstr(scene_read_error, "parents before") && unchanged);
    return true;
}

TEST_CASE(scene_read_scope_lifetime)
{
    scene_read_fixture *f = scene_read_fixture_new();
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *first = scene_read_take();
    REQUIRE(first && studio_scene_read_current(&scene_read_scope, f->s, first));
    studio_scene_read_invalidate(&scene_read_scope);
    studio_scene_read_invalidate(&scene_read_scope);
    REQUIRE(!scene_read_take());
    REQUIRE(!strcmp(scene_read_error, "Scene read requires its active Studio scope."));
    studio_scene_read_end(&scene_read_scope);
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *second = scene_read_take();
    REQUIRE(second && studio_scene_read_current(&scene_read_scope, f->s, second));
    REQUIRE(!scene_read_error[0]);
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, first));
    REQUIRE(!studio_scene_read_begin(&scene_read_scope, f->s));
    REQUIRE(scene_read_scope.entered && !scene_read_scope.owner);
    REQUIRE(!scene_read_take());
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, second));
    /* Only the successful outer begin ends; the rejected entrant never does. */
    studio_scene_read_end(&scene_read_scope);
    REQUIRE(scene_read_empty_facts(first) && scene_read_empty_facts(second));
    return true;
}

TEST_CASE(scene_read_association)
{
    scene_read_fixture *f = scene_read_fixture_new();
    f->other = xcalloc(1, sizeof *f->other);
    f->other->scene = f->scene;
    f->replacement = xcalloc(1, sizeof *f->replacement);
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *first = scene_read_take();
    REQUIRE(first && !studio_scene_read_current(&scene_read_scope, f->other, first));
    /* Inject only an association mismatch with the owner still alive. This
     * isolates the scene-identity guard, not a real scene replacement hook. */
    authoring_scene_owner *owner = scene_read_scope.owner;
    f->s->scene = f->replacement;
    bool mismatched = studio_scene_read_current(&scene_read_scope, f->s, first);
    f->s->scene = f->scene;
    REQUIRE(
        !mismatched && scene_read_scope.owner == owner &&
        studio_scene_read_current(&scene_read_scope, f->s, first)
    );
    studio_scene_read_invalidate(&scene_read_scope);
    f->s->scene = f->replacement;
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, first));
    studio_scene_read_end(&scene_read_scope);
    f->s->scene = f->scene;
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *second = scene_read_take();
    REQUIRE(second && studio_scene_read_current(&scene_read_scope, f->s, second));
    REQUIRE(!studio_scene_read_current(&scene_read_scope, f->s, first));
    studio_scene_read_end(&scene_read_scope);
    REQUIRE(scene_read_empty_facts(first) && scene_read_empty_facts(second));
    return true;
}

TEST_CASE(scene_read_producer_retirement)
{
    scene_read_fixture *f = scene_read_fixture_new();
    REQUIRE(scene_read_make_full(f));
    REQUIRE(studio_scene_read_begin(&scene_read_scope, f->s));
    studio_scene_read *read = scene_read_take();
    REQUIRE(read && scene_read_full_facts(read, false));
    REQUIRE(scene_read_independent(read, f));
    studio_scene_read_end(&scene_read_scope);
    scene_read_fixture_free();
    authoring_source_free(scene_read_source);
    scene_read_source = NULL;
    REQUIRE(scene_read_full_facts(read, false));
    REQUIRE(
        scene_read_capture_count == 13 && scene_read_successes == 8 &&
        scene_read_rejections == 5
    );
    REQUIRE(
        scene_read_preparations == 1 && scene_read_registrations == 4 &&
        scene_read_attachments == 12
    );
    return true;
}

static void studio_scene_read_tests()
{
    scene_read_capture_count = scene_read_successes = scene_read_rejections = 0;
    scene_read_preparations = scene_read_registrations = scene_read_attachments = 0;
    const script_catalog catalog = { 0 };
    const script_lower_bindings bindings = { 0 };
    scene_read_preparations++;
    scene_read_source = authoring_source_prepare_document(
        "SceneReadFixture.chibi",
        "behaviors/SceneReadFixture.chibi",
        "SceneReadFixture :: behavior {}",
        &catalog,
        &bindings,
        BEHAVIOR_BYTECODE
    );
    pu_setup(NULL, scene_read_cleanup);
    RUN_TEST_CASE(scene_read_empty);
    RUN_TEST_CASE(scene_read_raw_facts);
    RUN_TEST_CASE(scene_read_invalid_hierarchy);
    RUN_TEST_CASE(scene_read_scope_lifetime);
    RUN_TEST_CASE(scene_read_association);
    RUN_TEST_CASE(scene_read_producer_retirement);
    pu_clear_setup();
    authoring_source_free(scene_read_source);
    scene_read_source = NULL;
}
