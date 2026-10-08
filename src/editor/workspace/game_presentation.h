#ifndef CHIBI_GAME_PRESENTATION_H
#define CHIBI_GAME_PRESENTATION_H

#include "editor/workspace/viewport.h"
#include "editor/workspace/scene_authoring.h"

typedef struct app_game_presentation app_game_presentation;

/* Borrows the loaded scene and depth viewport. Starts its authored runtime. */
app_game_presentation *app_game_presentation_new(
    app_scene_authoring *scene,
    engine_viewport *viewport,
    bool interactive
);
void app_game_presentation_free(app_game_presentation *game);
/* Called after the host updates input. Zero continues, positive quits, negative fails. */
int app_game_presentation_frame(app_game_presentation *game, double now);

#endif
