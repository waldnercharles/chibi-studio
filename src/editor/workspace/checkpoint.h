#ifndef CHIBI_CHECKPOINT_H
#define CHIBI_CHECKPOINT_H

#include "editor/workspace/viewport.h"
#include <yyjson.h>
#include "runtime/engine.h"
#include "support/array.h"

#include <math.h>
#include <string.h>

/* Saved checkpoints contain owned JSON values, never process pointers. */
static inline void checkpoint_text(
    yyjson_mut_doc *doc,
    yyjson_mut_val *object,
    const char *key,
    const char *text
)
{
    yyjson_mut_obj_add_val(doc, object, key, text ? yyjson_mut_strcpy(doc, text) : yyjson_mut_null(doc));
}

static inline char *checkpoint_copy_text(yyjson_val *object, const char *key)
{
    const char *text = yyjson_get_str(yyjson_obj_get(object, key));
    return text ? xstrdup(text) : NULL;
}

static inline yyjson_mut_val *checkpoint_strings(yyjson_mut_doc *doc, char *const *strings)
{
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int i = 0; i < acount(strings); i++) {
        yyjson_mut_arr_add_strcpy(doc, array, strings[i]);
    }
    return array;
}

static inline bool checkpoint_read_strings(yyjson_val *array, char ***strings)
{
    if (!yyjson_is_arr(array)) {
        return false;
    }
    size_t i, count;
    yyjson_val *value;
    yyjson_arr_foreach(array, i, count, value)
    {
        if (!yyjson_is_str(value)) {
            return false;
        }
        apush(*strings, xstrdup(yyjson_get_str(value)));
    }
    return true;
}

static inline void checkpoint_free_strings(char **strings)
{
    for (int i = 0; i < acount(strings); i++) {
        free(strings[i]);
    }
    afree(strings);
}

/* A view as nine numbers, then its lens's twelve: eye, forward, up, field
   of view and clips. */
#define CHECKPOINT_CAMERA_VALUES 21

static inline yyjson_mut_val *checkpoint_camera(yyjson_mut_doc *doc, engine_viewport_view view)
{
    engine_viewport_lens lens = view.lens;
    double values[CHECKPOINT_CAMERA_VALUES] = { view.center_x,
                                                view.center_y,
                                                view.width,
                                                view.height,
                                                view.focal_distance,
                                                view.yaw,
                                                view.pitch,
                                                view.target_elevation,
                                                view.roll,
                                                lens.eye.x,
                                                lens.eye.y,
                                                lens.eye.z,
                                                lens.forward.x,
                                                lens.forward.y,
                                                lens.forward.z,
                                                lens.up.x,
                                                lens.up.y,
                                                lens.up.z,
                                                lens.fov,
                                                lens.near_clip,
                                                lens.far_clip };
    return yyjson_mut_arr_with_real(doc, values, CHECKPOINT_CAMERA_VALUES);
}

/* Reads a view checkpoint_camera wrote, or the nine numbers of one saved
   before views kept their lens. */
static inline bool checkpoint_read_camera(yyjson_val *array, engine_viewport_view *view)
{
    if (!yyjson_is_arr(array)) {
        return false;
    }
    size_t count = yyjson_arr_size(array);
    if (count != 9 && count != CHECKPOINT_CAMERA_VALUES) {
        return false;
    }
    float values[CHECKPOINT_CAMERA_VALUES] = { 0 };
    for (size_t i = 0; i < count; i++) {
        yyjson_val *value = yyjson_arr_get(array, i);
        if (!yyjson_is_num(value) || !isfinite(yyjson_get_num(value))) {
            return false;
        }
        values[i] = (float)yyjson_get_num(value);
        if (!isfinite(values[i])) {
            return false;
        }
    }
    if (values[4] < 0 || (values[4] > 0 && values[4] <= 1)) {
        return false;
    }
    float fov = values[18], near_clip = values[19], far_clip = values[20];
    if (fov < 0 || fov >= CF_PI || (near_clip > 0) != (far_clip > 0) ||
        (near_clip > 0 && far_clip <= near_clip)) {
        return false;
    }
    *view = (engine_viewport_view){ values[0],
                                    values[1],
                                    values[2],
                                    values[3],
                                    values[4],
                                    values[5],
                                    values[6],
                                    values[7],
                                    values[8],
                                    { .eye = { values[9], values[10], values[11] },
                                      .forward = { values[12], values[13], values[14] },
                                      .up = { values[15], values[16], values[17] },
                                      .fov = fov,
                                      .near_clip = near_clip,
                                      .far_clip = far_clip } };
    return true;
}

/* Unmapped old handles remain tombstones, in a generation that cannot name a
 * candidate actor. Distinct deleted actors retain distinct resurrection keys. */
static inline ecs_entity checkpoint_entity(
    uint64_t old,
    const uint64_t *before,
    const uint64_t *after,
    int count
)
{
    for (int i = 0; i < count; i++) {
        if (old == before[i]) {
            return ecs_entity_from_u64(after[i]);
        }
    }
    ecs_entity entity = ecs_entity_from_u64(old);
    if (entity.slot) {
        entity.generation = UINT32_MAX;
    }
    return entity;
}

#endif
