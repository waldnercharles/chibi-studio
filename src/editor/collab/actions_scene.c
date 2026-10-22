#include "editor/collab/actions_scene.h"

#include "editor/collab/collab.h"
#include "editor/collab/protocol.h"
#include "editor/documents/prototype_source_input.h"
#include "graph/model/registry.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_json.h"
#include "support/xalloc.h"
#include "types/array.h"
#include "types/type.h"
#include "types/value_document.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

yyjson_mut_val *scene_action_root(app_scene_authoring *scene, const char *operation, yyjson_mut_doc **out_doc)
{
    assert(scene && operation && out_doc);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", 1);
    yyjson_mut_obj_add_strcpy(doc, root, "family", "scene");
    yyjson_mut_obj_add_strcpy(doc, root, "op", operation);
    const char *tab = scene->active_tab >= 0 && scene->active_tab < acount(scene->tabs)
                        ? scene->tabs[scene->active_tab].key
                        : scene_tabs_current_scene_key(scene);
    if (tab) { yyjson_mut_obj_add_strcpy(doc, root, "tab", tab); }
    app_editor_document_context document = app_scene_authoring_document(scene);
    yyjson_mut_obj_add_int(doc, root, "document_kind", document.kind);
    if (document.id) {
        yyjson_mut_obj_add_strcpy(doc, root, "document_id", document.id);
    }
    *out_doc = doc;
    return root;
}

char *scene_action_write(yyjson_mut_doc *doc)
{
    if (!doc) { return NULL; }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

collab_action_gate scene_action_begin(app_scene_authoring *scene, const char *json)
{
    assert(scene && json);
    if (collab_replaying()) { return COLLAB_ACTION_APPLY; }
    if (scene->collab_action_depth) {
        scene->collab_action_depth++;
        return COLLAB_ACTION_APPLY;
    }
    collab_action_gate gate = collab_actions_begin(json);
    if (gate == COLLAB_ACTION_APPLY) { scene->collab_action_depth = 1; }
    return gate;
}

bool scene_action_finish(app_scene_authoring *scene, const char *json, bool success)
{
    assert(scene && json);
    if (collab_replaying()) { return success; }
    if (!scene->collab_action_depth) { return success; }
    if (--scene->collab_action_depth) { return success; }
    return collab_actions_finish(json, success);
}

bool scene_collab_is_editor(void)
{
    return collab_active() && !collab_replaying() && !collab_is_host() &&
           collab_can_edit();
}

bool scene_collab_can_write_locally(void)
{
    return !collab_active() || collab_replaying() ||
           (collab_can_edit() && !scene_collab_is_editor());
}

uint64_t scene_actor_next_identity(app_scene_authoring *scene)
{
    assert(scene);
    if (!scene->next_actor_id) { scene->next_actor_id = 1; }
    return scene->next_actor_id;
}

uint64_t scene_actor_allocate_identity(app_scene_authoring *scene)
{
    uint64_t identity = scene_actor_next_identity(scene);
    assert(identity < UINT64_MAX);
    scene->next_actor_id++;
    return identity;
}

uint64_t scene_actor_identity(const app_scene_authoring *scene, ecs_entity entity)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (ecs_entity_to_u64(scene->actors[i].entity) == ecs_entity_to_u64(entity)) {
            return scene->actors[i].identity;
        }
    }
    return 0;
}

scene_actor *scene_actor_by_identity(app_scene_authoring *scene, uint64_t identity)
{
    if (!scene || !identity) { return NULL; }
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene->actors[i].identity == identity) { return &scene->actors[i]; }
    }
    return NULL;
}

void scene_actor_set_identity(app_scene_authoring *scene, scene_actor *actor, uint64_t identity)
{
    assert(scene && actor && identity);
    for (int i = 0; i < acount(scene->actors); i++) {
        assert(&scene->actors[i] == actor || scene->actors[i].identity != identity);
    }
    actor->identity = identity;
    if (scene->next_actor_id <= identity) {
        assert(identity < UINT64_MAX);
        scene->next_actor_id = identity + 1;
    }
}

static bool fail(char *error, size_t size, const char *message)
{
    if (size) { snprintf(error, size, "%s", message); }
    return false;
}

static const char *text(yyjson_val *object, const char *name)
{
    yyjson_val *value = yyjson_obj_get(object, name);
    const char *result = yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
    return result && yyjson_get_len(value) == strlen(result) ? result : NULL;
}

static int tab_index(const app_scene_authoring *scene, const char *key)
{
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene->tabs[i].key && !strcmp(scene->tabs[i].key, key)) {
            return i;
        }
    }
    return -1;
}

static bool activate_action_tab(app_scene_authoring *scene, yyjson_val *root, char *error, size_t size)
{
    const char *key = text(root, "tab");
    int index = key ? tab_index(scene, key) : -1;
    if (index < 0 || scene_tabs_activate(scene, index) != 0) {
        return fail(error, size, "Scene action references an unavailable tab");
    }
    int kind = yyjson_get_int(yyjson_obj_get(root, "document_kind"));
    const char *id = text(root, "document_id");
    app_editor_document_context active = app_scene_authoring_document(scene);
    if (id && ((int)active.kind != kind || !active.id || strcmp(active.id, id))) {
        return fail(error, size, "Scene action document identity does not match its tab");
    }
    return true;
}

static scene_actor *action_actor(app_scene_authoring *scene, yyjson_val *root, char *error, size_t size)
{
    yyjson_val *id = yyjson_obj_get(root, "actor");
    if (!yyjson_is_uint(id) || !yyjson_get_uint(id)) {
        fail(error, size, "Scene action has no valid actor identity");
        return NULL;
    }
    scene_actor *actor = scene_actor_by_identity(scene, yyjson_get_uint(id));
    if (!actor) { fail(error, size, "Scene action actor no longer exists"); }
    return actor;
}

static const ecs_component_desc *component_named(app_scene_authoring *scene, const char *name)
{
    for (int i = 0; i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        if (desc && name && !strcmp(desc->name, name)) { return desc; }
    }
    return NULL;
}

static bool read_plain_value(pin_type type, const char *type_name, yyjson_val *json, pin_value *out)
{
    const type_desc *shape = type == PIN_STRUCT ? type_catalog_find(type_name) : NULL;
    if (type == PIN_STRUCT &&
        (!shape || !type_is_plain(shape) || !yyjson_is_obj(json))) {
        return false;
    }
    av_doc *doc = av_doc_new();
    av_value *document = av_from_json(doc, json);
    arena storage = { 0 };
    pin_value value;
    char error[192];
    bool valid = !pin_value_read_document(
        &storage,
        (pin_value){ .type = type, .type_name = type_name },
        document,
        &value,
        error,
        sizeof error
    );
    if (valid) {
        *out = value;
        out->type_name = type_name;
        if (type == PIN_STR || type == PIN_NAME) {
            valid = pin_value_copy(out, &value) == 0;
            out->type_name = type_name;
        }
        if (type == PIN_STRUCT) {
            out->ptr = xmalloc(shape->size);
            memcpy(out->ptr, value.ptr, shape->size);
        }
    }
    arena_free(&storage);
    av_doc_free(doc);
    return valid;
}

static void free_action_value(pin_value *value)
{
    if (value->type == PIN_STR || value->type == PIN_NAME) {
        pin_value_destroy(value);
    }
    if (value->type == PIN_STRUCT) { free(value->ptr); }
    *value = (pin_value){ 0 };
}

static bool read_field_value(
    const ecs_component_desc *component,
    const char *field_name,
    yyjson_val *json,
    int *index,
    pin_value *out
)
{
    if (!out) { return false; }
    *out = (pin_value){ 0 };
    if (!component || !component->type || !field_name || !json) {
        return false;
    }
    int field = 0;
    while (field < component->type->field_count &&
           strcmp(component->type->fields[field].name, field_name)) {
        field++;
    }
    if (field == component->type->field_count) { return false; }
    const type_field *schema = &component->type->fields[field];
    *out = (pin_value){ 0 };
    if (!read_plain_value(schema->type, schema->type_name, json, out) ||
        !editable_scalar(out)) {
        free_action_value(out);
        return false;
    }
    *index = field;
    return true;
}

static bool apply_property(app_scene_authoring *scene, yyjson_val *root, char *error, size_t size)
{
    scene_actor *actor = action_actor(scene, root, error, size);
    const char *component_name = text(root, "component");
    const char *field_name = text(root, "field");
    const ecs_component_desc *component = component_named(scene, component_name);
    pin_value value = { 0 };
    int field = -1;
    bool read = actor &&
                read_field_value(component, field_name, yyjson_obj_get(root, "value"), &field, &value);
    bool applied = read &&
                   ecs_component_has(scene->world, actor->entity, component->id) &&
                   ecs_component_write_field(
                       scene->world,
                       actor->entity,
                       component->id,
                       field,
                       value
                   ) == ECS_OK;
    free_action_value(&value);
    if (!applied) {
        return fail(error, size, "Scene property action failed validation or application");
    }
    scene->scene_dirty = true;
    asset_index_invalidate(scene);
    return true;
}

static bool prototype_edit_accepted(int result)
{ return result == 0 || result == APP_PROTOTYPE_EDIT_NOT_APPLIED; }

static bool apply_prototype_field(app_scene_authoring *scene, yyjson_val *root, char *error, size_t size)
{
    const char *prototype = text(root, "prototype");
    const char *component_name = text(root, "component");
    const char *field_name = text(root, "field");
    scene_prototype_asset *asset = prototype ? find_prototype(scene, prototype) : NULL;
    prototype_component *component = NULL;
    if (asset) {
        for (int i = 0; i < acount(asset->prototype->components); i++) {
            if (!strcmp(asset->prototype->components[i].desc->name, component_name)) {
                component = &asset->prototype->components[i];
                break;
            }
        }
    }
    int field = -1;
    pin_value value = { 0 };
    bool read = component &&
                read_field_value(component->desc, field_name, yyjson_obj_get(root, "value"), &field, &value);
    bool applied = read &&
                   prototype_edit_accepted(
                       app_scene_authoring_set_prototype_field(scene, prototype, component_name, field_name, value)
                   );
    free_action_value(&value);
    if (!applied) {
        return fail(error, size, "Prototype field action failed validation or application");
    }
    return true;
}

static bool apply_hierarchy_move(app_scene_authoring *scene, yyjson_val *root, char *error, size_t size)
{
    yyjson_val *actors = yyjson_obj_get(root, "actors");
    yyjson_val *active = yyjson_obj_get(root, "active");
    yyjson_val *parent = yyjson_obj_get(root, "parent");
    yyjson_val *before = yyjson_obj_get(root, "before");
    if (!yyjson_is_arr(actors) || !yyjson_arr_size(actors) ||
        yyjson_arr_size(actors) > (size_t)acount(scene->actors) ||
        !yyjson_is_uint(active) || !yyjson_is_uint(parent) ||
        !yyjson_is_uint(before)) {
        return fail(error, size, "Hierarchy move has no valid selection or destination");
    }
    uint64_t *ids = NULL;
    size_t i, count;
    yyjson_val *item;
    yyjson_arr_foreach(actors, i, count, item)
    {
        if (!yyjson_is_uint(item) || !yyjson_get_uint(item)) {
            afree(ids);
            return fail(error, size, "Hierarchy move has an invalid object identity");
        }
        apush(ids, yyjson_get_uint(item));
    }
    int result = scene_move_actors(
        scene,
        ids,
        acount(ids),
        yyjson_get_uint(active),
        yyjson_get_uint(parent),
        yyjson_get_uint(before)
    );
    afree(ids);
    return result == 0 || fail(error, size, app_scene_authoring_message(scene));
}

static bool apply_tab_action(
    app_scene_authoring *scene,
    const char *operation,
    yyjson_val *root,
    char *error,
    size_t size
)
{
    if (!strcmp(operation, "tab.activate")) {
        const char *key = text(root, "tab");
        int index = key ? tab_index(scene, key) : -1;
        return index >= 0 && scene_tabs_activate(scene, index) == 0
                 ? true
                 : fail(error, size, "Could not activate the requested tab");
    }
    if (!strcmp(operation, "tab.open_scene")) {
        const char *path = text(root, "path");
        return path && scene_tabs_open_scene(scene, path) == 0
                 ? true
                 : fail(error, size, "Could not open the requested scene tab");
    }
    if (!strcmp(operation, "tab.open_ui")) {
        const char *path = text(root, "path");
        return path && scene_tabs_open_ui(scene, path) == 0
                 ? true
                 : fail(error, size, "Could not open the requested UI tab");
    }
    if (!strcmp(operation, "tab.open_prototype")) {
        const char *name = text(root, "prototype");
        return name && app_scene_authoring_open_prototype(scene, name) == 0
                 ? true
                 : fail(error, size, "Could not open the requested prototype tab");
    }
    if (!strcmp(operation, "tab.open_behavior")) {
        const char *name = text(root, "behavior");
        return name && app_scene_authoring_open_behavior(scene, name) == 0
                 ? true
                 : fail(error, size, "Could not open the requested behavior tab");
    }
    if (!strcmp(operation, "tab.open_system")) {
        const char *path = text(root, "path");
        return path && app_scene_authoring_open_system(scene, path) == 0
                 ? true
                 : fail(error, size, "Could not open the requested system tab");
    }
    if (!strcmp(operation, "tab.close")) {
        const char *id = text(root, "target_id");
        int kind = yyjson_get_int(yyjson_obj_get(root, "target_kind"));
        if (id && kind >= APP_EDITOR_DOCUMENT_SCENE &&
            kind <= APP_EDITOR_DOCUMENT_RELATIONSHIP) {
            scene_tabs_close(scene, (app_editor_document_kind)kind, id);
            return true;
        }
        return fail(error, size, "Tab close action is incomplete");
    }
    return fail(error, size, "Unknown scene tab action");
}

bool scene_collab_apply_action(app_scene_authoring *scene, const char *json, char *error, size_t error_size)
{
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *family = text(root, "family");
    const char *operation = text(root, "op");
    bool ok = false;
    if (!scene || !yyjson_is_obj(root) ||
        yyjson_get_int(yyjson_obj_get(root, "version")) != 1 || !family ||
        strcmp(family, "scene") || !operation) {
        fail(error, error_size, "Invalid scene action");
        goto done;
    }
    if (!strcmp(operation, "prototype.source")) {
        if (strlen(json) > COLLAB_MAX_ACTION) {
            fail(error, error_size, "Prototype source action exceeds the room action limit");
            goto done;
        }
        ok = scene_source_input_apply(scene, root, error, error_size);
        goto done;
    }
    if (!strncmp(operation, "tab.", 4)) {
        ok = apply_tab_action(scene, operation, root, error, error_size);
        goto done;
    }
    if (!activate_action_tab(scene, root, error, error_size)) { goto done; }
    if (!strcmp(operation, "hierarchy.move")) {
        ok = apply_hierarchy_move(scene, root, error, error_size);
    } else if (!strcmp(operation, "instance.add")) {
        const char *prototype = text(root, "prototype");
        const char *name = text(root, "name");
        yyjson_val *x = yyjson_obj_get(root, "x"), *y = yyjson_obj_get(root, "y");
        uint64_t identity = yyjson_get_uint(yyjson_obj_get(root, "actor"));
        Transform position = { 0 };
        Transform *position_ptr = NULL;
        if (yyjson_is_num(x) || yyjson_is_num(y)) {
            if (!yyjson_is_num(x) || !yyjson_is_num(y) ||
                !isfinite(yyjson_get_num(x)) || !isfinite(yyjson_get_num(y))) {
                fail(error, error_size, "Instance position is invalid");
                goto done;
            }
            position = (Transform){ (float)yyjson_get_num(x),
                                    (float)yyjson_get_num(y) };
            position_ptr = &position;
        }
        if (!prototype || !name || !identity ||
            identity != scene_actor_next_identity(scene) ||
            add_instance_at(scene, prototype, position_ptr) != 0) {
            fail(error, error_size, "Could not add the requested scene instance");
            goto done;
        }
        scene_actor *actor = &alast(scene->actors);
        if (strcmp(actor->name, name)) {
            fail(error, error_size, "Scene instance name differs from the accepted action");
            goto done;
        }
        ok = actor->identity == identity;
    } else if (!strcmp(operation, "instance.duplicate")) {
        scene_actor *source = action_actor(scene, root, error, error_size);
        uint64_t identity = yyjson_get_uint(yyjson_obj_get(root, "created_actor"));
        if (!source || !identity || identity != scene_actor_next_identity(scene)) {
            fail(error, error_size, "Could not resolve the requested duplicate source");
            goto done;
        }
        select_instance(scene, (int)(source - scene->actors));
        ok = app_scene_authoring_duplicate_instance(scene) == 0 &&
             acount(scene->actors) && alast(scene->actors).identity == identity;
        if (!ok) {
            fail(error, error_size, "Could not duplicate the requested scene instance");
        }
    } else if (!strcmp(operation, "instance.delete")) {
        scene_actor *actor = action_actor(scene, root, error, error_size);
        if (!actor) { goto done; }
        select_instance(scene, (int)(actor - scene->actors));
        ok = app_scene_authoring_delete_instance(scene) == 0;
        if (!ok) {
            fail(error, error_size, "Could not delete the requested scene instance");
        }
    } else if (!strcmp(operation, "instance.rename")) {
        scene_actor *actor = action_actor(scene, root, error, error_size);
        const char *name = text(root, "name");
        ok = actor && name &&
             app_scene_authoring_rename_instance(scene, actor->entity, name) == 0;
        if (!ok) {
            fail(error, error_size, "Could not rename the requested scene instance");
        }
    } else if (!strcmp(operation, "instance.behavior")) {
        scene_actor *actor = action_actor(scene, root, error, error_size);
        const char *behavior = text(root, "behavior");
        yyjson_val *attached = yyjson_obj_get(root, "attached");
        if (!actor || !behavior || !yyjson_is_bool(attached)) {
            fail(error, error_size, "Instance behavior action is incomplete");
            goto done;
        }
        select_instance(scene, (int)(actor - scene->actors));
        ok = app_scene_authoring_set_instance_behavior(scene, behavior, yyjson_get_bool(attached)) ==
             0;
        if (!ok) {
            fail(error, error_size, "Could not update instance behavior");
        }
    } else if (!strcmp(operation, "instance.component_add") || !strcmp(operation, "instance.component_remove")) {
        scene_actor *actor = action_actor(scene, root, error, error_size);
        const char *component = text(root, "component");
        bool add = !strcmp(operation, "instance.component_add");
        if (!actor || !component) {
            fail(error, error_size, "Instance component action is incomplete");
            goto done;
        }
        select_instance(scene, (int)(actor - scene->actors));
        ok = (add ? app_scene_authoring_add_instance_component(scene, component)
                  : app_scene_authoring_remove_instance_component(scene, component)) == 0;
        if (!ok) {
            fail(error, error_size, "Could not update the requested instance component");
        }
    } else if (!strcmp(operation, "transform.set")) {
        scene_actor *actor = action_actor(scene, root, error, error_size);
        yyjson_val *x = yyjson_obj_get(root, "x"), *y = yyjson_obj_get(root, "y");
        if (!actor || !yyjson_is_num(x) || !yyjson_is_num(y) ||
            !isfinite(yyjson_get_num(x)) || !isfinite(yyjson_get_num(y))) {
            fail(error, error_size, "Transform action is incomplete");
            goto done;
        }
        finish_transform_edit(scene, false);
        Transform before = *Transform_get_const(scene->world, actor->entity);
        Transform after = { (float)yyjson_get_num(x), (float)yyjson_get_num(y) };
        write_position(scene, actor->entity, after);
        scene->scene_dirty = true;
        ok = true;
        (void)before;
    } else if (!strcmp(operation, "property.set")) {
        ok = apply_property(scene, root, error, error_size);
    } else if (!strcmp(operation, "prototype.field")) {
        ok = apply_prototype_field(scene, root, error, error_size);
    } else if (!strcmp(operation, "prototype.component_add")) {
        const char *prototype = text(root, "prototype"),
                   *component = text(root, "component");
        ok = prototype && component &&
             prototype_edit_accepted(
                 app_scene_authoring_add_prototype_component(scene, prototype, component)
             );
        if (!ok) {
            fail(error, error_size, "Could not add the requested prototype component");
        }
    } else if (!strcmp(operation, "prototype.component_remove")) {
        const char *prototype = text(root, "prototype"),
                   *component = text(root, "component");
        ok = prototype && component &&
             prototype_edit_accepted(
                 app_scene_authoring_remove_prototype_component(scene, prototype, component)
             );
        if (!ok) {
            fail(error, error_size, "Could not remove the requested prototype component");
        }
    } else if (!strcmp(operation, "prototype.behavior_attach") || !strcmp(operation, "prototype.behavior_detach")) {
        const char *prototype = text(root, "prototype"),
                   *behavior = text(root, "behavior");
        bool attach = !strcmp(operation, "prototype.behavior_attach");
        ok = prototype && behavior &&
             prototype_edit_accepted(
                 attach
                     ? app_scene_authoring_attach_prototype_behavior(scene, prototype, behavior)
                     : app_scene_authoring_detach_prototype_behavior(scene, prototype, behavior)
             );
        if (!ok) {
            fail(error, error_size, "Could not update prototype behavior");
        }
    } else if (!strcmp(operation, "system.replace")) {
        const char *system = text(root, "system");
        const char *path = text(root, "path");
        ok = system && load_system_document_text(scene, system, path, false) == 0;
        if (!ok) {
            fail(error, error_size, "Could not restore the requested system document");
        }
    } else {
        fail(error, error_size, "Unknown scene action");
    }
done:
    yyjson_doc_free(doc);
    return ok;
}
