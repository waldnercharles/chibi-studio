/* Studio editors for the authored scene: the viewport. The Outliner is
 * studio_outliner.c, and the Canvas, which the Scene area shows while a ui
 * asset previews, studio_canvas.c.
 *
 * The area is projects/studio's Scene and its header's controls
 * SceneHeader. Their sources show what changes as the user works: `scene`
 * the header's View Zoom and grid increment, and `scene_view` the overlays,
 * each camera frame's label (moved by a transform, so a pan lays nothing
 * out), the label naming the scene and selection, the camera's zoom, Live,
 * the generated world's progress and the notice line. The tools, snapping,
 * Show and Frame are commands the header binds through `commands`. The
 * surface stays native in its slot, where the pointer, the gizmos and the
 * camera are C's. */
#include "editor/workspace/viewport.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/model_preview.h"
#include "support/array.h"
#include "ui/input_feel.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool studio_editable(const studio *s)
{
    return scene_can_move(s->scene) && !s->scene->runtime_snapshot_valid;
}

bool studio_live(const studio *s)
{
    return s->scene->runtime_snapshot_valid;
}

bool studio_matches(const char *query, const char *text)
{
    if (!query[0]) {
        return true;
    }
    for (const char *h = text; *h; h++) {
        size_t i = 0;
        while (query[i] && h[i] &&
               tolower((unsigned char)query[i]) == tolower((unsigned char)h[i])) {
            i++;
        }
        if (!query[i]) {
            return true;
        }
    }
    return false;
}

/* ---------- View zoom ---------- */

/* The view zoom magnifies the Scene's rendered frame, game UI included, as
 * Unity's Game view Scale does; the wheel's camera zoom moves through the
 * world instead. The frame renders sharper up to VIEW_SHARP_MAX, and beyond
 * it its pixels magnify, so the canvas stays bounded. */
#define VIEW_ZOOM_MAX 8.0f
#define VIEW_SHARP_MAX 2.0f

static const float VIEW_ZOOMS[] = { 1, 2, 4, 8 };
#define VIEW_ZOOM_COUNT ((int)(sizeof VIEW_ZOOMS / sizeof *VIEW_ZOOMS))

/* Shows the frame at `zoom` around (x, y), kept inside the frame. */
static void place_view(studio *s, float zoom, float x, float y)
{
    float half = 0.5f / zoom;
    s->view.zoom = zoom;
    s->view.x = fminf(fmaxf(x, half), 1 - half);
    s->view.y = fminf(fmaxf(y, half), 1 - half);
}

/* Zooms keeping the frame point at (u, v), fractions of the area, in place. */
static void zoom_view(studio *s, float zoom, float u, float v)
{
    float next = fminf(fmaxf(zoom, 1), VIEW_ZOOM_MAX);
    float shift = 1 / s->view.zoom - 1 / next;
    place_view(s, next, s->view.x + (u - 0.5f) * shift, s->view.y + (v - 0.5f) * shift);
}

static bool zoomed_to(const studio *s, float zoom)
{
    return fabsf(s->view.zoom - zoom) < 0.005f;
}

/* A pinch, or Mod or Ctrl with the wheel, zooms the view toward the pointer,
 * as browsers zoom a page, and while the view magnifies the middle button
 * pans it. */
void studio_scene_view(studio *s, engine_viewport_host_input *input)
{
    float u = s->surface.x / s->surface.rect.w,
          v = s->surface.y / s->surface.rect.h;
    if (s->surface.hovered && (s->zoom_wheel != 0 || s->pinch != 0)) {
        zoom_view(s, s->view.zoom * feel_zoom_scale(s->zoom_wheel) * expf(s->pinch), u, v);
        input->wheel -= s->zoom_wheel;
    }
    float zoom = s->view.zoom;
    s->view.panning = zoom > 1 && input->down[2] &&
                      (s->view.panning || input->pressed[2]);
    if (s->view.panning && !input->pressed[2]) {
        place_view(
            s,
            zoom,
            s->view.x - (u - s->view.u) / zoom,
            s->view.y - (v - s->view.v) / zoom
        );
    }
    s->view.u = u;
    s->view.v = v;
    if (zoom > 1) {
        input->down[2] = input->pressed[2] = input->released[2] = false;
    }
    input->x = (s->view.x + (u - 0.5f) / zoom) * input->width;
    input->y = (s->view.y + (v - 0.5f) / zoom) * input->height;
    input->resolution = fminf(zoom, VIEW_SHARP_MAX);
}

void studio_view_crop(studio *s)
{
    float half = 0.5f / s->view.zoom;
    char crop[64];
    snprintf(crop, sizeof crop, "%g %g %g %g", s->view.x - half, s->view.y - half, 2 * half, 2 * half);
    kit_attr(s->kit, "rect", crop);
}

/* ---------- Grid increment ---------- */

/* The settings table's Grid Increment row, whose presets are its choices. */
static const studio_setting *increment_setting()
{
    for (int i = 0; i < studio_setting_count; i++) {
        if (studio_settings[i].offset == offsetof(studio_prefs, scene_increment)) {
            return &studio_settings[i];
        }
    }
    assert(!"the settings table has Grid Increment");
    return NULL;
}

static float increment_at(int choice)
{
    return strtof(increment_setting()->choices[choice].value, NULL);
}

float studio_grid_increment(const studio *s, int step)
{
    int count = increment_setting()->choice_count;
    float current = s->prefs.scene_increment, next = current;
    for (int i = 0; i < count; i++) {
        float up = increment_at(count - 1 - i), down = increment_at(i);
        next = step > 0 && up > current ? up : next;
        next = step < 0 && down < current ? down : next;
    }
    return next;
}

/* ---------- The header (`scene`) ---------- */

/* What the header shows: the scene's controls, a guest Studio's name, or
   the Canvas's controls (studio_canvas.c). */
enum
{
    MODE_SCENE,
    MODE_GUEST,
    MODE_CANVAS,
};

/* A MenuItem's values; only checked changes. */
static void add_item(yyjson_mut_doc *doc, yyjson_mut_val *items, const char *label)
{
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, items);
    yyjson_mut_obj_add_strcpy(doc, item, "label", label);
    yyjson_mut_obj_add_str(doc, item, "icon", "");
    yyjson_mut_obj_add_str(doc, item, "shortcut", "");
    yyjson_mut_obj_add_bool(doc, item, "checked", false);
    yyjson_mut_obj_add_bool(doc, item, "disabled", false);
}

/* `scene`: mode (MODE_*); zoom, the View Zoom button's label, zoomShown
   and zoomKbd, its tip's chord; zooms, its levels; increment, the grid
   increment's label, and increments, its presets. */
void studio_scene_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *scene = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "scene");
    yyjson_mut_obj_add_real(doc, scene, "mode", MODE_SCENE);
    yyjson_mut_obj_add_strcpy(doc, scene, "zoom", "");
    yyjson_mut_obj_add_bool(doc, scene, "zoomShown", false);
    yyjson_mut_obj_add_strcpy(doc, scene, "zoomKbd", "");
    yyjson_mut_val *zooms = yyjson_mut_obj_add_arr(doc, scene, "zooms");
    for (int i = 0; i < VIEW_ZOOM_COUNT; i++) {
        char label[8];
        snprintf(label, sizeof label, "%d%%", (int)(VIEW_ZOOMS[i] * 100));
        add_item(doc, zooms, label);
    }
    yyjson_mut_obj_add_strcpy(doc, scene, "increment", "");
    yyjson_mut_val *increments = yyjson_mut_obj_add_arr(doc, scene, "increments");
    const studio_setting *setting = increment_setting();
    for (int i = 0; i < setting->choice_count; i++) {
        add_item(doc, increments, setting->choices[i].label);
    }
}

static int header_mode(const studio *s)
{
    if (s->guest) {
        return MODE_GUEST;
    }
    return studio_previewed_ui(s) ? MODE_CANVAS : MODE_SCENE;
}

/* View Zoom shows in the header only while it magnifies, as a browser shows
   its zoom, so one zoom reads at a time; the Canvas always shows it. */
static bool show_zoom(studio *s, const studio_panel_sync *at, int mode)
{
    char label[16];
    snprintf(label, sizeof label, "%d%%", (int)lroundf(s->view.zoom * 100));
    bool shown = mode == MODE_CANVAS || (mode == MODE_SCENE && !zoomed_to(s, 1));
    bool changed = ui_data_put_text(at->doc, at->variable, "zoom", label);
    changed |= ui_data_put_bool(at->variable, "zoomShown", shown);
    changed |= ui_data_put_text(at->doc, at->variable, "zoomKbd", studio_chord(s, "Mod+Scroll"));
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(
        yyjson_mut_obj_get(at->variable, "zooms")
    );
    for (int i = 0; i < VIEW_ZOOM_COUNT; i++) {
        changed |= ui_data_put_bool(
            yyjson_mut_arr_iter_next(&it),
            "checked",
            zoomed_to(s, VIEW_ZOOMS[i])
        );
    }
    return changed;
}

static bool show_increment(studio *s, const studio_panel_sync *at)
{
    char label[16];
    snprintf(label, sizeof label, "%g", s->prefs.scene_increment);
    bool changed = ui_data_put_text(at->doc, at->variable, "increment", label);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(
        yyjson_mut_obj_get(at->variable, "increments")
    );
    for (int i = 0; i < increment_setting()->choice_count; i++) {
        bool on = increment_at(i) == s->prefs.scene_increment;
        changed |= ui_data_put_bool(yyjson_mut_arr_iter_next(&it), "checked", on);
    }
    return changed;
}

bool studio_scene_sync(studio *s, const studio_panel_sync *at)
{
    int mode = header_mode(s);
    bool changed = ui_data_put_number(at->variable, "mode", mode);
    changed |= show_zoom(s, at, mode);
    return show_increment(s, at) || changed;
}

/* scene.zoom and scene.increment choose a View Zoom level and a grid
   increment preset by index. */
void studio_scene_action(studio *s, const char *action, const ui_activation *a)
{
    int index = a->argc ? atoi(a->args[0]) : -1;
    if (!strcmp(action, "frameModel")) {
        scene_model_preview_frame(s->scene);
    } else if (!strcmp(action, "zoom") && index >= 0 && index < VIEW_ZOOM_COUNT) {
        zoom_view(s, VIEW_ZOOMS[index], 0.5f, 0.5f);
    } else if (
        !strcmp(action, "increment") && index >= 0 &&
        index < increment_setting()->choice_count
    ) {
        s->prefs.scene_increment = increment_at(index);
    }
}

void studio_scene_header(studio *s, int area)
{
    studio_ui_area_controls(s, area, "scene", "SceneHeader");
}

/* ---------- The overlays (`scene_view`) ---------- */

#define CAMERA_LABELS_MAX 16

static const char *const VIEW_TEXTS[] = { "context",
                                          "error",
                                          "warning",
                                          "model",
                                          "modelError",
                                          "modelSourceError",
                                          "modelParameterError",
                                          NULL };
static const char *const VIEW_FLAGS[] = { "labeled", "readout",      "session",
                                          "pending", "modelPreview", NULL };
static const char *const CAMERA_TEXTS[] = { "name", "at", NULL };
static const char *const CAMERA_FLAGS[] = { "active", "disabled", "selected", NULL };

static void add_members(
    yyjson_mut_doc *doc,
    yyjson_mut_val *object,
    const char *const *texts,
    const char *const *flags
)
{
    for (int i = 0; texts[i]; i++) {
        yyjson_mut_obj_add_strcpy(doc, object, texts[i], "");
    }
    for (int i = 0; flags[i]; i++) {
        yyjson_mut_obj_add_bool(doc, object, flags[i], false);
    }
}

/* `scene_view`: cameras, each camera frame's label (name, at, its
   transform, and active, disabled and selected); context, the label naming
   the scene and selection, while labeled; zoom, the camera's, while
   readout; session (Live); pending, while the generated world fills; the
   notice line's error, else warning. */
void studio_scene_view_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *view = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "scene_view");
    yyjson_mut_obj_add_arr(doc, view, "cameras");
    add_members(doc, view, VIEW_TEXTS, VIEW_FLAGS);
    yyjson_mut_obj_add_real(doc, view, "zoom", 0);
}

/* Gives `cameras` `count` labels, adding and removing at its end, so the
   labels shown keep their elements; true when its length changed. */
static bool fit_cameras(yyjson_mut_doc *doc, yyjson_mut_val *cameras, int count)
{
    int had = (int)yyjson_mut_arr_size(cameras);
    for (int i = had; i < count; i++) {
        add_members(doc, yyjson_mut_arr_add_obj(doc, cameras), CAMERA_TEXTS, CAMERA_FLAGS);
    }
    for (int i = had; i > count; i--) {
        yyjson_mut_arr_remove_last(cameras);
    }
    return had != count;
}

/* The camera frames in the part of the frame the view zoom shows, their
   corners as fractions of it; how many. */
static int visible_cameras(studio *s, app_scene_camera_label *labels)
{
    int count = app_scene_authoring_camera_labels(s->scene, labels, CAMERA_LABELS_MAX);
    float half = 0.5f / s->view.zoom;
    int shown = 0;
    for (int i = 0; i < count; i++) {
        app_scene_camera_label label = labels[i];
        label.x = (label.x - (s->view.x - half)) / (2 * half);
        label.y = (label.y - (s->view.y - half)) / (2 * half);
        if (label.x >= 0 && label.x < 1 && label.y >= 0 && label.y < 1) {
            labels[shown++] = label;
        }
    }
    return shown;
}

/* Each camera frame's name over its top-left corner, moved there by a
   transform, so a pan repaints it without laying out the Scene. It moves
   by whole pixels, where layout would have placed it, so its text stays
   sharp. */
static bool show_cameras(studio *s, const studio_panel_sync *at, bool shown)
{
    app_scene_camera_label labels[CAMERA_LABELS_MAX];
    int count = shown ? visible_cameras(s, labels) : 0;
    yyjson_mut_val *cameras = yyjson_mut_obj_get(at->variable, "cameras");
    bool changed = fit_cameras(at->doc, cameras, count);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(cameras);
    float ratio = studio_pixel_ratio(s);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *camera = yyjson_mut_arr_iter_next(&it);
        char place[64];
        snprintf(
            place,
            sizeof place,
            "translate(%.0fpx, %.0fpx)",
            roundf(labels[i].x * s->surface.rect.w * ratio),
            roundf(labels[i].y * s->surface.rect.h * ratio)
        );
        changed |= ui_data_put_text(at->doc, camera, "name", labels[i].name);
        changed |= ui_data_put_text(at->doc, camera, "at", place);
        changed |= ui_data_put_bool(camera, "active", labels[i].active);
        changed |= ui_data_put_bool(camera, "disabled", labels[i].disabled);
        changed |= ui_data_put_bool(camera, "selected", labels[i].selected);
    }
    return changed;
}

/* The camera's zoom, only once the surface is laid out and the scene has
   drawn: its first draw restores or fits the camera after this frame's
   UI, so an earlier value would be the reset camera's, rolling to the real
   one. The camera widens the framed span to the area's aspect, so its span,
   not the framed width, sets the pixels per world unit. */
static bool show_readout(studio *s, const studio_panel_sync *at, bool shown)
{
    bool readout = shown && s->surface.rect.w > 0 && s->scene->camera_target >= 0;
    bool changed = ui_data_put_bool(at->variable, "readout", readout);
    if (!readout) {
        return changed;
    }
    engine_viewport_camera camera;
    engine_viewport_get_camera(s->viewport, &camera);
    double zoom = roundf(s->surface.rect.w / fmaxf(camera.span_width, 1) * 100);
    return ui_data_put_number(at->variable, "zoom", zoom) || changed;
}

/* The renderer's error first, then the generated world's, then the game
   UI's (render_ui_error); else the renderer's warning. */
static bool show_notice(studio *s, const studio_panel_sync *at, bool shown)
{
    app_scene_authoring *scene = s->scene;
    const char *error = app_scene_renderer_error(scene->renderer);
    const char *world = app_scene_authoring_world_error(scene);
    error = error && error[0] ? error : world[0] ? world : s->ui_error;
    const char *warning = app_scene_renderer_warning(scene->renderer);
    bool changed = ui_data_put_text(at->doc, at->variable, "error", shown ? error : "");
    return ui_data_put_text(at->doc, at->variable, "warning", shown && warning ? warning : "") ||
           changed;
}

/* A guest Studio fills the area with none of them. While a session runs
   the Scene is the game's view, so the camera labels and the label leave
   the game's HUD alone and Live shows instead. */
bool studio_scene_view_sync(studio *s, const studio_panel_sync *at)
{
    bool scene = !s->guest, session = scene && runtime_session(s->scene);
    bool editing = scene && !session;
    /* The scene, then the active object, as the Canvas's crumbs read. */
    char title[128], context[256];
    scene_actor *actor = selected_actor(s->scene);
    studio_scene_title(s->scene, title, sizeof title);
    snprintf(
        context,
        sizeof context,
        "%s%s%s",
        title,
        actor ? " \xe2\x80\xba " : "",
        actor ? actor->name : ""
    );
    bool changed = show_cameras(s, at, editing);
    changed |= ui_data_put_bool(at->variable, "labeled", editing);
    changed |= ui_data_put_text(at->doc, at->variable, "context", context);
    changed |= show_readout(s, at, scene);
    changed |= ui_data_put_bool(at->variable, "session", session);
    changed |= ui_data_put_bool(
        at->variable,
        "pending",
        editing && app_scene_authoring_world_pending(s->scene)
    );
    const char *model = scene_model_preview_name(s->scene);
    bool model_shown = scene && model != NULL;
    changed |= ui_data_put_bool(at->variable, "modelPreview", model_shown);
    changed |= ui_data_put_text(at->doc, at->variable, "model", model_shown ? model : "");
    const char *parameter_error = model_shown ? scene_model_preview_error(s->scene)
                                              : "";
    changed |= ui_data_put_text(
        at->doc,
        at->variable,
        "modelParameterError",
        parameter_error ? parameter_error : ""
    );
    const char *model_error = "", *model_source_error = "";
    if (model_shown) {
        game_preview_definition *definition = game_preview_find_definition(s->scene->live, model);
        if (definition && definition->diagnostics.error_count) {
            model_error = "Showing last valid model";
            for (int i = 0; i < acount(definition->diagnostics.items); i++) {
                if (definition->diagnostics.items[i].severity == CMPL_ERROR) {
                    model_source_error = definition->diagnostics.items[i].message;
                    break;
                }
            }
        }
    }
    changed |= ui_data_put_text(at->doc, at->variable, "modelError", model_error);
    changed |= ui_data_put_text(at->doc, at->variable, "modelSourceError", model_source_error);
    return show_notice(s, at, scene) || changed;
}

/* ---------- The area ---------- */

/* The scene's surface, native in Scene's slot, or a guest Studio's, which
   fills the area as the Studio project's game, whatever document is
   active; or, while a ui asset previews, the Canvas. */
void studio_scene_body(studio *s, int area)
{
    if (!s->guest && studio_canvas_body(s, area)) {
        return;
    }
    kit *k = s->kit;
    rml_element *host = studio_ui_area(s, area, "Scene");
    if (!host || !kit_slot_begin(k, "surface", host, "#scene-surface")) {
        return;
    }
    if (s->guest) {
        kit_surface(k, "guest", "surface", "guest", &s->surface);
        kit_slot_end(k);
        return;
    }
    kit_surface(k, "surface", "surface", "viewport", &s->surface);
    studio_view_crop(s);
    studio_model_reference_overlay(s);
    /* A prototype dropped here is placed under the pointer by the viewport;
       an asset an object's field names lands on the object under it. */
    kit_drop_target(k, "prototype", s->dropped_prototype, sizeof s->dropped_prototype);
    studio_drop_scene(s);
    kit_slot_end(k);
}

void studio_scene_tools(studio *s)
{
    scene_gizmo *g = &s->scene->gizmo;
    bool gesture = g->active != SCENE_GIZMO_NONE;
    g->in = (scene_tool_input){
        .tool = s->prefs.tool,
        .grid = s->prefs.scene_grid,
        .snap = s->prefs.scene_snap,
        .increment = s->prefs.scene_increment,
        .shift = (s->mods & SDL_KMOD_SHIFT) != 0,
        .extend = (s->mods & SDL_KMOD_SHIFT) || studio_primary(s, s->mods),
        .ctrl = (s->mods & SDL_KMOD_CTRL) != 0,
        .alt = (s->mods & SDL_KMOD_ALT) != 0,
        .axis_x = g->modal && studio_key_pressed(s, SDLK_X),
        .axis_y = g->modal && studio_key_pressed(s, SDLK_Y),
        .confirm = g->modal && (studio_key_pressed(s, SDLK_RETURN) ||
                                studio_key_pressed(s, SDLK_KP_ENTER)),
        .cancel = gesture &&
                  (studio_key_pressed(s, SDLK_ESCAPE) || s->surface.pressed[1]),
    };
    /* A gesture's change shows live and is reported once it is recorded. */
    if (gesture && g->status[0]) {
        snprintf(s->status, sizeof s->status, "%s", g->status);
        s->status_time = s->time;
        s->scene_status = true;
    }
    if (g->report[0]) {
        studio_report(s, g->report);
        g->report[0] = '\0';
        s->scene_status = false;
    }
    if (!gesture && s->scene_status) {
        s->status[0] = '\0';
        s->scene_status = false;
    }
}
