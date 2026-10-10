/* Studio preferences: one plain store persisted as a whole (zustand's persist
 * middleware). Reading keeps defaults for missing or invalid fields, so older
 * files migrate without a version switch. Desktop writes a JSON file in the
 * user directory; the browser uses localStorage. Settings go through the
 * settings table (studio_settings.c); the area layout is kept here. */
#include "editor/studio/studio_internal.h"
#include "platform/cute.h"
#include "support/array.h"
#include "support/text_file.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(char *, prefs_read_browser, (), {
    try {
        const value = localStorage.getItem("chibi.studio");
        return value ? stringToNewUTF8(value) : 0;
}
catch(_)
{
    return 0;
}
})
EM_JS(bool, prefs_write_browser, (const char *json), {
    try {
        localStorage.setItem("chibi.studio", UTF8ToString(json));
        return true;
}
catch(_)
{
    return false;
}
})
EM_JS(bool, prefs_browser_reduced_motion, (), {
    try {
        return matchMedia("(prefers-reduced-motion: reduce)").matches;
}
catch(_)
{
    return false;
}
})
#endif

#define PREFS_VERSION 2
/* Before Assets had a view setting, its smallest tile size listed them. */
#define PREVIOUS_ASSETS_LIST 40

/* An area showing a panel saves as {"panel": "<name>"}. */
static const char *const EDITORS[STUDIO_EDITOR_COUNT] = {
    "scene",  "behavior",    "outliner", "properties", "output",
    "assets", "performance", "members",  "code",       "panel"
};

static const char *const WORKSPACES[STUDIO_WORKSPACE_COUNT] = { "verticalSplit", "horizontalSplit" };
static const char *const TOOLS[SCENE_TOOL_COUNT] = { "select", "move", "rotate", "scale" };
static const char *const PREVIOUS_WORKSPACES[STUDIO_WORKSPACE_COUNT] = {
    "layout",
    "ui"
};

/* Vertical Split stacks Scene over Behavior; Horizontal Split sets the Canvas
   beside the Text Editor, half and half. */
studio_layout studio_layout_default(studio_workspace workspace)
{
    assert(workspace >= 0 && workspace < STUDIO_WORKSPACE_COUNT);
    bool horizontal = workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    return (studio_layout){
        .side_open = true,
        .side_width = horizontal ? 320 : 372,
        .main_split = horizontal ? 0.5f : 0.62f,
        .side_split = 0.38f,
        .drawer_height = 240,
        .canvas_actions_open = false,
        .editors = { STUDIO_EDITOR_SCENE,
                     horizontal ? STUDIO_EDITOR_CODE : STUDIO_EDITOR_BEHAVIOR,
                     STUDIO_EDITOR_OUTLINER,
                     STUDIO_EDITOR_PROPERTIES,
                     STUDIO_EDITOR_OUTPUT },
    };
}

studio_layout *studio_active_layout(studio *s)
{
    return &s->prefs.layouts[s->prefs.workspace];
}

studio_prefs studio_prefs_default()
{
    studio_prefs p = { 0 };
    p.workspace = STUDIO_WORKSPACE_VERTICAL_SPLIT;
    for (int w = 0; w < STUDIO_WORKSPACE_COUNT; w++) {
        p.layouts[w] = studio_layout_default(w);
    }
    for (int i = 0; i < studio_setting_count; i++) {
        studio_setting_set(&p, &studio_settings[i], studio_settings[i].fallback);
    }
    p.assets_tile = STUDIO_ASSETS_TILE;
#ifdef __EMSCRIPTEN__
    p.reduced_motion = prefs_browser_reduced_motion();
#endif
    return p;
}

bool studio_prefs_equal(const studio_prefs *a, const studio_prefs *b)
{
    for (int i = 0; i < studio_setting_count; i++) {
        const studio_setting *s = &studio_settings[i];
        if (studio_setting_get(a, s) != studio_setting_get(b, s)) {
            return false;
        }
    }
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        if (strcmp(a->shortcuts[c], b->shortcuts[c])) {
            return false;
        }
    }
    for (int i = 0; i < STUDIO_RECENT_COUNT; i++) {
        if (strcmp(a->recent[i].key, b->recent[i].key) ||
            a->recent[i].uses != b->recent[i].uses) {
            return false;
        }
    }
    for (int w = 0; w < STUDIO_WORKSPACE_COUNT; w++) {
        const studio_layout *x = &a->layouts[w], *y = &b->layouts[w];
        for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
            if (x->editors[i] != y->editors[i] || strcmp(x->panels[i], y->panels[i])) {
                return false;
            }
        }
        if (x->side_open != y->side_open || x->side_width != y->side_width ||
            x->main_split != y->main_split || x->side_split != y->side_split ||
            x->drawer_open != y->drawer_open || x->drawer_height != y->drawer_height ||
            x->canvas_actions_open != y->canvas_actions_open) {
            return false;
        }
    }
    return a->workspace == b->workspace && a->tool == b->tool &&
           a->assets_tile == b->assets_tile && a->assets_list == b->assets_list;
}

static int name_index(const char *const *names, int count, yyjson_val *value, int fallback)
{
    const char *name = yyjson_get_str(value);
    for (int i = 0; name && i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return fallback;
}

static int workspace_index(yyjson_val *value, int fallback)
{
    const char *name = yyjson_get_str(value);
    for (int i = 0; name && i < STUDIO_WORKSPACE_COUNT; i++) {
        if (!strcmp(name, WORKSPACES[i]) || !strcmp(name, PREVIOUS_WORKSPACES[i])) {
            return i;
        }
    }
    return fallback;
}

static float number(yyjson_val *value, float fallback, float minimum, float maximum)
{
    if (!yyjson_is_num(value)) {
        return fallback;
    }
    double v = yyjson_get_num(value);
    return v >= minimum && v <= maximum ? (float)v : fallback;
}

/* The value at a dotted path ("code.vim") under `root`, or NULL. */
static yyjson_val *value_at(yyjson_val *root, const char *path)
{
    for (const char *at = path;; at++) {
        size_t length = strcspn(at, ".");
        root = yyjson_obj_getn(root, at, length);
        at += length;
        if (!*at) {
            return root;
        }
    }
}

/* Keeps the default for a missing value, or one of the wrong type or range. */
static void read_setting(studio_prefs *p, const studio_setting *s, yyjson_val *root)
{
    yyjson_val *value = value_at(root, s->key);
    if (s->type == STUDIO_SETTING_NUMBER && yyjson_is_num(value)) {
        char text[32];
        snprintf(text, sizeof text, "%.9g", yyjson_get_num(value));
        studio_setting_parse(p, s, text);
        return;
    }
    if (s->type == STUDIO_SETTING_BOOL && yyjson_is_bool(value)) {
        studio_setting_set(p, s, yyjson_get_bool(value));
        return;
    }
    if (s->type == STUDIO_SETTING_CHOICE && yyjson_is_str(value)) {
        studio_setting_parse(p, s, yyjson_get_str(value));
    }
}

#ifndef __EMSCRIPTEN__
/* Honor the direct file override used by Studio integrations and the
   directory override used by tests, keeping user preferences isolated. */
static bool prefs_path(char *path, size_t size)
{
    const char *override = getenv("CHIBI_STUDIO_PREFS_PATH");
    if (override && *override) {
        return snprintf(path, size, "%s", override) < (int)size;
    }
    const char *directory = getenv("CHIBI_PREFS_DIR");
    if (!directory || !directory[0]) {
        directory = cf_fs_get_user_directory("chibi", "editor");
    }
    if (!directory) {
        return false;
    }
    size_t length = strlen(directory);
    const char *slash = length && directory[length - 1] != '/' ? "/" : "";
    return snprintf(path, size, "%s%sstudio.json", directory, slash) < (int)size;
}
#endif

static char *read_json()
{
#ifdef __EMSCRIPTEN__
    return prefs_read_browser();
#else
    char path[1024];
    return prefs_path(path, sizeof path) ? text_file_read(path) : NULL;
#endif
}

static void read_layout(studio_layout *l, yyjson_val *layout)
{
    yyjson_val *side_open = yyjson_obj_get(layout, "sideOpen");
    if (yyjson_is_bool(side_open)) {
        l->side_open = yyjson_get_bool(side_open);
    }
    l->side_width = number(yyjson_obj_get(layout, "sideWidth"), l->side_width, 200, 1600);
    l->main_split = number(yyjson_obj_get(layout, "mainSplit"), l->main_split, 0.05f, 0.95f);
    l->side_split = number(yyjson_obj_get(layout, "sideSplit"), l->side_split, 0.05f, 0.95f);
    yyjson_val *drawer = yyjson_obj_get(layout, "drawer");
    yyjson_val *open = yyjson_obj_get(drawer, "open");
    if (yyjson_is_bool(open)) {
        l->drawer_open = yyjson_get_bool(open);
    }
    l->drawer_height = number(yyjson_obj_get(drawer, "height"), l->drawer_height, 80, 2000);
    yyjson_val *canvas_actions = yyjson_obj_get(layout, "canvasActionsOpen");
    if (yyjson_is_bool(canvas_actions)) {
        l->canvas_actions_open = yyjson_get_bool(canvas_actions);
    }
    yyjson_val *areas = yyjson_obj_get(layout, "areas");
    for (int i = 0; i < STUDIO_AREA_COUNT && i < (int)yyjson_arr_size(areas); i++) {
        yyjson_val *area = yyjson_arr_get(areas, (size_t)i);
        const char *panel = yyjson_get_str(yyjson_obj_get(area, "panel"));
        if (panel && panel[0] && strlen(panel) < sizeof l->panels[i]) {
            l->editors[i] = STUDIO_EDITOR_PANEL;
            snprintf(l->panels[i], sizeof l->panels[i], "%s", panel);
            continue;
        }
        int editor = name_index(EDITORS, STUDIO_EDITOR_COUNT, area, l->editors[i]);
        l->editors[i] = editor == STUDIO_EDITOR_PANEL ? l->editors[i]
                                                      : (studio_editor)editor;
    }
}

studio_prefs studio_prefs_read(const char *json)
{
    studio_prefs p = studio_prefs_default();
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return p;
    }
    for (int i = 0; i < studio_setting_count; i++) {
        read_setting(&p, &studio_settings[i], root);
    }
    yyjson_val *shortcuts = yyjson_obj_get(root, "shortcuts");
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        yyjson_val *saved = yyjson_obj_get(shortcuts, studio_command_id(c));
        if (!saved && c == STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT) {
            saved = yyjson_obj_get(shortcuts, "workspaceLayout");
        } else if (!saved && c == STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT) {
            saved = yyjson_obj_get(shortcuts, "workspaceUi");
        }
        const char *chord = yyjson_get_str(saved);
        if (studio_chord_valid(chord) &&
            strcmp(chord, studio_command_default_chord(c))) {
            snprintf(p.shortcuts[c], sizeof p.shortcuts[c], "%s", chord);
        }
    }
    yyjson_val *recent = yyjson_obj_get(yyjson_obj_get(root, "palette"), "recent");
    for (int i = 0, n = 0; n < STUDIO_RECENT_COUNT && i < (int)yyjson_arr_size(recent);
         i++) {
        yyjson_val *item = yyjson_arr_get(recent, (size_t)i);
        const char *key = yyjson_get_str(yyjson_obj_get(item, "key"));
        yyjson_val *uses = yyjson_obj_get(item, "uses");
        if (key && key[0] && strlen(key) < sizeof p.recent[n].key &&
            yyjson_is_int(uses) && yyjson_get_int(uses) > 0) {
            snprintf(p.recent[n].key, sizeof p.recent[n].key, "%s", key);
            p.recent[n++].uses = yyjson_get_int(uses);
        }
    }
    p.tool = name_index(TOOLS, SCENE_TOOL_COUNT, value_at(root, "scene.tool"), SCENE_TOOL_SELECT);
    yyjson_val *assets = yyjson_obj_get(root, "assets");
    yyjson_val *tile = yyjson_obj_get(assets, "tileSize");
    p.assets_tile = (int)number(tile, (float)p.assets_tile, STUDIO_ASSETS_TILE_MIN, STUDIO_ASSETS_TILE_MAX);
    yyjson_val *view = yyjson_obj_get(assets, "view");
    p.assets_list = view ? yyjson_is_str(view) && !strcmp(yyjson_get_str(view), "list")
                         : yyjson_is_int(tile) &&
                               yyjson_get_int(tile) == PREVIOUS_ASSETS_LIST;
    yyjson_val *layout = yyjson_obj_get(root, "layout");
    p.workspace = workspace_index(yyjson_obj_get(layout, "workspace"), STUDIO_WORKSPACE_VERTICAL_SPLIT);
    yyjson_val *presets = yyjson_obj_get(layout, "presets");
    for (int w = 0; w < STUDIO_WORKSPACE_COUNT; w++) {
        yyjson_val *preset = yyjson_obj_get(presets, WORKSPACES[w]);
        if (!preset) {
            preset = yyjson_obj_get(presets, PREVIOUS_WORKSPACES[w]);
        }
        read_layout(&p.layouts[w], preset);
    }
    /* A file from before presets holds one arrangement: Vertical Split's. */
    if (!presets) {
        read_layout(&p.layouts[STUDIO_WORKSPACE_VERTICAL_SPLIT], layout);
    }
    yyjson_doc_free(doc);
    return p;
}

studio_prefs studio_prefs_load()
{
    char *json = read_json();
    studio_prefs p = studio_prefs_read(json);
    free(json);
    return p;
}

/* The object holding the member at a dotted path ("code.vim"), made as
 * needed; *name becomes the member's name. */
static yyjson_mut_val *object_for(yyjson_mut_doc *doc, yyjson_mut_val *root, const char **name)
{
    for (const char *dot; (dot = strchr(*name, '.')); *name = dot + 1) {
        size_t length = (size_t)(dot - *name);
        yyjson_mut_val *child = yyjson_mut_obj_getn(root, *name, length);
        if (!child) {
            child = yyjson_mut_obj(doc);
            yyjson_mut_obj_add(root, yyjson_mut_strncpy(doc, *name, length), child);
        }
        root = child;
    }
    return root;
}

static void write_setting(
    const studio_prefs *p,
    const studio_setting *s,
    yyjson_mut_doc *doc,
    yyjson_mut_val *root
)
{
    const char *name = s->key;
    yyjson_mut_val *object = object_for(doc, root, &name);
    float value = studio_setting_get(p, s);
    switch (s->type) {
        case STUDIO_SETTING_BOOL:
            yyjson_mut_obj_add_bool(doc, object, name, value != 0);
            return;
        case STUDIO_SETTING_CHOICE:
            yyjson_mut_obj_add_str(doc, object, name, s->choices[(int)value].value);
            return;
        case STUDIO_SETTING_NUMBER:
            yyjson_mut_obj_add_real(doc, object, name, value);
            return;
    }
}

/* Keeps unknown saved preferences, except the retired tab-follow setting and
   workspace aliases. In `shortcuts`, a command this build knows without an
   override has its default chord, so its old override goes. */
static void keep_unknown(
    yyjson_mut_doc *doc,
    yyjson_mut_val *into,
    yyjson_val *saved,
    bool shortcuts,
    bool workspace_presets,
    bool root
)
{
    size_t i, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(saved, i, max, key, value)
    {
        const char *name = yyjson_get_str(key);
        if ((root && !strcmp(name, "followDocuments")) ||
            (shortcuts &&
             (!strcmp(name, "workspaceLayout") || !strcmp(name, "workspaceUi"))) ||
            (workspace_presets && (!strcmp(name, "layout") || !strcmp(name, "ui")))) {
            continue;
        }
        yyjson_mut_val *known = yyjson_mut_obj_get(into, name);
        if (!known && !(shortcuts && studio_command_named(name) >= 0)) {
            yyjson_mut_obj_add(into, yyjson_val_mut_copy(doc, key), yyjson_val_mut_copy(doc, value));
        } else if (yyjson_mut_is_obj(known) && yyjson_is_obj(value)) {
            keep_unknown(doc, known, value, !strcmp(name, "shortcuts"), !strcmp(name, "presets"), false);
        }
    }
}

static void write_layout(const studio_layout *l, yyjson_mut_doc *doc, yyjson_mut_val *layout)
{
    yyjson_mut_obj_add_bool(doc, layout, "sideOpen", l->side_open);
    yyjson_mut_obj_add_real(doc, layout, "sideWidth", l->side_width);
    yyjson_mut_obj_add_real(doc, layout, "mainSplit", l->main_split);
    yyjson_mut_obj_add_real(doc, layout, "sideSplit", l->side_split);
    yyjson_mut_val *drawer = yyjson_mut_obj_add_obj(doc, layout, "drawer");
    yyjson_mut_obj_add_bool(doc, drawer, "open", l->drawer_open);
    yyjson_mut_obj_add_real(doc, drawer, "height", l->drawer_height);
    yyjson_mut_obj_add_bool(doc, layout, "canvasActionsOpen", l->canvas_actions_open);
    yyjson_mut_val *areas = yyjson_mut_obj_add_arr(doc, layout, "areas");
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        if (l->editors[i] == STUDIO_EDITOR_PANEL) {
            yyjson_mut_obj_add_str(doc, yyjson_mut_arr_add_obj(doc, areas), "panel", l->panels[i]);
            continue;
        }
        yyjson_mut_arr_add_str(doc, areas, EDITORS[l->editors[i]]);
    }
}

char *studio_prefs_write(const studio_prefs *p, const char *saved)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", PREFS_VERSION);
    for (int i = 0; i < studio_setting_count; i++) {
        write_setting(p, &studio_settings[i], doc, root);
    }
    const char *tool = "scene.tool";
    yyjson_mut_obj_add_str(doc, object_for(doc, root, &tool), tool, TOOLS[p->tool]);
    yyjson_mut_val *shortcuts = yyjson_mut_obj_add_obj(doc, root, "shortcuts");
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        if (p->shortcuts[c][0]) {
            yyjson_mut_obj_add_str(doc, shortcuts, studio_command_id(c), p->shortcuts[c]);
        }
    }
    yyjson_mut_val *palette = yyjson_mut_obj_add_obj(doc, root, "palette");
    yyjson_mut_val *recent = yyjson_mut_obj_add_arr(doc, palette, "recent");
    for (int i = 0; i < STUDIO_RECENT_COUNT && p->recent[i].key[0]; i++) {
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, recent);
        yyjson_mut_obj_add_strcpy(doc, item, "key", p->recent[i].key);
        yyjson_mut_obj_add_int(doc, item, "uses", p->recent[i].uses);
    }
    yyjson_mut_val *assets = yyjson_mut_obj_add_obj(doc, root, "assets");
    yyjson_mut_obj_add_int(doc, assets, "tileSize", p->assets_tile);
    yyjson_mut_obj_add_str(doc, assets, "view", p->assets_list ? "list" : "grid");
    yyjson_mut_val *layout = yyjson_mut_obj_add_obj(doc, root, "layout");
    yyjson_mut_obj_add_str(doc, layout, "workspace", WORKSPACES[p->workspace]);
    yyjson_mut_val *presets = yyjson_mut_obj_add_obj(doc, layout, "presets");
    for (int w = 0; w < STUDIO_WORKSPACE_COUNT; w++) {
        write_layout(&p->layouts[w], doc, yyjson_mut_obj_add_obj(doc, presets, WORKSPACES[w]));
    }
    yyjson_doc *previous = saved ? yyjson_read(saved, strlen(saved), 0) : NULL;
    if (yyjson_is_obj(yyjson_doc_get_root(previous))) {
        keep_unknown(doc, root, yyjson_doc_get_root(previous), false, false, true);
    }
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_doc_free(previous);
    yyjson_mut_doc_free(doc);
    assert(json);
    return json;
}

static bool write_json(const char *json)
{
#ifdef __EMSCRIPTEN__
    return prefs_write_browser(json);
#else
    char path[1024];
    FILE *file = prefs_path(path, sizeof path) ? fopen(path, "wb") : NULL;
    bool ok = file && fputs(json, file) >= 0;
    if (file) {
        ok &= fclose(file) == 0;
    }
    return ok;
#endif
}

bool studio_prefs_save(const studio_prefs *p)
{
    char *saved = read_json();
    char *json = studio_prefs_write(p, saved);
    free(saved);
    bool ok = write_json(json);
    free(json);
    return ok;
}

#ifndef __EMSCRIPTEN__
#define STUDIO_RECENT_PROJECTS 10

static yyjson_doc *read_doc()
{
    char *json = read_json();
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    free(json);
    return doc;
}

/* The roots under "recentProjects", which studio_prefs_write keeps as it
   keeps any member it does not know. */
static char **recent_in(yyjson_doc *doc)
{
    char **roots = NULL;
    size_t i, count;
    yyjson_val *root;
    yyjson_val *recent = yyjson_obj_get(yyjson_doc_get_root(doc), "recentProjects");
    yyjson_arr_foreach(recent, i, count, root)
    {
        if (yyjson_is_str(root) && acount(roots) < STUDIO_RECENT_PROJECTS) {
            apush(roots, xstrdup(yyjson_get_str(root)));
        }
    }
    return roots;
}

char **studio_recent_projects()
{
    yyjson_doc *doc = read_doc();
    char **roots = recent_in(doc);
    yyjson_doc_free(doc);
    return roots;
}

void studio_recent_projects_free(char **roots)
{
    for (int i = 0; i < acount(roots); i++) {
        free(roots[i]);
    }
    afree(roots);
}

void studio_remember_project(const char *root)
{
    yyjson_doc *saved = read_doc();
    char **roots = recent_in(saved);
    yyjson_mut_doc *doc = yyjson_is_obj(yyjson_doc_get_root(saved))
                            ? yyjson_doc_mut_copy(saved, NULL)
                            : yyjson_mut_doc_new(NULL);
    yyjson_doc_free(saved);
    yyjson_mut_val *object = yyjson_mut_doc_get_root(doc);
    if (!object) {
        object = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, object);
    }
    yyjson_mut_obj_remove_key(object, "recentProjects");
    yyjson_mut_val *recent = yyjson_mut_obj_add_arr(doc, object, "recentProjects");
    yyjson_mut_arr_add_strcpy(doc, recent, root);
    for (int i = 0;
         i < acount(roots) && yyjson_mut_arr_size(recent) < STUDIO_RECENT_PROJECTS;
         i++) {
        if (strcmp(roots[i], root)) {
            yyjson_mut_arr_add_strcpy(doc, recent, roots[i]);
        }
    }
    studio_recent_projects_free(roots);
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(doc);
    assert(json);
    write_json(json);
    free(json);
}
#endif

bool studio_prefs_open_scene_view(game_save *store)
{
#ifdef __EMSCRIPTEN__
    return game_save_open(store, "chibi.sceneView");
#else
    const char *directory = cf_fs_get_user_directory("chibi", "editor");
    char path[1024];
    return directory &&
           snprintf(path, sizeof path, "%sscene-view.json", directory) <
               (int)sizeof path &&
           game_save_open(store, path);
#endif
}

/* ---------- Shared with authored panels ---------- */

void studio_prefs_share(studio_prefs *p, game_save *save, uint64_t *seen)
{
    if (game_save_revision(save) != *seen) {
        for (int i = 0; i < studio_setting_count; i++) {
            const char *text = game_save_get(save, studio_settings[i].save);
            if (text) {
                studio_setting_parse(p, &studio_settings[i], text);
            }
        }
    }
    for (int i = 0; i < studio_setting_count; i++) {
        char text[32];
        studio_setting_text(p, &studio_settings[i], text, sizeof text);
        game_save_set(save, studio_settings[i].save, text);
    }
    *seen = game_save_revision(save);
}
