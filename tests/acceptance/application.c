/* Compile the actual application only into this optional acceptance target.
 * Its object satisfies the application symbols before the Studio archive does. */
#include "../../src/editor/workspace/application.c"

studio *studio_acceptance_studio(void *state)
{
    return ((app_loop *)state)->studio;
}
