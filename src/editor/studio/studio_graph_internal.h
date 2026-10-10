#ifndef CHIBI_EDITOR_STUDIO_GRAPH_INTERNAL_H
#define CHIBI_EDITOR_STUDIO_GRAPH_INTERNAL_H

/* Shared by the graph canvas modules (studio_graph*.c). */
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor_internal.h"
#include "graph/authoring/align.h"
#include "graph/authoring/layout.h"

enum
{
    CODE_LINES = 12,
};

static const float ROW = 22; /* graph.rcss row pitch */
static const float SOCKET_PAD = 10, PART_GAP = 6, EDGE = 1;
/* Bind to Source beside a compound value: an 18 dp button and its gap. */
static const float BIND_WIDTH = 24;
/* Below this many device pixels per graph unit only titles show: 12 dp pin
   labels would draw under 7 px. Zoom alone would hide legible text on HiDPI. */
static const float FAR_PIXELS = 0.6f;
/* A graph opening in a small area shows its start at no less than this zoom
   instead of all of it as unreadable titles; expression code wraps to fit
   at it. */
static const float OPEN_ZOOM_MIN = 0.75f;

typedef enum
{
    FORM_FULL,
    FORM_OPERATOR,
    FORM_GETTER,
} node_form;

struct studio_node_layout
{
    uint64_t id;
    node_form form;
    float w, h;
    float label;           /* input label column, so input editors line up */
    bool label_clipped;    /* that column was narrowed to keep NODE_MAX */
    float top;             /* the first row's top, below the node's */
    float column;          /* an operator's input column */
    int head_in, head_out; /* pins drawn in the title, or -1 */
    int code_lines;        /* an expression's source lines */
    int add_pin;           /* a last row adds a pin to this side, or -1 */
};
typedef struct studio_node_layout node_layout;

/* World calculations survive camera movement. Their model and presentation
 * versions are checked before use; routing positions are checked separately. */
typedef struct
{
    node **nodes;
    arrange_box *boxes;
    arrange_point *pos;
    arrange_link *links;
    int *edge_link;
} studio_graph_route_input;

struct studio_graph_calculations
{
    uint64_t lifetime, topology;
    unsigned edit, assets, fonts;
    const behavior_asset *asset;
    editor_choices_resolver choices;
    void *choices_user;
    int forms, standard_family, code_family;
    float standard_size, code_size, pixel_ratio;
    bool comfortable, routes_dirty, unused_dirty;
    int *layout_index;
    uint64_t *definitions; /* lifetime/revision pairs of the owner's graphs */
    studio_graph_route_input routing;
    unsigned measurements, route_preparations, reachability_builds;
};
void sg_calculations_sync(const studio *s);
void sg_calculations_invalidate(studio *s);
void sg_calculations_free(studio *s);

/* A graph Back and Forward return to (studio_graph_nav.c), or with kind
 * APP_EDITOR_DOCUMENT_UI a place in the Text Editor (workstream Q). */
struct studio_visit
{
    int kind; /* app_editor_document_kind */
    char document[128];
    char path[512]; /* a System's file */
    uint64_t graph_id;
    int caller_count; /* the calls entered to reach it, for the breadcrumb */
    ed_graph_caller callers[ED_CALLER_LIMIT];
    ui_code_place place; /* a UI document's */
};

typedef enum
{
    VALUE_NONE,
    VALUE_CHOICE,
    VALUE_BOOL,
    VALUE_COLOR,
    VALUE_VECTOR,
    VALUE_TEXT,
    VALUE_NUMBER,
    VALUE_COMPOUND,
    VALUE_CHIP,
} value_kind;

typedef struct
{
    const pin_spec *spec;
    bool connected;
    bool inline_source;
    bool show_label;
    bool source_select;
    char label[128];
    char chip[160];
    uint32_t chip_icon;
    value_kind value;
} pin_parts;

/* Canvas menus (studio_graph_menu.c): a menu is written once, as the kit's
 * immediate menus were. Described for GraphMenu.rml, each item adds an entry
 * and returns false; run for the key a click chose, only that item returns
 * true, so its code acts. Keys are unique within their menu or submenu. */
typedef struct sg_menu sg_menu;
bool sg_menu_item(
    sg_menu *m,
    const char *key,
    const char *label,
    uint32_t icon,
    const char *shortcut,
    bool disabled
);
/* A color choice: a dot tinted `swatch` (0xRRGGBBAA), marked when current. */
bool sg_menu_swatch(sg_menu *m, const char *key, const char *label, uint32_t swatch, bool current, bool disabled);
/* A separator above the next item. */
void sg_menu_separator(sg_menu *m);
/* The menu's heading. */
void sg_menu_title(sg_menu *m, const char *title);
/* Items until sg_submenu_end open beside this row. */
void sg_submenu_begin(sg_menu *m, const char *key, const char *label, uint32_t icon);
void sg_submenu_end(sg_menu *m);
/* A node's menu (studio_graph.c). */
void sg_node_menu(studio *s, sg_menu *m, node *n);

/* Model access (studio_graph.c). */
editor *sg_graph_editor(const studio *s);
graph *sg_active_graph(const studio *s);
bool sg_graph_readonly(const studio *s);
/* Why the graph is read-only, for its header and refusals; NULL when it
   isn't. */
const char *sg_readonly_reason(const studio *s);
/* An edit the read-only graph refused says why, in one toast however often
   it is tried. */
void sg_refuse_edit(studio *s);
/* Whether a wire is being dragged from a pin: its press has moved past the
   drag threshold, so a plain click on a pin changes nothing on screen. */
bool sg_link_dragging(const studio *s);
bool sg_node_visible(const graph *g, const node *n);
node *sg_node_by_id(const studio *s, uint64_t id);
graph_comment_box *sg_comment_by_id(const studio *s, uint64_t id);
void sg_link_ref_of(const edge *ed, ed_link_ref *out);
bool sg_edge_ends(const graph *gr, const edge *ed, node **from, int *from_pin, node **to, int *to_pin);

/* Node layout and measurement (studio_graph_layout.c). */
bool sg_pin_exec(const node *n, int kind, int pin);
bool sg_in_head(const node_layout *l, int kind, int pin);
bool sg_pin_in_rows(const graph *g, const node *n, const node_layout *l, int kind, int pin);
int sg_pin_order(const graph *g, const node *n, const node_layout *l, int kind, int pin);
int sg_exec_rows(const graph *g, const node *n, const node_layout *l);
int sg_row_count(const graph *g, const node *n, const node_layout *l);
const char *sg_node_kind(const studio *s, const node *n);
uint32_t sg_node_icon(const studio *s, const node *n);
value_kind sg_value_kind_of(const studio *s, node *n, int output, int p);
void sg_pin_parts_of(const studio *s, node *n, int kind, int p, pin_parts *out);
const char *sg_node_title(const studio *s, node *n, node_form form, char *buf, size_t size);
int sg_code_lines(const studio *s, const char *code, float width, int starts[CODE_LINES]);
float sg_code_room(const node_layout *l);
float sg_code_height(const node_layout *l);
float sg_text_width(const studio *s, const char *text, float size, int weight);
const node_layout *sg_layout_of(const studio *s, node *n);
float sg_pin_y(const studio *s, node *n, int kind, int pin);
bool sg_node_box(const studio *s, node *n, float box[4]);
void sg_socket_at(const studio *s, node *n, int kind, int pin, float *x, float *y);

/* Camera and arrangement (studio_graph_view.c). */
float sg_zoom(const studio *s);
float studio_graph_pixel_ratio(const studio *s);
void sg_to_world(const studio *s, float wx, float wy, float *x, float *y);
void sg_zoom_at(studio *s, float zoom, float wx, float wy);
/* Zoomed out past FAR_PIXELS: nodes show titles only. */
bool sg_far(const studio *s);
void sg_camera_step(studio *s);
void sg_frame_nodes(studio *s, bool selection, bool animate);
/* The first view of a graph: fitted, or its start at a legible zoom. */
void sg_frame_open(studio *s);
/* Frames the selection holding n, or pans n into view unless it is in view. */
void sg_show_node(studio *s, node *n, bool frame);
void sg_follow_graph(studio *s);
/* The open graph keeps its camera though its asset was renamed. */
void sg_keep_graph_view(studio *s);
void sg_update_routes(studio *s);
void sg_arrange(studio *s, bool automatic_only);

/* Edits (studio_graph_edit.c). */
void sg_comment_contents(studio *s, uint64_t id, bool frame_only);
void sg_frame_selection(studio *s);
bool sg_connect_pins(studio *s, node *a, int a_kind, int a_pin, node *b, int b_kind, int b_pin);
bool sg_reconnect_pins(studio *s, node *to, int pin);
bool sg_pins_compatible(const node *a, int a_kind, int a_pin, const node *b, int b_kind, int b_pin);
int sg_body_target(const graph *gr, const node *from, int kind, int pin, const node *to);
/* A warning toast for an edit the graph refused, recorded in Output. */
void sg_show_error(studio *s, const char *title, const char *message);
void sg_select_only(editor *e, node *n);
void sg_delete_selection(studio *s);
void sg_copy_selection(studio *s);
void sg_paste(studio *s);
bool sg_is_unused(const studio_graph *g, uint64_t id);
void sg_find_unused(studio *s);
/* Collapse to Function (Ctrl+G) or to Macro, and Expand Node, each one undo
   step; a refusal shows its reason. */
void sg_collapse(studio *s, graph_kind kind);
void sg_expand_node(studio *s, node *call);
/* Whether the selection holds a library call to refresh. */
bool sg_refreshable(const studio *s);
void sg_refresh_nodes(studio *s);
void sg_convert_to_expression(studio *s);
void sg_optimize(studio *s);
void sg_collapse_math(studio *s);
/* Selection layout, each one undo step. Straightening moves each selected
   node level with the wire that drives it, left to right (Unreal's Q). */
void sg_align(studio *s, align_edge edge);
void sg_distribute(studio *s, bool vertical);
void sg_straighten(studio *s);
/* Moves the wire's target level with its source. */
void sg_straighten_wire(studio *s, int edge);
/* Adds the nodes wired to the selection's inputs or outputs: one step, or
   with `all` every node reached. */
void sg_select_linked(studio *s, bool upstream, bool all);
/* Places a catalog node by name, as the add-node search would. */
bool sg_spawn_node(studio *s, const char *name, float x, float y);

/* Pins (studio_graph_pins.c). */
int sg_pin_links(const graph *gr, const node *n, int kind, int pin);
/* Alt+click: every wire at the pin, as one undo step. */
void sg_break_links(studio *s, node *n, int kind, int pin);
void sg_add_run_pin(studio *s, node *n);
void sg_pin_menu(studio *s, sg_menu *m, node *n, int kind, int pin);
bool sg_pin_renameable(const studio *s, const node *n, int kind, int p);
void sg_rename_pin(studio *s, const node *n, int p);
void sg_emit_pin_edit(studio *s, node *n, int p);

/* Input (studio_graph_input.c). */
void sg_replay(studio *s);
void sg_pointer(studio *s);
void sg_shortcuts(studio *s);
void sg_cancel_gesture(studio *s);

/* Comment frames and node comments (studio_graph_comment.c). */
enum
{
    SG_EDGE_LEFT = 1,
    SG_EDGE_TOP = 2,
    SG_EDGE_RIGHT = 4,
    SG_EDGE_BOTTOM = 8,
};
void sg_emit_comment(studio *s, graph_comment_box *box);
void sg_comment_menu(studio *s, sg_menu *m, graph_comment_box *box);
/* A frame's color as its swatch shows it: the canvas's own when it has none. */
uint32_t sg_frame_color(const graph_comment_box *box);
/* Sets a node's comment and whether it hides, as one undo step. */
void sg_set_node_comment(studio *s, node *n, const char *text, bool hidden);
bool sg_set_node_disabled(studio *, node *, bool disabled);
void sg_edit_title(studio *s, uint64_t comment);
void sg_begin_resize(studio *s, graph_comment_box *box, unsigned edges);
void sg_resize(studio *s);
void sg_end_resize(studio *s, bool cancel);
const char *sg_resize_cursor(unsigned edges);
void sg_emit_bubble(studio *s, node *n);
void sg_edit_bubble(studio *s, uint64_t node_id);
bool sg_node_comment_items(studio *s, sg_menu *m, node *n);

/* Find in graph (studio_graph_find.c). */
void sg_find_panel(studio *s);

/* Navigation (studio_graph_nav.c). */
/* Records the open graph and the calls entered to reach it as a visit once
   it differs from the one last seen; the Behavior header does as it shows. */
void sg_nav_track(studio *s);
void sg_nav_keys(studio *s);

/* Live values (studio_graph_live.c). */
typedef struct studio_live_value
{
    uint64_t node, pin;
    bool output, found;
    bool color;      /* a color pin: the text is its hex, beside a swatch */
    uint32_t swatch; /* 0xRRGGBBAA */
    char text[48];
} studio_live_value;
/* The value a pin last carried in the debug target, compact, read at most
   ten times a second; false while it carried none. */
bool sg_live_value(studio *s, node *n, int output, int pin, studio_live_value *out);
/* Whether the pin shows as a color: flagged so, or a reflected color field. */
bool sg_color_pin(const node *n, const pin_spec *spec);
/* A color pin's value as 0xRRGGBBAA. */
uint32_t sg_literal_color(const pin_spec *spec, const pin_value *value);

/* Literals (studio_graph_literals.c), shared by the canvas's controls and
   Properties. */
typedef enum
{
    SG_STEP_ONCE,   /* a discrete edit: an Undo step of its own */
    SG_STEP_LIVE,   /* a gesture runs: its first edit opens the step */
    SG_STEP_COMMIT, /* a gesture ends, in its step, or one of its own */
} sg_step;
/* Writes a pin's default as `step` says; false when the graph refused it. */
bool sg_set_literal(studio *s, node *n, int output, int p, const pin_value *value, sg_step step);
double sg_literal_number(const pin_value *value);
void sg_set_literal_number(pin_value *value, double number);
/* A number type's range; unbounded types leave min and max as they are. */
void sg_literal_bounds(pin_type type, double *min, double *max);
double sg_literal_alpha(const pin_spec *spec, const pin_value *value);
bool sg_write_literal_color(const pin_spec *spec, pin_value *value, arena *scratch, uint32_t rgba);
void sg_write_literal_alpha(const pin_spec *spec, pin_value *value, arena *scratch, double alpha);
/* A color ramp's stops, which stay ordered by position. */
bool sg_ramp_stops(const pin_spec *shape);
/* Where stop `index` may move, between its neighbours; false when they
   leave it no room. */
bool sg_ramp_range(const pin_array *array, int index, double *min, double *max);
bool sg_array_can_add(const pin_value *value, const pin_spec *shape);
/* The array with item `remove` gone, or one added with `append` (a ramp's
   between its widest gap's stops); NULL when it cannot be. */
pin_array *sg_array_edit(arena *scratch, const pin_value *value, const pin_spec *shape, int remove, bool append);

/* Rendering (studio_graph_draw.c). */
/* A pin's socket color as 0xRRGGBBAA. */
uint32_t sg_pin_color(const studio *s, const pin_spec *spec);
const char *sg_material_picker(studio *s, kit *k, const char *value, unsigned flags);
void sg_emit_value_in(studio *s, kit *k, node *n, int output, int p);
/* One editable graph input: its typed constant, binding, inline source, or wire. */
void sg_emit_graph_input(studio *s, kit *k, node *n, int p);
void sg_begin_expression_edit(studio *s, kit *k, node *expression, uint64_t owner_pin);
bool sg_expression_editing(const studio *s, const kit *k, const node *expression, uint64_t owner_pin);
void sg_emit_expression_editor(studio *s, kit *k, node *expression, uint64_t owner_pin);
int sg_set_expression_source(studio *s, node *expression, const char *source);
void sg_emit_canvas(studio *s);

/* ---------- Properties' Node and Member tabs (workstream G1) ----------
 * Rows of graph_details (studio_graph_details.c), which the tabs write as a
 * sync walks them; see that file for the variable. */
#define GD_ROW_ID GD_ROW_ID_MAX
typedef struct gd_view gd_view;
typedef struct gd_rows
{
    gd_view *v;
    yyjson_mut_val *list, *panel;
    size_t count;
    bool open;
} gd_rows;
/* What every row shows; `control` names its control and `def` a param
   row's default's. A heading heads a popover's rows; `removable` gives an
   item its remove button, or an array its Add item; `fixed` keeps a param's
   container. */
typedef struct gd_row
{
    const char *id, *label, *about, *control, *def;
    bool readonly, invalid, heading, removable, fixed;
    bool bindable; /* offers Bind to Source, which shows its source field */
} gd_row;
/* How a number edits: Base UI's NumberField attributes. */
typedef struct gd_number_shape
{
    double min, max, step;
    int precision;
    bool integer, single;
} gd_number_shape;
void gd_identity(gd_view *v, uint32_t icon, const char *name, const char *of);
void gd_empty(gd_view *v, const char *text);
/* A panel, open as last folded; write rows while open, then end it. */
gd_rows gd_panel(gd_view *v, const char *key, const char *title);
/* The panel's add button below its rows, which sends addPin with `id`. */
void gd_panel_add(gd_rows *r, const char *id, const char *label, bool disabled);
void gd_panel_end(gd_rows *r);
yyjson_mut_val *gd_next_row(gd_rows *r, const gd_row *d);
void gd_put_text(gd_rows *r, yyjson_mut_val *row, const char *key, const char *text);
void gd_put_flag(gd_rows *r, yyjson_mut_val *row, const char *key, bool on);
void gd_put_int(gd_rows *r, yyjson_mut_val *row, const char *key, int n);
void gd_put_number(gd_rows *r, yyjson_mut_val *row, double value, const gd_number_shape *shape);
void gd_put_alpha(gd_rows *r, yyjson_mut_val *row, double alpha);
void gd_put_axes(gd_rows *r, yyjson_mut_val *row, const double *values, int count, const gd_number_shape *shape);
/* A choice's options, each valued by its place, `current` that of the
   value; listed only while the choice is open. */
void gd_put_choices(gd_rows *r, yyjson_mut_val *row, const editor_choices *choices, int current);
/* An asset field's kind, picker and problem; the problem, when no asset has
   its name, also goes in `problem` for the row's `invalid`. */
void gd_put_asset(gd_rows *r, yyjson_mut_val *row, reference_asset_kind kind, const char *value);
bool gd_asset_problem(studio *s, reference_asset_kind kind, const char *value, char problem[192]);
/* A type's swatch, as Members colors it, and its label. */
void gd_put_type(gd_rows *r, yyjson_mut_val *row, const pin_spec *pin, const char *label);
/* The rows of a row's popover. */
gd_rows gd_sub(gd_rows *r, yyjson_mut_val *row);
void gd_sub_end(gd_rows *r);
/* Intents changed what the tabs show. */
void gd_changed(studio *s);

/* The Node tab (studio_graph_properties.c). */
void studio_node_details(studio *s, gd_view *v);
void studio_node_details_action(studio *s, const char *action, const ui_activation *a);
/* An input's row: its source field, chip or value editor. */
void sg_input_row(gd_rows *r, studio *s, node *n, int p);
/* The Member tab (studio_member_properties.c). */
void studio_member_details(studio *s, gd_view *v);
void studio_member_details_action(studio *s, const char *action, const ui_activation *a);

/* How Properties shows an input (studio_graph_values.c), as the canvas does:
   execution, its source field, a chip or its value editor; `text` is the
   field's or the chip's. */
typedef enum
{
    SG_INPUT_NONE,
    SG_INPUT_EXEC,
    SG_INPUT_SOURCE,
    SG_INPUT_CHIP,
    SG_INPUT_VALUE,
} sg_input_kind;
sg_input_kind sg_input_view(studio *s, node *n, int p, char *text, size_t size);
/* Whether input p's value is a vector, color or compound that Bind to
   Source can give a source in place of its editor. */
bool sg_input_bindable(studio *s, node *n, int p);
/* Opens input p's source list under its field, whose row is `row`. */
void sg_source_open(studio *s, node *n, int p, const char *row, rml_element *field);
void sg_source_free(studio *s);

#endif
