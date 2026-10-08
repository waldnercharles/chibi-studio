/* Where an authored panel's activations go (docs/studio-rml-migration.md,
 * Events): emit('studio.<command>') runs one of Studio's commands, which
 * the palette lists too, and emit('<source>.<action>', ...) the action of a
 * panel source (studio_panel_sources). Other events are the panel's own, for
 * its entity's behaviors. */
#include "editor/studio/studio_internal.h"

#include <string.h>

#define COMMAND_PREFIX "studio."

/* The source whose name `event` starts with, before a dot, and its action. */
static const studio_panel_source *source_of(const char *event, const char **action)
{
    const char *dot = strchr(event, '.');
    for (int i = 0; dot && i < studio_panel_source_count; i++) {
        const studio_panel_source *source = &studio_panel_sources[i];
        size_t length = (size_t)(dot - event);
        if (source->action && strlen(source->name) == length &&
            !strncmp(source->name, event, length)) {
            *action = dot + 1;
            return source;
        }
    }
    return NULL;
}

bool studio_panel_route(studio *s, const ui_activation *a)
{
    if (!strncmp(a->event, COMMAND_PREFIX, sizeof COMMAND_PREFIX - 1)) {
        int command = studio_command_named(a->event + sizeof COMMAND_PREFIX - 1);
        if (command >= 0) {
            studio_command_run_with(
                s,
                (studio_command)command,
                a->has_value ? (int)a->value : -1
            );
        }
        return command >= 0;
    }
    const char *action;
    const studio_panel_source *source = source_of(a->event, &action);
    if (source) {
        source->action(s, action, a);
    }
    return source != NULL;
}
