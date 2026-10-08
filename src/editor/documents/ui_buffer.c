#include "editor/documents/ui_buffer.h"

#include "assets/new_asset.h"
#include "assets/ui_document.h"
#include "assets/ui_source.h"
#include "support/array.h"
#include "support/rcss.h"
#include "support/file_io.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Typing within this many seconds of the last typed change joins its step. */
#define UI_BUFFER_TYPING 1.0
/* Seconds between looks at the file on disk. */
#define UI_BUFFER_POLL 1.0

static char *copy(const char *text)
{
    return text ? xstrdup(text) : NULL;
}

static bool same(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

/* FNV-1a over each text in turn, each ended by a byte no text holds. */
static uint64_t hash_texts(char *const *texts)
{
    uint64_t hash = 14695981039346656037ull;
    for (int p = 0; p < UI_PART_COUNT; p++) {
        for (const char *c = texts[p]; c && *c; c++) {
            hash = (hash ^ (unsigned char)*c) * 1099511628211ull;
        }
        hash = (hash ^ 0xff) * 1099511628211ull;
    }
    return hash;
}

static void free_parts(char **parts)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        free(parts[p]);
        parts[p] = NULL;
    }
}

static void clear_steps(ui_buffer_step **steps)
{
    for (int i = 0; i < acount(*steps); i++) {
        free_parts((*steps)[i].parts);
    }
    afree(*steps);
    *steps = NULL;
}

static bool ends_with(const char *text, const char *suffix)
{
    size_t length = strlen(text), suffix_length = strlen(suffix);
    return length >= suffix_length && !strcmp(text + length - suffix_length, suffix);
}

/* `path` with its extension `from` made `to`, owned. */
static char *with_extension(const char *path, const char *from, const char *to)
{
    size_t stem = strlen(path) - strlen(from), size = stem + strlen(to) + 1;
    char *out = xmalloc(size);
    snprintf(out, size, "%.*s%s", (int)stem, path, to);
    return out;
}

static bool file_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

char *ui_buffer_document(const char *path)
{
    if (!ends_with(path, UI_STYLE_EXTENSION)) {
        return xstrdup(path);
    }
    char *markup = with_extension(path, UI_STYLE_EXTENSION, UI_MARKUP_EXTENSION);
    if (file_exists(markup)) {
        return markup;
    }
    free(markup);
    return xstrdup(path);
}

char *ui_buffer_part_file(const char *path, ui_part part)
{
    if (ends_with(path, UI_STYLE_EXTENSION)) {
        return part == UI_PART_RCSS ? xstrdup(path) : NULL;
    }
    return part == UI_PART_RCSS
             ? with_extension(path, UI_MARKUP_EXTENSION, UI_STYLE_EXTENSION)
             : xstrdup(path);
}

/* Each part's file text on disk, the files' hash, and whether the document
 * reads: its own file must, and a ui asset's styles are empty without one. */
static bool read_parts(char *const *files, char **parts, uint64_t *hash, char *error, size_t size)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        parts[p] = files[p] ? file_read_text(files[p]) : NULL;
    }
    *hash = hash_texts(parts);
    ui_part own = files[UI_PART_RML] ? UI_PART_RML : UI_PART_RCSS;
    if (!parts[own]) {
        snprintf(error, size, "cannot read %s", files[own]);
        free_parts(parts);
        return false;
    }
    if (files[UI_PART_RML] && !parts[UI_PART_RCSS]) {
        parts[UI_PART_RCSS] = xstrdup("");
    }
    return true;
}

static void set_files(ui_buffer *b, const char *path)
{
    b->path = xstrdup(path);
    b->stylesheet = ends_with(path, UI_STYLE_EXTENSION);
    for (int p = 0; p < UI_PART_COUNT; p++) {
        b->files[p] = ui_buffer_part_file(path, (ui_part)p);
    }
}

ui_buffer *ui_buffer_open(const char *path, char *error, size_t size)
{
    assert(path[0] == '/');
    ui_buffer *b = xcalloc(1, sizeof *b);
    set_files(b, path);
    uint64_t hash = 0;
    if (!read_parts(b->files, b->parts, &hash, error, size)) {
        ui_buffer_free(b);
        return NULL;
    }
    for (int p = 0; p < UI_PART_COUNT; p++) {
        b->saved[p] = copy(b->parts[p]);
    }
    b->disk = b->disk_seen = hash;
    b->part = b->stylesheet ? UI_PART_RCSS : UI_PART_RML;
    b->typed = -1;
    b->revision = 1;
    ui_buffer_check(b);
    return b;
}

static void publish(ui_buffer *b)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        if (b->files[p]) {
            text_overlay_set(b->files[p], b->parts[p]);
        }
    }
    b->published = true;
}

/* The files stand again; true when the text stood in for them. */
static bool unpublish(ui_buffer *b)
{
    bool was = b->published;
    for (int p = 0; was && p < UI_PART_COUNT; p++) {
        if (b->files[p]) {
            text_overlay_set(b->files[p], NULL);
        }
    }
    b->published = false;
    return was;
}

void ui_buffer_free(ui_buffer *b)
{
    if (!b) {
        return;
    }
    unpublish(b);
    free(b->path);
    free_parts(b->files);
    free_parts(b->parts);
    free_parts(b->saved);
    clear_steps(&b->undo);
    clear_steps(&b->redo);
    free(b);
}

const char *ui_buffer_text(const ui_buffer *b, ui_part part)
{
    return b->parts[part];
}

/* ---------- Edits and history ---------- */

static ui_buffer_step snapshot(const ui_buffer *b, ui_part part)
{
    ui_buffer_step step = { .part = part };
    for (int p = 0; p < UI_PART_COUNT; p++) {
        step.parts[p] = copy(b->parts[p]);
    }
    return step;
}

static void apply(ui_buffer *b, ui_part part, const text_edit *edits, int count, bool join)
{
    assert(b->parts[part]);
    if (!join) {
        apush(b->undo, snapshot(b, part));
        clear_steps(&b->redo);
    }
    char *text = text_edits_apply(b->parts[part], edits, count);
    free(b->parts[part]);
    b->parts[part] = text;
    b->part = part;
    b->revision++;
}

void ui_buffer_edit(ui_buffer *b, ui_part part, const text_edit *edits, int count)
{
    b->typed = -1;
    apply(b, part, edits, count, false);
}

static bool valid_edits(const char *text, const text_edit *edits, int count)
{
    if (!text || count < 1 || !edits) {
        return false;
    }
    int length = (int)strlen(text);
    for (int i = 0; i < count; i++) {
        if (!edits[i].text || edits[i].start < 0 ||
            edits[i].start > edits[i].end || edits[i].end > length) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if ((edits[i].start < edits[j].end && edits[j].start < edits[i].end) ||
                (edits[i].start == edits[i].end && edits[j].start == edits[j].end &&
                 edits[i].start == edits[j].start)) {
                return false;
            }
        }
    }
    return true;
}

bool ui_buffer_collab_edit(ui_buffer *b, ui_part part, const text_edit *edits, int count, bool history)
{
    if (!b || part < 0 || part >= UI_PART_COUNT ||
        !valid_edits(b->parts[part], edits, count)) {
        return false;
    }
    if (history) {
        ui_buffer_edit(b, part, edits, count);
        return true;
    }
    char *text = text_edits_apply(b->parts[part], edits, count);
    free(b->parts[part]);
    b->parts[part] = text;
    b->part = part;
    b->typed = -1;
    b->revision++;
    return true;
}

bool ui_buffer_collab_type(ui_buffer *b, ui_part part, const text_edit *edits, int count, double now)
{
    if (!b || part < 0 || part >= UI_PART_COUNT ||
        !valid_edits(b->parts[part], edits, count)) {
        return false;
    }
    char *text = text_edits_apply(b->parts[part], edits, count);
    ui_buffer_type(b, part, text, now);
    free(text);
    return true;
}

void ui_buffer_join_edit(ui_buffer *b, ui_part part, const text_edit *edits, int count)
{
    assert(ui_buffer_can_undo(b));
    b->typed = -1;
    apply(b, part, edits, count, true);
}

void ui_buffer_type(ui_buffer *b, ui_part part, const char *text, double now)
{
    const char *old = b->parts[part];
    assert(old);
    int length = (int)strlen(old), typed = (int)strlen(text);
    int start = 0, end = 0;
    while (start < length && start < typed && old[start] == text[start]) {
        start++;
    }
    while (end < length - start && end < typed - start &&
           old[length - 1 - end] == text[typed - 1 - end]) {
        end++;
    }
    if (start == length && start == typed) {
        return;
    }
    char *inserted = xmalloc((size_t)(typed - start - end) + 1);
    snprintf(inserted, (size_t)(typed - start - end) + 1, "%s", text + start);
    text_edit edit = { start, length - end, inserted };
    bool join = b->typed >= 0 && now - b->typed < UI_BUFFER_TYPING &&
                b->part == part && acount(b->undo);
    apply(b, part, &edit, 1, join);
    b->typed = now;
    free(inserted);
}

bool ui_buffer_can_undo(const ui_buffer *b)
{
    return acount(b->undo) > 0;
}

bool ui_buffer_can_redo(const ui_buffer *b)
{
    return acount(b->redo) > 0;
}

bool ui_buffer_history_target(const ui_buffer *b, bool redo, ui_part *part, const char **text)
{
    const ui_buffer_step *steps = redo ? b->redo : b->undo;
    int count = redo ? acount(b->redo) : acount(b->undo);
    if (!count) {
        return false;
    }
    const ui_buffer_step *step = &steps[count - 1];
    *part = step->part;
    *text = step->parts[step->part];
    return true;
}

bool ui_buffer_history_edit_matches(const ui_buffer *b, bool redo, ui_part part, const text_edit *edits, int count)
{
    const ui_buffer_step *steps = redo ? b->redo : b->undo;
    int steps_count = redo ? acount(b->redo) : acount(b->undo);
    if (part < 0 || part >= UI_PART_COUNT || !steps_count ||
        steps[steps_count - 1].part != part ||
        !valid_edits(b->parts[part], edits, count)) {
        return false;
    }
    const ui_buffer_step *step = &steps[steps_count - 1];
    char *result = text_edits_apply(b->parts[part], edits, count);
    bool matches = same(result, step->parts[part]);
    free(result);
    for (int p = 0; matches && p < UI_PART_COUNT; p++) {
        if (p != (int)part && !same(b->parts[p], step->parts[p])) {
            matches = false;
        }
    }
    return matches;
}

/* Restores the last step of `from`, recording the text it replaces in `to`. */
static void replay(ui_buffer *b, ui_buffer_step **from, ui_buffer_step **to)
{
    ui_buffer_step step = apop(*from);
    apush(*to, snapshot(b, step.part));
    free_parts(b->parts);
    memcpy(b->parts, step.parts, sizeof b->parts);
    b->part = step.part;
    b->typed = -1;
    b->revision++;
}

void ui_buffer_undo(ui_buffer *b)
{
    assert(ui_buffer_can_undo(b));
    replay(b, &b->undo, &b->redo);
}

void ui_buffer_redo(ui_buffer *b)
{
    assert(ui_buffer_can_redo(b));
    replay(b, &b->redo, &b->undo);
}

bool ui_buffer_dirty(const ui_buffer *b)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        if (!same(b->parts[p], b->saved[p])) {
            return true;
        }
    }
    return false;
}

bool ui_buffer_shown(const ui_buffer *b)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        const char *shown = b->files[p] ? text_overlay_text(b->files[p]) : NULL;
        if (shown ? strcmp(shown, b->parts[p]) : !same(b->parts[p], b->saved[p])) {
            return false;
        }
    }
    return true;
}

/* ---------- The files' text ---------- */

bool ui_buffer_check(ui_buffer *b)
{
    char directory[1024];
    const char *slash = strrchr(b->path, '/');
    snprintf(directory, sizeof directory, "%.*s", (int)(slash - b->path), b->path);
    b->error[0] = '\0';
    bool ok;
    if (b->stylesheet) {
        char *flat = rcss_expand(
            b->parts[UI_PART_RCSS],
            directory,
            NULL,
            b->error,
            sizeof b->error
        );
        ok = flat != NULL;
        free(flat);
    } else {
        ui_document document = { 0 };
        ok = ui_document_read(
            b->parts[UI_PART_RML],
            b->parts[UI_PART_RCSS],
            directory,
            NULL,
            &document,
            b->error,
            sizeof b->error
        );
        ui_document_free(&document);
    }
    b->checked = b->revision;
    return ok;
}

bool ui_buffer_update(ui_buffer *b, double now, bool live)
{
    if (b->seen != b->revision) {
        b->seen = b->revision;
        b->seen_time = now;
    }
    bool settled = now - b->seen_time >= UI_BUFFER_SETTLE;
    bool fresh = settled && b->checked != b->revision;
    if (fresh) {
        ui_buffer_check(b);
    }
    if (!live) {
        return unpublish(b);
    }
    if (!settled || b->error[0] || (!fresh && b->published)) {
        return false;
    }
    if (!ui_buffer_dirty(b)) {
        return unpublish(b);
    }
    publish(b);
    return true;
}

/* ---------- Disk ---------- */

static uint64_t disk_hash(const ui_buffer *b)
{
    char *texts[UI_PART_COUNT] = { 0 };
    for (int p = 0; p < UI_PART_COUNT; p++) {
        texts[p] = b->files[p] ? file_read_text(b->files[p]) : NULL;
    }
    uint64_t hash = hash_texts(texts);
    free_parts(texts);
    return hash;
}

bool ui_buffer_on_disk(const ui_buffer *b)
{
    return disk_hash(b) == b->disk;
}

bool ui_buffer_poll_disk(ui_buffer *b, double now)
{
    if (now < b->disk_due) {
        return false;
    }
    b->disk_due = now + UI_BUFFER_POLL;
    uint64_t hash = disk_hash(b);
    bool changed = hash != b->disk_seen && hash != b->disk;
    b->disk_seen = hash;
    return changed;
}

bool ui_buffer_saves(const ui_buffer *b, ui_part part)
{
    return b->files[part] && (b->parts[part][0] || file_exists(b->files[part]));
}

void ui_buffer_saved(ui_buffer *b)
{
    for (int p = 0; p < UI_PART_COUNT; p++) {
        free(b->saved[p]);
        b->saved[p] = copy(b->parts[p]);
    }
    b->disk = b->disk_seen = disk_hash(b);
    b->typed = -1;
    unpublish(b);
}

static bool format_part(ui_buffer *b, ui_part part, bool join)
{
    assert(b && part >= 0 && part < UI_PART_COUNT && b->parts[part]);
    char *formatted = ui_source_format(b->parts[part], part == UI_PART_RML ? UI_SOURCE_RML : UI_SOURCE_RCSS);
    if (!strcmp(formatted, b->parts[part])) {
        free(formatted);
        return false;
    }
    text_edit edit = { 0, (int)strlen(b->parts[part]), formatted };
    if (join) {
        ui_buffer_join_edit(b, part, &edit, 1);
    } else {
        ui_buffer_edit(b, part, &edit, 1);
    }
    free(formatted);
    return true;
}

bool ui_buffer_format(ui_buffer *b, ui_part part)
{
    return format_part(b, part, false);
}

bool ui_buffer_format_for_save(ui_buffer *b)
{
    ui_part active = b->part;
    bool changed = false;
    for (int offset = 0; offset < UI_PART_COUNT; offset++) {
        int p = (active + offset) % UI_PART_COUNT;
        if (ui_buffer_saves(b, (ui_part)p)) {
            bool joined = format_part(b, (ui_part)p, changed);
            changed |= joined;
        }
    }
    b->part = active;
    return changed;
}

bool ui_buffer_reload(ui_buffer *b, char *error, size_t size)
{
    char *parts[UI_PART_COUNT] = { 0 };
    uint64_t hash = 0;
    if (!read_parts(b->files, parts, &hash, error, size)) {
        return false;
    }
    free_parts(b->parts);
    free_parts(b->saved);
    for (int p = 0; p < UI_PART_COUNT; p++) {
        b->parts[p] = parts[p];
        b->saved[p] = copy(parts[p]);
    }
    b->disk = b->disk_seen = hash;
    clear_steps(&b->undo);
    clear_steps(&b->redo);
    b->typed = -1;
    b->revision++;
    unpublish(b);
    ui_buffer_check(b);
    return true;
}

/* ---------- Checkpoints ---------- */

static yyjson_mut_val *parts_value(yyjson_mut_doc *doc, char *const *parts)
{
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int p = 0; p < UI_PART_COUNT; p++) {
        yyjson_mut_arr_append(array, parts[p] ? yyjson_mut_strcpy(doc, parts[p]) : yyjson_mut_null(doc));
    }
    return array;
}

static yyjson_mut_val *steps_value(yyjson_mut_doc *doc, const ui_buffer_step *steps)
{
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int i = 0; i < acount(steps); i++) {
        yyjson_mut_val *step = yyjson_mut_arr_add_obj(doc, array);
        yyjson_mut_obj_add_val(doc, step, "parts", parts_value(doc, steps[i].parts));
        yyjson_mut_obj_add_int(doc, step, "part", steps[i].part);
    }
    return array;
}

yyjson_mut_val *ui_buffer_checkpoint(const ui_buffer *b, yyjson_mut_doc *doc)
{
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, value, "path", b->path);
    yyjson_mut_obj_add_val(doc, value, "parts", parts_value(doc, b->parts));
    yyjson_mut_obj_add_val(doc, value, "saved", parts_value(doc, b->saved));
    yyjson_mut_obj_add_uint(doc, value, "disk", b->disk);
    yyjson_mut_obj_add_int(doc, value, "part", b->part);
    yyjson_mut_obj_add_val(doc, value, "undo", steps_value(doc, b->undo));
    yyjson_mut_obj_add_val(doc, value, "redo", steps_value(doc, b->redo));
    return value;
}

static bool read_parts_value(yyjson_val *array, bool stylesheet, char **parts)
{
    if (yyjson_arr_size(array) != UI_PART_COUNT) {
        return false;
    }
    for (int p = 0; p < UI_PART_COUNT; p++) {
        yyjson_val *text = yyjson_arr_get(array, p);
        bool absent = stylesheet && p == UI_PART_RML;
        if (absent ? !yyjson_is_null(text) : !yyjson_is_str(text)) {
            return false;
        }
        parts[p] = absent ? NULL : xstrdup(yyjson_get_str(text));
    }
    return true;
}

static bool read_part(yyjson_val *value, ui_part *part)
{
    int index = yyjson_get_int(value);
    *part = (ui_part)index;
    return yyjson_is_int(value) && index >= 0 && index < UI_PART_COUNT;
}

static bool read_steps(yyjson_val *array, bool stylesheet, ui_buffer_step **steps)
{
    size_t i, count;
    yyjson_val *item;
    yyjson_arr_foreach(array, i, count, item)
    {
        ui_buffer_step step = { 0 };
        bool ok = read_parts_value(yyjson_obj_get(item, "parts"), stylesheet, step.parts) &&
                  read_part(yyjson_obj_get(item, "part"), &step.part);
        apush(*steps, step);
        if (!ok) {
            return false;
        }
    }
    return yyjson_is_arr(array);
}

ui_buffer *ui_buffer_restore(yyjson_val *value)
{
    const char *path = yyjson_get_str(yyjson_obj_get(value, "path"));
    if (!path || path[0] != '/') {
        return NULL;
    }
    ui_buffer *b = xcalloc(1, sizeof *b);
    set_files(b, path);
    b->disk = b->disk_seen = yyjson_get_uint(yyjson_obj_get(value, "disk"));
    b->typed = -1;
    b->revision = 1;
    if (!read_parts_value(yyjson_obj_get(value, "parts"), b->stylesheet, b->parts) ||
        !read_parts_value(yyjson_obj_get(value, "saved"), b->stylesheet, b->saved) ||
        !read_part(yyjson_obj_get(value, "part"), &b->part) ||
        !read_steps(yyjson_obj_get(value, "undo"), b->stylesheet, &b->undo) ||
        !read_steps(yyjson_obj_get(value, "redo"), b->stylesheet, &b->redo) ||
        !b->parts[b->part]) {
        ui_buffer_free(b);
        return NULL;
    }
    return b;
}

/* ---------- New assets ---------- */

/* Names become file names and <use ui="Name"> values. */
char *ui_buffer_new_path(const char *folder, ui_new_kind kind, const char *name, char *error, size_t size)
{
    if (!new_asset_name_valid(name, error, size)) {
        return NULL;
    }
    const char *extension = kind == UI_NEW_STYLESHEET ? UI_STYLE_EXTENSION
                                                      : UI_MARKUP_EXTENSION;
    size_t length = strlen(folder) + strlen(name) + strlen(extension) + 2;
    char *path = xmalloc(length);
    snprintf(path, length, "%s/%s%s", folder, name, extension);
    for (int p = 0; p < UI_PART_COUNT; p++) {
        char *file = ui_buffer_part_file(path, (ui_part)p);
        bool taken = file && file_exists(file);
        if (taken) {
            snprintf(error, size, "%s already exists", strrchr(file, '/') + 1);
        }
        free(file);
        if (taken) {
            free(path);
            return NULL;
        }
    }
    return path;
}

const char *ui_buffer_template(ui_new_kind kind, ui_part part)
{
    static const char *const RML[UI_NEW_KIND_COUNT] = {
        [UI_NEW_WIDGET] = "<div class=\"content\"><slot/></div>",
        [UI_NEW_DOCUMENT] = "<div class=\"panel\">\n  <div "
                            "class=\"title\">Title</div>\n</div>\n",
    };
    static const char *const RCSS[UI_NEW_KIND_COUNT] = {
        [UI_NEW_WIDGET] = "body { display: block; }\n"
                          ".content { display: flex; align-items: center; gap: "
                          "6dp; }\n",
        [UI_NEW_DOCUMENT] = "body { font-family: Inter; font-size: 16dp; "
                            "line-height: 24dp; color: #fff; }\n"
                            ".panel { display: block; padding: 12dp; }\n"
                            ".title { font-size: 16dp; font-weight: 600; }\n",
        [UI_NEW_STYLESHEET] = "/* Rules ui assets share with @import. */\n",
    };
    return part == UI_PART_RML ? RML[kind] : RCSS[kind];
}
