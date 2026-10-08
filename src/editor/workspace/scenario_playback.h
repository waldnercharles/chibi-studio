#pragma once

#include "editor/workspace/scene_authoring.h"

#include <stdbool.h>
#include <stddef.h>

/* Studio and the game record gameplay on the scene runtime into a scenarios
 * asset, or replay one on a loop. Both restart the scene first, so they follow
 * it from its start and a replay matches its recording. */
int app_scenario_record(app_scene_authoring *scene);
/* Stops recording and saves it as scenarios/<Scene> Recording[ N].kdl,
 * writing the asset name to `name`. */
int app_scenario_save(app_scene_authoring *scene, char *name, size_t size);
/* Replays the named scenario, restarting the scene whenever it finishes. */
int app_scenario_replay(app_scene_authoring *scene, const char *name);
/* Stops a replay, or a recording without saving it. */
void app_scenario_stop(app_scene_authoring *scene);
/* After each runtime frame: restarts a replay that finished. */
int app_scenario_follow(app_scene_authoring *scene);
bool app_scenario_recording(const app_scene_authoring *scene);
bool app_scenario_replaying(const app_scene_authoring *scene);
/* The project's scenarios by name, owned (afree the array, free each name). */
char **app_scenario_names(const app_scene_authoring *scene);
