#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor_camera.h"
#include "ui/input_feel.h"

#include <math.h>

/* ---------- The 3D camera ---------- */

/* The 3D camera's gestures, as Unity's Scene view's. */
enum
{
    CAMERA_IDLE,
    CAMERA_ORBIT, /* Alt with a left drag turns about the pivot */
    CAMERA_PAN,   /* the middle button, or Space with a left drag */
    CAMERA_LOOK,  /* the right button: the pointer looks, the fly keys fly */
};

static bool camera_free(const studio *s)
{
    return !app_scene_authoring_runtime_state(s->scene).playing ||
           (s->mods & SDL_KMOD_ALT);
}

void studio_scene_camera_cancel(studio *s)
{
    s->scene_nav.camera_drag = CAMERA_IDLE;
    s->scene_nav.fly_keys = 0;
}

void studio_scene_camera_focus_lost(studio *s)
{
    studio_scene_camera_cancel(s);
    for (int i = 0; i < 3; i++) {
        s->in.down[i] = s->in.pressed[i] = s->in.released[i] = false;
    }
}

/* Radians the camera turns for each pixel a drag moves, orbiting or looking. */
static const float CAMERA_TURN_RATE = 0.006f;
/* Flying covers the distance to the pivot each second, faster with Shift. */
static const float FLY_RATE = 1;
static const float FLY_FAST = 4;
/* The longest step one frame flies, so a stalled frame does not leap. */
static const double FLY_STEP_MAX = 0.1;

/* The fly keys and where each moves: x right, y up, z forward. */
static const SDL_Keycode FLY_KEYS[] = { SDLK_W, SDLK_A, SDLK_S,
                                        SDLK_D, SDLK_Q, SDLK_E };
static const CF_V3 FLY_STEPS[] = { { 0, 0, 1 }, { -1, 0, 0 }, { 0, 0, -1 },
                                   { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 } };
#define FLY_KEY_COUNT ((int)(sizeof FLY_KEYS / sizeof *FLY_KEYS))

bool studio_scene_fly_key(studio *s, const SDL_KeyboardEvent *key)
{
    for (int i = 0; i < FLY_KEY_COUNT; i++) {
        if (key->key != FLY_KEYS[i]) {
            continue;
        }
        unsigned bit = 1u << i;
        bool held = (s->scene_nav.fly_keys & bit) != 0;
        if (!key->down) {
            s->scene_nav.fly_keys &= ~bit;
            return held;
        }
        kit_rect r = s->surface.rect;
        float x = s->in.mouse_x, y = s->in.mouse_y;
        bool starts = s->scene_nav.camera_drag == CAMERA_IDLE &&
                      s->in.pressed[1] && camera_free(s) &&
                      engine_viewport_perspective(s->viewport) && !s->guest &&
                      !studio_previewed_ui(s) && !kit_wants_keyboard(s->kit) &&
                      s->scene->gizmo.active == SCENE_GIZMO_NONE && x >= r.x &&
                      x < r.x + r.w && y >= r.y && y < r.y + r.h;
        /* Keys own the look in the same event batch as its button press. */
        if ((!starts && s->scene_nav.camera_drag != CAMERA_LOOK) || !s->in.down[1]) {
            return false;
        }
        s->scene_nav.fly_keys |= bit;
        return true;
    }
    return false;
}

bool studio_scene_flying(const studio *s)
{
    return s->scene_nav.camera_drag == CAMERA_LOOK;
}

static void drop_button(engine_viewport_host_input *input, int button)
{
    input->down[button] = input->pressed[button] = input->released[button] = false;
}

/* Starts the gesture a press over the Scene begins, if it begins one. */
static void begin_camera_drag(studio *s, const engine_viewport_host_input *input)
{
    if (s->scene_nav.camera_drag != CAMERA_IDLE || !s->surface.hovered) {
        return;
    }
    int drag = input->pressed[2]               ? CAMERA_PAN
             : input->pressed[0] && input->alt ? CAMERA_ORBIT
             : input->pressed[1] && s->scene->gizmo.active == SCENE_GIZMO_NONE
                 ? CAMERA_LOOK
                 : CAMERA_IDLE;
    s->scene_nav.camera_drag = drag;
    s->scene_nav.drag_x = input->x;
    s->scene_nav.drag_y = input->y;
    s->scene_nav.flown_at = s->time;
}

/* Flies the camera along the fly keys held, for the time since the last
   frame flew. */
static bool fly(studio *s, editor_camera *camera)
{
    CF_V3 step = { 0 };
    for (int i = 0; i < FLY_KEY_COUNT; i++) {
        if (s->scene_nav.fly_keys & (1u << i)) {
            step = cf_add_v3(step, FLY_STEPS[i]);
        }
    }
    double seconds = fmin(s->time - s->scene_nav.flown_at, FLY_STEP_MAX);
    s->scene_nav.flown_at = s->time;
    if (cf_dot_v3(step, step) == 0 || seconds <= 0) {
        return false;
    }
    float speed = camera->distance * FLY_RATE *
                  ((s->mods & SDL_KMOD_SHIFT) ? FLY_FAST : 1);
    editor_camera_fly(camera, cf_mul_v3_f(cf_norm_v3(step), speed * (float)seconds));
    return true;
}

/* The gesture in progress moves the camera by the pointer's move since the
   last frame and ends as its button lifts; the button is the camera's. */
static bool drag_camera(studio *s, editor_camera *camera, engine_viewport_host_input *input)
{
    int drag = s->scene_nav.camera_drag;
    if (drag == CAMERA_IDLE) {
        return false;
    }
    float dx = input->x - s->scene_nav.drag_x, dy = input->y - s->scene_nav.drag_y;
    s->scene_nav.drag_x = input->x;
    s->scene_nav.drag_y = input->y;
    bool moved = dx != 0 || dy != 0;
    int button = drag == CAMERA_ORBIT ? 0 : drag == CAMERA_PAN ? 2 : 1;
    switch (drag) {
        case CAMERA_ORBIT:
            /* The scene turns with the pointer, as if grabbed. */
            editor_camera_orbit(camera, dx * CAMERA_TURN_RATE, -dy * CAMERA_TURN_RATE);
            break;
        case CAMERA_PAN:
            editor_camera_pan(
                camera,
                (CF_V2){ 2 * dx / input->width, -2 * dy / input->height }
            );
            break;
        default:
            editor_camera_look(camera, dx * CAMERA_TURN_RATE, -dy * CAMERA_TURN_RATE);
            moved |= fly(s, camera);
            break;
    }
    if (!input->down[button]) {
        studio_scene_camera_cancel(s);
    }
    drop_button(input, button);
    return moved;
}

/* In a 3D view the camera orbits its pivot with Alt and a left drag, pans
   with the middle button, Space with a drag and a trackpad's scroll,
   dollies toward the pointer with the wheel and a pinch and, while the
   right button is held, looks with the pointer and flies with W, A, S, D,
   Q and E (Unity's). The viewport's own 2D framing hears none of it. */
void studio_scene_camera_frame(studio *s, engine_viewport_host_input *input)
{
    if (!camera_free(s) || input->width <= 0 || input->height <= 0) {
        studio_scene_camera_cancel(s);
        return;
    }
    begin_camera_drag(s, input);
    editor_camera camera = editor_camera_get(s->viewport);
    bool moved = drag_camera(s, &camera, input);
    if (input->wheel != 0) {
        CF_V2 at = { 2 * input->x / input->width - 1, 1 - 2 * input->y / input->height };
        editor_camera_dolly(&camera, 1 / feel_zoom_scale(input->wheel), at);
        input->wheel = 0;
        moved = true;
    }
    if (s->surface.hovered && (s->scroll_x != 0 || s->scroll_y != 0)) {
        /* As far as the scroll pans a 2D view. */
        editor_camera_pan(
            &camera,
            (CF_V2){ -s->scroll_x * 2 * FEEL_SCROLL_POINTS / input->width,
                     -s->scroll_y * 2 * FEEL_SCROLL_POINTS / input->height }
        );
        moved = true;
    }
    drop_button(input, 2);
    if (moved) {
        editor_camera_set(s->viewport, &camera);
    }
}
