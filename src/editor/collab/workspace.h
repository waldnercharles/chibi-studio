#ifndef CHIBI_COLLAB_WORKSPACE_H
#define CHIBI_COLLAB_WORKSPACE_H

#include "editor/studio/studio.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/scene_authoring.h"

#include <stdbool.h>
#include <stddef.h>

/* Owned JSON for the current editable workspace. */
char *collab_workspace_checkpoint(
    app_scene_authoring *scene,
    editor *graph,
    studio *view,
    const char *project_root
);

/* Restore only into a disposable candidate workspace. */
bool collab_workspace_restore_model(
    app_scene_authoring *scene,
    editor *graph,
    const char *json,
    char *error,
    size_t error_size
);

bool collab_workspace_restore_presentation(studio *view, const char *json, char *error, size_t error_size);

#endif
