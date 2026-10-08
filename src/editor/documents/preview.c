#include "authoring/studio_schema.h"
#include "editor/graph/eval_view.h"
#include "editor/ui/labels.h"
#include "editor/workspace/editor_internal.h"
#include "graph/document/problems.h"
#include "graph/document/serialize.h"
#include "graph/nodes/component_nodes.h"
#include "graph/nodes/studio_nodes.h"

#include <assert.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Authoring edits mutate the working Behavior Asset, not the preview snapshot. */

void ed_compile(editor *e)
{
    if (e->presentation) {
        return; /* Executable source diagnostics come from its prepared generation. */
    }
    ed_refresh_stale_behavior_expressions(e);
    compile_report_free(&e->compile);
    compile_report_init(&e->compile);
    behavior_asset_free(e->compiled);
    e->compiled = behavior_asset_validate(e->asset, e->asset_catalog, acount(e->asset_catalog), &e->compile);
    if (!e->studio_nodes) {
        studio_nodes_check(e->asset, &e->compile);
    }
}

unsigned editor_edit_revision(const editor *e)
{
    return e ? e->edit_revision : 0;
}

bool editor_has_pending_edits(const editor *e)
{
    return e && e->edit_pending;
}

void editor_undo(editor *e)
{
    if (e) {
        ed_undo(e);
    }
}

void editor_redo(editor *e)
{
    if (e) {
        ed_redo(e);
    }
}

static void ed_refresh_edit_revision(editor *e)
{
    if (e->edit_revision_initialized && !e->edit_pending) {
        return;
    }
    av_value *snapshot = ed_document_snapshot(e);
    if (!snapshot) {
        return;
    }
    int changed = e->edit_revision_initialized
                    ? e->edit_revision_asset != e->asset ||
                          !e->edit_revision_snapshot ||
                          !av_equals_ordered(e->edit_revision_snapshot, snapshot)
                    : e->edit_pending;
    av_value_free(e->edit_revision_snapshot);
    e->edit_revision_snapshot = snapshot;
    e->edit_revision_asset = e->asset;
    e->edit_revision_initialized = 1;
    e->edit_pending = 0;
    if (changed) {
        e->edit_revision++;
    }
}

void editor_tick(editor *e, double monotonic_time)
{
    if (!e) {
        return;
    }
    ed_refresh_edit_revision(e);
    e->monotonic_time = monotonic_time;
}

void editor_set_inspector(editor *e, execution_inspector *inspector, uint64_t owner)
{
    if (e->inspector != inspector) {
        execution_inspector *retained = inspector ? execution_inspector_retain(inspector) : NULL;
        execution_inspector_free(e->inspector);
        e->inspector = retained;
    }
    e->inspection_owner = owner;
    ed_refresh_values(e);
}

void editor_set_inspection_blocked(editor *e, bool blocked)
{
    e->inspection_blocked = blocked;
    ed_refresh_values(e);
}

int editor_log_count(const editor *e)
{
    return e->log_count;
}

editor_log_line editor_log_at(const editor *e, int index)
{
    assert(index >= 0 && index < e->log_count);
    int64_t serial = e->log_dropped + index;
    const ed_log_entry *entry = &e->log[serial % ED_LOG_MAX];
    return (
        editor_log_line
    ){ serial, entry->timestamp, entry->step, entry->origin, entry->message };
}

void editor_clear_log(editor *e)
{
    e->log_dropped += e->log_count;
    e->log_count = 0;
}

void editor_log_print(editor *e, const char *origin, const char *message)
{
    if (e->log_count == ED_LOG_MAX) {
        e->log_dropped++;
        e->log_count--;
    }
    ed_log_entry *entry = &e->log[(e->log_dropped + e->log_count++) % ED_LOG_MAX];
    execution_inspector *inspector = ed_display_inspector(e);
    entry->timestamp = e->monotonic_time;
    entry->step = inspector ? execution_inspector_cursor(inspector).step : 0;
    snprintf(entry->origin, sizeof entry->origin, "%s", origin);
    snprintf(entry->message, sizeof entry->message, "%s", message ? message : "");
}

