#include "scene_gizmo3d.h"

/* A 3D scene for the collider handles: a crate whose box a Transform3D
   stretches twice along X, a ball and a pill, side by side. */
static const char crate_prototype[] = "name \"Crate\"\ncomponents {\n"
                                      "    GameTransform3D {\n"
                                      "        position x=0 y=50 z=0\n"
                                      "        rotation x=0 y=0 z=0 w=1\n"
                                      "        scale x=2 y=1 z=1\n"
                                      "    }\n"
                                      "    GameCollider3D {\n"
                                      "        size x=50 y=50 z=50\n"
                                      "    }\n"
                                      "}\n";
static const char
    ball_prototype[] = "name \"Ball\"\ncomponents {\n"
                       "    GameTransform3D {\n"
                       "        position x=200 y=50 z=0\n"
                       "        rotation x=0 y=0 z=0 w=1\n"
                       "        scale x=1 y=1 z=1\n"
                       "    }\n"
                       "    GameCollider3D shape=\"Sphere\" radius=25\n"
                       "}\n";
static const char pill_prototype[] = "name \"Pill\"\ncomponents {\n"
                                     "    GameTransform3D {\n"
                                     "        position x=-200 y=50 z=0\n"
                                     "        rotation x=0 y=0 z=0 w=1\n"
                                     "        scale x=1 y=1 z=1\n"
                                     "    }\n"
                                     "    GameCollider3D shape=\"Capsule\" "
                                     "radius=20 height=100\n"
                                     "}\n";
static const char
    collider_scene[] = "version 1\nname \"Scene\"\npresentation \"3d\"\n"
                       "(array)entities {\n"
                       "    - name=\"Crate\" prototype=\"Crate\"\n"
                       "    - name=\"Ball\" prototype=\"Ball\"\n"
                       "    - name=\"Pill\" prototype=\"Pill\"\n"
                       "}\n";
#define COLLIDER_FRAMES 18
static const CF_V3 CRATE_AT = { 0, 50, 0 };
static const CF_V3 BALL_AT = { 200, 50, 0 };
static const CF_V3 PILL_AT = { -200, 50, 0 };

static const GameCollider3D *collider_now(studio *s, int actor)
{
    return GAME_GameCollider3D_get_const(
        s->scene->world,
        s->scene->actors[actor].entity
    );
}

/* Presses the collider handle at `handle` and aims to drag it `distance`
   along `axis`. */
static bool grab_handle(studio *s, CF_V3 handle, CF_V3 axis, float distance, CF_V2 *from, CF_V2 *to)
{
    *from = shown_at(s, handle);
    *to = shown_along(s, handle, axis, distance);
    return button_at(s, SDL_BUTTON_LEFT, true, *from);
}

/* With the Select tool, the active object's collider handles drag its
   extent: the crate's -X side out a quarter of its stretched width widens
   its box, the ball's top raises its radius, and the pill's top dragged
   past its middle leaves a capsule no shorter than its caps. Each drag is
   one Undo step. */
static bool scene_collider3d_frame(void *state)
{
    static CF_V2 from, to;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= COLLIDER_FRAMES);
    app_scene_authoring *scene = s->scene;
    CF_V3 x = { 1, 0, 0 }, y = { 0, 1, 0 };
    switch (frames) {
        case 1:
            REQUIRE(app_scene_authoring_register_prototype(scene, crate_prototype, NULL) == 0);
            REQUIRE(app_scene_authoring_register_prototype(scene, ball_prototype, NULL) == 0);
            REQUIRE(app_scene_authoring_register_prototype(scene, pill_prototype, NULL) == 0);
            REQUIRE(scene_authoring_load_text(scene, collider_scene, NULL) == 0);
            s->prefs.tool = SCENE_TOOL_SELECT;
            s->prefs.scene_snap = false;
            s->prefs.scene_increment = 8;
            select_instance(scene, 0);
            return true;
        case 3:
            REQUIRE(engine_viewport_perspective(s->viewport));
            look_at(s, CRATE_AT, 0.5f, -0.4f);
            return true;
        /* The stretched box reaches 50 along X; its -X side faces the eye. */
        case 4:
            return grab_handle(s, (CF_V3){ -50, 50, 0 }, x, -25, &from, &to);
        case 5:
        case 9:
        case 13: return move_to(s, to);
        case 6:
        case 10:
        case 14: return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 7:
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 75, 50, 50 }, 0.1f));
            REQUIRE(undoes(s));
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 50, 50, 50 }, 0));
            select_instance(scene, 1);
            look_at(s, BALL_AT, 0.5f, -0.4f);
            return true;
        case 8:
            return grab_handle(s, cf_add_v3(BALL_AT, (CF_V3){ 0, 25, 0 }), y, 15, &from, &to);
        case 11:
            REQUIRE(near(collider_now(s, 1)->radius, 40, 0.1f));
            REQUIRE(undoes(s));
            REQUIRE(collider_now(s, 1)->radius == 25);
            s->prefs.scene_snap = true;
            select_instance(scene, 2);
            look_at(s, PILL_AT, 0.5f, -0.4f);
            return true;
        /* The pill's top, 50 up, dragged 80 down. */
        case 12:
            return grab_handle(s, cf_add_v3(PILL_AT, (CF_V3){ 0, 50, 0 }), y, -80, &from, &to);
        case 15:
            REQUIRE(collider_now(s, 2)->height == 40);
            REQUIRE(collider_now(s, 2)->radius == 20);
            REQUIRE(undoes(s));
            REQUIRE(collider_now(s, 2)->height == 100);
            return true;
        case COLLIDER_FRAMES: edits++; return true;
        default: return true;
    }
}

TEST_CASE(collider_3d)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(COLLIDER_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == COLLIDER_FRAMES && edits == 1);
    printf("Studio 3D collider handles accepted: frames=%d\n", frames);
    return true;
}
