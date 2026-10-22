#include "editor/workspace/scene_collider3d.h"
#include "scene_3d.h"

/* A 3D scene for the collider handles: a crate whose box a Transform3D
   stretches twice along X, a ball and a pill, side by side. */
static const char crate_prototype[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Crate\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"Transform3D\", data.Object([\n"
    "                    data.Field.Named(\"position\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(50)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(0))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"rotation\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"w\", data.Value.UInt(1))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"scale\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(2)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(1)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(1))\n"
    "                    ]))\n"
    "                ])),\n"
    "                data.Field.Named(\"Collider3D\", data.Object([\n"
    "                    data.Field.Named(\"size\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(50)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(50)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(50))\n"
    "                    ]))\n"
    "                ]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
static const char ball_prototype[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Ball\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"Transform3D\", data.Object([\n"
    "                    data.Field.Named(\"position\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(200)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(50)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(0))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"rotation\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"w\", data.Value.UInt(1))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"scale\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(1)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(1)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(1))\n"
    "                    ]))\n"
    "                ])),\n"
    "                data.Field.Named(\"Collider3D\", data.Object([\n"
    "                    data.Field.Named(\"shape\", "
    "data.Value.Text(\"Sphere\")),\n"
    "                    data.Field.Named(\"radius\", data.Value.UInt(25))\n"
    "                ]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
static const char pill_prototype[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Pill\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"Transform3D\", data.Object([\n"
    "                    data.Field.Named(\"position\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.Int(-200)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(50)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(0))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"rotation\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(0)),\n"
    "                        data.Field.Named(\"w\", data.Value.UInt(1))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"scale\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.UInt(1)),\n"
    "                        data.Field.Named(\"y\", data.Value.UInt(1)),\n"
    "                        data.Field.Named(\"z\", data.Value.UInt(1))\n"
    "                    ]))\n"
    "                ])),\n"
    "                data.Field.Named(\"Collider3D\", data.Object([\n"
    "                    data.Field.Named(\"shape\", "
    "data.Value.Text(\"Capsule\")),\n"
    "                    data.Field.Named(\"radius\", data.Value.UInt(20)),\n"
    "                    data.Field.Named(\"height\", data.Value.UInt(100))\n"
    "                ]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
static const char collider_scene
    [] = "package asset\n"
         "\n"
         "import data \"game:assets\"\n"
         "\n"
         "pub Build :: () -> byte[] {\n"
         "    return data.Encode(\n"
         "        data.Object([\n"
         "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
         "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
         "            data.Field.Named(\"presentation\", "
         "data.Value.Text(\"3d\")),\n"
         "            data.Field.Named(\"entities\", data.List([\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Crate\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Crate\"))\n"
         "                ]),\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Ball\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Ball\"))\n"
         "                ]),\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Pill\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Pill\"))\n"
         "                ])\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";
#define COLLIDER_FRAMES 28
static const CF_V3 CRATE_AT = { 0, 50, 0 };
static const CF_V3 BALL_AT = { 200, 50, 0 };
static const CF_V3 PILL_AT = { -200, 50, 0 };

static const Collider3D *collider_now(studio *s, int actor)
{
    return Collider3D_get_const(s->scene->world, s->scene->actors[actor].entity);
}

/* Each typed collider edit redoes exactly in one step; leave it undone. */
static bool collider_undoes(studio *s)
{
    REQUIRE(!s->scene->gizmo.active);
    Collider3D colliders[3];
    Transform3D transforms[3];
    for (int i = 0; i < 3; i++) {
        colliders[i] = *collider_now(s, i);
        transforms[i] = *Transform3D_get_const(
            s->scene->world,
            s->scene->actors[i].entity
        );
    }
    studio_command_run(s, STUDIO_COMMAND_UNDO);
    REQUIRE(scene_history_can_redo(s->scene->history));
    studio_command_run(s, STUDIO_COMMAND_REDO);
    for (int i = 0; i < 3; i++) {
        REQUIRE(!memcmp(&colliders[i], collider_now(s, i), sizeof colliders[i]));
        const Transform3D *transform = Transform3D_get_const(
            s->scene->world,
            s->scene->actors[i].entity
        );
        REQUIRE(!memcmp(&transforms[i], transform, sizeof transforms[i]));
    }
    studio_command_run(s, STUDIO_COMMAND_UNDO);
    return true;
}

/* Presses the collider handle at `handle` and aims to drag it `distance`
   along `axis`. */
static bool grab_handle(studio *s, CF_V3 handle, CF_V3 axis, float distance, CF_V2 *from, CF_V2 *to)
{
    *from = shown_at(s, handle);
    *to = shown_along(s, handle, axis, distance);
    scene_view3d view = scene_view3d_of(s->scene);
    int part = scene_collider3d_part(s->scene, &view, scene_view3d_on_screen(&view, handle));
    /* Hidden objects deliberately take no handle; visible ones must start
       the collider edit, rather than an ordinary object drag. */
    bool hidden = app_scene_authoring_actor_view(s->scene, s->scene->selected) &
                  APP_SCENE_VIEW_HIDDEN;
    if (!hidden) {
        if (part < SCENE_GIZMO_FACE || part >= SCENE_GIZMO_HANDLE) {
            fprintf(
                stderr,
                "Collider handle miss: tool=%d selected=%d movable=%d "
                "forward=(%g,%g,%g) eye=(%g,%g,%g) handle=(%g,%g,%g) "
                "depth=%g\n",
                s->scene->gizmo.in.tool,
                s->scene->selected,
                scene_can_move(s->scene),
                view.forward.x,
                view.forward.y,
                view.forward.z,
                view.camera.eye.x,
                view.camera.eye.y,
                view.camera.eye.z,
                handle.x,
                handle.y,
                handle.z,
                scene_view3d_world_per_pixel(&view, handle)
            );
        }
        REQUIRE(part >= SCENE_GIZMO_FACE && part < SCENE_GIZMO_HANDLE);
    }
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
    static bool dirty, can_undo, can_redo;
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
            {
                scene_view3d view = scene_view3d_of(scene);
                CF_Ray3 parallel = { .p = { -100, 50, 0 }, .d = { 1, 0, 0 } };
                REQUIRE(!scene_collider3d_begin(scene, SCENE_GIZMO_FACE, &view, parallel));
                REQUIRE(!scene->gizmo.active && !acount(scene->gizmo.fields));
                Collider3D
                    *collider = Collider3D_get(scene->world, scene->actors[0].entity);
                Collider3D saved = *collider;
                collider->size.x = NAN;
                CF_Ray3 ordinary = { .p = { 0, 50, 100 }, .d = { 0, 0, -1 } };
                REQUIRE(!scene_collider3d_begin(scene, SCENE_GIZMO_FACE, &view, ordinary));
                REQUIRE(!scene->gizmo.active && !acount(scene->gizmo.fields));
                *collider = saved;
            }
            look_at(s, CRATE_AT, 0.5f, -0.4f);
            return true;
        /* The stretched box reaches 50 along X; its -X side faces the eye. */
        case 4:
            return grab_handle(s, (CF_V3){ -50, 50, 0 }, x, -25, &from, &to);
        case 5:
        case 9:
        case 13:
            if (scene->gizmo.active < SCENE_GIZMO_FACE ||
                scene->gizmo.active >= SCENE_GIZMO_HANDLE) {
                fprintf(
                    stderr,
                    "Collider gesture did not start: active=%d hover=%d "
                    "selected=%d tool=%d\n",
                    scene->gizmo.active,
                    scene->gizmo.hover,
                    scene->selected,
                    scene->gizmo.in.tool
                );
            }
            REQUIRE(
                scene->gizmo.active >= SCENE_GIZMO_FACE && scene->gizmo.active < SCENE_GIZMO_HANDLE
            );
            REQUIRE(acount(scene->gizmo.fields) == 1);
            return move_to(s, to);
        case 6:
        case 10:
        case 14: return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 7:
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 75, 50, 50 }, 0.1f));
            REQUIRE(collider_undoes(s));
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 50, 50, 50 }, 0));
            select_instance(scene, 1);
            look_at(s, BALL_AT, 0.5f, -0.4f);
            return true;
        case 8:
            return grab_handle(s, cf_add_v3(BALL_AT, (CF_V3){ 0, 25, 0 }), y, 15, &from, &to);
        case 11:
            REQUIRE(near(collider_now(s, 1)->radius, 40, 0.1f));
            REQUIRE(collider_undoes(s));
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
            REQUIRE(collider_undoes(s));
            REQUIRE(collider_now(s, 2)->height == 100);
            s->prefs.scene_snap = false;
            select_instance(scene, 0);
            look_at(s, CRATE_AT, 0.5f, -0.4f);
            dirty = scene->scene_dirty;
            can_undo = scene_history_can_undo(scene->history);
            can_redo = scene_history_can_redo(scene->history);
            return true;
        case 16:
            return grab_handle(s, (CF_V3){ -50, 50, 0 }, x, -25, &from, &to);
        case 17: return move_to(s, to);
        case 18:
            REQUIRE(
                scene->gizmo.active >= SCENE_GIZMO_FACE && scene->gizmo.active < SCENE_GIZMO_HANDLE
            );
            REQUIRE(collider_now(s, 0)->size.x > 60);
            return press_key(SDLK_ESCAPE);
        case 19:
            REQUIRE(!scene->gizmo.active);
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 50, 50, 50 }, 0));
            REQUIRE(scene->scene_dirty == dirty);
            REQUIRE(scene_history_can_undo(scene->history) == can_undo);
            REQUIRE(scene_history_can_redo(scene->history) == can_redo);
            app_scene_authoring_set_actor_view(scene, 0, APP_SCENE_VIEW_HIDDEN);
            return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 20:
            return grab_handle(s, (CF_V3){ -50, 50, 0 }, x, -25, &from, &to);
        case 21: return move_to(s, to);
        case 22: return button_at(s, SDL_BUTTON_LEFT, false, to);
        case 23:
            REQUIRE(!scene->gizmo.active);
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 50, 50, 50 }, 0));
            REQUIRE(scene->scene_dirty == dirty);
            REQUIRE(scene_history_can_undo(scene->history) == can_undo);
            REQUIRE(scene_history_can_redo(scene->history) == can_redo);
            app_scene_authoring_set_actor_view(scene, 0, 0);
            select_instance(scene, 0);
            return grab_handle(s, (CF_V3){ -50, 50, 0 }, x, -25, &from, &to);
        case 24: return move_to(s, to);
        case 25:
            REQUIRE(
                scene->gizmo.active >= SCENE_GIZMO_FACE && scene->gizmo.active < SCENE_GIZMO_HANDLE
            );
            REQUIRE(collider_now(s, 0)->size.x > 60);
            return press_key(SDLK_H);
        case 26:
            REQUIRE(app_scene_authoring_actor_view(scene, 0) & APP_SCENE_VIEW_HIDDEN);
            REQUIRE(!scene->gizmo.active);
            REQUIRE(near_v3(collider_now(s, 0)->size, (CF_V3){ 50, 50, 50 }, 0));
            app_scene_authoring_set_actor_view(scene, 0, 0);
            REQUIRE(scene->scene_dirty == dirty);
            REQUIRE(scene_history_can_undo(scene->history) == can_undo);
            REQUIRE(scene_history_can_redo(scene->history) == can_redo);
            return button_at(s, SDL_BUTTON_LEFT, false, to);
        /* Show Colliders outlines every object's collider with nothing
           selected. */
        case 27:
            REQUIRE(!scene->view_options.colliders);
            studio_command_run(s, STUDIO_COMMAND_DESELECT);
            studio_command_run(s, STUDIO_COMMAND_SHOW_COLLIDERS);
            look_at(s, CRATE_AT, 0.5f, -0.4f);
            return true;
        case COLLIDER_FRAMES:
            REQUIRE(studio_command_checked(s, STUDIO_COMMAND_SHOW_COLLIDERS) == 1);
            REQUIRE(scene->view_options.colliders);
            edits++;
            return true;
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
    printf("\nStudio 3D collider handles accepted: frames=%d\n", frames);
    return true;
}
