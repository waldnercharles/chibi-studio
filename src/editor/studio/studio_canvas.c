#include "assets/ui_document.h"
#include "authoring/ui_loader.h"
/* The Canvas preview: one ui asset alone over an empty world, as Storybook
 * shows a story and Unity's UI Builder its Canvas, on its own frame and game
 * UI host so nothing of the scene shows or hears it. Its events land in a
 * log (Storybook's Actions) instead of reaching behaviors or running
 * commands. With the Studio project open, its ui assets are Studio's own
 * chrome, so the preview binds the data Studio's does (studio_sources.c).
 * Also the game UI errors of whatever the Scene shows, recorded once per
 * change.
 *
 * The Scene area shows it as projects/studio's Canvas, its controls in the
 * area's SceneHeader: the `canvas` source shows the asset and what uses
 * it, the screen size, the backdrop and the Actions log, and the frame
 * stays native in the stage's slot. */
#include "editor/workspace/viewport.h"
#include "editor/studio/studio_internal.h"
#include "platform/cute.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/xalloc.h"
#include "ui/list.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Screen sizes in logical pixels, as UMG's screen size and Unity UI Builder's
 * canvas size preview devices: game UI scales with the canvas height, so a
 * size sets the aspect the document lays out in. */
static const studio_canvas_size_info SIZES[STUDIO_CANVAS_SIZE_COUNT] = {
    [STUDIO_CANVAS_FIT] = { "Fit", 0, 0 },
    [STUDIO_CANVAS_PHONE] = { "Phone", 390, 844 },
    [STUDIO_CANVAS_TABLET] = { "Tablet", 820, 1180 },
    [STUDIO_CANVAS_DESKTOP] = { "Desktop", 1920, 1080 },
};

/* Storybook's light and dark backgrounds. */
#define BACKDROP_LIGHT 0xf4f4f5ffu
#define BACKDROP_DARK 0x1c1c1fffu

studio_canvas_size_info studio_canvas_size_of(studio_canvas_size size)
{
    return SIZES[size];
}

/* ---------- UI errors ---------- */

bool studio_ui_error_asset(const char *error, char *out, size_t size)
{
    static const char PREFIX[] = "ui/", SUFFIX[] = UI_MARKUP_EXTENSION;
    const char *end = strncmp(error, PREFIX, sizeof PREFIX - 1)
                        ? NULL
                        : strstr(error, SUFFIX);
    if (!end) {
        return false;
    }
    const char *name = error + sizeof PREFIX - 1;
    snprintf(out, size, "%.*s", (int)(end - name), name);
    return end > name;
}

void studio_ui_error(studio *s, const char *error)
{
    if (!strcmp(error, s->ui_error)) {
        return;
    }
    snprintf(s->ui_error, sizeof s->ui_error, "%s", error);
    if (error[0]) {
        studio_record(s, KIT_TOAST_ERROR, error);
    }
}

/* ---------- Preview ---------- */

static void draw_backdrop(void *user)
{
    const studio_canvas *c = user;
    engine_viewport_camera camera;
    engine_viewport_get_camera(c->viewport, &camera);
    uint32_t color = c->light ? BACKDROP_LIGHT : BACKDROP_DARK;
    cf_draw_push_color(cf_make_color_rgba(color >> 24, color >> 16, color >> 8, color));
    cf_draw_quad_fill(
        cf_make_aabb_pos_w_h(
            cf_v2(camera.center_x, camera.center_y),
            2 * camera.span_width,
            2 * camera.span_height
        ),
        0
    );
    cf_draw_pop_color();
}

/* Shows `asset`, or closes the preview for NULL, leaving the areas as they
   are. Closing keeps the preview until it would draw next, so the header and
   body emitting it this frame hold it. */
static void preview(studio *s, const char *asset)
{
    if (!asset || !asset[0]) {
        if (s->canvas) {
            s->canvas->asset[0] = '\0';
        }
        return;
    }
    if (!s->canvas) {
        s->canvas = xcalloc(1, sizeof *s->canvas);
        s->canvas->world = ecs_world_new();
        s->canvas->save = game_save_new();
        s->canvas->list = (ui_list_state)UI_LIST_STATE_INIT;
    }
    studio_canvas *c = s->canvas;
    if (strcmp(c->asset, asset)) {
        snprintf(c->asset, sizeof c->asset, "%s", asset);
        asetlen(c->actions, 0);
    }
}

void studio_preview_ui(studio *s, const char *asset)
{
    preview(s, asset);
    if (asset && asset[0]) {
        studio_show_editor(s, STUDIO_EDITOR_SCENE);
    }
}

/* The ui asset a UI document is, in `name`: a .rml file directly in the open
   project's ui folder, as mounts find assets by name; false for a stylesheet
   or another project's asset. */
bool studio_ui_document_asset(const studio *s, const char *path, char *name, size_t size)
{
    char ui[PATH_MAX], folder[PATH_MAX];
    snprintf(ui, sizeof ui, "%s/ui", s->scene->content_root);
    const char *base = content_basename(path);
    size_t length = strlen(base), extension = strlen(UI_MARKUP_EXTENSION);
    if (!realpath(ui, folder) || length <= extension ||
        strcmp(base + length - extension, UI_MARKUP_EXTENSION) ||
        strncmp(path, folder, strlen(folder)) || base != path + strlen(folder) + 1) {
        return false;
    }
    snprintf(name, size, "%.*s", (int)(length - extension), base);
    return true;
}

void studio_canvas_follow(studio *s)
{
    ui_buffer *b = studio_ui_buffer(s);
    char name[sizeof s->followed] = "";
    if ((b && !studio_ui_document_asset(s, b->path, name, sizeof name)) ||
        !strcmp(name, s->followed)) {
        return;
    }
    char before[sizeof s->followed];
    snprintf(before, sizeof before, "%s", s->followed);
    snprintf(s->followed, sizeof s->followed, "%s", name);
    if (name[0]) {
        preview(s, name);
        return;
    }
    /* Another kind of document is active: the Scene shows the scene again,
       unless something else was previewed since. */
    const char *shown = studio_previewed_ui(s);
    if (shown && !strcmp(shown, before)) {
        preview(s, NULL);
    }
}

const char *studio_previewed_ui(const studio *s)
{
    return s->canvas && s->canvas->asset[0] ? s->canvas->asset : NULL;
}

static bool same_action(const studio_canvas_action *a, const studio_canvas_action *b)
{
    return !strcmp(a->event, b->event) && !strcmp(a->element, b->element) &&
           !strcmp(a->value, b->value);
}

/* Logs an activation; a repeat of the last one counts on it. */
static void log_action(studio_canvas *c, const ui_activation *a, double time)
{
    studio_canvas_action action = { .time = time, .count = 1 };
    snprintf(action.event, sizeof action.event, "%s", a->event);
    if (a->id[0]) {
        snprintf(action.element, sizeof action.element, "#%s", a->id);
    }
    if (a->has_value) {
        snprintf(action.value, sizeof action.value, "%g", a->value);
    }
    int count = acount(c->actions);
    if (count && same_action(&c->actions[count - 1], &action)) {
        c->actions[count - 1].count++;
        c->actions[count - 1].time = time;
        return;
    }
    if (count == STUDIO_CANVAS_ACTIONS) {
        memmove(c->actions, c->actions + 1, (size_t)(count - 1) * sizeof *c->actions);
        asetlen(c->actions, count - 1);
    }
    apush(c->actions, action);
}

static engine_viewport *canvas_viewport(studio *s, studio_canvas *c)
{
    engine_viewport *viewport = engine_viewport_new(1, 1);
    assert(viewport);
    engine_viewport_set_render_callback(viewport, draw_backdrop, c);
    yyjson_mut_doc *data = s->own_project ? studio_sources_new() : NULL;
    engine_viewport_adopt_ui(viewport, authoring_ui_new(s->scene->content_root, data));
    return viewport;
}

bool studio_canvas_draw(studio *s, engine_viewport_host_input *input)
{
    if (!studio_previewed_ui(s)) {
        studio_canvas_free(s);
        return false;
    }
    studio_canvas *c = s->canvas;
    if (!c->viewport) {
        c->viewport = canvas_viewport(s, c);
    }
    ui_world world = { .ecs = c->world,
                       .page = app_scene_authoring_page(s->scene),
                       .save = c->save };
    authoring_viewport_set_ui(c->viewport, &world, s->scene->content_root);
    render_ui *ui = engine_viewport_ui(c->viewport);
    if (s->own_project) {
        studio_sources_show(s, &c->sources, render_ui_data(ui));
    }
    /* The theme is the root's class, as a page's .dark tokens expect. */
    render_ui_set_overlay(ui, c->asset, c->light ? "light" : "dark");
    engine_viewport_set_host_input(c->viewport, input);
    engine_viewport_set_time(c->viewport, s->time);
    engine_viewport_draw(c->viewport, &(engine_viewport_controls){ .playing = true });
    ui_activation a;
    while (render_ui_take_overlay_activation(ui, &a)) {
        log_action(c, &a, s->time);
    }
    studio_ui_error(s, render_ui_error(ui));
    rml_backend_set_texture(s->backend, "canvas", engine_viewport_texture(c->viewport));
    return true;
}

void studio_canvas_free(studio *s)
{
    studio_canvas *c = s->canvas;
    if (!c) {
        return;
    }
    rml_backend_set_texture(s->backend, "canvas", 0);
    engine_viewport_free(c->viewport);
    studio_sources_free(&c->sources);
    ecs_world_free(c->world);
    game_save_free(c->save);
    afree(c->actions);
    free(c);
    s->canvas = NULL;
}

/* ---------- The Canvas's UI (`canvas`) ---------- */

/* Where a ui asset is used: GameUi fields and <use ui> from the index, and
   the scene's and the project's root document. A root document's row opens
   the Scene tab of Properties, where it is chosen. */
typedef struct
{
    char label[288];
    uint32_t icon;
    bool page;
    studio_reference at;
} canvas_user;

static canvas_user *canvas_users(studio *s, const char *asset)
{
    app_scene_authoring *scene = s->scene;
    canvas_user *users = NULL;
    char title[128];
    if (scene->ui && !strcmp(scene->ui, asset)) {
        canvas_user user = { .icon = KIT_ICON_SCENE, .page = true };
        snprintf(
            user.label,
            sizeof user.label,
            "%s \xe2\x80\xba ui",
            studio_scene_title(scene, title, sizeof title)
        );
        apush(users, user);
    }
    if (scene->default_ui && !strcmp(scene->default_ui, asset)) {
        apush(
            users,
            ((canvas_user){ .label = "Project \xe2\x80\xba ui",
                            .icon = KIT_ICON_FOLDER,
                            .page = true })
        );
    }
    const reference_index *index = app_scene_authoring_asset_index(scene);
    int at = index ? reference_index_find_name(index, REFERENCE_ASSET_UI, asset) : -1;
    const reference **uses = NULL;
    if (at >= 0) {
        reference_index_find_uses(
            index,
            (reference_symbol){ .kind = REFERENCE_SYMBOL_ASSET, .asset = at },
            &uses
        );
    }
    for (int i = 0; i < acount(uses); i++) {
        canvas_user user = { .at = studio_reference_of(index, uses[i]) };
        user.icon = studio_asset_kind_icon(user.at.kind);
        snprintf(
            user.label,
            sizeof user.label,
            user.at.place[0] ? "%s \xe2\x80\xba %s" : "%s",
            user.at.asset,
            user.at.place
        );
        apush(users, user);
    }
    afree(uses);
    return users;
}

/* A widget's document previews in its place; anything else opens and the
   Scene area returns to the scene. */
static void open_user(studio *s, const canvas_user *user)
{
    if (user->page) {
        s->tab = STUDIO_TAB_SCENE;
        studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
        return;
    }
    if (user->at.kind == REFERENCE_ASSET_UI) {
        studio_preview_ui(s, user->at.asset);
        return;
    }
    studio_preview_ui(s, NULL);
    studio_open_reference(s, &user->at);
}

#define ACTIONS_LIST "canvas-actions-list"
#define FRAME_SLOT "canvas-frame-slot"

typedef enum
{
    ITEM_MENU,   /* a MenuItem's */
    ITEM_ACTION, /* an Actions row */
} item_shape;

static const char *const MENU_TEXTS[] = { "icon", "label", "shortcut", NULL };
static const char *const MENU_FLAGS[] = { "checked", "disabled", "current", NULL };
static const char *const ACTION_TEXTS[] = { "event", "element", "value",
                                            "count", "time",    NULL };
static const char *const CANVAS_TEXTS[] = { "asset", "frame", "usedBy", "error", NULL };
static const char *const CANVAS_FLAGS[] = { "light", "open", NULL };

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
    for (int i = 0; flags && flags[i]; i++) {
        yyjson_mut_obj_add_bool(doc, object, flags[i], false);
    }
}

static yyjson_mut_val *add_item(
    yyjson_mut_doc *doc,
    yyjson_mut_val *canvas,
    const char *name,
    const char *label,
    uint32_t icon
)
{
    yyjson_mut_val *item = yyjson_mut_obj_add_obj(doc, canvas, name);
    add_members(doc, item, MENU_TEXTS, MENU_FLAGS);
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    ui_data_put_text(doc, item, "label", label);
    ui_data_put_text(doc, item, "icon", glyph);
    return item;
}

/* `canvas`, which the Canvas and its header's controls show: asset, the
   previewed one; used, what uses it, as menu items, or unused; preview,
   the submenu, and previews, the ui assets to preview instead; size, the
   screen size's index among sizes; light, the backdrop; open, whether the
   Actions log shows, with total, its count, and rows, its List's window;
   frame and usedBy, the stage's label; error, the game UI's. */
void studio_canvas_panel_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *canvas = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "canvas");
    add_members(doc, canvas, CANVAS_TEXTS, CANVAS_FLAGS);
    yyjson_mut_obj_add_arr(doc, canvas, "used");
    yyjson_mut_val *unused = add_item(doc, canvas, "unused", "Nothing uses it yet", 0);
    ui_data_put_bool(unused, "disabled", true);
    add_item(doc, canvas, "preview", "Preview", studio_asset_kind_icon(REFERENCE_ASSET_UI));
    yyjson_mut_obj_add_arr(doc, canvas, "previews");
    yyjson_mut_obj_add_real(doc, canvas, "size", STUDIO_CANVAS_FIT);
    yyjson_mut_val *sizes = yyjson_mut_obj_add_arr(doc, canvas, "sizes");
    for (int i = 0; i < STUDIO_CANVAS_SIZE_COUNT; i++) {
        yyjson_mut_arr_add_str(doc, sizes, SIZES[i].label);
    }
    yyjson_mut_obj_add_real(doc, canvas, "total", 0);
    yyjson_mut_obj_add_arr(doc, canvas, "rows");
}

/* Gives `items` `length` items of `shape`, adding and removing at its end,
   so items already shown keep their elements; true when its length
   changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *items, int length, item_shape shape)
{
    int had = (int)yyjson_mut_arr_size(items);
    for (int i = had; i < length; i++) {
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, items);
        add_members(
            doc,
            item,
            shape == ITEM_MENU ? MENU_TEXTS : ACTION_TEXTS,
            shape == ITEM_MENU ? MENU_FLAGS : NULL
        );
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(items);
    }
    return had != length;
}

static bool put_item(yyjson_mut_doc *doc, yyjson_mut_val *item, const char *label, uint32_t icon, bool current)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    bool changed = ui_data_put_text(doc, item, "label", label);
    changed |= ui_data_put_text(doc, item, "icon", glyph);
    return ui_data_put_bool(item, "current", current) || changed;
}

/* The asset crumb's menu: what uses the asset, and the ui assets to
   preview instead, the previewed one current. */
static bool show_menu(studio *s, const studio_panel_sync *at, const canvas_user *users)
{
    yyjson_mut_val *used = yyjson_mut_obj_get(at->variable, "used");
    bool changed = fit(at->doc, used, acount(users), ITEM_MENU);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(used);
    for (int i = 0; i < acount(users); i++) {
        changed |= put_item(
            at->doc,
            yyjson_mut_arr_iter_next(&it),
            users[i].label,
            users[i].icon,
            false
        );
    }
    const char **names = studio_asset_names(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_UI);
    yyjson_mut_val *previews = yyjson_mut_obj_get(at->variable, "previews");
    changed |= fit(at->doc, previews, acount(names), ITEM_MENU);
    it = yyjson_mut_arr_iter_with(previews);
    for (int i = 0; i < acount(names); i++) {
        bool current = !strcmp(names[i], s->canvas->asset);
        changed |= put_item(
            at->doc,
            yyjson_mut_arr_iter_next(&it),
            names[i],
            studio_asset_kind_icon(REFERENCE_ASSET_UI),
            current
        );
    }
    afree(names);
    return changed;
}

/* The element with `id` in Studio's documents, or NULL. */
static rml_element *by_id(studio *s, const char *id)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* The stage's size in dp, as it last laid out. */
static kit_rect stage_size(studio *s, rml_element *stage)
{
    float ratio = studio_pixel_ratio(s);
    rml_vec2 size = stage ? rml_element_box_size(stage, RML_BOX_CONTENT)
                          : (rml_vec2){ 0 };
    return (kit_rect){ .w = size.x / ratio, .h = size.y / ratio };
}

/* The size and users of the previewed asset, over the stage's bottom left. */
static bool show_label(studio *s, const studio_panel_sync *at, const canvas_user *users)
{
    studio_canvas_size_info size = SIZES[s->canvas->size];
    char line[288];
    if (size.width) {
        snprintf(line, sizeof line, "%s \xc2\xb7 %d \xc3\x97 %d", size.label, size.width, size.height);
    } else {
        kit_rect stage = stage_size(s, at->preview ? NULL : by_id(s, FRAME_SLOT));
        float ui = s->prefs.ui_scale;
        snprintf(
            line,
            sizeof line,
            "Fit \xc2\xb7 %d \xc3\x97 %d",
            (int)(stage.w * ui),
            (int)(stage.h * ui)
        );
    }
    bool changed = ui_data_put_text(at->doc, at->variable, "frame", line);
    int used = snprintf(line, sizeof line, "%s", acount(users) ? "Used by " : "Not used yet");
    for (int i = 0; i < acount(users) && used < (int)sizeof line; i++) {
        used += snprintf(
            line + used,
            sizeof line - (size_t)used,
            "%s%s",
            i ? ", " : "",
            users[i].label
        );
    }
    return ui_data_put_text(at->doc, at->variable, "usedBy", line) || changed;
}

static bool show_action(yyjson_mut_doc *doc, yyjson_mut_val *row, const studio_canvas_action *a)
{
    char time[16], count[16];
    int seconds = (int)a->time;
    snprintf(time, sizeof time, "%02d:%02d", seconds / 60 % 100, seconds % 60);
    snprintf(count, sizeof count, "\xc3\x97%d", a->count);
    bool changed = ui_data_put_text(doc, row, "event", a->event);
    changed |= ui_data_put_text(doc, row, "element", a->element);
    changed |= ui_data_put_text(doc, row, "value", a->value);
    changed |= ui_data_put_text(doc, row, "count", a->count > 1 ? count : "");
    return ui_data_put_text(doc, row, "time", time) || changed;
}

/* The Actions log's window: its List follows new rows while at its end. */
static bool show_actions(studio *s, const studio_panel_sync *at, bool open)
{
    studio_canvas *c = s->canvas;
    int total = acount(c->actions), first = 0, length = 0;
    rml_element *list = open && !at->preview ? by_id(s, ACTIONS_LIST) : NULL;
    if (list) {
        ui_list window = { .count = total, .reveal = -1, .follow = true };
        length = ui_list_window(list, &window, &c->list, &first);
    }
    yyjson_mut_val *rows = yyjson_mut_obj_get(at->variable, "rows");
    bool changed = ui_data_put_number(at->variable, "total", total);
    changed |= fit(at->doc, rows, length, ITEM_ACTION);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(rows);
    for (int i = first; i < first + length; i++) {
        changed |= show_action(at->doc, yyjson_mut_arr_iter_next(&it), &c->actions[i]);
    }
    return changed;
}

bool studio_canvas_panel_sync(studio *s, const studio_panel_sync *at)
{
    studio_canvas *c = s->canvas;
    if (!studio_previewed_ui(s)) {
        return false;
    }
    bool open = studio_active_layout(s)->canvas_actions_open;
    canvas_user *users = canvas_users(s, c->asset);
    bool changed = ui_data_put_text(at->doc, at->variable, "asset", c->asset);
    changed |= show_menu(s, at, users);
    changed |= show_label(s, at, users);
    afree(users);
    changed |= ui_data_put_number(at->variable, "size", c->size);
    changed |= ui_data_put_bool(at->variable, "light", c->light);
    changed |= ui_data_put_bool(at->variable, "open", open);
    changed |= ui_data_put_text(at->doc, at->variable, "error", s->ui_error);
    return show_actions(s, at, open) || changed;
}

/* The used and previews rows a click names by index, which may have
   changed since they showed: past their end opens nothing. */
static void open_used(studio *s, int index)
{
    canvas_user *users = canvas_users(s, s->canvas->asset);
    if (index >= 0 && index < acount(users)) {
        open_user(s, &users[index]);
    }
    afree(users);
}

static void preview_other(studio *s, int index)
{
    const char **names = studio_asset_names(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_UI);
    if (index >= 0 && index < acount(names) && strcmp(names[index], s->canvas->asset)) {
        studio_preview_ui(s, names[index]);
    }
    afree(names);
}

/* canvas.back returns to the scene; .used and .preview open a row of the
   asset crumb's menu by index; .size chooses a screen size; .backdrop and
   .actions toggle the backdrop and the Actions log; .clear empties it and
   .scroll wakes the source as its List scrolls. */
void studio_canvas_panel_action(studio *s, const char *action, const ui_activation *a)
{
    studio_canvas *c = s->canvas;
    if (!studio_previewed_ui(s)) {
        return;
    }
    int index = a->argc ? atoi(a->args[0]) : -1;
    if (!strcmp(action, "back")) {
        studio_preview_ui(s, NULL);
    } else if (!strcmp(action, "used")) {
        open_used(s, index);
    } else if (!strcmp(action, "preview")) {
        preview_other(s, index);
    } else if (!strcmp(action, "size") && index >= 0 && index < STUDIO_CANVAS_SIZE_COUNT) {
        c->size = (studio_canvas_size)index;
    } else if (!strcmp(action, "backdrop")) {
        c->light = !c->light;
    } else if (!strcmp(action, "actions")) {
        studio_layout *layout = studio_active_layout(s);
        layout->canvas_actions_open = !layout->canvas_actions_open;
    } else if (!strcmp(action, "clear")) {
        asetlen(c->actions, 0);
    }
}

/* A screen size's frame, centered in the stage with room around it; Fit
   fills the stage. */
static void place_frame(kit *k, studio_canvas_size_info size, kit_rect stage)
{
    enum
    {
        MARGIN = 16,
    };
    float w = stage.w, h = stage.h;
    if (size.width) {
        float room_w = fmaxf(stage.w - 2 * MARGIN, 1),
              room_h = fmaxf(stage.h - 2 * MARGIN, 1);
        float aspect = (float)size.width / (float)size.height;
        w = fminf(room_w, room_h * aspect);
        h = w / aspect;
    }
    char value[32];
    snprintf(value, sizeof value, "%.1fdp", (stage.w - w) / 2);
    kit_style(k, "left", value);
    snprintf(value, sizeof value, "%.1fdp", (stage.h - h) / 2);
    kit_style(k, "top", value);
    snprintf(value, sizeof value, "%.1fdp", w);
    kit_style(k, "width", value);
    snprintf(value, sizeof value, "%.1fdp", h);
    kit_style(k, "height", value);
}

bool studio_canvas_body(studio *s, int area)
{
    if (!studio_previewed_ui(s)) {
        return false;
    }
    kit *k = s->kit;
    studio_canvas *c = s->canvas;
    rml_element *host = studio_ui_area(s, area, "Canvas");
    rml_element *stage = host ? rml_element_query_selector(host, "#" FRAME_SLOT) : NULL;
    if (!stage || !kit_slot_begin(k, "frame", host, "#" FRAME_SLOT)) {
        return true;
    }
    c->frame = kit_surface(k, "frame", "surface canvas-frame", "canvas", &s->surface);
    studio_view_crop(s);
    place_frame(k, SIZES[c->size], stage_size(s, stage));
    kit_slot_end(k);
    return true;
}
