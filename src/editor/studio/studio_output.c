#include "assets/ui_document.h"
#include "graph/authoring/graph_edit.h"
/* Output: the behavior log, compiler problems, the playing scene's runtime
 * errors (Unreal's message log), the operation history (Blender's Info
 * editor), Find References and the Call Hierarchy, as the authored panels
 * Output.rml (its views) and OutputHeader.rml (the area header's view
 * switch, scope, filter and Clear) over the `output` source. Logs follow new
 * output while scrolled to the bottom; problems and errors open their node.
 * Problems cover the open asset as it compiles live, or every graph asset of
 * the project as of its last check; and UI documents' syntax errors and
 * RmlUi warnings, the shown one's or every ui asset's and stylesheet's,
 * whose rows open their place in the Text Editor (workstream Q).
 *
 * Rows are a window of the view's rows (ui_list_window), each moved to its
 * place by translateY, as Virtuoso places items, and keyed: a row that stays
 * in the window keeps its element and its markup, so a log line is
 * highlighted and parsed once, as it arrives, however the window scrolls or
 * the oldest lines drop, and moving a row lays nothing out
 * (docs/studio-rml-migration.md, P4). */
#include "authoring/preview_source.h"
#include "editor/studio/studio_internal.h"
#include "editor/ui/highlight.h"
#include "editor/workspace/editor_internal.h"
#include "graph/compiler/validation.h"
#include "graph/document/problems.h"
#include "support/array.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    OUTPUT_LOG,
    OUTPUT_PROBLEMS,
    OUTPUT_RUNTIME,
    OUTPUT_INFO,
    OUTPUT_FIND,  /* Find References (studio_find.c) */
    OUTPUT_CALLS, /* Call Hierarchy (studio_calls.c) */
    OUTPUT_VIEWS,
};

enum
{
    PROBLEMS_ASSET,
    PROBLEMS_PROJECT,
};

/* A Problems row's key: its section times PROBLEMS_SECTION plus its index. */
enum
{
    SECTION_UI,       /* the shown game UI's error */
    SECTION_DOCUMENT, /* a UI document's problem */
    SECTION_GRAPH,    /* the open asset's diagnostic or a project problem */
};
#define PROBLEMS_SECTION (1 << 24)

#define OUTPUT_LINE 1536
#define RECORDS_MAX 500
/* The List's id in Output.rml. */
#define OUTPUT_LIST "output-list"
/* Rows shown before the List is laid out. */
#define UNLAID_ROWS 40
/* dp of rows past each edge of the view: three of the Log's. */
#define OVERSCAN 60
/* A row of the window that shows nothing yet. */
#define NO_KEY INT64_MIN

static bool contains(const char *text, const char *filter)
{
    if (!filter[0]) {
        return true;
    }
    for (const char *t = text; *t; t++) {
        const char *a = t, *b = filter;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

void studio_record(studio *s, kit_toast_type kind, const char *text)
{
    if (acount(s->records) >= RECORDS_MAX) {
        memmove(
            s->records,
            s->records + 1,
            (size_t)(acount(s->records) - 1) * sizeof *s->records
        );
        asetlen(s->records, acount(s->records) - 1);
        s->records_dropped++;
    }
    studio_record_entry entry = { .time = s->time, .kind = kind };
    snprintf(entry.text, sizeof entry.text, "%s", text);
    apush(s->records, entry);
}

/* ---------- Problems ---------- */

static const compile_report *problems(const studio *s)
{
    if (!s->scene->graph_editor) {
        return NULL;
    }
    editor_query q;
    editor_query_state(s->scene->graph_editor, &q);
    return q.compile;
}

/* "Asset › Graph › Node" for a problem, as far as the asset still holds it. */
static void problem_place(const editor *e, const catalog_problem *problem, char *out, size_t size)
{
    graph *g = behavior_asset_find_graph(problem->asset, problem->graph_id);
    node *n = g && problem->node_id ? graph_node_by_id(g, problem->node_id) : NULL;
    if (!g) {
        snprintf(out, size, "%s", problem->asset->name);
        return;
    }
    const char *name = g->name && g->name[0] ? g->name : "Event Graph";
    if (!n) {
        snprintf(out, size, "%s \xe2\x80\xba %s", problem->asset->name, name);
        return;
    }
    char title[128];
    ed_node_title_in(e, problem->asset, g, graph_inline_owner(g, n), title, sizeof title);
    snprintf(out, size, "%s \xe2\x80\xba %s \xe2\x80\xba %s", problem->asset->name, name, title);
}

static void source_problem(studio_ui_problem **out, const char *path, script_range range, const char *message)
{
    studio_ui_problem row = { .at = { .offset = range.start, .line = range.line },
                              .error = true };
    snprintf(row.at.path, sizeof row.at.path, "%s", path);
    snprintf(row.message, sizeof row.message, "%s", message);
    apush(*out, row);
}

static void source_project_problems(studio *s)
{
    const authoring_sources *sources = game_preview_sources_working(s->scene->live);
    if (!sources) {
        return;
    }
    for (int i = 0; i < authoring_sources_catalog_diagnostic_count(sources); i++) {
        const source_catalog_diagnostic *d = authoring_sources_catalog_diagnostic_at(sources, i);
        source_problem(&s->ui_project_problems, d->path, d->range, d->message);
    }
    for (int i = 0; i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        for (int d = 0; d < source->diagnostic_count; d++) {
            source_problem(
                &s->ui_project_problems,
                source->path,
                source->diagnostics[d].range,
                source->diagnostics[d].message
            );
        }
    }
}

/* Source diagnostics come from the checked working collection. Declarative
 * render graphs and UI documents use their owning validators, refreshed only
 * when the project changes or the user requests another check. */
static void check_project(studio *s)
{
    const editor *e = s->scene->graph_editor;
    const behavior_asset **native = NULL;
    for (int i = 0; e && i < acount(e->asset_catalog); i++) {
        const behavior_asset *asset = e->asset_catalog[i];
        if (behavior_domain_pure(asset->domain)) {
            apush(native, asset);
        }
    }
    catalog_problem *found = behavior_catalog_problems(native, acount(native));
    afree(native);
    asetlen(s->project_problems, 0);
    asetlen(s->ui_project_problems, 0);
    studio_code_project_problems(s, &s->ui_project_problems);
    source_project_problems(s);
    for (int i = 0; i < acount(found); i++) {
        studio_problem p = { .at = { .graph_id = found[i].graph_id,
                                     .node_id = found[i].node_id },
                             .error = found[i].severity == CMPL_ERROR };
        snprintf(p.at.asset, sizeof p.at.asset, "%s", found[i].asset->name);
        snprintf(p.message, sizeof p.message, "%s", found[i].message);
        problem_place(e, &found[i], p.place, sizeof p.place);
        apush(s->project_problems, p);
    }
    afree(found);
    s->problems_checked = true;
}

/* The shown game UI's error leads the rows of either scope (studio_ui_error). */
static int ui_problems(const studio *s)
{
    return s->ui_error[0] != '\0';
}

/* UI documents' problems in the scope shown. */
static const studio_ui_problem *document_problems(studio *s)
{
    return s->problems_scope == PROBLEMS_PROJECT ? s->ui_project_problems
                                                 : studio_code_problems(s);
}

static int problem_count(studio *s)
{
    int documents = acount(document_problems(s));
    if (s->problems_scope == PROBLEMS_PROJECT) {
        return acount(s->project_problems) + ui_problems(s) + documents;
    }
    const compile_report *report = problems(s);
    return (report ? report->error_count + report->warning_count : 0) +
           ui_problems(s) + documents;
}

/* The graph problems of the scope shown: the open asset's diagnostics or
   the project's problems. */
static int graph_problems(studio *s)
{
    if (s->problems_scope == PROBLEMS_PROJECT) {
        return acount(s->project_problems);
    }
    const compile_report *report = problems(s);
    return report ? acount(report->items) : 0;
}

/* The game UI error's asset and place, and its message after the place. */
static const char *ui_error_place(const studio *s, char *asset, size_t asset_size, char *place, size_t size)
{
    asset[0] = '\0';
    snprintf(place, size, "Game UI");
    if (!studio_ui_error_asset(s->ui_error, asset, asset_size)) {
        return s->ui_error;
    }
    snprintf(place, size, "ui/%s" UI_MARKUP_EXTENSION, asset);
    size_t length = strlen(place);
    return s->ui_error + length + (s->ui_error[length] == ' ');
}

/* "ui/Name.rcss line 4". */
static void document_place(const studio *s, const studio_ui_problem *p, char *out, size_t size)
{
    const char *root = s->scene->content_root;
    char *path = ui_buffer_part_file(p->at.path, p->at.part);
    size_t length = strlen(root);
    const char *name = !strncmp(path, root, length) && path[length] == '/'
                         ? path + length + 1
                         : path;
    snprintf(out, size, "%s line %d", name, p->at.line);
    free(path);
}

/* ---------- Each view's rows ---------- */

/* What a row shows: a line (kind, icon, place, markup, count) or a tree
   row (label, icon, meta, depth, twisty). */
typedef struct output_row
{
    const char *kind; /* error, warning, success, info, or "" for a log line */
    uint32_t icon;    /* 0 for none */
    char at[1200];    /* where, for a row that opens it; "" for none */
    char rml[OUTPUT_LINE * 2];
    char count[24];
    char label[320];
    char meta[160];
    int depth;
    bool parent, open, selected;
} output_row;

static const char *kind_name(kit_toast_type kind)
{
    switch (kind) {
        case KIT_TOAST_SUCCESS: return "success";
        case KIT_TOAST_WARNING: return "warning";
        case KIT_TOAST_ERROR: return "error";
        default: return "info";
    }
}

static uint32_t kind_icon(kit_toast_type kind)
{
    switch (kind) {
        case KIT_TOAST_SUCCESS: return KIT_ICON_PASS;
        case KIT_TOAST_WARNING: return KIT_ICON_WARNING;
        case KIT_TOAST_ERROR: return KIT_ICON_ERROR;
        default: return KIT_ICON_INFO;
    }
}

/* A muted lead, the origin's name when there is one, then the highlighted
   text, all escaped. RmlUi drops a space alone between two elements, so
   the gaps are &nbsp;. */
static void line_markup(char *out, size_t size, const char *lead, const char *name, const char *text)
{
    char body[OUTPUT_LINE], escaped[256];
    highlight_markup(HIGHLIGHT_LOG, text, body, sizeof body);
    highlight_escape(name ? name : "", escaped, sizeof escaped);
    snprintf(
        out,
        size,
        "<span class=\"tok-muted\">%s</span>&nbsp;%s%s%s%s",
        lead,
        name ? "<span class=\"tok-name\">" : "",
        escaped,
        name ? "</span>&nbsp;" : "",
        body
    );
}

/* A row at a place: its kind's icon, where, then the message. */
static void located(output_row *row, kit_toast_type kind, const char *place, const char *message)
{
    row->kind = kind_name(kind);
    row->icon = kind_icon(kind);
    snprintf(row->at, sizeof row->at, "%s", place);
    highlight_markup(HIGHLIGHT_LOG, message, row->rml, sizeof row->rml);
}

static void log_row(studio *s, int64_t key, output_row *row)
{
    editor *e = s->scene->graph_editor;
    editor_log_line line = editor_log_at(e, (int)(key - editor_log_at(e, 0).serial));
    char lead[48];
    snprintf(lead, sizeof lead, "%8.3f", line.time);
    line_markup(row->rml, sizeof row->rml, lead, line.origin, line.message);
}

static void info_row(studio *s, int64_t key, output_row *row)
{
    const studio_record_entry *entry = &s->records[key - s->records_dropped];
    char lead[24];
    int seconds = (int)entry->time;
    snprintf(lead, sizeof lead, "%02d:%02d", seconds / 60 % 100, seconds % 60);
    row->kind = kind_name(entry->kind);
    row->icon = kind_icon(entry->kind);
    line_markup(row->rml, sizeof row->rml, lead, NULL, entry->text);
}

static kit_toast_type severity_kind(int severity)
{
    return severity == CMPL_ERROR   ? KIT_TOAST_ERROR
         : severity == CMPL_WARNING ? KIT_TOAST_WARNING
                                    : KIT_TOAST_INFO;
}

static void problem_row(studio *s, int64_t key, output_row *row)
{
    int index = (int)(key % PROBLEMS_SECTION);
    switch (key / PROBLEMS_SECTION) {
        case SECTION_UI: {
            char asset[128], place[160];
            const char *message = ui_error_place(s, asset, sizeof asset, place, sizeof place);
            located(row, KIT_TOAST_ERROR, place, message);
            return;
        }
        case SECTION_DOCUMENT: {
            const studio_ui_problem *p = &document_problems(s)[index];
            char place[1200];
            document_place(s, p, place, sizeof place);
            located(row, p->error ? KIT_TOAST_ERROR : KIT_TOAST_WARNING, place, p->message);
            return;
        }
        default: break;
    }
    if (s->problems_scope == PROBLEMS_PROJECT) {
        const studio_problem *p = &s->project_problems[index];
        located(row, p->error ? KIT_TOAST_ERROR : KIT_TOAST_WARNING, p->place, p->message);
        return;
    }
    const compile_diagnostic *d = &problems(s)->items[index];
    studio_node_ref at = studio_problem_at(s, d);
    char place[256];
    studio_node_place(s, &at, place, sizeof place);
    located(row, severity_kind(d->severity), place, d->message);
}

static void runtime_row(studio *s, int64_t key, output_row *row)
{
    const studio_runtime_error *error = &s->runtime_errors[key];
    char place[256];
    snprintf(
        place,
        sizeof place,
        error->node[0] ? "%s \xe2\x80\xba %s" : "%s",
        error->behavior,
        error->node
    );
    located(row, KIT_TOAST_ERROR, place, error->message);
    if (error->count > 1) {
        snprintf(row->count, sizeof row->count, "\xc3\x97%d", error->count);
    }
}

static void reference_row(studio *s, int64_t key, output_row *row)
{
    const studio_reference *ref = &s->references[key];
    snprintf(
        row->label,
        sizeof row->label,
        ref->place[0] ? "%s \xe2\x80\xba %s" : "%s",
        ref->asset,
        ref->place
    );
    snprintf(row->meta, sizeof row->meta, "%s", ref->use);
    row->icon = studio_asset_kind_icon(ref->kind);
}

/* A call, or with a negative key the More row after row -1 - key. */
static void call_row(studio *s, int64_t key, output_row *row)
{
    if (key < 0) {
        snprintf(row->label, sizeof row->label, "More");
        row->icon = KIT_ICON_MORE;
        row->depth = s->calls.rows[-1 - key].depth + 1;
        return;
    }
    const studio_call *call = &s->calls.rows[key];
    snprintf(row->label, sizeof row->label, "%s", call->label);
    snprintf(row->meta, sizeof row->meta, "%s%s", call->meta, call->cycle ? " \xe2\x86\xbb" : "");
    row->icon = call->icon;
    row->depth = call->depth;
    row->parent = call->parent;
    row->open = call->open;
    row->selected = key == s->calls.selected;
}

static void render(studio *s, int64_t key, output_row *row)
{
    *row = (output_row){ .kind = "" };
    switch (s->output_view) {
        case OUTPUT_PROBLEMS: problem_row(s, key, row); break;
        case OUTPUT_RUNTIME: runtime_row(s, key, row); break;
        case OUTPUT_INFO: info_row(s, key, row); break;
        case OUTPUT_FIND: reference_row(s, key, row); break;
        case OUTPUT_CALLS: call_row(s, key, row); break;
        default: log_row(s, key, row); break;
    }
}

/* The log's lines under the filter, by serial. */
static int64_t *log_keys(studio *s)
{
    editor *e = s->scene->graph_editor;
    int64_t *keys = NULL;
    for (int i = 0; e && i < editor_log_count(e); i++) {
        editor_log_line line = editor_log_at(e, i);
        if (contains(line.message, s->output_filter) ||
            contains(line.origin, s->output_filter)) {
            apush(keys, line.serial);
        }
    }
    return keys;
}

static int64_t *info_keys(studio *s)
{
    int64_t *keys = NULL;
    for (int i = 0; i < acount(s->records); i++) {
        if (contains(s->records[i].text, s->output_filter)) {
            apush(keys, s->records_dropped + i);
        }
    }
    return keys;
}

/* The game UI's error, then UI documents' problems, then the graphs'. */
static int64_t *problem_keys(studio *s)
{
    int64_t *keys = NULL;
    if (ui_problems(s) && contains(s->ui_error, s->output_filter)) {
        apush(keys, SECTION_UI * PROBLEMS_SECTION);
    }
    const studio_ui_problem *documents = document_problems(s);
    for (int i = 0; i < acount(documents); i++) {
        if (contains(documents[i].message, s->output_filter) ||
            contains(documents[i].at.path, s->output_filter)) {
            apush(keys, SECTION_DOCUMENT * PROBLEMS_SECTION + i);
        }
    }
    bool project = s->problems_scope == PROBLEMS_PROJECT;
    const compile_report *report = problems(s);
    for (int i = 0; i < graph_problems(s); i++) {
        const char *message = project ? s->project_problems[i].message
                                      : report->items[i].message;
        if (contains(message, s->output_filter) ||
            (project && contains(s->project_problems[i].place, s->output_filter))) {
            apush(keys, SECTION_GRAPH * PROBLEMS_SECTION + i);
        }
    }
    return keys;
}

static int64_t *runtime_keys(studio *s)
{
    int64_t *keys = NULL;
    for (int i = 0; i < acount(s->runtime_errors); i++) {
        const studio_runtime_error *error = &s->runtime_errors[i];
        if (contains(error->message, s->output_filter) ||
            contains(error->behavior, s->output_filter) ||
            contains(error->node, s->output_filter)) {
            apush(keys, i);
        }
    }
    return keys;
}

static int64_t *widened(int *rows)
{
    int64_t *keys = NULL;
    for (int i = 0; i < acount(rows); i++) {
        apush(keys, rows[i]);
    }
    afree(rows);
    return keys;
}

static int64_t *view_keys(studio *s)
{
    switch (s->output_view) {
        case OUTPUT_PROBLEMS: return problem_keys(s);
        case OUTPUT_RUNTIME: return runtime_keys(s);
        case OUTPUT_INFO: return info_keys(s);
        case OUTPUT_FIND: return widened(studio_find_rows(s));
        case OUTPUT_CALLS: return widened(studio_calls_rows(s));
        default: return log_keys(s);
    }
}

/* What an empty view says. */
static const char *empty_text(studio *s)
{
    switch (s->output_view) {
        case OUTPUT_PROBLEMS:
            return graph_problems(s) || ui_problems(s) ||
                           acount(document_problems(s))
                     ? "No matches"
                     : "No problems";
        case OUTPUT_RUNTIME:
            return acount(s->runtime_errors) ? "No matches" : "No runtime errors";
        case OUTPUT_INFO:
            return acount(s->records) ? "No matches" : "No operations yet";
        case OUTPUT_FIND:
            return acount(s->references) ? "No matches" : "No references";
        case OUTPUT_CALLS:
            return acount(s->calls.rows) ? "No matches" : "No calls";
        default:
            return s->scene->graph_editor && editor_log_count(s->scene->graph_editor)
                     ? "No matches"
                     : "No output";
    }
}

/* Lines are written once: a key always shows the same line. */
static bool immutable(int view)
{
    return view == OUTPUT_LOG || view == OUTPUT_INFO;
}

/* The array a view's rows go in, each with the fields its markup binds: the
   Log's lines only their markup, so the rows that follow a busy log stay
   lean; other lines their kind's icon, place and count too; and tree rows
   TreeRow's. */
typedef enum
{
    SHAPE_LINE,
    SHAPE_ENTRY,
    SHAPE_ITEM,
    SHAPE_COUNT,
} shape;

static const char *const SHAPE_ARRAYS[SHAPE_COUNT] = { "lines", "entries", "items" };

static shape shape_of(int view)
{
    switch (view) {
        case OUTPUT_LOG: return SHAPE_LINE;
        case OUTPUT_FIND:
        case OUTPUT_CALLS: return SHAPE_ITEM;
        default: return SHAPE_ENTRY;
    }
}

/* ---------- The `output` variable ---------- */

static const char *const ENTRY_TEXTS[] = { "kind", "icon", "at", "count" };
static const char *const ITEM_TEXTS[] = { "label", "icon", "meta" };
/* TreeRow's flags, and `shown`, the open C wrote last: an open that differs
   from it is the twisty's. */
static const char *const ITEM_FLAGS[] = { "parent", "open",   "selected",
                                          "active", "marked", "hidden",
                                          "shown" };

static void add_texts(yyjson_mut_doc *doc, yyjson_mut_val *row, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_obj_add_strcpy(doc, row, names[i], "");
    }
}

static yyjson_mut_val *row_new(yyjson_mut_doc *doc, shape kind)
{
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, row, "offset", "");
    yyjson_mut_obj_add_real(doc, row, "key", 0);
    if (kind != SHAPE_ITEM) {
        yyjson_mut_obj_add_strcpy(doc, row, "rml", "");
    }
    if (kind == SHAPE_ENTRY) {
        add_texts(doc, row, ENTRY_TEXTS, sizeof ENTRY_TEXTS / sizeof *ENTRY_TEXTS);
    }
    if (kind != SHAPE_ITEM) {
        return row;
    }
    add_texts(doc, row, ITEM_TEXTS, sizeof ITEM_TEXTS / sizeof *ITEM_TEXTS);
    for (size_t i = 0; i < sizeof ITEM_FLAGS / sizeof *ITEM_FLAGS; i++) {
        yyjson_mut_obj_add_bool(doc, row, ITEM_FLAGS[i], false);
    }
    yyjson_mut_obj_add_real(doc, row, "depth", 0);
    return row;
}

void studio_output_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *output = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "output");
    yyjson_mut_obj_add_real(doc, output, "view", 0);
    yyjson_mut_obj_add_real(doc, output, "problems", 0);
    yyjson_mut_obj_add_real(doc, output, "runtime", 0);
    yyjson_mut_obj_add_real(doc, output, "scope", 0);
    yyjson_mut_obj_add_bool(doc, output, "clearable", false);
    yyjson_mut_obj_add_strcpy(doc, output, "filter", "");
    yyjson_mut_val *head = yyjson_mut_obj_add_obj(doc, output, "head");
    yyjson_mut_obj_add_strcpy(doc, head, "icon", "");
    yyjson_mut_obj_add_strcpy(doc, head, "title", "");
    yyjson_mut_obj_add_strcpy(doc, head, "count", "");
    yyjson_mut_obj_add_bool(doc, output, "callees", false);
}

void studio_output_rows_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *rows = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "output_rows");
    yyjson_mut_obj_add_strcpy(doc, rows, "empty", "");
    for (int i = 0; i < SHAPE_COUNT; i++) {
        yyjson_mut_obj_add_arr(doc, rows, SHAPE_ARRAYS[i]);
    }
}

static bool put_glyph(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *name, uint32_t icon)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    return ui_data_put_text(doc, object, name, glyph);
}

/* Writes what a row shows into its item. */
static bool show_row(yyjson_mut_doc *doc, yyjson_mut_val *item, const output_row *row, shape kind)
{
    if (kind == SHAPE_LINE) {
        return ui_data_put_text(doc, item, "rml", row->rml);
    }
    bool changed = put_glyph(doc, item, "icon", row->icon);
    if (kind == SHAPE_ITEM) {
        changed |= ui_data_put_text(doc, item, "label", row->label);
        changed |= ui_data_put_text(doc, item, "meta", row->meta);
        changed |= ui_data_put_number(item, "depth", row->depth);
        changed |= ui_data_put_bool(item, "parent", row->parent);
        changed |= ui_data_put_bool(item, "open", row->open);
        changed |= ui_data_put_bool(item, "shown", row->open);
        return ui_data_put_bool(item, "selected", row->selected) || changed;
    }
    changed |= ui_data_put_text(doc, item, "kind", row->kind);
    changed |= ui_data_put_text(doc, item, "at", row->at);
    changed |= ui_data_put_text(doc, item, "rml", row->rml);
    return ui_data_put_text(doc, item, "count", row->count) || changed;
}

/* Gives the array `length` rows, adding and removing at its end; true when
   its length changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *rows, int length, shape kind)
{
    int had = (int)yyjson_mut_arr_size(rows);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(rows, row_new(doc, kind));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(rows);
    }
    return had != length;
}

/* The slot already showing `key`, unclaimed, or -1. */
static int slot_of(const int64_t *slots, const bool *claimed, int length, int64_t key)
{
    for (int j = 0; j < length; j++) {
        if (!claimed[j] && slots[j] == key) {
            return j;
        }
    }
    return -1;
}

/* Shows the window's keys, the rows from `first`, in the array's items:
   a key already shown keeps its item and moves to its place, and the keys
   new to the window take the items the others left, so only their contents
   change. */
static bool show_window(
    studio *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    const int64_t *window,
    int length,
    int first,
    float height
)
{
    int view = s->output_view;
    shape kind = shape_of(view);
    bool changed = fit(doc, rows, length, kind);
    int had = acount(s->output.slots);
    asetlen(s->output.slots, length);
    for (int j = had; j < length; j++) {
        s->output.slots[j] = NO_KEY;
    }
    bool claimed[length > 0 ? length : 1];
    int at[length > 0 ? length : 1]; /* each position's slot */
    memset(claimed, 0, sizeof claimed);
    for (int p = 0; p < length; p++) {
        at[p] = slot_of(s->output.slots, claimed, length, window[p]);
        if (at[p] >= 0) {
            claimed[at[p]] = true;
        }
    }
    int free_slot = 0;
    for (int p = 0; p < length; p++) {
        bool fresh = at[p] < 0;
        while (fresh && claimed[free_slot]) {
            free_slot++;
        }
        int j = fresh ? free_slot : at[p];
        claimed[j] = true;
        s->output.slots[j] = window[p];
        yyjson_mut_val *item = yyjson_mut_arr_get(rows, (size_t)j);
        char offset[32];
        snprintf(offset, sizeof offset, "translateY(%.2fdp)", (first + p) * height);
        changed |= ui_data_put_text(doc, item, "offset", offset);
        changed |= ui_data_put_number(item, "key", (double)window[p]);
        if (fresh || !immutable(view)) {
            output_row row;
            render(s, window[p], &row);
            changed |= show_row(doc, item, &row, kind);
        }
    }
    return changed;
}

/* The List of Studio's documents, laid out, or NULL. */
static rml_element *output_list(studio *s, const studio_panel_sync *at)
{
    if (at->preview) {
        return NULL;
    }
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), OUTPUT_LIST);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static float row_height(rml_element *list)
{
    char value[32];
    rml_element_property(list, "--row", value, sizeof value);
    float row = strtof(value, NULL);
    return row > 0 ? row : 20;
}

/* The position of `key` among the keys, or -1. */
static int position_of(const int64_t *keys, int64_t key)
{
    for (int i = 0; i < acount(keys); i++) {
        if (keys[i] == key) {
            return i;
        }
    }
    return -1;
}

/* The window of the view's rows in its shape's array, the other arrays
   empty. A view starts at its top, or a following one at its end. */
static bool show_rows(studio *s, const studio_panel_sync *at)
{
    int view = s->output_view;
    rml_element *list = output_list(s, at);
    if (s->output.shown != view) {
        s->output.shown = view;
        s->output.list = (ui_list_state)UI_LIST_STATE_INIT;
        asetlen(s->output.slots, 0);
        if (list) {
            rml_element_set_scroll_top(list, 0);
        }
    }
    int64_t *keys = view_keys(s);
    int count = acount(keys);
    int first = 0, length = count < UNLAID_ROWS ? count : UNLAID_ROWS;
    float height = 20;
    if (list) {
        bool follow = view == OUTPUT_LOG || view == OUTPUT_INFO;
        height = row_height(list);
        int reveal = s->calls.reveal && view == OUTPUT_CALLS
                       ? position_of(keys, s->calls.selected)
                       : -1;
        if (reveal >= 0) {
            s->output.list.revealed = -1;
        }
        s->calls.reveal = false;
        ui_list window = { .count = count,
                           .row = height,
                           .reveal = reveal,
                           .overscan = OVERSCAN,
                           .follow = follow };
        length = ui_list_window(list, &window, &s->output.list, &first);
    }
    shape kind = shape_of(view);
    bool changed = false;
    for (int i = 0; i < SHAPE_COUNT; i++) {
        yyjson_mut_val *rows = yyjson_mut_obj_get(at->variable, SHAPE_ARRAYS[i]);
        changed |= i == (int)kind
                     ? show_window(s, at->doc, rows, keys ? keys + first : NULL, length, first, height)
                     : fit(at->doc, rows, 0, (shape)i);
    }
    changed |= ui_data_put_text(at->doc, at->variable, "empty", count ? "" : empty_text(s));
    afree(keys);
    return changed;
}

/* Find's head names what the references are to, Calls' what it traces. */
static bool show_head(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *head = yyjson_mut_obj_get(at->variable, "head");
    const char *title = "";
    uint32_t icon = 0;
    char count[16] = "";
    if (s->output_view == OUTPUT_FIND && s->references_title[0]) {
        title = s->references_title;
        icon = KIT_ICON_REFERENCES;
        snprintf(count, sizeof count, "%d", acount(s->references));
    }
    if (s->output_view == OUTPUT_CALLS && s->calls.root.title[0]) {
        title = s->calls.root.title;
        icon = s->calls.callees ? KIT_ICON_CALLEES : KIT_ICON_CALLERS;
    }
    bool changed = ui_data_put_text(at->doc, head, "title", title);
    changed |= put_glyph(at->doc, head, "icon", icon);
    changed |= ui_data_put_text(at->doc, head, "count", count);
    return ui_data_put_bool(at->variable, "callees", s->calls.callees) || changed;
}

static bool clearable(studio *s)
{
    switch (s->output_view) {
        case OUTPUT_LOG:
            return s->scene->graph_editor &&
                   editor_log_count(s->scene->graph_editor) > 0;
        case OUTPUT_RUNTIME: return acount(s->runtime_errors) > 0;
        case OUTPUT_INFO: return acount(s->records) > 0;
        case OUTPUT_FIND: return s->references_title[0] != '\0';
        case OUTPUT_CALLS: return s->calls.root.title[0] != '\0';
        default: return false;
    }
}

/* The filter is the field's: what it wrote is applied first. */
static bool sync_filter(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *filter = yyjson_mut_obj_get(at->variable, "filter");
    const char *typed = yyjson_mut_get_str(filter);
    if (!strcmp(typed, s->output_filter)) {
        return false;
    }
    if (at->wrote && !at->preview) {
        snprintf(s->output_filter, sizeof s->output_filter, "%s", typed);
        return false;
    }
    ui_data_set_text(at->doc, filter, s->output_filter);
    return true;
}

/* A Calls row's twisty writes its item's open; the row follows it. An open
   still as C showed it leaves the row as the keys may have folded it since. */
static void apply_twisties(studio *s, const studio_panel_sync *at)
{
    if (!at->wrote || at->preview || s->output.shown != OUTPUT_CALLS) {
        return;
    }
    yyjson_mut_val *items = yyjson_mut_obj_get(at->variable, "items");
    for (int j = 0; j < acount(s->output.slots) && j < (int)yyjson_mut_arr_size(items);
         j++) {
        int64_t key = s->output.slots[j];
        yyjson_mut_val *item = yyjson_mut_arr_get(items, (size_t)j);
        bool open = yyjson_mut_get_bool(yyjson_mut_obj_get(item, "open"));
        if (key >= 0 && key < acount(s->calls.rows) &&
            open != yyjson_mut_get_bool(yyjson_mut_obj_get(item, "shown"))) {
            s->calls.rows[key].open = open;
        }
    }
}

/* `output` changes as the view, its counts or its head do; its rows, which
   change with each new line, are `output_rows`, synced after it, so an
   appended line evaluates the rows' views alone. */
bool studio_output_sync(studio *s, const studio_panel_sync *at)
{
    bool changed = sync_filter(s, at);
    if (s->problems_scope == PROBLEMS_PROJECT && !s->problems_checked) {
        check_project(s);
    }
    changed |= ui_data_put_number(at->variable, "view", s->output_view);
    changed |= ui_data_put_number(at->variable, "problems", problem_count(s));
    changed |= ui_data_put_number(at->variable, "runtime", acount(s->runtime_errors));
    changed |= ui_data_put_number(at->variable, "scope", s->problems_scope);
    changed |= ui_data_put_bool(at->variable, "clearable", clearable(s));
    return show_head(s, at) || changed;
}

bool studio_output_rows_sync(studio *s, const studio_panel_sync *at)
{
    apply_twisties(s, at);
    return show_rows(s, at);
}

/* ---------- Actions ---------- */

static void clear(studio *s)
{
    switch (s->output_view) {
        case OUTPUT_LOG:
            if (s->scene->graph_editor) {
                editor_clear_log(s->scene->graph_editor);
            }
            break;
        case OUTPUT_RUNTIME: asetlen(s->runtime_errors, 0); break;
        case OUTPUT_INFO:
            s->records_dropped += acount(s->records);
            asetlen(s->records, 0);
            break;
        case OUTPUT_FIND: studio_find_clear(s); break;
        case OUTPUT_CALLS: studio_calls_clear(s); break;
    }
}

/* The game UI error's row opens its asset's source, or else its preview. */
static void open_ui_error(studio *s)
{
    char asset[128], place[160];
    ui_error_place(s, asset, sizeof asset, place, sizeof place);
    if (!asset[0]) {
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", s->scene->content_root, place);
    if (!studio_open_ui_source(s, path, 1)) {
        studio_preview_ui(s, asset);
    }
}

/* A problem's row opens its place: a UI document's in the Text Editor, a
   node's in its asset, revealed. */
static void open_problem(studio *s, int64_t key)
{
    int index = (int)(key % PROBLEMS_SECTION);
    switch (key / PROBLEMS_SECTION) {
        case SECTION_UI: open_ui_error(s); return;
        case SECTION_DOCUMENT:
            if (index < acount(document_problems(s))) {
                studio_code_open_place(s, &document_problems(s)[index].at);
            }
            return;
        default: break;
    }
    if (index >= graph_problems(s)) {
        return;
    }
    if (s->problems_scope == PROBLEMS_PROJECT) {
        studio_reveal(s, &s->project_problems[index].at);
        return;
    }
    if (s->scene->graph_document_id) {
        studio_node_ref at = studio_problem_at(s, &problems(s)->items[index]);
        studio_reveal(s, &at);
    }
}

/* A click on a row: problems, errors and references open; a Calls row is
   selected, and its More traces on. Rows may have changed since they were
   shown, so keys past the rows open nothing. */
static void click(studio *s, int64_t key)
{
    switch (s->output_view) {
        case OUTPUT_PROBLEMS: open_problem(s, key); break;
        case OUTPUT_RUNTIME:
            if (key < acount(s->runtime_errors)) {
                const studio_runtime_error *error = &s->runtime_errors[key];
                if (error->path[0]) {
                    ui_code_place place = { .offset = error->start,
                                            .line = error->line };
                    snprintf(place.path, sizeof place.path, "%s", error->path);
                    studio_code_open_place(s, &place);
                } else {
                    studio_reveal(s, &error->at);
                }
            }
            break;
        case OUTPUT_FIND:
            if (key < acount(s->references)) {
                studio_open_reference(s, &s->references[key]);
            }
            break;
        case OUTPUT_CALLS: studio_calls_click(s, (int)key); break;
        default: break;
    }
}

void studio_output_action(studio *s, const char *action, const ui_activation *a)
{
    int64_t key = a->argc ? strtoll(a->args[0], NULL, 10) : 0;
    if (!strcmp(action, "view") && key >= 0 && key < OUTPUT_VIEWS) {
        s->output_view = (int)key;
    } else if (!strcmp(action, "scope")) {
        s->problems_scope = key ? PROBLEMS_PROJECT : PROBLEMS_ASSET;
    } else if (!strcmp(action, "refresh")) {
        s->problems_checked = false;
    } else if (!strcmp(action, "clear")) {
        clear(s);
    } else if (!strcmp(action, "click")) {
        click(s, key);
    } else if (!strcmp(action, "open") && s->output_view == OUTPUT_CALLS) {
        studio_calls_open(s, (int)key);
    } else if (!strcmp(action, "direction")) {
        studio_calls_direction(s, key != 0);
    }
    /* A scroll only wakes the source, which reads where the List is. */
}

bool studio_output_key(studio *s, SDL_Keycode key, SDL_Keymod mod)
{
    return s->output_view == OUTPUT_CALLS && !studio_primary(s, mod) &&
           studio_calls_key(s, key);
}

/* ---------- The area ---------- */

/* The header's controls are OutputHeader's; Find focuses its filter. */
void studio_output_header(studio *s, int area)
{
    rml_element *host = studio_ui_area_controls(s, area, "output", "OutputHeader");
    rml_element *filter = host ? rml_element_by_id(host, "output-filter") : NULL;
    if (filter && s->find_request == STUDIO_EDITOR_OUTPUT + 1) {
        rml_element_focus(filter, true);
        s->find_request = 0;
    }
}

void studio_output_body(studio *s, int area)
{
    studio_ui_area(s, area, "Output");
}

/* A maximized area gives way so the editor shows. */
/* The area of the active workspace that holds `editor`, whether or not it
   shows, or -1. */
static int area_of(const studio_layout *layout, studio_editor editor)
{
    for (int area = 0; area < STUDIO_AREA_COUNT; area++) {
        if (layout->editors[area] == editor) {
            return area;
        }
    }
    return -1;
}

static bool side_area(int area)
{
    return area == STUDIO_AREA_OUTLINER || area == STUDIO_AREA_PROPERTIES;
}

bool studio_editor_shown(const studio *s, studio_editor editor)
{
    const studio_layout *layout = &s->prefs.layouts[s->prefs.workspace];
    for (int area = 0; area < STUDIO_AREA_COUNT; area++) {
        bool shown = area == STUDIO_AREA_DRAWER
                       ? layout->drawer_open
                       : !side_area(area) || layout->side_open;
        if (shown && layout->editors[area] == editor) {
            return true;
        }
    }
    return false;
}

/* A hidden side pane opens for its editors; any other editor not in view
   takes the drawer. */
void studio_show_editor(studio *s, studio_editor editor)
{
    studio_layout *layout = studio_active_layout(s);
    if (s->maximized >= 0 && layout->editors[s->maximized] != editor) {
        s->maximized = -1;
    }
    if (studio_editor_shown(s, editor)) {
        return;
    }
    if (side_area(area_of(layout, editor))) {
        layout->side_open = true;
        return;
    }
    if (editor == STUDIO_EDITOR_SCENE || editor == STUDIO_EDITOR_BEHAVIOR) {
        layout->editors[STUDIO_AREA_TOP] = editor;
        return;
    }
    studio_toggle_drawer(s, editor);
}

void studio_output_find(studio *s)
{
    s->output_view = OUTPUT_FIND;
    studio_show_editor(s, STUDIO_EDITOR_OUTPUT);
}

void studio_output_calls(studio *s)
{
    s->output_view = OUTPUT_CALLS;
    studio_show_editor(s, STUDIO_EDITOR_OUTPUT);
}
