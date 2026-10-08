#include "editor/ui/labels.h"

#include "graph/model/registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_node(const char *name, const char *expected)
{
    const node_desc *node = registry_node_by_name(name);
    assert(node);
    char label[128];
    editor_node_label(node, label, sizeof label);
    assert(strcmp(label, expected) == 0);
    assert(strcmp(node->name, name) == 0);
}

static void expect_component(const char *name, const char *expected)
{
    char label[128];
    editor_component_label(name, label, sizeof label);
    assert(strcmp(label, expected) == 0);
}

void test_labels()
{
    expect_component("GameTransform", "Transform");
    expect_component("GameGeometry3D", "3D Geometry");
    expect_component("GameAppearance", "2D Appearance");
    expect_component("GameSdfMotion", "SDF Motion");
    expect_component("GameMaterial", "Material");
    expect_component("GameHTTPResponse", "HTTP Response");
    expect_component("CF_V2", "CF V2");
    expect_component("CustomGameState", "Custom Game State");
    expect_component("GameplayState", "Gameplay State");
    expect_component("Game", "Game");
    expect_component("SdfSurface", "SDF Surface");
    expect_component("SdfishSurface", "Sdfish Surface");
    char small[] = { 'L', 'x', 'x', 'x', 'x', 'R' };
    editor_component_label("GameSdfMotion", small + 1, 0);
    assert(memcmp(small, "LxxxxR", sizeof small) == 0);
    editor_component_label("GameSdfMotion", NULL, 0);
    editor_component_label("GameSdfMotion", small + 1, 4);
    assert(strcmp(small + 1, "SDF") == 0 && small[0] == 'L' && small[5] == 'R');
    editor_component_label("GameMaterial", small + 1, 1);
    assert(small[1] == 0 && small[0] == 'L' && small[5] == 'R');
    expect_node("Get_GameTransform", "Get Transform");
    expect_node("Set_GameHealth", "Set Health");
    expect_node("Set_GameHealth_current", "Set Health Current");
    expect_node("Set_GameHealth_maximum", "Set Health Maximum");
    expect_node("GetEntity_GameCollider", "Get Entity Collider");
    expect_node("SetEntity_GameTransform", "Set Entity Transform");
    expect_node("Make_GameProjectile", "Make Projectile");
    expect_node("Break_GameInput", "Break Input");
    expect_node("Make_CF_V2", "Make CF V2");
    expect_node("Shader Color Ramp", "Color Ramp");
    expect_node("Shader Surface Output", "Surface Output");
    char label[128];
    editor_identifier_label("GameOver", label, sizeof label);
    assert(strcmp(label, "Game Over") == 0);
    editor_identifier_label("Get_GameTransform", label, sizeof label);
    assert(strcmp(label, "Get Game Transform") == 0);
    node_desc authored = { .name = "Get_GameScore" };
    editor_node_label(&authored, label, sizeof label);
    assert(strcmp(label, "Get Game Score") == 0);
    const node_desc *reflected = registry_node_by_name("Get_GameTransform");
    authored.type = reflected->type;
    authored.name = "My_GameTransform";
    editor_node_label(&authored, label, sizeof label);
    assert(strcmp(label, "My Game Transform") == 0);
    authored.name = "Get_GameScore";
    editor_node_label(&authored, label, sizeof label);
    assert(strcmp(label, "Get Game Score") == 0);
    char guarded[] = { 'L', 'x', 'x', 'x', 'x', 'R' };
    editor_node_label(reflected, guarded + 1, 4);
    assert(guarded[0] == 'L' && guarded[5] == 'R' && guarded[4] == 0);
    assert(strcmp(guarded + 1, "Get") == 0);
    editor_node_label(reflected, guarded + 1, 1);
    assert(guarded[1] == 0 && guarded[0] == 'L' && guarded[5] == 'R');
    puts("labels PASS: reflected component prefixes stay out of display names; authored identities are preserved");
}
