#include "assets/asset_source.h"
#include "assets/scene_document.h"
#include "authoring/preview_source.h"
#include "authoring/source_presentation.h"
#include "authoring/source_rename.h"
#include "editor/workspace/asset_refactor_internal.h"
#include "editor/workspace/scene_internal.h"
#include "runtime/declared_components.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int asset_at_path(const reference_index *index, const char *path)
{
    for (int i = 0; i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (asset->path && !strcmp(asset->path, path)) {
            return i;
        }
    }
    return -1;
}

static bool source_attachments(
    app_scene_authoring *scene,
    app_refactor *plan,
    const authoring_navigation *navigation,
    authoring_target target,
    int asset,
    const char *from,
    const char *to,
    char *error,
    size_t size
)
{
    const reference **uses = NULL;
    reference_index_find_uses(
        scene->asset_index,
        (reference_symbol){ .kind = REFERENCE_SYMBOL_ASSET, .asset = asset },
        &uses
    );
    bool valid = true;
    for (int i = 0; valid && i < acount(uses); i++) {
        const reference *use = uses[i];
        const reference_asset *owner = reference_index_asset_at(scene->asset_index, use->source);
        bool resolved = false;
        for (int j = 0;
             owner->path && j < authoring_navigation_reference_count(navigation);
             j++) {
            const authoring_reference *fact = authoring_navigation_reference_at(navigation, j);
            const authoring_document *document = authoring_navigation_document_at(
                navigation,
                fact->location.document
            );
            if (fact->kind != AUTHORING_DEFINE &&
                authoring_target_equal(fact->target, target) &&
                !strcmp(document->path, owner->path)) {
                resolved = true;
                break;
            }
        }
        if (resolved) {
            continue; /* Every resolved occurrence is already in the source edit plan. */
        }
        if (owner->path &&
            source_catalog_source_path(scene->content_root, owner->path)) {
            /* Qualified calls are rewritten by the canonical planner. String
               asset references need resolved source facts before migration. */
            if (use->role != REFERENCE_CALL && use->role != REFERENCE_ARGUMENT) {
                snprintf(
                    error,
                    size,
                    "Source asset reference needs a resolved rename location: "
                    "%s",
                    owner->name
                );
                valid = false;
            }
            continue;
        }
        if (use->role != REFERENCE_ATTACH || !owner->path ||
            (owner->kind != REFERENCE_ASSET_PROTOTYPE &&
             owner->kind != REFERENCE_ASSET_SCENE)) {
            snprintf(error, size, "Cannot rewrite asset reference in %s", owner->name);
            valid = false;
            break;
        }
        refactor_file *file = NULL;
        for (int j = 0; j < acount(plan->files); j++) {
            if (!plan->files[j].metadata && !strcmp(plan->files[j].from, owner->path)) {
                file = &plan->files[j];
                break;
            }
        }
        if (!file) {
            if (!refactor_saved(scene, owner->path, owner->name, error, size)) {
                valid = false;
                break;
            }
            char *text = read_scene_file(owner->path);
            if (!text) {
                valid = false;
                break;
            }
            apush(
                plan->files,
                ((refactor_file){ .kind = owner->kind,
                                  .name = xstrdup(owner->name),
                                  .from = xstrdup(owner->path),
                                  .to = xstrdup(owner->path),
                                  .text = xstrdup(text),
                                  .before = text })
            );
            file = &alast(plan->files);
        }
        char *text = NULL;
        if (owner->kind == REFERENCE_ASSET_PROTOTYPE) {
            prototype *prototype = prototype_load(file->text, &scene->registry, error, size);
            valid = prototype && !prototype_remove_graph(prototype, from) &&
                    !prototype_add_graph(prototype, to);
            text = valid ? prototype_save(prototype) : NULL;
            prototype_free(prototype);
        } else {
            scene_document *document = scene_document_load(file->text, error, size);
            int entity = document && use->object
                           ? scene_document_entity_index(document, use->object)
                           : -1;
            valid = entity >= 0;
            const char **names = NULL;
            bool found = false;
            if (valid) {
                for (int j = 0; j < acount(document->entities[entity].behaviors); j++) {
                    const char *name = document->entities[entity].behaviors[j];
                    bool match = !strcmp(name, from);
                    found |= match;
                    apush(names, match ? to : name);
                }
                valid = found &&
                        !scene_document_set_entity_behaviors(document, entity, names, acount(names));
            }
            afree(names);
            text = valid ? scene_document_save(document, project_components()) : NULL;
            scene_document_free(document);
        }
        if (text) {
            av_doc *document = asset_text_read(text, error, size);
            char *reconciled = document
                                 ? asset_source_reconcile(file->text, av_doc_root(document), error, size)
                                 : NULL;
            av_doc_free(document);
            free(text);
            text = reconciled;
        }
        if (text) {
            free(file->text);
            file->text = text;
            file->changes++;
        } else {
            valid = false;
        }
    }
    afree(uses);
    return valid;
}

static const refactor_file *source_file(const app_refactor *plan, const char *path)
{
    for (int i = 0; i < acount(plan->files); i++) {
        if (!plan->files[i].metadata && !strcmp(plan->files[i].from, path)) {
            return &plan->files[i];
        }
    }
    return NULL;
}

static void restart_edit(source_catalog_edit **edits, source_catalog_edit edit)
{
    for (int i = 0; i < acount(*edits); i++) {
        if (!strcmp((*edits)[i].path, edit.path)) {
            (*edits)[i] = edit;
            return;
        }
    }
    apush(*edits, edit);
}

/* Restart discovers document identities from paths, rather than the live
 * migration keys retained for compatible runtime state. Prepare that exact
 * complete catalog before rebasing identity-bearing sidecars. */
static authoring_sources *restart_sources(
    app_scene_authoring *scene,
    const app_refactor *plan,
    const authoring_sources *working,
    char *error,
    size_t size
)
{
    source_catalog_edit *edits = NULL;
    for (int i = 0; i < authoring_sources_count(working); i++) {
        const authoring_source *source = authoring_sources_at(working, i);
        const refactor_file *file = source_file(plan, source->path);
        restart_edit(
            &edits,
            (source_catalog_edit){ .path = file ? file->to : source->path,
                                   .text = file ? file->text : source->syntax->text }
        );
    }
    for (int i = 0; i < preview_sources_overlay_count(scene->live); i++) {
        const source_catalog_edit *overlay = preview_sources_overlay_at(scene->live, i);
        if (!source_catalog_source_path(scene->content_root, overlay->path)) {
            restart_edit(
                &edits,
                (source_catalog_edit){ .path = overlay->path, .text = overlay->text }
            );
        }
    }
    for (int i = 0; i < acount(plan->files); i++) {
        const refactor_file *file = &plan->files[i];
        if (file->metadata ||
            source_catalog_source_path(scene->content_root, file->from)) {
            continue;
        }
        if (source_catalog_edit_path(scene->content_root, file->from)) {
            if (strcmp(file->from, file->to)) {
                restart_edit(&edits, (source_catalog_edit){ .path = file->from });
            }
            restart_edit(&edits, (source_catalog_edit){ .path = file->to, .text = file->text });
        }
    }
    authoring_sources *
        result = preview_sources_candidate(scene->live, edits, acount(edits), true, error, size);
    afree(edits);
    return result;
}

static bool source_metadata(
    app_scene_authoring *scene,
    app_refactor *plan,
    const authoring_navigation *navigation,
    const authoring_rename_plan *rename,
    const authoring_sources *working,
    char *error,
    size_t size
)
{
    authoring_sources *restart = NULL, *original = NULL;
    bool valid = true;
    for (int i = 0; valid && i < authoring_navigation_document_count(navigation); i++) {
        const authoring_document *document = authoring_navigation_document_at(navigation, i);
        const refactor_file *found = source_file(plan, document->path);
        if (!document->analysis || !found) {
            continue;
        }
        /* Adding a companion can relocate plan->files. Keep this record local. */
        refactor_file file = *found;
        size_t length = strlen(file.from) + sizeof ".meta";
        char *path = xmalloc(length);
        snprintf(path, length, "%s.meta", file.from);
        char *metadata = read_scene_file(path);
        free(path);
        char *rebased = NULL;
        if (metadata) {
            if (!original) {
                original = restart_sources(scene, &(app_refactor){ 0 }, working, error, size);
            }
            const authoring_source *original_source = NULL;
            for (int j = 0; original && j < authoring_sources_count(original); j++) {
                const authoring_source *candidate = authoring_sources_at(original, j);
                if (!strcmp(candidate->path, file.from)) {
                    original_source = candidate;
                    break;
                }
            }
            authoring_source_presentation
                *before = original_source && authoring_sources_valid(original)
                            ? authoring_source_presentation_new(
                                  original_source->analysis,
                                  authoring_sources_catalog(original),
                                  NULL
                              )
                            : NULL;
            bool restored = before &&
                            authoring_source_presentation_restore(before, metadata);
            if (!restored) {
                authoring_source_presentation_free(before);
                before = authoring_source_presentation_new(
                    document->analysis,
                    authoring_navigation_catalog(navigation),
                    NULL
                );
                restored = before &&
                           authoring_source_presentation_restore(before, metadata);
            }
            if (restored) {
                if (!restart) {
                    restart = restart_sources(scene, plan, working, error, size);
                }
                const authoring_source *source = NULL;
                for (int j = 0; restart && j < authoring_sources_count(restart); j++) {
                    const authoring_source *candidate = authoring_sources_at(restart, j);
                    if (!strcmp(candidate->path, file.to)) {
                        source = candidate;
                        break;
                    }
                }
                authoring_source_presentation
                    *after = restart && authoring_sources_valid(restart) && source
                               ? authoring_source_presentation_new(
                                     source->analysis,
                                     authoring_sources_catalog(restart),
                                     NULL
                                 )
                               : NULL;
                text_edit *edits = NULL;
                for (int j = 0; j < rename->count; j++) {
                    const authoring_rename_edit *edit = &rename->edits[j];
                    if (edit->document == i) {
                        apush(
                            edits,
                            ((text_edit){ edit->range.start, edit->range.end, edit->replacement })
                        );
                    }
                }
                rebased = after
                            ? authoring_source_presentation_rebase(before, after, metadata, edits, acount(edits))
                            : NULL;
                afree(edits);
                authoring_source_presentation_free(after);
                if (!rebased) {
                    snprintf(error, size, "Cannot preserve source metadata for %s", file.from);
                    valid = false;
                }
            }
            authoring_source_presentation_free(before);
        }
        if (valid) {
            /* Stale/malformed optional metadata is discarded. A valid sidecar
             * must rebase successfully; its loss never silently passes. */
            refactor_source_metadata(plan, &file, rebased, metadata);
        }
        free(rebased);
        free(metadata);
    }
    authoring_sources_free(restart);
    authoring_sources_free(original);
    return valid;
}

int app_scene_authoring_plan_source_rename(
    app_scene_authoring *scene,
    const authoring_navigation *navigation,
    authoring_target target,
    const char *name,
    app_refactor **out,
    char *error,
    size_t size
)
{
    *out = NULL;
    if (runtime_session(scene)) {
        snprintf(error, size, "Stop the scene first");
        return -1;
    }
    refactor_sync(scene);
    app_scene_authoring_refresh_asset_index(scene);
    const authoring_sources *sources = preview_sources_working(scene->live);
    if (!sources) {
        snprintf(error, size, "Prepare project sources before rename");
        return -1;
    }
    const source_catalog_reference *references = sources
                                                   ? authoring_sources_references(sources)
                                                   : NULL;
    for (int i = 0; sources && i < authoring_sources_reference_count(sources); i++) {
        if (!references[i].id) {
            snprintf(
                error,
                size,
                "Rename requires resolved project references: %s",
                references[i].path
            );
            return -1;
        }
    }
    authoring_rename_plan *source = authoring_source_rename(navigation, target, name, error, size);
    if (!source) {
        return -1;
    }
    app_refactor *plan = xcalloc(1, sizeof *plan);
    int defining = -1;
    const authoring_reference *definition = authoring_navigation_definition(navigation, target);
    if (definition) {
        defining = definition->location.document;
    }
    bool valid = true;
    for (int i = 0; valid && i < authoring_navigation_document_count(navigation); i++) {
        const authoring_document *document = authoring_navigation_document_at(navigation, i);
        if (!refactor_saved(
                scene,
                document->path,
                document->analysis ? document->analysis->name : document->path,
                error,
                size
            )) {
            valid = false;
            break;
        }
        apush(
            plan->guards,
            ((refactor_guard){ xstrdup(document->path),
                               xstrdup(authoring_navigation_document_text(navigation, i)) })
        );
        int changes = 0;
        for (int j = 0; j < source->count; j++) {
            changes += source->edits[j].document == i;
        }
        if (!changes) {
            continue;
        }
        char *text = authoring_rename_document(
            source,
            i,
            authoring_navigation_document_text(navigation, i),
            error,
            size
        );
        if (!text) {
            valid = false;
            break;
        }
        int asset = asset_at_path(scene->asset_index, document->path);
        if (asset < 0) {
            free(text);
            snprintf(error, size, "Source asset is absent from the project index");
            valid = false;
            break;
        }
        const reference_asset *item = reference_index_asset_at(scene->asset_index, asset);
        bool moving = i == defining &&
                      (target.kind == AUTHORING_PROGRAM || !document->analysis);
        if (moving && (name[0] == '.' || strchr(name, '/') || strchr(name, '\\'))) {
            free(text);
            snprintf(error, size, "Program name cannot be used as an asset filename");
            valid = false;
            break;
        }
        refactor_file file = {
            .kind = item->kind,
            .name = xstrdup(item->name),
            .renamed = moving ? xstrdup(name) : NULL,
            .from = xstrdup(document->path),
            .to = moving ? refactor_renamed_path(document->path, name)
                         : xstrdup(document->path),
            .text = text,
            .before = xstrdup(authoring_navigation_document_text(navigation, i)),
            .identity = document->analysis ? xstrdup(document->analysis->identity) : NULL,
            .changes = changes
        };
        apush(plan->files, file);
        if (moving) {
            valid = source_attachments(scene, plan, navigation, target, asset, item->name, name, error, size);
        }
    }
    source_catalog_edit *edits = NULL;
    for (int i = 0; valid && i < acount(plan->files); i++) {
        const refactor_file *file = &plan->files[i];
        if (!file->metadata &&
            source_catalog_edit_path(scene->content_root, file->from)) {
            if (strcmp(file->from, file->to)) {
                apush(edits, ((source_catalog_edit){ .path = file->from }));
            }
            apush(
                edits,
                ((source_catalog_edit){ .path = file->to,
                                        .text = file->text,
                                        .identity = file->identity })
            );
        }
    }
    valid = valid &&
            (!acount(edits) ||
             !preview_sources_validate(scene->live, edits, acount(edits), error, size));
    afree(edits);
    valid = valid &&
            source_metadata(scene, plan, navigation, source, sources, error, size);
    authoring_rename_plan_free(source);
    if (!valid || !refactor_current(plan, error, size)) {
        app_refactor_free(plan);
        return -1;
    }
    *out = plan;
    return 0;
}
