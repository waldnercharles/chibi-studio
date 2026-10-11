#include "editor/workspace/scene_view3d.h"

#include "editor/workspace/scene_internal.h"

/* A ray this close to parallel to a line meets it nowhere it could be
   dragged to. */
static const float PARALLEL = 1e-3f;

scene_view3d scene_view3d_of(const app_scene_authoring *scene)
{
    scene_view3d view = { .pixel = scene->selection_scale > 0 ? scene->selection_scale
                                                              : 1 };
    engine_viewport_get_camera3d(scene->render_viewport, &view.camera);
    engine_viewport_get_camera(scene->render_viewport, &view.frame);
    const float *v = view.camera.view.elements;
    view.forward = (CF_V3){ -v[2], -v[6], -v[10] };
    return view;
}

CF_V2 scene_view3d_on_screen(const scene_view3d *view, CF_V3 point)
{
    CF_V3 ndc = render_scene3d_project(&view->camera, point);
    return (CF_V2){ view->frame.center_x + ndc.x * view->frame.span_width * 0.5f,
                    view->frame.center_y + ndc.y * view->frame.span_height * 0.5f };
}

CF_V2 scene_view3d_pointer_at(const scene_view3d *view, const engine_viewport_pointer *pointer)
{
    return (CF_V2){
        view->frame.center_x + pointer->ndc_x * view->frame.span_width * 0.5f,
        view->frame.center_y + pointer->ndc_y * view->frame.span_height * 0.5f
    };
}

CF_Ray3 scene_view3d_ray(const scene_view3d *view, const engine_viewport_pointer *pointer)
{
    return render_scene3d_ray(&view->camera, (CF_V2){ pointer->ndc_x, pointer->ndc_y });
}

float scene_view3d_world_per_pixel(const scene_view3d *view, CF_V3 point)
{
    float depth = cf_dot_v3(cf_sub_v3(point, view->camera.eye), view->forward);
    if (depth <= 0) {
        return 0;
    }
    /* NDC's height of 2 spans the canvas's height and the frustum's. */
    float tangent = 1 / view->camera.projection.elements[5];
    return 2 * depth * tangent * view->pixel / view->frame.span_height;
}

float scene_view3d_segment_distance(CF_V2 p, CF_V2 a, CF_V2 b)
{
    CF_V2 ab = cf_sub(b, a);
    float span = cf_dot(ab, ab);
    float t = span > 0 ? cf_clamp01(cf_dot(cf_sub(p, a), ab) / span) : 0;
    return cf_len(cf_sub(p, cf_add(a, cf_mul_v2_f(ab, t))));
}

bool scene_view3d_line_param(CF_V3 origin, CF_V3 axis, CF_Ray3 ray, float *t)
{
    float b = cf_dot_v3(axis, ray.d);
    float denominator = 1 - b * b;
    if (denominator < PARALLEL) {
        return false;
    }
    CF_V3 w = cf_sub_v3(origin, ray.p);
    *t = (b * cf_dot_v3(ray.d, w) - cf_dot_v3(axis, w)) / denominator;
    return true;
}
