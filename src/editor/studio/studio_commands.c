/* Studio's commands: one table the menus, the command palette, shortcuts and
 * authored UI share, as VS Code's command registry and Blender's operators.
 * Authored UI runs one with emit('studio.<id>'), or on a tab or an area with
 * emit('studio.<id>', index), as VS Code's commands take arguments. Ids are
 * camelCase, so `commands.<id>` binds the same name. */
#include "editor/workspace/viewport.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_internal.h"

#include <SDL3/SDL_events.h>
#include <string.h>

typedef struct
{
    const char *id;
    const char *label;
    const char *menu;       /* the label in a menu, when it differs */
    const char *chord;      /* default studio_chord notation */
    const char *aliases[4]; /* alternate chords active only without an override */
    const char *context;
    uint32_t contexts; /* zero is global; else a mask of editor types */
    uint32_t excluded_contexts;
    const char *keywords;
    uint32_t icon;
    bool indexed; /* runs only on the tab its argument names */
    bool repeat;
    int phase;
} command_entry;

#define EDITOR_BIT(editor) (1u << (editor))
#define EDITOR_ALL ((1u << (STUDIO_EDITOR_COUNT + 1)) - 1)
/* The areas that show the scene's objects. */
#define OBJECT_AREAS                                                           \
    (EDITOR_BIT(STUDIO_EDITOR_SCENE) | EDITOR_BIT(STUDIO_EDITOR_OUTLINER))
/* The areas whose search Find opens or focuses. */
#define FIND_AREAS                                                             \
    (EDITOR_BIT(STUDIO_EDITOR_BEHAVIOR) | EDITOR_BIT(STUDIO_EDITOR_CODE) |     \
     EDITOR_BIT(STUDIO_EDITOR_OUTLINER) |                                      \
     EDITOR_BIT(STUDIO_EDITOR_PROPERTIES) | EDITOR_BIT(STUDIO_EDITOR_ASSETS) | \
     EDITOR_BIT(STUDIO_EDITOR_OUTPUT) | EDITOR_BIT(STUDIO_EDITOR_MEMBERS))

enum
{
    SHORTCUT_DEBUG,
    SHORTCUT_EARLY,
    SHORTCUT_PLAY,
    SHORTCUT_EDITOR,
};

/* A New command for each authored asset kind. */
#define NEW_ASSET(name, kind, words, glyph)                                    \
    { .id = "new" name,                                                        \
      .label = "New " kind "\xe2\x80\xa6",                                     \
      .menu = kind,                                                            \
      .keywords = "create add asset " words,                                   \
      .icon = glyph,                                                           \
      .phase = SHORTCUT_EDITOR }
_Static_assert(
    STUDIO_COMMAND_NEW_STYLESHEET - STUDIO_COMMAND_NEW_SCENE + 1 == STUDIO_NEW_KIND_COUNT,
    "a New command per kind"
);

/* A Scene tool: its command checks while the tool is active. */
#define SCENE_TOOL(name, tool, key, words, glyph)                              \
    { .id = name,                                                              \
      .label = tool " Tool",                                                   \
      .menu = tool,                                                            \
      .chord = key,                                                            \
      .context = "Scene",                                                      \
      .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),                             \
      .keywords = "tool " words,                                               \
      .icon = glyph,                                                           \
      .phase = SHORTCUT_EDITOR }
_Static_assert(
    STUDIO_COMMAND_SCALE_TOOL - STUDIO_COMMAND_SELECT_TOOL + 1 == SCENE_TOOL_COUNT,
    "a command per Scene tool"
);

static const command_entry COMMANDS[STUDIO_COMMAND_COUNT] = {
    [STUDIO_COMMAND_SEARCH] = { .id = "search",
                                .label = "Search",
                                .chord = "Mod+K",
                                .aliases = { "Mod+P" },
                                .context = "Global",
                                .keywords = "find go to",
                                .icon = KIT_ICON_SEARCH,
                                .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_SETTINGS] = { .id = "settings",
                                  .label = "Settings",
                                  .chord = "Mod+,",
                                  .context = "Global",
                                  .keywords = "preferences options",
                                  .icon = KIT_ICON_SETTINGS,
                                  .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_SAVE] = { .id = "save",
                              .label = "Save",
                              .chord = "Mod+S",
                              .context = "Global",
                              .icon = KIT_ICON_SAVE,
                              .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_RELOAD] = { .id = "reload",
                                .label = "Reload",
                                .context = "Global",
                                .keywords = "revert disk",
                                .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_UNDO] = { .id = "undo",
                              .label = "Undo",
                              .chord = "Mod+Z",
                              .context = "Global",
                              .icon = KIT_ICON_UNDO,
                              .repeat = true,
                              .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_REDO] = { .id = "redo",
                              .label = "Redo",
                              .chord = "Mod+Shift+Z",
                              .aliases = { "Mod+Y" },
                              .context = "Global",
                              .icon = KIT_ICON_REDO,
                              .repeat = true,
                              .phase = SHORTCUT_EARLY },
    /* Blender's Shift+D, and Mod+D as Unity, Unreal and Godot duplicate. */
    [STUDIO_COMMAND_DUPLICATE] = { .id = "duplicate",
                                   .label = "Duplicate",
                                   .chord = "Shift+D",
                                   .aliases = { "Mod+D" },
                                   .context = "Scene, Outliner",
                                   .contexts = OBJECT_AREAS,
                                   .keywords = "copy",
                                   .icon = KIT_ICON_COPY,
                                   .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_DELETE] = { .id = "delete",
                                .label = "Delete",
                                .chord = "X",
                                /* A Mac's delete key is Backspace. */
                                .aliases = { "Backspace", "Delete" },
                                .context = "Scene, Outliner",
                                .contexts = OBJECT_AREAS,
                                .keywords = "remove",
                                .icon = KIT_ICON_TRASH,
                                .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_OPEN_PROTOTYPE] = { .id = "openPrototype",
                                        .label = "Open Prototype",
                                        .context = "Global",
                                        .keywords = "edit",
                                        .icon = KIT_ICON_PROTOTYPE,
                                        .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_FRAME] = { .id = "frame",
                               .label = "Frame Selected",
                               .chord = "F",
                               .context = "Scene, Outliner",
                               .contexts = OBJECT_AREAS,
                               .keywords = "focus zoom",
                               .icon = KIT_ICON_FOCUS,
                               .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_RESET_CAMERA] = { .id = "resetCamera",
                                      .label = "Reset Camera",
                                      .context = "Global",
                                      .keywords = "view",
                                      .phase = SHORTCUT_EDITOR },
    /* Hide in Editor, with Blender's keys. */
    [STUDIO_COMMAND_HIDE_SELECTED] = { .id = "hideSelected",
                                       .label = "Hide Selected",
                                       .chord = "H",
                                       .context = "Scene, Outliner",
                                       .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE) |
                                                   EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                       .keywords = "visibility eye hide in "
                                                   "editor",
                                       .icon = KIT_ICON_EYE_OFF,
                                       .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_HIDE_UNSELECTED] = { .id = "hideUnselected",
                                         .label = "Hide Unselected",
                                         .chord = "Shift+H",
                                         .context = "Scene, Outliner",
                                         .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE) |
                                                     EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                         .keywords = "visibility isolate solo "
                                                     "others",
                                         .icon = KIT_ICON_EYE_OFF,
                                         .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SHOW_ALL] = { .id = "showAll",
                                  .label = "Show All",
                                  .chord = "Alt+H",
                                  .context = "Scene, Outliner",
                                  .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE) |
                                              EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                  .keywords = "visibility unhide reveal eye",
                                  .icon = KIT_ICON_EYE,
                                  .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_TOGGLE_ASSETS] = { .id = "toggleAssets",
                                       .label = "Toggle Assets",
                                       .menu = "Assets",
                                       .chord = "Mod+Shift+A",
                                       .context = "Global",
                                       .keywords = "drawer browser content",
                                       .icon = KIT_ICON_ASSETS,
                                       .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_TOGGLE_OUTPUT] = { .id = "toggleOutput",
                                       .label = "Toggle Output",
                                       .menu = "Output",
                                       .chord = "Mod+Shift+U",
                                       .context = "Global",
                                       .keywords = "drawer log console "
                                                   "problems diagnostics",
                                       .icon = KIT_ICON_OUTPUT,
                                       .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_TOGGLE_PERFORMANCE] = { .id = "togglePerformance",
                                            .label = "Toggle Performance",
                                            .menu = "Performance",
                                            .context = "Global",
                                            .keywords = "drawer profiler frame "
                                                        "time fps",
                                            .icon = KIT_ICON_PERFORMANCE,
                                            .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_MAXIMIZE] = { .id = "maximize",
                                  .label = "Maximize Area",
                                  .chord = "Ctrl+Space",
                                  .context = "Area under pointer",
                                  .keywords = "fullscreen",
                                  .icon = KIT_ICON_MAXIMIZE,
                                  .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_RESET_LAYOUT] = { .id = "resetLayout",
                                      .label = "Reset Layout",
                                      .context = "Global",
                                      .keywords = "areas default",
                                      .icon = KIT_ICON_LAYOUT,
                                      .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_ZOOM_IN] = { .id = "zoomIn",
                                 .label = "Zoom In",
                                 .chord = "Mod+=",
                                 .aliases = { "Mod+Shift+=", "Mod+Plus", "Mod+Shift+Plus", "Mod+KeypadPlus" },
                                 .context = "Global",
                                 .keywords = "ui scale larger bigger",
                                 .icon = KIT_ICON_ZOOM_IN,
                                 .repeat = true,
                                 .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_ZOOM_OUT] = { .id = "zoomOut",
                                  .label = "Zoom Out",
                                  .chord = "Mod+-",
                                  .aliases = { "Mod+Shift+-", "Mod+Keypad -" },
                                  .context = "Global",
                                  .keywords = "ui scale smaller",
                                  .icon = KIT_ICON_ZOOM_OUT,
                                  .repeat = true,
                                  .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_ZOOM_RESET] = { .id = "zoomReset",
                                    .label = "Reset Zoom",
                                    .chord = "Mod+0",
                                    .aliases = { "Mod+Keypad 0" },
                                    .context = "Global",
                                    .keywords = "ui scale actual size 100%",
                                    .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_PLAY] = { .id = "play",
                              .label = "Play",
                              .chord = "Space",
                              .aliases = { "Shift+Space", "Alt+Space", "Alt+Shift+Space" },
                              .context = "Except Behavior",
                              .excluded_contexts = EDITOR_BIT(STUDIO_EDITOR_BEHAVIOR),
                              .keywords = "run start preview pause",
                              .icon = KIT_ICON_PLAY,
                              .phase = SHORTCUT_PLAY },
    /* VS Code's Stop, beside the debugger's F5, F10 and F11. */
    [STUDIO_COMMAND_STOP] = { .id = "stop",
                              .label = "Stop",
                              .chord = "Shift+F5",
                              .context = "Global",
                              .icon = KIT_ICON_STOP,
                              .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_STEP] = { .id = "step",
                              .label = "Step",
                              .context = "Global",
                              .keywords = "frame advance",
                              .icon = KIT_ICON_STEP,
                              .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_RECORD_GAMEPLAY] = { .id = "recordGameplay",
                                         .label = "Record Gameplay",
                                         .context = "Global",
                                         .keywords = "scenario capture replay "
                                                     "input",
                                         .icon = KIT_ICON_RECORD,
                                         .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_EXPORT_GAME] = { .id = "exportGame",
                                     .label = "Export Game",
                                     .context = "Global",
                                     .keywords = "build ship package release "
                                                 "app desktop",
                                     .icon = KIT_ICON_PACKAGE,
                                     .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_DENSITY] = { .id = "density",
                                 .label = "Comfortable Density",
                                 .context = "Global",
                                 .keywords = "spacing size settings",
                                 .icon = KIT_ICON_LAYOUT,
                                 .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_MOTION] = { .id = "motion",
                                .label = "Reduce Motion",
                                .context = "Global",
                                .keywords = "animation settings",
                                .icon = KIT_ICON_SETTINGS,
                                .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_THEME] = { .id = "theme",
                               .label = "Light Theme",
                               .context = "Global",
                               .keywords = "appearance color mode settings",
                               .icon = KIT_ICON_SUN,
                               .phase = SHORTCUT_EDITOR },
    /* UI Inspector (studio_inspect.c) */
    [STUDIO_COMMAND_INSPECT] = { .id = "inspectElement",
                                 .label = "Inspect Element",
                                 .chord = "Mod+Shift+C",
                                 .context = "Global",
                                 .keywords = "ui inspector devtools debugger "
                                             "pick styles rml rcss",
                                 .icon = KIT_ICON_SELECT,
                                 .phase = SHORTCUT_EARLY },
    /* UI wiring (workstream K) */
    [STUDIO_COMMAND_PREVIEW_UI] = { .id = "previewUi",
                                    .label = "Preview UI Asset\xe2\x80\xa6",
                                    .context = "Global",
                                    .keywords = "canvas storybook widget "
                                                "document screen size",
                                    .icon = KIT_ICON_LAYOUT,
                                    .phase = SHORTCUT_EDITOR },
    /* Assets ▸ New lists each kind, and the palette exposes each command. */
    [STUDIO_COMMAND_NEW_SCENE] = NEW_ASSET("Scene", "Scene", "level world", KIT_ICON_SCENE),
    [STUDIO_COMMAND_NEW_PROTOTYPE] = NEW_ASSET("Prototype", "Prototype", "prefab template entity", KIT_ICON_PROTOTYPE),
    [STUDIO_COMMAND_NEW_BEHAVIOR] = NEW_ASSET("Behavior", "Behavior", "graph script blueprint", KIT_ICON_GRAPH),
    [STUDIO_COMMAND_NEW_EVENT] = NEW_ASSET("Event", "Event", "signal message definition", KIT_ICON_EVENT),
    [STUDIO_COMMAND_NEW_SYSTEM] = NEW_ASSET("System", "System", "ecs query graph", KIT_ICON_SYSTEM),
    [STUDIO_COMMAND_NEW_LIBRARY] = NEW_ASSET("Library", "Library", "functions macros graph", KIT_ICON_LIBRARY),
    [STUDIO_COMMAND_NEW_MATERIAL] = NEW_ASSET("Material", "Material", "shader surface", KIT_ICON_MATERIAL),
    [STUDIO_COMMAND_NEW_SHAPE] = NEW_ASSET("Shape", "Shape", "shader sdf distance", KIT_ICON_HEXAGON),
    [STUDIO_COMMAND_NEW_FIELD] = NEW_ASSET("Field", "Field", "procedural terrain noise", KIT_ICON_GRID),
    [STUDIO_COMMAND_NEW_SHADER_LIBRARY] = NEW_ASSET("ShaderLibrary", "Shader Library", "shader functions", KIT_ICON_LIBRARY),
    [STUDIO_COMMAND_NEW_MODEL] = NEW_ASSET("Model", "Model", "3d sdf geometry asset", KIT_ICON_HEXAGON),
    [STUDIO_COMMAND_NEW_UI_WIDGET] = NEW_ASSET("UiWidget", "UI Widget", "component rml", KIT_ICON_LAYOUT),
    [STUDIO_COMMAND_NEW_UI_DOCUMENT] = NEW_ASSET("UiDocument", "UI Document", "panel screen rml", KIT_ICON_LAYOUT),
    [STUDIO_COMMAND_NEW_STYLESHEET] = NEW_ASSET("Stylesheet", "Stylesheet", "ui styles rcss css", KIT_ICON_FILE),
    [STUDIO_COMMAND_RENAME] = { .id = "rename",
                                .label = "Rename",
                                .chord = "F2",
                                .context = "Scene, Outliner",
                                .contexts = OBJECT_AREAS,
                                .keywords = "name object",
                                .icon = KIT_ICON_EDIT,
                                .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_ADD_COMPONENT] = { .id = "addComponent",
                                       .label = "Add Component\xe2\x80\xa6",
                                       .context = "Properties",
                                       .contexts = EDITOR_BIT(STUDIO_EDITOR_PROPERTIES),
                                       .keywords = "prototype object attach",
                                       .icon = KIT_ICON_ADD,
                                       .phase = SHORTCUT_EDITOR },
    /* Unreal's Keep Simulation Changes, and its K. */
    [STUDIO_COMMAND_KEEP_CHANGES] = { .id = "keepChanges",
                                      .label = "Keep Changes",
                                      .chord = "K",
                                      .context = "Scene, Properties",
                                      .contexts = EDITOR_BIT(STUDIO_EDITOR_PROPERTIES) |
                                                  EDITOR_BIT(STUDIO_EDITOR_SCENE),
                                      .keywords = "play live runtime "
                                                  "simulation "
                                                  "values apply copy",
                                      .icon = KIT_ICON_PIN,
                                      .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT] = { .id = "workspaceVerticalSpli"
                                                        "t",
                                                  .label = "Vertical Split",
                                                  .menu = "Vertical Split",
                                                  .context = "Global",
                                                  .keywords = "vertical split "
                                                              "stacked scene "
                                                              "behavior",
                                                  .icon = KIT_ICON_LAYOUT,
                                                  .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT] = { .id = "workspaceHorizontalS"
                                                          "plit",
                                                    .label = "Horizontal Split",
                                                    .menu = "Horizontal Split",
                                                    .context = "Global",
                                                    .keywords = "horizontal "
                                                                "split side by "
                                                                "side canvas "
                                                                "text editor",
                                                    .icon = KIT_ICON_LAYOUT,
                                                    .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_TOGGLE_SIDE_PANE] = { .id = "toggleSidePane",
                                          .label = "Side Pane",
                                          .context = "Global",
                                          .keywords = "sidebar outliner "
                                                      "properties hierarchy "
                                                      "details column",
                                          .icon = KIT_ICON_SIDE_PANE,
                                          .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION] = { .id = "collaboration",
                                       .label = "Collaboration",
                                       .context = "Global",
                                       .keywords = "share room invite host "
                                                   "join",
                                       .icon = KIT_ICON_BRANCH,
                                       .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_TOKEN] = { .id = "collaborationToken",
                                             .label = "Use admin token",
                                             .context = "Global",
                                             .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_FOLLOW] = { .id = "collaborationFollow",
                                              .label = "Follow participant",
                                              .context = "Global",
                                              .indexed = true,
                                              .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_UNFOLLOW] = { .id = "collaborationUnfollow",
                                                .label = "Stop following",
                                                .context = "Global",
                                                .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_HOST] = { .id = "collaborationHost",
                                            .label = "Host Collaboration Room",
                                            .context = "Collaboration",
                                            .icon = KIT_ICON_BRANCH,
                                            .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_JOIN] = { .id = "collaborationJoin",
                                            .label = "Join Collaboration Room",
                                            .context = "Collaboration",
                                            .icon = KIT_ICON_EXTERNAL,
                                            .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_LEAVE] = { .id = "collaborationLeave",
                                             .label = "Leave Collaboration "
                                                      "Room",
                                             .context = "Collaboration",
                                             .icon = KIT_ICON_CLOSE,
                                             .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_INVITE_VIEWER] = { .id = "collaborationInvite"
                                                           "Viewer",
                                                     .label = "Copy Viewer "
                                                              "Invite",
                                                     .context = "Collaboration",
                                                     .icon = KIT_ICON_EYE,
                                                     .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_COLLABORATION_INVITE_EDITOR] = { .id = "collaborationInvite"
                                                           "Editor",
                                                     .label = "Copy Editor "
                                                              "Invite",
                                                     .context = "Collaboration",
                                                     .icon = KIT_ICON_EDIT,
                                                     .phase = SHORTCUT_EARLY },
    /* Authored top bar and area headers (workstream T): the document tabs'
       commands take the tab's index, emit('studio.selectTab', i). */
    [STUDIO_COMMAND_SELECT_TAB] = { .id = "selectTab",
                                    .label = "Select Tab",
                                    .context = "Global",
                                    .keywords = "document switch",
                                    .icon = KIT_ICON_FILE,
                                    .indexed = true,
                                    .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_CLOSE_TAB] = { .id = "closeTab",
                                   .chord = "Mod+W",
                                   .label = "Close Tab",
                                   .context = "Global",
                                   .keywords = "document",
                                   .icon = KIT_ICON_CLOSE,
                                   .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_TOGGLE_PIN_TAB] = { .id = "togglePinTab",
                                        .label = "Pin / Unpin Tab",
                                        .context = "Global",
                                        .keywords = "document keep",
                                        .icon = KIT_ICON_PIN,
                                        .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_PREVIOUS_TAB] = { .id = "previousTab",
                                      .label = "Previous Tab",
                                      .chord = "Ctrl+Shift+Tab",
                                      .context = "Global",
                                      .icon = KIT_ICON_FILE,
                                      .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_NEXT_TAB] = { .id = "nextTab",
                                  .label = "Next Tab",
                                  .chord = "Ctrl+Tab",
                                  .context = "Global",
                                  .icon = KIT_ICON_FILE,
                                  .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_CLOSE_DRAWER] = { .id = "closeDrawer",
                                      .label = "Close Drawer",
                                      .context = "Global",
                                      .keywords = "hide panel",
                                      .icon = KIT_ICON_CLOSE,
                                      .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SELECT_PREVIOUS] = { .id = "selectPrevious",
                                         .label = "Select Previous",
                                         .chord = "Up",
                                         .context = "Outliner",
                                         .contexts = EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                         .repeat = true,
                                         .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SELECT_NEXT] = { .id = "selectNext",
                                     .label = "Select Next",
                                     .chord = "Down",
                                     .context = "Outliner",
                                     .contexts = EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                     .repeat = true,
                                     .phase = SHORTCUT_EDITOR },
    /* View › Show (studio_scene.c): toggles of studio_prefs, Unity's
       Scene view effects and Unreal's Show flags. */
    [STUDIO_COMMAND_SHOW_WORLD] = { .id = "showGeneratedWorld",
                                    .label = "Show Generated World",
                                    .menu = "Generated World",
                                    .context = "Global",
                                    .keywords = "scene view procedural terrain "
                                                "material fluid preview",
                                    .icon = KIT_ICON_TERRAIN,
                                    .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SHOW_LIGHTING] = { .id = "showLighting",
                                       .label = "Show Lighting",
                                       .menu = "Lighting",
                                       .context = "Global",
                                       .keywords = "scene view hrc unlit "
                                                   "light",
                                       .icon = KIT_ICON_LIGHT,
                                       .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SHOW_FOG] = { .id = "showFog",
                                  .label = "Show Fog of War",
                                  .menu = "Fog of War",
                                  .context = "Global",
                                  .keywords = "scene view visibility vision",
                                  .icon = KIT_ICON_CLOUD,
                                  .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SHOW_CAMERAS] = { .id = "showCameraFrames",
                                      .label = "Show Camera Frames",
                                      .menu = "Camera Frames",
                                      .context = "Global",
                                      .keywords = "scene view camera frustum "
                                                  "bounds overlay",
                                      .icon = KIT_ICON_CAMERA,
                                      .phase = SHORTCUT_EDITOR },
    /* Blender's Numpad 0; Shift+C where there is no keypad. */
    [STUDIO_COMMAND_VIEW_THROUGH_CAMERA] = { .id = "viewThroughCamera",
                                             .label = "View Through Camera",
                                             .chord = "Shift+C",
                                             .aliases = { "Keypad 0" },
                                             .context = "Scene, Outliner",
                                             .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE) |
                                                         EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                             .keywords = "game camera view "
                                                         "frame pilot",
                                             .icon = KIT_ICON_VIEWPORT,
                                             .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_RESUME] = { .id = "resume",
                                .label = "Resume",
                                .chord = "F5",
                                .context = "When paused",
                                .icon = KIT_ICON_CONTINUE,
                                .repeat = true,
                                .phase = SHORTCUT_DEBUG },
    [STUDIO_COMMAND_STEP_OVER] = { .id = "stepOver",
                                   .label = "Step Over",
                                   .chord = "F10",
                                   .context = "When paused",
                                   .icon = KIT_ICON_STEP_OVER,
                                   .repeat = true,
                                   .phase = SHORTCUT_DEBUG },
    [STUDIO_COMMAND_STEP_INTO] = { .id = "stepInto",
                                   .label = "Step Into",
                                   .chord = "F11",
                                   .context = "When paused",
                                   .icon = KIT_ICON_STEP_INTO,
                                   .repeat = true,
                                   .phase = SHORTCUT_DEBUG },
    [STUDIO_COMMAND_STEP_OUT] = { .id = "stepOut",
                                  .label = "Step Out",
                                  .chord = "Shift+F11",
                                  .context = "When paused",
                                  .icon = KIT_ICON_STEP_OUT,
                                  .repeat = true,
                                  .phase = SHORTCUT_DEBUG },
    /* The Scene's tools, on Unity's and Godot's keys, and Blender's G. */
    [STUDIO_COMMAND_SELECT_TOOL] = SCENE_TOOL("selectTool", "Select", "Q", "pick cursor", KIT_ICON_SELECT),
    [STUDIO_COMMAND_MOVE_TOOL] = SCENE_TOOL("moveTool", "Move", "W", "translate position gizmo arrows", KIT_ICON_MOVE),
    [STUDIO_COMMAND_ROTATE_TOOL] = SCENE_TOOL("rotateTool", "Rotate", "E", "orientation angle turn gizmo ring", KIT_ICON_ROTATE),
    [STUDIO_COMMAND_SCALE_TOOL] = SCENE_TOOL("scaleTool", "Scale", "R", "resize size width height handles gizmo", KIT_ICON_SCALE),
    [STUDIO_COMMAND_GRAB] = { .id = "grab",
                              .label = "Grab",
                              .chord = "G",
                              .context = "Scene",
                              .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),
                              .keywords = "move modal translate follow pointer",
                              .icon = KIT_ICON_PAN,
                              .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_TOGGLE_SNAP] = { .id = "toggleSnap",
                                     .label = "Snap to Grid",
                                     .menu = "Snap",
                                     .chord = "Shift+G",
                                     .context = "Scene",
                                     .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),
                                     .keywords = "magnet increment align",
                                     .icon = KIT_ICON_MAGNET,
                                     .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_TOGGLE_GRID] = { .id = "toggleGrid",
                                     .label = "Show Grid",
                                     .menu = "Grid",
                                     .context = "Scene",
                                     .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),
                                     .keywords = "lines axes overlay",
                                     .icon = KIT_ICON_GRID,
                                     .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_FINER_GRID] = { .id = "finerGrid",
                                    .label = "Finer Grid Increment",
                                    .context = "Scene",
                                    .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),
                                    .keywords = "snap step smaller spacing",
                                    .icon = KIT_ICON_GRID,
                                    .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_COARSER_GRID] = { .id = "coarserGrid",
                                      .label = "Coarser Grid Increment",
                                      .context = "Scene",
                                      .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),
                                      .keywords = "snap step larger spacing",
                                      .icon = KIT_ICON_GRID,
                                      .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_SELECT_ALL] = { .id = "selectAll",
                                    .label = "Select All",
                                    .chord = "Mod+A",
                                    .context = "Scene, Outliner",
                                    .contexts = OBJECT_AREAS,
                                    .keywords = "objects every",
                                    .icon = KIT_ICON_SELECT,
                                    .phase = SHORTCUT_EDITOR },
    /* Escape deselects, as Unreal's viewport and Godot's editors do. */
    [STUDIO_COMMAND_DESELECT] = { .id = "deselect",
                                  .label = "Deselect All",
                                  .chord = "Escape",
                                  .context = "Scene, Outliner",
                                  .contexts = OBJECT_AREAS,
                                  .keywords = "clear selection none",
                                  .phase = SHORTCUT_EDITOR },
    /* Blender's View All. */
    [STUDIO_COMMAND_FRAME_ALL] = { .id = "frameAll",
                                   .label = "Frame All",
                                   .chord = "Home",
                                   .context = "Scene",
                                   .contexts = EDITOR_BIT(STUDIO_EDITOR_SCENE),
                                   .keywords = "view fit everything zoom",
                                   .icon = KIT_ICON_FRAME,
                                   .phase = SHORTCUT_EDITOR },
    /* The area's own search: the graph's and the Text Editor's find
       widgets, else its filter field. */
    [STUDIO_COMMAND_FIND] = { .id = "find",
                              .label = "Find",
                              .chord = "Mod+F",
                              .context = "Area under pointer",
                              .contexts = FIND_AREAS,
                              .keywords = "search filter",
                              .icon = KIT_ICON_SEARCH,
                              .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_KEYBOARD_SHORTCUTS] = { .id = "keyboardShortcuts",
                                            .label = "Keyboard Shortcuts",
                                            .chord = "Mod+/",
                                            .context = "Global",
                                            .keywords = "keys keymap hotkeys "
                                                        "bindings cheat sheet "
                                                        "help",
                                            .icon = KIT_ICON_KEYBOARD,
                                            .phase = SHORTCUT_EARLY },
    /* The object clipboard, as Unity's and Godot's; a focused field keeps
       these chords for its text. */
    [STUDIO_COMMAND_COPY] = { .id = "copy",
                              .label = "Copy",
                              .chord = "Mod+C",
                              .context = "Scene, Outliner",
                              .contexts = OBJECT_AREAS,
                              .keywords = "clipboard objects",
                              .icon = KIT_ICON_CLIPBOARD,
                              .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_CUT] = { .id = "cut",
                             .label = "Cut",
                             .chord = "Mod+X",
                             .context = "Scene, Outliner",
                             .contexts = OBJECT_AREAS,
                             .keywords = "clipboard objects remove",
                             .icon = KIT_ICON_CUT,
                             .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_PASTE] = { .id = "paste",
                               .label = "Paste",
                               .chord = "Mod+V",
                               .context = "Scene, Outliner",
                               .contexts = OBJECT_AREAS,
                               .keywords = "clipboard objects insert",
                               .icon = KIT_ICON_PASTE,
                               .phase = SHORTCUT_EDITOR },
    /* Shift+arrows extend a list's selection, as Finder's and VS Code's. */
    [STUDIO_COMMAND_EXTEND_PREVIOUS] = { .id = "extendPrevious",
                                         .label = "Extend Selection Up",
                                         .chord = "Shift+Up",
                                         .context = "Outliner",
                                         .contexts = EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                         .keywords = "range multiple",
                                         .repeat = true,
                                         .phase = SHORTCUT_EDITOR },
    [STUDIO_COMMAND_EXTEND_NEXT] = { .id = "extendNext",
                                     .label = "Extend Selection Down",
                                     .chord = "Shift+Down",
                                     .context = "Outliner",
                                     .contexts = EDITOR_BIT(STUDIO_EDITOR_OUTLINER),
                                     .keywords = "range multiple",
                                     .repeat = true,
                                     .phase = SHORTCUT_EDITOR },
    /* Unreal's watches, on every data output at once. */
    [STUDIO_COMMAND_SHOW_LIVE_VALUES] = { .id = "showLiveValues",
                                          .label = "Show Live Values",
                                          .menu = "Live Values",
                                          .context = "Global",
                                          .keywords = "graph debug watch "
                                                      "values chips running "
                                                      "inspect",
                                          .icon = KIT_ICON_EYE,
                                          .phase = SHORTCUT_EDITOR },
    /* Playback speed on UI Scale's keys with Alt: Mod chords reach Studio
       while the game has the keyboard. */
    [STUDIO_COMMAND_SLOWER] = { .id = "slower",
                                .label = "Slower",
                                .chord = "Mod+Alt+-",
                                .context = "Global",
                                .keywords = "playback speed slow motion time "
                                            "scale slomo",
                                .icon = KIT_ICON_SLOWER,
                                .repeat = true,
                                .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_FASTER] = { .id = "faster",
                                .label = "Faster",
                                .chord = "Mod+Alt+=",
                                .context = "Global",
                                .keywords = "playback speed fast forward time "
                                            "scale",
                                .icon = KIT_ICON_FASTER,
                                .repeat = true,
                                .phase = SHORTCUT_EARLY },
    [STUDIO_COMMAND_NORMAL_SPEED] = { .id = "normalSpeed",
                                      .label = "Normal Speed",
                                      .chord = "Mod+Alt+0",
                                      .context = "Global",
                                      .keywords = "playback speed reset time "
                                                  "scale 1x real time",
                                      .phase = SHORTCUT_EARLY },
    /* Unity's Overrides ▸ Apply and Unreal's Apply Instance Changes to
       Blueprint. */
    [STUDIO_COMMAND_APPLY_OVERRIDES] = { .id = "applyOverrides",
                                         .label = "Apply Overrides to "
                                                  "Prototype\xe2\x80\xa6",
                                         .menu = "Apply Overrides\xe2\x80\xa6",
                                         .context = "Global",
                                         .keywords = "prefab instance push "
                                                     "defaults",
                                         .icon = KIT_ICON_PROTOTYPE,
                                         .phase = SHORTCUT_EDITOR },
    /* Unity's new prefab from an object, Godot's Save Branch as Scene. */
    [STUDIO_COMMAND_SAVE_AS_PROTOTYPE] = { .id = "saveAsPrototype",
                                           .label = "Save as New "
                                                    "Prototype\xe2\x80\xa6",
                                           .context = "Global",
                                           .keywords = "prefab variant "
                                                       "instance create",
                                           .icon = KIT_ICON_ADD,
                                           .phase = SHORTCUT_EDITOR },
    /* Any project folder, through the platform's dialog. The browser keeps
       its projects itself. */
    [STUDIO_COMMAND_OPEN_PROJECT] = { .id = "openProject",
                                      .label = "Open Project\xe2\x80\xa6",
                                      .chord = "Mod+O",
                                      .context = "Global",
                                      .keywords = "folder load",
                                      .icon = KIT_ICON_FOLDER,
                                      .phase = SHORTCUT_EARLY },
    /* The window's close button, as a command: Studio asks first while edits
       are unsaved. The browser has no Quit. */
    [STUDIO_COMMAND_QUIT] = { .id = "quit",
                              .label = "Quit",
                              .chord = "Mod+Q",
                              .context = "Global",
                              .keywords = "exit close window",
                              .phase = SHORTCUT_EARLY },
};

const char *studio_command_default_chord(studio_command command)
{
    const char *chord = COMMANDS[command].chord;
    return chord ? chord : "";
}

const char *studio_command_chord(const studio_prefs *prefs, studio_command command)
{
    return prefs->shortcuts[command][0] ? prefs->shortcuts[command]
                                        : studio_command_default_chord(command);
}

const char *studio_command_context(studio_command command)
{
    const char *context = COMMANDS[command].context;
    return context ? context : "";
}

static uint32_t command_contexts(const command_entry *entry)
{
    uint32_t contexts = entry->contexts ? entry->contexts : EDITOR_ALL;
    return contexts & ~entry->excluded_contexts;
}

static bool contexts_overlap(const command_entry *a, const command_entry *b)
{
    return command_contexts(a) & command_contexts(b);
}

static bool chord_conflicts(const char *a, const char *b, bool mac)
{
    studio_key left, right;
    return studio_chord_parse(a, mac, &left) && studio_chord_parse(b, mac, &right) &&
           left.key == right.key && left.mod == right.mod;
}

static bool override_set(const studio_prefs *prefs, studio_command command)
{
    return prefs->shortcuts[command][0] != '\0';
}

static const char *default_binding(const command_entry *entry, int index)
{
    if (!index) {
        return entry->chord;
    }
    return entry->aliases[index - 1];
}

/* The current binding and, only while unmodified, its default aliases. */
static int binding_count(const studio_prefs *prefs, studio_command command)
{
    const command_entry *entry = &COMMANDS[command];
    if (override_set(prefs, command)) {
        return 1;
    }
    int count = entry->chord != NULL;
    while (count < 5 && entry->aliases[count - (entry->chord != NULL)]) {
        count++;
    }
    return count;
}

static const char *binding_at(const studio_prefs *prefs, studio_command command, int index)
{
    if (override_set(prefs, command)) {
        return index ? NULL : prefs->shortcuts[command];
    }
    return default_binding(&COMMANDS[command], index);
}

int studio_command_bindings(const studio_prefs *prefs, studio_command command, const char **out, int max)
{
    int count = 0;
    for (int i = 0; i < binding_count(prefs, command) && count < max; i++) {
        out[count++] = binding_at(prefs, command, i);
    }
    return count;
}

const char *studio_command_label(studio_command command)
{
    return COMMANDS[command].label;
}

const char *studio_command_group(studio_command command)
{
    const command_entry *entry = &COMMANDS[command];
    if (entry->phase == SHORTCUT_DEBUG) {
        return "Debugger";
    }
    /* Find acts in most areas, as the global commands do. */
    return entry->contexts && __builtin_popcount(entry->contexts) <= 2
             ? entry->context
             : "Global";
}

int studio_command_conflict(const studio_prefs *prefs, studio_command command, const char *chord, bool mac)
{
    if (!chord || !chord[0]) {
        return -1;
    }
    const command_entry *source = &COMMANDS[command];
    bool source_override = override_set(prefs, command);
    int source_count = source_override ||
                               strcmp(chord, studio_command_chord(prefs, command))
                         ? 1
                         : binding_count(prefs, command);
    if (!source_count) {
        source_count = 1;
    }
    for (int other = 0; other < STUDIO_COMMAND_COUNT; other++) {
        if (other == (int)command || COMMANDS[other].indexed ||
            !contexts_overlap(source, &COMMANDS[other])) {
            continue;
        }
        int other_count = binding_count(prefs, (studio_command)other);
        for (int source_index = 0; source_index < source_count; source_index++) {
            const char *source_binding = source_index
                                           ? binding_at(prefs, command, source_index)
                                           : chord;
            for (int candidate = 0; candidate < other_count; candidate++) {
                if (chord_conflicts(
                        source_binding,
                        binding_at(prefs, (studio_command)other, candidate),
                        mac
                    )) {
                    return other;
                }
            }
        }
    }
    return -1;
}

static bool command_context_matches(const command_entry *entry, studio_editor editor)
{
    if (entry->contexts && (editor >= STUDIO_EDITOR_COUNT ||
                            !(entry->contexts & EDITOR_BIT(editor)))) {
        return false;
    }
    return editor >= STUDIO_EDITOR_COUNT ||
           !(entry->excluded_contexts & EDITOR_BIT(editor));
}

static bool command_binding_matches(
    const studio_prefs *prefs,
    studio_command command,
    SDL_Keycode key,
    SDL_Keymod mod,
    bool mac
)
{
    int count = binding_count(prefs, command);
    for (int i = 0; i < count; i++) {
        if (studio_chord_matches(binding_at(prefs, command, i), key, mod, mac)) {
            return true;
        }
    }
    return false;
}

int studio_command_for_key(
    const studio_prefs *prefs,
    SDL_Keycode key,
    SDL_Keymod mod,
    studio_editor editor,
    bool mac
)
{
    /* A user's binding wins over another command's default in a collision. */
    for (int overrides = 1; overrides >= 0; overrides--) {
        for (int phase = SHORTCUT_DEBUG; phase <= SHORTCUT_EDITOR; phase++) {
            for (int command = 0; command < STUDIO_COMMAND_COUNT; command++) {
                const command_entry *entry = &COMMANDS[command];
                if (entry->phase != phase || entry->indexed ||
                    override_set(prefs, (studio_command)command) != overrides ||
                    !command_context_matches(entry, editor)) {
                    continue;
                }
                if (command_binding_matches(prefs, (studio_command)command, key, mod, mac)) {
                    return command;
                }
            }
        }
    }
    return -1;
}

studio_command_info studio_command_describe(studio *s, studio_command command)
{
    const command_entry *e = &COMMANDS[command];
    const char *chord = studio_command_chord(&s->prefs, command);
    studio_command_info info = {
        .id = e->id,
        .label = e->label,
        .menu = e->menu ? e->menu : e->label,
        .shortcut = chord[0] ? studio_chord(s, chord) : NULL,
        .keywords = e->keywords,
        .icon = e->icon,
        .indexed = e->indexed,
    };
    switch (command) {
        case STUDIO_COMMAND_TOGGLE_PIN_TAB:
            info.label = info.menu = s->scene->active_tab >= 0 &&
                                             s->scene->tabs[s->scene->active_tab]
                                                 .pinned
                                       ? "Unpin Tab"
                                       : "Pin Tab";
            break;
        case STUDIO_COMMAND_PLAY:
            /* Play resumes a run paused at a breakpoint (Unreal). */
            if (s->pause.active) {
                info.label = info.menu = "Resume";
                info.icon = KIT_ICON_CONTINUE;
            } else if (s->guest) {
                info.label = info.menu = "Stop Studio";
            } else if (app_scene_authoring_runtime_state(s->scene).playing) {
                info.label = info.menu = "Pause";
                info.icon = KIT_ICON_PAUSE;
            }
            break;
        case STUDIO_COMMAND_RECORD_GAMEPLAY:
            if (app_scenario_recording(s->scene)) {
                info.label = info.menu = "Stop Recording";
                info.icon = KIT_ICON_STOP;
            }
            break;
        case STUDIO_COMMAND_DENSITY:
            info.label = info.menu = s->prefs.comfortable ? "Compact Density"
                                                          : e->label;
            break;
        case STUDIO_COMMAND_MOTION:
            info.label = info.menu = s->prefs.reduced_motion ? "Full Motion" : e->label;
            break;
        case STUDIO_COMMAND_THEME:
            info.label = info.menu = s->light ? "Dark Theme" : e->label;
            info.icon = s->light ? KIT_ICON_MOON : e->icon;
            break;
        default: break;
    }
    return info;
}

static bool drawer_showing(const studio *s, studio_editor editor)
{
    const studio_layout *layout = &s->prefs.layouts[s->prefs.workspace];
    return layout->drawer_open && layout->editors[STUDIO_AREA_DRAWER] == editor;
}

int studio_command_checked(studio *s, studio_command command)
{
    switch (command) {
        case STUDIO_COMMAND_TOGGLE_PIN_TAB:
            return s->scene->active_tab >= 0 &&
                   s->scene->tabs[s->scene->active_tab].pinned;
        case STUDIO_COMMAND_TOGGLE_ASSETS:
            return drawer_showing(s, STUDIO_EDITOR_ASSETS);
        case STUDIO_COMMAND_TOGGLE_OUTPUT:
            return drawer_showing(s, STUDIO_EDITOR_OUTPUT);
        case STUDIO_COMMAND_TOGGLE_PERFORMANCE:
            return drawer_showing(s, STUDIO_EDITOR_PERFORMANCE);
        case STUDIO_COMMAND_MAXIMIZE: return s->maximized >= 0;
        case STUDIO_COMMAND_INSPECT: return studio_inspect_picking(s);
        case STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT:
            return s->prefs.workspace == STUDIO_WORKSPACE_VERTICAL_SPLIT;
        case STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT:
            return s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
        case STUDIO_COMMAND_TOGGLE_SIDE_PANE:
            return studio_active_layout(s)->side_open;
        case STUDIO_COMMAND_SELECT_TOOL:
        case STUDIO_COMMAND_MOVE_TOOL:
        case STUDIO_COMMAND_ROTATE_TOOL:
        case STUDIO_COMMAND_SCALE_TOOL:
            return s->prefs.tool == (scene_tool)(command - STUDIO_COMMAND_SELECT_TOOL);
        case STUDIO_COMMAND_TOGGLE_SNAP: return s->prefs.scene_snap;
        case STUDIO_COMMAND_TOGGLE_GRID: return s->prefs.scene_grid;
        case STUDIO_COMMAND_SHOW_WORLD: return s->prefs.scene_world;
        case STUDIO_COMMAND_SHOW_LIGHTING: return s->prefs.scene_lighting;
        case STUDIO_COMMAND_SHOW_FOG: return s->prefs.scene_fog;
        case STUDIO_COMMAND_SHOW_CAMERAS: return s->prefs.scene_cameras;
        case STUDIO_COMMAND_SHOW_LIVE_VALUES: return s->prefs.live_values;
        case STUDIO_COMMAND_VIEW_THROUGH_CAMERA:
            return app_scene_authoring_viewing_through_camera(s->scene);
        default: return -1;
    }
}

/* The tab `argument` names, else the active one. */
static int tab_at(const studio *s, int argument)
{
    return argument >= 0 ? argument : s->scene->active_tab;
}

int studio_command_area(const studio *s)
{
    return s->key_area >= 0 ? s->key_area : s->focus_area;
}

/* The editor in the area `argument` names, else in the command's area. */
static studio_editor editor_at(studio *s, int argument)
{
    int area = argument >= 0 ? argument : studio_command_area(s);
    return area >= 0 && area < STUDIO_AREA_COUNT
             ? studio_active_layout(s)->editors[area]
             : STUDIO_EDITOR_COUNT;
}

static bool finds(studio *s, studio_editor editor)
{
    if (editor == STUDIO_EDITOR_BEHAVIOR || editor == STUDIO_EDITOR_MEMBERS) {
        return studio_graph_active(s);
    }
    return editor < STUDIO_EDITOR_COUNT && (FIND_AREAS & EDITOR_BIT(editor));
}

/* The graph's and the Text Editor's find widgets open; elsewhere the area's
   filter field takes focus when it is next emitted. */
static void find(studio *s, studio_editor editor)
{
    switch (editor) {
        case STUDIO_EDITOR_BEHAVIOR: studio_graph_find(s); break;
        case STUDIO_EDITOR_CODE: studio_code_find(s); break;
        default:
            s->find_request = editor + 1;
            s->ui_schedule.wake = true;
            break;
    }
}

/* Up and Down walk the Outliner's visible rows (Blender); Shift extends the
   selection from its anchor. */
void studio_step_selection(studio *s, int direction, bool extend)
{
    int *rows = studio_outliner_rows(s);
    int count = acount(rows), at = -1;
    for (int r = 0; r < count; r++) {
        at = rows[r] == s->scene->selected ? r : at;
    }
    int next = at < 0 ? (direction > 0 ? 0 : count - 1) : at + direction;
    if (count && next >= 0 && next < count) {
        finish_transform_edit(s->scene, false);
        if (extend) {
            studio_select_range(s, rows, count, next);
        } else {
            select_instance(s->scene, rows[next]);
        }
        studio_reveal_selection(s);
    }
    afree(rows);
}

/* Whether the command runs on `argument`, a tab or an area, or on the active
 * one for -1. */
static bool enabled_with(studio *s, studio_command command, int argument)
{
    bool editable = studio_editable(s);
    bool selection = selected_actor(s->scene) != NULL;
    switch (command) {
        case STUDIO_COMMAND_MAXIMIZE: return argument < STUDIO_AREA_COUNT;
        case STUDIO_COMMAND_SELECT_TAB:
            return argument >= 0 && argument < acount(s->scene->tabs) &&
                   argument != s->scene->active_tab;
        case STUDIO_COMMAND_TOGGLE_PIN_TAB:
            return tab_at(s, argument) >= 0 &&
                   tab_at(s, argument) < acount(s->scene->tabs);
        case STUDIO_COMMAND_PREVIOUS_TAB:
        case STUDIO_COMMAND_NEXT_TAB: return acount(s->scene->tabs) > 1;
        case STUDIO_COMMAND_CLOSE_TAB:
            return studio_tab_closes(s, tab_at(s, argument));
        case STUDIO_COMMAND_CLOSE_DRAWER:
            return studio_active_layout(s)->drawer_open;
        case STUDIO_COMMAND_SAVE: return studio_active_dirty(s);
        case STUDIO_COMMAND_RELOAD: return editable;
#ifdef __EMSCRIPTEN__
        case STUDIO_COMMAND_OPEN_PROJECT:
        case STUDIO_COMMAND_QUIT: return false;
#else
        case STUDIO_COMMAND_OPEN_PROJECT:
        case STUDIO_COMMAND_QUIT: return !s->hosted;
#endif
        case STUDIO_COMMAND_UNDO:
            return editable && studio_can_replay(s, false);
        case STUDIO_COMMAND_REDO: return editable && studio_can_replay(s, true);
        case STUDIO_COMMAND_DUPLICATE:
        /* X and Delete constrain or cancel a running gizmo instead. */
        case STUDIO_COMMAND_DELETE:
            return editable && selection && !s->scene->gizmo.active;
        case STUDIO_COMMAND_CUT:
            return editable && selection && !s->scene->gizmo.active &&
                   !studio_designer_active(s);
        case STUDIO_COMMAND_COPY:
            return selection && !studio_designer_active(s);
        case STUDIO_COMMAND_PASTE:
            return editable && !s->scene->gizmo.active && !studio_designer_active(s);
        case STUDIO_COMMAND_OPEN_PROTOTYPE:
        case STUDIO_COMMAND_FRAME: return selection;
        case STUDIO_COMMAND_APPLY_OVERRIDES: {
            scene_actor *actor = studio_prototype_action_actor(s);
            game_prototype_field *overrides = actor ? game_prototype_instance_overrides(
                                                          actor->base,
                                                          s->scene->world,
                                                          actor->entity
                                                      )
                                                    : NULL;
            bool any = acount(overrides) > 0;
            afree(overrides);
            return any;
        }
        case STUDIO_COMMAND_SAVE_AS_PROTOTYPE:
            return studio_prototype_action_actor(s) != NULL;
        case STUDIO_COMMAND_HIDE_SELECTED:
            return selection && !studio_designer_active(s);
        case STUDIO_COMMAND_HIDE_UNSELECTED:
            return acount(s->scene->actors) && !studio_designer_active(s);
        case STUDIO_COMMAND_SHOW_ALL: return studio_hidden_count(s) > 0;
        case STUDIO_COMMAND_ZOOM_IN:
            return s->prefs.ui_scale < STUDIO_UI_SCALE_MAX;
        case STUDIO_COMMAND_ZOOM_OUT:
            return s->prefs.ui_scale > STUDIO_UI_SCALE_MIN;
        case STUDIO_COMMAND_ZOOM_RESET: return s->prefs.ui_scale != 1;
        case STUDIO_COMMAND_PLAY: return !s->hosted;
        case STUDIO_COMMAND_STOP: return !s->hosted && studio_session(s);
        case STUDIO_COMMAND_STEP:
            return !s->hosted && !s->guest && !s->pause.active;
        case STUDIO_COMMAND_SLOWER:
            return !s->hosted &&
                   app_scene_authoring_time_scale(s->scene) > STUDIO_SPEED_MIN;
        case STUDIO_COMMAND_FASTER:
            return !s->hosted &&
                   app_scene_authoring_time_scale(s->scene) < STUDIO_SPEED_MAX;
        case STUDIO_COMMAND_NORMAL_SPEED:
            return !s->hosted && app_scene_authoring_time_scale(s->scene) != 1;
        case STUDIO_COMMAND_COLLABORATION_HOST:
        case STUDIO_COMMAND_COLLABORATION_JOIN:
            return studio_collab_available() && !collab_active() &&
                   !studio_collab_busy(s);
        case STUDIO_COMMAND_COLLABORATION_TOKEN: return !studio_collab_busy(s);
        case STUDIO_COMMAND_COLLABORATION_FOLLOW:
            return argument >= 0 && argument < acount(s->collaboration.peers);
        case STUDIO_COMMAND_COLLABORATION_UNFOLLOW:
            return s->collaboration.following;
        case STUDIO_COMMAND_COLLABORATION_LEAVE:
            return studio_collab_available() && collab_active();
        case STUDIO_COMMAND_COLLABORATION_INVITE_VIEWER:
        case STUDIO_COMMAND_COLLABORATION_INVITE_EDITOR:
            return studio_collab_available() && collab_active() && collab_is_host();
        case STUDIO_COMMAND_RECORD_GAMEPLAY:
            return !s->hosted && !s->guest && s->scene->scene_path;
        case STUDIO_COMMAND_EXPORT_GAME: return studio_export_available(s);
        case STUDIO_COMMAND_RENAME:
            return editable && selection && !studio_designer_active(s);
        case STUDIO_COMMAND_ADD_COMPONENT:
            return editable && studio_can_add_component(s);
        case STUDIO_COMMAND_KEEP_CHANGES:
            return studio_live(s) && selection &&
                   selected_scene_entity(s->scene).slot;
        case STUDIO_COMMAND_RESUME:
        case STUDIO_COMMAND_STEP_OVER:
        case STUDIO_COMMAND_STEP_INTO:
        case STUDIO_COMMAND_STEP_OUT: return s->pause.active;
        case STUDIO_COMMAND_GRAB:
            return editable && selection && !studio_previewed_ui(s) &&
                   !s->scene->gizmo.active;
        case STUDIO_COMMAND_FINER_GRID:
            return studio_grid_increment(s, -1) != s->prefs.scene_increment;
        case STUDIO_COMMAND_COARSER_GRID:
            return studio_grid_increment(s, 1) != s->prefs.scene_increment;
        case STUDIO_COMMAND_VIEW_THROUGH_CAMERA: {
            engine_viewport_view view;
            return editable && app_scene_authoring_camera_framing(s->scene, &view);
        }
        case STUDIO_COMMAND_SELECT_ALL:
            return acount(s->scene->actors) && !studio_designer_active(s);
        case STUDIO_COMMAND_DESELECT:
            /* Not while a press drags, which Escape cancels instead. */
            /* Escape cancels a gizmo and leaves View Through Camera first. */
            return (selection || s->scene->runtime_selected.slot) && !s->in.down[0] &&
                   !studio_designer_active(s) && !s->scene->gizmo.active &&
                   !app_scene_authoring_viewing_through_camera(s->scene);
        case STUDIO_COMMAND_FRAME_ALL: return !studio_designer_active(s);
        case STUDIO_COMMAND_FIND: return finds(s, editor_at(s, argument));
        default: return true;
    }
}

bool studio_command_enabled(studio *s, studio_command command)
{
    return enabled_with(s, command, -1);
}

void studio_command_run(studio *s, studio_command command)
{
    studio_command_run_with(s, command, -1);
}

void studio_command_run_with(studio *s, studio_command command, int argument)
{
    if (!enabled_with(s, command, argument)) {
        return;
    }
    switch (command) {
        case STUDIO_COMMAND_SEARCH:
            studio_palette_open(s, STUDIO_PALETTE_SEARCH, STUDIO_PALETTE_PLACEHOLDER);
            break;
        case STUDIO_COMMAND_SETTINGS: studio_ui_show(s, "Settings"); break;
        case STUDIO_COMMAND_COLLABORATION:
            studio_ui_show(s, "Collaboration");
            break;
        case STUDIO_COMMAND_COLLABORATION_TOKEN:
            s->collaboration.admin_token_expanded = !s->collaboration.admin_token_expanded;
            break;
        case STUDIO_COMMAND_COLLABORATION_FOLLOW:
            studio_collab_follow(s, argument);
            break;
        case STUDIO_COMMAND_COLLABORATION_UNFOLLOW:
            studio_collab_unfollow(s);
            break;
        case STUDIO_COMMAND_COLLABORATION_HOST:
        case STUDIO_COMMAND_COLLABORATION_JOIN:
        case STUDIO_COMMAND_COLLABORATION_LEAVE:
        case STUDIO_COMMAND_COLLABORATION_INVITE_VIEWER:
        case STUDIO_COMMAND_COLLABORATION_INVITE_EDITOR:
            studio_ui_collaboration_action(s, command);
            break;
        case STUDIO_COMMAND_SAVE: studio_save(s); break;
        case STUDIO_COMMAND_RELOAD: studio_reload(s); break;
        case STUDIO_COMMAND_OPEN_PROJECT:
#ifndef __EMSCRIPTEN__
            studio_pick_project();
#endif
            break;
        case STUDIO_COMMAND_QUIT:
            SDL_PushEvent(&(SDL_Event){ .type = SDL_EVENT_QUIT });
            break;
        case STUDIO_COMMAND_UNDO: studio_history(s, false); break;
        case STUDIO_COMMAND_REDO: studio_history(s, true); break;
        case STUDIO_COMMAND_DUPLICATE: studio_duplicate(s); break;
        case STUDIO_COMMAND_DELETE: studio_delete(s); break;
        case STUDIO_COMMAND_OPEN_PROTOTYPE:
            app_scene_authoring_open_prototype(
                s->scene,
                selected_actor(s->scene)->prototype_name
            );
            break;
        case STUDIO_COMMAND_FRAME: studio_frame_selection(s); break;
        case STUDIO_COMMAND_HIDE_SELECTED: studio_hide(s, true); break;
        case STUDIO_COMMAND_HIDE_UNSELECTED: studio_hide(s, false); break;
        case STUDIO_COMMAND_SHOW_ALL: studio_show_all(s); break;
        case STUDIO_COMMAND_RESET_CAMERA:
            engine_viewport_reset_camera(s->viewport);
            app_scene_authoring_framed(s->scene);
            break;
        case STUDIO_COMMAND_TOGGLE_ASSETS:
            studio_toggle_drawer(s, STUDIO_EDITOR_ASSETS);
            break;
        case STUDIO_COMMAND_TOGGLE_OUTPUT:
            studio_toggle_drawer(s, STUDIO_EDITOR_OUTPUT);
            break;
        case STUDIO_COMMAND_TOGGLE_PERFORMANCE:
            studio_toggle_drawer(s, STUDIO_EDITOR_PERFORMANCE);
            break;
        case STUDIO_COMMAND_MAXIMIZE:
            studio_toggle_maximized(s, argument >= 0 ? argument : studio_command_area(s));
            break;
        case STUDIO_COMMAND_RESET_LAYOUT: studio_reset_layout(s); break;
        case STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT:
            studio_set_workspace(s, STUDIO_WORKSPACE_VERTICAL_SPLIT);
            break;
        case STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT:
            studio_set_workspace(s, STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
            break;
        case STUDIO_COMMAND_TOGGLE_SIDE_PANE: studio_toggle_side_pane(s); break;
        case STUDIO_COMMAND_ZOOM_IN: studio_zoom(s, 1); break;
        case STUDIO_COMMAND_ZOOM_OUT: studio_zoom(s, -1); break;
        case STUDIO_COMMAND_ZOOM_RESET: studio_zoom(s, 0); break;
        case STUDIO_COMMAND_PLAY: studio_runtime(s, APP_RUNTIME_PLAY); break;
        case STUDIO_COMMAND_STOP: studio_runtime(s, APP_RUNTIME_STOP); break;
        case STUDIO_COMMAND_STEP: studio_runtime(s, APP_RUNTIME_STEP); break;
        case STUDIO_COMMAND_SLOWER: studio_speed_step(s, -1); break;
        case STUDIO_COMMAND_FASTER: studio_speed_step(s, 1); break;
        case STUDIO_COMMAND_NORMAL_SPEED: studio_speed_step(s, 0); break;
        case STUDIO_COMMAND_RECORD_GAMEPLAY: studio_record_gameplay(s); break;
        case STUDIO_COMMAND_EXPORT_GAME: studio_export_game(s); break;
        case STUDIO_COMMAND_SELECT_PREVIOUS:
            studio_step_selection(s, -1, false);
            break;
        case STUDIO_COMMAND_SELECT_NEXT:
            studio_step_selection(s, 1, false);
            break;
        case STUDIO_COMMAND_EXTEND_PREVIOUS:
            studio_step_selection(s, -1, true);
            break;
        case STUDIO_COMMAND_EXTEND_NEXT:
            studio_step_selection(s, 1, true);
            break;
        case STUDIO_COMMAND_RESUME: studio_debug(s, STUDIO_RESUME); break;
        case STUDIO_COMMAND_STEP_OVER: studio_debug(s, STUDIO_STEP_OVER); break;
        case STUDIO_COMMAND_STEP_INTO: studio_debug(s, STUDIO_STEP_INTO); break;
        case STUDIO_COMMAND_STEP_OUT: studio_debug(s, STUDIO_STEP_OUT); break;
        case STUDIO_COMMAND_DENSITY:
            s->prefs.comfortable = !s->prefs.comfortable;
            break;
        case STUDIO_COMMAND_MOTION:
            s->prefs.reduced_motion = !s->prefs.reduced_motion;
            break;
        case STUDIO_COMMAND_THEME:
            s->prefs.theme = s->light ? STUDIO_THEME_DARK : STUDIO_THEME_LIGHT;
            break;
        case STUDIO_COMMAND_SHOW_WORLD:
            s->prefs.scene_world = !s->prefs.scene_world;
            break;
        case STUDIO_COMMAND_SHOW_LIGHTING:
            s->prefs.scene_lighting = !s->prefs.scene_lighting;
            break;
        case STUDIO_COMMAND_SHOW_FOG:
            s->prefs.scene_fog = !s->prefs.scene_fog;
            break;
        case STUDIO_COMMAND_SHOW_CAMERAS:
            s->prefs.scene_cameras = !s->prefs.scene_cameras;
            break;
        case STUDIO_COMMAND_SHOW_LIVE_VALUES:
            s->prefs.live_values = !s->prefs.live_values;
            break;
        case STUDIO_COMMAND_VIEW_THROUGH_CAMERA:
            app_scene_authoring_view_through_camera(
                s->scene,
                !app_scene_authoring_viewing_through_camera(s->scene)
            );
            break;
        case STUDIO_COMMAND_INSPECT: studio_inspect_toggle(s); break;
        case STUDIO_COMMAND_PREVIEW_UI:
            studio_pick_open(
                s,
                STUDIO_PICK_PREVIEW,
                studio_previewed_ui(s),
                &(studio_pick_desc){ .kind = REFERENCE_ASSET_UI },
                NULL
            );
            break;
        case STUDIO_COMMAND_NEW_SCENE:
        case STUDIO_COMMAND_NEW_PROTOTYPE:
        case STUDIO_COMMAND_NEW_BEHAVIOR:
        case STUDIO_COMMAND_NEW_EVENT:
        case STUDIO_COMMAND_NEW_SYSTEM:
        case STUDIO_COMMAND_NEW_LIBRARY:
        case STUDIO_COMMAND_NEW_MATERIAL:
        case STUDIO_COMMAND_NEW_SHAPE:
        case STUDIO_COMMAND_NEW_FIELD:
        case STUDIO_COMMAND_NEW_SHADER_LIBRARY:
        case STUDIO_COMMAND_NEW_MODEL:
        case STUDIO_COMMAND_NEW_UI_WIDGET:
        case STUDIO_COMMAND_NEW_UI_DOCUMENT:
        case STUDIO_COMMAND_NEW_STYLESHEET:
            studio_new_asset_start(s, command - STUDIO_COMMAND_NEW_SCENE);
            break;
        case STUDIO_COMMAND_SELECT_TAB:
            finish_transform_edit(s->scene, false);
            scene_tabs_activate(s->scene, argument);
            break;
        case STUDIO_COMMAND_TOGGLE_PIN_TAB:
            scene_tabs_toggle_pin(s->scene, tab_at(s, argument));
            break;
        case STUDIO_COMMAND_PREVIOUS_TAB:
        case STUDIO_COMMAND_NEXT_TAB: {
            finish_transform_edit(s->scene, false);
            int count = acount(s->scene->tabs);
            int direction = command == STUDIO_COMMAND_NEXT_TAB ? 1 : -1;
            scene_tabs_activate(s->scene, (s->scene->active_tab + direction + count) % count);
            break;
        }
        case STUDIO_COMMAND_CLOSE_TAB:
            studio_close_tab(s, tab_at(s, argument));
            break;
        case STUDIO_COMMAND_CLOSE_DRAWER:
            studio_active_layout(s)->drawer_open = false;
            break;
        /* Rename objects and prototype components */
        case STUDIO_COMMAND_RENAME: studio_rename_object(s); break;
        case STUDIO_COMMAND_ADD_COMPONENT: studio_add_component_open(s); break;
        case STUDIO_COMMAND_SELECT_TOOL:
        case STUDIO_COMMAND_MOVE_TOOL:
        case STUDIO_COMMAND_ROTATE_TOOL:
        case STUDIO_COMMAND_SCALE_TOOL:
            s->prefs.tool = (scene_tool)(command - STUDIO_COMMAND_SELECT_TOOL);
            break;
        case STUDIO_COMMAND_GRAB: s->scene->gizmo.grab = true; break;
        case STUDIO_COMMAND_TOGGLE_SNAP:
            s->prefs.scene_snap = !s->prefs.scene_snap;
            break;
        case STUDIO_COMMAND_TOGGLE_GRID:
            s->prefs.scene_grid = !s->prefs.scene_grid;
            break;
        case STUDIO_COMMAND_FINER_GRID:
            s->prefs.scene_increment = studio_grid_increment(s, -1);
            break;
        case STUDIO_COMMAND_COARSER_GRID:
            s->prefs.scene_increment = studio_grid_increment(s, 1);
            break;
        case STUDIO_COMMAND_KEEP_CHANGES: studio_keep_changes(s, NULL); break;
        case STUDIO_COMMAND_APPLY_OVERRIDES:
        case STUDIO_COMMAND_SAVE_AS_PROTOTYPE:
            studio_prototype_action_open(s, command == STUDIO_COMMAND_SAVE_AS_PROTOTYPE);
            break;
        case STUDIO_COMMAND_SELECT_ALL: studio_select_all(s); break;
        case STUDIO_COMMAND_DESELECT: studio_deselect(s); break;
        case STUDIO_COMMAND_FRAME_ALL: studio_frame_all(s); break;
        case STUDIO_COMMAND_FIND: find(s, editor_at(s, argument)); break;
        case STUDIO_COMMAND_KEYBOARD_SHORTCUTS:
            studio_ui_show(s, "Shortcuts");
            break;
        case STUDIO_COMMAND_COPY: studio_copy(s); break;
        case STUDIO_COMMAND_CUT: studio_cut(s); break;
        case STUDIO_COMMAND_PASTE: studio_paste(s); break;
        case STUDIO_COMMAND_COUNT: break;
    }
}

const char *studio_command_id(studio_command command)
{
    return COMMANDS[command].id;
}

int studio_command_named(const char *id)
{
    for (int c = 0; c < STUDIO_COMMAND_COUNT; c++) {
        if (!strcmp(COMMANDS[c].id, id)) {
            return c;
        }
    }
    return -1;
}

bool studio_command_item(studio *s, studio_command command)
{
    studio_command_info info = studio_command_describe(s, command);
    kit_menu_item_desc d = { .label = info.menu,
                             .icon = info.icon,
                             .shortcut = info.shortcut,
                             .checked = studio_command_checked(s, command),
                             .disabled = !studio_command_enabled(s, command) };
    if (!kit_menu_item(s->kit, info.id, &d)) {
        return false;
    }
    studio_command_run(s, command);
    return true;
}

bool studio_text_key(SDL_Keycode key)
{
    switch (key) {
        case SDLK_A:
        case SDLK_C:
        case SDLK_V:
        case SDLK_X:
        case SDLK_Y:
        case SDLK_Z:
        case SDLK_LEFT:
        case SDLK_RIGHT:
        case SDLK_UP:
        case SDLK_DOWN:
        case SDLK_HOME:
        case SDLK_END:
        case SDLK_BACKSPACE:
        case SDLK_DELETE: return true;
        default: return false;
    }
}

/* Games bind no function key, so those reach Studio while a game has the
   keyboard: Stop, and the debugger's steps. */
static bool function_key(SDL_Keycode key)
{
    return key >= SDLK_F1 && key <= SDLK_F12;
}

void studio_shortcuts(studio *s, bool field)
{
    /* A modal move takes its keys (studio_scene_tools), and no others. */
    if (s->scene->gizmo.modal) {
        return;
    }
    /* Keys act on the area under the pointer, or on the one whose list the
       keyboard focused (Tab). */
    int area = studio_ui_keyboard_area(s);
    area = area >= 0 ? area : s->hover_area;
    studio_editor editor = area >= 0 ? studio_active_layout(s)->editors[area]
                                     : STUDIO_EDITOR_COUNT;
    /* A game in the focused viewport owns keys without its primary modifier. */
    bool gameplay = !studio_previewed_ui(s) && engine_viewport_focused(s->viewport);
    s->key_area = area;
    for (int i = 0; i < acount(s->keys); i++) {
        studio_key key = s->keys[i];
        bool primary = studio_primary(s, key.mod);
        int command = studio_command_for_key(&s->prefs, key.key, key.mod, editor, s->mac);
        bool tab_navigation = command == STUDIO_COMMAND_PREVIOUS_TAB ||
                              command == STUDIO_COMMAND_NEXT_TAB;
        /* A focused field keeps its keys; chords that edit no text still
           reach Studio, as VS Code's do. */
        if (field && !tab_navigation && (!primary || studio_text_key(key.key))) {
            continue;
        }
        bool repeatable = command >= 0
                            ? COMMANDS[command].repeat
                            : !primary &&
                                  (key.key == SDLK_UP || key.key == SDLK_DOWN ||
                                   key.key == SDLK_LEFT || key.key == SDLK_RIGHT);
        if (key.repeat && !repeatable) {
            continue;
        }
        if (command >= 0 && COMMANDS[command].phase == SHORTCUT_DEBUG &&
            s->pause.active) {
            studio_command_run(s, (studio_command)command);
            continue;
        }
        if (gameplay && !primary && !tab_navigation && !function_key(key.key)) {
            continue;
        }
        if (command >= 0 && COMMANDS[command].phase == SHORTCUT_EARLY) {
            studio_command_run(s, (studio_command)command);
            continue;
        }
        if (studio_designer_key(s, editor, key.key, key.mod)) {
            /* Designer keys precede Space, which carries a row in the Canvas. */
            continue;
        }
        if (command >= 0 && COMMANDS[command].phase == SHORTCUT_PLAY) {
            bool held = command == STUDIO_COMMAND_PLAY &&
                        editor == STUDIO_EDITOR_SCENE && studio_scene_space(s, key);
            if (!held) {
                studio_command_run(s, (studio_command)command);
            }
            continue;
        }
        if (editor == STUDIO_EDITOR_MEMBERS && studio_members_key(s, key.key, key.mod)) {
            continue;
        }
        if (editor == STUDIO_EDITOR_ASSETS && studio_assets_key(s, key.key, key.mod)) {
            continue;
        }
        if (studio_ui_area_key(s, area, key.key, key.mod)) {
            continue;
        }
        if (editor == STUDIO_EDITOR_OUTPUT && studio_output_key(s, key.key, key.mod)) {
            continue;
        }
        if (command >= 0 && COMMANDS[command].phase == SHORTCUT_EDITOR) {
            studio_command_run(s, (studio_command)command);
        }
    }
    s->key_area = -1;
}
