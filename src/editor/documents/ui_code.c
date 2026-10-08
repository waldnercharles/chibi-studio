/* Code intelligence for the Text Editor (ui_code.h): the language service and
 * RmlUi's check, turned from files into the part the editor shows. */
#include "editor/documents/ui_code.h"

#include "assets/ui_document.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "ui/rml.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ui_source_language ui_code_language(ui_part part)
{
    return part == UI_PART_RML ? UI_SOURCE_RML : UI_SOURCE_RCSS;
}

static bool ends_with(const char *text, const char *suffix)
{
    size_t length = strlen(text), size = strlen(suffix);
    return length >= size && !strcmp(text + length - size, suffix);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool ui_code_content_root(const char *path, char *root, size_t size)
{
    const char *ui = NULL;
    for (const char *p = strstr(path, "/ui/"); p; p = strstr(p + 1, "/ui/")) {
        ui = p;
    }
    if (!ui) {
        return false;
    }
    snprintf(root, size, "%.*s", (int)(ui - path), path);
    return true;
}

void ui_code_scan(ui_source_project *project, const char *content_root)
{
    ui_source_project_scan(project, content_root);
    char **names = NULL;
    ui_check_property_names(&names);
    for (int i = 0; i < acount(names); i++) {
        ui_source_list_add(&project->properties, names[i]);
        free(names[i]);
    }
    afree(names);
}

/* A stylesheet's own lines, its imports resolved beside it. */
static bool check_stylesheet(
    const ui_buffer *b,
    rml_context *context,
    ui_check_message **messages,
    char *error,
    size_t size
)
{
    char folder[PATH_MAX];
    snprintf(folder, sizeof folder, "%.*s", (int)(base_name(b->path) - b->path), b->path);
    const char *text = ui_buffer_text(b, UI_PART_RCSS);
    return ui_check_stylesheet(context, folder, text, messages, error, size);
}

bool ui_code_check(const ui_buffer *b, rml_context *context, ui_check_message **messages, char *error, size_t size)
{
    if (b->stylesheet) {
        return check_stylesheet(b, context, messages, error, size);
    }
    /* Outside a ui folder, widgets and imports have nowhere to resolve. */
    char root[PATH_MAX], name[128];
    if (!ui_code_content_root(b->path, root, sizeof root)) {
        return true;
    }
    snprintf(name, sizeof name, "%s", base_name(b->path));
    name[strlen(name) - strlen(UI_MARKUP_EXTENSION)] = '\0';
    return ui_check_in(
        context,
        root,
        name,
        ui_buffer_text(b, UI_PART_RML),
        ui_buffer_text(b, UI_PART_RCSS),
        messages,
        error,
        size
    );
}

/* ---------- The editor over a buffer ---------- */

static bool continuation(char c)
{
    return ((unsigned char)c & 0xc0) == 0x80;
}

/* What lies between the common start and end of two texts, on whole UTF-8
   characters. */
static ui_code_change difference(const char *from, int from_length, const char *to, int to_length)
{
    int start = 0, end = 0;
    while (start < from_length && start < to_length && from[start] == to[start]) {
        start++;
    }
    while (start > 0 && (continuation(from[start]) || continuation(to[start]))) {
        start--;
    }
    while (end < from_length - start && end < to_length - start &&
           from[from_length - 1 - end] == to[to_length - 1 - end]) {
        end++;
    }
    while (end > 0 && (continuation(from[from_length - end]) ||
                       continuation(to[to_length - end]))) {
        end--;
    }
    return (ui_code_change){ start, from_length - end, to_length - start - end };
}

/* The edit a change makes, its text owned. */
static text_edit edit_of(ui_code_change d, const char *to)
{
    char *insert = xmalloc((size_t)d.inserted + 1);
    memcpy(insert, to + d.start, (size_t)d.inserted);
    insert[d.inserted] = '\0';
    return (text_edit){ d.start, d.end, insert };
}

static bool unchanged(ui_code_change d)
{
    return d.start == d.end && !d.inserted;
}

static int map_offset(int offset, const text_edit *edits, int count)
{
    long shift = 0, inside = -1;
    for (int i = 0; i < count; i++) {
        const text_edit *edit = &edits[i];
        long added = (long)strlen(edit->text);
        if (edit->end <= offset && edit->start < offset) {
            shift += added - (edit->end - edit->start);
        } else if (edit->start < offset) {
            inside = edit->start + added;
        }
    }
    return (int)((inside >= 0 ? inside : offset) + shift);
}

bool ui_code_text_diff(const char *from, const char *to, text_edit *edit)
{
    ui_code_change change = difference(from, (int)strlen(from), to, (int)strlen(to));
    if (unchanged(change)) {
        *edit = (text_edit){ 0, 0, NULL };
        return false;
    }
    *edit = edit_of(change, to);
    return true;
}

/* What typing changed from `base`, as an edit of `now`, the part as it
   stands: where another change of `base` made meanwhile left the typed
   bytes alone, the typing moves with it; where both changed the same bytes,
   the editor's text stands. */
static text_edit rebased(const char *base, ui_code_change typed, const char *text, int length, const char *now)
{
    int base_length = (int)strlen(base), now_length = (int)strlen(now);
    ui_code_change other = difference(base, base_length, now, now_length);
    int shift = other.inserted - (other.end - other.start);
    if (typed.end <= other.start) {
        return edit_of(typed, text);
    }
    if (typed.start >= other.end) {
        text_edit edit = edit_of(typed, text);
        edit.start += shift;
        edit.end += shift;
        return edit;
    }
    return edit_of(difference(now, now_length, text, length), text);
}

bool ui_code_prepare(
    const ui_buffer *b,
    ui_part part,
    kit_code_editor *e,
    const char *base,
    ui_code_change *typed,
    text_edit *edit
)
{
    size_t length;
    const char *text = kit_code_text(e, &length);
    *typed = difference(base, (int)strlen(base), text, (int)length);
    *edit = (text_edit){ 0, 0, NULL };
    if (unchanged(*typed)) {
        return false;
    }
    const char *old = ui_buffer_text(b, part);
    *edit = rebased(base, *typed, text, (int)length, old);
    return true;
}

ui_code_change ui_code_record(
    ui_buffer *b,
    ui_part part,
    kit_code_editor *e,
    const char *base,
    double now,
    bool step
)
{
    ui_code_change typed;
    text_edit edit;
    if (!ui_code_prepare(b, part, e, base, &typed, &edit)) {
        return typed;
    }
    if (step) {
        ui_buffer_edit(b, part, &edit, 1);
    } else {
        char *composed = text_edits_apply(ui_buffer_text(b, part), &edit, 1);
        ui_buffer_type(b, part, composed, now);
        free(composed);
    }
    free((char *)edit.text);
    return typed;
}

bool ui_code_follow(kit_code_editor *e, const char *text, bool land)
{
    size_t length;
    const char *shown = kit_code_text(e, &length);
    ui_code_change d = difference(shown, (int)length, text, (int)strlen(text));
    if (unchanged(d)) {
        return false;
    }

    /* The formatter changes trivia only. Splitting its edit at every
       unchanged token keeps the caret and selection anchored in attribute
       values, binding expressions and prose while whitespace is reflowed. */
    int target_length = (int)strlen(text), from = 0, to = 0;
    text_edit *edits = NULL;
    bool whitespace_only = true;
    for (;;) {
        int from_space = from, to_space = to;
        while (from < (int)length && isspace((unsigned char)shown[from])) {
            from++;
        }
        while (to < target_length && isspace((unsigned char)text[to])) {
            to++;
        }
        if (from - from_space != to - to_space ||
            memcmp(shown + from_space, text + to_space, (size_t)(from - from_space))) {
            int inserted = to - to_space;
            char *replacement = xmalloc((size_t)inserted + 1);
            memcpy(replacement, text + to_space, (size_t)inserted);
            replacement[inserted] = '\0';
            apush(edits, ((text_edit){ from_space, from, replacement }));
        }
        while (from < (int)length && to < target_length &&
               !isspace((unsigned char)shown[from]) &&
               !isspace((unsigned char)text[to])) {
            if (shown[from] != text[to]) {
                whitespace_only = false;
                break;
            }
            from++;
            to++;
        }
        if (!whitespace_only) {
            break;
        }
        if (from == (int)length && to == target_length) {
            break;
        }
        if ((from == (int)length && to < target_length &&
             !isspace((unsigned char)text[to])) ||
            (to == target_length && from < (int)length &&
             !isspace((unsigned char)shown[from]))) {
            whitespace_only = false;
            break;
        }
    }

    if (!whitespace_only) {
        for (int i = 0; i < acount(edits); i++) {
            free((char *)edits[i].text);
        }
        afree(edits);
        text_edit edit = edit_of(d, text);
        kit_code_apply(e, &edit, 1);
        free((char *)edit.text);
        if (land) {
            kit_code_set_cursor(e, d.start + d.inserted);
        }
        return true;
    }

    int cursor = kit_code_cursor(e), selection_start = 0, selection_end = 0;
    bool selected = kit_code_selection(e, &selection_start, &selection_end);
    int anchor = selected && cursor <= selection_start ? selection_end : selection_start;
    kit_code_apply(e, edits, acount(edits));
    if (selected) {
        int mapped_anchor = map_offset(anchor, edits, acount(edits));
        int mapped_cursor = map_offset(cursor, edits, acount(edits));
        if (!land) {
            kit_code_set_selection(e, mapped_anchor, mapped_cursor);
        }
    }
    for (int i = 0; i < acount(edits); i++) {
        free((char *)edits[i].text);
    }
    afree(edits);
    if (land) {
        kit_code_set_cursor(e, d.start + d.inserted);
    }
    return true;
}

bool ui_code_replay(ui_buffer *b, bool redo)
{
    if (redo ? !ui_buffer_can_redo(b) : !ui_buffer_can_undo(b)) {
        return false;
    }
    if (redo) {
        ui_buffer_redo(b);
    } else {
        ui_buffer_undo(b);
    }
    return true;
}

/* ---------- Diagnostics ---------- */

/* The bytes of 1-based line `line`, its '\n' left out. */
static void line_bytes(const char *text, int line, int *start, int *end)
{
    int at = 0;
    for (int n = 1; n < line && text[at]; n++) {
        at += (int)strcspn(text + at, "\n");
        at += text[at] == '\n';
    }
    *start = at;
    *end = at + (int)strcspn(text + at, "\n");
}

/* Where `needle` first lies in bytes [from, to), as the range the marker
   underlines, on the line holding it; false when they do not hold it. */
static bool found_in(const char *text, int from, int to, const char *needle, kit_code_marker *m)
{
    int length = (int)strlen(needle);
    for (int at = from; length && at + length <= to; at++) {
        if (!strncmp(text + at, needle, (size_t)length)) {
            m->start = at;
            m->end = at + length;
            m->line = 1;
            for (int i = 0; i < at; i++) {
                m->line += text[i] == '\n';
            }
            return true;
        }
    }
    return false;
}

static bool on_line(const char *text, const char *needle, kit_code_marker *m)
{
    int start, end;
    line_bytes(text, m->line, &start, &end);
    return found_in(text, start, end, needle, m);
}

/* The marker's range: the first text the message quotes that its line
   holds, as RmlUi quotes a declaration or a data expression; or, since
   RmlUi places a rule's warnings at its selector's line, the rule's lines
   that follow, up to its '}'. */
static void quoted_range(const char *text, const char *message, kit_code_marker *m)
{
    int start, end;
    line_bytes(text, m->line, &start, &end);
    int rule_end = start + (int)strcspn(text + start, "}");
    for (const char *open = strpbrk(message, "'\""); open;
         open = strpbrk(open + 1, "'\"")) {
        const char *close = strchr(open + 1, *open);
        if (!close) {
            return;
        }
        char quoted[256];
        snprintf(quoted, sizeof quoted, "%.*s", (int)(close - open - 1), open + 1);
        if (found_in(text, start, end, quoted, m) ||
            found_in(text, start, rule_end, quoted, m)) {
            return;
        }
        open = close;
    }
}

/* The 1-based line of the first place `text` holds `needle`; 1 without. */
static int line_naming(const char *text, const char *needle)
{
    const char *found = needle[0] ? strstr(text, needle) : NULL;
    int line = 1;
    for (const char *p = text; found && p < found; p++) {
        line += *p == '\n';
    }
    return line;
}

/* The first quoted text of a message, as an error names a widget. */
static void first_quoted(const char *message, char *out, size_t size)
{
    out[0] = '\0';
    const char *open = strpbrk(message, "'\"");
    const char *close = open ? strchr(open + 1, *open) : NULL;
    if (close) {
        snprintf(out, size, "%.*s", (int)(close - open - 1), open + 1);
    }
}

/* A message's first line: RmlUi follows a data expression's warning with
   the expression and a caret. */
static char *first_line(const char *prefix, const char *text)
{
    size_t size = strcspn(text, "\n") + strlen(prefix) + 1;
    char *line = xmalloc(size);
    snprintf(line, size, "%s%.*s", prefix, (int)strcspn(text, "\n"), text);
    return line;
}

/* A marker of the whole line, until a range is found. */
static kit_code_marker line_marker(int line, kit_code_severity severity, char *message)
{
    return (kit_code_marker){ .line = line > 0 ? line : 1,
                              .severity = severity,
                              .message = message };
}

/* A widget's or an import's message marks where the part names it; another
   the text it quotes on its line. */
static void add_message(kit_code_marker **markers, const char *text, const ui_check_message *m)
{
    if (!m->source) {
        kit_code_marker marker = line_marker(m->line, KIT_CODE_WARNING, first_line("", m->text));
        quoted_range(text, m->text, &marker);
        apush(*markers, marker);
        return;
    }
    char needle[256], prefix[160];
    if (m->part == UI_CHECK_WIDGETS) {
        snprintf(needle, sizeof needle, "ui=\"%s\"", m->source);
    } else {
        snprintf(needle, sizeof needle, "%s", base_name(m->source));
    }
    snprintf(prefix, sizeof prefix, "%s: ", base_name(m->source));
    kit_code_marker marker = line_marker(
        line_naming(text, needle),
        KIT_CODE_WARNING,
        first_line(prefix, m->text)
    );
    on_line(text, needle, &marker);
    apush(*markers, marker);
}

static int by_line(const void *a, const void *b)
{
    const kit_code_marker *x = a, *y = b;
    return x->line != y->line
             ? (x->line > y->line) - (x->line < y->line)
             : (x->severity > y->severity) - (x->severity < y->severity);
}

void ui_code_markers(
    const ui_source *source,
    ui_part part,
    const ui_check_message *messages,
    const char *failure,
    kit_code_marker **markers
)
{
    for (int i = 0; i < acount(source->diagnostics); i++) {
        const ui_source_diagnostic *d = &source->diagnostics[i];
        kit_code_marker marker = line_marker(d->line, KIT_CODE_ERROR, xstrdup(d->message));
        marker.start = d->start;
        marker.end = d->end;
        apush(*markers, marker);
    }
    for (int i = 0; i < acount(messages); i++) {
        const ui_check_message *m = &messages[i];
        ui_part about = m->part == UI_CHECK_RCSS ? UI_PART_RCSS : UI_PART_RML;
        if (about == part) {
            add_message(markers, source->text, m);
        }
    }
    if (failure && failure[0] && part == UI_PART_RML) {
        char name[128];
        first_quoted(failure, name, sizeof name);
        kit_code_marker marker = line_marker(
            line_naming(source->text, name),
            KIT_CODE_ERROR,
            xstrdup(failure)
        );
        on_line(source->text, name, &marker);
        apush(*markers, marker);
    }
    if (acount(*markers) > 1) {
        qsort(*markers, (size_t)acount(*markers), sizeof **markers, by_line);
    }
}
void ui_code_markers_free(kit_code_marker *markers)
{
    for (int i = 0; i < acount(markers); i++) {
        free((char *)markers[i].message);
    }
    afree(markers);
}

/* ---------- Completion ---------- */

static const char *kind_detail(ui_source_completion_kind kind)
{
    switch (kind) {
        case UI_COMPLETE_TAG: return "element";
        case UI_COMPLETE_ATTRIBUTE: return "attribute";
        case UI_COMPLETE_WIDGET: return "widget";
        case UI_COMPLETE_CLASS: return "class";
        case UI_COMPLETE_VARIABLE: return "custom property";
        case UI_COMPLETE_PROPERTY: return "property";
        case UI_COMPLETE_SAVE_KEY: return "save key";
    }
    return NULL;
}

static code_completion_kind completion_kind(ui_source_completion_kind kind)
{
    switch (kind) {
        case UI_COMPLETE_TAG:
        case UI_COMPLETE_CLASS: return CODE_COMPLETE_TYPE;
        case UI_COMPLETE_WIDGET: return CODE_COMPLETE_ASSET;
        case UI_COMPLETE_ATTRIBUTE:
        case UI_COMPLETE_PROPERTY: return CODE_COMPLETE_FIELD;
        case UI_COMPLETE_VARIABLE:
        case UI_COMPLETE_SAVE_KEY: return CODE_COMPLETE_VALUE;
    }
    return CODE_COMPLETE_UNKNOWN;
}

void ui_code_complete(
    const ui_source *source,
    int offset,
    const ui_source_project *project,
    ui_code_completions *out
)
{
    *out = (ui_code_completions){ 0 };
    ui_source_complete(source, offset, project, &out->found);
    out->start = out->found.start;
    for (int i = 0; i < acount(out->found.items) && i < UI_CODE_COMPLETIONS; i++) {
        const ui_source_completion *c = &out->found.items[i];
        apush(
            out->items,
            ((
                kit_code_completion
            ){ c->label, kind_detail(c->kind), NULL, completion_kind(c->kind) })
        );
    }
}

void ui_code_completions_free(ui_code_completions *completions)
{
    ui_source_completions_free(&completions->found);
    afree(completions->items);
    *completions = (ui_code_completions){ 0 };
}

/* ---------- Hover ---------- */

/* The property a declaration at `offset` names, or false. */
static bool property_at(const ui_source *source, int offset, char *name, size_t size)
{
    for (int i = 0; i < acount(source->spans); i++) {
        const ui_source_span *span = &source->spans[i];
        const char *text = source->text + span->start;
        if (span->kind == UI_SOURCE_PROPERTY && span->start <= offset &&
            offset < span->end && strncmp(text, "--", 2)) {
            snprintf(name, size, "%.*s", span->end - span->start, text);
            return true;
        }
    }
    return false;
}

char *ui_code_hover(const ui_source *source, int offset, const ui_source_project *project)
{
    char *text = ui_source_hover(source, offset, project);
    char name[128], initial[128];
    bool inherited, shorthand;
    if (!text || !property_at(source, offset, name, sizeof name) ||
        !rml_property_describe(name, initial, sizeof initial, &inherited, &shorthand)) {
        return text;
    }
    char detail[256];
    if (shorthand) {
        snprintf(detail, sizeof detail, "\nA shorthand");
    } else {
        snprintf(
            detail,
            sizeof detail,
            "\nInitial %s%s",
            initial[0] ? initial : "none",
            inherited ? " \xc2\xb7 inherited" : ""
        );
    }
    size_t size = strlen(text) + strlen(detail) + 1;
    text = xrealloc(text, size);
    strcat(text, detail);
    return text;
}

/* ---------- Places ---------- */

static bool same_symbol(const ui_source_symbol *a, const ui_source_symbol *b)
{
    return a->kind == b->kind && !strcmp(a->name, b->name);
}

static bool same_file(const char *a, const char *b)
{
    char x[PATH_MAX], y[PATH_MAX];
    return !strcmp(a, b) || (realpath(a, x) && realpath(b, y) && !strcmp(x, y));
}

static void add_place(
    ui_code_place **places,
    const char *path,
    ui_part part,
    const ui_source *source,
    int offset,
    bool definition
)
{
    ui_code_place place = { .part = part, .offset = offset, .definition = definition };
    snprintf(place.path, sizeof place.path, "%s", path);
    int column;
    ui_source_position(source, offset, &place.line, &column);
    apush(*places, place);
}

/* The places the buffer's own parts name the symbol, as they stand. */
static void own_places(
    const ui_buffer *b,
    const ui_source_symbol *symbol,
    ui_code_place **definitions,
    ui_code_place **uses
)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        const char *text = ui_buffer_text(b, (ui_part)p);
        if (!text) {
            continue;
        }
        ui_source *part = ui_source_parse(text, ui_code_language((ui_part)p));
        ui_source_symbol *found = NULL;
        ui_source_symbols(part, &found);
        for (int i = 0; i < acount(found); i++) {
            if (same_symbol(&found[i], symbol)) {
                add_place(
                    found[i].definition ? definitions : uses,
                    b->path,
                    (ui_part)p,
                    part,
                    found[i].start,
                    found[i].definition
                );
            }
        }
        afree(found);
        ui_source_free(part);
    }
}

/* Whether `path` is one of the buffer's files, which its parts stand for. */
static bool own_file(const ui_buffer *b, const char *path)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        if (b->files[p] && same_file(path, b->files[p])) {
            return true;
        }
    }
    return false;
}

static void other_places(
    const ui_buffer *b,
    const ui_source_symbol *symbol,
    const ui_source_project *project,
    ui_code_place **definitions,
    ui_code_place **uses
)
{
    ui_source_location *found = NULL;
    ui_source_references(project, symbol, &found);
    for (int i = 0; i < acount(found); i++) {
        const ui_source_location *l = &found[i];
        if (own_file(b, l->path)) {
            continue;
        }
        ui_code_place place = {
            .part = ends_with(l->path, UI_STYLE_EXTENSION) ? UI_PART_RCSS : UI_PART_RML,
            .offset = l->start,
            .line = l->line,
            .definition = l->definition,
        };
        char *document = ui_buffer_document(l->path);
        snprintf(place.path, sizeof place.path, "%s", document);
        free(document);
        apush(*(l->definition ? definitions : uses), place);
    }
    ui_source_locations_free(found);
}

ui_source_symbol ui_code_places(
    const ui_buffer *b,
    const ui_source *source,
    int offset,
    const ui_source_project *project,
    ui_code_place **places
)
{
    ui_source_symbol symbol = ui_source_symbol_at(source, offset);
    if (symbol.kind == UI_SYMBOL_NONE) {
        return symbol;
    }
    ui_code_place *uses = NULL;
    own_places(b, &symbol, places, &uses);
    other_places(b, &symbol, project, places, &uses);
    for (int i = 0; i < acount(uses); i++) {
        apush(*places, uses[i]);
    }
    afree(uses);
    return symbol;
}

bool ui_code_definition(
    const ui_buffer *b,
    const ui_source *source,
    int offset,
    const ui_source_project *project,
    ui_code_place *definition
)
{
    ui_code_place *places = NULL;
    ui_source_symbol symbol = ui_code_places(b, source, offset, project, &places);
    bool found = false;
    for (int i = 0; i < acount(places) && !found; i++) {
        const ui_code_place *p = &places[i];
        bool here = !strcmp(p->path, b->path) && p->part == b->part &&
                    p->offset == symbol.start;
        if (p->definition && !here) {
            *definition = *p;
            found = true;
        }
    }
    afree(places);
    return found;
}
