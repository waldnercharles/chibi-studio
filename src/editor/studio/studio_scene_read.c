#include "editor/studio/studio_scene_read.h"

#include "editor/studio/studio_internal.h"
#include "support/arena.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct studio_scene_read
{
    authoring_scene_snapshot *snapshot;
    arena text;
    studio_scene_read_actor *actors;
    studio_scene_read_prototype *prototypes;
    studio_scene_read_component *components;
    studio_scene_read_field *fields;
    studio_scene_read_attachment *attachments;
    studio_scene_read_facts facts;
};

void studio_scene_read_invalidate(studio_scene_read_scope *scope)
{
    assert(scope);
    if (scope->owner) {
        authoring_scene_owner_free(scope->owner);
        scope->owner = NULL;
    }
}

bool studio_scene_read_begin(studio_scene_read_scope *scope, studio *s)
{
    assert(scope && s && s->scene);
    if (scope->entered) {
        studio_scene_read_invalidate(scope);
        return false;
    }
    assert(!scope->owner);
    *scope = (studio_scene_read_scope){ .owner = authoring_scene_owner_new(),
                                        .studio = s,
                                        .scene = s->scene,
                                        .entered = true };
    return true;
}

void studio_scene_read_end(studio_scene_read_scope *scope)
{
    assert(scope && scope->entered);
    studio_scene_read_invalidate(scope);
    *scope = (studio_scene_read_scope){ 0 };
}

static bool scope_matches(const studio_scene_read_scope *scope, const studio *s)
{
    return scope->entered && scope->owner && scope->studio == s &&
           scope->scene == s->scene;
}

bool studio_scene_read_current(
    const studio_scene_read_scope *scope,
    const studio *s,
    const studio_scene_read *read
)
{
    assert(scope && s && read);
    return scope_matches(scope, s) &&
           authoring_scene_snapshot_current(read->snapshot, scope->owner);
}

static const char *copy_text(studio_scene_read *read, const char *text)
{
    text = text ? text : "";
    size_t size = strlen(text) + 1;
    char *copy = arena_alloc(&read->text, size);
    memcpy(copy, text, size);
    return copy;
}

static int parent_index(const authoring_scene_node *nodes, ecs_entity parent)
{
    if (!parent.slot) {
        return -1;
    }
    for (int i = 0; i < acount(nodes); i++) {
        if (ecs_entity_to_u64(nodes[i].entity) == ecs_entity_to_u64(parent)) {
            return i;
        }
    }
    /* The SDK rejects a missing or later parent. Never silently make a root. */
    return acount(nodes);
}

static authoring_scene_snapshot *capture_nodes(
    const app_scene_authoring *scene,
    authoring_scene_owner *owner,
    char *error,
    size_t error_size
)
{
    authoring_scene_node *nodes = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        const scene_actor *actor = &scene->actors[i];
        const Transform *position = Transform_get_const(scene->world, actor->entity);
        const Orientation
            *orientation = Orientation_get_const(scene->world, actor->entity);
        const Transform3D *spatial = Transform3D_get_const(scene->world, actor->entity);
        authoring_scene_node node = {
            .entity = actor->entity,
            .name = actor->name,
            .parent = parent_index(nodes, ecs_parent(scene->world, actor->entity)),
            .has_transform_2d = position != NULL,
            .has_orientation = orientation != NULL,
            .has_transform_3d = spatial != NULL,
        };
        if (position) {
            node.position = *position;
        }
        if (orientation) {
            node.orientation = *orientation;
        }
        if (spatial) {
            node.transform_3d = *spatial;
        }
        apush(nodes, node);
    }
    authoring_scene_snapshot
        *snapshot = authoring_scene_snapshot_new(owner, nodes, acount(nodes), error, error_size);
    afree(nodes);
    return snapshot;
}

static void capture_prototypes(studio_scene_read *read, const studio *s)
{
    for (int i = 0; i < acount(s->scene->prototypes); i++) {
        const scene_prototype_asset *asset = &s->scene->prototypes[i];
        const prototype *p = asset->prototype;
        assert(p && p->name);
        studio_scene_read_prototype captured = {
            .identity = copy_text(read, prototype_identity(p)),
            .name = copy_text(read, p->name),
            .path = copy_text(read, asset->path),
            .component_first = acount(read->components),
            .component_count = acount(p->components),
        };
        for (int c = 0; c < acount(p->components); c++) {
            const ecs_component_desc *desc = p->components[c].desc;
            apush(
                read->components,
                ((studio_scene_read_component){
                    .component = desc->id,
                    .type_name = copy_text(read, desc->type->name),
                    .icon = component_presentation_icon(&s->presentation.hints, desc),
                })
            );
        }
        apush(read->prototypes, captured);
    }
}

static int captured_prototype_index(const studio_scene_read *read, const char *name)
{
    for (int i = 0; name && i < acount(read->prototypes); i++) {
        if (!strcmp(name, read->prototypes[i].name)) {
            return i;
        }
    }
    return -1;
}

static void capture_fields(studio_scene_read *read, const app_scene_authoring *scene, ecs_entity entity)
{
    /* The same presence/type/classification reads as scene_asset_field. Retain
     * every matching field, including duplicates, without choosing a drop kind
     * or the first field that a future command would write. */
    for (int c = 0; c < scene->registry.component_count; c++) {
        const ecs_component_desc *desc = scene->registry.components[c];
        if (!ecs_component_has(scene->world, entity, desc->id)) {
            continue;
        }
        for (int f = 0; f < desc->type->field_count; f++) {
            const type_field *field = &desc->type->fields[f];
            if (field->type != PIN_STR && field->type != PIN_NAME) {
                continue;
            }
            reference_asset_kind
                kind = reference_named_field_kind(desc->type->name, field->name);
            if (kind == REFERENCE_ASSET_KIND_COUNT) {
                continue;
            }
            apush(
                read->fields,
                ((studio_scene_read_field){
                    .component = desc->id,
                    .component_type = copy_text(read, desc->type->name),
                    .name = copy_text(read, field->name),
                    .field = f,
                    .type = field->type,
                    .kind = kind,
                })
            );
        }
    }
}

static void capture_attachments(studio_scene_read *read, const app_scene_authoring *scene)
{
    /* Match instance_behavior_names' source registry, including during play. */
    const source_registry *sources = scene->live->runtime->sources;
    int count = source_attachment_count(sources);
    for (int i = 0; i < count; i++) {
        source_attachment_info info = source_attachment_at(sources, i);
        apush(
            read->attachments,
            ((studio_scene_read_attachment){
                .entity = info.entity,
                .sequence = info.sequence,
                .name = copy_text(read, info.name),
                .begin = info.begin,
            })
        );
    }
}

studio_scene_read *studio_scene_read_capture(
    studio_scene_read_scope *scope,
    studio *s,
    char *error,
    size_t error_size
)
{
    assert(scope && s && (!error_size || error));
    if (!scope_matches(scope, s)) {
        if (error_size) {
            snprintf(error, error_size, "Scene read requires its active Studio scope.");
        }
        return NULL;
    }
    app_scene_authoring *scene = s->scene;
    studio_scene_read *read = xcalloc(1, sizeof *read);
    int count = acount(scene->actors);
    asetlen(read->actors, count);
    /* sync_view may replace cached text. Finish it before borrowing any raw
     * actor, prototype or source text. Its actual path invokes no callbacks. */
    for (int i = 0; i < count; i++) {
        read->actors[i] = (studio_scene_read_actor){
            .view_flags = app_scene_authoring_actor_view(scene, i),
        };
    }
    read->snapshot = capture_nodes(scene, scope->owner, error, error_size);
    if (!read->snapshot) {
        studio_scene_read_free(read);
        return NULL;
    }
    capture_prototypes(read, s);
    for (int i = 0; i < count; i++) {
        const scene_actor *actor = &scene->actors[i];
        studio_scene_read_actor *captured = &read->actors[i];
        captured->identity = actor->identity;
        captured->prototype_name = copy_text(read, actor->prototype_name);
        captured->prototype = captured_prototype_index(read, actor->prototype_name);
        captured->selected = scene_instance_selected(scene, i);
        captured->field_first = acount(read->fields);
        capture_fields(read, scene, actor->entity);
        captured->field_count = acount(read->fields) - captured->field_first;
    }
    capture_attachments(read, scene);
    int node_count;
    const authoring_scene_node *nodes = authoring_scene_snapshot_nodes(read->snapshot, &node_count);
    assert(node_count == count);
    read->facts = (studio_scene_read_facts){
        .nodes = nodes,
        .actors = read->actors,
        .actor_count = count,
        .prototypes = read->prototypes,
        .prototype_count = acount(read->prototypes),
        .components = read->components,
        .component_count = acount(read->components),
        .fields = read->fields,
        .field_count = acount(read->fields),
        .attachments = read->attachments,
        .attachment_count = acount(read->attachments),
        .active = scene->selected,
        .rename = ecs_entity_from_u64(s->rename.entity),
        .rename_outliner = s->rename.outliner,
        .editable = studio_editable(s),
        .live = studio_live(s),
        .filter = copy_text(read, s->outliner_filter),
        .scene_path = copy_text(read, scene->scene_path),
    };
    return read;
}

const studio_scene_read_facts *studio_scene_read_get(const studio_scene_read *read)
{
    assert(read);
    return &read->facts;
}

void studio_scene_read_free(studio_scene_read *read)
{
    assert(read);
    if (read->snapshot) {
        authoring_scene_snapshot_free(read->snapshot);
    }
    afree(read->actors);
    afree(read->prototypes);
    afree(read->components);
    afree(read->fields);
    afree(read->attachments);
    arena_free(&read->text);
    free(read);
}
