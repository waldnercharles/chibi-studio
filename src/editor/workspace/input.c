#include "editor/workspace/input.h"

#include <assert.h>
#include <cute_binding.h>
#include <math.h>

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
static CF_AxisBinding horizontal, vertical;
static CF_AxisBinding aim_horizontal, aim_vertical;
static bool ready;
static bool first_frame;
static bool held_at_start[APP_INPUT_COUNT];
static bool controller_aim;
static bool pressed_actions[APP_INPUT_COUNT], frame_actions[APP_INPUT_COUNT];
static uint64_t held_controls;
static uint64_t pressed_controls, frame_controls;
static uint64_t raw_controls();

static void add_gameplay_stick_y(CF_AxisBinding binding, CF_JoypadAxis axis, float threshold)
{
    /* Cute's stick helpers make Y positive up; gameplay coordinates make Y positive down. */
    cf_axis_binding_add_triggers(binding, axis, axis, threshold);
}

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
    int source = GAME_CONTROL_NONE;
    if (key >= CF_KEY_A && key <= CF_KEY_Z) {
        source = GAME_CONTROL_A + key - CF_KEY_A;
    } else {
        switch (key) {
            case CF_KEY_SPACE: source = GAME_CONTROL_SPACE; break;
            case CF_KEY_LSHIFT:
            case CF_KEY_RSHIFT: source = GAME_CONTROL_SHIFT; break;
            case CF_KEY_TAB: source = GAME_CONTROL_TAB; break;
            case CF_KEY_RETURN: source = GAME_CONTROL_ENTER; break;
            default: return;
        }
    }
    pressed_controls |= UINT64_C(1) << source;
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
    horizontal = cf_make_axis_binding(0);
    vertical = cf_make_axis_binding(0);
    cf_axis_binding_add_keys(horizontal, CF_KEY_A, CF_KEY_D);
    cf_axis_binding_add_keys(horizontal, CF_KEY_LEFT, CF_KEY_RIGHT);
    cf_axis_binding_add_keys(vertical, CF_KEY_W, CF_KEY_S);
    cf_axis_binding_add_keys(vertical, CF_KEY_UP, CF_KEY_DOWN);
    cf_axis_binding_add_left_stick_x(horizontal, 0.18f);
    add_gameplay_stick_y(vertical, CF_JOYPAD_AXIS_LEFTY, 0.18f);
    aim_horizontal = cf_make_axis_binding(0);
    aim_vertical = cf_make_axis_binding(0);
    cf_axis_binding_add_right_stick_x(aim_horizontal, 0.18f);
    add_gameplay_stick_y(aim_vertical, CF_JOYPAD_AXIS_RIGHTY, 0.18f);
    cf_axis_binding_set_conflict(horizontal, CF_AXIS_CONFLICT_CANCEL);
    cf_axis_binding_set_conflict(vertical, CF_AXIS_CONFLICT_CANCEL);
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
    uint64_t controls = raw_controls();
    frame_controls = controls | pressed_controls;
    pressed_controls = 0;
    held_controls = first_frame ? controls : held_controls & controls;
    first_frame = false;
    float aim_x = cf_axis_binding_value(aim_horizontal);
    float aim_y = cf_axis_binding_value(aim_vertical);
    if (aim_x != 0 || aim_y != 0) {
        controller_aim = true;
    } else if (cf_mouse_motion_x() != 0 || cf_mouse_motion_y() != 0 || cf_mouse_down(CF_MOUSE_BUTTON_LEFT)) {
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
    cf_destroy_axis_binding(horizontal);
    cf_destroy_axis_binding(vertical);
    cf_destroy_axis_binding(aim_horizontal);
    cf_destroy_axis_binding(aim_vertical);
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
    *x = ready ? cf_axis_binding_value(horizontal) : 0;
    *y = ready ? cf_axis_binding_value(vertical) : 0;
    float length = hypotf(*x, *y);
    if (length > 1) {
        *x /= length;
        *y /= length;
    }
}

bool app_input_aim(float *x, float *y)
{
    *x = ready ? cf_axis_binding_value(aim_horizontal) : 0;
    *y = ready ? cf_axis_binding_value(aim_vertical) : 0;
    float length = hypotf(*x, *y);
    if (length <= 0) {
        return false;
    }
    *x /= length;
    *y /= length;
    return true;
}

static bool control_down(GameControl source)
{
    if (source >= GAME_CONTROL_A && source <= GAME_CONTROL_Z) {
        return cf_key_down(CF_KEY_A + source - GAME_CONTROL_A);
    }
    if (source >= GAME_CONTROL_PAD_A && source <= GAME_CONTROL_PAD_Y) {
        return cf_joypad_button_down(0, CF_JOYPAD_BUTTON_A + source - GAME_CONTROL_PAD_A);
    }
    switch (source) {
        case GAME_CONTROL_MOUSE_LEFT:
            return cf_mouse_down(CF_MOUSE_BUTTON_LEFT);
        case GAME_CONTROL_MOUSE_RIGHT:
            return cf_mouse_down(CF_MOUSE_BUTTON_RIGHT);
        case GAME_CONTROL_SPACE: return cf_key_down(CF_KEY_SPACE);
        case GAME_CONTROL_SHIFT: return cf_key_shift();
        case GAME_CONTROL_TAB: return cf_key_down(CF_KEY_TAB);
        case GAME_CONTROL_ENTER: return cf_key_down(CF_KEY_RETURN);
        case GAME_CONTROL_PAD_LEFT_SHOULDER:
            return cf_joypad_button_down(0, CF_JOYPAD_BUTTON_LEFTSHOULDER);
        case GAME_CONTROL_PAD_RIGHT_SHOULDER:
            return cf_joypad_button_down(0, CF_JOYPAD_BUTTON_RIGHTSHOULDER);
        case GAME_CONTROL_PAD_LEFT_TRIGGER:
            return cf_joypad_axis(0, CF_JOYPAD_AXIS_TRIGGERLEFT) > 8192;
        case GAME_CONTROL_PAD_RIGHT_TRIGGER:
            return cf_joypad_axis(0, CF_JOYPAD_AXIS_TRIGGERRIGHT) > 8192;
        case GAME_CONTROL_AIM_STICK: {
            float x, y;
            return app_input_aim(&x, &y);
        }
        default: return false;
    }
}

static uint64_t raw_controls()
{
    uint64_t controls = 0;
    for (int i = 1; i < GAME_CONTROL_COUNT; i++) {
        if (control_down(i)) {
            controls |= UINT64_C(1) << i;
        }
    }
    return controls;
}

uint64_t app_input_controls()
{
    return ready ? frame_controls & ~held_controls : 0;
}

bool app_input_mouse_aiming()
{
    return ready && !controller_aim;
}
