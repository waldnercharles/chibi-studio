#include "authoring/ui_loader.h"
#include "diagnostics/tracy.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_nodes.h"
#include "editor/studio/studio_thumbnails.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/viewport.h"
#include "ui/input_feel.h"
#ifdef CHIBI_ENABLE_WEB_TRACE
#include "editor/studio/studio_graph_internal.h"
#endif
#include "platform/cute.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

static studio *web_studio;

/* SDL's browser backend reports no pinch: the page shell turns a browser's
 * pinch, a wheel with Ctrl held or Safari's gesture events, into the pinch
 * SDL's macOS backend reports. */
EMSCRIPTEN_KEEPALIVE void studio_web_pinch(float scale)
{
    SDL_Event event = { .pinch = { .type = SDL_EVENT_PINCH_UPDATE, .scale = scale } };
    SDL_PushEvent(&event);
}

static bool web_wants_text(const studio *s)
{
    if (kit_wants_text(s->kit, NULL) ||
        (s->graph.kit && kit_wants_text(s->graph.kit, NULL))) {
        return true;
    }
    return s->viewport && engine_viewport_ui_typing(s->viewport);
}

/* SDL leaves a key to the page while a field takes text, keeping only
   Control chords from the browser: ⌘S would save the page, ⌘= zoom it and ⌘F
   find in it. Command chords are Studio's, but ⌘V, whose paste event carries
   the clipboard. */
// clang-format off
EM_JS(void, web_keep_chords, (), {
    if (Module.chibiChords) return;
    Module.chibiChords = true;
    window.addEventListener('keydown', (event) => {
        if (event.metaKey && event.key.toLowerCase() !== 'v') event.preventDefault();
    }, true);
})
// clang-format on

EMSCRIPTEN_KEEPALIVE int studio_web_wants_text()
{
    if (!web_studio) {
        return 0;
    }
    return web_wants_text(web_studio) ||
           (web_studio->guest && web_wants_text(web_studio->guest));
}

/* Presses and releases Mod+V for SDL, as the browser kept the key. */
static bool push_paste_key(int meta)
{
    SDL_Event event = { .type = SDL_EVENT_KEY_DOWN };
    event.key.windowID = SDL_GetWindowID(cf_app_get_window());
    event.key.key = SDLK_V;
    event.key.scancode = SDL_SCANCODE_V;
    event.key.mod = meta ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    event.key.down = true;
    if (!SDL_PushEvent(&event)) {
        return false;
    }
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    SDL_PushEvent(&event);
    return true;
}

/* A trusted DOM paste provides text only during its event. Give RmlUi's
 * normal Control+V handler that text through its clipboard interface. */
EMSCRIPTEN_KEEPALIVE int studio_web_paste(const char *text, int meta)
{
    if (!text || !text[0]) {
        return 0;
    }
    rml_backend_set_pending_paste_text(text);
    if (!push_paste_key(meta)) {
        rml_backend_clear_pending_paste_text();
        return 0;
    }
    return 1;
}

/* Mod+V with no field focused: the browser's clipboard text, when its paste
 * event had any, becomes SDL's before the key reaches Studio's Paste and the
 * graph's, so objects and nodes paste between sessions. */
EMSCRIPTEN_KEEPALIVE void studio_web_paste_key(const char *text, int meta)
{
    if (text && text[0]) {
        cf_clipboard_set(text);
    }
    push_paste_key(meta);
}
#endif

#if defined(__EMSCRIPTEN__) && defined(CHIBI_ENABLE_WEB_TRACE)
/* Profiling reads counters on demand; it installs no timer or frame callback. */
EM_JS(void, install_editor_counters, (), {
    if (Module.chibiEditorCounters) {
        return;
    }
    Object.defineProperty(Module, 'chibiEditorCounters', {
        value: function() {
            return JSON.parse(UTF8ToString(_studio_web_counters()));
        },
        writable: false,
        configurable: false
});
})

EMSCRIPTEN_KEEPALIVE const char *studio_web_counters()
{
    static char json[2048];
    if (!web_studio) {
        return "null";
    }
    const studio *s = web_studio;
    rml_backend_stats host = rml_backend_get_stats(s->backend);
    rml_backend_stats graph = s->graph.backend
                                ? rml_backend_get_stats(s->graph.backend)
                                : (rml_backend_stats){ 0 };
    const struct studio_graph_calculations *cache = s->graph.calculations;
    snprintf(
        json,
        sizeof json,
        "{\"host\":{\"geometry_compiles\":%llu,\"uploaded_bytes\":%llu,"
        "\"render_passes\":%llu,\"static_layer_updates\":%llu,"
        "\"static_partial_updates\":%llu,\"static_commands_replayed\":%llu,"
        "\"output_updates\":%llu,\"texture_composites\":%llu,\"cached_texture_"
        "bytes\":%llu,\"context_updates\":%llu,\"render_captures\":%llu,"
        "\"retained_frames\":%llu},"
        "\"graph\":{\"geometry_compiles\":%llu,\"uploaded_bytes\":%llu,"
        "\"render_passes\":%llu,\"static_layer_updates\":%llu,"
        "\"static_partial_updates\":%llu,\"static_commands_replayed\":%llu,"
        "\"output_updates\":%llu,\"texture_composites\":%llu,\"cached_texture_"
        "bytes\":%llu,\"context_updates\":%llu,\"render_captures\":%llu,"
        "\"retained_frames\":%llu},"
        "\"ui_emissions\":%llu,\"telemetry_publications\":%llu,"
        "\"graph_cache\":{\"measurements\":%u,"
        "\"route_preparations\":%u,\"reachability_builds\":%u}}",
        (unsigned long long)host.geometry_compiles,
        (unsigned long long)host.uploaded_bytes,
        (unsigned long long)host.render_passes,
        (unsigned long long)host.static_layer_updates,
        (unsigned long long)host.static_partial_updates,
        (unsigned long long)host.static_commands_replayed,
        (unsigned long long)host.output_updates,
        (unsigned long long)host.texture_composites,
        (unsigned long long)host.cached_texture_bytes,
        (unsigned long long)host.context_updates,
        (unsigned long long)host.render_captures,
        (unsigned long long)host.retained_frames,
        (unsigned long long)graph.geometry_compiles,
        (unsigned long long)graph.uploaded_bytes,
        (unsigned long long)graph.render_passes,
        (unsigned long long)graph.static_layer_updates,
        (unsigned long long)graph.static_partial_updates,
        (unsigned long long)graph.static_commands_replayed,
        (unsigned long long)graph.output_updates,
        (unsigned long long)graph.texture_composites,
        (unsigned long long)graph.cached_texture_bytes,
        (unsigned long long)graph.context_updates,
        (unsigned long long)graph.render_captures,
        (unsigned long long)graph.retained_frames,
        (unsigned long long)s->ui_schedule.frames,
        (unsigned long long)s->perf_publications,
        cache ? cache->measurements : 0,
        cache ? cache->route_preparations : 0,
        cache ? cache->reachability_builds : 0
    );
    return json;
}
#endif

/* SDL keys forwarded to RmlUi; values are RmlUi 6.3 key identifiers. */
static rml_input_key input_key(SDL_Keycode key)
{
    if (key >= SDLK_A && key <= SDLK_Z) {
        return (rml_input_key)(RML_INPUT_KEY_A + (int)(key - SDLK_A));
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        return (rml_input_key)(RML_INPUT_KEY_0 + (int)(key - SDLK_0));
    }
    if (key >= SDLK_F1 && key <= SDLK_F12) {
        return (rml_input_key)(RML_INPUT_KEY_F1 + (int)(key - SDLK_F1));
    }
    switch (key) {
        case SDLK_RETURN: return RML_INPUT_KEY_ENTER;
        case SDLK_KP_ENTER: return RML_INPUT_KEY_NUMPAD_ENTER;
        case SDLK_ESCAPE: return RML_INPUT_KEY_ESCAPE;
        case SDLK_BACKSPACE: return RML_INPUT_KEY_BACKSPACE;
        case SDLK_TAB: return RML_INPUT_KEY_TAB;
        case SDLK_SPACE: return RML_INPUT_KEY_SPACE;
        case SDLK_LEFT: return RML_INPUT_KEY_LEFT;
        case SDLK_RIGHT: return RML_INPUT_KEY_RIGHT;
        case SDLK_UP: return RML_INPUT_KEY_UP;
        case SDLK_DOWN: return RML_INPUT_KEY_DOWN;
        case SDLK_HOME: return RML_INPUT_KEY_HOME;
        case SDLK_END: return RML_INPUT_KEY_END;
        case SDLK_PAGEUP: return RML_INPUT_KEY_PAGE_UP;
        case SDLK_PAGEDOWN: return RML_INPUT_KEY_PAGE_DOWN;
        case SDLK_DELETE: return RML_INPUT_KEY_DELETE;
        case SDLK_MINUS: return RML_INPUT_KEY_OEM_MINUS;
        case SDLK_PERIOD: return RML_INPUT_KEY_OEM_PERIOD;
        case SDLK_EQUALS: return RML_INPUT_KEY_OEM_PLUS;
        /* Workstream I: Settings (Mod+,) and Vim's Control+[ from the Text
           Editor. */
        case SDLK_COMMA: return RML_INPUT_KEY_OEM_COMMA;
        case SDLK_SEMICOLON: return RML_INPUT_KEY_OEM_1;
        case SDLK_SLASH: return RML_INPUT_KEY_OEM_2; /* Keyboard Shortcuts */
        case SDLK_LEFTBRACKET: return RML_INPUT_KEY_OEM_4;
        case SDLK_RIGHTBRACKET: return RML_INPUT_KEY_OEM_6;
        case SDLK_KP_MINUS: return RML_INPUT_KEY_SUBTRACT;
        case SDLK_LSHIFT: return RML_INPUT_KEY_LEFT_SHIFT;
        case SDLK_RSHIFT: return RML_INPUT_KEY_RIGHT_SHIFT;
        case SDLK_LCTRL: return RML_INPUT_KEY_LEFT_CONTROL;
        case SDLK_RCTRL: return RML_INPUT_KEY_RIGHT_CONTROL;
        case SDLK_LALT: return RML_INPUT_KEY_LEFT_ALT;
        case SDLK_RALT: return RML_INPUT_KEY_RIGHT_ALT;
        case SDLK_LGUI: return RML_INPUT_KEY_LEFT_META;
        case SDLK_RGUI: return RML_INPUT_KEY_RIGHT_META;
        default: return RML_INPUT_KEY_UNKNOWN;
    }
}

static unsigned rml_modifiers(SDL_Keymod mod);

/* On macOS, Command edits text (copy, paste, select all) where RmlUi expects
   Control, so key events trade the two there. Pointer events keep them. */
static unsigned rml_key_modifiers(const studio *s, SDL_Keymod mod)
{
    if (s->mac) {
        mod = (SDL_Keymod)((mod & ~(SDL_KMOD_CTRL | SDL_KMOD_GUI)) |
                           ((mod & SDL_KMOD_GUI) ? SDL_KMOD_CTRL : 0) |
                           ((mod & SDL_KMOD_CTRL) ? SDL_KMOD_GUI : 0));
    }
    return rml_modifiers(mod);
}

static unsigned rml_modifiers(SDL_Keymod mod)
{
    return ((mod & SDL_KMOD_CTRL) ? RML_INPUT_MOD_CONTROL : 0u) |
           ((mod & SDL_KMOD_SHIFT) ? RML_INPUT_MOD_SHIFT : 0u) |
           ((mod & SDL_KMOD_ALT) ? RML_INPUT_MOD_ALT : 0u) |
           ((mod & SDL_KMOD_GUI) ? RML_INPUT_MOD_META : 0u);
}

/* SDL mouse buttons are 1 left, 2 middle, 3 right; the kit uses 0 left, 1 right, 2 middle. */
static int kit_button_index(Uint8 button)
{
    return button == SDL_BUTTON_LEFT   ? 0
         : button == SDL_BUTTON_RIGHT  ? 1
         : button == SDL_BUTTON_MIDDLE ? 2
                                       : -1;
}

/* Records a key press without the primary modifier, which Studio's shortcuts
 * keep, or typed text, in order, as input for the game documents in the Scene
 * area. The text stays SDL's, valid until the next frame's events. */
static void game_input(studio *s, const rml_input *input, const SDL_Event *e)
{
    bool key = input->type == RML_INPUT_KEY_PRESS && !studio_primary(s, e->key.mod);
    bool paste = input->type == RML_INPUT_KEY_PRESS && e->type == SDL_EVENT_KEY_DOWN &&
                 e->key.key == SDLK_V && studio_primary(s, e->key.mod) &&
                 s->viewport && engine_viewport_ui_typing(s->viewport);
    if (key || paste || input->type == RML_INPUT_TEXT_INPUT) {
        apush(s->game_input, *input);
    }
}

/* Polled state loses a press and release within one frame; queue events instead. */
static bool SDLCALL queue_event(void *user, SDL_Event *event)
{
    studio *s = user;
    switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_PINCH_UPDATE:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_FOCUS_LOST: apush(s->events, *event); break;
        case SDL_EVENT_SYSTEM_THEME_CHANGED:
            s->ui_schedule.wake = true;
            s->system_light = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT;
            break;
        default: break;
    }
    return true;
}

bool studio_key_pressed(const studio *s, SDL_Keycode key)
{
    for (int i = 0; i < acount(s->keys); i++) {
        if (s->keys[i].key == key) {
            return true;
        }
    }
    return false;
}

float studio_pixel_ratio(const studio *s)
{
    return cf_app_get_pixel_scale() * s->prefs.ui_scale;
}

/* SDL reports window points; the editor lays out in dp, UI Scale apart. */
static float to_dp(const studio *s, float points)
{
    return points / s->prefs.ui_scale;
}

/* How long frames run at full rate after input, for the tooltips and
 * delayed motion it starts. */
#define ACTIVE_AFTER_INPUT 1.0

bool studio_active(const studio *s)
{
    return s->time < s->active_until || s->guest != NULL ||
           (app_scene_authoring_runtime_state(s->scene).playing && !s->pause.active);
}

/* No frame is needed while nothing moves: no input lately, no scene playing
 * (a paused one waits), and the last frame drew what the one before it did,
 * so any animation keeps frames coming without declaring itself. Timers
 * without motion, such as a toast's lifetime, wait for the host's next idle
 * frame. */
double studio_idle_time(studio *s)
{
    bool changed = rml_backend_take_changed(s->backend);
    if (s->graph.backend) {
        changed |= rml_backend_take_changed(s->graph.backend);
    }
    if (changed || studio_active(s) || studio_thumbs_pending(s)) {
        return 0;
    }
    double idle = fmin(kit_idle_time(s->kit), fmax(0, s->ui_schedule.next - s->time));
    return s->graph.kit ? fmin(idle, kit_idle_time(s->graph.kit)) : idle;
}

/* ---------- Studio in Studio ---------- */

/* The guest's window: the Scene area's surface, in the host's window points. */
static kit_rect guest_window(const studio *s)
{
    float ui = s->prefs.ui_scale;
    kit_rect r = s->surface.rect;
    return (kit_rect){ r.x * ui, r.y * ui, r.w * ui, r.h * ui };
}

static bool window_contains(kit_rect r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Keys follow the pointer, as Blender's areas take them, unless a host field
 * has the keyboard; a press in the guest keeps them there until a press
 * elsewhere. */
static bool guest_has_keys(const studio *s)
{
    return s->guest_keys || (s->guest_hover && !s->typing);
}

/* A pointer event at (x, y) in window points reaches the guest while it is
 * over the guest, uncovered by the host's popups, or while a press there is
 * held; the guest hears it in its own window's points. */
static void guest_pointer(studio *s, SDL_Event *moved, float *x, float *y)
{
    kit_rect w = guest_window(s);
    bool inside = window_contains(w, *x, *y) && s->surface.hovered;
    bool press = moved->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (press) {
        s->guest_keys = inside;
        s->guest_capture = inside;
    }
    bool reach = inside || s->guest_capture;
    if (moved->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        s->guest_capture = false;
    }
    if (!reach && s->guest_hover) {
        apush(s->guest->events, ((SDL_Event){ .type = SDL_EVENT_WINDOW_MOUSE_LEAVE }));
    }
    s->guest_hover = reach;
    if (!reach) {
        return;
    }
    *x -= w.x;
    *y -= w.y;
    apush(s->guest->events, *moved);
}

static bool guest_keys(studio *s, const SDL_Event *e)
{
    if (!guest_has_keys(s)) {
        return false;
    }
    apush(s->guest->events, *e);
    return true;
}

/* Hands the guest what is its; true when the host must not handle the event. */
static bool to_guest(studio *s, const SDL_Event *e)
{
    SDL_Event moved = *e;
    switch (e->type) {
        case SDL_EVENT_MOUSE_MOTION:
            guest_pointer(s, &moved, &moved.motion.x, &moved.motion.y);
            return false;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            guest_pointer(s, &moved, &moved.button.x, &moved.button.y);
            return false;
        case SDL_EVENT_MOUSE_WHEEL:
            guest_pointer(s, &moved, &moved.wheel.mouse_x, &moved.wheel.mouse_y);
            return false;
        case SDL_EVENT_PINCH_UPDATE:
            if (s->guest_hover) {
                apush(s->guest->events, *e);
            }
            return s->guest_hover;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            s->guest->mods = e->key.mod;
            return guest_keys(s, e);
        case SDL_EVENT_TEXT_INPUT: return guest_keys(s, e);
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (s->guest_hover) {
                apush(s->guest->events, *e);
            }
            s->guest_hover = false;
            return false;
        default: return false;
    }
}

/* Once the graph owns a pan, interior motion changes only its camera. Keep
 * the host surface's hover/focus intact; boundaries and every other event
 * still reach the host so capture, overlays and controls retain their lifecycle. */
static bool graph_pan_owns_motion(const studio *s, float x, float y)
{
    const studio_graph *g = &s->graph;
    kit_rect r = g->surface.rect;
    if (g->area < 0 || !studio_graph_active(s) || !g->captured ||
        g->gesture != STUDIO_GRAPH_PAN || !g->surface.hovered ||
        !(s->in.down[0] || s->in.down[1] || s->in.down[2]) || s->guest ||
        kit_wants_pointer(s->kit) || kit_wants_keyboard(s->kit) ||
        kit_has_popup(s->kit) || (g->kit && kit_has_popup(g->kit)) ||
        kit_dragging(s->kit, NULL) || studio_inspect_picking(s) || x < r.x ||
        y < r.y || x >= r.x + r.w || y >= r.y + r.h) {
        return false;
    }
    rml_context *context = rml_backend_context(s->backend);
    rml_element *hover = rml_context_hover_element(context);
    /* An authored overlay can appear without changing the native kit stack. */
    return hover && hover == rml_context_element_at_point(
                                 context,
                                 (rml_vec2){ x * s->in.dp_ratio, y * s->in.dp_ratio },
                                 NULL
                             );
}

/* The editor's documents hear a wheel as the distance it scrolls; the graph
   and the Scene count its notches. */
static void process_input(studio *s, rml_input input)
{
    if (input.type == RML_INPUT_POINTER_WHEEL) {
        float scale = cf_app_get_pixel_scale();
        input.wheel_x = feel_scroll_units(input.wheel_x, s->in.dp_ratio, scale);
        input.wheel_y = feel_scroll_units(input.wheel_y, s->in.dp_ratio, scale);
    }
    rml_backend_process_input(s->backend, &input);
}

/* A scroll gesture ends when its wheel events pause this long. */
#define SCROLL_GESTURE_GAP 0.3

/* Whether a wheel event belongs to a trackpad's two-finger scroll. SDL names
   neither device, but a mouse wheel turns whole notches on one axis, while a
   trackpad scrolls fractions of one and sideways too; once a gesture looks
   like a trackpad's, it stays one until it pauses. A high-resolution wheel
   turning fractions, or a tilted one, scrolls as a trackpad does. */
static bool trackpad_scroll(studio *s, const SDL_MouseWheelEvent *wheel)
{
    bool continues = s->time - s->wheel_at < SCROLL_GESTURE_GAP;
    bool fractional = wheel->x != 0 || wheel->y != roundf(wheel->y);
    s->wheel_trackpad = (continues && s->wheel_trackpad) || fractional;
    s->wheel_at = s->time;
    return s->wheel_trackpad;
}

static bool forward_input(studio *s)
{
    kit_frame_input *in = &s->in;
    if (acount(s->events)) {
        s->active_until = s->time + ACTIVE_AFTER_INPUT;
    }
    for (int b = 0; b < 3; b++) {
        in->pressed[b] = in->released[b] = false;
        in->press_position[b] = false;
    }
    in->wheel = s->zoom_wheel = s->scene_wheel = s->pinch = 0;
    s->scroll_x = s->scroll_y = 0;
    asetlen(s->keys, 0);
    asetlen(s->game_input, 0);
    s->nav_button = 0;
    bool host_input = false;
    bool recording = s->shortcut_recording != 0;
    for (int i = 0; i < acount(s->events); i++) {
        const SDL_Event *e = &s->events[i];
        bool graph_motion = e->type == SDL_EVENT_MOUSE_MOTION &&
                            graph_pan_owns_motion(
                                s,
                                to_dp(s, e->motion.x),
                                to_dp(s, e->motion.y)
                            );
        host_input |= !graph_motion;
        if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            s->shortcut_recording = 0;
            recording = false;
            s->mods = SDL_KMOD_NONE;
            /* A Space let go in another window neither plays nor pans. */
            s->space_down = s->scene_nav.space_play = s->scene_nav.space_drag = false;
            /* Nor do buttons and fly keys let go there keep the camera moving. */
            studio_scene_stop_camera(s);
            continue;
        }
        /* Space lifts as its key does, while a chord records too. */
        bool key = e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP;
        if (key && e->key.key == SDLK_SPACE) {
            s->space_down = e->type == SDL_EVENT_KEY_DOWN;
        }
        if (recording && (key || e->type == SDL_EVENT_TEXT_INPUT)) {
            if (e->type != SDL_EVENT_TEXT_INPUT) {
                s->mods = e->key.mod;
            }
            if (e->type == SDL_EVENT_KEY_DOWN) {
                studio_shortcut_record(
                    &s->prefs,
                    &s->shortcut_recording,
                    (studio_key){ studio_event_key(&e->key), e->key.mod, e->key.repeat },
                    s->mac
                );
            }
            continue;
        }
        if (s->guest && to_guest(s, e)) {
            continue;
        }
        /* The fly keys of a look around a 3D view reach no shortcut. */
        if (key && studio_scene_fly_key(s, &e->key)) {
            continue;
        }
        /* The Inspector's pick keys reach no document. */
        if (e->type == SDL_EVENT_KEY_DOWN && studio_inspect_key(s, &e->key)) {
            continue;
        }
        rml_input input = { .x = in->mouse_x,
                            .y = in->mouse_y,
                            .modifiers = rml_modifiers(s->mods) };
        switch (e->type) {
            case SDL_EVENT_MOUSE_MOTION:
                in->mouse_x = input.x = to_dp(s, e->motion.x);
                in->mouse_y = input.y = to_dp(s, e->motion.y);
                input.type = RML_INPUT_POINTER_MOVE;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int b = kit_button_index(e->button.button);
                bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (down && (e->button.button == SDL_BUTTON_X1 ||
                             e->button.button == SDL_BUTTON_X2)) {
                    s->nav_button = e->button.button == SDL_BUTTON_X1 ? -1 : 1;
                }
                if (b < 0) {
                    continue;
                }
                in->mouse_x = input.x = to_dp(s, e->button.x);
                in->mouse_y = input.y = to_dp(s, e->button.y);
                rml_input move = input;
                move.type = RML_INPUT_POINTER_MOVE;
                rml_backend_process_input(s->backend, &move);
                apush(s->graph.events, move);
                in->down[b] = down;
                if (down) {
                    in->press_x[b] = input.x;
                    in->press_y[b] = input.y;
                    in->press_position[b] = true;
                }
                in->pressed[b] |= down;
                in->released[b] |= !down;
                input.type = down ? RML_INPUT_POINTER_DOWN : RML_INPUT_POINTER_UP;
                input.button = b;
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {
                /* Zoom anchors where the wheel turned, which a motion event
                   may not have reported yet. */
                in->mouse_x = input.x = to_dp(s, e->wheel.mouse_x);
                in->mouse_y = input.y = to_dp(s, e->wheel.mouse_y);
                float notches = feel_zoom_notches(e->wheel.y);
                in->wheel += notches;
                /* The modifiers as the wheel turned: a key released later in
                   the same frame must not undo the gesture. */
                bool zooms = studio_primary(s, s->mods) || (s->mods & SDL_KMOD_CTRL);
                if (zooms) {
                    s->zoom_wheel += notches;
                }
                if (trackpad_scroll(s, &e->wheel) && !zooms) {
                    s->scroll_x += e->wheel.x;
                    s->scroll_y += e->wheel.y;
                } else {
                    s->scene_wheel += notches;
                }
                input.type = RML_INPUT_POINTER_WHEEL;
                input.wheel_x = -e->wheel.x;
                input.wheel_y = -e->wheel.y;
                break;
            }
            case SDL_EVENT_PINCH_UPDATE:
                /* The Scene view and the graph canvas zoom a pinch at the
                   pointer; scales multiply, so their logs add. */
                s->pinch += logf(e->pinch.scale);
                continue;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                input.type = RML_INPUT_POINTER_LEAVE;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                input.type = e->type == SDL_EVENT_KEY_DOWN ? RML_INPUT_KEY_PRESS
                                                           : RML_INPUT_KEY_RELEASE;
                input.key = input_key(e->key.key);
                s->mods = e->key.mod;
                input.modifiers = rml_key_modifiers(s, e->key.mod);
                if (e->type == SDL_EVENT_KEY_DOWN) {
                    apush(
                        s->keys,
                        ((studio_key){ studio_event_key(&e->key),
                                       e->key.mod,
                                       e->key.repeat })
                    );
                }
                if (input.key == RML_INPUT_KEY_UNKNOWN) {
                    continue;
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                input.type = RML_INPUT_TEXT_INPUT;
                input.modifiers = rml_key_modifiers(s, s->mods);
                input.text = e->text.text;
                input.text_size = strlen(e->text.text);
                break;
            default: continue;
        }
        if (!graph_motion) {
            process_input(s, input);
        }
        apush(s->graph.events, input);
        game_input(s, &input, e);
    }
    asetlen(s->events, 0);
    SDL_Keymod mod = s->mods;
    in->modifiers = ((mod & SDL_KMOD_CTRL) ? KIT_MOD_CTRL : 0u) |
                    ((mod & SDL_KMOD_SHIFT) ? KIT_MOD_SHIFT : 0u) |
                    ((mod & SDL_KMOD_ALT) ? KIT_MOD_ALT : 0u) |
                    ((mod & SDL_KMOD_GUI) ? KIT_MOD_META : 0u);
    /* Text input consumed by the watch must not reach Cute's gameplay buffer twice. */
    cf_input_text_clear();
    return host_input;
}

/* The cursor the element under the pointer asks for, the guest's over it. */
static const char *cursor_name(studio *s)
{
    if (s->guest && s->guest_hover) {
        return cursor_name(s->guest);
    }
    const char *graph = studio_graph_cursor(s);
    const char *scene = studio_scene_cursor(s);
    return kit_dragging(s->kit, NULL) ? "move"
         : graph                      ? graph
         : scene                      ? scene
                                      : rml_backend_cursor(s->backend);
}

/* RCSS `cursor` names to system cursors: resize on splitters and scrubbed
   fields, text in inputs, a crosshair in the color plane. */
static void sync_cursor(studio *s)
{
    static const struct
    {
        const char *name;
        SDL_SystemCursor cursor;
    } names[] = {
        { "text", SDL_SYSTEM_CURSOR_TEXT },
        { "ew-resize", SDL_SYSTEM_CURSOR_EW_RESIZE },
        { "col-resize", SDL_SYSTEM_CURSOR_EW_RESIZE },
        { "ns-resize", SDL_SYSTEM_CURSOR_NS_RESIZE },
        { "nwse-resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE },
        { "nesw-resize", SDL_SYSTEM_CURSOR_NESW_RESIZE },
        { "row-resize", SDL_SYSTEM_CURSOR_NS_RESIZE },
        { "crosshair", SDL_SYSTEM_CURSOR_CROSSHAIR },
        { "pointer", SDL_SYSTEM_CURSOR_POINTER },
        { "move", SDL_SYSTEM_CURSOR_MOVE },
        { "grab", SDL_SYSTEM_CURSOR_MOVE },
        { "not-allowed", SDL_SYSTEM_CURSOR_NOT_ALLOWED },
    };
    const char *name = cursor_name(s);
    SDL_SystemCursor wanted = SDL_SYSTEM_CURSOR_DEFAULT;
    for (size_t i = 0; name && i < sizeof names / sizeof *names; i++) {
        if (strcmp(names[i].name, name) == 0) {
            wanted = names[i].cursor;
        }
    }
    if ((int)wanted == s->cursor_shown) {
        return;
    }
    if (!s->cursors[wanted]) {
        s->cursors[wanted] = SDL_CreateSystemCursor(wanted);
    }
    SDL_SetCursor(s->cursors[wanted]);
    s->cursor_shown = (int)wanted;
}

/* Whether a text field has focus in the game documents that hear the Scene
 * area's keys: the Canvas preview's while it shows, else the scene's. */
static bool game_typing(const studio *s)
{
    const studio_canvas *c = studio_previewed_ui(s) ? s->canvas : NULL;
    engine_viewport *viewport = c ? c->viewport : s->viewport;
    return viewport && engine_viewport_ui_typing(viewport);
}

static bool accepts_text(studio *s)
{
    return kit_accepts_text(s->kit) || studio_graph_accepts_text(s) ||
           game_typing(s);
}

/* SDL delivers text only while text input is started; follow RmlUi keyboard
 * focus, the game documents' and a guest's with the keyboard included. */
static void sync_text_input(studio *s)
{
    kit_rect caret;
    bool wants = accepts_text(s) ||
                 (s->guest && guest_has_keys(s) && accepts_text(s->guest));
    if (wants != s->text_input) {
        s->text_input = wants;
        if (wants) {
            cf_input_enable_ime();
        } else {
            cf_input_disable_ime();
        }
    }
    if (kit_wants_text(s->kit, &caret)) {
        cf_input_set_ime_rect(
            (int)caret.x,
            (int)caret.y,
            (int)caret.w,
            (int)caret.h
        );
    }
}

static int area_showing(const studio *s, studio_editor editor)
{
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        if (s->prefs.layouts[s->prefs.workspace].editors[i] == editor &&
            s->areas[i].rect.w > 0) {
            return i;
        }
    }
    return -1;
}

static void draw_viewport(studio *s)
{
    /* The guest's own Scene area draws the scene, resizing its target: the
       texture this Studio showed last is gone by the time it shows the
       scene again, so it names none until it draws. */
    if (s->guest) {
        rml_backend_set_texture(s->backend, "viewport", 0);
        return;
    }
    int area = area_showing(s, STUDIO_EDITOR_SCENE);
    if (area < 0 || s->surface.rect.w <= 0 || s->surface.rect.h <= 0) {
        engine_viewport_set_host_input(s->viewport, NULL);
        return;
    }
    /* The Scene renders in window points, as games' UI scales with the view
       and not with the editor's UI Scale. */
    float ui = s->prefs.ui_scale;
    engine_viewport_host_input input = {
        .width = s->surface.rect.w * ui,
        .height = s->surface.rect.h * ui,
        .x = s->surface.x * ui,
        .y = s->surface.y * ui,
        .press_x = s->surface.press_x[0] * ui,
        .press_y = s->surface.press_y[0] * ui,
        .press_position = s->surface.press_position[0],
        .hovered = s->surface.hovered,
        .focused = s->focus_area == area && !kit_wants_keyboard(s->kit),
        .blocked = studio_palette_is_open(s),
        .wheel = s->surface.wheel,
        .alt = (s->mods & SDL_KMOD_ALT) != 0,
        .modifiers = rml_key_modifiers(s, s->mods),
        .escape = studio_key_pressed(s, SDLK_ESCAPE) && !kit_wants_keyboard(s->kit),
    };
    input.keys = s->game_input;
    input.key_count = acount(s->game_input);
    snprintf(input.dropped_prototype, sizeof input.dropped_prototype, "%s", s->dropped_prototype);
    s->dropped_prototype[0] = '\0';
    for (int b = 0; b < 3; b++) {
        input.down[b] = s->surface.down[b];
        input.pressed[b] = s->surface.pressed[b];
        input.released[b] = s->surface.released[b];
    }
    studio_scene_view(s, &input);
    /* An Outliner row the pointer rests on outlines its object, as the
       Scene's own hover does (Unity's and Godot's cross-highlight). */
    ecs_entity row = studio_outliner_hovered(s);
    if (row.slot) {
        s->scene->hovered = row;
    }
    if (studio_canvas_draw(s, &input)) {
        engine_viewport_set_host_input(s->viewport, NULL);
        return;
    }
    engine_viewport_set_host_input(s->viewport, &input);
    studio_scene_tools(s);
    app_scene_authoring_set_view_options(
        s->scene,
        (app_scene_view_options){ .generated_world = s->prefs.scene_world,
                                  .lighting = s->prefs.scene_lighting,
                                  .fog = s->prefs.scene_fog,
                                  .cameras = s->prefs.scene_cameras }
    );
    /* Escape leaves View Through Camera, as Blender's Numpad 0 again. */
    if (input.escape && (input.focused || input.hovered)) {
        app_scene_authoring_view_through_camera(s->scene, false);
    }
    engine_viewport_set_time(s->viewport, s->time);
    app_scene_authoring_draw_viewport(s->scene, s->viewport);
    rml_backend_set_texture(s->backend, "viewport", engine_viewport_texture(s->viewport));
    studio_ui_error(s, render_ui_error(engine_viewport_ui(s->viewport)));
}

/* Preferences apply by comparison with the last applied copy, like a zustand
 * selector, and save half a second after the last change. */
static void apply_prefs(studio *s, bool initial)
{
    const studio_prefs *p = &s->prefs;
    bool light = studio_theme_is_light(p->theme, s->system_light);
    if (s->theme_switching && !--s->theme_switching) {
        kit_set_body_class(s->kit, "theme-switching", false);
    }
    if (initial || p->theme != s->applied.theme || light != s->light) {
        /* next-themes: no transitions while the palette changes, and no flash
           of the wrong theme because this runs before the first frame. */
        s->light = light;
        kit_set_body_class(s->kit, "theme-switching", !initial);
        s->theme_switching = initial ? 0 : 1;
        studio_theme_apply(s->kit, p->theme, s->system_light);
    }
    if (initial || p->comfortable != s->applied.comfortable) {
        kit_set_body_class(s->kit, "comfortable", p->comfortable);
    }
    if (initial || p->reduced_motion != s->applied.reduced_motion) {
        kit_set_theme(s->kit, "reduced-motion", p->reduced_motion);
    }
    if (initial || p->standard_font_family != s->applied.standard_font_family ||
        p->standard_font_size != s->applied.standard_font_size ||
        p->code_font_family != s->applied.code_font_family ||
        p->code_font_size != s->applied.code_font_size) {
        studio_apply_font_settings(s->kit, p);
        if (s->graph.kit) {
            studio_apply_graph_font_settings(s->graph.kit, p);
        }
    }
    if (initial) {
        s->applied = *p;
        return;
    }
    if (s->collaboration.restoring_prefs) {
        s->applied = *p;
        s->collaboration.restoring_prefs = false;
        return;
    }
    if (!studio_prefs_equal(p, &s->applied)) {
        s->applied = *p;
        s->prefs_changed_at = s->time;
    }
    /* A guest's preferences are its own and last while it runs. */
    if (!s->hosted && s->prefs_changed_at > 0 && s->time - s->prefs_changed_at >= 0.5) {
        studio_prefs_save(p);
        s->prefs_changed_at = 0;
    }
}

/* The scenes' editor view state, hidden and locked objects, saves with each
 * change; a guest's changes are the host's to save. */
static void save_scene_view(studio *s)
{
    game_save *view = app_scene_authoring_view_store(s->scene);
    if (s->hosted || game_save_revision(view) == s->scene_view_saved) {
        return;
    }
    game_save_commit(view);
    s->scene_view_saved = game_save_revision(view);
}

/* With the Studio project open, its scene and previews are the editor's own UI,
 * so their settings are these preferences, as the editor's panels' are. */
static void share_project_prefs(studio *s)
{
    if (!s->own_project || s->hosted) {
        return;
    }
    game_preview_runtime *previews = game_prototype_preview_live(s->scene->prototype_preview)->runtime;
    studio_prefs_share(&s->prefs, s->scene->live->runtime->save, &s->scene_prefs_seen);
    studio_prefs_share(&s->prefs, previews->save, &s->preview_prefs_seen);
}

/* Studio's chrome is the Studio scene's page: the scene's root document's
 * RCSS styles it, and an edit to it or an import restyles the editor. */
static void sync_theme(studio *s)
{
    const char *name = s->ui ? studio_ui_page(s->ui) : "";
    if (!authoring_ui_page_sync(&s->page, resource_studio_content(), name, s->time) &&
        !s->page.error[0]) {
        return;
    }
    s->ui_schedule.wake = true;
    const char *rcss = s->page.document.rcss ? s->page.document.rcss : "";
    kit_theme(s->kit, rcss);
    if (s->graph.kit) {
        kit_theme(s->graph.kit, rcss);
    }
    if (s->page.error[0]) {
        fprintf(stderr, "Studio UI: %s\n", s->page.error);
        studio_error(s, "Studio UI", s->page.error);
        s->page.error[0] = '\0';
    }
}

/* A Studio over the scene authoring: the window's, or with `host` a guest
 * that starts from the host's preferences. */
static studio *create(
    app_scene_authoring *scene,
    engine_viewport *viewport,
    const char *project_root,
    const studio *host
)
{
    studio *s = xcalloc(1, sizeof *s);
    s->scene = scene;
    s->viewport = viewport;
    s->project_root = project_root;
    s->hosted = host != NULL;
    char error[256];
    /* A project that would not open shows empty Project settings. */
    chibi_project_open(&s->project, project_root, error, sizeof error);
    char studio_root[PATH_MAX];
    s->own_project = realpath(STUDIO_PROJECT, studio_root) &&
                     !strcmp(studio_root, project_root);
    s->backend = rml_backend_create(cf_app_get_width(), cf_app_get_height(), cf_app_get_pixel_scale());
    assert(s->backend);
    s->kit = kit_create(rml_backend_context(s->backend), "studio.rml");
    assert(s->kit);
    s->prefs = host ? host->prefs : studio_prefs_load();
    s->ui = studio_ui_new(s, STUDIO_PROJECT, error, sizeof error);
    sync_theme(s);
    if (!s->ui) {
        fprintf(stderr, "Studio UI: %s\n", error);
    }
    s->system_light = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT;
    s->mac = studio_detect_mac();
    apply_prefs(s, true);
    s->mods = SDL_GetModState();
    s->tab = STUDIO_TAB_OBJECT;
    s->assets_kind = -1;
    s->assets_hover = s->assets_rest = -1;
    s->cursor_shown = -1;
    s->hover_area = s->focus_area = s->key_area = s->maximized = -1;
    s->bound.behavior = -2;
    s->view.zoom = 1;
    s->view.x = s->view.y = 0.5f;
    s->graph.area = -1;
    s->output.shown = s->calls.selected = -1;
    s->in.mouse_x = s->in.mouse_y = -1e9f;
    return s;
}

studio *studio_new(app_scene_authoring *scene, engine_viewport *viewport, const char *project_root)
{
    studio *s = create(scene, viewport, project_root, NULL);
    editor_set_source_edit_handler(scene->graph_editor, studio_code_edit_source, s);
    /* Studio's own project runs Studio's behaviors, whose nodes act here. */
    scene->studio_nodes = (studio_node_service){ s, studio_node_run };
#ifdef __EMSCRIPTEN__
    web_studio = s;
    web_keep_chords();
#ifdef CHIBI_ENABLE_WEB_TRACE
    install_editor_counters();
#endif
#endif
    /* A guest opens no project: the window's Studio owns the workspace. */
    studio_projects_load(s);
    game_save *view = app_scene_authoring_view_store(scene);
    studio_prefs_open_scene_view(view);
    s->scene_view_saved = game_save_revision(view);
    s->in.mouse_x = cf_mouse_x();
    s->in.mouse_y = cf_mouse_y();
    SDL_AddEventWatch(queue_event, s);
    return s;
}

void studio_guest_start(studio *s)
{
    s->guest_request = 1;
}

void studio_guest_stop(studio *s)
{
    s->guest_request = -1;
}

/* Starts or stops the guest between frames, never while either one's
 * documents are being emitted or drawn. */
static void guest_requests(studio *s)
{
    int request = s->guest_request;
    s->guest_request = 0;
    if (request > 0 && !s->guest) {
        s->guest = create(s->scene, s->viewport, s->project_root, s);
        /* The host already reported the authoring's last message. */
        snprintf(s->guest->seen_message, sizeof s->guest->seen_message, "%s", s->seen_message);
        return;
    }
    if (request < 0 && s->guest) {
        rml_backend_set_texture(s->backend, "guest", 0);
        studio_free(s->guest);
        s->guest = NULL;
        s->guest_hover = s->guest_keys = s->guest_capture = false;
    }
}

/* What the window's Studio holds of the window: its event watch, IME,
   cursor and saved preferences. A guest holds none of it. */
static void release_window(studio *s)
{
    if (s->text_input) {
        cf_input_disable_ime();
    }
    SDL_RemoveEventWatch(queue_event, s);
    if (s->prefs_changed_at > 0) {
        studio_prefs_save(&s->prefs);
    }
    SDL_SetCursor(SDL_GetDefaultCursor());
    for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++) {
        if (s->cursors[i]) {
            SDL_DestroyCursor(s->cursors[i]);
        }
    }
    editor_set_retained_keyboard(false);
}

void studio_free(studio *s)
{
    if (!s) {
        return;
    }
#ifdef __EMSCRIPTEN__
    if (web_studio == s) {
        web_studio = NULL;
    }
#endif
    studio_free(s->guest);
    if (!s->hosted) {
        editor_set_source_edit_handler(s->scene->graph_editor, NULL, NULL);
        s->scene->studio_nodes = (studio_node_service){ 0 };
        release_window(s);
    }
    studio_export_free(s);
    free(s->tab_menu_key);
    afree(s->events);
    afree(s->bound.bindings);
    afree(s->records);
    afree(s->runtime_errors);
    afree(s->project_problems);
    afree(s->references);
    afree(s->calls.rows);
    afree(s->output.slots);
    afree(s->members.uses.items);
    rml_element_ref_destroy(s->members.list_element);
    studio_performance_free(s);
    studio_assets_free(s);
    studio_thumbs_free(s);
    afree(s->gallery.bin);
    afree(s->properties.folds);
    afree(s->keys);
    afree(s->game_input);
    studio_projects_free(s);
    chibi_project_free(&s->project);
    studio_disk_free(s);
    studio_refactor_free(s);
    studio_palette_free(s);
    studio_canvas_free(s);
    studio_add_menu_close(s);
    engine_viewport_set_host_input(s->viewport, NULL);
    studio_graph_free(s);
    component_presentation_free(&s->presentation.hints);
    studio_code_free(s);
    /* Visual designer (workstream H). */
    studio_designer_free(s);
    kit_destroy(s->kit);
    studio_ui_free(s->ui);
    /* After the panels' data, which shows the Inspector's strings. */
    studio_inspect_free(s->inspect);
    ui_page_free(&s->page);
    rml_backend_destroy(s->backend);
    free(s);
}

/* Runtime inspection is passive; simulation and authored behavior clocks are
 * independent. Input, edits and kit deadlines bypass this refresh interval. */
#define STUDIO_INSPECTION_INTERVAL (1.0 / 20.0)
#define STUDIO_IDLE_INSPECTION_INTERVAL 0.25

static uint64_t ui_state(studio *s)
{
    const app_scene_authoring *scene = s->scene;
    const editor *e = scene->graph_editor;
    const graph *g = e ? e->active_graph : NULL;
    const uint64_t values[] = {
        (uintptr_t)e,
        e ? editor_edit_revision(e) : 0,
        e && editor_has_pending_edits(e),
        (uintptr_t)g,
        g ? graph_topology_revision(g) : 0,
        e ? (uint64_t)acount(e->selected) : 0,
        e ? (uint64_t)acount(e->selected_links) : 0,
        scene->asset_index_generation,
        scene->asset_index_revision,
        (uintptr_t)scene->history,
        (uint64_t)scene->selected,
        (uint64_t)scene->selected_prototype,
        (uint64_t)scene->active_tab,
        scene->runtime_playing,
        scene->preview_playing,
        scene->property_dirty,
        scene->edit_dirty,
        scene->scene_dirty,
        studio_palette_is_open(s),
        (uint64_t)s->hover_area,
        (uint64_t)s->focus_area,
        (uint64_t)acount(s->records),
        (uint64_t)acount(s->runtime_errors),
        (uint64_t)acount(scene->actors),
        studio_designer_stamp(s),
        (uint64_t)s->tab,
        (uint64_t)s->maximized,
        s->pause.active,
        s->system_light,
        s->shortcut_recording,
        s->rename.entity,
        s->rename.outliner,
        s->rename.focus,
        s->graph.graph_id,
        (uint64_t)s->members.kind,
        s->members.id,
        (uint64_t)s->members.rename_kind,
        s->members.rename_id,
        s->graph.comment,
    };
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < (int)(sizeof values / sizeof *values); i++) {
        key = (key ^ values[i]) * 1099511628211ull;
    }
    const studio_record_entry *last = acount(s->records) ? &alast(s->records) : NULL;
    bool graph_motion = graph_pan_owns_motion(s, s->in.mouse_x, s->in.mouse_y);
    const double transient[] = { graph_motion ? -1e9 : s->in.mouse_x,
                                 graph_motion ? -1e9 : s->in.mouse_y,
                                 s->status_time,
                                 last ? last->time : 0 };
    const unsigned char *bytes = (const unsigned char *)transient;
    for (size_t i = 0; i < sizeof transient; i++) {
        key = (key ^ bytes[i]) * 1099511628211ull;
    }
    key = (key ^ (last ? last->kind : 0)) * 1099511628211ull;
    const char *texts[] = { s->status,
                            s->ui_error,
                            last ? last->text : "",
                            studio_palette_query(s) };
    for (int i = 0; i < (int)(sizeof texts / sizeof *texts); i++) {
        const unsigned char *p = (const unsigned char *)texts[i];
        do {
            key = (key ^ *p) * 1099511628211ull;
        } while (*p++);
    }
    if (!e) {
        return key;
    }
    for (int i = 0; i < acount(e->selected); i++) {
        key = (key ^ e->selected[i]->id) * 1099511628211ull;
    }
    for (int i = 0; i < acount(e->selected_links); i++) {
        const ed_link_ref *link = &e->selected_links[i];
        key = (key ^ link->from_id) * 1099511628211ull;
        key = (key ^ link->from_pin_id) * 1099511628211ull;
        key = (key ^ link->to_id) * 1099511628211ull;
        key = (key ^ link->to_pin_id) * 1099511628211ull;
    }
    return key;
}

static bool host_ui_due(studio *s, bool input, bool authored)
{
    uint64_t state = ui_state(s);
    /* A producer can enqueue a toast or restyle between emissions. Observe
     * immediate work now, but keep positive deadlines at their original time. */
    bool immediate = kit_idle_time(s->kit) <= 0;
    bool due = !s->ui_schedule.frames || input || authored || immediate ||
               s->ui_schedule.wake || s->theme_switching > 0 ||
               state != s->ui_schedule.state || s->time >= s->ui_schedule.next ||
               ((s->in.down[0] || s->in.down[1] || s->in.down[2]) &&
                !graph_pan_owns_motion(s, s->in.mouse_x, s->in.mouse_y)) ||
               s->in.width != s->ui_schedule.width ||
               s->in.height != s->ui_schedule.height ||
               s->in.dp_ratio != s->ui_schedule.ratio ||
               !studio_prefs_equal(&s->prefs, &s->applied) || studio_areas_moved(s);
    s->ui_schedule.state = state;
    if (due) {
        s->ui_schedule.wake = false;
        s->ui_schedule.width = s->in.width;
        s->ui_schedule.height = s->in.height;
        s->ui_schedule.ratio = s->in.dp_ratio;
    }
    return due;
}

/* The surface is measured during emission, before its new frame styles lay
   out. Reconcile that measurement on the next frame without waiting for input. */
static bool canvas_layout_changed(const studio *s)
{
    if (!studio_previewed_ui(s) || !s->canvas->frame) {
        return false;
    }
    rml_element *frame = s->canvas->frame;
    if (!rml_element_is_visible(frame, false)) {
        return false;
    }
    float ratio = studio_pixel_ratio(s);
    rml_vec2 at = rml_element_absolute_offset(frame, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(frame, RML_BOX_BORDER);
    kit_rect before = s->surface.rect;
    return fabsf(at.x / ratio - before.x) > 0.5f ||
           fabsf(at.y / ratio - before.y) > 0.5f ||
           fabsf(size.x / ratio - before.w) > 0.5f ||
           fabsf(size.y / ratio - before.h) > 0.5f;
}

/* A text field has the keyboard, and no popup, palette or modal dialog that
   keeps every key: Studio's chords that edit no text still run. */
static bool field_chords(studio *s)
{
    kit *graph = s->graph.kit;
    bool field = kit_input_focused(s->kit) || (graph && kit_input_focused(graph));
    return field && !s->shortcut_recording && !kit_has_popup(s->kit) &&
           !(graph && kit_has_popup(graph)) && !studio_palette_is_open(s) &&
           !studio_ui_modal_open(s);
}

/* One frame of a Studio in a window of `width` by `height` points: the
 * window's, or a guest's in its host's Scene area. */
static void run(studio *s, double delta_seconds, float width, float height)
{
    float scale = cf_app_get_pixel_scale(), ratio = studio_pixel_ratio(s);
    rml_backend_resize_pixels(s->backend, (int)lroundf(width * scale), (int)lroundf(height * scale), ratio);
    s->time += delta_seconds;
    s->in.time = s->time;
    s->in.dp_ratio = ratio;
    s->in.width = to_dp(s, width);
    s->in.height = to_dp(s, height);
    bool input = forward_input(s);
    studio_performance_sample(s, delta_seconds);
    if (!s->hosted) {
        app_scene_authoring_update(s->scene, s->in.down[0] || kit_wants_keyboard(s->kit));
    }
    sync_theme(s);
    bool authored = studio_ui_tick(s);
    share_project_prefs(s);
    studio_debug_frame(s);
    studio_export_poll(s);
    bool update_ui = host_ui_due(s, input, authored);
    /* The Scene and its tools read the surface every frame, but only an emit
       refreshes it: a click or a wheel turn reaches them in its own frame. */
    s->surface.wheel = 0;
    for (int b = 0; b < 3; b++) {
        s->surface.pressed[b] = s->surface.released[b] = false;
    }
    if (update_ui) {
        CHIBI_TRACE_ZONE_BEGIN(sync_zone, "Studio model sync");
        studio_ui_frame(s);
        CHIBI_TRACE_ZONE_END(sync_zone);
        CHIBI_TRACE_ZONE_BEGIN(emit_zone, "Studio host emit");
        kit_begin(s->kit, &s->in);
        if (s->canvas) {
            s->canvas->frame = NULL;
        }
        studio_emit(s);
        studio_inspect_emit(s);
        kit_end(s->kit);
        s->ui_schedule.frames++;
        CHIBI_TRACE_ZONE_END(emit_zone);
    }
    /* This frame's keys reached whatever had the keyboard as they arrived:
       a field that Escape or Enter just released still had them. */
    s->typed = s->typing;
    s->typing = kit_wants_keyboard(s->kit) || studio_graph_wants_keyboard(s);
    s->typed |= s->typing;
    bool toolbar_shortcuts = !studio_graph_wants_keyboard(s) &&
                             !s->shortcut_recording && studio_ui_shortcut_focus(s);
    if (!s->typed || toolbar_shortcuts) {
        studio_shortcuts(s, false);
    } else if (field_chords(s)) {
        studio_shortcuts(s, true);
    }
    studio_scene_space_release(s);
    bool prefs_dirty = s->theme_switching > 0 ||
                       !studio_prefs_equal(&s->prefs, &s->applied);
    apply_prefs(s, false);
    save_scene_view(s);
    CHIBI_TRACE_ZONE_BEGIN(viewport_zone, "Studio viewport");
    draw_viewport(s);
    CHIBI_TRACE_ZONE_END(viewport_zone);
    CHIBI_TRACE_ZONE_BEGIN(thumbs_zone, "Studio thumbnails");
    studio_thumbs_frame(s);
    CHIBI_TRACE_ZONE_END(thumbs_zone);
    if (s->guest) {
        kit_rect window = guest_window(s);
        run(s->guest, delta_seconds, fmaxf(window.w, 1), fmaxf(window.h, 1));
        rml_backend_set_texture_revision(
            s->backend,
            "guest",
            rml_backend_texture(s->guest->backend),
            rml_backend_texture_revision(s->guest->backend)
        );
    }
    studio_graph_frame(s, delta_seconds);
    CHIBI_TRACE_ZONE_BEGIN(frame_zone, "Studio host retained frame");
    rml_backend_frame_retained(s->backend, delta_seconds, update_ui || prefs_dirty);
    CHIBI_TRACE_ZONE_END(frame_zone);
    if (update_ui && canvas_layout_changed(s)) {
        s->ui_schedule.wake = true;
    }
    if (update_ui) {
        bool playing = app_scene_authoring_runtime_state(s->scene).playing;
        double refresh = playing ? STUDIO_INSPECTION_INTERVAL
                                 : STUDIO_IDLE_INSPECTION_INTERVAL;
        double delay = fmin(refresh, kit_idle_time(s->kit));
        s->ui_schedule.next = s->time + fmax(0, delay);
    }
    if (!s->hosted) {
        rml_backend_clear_pending_paste_text();
    }
}

void studio_frame(studio *s, double delta_seconds)
{
    guest_requests(s);
    run(s, delta_seconds, (float)cf_app_get_width(), (float)cf_app_get_height());
    editor_set_retained_keyboard(s->typing || (s->guest && s->guest->typing));
    sync_text_input(s);
    sync_cursor(s);
    rml_backend_present(s->backend);
}
