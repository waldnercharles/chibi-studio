#include "editor/collab/actions.h"

#include "editor/collab/actions_internal.h"
#include "yyjson.h"

#include <stdio.h>
#include <string.h>

bool collab_actions_apply(
    app_scene_authoring *scene,
    editor *graph_editor,
    const char *json,
    char *error,
    size_t error_size
)
{
    if (!json || !error || !error_size) {
        return false;
    }
    error[0] = 0;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *family = yyjson_get_str(yyjson_obj_get(root, "family"));
    bool result = false;
    if (family && !strcmp(family, "graph")) {
        result = graph_collab_apply_action(graph_editor, json, error, error_size);
    } else if (family && !strcmp(family, "scene")) {
        result = scene_collab_apply_action(scene, json, error, error_size);
    } else {
        snprintf(error, error_size, "Unsupported collaboration action family");
    }
    yyjson_doc_free(doc);
    return result;
}
