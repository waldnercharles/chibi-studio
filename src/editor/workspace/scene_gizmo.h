#ifndef CHIBI_EDITOR_SCENE_GIZMO_H
#define CHIBI_EDITOR_SCENE_GIZMO_H

#include "editor/workspace/viewport.h"
#include "runtime/engine.h"

#include <cute_math.h>
#include <stdbool.h>

/* The Scene's transform tools, as Unity's and Godot's Q, W, E and R. */
typedef enum
{
    SCENE_TOOL_SELECT,
    SCENE_TOOL_MOVE,
    SCENE_TOOL_ROTATE,
    SCENE_TOOL_SCALE,
    SCENE_TOOL_COUNT,
} scene_tool;

/* Orange selection in the viewport (DESIGN.md), Blender's: the active
   object brighter than the rest. */
#define SCENE_ACTIVE_COLOR 0xFFA53AFFu
#define SCENE_SELECTED_COLOR 0xE0701CFFu

/* Overlays' antialiasing, in screen pixels. */
#define SCENE_OVERLAY_AA 1.5f

/* Rotation snaps in steps of 15 degrees. */
#define SCENE_ROTATION_STEP 0.2617993878f

/* What the host's tools ask of the Scene for one frame. */
typedef struct
{
    scene_tool tool;
    bool grid;       /* the grid shows while the scene is edited */
    bool snap;       /* snapping, before Ctrl inverts it */
    float increment; /* the grid's spacing, in world units */
    bool shift;      /* held: a resize keeps the aspect ratio */
    bool extend;     /* Shift or Mod held: a click toggles an object in the
                        selection and a marquee adds to it */
    bool ctrl;       /* held: inverts snapping */
    bool alt;        /* held: a resize keeps the center */
    bool axis_x;     /* pressed: a modal move follows X only, or again freely */
    bool axis_y;
    bool confirm; /* Enter confirms a modal move */
    bool cancel;  /* Escape or a right press cancels a gesture */
} scene_tool_input;

/* The parts of a gizmo, as hovered or dragged. */
typedef enum
{
    SCENE_GIZMO_NONE,
    SCENE_GIZMO_FREE, /* the move square, a dragged object or a modal move */
    SCENE_GIZMO_AXIS_X,
    SCENE_GIZMO_AXIS_Y,
    SCENE_GIZMO_RING,
    SCENE_GIZMO_HANDLE, /* the first of eight resize handles, from the bottom left counterclockwise */
} scene_gizmo_part;

typedef struct scene_gizmo_target scene_gizmo_target;
typedef struct scene_gizmo_field scene_gizmo_field;

/* The gesture in progress: what it started from and the fields it writes.
   Every gesture is one Undo step. */
typedef struct
{
    scene_tool_input in;
    bool grab;    /* the host asked for a modal move (Blender's G) */
    int hover;    /* scene_gizmo_part under the pointer */
    int active;   /* scene_gizmo_part dragged, NONE while idle */
    bool modal;   /* the move follows the pointer until a click */
    bool moved;   /* the pointer moved the selection */
    int axis;     /* FREE, AXIS_X or AXIS_Y */
    int reselect; /* the actor a click without a drag selects alone, or -1 */
    int primary;  /* the target whose position or angle snaps */
    CF_V2 origin; /* the pointer when the gesture began, canvas units */
    CF_V2 pivot;  /* the selection's median point, canvas units */
    CF_V2 center; /* the resize box's center, half size and angle */
    CF_V2 half;
    float angle;
    CF_V2 grip; /* the dragged handle less the pointer, box units */
    float turn; /* the rotation so far, canvas radians */
    float last; /* the pointer's angle about the pivot last frame */
    bool dirty; /* the scene's dirty state when the gesture began */
    scene_gizmo_target *targets; /* array */
    scene_gizmo_field *fields;   /* array */
    char status[96]; /* the gesture's change so far, for the status bar */
    char report[96]; /* the last committed gesture's, until the host takes it */
} scene_gizmo;

typedef struct app_scene_authoring app_scene_authoring;

/* The pointer's frame in the Scene: hovering and dragging the gizmo, a
 * dragged object's move and a modal move. True when the gizmo took the
 * pointer, so it neither selects nor starts a marquee. */
bool scene_gizmo_pointer(
    app_scene_authoring *scene,
    engine_viewport *viewport,
    const engine_viewport_pointer *pointer
);
/* Starts moving the selection with actor `index`, picked at `pointer`: it
 * joins a selection it is part of, else is selected alone. */
void scene_gizmo_drag_object(app_scene_authoring *scene, int index, const engine_viewport_pointer *pointer);
/* Starts dragging gizmo `part` from canvas point `at`. */
void scene_gizmo_begin(app_scene_authoring *scene, scene_gizmo_part part, CF_V2 at);
/* Applies the gesture for the pointer at canvas point `at`. */
void scene_gizmo_update(app_scene_authoring *scene, CF_V2 at);
/* Ends the gesture: one Undo step for what changed, or none on `cancel`,
 * which restores the selection. */
void scene_gizmo_finish(app_scene_authoring *scene, bool cancel);
/* Whether the selection has what the tool edits: an Orientation to rotate,
   a size to resize. */
bool scene_gizmo_enabled(app_scene_authoring *scene);
/* The grid's spacing while it shows under the scene, which is while a 2D
   scene is edited, else zero. */
float scene_gizmo_grid(const app_scene_authoring *scene);
/* The tool's gizmo over the selection in a 2D view, and a modal move's
   constraint. */
void scene_gizmo_draw(app_scene_authoring *scene);
/* `value` at the nearest multiple of `increment`. */
float scene_snap(float value, float increment);

#endif
