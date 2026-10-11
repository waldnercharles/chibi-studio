/* Studio's editor nodes, run (studio_nodes.h): the "studio" service's one
 * call. Reads look at what Studio holds now; effects record history as the
 * native panels' did. An object gone since its row showed is skipped, as a
 * click on a row removed meanwhile does nothing. */
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_nodes.h"
#include "types/array.h"
#include "support/array.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Values ---------- */

static const char *copy_text(arena *storage, const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = arena_alloc(storage, size);
    memcpy(copy, text, size);
    return copy;
}

static pin_value text_value(arena *storage, const char *text)
{
    return (pin_value){ .type = PIN_STR,
                        .type_name = "const char *",
                        .str = copy_text(storage, text) };
}

static pin_value int_value(int i)
{
    return (pin_value){ .type = PIN_I32, .type_name = "int", .i = i };
}

static pin_value bool_value(bool b)
{
    return (pin_value){ .type = PIN_BOOL, .type_name = "bool", .b = b };
}

static const char *glyph_text(arena *storage, uint32_t icon)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    return copy_text(storage, glyph);
}

/* An object's key: its entity as text; "" for none. */
static void key_of(char out[24], const scene_actor *actor)
{
    out[0] = '\0';
    if (actor) {
        snprintf(out, 24, "%llu", (unsigned long long)ecs_entity_to_u64(actor->entity));
    }
}

/* Where actor `index` is in `rows`; -1. */
static int row_in(const int *rows, int index)
{
    for (int r = 0; index >= 0 && r < acount(rows); r++) {
        if (rows[r] == index) {
            return r;
        }
    }
    return -1;
}

/* ---------- Reads ---------- */

static studio_object_row object_row(studio *s, arena *storage, int index)
{
    app_scene_authoring *scene = s->scene;
    const scene_actor *actor = &scene->actors[index];
    char key[24], drop[160];
    key_of(key, actor);
    char **names = instance_behavior_names(scene, actor->entity);
    int behaviors = acount(names);
    free_behavior_names(names);
    studio_drop_types(s, index, drop, sizeof drop);
    unsigned view = app_scene_authoring_actor_view(scene, index);
    int depth = 1;
    for (ecs_entity parent = ecs_parent(scene->world, actor->entity); parent.slot;
         parent = ecs_parent(scene->world, parent)) {
        depth++;
    }
    bool selected = scene_instance_selected(scene, index),
         active = scene->selected == index;
    return (studio_object_row){
        .key = copy_text(storage, key),
        .label = copy_text(storage, actor->name),
        .icon = glyph_text(storage, studio_prototype_icon(s, actor->prototype_name)),
        .prototype = copy_text(storage, actor->prototype_name ? actor->prototype_name : ""),
        .drop = copy_text(storage, drop),
        .depth = depth,
        .behaviors = behaviors,
        .active = active,
        .marked = selected && !active,
        .hidden = view & APP_SCENE_VIEW_HIDDEN,
        .locked = view & APP_SCENE_VIEW_LOCKED,
        .renaming = s->rename.outliner &&
                    s->rename.entity == ecs_entity_to_u64(actor->entity),
    };
}

/* Count objects from First of those the object filter shows, as rows. */
static void scene_objects(studio *s, const pin_value *in, arena *storage, pin_value *out)
{
    int *rows = studio_outliner_rows(s);
    int shown = acount(rows);
    int first = in[0].i < 0 ? 0 : in[0].i > shown ? shown : in[0].i;
    int count = in[1].i < 0             ? 0
              : in[1].i > shown - first ? shown - first
                                        : in[1].i;
    pin_array *items = pin_array_snapshot_allocate(storage, PIN_STRUCT, STUDIO_OBJECT_ROW_TYPE, count);
    for (int i = 0; i < count; i++) {
        studio_object_row *row = arena_alloc(storage, sizeof *row);
        *row = object_row(s, storage, rows[first + i]);
        items->items[i] = (pin_value){ .type = PIN_STRUCT,
                                       .type_name = items->type_name,
                                       .ptr = row };
    }
    afree(rows);
    out[0] = (pin_value){ .type = PIN_STRUCT,
                          .type_name = items->type_name,
                          .container = PIN_CONTAINER_ARRAY,
                          .ptr = items };
    out[1] = int_value(shown);
}

static void scene_info(studio *s, arena *storage, pin_value *out)
{
    char title[128];
    out[0] = text_value(storage, studio_scene_title(s->scene, title, sizeof title));
    out[1] = int_value(acount(s->scene->actors));
    out[2] = text_value(storage, s->outliner_filter);
}

static void selection(studio *s, arena *storage, pin_value *out)
{
    const scene_actor *actor = selected_actor(s->scene);
    char key[24];
    key_of(key, actor);
    out[0] = text_value(storage, key);
    out[1] = int_value(scene_selection_count(s->scene));
    out[2] = text_value(storage, actor ? actor->name : "");
    out[3] = text_value(storage, actor && actor->prototype_name ? actor->prototype_name : "");
}

static void renaming(studio *s, arena *storage, pin_value *out)
{
    char key[24] = "";
    if (s->rename.entity && s->rename.outliner) {
        snprintf(key, sizeof key, "%llu", (unsigned long long)s->rename.entity);
    }
    out[0] = text_value(storage, key);
}

/* The prototypes the scene loaded, named, as options. */
static void prototype_choices(studio *s, arena *storage, pin_value *out)
{
    int count = 0;
    for (int i = 0; i < acount(s->scene->prototypes); i++) {
        const prototype *prototype = s->scene->prototypes[i].prototype;
        count += prototype && prototype->name;
    }
    pin_array *items = pin_array_snapshot_allocate(storage, PIN_STRUCT, STUDIO_CHOICE_TYPE, count);
    for (int i = 0, at = 0; i < acount(s->scene->prototypes); i++) {
        const prototype *prototype = s->scene->prototypes[i].prototype;
        if (!prototype || !prototype->name) {
            continue;
        }
        studio_choice *choice = arena_alloc(storage, sizeof *choice);
        const char *name = copy_text(storage, prototype->name);
        *choice = (studio_choice){
            .value = name,
            .label = name,
            .icon = glyph_text(storage, studio_prototype_icon(s, prototype->name)),
        };
        items->items[at++] = (pin_value){ .type = PIN_STRUCT,
                                          .type_name = items->type_name,
                                          .ptr = choice };
    }
    out[0] = (pin_value){ .type = PIN_STRUCT,
                          .type_name = items->type_name,
                          .container = PIN_CONTAINER_ARRAY,
                          .ptr = items };
}

/* ---------- Revision ---------- */

static uint64_t mix(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * 1099511628211ull;
}

static uint64_t mix_text(uint64_t hash, const char *text)
{
    for (const char *c = text; *c; c++) {
        hash = mix(hash, (unsigned char)*c);
    }
    return mix(hash, 0);
}

/* Stands for everything the reads above look at, as counters their owners
   keep: it differs after a change, and between changes it is the same, so
   a panel refreshes only when it moved. A scene edit invalidates the asset
   index, which then counts it, as does an Undo or Redo the history's heads;
   the selection and the view flags count their own. A node that reads
   something new adds its counter here. */
uint64_t studio_nodes_stamp(const studio *s)
{
    const app_scene_authoring *scene = s->scene;
    const uint64_t counters[] = {
        scene->asset_index_generation,
        scene->asset_index_revision,
        (uintptr_t)scene->history,
        scene_history_step_id(scene->history, false),
        scene_history_step_id(scene->history, true),
        (uint64_t)acount(scene->actors),
        (uint64_t)acount(scene->prototypes),
        (uint64_t)scene->selected,
        scene->selection_revision,
        save_revision(scene->view_store),
        studio_editable(s),
        studio_live(s),
        s->mac,
        s->rename.entity,
        s->rename.outliner,
    };
    uint64_t stamp = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof counters / sizeof *counters; i++) {
        stamp = mix(stamp, counters[i]);
    }
    stamp = mix_text(stamp, s->outliner_filter);
    stamp = mix_text(stamp, scene->scene_path ? scene->scene_path : "");
    for (int i = 0; i < acount(scene->spawn_breaks); i++) {
        stamp = mix_text(stamp, scene->spawn_breaks[i]);
    }
    return stamp;
}

/* ---------- Effects ---------- */

/* Selects the object as a click on its row does: alone, Toggle in or out
   of the selection, or with Extend every object the object filter shows
   from the anchor through it. */
static void select_object(studio *s, const pin_value *a)
{
    int index = studio_object_index(s, a[0].str);
    int *rows = studio_outliner_rows(s), row = row_in(rows, index);
    if (row >= 0) {
        finish_transform_edit(s->scene, false);
        if (a[2].b) {
            studio_select_range(s, rows, acount(rows), row);
        } else if (a[1].b) {
            scene_toggle_instance(s->scene, index);
            s->range.active = 0;
        } else {
            select_instance(s->scene, index);
            s->range.active = 0;
        }
    }
    afree(rows);
}

static void set_view(studio *s, const pin_value *a, unsigned flag)
{
    int index = studio_object_index(s, a[0].str);
    if (index >= 0) {
        studio_set_object_view(s, index, flag, a[1].b);
    }
}

static void drop_on_object(studio *s, const pin_value *a)
{
    int index = studio_object_index(s, a[0].str);
    if (index >= 0 && studio_editable(s) && a[1].str && a[2].str) {
        studio_drop_on(s, index, a[1].str, a[2].str);
    }
}

static const char *text_of(const pin_value *v)
{
    return v->str ? v->str : "";
}

const char *studio_node_run(studio *s, studio_node_op op, const pin_value *in, arena *storage, pin_value *out)
{
    int index;
    switch (op) {
        case STUDIO_NODE_COUNT: return "Invalid Studio operation";
        case STUDIO_NODE_SCENE_OBJECTS:
            scene_objects(s, in, storage, out);
            break;
        case STUDIO_NODE_SCENE_INFO: scene_info(s, storage, out); break;
        case STUDIO_NODE_SELECTION: selection(s, storage, out); break;
        case STUDIO_NODE_OBJECT_SELECTED:
            index = studio_object_index(s, in[0].str);
            out[0] = bool_value(index >= 0 && scene_instance_selected(s->scene, index));
            break;
        case STUDIO_NODE_OBJECT_ROW: {
            int *rows = studio_outliner_rows(s);
            out[0] = int_value(row_in(rows, studio_object_index(s, in[0].str)));
            afree(rows);
            break;
        }
        case STUDIO_NODE_RENAMING: renaming(s, storage, out); break;
        case STUDIO_NODE_EDITOR_STATE:
            out[0] = bool_value(studio_editable(s));
            out[1] = bool_value(studio_live(s));
            out[2] = bool_value(s->mac);
            break;
        case STUDIO_NODE_SPAWN_BREAK:
            out[0] = bool_value(
                text_of(&in[0])[0] &&
                app_scene_authoring_spawn_break(s->scene, in[0].str)
            );
            break;
        case STUDIO_NODE_PROTOTYPE_CHOICES:
            prototype_choices(s, storage, out);
            break;
        case STUDIO_NODE_SELECT_OBJECT: select_object(s, in); break;
        case STUDIO_NODE_RENAME_OBJECT:
            if (studio_object_index(s, in[0].str) >= 0) {
                studio_rename_commit(s, strtoull(in[0].str, NULL, 10), text_of(&in[1]));
            }
            break;
        case STUDIO_NODE_END_RENAME: s->rename.entity = 0; break;
        case STUDIO_NODE_SET_HIDDEN:
            set_view(s, in, APP_SCENE_VIEW_HIDDEN);
            break;
        case STUDIO_NODE_SET_LOCKED:
            set_view(s, in, APP_SCENE_VIEW_LOCKED);
            break;
        case STUDIO_NODE_ISOLATE:
            index = studio_object_index(s, in[0].str);
            if (index >= 0) {
                studio_isolate(s, index);
            }
            break;
        case STUDIO_NODE_ADD_OBJECT:
            if (text_of(&in[0])[0]) {
                studio_add_object(s, in[0].str);
            }
            break;
        case STUDIO_NODE_DROP_ON_OBJECT: drop_on_object(s, in); break;
        case STUDIO_NODE_MOVE_OBJECTS:
            studio_outliner_drop(s, text_of(&in[0]), text_of(&in[1]), text_of(&in[2]));
            break;
        case STUDIO_NODE_SET_FILTER:
            snprintf(s->outliner_filter, sizeof s->outliner_filter, "%s", text_of(&in[0]));
            break;
        case STUDIO_NODE_TOGGLE_SPAWN_BREAK:
            if (text_of(&in[0])[0]) {
                studio_toggle_spawn_break(s, in[0].str);
            }
            break;
    }
    return NULL;
}
