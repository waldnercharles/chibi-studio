/* The Hierarchy (workstream H, authored in D1): while a UI document is
 * active, the Outliner lists its elements, as Unity's UI Builder and
 * Unreal's UMG Designer list a widget's tree: rows select and fold, drag to
 * reorder and reparent, or move with the keyboard as dnd-kit's keyboard
 * sensor moves them (workstream Q), and a menu adds, duplicates, wraps and
 * deletes. Hierarchy.rml and HierarchyHeader.rml are the panel; C keeps the
 * selection, the folds, the filter and every change to the document, which
 * studio_designer.c makes as text edits.
 *
 * The `hierarchy` source publishes the document's row and each area's
 * window of element rows (ui_list_window) and takes the panel's intents;
 * `hierarchy_ui`, apart so a selection reads only its views, holds the
 * selected row's key (TreeRow's data-alias-selection and -active) and the
 * row menu; `hierarchy_carry` holds a row carried with the keyboard;
 * `hierarchy_header` what the header's Add says.
 *
 *   hierarchy: { query, empty, root: ROW, views: [{ root, rows: [ROW] }] }
 *   ROW: { key, label, icon, meta, payload, zones, depth, parent, open,
 *          shown, marked, hidden, bound }
 *
 * hierarchy.click, .open (a double click), .menu and .drop name a row by
 * key ("doc", or "e<element>"); .drop takes the payload ("#<element>" of a
 * row, or a Library item's markup) and the zone; .duplicate, .wrap,
 * .source, .widget and .delete act on the element the menu opened for;
 * .scroll wakes the source. */
#include "editor/studio/studio_designer_internal.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "ui/drag.h"
#include "ui/list.h"
#include "ui/popover.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Characters of an element's text its row shows. */
#define EXCERPT 32
/* The document's row in a window, and its key. */
#define DOCUMENT_KEY "doc"
/* The List of each area's Hierarchy, in its host `hierarchy-view-<area>`. */
#define LIST_SELECTOR "#hierarchy-list"

static const char *const PLACE_NAMES[] = { "inside", "before", "after" };

/* ---------- Rows ---------- */

static bool has_children(const ui_source *rml, int element)
{
    return element + 1 < acount(rml->elements) &&
           rml->elements[element + 1].parent == element;
}

static int depth_of(const ui_source *rml, int element)
{
    int depth = 0;
    for (int i = rml->elements[element].parent; i >= 0; i = rml->elements[i].parent) {
        depth++;
    }
    return depth;
}

static bool has_bindings(const ui_source *rml, int element)
{
    const ui_source_element *el = &rml->elements[element];
    for (int i = 0; i < acount(el->attributes); i++) {
        const ui_source_attribute *a = &el->attributes[i];
        if (ui_source_is_binding(rml->text + a->name_start, a->name_end - a->name_start)) {
            return true;
        }
    }
    return false;
}

static void append(char *out, size_t size, const char *text, int length)
{
    size_t used = strlen(out);
    if (used + 1 < size) {
        snprintf(out + used, size - used, "%.*s", length, text);
    }
}

/* The start of an element's text, its spaces collapsed, in quotes. */
static void append_excerpt(char *out, size_t size, const ui_source *rml, int element)
{
    const ui_source_element *el = &rml->elements[element];
    char excerpt[EXCERPT + 8] = "";
    int length = 0;
    bool space = false;
    for (int i = el->open_end; i < el->close_start && length < EXCERPT; i++) {
        char c = rml->text[i];
        bool blank = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (!blank && space && length) {
            excerpt[length++] = ' ';
        }
        space = blank;
        if (!blank) {
            excerpt[length++] = c;
        }
    }
    excerpt[length] = '\0';
    if (length) {
        append(out, size, " \"", 2);
        append(out, size, excerpt, length);
        append(out, size, length == EXCERPT ? "\xe2\x80\xa6\"" : "\"", length == EXCERPT ? 4 : 1);
    }
}

/* "#id.a.b", then the text of an element holding text alone. */
static void element_meta(const ui_source *rml, int element, char *out, size_t size)
{
    const ui_source_element *el = &rml->elements[element];
    out[0] = '\0';
    if (el->id) {
        append(out, size, "#", 1);
        append(out, size, el->id, (int)strlen(el->id));
    }
    for (const char *c = el->classes ? el->classes : ""; *c;) {
        size_t n = strcspn(c, " \t\n");
        if (n) {
            append(out, size, ".", 1);
            append(out, size, c, (int)n);
        }
        c += n + (c[n] != '\0');
    }
    if (ui_design_text_only(rml, element)) {
        append_excerpt(out, size, rml, element);
    }
}

/* An element's place among its siblings down from the top, "0.2.1": folds
 * keep to places, so they outlast edits elsewhere. */
static void place_of(const ui_source *rml, int element, char *out, size_t size)
{
    if (element < 0) {
        out[0] = '\0';
        return;
    }
    int parent = rml->elements[element].parent, nth = 0;
    for (int i = parent + 1; i < element; i++) {
        nth += rml->elements[i].parent == parent;
    }
    char above[160];
    place_of(rml, parent, above, sizeof above);
    snprintf(out, size, "%s%s%d", above, above[0] ? "." : "", nth);
}

static int fold_index(const studio_designer *d, int element)
{
    char place[160];
    place_of(d->rml, element, place, sizeof place);
    for (int i = 0; i < acount(d->folded); i++) {
        if (!strcmp(d->folded[i], place)) {
            return i;
        }
    }
    return -1;
}

static void set_folded(studio_designer *d, int element, bool fold)
{
    int at = fold_index(d, element);
    if (fold && at < 0 && has_children(d->rml, element)) {
        char place[160];
        place_of(d->rml, element, place, sizeof place);
        apush(d->folded, xstrdup(place));
        d->folds++;
    } else if (!fold && at >= 0) {
        free(d->folded[at]);
        d->folded[at] = d->folded[acount(d->folded) - 1];
        asetlen(d->folded, acount(d->folded) - 1);
        d->folds++;
    }
}

static void set_closed(studio_designer *d, bool closed)
{
    d->folds += d->closed != closed;
    d->closed = closed;
}

void designer_hierarchy_forget(studio_designer *d)
{
    for (int i = 0; i < acount(d->folded); i++) {
        free(d->folded[i]);
    }
    afree(d->folded);
    afree(d->tree);
}

static bool row_matches(const studio_designer *d, int element)
{
    char name[96], meta[256];
    designer_element_name(d->rml, element, name, sizeof name);
    element_meta(d->rml, element, meta, sizeof meta);
    return studio_matches(d->filter, name) || studio_matches(d->filter, meta);
}

static bool subtree_matches(const studio_designer *d, int element)
{
    int end = element + ui_design_extent(d->rml, element);
    for (int i = element; i < end; i++) {
        if (row_matches(d, i)) {
            return true;
        }
    }
    return false;
}

/* Canvas and source selections must be visible even when the Hierarchy
   was folded or filtered before selecting them. */
void designer_reveal(studio_designer *d)
{
    d->reveal_row = true;
    if (d->selected < 0) {
        return;
    }
    set_closed(d, false);
    if (d->filter[0] && !subtree_matches(d, d->selected)) {
        d->filter[0] = '\0';
    }
    for (int parent = d->rml->elements[d->selected].parent; parent >= 0;
         parent = d->rml->elements[parent].parent) {
        set_folded(d, parent, false);
    }
}

/* The document, then the elements shown: unfolded, or while filtering
 * those that match or hold a match. */
static tree_row *tree_rows(const studio_designer *d)
{
    tree_row *rows = NULL;
    apush(rows, ((tree_row){ -1, 0 }));
    bool filtering = d->filter[0] != '\0';
    for (int i = 0; i < acount(d->rml->elements) && (filtering || !d->closed);) {
        bool shown = !filtering || subtree_matches(d, i);
        if (shown) {
            apush(rows, ((tree_row){ i, depth_of(d->rml, i) + 1 }));
        }
        bool skip = !shown || (!filtering && fold_index(d, i) >= 0);
        i += skip ? ui_design_extent(d->rml, i) : 1;
    }
    return rows;
}

/* The rows, listed again only when the document, a fold or the filter
 * changed; true when they were. */
static bool list_rows(studio_designer *d)
{
    struct designer_tree_seen *seen = &d->tree_seen;
    if (seen->built && seen->parsed == d->parsed && seen->folds == d->folds &&
        seen->closed == d->closed && !strcmp(seen->filter, d->filter)) {
        return false;
    }
    afree(d->tree);
    d->tree = tree_rows(d);
    seen->built = true;
    seen->closed = d->closed;
    seen->parsed = d->parsed;
    seen->folds = d->folds;
    seen->generation++;
    snprintf(seen->filter, sizeof seen->filter, "%s", d->filter);
    return true;
}

/* ---------- Keys ---------- */

/* Up and Down walk the rows shown, Left and Right fold. */
void designer_step(studio *s, studio_designer *d, int direction)
{
    list_rows(d);
    int at = 0;
    for (int r = 0; r < acount(d->tree); r++) {
        at = d->tree[r].element == d->selected ? r : at;
    }
    int next = at + direction;
    if (next >= 0 && next < acount(d->tree)) {
        designer_select(s, d->tree[next].element);
    }
}

void designer_fold_selected(studio_designer *d, bool fold)
{
    if (d->selected >= 0) {
        set_folded(d, d->selected, fold);
    }
}

/* ---------- Keyboard reordering (workstream Q) ---------- */

/* dnd-kit's keyboard sensor over the Hierarchy: Space or Enter picks the
   selected row up, Up and Down move it through the gaps between rows at
   each depth, Space, Enter or Tab drops it and Escape cancels. Each step is
   announced in the status bar, as dnd-kit announces pickup, over, drop and
   cancel. */

/* The slots a carried row steps through, as the Hierarchy shows its rows:
   what folded rows hold is left out. */
static ui_design_slot *carry_slots(const studio_designer *d, int *home)
{
    int count = acount(d->rml->elements);
    bool *folded = xcalloc((size_t)count + 1, sizeof *folded);
    for (int i = 0; i < count; i++) {
        folded[i] = fold_index(d, i) >= 0;
    }
    ui_design_slot *slots = NULL;
    *home = ui_design_slots(d->rml, d->carry.element, folded, &slots);
    free(folded);
    return slots;
}

static void announce_slot(studio *s, const studio_designer *d, const ui_design_slot *slot, int index, int count)
{
    static const char *const VERBS[] = { "Into", "Before", "After" };
    char name[96], message[192];
    if (slot->target >= 0) {
        designer_element_name(d->rml, slot->target, name, sizeof name);
    } else {
        snprintf(name, sizeof name, "%s", d->asset);
    }
    snprintf(message, sizeof message, "%s %s \xc2\xb7 %d of %d", VERBS[slot->place], name, index + 1, count);
    studio_report(s, message);
}

void designer_pick_up(studio *s, studio_designer *d, const ui_buffer *b)
{
    if (d->selected < 0 || d->filter[0]) {
        studio_report(s, d->filter[0] ? "Clear the filter to move a row with the keyboard" : "Select a row to move");
        return;
    }
    set_closed(d, false);
    for (int a = d->rml->elements[d->selected].parent; a >= 0;
         a = d->rml->elements[a].parent) {
        set_folded(d, a, false);
    }
    d->carry.active = true;
    d->carry.element = d->selected;
    d->carry.revision = b->revision;
    ui_design_slot *slots = carry_slots(d, &d->carry.slot);
    d->carry.at = slots[d->carry.slot];
    afree(slots);
    char name[96], message[192];
    designer_element_name(d->rml, d->selected, name, sizeof name);
    snprintf(message, sizeof message, "Picked up %s \xc2\xb7 arrows move it, Space drops, Escape cancels", name);
    studio_report(s, message);
}

static void move_carried(studio *s, studio_designer *d, int direction)
{
    int home;
    ui_design_slot *slots = carry_slots(d, &home);
    int next = d->carry.slot + direction;
    d->carry.slot = next >= 0 && next < acount(slots) ? next : d->carry.slot;
    d->carry.at = slots[d->carry.slot];
    announce_slot(s, d, &d->carry.at, d->carry.slot, acount(slots));
    d->reveal_row = true;
    afree(slots);
}

static void drop_carried(studio *s, studio_designer *d, ui_buffer *b)
{
    int home;
    ui_design_slot *slots = carry_slots(d, &home);
    ui_design_slot slot = slots[d->carry.slot];
    bool stays = d->carry.slot == home;
    int element = d->carry.element;
    afree(slots);
    d->carry.active = false;
    if (stays) {
        studio_report(s, "Dropped where it was");
        return;
    }
    designer_move(s, b, element, slot.target, slot.place);
}

static void cancel_carry(studio *s, studio_designer *d, const char *why)
{
    d->carry.active = false;
    d->reveal_row = true;
    studio_report(s, why);
}

void designer_carry_key(studio *s, studio_designer *d, ui_buffer *b, SDL_Keycode key)
{
    switch (key) {
        case SDLK_UP:
        case SDLK_DOWN: move_carried(s, d, key == SDLK_DOWN ? 1 : -1); break;
        case SDLK_SPACE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_TAB: drop_carried(s, d, b); break;
        case SDLK_ESCAPE: cancel_carry(s, d, "Cancelled the move"); break;
        default: break;
    }
}

/* A change to the text, or a selection elsewhere, puts a carried row down
   where it was. */
void designer_keep_carry(studio *s, studio_designer *d, const ui_buffer *b)
{
    if (d->carry.active &&
        (b->revision != d->carry.revision || d->selected != d->carry.element)) {
        cancel_carry(s, d, "Cancelled the move");
    }
}

/* ---------- Data ---------- */

static const char *const ROW_TEXTS[] = { "key",     "label", "icon", "meta",
                                         "payload", "zones", NULL };
static const char *const ROW_FLAGS[] = { "parent", "open",  "shown", "marked",
                                         "hidden", "bound", NULL };

static yyjson_mut_val *member(yyjson_mut_val *object, const char *name)
{
    yyjson_mut_val *value = yyjson_mut_obj_get(object, name);
    assert(value);
    return value;
}

static void add_members(
    yyjson_mut_doc *doc,
    yyjson_mut_val *object,
    const char *const *texts,
    const char *const *flags
)
{
    for (int i = 0; texts && texts[i]; i++) {
        yyjson_mut_obj_add_strcpy(doc, object, texts[i], "");
    }
    for (int i = 0; flags && flags[i]; i++) {
        yyjson_mut_obj_add_bool(doc, object, flags[i], false);
    }
}

/* A TreeRow's item: an element's row, or the document's at depth 0. */
static yyjson_mut_val *row_new(yyjson_mut_doc *doc, int depth)
{
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    add_members(doc, row, ROW_TEXTS, ROW_FLAGS);
    yyjson_mut_obj_add_int(doc, row, "depth", depth);
    return row;
}

/* A MenuItem's item. */
static void add_menu_item(
    yyjson_mut_doc *doc,
    yyjson_mut_val *menu,
    const char *name,
    const char *label,
    uint32_t icon
)
{
    static const char *const TEXTS[] = { "label", "icon", "shortcut", NULL };
    static const char *const FLAGS[] = { "checked", "disabled", NULL };
    yyjson_mut_val *item = yyjson_mut_obj_add_obj(doc, menu, name);
    add_members(doc, item, TEXTS, FLAGS);
    char glyph[5];
    studio_glyph(glyph, icon);
    ui_data_set_text(doc, member(item, "label"), label);
    ui_data_set_text(doc, member(item, "icon"), glyph);
}

void studio_hierarchy_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *hierarchy = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "hierarchy");
    static const char *const TEXTS[] = { "query", "empty", NULL };
    add_members(doc, hierarchy, TEXTS, NULL);
    yyjson_mut_obj_add_val(doc, hierarchy, "root", row_new(doc, 0));
    yyjson_mut_val *views = yyjson_mut_obj_add_arr(doc, hierarchy, "views");
    for (int area = 0; area < STUDIO_AREA_COUNT; area++) {
        yyjson_mut_val *view = yyjson_mut_arr_add_obj(doc, views);
        yyjson_mut_obj_add_bool(doc, view, "root", false);
        yyjson_mut_obj_add_arr(doc, view, "rows");
    }
}

/* Gives `rows` `length` items, adding and removing at its end, so rows
 * already shown keep their elements; true when its length changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *rows, int length)
{
    int had = (int)yyjson_mut_arr_size(rows);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(rows, row_new(doc, 1));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(rows);
    }
    return had != length;
}

static bool put_glyph(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *name, uint32_t icon)
{
    char glyph[5];
    studio_glyph(glyph, icon);
    return ui_data_put_text(doc, object, name, glyph);
}

/* Writes the flags a row shows open, as C last wrote them, so a twisty the
   panel flipped reads as a difference. */
static bool put_open(yyjson_mut_val *row, bool open)
{
    bool changed = ui_data_put_bool(row, "open", open);
    return ui_data_put_bool(row, "shown", open) || changed;
}

/* The document's row: its asset, the elements it holds and its fold. */
static bool show_document(yyjson_mut_doc *doc, yyjson_mut_val *row, const studio_designer *d)
{
    char count[16];
    snprintf(count, sizeof count, "%d", acount(d->rml->elements));
    bool changed = ui_data_put_text(doc, row, "key", DOCUMENT_KEY);
    changed |= ui_data_put_text(doc, row, "label", d->asset);
    changed |= put_glyph(doc, row, "icon", KIT_ICON_LAYOUT);
    changed |= ui_data_put_text(doc, row, "meta", count);
    changed |= ui_data_put_bool(row, "parent", true);
    return put_open(row, !d->closed) || changed;
}

/* An element as a row: its name, glyph, id, classes and text, how it folds
   and whether it carries bindings. Elements that hold nothing drop between
   leaves, in halves; containers in quarters, as dnd-kit's trees do. */
static bool show_row(yyjson_mut_doc *doc, yyjson_mut_val *row, const studio_designer *d, const tree_row *shown)
{
    int e = shown->element;
    char key[16], name[96], meta[256], payload[16];
    snprintf(key, sizeof key, "e%d", e);
    snprintf(payload, sizeof payload, "#%d", e);
    designer_element_name(d->rml, e, name, sizeof name);
    element_meta(d->rml, e, meta, sizeof meta);
    bool parent = has_children(d->rml, e);
    bool changed = ui_data_put_text(doc, row, "key", key);
    changed |= ui_data_put_text(doc, row, "label", name);
    changed |= put_glyph(doc, row, "icon", designer_element_icon(d->rml, e));
    changed |= ui_data_put_text(doc, row, "meta", meta);
    changed |= ui_data_put_text(doc, row, "payload", payload);
    changed |= ui_data_put_text(doc, row, "zones", ui_design_container(d->rml, e) ? "tree" : "list");
    changed |= ui_data_put_int(row, "depth", shown->depth);
    changed |= ui_data_put_bool(row, "parent", parent);
    changed |= ui_data_put_bool(row, "bound", has_bindings(d->rml, e));
    return put_open(row, parent && fold_index(d, e) < 0) || changed;
}

/* ---------- The panel ---------- */

/* The element with `id` in Studio's documents, or NULL. */
static rml_element *by_id(studio *s, const char *id)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static void host_id(char *out, size_t size, int area)
{
    snprintf(out, size, "hierarchy-view-%d", area);
}

/* The List of area `area`'s Hierarchy, or NULL while the area shows none. */
static rml_element *area_list(studio *s, int area)
{
    if (studio_active_layout(s)->editors[area] != STUDIO_EDITOR_OUTLINER ||
        !studio_designer_active(s)) {
        return NULL;
    }
    char id[32];
    host_id(id, sizeof id, area);
    rml_element *host = by_id(s, id);
    return host ? rml_element_query_selector(host, LIST_SELECTOR) : NULL;
}

/* The row to scroll to: the selection, or while a row is carried the row
   its slot names; the document's row is the list's first. */
static int reveal_index(const studio_designer *d)
{
    int shown = d->carry.active ? d->carry.at.target : d->selected;
    int reveal = shown < 0 ? 0 : -1;
    for (int r = 1; r < acount(d->tree); r++) {
        reveal = d->tree[r].element == shown ? r : reveal;
    }
    return d->reveal_row ? reveal : -1;
}

/* What a view last showed: the rows' generation and its window, so a
   scroll that moves nothing writes nothing. */
static bool show_view(
    studio *s,
    yyjson_mut_doc *doc,
    studio_designer *d,
    int area,
    yyjson_mut_val *view,
    bool *shown
)
{
    rml_element *list = area_list(s, area);
    int first = 0, length = 0;
    if (list) {
        ui_list window = { .count = acount(d->tree), .reveal = reveal_index(d) };
        length = ui_list_window(list, &window, &d->lists[area], &first);
        *shown = true;
    }
    struct designer_view_seen *seen = &d->views[area];
    if (seen->built && seen->generation == d->tree_seen.generation &&
        seen->first == first && seen->length == length) {
        return false;
    }
    *seen = (struct designer_view_seen){ true, d->tree_seen.generation, first, length };
    /* The document's row is the list's first; the elements follow it. */
    bool root = length > 0 && first == 0;
    int start = first > 0 ? first : 1;
    yyjson_mut_val *rows = member(view, "rows");
    bool changed = ui_data_put_bool(view, "root", root);
    changed |= fit(doc, rows, length - root);
    yyjson_mut_val *row;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(rows);
    for (int i = start; (row = yyjson_mut_arr_iter_next(&it)); i++) {
        changed |= show_row(doc, row, d, &d->tree[i]);
    }
    return changed;
}

/* A row's twisty flipped its `open`, where C wrote `shown`: the fold
   follows it, and the click that flipped it selects nothing. The key names
   the element as the row was written. A row with nothing to fold gets its
   flag back. */
static void take_fold(studio_designer *d, yyjson_mut_val *row)
{
    bool open = yyjson_mut_get_bool(member(row, "open"));
    bool shown = yyjson_mut_get_bool(member(row, "shown"));
    if (open == shown) {
        return;
    }
    yyjson_mut_set_bool(member(row, "open"), shown);
    if (!yyjson_mut_get_bool(member(row, "parent"))) {
        return;
    }
    const char *key = yyjson_mut_get_str(member(row, "key"));
    snprintf(d->twisty, sizeof d->twisty, "%s", key);
    if (!strcmp(key, DOCUMENT_KEY)) {
        set_closed(d, !open);
        return;
    }
    int element = key[0] == 'e' ? atoi(key + 1) : -1;
    if (element >= 0 && element < acount(d->rml->elements)) {
        set_folded(d, element, !open);
    }
}

static void take_view_state(studio_designer *d, yyjson_mut_val *hierarchy, bool wrote)
{
    d->twisty[0] = '\0';
    const char *query = yyjson_mut_get_str(member(hierarchy, "query"));
    if (wrote && strcmp(query, d->filter)) {
        snprintf(d->filter, sizeof d->filter, "%s", query);
    }
    take_fold(d, member(hierarchy, "root"));
    yyjson_mut_val *views = member(hierarchy, "views");
    for (int area = 0; area < STUDIO_AREA_COUNT; area++) {
        yyjson_mut_val *rows = member(yyjson_mut_arr_get(views, (size_t)area), "rows");
        yyjson_mut_val *row;
        yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(rows);
        while ((row = yyjson_mut_arr_iter_next(&it))) {
            take_fold(d, row);
        }
    }
}

bool studio_hierarchy_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = at->preview ? NULL : designer_state(s, &b);
    if (!d) {
        return false;
    }
    yyjson_mut_val *hierarchy = at->variable;
    take_view_state(d, hierarchy, at->wrote);
    list_rows(d);
    const char *empty = d->filter[0] && acount(d->tree) == 1 ? "No matches" : "";
    bool changed = ui_data_put_text(at->doc, hierarchy, "query", d->filter);
    changed |= ui_data_put_text(at->doc, hierarchy, "empty", empty);
    changed |= show_document(at->doc, member(hierarchy, "root"), d);
    yyjson_mut_val *views = member(hierarchy, "views");
    bool shown = false;
    for (int area = 0; area < STUDIO_AREA_COUNT; area++) {
        changed |= show_view(s, at->doc, d, area, yyjson_mut_arr_get(views, (size_t)area), &shown);
    }
    d->reveal_row &= !shown;
    return changed;
}

/* ---------- What changes with each selection ---------- */

void studio_hierarchy_ui_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *ui = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "hierarchy_ui");
    yyjson_mut_obj_add_strcpy(doc, ui, "selected", "");
    yyjson_mut_val *menu = yyjson_mut_obj_add_obj(doc, ui, "menu");
    yyjson_mut_obj_add_strcpy(doc, menu, "label", "");
    yyjson_mut_obj_add_bool(doc, menu, "element", false);
    yyjson_mut_obj_add_bool(doc, menu, "widget", false);
    add_menu_item(doc, menu, "add", "Add", KIT_ICON_ADD);
    add_menu_item(doc, menu, "duplicate", "Duplicate", KIT_ICON_COPY);
    add_menu_item(doc, menu, "wrap", "Wrap in Div", KIT_ICON_WRAP);
    add_menu_item(doc, menu, "source", "Edit Source", KIT_ICON_EDIT);
    add_menu_item(doc, menu, "openWidget", "Open Widget", KIT_ICON_EXTERNAL);
    add_menu_item(doc, menu, "delete", "Delete", KIT_ICON_TRASH);
}

/* A row's key for an element, "doc" for the document. */
static void key_of(int element, char out[16])
{
    if (element < 0) {
        snprintf(out, 16, "%s", DOCUMENT_KEY);
        return;
    }
    snprintf(out, 16, "e%d", element);
}

bool studio_hierarchy_ui_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = at->preview ? NULL : designer_state(s, &b);
    if (!d) {
        return false;
    }
    yyjson_mut_val *ui = at->variable;
    yyjson_mut_val *menu = member(ui, "menu");
    char key[16], name[96];
    key_of(d->selected, key);
    bool changed = ui_data_put_text(at->doc, ui, "selected", key);
    int e = d->menu >= 0 && d->menu < acount(d->rml->elements) ? d->menu : -1;
    if (e >= 0) {
        designer_element_name(d->rml, e, name, sizeof name);
    } else {
        snprintf(name, sizeof name, "%s", d->asset);
    }
    changed |= ui_data_put_text(at->doc, menu, "label", name);
    changed |= ui_data_put_bool(menu, "element", e >= 0);
    changed |= ui_data_put_bool(
        menu,
        "widget",
        e >= 0 && !strcmp(d->rml->elements[e].tag, "use")
    );
    changed |= ui_data_put_text(at->doc, member(menu, "duplicate"), "shortcut", studio_chord(s, "Shift+D"));
    changed |= ui_data_put_text(at->doc, member(menu, "delete"), "shortcut", "X");
    return changed;
}

void studio_hierarchy_carry_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *carry = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "hierarchy_carry");
    static const char *const TEXTS[] = { "element", "target", "place", NULL };
    add_members(doc, carry, TEXTS, NULL);
}

/* The row carried with the keyboard and the row and place its slot names,
 * which the rows mark as a pointer drag marks its target. */
bool studio_hierarchy_carry_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = at->preview ? NULL : designer_state(s, &b);
    char element[16] = "", target[16] = "";
    const char *place = "";
    if (d && d->carry.active) {
        key_of(d->carry.element, element);
        key_of(d->carry.at.target, target);
        place = PLACE_NAMES[d->carry.at.place];
    }
    bool changed = ui_data_put_text(at->doc, at->variable, "element", element);
    changed |= ui_data_put_text(at->doc, at->variable, "target", target);
    return ui_data_put_text(at->doc, at->variable, "place", place) || changed;
}

void studio_hierarchy_header_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *header = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "hierarchy_header");
    yyjson_mut_obj_add_strcpy(doc, header, "about", "");
}

bool studio_hierarchy_header_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = at->preview ? NULL : designer_state(s, &b);
    return d && ui_data_put_text(
                    at->doc,
                    at->variable,
                    "about",
                    d->selected >= 0 ? "Into the selection, or after it" : "Into the document"
                );
}

/* ---------- Intents ---------- */

/* The element a row key names: -1 for the document, -2 for none. */
static int element_of(const studio_designer *d, const char *key)
{
    if (!strcmp(key, DOCUMENT_KEY)) {
        return -1;
    }
    int element = key[0] == 'e' ? atoi(key + 1) : -2;
    return element >= 0 && element < acount(d->rml->elements) ? element : -2;
}

/* A right press selects the row and opens the row menu at the pointer. */
static void open_menu(studio *s, studio_designer *d, int element, const ui_activation *a)
{
    designer_select(s, element);
    d->menu = element;
    studio_ui_resync(s, "hierarchy_ui");
    rml_element *row = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    rml_element *menu = row
                          ? rml_element_query_selector(rml_element_owner_document(row), "#hierarchy-menu")
                          : NULL;
    if (menu) {
        popover_show(menu, &(popover_anchor){ .side = POPOVER_POINT, .box = { a->x, a->y, 0, 0 } });
    }
}

static ui_source_place zone_place(const char *zone)
{
    return !strcmp(zone, "before") ? UI_SOURCE_BEFORE
         : !strcmp(zone, "after")  ? UI_SOURCE_AFTER
                                   : UI_SOURCE_INSIDE;
}

/* Opens the source of an element, or the widget a <use> shows. */
static void open_element(studio *s, studio_designer *d, int element)
{
    if (!strcmp(d->rml->elements[element].tag, "use")) {
        designer_open_widget(s, element);
        return;
    }
    studio_open_ui_source(s, d->path, d->rml->elements[element].line);
}

void studio_hierarchy_action(studio *s, const char *action, const ui_activation *a)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    if (!d || !strcmp(action, "scroll")) {
        return;
    }
    int element = a->argc ? element_of(d, a->args[0]) : -2;
    if (!strcmp(action, "click") && element > -2) {
        /* A twisty folded its row in this frame's sync; it selects nothing. */
        if (strcmp(d->twisty, a->args[0])) {
            designer_select(s, element);
        }
        studio_ui_resync(s, "hierarchy_ui");
    } else if (!strcmp(action, "open") && element >= 0) {
        open_element(s, d, element);
    } else if (!strcmp(action, "menu") && element > -2) {
        open_menu(s, d, element, a);
    } else if (!strcmp(action, "drop") && a->argc == 3) {
        /* A drop on the list itself, named by no row, goes into the document. */
        designer_drop(s, a->args[1], element == -2 ? -1 : element, zone_place(a->args[2]));
    } else if (!strcmp(action, "duplicate")) {
        designer_duplicate(s);
    } else if (!strcmp(action, "wrap")) {
        designer_wrap(s);
    } else if (!strcmp(action, "source") && d->menu >= 0) {
        studio_open_ui_source(s, d->path, d->rml->elements[d->menu].line);
    } else if (!strcmp(action, "widget") && d->menu >= 0) {
        designer_open_widget(s, d->menu);
    } else if (!strcmp(action, "delete")) {
        designer_delete(s);
    }
}

/* ---------- Panel ---------- */

void studio_designer_hierarchy_header(studio *s, int area)
{
    rml_element *host = studio_ui_area_controls(s, area, "hierarchy", "HierarchyHeader");
    rml_element *field = host && s->find_request == STUDIO_EDITOR_OUTLINER + 1
                           ? rml_element_query_selector(host, "#hierarchy-filter")
                           : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        s->find_request = 0;
    }
}

void studio_designer_hierarchy(studio *s, int area)
{
    rml_element *host = studio_ui_area_view(s, area, "Hierarchy", "hierarchy.views");
    if (!host) {
        return;
    }
    char id[32];
    host_id(id, sizeof id, area);
    kit_attr(s->kit, "id", id);
}

/* The element whose Hierarchy row the pointer is over, or -1. */
int designer_hovered_row(studio *s)
{
    rml_element *el = rml_context_hover_element(rml_backend_context(s->backend));
    for (; el; el = rml_element_parent(el)) {
        char key[16];
        if (rml_element_attribute(el, "data-element", key, sizeof key) > 0 &&
            key[0] == 'e') {
            return atoi(key + 1);
        }
    }
    return -1;
}
