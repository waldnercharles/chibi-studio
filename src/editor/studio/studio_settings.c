/* The editor's preferences as one table, as VS Code's settings registry: each
 * row says where a studio_prefs field lives in the preferences file and in
 * the saved keys authored panels bind, and how the Settings page shows it.
 * The area layout is not a setting; studio_prefs.c keeps it by hand. */
#include "editor/studio/studio_internal.h"
#include "ui/rml.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const studio_settings_category_info studio_settings_categories[STUDIO_SETTINGS_CATEGORY_COUNT] = {
    [STUDIO_SETTINGS_APPEARANCE] = { "appearance", "Appearance", KIT_ICON_MATERIAL, false },
    [STUDIO_SETTINGS_SCENE] = { "scene", "Scene", KIT_ICON_VIEWPORT, false },
    [STUDIO_SETTINGS_GRAPH] = { "graph", "Graph", KIT_ICON_GRAPH, false },
    [STUDIO_SETTINGS_TEXT] = { "text", "Text Editor", KIT_ICON_EXPRESSION, false },
    [STUDIO_SETTINGS_KEYBOARD] = { "keyboard", "Keyboard", KIT_ICON_KEYBOARD, false },
    [STUDIO_SETTINGS_PROJECT] = { "project", "General", KIT_ICON_SETTINGS, true },
};

_Static_assert(sizeof(studio_theme) == sizeof(int), "choices are int fields");
_Static_assert(sizeof(kit_wire_style) == sizeof(int), "choices are int fields");

static const studio_setting_choice THEMES[STUDIO_THEME_COUNT] = {
    [STUDIO_THEME_SYSTEM] = { "system", "System" },
    [STUDIO_THEME_DARK] = { "dark", "Dark" },
    [STUDIO_THEME_LIGHT] = { "light", "Light" },
    [STUDIO_THEME_DRACULA] = { "dracula", "Dracula" },
    [STUDIO_THEME_SOLARIZED_LIGHT] = { "solarized-light", "Solarized Light" },
    [STUDIO_THEME_SOLARIZED_DARK] = { "solarized-dark", "Solarized Dark" },
    [STUDIO_THEME_TOKYO_NIGHT] = { "tokyo-night", "Tokyo Night" },
    [STUDIO_THEME_TOKYO_STORM] = { "tokyo-storm", "Tokyo Night Storm" },
    [STUDIO_THEME_TOKYO_DAY] = { "tokyo-day", "Tokyo Night Day" },
    [STUDIO_THEME_CATPPUCCIN_LATTE] = { "catppuccin-latte", "Catppuccin Latte" },
    [STUDIO_THEME_CATPPUCCIN_FRAPPE] = { "catppuccin-frappe", "Catppuccin Frappé" },
    [STUDIO_THEME_CATPPUCCIN_MACCHIATO] = { "catppuccin-macchiato", "Catppuccin Macchiato" },
    [STUDIO_THEME_CATPPUCCIN_MOCHA] = { "catppuccin-mocha", "Catppuccin Mocha" },
    [STUDIO_THEME_ROSE_PINE] = { "rose-pine", "Rosé Pine" },
    [STUDIO_THEME_ROSE_PINE_MOON] = { "rose-pine-moon", "Rosé Pine Moon" },
    [STUDIO_THEME_ROSE_PINE_DAWN] = { "rose-pine-dawn", "Rosé Pine Dawn" },
    [STUDIO_THEME_GRUVBOX_LIGHT] = { "gruvbox-light", "Gruvbox Light" },
    [STUDIO_THEME_GRUVBOX_DARK] = { "gruvbox-dark", "Gruvbox Dark" },
};

bool studio_theme_is_light(studio_theme theme, bool system_light)
{
    switch (theme) {
        case STUDIO_THEME_SYSTEM: return system_light;
        case STUDIO_THEME_LIGHT:
        case STUDIO_THEME_SOLARIZED_LIGHT:
        case STUDIO_THEME_TOKYO_DAY:
        case STUDIO_THEME_CATPPUCCIN_LATTE:
        case STUDIO_THEME_ROSE_PINE_DAWN:
        case STUDIO_THEME_GRUVBOX_LIGHT: return true;
        default: return false;
    }
}

void studio_theme_apply(kit *k, studio_theme theme, bool system_light)
{
    kit_set_body_class(k, "dark", !studio_theme_is_light(theme, system_light));
    for (int i = STUDIO_THEME_DRACULA; i < STUDIO_THEME_COUNT; i++) {
        char name[64];
        snprintf(name, sizeof name, "theme-%s", THEMES[i].value);
        kit_set_body_class(k, name, theme == (studio_theme)i);
    }
}
static const studio_setting_choice DENSITIES[] = { { "compact", "Compact" },
                                                   { "comfortable", "Comfortable" } };
static const studio_setting_choice WIRES[KIT_WIRE_STYLE_COUNT] = {
    { "curved", "Curved" },
    { "orthogonal", "Orthogonal" },
    { "straight", "Straight" },
    { "subway", "Subway" },
};
static const studio_setting_choice STANDARD_FONTS[] = {
    { "Inter", "Inter" },
    { "Noto Sans", "Noto Sans" },
    { "Geist", "Geist" },
};
static const studio_setting_choice MONOSPACE_FONTS[] = {
    { "JetBrains Mono", "JetBrains Mono" },
    { "Inconsolata", "Inconsolata" },
};
static const char *const STANDARD_FONT_FACES[] = { "inter", "noto sans", "geist" };
static const char *const MONOSPACE_FONT_FACES[] = { "jetbrains mono", "inconsolata" };
/* View › UI Scale's steps (studio_zoom), as browsers step a page's zoom. */
static const studio_setting_choice UI_SCALES[] = {
    { "0.5", "50%" },   { "0.67", "67%" },  { "0.75", "75%" },
    { "0.8", "80%" },   { "0.9", "90%" },   { "1", "100%" },
    { "1.1", "110%" },  { "1.25", "125%" }, { "1.5", "150%" },
    { "1.75", "175%" }, { "2", "200%" },
};

/* The grid increments snapping offers, in world units: 32 is a meter. */
static const studio_setting_choice GRID_INCREMENTS[] = {
    { "1", "1" },   { "4", "4" },   { "8", "8" },     { "16", "16" },
    { "32", "32" }, { "64", "64" }, { "128", "128" },
};

#define FIELD(name)                                                            \
    offsetof(studio_prefs, name), sizeof(((studio_prefs *)0)->name)
#define CHOICES(list) list, (int)(sizeof list / sizeof *list)

const studio_setting studio_settings[] = {
    { .key = "theme",
      .save = "theme",
      .category = STUDIO_SETTINGS_APPEARANCE,
      .label = "Theme",
      .description = "Choose a palette. System follows the operating system's "
                     "light or dark appearance.",
      .keywords = "color colour mode dark light appearance palette dracula "
                  "solarized "
                  "tokyo catppuccin rose pine gruvbox",
      .type = STUDIO_SETTING_CHOICE,
      FIELD(theme),
      CHOICES(THEMES),
      .fallback = STUDIO_THEME_SYSTEM },
    { .key = "density",
      .save = "density",
      .category = STUDIO_SETTINGS_APPEARANCE,
      .label = "Density",
      .description = "Comfortable makes rows and controls taller; text keeps "
                     "its size.",
      .keywords = "spacing size compact comfortable row height padding",
      .type = STUDIO_SETTING_CHOICE,
      FIELD(comfortable),
      CHOICES(DENSITIES) },
    { .key = "uiScale",
      .save = "uiScale",
      .category = STUDIO_SETTINGS_APPEARANCE,
      .label = "UI Scale",
      .description = "The size of the whole editor. Games keep their own UI "
                     "scale.",
      .keywords = "zoom size scale resolution larger smaller bigger",
      .type = STUDIO_SETTING_NUMBER,
      FIELD(ui_scale),
      CHOICES(UI_SCALES),
      .min = STUDIO_UI_SCALE_MIN,
      .max = STUDIO_UI_SCALE_MAX,
      .fallback = 1 },
    { .key = "reduceMotion",
      .save = "reduceMotion",
      .category = STUDIO_SETTINGS_APPEARANCE,
      .label = "Reduce Motion",
      .description = "Turns off transitions and animations.",
      .keywords = "animation transition motion accessibility",
      .type = STUDIO_SETTING_BOOL,
      FIELD(reduced_motion) },
    { .key = "font.standardFamily",
      .save = "standardFontFamily",
      .category = STUDIO_SETTINGS_APPEARANCE,
      .label = "Standard Font",
      .description = "The interface's standard text face.",
      .keywords = "font family typeface sans inter noto geist",
      .type = STUDIO_SETTING_CHOICE,
      FIELD(standard_font_family),
      CHOICES(STANDARD_FONTS),
      .fallback = 2 },
    { .key = "font.standardSize",
      .save = "standardFontSize",
      .category = STUDIO_SETTINGS_APPEARANCE,
      .label = "Standard Font Size",
      .description = "The size of interface text. UI Scale resizes the whole "
                     "editor.",
      .keywords = "font size text interface standard readable larger smaller",
      .type = STUDIO_SETTING_NUMBER,
      FIELD(standard_font_size),
      .min = 10,
      .max = 18,
      .step = 1,
      .fallback = 13 },
    /* The Scene view's Show toggles (View › Show), as Unity's Scene view
       effects and Unreal's Show flags. */
    { .key = "scene.generatedWorld",
      .save = "sceneGeneratedWorld",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Generated World",
      .description = "While you edit, the Scene shows terrain, clouds and "
                     "liquids as the game's first frame makes them.",
      .keywords = "procedural terrain field material fluid cave preview "
                  "realtime",
      .type = STUDIO_SETTING_BOOL,
      FIELD(scene_world),
      .fallback = 1 },
    { .key = "scene.lighting",
      .save = "sceneLighting",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Lighting",
      .description = "Lights the edited scene as the game does. Off shows "
                     "material colors unlit.",
      .keywords = "hrc radiance light shadows unlit",
      .type = STUDIO_SETTING_BOOL,
      FIELD(scene_lighting),
      .fallback = 1 },
    { .key = "scene.fog",
      .save = "sceneFog",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Fog of War",
      .description = "Covers the edited scene with its fog of war as the "
                     "game starts.",
      .keywords = "visibility vision explored conceal",
      .type = STUDIO_SETTING_BOOL,
      FIELD(scene_fog) },
    { .key = "scene.cameraFrames",
      .save = "sceneCameraFrames",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Camera Frames",
      .description = "Outlines what each camera shows, labeled with its "
                     "object's name.",
      .keywords = "camera frustum view bounds overlay",
      .type = STUDIO_SETTING_BOOL,
      FIELD(scene_cameras),
      .fallback = 1 },
    { .key = "wires",
      .save = "wires",
      .category = STUDIO_SETTINGS_GRAPH,
      .label = "Wire Style",
      .description = "How wires route between nodes.",
      .keywords = "edges links connections routing curved orthogonal straight "
                  "subway",
      .type = STUDIO_SETTING_CHOICE,
      FIELD(wires),
      CHOICES(WIRES) },
    { .key = "nodes.shortBindings",
      .save = "shortBindings",
      .category = STUDIO_SETTINGS_GRAPH,
      .label = "Short Bindings",
      .description = "Binding chips name the component and field, as "
                     "\xe2\x80\x9c"
                     "Aim \xe2\x80\xba Manual\xe2\x80\x9d, not the full path.",
      .keywords = "chips component field path names",
      .type = STUDIO_SETTING_BOOL,
      FIELD(short_bindings),
      .fallback = 1 },
    { .key = "nodes.titleFlow",
      .save = "titleFlow",
      .category = STUDIO_SETTINGS_GRAPH,
      .label = "Flow in Titles",
      .description = "Unlabeled execution pins sit in node titles.",
      .keywords = "exec execution pins header title",
      .type = STUDIO_SETTING_BOOL,
      FIELD(title_flow),
      .fallback = 1 },
    { .key = "nodes.compact",
      .save = "compactNodes",
      .category = STUDIO_SETTINGS_GRAPH,
      .label = "Compact Nodes",
      .description = "Operators show as symbols, and reads with one output as "
                     "pills.",
      .keywords = "operators symbols pills small dense",
      .type = STUDIO_SETTING_BOOL,
      FIELD(compact_nodes),
      .fallback = 1 },
    { .key = "debug.liveValues",
      .save = "liveValues",
      .category = STUDIO_SETTINGS_GRAPH,
      .label = "Show Live Values",
      .description = "While the debug target runs, every data output shows "
                     "the value it last carried.",
      .keywords = "watch debug values chips play running inspect",
      .type = STUDIO_SETTING_BOOL,
      FIELD(live_values) },
    { .key = "code.vim",
      .save = "codeVim",
      .category = STUDIO_SETTINGS_TEXT,
      .label = "Vim Keys",
      .description = "Edit with Vim's modes and keys.",
      .keywords = "vi modal keybindings normal insert",
      .type = STUDIO_SETTING_BOOL,
      FIELD(code_vim) },
    { .key = "code.fontFamily",
      .save = "codeFontFamily",
      .category = STUDIO_SETTINGS_TEXT,
      .label = "Monospace Font",
      .description = "The face used by code and monospace text.",
      .keywords = "font family typeface mono code jetbrains inconsolata",
      .type = STUDIO_SETTING_CHOICE,
      FIELD(code_font_family),
      CHOICES(MONOSPACE_FONTS) },
    { .key = "code.fontSize",
      .save = "codeFontSize",
      .category = STUDIO_SETTINGS_TEXT,
      .label = "Monospace Font Size",
      .description = "The size of code and monospace text. UI Scale enlarges "
                     "it too.",
      .keywords = "text code size zoom points mono",
      .type = STUDIO_SETTING_NUMBER,
      FIELD(code_font_size),
      .min = 8,
      .max = 32,
      .step = 1,
      .fallback = 13 },
    { .key = "code.applyLive",
      .save = "codeApplyLive",
      .category = STUDIO_SETTINGS_TEXT,
      .label = "Apply Live",
      .description = "UI edits apply as you type. Off, they apply when you "
                     "save.",
      .keywords = "live preview hot reload save apply mode",
      .type = STUDIO_SETTING_BOOL,
      FIELD(code_apply_live),
      .fallback = 1 },
    /* Workstream I */
    { .key = "code.wrap",
      .save = "codeWrap",
      .category = STUDIO_SETTINGS_TEXT,
      .label = "Word Wrap",
      .description = "Long lines wrap at the Text Editor's edge. Off, they "
                     "run on and scroll sideways.",
      .keywords = "wrap lines horizontal scroll long",
      .type = STUDIO_SETTING_BOOL,
      FIELD(code_wrap),
      .fallback = 1 },
    { .key = "scene.grid",
      .save = "sceneGrid",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Show Grid",
      .description = "Draws the grid under objects while a 2D scene is "
                     "edited.",
      .keywords = "lines axes overlay",
      .type = STUDIO_SETTING_BOOL,
      FIELD(scene_grid),
      .fallback = 1 },
    { .key = "scene.snap",
      .save = "sceneSnap",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Snap to Grid",
      .description = "Moved and resized objects land on the grid increment, "
                     "and rotations on 15\xc2\xb0 steps. Holding Ctrl while "
                     "dragging does the opposite.",
      .keywords = "magnet align round increment",
      .type = STUDIO_SETTING_BOOL,
      FIELD(scene_snap) },
    { .key = "scene.gridIncrement",
      .save = "sceneGridIncrement",
      .category = STUDIO_SETTINGS_SCENE,
      .label = "Grid Increment",
      .description = "The grid's spacing in world units, and the step "
                     "snapping moves by. 32 units are a meter.",
      .keywords = "size spacing step snap units",
      .type = STUDIO_SETTING_NUMBER,
      FIELD(scene_increment),
      CHOICES(GRID_INCREMENTS),
      .min = 1,
      .max = 128,
      .fallback = 32 },
};
const int studio_setting_count = sizeof studio_settings / sizeof *studio_settings;

const char *studio_standard_font(const studio_prefs *prefs)
{
    return STANDARD_FONT_FACES[prefs->standard_font_family];
}

const char *studio_code_font(const studio_prefs *prefs)
{
    return MONOSPACE_FONT_FACES[prefs->code_font_family];
}

static void apply_font_settings(kit *k, const studio_prefs *prefs, bool graph)
{
    kit_set_standard_font_family(k, studio_standard_font(prefs));
    char css[24576];
    float scale = graph ? 1 : prefs->standard_font_size / 13.0f;
    int h = graph ? 22 : (int)lroundf(26 * scale);
    int text_line = graph ? 20 : (int)lroundf(24 * scale);
    int h_sm = graph ? 18 : (int)lroundf(22 * scale);
    int row = h;
    int gap = (int)lroundf(2 * scale);
    int header_h = (int)lroundf(32 * scale);
    int topbar_h = (int)lroundf(40 * scale);
    int line_extra = 0;
    int font_size = graph ? 12 : (int)lroundf(prefs->standard_font_size);
    uintptr_t face = rml_font_face(studio_standard_font(prefs), RML_FONT_NORMAL, 400, font_size);
    assert(face);
    int body_line = graph ? 16 : (int)lroundf(18 * scale);
    line_extra = (int)ceilf(rml_font_face_metrics(face).line_spacing) - body_line;
    if (line_extra < 0 || graph) {
        line_extra = 0;
    }
    int input_line = (graph ? 16 : (int)lroundf(16 * scale)) + line_extra;
    const char *numeric_fonts[] = { "InterNumeric", "noto sans", "GeistNumeric" };
    int written = snprintf(
        css,
        sizeof css,
        "body, body.dark { --studio-standard-font-family: %s; "
        "--studio-code-font-family: %s; --studio-numeric-font-family: %s; "
        "font-size: %ddp; line-height: %ddp; --studio-property-label-max: "
        "%ddp; "
        "--h: %ddp; --text-line: %ddp; --h-sm: %ddp; "
        "--input-line: %ddp; --input-pad: %.1fdp; "
        "--row: %ddp; --gap: %ddp; --header-h: %ddp; "
        "--dropdown-list-h: %ddp; --dropdown-box-h: %ddp; "
        "--topbar-h: %ddp; --status-h: %ddp; --panel-h: %ddp; }\n"
        "body.comfortable, body.dark.comfortable { --h: %ddp; "
        "--text-line: %ddp; --h-sm: %ddp; "
        "--input-line: %ddp; --input-pad: %.1fdp; "
        "--row: %ddp; --gap: %ddp; --header-h: %ddp; "
        "--dropdown-list-h: %ddp; --dropdown-box-h: %ddp; "
        "--topbar-h: %ddp; --status-h: %ddp; --panel-h: %ddp; }\n"
        ".code-completions, .code-hover, .code-hover-title, .code-hover-line, "
        ".code-mode, kit-code { font-family: var(--studio-code-font-family, "
        "jetbrains mono); }\n"
        ".code-completions { font-size: var(--studio-mono-font-12, 12dp); }\n"
        ".code-hover { font-size: var(--studio-mono-font-12, 12dp); }\n"
        ".code-hover-line { font-size: var(--studio-mono-font-11, 11dp); }\n"
        ".code-mode { font-size: var(--studio-mono-font-10, 10dp); }\n"
        ".code-completion .detail { font-family: "
        "var(--studio-standard-font-family, inter); }\n"
        ".designer-bindings .field .value { font-family: "
        "var(--studio-code-font-family, jetbrains mono); }\n"
        ".field input.value, .field.text .value, .field.search .value, "
        ".field.area textarea.value, .field.select .value { "
        "font-family: var(--studio-standard-font-family, inter); }\n",
        studio_standard_font(prefs),
        studio_code_font(prefs),
        numeric_fonts[prefs->standard_font_family],
        font_size,
        body_line + line_extra,
        (int)lroundf(160 * scale),
        h,
        text_line,
        h_sm,
        input_line,
        fmaxf(0, (text_line - input_line) * 0.5f),
        row,
        gap,
        header_h,
        7 * h,
        7 * h + 10,
        topbar_h,
        (int)lroundf(28 * scale),
        (int)lroundf(30 * scale),
        graph ? 22 : (int)lroundf(30 * scale),
        graph ? 20 : (int)lroundf(28 * scale),
        graph ? 18 : (int)lroundf(26 * scale),
        input_line,
        fmaxf(0, ((graph ? 20 : (int)lroundf(28 * scale)) - input_line) * 0.5f),
        graph ? 22 : (int)lroundf(30 * scale),
        (int)lroundf(3 * scale),
        (int)lroundf(36 * scale),
        7 * (graph ? 22 : (int)lroundf(30 * scale)),
        7 * (graph ? 22 : (int)lroundf(30 * scale)) + 10,
        (int)lroundf(44 * scale),
        (int)lroundf(30 * scale),
        (int)lroundf(34 * scale)
    );
    assert(written > 0 && (size_t)written < sizeof css);
    size_t used = (size_t)written;
    for (int size = 8; size <= 64; size++) {
        int standard = (int)lroundf((float)size * scale);
        int mono = (int)lroundf((float)size * prefs->code_font_size / 13.0f);
        int added = snprintf(
            css + used,
            sizeof css - used,
            "body { --studio-font-%d: %ddp; --studio-mono-font-%d: %ddp; "
            "--studio-line-%d: %ddp; --studio-mono-line-%d: %ddp; "
            "--studio-height-%d: %ddp; --studio-mono-height-%d: %ddp; }\n",
            size,
            standard,
            size,
            mono,
            size,
            (int)lroundf((float)size * scale) + line_extra,
            size,
            (int)lroundf((float)size * prefs->code_font_size / 13.0f),
            size,
            (int)lroundf((float)size * scale),
            size,
            (int)lroundf((float)size * prefs->code_font_size / 13.0f)
        );
        assert(added > 0 && (size_t)added < sizeof css - used);
        used += (size_t)added;
    }
    kit_style_sheet(k, "studio-font-settings", css);
}

void studio_apply_font_settings(kit *k, const studio_prefs *prefs)
{
    apply_font_settings(k, prefs, false);
}

void studio_apply_graph_font_settings(kit *k, const studio_prefs *prefs)
{
    studio_prefs graph_prefs = *prefs;
    graph_prefs.standard_font_size = 12;
    graph_prefs.code_font_size = 13;
    apply_font_settings(k, &graph_prefs, true);
}

static const void *field_at(const studio_prefs *prefs, const studio_setting *s)
{
    return (const char *)prefs + s->offset;
}

float studio_setting_get(const studio_prefs *prefs, const studio_setting *s)
{
    const void *field = field_at(prefs, s);
    switch (s->type) {
        case STUDIO_SETTING_BOOL:
            assert(s->size == sizeof(bool));
            return *(const bool *)field;
        case STUDIO_SETTING_CHOICE:
            if (s->size == sizeof(bool)) {
                return *(const bool *)field;
            }
            assert(s->size == sizeof(int));
            return (float)*(const int *)field;
        case STUDIO_SETTING_NUMBER:
            assert(s->size == sizeof(float));
            return *(const float *)field;
    }
    assert(!"a setting type");
    return 0;
}

void studio_setting_set(studio_prefs *prefs, const studio_setting *s, float value)
{
    void *field = (char *)prefs + s->offset;
    if (s->type == STUDIO_SETTING_NUMBER) {
        assert(s->size == sizeof(float) && value >= s->min && value <= s->max);
        *(float *)field = value;
        return;
    }
    assert(
        s->type == STUDIO_SETTING_BOOL ? value == 0 || value == 1
                                       : value >= 0 && value < s->choice_count
    );
    if (s->size == sizeof(bool)) {
        *(bool *)field = value != 0;
        return;
    }
    assert(s->type == STUDIO_SETTING_CHOICE && s->size == sizeof(int));
    *(int *)field = (int)value;
}

void studio_setting_text(const studio_prefs *prefs, const studio_setting *s, char *out, size_t size)
{
    float value = studio_setting_get(prefs, s);
    switch (s->type) {
        case STUDIO_SETTING_BOOL:
            snprintf(out, size, "%s", value ? "true" : "false");
            return;
        case STUDIO_SETTING_CHOICE:
            snprintf(out, size, "%s", s->choices[(int)value].value);
            return;
        case STUDIO_SETTING_NUMBER: snprintf(out, size, "%g", value); return;
    }
}

/* A number with presets takes one of them, so a select of them always shows
 * its value, as UI Scale's steps do. */
static bool preset(const studio_setting *s, float value)
{
    for (int i = 0; i < s->choice_count; i++) {
        if (strtof(s->choices[i].value, NULL) == value) {
            return true;
        }
    }
    return s->choice_count == 0;
}

bool studio_setting_parse(studio_prefs *prefs, const studio_setting *s, const char *text)
{
    if (s->type == STUDIO_SETTING_BOOL) {
        bool on = !strcmp(text, "true");
        if (!on && strcmp(text, "false") != 0) {
            return false;
        }
        studio_setting_set(prefs, s, on);
        return true;
    }
    if (s->type == STUDIO_SETTING_CHOICE) {
        for (int i = 0; i < s->choice_count; i++) {
            if (!strcmp(s->choices[i].value, text)) {
                studio_setting_set(prefs, s, (float)i);
                return true;
            }
        }
        return false;
    }
    char *end;
    float value = strtof(text, &end);
    if (end == text || *end || !isfinite(value) || value < s->min ||
        value > s->max || !preset(s, value)) {
        return false;
    }
    studio_setting_set(prefs, s, value);
    return true;
}

/* Whether `word`, `length` bytes, is in `text`, ignoring case. */
static bool contains_word(const char *text, const char *word, size_t length)
{
    for (const char *at = text; *at; at++) {
        size_t i = 0;
        while (i < length && at[i] &&
               tolower((unsigned char)at[i]) == tolower((unsigned char)word[i])) {
            i++;
        }
        if (i == length) {
            return true;
        }
    }
    return false;
}

bool studio_settings_match(const char *query, const char *text)
{
    for (const char *at = query; *at;) {
        at += strspn(at, " ");
        size_t length = strcspn(at, " ");
        if (length && !contains_word(text, at, length)) {
            return false;
        }
        at += length;
    }
    return true;
}

bool studio_setting_matches(const studio_setting *s, const char *query)
{
    char text[1024];
    snprintf(
        text,
        sizeof text,
        "%s %s %s %s",
        s->label,
        s->description,
        s->keywords,
        studio_settings_categories[s->category].label
    );
    return studio_settings_match(query, text);
}
