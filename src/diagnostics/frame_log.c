#include "diagnostics/frame_log.h"

#include "support/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum
{
    FRAME_LOG_CAPACITY = 7200 /* two minutes at 60 Hz */
};

static frame_log_entry entries[FRAME_LOG_CAPACITY];
static int next, count;

void frame_log_push(frame_log_entry entry)
{
    entries[next] = entry;
    next = (next + 1) % FRAME_LOG_CAPACITY;
    count += count < FRAME_LOG_CAPACITY;
}

#ifdef __EMSCRIPTEN__
/* Owned CSV of the logged frames, oldest first, for the page to free. */
EMSCRIPTEN_KEEPALIVE char *chibi_frame_log_csv()
{
    static const char
        header[] = "time_ms,interval_ms,steps,dropped_ms,behavior_ms,physics_"
                   "ms,systems_ms,rendering_ms\n";
    enum
    {
        ROW_MAX = 128
    };
    size_t size = sizeof header + (size_t)count * ROW_MAX;
    char *csv = xmalloc(size);
    int length = snprintf(csv, size, "%s", header);
    for (int i = 0; i < count; i++) {
        const frame_log_entry *e = &entries[(next - count + i + FRAME_LOG_CAPACITY) % FRAME_LOG_CAPACITY];
        length += snprintf(
            csv + length,
            size - (size_t)length,
            "%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            e->time_ms,
            e->interval_ms,
            e->steps,
            e->dropped_ms,
            e->behavior_ms,
            e->physics_ms,
            e->systems_ms,
            e->rendering_ms
        );
    }
    return csv;
}
#endif
