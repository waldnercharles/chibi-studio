/* The Library (workstream H, authored in D1): while a UI document is
 * active, the Assets area lists what can go into it, as Unity UI Builder's
 * Library and UMG's Palette do: basic elements, then the project's ui
 * assets as widgets (<use ui>). Drag an item onto the Hierarchy or the
 * Canvas, or click it to add it to the selection. The Hierarchy's Add menus
 * list the same items. Library.rml and LibraryHeader.rml bind the `library`
 * source, which every refresh keeps, since the Hierarchy's menus use it too:
 *
 *   library: { filter, none, widget: MENU ITEM,
 *              elements: [ITEM], widgets: [ITEM] }
 *   ITEM: { label, icon, markup, shown, shortcut, checked, disabled }
 *
 * `shown` is whether the filter keeps the item as a tile; the menus list all
 * of them. library.add takes an item's markup and label. */
#include "editor/studio/studio_designer_internal.h"
#include "support/array.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
    const char *label;
    uint32_t icon;
    const char *markup;
} library_element;

static const library_element ELEMENTS[] = {
    { "Container", KIT_ICON_ELEMENT, "<div/>" },
    { "Text", KIT_ICON_TEXT, "<span>Text</span>" },
    { "Button", KIT_ICON_BUTTON, "<button>Button</button>" },
    { "Text Field", KIT_ICON_INPUT, "<input type=\"text\"/>" },
    { "Checkbox", KIT_ICON_CHECKBOX, "<input type=\"checkbox\"/>" },
    { "Slider", KIT_ICON_SLIDER, "<input type=\"range\"/>" },
    { "Image", KIT_ICON_IMAGE, "<img src=\"\"/>" },
    { "Progress", KIT_ICON_PROGRESS, "<progress value=\"0.5\"/>" },
    /* Native elements for authored UI (workstream N; ui/motion.h). */
    { "Number Flow", KIT_ICON_NUMBER, "<number-flow value=\"0\"/>" },
    { "Morph Text", KIT_ICON_TEXT, "<morph-text>Text</morph-text>" },
};

enum
{
    ELEMENT_COUNT = sizeof ELEMENTS / sizeof *ELEMENTS,
};

/* The project's ui assets but the document itself, which cannot use
 * itself. The caller afrees the array; the names belong to the index. */
static const char **widget_names(studio *s, const studio_designer *d)
{
    const char **names = studio_asset_names(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_UI);
    int kept = 0;
    for (int i = 0; i < acount(names); i++) {
        if (strcmp(names[i], d->asset)) {
            names[kept++] = names[i];
        }
    }
    asetlen(names, kept);
    return names;
}

/* ---------- Data ---------- */

static const char *const ITEM_TEXTS[] = { "label", "icon", "markup", "shortcut", NULL };
static const char *const ITEM_FLAGS[] = { "shown", "checked", "disabled", NULL };

static yyjson_mut_val *item_new(yyjson_mut_doc *doc)
{
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    for (const char *const *text = ITEM_TEXTS; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, item, *text, "");
    }
    for (const char *const *flag = ITEM_FLAGS; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, item, *flag, false);
    }
    return item;
}

void studio_library_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *library = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "library");
    yyjson_mut_obj_add_strcpy(doc, library, "filter", "");
    yyjson_mut_obj_add_bool(doc, library, "none", false);
    yyjson_mut_obj_add_val(doc, library, "widget", item_new(doc));
    yyjson_mut_obj_add_arr(doc, library, "elements");
    yyjson_mut_obj_add_arr(doc, library, "widgets");
}

/* Gives `array` `length` items, adding and removing at its end, so the
 * items shown keep their elements; true when its length changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *array, int length)
{
    int had = (int)yyjson_mut_arr_size(array);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(array, item_new(doc));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(array);
    }
    return had != length;
}

static bool show_item(
    yyjson_mut_doc *doc,
    yyjson_mut_val *item,
    const studio_designer *d,
    const char *label,
    uint32_t icon,
    const char *markup
)
{
    char glyph[5];
    studio_glyph(glyph, icon);
    bool changed = ui_data_put_text(doc, item, "label", label);
    changed |= ui_data_put_text(doc, item, "icon", glyph);
    changed |= ui_data_put_text(doc, item, "markup", markup);
    return ui_data_put_bool(item, "shown", studio_matches(d->library_filter, label)) ||
           changed;
}

static void widget_markup(const char *name, char *markup, size_t size)
{
    snprintf(markup, size, "<use ui=\"%s\"/>", name);
}

/* Whether the items shown are the ones the assets and the filter give, so a
 * refresh that changes nothing walks nothing. */
static bool current(const studio *s, const studio_designer *d)
{
    return d->library_seen.built &&
           d->library_seen.revision == s->scene->asset_index_revision &&
           !strcmp(d->library_seen.asset, d->asset) &&
           !strcmp(d->library_seen.filter, d->library_filter);
}

bool studio_library_sync(studio *s, const studio_panel_sync *at)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    if (!d || at->preview) {
        return false;
    }
    yyjson_mut_val *library = at->variable;
    yyjson_mut_val *filter = yyjson_mut_obj_get(library, "filter");
    if (at->wrote && strcmp(yyjson_mut_get_str(filter), d->library_filter)) {
        snprintf(d->library_filter, sizeof d->library_filter, "%s", yyjson_mut_get_str(filter));
    }
    if (current(s, d)) {
        return false;
    }
    d->library_seen.built = true;
    d->library_seen.revision = s->scene->asset_index_revision;
    snprintf(d->library_seen.asset, sizeof d->library_seen.asset, "%s", d->asset);
    snprintf(d->library_seen.filter, sizeof d->library_seen.filter, "%s", d->library_filter);
    bool changed = ui_data_put_text(at->doc, library, "filter", d->library_filter);
    char glyph[5];
    studio_glyph(glyph, KIT_ICON_WIDGET);
    yyjson_mut_val *widget = yyjson_mut_obj_get(library, "widget");
    changed |= ui_data_put_text(at->doc, widget, "label", "Widget");
    changed |= ui_data_put_text(at->doc, widget, "icon", glyph);
    yyjson_mut_val *elements = yyjson_mut_obj_get(library, "elements");
    changed |= fit(at->doc, elements, ELEMENT_COUNT);
    for (int i = 0; i < ELEMENT_COUNT; i++) {
        changed |= show_item(
            at->doc,
            yyjson_mut_arr_get(elements, (size_t)i),
            d,
            ELEMENTS[i].label,
            ELEMENTS[i].icon,
            ELEMENTS[i].markup
        );
    }
    const char **names = widget_names(s, d);
    yyjson_mut_val *widgets = yyjson_mut_obj_get(library, "widgets");
    changed |= fit(at->doc, widgets, acount(names));
    for (int i = 0; i < acount(names); i++) {
        char markup[160];
        widget_markup(names[i], markup, sizeof markup);
        changed |= show_item(at->doc, yyjson_mut_arr_get(widgets, (size_t)i), d, names[i], KIT_ICON_WIDGET, markup);
    }
    changed |= ui_data_put_bool(library, "none", !acount(names));
    afree(names);
    return changed;
}

void studio_library_action(studio *s, const char *action, const ui_activation *a)
{
    ui_buffer *b;
    studio_designer *d = designer_state(s, &b);
    if (d && !strcmp(action, "add") && a->argc == 2) {
        designer_add(s, d->selected, a->args[0], a->args[1]);
    }
}

/* ---------- Panel ---------- */

void studio_designer_library_header(studio *s, int area)
{
    rml_element *host = studio_ui_area_controls(s, area, "library", "LibraryHeader");
    rml_element *field = host && s->find_request == STUDIO_EDITOR_ASSETS + 1
                           ? rml_element_query_selector(host, "#library-filter")
                           : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        s->find_request = 0;
    }
}

void studio_designer_library(studio *s, int area)
{
    studio_ui_area(s, area, "Library");
}
