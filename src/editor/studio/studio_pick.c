#include "assets/ui_document.h"
/* Asset pickers, a client of the palette (studio_palette_view.c): a field
 * naming an asset shows it as a select and chooses another of its kind in
 * a searchable list under it, as Unity's object field and Unreal's asset
 * picker search a project's assets. An authored AssetField hears the name
 * chosen as change; a kit field takes it at its next call, as the kit
 * returns a control's input; Preview UI Asset previews it. */
#include "editor/studio/studio_internal.h"
#include "support/array.h"
#include "support/asset_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* What a pick's result does. */
enum
{
    CHOOSE_NONE,
    CHOOSE_ASSET,
    CHOOSE_TYPED, /* a name no asset has yet */
};

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

bool studio_asset_nameable(const reference_asset *asset, int kind)
{
    const char *extension = asset->path ? strrchr(asset->path, '.') : NULL;
    bool source = !extension ||
                  !strcmp(extension, kind == REFERENCE_ASSET_UI ? UI_MARKUP_EXTENSION : DECLARATIVE_ASSET_EXTENSION) ||
                  (kind != REFERENCE_ASSET_UI &&
                   !strcmp(extension, CODE_ASSET_EXTENSION));
    return (int)asset->kind == kind && !asset->unresolved && source;
}

const char **studio_asset_names(const reference_index *index, int kind)
{
    const char **names = NULL;
    for (int i = 0; index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (studio_asset_nameable(asset, kind)) {
            apush(names, asset->name);
        }
    }
    if (acount(names) > 1) {
        qsort(names, (size_t)acount(names), sizeof *names, by_name);
    }
    return names;
}

/* Starts a pick for `key`; `placeholder` names the kind's search. */
static void pick_begin(
    studio *s,
    const char *key,
    const char *current,
    const studio_pick_desc *desc,
    char placeholder[96]
)
{
    s->pick.chosen = false;
    s->pick.kind = desc->kind;
    s->pick.text = desc->text;
    snprintf(s->pick.key, sizeof s->pick.key, "%s", key);
    snprintf(s->pick.none, sizeof s->pick.none, "%s", desc->none ? desc->none : "");
    snprintf(s->pick.current, sizeof s->pick.current, "%s", current ? current : "");
    snprintf(placeholder, 96, "Search %s", studio_asset_kind_label(desc->kind));
}

void studio_pick_open(
    studio *s,
    const char *key,
    const char *current,
    const studio_pick_desc *desc,
    rml_element *anchor
)
{
    char placeholder[96];
    pick_begin(s, key, current, desc, placeholder);
    if (!anchor) {
        studio_palette_open(s, STUDIO_PALETTE_ASSETS, placeholder);
        return;
    }
    studio_palette_open_at(s, STUDIO_PALETTE_ASSETS, placeholder, anchor);
}

void studio_pick_open_box(
    studio *s,
    const char *key,
    const char *current,
    const studio_pick_desc *desc,
    const float box[4]
)
{
    char placeholder[96];
    pick_begin(s, key, current, desc, placeholder);
    studio_palette_open_box(s, STUDIO_PALETTE_ASSETS, placeholder, box);
}

static bool any_matches(const char *const *names, const char *query)
{
    for (int i = 0; i < acount(names); i++) {
        if (studio_matches(query, names[i])) {
            return true;
        }
    }
    return false;
}

void studio_pick_entries(studio *s, const char *query, studio_palette_entry **entries)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    const char **names = studio_asset_names(index, s->pick.kind);
    const char *group = studio_asset_kind_label(s->pick.kind);
    uint32_t icon = studio_asset_kind_icon(s->pick.kind);
    if (s->pick.none[0]) {
        studio_palette_entry *none = studio_palette_add(entries, "", s->pick.none);
        none->icon = KIT_ICON_CIRCLE;
        none->kind = CHOOSE_NONE;
        none->current = !s->pick.current[0];
    }
    for (int i = 0; i < acount(names); i++) {
        studio_palette_entry *e = studio_palette_add(entries, group, names[i]);
        e->icon = icon;
        e->kind = CHOOSE_ASSET;
        e->current = !strcmp(names[i], s->pick.current);
    }
    /* A name no asset has yet, as typed, once no asset matches it: an exact
       match would otherwise rank it above the assets. */
    if (s->pick.text && query[0] && !any_matches(names, query)) {
        char label[STUDIO_PALETTE_TEXT];
        snprintf(label, sizeof label, "Use \"%s\"", query);
        studio_palette_entry *typed = studio_palette_add(entries, "", label);
        snprintf(typed->keywords, sizeof typed->keywords, "%s", query);
        snprintf(typed->about, sizeof typed->about, "No asset has this name yet");
        typed->icon = KIT_ICON_EDIT;
        typed->kind = CHOOSE_TYPED;
    }
    afree(names);
}

void studio_pick_choose(studio *s, const studio_palette_entry *entry, rml_element *anchor)
{
    const char *value = entry->kind == CHOOSE_ASSET ? entry->label
                      : entry->kind == CHOOSE_TYPED ? entry->keywords
                                                    : "";
    if (!strcmp(s->pick.key, STUDIO_PICK_PREVIEW)) {
        studio_preview_ui(s, value);
        return;
    }
    if (!strcmp(s->pick.key, STUDIO_PICK_FIELD)) {
        if (anchor) {
            rml_element_dispatch_value(anchor, "change", value, false);
        }
        return;
    }
    snprintf(s->pick.value, sizeof s->pick.value, "%s", value);
    s->pick.chosen = true;
}

const char *studio_asset_field(
    studio *s,
    const char *key,
    const char *value,
    const studio_pick_desc *desc,
    unsigned flags
)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    /* Events are names: host, action and ui events have no asset. */
    bool named = value && value[0] && index && desc->kind != REFERENCE_ASSET_EVENT;
    const reference_asset *found = named
                                     ? reference_index_asset_at(
                                           index,
                                           reference_index_find_name(index, desc->kind, value)
                                       )
                                     : NULL;
    bool unresolved = named && (!found || found->unresolved);
    char tip[192] = "";
    if (unresolved) {
        snprintf(tip, sizeof tip, "No %s asset is named %s", studio_asset_kind_name(desc->kind), value);
    }
    /* An asset dragged over the field shows in it (studio_drop.c). */
    const char *dragged = studio_drop_shown(s, key);
    kit_picker_desc picker = { .value = dragged ? dragged : value,
                               .placeholder = desc->none ? desc->none : "",
                               .icon = studio_asset_kind_icon(desc->kind),
                               .tip = tip[0] ? tip : NULL,
                               .flags = flags |
                                        (unresolved && !dragged ? KIT_INVALID : 0) };
    if (kit_picker(s->kit, "value", &picker)) {
        studio_pick_open(s, key, value, desc, kit_item_element(s->kit));
    }
    kit_flag(s->kit, "data-drop-preview", dragged != NULL);
    bool editable = !(flags & (KIT_READONLY | KIT_DISABLED)) &&
                    desc->kind != REFERENCE_ASSET_EVENT;
    const char *dropped = editable ? studio_drop_picker(s, key, desc->kind) : NULL;
    if (dropped) {
        return dropped;
    }
    if (!s->pick.chosen || strcmp(s->pick.key, key)) {
        return NULL;
    }
    s->pick.chosen = false;
    return s->pick.value;
}
