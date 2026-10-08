/* The graph canvas's context menus: a node's, a pin's, a wire's and a comment
 * frame's (projects/studio's GraphMenu.rml, the `graph_menu` source), after
 * Unreal's graph context menus. A menu is written once, as the kit's
 * immediate menus were: described, each item adds an entry to the variable
 * and returns false; run for the key a click sends, only that item returns
 * true and its code acts. Its document mounts while the menu is open.
 *
 * graph_menu: { group: { label }, items: [ENTRY] }
 * ENTRY: { key, label, icon, shortcut, swatch, checked, disabled, current,
 *          sep, sub, items: [ENTRY] } */
#include "editor/studio/studio_graph_internal.h"
#include "support/array.h"
#include "ui/popover.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Ids in GraphMenu.rml. */
#define MENU_ID "graph-menu"

typedef struct sg_menu_entry
{
    char key[64], label[272], shortcut[32];
    uint32_t icon, swatch; /* a swatch tints a dot instead of an icon */
    bool disabled, current;
    bool sep;   /* a separator above it */
    bool sub;   /* it opens the entries whose parent it is */
    int parent; /* its submenu's entry, or -1 */
} sg_menu_entry;

struct sg_menu
{
    const char *run; /* the key to run, or NULL while describing */
    sg_menu_entry *entries;
    char title[256];
    int parent; /* the submenu being written, or -1 */
    bool sep;   /* a separator goes above the next entry */
};

/* ---------- Writing a menu ---------- */

static void key_path(const sg_menu *m, const char *key, char out[64])
{
    if (m->parent < 0) {
        snprintf(out, 64, "%s", key);
        return;
    }
    snprintf(out, 64, "%s/%s", m->entries[m->parent].key, key);
}

static sg_menu_entry *add(sg_menu *m, const char *key, const char *label)
{
    sg_menu_entry entry = { .parent = m->parent, .sep = m->sep };
    key_path(m, key, entry.key);
    snprintf(entry.label, sizeof entry.label, "%s", label);
    m->sep = false;
    apush(m->entries, entry);
    return &alast(m->entries);
}

static bool chosen(const sg_menu *m, const char *key, bool disabled)
{
    char path[64];
    key_path(m, key, path);
    return !disabled && !strcmp(path, m->run);
}

bool sg_menu_item(sg_menu *m, const char *key, const char *label, uint32_t icon, const char *shortcut, bool disabled)
{
    if (m->run) {
        return chosen(m, key, disabled);
    }
    sg_menu_entry *e = add(m, key, label);
    e->icon = icon;
    e->disabled = disabled;
    snprintf(e->shortcut, sizeof e->shortcut, "%s", shortcut ? shortcut : "");
    return false;
}

bool sg_menu_swatch(sg_menu *m, const char *key, const char *label, uint32_t swatch, bool current, bool disabled)
{
    if (m->run) {
        return chosen(m, key, disabled);
    }
    sg_menu_entry *e = add(m, key, label);
    e->icon = KIT_ICON_DOT;
    e->swatch = swatch;
    e->current = current;
    e->disabled = disabled;
    return false;
}

void sg_menu_separator(sg_menu *m)
{
    m->sep = true;
}

void sg_menu_title(sg_menu *m, const char *title)
{
    snprintf(m->title, sizeof m->title, "%s", title);
}

void sg_submenu_begin(sg_menu *m, const char *key, const char *label, uint32_t icon)
{
    assert(m->parent < 0);
    if (m->run) {
        char path[64];
        key_path(m, key, path);
        sg_menu_entry entry = { .parent = -1 };
        snprintf(entry.key, sizeof entry.key, "%s", path);
        apush(m->entries, entry);
    } else {
        sg_menu_entry *e = add(m, key, label);
        e->icon = icon;
        e->sub = true;
    }
    m->parent = acount(m->entries) - 1;
}

void sg_submenu_end(sg_menu *m)
{
    m->parent = -1;
    m->sep = false;
}

/* ---------- The menus ---------- */

/* The wire's menu. */
static void wire_menu(studio *s, sg_menu *m, int index)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    bool readonly = sg_graph_readonly(s);
    edge *ed = &gr->edges[index];
    if (ed->reroute_count &&
        sg_menu_item(m, "reroutes", "Clear Reroutes", KIT_ICON_RESET, NULL, readonly)) {
        ed_snapshot_before(e);
        ed->reroute_count = 0;
        return;
    }
    if (sg_menu_item(m, "straighten", "Straighten Connection", KIT_ICON_MOVE, "Q", readonly)) {
        sg_straighten_wire(s, index);
        return;
    }
    if (sg_menu_item(m, "delete", "Delete Wire", KIT_ICON_TRASH, studio_chord(s, "Alt+Click"), readonly)) {
        ed_link_ref link;
        sg_link_ref_of(ed, &link);
        ed_clear_selection(e);
        ed_add_selected_link(e, &link);
        ed_delete_selected(e);
    }
}

/* The menu of what the right click opened it on: a pin, a node, a comment
   frame or a wire. */
static void write_menu(studio *s, sg_menu *m)
{
    studio_graph *g = &s->graph;
    graph *gr = sg_active_graph(s);
    node *n = sg_node_by_id(s, g->menu_node);
    graph_comment_box *box = sg_comment_by_id(s, g->menu_comment);
    int output = 0, pin = -1;
    if (n && graph_pin_by_id(n, g->menu_pin, &output, &pin)) {
        sg_pin_menu(s, m, n, output ? ED_PIN_OUTPUT : ED_PIN_INPUT, pin);
    } else if (n) {
        sg_node_menu(s, m, n);
    } else if (box) {
        sg_comment_menu(s, m, box);
    } else if (g->menu_edge >= 0 && g->menu_edge < acount(gr->edges)) {
        wire_menu(s, m, g->menu_edge);
    }
}

/* ---------- The variable ---------- */

static const char *const TEXTS[] = { "key",      "label",  "icon",
                                     "shortcut", "swatch", NULL };
static const char *const FLAGS[] = { "checked", "disabled", "current",
                                     "sep",     "sub",      NULL };

static yyjson_mut_val *entry_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *entry = yyjson_mut_obj(doc);
    for (const char *const *text = TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, entry, *text, "");
    }
    for (const char *const *flag = FLAGS; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, entry, *flag, false);
    }
    yyjson_mut_obj_add_arr(doc, entry, "items");
    return entry;
}

void studio_graph_menu_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *menu = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "graph_menu");
    yyjson_mut_val *group = yyjson_mut_obj_add_obj(doc, menu, "group");
    yyjson_mut_obj_add_strcpy(doc, group, "label", "");
    yyjson_mut_obj_add_arr(doc, menu, "items");
}

/* Gives `array` `count` entries, adding and removing at its end. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *array, int count)
{
    int had = (int)yyjson_mut_arr_size(array);
    for (int i = had; i < count; i++) {
        yyjson_mut_arr_append(array, entry_new(doc));
    }
    for (int i = had; i > count; i--) {
        yyjson_mut_arr_remove_last(array);
    }
    return had != count;
}

static bool put_entry(yyjson_mut_doc *doc, yyjson_mut_val *item, const sg_menu_entry *e)
{
    char icon[5] = "", swatch[16] = "";
    if (e->icon) {
        studio_glyph(icon, e->icon);
    }
    if (e->swatch) {
        snprintf(swatch, sizeof swatch, "#%08x", e->swatch);
    }
    bool changed = ui_data_put_text(doc, item, "key", e->key);
    changed |= ui_data_put_text(doc, item, "label", e->label);
    changed |= ui_data_put_text(doc, item, "icon", icon);
    changed |= ui_data_put_text(doc, item, "shortcut", e->shortcut);
    changed |= ui_data_put_text(doc, item, "swatch", swatch);
    changed |= ui_data_put_bool(item, "disabled", e->disabled);
    changed |= ui_data_put_bool(item, "current", e->current);
    changed |= ui_data_put_bool(item, "sep", e->sep);
    return ui_data_put_bool(item, "sub", e->sub) || changed;
}

/* Writes the entries whose parent is `parent` into `array`. */
static bool put_entries(yyjson_mut_doc *doc, yyjson_mut_val *array, const sg_menu *m, int parent)
{
    int count = 0;
    for (int i = 0; i < acount(m->entries); i++) {
        count += m->entries[i].parent == parent;
    }
    bool changed = fit(doc, array, count);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = 0; i < acount(m->entries); i++) {
        if (m->entries[i].parent != parent) {
            continue;
        }
        yyjson_mut_val *item = yyjson_mut_arr_iter_next(&it);
        changed |= put_entry(doc, item, &m->entries[i]);
        if (parent < 0) {
            changed |= put_entries(doc, yyjson_mut_obj_get(item, "items"), m, i);
        }
    }
    return changed;
}

/* The open menu's entries, written again each refresh it shows, as the kit
   emitted its items each frame: what an item allows follows the graph. */
bool studio_graph_menu_sync(studio *s, const studio_panel_sync *at)
{
    if (!s->graph.menu_open || at->preview || !studio_graph_active(s)) {
        return false;
    }
    sg_menu m = { .parent = -1 };
    write_menu(s, &m);
    yyjson_mut_val *group = yyjson_mut_obj_get(at->variable, "group");
    bool changed = ui_data_put_text(at->doc, group, "label", m.title);
    changed |= put_entries(at->doc, yyjson_mut_obj_get(at->variable, "items"), &m, -1);
    afree(m.entries);
    return changed;
}

/* run: the item a click chose, by key. An action may free the node, its
   graph or the whole asset, so only the one runs; the click closes the
   menu. */
void studio_graph_menu_action(studio *s, const char *action, const ui_activation *a)
{
    if (strcmp(action, "run") || !a->argc || !s->graph.menu_open ||
        !studio_graph_active(s)) {
        return;
    }
    sg_menu m = { .run = a->args[0], .parent = -1 };
    write_menu(s, &m);
    afree(m.entries);
}

/* ---------- The document ---------- */

void studio_graph_menu_frame(studio *s)
{
    studio_graph *g = &s->graph;
    if (g->menu_request) {
        g->menu_open = g->menu_opening = true;
        g->menu_request = false;
    }
    g->menu_open &= studio_graph_active(s);
    /* Its document lives while it shows; a host the kit removes lets its
       mount go first. */
    if (!g->menu_open) {
        if (g->menu_mounted) {
            studio_ui_panel_detach(s, "GraphMenu");
            g->menu_mounted = false;
        }
        return;
    }
    kit_document_begin(s->kit, "graph-menu", "bar-place", true);
    rml_element *host = studio_ui_panel(s, "graph-menu", "GraphMenu");
    kit_document_end(s->kit);
    g->menu_mounted = true;
    rml_element *popover = host ? rml_element_query_selector(host, "#" MENU_ID) : NULL;
    if (popover && g->menu_opening) {
        /* A press on the canvas leaves a field it took the focus from, so
           the menu, which keeps the focus where it is, hears the keys. */
        rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
        if (focus) {
            rml_element_blur(focus);
        }
        float ratio = studio_pixel_ratio(s);
        popover_show(
            popover,
            &(popover_anchor){ .side = POPOVER_POINT,
                               .box = { g->menu_x * ratio, g->menu_y * ratio, 0, 0 } }
        );
        g->menu_opening = false;
    } else if (popover && !popover_open(popover)) {
        g->menu_open = false;
    }
}
