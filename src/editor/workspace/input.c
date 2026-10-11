#include "editor/workspace/input.h"

#include "render/device_controls.h"

#include <assert.h>
#include <cute_binding.h>

static const CF_KeyButton keys[] = {
    CF_KEY_ESCAPE, CF_KEY_Z,      CF_KEY_Y,     CF_KEY_C,
    CF_KEY_X,      CF_KEY_V,      CF_KEY_D,     CF_KEY_S,
    CF_KEY_DELETE, CF_KEY_LSHIFT, CF_KEY_LCTRL,
#ifdef __APPLE__
    CF_KEY_LGUI,
#else
    CF_KEY_LCTRL,
#endif
};
_Static_assert(sizeof keys / sizeof *keys == APP_INPUT_COUNT, "action keys");

static CF_ButtonBinding buttons[APP_INPUT_COUNT];
static bool ready;
static bool first_frame;
static bool held_at_start[APP_INPUT_COUNT];
static bool controller_aim;
static bool pressed_actions[APP_INPUT_COUNT], frame_actions[APP_INPUT_COUNT];
static uint64_t held_controls;
static uint64_t pressed_controls, frame_controls;

/* Preserve a tap whose down and up events arrive between rendered frames. */
static void key_changed(CF_KeyButton key, bool down)
{
    if (!down) {
        return;
    }
    for (int i = 0; i < APP_INPUT_COUNT; i++) {
        pressed_actions[i] |= key == keys[i];
    }
    pressed_actions[APP_INPUT_DELETE] |= key == CF_KEY_BACKSPACE;
    pressed_actions[APP_INPUT_SHIFT] |= key == CF_KEY_RSHIFT;
    pressed_actions[APP_INPUT_CONTROL] |= key == CF_KEY_RCTRL;
#ifdef __APPLE__
    pressed_actions[APP_INPUT_COMMAND] |= key == CF_KEY_RGUI;
#else
    pressed_actions[APP_INPUT_COMMAND] |= key == CF_KEY_RCTRL;
#endif
    Control source = control_for_key(key);
    if (source) {
      pressed_controls |= UINT64_C(1) << source;
    }
}

void app_input_initialize()
{
    if (ready) {
        return;
    }
    for (int i = 0; i < APP_INPUT_COUNT; i++) {
        pressed_actions[i] = frame_actions[i] = false;
        buttons[i] = cf_make_button_binding(0, 0);
        cf_button_binding_add_key(buttons[i], keys[i]);
        if (i >= APP_INPUT_UNDO && i <= APP_INPUT_DELETE) {
            cf_button_binding_set_repeat(buttons[i], 0.275f, 0.05f);
        }
    }
    cf_button_binding_add_key(buttons[APP_INPUT_DELETE], CF_KEY_BACKSPACE);
    cf_button_binding_add_key(buttons[APP_INPUT_SHIFT], CF_KEY_RSHIFT);
    cf_button_binding_add_key(buttons[APP_INPUT_CONTROL], CF_KEY_RCTRL);
#ifdef __APPLE__
    cf_button_binding_add_key(buttons[APP_INPUT_COMMAND], CF_KEY_RGUI);
#else
    cf_button_binding_add_key(buttons[APP_INPUT_COMMAND], CF_KEY_RCTRL);
#endif
    first_frame = true;
    controller_aim = false;
    pressed_controls = frame_controls = 0;
    cf_register_key_callback(key_changed);
    ready = true;
}

/* Sample after Cute updates its bindings, before any application consumer.
 * Held movement remains continuous; a held action must be released before it
 * can fire after startup. */
void app_input_frame()
{
    if (!ready) {
        return;
    }
    for (int i = 0; i < APP_INPUT_COUNT; i++) {
        frame_actions[i] = pressed_actions[i];
        pressed_actions[i] = false;
        bool down = cf_button_binding_down(buttons[i]);
        if (first_frame) {
            held_at_start[i] = down;
        }
        if (!down) {
            held_at_start[i] = false;
        }
    }
    uint64_t controls = controls_down();
    frame_controls = controls | pressed_controls;
    pressed_controls = 0;
    held_controls = first_frame ? controls : held_controls & controls;
    first_frame = false;
    CF_V2 aim = controls_aim();
    if (aim.x != 0 || aim.y != 0) {
      controller_aim = true;
    } else if (cf_mouse_motion_x() != 0 || cf_mouse_motion_y() != 0 ||
               cf_mouse_down(CF_MOUSE_BUTTON_LEFT)) {
      controller_aim = false;
    }
}

void app_input_shutdown()
{
    if (!ready) {
        return;
    }
    for (int i = 0; i < APP_INPUT_COUNT; i++) {
        cf_destroy_button_binding(buttons[i]);
    }
    cf_register_key_callback(NULL);
    ready = false;
}

bool app_input_pressed(app_input_action action)
{
    assert(action >= 0 && action < APP_INPUT_COUNT);
    return ready && !held_at_start[action] &&
           (frame_actions[action] || cf_button_binding_pressed(buttons[action]) ||
            cf_button_binding_repeated(buttons[action]));
}

void app_input_movement(float *x, float *y)
{
    CF_V2 movement = ready ? controls_movement() : (CF_V2){ 0 };
    *x = movement.x;
    *y = movement.y;
}

bool app_input_aim(float *x, float *y)
{
    CF_V2 aim = ready ? controls_aim() : (CF_V2){ 0 };
    *x = aim.x;
    *y = aim.y;
    return aim.x || aim.y;
}

uint64_t app_input_controls()
{
    return ready ? frame_controls & ~held_controls : 0;
}

bool app_input_mouse_aiming()
{
    return ready && !controller_aim;
}
