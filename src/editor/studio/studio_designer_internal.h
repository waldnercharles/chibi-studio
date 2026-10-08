#ifndef CHIBI_EDITOR_STUDIO_DESIGNER_INTERNAL_H
#define CHIBI_EDITOR_STUDIO_DESIGNER_INTERNAL_H

/* What the visual designer's files share: studio_designer.c keeps the
 * document, the selection, its changes and the Canvas;
 * studio_designer_hierarchy.c the Hierarchy, studio_designer_details.c
 * Details and studio_designer_library.c the Library and the Add menus.
 * Details' capabilities (values, document queries, changes) are plain C in
 * studio_designer_values.c and studio_designer_edit.c
 * (studio_designer_edit.h). */
#include "editor/documents/ui_design.h"
#include "editor/studio/studio_internal.h"
#include "ui/list.h"

/* The drag type of Hierarchy rows ("#<element>") and Library items (their
 * markup). */
#define DESIGNER_DRAG "ui"

/* The edges a Canvas resize handle moves. */
enum
{
    DESIGNER_RESIZE_WIDTH = 1u << 0,
    DESIGNER_RESIZE_HEIGHT = 1u << 1,
};

/* An element's border box, width then height, and the padding and border
 * inside it, in document dp; and whether its width and height size that
 * box (box-sizing: border-box) rather than its content. */
typedef struct designer_box
{
    float size[2], extra[2];
    bool border_box;
} designer_box;

/* The width and height, in whole dp, a resize sets after the pointer moved
 * (dx, dy) document dp: the moved axes grow by it, never under the padding
 * and border; with `aspect` and both axes, the one moved further leads and
 * the other keeps the ratio. */
void designer_resize_css(
    const designer_box *box,
    unsigned axes,
    float dx,
    float dy,
    bool aspect,
    float css[2]
);

/* A row of the Hierarchy: an element, or -1 for the document, and its depth
 * under the document. */
typedef struct tree_row
{
    int element;
    int depth;
} tree_row;

struct studio_designer
{
    char path[1024];       /* the UI document the rest describes */
    char asset[128];       /* its ui asset's name, as data-src names it */
    unsigned parsed;       /* its buffer's revision when parsed */
    ui_source *rml, *rcss; /* its parts */
    int selected;          /* an element of rml, or -1 for the document */
    int hovered; /* the element whose Hierarchy row the pointer is over, or -1 */
    int menu;    /* the element the Hierarchy's menu is for */
    bool reveal_row;      /* scroll the Hierarchy to the selection */
    int reveal_cursor;    /* where to put the Text Editor's cursor, or -1 */
    int cursor;           /* the Text Editor's cursor as last seen */
    unsigned cursor_seen; /* the buffer's revision then */
    bool closed;          /* the Hierarchy folds the document */
    char **folded;        /* places ("0.2.1") of the elements it folds */
    unsigned folds;       /* counts the changes to closed and folded */
    char filter[128];     /* the Hierarchy's */
    tree_row *tree;       /* its rows, the document's first (array) */
    struct designer_tree_seen
    {
        bool built, closed;
        unsigned parsed, folds, generation; /* generation counts the listings */
        char filter[128];
    } tree_seen; /* what `tree` lists */
    struct designer_view_seen
    {
        bool built;
        unsigned generation;
        int first, length;
    } views[STUDIO_AREA_COUNT]; /* each area's window as last written */
    ui_list_state lists[STUDIO_AREA_COUNT]; /* and the List's own state */
    char twisty[16]; /* the row whose twisty folded in the last sync */
    char library_filter[128];
    struct
    {
        bool built;
        uint64_t revision; /* the asset index's when the items were listed */
        char asset[128];   /* the document then, which the widgets leave out */
        char filter[128];
    } library_seen; /* what the `library` source last showed */
    struct
    {
        bool active;  /* a scrub or picker session made the last undo step */
        char key[96]; /* the value it changes */
        unsigned revision; /* the buffer's after its last change */
    } gesture;
    struct
    {
        int toast;         /* offers Undo while the delete is the latest edit */
        unsigned revision; /* the buffer's right after it */
    } undo;
    /* A Canvas resize (Unity UI Builder's handles): the element, the edges
       its handle moves, where the press was, its border box then and the
       padding and border inside it in document dp, the buffer's revision,
       and the inline width and height it had ("" for none), which Escape
       puts back. Only the Canvas shows the new size until release writes it. */
    struct
    {
        bool active;
        int element;
        unsigned axes; /* DESIGNER_RESIZE_WIDTH | DESIGNER_RESIZE_HEIGHT */
        float x0, y0;
        designer_box box;
        bool moved;
        unsigned revision;
        char before[2][64];
        float css[2]; /* the width and height a release writes */
    } resize;
    struct
    {
        int kind, target; /* what the Add popover adds, and to what */
        int focus;        /* frames the field may still ask for the focus */
        char text[192];   /* what is typed in it */
    } add;                /* the Add popover Details' buttons share */
    struct designer_add_seen
    {
        bool built;
        unsigned parsed;
        int kind, target;
        char text[192];
    } add_seen;        /* what the popover's suggestions list */
    char **shut;       /* the keys of the Details panels folded (array) */
    unsigned shuts;    /* counts the changes to shut */
    char choosing[40]; /* the keyword row whose list is open */
    struct designer_details_seen
    {
        bool built;
        unsigned parsed, shuts;
        int selected;
        char search[128], choosing[40];
    } details_seen; /* what the `details` source last showed */
    /* Workstream Q: a Hierarchy row carried with the keyboard, as dnd-kit's
       keyboard sensor carries one: the element, the slot it would drop in
       (an index of ui_design_slots', and that slot), and the buffer's
       revision when it was picked up, since a change to the text puts it
       down. */
    struct
    {
        bool active;
        int element;
        int slot;
        ui_design_slot at;
        unsigned revision;
    } carry;
};

/* The active UI document with markup and its designer state, parsed as its
 * text is now; NULL while no such document is active. */
studio_designer *designer_state(studio *s, ui_buffer **b);
/* Selects element `element` of the current parse (-1 for the document) and
 * reveals it in the Hierarchy and the Text Editor. */
void designer_select(studio *s, int element);
/* Visible bounds of a source element, including expanded widget roots. */
kit_rect studio_designer_element_box(rml_element *element);
/* Moves element `element` to `place` of `target`; selects it, or warns why
 * it cannot. */
void designer_move(studio *s, ui_buffer *b, int element, int target, ui_source_place place);
/* A drop: a row's payload ("#<element>") moves it, a Library item's markup
 * is added. */
void designer_drop(studio *s, const char *payload, int target, ui_source_place place);
/* The selection's changes. */
void designer_duplicate(studio *s);
void designer_wrap(studio *s);
void designer_delete(studio *s);
/* Opens the widget a <use> element shows, beside the document. */
void designer_open_widget(studio *s, int element);

/* The Hierarchy's model (studio_designer_hierarchy.c): folds and the keyboard
 * sensor, which studio_designer_key drives. */
void designer_hierarchy_forget(studio_designer *d);
/* Frees Details' folded panels. */
void designer_details_forget(studio_designer *d);
/* Shows the selection's row: unfolds its ancestors, clears a filter that
 * hides it and scrolls to it. */
void designer_reveal(studio_designer *d);
void designer_step(studio *s, studio_designer *d, int direction);
void designer_fold_selected(studio_designer *d, bool fold);
void designer_pick_up(studio *s, studio_designer *d, const ui_buffer *b);
void designer_carry_key(studio *s, studio_designer *d, ui_buffer *b, SDL_Keycode key);
void designer_keep_carry(studio *s, studio_designer *d, const ui_buffer *b);
/* The element whose Hierarchy row the pointer is over, or -1. */
int designer_hovered_row(studio *s);

/* A change's warning toast. */
void designer_warn(studio *s, const char *title, const char *error);
/* An element's name as the Hierarchy shows it: its tag, or a <use>'s widget. */
void designer_element_name(const ui_source *rml, int element, char *out, size_t size);
uint32_t designer_element_icon(const ui_source *rml, int element);
/* Adds markup for element `target` (-1 the document): inside a container,
 * else after it; selects what it added. */
void designer_add(studio *s, int target, const char *markup, const char *label);

#endif
