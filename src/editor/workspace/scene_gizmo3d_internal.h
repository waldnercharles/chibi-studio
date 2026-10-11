#ifndef CHIBI_EDITOR_SCENE_GIZMO3D_INTERNAL_H
#define CHIBI_EDITOR_SCENE_GIZMO3D_INTERNAL_H

#include "editor/workspace/scene_internal.h"
#include "editor/workspace/scene_view3d.h"
#include "runtime/component_catalog.h"

#define GIZMO3D_ARROW_LENGTH 80
#define GIZMO3D_FIELD_COUNT 13 /* reflected fields in the target adapter */

/* A selected object as the gesture began, in the world. */
struct scene_gizmo_target3d
{
    ecs_entity entity;
    bool spatial; /* a Transform3D places it */
    bool valid;   /* the parent has a stable world-to-local inverse */
    bool turns;   /* a world rotation is representable in its authored fields */
    CF_V3 position;
    CF_Quat rotation;
    CF_M4x4 from_world;             /* the world into its parent's space */
    CF_Quat parent_turn;            /* its parent's rotation */
    bool mirrored;                  /* its parent mirrors its X */
    GameGlobalTransform2D parent;   /* without a Transform3D, where its parent
                                       places it; zero for a root */
    int field[GIZMO3D_FIELD_COUNT]; /* index in the gesture's fields, or -1 */
};

/* Where the selection's gizmo stands. */
typedef struct
{
    CF_V3 pivot; /* the median of the objects' positions */
    CF_V3 axes[3];
    float length;     /* an arrow's, in world units */
    bool local;       /* the axes are the active object's */
    bool turnable[3]; /* every target can turn about the axis, and one does */
} gizmo3d;

scene_gizmo_target3d *gizmo3d_targets(const app_scene_authoring *scene);
int gizmo3d_primary(const app_scene_authoring *scene, const scene_gizmo_target3d *targets);
bool gizmo3d_shape(
    const app_scene_authoring *scene,
    const scene_view3d *view,
    const scene_gizmo_target3d *targets,
    gizmo3d *out
);
void gizmo3d_record(app_scene_authoring *scene, scene_gizmo_target3d *target, scene_tool tool);
void gizmo3d_place(app_scene_authoring *scene, const scene_gizmo_target3d *target, CF_V3 at);
void gizmo3d_turn(app_scene_authoring *scene, const scene_gizmo_target3d *target, CF_V3 axis, float radians);
void gizmo3d_scale(app_scene_authoring *scene, const scene_gizmo_target3d *target, CF_V3 factor);

int gizmo3d_hit(scene_tool tool, const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at);
CF_V2 gizmo3d_ring_drag(const scene_view3d *view, const gizmo3d *gizmo, int axis, CF_V2 at);

#endif
