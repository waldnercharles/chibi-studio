#ifndef CHIBI_INPUT_H
#define CHIBI_INPUT_H
#include "runtime/components.h"

#include <stdbool.h>

typedef enum
{
    APP_INPUT_ESCAPE,
    APP_INPUT_UNDO,
    APP_INPUT_REDO,
    APP_INPUT_COPY,
    APP_INPUT_CUT,
    APP_INPUT_PASTE,
    APP_INPUT_DUPLICATE,
    APP_INPUT_SAVE,
    APP_INPUT_DELETE,
    APP_INPUT_SHIFT,
    APP_INPUT_CONTROL,
    APP_INPUT_COMMAND,
    APP_INPUT_COUNT
} app_input_action;

/* Initialize bindings before cf_app_update; destroy them before Cute teardown. */
void app_input_initialize();
void app_input_shutdown();
/* Once per frame, immediately after cf_app_update. */
void app_input_frame();
bool app_input_pressed(app_input_action action);
void app_input_movement(float *x, float *y);
/* Returns true while the controller aim stick is outside its dead zone. */
bool app_input_aim(float *x, float *y);
uint64_t app_input_controls();
bool app_input_mouse_aiming();
#endif
