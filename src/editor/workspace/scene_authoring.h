#ifndef CHIBI_APP_SCENE_AUTHORING_H
#define CHIBI_APP_SCENE_AUTHORING_H
#include "authoring/preview.h"
#include "editor/workspace/viewport.h"
#include "runtime/engine.h"
#include "runtime/scenes/physics_settings.h"

typedef struct app_scene_authoring app_scene_authoring;
/* Authored world settings; editable while Scene and prototype preview are stopped.
 * Records one undo step. Invalid settings leave the world unchanged. */
int app_scene_authoring_set_physics(app_scene_authoring *scene, physics_settings settings);
/* The root ui document the scene's UI shows on: its own, else the project's
 * default, else NULL. */
const char *app_scene_authoring_page(const app_scene_authoring *scene);
/* Names the scene's root ui document; NULL or "" uses the project's. */
void app_scene_authoring_set_ui(app_scene_authoring *scene, const char *ui);
/* The project's default root ui document; NULL for none. */
void app_scene_authoring_set_default_ui(app_scene_authoring *scene, const char *ui);
/* The project is Studio's own: its worlds take the editor nodes. */
void app_scene_authoring_set_studio_project(app_scene_authoring *scene);
typedef struct editor editor;
typedef const char *(*app_scene_prototype_resolve_fn)(const char *name, void *user);

typedef enum
{
    APP_RUNTIME_PREVIEW,
    APP_RUNTIME_SCENE,
} app_runtime_target;

typedef enum
{
    APP_RUNTIME_PLAY,
    APP_RUNTIME_PAUSE,
    APP_RUNTIME_STEP,
    APP_RUNTIME_RESTART,
    APP_RUNTIME_STOP,
} app_runtime_command;

typedef struct
{
    app_runtime_target target;
    bool playing;
    ecs_entity selected_entity;
    int component_count;
    int behavior_count;
    const char *message;
} app_runtime_state;

typedef enum
{
    APP_DEBUG_NONE,
    APP_DEBUG_AVAILABLE,
    APP_DEBUG_UNAVAILABLE,
    APP_DEBUG_OTHER_GRAPH,
    APP_DEBUG_STALE,
} app_debug_status;

/* Pins the debug target to the entity's asset instance, so the graph reads it
 * while the selection moves; unpinning returns it to the selection. */
int app_scene_authoring_set_debug_target(
    app_scene_authoring *scene,
    app_runtime_target target,
    ecs_entity entity,
    const char *asset_name
);
void app_scene_authoring_set_debug_pinned(app_scene_authoring *scene, bool pinned);
/* Debugs the entity's instance of the open behavior, or nothing for a null
 * entity (No target), until the selection or the open behavior changes, as
 * Unreal's debug object menu does. -1 when it has no such instance. */
int app_scene_authoring_choose_debug_target(app_scene_authoring *scene, ecs_entity entity);
/* Debugs the newest scene instance of the open behavior on a `prototype`
 * object, or the first to spawn, and each newer one as the game spawns it
 * (Unreal's debug filter), until a target is chosen or selected. */
void app_scene_authoring_follow_newest(app_scene_authoring *scene, const char *prototype);
/* Selects a running scene object, placed or spawned, keeping an open
 * behavior's tab, as the debugger does for the instance it stops in. */
int app_scene_authoring_select_entity(app_scene_authoring *scene, ecs_entity entity);
/* While no graph area shows the open behavior, no instance traces. */
void app_scene_authoring_set_debug_hidden(app_scene_authoring *scene, bool hidden);

typedef enum
{
    APP_EDITOR_DOCUMENT_SCENE,
    APP_EDITOR_DOCUMENT_PROTOTYPE,
    APP_EDITOR_DOCUMENT_BEHAVIOR,
    APP_EDITOR_DOCUMENT_SYSTEM,
    APP_EDITOR_DOCUMENT_UI, /* a ui asset or stylesheet, identified by its absolute path */
    APP_EDITOR_DOCUMENT_RELATIONSHIP, /* static .chibi source, identified by absolute path */
} app_editor_document_kind;

typedef struct
{
    app_editor_document_kind kind;
    const char *id; /* borrowed stable asset or scene identity */
} app_editor_document_context;

app_scene_authoring *app_scene_authoring_new(
    ecs_world *world,
    preview_world *live,
    preview_definition *definition,
    const behavior_asset *editor_working,
    editor *graph_editor,
    app_scene_prototype_resolve_fn resolve_prototype,
    void *resolve_user,
    engine *runtime
);
app_scene_authoring *app_scene_authoring_new_with_content_root(
    ecs_world *world,
    preview_world *live,
    preview_definition *definition,
    const behavior_asset *editor_working,
    editor *graph_editor,
    app_scene_prototype_resolve_fn resolve_prototype,
    void *resolve_user,
    engine *runtime,
    const char *content_root
);
void app_scene_authoring_set_editor_working(app_scene_authoring *scene, const behavior_asset *editor_working);
int app_scene_authoring_apply_pending(app_scene_authoring *scene);
int app_scene_authoring_set_content_root(app_scene_authoring *scene, const char *path);
bool app_scene_authoring_refresh_content(app_scene_authoring *scene);
int app_scene_authoring_load_content(app_scene_authoring *scene);
int app_scene_authoring_register_prototype(
    app_scene_authoring *scene,
    const char *json,
    const char *path
);
int app_scene_authoring_open_prototype(app_scene_authoring *scene, const char *name);
int app_scene_authoring_save_prototype(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *path
);
/* Struct storage is borrowed until the next edit. Native string field
 * descriptors and bytes are copied into caller-owned storage. */
int app_scene_authoring_read_prototype_field(
    arena *storage,
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component,
    const char *field,
    pin_value *value
);
/* Prototype mutation commands return 0 when applied/no-op, 1 when queued
 * with the collaboration host, 2 when accepted but runtime application failed,
 * and -1 when rejected. Accepted edits retain their source and history. */
enum
{
    APP_PROTOTYPE_EDIT_PENDING = 1,
    APP_PROTOTYPE_EDIT_NOT_APPLIED = 2
};
int app_scene_authoring_set_prototype_field(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name,
    const char *field_name,
    pin_value value
);
/* Adds a component with its defaults, or removes one, as one step of the
 * prototype's Undo. Instances follow, keeping the values they override of a
 * removed component for its return. -1 when the prototype already has it,
 * or lacks it. */
int app_scene_authoring_add_prototype_component(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name
);
int app_scene_authoring_remove_prototype_component(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *component_name
);
void app_scene_authoring_free(app_scene_authoring *scene);
int app_scene_authoring_open_behavior(app_scene_authoring *scene, const char *asset_name);
int app_scene_authoring_open_system(app_scene_authoring *scene, const char *path);
int app_scene_authoring_attach_prototype_behavior(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *asset_name
);
int app_scene_authoring_detach_prototype_behavior(
    app_scene_authoring *scene,
    const char *prototype_name,
    const char *asset_name
);
int app_scene_authoring_set_runtime_target(app_scene_authoring *scene, app_runtime_target target);
const ecs_world *app_scene_authoring_preview_world(const app_scene_authoring *scene);
int app_scene_authoring_preview_prototype(app_scene_authoring *scene, const char *prototype_name);
void app_scene_authoring_stop_prototype_preview(app_scene_authoring *scene);
int app_scene_authoring_runtime_command(
    app_scene_authoring *scene,
    app_runtime_target target,
    app_runtime_command command
);
app_runtime_state app_scene_authoring_runtime_state(const app_scene_authoring *scene);
/* Playback speed for both runtimes (engine_set_time_scale); Stop returns it
 * to 1, as Unity's Play Mode does Time.timeScale. */
void app_scene_authoring_set_time_scale(app_scene_authoring *scene, double scale);
double app_scene_authoring_time_scale(const app_scene_authoring *scene);
/* Break When Spawned, per prototype: the playing scene pauses after the
 * frame that spawns one and selects it, which makes it the debug target.
 * Kept for the editor session, as breakpoints set while debugging are. */
bool app_scene_authoring_spawn_break(const app_scene_authoring *scene, const char *prototype);
void app_scene_authoring_set_spawn_break(app_scene_authoring *scene, const char *prototype, bool on);
const engine_frame_timing *app_scene_authoring_runtime_timing(const app_scene_authoring *scene);
int app_scene_authoring_save(app_scene_authoring *scene, const char *path);
int app_scene_authoring_load(app_scene_authoring *scene, const char *path);
int app_scene_authoring_reload(app_scene_authoring *scene);

/* What a content file another program changed did to the editor. */
typedef enum
{
    APP_DISK_NONE, /* nothing open uses it */
    APP_DISK_RELOADED,
    APP_DISK_FAILED, /* the previous version stays; the message says why */
    APP_DISK_UNSAVED, /* its document keeps unsaved edits until reloaded by path */
} app_disk_result;

typedef struct
{
    app_disk_result result;
    char *path;
    char message[256];
} app_disk_change;

/* Reloads content files that changed on disk, checking at most once a second:
 * graph assets and systems at once, prototypes and scenes once the runtime
 * stops. Returns an owned array of what used them, or NULL. */
app_disk_change *app_scene_authoring_sync_disk(app_scene_authoring *scene, double now);
void app_disk_changes_free(app_disk_change *changes);
/* Replaces the document at `path` with the file, discarding unsaved edits;
 * the message says what happened. */
app_disk_result app_scene_authoring_reload_path(app_scene_authoring *scene, const char *path);

const char *app_scene_authoring_message(const app_scene_authoring *scene);
/* Select a live host-spawned entity without adding it to the authored scene. */
int app_scene_authoring_select_runtime_entity(app_scene_authoring *scene, ecs_entity entity);
app_editor_document_context app_scene_authoring_document(const app_scene_authoring *scene);
/* Catalogs, inspection and asset index upkeep without drawing; ui_active defers indexing. */
void app_scene_authoring_update(app_scene_authoring *scene, bool ui_active);
int app_scene_authoring_draw_viewport(app_scene_authoring *scene, engine_viewport *viewport);
void app_scene_authoring_render(void *user);

/* What the Scene view shows of the game while the scene is edited, as Unity's
 * Scene view effects and Unreal's Show flags; a session shows the game. A
 * new scene shows lighting only. */
typedef struct
{
    /* Terrain, clouds and liquids as the game's first frame generates them,
       without Begin, behaviors or a step. */
    bool generated_world;
    bool lighting;  /* HRC lighting; off shows material colors unlit */
    bool fog;       /* the fog of war as the game starts */
    bool cameras;   /* each Camera's frame, labeled */
    bool colliders; /* every Collider3D's outline, not only the selection's */
} app_scene_view_options;
void app_scene_authoring_set_view_options(app_scene_authoring *scene, app_scene_view_options options);
/* The generated world has waited on regeneration or streaming long enough to
 * say so. */
bool app_scene_authoring_world_pending(const app_scene_authoring *scene);
/* Why the world could not generate, or "". */
const char *app_scene_authoring_world_error(const app_scene_authoring *scene);

/* View Through Camera (Blender's Numpad 0): frames the Scene view exactly on
 * a Camera's span, the selected object's or else the first enabled one's,
 * and follows it until the view moves or a session starts. Leaving restores
 * the view from before. -1 without a camera or before the first draw. */
int app_scene_authoring_view_through_camera(app_scene_authoring *scene, bool on);
bool app_scene_authoring_viewing_through_camera(const app_scene_authoring *scene);
/* The span View Through Camera would show; false without a camera. */
bool app_scene_authoring_camera_framing(app_scene_authoring *scene, engine_viewport_view *view);

typedef struct
{
    const char *name; /* the object's, borrowed until the scene changes */
    float x, y; /* the frame's top-left corner in the view, fractions from its top-left */
    bool active, disabled, selected;
} app_scene_camera_label;
/* The camera frames the Scene view draws, as of its last draw, for a host's
 * labels: their count, writing at most `max`. */
int app_scene_authoring_camera_labels(
    app_scene_authoring *scene,
    app_scene_camera_label *labels,
    int max
);
/* The Scene view's zoom against its last framing, which a fit, Frame or
 * Reset Camera sets: 1 as framed, 2 twice as close. Call
 * app_scene_authoring_framed after framing the view another way. 0 before
 * the first framing and in the model preview. */
float app_scene_authoring_zoom(app_scene_authoring *scene);
void app_scene_authoring_framed(app_scene_authoring *scene);
/* An object nothing draws, with no appearance, tilemap or surface, which a
 * host marks with an icon a click on it picks (scene_tool_input's
 * marker_radius). */
typedef struct
{
    int actor;  /* in the scene's objects */
    float x, y; /* its place in the view, fractions from its top-left */
    bool selected, active, locked;
} app_scene_marker;
/* The objects to mark in the 2D view while the scene is edited, as of its
 * last draw: their count, writing at most `max`. */
int app_scene_authoring_markers(app_scene_authoring *scene, app_scene_marker *markers, int max);
int app_scene_authoring_frame(app_scene_authoring *scene, double monotonic_time);
int app_scene_authoring_move(app_scene_authoring *scene, float x, float y);
/* Sample aiming independently of movement; points use game coordinates. */
int app_scene_authoring_aim(app_scene_authoring *scene, CF_V2 direction, bool firing);
int app_scene_authoring_aim_at(app_scene_authoring *scene, CF_V2 point, bool firing);
int app_scene_authoring_input(app_scene_authoring *scene, const char *action, double value, int pressed);

/* Authored lifecycle actions, recorded in Scene Undo. */
int app_scene_authoring_add_instance(app_scene_authoring *scene, const char *prototype_name);
int app_scene_authoring_duplicate_instance(app_scene_authoring *scene);
int app_scene_authoring_delete_instance(app_scene_authoring *scene);
/* Names the object `entity` authors; the name must be unused and not empty.
 * Nothing references objects by name, so nothing else changes. -1 with the
 * message set when the name is refused. */
int app_scene_authoring_rename_instance(app_scene_authoring *scene, ecs_entity entity, const char *name);

/* Moves the selected top-level subtrees in one undo step, preserving their
 * world placement and stable identities. Descendants selected with a parent
 * move once. A sibling name conflict, cycle, invalid transform conversion, or
 * running scene rejects the entire operation. ROOT appends to scene roots. */
typedef enum
{
    APP_SCENE_DROP_BEFORE,
    APP_SCENE_DROP_INSIDE,
    APP_SCENE_DROP_AFTER,
    APP_SCENE_DROP_ROOT,
} app_scene_drop;
bool app_scene_authoring_can_drop(app_scene_authoring *scene, ecs_entity target, app_scene_drop drop);
/* 0 applied/no-op, 1 waiting for the collaboration host, -1 with message. */
int app_scene_authoring_drop_selection(app_scene_authoring *scene, ecs_entity target, app_scene_drop drop);

/* Editor view state (Blender's Outliner eye and selectability, Unity's Scene
 * visibility and pickability): while the scene is edited, its view leaves
 * hidden objects out and picks neither hidden nor locked ones. It belongs to
 * the editor, by scene file and object name, never to the scene, its Undo or
 * Play. */
enum
{
    APP_SCENE_VIEW_HIDDEN = 1 << 0,
    APP_SCENE_VIEW_LOCKED = 1 << 1,
};
unsigned app_scene_authoring_actor_view(app_scene_authoring *scene, int actor);
void app_scene_authoring_set_actor_view(app_scene_authoring *scene, int actor, unsigned flags);
/* Every scene's view state by scene path, in memory until the host binds the
 * store to storage (save_open) and commits it. */
typedef struct save save;
save *app_scene_authoring_view_store(app_scene_authoring *scene);
/* Adds a component with its defaults to the selected object, overriding its
 * prototype, or removes one it added, as one Undo step. 1 while a
 * collaboration host decides; -1 when the object has the component (or
 * lacks it, or its prototype has it, for remove), was spawned while
 * playing, or the scene runs. */
int app_scene_authoring_add_instance_component(app_scene_authoring *scene, const char *component_name);
int app_scene_authoring_remove_instance_component(app_scene_authoring *scene, const char *component_name);
int app_scene_authoring_set_instance_behavior(
    app_scene_authoring *scene,
    const char *asset_name,
    bool attached
);

/* Owned, versioned workspace JSON; restore only into a disposable candidate. */
char *app_scene_authoring_checkpoint(app_scene_authoring *scene);
int app_scene_authoring_restore_checkpoint(app_scene_authoring *scene, const char *json);

#endif
