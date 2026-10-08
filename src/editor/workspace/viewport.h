#ifndef CHIBI_EDITOR_VIEWPORT_H
#define CHIBI_EDITOR_VIEWPORT_H

#include "render/viewport.h"

/* ImGui presentation and input adapters for the shared render surface. */
int engine_viewport_image(engine_viewport *, float width, float height);
void engine_viewport_ui_input(engine_viewport *, bool active);
void engine_viewport_ui_keys(engine_viewport *, bool active);

#endif
