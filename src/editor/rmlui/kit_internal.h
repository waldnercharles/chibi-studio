/* Studio kit internals shared by the reconciler (kit.c) and its feature
 * modules: controls, number fields, popups and menus, the command palette,
 * motion, text in place, toasts, lists, drag and drop and wires. Not part of
 * the C API.
 *
 * Conventions:
 * - An owned string is a `char *`, NULL until first set. str_set replaces it
 *   with a copy and reports a change; str_eq compares. Both treat a NULL
 *   string or NULL text as "". A NULL string always changes on its first
 *   str_set, which applies a widget's label, unit, addon, meta and badge on
 *   their first frame even when empty.
 * - Vectors are support/array.h arrays (apush, acount, asetlen, afree), NULL
 *   when empty. An array marked owned frees its elements too.
 * - A node's attrs and styles are arrays of pairs searched linearly.
 * - Per-kit indexes are CF_MAPs: by_element keyed by the element pointer;
 *   by_path, popups and springs keyed by str_hash of a path or key the value
 *   carries, and every lookup compares it (path_node, popup_find). A popup is
 *   heap allocated, so pointers to it stay valid; spring values are inline and
 *   move on insertion. No popup loop depends on key order.
 * - Owned pointers (node children; exiting nodes; roots, popups)
 *   are freed only by node_free and kit_destroy; update them with any new
 *   owned field.
 * - Strings for one frame (frame_format, kit_string, kit_classes) are freed by
 *   kit_end. */
#ifndef CHIBI_EDITOR_RMLUI_KIT_INTERNAL_H
#define CHIBI_EDITOR_RMLUI_KIT_INTERNAL_H

#include "editor/ui/kit.h"
#include "platform/cute.h"
#include "support/array.h"
#include "ui/controls/controls.h"
#include "ui/drag.h"
#include "ui/input_feel.h"
#include "ui/list.h"
#include "ui/motion.h"
#include "ui/rml.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum node_kind
{
    KIND_BOX,
    KIND_ROOT,
    KIND_BODY,
    KIND_BUTTON,
    KIND_NUMBER,
    KIND_CHECK,
    KIND_TOGGLE,
    KIND_SELECT,
    KIND_TEXT,
    KIND_SEARCH,
    KIND_AUTOCOMPLETE,
    KIND_COLOR,
    KIND_PANEL,
    KIND_PROP,
    KIND_ROW,
    KIND_POPUP,
    KIND_MENU_ITEM,
    KIND_SUBMENU,
    KIND_SURFACE,
    KIND_LIST,
    KIND_CODE, /* kit_code: takes keys and text while focused */
} node_kind;

/* Interaction recorded on a node during input, cleared by kit_end. */
enum
{
    EV_CLICK = 1 << 0,
    EV_DOUBLE = 1 << 1,
    EV_CONTEXT = 1 << 2,
    EV_PRESS = 1 << 3,
    EV_TWISTY = 1 << 4,
};

typedef struct node node;

typedef struct pair
{
    char *name;
    char *value;
} pair;

/* Control state. A new node's widget has pending_index -1 and icon
 * UINT32_MAX; everything else starts zero. */
typedef struct widget
{
    bool open;
    bool pending;
    bool committed;
    bool focused;
    bool area; /* a text field over several lines */
    bool autocomplete_query_active;
    bool autocomplete_submitted;
    bool autocomplete_canceled;
    bool autocomplete_sync;
    bool secret;
    double value;
    int pending_index;
    kit_number_desc desc; /* its strings are borrowed for one call only */
    char *addon, *draft, *applied, *label, *meta, *badge, *placeholder;
    char **labels; /* owned strings */
    uint32_t icon;
    uint32_t color;
    ui_list_state list; /* list: its window's scroll state (ui/list.h) */
    rml_element *body;  /* list content */
    rml_element *value_el, *input_el, *label_el, *icon_el, *meta_el, *twisty_el,
        *kbd_el, *badge_el, *head_el, *about_el;
} widget;

struct node
{
    char *key;
    char *path;
    node_kind kind;
    rml_element *el;
    rml_element *container; /* where tracked children go */
    node *parent;
    node **children; /* owned */
    int cursor;
    uint64_t frame;
    char *cls, *text, *tag;
    pair *attrs, *styles;
    unsigned events; /* EV_* */
    widget w;
    uint64_t created; /* frame of creation */
    bool presence; /* kept with data-ending-style until its transitions finish */
    bool layout;   /* position changes animate (FLIP) */
    bool layout_known;
    kit_rect layout_rect;
    spring layout_x, layout_y;
    char **drop_types; /* accepted drag types, set each frame by kit_drop_target */
    bool drag_source; /* kit_drag_source named it this frame */
    bool hosted;      /* kit_host: its children are mounted content */
    /* kit_document_begin: its container is the body of a document of its
       own, shown over el; it adds no step to its children's paths. */
    bool document;
};

/* A toast (kit_toast.c). Positions are springs around their targets, in dp. */
typedef struct toast
{
    int id;
    kit_toast_type type;
    char *title, *description, *action;
    double duration, remaining; /* seconds; a negative duration stays until closed */
    bool placed, removing, swiping, swiped, forced_height;
    float height, press_x, swipe_velocity;
    double y_target, x_target, scale_target, opacity_target;
    spring y, x, scale, opacity;
} toast;

/* A retired node kept until its exit transitions end. */
typedef struct exiting
{
    node *node; /* owned */
    double until;
} exiting;

/* popup_at creates one closed, with highlight -1. Open, it is stacked with
 * ui/popover.c from its first emission (shown) until popover.c or the kit
 * closes it. */
typedef struct popup
{
    char *key;
    bool open;
    bool shown;
    bool at_point;
    bool beside; /* a submenu, beside its row */
    bool keyboard;
    bool dropdown;
    bool dropdown_focus;
    bool dropdown_reveal;
    bool autocomplete;
    bool autocomplete_opening;
    char *trigger;     /* the path of what focus returns to */
    char *anchor_path; /* the path it hangs from; NULL hangs from `anchor` */
    float x, y;
    kit_rect anchor;
    char *parent; /* parent popup key for a submenu or nested dropdown */
    char *dropdown_placeholder;
    char *dropdown_trigger_path;
    char dropdown_query[256];
    rml_element *dropdown_search;
    int highlight;       /* a dropdown's, which its search field moves */
    node **items;        /* entries may be NULL */
    node *node;          /* the emitted popup, NULL once unmapped */
    float content;       /* widest row this frame, dp; sizes the menu */
    float content_shown; /* the width applied */
} popup;

struct kit
{
    rml_context *context;
    rml_element *doc;
    /* Every document the kit emits into, each with its listener: doc, the
       layer's, then those kit_document_begin opened. */
    rml_element **documents;
    char **body_classes; /* owned: kit_set_body_class's, on every document */
    rml_listener *listener;
    char *document; /* the markup's file name, which names its sheets */
    char *theme;    /* kit_theme's RCSS, the document's stylesheet */
    char *standard_font_family; /* lowercase RmlUi family used for text
                                   measurements */
    pair *extra_styles; /* kit_style's RCSS, added after the stylesheets */
    bool restyle;       /* the theme or the extra styles changed */
    node **roots;       /* owned: the layer, then kit_root elements */
    CF_MAP(node *) by_element; /* keyed by the element pointer */
    CF_MAP(node *) by_path;    /* keyed by str_hash(path) */
    node **stack;
    node *last;
    node *root;
    node *layer;
    uint64_t frame; /* starts at 1 */
    kit_frame_input in;
    CF_MAP(popup *) popups; /* owned; keyed by str_hash(key) */
    rml_element *tooltip;
    rml_element *tip_target;
    double tip_since;
    double tip_warm_until;
    bool tip_visible;
    uint64_t tip_shown_frame;
    char *tip_image; /* the picture the shown tooltip holds, owned */
    char *surface_capture;
    int surface_button; /* -1 none */
    popup **emitting;   /* popups being emitted, innermost last */
    bool keyboard_edit;
    char **strings; /* owned frame strings; freed by kit_end */
    bool reduced_motion;
    double motion_time;
    node **entering; /* data-starting-style set at creation */
    exiting *exiting;
    node **layout_nodes;
    bool dragging; /* a drag ran in the context at the last kit_begin */
    toast *toasts;
    CF_MAP(uint64_t) toast_actions; /* toast id -> frame the action was pressed */
    int toast_next;
    double toast_time;
    float front_height;
    bool toasts_expanded;
    node **slots; /* owned: native slots (kit_slot.c), outer ones first */
};

/* ---------- Core (kit.c) ---------- */

/* Strings. str_cat, str_catf and escape append to an owned string, starting
 * one when NULL. */
uint64_t str_hash(const char *s);
bool str_eq(const char *a, const char *b);
bool str_set(char **s, const char *text);
void str_cat(char **s, const char *text);
void str_catf(char **s, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
/* Appends text with &, <, > and " as RML entities. */
void escape(char **s, const char *text);
void utf8(char out[static 5], uint32_t cp);
/* A formatted string that lives until kit_end. */
const char *frame_format(kit *k, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
bool rect_contains(kit_rect r, float x, float y);

/* Element reads, as owned strings. */
char *element_value(const rml_element *el);
char *element_attribute(const rml_element *el, const char *name);
char *element_inner_rml(const rml_element *el);

/* Geometry in logical pixels. */
float dp(const kit *k);
kit_rect element_rect(const kit *k, rml_element *el);

/* Reconciled attribute writes: each touches the element only on change. */
void set_class(node *n, const char *cls);
void set_attr(node *n, const char *name, const char *value);
void set_flag(node *n, const char *name, bool on);
void flag_element(rml_element *el, const char *name, bool on);
void set_text(node *n, const char *text);
void set_element_text(rml_element *el, const char *text);
/* Escapes text into the element's content unconditionally. */
void set_inner_text(rml_element *el, const char *text);

/* Untracked child elements owned by a widget; text may be NULL. */
rml_element *raw(rml_element *parent, const char *tag, const char *cls, const char *text);
rml_element *raw_icon(rml_element *parent, const char *cls, uint32_t cp);

/* Keyed emission. node_free frees a node, its children and everything they
 * own, never their elements. */
node *node_new(node_kind kind);
void node_free(node *n);
void unmap(kit *k, node *n);
node *emit_in(kit *k, node *parent, const char *key, node_kind kind, const char *tag, const char *cls);
node *emit(kit *k, const char *key, node_kind kind, const char *tag, const char *cls);
void push_node(kit *k, node *n);
void pop_node(kit *k);
node *node_for(const kit *k, rml_element *el);
node *path_node(const kit *k, const char *path);
bool within(rml_element *el, rml_element *ancestor);
/* The index of n in a node array, or -1. */
int nodes_index(node *const *list, const node *n);
/* Removes every n from a node array, keeping order. */
void nodes_remove(node **list, const node *n);
/* Whether the kit emits into this document. */
bool owns_document(const kit *k, const rml_element *document);

/* Popups by key: find returns NULL when absent, at creates one closed. */
popup *popup_find(const kit *k, const char *key);
popup *popup_at(kit *k, const char *key);

/* Event routing: the listener dispatches each event by node kind to the
 * handlers below. Returns KIT_MOD_* bits. */
unsigned event_modifiers(const rml_event *e);

/* ---------- Motion (kit_motion.c; springs in ui/motion.h) ---------- */

/* Takes the node: removes its element now, or keeps a presence node until its
 * exit transitions end. */
void retire(kit *k, node *n);
/* Frees the exiting nodes whose elements are about to go with an ancestor. */
void release_exiting_within(kit *k, rml_element *ancestor);
void presence(kit *k, node *n);
void motion_frame(kit *k);
void forget_motion(kit *k, node *n);

/* ---------- Toasts (kit_toast.c) ---------- */

void toasts_frame(kit *k);

/* ---------- Drag and drop (kit_drag.c over ui/drag.h) ---------- */

/* At kit_begin: ends a drag whose release the context missed, and closes
 * the popups as one starts. */
void drag_frame(kit *k);
/* At kit_end, for each node emitted: its source and target attributes as
 * this frame named them. */
void drag_flush(node *n);
/* Forgets the drag types a node accepted last frame. */
void drop_types_clear(node *n);

/* ---------- Custom elements (kit_wires.c) ---------- */

/* Registered per kit: RmlUi's factory is rebuilt whenever it reinitializes. */
void register_wires_element();
void register_dot_element();

/* ---------- Other controls (kit_controls.c) ---------- */

/* Handlers ignore kinds they do not own. */
void control_click(kit *k, node *n, rml_element *target);
node *emit_input(kit *k, const char *key, node_kind kind, const char *cls, const char *placeholder);
void control_key(kit *k, node *n, const rml_event *e);
void control_change(kit *k, node *n, rml_element *target);
void control_focus(kit *k, node *n, rml_element *target);
void control_blur(node *n, rml_element *target);

/* ---------- Popups, menus and tooltips (kit_popup.c) ---------- */

void close_popup(kit *k, const char *key);
/* The kit's popups and the context's authored popovers. */
void close_all_popups(kit *k);
/* Closes those popover.c closed; kit_end runs it after their last frame. */
void forget_closed_popups(kit *k);
/* parent is NULL or "" for a top-level popup. */
popup *open_popup(kit *k, const char *key, const char *parent);
/* Opens below `anchor`, closing the kit's popups but the one holding
   `trigger` and its parents. */
popup *popup_open_on(kit *k, const char *key, const node *anchor, const node *trigger);
/* NULL when it toggled the trigger's open dropdown closed. */
popup *dropdown_open_on(kit *k, const char *key, const char *placeholder, const node *trigger);
/* Emits open dropdown `key`'s search and its items ranked by the query;
   the chosen item's original index, or -1. Call it each frame while open. */
int dropdown_items(kit *k, const char *key, const kit_command_item *items, int count);
void autocomplete_open_on(kit *k, const char *key, node *trigger);
void size_popups(kit *k);
/* Sees every press: one outside an autocomplete ends its field's editing. */
void popup_press(kit *k, rml_element *target);
void menu_click(kit *k, node *n);
void menu_hover(kit *k, node *n);
bool dropdown_key(kit *k, const rml_event *e, node *n);
bool autocomplete_key(kit *k, const rml_event *e, node *n);
void set_tip(node *n, const char *label, const char *shortcut, const char *description);
void update_tooltip(kit *k);

/* ---------- Command palette (kit_command.c) ---------- */

/* Key and change handlers return false for events that are not the
 * palette's. */

/* ---------- Native slots (kit_slot.c) ---------- */

/* Drops the slots whose adopted element left its host; call before input
 * or emission reads their nodes. Touches no element of a dropped slot. */
void check_slots(kit *k);
/* Drops the slots this frame did not begin. */
void end_slots(kit *k);
void free_slots(kit *k);

/* ---------- Code editor (kit_code.c) ---------- */

void register_code_element();
/* The cursor's cell in logical pixels, for the IME. */
kit_rect code_caret(const kit *k, node *n);

#endif
