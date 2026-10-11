#ifndef CHIBI_EDITOR_SCENE_VIEW3D_H
#define CHIBI_EDITOR_SCENE_VIEW3D_H

#include "editor/workspace/viewport.h"
#include "platform/cute.h"
#include "render/scene3d.h"

#include <cute_math3d.h>

typedef struct app_scene_authoring app_scene_authoring;

/* What a 3D Scene view shows, for placing overlays on the canvas they draw
   on (scene_gizmo3d.c, scene_collider3d.c). */
typedef struct
{
    render_scene3d_camera camera;
    engine_viewport_camera frame;
    CF_V3 forward;
    float pixel; /* canvas units per pixel */
} scene_view3d;

scene_view3d scene_view3d_of(const app_scene_authoring *scene);
/* A world point's place on the canvas. */
CF_V2 scene_view3d_on_screen(const scene_view3d *view, CF_V3 point);
/* The pointer's place on the canvas. */
CF_V2 scene_view3d_pointer_at(const scene_view3d *view, const engine_viewport_pointer *pointer);
/* The ray from the eye through the pointer. */
CF_Ray3 scene_view3d_ray(const scene_view3d *view, const engine_viewport_pointer *pointer);
/* The world's units per pixel at `point`'s depth, or zero behind the eye. */
float scene_view3d_world_per_pixel(const scene_view3d *view, CF_V3 point);
/* How far canvas point `p` is from the segment from `a` to `b`. */
float scene_view3d_segment_distance(CF_V2 p, CF_V2 a, CF_V2 b);
/* Where along the line through `origin` in unit direction `axis` the ray
   passes nearest it, or false while the ray runs along it. */
bool scene_view3d_line_param(CF_V3 origin, CF_V3 axis, CF_Ray3 ray, float *t);

#endif
