#include "editor/workspace/viewport.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/workspace/model_preview.h"
#include "runtime/models/model_inspection.h"
#include "render/shader_images.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(bool, studio_model_download, (const char *directory, const char *name), {
    return Boolean(
        Module.chibiDownloadModelBundle &&
        Module.chibiDownloadModelBundle(UTF8ToString(directory), UTF8ToString(name))
    );
})
#endif

static void bake_controls(studio *s, const editor *e)
{
    kit *k = s->kit;
    model_preview_bake_settings settings = scene_model_preview_bake_settings(s->scene);
    bool changed = false;
#ifndef __EMSCRIPTEN__
    kit_prop_begin(k, "bake-directory", "New bundle directory");
    changed = kit_text_field(
        k,
        "value",
        settings.destination,
        sizeof settings.destination,
        "New directory; parent must exist",
        0
    );
    kit_prop_end(k);
#endif
    double cell = settings.cell_size;
    kit_prop_begin(k, "bake-cell", "Mesh cell size");
    changed |= kit_number(
        k,
        "value",
        &cell,
        &(kit_number_desc){ .min = 0, .max = 1e6, .precision = 4, .step = .01, .single_precision = true }
    );
    kit_prop_end(k);
    settings.cell_size = (float)cell;
    if (kit_panel_begin(k, "bake-limits", "Bake Limits", NULL, false)) {
        const char *keys[] = { "max-triangles", "max-samples", "max-work" };
        const char *names[] = { "Triangles", "Sample queries", "Node work" };
        const double maximum[] = { MODEL_BAKE_TRIANGLE_MAX, 100000000, 100000000000 };
        double values[] = { settings.max_triangles,
                            (double)settings.max_samples,
                            (double)settings.max_work };
        for (int i = 0; i < 3; i++) {
            kit_prop_begin(k, keys[i], names[i]);
            changed |= kit_number(
                k,
                "value",
                &values[i],
                &(
                    kit_number_desc
                ){ .min = 1, .max = maximum[i], .step = 1, .precision = 0, .integer = true }
            );
            kit_prop_end(k);
        }
        settings.max_triangles = (int)values[0];
        settings.max_samples = (uint64_t)values[1];
        settings.max_work = (uint64_t)values[2];
        kit_push(k, "limits-help", NULL, "column");
        kit_label(k, "value", "dim", "Sample queries include distance and hit/frame evaluations. Node work is a conservative CPU budget, not a GPU instruction count.");
        kit_pop(k);
    }
    kit_panel_end(k);
    if (changed) {
        scene_model_preview_set_bake_settings(s->scene, &settings);
    }
    bool dirty = studio_active_dirty(s);
    kit_push(k, "bake-action", NULL, "column");
    kit_label(k, "bake-help", "dim", dirty ? "Save model source before baking." : "Zero cell size uses automatic resolution. Thin details may be lost.");
    kit_pop(k);
    if (kit_button(
            k,
            "bake",
            &(kit_button_desc){ .label = "Bake Mesh Bundle",
                                .flags = dirty ? KIT_DISABLED : 0 }
        )) {
        model_export_summary summary = { 0 };
        char error[256];
        bool ok = scene_model_preview_bake(s->scene, &summary, error, sizeof error);
#ifdef __EMSCRIPTEN__
        settings = scene_model_preview_bake_settings(s->scene);
        if (ok && !studio_model_download(settings.destination, e->asset->name)) {
            ok = false;
            snprintf(error, sizeof error, "Mesh baked, but browser download could not start.");
        }
#endif
        char message[1152];
#ifdef __EMSCRIPTEN__
        const char *destination = "bundle downloaded";
#else
        const char *destination = settings.destination;
#endif
        snprintf(
            message,
            sizeof message,
            "%d triangles · max residual %.4g · %s",
            summary.triangles,
            summary.max_residual,
            destination
        );
        kit_toast(
            k,
            0,
            &(kit_toast_desc){ .type = ok ? KIT_TOAST_SUCCESS : KIT_TOAST_ERROR,
                               .title = ok ? "Mesh bundle created" : "Could not bake model",
                               .description = ok ? message : error }
        );
    }
}

static bool asset_choice(kit *k, const char *key, int *selected, const char *const *labels, int count, unsigned flags)
{
    return kit_choice(k, key, selected, labels, count, flags);
}

static void reference_controls(studio *s, editor *e, bool shown)
{
    kit *k = s->kit;
    model_reference reference = e->asset->reference;
    unsigned flags = sg_graph_readonly(s) ? KIT_READONLY : 0;
    bool changed = false, committed = false;
    const editor_choices *choices = &s->scene->image_choices;
    const char **labels = NULL;
    apush(labels, "None");
    int selected = 0;
    for (int i = 0; i < choices->count; i++) {
        editor_choice item = editor_choice_at(choices, i);
        apush(labels, item.label);
        if (reference.image && !strcmp(reference.image, item.label)) {
            selected = i + 1;
        }
    }
    if (reference.image && !selected) {
        apush(labels, reference.image);
        selected = acount(labels) - 1;
    }
    kit_prop_begin(k, "reference-image", "Reference image");
    if (asset_choice(k, "value", &selected, labels, acount(labels), flags)) {
        reference.image = (char *)(selected ? labels[selected] : NULL);
        if (reference.span <= 0) {
            reference.span = 2;
        }
        reference.locked = true;
        changed = committed = true;
    }
    kit_prop_end(k);
    if (reference.image) {
        if (kit_checkbox(k, "reference-locked", &reference.locked, "Lock reference alignment", flags)) {
            changed = committed = true;
        }
        const char *names[] = { "Reference X", "Reference Y", "Reference height", "Reference angle" };
        float *values[] = { &reference.center[0],
                            &reference.center[1],
                            &reference.span,
                            &reference.angle };
        for (int i = 0; i < 4; i++) {
            char key[32];
            snprintf(key, sizeof key, "reference-%d", i);
            kit_prop_begin(k, key, names[i]);
            double value = *values[i];
            if (kit_number(
                    k,
                    "value",
                    &value,
                    &(kit_number_desc){
                        .precision = 3,
                        .step = .1,
                        .single_precision = true,
                        .min = i == 2 ? .001 : 0,
                        .max = i == 2 ? 1e6 : 0,
                        .unit = i == 3 ? "rad" : NULL,
                        .flags = flags | (reference.locked ? KIT_READONLY : 0) }
                )) {
                *values[i] = (float)value;
                changed = true;
            }
            committed |= kit_item_committed(k);
            kit_prop_end(k);
        }
        if (shown) {
            float opacity;
            bool visible = scene_model_preview_reference(s->scene, &opacity);
            bool preview_changed = kit_checkbox(k, "reference-visible", &visible, "Show reference overlay", 0);
            double value = opacity;
            kit_prop_begin(k, "reference-opacity", "Overlay opacity");
            preview_changed |= kit_number(
                k,
                "value",
                &value,
                &(kit_number_desc){ .min = 0, .max = 1, .step = .05, .precision = 2, .slider = true }
            );
            kit_prop_end(k);
            if (preview_changed) {
                scene_model_preview_set_reference(s->scene, visible, (float)value);
            }
        }
    }
    if (changed) {
        if (!e->pending_snapshot) {
            e->pending_snapshot = ed_snapshot(e);
        }
        bool valid = behavior_asset_set_model_reference(e->asset, &reference);
        assert(valid);
        e->edit_pending = 1;
    }
    if (committed) {
        ed_commit_snapshot(e);
    }
    afree(labels);
}

static void material_resource_controls(studio *s, int model)
{
    kit *k = s->kit;
    shader_inspection maximum = { 0 };
    int count = 0;
    for (int i = 0; i < MODEL_MATERIAL_SLOT_MAX; i++) {
        model_material_slot slot;
        shader_inspection report;
        if (!render_shader_model_material_slot(model, i, &slot) ||
            !render_shader_material_inspection(render_shader_find(slot.material), &report)) {
            continue;
        }
        count++;
#define MAXIMUM(field)                                                         \
    if (report.field > maximum.field) {                                        \
        maximum.field = report.field;                                          \
    }
        MAXIMUM(nodes)
        MAXIMUM(uniforms)
        MAXIMUM(images)
        MAXIMUM(ramp_vec4s)
        MAXIMUM(texture_samples)
        MAXIMUM(detail_expressions)
#undef MAXIMUM
        char key[24];
        snprintf(key, sizeof key, "material-%d", i);
        if (kit_panel_begin(k, key, slot.name, slot.material, false)) {
            const char *names[] = { "IR nodes",      "Uniforms",
                                    "Images",        "Ramp vec4s",
                                    "Texture calls", "Helper evaluations" };
            int values[] = { report.nodes,           report.uniforms,
                             report.images,          report.ramp_vec4s,
                             report.texture_samples, report.detail_expressions };
            int limits[] = { SHADER_NODE_MAX,
                             SHADER_PARAMETER_MAX,
                             RENDER_SHADER_IMAGE_MAX,
                             RENDER_SHADER_RAMP_VEC4_MAX,
                             0,
                             0 };
            for (int row = 0; row < 6; row++) {
                char key[24], value[64];
                snprintf(key, sizeof key, "resource-%d", row);
                if (limits[row]) {
                    snprintf(value, sizeof value, "%d / %d", values[row], limits[row]);
                } else {
                    snprintf(value, sizeof value, "%d", values[row]);
                }
                kit_prop_begin(k, key, names[row]);
                kit_label(k, "value", NULL, value);
                kit_prop_end(k);
            }
        }
        kit_panel_end(k);
    }
    if (count) {
        char value[192];
        snprintf(
            value,
            sizeof value,
            "IR %d · uniforms %d · images %d · ramps %d · samples ≤ %d · "
            "helper evaluations %d",
            maximum.nodes,
            maximum.uniforms,
            maximum.images,
            maximum.ramp_vec4s,
            maximum.texture_samples,
            maximum.detail_expressions
        );
        kit_prop_begin(k, "material-maximum", "Maximum per material");
        kit_label(k, "value", NULL, value);
        kit_prop_end(k);
        kit_label(k, "material-help", "dim", "Static sample bound per shading evaluation, including detail helpers. Not measured GPU work.");
    }
}

static void inspect_controls(studio *s)
{
    const model_inspection *report = render_shader_model_inspection(
        render_shader_find(scene_model_preview_name(s->scene))
    );
    if (!report) {
        return;
    }
    kit *k = s->kit;
    model_preview_inspection value = scene_model_preview_inspection(s->scene);
    char **labels = NULL;
    apush(labels, xstrdup("Complete model"));
    for (int i = 0; i < acount(report->parts); i++) {
        const model_part_inspection *part = &report->parts[i];
        size_t size = strlen(part->name) + strlen(part->path) + 4;
        char *label = xmalloc(size);
        snprintf(label, size, "%s (%s)", part->name, part->path);
        apush(labels, label);
    }
    int selected = value.part + 1;
    kit_prop_begin(k, "isolation", "Inspect part");
    bool changed = asset_choice(k, "value", &selected, (const char *const *)labels, acount(labels), 0);
    kit_prop_end(k);
    for (int i = 0; i < acount(labels); i++) {
        free(labels[i]);
    }
    afree(labels);
    value.part = selected - 1;
    changed |= kit_checkbox(k, "section", &value.section, "Section cut", 0);
    if (value.section) {
        static const char *const axes[] = { "X", "Y", "Z" };
        kit_prop_begin(k, "section-axis", "Section axis");
        changed |= kit_choice(k, "value", &value.axis, axes, 3, 0);
        kit_prop_end(k);
        double offset = value.offset;
        kit_prop_begin(k, "section-offset", "Section position");
        changed |= kit_number(
            k,
            "value",
            &offset,
            &(kit_number_desc){ .precision = 3, .step = .1, .single_precision = true }
        );
        kit_prop_end(k);
        value.offset = (float)offset;
        kit_label(k, "section-help", "dim", "Keeps model points below the selected plane.");
    }
    if (changed) {
        scene_model_preview_set_inspection(s->scene, value);
    }
    for (int i = 0;; i++) {
        model_anchor anchor;
        float position[3], angles[3];
        if (!scene_model_preview_anchor(s->scene, i, &anchor, position, angles)) {
            break;
        }
        char key[24], value[128];
        snprintf(key, sizeof key, "landmark-%d", i);
        snprintf(
            value,
            sizeof value,
            "(%.4g, %.4g, %.4g) · angles (%.3g, %.3g, %.3g) rad",
            position[0],
            position[1],
            position[2],
            angles[0],
            angles[1],
            angles[2]
        );
        kit_prop_begin(k, key, anchor.name);
        kit_label(k, "value", NULL, value);
        kit_prop_end(k);
    }
}

static void inspection_controls(studio *s, const char *name)
{
    const model_inspection *report = render_shader_model_inspection(
        render_shader_find(name)
    );
    if (!report) {
        return;
    }
    kit *k = s->kit;
    if (kit_panel_begin(k, "resources", "Model Resources", NULL, true)) {
        const char *names[] = { "Distance IR",     "Hit IR",
                                "Uniform slots",   "Material slots",
                                "Geometry images", "Geometry ramp entries" };
        int values[] = { report->distance_nodes,
                         report->hit_nodes,
                         report->public_slots + report->private_slots,
                         report->material_slots,
                         report->images,
                         report->ramp_vec4s };
        int limits[] = { report->distance_limit,  report->hit_limit,
                         report->parameter_limit, report->material_limit,
                         report->image_limit,     report->ramp_limit };
        for (int i = 0; i < 6; i++) {
            char key[24], value[64];
            snprintf(key, sizeof key, "budget-%d", i);
            snprintf(value, sizeof value, "%d / %d", values[i], limits[i]);
            kit_prop_begin(k, key, names[i]);
            kit_label(k, "value", NULL, value);
            kit_prop_end(k);
        }
        material_resource_controls(s, render_shader_find(name));
        kit_label(k, "part-help", "dim", "Part counts include shared work and overlap.");
        int first, last;
        kit_push(k, "cost-list", NULL, NULL);
        char height[48];
        snprintf(
            height,
            sizeof height,
            "height:%ddp;",
            acount(report->parts) < 7 ? acount(report->parts) * 28 : 196
        );
        kit_attr(k, "style", height);
        kit_list_begin(
            k,
            "part-costs",
            NULL,
            &(kit_list_desc){ .count = acount(report->parts),
                              .row_height = 28,
                              .reveal = -1 },
            &first,
            &last
        );
        for (int i = first; i <= last; i++) {
            const model_part_inspection *part = &report->parts[i];
            char key[24], value[96];
            snprintf(key, sizeof key, "cost-%d", i);
            snprintf(value, sizeof value, "%d inclusive distance nodes", part->distance_nodes);
            kit_prop_begin(k, key, part->path);
            kit_label(k, "value", NULL, value);
            kit_prop_end(k);
        }
        kit_list_end(k);
        kit_pop(k);
    }
    kit_panel_end(k);
}

void studio_model_preview_controls(studio *s, const editor *e)
{
    if (e->asset->domain != BEHAVIOR_DOMAIN_MODEL) {
        return;
    }
    kit *k = s->kit;
    app_scene_authoring *scene = s->scene;
    bool shown = scene_model_preview_name(scene) &&
                 !strcmp(scene_model_preview_name(scene), e->asset->name);
    if (kit_panel_begin(k, "model-preview", "Model Preview", NULL, true)) {
        reference_controls(s, sg_graph_editor(s), shown);
        kit_push(k, "show", NULL, "row");
        if (kit_button(
                k,
                "preview",
                &(kit_button_desc){
                    .label = shown ? "Show Preview" : "Preview Model",
                    .icon = KIT_ICON_HEXAGON,
                    .flags = scene->runtime_snapshot_valid ? KIT_DISABLED : 0 }
            )) {
            if (scene_model_preview_start(scene, e->asset->name) == 0) {
                studio_show_editor(s, STUDIO_EDITOR_SCENE);
                shown = true;
            } else {
                kit_toast(
                    k,
                    0,
                    &(kit_toast_desc){ .type = KIT_TOAST_ERROR,
                                       .title = "Cannot preview model",
                                       .description = scene->preview_message }
                );
            }
        }
        kit_pop(k);
        if (shown) {
            inspect_controls(s);
            bake_controls(s, e);
            float dimensions[3];
            if (scene_model_preview_dimensions(scene, dimensions)) {
                char value[96];
                snprintf(
                    value,
                    sizeof value,
                    "X %.4g · Y %.4g · Z %.4g model units",
                    dimensions[0],
                    dimensions[1],
                    dimensions[2]
                );
                kit_prop_begin(k, "dimensions", "Dimensions");
                kit_label(k, "value", NULL, value);
                kit_prop_end(k);
            }
            static const char *const views[] = { "Orbit", "Front", "Side", "Top" };
            int view = scene_model_preview_view(scene);
            kit_prop_begin(k, "view", "View");
            if (kit_choice(k, "value", &view, views, 4, 0)) {
                scene_model_preview_set_view(scene, (model_preview_view)view);
            }
            kit_prop_end(k);
            kit_push(k, "instructions", "p", NULL);
            kit_label(k, "gesture", "dim", "Drag to rotate. Wheel to zoom. Middle drag to pan.");
            kit_pop(k);
            kit_push(k, "actions", NULL, "row");
            if (kit_button(k, "frame", &(kit_button_desc){ .label = "Frame Model", .icon = KIT_ICON_FRAME })) {
                scene_model_preview_frame(scene);
            }
            if (kit_button(
                    k,
                    "reset",
                    &(kit_button_desc){ .label = "Reset Preview Values", .variant = KIT_SUBTLE }
                )) {
                scene_model_preview_reset_parameters(scene);
            }
            kit_pop(k);
            for (int slot = 0; slot < SHADER_PARAMETER_MAX; slot++) {
                model_parameter parameter;
                float values[4];
                if (!scene_model_preview_parameter(scene, slot, &parameter, values)) {
                    continue;
                }
                char key[32];
                snprintf(key, sizeof key, "uniform-%d", slot);
                kit_prop_begin(k, key, parameter.name);
                bool changed = false;
                static const char *const axes[] = { "X", "Y", "Z", "W" };
                for (int axis = 0; axis < parameter.width; axis++) {
                    double value = values[axis];
                    if (kit_number(
                            k,
                            axes[axis],
                            &value,
                            &(kit_number_desc){
                                .min = parameter.minimum[axis],
                                .max = parameter.maximum[axis],
                                .precision = 3,
                                .single_precision = true,
                                .step = .1,
                                .addon = parameter.width > 1 ? axes[axis] : NULL }
                        )) {
                        values[axis] = (float)value;
                        changed = true;
                    }
                }
                if (changed) {
                    scene_model_preview_set_parameter(scene, slot, values);
                }
                kit_prop_end(k);
            }
            const char *error = scene_model_preview_error(scene);
            if (error[0]) {
                kit_label(k, "parameter-error", "notice", error);
            }
            game_preview_definition
                *definition = game_preview_find_definition(scene->live, e->asset->name);
            if (definition && definition->diagnostics.error_count) {
                kit_label(k, "stale", "notice", "Showing last valid model");
                for (int i = 0; i < acount(definition->diagnostics.items); i++) {
                    const compile_diagnostic
                        *diagnostic = &definition->diagnostics.items[i];
                    if (diagnostic->severity == CMPL_ERROR) {
                        kit_label(k, "source-error", "notice", diagnostic->message);
                        break;
                    }
                }
            }
        }
    }
    kit_panel_end(k);
    inspection_controls(s, e->asset->name);
}

void studio_model_reference_overlay(studio *s)
{
    float opacity;
    const char *name = scene_model_preview_name(s->scene);
    if (!name || s->scene->active_runtime_target != APP_RUNTIME_PREVIEW ||
        !scene_model_preview_reference(s->scene, &opacity) || opacity <= 0) {
        return;
    }
    game_preview_definition *definition = game_preview_find_definition(s->scene->live, name);
    if (!definition) {
        return;
    }
    const model_reference *reference = &definition->working->reference;
    const editor *e = sg_graph_editor(s);
    if (e && e->asset->domain == BEHAVIOR_DOMAIN_MODEL &&
        !strcmp(e->asset->name, name)) {
        reference = &e->asset->reference;
    }
    if (!reference->image) {
        return;
    }
    int width, height;
    shader_error error = { 0 };
    char key[1024];
    snprintf(key, sizeof key, "content/%s", reference->image);
    shader_image_source image = { .key = key,
                                  .color_space = SHADER_IMAGE_LINEAR,
                                  .filter = SHADER_IMAGE_BILINEAR,
                                  .wrap = SHADER_IMAGE_CLAMP };
    int variant = render_image_variant(&image, &error);
    if (!variant) {
        kit_label(s->kit, "reference-error", "notice", error.message);
        return;
    }
    CF_Texture texture = render_image_texture(variant, &error);
    if (error.message[0] || !render_image_info(key, &width, &height)) {
        kit_label(s->kit, "reference-error", "notice", error.message);
        return;
    }
    rml_backend_set_texture(s->backend, "model-reference", texture.id);
    engine_viewport_view view;
    engine_viewport_get_view(s->viewport, &view);
    float image_width = reference->span * (float)width / (float)height;
    float left = .5f + (reference->center[0] - view.center_x - .5f * image_width) /
                           view.width;
    float top = .5f + (-reference->center[1] - view.center_y - .5f * reference->span) /
                          view.height;
    char style[512];
    snprintf(
        style,
        sizeof style,
        "position:absolute;pointer-events:none;left:%.6g%%;top:%.6g%%;width:%."
        "6g%%;height:%.6g%%;opacity:%.6g;transform:rotate(%.6grad);",
        left * 100,
        top * 100,
        image_width / view.width * 100,
        reference->span / view.height * 100,
        opacity,
        reference->angle
    );
    kit_push(s->kit, "reference-frame", NULL, NULL);
    kit_attr(s->kit, "style", "position:absolute;left:0;top:0;width:100%;height:100%;overflow:hidden;clip:always;pointer-events:none;");
    kit_push(s->kit, "model-reference", "img", NULL);
    kit_attr(s->kit, "src", "kit:model-reference");
    kit_attr(s->kit, "style", style);
    kit_pop(s->kit);
    kit_pop(s->kit);
}
