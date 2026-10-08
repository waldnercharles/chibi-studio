#ifndef CHIBI_EDITOR_UI_KIT_H
#define CHIBI_EDITOR_UI_KIT_H

#include "support/code_completion.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Immediate-mode calls reconciled onto retained RmlUi documents: the
     * kit's, its overlay layer's and those kit_document_begin opens.
     *
     * Each frame the editor emits its UI with keyed calls. Elements are created
     * on first use, updated when their properties change and removed when a
     * frame no longer emits them. Interaction results recorded during input
     * processing are returned by the next call with the same key, so widgets
     * read like ImGui: `if (kit_button(...))`, `kit_number(...)` returns true
     * on change, and kit_item_* queries describe the last emitted item.
     *
     * Keys are unique among siblings. Paths join ancestor keys with '/' and name
     * items for tests and inspection. Styling lives in the Studio scene's root
     * document (projects/studio/content/ui/StudioShell.rml, its .rcss and the theme
     * files it imports); behavior follows the Base UI contracts in
     * docs/prototypes/studio/kit.html. */

    typedef struct kit kit;

    typedef struct kit_rect
    {
        float x, y, w, h;
    } kit_rect;

    enum
    {
        KIT_MOD_CTRL = 1 << 0,
        KIT_MOD_SHIFT = 1 << 1,
        KIT_MOD_ALT = 1 << 2,
        KIT_MOD_META = 1 << 3,
    };

    enum
    {
        KIT_DISABLED = 1 << 0,
        KIT_READONLY = 1 << 1,
        KIT_OVERRIDDEN = 1 << 2,
        KIT_ON = 1 << 3,     /* pressed toggle, checked item, selected row */
        KIT_ACTIVE = 1 << 4, /* active tree row */
        KIT_INVALID = 1 << 5,
        KIT_SECRET = 1 << 6, /* text fields mask their value (passwords, tokens) */
        /* Several objects' values differ: a field shows a dash, a checkbox a
           dash in its box, as Unity's mixed values (data-mixed). */
        KIT_MIXED = 1 << 7,
    };

    /* Borrowed Rml::Context*; the kit's documents stay loaded until
     * kit_destroy. */
    kit *kit_create(void *context, const char *document_path);
    void kit_destroy(kit *k);
    /* RCSS that precedes the document's own stylesheet: the tokens and control
       styles of a scene's root document (render/ui.h ui_page). */
    void kit_theme(kit *k, const char *rcss);
    /* Positive dp metric from the authored theme, e.g. --row. */
    float kit_metric(const kit *k, const char *name);

    /* Host pointer state for the frame, in logical pixels. */
    typedef struct kit_frame_input
    {
        double time;
        float dp_ratio;
        float width, height;
        float mouse_x, mouse_y;
        float press_x[3], press_y[3];
        bool press_position[3]; /* button-down coordinates survived frame coalescing */
        bool down[3], pressed[3], released[3];
        float wheel;
        unsigned modifiers; /* KIT_MOD_* */
        /* A canvas's zoom, folded into dp_ratio, which tooltips undo to keep
         * their size on screen; 0 means 1. */
        float zoom;
    } kit_frame_input;

    /* Emit the frame between begin and end. Input for this frame must already be
     * processed; end removes anything not emitted and applies deferred work. */
    void kit_begin(kit *k, const kit_frame_input *input);
    void kit_end(kit *k);
    /* Seconds until RmlUi next needs a frame for a caret blink or a
     * transition; infinity when only input can change the kit. */
    double kit_idle_time(const kit *k);

    /* Structure. `tag` defaults to div; `cls` is a space-separated class list. */
    void kit_root(kit *k, const char *element_id);
    void kit_push(kit *k, const char *key, const char *tag, const char *cls);
    void kit_pop(kit *k);
    /* Places a box with `cls` and emits what follows, until
     * kit_document_end, into a document of its own shown over the box, as
     * an <iframe> shows one (ui/iframe.h): a change inside lays out only
     * that document. With `fit` the box takes the document's height. The
     * box is the last item. The document adds no step to the paths of what
     * it holds, so their keys must also differ from the box's siblings'. */
    void kit_document_begin(kit *k, const char *key, const char *cls, bool fit);
    void kit_document_end(kit *k);
    void kit_leaf(kit *k, const char *key, const char *tag, const char *cls);
    void kit_label(kit *k, const char *key, const char *cls, const char *text);
    /* A changing, single-line readout: the native <live-text>. Equal-width
     * values update glyphs locally; size and typography changes still use
     * normal layout. */
    void kit_live_label(kit *k, const char *key, const char *cls, const char *text);
    void kit_icon(kit *k, const char *key, const char *cls, uint32_t codepoint);
    /* A span whose content is markup the caller built and escaped, e.g. by
     * highlight_markup; reapplied only when it changes. */
    void kit_markup(kit *k, const char *key, const char *cls, const char *rml);
    /* Modify the last emitted element. */
    void kit_attr(kit *k, const char *name, const char *value);
    void kit_flag(kit *k, const char *name, bool on);
    void kit_style(kit *k, const char *property, const char *value);
    /* A tooltip on the last item: a label, an optional shortcut and, when the
     * label alone does not explain the item, a short description that wraps. */
    void kit_tip(kit *k, const char *label, const char *shortcut, const char *description);

    /* Motion. Presence (Motion's AnimatePresence, Base UI's starting and ending
     * styles): the last item mounts with data-starting-style for one frame and,
     * once no longer emitted, stays with data-ending-style until the transitions
     * its RCSS declares finish. Layout: position changes within its parent animate
     * with a spring (FLIP). Reduced motion makes both immediate. */
    void kit_presence(kit *k);
    void kit_layout(kit *k);

    /* NumberFlow: digits spin to each new value with the trend of the change
     * and keep their place value. For discrete changes (counts, zoom steps); a
     * continuously changing value should stay plain text. */
    typedef struct kit_number_flow_desc
    {
        int precision; /* fraction digits */
        bool grouping; /* thousands separators */
        const char *prefix;
        const char *suffix;
    } kit_number_flow_desc;
    void kit_number_flow(kit *k, const char *key, const char *cls, double value, const kit_number_flow_desc *desc);
    /* Torph: when the text changes, kept characters (or words) slide to their
     * new places, removed ones fade in place and new ones fade in. Never for
     * text being edited. */
    void kit_morph(kit *k, const char *key, const char *cls, const char *text);

    /* Last-item queries, valid until the next emitted item. */
    bool kit_item_clicked(kit *k);
    bool kit_item_pressed(kit *k);   /* the primary button went down on it */
    bool kit_item_active(kit *k);    /* scrubbing, editing or held */
    bool kit_item_committed(kit *k); /* a gesture or edit finished this frame */
    kit_rect kit_item_rect(kit *k);  /* logical pixels, latest layout */
    bool kit_item_hovered(kit *k); /* under the pointer and not clipped away */

    /* Conditional class lists (clsx): KIT_CX(k, "area", { "fill", fill }, {
     * "live", live }) joins the base with each class whose condition holds. The
     * string lives until kit_end. State belongs in data-* attributes
     * (kit_flag), not classes. */
    typedef struct kit_class
    {
        const char *name;
        bool on;
    } kit_class;
    const char *kit_classes(kit *k, const char *base, const kit_class *list, int count);
    /* A copy of text that lives until kit_end, for labels built per frame. */
    const char *kit_string(kit *k, const char *text);
    /* Puts text on the system clipboard, the browser's too in the web build,
       where a paste with no field focused hands the browser's text back to
       SDL's clipboard before the Mod+V key arrives. */
    void kit_clipboard_set(const char *text);
#define KIT_CX(k, base, ...)                                                   \
    kit_classes(                                                               \
        k,                                                                     \
        base,                                                                  \
        (const kit_class[]){ __VA_ARGS__ },                                    \
        (int)(sizeof((const kit_class[]){ __VA_ARGS__ }) / sizeof(kit_class))  \
    )

    /* Variants (cva): each maps to classes in one table in kit_controls.c. */
    typedef enum
    {
        KIT_GHOST, /* default: transparent until hovered */
        KIT_SUBTLE,
        KIT_PRIMARY,
        KIT_OUTLINE,
        KIT_DANGER,
    } kit_variant;
    typedef enum
    {
        KIT_MD,
        KIT_SM,
    } kit_size;

    /* Controls. Values are read every frame; a true return means the user
     * changed the value this frame and *value holds the new value. */
    typedef struct kit_button_desc
    {
        const char *label;
        uint32_t icon; /* an icon without a label renders square */
        kit_variant variant;
        kit_size size;
        const char *cls; /* placement hooks for the surrounding layout, never variants */
        bool morph; /* the label morphs when it changes (Torph) */
        const char *tip;
        const char *shortcut;
        unsigned flags;
        const char *description; /* the tip's second line, as kit_tip's */
    } kit_button_desc;
    bool kit_button(kit *k, const char *key, const kit_button_desc *desc);

    typedef struct kit_number_desc
    {
        double min, max; /* both zero means unbounded */
        double step;     /* zero means 1 */
        int precision;
        bool integer;
        bool single_precision; /* edit text round-trips a float rather than a double */
        bool slider;           /* bounded values draw a fill */
        const char *unit;
        const char *addon;     /* leading label inside the field, e.g. "X" */
        const char *addon_cls; /* the field's data-axis, e.g. "x" */
        unsigned flags;
    } kit_number_desc;
    /* The native <number-field> (ui/controls/controls.h): scrub, click-to-type,
     * expressions (support/number_expr.h) and keyboard steps.
     * kit_item_committed() is true once per gesture; record Undo there. */
    bool kit_number(kit *k, const char *key, double *value, const kit_number_desc *desc);
    bool kit_checkbox(kit *k, const char *key, bool *value, const char *label, unsigned flags);
    /* Up to three options render as a toggle group, more as a select. */
    bool kit_choice(kit *k, const char *key, int *value, const char *const *labels, int count, unsigned flags);
    /* Base UI ToggleGroup: one pressed segment among any count, for switching
     * views where every choice should stay visible. */
    bool kit_toggle_group(kit *k, const char *key, int *value, const char *const *labels, int count, unsigned flags);
    /* Commits on Enter or blur; Escape restores. Returns true on commit.
     * KIT_SECRET masks the value for passwords and tokens. */
    bool kit_text_field(kit *k, const char *key, char *buffer, size_t size, const char *placeholder, unsigned flags);
    /* A text field over several lines (shadcn Textarea): Shift+Enter starts a
     * line and the field grows with its lines up to a scrolling height;
     * otherwise it behaves as kit_text_field. */
    bool kit_text_area(kit *k, const char *key, char *buffer, size_t size, const char *placeholder, unsigned flags);
    /* Filters live; returns true whenever the text changes. Escape clears. */
    bool kit_search(kit *k, const char *key, char *buffer, size_t size, const char *placeholder);
    /* RGBA as 0xRRGGBBAA: the native <color-field>, live while its picker is
     * open; commits on close. */
    bool kit_color(kit *k, const char *key, uint32_t *rgba, unsigned flags);

    /* Containers. */
    bool kit_panel_begin(kit *k, const char *key, const char *title, const char *badge, bool default_open);
    void kit_panel_end(kit *k);
    void kit_prop_begin(kit *k, const char *key, const char *label);
    void kit_prop_end(kit *k);

    typedef struct kit_row_desc
    {
        const char *label;
        uint32_t icon;
        int depth;
        int twisty; /* -1 none, 0 collapsed, 1 expanded */
        const char *meta;
        unsigned flags; /* KIT_ON selected, KIT_ACTIVE active */
    } kit_row_desc;
    enum
    {
        KIT_ROW_CLICK = 1 << 0,
        KIT_ROW_DOUBLE_CLICK = 1 << 1,
        KIT_ROW_CONTEXT = 1 << 2,
        KIT_ROW_TWISTY = 1 << 3,
    };
    /* Begins a row; emit trailing toggles inside, then kit_row_end. Returns
     * KIT_ROW_* flags. */
    unsigned kit_row_begin(kit *k, const char *key, const kit_row_desc *desc);
    void kit_row_end(kit *k);

    /* Virtualized list (Virtuoso) of fixed-height rows. Begin sizes a scroll
     * container for every row and returns the visible range with overscan; emit
     * rows first..last in order, then end. Rows keep stable keys, so scrolling
     * reuses their elements. */
    typedef struct kit_list_desc
    {
        int count;
        float row_height; /* dp; every row must be exactly this tall */
        float overscan;   /* dp beyond the viewport; 0 means 120 */
        bool follow; /* stay pinned to the bottom while already there (logs) */
        int reveal; /* scroll this row into view when the index changes; -1 none */
    } kit_list_desc;
    bool kit_list_begin(
        kit *k,
        const char *key,
        const char *cls,
        const kit_list_desc *desc,
        int *first,
        int *last
    );
    void kit_list_end(kit *k);

    /* Popups live in the document's overlay layer. Open anchors below the last
     * item or at a point; begin returns true while open; end closes on
     * activation, and close closes one and its submenus at once. */
    void kit_popup_open(kit *k, const char *key);
    void kit_popup_open_at(kit *k, const char *key, float x, float y);
    bool kit_popup_is_open(const kit *k, const char *key);
    bool kit_has_popup(const kit *k);
    /* The "popover" class selects nonmodal form behavior: first tabbable
     * focus, normal control keys, Tab navigation and dismissal on focus out.
     * Other popups use menu keys and close on Tab. */
    bool kit_popup_begin(kit *k, const char *key, const char *cls);
    void kit_popup_end(kit *k);
    void kit_popup_close(kit *k, const char *key);

    typedef struct kit_menu_item_desc
    {
        const char *label;
        uint32_t icon;
        const char *shortcut;
        int checked; /* -1 not checkable */
        bool disabled;
        bool current; /* the active choice among items that keep their icons */
        uint32_t swatch; /* 0xRRGGBBAA: a color dot in the icon's place; 0 none */
        const char *description; /* optional second line, used by searchable choices */
    } kit_menu_item_desc;
    bool kit_menu_item(kit *k, const char *key, const kit_menu_item_desc *desc);
    void kit_menu_separator(kit *k, const char *key);
    void kit_menu_group(kit *k, const char *key, const char *label);
    bool kit_submenu_begin(kit *k, const char *key, const char *label, uint32_t icon);
    void kit_submenu_end(kit *k);

    /* An item of a searchable list (a select's dropdown, an autocomplete). A
     * description shows as a muted second line under the label. Items rank
     * by support/text_rank.h on their label, then their keywords, plus their
     * boost. The palette and Studio's pickers are authored (Palette.rml,
     * Picker.rml). */
    typedef struct kit_command_item
    {
        const char *label;
        const char *group;
        const char *meta;
        const char *keywords;
        uint32_t icon;
        const char *description;
        int boost;
        bool current;
    } kit_command_item;
    typedef struct kit_autocomplete_result
    {
        int selected; /* original catalog index, or -1 */
        bool submitted; /* Enter without an active suggestion; caller validates text */
        bool canceled; /* Escape restored the displayed value */
    } kit_autocomplete_result;
    /* Whether a select's searchable choice list (`select:<path>`) is open. */
    bool kit_dropdown_is_open(const kit *k, const char *key);
    /* Inline text input with an anchored, list-only suggestion popup. `key` is
     * the local child key in the current kit container. `value` remains caller-
     * owned and is never changed; query is NULL while closed and empty before
     * typing, even though the field displays `value`. Escape restores `value`.
     * Enter or Tab accepts a suggestion; Shift+Tab moves focus normally.
     * Item `meta` is the trailing type; `description` is hover help.
     * Submit a validated free-text value with kit_autocomplete_close. */
    const char *kit_autocomplete_query(const kit *k, const char *key);
    kit_autocomplete_result kit_autocomplete(
        kit *k,
        const char *key,
        const char *value,
        const kit_command_item *items,
        int count,
        const char *placeholder,
        unsigned flags
    );
    /* Close after accepting a submitted free-text value. */
    void kit_autocomplete_close(kit *k, const char *key);

    /* A surface shows a host texture ("kit:<name>") and receives raw pointer
     * input. Its returned element lasts while this key is emitted. */
    typedef struct rml_element rml_element;
    typedef struct kit_surface_input
    {
        kit_rect rect;
        float x, y; /* pointer relative to rect */
        float press_x[3], press_y[3];
        bool press_position[3];
        bool hovered;
        bool pressed[3], released[3], down[3];
        float wheel;
    } kit_surface_input;
    rml_element *kit_surface(
        kit *k,
        const char *key,
        const char *cls,
        const char *texture,
        kit_surface_input *input
    );

    /* A host element whose children the kit leaves alone, for content mounted
     * from a ui asset (render/ui.h). It lasts while its key is emitted. Keys
     * typed inside it are the content's, except Escape and Tab in a popup. */
    rml_element *kit_host(kit *k, const char *key, const char *cls);
    /* The last item's element, where an authored popover anchors. */
    rml_element *kit_item_element(kit *k);
    /* A host that covers the window above the editor and takes the pointer, for
     * hosted modal content such as a dialog. While it shows, Tab and Shift+Tab
     * move the focus through its content only (Base UI's Dialog). */
    rml_element *kit_modal_host(kit *k, const char *key);
    /* A modal whose children are emitted by the kit. It has the same focus
     * trap and layer behavior as kit_modal_host, with kit_modal_end closing it. */
    void kit_modal_begin(kit *k, const char *key);
    void kit_modal_end(kit *k);
    /* Adds `rcss` after the kit's stylesheets under `key`, replacing what the key
     * held; NULL removes it. Hosted content installs its scoped RCSS here. */
    void kit_style_sheet(kit *k, const char *key, const char *rcss);
    /* Sets the lowercase registered family used by kit_text_width. */
    void kit_set_standard_font_family(kit *k, const char *family);

    /* Toasts (Sonner): a bottom-right stack, three visible, four seconds each,
     * paused while hovered, dismissed by a drag to the right. Passing the id of
     * a live toast updates it in place (a loading toast becoming success); 0
     * creates one. */
    typedef enum
    {
        KIT_TOAST_DEFAULT,
        KIT_TOAST_SUCCESS,
        KIT_TOAST_INFO,
        KIT_TOAST_WARNING,
        KIT_TOAST_ERROR,
        KIT_TOAST_LOADING, /* stays until updated */
    } kit_toast_type;
    typedef struct kit_toast_desc
    {
        kit_toast_type type;
        const char *title;
        const char *description;
        const char *action; /* one button, e.g. "Undo"; pressing it dismisses the toast */
        double duration; /* seconds; 0 means 4, negative stays until closed */
    } kit_toast_desc;
    int kit_toast(kit *k, int id, const kit_toast_desc *desc);
    void kit_toast_dismiss(kit *k, int id);
    /* True once, the frame after the toast's action was pressed. */
    bool kit_toast_action(kit *k, int id);

    /* Drag and drop (dnd-kit), thin wrappers over the drag every document
     * shares (ui/drag.h), so kit and authored lists drag into each other. The
     * last item becomes a source (data-drag): moving FEEL_DRAG_DP with the
     * button held picks it up; an overlay follows the pointer and Escape
     * cancels. The last item becomes a target (data-drop) for `type`, and for
     * each other type another call names: while a drag it accepts runs it is
     * marked data-drop-accept, and data-drop-over while that drag is over it;
     * it returns true, with the payload, in the frame after a drop of `type`
     * lands on it. */
    typedef struct kit_drag_desc
    {
        const char *type; /* e.g. "prototype" */
        const char *payload;
        const char *label; /* overlay text; defaults to the payload */
        uint32_t icon;
    } kit_drag_desc;
    void kit_drag_source(kit *k, const kit_drag_desc *desc);
    bool kit_drop_target(kit *k, const char *type, char *payload, size_t size);
    /* While a drag the last target accepts is over it (dnd-kit's `over`), its
     * type and payload, so the target can preview the drop. */
    bool kit_drop_hover(kit *k, char *type, size_t type_size, char *payload, size_t size);
    /* What dropping here would do, under the overlay's label until the
     * pointer reaches another target, as dnd-kit announces a drag over a
     * droppable. */
    void kit_drag_hint(kit *k, const char *text);
    /* A drag of this type (any when NULL) is in progress. */
    bool kit_dragging(const kit *k, const char *type);

    /* Node-editor wires and the canvas grid, drawn by one element (xyflow
     * edges, Blender noodles). A wire end anchors to the center of the element
     * at a kit path, resolved after layout, or to a point; points and waypoints
     * are world dp, offset by camera_x/y into the element. Wires take the shape
     * arrange_path gives their style, so the styles follow arrange_route_style. */
    typedef enum kit_wire_style
    {
        KIT_WIRE_CURVED,
        KIT_WIRE_ORTHOGONAL, /* level and vertical runs with rounded corners */
        KIT_WIRE_STRAIGHT,
        KIT_WIRE_SUBWAY, /* level runs joined by 45 degree diagonals */
        KIT_WIRE_STYLE_COUNT,
    } kit_wire_style;
    enum
    {
        KIT_WIRE_DASHED = 1 << 0,
        /* Bubbles travel from source to target at a constant on-screen speed,
         * evenly spaced, sized by `pulse` (Unreal's execution pulses). */
        KIT_WIRE_FLOW = 1 << 1,
    };
    typedef struct kit_wire
    {
        const char *from, *to; /* kit paths; NULL uses the point */
        float from_x, from_y, to_x, to_y;
        const float *via; /* waypoints as x, y pairs */
        int via_count;
        uint32_t color; /* 0xRRGGBBAA */
        float width;    /* dp; 0 means 1.5 */
        unsigned flags;
        float flow;       /* seconds, the bubbles' phase with KIT_WIRE_FLOW */
        float pulse;      /* the bubbles' size, 0 to 1 */
        float flow_width; /* optional hot stroke width in dp; 0 none */
        uint32_t flow_color; /* hot stroke/bubble color; 0 uses color */
    } kit_wire;
    typedef struct kit_wires_desc
    {
        const kit_wire *wires;
        int count;
        kit_wire_style style;
        float camera_x, camera_y; /* world dp at the element's top-left */
        float grid;               /* line spacing in dp; 0 draws no grid */
        float grid_x, grid_y; /* grid coordinate at the element's top-left */
        uint32_t grid_color, grid_major_color;
        /* Screen pixels per dp without the canvas's zoom, in which bubble
         * spacing and speed stay constant; 0 uses the canvas's dp ratio. */
        float flow_scale;
    } kit_wires_desc;
    void kit_wires(kit *k, const char *key, const char *cls, const kit_wires_desc *desc);
    /* The cubic pieces of a wire drawn in `style` through `count` knots, given
     * as x, y pairs with both ends included. Writes up to `max` pieces as
     * x0 y0 x1 y1 x2 y2 x3 y3 and returns the piece count, which may exceed
     * max. */
    int kit_wire_shape(kit_wire_style style, const float *knots, int count, float *pieces, int max);
    /* The last drawn curve of wire `index` as x, y pairs in the element's dp.
     * Returns the point count, which may exceed max. */
    int kit_wire_points(const kit *k, const char *path, int index, float *xy, int max);

    /* A vector glyph that fills its box with one-pixel antialiasing at any
     * scale: a node socket, a status dot. The ring is drawn inside the box. */
    typedef enum kit_dot_shape
    {
        KIT_DOT_CIRCLE,
        KIT_DOT_ARROW,  /* pointing right */
        KIT_DOT_SQUARE, /* a many-valued socket: arrays and sets */
    } kit_dot_shape;
    typedef struct kit_dot_desc
    {
        kit_dot_shape shape;
        uint32_t fill; /* 0xRRGGBBAA */
        uint32_t ring;
        float ring_width; /* dp */
    } kit_dot_desc;
    void kit_dot(kit *k, const char *key, const char *cls, const kit_dot_desc *desc);

    /* Host queries. */
    bool kit_wants_keyboard(const kit *k);
    bool kit_wants_text(const kit *k, kit_rect *caret);
    /* True while focus can receive typed characters, including a focused number
     * field. */
    bool kit_accepts_text(const kit *k);
    /* True while an input or textarea has focus: a text field, not a code
     * editor or a hosted control. */
    bool kit_input_focused(const kit *k);
    bool kit_wants_pointer(const kit *k);
    /* Width in dp of one line of the configured standard face at `size` dp
     * and `weight` (400 regular),
     * as drawn at `ratio` pixels per dp: text renders at whole pixel sizes with
     * hinting, so small sizes run wider than a scaled measurement. */
    float kit_text_width(const kit *k, const char *text, float size, int weight, float ratio);
    /* Focuses the item emitted last, selecting a field's text (Base UI autoFocus).
     */
    void kit_focus(kit *k);
    /* Drops keyboard focus, as a press elsewhere in the window does. */
    void kit_blur(kit *k);
    /* The path of the innermost emitted item under the pointer, or NULL. */
    const char *kit_hovered_path(const kit *k);
    /* True over a control that takes the pointer itself: fields, buttons, choices.
     */
    bool kit_pointer_on_control(const kit *k);
    bool kit_find(const kit *k, const char *path, kit_rect *out);
    void kit_set_theme(kit *k, const char *name, bool active);
    void kit_set_body_class(kit *k, const char *name, bool on);

    /* ---------- UI Inspector (kit_inspect.c) ---------- */

    /* An element's boxes in the kit's logical pixels, outermost first. An
     * empty clip leaves the overlay unclipped; otherwise it stays inside it,
     * as over the surface that shows another context's element. */
    typedef struct kit_highlight_desc
    {
        kit_rect margin, border, padding, content;
        kit_rect clip;
        const char *label; /* beside the border box, e.g. "div.row 120 × 24" */
    } kit_highlight_desc;
    /* The Inspector's overlay, above everything in the layer. While picking
     * it takes the pointer, so a press picks rather than reaching what is
     * under it; after kit_inspect_end, kit_item_clicked tells when it was
     * clicked. Returns its element, which picks ignore. */
    rml_element *kit_inspect_begin(kit *k, const char *key, bool picking);
    void kit_inspect_end(kit *k);
    /* Authoring guides use the same highlights below popups and dialogs.
     * UI inspection stays above them so it can inspect those controls. */
    void kit_guides_begin(kit *k, const char *key);
    void kit_guides_end(kit *k);
    /* DevTools' node highlight: margin, border, padding and content bands in
     * their colors. Emit inside an inspection or authoring-guide overlay. */
    void kit_highlight(kit *k, const char *key, const kit_highlight_desc *desc, const char *cls);
    /* A readable path for `element`: the key path of one the kit emitted,
     * else the nearest emitted ancestor's path and tag#id.class steps below
     * it. True when the kit draws it, in any of its documents, rather than
     * content mounted in a kit_host or an element of another document. */
    bool kit_element_path(const kit *k, rml_element *element, char *out, size_t size);
    /* tag#id.class.class, as a selector names the element. */
    void kit_element_selector(rml_element *element, char *out, size_t size);
    /* ---------- UI wiring (workstream K) ---------- */

    /* An asset field (Unity's object field, Unreal's asset picker, Base UI
     * Combobox's trigger): the value with its icon and a caret, shaped like a
     * select. Pressed, it returns true and the caller opens a search over
     * the choices (kit_command); read-only and disabled fields never do. */
    typedef struct kit_picker_desc
    {
        const char *value;       /* NULL or "" shows the placeholder, muted */
        const char *placeholder; /* "None" */
        uint32_t icon;
        const char *tip;
        unsigned flags; /* KIT_READONLY, KIT_DISABLED, KIT_INVALID */
    } kit_picker_desc;
    bool kit_picker(kit *k, const char *key, const kit_picker_desc *desc);

    /* ---------- Code editor (kit_code.c, workstream C) ---------- */

    /* A code editor over one text buffer: Zep (src/editor/text/zep_bridge.h)
     * with Vim or standard keys, drawn through RmlUi in JetBrains Mono. The
     * editor object keeps the text, the cursor and one undo history for as
     * long as the caller keeps it, apart from any element; kit_code shows it
     * for a frame, as VS Code's editor shows a model. A caller whose document
     * keeps its own history takes the undo keys instead (desc.history).
     * Positions are byte offsets into kit_code_text, which keeps '\n' line
     * ends only. */
    typedef struct kit_code_editor kit_code_editor;
    kit_code_editor *kit_code_editor_create(const char *text);
    void kit_code_editor_destroy(kit_code_editor *e);
    /* Replaces the text and forgets the history, as reloading a file does. */
    void kit_code_set_text(kit_code_editor *e, const char *text);
    /* The current text; valid until the next change. */
    const char *kit_code_text(kit_code_editor *e, size_t *length);
    /* Applies text_edits (support/text_edit.h: byte ranges into the current
     * text, in any order) as one undo step; the cursor keeps its place in
     * the text around them. */
    typedef struct text_edit text_edit;
    void kit_code_apply(kit_code_editor *e, const text_edit *edits, int count);
    /* Inserts pasted text at the cursor or replaces the active selection.
     * CRLF and CR line endings become LF; the paste is one undo step. */
    void kit_code_paste(kit_code_editor *e, const char *text);
    /* The editor's own history. */
    void kit_code_undo(kit_code_editor *e);
    void kit_code_redo(kit_code_editor *e);
    int kit_code_cursor(const kit_code_editor *e);
    void kit_code_set_cursor(kit_code_editor *e, int byte);
    /* Selects [start, end), the cursor at its end, as Find shows a match. */
    void kit_code_select(kit_code_editor *e, int start, int end);
    /* The active visual selection's ordered byte range; false without one. */
    bool kit_code_selection(const kit_code_editor *e, int *start, int *end);
    /* Selects from the active editor's anchor to its cursor, including reverse
     * selections; offsets are byte boundaries in the document. */
    void kit_code_set_selection(kit_code_editor *e, int anchor, int cursor);
    /* The byte whose glyph the pointer has rested on, or -1: what the next
     * frame's desc.hover describes. */
    int kit_code_hover(const kit_code_editor *e);
    /* Vim's mode as its status line names it ("NORMAL", "INSERT",
     * "VISUAL"), or NULL with the standard keys. */
    const char *kit_code_mode(const kit_code_editor *e);

    /* Events since the last kit_code for this editor. */
    enum
    {
        KIT_CODE_CHANGED = 1 << 0,   /* the text changed */
        KIT_CODE_SAVED = 1 << 1,     /* :w, :wq or :x */
        KIT_CODE_CURSOR = 1 << 2,    /* the cursor moved */
        KIT_CODE_CHORD = 1 << 3,     /* a shortcut for the host: see chord */
        KIT_CODE_COMPLETED = 1 << 4, /* a completion was inserted: completion */
        KIT_CODE_DISMISSED = 1 << 5, /* Escape closed the completions */
        /* With desc.history: undo or redo was asked for (the primary
         * modifier and Z or Y, Vim's u and Control+R) and nothing changed. */
        KIT_CODE_UNDO = 1 << 6,
        KIT_CODE_REDO = 1 << 7,
        /* Go to Definition (F12, or a primary click, which places the
         * cursor first) and Find References (Shift+F12), at the cursor. */
        KIT_CODE_DEFINITION = 1 << 8,
        KIT_CODE_REFERENCES = 1 << 9,
        /* Control+Space: complete at the cursor. */
        KIT_CODE_SUGGEST = 1 << 10,
        KIT_CODE_BREAKPOINT = 1 << 11,
        KIT_CODE_RENAME = 1 << 12, /* F2, or Vim gR */
        KIT_CODE_CALLS = 1 << 13,  /* Shift+Alt+H, or Vim gc */
    };
    typedef struct kit_code_result
    {
        unsigned events;
        /* A chord with the primary modifier the editor leaves to the host,
         * in studio_chord notation ("Mod+S", "Mod+Shift+A"): Save, Search,
         * UI Scale and the drawers always, anything the standard keys do not
         * edit with, and on a Mac every Command chord in Vim but Z and V. */
        char chord[32];
        int completion; /* the accepted item, or -1 */
        bool focused;   /* the editor holds the keyboard */
    } kit_code_result;

    typedef enum kit_code_severity
    {
        KIT_CODE_ERROR,
        KIT_CODE_WARNING,
        KIT_CODE_INFO,
    } kit_code_severity;
    /* A diagnostic: a gutter mark and an underline whose message shows in
     * the hover card when the pointer rests on the line. */
    typedef struct kit_code_marker
    {
        int line; /* 1-based */
        kit_code_severity severity;
        const char *message;
        /* Workstream Q: bytes [start, end) of the text it is about, which
         * the underline spans; the whole line when end <= start. */
        int start, end;
        bool indicator_only;
    } kit_code_marker;
    /* Highlighting, as Shiki's tokens carry classes: bytes [start, end)
     * draw in the color the theme gives `cls`, tok-* classes such as
     * ui_source_kind_class's "tok-keyword tok-tag". */
    typedef struct kit_code_span
    {
        int start, end;
        const char *cls;
    } kit_code_span;
    /* A completion item: accepting it replaces the bytes from the desc's
     * completion_start to the cursor with insert (label when NULL). */
    typedef struct kit_code_completion
    {
        const char *label;
        const char *detail;
        const char *insert;
        code_completion_kind kind;
    } kit_code_completion;
    typedef struct kit_code_desc
    {
        const char *cls;
        bool vim;        /* studio_prefs.code_vim */
        float font_size; /* dp; studio_prefs.code_font_size, 0 means 13 */
        const char *font_family; /* a registered RmlUi family, NULL means JetBrains Mono */
        /* The host trades Control and Command in RmlUi's key events on a
         * Mac (studio_host.c), so primary chords read as Control; the
         * editor trades them back so Vim sees the physical Control key. */
        bool mac;
        const kit_code_span *spans; /* in order, not overlapping */
        int span_count;
        const kit_code_marker *markers;
        int marker_count;
        /* Shown under the cursor while there are any and the editor has
         * focus; Up and Down choose, Enter or Tab accepts, Escape dismisses. */
        const kit_code_completion *completions;
        int completion_count;
        int completion_start;
        /* The caller keeps the undo history: the undo keys report
         * KIT_CODE_UNDO and KIT_CODE_REDO, and the editor keeps none. */
        bool history;
        /* Lines run on past the right edge, which scrolls to the cursor and
         * with the wheel sideways, instead of wrapping. */
        bool no_wrap;
        /* What the byte kit_code_hover names is: a title line, then detail
         * lines. It shows in a card under the pointer, after the diagnostics
         * of its line. */
        const char *hover;
    } kit_code_desc;
    kit_code_result kit_code(kit *k, const char *key, kit_code_editor *e, const kit_code_desc *desc);

    /* ---------- Native slots (kit_slot.c, workstream J) ---------- */

    /* Kit content inside a kit_host's authored content, as Unity's
     * IMGUIContainer: begin adopts the element `selector` finds in `host`
     * and returns true while there is one; emit into it as into kit_push's
     * box, then call kit_slot_end. Its path is the host's and `key`. The
     * element is marked data-kit-slot="<key>" and found again every frame,
     * so when the content replaces it (a mount reloading its asset, data-for
     * recreating an item) the slot builds anew in the new element. A slot
     * not begun in a frame loses its content. Name one element, as an id
     * does; its own children stay, before the kit's. */
    bool kit_slot_begin(kit *k, const char *key, rml_element *host, const char *selector);
    void kit_slot_end(kit *k);
    /* Moves focus out of authored chrome while fields in native slots keep it. */
    void kit_release_focus(kit *k, rml_element *host);

#ifdef __cplusplus
}
#endif

#endif
