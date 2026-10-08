#ifndef CHIBI_APP_SCENE_HISTORY_H
#define CHIBI_APP_SCENE_HISTORY_H

#include "ecs/ecs.h"
#include "runtime/components.h"
#include "runtime/scenes/physics_settings.h"
#include "support/asset_value.h"

#include <stdbool.h>

typedef struct scene_history scene_history;

scene_history *scene_history_new();
bool scene_history_push_physics(scene_history *history, game_physics_settings before, game_physics_settings after);
const game_physics_settings *scene_history_peek_physics(const scene_history *history, bool redo);
void scene_history_complete_physics(scene_history *history, bool redo);
void scene_history_free(scene_history *history);
void scene_history_clear(scene_history *history);
/* Explicit scopes only; nesting is invalid. Empty scopes preserve redo.
 * IDs survive replay. Zero means an ungrouped head or an empty stack.
 * Replay one entry at a time while the nonzero head ID matches. On failure,
 * reverse exactly the number of completed entries, not the entire group. */
void scene_history_begin_group(scene_history *history);
void scene_history_end_group(scene_history *history);
uint64_t scene_history_group_id(const scene_history *history, bool redo);
/* The head step's id on the Undo or Redo stack: its group's, or a lone
 * edit's own. Ids grow with each new step and never repeat; zero means an
 * empty stack or an edit restored from a capture. */
uint64_t scene_history_step_id(const scene_history *history, bool redo);
/* Records an already-applied transform edit. A no-op preserves redo. */
bool scene_history_push(scene_history *history, ecs_entity entity, GameTransform before, GameTransform after);
typedef struct
{
    ecs_component_id component;
    int field;
    pin_value before;
    pin_value after;
} scene_field_edit;

/* Records already-applied scalar or plain reflected-struct edits as one undo
 * step. Owns copied strings, struct bytes and nominal type names. Unsupported
 * values or duplicate fields reject the batch; empty/no-op batches preserve
 * redo. Replay validates every field before writing. */
bool scene_history_push_fields(
    scene_history *history,
    ecs_entity entity,
    const scene_field_edit *fields,
    int count
);
bool scene_history_push_field(
    scene_history *history,
    ecs_entity entity,
    ecs_component_id component,
    int field,
    pin_value before,
    pin_value after
);
typedef struct
{
    uint64_t identity;
    int index;
    const char *name;
    const char *prototype_name;
    av_value *base;
    av_value *state;
    bool created;
    av_value *retained;
} scene_actor_edit;

/* The app owns actor replay. History owns cloned values and copied strings. Peek returns
 * NULL unless the selected stack's head is an actor edit; outputs then remain
 * unchanged. present says whether replay must restore or remove the actor. */
bool scene_history_push_actor(scene_history *history, ecs_entity entity, const scene_actor_edit *edit);
const scene_actor_edit *scene_history_peek_actor(
    const scene_history *history,
    bool redo,
    ecs_entity *entity,
    bool *present
);
/* Call only after successful replay. Restoration supplies the new full handle
 * and rebinds this actor's older/newer edits; removal supplies a zero handle.
 * The peeked pointer is borrowed until the next history mutation. */
void scene_history_complete_actor(scene_history *history, bool redo, ecs_entity replacement);

typedef struct
{
    const char *const *before;
    int before_count;
    const char *const *after;
    int after_count;
} scene_behavior_edit;

/* Ordered names are copied and owned by history. Invalid lists (negative count,
 * missing/empty names or duplicates) and unchanged lists preserve both stacks.
 * The app restores before on Undo or after on Redo, then completes only success. */
bool scene_history_push_behaviors(scene_history *history, ecs_entity entity, const scene_behavior_edit *edit);
const scene_behavior_edit *scene_history_peek_behaviors(
    const scene_history *history,
    bool redo,
    ecs_entity *entity
);
void scene_history_complete_behaviors(scene_history *history, bool redo);

/* A prototype's definition before and after a step that changed it from the
 * scene, as Apply Overrides to Prototype does, so one Undo covers the
 * prototype and the instances that follow it. History owns copies. The app
 * replays it: peek returns the definition the head restores (before on Undo,
 * after on Redo), or NULL unless the head is one; complete after success. */
typedef struct
{
    const char *name;
    av_value *before;
    av_value *after;
} scene_prototype_edit;
bool scene_history_push_prototype(scene_history *history, const scene_prototype_edit *edit);
const scene_prototype_edit *scene_history_peek_prototype(const scene_history *history, bool redo);
void scene_history_complete_prototype(scene_history *history, bool redo);

/* Records an already-applied rename of the entity's object; history owns
 * copies of the names. An unchanged name preserves both stacks. The app
 * replays it: peek returns the name the head gives the object (before on
 * Undo, after on Redo), or NULL unless the head is a rename; complete only
 * after success. */
bool scene_history_push_rename(
    scene_history *history,
    ecs_entity entity,
    const char *before,
    const char *after
);
const char *scene_history_peek_rename(const scene_history *history, bool redo, ecs_entity *entity);
void scene_history_complete_rename(scene_history *history, bool redo);

bool scene_history_can_undo(const scene_history *history);
bool scene_history_can_redo(const scene_history *history);
/* Returns 1 after applying, 0 when empty, or -1 if the entity/component/field
 * is missing, its reflected schema no longer matches, or the head requires app-owned actor/behavior/rename replay. Failure preserves both
 * stacks and all component values. out_entity is
 * optional and is assigned only after applying. Entities are never recreated. */
int scene_history_undo(scene_history *history, ecs_world *world, ecs_entity *out_entity);
int scene_history_redo(scene_history *history, ecs_world *world, ecs_entity *out_entity);

av_value *scene_history_capture(const scene_history *history);
bool scene_history_restore_value(
    scene_history *history,
    av_value *value,
    const uint64_t *old_entities,
    const uint64_t *new_entities,
    int count
);

char *scene_history_checkpoint(const scene_history *history);
bool scene_history_restore_checkpoint(
    scene_history *history,
    const char *json,
    const uint64_t *old_entities,
    const uint64_t *new_entities,
    int count
);

#endif
