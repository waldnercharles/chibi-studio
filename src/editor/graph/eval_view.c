#include "editor/graph/eval_view.h"

#include "capabilities/catalog.h"
#include "support/xalloc.h"
#include "types/value_format.h"

#include <stdlib.h>

struct eval_view
{
    execution_inspector *inspector;
    authoring_source_presentation *presentation;
    uint64_t owner;
    editor_value_formatter formatter;
    void *formatter_user;
    char formatted[8][256];
    unsigned format_slot;
};

eval_view *eval_view_new()
{
    return xcalloc(1, sizeof(eval_view));
}

void eval_view_free(eval_view *view)
{
    if (!view) {
        return;
    }
    execution_inspector_free(view->inspector);
    authoring_source_presentation_free(view->presentation);
    free(view);
}

void eval_view_set_inspector(
    eval_view *view,
    execution_inspector *inspector,
    uint64_t owner,
    const authoring_source_presentation *presentation
)
{
    if (view->inspector != inspector) {
        execution_inspector *retained = inspector
                                          ? execution_inspector_retain(inspector)
                                          : NULL;
        execution_inspector_free(view->inspector);
        view->inspector = retained;
    }
    if (view->presentation != presentation) {
        authoring_source_presentation *retained = presentation
                                                    ? authoring_source_presentation_retain(
                                                          (authoring_source_presentation *)presentation
                                                      )
                                                    : NULL;
        authoring_source_presentation_free(view->presentation);
        view->presentation = retained;
    }
    view->owner = owner;
}

void eval_view_set_formatter(eval_view *view, editor_value_formatter formatter, void *user)
{
    view->formatter = formatter;
    view->formatter_user = user;
}

const char *eval_view_format_value(const eval_view *view, const pin_value *value)
{
    if (view && view->formatter) {
        eval_view *mutable = (eval_view *)view;
        char *buffer = mutable->formatted[mutable->format_slot++ % 8];
        buffer[0] = 0;
        if (view->formatter(value, view->owner, buffer, sizeof mutable->formatted[0], view->formatter_user)) {
            buffer[sizeof mutable->formatted[0] - 1] = 0;
            return buffer;
        }
    }
    return pin_value_to_string(value);
}

const char *eval_view_error(const eval_view *)
{
    return "";
}

bool eval_view_live(const eval_view *view)
{
    return view && view->inspector;
}

static int recorded_value(const eval_view *view, const node *n, int output, int pin, pin_value *out)
{
    if (!view || !view->inspector || !view->presentation) {
        return -1;
    }
    const pin_spec *spec = output ? &n->output_pins[pin] : &n->input_pins[pin];
    const behavior_asset *asset = authoring_source_presentation_view(view->presentation);
    const source_presentation_origin *origin = NULL;
    for (int i = 0; i < acount(asset->graphs) && !origin; i++) {
        if (graph_node_by_id(asset->graphs[i], n->id) == n) {
            origin = authoring_source_presentation_origin(
                view->presentation,
                asset->graphs[i]->graph_id,
                n->id,
                spec->id
            );
        }
    }
    if (!origin) {
        return -1;
    }
    uint64_t module = script_catalog_id(
        "module",
        authoring_source_presentation_identity(view->presentation)
    );
    int ordinal = output ? origin->result_index : origin->parameter;
    if (ordinal < 0) {
        ordinal = origin->port_index;
    }
    for (int pass = 0; pass < (output ? 1 : 2); pass++) {
        uint64_t occurrence = pass ? origin->occurrence : origin->node_occurrence;
        bool recorded_output = pass ? true : !!output;
        int recorded_ordinal = pass ? 0 : ordinal;
        for (int i = execution_inspector_watch_count(view->inspector) - 1; i >= 0; i--) {
            const execution_debug_watch *watch = execution_inspector_watch_at(view->inspector, i);
            if (watch->owner != view->owner || watch->module != module ||
                watch->source.output != recorded_output ||
                watch->source.origin.occurrence != occurrence ||
                watch->source.ordinal != recorded_ordinal) {
                continue;
            }
            if (watch->availability != EXECUTION_WATCH_AVAILABLE) {
                return -1 - watch->availability;
            }
            *out = watch->value;
            return 0;
        }
    }
    return -1;
}

int eval_view_live_value(const eval_view *view, const graph *, const node *n, int output, int pin, pin_value *out)
{
    return recorded_value(view, n, output, pin, out);
}

static const char *unavailable(int status)
{
    switch (status) {
        case -2: return "Reference not read";
        case -3: return "Watch budget exceeded";
        case -4: return "Value unavailable";
        default: return "Not recorded";
    }
}

const char *eval_view_output(const eval_view *view, const node *n, int pin)
{
    if (!n || pin < 0 || pin >= n->output_count || n->output_pins[pin].kind != PIN_DATA) {
        return "";
    }
    pin_value value;
    int status = recorded_value(view, n, true, pin, &value);
    if (status == 0) {
        return eval_view_format_value(view, &value);
    }
    if (eval_view_live(view)) {
        return unavailable(status);
    }
    if (!eval_view_live(view) && n->desc->pure_kind == NODE_PURE_CONST) {
        return eval_view_format_value(view, &n->output_pins[pin].default_value);
    }
    return "";
}

const char *eval_view_input(const eval_view *view, const graph *g, const node *n, int pin)
{
    if (!n || pin < 0 || pin >= n->input_count || n->input_pins[pin].kind != PIN_DATA) {
        return NULL;
    }
    pin_value value;
    int status = recorded_value(view, n, false, pin, &value);
    if (status == 0) {
        return eval_view_format_value(view, &value);
    }
    if (eval_view_live(view)) {
        return unavailable(status);
    }
    const pin_spec *spec = &n->input_pins[pin];
    node *source;
    int source_pin;
    if (graph_input_source(g, n, pin, &source, &source_pin)) {
        return NULL;
    }
    if (spec->binding) {
        return "Input binding unavailable";
    }
    if (spec->type == PIN_HANDLE && (spec->flags & PIN_FLAG_DEFAULT_SELF) &&
        !spec->default_value.u64) {
        return "Self";
    }
    return eval_view_format_value(view, &spec->default_value);
}
