#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"

#include <string.h>

collab_action_gate collab_actions_begin(const char *json)
{
    if (!collab_active() || collab_replaying()) {
        return COLLAB_ACTION_APPLY;
    }
    if (!json || strlen(json) > 60 * 1024) {
        return COLLAB_ACTION_REJECT;
    }
    if (!collab_can_edit()) {
        return COLLAB_ACTION_REJECT;
    }
    if (collab_is_host()) {
        return COLLAB_ACTION_APPLY;
    }
    return collab_propose(json) ? COLLAB_ACTION_PENDING : COLLAB_ACTION_REJECT;
}

bool collab_actions_finish(const char *json, bool success)
{
    if (!success || !collab_active() || collab_replaying() || !collab_is_host()) {
        return true;
    }
    return collab_publish(json);
}
