/* Material/shape authoring: a retained production preview, typed defaults and
 * deliberately opened diagnostics. GPU work runs only when the picture changes. */
#include "authoring/shader_evidence.h"
#include "editor/studio/studio_graph_internal.h"
#include "graph/authoring/graph_edit.h"
#include "graph/compiler/validation.h"
#include "graph/document/serialize.h"
#include "render/shader_catalog.h"
#include "render/shader_preview.h"
#include "runtime/images/catalog.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_video.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_WIDTH 320
#define PREVIEW_HEIGHT 200
#define PI 3.14159265358979323846f

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(bool, shader_download_evidence, (const char *directory), {
    return Boolean(
        Module.chibiDownloadEvidenceBundle &&
        Module.chibiDownloadEvidenceBundle(UTF8ToString(directory))
    );
})
#endif

struct studio_shader
{
    render_shader_preview *renderer;
    CF_Canvas canvas;
    uint64_t identity;
    unsigned revision, catalog_revision, report_catalog_revision;
    const behavior_asset *document;
    unsigned controls_revision;
    bool visible, dirty, drawn;
    int *image_ids;
    uint64_t *image_revisions;
    float yaw, pitch, zoom, time, uv[2];
    float pointer_x, pointer_y;
    char error[512];
    bool editing;
    int editing_slot, editing_width;
    float original[4];
    shader_authoring_evidence evidence;
    shader_authoring_evidence_plan *evidence_plan;
    CF_Canvas evidence_canvas;
    CF_Readback readback;
    uint64_t readback_started;
    int evidence_sequence, evidence_toast;
    char evidence_path[PATH_MAX], evidence_content[PATH_MAX];
};

static void drop_picture(studio *s)
{
    struct studio_shader *v = s->shader;
    rml_backend_set_image(s->backend, "shader-preview", 0);
    render_shader_preview_free(v->renderer);
    v->renderer = NULL;
    if (v->canvas.id) {
        cf_destroy_canvas(v->canvas);
        v->canvas = (CF_Canvas){ 0 };
    }
    v->drawn = false;
    afree(v->image_ids);
    afree(v->image_revisions);
}

void studio_shader_free(studio *s)
{
    if (!s->shader) {
        return;
    }
    drop_picture(s);
    if (s->shader->readback.id) {
        cf_destroy_readback(s->shader->readback);
    }
    if (s->shader->evidence_canvas.id) {
        cf_destroy_canvas(s->shader->evidence_canvas);
    }
    behavior_asset_free((behavior_asset *)s->shader->evidence.snapshot);
    shader_authoring_evidence_free(s->shader->evidence_plan);
    free(s->shader);
    s->shader = NULL;
}

void studio_shader_hide(studio *s)
{
    if (s->shader) {
        s->shader->visible = false;
    }
}

static node *parameter_node(editor *e, int slot)
{
    for (int g = 0; g < acount(e->asset->graphs); g++) {
        for (int i = 0; i < acount(e->asset->graphs[g]->nodes); i++) {
            node *n = e->asset->graphs[g]->nodes[i];
            if (!strcmp(n->desc->name, "Shader Parameter") &&
                n->reference_id == (uint64_t)slot) {
                return n;
            }
        }
    }
    return NULL;
}

/* Automatic publication keeps dirty set on failure. The applied snapshot is
 * the source that actually reached the renderer, including linked libraries. */
static const behavior_asset *published_snapshot(studio *s, const editor *e)
{
    preview_definition
        *definition = preview_find_definition(s->scene->live, e->asset->name);
    if (!definition || !definition->applied || definition->dirty ||
        !preview_working_matches(definition, e->asset)) {
        return NULL;
    }
    return definition->applied;
}

static void refresh_report(studio *s, editor *e)
{
    const behavior_asset *applied = published_snapshot(s, e);
    const behavior_asset *asset = applied     ? applied
                                : e->compiled ? e->compiled
                                              : e->asset;
    unsigned catalog = render_shader_revision();
    if (s->shader_report_identity == asset->identity &&
        s->shader_report_revision == e->edit_revision &&
        s->shader->report_catalog_revision == catalog) {
        return;
    }
    s->shader->report_catalog_revision = catalog;
    s->shader_report_identity = asset->identity;
    s->shader_report_revision = e->edit_revision;
    const compile_diagnostic *diagnostic = applied
                                             ? NULL
                                             : compile_report_first_error(&e->compile);
    if (diagnostic) {
        s->shader_report_valid = false;
        s->shader_report_error.node = diagnostic->node_id;
        snprintf(
            s->shader_report_error.message,
            sizeof s->shader_report_error.message,
            "%s",
            diagnostic->message
        );
    } else {
        s->shader_report_valid = shader_authoring_inspect(asset, &s->shader_report, &s->shader_report_error);
    }
    s->shader_probe_attempted = false;
    s->shader->dirty = true;
}

static void dirty(studio *s)
{
    s->shader->dirty = true;
    s->shader_probe_attempted = false;
    s->ui_schedule.wake = true;
}

static bool images_changed(const struct studio_shader *v)
{
    for (int i = 0; i < acount(v->image_ids); i++) {
        uint64_t revision = 0;
        shader_image_source source;
        shader_image_view view;
        if (!render_image_sampler(v->image_ids[i], &source, &view, &revision) ||
            revision != v->image_revisions[i]) {
            return true;
        }
    }
    return false;
}

static bool remember_images(studio *s)
{
    struct studio_shader *v = s->shader;
    afree(v->image_ids);
    afree(v->image_revisions);
    for (int i = 0; i < acount(s->shader_report.program.images); i++) {
        shader_error error = { 0 };
        int id = render_image_variant_resident(&s->shader_report.program.images[i], &error);
        uint64_t revision = 0;
        shader_image_source source;
        shader_image_view view;
        if (!id || !render_image_sampler(id, &source, &view, &revision)) {
            snprintf(v->error, sizeof v->error, "%s", error.message[0] ? error.message : "Preview image is unavailable.");
            return false;
        }
        apush(v->image_ids, id);
        apush(v->image_revisions, revision);
    }
    return true;
}

static void finish_edit(studio *s, editor *e)
{
    struct studio_shader *v = s->shader;
    if (!e->pending_snapshot) {
        v->editing = false;
    }
    if (!v->editing) {
        return;
    }
    if (memcmp(
            s->shader_report.defaults[v->editing_slot],
            v->original,
            (size_t)v->editing_width * sizeof(float)
        )) {
        ed_commit_snapshot(e);
    } else {
        ed_cancel_snapshot(e);
    }
    v->editing = false;
}

/* Document history owns a pending gesture even if its controls disappear.
 * Number-field Escape restores the value without adding an empty undo step;
 * the shared color picker commits when closed. */
static bool edit_parameter(studio *s, editor *e, int slot, const float values[4], bool committed)
{
    struct studio_shader *v = s->shader;
    if (!e->pending_snapshot) {
        v->editing = false;
    }
    shader_authoring_report *report = &s->shader_report;
    int width = report->widths[slot];
    bool changed = memcmp(values, report->defaults[slot], (size_t)width * sizeof(float)) !=
                   0;
    if (changed) {
        if (v->editing && v->editing_slot != slot) {
            finish_edit(s, e);
        }
        bool started = !v->editing;
        if (!v->editing) {
            ed_commit_snapshot(e);
            e->pending_snapshot = ed_snapshot(e);
            v->editing = true;
            v->editing_slot = slot;
            v->editing_width = width;
            memcpy(v->original, report->defaults[slot], sizeof v->original);
        }
        shader_error error = { 0 };
        if (!shader_authoring_set_default(e->asset, slot, values, width, &error)) {
            if (started) {
                ed_cancel_snapshot(e);
                v->editing = false;
            }
            studio_error(s, "Could not change parameter", error.message);
            return false;
        }
        memcpy(report->defaults[slot], values, sizeof report->defaults[slot]);
        e->edit_pending = 1;
        dirty(s);
    }
    if (committed && v->editing && v->editing_slot == slot) {
        finish_edit(s, e);
    }
    return changed;
}

static void export_result(studio *s, bool ok, const char *error)
{
    struct studio_shader *v = s->shader;
#ifdef __EMSCRIPTEN__
    if (ok && !shader_download_evidence(v->evidence_path)) {
        ok = false;
        error = "Evidence was created, but the browser download could not "
                "start.";
    }
#endif
    v->evidence_toast = kit_toast(
        s->kit,
        v->evidence_toast,
        &(kit_toast_desc){ .type = ok ? KIT_TOAST_SUCCESS : KIT_TOAST_ERROR,
                           .title = ok ? "Evidence exported" : "Could not export evidence",
                           .description = ok ? v->evidence_path : error }
    );
    cf_destroy_readback(v->readback);
    v->readback = (CF_Readback){ 0 };
    cf_destroy_canvas(v->evidence_canvas);
    v->evidence_canvas = (CF_Canvas){ 0 };
    behavior_asset_free((behavior_asset *)v->evidence.snapshot);
    v->evidence.snapshot = NULL;
    shader_authoring_evidence_free(v->evidence_plan);
    v->evidence_plan = NULL;
    s->ui_schedule.wake = true;
}

static void poll_export(studio *s)
{
    struct studio_shader *v = s->shader;
    if (!v->readback.id) {
        return;
    }
    if (!cf_readback_ready(v->readback)) {
        if (cf_get_ticks() - v->readback_started > cf_get_tick_frequency() * 10) {
            export_result(s, false, "Preview readback timed out.");
        }
        s->ui_schedule.wake = true;
        return;
    }
    int size = PREVIEW_WIDTH * PREVIEW_HEIGHT * (int)sizeof(CF_Pixel);
    CF_Pixel *pixels = xmalloc((size_t)size);
    bool ok = cf_readback_size(v->readback) == size &&
              cf_readback_data(v->readback, pixels, size) == size;
    void *png = NULL;
    int bytes = 0;
    CF_Image image = { PREVIEW_WIDTH, PREVIEW_HEIGHT, pixels };
    ok = ok && !cf_is_error(cf_image_save_png_to_memory(&image, &png, &bytes)) &&
         bytes > 0;
    char error[512] = "Could not encode the rendered preview.";
    if (ok) {
        ok = shader_authoring_evidence_finalize(v->evidence_plan, png, (size_t)bytes, error, sizeof error);
    }
    cf_free(png);
    free(pixels);
    export_result(s, ok, error);
}

static void export_evidence(studio *s, editor *e)
{
    struct studio_shader *v = s->shader;
    const behavior_asset *snapshot = published_snapshot(s, e);
    if (!snapshot) {
        studio_error(s, "Could not export evidence", "The current source has not produced a valid preview.");
        return;
    }
    char folder[PATH_MAX];
    snprintf(folder, sizeof folder, "%s/.chibi/evidence", s->project_root);
    if (!SDL_CreateDirectory(folder)) {
        studio_error(s, "Could not export evidence", SDL_GetError());
        return;
    }
    SDL_PathInfo info;
    do {
        snprintf(v->evidence_path, sizeof v->evidence_path, "%s/shader-%06d", folder, ++v->evidence_sequence);
    } while (SDL_GetPathInfo(v->evidence_path, &info));
    snprintf(v->evidence_content, sizeof v->evidence_content, "%s", s->scene->content_root);
    shader_sample_context context = shader_authoring_center_context(e->asset);
    context.time = v->time;
    memcpy(context.uv, v->uv, sizeof context.uv);
    context.resolve_image = render_image_resolve;
    v->evidence = (shader_authoring_evidence){
        .snapshot = behavior_asset_clone(snapshot),
        .content_root = v->evidence_content,
        .destination = v->evidence_path,
        .cpu_attempted = true,
        .cpu_context = context,
        .width = PREVIEW_WIDTH,
        .height = PREVIEW_HEIGHT,
        .time = v->time,
        .yaw = v->yaw,
        .pitch = v->pitch,
        .zoom = v->zoom,
        .engine_version = CHIBI_STUDIO_ENGINE_VERSION,
        .engine_build_id = CHIBI_STUDIO_SDK_BUILD_ID,
        .engine_manifest_hash = CHIBI_STUDIO_SDK_MANIFEST_SHA256,
        .backend = cf_backend_type_to_string(cf_query_backend()),
        .video_driver = SDL_GetCurrentVideoDriver(),
        .edit_revision = e->edit_revision,
    };
    v->evidence.cpu_valid = shader_authoring_sample(
        &s->shader_report,
        &context,
        NULL,
        v->evidence.cpu_outputs,
        &v->evidence.cpu_error
    );
    char error[512];
    v->evidence_plan = shader_authoring_evidence_prepare(&v->evidence, error, sizeof error);
    if (!v->evidence_plan) {
        behavior_asset_free((behavior_asset *)v->evidence.snapshot);
        v->evidence.snapshot = NULL;
        studio_error(s, "Could not export evidence", error);
        return;
    }
    v->readback = cf_canvas_readback(v->canvas);
    if (!v->readback.id) {
        behavior_asset_free((behavior_asset *)v->evidence.snapshot);
        v->evidence.snapshot = NULL;
        shader_authoring_evidence_free(v->evidence_plan);
        v->evidence_plan = NULL;
        studio_error(s, "Could not export evidence", "Preview readback could not start.");
        return;
    }
    v->evidence_canvas = v->canvas;
    v->canvas = (CF_Canvas){ 0 };
    v->readback_started = cf_get_ticks();
    v->evidence_toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_LOADING, .title = "Exporting evidence", .description = "Capturing this preview and its reproducible inputs." }
    );
    v->dirty = true;
    s->ui_schedule.wake = true;
}

static uint32_t color_byte(float value)
{
    return (uint32_t)lroundf(fminf(fmaxf(value, 0), 1) * 255);
}

static void parameter_controls(studio *s, editor *e)
{
    kit *k = s->kit;
    shader_authoring_report *report = &s->shader_report;
    bool changed = false;
    if (kit_panel_begin(k, "parameters", "Parameters", NULL, true)) {
        if (!report->costs.uniforms) {
            kit_label(k, "empty", "shader-note", "Expose a parameter in the graph to edit its default here.");
        }
        for (int i = 0; i < SHADER_PARAMETER_MAX; i++) {
            int width = report->widths[i];
            if (!width) {
                continue;
            }
            node *source = parameter_node(e, i);
            unsigned flags = sg_graph_readonly(s) || !source ? KIT_READONLY : 0;
            char key[48];
            snprintf(key, sizeof key, "parameter-%u-%d", s->shader->controls_revision, i);
            kit_push(k, key, NULL, width > 1 ? "shader-vector" : "shader-scalar");
            kit_prop_begin(k, "property", report->names[i]);
            float values[4];
            memcpy(values, report->defaults[i], sizeof values);
            bool committed = false;
            const pin_spec *pin = source && source->output_count
                                    ? &source->output_pins[0]
                                    : NULL;
            if (pin && pin->type == PIN_BOOL) {
                bool value = values[0] != 0;
                bool edit = kit_checkbox(k, "value", &value, NULL, flags);
                values[0] = value;
                committed = edit;
            } else {
                if (report->presentation[i] == SHADER_AUTHORING_COLOR) {
                    uint32_t rgba = color_byte(values[0]) << 24 |
                                    color_byte(values[1]) << 16 |
                                    color_byte(values[2]) << 8 |
                                    (width == 4 ? color_byte(values[3]) : 255);
                    if (kit_color(k, "color", &rgba, flags)) {
                        for (int c = 0; c < width; c++) {
                            values[c] = (float)((rgba >> (24 - c * 8)) & 255) / 255;
                        }
                    }
                    committed |= kit_item_committed(k);
                    kit_tip(k, "Choose color", NULL, "Numeric components preserve values above one.");
                }
                kit_push(k, "components", NULL, "shader-components");
                const char *axes = report->presentation[i] == SHADER_AUTHORING_COLOR
                                     ? "RGBA"
                                     : "XYZW";
                for (int c = 0; c < width; c++) {
                    char component[8], addon[2] = { axes[c], 0 };
                    snprintf(component, sizeof component, "c%d", c);
                    double value = values[c];
                    kit_number(
                        k,
                        component,
                        &value,
                        &(kit_number_desc){ .precision = 4,
                                            .step = .01,
                                            .single_precision = true,
                                            .integer = pin && pin->type == PIN_I32,
                                            .addon = width > 1 ? addon : NULL,
                                            .flags = flags }
                    );
                    kit_attr(k, "aria-label", width == 1 ? report->names[i] : addon);
                    values[c] = (float)value;
                    committed |= kit_item_committed(k);
                }
                kit_pop(k);
            }
            changed |= edit_parameter(s, e, i, values, committed);
            kit_prop_end(k);
            kit_pop(k);
        }
    }
    kit_panel_end(k);
    if (changed) {
        ed_compile(e);
        s->shader_report_revision = UINT_MAX;
    }
}

static void texture_controls(studio *s, editor *e)
{
    kit *k = s->kit;
    bool has_images = false;
    for (int g = 0; g < acount(e->asset->graphs); g++) {
        for (int i = 0; i < acount(e->asset->graphs[g]->nodes); i++) {
            has_images |= !strcmp(e->asset->graphs[g]->nodes[i]->desc->name, "Shader Image Texture");
        }
    }
    if (!has_images) {
        return;
    }
    if (kit_panel_begin(k, "textures", "Textures", NULL, true)) {
        const editor_choices *choices = &s->scene->image_choices;
        kit_command_item *items = NULL;
        for (int i = 0; i < choices->count; i++) {
            editor_choice choice = editor_choice_at(choices, i);
            apush(items, ((kit_command_item){ .label = choice.label, .icon = KIT_ICON_IMAGE }));
        }
        int ordinal = 0;
        bool changed = false;
        for (int g = 0; g < acount(e->asset->graphs); g++) {
            graph *owner = e->asset->graphs[g];
            for (int i = 0; i < acount(owner->nodes); i++) {
                node *n = owner->nodes[i];
                if (strcmp(n->desc->name, "Shader Image Texture")) {
                    continue;
                }
                char key[48], label[32];
                snprintf(
                    key,
                    sizeof key,
                    "image-%u-%llu",
                    s->shader->controls_revision,
                    (unsigned long long)n->id
                );
                snprintf(label, sizeof label, "Image %d", ++ordinal);
                kit_prop_begin(k, key, label);
                const char *value = n->input_pins[SHADER_IMAGE_INPUT_IMAGE]
                                        .default_value.str;
                const char *shown = value && !strncmp(value, "content/", 8)
                                      ? value + 8
                                      : value;
                kit_autocomplete_result choice = kit_autocomplete(
                    k,
                    "asset",
                    shown,
                    items,
                    acount(items),
                    "Choose image",
                    sg_graph_readonly(s) ? KIT_READONLY : 0
                );
                if (choice.selected >= 0 && choice.selected < acount(items) &&
                    (!shown || strcmp(shown, items[choice.selected].label))) {
                    ed_snapshot_before(e);
                    char image_key[PATH_MAX];
                    snprintf(
                        image_key,
                        sizeof image_key,
                        "content/%s",
                        items[choice.selected].label
                    );
                    pin_value selected = { .type = PIN_STR, .str = image_key };
                    changed |= graph_set_pin_default(owner, n, false, SHADER_IMAGE_INPUT_IMAGE, &selected) ==
                               0;
                }
                kit_prop_end(k);
            }
        }
        afree(items);
        if (changed) {
            ed_compile(e);
            s->shader_report_revision = UINT_MAX;
            dirty(s);
        }
    }
    kit_panel_end(k);
}

static void view_controls(studio *s, bool material)
{
    struct studio_shader *v = s->shader;
    kit *k = s->kit;
    if (kit_panel_begin(k, "view-settings", "Preview settings", NULL, false)) {
        const char *labels[] = { "Yaw", "Pitch", "Zoom", "Time" };
        float *values[] = { &v->yaw, &v->pitch, &v->zoom, &v->time };
        double minima[] = { -PI, -PI / 2, .25, 0 };
        double maxima[] = { PI, PI / 2, 4, 3600 };
        for (int i = material ? 0 : 2; i < 4; i++) {
            char key[16];
            snprintf(key, sizeof key, "view-%d", i);
            kit_prop_begin(k, key, labels[i]);
            double value = *values[i];
            if (kit_number(
                    k,
                    "value",
                    &value,
                    &(kit_number_desc){ .min = minima[i],
                                        .max = maxima[i],
                                        .precision = 3,
                                        .step = i == 3 ? .1 : .01,
                                        .unit = i < 2  ? "rad"
                                              : i == 3 ? "s"
                                                       : NULL,
                                        .single_precision = true }
                )) {
                *values[i] = (float)value;
                dirty(s);
            }
            kit_prop_end(k);
        }
        kit_label(k, "time-help", "shader-note", "Time stays fixed for repeatable comparisons.");
    }
    kit_panel_end(k);
}

static void report_row(kit *k, const char *key, const char *label, const char *value)
{
    kit_push(k, key, NULL, "shader-stat");
    kit_label(k, "label", "shader-stat-name", label);
    kit_label(k, "value", "shader-stat-value", value);
    kit_pop(k);
}

static void diagnostics(studio *s, editor *e)
{
    kit *k = s->kit;
    struct studio_shader *v = s->shader;
    if (kit_panel_begin(k, "shader-diagnostics", "Diagnostics", NULL, false)) {
        kit_label(k, "scope", "shader-note", "Static shader work, not measured GPU time.");
        const shader_inspection *costs = &s->shader_report.costs;
        const char *labels[] = {
            "Compiled operations", "Parameters",           "Images",
            "Gradient storage",    "Texture sample bound", "Helper evaluations"
        };
        int values[] = { costs->nodes,           costs->uniforms,
                         costs->images,          costs->ramp_vec4s,
                         costs->texture_samples, costs->detail_expressions };
        for (int i = 0; i < 6; i++) {
            char key[24], value[32];
            snprintf(key, sizeof key, "cost-%d", i);
            snprintf(value, sizeof value, "%d", values[i]);
            report_row(k, key, labels[i], value);
        }
        kit_label(k, "sample-heading", "shader-subheading", "CPU sample");
        kit_tip(k, "Numerical shader evaluation", NULL, "Local/world origin; normal and view +Y; zero beat and influence. Values are shader outputs before lighting, not rendered pixels.");
        kit_prop_begin(k, "sample-uv", "UV");
        for (int i = 0; i < 2; i++) {
            double value = v->uv[i];
            if (kit_number(
                    k,
                    i ? "v" : "u",
                    &value,
                    &(kit_number_desc){ .precision = 3,
                                        .step = .01,
                                        .addon = i ? "V" : "U",
                                        .single_precision = true }
                )) {
                v->uv[i] = (float)value;
                s->shader_probe_attempted = false;
            }
        }
        kit_prop_end(k);
        if (kit_button(k, "probe", &(kit_button_desc){ .label = "Evaluate sample", .variant = KIT_OUTLINE })) {
            shader_sample_context context = shader_authoring_center_context(e->asset);
            memcpy(context.uv, v->uv, sizeof context.uv);
            context.time = v->time;
            context.resolve_image = render_image_resolve;
            s->shader_probe_attempted = true;
            s->shader_probe_valid = shader_authoring_sample(
                &s->shader_report,
                &context,
                NULL,
                s->shader_probe,
                &s->shader_probe_error
            );
        }
        if (s->shader_probe_attempted) {
            if (!s->shader_probe_valid) {
                kit_label(k, "sample-error", "notice", s->shader_probe_error.message);
            } else {
                const char *names[] = { "Base color", "Emission", "Roughness",
                                        "Metallic",   "Normal",   "Opacity",
                                        "Unlit",      "Height",   "Occlusion" };
                const int widths[] = { 3, 3, 1, 1, 3, 1, 1, 1, 1 };
                for (int i = 0; i < s->shader_report.program.output_count; i++) {
                    char key[24], value[128] = "";
                    snprintf(key, sizeof key, "sample-%d", i);
                    int width = e->asset->domain == BEHAVIOR_DOMAIN_SHAPE
                                  ? 1
                                  : widths[i];
                    int used = 0;
                    for (int c = 0; c < width; c++) {
                        used += snprintf(
                            value + used,
                            sizeof value - (size_t)used,
                            "%s%.6g",
                            c ? ", " : "",
                            s->shader_probe[i][c]
                        );
                    }
                    report_row(
                        k,
                        key,
                        e->asset->domain == BEHAVIOR_DOMAIN_SHAPE ? "Distance"
                                                                  : names[i],
                        value
                    );
                }
            }
        }
    }
    kit_panel_end(k);
}

void studio_shader_controls(studio *s, editor *e)
{
    if (!s->shader) {
        s->shader = xcalloc(1, sizeof *s->shader);
        s->shader->zoom = 1;
        s->shader->uv[0] = s->shader->uv[1] = .5f;
    }
    struct studio_shader *v = s->shader;
    if (v->identity != e->asset->identity) {
        drop_picture(s);
        v->editing = false;
        v->identity = e->asset->identity;
        v->dirty = true;
        v->error[0] = 0;
        v->yaw = v->pitch = v->time = 0;
        v->zoom = 1;
        v->uv[0] = v->uv[1] = .5f;
    }
    /* History adopts new owned graph storage. Retire its old native input
     * gestures too, so releasing an undone scrub cannot reapply that value. */
    if (v->document != e->asset) {
        v->document = e->asset;
        /* Live source synchronization can also replace graph storage while
         * this document still owns its pending gesture. Keep that field. */
        if (!v->editing || !e->pending_snapshot) {
            v->controls_revision++;
            v->editing = false;
        }
    }
    v->visible = true;
    if (images_changed(v)) {
        dirty(s);
    }
    refresh_report(s, e);
    bool material = e->asset->domain == BEHAVIOR_DOMAIN_MATERIAL;
    kit *k = s->kit;
    kit_push(k, "shader-editor", NULL, "shader-editor");
    kit_push(k, "heading", NULL, "shader-heading");
    kit_label(k, "name", "shader-title", e->asset->name);
    kit_attr(k, "data-tip-clipped", "");
    kit_label(k, "domain", "dim", material ? "Material" : "Shape");
    kit_pop(k);
    kit_surface_input input = { 0 };
    kit_surface(k, "preview", "shader-preview", "shader-preview", &input);
    char width[16], height[16];
    snprintf(width, sizeof width, "%d", PREVIEW_WIDTH);
    snprintf(height, sizeof height, "%d", PREVIEW_HEIGHT);
    kit_attr(k, "width", width);
    kit_attr(k, "height", height);
    kit_attr(k, "aria-label", material ? "Material preview" : "Shape preview");
    kit_tip(
        k,
        material ? "Drag to orbit; scroll to zoom" : "Scroll to zoom",
        NULL,
        "Preview settings also provides keyboard-editable view controls."
    );
    if (input.down[0] && !input.pressed[0] && material) {
        float dx = input.x - v->pointer_x, dy = input.y - v->pointer_y;
        if (dx || dy) {
            v->yaw = remainderf(v->yaw - dx * .012f, 2 * PI);
            v->pitch = fminf(fmaxf(v->pitch + dy * .012f, -PI / 2), PI / 2);
            dirty(s);
        }
    }
    v->pointer_x = input.x;
    v->pointer_y = input.y;
    if (input.wheel) {
        v->zoom = fminf(fmaxf(v->zoom * expf(input.wheel * .1f), .25f), 4);
        dirty(s);
    }
    const char *status = !s->shader_report_valid ? "Graph needs attention"
                       : v->error[0]             ? "Preview unavailable"
                       : v->dirty || !v->drawn   ? "Updating preview"
                                                 : "Rendered preview";
    kit_push(k, "preview-toolbar", NULL, "shader-toolbar");
    kit_label(k, "status", "shader-status", status);
    kit_flag(k, "data-error", !s->shader_report_valid || v->error[0]);
    if (kit_button(
            k,
            "reset-view",
            &(kit_button_desc){ .label = "Reset view", .variant = KIT_SUBTLE, .size = KIT_SM }
        )) {
        v->yaw = v->pitch = 0;
        v->zoom = 1;
        dirty(s);
    }
    kit_pop(k);
    if (!s->shader_report_valid || v->error[0]) {
        kit_label(
            k,
            "preview-error",
            "notice shader-note",
            !s->shader_report_valid ? s->shader_report_error.message : v->error
        );
        uint64_t id = s->shader_report_error.node;
        if (id &&
            kit_button(k, "show-error", &(kit_button_desc){ .label = "Show node", .variant = KIT_SUBTLE })) {
            for (int g = 0; g < acount(e->asset->graphs); g++) {
                if (graph_node_by_id(e->asset->graphs[g], id)) {
                    studio_reveal_node(s, e->asset->graphs[g]->graph_id, id);
                    break;
                }
            }
        }
    }
    if (s->shader_report_valid) {
        parameter_controls(s, e);
        texture_controls(s, e);
    }
    bool ready = s->shader_report_valid && v->drawn && !v->dirty &&
                 !v->error[0] && !v->readback.id &&
                 !editor_has_pending_edits(e) && v->revision == e->edit_revision;
    if (kit_button(
            k,
            "export-evidence",
            &(kit_button_desc){
                .label = "Export evidence",
                .variant = KIT_OUTLINE,
                .flags = ready ? 0 : KIT_DISABLED,
                .description = "Save this rendered preview, linked source, "
                               "sample results and CLI replay inputs." }
        )) {
        export_evidence(s, e);
    }
    view_controls(s, material);
    if (s->shader_report_valid) {
        diagnostics(s, e);
    }
    kit_pop(k);
}

void studio_shader_frame(studio *s)
{
    struct studio_shader *v = s->shader;
    if (!v) {
        return;
    }
    poll_export(s);
    editor *e = s->scene->graph_editor;
    if (!v->visible || !e || e->asset->identity != v->identity) {
        if (!v->readback.id) {
            if (e && e->asset->identity == v->identity) {
                finish_edit(s, e);
            }
            studio_shader_free(s);
        }
        return;
    }
    refresh_report(s, e);
    if (!s->shader_report_valid || editor_has_pending_edits(e)) {
        if (!s->shader_report_valid) {
            rml_backend_set_image(s->backend, "shader-preview", 0);
            v->drawn = false;
        }
        return;
    }
    unsigned revision = render_shader_revision();
    if (images_changed(v)) {
        v->dirty = true;
    }
    if (revision != v->catalog_revision) {
        render_shader_preview_free(v->renderer);
        v->renderer = NULL;
        v->dirty = true;
    }
    if (!v->dirty) {
        return;
    }
    if (!published_snapshot(s, e)) {
        if (!s->scene->behavior_apply_pending && !s->scene->preview_retry) {
            snprintf(
                v->error,
                sizeof v->error,
                "%s",
                s->scene->preview_message[0] ? s->scene->preview_message : "The edited shader could not be published for preview."
            );
            v->dirty = false;
            v->drawn = false;
            rml_backend_set_image(s->backend, "shader-preview", 0);
            s->ui_schedule.wake = true;
        }
        return;
    }
    v->error[0] = 0;
    if (!v->canvas.id) {
        v->canvas = cf_make_canvas(cf_canvas_defaults(PREVIEW_WIDTH, PREVIEW_HEIGHT));
    }
    shader_error error = { 0 };
    if (!v->renderer) {
        v->renderer = render_shader_preview_find(e->asset->name, PREVIEW_WIDTH, PREVIEW_HEIGHT, &error);
    }
    bool drawn = v->renderer &&
                 render_shader_preview_set_view(v->renderer, v->yaw, v->pitch, v->zoom);
    cf_draw_push();
    if (drawn) {
        drawn = render_shader_preview_draw(v->renderer, v->time, NULL);
        cf_render_to(v->canvas, true);
    }
    cf_draw_pop();
    if (drawn) {
        drawn = remember_images(s);
    }
    if (!drawn && !v->error[0]) {
        snprintf(
            v->error,
            sizeof v->error,
            "%s",
            error.message[0] ? error.message : render_shader_preview_error(v->renderer)
        );
    }
    v->drawn = drawn;
    v->dirty = false;
    v->catalog_revision = revision;
    v->revision = e->edit_revision;
    rml_backend_set_image(
        s->backend,
        "shader-preview",
        drawn ? cf_canvas_get_target(v->canvas).id : 0
    );
    s->ui_schedule.wake = true;
}
