#ifndef CHIBI_EDITOR_STUDIO_SCENE_READ_H
#define CHIBI_EDITOR_STUDIO_SCENE_READ_H

#include "authoring/component_presentation.h"
#include "authoring/reference_index.h"
#include "authoring/scene_snapshot.h"

typedef struct studio studio;
typedef struct app_scene_authoring app_scene_authoring;
typedef struct studio_scene_read studio_scene_read;

/* Private, unbound capture contract. This is not a public language type or a
 * live Outliner service. All arrays and strings below belong to the read until
 * studio_scene_read_free; none borrow a scene, descriptor or source registry.
 * Optional source text is copied as "". Identities never pass through F64. */
typedef struct
{
    uint64_t identity; /* Persistent actor identity, distinct from node.entity. */
    const char *prototype_name;
    int prototype; /* Index into prototypes, or -1 when not resolved. */
    int field_first, field_count;
    unsigned view_flags;
    bool selected;
} studio_scene_read_actor;

typedef struct
{
    const char *identity, *name, *path;
    int component_first, component_count;
} studio_scene_read_prototype;

typedef struct
{
    ecs_component_id component;
    const char *type_name;
    component_icon icon; /* Raw enum; authored policy selects priority/glyph. */
} studio_scene_read_component;

typedef struct
{
    ecs_component_id component;
    const char *component_type, *name;
    int field;
    pin_type type; /* The actual PIN_STR or PIN_NAME field classification. */
    reference_asset_kind kind;
} studio_scene_read_field;

typedef struct
{
    ecs_entity entity;
    uint64_t sequence; /* Registry attachment identity, never an array index. */
    const char *name;
    int begin;
} studio_scene_read_attachment;

typedef struct
{
    /* Both arrays have actor_count entries, preserving scene actor order.
     * Parents are complete, earlier actor indices; invalid hierarchy rejects
     * capture rather than sorting, dropping ancestors or repairing it. */
    const authoring_scene_node *nodes;
    const studio_scene_read_actor *actors;
    int actor_count;
    const studio_scene_read_prototype *prototypes;
    int prototype_count;
    const studio_scene_read_component *components;
    int component_count;
    const studio_scene_read_field *fields;
    int field_count;
    /* All explicit attachments in scene->live's registry, in registry order.
     * Authored policy selects by entity and derives each behavior count. */
    const studio_scene_read_attachment *attachments;
    int attachment_count;
    int active;
    ecs_entity rename;
    bool rename_outliner, editable, live;
    const char *filter, *scene_path;
} studio_scene_read_facts;

/* Exactly one zero-initialized, noncopyable scope must belong to each Studio.
 * Only that canonical scope may be passed; the final service must fetch it
 * from Studio, never from authored input. These fields are implementation
 * state, not a caller-supplied authority token. Integration remains unbound.
 *
 * Every scope/capture operation, including free and SDK anchor retain/release,
 * is confined to the same serialized Studio thread. Immutable owned facts do
 * not make the SDK's ordinary-int owner references thread-safe. */
typedef struct
{
    authoring_scene_owner *owner;
    const studio *studio;
    const app_scene_authoring *scene;
    bool entered;
} studio_scene_read_scope;

/* Opens a fresh owner for one verified dispatcher boundary. Nested entry
 * invalidates the outer owner and returns false. A rejected entrant must not
 * dispatch or call end; only a successful begin owns its matching end. */
bool studio_scene_read_begin(studio_scene_read_scope *, studio *);
/* Call BEFORE any exposed scene/editor fact can change, including callbacks,
 * replacement and destruction. Idempotent; denied until the matching end. */
void studio_scene_read_invalidate(studio_scene_read_scope *);
/* Close immediately after dispatch returns, including queue/pause/suspend or
 * failure, BEFORE error reporting, toasts, UI effects or other callbacks. */
void studio_scene_read_end(studio_scene_read_scope *);

/* Captures synchronously using callback-free reads. View-cache synchronization
 * finishes before any producer text is borrowed. Invalid hierarchy or a closed
 * scope returns NULL with an error; neither changes the owner or any UI model.
 * Allocation failure aborts. This does not filter or compute row policy. */
studio_scene_read *studio_scene_read_capture(studio_scene_read_scope *, studio *, char *error, size_t error_size);
/* Historical data remains readable after scope end and producer destruction. */
const studio_scene_read_facts *studio_scene_read_get(const studio_scene_read *);
void studio_scene_read_free(studio_scene_read *);

/* Scene-side currentness only, conditional on the canonical-scope contract.
 * Never return this bool to authored code as permission to publish later.
 * Final publication must check the actual canonical scope AND independent
 * controller/window/mount authority, then copy the complete validated model
 * inside the SAME callback-free native call. First/Count/collapse/reveal may
 * change within a batch or tick without a Studio service effect. No live
 * binding is safe until those invalidation/publication paths are integrated.
 * Paused work keeps historical inputs, never authority across scope end. */
bool studio_scene_read_current(const studio_scene_read_scope *, const studio *, const studio_scene_read *);

#endif
