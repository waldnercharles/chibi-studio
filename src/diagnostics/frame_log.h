#ifndef CHIBI_DIAGNOSTICS_FRAME_LOG_H
#define CHIBI_DIAGNOSTICS_FRAME_LOG_H

/* The last FRAME_LOG_CAPACITY frames' timing, which the browser page reads
 * as Module.chibiFrames() to find and compare hitches. */
typedef struct
{
    double time_ms;     /* since the first logged frame */
    double interval_ms; /* since the previous frame */
    int steps;          /* fixed steps the frame ran */
    double dropped_ms;  /* simulation time the frame discarded */
    double behavior_ms, physics_ms, systems_ms, rendering_ms;
} frame_log_entry;

void frame_log_push(frame_log_entry entry);

#endif
