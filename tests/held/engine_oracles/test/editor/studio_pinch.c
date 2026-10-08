#include "editor/workspace/viewport.h"
#include "editor/studio/studio_internal.h"
#include "support/xalloc.h"
#include "test/suite_internal.h"

#include <math.h>
#include <stdlib.h>

/* The Scene's pointer input after this frame's pinch by `scale`, with the
 * wheel turned `wheel` steps for the camera. */
static engine_viewport_host_input pinch(studio *s, float scale, float wheel)
{
    engine_viewport_host_input input = { .width = 400, .height = 200, .wheel = wheel };
    s->pinch = logf(scale);
    studio_scene_view(s, &input);
    return input;
}

static bool nearly(float a, float b)
{
    return fabsf(a - b) < 1e-3f;
}

/* A pinch zooms the Scene view toward the pointer: the frame point under it
 * stays put, the zoom stays between fitting the frame and 8x, and the
 * camera's wheel is untouched. */
void test_studio_pinch()
{
    studio *s = xcalloc(1, sizeof *s);
    s->view.zoom = 1;
    s->view.x = s->view.y = 0.5f;
    s->surface.rect = (kit_rect){ .w = 200, .h = 100 };
    s->surface.x = 50;
    s->surface.y = 75;
    s->surface.hovered = true;
    engine_viewport_host_input fit = pinch(s, 1, 0);
    engine_viewport_host_input in = pinch(s, 3, 2);
    test_check(nearly(s->view.zoom, 3) && nearly(in.x, fit.x) && nearly(in.y, fit.y) && in.wheel == 2, "a pinch zooms the Scene view keeping the point under the pointer in place");
    pinch(s, 100, 0);
    bool most = nearly(s->view.zoom, 8);
    in = pinch(s, 0.001f, 0);
    test_check(most && nearly(s->view.zoom, 1) && nearly(s->view.x, 0.5f) && nearly(in.x, fit.x), "a pinch zooms the Scene view no further than 8x and no less than fitting the frame");
    s->surface.hovered = false;
    pinch(s, 2, 0);
    test_check(nearly(s->view.zoom, 1), "a pinch elsewhere leaves the Scene view");
    free(s);
}
