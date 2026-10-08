#ifndef CHIBI_SCENE_RETENTION_H
#define CHIBI_SCENE_RETENTION_H

#include "assets/prototype.h"

#include <stdbool.h>

typedef struct
{
    char *name;
    av_value *values;
    av_value *defaults;
    bool automatic;
} scene_retained_component;

void scene_retention_remove(scene_retained_component **components, const char *name);
void scene_retention_free(scene_retained_component *components);
av_value *scene_retention_save(const scene_retained_component *components);
/* Atomic replacement. NULL value means no retained components. */
int scene_retention_load(av_value *value, scene_retained_component **components);
bool scene_retention_validate(
    const scene_retained_component *components,
    const game_prototype_registry *registry
);

#endif
