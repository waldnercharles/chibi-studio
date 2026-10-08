#include "editor/workspace/viewport.h"

#include "platform/cute.h"
#include "support/array.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <imgui/dcimgui.h>
#include <stdint.h>

/* ImGui's modifiers as RmlUi's. On macOS Command edits text (select all,
   copy, paste) where RmlUi expects Control, so the two trade places. */
static unsigned ui_modifiers(const ImGuiIO *io)
{
#ifdef __APPLE__
    bool control = io->KeySuper, meta = io->KeyCtrl;
#else
    bool control = io->KeyCtrl, meta = io->KeySuper;
#endif
    return (control ? RML_INPUT_MOD_CONTROL : 0u) |
           (io->KeyShift ? RML_INPUT_MOD_SHIFT : 0u) |
           (io->KeyAlt ? RML_INPUT_MOD_ALT : 0u) | (meta ? RML_INPUT_MOD_META : 0u);
}

int engine_viewport_image(engine_viewport *viewport, float width, float height)
{
    if (engine_viewport_render(viewport, width, height) != 0) {
        return -1;
    }
    CF_Texture target = { engine_viewport_texture(viewport) };
    ImTextureRef texture = {
        ._TexData = NULL,
        ._TexID = (ImTextureID)(uintptr_t)cf_texture_handle(target),
    };
    ImGui_Image(texture, (ImVec2){ width, height });
    ImVec2 min = ImGui_GetItemRectMin();
    engine_viewport_set_image_origin(viewport, min.x, min.y);
    return 0;
}

void engine_viewport_ui_input(engine_viewport *viewport, bool active)
{
    ImGuiIO *io = ImGui_GetIO();
    engine_viewport_image_pointer(
        viewport,
        io->MousePos.x,
        io->MousePos.y,
        io->MouseClicked[0],
        io->MouseReleased[0],
        io->MouseWheel,
        ui_modifiers(io),
        active
    );
}

/* The keys game documents bind with data-keys or edit text with, besides
   letters and digits. */
static const struct
{
    ImGuiKey imgui;
    rml_input_key rml;
} UI_KEYS[] = {
    { ImGuiKey_Space, RML_INPUT_KEY_SPACE },
    { ImGuiKey_Enter, RML_INPUT_KEY_ENTER },
    { ImGuiKey_KeypadEnter, RML_INPUT_KEY_NUMPAD_ENTER },
    { ImGuiKey_Escape, RML_INPUT_KEY_ESCAPE },
    { ImGuiKey_Tab, RML_INPUT_KEY_TAB },
    { ImGuiKey_Backspace, RML_INPUT_KEY_BACKSPACE },
    { ImGuiKey_Delete, RML_INPUT_KEY_DELETE },
    { ImGuiKey_LeftArrow, RML_INPUT_KEY_LEFT },
    { ImGuiKey_RightArrow, RML_INPUT_KEY_RIGHT },
    { ImGuiKey_UpArrow, RML_INPUT_KEY_UP },
    { ImGuiKey_DownArrow, RML_INPUT_KEY_DOWN },
    { ImGuiKey_Home, RML_INPUT_KEY_HOME },
    { ImGuiKey_End, RML_INPUT_KEY_END },
    { ImGuiKey_PageUp, RML_INPUT_KEY_PAGE_UP },
    { ImGuiKey_PageDown, RML_INPUT_KEY_PAGE_DOWN },
};

static rml_input_key ui_key(ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return (rml_input_key)(RML_INPUT_KEY_A + (key - ImGuiKey_A));
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return (rml_input_key)(RML_INPUT_KEY_0 + (key - ImGuiKey_0));
    }
    for (size_t i = 0; i < sizeof UI_KEYS / sizeof *UI_KEYS; i++) {
        if (UI_KEYS[i].imgui == key) {
            return UI_KEYS[i].rml;
        }
    }
    return RML_INPUT_KEY_UNKNOWN;
}

/* Appends code point `c` to `text` as UTF-8. */
static void append_utf8(char **text, unsigned c)
{
    static const unsigned char LEAD[] = { 0x00, 0xC0, 0xE0, 0xF0 };
    int extra = c < 0x80 ? 0 : c < 0x800 ? 1 : c < 0x10000 ? 2 : 3;
    apush(*text, (char)(LEAD[extra] | c >> (6 * extra)));
    for (int i = extra - 1; i >= 0; i--) {
        apush(*text, (char)(0x80 | (c >> (6 * i) & 0x3F)));
    }
}

/* This frame's typed text, taken from Cute's buffer, as an array without a
   terminator; NULL when there is none. The buffer is read in order: its pop
   takes the last code point. */
static char *typed_text()
{
    char *text = NULL;
    CF_InputTextBuffer buffer;
    cf_input_text_get_buffer(&buffer);
    for (int i = 0; i < buffer.len; i++) {
        append_utf8(&text, (unsigned)buffer.codepoints[i]);
    }
    cf_input_text_clear();
    return text;
}

void engine_viewport_ui_keys(engine_viewport *viewport, bool active)
{
    assert(engine_viewport_ui(viewport));
    ImGuiIO *io = ImGui_GetIO();
    char *text = typed_text();
    unsigned modifiers = ui_modifiers(io);
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
        rml_input_key rml = ui_key(key);
        if (rml == RML_INPUT_KEY_UNKNOWN) {
            continue;
        }
        if (active && ImGui_IsKeyPressedEx(key, false)) {
            engine_viewport_ui_event(
                viewport,
                &(rml_input){ .type = RML_INPUT_KEY_PRESS, .key = rml, .modifiers = modifiers }
            );
        }
        if (ImGui_IsKeyReleased(key)) {
            engine_viewport_ui_event(
                viewport,
                &(rml_input){ .type = RML_INPUT_KEY_RELEASE, .key = rml, .modifiers = modifiers }
            );
        }
    }
    if (active && text) {
        engine_viewport_ui_event(
            viewport,
            &(rml_input){ .type = RML_INPUT_TEXT_INPUT,
                          .modifiers = modifiers,
                          .text = text,
                          .text_size = (size_t)acount(text) }
        );
    }
    afree(text);
    engine_viewport_ui_keyboard(viewport, active);
}
