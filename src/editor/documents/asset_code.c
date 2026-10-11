#include "editor/documents/asset_code.h"

#include "assets/scene_document.h"
#include "authoring/source_intelligence.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/scene_internal.h"
#include "execution/backend.h"
#include "graph/document/serialize.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/file_io.h"
#include "support/text_edit.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Seconds of quiet that end an undo step of typing. */
static const double TYPING_PAUSE = 1.0;
/* Seconds between reads of the file for edits made outside Studio. */
static const double DISK_POLL = 0.5;

typedef struct
{
    char *text, *metadata;
} source_snapshot;
struct asset_code
{
    char *path, *text, *disk, *valid, *valid_metadata, *metadata, *disk_metadata;
    char *observed_metadata; /* Exact disk bytes; generated layout may form a clean baseline. */
    char *content_root;
    asset_source *source; /* Declarative KDL only. */
    script_syntax *syntax; /* Canonical draft before project context is attached. */
    authoring_sources *working, *last_valid, *execution;
    const authoring_source *facts;
    authoring_navigation *navigation;
    int document;
    authoring_source_presentation *presentation, *valid_presentation,
        *execution_presentation;
    preview_world *world; /* Bound scene; semantic generations are retained independently. */
    behavior_asset *candidate, *shared; /* Native KDL graph documents only. */
    const behavior_asset *shared_asset;
    unsigned shared_revision;
    bool source_pending, replaying, runtime_held, graph_draft, native_view,
        source_gesture, typing;
    uint64_t execution_graph, execution_node;
    double time, apply_due, typed_at, refresh_due;
    kit_code_editor *editor;
    kit_code_span *spans;
    kit_code_marker *markers, *breakpoints;
    asset_source_completions completions;
    authoring_completions source_completions;
    kit_code_completion *items;
    char *hover;
    source_snapshot *history;
    char **names;
    int step;
    bool completing;
    char error[256], preparation_error[256];
    const prototype_registry *registry;
    uintptr_t state_identity;
    unsigned long long overlay_revision;
};
static void complete(asset_code *, bool);
static void understand_with_metadata(asset_code *, const char *, const char *);
static void understand(asset_code *, const char *);
static void valid_overlay(asset_code *, bool);
static void position(const char *text, int offset, int *line, int *column)
{
    *line = *column = 1;
    for (int i = 0; text[i] && i < offset; i++) {
        if (text[i] == '\n') {
            (*line)++;
            *column = 1;
        } else {
            (*column)++;
        }
    }
}
static char *metadata_path(const asset_code *c)
{
    size_t size = strlen(c->path) + sizeof ".meta";
    char *path = xmalloc(size);
    snprintf(path, size, "%s.meta", c->path);
    return path;
}
static char *read_metadata(const asset_code *c)
{
    if (!c->native_view) {
        return NULL;
    }
    char *path = metadata_path(c), *text = file_read_text(path);
    free(path);
    return text;
}
static kit_code_editor *create_view(asset_code *c, const char *text)
{
    size_t length = strlen(c->path), suffix = strlen(CODE_ASSET_EXTENSION);
    c->native_view = length >= suffix &&
                     !strcmp(c->path + length - suffix, CODE_ASSET_EXTENSION);
    if (!c->native_view &&
        !(length >= strlen(DECLARATIVE_ASSET_EXTENSION) &&
          !strcmp(c->path + length - strlen(DECLARATIVE_ASSET_EXTENSION), DECLARATIVE_ASSET_EXTENSION))) {
        script_syntax *syntax = script_parse(text);
        TSNode root = ts_tree_root_node(syntax->tree);
        for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) {
            const char *type = ts_node_type(ts_node_named_child(root, i));
            if (!strcmp(type, "behavior") || !strcmp(type, "library") ||
                !strcmp(type, "system")) {
                c->native_view = true;
                break;
            }
        }
        script_syntax_free(syntax);
    }
    return kit_code_editor_create(text);
}
static const authoring_source *find_source(const authoring_sources *sources, const char *path)
{
    for (int i = 0; sources && i < authoring_sources_count(sources); i++) {
        const authoring_source *s = authoring_sources_at(sources, i);
        if (!strcmp(s->path, path)) {
            return s;
        }
    }
    return NULL;
}
static void navigation(asset_code *c)
{
    authoring_navigation_free(c->navigation);
    c->navigation = NULL;
    c->document = -1;
    if (!c->working) {
        return;
    }
    authoring_document *documents = NULL;
    for (int i = 0; i < authoring_sources_count(c->working); i++) {
        const authoring_source *s = authoring_sources_at(c->working, i);
        if (!strcmp(s->path, c->path)) {
            c->document = acount(documents);
        }
        apush(documents, ((authoring_document){ s->path, s->syntax, s->analysis }));
    }
    c->navigation = authoring_navigation_new_project(
        documents,
        acount(documents),
        authoring_sources_catalog(c->working),
        authoring_sources_definitions(c->working),
        authoring_sources_definition_count(c->working),
        authoring_sources_references(c->working),
        authoring_sources_reference_count(c->working)
    );
    afree(documents);
}
static void adopt_working(asset_code *c, const authoring_sources *sources)
{
    authoring_sources *owned = authoring_sources_retain(sources);
    authoring_navigation_free(c->navigation);
    c->navigation = NULL;
    authoring_sources_free(c->working);
    c->working = owned;
    c->facts = find_source(owned, c->path);
    if (c->facts && strcmp(c->facts->syntax->text, c->text)) {
        c->facts = NULL;
    }
    if (c->facts) {
        navigation(c);
    }
}
static void mark(asset_code *c, const script_diagnostic *d, kit_code_severity severity)
{
    apush(
        c->markers,
        ((kit_code_marker){ .line = d->range.line,
                            .severity = severity,
                            .message = d->message,
                            .start = d->range.start,
                            .end = d->range.end })
    );
}
static void diagnostic(asset_code *c, const script_diagnostic *d)
{
    mark(c, d, KIT_CODE_ERROR);
    if (!c->error[0]) {
        snprintf(c->error, sizeof c->error, "%s", d->message);
    }
}
static void breakpoint_markers(asset_code *c)
{
    if (!c->presentation ||
        strcmp(authoring_source_presentation_text(c->presentation), c->text)) {
        return;
    }
    for (int i = 0; i < authoring_source_presentation_origin_count(c->presentation);
         i++) {
        const source_presentation_origin
            *o = authoring_source_presentation_origin_at(c->presentation, i);
        if (!o->node || o->pin || o->range.start < 0) {
            continue;
        }
        graph *g = behavior_asset_find_graph(
            authoring_source_presentation_view(c->presentation),
            o->graph
        );
        node *n = graph_node_by_id(g, o->node);
        if (!n || !n->breakpoint) {
            continue;
        }
        int line, column;
        position(c->text, o->range.start, &line, &column);
        apush(
            c->breakpoints,
            ((kit_code_marker){ .line = line,
                                .severity = KIT_CODE_BREAKPOINT_MARKER,
                                .message = "Breakpoint (F9 to remove)",
                                .start = o->range.start,
                                .end = o->range.end,
                                .indicator_only = true })
        );
    }
}
static void decorate(asset_code *c)
{
    asetlen(c->spans, 0);
    asetlen(c->markers, 0);
    asetlen(c->breakpoints, 0);
    c->error[0] = 0;
    if (c->native_view) {
        if (c->facts) {
            for (int i = 0; i < c->facts->diagnostic_count; i++) {
                diagnostic(c, &c->facts->diagnostics[i]);
            }
            /* Unit mismatches warn; the source still runs. */
            const script_analysis *analysis = c->facts->analysis;
            for (int i = 0; i < analysis->warning_count; i++) {
                mark(c, &analysis->warnings[i], KIT_CODE_WARNING);
            }
            int count = 0;
            authoring_highlight *spans = authoring_source_highlights(c->navigation, c->document, &count);
            for (int i = 0; i < count; i++) {
                apush(
                    c->spans,
                    ((kit_code_span){ spans[i].range.start,
                                      spans[i].range.end,
                                      spans[i].cls })
                );
            }
            free(spans);
        } else {
            for (int i = 0; c->syntax && i < c->syntax->diagnostic_count; i++) {
                diagnostic(c, &c->syntax->diagnostics[i]);
            }
            if (c->preparation_error[0]) {
                apush(
                    c->markers,
                    ((kit_code_marker){ .line = 1,
                                        .severity = KIT_CODE_ERROR,
                                        .message = c->preparation_error })
                );
                snprintf(c->error, sizeof c->error, "%s", c->preparation_error);
            }
        }
        breakpoint_markers(c);
        return;
    }
    for (int i = 0; i < acount(c->source->spans); i++) {
        asset_source_span s = c->source->spans[i];
        apush(c->spans, ((kit_code_span){ s.start, s.end, s.cls }));
    }
    for (int i = 0; i < acount(c->source->diagnostics); i++) {
        const asset_source_diagnostic *d = &c->source->diagnostics[i];
        apush(
            c->markers,
            ((kit_code_marker){ .line = d->line,
                                .severity = KIT_CODE_ERROR,
                                .message = d->message,
                                .start = d->start,
                                .end = d->end })
        );
        if (!c->error[0]) {
            snprintf(c->error, sizeof c->error, "%s", d->message);
        }
    }
}

static size_t visible_length(const asset_code *, const char *text)
{
    return strlen(text);
}

static char *visible_text(asset_code *, const char *text)
{
    return xstrdup(text);
}

static char *edited_text(asset_code *c)
{
    return xstrdup(kit_code_text(c->editor, NULL));
}

static void follow(asset_code *c, const char *source, bool land)
{
    kit_code_editor *editor = c->editor;
    char *text = visible_text(c, source);
    int cursor = kit_code_cursor(editor), anchor = cursor, selection_start,
        selection_end;
    if (kit_code_selection(editor, &selection_start, &selection_end)) {
        anchor = cursor == selection_start ? selection_end : selection_start;
    }
    const char *shown = kit_code_text(editor, NULL);
    size_t start = 0, a = strlen(shown), b = strlen(text);
    while (start < a && start < b && shown[start] == text[start]) {
        start++;
    }
    if (start == a && start == b) {
        free(text);
        return;
    }
    while (a > start && b > start && shown[a - 1] == text[b - 1]) {
        a--;
        b--;
    }
    char *replacement = xmalloc(b - start + 1);
    memcpy(replacement, text + start, b - start);
    replacement[b - start] = 0;
    text_edit edit = { (int)start, (int)a, replacement };
    kit_code_apply(editor, &edit, 1);
    if (land) {
        kit_code_set_cursor(editor, (int)b);
    } else {
        int positions[] = { anchor, cursor };
        for (int i = 0; i < 2; i++) {
            if (positions[i] > (int)a) {
                positions[i] += (int)b - (int)a;
            } else if (positions[i] > (int)b) {
                positions[i] = (int)b;
            }
        }
        kit_code_set_selection(editor, positions[0], positions[1]);
    }
    free(replacement);
    free(text);
}

static void check_asset(asset_code *c)
{
    if (!c->source->document || acount(c->source->diagnostics)) {
        return;
    }
    av_value *root = av_doc_root(c->source->document);
    char error[256] = "";
    bool ok = true;
    if (strstr(c->path, "/scenes/")) {
        scene_document *scene = scene_document_load_value(root, error, sizeof error);
        ok = scene != NULL;
        scene_document_free(scene);
    } else if (strstr(c->path, "/prototypes/") && c->registry) {
        prototype *prototype = prototype_load_value(root, c->registry, error, sizeof error);
        ok = prototype != NULL;
        prototype_free(prototype);
    } else if (
        av_obj_get(root, "graphs") || strstr(c->path, "/behaviors/") ||
        strstr(c->path, "/libraries/") || strstr(c->path, "/events/") ||
        strstr(c->path, "/materials/") || strstr(c->path, "/shapes/") ||
        strstr(c->path, "/fields/") || strstr(c->path, "/models/")
    ) {
        behavior_asset *asset = behavior_asset_from_value(root);
        ok = asset != NULL;
        if (!ok) {
            snprintf(error, sizeof error, "%s", graph_last_error());
        }
        behavior_asset_free(asset);
    }
    if (ok) {
        return;
    }
    asset_source_diagnostic d = { .line = 1, .column = 1 };
    snprintf(d.message, sizeof d.message, "%s", *error ? error : "Invalid asset document");
    apush(c->source->diagnostics, d);
}

static void snapshot_free(source_snapshot snapshot)
{
    free(snapshot.text);
    free(snapshot.metadata);
}

static source_snapshot snapshot_create(asset_code *c, const char *text)
{
    return (source_snapshot){ xstrdup(text), c->metadata ? xstrdup(c->metadata) : NULL };
}

static void record(asset_code *c, const char *text)
{
    c->source_gesture = false;
    c->typing = false;
    while (acount(c->history) > c->step + 1) {
        snapshot_free(alast(c->history));
        apop(c->history);
    }
    apush(c->history, snapshot_create(c, text));
    c->step++;
}

av_value *asset_code_capture(asset_code *c)
{
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_doc_set_root(doc, root);
    av_obj_add_str(doc, root, "path", c->path);
    av_obj_add_str(doc, root, "text", c->text);
    av_obj_add_str(doc, root, "disk", c->disk);
    if (c->disk_metadata) {
        av_obj_add_str(doc, root, "disk_metadata", c->disk_metadata);
    }
    if (c->observed_metadata) {
        av_obj_add_str(doc, root, "observed_metadata", c->observed_metadata);
    }
    if (c->metadata) {
        av_obj_add_str(doc, root, "metadata", c->metadata);
    }
    if (c->valid) {
        av_obj_add_str(doc, root, "valid", c->valid);
        if (c->valid_metadata) {
            av_obj_add_str(doc, root, "valid_metadata", c->valid_metadata);
        }
    }
    av_obj_add_bool(doc, root, "overlay", text_overlay_text(c->path) != NULL);
    av_obj_add_int(doc, root, "step", c->step);
    if (c->runtime_held) {
        av_obj_add_bool(doc, root, "runtime_hold", true);
    }
    if (c->graph_draft) {
        av_obj_add_bool(doc, root, "graph_draft", true);
    }
    av_value *history = av_obj_add_arr(doc, root, "history");
    for (int i = 0; i < acount(c->history); i++) {
        av_value *snapshot = av_arr_add_obj(doc, history);
        av_obj_add_str(doc, snapshot, "text", c->history[i].text);
        if (c->history[i].metadata) {
            av_obj_add_str(doc, snapshot, "metadata", c->history[i].metadata);
        }
    }
    int cursor = kit_code_cursor(c->editor), start, end;
    int anchor = cursor;
    if (kit_code_selection(c->editor, &start, &end)) {
        anchor = cursor == start ? end : start;
    }
    av_obj_add_int(doc, root, "cursor", cursor);
    av_obj_add_int(doc, root, "anchor", anchor);
    av_value *state = av_value_copy(root);
    av_doc_free(doc);
    c->state_identity = (uintptr_t)state;
    return state;
}

bool asset_code_has_state(const asset_code *c, const av_value *state)
{
    return c->state_identity == (uintptr_t)state;
}

const char *asset_code_path(const asset_code *c)
{
    return c->path;
}

bool asset_code_dirty(const asset_code *c)
{
    return strcmp(c->disk, c->text) != 0 ||
           strcmp(c->metadata ? c->metadata : "", c->disk_metadata ? c->disk_metadata : "") !=
               0;
}

const char *asset_code_error(const asset_code *c)
{
    return c->error;
}

const char *asset_code_text(const asset_code *c)
{
    return c->text;
}

kit_code_editor *asset_code_editor(asset_code *c)
{
    return c->editor;
}

bool asset_code_can_replay(const asset_code *c, bool redo)
{
    int next = c->step + (redo ? 1 : -1);
    return next >= 0 && next < acount(c->history);
}

bool asset_code_replay(asset_code *c, bool redo, bool live)
{
    c->source_gesture = false;
    c->typing = false;
    if (!asset_code_can_replay(c, redo)) {
        return false;
    }
    c->step += redo ? 1 : -1;
    source_snapshot snapshot = c->history[c->step];
    follow(c, snapshot.text, true);
    understand_with_metadata(c, snapshot.text, snapshot.metadata);
    c->replaying = true;
    valid_overlay(c, live);
    c->completing = false;
    return true;
}

/* Replaces the newest step instead of adding one. */
static bool amendable(const asset_code *c)
{
    return c->step > 0 && c->step == acount(c->history) - 1;
}

static void amend_step(asset_code *c, const char *text)
{
    snapshot_free(c->history[c->step]);
    c->history[c->step] = snapshot_create(c, text);
}

static bool replace_source(asset_code *c, const char *before, const char *text, const char *metadata, bool amend)
{
    if (strcmp(before, c->text)) {
        return false;
    }
    follow(c, text, false);
    understand_with_metadata(c, text, metadata);
    if (amend) {
        amend_step(c, text);
    } else {
        record(c, text);
    }
    c->apply_due = c->time;
    c->completing = false;
    return true;
}

bool asset_code_replace_with_metadata(asset_code *c, const char *before, const char *text, const char *metadata)
{
    return replace_source(c, before, text, metadata, false);
}

bool asset_code_edit_source(asset_code *c, const char *before, const char *text, const char *metadata, bool coalesce)
{
    bool amend = coalesce && c->source_gesture && amendable(c);
    if (!replace_source(c, before, text, metadata, amend)) {
        return false;
    }
    c->source_gesture = true;
    return true;
}

bool asset_code_replace(asset_code *c, const char *before, const char *text)
{
    return asset_code_replace_with_metadata(c, before, text, NULL);
}

void asset_code_time(asset_code *c, double time)
{
    c->time = time;
}

void asset_code_end_typing(asset_code *c)
{
    c->typing = false;
}

bool asset_code_pending(const asset_code *c)
{
    return c->source_pending;
}

bool asset_code_graph_blocked(const asset_code *c)
{
    return c->source_pending || (c->error[0] && !c->graph_draft);
}

bool asset_code_runtime_held(const asset_code *c)
{
    return c->runtime_held;
}

void asset_code_hold_runtime(asset_code *c, bool hold)
{
    c->runtime_held = hold;
}

const kit_code_marker *asset_code_markers(const asset_code *c)
{
    return c->markers;
}

void asset_code_set_names(asset_code *c, const char *const *names, int count)
{
    bool same = count == acount(c->names);
    for (int i = 0; same && i < count; i++) {
        same = !strcmp(c->names[i], names[i]);
    }
    if (same) {
        return;
    }
    for (int i = 0; i < acount(c->names); i++) {
        free(c->names[i]);
    }
    asetlen(c->names, 0);
    for (int i = 0; i < count; i++) {
        apush(c->names, xstrdup(names[i]));
    }
}

static av_value *merge(
    av_doc *doc,
    const av_value *base,
    const av_value *current,
    const av_value *incoming,
    bool *ok
)
{
    if (av_equals(base, incoming) || av_equals(current, incoming)) {
        return av_clone(doc, current);
    }
    if (av_equals(base, current)) {
        return av_clone(doc, incoming);
    }
    if (!av_is_obj(base) || !av_is_obj(current) || !av_is_obj(incoming)) {
        *ok = false;
        return NULL;
    }
    av_value *result = av_obj(doc);
    for (size_t i = 0; i < av_obj_size(current); i++) {
        const char *key = av_get_str(av_obj_key_at(current, i));
        const av_value *a = av_obj_get(base, key), *b = av_obj_get(current, key),
                       *c = av_obj_get(incoming, key);
        if (!c) {
            if (a && !av_equals(a, b)) {
                *ok = false;
                return NULL;
            }
            if (!a) {
                av_obj_add_val(doc, result, key, av_clone(doc, b));
            }
            continue;
        }
        av_value *value = merge(doc, a, b, c, ok);
        if (!*ok) {
            return NULL;
        }
        av_obj_add_val(doc, result, key, value);
    }
    for (size_t i = 0; i < av_obj_size(incoming); i++) {
        const char *key = av_get_str(av_obj_key_at(incoming, i));
        if (av_obj_get(current, key)) {
            continue;
        }
        const av_value *a = av_obj_get(base, key), *c = av_obj_get(incoming, key);
        if (a && !av_equals(a, c)) {
            *ok = false;
            return NULL;
        }
        if (!a) {
            av_obj_add_val(doc, result, key, av_clone(doc, c));
        }
    }
    return result;
}

static void understand_with_metadata(asset_code *c, const char *text, const char *metadata)
{
    char *next = xstrdup(text), *layout = metadata ? xstrdup(metadata) : NULL;
    free(c->text);
    c->text = next;
    asset_source_free(c->source);
    c->source = NULL;
    script_syntax_free(c->syntax);
    c->syntax = NULL;
    behavior_asset_free(c->candidate);
    c->candidate = NULL;
    authoring_navigation_free(c->navigation);
    c->navigation = NULL;
    authoring_sources_free(c->working);
    c->working = NULL;
    c->facts = NULL;
    c->preparation_error[0] = 0;
    if (c->native_view) {
        c->syntax = script_parse(c->text);
        if (c->world) {
            char error[256] = "";
            int status = preview_source_edit(c->world, c->path, c->text, error, sizeof error);
            const authoring_sources *working = preview_sources_working(c->world);
            if (working) {
                adopt_working(c, working);
            }
            if (status && !c->facts) {
                snprintf(c->preparation_error, sizeof c->preparation_error, "%s", error);
            }
        }
        authoring_source_presentation *p = c->facts
                                             ? authoring_source_presentation_new(
                                                   c->facts->analysis,
                                                   authoring_sources_catalog(c->working),
                                                   c->presentation ? c->presentation
                                                                   : c->valid_presentation
                                               )
                                             : NULL;
        if (p) {
            if (layout) {
                authoring_source_presentation_restore(p, layout);
            }
            authoring_source_presentation_free(c->presentation);
            c->presentation = p;
        }
    } else {
        c->source = asset_source_parse(c->text);
        check_asset(c);
        av_value *root = c->source->document ? av_doc_root(c->source->document) : NULL;
        if (root && av_obj_get(root, "graphs")) {
            c->candidate = behavior_asset_from_value(root);
        }
    }
    free(c->metadata);
    c->metadata = c->presentation &&
                          !strcmp(authoring_source_presentation_text(c->presentation), c->text)
                    ? authoring_source_presentation_metadata(c->presentation)
                : layout ? xstrdup(layout)
                         : NULL;
    free(layout);
    /* Only text a graph follows waits for validation; other KDL is checked
       as it parses. */
    c->source_pending = c->native_view || c->candidate;
    c->replaying = false;
    c->graph_draft = false;
    decorate(c);
}
static void understand(asset_code *c, const char *text)
{
    char *metadata = c->metadata && c->text && !strcmp(text, c->text)
                       ? xstrdup(c->metadata)
                       : NULL;
    understand_with_metadata(c, text, metadata);
    free(metadata);
}
static void valid_overlay(asset_code *c, bool live)
{
    if (c->source_pending || c->error[0] ||
        (c->native_view && (!c->facts || !c->facts->valid))) {
        return;
    }
    free(c->valid);
    c->valid = xstrdup(c->text);
    free(c->valid_metadata);
    c->valid_metadata = c->metadata ? xstrdup(c->metadata) : NULL;
    if (c->native_view) {
        authoring_sources_free(c->last_valid);
        c->last_valid = authoring_sources_retain(c->working);
        authoring_source_presentation_free(c->valid_presentation);
        c->valid_presentation = authoring_source_presentation_retain(c->presentation);
    }
    if (live) {
        text_overlay_set(c->path, c->valid);
        c->overlay_revision = text_overlay_revision(c->path);
    }
}
asset_code *asset_code_open(const char *path, char *error, size_t size)
{
    char *disk = file_read_text(path);
    if (!disk) {
        snprintf(error, size, "Could not read %s", path);
        return NULL;
    }
    asset_code *c = xcalloc(1, sizeof *c);
    c->document = -1;
    c->path = xstrdup(path);
    c->disk = disk;
    const char *text = text_overlay_text(path);
    text = text ? text : disk;
    c->editor = create_view(c, text);
    c->overlay_revision = text_overlay_revision(path);
    c->disk_metadata = read_metadata(c);
    c->observed_metadata = c->disk_metadata ? xstrdup(c->disk_metadata) : NULL;
    understand_with_metadata(c, text, c->disk_metadata);
    if (!c->native_view) {
        c->source_pending = false;
        valid_overlay(c, false);
    }
    apush(c->history, snapshot_create(c, text));
    return c;
}
asset_code *asset_code_restore(const av_value *state, const prototype_registry *registry)
{
    const char *path = av_get_str(av_obj_get(state, "path")),
               *text = av_get_str(av_obj_get(state, "text"));
    const char *disk = av_get_str(av_obj_get(state, "disk"));
    av_value *history = av_obj_get(state, "history");
    int step = av_get_int(av_obj_get(state, "step"));
    if (!path || !text || !disk || !av_is_arr(history) || step < 0 ||
        (size_t)step >= av_arr_size(history)) {
        return NULL;
    }
    for (size_t i = 0; i < av_arr_size(history); i++) {
        if (!av_get_str(av_obj_get(av_arr_get(history, i), "text"))) {
            return NULL;
        }
    }
    asset_code *c = xcalloc(1, sizeof *c);
    c->document = -1;
    c->path = xstrdup(path);
    c->disk = xstrdup(disk);
    c->registry = registry;
    c->runtime_held = av_get_bool(av_obj_get(state, "runtime_hold"));
    c->state_identity = (uintptr_t)state;
    const char *valid = av_get_str(av_obj_get(state, "valid"));
    c->valid = valid ? xstrdup(valid) : NULL;
    const char *valid_metadata = av_get_str(av_obj_get(state, "valid_metadata"));
    c->valid_metadata = valid_metadata ? xstrdup(valid_metadata) : NULL;
    const char *disk_metadata = av_get_str(av_obj_get(state, "disk_metadata"));
    c->disk_metadata = disk_metadata ? xstrdup(disk_metadata) : NULL;
    const char *observed = av_get_str(av_obj_get(state, "observed_metadata"));
    c->observed_metadata = observed ? xstrdup(observed) : NULL;
    c->editor = create_view(c, text);
    understand_with_metadata(c, text, av_get_str(av_obj_get(state, "metadata")));
    c->graph_draft = av_get_bool(av_obj_get(state, "graph_draft"));
    for (size_t i = 0; i < av_arr_size(history); i++) {
        const av_value *s = av_arr_get(history, i);
        const char *m = av_get_str(av_obj_get(s, "metadata"));
        apush(
            c->history,
            ((source_snapshot){ xstrdup(av_get_str(av_obj_get(s, "text"))),
                                m ? xstrdup(m) : NULL })
        );
    }
    c->step = step;
    int cursor = av_get_int(av_obj_get(state, "cursor")),
        anchor = av_get_int(av_obj_get(state, "anchor"));
    int length = (int)strlen(text);
    if (cursor < 0 || cursor > length) {
        cursor = length;
    }
    if (anchor < 0 || anchor > length) {
        anchor = cursor;
    }
    kit_code_set_selection(c->editor, anchor, cursor);
    if (c->valid && av_get_bool(av_obj_get(state, "overlay"))) {
        text_overlay_set(path, c->valid);
        c->overlay_revision = text_overlay_revision(path);
    }
    return c;
}
void asset_code_adopt_overlay(asset_code *c)
{
    c->overlay_revision = text_overlay_revision(c->path);
}
void asset_code_free(asset_code *c)
{
    if (!c) {
        return;
    }
    if (c->overlay_revision && c->overlay_revision == text_overlay_revision(c->path)) {
        text_overlay_set(c->path, NULL);
    }
    authoring_navigation_free(c->navigation);
    authoring_sources_free(c->working);
    authoring_sources_free(c->last_valid);
    authoring_sources_free(c->execution);
    authoring_source_presentation_free(c->presentation);
    authoring_source_presentation_free(c->valid_presentation);
    authoring_source_presentation_free(c->execution_presentation);
    script_syntax_free(c->syntax);
    asset_source_free(c->source);
    behavior_asset_free(c->candidate);
    behavior_asset_free(c->shared);
    kit_code_editor_destroy(c->editor);
    authoring_completions_free(&c->source_completions);
    asset_source_completions_free(&c->completions);
    for (int i = 0; i < acount(c->history); i++) {
        snapshot_free(c->history[i]);
    }
    for (int i = 0; i < acount(c->names); i++) {
        free(c->names[i]);
    }
    afree(c->history);
    afree(c->names);
    afree(c->items);
    afree(c->spans);
    afree(c->markers);
    afree(c->breakpoints);
    free(c->path);
    free(c->text);
    free(c->disk);
    free(c->valid);
    free(c->valid_metadata);
    free(c->metadata);
    free(c->disk_metadata);
    free(c->observed_metadata);
    free(c->content_root);
    free(c->hover);
    free(c);
}
void asset_code_set_registry(asset_code *c, const prototype_registry *registry)
{
    if (c->registry == registry) {
        return;
    }
    c->registry = registry;
    if (!c->native_view) {
        understand(c, c->text);
    }
}
void asset_code_set_source_world(asset_code *c, preview_world *world, const char *content_root)
{
    bool clean = !asset_code_dirty(c);
    bool same = c->world == world &&
                !strcmp(c->content_root ? c->content_root : "", content_root ? content_root : "");
    if (same && (!world || !c->native_view ||
                 preview_sources_working(world) == c->working)) {
        return;
    }
    if (same) {
        const authoring_sources *fresh = preview_sources_working(world);
        const authoring_source *source = find_source(fresh, c->path);
        if (!source || strcmp(source->syntax->text, c->text)) {
            return;
        }
        adopt_working(c, fresh);
        authoring_source_presentation *p = authoring_source_presentation_new(
            c->facts->analysis,
            authoring_sources_catalog(c->working),
            c->presentation
        );
        if (p) {
            authoring_source_presentation_free(c->presentation);
            c->presentation = p;
            free(c->metadata);
            c->metadata = authoring_source_presentation_metadata(p);
            if (clean) {
                free(c->disk_metadata);
                c->disk_metadata = c->metadata ? xstrdup(c->metadata) : NULL;
            }
        }
        c->source_pending = c->native_view || c->candidate;
        decorate(c);
        return;
    }
    c->world = world;
    free(c->content_root);
    c->content_root = content_root ? xstrdup(content_root) : NULL;
    if (!world || !c->native_view) {
        return;
    }
    char error[256];
    if (!preview_sources_working(world) && content_root) {
        preview_sources_prepare(world, content_root, behavior_default_backend(), error, sizeof error);
    }
    if (c->valid && !c->last_valid && strcmp(c->valid, c->text)) {
        const authoring_sources *applied = preview_sources_applied(world);
        const authoring_source *source = find_source(applied, c->path);
        if (!source || strcmp(source->syntax->text, c->valid)) {
            preview_source_edit(world, c->path, c->valid, error, sizeof error);
            applied = preview_sources_working(world);
            source = find_source(applied, c->path);
        }
        if (source && source->valid && !strcmp(source->syntax->text, c->valid)) {
            c->last_valid = authoring_sources_retain(applied);
            c->valid_presentation = authoring_source_presentation_new(
                source->analysis,
                authoring_sources_catalog(applied),
                NULL
            );
            if (c->valid_presentation && c->valid_metadata) {
                authoring_source_presentation_restore(c->valid_presentation, c->valid_metadata);
            }
        }
    }
    understand(c, c->text);
    if (clean) {
        free(c->disk_metadata);
        c->disk_metadata = c->metadata ? xstrdup(c->metadata) : NULL;
        if (c->step < acount(c->history)) {
            free(c->history[c->step].metadata);
            c->history[c->step].metadata = c->metadata ? xstrdup(c->metadata) : NULL;
        }
    }
}
bool asset_code_validate(asset_code *c, editor *)
{
    if (c->source_pending && c->time < c->apply_due) {
        return false;
    }
    if (c->error[0] || (c->native_view && (!c->facts || !c->facts->valid))) {
        return false;
    }
    c->source_pending = false;
    valid_overlay(c, false);
    return true;
}

const authoring_source *asset_code_source(const asset_code *c)
{
    return c->facts;
}
const script_syntax *asset_code_syntax(const asset_code *c)
{
    return c->facts ? c->facts->syntax : c->syntax;
}
const script_catalog *asset_code_catalog(const asset_code *c)
{
    return c->working ? authoring_sources_catalog(c->working) : NULL;
}
authoring_source_presentation *asset_code_presentation(const asset_code *c)
{
    return c->presentation;
}
const authoring_navigation *asset_code_navigation(const asset_code *c)
{
    return c->navigation;
}
int asset_code_document(const asset_code *c)
{
    return c->document;
}
const authoring_source *asset_code_graph_source(const asset_code *c, const editor *e)
{
    if (editor_has_pending_edits(e) ||
        (c->shared_asset && (c->shared_asset != editor_behavior_asset(e) ||
                             c->shared_revision != editor_edit_revision(e)))) {
        return NULL;
    }
    return c->source_pending || c->error[0] ? find_source(c->last_valid, c->path)
                                            : c->facts;
}
bool asset_code_sync_graph(asset_code *c, editor *e, bool live)
{
    /* Preparing source replaces the world's working presentations, including
       library assets borrowed by the palette. Refresh even for invalid drafts
       or unchanged graph state before the editor can read those assets. */
    if (c->world) {
        editor_refresh_asset_catalog(e, c->world);
    }
    if (!c->native_view && !c->shared && !c->source_pending && !asset_code_dirty(c)) {
        c->shared = behavior_asset_clone(editor_behavior_asset(e));
        c->shared_asset = editor_behavior_asset(e);
        c->shared_revision = editor_edit_revision(e);
        return false;
    }
    if (c->source_pending) {
        if (!asset_code_validate(c, e)) {
            return false;
        }
        if (c->native_view) {
            if (!c->presentation || editor_source_presentation(e, c->presentation)) {
                snprintf(c->error, sizeof c->error, "Could not apply source presentation");
                return false;
            }
        } else if (c->candidate && editor_declarative_graph(e, c->candidate)) {
            snprintf(c->error, sizeof c->error, "Could not apply declarative graph");
            return false;
        }
        if (c->replaying) {
            e->arrange_pending = false;
        }
        c->replaying = false;
        c->shared_asset = editor_behavior_asset(e);
        c->shared_revision = editor_edit_revision(e);
        behavior_asset_free(c->shared);
        c->shared = c->native_view ? NULL : behavior_asset_clone(c->shared_asset);
        valid_overlay(c, live);
        return true;
    }
    if (c->shared_asset == editor_behavior_asset(e) &&
        c->shared_revision == editor_edit_revision(e)) {
        return false;
    }
    if (c->error[0]) {
        return false;
    }
    if (c->native_view) {
        const authoring_source_presentation *shown = editor_presentation(e);
        if (!shown || strcmp(authoring_source_presentation_text(shown), c->text)) {
            return false;
        }
        /* Source edits arrive as explicit patches. A graph-only revision here
         * is optional presentation state and never executable source. */
        char *metadata = authoring_source_presentation_metadata(shown);
        bool changed = strcmp(metadata ? metadata : "", c->metadata ? c->metadata : "") !=
                       0;
        if (changed) {
            authoring_source_presentation_free(c->presentation);
            c->presentation = authoring_source_presentation_clone(shown);
            free(c->metadata);
            c->metadata = metadata;
            metadata = NULL;
            record(c, c->text);
            valid_overlay(c, live);
            decorate(c);
        }
        free(metadata);
        c->shared_asset = editor_behavior_asset(e);
        c->shared_revision = editor_edit_revision(e);
        return changed;
    }
    const behavior_asset *working = editor_behavior_asset(e);
    if (!c->shared || behavior_asset_equal(c->shared, working)) {
        c->shared_asset = working;
        c->shared_revision = editor_edit_revision(e);
        return false;
    }
    av_doc *doc = av_doc_new();
    char error[256];
    char *text = asset_source_reconcile(c->text, behavior_asset_to_value(doc, working), error, sizeof error);
    av_doc_free(doc);
    if (!text) {
        snprintf(c->error, sizeof c->error, "%s", error);
        return false;
    }
    follow(c, text, false);
    understand(c, text);
    record(c, text);
    free(text);
    c->apply_due = c->time;
    return asset_code_sync_graph(c, e, live);
}
bool asset_code_location(const asset_code *c, uint64_t graph, uint64_t node, int *start, int *end)
{
    if (!c->presentation ||
        strcmp(authoring_source_presentation_text(c->presentation), c->text)) {
        return false;
    }
    const source_presentation_origin
        *o = authoring_source_presentation_origin(c->presentation, graph, node, 0);
    if (!o || o->range.start < 0) {
        return false;
    }
    *start = o->range.start;
    *end = o->range.end;
    return true;
}
static bool node_at(const asset_code *c, int offset, bool executable, uint64_t *graph_id, uint64_t *node_id)
{
    if (!c->presentation ||
        strcmp(authoring_source_presentation_text(c->presentation), c->text)) {
        return false;
    }
    const source_presentation_origin *best = NULL;
    for (int i = 0; c->presentation &&
                    i < authoring_source_presentation_origin_count(c->presentation);
         i++) {
        const source_presentation_origin
            *o = authoring_source_presentation_origin_at(c->presentation, i);
        if (!o->node || o->pin || offset < o->range.start || offset >= o->range.end) {
            continue;
        }
        if (executable) {
            graph *g = behavior_asset_find_graph(
                authoring_source_presentation_view(c->presentation),
                o->graph
            );
            node *n = g ? graph_node_by_id(g, o->node) : NULL;
            if (!n || node_behavior_for(n) == NODE_BEHAVIOR_PURE) {
                continue;
            }
        }
        if (!best ||
            o->range.end - o->range.start < best->range.end - best->range.start) {
            best = o;
        }
    }
    if (!best) {
        return false;
    }
    *graph_id = best->graph;
    *node_id = best->node;
    return true;
}
bool asset_code_node_at(const asset_code *c, int offset, uint64_t *graph, uint64_t *node)
{
    return node_at(c, offset, false, graph, node);
}
bool asset_code_breakpoint_at(const asset_code *c, int offset, uint64_t *graph, uint64_t *node)
{
    return node_at(c, offset, true, graph, node);
}
bool asset_code_definition(const asset_code *c, int offset, int *start)
{
    const authoring_reference *at = c->navigation
                                      ? authoring_navigation_at(c->navigation, c->document, offset)
                                      : NULL;
    const authoring_reference
        *definition = at ? authoring_navigation_definition(c->navigation, at->target)
                         : NULL;
    if (!definition || definition->location.document != c->document) {
        return false;
    }
    *start = definition->location.range.start;
    return true;
}
asset_source_span *asset_code_references(const asset_code *c, int offset)
{
    const authoring_reference *at = c->navigation
                                      ? authoring_navigation_at(c->navigation, c->document, offset)
                                      : NULL;
    asset_source_span *spans = NULL;
    if (!at) {
        return NULL;
    }
    for (int i = 0; i < authoring_navigation_reference_count(c->navigation); i++) {
        const authoring_reference *r = authoring_navigation_reference_at(c->navigation, i);
        if (r->location.document == c->document &&
            authoring_target_equal(r->target, at->target)) {
            apush(
                spans,
                ((asset_source_span){ r->location.range.start, r->location.range.end, "symbol" })
            );
        }
    }
    return spans;
}
void asset_code_execution(asset_code *c, uint64_t graph, uint64_t node)
{
    c->execution_graph = graph;
    c->execution_node = node;
    if (!node) {
        authoring_sources_free(c->execution);
        c->execution = NULL;
        authoring_source_presentation_free(c->execution_presentation);
        c->execution_presentation = NULL;
        return;
    }
    if (c->execution_presentation || c->runtime_held || !c->last_valid ||
        !c->valid_presentation) {
        return;
    }
    c->execution = authoring_sources_retain(c->last_valid);
    c->execution_presentation = authoring_source_presentation_clone(c->valid_presentation);
}
bool asset_code_execution_location(const asset_code *c, int *start, int *end)
{
    if (!c->execution_presentation ||
        strcmp(authoring_source_presentation_text(c->execution_presentation), c->text)) {
        return false;
    }
    const source_presentation_origin *o = authoring_source_presentation_origin(
        c->execution_presentation,
        c->execution_graph,
        c->execution_node,
        0
    );
    if (!o || o->range.start < 0) {
        return false;
    }
    *start = o->range.start;
    *end = o->range.end;
    return true;
}

typedef struct
{
    size_t start, end;
    char *text;
} changed_span;
static changed_span difference(const char *before, const char *after)
{
    size_t start = 0, a = strlen(before), b = strlen(after);
    while (start < a && start < b && before[start] == after[start]) {
        start++;
    }
    while (a > start && b > start && before[a - 1] == after[b - 1]) {
        a--;
        b--;
    }
    char *text = xmalloc(b - start + 1);
    memcpy(text, after + start, b - start);
    text[b - start] = 0;
    return (changed_span){ start, a, text };
}
static char *merge_source(const char *base, const char *current, const char *incoming)
{
    if (!strcmp(current, base)) {
        return xstrdup(incoming);
    }
    if (!strcmp(incoming, base)) {
        return xstrdup(current);
    }
    if (!strcmp(current, incoming)) {
        return xstrdup(current);
    }
    changed_span a = difference(base, current), b = difference(base, incoming);
    char *result = NULL;
    if (a.end <= b.start && !(a.start == a.end && a.start == b.start)) {
        text_edit edits[] = { { (int)b.start, (int)b.end, b.text },
                              { (int)a.start, (int)a.end, a.text } };
        result = text_edits_apply(base, edits, 2);
    } else if (b.end <= a.start && !(b.start == b.end && b.start == a.start)) {
        text_edit edits[] = { { (int)a.start, (int)a.end, a.text },
                              { (int)b.start, (int)b.end, b.text } };
        result = text_edits_apply(base, edits, 2);
    }
    free(a.text);
    free(b.text);
    return result;
}
void asset_code_refresh(asset_code *c)
{
    char *disk = file_read_text(c->path);
    if (!disk) {
        return;
    }
    char *disk_metadata = read_metadata(c);
    bool layout_changed = strcmp(
                              disk_metadata ? disk_metadata : "",
                              c->observed_metadata ? c->observed_metadata : ""
                          ) != 0;
    if (!strcmp(disk, c->disk) && !layout_changed) {
        free(disk);
        free(disk_metadata);
        return;
    }
    if (layout_changed &&
        strcmp(c->metadata ? c->metadata : "", c->disk_metadata ? c->disk_metadata : "") &&
        strcmp(c->metadata ? c->metadata : "", disk_metadata ? disk_metadata : "")) {
        snprintf(c->error, sizeof c->error, "Presentation metadata conflicts with a disk edit; the current draft is retained");
        free(disk);
        free(disk_metadata);
        return;
    }
    char *text = NULL;
    if (!asset_code_dirty(c)) {
        text = xstrdup(disk);
    } else if (!strcmp(disk, c->disk)) {
        text = xstrdup(c->text);
    } else if (c->native_view) {
        text = merge_source(c->disk, c->text, disk);
    } else {
        char error[256];
        av_doc *base = av_doc_read(c->disk, error, sizeof error),
               *incoming = av_doc_read(disk, error, sizeof error);
        av_doc *merged = av_doc_new();
        bool ok = base && incoming && c->source->document;
        if (ok) {
            av_value *value = merge(
                merged,
                av_doc_root(base),
                av_doc_root(c->source->document),
                av_doc_root(incoming),
                &ok
            );
            if (ok) {
                text = asset_source_reconcile(c->text, value, error, sizeof error);
            }
        }
        av_doc_free(base);
        av_doc_free(incoming);
        av_doc_free(merged);
    }
    if (!text) {
        snprintf(c->error, sizeof c->error, "Source conflicts with a disk edit; the current text is retained");
        free(disk);
        free(disk_metadata);
        return;
    }
    free(c->disk);
    c->disk = disk;
    free(c->disk_metadata);
    c->disk_metadata = disk_metadata;
    free(c->observed_metadata);
    c->observed_metadata = disk_metadata ? xstrdup(disk_metadata) : NULL;
    follow(c, text, false);
    understand_with_metadata(c, text, disk_metadata);
    record(c, text);
    free(text);
    if (!c->native_view) {
        c->source_pending = false;
        valid_overlay(c, true);
    }
}
bool asset_code_save(asset_code *c, char *error, size_t size)
{
    if (c->source_pending || c->error[0] ||
        (c->native_view && (!c->facts || !c->facts->valid))) {
        snprintf(error, size, "%s", c->error[0] ? c->error : "Validate the source before saving");
        return false;
    }
    char *disk = file_read_text(c->path);
    bool changed = !disk || strcmp(disk, c->disk);
    free(disk);
    if (changed) {
        snprintf(error, size, "Asset changed on disk; reopen it before saving");
        return false;
    }
    char *observed = read_metadata(c);
    bool metadata_changed = strcmp(
                                observed ? observed : "",
                                c->observed_metadata ? c->observed_metadata : ""
                            ) != 0;
    free(observed);
    if (metadata_changed) {
        snprintf(error, size, "Presentation metadata changed on disk; refresh it before saving");
        return false;
    }
    if (write_file_atomic(c->path, c->text)) {
        snprintf(error, size, "Could not write %s", c->path);
        return false;
    }
    free(c->disk);
    c->disk = xstrdup(c->text);
    if (c->native_view && c->metadata) {
        char *path = metadata_path(c);
        int status = write_file_atomic(path, c->metadata);
        free(path);
        if (status) {
            snprintf(error, size, "Could not save source presentation metadata for %s", c->path);
            return false;
        }
    }
    free(c->disk_metadata);
    c->disk_metadata = c->metadata ? xstrdup(c->metadata) : NULL;
    free(c->observed_metadata);
    c->observed_metadata = c->metadata ? xstrdup(c->metadata) : NULL;
    c->runtime_held = false;
    c->typing = false;
    valid_overlay(c, true);
    c->error[0] = 0;
    return true;
}
static code_completion_kind completion_kind(authoring_completion_kind kind)
{
    switch (kind) {
        case AUTHORING_COMPLETE_VARIABLE: return CODE_COMPLETE_VALUE;
        case AUTHORING_COMPLETE_CALLABLE:
        case AUTHORING_COMPLETE_EVENT: return CODE_COMPLETE_FUNCTION;
        case AUTHORING_COMPLETE_TYPE: return CODE_COMPLETE_TYPE;
        case AUTHORING_COMPLETE_FIELD: return CODE_COMPLETE_FIELD;
        case AUTHORING_COMPLETE_KEYWORD: return CODE_COMPLETE_KEYWORD;
    }
    assert(false);
    return CODE_COMPLETE_UNKNOWN;
}
/* The completions at the cursor; the list stays closed where nothing was
   typed that asked for them, and closes once the word is complete. */
static void complete(asset_code *c, bool asked)
{
    asset_source_completions_free(&c->completions);
    authoring_completions_free(&c->source_completions);
    asetlen(c->items, 0);
    if (c->native_view) {
        if (c->navigation && c->facts) {
            authoring_source_complete(
                c->navigation,
                c->document,
                kit_code_cursor(c->editor),
                &c->source_completions
            );
        }
        for (int i = 0; i < c->source_completions.count; i++) {
            authoring_completion *s = &c->source_completions.items[i];
            apush(
                c->items,
                ((
                    kit_code_completion
                ){ s->label, s->detail, s->insert, completion_kind(s->kind) })
            );
        }
    } else {
        asset_source_complete(
            c->source,
            kit_code_cursor(c->editor),
            c->path,
            (const char *const *)c->names,
            acount(c->names),
            &c->completions
        );
        for (int i = 0; i < acount(c->completions.items); i++) {
            asset_source_completion *s = &c->completions.items[i];
            apush(c->items, ((kit_code_completion){ s->label, s->detail, s->insert, s->kind }));
        }
    }
    int start = c->native_view ? c->source_completions.range.start
                               : c->completions.start;
    int typed = kit_code_cursor(c->editor) - start, count = acount(c->items);
    bool done = count == 1 && (int)strlen(c->items[0].label) == typed &&
                !strncmp(c->text + start, c->items[0].label, (size_t)typed);
    c->completing = count > 0 && (asked || typed > 0) && !done;
}

/* One character typed at the cursor that starts completion: a word's
   character, a member's dot or a quoted name. */
static bool starts_completion(const char *before, const char *after, int cursor)
{
    size_t length = strlen(before);
    if (cursor < 1 || strlen(after) != length + 1 ||
        strncmp(before, after, (size_t)cursor - 1) ||
        strcmp(before + cursor - 1, after + cursor)) {
        return false;
    }
    char typed = after[cursor - 1];
    return isalnum((unsigned char)typed) || typed == '_' || typed == '.' ||
           typed == '"';
}

/* Typing joins the step before it until a pause, a caret move, a save or
   another kind of edit, so undo takes back a burst instead of one
   character. */
static void typed(asset_code *c, const char *text, bool typing)
{
    if (c->typing && typing && c->time - c->typed_at < TYPING_PAUSE && amendable(c)) {
        amend_step(c, text);
    } else {
        record(c, text);
    }
    c->typing = typing;
    c->typed_at = c->time;
}

/* The symbol under the pointer while the primary modifier is held, when it
   has a definition to go to: Mod+click's underline. */
static void link_symbol(const asset_code *c, kit_code_desc *desc)
{
    int at = kit_code_link(c->editor);
    const authoring_reference *reference = at >= 0 && c->navigation
                                             ? authoring_navigation_at(c->navigation, c->document, at)
                                             : NULL;
    if (reference && authoring_navigation_definition(c->navigation, reference->target)) {
        desc->link_start = reference->location.range.start;
        desc->link_end = reference->location.range.end;
    }
}

kit_code_result asset_code_draw(asset_code *c, kit *k, const char *key, const kit_code_desc *preferences, bool live)
{
    if (c->time >= c->refresh_due || c->time < c->refresh_due - DISK_POLL) {
        c->refresh_due = c->time + DISK_POLL;
        asset_code_refresh(c);
    }
    free(c->hover);
    c->hover = c->native_view
                 ? (c->navigation
                        ? authoring_source_hover(c->navigation, c->document, kit_code_hover(c->editor))
                        : NULL)
                 : asset_source_hover(c->source, kit_code_hover(c->editor));
    kit_code_desc desc = *preferences;
    int length = (int)visible_length(c, c->text);
    kit_code_span *shown_spans = NULL;
    for (int i = 0; i < acount(c->spans); i++) {
        kit_code_span span = c->spans[i];
        if (span.start >= length) {
            continue;
        }
        span.end = span.end < length ? span.end : length;
        apush(shown_spans, span);
    }
    desc.spans = shown_spans;
    desc.span_count = acount(shown_spans);
    desc.markers = c->markers;
    desc.marker_count = acount(c->markers);
    kit_code_marker *shown_markers = NULL;
    for (int i = 0; i < acount(c->markers); i++) {
        if (c->markers[i].start < length) {
            kit_code_marker marker = c->markers[i];
            marker.end = marker.end < length ? marker.end : length;
            apush(shown_markers, marker);
        }
    }
    int start, end;
    for (int i = 0; i < acount(c->breakpoints); i++) {
        if (c->breakpoints[i].start < length) {
            apush(shown_markers, c->breakpoints[i]);
        }
    }
    if (c->execution_node && asset_code_execution_location(c, &start, &end) &&
        start < length) {
        int line, column;
        position(c->text, start, &line, &column);
        apush(
            shown_markers,
            (
                (kit_code_marker){ .line = line,
                                   .severity = KIT_CODE_INFO,
                                   .message = c->error[0] || c->runtime_held ? "Paused in the last valid runtime; this source has unapplied edits"
                                                                             : "Execution paused here",
                                   .start = start,
                                   .end = end }
            )
        );
    }
    desc.markers = shown_markers;
    desc.marker_count = acount(shown_markers);
    desc.completions = c->completing ? c->items : NULL;
    desc.completion_count = c->completing ? acount(c->items) : 0;
    desc.completion_start = c->native_view ? c->source_completions.range.start
                                           : c->completions.start;
    desc.hover = c->hover;
    desc.history = true;
    /* KDL names take hyphens; .chibi names are C's. */
    desc.word_chars = c->native_view ? NULL : "-";
    link_symbol(c, &desc);
    kit_code_result result = kit_code(k, key, c->editor, &desc);
    afree(shown_markers);
    afree(shown_spans);
    bool asked = result.events & KIT_CODE_SUGGEST;
    if (result.events & KIT_CODE_CHANGED) {
        char *text = edited_text(c);
        /* A programmatic follow also changes the widget revision. */
        if (strcmp(text, c->text)) {
            bool typing = !(result.events & (KIT_CODE_COMPLETED | KIT_CODE_STEP));
            asked |= typing &&
                     starts_completion(c->text, text, kit_code_cursor(c->editor));
            understand(c, text);
            typed(c, text, typing);
            c->apply_due = c->time + 0.2;
            valid_overlay(c, live);
        } else {
            result.events &= ~KIT_CODE_CHANGED;
        }
        free(text);
    } else if (result.events & KIT_CODE_CURSOR) {
        c->typing = false;
    }
    if (result.events & (KIT_CODE_UNDO | KIT_CODE_REDO)) {
        asset_code_replay(c, result.events & KIT_CODE_REDO, live);
    }
    if (result.events &
        (KIT_CODE_COMPLETED | KIT_CODE_DISMISSED | KIT_CODE_UNDO | KIT_CODE_REDO)) {
        c->completing = false;
    } else if (asked || (c->completing && result.events & (KIT_CODE_CHANGED | KIT_CODE_CURSOR))) {
        complete(c, asked);
    }
    return result;
}
