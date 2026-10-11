#ifndef CHIBI_EDITOR_SCENE_GIZMO_H
#define CHIBI_EDITOR_SCENE_GIZMO_H

#include "editor/workspace/viewport.h"
#include "platform/cute.h"
#include "runtime/engine.h"

#include <cute_math.h>
#include <cute_math3d.h>
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

/* The gizmos' axes, Unity's and Blender's: X red, Y green and Z blue; a
   dragged part, and a disabled one. */
#define SCENE_GIZMO_X_COLOR 0xE0727AFFu
#define SCENE_GIZMO_Y_COLOR 0x82C46BFFu
#define SCENE_GIZMO_Z_COLOR 0x6E9BF5FFu
#define SCENE_GIZMO_DRAGGED_COLOR 0xFFD75EFFu
#define SCENE_GIZMO_DISABLED_COLOR 0x9AA3B266u

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
    bool local;   /* a 3D view's Move and Rotate follow the active object's
                     axes; Scale always does */
    bool confirm; /* Enter confirms a modal move */
    bool cancel;  /* Escape or a right press cancels a gesture */
    float marker_radius; /* how far from an undrawn object its marker icon
                            reaches, in the view's pixels */
} scene_tool_input;

/* The parts of a gizmo, as hovered or dragged. */
typedef enum
{
    SCENE_GIZMO_NONE,
    SCENE_GIZMO_FREE, /* the move square, a dragged object or a modal move */
    SCENE_GIZMO_AXIS_X,
    SCENE_GIZMO_AXIS_Y,
    SCENE_GIZMO_RING,
    /* A 3D view's (scene_gizmo3d.c): the move arrows along X, Y and Z, the
       squares that move across the other two axes, the center that moves
       across the view, the rings about each axis, and the scale handles
       along each axis and the center's uniform scale. */
    SCENE_GIZMO_ARROW_X,
    SCENE_GIZMO_ARROW_Y,
    SCENE_GIZMO_ARROW_Z,
    SCENE_GIZMO_PLANE_X,
    SCENE_GIZMO_PLANE_Y,
    SCENE_GIZMO_PLANE_Z,
    SCENE_GIZMO_SCREEN,
    SCENE_GIZMO_RING_X,
    SCENE_GIZMO_RING_Y,
    SCENE_GIZMO_RING_Z,
    SCENE_GIZMO_SCALE_X,
    SCENE_GIZMO_SCALE_Y,
    SCENE_GIZMO_SCALE_Z,
    SCENE_GIZMO_SCALE_ALL,
    SCENE_GIZMO_HANDLE, /* the first of eight resize handles, from the bottom left counterclockwise */
} scene_gizmo_part;

/* Whether `part` is a 3D view's. */
bool scene_gizmo_spatial(int part);

typedef struct scene_gizmo_target scene_gizmo_target;
typedef struct scene_gizmo_target3d scene_gizmo_target3d;
typedef struct scene_gizmo_field scene_gizmo_field;

/* A 3D view's gesture as it began, in world units. */
typedef struct
{
    CF_V3 pivot;
    CF_V3 axes[3]; /* the gizmo's X, Y and Z: the world's or the active object's */
    CF_V3 grab;    /* where the pointer's ray met the dragged part */
    float along;   /* where along a dragged arrow it did */
    float length;  /* the gizmo's arrows, as long as they show */
    bool local;    /* the axes are the active object's */
    CF_V2 tangent; /* a ring seen near edge-on turns as the pointer moves
                      along this, its direction on screen at the grab; zero
                      for one that turns with the pointer's angle */
    scene_gizmo_target3d *targets; /* array */
} scene_gizmo_space;

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
    float tilt; /* the gizmo box's angle when last hovered */
    bool dirty; /* the scene's dirty state when the gesture began */
    scene_gizmo_target *targets; /* array */
    scene_gizmo_space space;     /* a 3D view's gesture */
    scene_gizmo_field *fields;   /* array */
    char status[96]; /* the gesture's change so far, for the status bar */
    char delta[64];  /* that change alone, for a label by the pointer */
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
/* Why the tool's gizmo shows grey in a 2D view: the component the selection
   lacks, Rotate's Orientation or Scale's appearance, which gives it a size,
   and where the gizmo shows, as fractions of the view from its top-left. */
typedef struct
{
    const ecs_component_desc *missing;
    float x, y;
} scene_tool_hint;
/* False while the gizmo works, or shows nothing. */
bool scene_gizmo_hint(app_scene_authoring *scene, scene_tool_hint *hint);
/* The grid's spacing while it shows under the scene, which is while a 2D
   scene is edited, else zero. */
float scene_gizmo_grid(const app_scene_authoring *scene);
/* The tool's gizmo over the selection in a 2D view, and a modal move's
   constraint. */
void scene_gizmo_draw(app_scene_authoring *scene);
/* The cursor, as RCSS names it, for gizmo `part` on a box turned by
   `angle`: Move's four arrows, an axis's or a handle's resize direction and
   the ring's hand; NULL for none. */
const char *scene_gizmo_part_cursor(int part, float angle);
/* Part `part` in `color`: yellow while dragged, lighter while hovered. */
CF_Color scene_gizmo_color(const scene_gizmo *g, int part, unsigned color);
/* The cursor for the part dragged, else hovered, or NULL. */
const char *scene_gizmo_cursor(const app_scene_authoring *scene);
/* A 3D view's gizmo (scene_gizmo3d.c): hovering and dragging its parts,
   true when it took the pointer, as scene_gizmo_pointer. */
bool scene_gizmo3d_pointer(app_scene_authoring *scene, const engine_viewport_pointer *pointer);
/* Whether the selection has what the tool edits in a 3D view. */
bool scene_gizmo3d_enabled(app_scene_authoring *scene);
/* The tool's gizmo over the selection in a 3D view. */
void scene_gizmo3d_draw(app_scene_authoring *scene);

/* For scene_gizmo3d.c: records field `field` of `component` on `entity` as
   the gesture writes it, returning its index in the gesture, or -1 when the
   entity lacks the component. */
int scene_gizmo_record(
    app_scene_authoring *scene,
    ecs_entity entity,
    const ecs_component_desc *component,
    const char *field
);
/* Writes recorded field `index`, live and for the gesture's Undo step. */
void scene_gizmo_write(app_scene_authoring *scene, int index, pin_value value);
/* The recorded field's value as the gesture began. */
pin_value scene_gizmo_started(const app_scene_authoring *scene, int index);
/* The status bar's line for the change so far: `verb` and the delta. */
void scene_gizmo_describe(scene_gizmo *g, const char *verb);
/* Snapping, as the settings and Ctrl have it. */
bool scene_gizmo_snapping(const scene_gizmo *g);

/* `value` at the nearest multiple of `increment`. */
float scene_snap(float value, float increment);

#endif
