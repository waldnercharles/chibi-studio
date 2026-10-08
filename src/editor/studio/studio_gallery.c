/* The widget gallery's List, TreeRow and Tile samples (Widgets.rml), the
 * `gallery` panel source: the project's assets as a List whose window this
 * fills (ui_list_window), dragged as the Assets area drags them, and
 * Collected, which takes assets dropped from either list before or after
 * its rows and shows them as Tiles too. A drag from it lands on the native
 * Outliner and Properties, and one from native Assets lands on it, across
 * area documents (docs/studio-rml-migration.md, F4). */
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_thumbnails.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The List's id in Widgets.rml. */
#define GALLERY_LIST "gallery-assets"
/* Rows shown before the List is laid out. */
#define UNLAID_ROWS 40

static const char *const TEXTS[] = { "label", "icon",  "meta", "drag",
                                     "name",  "image", "badge" };
static const char *const FLAGS[] = { "parent",  "open",      "selected",
                                     "active",  "marked",    "hidden",
                                     "loading", "unresolved" };

/* A row's members, for TreeRow and Tile both. */
static yyjson_mut_val *item_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    for (size_t i = 0; i < sizeof TEXTS / sizeof *TEXTS; i++) {
        yyjson_mut_obj_add_strcpy(doc, item, TEXTS[i], "");
    }
    for (size_t i = 0; i < sizeof FLAGS / sizeof *FLAGS; i++) {
        yyjson_mut_obj_add_bool(doc, item, FLAGS[i], false);
    }
    yyjson_mut_obj_add_int(doc, item, "depth", 0);
    return item;
}

void studio_gallery_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *gallery = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "gallery");
    yyjson_mut_obj_add_arr(doc, gallery, "assets");
    yyjson_mut_obj_add_arr(doc, gallery, "bin");
}

/* ---------- Values, written in place ---------- */

/* Gives the array `length` rows, adding and removing at its end, so rows
 * already shown keep their elements; true when its length changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *rows, int length)
{
    int had = (int)yyjson_mut_arr_size(rows);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(rows, item_new(doc));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(rows);
    }
    return had != length;
}

/* An icon's codepoint as UTF-8; Symbols' glyphs are past U+0800. */
static void glyph(char out[5], uint32_t cp)
{
    assert(cp >= 0x800 && cp < 0x110000);
    bool wide = cp >= 0x10000;
    int n = 0;
    if (wide) {
        out[n++] = (char)(0xf0 | (cp >> 18));
    }
    out[n++] = (char)((wide ? 0x80 : 0xe0) | ((cp >> 12) & (wide ? 0x3f : 0x0f)));
    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[n++] = (char)(0x80 | (cp & 0x3f));
    out[n] = '\0';
}

/* The icon Assets shows for the asset. */
static uint32_t asset_icon(studio *s, const reference_asset *asset)
{
    if (asset->unresolved) {
        return KIT_ICON_WARNING;
    }
    return asset->kind == REFERENCE_ASSET_PROTOTYPE
             ? studio_prototype_icon(s, asset->name)
             : studio_asset_kind_icon(asset->kind);
}

/* Shows asset `id` in a row: its name, kind and icon, and the drag Assets
 * gives it. */
static bool show_asset(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *item, int id)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    const reference_asset *asset = reference_index_asset_at(index, id);
    const char *kind = studio_asset_kind_name(asset->kind);
    char icon[5];
    glyph(icon, asset_icon(s, asset));
    bool changed = ui_data_put_text(doc, item, "label", asset->name);
    changed |= ui_data_put_text(doc, item, "name", asset->name);
    changed |= ui_data_put_text(doc, item, "icon", icon);
    changed |= ui_data_put_text(doc, item, "meta", kind);
    changed |= ui_data_put_text(doc, item, "drag", studio_asset_draggable(asset) ? kind : "");
    changed |= ui_data_put_bool(item, "unresolved", asset->unresolved);
    return changed;
}

/* The element with `id` in Studio's documents, or NULL. */
static rml_element *studio_by_id(studio *s, const char *id)
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

/* The rows of the List in view, as ui_list_window places them. */
static bool show_assets(studio *s, const studio_panel_sync *at, yyjson_mut_val *rows)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int count = index ? reference_index_asset_count(index) : 0;
    rml_element *list = at->preview ? NULL : studio_by_id(s, GALLERY_LIST);
    int first = 0, length = count < UNLAID_ROWS ? count : UNLAID_ROWS;
    if (list) {
        ui_list window = { .count = count, .reveal = -1 };
        length = ui_list_window(list, &window, &s->gallery.list, &first);
    }
    bool changed = fit(at->doc, rows, length);
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(rows);
    for (int i = first; (item = yyjson_mut_arr_iter_next(&it)); i++) {
        changed |= show_asset(s, at->doc, item, i);
    }
    return changed;
}

/* Collected's assets, with their pictures once ready. */
static bool show_bin(studio *s, const studio_panel_sync *at, yyjson_mut_val *rows)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    bool changed = fit(at->doc, rows, acount(s->gallery.bin));
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(rows);
    for (int i = 0; (item = yyjson_mut_arr_iter_next(&it)); i++) {
        const studio_gallery_item *held = &s->gallery.bin[i];
        int id = index ? reference_index_find_name(index, held->kind, held->name)
                       : -1;
        if (id < 0) {
            char icon[5];
            glyph(icon, KIT_ICON_WARNING);
            changed |= ui_data_put_text(at->doc, item, "label", held->name);
            changed |= ui_data_put_text(at->doc, item, "icon", icon);
            changed |= ui_data_put_bool(item, "unresolved", true);
            continue;
        }
        changed |= show_asset(s, at->doc, item, id);
        bool loading = false;
        const char *image = studio_thumb(s, id, STUDIO_THUMB_TILE, &loading);
        char badge[5] = "";
        if (image) {
            glyph(badge, studio_asset_kind_icon(held->kind));
        }
        changed |= ui_data_put_text(at->doc, item, "image", image ? image : "");
        changed |= ui_data_put_text(at->doc, item, "badge", badge);
        changed |= ui_data_put_bool(item, "loading", image && loading);
    }
    return changed;
}

bool studio_gallery_sync(studio *s, const studio_panel_sync *at)
{
    bool changed = show_assets(s, at, yyjson_mut_obj_get(at->variable, "assets"));
    return show_bin(s, at, yyjson_mut_obj_get(at->variable, "bin")) || changed;
}

/* ---------- Drops ---------- */

static int kind_named(const char *type)
{
    for (int kind = 0; kind < REFERENCE_ASSET_KIND_COUNT; kind++) {
        if (!strcmp(studio_asset_kind_name(kind), type)) {
            return kind;
        }
    }
    return -1;
}

/* An asset dropped on Collected: after its rows, or before or after the
 * row it landed on. A drop on a row bubbles to Collected, zone and all,
 * after the row placed it. */
static void drop(studio *s, const ui_activation *a)
{
    int row = atoi(a->args[2]), kind = kind_named(a->args[0]);
    const char *zone = a->args[3];
    if ((row < 0 && zone[0]) || kind < 0) {
        return;
    }
    int count = acount(s->gallery.bin);
    int at = row < 0 ? count : row + !strcmp(zone, "after");
    at = at > count ? count : at;
    studio_gallery_item item = { .kind = kind };
    snprintf(item.name, sizeof item.name, "%s", a->args[1]);
    apush(s->gallery.bin, item);
    memmove(&s->gallery.bin[at + 1], &s->gallery.bin[at], (size_t)(count - at) * sizeof item);
    s->gallery.bin[at] = item;
}

void studio_gallery_action(studio *s, const char *action, const ui_activation *a)
{
    /* A scroll only wakes the source, which reads where the List is. */
    if (!strcmp(action, "drop") && a->argc == 4) {
        drop(s, a);
    }
}
