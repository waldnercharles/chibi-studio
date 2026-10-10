/* The kit's searchable lists: dropdowns anchored to their trigger and
 * autocompletes under a field, ranked by the search Studio's palette, its
 * pickers share (support/text_rank.h), groups ordered by their
 * best match. The command palette itself is authored (Studio's
 * Palette.rml). */
#include "editor/rmlui/kit_internal.h"
#include "editor/ui/kit_icons.h"
#include "support/math.h"
#include "support/text_rank.h"
#include "support/text_search.h"
#include "support/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Matches the shared vertical scrollbar so long rows keep their full label. */
#define LIST_SCROLLBAR_WIDTH 8

/* The rows to show for `query`, in order: every match, groups kept
   together. */
static text_rank_row *rank_items(const kit_command_item *items, int count, const char *query)
{
    text_rank_entry *entries = NULL;
    for (int i = 0; i < count; i++) {
        apush(
            entries,
            ((
                text_rank_entry
            ){ items[i].label, items[i].keywords, items[i].group, items[i].boost })
        );
    }
    text_rank_row *rows = text_rank(entries, count, query, 0);
    afree(entries);
    return rows;
}

/* The row to highlight once nothing is: the current item's, else the
   first. */
static int current_row(const kit_command_item *items, const text_rank_row *rows)
{
    for (int i = 0; i < acount(rows); i++) {
        if (items[rows[i].index].current) {
            return i;
        }
    }
    return 0;
}

int dropdown_items(kit *k, const char *key, const kit_command_item *items, int count)
{
    popup *p = popup_find(k, key);
    if (!p || !p->open || !p->dropdown || !kit_popup_begin(k, key, "dropdown")) {
        return -1;
    }

    bool changed = kit_search(k, "search", p->dropdown_query, sizeof p->dropdown_query, p->dropdown_placeholder);
    p->dropdown_search = k->last->w.input_el;
    bool opening = p->dropdown_focus;
    if (p->dropdown_focus) {
        p->dropdown_focus = false;
        p->keyboard = true;
        kit_focus(k);
    }
    if (changed) {
        p->highlight = -1;
    }

    text_rank_row *rows = rank_items(items, count, p->dropdown_query);
    if (changed) {
        p->highlight = acount(rows) ? 0 : -1;
    } else if (p->keyboard && p->highlight < 0 && acount(rows)) {
        p->highlight = current_row(items, rows);
    }
    p->dropdown_reveal |= opening || changed;
    kit_push(k, "results", NULL, "dropdown-results");
    if (changed) {
        rml_element_set_scroll_top(k->last->el, 0);
    }
    const char *group = NULL;
    int chosen = -1;
    if (!acount(rows)) {
        kit_label(k, "empty", "dropdown-empty", "No matching options");
    }
    for (int i = 0; i < acount(rows); i++) {
        int index = rows[i].index;
        const kit_command_item *item = &items[index];
        if (item->group && item->group[0] && !str_eq(group, item->group)) {
            char group_key[32];
            snprintf(group_key, sizeof group_key, "group-%d", i);
            kit_menu_group(k, group_key, item->group);
            group = item->group;
        }
        char row_key[32];
        snprintf(row_key, sizeof row_key, "option-%d", index);
        kit_menu_item_desc d = {
            .label = item->label,
            .icon = item->icon,
            .checked = -1,
            .current = item->current,
            .description = item->description,
        };
        if (kit_menu_item(k, row_key, &d)) {
            chosen = index;
        }
    }
    p->content = math_max(p->content + LIST_SCROLLBAR_WIDTH, p->anchor.w - 10);
    kit_pop(k);
    kit_popup_end(k);
    afree(rows);
    return chosen;
}

static node *autocomplete_field(const kit *k, const char *key)
{
    if (!acount(k->stack)) {
        return NULL;
    }
    node *parent = alast(k->stack);
    for (int i = 0; i < acount(parent->children); i++) {
        if (str_eq(parent->children[i]->key, key)) {
            return parent->children[i];
        }
    }
    return NULL;
}

static char *autocomplete_popup_key(const node *field)
{
    char *key = NULL;
    str_catf(&key, "autocomplete:%s", field->path);
    return key;
}

/* The label with the letters the query's ranking matched in bold
   (text_search_match), consecutive ones in one <b>; a run of only spaces
   keeps its width as no-break spaces, which RmlUi would drop. */
static char *match_rml(const char *label, const char *query)
{
    size_t count = strlen(label);
    bool *hits = xcalloc(count + 1, sizeof *hits);
    if (text_search_match(query, label, hits, count + 1) < 0) {
        memset(hits, 0, (count + 1) * sizeof *hits);
    }
    /* A space between two matches joins them in one run. */
    for (size_t i = 1; i + 1 < count; i++) {
        hits[i] |= label[i] == ' ' && hits[i - 1] && hits[i + 1];
    }
    char *rml = NULL;
    for (size_t start = 0, end; start < count; start = end) {
        bool blank = true;
        for (end = start; end < count && hits[end] == hits[start]; end++) {
            blank &= label[end] == ' ';
        }
        str_cat(&rml, hits[start] ? "<b>" : "");
        for (size_t i = start; i < end; i++) {
            if (blank) {
                str_cat(&rml, "\xc2\xa0");
                continue;
            }
            escape(&rml, (char[]){ label[i], 0 });
        }
        str_cat(&rml, hits[start] ? "</b>" : "");
    }
    free(hits);
    return rml;
}

/* Completion uses the text editor's compact name/type row while retaining the
   menu primitive's focus, hover, scrolling and activation behavior. */
static void completion_row(kit *k, popup *p, const kit_command_item *item, const char *query)
{
    widget *w = &k->last->w;
    const char *member = strrchr(query, '.');
    const char *label = item->label ? item->label : "";
    const char *match = member && text_search_score(query, label, NULL) < 0
                          ? member + 1
                          : query;
    char *rml = match_rml(label, match);
    if (str_set(&w->applied, rml ? rml : "")) {
        rml_element_set_inner_rml(w->label_el, w->applied);
    }
    free(rml);
    if (!w->meta_el) {
        w->meta_el = raw(k->last->el, "span", "detail", NULL);
    }
    set_element_text(w->meta_el, item->meta);
    flag_element(w->meta_el, "data-empty", !item->meta || !item->meta[0]);
    rml_element_set_attribute(w->icon_el, "aria-hidden", "true");
    kit_tip(k, item->label, NULL, item->description);

    float ratio = rml_context_dp_ratio(k->context);
    uintptr_t face = rml_font_face("jetbrains mono", RML_FONT_NORMAL, 400, (int)(12 * ratio));
    float label_width = face ? rml_font_string_width(face, label) / ratio : 0;
    float detail_width = item->meta && item->meta[0]
                           ? 16 + kit_text_width(k, item->meta, 12, 400, ratio)
                           : 0;
    p->content = math_max(p->content, 6 + 16 + 8 + label_width + detail_width + 6);
}

const char *kit_autocomplete_query(const kit *k, const char *key)
{
    node *field = autocomplete_field(k, key);
    if (!field || field->kind != KIND_AUTOCOMPLETE) {
        return NULL;
    }
    char *popup_key = autocomplete_popup_key(field);
    popup *p = popup_find(k, popup_key);
    free(popup_key);
    if (!p || !p->open) {
        return NULL;
    }
    return field->w.autocomplete_query_active ? field->w.draft : "";
}

kit_autocomplete_result kit_autocomplete(
    kit *k,
    const char *key,
    const char *value,
    const kit_command_item *items,
    int count,
    const char *placeholder,
    unsigned flags
)
{
    node *field = emit_input(k, key, KIND_AUTOCOMPLETE, "field autocomplete", placeholder);
    widget *w = &field->w;
    w->desc.flags = flags;
    set_flag(field, "data-readonly", flags & KIT_READONLY);
    set_flag(field, "data-disabled", flags & KIT_DISABLED);
    if (flags & (KIT_READONLY | KIT_DISABLED)) {
        rml_element_set_attribute(w->input_el, "readonly", "");
        w->autocomplete_query_active = false;
        w->autocomplete_sync = true;
    } else {
        rml_element_remove_attribute(w->input_el, "readonly");
    }
    const char *shown = value ? value : "";
    if (w->autocomplete_sync || !w->focused || !str_eq(w->applied, shown)) {
        str_set(&w->applied, shown);
        if (!w->focused || !w->autocomplete_query_active) {
            if (str_set(&w->draft, shown)) {
                rml_element_set_value(w->input_el, shown);
            }
            w->autocomplete_query_active = false;
        }
        w->autocomplete_sync = false;
    }
    rml_element_set_attribute(w->input_el, "role", "combobox");
    rml_element_set_attribute(w->input_el, "aria-autocomplete", "list");

    char *popup_key = autocomplete_popup_key(field);
    popup *p = popup_find(k, popup_key);
    if ((flags & (KIT_READONLY | KIT_DISABLED)) && p && p->open) {
        close_popup(k, popup_key);
    }
    p = popup_find(k, popup_key);
    bool open = p && p->open && p->autocomplete;
    set_flag(field, "data-open", open);
    rml_element_set_attribute(w->input_el, "aria-expanded", open ? "true" : "false");
    if (!open) {
        rml_element_remove_attribute(w->input_el, "aria-controls");
        rml_element_remove_attribute(w->input_el, "aria-activedescendant");
    }

    kit_autocomplete_result result = { .selected = -1 };
    result.canceled = w->autocomplete_canceled;
    w->autocomplete_canceled = false;
    result.submitted = w->autocomplete_submitted;
    w->autocomplete_submitted = false;
    if (open && kit_popup_begin(k, popup_key, "autocomplete")) {
        kit_attr(k, "id", k->last->path);
        const char *query = w->autocomplete_query_active ? w->draft : "";
        bool changed = !str_eq(p->dropdown_query, query);
        bool opening = p->autocomplete_opening;
        p->autocomplete_opening = false;
        if (changed) {
            snprintf(p->dropdown_query, sizeof p->dropdown_query, "%s", query);
            p->highlight = -1;
        }
        text_rank_row *rows = rank_items(items, count, query);
        if (changed) {
            p->highlight = acount(rows) ? 0 : -1;
        } else if ((opening || p->highlight < 0) && acount(rows)) {
            p->highlight = current_row(items, rows);
        }
        p->dropdown_reveal |= opening || changed;
        kit_push(k, "results", NULL, "autocomplete-results");
        if (changed) {
            rml_element_set_scroll_top(k->last->el, 0);
        }
        kit_attr(k, "id", k->last->path);
        kit_attr(k, "role", "listbox");
        rml_element_set_attribute(w->input_el, "aria-controls", k->last->path);
        rml_element_remove_attribute(w->input_el, "aria-activedescendant");
        const char *group = NULL;
        for (int i = 0; i < acount(rows); i++) {
            int index = rows[i].index;
            const kit_command_item *item = &items[index];
            if (item->group && item->group[0] && !str_eq(group, item->group)) {
                char group_key[32];
                snprintf(group_key, sizeof group_key, "group-%d", i);
                kit_menu_group(k, group_key, item->group);
                group = item->group;
            }
            char row_key[32];
            snprintf(row_key, sizeof row_key, "option-%d", index);
            kit_menu_item_desc desc = { .label = item->label,
                                        .icon = item->icon,
                                        .checked = -1,
                                        .current = item->current };
            if (kit_menu_item(k, row_key, &desc)) {
                result.selected = index;
            }
            completion_row(k, p, item, query);
            kit_attr(k, "id", k->last->path);
            kit_attr(k, "role", "option");
            kit_attr(k, "aria-selected", p->highlight == i ? "true" : "false");
            if (p->highlight == i) {
                rml_element_set_attribute(
                    w->input_el,
                    "aria-activedescendant",
                    k->last->path
                );
            }
        }
        if (!acount(rows)) {
            kit_label(k, "empty", "dropdown-empty", "No matching options");
        }
        kit_pop(k);
        p->content = math_max(p->content + LIST_SCROLLBAR_WIDTH, p->anchor.w - 10);
        kit_popup_end(k);
        afree(rows);
    }
    if (result.selected >= 0) {
        close_popup(k, popup_key);
        w->autocomplete_sync = true;
        w->autocomplete_query_active = false;
    }
    k->last = field;
    free(popup_key);
    return result;
}

void kit_autocomplete_close(kit *k, const char *key)
{
    node *field = autocomplete_field(k, key);
    if (!field || field->kind != KIND_AUTOCOMPLETE) {
        return;
    }
    char *popup_key = autocomplete_popup_key(field);
    close_popup(k, popup_key);
    free(popup_key);
    field->w.autocomplete_sync = true;
    field->w.autocomplete_query_active = false;
    /* The choice is made: the field keeps the focus with its list shut. */
    field->w.autocomplete_refocus = true;
    rml_element_focus(field->w.input_el, true);
    field->w.autocomplete_refocus = false;
}
