#include "authoring/material_prepare.h"
#include "execution/status.h"
#include "editor/collab/actions_scene.h"
#include "editor/workspace/model_preview.h"
#include "editor/workspace/scene_internal.h"
#include "render/shader_catalog.h"
#include "runtime/component_catalog.h"
#include "types/array.h"
#include "types/type.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void invalidate_runtime_snapshot(app_scene_authoring *scene)
{
    scene_kept_clear(scene);
    scene->runtime_selected = (ecs_entity){ 0 };
    for (int i = 0; i < acount(scene->actors); i++) {
        prototype_free(scene->actors[i].runtime_snapshot);
        scene->actors[i].runtime_snapshot = NULL;
        scene->actors[i].runtime_parent = 0;
    }
    scene->runtime_snapshot_valid = false;
}

static int capture_runtime_snapshot(app_scene_authoring *scene)
{
    if (scene->runtime_snapshot_valid) {
        return 0;
    }
    prototype **snapshots = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene_actor *actor = &scene->actors[i];
        prototype *captured = prototype_capture_entity(
            actor->prototype_name,
            scene->world,
            actor->entity,
            &scene->registry
        );
        if (!captured) {
            goto fail;
        }
        /* Re-clone with the authored identity so prototype tags stay stable. */
        free(captured->identity);
        captured->identity = xstrdup(actor->base->identity);
        char error[256];
        prototype *snapshot = prototype_clone(captured, error, sizeof error);
        prototype_free(captured);
        if (!snapshot) {
            goto fail;
        }
        apush(snapshots, snapshot);
        int count = scene_debug_attachment_count_for_entity(
            scene->live->runtime->sources,
            actor->entity
        );
        for (int j = 0; j < count; j++) {
            scene_debug_attachment info;
            if (scene_debug_attachment_at(scene->live->runtime->sources, actor->entity, j, &info) !=
                    0 ||
                prototype_add_graph(snapshot, info.asset_name) != 0) {
                goto fail;
            }
        }
    }
    for (int i = 0; i < acount(snapshots); i++) {
        scene->actors[i].runtime_snapshot = snapshots[i];
        scene->actors[i].runtime_parent = scene_actor_parent(scene, &scene->actors[i]);
    }
    afree(snapshots);
    scene->runtime_snapshot_valid = true;
    return 0;
fail:
    free_behavior_snapshots(snapshots);
    snprintf(scene->message, sizeof scene->message, "Could not capture authored Scene state.");
    return -1;
}

/* Respawns the actor under its authored parent, which respawned before it. */
static int restart_runtime_actor(app_scene_authoring *scene, scene_actor *actor)
{
    uint64_t parent = scene_actor_parent(scene, actor);
    assert(actor->runtime_snapshot);
    const prototype_spawn_hooks hooks = { attach_prototype_behavior, scene };
    char error[256];
    ecs_entity entity = { 0 };
    if (!authoring_prototype_parameters(scene->live->runtime->entities, actor->runtime_snapshot, error, sizeof error) ||
        prototype_spawn(actor->runtime_snapshot, scene->world, &hooks, &entity, error, sizeof error) !=
            ECS_OK) {
        return -1;
    }
    actor->entity = entity;
    const scene_actor *holder = parent ? scene_actor_by_identity(scene, parent) : NULL;
    assert(!parent || holder);
    if (holder) {
        int placed = ecs_world_record_add_pair(
                         scene->world,
                         entity,
                         ECS_CHILD_OF,
                         holder->entity,
                         0
                     ) ||
                     ecs_world_apply_commands(scene->world);
        assert(placed == 0);
    }
    return 0;
}

/* Destroys the replaced entities, children first, which destroying their
   parent would destroy unseen. */
static void destroy_replaced(app_scene_authoring *scene, ecs_entity *replaced)
{
    for (int i = acount(replaced) - 1; i >= 0; i--) {
        if (ecs_entity_is_alive(scene->world, replaced[i])) {
            source_detach_entity(scene->live->runtime->sources, replaced[i]);
            ecs_world_destroy_entity(scene->world, replaced[i]);
        }
    }
    afree(replaced);
}

static int restart_runtime(app_scene_authoring *scene)
{
    engine_reset(scene->runtime);
    render_shader_clear_world(scene->world);
    finish_transform_edit(scene, false);
    if (capture_runtime_snapshot(scene) != 0) {
        return -1;
    }
    scene_history_clear(scene->history);
    scene->debug_sessions[APP_RUNTIME_SCENE]++;
    editor_set_inspection_blocked(scene->graph_editor, true);
    scene->live->runtime->deferred = true;
    scene->runtime_selected = (ecs_entity){ 0 };
    entities_reset(scene->live->runtime->entities);
    particles_clear(scene->live->runtime->particles);
    /* Parents come before their children. */
    ecs_entity *replaced = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        apush(replaced, scene->actors[i].entity);
    }
    int respawned = 0;
    while (respawned < acount(scene->actors) &&
           restart_runtime_actor(scene, &scene->actors[respawned]) == 0) {
        respawned++;
    }
    asetlen(replaced, respawned);
    destroy_replaced(scene, replaced);
    if (respawned < acount(scene->actors)) {
        snprintf(scene->message, sizeof scene->message, "Could not restart Scene Runtime.");
        return -1;
    }
    /* The respawned objects name each other again. */
    scene_link_paths(scene);
    if (entities_rebuild_global_bindings(scene->live->runtime->entities) != 0) {
        snprintf(scene->message, sizeof scene->message, "Global binding names must be unique.");
        return -1;
    }
    if (scene->selected >= 0 && scene->selected < acount(scene->actors)) {
        select_instance(scene, scene->selected);
    }
    return 0;
}

bool runtime_session(const app_scene_authoring *scene)
{
    return scene->runtime_snapshot_valid || scene->preview_prototype_id ||
           scene->model_preview;
}

/* ---------- Keep Changes ---------- */

static void kept_free(scene_kept *kept)
{
    for (int i = 0; i < acount(kept->fields); i++) {
        scene_property_value_free(&kept->fields[i].after);
    }
    afree(kept->fields);
}

void scene_kept_clear(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->kept); i++) {
        kept_free(&scene->kept[i]);
    }
    afree(scene->kept);
}

void scene_kept_drop(app_scene_authoring *scene)
{
    assert(acount(scene->kept));
    kept_free(&alast(scene->kept));
    (void)apop(scene->kept);
}

/* Appends the entity's fields whose running value differs from `authored`. */
static void keep_component(
    app_scene_authoring *scene,
    ecs_entity entity,
    const prototype_component *authored,
    scene_kept *kept
)
{
    const ecs_component_desc *desc = authored->desc;
    for (int f = 0; f < desc->type->field_count; f++) {
        pin_value running = { 0 };
        if (ecs_component_read_field(scene->world, entity, desc->id, f, &running) != ECS_OK ||
            !editable_scalar(&running)) {
            continue;
        }
        pin_value before = running;
        pin_read_field(authored->data, &desc->type->fields[f], &before);
        if (pin_value_equal(&running, &before)) {
            continue;
        }
        scene_field_edit edit = { .component = desc->id, .field = f };
        if (!scene_property_value_copy(&edit.after, running)) {
            abort();
        }
        apush(kept->fields, edit);
    }
}

int scene_keep_changes(app_scene_authoring *scene, const ecs_component_desc *only)
{
    scene_actor *actor = selected_actor(scene);
    if (!scene->runtime_snapshot_valid || !actor ||
        !ecs_entity_is_alive(scene->world, actor->entity)) {
        return -1;
    }
    scene_kept kept = { .actor = actor->identity, .only = only };
    const prototype *authored = actor->runtime_snapshot;
    for (int i = 0; i < acount(authored->components); i++) {
        const prototype_component *component = &authored->components[i];
        if (component->data && (!only || component->desc->id == only->id) &&
            ecs_component_has(scene->world, actor->entity, component->desc->id)) {
            keep_component(scene, actor->entity, component, &kept);
        }
    }
    apush(scene->kept, kept);
    return acount(kept.fields);
}

/* Whether a later record of the same instance covers field `f` of record
   `index`, so the later one decides its value. */
static bool kept_superseded(const app_scene_authoring *scene, int index, int f)
{
    const scene_kept *kept = &scene->kept[index];
    for (int i = index + 1; i < acount(scene->kept); i++) {
        const scene_kept *later = &scene->kept[i];
        if (later->actor == kept->actor &&
            (!later->only || later->only->id == kept->fields[f].component)) {
            return true;
        }
    }
    return false;
}

int scene_kept_values(app_scene_authoring *scene, int *objects, const char **name)
{
    int values = 0;
    *objects = 0;
    *name = NULL;
    uint64_t *counted = NULL;
    for (int i = 0; i < acount(scene->kept); i++) {
        scene_actor *actor = scene_actor_by_identity(scene, scene->kept[i].actor);
        int count = 0;
        for (int f = 0; actor && f < acount(scene->kept[i].fields); f++) {
            count += !kept_superseded(scene, i, f);
        }
        bool seen = false;
        for (int j = 0; j < acount(counted); j++) {
            seen |= counted[j] == scene->kept[i].actor;
        }
        if (count && !seen) {
            apush(counted, scene->kept[i].actor);
            *name = acount(counted) == 1 ? actor->name : NULL;
        }
        values += count;
    }
    *objects = acount(counted);
    afree(counted);
    return values;
}

/* One kept value as an authored property edit, recorded and shared as
   Properties records one. */
static void apply_kept_field(app_scene_authoring *scene, ecs_entity entity, const scene_field_edit *kept)
{
    pin_value before = { 0 };
    if (ecs_component_read_field(scene->world, entity, kept->component, kept->field, &before) !=
        ECS_OK) {
        return;
    }
    scene->property_actor = entity;
    scene->property_edit = (scene_field_edit){ .component = kept->component,
                                               .field = kept->field };
    if (!scene_property_value_copy(&scene->property_edit.before, before) ||
        !scene_property_value_copy(&scene->property_edit.after, kept->after)) {
        abort();
    }
    scene->property_dirty = scene->scene_dirty;
    if (scene_collab_can_write_locally() &&
        ecs_component_write_field(scene->world, entity, kept->component, kept->field, kept->after) ==
            ECS_OK) {
        scene->scene_dirty = true;
    }
    finish_property_edit(scene);
}

/* After Stop restored the authored scene: each value its latest record
   decides, together one Undo step. */
static void apply_kept(app_scene_authoring *scene)
{
    if (!acount(scene->kept)) {
        return;
    }
    scene_history_begin_group(scene->history);
    for (int i = 0; i < acount(scene->kept); i++) {
        scene_actor *actor = scene_actor_by_identity(scene, scene->kept[i].actor);
        for (int f = 0; actor && f < acount(scene->kept[i].fields); f++) {
            if (!kept_superseded(scene, i, f)) {
                apply_kept_field(scene, actor->entity, &scene->kept[i].fields[f]);
            }
        }
    }
    scene_history_end_group(scene->history);
    asset_index_invalidate(scene);
    scene_kept_clear(scene);
}

int app_scene_authoring_set_runtime_target(app_scene_authoring *scene, app_runtime_target target)
{
    if (!scene || target < APP_RUNTIME_PREVIEW || target > APP_RUNTIME_SCENE) {
        return -1;
    }
    if (runtime_session(scene)) {
        snprintf(scene->message, sizeof scene->message, "Stop simulation to change target.");
        return -1;
    }
    if (target == APP_RUNTIME_PREVIEW) {
        scene_prototype_asset *asset = selected_prototype(scene);
        if (!asset) {
            return -1;
        }
        scene->runtime_preview_prototype = (int)(asset - scene->prototypes);
    }
    scene->active_runtime_target = target;
    sync_editor_inspection(scene);
    return 0;
}

const ecs_world *app_scene_authoring_preview_world(const app_scene_authoring *scene)
{
    return prototype_preview_world(scene->prototype_preview);
}

int app_scene_authoring_preview_prototype(app_scene_authoring *scene, const char *prototype_name)
{
    if (!scene) {
        return -1;
    }
    scene_prototype_asset *asset = find_prototype(scene, prototype_name);
    if (!asset || !asset->prototype) {
        return -1;
    }
    if (scene->runtime_snapshot_valid) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Stop the scene before previewing a prototype.");
        return -1;
    }
    bool continuing = previewed_prototype(scene) == asset;
    bool loop = continuing ? scene->preview_loop
                           : prototype_has_component_id(
                                 asset->prototype,
                                 COMPONENT_Projectile.id
                             );
    bool follow = continuing ? scene->preview_follow : loop;
    char error[256] = "";
    finish_transform_edit(scene, true);
    app_scene_authoring_stop_prototype_preview(scene);
    int configured = prototype_preview_configure_physics(
        scene->prototype_preview,
        physics_configuration(scene->live->runtime->physics)
    );
    assert(configured == 0);
    /* Source behaviors reach the preview's world as their sources publish
       (scene_stage_source_edits); only graph behaviors are copied here. */
    for (int i = 0; i < preview_definition_count(scene->live); i++) {
        preview_definition *definition = preview_definition_at(scene->live, i);
        if (definition->working_presentation) {
            continue;
        }
        if (prototype_preview_update_behavior(
                scene->prototype_preview,
                definition->name,
                definition->applied,
                true,
                error,
                sizeof error
            ) == 0) {
            continue;
        }
        if (!error[0]) {
            snprintf(error, sizeof error, "%s did not update", definition->name);
        }
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "Could not preview %s: %s.",
            prototype_name,
            error
        );
        return -1;
    }
    int result = prototype_preview_launch(scene->prototype_preview, asset->prototype, error, sizeof error);
    if (result == ECS_OK) {
        scene->active_runtime_target = APP_RUNTIME_PREVIEW;
        scene->preview_prototype_id = xstrdup(prototype_identity(asset->prototype));
        scene->runtime_preview_prototype = (int)(asset - scene->prototypes);
        const Transform *origin = Transform_get_const(
            prototype_preview_world(scene->prototype_preview),
            prototype_preview_entity(scene->prototype_preview)
        );
        scene->preview_origin = origin ? *origin : (Transform){ 0 };
    }
    scene->preview_playing = result == ECS_OK;
    scene->preview_loop = loop;
    scene->preview_follow = follow;
    scene->preview_step = false;
    snprintf(
        scene->prototype_message,
        sizeof scene->prototype_message,
        result == ECS_OK ? "Previewing %s." : "Could not preview %s: %s.",
        prototype_name,
        result == ECS_OK ? "" : error
    );
    return result;
}

void app_scene_authoring_stop_prototype_preview(app_scene_authoring *scene)
{
    if (scene) {
        if (scene->preview_prototype_id) {
            scene->debug_sessions[APP_RUNTIME_PREVIEW]++;
            editor_set_inspector(scene->graph_editor, NULL, 0);
        }
        render_shader_clear_world(prototype_preview_world(scene->prototype_preview));
        prototype_preview_stop(scene->prototype_preview);
        scene_model_preview_free(scene);
        free(scene->preview_prototype_id);
        scene->preview_prototype_id = NULL;
        scene->preview_playing = false;
        scene->preview_step = false;
        scene->active_runtime_target = APP_RUNTIME_SCENE;
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "Prototype preview stopped.");
    }
}

int app_scene_authoring_runtime_command(
    app_scene_authoring *scene,
    app_runtime_target target,
    app_runtime_command command
)
{
    if (!scene || target < APP_RUNTIME_PREVIEW || target > APP_RUNTIME_SCENE ||
        command < APP_RUNTIME_PLAY || command > APP_RUNTIME_STOP) {
        return -1;
    }
    if (target != scene->active_runtime_target && runtime_session(scene)) {
        snprintf(scene->message, sizeof scene->message, "Stop simulation to change target.");
        return -1;
    }
    if (target == APP_RUNTIME_PREVIEW && !scene->preview_prototype_id &&
        !scene->model_preview && command != APP_RUNTIME_STOP) {
        scene_prototype_asset *asset = prototype_at(scene, scene->runtime_preview_prototype);
        if (!asset ||
            app_scene_authoring_preview_prototype(scene, asset->prototype->name) != 0) {
            return -1;
        }
    }
    if (command == APP_RUNTIME_PAUSE || command == APP_RUNTIME_STOP ||
        command == APP_RUNTIME_RESTART) {
        app_scene_authoring_move(scene, 0, 0);
        app_scene_authoring_aim(scene, (CF_V2){ 0 }, false);
    }
    if (command == APP_RUNTIME_RESTART || command == APP_RUNTIME_STOP) {
        app_scene_renderer_reset(scene->renderer);
    }
    if (command == APP_RUNTIME_STOP) {
        app_scene_authoring_set_time_scale(scene, 1);
    }
    bool *playing = target == APP_RUNTIME_PREVIEW ? &scene->preview_playing
                                                  : &scene->runtime_playing;
    finish_transform_edit(scene, false);
    bool *step = target == APP_RUNTIME_PREVIEW ? &scene->preview_step
                                               : &scene->runtime_step;
    if (target == APP_RUNTIME_SCENE) {
        scene->active_runtime_target = APP_RUNTIME_SCENE;
        if (command == APP_RUNTIME_PLAY || command == APP_RUNTIME_STEP) {
            /* The game generates its own world on its first step. */
            if (!scene->runtime_snapshot_valid) {
                scene_world_preview_discard(scene);
            }
            if (capture_runtime_snapshot(scene) != 0) {
                return -1;
            }
            scene->live->runtime->deferred = false;
            if (source_begin(
                    scene->live->runtime->sources,
                    scene->live->runtime->now,
                    scene->live->runtime->delta
                ) != 0) {
                *playing = false;
                *step = false;
                engine_set_playing(scene->runtime, false);
                snprintf(
                    scene->message,
                    sizeof scene->message,
                    "Scene Begin failed: %s. Stop to return to editing.",
                    source_error(scene->live->runtime->sources)
                );
                return -1;
            }
        }
        if (command == APP_RUNTIME_PLAY || command == APP_RUNTIME_PAUSE) {
            *playing = command == APP_RUNTIME_PLAY;
            *step = false;
            return engine_set_playing(scene->runtime, *playing);
        }
        if (command == APP_RUNTIME_STEP) {
            *playing = false;
            *step = true;
            return engine_set_playing(scene->runtime, false) == 0
                     ? engine_step(scene->runtime)
                     : -1;
        }
        *playing = false;
        *step = false;
        if (engine_set_playing(scene->runtime, false) != 0) {
            return -1;
        }
        int result = restart_runtime(scene);
        if (result != 0) {
            return result;
        }
        if (command == APP_RUNTIME_STOP) {
            apply_kept(scene);
            invalidate_runtime_snapshot(scene);
            return 0;
        }
        scene->live->runtime->deferred = false;
        if (source_begin(
                scene->live->runtime->sources,
                scene->live->runtime->now,
                scene->live->runtime->delta
            ) != 0) {
            snprintf(
                scene->message,
                sizeof scene->message,
                "Scene Begin failed: %s. Stop to return to editing.",
                source_error(scene->live->runtime->sources)
            );
            return -1;
        }
        *playing = true;
        return engine_set_playing(scene->runtime, true);
    }
    if (command == APP_RUNTIME_PLAY || command == APP_RUNTIME_PAUSE) {
        scene->active_runtime_target = APP_RUNTIME_PREVIEW;
        *playing = command == APP_RUNTIME_PLAY;
        *step = false;
        return engine_set_playing(prototype_preview_runtime(scene->prototype_preview), *playing);
    }
    if (command == APP_RUNTIME_STEP) {
        scene->active_runtime_target = APP_RUNTIME_PREVIEW;
        *playing = false;
        *step = true;
        engine *preview = prototype_preview_runtime(scene->prototype_preview);
        engine_set_playing(preview, false);
        return engine_step(preview);
    }
    if (command == APP_RUNTIME_STOP) {
        app_scene_authoring_stop_prototype_preview(scene);
        return 0;
    }
    if (scene->model_preview) {
        scene_model_preview_reset_parameters(scene);
        return 0;
    }
    scene_prototype_asset *asset = previewed_prototype(scene);
    return asset ? app_scene_authoring_preview_prototype(scene, asset->prototype->name)
                 : -1;
}

void app_scene_authoring_set_time_scale(app_scene_authoring *scene, double scale)
{
    engine_set_time_scale(scene->runtime, scale);
    engine_set_time_scale(prototype_preview_runtime(scene->prototype_preview), scale);
}

double app_scene_authoring_time_scale(const app_scene_authoring *scene)
{
    return engine_time_scale(scene->runtime);
}

const engine_frame_timing *app_scene_authoring_runtime_timing(const app_scene_authoring *scene)
{
    engine *runtime = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                        ? prototype_preview_runtime(scene->prototype_preview)
                        : scene->runtime;
    return engine_last_timing(runtime);
}

app_runtime_state app_scene_authoring_runtime_state(const app_scene_authoring *scene)
{
    app_runtime_state state = { 0 };
    if (!scene) {
        return state;
    }
    state.target = scene->active_runtime_target;
    state.playing = state.target == APP_RUNTIME_PREVIEW ? scene->preview_playing
                                                        : scene->runtime_playing;
    ecs_world *world = NULL;
    source_registry *behaviors = NULL;
    if (state.target == APP_RUNTIME_PREVIEW) {
        world = prototype_preview_world(scene->prototype_preview);
        behaviors = prototype_preview_sources(scene->prototype_preview);
        state.selected_entity = prototype_preview_entity(scene->prototype_preview);
        state.message = scene->prototype_message;
    } else {
        world = scene->world;
        behaviors = scene->live ? scene->live->runtime->sources : NULL;
        state.selected_entity = selected_scene_entity(scene);
        state.message = scene->message;
    }
    if (world && state.selected_entity.slot) {
        for (int i = 0; i < ecs_world_component_count(world); i++) {
            const ecs_component_desc *desc = ecs_world_component_at(world, i);
            if (desc && ecs_component_has(world, state.selected_entity, desc->id)) {
                state.component_count++;
            }
        }
    }
    if (behaviors && state.selected_entity.slot) {
        state.behavior_count = scene_debug_attachment_count_for_entity(
            behaviors,
            state.selected_entity
        );
    }
    return state;
}

/* ---------- Break When Spawned ---------- */

static int compare_ids(const void *a, const void *b)
{
    uint64_t left = *(const uint64_t *)a, right = *(const uint64_t *)b;
    return (left > right) - (left < right);
}

static bool seen(const app_scene_authoring *scene, uint64_t id)
{
    return acount(scene->spawn_seen) &&
           bsearch(&id, scene->spawn_seen, (size_t)acount(scene->spawn_seen), sizeof id, compare_ids);
}

/* Notes the spawned objects now running and returns the last one new since
   the previous call whose prototype has a break, else a null entity. */
static ecs_entity watch_spawns(app_scene_authoring *scene)
{
    ecs_entity *running = NULL, spawned = { 0 };
    uint64_t *ids = NULL;
    entities_copy_live(scene->live->runtime->entities, &running);
    for (int i = 0; i < acount(running); i++) {
        uint64_t id = ecs_entity_to_u64(running[i]);
        const char *prototype = seen(scene, id)
                                  ? NULL
                                  : scene_entity_prototype(scene, running[i]);
        if (prototype && app_scene_authoring_spawn_break(scene, prototype)) {
            spawned = running[i];
        }
        apush(ids, id);
    }
    if (acount(ids)) {
        qsort(ids, (size_t)acount(ids), sizeof *ids, compare_ids);
    }
    afree(scene->spawn_seen);
    scene->spawn_seen = ids;
    afree(running);
    return spawned;
}

bool app_scene_authoring_spawn_break(const app_scene_authoring *scene, const char *prototype)
{
    for (int i = 0; i < acount(scene->spawn_breaks); i++) {
        if (!strcmp(scene->spawn_breaks[i], prototype)) {
            return true;
        }
    }
    return false;
}

void app_scene_authoring_set_spawn_break(app_scene_authoring *scene, const char *prototype, bool on)
{
    for (int i = 0; i < acount(scene->spawn_breaks); i++) {
        if (!strcmp(scene->spawn_breaks[i], prototype)) {
            free(scene->spawn_breaks[i]);
            adel(scene->spawn_breaks, i);
            break;
        }
    }
    if (on) {
        apush(scene->spawn_breaks, xstrdup(prototype));
        /* Only what spawns from now on breaks. */
        watch_spawns(scene);
    }
}

/* Pauses after the frame that spawned an object with a break, selecting it,
   which makes its instance of the open behavior the debug target. What a
   Step spawns is noted without pausing again. */
static void break_on_spawn(app_scene_authoring *scene)
{
    if (!acount(scene->spawn_breaks) || !scene->runtime_snapshot_valid ||
        scene->active_runtime_target != APP_RUNTIME_SCENE) {
        return;
    }
    ecs_entity spawned = watch_spawns(scene);
    if (!spawned.slot || !scene->runtime_playing ||
        app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PAUSE) !=
            0) {
        return;
    }
    app_scene_authoring_select_entity(scene, spawned);
    snprintf(
        scene->message,
        sizeof scene->message,
        "Paused: %s %u spawned",
        runtime_entity_label(scene, spawned),
        (unsigned)spawned.slot
    );
    scene_notice(scene);
}

int app_scene_authoring_frame(app_scene_authoring *scene, double monotonic_time)
{
    if (!scene) {
        return FLOW_ERR_ARGUMENT;
    }
    preview_behavior_edits(scene);
    int result = 0;
    ecs_entity preview_entity = prototype_preview_entity(scene->prototype_preview);
    engine *preview_runtime = prototype_preview_runtime(scene->prototype_preview);
    engine_set_focus(preview_runtime, engine_get_focus(scene->runtime));
    if (preview_entity.slot) {
        result = prototype_preview_tick(scene->prototype_preview, monotonic_time);
        if (result < 0) {
            snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", engine_error(preview_runtime));
        }
    }
    if (scene->preview_loop && scene->preview_playing && result >= 0 &&
        (engine_last_timing(preview_runtime)->simulation_time >= 2.0 ||
         !ecs_entity_is_alive(prototype_preview_world(scene->prototype_preview), preview_entity))) {
        scene_prototype_asset *asset = previewed_prototype(scene);
        if (asset) {
            result = app_scene_authoring_preview_prototype(
                scene,
                asset->prototype->name
            );
        }
    }
    int frame_result = engine_frame(scene->runtime, monotonic_time);
    if (frame_result < 0) {
        snprintf(scene->message, sizeof scene->message, "%s", engine_error(scene->runtime));
    }
    break_on_spawn(scene);
    app_scene_renderer_set_time(
        scene->renderer,
        (float)engine_last_timing(
            scene->active_runtime_target == APP_RUNTIME_PREVIEW ? preview_runtime
                                                                : scene->runtime
        )
            ->simulation_time
    );
    if (frame_result < 0 && result == 0) {
        result = frame_result;
    }
    scene->runtime_step = false;
    scene->preview_step = false;
    return result;
}

int app_scene_authoring_input(app_scene_authoring *scene, const char *action, double value, int pressed)
{
    if (!scene) {
        return FLOW_ERR_ARGUMENT;
    }
    if (scene->active_runtime_target == APP_RUNTIME_PREVIEW) {
        engine_set_focus(
            prototype_preview_runtime(scene->prototype_preview),
            engine_get_focus(scene->runtime)
        );
        return prototype_preview_input(scene->prototype_preview, action, value, pressed, NULL);
    }
    return engine_dispatch_input(scene->runtime, action, value, pressed, NULL);
}

int app_scene_authoring_move(app_scene_authoring *scene, float x, float y)
{
    if (!scene || !isfinite(x) || !isfinite(y)) {
        return FLOW_ERR_ARGUMENT;
    }
    bool preview = scene->active_runtime_target == APP_RUNTIME_PREVIEW;
    if (engine_get_focus(scene->runtime) != ENGINE_FOCUS_VIEWPORT ||
        !(preview ? scene->preview_playing : scene->runtime_playing)) {
        return FLOW_ERR_STATE;
    }
    if (preview) {
        engine_set_focus(
            prototype_preview_runtime(scene->prototype_preview),
            engine_get_focus(scene->runtime)
        );
        return prototype_preview_sample_movement(scene->prototype_preview, x, y);
    }
    return engine_sample_movement(scene->runtime, x, y);
}

static int sample_aim(app_scene_authoring *scene, CF_V2 value, bool firing, bool point)
{
    engine *runtime = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                        ? prototype_preview_runtime(scene->prototype_preview)
                        : scene->runtime;
    engine_set_focus(runtime, engine_get_focus(scene->runtime));
    return engine_sample_aim(runtime, value, firing, point);
}

int app_scene_authoring_aim(app_scene_authoring *scene, CF_V2 direction, bool firing)
{
    return sample_aim(scene, direction, firing, false);
}

int app_scene_authoring_aim_at(app_scene_authoring *scene, CF_V2 point, bool firing)
{
    return sample_aim(scene, point, firing, true);
}
