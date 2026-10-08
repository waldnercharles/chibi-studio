#ifndef CHIBI_COLLAB_ACTIONS_H
#define CHIBI_COLLAB_ACTIONS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct app_scene_authoring app_scene_authoring;
typedef struct editor editor;

/* Applies one accepted graph or scene action through its normal editor APIs.
 * The caller owns the replay guard around this function. */
bool collab_actions_apply(
    app_scene_authoring *scene,
    editor *graph_editor,
    const char *json,
    char *error,
    size_t error_size
);

#endif
