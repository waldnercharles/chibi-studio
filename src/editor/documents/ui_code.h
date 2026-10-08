#ifndef CHIBI_UI_CODE_H
#define CHIBI_UI_CODE_H

#include "assets/ui_source.h"
#include "editor/documents/ui_buffer.h"
#include "editor/ui/kit.h"
#include "authoring/ui_check.h"

#include <stdbool.h>
#include <stddef.h>

/* Code intelligence for the Text Editor, which shows one part of a UI
 * document (ui_buffer.h) alone: the language service (assets/ui_source.h)
 * and RmlUi's check (authoring/ui_check.h) in the part's terms. Offsets are
 * bytes of a part's text and lines are 1-based lines of it. */

/* ---------- The editor over a buffer ----------
 *
 * One history: the buffer keeps it and the code editor keeps none
 * (kit_code_desc.history). The editor's text reaches the buffer as typed
 * text, or as a step of its own; the undo keys step the buffer; and whatever
 * changes the buffer comes back to the editor as one edit, so its cursor
 * keeps its place. */

/* Bytes [start, end) of one text became `inserted` bytes at start. */
typedef struct ui_code_change
{
    int start, end, inserted;
} ui_code_change;
/* Computes the single edit from the buffer's current `part` to the editor's
 * text, rebasing the editor change from `base`. The owned `edit->text` is
 * empty when there is no change; free it after use. */
bool ui_code_prepare(
    const ui_buffer *b,
    ui_part part,
    kit_code_editor *e,
    const char *base,
    ui_code_change *typed,
    text_edit *edit
);
/* Computes one edit from `from` to `to`; `edit->text` is owned. */
bool ui_code_text_diff(const char *from, const char *to, text_edit *edit);
/* Records what the editor's text changed from `base`, the text the editor
 * and the buffer's `part` last shared, into the part as it stands: a change
 * made meanwhile elsewhere, as a designer edit in the same frame, composes
 * with the typing instead of being overwritten, unless both changed the
 * same bytes, when the editor's text stands. Typed text joins the step
 * being typed (ui_buffer_type), a `step` is one of its own
 * (ui_buffer_edit). Returns what changed from `base`, in the editor's
 * text. */
ui_code_change ui_code_record(
    ui_buffer *b,
    ui_part part,
    kit_code_editor *e,
    const char *base,
    double now,
    bool step
);
/* Shows `text` in the editor through the one edit that makes it, as
 * kit_code_apply takes edits; with `land`, the cursor lands at the end of
 * the change, as after an undo. True when the text changed. */
bool ui_code_follow(kit_code_editor *e, const char *text, bool land);
/* Steps the buffer's history as Studio's Undo and Redo do; false with
 * nothing to step. */
bool ui_code_replay(ui_buffer *b, bool redo);

/* Completions a list offers at most; typing narrows the rest. */
#define UI_CODE_COMPLETIONS 60

ui_source_language ui_code_language(ui_part part);

/* The content root a UI document lies in: the folder holding the ui folder
 * above it, where its widgets and imports resolve. False outside one. */
bool ui_code_content_root(const char *path, char *root, size_t size);

/* What completion and navigation know of the project around `content_root`:
 * its ui assets and stylesheets as text_file_read reads them, with the
 * properties RmlUi registers. Free with ui_source_project_free. */
void ui_code_scan(ui_source_project *project, const char *content_root);

/* RmlUi's warnings for a ui asset's text as it stands, loaded in
 * `context`, a scratch context the caller keeps (ui_check_in), or a
 * stylesheet's, its imports resolved beside it (ui_check_stylesheet);
 * false with why in `error` when it cannot be checked. A ui asset outside
 * a ui folder has none. */
bool ui_code_check(
    const ui_buffer *b,
    rml_context *context,
    ui_check_message **messages,
    char *error,
    size_t size
);

/* Gutter markers for `part`, in line order: each syntax error of `source`,
 * the part parsed alone, then each of RmlUi's `messages` about the part
 * (ui_code_check), and `failure`, why it could not be checked, on the
 * markup. A message about a widget or an import marks the line that uses
 * it. Each spans what it is about where that is known: a syntax error's
 * range, the text naming the widget or import, or the text RmlUi quotes
 * on the line; else the line. Markers own their messages. */
void ui_code_markers(
    const ui_source *source,
    ui_part part,
    const ui_check_message *messages,
    const char *failure,
    kit_code_marker **markers
);
void ui_code_markers_free(kit_code_marker *markers);

/* Completion at `offset` of a part: ui_source_complete's items as kit_code
 * shows them, each with its kind, replacing from `start`. */
typedef struct ui_code_completions
{
    ui_source_completions found;
    kit_code_completion *items; /* array; labels are found's */
    int start;
} ui_code_completions;
void ui_code_complete(
    const ui_source *source,
    int offset,
    const ui_source_project *project,
    ui_code_completions *out
);
void ui_code_completions_free(ui_code_completions *completions);

/* What the text names at `offset`, as the hover card shows it
 * (ui_source_hover), with RmlUi's knowledge of an RCSS property: its initial
 * value and whether it inherits. Owned; NULL where it names nothing. */
char *ui_code_hover(const ui_source *source, int offset, const ui_source_project *project);

/* A place in a UI document's part. */
typedef struct ui_code_place
{
    char path[1024];
    ui_part part;
    int offset;
    int line;
    bool definition;
} ui_code_place;

/* The symbol at `offset` of the buffer's shown part, parsed as `source`,
 * and every place that names it, definitions first: the buffer's parts as
 * they stand, then the project's other files. Kind NONE names nothing. */
ui_source_symbol ui_code_places(
    const ui_buffer *b,
    const ui_source *source,
    int offset,
    const ui_source_project *project,
    ui_code_place **places
);
/* Where Go to Definition goes from `offset`: the first definition besides
 * the one there; false when there is none. */
bool ui_code_definition(
    const ui_buffer *b,
    const ui_source *source,
    int offset,
    const ui_source_project *project,
    ui_code_place *definition
);

#endif
