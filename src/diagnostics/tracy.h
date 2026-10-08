#ifndef CHIBI_DIAGNOSTICS_TRACY_H
#define CHIBI_DIAGNOSTICS_TRACY_H

/* Keep the runtime's profiling calls independent of Tracy's C API. The
 * profiling build supplies CHIBI_ENABLE_TRACY; every other build removes the
 * calls at preprocessing time. */
#ifdef CHIBI_ENABLE_TRACY
#include <tracy/TracyC.h>

#define CHIBI_TRACE_FRAME_MARK(name)                                           \
    do {                                                                       \
        TracyCFrameMarkNamed(name);                                            \
    } while (0)
#define CHIBI_TRACE_SET_THREAD_NAME(name)                                      \
    do {                                                                       \
        TracyCSetThreadName(name);                                             \
    } while (0)
#define CHIBI_TRACE_ZONE_BEGIN(zone, name) TracyCZoneN(zone, name, 1)
#define CHIBI_TRACE_ZONE_END(zone) TracyCZoneEnd(zone)
#define CHIBI_TRACE_PLOT(name, value) TracyCPlot(name, value)
#elif defined(CHIBI_ENABLE_WEB_TRACE)
/* The web profiling build shows zones as User Timing measures in the
 * browser's performance timeline. */
#include "diagnostics/web_trace.h"

#define CHIBI_TRACE_FRAME_MARK(name)                                           \
    do {                                                                       \
    } while (0)
#define CHIBI_TRACE_SET_THREAD_NAME(name)                                      \
    do {                                                                       \
    } while (0)
#define CHIBI_TRACE_ZONE_BEGIN(zone, name)                                     \
    web_trace_zone zone = web_trace_begin(name)
#define CHIBI_TRACE_ZONE_END(zone) web_trace_end(zone)
#define CHIBI_TRACE_PLOT(name, value)                                          \
    do {                                                                       \
    } while (0)
#else
#define CHIBI_TRACE_FRAME_MARK(name)                                           \
    do {                                                                       \
    } while (0)
#define CHIBI_TRACE_SET_THREAD_NAME(name)                                      \
    do {                                                                       \
    } while (0)
#define CHIBI_TRACE_ZONE_BEGIN(zone, name)
#define CHIBI_TRACE_ZONE_END(zone)                                             \
    do {                                                                       \
    } while (0)
#define CHIBI_TRACE_PLOT(name, value)                                          \
    do {                                                                       \
    } while (0)
#endif

#endif
