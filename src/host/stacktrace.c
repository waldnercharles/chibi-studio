/* host/stacktrace.c -- POSIX crash diagnostics.
 *
 * On a fatal signal, print a symbolized stack trace to stderr and exit. On
 * macOS the trace is resolved through `atos`; elsewhere `addr2line` is tried
 * with a raw-symbol fallback. Best-effort: backtrace/popen/fprintf are not
 * async-signal-safe, but the process is already dying.
 */
#include "host/stacktrace.h"

#include "host/vendor/debugbreak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

#ifdef _WIN32

void stacktrace_init(char *program_name)
{
    (void)program_name;
}
void stacktrace_assert_handler(bool expr, const char *message, const char *file, int line)
{
    if (!expr) {
        fprintf(stderr, "Assertion failed: %s (%s:%d)\n", message, file, line);
        debug_break();
    }
}

#elif defined(__EMSCRIPTEN__)

void stacktrace_init(char *) {}
void stacktrace_assert_handler(bool expr, const char *message, const char *file, int line)
{
    if (!expr) {
        fprintf(stderr, "Assertion failed: %s (%s:%d)\n", message, file, line);
        debug_break();
    }
}

#else

#define MAX_FRAMES 64

static char *s_program_name;

/* Resolve all addresses in one atos/addr2line invocation; fall back to raw
 * symbols for any line that fails to resolve. */
static void resolve_and_print(void **frames, char **symbols, int count, int skip)
{
    char cmd[4096];
    int off = 0;
#ifdef __APPLE__
    off = snprintf(cmd, sizeof cmd, "atos -o %.256s -p %d", s_program_name, getpid());
#else
    off = snprintf(cmd, sizeof cmd, "addr2line -f -e %.256s", s_program_name);
#endif
    for (int i = skip; i < count; i++) {
        off += snprintf(cmd + off, sizeof cmd - (size_t)off, " %p", frames[i]);
    }
    snprintf(cmd + off, sizeof cmd - (size_t)off, " 2>/dev/null");

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        for (int i = skip; i < count; i++) {
            fprintf(stderr, "  [%2d] %s\n", count - i - 1, symbols ? symbols[i] : "???");
        }
        return;
    }
    char line[1024];
    for (int i = skip; i < count; i++) {
        int nr = count - i - 1;
        if (fgets(line, sizeof line, fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            /* atos echoes the raw address when it cannot resolve. */
            if (line[0] == '0' && line[1] == 'x') {
                fprintf(stderr, "  [%2d] %s\n", nr, symbols ? symbols[i] : "???");
            } else {
                fprintf(stderr, "  [%2d] %s\n", nr, line);
            }
        } else {
            fprintf(stderr, "  [%2d] %s\n", nr, symbols ? symbols[i] : "???");
        }
    }
    pclose(fp);
}

static void print_stacktrace(int skip)
{
    void *frames[MAX_FRAMES];
    int n = backtrace(frames, MAX_FRAMES);
    char **symbols = backtrace_symbols(frames, n);

    fprintf(stderr, "\nStack trace:\n");
    resolve_and_print(frames, symbols, n, skip);
    fprintf(stderr, "\n");

    free(symbols);
}

static void signal_handler(int sig)
{
    const char *name;
    switch (sig) {
        case SIGABRT: name = "SIGABRT (abort)"; break;
        case SIGFPE: name = "SIGFPE (arithmetic error)"; break;
        case SIGILL: name = "SIGILL (illegal instruction)"; break;
        case SIGSEGV: name = "SIGSEGV (segfault)"; break;
        case SIGTERM: name = "SIGTERM (terminated)"; break;
        default: name = "unknown signal"; break;
    }
    fprintf(stderr, "Caught %s\n", name);
    print_stacktrace(2);
    _Exit(EXIT_FAILURE);
}

void stacktrace_init(char *program_name)
{
    s_program_name = program_name;

    signal(SIGABRT, signal_handler);
    signal(SIGFPE, signal_handler);
    signal(SIGILL, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGTERM, signal_handler);
}

void stacktrace_assert_handler(bool expr, const char *message, const char *file, int line)
{
    if (!expr) {
        fprintf(stderr, "Assertion failed: %s (%s:%d)\n", message, file, line);
        print_stacktrace(1);
        debug_break();
    }
}

#endif
