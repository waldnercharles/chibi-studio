#include "editor/workspace/viewport.h"
#include "editor/workspace/gameplay_input.h"

#include "editor/workspace/editor.h"
#include "editor/workspace/input.h"
#include "editor/workspace/scene_internal.h"

#include <stdio.h>

void app_gameplay_sample(app_scene_authoring *scene, bool accepts_input, const CF_V2 *mouse_target)
{
    engine *runtime = scene->active_runtime_target == APP_RUNTIME_PREVIEW
                        ? game_prototype_preview_runtime(scene->prototype_preview)
                        : scene->runtime;
    bool firing = false;
    if (engine_sample_controls(runtime, accepts_input ? app_input_controls() : 0, &firing) <
        0) {
        snprintf(scene->message, sizeof scene->message, "Action binding requires a source and event name.");
    }
    if (!accepts_input) {
        app_scene_authoring_move(scene, 0, 0);
        app_scene_authoring_aim(scene, (CF_V2){ 0 }, false);
        return;
    }
    float x, y;
    app_input_movement(&x, &y);
    app_scene_authoring_move(scene, x, y);
    if (app_input_aim(&x, &y)) {
        app_scene_authoring_aim(scene, (CF_V2){ x, y }, firing);
    } else if (app_input_mouse_aiming() && mouse_target) {
        app_scene_authoring_aim_at(scene, *mouse_target, firing);
    } else {
        app_scene_authoring_aim(scene, (CF_V2){ 0 }, firing);
    }
}

void app_gameplay_input(app_scene_authoring *scene, engine *runtime, engine_viewport *viewport)
{
    bool focused = engine_viewport_focused(viewport) &&
                   !app_input_pressed(APP_INPUT_ESCAPE);
    /* A focused game text field takes the letters gameplay would. */
    bool accepts_input = focused && !editor_host_captures_keyboard() &&
                         !engine_viewport_ui_typing(viewport);
    if (!accepts_input) {
        app_gameplay_sample(scene, false, NULL);
    }
    engine_set_focus(runtime, focused ? ENGINE_FOCUS_VIEWPORT : ENGINE_FOCUS_CANVAS);
    if (!accepts_input) {
        return;
    }
    float x = 0, y = 0;
    bool mouse = engine_viewport_mouse_world(viewport, &x, &y);
    CF_V2 target = { x, -y };
    app_gameplay_sample(scene, true, mouse ? &target : NULL);
}
