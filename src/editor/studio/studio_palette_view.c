/* The palette and the pickers (docs/studio-rml-migration.md, C4): one
 * ranked list of results its client fills, shown either as the ⌘K palette
 * (Palette.rml, a modal over the editor, cmdk's Command.Dialog) or as a
 * picker under the field that opened it (Picker.rml, a popover, as Unreal's
 * asset picker hangs under its property). Clients: the search
 * (studio_palette.c), the graph's add-node search (studio_graph_edit.c), Add
 * Component (studio_properties.c) and the asset pickers (studio_pick.c).
 *
 * C ranks (support/text_rank.h) when the query or what the client lists
 * changes, and windows the ranked rows into the `palette` variable as the
 * List scrolls (ui/list.h, with rows of three heights: group headings,
 * results and results with a second line). The highlighted result's place
 * lives apart in `palette_ui`, so moving it evaluates one view; the
 * virtualized <choice-list> marks it, sends `highlight` as keys and the
 * pointer move it and `change` as one is chosen. Both documents mount only
 * while open, so a closed palette costs nothing.
 *
 * A field's list (a graph input's source field in Properties) is the
 * client's markup over the same rows, under the field its text is typed in,
 * as Base UI's Combobox lists under its input.
 *
 * palette: { placeholder, empty, count,
 *   rows: [{ heading, label (markup, the match in bold), about, meta,
 *            shortcut, tip, icon, image, place, current }] }
 * palette_ui: { highlight } */
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_thumbnails.h"
#include "editor/ui/highlight.h"
#include "support/array.h"
#include "support/text_rank.h"
#include "support/text_search.h"
#include "ui/list.h"
#include "ui/popover.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ids in Palette.rml and Picker.rml. */
#define LIST_ID "palette-list"
#define SEARCH_ID "palette-search"
#define PICKER_ID "picker"
/* Rows written before the List has laid out. */
#define UNLAID_ROWS 24

/* ---------- Opening and closing ---------- */

static void reset(studio_palette_state *p, studio_palette_client client, const char *placeholder)
{
    rml_element_ref_destroy(p->anchor);
    rml_element_ref_destroy(p->list);
    *p = (studio_palette_state){
        .client = client,
        .mounted = p->mounted,
        .entries = p->entries,
        .rows = p->rows,
        .places = p->places,
        .tops = p->tops,
        .window = UI_LIST_STATE_INIT,
        .reveal = -1,
        .collect = true,
        .rank = true,
    };
    aclear(p->entries);
    aclear(p->rows);
    aclear(p->places);
    aclear(p->tops);
    snprintf(p->placeholder, sizeof p->placeholder, "%s", placeholder);
}

/* The modal closes every popup, as a dialog opening does; a picker's
   popover closes those it did not open from as it shows. */
void studio_palette_open(studio *s, studio_palette_client client, const char *placeholder)
{
    popover_hide_all(rml_backend_context(s->backend));
    reset(&s->palette, client, placeholder);
    s->ui_schedule.wake = true;
}

void studio_palette_open_at(studio *s, studio_palette_client client, const char *placeholder, rml_element *anchor)
{
    reset(&s->palette, client, placeholder);
    s->palette.anchored = true;
    s->palette.anchor = rml_element_ref_create(anchor);
    s->ui_schedule.wake = true;
}

void studio_palette_open_field(studio *s, studio_palette_client client, rml_element *field)
{
    studio_palette_open_at(s, client, "", field);
    s->palette.field = true;
}

void studio_palette_open_box(
    studio *s,
    studio_palette_client client,
    const char *placeholder,
    const float box[4]
)
{
    reset(&s->palette, client, placeholder);
    s->palette.anchored = s->palette.boxed = true;
    memcpy(s->palette.box, box, sizeof s->palette.box);
    s->ui_schedule.wake = true;
}

void studio_palette_close(studio *s)
{
    reset(&s->palette, STUDIO_PALETTE_CLOSED, "");
}

bool studio_palette_is_open(const studio *s)
{
    return s->palette.client && !s->palette.anchored;
}

const char *studio_palette_query(const studio *s)
{
    return s->palette.query;
}

void studio_palette_set_query(studio *s, const char *query)
{
    studio_palette_state *p = &s->palette;
    snprintf(p->query, sizeof p->query, "%s", query);
    p->typed = true;
    p->collect = p->rank = true;
}

rml_element *studio_palette_anchor(const studio *s)
{
    return s->palette.anchor ? rml_element_ref_get(s->palette.anchor) : NULL;
}

studio_palette_entry *studio_palette_add(
    studio_palette_entry **entries,
    const char *group,
    const char *label
)
{
    apush(*entries, ((studio_palette_entry){ .asset = -1 }));
    studio_palette_entry *e = &alast(*entries);
    snprintf(e->group, sizeof e->group, "%s", group ? group : "");
    snprintf(e->label, sizeof e->label, "%s", label);
    return e;
}

/* ---------- Clients ---------- */

static void collect(studio *s, studio_palette_state *p)
{
    aclear(p->entries);
    switch (p->client) {
        case STUDIO_PALETTE_SEARCH:
            studio_search_entries(s, p->query, &p->entries);
            break;
        case STUDIO_PALETTE_NODES:
            studio_graph_palette_entries(s, p->query, &p->entries);
            break;
        case STUDIO_PALETTE_COMPONENTS:
            studio_component_entries(s, &p->entries);
            break;
        case STUDIO_PALETTE_ASSETS:
            studio_pick_entries(s, p->query, &p->entries);
            break;
        case STUDIO_PALETTE_SOURCES:
            studio_graph_source_entries(s, p->query, &p->entries);
            break;
        case STUDIO_PALETTE_TYPES:
            studio_member_type_entries(s, &p->entries);
            break;
        case STUDIO_PALETTE_CHOICES:
            studio_graph_choice_entries(s, &p->entries);
            break;
        case STUDIO_PALETTE_CLOSED: break;
    }
}

/* Whether a query that changed from `was` lists other entries. */
static bool lists_anew(const studio_palette_state *p, const char *was)
{
    switch (p->client) {
        case STUDIO_PALETTE_SEARCH:
            return studio_search_lists_anew(was, p->query);
        case STUDIO_PALETTE_NODES:
        case STUDIO_PALETTE_ASSETS:
        case STUDIO_PALETTE_SOURCES: return true;
        case STUDIO_PALETTE_COMPONENTS:
        case STUDIO_PALETTE_TYPES:
        case STUDIO_PALETTE_CHOICES:
        case STUDIO_PALETTE_CLOSED: return false;
    }
    return false;
}

/* The text the rows rank against, and how. The palette keeps a few rows a
   group while filtering, as cmdk; a picker lists every match. */
static const char *rank_query(const studio_palette_state *p, unsigned *flags)
{
    *flags = p->anchored ? 0 : TEXT_RANK_LIMIT;
    switch (p->client) {
        case STUDIO_PALETTE_SEARCH: return studio_search_query(p->query);
        /* The graph's search ranks the nodes; the query still marks them. */
        case STUDIO_PALETTE_NODES: *flags = TEXT_RANK_ORDERED; return p->query;
        default: return p->query;
    }
}

static void choose(studio *s, int place)
{
    studio_palette_state *p = &s->palette;
    if (place < 0 || place >= acount(p->places)) {
        return;
    }
    studio_palette_entry entry = p->entries[p->rows[p->places[place]].entry];
    studio_palette_client client = p->client;
    rml_element *anchor = studio_palette_anchor(s);
    studio_palette_close(s);
    switch (client) {
        case STUDIO_PALETTE_SEARCH: studio_search_choose(s, &entry); break;
        case STUDIO_PALETTE_NODES:
            studio_graph_palette_choose(s, &entry);
            break;
        case STUDIO_PALETTE_COMPONENTS:
            studio_component_choose(s, &entry);
            break;
        case STUDIO_PALETTE_ASSETS:
            studio_pick_choose(s, &entry, anchor);
            break;
        case STUDIO_PALETTE_SOURCES:
            studio_graph_source_choose(s, &entry);
            break;
        case STUDIO_PALETTE_TYPES: studio_member_type_choose(s, &entry); break;
        case STUDIO_PALETTE_CHOICES:
            studio_graph_choice_choose(s, &entry);
            break;
        case STUDIO_PALETTE_CLOSED: break;
    }
}

/* ---------- Ranking ---------- */

/* What the entries list: the assets and the scene's objects. */
static uint64_t inputs_of(const studio *s)
{
    return s->scene->asset_index_revision * 1099511628211ull ^
           (uint64_t)acount(s->scene->actors);
}

/* Ranks the entries into rows, each group's heading before its first. */
static void rank(studio_palette_state *p)
{
    unsigned flags;
    const char *query = rank_query(p, &flags);
    text_rank_entry *ranked = NULL;
    for (int i = 0; i < acount(p->entries); i++) {
        const studio_palette_entry *e = &p->entries[i];
        apush(ranked, ((text_rank_entry){ e->label, e->keywords, e->group, e->boost }));
    }
    text_rank_row *rows = text_rank(ranked, acount(ranked), query, flags);
    aclear(p->rows);
    aclear(p->places);
    int group = -1;
    for (int r = 0; r < acount(rows); r++) {
        int entry = rows[r].index;
        if (rows[r].group != group && p->entries[entry].group[0]) {
            apush(p->rows, ((studio_palette_row){ .entry = entry, .place = -1 }));
        }
        group = rows[r].group;
        apush(p->rows, ((studio_palette_row){ .entry = entry, .place = acount(p->places) }));
        apush(p->places, acount(p->rows) - 1);
    }
    afree(rows);
    afree(ranked);
    aclear(p->tops);
    snprintf(p->ranked, sizeof p->ranked, "%s", query);
}

/* The place of the option a list opening on a field highlights: its
   current value's, else the first. */
static int current_place(const studio_palette_state *p)
{
    for (int i = 0; i < acount(p->places); i++) {
        if (p->entries[p->rows[p->places[i]].entry].current) {
            return i;
        }
    }
    return 0;
}

/* Collects and ranks again what changed: true when the rows did. */
static bool refresh(studio *s, studio_palette_state *p)
{
    uint64_t inputs = inputs_of(s);
    p->collect |= inputs != p->inputs;
    if (!p->collect && !p->rank) {
        return false;
    }
    bool opening = p->collect && !acount(p->entries) && !p->query[0];
    if (p->collect) {
        collect(s, p);
        p->inputs = inputs;
    }
    rank(p);
    p->collect = p->rank = false;
    p->highlight = opening ? current_place(p) : 0;
    p->reveal = acount(p->places) ? p->places[p->highlight] : -1;
    p->top = !opening;
    return true;
}

/* ---------- The variables ---------- */

static const char *const ROW_TEXTS[] = { "label", "about", "meta",  "shortcut",
                                         "tip",   "icon",  "image", NULL };

static yyjson_mut_val *row_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *row = yyjson_mut_obj(doc);
    for (const char *const *text = ROW_TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, row, *text, "");
    }
    yyjson_mut_obj_add_bool(doc, row, "heading", false);
    yyjson_mut_obj_add_bool(doc, row, "current", false);
    yyjson_mut_obj_add_int(doc, row, "place", 0);
    return row;
}

void studio_palette_add_variables(yyjson_mut_doc *doc)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *palette = yyjson_mut_obj_add_obj(doc, root, "palette");
    yyjson_mut_obj_add_strcpy(doc, palette, "placeholder", "");
    yyjson_mut_obj_add_strcpy(doc, palette, "empty", "");
    yyjson_mut_obj_add_int(doc, palette, "count", 0);
    yyjson_mut_obj_add_arr(doc, palette, "rows");
    yyjson_mut_val *ui = yyjson_mut_obj_add_obj(doc, root, "palette_ui");
    yyjson_mut_obj_add_int(doc, ui, "highlight", 0);
}

/* The label as markup, the letters the query matched in bold; a space
   between two matched runs joins them, and a run of only spaces keeps its
   width, which RmlUi would drop between two elements. */
static void label_markup(const char *label, const char *query, char *out, size_t size)
{
    size_t count = strlen(label);
    bool hits[STUDIO_PALETTE_TEXT] = { false };
    assert(count < sizeof hits);
    if (text_search_match(query, label, hits, sizeof hits) < 0 || !query[0]) {
        memset(hits, 0, sizeof hits);
    }
    for (size_t i = 1; i + 1 < count; i++) {
        hits[i] |= label[i] == ' ' && hits[i - 1] && hits[i + 1];
    }
    size_t used = 0;
    out[0] = '\0';
    for (size_t start = 0, end; start < count; start = end) {
        char run[STUDIO_PALETTE_TEXT], escaped[STUDIO_PALETTE_TEXT * 8];
        for (end = start; end < count && hits[end] == hits[start]; end++) {
        }
        snprintf(run, sizeof run, "%.*s", (int)(end - start), label + start);
        if (strspn(run, " ") == strlen(run)) {
            escaped[0] = '\0';
            for (size_t i = 0; i < strlen(run); i++) {
                strcat(escaped, "&nbsp;");
            }
        } else {
            highlight_escape(run, escaped, sizeof escaped);
        }
        int wrote = snprintf(
            out + used,
            size - used,
            "%s%s%s",
            hits[start] ? "<b>" : "",
            escaped,
            hits[start] ? "</b>" : ""
        );
        used += wrote > 0 ? (size_t)wrote : 0;
        if (used >= size) {
            out[size - 1] = '\0';
            return;
        }
    }
}

static bool show_heading(yyjson_mut_doc *doc, yyjson_mut_val *row, const studio_palette_entry *e)
{
    bool changed = ui_data_put_bool(row, "heading", true);
    changed |= ui_data_put_text(doc, row, "label", e->group);
    changed |= ui_data_put_int(row, "place", 0);
    for (const char *const *text = ROW_TEXTS + 1; *text; text++) {
        changed |= ui_data_put_text(doc, row, *text, "");
    }
    return ui_data_put_bool(row, "current", false) || changed;
}

static bool show_entry(
    studio *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *row,
    const studio_palette_entry *e,
    int place
)
{
    const studio_palette_state *p = &s->palette;
    char label[STUDIO_PALETTE_TEXT * 16], icon[5] = "";
    /* A picker's rows are plain, as the kit's dropdowns were; a field's
       list marks what was typed, as the kit's autocompletes did. */
    label_markup(e->label, p->anchored && !p->field ? "" : p->ranked, label, sizeof label);
    if (e->icon) {
        studio_glyph(icon, e->icon);
    }
    const char *image = e->asset >= 0 ? studio_thumb_cached(s, e->asset) : NULL;
    bool changed = ui_data_put_bool(row, "heading", false);
    changed |= ui_data_put_text(doc, row, "label", label);
    changed |= ui_data_put_text(doc, row, "about", e->about);
    changed |= ui_data_put_text(doc, row, "meta", e->meta);
    changed |= ui_data_put_text(doc, row, "shortcut", e->shortcut);
    changed |= ui_data_put_text(doc, row, "tip", e->tip);
    changed |= ui_data_put_text(doc, row, "icon", icon);
    changed |= ui_data_put_text(doc, row, "image", image ? image : "");
    changed |= ui_data_put_int(row, "place", place + 1);
    return ui_data_put_bool(row, "current", e->current) || changed;
}

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

/* The List, found once while it lives. */
static rml_element *list_of(studio *s, studio_palette_state *p)
{
    rml_element *list = p->list ? rml_element_ref_get(p->list) : NULL;
    if (!list && (list = by_id(s, LIST_ID))) {
        rml_element_ref_destroy(p->list);
        p->list = rml_element_ref_create(list);
    }
    return list;
}

static float dp_of(rml_element *el, const char *property)
{
    char value[32];
    rml_element_property(el, property, value, sizeof value);
    float dp = strtof(value, NULL);
    assert(dp > 0);
    return dp;
}

/* Each row's top: headings, results and results with a second line take
   the heights the List's --row-heading, --row and --row-about give. They
   are placed again only when the rows or the heights change. Returns the
   shortest. */
static float place_rows(studio_palette_state *p, rml_element *list)
{
    float heading = dp_of(list, "--row-heading"), row = dp_of(list, "--row"),
          about = dp_of(list, "--row-about"), top = 0;
    float shortest = fminf(heading, fminf(row, about));
    bool same = heading == p->heights[0] && row == p->heights[1] &&
                about == p->heights[2];
    if (same && acount(p->tops) == acount(p->rows) + 1) {
        return shortest;
    }
    p->heights[0] = heading;
    p->heights[1] = row;
    p->heights[2] = about;
    aclear(p->tops);
    for (int r = 0; r < acount(p->rows); r++) {
        apush(p->tops, top);
        const studio_palette_row *at = &p->rows[r];
        top += at->place < 0                  ? heading
             : p->entries[at->entry].about[0] ? about
                                              : row;
    }
    apush(p->tops, top);
    return shortest;
}

/* Gives `rows` `length` items, adding and removing at its end. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *rows, int length)
{
    int had = (int)yyjson_mut_arr_size(rows);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(rows, row_new(doc));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(rows);
    }
    return had != length;
}

/* The rows of the List in view, as ui_list_window places them. */
static bool show_rows(studio *s, const studio_panel_sync *at, yyjson_mut_val *array)
{
    studio_palette_state *p = &s->palette;
    int count = acount(p->rows), first = 0;
    int length = count < UNLAID_ROWS ? count : UNLAID_ROWS;
    rml_element *list = list_of(s, p);
    if (list) {
        if (p->top) {
            rml_element_set_scroll_top(list, 0);
            p->top = false;
        }
        float shortest = place_rows(p, list);
        ui_list window = { .count = count,
                           .row = shortest,
                           .tops = p->tops,
                           .reveal = p->reveal };
        length = ui_list_window(list, &window, &p->window, &first);
        p->reveal = -1;
    }
    bool changed = fit(at->doc, array, length);
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int r = first; (item = yyjson_mut_arr_iter_next(&it)); r++) {
        const studio_palette_row *row = &p->rows[r];
        const studio_palette_entry *e = &p->entries[row->entry];
        changed |= row->place < 0 ? show_heading(at->doc, item, e)
                                  : show_entry(s, at->doc, item, e, row->place);
    }
    return changed;
}

bool studio_palette_sync(studio *s, const studio_panel_sync *at)
{
    studio_palette_state *p = &s->palette;
    yyjson_mut_val *rows = yyjson_mut_obj_get(at->variable, "rows");
    if (at->preview || !p->client) {
        return fit(at->doc, rows, 0);
    }
    refresh(s, p);
    char empty[320] = "No matching options";
    if (!p->anchored) {
        snprintf(empty, sizeof empty, "No results for \"%s\"", p->query);
    }
    bool changed = ui_data_put_text(at->doc, at->variable, "placeholder", p->placeholder);
    changed |= ui_data_put_text(at->doc, at->variable, "empty", empty);
    changed |= ui_data_put_int(at->variable, "count", acount(p->places));
    changed |= show_rows(s, at, rows);
    yyjson_mut_val *ui = yyjson_mut_obj_get(yyjson_mut_doc_get_root(at->doc), "palette_ui");
    if (ui_data_put_int(ui, "highlight", p->highlight)) {
        rml_data_model_dirty(at->model, "palette_ui");
    }
    return changed;
}

/* ---------- Intents ---------- */

/* emit('palette.pick', kind, value, none, typed) on an AssetField opens its
   picker of the kind's assets, which sends the field change with the name
   chosen; `none` labels choosing none ("" when a name is required) and a
   `typed` one offers a name no asset has yet. Pressing the field again
   closes it, as a select does. */
static void pick_field(studio *s, const ui_activation *a)
{
    rml_element *field = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    int kind = REFERENCE_ASSET_KIND_COUNT;
    for (int k = 0; a->argc >= 2 && k < REFERENCE_ASSET_KIND_COUNT; k++) {
        kind = !strcmp(studio_asset_kind_name(k), a->args[0]) ? k : kind;
    }
    if (!field || kind == REFERENCE_ASSET_KIND_COUNT) {
        return;
    }
    if (s->palette.client && studio_palette_anchor(s) == field) {
        studio_palette_close(s);
        return;
    }
    studio_pick_desc desc = { .kind = kind,
                              .none = a->argc > 2 ? a->args[2] : NULL,
                              .text = a->argc > 3 && a->args[3][0] };
    studio_pick_open(s, STUDIO_PICK_FIELD, a->args[1], &desc, field);
}

/* query: the search field's text as typed; highlight: the place a key or,
   with a second argument, the pointer moved to, which scrolls nothing
   (cmdk); choose: the place chosen, from 1; scroll: the List moved; close:
   Escape or a press outside; pick: an AssetField's picker. */
void studio_palette_action(studio *s, const char *action, const ui_activation *a)
{
    studio_palette_state *p = &s->palette;
    int place = a->has_value ? (int)a->value : -1;
    if (!strcmp(action, "pick")) {
        pick_field(s, a);
        return;
    }
    if (!p->client) {
        return;
    }
    if (!strcmp(action, "query") && a->argc) {
        char was[sizeof p->query];
        snprintf(was, sizeof was, "%s", p->query);
        snprintf(p->query, sizeof p->query, "%s", a->args[0]);
        p->collect |= lists_anew(p, was);
        p->rank = strcmp(was, p->query) != 0;
    } else if (!strcmp(action, "highlight") && place >= 0 && place < acount(p->places)) {
        bool pointer = a->argc > 1 && !strcmp(a->args[1], "1");
        p->highlight = place;
        p->reveal = pointer ? -1 : p->places[place];
    } else if (!strcmp(action, "choose") && place >= 1) {
        choose(s, place - 1);
    } else if (!strcmp(action, "close")) {
        studio_palette_close(s);
    }
}

/* ---------- Documents ---------- */

/* The documents, mounted while their kind of list is open. */
static const char *const PANELS[] = {
    [STUDIO_PALETTE_MODAL] = "Palette",
    [STUDIO_PALETTE_PICKER] = "Picker",
};

/* A Studio project without the document, such as a served copy older than
   the build, would leave the palette open, unseen and holding the keys. */
static void missing(studio *s, const char *document)
{
    char text[96];
    snprintf(text, sizeof text, "The Studio project has no %s document; update its copy", document);
    kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_ERROR,
                           .title = "Palette unavailable",
                           .description = text }
    );
    studio_record(s, KIT_TOAST_ERROR, text);
    studio_palette_close(s);
}

/* The modal keeps the focus in its search field, as cmdk's dialog does,
   and shows a query its client set. */
static void modal_frame(studio *s, studio_palette_state *p)
{
    rml_element *host = studio_ui_dialog(s, "palette", PANELS[STUDIO_PALETTE_MODAL]);
    if (!host) {
        missing(s, PANELS[STUDIO_PALETTE_MODAL]);
        return;
    }
    rml_element *search = rml_element_query_selector(host, "#" SEARCH_ID);
    if (!search) {
        return;
    }
    if (p->typed) {
        rml_element_set_value(search, p->query);
        int end = (int)strlen(p->query);
        rml_element_set_selection_range(search, end, end);
        p->typed = false;
    }
    rml_context *context = rml_backend_context(s->backend);
    if (rml_context_focus_element(context) != search) {
        rml_element_focus(search, true);
    }
}

/* A picker hangs under its field, in a document of its own as tall as
   nothing, until its popover closes. */
static void picker_frame(studio *s, studio_palette_state *p)
{
    kit_document_begin(s->kit, "picker", "bar-place", true);
    rml_element *host = studio_ui_panel(s, "picker", PANELS[STUDIO_PALETTE_PICKER]);
    kit_document_end(s->kit);
    if (!host) {
        missing(s, PANELS[STUDIO_PALETTE_PICKER]);
        return;
    }
    rml_element *popover = rml_element_query_selector(host, "#" PICKER_ID);
    rml_element *anchor = studio_palette_anchor(s);
    if (!popover || p->shown) {
        if (popover && !popover_open(popover)) {
            studio_palette_close(s);
        }
        return;
    }
    if (!anchor && !p->boxed) {
        studio_palette_close(s);
        return;
    }
    popover_anchor at = { .side = POPOVER_BELOW, .element = anchor, .trigger = anchor };
    memcpy(at.box, p->box, sizeof at.box);
    popover_show(popover, &at);
    p->shown = true;
}

void studio_palette(studio *s)
{
    studio_palette_state *p = &s->palette;
    studio_graph_palette(s);
    studio_palette_mount want = !p->client || p->field ? STUDIO_PALETTE_UNMOUNTED
                              : p->anchored ? STUDIO_PALETTE_PICKER
                                            : STUDIO_PALETTE_MODAL;
    /* A host the kit removes lets its mount go first. */
    if (p->mounted && p->mounted != want) {
        studio_ui_panel_detach(s, PANELS[p->mounted]);
    }
    p->mounted = want;
    if (want == STUDIO_PALETTE_PICKER) {
        picker_frame(s, p);
    } else if (want == STUDIO_PALETTE_MODAL) {
        modal_frame(s, p);
    }
}

void studio_palette_free(studio *s)
{
    studio_palette_state *p = &s->palette;
    rml_element_ref_destroy(p->anchor);
    rml_element_ref_destroy(p->list);
    afree(p->entries);
    afree(p->rows);
    afree(p->places);
    afree(p->tops);
    *p = (studio_palette_state){ 0 };
}
