#ifndef CHIBI_EDITOR_SCENE_COLLIDER3D_H
#define CHIBI_EDITOR_SCENE_COLLIDER3D_H

#include "editor/workspace/scene_view3d.h"
#include "render/skip.h"

/* A 3D view's colliders (scene_collider3d.c): the selection's Collider3D
 * outlines, and with the Select tool handles on the active object's that
 * drag its extents, one Undo step per drag. The Colliders view option
 * outlines every object's. Objects the view hides show none of these. */

/* The handle at canvas point `at`, or SCENE_GIZMO_NONE. */
int scene_collider3d_part(app_scene_authoring *scene, const scene_view3d *view, CF_V2 at);
/* Starts dragging handle `part` with the pointer's `ray`; false when the
   active object has no collider to drag. */
bool scene_collider3d_begin(app_scene_authoring *scene, int part, const scene_view3d *view, CF_Ray3 ray);
/* Drags the handle to where `ray` passes nearest its axis. */
void scene_collider3d_drag(app_scene_authoring *scene, CF_Ray3 ray);
void scene_collider3d_draw(app_scene_authoring *scene, render_skip hidden);

#endif
