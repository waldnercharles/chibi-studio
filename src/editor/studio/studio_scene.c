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
#include "editor/studio/studio_internal.h"
#include "editor/ui/labels.h"
#include "editor/workspace/model_preview.h"
#include "editor/workspace/viewport.h"
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

/* ---------- Navigation ---------- */

/* The Canvas's page zoom magnifies its frame, as a browser zooms a page; the
   Scene has one zoom, its camera's. The Canvas renders sharper up to
   VIEW_SHARP_MAX, and beyond it its pixels magnify, so it stays bounded. */
#define VIEW_ZOOM_MAX 8.0f
#define VIEW_SHARP_MAX 2.0f

static const float VIEW_ZOOMS[] = { 1, 2, 4, 8 };
#define VIEW_ZOOM_COUNT ((int)(sizeof VIEW_ZOOMS / sizeof *VIEW_ZOOMS))

/* The readout stays plain text while the wheel or a pinch keeps changing the
   zoom, and rolls only for a jump such as Frame. */
#define ZOOM_SETTLE_SECONDS 0.3

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

/* In the Canvas a pinch, or Mod or Ctrl with the wheel, zooms the page
   toward the pointer, as browsers zoom one, and while it magnifies the
   middle button pans it. */
static void canvas_view(studio *s, engine_viewport_host_input *input)
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

/* Whether the Scene's runtime target, the scene or a previewed prototype,
   plays, so the pointer and keys are its game's. */
static bool target_playing(const studio *s)
{
    return app_scene_authoring_runtime_state(s->scene).playing;
}

/* Whether the Scene's camera follows the pointer: while the target is edited
   or paused, and with Alt held while it plays (render/viewport.c). */
static bool camera_free(const studio *s)
{
    return !target_playing(s) || (s->mods & SDL_KMOD_ALT);
}

/* A mouse wheel's notches and a pinch zoom the camera toward the pointer.
   While the game has the camera, the wheel and a trackpad's scroll alike
   scroll its UI (render/viewport.c). */
static void zoom_camera(studio *s, engine_viewport_host_input *input)
{
    if (!camera_free(s)) {
        return;
    }
    float notches = s->pinch / (FEEL_ZOOM_RATE * logf(2));
    input->wheel = s->surface.hovered ? s->scene_wheel + notches : 0;
    if (input->wheel != 0) {
        s->scene_nav.zoomed_at = s->time;
    }
}

/* A trackpad's two-finger scroll pans the camera the distance it would
   scroll a page, as Figma's canvas pans. */
static void scroll_camera(studio *s, float width)
{
    if (!s->surface.hovered || (s->scroll_x == 0 && s->scroll_y == 0) ||
        !camera_free(s)) {
        return;
    }
    engine_viewport_camera camera;
    engine_viewport_get_camera(s->viewport, &camera);
    float step = FEEL_SCROLL_POINTS * camera.span_width / width;
    engine_viewport_view view;
    engine_viewport_get_view(s->viewport, &view);
    view.center_x += s->scroll_x * step;
    view.center_y += s->scroll_y * step;
    engine_viewport_set_view(s->viewport, &view);
}

/* Space held turns a left drag into the middle button's pan, as Figma's and
   the graph editor's Space do; the drag pans until the button lifts. */
static void space_pan(studio *s, engine_viewport_host_input *input)
{
    bool space = s->space_down && !kit_wants_keyboard(s->kit) && camera_free(s);
    s->scene_nav.space_drag |= space && input->pressed[0];
    if (!s->scene_nav.space_drag) {
        return;
    }
    s->scene_nav.space_panned = true;
    s->scene_nav.space_drag = input->down[0] && !input->released[0];
    input->down[2] = input->down[0];
    input->pressed[2] = input->pressed[0];
    input->released[2] = input->released[0];
    input->down[0] = input->pressed[0] = input->released[0] = false;
}

/* The Scene's camera zooms toward the pointer for a mouse wheel and a pinch
   alike, and pans for a trackpad's scroll, the middle button and Space with
   a drag (Figma). */
static void scene_view(studio *s, engine_viewport_host_input *input)
{
    zoom_camera(s, input);
    scroll_camera(s, input->width);
    space_pan(s, input);
}

void studio_scene_view(studio *s, engine_viewport_host_input *input)
{
    if (studio_previewed_ui(s)) {
        canvas_view(s, input);
        return;
    }
    scene_view(s, input);
}

bool studio_scene_space(studio *s, studio_key key)
{
    bool plain = !(key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_CTRL | SDL_KMOD_GUI));
    if (key.key != SDLK_SPACE || !plain || key.repeat || target_playing(s) ||
        studio_previewed_ui(s)) {
        return false;
    }
    s->scene_nav.space_play = true;
    s->scene_nav.space_panned = false;
    return true;
}

void studio_scene_space_release(studio *s)
{
    if (s->space_down || !s->scene_nav.space_play) {
        return;
    }
    s->scene_nav.space_play = false;
    if (!s->scene_nav.space_panned) {
        studio_command_run(s, STUDIO_COMMAND_PLAY);
    }
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
    yyjson_mut_val *show = yyjson_mut_obj_add_obj(doc, scene, "show");
    yyjson_mut_obj_add_strcpy(doc, show, "label", "Show");
    char icon[5];
    studio_glyph(icon, KIT_ICON_EYE);
    yyjson_mut_obj_add_strcpy(doc, show, "icon", icon);
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

/* The Canvas's page zoom shows in the header; the Scene's camera zoom reads
   in its corner (show_readout). */
static bool show_zoom(studio *s, const studio_panel_sync *at, int mode)
{
    char label[16];
    snprintf(label, sizeof label, "%d%%", (int)lroundf(s->view.zoom * 100));
    bool shown = mode == MODE_CANVAS;
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
   increment preset by index; scene.addMissing adds what the tool's grey
   gizmo lacks. */
void studio_scene_action(studio *s, const char *action, const ui_activation *a)
{
    int index = a->argc ? atoi(a->args[0]) : -1;
    scene_tool_hint hint;
    if (!strcmp(action, "frameModel")) {
        scene_model_preview_frame(s->scene);
    } else if (!strcmp(action, "addMissing") && scene_gizmo_hint(s->scene, &hint)) {
        studio_add_component_to_selection(s, hint.missing);
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

/* A marker's icon reaches this far from its object, as Scene.rcss draws it. */
#define MARKER_RADIUS_DP 10.0f
/* The delta label sits this far right of and below the pointer. */
#define DELTA_OFFSET_X_DP 16.0f
#define DELTA_OFFSET_Y_DP 20.0f
/* Room kept for the labels at the Scene's right and bottom edges. */
#define DELTA_ROOM_X_DP 140.0f
#define DELTA_ROOM_Y_DP 24.0f
#define HINT_ROOM_Y_DP 40.0f
/* The hint's box, centered under the gizmo, holds its pill; in a narrower
   Scene it keeps this gap from each edge. */
#define HINT_BOX_DP 320.0f
#define HINT_GAP_DP 8.0f
/* The tool's hint sits below the gizmo's ring (scene_gizmo.c's RING_RADIUS,
   in the view's pixels), with a gap. */
#define HINT_BELOW_PX 76.0f

static const char *const VIEW_TEXTS[] = { "error",
                                          "warning",
                                          "model",
                                          "modelError",
                                          "modelSourceError",
                                          "modelParameterError",
                                          "zoomText",
                                          "delta",
                                          "deltaAt",
                                          "hint",
                                          "hintAction",
                                          "hintAt",
                                          "hintWidth",
                                          NULL };
static const char *const VIEW_FLAGS[] = { "readout",      "zooming",
                                          "session",      "pending",
                                          "modelPreview", "hinted",
                                          "hintAddable",  NULL };
static const char *const CAMERA_TEXTS[] = { "name", "at", NULL };
static const char *const CAMERA_FLAGS[] = { "active", "disabled", "selected", NULL };
static const char *const MARKER_TEXTS[] = { "icon", "at", "name", NULL };
static const char *const MARKER_FLAGS[] = { "active", "selected", "hovered", "locked", NULL };

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
   transform, and active, disabled and selected); markers, the icon over
   each object nothing draws (icon, name, at, and active, selected, hovered
   and locked); zoom, the camera's against its framing, while readout, and
   zoomText while zooming; delta, a gesture's change by the pointer at
   deltaAt; hint, why the tool's gizmo is grey, with hintAction while
   hintAddable, in a box hintWidth wide at hintAt; session (Live); pending,
   while the generated world fills; the notice line's error, else warning. */
void studio_scene_view_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *view = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "scene_view");
    yyjson_mut_obj_add_arr(doc, view, "cameras");
    yyjson_mut_obj_add_arr(doc, view, "markers");
    add_members(doc, view, VIEW_TEXTS, VIEW_FLAGS);
    yyjson_mut_obj_add_real(doc, view, "zoom", 0);
}

/* Gives `items` `count` entries with `texts` and `flags`, adding and
   removing at its end, so those shown keep their elements; true when its
   length changed. */
static bool fit_items(
    yyjson_mut_doc *doc,
    yyjson_mut_val *items,
    int count,
    const char *const *texts,
    const char *const *flags
)
{
    int had = (int)yyjson_mut_arr_size(items);
    for (int i = had; i < count; i++) {
        add_members(doc, yyjson_mut_arr_add_obj(doc, items), texts, flags);
    }
    for (int i = had; i > count; i--) {
        yyjson_mut_arr_remove_last(items);
    }
    return had != count;
}

/* A transform to the point (x, y), fractions of the Scene from its
   top-left, by whole pixels, where layout would have placed it, so text
   moved there stays sharp. */
static void place_at(studio *s, float x, float y, char out[64])
{
    float ratio = studio_pixel_ratio(s);
    snprintf(
        out,
        64,
        "translate(%.0fpx, %.0fpx)",
        roundf(x * s->surface.rect.w * ratio),
        roundf(y * s->surface.rect.h * ratio)
    );
}

/* Each camera frame's name over its top-left corner, moved there by a
   transform, so a pan repaints it without laying out the Scene. */
static bool show_cameras(studio *s, const studio_panel_sync *at, bool shown)
{
    app_scene_camera_label labels[CAMERA_LABELS_MAX];
    int count = shown ? app_scene_authoring_camera_labels(s->scene, labels, CAMERA_LABELS_MAX)
                      : 0;
    yyjson_mut_val *cameras = yyjson_mut_obj_get(at->variable, "cameras");
    bool changed = fit_items(at->doc, cameras, count, CAMERA_TEXTS, CAMERA_FLAGS);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(cameras);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *camera = yyjson_mut_arr_iter_next(&it);
        char place[64];
        place_at(s, labels[i].x, labels[i].y, place);
        changed |= ui_data_put_text(at->doc, camera, "name", labels[i].name);
        changed |= ui_data_put_text(at->doc, camera, "at", place);
        changed |= ui_data_put_bool(camera, "active", labels[i].active);
        changed |= ui_data_put_bool(camera, "disabled", labels[i].disabled);
        changed |= ui_data_put_bool(camera, "selected", labels[i].selected);
    }
    return changed;
}

/* The object the pointer rests on: in the Scene, or by its Outliner row. */
static ecs_entity hovered_object(studio *s)
{
    ecs_entity row = studio_outliner_hovered(s);
    return row.slot ? row : s->scene->hovered;
}

/* Each object nothing draws shows its Outliner icon where it is, as Godot
   and Unity mark lights, cameras and empty objects, so it can be seen and
   picked (pick_scene_entity hits within MARKER_RADIUS_DP). */
static bool show_markers(studio *s, const studio_panel_sync *at, bool shown)
{
    /* Each object nothing draws picks where it is (pick_scene_entity), so
       each shows a marker there. */
    app_scene_marker *markers = NULL;
    int max = shown ? acount(s->scene->actors) : 0;
    asetlen(markers, max);
    int count = app_scene_authoring_markers(s->scene, markers, max);
    yyjson_mut_val *items = yyjson_mut_obj_get(at->variable, "markers");
    bool changed = fit_items(at->doc, items, count, MARKER_TEXTS, MARKER_FLAGS);
    ecs_entity hovered = hovered_object(s);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(items);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = yyjson_mut_arr_iter_next(&it);
        const scene_actor *actor = &s->scene->actors[markers[i].actor];
        char place[64], icon[5];
        place_at(s, markers[i].x, markers[i].y, place);
        studio_glyph(icon, studio_prototype_icon(s, actor->prototype_name));
        bool over = ecs_entity_to_u64(actor->entity) == ecs_entity_to_u64(hovered);
        changed |= ui_data_put_text(at->doc, item, "icon", icon);
        changed |= ui_data_put_text(at->doc, item, "name", actor->name);
        changed |= ui_data_put_text(at->doc, item, "at", place);
        changed |= ui_data_put_bool(item, "active", markers[i].active);
        changed |= ui_data_put_bool(item, "selected", markers[i].selected);
        changed |= ui_data_put_bool(item, "hovered", over);
        changed |= ui_data_put_bool(item, "locked", markers[i].locked);
    }
    afree(markers);
    return changed;
}

/* Room for any float's whole digits, their separators and "%". */
#define PERCENT_DIGITS 48
#define PERCENT_TEXT (PERCENT_DIGITS + PERCENT_DIGITS / 3 + 2)

/* `percent` with thousands separators, as the readout's NumberFlow shows it. */
static void grouped_percent(double percent, char out[PERCENT_TEXT])
{
    char digits[PERCENT_DIGITS];
    int length = snprintf(digits, sizeof digits, "%.0f", fabs(percent));
    assert(length > 0 && length < PERCENT_DIGITS);
    int at = 0;
    for (int i = 0; i < length; i++) {
        bool separate = i > 0 && (length - i) % 3 == 0;
        at += snprintf(out + at, PERCENT_TEXT - (size_t)at, separate ? ",%c" : "%c", digits[i]);
    }
    snprintf(out + at, PERCENT_TEXT - (size_t)at, "%%");
}

/* The camera's zoom against its last framing, so Frame reads 100%, once
   the scene has drawn and framed it. Its NumberFlow rolls for a jump; while
   a wheel or pinch keeps changing it, plain text follows instead. */
static bool show_readout(studio *s, const studio_panel_sync *at, bool shown)
{
    float zoom = shown && s->surface.rect.w > 0 ? app_scene_authoring_zoom(s->scene)
                                                : 0;
    bool readout = zoom > 0;
    bool changed = ui_data_put_bool(at->variable, "readout", readout);
    if (!readout) {
        return changed;
    }
    double percent = round(zoom * 100);
    char text[PERCENT_TEXT];
    grouped_percent(percent, text);
    bool zooming = s->time - s->scene_nav.zoomed_at < ZOOM_SETTLE_SECONDS;
    changed |= ui_data_put_number(at->variable, "zoom", percent);
    changed |= ui_data_put_text(at->doc, at->variable, "zoomText", text);
    return ui_data_put_bool(at->variable, "zooming", zooming) || changed;
}

/* A gesture's change so far beside the pointer (Figma's and Blender's), as
   the status bar also reports it; kept inside the Scene. */
static bool show_delta(studio *s, const studio_panel_sync *at, bool shown)
{
    const scene_gizmo *g = &s->scene->gizmo;
    const char *delta = shown && g->active ? g->delta : "";
    char place[64] = "";
    if (delta[0]) {
        kit_rect r = s->surface.rect;
        float x = fminf(s->surface.x + DELTA_OFFSET_X_DP, r.w - DELTA_ROOM_X_DP);
        float y = fminf(s->surface.y + DELTA_OFFSET_Y_DP, r.h - DELTA_ROOM_Y_DP);
        place_at(s, fmaxf(x, 0) / r.w, fmaxf(y, 0) / r.h, place);
    }
    bool changed = ui_data_put_text(at->doc, at->variable, "delta", delta);
    return ui_data_put_text(at->doc, at->variable, "deltaAt", place) || changed;
}

/* Whether one click can add `desc` to the selection: one placed object that
   lacks it, while the scene is edited. */
static bool addable(studio *s, const ecs_component_desc *desc)
{
    scene_actor *actor = selected_actor(s->scene);
    return actor && actor->identity && scene_selection_count(s->scene) == 1 &&
           studio_editable(s) &&
           !ecs_component_has(s->scene->world, actor->entity, desc->id);
}

/* Why the tool's gizmo is grey, below it, with the one click that fixes it
   when there is one: a disabled control says why. It comes and goes with
   presence, so a hint leaving keeps its text and place while it fades. */
static bool show_hint(studio *s, const studio_panel_sync *at, bool shown)
{
    scene_tool_hint hint;
    bool hinted = shown && !s->scene->gizmo.active &&
                  scene_gizmo_hint(s->scene, &hint);
    bool changed = ui_data_put_bool(at->variable, "hinted", hinted);
    if (!hinted) {
        return changed;
    }
    char text[160], action[160], place[64], width[32], label[128];
    editor_component_label(hint.missing->name, label, sizeof label);
    /* The tool the gizmo has, which takes a tool chosen a frame later. */
    if (s->scene->gizmo.in.tool == SCENE_TOOL_ROTATE) {
        snprintf(text, sizeof text, "No %s to rotate", label);
    } else {
        snprintf(text, sizeof text, "No size to scale");
    }
    snprintf(action, sizeof action, "Add %s", label);
    kit_rect r = s->surface.rect;
    float half = fminf(HINT_BOX_DP, fmaxf(r.w - 2 * HINT_GAP_DP, 0)) / 2;
    float x = fminf(fmaxf(hint.x * r.w, half + HINT_GAP_DP), r.w - half - HINT_GAP_DP);
    float below = HINT_BELOW_PX / s->prefs.ui_scale;
    float y = fminf(hint.y * r.h + below, r.h - HINT_ROOM_Y_DP);
    place_at(s, (x - half) / r.w, fmaxf(y, 0) / r.h, place);
    snprintf(width, sizeof width, "%.0fdp", 2 * half);
    changed |= ui_data_put_text(at->doc, at->variable, "hint", text);
    changed |= ui_data_put_text(at->doc, at->variable, "hintAction", action);
    changed |= ui_data_put_bool(at->variable, "hintAddable", addable(s, hint.missing));
    changed |= ui_data_put_text(at->doc, at->variable, "hintWidth", width);
    return ui_data_put_text(at->doc, at->variable, "hintAt", place) || changed;
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
   the Scene is the game's view, so the camera labels, markers and hints
   leave the game's HUD alone and Live shows instead. */
bool studio_scene_view_sync(studio *s, const studio_panel_sync *at)
{
    bool scene = !s->guest, session = scene && runtime_session(s->scene);
    bool editing = scene && !session;
    bool changed = show_cameras(s, at, editing);
    changed |= show_markers(s, at, editing);
    changed |= show_delta(s, at, scene);
    changed |= show_hint(s, at, editing);
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
        preview_definition *definition = preview_find_definition(s->scene->live, model);
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
    /* The Scene shows its whole frame; only the Canvas magnifies its own. */
    kit_attr(k, "rect", "0 0 1 1");
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
        /* The view's pixels are the area's points at the UI Scale. */
        .marker_radius = MARKER_RADIUS_DP * s->prefs.ui_scale,
    };
    /* A gesture's change shows live and is reported once it is recorded. */
    if (gesture && g->status[0]) {
        snprintf(s->status, sizeof s->status, "%s", g->status);
        s->status_warning = false;
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

const char *studio_scene_cursor(const studio *s)
{
    const app_scene_authoring *scene = s->scene;
    if (s->guest || studio_previewed_ui(s)) {
        return NULL;
    }
    /* The camera pans while playing too, with Alt held (render/viewport.c). */
    if (s->surface.down[2] && camera_free(s)) {
        return "move";
    }
    if (!scene_can_move(scene)) {
        return NULL;
    }
    if (scene->gizmo.active != SCENE_GIZMO_NONE) {
        return scene_gizmo_cursor(scene);
    }
    /* A marquee, or a paused run's object dragged, over the gizmo leaves it be. */
    bool idle = !scene->marquee && !scene->dragging_actor;
    return idle && s->surface.hovered ? scene_gizmo_cursor(scene) : NULL;
}
