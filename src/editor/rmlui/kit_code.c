/* Code editor (kit_code.c), after VS Code's editor and Blender's Text Editor.
 * Zep (src/editor/text/zep_bridge.h) keeps the buffer, its undo history and
 * the Vim and standard keys; this module routes RmlUi's input to it and draws
 * its display through RmlUi.
 *
 * - Drawing: one kit-code element replays Zep's draw calls into batches in
 *   paint order. Rectangles and lines become one mesh per batch, kept while
 *   the next render builds the same one. Text becomes runs, glyphs of one
 *   color side by side on a line, drawn in JetBrains Mono by RmlUi's font
 *   engine; a run's mesh is cached by its text and color, so unchanged lines
 *   reuse theirs. A run no render uses is freed, and a change of the face's
 *   glyph textures clears them all.
 * - Colors: hidden swatch children take the theme's roles from
 *   theme/code.rcss, so Zep's palette follows the Studio theme, and one
 *   swatch per class the spans name gives their text its color.
 * - Input: the element takes keys, text, the pointer and focus, and stops
 *   each key so RmlUi's Tab navigation never sees it. Vim reads the physical
 *   Control key; the standard keys read the primary modifier (Command on a
 *   Mac). Chords the editor does not take go back to the host, as do undo
 *   and redo when the host keeps the history.
 * - Hover: once the pointer rests on a glyph, the host describes it; a card
 *   in the overlay layer shows that with the diagnostics of its line, so it
 *   never clips at the editor's edge.
 * - Clipboard: Cute's (SDL's) for native copy and paste. Browsers only expose
 *   pasted text during the DOM paste event, which the web build routes here. */
#include "editor/rmlui/kit_internal.h"
#include "editor/text/zep_bridge.h"
#include "editor/ui/kit_icons.h"
#include "support/math.h"
#include "support/text_edit.h"
#include "support/text_search.h"
#include "support/xalloc.h"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* The font provider keys families in lower case. */
#define CODE_FAMILY "jetbrains mono"
#define CODE_FONT_SIZE 13.0f
/* Spaces a run spans to join glyphs a blank separates. */
#define RUN_GAP_CELLS 16
#define NO_KEY UINT32_MAX
/* Seconds the pointer rests before the hover card, as the kit's tooltips. */
#define HOVER_DELAY FEEL_TOOLTIP_DELAY
/* Cells kept between the cursor and a side edge while lines scroll sideways. */
#define SCROLL_MARGIN_CELLS 4
/* Lines zep-wheel-scroll.patch scrolls for each unit it is handed. */
#define ZEP_WHEEL_LINES 3.0f

typedef struct text_run
{
    float x, y;
    rml_color color;
    char *text; /* array */
    int cells;
} text_run;

typedef struct batch
{
    bool text;
    bool clipped;
    float clip[4]; /* x0 y0 x1 y1 in the element's pixels */
    rml_vertex *vertices;
    int *indices;
    text_run *runs;
} batch;

/* A drawn mesh kept for the next render. */
typedef struct kept_mesh
{
    rml_vertex *vertices;
    int *indices;
    rml_geometry *geometry;
} kept_mesh;

typedef struct text_mesh
{
    uint64_t key;
    rml_text *mesh;
    uint64_t used; /* the render that last drew it */
} text_mesh;

/* A theme role read from a hidden child's color. */
static const struct
{
    const char *cls;
    zep_color roles[3]; /* ends at ZEP_COLOR_NONE */
} SWATCHES[] = {
    { "zep-text", { ZEP_COLOR_TEXT, ZEP_COLOR_NORMAL } },
    { "zep-background", { ZEP_COLOR_BACKGROUND } },
    { "zep-gutter", { ZEP_COLOR_LINE_NUMBER_BACKGROUND } },
    { "zep-line-number", { ZEP_COLOR_LINE_NUMBER, ZEP_COLOR_TEXT_DIM } },
    { "zep-line-number-active", { ZEP_COLOR_LINE_NUMBER_ACTIVE } },
    { "zep-cursor", { ZEP_COLOR_CURSOR_NORMAL } },
    { "zep-cursor-insert", { ZEP_COLOR_CURSOR_INSERT } },
    { "zep-selection", { ZEP_COLOR_VISUAL_SELECT_BACKGROUND } },
    { "zep-cursor-line", { ZEP_COLOR_CURSOR_LINE_BACKGROUND } },
    { "zep-status", { ZEP_COLOR_AIRLINE_BACKGROUND } },
    { "zep-mode", { ZEP_COLOR_MODE } },
    { "zep-whitespace", { ZEP_COLOR_WHITESPACE, ZEP_COLOR_HIDDEN_TEXT } },
    { "zep-scrollbar-track", { ZEP_COLOR_WIDGET_BACKGROUND } },
    { "zep-scrollbar", { ZEP_COLOR_WIDGET_INACTIVE } },
    { "zep-scrollbar-hover", { ZEP_COLOR_WIDGET_ACTIVE } },
    { "zep-tip-border", { ZEP_COLOR_WIDGET_BORDER } },
    { "zep-flash", { ZEP_COLOR_FLASH } },
    { "zep-info", { ZEP_COLOR_INFO } },
    { "tok-error", { ZEP_COLOR_ERROR } },
    { "tok-warning", { ZEP_COLOR_WARNING } },
};

#define SWATCH_COUNT (sizeof SWATCHES / sizeof *SWATCHES)

/* The spans of one class, colored as its hidden swatch is. */
typedef struct code_style
{
    char *cls;
    rml_element *swatch;
    zep_rgba color;
} code_style;

typedef struct code_span
{
    int start, end;
    int style; /* index into the view's styles */
} code_span;

/* The element's user data: what one kit-code element draws with. */
typedef struct code_view
{
    kit_code_editor *editor; /* NULL once its editor is gone */
    rml_element *el;
    rml_listener *listener;
    rml_element *swatches[SWATCH_COUNT];
    rml_color colors[SWATCH_COUNT];
    bool colored;
    code_style *styles;  /* array: each class spans named here */
    float width, height; /* the region Zep lays out in, pixels */
    bool rendered;
    bool dragging; /* the left button went down here and is held */
    long anchor;   /* where the press put the cursor */
    /* Drawing: Zep's clip, then what it drew, in order. */
    bool clipped;
    float clip[4];
    batch *batches;
    kept_mesh *kept;
    CF_MAP(text_mesh *) texts;
    uint64_t render;
    uintptr_t face;
    int face_version;
    unsigned font_generation;
} code_view;

typedef struct completion
{
    char *label, *insert, *detail;
    code_completion_kind kind;
} completion;

/* A diagnostic kept for the hover card. */
typedef struct mark
{
    int line;
    kit_code_severity severity;
    char *message;
} mark;

struct kit_code_editor
{
    zep_editor *zep;
    zep_host host;
    code_view *view;
    char *text; /* the buffer's text, NULL once a change makes it stale */
    size_t length;
    bool quiet; /* changes the caller made report no event */
    kit_code_result result;
    long cursor;
    bool focused;
    bool vim, mac;
    bool vim_go;
    /* The face at the size and pixel ratio last shown, and the font
       generation it is from (rml_font_generation). */
    int font_pixels;
    unsigned font_generation;
    char font_family[64];
    uintptr_t face;
    float ascent, line, advance;
    code_span *spans;   /* array */
    uint64_t span_hash; /* of the spans shown; 0 before the view maps them */
    uint64_t markers;   /* hash of the markers shown */
    mark *marks;        /* array: the markers shown */
    completion *completions; /* array */
    uint64_t completion_hash;
    int completion_start, selected;
    int shown_selected; /* the item last scrolled into view */
    bool host_history;  /* undo keys report to the host */
    bool no_wrap;
    float scroll_x; /* lines scrolled left, pixels */
    long followed;  /* the cursor last scrolled into view sideways */
    /* The pointer: where it last moved, in the context's pixels, and since
       when it rests there. A key or a press hides the card until it moves. */
    bool pointer_in, pointer_moved, hover_blocked;
    float pointer_x, pointer_y;
    double rest_since;
    int hover; /* the byte under the resting pointer, or -1 */
};

static kit_code_editor *focused_code_editor;

static const rml_event_id VIEW_EVENTS[] = {
    RML_EVENT_KEYDOWN,   RML_EVENT_KEYUP,   RML_EVENT_TEXTINPUT,
    RML_EVENT_MOUSEDOWN, RML_EVENT_MOUSEUP, RML_EVENT_MOUSEMOVE,
    RML_EVENT_DRAG,      RML_EVENT_DRAGEND, RML_EVENT_MOUSESCROLL,
    RML_EVENT_FOCUS,     RML_EVENT_BLUR,    RML_EVENT_DBLCLICK,
    RML_EVENT_MOUSEOUT,
};

#define VIEW_EVENT_COUNT (sizeof VIEW_EVENTS / sizeof *VIEW_EVENTS)

/* ---------- Web clipboard ---------- */

#ifdef __EMSCRIPTEN__
/* clang-format would space out the arrow functions. */
// clang-format off
EM_JS(void, web_listen_paste, (), {
    if (Module.chibiPasteListener) return;
    Module.chibiPasteListener = true;
    /* Without a field, SDL would take Mod+V and the browser would never
       paste: the key waits for the paste event's text, or a moment. */
    const pasteKey = (text, meta) => {
        Module.chibiKeyPaste = false;
        const pointer = text ? stringToNewUTF8(text) : 0;
        Module._studio_web_paste_key(pointer, meta ? 1 : 0);
        if (pointer) Module._free(pointer);
    };
    document.addEventListener('keydown', (event) => {
        if (document.activeElement !== Module.canvas) return;
        const primary = event.metaKey || event.ctrlKey;
        if (!primary || event.key.toLowerCase() !== 'v') {
            Module.chibiPasteMeta = false;
            return;
        }
        Module.chibiPasteMeta = event.metaKey;
        event.stopPropagation();
        if (Module._studio_web_wants_text()) return;
        Module.chibiKeyPaste = true;
        setTimeout(() => { if (Module.chibiKeyPaste) pasteKey(null, event.metaKey); }, 0);
    }, true);
    document.addEventListener('paste', (event) => {
        if (document.activeElement !== Module.canvas) return;
        const text = event.clipboardData?.getData('text/plain');
        if (Module.chibiKeyPaste) {
            event.preventDefault();
            pasteKey(typeof text === 'string' ? text : null, Module.chibiPasteMeta);
            return;
        }
        if (typeof text !== 'string' || !text.length) return;
        const pointer = stringToNewUTF8(text);
        try {
            if (Module._kit_code_web_paste(pointer)) {
                Module.chibiPasteMeta = false;
                event.preventDefault();
                return;
            }
            if (!Module._studio_web_wants_text()) return;
            const meta = Module.chibiPasteMeta || /Mac|iPhone|iPad/.test(navigator.platform);
            if (Module._studio_web_paste(pointer, meta ? 1 : 0)) {
                Module.chibiPasteMeta = false;
                event.preventDefault();
            }
        } finally {
            Module._free(pointer);
        }
    }, true);
})
EM_JS(void, web_copy, (const char *text), {
    if (navigator.clipboard) navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
})
// clang-format on
#endif

/* ---------- Colors and text ---------- */

static float unit(float v)
{
    return math_clamp(v, 0.0f, 1.0f);
}

static rml_color premultiplied(zep_rgba c)
{
    float a = unit(c.a);
    return (rml_color){ (uint8_t)lroundf(unit(c.r) * a * 255),
                        (uint8_t)lroundf(unit(c.g) * a * 255),
                        (uint8_t)lroundf(unit(c.b) * a * 255),
                        (uint8_t)lroundf(a * 255) };
}

static zep_rgba straight(rml_color c)
{
    return (zep_rgba){ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
}

static bool same_color(rml_color a, rml_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static int utf8_length(unsigned char lead)
{
    return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
}

static int codepoints(const char *text, size_t size)
{
    int count = 0;
    for (size_t i = 0; i < size; i++) {
        count += ((unsigned char)text[i] & 0xc0) != 0x80;
    }
    return count;
}

/* Byte offset where 1-based `line` starts, clamped to the text. */
static long line_start(const char *text, size_t length, int line)
{
    long at = 0;
    for (int l = 1; l < line && (size_t)at < length; l++) {
        const char *end = memchr(text + at, '\n', length - (size_t)at);
        if (!end) {
            return (long)length;
        }
        at = end - text + 1;
    }
    return at;
}

/* ---------- Batches ---------- */

/* The batch a draw of this kind joins: the last one, while the kind and
   Zep's clip stay the same. */
static batch *current_batch(code_view *v, bool text)
{
    if (acount(v->batches)) {
        batch *b = &alast(v->batches);
        bool same_clip = b->clipped == v->clipped &&
                         (!v->clipped || !memcmp(b->clip, v->clip, sizeof b->clip));
        if (b->text == text && same_clip) {
            return b;
        }
    }
    batch b = { .text = text, .clipped = v->clipped };
    memcpy(b.clip, v->clip, sizeof b.clip);
    apush(v->batches, b);
    return &alast(v->batches);
}

static void batches_free(code_view *v)
{
    for (int i = 0; i < acount(v->batches); i++) {
        batch *b = &v->batches[i];
        afree(b->vertices);
        afree(b->indices);
        for (int r = 0; r < acount(b->runs); r++) {
            afree(b->runs[r].text);
        }
        afree(b->runs);
    }
    afree(v->batches);
}

static void quad(batch *b, const float p[8], rml_color color)
{
    int base = acount(b->vertices);
    for (int i = 0; i < 4; i++) {
        apush(b->vertices, (rml_vertex){ { p[i * 2], p[i * 2 + 1] }, color, { 0, 0 } });
    }
    int order[] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        apush(b->indices, base + order[i]);
    }
}

/* ---------- Zep's display ---------- */

/* Zep draws only inside zep_render, which the view's render runs. */
static code_view *drawing(kit_code_editor *e)
{
    assert(e->view);
    return e->view;
}

static void on_clip(void *user, float x0, float y0, float x1, float y1)
{
    code_view *v = drawing(user);
    v->clipped = x1 > x0 && y1 > y0;
    memcpy(v->clip, (float[]){ x0, y0, x1, y1 }, sizeof v->clip);
}

static void on_draw_rect(void *user, float x0, float y0, float x1, float y1, zep_rgba color)
{
    rml_color c = premultiplied(color);
    if (c.a) {
        quad(current_batch(drawing(user), false), (float[]){ x0, y0, x1, y0, x1, y1, x0, y1 }, c);
    }
}

static void on_draw_line(void *user, float x0, float y0, float x1, float y1, zep_rgba color, float width)
{
    float dx = x1 - x0, dy = y1 - y0, length = hypotf(dx, dy);
    if (length <= 0) {
        return;
    }
    float nx = -dy / length * width / 2, ny = dx / length * width / 2;
    batch *b = current_batch(drawing(user), false);
    quad(
        b,
        (float[]){ x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny },
        premultiplied(color)
    );
}

/* Whole cells from the end of `run` to x, or -1 when x is not on the grid. */
static int cells_to(const kit_code_editor *e, const text_run *run, float x)
{
    float cells = (x - run->x) / e->advance - (float)run->cells;
    int whole = (int)lroundf(cells);
    return fabsf(cells - (float)whole) < 0.05f ? whole : -1;
}

static void on_draw_text(void *user, float x, float y, zep_rgba color, const char *text, size_t size)
{
    kit_code_editor *e = user;
    batch *b = current_batch(drawing(e), true);
    rml_color c = premultiplied(color);
    if (c.a == 0 || !size) {
        return;
    }
    text_run *run = acount(b->runs) ? &alast(b->runs) : NULL;
    int gap = run && run->y == y && same_color(run->color, c) ? cells_to(e, run, x)
                                                              : -1;
    if (gap < 0 || gap > RUN_GAP_CELLS) {
        apush(b->runs, (text_run){ x, y, c, NULL, 0 });
        run = &alast(b->runs);
        gap = 0;
    }
    for (int i = 0; i < gap; i++) {
        apush(run->text, ' ');
    }
    for (size_t i = 0; i < size; i++) {
        apush(run->text, text[i]);
    }
    run->cells += gap + codepoints(text, size);
}

/* The face at `pixels`, and its metrics. */
static void load_face(kit_code_editor *e, int pixels, const char *family)
{
    e->font_pixels = pixels;
    e->font_generation = rml_font_generation();
    if (family != e->font_family) {
        snprintf(e->font_family, sizeof e->font_family, "%s", family);
    }
    e->face = rml_font_face(e->font_family, RML_FONT_NORMAL, 400, pixels);
    assert(e->face);
    rml_font_metrics metrics = rml_font_face_metrics(e->face);
    e->ascent = roundf(metrics.ascent);
    e->line = ceilf(metrics.ascent + metrics.descent);
    e->advance = (float)rml_font_string_width_n(e->face, "M", 1);
}

/* The face, again after RmlUi released its font resources, as destroying a
   backend does: a handle from before is gone. 0 before the first frame. */
static uintptr_t face_of(kit_code_editor *e)
{
    if (e->face && e->font_generation != rml_font_generation()) {
        load_face(e, e->font_pixels, e->font_family);
    }
    return e->face;
}

static void on_text_size(void *user, const char *text, size_t size, float *width, float *height)
{
    kit_code_editor *e = user;
    /* Before the first frame chooses a face, a nominal monospace cell. */
    if (!face_of(e)) {
        *width = 8.0f * (float)codepoints(text, size);
        *height = 16;
        return;
    }
    *width = (float)rml_font_string_width_n(e->face, text, size);
    *height = e->line;
}

/* ---------- Zep's notifications ---------- */

static void on_changed(void *user, zep_change change, long, long)
{
    kit_code_editor *e = user;
    if (change == ZEP_CHANGE_BEFORE || change == ZEP_CHANGE_MARKERS) {
        return;
    }
    free(e->text);
    e->text = NULL;
    e->result.events |= e->quiet ? 0 : KIT_CODE_CHANGED;
    /* Zep drops a marker text is typed inside, and markers are lines. */
    e->markers = 0;
}

static bool command_is(const char *name, size_t length, const char *const *names)
{
    for (; *names; names++) {
        if (strlen(*names) == length && !strncmp(name, *names, length)) {
            return true;
        }
    }
    return false;
}

static void go_to_line(kit_code_editor *e, int line)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    zep_set_cursor(e->zep, line_start(text, length, line));
}

/* Ex commands: writes save through the host, a number goes to its line, and
   the ones that would open windows, buffers or files do nothing. */
static bool on_command(void *user, const char *command)
{
    static const char *const SAVE[] = { "w",   "write", "wq",     "x",
                                        "xit", "up",    "update", NULL };
    static const char *const QUIT[] = { "q", "q!", "quit", "close", "clo", NULL };
    static const char *const ZEP[] = { "reg", NULL };
    kit_code_editor *e = user;
    const char *name = command + (command[0] == ':');
    size_t length = strcspn(name, " ");
    if (command_is(name, length, SAVE)) {
        e->result.events |= KIT_CODE_SAVED;
        return true;
    }
    if (length && strspn(name, "0123456789") == length) {
        go_to_line(e, atoi(name));
        return true;
    }
    if (!strcmp(name, "$")) {
        go_to_line(e, INT32_MAX);
        return true;
    }
    if (command_is(name, length, ZEP)) {
        return false;
    }
    if (!command_is(name, length, QUIT)) {
        char message[96];
        snprintf(message, sizeof message, "Not an editor command: %.*s", (int)length, name);
        zep_set_command_text(e->zep, message);
    }
    return true;
}

void kit_clipboard_set(const char *text)
{
    cf_clipboard_set(text);
#ifdef __EMSCRIPTEN__
    web_copy(text);
#endif
}

static void on_set_clipboard(void *, const char *text)
{
    kit_clipboard_set(text);
}

static char *on_get_clipboard(void *)
{
    /* An empty clipboard leaves Zep its own registers, as without a system
       clipboard. */
    char *text = cf_clipboard_get();
    if (!text) {
        return NULL;
    }
    char *copy = text[0] ? xstrdup(text) : NULL;
    cf_free(text);
    return copy;
}

/* With the host keeping the history, undo and redo report to it. */
static bool on_history(void *user, bool redo)
{
    kit_code_editor *e = user;
    if (!e->host_history) {
        return false;
    }
    e->result.events |= redo ? KIT_CODE_REDO : KIT_CODE_UNDO;
    return true;
}

static bool on_syntax(void *user, long byte, zep_rgba *color)
{
    const kit_code_editor *e = user;
    int lo = 0, hi = acount(e->spans);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (e->spans[mid].start <= byte) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || byte >= e->spans[lo - 1].end) {
        return false;
    }
    *color = e->view->styles[e->spans[lo - 1].style].color;
    return true;
}

/* ---------- The editor ---------- */

kit_code_editor *kit_code_editor_create(const char *text)
{
    kit_code_editor *e = xcalloc(1, sizeof *e);
    e->host = (zep_host){
        .user = e,
        .draw_text = on_draw_text,
        .draw_rect = on_draw_rect,
        .draw_line = on_draw_line,
        .clip = on_clip,
        .text_size = on_text_size,
        .changed = on_changed,
        .command = on_command,
        .set_clipboard = on_set_clipboard,
        .get_clipboard = on_get_clipboard,
        .syntax = on_syntax,
        .history = on_history,
    };
    e->zep = zep_create(&e->host);
    zep_hide_idle_command_line(e->zep, true);
    /* Zep's scrollbar draws square step buttons; the wheel and keys scroll. */
    zep_set_window_flags(
        e->zep,
        ZEP_WINDOW_LINE_NUMBERS | ZEP_WINDOW_INDICATORS | ZEP_WINDOW_WRAP | ZEP_WINDOW_HIDE_SCROLLBAR
    );
    zep_set_vim(e->zep, false);
    e->result.completion = -1;
    e->hover = -1;
    e->shown_selected = -1;
    kit_code_set_text(e, text);
    return e;
}

static void completions_free(kit_code_editor *e)
{
    for (int i = 0; i < acount(e->completions); i++) {
        free(e->completions[i].label);
        free(e->completions[i].insert);
        free(e->completions[i].detail);
    }
    afree(e->completions);
}

static void marks_free(kit_code_editor *e)
{
    for (int i = 0; i < acount(e->marks); i++) {
        free(e->marks[i].message);
    }
    afree(e->marks);
}

void kit_code_editor_destroy(kit_code_editor *e)
{
    if (!e) {
        return;
    }
    if (focused_code_editor == e) {
        focused_code_editor = NULL;
    }
    if (e->view) {
        e->view->editor = NULL;
    }
    zep_destroy(e->zep);
    completions_free(e);
    marks_free(e);
    afree(e->spans);
    free(e->text);
    free(e);
}

void kit_code_set_text(kit_code_editor *e, const char *text)
{
    e->quiet = true;
    zep_set_text(e->zep, text ? text : "");
    e->quiet = false;
    zep_clear_history(e->zep);
    zep_set_cursor(e->zep, 0);
    e->cursor = 0;
}

const char *kit_code_text(kit_code_editor *e, size_t *length)
{
    if (!e->text) {
        e->length = zep_text(e->zep, NULL, 0);
        e->text = xmalloc(e->length + 1);
        zep_text(e->zep, e->text, e->length + 1);
    }
    if (length) {
        *length = e->length;
    }
    return e->text;
}

static int by_start_descending(const void *a, const void *b)
{
    const text_edit *x = *(const text_edit *const *)a,
                    *y = *(const text_edit *const *)b;
    return (x->start < y->start) - (x->start > y->start);
}

/* Where a byte ends up once the edits apply: moved by the edits before it,
   and to the end of the replacement of an edit it was inside. */
static long moved(long at, const text_edit *edits, int count)
{
    long shift = 0, inside = -1;
    for (int i = 0; i < count; i++) {
        const text_edit *t = &edits[i];
        long added = (long)strlen(t->text);
        if (t->end <= at && t->start < at) {
            shift += added - (t->end - t->start);
        } else if (t->start < at) {
            inside = t->start + added;
        }
    }
    return (inside >= 0 ? inside : at) + shift;
}

/* One undo step, applied from the end so earlier offsets hold. */
static void apply(kit_code_editor *e, const text_edit *edits, int count, long cursor)
{
    size_t length;
    kit_code_text(e, &length);
    const text_edit **order = xmalloc(sizeof *order * (size_t)count);
    for (int i = 0; i < count; i++) {
        assert(
            edits[i].start >= 0 && edits[i].start <= edits[i].end &&
            (size_t)edits[i].end <= length
        );
        order[i] = &edits[i];
    }
    qsort(order, (size_t)count, sizeof *order, by_start_descending);
    zep_begin_undo_group(e->zep);
    for (int i = 0; i < count; i++) {
        const text_edit *t = order[i];
        if (t->start == t->end) {
            zep_insert(e->zep, t->start, t->text);
        } else if (!t->text[0]) {
            zep_delete(e->zep, t->start, t->end);
        } else {
            zep_replace(e->zep, t->start, t->end, t->text);
        }
    }
    free(order);
    zep_set_cursor(e->zep, cursor);
}

void kit_code_apply(kit_code_editor *e, const text_edit *edits, int count)
{
    if (count <= 0) {
        return;
    }
    e->quiet = true;
    apply(e, edits, count, moved(zep_cursor(e->zep), edits, count));
    e->quiet = false;
    e->cursor = zep_cursor(e->zep);
}

static void note_cursor(kit_code_editor *e)
{
    long cursor = zep_cursor(e->zep);
    if (cursor != e->cursor) {
        e->cursor = cursor;
        e->result.events |= KIT_CODE_CURSOR;
    }
}

void kit_code_undo(kit_code_editor *e)
{
    zep_undo(e->zep);
    note_cursor(e);
}

void kit_code_redo(kit_code_editor *e)
{
    zep_redo(e->zep);
    note_cursor(e);
}

int kit_code_cursor(const kit_code_editor *e)
{
    return (int)zep_cursor(e->zep);
}

void kit_code_set_cursor(kit_code_editor *e, int byte)
{
    zep_set_cursor(e->zep, byte);
    e->cursor = zep_cursor(e->zep);
}

void kit_code_set_selection(kit_code_editor *e, int anchor, int cursor)
{
    if (e->vim) {
        if (cursor > anchor) {
            cursor--;
        } else if (cursor < anchor) {
            anchor--;
        }
    }
    zep_select(e->zep, anchor, cursor);
    e->cursor = zep_cursor(e->zep);
}

void kit_code_select(kit_code_editor *e, int start, int end)
{
    kit_code_set_selection(e, start, end);
}

bool kit_code_selection(const kit_code_editor *e, int *start, int *end)
{
    long low, high;
    if (!zep_selection(e->zep, &low, &high)) {
        return false;
    }
    *start = (int)low;
    *end = (int)high;
    return true;
}

int kit_code_hover(const kit_code_editor *e)
{
    return e->hover;
}

const char *kit_code_mode(const kit_code_editor *e)
{
    if (!e->vim) {
        return NULL;
    }
    switch (zep_current_mode(e->zep)) {
        case ZEP_MODE_INSERT: return "INSERT";
        case ZEP_MODE_VISUAL: return "VISUAL";
        case ZEP_MODE_EX: return "COMMAND";
        case ZEP_MODE_NONE:
        case ZEP_MODE_NORMAL: break;
    }
    return "NORMAL";
}

/* ---------- Keys ---------- */

static uint32_t special_key(rml_key key)
{
    if (key >= RML_KEY_F1 && key <= RML_KEY_F12) {
        return ZEP_KEY_F1 + (uint32_t)(key - RML_KEY_F1);
    }
    switch (key) {
        case RML_KEY_RETURN:
        case RML_KEY_NUMPADENTER: return ZEP_KEY_RETURN;
        case RML_KEY_ESCAPE: return ZEP_KEY_ESCAPE;
        case RML_KEY_BACK: return ZEP_KEY_BACKSPACE;
        case RML_KEY_TAB: return ZEP_KEY_TAB;
        case RML_KEY_LEFT: return ZEP_KEY_LEFT;
        case RML_KEY_RIGHT: return ZEP_KEY_RIGHT;
        case RML_KEY_UP: return ZEP_KEY_UP;
        case RML_KEY_DOWN: return ZEP_KEY_DOWN;
        case RML_KEY_DELETE: return ZEP_KEY_DELETE;
        case RML_KEY_HOME: return ZEP_KEY_HOME;
        case RML_KEY_END: return ZEP_KEY_END;
        case RML_KEY_PRIOR: return ZEP_KEY_PAGE_UP;
        case RML_KEY_NEXT: return ZEP_KEY_PAGE_DOWN;
        default: return NO_KEY;
    }
}

/* The character a chord names. */
static uint32_t chord_char(rml_key key)
{
    if (key >= RML_KEY_A && key <= RML_KEY_Z) {
        return 'a' + (uint32_t)(key - RML_KEY_A);
    }
    if (key >= RML_KEY_0 && key <= RML_KEY_9) {
        return '0' + (uint32_t)(key - RML_KEY_0);
    }
    switch ((int)key) {
        case RML_KEY_SPACE: return ' ';
        case RML_KEY_OEM_MINUS:
        case RML_KEY_SUBTRACT: return '-';
        case RML_KEY_OEM_PLUS: return '=';
        case RML_KEY_OEM_PERIOD: return '.';
        case RML_KEY_OEM_COMMA: return ',';
        case RML_KEY_OEM_2: return '/';
        case RML_KEY_OEM_1: return ';';
        case RML_KEY_OEM_4: return '[';
        case RML_KEY_OEM_6: return ']';
        default: return NO_KEY;
    }
}

/* Studio's own chords, which always go back to it: Save, Search, Settings,
   UI Scale and the Assets and Output drawers. */
static bool host_key(rml_key key, bool shift)
{
    switch ((int)key) {
        case RML_KEY_S:
        case RML_KEY_P:
        case RML_KEY_K:
        case RML_KEY_0:
        case RML_KEY_OEM_COMMA:
        case RML_KEY_OEM_PLUS:
        case RML_KEY_OEM_MINUS:
        case RML_KEY_SUBTRACT: return true;
        case RML_KEY_A:
        case RML_KEY_U: return shift;
        default: return false;
    }
}

/* What the standard keys edit with under the primary modifier. */
static bool standard_key(rml_key key)
{
    switch (key) {
        case RML_KEY_Z:
        case RML_KEY_Y:
        case RML_KEY_X:
        case RML_KEY_C:
        case RML_KEY_V:
        case RML_KEY_A:
        case RML_KEY_LEFT:
        case RML_KEY_RIGHT:
        case RML_KEY_BACK:
        case RML_KEY_DELETE:
        case RML_KEY_HOME:
        case RML_KEY_END: return true;
        default: return false;
    }
}

/* A key's modifiers as the person pressed them. */
typedef struct keys
{
    bool primary; /* Command on a Mac, Control elsewhere */
    bool ctrl;    /* the physical Control key */
    bool alt, shift;
} keys;

static keys read_keys(const kit_code_editor *e, unsigned modifiers)
{
    /* The host trades Control and Command on a Mac, so the primary modifier
       always reads as Control. */
    return (keys){
        .primary = modifiers & RML_KM_CTRL,
        .ctrl = modifiers & (e->mac ? RML_KM_META : RML_KM_CTRL),
        .alt = modifiers & RML_KM_ALT,
        .shift = modifiers & RML_KM_SHIFT,
    };
}

static unsigned zep_modifiers(const kit_code_editor *e, keys m)
{
    bool ctrl = e->vim ? m.ctrl : m.primary;
    return (ctrl ? ZEP_MOD_CTRL : 0u) | (m.alt ? ZEP_MOD_ALT : 0u) |
           (m.shift ? ZEP_MOD_SHIFT : 0u);
}

static void chord_name(char out[32], rml_key key, keys m)
{
    char name[8];
    uint32_t c = chord_char(key);
    if (c != NO_KEY && c != ' ') {
        snprintf(name, sizeof name, "%c", c >= 'a' && c <= 'z' ? c - 'a' + 'A' : (int)c);
    } else if (key >= RML_KEY_F1 && key <= RML_KEY_F12) {
        snprintf(name, sizeof name, "F%d", key - RML_KEY_F1 + 1);
    } else {
        snprintf(name, sizeof name, "%s", c == ' ' ? "Space" : "?");
    }
    snprintf(out, 32, "Mod+%s%s%s", m.alt ? "Alt+" : "", m.shift ? "Shift+" : "", name);
}

static void report_chord(kit_code_editor *e, rml_key key, keys m)
{
    chord_name(e->result.chord, key, m);
    e->result.events |= KIT_CODE_CHORD;
}

void kit_code_paste(kit_code_editor *e, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    size_t size = strlen(text);
    assert(size <= INT_MAX);
    char *normalized = xmalloc(size + 1);
    size_t length = 0;
    for (size_t i = 0; i < size; i++) {
        if (text[i] == '\r') {
            if (text[i + 1] == '\n') {
                i++;
            }
            normalized[length++] = '\n';
            continue;
        }
        normalized[length++] = text[i];
    }
    normalized[length] = '\0';
    if (!length) {
        free(normalized);
        return;
    }
    long start = zep_cursor(e->zep), end = start;
    bool selected = zep_selection(e->zep, &start, &end);
    if (selected && e->vim) {
        zep_press_key(e->zep, ZEP_KEY_ESCAPE, 0);
    }
    assert(start <= INT_MAX && end <= INT_MAX);
    text_edit edit = { (int)start, (int)end, normalized };
    apply(e, &edit, 1, start + (long)length);
    if (selected && !e->vim) {
        zep_select(e->zep, start + (long)length, start + (long)length);
    }
    free(normalized);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE int kit_code_web_paste(const char *text)
{
    if (!focused_code_editor || !focused_code_editor->focused) {
        return 0;
    }
    kit_code_paste(focused_code_editor, text);
    return 1;
}
#endif

static void paste(kit_code_editor *e)
{
    char *text = on_get_clipboard(e);
    if (!text) {
        return;
    }
    kit_code_paste(e, text);
    free(text);
}

/* Undo or redo: the host's history when it keeps one, else Zep's. */
static void undo_key(kit_code_editor *e, bool redo)
{
    if (on_history(e, redo)) {
        return;
    }
    if (redo) {
        zep_redo(e->zep);
    } else {
        zep_undo(e->zep);
    }
}

/* Command in Vim on a Mac: Z undoes, V pastes, and C and X copy and cut
   the selection (Vim's y and d, which write the clipboard), as in VS Code's
   Vim; the rest belong to the host. */
static void mac_command(kit_code_editor *e, rml_key key, keys m)
{
    bool selected = zep_current_mode(e->zep) == ZEP_MODE_VISUAL;
    switch (key) {
        case RML_KEY_Z: undo_key(e, m.shift); return;
        case RML_KEY_V: paste(e); return;
        case RML_KEY_C:
        case RML_KEY_X:
            if (selected) {
                zep_press_key(e->zep, key == RML_KEY_C ? 'y' : 'd', 0);
            }
            return;
        default: report_chord(e, key, m); return;
    }
}

/* The standard keys on a Mac move by words with Option and to the ends of a
   line with Command, as its text fields do. */
static uint32_t mac_motion(const kit_code_editor *e, rml_key key, keys *m)
{
    bool sideways = key == RML_KEY_LEFT || key == RML_KEY_RIGHT;
    if (e->vim || !e->mac || !sideways || !(m->alt || m->primary)) {
        return special_key(key);
    }
    if (m->primary) {
        m->primary = false;
        return key == RML_KEY_LEFT ? ZEP_KEY_HOME : ZEP_KEY_END;
    }
    m->alt = false;
    m->primary = true;
    return special_key(key);
}

/* Command+Up and Down on a Mac, Control+Home and End elsewhere, go to the
   start and the end of the text, as text fields do; -1 for other keys. */
static long document_edge(kit_code_editor *e, rml_key key, keys m)
{
    bool up = e->mac ? m.primary && key == RML_KEY_UP : m.ctrl && key == RML_KEY_HOME;
    bool down = e->mac ? m.primary && key == RML_KEY_DOWN : m.ctrl && key == RML_KEY_END;
    if (e->vim || m.shift || !(up || down)) {
        return -1;
    }
    size_t length;
    kit_code_text(e, &length);
    return up ? 0 : (long)length;
}

static void accept_completion(kit_code_editor *e, int index);

/* Up, Down, Enter, Tab and Escape choose among shown completions. */
static bool completion_key(kit_code_editor *e, rml_key key)
{
    int count = acount(e->completions);
    if (!count || !e->focused) {
        return false;
    }
    switch (key) {
        case RML_KEY_UP:
            e->selected = (e->selected + count - 1) % count;
            return true;
        case RML_KEY_DOWN: e->selected = (e->selected + 1) % count; return true;
        case RML_KEY_RETURN:
        case RML_KEY_NUMPADENTER:
        case RML_KEY_TAB: accept_completion(e, e->selected); return true;
        case RML_KEY_ESCAPE:
            completions_free(e);
            e->result.events |= KIT_CODE_DISMISSED;
            return true;
        default: return false;
    }
}

static bool modifier_key(rml_key key)
{
    return key == RML_KEY_UNKNOWN ||
           (key >= RML_KEY_LSHIFT && key <= RML_KEY_RMENU) ||
           key == RML_KEY_LMETA || key == RML_KEY_RMETA;
}

/* A key or a press hides the hover card until the pointer moves again,
   whatever moves came before it. */
static void block_hover(kit_code_editor *e)
{
    e->hover_blocked = true;
    e->pointer_moved = false;
}

/* Go to Definition (F12), Find References (Shift+F12) and completion
   (Control+Space) ask the host. */
static bool host_request(kit_code_editor *e, rml_key key, keys m)
{
    unsigned event = 0;
    if (e->vim && m.ctrl && !m.shift && !m.alt &&
        zep_current_mode(e->zep) == ZEP_MODE_NORMAL &&
        (key == RML_KEY_O || key == RML_KEY_I)) {
        e->result.events |= KIT_CODE_CHORD;
        snprintf(e->result.chord, sizeof e->result.chord, "%s", key == RML_KEY_O ? "Mod+[" : "Mod+]");
        return true;
    }
    if (key == RML_KEY_F2 && !m.primary && !m.alt && !m.shift) {
        event = KIT_CODE_RENAME;
    } else if (key == RML_KEY_H && m.shift && m.alt && !m.primary) {
        event = KIT_CODE_CALLS;
    } else if (key == RML_KEY_F12 && !m.primary && !m.alt) {
        event = m.shift ? KIT_CODE_REFERENCES : KIT_CODE_DEFINITION;
    } else if (key == RML_KEY_F9 && !m.primary && !m.alt && !m.shift) {
        event = KIT_CODE_BREAKPOINT;
    } else if ((key == RML_KEY_F5 || key == RML_KEY_F10 || key == RML_KEY_F11) && !m.primary && !m.alt) {
        e->result.events |= KIT_CODE_CHORD;
        snprintf(
            e->result.chord,
            sizeof e->result.chord,
            "%sF%d",
            m.shift ? "Shift+" : "",
            (int)(key - RML_KEY_F1) + 1
        );
        return true;
    } else if (key == RML_KEY_SPACE && m.ctrl && !m.shift && !m.alt) {
        event = KIT_CODE_SUGGEST;
    }
    e->result.events |= event;
    return event != 0;
}

/* Newlines inherit the current line's whitespace. Only code braces change
   nesting; quoted strings and comments must not affect indentation. */
static bool indent_space(char c)
{
    return c == ' ' || c == '\t';
}

static char indent_last(const char *text, int end)
{
    char quote = 0, last = 0;
    bool line = false, block = false;
    for (int i = 0; i < end; i++) {
        char c = text[i], next = text[i + 1];
        if (line) {
            if (c == '\n') {
                line = false;
                last = 0;
            }
            continue;
        }
        if (block) {
            if (c == '*' && next == '/') {
                block = false;
                i++;
            }
            continue;
        }
        if (quote) {
            if (c == '\\') {
                i++;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '/' && (next == '/' || next == '*')) {
            line = next == '/';
            block = next == '*';
            i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            last = c;
            continue;
        }
        if (c == '\n') {
            last = 0;
        } else if (!isspace((unsigned char)c)) {
            last = c;
        }
    }
    return quote || block ? 0 : last;
}

static char *line_indent(const char *text, int at, bool above, int *base)
{
    int begin = at;
    while (begin > 0 && text[begin - 1] != '\n') {
        begin--;
    }
    int end = begin;
    while (indent_space(text[end])) {
        end++;
    }
    *base = end - begin;
    char *indent = xmalloc((size_t)*base + 5);
    memcpy(indent, text + begin, (size_t)*base);
    int count = *base;
    if (!above && indent_last(text, at) == '{') {
        if (count && indent[count - 1] == '\t') {
            indent[count++] = '\t';
        } else {
            memcpy(indent + count, "    ", 4);
            count += 4;
        }
    }
    indent[count] = '\0';
    return indent;
}

static void typed_indent(kit_code_editor *e, const char *indent)
{
    if (!indent[0]) {
        return;
    }
    if (!e->vim) {
        long at = zep_cursor(e->zep);
        zep_insert(e->zep, at, indent);
        zep_set_cursor(e->zep, at + (long)strlen(indent));
        return;
    }
    for (const char *c = indent; *c; c++) {
        if (*c == '\t') {
            long at = zep_cursor(e->zep);
            zep_insert(e->zep, at, "\t");
            zep_set_cursor(e->zep, at + 1);
        } else {
            zep_press_key(e->zep, ' ', 0);
        }
    }
}

static void newline(kit_code_editor *e)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    long start = zep_cursor(e->zep), end = start;
    bool selected = zep_selection(e->zep, &start, &end);
    int base;
    char *indent = line_indent(text, (int)start, false, &base);
    int right = (int)end;
    while ((size_t)right < length && indent_space(text[right])) {
        right++;
    }
    bool pair = indent_last(text, (int)start) == '{' && text[right] == '}';
    int count = (int)strlen(indent);
    if (!pair && text[right] == '}' && count == base) {
        count -= count && indent[count - 1] == '\t' ? 1 : count < 4 ? count : 4;
        indent[count] = '\0';
    }
    if (selected || pair) {
        char *insert = xmalloc((size_t)count + (size_t)base + 3);
        insert[0] = '\n';
        memcpy(insert + 1, indent, (size_t)count);
        int total = count + 1;
        if (pair) {
            insert[total++] = '\n';
            memcpy(insert + total, indent, (size_t)base);
            total += base;
            end = right;
        }
        insert[total] = '\0';
        apply(e, &(text_edit){ (int)start, (int)end, insert }, 1, start + count + 1);
        zep_select(e->zep, start + count + 1, start + count + 1);
        free(insert);
    } else {
        zep_press_key(e->zep, ZEP_KEY_RETURN, 0);
        typed_indent(e, indent);
    }
    free(indent);
}

static void open_line(kit_code_editor *e, uint32_t key)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    int at = (int)zep_cursor(e->zep);
    if (key == 'o') {
        while ((size_t)at < length && text[at] != '\n') {
            at++;
        }
    }
    int base;
    char *indent = line_indent(text, at, key == 'O', &base);
    zep_press_key(e->zep, key, 0);
    if (zep_current_mode(e->zep) == ZEP_MODE_INSERT) {
        typed_indent(e, indent);
    }
    free(indent);
}

static void key_down(kit_code_editor *e, const rml_event *event)
{
    keys m = read_keys(e, event->modifiers);
    rml_key key = event->key;
    if (key == RML_KEY_ESCAPE) {
        e->vim_go = false;
    }
    if (modifier_key(key)) {
        return;
    }
    block_hover(e);
    /* Physical Control switches documents, including on a Mac, before Tab
       can accept a completion or change the text. */
    if (m.ctrl && !m.alt && key == RML_KEY_TAB) {
        snprintf(e->result.chord, sizeof e->result.chord, "Ctrl+%sTab", m.shift ? "Shift+" : "");
        e->result.events |= KIT_CODE_CHORD;
        return;
    }
    if (completion_key(e, key) || host_request(e, key, m)) {
        return;
    }
#ifdef __EMSCRIPTEN__
    /* Browser clipboard reads are only available in the paste event. The
       listener routes that text into this editor after the shortcut. */
    if (m.primary && key == RML_KEY_V) {
        return;
    }
#endif
    /* Vim takes Control+[ for Escape, as terminals send it. */
    if (e->vim && m.ctrl && (int)key == RML_KEY_OEM_4) {
        zep_press_key(e->zep, ZEP_KEY_ESCAPE, 0);
        return;
    }
    long edge = document_edge(e, key, m);
    if (edge >= 0) {
        zep_set_cursor(e->zep, edge);
        return;
    }
    if (m.primary && (host_key(key, m.shift) || (!e->vim && !standard_key(key)))) {
        report_chord(e, key, m);
        return;
    }
    if (m.primary && e->vim && e->mac) {
        mac_command(e, key, m);
        return;
    }
    uint32_t special = mac_motion(e, key, &m);
    if (special != NO_KEY) {
        if (special == ZEP_KEY_RETURN && !m.primary && !m.alt && !m.ctrl &&
            (!e->vim || zep_current_mode(e->zep) == ZEP_MODE_INSERT)) {
            newline(e);
            return;
        }
        zep_press_key(e->zep, special, zep_modifiers(e, m));
        return;
    }
    /* Characters arrive as text input, except under Control. Zep's Control+P
       opens a file search window of its own, which the editor has no room
       for. */
    uint32_t c = chord_char(key);
    if ((zep_modifiers(e, m) & ZEP_MOD_CTRL) && c != NO_KEY && c != 'p') {
        zep_press_key(e->zep, c, zep_modifiers(e, m));
    }
}

/* Zep reads one byte a key, so other characters go in as edits while the
   mode inserts. */
static void text_input(kit_code_editor *e, const char *text)
{
    /* Typing over a selection replaces it, as in a text field; Zep's
       standard mode would drop the keys. */
    long start, end;
    if (!e->vim && zep_selection(e->zep, &start, &end)) {
        apply(e, &(text_edit){ (int)start, (int)end, "" }, 1, start);
        zep_select(e->zep, start, start);
    }
    for (const char *c = text; *c;) {
        unsigned char lead = (unsigned char)*c;
        int length = utf8_length(lead);
        if (e->vim && zep_current_mode(e->zep) == ZEP_MODE_NORMAL) {
            if (e->vim_go) {
                e->vim_go = false;
                if (lead == 'd' || lead == 'r' || lead == 'R' || lead == 'c') {
                    zep_press_key(e->zep, ZEP_KEY_ESCAPE, 0);
                    e->result.events |= lead == 'd' ? KIT_CODE_DEFINITION
                                      : lead == 'r' ? KIT_CODE_REFERENCES
                                      : lead == 'R' ? KIT_CODE_RENAME
                                                    : KIT_CODE_CALLS;
                    c += length;
                    continue;
                }
                zep_press_key(e->zep, 'g', 0);
            } else if (lead == 'g') {
                e->vim_go = true;
                c += length;
                continue;
            }
        } else {
            e->vim_go = false;
        }
        if (lead >= 0x20 && lead < 0x7f) {
            if (e->vim && zep_current_mode(e->zep) == ZEP_MODE_NORMAL &&
                (lead == 'o' || lead == 'O')) {
                open_line(e, lead);
            } else {
                zep_press_key(e->zep, lead, 0);
            }
        } else if (lead >= 0x80 && zep_current_mode(e->zep) == ZEP_MODE_INSERT) {
            char glyph[5] = { 0 };
            memcpy(glyph, c, (size_t)length);
            long at = zep_cursor(e->zep);
            text_edit edit = { (int)at, (int)at, glyph };
            apply(e, &edit, 1, at + length);
        }
        c += length;
    }
}

static void event_text(kit_code_editor *e, const rml_event *event)
{
    size_t size = rml_event_text(event, NULL, 0) + 1;
    char *text = xmalloc(size);
    rml_event_text(event, text, size);
    text_input(e, text);
    free(text);
}

/* ---------- Pointer and focus ---------- */

static rml_vec2 local(const code_view *v, const rml_event *event)
{
    rml_vec2 origin = rml_element_absolute_offset(v->el, RML_BOX_CONTENT);
    return (rml_vec2){ event->mouse_x - origin.x, event->mouse_y - origin.y };
}

/* Zep finds the glyph under the pointer as it draws, so a press lays the
   editor out at the pointer first and drops what that pass drew. */
static void locate_pointer(code_view *v)
{
    if (!v->rendered) {
        return;
    }
    v->clipped = false;
    zep_render(v->editor->zep);
    batches_free(v);
}

/* RmlUi's buttons are 0 left, 1 right, 2 middle; Zep's 0 left, 1 middle,
   2 right. */
static int zep_button(int button)
{
    return button == 1 ? 2 : button == 2 ? 1 : button;
}

/* A press places the cursor and anchors a selection that dragging extends,
   as in a text field. */
static void press(code_view *v, rml_vec2 at, int button)
{
    zep_editor *z = v->editor->zep;
    zep_mouse_move(z, at.x, at.y);
    locate_pointer(v);
    zep_mouse_down(z, at.x, at.y, zep_button(button));
    v->dragging = button == 0;
    if (v->dragging) {
        v->anchor = zep_cursor(z);
        zep_select(z, v->anchor, v->anchor);
    }
}

/* Zep places the cursor on a press, so a drag presses again under the
   pointer and selects from the anchor to there. */
static void drag(code_view *v, rml_vec2 at)
{
    zep_editor *z = v->editor->zep;
    zep_mouse_move(z, at.x, at.y);
    if (!v->dragging) {
        return;
    }
    locate_pointer(v);
    zep_mouse_down(z, at.x, at.y, 0);
    zep_select(z, v->anchor, zep_cursor(z));
}

static bool word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/* A double click selects the word under the cursor, names with '-' in them
   whole, as RCSS and RML spell them. */
static void select_word(code_view *v)
{
    kit_code_editor *e = v->editor;
    size_t length;
    const char *text = kit_code_text(e, &length);
    long start = zep_cursor(e->zep), end = start;
    if ((size_t)start >= length || !word_char(text[start])) {
        return;
    }
    while (start > 0 && word_char(text[start - 1])) {
        start--;
    }
    while ((size_t)end < length && word_char(text[end])) {
        end++;
    }
    /* Vim's selection holds the glyph under its cursor; the standard keys'
       ends before it. */
    zep_select(e->zep, start, e->vim ? end - 1 : end);
    v->dragging = false;
}

/* A press with the primary modifier (Command on a Mac; pointer events keep
   the keys as pressed) goes to the definition under it. */
static bool primary_click(const kit_code_editor *e, const rml_event *event)
{
    return event->button == 0 &&
           (event->modifiers & (e->mac ? RML_KM_META : RML_KM_CTRL));
}

static float longest_line(kit_code_editor *e)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    int longest = 0;
    for (size_t at = 0; at < length;) {
        const char *end = memchr(text + at, '\n', length - at);
        size_t size = end ? (size_t)(end - text) - at : length - at;
        longest = math_max(longest, codepoints(text + at, size));
        at += size + 1;
    }
    return (float)longest * e->advance;
}

/* Lines that do not wrap scroll sideways, as far as the longest line and a
   margin reach. */
static void scroll_sideways(code_view *v, float cells)
{
    kit_code_editor *e = v->editor;
    if (!e->no_wrap || !v->rendered) {
        return;
    }
    float left, right;
    zep_text_region_x(e->zep, &left, &right);
    float most = longest_line(e) + SCROLL_MARGIN_CELLS * e->advance - (right - left);
    e->scroll_x = math_clamp(e->scroll_x + cells * e->advance, 0.0f, math_max(most, 0.0f));
    zep_set_text_offset_x(e->zep, e->scroll_x);
}

/* The wheel scrolls the distance it scrolls a list, in lines and cells. */
static void wheel(code_view *v, const rml_event *event, rml_vec2 at)
{
    kit_code_editor *e = v->editor;
    rml_vec2 delta = rml_event_wheel(event);
    float pixels = FEEL_RMLUI_SCROLL_DP *
                   rml_context_dp_ratio(rml_element_context(v->el));
    if (delta.y) {
        zep_mouse_wheel(e->zep, at.x, at.y, -delta.y * pixels / (e->line * ZEP_WHEEL_LINES));
    }
    if (delta.x) {
        scroll_sideways(v, delta.x * pixels / e->advance);
    }
}

/* Where the pointer is, for the hover card: it rests once it stops moving. */
static void track_pointer(kit_code_editor *e, const rml_event *event)
{
    bool moved = event->mouse_x != e->pointer_x || event->mouse_y != e->pointer_y;
    e->pointer_moved |= moved || !e->pointer_in;
    e->pointer_in = true;
    e->pointer_x = event->mouse_x;
    e->pointer_y = event->mouse_y;
}

static void on_view_event(void *user, const rml_event *event)
{
    code_view *v = user;
    kit_code_editor *e = v->editor;
    if (!e) {
        return;
    }
    rml_vec2 at = local(v, event);
    switch (event->id) {
        case RML_EVENT_KEYDOWN: key_down(e, event); break;
        case RML_EVENT_TEXTINPUT: event_text(e, event); break;
        case RML_EVENT_MOUSEDOWN:
            block_hover(e);
            press(v, at, event->button);
            if (primary_click(e, event)) {
                v->dragging = false;
                e->result.events |= KIT_CODE_DEFINITION;
            }
            break;
        case RML_EVENT_DBLCLICK: select_word(v); break;
        case RML_EVENT_MOUSEUP:
            v->dragging = false;
            zep_mouse_up(e->zep, at.x, at.y, zep_button(event->button));
            break;
        case RML_EVENT_DRAGEND: v->dragging = false; break;
        case RML_EVENT_MOUSEMOVE:
            track_pointer(e, event);
            zep_mouse_move(e->zep, at.x, at.y);
            break;
        case RML_EVENT_MOUSEOUT: e->pointer_in = event->target != v->el; break;
        case RML_EVENT_DRAG: drag(v, at); break;
        case RML_EVENT_MOUSESCROLL: wheel(v, event, at); break;
        case RML_EVENT_FOCUS:
        case RML_EVENT_BLUR:
            e->focused = event->id == RML_EVENT_FOCUS;
            if (e->focused) {
                focused_code_editor = e;
            } else if (focused_code_editor == e) {
                focused_code_editor = NULL;
            }
            zep_set_focused(e->zep, e->focused);
            break;
        default: break;
    }
    /* Keys and the wheel stop here, before RmlUi's Tab navigation and
       scrolling act on them. */
    switch (event->id) {
        case RML_EVENT_KEYDOWN:
        case RML_EVENT_KEYUP:
        case RML_EVENT_TEXTINPUT:
        case RML_EVENT_MOUSESCROLL: rml_event_stop_propagation(event); break;
        default: break;
    }
    note_cursor(e);
}

/* ---------- The element ---------- */

static void kept_free(code_view *v)
{
    for (int i = 0; i < acount(v->kept); i++) {
        afree(v->kept[i].vertices);
        afree(v->kept[i].indices);
        rml_geometry_destroy(v->kept[i].geometry);
    }
    afree(v->kept);
}

static void texts_clear(code_view *v)
{
    for (int i = 0; i < cf_map_size(v->texts); i++) {
        rml_text_destroy(v->texts[i]->mesh);
        free(v->texts[i]);
    }
    cf_map_clear(v->texts);
}

static void view_release(void *user)
{
    code_view *v = user;
    if (v->editor) {
        if (focused_code_editor == v->editor) {
            focused_code_editor = NULL;
        }
        v->editor->focused = false;
        zep_set_focused(v->editor->zep, false);
        v->editor->view = NULL;
    }
    for (size_t i = 0; i < VIEW_EVENT_COUNT; i++) {
        rml_element_remove_event_listener(v->el, VIEW_EVENTS[i], v->listener, false);
    }
    rml_listener_destroy(v->listener);
    for (int i = 0; i < acount(v->styles); i++) {
        free(v->styles[i].cls);
    }
    afree(v->styles);
    batches_free(v);
    kept_free(v);
    texts_clear(v);
    cf_map_free(v->texts);
    free(v);
}

/* The mesh for geometry batch `index`: last render's when unchanged. */
static rml_geometry *batch_geometry(code_view *v, rml_render_manager *manager, int index, batch *b)
{
    while (acount(v->kept) <= index) {
        apush(v->kept, (kept_mesh){ 0 });
    }
    kept_mesh *k = &v->kept[index];
    bool same = acount(k->vertices) == acount(b->vertices) &&
                acount(k->indices) == acount(b->indices) &&
                !memcmp(k->vertices, b->vertices, sizeof *k->vertices * (size_t)acount(b->vertices)) &&
                !memcmp(k->indices, b->indices, sizeof *k->indices * (size_t)acount(b->indices));
    if (same && k->geometry) {
        return k->geometry;
    }
    rml_geometry_destroy(k->geometry);
    afree(k->vertices);
    afree(k->indices);
    k->vertices = b->vertices;
    k->indices = b->indices;
    b->vertices = NULL;
    b->indices = NULL;
    k->geometry = rml_geometry_create(
        manager,
        (rml_mesh){ k->vertices,
                    (size_t)acount(k->vertices),
                    k->indices,
                    (size_t)acount(k->indices) }
    );
    return k->geometry;
}

static uint64_t run_key(const code_view *v, const text_run *run)
{
    uint64_t h = cf_fnv1a(&run->color, sizeof run->color);
    h ^= cf_fnv1a(run->text, acount(run->text)) * 31;
    return h ^ ((uint64_t)v->face << 1);
}

static rml_text *run_mesh(code_view *v, rml_render_manager *manager, const text_run *run)
{
    uint64_t key = run_key(v, run);
    text_mesh *t = cf_map_get(v->texts, key);
    if (!t) {
        t = xcalloc(1, sizeof *t);
        t->key = key;
        t->mesh = rml_text_create(manager, v->face, run->text, (size_t)acount(run->text), run->color);
        cf_map_set(v->texts, key, t);
    }
    t->used = v->render;
    return t->mesh;
}

/* Frees the runs this render did not draw. */
static void texts_prune(code_view *v)
{
    for (int i = cf_map_size(v->texts) - 1; i >= 0; i--) {
        text_mesh *t = v->texts[i];
        if (t->used != v->render) {
            rml_text_destroy(t->mesh);
            cf_map_del(v->texts, t->key);
            free(t);
        }
    }
}

static rml_rect intersect(rml_rect a, rml_rect b)
{
    int x0 = math_max(a.x, b.x), y0 = math_max(a.y, b.y);
    int x1 = math_min(a.x + a.width, b.x + b.width),
        y1 = math_min(a.y + a.height, b.y + b.height);
    return (rml_rect){ x0, y0, math_max(0, x1 - x0), math_max(0, y1 - y0) };
}

static rml_rect batch_scissor(const batch *b, rml_rect base, rml_vec2 origin)
{
    if (!b->clipped) {
        return base;
    }
    int x0 = (int)floorf(origin.x + b->clip[0]),
        y0 = (int)floorf(origin.y + b->clip[1]);
    int x1 = (int)ceilf(origin.x + b->clip[2]),
        y1 = (int)ceilf(origin.y + b->clip[3]);
    return intersect(base, (rml_rect){ x0, y0, x1 - x0, y1 - y0 });
}

static void draw_batches(code_view *v, rml_render_manager *manager, rml_vec2 origin, rml_rect base)
{
    const kit_code_editor *e = v->editor;
    int geometry = 0;
    for (int i = 0; i < acount(v->batches); i++) {
        batch *b = &v->batches[i];
        rml_rect scissor = batch_scissor(b, base, origin);
        rml_render_set_scissor(manager, &scissor);
        if (!b->text) {
            rml_geometry_render(batch_geometry(v, manager, geometry++, b), origin);
            continue;
        }
        for (int r = 0; r < acount(b->runs); r++) {
            const text_run *run = &b->runs[r];
            rml_vec2 at = { roundf(origin.x + run->x),
                            roundf(origin.y + run->y + e->ascent) };
            rml_text_render(run_mesh(v, manager, run), at);
        }
    }
}

/* The element's clip: its content box within the scissor RmlUi set. */
static rml_rect element_scissor(code_view *v, rml_render_manager *manager, rml_vec2 origin, rml_vec2 size)
{
    rml_rect own = { (int)floorf(origin.x),
                     (int)floorf(origin.y),
                     (int)ceilf(size.x),
                     (int)ceilf(size.y) };
    rml_rect current;
    return rml_render_scissor(manager, &current) ? intersect(current, own) : own;
}

/* Text meshes follow the face's glyph textures, which a new face or
   generation replaces. */
static void sync_face(code_view *v)
{
    uintptr_t face = face_of(v->editor);
    int version = rml_font_version(face);
    unsigned generation = rml_font_generation();
    if (face != v->face || version != v->face_version ||
        generation != v->font_generation) {
        texts_clear(v);
        v->face = face;
        v->face_version = version;
        v->font_generation = generation;
    }
}

/* Lines that do not wrap scroll sideways to keep a moved cursor in view,
   by its cell as last laid out. */
static void follow_cursor(code_view *v)
{
    kit_code_editor *e = v->editor;
    long cursor = zep_cursor(e->zep);
    if (!e->no_wrap || !v->rendered || cursor == e->followed) {
        return;
    }
    e->followed = cursor;
    float left, right, x, y, w, h;
    zep_text_region_x(e->zep, &left, &right);
    zep_cursor_rect(e->zep, &x, &y, &w, &h);
    float margin = math_min(SCROLL_MARGIN_CELLS * e->advance, (right - left) / 4);
    float cells = x < left + margin      ? (x - left - margin) / e->advance
                : x + w > right - margin ? (x + w - right + margin) / e->advance
                                         : 0;
    if (cells) {
        scroll_sideways(v, cells);
    }
}

static void view_render(rml_element *el, void *user)
{
    code_view *v = user;
    kit_code_editor *e = v->editor;
    rml_render_manager *manager = rml_element_render_manager(el);
    if (!e || !manager || !e->face) {
        return;
    }
    rml_vec2 origin = rml_element_absolute_offset(el, RML_BOX_CONTENT);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_CONTENT);
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    if (size.x != v->width || size.y != v->height) {
        v->width = size.x;
        v->height = size.y;
        zep_set_region(e->zep, 0, 0, size.x, size.y);
    }
    follow_cursor(v);
    sync_face(v);
    v->render++;
    v->clipped = false;
    zep_render(e->zep);
    v->rendered = true;
    rml_rect saved;
    bool had = rml_render_scissor(manager, &saved);
    draw_batches(v, manager, origin, element_scissor(v, manager, origin, size));
    rml_render_set_scissor(manager, had ? &saved : NULL);
    batches_free(v);
    texts_prune(v);
}

static const rml_element_hooks code_hooks = { view_render, NULL, view_release };

void register_code_element()
{
    static rml_instancer *instancer;
    if (!instancer) {
        instancer = rml_instancer_create(&code_hooks);
#ifdef __EMSCRIPTEN__
        web_listen_paste();
#endif
    }
    rml_register_element_instancer("kit-code", instancer);
}

static code_view *view_of(node *n)
{
    code_view *v = rml_element_user(n->el);
    if (v) {
        return v;
    }
    v = xcalloc(1, sizeof *v);
    v->el = n->el;
    v->listener = rml_listener_create(on_view_event, v);
    for (size_t i = 0; i < VIEW_EVENT_COUNT; i++) {
        rml_element_add_event_listener(n->el, VIEW_EVENTS[i], v->listener, false);
    }
    for (size_t i = 0; i < SWATCH_COUNT; i++) {
        v->swatches[i] = raw(n->el, "span", SWATCHES[i].cls, NULL);
    }
    rml_element_set_user(n->el, v);
    return v;
}

/* Shows the editor in this view, taking it from any other. */
static void attach(code_view *v, kit_code_editor *e)
{
    if (v->editor == e) {
        return;
    }
    if (v->editor) {
        if (focused_code_editor == v->editor) {
            focused_code_editor = NULL;
        }
        v->editor->focused = false;
        zep_set_focused(v->editor->zep, false);
        v->editor->view = NULL;
    }
    if (e->view) {
        if (focused_code_editor == e) {
            focused_code_editor = NULL;
        }
        e->focused = false;
        zep_set_focused(e->zep, false);
        e->view->editor = NULL;
    }
    v->editor = e;
    e->view = v;
    v->width = v->height = 0;
    v->colored = false;
    v->rendered = false;
    e->span_hash = 0;
    e->focused = rml_context_focus_element(rml_element_context(v->el)) == v->el;
    if (e->focused) {
        focused_code_editor = e;
    }
    zep_set_focused(e->zep, e->focused);
}

/* Zep's palette from the swatches, when the theme changed it. */
static void sync_colors(code_view *v)
{
    for (size_t i = 0; i < SWATCH_COUNT; i++) {
        rml_color c = rml_element_color(v->swatches[i]);
        if (v->colored && same_color(c, v->colors[i])) {
            continue;
        }
        v->colors[i] = c;
        for (const zep_color *role = SWATCHES[i].roles; *role; role++) {
            zep_set_color(v->editor->zep, *role, straight(c));
        }
    }
    for (int i = 0; i < acount(v->styles); i++) {
        v->styles[i].color = straight(rml_element_color(v->styles[i].swatch));
    }
    v->colored = true;
}

static void set_wrap(kit_code_editor *e, bool no_wrap)
{
    if (no_wrap == e->no_wrap) {
        return;
    }
    e->no_wrap = no_wrap;
    e->scroll_x = 0;
    e->followed = -1;
    zep_set_text_offset_x(e->zep, 0);
    zep_set_window_flags(
        e->zep,
        ZEP_WINDOW_LINE_NUMBERS | ZEP_WINDOW_INDICATORS |
            ZEP_WINDOW_HIDE_SCROLLBAR | (no_wrap ? 0u : (unsigned)ZEP_WINDOW_WRAP)
    );
}

static void configure(kit *k, kit_code_editor *e, const kit_code_desc *d)
{
    if (d->vim != e->vim) {
        e->vim = d->vim;
        zep_set_vim(e->zep, e->vim);
    }
    e->mac = d->mac;
    e->host_history = d->history;
    /* Zep's own history would only grow while the host keeps one. */
    if (e->host_history) {
        zep_clear_history(e->zep);
    }
    set_wrap(e, d->no_wrap);
    float ratio = dp(k);
    float size = d->font_size > 0 ? d->font_size : CODE_FONT_SIZE;
    const char *family = d->font_family && d->font_family[0] ? d->font_family
                                                             : CODE_FAMILY;
    /* RmlUi draws faces at whole pixel sizes. */
    int pixels = math_max(1, (int)(size * ratio));
    if (pixels == e->font_pixels && !strcmp(family, e->font_family)) {
        face_of(e);
        return;
    }
    load_face(e, pixels, family);
    zep_set_pixel_scale(e->zep, ratio);
    zep_set_font_height(e->zep, (int)e->line);
}

static uint64_t spans_hash(const kit_code_desc *d)
{
    uint64_t h = cf_fnv1a(&d->span_count, sizeof d->span_count);
    for (int i = 0; i < d->span_count; i++) {
        const kit_code_span *span = &d->spans[i];
        h = h * 31 + (uint64_t)span->start;
        h = h * 31 + (uint64_t)span->end;
        h = h * 31 + (span->cls ? str_hash(span->cls) : 0);
    }
    return h | 1;
}

/* The style of spans of class `cls`, its swatch added on first use; the
   swatch takes its color once the document next updates. */
static int style_of(code_view *v, const char *cls)
{
    for (int i = 0; i < acount(v->styles); i++) {
        if (!strcmp(v->styles[i].cls, cls)) {
            return i;
        }
    }
    code_style style = { .cls = xstrdup(cls), .swatch = raw(v->el, "span", cls, NULL) };
    style.color = straight(rml_element_color(style.swatch));
    apush(v->styles, style);
    return acount(v->styles) - 1;
}

static void highlight(code_view *v, kit_code_editor *e, const kit_code_desc *d)
{
    uint64_t hash = spans_hash(d);
    if (hash == e->span_hash) {
        return;
    }
    e->span_hash = hash;
    asetlen(e->spans, 0);
    for (int i = 0; i < d->span_count; i++) {
        const kit_code_span *span = &d->spans[i];
        if (span->cls) {
            apush(e->spans, ((code_span){ span->start, span->end, style_of(v, span->cls) }));
        }
    }
}

static uint64_t markers_hash(const kit_code_desc *d)
{
    uint64_t h = cf_fnv1a(&d->marker_count, sizeof d->marker_count);
    for (int i = 0; i < d->marker_count; i++) {
        const kit_code_marker *m = &d->markers[i];
        h = h * 31 + cf_fnv1a(&m->line, sizeof m->line);
        h = h * 31 + cf_fnv1a(&m->start, sizeof m->start);
        h = h * 31 + cf_fnv1a(&m->end, sizeof m->end);
        h = h * 31 + (uint64_t)m->severity;
        h = h * 31 + (uint64_t)m->indicator_only;
        h = h * 31 + (m->message ? str_hash(m->message) : 0);
    }
    return h;
}

static zep_color severity_role(kit_code_severity severity)
{
    switch (severity) {
        case KIT_CODE_ERROR: return ZEP_COLOR_ERROR;
        case KIT_CODE_WARNING: return ZEP_COLOR_WARNING;
        case KIT_CODE_INFO: return ZEP_COLOR_INFO;
    }
    return ZEP_COLOR_INFO;
}

/* The bytes a marker underlines: its range within the text, else its line. */
static void marked_range(const kit_code_marker *m, const char *text, size_t length, long *start, long *end)
{
    if (m->end > m->start && (size_t)m->start < length) {
        *start = m->start;
        *end = math_min((long)m->end, (long)length);
        return;
    }
    *start = line_start(text, length, m->line);
    *end = math_max(line_start(text, length, m->line + 1), *start + 1);
}

/* Diagnostics: a gutter mark and an underline over the range the caller
   knows, else the line; the hover card shows the message. */
static void diagnose(kit_code_editor *e, const kit_code_desc *d)
{
    uint64_t hash = markers_hash(d);
    if (hash == e->markers) {
        return;
    }
    e->markers = hash;
    zep_clear_markers(e->zep);
    marks_free(e);
    size_t length;
    const char *text = kit_code_text(e, &length);
    for (int i = 0; i < d->marker_count; i++) {
        const kit_code_marker *m = &d->markers[i];
        long start, end;
        marked_range(m, text, length, &start, &end);
        zep_add_marker(
            e->zep,
            start,
            end,
            ZEP_MARK_INDICATOR | (m->indicator_only ? 0 : ZEP_MARK_UNDERLINE),
            severity_role(m->severity),
            m->message ? m->message : ""
        );
        apush(e->marks, ((mark){ m->line, m->severity, xstrdup(m->message ? m->message : "") }));
    }
}

static void accept_completion(kit_code_editor *e, int index)
{
    const completion *c = &e->completions[index];
    long cursor = zep_cursor(e->zep);
    long start = math_clamp((long)e->completion_start, 0L, cursor);
    text_edit edit = { (int)start, (int)cursor, c->insert };
    apply(e, &edit, 1, start + (long)strlen(c->insert));
    completions_free(e);
    e->result.completion = index;
    e->result.events |= KIT_CODE_COMPLETED;
    note_cursor(e);
}

static uint64_t completions_hash(const kit_code_desc *d)
{
    uint64_t h = cf_fnv1a(&d->completion_count, sizeof d->completion_count);
    for (int i = 0; i < d->completion_count; i++) {
        const kit_code_completion *c = &d->completions[i];
        assert(c->label);
        h = h * 31 + str_hash(c->label);
        h = h * 31 + (c->insert ? str_hash(c->insert) : 0);
        h = h * 31 + (c->detail ? str_hash(c->detail) : 0);
        h = h * 31 + c->kind;
    }
    return h;
}

/* The caller's items, copied for the keys between frames; a new list selects
   its first item. */
static void take_completions(kit_code_editor *e, const kit_code_desc *d)
{
    uint64_t hash = completions_hash(d);
    e->completion_start = d->completion_start;
    if (hash == e->completion_hash) {
        return;
    }
    e->completion_hash = hash;
    completions_free(e);
    for (int i = 0; i < d->completion_count; i++) {
        const kit_code_completion *c = &d->completions[i];
        apush(
            e->completions,
            ((completion){
                .label = xstrdup(c->label),
                .insert = xstrdup(c->insert ? c->insert : c->label),
                .detail = xstrdup(c->detail ? c->detail : ""),
                .kind = c->kind,
            })
        );
    }
    e->selected = 0;
    e->shown_selected = -1;
}

/* Whether focus is in the completion list, as a press on an item puts it. */
static bool list_focused(kit *k, const char *key)
{
    node *list = path_node(k, frame_format(k, "layer/%s", key));
    rml_element *focus = rml_context_focus_element(k->context);
    return list && focus && within(focus, list->el);
}

static uint32_t completion_icon(code_completion_kind kind)
{
    switch (kind) {
        case CODE_COMPLETE_FIELD: return KIT_ICON_FIELD;
        case CODE_COMPLETE_VALUE: return KIT_ICON_CONSTANT;
        case CODE_COMPLETE_TYPE: return KIT_ICON_COMPONENT;
        case CODE_COMPLETE_FUNCTION: return KIT_ICON_FUNCTION;
        case CODE_COMPLETE_KEYWORD: return KIT_ICON_EXPRESSION;
        case CODE_COMPLETE_ASSET: return KIT_ICON_FILE;
        case CODE_COMPLETE_UNKNOWN: return KIT_ICON_VARIABLE;
    }
    assert(false);
    return 0;
}

static char *completion_query(kit_code_editor *e)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    int end = math_clamp((int)zep_cursor(e->zep), 0, (int)length);
    int start = math_clamp(e->completion_start, 0, end);
    char *query = xmalloc((size_t)(end - start) + 1);
    memcpy(query, text + start, (size_t)(end - start));
    query[end - start] = 0;
    return query;
}

/* The shared matcher supplies byte positions for prefix and fuzzy matches. */
static char *completion_markup(const char *label, const char *query)
{
    size_t length = strlen(label);
    bool *hits = xcalloc(length + 1, sizeof *hits);
    text_search_match(query, label, hits, length);
    char *markup = NULL;
    bool marked = false;
    for (size_t i = 0; i < length; i++) {
        if (hits[i] != marked) {
            str_cat(&markup, hits[i] ? "<b>" : "</b>");
            marked = hits[i];
        }
        if (label[i] == ' ') {
            str_cat(&markup, "\xc2\xa0");
        } else {
            escape(&markup, (char[]){ label[i], 0 });
        }
    }
    if (marked) {
        str_cat(&markup, "</b>");
    }
    free(hits);
    return markup ? markup : xstrdup("");
}

/* The list under the cursor, in the overlay layer. */
static void show_completions(kit *k, node *n, kit_code_editor *e)
{
    const char *key = frame_format(k, "@code:%s", n->path);
    if (!acount(e->completions) || !(e->focused || list_focused(k, key))) {
        return;
    }
    kit_rect caret = code_caret(k, n);
    node *list = emit_in(k, k->layer, key, KIND_BOX, "div", "code-completions");
    float height = fminf(7, acount(e->completions)) *
                       kit_metric(k, "--autocomplete-row-h") +
                   8;
    float below = k->in.height - 4 - caret.y - caret.h;
    float above = caret.y - 4;
    bool up = below < height && above > below;
    height = fminf(height, fmaxf(1, up ? above : below));
    float top = up ? caret.y - height : caret.y + caret.h;
    top = math_clamp(top, 4.0f, math_max(4.0f, k->in.height - 4 - height));
    kit_style(k, "left", frame_format(k, "%.1fdp", caret.x));
    kit_style(k, "top", frame_format(k, "%.1fdp", top));
    kit_style(k, "max-height", frame_format(k, "%.1fdp", height));
    kit_attr(k, "role", "listbox");
    push_node(k, list);
    char *query = completion_query(e);
    int accepted = -1;
    for (int i = 0; i < acount(e->completions); i++) {
        kit_push(k, frame_format(k, "i%d", i), NULL, "code-completion");
        kit_flag(k, "data-highlighted", i == e->selected);
        const completion *item = &e->completions[i];
        kit_attr(k, "role", "option");
        kit_attr(k, "aria-selected", i == e->selected ? "true" : "false");
        kit_icon(k, "icon", "lead", completion_icon(item->kind));
        char *markup = completion_markup(item->label, query);
        kit_markup(k, "label", "label", markup);
        free(markup);
        kit_label(k, "detail", "detail", item->detail);
        kit_flag(k, "data-empty", !item->detail[0]);
        kit_pop(k);
        accepted = kit_item_clicked(k) ? i : accepted;
    }
    free(query);
    pop_node(k);
    if (e->selected != e->shown_selected) {
        node *chosen = path_node(k, frame_format(k, "layer/%s/i%d", key, e->selected));
        if (chosen) {
            rml_element_scroll_into_view(chosen->el, RML_SCROLL_NEAREST, RML_SCROLL_NEAREST);
        }
        e->shown_selected = e->selected;
    }
    if (accepted >= 0) {
        accept_completion(e, accepted);
        rml_element_focus(n->el, false);
    }
}

/* The byte under the pointer once it has rested there, for the next frame;
   a key or a press hides it until the pointer moves. */
static void update_hover(kit *k, kit_code_editor *e)
{
    if (e->pointer_moved) {
        e->pointer_moved = false;
        e->hover_blocked = false;
        e->rest_since = k->in.time;
    }
    bool rested = e->pointer_in && !e->hover_blocked && e->view &&
                  e->view->rendered && k->in.time - e->rest_since >= HOVER_DELAY;
    e->hover = rested ? (int)zep_hover_byte(e->zep) : -1;
}

/* The 1-based line of a byte. */
static int line_of(kit_code_editor *e, int byte)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    int line = 1;
    for (int i = 0; i < byte && (size_t)i < length; i++) {
        line += text[i] == '\n';
    }
    return line;
}

static uint32_t severity_icon(kit_code_severity severity)
{
    switch (severity) {
        case KIT_CODE_ERROR: return KIT_ICON_ERROR;
        case KIT_CODE_WARNING: return KIT_ICON_WARNING;
        case KIT_CODE_INFO: return KIT_ICON_INFO;
    }
    return KIT_ICON_INFO;
}

static const char *severity_name(kit_code_severity severity)
{
    switch (severity) {
        case KIT_CODE_ERROR: return "error";
        case KIT_CODE_WARNING: return "warning";
        case KIT_CODE_INFO: return "info";
    }
    return "info";
}

static bool line_marked(const kit_code_editor *e, int line)
{
    for (int i = 0; i < acount(e->marks); i++) {
        if (e->marks[i].line == line) {
            return true;
        }
    }
    return false;
}

/* The card's rows: the diagnostics of the hovered line, then the host's
   title and detail lines. */
static void hover_rows(kit *k, const kit_code_editor *e, int line, const char *hover)
{
    for (int i = 0; i < acount(e->marks); i++) {
        const mark *m = &e->marks[i];
        if (m->line != line) {
            continue;
        }
        kit_push(k, frame_format(k, "m%d", i), NULL, "code-hover-problem");
        kit_attr(k, "data-severity", severity_name(m->severity));
        kit_icon(k, "icon", NULL, severity_icon(m->severity));
        kit_label(k, "message", "message", m->message);
        kit_pop(k);
    }
    for (int i = 0; hover && *hover; i++) {
        size_t size = strcspn(hover, "\n");
        const char *text = frame_format(k, "%.*s", (int)size, hover);
        kit_label(k, frame_format(k, "h%d", i), i == 0 ? "code-hover-title" : "code-hover-line", text);
        hover += size + (hover[size] == '\n');
    }
}

/* Under the pointer's line, or above it where the window ends; never past a
   side. */
static void place_hover(kit *k, const kit_code_editor *e, node *card)
{
    float s = dp(k), edge = 4;
    kit_rect size = element_rect(k, card->el);
    float x = e->pointer_x / s - 8, line = e->line / s;
    float y = e->pointer_y / s + line;
    if (y + size.h > k->in.height - edge) {
        y = e->pointer_y / s - line - size.h;
    }
    x = math_clamp(x, edge, math_max(edge, k->in.width - size.w - edge));
    kit_style(k, "left", frame_format(k, "%.1fdp", x));
    kit_style(k, "top", frame_format(k, "%.1fdp", math_max(edge, y)));
}

/* The card for the byte the caller described, while the pointer rests. */
static void show_hover(kit *k, node *n, kit_code_editor *e, const kit_code_desc *d)
{
    bool resting = e->hover >= 0 && e->pointer_in && !e->pointer_moved &&
                   !e->hover_blocked;
    int line = resting ? line_of(e, e->hover) : 0;
    if (!resting || (!(d->hover && d->hover[0]) && !line_marked(e, line))) {
        return;
    }
    node *card = emit_in(k, k->layer, frame_format(k, "@hover:%s", n->path), KIND_BOX, "div", "code-hover");
    kit_presence(k);
    place_hover(k, e, card);
    push_node(k, card);
    hover_rows(k, e, line, d->hover);
    pop_node(k);
}

kit_code_result kit_code(kit *k, const char *key, kit_code_editor *e, const kit_code_desc *d)
{
    node *n = emit(k, key, KIND_CODE, "kit-code", KIT_CX(k, "code", { d->cls, d->cls != NULL }));
    code_view *v = view_of(n);
    attach(v, e);
    configure(k, e, d);
    highlight(v, e, d);
    sync_colors(v);
    diagnose(e, d);
    take_completions(e, d);
    show_completions(k, n, e);
    show_hover(k, n, e, d);
    update_hover(k, e);
    k->last = n;
    kit_code_result result = e->result;
    result.focused = e->focused;
    e->result = (kit_code_result){ .completion = -1 };
    return result;
}

kit_rect code_caret(const kit *k, node *n)
{
    code_view *v = rml_element_user(n->el);
    if (!v || !v->editor || !v->rendered) {
        return element_rect(k, n->el);
    }
    float x, y, w, h;
    zep_cursor_rect(v->editor->zep, &x, &y, &w, &h);
    rml_vec2 origin = rml_element_absolute_offset(n->el, RML_BOX_CONTENT);
    float s = dp(k);
    return (kit_rect){ (origin.x + x) / s, (origin.y + y) / s, w / s, h / s };
}
