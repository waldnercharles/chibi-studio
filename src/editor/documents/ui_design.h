#ifndef CHIBI_UI_DESIGN_H
#define CHIBI_UI_DESIGN_H

#include "assets/ui_source.h"
#include "editor/documents/ui_buffer.h"

#include <stdbool.h>
#include <stddef.h>

/* The visual designer's changes to a UI document, as Unity's UI Builder and
 * Unreal's UMG Designer make them, but written as text: each is a ui_source
 * edit of the buffer's RML or RCSS part (ui_source_parse_block), applied as
 * one undo step, so the author's formatting survives and the Text Editor,
 * the designer share one history.
 *
 * Elements are indexes in the RML part's outline, rules and declarations in
 * the RCSS part's. A change that cannot be made returns -1 or false with why
 * in `error` and leaves the buffer alone. */

/* The RML or RCSS part parsed; free with ui_source_free. */
ui_source *ui_design_parse(const ui_buffer *b, ui_part part);

/* How many elements `element` holds, itself included: its descendants follow
 * it in document order. */
int ui_design_extent(const ui_source *source, int element);

/* Structure. Each returns the element to select afterwards: the one moved,
 * inserted, copied or wrapping, or after a delete its next sibling, else its
 * previous one, else its parent (-1 at the top level). */
int ui_design_move(ui_buffer *b, int element, int target, ui_source_place place, char *error, size_t size);
int ui_design_insert(
    ui_buffer *b,
    int target,
    ui_source_place place,
    const char *markup,
    char *error,
    size_t size
);
int ui_design_duplicate(ui_buffer *b, int element, char *error, size_t size);
/* Puts the element inside a new `tag` element in its place. */
int ui_design_wrap(ui_buffer *b, int element, const char *tag, char *error, size_t size);
int ui_design_delete(ui_buffer *b, int element, char *error, size_t size);

/* Attributes. A NULL value removes the attribute or style property. With
 * `join`, the change replaces the undo step the last call made, so a scrub
 * or a picker session stays one step. */
bool ui_design_set_attribute(
    ui_buffer *b,
    int element,
    const char *name,
    const char *value,
    char *error,
    size_t size
);
bool ui_design_class(ui_buffer *b, int element, const char *name, bool on, char *error, size_t size);
bool ui_design_set_style(
    ui_buffer *b,
    int element,
    const char *property,
    const char *value,
    bool join,
    char *error,
    size_t size
);
/* Sets several of the element's inline style properties as one undo step,
 * as a Canvas resize sets its width and height. */
typedef struct ui_design_style
{
    const char *property;
    const char *value;
} ui_design_style;
bool ui_design_set_styles(
    ui_buffer *b,
    int element,
    const ui_design_style *styles,
    int count,
    char *error,
    size_t size
);
/* Replaces the text an element holds when it holds no elements. */
bool ui_design_set_text(ui_buffer *b, int element, const char *text, char *error, size_t size);
/* Replaces the value of declaration `declaration` of rule `rule`. */
bool ui_design_set_declaration(
    ui_buffer *b,
    int rule,
    int declaration,
    const char *value,
    bool join,
    char *error,
    size_t size
);
/* Adds `property: value;` after the rule's last declaration: on a line of
 * its own, indented as the rule's lines are, when the rule spans lines. */
bool ui_design_add_declaration(
    ui_buffer *b,
    int rule,
    const char *property,
    const char *value,
    char *error,
    size_t size
);
/* Removes a rule's declaration, and its line when it has one of its own. */
bool ui_design_remove_declaration(ui_buffer *b, int rule, int declaration, char *error, size_t size);
/* Extract to class: the element's inline style becomes a new rule for class
 * `name` at the end of the RCSS, written as its rules are, and the element
 * takes the class in place of its style attribute; one undo step over both
 * parts. */
bool ui_design_extract_class(ui_buffer *b, int element, const char *name, char *error, size_t size);

/* ---------- Keyboard reordering ---------- */

/* Whether elements dropped on it go inside it: tags that hold no content,
 * as input and img, do not. */
bool ui_design_container(const ui_source *source, int element);

/* A place a carried element drops, as ui_design_move takes it: before an
 * element, after the last of its parent's children, or inside a container
 * holding none (a target of -1 is the markup). */
typedef struct ui_design_slot
{
    int target;
    ui_source_place place;
} ui_design_slot;

/* The places the Hierarchy's keyboard sensor steps through with the arrows,
 * as dnd-kit's sortable tree does, in the order its rows show them: each
 * gap between rows at each depth, leaving out the carried `element` with
 * what it holds and what folded elements hold (`folded[i]` for element i,
 * or NULL for none). Appends them to `slots` and returns the index of the
 * one where the element lies now. */
int ui_design_slots(const ui_source *source, int element, const bool *folded, ui_design_slot **slots);

/* ---------- Selection ---------- */

/* The line of an element's data-src, "<asset>:<line>", when it names
 * `asset`; 0 otherwise. */
int ui_design_source_line(const char *src, const char *asset);
/* The `nth` (from 0) child of `parent` (-1: the markup's top level) whose
 * tag starts on `line`: the element a shown element was written as, since
 * shown elements carry only their line. -1 when there is none. */
int ui_design_child_on_line(const ui_source *source, int parent, int line, int nth);
/* The element a cursor at `offset` selects: the innermost one holding it,
 * or, in a line's leading space, the first one starting on the line; -1. */
int ui_design_element_at(const ui_source *source, int offset);
/* Whether the element holds text and no elements, so it can be retyped. */
bool ui_design_text_only(const ui_source *source, int element);
/* Element `element` of `before` in `after`, the same markup once changed by
 * an undo, a redo or typing: found where it starts when the change left its
 * start alone; -1 when the change rewrote its start. */
int ui_design_follow(const ui_source *before, const ui_source *after, int element);

#endif
