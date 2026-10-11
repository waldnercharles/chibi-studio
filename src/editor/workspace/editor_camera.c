#include "editor/workspace/editor_camera.h"

#include "render/scene3d.h"

#include <math.h>

/* 89 degrees: short of straight up or down, where the view's up vanishes. */
static const float PITCH_LIMIT = 1.5533430f;
static const float DISTANCE_MIN = 0.01f;
static const float DISTANCE_MAX = 1e6f;
/* What a frame leaves around the sphere it shows, as a fit's 20 percent. */
static const float FRAME_MARGIN = 1.2f;
static const CF_V3 WORLD_UP = { 0, 1, 0 };

static float clamp(float value, float least, float most)
{
    return fminf(fmaxf(value, least), most);
}

CF_V3 editor_camera_forward(const editor_camera *camera)
{
    float level = cosf(camera->pitch);
    return (CF_V3){ sinf(camera->yaw) * level,
                    sinf(camera->pitch),
                    -cosf(camera->yaw) * level };
}

CF_V3 editor_camera_eye(const editor_camera *camera)
{
    return cf_sub_v3(
        camera->pivot,
        cf_mul_v3_f(editor_camera_forward(camera), camera->distance)
    );
}

static CF_V3 right_of(const editor_camera *camera)
{
    return (CF_V3){ cosf(camera->yaw), 0, sinf(camera->yaw) };
}

static CF_V3 up_of(const editor_camera *camera)
{
    return cf_cross_v3(right_of(camera), editor_camera_forward(camera));
}

/* Where `ndc` lies at the pivot's depth. */
static CF_V3 at_pivot_depth(const editor_camera *camera, CF_V2 ndc)
{
    float half_height = camera->distance * tanf(camera->fov * 0.5f);
    CF_V3 across = cf_mul_v3_f(right_of(camera), ndc.x * half_height * camera->aspect);
    CF_V3 up = cf_mul_v3_f(up_of(camera), ndc.y * half_height);
    return cf_add_v3(camera->pivot, cf_add_v3(across, up));
}

editor_camera editor_camera_get(const engine_viewport *viewport)
{
    engine_viewport_camera frame;
    engine_viewport_get_camera(viewport, &frame);
    render_scene3d_camera shown;
    engine_viewport_get_camera3d(viewport, &shown);
    /* The view matrix's third row is the eye's backward direction. */
    CF_V3 forward = { -shown.view.elements[2],
                      -shown.view.elements[6],
                      -shown.view.elements[10] };
    float tangent = 1 / shown.projection.elements[5];
    float distance = clamp(frame.span_height * 0.5f / tangent, DISTANCE_MIN, DISTANCE_MAX);
    return (editor_camera){
        .pivot = cf_add_v3(shown.eye, cf_mul_v3_f(forward, distance)),
        .yaw = atan2f(forward.x, -forward.z),
        .pitch = clamp(asinf(clamp(forward.y, -1, 1)), -PITCH_LIMIT, PITCH_LIMIT),
        .distance = distance,
        .fov = 2 * atanf(tangent),
        .aspect = frame.span_width / frame.span_height,
    };
}

void editor_camera_set(engine_viewport *viewport, const editor_camera *camera)
{
    /* The view keeps its shape, so the zoom read against its framing
       changes only as the distance does. */
    float scale = camera->distance / editor_camera_get(viewport).distance;
    engine_viewport_view view;
    engine_viewport_get_view(viewport, &view);
    view.center_x = camera->pivot.x;
    view.center_y = -camera->pivot.z;
    view.width *= scale;
    view.height *= scale;
    view.lens = (engine_viewport_lens){ .eye = editor_camera_eye(camera),
                                        .forward = editor_camera_forward(camera),
                                        .up = WORLD_UP,
                                        .fov = camera->fov };
    engine_viewport_set_view(viewport, &view);
}

void editor_camera_orbit(editor_camera *camera, float yaw, float pitch)
{
    camera->yaw = remainderf(camera->yaw + yaw, 2 * CF_PI);
    camera->pitch = clamp(camera->pitch + pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

void editor_camera_look(editor_camera *camera, float yaw, float pitch)
{
    CF_V3 eye = editor_camera_eye(camera);
    editor_camera_orbit(camera, yaw, pitch);
    camera->pivot = cf_add_v3(
        eye,
        cf_mul_v3_f(editor_camera_forward(camera), camera->distance)
    );
}

void editor_camera_pan(editor_camera *camera, CF_V2 ndc)
{
    camera->pivot = at_pivot_depth(camera, cf_neg_v2(ndc));
}

void editor_camera_fly(editor_camera *camera, CF_V3 step)
{
    CF_V3 across = cf_mul_v3_f(right_of(camera), step.x);
    CF_V3 up = cf_mul_v3_f(WORLD_UP, step.y);
    CF_V3 ahead = cf_mul_v3_f(editor_camera_forward(camera), step.z);
    camera->pivot = cf_add_v3(camera->pivot, cf_add_v3(across, cf_add_v3(up, ahead)));
}

void editor_camera_dolly(editor_camera *camera, float scale, CF_V2 ndc)
{
    CF_V3 anchor = at_pivot_depth(camera, ndc);
    float distance = clamp(camera->distance * scale, DISTANCE_MIN, DISTANCE_MAX);
    CF_V3 offset = cf_sub_v3(camera->pivot, anchor);
    camera->pivot = cf_add_v3(anchor, cf_mul_v3_f(offset, distance / camera->distance));
    camera->distance = distance;
}

void editor_camera_frame(editor_camera *camera, CF_V3 center, float radius)
{
    camera->pivot = center;
    if (radius <= 0) {
        return;
    }
    float half = fminf(
        camera->fov * 0.5f,
        atanf(tanf(camera->fov * 0.5f) * camera->aspect)
    );
    camera->distance = clamp(radius * FRAME_MARGIN / sinf(half), DISTANCE_MIN, DISTANCE_MAX);
}
