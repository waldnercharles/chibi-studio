/* Exercise the production UI publication path without opening a window. */
#include "../../src/editor/studio/studio_ui.c"

bool studio_acceptance_publish_ui_sources(preview_world *world, const char *root, char *error, size_t size)
{
    return publish_sources(world, root, error, size);
}
