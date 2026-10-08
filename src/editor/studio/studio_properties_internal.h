/* Properties (workstream P1): its model and intents (studio_properties.c)
 * and the `properties` variable its authored panel binds
 * (studio_properties_view.c). */
#ifndef CHIBI_STUDIO_PROPERTIES_INTERNAL_H
#define CHIBI_STUDIO_PROPERTIES_INTERNAL_H

#include "editor/studio/studio_internal.h"

/* What Properties shows: a prototype document's prototype, or the selected
 * object, authored or spawned by the running scene. */
typedef struct properties_subject
{
    scene_prototype_asset *asset;
    scene_actor *actor;
    scene_actor spawned; /* what actor points at for a spawned object */
    bool multi;          /* several objects selected: their shared components */
    bool locked; /* structure may not change: the scene runs or replays */
} properties_subject;

void properties_subject_of(studio *s, properties_subject *out);
/* Follows the graph and Members selections into the Node and Member tabs,
 * and keeps s->tab one the subject has; `available` gets each tab's. */
void properties_follow_tabs(studio *s, const properties_subject *subject, bool available[STUDIO_TAB_COUNT]);

studio_tab properties_tab_of(const studio *s, const ecs_component_desc *desc);
const char *properties_tab_label(studio_tab tab);
uint32_t properties_tab_icon(studio_tab tab);
/* A component panel's key: its name past the namespace. */
const char *properties_panel_key(const char *component);
/* Whether the panel `key` shows open, as last folded or opened, else while
 * searching and as it opens first: Transform, Appearance, Orientation, the
 * Scene tab's and Behaviors, and a component just added. */
bool properties_open(const studio *s, const char *key);

/* What a row edits, as its id names it: an object's field, a prototype's,
 * or one of the Scene tab's settings. */
typedef enum
{
    PROPERTIES_OBJECT,
    PROPERTIES_PROTOTYPE,
    PROPERTIES_SCENE,
} properties_row_kind;

typedef enum
{
    SCENE_WORKERS,
    SCENE_SUBSTEPS,
    SCENE_SLEEP,
    SCENE_CONTINUOUS,
    SCENE_FORCE_DRIVE,
    SCENE_HERTZ,
    SCENE_DAMPING,
    SCENE_UI,
    SCENE_SETTING_COUNT,
} properties_scene_setting;

typedef struct properties_row
{
    properties_row_kind kind;
    uint64_t entity; /* an object's */
    ecs_component_id component;
    int field; /* in the component, or a properties_scene_setting */
    char prototype[128];
} properties_row;

/* An id fits an emit argument. */
#define PROPERTIES_ROW_ID 128
void properties_row_id(const properties_row *row, char out[PROPERTIES_ROW_ID]);
bool properties_row_parse(const char *id, properties_row *out);

/* The registry's component `id`, or NULL. */
const ecs_component_desc *properties_component(const app_scene_authoring *scene, ecs_component_id id);
/* The prototype component `desc` of the actor's prototype, or `added`, which
 * has no data, when its prototype lacks one. */
const game_prototype_component *properties_base(
    const scene_actor *actor,
    const ecs_component_desc *desc,
    game_prototype_component *added
);
/* Whether the object's field differs from its prototype's. */
bool properties_overridden(
    const app_scene_authoring *scene,
    const scene_actor *actor,
    const game_prototype_component *component,
    int field
);
/* Several objects are selected and `actor` is the active one. */
bool properties_multi(studio *s, const scene_actor *actor);
/* Whether another selected object's field differs from `value`. */
bool properties_mixed(
    studio *s,
    const scene_actor *actor,
    ecs_component_id component,
    int field,
    const pin_value *value
);
/* The other selected objects that have `component`, by actor index (an
 * array the caller frees). */
int *properties_others(studio *s, ecs_component_id component);
/* A 2-4 axis float vector's type, or NULL. */
const type_desc *properties_vector(const pin_value *value);
/* The kind of asset a text field names, or REFERENCE_ASSET_KIND_COUNT. */
reference_asset_kind properties_named(
    const ecs_component_desc *desc,
    const type_field *field,
    const pin_value *value
);
/* The field's label; false when the search leaves it out. */
bool properties_label(
    studio *s,
    const game_prototype_component *component,
    const type_field *field,
    char *label,
    size_t size
);
/* Why a structural control is disabled while a run holds the scene, for its
 * tip; NULL otherwise. */
const char *properties_while_playing(const studio *s, bool locked, const char *reason);

/* The row an asset dragged over Properties hovers, by id, and the asset;
 * false when none (studio_properties_view.c). */
bool properties_drop_over(studio *s, char row[PROPERTIES_ROW_ID], char *asset, size_t size);

#endif
