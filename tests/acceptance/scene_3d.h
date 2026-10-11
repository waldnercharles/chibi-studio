#ifndef STUDIO_ACCEPTANCE_SCENE_3D_H
#define STUDIO_ACCEPTANCE_SCENE_3D_H

/* Shared view setup and projected pointer positions for 3D scene fixtures. */
/* Looks at `pivot` from 600 units, turned `yaw` and tilted `pitch`. */
static void look_at(studio *s, CF_V3 pivot, float yaw, float pitch)
{
    editor_camera camera = editor_camera_get(s->viewport);
    camera.pivot = pivot;
    camera.yaw = yaw;
    camera.pitch = pitch;
    camera.distance = 600;
    editor_camera_set(s->viewport, &camera);
}

/* `from` moved `distance` along `axis`, on screen. */
static CF_V2 shown_along(studio *s, CF_V3 from, CF_V3 axis, float distance)
{
    return shown_at(s, cf_add_v3(from, cf_mul_v3_f(axis, distance)));
}

#endif
