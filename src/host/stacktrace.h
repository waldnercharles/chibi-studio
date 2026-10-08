#ifndef CHIBI_STACKTRACE_H
#define CHIBI_STACKTRACE_H

#include <stdbool.h>

/* Store argv[0] and install fatal-signal handlers (SIGSEGV, SIGABRT, ...).
 * Call at the top of main before anything else. */
void stacktrace_init(char *program_name);

/* Assert handler for cute_framework's cf_set_assert_handler: prints the
 * failure, a stack trace, and breaks into a debugger via debug_break(). */
void stacktrace_assert_handler(bool expr, const char *message, const char *file, int line);

#endif
