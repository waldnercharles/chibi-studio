/* Keyboard Shortcuts: a searchable sheet of the keys Studio takes, grouped
 * by the area they act in, as VS Code's Keyboard Shortcuts reference and
 * Figma's shortcut panel list theirs. Commands show their chords as Settings
 * ▸ Keyboard sets them (studio_commands.c); the keys editors handle
 * themselves come from the table below, which docs/studio-keys.md mirrors. */
#include "editor/studio/studio_internal.h"

#include <string.h>

/* Keys an editor takes itself, in studio_chord notation separated by
 * spaces; `mac` replaces them on macOS where the platforms differ. */
typedef struct
{
    const char *area, *label, *keys, *mac;
} fixed_key;

static const fixed_key FIXED[] = {
    /* studio_scene.c and workspace/scene_viewport.c: multi-selection */
    { "Scene", "Add an object to the selection, or take it out", "Shift+Click Mod+Click" },
    { "Scene", "Add a marquee's objects to the selection", "Shift+Drag Mod+Drag" },
    { "Outliner", "Add a row to the selection, or take it out", "Mod+Click" },
    { "Outliner", "Select the rows from the last clicked one", "Shift+Click" },
    /* studio_graph_input.c, studio_graph_find.c, studio_graph_nav.c */
    { "Behavior", "Add node", "Shift+A Tab" },
    { "Behavior", "Delete nodes", "X Backspace Delete" },
    { "Behavior", "Duplicate nodes", "Shift+D Mod+D" },
    { "Behavior", "Select all nodes", "Mod+A" },
    { "Behavior", "Copy nodes", "Mod+C" },
    { "Behavior", "Cut nodes", "Mod+X" },
    { "Behavior", "Paste nodes at the pointer", "Mod+V" },
    { "Behavior", "Frame selection, or all", "F" },
    { "Behavior", "Frame all", "Home" },
    { "Behavior", "Comment frame around the selection", "C" },
    { "Behavior", "Straighten", "Q" },
    { "Behavior", "Arrange", "Shift+L" },
    { "Behavior", "Select linked inputs", "L" },
    { "Behavior", "Select linked outputs", "Alt+L" },
    { "Behavior", "Collapse to function", "Mod+G" },
    { "Behavior", "Go to definition", "Alt+G" },
    { "Behavior", "Find references", "Shift+Alt+F" },
    { "Behavior", "Call hierarchy", "Shift+Alt+H" },
    { "Behavior", "Rename the pin or event under the pointer", "F2" },
    { "Behavior", "Toggle breakpoint", "F9" },
    { "Behavior", "Back and forward", "Alt+Left Alt+Right" },
    { "Behavior", "Pan while held", "Space" },
    { "Behavior",
      "Place Branch, Sequence, Delay, For Each, Do N, Do Once or Gate: hold "
      "and click",
      "B S D F N O G" },
    { "Behavior", "Snap while moving", "Ctrl" },
    { "Behavior", "Cancel the drag", "Escape" },
    { "Behavior", "Find: next and previous result", "Down Up" },
    { "Behavior", "Find: go to the result", "Return" },
    /* studio_members.c */
    { "Members", "Rename", "F2" },
    { "Members", "Delete", "X Backspace Delete" },
    { "Members", "Duplicate variable", "Shift+D Mod+D" },
    { "Members", "Drag in as a Set node", "Alt" },
    /* kit_code.c, studio_code.c */
    { "Text Editor", "Go to definition", "F12" },
    { "Text Editor", "Find references", "Shift+F12" },
    { "Text Editor", "Suggest completions", "Ctrl+Space" },
    { "Text Editor", "Back and forward", "Mod+[ Mod+]" },
    { "Text Editor", "Start and end of the text", "Ctrl+Home Ctrl+End", "Mod+Up Mod+Down" },
    { "Text Editor", "Find: next result", "Return Down" },
    { "Text Editor", "Find: previous result", "Shift+Return Up" },
    { "Text Editor", "Find: back to the text", "Escape" },
    /* studio_assets.c */
    { "Assets", "Rename", "F2" },
    { "Assets", "Delete", "X Backspace Delete" },
    /* studio_designer.c: the Outliner and Scene showing a UI document */
    { "UI Hierarchy", "Select previous and next", "Up Down" },
    { "UI Hierarchy", "Fold and unfold", "Left Right" },
    { "UI Hierarchy", "Pick up to move", "Space Return" },
    { "UI Hierarchy", "Move the picked-up element", "Up Down" },
    { "UI Hierarchy", "Drop it", "Space Return Tab" },
    { "UI Hierarchy", "Cancel the move", "Escape" },
    { "UI Hierarchy", "Duplicate", "Shift+D Mod+D" },
    { "UI Hierarchy", "Delete", "X Backspace Delete" },
    /* studio_inspect.c */
    { "Inspector", "Stop picking", "Escape" },
    /* menus and popovers (ui/popover.c), the command palette and fields */
    { "Menus and Search", "Move", "Up Down" },
    { "Menus and Search", "First and last", "Home End" },
    { "Menus and Search", "Choose", "Return Space" },
    { "Menus and Search", "Close", "Escape" },
    { "Menus and Search",
      "Open or close a submenu; the menubar's next or previous menu",
      "Right Left" },
    { "Menus and Search", "Jump to an item by the first letters of its label", "A-Z" },
    { "Menus and Search", "Move through a popover, or close a menu", "Tab" },
    { "Menus and Search", "Commands, symbols and help in Search", "> @ ?" },
    { "Fields", "Commit", "Return Tab" },
    { "Fields", "Cancel", "Escape" },
    { "Fields", "Step a number", "Up Down" },
    { "Fields", "Coarse step", "PageUp PageDown" },
    { "Fields", "Minimum and maximum", "Home End" },
    { "Fields", "Fine, coarse and snapped steps", "Alt Shift Ctrl" },
};

/* The areas, in the order the sheet lists them; studio_command_group names
   the commands'. */
static const char *const AREAS[] = {
    "Global",    "Scene, Outliner",  "Scene, Properties",
    "Scene",     "Outliner",         "Properties",
    "Behavior",  "Members",          "Text Editor",
    "Assets",    "UI Hierarchy",     "Debugger",
    "Inspector", "Menus and Search", "Fields",
};

void studio_keymap_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *sheet = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "shortcuts");
    yyjson_mut_obj_add_str(doc, sheet, "query", "");
    yyjson_mut_obj_add_int(doc, sheet, "matches", 0);
    yyjson_mut_obj_add_arr(doc, sheet, "groups");
}

bool studio_keymap_area(const char *area)
{
    for (size_t i = 0; i < sizeof AREAS / sizeof *AREAS; i++) {
        if (!strcmp(AREAS[i], area)) {
            return true;
        }
    }
    return false;
}

/* Adds a row of `count` chords to `rows` when the query's words are in its
   area, label or keys; true then. */
static bool add_row(
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    const char *area,
    const char *label,
    const char *const *chords,
    int count,
    bool mac,
    const char *query
)
{
    char shown[8][STUDIO_CHORD_SIZE], text[512];
    size_t used = (size_t)snprintf(text, sizeof text, "%s %s", area, label);
    for (int i = 0; i < count && used < sizeof text; i++) {
        studio_chord_format(shown[i], sizeof shown[i], chords[i], mac);
        used += (size_t)
            snprintf(text + used, sizeof text - used, " %s %s", chords[i], shown[i]);
    }
    if (!studio_settings_match(query, text)) {
        return false;
    }
    yyjson_mut_val *row = yyjson_mut_arr_add_obj(doc, rows);
    yyjson_mut_obj_add_strcpy(doc, row, "label", label);
    yyjson_mut_val *keys = yyjson_mut_obj_add_arr(doc, row, "keys");
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_strcpy(doc, keys, shown[i]);
    }
    return true;
}

/* The fixed row's chords, split at spaces into `out`. */
static int fixed_chords(const fixed_key *key, bool mac, char out[8][STUDIO_CHORD_SIZE], const char **chords)
{
    const char *keys = mac && key->mac ? key->mac : key->keys;
    int count = 0;
    for (const char *at = keys; *at && count < 8;) {
        size_t length = strcspn(at, " ");
        snprintf(out[count], STUDIO_CHORD_SIZE, "%.*s", (int)length, at);
        chords[count] = out[count];
        count++;
        at += length + (at[length] == ' ');
    }
    return count;
}

/* The rows of `area` that match the query; their count. */
static int add_area(
    yyjson_mut_doc *doc,
    yyjson_mut_val *rows,
    const char *area,
    const studio_prefs *prefs,
    bool mac,
    const char *query
)
{
    int shown = 0;
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        const char *chords[8];
        int count = studio_command_bindings(prefs, (studio_command)c, chords, 3);
        if (count && !strcmp(studio_command_group((studio_command)c), area)) {
            shown += add_row(doc, rows, area, studio_command_label((studio_command)c), chords, count, mac, query);
        }
    }
    for (size_t i = 0; i < sizeof FIXED / sizeof *FIXED; i++) {
        char split[8][STUDIO_CHORD_SIZE];
        const char *chords[8];
        if (!strcmp(FIXED[i].area, area)) {
            int count = fixed_chords(&FIXED[i], mac, split, chords);
            shown += add_row(doc, rows, area, FIXED[i].label, chords, count, mac, query);
        }
    }
    return shown;
}

bool studio_keymap_show(yyjson_mut_doc *doc, const studio_prefs *prefs, bool mac, studio_keymap_cache *cache)
{
    yyjson_mut_val *sheet = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "shortcuts");
    const char *query = yyjson_mut_get_str(yyjson_mut_obj_get(sheet, "query"));
    if (cache->valid && cache->doc == doc && cache->mac == mac &&
        !strcmp(cache->query, query) &&
        !memcmp(cache->shortcuts, prefs->shortcuts, sizeof cache->shortcuts)) {
        return false;
    }
    cache->valid = true;
    cache->doc = doc;
    cache->mac = mac;
    snprintf(cache->query, sizeof cache->query, "%s", query);
    memcpy(cache->shortcuts, prefs->shortcuts, sizeof cache->shortcuts);
    yyjson_mut_val *groups = yyjson_mut_obj_get(sheet, "groups");
    yyjson_mut_arr_clear(groups);
    int matches = 0;
    for (size_t a = 0; a < sizeof AREAS / sizeof *AREAS; a++) {
        yyjson_mut_val *group = yyjson_mut_arr_add_obj(doc, groups);
        yyjson_mut_obj_add_str(doc, group, "label", AREAS[a]);
        yyjson_mut_val *rows = yyjson_mut_obj_add_arr(doc, group, "rows");
        int shown = add_area(doc, rows, AREAS[a], prefs, mac, cache->query);
        if (!shown) {
            yyjson_mut_arr_remove_last(groups);
        }
        matches += shown;
    }
    yyjson_mut_set_int(yyjson_mut_obj_get(sheet, "matches"), matches);
    return true;
}
