#include "editor/ui/highlight.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *const CLASSES[HIGHLIGHT_COUNT] = {
    NULL,       "tok-number", "tok-string", "tok-keyword", "tok-name",
    "tok-path", "tok-punct",  "tok-muted",  "tok-error",   "tok-warning",
};

static bool word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static bool word_is(const char *text, int length, const char *const *words)
{
    for (int i = 0; words[i]; i++) {
        if ((int)strlen(words[i]) == length &&
            strncmp(text, words[i], (size_t)length) == 0) {
            return true;
        }
    }
    return false;
}

static int number_length(const char *text, int length)
{
    int i = 0;
    if (i < length && (text[i] == '-' || text[i] == '+') && i + 1 < length &&
        isdigit((unsigned char)text[i + 1])) {
        i++;
    }
    if (i >= length || !(isdigit((unsigned char)text[i]) ||
                         (text[i] == '.' && i + 1 < length &&
                          isdigit((unsigned char)text[i + 1])))) {
        return 0;
    }
    while (i < length && (isdigit((unsigned char)text[i]) || text[i] == '.')) {
        i++;
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        int j = i + 1 + (i + 1 < length && (text[i + 1] == '-' || text[i + 1] == '+'));
        if (j < length && isdigit((unsigned char)text[j])) {
            i = j;
            while (i < length && isdigit((unsigned char)text[i])) {
                i++;
            }
        }
    }
    return i;
}

static int string_length(const char *text, int length)
{
    char quote = text[0];
    int i = 1;
    while (i < length && text[i] != quote) {
        i += text[i] == '\\' && i + 1 < length ? 2 : 1;
    }
    return i < length ? i + 1 : length;
}

/* A slash-separated token such as content/scenes/Sandbox.json. */
static int path_length(const char *text, int length)
{
    int i = 0;
    bool slash = false;
    while (i < length && (word_char(text[i]) || text[i] == '/' ||
                          text[i] == '.' || text[i] == '-')) {
        slash |= text[i] == '/';
        i++;
    }
    while (i > 0 && text[i - 1] == '.') {
        i--;
    }
    return slash ? i : 0;
}

static int push(highlight_run *runs, int count, int max, int start, int length, highlight_token token)
{
    if (length <= 0) {
        return count;
    }
    if (count > 0 && runs[count - 1].token == token &&
        runs[count - 1].start + runs[count - 1].length == start) {
        runs[count - 1].length += length;
        return count;
    }
    if (count == max) {
        runs[count - 1].length = start + length - runs[count - 1].start;
        return count;
    }
    runs[count] = (highlight_run){ start, length, token };
    return count + 1;
}

int highlight_runs(highlight_language language, const char *text, int length, highlight_run *runs, int max)
{
    static const char *const LITERALS[] = { "true", "false", "null", "nil",
                                            "inf",  "nan",   NULL };
    static const char *const LOG_ERRORS[] = { "error",  "Error",  "ERROR",
                                              "failed", "Failed", "Could",
                                              NULL };
    static const char *const LOG_WARNINGS[] = { "warning", "Warning", "WARNING",
                                                "note",    "Note",    NULL };
    static const char *const EXPRESSION_WORDS[] = { "if", "then", "else", "and",
                                                    "or", "not",  "pi",   NULL };
    if (max <= 0) {
        return 0;
    }
    int count = 0;
    int i = 0;
    while (i < length) {
        char c = text[i];
        const char *at = text + i;
        int rest = length - i;
        int n;
        if (c == '"' || (c == '\'' && language != HIGHLIGHT_JSON)) {
            n = string_length(at, rest);
            /* A JSON key is a string followed by a colon. */
            int j = i + n;
            while (j < length && text[j] == ' ') {
                j++;
            }
            bool key = language == HIGHLIGHT_JSON && j < length && text[j] == ':';
            count = push(runs, count, max, i, n, key ? HIGHLIGHT_NAME : HIGHLIGHT_STRING);
            i += n;
            continue;
        }
        if ((n = number_length(at, rest)) && (i == 0 || !word_char(text[i - 1]))) {
            count = push(runs, count, max, i, n, HIGHLIGHT_NUMBER);
            i += n;
            continue;
        }
        if (language == HIGHLIGHT_LOG && (n = path_length(at, rest))) {
            count = push(runs, count, max, i, n, HIGHLIGHT_PATH);
            i += n;
            continue;
        }
        if (word_char(c)) {
            n = 1;
            while (n < rest && word_char(at[n])) {
                n++;
            }
            highlight_token token = HIGHLIGHT_TEXT;
            if (word_is(at, n, LITERALS) || (language == HIGHLIGHT_EXPRESSION &&
                                             word_is(at, n, EXPRESSION_WORDS))) {
                token = HIGHLIGHT_KEYWORD;
            } else if (language == HIGHLIGHT_LOG && word_is(at, n, LOG_ERRORS)) {
                token = HIGHLIGHT_ERROR;
            } else if (language == HIGHLIGHT_LOG && word_is(at, n, LOG_WARNINGS)) {
                token = HIGHLIGHT_WARNING;
            } else if (language == HIGHLIGHT_EXPRESSION && n < rest && at[n] == '(') {
                token = HIGHLIGHT_NAME;
            }
            count = push(runs, count, max, i, n, token);
            i += n;
            continue;
        }
        bool punctuation = strchr("{}[](),:;=<>+-*/%|&!.", c) != NULL;
        count = push(runs, count, max, i, 1, punctuation ? HIGHLIGHT_PUNCTUATION : HIGHLIGHT_TEXT);
        i++;
    }
    return count;
}

static const char *entity(char c)
{
    switch (c) {
        case '&': return "&amp;";
        case '<': return "&lt;";
        case '>': return "&gt;";
        case '"': return "&quot;";
        /* Braces in markup would open RmlUi data expressions. */
        case '{': return "&#123;";
        case '}': return "&#125;";
        default: return NULL;
    }
}

/* How many of text's characters fit in room bytes once escaped. */
static int fitting(const char *text, int length, size_t room)
{
    size_t used = 0;
    int count = 0;
    while (count < length) {
        const char *escaped = entity(text[count]);
        used += escaped ? strlen(escaped) : 1;
        if (used > room) {
            break;
        }
        count++;
    }
    return count;
}

static size_t append(char *out, size_t used, const char *text, size_t length)
{
    memcpy(out + used, text, length);
    return used + length;
}

static size_t append_escaped(char *out, size_t used, const char *text, int length)
{
    for (int i = 0; i < length; i++) {
        const char *escaped = entity(text[i]);
        used = escaped ? append(out, used, escaped, strlen(escaped))
                       : append(out, used, text + i, 1);
    }
    return used;
}

size_t highlight_escape(const char *text, char *out, size_t size)
{
    assert(size > 0);
    size_t used = append_escaped(out, 0, text, fitting(text, (int)strlen(text), size - 1));
    out[used] = '\0';
    return used;
}

size_t highlight_markup(highlight_language language, const char *text, char *out, size_t size)
{
    enum
    {
        MAX_RUNS = 256
    };
    highlight_run runs[MAX_RUNS];
    int length = (int)strlen(text);
    int count = highlight_runs(language, text, length, runs, MAX_RUNS);
    size_t used = 0;
    /* A run that does not fit ends the markup after as many whole characters
       as fit inside its tags, so no tag or entity is ever cut. */
    for (int r = 0; r < count; r++) {
        const char *cls = CLASSES[runs[r].token];
        char open[48] = "";
        if (cls) {
            snprintf(open, sizeof open, "<span class=\"%s\">", cls);
        }
        const char *close = cls ? "</span>" : "";
        size_t tags = strlen(open) + strlen(close);
        if (used + tags >= size) {
            break;
        }
        const char *start = text + runs[r].start;
        /* RmlUi drops text that is only spaces between two elements, so a gap
           between an expression's tokens keeps its width as &nbsp;. */
        bool gap = language == HIGHLIGHT_EXPRESSION && !cls && runs[r].length > 0;
        for (int i = 0; gap && i < runs[r].length; i++) {
            gap = start[i] == ' ';
        }
        size_t room = size - 1 - used - tags;
        int fit = gap ? (int)(room / 6 < (size_t)runs[r].length
                                  ? room / 6
                                  : (size_t)runs[r].length)
                      : fitting(start, runs[r].length, room);
        if (fit == 0) {
            break;
        }
        used = append(out, used, open, strlen(open));
        for (int i = 0; gap && i < fit; i++) {
            used = append(out, used, "&nbsp;", 6);
        }
        if (!gap) {
            used = append_escaped(out, used, start, fit);
        }
        used = append(out, used, close, strlen(close));
        if (fit < runs[r].length) {
            break;
        }
    }
    if (size) {
        out[used] = '\0';
    }
    return used;
}
