/* Typed asset assignment by drag, as Unity's object fields and Scene view
 * take a dragged asset and Godot's inspector marks the resource pickers that
 * accept one. An asset dragged from Assets marks every Properties field of
 * its kind and every Outliner row whose object has a field of its kind as a
 * target (the Scene takes it on such an object). Over a target the asset
 * shows in place: in the field, and on the object in the Scene, outlined.
 * Release assigns it as one Undo step; Escape, or a release anywhere else,
 * leaves everything as it was, since the preview never left the draw. */
#include "editor/studio/studio_internal.h"

#include <stdio.h>
#include <string.h>

/* The kinds an object's fields name (reference_named_field_kind). A
   prototype dropped on the Scene or the Outliner places one instead. */
static const reference_asset_kind ASSIGNED[] = {
    REFERENCE_ASSET_MATERIAL, REFERENCE_ASSET_SHAPE,  REFERENCE_ASSET_UI,
    REFERENCE_ASSET_FIELD,    REFERENCE_ASSET_SYSTEM,
};
#define ASSIGNED_COUNT ((int)(sizeof ASSIGNED / sizeof *ASSIGNED))

bool studio_asset_draggable(const reference_asset *asset)
{
    if (asset->unresolved) {
        return false;
    }
    if (asset->kind == REFERENCE_ASSET_PROTOTYPE ||
        asset->kind == REFERENCE_ASSET_BEHAVIOR) {
        return true;
    }
    for (int i = 0; i < ASSIGNED_COUNT; i++) {
        if (asset->kind == ASSIGNED[i]) {
            return true;
        }
    }
    return false;
}

/* An asset of the kind, as a hint names it. */
static const char *one_of(int kind)
{
    switch (kind) {
        case REFERENCE_ASSET_MATERIAL: return "a material";
        case REFERENCE_ASSET_SHAPE: return "a shape";
        case REFERENCE_ASSET_UI: return "a UI document";
        case REFERENCE_ASSET_FIELD: return "a field";
        case REFERENCE_ASSET_SYSTEM: return "a system";
        default: return "a behavior";
    }
}

static int kind_named(const char *type)
{
    for (int kind = 0; kind < REFERENCE_ASSET_KIND_COUNT; kind++) {
        if (!strcmp(studio_asset_kind_name(kind), type)) {
            return kind;
        }
    }
    return -1;
}

void studio_drop_frame(studio *s)
{
    /* A preview the Scene did not draw, as when no area shows it, goes. */
    scene_field_previews_clear(s->scene);
    if (!kit_dragging(s->kit, NULL)) {
        s->drop.key[0] = '\0';
    }
}

const char *studio_drop_shown(const studio *s, const char *key)
{
    return s->drop.key[0] && !strcmp(s->drop.key, key) && kit_dragging(s->kit, NULL)
             ? s->drop.name
             : NULL;
}

const char *studio_drop_picker(studio *s, const char *key, int kind)
{
    kit *k = s->kit;
    char type[32];
    if (kit_drop_target(
            k,
            studio_asset_kind_name(kind),
            s->drop.landed,
            sizeof s->drop.landed
        )) {
        s->drop.key[0] = '\0';
        return s->drop.landed;
    }
    if (kit_drop_hover(k, type, sizeof type, s->drop.name, sizeof s->drop.name)) {
        snprintf(s->drop.key, sizeof s->drop.key, "%s", key);
        kit_drag_hint(k, "Assign");
    } else if (!strcmp(s->drop.key, key)) {
        s->drop.key[0] = '\0';
    }
    return NULL;
}

/* Sets the field naming the asset, as Properties sets it: one Undo step. */
static void assign(studio *s, scene_actor *actor, int kind, const char *name)
{
    ecs_component_id component;
    int field = scene_asset_field(s->scene, actor->entity, kind, &component);
    if (field < 0) {
        return;
    }
    studio_set_field(s, actor, component, field, name);
    char message[320];
    snprintf(message, sizeof message, "Assigned %s to %s", name, actor->name);
    studio_report(s, message);
}

void studio_attach_behavior(studio *s, int index, const char *name)
{
    app_scene_authoring *scene = s->scene;
    finish_transform_edit(scene, false);
    select_instance(scene, index);
    if (app_scene_authoring_set_instance_behavior(scene, name, true) != 0) {
        return;
    }
    char message[320];
    snprintf(
        message,
        sizeof message,
        "Attached %s to %s",
        name,
        scene->actors[index].name
    );
    studio_report(s, message);
}

/* While an asset of `kind` hovers the object: it shows there, and the
   overlay says what a drop does. False when no field takes it. */
static bool preview(studio *s, scene_actor *actor, int kind, const char *name)
{
    char hint[320];
    if (kind == REFERENCE_ASSET_BEHAVIOR) {
        snprintf(hint, sizeof hint, "Attach to %s", actor->name);
        kit_drag_hint(s->kit, hint);
        return true;
    }
    ecs_component_id component;
    int field = scene_asset_field(s->scene, actor->entity, kind, &component);
    if (field < 0) {
        return false;
    }
    scene_preview_field(s->scene, actor->entity, component, field, name);
    snprintf(hint, sizeof hint, "Assign to %s", actor->name);
    kit_drag_hint(s->kit, hint);
    return true;
}

void studio_drop_types(studio *s, int index, char *out, size_t size)
{
    out[0] = '\0';
    if (!studio_editable(s)) {
        return;
    }
    snprintf(out, size, "%s", studio_asset_kind_name(REFERENCE_ASSET_BEHAVIOR));
    ecs_entity entity = s->scene->actors[index].entity;
    for (int i = 0; i < ASSIGNED_COUNT; i++) {
        ecs_component_id component;
        if (scene_asset_field(s->scene, entity, ASSIGNED[i], &component) >= 0) {
            size_t used = strlen(out);
            snprintf(out + used, size - used, " %s", studio_asset_kind_name(ASSIGNED[i]));
        }
    }
}

void studio_drop_preview(studio *s, int index, const char *type, const char *name)
{
    preview(s, &s->scene->actors[index], kind_named(type), name);
}

void studio_drop_on(studio *s, int index, const char *type, const char *name)
{
    int kind = kind_named(type);
    if (kind == REFERENCE_ASSET_BEHAVIOR) {
        studio_attach_behavior(s, index, name);
        return;
    }
    assign(s, &s->scene->actors[index], kind, name);
}

/* The authored object under the pointer, or -1. */
static int pointed_actor(studio *s)
{
    ecs_entity picked = scene_pick_at_pointer(s->scene, s->viewport);
    for (int i = 0; picked.slot && i < acount(s->scene->actors); i++) {
        if (ecs_entity_to_u64(s->scene->actors[i].entity) == ecs_entity_to_u64(picked)) {
            return i;
        }
    }
    return -1;
}

void studio_drop_scene(studio *s)
{
    if (!studio_editable(s)) {
        return;
    }
    kit *k = s->kit;
    char name[256], type[32];
    int kinds[ASSIGNED_COUNT + 1] = { REFERENCE_ASSET_BEHAVIOR };
    memcpy(kinds + 1, ASSIGNED, sizeof ASSIGNED);
    for (int i = 0; i < ASSIGNED_COUNT + 1; i++) {
        if (!kit_drop_target(k, studio_asset_kind_name(kinds[i]), name, sizeof name)) {
            continue;
        }
        int actor = pointed_actor(s);
        if (actor >= 0 && kinds[i] == REFERENCE_ASSET_BEHAVIOR) {
            studio_attach_behavior(s, actor, name);
        } else if (actor >= 0) {
            assign(s, &s->scene->actors[actor], kinds[i], name);
        }
    }
    if (!kit_drop_hover(k, type, sizeof type, name, sizeof name)) {
        return;
    }
    int kind = kind_named(type);
    if (kind == REFERENCE_ASSET_PROTOTYPE) {
        kit_drag_hint(k, "Place here");
        return;
    }
    int actor = pointed_actor(s);
    if (actor < 0 || !preview(s, &s->scene->actors[actor], kind, name)) {
        char hint[160];
        snprintf(hint, sizeof hint, "Drop on an object that takes %s", one_of(kind));
        kit_drag_hint(k, hint);
    }
}
