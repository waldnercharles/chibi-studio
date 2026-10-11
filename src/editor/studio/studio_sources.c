#include "authoring/ui_loader.h"
/* The data Studio's authored panels bind beyond their own world's `save`
 * (render/ui.h), one variable per panel source (studio_panel_sources, at the
 * end): `world`, the saved keys of the world the Scene shows, and `theme`,
 * the custom properties of the stylesheets the open scene's root document
 * imports; `collaboration` and `new_project`, their dialogs'; `settings`, the
 * Settings page's preferences; `shortcuts`, Keyboard Shortcuts';
 * `inspector`, the UI Inspector's (see its section below); `editor`, what
 * the status bar, the top bar and the area headers show; and `commands`,
 * each command's state (the last sections). A panel's edits apply back: a
 * saved key to the save, a theme value to its file, which restyles
 * everything the page styles, and a setting to the preferences. A preview of
 * Studio's own ui assets shows the same data and applies nothing. */
#include "editor/workspace/viewport.h"
#include "assets/asset_source.h"
#include "assets/project.h"
#include "editor/collab/collab.h"
#include "editor/documents/component_properties.h"
#include "editor/studio/studio_internal.h"
#include "authoring/preview_runtime.h"
#include "runtime/declared_components.h"
#include "authoring/prototype_preview.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/rcss.h"
#include "support/text_file.h"
#include "support/xalloc.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yyjson_mut_val *root_object(yyjson_mut_doc *doc, const char *name)
{
    return yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), name);
}

/* ---------- World ---------- */

static void add_world(yyjson_mut_doc *doc)
{
    yyjson_mut_val *world = root_object(doc, "world");
    yyjson_mut_obj_add_str(doc, world, "title", "");
    yyjson_mut_obj_add_arr(doc, world, "saved");
}

/* The save of the world the Scene shows, and its title. */
static save *scene_save(studio *s, char *title, size_t size)
{
    app_scene_authoring *scene = s->scene;
    scene_prototype_asset *previewed = previewed_prototype(scene);
    if (scene->active_runtime_target == APP_RUNTIME_PREVIEW && previewed) {
        snprintf(title, size, "Preview \xc2\xb7 %s", previewed->prototype->name);
        return prototype_preview_live(scene->prototype_preview)->runtime->save;
    }
    char name[128];
    snprintf(title, size, "Scene \xc2\xb7 %s", studio_scene_title(scene, name, sizeof name));
    return scene->live->runtime->save;
}

/* Saves the values rows hold that differ from `save`. */
static void save_rows(yyjson_mut_val *rows, save *save)
{
    for (size_t i = 0; i < yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, i);
        yyjson_mut_val *value = yyjson_mut_obj_get(row, "value");
        if (yyjson_mut_is_str(value)) {
            save_set(save, yyjson_mut_get_str(yyjson_mut_obj_get(row, "key")), yyjson_mut_get_str(value));
        }
    }
}

/* Whether the rows name `save`'s keys, in order. */
static bool rows_match(yyjson_mut_val *rows, const save *save)
{
    if ((int)yyjson_mut_arr_size(rows) != save_count(save)) {
        return false;
    }
    for (int i = 0; i < save_count(save); i++) {
        yyjson_mut_val *key = yyjson_mut_obj_get(yyjson_mut_arr_get(rows, (size_t)i), "key");
        if (strcmp(yyjson_mut_get_str(key), save_key(save, i)) != 0) {
            return false;
        }
    }
    return true;
}

static void show_rows(yyjson_mut_doc *doc, yyjson_mut_val *rows, const save *save)
{
    if (!rows_match(rows, save)) {
        yyjson_mut_arr_clear(rows);
        for (int i = 0; i < save_count(save); i++) {
            yyjson_mut_val *row = yyjson_mut_arr_add_obj(doc, rows);
            yyjson_mut_obj_add_strcpy(doc, row, "key", save_key(save, i));
            yyjson_mut_obj_add_null(doc, row, "value");
        }
    }
    for (int i = 0; i < save_count(save); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)i);
        ui_data_set_text(doc, yyjson_mut_obj_get(row, "value"), save_get(save, save_key(save, i)));
    }
}

static bool sync_world(studio *s, const studio_panel_sync *at)
{
    studio_sources *src = at->sources;
    yyjson_mut_val *rows = yyjson_mut_obj_get(at->variable, "saved");
    yyjson_mut_val *shown_title = yyjson_mut_obj_get(at->variable, "title");
    char title[192];
    save *save = scene_save(s, title, sizeof title);
    if (at->wrote && save == src->save) {
        save_rows(rows, save);
    }
    bool retitled = strcmp(yyjson_mut_get_str(shown_title), title) != 0;
    if (save == src->save && save_revision(save) == src->save_seen && !retitled) {
        return false;
    }
    src->save = save;
    src->save_seen = save_revision(save);
    ui_data_set_text(at->doc, shown_title, title);
    show_rows(at->doc, rows, save);
    return true;
}

/* ---------- Theme ---------- */

static void free_tokens(studio_sources *src)
{
    for (int i = 0; i < acount(src->tokens); i++) {
        free(src->tokens[i].value);
    }
    afree(src->tokens);
    src->tokens = NULL;
}

static bool starts(const char *text, const char *prefix)
{
    return !strncmp(text, prefix, strlen(prefix));
}

/* Whether a value names a color: a literal, a color function, or a token
 * holding one. */
static bool color_value(const studio_sources *src, const char *value)
{
    static const char *const COLORS[] = { "#", "rgb", "hsl", "color-mix(", "transparent" };
    for (size_t i = 0; i < sizeof COLORS / sizeof *COLORS; i++) {
        if (starts(value, COLORS[i])) {
            return true;
        }
    }
    if (!starts(value, "var(--")) {
        return false;
    }
    size_t length = strcspn(value + 4, ",)");
    for (int i = acount(src->tokens) - 1; i >= 0; i--) {
        const studio_token *t = &src->tokens[i];
        if (strlen(t->name) == length && !strncmp(t->name, value + 4, length)) {
            return t->color;
        }
    }
    return false;
}

/* The file's name without its folder and extension. */
static void file_title(char *out, size_t size, const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(out, size, "%.*s", (int)strcspn(name, "."), name);
}

static void add_theme(yyjson_mut_doc *doc)
{
    yyjson_mut_obj_add_arr(doc, root_object(doc, "theme"), "sections");
}

/* Lists the theme's files' custom properties, a section per file and rule. */
static void show_theme(const studio_panel_sync *at)
{
    studio_sources *src = at->sources;
    yyjson_mut_doc *doc = at->doc;
    yyjson_mut_val *sections = yyjson_mut_obj_get(at->variable, "sections");
    yyjson_mut_arr_clear(sections);
    free_tokens(src);
    yyjson_mut_val *rows = NULL;
    for (int f = 0; f < acount(authoring_ui_page_files(&src->page)); f++) {
        const char *path = authoring_ui_page_files(&src->page)[f].path;
        char *text = file_watch_exists(&authoring_ui_page_files(&src->page)[f]) ? text_file_read(path)
                                                              : NULL;
        rcss_variable *vars = NULL;
        if (text) {
            rcss_variables(text, &vars);
        }
        for (int i = 0; i < acount(vars); i++) {
            const rcss_variable *v = &vars[i];
            if (!i || strcmp(v->selector, vars[i - 1].selector) != 0) {
                char title[64];
                file_title(title, sizeof title, path);
                yyjson_mut_val *section = yyjson_mut_arr_add_obj(doc, sections);
                yyjson_mut_obj_add_strcpy(doc, section, "file", title);
                yyjson_mut_obj_add_strcpy(doc, section, "selector", v->selector);
                rows = yyjson_mut_obj_add_arr(doc, section, "rows");
            }
            studio_token token = { .file = f, .start = v->start, .end = v->end };
            token.value = xmalloc((size_t)(v->end - v->start) + 1);
            snprintf(token.value, (size_t)(v->end - v->start) + 1, "%s", text + v->start);
            snprintf(token.name, sizeof token.name, "%s", v->name);
            token.color = color_value(src, token.value);
            apush(src->tokens, token);
            yyjson_mut_val *row = yyjson_mut_arr_add_obj(doc, rows);
            yyjson_mut_obj_add_strcpy(doc, row, "name", token.name);
            yyjson_mut_obj_add_strcpy(doc, row, "value", token.value);
            yyjson_mut_obj_add_bool(doc, row, "color", token.color);
        }
        afree(vars);
        free(text);
    }
}

/* The value each token's row holds, in token order; NULL past the rows. */
static const char *row_value(yyjson_mut_val *theme, int token)
{
    yyjson_mut_val *sections = yyjson_mut_obj_get(theme, "sections");
    for (size_t i = 0; i < yyjson_mut_arr_size(sections); i++) {
        yyjson_mut_val *rows = yyjson_mut_obj_get(yyjson_mut_arr_get(sections, i), "rows");
        size_t count = yyjson_mut_arr_size(rows);
        if ((size_t)token < count) {
            yyjson_mut_val *value = yyjson_mut_obj_get(yyjson_mut_arr_get(rows, (size_t)token), "value");
            return yyjson_mut_is_str(value) ? yyjson_mut_get_str(value) : NULL;
        }
        token -= (int)count;
    }
    return NULL;
}

static bool theme_edited(const studio_panel_sync *at)
{
    const studio_sources *src = at->sources;
    for (int i = 0; i < acount(src->tokens); i++) {
        const char *value = row_value(at->variable, i);
        if (value && strcmp(value, src->tokens[i].value) != 0) {
            return true;
        }
    }
    return false;
}

/* Writes file `file` with each edited token's new value in place of its
 * old one; a value the file no longer holds is left alone. */
static void save_file(studio *s, const studio_panel_sync *sync, int file)
{
    const studio_sources *src = sync->sources;
    const char *path = authoring_ui_page_files(&src->page)[file].path;
    char *text = text_file_read(path);
    if (!text) {
        return;
    }
    char *out = NULL;
    int at = 0;
    for (int i = 0; i < acount(src->tokens); i++) {
        const studio_token *t = &src->tokens[i];
        const char *value = row_value(sync->variable, i);
        size_t old = strlen(t->value);
        if (t->file != file || !value || !strcmp(value, t->value) ||
            (size_t)(t->end - t->start) != old ||
            strncmp(text + t->start, t->value, old) != 0) {
            continue;
        }
        int start = acount(out);
        asetlen(out, start + (t->start - at) + (int)strlen(value));
        memcpy(out + start, text + at, (size_t)(t->start - at));
        memcpy(out + start + (t->start - at), value, strlen(value));
        at = t->end;
    }
    int start = acount(out);
    asetlen(out, start + (int)strlen(text + at) + 1);
    memcpy(out + start, text + at, strlen(text + at) + 1);
    if (write_file_atomic(path, out) != 0) {
        char message[1100];
        snprintf(message, sizeof message, "Could not save %s", path);
        kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = "Theme", .description = message });
        studio_record(s, KIT_TOAST_ERROR, message);
    }
    afree(out);
    free(text);
}

static void save_theme(studio *s, const studio_panel_sync *at)
{
    const studio_sources *src = at->sources;
    for (int f = 0; f < acount(authoring_ui_page_files(&src->page)); f++) {
        bool edited = false;
        for (int i = 0; i < acount(src->tokens) && !edited; i++) {
            const char *value = row_value(at->variable, i);
            edited = src->tokens[i].file == f && value &&
                     strcmp(value, src->tokens[i].value) != 0;
        }
        if (edited) {
            save_file(s, at, f);
        }
    }
    show_theme(at);
}

static bool sync_theme(studio *s, const studio_panel_sync *at)
{
    studio_sources *src = at->sources;
    const char *content = s->scene->content_root;
    if (strcmp(src->content_root, content) != 0) {
        ui_page_free(&src->page);
        snprintf(src->content_root, sizeof src->content_root, "%s", content);
    }
    bool saved = at->wrote && theme_edited(at);
    if (saved) {
        save_theme(s, at);
    }
    const char *page = app_scene_authoring_page(s->scene);
    bool read = authoring_ui_page_sync(&src->page, content, page ? page : "", s->time);
    if (read) {
        show_theme(at);
    }
    return saved || read;
}

/* ---------- Settings ---------- */

/* `settings`, the Settings page: the Editor or Project tab (`scope`), the
 * category, the search query, the categories with their search counts, and
 * one row per preference, command and project setting. Rows are built once
 * and shown or hidden, so the page never rebuilds as the query changes. A
 * row's value is text, and its `control` ("bool", "choice", "range", "text",
 * or "" for a shortcut) chooses the one control the page builds for it with
 * data-when, which removes the others where data-if would keep them bound. A
 * slider moves `position`, from 0 to 1 in steps of `tick`: a range input
 * clamps its value when its bounds arrive, so its bounds stay fixed. The
 * Record button sends emit('settings.record', key). */

/* The Project tab's rows, after the preferences and commands. A field the
   manifest may leave out (`optional`) offers None and Reset; the others must
   name something, so an empty value is dropped. */
static const struct
{
    const char *key;   /* the row's */
    const char *field; /* in project.kdl */
    const char *label, *description;
    bool optional;
} PROJECT_ROWS[STUDIO_PROJECT_FIELD_COUNT] = {
    [STUDIO_PROJECT_NAME] = { "project.name", "name", "Name", "The project's name, as Open Project and the command palette list it." },
    [STUDIO_PROJECT_START_SCENE] = { "project.startScene", "start_scene", "Start Scene", "The scene the project opens with, in Studio and when the game runs." },
    [STUDIO_PROJECT_UI] = { "project.ui", "ui", "Root Document", "The ui document every scene's UI shows on unless the scene names its own. Its stylesheets style the whole page.", true },
};
enum
{
    SETTINGS_KEYS_MAX = 8, /* keys in one shortcut */
};

static yyjson_mut_val *settings_page(yyjson_mut_doc *doc)
{
    return yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "settings");
}

static const char *text_of(yyjson_mut_val *object, const char *name)
{
    const char *text = yyjson_mut_get_str(yyjson_mut_obj_get(object, name));
    return text ? text : "";
}

/* The text a binding left in a row's member: a switch writes a boolean and
 * a slider a number. */
static const char *bound_text(yyjson_mut_val *row, const char *name, char *buffer, size_t size)
{
    yyjson_mut_val *value = yyjson_mut_obj_get(row, name);
    if (yyjson_mut_is_bool(value)) {
        return yyjson_mut_get_bool(value) ? "true" : "false";
    }
    if (yyjson_mut_is_num(value)) {
        snprintf(buffer, size, "%g", yyjson_mut_get_num(value));
        return buffer;
    }
    return yyjson_mut_is_str(value) ? yyjson_mut_get_str(value) : "";
}

/* An icon's codepoint as UTF-8; Symbols' glyphs are past U+0800. */
void studio_glyph(char out[5], uint32_t c)
{
    assert(c >= 0x800);
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        out[3] = '\0';
        return;
    }
    out[0] = (char)(0xf0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    out[4] = '\0';
}

void studio_settings_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *page = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "settings");
    yyjson_mut_obj_add_str(doc, page, "scope", "editor");
    yyjson_mut_obj_add_str(doc, page, "category", studio_settings_categories[0].id);
    yyjson_mut_obj_add_str(doc, page, "query", "");
    yyjson_mut_obj_add_str(doc, page, "title", studio_settings_categories[0].label);
    yyjson_mut_obj_add_str(doc, page, "project", "");
    yyjson_mut_obj_add_real(doc, page, "matches", 0);
    yyjson_mut_obj_add_real(doc, page, "conflicts", 0);
    yyjson_mut_val *categories = yyjson_mut_obj_add_arr(doc, page, "categories");
    for (int c = 0; c < STUDIO_SETTINGS_CATEGORY_COUNT; c++) {
        const studio_settings_category_info *info = &studio_settings_categories[c];
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, categories);
        char icon[5];
        studio_glyph(icon, info->icon);
        yyjson_mut_obj_add_str(doc, item, "id", info->id);
        yyjson_mut_obj_add_str(doc, item, "label", info->label);
        yyjson_mut_obj_add_strcpy(doc, item, "icon", icon);
        yyjson_mut_obj_add_bool(doc, item, "hidden", info->project);
        yyjson_mut_obj_add_bool(doc, item, "dim", false);
        yyjson_mut_obj_add_bool(doc, item, "dot", false);
        yyjson_mut_obj_add_str(doc, item, "badge", "");
    }
    yyjson_mut_obj_add_arr(doc, page, "rows");
}

/* A row with every member it may show, empty. */
static yyjson_mut_val *add_row(
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    studio_settings_category category,
    const char *type
)
{
    static const char *const TEXTS[] = { "key",     "label",    "description",
                                         "value",   "default",  "position",
                                         "context", "conflict", "bound",
                                         "aliases", "control" };
    static const char *const FLAGS[] = { "shown", "head", "modified", "recording", "indexed" };
    static const char *const LISTS[] = { "choices", "keys" };
    static const char *const NUMBERS[] = { "min", "max", "step", "tick" };
    yyjson_mut_val *row = yyjson_mut_arr_add_obj(doc, rows);
    yyjson_mut_obj_add_str(
        doc,
        row,
        "category",
        studio_settings_categories[category].id
    );
    yyjson_mut_obj_add_str(
        doc,
        row,
        "group",
        studio_settings_categories[category].label
    );
    yyjson_mut_obj_add_str(doc, row, "type", type);
    for (size_t i = 0; i < sizeof TEXTS / sizeof *TEXTS; i++) {
        yyjson_mut_obj_add_str(doc, row, TEXTS[i], "");
    }
    for (size_t i = 0; i < sizeof FLAGS / sizeof *FLAGS; i++) {
        yyjson_mut_obj_add_bool(doc, row, FLAGS[i], false);
    }
    for (size_t i = 0; i < sizeof LISTS / sizeof *LISTS; i++) {
        yyjson_mut_obj_add_arr(doc, row, LISTS[i]);
    }
    for (size_t i = 0; i < sizeof NUMBERS / sizeof *NUMBERS; i++) {
        yyjson_mut_obj_add_real(doc, row, NUMBERS[i], 0);
    }
    return row;
}

/* Sets the control the page shows the row with. */
static void gate(yyjson_mut_val *row, const char *control)
{
    yyjson_mut_set_str(yyjson_mut_obj_get(row, "control"), control);
}

/* A number without presets, which a slider sets. */
static bool ranged(const studio_setting *s)
{
    assert(
        s->type != STUDIO_SETTING_NUMBER || s->choice_count ||
        (s->max > s->min && s->step > 0)
    );
    return s->type == STUDIO_SETTING_NUMBER && !s->choice_count;
}

static void add_setting(
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    const studio_setting *s,
    const studio_prefs *defaults
)
{
    static const char *const TYPES[] = { [STUDIO_SETTING_BOOL] = "bool",
                                         [STUDIO_SETTING_CHOICE] = "choice",
                                         [STUDIO_SETTING_NUMBER] = "number" };
    yyjson_mut_val *row = add_row(doc, rows, s->category, TYPES[s->type]);
    yyjson_mut_set_str(yyjson_mut_obj_get(row, "key"), s->key);
    yyjson_mut_set_str(yyjson_mut_obj_get(row, "label"), s->label);
    yyjson_mut_set_str(yyjson_mut_obj_get(row, "description"), s->description);
    char text[32];
    studio_setting_text(defaults, s, text, sizeof text);
    ui_data_put_text(doc, row, "default", text);
    yyjson_mut_val *choices = yyjson_mut_obj_get(row, "choices");
    for (int i = 0; i < s->choice_count; i++) {
        yyjson_mut_val *choice = yyjson_mut_arr_add_obj(doc, choices);
        yyjson_mut_obj_add_str(doc, choice, "value", s->choices[i].value);
        yyjson_mut_obj_add_str(doc, choice, "label", s->choices[i].label);
    }
    gate(
        row,
        s->type == STUDIO_SETTING_BOOL ? "bool"
        : s->choice_count              ? "choice"
                                       : "range"
    );
    ui_data_put_number(row, "min", s->min);
    ui_data_put_number(row, "max", s->max);
    ui_data_put_number(row, "step", s->step);
    if (ranged(s)) {
        ui_data_put_number(row, "tick", s->step / (s->max - s->min));
    }
}

static void build_rows(const studio_settings_view *view, yyjson_mut_doc *doc, yyjson_mut_val *rows)
{
    yyjson_mut_arr_clear(rows);
    studio_prefs defaults = studio_prefs_default();
    for (int i = 0; i < studio_setting_count; i++) {
        add_setting(doc, rows, &studio_settings[i], &defaults);
    }
    for (int c = 0; c < view->command_count; c++) {
        yyjson_mut_val *row = add_row(doc, rows, STUDIO_SETTINGS_KEYBOARD, "shortcut");
        char text[160];
        snprintf(text, sizeof text, "command.%s", view->commands[c].id);
        ui_data_put_text(doc, row, "key", text);
        snprintf(text, sizeof text, "studio.%s", view->commands[c].id);
        ui_data_put_text(doc, row, "description", text);
        int command = studio_command_named(view->commands[c].id);
        if (command >= 0) {
            ui_data_put_text(doc, row, "default", studio_command_default_chord(command));
            ui_data_put_text(doc, row, "context", studio_command_context(command));
        }
        ui_data_put_bool(row, "indexed", view->commands[c].indexed);
    }
    for (int f = 0; f < STUDIO_PROJECT_FIELD_COUNT; f++) {
        bool text = f == STUDIO_PROJECT_NAME;
        yyjson_mut_val *row = add_row(doc, rows, STUDIO_SETTINGS_PROJECT, text ? "text" : "asset");
        yyjson_mut_set_str(yyjson_mut_obj_get(row, "key"), PROJECT_ROWS[f].key);
        yyjson_mut_set_str(yyjson_mut_obj_get(row, "label"), PROJECT_ROWS[f].label);
        yyjson_mut_set_str(
            yyjson_mut_obj_get(row, "description"),
            PROJECT_ROWS[f].description
        );
        gate(row, text ? "text" : "choice");
    }
}

static yyjson_mut_val *project_row(const studio_settings_view *view, yyjson_mut_val *rows, int field)
{
    return yyjson_mut_arr_get(rows, (size_t)(studio_setting_count + view->command_count + field));
}

static bool is_choice(const studio_setting_choice *choices, const char *value)
{
    for (int i = 0; i < acount(choices); i++) {
        if (!strcmp(choices[i].value, value)) {
            return true;
        }
    }
    return false;
}

/* Whether a project field may take `value`: any name, one of the assets the
   field chooses from, or none where the field may be left out. */
static bool project_value(const studio_settings_view *view, int field, const char *value)
{
    if (!value[0]) {
        return PROJECT_ROWS[field].optional;
    }
    return strlen(value) < sizeof view->fields[field] &&
           (field == STUDIO_PROJECT_NAME || is_choice(view->choices[field], value));
}

/* A slider's position as the value it stands for, on a step. */
static void apply_position(studio_prefs *prefs, const studio_setting *s, yyjson_mut_val *row)
{
    char buffer[32], text[32], *end;
    const char *position = bound_text(row, "position", buffer, sizeof buffer);
    double fraction = strtod(position, &end);
    if (end == position || *end) {
        return;
    }
    double steps = round(fraction * (s->max - s->min) / s->step);
    snprintf(text, sizeof text, "%g", s->min + steps * s->step);
    studio_setting_parse(prefs, s, text);
}

/* Applies each row whose value a binding changed, or whose slider moved. An
 * invalid value is dropped, and the row shows the setting again. */
static void apply_rows(studio_settings_view *view, yyjson_mut_val *rows)
{
    for (int i = 0; i < studio_setting_count; i++) {
        char buffer[32], text[32];
        const studio_setting *s = &studio_settings[i];
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)i);
        const char *value = bound_text(row, "value", buffer, sizeof buffer);
        studio_setting_text(view->prefs, s, text, sizeof text);
        if (strcmp(value, text) != 0) {
            studio_setting_parse(view->prefs, s, value);
        } else if (ranged(s)) {
            apply_position(view->prefs, s, row);
        }
    }
    for (int c = 0; c < view->command_count; c++) {
        int command = studio_command_named(view->commands[c].id);
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)(studio_setting_count + c));
        if (command < 0 || view->commands[c].indexed) {
            continue;
        }
        const char *value = text_of(row, "value");
        if (!strcmp(value, text_of(row, "bound"))) {
            continue;
        }
        if (!strcmp(value, studio_command_default_chord(command))) {
            view->prefs->shortcuts[command][0] = '\0';
            if (view->recording == command + 1) {
                view->recording = 0;
            }
        } else if (studio_chord_valid(value)) {
            snprintf(view->prefs->shortcuts[command], STUDIO_CHORD_SIZE, "%s", value);
        }
    }
    for (int f = 0; f < STUDIO_PROJECT_FIELD_COUNT; f++) {
        yyjson_mut_val *row = project_row(view, rows, f);
        const char *value = text_of(row, "value");
        if (strcmp(value, view->fields[f]) != 0 && project_value(view, f, value)) {
            snprintf(view->fields[f], sizeof view->fields[f], "%s", value);
        }
    }
}

/* Shows a shortcut's keys: "⇧⌘Z" as ⇧, ⌘ and Z, "Ctrl+Shift+Z" split at
 * each '+'. The only characters beyond ASCII are macOS's modifier glyphs. */
static bool show_keys(yyjson_mut_doc *doc, yyjson_mut_val *keys, const char *shortcut)
{
    char parts[SETTINGS_KEYS_MAX][24];
    int count = 0;
    const char *at = shortcut;
    for (; (unsigned char)*at >= 0x80 && count < SETTINGS_KEYS_MAX; at += 3) {
        snprintf(parts[count++], sizeof parts[0], "%.3s", at);
    }
    while (*at == ' ') {
        at++;
    }
    while (*at && count < SETTINGS_KEYS_MAX) {
        size_t length = at[0] == '+' ? 1 : strcspn(at, "+");
        snprintf(parts[count++], sizeof parts[0], "%.*s", (int)length, at);
        at += length + (at[length] == '+');
    }
    bool same = (int)yyjson_mut_arr_size(keys) == count;
    for (int i = 0; same && i < count; i++) {
        same = !strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(keys, (size_t)i)), parts[i]);
    }
    if (same) {
        return false;
    }
    yyjson_mut_arr_clear(keys);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_strcpy(doc, keys, parts[i]);
    }
    return true;
}

/* An asset field's choices: None where the field may be left out, the
 * project's assets of its kind, and the current one if the index has not
 * listed it, since a select shows its first option for a value it lacks. */
static bool show_project_choices(
    const studio_settings_view *view,
    int field,
    yyjson_mut_doc *doc,
    yyjson_mut_val *choices
)
{
    const char *value = view->fields[field];
    studio_setting_choice *shown = NULL;
    if (PROJECT_ROWS[field].optional) {
        apush(shown, ((studio_setting_choice){ "", "None" }));
    }
    for (int i = 0; i < acount(view->choices[field]); i++) {
        apush(shown, view->choices[field][i]);
    }
    if (value[0] && !is_choice(view->choices[field], value)) {
        apush(shown, ((studio_setting_choice){ value, value }));
    }
    bool same = (int)yyjson_mut_arr_size(choices) == acount(shown);
    for (int i = 0; same && i < acount(shown); i++) {
        yyjson_mut_val *choice = yyjson_mut_arr_get(choices, (size_t)i);
        same = !strcmp(text_of(choice, "value"), shown[i].value) &&
               !strcmp(text_of(choice, "label"), shown[i].label);
    }
    if (!same) {
        yyjson_mut_arr_clear(choices);
        for (int i = 0; i < acount(shown); i++) {
            yyjson_mut_val *choice = yyjson_mut_arr_add_obj(doc, choices);
            yyjson_mut_obj_add_strcpy(doc, choice, "value", shown[i].value);
            yyjson_mut_obj_add_strcpy(doc, choice, "label", shown[i].label);
        }
    }
    afree(shown);
    return !same;
}

/* Reparse shortcuts only when their inputs change. A rebuilt model must
 * receive its keys even if its allocator reuses the previous addresses. */
static bool refresh_shortcuts(
    const studio_settings_view *view,
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    bool built
)
{
    studio_settings_cache *cache = view->cache;
    if (!cache) {
        return true;
    }
    assert(view->command_count <= STUDIO_COMMAND_COUNT);
    bool refresh = built || !cache->valid || cache->doc != doc ||
                   cache->rows != rows ||
                   cache->command_count != view->command_count ||
                   cache->mac != view->mac ||
                   memcmp(cache->shortcuts, view->prefs->shortcuts, sizeof cache->shortcuts);
    for (int c = 0; !refresh && c < view->command_count; c++) {
        refresh = cache->commands[c] < 0 ||
                  strcmp(view->commands[c].id, studio_command_id(cache->commands[c]));
    }
    if (!refresh) {
        return false;
    }
    for (int c = 0; c < view->command_count; c++) {
        int command = studio_command_named(view->commands[c].id);
        cache->commands[c] = command;
        cache->conflicts[c] = command >= 0
                                ? studio_command_conflict(
                                      view->prefs,
                                      command,
                                      studio_command_chord(view->prefs, command),
                                      view->mac
                                  )
                                : -1;
    }
    memcpy(cache->shortcuts, view->prefs->shortcuts, sizeof cache->shortcuts);
    cache->doc = doc;
    cache->rows = rows;
    cache->command_count = view->command_count;
    cache->mac = view->mac;
    cache->valid = true;
    return true;
}

/* "Also" and the chords after the first that run the command, which its
 * default's aliases give while it is not overridden. */
static void alias_text(const studio_settings_view *view, int command, char *out, size_t size)
{
    const char *chords[5];
    int count = command >= 0
                  ? studio_command_bindings(view->prefs, command, chords, 5)
                  : 0;
    size_t used = 0;
    out[0] = '\0';
    for (int i = 1; i < count && used < size; i++) {
        char shown[STUDIO_CHORD_SIZE];
        studio_chord_format(shown, sizeof shown, chords[i], view->mac);
        used += (size_t)snprintf(out + used, size - used, "%s%s", i == 1 ? "Also " : ", ", shown);
    }
}

/* Shows each row's value, whether it differs from its default, and a
 * command's label and keys. */
static bool show_values(
    const studio_settings_view *view,
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    bool shortcuts_changed
)
{
    bool changed = false;
    for (int i = 0; i < studio_setting_count; i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)i);
        const studio_setting *s = &studio_settings[i];
        char text[32];
        studio_setting_text(view->prefs, s, text, sizeof text);
        changed |= ui_data_put_text(doc, row, "value", text);
        changed |= ui_data_put_bool(row, "modified", strcmp(text, text_of(row, "default")) != 0);
        if (ranged(s)) {
            float value = studio_setting_get(view->prefs, s);
            snprintf(text, sizeof text, "%g", (value - s->min) / (s->max - s->min));
            changed |= ui_data_put_text(doc, row, "position", text);
        }
    }
    for (int c = 0; c < view->command_count; c++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)(studio_setting_count + c));
        int command = view->cache ? view->cache->commands[c]
                                  : studio_command_named(view->commands[c].id);
        const char *chord = command >= 0 ? studio_command_chord(view->prefs, command)
                                         : "";
        changed |= ui_data_put_text(doc, row, "label", view->commands[c].label);
        changed |= ui_data_put_text(doc, row, "value", chord);
        changed |= ui_data_put_text(doc, row, "bound", chord);
        if (shortcuts_changed) {
            char shortcut[STUDIO_CHORD_SIZE], aliases[192];
            studio_chord_format(shortcut, sizeof shortcut, chord, view->mac);
            changed |= show_keys(doc, yyjson_mut_obj_get(row, "keys"), shortcut);
            alias_text(view, command, aliases, sizeof aliases);
            changed |= ui_data_put_text(doc, row, "aliases", aliases);
        }
        changed |= ui_data_put_bool(
            row,
            "modified",
            command >= 0 && view->prefs->shortcuts[command][0]
        );
        changed |= ui_data_put_bool(row, "recording", command >= 0 && view->recording == command + 1);
        int conflict = view->cache ? view->cache->conflicts[c]
                     : command >= 0
                         ? studio_command_conflict(view->prefs, command, chord, view->mac)
                         : -1;
        const char *label = conflict >= 0 ? studio_command_id(conflict) : "";
        for (int other = 0; conflict >= 0 && other < view->command_count; other++) {
            if (!strcmp(view->commands[other].id, label)) {
                label = view->commands[other].label;
                break;
            }
        }
        char message[192] = "";
        if (conflict >= 0) {
            snprintf(message, sizeof message, "Also assigned to %s", label);
        }
        changed |= ui_data_put_text(doc, row, "conflict", message);
    }
    for (int f = 0; f < STUDIO_PROJECT_FIELD_COUNT; f++) {
        yyjson_mut_val *row = project_row(view, rows, f);
        if (f != STUDIO_PROJECT_NAME) {
            changed |= show_project_choices(view, f, doc, yyjson_mut_obj_get(row, "choices"));
        }
        changed |= ui_data_put_text(doc, row, "value", view->fields[f]);
        changed |= ui_data_put_bool(row, "modified", PROJECT_ROWS[f].optional && view->fields[f][0]);
    }
    return changed;
}

static studio_settings_category row_category(const studio_settings_view *view, int index)
{
    if (index < studio_setting_count) {
        return studio_settings[index].category;
    }
    return index < studio_setting_count + view->command_count
             ? STUDIO_SETTINGS_KEYBOARD
             : STUDIO_SETTINGS_PROJECT;
}

static bool row_matches(const studio_settings_view *view, int index, const char *query)
{
    if (index < studio_setting_count) {
        return studio_setting_matches(&studio_settings[index], query);
    }
    char text[512];
    int command = index - studio_setting_count;
    if (command < view->command_count) {
        const studio_command_info *c = &view->commands[command];
        bool conflict = view->cache && view->cache->conflicts[command] >= 0;
        snprintf(
            text,
            sizeof text,
            "%s %s %s Keyboard shortcut%s",
            c->label,
            c->keywords ? c->keywords : "",
            c->id,
            conflict ? " conflict" : ""
        );
    } else {
        int f = command - view->command_count;
        snprintf(
            text,
            sizeof text,
            "%s %s %s General project",
            PROJECT_ROWS[f].label,
            PROJECT_ROWS[f].description,
            PROJECT_ROWS[f].field
        );
    }
    return studio_settings_match(query, text);
}

/* The category named `id` under the Project tab or the Editor tab, else the
 * tab's first. */
static int category_in(const char *id, bool project)
{
    int first = -1;
    for (int c = STUDIO_SETTINGS_CATEGORY_COUNT - 1; c >= 0; c--) {
        const studio_settings_category_info *info = &studio_settings_categories[c];
        if (info->project == project && !strcmp(info->id, id)) {
            return c;
        }
        first = info->project == project ? c : first;
    }
    assert(first >= 0);
    return first;
}

/* The tab chosen, and a category under it: switching tabs shows its first. */
static int show_scope(yyjson_mut_doc *doc, yyjson_mut_val *page, bool *changed)
{
    bool project = !strcmp(text_of(page, "scope"), "project");
    int category = category_in(text_of(page, "category"), project);
    *changed |= ui_data_put_text(doc, page, "scope", project ? "project" : "editor");
    *changed |= ui_data_put_text(
        doc,
        page,
        "category",
        studio_settings_categories[category].id
    );
    *changed |= ui_data_put_text(
        doc,
        page,
        "title",
        studio_settings_categories[category].label
    );
    return category;
}

/* Shows the rows of the category, or while searching those of every
 * category under the tab that match, headed by their category; then each
 * category's matches and whether it holds a changed setting. */
static bool show_visible(const studio_settings_view *view, yyjson_mut_doc *doc, yyjson_mut_val *page)
{
    bool changed = false;
    int category = show_scope(doc, page, &changed);
    bool project = studio_settings_categories[category].project;
    const char *query = text_of(page, "query");
    bool searching = query[strspn(query, " ")] != '\0';
    yyjson_mut_val *rows = yyjson_mut_obj_get(page, "rows");
    int matches[STUDIO_SETTINGS_CATEGORY_COUNT] = { 0 }, shown = 0, last = -1,
        conflicts = 0;
    bool modified[STUDIO_SETTINGS_CATEGORY_COUNT] = { false };
    for (int i = 0; i < (int)yyjson_mut_arr_size(rows); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)i);
        int c = row_category(view, i);
        conflicts += text_of(row, "conflict")[0] != '\0';
        bool show = studio_settings_categories[c].project == project &&
                    (searching ? row_matches(view, i, query) : c == category);
        matches[c] += show;
        modified[c] |= yyjson_mut_get_bool(yyjson_mut_obj_get(row, "modified"));
        changed |= ui_data_put_bool(row, "shown", show);
        changed |= ui_data_put_bool(row, "head", show && searching && c != last);
        last = show ? c : last;
        shown += show;
    }
    yyjson_mut_val *categories = yyjson_mut_obj_get(page, "categories");
    for (int c = 0; c < STUDIO_SETTINGS_CATEGORY_COUNT; c++) {
        yyjson_mut_val *item = yyjson_mut_arr_get(categories, (size_t)c);
        char badge[16] = "";
        if (searching && matches[c]) {
            snprintf(badge, sizeof badge, "%d", matches[c]);
        }
        changed |= ui_data_put_bool(
            item,
            "hidden",
            studio_settings_categories[c].project != project
        );
        changed |= ui_data_put_bool(item, "dim", searching && !matches[c]);
        changed |= ui_data_put_bool(item, "dot", modified[c]);
        changed |= ui_data_put_text(doc, item, "badge", badge);
    }
    changed |= ui_data_put_number(page, "matches", shown);
    changed |= ui_data_put_number(page, "conflicts", conflicts);
    return changed;
}

bool studio_settings_sync(studio_settings_view *view, yyjson_mut_doc *doc, bool wrote)
{
    yyjson_mut_val *page = settings_page(doc);
    yyjson_mut_val *rows = yyjson_mut_obj_get(page, "rows");
    bool built = (int)yyjson_mut_arr_size(rows) !=
                 studio_setting_count + view->command_count + STUDIO_PROJECT_FIELD_COUNT;
    if (built) {
        build_rows(view, doc, rows);
    } else if (wrote) {
        apply_rows(view, rows);
    }
    yyjson_mut_val *recording = NULL;
    for (int c = 0; view->recording && c < view->command_count; c++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)(studio_setting_count + c));
        if (studio_command_named(view->commands[c].id) == view->recording - 1 &&
            yyjson_mut_get_bool(yyjson_mut_obj_get(row, "shown"))) {
            recording = row;
        }
    }
    bool shortcuts_changed = refresh_shortcuts(view, doc, rows, built);
    bool changed = show_values(view, doc, rows, shortcuts_changed);
    changed |= ui_data_put_text(doc, page, "project", view->project);
    changed |= show_visible(view, doc, page);
    if (recording && !yyjson_mut_get_bool(yyjson_mut_obj_get(recording, "shown"))) {
        view->recording = 0;
        changed |= ui_data_put_bool(recording, "recording", false);
    }
    return changed || built;
}

static int by_label(const void *a, const void *b)
{
    return strcmp(
        ((const studio_setting_choice *)a)->label,
        ((const studio_setting_choice *)b)->label
    );
}

/* The open project's assets of `kind` by name, sorted: ui assets by their
   name, scenes by their path under content, as project.kdl names them. */
static studio_setting_choice *asset_choices(const studio *s, reference_asset_kind kind)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    size_t root = strlen(s->scene->content_root);
    studio_setting_choice *choices = NULL;
    for (int i = 0; index && i < reference_index_asset_count(index); i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (asset->kind != kind || asset->unresolved || !asset->name) {
            continue;
        }
        if (kind == REFERENCE_ASSET_UI) {
            apush(choices, ((studio_setting_choice){ asset->name, asset->name }));
        } else if (asset->path && !strncmp(asset->path, s->scene->content_root, root) && asset->path[root] == '/') {
            apush(
                choices,
                ((studio_setting_choice){ asset->path + root + 1, asset->name })
            );
        }
    }
    if (acount(choices)) {
        qsort(choices, (size_t)acount(choices), sizeof *choices, by_label);
    }
    return choices;
}

/* The open project's name, from its manifest, else its folder's. */
static const char *project_name(const studio *s)
{
    if (s->project.name) {
        return s->project.name;
    }
    const char *slash = strrchr(s->project_root, '/');
    return slash ? slash + 1 : s->project_root;
}

static void settings_error(studio *s, const char *message)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = "Settings", .description = message });
    studio_record(s, KIT_TOAST_ERROR, message);
}

static void set_string(char **field, const char *value)
{
    free(*field);
    *field = value[0] ? xstrdup(value) : NULL;
}

/* What reads a field takes its new value: the Open Project menu the name, a
   relaunch the start scene, and the open scenes the root document. */
static void apply_project_field(studio *s, studio_project_field field, const char *value)
{
    switch (field) {
        case STUDIO_PROJECT_NAME:
            set_string(&s->project.name, value);
            for (int i = 0; i < acount(s->projects); i++) {
                if (studio_project_current(s, i)) {
                    set_string(&s->projects[i].name, value);
                }
            }
            return;
        case STUDIO_PROJECT_START_SCENE:
            set_string(&s->project.start_scene, value);
            return;
        case STUDIO_PROJECT_UI:
            app_scene_authoring_set_default_ui(s->scene, value);
            return;
        case STUDIO_PROJECT_FIELD_COUNT: break;
    }
    assert(!"a project field");
}

/* Writes a field of the project's project.kdl in its place, adding it last
 * or, when empty, leaving it out, then applies it. */
static void set_project_field(studio *s, studio_project_field field, const char *value)
{
    char path[1024], error[256] = "cannot be read";
    snprintf(path, sizeof path, "%s/" CHIBI_PROJECT_MANIFEST, s->project_root);
    char *text = text_file_read(path);
    av_doc *doc = text ? av_doc_read(text, error, sizeof error) : NULL;
    av_value *object = av_doc_root(doc);
    if (!av_is_obj(object)) {
        char message[1400];
        snprintf(message, sizeof message, "%s %s", path, error);
        settings_error(s, message);
        av_doc_free(doc);
        free(text);
        return;
    }
    const char *key = PROJECT_ROWS[field].field;
    if (!value[0]) {
        av_obj_remove_key(object, key);
    } else {
        av_obj_set(doc, object, key, av_str(doc, value));
    }
    char *edited = asset_source_reconcile(text, object, error, sizeof error);
    av_doc_free(doc);
    free(text);
    if (!edited || write_file_atomic(path, edited) != 0) {
        char message[1400];
        snprintf(message, sizeof message, "Could not save %s: %s", path, edited ? "write failed" : error);
        settings_error(s, message);
        free(edited);
        return;
    }
    free(edited);
    apply_project_field(s, field, value);
}

static bool sync_settings(studio *s, const studio_panel_sync *at)
{
    studio_command_info commands[STUDIO_COMMAND_COUNT];
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        commands[c] = studio_command_describe(s, (studio_command)c);
    }
    studio_settings_view view = { .prefs = &s->prefs,
                                  .cache = &at->sources->settings_cache,
                                  .commands = commands,
                                  .command_count = STUDIO_COMMAND_COUNT,
                                  .recording = s->shortcut_recording,
                                  .mac = s->mac,
                                  .project = project_name(s) };
    snprintf(view.fields[STUDIO_PROJECT_NAME], sizeof view.fields[0], "%s", project_name(s));
    snprintf(
        view.fields[STUDIO_PROJECT_START_SCENE],
        sizeof view.fields[0],
        "%s",
        s->project.start_scene ? s->project.start_scene : ""
    );
    snprintf(
        view.fields[STUDIO_PROJECT_UI],
        sizeof view.fields[0],
        "%s",
        s->scene->default_ui ? s->scene->default_ui : ""
    );
    view.choices[STUDIO_PROJECT_START_SCENE] = asset_choices(s, REFERENCE_ASSET_SCENE);
    view.choices[STUDIO_PROJECT_UI] = asset_choices(s, REFERENCE_ASSET_UI);
    char before[STUDIO_PROJECT_FIELD_COUNT][256];
    memcpy(before, view.fields, sizeof before);
    bool changed = studio_settings_sync(&view, at->doc, at->wrote);
    s->shortcut_recording = view.recording;
    for (int f = 0; f < STUDIO_PROJECT_FIELD_COUNT; f++) {
        if (strcmp(view.fields[f], before[f]) != 0) {
            set_project_field(s, (studio_project_field)f, view.fields[f]);
        }
        afree(view.choices[f]);
    }
    return changed;
}

/* emit('settings.record', key) records a shortcut for the row's command,
 * "command.<id>", unless an index chooses its target. */
static void settings_action(studio *s, const char *action, const ui_activation *a)
{
    const char *key = a->args[0];
    int command = !strcmp(action, "record") && a->argc && !strncmp(key, "command.", 8)
                    ? studio_command_named(key + 8)
                    : -1;
    if (command >= 0 && !studio_command_describe(s, (studio_command)command).indexed) {
        s->shortcut_recording = command + 1;
    }
}

static bool sync_shortcuts(studio *s, const studio_panel_sync *at)
{
    return studio_keymap_show(at->doc, &s->prefs, s->mac, &at->sources->keymap_cache);
}

/* ---------- Collaboration ---------- */

static void add_collaboration(yyjson_mut_doc *doc)
{
    static const char *const FLAGS[] = { "available", "connected",      "host",
                                         "error",     "token_expanded", "busy",
                                         "has_peers" };
    static const char *const TEXTS[] = { "status", "token", "invite" };
    yyjson_mut_val *collaboration = root_object(doc, "collaboration");
    for (size_t i = 0; i < sizeof FLAGS / sizeof *FLAGS; i++) {
        yyjson_mut_obj_add_bool(doc, collaboration, FLAGS[i], false);
    }
    for (size_t i = 0; i < sizeof TEXTS / sizeof *TEXTS; i++) {
        yyjson_mut_obj_add_str(doc, collaboration, TEXTS[i], "");
    }
    yyjson_mut_obj_add_arr(doc, collaboration, "peers");
}

static bool sync_collaboration(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_doc *doc = at->doc;
    yyjson_mut_val *collaboration = at->variable;
    bool changed = false;
    changed |= ui_data_put_bool(
        collaboration,
        "token_expanded",
        s->collaboration.admin_token_expanded
    );
    changed |= ui_data_put_bool(collaboration, "busy", studio_collab_busy(s));
    changed |= ui_data_put_bool(collaboration, "available", studio_collab_available());
    changed |= ui_data_put_bool(collaboration, "connected", collab_active());
    changed |= ui_data_put_bool(collaboration, "host", collab_active() && collab_is_host());
    changed |= ui_data_put_bool(collaboration, "error", studio_collab_status_error());
    changed |= ui_data_put_text(doc, collaboration, "status", studio_collab_display_status());
    yyjson_mut_val *peers = yyjson_mut_obj_get(collaboration, "peers");
    int count = acount(s->collaboration.peers);
    changed |= ui_data_put_bool(collaboration, "has_peers", count > 0);
    if ((int)yyjson_mut_arr_size(peers) != count) {
        yyjson_mut_arr_clear(peers);
        for (int i = 0; i < count; i++) {
            yyjson_mut_val *peer = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, peer, "name", "");
            yyjson_mut_obj_add_str(doc, peer, "color", "");
            yyjson_mut_obj_add_bool(doc, peer, "following", false);
            yyjson_mut_arr_append(peers, peer);
        }
        changed = true;
    }
    for (int i = 0; i < count; i++) {
        studio_collab_peer *peer = &s->collaboration.peers[i];
        yyjson_mut_val *row = yyjson_mut_arr_get(peers, (size_t)i);
        char color[16];
        snprintf(color, sizeof color, "#%08x", peer->color ? peer->color : 0x6ea8feffu);
        changed |= ui_data_put_text(doc, row, "name", peer->name);
        changed |= ui_data_put_text(doc, row, "color", color);
        changed |= ui_data_put_bool(
            row,
            "following",
            s->collaboration.following &&
                !strcmp(s->collaboration.follow_client, peer->client)
        );
    }
    return changed;
}

/* ---------- New Project ---------- */

/* `new_project`, the New Project dialog: the name the panel edits, and the
 * error, waiting and save confirmation project operations own. */
static void add_new_project(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "name",    "error",    "error_markup",
                                         "confirm", "location", "folder" };
    yyjson_mut_val *page = root_object(doc, "new_project");
    for (size_t i = 0; i < sizeof TEXTS / sizeof *TEXTS; i++) {
        yyjson_mut_obj_add_str(doc, page, TEXTS[i], "");
    }
    yyjson_mut_obj_add_bool(doc, page, "waiting", false);
    yyjson_mut_obj_add_bool(doc, page, "confirm_save", false);
}

/* Markup showing `text` as text, so names and paths never parse as tags. */
static void escape_markup(char *out, size_t size, const char *text)
{
    size_t length = 0;
    for (const char *p = text; *p && length + 6 < size; p++) {
        const char *escaped = *p == '&' ? "&amp;"
                            : *p == '<' ? "&lt;"
                            : *p == '>' ? "&gt;"
                                        : NULL;
        if (escaped) {
            length += (size_t)snprintf(out + length, size - length, "%s", escaped);
            continue;
        }
        out[length++] = *p;
    }
    out[length] = '\0';
}

/* Takes the name the panel edits, unless the dialog has just opened or an
 * operation holds it, then shows the dialog's state. */
static bool sync_new_project(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *page = at->variable;
    yyjson_mut_val *name = yyjson_mut_obj_get(page, "name");
    const char *text = yyjson_mut_get_str(name);
    studio_new_project *dialog = &s->new_project;
    if (!at->preview && !dialog->focus && !dialog->waiting &&
        !dialog->confirm_save && strcmp(dialog->name, text)) {
        snprintf(dialog->name, sizeof dialog->name, "%s", text);
        dialog->error[0] = '\0';
        dialog->created_root[0] = '\0';
    }
    yyjson_mut_val *error = yyjson_mut_obj_get(page, "error");
    yyjson_mut_val *waiting = yyjson_mut_obj_get(page, "waiting");
    yyjson_mut_val *confirmation = yyjson_mut_obj_get(page, "confirm_save");
    yyjson_mut_val *confirm = yyjson_mut_obj_get(page, "confirm");
    yyjson_mut_val *location = yyjson_mut_obj_get(page, "location");
    yyjson_mut_val *folder = yyjson_mut_obj_get(page, "folder");
    char shown[sizeof dialog->folder + 4];
    studio_new_project_location(s, shown, sizeof shown);
    bool changed = strcmp(text, dialog->name) ||
                   strcmp(yyjson_mut_get_str(error), dialog->error) ||
                   strcmp(yyjson_mut_get_str(confirm), dialog->confirm) ||
                   strcmp(yyjson_mut_get_str(location), shown) ||
                   strcmp(yyjson_mut_get_str(folder), dialog->folder) ||
                   yyjson_mut_get_bool(waiting) != dialog->waiting ||
                   yyjson_mut_get_bool(confirmation) != dialog->confirm_save;
    if (!changed) {
        return false;
    }
    ui_data_set_text(at->doc, name, dialog->name);
    ui_data_set_text(at->doc, error, dialog->error);
    char markup[sizeof dialog->error * 6 + 1];
    escape_markup(markup, sizeof markup, dialog->error);
    ui_data_set_text(at->doc, yyjson_mut_obj_get(page, "error_markup"), markup);
    yyjson_mut_set_bool(waiting, dialog->waiting);
    ui_data_set_text(at->doc, confirm, dialog->confirm);
    ui_data_set_text(at->doc, location, shown);
    ui_data_set_text(at->doc, folder, dialog->folder);
    yyjson_mut_set_bool(confirmation, dialog->confirm_save);
    return true;
}

/* ---------- Panel sources ---------- */

/* Runs source `i`'s sync over `data` and dirties its variable when it
 * changed. */
static void sync_source(studio *s, studio_sources *src, ui_data *data, int i, bool wrote)
{
    const studio_panel_source *source = &studio_panel_sources[i];
    studio_panel_sync at = {
        .sources = src,
        .doc = data->doc,
        .variable = yyjson_mut_obj_get(
            yyjson_mut_doc_get_root(data->doc),
            source->name
        ),
        .model = data->model,
        .preview = src->preview,
        .wrote = wrote,
    };
    assert(at.variable);
    if (source->sync(s, &at)) {
        rml_data_model_dirty(data->model, source->name);
    }
}

yyjson_mut_doc *studio_sources_new()
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    for (int i = 0; i < studio_panel_source_count; i++) {
        studio_panel_sources[i].add(doc);
    }
    return doc;
}

/* A source syncs while its panel shows, as the last UI refresh mounted it,
 * and whenever a binding wrote, so no edit is lost to a panel closing. Only
 * a shown panel's bindings can have written its variable: a hidden one's
 * values are only stale, and taking them would undo what changed since,
 * as the Members filter's typing once reset a density a command set. */
void studio_sources_sync(studio *s, studio_sources *src, ui_data *data)
{
    uint64_t writes = rml_data_model_writes(data->model);
    bool wrote = writes != src->writes_seen;
    src->writes_seen = writes;
    for (int i = 0; i < studio_panel_source_count; i++) {
        bool shown = !studio_panel_sources[i].panel ||
                     src->shown_at[i] >= s->ui_schedule.frames;
        if (shown || wrote) {
            sync_source(s, src, data, i, wrote && shown);
        }
    }
}

void studio_sources_resync(studio *s, studio_sources *src, ui_data *data, const char *name)
{
    for (int i = 0; i < studio_panel_source_count; i++) {
        if (!strcmp(studio_panel_sources[i].name, name)) {
            sync_source(s, src, data, i, false);
        }
    }
}

void studio_sources_mounted(studio *s, studio_sources *src, ui_data *data, const char *panel)
{
    for (int i = 0; i < studio_panel_source_count; i++) {
        const char *shows = studio_panel_sources[i].panel;
        if (!shows || strcmp(shows, panel)) {
            continue;
        }
        if (src->shown_at[i] < s->ui_schedule.frames) {
            sync_source(s, src, data, i, false);
        }
        src->shown_at[i] = s->ui_schedule.frames + 1;
    }
}

void studio_sources_acted(studio *s, studio_sources *src, ui_data *data, const char *event)
{
    const char *dot = strchr(event, '.');
    bool wrote = rml_data_model_writes(data->model) != src->writes_seen;
    for (int i = 0; dot && i < studio_panel_source_count; i++) {
        const studio_panel_source *source = &studio_panel_sources[i];
        if (source->action && strlen(source->name) == (size_t)(dot - event) &&
            !strncmp(source->name, event, (size_t)(dot - event))) {
            sync_source(s, src, data, i, wrote);
        }
    }
}

void studio_sources_show(studio *s, studio_sources *src, ui_data *data)
{
    src->preview = true;
    for (int i = 0; i < studio_panel_source_count; i++) {
        sync_source(s, src, data, i, false);
    }
}

void studio_sources_search_settings(ui_data *data, const char *query)
{
    yyjson_mut_val *page = settings_page(data->doc);
    ui_data_put_text(data->doc, page, "scope", "editor");
    ui_data_put_text(data->doc, page, "query", query);
    rml_data_model_dirty(data->model, "settings");
}

void studio_sources_free(studio_sources *src)
{
    free_tokens(src);
    ui_page_free(&src->page);
    *src = (studio_sources){ 0 };
}

/* ---------- Inspector ---------- */

/* `inspector` is what the Inspector panel shows (studio_inspect.c): its
 * target and pick state, and the selection's box, styles, computed values
 * and attributes. Its strings point at the Inspector's own, which it keeps
 * until its next read, so showing a model allocates nothing once each list
 * has its items; the items a list sheds wait in `spare` until it grows
 * again. The panel writes what it asks for: rule (a Styles row, -1 for
 * none), action ("source" for Edit Source), target, and force (the pseudo
 * classes forced).
 *
 * Its source keeps two variables apart, so a change reads only the views
 * that bind it: `inspector_tree`, the window of Elements rows its List
 * shows, each keyed by its element (the key is its address as text),
 * and `inspector_ui`, the keys of the selected row and of the row a pick is
 * over ("" for none), which TreeRow compares with each row's
 * (data-alias-selection). A row sends inspector.click with its key and its
 * open state, which its twisty may have flipped; the List sends
 * inspector.scroll, which only wakes the source. */

static const char *const TARGET_NAMES[STUDIO_INSPECT_TARGET_COUNT] = { "studio",
                                                                       "graph",
                                                                       "game" };
static const char *const PSEUDO_KEYS[] = { "hover", "active", "focus", "checked", NULL };
static const char *const EDGE_KEYS[] = { "top", "right", "bottom", "left", NULL };
static const char *const BOX_KEYS[] = { "margin", "border", "padding" };

/* A list item's members: strings, then booleans, each list NULL-terminated. */
typedef struct item_shape
{
    const char *const *texts;
    const char *const *flags;
    const char *const *ints; /* or NULL */
} item_shape;

/* A TreeRow's item: the tag is its label; the icon and meta stay empty. */
static const char *const ROW_TEXTS[] = { "key",     "label",  "id",
                                         "classes", "widget", "text",
                                         "icon",    "meta",   NULL };
static const char *const ROW_FLAGS[] = { "parent", "open", "node", "hidden", NULL };
static const char *const ROW_INTS[] = { "depth", NULL };
static const char *const STYLE_TEXTS[] = { "selector", "origin", "name", "value", NULL };
static const char *const STYLE_FLAGS[] = { "heading", "ancestor", "inherited", "linked", NULL };
static const char *const PAIR_TEXTS[] = { "name", "value", NULL };
static const char *const NO_FLAGS[] = { NULL };
static const item_shape ROW = { ROW_TEXTS, ROW_FLAGS, ROW_INTS };
static const item_shape STYLE = { STYLE_TEXTS, STYLE_FLAGS };
static const item_shape PAIR = { PAIR_TEXTS, NO_FLAGS };

static void add_texts(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys)
{
    for (; *keys; keys++) {
        yyjson_mut_obj_add_str(doc, obj, *keys, "");
    }
}

static void add_flags(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *const *keys)
{
    for (; *keys; keys++) {
        yyjson_mut_obj_add_bool(doc, obj, *keys, false);
    }
}

static void add_inspector(yyjson_mut_doc *doc)
{
    static const char *const TEXTS[] = { "target", "shortcut", "title",
                                         "path",   "source",   "pseudo",
                                         "size",   "action",   NULL };
    static const char *const FLAGS[] = { "picking", "wide",      "graph",
                                         "game",    "selection", "detached",
                                         "native",  "editable",  NULL };
    static const char *const LISTS[] = { "styles", "computed", "attributes" };
    static const char *const SPARES[] = { "styles", "pairs" };
    yyjson_mut_val *in = root_object(doc, "inspector");
    add_texts(doc, in, TEXTS);
    add_flags(doc, in, FLAGS);
    yyjson_mut_obj_add_str(doc, in, "tab", "styles"); /* the panel's own */
    yyjson_mut_obj_add_int(doc, in, "rule", -1);
    add_flags(doc, yyjson_mut_obj_add_obj(doc, in, "force"), PSEUDO_KEYS);
    yyjson_mut_val *box = yyjson_mut_obj_add_obj(doc, in, "box");
    for (size_t b = 0; b < sizeof BOX_KEYS / sizeof *BOX_KEYS; b++) {
        add_texts(doc, yyjson_mut_obj_add_obj(doc, box, BOX_KEYS[b]), EDGE_KEYS);
    }
    for (size_t i = 0; i < sizeof LISTS / sizeof *LISTS; i++) {
        yyjson_mut_obj_add_arr(doc, in, LISTS[i]);
    }
    yyjson_mut_val *spare = yyjson_mut_obj_add_obj(doc, in, "spare");
    for (size_t i = 0; i < sizeof SPARES / sizeof *SPARES; i++) {
        yyjson_mut_obj_add_arr(doc, spare, SPARES[i]);
    }
    yyjson_mut_val *tree = root_object(doc, "inspector_tree");
    yyjson_mut_obj_add_arr(doc, tree, "rows");
    yyjson_mut_obj_add_arr(doc, tree, "spare");
    yyjson_mut_val *ui = root_object(doc, "inspector_ui");
    yyjson_mut_obj_add_str(doc, ui, "selected", "");
    yyjson_mut_obj_add_str(doc, ui, "picked", "");
}

static yyjson_mut_val *member(yyjson_mut_val *obj, const char *key)
{
    return yyjson_mut_obj_get(obj, key);
}

static const char *text_at(yyjson_mut_val *obj, const char *key)
{
    const char *text = yyjson_mut_get_str(member(obj, key));
    return text ? text : "";
}

static int number_at(yyjson_mut_val *obj, const char *key)
{
    yyjson_mut_val *v = member(obj, key);
    return yyjson_mut_is_num(v) ? (int)yyjson_mut_get_num(v) : -1;
}

/* A request the panel writes once, taken and cleared. */
static int take(yyjson_mut_val *in, const char *key)
{
    int value = number_at(in, key);
    yyjson_mut_set_int(member(in, key), -1);
    return value;
}

static int target_named(const char *name)
{
    for (int t = 0; t < STUDIO_INSPECT_TARGET_COUNT; t++) {
        if (!strcmp(TARGET_NAMES[t], name)) {
            return t;
        }
    }
    return -1;
}

static studio_inspect_input inspector_input(yyjson_mut_val *in)
{
    studio_inspect_input input = {
        .rule = take(in, "rule"),
        .target = target_named(text_at(in, "target")),
        .source = !strcmp(text_at(in, "action"), "source"),
    };
    yyjson_mut_set_str(member(in, "action"), "");
    for (int p = 0; p < STUDIO_PSEUDO_COUNT; p++) {
        input.force[p] = yyjson_mut_get_bool(member(member(in, "force"), PSEUDO_KEYS[p]));
    }
    return input;
}

/* Points string member `key` at `text`, without a copy. */
static void point(yyjson_mut_val *obj, const char *key, const char *text)
{
    yyjson_mut_set_strn(member(obj, key), text, strlen(text));
}

/* Whether the flag changed. */
static bool flag(yyjson_mut_val *obj, const char *key, bool on)
{
    yyjson_mut_val *value = member(obj, key);
    bool changed = yyjson_mut_get_bool(value) != on;
    yyjson_mut_set_bool(value, on);
    return changed;
}

static yyjson_mut_val *make_item(yyjson_mut_doc *doc, const item_shape *shape)
{
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    add_texts(doc, item, shape->texts);
    add_flags(doc, item, shape->flags);
    for (const char *const *key = shape->ints; key && *key; key++) {
        yyjson_mut_obj_add_int(doc, item, *key, 0);
    }
    return item;
}

/* Gives `list` `count` items, shedding extra ones to `spare` and taking
 * new ones from it before making any. */
static void resize(
    yyjson_mut_doc *doc,
    yyjson_mut_val *list,
    yyjson_mut_val *spare,
    int count,
    const item_shape *shape
)
{
    while ((int)yyjson_mut_arr_size(list) > count) {
        yyjson_mut_arr_append(spare, yyjson_mut_arr_remove_last(list));
    }
    while ((int)yyjson_mut_arr_size(list) < count) {
        yyjson_mut_val *item = yyjson_mut_arr_size(spare)
                                 ? yyjson_mut_arr_remove_last(spare)
                                 : make_item(doc, shape);
        yyjson_mut_arr_append(list, item);
    }
}

/* Shows an item's members; true when a flag differed. */
static bool show_item(
    yyjson_mut_val *item,
    const item_shape *shape,
    const char *const *texts,
    const bool *flags
)
{
    bool changed = false;
    for (int t = 0; shape->texts[t]; t++) {
        point(item, shape->texts[t], texts[t]);
    }
    for (int f = 0; shape->flags[f]; f++) {
        changed |= flag(item, shape->flags[f], flags[f]);
    }
    return changed;
}

/* The window of rows; true when a flag or depth differed, which a twisty's
 * flip of `open` may leave. */
static bool show_elements(yyjson_mut_doc *doc, yyjson_mut_val *tree, const studio_inspect_model *m)
{
    yyjson_mut_val *list = member(tree, "rows");
    bool changed = (int)yyjson_mut_arr_size(list) != acount(m->rows);
    resize(doc, list, member(tree, "spare"), acount(m->rows), &ROW);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(list);
    for (int i = 0; i < acount(m->rows); i++) {
        const studio_inspect_row *r = &m->rows[i];
        const char *texts[] = { r->key,    r->tag,  r->id, r->classes,
                                r->widget, r->text, "",    "" };
        bool flags[] = { r->parent, r->open, r->node, r->hidden };
        yyjson_mut_val *item = yyjson_mut_arr_iter_next(&it);
        changed |= show_item(item, &ROW, texts, flags);
        changed |= ui_data_put_int(item, "depth", r->depth);
    }
    return changed;
}

static void show_styles(yyjson_mut_doc *doc, yyjson_mut_val *in, const studio_inspect_style *styles)
{
    yyjson_mut_val *list = member(in, "styles");
    resize(doc, list, member(member(in, "spare"), "styles"), acount(styles), &STYLE);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(list);
    for (int i = 0; i < acount(styles); i++) {
        const studio_inspect_style *st = &styles[i];
        const char *texts[] = { st->selector, st->origin, st->name, st->value };
        bool flags[] = { st->heading, st->ancestor, st->inherited, st->linked };
        show_item(yyjson_mut_arr_iter_next(&it), &STYLE, texts, flags);
    }
}

static void show_pairs(yyjson_mut_doc *doc, yyjson_mut_val *in, const char *key, const studio_inspect_pair *pairs)
{
    yyjson_mut_val *list = member(in, key);
    resize(doc, list, member(member(in, "spare"), "pairs"), acount(pairs), &PAIR);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(list);
    for (int i = 0; i < acount(pairs); i++) {
        const char *texts[] = { pairs[i].name, pairs[i].value };
        show_item(yyjson_mut_arr_iter_next(&it), &PAIR, texts, NULL);
    }
}

static void show_inspector(yyjson_mut_doc *doc, yyjson_mut_val *in, const studio_inspect_model *m)
{
    point(in, "target", TARGET_NAMES[m->target]);
    const char *texts[] = { m->shortcut, m->title,  m->path,
                            m->source,   m->pseudo, m->size };
    static const char *const TEXTS[] = { "shortcut", "title",  "path",
                                         "source",   "pseudo", "size" };
    for (size_t t = 0; t < sizeof TEXTS / sizeof *TEXTS; t++) {
        point(in, TEXTS[t], texts[t]);
    }
    bool flags[] = { m->picking,
                     m->wide,
                     m->targets[STUDIO_INSPECT_GRAPH],
                     m->targets[STUDIO_INSPECT_GAME],
                     m->selection,
                     m->detached,
                     m->native,
                     m->editable };
    static const char *const FLAGS[] = { "picking", "wide",      "graph",
                                         "game",    "selection", "detached",
                                         "native",  "editable" };
    for (size_t f = 0; f < sizeof FLAGS / sizeof *FLAGS; f++) {
        flag(in, FLAGS[f], flags[f]);
    }
    for (int p = 0; p < STUDIO_PSEUDO_COUNT; p++) {
        flag(member(in, "force"), PSEUDO_KEYS[p], m->force[p]);
    }
    for (size_t b = 0; b < sizeof BOX_KEYS / sizeof *BOX_KEYS; b++) {
        for (int e = 0; EDGE_KEYS[e]; e++) {
            point(member(member(in, "box"), BOX_KEYS[b]), EDGE_KEYS[e], m->box[b][e]);
        }
    }
    show_styles(doc, in, m->styles);
    show_pairs(doc, in, "computed", m->computed);
    show_pairs(doc, in, "attributes", m->attributes);
}

/* Shows the Inspector's model when its revision is not the one shown, as its
   strings may have moved since; a preview shows the model as it stands. The
   Inspector reads nothing while neither shown nor picking, so it syncs at
   every refresh. */
static const studio_inspect_model *inspector_model(studio *s, const studio_panel_sync *at)
{
    if (at->preview) {
        return studio_inspect_shown(s);
    }
    studio_inspect_input input = inspector_input(at->variable);
    return studio_inspect_sync(s, &input);
}

/* The window of rows and the keys the rows compare, each dirtying only its
 * own variable. */
static void show_tree(const studio_panel_sync *at, const studio_inspect_model *m)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(at->doc);
    bool rows = m->rows_revision != at->sources->inspected_rows;
    at->sources->inspected_rows = m->rows_revision;
    if (show_elements(at->doc, member(root, "inspector_tree"), m) || rows) {
        rml_data_model_dirty(at->model, "inspector_tree");
    }
    if (m->marks_revision != at->sources->inspected_marks) {
        at->sources->inspected_marks = m->marks_revision;
        yyjson_mut_val *ui = member(root, "inspector_ui");
        point(ui, "selected", m->selected_key);
        point(ui, "picked", m->picked_key);
        rml_data_model_dirty(at->model, "inspector_ui");
    }
}

static bool sync_inspector(studio *s, const studio_panel_sync *at)
{
    const studio_inspect_model *m = inspector_model(s, at);
    if (!m) {
        return false;
    }
    show_tree(at, m);
    if (m->revision == at->sources->inspected) {
        return false;
    }
    at->sources->inspected = m->revision;
    show_inspector(at->doc, at->variable, m);
    return true;
}

/* A row's click names its key and its open state; a scroll only wakes the
 * source, which reads where the List is. */
static void inspector_action(studio *s, const char *action, const ui_activation *a)
{
    if (!strcmp(action, "click") && a->argc == 2) {
        studio_inspect_click(s, a->args[0], atoi(a->args[1]) != 0);
    }
}

/* ---------- Editor (workstreams J and T) ---------- */

/* `editor`, what the editor's authored chrome shows (studio_status): for
 * the status bar, `hints`, a list of {icon, key, label}, report,
 * reporting and warning, scene, selection, objects, speed ("" at 1×) and
 * playing; for the top bar, `tabs`, a list of {label, path, icon, dirty,
 * active, closable}, and paused; and `areas`, an {index, drawer, maximized,
 * live, editor, panel, icon, title} per area, which each AreaHeader binds as
 * `area`: its editor type menu runs emit('editor.type', area.index, type) and
 * emit('editor.panel', area.index, name). Values change only when they differ,
 * so bindings update in place, and items a list sheds wait in `spare`. */

/* Reports show for three seconds, then fade (DESIGN.md). */
#define REPORT_SECONDS 3.0

static const char *const HINT_TEXTS[] = { "icon", "key", "label", NULL };
static const item_shape HINT = { HINT_TEXTS, NO_FLAGS };
static const char *const TAB_TEXTS[] = { "label",    "path",        "icon",
                                         "pinLabel", "closeReason", "shortcut",
                                         NULL };
static const char *const TAB_FLAGS[] = { "dirty",    "active",   "closable",
                                         "pinned",   "pinnable", "checked",
                                         "disabled", NULL };
static const item_shape TAB = { TAB_TEXTS, TAB_FLAGS };
static const char *const STATUS_TEXTS[] = { "report", "scene", "selection", "speed", NULL };
static const char *const STATUS_FLAGS[] = { "reporting", "warning", "playing", "paused", NULL };
static const char *const AREA_TEXTS[] = { "panel", "icon", "title", NULL };

static void add_areas(yyjson_mut_doc *doc, yyjson_mut_val *editor)
{
    yyjson_mut_val *areas = yyjson_mut_obj_add_arr(doc, editor, "areas");
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        yyjson_mut_val *area = yyjson_mut_arr_add_obj(doc, areas);
        yyjson_mut_obj_add_int(doc, area, "index", i);
        yyjson_mut_obj_add_bool(doc, area, "drawer", i == STUDIO_AREA_DRAWER);
        yyjson_mut_obj_add_bool(doc, area, "maximized", false);
        yyjson_mut_obj_add_bool(doc, area, "live", false);
        yyjson_mut_obj_add_real(doc, area, "editor", -1);
        add_texts(doc, area, AREA_TEXTS);
    }
}

void studio_status_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *editor = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "editor");
    yyjson_mut_obj_add_arr(doc, editor, "hints");
    yyjson_mut_obj_add_arr(doc, editor, "tabs");
    yyjson_mut_val *spare = yyjson_mut_obj_add_obj(doc, editor, "spare");
    yyjson_mut_obj_add_arr(doc, spare, "hints");
    yyjson_mut_obj_add_arr(doc, spare, "tabs");
    add_texts(doc, editor, STATUS_TEXTS);
    add_flags(doc, editor, STATUS_FLAGS);
    yyjson_mut_obj_add_real(doc, editor, "selected", 0);
    yyjson_mut_obj_add_real(doc, editor, "objects", 0);
    yyjson_mut_obj_add_real(doc, editor, "hidden", 0);
    add_areas(doc, editor);
}

static bool show_hints(yyjson_mut_doc *doc, yyjson_mut_val *editor, const studio_status *status)
{
    yyjson_mut_val *list = member(editor, "hints");
    bool changed = (int)yyjson_mut_arr_size(list) != status->hint_count;
    resize(doc, list, member(member(editor, "spare"), "hints"), status->hint_count, &HINT);
    for (int i = 0; i < status->hint_count; i++) {
        const studio_hint *h = &status->hints[i];
        yyjson_mut_val *item = yyjson_mut_arr_get(list, (size_t)i);
        char icon[5] = "";
        if (h->icon) {
            studio_glyph(icon, h->icon);
        }
        changed |= ui_data_put_text(doc, item, "icon", icon);
        changed |= ui_data_put_text(doc, item, "key", h->key ? h->key : "");
        changed |= ui_data_put_text(doc, item, "label", h->label);
    }
    return changed;
}

static bool show_tabs(yyjson_mut_doc *doc, yyjson_mut_val *editor, const studio_status *status)
{
    yyjson_mut_val *list = member(editor, "tabs");
    bool changed = (int)yyjson_mut_arr_size(list) != status->tab_count;
    resize(doc, list, member(member(editor, "spare"), "tabs"), status->tab_count, &TAB);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(list);
    for (int i = 0; i < status->tab_count; i++) {
        const studio_tab_view *t = &status->tabs[i];
        yyjson_mut_val *item = yyjson_mut_arr_iter_next(&it);
        char icon[5];
        studio_glyph(icon, t->icon);
        const char *texts[] = { t->label,     t->path,         icon,
                                t->pin_label, t->close_reason, "" };
        for (int k = 0; TAB_TEXTS[k]; k++) {
            changed |= ui_data_put_text(doc, item, TAB_TEXTS[k], texts[k]);
        }
        const bool flags[] = { t->dirty,    t->active, t->closable, t->pinned,
                               t->pinnable, t->active, false };
        for (int f = 0; TAB_FLAGS[f]; f++) {
            changed |= ui_data_put_bool(item, TAB_FLAGS[f], flags[f]);
        }
    }
    return changed;
}

static bool show_areas(yyjson_mut_doc *doc, yyjson_mut_val *editor, const studio_status *status)
{
    bool changed = false;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(member(editor, "areas"));
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        yyjson_mut_val *area = yyjson_mut_arr_iter_next(&it);
        changed |= ui_data_put_bool(area, "maximized", i == status->maximized);
        changed |= ui_data_put_bool(area, "live", status->live & 1u << i);
        const studio_area_view *view = &status->areas[i];
        char icon[5] = "";
        if (view->icon) {
            studio_glyph(icon, view->icon);
        }
        changed |= ui_data_put_number(area, "editor", view->editor);
        const char *texts[] = { view->panel ? view->panel : "",
                                icon,
                                view->title ? view->title : "" };
        for (int t = 0; AREA_TEXTS[t]; t++) {
            changed |= ui_data_put_text(doc, area, AREA_TEXTS[t], texts[t]);
        }
    }
    return changed;
}

bool studio_status_show(yyjson_mut_doc *doc, const studio_status *status)
{
    yyjson_mut_val *editor = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "editor");
    bool changed = show_hints(doc, editor, status);
    changed |= show_tabs(doc, editor, status);
    changed |= show_areas(doc, editor, status);
    const char *texts[] = { status->report,
                            status->scene,
                            status->selection,
                            status->speed };
    for (int t = 0; STATUS_TEXTS[t]; t++) {
        changed |= ui_data_put_text(doc, editor, STATUS_TEXTS[t], texts[t] ? texts[t] : "");
    }
    const bool flags[] = { status->reporting,
                           status->warning,
                           status->playing,
                           status->paused };
    for (int f = 0; STATUS_FLAGS[f]; f++) {
        changed |= ui_data_put_bool(editor, STATUS_FLAGS[f], flags[f]);
    }
    changed |= ui_data_put_number(editor, "selected", status->selected);
    changed |= ui_data_put_number(editor, "objects", status->objects);
    changed |= ui_data_put_number(editor, "hidden", status->hidden);
    return changed;
}

typedef struct hint_list
{
    const studio_hint *hints;
    int count;
} hint_list;

#define HINT_LIST(list) ((hint_list){ list, (int)(sizeof list / sizeof *list) })

/* The Scene's hints follow its tool and gesture (Blender's status bar):
 * what the pointer does, then the keys that change it. */
static hint_list scene_hints(studio *s)
{
    static studio_hint hints[STUDIO_HINT_MAX];
    const scene_gizmo *g = &s->scene->gizmo;
    const char *snap = s->prefs.scene_snap ? "Free" : "Snap";
    int n = 0;
    if (g->modal) {
        hints[n++] = (studio_hint){ KIT_ICON_MOUSE, NULL, "Confirm" };
        hints[n++] = (studio_hint){ 0, "Escape", "Cancel" };
        hints[n++] = (studio_hint){ 0, "X/Y", "Constrain" };
        hints[n++] = (studio_hint){ 0, "Ctrl", snap };
        return (hint_list){ hints, n };
    }
    if (g->active) {
        hints[n++] = (studio_hint){ 0, "Escape", "Cancel" };
        hints[n++] = (studio_hint){ 0, "Ctrl", snap };
        if (g->active >= SCENE_GIZMO_HANDLE) {
            hints[n++] = (studio_hint){ 0, "Shift", "Keep aspect" };
            hints[n++] = (studio_hint){ 0, "Alt", "From center" };
        }
        return (hint_list){ hints, n };
    }
    if (studio_scene_flying(s)) {
        hints[n++] = (studio_hint){ KIT_ICON_MOUSE, NULL, "Look" };
        hints[n++] = (studio_hint){ 0, "W/A/S/D", "Fly" };
        hints[n++] = (studio_hint){ 0, "Q/E", "Down/Up" };
        hints[n++] = (studio_hint){ 0, "Shift", "Faster" };
        return (hint_list){ hints, n };
    }
    bool perspective = engine_viewport_perspective(s->viewport);
    scene_tool tool = s->prefs.tool;
    if (tool != SCENE_TOOL_SELECT && !scene_gizmo_enabled(s->scene)) {
        hints[n++] = (studio_hint){ KIT_ICON_WARNING,
                                    NULL,
                                    tool == SCENE_TOOL_ROTATE
                                        ? "Rotate needs an Orientation"
                                        : "Scale needs a size" };
    }
    switch (tool) {
        case SCENE_TOOL_MOVE:
            hints[n++] = (studio_hint){ KIT_ICON_MOUSE, NULL, "Drag arrows" };
            break;
        case SCENE_TOOL_ROTATE:
            hints[n++] = (studio_hint){ KIT_ICON_MOUSE,
                                        NULL,
                                        perspective ? "Drag rings" : "Drag ring" };
            break;
        case SCENE_TOOL_SCALE:
            hints[n++] = (studio_hint){ KIT_ICON_MOUSE, NULL, "Drag handles" };
            break;
        default:
            hints[n++] = (studio_hint){ KIT_ICON_MOUSE, NULL, "Select" };
            break;
    }
    if (perspective) {
        /* The camera's keys first: a 3D view is hard to look around without them. */
        hints[n++] = (studio_hint){ KIT_ICON_MOUSE, "Alt", "Orbit" };
        hints[n++] = (studio_hint){ KIT_ICON_MOUSE, NULL, "Right-drag to fly" };
        studio_hint more[] = {
            { 0, NULL, s->prefs.scene_local ? "World" : "Local", STUDIO_COMMAND_TOGGLE_LOCAL + 1 },
            { KIT_ICON_MOUSE, "Shift", "Toggle" },
            { 0, NULL, "Frame", STUDIO_COMMAND_FRAME + 1 },
        };
        /* Select shows no gizmo for Local to turn. */
        int first = tool == SCENE_TOOL_SELECT ? 1 : 0;
        for (int i = first; i < 3 && n < STUDIO_HINT_MAX; i++) {
            hints[n++] = more[i];
        }
        return (hint_list){ hints, n };
    }
    hints[n++] = (studio_hint){ KIT_ICON_MOUSE, "Shift", "Toggle" };
    hints[n++] = (studio_hint){ 0, NULL, "Grab", STUDIO_COMMAND_GRAB + 1 };
    if (n < STUDIO_HINT_MAX) {
        hints[n++] = (studio_hint){ 0, "Ctrl", snap };
    }
    if (n < STUDIO_HINT_MAX) {
        hints[n++] = (studio_hint){ 0, NULL, "Frame", STUDIO_COMMAND_FRAME + 1 };
    }
    if (n < STUDIO_HINT_MAX) {
        hints[n++] = (studio_hint){ 0, NULL, "Hide", STUDIO_COMMAND_HIDE_SELECTED + 1 };
    }
    return (hint_list){ hints, n };
}

/* The key hints of the editor under the pointer; keys in studio_chord
 * notation. */
static hint_list editor_hints(studio *s)
{
    static const studio_hint PROPERTIES[] = { { KIT_ICON_MOUSE, NULL, "Drag to scrub" },
                                              { 0, "Alt", "Fine" },
                                              { 0, "Shift", "Coarse" },
                                              { 0, "Ctrl", "Snap" } };
    static const studio_hint OUTLINER[] = {
        { KIT_ICON_MOUSE, NULL, "Select" },
        { KIT_ICON_MOUSE, "Mod", "Toggle" },
        { KIT_ICON_MOUSE, "Shift", "Range" },
        { 0, NULL, "Frame", STUDIO_COMMAND_FRAME + 1 },
        { 0, NULL, "Rename", STUDIO_COMMAND_RENAME + 1 }
    };
    /* Visual designer (workstream H): the Outliner as a UI document's Hierarchy. */
    static const studio_hint HIERARCHY[] = { { KIT_ICON_MOUSE, NULL, "Select" },
                                             { KIT_ICON_MOVE, NULL, "Drag to move" },
                                             { 0, "Shift+D", "Duplicate" },
                                             { 0, "X", "Delete" } };
    static const studio_hint MEMBERS[] = { { KIT_ICON_MOUSE, NULL, "Drag to graph" },
                                           { 0, "Alt", "Set" },
                                           { 0, "F2", "Rename" },
                                           { 0, "X", "Delete" } };
    static const studio_hint BEHAVIOR[] = { { KIT_ICON_MOUSE, NULL, "Select" },
                                            { 0, "Shift+A", "Add" },
                                            { 0, "Q", "Straighten" } };
    /* A read-only graph offers no edits; its header says why. */
    static const studio_hint READONLY[] = { { KIT_ICON_MOUSE, NULL, "Select" },
                                            { 0, "F", "Frame" } };
    /* While the game runs, the graph follows the object selected. */
    static const studio_hint DEBUGGING[] = {
        { KIT_ICON_MOUSE, NULL, "Select" },
        { KIT_ICON_TARGET, NULL, "Select an object to debug it" },
        { 0, "F9", "Breakpoint" }
    };
    static const studio_hint MOVING[] = { { 0, "Ctrl", "Snap" } };
    static const studio_hint OTHER[] = {
        { KIT_ICON_MOUSE, NULL, "Select" },
        { 0, NULL, "Search", STUDIO_COMMAND_SEARCH + 1 },
        { 0, NULL, "Shortcuts", STUDIO_COMMAND_KEYBOARD_SHORTCUTS + 1 }
    };
    /* A drag drops wherever it is released, and Escape puts it back. */
    static const studio_hint DRAGGING[] = { { KIT_ICON_MOUSE, NULL, "Release to drop" },
                                            { 0, "Escape", "Cancel" } };
    studio_editor editor = s->hover_area >= 0
                             ? studio_active_layout(s)->editors[s->hover_area]
                             : STUDIO_EDITOR_COUNT;
    /* A Canvas resize handle: Shift keeps the aspect at the corner. */
    static const studio_hint RESIZING[] = { { KIT_ICON_MOUSE, NULL, "Release to resize" },
                                            { 0, "Escape", "Cancel" },
                                            { 0, "Shift", "Keep aspect" } };
    if (kit_dragging(s->kit, NULL)) {
        return HINT_LIST(DRAGGING);
    }
    if (studio_designer_resizing(s)) {
        return HINT_LIST(RESIZING);
    }
    switch (editor) {
        case STUDIO_EDITOR_SCENE: return scene_hints(s);
        case STUDIO_EDITOR_PROPERTIES: return HINT_LIST(PROPERTIES);
        case STUDIO_EDITOR_OUTLINER:
            return studio_designer_active(s) ? HINT_LIST(HIERARCHY)
                                             : HINT_LIST(OUTLINER);
        case STUDIO_EDITOR_MEMBERS: return HINT_LIST(MEMBERS);
        case STUDIO_EDITOR_BEHAVIOR:
            if (!studio_graph_active(s)) {
                return HINT_LIST(OTHER);
            }
            if (s->graph.gesture == STUDIO_GRAPH_MOVE) {
                return HINT_LIST(MOVING);
            }
            if (studio_session(s)) {
                return HINT_LIST(DEBUGGING);
            }
            return sg_graph_readonly(s) ? HINT_LIST(READONLY) : HINT_LIST(BEHAVIOR);
        default: return HINT_LIST(OTHER);
    }
}

/* The name of an object the running scene spawned, when one is selected. */
static const char *spawned_selection(app_scene_authoring *scene)
{
    ecs_entity entity = selected_scene_entity(scene);
    return scene->runtime_selected.slot && entity.slot
             ? runtime_entity_label(scene, entity)
             : "";
}

static bool sync_editor(studio *s, const studio_panel_sync *at)
{
    char title[128], speed[16];
    scene_actor *actor = selected_actor(s->scene);
    studio_tab_view *tabs = studio_document_tabs(s);
    studio_speed_text(s, speed, sizeof speed);
    studio_status status = {
        .report = s->status,
        .reporting = s->status[0] && s->time - s->status_time < REPORT_SECONDS,
        .warning = s->status_warning,
        .scene = studio_scene_title(s->scene, title, sizeof title),
        .selection = actor ? actor->name : spawned_selection(s->scene),
        .selected = scene_selection_count(s->scene) +
                    (spawned_selection(s->scene)[0] != '\0'),
        .objects = acount(s->scene->actors),
        .hidden = studio_hidden_count(s),
        .speed = speed,
        .playing = app_scene_authoring_runtime_state(s->scene).playing || s->guest != NULL,
        .paused = s->pause.active,
        .tabs = tabs,
        .tab_count = acount(tabs),
        .maximized = s->maximized,
    };
    hint_list hints = editor_hints(s);
    assert(hints.count <= STUDIO_HINT_MAX);
    status.hint_count = hints.count;
    for (int i = 0; i < hints.count; i++) {
        status.hints[i] = hints.hints[i];
        const char *chord = hints.hints[i].command
                              ? studio_command_chord(&s->prefs, hints.hints[i].command - 1)
                              : hints.hints[i].key;
        status.hints[i].key = chord ? studio_chord(s, chord) : NULL;
    }
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        status.areas[i] = studio_area_view_of(s, i);
    }
    bool live = studio_properties_live(s);
    for (int i = 0; live && i < STUDIO_AREA_COUNT; i++) {
        if (studio_active_layout(s)->editors[i] == STUDIO_EDITOR_PROPERTIES) {
            status.live |= 1u << i;
        }
    }
    bool changed = studio_status_show(at->doc, &status);
    afree(tabs);
    return changed;
}

/* An area header's editor type menu: emit('editor.type', area, type) shows
   an editor type, emit('editor.panel', area, name) a Studio panel. */
static void editor_action(studio *s, const char *action, const ui_activation *a)
{
    if (a->argc < 2) {
        return;
    }
    int area = atoi(a->args[0]);
    if (!strcmp(action, "type")) {
        studio_area_show(s, area, atoi(a->args[1]), "");
    } else if (!strcmp(action, "panel")) {
        studio_area_show(s, area, STUDIO_EDITOR_PANEL, a->args[1]);
    }
}

/* ---------- Commands (workstream T) ---------- */

/* `commands`, each command's state by id (studio_command_state): its id,
 * label, menu, shortcut and icon as text, enabled, checkable and checked. An
 * authored button runs its command with emit('studio.<id>') and reads it
 * here, as data-attr-data-tip="commands.play.label" does, and MenuCommand
 * runs emit('studio.' + command.id). Members follow the registry's order, so
 * showing walks them without a lookup. */

static const char *const COMMAND_TEXTS[] = { "label", "menu", "shortcut", "icon", NULL };
static const char *const COMMAND_FLAGS[] = { "enabled", "checkable", "checked", NULL };

void studio_commands_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *commands = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "commands");
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const char *id = studio_command_id((studio_command)c);
        yyjson_mut_val *command = yyjson_mut_obj_add_obj(doc, commands, id);
        yyjson_mut_obj_add_str(doc, command, "id", id);
        add_texts(doc, command, COMMAND_TEXTS);
        add_flags(doc, command, COMMAND_FLAGS);
    }
}

bool studio_commands_show(yyjson_mut_doc *doc, const studio_command_state *states)
{
    yyjson_mut_val *commands = member(yyjson_mut_doc_get_root(doc), "commands");
    yyjson_mut_obj_iter it = yyjson_mut_obj_iter_with(commands);
    bool changed = false;
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const studio_command_info *info = &states[c].info;
        yyjson_mut_val *command = yyjson_mut_obj_iter_get_val(
            yyjson_mut_obj_iter_next(&it)
        );
        char icon[5] = "";
        if (info->icon) {
            studio_glyph(icon, info->icon);
        }
        const char *texts[] = { info->label,
                                info->menu,
                                info->shortcut ? info->shortcut : "",
                                icon };
        for (int t = 0; COMMAND_TEXTS[t]; t++) {
            changed |= ui_data_put_text(doc, command, COMMAND_TEXTS[t], texts[t]);
        }
        changed |= ui_data_put_bool(command, "enabled", states[c].enabled);
        changed |= ui_data_put_bool(command, "checkable", states[c].checkable);
        changed |= ui_data_put_bool(command, "checked", states[c].checked);
    }
    return changed;
}

static bool sync_commands(studio *s, const studio_panel_sync *at)
{
    studio_command_state states[STUDIO_COMMAND_COUNT];
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        studio_command command = (studio_command)c;
        int checked = studio_command_checked(s, command);
        states[c] = (studio_command_state){
            .info = studio_command_describe(s, command),
            .enabled = studio_command_enabled(s, command),
            .checkable = checked >= 0,
            .checked = checked == 1,
        };
    }
    return studio_commands_show(at->doc, states);
}

/* ---------- Choices (workstream F3) ---------- */

/* `choices`, the tables choice fields and menus list, which rarely change.
 * A table is { label, icon, groups: [{ label, options: [{ value, label,
 * icon }] }] }:
 * - fields.<Component>.<field>: every value of a reflected enum or named
 *   choice field (component_properties.h), as EnumField lists them;
 * - assets.<kind>: a kind's (scene, prototype, behavior, event, material,
 *   shape, field, library, system, ui) label and icon, and none, the label
 *   of choosing none, as AssetField shows them; its picker lists the
 *   assets (studio_pick.c);
 * - editors: the editor types by task, as an area header's menu lists them,
 *   each option with drawer, whether a drawer may show it, and each group
 *   with tools, whether the Studio panels that are tools follow it;
 * - panels: { label, icon, tools, others }, the Studio panels, the others in
 *   the menu's Studio UI submenu. */

static yyjson_mut_val *add_table(
    yyjson_mut_doc *doc,
    yyjson_mut_val *obj,
    const char *key,
    const char *label,
    uint32_t icon
)
{
    yyjson_mut_val *table = yyjson_mut_obj_add_obj(doc, obj, key);
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    yyjson_mut_obj_add_strcpy(doc, table, "label", label);
    yyjson_mut_obj_add_strcpy(doc, table, "icon", glyph);
    yyjson_mut_obj_add_arr(doc, table, "groups");
    return table;
}

/* A group's options, the group made when `label` differs from the last. */
static yyjson_mut_val *group_options(yyjson_mut_doc *doc, yyjson_mut_val *table, const char *label)
{
    yyjson_mut_val *groups = member(table, "groups");
    size_t count = yyjson_mut_arr_size(groups);
    yyjson_mut_val *last = count ? yyjson_mut_arr_get(groups, count - 1) : NULL;
    if (last && !strcmp(text_at(last, "label"), label)) {
        return member(last, "options");
    }
    yyjson_mut_val *group = yyjson_mut_arr_add_obj(doc, groups);
    yyjson_mut_obj_add_strcpy(doc, group, "label", label);
    return yyjson_mut_obj_add_arr(doc, group, "options");
}

static void add_field_choices(yyjson_mut_doc *doc, yyjson_mut_val *fields, const ecs_component_desc *desc)
{
    yyjson_mut_val *component = NULL;
    for (int f = 0; desc->type && f < desc->type->field_count; f++) {
        const type_field *field = &desc->type->fields[f];
        const editor_choices *choices = property_choices(desc->type, field);
        if (!choices || !editor_choices_count(choices)) {
            continue;
        }
        component = component ? component
                              : yyjson_mut_obj_add_obj(doc, fields, desc->name);
        yyjson_mut_val *table = add_table(doc, component, field->name, field->name, 0);
        for (int i = 0; i < editor_choices_count(choices); i++) {
            editor_choice choice = editor_choice_at(choices, i);
            yyjson_mut_val *option = yyjson_mut_arr_add_obj(
                doc,
                group_options(doc, table, choice.group ? choice.group : "")
            );
            char glyph[5] = "";
            if (choice.icon) {
                studio_glyph(glyph, choice.icon);
            }
            yyjson_mut_obj_add_int(doc, option, "value", choice.value);
            yyjson_mut_obj_add_strcpy(doc, option, "label", choice.label);
            yyjson_mut_obj_add_strcpy(doc, option, "icon", glyph);
        }
    }
}

static void add_choices(yyjson_mut_doc *doc)
{
    yyjson_mut_val *choices = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "choices");
    yyjson_mut_val *fields = yyjson_mut_obj_add_obj(doc, choices, "fields");
    ecs_component_catalog project = project_components();
    for (int c = 0; c < project.component_count; c++) {
        add_field_choices(doc, fields, project.components[c]);
    }
    yyjson_mut_val *assets = yyjson_mut_obj_add_obj(doc, choices, "assets");
    for (int kind = 0; kind < REFERENCE_ASSET_KIND_COUNT; kind++) {
        yyjson_mut_val *table = add_table(
            doc,
            assets,
            studio_asset_kind_name(kind),
            studio_asset_kind_label(kind),
            studio_asset_kind_icon(kind)
        );
        yyjson_mut_obj_add_str(doc, table, "none", "None");
    }
    add_table(doc, choices, "editors", "", 0);
    yyjson_mut_val *panels = add_table(doc, choices, "panels", "Studio UI", KIT_ICON_LAYOUT);
    yyjson_mut_obj_add_arr(doc, panels, "tools");
    yyjson_mut_obj_add_arr(doc, panels, "others");
}

/* The editor types are fixed; the panels follow the Studio scene's. */
static bool sync_choices(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *editors = member(at->variable, "editors");
    yyjson_mut_val *panels = member(at->variable, "panels");
    size_t listed = yyjson_mut_arr_size(member(panels, "tools")) +
                    yyjson_mut_arr_size(member(panels, "others"));
    bool menu = !yyjson_mut_arr_size(member(editors, "groups")) ||
                (int)listed != studio_ui_panel_count(s);
    if (menu) {
        studio_editor_choices(s, at->doc, editors, panels);
    }
    return menu;
}

/* ---------- The table ---------- */

/* Each panel's source, in the order they sync. The chrome every area shows
 * (editor, commands, collaboration's status in the top bar) and the
 * Inspector, which reads nothing while hidden, sync at every refresh. */
const studio_panel_source studio_panel_sources[] = {
    { "inspector", NULL, add_inspector, sync_inspector, inspector_action },
    { "world", "Data", add_world, sync_world, NULL },
    { "theme", "Theme", add_theme, sync_theme, NULL },
    { "settings", "Settings", studio_settings_add, sync_settings, settings_action },
    { "shortcuts", "Shortcuts", studio_keymap_add, sync_shortcuts, NULL },
    { "editor", NULL, studio_status_add, sync_editor, editor_action },
    { "commands", NULL, studio_commands_add, sync_commands, NULL },
    { "collaboration", NULL, add_collaboration, sync_collaboration, NULL },
    { "new_project", "NewProject", add_new_project, sync_new_project, NULL },
    /* Popovers and choices (workstream F3). */
    { "choices", NULL, add_choices, sync_choices, NULL },
    /* Lists and drag (workstream F4). */
    { "gallery", "Widgets", studio_gallery_add, studio_gallery_sync, studio_gallery_action },
    /* Properties (workstream P1). */
    { "properties", "Properties", studio_properties_add, studio_properties_sync, studio_properties_action },
    /* Assets (workstream P3). */
    { "assets", "Assets", studio_assets_add, studio_assets_sync, studio_assets_action },
    { "asset_view", "Assets", studio_asset_view_add, studio_asset_view_sync, NULL },
    { "asset_menus", NULL, studio_asset_menus_add, studio_asset_menus_sync, studio_asset_menus_action },
    /* Output (workstream P4). */
    { "output", "Output", studio_output_add, studio_output_sync, studio_output_action },
    { "output_rows", "Output", studio_output_rows_add, studio_output_rows_sync, NULL },
    /* Members (workstream P5). */
    { "members", "Members", studio_members_add, studio_members_sync, studio_members_action },
    { "members_ui", "Members", studio_members_ui_add, studio_members_ui_sync, NULL },
    /* The shell (workstream C3). */
    { "menubar", "TopBar", studio_menubar_add, studio_menubar_sync, studio_menubar_action },
    { "shell", NULL, studio_shell_add, studio_shell_sync, studio_shell_action },
    { "workspace", "Workspace", studio_workspace_add, studio_workspace_sync, studio_workspace_action },
    /* The Scene area (workstream C1). */
    { "scene", "SceneHeader", studio_scene_add, studio_scene_sync, studio_scene_action },
    { "scene_view", "Scene", studio_scene_view_add, studio_scene_view_sync, NULL },
    { "canvas", "Canvas", studio_canvas_panel_add, studio_canvas_panel_sync, studio_canvas_panel_action },
    /* Editor chrome (workstream C2). */
    { "performance", "Performance", studio_performance_add, studio_performance_sync, NULL },
    { "graph_header",
      "BehaviorHeader",
      studio_graph_header_add,
      studio_graph_header_sync,
      studio_graph_header_action },
    { "graph_menus", "BehaviorHeader", studio_graph_menus_add, studio_graph_menus_sync, NULL },
    { "graph_view", "BehaviorHeader", studio_graph_view_add, studio_graph_view_sync, NULL },
    { "graph_targets", "BehaviorHeader", studio_graph_targets_add, studio_graph_targets_sync, NULL },
    { "graph_find", "GraphFind", studio_graph_find_add, studio_graph_find_sync, studio_graph_find_action },
    { "code_header", "CodeHeader", studio_code_header_add, studio_code_header_sync, studio_code_header_action },
    { "code_find", "CodeFind", studio_code_find_add, studio_code_find_sync, studio_code_find_action },
    /* The visual designer (workstream D1). */
    { "library", NULL, studio_library_add, studio_library_sync, studio_library_action },
    { "hierarchy", "Hierarchy", studio_hierarchy_add, studio_hierarchy_sync, studio_hierarchy_action },
    { "hierarchy_ui", "Hierarchy", studio_hierarchy_ui_add, studio_hierarchy_ui_sync, NULL },
    { "hierarchy_carry", "Hierarchy", studio_hierarchy_carry_add, studio_hierarchy_carry_sync, NULL },
    { "hierarchy_header",
      "HierarchyHeader",
      studio_hierarchy_header_add,
      studio_hierarchy_header_sync,
      NULL },
    { "details", "Details", studio_details_add, studio_details_sync, studio_details_action },
    { "details_add", "Details", studio_details_popover_add, studio_details_popover_sync, NULL },
    /* The palette and pickers (workstream C4); `palette_ui` with them. */
    { "palette", NULL, studio_palette_add_variables, studio_palette_sync, studio_palette_action },
    { "refactor", NULL, studio_refactor_add, studio_refactor_sync, studio_refactor_action },
    /* Graph chrome (workstream G1). */
    { "graph_menu", NULL, studio_graph_menu_add, studio_graph_menu_sync, studio_graph_menu_action },
    { "graph_details",
      "Properties",
      studio_graph_details_add,
      studio_graph_details_sync,
      studio_graph_details_action },
};
const int studio_panel_source_count = (int)(sizeof studio_panel_sources /
                                            sizeof *studio_panel_sources);
_Static_assert(
    sizeof studio_panel_sources / sizeof *studio_panel_sources <= STUDIO_PANEL_SOURCE_MAX,
    "studio_sources keeps a shown_at for each source"
);
