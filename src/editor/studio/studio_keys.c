/* Shortcuts follow the platform: Command is the primary modifier on macOS,
 * including the web build on a Mac, and Control elsewhere; chords are written
 * in each platform's notation (⇧⌘Z, Ctrl+Shift+Z). */
#include "editor/studio/studio_internal.h"

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* clang-format would space out a regular expression, so plain string tests. */
// clang-format off
EM_JS(bool, browser_is_mac, (), {
    const platform = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "").toLowerCase();
    return platform.includes("mac") || platform.includes("iphone") || platform.includes("ipad");
})
// clang-format on
#endif

bool studio_detect_mac()
{
#if defined(__EMSCRIPTEN__)
    return browser_is_mac();
#elif defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

SDL_Keycode studio_event_key(const SDL_KeyboardEvent *e)
{
    bool composed = e->key >= 0x80 && !(e->key & SDLK_SCANCODE_MASK);
    if ((e->mod & SDL_KMOD_ALT) && composed) {
        return SDL_GetKeyFromScancode(e->scancode, SDL_KMOD_NONE, true);
    }
    return e->key;
}

bool studio_primary(const studio *s, SDL_Keymod mod)
{
    return mod & (s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL);
}

/* A key as menus write it: arrows as glyphs, and a Mac's delete keys by the
   names on its keyboard. */
static const char *key_text(const char *key, bool mac)
{
    static const struct
    {
        const char *name, *mac, *other;
    } KEYS[] = {
        { "Up", "\xe2\x86\x91", "\xe2\x86\x91" },
        { "Down", "\xe2\x86\x93", "\xe2\x86\x93" },
        { "Left", "\xe2\x86\x90", "\xe2\x86\x90" },
        { "Right", "\xe2\x86\x92", "\xe2\x86\x92" },
        { "Backspace", "Delete", "Backspace" },
        { "Delete", "Fwd Delete", "Delete" },
        { "Return", "\xe2\x86\xa9", "Enter" },
        { "Escape", "Esc", "Esc" },
        { "Plus", "+", "Plus" },
        { "KeypadPlus", "Keypad +", "Keypad +" },
    };
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
        if (!strcmp(KEYS[i].name, key)) {
            return mac ? KEYS[i].mac : KEYS[i].other;
        }
    }
    return key;
}

/* `chord` joins Mod, Ctrl, Shift and Alt with a key by '+', as "Mod+Shift+Z". */
void studio_chord_format(char *out, size_t size, const char *chord, bool mac)
{
    static const struct
    {
        const char *name, *mac, *other;
        int mac_order;
    } MODIFIERS[] = {
        { "Ctrl", "\xe2\x8c\x83", "Ctrl", 0 },
        { "Alt", "\xe2\x8c\xa5", "Alt", 1 },
        { "Shift", "\xe2\x87\xa7", "Shift", 2 },
        { "Meta", "\xe2\x8c\x98", "Meta", 4 },
        { "Mod", "\xe2\x8c\x98", "Ctrl", 3 },
    };
    enum
    {
        MODIFIER_COUNT = sizeof MODIFIERS / sizeof *MODIFIERS
    };
    bool held[MODIFIER_COUNT] = { false };
    char key[32] = "";
    for (const char *at = chord; *at;) {
        const char *end = strchr(at, '+');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        bool modifier = false;
        for (int m = 0; m < MODIFIER_COUNT; m++) {
            if (strlen(MODIFIERS[m].name) == length &&
                !strncmp(at, MODIFIERS[m].name, length)) {
                held[m] = modifier = true;
            }
        }
        if (!modifier) {
            snprintf(key, sizeof key, "%.*s", (int)length, at);
        }
        at += length + (end != NULL);
    }
    char text[64] = "";
    size_t used = 0;
    /* macOS writes ⌃⌥⇧⌘ then the key; others write Ctrl+Alt+Shift+key. */
    for (int order = 0; order < MODIFIER_COUNT; order++) {
        for (int m = 0; m < MODIFIER_COUNT; m++) {
            int position = MODIFIERS[m].mac_order;
            if (!mac && !strcmp(MODIFIERS[m].name, "Mod")) {
                position = 0;
            }
            if (!held[m] || position != order) {
                continue;
            }
            used += (size_t)snprintf(
                text + used,
                sizeof text - used,
                mac ? "%s" : "%s+",
                mac ? MODIFIERS[m].mac : MODIFIERS[m].other
            );
        }
    }
    const char *shown = key_text(key, mac);
    bool word = strlen(shown) > 1 && (unsigned char)shown[0] < 0x80 && used && mac;
    snprintf(text + used, sizeof text - used, "%s%s", word ? " " : "", shown);
    size_t length = strlen(text);
    if (!key[0] && length && text[length - 1] == '+') {
        text[length - 1] = '\0';
    }
    snprintf(out, size, "%s", text);
}

const char *studio_chord(const studio *s, const char *chord)
{
    char text[STUDIO_CHORD_SIZE];
    studio_chord_format(text, sizeof text, chord, s->mac);
    return kit_string(s->kit, text);
}

/* Collapse left/right modifiers and ignore lock keys. */
static SDL_Keymod chord_modifiers(SDL_Keymod mod)
{
    return (mod & SDL_KMOD_CTRL ? SDL_KMOD_CTRL : 0) |
           (mod & SDL_KMOD_GUI ? SDL_KMOD_GUI : 0) |
           (mod & SDL_KMOD_ALT ? SDL_KMOD_ALT : 0) |
           (mod & SDL_KMOD_SHIFT ? SDL_KMOD_SHIFT : 0);
}

static bool modifier_key(SDL_Keycode key)
{
    return key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LGUI ||
           key == SDLK_RGUI || key == SDLK_LALT || key == SDLK_RALT ||
           key == SDLK_LSHIFT || key == SDLK_RSHIFT;
}

bool studio_chord_parse(const char *chord, bool mac, studio_key *out)
{
    if (!chord || !chord[0] || strlen(chord) >= STUDIO_CHORD_SIZE) {
        return false;
    }
    *out = (studio_key){ 0 };
    const char *at = chord;
    while (strchr(at, '+')) {
        const char *end = strchr(at, '+');
        size_t length = (size_t)(end - at);
        SDL_Keymod mod = 0;
        if (length == 3 && !strncmp(at, "Mod", length)) {
            mod = mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
        } else if (length == 4 && !strncmp(at, "Ctrl", length)) {
            mod = SDL_KMOD_CTRL;
        } else if (length == 4 && !strncmp(at, "Meta", length)) {
            mod = SDL_KMOD_GUI;
        } else if (length == 3 && !strncmp(at, "Alt", length)) {
            mod = SDL_KMOD_ALT;
        } else if (length == 5 && !strncmp(at, "Shift", length)) {
            mod = SDL_KMOD_SHIFT;
        } else {
            return false;
        }
        out->mod |= mod;
        at = end + 1;
    }
    out->key = !strcmp(at, "Plus")       ? SDLK_PLUS
             : !strcmp(at, "KeypadPlus") ? SDLK_KP_PLUS
                                         : SDL_GetKeyFromName(at);
    return out->key != SDLK_UNKNOWN && !modifier_key(out->key);
}

bool studio_chord_valid(const char *chord)
{
    studio_key key;
    return studio_chord_parse(chord, false, &key);
}

bool studio_chord_matches(const char *chord, SDL_Keycode key, SDL_Keymod mod, bool mac)
{
    studio_key binding;
    return studio_chord_parse(chord, mac, &binding) && binding.key == key &&
           binding.mod == chord_modifiers(mod);
}

/* Escape cancels, modifier presses and held-key repeats keep recording.
 * A completed recording is one preference edit. */
bool studio_shortcut_record(studio_prefs *prefs, int *recording, studio_key key, bool mac)
{
    if (!*recording) {
        return false;
    }
    if (key.key == SDLK_ESCAPE) {
        *recording = 0;
        return true;
    }
    if (key.repeat || modifier_key(key.key)) {
        return true;
    }
    const char *name = key.key == SDLK_PLUS    ? "Plus"
                     : key.key == SDLK_KP_PLUS ? "KeypadPlus"
                                               : SDL_GetKeyName(key.key);
    char chord[STUDIO_CHORD_SIZE];
    SDL_Keymod mod = chord_modifiers(key.mod);
    snprintf(
        chord,
        sizeof chord,
        "%s%s%s%s%s",
        mod & (mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL) ? "Mod+" : "",
        mod & (mac ? SDL_KMOD_CTRL : SDL_KMOD_GUI) ? (mac ? "Ctrl+" : "Meta+") : "",
        mod & SDL_KMOD_ALT ? "Alt+" : "",
        mod & SDL_KMOD_SHIFT ? "Shift+" : "",
        name
    );
    if (!studio_chord_valid(chord)) {
        return true;
    }
    int command = *recording - 1;
    snprintf(
        prefs->shortcuts[command],
        STUDIO_CHORD_SIZE,
        "%s",
        studio_chord_matches(studio_command_default_chord(command), key.key, key.mod, mac)
            ? ""
            : chord
    );
    *recording = 0;
    return true;
}
