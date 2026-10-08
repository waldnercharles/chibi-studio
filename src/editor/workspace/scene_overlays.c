/* What the Scene view draws over an edited scene beside its selection:
 * GameCamera frames (Blender's camera frames, Unreal's camera frustums), the
 * selected GameFog grid's extent, and View Through Camera (Blender's
 * Numpad 0), which frames the view on a camera's span and dims the rest. */
#include "editor/workspace/viewport.h"
#include "editor/workspace/scene_internal.h"
#include "platform/cute.h"
#include "runtime/component_catalog.h"

#include <math.h>

/* Frame strokes and dashes, in pixels. */
#define FRAME_STROKE 1.0f
#define SELECTED_STROKE 2.0f
#define DASH 6.0f
#define DASH_GAP 4.0f

/* Packed RGBA. Frames read as the game's view, not as selection. */
#define CAMERA_COLOR 0xE8EDF5FFu
#define CAMERA_INACTIVE_COLOR 0xE8EDF580u
#define CAMERA_DISABLED_COLOR 0xE8EDF559u
#define FOG_EXTENT_COLOR 0x7FD8D0FFu
/* Outside a selected camera's frame, and outside the camera viewed through. */
#define SELECTED_DIM 0x00000040u
#define VIEWED_DIM 0x0000008Cu

static CF_Color packed(unsigned color)
{
    return cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color);
}

/* The camera's span around its object, as its game frames it. */
static bool camera_view(const ecs_world *world, ecs_entity entity, engine_viewport_view *out)
{
    const GameCamera *camera = GAME_GameCamera_get_const(world, entity);
    const GameTransform *position = GAME_GameTransform_get_const(world, entity);
    if (!camera || !position) {
        return false;
    }
    *out = (engine_viewport_view){ position->x + camera->offset.x,
                                   -position->y - camera->offset.y,
                                   camera->width,
                                   camera->height,
                                   camera->focal_distance };
    return isfinite(out->center_x) && isfinite(out->center_y) &&
           isfinite(out->width) && isfinite(out->height) && out->width > 0 &&
           out->height > 0 && isfinite(out->focal_distance) &&
           (out->focal_distance == 0 || out->focal_distance > 1);
}

static CF_Aabb view_box(const engine_viewport_view *view)
{
    return (CF_Aabb){ { view->center_x - view->width * 0.5f,
                        view->center_y - view->height * 0.5f },
                      { view->center_x + view->width * 0.5f,
                        view->center_y + view->height * 0.5f } };
}

/* The game's camera: the first enabled one in authored order, or -1. */
static int active_camera(const app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        const GameCamera
            *camera = GAME_GameCamera_get_const(scene->world, scene->actors[i].entity);
        if (camera && !camera->disabled) {
            return i;
        }
    }
    return -1;
}

/* The camera View Through Camera shows: the selected one, else the game's. */
static ecs_entity viewed_camera(app_scene_authoring *scene)
{
    scene_actor *selected = selected_actor(scene);
    if (selected && GAME_GameCamera_has(scene->world, selected->entity)) {
        return selected->entity;
    }
    int active = active_camera(scene);
    return active >= 0 ? scene->actors[active].entity : (ecs_entity){ 0 };
}

bool app_scene_authoring_camera_framing(app_scene_authoring *scene, engine_viewport_view *view)
{
    ecs_entity entity = viewed_camera(scene);
    return entity.slot && camera_view(scene->world, entity, view);
}

bool app_scene_authoring_viewing_through_camera(const app_scene_authoring *scene)
{
    return scene->camera_view.active;
}

int app_scene_authoring_view_through_camera(app_scene_authoring *scene, bool on)
{
    scene_camera_view *through = &scene->camera_view;
    engine_viewport *viewport = scene->render_viewport;
    if (!on) {
        if (through->active) {
            engine_viewport_set_view(viewport, &through->before);
        }
        through->active = false;
        return 0;
    }
    engine_viewport_view view;
    if (!viewport || !app_scene_authoring_camera_framing(scene, &view)) {
        return -1;
    }
    if (!through->active) {
        engine_viewport_get_view(viewport, &through->before);
    }
    engine_viewport_set_view(viewport, &view);
    through->active = true;
    through->entity = viewed_camera(scene);
    through->shown = view;
    return 0;
}

static bool same_view(const engine_viewport_view *a, const engine_viewport_view *b)
{
    return a->center_x == b->center_x && a->center_y == b->center_y &&
           a->width == b->width && a->height == b->height &&
           a->focal_distance == b->focal_distance;
}

void scene_camera_view_follow(app_scene_authoring *scene, engine_viewport *viewport, bool editing)
{
    scene_camera_view *through = &scene->camera_view;
    if (!through->active) {
        return;
    }
    engine_viewport_view current, view;
    engine_viewport_get_view(viewport, &current);
    /* Moving the view, playing or losing the camera leaves the view as it is. */
    if (!editing || !same_view(&current, &through->shown) ||
        !ecs_entity_is_alive(scene->world, through->entity) ||
        !camera_view(scene->world, through->entity, &view)) {
        through->active = false;
        return;
    }
    engine_viewport_set_view(viewport, &view);
    through->shown = view;
}

static void draw_dashed(CF_V2 a, CF_V2 b, float stroke, float pixel)
{
    float length = hypotf(b.x - a.x, b.y - a.y);
    float step = (DASH + DASH_GAP) * pixel;
    CF_V2 direction = { (b.x - a.x) / length, (b.y - a.y) / length };
    for (float at = 0; at < length; at += step) {
        float end = fminf(at + DASH * pixel, length);
        cf_draw_line(
            (CF_V2){ a.x + direction.x * at, a.y + direction.y * at },
            (CF_V2){ a.x + direction.x * end, a.y + direction.y * end },
            stroke
        );
    }
}

static void draw_frame(CF_Aabb box, unsigned color, float stroke, bool dashed, float pixel)
{
    cf_draw_push_color(packed(color));
    if (!dashed) {
        cf_draw_quad(box, stroke, 0);
        cf_draw_pop_color();
        return;
    }
    CF_V2 corners[] = { box.min,
                        { box.max.x, box.min.y },
                        box.max,
                        { box.min.x, box.max.y } };
    for (int i = 0; i < 4; i++) {
        draw_dashed(corners[i], corners[(i + 1) % 4], stroke, pixel);
    }
    cf_draw_pop_color();
}

/* Shades the view outside `box`, as Blender's passepartout. */
static void dim_outside(CF_Aabb box, const engine_viewport_camera *view, unsigned color)
{
    CF_Aabb all = {
        { view->center_x - view->span_width, view->center_y - view->span_height },
        { view->center_x + view->span_width, view->center_y + view->span_height }
    };
    CF_Aabb sides[] = {
        { all.min, { all.max.x, box.min.y } },
        { { all.min.x, box.max.y }, all.max },
        { { all.min.x, box.min.y }, { box.min.x, box.max.y } },
        { { box.max.x, box.min.y }, { all.max.x, box.max.y } },
    };
    cf_draw_push_color(packed(color));
    for (int i = 0; i < 4; i++) {
        if (sides[i].max.x > sides[i].min.x && sides[i].max.y > sides[i].min.y) {
            cf_draw_quad_fill(sides[i], 0);
        }
    }
    cf_draw_pop_color();
}

static void draw_cameras(app_scene_authoring *scene, const engine_viewport_camera *view, float pixel)
{
    scene_actor *selected = selected_actor(scene);
    int active = active_camera(scene);
    for (int i = 0; i < acount(scene->actors); i++) {
        ecs_entity entity = scene->actors[i].entity;
        engine_viewport_view frame;
        if (!camera_view(scene->world, entity, &frame)) {
            continue;
        }
        CF_Aabb box = view_box(&frame);
        if (selected == &scene->actors[i]) {
            dim_outside(box, view, SELECTED_DIM);
            draw_frame(box, CAMERA_COLOR, SELECTED_STROKE * pixel, false, pixel);
            continue;
        }
        bool disabled = GAME_GameCamera_get_const(scene->world, entity)->disabled;
        unsigned color = disabled    ? CAMERA_DISABLED_COLOR
                       : i == active ? CAMERA_COLOR
                                     : CAMERA_INACTIVE_COLOR;
        draw_frame(box, color, FRAME_STROKE * pixel, disabled, pixel);
    }
}

/* The selected object's fog grid, where the game keeps its fog of war. */
static void draw_fog_extent(app_scene_authoring *scene, float pixel)
{
    scene_actor *selected = selected_actor(scene);
    const GameFog *fog = selected
                           ? GAME_GameFog_get_const(scene->world, selected->entity)
                           : NULL;
    if (!fog || fog->columns <= 0 || fog->rows <= 0 || !(fog->cell_size > 0)) {
        return;
    }
    CF_Aabb box = { { fog->origin.x, -fog->origin.y - fog->rows * fog->cell_size },
                    { fog->origin.x + fog->columns * fog->cell_size,
                      -fog->origin.y } };
    draw_frame(box, FOG_EXTENT_COLOR, SELECTED_STROKE * pixel, true, pixel);
}

void scene_overlays_draw(app_scene_authoring *scene)
{
    engine_viewport *viewport = scene->render_viewport;
    if (!viewport || engine_viewport_perspective(viewport)) {
        return;
    }
    engine_viewport_camera view;
    engine_viewport_get_camera(viewport, &view);
    float pixel = scene->selection_scale > 0 ? scene->selection_scale : 1;
    engine_viewport_view viewed;
    if (scene->camera_view.active &&
        camera_view(scene->world, scene->camera_view.entity, &viewed)) {
        dim_outside(view_box(&viewed), &view, VIEWED_DIM);
    }
    if (scene->view_options.cameras) {
        draw_cameras(scene, &view, pixel);
    }
    draw_fog_extent(scene, pixel);
}

int app_scene_authoring_camera_labels(app_scene_authoring *scene, app_scene_camera_label *labels, int max)
{
    engine_viewport *viewport = scene->render_viewport;
    if (!scene->view_options.cameras || !viewport || runtime_session(scene) ||
        engine_viewport_perspective(viewport)) {
        return 0;
    }
    engine_viewport_camera view;
    engine_viewport_get_camera(viewport, &view);
    scene_actor *selected = selected_actor(scene);
    int active = active_camera(scene), count = 0;
    for (int i = 0; i < acount(scene->actors) && count < max; i++) {
        engine_viewport_view frame;
        if (!camera_view(scene->world, scene->actors[i].entity, &frame)) {
            continue;
        }
        CF_Aabb box = view_box(&frame);
        float left = view.center_x - view.span_width * 0.5f;
        float top = view.center_y + view.span_height * 0.5f;
        float x = (box.min.x - left) / view.span_width;
        float y = (top - box.max.y) / view.span_height;
        float right = (box.max.x - left) / view.span_width;
        float bottom = (top - box.min.y) / view.span_height;
        if (right < 0 || x > 1 || bottom < 0 || y > 1) {
            continue;
        }
        labels[count++] = (app_scene_camera_label){
            .name = scene->actors[i].name,
            .x = fmaxf(x, 0),
            .y = fmaxf(y, 0),
            .active = i == active,
            .disabled = GAME_GameCamera_get_const(scene->world, scene->actors[i].entity)
                            ->disabled,
            .selected = selected == &scene->actors[i],
        };
    }
    return count;
}
