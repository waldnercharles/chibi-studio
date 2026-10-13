#ifndef STUDIO_ACCEPTANCE_MATERIAL_DEPENDENCIES_H
#define STUDIO_ACCEPTANCE_MATERIAL_DEPENDENCIES_H

#include "graph/document/library.h"
#include "render/shader_catalog.h"

/* Included by the material route: two registered KDL graph definitions, no disk
 * fixture or additional app, renderer, readback or frames. The preview world owns
 * registered definitions; local graph clones are released after each phase. */
static unsigned material_dependency_revision;

static behavior_asset *material_dependency_library(float value)
{
    behavior_asset *library = behavior_asset_new("AcceptanceHeightLibrary");
    library->domain = BEHAVIOR_DOMAIN_SHADER_LIBRARY;
    graph *function = behavior_asset_add_function(library, "Height", true);
    node *result = function ? graph_node_by_id(function, function->return_id) : NULL;
    pin_spec output = { .name = "Value",
                        .kind = PIN_DATA,
                        .type = PIN_F32,
                        .default_value = { .type = PIN_F32 } };
    if (!result || graph_add_pin(function, result, false, &output) != 0 ||
        graph_set_pin_default(function, result, false, 0, &(pin_value){ .type = PIN_F32, .f = value })) {
        behavior_asset_free(library);
        return NULL;
    }
    return library;
}

static bool material_dependency_setup(studio *s)
{
    standard_material defaults = material_authoring_defaults(MATERIAL_AUTHORING_MATTE);
    behavior_asset *material = standard_material_build("AcceptanceDependent", &defaults);
    behavior_asset *library = material_dependency_library(.25f);
    char *material_text = NULL, *library_text = NULL;
    bool valid = false, library_registered = false, material_registered = false;
    preview_definition *library_definition = NULL, *material_definition = NULL;
    char error[512] = { 0 };
    if (!material || !library) {
        goto done;
    }
    graph *function = library->graphs[1];
    node *surface = NULL;
    for (int i = 0; i < acount(material->graph->nodes); i++) {
        node *n = material->graph->nodes[i];
        if (!strcmp(n->desc->name, "Shader Surface Output")) {
            surface = n;
        }
    }
    node *call = graph_add_node(material->graph, &NODE_CallPureFunction);
    if (!surface || !call ||
        graph_library_refresh_call(material->graph, call, function) ||
        graph_set_node_reference_asset(material->graph, call, library->name) ||
        graph_connect_data(material->graph, call, 0, surface, SHADER_SURFACE_HEIGHT)) {
        goto done;
    }
    material_text = behavior_asset_save(material);
    library_text = behavior_asset_save(library);
    if (!material_text || !library_text || strlen(material_text) > 16384 ||
        strlen(library_text) > 16384) {
        goto done;
    }
    library_definition = preview_definition_new(library->name, library);
    if (!library_definition) {
        goto done;
    }
    library = NULL;
    material_definition = preview_definition_new(material->name, material);
    if (!material_definition) {
        goto done;
    }
    material = NULL;
    if (preview_add_definition(s->scene->live, library_definition)) {
        snprintf(error, sizeof error, "Shader library registration failed.");
        goto done;
    }
    library_registered = true;
    if (preview_add_definition(s->scene->live, material_definition)) {
        snprintf(error, sizeof error, "Dependent material registration failed.");
        goto done;
    }
    material_registered = true;
    valid = !app_scene_authoring_open_behavior(s->scene, "AcceptanceDependent");
done:
    if (!valid && error[0]) {
        fprintf(stderr, "Material dependency setup: %s\n", error);
    }
    if (!library_registered) {
        preview_definition_free(library_definition);
    }
    if (!material_registered) {
        preview_definition_free(material_definition);
    }
    free(material_text);
    free(library_text);
    behavior_asset_free(material);
    behavior_asset_free(library);
    return valid;
}

static bool material_dependency_sample(studio *s, float expected)
{
    REQUIRE(!strcmp(s->scene->graph_editor->asset->name, "AcceptanceDependent"));
    REQUIRE(editor_edit_revision(s->scene->graph_editor) == material_dependency_revision);
    REQUIRE(s->shader_report_valid);
    shader_sample_context context = shader_authoring_center_context(
        s->scene->graph_editor->asset
    );
    float outputs[SHADER_OUTPUT_MAX][4];
    shader_error error = { 0 };
    REQUIRE(shader_authoring_sample(&s->shader_report, &context, NULL, outputs, &error));
    if (fabsf(outputs[SHADER_SURFACE_HEIGHT][0] - expected) >= .00001f) {
        fprintf(
            stderr,
            "Material dependency sample: expected=%g actual=%g "
            "edit_revision=%u\n",
            expected,
            outputs[SHADER_SURFACE_HEIGHT][0],
            material_dependency_revision
        );
        return false;
    }
    return true;
}

static bool material_dependency_publish(studio *s)
{
    material_dependency_revision = editor_edit_revision(s->scene->graph_editor);
    REQUIRE(material_dependency_sample(s, .25f));
    const preview_definition *opened = preview_find_definition(s->scene->live, "AcceptanceDependent");
    /* The production comparison ignores only automatic node placement, so a
     * successful match also proves the opened graph has no authored drift. */
    REQUIRE(preview_working_matches(opened, s->scene->graph_editor->asset));
    preview_definition *definition = preview_find_definition(s->scene->live, "AcceptanceHeightLibrary");
    REQUIRE(definition && definition->working);
    behavior_asset *library = behavior_asset_clone(definition->working);
    graph *function = library && acount(library->graphs) == 2 ? library->graphs[1]
                                                              : NULL;
    node *result = function ? graph_node_by_id(function, function->return_id) : NULL;
    bool changed = result && !graph_set_pin_default(
                                 function,
                                 result,
                                 false,
                                 0,
                                 &(pin_value){ .type = PIN_F32, .f = .75f }
                             );
    char *text = changed ? behavior_asset_save(library) : NULL;
    char error[512] = { 0 };
    bool valid = text && strlen(text) <= 16384 &&
                 !preview_sync_working(definition, library) &&
                 !preview_update(s->scene->live, definition, error, sizeof error);
    preview_definition *dependent = preview_find_definition(s->scene->live, "AcceptanceDependent");
    if (valid) {
        shader_authoring_report applied;
        shader_authoring_report_init(&applied);
        shader_error diagnostic = { 0 };
        float outputs[SHADER_OUTPUT_MAX][4] = { 0 };
        valid = dependent && dependent->applied &&
                shader_authoring_inspect(dependent->applied, &applied, &diagnostic);
        if (valid) {
            shader_sample_context context = shader_authoring_center_context(
                dependent->applied
            );
            valid = shader_authoring_sample(&applied, &context, NULL, outputs, &diagnostic) &&
                    fabsf(outputs[SHADER_SURFACE_HEIGHT][0] - .75f) < .00001f;
        }
        if (!valid) {
            fprintf(
                stderr,
                "Material dependency applied: height=%g message=%s\n",
                outputs[SHADER_SURFACE_HEIGHT][0],
                diagnostic.message
            );
        }
        shader_authoring_report_free(&applied);
    }
    free(text);
    behavior_asset_free(library);
    if (!valid) {
        fprintf(stderr, "Material dependency publication: %s\n", error);
    }
    return valid;
}

static bool material_dependency_restore(studio *s)
{
    REQUIRE(!app_scene_authoring_open_behavior(s->scene, "AcceptanceMetal"));
    REQUIRE(!preview_remove_definition(s->scene->live, "AcceptanceDependent"));
    REQUIRE(!preview_remove_definition(s->scene->live, "AcceptanceHeightLibrary"));
    return true;
}

#endif
