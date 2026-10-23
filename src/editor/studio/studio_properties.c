#include "authoring/source_asset.h"
#include "editor/documents/prototype_source_input.h"
/* Studio Properties: its model and intents. The panel is authored
 * (Properties.rml and PropertiesHeader.rml over the `properties` variable,
 * studio_properties_view.c); it sends intents (emit('properties.<action>',
 * ...)) that this file runs: writes to reflected fields, one Undo step per
 * gesture, Reset and Apply to Prototype, Keep Changes, components added and
 * removed, behaviors attached, and renames. The Node and Member tabs stay
 * native in its #native slot (workstream G1). */
#include "editor/collab/actions_scene.h"
#include "editor/documents/component_properties.h"
#include "editor/studio/studio_properties_internal.h"
#include "editor/ui/labels.h"
#include "graph/model/registry.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "types/text.h"
#include "types/value_format.h"
#include "ui/popover.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Tabs ---------- */

static const uint32_t TAB_ICONS[STUDIO_TAB_COUNT] = {
    KIT_ICON_SETTINGS, KIT_ICON_PROTOTYPE, KIT_ICON_PHYSICS, KIT_ICON_MATERIAL,
    KIT_ICON_GAMEPLAY, KIT_ICON_GRAPH,     KIT_ICON_HEXAGON, KIT_ICON_VARIABLE,
};

uint32_t properties_tab_icon(studio_tab tab)
{
    return TAB_ICONS[tab];
}

const char *properties_tab_label(studio_tab tab)
{
    switch (tab) {
        case STUDIO_TAB_SCENE: return "Scene";
        case STUDIO_TAB_BEHAVIORS: return "Behaviors";
        case STUDIO_TAB_NODE: return "Node";
        case STUDIO_TAB_MEMBER: return "Member";
        default: return COMPONENT_TAB_NAMES[tab - STUDIO_TAB_OBJECT];
    }
}

/* editor/Components.chibi names a component's tab; the rest belong to Object. */
studio_tab properties_tab_of(const studio *s, const ecs_component_desc *desc)
{
    return (studio_tab)(STUDIO_TAB_OBJECT +
                        component_presentation_tab(&s->presentation.hints, desc));
}

const char *properties_panel_key(const char *component)
{
    const char *dot = strrchr(component, '.');
    return dot ? dot + 1 : component;
}

/* ---------- Panels folded, as Unity remembers a component's foldout ---------- */

static studio_fold *fold_of(const studio *s, const char *key)
{
    for (int i = 0; i < acount(s->properties.folds); i++) {
        if (!strcmp(s->properties.folds[i].key, key)) {
            return &s->properties.folds[i];
        }
    }
    return NULL;
}

static bool open_first(const studio *s, const char *key)
{
    static const char *const FIRST[] = { "Transform",   "Appearance",
                                         "Orientation", "behaviors",
                                         "ui",          "physics",
                                         "parameters" };
    for (size_t i = 0; i < sizeof FIRST / sizeof *FIRST; i++) {
        if (!strcmp(FIRST[i], key)) {
            return true;
        }
    }
    return !strcmp(properties_panel_key(s->components.added), key);
}

bool properties_open(const studio *s, const char *key)
{
    const studio_fold *fold = fold_of(s, key);
    return fold ? fold->open : s->property_search[0] || open_first(s, key);
}

static void fold(studio *s, const char *key)
{
    studio_fold *fold = fold_of(s, key);
    if (!fold) {
        studio_fold added = { .open = properties_open(s, key) };
        snprintf(added.key, sizeof added.key, "%s", key);
        apush(s->properties.folds, added);
        fold = &alast(s->properties.folds);
    }
    fold->open = !fold->open;
}

/* ---------- What Properties shows ---------- */

/* An object the running scene spawned shows as an instance of its prototype;
   its values edit live, and it has no authored state to keep. */
static scene_actor *spawned_actor(app_scene_authoring *scene, scene_actor *out)
{
    ecs_entity entity = selected_scene_entity(scene);
    if (!scene->runtime_selected.slot || !entity.slot) {
        return NULL;
    }
    /* Borrowed for this frame; nothing writes through it. */
    char *name = (char *)runtime_entity_label(scene, entity);
    scene_prototype_asset *asset = find_prototype(scene, name);
    *out = (scene_actor){ .entity = entity,
                          .name = name,
                          .prototype_name = name,
                          .base = asset ? asset->prototype : NULL };
    return out;
}

void properties_subject_of(studio *s, properties_subject *out)
{
    app_scene_authoring *scene = s->scene;
    bool prototype = app_scene_authoring_document(scene).kind ==
                     APP_EDITOR_DOCUMENT_PROTOTYPE;
    *out = (properties_subject){ .asset = prototype ? selected_prototype(scene) : NULL,
                                 .locked = !studio_editable(s) };
    if (out->asset && scene_prototype_source_owned(out->asset)) {
        char error[256];
        asset_code *code = scene_prototype_source_session(scene, out->asset, error, sizeof error);
        if (!code ||
            scene_prototype_source_sync(scene, out->asset, !asset_code_runtime_held(code)) <
                0) {
            out->asset = NULL;
            out->locked = true;
        }
    }
    out->actor = prototype ? NULL : selected_actor(scene);
    if (!out->actor && !prototype) {
        out->actor = spawned_actor(scene, &out->spawned);
    }
    out->multi = out->actor && properties_multi(s, out->actor);
}

static bool shared_component(studio *s, ecs_component_id component)
{
    app_scene_authoring *scene = s->scene;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i) &&
            !ecs_component_has(scene->world, scene->actors[i].entity, component)) {
            return false;
        }
    }
    return true;
}

static studio_tab prototype_default_tab(const studio *s, const scene_prototype_asset *asset)
{
    const prototype *prototype = asset->prototype;
    for (int i = 0; i < acount(prototype->components); i++) {
        if (properties_tab_of(s, prototype->components[i].desc) == STUDIO_TAB_OBJECT) {
            return STUDIO_TAB_OBJECT;
        }
    }
    return acount(prototype->components)
             ? properties_tab_of(s, prototype->components[0].desc)
             : STUDIO_TAB_OBJECT;
}

void properties_follow_tabs(studio *s, const properties_subject *subject, bool available[STUDIO_TAB_COUNT])
{
    app_scene_authoring *scene = s->scene;
    memset(available, 0, STUDIO_TAB_COUNT * sizeof *available);
    available[STUDIO_TAB_SCENE] = subject->asset == NULL;
    if (subject->asset && subject->asset->prototype) {
        available[STUDIO_TAB_OBJECT] = true;
        for (int i = 0; i < acount(subject->asset->prototype->components); i++) {
            available
                [properties_tab_of(s, subject->asset->prototype->components[i].desc)] = true;
        }
    } else if (subject->actor) {
        available[STUDIO_TAB_OBJECT] = true;
        available[STUDIO_TAB_BEHAVIORS] = !subject->multi;
        for (int i = 0; i < scene->registry.component_count; i++) {
            const ecs_component_desc *desc = scene->registry.components[i];
            if (ecs_component_has(scene->world, subject->actor->entity, desc->id) &&
                (!subject->multi || shared_component(s, desc->id))) {
                available[properties_tab_of(s, desc)] = true;
            }
        }
    }
    /* The Node tab follows the graph selection, as Object follows the
       scene's: a newly selected node shows its details, a newly selected
       object its components; Member follows the Members selection. */
    uint64_t member = studio_member_shown(s);
    uint64_t node = studio_graph_node(s) ? studio_graph_node(s)
                                         : studio_graph_comment(s);
    app_editor_document_kind kind = app_scene_authoring_document(scene).kind;
    bool graph = kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
                 kind == APP_EDITOR_DOCUMENT_SYSTEM;
    bool model_document = graph && s->scene->graph_editor &&
                          editor_behavior_asset(s->scene->graph_editor)->domain ==
                              BEHAVIOR_DOMAIN_MODEL;
    available[STUDIO_TAB_NODE] = graph && (node != 0 || model_document);
    if (node && node != s->shown_node && !(s->tab == STUDIO_TAB_MEMBER && member)) {
        s->tab = STUDIO_TAB_NODE;
    } else if (
        s->tab == STUDIO_TAB_NODE && scene->selected != s->shown_actor &&
        subject->actor
    ) {
        s->tab = STUDIO_TAB_OBJECT;
    } else if (s->tab == STUDIO_TAB_SCENE && s->shown_actor < 0 && scene->selected >= 0) {
        /* A selection made from none, as a marquee makes one, shows it. */
        s->tab = STUDIO_TAB_OBJECT;
    }
    available[STUDIO_TAB_MEMBER] = graph && member != 0;
    if (member && member != s->shown_member) {
        s->tab = STUDIO_TAB_MEMBER;
    }
    s->shown_member = member;
    s->shown_node = node;
    s->shown_actor = scene->selected;
    if (!available[s->tab]) {
        s->tab = subject->asset ? prototype_default_tab(s, subject->asset)
               : subject->actor ? STUDIO_TAB_OBJECT
                                : STUDIO_TAB_SCENE;
    }
}

/* ---------- Rows ---------- */

static const char *const SCENE_SETTINGS[SCENE_SETTING_COUNT] = {
    "workers",     "substeps", "sleep",   "continuous",
    "force_drive", "hertz",    "damping", "ui",
};

void properties_row_id(const properties_row *row, char out[PROPERTIES_ROW_ID])
{
    switch (row->kind) {
        case PROPERTIES_OBJECT:
            snprintf(
                out,
                PROPERTIES_ROW_ID,
                "o%" PRIu64 ".%" PRIu64 ".%d",
                row->entity,
                row->component,
                row->field
            );
            return;
        case PROPERTIES_PROTOTYPE:
            snprintf(out, PROPERTIES_ROW_ID, "p%" PRIu64 ".%d.%s", row->component, row->field, row->prototype);
            return;
        case PROPERTIES_PARAMETER:
            snprintf(out, PROPERTIES_ROW_ID, "q.%zu.%" PRIx64, row->parameter_index, row->parameter_identity);
            return;
        case PROPERTIES_SCENE:
            snprintf(out, PROPERTIES_ROW_ID, "s.%s", SCENE_SETTINGS[row->field]);
            return;
    }
}

bool properties_row_parse(const char *id, properties_row *out)
{
    *out = (properties_row){ 0 };
    int used = 0;
    if (id[0] == 'o') {
        out->kind = PROPERTIES_OBJECT;
        return sscanf(id, "o%" SCNu64 ".%" SCNu64 ".%d%n", &out->entity, &out->component, &out->field, &used) ==
                   3 &&
               !id[used];
    }
    if (id[0] == 'p') {
        out->kind = PROPERTIES_PROTOTYPE;
        if (sscanf(id, "p%" SCNu64 ".%d.%n", &out->component, &out->field, &used) != 2 ||
            !used) {
            return false;
        }
        snprintf(out->prototype, sizeof out->prototype, "%s", id + used);
        return true;
    }
    if (!strncmp(id, "q.", 2)) {
        out->kind = PROPERTIES_PARAMETER;
        return sscanf(id, "q.%zu.%" SCNx64 "%n", &out->parameter_index, &out->parameter_identity, &used) ==
                   2 &&
               !id[used];
    }
    out->kind = PROPERTIES_SCENE;
    for (int i = 0; i < SCENE_SETTING_COUNT; i++) {
        if (!strncmp(id, "s.", 2) && !strcmp(id + 2, SCENE_SETTINGS[i])) {
            out->field = i;
            return true;
        }
    }
    return false;
}

/* ---------- Reflection ---------- */

const ecs_component_desc *properties_component(const app_scene_authoring *scene, ecs_component_id id)
{
    for (int i = 0; i < scene->registry.component_count; i++) {
        if (scene->registry.components[i]->id == id) {
            return scene->registry.components[i];
        }
    }
    return NULL;
}

const prototype_component *properties_base(
    const scene_actor *actor,
    const ecs_component_desc *desc,
    prototype_component *added
)
{
    *added = (prototype_component){ .desc = desc };
    for (int j = 0; actor->base && j < acount(actor->base->components); j++) {
        if (actor->base->components[j].desc->id == desc->id) {
            return &actor->base->components[j];
        }
    }
    return added;
}

bool properties_overridden(
    const app_scene_authoring *scene,
    const scene_actor *actor,
    const prototype_component *component,
    int field
)
{
    arena storage = { 0 };
    pin_value value = { 0 }, inherited = { 0 };
    if (ecs_component_read_field(
            &storage,
            scene->world,
            actor->entity,
            component->desc->id,
            field,
            &value
        ) != ECS_OK) {
        arena_free(&storage);
        return false;
    }
    inherited = value;
    if (component->data) {
        if (pin_read_field(&storage, component->data, &component->desc->type->fields[field], &inherited)) {
            arena_free(&storage);
            return false;
        }
    }
    bool overridden = !pin_value_equal(&value, &inherited);
    arena_free(&storage);
    return overridden;
}

const type_desc *properties_vector(const pin_value *value)
{
    if (value->type != PIN_STRUCT || value->container != PIN_CONTAINER_SINGLE ||
        !value->ptr) {
        return NULL;
    }
    const type_desc *type = type_catalog_find(value->type_name);
    if (!type || type->field_count < 2 || type->field_count > 4 ||
        !type_is_plain(type)) {
        return NULL;
    }
    static const char *const axes[] = { "x", "y", "z", "w" };
    for (int i = 0; i < type->field_count; i++) {
        const type_field *axis = &type->fields[i];
        if (!axis->name || strcmp(axis->name, axes[i]) ||
            (axis->type != PIN_F32 && axis->type != PIN_F64)) {
            return NULL;
        }
    }
    return type;
}

reference_asset_kind properties_named(
    const ecs_component_desc *desc,
    const type_field *field,
    const pin_value *value
)
{
    bool text = value->type == PIN_STR || value->type == PIN_NAME;
    return text ? reference_named_field_kind(desc->type->name, field->name)
                : REFERENCE_ASSET_KIND_COUNT;
}

bool properties_label(
    studio *s,
    const prototype_component *component,
    const type_field *field,
    char *label,
    size_t size
)
{
    property_label(component, field, label, size);
    if (!strcmp(field->name, "radians")) {
        snprintf(label, size, "Rotation");
    }
    return studio_matches(s->property_search, label);
}

const char *properties_while_playing(const studio *s, bool locked, const char *reason)
{
    return locked && runtime_session(s->scene) ? reason : NULL;
}

bool studio_properties_live(studio *s)
{
    return studio_live(s) && !studio_designer_active(s) &&
           s->scene->document.kind != APP_EDITOR_DOCUMENT_PROTOTYPE;
}

/* ---------- Several objects, as Unity edits them ---------- */

/* With several objects selected, Properties shows the components they all
   have, a value they differ in as mixed, and writes a change to each of
   them as one Undo step. */
bool properties_multi(studio *s, const scene_actor *actor)
{
    return actor == selected_actor(s->scene) && scene_selection_count(s->scene) > 1;
}

int *properties_others(studio *s, ecs_component_id component)
{
    s->panel_work.properties_mixed_lists++;
    app_scene_authoring *scene = s->scene;
    int *others = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        if (i != scene->selected && scene_instance_selected(scene, i) &&
            ecs_component_has(scene->world, scene->actors[i].entity, component)) {
            apush(others, i);
        }
    }
    return others;
}

static bool same_path(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

bool properties_mixed(
    studio *s,
    const scene_actor *actor,
    ecs_component_id component,
    int field,
    const pin_value *value
)
{
    if (!properties_multi(s, actor)) {
        return false;
    }
    app_scene_authoring *scene = s->scene;
    /* Objects naming different paths differ though none resolves. */
    arena storage = { 0 };
    const char *path = scene_link_path(actor->paths, component, field);
    for (int i = 0; i < acount(scene->actors); i++) {
        if (i == scene->selected || !scene_instance_selected(scene, i) ||
            !ecs_component_has(scene->world, scene->actors[i].entity, component)) {
            continue;
        }
        const scene_actor *each = &scene->actors[i];
        const char *named = scene_link_path(each->paths, component, field);
        pin_value other = { 0 };
        if ((ecs_component_read_field(&storage, scene->world, each->entity, component, field, &other) ==
                 ECS_OK &&
             !pin_value_equal(&other, value)) ||
            !same_path(path, named)) {
            arena_free(&storage);
            return true;
        }
    }
    arena_free(&storage);
    return false;
}

/* ---------- Writes ---------- */

/* A field's change in Properties, one input of a gesture. */
typedef struct
{
    ecs_component_id component;
    int field;
    pin_value before, value;
    bool active;    /* the gesture goes on: input, not change */
    bool selection; /* the other selected objects take the change too */
} field_change;

/* Writes `c` to `entity`, whose field is `current`: a vector takes only the
   axes the change moved, so each object keeps its others. */
static void write_merged(ecs_world *world, ecs_entity entity, const field_change *c, pin_value current)
{
    const type_desc *type = c->value.type == PIN_STRUCT
                              ? type_catalog_find(c->value.type_name)
                              : NULL;
    if (!type || !current.ptr || !c->before.ptr) {
        ecs_component_write_field(world, entity, c->component, c->field, c->value);
        return;
    }
    arena storage = { 0 };
    void *merged = xmalloc(type->size);
    memcpy(merged, current.ptr, type->size);
    for (int i = 0; i < type->field_count; i++) {
        pin_value before, after;
        if (pin_read_field(&storage, c->before.ptr, &type->fields[i], &before) ||
            pin_read_field(&storage, c->value.ptr, &type->fields[i], &after)) {
            free(merged);
            arena_free(&storage);
            return;
        }
        if (!pin_value_equal(&before, &after)) {
            if (pin_write_field(&storage, merged, &type->fields[i], after)) {
                free(merged);
                arena_free(&storage);
                return;
            }
        }
    }
    pin_value value = { .type = PIN_STRUCT, .type_name = type->name, .ptr = merged };
    ecs_component_write_field(world, entity, c->component, c->field, value);
    free(merged);
    arena_free(&storage);
}

/* The change on the other selected objects; `record` keeps each one's value
   before the gesture, which finish_property_edit records with it. */
static void write_others(studio *s, const field_change *c, bool record)
{
    app_scene_authoring *scene = s->scene;
    arena storage = { 0 };
    int *others = properties_others(s, c->component);
    for (int i = 0; i < acount(others); i++) {
        ecs_entity entity = scene->actors[others[i]].entity;
        pin_value current = { 0 };
        if (ecs_component_read_field(&storage, scene->world, entity, c->component, c->field, &current) !=
            ECS_OK) {
            continue;
        }
        bool tracked = false;
        for (int t = 0; t < acount(scene->property_others) && !tracked; t++) {
            tracked = ecs_entity_to_u64(scene->property_others[t].entity) ==
                      ecs_entity_to_u64(entity);
        }
        if (record && !tracked) {
            scene_field_target target = { .entity = entity };
            bool copied = scene_property_value_copy(&target.before, current);
            assert(copied);
            apush(scene->property_others, target);
        }
        write_merged(scene->world, entity, c, current);
    }
    afree(others);
    arena_free(&storage);
}

/* One Undo step per gesture: the first input opens an edit, the change
   records it. While the scene runs, a change writes only its running world. */
static void write_field(studio *s, scene_actor *actor, field_change c)
{
    app_scene_authoring *scene = s->scene;
    if (!editable_scalar(&c.value)) {
        return;
    }
    if (studio_live(s)) {
        ecs_component_write_field(scene->world, actor->entity, c.component, c.field, c.value);
        if (c.selection) {
            write_others(s, &c, false);
        }
        return;
    }
    bool pending = scene->property_actor.slot &&
                   ecs_entity_to_u64(scene->property_actor) ==
                       ecs_entity_to_u64(actor->entity) &&
                   scene->property_edit.component == c.component &&
                   scene->property_edit.field == c.field;
    if (!pending) {
        finish_transform_edit(scene, false);
        scene->property_actor = actor->entity;
        scene->property_edit = (scene_field_edit){ .component = c.component,
                                                   .field = c.field };
        if (!scene_property_value_copy(&scene->property_edit.before, c.before) ||
            !scene_property_value_copy(&scene->property_edit.after, c.before)) {
            abort();
        }
        scene->property_dirty = scene->scene_dirty;
    }
    if (scene_collab_can_write_locally()) {
        if (ecs_component_write_field(scene->world, actor->entity, c.component, c.field, c.value) ==
            ECS_OK) {
            arena storage = { 0 };
            pin_value written = { 0 };
            if (ecs_component_read_field(&storage, scene->world, actor->entity, c.component, c.field, &written) !=
                ECS_OK) {
                abort();
            }
            scene_property_value_free(&scene->property_edit.after);
            if (!scene_property_value_copy(&scene->property_edit.after, written)) {
                abort();
            }
            arena_free(&storage);
            scene->scene_dirty = true;
            asset_index_invalidate(scene);
        }
        if (c.selection) {
            write_others(s, &c, true);
        }
    } else if (scene_collab_is_editor()) {
        scene_property_value_free(&scene->property_edit.after);
        if (!scene_property_value_copy(&scene->property_edit.after, c.value)) {
            abort();
        }
    }
    if (!c.active) {
        finish_property_edit(scene);
    }
}

void studio_set_field(studio *s, scene_actor *actor, ecs_component_id component, int field, const char *text)
{
    arena storage = { 0 };
    pin_value before = { 0 };
    if (ecs_component_read_field(&storage, s->scene->world, actor->entity, component, field, &before) !=
        ECS_OK) {
        arena_free(&storage);
        return;
    }
    pin_value value = before;
    if (value.type == PIN_STR) {
        const char *type_name = value.type_name;
        const char *error = pin_value_text(&storage, &value, text, strlen(text));
        assert(!error);
        (void)error;
        value.type_name = type_name;
    } else {
        value.str = text;
    }
    write_field(
        s,
        actor,
        (field_change){ .component = component, .field = field, .before = before, .value = value }
    );
    arena_free(&storage);
}

/* Moves an object by one axis of its Position, as the Scene's drag does: a
   transform edit, one Undo step when the gesture ends, Escape putting it
   back. Several selected objects take the axis as a field. */
static void move(studio *s, scene_actor *actor, int axis, double v, bool active, bool canceled)
{
    app_scene_authoring *scene = s->scene;
    const Transform *position = Transform_get_const(scene->world, actor->entity);
    if (properties_multi(s, actor)) {
        write_field(
            s,
            actor,
            (field_change){ .component = COMPONENT_Transform.id,
                            .field = axis,
                            .before = { .type = PIN_F32,
                                        .f = axis ? position->y : position->x },
                            .value = { .type = PIN_F32, .f = (float)v },
                            .active = active,
                            .selection = true }
        );
        return;
    }
    Transform after = *position;
    if (axis) {
        after.y = (float)v;
    } else {
        after.x = (float)v;
    }
    if (!scene->edit_actor.slot || ecs_entity_to_u64(scene->edit_actor) !=
                                       ecs_entity_to_u64(actor->entity)) {
        begin_transform_edit(scene, actor->entity);
    }
    write_transform_edit(scene, actor->entity, after);
    if (scene_collab_can_write_locally() && !studio_live(s)) {
        scene->scene_dirty = true;
    }
    if (!active && !scene->dragging_actor) {
        finish_transform_edit(scene, canceled);
    }
}

/* ---------- Values from the panel ---------- */

bool properties_angle(const type_field *field)
{
    const char *unit = field->unit;
    return !strcmp(field->name, "radians") ||
           (unit && !strncmp(unit, "rad", 3) && (!unit[3] || unit[3] == '/'));
}

static bool parse_integer64(const char *text, pin_value *value)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (value->type == PIN_U64 && *text == '-') {
        return false;
    }
    errno = 0;
    char *end;
    pin_value parsed = *value;
    if (value->type == PIN_I64) {
        parsed.i64 = strtoimax(text, &end, 10);
    } else {
        parsed.u64 = strtoumax(text, &end, 10);
    }
    if (errno == ERANGE || end == text) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end) {
        return false;
    }
    *value = parsed;
    return true;
}

static void set_integer(pin_value *value, double v)
{
    bool unsigned_type = value->type == PIN_U32 || value->type == PIN_U8 ||
                         value->type == PIN_U16;
    if (unsigned_type && v < 0) {
        v = 0;
    }
    switch (value->type) {
        case PIN_ENUM: value->e = (int32_t)v; break;
        case PIN_I32: value->i = (int32_t)v; break;
        case PIN_U32: value->u = (uint32_t)v; break;
        case PIN_I8: value->i8 = (int8_t)v; break;
        case PIN_U8: value->u8 = (uint8_t)v; break;
        case PIN_I16: value->i16 = (int16_t)v; break;
        default: value->u16 = (uint16_t)v; break;
    }
}

/* What the panel sent for a field, `text` as its control sends it and
   `axis` a vector's; a struct's storage, which the caller frees, holds a
   vector. False when the text names no value of the field's type. */
static bool parse_field(
    arena *storage,
    const ecs_component_desc *desc,
    const type_field *field,
    const char *text,
    const char *axis,
    pin_value *value
)
{
    switch (value->type) {
        case PIN_BOOL:
            value->b = !strcmp(text, "true") || !strcmp(text, "1");
            return true;
        case PIN_U32:
            if (appearance_color_field(desc->type, field)) {
                value->u = text[0] == '#' ? (uint32_t)strtoul(text + 1, NULL, 16)
                                          : value->u;
                return text[0] == '#';
            }
            /* fallthrough */
        case PIN_I32:
        case PIN_ENUM:
        case PIN_I8:
        case PIN_U8:
        case PIN_I16:
        case PIN_U16: set_integer(value, strtod(text, NULL)); return true;
        case PIN_I64:
        case PIN_U64: return parse_integer64(text, value);
        case PIN_F32:
        case PIN_F64: {
            double v = strtod(text, NULL);
            v = properties_angle(field) ? v * M_PI / 180 : v;
            if (value->type == PIN_F32) {
                value->f = (float)v;
            } else {
                value->d = v;
            }
            return true;
        }
        case PIN_STRUCT: {
            const type_desc *type = properties_vector(value);
            for (int i = 0; type && i < type->field_count; i++) {
                const type_field *f = &type->fields[i];
                if (strcmp(f->name, axis)) {
                    continue;
                }
                void *vector = xmalloc(type->size);
                memcpy(vector, value->ptr, type->size);
                double v = strtod(text, NULL);
                const char *error = pin_write_field(
                    storage,
                    vector,
                    f,
                    f->type == PIN_F32 ? (pin_value){ .type = PIN_F32, .f = (float)v }
                                       : (pin_value){ .type = PIN_F64, .d = v }
                );
                if (error) {
                    free(vector);
                    return false;
                }
                value->ptr = vector;
                return true;
            }
            return false;
        }
        case PIN_NAME: value->str = text; return true;
        case PIN_STR: {
            const char *type_name = value->type_name;
            const char *error = pin_value_text(storage, value, text, strlen(text));
            value->type_name = type_name;
            return !error;
        }
        default: return false;
    }
}

/* ---------- Edits by row ---------- */

/* What a gesture's input or change carries: the row, its control's text,
   a vector's axis, and whether it ends the gesture or Escape canceled it. */
typedef struct edit
{
    properties_row row;
    const char *text, *axis;
    bool active, canceled;
} edit;

static void edit_object(studio *s, const edit *e)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_actor *actor = subject.actor;
    const ecs_component_desc *desc = properties_component(s->scene, e->row.component);
    if (!actor || !desc || ecs_entity_to_u64(actor->entity) != e->row.entity ||
        (subject.locked && !studio_live(s))) {
        return;
    }
    if (desc->id == COMPONENT_Transform.id) {
        move(s, actor, !strcmp(e->axis, "y"), strtod(e->text, NULL), e->active, e->canceled);
        return;
    }
    arena storage = { 0 };
    pin_value before = { 0 };
    if (e->row.field < 0 || e->row.field >= desc->type->field_count ||
        ecs_component_read_field(
            &storage,
            s->scene->world,
            actor->entity,
            desc->id,
            e->row.field,
            &before
        ) != ECS_OK) {
        arena_free(&storage);
        return;
    }
    pin_value value = before;
    if (!parse_field(&storage, desc, &desc->type->fields[e->row.field], e->text, e->axis, &value)) {
        arena_free(&storage);
        return;
    }
    write_field(
        s,
        actor,
        (field_change){ .component = desc->id,
                        .field = e->row.field,
                        .before = before,
                        .value = value,
                        .active = e->active,
                        .selection = subject.multi }
    );
    if (value.type == PIN_STRUCT && value.ptr != before.ptr) {
        free(value.ptr);
    }
    arena_free(&storage);
}

void properties_parameter_row(const char *path, const char *name, size_t index, properties_row *row)
{
    *row = (properties_row){ .kind = PROPERTIES_PARAMETER, .parameter_index = index };
    uint64_t identity = UINT64_C(14695981039346656037);
    for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
        identity = (identity ^ *c) * UINT64_C(1099511628211);
    }
    identity *= UINT64_C(1099511628211); /* Path/name separator. */
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        identity = (identity ^ *c) * UINT64_C(1099511628211);
    }
    row->parameter_identity = identity;
}

const char *properties_parameter_name(const av_value *parameters, const char *path, const properties_row *row)
{
    if (row->parameter_index >= av_obj_size(parameters)) {
        return NULL;
    }
    const char *name = av_get_str(av_obj_key_at(parameters, row->parameter_index));
    properties_row current;
    properties_parameter_row(path, name, row->parameter_index, &current);
    return current.parameter_identity == row->parameter_identity ? name : NULL;
}

bool properties_parameter_value(arena *storage, const av_value *value, pin_value *out)
{
    *out = (pin_value){ 0 };
    if (!value) {
        return false;
    }
    switch (av_get_type(value)) {
        case AV_BOOL:
            *out = (pin_value){ .type = PIN_BOOL, .b = av_get_bool(value) };
            return true;
        case AV_STRING:
            return !pin_value_text(storage, out, av_get_str(value), strlen(av_get_str(value)));
        case AV_REAL:
            *out = (pin_value){ .type = PIN_F64, .d = av_get_num(value) };
            return true;
        case AV_SINT:
        case AV_UINT: {
            if (av_is_uint(value)) {
                *out = (pin_value){ .type = PIN_U64, .u64 = av_get_uint(value) };
                return true;
            }
            int64_t number = av_get_sint(value);
            if (number >= INT32_MIN && number <= INT32_MAX) {
                *out = (pin_value){ .type = PIN_I32, .i = (int32_t)number };
                return true;
            }
            *out = (pin_value){ .type = PIN_I64, .i64 = number };
            return true;
        }
        default: return false;
    }
}

static void source_edit_result(studio *s, int result)
{
    if (result < 0 || result == APP_PROTOTYPE_EDIT_NOT_APPLIED) {
        studio_error(
            s,
            result < 0 ? "Could not edit prototype" : "Edited, but not applied",
            s->scene->prototype_message
        );
    }
}

static void edit_parameter(studio *s, const edit *e)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_prototype_asset *asset = subject.asset;
    if (!asset || subject.locked || !scene_prototype_source_owned(asset)) {
        return;
    }
    char error[256];
    av_doc *doc = authoring_prototype_source_parameters(
        scene_source_input_text(s->scene, asset),
        error,
        sizeof error
    );
    const char *name = doc
                         ? properties_parameter_name(av_doc_root(doc), asset->path, &e->row)
                         : NULL;
    if (!name) {
        av_doc_free(doc);
        return;
    }
    arena storage = { 0 };
    pin_value value;
    bool valid = properties_parameter_value(&storage, av_obj_get(av_doc_root(doc), name), &value);
    if (valid) {
        char *end;
        errno = 0;
        switch (value.type) {
            case PIN_BOOL:
                value.b = !strcmp(e->text, "1") || !strcmp(e->text, "true");
                break;
            case PIN_STR:
                valid = !pin_value_text(&storage, &value, e->text, strlen(e->text));
                break;
            case PIN_I64:
            case PIN_U64: valid = parse_integer64(e->text, &value); break;
            case PIN_I32: {
                int64_t number = strtoimax(e->text, &end, 10);
                valid = end != e->text && !*end && errno != ERANGE &&
                        number >= INT32_MIN && number <= INT32_MAX;
                if (valid) {
                    value.i = (int32_t)number;
                }
                break;
            }
            case PIN_F64:
                value.d = strtod(e->text, &end);
                valid = end != e->text && !*end && errno != ERANGE &&
                        isfinite(value.d);
                break;
            default: valid = false; break;
        }
    }
    if (valid) {
        char key[192];
        properties_row_id(&e->row, key);
        authoring_prototype_edit change = { .kind = AUTHORING_PROTOTYPE_SET_PARAMETER,
                                            .field = name,
                                            .value = value };
        source_edit_result(
            s,
            scene_source_input_edit(s->scene, asset, &change, 1, key, !strcmp(s->prototype_edit, key))
        );
        snprintf(s->prototype_edit, sizeof s->prototype_edit, "%s", key);
    }
    av_doc_free(doc);
    arena_free(&storage);
    if (!e->active) {
        s->prototype_edit[0] = 0;
        scene_source_input_end(s->scene, asset->path);
        asset_code_end_typing(asset->source);
    }
}

static void reset_source_override(studio *s, const properties_row *row)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_prototype_asset *asset = subject.asset;
    const ecs_component_desc *desc = properties_component(s->scene, row->component);
    if (row->kind != PROPERTIES_PROTOTYPE || !asset || subject.locked ||
        !desc || strcmp(row->prototype, asset->prototype->name) ||
        row->field < 0 || row->field >= desc->type->field_count ||
        !scene_prototype_source_owned(asset)) {
        return;
    }
    authoring_prototype_edit change = { .kind = AUTHORING_PROTOTYPE_RESET_FIELD_OVERRIDE,
                                        .component = desc->name,
                                        .field = desc->type->fields[row->field].name };
    s->prototype_edit[0] = 0;
    source_edit_result(s, scene_source_input_edit(s->scene, asset, &change, 1, "reset-override", false));
    scene_source_input_end(s->scene, asset->path);
}

/* Prototype edits preview live and record one Undo step per gesture, on the
   prototype's own history. */
static void edit_prototype(studio *s, const edit *e)
{
    scene_prototype_asset *asset = find_prototype(s->scene, e->row.prototype);
    const ecs_component_desc *desc = properties_component(s->scene, e->row.component);
    const prototype_component *component = NULL;
    for (int i = 0; asset && desc && i < acount(asset->prototype->components); i++) {
        component = asset->prototype->components[i].desc == desc
                      ? &asset->prototype->components[i]
                      : component;
    }
    if (!component || !component->data || !studio_editable(s) ||
        e->row.field < 0 || e->row.field >= desc->type->field_count) {
        return;
    }
    const type_field *field = &desc->type->fields[e->row.field];
    arena storage = { 0 };
    pin_value value = { 0 };
    if (pin_read_field(&storage, component->data, field, &value)) {
        arena_free(&storage);
        return;
    }
    const void *original = value.ptr;
    char key[192];
    snprintf(
        key,
        sizeof key,
        "%s.%s.%s.%s",
        asset->prototype->name,
        desc->name,
        field->name,
        e->axis
    );
    if (parse_field(&storage, desc, field, e->text, e->axis, &value) &&
        editable_scalar(&value)) {
        if (scene_prototype_source_owned(asset)) {
            char path[192];
            snprintf(path, sizeof path, "%s", field->name);
            pin_value leaf = value;
            const type_desc *vector = properties_vector(&value);
            for (int i = 0; vector && i < vector->field_count; i++) {
                if (!strcmp(vector->fields[i].name, e->axis)) {
                    snprintf(path, sizeof path, "%s.%s", field->name, e->axis);
                    if (pin_read_field(&storage, value.ptr, &vector->fields[i], &leaf)) {
                        free(value.ptr);
                        arena_free(&storage);
                        return;
                    }
                    break;
                }
            }
            authoring_prototype_edit change = { .kind = AUTHORING_PROTOTYPE_SET_FIELD,
                                                .component = desc->name,
                                                .field = path,
                                                .value = leaf };
            int result = scene_source_input_edit(
                s->scene,
                asset,
                &change,
                1,
                key,
                !strcmp(s->prototype_edit, key)
            );
            if (result < 0 || result == APP_PROTOTYPE_EDIT_NOT_APPLIED) {
                studio_error(
                    s,
                    result < 0 ? "Could not edit prototype" : "Edited, but not applied",
                    s->scene->prototype_message
                );
            }
        } else {
            if (strcmp(s->prototype_edit, key)) {
                prototype_history_before(s->scene, asset->prototype);
            }
            write_prototype_field(s->scene, asset, desc->name, field->name, value);
        }
        snprintf(s->prototype_edit, sizeof s->prototype_edit, "%s", key);
    }
    if (!e->active) {
        s->prototype_edit[0] = '\0';
        if (asset->source) {
            scene_source_input_end(s->scene, asset->path);
            asset_code_end_typing(asset->source);
        }
    }
    if (value.type == PIN_STRUCT && value.ptr != original) {
        free(value.ptr);
    }
    arena_free(&storage);
}

/* The Scene tab's settings change once a gesture ends, one Undo step. */
static void edit_scene(studio *s, const edit *e)
{
    app_scene_authoring *scene = s->scene;
    if (e->active || !studio_editable(s)) {
        return;
    }
    if (e->row.field == SCENE_UI) {
        if (strcmp(e->text, scene->ui ? scene->ui : "")) {
            app_scene_authoring_set_ui(scene, e->text);
        }
        return;
    }
    physics_settings next = physics_configuration(scene->live->runtime->physics);
    double v = strtod(e->text, NULL);
    bool on = v != 0, *flag = NULL;
    switch ((properties_scene_setting)e->row.field) {
        case SCENE_WORKERS: next.workers = (int)v; break;
        case SCENE_SUBSTEPS: next.substeps = (int)v; break;
        case SCENE_SLEEP: flag = &next.sleep; break;
        case SCENE_CONTINUOUS: flag = &next.continuous; break;
        case SCENE_FORCE_DRIVE: flag = &next.force_drive; break;
        case SCENE_HERTZ: next.contact_hertz = (float)v; break;
        case SCENE_DAMPING: next.contact_damping = (float)v; break;
        default: return;
    }
    if (flag && *flag == on) {
        return;
    }
    if (flag) {
        *flag = on;
    }
    if (app_scene_authoring_set_physics(scene, next) != 0) {
        studio_report(s, "Invalid physics settings");
    }
}

static void edit_row(studio *s, const edit *e)
{
    switch (e->row.kind) {
        case PROPERTIES_OBJECT: edit_object(s, e); return;
        case PROPERTIES_PROTOTYPE: edit_prototype(s, e); return;
        case PROPERTIES_SCENE: edit_scene(s, e); return;
        case PROPERTIES_PARAMETER: edit_parameter(s, e); return;
    }
}

/* A checkbox sets its field to its state, a mixed one every object's.
   RmlUi sends change when a binding sets checked too, so a field that
   holds the state already stays as it is. */
static void check(studio *s, const properties_row *row, bool on)
{
    edit e = { .row = *row, .text = on ? "1" : "0", .axis = "" };
    if (row->kind == PROPERTIES_SCENE || row->kind == PROPERTIES_PARAMETER) {
        edit_row(s, &e);
        return;
    }
    arena storage = { 0 };
    pin_value value = { 0 };
    bool read = false;
    properties_subject subject;
    properties_subject_of(s, &subject);
    if (row->kind == PROPERTIES_PROTOTYPE) {
        scene_prototype_asset *asset = find_prototype(s->scene, row->prototype);
        const ecs_component_desc *desc = properties_component(s->scene, row->component);
        for (int i = 0; asset && desc && i < acount(asset->prototype->components); i++) {
            const prototype_component *c = &asset->prototype->components[i];
            if (c->desc == desc && c->data && row->field < desc->type->field_count) {
                if (pin_read_field(&storage, c->data, &desc->type->fields[row->field], &value)) {
                    arena_free(&storage);
                    return;
                }
                read = true;
            }
        }
    } else if (subject.actor) {
        if (ecs_component_read_field(
                &storage,
                s->scene->world,
                subject.actor->entity,
                row->component,
                row->field,
                &value
            ) != ECS_OK) {
            arena_free(&storage);
            return;
        }
        read = true;
    }
    if (!read || value.type != PIN_BOOL) {
        arena_free(&storage);
        return;
    }
    bool mixed = row->kind == PROPERTIES_OBJECT && subject.actor &&
                 properties_mixed(s, subject.actor, row->component, row->field, &value);
    if (mixed || value.b != on) {
        edit_row(s, &e);
    }
    arena_free(&storage);
}

/* An asset dropped on its field is one Undo step, to every selected object
   from a multi-object field. */
static void drop(studio *s, const properties_row *row, const char *asset)
{
    edit e = { .row = *row, .text = asset, .axis = "" };
    edit_row(s, &e);
}

/* ---------- Reset and Apply to Prototype ---------- */

/* The prototype's Position, which a Reset puts back whole, as the row
   shows both axes. */
static Transform inherited_position(const prototype_component *component, Transform at)
{
    arena storage = { 0 };
    pin_value x = { .type = PIN_F32, .f = at.x }, y = { .type = PIN_F32, .f = at.y };
    if (pin_read_field(&storage, component->data, &component->desc->type->fields[0], &x) ||
        pin_read_field(&storage, component->data, &component->desc->type->fields[1], &y)) {
        arena_free(&storage);
        return at;
    }
    at.x = x.f;
    at.y = y.f;
    arena_free(&storage);
    return at;
}

/* Reset one instance value to its prototype default as a single Undo step. */
static void reset_field(studio *s, scene_actor *actor)
{
    app_scene_authoring *scene = s->scene;
    const ecs_component_desc *desc = properties_component(scene, s->context.component);
    if (!desc || desc->derived_from) {
        return;
    }
    prototype_component added;
    const prototype_component *component = properties_base(actor, desc, &added);
    arena storage = { 0 };
    pin_value before = { 0 }, inherited = { 0 };
    if (!component->data || ecs_component_read_field(
                                &storage,
                                scene->world,
                                actor->entity,
                                desc->id,
                                s->context.field,
                                &before
                            ) != ECS_OK) {
        arena_free(&storage);
        return;
    }
    inherited = before;
    if (pin_read_field(&storage, component->data, &desc->type->fields[s->context.field], &inherited)) {
        arena_free(&storage);
        return;
    }
    if (desc->id == COMPONENT_Transform.id) {
        Transform after = inherited_position(
            component,
            *Transform_get_const(scene->world, actor->entity)
        );
        begin_transform_edit(scene, actor->entity);
        write_transform_edit(scene, actor->entity, after);
        if (scene_collab_can_write_locally() && !studio_live(s)) {
            scene->scene_dirty = true;
        }
        finish_transform_edit(scene, false);
        arena_free(&storage);
        return;
    }
    write_field(
        s,
        actor,
        (field_change){ .component = desc->id,
                        .field = s->context.field,
                        .before = before,
                        .value = inherited }
    );
    arena_free(&storage);
}

/* Reset to Prototype resets each selected object, to its own prototype's
   value, as one Undo step. */
static void reset(studio *s)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_actor *actor = subject.actor;
    if (!actor || s->context.entity != ecs_entity_to_u64(actor->entity) ||
        (subject.locked && !studio_live(s))) {
        return;
    }
    if (!subject.multi) {
        reset_field(s, actor);
        return;
    }
    app_scene_authoring *scene = s->scene;
    finish_transform_edit(scene, false);
    scene_history_begin_group(scene->history);
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene_instance_selected(scene, i) &&
            ecs_component_has(scene->world, scene->actors[i].entity, s->context.component)) {
            reset_field(s, &scene->actors[i]);
        }
    }
    scene_history_end_group(scene->history);
}

/* Unity's Apply to Prefab: the active object's value becomes its
   prototype's. */
static void apply(studio *s)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_actor *actor = subject.actor;
    const ecs_component_desc *desc = properties_component(s->scene, s->context.component);
    if (actor && desc && s->context.entity == ecs_entity_to_u64(actor->entity) &&
        !subject.locked) {
        studio_apply_field(s, actor, (prototype_field){ desc, s->context.field });
    }
}

/* ---------- Components ---------- */

bool studio_can_add_component(studio *s)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    return subject.asset ||
           (subject.actor && subject.actor->identity && !subject.multi);
}

/* Add Component adds to the prototype a prototype document shows, or to the
   one placed object selected, as an override of its prototype (Unity's and
   Godot's Add Component on an instance). */
void studio_add_component_open(studio *s)
{
    assert(studio_can_add_component(s));
    properties_subject subject;
    properties_subject_of(s, &subject);
    s->components.object = subject.asset ? 0 : subject.actor->identity;
    snprintf(
        s->components.prototype,
        sizeof s->components.prototype,
        "%s",
        subject.asset ? subject.asset->prototype->name : ""
    );
    studio_palette_open(s, STUDIO_PALETTE_COMPONENTS, "Search components");
}

/* Adds to the target and names it in `target`: 0, 1 while a collaboration
   host decides, or -1 with why in Studio's error. */
static int add_to_target(studio *s, const ecs_component_desc *desc, char target[128])
{
    app_scene_authoring *scene = s->scene;
    if (!s->components.object) {
        snprintf(target, 128, "%s", s->components.prototype);
        int added = app_scene_authoring_add_prototype_component(scene, target, desc->name);
        if (added < 0) {
            studio_error(s, "Could not add component", scene->prototype_message);
        }
        if (added == APP_PROTOTYPE_EDIT_NOT_APPLIED) {
            studio_error(s, "Edited, but not applied", scene->prototype_message);
            return 0;
        }
        return added;
    }
    scene_actor *actor = scene_actor_by_identity(scene, s->components.object);
    if (!actor) {
        return -1;
    }
    select_instance(scene, (int)(actor - scene->actors));
    /* Adding replaces the object, and its name with it. */
    snprintf(target, 128, "%s", actor->name);
    int added = app_scene_authoring_add_instance_component(scene, desc->name);
    if (added < 0) {
        studio_error(s, "Could not add component", scene->message);
    }
    return added;
}

static void add_component(studio *s, const ecs_component_desc *desc)
{
    char target[128];
    if (!studio_editable(s) || add_to_target(s, desc, target) != 0) {
        return;
    }
    char label[128], report[320];
    editor_component_label(desc->name, label, sizeof label);
    snprintf(report, sizeof report, "Added %s to %s", label, target);
    studio_report(s, report);
    /* Its panel shows, open, as Unity adds a component. */
    s->tab = properties_tab_of(s, desc);
    snprintf(s->components.added, sizeof s->components.added, "%s", desc->name);
}

void studio_add_component_to_selection(studio *s, const ecs_component_desc *desc)
{
    scene_actor *actor = selected_actor(s->scene);
    assert(actor && actor->identity);
    s->components.object = actor->identity;
    add_component(s, desc);
}

/* A component the prototype lacks, as the palette lists it. */
typedef struct
{
    int order; /* its tab's, the current tab's first */
    char label[128];
    int index; /* in the registry */
} component_choice;

/* The fields an entry lists before it counts the rest. */
#define ABOUT_FIELDS 4

/* An entry's second line, which tells components with like names apart:
   their first fields, as "Speed · Lifetime · Rate". The SDK gives
   components no description yet; once it does, that takes this place. */
static void component_about(const ecs_component_desc *desc, char *out, size_t size)
{
    const type_desc *type = desc->type;
    out[0] = '\0';
    if (!type->field_count) {
        snprintf(out, size, "A tag, with no values");
        return;
    }
    size_t used = 0;
    for (int i = 0; i < type->field_count && i < ABOUT_FIELDS; i++) {
        char label[64];
        editor_identifier_label(type->fields[i].name, label, sizeof label);
        used += (size_t)snprintf(out + used, size - used, "%s%s", i ? " \xc2\xb7 " : "", label);
        if (used >= size) {
            return;
        }
    }
    int rest = type->field_count - ABOUT_FIELDS;
    if (rest > 0) {
        snprintf(out + used, size - used, " \xc2\xb7 %d more", rest);
    }
}

static int by_tab_and_label(const void *a, const void *b)
{
    const component_choice *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcmp(x->label, y->label);
}

void studio_component_entries(studio *s, studio_palette_entry **entries)
{
    app_scene_authoring *scene = s->scene;
    scene_prototype_asset *asset = s->components.object
                                     ? NULL
                                     : find_prototype(scene, s->components.prototype);
    scene_actor *actor = s->components.object
                           ? scene_actor_by_identity(scene, s->components.object)
                           : NULL;
    int current = s->tab >= STUDIO_TAB_OBJECT && s->tab <= STUDIO_TAB_GAMEPLAY
                    ? s->tab - STUDIO_TAB_OBJECT
                    : 0;
    component_choice *choices = NULL;
    for (int i = 0; (asset || actor) && i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        /* The engine computes a derived component from its source. */
        if (desc->derived_from ||
            (asset ? prototype_has_component_id(asset->prototype, desc->id)
                   : ecs_component_has(scene->world, actor->entity, desc->id))) {
            continue;
        }
        component_choice choice = {
            .order = (properties_tab_of(s, desc) - STUDIO_TAB_OBJECT - current +
                      COMPONENT_TAB_COUNT) %
                     COMPONENT_TAB_COUNT,
            .index = i,
        };
        editor_component_label(desc->name, choice.label, sizeof choice.label);
        apush(choices, choice);
    }
    if (acount(choices)) {
        qsort(choices, (size_t)acount(choices), sizeof *choices, by_tab_and_label);
    }
    for (int i = 0; i < acount(choices); i++) {
        const ecs_component_desc *desc = scene->registry.components[choices[i].index];
        studio_tab tab = properties_tab_of(s, desc);
        studio_palette_entry *e = studio_palette_add(
            entries,
            properties_tab_label(tab),
            choices[i].label
        );
        snprintf(e->keywords, sizeof e->keywords, "%s", desc->name);
        component_about(desc, e->about, sizeof e->about);
        e->icon = TAB_ICONS[tab];
        e->index = choices[i].index;
    }
    afree(choices);
}

void studio_component_choose(studio *s, const studio_palette_entry *entry)
{
    add_component(s, s->scene->registry.components[entry->index]);
}

static void removed_toast(studio *s, const char *title)
{
    kit_toast_dismiss(s->kit, s->components.toast);
    s->components.toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .title = title, .action = "Undo", .duration = 6 }
    );
    studio_record(s, KIT_TOAST_DEFAULT, title);
}

/* Destructive edits offer Undo in a toast while they are still the latest
   edit, as Delete does. */
static void remove_component(studio *s, scene_prototype_asset *asset)
{
    const char *name = s->components.menu;
    char label[128], title[320];
    editor_component_label(name, label, sizeof label);
    snprintf(title, sizeof title, "Removed %s from %s", label, asset->prototype->name);
    int result = app_scene_authoring_remove_prototype_component(
        s->scene,
        asset->prototype->name,
        name
    );
    if (result == APP_PROTOTYPE_EDIT_PENDING) {
        return;
    }
    if (result < 0) {
        studio_error(s, "Could not remove component", s->scene->prototype_message);
        return;
    }
    removed_toast(s, title);
    if (!result &&
        strstr(s->scene->prototype_message, "Inherited composition is now local")) {
        studio_report(s, s->scene->prototype_message);
    }
    if (result == APP_PROTOTYPE_EDIT_NOT_APPLIED) {
        studio_error(s, "Edited, but not applied", s->scene->prototype_message);
    }
    s->components.step = asset->source ? asset_code_step(asset->source)
                                       : alast(asset->undo);
    s->components.scene_step = 0;
}

/* An object removes only a component it added; its prototype's stay. */
static void remove_object_component(studio *s, const scene_actor *actor)
{
    const char *name = s->components.menu;
    char label[128], title[320];
    editor_component_label(name, label, sizeof label);
    snprintf(title, sizeof title, "Removed %s from %s", label, actor->name);
    int removed = app_scene_authoring_remove_instance_component(s->scene, name);
    if (removed < 0) {
        studio_error(s, "Could not remove component", s->scene->message);
    }
    if (removed != 0) {
        return;
    }
    removed_toast(s, title);
    s->components.scene_step = scene_history_step_id(s->scene->history, false);
}

static bool removal_current(studio *s)
{
    app_scene_authoring *scene = s->scene;
    if (s->components.scene_step) {
        return scene->document.kind == APP_EDITOR_DOCUMENT_SCENE &&
               scene_history_step_id(scene->history, false) == s->components.scene_step;
    }
    scene_prototype_asset *asset = scene->document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE
                                     ? selected_prototype(scene)
                                     : NULL;
    return asset &&
           (asset->source
                ? asset_code_can_replay(asset->source, false) &&
                      asset_code_step(asset->source) == s->components.step
                : acount(asset->undo) && alast(asset->undo) == s->components.step);
}

static int undo_removal(studio *s)
{
    if (s->components.scene_step) {
        return replay_scene_edit(s->scene, false);
    }
    return prototype_undo(s->scene);
}

void studio_component_toast(studio *s)
{
    int toast = s->components.toast;
    if (!toast) {
        return;
    }
    bool current = removal_current(s);
    if (kit_toast_action(s->kit, toast)) {
        s->components.toast = 0;
        if (current && studio_editable(s)) {
            int result = undo_removal(s);
            if (!result) {
                studio_report(s, "Undo");
            } else if (result == APP_PROTOTYPE_EDIT_PENDING && !s->components.scene_step) {
                studio_error(s, "Waiting for shared edit", s->scene->prototype_message);
            } else if (result > 0) {
                studio_error(s, "Edited, but not applied", s->scene->prototype_message);
            }
        }
        return;
    }
    if (!current) {
        kit_toast_dismiss(s->kit, toast);
        s->components.toast = 0;
    }
}

/* A component panel's menu, its ⋯ or a right press on its head (Unity's
   component menu, Blender's modifier menu): Remove Component on a
   prototype or a component an object added, Keep Changes on a running
   object. */
static void component_action(studio *s)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    if (subject.asset && !subject.locked) {
        remove_component(s, subject.asset);
        return;
    }
    if (subject.actor && !subject.locked && !subject.multi && !studio_live(s)) {
        remove_object_component(s, subject.actor);
        return;
    }
    for (int i = 0;
         subject.actor && studio_live(s) && i < s->scene->registry.component_count;
         i++) {
        const ecs_component_desc *desc = s->scene->registry.components[i];
        if (!strcmp(desc->name, s->components.menu)) {
            studio_keep_changes(s, desc);
        }
    }
}

/* ---------- Keep Changes ---------- */

void studio_keep_changes(studio *s, const ecs_component_desc *only)
{
    scene_actor *actor = selected_actor(s->scene);
    int kept = scene_keep_changes(s->scene, only);
    if (kept < 0) {
        return;
    }
    char title[320];
    kit_toast_dismiss(s->kit, s->keep.toast);
    s->keep.toast = 0;
    if (!kept) {
        snprintf(title, sizeof title, "%s matches the scene", actor->name);
        kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_INFO, .title = title });
        return;
    }
    snprintf(
        title,
        sizeof title,
        "Kept %d %s on %s",
        kept,
        kept == 1 ? "value" : "values",
        actor->name
    );
    s->keep.toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){
            .title = title,
            .description = "They apply to the scene when you stop",
            .action = "Undo",
            .duration = 6 }
    );
    s->keep.depth = acount(s->scene->kept);
    studio_record(s, KIT_TOAST_DEFAULT, title);
}

void studio_keep_toast(studio *s)
{
    int toast = s->keep.toast;
    if (!toast) {
        return;
    }
    bool current = acount(s->scene->kept) == s->keep.depth;
    if (kit_toast_action(s->kit, toast)) {
        s->keep.toast = 0;
        if (current) {
            scene_kept_drop(s->scene);
            studio_report(s, "Undo Keep Changes");
        }
        return;
    }
    if (!current) {
        kit_toast_dismiss(s->kit, toast);
        s->keep.toast = 0;
    }
}

/* ---------- The selection ---------- */

/* Keeps only the selected objects of `prototype` selected, as Unity's
   "Narrow the Selection" does. */
static void narrow_selection(studio *s, const char *prototype)
{
    app_scene_authoring *scene = s->scene;
    finish_transform_edit(scene, false);
    bool any = false;
    for (int i = 0; i < acount(scene->actors); i++) {
        scene->actors[i].marked = scene_instance_selected(scene, i) &&
                                  !strcmp(scene->actors[i].prototype_name, prototype);
        any |= scene->actors[i].marked;
    }
    for (int i = 0;
         any && !scene->actors[scene->selected].marked && i < acount(scene->actors);
         i++) {
        scene->selected = scene->actors[i].marked ? i : scene->selected;
    }
}

/* Selects the object an entity field names (studio_properties_view.c),
   as Unity's object field pings its object; nothing when the selected
   objects name different ones. */
static void reveal(studio *s, const properties_row *row)
{
    arena storage = { 0 };
    pin_value value = { 0 };
    properties_subject subject;
    properties_subject_of(s, &subject);
    if (row->kind != PROPERTIES_OBJECT ||
        ecs_component_read_field(
            &storage,
            s->scene->world,
            ecs_entity_from_u64(row->entity),
            row->component,
            row->field,
            &value
        ) != ECS_OK ||
        value.type != PIN_HANDLE || !value.u64 ||
        (subject.actor &&
         properties_mixed(s, subject.actor, row->component, row->field, &value))) {
        arena_free(&storage);
        return;
    }
    /* A placed object selects as the Outliner selects it; a spawned one as
       a pick in the Scene does. */
    app_scene_authoring_select_entity(s->scene, ecs_entity_from_u64(value.u64));
    arena_free(&storage);
}

/* ---------- Menus ---------- */

/* The element that sent `a`, while it lasts. */
static rml_element *sender(const ui_activation *a)
{
    return a->anchor ? rml_element_ref_get(a->anchor) : NULL;
}

/* Shows the Properties menu `id` from the document that sent `a`: under the
   button that opened it, as Base UI anchors a menu to its trigger, so a
   keyboard press opens it there too; at the pointer for a right press. */
static void show_menu(const ui_activation *a, const char *id)
{
    rml_element *from = sender(a);
    rml_element *menu = from ? rml_element_by_id(rml_element_owner_document(from), id)
                             : NULL;
    if (!menu) {
        return;
    }
    if (!strcmp(rml_element_tag_name(from), "button")) {
        popover_show(menu, &(popover_anchor){ .side = POPOVER_BELOW, .element = from, .trigger = from });
        return;
    }
    popover_show(menu, &(popover_anchor){ .side = POPOVER_POINT, .box = { a->x, a->y, 0, 0 } });
}

/* A right press on a field opens its menu: Reset and Apply to Prototype. */
static void field_menu(studio *s, const properties_row *row, const ui_activation *a)
{
    if (row->kind != PROPERTIES_OBJECT) {
        return;
    }
    s->context.entity = row->entity;
    s->context.component = row->component;
    s->context.field = row->field;
    show_menu(a, "field-menu");
}

static void component_menu(studio *s, const char *key, const ui_activation *a)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    const ecs_component_desc *desc = NULL;
    for (int i = 0; i < s->scene->registry.component_count; i++) {
        const ecs_component_desc *d = s->scene->registry.components[i];
        desc = !strcmp(properties_panel_key(d->name), key) ? d : desc;
    }
    if (!desc || desc->derived_from) {
        return;
    }
    prototype_component added;
    bool own = subject.actor && !subject.multi &&
               properties_base(subject.actor, desc, &added) == &added;
    bool menu = subject.asset || (subject.actor && subject.actor->identity &&
                                  (studio_live(s) || own));
    if (!menu) {
        return;
    }
    snprintf(s->components.menu, sizeof s->components.menu, "%s", desc->name);
    show_menu(a, "component-menu");
}

const char *properties_text(arena *storage, const pin_value *value)
{
    if (value->type == PIN_NAME) {
        return value->str ? value->str : "";
    }
    const char *text;
    if (value->type == PIN_STR && !pin_text_cstring(storage, *value, &text, NULL)) {
        return text;
    }
    return pin_value_to_string(value);
}

/* ---------- Text fields ---------- */

/* A text field shows the model again and lets go: Escape keeps what it had,
   as the kit's fields do, and a commit shows what the model took. `id` is
   the field's row, or "name" for the object's name. */
static void settle(studio *s, rml_element *field, const char *id)
{
    if (!field) {
        return;
    }
    arena storage = { 0 };
    properties_subject subject;
    properties_subject_of(s, &subject);
    properties_row row;
    pin_value value = { 0 };
    char text[512] = "";
    if (!strcmp(id, "name") && subject.actor) {
        snprintf(text, sizeof text, "%s", subject.actor->name);
    } else if (
        properties_row_parse(id, &row) && row.kind == PROPERTIES_PARAMETER &&
        subject.asset
    ) {
        char error[256];
        av_doc *doc = authoring_prototype_source_parameters(
            scene_source_input_text(s->scene, subject.asset),
            error,
            sizeof error
        );
        const char *name = doc ? properties_parameter_name(
                                     av_doc_root(doc),
                                     subject.asset->path,
                                     &row
                                 )
                               : NULL;
        if (name &&
            properties_parameter_value(&storage, av_obj_get(av_doc_root(doc), name), &value)) {
            snprintf(text, sizeof text, "%s", properties_text(&storage, &value));
        }
        av_doc_free(doc);
    } else if (
        properties_row_parse(id, &row) && row.kind == PROPERTIES_PROTOTYPE &&
        subject.asset
    ) {
        const ecs_component_desc *desc = properties_component(s->scene, row.component);
        const prototype *shown = scene_source_input_preview(s->scene, subject.asset);
        if (desc && row.field >= 0 && row.field < desc->type->field_count &&
            !prototype_read_field(
                &storage,
                shown,
                desc->name,
                desc->type->fields[row.field].name,
                &value
            )) {
            snprintf(text, sizeof text, "%s", properties_text(&storage, &value));
        }
    } else if (
        properties_row_parse(id, &row) && row.kind == PROPERTIES_OBJECT &&
        subject.actor &&
        ecs_component_read_field(
            &storage,
            s->scene->world,
            subject.actor->entity,
            row.component,
            row.field,
            &value
        ) == ECS_OK
    ) {
        snprintf(text, sizeof text, "%s", properties_text(&storage, &value));
    }
    rml_element_set_value(field, text);
    rml_element_blur(field);
    arena_free(&storage);
}

/* The name renames the object as one Undo step, as Enter or leaving the
   field commits it; a name another object has is refused with a toast. */
static void rename_object(studio *s, const ui_activation *a)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    if (subject.actor && subject.actor->identity && !subject.locked && a->argc) {
        studio_rename_commit(s, ecs_entity_to_u64(subject.actor->entity), a->args[0]);
    }
    settle(s, sender(a), "name");
}

/* ---------- Intents ---------- */

static void attach(studio *s, const char *behavior)
{
    properties_subject subject;
    properties_subject_of(s, &subject);
    if (!subject.actor || subject.locked) {
        return;
    }
    char **names = instance_behavior_names(s->scene, subject.actor->entity);
    bool attached = false;
    for (int i = 0; i < acount(names); i++) {
        attached |= !strcmp(names[i], behavior);
    }
    free_behavior_names(names);
    app_scene_authoring_set_instance_behavior(s->scene, behavior, !attached);
}

/* A gesture's input or change: emit('properties.input', row, value[, axis])
   and emit('properties.change', row, value[, canceled[, axis]]). */
static void gesture(studio *s, const ui_activation *a, bool active)
{
    properties_row row;
    if (a->argc < 2 || !properties_row_parse(a->args[0], &row)) {
        return;
    }
    int axis = active ? 2 : 3;
    edit e = { .row = row,
               .text = a->args[1],
               .axis = a->argc > axis ? a->args[axis] : "",
               .active = active,
               .canceled = !active && a->argc > 2 && !strcmp(a->args[2], "1") };
    edit_row(s, &e);
    rml_element *field = sender(a);
    if (!active && field && !strcmp(rml_element_tag_name(field), "input")) {
        settle(s, field, a->args[0]);
    }
}

void studio_properties_action(studio *s, const char *action, const ui_activation *a)
{
    properties_row row;
    bool parsed = a->argc && properties_row_parse(a->args[0], &row);
    const char *arg = a->argc ? a->args[0] : "";
    if (!strcmp(action, "input") || !strcmp(action, "change")) {
        gesture(s, a, !strcmp(action, "input"));
    } else if (!strcmp(action, "check") && parsed) {
        check(s, &row, a->argc > 1 && a->args[1][0]);
    } else if (!strcmp(action, "choices")) {
        snprintf(s->properties.choosing, sizeof s->properties.choosing, "%s", arg);
    } else if (!strcmp(action, "drop") && parsed && a->argc > 1) {
        drop(s, &row, a->args[1]);
    } else if (!strcmp(action, "menu") && parsed) {
        field_menu(s, &row, a);
    } else if (!strcmp(action, "resetOverride") && parsed) {
        reset_source_override(s, &row);
    } else if (!strcmp(action, "reset")) {
        reset(s);
    } else if (!strcmp(action, "apply")) {
        apply(s);
    } else if (!strcmp(action, "panelMenu")) {
        component_menu(s, arg, a);
    } else if (!strcmp(action, "componentAction")) {
        component_action(s);
    } else if (!strcmp(action, "tab") && a->has_value && a->value >= 0 && a->value < STUDIO_TAB_COUNT) {
        s->tab = (studio_tab)a->value;
    } else if (!strcmp(action, "fold")) {
        fold(s, arg);
    } else if (!strcmp(action, "rename")) {
        rename_object(s, a);
    } else if (!strcmp(action, "cancel")) {
        settle(s, sender(a), a->id);
    } else if (!strcmp(action, "clear")) {
        s->property_search[0] = '\0';
        rml_element *field = sender(a);
        if (field) {
            rml_element_set_value(field, "");
            rml_element_blur(field);
        }
    } else if (!strcmp(action, "openPrototype")) {
        properties_subject subject;
        properties_subject_of(s, &subject);
        if (subject.actor) {
            app_scene_authoring_open_prototype(s->scene, subject.actor->prototype_name);
        }
    } else if (!strcmp(action, "narrow")) {
        narrow_selection(s, arg);
    } else if (!strcmp(action, "reveal") && parsed) {
        reveal(s, &row);
    } else if (!strcmp(action, "openBehavior")) {
        if (!s->scene->graph_document_id || strcmp(s->scene->graph_document_id, arg)) {
            app_scene_authoring_open_behavior(s->scene, arg);
        }
    } else if (!strcmp(action, "detach") && studio_editable(s)) {
        app_scene_authoring_set_instance_behavior(s->scene, arg, false);
    } else if (!strcmp(action, "attach")) {
        attach(s, arg);
    }
}

/* ---------- The area ---------- */

/* An asset dragged over a field shows on its objects in the Scene, for this
   draw only. */
static void preview_drop(studio *s)
{
    char id[PROPERTIES_ROW_ID], asset[256];
    properties_row row;
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_actor *actor = subject.actor;
    if (!actor || subject.locked || !properties_drop_over(s, id, asset, sizeof asset) ||
        !properties_row_parse(id, &row) || row.kind != PROPERTIES_OBJECT ||
        row.entity != ecs_entity_to_u64(actor->entity)) {
        return;
    }
    scene_preview_field(s->scene, actor->entity, row.component, row.field, asset);
    int *others = subject.multi ? properties_others(s, row.component) : NULL;
    for (int i = 0; i < acount(others); i++) {
        scene_preview_field(s->scene, s->scene->actors[others[i]].entity, row.component, row.field, asset);
    }
    afree(others);
}

/* Focuses the input `selector` finds in `host` and selects its text; false
   until the panel has built it. */
static bool focus_input(rml_element *host, const char *selector)
{
    rml_element *input = host ? rml_element_query_selector(host, selector) : NULL;
    if (!input || !rml_element_is_visible(input, true)) {
        return false;
    }
    rml_element_focus(input, true);
    rml_element_select(input);
    return true;
}

void studio_properties_header(studio *s, int area)
{
    /* Visual designer (workstream H): a UI document's Details. */
    if (studio_designer_active(s)) {
        studio_designer_details_header(s, area);
        return;
    }
    rml_element *host = studio_ui_area_controls(s, area, "properties", "PropertiesHeader");
    if (s->find_request == STUDIO_EDITOR_PROPERTIES + 1 &&
        focus_input(host, "#search")) {
        s->find_request = 0;
    }
}

void studio_properties_body(studio *s, int area)
{
    /* Visual designer (workstream H): a UI document's Details. */
    if (studio_designer_active(s)) {
        studio_designer_details(s, area);
        return;
    }
    rml_element *host = studio_ui_area(s, area, "Properties");
    if (!host) {
        return;
    }
    /* Rename (F2) focuses the name while no area shows the Outliner. */
    if (s->rename.entity && !s->rename.outliner) {
        if (focus_input(host, "#name")) {
            s->rename.entity = 0;
        } else {
            s->ui_schedule.wake = true;
        }
    }
    app_editor_document_kind kind = app_scene_authoring_document(s->scene).kind;
    bool graph_document = kind == APP_EDITOR_DOCUMENT_BEHAVIOR ||
                          kind == APP_EDITOR_DOCUMENT_SYSTEM;
    bool model_document = graph_document && s->scene->graph_editor &&
                          editor_behavior_asset(s->scene->graph_editor)->domain ==
                              BEHAVIOR_DOMAIN_MODEL;
    if (model_document && s->tab == STUDIO_TAB_NODE &&
        kit_slot_begin(s->kit, "graph-native", host, "#graph-native")) {
        studio_model_preview_controls(s, s->scene->graph_editor);
        studio_graph_model_controls(s);
        kit_slot_end(s->kit);
    }
    preview_drop(s);
}
