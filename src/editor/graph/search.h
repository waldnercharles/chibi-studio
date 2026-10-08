#ifndef CHIBI_EDITOR_GRAPH_SEARCH_H
#define CHIBI_EDITOR_GRAPH_SEARCH_H

#include "graph/authoring/describe.h"
#include "graph/model/behavior_asset.h"

#include <stdint.h>

typedef struct editor editor;
struct ui_event;
struct ui_data_binding;
typedef struct ed_search_match
{
    const node_desc *desc;
    graph *function;
    const behavior_variable *variable;
    const char *prototype;
    const behavior_asset *event;
    int host_event; /* One-based index in the editor host event catalog. */
    const struct ui_event *ui_event; /* an event a GameUi document sends */
    const struct ui_data_binding *ui_data; /* a value one binds, for Set UI Data */
    /* Component node presets: the Component or Type setting, and for Set the
     * one field it writes. */
    const char *setting;
    const char *field;
    const char *category;
    char name[384];
    int score;
} ed_search_match;

/* Caller owns the returned core/array. Entries borrow the current catalogs. */
ed_search_match *ed_search_matches(const editor *e, const char *filter);
/* What a result stands for, to describe it (node_subject_describe). */
node_subject ed_search_match_subject(const editor *e, const ed_search_match *match);
/* Spawns at world (x, y) as one undo step: shader parameters, System compaction
 * and event naming apply, and a pending link-drag wires the new node. Comment
 * boxes are created and return NULL. */
/* Source creation reports acceptance; the new view is adopted after dispatch. */
bool ed_source_spawn_match(editor *, const ed_search_match *);
node *ed_spawn_match_at(editor *e, const ed_search_match *match, float x, float y);
/* Returns the surviving node and reports its allocated ID even when autowiring
 * folds it into an inline source. */
node *ed_spawn_match_at_with_id(editor *e, const ed_search_match *match, float x, float y, uint64_t *created_id);

#endif
