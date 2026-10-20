/* Rename and safe delete across files: one popover names the symbol anew
 * and lists every asset the rename rewrites, or lists what still uses a
 * symbol being deleted and asks to delete it anyway (JetBrains' Rename and
 * Safe Delete; Blender asks in a popup menu). Applying writes every file or
 * none (editor/workspace/asset_refactor.h); a toast then offers Undo, which
 * puts every file back. */
#include "assets/refactor.h"
#include "authoring/preview_source.h"
#include "authoring/source_rename.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/workspace/asset_refactor.h"
#include "graph/compiler/validation.h"
#include "graph/document/events.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "ui/popover.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LISTED 10 /* assets or uses a popover lists before "N more" */

static void close_state(studio_refactor *r)
{
    app_refactor_free(r->plan);
    free(r->source_before);
    free(r->source_after);
    afree(r->uses);
    *r = (studio_refactor){ .undo = r->undo,
                            .undo_toast = r->undo_toast,
                            .mounted = r->mounted };
}

/* The symbol with its asset found again, since the index may have been
   rebuilt since the popover opened. */
static reference_symbol resolved(studio *s)
{
    studio_refactor *r = &s->refactor;
    app_scene_authoring_refresh_asset_index(s->scene);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    reference_symbol symbol = r->symbol;
    symbol.asset = reference_index_find_name(index, r->asset_kind, r->asset);
    symbol.name = symbol.kind == REFERENCE_SYMBOL_EVENT ? r->event : NULL;
    return symbol;
}

static void begin(studio *s, studio_refactor_mode mode, reference_symbol symbol, const char *name)
{
    studio_refactor *r = &s->refactor;
    close_state(r);
    const reference_asset *asset = reference_index_asset_at(
        app_scene_authoring_asset_index(s->scene),
        symbol.asset
    );
    bool event = symbol.kind == REFERENCE_SYMBOL_EVENT;
    r->mode = mode;
    r->symbol = symbol;
    r->asset_kind = event ? REFERENCE_ASSET_EVENT : (int)asset->kind;
    snprintf(
        r->asset,
        sizeof r->asset,
        "%s",
        event ? (symbol.name ? symbol.name : "") : asset->name
    );
    snprintf(r->event, sizeof r->event, "%s", event ? symbol.name : "");
    snprintf(r->title, sizeof r->title, "%s", name);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->opening = true;
}

static asset_code *source_code(studio *s, const char *path)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        if (!strcmp(path, asset_code_path(s->code.assets[i]))) {
            return s->code.assets[i];
        }
    }
    return NULL;
}

/* A rename across files edits the saved files, so it waits for Save. */
static bool unsaved_source(studio *s, asset_code *code)
{
    if (!asset_code_dirty(code)) {
        return false;
    }
    snprintf(
        s->refactor.error,
        sizeof s->refactor.error,
        "Save %s before renaming across files",
        content_basename(asset_code_path(code))
    );
    return true;
}

/* Whether renaming the symbol rewrites another file. */
static bool crosses_files(const authoring_navigation *index, int document, const authoring_reference *reference)
{
    if (reference->target.kind == AUTHORING_PROGRAM) {
        return true;
    }
    for (int i = 0; i < authoring_navigation_reference_count(index); i++) {
        const authoring_reference *found = authoring_navigation_reference_at(index, i);
        if (found->location.document != document &&
            authoring_target_equal(found->target, reference->target)) {
            return true;
        }
    }
    return false;
}

void studio_refactor_source(studio *s, const char *path, int offset)
{
    asset_code *code = source_code(s, path);
    assert(code);
    const authoring_navigation *index = asset_code_navigation(code);
    const authoring_reference
        *reference = index
                       ? authoring_navigation_at(index, asset_code_document(code), offset)
                       : NULL;
    if (!reference || !authoring_navigation_definition(index, reference->target)) {
        kit_code_notice(asset_code_editor(code), "No symbol to rename here");
        return;
    }
    const char *name = authoring_navigation_name(index, reference->target);
    studio_refactor *r = &s->refactor;
    close_state(r);
    r->mode = STUDIO_REFACTOR_RENAME;
    r->opening = true;
    r->source_offset = offset;
    kit_rect c = { 0 };
    if (kit_wants_text(s->kit, &c) && c.h > 0) { /* hang below the caret */
        float ratio = studio_pixel_ratio(s), *box = r->anchor;
        box[0] = c.x * ratio;
        box[1] = c.y * ratio;
        box[2] = (c.w > 0 ? c.w : 1) * ratio;
        box[3] = c.h * ratio;
    }
    snprintf(r->source_path, sizeof r->source_path, "%s", path);
    snprintf(r->title, sizeof r->title, "%s", name ? name : "");
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    r->source_before = xstrdup(asset_code_text(code));
    if (crosses_files(index, asset_code_document(code), reference)) {
        unsaved_source(s, code);
    }
}

static void plan_source(studio *s, asset_code *code)
{
    studio_refactor *r = &s->refactor;
    const authoring_navigation *index = asset_code_navigation(code);
    int document = asset_code_document(code);
    const authoring_reference *reference = authoring_navigation_at(index, document, r->source_offset);
    if (!reference) {
        snprintf(r->error, sizeof r->error, "Source symbol is no longer available");
        return;
    }
    authoring_rename_plan *rename = authoring_source_rename(
        index,
        reference->target,
        r->name,
        r->error,
        sizeof r->error
    );
    if (!rename) {
        return;
    }
    r->source_project = reference->target.kind == AUTHORING_PROGRAM;
    for (int i = 0; i < rename->count; i++) {
        r->source_project |= rename->edits[i].document != document;
    }
    if (r->source_project && !unsaved_source(s, code)) {
        app_scene_authoring_plan_source_rename(
            s->scene,
            index,
            reference->target,
            r->name,
            &r->plan,
            r->error,
            sizeof r->error
        );
    } else if (!r->source_project) {
        r->source_after = authoring_rename_document(
            rename,
            document,
            r->source_before,
            r->error,
            sizeof r->error
        );
        source_catalog_edit edit = { .path = asset_code_path(code),
                                     .text = r->source_after };
        if (r->source_after && preview_sources_validate(
                                   s->scene->live,
                                   &edit,
                                   1,
                                   r->error,
                                   sizeof r->error
                               )) {
            free(r->source_after);
            r->source_after = NULL;
        }
    }
    authoring_rename_plan_free(rename);
}

static void plan(studio *s)
{
    studio_refactor *r = &s->refactor;
    app_refactor_free(r->plan);
    r->plan = NULL;
    r->error[0] = '\0';
    snprintf(r->planned, sizeof r->planned, "%s", r->name);
    asset_code *code = r->source_path[0] ? source_code(s, r->source_path) : NULL;
    if (r->source_path[0] &&
        (!code || strcmp(r->source_before, asset_code_text(code)))) {
        snprintf(r->error, sizeof r->error, "Source changed; reopen Rename");
        return;
    }
    if (r->source_path[0]) {
        free(r->source_after);
        r->source_after = NULL;
        plan_source(s, code);
        return;
    }

    app_scene_authoring_plan_rename(
        s->scene,
        resolved(s),
        r->name,
        &r->plan,
        r->error,
        sizeof r->error
    );
}

static void open_rename(studio *s, reference_symbol symbol, const char *name)
{
    begin(s, STUDIO_REFACTOR_RENAME, symbol, name);
}

bool studio_refactor_member_rename(studio *s, reference_symbol symbol, const char *name, const char *new_name)
{
    if (!app_scene_authoring_refactor_reaches(s->scene, symbol)) {
        return false;
    }
    begin(s, STUDIO_REFACTOR_RENAME, symbol, name);
    snprintf(s->refactor.name, sizeof s->refactor.name, "%s", new_name);
    plan(s);
    return true;
}

/* What still uses a symbol, as "Asset › Place". */
static void describe_uses(studio *s, reference_symbol symbol)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    const reference **uses = NULL;
    refactor_blockers(index, symbol, &uses);
    for (int i = 0; i < acount(uses); i++) {
        char place[192];
        refactor_describe(index, uses[i], place, sizeof place);
        studio_refactor_use use;
        snprintf(use.text, sizeof use.text, "%s", place);
        apush(s->refactor.uses, use);
    }
    afree(uses);
}

bool studio_refactor_confirm_delete(
    studio *s,
    reference_symbol symbol,
    const char *name,
    studio_member_kind member,
    uint64_t id
)
{
    app_scene_authoring_refresh_asset_index(s->scene);
    const reference **uses = NULL;
    refactor_blockers(app_scene_authoring_asset_index(s->scene), symbol, &uses);
    bool used = acount(uses) > 0;
    afree(uses);
    if (!used) {
        return false;
    }
    begin(s, STUDIO_REFACTOR_DELETE, symbol, name);
    s->refactor.member = member;
    s->refactor.member_id = id;
    describe_uses(s, symbol);
    return true;
}

/* ---------- Applying ---------- */

static void offer_undo(studio *s, app_refactor *undo, const char *title, int assets)
{
    studio_refactor *r = &s->refactor;
    app_refactor_free(r->undo);
    r->undo = undo;
    char description[64];
    snprintf(description, sizeof description, "%d %s", assets, assets == 1 ? "asset" : "assets");
    kit_toast_dismiss(s->kit, r->undo_toast);
    r->undo_toast = kit_toast(
        s->kit,
        0,
        &(
            kit_toast_desc
        ){ .title = title, .description = description, .action = "Undo", .duration = 6 }
    );
    studio_record(s, KIT_TOAST_DEFAULT, title);
}

static void refused(studio *s, const char *title, const char *error)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title, .description = error });
}

/* Applies a plan; renaming the open graph asset keeps its graph's camera. */
static int apply_plan(studio *s, const app_refactor *plan, app_refactor **undo, char *error, size_t size)
{
    const char *id = s->scene->graph_document_id;
    char *open = id ? xstrdup(id) : NULL;
    int result = app_scene_authoring_apply_refactor(s->scene, plan, undo, error, size);
    for (int i = 0; result == 0 && open && i < app_refactor_count(plan); i++) {
        app_refactor_asset asset = app_refactor_at(plan, i);
        if (asset.renamed && !strcmp(asset.name, open) && s->scene->graph_document_id &&
            !strcmp(asset.renamed, s->scene->graph_document_id)) {
            sg_keep_graph_view(s);
        }
    }
    free(open);
    return result;
}

static void apply(studio *s, const app_refactor *plan, const char *title)
{
    app_refactor *undo = NULL;
    char error[256] = "";
    if (apply_plan(s, plan, &undo, error, sizeof error) != 0) {
        refused(s, "Nothing changed", error);
        return;
    }
    offer_undo(s, undo, title, app_refactor_count(plan));
}

static void apply_rename(studio *s)
{
    studio_refactor *r = &s->refactor;
    if (r->source_project) {
        asset_code *code = source_code(s, r->source_path);
        if (!code || strcmp(r->source_before, asset_code_text(code))) {
            snprintf(r->error, sizeof r->error, "Source changed; reopen Rename");
            return;
        }
        if (unsaved_source(s, code)) {
            return;
        }
    }
    if (r->source_path[0] && !r->source_project) {
        if (!r->source_after ||
            !studio_code_replace(s, r->source_path, r->source_before, r->source_after)) {
            snprintf(r->error, sizeof r->error, "Source changed; reopen Rename");
            return;
        }
        studio_report(s, "Renamed symbol");
        close_state(r);
        return;
    }
    char title[192];
    snprintf(title, sizeof title, "Renamed %s", r->title);
    apply(s, r->plan, title);
    close_state(r);
}

bool studio_asset_refactorable(const reference_asset *asset)
{
    return asset && !asset->unresolved && asset->kind != REFERENCE_ASSET_SCENE &&
           asset->kind != REFERENCE_ASSET_SYSTEM &&
           (asset->kind != REFERENCE_ASSET_UI ||
            studio_asset_nameable(asset, REFERENCE_ASSET_UI));
}

bool studio_asset_deletable(studio *s, const reference_asset *asset)
{
    return studio_asset_refactorable(asset) &&
           !(asset->path &&
             source_catalog_source_path(s->scene->content_root, asset->path));
}

static const char CODE_UNDELETABLE[] = "Studio can't delete code assets yet";

/* Deletes an asset's file, or an event definition's. */
static void delete_asset(studio *s, reference_symbol symbol, const char *name)
{
    app_refactor *plan = NULL;
    char error[256] = "";
    char title[192];
    snprintf(title, sizeof title, "Deleted %s", name);
    if (app_scene_authoring_plan_delete(s->scene, symbol, &plan, error, sizeof error) !=
        0) {
        refused(s, "Can't delete", error);
        return;
    }
    apply(s, plan, title);
    app_refactor_free(plan);
}

void studio_refactor_delete_asset(studio *s, int asset)
{
    const reference_asset *
        found = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), asset);
    if (!studio_asset_refactorable(found)) {
        return;
    }
    if (!studio_asset_deletable(s, found)) {
        refused(s, "Nothing deleted", CODE_UNDELETABLE);
        return;
    }
    reference_symbol symbol = { .kind = REFERENCE_SYMBOL_ASSET, .asset = asset };
    char name[128];
    snprintf(name, sizeof name, "%s", found->name);
    if (!studio_refactor_confirm_delete(s, symbol, name, STUDIO_MEMBER_NONE, 0)) {
        delete_asset(s, symbol, name);
    }
}

/* The first of the assets something still uses, or -1. */
static int first_used(studio *s, const int *assets, int count)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    for (int i = 0; i < count; i++) {
        const reference **uses = NULL;
        refactor_blockers(
            index,
            (reference_symbol){ .kind = REFERENCE_SYMBOL_ASSET, .asset = assets[i] },
            &uses
        );
        bool used = acount(uses) > 0;
        afree(uses);
        if (used) {
            return i;
        }
    }
    return -1;
}

/* "Deleted A and B", or "Deleted A and 3 others". */
static void deleted_title(const reference_index *index, const int *assets, int count, char *out, size_t size)
{
    const char *first = reference_index_asset_at(index, assets[0])->name;
    if (count == 2) {
        snprintf(
            out,
            size,
            "Deleted %s and %s",
            first,
            reference_index_asset_at(index, assets[1])->name
        );
        return;
    }
    snprintf(out, size, "Deleted %s and %d others", first, count - 1);
}

/* The assets in the rebuilt index, which can number them differently from
   the one they were selected in; NULL, refusing, when one is gone. */
static int *find_assets(studio *s, const studio_asset_ref *refs, int count)
{
    app_scene_authoring_refactor_index(s->scene);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int *assets = NULL;
    for (int i = 0; i < count; i++) {
        int asset = reference_index_find_name(index, refs[i].kind, refs[i].name);
        if (asset < 0) {
            char error[256];
            snprintf(error, sizeof error, "%s no longer exists", refs[i].name);
            refused(s, "Nothing deleted", error);
            afree(assets);
            return NULL;
        }
        apush(assets, asset);
    }
    return assets;
}

/* Deletes the found assets as one change. */
static void delete_found(studio *s, const int *assets, int count)
{
    int used = first_used(s, assets, count);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    char title[192], error[256] = "";
    for (int i = 0; i < count; i++) {
        const reference_asset *asset = reference_index_asset_at(index, assets[i]);
        if (!studio_asset_deletable(s, asset)) {
            snprintf(error, sizeof error, "Studio can't delete %s", asset->name);
            refused(s, "Nothing deleted", studio_asset_refactorable(asset) ? CODE_UNDELETABLE : error);
            return;
        }
    }
    if (used >= 0) {
        snprintf(
            error,
            sizeof error,
            "%s is still used; delete it alone to see where",
            reference_index_asset_at(index, assets[used])->name
        );
        refused(s, "Nothing deleted", error);
        return;
    }
    deleted_title(index, assets, count, title, sizeof title);
    app_refactor *plan = NULL;
    for (int i = 0; i < count; i++) {
        app_refactor *one = NULL;
        reference_symbol symbol = { .kind = REFERENCE_SYMBOL_ASSET,
                                    .asset = assets[i] };
        if (app_scene_authoring_plan_delete(s->scene, symbol, &one, error, sizeof error) !=
            0) {
            app_refactor_free(plan);
            refused(s, "Nothing deleted", error);
            return;
        }
        if (plan) {
            app_refactor_join(plan, one);
        } else {
            plan = one;
        }
    }
    apply(s, plan, title);
    app_refactor_free(plan);
}

/* Deletes several unused assets as one change, which one Undo restores. A
   used one refuses them all: the single delete lists where it is used. */
void studio_refactor_delete_assets(studio *s, const studio_asset_ref *refs, int count)
{
    int *assets = find_assets(s, refs, count);
    if (!assets) {
        return;
    }
    if (count == 1) {
        studio_refactor_delete_asset(s, assets[0]);
    } else {
        delete_found(s, assets, count);
    }
    afree(assets);
}

void studio_refactor_rename_asset(studio *s, int asset)
{
    const reference_asset *
        found = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), asset);
    if (studio_asset_refactorable(found)) {
        open_rename(
            s,
            (reference_symbol){ .kind = REFERENCE_SYMBOL_ASSET, .asset = asset },
            found->name
        );
    }
}

static void confirm_delete(studio *s)
{
    studio_refactor *r = &s->refactor;
    if (r->member) {
        studio_members_delete_now(s, r->member, r->member_id);
    } else {
        char name[128];
        snprintf(name, sizeof name, "%s", r->title);
        delete_asset(s, resolved(s), name);
    }
    close_state(r);
}

/* ---------- Symbols ---------- */

const char *studio_event_renameable(const studio *s, const node *n)
{
    if (studio_node_reactive(s, n)) {
        return NULL;
    }
    const char *name = n && n->desc ? n->desc->name : "";
    bool event = !strcmp(name, "Event") || !strcmp(name, "Call Event") ||
                 !strcmp(name, "Send Event");
    return event && n->reference_name && n->reference_name[0] &&
                   !host_event_reserved(n->reference_name)
             ? n->reference_name
             : NULL;
}

void studio_refactor_rename_event(studio *s, const char *event)
{
    if (host_event_reserved(event)) {
        return;
    }
    app_scene_authoring_refresh_asset_index(s->scene);
    reference_symbol symbol = {
        .kind = REFERENCE_SYMBOL_EVENT,
        .asset = reference_index_find_name(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_EVENT, event),
        .name = event,
    };
    open_rename(s, symbol, event);
}

/* ---------- The popover (Refactor.rml, the `refactor` source) ---------- */

/* Ids in Refactor.rml. */
#define POPOVER_ID "refactor"
#define NAME_ID "refactor-name"

/* What the popover lists: a rename's assets and its mentions, or what
   still uses a symbol being deleted. */
typedef struct
{
    uint32_t icon;
    char label[192];
    char meta[64];
} listed_row;

static void list_row(listed_row **rows, uint32_t icon, const char *label, const char *meta)
{
    apush(*rows, ((listed_row){ .icon = icon }));
    snprintf(alast(*rows).label, sizeof alast(*rows).label, "%s", label);
    snprintf(alast(*rows).meta, sizeof alast(*rows).meta, "%s", meta);
}

static void list_more(listed_row **rows, int more)
{
    char text[32];
    snprintf(text, sizeof text, "%d more", more);
    list_row(rows, KIT_ICON_MORE, text, "");
}

static bool planned_now(const studio_refactor *r)
{
    return (r->source_path[0] && !r->source_project ? r->source_after != NULL
                                                    : r->plan != NULL) &&
           !strcmp(r->planned, r->name);
}

/* A rename lists each asset it rewrites with the name it takes or how many
   changes it makes, LISTED at most, and how often prose mentions it. */
static void plan_rows(const studio_refactor *r, listed_row **rows)
{
    int count = app_refactor_count(r->plan);
    for (int i = 0; i < count && i < LISTED; i++) {
        app_refactor_asset asset = app_refactor_at(r->plan, i);
        char meta[64];
        if (asset.renamed) {
            snprintf(meta, sizeof meta, "%s", asset.renamed);
        } else {
            snprintf(meta, sizeof meta, "%d", asset.changes);
        }
        list_row(rows, studio_asset_kind_icon(asset.kind), asset.name, meta);
    }
    if (count > LISTED) {
        list_more(rows, count - LISTED);
    }
    int mentions = app_refactor_mentions(r->plan);
    if (mentions) {
        char text[48];
        snprintf(text, sizeof text, "%d %s", mentions, mentions == 1 ? "mention" : "mentions");
        list_row(rows, KIT_ICON_WARNING, text, "");
    }
}

static listed_row *rows_of(const studio_refactor *r)
{
    listed_row *rows = NULL;
    if (r->mode == STUDIO_REFACTOR_RENAME && planned_now(r)) {
        if (r->source_path[0] && !r->source_project) {
            list_row(&rows, KIT_ICON_EXPRESSION, r->source_path, "Shared Undo");
        } else {
            plan_rows(r, &rows);
        }
    }
    for (int i = 0;
         r->mode == STUDIO_REFACTOR_DELETE && i < acount(r->uses) && i < LISTED;
         i++) {
        list_row(&rows, KIT_ICON_REFERENCES, r->uses[i].text, "");
    }
    if (r->mode == STUDIO_REFACTOR_DELETE && acount(r->uses) > LISTED) {
        list_more(&rows, acount(r->uses) - LISTED);
    }
    return rows;
}

static const char *const ROW_TEXTS[] = { "icon", "label", "meta" };
static const char *const ITEM_TEXTS[] = { "icon", "label", "shortcut" };

static void add_texts(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_obj_add_strcpy(doc, obj, names[i], "");
    }
}

void studio_refactor_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *r = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "refactor");
    yyjson_mut_obj_add_strcpy(doc, r, "title", "");
    yyjson_mut_obj_add_strcpy(doc, r, "error", "");
    yyjson_mut_obj_add_bool(doc, r, "rename", false);
    yyjson_mut_obj_add_arr(doc, r, "rows");
    yyjson_mut_val *apply = yyjson_mut_obj_add_obj(doc, r, "apply");
    add_texts(doc, apply, ITEM_TEXTS, sizeof ITEM_TEXTS / sizeof *ITEM_TEXTS);
    yyjson_mut_obj_add_bool(doc, apply, "checked", false);
    yyjson_mut_obj_add_bool(doc, apply, "disabled", false);
}

static bool put_glyph(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *name, uint32_t icon)
{
    char glyph[5];
    studio_glyph(glyph, icon);
    return ui_data_put_text(doc, obj, name, glyph);
}

static bool show_rows(yyjson_mut_doc *doc, yyjson_mut_val *array, const listed_row *rows)
{
    int had = (int)yyjson_mut_arr_size(array);
    bool changed = had != acount(rows);
    for (int i = had; i < acount(rows); i++) {
        add_texts(doc, yyjson_mut_arr_add_obj(doc, array), ROW_TEXTS, sizeof ROW_TEXTS / sizeof *ROW_TEXTS);
    }
    for (int i = had; i > acount(rows); i--) {
        yyjson_mut_arr_remove_last(array);
    }
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = 0; (item = yyjson_mut_arr_iter_next(&it)); i++) {
        changed |= put_glyph(doc, item, "icon", rows[i].icon);
        changed |= ui_data_put_text(doc, item, "label", rows[i].label);
        changed |= ui_data_put_text(doc, item, "meta", rows[i].meta);
    }
    return changed;
}

/* The title, a rename's error, its rows and the action that applies:
   Rename once the name shown was planned, or Delete. */
bool studio_refactor_sync(studio *s, const studio_panel_sync *at)
{
    studio_refactor *r = &s->refactor;
    if (!r->mode || at->preview) {
        return false;
    }
    bool rename = r->mode == STUDIO_REFACTOR_RENAME;
    bool changed = ui_data_put_text(at->doc, at->variable, "title", r->title);
    changed |= ui_data_put_text(at->doc, at->variable, "error", rename ? r->error : "");
    changed |= ui_data_put_bool(at->variable, "rename", rename);
    listed_row *rows = rows_of(r);
    changed |= show_rows(at->doc, yyjson_mut_obj_get(at->variable, "rows"), rows);
    afree(rows);
    yyjson_mut_val *apply = yyjson_mut_obj_get(at->variable, "apply");
    changed |= ui_data_put_text(at->doc, apply, "label", rename ? "Rename" : "Delete");
    changed |= put_glyph(at->doc, apply, "icon", rename ? KIT_ICON_EDIT : KIT_ICON_TRASH);
    return ui_data_put_bool(apply, "disabled", rename && !planned_now(r)) || changed;
}

/* name: the name as typed; enter: Enter in the name, which shows what a new
   name rewrites, and applies it once shown; apply: the Rename or Delete
   item. */
void studio_refactor_action(studio *s, const char *action, const ui_activation *a)
{
    studio_refactor *r = &s->refactor;
    if (!strcmp(action, "name") && a->argc && r->mode == STUDIO_REFACTOR_RENAME) {
        snprintf(r->name, sizeof r->name, "%s", a->args[0]);
    } else if (!strcmp(action, "enter") && r->mode == STUDIO_REFACTOR_RENAME) {
        if (planned_now(r)) {
            apply_rename(s);
        } else if (strcmp(r->name, r->title)) {
            plan(s);
        }
    } else if (!strcmp(action, "apply") && r->mode == STUDIO_REFACTOR_RENAME) {
        if (planned_now(r)) {
            apply_rename(s);
        }
    } else if (!strcmp(action, "apply") && r->mode == STUDIO_REFACTOR_DELETE) {
        confirm_delete(s);
    }
}

/* Shows the popover under its anchor, else at the pointer, its name field
   focused with the name selected, as the kit's popover opened. */
static void show(studio *s, rml_element *popover)
{
    studio_refactor *r = &s->refactor;
    float ratio = studio_pixel_ratio(s);
    popover_anchor at = { .side = POPOVER_POINT,
                          .box = { s->in.mouse_x * ratio, s->in.mouse_y * ratio, 0, 0 } };
    if (r->anchor[2] > 0) {
        at = (popover_anchor){ .side = POPOVER_BELOW, .keyboard = true };
        memcpy(at.box, r->anchor, sizeof at.box);
    }
    popover_show(popover, &at);
    rml_element *field = rml_element_query_selector(popover, "#" NAME_ID);
    if (field && r->mode == STUDIO_REFACTOR_RENAME) {
        rml_element_set_value(field, r->name);
        rml_element_focus(field, true);
        rml_element_select(field);
    }
}

void studio_refactor_frame(studio *s)
{
    kit *k = s->kit;
    studio_refactor *r = &s->refactor;
    if (r->undo_toast && kit_toast_action(k, r->undo_toast)) {
        r->undo_toast = 0;
        app_refactor *undo = r->undo;
        r->undo = NULL;
        app_refactor *redo = NULL;
        char error[256] = "";
        if (apply_plan(s, undo, &redo, error, sizeof error) == 0) {
            studio_report(s, "Undo");
        } else {
            refused(s, "Can't undo", error);
        }
        app_refactor_free(undo);
        app_refactor_free(redo);
    }
    /* Its document lives while it asks; a host the kit removes lets its
       mount go first. */
    if (!r->mode) {
        if (r->mounted) {
            studio_ui_panel_detach(s, "Refactor");
            r->mounted = false;
        }
        return;
    }
    kit_document_begin(k, "refactor", "bar-place", true);
    rml_element *host = studio_ui_panel(s, "refactor", "Refactor");
    kit_document_end(k);
    r->mounted = true;
    rml_element *popover = host ? rml_element_query_selector(host, "#" POPOVER_ID)
                                : NULL;
    if (popover && r->opening) {
        show(s, popover);
        r->opening = false;
    } else if (popover && r->mode && !popover_open(popover)) {
        close_state(r);
    }
}

void studio_refactor_free(studio *s)
{
    close_state(&s->refactor);
    app_refactor_free(s->refactor.undo);
    s->refactor.undo = NULL;
}
