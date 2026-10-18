#ifndef CHIBI_PROTOTYPE_SOURCE_INPUT_H
#define CHIBI_PROTOTYPE_SOURCE_INPUT_H

#include "editor/workspace/scene_internal.h"

/* Client-local input is deliberately absent from workspace checkpoints. */
void scene_source_input_tick(app_scene_authoring *scene);
void scene_source_input_free(app_scene_authoring *scene);
void scene_source_input_rebind(app_scene_authoring *scene);
void scene_source_input_harvest(app_scene_authoring *scene);
const char *scene_source_input_text(app_scene_authoring *, scene_prototype_asset *);
/* Borrowed presentation-only candidate; never registered or checkpointed. */
const prototype *scene_source_input_preview(app_scene_authoring *, const scene_prototype_asset *);
bool scene_source_input_blocked(app_scene_authoring *, const char *path);
bool scene_source_input_any(const app_scene_authoring *);
void scene_source_input_end(app_scene_authoring *, const char *path);
int scene_source_input_replace(
    app_scene_authoring *,
    scene_prototype_asset *,
    const char *before,
    const char *after,
    bool coalesce,
    bool live
);
int scene_source_input_text_edit(
    app_scene_authoring *,
    scene_prototype_asset *,
    const char *,
    bool coalesce,
    bool live
);
int scene_source_input_edit(
    app_scene_authoring *,
    scene_prototype_asset *,
    const authoring_prototype_edit *,
    int count,
    const char *key,
    bool coalesce
);
int scene_source_input_history(app_scene_authoring *, scene_prototype_asset *, bool redo);
bool scene_source_input_apply(app_scene_authoring *, yyjson_val *, char *, size_t);
/* First unresolved local conflict, including a source absent from a snapshot. */
const char *scene_source_input_conflict(app_scene_authoring *, char *message, size_t);
uint64_t scene_source_input_conflict_identity(app_scene_authoring *);
int scene_source_input_retry_identity(app_scene_authoring *, uint64_t);
bool scene_source_input_use_shared_identity(app_scene_authoring *, uint64_t);
int scene_source_input_retry(app_scene_authoring *, const char *path);
bool scene_source_input_use_shared(app_scene_authoring *, const char *path);
int scene_source_input_save(app_scene_authoring *, scene_prototype_asset *);
int scene_source_input_reload(app_scene_authoring *, scene_prototype_asset *);

#endif
