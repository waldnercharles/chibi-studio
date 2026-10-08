#ifndef CHIBI_EDITOR_EVAL_VIEW_H
#define CHIBI_EDITOR_EVAL_VIEW_H
#include "authoring/source_presentation.h"
#include "execution/inspection.h"
#include "graph/model/graph.h"

typedef struct eval_view eval_view;
typedef bool (*editor_value_formatter)(const pin_value *, uint64_t owner, char *, size_t, void *);
eval_view *eval_view_new();
void eval_view_free(eval_view *);
void eval_view_set_formatter(eval_view *, editor_value_formatter, void *);
/* Retains the snapshot handle and semantic presentation independently. Never
 * executes a producer to obtain a displayed value. */
void eval_view_set_inspector(eval_view *, execution_inspector *, uint64_t owner, const authoring_source_presentation *);
const char *eval_view_error(const eval_view *);
bool eval_view_live(const eval_view *);
int eval_view_live_value(const eval_view *, const graph *, const node *, int output, int pin, pin_value *);
const char *eval_view_format_value(const eval_view *, const pin_value *);
const char *eval_view_output(const eval_view *, const node *, int pin);
const char *eval_view_input(const eval_view *, const graph *, const node *, int pin);
#endif
