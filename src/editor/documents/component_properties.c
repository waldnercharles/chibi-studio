#include "editor/documents/component_properties.h"

#include "editor/ui/labels.h"
#include "runtime/component_catalog.h"

#include <stddef.h>
#include <stdio.h>

static const editor_choice SHAPES[] = {
    { SHAPE_BOX, "Box" },
    { SHAPE_CIRCLE, "Circle" },
    { SHAPE_DIAMOND, "Diamond" },
    { SHAPE_CHEVRON, "Chevron" },
    { SHAPE_STROKED_CHEVRON, "Chevron Outline" },
    { SHAPE_CAPSULE, "Capsule" },
    { SHAPE_TRIANGLE, "Triangle" },
    { SHAPE_STROKED_TRIANGLE, "Triangle Outline" },
};
static const editor_choices APPEARANCE_SHAPES = { SHAPES, 8 };
static const editor_choice PRIMITIVES[] = {
    { PRIMITIVE_BOX, "Box" },
    { PRIMITIVE_SPHERE, "Sphere" },
    { PRIMITIVE_CAPSULE, "Capsule" },
    { PRIMITIVE_CONE, "Cone" },
    { PRIMITIVE_TRIANGLE_PRISM, "Triangle Prism" },
    { PRIMITIVE_OCTAHEDRON, "Octahedron" },
    { PRIMITIVE_TETRAHEDRON, "Tetrahedron" },
    { PRIMITIVE_CHEVRON_PYRAMID, "Chevron Pyramid" },
    { PRIMITIVE_BOX_FRAME, "Box Frame" },
    { PRIMITIVE_TRIANGLE_OUTLINE, "Triangle Outline" },
    { PRIMITIVE_TRIANGLE, "Triangle" },
};
static const editor_choices SPATIAL_PRIMITIVES = { PRIMITIVES, PRIMITIVE_COUNT };
static const editor_choice PARTICLE_SHAPES[] = {
    { PARTICLE_ROUND, "Round" },
    { PARTICLE_TRIANGLE, "Triangle" },
    { PARTICLE_TRIANGLE_OUTLINE, "Triangle Outline" },
    { PARTICLE_RING, "Ring" },
    { PARTICLE_STREAK, "Streak" },
};
static const editor_choices EMITTER_SHAPES = { PARTICLE_SHAPES, PARTICLE_SHAPE_COUNT };
static const editor_choice BODY_TYPES[] = {
    { BODY_STATIC, "Static" },
    { BODY_DYNAMIC, "Dynamic" },
    { BODY_KINEMATIC, "Kinematic" },
};
static const editor_choices PHYSICS_BODIES = { BODY_TYPES, 3 };
static const editor_choice COLLIDER_SHAPES[] = {
    { COLLIDER_CIRCLE, "Circle" },
    { COLLIDER_BOX, "Box" },
};
static const editor_choices PHYSICS_COLLIDERS = { COLLIDER_SHAPES, 2 };
static const editor_choice ENCOUNTER_STATES[] = {
    { ENCOUNTER_ACTIVE, "Active" },
    { ENCOUNTER_CLEARED, "Cleared" },
    { ENCOUNTER_DEFEATED, "Defeated" },
};
static const editor_choices ENCOUNTER_STATUS = { ENCOUNTER_STATES, 3 };
static const editor_choice ENCOUNTER_ROLES[] = {
    { ENCOUNTER_PLAYER, "Player" },
    { ENCOUNTER_ENEMY, "Enemy" },
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
    if (type == COMPONENT_Appearance.type &&
        field->offset == offsetof(Appearance, shape)) {
        return &APPEARANCE_SHAPES;
    }
    if (type == COMPONENT_Geometry3D.type &&
        field->offset == offsetof(Geometry3D, primitive)) {
        return &SPATIAL_PRIMITIVES;
    }
    if (type == COMPONENT_ParticleEmitter.type &&
        field->offset == offsetof(ParticleEmitter, shape)) {
        return &EMITTER_SHAPES;
    }
    if (type == COMPONENT_Body.type && field->offset == offsetof(Body, type)) {
        return &PHYSICS_BODIES;
    }
    if (type == COMPONENT_Collider.type && field->offset == offsetof(Collider, shape)) {
        return &PHYSICS_COLLIDERS;
    }
    if (type == COMPONENT_Encounter.type &&
        field->offset == offsetof(Encounter, status)) {
        return &ENCOUNTER_STATUS;
    }
    if (type == COMPONENT_EncounterMember.type &&
        field->offset == offsetof(EncounterMember, role)) {
        return &ENCOUNTER_MEMBERS;
    }
    return field->type == PIN_ENUM ? editor_choices_for_enum(field->type_name) : NULL;
}

bool property_field_visible(const ecs_component_desc *component, const type_field *field, const void *data)
{
    if (component->id != COMPONENT_Collider.id) {
        return true;
    }
    const Collider *collider = data;
    if (field->offset == offsetof(Collider, radius)) {
        return collider->shape != COLLIDER_BOX;
    }
    if (field->offset == offsetof(Collider, width) ||
        field->offset == offsetof(Collider, height)) {
        return collider->shape != COLLIDER_CIRCLE;
    }
    return true;
}

void property_label(const prototype_component *component, const type_field *field, char *label, size_t size)
{
    if (component->desc->id == COMPONENT_Material.id) {
        if (field->offset == offsetof(Material, scene)) {
            snprintf(label, size, "Base Color");
            return;
        }
        if (field->offset == offsetof(Material, border_scene)) {
            snprintf(label, size, "Border Color");
            return;
        }
    }
    editor_identifier_label(field->name, label, size);
}
