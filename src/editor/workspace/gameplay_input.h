#ifndef CHIBI_GAMEPLAY_INPUT_H
#define CHIBI_GAMEPLAY_INPUT_H

#include "editor/workspace/viewport.h"
#include "editor/workspace/scene_authoring.h"
#include "runtime/engine.h"

/* Poll the host after its input update and before stepping the runtime. */
void app_gameplay_input(app_scene_authoring *scene, engine *runtime, engine_viewport *viewport);
/* Both hosts sample the same controls; mouse_target uses gameplay coordinates. */
void app_gameplay_sample(app_scene_authoring *scene, bool accepts_input, const CF_V2 *mouse_target);

#endif
