#include "editor/documents/component_properties.h"

#include "editor/ui/labels.h"
#include "runtime/component_catalog.h"

#include <stddef.h>
#include <stdio.h>

static const editor_choice SHAPES[] = {
    { GAME_SHAPE_BOX, "Box" },
    { GAME_SHAPE_CIRCLE, "Circle" },
    { GAME_SHAPE_DIAMOND, "Diamond" },
    { GAME_SHAPE_CHEVRON, "Chevron" },
    { GAME_SHAPE_STROKED_CHEVRON, "Chevron Outline" },
    { GAME_SHAPE_CAPSULE, "Capsule" },
    { GAME_SHAPE_TRIANGLE, "Triangle" },
    { GAME_SHAPE_STROKED_TRIANGLE, "Triangle Outline" },
};
static const editor_choices APPEARANCE_SHAPES = { SHAPES, 8 };
static const editor_choice PRIMITIVES[] = {
    { GAME_PRIMITIVE_BOX, "Box" },
    { GAME_PRIMITIVE_SPHERE, "Sphere" },
    { GAME_PRIMITIVE_CAPSULE, "Capsule" },
    { GAME_PRIMITIVE_CONE, "Cone" },
    { GAME_PRIMITIVE_TRIANGLE_PRISM, "Triangle Prism" },
    { GAME_PRIMITIVE_OCTAHEDRON, "Octahedron" },
    { GAME_PRIMITIVE_TETRAHEDRON, "Tetrahedron" },
    { GAME_PRIMITIVE_CHEVRON_PYRAMID, "Chevron Pyramid" },
    { GAME_PRIMITIVE_BOX_FRAME, "Box Frame" },
    { GAME_PRIMITIVE_TRIANGLE_OUTLINE, "Triangle Outline" },
    { GAME_PRIMITIVE_TRIANGLE, "Triangle" },
};
static const editor_choices SPATIAL_PRIMITIVES = { PRIMITIVES, GAME_PRIMITIVE_COUNT };
static const editor_choice PARTICLE_SHAPES[] = {
    { GAME_PARTICLE_ROUND, "Round" },
    { GAME_PARTICLE_TRIANGLE, "Triangle" },
    { GAME_PARTICLE_TRIANGLE_OUTLINE, "Triangle Outline" },
    { GAME_PARTICLE_RING, "Ring" },
    { GAME_PARTICLE_STREAK, "Streak" },
};
static const editor_choices EMITTER_SHAPES = { PARTICLE_SHAPES,
                                               GAME_PARTICLE_SHAPE_COUNT };
static const editor_choice BODY_TYPES[] = {
    { GAME_BODY_STATIC, "Static" },
    { GAME_BODY_DYNAMIC, "Dynamic" },
    { GAME_BODY_KINEMATIC, "Kinematic" },
};
static const editor_choices PHYSICS_BODIES = { BODY_TYPES, 3 };
static const editor_choice COLLIDER_SHAPES[] = {
    { GAME_COLLIDER_CIRCLE, "Circle" },
    { GAME_COLLIDER_BOX, "Box" },
};
static const editor_choices PHYSICS_COLLIDERS = { COLLIDER_SHAPES, 2 };
static const editor_choice ENCOUNTER_STATES[] = {
    { GAME_ENCOUNTER_ACTIVE, "Active" },
    { GAME_ENCOUNTER_CLEARED, "Cleared" },
    { GAME_ENCOUNTER_DEFEATED, "Defeated" },
};
static const editor_choices ENCOUNTER_STATUS = { ENCOUNTER_STATES, 3 };
static const editor_choice ENCOUNTER_ROLES[] = {
    { GAME_ENCOUNTER_PLAYER, "Player" },
    { GAME_ENCOUNTER_ENEMY, "Enemy" },
};
static const editor_choices ENCOUNTER_MEMBERS = { ENCOUNTER_ROLES, 2 };
bool appearance_color_field(const type_desc *, const type_field *field)
{
    return field && field->type == PIN_U32 && field->color;
}

const editor_choices *property_choices(const type_desc *type, const type_field *field)
{
    if (!field || (field->type != PIN_I32 && field->type != PIN_ENUM)) {
        return NULL;
    }
    if (type == GAME_COMPONENT_GameAppearance.type &&
        field->offset == offsetof(GameAppearance, shape)) {
        return &APPEARANCE_SHAPES;
    }
    if (type == GAME_COMPONENT_GameGeometry3D.type &&
        field->offset == offsetof(GameGeometry3D, primitive)) {
        return &SPATIAL_PRIMITIVES;
    }
    if (type == GAME_COMPONENT_GameParticleEmitter.type &&
        field->offset == offsetof(GameParticleEmitter, shape)) {
        return &EMITTER_SHAPES;
    }
    if (type == GAME_COMPONENT_GameBody.type &&
        field->offset == offsetof(GameBody, type)) {
        return &PHYSICS_BODIES;
    }
    if (type == GAME_COMPONENT_GameCollider.type &&
        field->offset == offsetof(GameCollider, shape)) {
        return &PHYSICS_COLLIDERS;
    }
    if (type == GAME_COMPONENT_GameEncounter.type &&
        field->offset == offsetof(GameEncounter, status)) {
        return &ENCOUNTER_STATUS;
    }
    if (type == GAME_COMPONENT_GameEncounterMember.type &&
        field->offset == offsetof(GameEncounterMember, role)) {
        return &ENCOUNTER_MEMBERS;
    }
    return field->type == PIN_ENUM ? editor_choices_for_enum(field->type_name) : NULL;
}

bool property_field_visible(const ecs_component_desc *component, const type_field *field, const void *data)
{
    if (component->id != GAME_COMPONENT_GameCollider.id) {
        return true;
    }
    const GameCollider *collider = data;
    if (field->offset == offsetof(GameCollider, radius)) {
        return collider->shape != GAME_COLLIDER_BOX;
    }
    if (field->offset == offsetof(GameCollider, width) ||
        field->offset == offsetof(GameCollider, height)) {
        return collider->shape != GAME_COLLIDER_CIRCLE;
    }
    return true;
}

void property_label(
    const game_prototype_component *component,
    const type_field *field,
    char *label,
    size_t size
)
{
    if (component->desc->id == GAME_COMPONENT_GameMaterial.id) {
        if (field->offset == offsetof(GameMaterial, scene)) {
            snprintf(label, size, "Base Color");
            return;
        }
        if (field->offset == offsetof(GameMaterial, border_scene)) {
            snprintf(label, size, "Border Color");
            return;
        }
    }
    editor_identifier_label(field->name, label, size);
}
