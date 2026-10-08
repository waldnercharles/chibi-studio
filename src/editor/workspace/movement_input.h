#ifndef CHIBI_APP_MOVEMENT_INPUT_H
#define CHIBI_APP_MOVEMENT_INPUT_H

#include <math.h>
#include <stdbool.h>

typedef struct
{
    float x, y;
} app_movement_input;

/* Unit input in game coordinates: right +X, down +Y. Opposing keys cancel;
 * diagonal motion has the same authored speed as movement on one axis. */
static inline app_movement_input app_movement_from_keys(bool left, bool right, bool up, bool down)
{
    app_movement_input input = { (float)right - left, (float)down - up };
    float length = hypotf(input.x, input.y);
    if (length > 0) {
        input.x /= length;
        input.y /= length;
    }
    return input;
}

#endif
