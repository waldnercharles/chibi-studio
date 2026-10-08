#include "diagnostics/web_trace.h"

#ifdef CHIBI_ENABLE_WEB_TRACE
#include <emscripten.h>

/* Begin and end must use the same Performance time origin, including in
 * threaded builds where Emscripten's clock can be epoch-relative. */
EM_JS(double, trace_now, (), { return performance.now(); })

/* Names are static, so each is decoded once. clang-format reads this as C,
   so it avoids operators such as === that C does not have. */
EM_JS(void, measure, (const char *name, double start), {
    let names = Module['chibiTraceNames'];
    if (!names) {
        names = Module['chibiTraceNames'] = new Map();
    }
    if (!names.has(name)) {
        names.set(name, UTF8ToString(name));
    }
    performance.measure(names.get(name), { start, end: performance.now() });
})

web_trace_zone web_trace_begin(const char *name)
{
    return (web_trace_zone){ name, trace_now() };
}

void web_trace_end(web_trace_zone zone)
{
    measure(zone.name, zone.start);
}
#endif
