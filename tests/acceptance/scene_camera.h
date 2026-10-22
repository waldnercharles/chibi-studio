#include "editor/workspace/checkpoint.h"
#include "editor/workspace/editor_camera.h"
#include "render/scene3d.h"
#include "ui/input_feel.h"

/* A 3D scene of one block 100 units on a side, standing at x 200. */
static const char block_prototype[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Block\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"Transform\", data.Object([\n"
    "                    data.Field.Named(\"x\", data.Value.UInt(200)),\n"
    "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
    "                ])),\n"
    "                data.Field.Named(\"Geometry3D\", data.Object([\n"
    "                    data.Field.Named(\"primitive\", data.Value.UInt(0)),\n"
    "                    data.Field.Named(\"width\", data.Value.Real(100.0)),\n"
    "                    data.Field.Named(\"depth\", data.Value.Real(100.0)),\n"
    "                    data.Field.Named(\"height\", "
    "data.Value.Real(100.0)),\n"
    "                    data.Field.Named(\"elevation\", "
    "data.Value.Real(50.0))\n"
    "                ]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
static const char block_scene
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
         "data.Value.Text(\"Block\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Block\"))\n"
         "                ])\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";
#define CAMERA_FRAMES 30
/* The frame the right button goes down to look, then fly. */
#define LOOK_FRAME 14

/* Queues `button` going down or up at the window point (x, y). */
static bool push_button(Uint8 button, bool down, float x, float y)
{
    SDL_WindowID window = window_id();
    REQUIRE(window);
    Uint32 type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    SDL_Event e = { .button = { .type = type,
                                .windowID = window,
                                .button = button,
                                .down = down,
                                .clicks = 1,
                                .x = x,
                                .y = y } };
    REQUIRE(SDL_PushEvent(&e));
    return true;
}

/* The window point at normalized device point `ndc` of the Scene's image. */
static CF_V2 scene_point(studio *s, CF_V2 ndc)
{
    kit_rect r = s->surface.rect;
    float ui = s->prefs.ui_scale;
    return (CF_V2){ (r.x + (ndc.x + 1) * 0.5f * r.w) * ui,
                    (r.y + (1 - ndc.y) * 0.5f * r.h) * ui };
}

/* Queues the pointer moving to, then `button` changing at, `ndc`. */
static bool button_at(studio *s, Uint8 button, bool down, CF_V2 ndc)
{
    CF_V2 at = scene_point(s, ndc);
    REQUIRE(push_mouse(SDL_EVENT_MOUSE_MOTION, at.x, at.y, 0, 0));
    return push_button(button, down, at.x, at.y);
}

static bool move_to(studio *s, CF_V2 ndc)
{
    CF_V2 at = scene_point(s, ndc);
    return push_mouse(SDL_EVENT_MOUSE_MOTION, at.x, at.y, 0, 0);
}

/* Where `world` shows in the Scene's image, in normalized device units. */
static CF_V2 shown_at(studio *s, CF_V3 world)
{
    render_scene3d_camera camera;
    engine_viewport_get_camera3d(s->viewport, &camera);
    CF_V3 at = render_scene3d_project(&camera, world);
    return (CF_V2){ at.x, at.y };
}

static bool near(float a, float b, float tolerance)
{
    if (fabsf(a - b) > tolerance) {
        fprintf(stderr, "Studio camera acceptance: %g is not %g\n", a, b);
    }
    return fabsf(a - b) <= tolerance;
}

static bool near_v3(CF_V3 a, CF_V3 b, float tolerance)
{
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

/* A view keeps its lens through a checkpoint, and one saved with nine
   numbers before views had a lens still reads. */
static bool checkpoint_keeps_lens(studio *s)
{
    engine_viewport_view view, read;
    engine_viewport_get_view(s->viewport, &view);
    REQUIRE(cf_dot_v3(view.lens.forward, view.lens.forward) > 0);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, checkpoint_camera(doc, view));
    yyjson_doc *saved = yyjson_mut_doc_imut_copy(doc, NULL);
    yyjson_mut_doc_free(doc);
    bool kept = checkpoint_read_camera(yyjson_doc_get_root(saved), &read);
    yyjson_doc_free(saved);
    REQUIRE(kept && read.width == view.width && read.lens.fov == view.lens.fov);
    REQUIRE(
        near_v3(read.lens.eye, view.lens.eye, 0) &&
        near_v3(read.lens.forward, view.lens.forward, 0)
    );
    const char nine[] = "[1,2,3,4,0,0,0,0,0]";
    yyjson_doc *old = yyjson_read(nine, sizeof nine - 1, 0);
    kept = checkpoint_read_camera(yyjson_doc_get_root(old), &read);
    yyjson_doc_free(old);
    REQUIRE(kept && read.width == 3 && cf_dot_v3(read.lens.forward, read.lens.forward) == 0);
    return true;
}

/* A 3D view's camera: Alt with a left drag orbits the pivot and selects
   nothing, the middle button pans so the pivot follows the pointer, the
   wheel dollies toward the pointer, keeping the point under it, and the
   right button looks while W flies forward and switches no tool. Frame
   then centers the block along the turned direction, a press above a
   level view's horizon still selects and drags nothing, and a checkpoint
   keeps the lens. */
static bool scene_camera_frame(void *state)
{
    static int scene_area;
    static editor_camera before;
    static float zoom;
    static CF_V2 anchor;
    static CF_V3 anchored;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= CAMERA_FRAMES);
    app_scene_authoring *scene = s->scene;
    editor_camera camera = s->viewport ? editor_camera_get(s->viewport)
                                       : (editor_camera){ 0 };
    float rate = 0.006f * s->prefs.ui_scale; /* radians per point dragged */
    switch (frames) {
        case 1:
            REQUIRE(app_scene_authoring_register_prototype(scene, block_prototype, NULL) == 0);
            REQUIRE(scene_authoring_load_text(scene, block_scene, NULL) == 0);
            studio_command_run(s, STUDIO_COMMAND_DESELECT);
            return true;
        case 4:
            REQUIRE(engine_viewport_perspective(s->viewport));
            REQUIRE(scene_selection_count(scene) == 0);
            before = camera;
            zoom = app_scene_authoring_zoom(scene);
            REQUIRE(zoom > 0);
            REQUIRE(push_key(SDLK_LALT, SDL_KMOD_LALT, true));
            return button_at(s, SDL_BUTTON_LEFT, true, (CF_V2){ 0, 0 });
        case 5:
            /* 100 points right and 50 down. */
            REQUIRE(studio_scene_flying(s) == false);
            return move_to(
                s,
                (CF_V2){ 200 / s->surface.rect.w, -100 / s->surface.rect.h }
            );
        case 6:
            REQUIRE(near(camera.yaw, before.yaw + 100 * rate, 1e-3f));
            REQUIRE(near(camera.pitch, fmaxf(before.pitch - 50 * rate, -1.5533430f), 1e-3f));
            REQUIRE(near_v3(camera.pivot, before.pivot, 1e-2f));
            /* Turning is no zoom. */
            REQUIRE(near(app_scene_authoring_zoom(scene), zoom, 1e-4f));
            REQUIRE(push_button(SDL_BUTTON_LEFT, false, 0, 0));
            return push_key(SDLK_LALT, SDL_KMOD_NONE, false);
        case 7:
            REQUIRE(scene_selection_count(scene) == 0 && !scene->marquee);
            before = camera;
            return button_at(s, SDL_BUTTON_MIDDLE, true, (CF_V2){ 0, 0 });
        case 8: return move_to(s, (CF_V2){ 0.25f, 0 });
        case 9:
            /* The pivot followed the pointer a quarter across. */
            REQUIRE(near(camera.yaw, before.yaw, 1e-5f));
            REQUIRE(near(shown_at(s, before.pivot).x, 0.25f, 1e-3f));
            REQUIRE(near(shown_at(s, before.pivot).y, 0, 1e-3f));
            return button_at(s, SDL_BUTTON_MIDDLE, false, (CF_V2){ 0.25f, 0 });
        case 10: {
            before = camera;
            anchor = (CF_V2){ -0.5f, 0.25f };
            /* The point under the pointer at the pivot's depth. */
            float half = camera.distance * tanf(camera.fov * 0.5f);
            CF_V3 right = { cosf(camera.yaw), 0, sinf(camera.yaw) };
            CF_V3 up = cf_cross_v3(right, editor_camera_forward(&camera));
            anchored = cf_add_v3(
                camera.pivot,
                cf_add_v3(
                    cf_mul_v3_f(right, anchor.x * half * camera.aspect),
                    cf_mul_v3_f(up, anchor.y * half)
                )
            );
            CF_V2 at = scene_point(s, anchor);
            REQUIRE(push_mouse(SDL_EVENT_MOUSE_MOTION, at.x, at.y, 0, 0));
            return push_mouse(SDL_EVENT_MOUSE_WHEEL, at.x, at.y, 0, 1);
        }
        case 11:
            REQUIRE(near(camera.distance, before.distance / feel_zoom_scale(1), 1e-2f));
            REQUIRE(near(shown_at(s, anchored).x, anchor.x, 1e-3f));
            REQUIRE(near(shown_at(s, anchored).y, anchor.y, 1e-3f));
            return true;
        case 12: {
            REQUIRE(fabsf(s->surface.rect.w - s->surface.rect.h) > 10);
            before = camera;
            CF_V2 at = scene_point(s, (CF_V2){ 0, 0 });
            REQUIRE(push_mouse(SDL_EVENT_MOUSE_MOTION, at.x, at.y, 0, 0));
            return push_mouse(SDL_EVENT_MOUSE_WHEEL, at.x, at.y, 0.25f, 0.25f);
        }
        case 13: {
            CF_V2 at = shown_at(s, before.pivot);
            float points = 0.25f * FEEL_SCROLL_POINTS;
            REQUIRE(near(at.x, -2 * points / (s->surface.rect.w * s->prefs.ui_scale), 1e-3f));
            REQUIRE(near(at.y, -2 * points / (s->surface.rect.h * s->prefs.ui_scale), 1e-3f));
            return true;
        }
        case LOOK_FRAME:
            before = camera;
            REQUIRE(button_at(s, SDL_BUTTON_RIGHT, true, (CF_V2){ 0, 0 }));
            return push_key(SDLK_W, SDL_KMOD_NONE, true);
        case LOOK_FRAME + 1:
            REQUIRE(studio_scene_flying(s) && s->scene_nav.fly_keys);
            REQUIRE(s->prefs.tool == SCENE_TOOL_SELECT);
            return true;
        case LOOK_FRAME + 4: {
            /* W flew along the view, and stayed out of the Move tool. */
            CF_V3 moved = cf_sub_v3(camera.pivot, before.pivot);
            REQUIRE(s->prefs.tool == SCENE_TOOL_SELECT && s->scene_nav.fly_keys);
            REQUIRE(
                cf_dot_v3(moved, editor_camera_forward(&before)) >
                0.01f * before.distance
            );
            REQUIRE(near(camera.yaw, before.yaw, 1e-5f));
            REQUIRE(push_key(SDLK_W, SDL_KMOD_NONE, false));
            return push_button(SDL_BUTTON_RIGHT, false, 0, 0);
        }
        case LOOK_FRAME + 5:
            REQUIRE(!studio_scene_flying(s) && !s->scene_nav.fly_keys);
            before = camera;
            studio_command_run(s, STUDIO_COMMAND_FRAME_ALL);
            REQUIRE(near_v3(editor_camera_get(s->viewport).pivot, (CF_V3){ 200, 50, 0 }, 1e-2f));
            REQUIRE(near(editor_camera_get(s->viewport).yaw, before.yaw, 1e-4f));
            return true;
        case LOOK_FRAME + 6:
            /* Level at the block's middle: its upper half is above the horizon. */
            camera.pitch = 0;
            camera.pivot = (CF_V3){ 200, 50, 0 };
            camera.distance = 600;
            editor_camera_set(s->viewport, &camera);
            return true;
        case LOOK_FRAME + 7:
            anchor = shown_at(s, (CF_V3){ 200, 85, 0 });
            REQUIRE(anchor.y > 0);
            return button_at(s, SDL_BUTTON_LEFT, true, anchor);
        case LOOK_FRAME + 8:
            /* Down onto the ground before the press lifts. */
            return move_to(s, (CF_V2){ anchor.x, -0.5f });
        case LOOK_FRAME + 9:
            return button_at(s, SDL_BUTTON_LEFT, false, (CF_V2){ anchor.x, -0.5f });
        case LOOK_FRAME + 10:
            /* Selected, and not dragged: the press had no ground to start from. */
            REQUIRE(scene_selection_count(scene) == 1 && scene->selected == 0);
            REQUIRE(Transform_get_const(scene->world, scene->actors[0].entity)->x == 200);
            return checkpoint_keeps_lens(s);
        case 25:
            REQUIRE(button_at(s, SDL_BUTTON_RIGHT, true, (CF_V2){ 0, 0 }));
            return push_key(SDLK_W, SDL_KMOD_NONE, true);
        case 26: {
            REQUIRE(studio_scene_flying(s) && s->scene_nav.fly_keys);
            before = camera;
            SDL_Event event = { .window = { .type = SDL_EVENT_WINDOW_FOCUS_LOST,
                                            .windowID = window_id() } };
            REQUIRE(SDL_PushEvent(&event));
            return true;
        }
        case 27: {
            REQUIRE(!studio_scene_flying(s) && !s->scene_nav.fly_keys && !s->in.down[1]);
            REQUIRE(near_v3(camera.pivot, before.pivot, 1e-4f));
            SDL_Event event = { .window = { .type = SDL_EVENT_WINDOW_FOCUS_GAINED,
                                            .windowID = window_id() } };
            REQUIRE(SDL_PushEvent(&event));
            return true;
        }
        case 28:
            REQUIRE(!studio_scene_flying(s) && !s->scene_nav.fly_keys);
            REQUIRE(near_v3(camera.pivot, before.pivot, 1e-4f));
            REQUIRE(button_at(s, SDL_BUTTON_RIGHT, true, (CF_V2){ 0, 0 }));
            return push_key(SDLK_W, SDL_KMOD_NONE, true);
        case 29:
            REQUIRE(studio_scene_flying(s) && s->scene_nav.fly_keys);
            scene_area = -1;
            for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
                if (studio_active_layout(s)->editors[i] == STUDIO_EDITOR_SCENE) {
                    REQUIRE(scene_area < 0);
                    scene_area = i;
                }
            }
            REQUIRE(scene_area >= 0);
            studio_area_show(s, scene_area, STUDIO_EDITOR_OUTPUT, NULL);
            return true;
        case CAMERA_FRAMES:
            REQUIRE(!studio_scene_flying(s) && !s->scene_nav.fly_keys);
            studio_area_show(s, scene_area, STUDIO_EDITOR_SCENE, NULL);
            edits++;
            return true;
        default: return true;
    }
}

TEST_CASE(camera_navigation)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(CAMERA_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == CAMERA_FRAMES && edits == 1);
    printf("\nStudio 3D camera accepted: frames=%d\n", frames);
    return true;
}
