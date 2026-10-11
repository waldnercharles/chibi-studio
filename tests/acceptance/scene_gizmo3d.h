#include "runtime/transform_propagation.h"

/* A 3D scene for the gizmo: a cube a Transform3D places, turned a
   quarter about Y so its X runs along the world's -Z, a block a Transform
   raises by its elevation, turned by an Orientation, and a gem placed by a
   mirror, which turns it a quarter about Y and flips its X. */
static const char
    cube_prototype[] = "name \"Cube\"\ncomponents {\n"
                       "    GameTransform3D {\n"
                       "        position x=0 y=50 z=0\n"
                       "        rotation x=0 y=0.70710678 z=0 w=0.70710678\n"
                       "        scale x=1 y=1 z=1\n"
                       "    }\n"
                       "    GameGeometry3D primitive=0 width=100.0 "
                       "depth=100.0 height=100.0\n"
                       "}\n";
static const char
    turner_prototype[] = "name \"Turner\"\ncomponents {\n"
                         "    GameTransform x=200 y=0\n"
                         "    GameOrientation radians=0\n"
                         "    GameGeometry3D primitive=0 width=100.0 "
                         "depth=100.0 height=100.0 elevation=50.0\n"
                         "}\n";
static const char
    mirror_prototype[] = "name \"Mirror\"\ncomponents {\n"
                         "    GameTransform3D {\n"
                         "        position x=0 y=50 z=300\n"
                         "        rotation x=0 y=0.70710678 z=0 w=0.70710678\n"
                         "        scale x=-1 y=1 z=1\n"
                         "    }\n"
                         "}\n";
static const char gem_prototype[] = "name \"Gem\"\ncomponents {\n"
                                    "    GameTransform3D {\n"
                                    "        rotation x=0 y=0 z=0 w=1\n"
                                    "        scale x=1 y=1 z=1\n"
                                    "    }\n"
                                    "    GameGeometry3D primitive=0 width=50.0 "
                                    "depth=50.0 height=50.0\n"
                                    "}\n";
static const char gizmo_scene
    [] = "version 1\nname \"Scene\"\npresentation \"3d\"\n"
         "(array)entities {\n"
         "    - name=\"Cube\" prototype=\"Cube\"\n"
         "    - name=\"Turner\" prototype=\"Turner\"\n"
         "    - name=\"Mirror\" prototype=\"Mirror\" { (array)children "
         "{ - name=\"Gem\" prototype=\"Gem\"; }; }\n"
         "}\n";
#define GIZMO_FRAMES 38
static const CF_V3 CUBE_AT = { 0, 50, 0 };
static const CF_V3 TURNER_AT = { 200, 50, 0 };
static const CF_V3 GEM_AT = { 0, 50, 300 };
static const float TOP_PITCH = -1.5533430f;
/* A view across the cube's own X, the world's -Z. */
static const float SIDE_YAW = 2.07f;

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

/* The gizmo's arrows' length at `point`: 80 pixels at its depth. */
static float gizmo_length(studio *s, CF_V3 point)
{
    editor_camera camera = editor_camera_get(s->viewport);
    float depth = cf_dot_v3(cf_sub_v3(point, editor_camera_eye(&camera)), editor_camera_forward(&camera));
    int width, height;
    engine_viewport_pixel_size(s->viewport, &width, &height);
    return 80 * 2 * depth * tanf(camera.fov * 0.5f) / (float)height;
}

/* `from` moved `distance` along `axis`, on screen. */
static CF_V2 shown_along(studio *s, CF_V3 from, CF_V3 axis, float distance)
{
    return shown_at(s, cf_add_v3(from, cf_mul_v3_f(axis, distance)));
}

static GameTransform3D cube_now(studio *s)
{
    return *GAME_GameTransform3D_get_const(s->scene->world, s->scene->actors[0].entity);
}

/* Undo takes a gesture back in one step. */
static bool undoes(studio *s)
{
    REQUIRE(!s->scene->gizmo.active);
    studio_command_run(s, STUDIO_COMMAND_UNDO);
    return true;
}

/* The quarter turn counterclockwise on screen that Rotate's Y ring takes
   seen from above, off the X and Z rings, which show edge-on there. */
static bool grab_y_ring(studio *s, CF_V3 pivot, CF_V2 *from, CF_V2 *to)
{
    float radius = gizmo_length(s, pivot);
    *from = shown_along(s, pivot, (CF_V3){ 0.70710678f, 0, -0.70710678f }, radius);
    *to = shown_along(s, pivot, (CF_V3){ -0.70710678f, 0, -0.70710678f }, radius);
    return button_at(s, SDL_BUTTON_LEFT, true, *from);
}

/* Whether `turn` takes the axes `before` to `after`. */
static bool turns_axes(CF_Quat turn, const GameGlobalTransform3D *before, const GameGlobalTransform3D *after)
{
    return near_v3(after->x_axis, cf_mul_q_v3(turn, before->x_axis), 0.01f) &&
           near_v3(after->y_axis, cf_mul_q_v3(turn, before->y_axis), 0.01f) &&
           near_v3(after->z_axis, cf_mul_q_v3(turn, before->z_axis), 0.01f);
}

/* A 3D view's gizmo: dragging Move's X arrow moves the cube along the
   world's X, with snapping onto the grid, and along its own turned X in
   Local Space; dragging Rotate's Y ring a quarter turn turns the block by
   its Orientation, and dragging Scale's X handle out doubles the cube's
   own X. Rotate's X ring seen edge-on turns as the pointer moves along
   it, and a mirrored parent's child turns as dragged in the world. Each
   drag is one Undo step, and Escape puts a drag back. */
static bool scene_gizmo3d_frame(void *state)
{
    static CF_V2 from, to;
    static GameGlobalTransform3D before;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= GIZMO_FRAMES);
    app_scene_authoring *scene = s->scene;
    CF_V3 x = { 1, 0, 0 }, local_x = { 0, 0, -1 };
    switch (frames) {
        case 1:
            REQUIRE(app_scene_authoring_register_prototype(scene, cube_prototype, NULL) == 0);
            REQUIRE(app_scene_authoring_register_prototype(scene, turner_prototype, NULL) == 0);
            REQUIRE(app_scene_authoring_register_prototype(scene, mirror_prototype, NULL) == 0);
            REQUIRE(app_scene_authoring_register_prototype(scene, gem_prototype, NULL) == 0);
            REQUIRE(scene_authoring_load_text(scene, gizmo_scene, NULL) == 0);
            s->prefs.tool = SCENE_TOOL_MOVE;
            s->prefs.scene_snap = false;
            s->prefs.scene_local = false;
            s->prefs.scene_increment = 8;
            select_instance(scene, 0);
            return true;
        case 3:
            REQUIRE(engine_viewport_perspective(s->viewport));
            look_at(s, CUBE_AT, 0.5f, -0.4f);
            return true;
        case 4:
        case 8:
        case 12: {
            CF_V3 axis = frames == 12 ? local_x : x;
            float grab = 0.6f * gizmo_length(s, CUBE_AT);
            from = shown_along(s, CUBE_AT, axis, grab);
            to = shown_along(s, CUBE_AT, axis, grab + (frames == 8 ? 99 : 100));
            return button_at(s, SDL_BUTTON_LEFT, true, from);
        }
        case 5:
        case 9:
        case 13: return move_to(s, to);
        case 6:
        case 10:
        case 14: return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 7:
            REQUIRE(near_v3(cube_now(s).position, (CF_V3){ 100, 50, 0 }, 0.05f));
            REQUIRE(undoes(s));
            REQUIRE(near_v3(cube_now(s).position, CUBE_AT, 0));
            s->prefs.scene_snap = true;
            return true;
        case 11:
            /* 99 lands on the grid's 96. */
            REQUIRE(near_v3(cube_now(s).position, (CF_V3){ 96, 50, 0 }, 0));
            REQUIRE(undoes(s));
            s->prefs.scene_snap = false;
            s->prefs.scene_local = true;
            /* Side on to the cube's X, which runs nearly along the view so far. */
            look_at(s, CUBE_AT, SIDE_YAW, -0.4f);
            return true;
        case 15:
            REQUIRE(near_v3(cube_now(s).position, (CF_V3){ 0, 50, -100 }, 0.05f));
            REQUIRE(undoes(s));
            REQUIRE(near_v3(cube_now(s).position, CUBE_AT, 0));
            s->prefs.tool = SCENE_TOOL_ROTATE;
            select_instance(scene, 1);
            /* From above, where the Y ring shows round. */
            look_at(s, TURNER_AT, 0, TOP_PITCH);
            return true;
        case 16: return grab_y_ring(s, TURNER_AT, &from, &to);
        case 17:
        case 21:
        case 25:
        case 29:
        case 33: return move_to(s, to);
        case 18:
        case 22:
        case 26:
        case 30: return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 19: {
            const GameOrientation *turned = GAME_GameOrientation_get_const(
                scene->world,
                scene->actors[1].entity
            );
            /* A quarter turn about +Y, which gameplay radians run against. */
            REQUIRE(near(turned->radians, -CF_PI / 2, 0.02f));
            REQUIRE(undoes(s));
            REQUIRE(
                GAME_GameOrientation_get_const(scene->world, scene->actors[1].entity)
                    ->radians == 0
            );
            s->prefs.tool = SCENE_TOOL_SCALE;
            select_instance(scene, 0);
            look_at(s, CUBE_AT, SIDE_YAW, -0.4f);
            return true;
        }
        case 20: {
            /* Scale follows the cube's own X whatever the space; its handle
               dragged out as far again doubles it. */
            CF_V2 pivot = shown_at(s, CUBE_AT);
            from = shown_along(s, CUBE_AT, local_x, gizmo_length(s, CUBE_AT));
            to = cf_add(from, cf_sub(from, pivot));
            return button_at(s, SDL_BUTTON_LEFT, true, from);
        }
        case 23:
            REQUIRE(near_v3(cube_now(s).scale, (CF_V3){ 2, 1, 1 }, 0.01f));
            REQUIRE(near_v3(cube_now(s).position, CUBE_AT, 0));
            REQUIRE(undoes(s));
            REQUIRE(near_v3(cube_now(s).scale, (CF_V3){ 1, 1, 1 }, 0));
            s->prefs.tool = SCENE_TOOL_ROTATE;
            s->prefs.scene_local = false;
            s->prefs.scene_snap = true;
            look_at(s, CUBE_AT, 0, TOP_PITCH);
            return true;
        case 24: {
            /* From above the X ring shows as a line along Z. Its point
               halfway up, dragged on along Z by a quarter turn's arc, turns
               the cube a quarter about X, though the pointer's angle about
               the center never changes. */
            CF_V3 up = { 0, 0.70710678f, 0.70710678f };
            CF_V3 grab = cf_add_v3(CUBE_AT, cf_mul_v3_f(up, gizmo_length(s, CUBE_AT)));
            before = game_transform_world_3d(scene->world, scene->actors[0].entity);
            from = shown_at(s, grab);
            to = shown_along(s, grab, (CF_V3){ 0, 0, 1 }, CF_PI / 2 * gizmo_length(s, grab));
            return button_at(s, SDL_BUTTON_LEFT, true, from);
        }
        case 27: {
            GameGlobalTransform3D
                after = game_transform_world_3d(scene->world, scene->actors[0].entity);
            REQUIRE(turns_axes(cf_quat_from_axis_angle(x, CF_PI / 2), &before, &after));
            REQUIRE(undoes(s));
            REQUIRE(near_v3(cube_now(s).position, CUBE_AT, 0));
            REQUIRE(child_of(scene, 3, 2));
            select_instance(scene, 3);
            look_at(s, GEM_AT, 0, TOP_PITCH);
            return true;
        }
        case 28:
            before = game_transform_world_3d(scene->world, scene->actors[3].entity);
            return grab_y_ring(s, GEM_AT, &from, &to);
        case 31: {
            /* The mirror's child turns a quarter about the world's +Y, as
               dragged, and stays where it stood. */
            GameGlobalTransform3D
                after = game_transform_world_3d(scene->world, scene->actors[3].entity);
            REQUIRE(
                turns_axes(cf_quat_from_axis_angle((CF_V3){ 0, 1, 0 }, CF_PI / 2), &before, &after)
            );
            REQUIRE(near_v3(after.position, GEM_AT, 0.05f));
            REQUIRE(undoes(s));
            s->prefs.tool = SCENE_TOOL_MOVE;
            s->prefs.scene_snap = false;
            select_instance(scene, 0);
            look_at(s, CUBE_AT, 0.5f, -0.4f);
            return true;
        }
        case 32: {
            float grab = 0.6f * gizmo_length(s, CUBE_AT);
            from = shown_along(s, CUBE_AT, x, grab);
            to = shown_along(s, CUBE_AT, x, grab + 100);
            return button_at(s, SDL_BUTTON_LEFT, true, from);
        }
        case 34:
            REQUIRE(scene->gizmo.active);
            REQUIRE(cube_now(s).position.x > 50);
            return press_key(SDLK_ESCAPE);
        case 35:
            /* Escape puts the cube back while the button is still down. */
            REQUIRE(!scene->gizmo.active);
            REQUIRE(near_v3(cube_now(s).position, CUBE_AT, 0));
            return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 36:
            REQUIRE(near_v3(cube_now(s).position, CUBE_AT, 0));
            return true;
        case GIZMO_FRAMES: edits++; return true;
        default: return true;
    }
}

TEST_CASE(gizmo_3d)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(GIZMO_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == GIZMO_FRAMES && edits == 1);
    printf("Studio 3D gizmo accepted: frames=%d\n", frames);
    return true;
}
