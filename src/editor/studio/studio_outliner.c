/* The Outliner, authored and behavior-driven (projects/studio Outliner.rml
 * and OutlinerHeader.rml; docs/studio-rml-migration.md, Behavior-driven
 * panels): the scene's objects as rows with rename in place, Hide in
 * Editor, multi-selection, drops and a row menu. Its logic is its
 * prototypes' behaviors (OutlinerRows, OutlinerInput, OutlinerRename,
 * OutlinerHeaderInput), which call Studio's editor nodes (studio_nodes.c).
 * This file mounts the panel in the areas that show it and keeps the
 * capabilities those nodes and Studio's commands share: the rows the
 * object filter shows, ranges from the anchor, rename, and Hide in Editor.
 * Blender's Outliner and Unity's Hierarchy are the references. */
#include "editor/studio/studio_internal.h"
#include "support/array.h"
#include "ui/drag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void preview_move(studio *s, rml_element *host);

/* ---------- Header and body ---------- */

void studio_outliner_header(studio *s, int area)
{
    /* Visual designer (workstream H): a UI document's Hierarchy. */
    if (studio_designer_active(s)) {
        studio_designer_hierarchy_header(s, area);
        return;
    }
    rml_element *host = studio_ui_area_controls(s, area, "outliner", "OutlinerHeader");
    rml_element *field = host && s->find_request == STUDIO_EDITOR_OUTLINER + 1
                           ? rml_element_query_selector(host, "#outliner-filter")
                           : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        s->find_request = 0;
    }
}

void studio_outliner_body(studio *s, int area)
{
    /* Visual designer (workstream H): a UI document's Hierarchy. */
    if (studio_designer_active(s)) {
        studio_designer_hierarchy(s, area);
        return;
    }
    rml_element *host = studio_ui_area(s, area, "Outliner");
    if (host) {
        preview_move(s, host);
    }
}

/* ---------- Objects ---------- */

int studio_object_index(const studio *s, const char *key)
{
    uint64_t entity = key ? strtoull(key, NULL, 10) : 0;
    for (int i = 0; entity && i < acount(s->scene->actors); i++) {
        if (ecs_entity_to_u64(s->scene->actors[i].entity) == entity) {
            return i;
        }
    }
    return -1;
}

/* Actors shown by the object filter, in scene order. The caller frees the array. */
int *studio_outliner_rows(studio *s)
{
    int *rows = NULL;
    for (int i = 0; i < acount(s->scene->actors); i++) {
        const scene_actor *actor = &s->scene->actors[i];
        if (studio_matches(s->outliner_filter, actor->name) ||
            studio_matches(s->outliner_filter, actor->prototype_name)) {
            apush(rows, i);
        }
    }
    return rows;
}

void studio_reveal_selection(studio *s)
{
    const scene_actor *actor = selected_actor(s->scene);
    char key[24];
    snprintf(
        key,
        sizeof key,
        "%llu",
        actor ? (unsigned long long)ecs_entity_to_u64(actor->entity) : 0ull
    );
    studio_ui_send(s, "RevealObject", key);
}

ecs_entity studio_outliner_hovered(studio *s)
{
    int area = s->hover_area;
    if (area < 0 || studio_active_layout(s)->editors[area] != STUDIO_EDITOR_OUTLINER ||
        studio_designer_active(s)) {
        return (ecs_entity){ 0 };
    }
    rml_element *el = rml_context_hover_element(rml_backend_context(s->backend));
    for (; el; el = rml_element_parent(el)) {
        char key[24];
        if (rml_element_attribute(el, "data-object", key, sizeof key)) {
            int index = studio_object_index(s, key);
            return index >= 0 ? s->scene->actors[index].entity : (ecs_entity){ 0 };
        }
    }
    return (ecs_entity){ 0 };
}

/* ---------- Rename ---------- */

static bool editor_shown(const studio *s, studio_editor editor)
{
    if (s->maximized >= 0) {
        return s->prefs.layouts[s->prefs.workspace].editors[s->maximized] == editor;
    }
    return studio_editor_shown(s, editor);
}

void studio_rename_object(studio *s)
{
    scene_actor *actor = selected_actor(s->scene);
    bool outliner = editor_shown(s, STUDIO_EDITOR_OUTLINER);
    s->rename.entity = ecs_entity_to_u64(actor->entity);
    s->rename.outliner = outliner;
    /* The row's field focuses itself as it is built (autofocus). */
    s->rename.focus = !outliner;
    snprintf(s->rename.name, sizeof s->rename.name, "%s", actor->name);
    if (!outliner) {
        s->tab = STUDIO_TAB_OBJECT;
        studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
        return;
    }
    /* Its row shows, scrolled into view. */
    if (!studio_matches(s->outliner_filter, actor->name) &&
        !studio_matches(s->outliner_filter, actor->prototype_name)) {
        s->outliner_filter[0] = '\0';
    }
    studio_reveal_selection(s);
}

void studio_rename_commit(studio *s, uint64_t entity, const char *name)
{
    char key[24];
    snprintf(key, sizeof key, "%llu", (unsigned long long)entity);
    int index = studio_object_index(s, key);
    scene_actor *actor = index >= 0 ? &s->scene->actors[index] : NULL;
    if (!actor || !studio_editable(s) || !strcmp(actor->name, name)) {
        return;
    }
    char report[544];
    snprintf(report, sizeof report, "Renamed %s to %s", actor->name, name);
    finish_transform_edit(s->scene, false);
    if (app_scene_authoring_rename_instance(s->scene, actor->entity, name) != 0) {
        kit_toast(
            s->kit,
            0,
            &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                               .title = "Can't rename",
                               .description = s->scene->message }
        );
        return;
    }
    studio_report(s, report);
}

/* ---------- Hide in Editor ---------- */

static unsigned actor_view(studio *s, int actor)
{
    return app_scene_authoring_actor_view(s->scene, actor);
}

void studio_set_object_view(studio *s, int actor, unsigned flag, bool on)
{
    unsigned view = actor_view(s, actor) & ~flag;
    app_scene_authoring_set_actor_view(s->scene, actor, view | (on ? flag : 0));
}

void studio_hide(studio *s, bool selected)
{
    app_scene_authoring *scene = s->scene;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i) == selected) {
            studio_set_object_view(s, i, APP_SCENE_VIEW_HIDDEN, true);
        }
    }
}

void studio_show_all(studio *s)
{
    for (int i = 0; i < acount(s->scene->actors); i++) {
        studio_set_object_view(s, i, APP_SCENE_VIEW_HIDDEN, false);
    }
}

int studio_hidden_count(studio *s)
{
    int count = 0;
    for (int i = 0; i < acount(s->scene->actors); i++) {
        count += (actor_view(s, i) & APP_SCENE_VIEW_HIDDEN) != 0;
    }
    return count;
}

/* Shows its object alone, or every object once it is the only one shown,
   as Blender's Ctrl+click on the eye isolates. */
void studio_isolate(studio *s, int actor)
{
    bool alone = true;
    for (int i = 0; i < acount(s->scene->actors); i++) {
        alone &= (i == actor) != ((actor_view(s, i) & APP_SCENE_VIEW_HIDDEN) != 0);
    }
    for (int i = 0; i < acount(s->scene->actors); i++) {
        studio_set_object_view(s, i, APP_SCENE_VIEW_HIDDEN, !alone && i != actor);
    }
}

/* ---------- Multi-selection ---------- */

static int actor_by_identity(const studio *s, uint64_t identity)
{
    for (int i = 0; identity && i < acount(s->scene->actors); i++) {
        if (s->scene->actors[i].identity == identity) {
            return i;
        }
    }
    return -1;
}

/* A range extends from the anchor, which the last plain or Mod+click left:
   while the active object is the one the last range left, the anchor holds,
   else the active object becomes it (Finder's and VS Code's lists). */
void studio_select_range(studio *s, const int *rows, int count, int row)
{
    app_scene_authoring *scene = s->scene;
    scene_actor *active = selected_actor(scene);
    if (!active || active->identity != s->range.active) {
        s->range.anchor = active ? active->identity : 0;
    }
    int anchor = actor_by_identity(s, s->range.anchor), from = row;
    for (int r = 0; r < count; r++) {
        from = rows[r] == anchor ? r : from;
    }
    scene_select_range(scene, rows, from, row);
    s->range.active = scene->actors[rows[row]].identity;
}

/* ---------- Moving objects ---------- */

static app_scene_drop drop_place(const char *target, const char *zone)
{
    if (!target[0]) {
        return APP_SCENE_DROP_ROOT;
    }
    if (!strcmp(zone, "before")) {
        return APP_SCENE_DROP_BEFORE;
    }
    if (!strcmp(zone, "after")) {
        return APP_SCENE_DROP_AFTER;
    }
    return APP_SCENE_DROP_INSIDE;
}

/* A drag on a selected row carries the whole selection. An unselected
   row becomes the selection only once its press becomes a drag. */
static bool select_drag_source(studio *s, const char *source)
{
    int index = studio_object_index(s, source);
    if (index < 0 || !studio_editable(s)) {
        return false;
    }
    if (!scene_instance_selected(s->scene, index)) {
        finish_transform_edit(s->scene, false);
        select_instance(s->scene, index);
        s->range.active = 0;
    }
    return true;
}

static void move_selection(studio *s, ecs_entity target, app_scene_drop place)
{
    uint64_t before = scene_history_step_id(s->scene->history, false);
    int result = app_scene_authoring_drop_selection(s->scene, target, place);
    if (result < 0) {
        kit_toast(
            s->kit,
            0,
            &(kit_toast_desc){
                .type = KIT_TOAST_WARNING,
                .title = "Can't move objects",
                .description = s->scene->message,
            }
        );
        snprintf(s->seen_message, sizeof s->seen_message, "%s", s->scene->message);
        studio_warn(s, s->seen_message);
        return;
    }
    bool changed = scene_history_step_id(s->scene->history, false) != before;
    studio_report(
        s,
        result > 0 ? "Object move submitted"
        : changed  ? "Moved objects; world transforms preserved"
                   : "Objects are already in that position"
    );
    studio_reveal_selection(s);
}

void studio_outliner_drop(studio *s, const char *source, const char *target, const char *zone)
{
    if ((!target[0] && strcmp(zone, "root")) ||
        (target[0] && strcmp(zone, "before") && strcmp(zone, "inside") &&
         strcmp(zone, "after"))) {
        studio_report(s, "Choose a position before, inside or after an object, or the scene root");
        return;
    }
    if (!select_drag_source(s, source)) {
        return;
    }
    int index = studio_object_index(s, target);
    if (target[0] && index < 0) {
        studio_warn(s, "The drop target is no longer in the scene");
        return;
    }
    ecs_entity entity = index >= 0 ? s->scene->actors[index].entity
                                   : (ecs_entity){ 0 };
    move_selection(s, entity, drop_place(target, zone));
}

/* Shared drag handles the threshold, zones, marks and Escape. The panel
   supplies validation and a destination label, as asset assignment does. */
static void preview_move(studio *s, rml_element *host)
{
    rml_element *rejected = rml_element_query_selector(host, "[data-drop-rejected]");
    if (rejected) {
        rml_element_remove_attribute(rejected, "data-drop-rejected");
    }
    ui_drag_item item;
    rml_context *context = rml_backend_context(s->backend);
    if (!ui_drag_active(context, &item) || strcmp(item.type, "object") ||
        !select_drag_source(s, item.payload)) {
        return;
    }
    rml_element *over = rml_element_query_selector(host, "[data-drop-over]");
    if (!over || !ui_drag_over(over, &item)) {
        return;
    }
    char key[24] = "";
    rml_element_attribute(over, "data-object", key, sizeof key);
    int index = studio_object_index(s, key);
    ecs_entity target = index >= 0 ? s->scene->actors[index].entity
                                   : (ecs_entity){ 0 };
    app_scene_drop place = drop_place(key, item.zone);
    if (!app_scene_authoring_can_drop(s->scene, target, place)) {
        rml_element_set_attribute(over, "data-drop-rejected", "");
        ui_drag_hint(context, "Cannot move the selection here");
        return;
    }
    char hint[384];
    const char *relation = place == APP_SCENE_DROP_BEFORE ? "before"
                         : place == APP_SCENE_DROP_AFTER  ? "after"
                                                          : "inside";
    if (index < 0) {
        snprintf(hint, sizeof hint, "Move to scene root; preserve world transforms");
    } else {
        snprintf(
            hint,
            sizeof hint,
            "Move %s %s; preserve world transforms",
            relation,
            s->scene->actors[index].name
        );
    }
    ui_drag_hint(context, hint);
}

/* Arrow commands move top-level selected siblings together. Mixed-parent
   groups can still be dragged to an explicit destination or to the root. */
static bool keyboard_target(studio *s, studio_command command, ecs_entity *target, app_scene_drop *place)
{
    app_scene_authoring *scene = s->scene;
    if (!studio_editable(s) || studio_designer_active(s) || !selected_actor(scene)) {
        return false;
    }
    *target = (ecs_entity){ 0 };
    if (command == STUDIO_COMMAND_OBJECT_ROOT) {
        *place = APP_SCENE_DROP_ROOT;
        return true;
    }
    int first = -1, last = -1;
    ecs_entity parent = { 0 };
    for (int i = 0; i < acount(scene->actors); i++) {
        if (!scene_instance_selected(scene, i)) {
            continue;
        }
        ecs_entity p = ecs_parent(scene->world, scene->actors[i].entity);
        if (first >= 0 && ecs_entity_to_u64(parent) != ecs_entity_to_u64(p)) {
            return false;
        }
        parent = p;
        first = first < 0 ? i : first;
        last = i;
        i = scene_actor_subtree_end(scene, i) - 1;
    }
    if (command == STUDIO_COMMAND_OBJECT_OUTDENT) {
        *target = parent;
        *place = APP_SCENE_DROP_AFTER;
        return parent.slot != 0;
    }
    bool down = command == STUDIO_COMMAND_OBJECT_DOWN;
    int step = down ? 1 : -1;
    for (int i = (down ? last : first) + step; i >= 0 && i < acount(scene->actors);
         i += step) {
        if (ecs_entity_to_u64(ecs_parent(scene->world, scene->actors[i].entity)) !=
                ecs_entity_to_u64(parent) ||
            scene_instance_selected(scene, i)) {
            continue;
        }
        *target = scene->actors[i].entity;
        *place = down ? APP_SCENE_DROP_AFTER
               : command == STUDIO_COMMAND_OBJECT_INDENT ? APP_SCENE_DROP_INSIDE
                                                         : APP_SCENE_DROP_BEFORE;
        return true;
    }
    return false;
}

bool studio_outliner_move_enabled(studio *s, studio_command command)
{
    ecs_entity target;
    app_scene_drop place;
    return keyboard_target(s, command, &target, &place);
}

void studio_outliner_move(studio *s, studio_command command)
{
    ecs_entity target;
    app_scene_drop place;
    if (keyboard_target(s, command, &target, &place)) {
        move_selection(s, target, place);
    }
}
