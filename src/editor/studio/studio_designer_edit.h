#ifndef CHIBI_EDITOR_STUDIO_DESIGNER_EDIT_H
#define CHIBI_EDITOR_STUDIO_DESIGNER_EDIT_H

/* The visual designer's editing capabilities (workstream D1): what a value
 * is and which control it takes (studio_designer_values.c), what the
 * document holds (attributes, the inline style, the rules that match an
 * element, what an Add popover suggests) and each change a panel can make
 * to it (studio_designer_edit.c).
 *
 * They are plain C over the parsed document (`ui_source`) and `ui_design`'s
 * undoable text edits: no JSON, no RmlUi elements, no activations and no
 * toasts. A change returns what happened and, when the document would not
 * take it, why, so a panel's glue only reads its controls, calls one of
 * these and shows the result, and each can become a behavior node. */
#include "editor/studio/studio_designer_internal.h"

/* ---------- What a value is ---------- */

typedef enum
{
    DESIGNER_VALUE_TEXT,
    DESIGNER_VALUE_LENGTH,
    DESIGNER_VALUE_NUMBER,
    DESIGNER_VALUE_COLOR,
    DESIGNER_VALUE_KEYWORD,
} designer_value_kind;

/* An RCSS property whose values take a control, and for a keyword the ones
 * it takes. */
typedef struct
{
    const char *name;
    designer_value_kind kind;
    const char *const *keywords;
    int count;
} designer_property_type;

/* What a property's value shows as: the control its type calls for, and
 * what that control reads. */
typedef struct
{
    const char *control; /* text, number, color, choice or toggle */
    const designer_property_type *type; /* a keyword's */
    int keyword;                        /* and which of them the value is */
    char number[48], unit[8], color[16];
    double step, min, max;
    int precision;
    bool integer, slider;
} designer_value_shape;

/* The control `value` of `property` is shown in. A length scrubs with its
 * unit, opacity is a slider, a value its type calls no control for, as
 * var(--x), is text. */
designer_value_shape designer_value_shape_of(const char *property, const char *value);
/* What a control sends back (`argument`) as the text the document gets, for
 * the value `current`; false when that control takes no such value. */
bool designer_value_text(
    const char *property,
    const char *current,
    const char *argument,
    char *out,
    size_t size
);
/* What a new property of this name starts with. */
const char *designer_default_value(const char *property);

/* ---------- The document, read ---------- */

/* An attribute of `element` by name, or NULL; its value as written (which
 * writes back unchanged) and its name. */
const ui_source_attribute *designer_attribute(const ui_source *rml, int element, const char *name);
void designer_attribute_text(const ui_source *rml, const ui_source_attribute *a, char *out, size_t size);
void designer_attribute_name(const ui_source *rml, const ui_source_attribute *a, char *out, size_t size);
/* Whether a row of its own shows the attribute `name` of an element `tag`
 * (id, class, style and a <use>'s ui). */
bool designer_own_attribute(const char *tag, const char *name);
/* The text of an element holding text alone, without the space around it. */
void designer_element_text(const ui_source *rml, int element, char *out, size_t size);

/* One declaration of an inline style. */
typedef struct
{
    char property[128], value[256];
} designer_declaration;

/* The inline style's declarations, in order. The caller afrees the array. */
designer_declaration *designer_style_declarations(const ui_source *rml, int element);
/* Whether the rule `selector` of the document's RCSS matches `element`. */
bool designer_rule_matches(const ui_source *rml, const char *selector, int element);

/* What an Add popover adds to: the element, or for a declaration its rule
 * (workstream Q); Extract to Class names the class the inline style moves
 * to. */
typedef enum
{
    DESIGNER_ADD_CLASS,
    DESIGNER_ADD_ATTRIBUTE,
    DESIGNER_ADD_STYLE,
    DESIGNER_ADD_DECLARATION,
    DESIGNER_ADD_EXTRACT,
} designer_add_kind;

/* What an Add popover suggests for the text typed so far, for `target` (an
 * element, or for a declaration its rule); none for a target the document
 * lacks. The caller frees each and afrees the array. */
char **designer_suggestions(const studio_designer *d, int target, designer_add_kind kind, const char *typed);

/* ---------- Targets ---------- */

/* What a row of Details edits. */
typedef enum
{
    DESIGNER_TARGET_NONE,
    DESIGNER_TARGET_ATTRIBUTE,   /* the selection's attribute `index` */
    DESIGNER_TARGET_ID,          /* its id */
    DESIGNER_TARGET_TEXT,        /* its text */
    DESIGNER_TARGET_WIDGET,      /* the widget a <use> shows */
    DESIGNER_TARGET_STYLE,       /* its inline `property` */
    DESIGNER_TARGET_DECLARATION, /* declaration `index` of rule `rule` */
} designer_target_kind;

typedef struct
{
    designer_target_kind kind;
    int index, rule;
    char property[128];
} designer_target;

/* The text a target's value is in the document now; false when it names
 * nothing there. */
bool designer_target_text(const studio_designer *d, const designer_target *t, char *out, size_t size);

/* ---------- Changes ---------- */

/* What a change did: nothing to do (the value is the one it has, or names
 * nothing), made, or refused by the document, whose reason is in `error`. */
typedef enum
{
    DESIGNER_UNCHANGED,
    DESIGNER_CHANGED,
    DESIGNER_REFUSED,
} designer_change;

/* A text field's commit of `text`. */
designer_change designer_set_text(
    studio_designer *d,
    ui_buffer *b,
    const designer_target *t,
    const char *text,
    char *error,
    size_t size
);
/* A control's value: what a scrub, a picker or a list sends as it moves
 * (`active`) and as it lets go. The changes of one gesture are one Undo
 * step; `canceled` (Escape) undoes them. */
designer_change designer_set_value(
    studio_designer *d,
    ui_buffer *b,
    const designer_target *t,
    const char *argument,
    bool active,
    bool canceled,
    char *error,
    size_t size
);
/* Takes out an attribute, an inline property or a declaration. */
designer_change designer_remove_target(
    studio_designer *d,
    ui_buffer *b,
    const designer_target *t,
    char *error,
    size_t size
);
/* Adds what an Add popover's `text` names to `target`: an element, or a
 * declaration's rule. */
designer_change designer_add_value(
    studio_designer *d,
    ui_buffer *b,
    int target,
    designer_add_kind kind,
    const char *text,
    char *error,
    size_t size
);

#endif
