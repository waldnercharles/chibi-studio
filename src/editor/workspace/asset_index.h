#ifndef CHIBI_EDITOR_ASSET_INDEX_H
#define CHIBI_EDITOR_ASSET_INDEX_H

#include "authoring/reference_index.h"

/* Studio's asset index: the project's reference index, rebuilt from content
 * files and the authored scene, prototype, graph and tab state, unsaved
 * edits included. Unsaved assets are keyed by their tab or definition. */
struct app_scene_authoring;
int app_scene_authoring_refresh_asset_index(struct app_scene_authoring *scene);
const reference_index *app_scene_authoring_asset_index(const struct app_scene_authoring *scene);
int app_scene_authoring_open_asset(struct app_scene_authoring *scene, int asset);

#endif
