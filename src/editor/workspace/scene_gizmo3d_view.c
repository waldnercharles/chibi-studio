#include "editor/workspace/scene_gizmo3d_internal.h"
#include "platform/cute.h"
#include "support/array.h"

#include <math.h>

/* Sizes on screen, in pixels. */
static const float ARROW_HEAD = 9;
static const float CENTER = 14;
static const float CUBE = 9;
static const float LINE = 2;
/* How far past a part a press still takes it. */
static const float SLOP = 7;
/* The plane squares' span along each of their axes, as a fraction of an
   arrow. */
static const float PLANE_NEAR = 0.2f;
static const float PLANE_FAR = 0.45f;
static const int RING_SEGMENTS = 64;
/* How far behind the pivot, as the cosine from the eye, a ring still shows. */
static const float RING_BACK = 0.05f;
/* An arrow pointing this close to the eye shows too short to take, and a
   square seen this close to edge-on too thin. */
static const float FORESHORTENED = 0.2f;

static const unsigned CENTER_COLOR = 0xE8E8E8FF;
static const unsigned AXIS_COLORS[3] = { SCENE_GIZMO_X_COLOR,
                                         SCENE_GIZMO_Y_COLOR,
                                         SCENE_GIZMO_Z_COLOR };

static CF_Color packed(unsigned color)
{
    return cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color);
}

/* Whether `axis` points toward the eye from `point`. */
static bool facing(const scene_view3d *view, CF_V3 point, CF_V3 axis)
{
    return cf_dot_v3(axis, cf_sub_v3(view->camera.eye, point)) >= 0;
}

/* ---------- Parts ---------- */

static CF_V3 along(const gizmo3d *gizmo, int k, float fraction)
{
    return cf_add_v3(gizmo->pivot, cf_mul_v3_f(gizmo->axes[k], gizmo->length * fraction));
}

/* Whether arrow or handle `k` shows long enough on screen to take. */
static bool arrow_shown(const scene_view3d *view, const gizmo3d *gizmo, int k)
{
    CF_V2 a = scene_view3d_on_screen(view, gizmo->pivot),
          b = scene_view3d_on_screen(view, along(gizmo, k, 1));
    return cf_len(cf_sub(b, a)) >= FORESHORTENED * GIZMO3D_ARROW_LENGTH * view->pixel;
}

/* The corners of the square across the axes other than `k`, on their sides
   toward the eye. */
static void plane_corners(const scene_view3d *view, const gizmo3d *gizmo, int k, CF_V2 corners[4])
{
    int i = (k + 1) % 3, j = (k + 2) % 3;
    float si = facing(view, gizmo->pivot, gizmo->axes[i]) ? 1 : -1;
    float sj = facing(view, gizmo->pivot, gizmo->axes[j]) ? 1 : -1;
    const float spans[4][2] = { { PLANE_NEAR, PLANE_NEAR },
                                { PLANE_FAR, PLANE_NEAR },
                                { PLANE_FAR, PLANE_FAR },
                                { PLANE_NEAR, PLANE_FAR } };
    for (int c = 0; c < 4; c++) {
        CF_V3 offset = cf_add_v3(
            cf_mul_v3_f(gizmo->axes[i], si * spans[c][0] * gizmo->length),
            cf_mul_v3_f(gizmo->axes[j], sj * spans[c][1] * gizmo->length)
        );
        corners[c] = scene_view3d_on_screen(view, cf_add_v3(gizmo->pivot, offset));
    }
}

static bool plane_shown(const scene_view3d *view, const gizmo3d *gizmo, int k)
{
    CF_V3 to_eye = cf_norm_v3(cf_sub_v3(view->camera.eye, gizmo->pivot));
    return fabsf(cf_dot_v3(gizmo->axes[k], to_eye)) >= FORESHORTENED;
}

static bool inside(CF_V2 p, const CF_V2 corners[4])
{
    int sign = 0;
    for (int c = 0; c < 4; c++) {
        CF_V2 edge = cf_sub(corners[(c + 1) % 4], corners[c]);
        float side = cf_cross(edge, cf_sub(p, corners[c]));
        int s = side > 0 ? 1 : (side < 0 ? -1 : 0);
        if (s && sign && s != sign) {
            return false;
        }
        sign = s ? s : sign;
    }
    return true;
}

/* Ring `k`'s point at `angle`, in the world. */
static CF_V3 ring_point(const gizmo3d *gizmo, int k, float angle)
{
    CF_V3 u = gizmo->axes[(k + 1) % 3], v = gizmo->axes[(k + 2) % 3];
    CF_V3 offset = cf_add_v3(cf_mul_v3_f(u, cosf(angle)), cf_mul_v3_f(v, sinf(angle)));
    return cf_add_v3(gizmo->pivot, cf_mul_v3_f(offset, gizmo->length));
}

/* Whether ring `k` shows at `angle`: a ring shows its half toward the eye,
   as Unity's, and all of itself while it faces the eye. */
static bool ring_front(const scene_view3d *view, const gizmo3d *gizmo, int k, float angle)
{
    CF_V3 out = cf_sub_v3(ring_point(gizmo, k, angle), gizmo->pivot);
    CF_V3 to_eye = cf_sub_v3(view->camera.eye, gizmo->pivot);
    return cf_dot_v3(out, to_eye) >= -RING_BACK * cf_len_v3(out) * cf_len_v3(to_eye);
}

static float segment_angle(int segment)
{
    return 2 * CF_PI * (float)segment / (float)RING_SEGMENTS;
}

/* How far `at` is from ring `k` on screen, and the angle on the ring of
   the segment nearest it. */
static float ring_distance(const scene_view3d *view, const gizmo3d *gizmo, int k, CF_V2 at, float *angle)
{
    float best = INFINITY;
    for (int s = 0; s < RING_SEGMENTS; s++) {
        float a = segment_angle(s), b = segment_angle(s + 1);
        if (!ring_front(view, gizmo, k, (a + b) / 2)) {
            continue;
        }
        CF_V2 from = scene_view3d_on_screen(view, ring_point(gizmo, k, a));
        CF_V2 to = scene_view3d_on_screen(view, ring_point(gizmo, k, b));
        float distance = scene_view3d_segment_distance(at, from, to);
        if (distance < best) {
            best = distance;
            *angle = (a + b) / 2;
        }
    }
    return best;
}

/* Whether ring `k` shows too thin to follow the pointer's angle around it. */
static bool ring_edge_on(const scene_view3d *view, const gizmo3d *gizmo, int k)
{
    return !plane_shown(view, gizmo, k);
}

/* The direction a positive turn moves ring `k`'s point at `angle` on
   screen. A ring's points advance with their angle about its axis. */
static CF_V2 ring_tangent(const scene_view3d *view, const gizmo3d *gizmo, int k, float angle)
{
    const float step = CF_PI / RING_SEGMENTS;
    CF_V2 ahead = scene_view3d_on_screen(view, ring_point(gizmo, k, angle + step));
    CF_V2 behind = scene_view3d_on_screen(view, ring_point(gizmo, k, angle - step));
    return cf_safe_norm(cf_sub(ahead, behind));
}

static bool in_center(const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at, float size)
{
    CF_V2 d = cf_sub(at, scene_view3d_on_screen(view, gizmo->pivot));
    float half = (size / 2 + 1) * view->pixel;
    return fabsf(d.x) <= half && fabsf(d.y) <= half;
}

/* The arrow, or scale handle, of `first` + k nearest `at` within its slop. */
static int nearest_arrow(const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at, int first)
{
    int part = SCENE_GIZMO_NONE;
    float best = SLOP * view->pixel;
    CF_V2 pivot = scene_view3d_on_screen(view, gizmo->pivot);
    for (int k = 0; k < 3; k++) {
        float distance = scene_view3d_segment_distance(
            at,
            pivot,
            scene_view3d_on_screen(view, along(gizmo, k, 1))
        );
        if (arrow_shown(view, gizmo, k) && distance <= best) {
            best = distance;
            part = first + k;
        }
    }
    return part;
}

int gizmo3d_hit(scene_tool tool, const scene_view3d *view, const gizmo3d *gizmo, CF_V2 at)
{
    switch (tool) {
        case SCENE_TOOL_MOVE:
            if (in_center(view, gizmo, at, CENTER)) {
                return SCENE_GIZMO_SCREEN;
            }
            for (int k = 0; k < 3; k++) {
                CF_V2 corners[4];
                plane_corners(view, gizmo, k, corners);
                if (plane_shown(view, gizmo, k) && inside(at, corners)) {
                    return SCENE_GIZMO_PLANE_X + k;
                }
            }
            return nearest_arrow(view, gizmo, at, SCENE_GIZMO_ARROW_X);
        case SCENE_TOOL_ROTATE: {
            int part = SCENE_GIZMO_NONE;
            float best = SLOP * view->pixel;
            for (int k = 0; k < 3; k++) {
                float angle;
                float distance = ring_distance(view, gizmo, k, at, &angle);
                if (gizmo->turnable[k] && distance <= best) {
                    best = distance;
                    part = SCENE_GIZMO_RING_X + k;
                }
            }
            return part;
        }
        case SCENE_TOOL_SCALE:
            if (in_center(view, gizmo, at, CUBE)) {
                return SCENE_GIZMO_SCALE_ALL;
            }
            return nearest_arrow(view, gizmo, at, SCENE_GIZMO_SCALE_X);
        default: return SCENE_GIZMO_NONE;
    }
}

/* ---------- Drawing ---------- */

static void draw_arrows(const scene_gizmo *g, const scene_view3d *view, const gizmo3d *gizmo)
{
    CF_V2 pivot = scene_view3d_on_screen(view, gizmo->pivot);
    for (int k = 0; k < 3; k++) {
        if (!arrow_shown(view, gizmo, k)) {
            continue;
        }
        cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_ARROW_X + k, AXIS_COLORS[k]));
        cf_draw_arrow(
            pivot,
            scene_view3d_on_screen(view, along(gizmo, k, 1)),
            LINE * view->pixel,
            ARROW_HEAD * view->pixel
        );
        cf_draw_pop_color();
    }
    for (int k = 0; k < 3; k++) {
        if (!plane_shown(view, gizmo, k)) {
            continue;
        }
        CF_V2 c[4];
        plane_corners(view, gizmo, k, c);
        CF_Color color = scene_gizmo_color(g, SCENE_GIZMO_PLANE_X + k, AXIS_COLORS[k]);
        CF_Color fill = color;
        fill.a *= 0.35f;
        cf_draw_push_color(fill);
        cf_draw_quad_fill2(c[0], c[1], c[2], c[3], 0);
        cf_draw_pop_color();
        cf_draw_push_color(color);
        cf_draw_quad2(c[0], c[1], c[2], c[3], view->pixel, 0);
        cf_draw_pop_color();
    }
    float half = CENTER / 2 * view->pixel;
    CF_Aabb square = cf_make_aabb_center_half_extents(pivot, (CF_V2){ half, half });
    cf_draw_push_color(scene_gizmo_color(g, SCENE_GIZMO_SCREEN, CENTER_COLOR));
    cf_draw_quad(square, view->pixel, 0);
    cf_draw_pop_color();
}

static void draw_rings(const scene_gizmo *g, const scene_view3d *view, const gizmo3d *gizmo)
{
    for (int k = 0; k < 3; k++) {
        int part = SCENE_GIZMO_RING_X + k;
        cf_draw_push_color(
            gizmo->turnable[k] ? scene_gizmo_color(g, part, AXIS_COLORS[k])
                               : packed(SCENE_GIZMO_DISABLED_COLOR)
        );
        float width = (g->hover == part && !g->active ? 3 : LINE) * view->pixel;
        for (int s = 0; s < RING_SEGMENTS; s++) {
            float a = segment_angle(s), b = segment_angle(s + 1);
            if (ring_front(view, gizmo, k, (a + b) / 2)) {
                cf_draw_line(
                    scene_view3d_on_screen(view, ring_point(gizmo, k, a)),
                    scene_view3d_on_screen(view, ring_point(gizmo, k, b)),
                    width
                );
            }
        }
        cf_draw_pop_color();
    }
}

static void draw_cube(CF_V2 at, float size, CF_Color color)
{
    CF_Aabb cube = cf_make_aabb_center_half_extents(at, (CF_V2){ size / 2, size / 2 });
    cf_draw_push_color(color);
    cf_draw_quad_fill(cube, 0);
    cf_draw_pop_color();
}

static void draw_scale(const scene_gizmo *g, const scene_view3d *view, const gizmo3d *gizmo)
{
    CF_V2 pivot = scene_view3d_on_screen(view, gizmo->pivot);
    for (int k = 0; k < 3; k++) {
        if (!arrow_shown(view, gizmo, k)) {
            continue;
        }
        CF_Color color = scene_gizmo_color(g, SCENE_GIZMO_SCALE_X + k, AXIS_COLORS[k]);
        CF_V2 tip = scene_view3d_on_screen(view, along(gizmo, k, 1));
        cf_draw_push_color(color);
        cf_draw_line(pivot, tip, LINE * view->pixel);
        cf_draw_pop_color();
        draw_cube(tip, CUBE * view->pixel, color);
    }
    draw_cube(pivot, CUBE * view->pixel, scene_gizmo_color(g, SCENE_GIZMO_SCALE_ALL, CENTER_COLOR));
}

void scene_gizmo3d_draw(app_scene_authoring *scene)
{
    const scene_gizmo *g = &scene->gizmo;
    if (g->in.tool == SCENE_TOOL_SELECT || g->modal) {
        return;
    }
    scene_view3d view = scene_view3d_of(scene);
    scene_gizmo_target3d *targets = gizmo3d_targets(scene);
    gizmo3d gizmo;
    if (gizmo3d_shape(scene, &view, targets, &gizmo)) {
        switch (g->in.tool) {
            case SCENE_TOOL_MOVE: draw_arrows(g, &view, &gizmo); break;
            case SCENE_TOOL_ROTATE: draw_rings(g, &view, &gizmo); break;
            case SCENE_TOOL_SCALE: draw_scale(g, &view, &gizmo); break;
            default: break;
        }
    }
    afree(targets);
}

CF_V2 gizmo3d_ring_drag(const scene_view3d *view, const gizmo3d *gizmo, int axis, CF_V2 at)
{
    if (!ring_edge_on(view, gizmo, axis)) {
        return (CF_V2){ 0 };
    }
    float angle = 0;
    ring_distance(view, gizmo, axis, at, &angle);
    return ring_tangent(view, gizmo, axis, angle);
}
