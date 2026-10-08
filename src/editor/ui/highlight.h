#ifndef CHIBI_EDITOR_UI_HIGHLIGHT_H
#define CHIBI_EDITOR_UI_HIGHLIGHT_H

#include <stddef.h>

/* Syntax highlighting as token runs, after Shiki: a lexer per language turns a
 * line into runs, and themes color runs by token class (tok-* in the Studio theme, theme/roles.rcss).
 * Lines are independent; no language here carries state across lines. */
typedef enum
{
    HIGHLIGHT_TEXT,
    HIGHLIGHT_NUMBER,
    HIGHLIGHT_STRING,
    HIGHLIGHT_KEYWORD,
    HIGHLIGHT_NAME,
    HIGHLIGHT_PATH,
    HIGHLIGHT_PUNCTUATION,
    HIGHLIGHT_MUTED,
    HIGHLIGHT_ERROR,
    HIGHLIGHT_WARNING,
    HIGHLIGHT_COUNT,
} highlight_token;

typedef enum
{
    HIGHLIGHT_LOG, /* free text: numbers, quoted strings, paths, literals, severities */
    HIGHLIGHT_JSON,
    HIGHLIGHT_EXPRESSION, /* behavior expressions and number-field arithmetic */
} highlight_language;

typedef struct
{
    int start;
    int length;
    highlight_token token;
} highlight_run;

/* Splits text[0, length) into runs covering it in order; returns the run count,
 * at most max. */
int highlight_runs(highlight_language language, const char *text, int length, highlight_run *runs, int max);
/* Writes RmlUi markup: escaped text with a <span class="tok-..."> per colored
 * run. Returns the length written, cut to fit size between whole characters,
 * so every tag and entity stays whole. */
size_t highlight_markup(highlight_language language, const char *text, char *out, size_t size);
/* Writes text as RmlUi markup that shows it as is, in no runs, cut between
   whole characters to fit size. */
size_t highlight_escape(const char *text, char *out, size_t size);

#endif
