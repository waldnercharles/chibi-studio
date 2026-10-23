#include "editor/documents/scene_history.h"

#include "assets/physics_settings.h"
#include "graph/model/registry.h"
#include "runtime/component_catalog.h"
#include "runtime/declared_components.h"
#include "support/array.h"
#include "support/asset_json.h"
#include "support/xalloc.h"
#include "types/array.h"
#include "types/text.h"
#include "types/type.h"
#include "types/value_document.h"

#include <assert.h>
#include <math.h>
#include <string.h>

typedef struct
{
    ecs_entity entity;
    uint64_t group;
    uint64_t step; /* the group's id, or this lone edit's own; 0 once restored */
    Transform before;
    Transform after;
    scene_field_edit *fields;
    scene_actor_edit *actor;
    scene_behavior_edit *behaviors;
    scene_prototype_edit *prototype;
    av_value *hierarchy_before, *hierarchy_after;
    bool physics;
    physics_settings physics_before, physics_after;
    char *name_before, *name_after; /* a rename, when name_after is set */
    scene_path_ref *renamed_paths;  /* the entity paths the rename rewrote */
} scene_edit;

struct scene_history
{
    scene_edit *undo;
    scene_edit *redo;
    uint64_t group;
    uint64_t next_group;
};

scene_history *scene_history_new()
{
    return xcalloc(1, sizeof(scene_history));
}

static void free_names(const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        free((void *)names[i]);
    }
    free((void *)names);
}

static void clear_edits(scene_edit **edits)
{
    for (int i = 0; i < acount(*edits); i++) {
        scene_field_edit *fields = (*edits)[i].fields;
        for (int j = 0; j < acount(fields); j++) {
            free((void *)fields[j].before.type_name);
            free((void *)fields[j].after.type_name);
            if (fields[j].before.type == PIN_STR || fields[j].before.type == PIN_NAME) {
                pin_value_destroy(&fields[j].before);
                pin_value_destroy(&fields[j].after);
            }
            if (fields[j].before.type == PIN_STRUCT) {
                free(fields[j].before.ptr);
                free(fields[j].after.ptr);
            }
        }
        afree(fields);
        scene_actor_edit *actor = (*edits)[i].actor;
        if (actor) {
            free((void *)actor->name);
            free((void *)actor->prototype_name);
            av_value_free(actor->base);
            av_value_free(actor->state);
            av_value_free(actor->retained);
            av_value_free(actor->paths);
            free(actor);
        }
        scene_prototype_edit *prototype = (*edits)[i].prototype;
        if (prototype) {
            free((void *)prototype->name);
            av_value_free(prototype->before);
            av_value_free(prototype->after);
            free((void *)prototype->source_before);
            free((void *)prototype->source_after);
            free(prototype);
        }
        scene_behavior_edit *behaviors = (*edits)[i].behaviors;
        if (behaviors) {
            free_names(behaviors->before, behaviors->before_count);
            free_names(behaviors->after, behaviors->after_count);
            free(behaviors);
        }
        av_value_free((*edits)[i].hierarchy_before);
        av_value_free((*edits)[i].hierarchy_after);
        free((*edits)[i].name_before);
        free((*edits)[i].name_after);
        afree((*edits)[i].renamed_paths);
    }
    aclear(*edits);
}

void scene_history_free(scene_history *history)
{
    if (!history) {
        return;
    }
    clear_edits(&history->undo);
    clear_edits(&history->redo);
    afree(history->undo);
    afree(history->redo);
    free(history);
}

void scene_history_clear(scene_history *history)
{
    assert(history);
    clear_edits(&history->undo);
    clear_edits(&history->redo);
    history->group = 0;
}

void scene_history_begin_group(scene_history *history)
{
    assert(history && !history->group && history->next_group != UINT64_MAX);
    history->group = ++history->next_group;
}

void scene_history_end_group(scene_history *history)
{
    assert(history && history->group);
    history->group = 0;
}

uint64_t scene_history_group_id(const scene_history *history, bool redo)
{
    assert(history);
    const scene_edit *edits = redo ? history->redo : history->undo;
    return acount(edits) ? alast(edits).group : 0;
}

/* A group is one step; an edit outside a group is a step of its own. Ids
   share the group counter, so they grow and never repeat. */
static uint64_t next_step(scene_history *history)
{
    if (history->group) {
        return history->group;
    }
    assert(history->next_group != UINT64_MAX);
    return ++history->next_group;
}

uint64_t scene_history_step_id(const scene_history *history, bool redo)
{
    assert(history);
    const scene_edit *edits = redo ? history->redo : history->undo;
    return acount(edits) ? alast(edits).step : 0;
}

bool scene_history_push(scene_history *history, ecs_entity entity, Transform before, Transform after)
{
    assert(history && entity.slot);
    assert(isfinite(before.x) && isfinite(before.y));
    assert(isfinite(after.x) && isfinite(after.y));
    if (before.x == after.x && before.y == after.y) {
        return false;
    }
    apush(
        history->undo,
        ((scene_edit){ .entity = entity,
                       .group = history->group,
                       .step = next_step(history),
                       .before = before,
                       .after = after })
    );
    clear_edits(&history->redo);
    return true;
}

static bool scalar(pin_value value)
{
    if (value.container != PIN_CONTAINER_SINGLE) {
        return false;
    }
    switch (value.type) {
        case PIN_BOOL:
        case PIN_I8:
        case PIN_U8:
        case PIN_I16:
        case PIN_U16:
        case PIN_I32:
        case PIN_U32:
        case PIN_I64:
        case PIN_U64:
        case PIN_ENUM:
        case PIN_STR:
        case PIN_NAME:
        case PIN_HANDLE: return true;
        case PIN_F32: return isfinite(value.f);
        case PIN_F64: return isfinite(value.d);
        case PIN_STRUCT: {
            const type_desc *type = type_catalog_find(value.type_name);
            return value.ptr && type && type_value_is_finite(type, value.ptr);
        }
        default: return false;
    }
}

static bool same_type(pin_value a, pin_value b)
{
    if (a.type != b.type) {
        return false;
    }
    if (a.type != PIN_ENUM && a.type != PIN_HANDLE && a.type != PIN_STRUCT) {
        return true;
    }
    return a.type_name && b.type_name && strcmp(a.type_name, b.type_name) == 0;
}

static bool field_value_valid(const type_field *field, pin_value value)
{
    pin_value expected = { .type = field->type, .type_name = field->type_name };
    if (!scalar(value) || !same_type(expected, value)) {
        return false;
    }
    if (field->type != PIN_STR || field->text_descriptor) {
        return true;
    }
    pin_string text = pin_text(value);
    return !text.length || !memchr(text.bytes, 0, text.length);
}

static bool same_value(pin_value a, pin_value b)
{
    switch (a.type) {
        case PIN_BOOL: return a.b == b.b;
        case PIN_I8: return a.i8 == b.i8;
        case PIN_U8: return a.u8 == b.u8;
        case PIN_I16: return a.i16 == b.i16;
        case PIN_U16: return a.u16 == b.u16;
        case PIN_I32: return a.i == b.i;
        case PIN_U32: return a.u == b.u;
        case PIN_I64: return a.i64 == b.i64;
        case PIN_U64:
        case PIN_HANDLE: return a.u64 == b.u64;
        case PIN_F32: return a.f == b.f;
        case PIN_F64: return a.d == b.d;
        case PIN_ENUM: return a.e == b.e;
        case PIN_STR:
        case PIN_NAME: return pin_value_equal(&a, &b);
        case PIN_STRUCT: return pin_value_equal(&a, &b);
        default: return false;
    }
}

bool scene_history_push_fields(
    scene_history *history,
    ecs_entity entity,
    const scene_field_edit *fields,
    int count
)
{
    assert(history && entity.slot && count >= 0 && (!count || fields));
    bool changed = false;
    for (int i = 0; i < count; i++) {
        if (fields[i].field < 0 || !scalar(fields[i].before) ||
            !scalar(fields[i].after) ||
            !same_type(fields[i].before, fields[i].after)) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (fields[i].component == fields[j].component &&
                fields[i].field == fields[j].field) {
                return false;
            }
        }
        changed |= !same_value(fields[i].before, fields[i].after);
    }
    if (!changed) {
        return false;
    }
    scene_edit edit = { .entity = entity };
    for (int i = 0; i < count; i++) {
        scene_field_edit field = fields[i];
        if (field.before.type == PIN_STR || field.before.type == PIN_NAME) {
            int before_error = pin_value_copy(&field.before, &fields[i].before);
            int after_error = pin_value_copy(&field.after, &fields[i].after);
            assert(!before_error && !after_error);
            (void)before_error;
            (void)after_error;
        }
        if (field.before.type == PIN_STRUCT) {
            const type_desc *type = type_catalog_find(field.before.type_name);
            assert(type && type_is_plain(type));
            field.before.ptr = xmalloc(type->size);
            field.after.ptr = xmalloc(type->size);
            memcpy(field.before.ptr, fields[i].before.ptr, type->size);
            memcpy(field.after.ptr, fields[i].after.ptr, type->size);
        }
        field.before.type_name = field.before.type_name
                                   ? xstrdup(field.before.type_name)
                                   : NULL;
        field.after.type_name = field.after.type_name ? xstrdup(field.after.type_name)
                                                      : NULL;
        apush(edit.fields, field);
    }
    edit.group = history->group;
    edit.step = next_step(history);
    apush(history->undo, edit);
    clear_edits(&history->redo);
    return true;
}

bool scene_history_push_field(
    scene_history *history,
    ecs_entity entity,
    ecs_component_id component,
    int field,
    pin_value before,
    pin_value after
)
{
    scene_field_edit edit = { component, field, before, after };
    return scene_history_push_fields(history, entity, &edit, 1);
}

static bool valid_fields(ecs_world *world, const scene_edit *edit)
{
    for (int i = 0; i < acount(edit->fields); i++) {
        const scene_field_edit *field = &edit->fields[i];
        const ecs_component_desc *desc = ecs_world_component(world, field->component);
        if (!desc || !desc->type || field->field < 0 ||
            field->field >= desc->type->field_count ||
            !ecs_component_has(world, edit->entity, field->component)) {
            return false;
        }
        const type_field *type = &desc->type->fields[field->field];
        if (!field_value_valid(type, field->before) ||
            !field_value_valid(type, field->after)) {
            return false;
        }
    }
    return true;
}

bool scene_history_push_actor(scene_history *history, ecs_entity entity, const scene_actor_edit *actor)
{
    assert(history && entity.slot && actor && actor->index >= 0);
    assert(actor->name && actor->prototype_name && actor->base && actor->state);
    scene_actor_edit *copy = xmalloc(sizeof *copy);
    *copy = *actor;
    copy->name = xstrdup(actor->name);
    copy->prototype_name = xstrdup(actor->prototype_name);
    copy->base = av_value_copy(actor->base);
    copy->state = av_value_copy(actor->state);
    copy->retained = actor->retained ? av_value_copy(actor->retained) : NULL;
    copy->paths = actor->paths ? av_value_copy(actor->paths) : NULL;
    apush(
        history->undo,
        ((scene_edit){ .entity = entity,
                       .group = history->group,
                       .step = next_step(history),
                       .actor = copy })
    );
    clear_edits(&history->redo);
    return true;
}

const scene_actor_edit *scene_history_peek_actor(
    const scene_history *history,
    bool redo,
    ecs_entity *entity,
    bool *present
)
{
    assert(history);
    const scene_edit *source = redo ? history->redo : history->undo;
    if (!acount(source) || !alast(source).actor) {
        return NULL;
    }
    const scene_edit *edit = &alast(source);
    if (entity) {
        *entity = edit->entity;
    }
    if (present) {
        *present = edit->actor->created ? redo : !redo;
    }
    return edit->actor;
}

static void rebind_actor(scene_edit *edits, ecs_entity old, ecs_entity replacement)
{
    for (int i = 0; i < acount(edits); i++) {
        if (ecs_entity_to_u64(edits[i].entity) == ecs_entity_to_u64(old)) {
            edits[i].entity = replacement;
        }
    }
}

void scene_history_complete_actor(scene_history *history, bool redo, ecs_entity replacement)
{
    assert(history);
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    assert(acount(*source) && alast(*source).actor);
    bool present = alast(*source).actor->created ? redo : !redo;
    assert(present == (replacement.slot != 0));
    if (present) {
        ecs_entity old = alast(*source).entity;
        rebind_actor(history->undo, old, replacement);
        rebind_actor(history->redo, old, replacement);
    }
    scene_edit edit = alast(*source);
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
}

static bool valid_names(const char *const *names, int count)
{
    if (count < 0 || (count && !names)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!names[i] || !names[i][0]) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(names[i], names[j]) == 0) {
                return false;
            }
        }
    }
    return true;
}

static const char **copy_names(const char *const *names, int count)
{
    if (!count) {
        return NULL;
    }
    const char **copy = xmalloc((size_t)count * sizeof *copy);
    for (int i = 0; i < count; i++) {
        copy[i] = xstrdup(names[i]);
    }
    return copy;
}

bool scene_history_push_behaviors(scene_history *history, ecs_entity entity, const scene_behavior_edit *edit)
{
    assert(history && entity.slot && edit);
    if (!valid_names(edit->before, edit->before_count) ||
        !valid_names(edit->after, edit->after_count)) {
        return false;
    }
    bool changed = edit->before_count != edit->after_count;
    for (int i = 0; !changed && i < edit->before_count; i++) {
        changed = strcmp(edit->before[i], edit->after[i]) != 0;
    }
    if (!changed) {
        return false;
    }
    scene_behavior_edit *copy = xmalloc(sizeof *copy);
    *copy = *edit;
    copy->before = copy_names(edit->before, edit->before_count);
    copy->after = copy_names(edit->after, edit->after_count);
    apush(
        history->undo,
        ((scene_edit){ .entity = entity,
                       .group = history->group,
                       .step = next_step(history),
                       .behaviors = copy })
    );
    clear_edits(&history->redo);
    return true;
}

const scene_behavior_edit *scene_history_peek_behaviors(
    const scene_history *history,
    bool redo,
    ecs_entity *entity
)
{
    assert(history);
    const scene_edit *source = redo ? history->redo : history->undo;
    if (!acount(source) || !alast(source).behaviors) {
        return NULL;
    }
    if (entity) {
        *entity = alast(source).entity;
    }
    return alast(source).behaviors;
}

void scene_history_complete_behaviors(scene_history *history, bool redo)
{
    assert(history);
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    assert(acount(*source) && alast(*source).behaviors);
    scene_edit edit = alast(*source);
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
}

static scene_prototype_edit *copy_prototype_edit(const scene_prototype_edit *edit)
{
    scene_prototype_edit *copy = xmalloc(sizeof *copy);
    *copy = (scene_prototype_edit){
        .name = xstrdup(edit->name),
        .before = av_value_copy(edit->before),
        .after = av_value_copy(edit->after),
        .source_before = edit->source_before ? xstrdup(edit->source_before) : NULL,
        .source_after = edit->source_after ? xstrdup(edit->source_after) : NULL
    };
    return copy;
}

bool scene_history_push_prototype(scene_history *history, const scene_prototype_edit *edit)
{
    assert(history && edit->name && edit->before && edit->after);
    if (!!edit->source_before != !!edit->source_after ||
        (edit->source_before && !strcmp(edit->source_before, edit->source_after) &&
         av_equals(edit->before, edit->after))) {
        return false;
    }
    apush(
        history->undo,
        ((scene_edit){ .group = history->group,
                       .step = next_step(history),
                       .prototype = copy_prototype_edit(edit) })
    );
    clear_edits(&history->redo);
    return true;
}

const scene_prototype_edit *scene_history_peek_prototype(const scene_history *history, bool redo)
{
    assert(history);
    const scene_edit *source = redo ? history->redo : history->undo;
    return acount(source) ? alast(source).prototype : NULL;
}

void scene_history_complete_prototype(scene_history *history, bool redo)
{
    assert(history);
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    assert(acount(*source) && alast(*source).prototype);
    scene_edit edit = alast(*source);
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
}

bool scene_history_push_rename(
    scene_history *history,
    ecs_entity entity,
    const char *before,
    const char *after,
    const scene_path_ref *paths
)
{
    assert(history && entity.slot && before && before[0] && after && after[0]);
    if (!strcmp(before, after)) {
        return false;
    }
    scene_path_ref *copied = NULL;
    for (int i = 0; i < acount(paths); i++) {
        apush(copied, paths[i]);
    }
    apush(
        history->undo,
        ((scene_edit){ .entity = entity,
                       .group = history->group,
                       .step = next_step(history),
                       .name_before = xstrdup(before),
                       .name_after = xstrdup(after),
                       .renamed_paths = copied })
    );
    clear_edits(&history->redo);
    return true;
}

const char *scene_history_peek_rename(const scene_history *history, bool redo, ecs_entity *entity)
{
    assert(history);
    const scene_edit *source = redo ? history->redo : history->undo;
    if (!acount(source) || !alast(source).name_after) {
        return NULL;
    }
    *entity = alast(source).entity;
    return redo ? alast(source).name_after : alast(source).name_before;
}

const scene_path_ref *scene_history_peek_rename_paths(const scene_history *history, bool redo)
{
    assert(history);
    const scene_edit *source = redo ? history->redo : history->undo;
    assert(acount(source) && alast(source).name_after);
    return alast(source).renamed_paths;
}

void scene_history_complete_rename(scene_history *history, bool redo)
{
    assert(history);
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    assert(acount(*source) && alast(*source).name_after);
    scene_edit edit = alast(*source);
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
}

bool scene_history_push_hierarchy(scene_history *history, av_value *before, av_value *after)
{
    assert(history && before && after);
    if (av_equals(before, after)) {
        return false;
    }
    apush(
        history->undo,
        ((scene_edit){ .group = history->group,
                       .step = next_step(history),
                       .hierarchy_before = av_value_copy(before),
                       .hierarchy_after = av_value_copy(after) })
    );
    clear_edits(&history->redo);
    return true;
}

av_value *scene_history_peek_hierarchy(const scene_history *history, bool redo)
{
    const scene_edit *edits = redo ? history->redo : history->undo;
    if (!acount(edits)) {
        return NULL;
    }
    return redo ? alast(edits).hierarchy_after : alast(edits).hierarchy_before;
}

void scene_history_complete_hierarchy(scene_history *history, bool redo)
{
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    assert(acount(*source) && alast(*source).hierarchy_after);
    scene_edit edit = alast(*source);
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
}

bool scene_history_can_undo(const scene_history *history)
{
    assert(history);
    return acount(history->undo) > 0;
}

bool scene_history_push_physics(scene_history *history, physics_settings before, physics_settings after)
{
    assert(physics_settings_valid(before) && physics_settings_valid(after));
    if (physics_settings_equal(before, after)) {
        return false;
    }
    apush(
        history->undo,
        ((scene_edit){ .physics = true,
                       .group = history->group,
                       .step = next_step(history),
                       .physics_before = before,
                       .physics_after = after })
    );
    clear_edits(&history->redo);
    return true;
}

const physics_settings *scene_history_peek_physics(const scene_history *history, bool redo)
{
    const scene_edit *edits = redo ? history->redo : history->undo;
    if (!acount(edits) || !alast(edits).physics) {
        return NULL;
    }
    return redo ? &alast(edits).physics_after : &alast(edits).physics_before;
}

void scene_history_complete_physics(scene_history *history, bool redo)
{
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    assert(acount(*source) && alast(*source).physics);
    scene_edit edit = alast(*source);
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
}

bool scene_history_can_redo(const scene_history *history)
{
    assert(history);
    return acount(history->redo) > 0;
}

static int apply_edit(scene_history *history, ecs_world *world, ecs_entity *out_entity, bool redo)
{
    assert(history && world);
    scene_edit **source = redo ? &history->redo : &history->undo;
    scene_edit **destination = redo ? &history->undo : &history->redo;
    if (!acount(*source)) {
        return 0;
    }
    scene_edit edit = alast(*source);
    if (edit.actor || edit.behaviors || edit.prototype || edit.physics || edit.name_after ||
        edit.hierarchy_after || !ecs_entity_is_alive(world, edit.entity)) {
        return -1;
    }
    if (edit.fields) {
        if (!valid_fields(world, &edit)) {
            return -1;
        }
        for (int i = 0; i < acount(edit.fields); i++) {
            scene_field_edit *field = &edit.fields[i];
            int result = ecs_component_write_field(
                world,
                edit.entity,
                field->component,
                field->field,
                redo ? field->after : field->before
            );
            assert(result == ECS_OK);
        }
    } else {
        Transform *transform = Transform_get(world, edit.entity);
        if (!transform) {
            return -1;
        }
        *transform = redo ? edit.after : edit.before;
    }
    asetlen(*source, acount(*source) - 1);
    apush(*destination, edit);
    if (out_entity) {
        *out_entity = edit.entity;
    }
    return 1;
}

int scene_history_undo(scene_history *history, ecs_world *world, ecs_entity *out_entity)
{
    return apply_edit(history, world, out_entity, false);
}

int scene_history_redo(scene_history *history, ecs_world *world, ecs_entity *out_entity)
{
    return apply_edit(history, world, out_entity, true);
}

/* History uses reflected names so restoring a saved workspace cannot retarget
 * an Undo after a component or field is reordered. */

static void history_text(av_doc *doc, av_value *object, const char *key, const char *text)
{
    if (text) {
        av_obj_add_str(doc, object, key, text);
    }
}

static char *history_copy_text(av_value *object, const char *key)
{
    const char *text = av_get_str(av_obj_get(object, key));
    return text ? xstrdup(text) : NULL;
}

static void history_free_strings(char **strings)
{
    for (int i = 0; i < acount(strings); i++) {
        free(strings[i]);
    }
    afree(strings);
}

static bool history_read_strings(av_value *array, char ***out)
{
    if (!av_is_arr(array)) {
        return false;
    }
    size_t i, count;
    av_value *value;
    av_arr_foreach(array, i, count, value)
    {
        if (!av_is_str(value)) {
            return false;
        }
        apush(*out, xstrdup(av_get_str(value)));
    }
    return true;
}

static av_value *history_position(av_doc *doc, const double *positions)
{
    av_value *array = av_arr(doc);
    for (int i = 0; i < 4; i++) {
        av_arr_append(array, av_real(doc, positions[i]));
    }
    return array;
}

static bool history_read_position(av_value *array, double *values)
{
    if (!av_is_arr(array) || av_arr_size(array) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        av_value *value = av_arr_get(array, i);
        if (!av_is_num(value) || !isfinite(av_get_num(value))) {
            return false;
        }
        values[i] = av_get_num(value);
    }
    return true;
}

static const ecs_component_desc *history_component(const char *name)
{
    ecs_component_catalog project = project_components();
    for (int i = 0; name && i < project.component_count; i++) {
        if (!strcmp(name, project.components[i]->name)) {
            return project.components[i];
        }
    }
    return NULL;
}

static const ecs_component_desc *history_component_id(ecs_component_id id)
{
    ecs_component_catalog project = project_components();
    for (int i = 0; i < project.component_count; i++) {
        if (project.components[i]->id == id) {
            return project.components[i];
        }
    }
    return NULL;
}

/* The index of the component's field named `name`, or -1. */
static int history_field(const ecs_component_desc *component, const char *name)
{
    for (int i = 0; name && i < component->type->field_count; i++) {
        if (!strcmp(component->type->fields[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* Reads a rename's paths; an older history without them rewrote none. */
static bool history_read_paths(av_value *array, scene_path_ref **paths)
{
    if (!array) {
        return true;
    }
    if (!av_is_arr(array)) {
        return false;
    }
    size_t i, count;
    av_value *item;
    av_arr_foreach(array, i, count, item)
    {
        av_value *actor = av_obj_get(item, "actor");
        const ecs_component_desc *component = history_component(
            av_get_str(av_obj_get(item, "component"))
        );
        int field = component && component->type
                      ? history_field(component, av_get_str(av_obj_get(item, "field")))
                      : -1;
        if (!av_is_uint(actor) || field < 0) {
            return false;
        }
        apush(*paths, ((scene_path_ref){ av_get_uint(actor), component->id, field }));
    }
    return true;
}

static av_value *history_value(av_doc *doc, pin_value value)
{
    av_value *object = av_obj(doc);
    av_obj_add_int(doc, object, "type", value.type);
    history_text(doc, object, "name", value.type_name);
    switch (value.type) {
        case PIN_BOOL: av_obj_add_bool(doc, object, "value", value.b); break;
        case PIN_NAME:
            history_text(doc, object, "value", value.str ? value.str : "");
            break;
        case PIN_I8: av_obj_add_int(doc, object, "value", value.i8); break;
        case PIN_I16: av_obj_add_int(doc, object, "value", value.i16); break;
        case PIN_I32: av_obj_add_int(doc, object, "value", value.i); break;
        case PIN_I64: av_obj_add_int(doc, object, "value", value.i64); break;
        case PIN_ENUM: av_obj_add_int(doc, object, "value", value.e); break;
        case PIN_U8: av_obj_add_uint(doc, object, "value", value.u8); break;
        case PIN_U16: av_obj_add_uint(doc, object, "value", value.u16); break;
        case PIN_U32: av_obj_add_uint(doc, object, "value", value.u); break;
        case PIN_U64:
        case PIN_HANDLE:
            av_obj_add_uint(doc, object, "value", value.u64);
            break;
        case PIN_F32:
            av_obj_add_val(doc, object, "value", av_real(doc, value.f));
            break;
        case PIN_F64:
            av_obj_add_val(doc, object, "value", av_real(doc, value.d));
            break;
        case PIN_STR:
        case PIN_STRUCT: {
            pin_value_write_document(doc, object, "value", value.type, value.type_name, &value);
            break;
        }
        default: assert(false);
    }
    return object;
}

static bool history_read_typed_value(pin_type type, const char *type_name, av_value *document, pin_value *out)
{
    const type_desc *shape = type == PIN_STRUCT ? type_catalog_find(type_name) : NULL;
    if (type == PIN_STRUCT &&
        (!shape || !type_is_plain(shape) || !av_is_obj(document))) {
        return false;
    }
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
    return valid;
}

static bool history_read_value(av_value *object, pin_value *out)
{
    av_value *type = av_obj_get(object, "type"), *value = av_obj_get(object, "value");
    if (!av_is_int(type)) {
        return false;
    }
    out->type = av_get_int(type);
    out->type_name = history_copy_text(object, "name");
    if (out->type == PIN_STRUCT) {
        av_value *value_object = av_obj_get(object, "value");
        pin_value value = { 0 };
        if (!history_read_typed_value(PIN_STRUCT, out->type_name, value_object, &value)) {
            return false;
        }
        out->ptr = value.ptr;
        return scalar(*out);
    }
    if (out->type == PIN_STR) {
        return history_read_typed_value(PIN_STR, out->type_name, value, out);
    }
    if (out->type == PIN_NAME) {
        if (!av_is_str(value)) {
            return false;
        }
        if (memchr(av_get_str(value), 0, av_get_len(value))) {
            return false;
        }
        out->str = xstrdup(av_get_str(value));
        return true;
    }
    if (out->type == PIN_BOOL) {
        if (!av_is_bool(value)) {
            return false;
        }
        out->b = av_get_bool(value);
        return true;
    }
    if (!av_is_num(value)) {
        return false;
    }
    bool signed_number = av_is_int(value) &&
                         (!av_is_uint(value) || av_get_uint(value) <= INT64_MAX);
    bool unsigned_number = av_is_uint(value);
    int64_t signed_value = av_get_sint(value);
    uint64_t unsigned_value = av_get_uint(value);
    switch (out->type) {
        case PIN_I8:
            if (!signed_number || signed_value < INT8_MIN || signed_value > INT8_MAX) {
                return false;
            }
            out->i8 = signed_value;
            break;
        case PIN_I16:
            if (!signed_number || signed_value < INT16_MIN || signed_value > INT16_MAX) {
                return false;
            }
            out->i16 = signed_value;
            break;
        case PIN_I32:
            if (!signed_number || signed_value < INT32_MIN || signed_value > INT32_MAX) {
                return false;
            }
            out->i = signed_value;
            break;
        case PIN_I64:
            if (!signed_number || signed_value < INT64_MIN || signed_value > INT64_MAX) {
                return false;
            }
            out->i64 = signed_value;
            break;
        case PIN_ENUM:
            if (!signed_number || signed_value < INT32_MIN || signed_value > INT32_MAX) {
                return false;
            }
            out->e = signed_value;
            break;
        case PIN_U8:
            if (!unsigned_number || unsigned_value > UINT8_MAX) {
                return false;
            }
            out->u8 = unsigned_value;
            break;
        case PIN_U16:
            if (!unsigned_number || unsigned_value > UINT16_MAX) {
                return false;
            }
            out->u16 = unsigned_value;
            break;
        case PIN_U32:
            if (!unsigned_number || unsigned_value > UINT32_MAX) {
                return false;
            }
            out->u = unsigned_value;
            break;
        case PIN_U64:
        case PIN_HANDLE:
            if (!unsigned_number) {
                return false;
            }
            out->u64 = unsigned_value;
            break;
        case PIN_F32: out->f = av_get_num(value); break;
        case PIN_F64: out->d = av_get_num(value); break;
        default: return false;
    }
    return scalar(*out);
}

static av_value *history_edits(av_doc *doc, const scene_edit *edits)
{
    av_value *array = av_arr(doc);
    for (int i = 0; i < acount(edits); i++) {
        const scene_edit *edit = &edits[i];
        av_value *object = av_obj(doc);
        av_arr_append(array, object);
        av_obj_add_uint(doc, object, "entity", ecs_entity_to_u64(edit->entity));
        av_obj_add_uint(doc, object, "group", edit->group);
        if (edit->hierarchy_after) {
            av_obj_add_val(doc, object, "hierarchy_before", av_clone(doc, edit->hierarchy_before));
            av_obj_add_val(doc, object, "hierarchy_after", av_clone(doc, edit->hierarchy_after));
        }
        if (edit->physics) {
            av_obj_add_val(doc, object, "physics_before", physics_settings_write(doc, edit->physics_before));
            av_obj_add_val(doc, object, "physics_after", physics_settings_write(doc, edit->physics_after));
        }
        double positions[] = { edit->before.x,
                               edit->before.y,
                               edit->after.x,
                               edit->after.y };
        av_obj_add_val(doc, object, "position", history_position(doc, positions));
        av_value *fields = av_obj_add_arr(doc, object, "fields");
        for (int j = 0; j < acount(edit->fields); j++) {
            const scene_field_edit *field = &edit->fields[j];
            const ecs_component_desc *component = history_component_id(field->component);
            assert(
                component && component->type && field->field < component->type->field_count
            );
            av_value *item = av_arr_add_obj(doc, fields);
            history_text(doc, item, "component", component->name);
            history_text(
                doc,
                item,
                "field",
                component->type->fields[field->field].name
            );
            av_obj_add_val(doc, item, "before", history_value(doc, field->before));
            av_obj_add_val(doc, item, "after", history_value(doc, field->after));
        }
        if (edit->actor) {
            const scene_actor_edit *actor = edit->actor;
            av_value *item = av_obj_add_obj(doc, object, "actor");
            if (actor->identity) {
                av_obj_add_uint(doc, item, "identity", actor->identity);
            }
            av_obj_add_uint(doc, item, "parent", actor->parent);
            av_obj_add_int(doc, item, "index", actor->index);
            av_obj_add_bool(doc, item, "created", actor->created);
            av_obj_add_bool(doc, item, "own_behaviors", actor->own_behaviors);
            history_text(doc, item, "name", actor->name);
            history_text(doc, item, "prototype", actor->prototype_name);
            av_obj_add_val(doc, item, "base", av_clone(doc, actor->base));
            av_obj_add_val(doc, item, "state", av_clone(doc, actor->state));
            if (actor->retained) {
                av_obj_add_val(doc, item, "retained", av_clone(doc, actor->retained));
            }
            if (actor->paths) {
                av_obj_add_val(doc, item, "paths", av_clone(doc, actor->paths));
            }
        }
        if (edit->behaviors) {
            av_value *item = av_obj_add_obj(doc, object, "behaviors");
            av_value *before = av_obj_add_arr(doc, item, "before");
            av_value *after = av_obj_add_arr(doc, item, "after");
            for (int j = 0; j < edit->behaviors->before_count; j++) {
                av_arr_add_strcpy(doc, before, edit->behaviors->before[j]);
            }
            for (int j = 0; j < edit->behaviors->after_count; j++) {
                av_arr_add_strcpy(doc, after, edit->behaviors->after[j]);
            }
        }
        if (edit->prototype) {
            av_value *item = av_obj_add_obj(doc, object, "prototype");
            history_text(doc, item, "name", edit->prototype->name);
            av_obj_add_val(doc, item, "before", av_clone(doc, edit->prototype->before));
            av_obj_add_val(doc, item, "after", av_clone(doc, edit->prototype->after));
            history_text(doc, item, "source_before", edit->prototype->source_before);
            history_text(doc, item, "source_after", edit->prototype->source_after);
        }
        if (edit->name_after) {
            av_value *item = av_obj_add_obj(doc, object, "rename");
            history_text(doc, item, "before", edit->name_before);
            history_text(doc, item, "after", edit->name_after);
            av_value *paths = av_obj_add_arr(doc, item, "paths");
            for (int j = 0; j < acount(edit->renamed_paths); j++) {
                const scene_path_ref *path = &edit->renamed_paths[j];
                const ecs_component_desc *component = history_component_id(path->component);
                assert(component && path->field < component->type->field_count);
                av_value *entry = av_arr_add_obj(doc, paths);
                av_obj_add_uint(doc, entry, "actor", path->actor);
                history_text(doc, entry, "component", component->name);
                history_text(
                    doc,
                    entry,
                    "field",
                    component->type->fields[path->field].name
                );
            }
        }
    }
    return array;
}

av_value *scene_history_capture(const scene_history *history)
{
    assert(!history->group);
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_doc_set_root(doc, root);
    av_obj_add_int(doc, root, "version", 1);
    av_obj_add_uint(doc, root, "next_group", history->next_group);
    av_obj_add_val(doc, root, "undo", history_edits(doc, history->undo));
    av_obj_add_val(doc, root, "redo", history_edits(doc, history->redo));
    av_value *value = av_value_copy(root);
    av_doc_free(doc);
    return value;
}

static bool history_read_edits(av_value *array, scene_edit **edits, uint64_t **before, uint64_t **after)
{
    if (!av_is_arr(array)) {
        return false;
    }
    size_t i, count;
    av_value *object;
    av_arr_foreach(array, i, count, object)
    {
        if (!av_is_obj(object) || !av_is_uint(av_obj_get(object, "entity"))) {
            return false;
        }
        scene_edit edit = { 0 };
        apush(*edits, edit);
        scene_edit *out = &alast(*edits);
        uint64_t old = av_get_uint(av_obj_get(object, "entity"));
        int mapping = 0;
        while (mapping < acount(*before) && (*before)[mapping] != old) {
            mapping++;
        }
        if (mapping == acount(*before)) {
            apush(*before, old);
            apush(*after, ecs_entity_to_u64((ecs_entity){ .slot = mapping + 1, .generation = UINT32_MAX }));
        }
        out->entity = ecs_entity_from_u64((*after)[mapping]);
        out->group = av_get_uint(av_obj_get(object, "group"));
        av_value *hierarchy = av_obj_get(object, "hierarchy_before");
        if (hierarchy) {
            av_value *next = av_obj_get(object, "hierarchy_after");
            if (!av_is_obj(hierarchy) || !av_is_obj(next)) {
                return false;
            }
            out->entity = (ecs_entity){ 0 };
            out->hierarchy_before = av_value_copy(hierarchy);
            out->hierarchy_after = av_value_copy(next);
        }
        av_value *physics = av_obj_get(object, "physics_before");
        if (physics) {
            av_value *next = av_obj_get(object, "physics_after");
            if (!next || !physics_settings_read(physics, &out->physics_before) ||
                !physics_settings_read(next, &out->physics_after)) {
                return false;
            }
            out->entity = (ecs_entity){ 0 };
            out->physics = true;
        }
        double position[4];
        if (!history_read_position(av_obj_get(object, "position"), position)) {
            return false;
        }
        out->before = (Transform){ position[0], position[1] };
        out->after = (Transform){ position[2], position[3] };
        av_value *fields = av_obj_get(object, "fields");
        if (!av_is_arr(fields)) {
            return false;
        }
        size_t j, field_count;
        av_value *item;
        av_arr_foreach(fields, j, field_count, item)
        {
            const ecs_component_desc *component = history_component(
                av_get_str(av_obj_get(item, "component"))
            );
            int index = component && component->type
                          ? history_field(component, av_get_str(av_obj_get(item, "field")))
                          : -1;
            if (index < 0) {
                return false;
            }
            scene_field_edit field = { .component = component->id, .field = index };
            apush(out->fields, field);
            scene_field_edit *stored = &alast(out->fields);
            if (!history_read_value(av_obj_get(item, "before"), &stored->before) ||
                !history_read_value(av_obj_get(item, "after"), &stored->after)) {
                return false;
            }
            const type_field *type = &component->type->fields[index];
            if (!field_value_valid(type, stored->before) ||
                !field_value_valid(type, stored->after)) {
                return false;
            }
        }
        item = av_obj_get(object, "actor");
        if (item) {
            out->actor = xcalloc(1, sizeof *out->actor);
            scene_actor_edit *actor = out->actor;
            av_value *identity = av_obj_get(item, "identity");
            if (identity && !av_is_uint(identity)) {
                return false;
            }
            actor->identity = av_get_uint(identity);
            actor->parent = av_get_uint(av_obj_get(item, "parent"));
            actor->index = av_get_int(av_obj_get(item, "index"));
            actor->created = av_get_bool(av_obj_get(item, "created"));
            actor->own_behaviors = av_get_bool(av_obj_get(item, "own_behaviors"));
            actor->name = history_copy_text(item, "name");
            actor->prototype_name = history_copy_text(item, "prototype");
            av_value *base = av_obj_get(item, "base");
            av_value *state = av_obj_get(item, "state");
            av_value *retained = av_obj_get(item, "retained");
            av_value *paths = av_obj_get(item, "paths");
            if (!av_is_obj(base) || !av_is_obj(state) ||
                (retained && !av_is_obj(retained)) || (paths && !av_is_obj(paths))) {
                return false;
            }
            actor->base = av_value_copy(base);
            actor->state = av_value_copy(state);
            actor->retained = retained ? av_value_copy(retained) : NULL;
            actor->paths = paths ? av_value_copy(paths) : NULL;
            if (actor->index < 0 || !actor->name || !actor->prototype_name ||
                !actor->base || !actor->state) {
                return false;
            }
        }
        item = av_obj_get(object, "behaviors");
        if (item) {
            char **previous = NULL, **next = NULL;
            bool valid = history_read_strings(av_obj_get(item, "before"), &previous) &&
                         history_read_strings(av_obj_get(item, "after"), &next);
            out->behaviors = xcalloc(1, sizeof *out->behaviors);
            *out->behaviors = (scene_behavior_edit){
                copy_names((const char *const *)previous, acount(previous)),
                acount(previous),
                copy_names((const char *const *)next, acount(next)),
                acount(next)
            };
            history_free_strings(previous);
            history_free_strings(next);
            if (!valid) {
                return false;
            }
        }
        item = av_obj_get(object, "prototype");
        if (item) {
            char *name = history_copy_text(item, "name");
            av_value *before = av_obj_get(item, "before");
            av_value *after = av_obj_get(item, "after");
            av_value *source_before = av_obj_get(item, "source_before");
            av_value *source_after = av_obj_get(item, "source_after");
            bool valid = name && av_is_obj(before) && av_is_obj(after) &&
                         ((!source_before && !source_after) ||
                          (av_is_str(source_before) && av_is_str(source_after)));
            scene_prototype_edit edit = { .name = name,
                                          .before = before,
                                          .after = after,
                                          .source_before = av_get_str(source_before),
                                          .source_after = av_get_str(source_after) };
            out->entity = (ecs_entity){ 0 };
            out->prototype = valid ? copy_prototype_edit(&edit) : NULL;
            free(name);
            if (!valid) {
                return false;
            }
        }
        item = av_obj_get(object, "rename");
        if (item) {
            out->name_before = history_copy_text(item, "before");
            out->name_after = history_copy_text(item, "after");
            if (!out->name_before || !out->name_after ||
                !history_read_paths(av_obj_get(item, "paths"), &out->renamed_paths)) {
                return false;
            }
        }
    }
    return true;
}

bool scene_history_restore_value(
    scene_history *history,
    av_value *root,
    const uint64_t *old_entities,
    const uint64_t *new_entities,
    int count
)
{

    if (av_get_int(av_obj_get(root, "version")) != 1) {
        return false;
    }
    scene_history candidate = { .next_group = av_get_uint(av_obj_get(root, "next_group")) };
    uint64_t *before = NULL, *after = NULL;
    for (int i = 0; i < count; i++) {
        apush(before, old_entities[i]);
        apush(after, new_entities[i]);
    }
    bool valid = history_read_edits(av_obj_get(root, "undo"), &candidate.undo, &before, &after) &&
                 history_read_edits(av_obj_get(root, "redo"), &candidate.redo, &before, &after);
    afree(before);
    afree(after);
    if (valid) {
        scene_history previous = *history;
        *history = candidate;
        candidate = previous;
    }
    clear_edits(&candidate.undo);
    clear_edits(&candidate.redo);
    afree(candidate.undo);
    afree(candidate.redo);
    return valid;
}

/* JSON is the collaboration checkpoint boundary, never the native history store. */
char *scene_history_checkpoint(const scene_history *history)
{
    av_value *value = scene_history_capture(history);
    char *json = av_json_write(value);
    av_value_free(value);
    return json;
}

bool scene_history_restore_checkpoint(
    scene_history *history,
    const char *json,
    const uint64_t *old_entities,
    const uint64_t *new_entities,
    int count
)
{
    av_doc *doc = json ? av_json_read(json) : NULL;
    bool valid = doc &&
                 scene_history_restore_value(history, av_doc_root(doc), old_entities, new_entities, count);
    av_doc_free(doc);
    return valid;
}
