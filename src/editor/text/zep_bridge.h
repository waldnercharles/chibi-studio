#ifndef CHIBI_EDITOR_TEXT_ZEP_BRIDGE_H
#define CHIBI_EDITOR_TEXT_ZEP_BRIDGE_H

/* A C binding for Zep (https://github.com/Rezonality/zep), the code editor
 * kit_code draws through RmlUi. It is pure pass-through, as src/ui/rml.h is:
 * zep_bridge.cpp only converts types and forwards calls, and its subclasses of
 * Zep's display, font, syntax and file system forward to the C callbacks in
 * zep_host. Input routing, drawing and policy live in C
 * (src/editor/rmlui/kit_code.c).
 *
 * One editor shows one buffer in one window. Positions are byte offsets into
 * the buffer's text, which keeps '\n' line ends only (Zep drops '\r').
 * Coordinates are pixels in the space zep_set_region names. Enum values match
 * Zep's, which zep_bridge.cpp checks. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct zep_editor zep_editor;

    /* Zep::ThemeColor, the roles the theme colors and syntax refer to. */
    typedef enum zep_color
    {
        ZEP_COLOR_NONE,
        ZEP_COLOR_TAB_BORDER,
        ZEP_COLOR_HIDDEN_TEXT,
        ZEP_COLOR_TEXT,
        ZEP_COLOR_TEXT_DIM,
        ZEP_COLOR_BACKGROUND,
        ZEP_COLOR_TAB_INACTIVE,
        ZEP_COLOR_TAB_ACTIVE,
        ZEP_COLOR_LINE_NUMBER_BACKGROUND,
        ZEP_COLOR_LINE_NUMBER,
        ZEP_COLOR_LINE_NUMBER_ACTIVE,
        ZEP_COLOR_CURSOR_NORMAL,
        ZEP_COLOR_CURSOR_INSERT,
        ZEP_COLOR_LIGHT,
        ZEP_COLOR_DARK,
        ZEP_COLOR_VISUAL_SELECT_BACKGROUND,
        ZEP_COLOR_CURSOR_LINE_BACKGROUND,
        ZEP_COLOR_AIRLINE_BACKGROUND,
        ZEP_COLOR_MODE,
        ZEP_COLOR_NORMAL,
        ZEP_COLOR_KEYWORD,
        ZEP_COLOR_IDENTIFIER,
        ZEP_COLOR_NUMBER,
        ZEP_COLOR_STRING,
        ZEP_COLOR_COMMENT,
        ZEP_COLOR_WHITESPACE,
        ZEP_COLOR_HIDDEN_CHAR,
        ZEP_COLOR_PARENTHESIS,
        ZEP_COLOR_ERROR,
        ZEP_COLOR_WARNING,
        ZEP_COLOR_INFO,
        ZEP_COLOR_WIDGET_BORDER,
        ZEP_COLOR_WIDGET_BACKGROUND,
        ZEP_COLOR_WIDGET_ACTIVE,
        ZEP_COLOR_WIDGET_INACTIVE,
        ZEP_COLOR_FLASH,
        ZEP_COLOR_CUSTOM,
        ZEP_COLOR_COUNT,
    } zep_color;

    /* Zep::ExtKeys: keys that are not characters. zep_press_key takes these or
     * an ASCII character. */
    typedef enum zep_key
    {
        ZEP_KEY_RETURN = 0,
        ZEP_KEY_ESCAPE = 1,
        ZEP_KEY_BACKSPACE = 2,
        ZEP_KEY_LEFT = 3,
        ZEP_KEY_RIGHT = 4,
        ZEP_KEY_UP = 5,
        ZEP_KEY_DOWN = 6,
        ZEP_KEY_TAB = 7,
        ZEP_KEY_DELETE = 8,
        ZEP_KEY_HOME = 9,
        ZEP_KEY_END = 10,
        ZEP_KEY_PAGE_DOWN = 11,
        ZEP_KEY_PAGE_UP = 12,
        ZEP_KEY_F1 = 13,
        ZEP_KEY_F12 = 24,
    } zep_key;

    /* Zep::ModifierKey bits. */
    enum
    {
        ZEP_MOD_CTRL = 1 << 0,
        ZEP_MOD_ALT = 1 << 1,
        ZEP_MOD_SHIFT = 1 << 2,
    };

    /* Zep::EditorMode. */
    typedef enum zep_mode
    {
        ZEP_MODE_NONE,
        ZEP_MODE_NORMAL,
        ZEP_MODE_INSERT,
        ZEP_MODE_VISUAL,
        ZEP_MODE_EX,
    } zep_mode;

    /* Zep::BufferMessageType, for zep_host.changed. */
    typedef enum zep_change
    {
        ZEP_CHANGE_BEFORE,
        ZEP_CHANGE_TEXT,    /* the range changed in place */
        ZEP_CHANGE_DELETED, /* the range was removed */
        ZEP_CHANGE_ADDED,   /* the range was inserted */
        ZEP_CHANGE_LOADED,
        ZEP_CHANGE_MARKERS,
    } zep_change;

    /* Zep::WindowFlags. */
    enum
    {
        ZEP_WINDOW_WHITESPACE = 1 << 0,
        ZEP_WINDOW_LINE_NUMBERS = 1 << 2,
        ZEP_WINDOW_INDICATORS = 1 << 3,
        ZEP_WINDOW_HIDE_SCROLLBAR = 1 << 4,
        ZEP_WINDOW_WRAP = 1 << 6,
    };

    /* Zep::RangeMarkerDisplayType bits. */
    enum
    {
        ZEP_MARK_UNDERLINE = 1 << 0,
        ZEP_MARK_BACKGROUND = 1 << 1,
        ZEP_MARK_TOOLTIP = 1 << 2,
        ZEP_MARK_TOOLTIP_AT_LINE = 1 << 3,
        ZEP_MARK_CURSOR_TIP = 1 << 4,
        ZEP_MARK_CURSOR_TIP_AT_LINE = 1 << 5,
        ZEP_MARK_INDICATOR = 1 << 6,
    };

    /* Zep::NVec4f: straight alpha, 0 to 1. */
    typedef struct zep_rgba
    {
        float r, g, b, a;
    } zep_rgba;

    /* What the editor calls back into. Every callback is required and runs
     * inside the zep_* call that caused it. */
    typedef struct zep_host
    {
        void *user;
        /* Display: draws in Zep's paint order. text is one glyph or more,
         * at the top-left of its cell. */
        void (*draw_text)(void *user, float x, float y, zep_rgba color, const char *text, size_t size);
        void (*draw_rect)(void *user, float x0, float y0, float x1, float y1, zep_rgba color);
        void (*draw_line)(void *user, float x0, float y0, float x1, float y1, zep_rgba color, float width);
        /* Later draws clip to the rectangle; an empty one clips nothing. */
        void (*clip)(void *user, float x0, float y0, float x1, float y1);
        /* The size of text[0, size) in the editor's face. */
        void (*text_size)(void *user, const char *text, size_t size, float *width, float *height);
        /* The buffer changed over [start, end). */
        void (*changed)(void *user, zep_change change, long start, long end);
        /* An ex command, as typed with its ':'; true when the host handled it
         * and Zep should not. */
        bool (*command)(void *user, const char *command);
        void (*set_clipboard)(void *user, const char *text);
        /* A malloc'd copy of the clipboard, or NULL; the bridge frees it. */
        char *(*get_clipboard)(void *user);
        /* True to draw the text at a byte in `color`; false draws it in
         * the text role. */
        bool (*syntax)(void *user, long byte, zep_rgba *color);
        /* Undo or redo was asked for, by a key or a command; true when the
         * host keeps the history and took it, false to replay Zep's own. */
        bool (*history)(void *user, bool redo);
    } zep_host;

    /* The host must outlive the editor. */
    zep_editor *zep_create(const zep_host *host);
    void zep_destroy(zep_editor *z);

    /* Text. zep_set_text keeps the history, which zep_clear_history drops. */
    void zep_set_text(zep_editor *z, const char *text);
    /* Copied as snprintf does: returns the full length and writes at most
     * size - 1 bytes and a NUL. */
    size_t zep_text(const zep_editor *z, char *buffer, size_t size);
    void zep_clear_history(zep_editor *z);

    /* Edits, each an undoable command. A group marker starts one undo step,
     * which runs to the next marker. */
    void zep_begin_undo_group(zep_editor *z);
    void zep_insert(zep_editor *z, long at, const char *text);
    void zep_delete(zep_editor *z, long start, long end);
    void zep_replace(zep_editor *z, long start, long end, const char *text);
    /* Replay Zep's own history, whatever zep_host.history says. */
    void zep_undo(zep_editor *z);
    void zep_redo(zep_editor *z);

    long zep_cursor(const zep_editor *z);
    void zep_set_cursor(zep_editor *z, long byte);
    /* The cursor's cell as last laid out: top-left and size in pixels. */
    void zep_cursor_rect(zep_editor *z, float *x, float *y, float *width, float *height);
    /* Selects from anchor to cursor in the mode's visual selection; an
     * empty range ends the selection and places the cursor. */
    void zep_select(zep_editor *z, long anchor, long cursor);
    /* The visual selection's bytes, [start, end); false without one. */
    bool zep_selection(const zep_editor *z, long *start, long *end);
    /* Keys go through Vim's mappings when true, the standard ones when false. */
    void zep_set_vim(zep_editor *z, bool vim);
    zep_mode zep_current_mode(const zep_editor *z);
    /* Shows text on the command line under the buffer. */
    void zep_set_command_text(zep_editor *z, const char *text);

    /* Input. key is a zep_key or an ASCII character; modifiers ZEP_MOD_*. */
    void zep_press_key(zep_editor *z, uint32_t key, unsigned modifiers);
    void zep_mouse_move(zep_editor *z, float x, float y);
    /* button: 0 left, 1 middle, 2 right. */
    void zep_mouse_down(zep_editor *z, float x, float y, int button);
    void zep_mouse_up(zep_editor *z, float x, float y, int button);
    /* distance in lines; positive scrolls up. */
    void zep_mouse_wheel(zep_editor *z, float x, float y, float distance);

    /* Display. */
    void zep_set_region(zep_editor *z, float x0, float y0, float x1, float y1);
    void zep_set_pixel_scale(zep_editor *z, float scale);
    /* The face's line height; the host measures glyphs at it. */
    void zep_set_font_height(zep_editor *z, int pixels);
    void zep_set_color(zep_editor *z, zep_color role, zep_rgba color);
    /* ZEP_WINDOW_* flags. */
    void zep_set_window_flags(zep_editor *z, unsigned flags);
    /* The cursor blinks while focused. */
    void zep_set_focused(zep_editor *z, bool focused);
    void zep_render(zep_editor *z);
    /* The byte whose glyph the pointer rested on as last rendered, or -1. */
    long zep_hover_byte(const zep_editor *z);
    /* Lines that do not wrap scroll left by `offset` pixels; the text region
     * spans left to right. */
    void zep_set_text_offset_x(zep_editor *z, float offset);
    void zep_text_region_x(const zep_editor *z, float *left, float *right);
    /* With `hide`, the command line and the status line under the text take
     * no room while the command line is empty, as outside an ex command. */
    void zep_hide_idle_command_line(zep_editor *z, bool hide);

    /* Marks over the text, as diagnostics: ZEP_MARK_* display bits, the role
     * that colors the mark and a message for its tooltip. */
    void zep_add_marker(
        zep_editor *z,
        long start,
        long end,
        unsigned display,
        zep_color color,
        const char *message
    );
    void zep_clear_markers(zep_editor *z);

#ifdef __cplusplus
}
#endif

#endif
