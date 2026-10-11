#include "editor/workspace/scenario_playback.h"

#include "assets/scenario_document.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/save.h"
#include "support/array.h"
#include "support/asset_json.h"
#include "support/asset_text.h"
#include "support/text_file.h"
#include "support/xalloc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SCENARIO_FOLDER "scenarios"

static void scenario_path(const app_scene_authoring *scene, const char *name, char *out, size_t size)
{
    snprintf(
        out,
        size,
        "%s/" SCENARIO_FOLDER "/%s" DECLARATIVE_ASSET_EXTENSION,
        scene->content_root,
        name
    );
}

/* The open scene's asset name: its file name without the extension. */
static void read_scene_name(const app_scene_authoring *scene, char *out, size_t size)
{
    const char *path = scene->scene_path ? scene->scene_path : "Scene";
    const char *slash = strrchr(path, '/');
    snprintf(out, size, "%s", slash ? slash + 1 : path);
    char *extension = strstr(out, DECLARATIVE_ASSET_EXTENSION);
    if (extension) {
        *extension = 0;
    }
}

static int restart(app_scene_authoring *scene)
{
    return app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_RESTART);
}

static save *saved_progress(app_scene_authoring *scene)
{
    return scene->live->runtime->save;
}

int app_scenario_record(app_scene_authoring *scene)
{
    app_scenario_stop(scene);
    scene->scenario_seed = (unsigned)time(NULL);
    char *saved = save_export(saved_progress(scene));
    av_doc *doc = av_json_read(saved);
    free(saved);
    scene->scenario_saved_state = av_value_copy(av_doc_root(doc));
    av_doc_free(doc);
    engine_record_input(scene->runtime, scene->scenario_seed);
    scene->scenario_recording = true;
    return restart(scene);
}

/* Replays from the start with the recording's progress and random stream. */
static int start_replay(app_scene_authoring *scene)
{
    const scenario_document *document = scene->scenario;
    char *saved = document->saved_state ? av_json_write(document->saved_state)
                                        : xstrdup("{}");
    bool imported = save_import(saved_progress(scene), saved);
    free(saved);
    if (!imported) {
        snprintf(
            scene->message,
            sizeof scene->message,
            "Cannot replay: %s",
            save_error(saved_progress(scene))
        );
        return -1;
    }
    engine_replay_input(
        scene->runtime,
        document->keys,
        acount(document->keys),
        document->seed
    );
    return restart(scene);
}

/* The first free name of <Scene> Recording, <Scene> Recording 2, ... */
static void recording_name(const app_scene_authoring *scene, char *out, size_t size)
{
    char base[128];
    read_scene_name(scene, base, sizeof base);
    for (int i = 1;; i++) {
        char path[1024];
        snprintf(out, size, i == 1 ? "%s Recording" : "%s Recording %d", base, i);
        scenario_path(scene, out, path, sizeof path);
        struct stat info;
        if (stat(path, &info) != 0) {
            return;
        }
    }
}

int app_scenario_save(app_scene_authoring *scene, char *name, size_t size)
{
    engine_stop_recording(scene->runtime);
    scene->scenario_recording = false;
    int count;
    const input_keyframe *keys = engine_recorded_input(scene->runtime, &count);
    int frames = (int)engine_last_timing(scene->runtime)->simulation_steps;
    if (!count || frames <= keys[count - 1].step) {
        return -1;
    }
    char scene_asset[128];
    read_scene_name(scene, scene_asset, sizeof scene_asset);
    bool progress = av_obj_size(scene->scenario_saved_state) != 0;
    scenario_document document = { .scene = scene_asset,
                                   .frames = frames,
                                   .seed = scene->scenario_seed,
                                   .saved_state = progress ? scene->scenario_saved_state
                                                           : NULL };
    for (int i = 0; i < count; i++) {
        apush(document.keys, keys[i]);
    }
    char folder[1024], path[1024];
    snprintf(folder, sizeof folder, "%s/" SCENARIO_FOLDER, scene->content_root);
    mkdir(folder, 0755);
    recording_name(scene, name, size);
    scenario_path(scene, name, path, sizeof path);
    char *json = scenario_document_save(&document);
    int result = save_content_file(scene, path, json);
    free(json);
    afree(document.keys);
    return result;
}

int app_scenario_replay(app_scene_authoring *scene, const char *name)
{
    app_scenario_stop(scene);
    char path[1024], error[256];
    scenario_path(scene, name, path, sizeof path);
    char *text = text_file_read(path);
    if (!text) {
        snprintf(scene->message, sizeof scene->message, "Cannot replay %s: not found", name);
        return -1;
    }
    scenario_document *document = scenario_document_load(text, error, sizeof error);
    free(text);
    if (!document) {
        snprintf(scene->message, sizeof scene->message, "Cannot replay %s: %s", name, error);
        return -1;
    }
    scene->scenario = document;
    return start_replay(scene);
}

void app_scenario_stop(app_scene_authoring *scene)
{
    engine_stop_recording(scene->runtime);
    engine_stop_replay(scene->runtime);
    scene->scenario_recording = false;
    av_value_free(scene->scenario_saved_state);
    scene->scenario_saved_state = NULL;
    scenario_document_free(scene->scenario);
    scene->scenario = NULL;
}

int app_scenario_follow(app_scene_authoring *scene)
{
    if (!scene->scenario || engine_last_timing(scene->runtime)->simulation_steps <
                                (uint64_t)scene->scenario->frames) {
        return 0;
    }
    return start_replay(scene);
}

bool app_scenario_recording(const app_scene_authoring *scene)
{
    return scene->scenario_recording;
}

bool app_scenario_replaying(const app_scene_authoring *scene)
{
    return scene->scenario != NULL;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **app_scenario_names(const app_scene_authoring *scene)
{
    char folder[1024];
    snprintf(folder, sizeof folder, "%s/" SCENARIO_FOLDER, scene->content_root);
    char **names = NULL;
    DIR *dir = opendir(folder);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        size_t length = strlen(entry->d_name),
               extension = strlen(DECLARATIVE_ASSET_EXTENSION);
        if (entry->d_name[0] == '.' || length <= extension ||
            strcmp(entry->d_name + length - extension, DECLARATIVE_ASSET_EXTENSION)) {
            continue;
        }
        char *name = xstrdup(entry->d_name);
        name[length - extension] = 0;
        apush(names, name);
    }
    if (dir) {
        closedir(dir);
    }
    if (acount(names) > 1) {
        qsort(names, (size_t)acount(names), sizeof *names, compare_names);
    }
    return names;
}
