#ifndef CHIBI_UI_BUFFER_H
#define CHIBI_UI_BUFFER_H

#include "support/text_edit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

/* The text of a UI document open for editing: a ui asset's RML (its .rml
 * file) and RCSS (the .rcss beside it), or a stylesheet's RCSS. Studio's
 * Text Editor and the designer change it through text_edits; the buffer
 * keeps the undo history and whether the text reads as its files would.
 * Published, the text stands in for the files (support/text_overlay.h), so
 * everything that reads them sees it. */
typedef enum ui_part
{
    UI_PART_RML,
    UI_PART_RCSS,
    UI_PART_COUNT,
} ui_part;

/* The parts before an undo step, and the part it changed. */
typedef struct ui_buffer_step
{
    char *parts[UI_PART_COUNT];
    ui_part part;
} ui_buffer_step;

typedef struct ui_buffer
{
    char *path;      /* absolute: the .rml, or a stylesheet's .rcss */
    bool stylesheet; /* an .rcss file alone, whose text is its RCSS part */
    char *files[UI_PART_COUNT]; /* each part's file; a stylesheet's RML has none */
    char *parts[UI_PART_COUNT]; /* a stylesheet's RML part is NULL */
    char *saved[UI_PART_COUNT]; /* as last loaded or saved */
    uint64_t disk;              /* the files' hash as last loaded or saved */
    uint64_t disk_seen;         /* their hash when last polled */
    double disk_due;            /* the next poll, in seconds */
    ui_part part;               /* the part the Text Editor shows */
    ui_buffer_step *undo, *redo;
    double typed; /* when the last typed change was made; -1 after other changes */
    unsigned revision; /* changes with each change to the parts */
    unsigned seen;     /* the revision ui_buffer_update last saw */
    double seen_time;  /* when it saw it */
    unsigned checked;  /* the revision `error` describes */
    bool published;    /* the text stands in for the files */
    char error[256];   /* why the text does not read; empty when it does */
} ui_buffer;

/* The owned path of the document a ui file belongs to: a ui asset's .rcss
 * belongs to the .rml beside it; any other file is its own. */
char *ui_buffer_document(const char *path);
/* The owned path of the file holding `part` of the document at `path` (see
 * ui_buffer_document); NULL for a stylesheet's RML. */
char *ui_buffer_part_file(const char *path, ui_part part);

/* Reads the document at `path`, a ui asset's .rml with the .rcss beside it
 * when there is one, or a stylesheet's .rcss; NULL with why in `error`. */
ui_buffer *ui_buffer_open(const char *path, char *error, size_t size);
/* Frees the buffer; the files read from disk again. */
void ui_buffer_free(ui_buffer *b);
/* A part's text; NULL for a stylesheet's RML. */
const char *ui_buffer_text(const ui_buffer *b, ui_part part);

/* Applies `edits` to a part as one undo step. */
void ui_buffer_edit(ui_buffer *b, ui_part part, const text_edit *edits, int count);
/* Applies an accepted collaboration edit. `history` records a normal local
 * step (for the host or its author); remote replays leave this buffer's
 * local undo and redo stacks alone. */
bool ui_buffer_collab_edit(ui_buffer *b, ui_part part, const text_edit *edits, int count, bool history);
/* Applies accepted typed ranges with the buffer's usual typing coalescing. */
bool ui_buffer_collab_type(ui_buffer *b, ui_part part, const text_edit *edits, int count, double now);
/* Applies `edits` to a part within the last undo step, so a change of both
 * parts, as ui_design_extract_class makes, undoes as one. */
void ui_buffer_join_edit(ui_buffer *b, ui_part part, const text_edit *edits, int count);
/* Replaces a part's text as typed `now`, in seconds: a change within a second
 * of the last one typed into the same part joins its undo step. */
void ui_buffer_type(ui_buffer *b, ui_part part, const char *text, double now);
bool ui_buffer_can_undo(const ui_buffer *b);
bool ui_buffer_can_redo(const ui_buffer *b);
/* Text the next history step would restore, and its part. Borrowed; false if
 * there is no step. */
bool ui_buffer_history_target(const ui_buffer *b, bool redo, ui_part *part, const char **text);
/* Whether the next history operation matches `edits` on `part` and leaves
 * every other part unchanged. */
bool ui_buffer_history_edit_matches(
    const ui_buffer *b,
    bool redo,
    ui_part part,
    const text_edit *edits,
    int count
);
/* Steps through the history; the Text Editor turns to the part changed. */
void ui_buffer_undo(ui_buffer *b);
void ui_buffer_redo(ui_buffer *b);
/* The text differs from the files as last loaded or saved. */
bool ui_buffer_dirty(const ui_buffer *b);
/* Whether what reads the files sees the text as it is: each part stands in
 * for its file, or equals it as last loaded or saved. */
bool ui_buffer_shown(const ui_buffer *b);

/* Whether the text reads as a ui asset (ui_document_read) or a stylesheet
 * (rcss_expand), with why not in `error`. */
bool ui_buffer_check(ui_buffer *b);
/* Once a change has rested for UI_BUFFER_SETTLE seconds, checks the text and,
 * with `live`, publishes it when it reads, so an invalid edit keeps the last
 * valid text everywhere. Without `live` the files stand. True when what
 * stands in for the files changed. */
#define UI_BUFFER_SETTLE 0.15
bool ui_buffer_update(ui_buffer *b, double now, bool live);

/* The files on disk are the ones loaded or last saved. */
bool ui_buffer_on_disk(const ui_buffer *b);
/* Another program changed the files since the last poll; polls at most once
 * a second. */
bool ui_buffer_poll_disk(ui_buffer *b, double now);
/* Whether saving writes `part`'s file: it has one, and it exists or the
 * part holds text. */
bool ui_buffer_saves(const ui_buffer *b, ui_part part);
/* Records the parts as written to their files. */
void ui_buffer_saved(ui_buffer *b);
/* Applies the fixed RML or RCSS formatter as one undo step. Invalid text and
 * already formatted text are left untouched; true means a change was made. */
bool ui_buffer_format(ui_buffer *b, ui_part part);
/* Formats the parts this document will save as a single undo step. */
bool ui_buffer_format_for_save(ui_buffer *b);
/* Replaces the text with the files', discarding edits and history; false
 * with why in `error`, keeping the text. */
bool ui_buffer_reload(ui_buffer *b, char *error, size_t size);

/* The buffer in workspace checkpoints (editor/workspace/checkpoint.h). */
yyjson_mut_val *ui_buffer_checkpoint(const ui_buffer *b, yyjson_mut_doc *doc);
ui_buffer *ui_buffer_restore(yyjson_val *value);

/* A new UI asset: a widget has a <slot/> for its use's content and RCSS
 * scoped to it; a document is a panel; a stylesheet is RCSS for others to
 * @import. */
typedef enum ui_new_kind
{
    UI_NEW_WIDGET,
    UI_NEW_DOCUMENT,
    UI_NEW_STYLESHEET,
    UI_NEW_KIND_COUNT,
} ui_new_kind;

/* The owned document path (see ui_buffer_document) of a new `kind` named
 * `name` in `folder`, the project's ui assets; NULL with why in `error` when
 * the name is invalid or one of its files exists. */
char *ui_buffer_new_path(const char *folder, ui_new_kind kind, const char *name, char *error, size_t size);
/* The text a new `kind`'s `part` starts with; NULL when it has no file. */
const char *ui_buffer_template(ui_new_kind kind, ui_part part);

#endif
