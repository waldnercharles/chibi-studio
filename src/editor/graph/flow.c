#include "capabilities/catalog.h"
#include "editor/graph/flow_visual.h"
#include "editor/workspace/editor_internal.h"
#include "support/xalloc.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool ed_inspection_blocked(const editor *e)
{
    return e->inspection_blocked ||
           (e->inspector && (e->edit_pending || e->pending_snapshot));
}

execution_inspector *ed_display_inspector(const editor *e)
{
    return ed_inspection_blocked(e) ? NULL : e->inspector;
}

typedef struct
{
    execution_debug_activity activity;
    double fired;
} seen_activity;

typedef struct ed_flow_activity_index
{
    const execution_inspector *inspector;
    seen_activity *rows;
    bool primed;
} ed_flow_activity_index;

static bool same_activity(const execution_debug_activity *a, const execution_debug_activity *b)
{
    return a->generation == b->generation && a->owner == b->owner &&
           a->module == b->module && a->function == b->function &&
           a->occurrence == b->occurrence && a->from_module == b->from_module &&
           a->from_function == b->from_function &&
           a->from_occurrence == b->from_occurrence;
}

static ed_flow_activity_index *activity_index(const editor *e)
{
    editor *mutable = (editor *)e;
    if (!mutable->flow_activity) {
        mutable->flow_activity = xcalloc(1, sizeof *mutable->flow_activity);
    }
    ed_flow_activity_index *index = mutable->flow_activity;
    execution_inspector *inspector = ed_display_inspector(e);
    if (index->inspector != inspector) {
        aclear(index->rows);
        index->primed = false;
        index->inspector = inspector;
    }
    for (int i = 0; inspector && i < execution_inspector_activity_count(inspector);
         i++) {
        execution_debug_activity activity = execution_inspector_activity_at(inspector, i);
        if (activity.owner != e->inspection_owner) {
            continue;
        }
        int row = -1;
        for (int j = 0; j < acount(index->rows); j++) {
            if (same_activity(&index->rows[j].activity, &activity)) {
                row = j;
                break;
            }
        }
        if (row < 0) {
            /* Keep the derived UI cache bounded alongside the runtime cache. */
            if (acount(index->rows) == 512) {
                memmove(index->rows, index->rows + 1, 511 * sizeof *index->rows);
                asetlen(index->rows, 511);
            }
            apush(
                index->rows,
                ((seen_activity){ .activity = activity,
                                  .fired = index->primed ? e->monotonic_time : -INFINITY })
            );
            continue;
        }
        if (index->rows[row].activity.count != activity.count) {
            index->rows[row].fired = e->monotonic_time;
            index->rows[row].activity = activity;
        }
    }
    index->primed = true;
    return index;
}

void ed_flow_activity_free(editor *e)
{
    if (e->flow_activity) {
        afree(e->flow_activity->rows);
        free(e->flow_activity);
        e->flow_activity = NULL;
    }
}

static uint64_t node_occurrence(const editor *e, uint64_t id)
{
    if (!e->presentation) {
        return 0;
    }
    const source_presentation_origin *origin = authoring_source_presentation_origin(
        e->presentation,
        e->active_graph->graph_id,
        id,
        0
    );
    return origin ? origin->node_occurrence : 0;
}

void editor_query_edge_activity(const editor *e, const edge *edge, editor_edge_activity *out)
{
    assert(e && edge && out);
    *out = (editor_edge_activity){ 0 };
    if (!e->presentation || edge->kind != GRAPH_EDGE_EXEC) {
        return;
    }
    uint64_t from = node_occurrence(e, edge->from_id),
             to = node_occurrence(e, edge->to_id);
    uint64_t module = script_catalog_id("module", authoring_source_presentation_identity(e->presentation));
    ed_flow_activity_index *index = activity_index(e);
    for (int i = 0; i < acount(index->rows); i++) {
        const seen_activity *row = &index->rows[i];
        if (from && to && row->activity.module == module &&
            row->activity.from_module == module && row->activity.occurrence == to &&
            row->activity.from_occurrence == from) {
            out->count += row->activity.count;
            double since = fmax(0, e->monotonic_time - row->fired);
            out->pulse = fmaxf(out->pulse, (float)flow_visual_pulse(since));
            out->heat = fmaxf(out->heat, (float)flow_visual_heat(since));
        }
    }
}

void editor_query_node_activity(const editor *e, uint64_t id, editor_node_activity *out)
{
    assert(e && out);
    *out = (editor_node_activity){ .since = INFINITY };
    uint64_t occurrence = node_occurrence(e, id);
    if (!occurrence) {
        return;
    }
    uint64_t module = script_catalog_id("module", authoring_source_presentation_identity(e->presentation));
    ed_flow_activity_index *index = activity_index(e);
    for (int i = 0; i < acount(index->rows); i++) {
        const seen_activity *row = &index->rows[i];
        if (row->activity.module == module && row->activity.occurrence == occurrence) {
            out->runs += row->activity.count;
            out->since = fmin(out->since, fmax(0, e->monotonic_time - row->fired));
        }
    }
    out->pulse = (float)flow_visual_pulse(out->since);
}
