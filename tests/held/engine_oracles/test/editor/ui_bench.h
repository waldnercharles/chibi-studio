#ifndef CHIBI_TEST_UI_BENCH_H
#define CHIBI_TEST_UI_BENCH_H

/* The S0 UI bench of docs/studio-rml-migration.md: what authored RML panels
 * cost in RmlUi and the JSON bridge. ui_bench.c measures headless, so the
 * same cases run natively (test_suite --ui-bench) and as WASM under Node
 * (build-web/ui_bench.js); ui_bench_studio.c adds the native Studio. */

#include "ecs/ecs.h"
#include "authoring/component_presentation.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <stdbool.h>
#include <stdint.h>

/* Milliseconds: the median and the 25th and 75th percentiles of runs. */
typedef struct bench_stat
{
    double median, low, high;
} bench_stat;

/* What one run did, which no machine load changes. */
typedef struct bench_counts
{
    uint64_t values;  /* values views evaluated */
    uint64_t lookups; /* members and items they looked up */
    uint64_t layouts; /* layouts of the measured document */
    int elements;     /* elements in it afterwards */
} bench_counts;

double bench_now();
/* Sorts `ms`. */
bench_stat bench_stat_of(double *ms, int count);
void bench_header(const char *title);
void bench_row(const char *id, const char *name, bench_stat time, bench_counts counts);
/* A row comparing two interleaved arms: B's median over A's. */
void bench_ratio(const char *id, const char *name, bench_stat a, bench_stat b);

/* An element whose layouts bench_layouts counts; a document lays out all
 * its elements at once, so this counts the document's layouts. */
rml_element *bench_probe(rml_element *document);
uint64_t bench_layouts();

/* The authored Properties Object tab (projects/studio/content/ui/spike/
 * PropertiesSpike.rml) over one entity's reflected components, in a
 * context of its own the size of the Properties area. */
typedef struct bench_spike bench_spike;

bench_spike *bench_spike_new(int width, int height);
void bench_spike_free(bench_spike *spike);
/* Builds the rows of `entity`'s components on the Object tab that `tabs`
 * gives them, as a selection change does. */
void bench_spike_show(
    bench_spike *spike,
    const ecs_world *world,
    ecs_entity entity,
    const char *name,
    const component_presentation *tabs
);
/* Shows the entity's current values, changing only the texts that differ;
 * returns how many did. */
int bench_spike_refresh(bench_spike *spike);
rml_backend *bench_spike_backend(bench_spike *spike);
bench_counts bench_spike_counts(bench_spike *spike);
int bench_spike_rows(const bench_spike *spike);

/* Moves every float `entity` stores by `delta`, as a running scene does;
 * returns how many it moved. */
int bench_move_floats(ecs_world *world, ecs_entity entity, float delta);
/* Adds `delta` to one float field, as an edit does. */
void bench_edit_float(
    ecs_world *world,
    ecs_entity entity,
    const char *component,
    const char *field,
    float delta
);

/* The cases that need no window; returns nonzero on failure. */
int ui_bench_headless();
/* The Studio cases: native only; only Members (case 10) with
 * `members_only`. */
int ui_bench_studio(const char *program, bool members_only);

#endif
