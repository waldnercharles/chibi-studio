#ifndef CHIBI_SCENE_INTERNAL_H
#define CHIBI_SCENE_INTERNAL_H
#include "assets/prototype_instance.h"
#include "assets/scenario_document.h"
#include "assets/scene_document.h"
#include "authoring/prototype_preview.h"
#include "authoring/source_catalog.h"
#include "authoring/studio_schema.h"
#include "editor/documents/scene_history.h"
#include "editor/documents/scene_retention.h"
#include "editor/documents/ui_buffer.h"
#include "editor/workspace/asset_index.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/scene_authoring.h"
#include "editor/workspace/scene_debug.h"
#include "editor/workspace/scene_gizmo.h"
#include "editor/workspace/viewport.h"
#include "render/scene_renderer.h"
#include "editor/workspace/tree_watch.h"

typedef struct scene_model_preview scene_model_preview;

typedef struct
{
    uint64_t identity;
    ecs_entity entity;
    char *name;
    char *prototype_name;
    game_prototype *base;
    game_prototype *runtime_snapshot;
    game_prototype *restored_state;
    scene_retained_component *retained;
    bool own_behaviors; /* the scene lists its behaviors instead of the prototype's */
    bool marked;
} scene_actor;

/* The objects selected before and after a history step, by identity, so
   Undo and Redo restore them as Unity and Blender do. */
typedef struct
{
    uint64_t *ids;
    uint64_t active;
} scene_selection_set;

typedef struct
{
    uint64_t step;
    scene_selection_set before, after;
} scene_step_selection;

/* An object a multi-object Properties edit also writes, and its value
   before the edit. */
typedef struct
{
    ecs_entity entity;
    pin_value before; /* owned, as scene_property_value_copy copies */
} scene_field_target;

/* An object the editor hides or locks in the open scene, by its name. */
typedef struct
{
    char *name;
    unsigned flags; /* APP_SCENE_VIEW_* */
} scene_view_entry;

typedef struct
{
    engine_viewport_view view;
    bool saved;
} scene_camera;

/* What the Generated World preview reads, hashed. */
typedef struct
{
    uint64_t terrain; /* terrain sources and field graphs */
    uint64_t sources; /* destructibles, fluid volumes and material worlds */
} scene_world_inputs;

/* The Generated World preview's state (scene_world_preview.c). */
typedef struct
{
    scene_world_inputs inputs;
    double changed_at;    /* when the inputs last changed */
    double waiting_since; /* since when terrain has streamed, or 0 */
    bool built;           /* the scene's materials hold a preview */
    bool dirty;           /* the inputs changed since it was built */
    bool terrain_dirty;   /* and terrain's among them */
    char error[256];
} scene_world_preview;

/* The preview reports waiting once it has waited this long. */
#define SCENE_WORLD_PENDING_SECONDS 0.1

/* View Through Camera (scene_overlays.c). */
typedef struct
{
    bool active;
    ecs_entity entity;
    engine_viewport_view before; /* restored on leaving */
    engine_viewport_view shown;  /* set last; any other view means it moved */
} scene_camera_view;
/* Keep Changes: an instance's running values that differ from its authored
   state, waiting for Stop. */
typedef struct
{
    uint64_t actor;                 /* identity */
    const ecs_component_desc *only; /* the component kept, or NULL for all */
    scene_field_edit *fields;       /* array; `after` holds each kept value */
} scene_kept;

/* An asset dragged over an object, in the field that names its kind
   (scene_asset_drop.c): the next draw shows it, then puts `before` back. */
typedef struct
{
    ecs_entity entity;
    ecs_component_id component;
    int field;
    char *text;       /* the asset's name, owned */
    pin_value before; /* while drawn; owned, as scene_property_value_copy copies */
    bool shown;
} scene_field_preview;

/* The Scene outlines an object a dragged asset would land on in the kit's
   drop color, a bright primary, beside the orange selection. */
#define SCENE_DROP_COLOR 0x5B8DEFFFu
/* The object a click would select, half the active selection's orange, as
   Figma and Unreal outline what the pointer rests on. */
#define SCENE_HOVER_COLOR 0xFFA53A80u

typedef struct
{
    char *path;
    game_prototype *prototype;
    int dirty;
    scene_camera camera;
    av_value **undo;
    av_value **redo;
} scene_prototype_asset;

typedef struct
{
    char *path;
    bool directory;
    bool image;
} scene_content_entry;

typedef struct
{
    app_editor_document_kind kind;
    char *key;
    char *label;
    char *path;
    bool pinned;
    av_value *scene_value;
    av_value *scene_history;
    uint64_t *scene_entities;
    uint64_t *scene_actor_ids;
    int scene_entity_count;
    uint64_t selected_entity;
    engine_viewport_view camera;
    bool camera_saved;
    bool scene_dirty;
    int selected;
    int selected_prototype;
    char *selected_prototype_name;
    av_value *asset_saved;
    unsigned system_editor_revision;
    ui_buffer *ui;     /* a UI document's text, owned; NULL for other kinds */
    bool source_dirty; /* exact code text differs from disk */
    bool source_only;  /* executable source whose graph has not loaded */
    av_value *source_state; /* source draft, history and selection; owned native checkpoint */
} scene_document_tab;

/* Private authoring state. Module replacement transfers versioned document
 * checkpoints, never this layout or its runtime pointers. */
struct app_scene_authoring
{
    char source_preview_hold[1024]; /* On Save source edits still update the authored graph. */
    app_scene_renderer *renderer;
    engine_viewport *render_viewport;
    bool perspective;
    ecs_world *world;
    game_preview_world *live;
    game_preview_definition *definition;
    const behavior_asset *editor_working;
    editor *graph_editor;
    unsigned editor_revision_seen;
    const behavior_asset *editor_behavior_asset_seen;
    game_preview_definition *editor_definition_seen;
    bool editor_state_seen;
    bool behavior_apply_pending;
    bool preview_retry;
    char preview_message[256];
    app_scene_prototype_resolve_fn resolve_prototype;
    void *resolve_user;
    game_prototype_registry registry;
    editor_choices component_choices;
    editor_choices type_choices;
    editor_choices image_choices; /* Labels and names borrow content paths. */
    editor_choices prototype_choices; /* Names are owned across prototype Undo/reload. */
    editor_choices global_choices; /* Names and labels are owned by this scene. */
    char *system_path;
    engine *runtime;
    game_prototype_preview *prototype_preview;
    scene_model_preview *model_preview;
    scene_camera model_camera;
    char *preview_prototype_id;
    int runtime_preview_prototype;
    scene_actor *actors;
    char **component_strings; /* Keeps scene-authored component string fields alive. */
    scene_prototype_asset *prototypes;
    scene_content_entry *content;
    reference_index *asset_index;
    unsigned asset_index_generation;
    unsigned asset_index_built_generation;
    uint64_t asset_index_revision; /* completed rebuilds, including explicit refresh */
    char *content_root;
    char *ui;         /* the scene's root ui document, or NULL for default_ui */
    char *default_ui; /* the project's, or NULL */
    bool studio_project; /* Studio's own project, whose behaviors call the editor nodes */
    studio_node_service studio_nodes; /* those calls' service, run by the window's Studio */
    tree_watch disk;  /* declarative content files as last loaded or saved */
    tree_watch code_disk;
    tree_watch code_metadata_disk;
    char **disk_pending; /* changes waiting for the runtime to stop */
    int selected;
    /* The selection each recent step began and ended with (scene_actors.c). */
    scene_step_selection *step_selections;
    scene_selection_set frame_selection; /* as the frame began */
    uint64_t selection_revision; /* grows when a frame begins with another selection than the last */
    uint64_t seen_step;
    ecs_entity reveal_actor;
    ecs_entity runtime_selected;
    int selected_prototype;
    scene_debug debug;
    app_runtime_target debug_target;
    uint64_t debug_sessions[2];
    bool debug_pinned; /* the debug target stays put while the selection moves */
    bool debug_hidden; /* no graph shows the open behavior: trace nothing */
    struct
    {
        ecs_entity entity;
        char *asset;
        uint64_t session;
        app_runtime_target target;
    } debug_followed; /* what the debug target last followed */
    /* Follow newest (Unreal's debug filter by class): the target moves to
       each instance of this prototype the game spawns; NULL when off. */
    char *debug_newest;
    app_debug_status debug_status;
    /* The target's entity was destroyed: the graph shows what it last did. */
    bool debug_destroyed;
    /* Break When Spawned (app_scene_authoring_set_spawn_break): prototype
       names, and the spawned entities seen at the last frame, sorted. */
    char **spawn_breaks;
    uint64_t *spawn_seen;
    bool runtime_snapshot_valid;
    scene_kept *kept; /* Keep Changes, oldest first; Stop applies them */
    scene_field_preview *field_previews; /* until the next draw (scene_asset_drop.c) */
    bool runtime_playing;
    bool runtime_step;
    bool preview_playing;
    bool preview_step;
    bool preview_loop;
    bool preview_follow;
    GameTransform preview_origin;
    app_runtime_target active_runtime_target;
    scene_camera camera;
    int camera_target;
    app_scene_view_options view_options;
    scene_world_preview world_preview;
    scene_camera_view camera_view;
    scene_history *history;
    ecs_entity edit_actor;
    ecs_entity property_actor;
    scene_field_edit property_edit;
    scene_field_target *property_others; /* array; recorded with the edit as one step */
    bool property_dirty;
    GameTransform edit_before;
    GameTransform edit_after;
    bool edit_dirty;
    bool dragging_actor;       /* a viewport gesture is in progress */
    GameTransform drag_origin; /* where a paused run's spawned object was grabbed */
    scene_gizmo gizmo;
    bool marquee;
    bool marquee_visible;
    int *marquee_base; /* the selection an extending marquee adds to; array of actors */
    int marquee_active;  /* the active object when it began */
    CF_V2 marquee_start; /* Normalized viewport coordinates. */
    CF_V2 marquee_end;
    float selection_scale;
    ecs_entity hovered; /* what a click would select, outlined while idle */
    unsigned asset_index_editor_revision;
    unsigned asset_index_system_revision;
    unsigned system_runtime_editor_revision;
    app_editor_document_context document;
    char *document_id;
    char *graph_document_id;
    app_editor_document_kind graph_document_kind;
    char *scene_path;
    bool scene_dirty;
    scene_document_tab *tabs;
    int active_tab;
    char *current_scene_tab_key;
    int next_scene_key;
    uint64_t next_actor_id;
    int collab_action_depth;
    bool tabs_transitioning;
    bool scene_tab_restoring;
    /* Editor view state (scene_view.c). */
    game_save *view_store;  /* by scene path; the host persists it */
    char *view_path;        /* the scene `view` was read for */
    uint64_t view_revision; /* of view_store when `view` was read or written */
    scene_view_entry *view; /* sorted by name */
    uint64_t *view_hidden; /* this frame's hidden entities, which draws leave out */
    uint64_t *view_unpickable; /* the hidden and locked ones, which picks leave out */
    scenario_document *scenario; /* replayed on the scene runtime, or NULL */
    bool scenario_recording;
    unsigned scenario_seed;         /* of the recording */
    av_value *scenario_saved_state; /* progress the recording started from, owned */
    char message[256];
    char notice[256]; /* the message, when it reports a success rather than a problem */
    char prototype_message[256];
};

/* Marks the message just written as a success, which Studio reports plainly,
   not as a problem. */
void scene_notice(app_scene_authoring *scene);

/* Document I/O is shared by disk commands and saved workspace checkpoints. */
/* The open scene as saving would write it, leaving an edit in progress open,
   so reading it changes nothing. Owned; NULL when an object does not save. */
game_scene_document *scene_authoring_document(app_scene_authoring *scene);
av_value *scene_authoring_scene_value(app_scene_authoring *scene);
char *scene_authoring_scene_text(app_scene_authoring *scene);
int scene_authoring_load_value(app_scene_authoring *scene, av_value *value, const char *path);
int scene_authoring_load_text(app_scene_authoring *scene, const char *json, const char *path);
int scene_authoring_sync_document(app_scene_authoring *scene);
void scene_authoring_select_document(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *id
);
void scene_tabs_initialize(app_scene_authoring *scene);
void scene_tabs_free(app_scene_authoring *scene);
void scene_tabs_sync_current(app_scene_authoring *scene);
void scene_tabs_mark_document_saved(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *id
);
void scene_tabs_document_selected(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *id
);
int scene_tabs_open_scene(app_scene_authoring *scene, const char *path);
bool scene_tabs_path_in_use(
    const app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *path
);
int scene_tabs_activate(app_scene_authoring *scene, int index);
int scene_tabs_open_source(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *name,
    const char *path
);
/* A behavior's or prototype's tab follows its rename, or closes. */
void scene_tabs_rename(
    app_scene_authoring *scene,
    app_editor_document_kind kind,
    const char *from,
    const char *to,
    const char *path
);
void scene_tabs_close(app_scene_authoring *scene, app_editor_document_kind kind, const char *id);
/* Asset removal closes its tab even when it was pinned. */
void scene_tabs_forget(app_scene_authoring *scene, app_editor_document_kind kind, const char *id);
int scene_tabs_discard(app_scene_authoring *scene, int index);
bool scene_tabs_can_close(const app_scene_authoring *scene, int index);
void scene_tabs_toggle_pin(app_scene_authoring *scene, int index);
bool scene_tabs_dirty(app_scene_authoring *scene, const scene_document_tab *tab);
const char *scene_tabs_current_scene_key(const app_scene_authoring *scene);
int load_system_document_text(app_scene_authoring *scene, const char *json, const char *path, bool reload);
/* Scene model operations shared by the authoring modules and Studio. */

void begin_transform_edit(app_scene_authoring *scene, ecs_entity entity);

const char *content_basename(const char *path);
bool content_path_in_folder(const app_scene_authoring *scene, const char *path, const char *folder);
void content_refresh(app_scene_authoring *scene);
void asset_index_invalidate(app_scene_authoring *scene);
bool editable_scalar(const pin_value *value);
bool scene_property_value_copy(pin_value *out, pin_value value);
void scene_property_value_free(pin_value *value);

void finish_property_edit(app_scene_authoring *scene);
void finish_transform_edit(app_scene_authoring *scene, bool cancel);
void write_transform_edit(app_scene_authoring *scene, ecs_entity entity, GameTransform position);
/* Fits the `count` objects in `only` in the view, or every object when
   `only` is NULL; false when none of them draws. */
bool fit_scene_view(engine_viewport *viewport, ecs_world *world, const ecs_entity *only, int count);
/* Generated World (scene_world_preview.c): prepares the scene's materials for
 * the edited view each draw, and discards them for a session or when off. */
void scene_world_preview_update(app_scene_authoring *scene, const engine_viewport_camera *view);
void scene_world_preview_discard(app_scene_authoring *scene);
/* Records the selection around the step the last frame added, and frees it. */
void scene_selection_track(app_scene_authoring *scene);
void scene_selection_free(app_scene_authoring *scene);
/* Overlays and View Through Camera (scene_overlays.c). */
void scene_overlays_draw(app_scene_authoring *scene);
void scene_camera_view_follow(app_scene_authoring *scene, engine_viewport *viewport, bool editing);
void free_behavior_names(char **names);
char **instance_behavior_names(app_scene_authoring *scene, ecs_entity entity);
scene_prototype_asset *previewed_prototype(app_scene_authoring *scene);

bool prototype_has_component_id(const game_prototype *prototype, ecs_component_id id);
/* The live world and the prototype preview spawn the prototype as it is now. */
int register_prototype_hosts(app_scene_authoring *scene, const game_prototype *prototype);

/* ---------- Prototype actions (scene_prototype_actions.c) ---------- */

/* How many other instances of its prototype take one of the fields from
   `source` (game_prototype_instance_overrides), and so change when they are
   applied. */
int scene_override_reach(
    const app_scene_authoring *scene,
    const scene_actor *source,
    const game_prototype_field *fields,
    int count
);
/* Copies the instance's fields into its prototype; the instances that
   inherit them follow. One scene Undo step, also an Undo step of the
   prototype's own document. */
int scene_apply_overrides(
    app_scene_authoring *scene,
    scene_actor *source,
    const game_prototype_field *fields,
    int count
);
/* Replaces prototype `name` with its definition `value`, as its scene Undo
   step replays it; every instance keeps its values. */
int scene_restore_prototype(app_scene_authoring *scene, const char *name, av_value *value);
/* Whether a new prototype can be named `name`: a valid asset name no asset
   or file has; false with why in `error`. */
bool scene_prototype_name_free(app_scene_authoring *scene, const char *name, char *error, size_t size);
/* Saves the instance as a new prototype asset `name` and makes the instance
   one of it, as one Undo step; -1 with why in `error` when the name is
   invalid or taken. */
int scene_save_as_prototype(
    app_scene_authoring *scene,
    scene_actor *actor,
    const char *name,
    char *error,
    size_t size
);
/* Makes the object at `index` an instance of `prototype`, keeping its name,
   place, values, behaviors and identity, as one Undo step that removes it
   and adds it back (scene_actors.c). */
int scene_repoint_actor(app_scene_authoring *scene, int index, const char *prototype);
void prototype_history_before(app_scene_authoring *scene, game_prototype *prototype);
/* The prototype a running scene entity was made from, or NULL. */
const char *scene_entity_prototype(const app_scene_authoring *scene, ecs_entity entity);
const char *runtime_entity_label(const app_scene_authoring *scene, ecs_entity entity);
bool runtime_session(const app_scene_authoring *scene);
/* Keep Changes (Unreal's Keep Simulation Changes): records the running values
   of the selected instance, or of its component `only`, that differ from its
   authored state. The running world holds the scene until Stop, which applies
   them as one Undo step; a later record decides the values it covers.
   Returns how many values it kept, or -1 without a running scene or a
   selected instance. */
int scene_keep_changes(app_scene_authoring *scene, const ecs_component_desc *only);
/* Forgets the latest Keep Changes before Stop applies it. */
void scene_kept_drop(app_scene_authoring *scene);
/* The values Stop will keep, on how many objects; `name` is the object's
   when there is one. */
int scene_kept_values(app_scene_authoring *scene, int *objects, const char **name);
void scene_kept_clear(app_scene_authoring *scene);
bool scene_can_move(const app_scene_authoring *scene);
/* The scene is edited: no session runs, paused or not. */
bool scene_editing(const app_scene_authoring *scene);
void select_instance(app_scene_authoring *scene, int actor_index);
/* Multi-selection, as Unity's, Godot's and Blender's: `selected` is the
   active object and `marked` the rest of the selection. */
bool scene_instance_selected(const app_scene_authoring *scene, int index);
int scene_selection_count(const app_scene_authoring *scene);
/* Adds the object to the selection as its active one, or takes it out,
   another selected object becoming active (Mod+click, Shift+click). */
void scene_toggle_instance(app_scene_authoring *scene, int index);
/* Selects actors rows[from] through rows[to] alone, rows[to] active, as
   Shift+click selects a range of a list. */
void scene_select_range(app_scene_authoring *scene, const int *rows, int from, int to);
/* Duplicates the selected objects `offset` away as one Undo step; the copies
   become the selection, the active object's copy active. */
int scene_duplicate_selection(app_scene_authoring *scene, GameTransform offset);
/* The selected objects as scene text: a scene document of their entities,
   each its prototype, overridden fields and behaviors, as the clipboard
   carries them. NULL without a selection. */
char *scene_copy_selection(app_scene_authoring *scene);
/* Where pasted objects land: the group's center on `*center`, else moved by
   `offset`; a positive `snap` rounds the move, so objects on the grid stay
   on it. */
typedef struct
{
    const GameTransform *center;
    GameTransform offset;
    float snap;
} scene_paste_place;
/* Adds the objects scene text `text` holds, keeping their layout and names
   unless taken, as one Undo step, and selects them. Returns how many, or -1
   with the message set when the text holds no objects or one this project
   cannot make. */
int scene_paste(app_scene_authoring *scene, const char *text, scene_paste_place place);
scene_actor *selected_actor(app_scene_authoring *scene);
scene_prototype_asset *selected_prototype(app_scene_authoring *scene);
ecs_entity selected_scene_entity(const app_scene_authoring *scene);
int write_prototype_field(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *component,
    const char *field,
    pin_value value
);
#define APP_SCENE_DOCUMENT_ID "Scene"

int actor_has_behavior(game_source_registry *sources, const scene_actor *actor, const char *asset_name);
int add_actor(app_scene_authoring *scene, const char *name, const char *prototype_name);
/* Scene document entities (scene_document.c), as loading and the clipboard
   share them: whether this project can make one, with the message set when
   not; one spawned as a new object named `name`, its actor last; and an
   object written as one. */
int scene_validate_entity(app_scene_authoring *scene, const game_scene_entity *entity);
int scene_spawn_entity(app_scene_authoring *scene, const game_scene_entity *entity, const char *name);
int scene_document_add_actor(
    app_scene_authoring *scene,
    game_scene_document *document,
    const scene_actor *actor
);
/* The scene document's name: its file's, without the extension. */
void scene_document_name(const app_scene_authoring *scene, char *name, size_t size);
int add_instance_at(app_scene_authoring *scene, const char *prototype_name, const GameTransform *position);
game_prototype_registry app_component_registry(void);
int apply_components_value(app_scene_authoring *scene, ecs_entity entity, av_value *components);
int attach_prototype_behavior(ecs_world *world, ecs_entity entity, const char *asset_name, void *user);
void clear_prototype_history(scene_prototype_asset *asset);
char *content_path_join(const char *base, const char *name);
scene_prototype_asset *find_prototype(app_scene_authoring *scene, const char *name);
void free_behavior_snapshots(game_prototype **snapshots);
void invalidate_runtime_snapshot(app_scene_authoring *scene);
int load_system_asset(app_scene_authoring *scene, const char *path, bool reload);
int open_behavior_asset(app_scene_authoring *scene, const char *asset_name);
/* Shows a behavior in the graph editor without changing the document. */
int open_behavior_graph(app_scene_authoring *scene, const char *asset_name);
int open_system_asset(app_scene_authoring *scene, const char *path);
void preview_behavior_edits(app_scene_authoring *scene);
scene_prototype_asset *prototype_at(app_scene_authoring *scene, int index);
int prototype_index(app_scene_authoring *scene, const char *name);
int prototype_redo(app_scene_authoring *scene);
int prototype_undo(app_scene_authoring *scene);
char *read_scene_file(const char *path);
int refresh_prototype_instances(app_scene_authoring *scene, scene_prototype_asset *asset);
/* Replace an asset with its file, as File > Reload does. */
int reload_behavior_from_disk(app_scene_authoring *scene, const char *asset_name);
int reload_prototype_asset(app_scene_authoring *scene, scene_prototype_asset *asset);
void remove_selected(app_scene_authoring *scene);
int replay_scene_edit(app_scene_authoring *scene, bool redo);
const ecs_component_desc *resolve_system_component(const char *name, void *);
int restore_retained_components(app_scene_authoring *scene, scene_actor *actor, const game_prototype *prototype);
int save_active_document(app_scene_authoring *scene);
/* Writes a content file and records it, so it does not reload as a change on disk. */
av_doc *scene_code_document_path(
    app_scene_authoring *scene,
    const char *text,
    const char *path,
    char *error,
    size_t size
);
behavior_asset *scene_graph_source_path(
    app_scene_authoring *scene,
    const char *text,
    const char *path,
    char *error,
    size_t size
);
authoring_source_presentation *scene_source_projection_path(
    app_scene_authoring *,
    const char *text,
    const char *path,
    char *error,
    size_t size
);
int save_content_file(app_scene_authoring *scene, const char *path, const char *json);
int scene_publish_source_edits(
    app_scene_authoring *,
    const source_catalog_edit *,
    int count,
    char *error,
    size_t size
);
int scene_publish_source_edits_restart(
    app_scene_authoring *,
    const source_catalog_edit *,
    int count,
    const char *restart_path,
    char *error,
    size_t size
);
int scene_publish_sources(app_scene_authoring *, const char *restart_path, char *error, size_t size);
typedef struct game_preview_source_staging game_preview_source_staging;
/* Two output stages, initialized to NULL. Failure discards any completed stage. */
int scene_stage_source_edits(
    app_scene_authoring *,
    const source_catalog_edit *,
    int count,
    game_preview_source_staging **stages,
    const char *restart_path,
    char *error,
    size_t size
);
void scene_finish_source_edits(app_scene_authoring *, game_preview_source_staging **stages, bool commit);
int save_behavior_document(app_scene_authoring *scene);
void select_prototype(app_scene_authoring *scene, int index);
void set_behavior_component_context(app_scene_authoring *scene, const char *asset_name);
int sync_behavior_working(app_scene_authoring *scene);
void refresh_asset_catalog(app_scene_authoring *scene);
void sync_editor_inspection(app_scene_authoring *scene);
const char *system_component_name(ecs_component_id id, void *);
void write_position(app_scene_authoring *scene, ecs_entity entity, GameTransform position);
/* Replaces the file with `text` at once; on the web the workspace server
 * keeps it too. */
int write_file_atomic(const char *path, const char *text);
/* Reconciles authored code with existing source before writing atomically. */
int write_text_file_atomic(const char *path, const char *text);
/* Removes a content file, from browser storage too. */
int remove_content_file(const char *path);
/* A file a refactor wrote (from == to), moved, created (no from) or removed
   (no to). */
typedef struct
{
    const char *from, *to;
} scene_move;
/* Brings what Studio holds of moved files up to date: new files load, a
   renamed prototype or behavior keeps its tab, changed files reload, and
   what a removed file held goes. */
/* Whether two paths name one file, as realpath resolves them. */
bool scene_same_file(const char *a, const char *b);
void scene_disk_note(app_scene_authoring *scene, const char *path);
void scene_disk_follow(app_scene_authoring *scene, const scene_move *moves, int count);
/* In Studio's own project, prepares `world` with the editor nodes and binds
   their service; otherwise does nothing. */
int scene_attach_studio_nodes(
    app_scene_authoring *scene,
    game_preview_world *world,
    char *error,
    size_t size
);

/* ---------- Editor view state (scene_view.c) ---------- */

void scene_view_init(app_scene_authoring *scene);
void scene_view_free(app_scene_authoring *scene);
/* An object's view state follows its rename; NULL `to` forgets it. */
void scene_view_rename(app_scene_authoring *scene, const char *from, const char *to);
/* The objects with any of `flags` while the scene is edited (no run), for a
   draw or a pick to leave out, in `buffer` until the next call with it. */
render_skip scene_view_skip(app_scene_authoring *scene, unsigned flags, uint64_t **buffer);
/* The open scene's view state follows it to `path`, where it is saved. */
void scene_view_save_as(app_scene_authoring *scene, const char *path);
/* The topmost object at (x, y) in the 2D edit view that `skip` does not hold. */
ecs_entity pick_scene_entity(app_scene_authoring *scene, float x, float y, render_skip skip);
/* The object a click at the pointer would pick, as of the Scene's last draw. */
ecs_entity scene_pick_at_pointer(app_scene_authoring *scene, engine_viewport *viewport);

/* ---------- Assets dropped on objects (scene_asset_drop.c) ---------- */

/* The field of `entity` whose text names an asset of `kind`
   (reference_named_field_kind), its component in `component`; -1 for none. */
int scene_asset_field(
    const app_scene_authoring *scene,
    ecs_entity entity,
    reference_asset_kind kind,
    ecs_component_id *component
);
/* Shows `text` in the field for the next draw only, outlined in
   SCENE_DROP_COLOR, as Unity previews a material dragged over an object. */
void scene_preview_field(
    app_scene_authoring *scene,
    ecs_entity entity,
    ecs_component_id component,
    int field,
    const char *text
);
/* The draw writes the previews before it renders; clearing puts the authored
   values back and forgets them. */
void scene_field_previews_apply(app_scene_authoring *scene);
void scene_field_previews_clear(app_scene_authoring *scene);

/* ---------- UI documents (Studio UI authoring, workstream D) ---------- */

/* Opens the ui asset or stylesheet at `path` in its tab, or activates the tab
 * already showing it; -1 with the message set when the file cannot open. */
int scene_tabs_open_ui(app_scene_authoring *scene, const char *path);
/* The tab of the UI document at `path`, however it is spelled, or NULL. */
scene_document_tab *scene_tabs_ui(app_scene_authoring *scene, const char *path);
/* Writes a UI document's text as its file, whatever the file holds now. */
int save_ui_document(app_scene_authoring *scene, scene_document_tab *tab);
/* Replaces a UI document's text with its file, discarding its edits. */
int reload_ui_document(app_scene_authoring *scene, scene_document_tab *tab);
#endif
