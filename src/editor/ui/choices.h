#ifndef CHIBI_EDITOR_CHOICES_H
#define CHIBI_EDITOR_CHOICES_H

#include "graph/model/node.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct editor_choice
{
    int value;
    const char *label;
    const char *name; /* Stable value for PIN_NAME catalogs; NULL for integers. */
    const char *group;
    uint32_t icon;
} editor_choice;

typedef struct editor_choices
{
    const editor_choice *items;
    int count;
    const enum_desc *enumeration;
} editor_choices;

typedef const editor_choices
    *(*editor_choices_resolver)(const node *, int output, int pin, void *user);

/* Catalogs are borrowed; labels are nonempty and values unique. Unknown values
 * remain unchanged until the user selects an item. */
int editor_choices_count(const editor_choices *choices);
editor_choice editor_choice_at(const editor_choices *choices, int index);
const editor_choices *editor_choices_for_enum(const char *type_name);

#endif
