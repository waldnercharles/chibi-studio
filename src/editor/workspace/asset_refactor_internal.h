#ifndef CHIBI_EDITOR_ASSET_REFACTOR_INTERNAL_H
#define CHIBI_EDITOR_ASSET_REFACTOR_INTERNAL_H
#include "editor/workspace/asset_refactor.h"

/* One file of a plan: `text` replaces `from` at `to`. Without `from` the
   file is new; without `to` it goes. */
typedef struct
{
    int kind;
    char *name, *renamed;
    char *from, *to;
    char *text, *before;
    char *identity; /* Canonical source key survives a file move and its Undo. */
    int changes;
    bool metadata;
} refactor_file;
typedef struct
{
    char *path, *text;
} refactor_guard;

struct app_refactor
{
    refactor_file *files;
    refactor_guard *guards;
    int mentions;
};

bool refactor_saved(struct app_scene_authoring *, const char *path, const char *name, char *error, size_t size);
void refactor_sync(struct app_scene_authoring *);
char *refactor_renamed_path(const char *path, const char *name);
bool refactor_current(const app_refactor *, char *error, size_t size);
void refactor_source_metadata(
    app_refactor *,
    const refactor_file *,
    const char *metadata,
    const char *before_metadata
);
#endif
