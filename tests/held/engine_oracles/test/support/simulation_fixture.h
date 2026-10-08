#ifndef CHIBI_TEST_SIMULATION_FIXTURE_H
#define CHIBI_TEST_SIMULATION_FIXTURE_H

#include "runtime/engine.h"
#include "authoring/prototype_preview.h"

#include <assert.h>
#include <math.h>

typedef struct
{
    engine *runtime;
    game_prototype_preview *preview;
    double time;
} simulation_fixture;

static inline simulation_fixture simulation_fixture_new(engine *runtime, game_prototype_preview *preview)
{
    return (
        simulation_fixture
    ){ preview ? game_prototype_preview_runtime(preview) : runtime, preview, 0 };
}

static inline void simulation_frame(simulation_fixture *f, double time)
{
    assert(time >= f->time);
    int result = f->preview ? game_prototype_preview_tick(f->preview, time)
                            : engine_frame(f->runtime, time);
    assert(result >= 0);
    f->time = time;
}

/* Gameplay tests request elapsed time; overload/drop policy has separate clock
 * regressions. Display each fixed interval so long lifetimes are fully tested. */
static inline void simulation_advance(simulation_fixture *f, double time)
{
    simulation_frame(f, f->time);
    while (f->time + 1e-10 < time) {
        simulation_frame(f, fmin(time, f->time + ENGINE_FIXED_SECONDS));
    }
}

static inline void simulation_step(simulation_fixture *f)
{
    simulation_advance(f, f->time + ENGINE_FIXED_SECONDS);
}

static inline void simulation_action(simulation_fixture *f, const char *action)
{
    assert(engine_dispatch_input(f->runtime, action, 1, 1, NULL) >= 0);
    assert(engine_dispatch_input(f->runtime, action, 0, 0, NULL) >= 0);
    simulation_step(f);
}

#endif
