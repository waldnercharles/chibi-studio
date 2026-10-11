#ifndef CHIBI_EDITOR_EDITOR_CAMERA_H
#define CHIBI_EDITOR_EDITOR_CAMERA_H

#include "render/viewport.h"

#include <cute_math3d.h>

/* The Scene's 3D editor camera, Unity's and Godot's: it orbits a pivot, pans
 * across the view, dollies toward the pointer and flies from its eye. The
 * viewport's view keeps it, so a saved view keeps its pivot: the lens holds
 * the eye and forward, and the pivot lies along forward at the distance the
 * view's height frames at the lens's field of view. A view without a lens
 * reads as the overhead framing it shows. */
typedef struct
{
    CF_V3 pivot;
    float yaw;      /* radians from -Z toward +X */
    float pitch;    /* radians above level, short of straight up or down */
    float distance; /* from the eye to the pivot */
    float fov;      /* vertical, radians */
    float aspect;   /* the image's width over its height */
} editor_camera;

editor_camera editor_camera_get(const engine_viewport *viewport);
/* Shows `camera` in the viewport. The view keeps its 2D fields and its
   shape, scaled to the camera's distance. */
void editor_camera_set(engine_viewport *viewport, const editor_camera *camera);
CF_V3 editor_camera_forward(const editor_camera *camera);
CF_V3 editor_camera_eye(const editor_camera *camera);
/* Turns the camera about its pivot. */
void editor_camera_orbit(editor_camera *camera, float yaw, float pitch);
/* Turns it about its eye, carrying the pivot along. */
void editor_camera_look(editor_camera *camera, float yaw, float pitch);
/* Moves it across the view as a pointer moving `ndc`, normalized device
   units, would drag a point at the pivot's depth. */
void editor_camera_pan(editor_camera *camera, CF_V2 ndc);
/* Moves its eye and pivot `step` world units: x right, y up, z forward. */
void editor_camera_fly(editor_camera *camera, CF_V3 step);
/* Scales its distance by `scale` about the point under `ndc` at the pivot's
   depth, which stays under the pointer. */
void editor_camera_dolly(editor_camera *camera, float scale, CF_V2 ndc);
/* Looks at a sphere along the camera's direction, the sphere and a fifth
   more filling the view. A point keeps the distance. */
void editor_camera_frame(editor_camera *camera, CF_V3 center, float radius);

#endif
