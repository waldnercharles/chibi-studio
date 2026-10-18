#ifndef CHIBI_ASSET_CODE_H
#define CHIBI_ASSET_CODE_H

#include "assets/asset_source.h"
#include "assets/prototype.h"
#include "authoring/preview_source.h"
#include "authoring/source_navigation.h"
#include "authoring/source_presentation.h"
#include "editor/ui/kit.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct asset_code asset_code;
typedef struct editor editor;
typedef struct behavior_asset behavior_asset;
/* An editor session owns exact source text and a last valid overlay, shared by
 * loaders and live preview. Invalid typing never replaces that overlay. */
asset_code *asset_code_open(const char *path, char *error, size_t size);
asset_code *asset_code_retain(asset_code *code);
void asset_code_free(asset_code *code);
/* Identity of the current source history entry, for latest-edit Undo actions. */
const void *asset_code_step(const asset_code *code);
/* Follow ownership when the workspace restores this source's preview overlay. */
void asset_code_adopt_overlay(asset_code *code);
/* Native checkpoint state; protocol conversion belongs to the workspace boundary. */
av_value *asset_code_capture(asset_code *code);
bool asset_code_has_state(const asset_code *code, const av_value *state);
asset_code *asset_code_restore(const av_value *state, const prototype_registry *registry);
const char *asset_code_path(const asset_code *code);
bool asset_code_dirty(const asset_code *code);
const char *asset_code_error(const asset_code *code);
/* Complete executable source; optional graph metadata is stored separately. */
const char *asset_code_text(const asset_code *code);
/* Authored prototypes/relationships are data, never executable graph drafts. */
bool asset_code_static(const asset_code *code);
void asset_code_publish_static(asset_code *code);
/* Canonical prototype overlays publish only with workspace model acceptance. */
void asset_code_own_prototype(asset_code *code);
/* Retain a deleted canonical document as dirty; automatic disk refresh waits
 * for explicit Save/Reload and the source history remains available. */
void asset_code_file_removed(asset_code *code);
bool asset_code_file_missing(const asset_code *code);
void asset_code_reject_static(asset_code *code, const char *error);
/* The editable view exposes the complete executable source. */
kit_code_editor *asset_code_editor(asset_code *code);
/* One source and metadata history serves both views, including invalid drafts. */
bool asset_code_can_replay(const asset_code *code, bool redo);
bool asset_code_replay(asset_code *code, bool redo, bool live);
bool asset_code_sync_graph(asset_code *code, editor *graph_editor, bool live);
bool asset_code_location(const asset_code *code, uint64_t graph, uint64_t node, int *start, int *end);
bool asset_code_node_at(const asset_code *code, int offset, uint64_t *graph, uint64_t *node);
bool asset_code_breakpoint_at(const asset_code *code, int offset, uint64_t *graph, uint64_t *node);
const authoring_source *asset_code_source(const asset_code *code);
const script_syntax *asset_code_syntax(const asset_code *code);
const script_catalog *asset_code_catalog(const asset_code *code);
authoring_source_presentation *asset_code_presentation(const asset_code *code);
const authoring_navigation *asset_code_navigation(const asset_code *code);
int asset_code_document(const asset_code *code);
/* Borrowed facts for the synchronized graph, including an incomplete draft
 * whose last valid generation remains displayed. Pending graph edits have no
 * source facts until reconciliation. */
const authoring_source *asset_code_graph_source(const asset_code *code, const editor *e);
/* Validate a source-only draft against the catalog without replacing a graph. */
bool asset_code_validate(asset_code *code, editor *editor);
/* A validated refactor is one shared draft undo step; reject stale proposals. */
bool asset_code_replace(asset_code *code, const char *before, const char *text);
bool asset_code_replace_with_metadata(
    asset_code *code,
    const char *before,
    const char *text,
    const char *metadata
);
/* Consecutive updates in one graph gesture share an undo entry. Ordinary source
 * edits and undo/redo end that group; every update still rejects stale text. */
bool asset_code_edit_source(
    asset_code *,
    const char *before,
    const char *text,
    const char *metadata,
    bool coalesce
);
bool asset_code_definition(const asset_code *code, int offset, int *start);
asset_source_span *asset_code_references(const asset_code *code, int offset);
void asset_code_execution(asset_code *code, uint64_t graph, uint64_t node);
/* Only unchanged text can identify a paused generation in the current draft. */
bool asset_code_execution_location(const asset_code *code, int *start, int *end);
void asset_code_time(asset_code *code, double time);
/* A command moved the caret: the next typing starts an undo step of its own. */
void asset_code_end_typing(asset_code *code);
bool asset_code_pending(const asset_code *code);
bool asset_code_graph_blocked(const asset_code *code);
bool asset_code_runtime_held(const asset_code *code);
void asset_code_hold_runtime(asset_code *code, bool hold);
const kit_code_marker *asset_code_markers(const asset_code *code);
void asset_code_set_registry(asset_code *code, const prototype_registry *registry);
/* Binds project preparation/publication while retained immutable generations
 * keep diagnostics/navigation safe across later edits and scene replacement. */
void asset_code_set_source_world(asset_code *, preview_world *, const char *content_root);
void asset_code_set_names(asset_code *code, const char *const *names, int count);
/* Check external/visual saves and update the editor with a single text edit. */
void asset_code_refresh(asset_code *code);
/* Render the existing code primitive with shared code language intelligence. */
kit_code_result asset_code_draw(
    asset_code *code,
    kit *k,
    const char *key,
    const kit_code_desc *preferences,
    bool live
);
/* Saves valid source and separate graph metadata. False on diagnostics or disk conflict. */
bool asset_code_save(asset_code *code, char *error, size_t size);

#endif
