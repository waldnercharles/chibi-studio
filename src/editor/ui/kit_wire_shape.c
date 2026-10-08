#include "editor/ui/kit.h"
#include "graph/authoring/arrange_path.h"
#include "support/array.h"

#include <assert.h>

static_assert(
    (int)KIT_WIRE_CURVED == ARRANGE_ROUTE_CURVED &&
        (int)KIT_WIRE_ORTHOGONAL == ARRANGE_ROUTE_ORTHOGONAL &&
        (int)KIT_WIRE_STRAIGHT == ARRANGE_ROUTE_STRAIGHT &&
        (int)KIT_WIRE_SUBWAY == ARRANGE_ROUTE_SUBWAY &&
        (int)KIT_WIRE_STYLE_COUNT == ARRANGE_ROUTE_STYLE_COUNT,
    "kit wire styles follow arrange_route_style"
);

/* The C side of kit_wires: its C++ renderer cannot include arrange_path.h. */
int kit_wire_shape(kit_wire_style style, const float *knots, int count, float *pieces, int max)
{
    assert(count >= 2);
    arrange_point *via = NULL;
    for (int i = 1; i + 1 < count; i++) {
        apush(via, ((arrange_point){ knots[i * 2], knots[i * 2 + 1] }));
    }
    arrange_piece *out = NULL;
    arrange_path(
        (arrange_route_style)style,
        (arrange_point){ knots[0], knots[1] },
        (arrange_point){ knots[count * 2 - 2], knots[count * 2 - 1] },
        via,
        acount(via),
        &out
    );
    int n = acount(out);
    for (int i = 0; i < n && i < max; i++) {
        const arrange_piece *p = &out[i];
        float xy[8] = { p->p0.x, p->p0.y, p->p1.x, p->p1.y,
                        p->p2.x, p->p2.y, p->p3.x, p->p3.y };
        for (int k = 0; k < 8; k++) {
            pieces[i * 8 + k] = xy[k];
        }
    }
    afree(via);
    afree(out);
    return n;
}
