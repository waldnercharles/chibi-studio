#include "editor/collab/workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

char *collab_workspace_checkpoint(app_scene_authoring *scene, editor *graph, studio *view, const char *project_root)
{
    char *scene_json = app_scene_authoring_checkpoint(scene);
    char *graph_json = editor_checkpoint(graph);
    char *presentation_json = studio_collab_capture(view);
    if (!scene_json || !graph_json || !presentation_json || !project_root) {
        free(scene_json);
        free(graph_json);
        free(presentation_json);
        return NULL;
    }

    yyjson_doc *presentation_doc = yyjson_read(presentation_json, strlen(presentation_json), 0);
    yyjson_val *presentation = presentation_doc
                                 ? yyjson_doc_get_root(presentation_doc)
                                 : NULL;
    if (!yyjson_is_obj(presentation)) {
        yyjson_doc_free(presentation_doc);
        free(scene_json);
        free(graph_json);
        free(presentation_json);
        return NULL;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", 1);
    yyjson_mut_obj_add_str(doc, root, "project", project_root);
    yyjson_mut_obj_add_str(doc, root, "scene", scene_json);
    yyjson_mut_obj_add_str(doc, root, "graph", graph_json);
    yyjson_mut_obj_add_str(doc, root, "presentation", presentation_json);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(presentation_doc);
    free(scene_json);
    free(graph_json);
    free(presentation_json);
    return json;
}

static bool invalid(char *error, size_t error_size, const char *message)
{
    if (error_size) {
        snprintf(error, error_size, "%s", message);
    }
    return false;
}

bool collab_workspace_restore_model(
    app_scene_authoring *scene,
    editor *graph,
    const char *json,
    char *error,
    size_t error_size
)
{
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *project_value = yyjson_obj_get(root, "project");
    yyjson_val *scene_value = yyjson_obj_get(root, "scene");
    yyjson_val *graph_value = yyjson_obj_get(root, "graph");
    yyjson_val *presentation_value = yyjson_obj_get(root, "presentation");
    const char *scene_json = yyjson_get_str(scene_value);
    const char *graph_json = yyjson_get_str(graph_value);
    bool valid = yyjson_is_obj(root) &&
                 yyjson_get_int(yyjson_obj_get(root, "version")) == 1 &&
                 yyjson_is_str(project_value) &&
                 yyjson_get_str(project_value) && yyjson_is_str(scene_value) &&
                 scene_json && yyjson_is_str(graph_value) && graph_json &&
                 yyjson_is_str(presentation_value) &&
                 yyjson_get_str(presentation_value);
    if (!valid) {
        yyjson_doc_free(doc);
        return invalid(error, error_size, "Invalid collaboration workspace checkpoint");
    }

    if (app_scene_authoring_restore_checkpoint(scene, scene_json) != 0 ||
        !editor_restore_checkpoint(graph, graph_json)) {
        yyjson_doc_free(doc);
        return invalid(error, error_size, "Could not restore scene or graph checkpoint");
    }
    app_scene_authoring_set_editor_working(scene, editor_behavior_asset(graph));
    yyjson_doc_free(doc);
    return true;
}

bool collab_workspace_restore_presentation(studio *view, const char *json, char *error, size_t error_size)
{
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *presentation_json = yyjson_is_obj(root)
                                      ? yyjson_get_str(yyjson_obj_get(root, "presentation"))
                                      : NULL;
    bool restored = presentation_json &&
                    studio_collab_restore(view, presentation_json);
    yyjson_doc_free(doc);
    if (!restored) {
        return invalid(error, error_size, "Invalid or unsupported Studio presentation");
    }
    return true;
}
