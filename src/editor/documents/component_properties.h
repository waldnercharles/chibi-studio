#ifndef CHIBI_COMPONENT_PROPERTIES_H
#define CHIBI_COMPONENT_PROPERTIES_H

#include "assets/prototype.h"
#include "editor/ui/choices.h"

/* How the editor labels and edits reflected component fields. */
bool appearance_color_field(const type_desc *type, const type_field *field);
const editor_choices *property_choices(const type_desc *type, const type_field *field);
bool property_field_visible(
    const ecs_component_desc *component,
    const type_field *field,
    const void *data
);
void property_label(
    const game_prototype_component *component,
    const type_field *field,
    char *label,
    size_t size
);

#endif
