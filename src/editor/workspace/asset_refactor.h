#ifndef CHIBI_EDITOR_ASSET_REFACTOR_H
#define CHIBI_EDITOR_ASSET_REFACTOR_H

#include "authoring/reference_index.h"
#include "authoring/source_navigation.h"

#include <stdbool.h>
#include <stddef.h>

/* Studio's rename and safe delete across files: the shared plan
 * (assets/refactor.h) on the saved file of each asset of the asset index,
 * published as one change checked against those files, after which what
 * Studio holds of them reloads. A plan never touches an asset with unsaved
 * edits, and applying one returns the plan that undoes it. */
struct app_scene_authoring;
typedef struct app_refactor app_refactor;

/* Whether a symbol's references reach past its own asset, so renaming it
 * means rewriting other files. */
bool app_scene_authoring_refactor_reaches(struct app_scene_authoring *scene, reference_symbol symbol);
int app_scene_authoring_plan_rename(
    struct app_scene_authoring *scene,
    reference_symbol symbol,
    const char *name,
    app_refactor **plan,
    char *error,
    size_t error_size
);
int app_scene_authoring_plan_source_rename(
    struct app_scene_authoring *,
    const authoring_navigation *,
    authoring_target,
    const char *name,
    app_refactor **plan,
    char *error,
    size_t error_size
);
/* Deletes whatever still uses the symbol would leave dangling. */
int app_scene_authoring_plan_delete(
    struct app_scene_authoring *scene,
    reference_symbol symbol,
    app_refactor **plan,
    char *error,
    size_t error_size
);
/* Writes every file of the plan or none, each checked against the text it
 * was planned on, then reloads them; *undo restores them. */
int app_scene_authoring_apply_refactor(
    struct app_scene_authoring *scene,
    const app_refactor *plan,
    app_refactor **undo,
    char *error,
    size_t error_size
);
void app_refactor_free(app_refactor *plan);

/* An asset a plan changes, for its preview. */
typedef struct
{
    int kind; /* reference_asset_kind */
    const char *name;
    const char *renamed; /* its new name, or NULL */
    int changes;         /* references rewritten in it */
} app_refactor_asset;

int app_refactor_count(const app_refactor *plan);
app_refactor_asset app_refactor_at(const app_refactor *plan, int index);
/* Values that may still name the old name (refactor_mention). */
int app_refactor_mentions(const app_refactor *plan);

#endif
