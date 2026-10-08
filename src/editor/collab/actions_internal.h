#ifndef CHIBI_COLLAB_ACTIONS_INTERNAL_H
#define CHIBI_COLLAB_ACTIONS_INTERNAL_H

#include "editor/collab/actions.h"
#include "editor/graph/search.h"
#include "editor/workspace/editor.h"

#include <stdint.h>

typedef enum
{
    COLLAB_ACTION_REJECT = -1,
    COLLAB_ACTION_APPLY,
    COLLAB_ACTION_PENDING,
} collab_action_gate;

/* Offline/replay allows local execution, hosts apply locally, editor clients
 * propose without optimistic mutation, and spectators or busy sessions reject. */
collab_action_gate collab_actions_begin(const char *json);
/* Publishes an applied local host edit. Offline/replay and editor proposals
 * need no post-mutation action. */
bool collab_actions_finish(const char *json, bool success);

bool graph_collab_apply_action(editor *graph_editor, const char *json, char *error, size_t error_size);
bool scene_collab_apply_action(app_scene_authoring *scene, const char *json, char *error, size_t error_size);
char *graph_collab_action_for_command(const editor *graph_editor, editor_command command);
char *graph_collab_action_open(const editor *graph_editor, uint64_t graph_id);
char *graph_collab_action_spawn(const editor *graph_editor, const node_desc *desc, float x, float y);
char *graph_collab_action_spawn_match(
    const editor *graph_editor,
    const ed_search_match *match,
    float x,
    float y
);
char *graph_collab_action_connect(
    const editor *graph_editor,
    node *a,
    int a_kind,
    int a_pin,
    node *b,
    int b_kind,
    int b_pin
);
/* A picked-up wire is one edit, including its original disconnection. */
char *graph_collab_action_reconnect(
    const editor *e,
    node *from,
    int from_pin,
    node *old_to,
    int old_pin,
    node *new_to,
    int new_pin
);
bool graph_collab_reconnect_apply(
    editor *e,
    node *from,
    int from_pin,
    node *old_to,
    int old_pin,
    node *new_to,
    int new_pin
);
char *graph_collab_action_reset_pin(
    const editor *graph_editor,
    const node *n,
    int pin_index,
    const pin_value *value
);
char *graph_collab_action_set_binding(
    const editor *graph_editor,
    const node *target,
    int input_index,
    const pin_binding *binding
);
char *graph_collab_action_set_input_default(
    const editor *graph_editor,
    const node *target,
    int input_index,
    const pin_value *value
);
char *graph_collab_action_expand_inline(const editor *graph_editor, const node *target, int input_index);

#endif
