#include "editor/workspace/movement_input.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

void test_movement_input()
{
    for (int bits = 0; bits < 16; bits++) {
        bool left = bits & 1, right = bits & 2, up = bits & 4, down = bits & 8;
        app_movement_input input = app_movement_from_keys(left, right, up, down);
        int x = (int)right - left, y = (int)down - up;
        assert((x < 0) == (input.x < 0) && (x > 0) == (input.x > 0));
        assert((y < 0) == (input.y < 0) && (y > 0) == (input.y > 0));
        assert(fabsf(hypotf(input.x, input.y) - ((x || y) ? 1 : 0)) < 0.00001f);
    }
    puts("movement input PASS: four directions, opposing keys and normalized diagonals");
}
