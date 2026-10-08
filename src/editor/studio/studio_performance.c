/* Performance: live charts of the last 30 seconds (Liveline). Frame time
 * against the 60 Hz budget, the running scene's behavior, physics, systems
 * and rendering time with a legend (Recharts), and the scene's GPU time
 * against target_fps with the lighting grids adaptive resolution chose.
 * projects/studio's Performance and PerformanceHeader show them: each
 * publication hands the samples to their <line-chart>s by name
 * (line_chart_publish), and the `performance` source holds the rest.
 * Readouts refresh four times a second so they stay readable. */
#include "editor/studio/studio_internal.h"
#include "render/scene_renderer.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "ui/controls/controls.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW 30.0

enum
{
    SERIES_BEHAVIOR,
    SERIES_PHYSICS,
    SERIES_SYSTEMS,
    SERIES_RENDERING,
    SERIES_COUNT,
};

/* Performance.rml's charts name these; Performance.rcss colors the
   legend's dots as these. */
static const char *const SERIES_NAMES[SERIES_COUNT] = { "perf:behavior",
                                                        "perf:physics",
                                                        "perf:systems",
                                                        "perf:rendering" };
static const uint32_t SERIES_COLORS[SERIES_COUNT] = { 0x6f9fe8ff,
                                                      0x57ab5aff,
                                                      0xd9a53dff,
                                                      0xc586c0ff };
#define FRAME_SERIES "perf:frame"
#define GPU_SERIES "perf:gpu"

static void publish_telemetry(studio *s);

/* Samples only frames the editor runs for input or a playing scene: an idle
 * editor's frames are not worth charting, and charting them would keep it
 * drawing. */
void studio_performance_sample(studio *s, double delta_seconds)
{
    if (!studio_active(s)) {
        publish_telemetry(s);
        return;
    }
    const engine_frame_timing *timing = app_scene_authoring_runtime_timing(s->scene);
    bool running = timing && app_scene_authoring_runtime_state(s->scene).playing;
    render_radiance_status lighting;
    bool lit = app_scene_renderer_lighting_status(s->scene->renderer, &lighting);
    studio_perf_sample sample = {
        .time = (float)s->time,
        .frame_ms = (float)(delta_seconds * 1000),
        .scene_gpu_ms = lit ? (float)lighting.gpu_ms : -1,
        .running = running,
    };
    if (running) {
        sample.ms[SERIES_BEHAVIOR] = (float)timing->behavior_ms;
        sample.ms[SERIES_PHYSICS] = (float)timing->physics_ms;
        sample.ms[SERIES_SYSTEMS] = (float)timing->systems_ms;
        sample.ms[SERIES_RENDERING] = (float)timing->rendering_ms;
    }
    apush(s->perf, sample);
    int old = 0;
    while (old < acount(s->perf) && s->perf[old].time < s->time - WINDOW - 1) {
        old++;
    }
    if (old) {
        memmove(
            s->perf,
            s->perf + old,
            (size_t)(acount(s->perf) - old) * sizeof *s->perf
        );
        asetlen(s->perf, acount(s->perf) - old);
    }
    publish_telemetry(s);
}

/* The published samples' column `value` (frame time, a phase's time or the
   GPU's) where `keep` holds, under `name`. */
typedef enum
{
    COLUMN_FRAME,
    COLUMN_PHASE,
    COLUMN_GPU,
} column;

static bool kept(const studio_perf_sample *sample, column c)
{
    switch (c) {
        case COLUMN_FRAME: return true;
        case COLUMN_PHASE: return sample->running;
        case COLUMN_GPU: return sample->scene_gpu_ms >= 0;
    }
    return false;
}

static float value_of(const studio_perf_sample *sample, column c, int phase)
{
    switch (c) {
        case COLUMN_FRAME: return sample->frame_ms;
        case COLUMN_PHASE: return sample->ms[phase];
        case COLUMN_GPU: return sample->scene_gpu_ms;
    }
    return 0;
}

static void publish_column(studio *s, const char *name, column c, int phase, uint32_t color)
{
    int count = acount(s->perf_published), kept_count = 0;
    float *times = xmalloc((size_t)(count ? count : 1) * sizeof *times);
    float *values = xmalloc((size_t)(count ? count : 1) * sizeof *values);
    for (int i = 0; i < count; i++) {
        const studio_perf_sample *sample = &s->perf_published[i];
        if (kept(sample, c)) {
            times[kept_count] = sample->time;
            values[kept_count++] = value_of(sample, c, phase);
        }
    }
    line_chart_series series = {
        .times = times,
        .values = values,
        .count = kept_count,
        .color = color,
        .now = count ? s->perf_published[count - 1].time : 0,
    };
    line_chart_publish(name, &series);
    free(times);
    free(values);
}

/* Hands the published samples to Performance's charts. */
static void publish_series(studio *s)
{
    publish_column(s, FRAME_SERIES, COLUMN_FRAME, 0, 0);
    for (int c = 0; c < SERIES_COUNT; c++) {
        publish_column(s, SERIES_NAMES[c], COLUMN_PHASE, c, SERIES_COLORS[c]);
    }
    publish_column(s, GPU_SERIES, COLUMN_GPU, 0, SERIES_COLORS[SERIES_RENDERING]);
}

void studio_performance_free(studio *s)
{
    line_chart_publish(FRAME_SERIES, NULL);
    line_chart_publish(GPU_SERIES, NULL);
    for (int c = 0; c < SERIES_COUNT; c++) {
        line_chart_publish(SERIES_NAMES[c], NULL);
    }
    afree(s->perf);
    afree(s->perf_published);
}

/* Input continues to redraw the charts for scrubbing and range easing. Their
 * data advances independently, retaining every recorded frame between
 * publications. */
static void publish_telemetry(studio *s)
{
    bool playing = app_scene_authoring_runtime_state(s->scene).playing;
    if (s->perf_publications && playing == s->perf_publish_playing &&
        s->time < s->perf_publish_next) {
        return;
    }
    int count = acount(s->perf);
    asetlen(s->perf_published, count);
    if (count) {
        memcpy(s->perf_published, s->perf, (size_t)count * sizeof *s->perf);
    }
    s->perf_publications++;
    s->perf_publish_playing = playing;
    s->perf_publish_next = s->time + (playing ? 1.0 / 20.0 : 0.25);
    publish_series(s);
    render_radiance_status lighting;
    s->perf_lighting.available = app_scene_renderer_lighting_status(s->scene->renderer, &lighting);
    if (!s->perf_lighting.available) {
        return;
    }
    s->perf_lighting.budget_ms = (float)lighting.budget_ms;
    static const char *const detail[] = { "Full", "Half", "Quarter", "Eighth" };
    int used = snprintf(
        s->perf_lighting.text,
        sizeof s->perf_lighting.text,
        "Resolution %g \xc2\xb7 color %s \xc2\xb7 gather %s",
        lighting.resolution,
        detail[lighting.color],
        lighting.gather_half ? "Half" : "Full"
    );
    if (lighting.adaptive && lighting.rungs > 1) {
        snprintf(
            s->perf_lighting.text + used,
            sizeof s->perf_lighting.text - used,
            " \xc2\xb7 step %d of %d",
            lighting.rung + 1,
            lighting.rungs
        );
    }
}

/* Averages refresh at 4 Hz; a readout that changes every frame cannot be read.
 * An idle editor draws only when something changes, which it says. */
static void refresh_readout(studio *s)
{
    if (!studio_active(s)) {
        snprintf(s->perf_readout, sizeof s->perf_readout, "Idle");
        return;
    }
    if (s->time - s->perf_readout_time < 0.25 && s->perf_readout_time > 0) {
        return;
    }
    s->perf_readout_time = s->time;
    double sum = 0, peak = 0, last = acount(s->perf) ? alast(s->perf).time : s->time;
    int count = 0;
    for (int i = 0; i < acount(s->perf); i++) {
        if (s->perf[i].time >= last - 2) {
            sum += s->perf[i].frame_ms;
            peak = fmax(peak, s->perf[i].frame_ms);
            count++;
        }
    }
    double average = count ? sum / count : 0;
    snprintf(
        s->perf_readout,
        sizeof s->perf_readout,
        "%.0f fps \xc2\xb7 %.1f ms \xc2\xb7 max %.1f ms",
        average > 0 ? 1000 / average : 0,
        average,
        peak
    );
}

/* ---------- The panel ---------- */

void studio_performance_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *p = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "performance");
    yyjson_mut_obj_add_strcpy(doc, p, "readout", "");
    yyjson_mut_obj_add_bool(doc, p, "live", false);
    yyjson_mut_obj_add_bool(doc, p, "running", false);
    yyjson_mut_obj_add_bool(doc, p, "lighting", false);
    yyjson_mut_obj_add_strcpy(doc, p, "lightingText", "");
    yyjson_mut_obj_add_bool(doc, p, "gpu", false);
    yyjson_mut_obj_add_real(doc, p, "budget", 0);
}

/* What the charts' series leave to markup: the readout, whether the
   simulation and the GPU have samples to chart, and the lighting's state.
   The samples themselves reach the charts by line_chart_publish. */
bool studio_performance_sync(studio *s, const studio_panel_sync *at)
{
    refresh_readout(s);
    bool running = false, gpu = false;
    for (int i = 0; i < acount(s->perf_published); i++) {
        running |= s->perf_published[i].running;
        gpu |= s->perf_published[i].scene_gpu_ms >= 0;
    }
    yyjson_mut_val *p = at->variable;
    bool changed = ui_data_put_text(at->doc, p, "readout", s->perf_readout);
    changed |= ui_data_put_bool(p, "live", studio_active(s));
    changed |= ui_data_put_bool(p, "running", running);
    changed |= ui_data_put_bool(p, "lighting", s->perf_lighting.available);
    changed |= ui_data_put_text(at->doc, p, "lightingText", s->perf_lighting.text);
    changed |= ui_data_put_bool(p, "gpu", gpu);
    return ui_data_put_number(p, "budget", s->perf_lighting.budget_ms) || changed;
}

void studio_performance_header(studio *s, int area)
{
    studio_ui_area_controls(s, area, "performance", "PerformanceHeader");
}

void studio_performance_body(studio *s, int area)
{
    studio_ui_area(s, area, "Performance");
}
