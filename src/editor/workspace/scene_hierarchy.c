#include "authoring/scene_hierarchy.h"

#include "editor/collab/actions_scene.h"
#include "editor/collab/collab.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/transform_propagation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same_entity(ecs_entity a, ecs_entity b)
{
    return ecs_entity_to_u64(a) == ecs_entity_to_u64(b);
}

static int node_index(const authoring_scene_node *nodes, int count, ecs_entity entity)
{
    for (int i = 0; entity.slot && i < count; i++) {
        if (same_entity(nodes[i].entity, entity)) {
            return i;
        }
    }
    return -1;
}

static authoring_scene_node *capture_nodes(const app_scene_authoring *scene)
{
    authoring_scene_node *nodes = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        const scene_actor *actor = &scene->actors[i];
        const GameTransform
            *position = GAME_GameTransform_get_const(scene->world, actor->entity);
        const GameOrientation
            *orientation = GAME_GameOrientation_get_const(scene->world, actor->entity);
        const GameTransform3D
            *spatial = GAME_GameTransform3D_get_const(scene->world, actor->entity);
        ecs_entity parent = ecs_parent(scene->world, actor->entity);
        int parent_index = node_index(nodes, acount(nodes), parent);
        if (parent.slot && parent_index < 0) {
            /* The SDK rejects parents outside the authored preorder. */
            parent_index = acount(scene->actors);
        }
        authoring_scene_node node = { .entity = actor->entity,
                                      .name = actor->name,
                                      .parent = parent_index,
                                      .has_transform_2d = position != NULL,
                                      .has_orientation = orientation != NULL,
                                      .has_transform_3d = spatial != NULL };
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
    return nodes;
}

static av_value *pose_values(av_doc *doc, const float *values, int count)
{
    av_value *array = av_arr(doc);
    for (int i = 0; i < count; i++) {
        av_arr_append(array, av_real(doc, values[i]));
    }
    return array;
}

static av_value *capture_hierarchy(
    app_scene_authoring *scene,
    const authoring_scene_node *nodes,
    const uint64_t *selection,
    int count,
    uint64_t active
)
{
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_value *actors = av_obj_add_arr(doc, root, "actors");
    av_value *selected = av_obj_add_arr(doc, root, "selection");
    av_obj_add_uint(doc, root, "active", active);
    for (int i = 0; i < count; i++) {
        av_arr_append(selected, av_uint(doc, selection[i]));
    }
    for (int i = 0; i < acount(nodes); i++) {
        const authoring_scene_node *node = &nodes[i];
        av_value *item = av_arr_add_obj(doc, actors);
        av_obj_add_uint(doc, item, "id", scene_actor_identity(scene, node->entity));
        av_obj_add_int(doc, item, "parent", node->parent);
        if (node->has_transform_2d) {
            float values[] = { node->position.x, node->position.y };
            av_obj_add_val(doc, item, "position", pose_values(doc, values, 2));
        }
        if (node->has_orientation) {
            av_obj_add_val(
                doc,
                item,
                "orientation",
                av_real(doc, node->orientation.radians)
            );
        }
        if (node->has_transform_3d) {
            const GameTransform3D *t = &node->transform_3d;
            float values[] = { t->position.x, t->position.y, t->position.z,
                               t->rotation.x, t->rotation.y, t->rotation.z,
                               t->rotation.w, t->scale.x,    t->scale.y,
                               t->scale.z };
            av_obj_add_val(doc, item, "spatial", pose_values(doc, values, 10));
        }
    }
    av_value *copy = av_value_copy(root);
    av_doc_free(doc);
    return copy;
}

static bool read_pose(av_value *array, float *values, int count)
{
    if (!av_is_arr(array) || av_arr_size(array) != (size_t)count) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        av_value *value = av_arr_get(array, i);
        if (!av_is_num(value) || !isfinite(av_get_num(value)) ||
            !isfinite((float)av_get_num(value))) {
            return false;
        }
        values[i] = (float)av_get_num(value);
    }
    return true;
}

static bool read_nodes(app_scene_authoring *scene, av_value *root, authoring_scene_node **nodes)
{
    av_value *actors = av_obj_get(root, "actors");
    if (!av_is_arr(actors) || av_arr_size(actors) != (size_t)acount(scene->actors)) {
        return false;
    }
    for (int i = 0; i < acount(scene->actors); i++) {
        av_value *item = av_arr_get(actors, i);
        av_value *identity = av_obj_get(item, "id"),
                 *parent = av_obj_get(item, "parent");
        scene_actor *actor = av_is_uint(identity)
                               ? scene_actor_by_identity(scene, av_get_uint(identity))
                               : NULL;
        if (!actor || !av_is_int(parent) || av_get_sint(parent) < -1 ||
            av_get_sint(parent) >= i) {
            return false;
        }
        authoring_scene_node node = { .entity = actor->entity,
                                      .name = actor->name,
                                      .parent = av_get_int(parent) };
        av_value *position = av_obj_get(item, "position");
        if (position) {
            float values[2];
            if (!read_pose(position, values, 2)) {
                return false;
            }
            node.has_transform_2d = true;
            node.position = (GameTransform){ values[0], values[1] };
        }
        av_value *orientation = av_obj_get(item, "orientation");
        if (orientation) {
            if (!av_is_num(orientation) || !isfinite(av_get_num(orientation))) {
                return false;
            }
            node.has_orientation = true;
            node.orientation.radians = (float)av_get_num(orientation);
        }
        av_value *spatial = av_obj_get(item, "spatial");
        if (spatial) {
            float values[10];
            if (!read_pose(spatial, values, 10)) {
                return false;
            }
            node.has_transform_3d = true;
            node.transform_3d = (GameTransform3D){
                .position = { values[0], values[1], values[2] },
                .rotation = { values[3], values[4], values[5], values[6] },
                .scale = { values[7], values[8], values[9] }
            };
        }
        apush(*nodes, node);
    }
    return authoring_scene_validate(
               *nodes,
               acount(*nodes),
               scene->message,
               sizeof scene->message
           ) == 0;
}

static void record_pose(
    ecs_world *world,
    ecs_entity entity,
    ecs_component_id component,
    bool present,
    const void *value
)
{
    void *current = ecs_component_data(world, entity, component);
    int result = ECS_OK;
    if (present && current) {
        size_t size = ecs_world_component(world, component)->element_size;
        if (!memcmp(current, value, size)) {
            return;
        }
        memcpy(current, value, size);
        result = ecs_component_modified(world, entity, component);
        assert(result == ECS_OK);
        return;
    }
    if (present) {
        result = ecs_world_record_add(world, entity, component, value, 2);
    } else if (current) {
        result = ecs_world_record_remove(world, entity, component, 2);
    }
    assert(result == ECS_OK);
}

/* Validation precedes every write. The detached phase permits sibling names
 * that occur at another level and avoids transient cycles during replay. */
static void apply_nodes(app_scene_authoring *scene, const authoring_scene_node *nodes)
{
    assert(ecs_world_command_count(scene->world) == 0);
    bool *named = NULL;
    for (int i = 0; i < acount(nodes); i++) {
        const authoring_scene_node *node = &nodes[i];
        ecs_entity old = ecs_parent(scene->world, node->entity);
        ecs_entity parent = node->parent < 0 ? (ecs_entity){ 0 }
                                             : nodes[node->parent].entity;
        bool changed = !same_entity(old, parent);
        bool has_name = changed && ecs_entity_name(scene->world, node->entity) != NULL;
        apush(named, has_name);
        if (has_name) {
            int result = ecs_world_record_remove(scene->world, node->entity, ECS_NAME, 0);
            assert(result == ECS_OK);
        }
        if (changed && old.slot) {
            int result = ecs_world_record_remove_pair(scene->world, node->entity, ECS_CHILD_OF, old, 1);
            assert(result == ECS_OK);
        }
    }
    int result = ecs_world_apply_commands(scene->world);
    assert(result == ECS_OK);
    scene_actor *ordered = NULL;
    for (int i = 0; i < acount(nodes); i++) {
        const authoring_scene_node *node = &nodes[i];
        ecs_entity parent = node->parent < 0 ? (ecs_entity){ 0 }
                                             : nodes[node->parent].entity;
        if (parent.slot &&
            !same_entity(ecs_parent(scene->world, node->entity), parent)) {
            result = ecs_world_record_add_pair(scene->world, node->entity, ECS_CHILD_OF, parent, 0);
            assert(result == ECS_OK);
        }
        if (named[i]) {
            result = ecs_world_record_name(scene->world, node->entity, node->name, 1);
            assert(result == ECS_OK);
        }
        record_pose(
            scene->world,
            node->entity,
            GAME_COMPONENT_GameTransform.id,
            node->has_transform_2d,
            &node->position
        );
        record_pose(
            scene->world,
            node->entity,
            GAME_COMPONENT_GameOrientation.id,
            node->has_orientation,
            &node->orientation
        );
        record_pose(
            scene->world,
            node->entity,
            GAME_COMPONENT_GameTransform3D.id,
            node->has_transform_3d,
            &node->transform_3d
        );
        scene_actor *actor = scene_actor_by_identity(scene, scene_actor_identity(scene, node->entity));
        assert(actor);
        apush(ordered, *actor);
    }
    result = ecs_world_apply_commands(scene->world);
    assert(result == ECS_OK);
    for (int i = 0; i < acount(nodes); i++) {
        if (nodes[i].parent < 0) {
            continue;
        }
        int index = 0;
        for (int j = 0; j < i; j++) {
            index += nodes[j].parent == nodes[i].parent;
        }
        result = ecs_world_record_child_index(scene->world, nodes[i].entity, index, (uint64_t)i);
        assert(result == ECS_OK);
    }
    result = ecs_world_apply_commands(scene->world);
    assert(result == ECS_OK);
    afree(named);
    afree(scene->actors);
    scene->actors = ordered;
    result = game_transform_propagate(scene->world);
    assert(result == ECS_OK);
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
}

int scene_restore_hierarchy(app_scene_authoring *scene, av_value *value)
{
    if (!scene_editing(scene)) {
        snprintf(scene->message, sizeof scene->message, "Stop the scene before moving objects.");
        return -1;
    }
    authoring_scene_node *nodes = NULL;
    av_value *selection = av_obj_get(value, "selection"),
             *active = av_obj_get(value, "active");
    bool valid = av_is_arr(selection) && av_is_uint(active) &&
                 read_nodes(scene, value, &nodes);
    bool active_found = false;
    for (size_t i = 0; valid && i < av_arr_size(selection); i++) {
        av_value *id = av_arr_get(selection, i);
        valid = av_is_uint(id) && scene_actor_by_identity(scene, av_get_uint(id));
        active_found |= av_get_uint(id) == av_get_uint(active);
    }
    if (!valid || !active_found) {
        afree(nodes);
        snprintf(scene->message, sizeof scene->message, "The hierarchy history no longer matches this scene.");
        return -1;
    }
    apply_nodes(scene, nodes);
    afree(nodes);
    scene->selected = -1;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        actor->marked = false;
        for (size_t j = 0; j < av_arr_size(selection); j++) {
            actor->marked |= actor->identity == av_get_uint(av_arr_get(selection, j));
        }
        if (actor->identity == av_get_uint(active)) {
            scene->selected = i;
        }
    }
    scene->runtime_selected = (ecs_entity){ 0 };
    sync_editor_inspection(scene);
    return 0;
}

int scene_move_actors(
    app_scene_authoring *scene,
    const uint64_t *identities,
    int count,
    uint64_t active,
    uint64_t parent,
    uint64_t before
)
{
    if (!scene_editing(scene) || count <= 0 || count > acount(scene->actors)) {
        snprintf(scene->message, sizeof scene->message, "Stop the scene and select objects before moving them.");
        return -1;
    }
    authoring_scene_node *nodes = capture_nodes(scene);
    ecs_entity *selected = NULL;
    bool valid = false, active_found = false;
    for (int i = 0; i < count; i++) {
        scene_actor *actor = scene_actor_by_identity(scene, identities[i]);
        if (!actor) {
            snprintf(scene->message, sizeof scene->message, "A selected object no longer exists.");
            goto done;
        }
        apush(selected, actor->entity);
        active_found |= actor->identity == active;
    }
    scene_actor *parent_actor = scene_actor_by_identity(scene, parent);
    scene_actor *before_actor = scene_actor_by_identity(scene, before);
    if (!active_found || (parent && !parent_actor) || (before && !before_actor)) {
        snprintf(scene->message, sizeof scene->message, "The move selection or destination no longer exists.");
        goto done;
    }
    av_value *previous = capture_hierarchy(scene, nodes, identities, count, active);
    valid = authoring_scene_move(
                nodes,
                acount(nodes),
                selected,
                count,
                parent_actor ? parent_actor->entity : (ecs_entity){ 0 },
                before_actor ? before_actor->entity : (ecs_entity){ 0 },
                true,
                scene->message,
                sizeof scene->message
            ) == 0;
    if (valid) {
        av_value *next = capture_hierarchy(scene, nodes, identities, count, active);
        if (!av_equals(previous, next)) {
            finish_transform_edit(scene, false);
            valid = scene_restore_hierarchy(scene, next) == 0;
            if (valid && collab_record_history()) {
                scene_history_push_hierarchy(scene->history, previous, next);
            }
        }
        av_value_free(next);
    }
    av_value_free(previous);
done:
    afree(nodes);
    afree(selected);
    return valid ? 0 : -1;
}

static bool destination(
    app_scene_authoring *scene,
    ecs_entity target,
    app_scene_drop drop,
    ecs_entity *parent,
    ecs_entity *before
)
{
    *parent = (ecs_entity){ 0 };
    *before = (ecs_entity){ 0 };
    if (drop == APP_SCENE_DROP_ROOT) {
        return !target.slot;
    }
    int at = -1;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (same_entity(scene->actors[i].entity, target)) {
            at = i;
        }
    }
    if (at < 0) {
        return false;
    }
    if (drop == APP_SCENE_DROP_INSIDE) {
        *parent = target;
        return true;
    }
    *parent = ecs_parent(scene->world, target);
    if (drop == APP_SCENE_DROP_BEFORE) {
        *before = target;
        return true;
    }
    if (drop != APP_SCENE_DROP_AFTER) {
        return false;
    }
    for (int i = scene_actor_subtree_end(scene, at); i < acount(scene->actors); i++) {
        if (same_entity(ecs_parent(scene->world, scene->actors[i].entity), *parent)) {
            *before = scene->actors[i].entity;
            break;
        }
    }
    return true;
}

static uint64_t *selected_ids(app_scene_authoring *scene)
{
    uint64_t *ids = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i)) {
            apush(ids, scene->actors[i].identity);
        }
    }
    return ids;
}

bool app_scene_authoring_can_drop(app_scene_authoring *scene, ecs_entity target, app_scene_drop drop)
{
    ecs_entity parent, before;
    if (!scene_editing(scene) || !destination(scene, target, drop, &parent, &before)) {
        return false;
    }
    authoring_scene_node *nodes = capture_nodes(scene);
    ecs_entity *selected = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i)) {
            apush(selected, scene->actors[i].entity);
        }
    }
    char error[256];
    bool valid = authoring_scene_move(
                     nodes,
                     acount(nodes),
                     selected,
                     acount(selected),
                     parent,
                     before,
                     true,
                     error,
                     sizeof error
                 ) == 0;
    afree(selected);
    afree(nodes);
    return valid;
}

int app_scene_authoring_drop_selection(app_scene_authoring *scene, ecs_entity target, app_scene_drop drop)
{
    ecs_entity parent, before;
    if (!scene_editing(scene) || !destination(scene, target, drop, &parent, &before)) {
        snprintf(scene->message, sizeof scene->message, "Stop the scene and choose a valid destination.");
        return -1;
    }
    uint64_t *ids = selected_ids(scene);
    scene_actor *actor = selected_actor(scene);
    uint64_t active = actor ? actor->identity : 0;
    uint64_t parent_id = scene_actor_identity(scene, parent),
             before_id = scene_actor_identity(scene, before);
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "hierarchy.move", &doc);
    yyjson_mut_val *selection = yyjson_mut_obj_add_arr(doc, root, "actors");
    for (int i = 0; i < acount(ids); i++) {
        yyjson_mut_arr_add_uint(doc, selection, ids[i]);
    }
    yyjson_mut_obj_add_uint(doc, root, "active", active);
    yyjson_mut_obj_add_uint(doc, root, "parent", parent_id);
    yyjson_mut_obj_add_uint(doc, root, "before", before_id);
    char *action = scene_action_write(doc);
    collab_action_gate gate = action ? scene_action_begin(scene, action)
                                     : COLLAB_ACTION_REJECT;
    int result = gate == COLLAB_ACTION_PENDING ? 1 : -1;
    if (gate == COLLAB_ACTION_APPLY) {
        result = scene_move_actors(scene, ids, acount(ids), active, parent_id, before_id);
        scene_action_finish(scene, action, result == 0);
    }
    free(action);
    afree(ids);
    return result;
}
