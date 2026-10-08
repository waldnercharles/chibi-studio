#ifndef CHIBI_DIAGNOSTICS_WEB_TRACE_H
#define CHIBI_DIAGNOSTICS_WEB_TRACE_H

typedef struct
{
    const char *name; /* static */
    double start;     /* performance.now() milliseconds */
} web_trace_zone;

web_trace_zone web_trace_begin(const char *name);
/* Records the zone as a performance.measure named for it. */
void web_trace_end(web_trace_zone zone);

#endif
