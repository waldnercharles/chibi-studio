/* Prototype actions on the selected instance, from the menu beside its
 * prototype in Properties, the Outliner's row menu, Object and Search. Apply
 * Overrides to Prototype lists the fields the instance overrides, each
 * checked, as Unity's Overrides dropdown lists them, and says how many other
 * instances follow before it applies; Save as New Prototype names the asset
 * the tuned instance becomes, as Assets ▸ New names one. Each is one Undo
 * step, offered in its toast too. */
#include "editor/collab/collab.h"
#include "editor/documents/component_properties.h"
#include "editor/studio/studio_internal.h"
#include "editor/ui/labels.h"
#include "types/value_format.h"
#include "support/array.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPLY "prototype-apply"
#define SAVE "prototype-save"

scene_actor *studio_prototype_action_actor(studio *s)
{
    app_scene_authoring *scene = s->scene;
    scene_actor *actor = selected_actor(scene);
    bool single = actor && actor->identity && scene_selection_count(scene) == 1;
    return single && studio_editable(s) && !collab_active() &&
                   scene->document.kind == APP_EDITOR_DOCUMENT_SCENE &&
                   find_prototype(scene, actor->prototype_name)
             ? actor
             : NULL;
}

/* A free name for the new prototype: the object's, numbered from 2 when an
   asset has it. */
static void default_name(studio *s, const scene_actor *actor, char *out, size_t size)
{
    char error[192];
    snprintf(out, size, "%s", actor->name);
    for (int i = 2;
         !scene_prototype_name_free(s->scene, out, error, sizeof error) && i < 100;
         i++) {
        snprintf(out, size, "%s%d", actor->name, i);
    }
}

void studio_prototype_action_open(studio *s, bool saving)
{
    scene_actor *actor = studio_prototype_action_actor(s);
    if (!actor) {
        return;
    }
    struct studio_prototype_action *a = &s->prototype_action;
    afree(a->skipped);
    a->skipped = NULL;
    a->actor = actor->identity;
    a->saving = saving;
    a->opening = a->focus = true;
    a->shown = false;
    a->error[0] = '\0';
    app_scene_authoring_refresh_asset_index(s->scene);
    default_name(s, actor, a->name, sizeof a->name);
}

/* The Undo the last action's toast offers, while it is the step Undo
   would take. */
static void offer_undo(studio *s, const char *title, const char *description)
{
    struct studio_prototype_action *a = &s->prototype_action;
    kit_toast_dismiss(s->kit, a->toast);
    a->step = scene_history_step_id(s->scene->history, false);
    kit_toast_desc toast = { .title = title,
                             .description = description,
                             .action = "Undo",
                             .duration = 6 };
    a->toast = kit_toast(s->kit, 0, &toast);
    studio_record(s, KIT_TOAST_DEFAULT, title);
}

static void undo_toast(studio *s)
{
    struct studio_prototype_action *a = &s->prototype_action;
    if (!a->toast) {
        return;
    }
    bool current = scene_history_step_id(s->scene->history, false) == a->step &&
                   s->scene->document.kind == APP_EDITOR_DOCUMENT_SCENE;
    if (kit_toast_action(s->kit, a->toast)) {
        a->toast = 0;
        if (current && studio_editable(s)) {
            int result = replay_scene_edit(s->scene, false);
            if (!result) {
                studio_report(s, "Undo");
            } else if (result > 0) {
                studio_error(s, "Edited, but not applied", s->scene->prototype_message);
            }
        }
        return;
    }
    if (!current) {
        kit_toast_dismiss(s->kit, a->toast);
        a->toast = 0;
    }
}

static void apply(studio *s, scene_actor *actor, const prototype_field *fields, int count)
{
    char prototype[256];
    snprintf(prototype, sizeof prototype, "%s", actor->prototype_name);
    int reach = scene_override_reach(s->scene, actor, fields, count);
    int result = scene_apply_overrides(s->scene, actor, fields, count);
    if (result < 0) {
        studio_warn(s, "Could not apply the overrides");
        return;
    }
    if (result > 0) {
        offer_undo(s, "Edited prototype; application incomplete", s->scene->prototype_message);
        return;
    }
    char title[320], description[96];
    snprintf(title, sizeof title, "Applied %d %s to %s", count, count == 1 ? "override" : "overrides", prototype);
    snprintf(description, sizeof description, "%d other %s followed", reach, reach == 1 ? "instance" : "instances");
    offer_undo(s, title, reach ? description : NULL);
}

void studio_apply_field(studio *s, scene_actor *actor, prototype_field field)
{
    if (studio_prototype_action_actor(s) == actor) {
        apply(s, actor, &field, 1);
    }
}

static bool skipped(const prototype_field *list, prototype_field field)
{
    for (int i = 0; i < acount(list); i++) {
        if (list[i].component == field.component && list[i].field == field.field) {
            return true;
        }
    }
    return false;
}

/* A checkbox per overridden field, labeled as Properties labels it, with the
   instance's value. Returns the checked ones; the caller frees them. */
static prototype_field *field_rows(studio *s, scene_actor *actor, const prototype_field *fields)
{
    kit *k = s->kit;
    struct studio_prototype_action *a = &s->prototype_action;
    prototype_field *checked = NULL;
    for (int i = 0; i < acount(fields); i++) {
        const ecs_component_desc *desc = fields[i].component;
        const type_field *field = &desc->type->fields[fields[i].field];
        prototype_component component = { .desc = desc };
        char component_label[96], field_label[96], label[200], key[16];
        editor_component_label(desc->name, component_label, sizeof component_label);
        property_label(&component, field, field_label, sizeof field_label);
        snprintf(label, sizeof label, "%s \xc2\xb7 %s", component_label, field_label);
        snprintf(key, sizeof key, "f%d", i);
        pin_value value = { 0 };
        ecs_component_read_field(
            s->scene->world,
            actor->entity,
            desc->id,
            fields[i].field,
            &value
        );
        bool on = !skipped(a->skipped, fields[i]);
        kit_push(k, key, NULL, "prototype-field");
        if (kit_checkbox(k, "check", &on, label, 0) && on) {
            for (int j = 0; j < acount(a->skipped); j++) {
                if (a->skipped[j].component == desc &&
                    a->skipped[j].field == fields[i].field) {
                    adel(a->skipped, j);
                    break;
                }
            }
        } else if (!on && !skipped(a->skipped, fields[i])) {
            apush(a->skipped, fields[i]);
        }
        kit_label(k, "value", "kbd", pin_value_to_string(&value));
        kit_pop(k);
        if (on) {
            apush(checked, fields[i]);
        }
    }
    return checked;
}

static void apply_popover(studio *s)
{
    kit *k = s->kit;
    if (!kit_popup_begin(k, APPLY, "menu prototype-apply")) {
        afree(s->prototype_action.skipped);
        s->prototype_action.skipped = NULL;
        return;
    }
    scene_actor *actor = studio_prototype_action_actor(s);
    if (!actor || actor->identity != s->prototype_action.actor) {
        kit_popup_close(k, APPLY);
        kit_popup_end(k);
        return;
    }
    char text[320];
    snprintf(text, sizeof text, "Apply to %s", actor->prototype_name);
    kit_menu_group(k, "title", text);
    prototype_field *fields = prototype_instance_overrides(
        actor->base,
        s->scene->world,
        actor->entity
    );
    prototype_field *checked = field_rows(s, actor, fields);
    int count = acount(checked),
        reach = scene_override_reach(s->scene, actor, checked, count);
    /* What applying does is a note under the fields, not a row that looks
       like an action. */
    if (!acount(fields)) {
        kit_label(k, "reach", "menu-note", "Matches its prototype");
    } else if (reach) {
        snprintf(text, sizeof text, "%d other %s will follow", reach, reach == 1 ? "instance" : "instances");
        kit_label(k, "reach", "menu-note", text);
    } else {
        kit_label(k, "reach", "menu-note", "No other instance inherits these");
    }
    kit_menu_separator(k, "sep");
    snprintf(text, sizeof text, count == 1 ? "Apply %d Override" : "Apply %d Overrides", count);
    kit_menu_item_desc go = { .label = text,
                              .icon = KIT_ICON_PROTOTYPE,
                              .checked = -1,
                              .disabled = !count };
    bool chosen = kit_menu_item(k, "apply", &go);
    kit_popup_end(k);
    if (chosen) {
        apply(s, actor, checked, count);
    }
    afree(fields);
    afree(checked);
}

static bool enter_pressed(const studio *s)
{
    for (int i = 0; i < acount(s->keys); i++) {
        if ((s->keys[i].key == SDLK_RETURN || s->keys[i].key == SDLK_KP_ENTER) &&
            !s->keys[i].repeat) {
            return true;
        }
    }
    return false;
}

static void save_actor(studio *s, scene_actor *actor)
{
    struct studio_prototype_action *a = &s->prototype_action;
    char from[256];
    snprintf(from, sizeof from, "%s", actor->prototype_name);
    if (scene_save_as_prototype(s->scene, actor, a->name, a->error, sizeof a->error) != 0) {
        a->opening = a->focus = true;
        return;
    }
    kit_popup_close(s->kit, SAVE);
    char title[320], description[320];
    snprintf(title, sizeof title, "Saved %s as a new prototype", a->name);
    snprintf(description, sizeof description, "Undo makes it %s again; %s stays in Assets", from, a->name);
    offer_undo(s, title, description);
}

static void save_popover(studio *s)
{
    kit *k = s->kit;
    struct studio_prototype_action *a = &s->prototype_action;
    if (!kit_popup_begin(k, SAVE, "menu new-asset")) {
        a->shown = false;
        return;
    }
    scene_actor *actor = studio_prototype_action_actor(s);
    if (!actor || actor->identity != a->actor) {
        kit_popup_close(k, SAVE);
        kit_popup_end(k);
        return;
    }
    kit_menu_group(k, "title", "Save as New Prototype");
    kit_push(k, "field", NULL, "new-asset-field");
    kit_text_field(k, "name", a->name, sizeof a->name, "Name", 0);
    if (a->focus) {
        kit_focus(k);
        a->focus = false;
    }
    kit_pop(k);
    if (a->error[0]) {
        kit_push(k, "error", NULL, "new-asset-error");
        kit_icon(k, "icon", NULL, KIT_ICON_WARNING);
        kit_label(k, "label", "label", a->error);
        kit_pop(k);
    }
    kit_menu_separator(k, "sep");
    kit_menu_item_desc go = { .label = "Save", .icon = KIT_ICON_PROTOTYPE, .checked = -1 };
    bool chosen = kit_menu_item(k, "save", &go);
    /* The Enter that ran the command opened the popover and is not for it. */
    bool entered = a->shown && enter_pressed(s);
    kit_popup_end(k);
    a->shown = true;
    if (chosen || entered) {
        save_actor(s, actor);
    }
}

void studio_prototype_actions_frame(studio *s)
{
    struct studio_prototype_action *a = &s->prototype_action;
    if (a->opening) {
        kit_popup_open_at(s->kit, a->saving ? SAVE : APPLY, s->in.mouse_x, s->in.mouse_y);
        a->opening = false;
    }
    apply_popover(s);
    save_popover(s);
    undo_toast(s);
}
