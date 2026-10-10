#define PICO_UNIT_IMPLEMENTATION
#include "assets/input_map.h"
#include "authoring/preview_source.h"
#include "authoring/source_presentation.h"
#include "editor/collab/collab.h"
#include "editor/documents/asset_code.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/asset_refactor.h"
#include "editor/workspace/scene_internal.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/local_edit.h"
#include "graph/nodes/core_nodes.h"
#include "graph/nodes/vector_nodes.h"
#include "support/array.h"
#include "support/file_io.h"
#include "support/xalloc.h"
#include "vendor/pico_unit.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

studio *studio_acceptance_studio(void *);
int studio_acceptance_host_main(int, char **);

static const char *project;
static const char *executable;
static int frames, edits, undos, redos, keys;
static bool callback_ok = true;
static enum {
    SOURCE_EDIT,
    CODE_KEYS,
    KDL_KEYS,
    EXPORT_GAME,
    GRAPH_VISIBILITY,
    GRAPH_KEYS,
    UNIT_WARNINGS,
    SCENE_HIERARCHY,
    ASSET_SELECTION,
    SCENE_VIEWPORT,
    PLAY_KEYS,
    PROMPTS
} mode;
static char exported[1024]; /* the export Studio reported */
static const char counter_bytes[] =
    "behavior Counter {\n"
    "    state count: int = 1\n"
    "    on Tick {\n"
    "        count = count + 1\n"
    "    }\n"
    "}\n";

static bool unchanged_source()
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Counter.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    char *text = file_read_text(path);
    REQUIRE(text);
    bool same = !strcmp(text, counter_bytes);
    free(text);
    REQUIRE(same);
    return true;
}

/* Counter's file holds `text`. */
static bool counter_says(const char *text)
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Counter.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    char *saved = file_read_text(path);
    REQUIRE(saved);
    bool found = strstr(saved, text) != NULL;
    free(saved);
    REQUIRE(found);
    return true;
}

static asset_code *counter_source(studio *s)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        asset_code *code = s->code.assets[i];
        const authoring_source *source = asset_code_source(code);
        if (source && source->valid && !strcmp(source->analysis->name, "Counter")) {
            return code;
        }
    }
    return NULL;
}

/* Check exact source, resolved initializer and current owned graph generation. */
static bool check_counter(studio *s, int expected, bool dirty, const node **literal)
{
    asset_code *code = counter_source(s);
    REQUIRE(code);
    const authoring_source *source = asset_code_source(code);
    REQUIRE(source && source->valid);
    const script_analysis *a = source->analysis;
    int expression = -1;
    for (int i = 0; i < a->symbol_count; i++) {
        if (a->symbols[i].kind == SCRIPT_STATE && !strcmp(a->symbols[i].name, "count")) {
            REQUIRE(expression == -1);
            expression = a->symbols[i].initializer;
        }
    }
    REQUIRE(expression >= 0 && expression < a->expression_count);
    const script_expression *value = &a->expressions[expression];
    const char *text = asset_code_text(code);
    bool actual_dirty = asset_code_dirty(code);
    REQUIRE(value->kind == SCRIPT_EXPR_CONSTANT);
    REQUIRE(value->constant.type == PIN_I32 && value->constant.i == expected);
    REQUIRE(value->range.end == value->range.start + 1);
    REQUIRE(text[value->range.start] == '0' + expected);
    editor *e = s->scene->graph_editor;
    const authoring_source_presentation *view = editor_presentation(e);
    REQUIRE(view);
    REQUIRE(!strcmp(authoring_source_presentation_text(view), text));
    REQUIRE(authoring_source_presentation_matches(view, a, asset_code_catalog(code)));
    const graph *g = editor_active_graph(e);
    REQUIRE(g);
    const node *found = NULL;
    for (int i = 0; i < authoring_source_presentation_origin_count(view); i++) {
        const source_presentation_origin *o = authoring_source_presentation_origin_at(view, i);
        if (o->pin || o->kind != SOURCE_PRESENTATION_EXPRESSION ||
            o->record != expression || o->graph != g->graph_id) {
            continue;
        }
        for (int j = 0; j < acount(g->nodes); j++) {
            if (g->nodes[j]->id == o->node) {
                REQUIRE(!found);
                found = g->nodes[j];
            }
        }
    }
    REQUIRE(found);
    char *shown = studio_code_expression(s, found);
    REQUIRE(shown);
    bool equal = shown[0] == '0' + expected && !shown[1];
    free(shown);
    REQUIRE(equal);
    REQUIRE(actual_dirty == dirty);
    if (literal) {
        *literal = found;
    }
    return true;
}

/* A full host frame consumes the edit/undo/redo overlay into runtime source. */
static bool counter_applied(studio *s)
{
    asset_code *code = counter_source(s);
    REQUIRE(code);
    const authoring_source
        *applied = game_preview_source_find(s->scene->live, asset_code_path(code), true);
    REQUIRE(applied && applied->valid);
    REQUIRE(!strcmp(applied->syntax->text, asset_code_text(code)));
    return true;
}

/* A changed input map republishes sources with its actions. */
static bool input_map_reloads(studio *s)
{
    char *path = content_path_join(s->scene->content_root, INPUT_MAP_FILE);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    fputs("Jump {\n    (array)sources \"Space\"\n}\n", file);
    fclose(file);
    app_disk_result result = app_scene_authoring_reload_path(s->scene, path);
    remove(path);
    free(path);
    REQUIRE(result == APP_DISK_RELOADED);
    const authoring_sources *sources = game_preview_sources_working(s->scene->live);
    REQUIRE(sources);
    const script_catalog *catalog = authoring_sources_catalog(sources);
    REQUIRE(script_catalog_find_capability(catalog, "Jump", SCRIPT_CAPABILITY_EVENT) >= 0);
    return true;
}

/* A report of what kept an edit from happening shows as a warning, and the
 * next report clears it; the scene's successes, such as opening Counter, do
 * not. */
static bool reports_warn(studio *s)
{
    REQUIRE(!strcmp(s->status, "Editing behavior asset Counter.") && !s->status_warning);
    studio_warn(s, "Could not duplicate");
    REQUIRE(s->status_warning && !strcmp(s->status, "Could not duplicate"));
    studio_report(s, "Duplicated");
    REQUIRE(!s->status_warning);
    return true;
}

/* With Counter unsaved, the title marks it and quitting asks once, in a toast
 * that a repeated request updates in place. */
static bool quit_asks(studio *s)
{
    REQUIRE(!strncmp(s->window_title, "\xe2\x97\x8f ", 4));
    REQUIRE(strstr(s->window_title, "Counter"));
    REQUIRE(!studio_quit(s, false) && !s->unsaved_prompt.toast);
    REQUIRE(!studio_quit(s, true) && s->unsaved_prompt.toast);
    REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_QUIT);
    int toast = s->unsaved_prompt.toast;
    REQUIRE(!studio_quit(s, true) && s->unsaved_prompt.toast == toast);
    return true;
}

/* The project remembered last comes first among the recent ones, once; a
 * folder without a manifest, or the open project itself, opens nothing. */
static bool recent_order(const char *first, const char *second)
{
    char **recent = studio_recent_projects();
    bool ordered = acount(recent) == 2 && !strcmp(recent[0], first) &&
                   !strcmp(recent[1], second);
    studio_recent_projects_free(recent);
    return ordered;
}

static bool projects_open(studio *s)
{
    studio_remember_project("/elsewhere");
    studio_remember_project(s->project_root);
    REQUIRE(recent_order(s->project_root, "/elsewhere"));
    studio_remember_project("/elsewhere");
    REQUIRE(recent_order("/elsewhere", s->project_root));
    char content[1024];
    int length = snprintf(content, sizeof content, "%s/content", s->project_root);
    REQUIRE(length > 0 && (size_t)length < sizeof content);
    studio_open_project_folder(s, content);
    studio_open_project_folder(s, s->project_root);
    REQUIRE(!s->open_project && !s->unsaved_prompt.toast);
    return true;
}

/* Shows Counter's default graph, which holds its state and so the count
   literal the edits change. */
static bool show_state_graph(studio *s)
{
    editor *e = s->scene->graph_editor;
    const graph *main = behavior_asset_find_graph(editor_behavior_asset(e), 0);
    REQUIRE(main && editor_open_graph(e, main->graph_id) == 0);
    return true;
}

/* Counter opens on its Tick handler, where its events are, rather than its
 * default graph, which holds only its state (Blueprint's event graph); the
 * edit below then goes to that default graph. */
static bool opens_on_handler(studio *s)
{
    const graph *shown = editor_active_graph(s->scene->graph_editor);
    REQUIRE(shown && shown->name && !strcmp(shown->name, "Tick"));
    return show_state_graph(s);
}

static bool source_edit_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 4);
    if (frames > 1) {
        REQUIRE(counter_applied(s));
    }
    if (frames == 1) {
        REQUIRE(projects_open(s));
        REQUIRE(reports_warn(s));
        REQUIRE(opens_on_handler(s));
        const node *literal = NULL;
        REQUIRE(check_counter(s, 1, false, &literal));
        s->prefs.code_apply_live = true;
        studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
        studio_show_editor(s, STUDIO_EDITOR_CODE);
        char error[256];
        bool changed = studio_code_expression_replace(s, literal, "2", error, sizeof error);
        if (!changed) {
            fprintf(stderr, "Studio edit acceptance: %s\n", error);
        }
        REQUIRE(changed);
        edits++;
    } else if (frames == 2) {
        REQUIRE(check_counter(s, 2, true, NULL));
        REQUIRE(studio_code_history(s, false, true));
        undos++;
        REQUIRE(check_counter(s, 1, false, NULL));
        /* Nothing unsaved: quitting needs no confirmation. */
        REQUIRE(studio_quit(s, true));
    } else if (frames == 3) {
        REQUIRE(check_counter(s, 1, false, NULL));
        REQUIRE(studio_code_history(s, true, true));
        redos++;
        REQUIRE(check_counter(s, 2, true, NULL));
    } else {
        REQUIRE(check_counter(s, 2, true, NULL));
        REQUIRE(studio_editor_shown(s, STUDIO_EDITOR_BEHAVIOR));
        REQUIRE(studio_editor_shown(s, STUDIO_EDITOR_CODE));
        REQUIRE(input_map_reloads(s));
        REQUIRE(quit_asks(s));
    }
    return true;
}

/* The Text Editor settles before the keys, which leave frames between them
   for the completion list to open, then comments a line, finds, rests the
   caret in another tab and steps a frame. */
#define KEY_FRAMES 38
#define KDL_FRAMES 14
#define TEXT(x) #x
#define FRAMES_TEXT(x) TEXT(x)

/* The window the queued events go to. */
static SDL_WindowID window_id()
{
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_WindowID window = count ? SDL_GetWindowID(windows[0]) : 0;
    SDL_free(windows);
    return window;
}

/* Queues a key press or release for the window, as the platform would. */
static bool push_key(SDL_Keycode key, SDL_Keymod mod, bool down)
{
    SDL_WindowID window = window_id();
    REQUIRE(window);
    SDL_Event e = { .key = { .type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP,
                             .windowID = window,
                             .key = key,
                             .mod = mod,
                             .scancode = SDL_GetScancodeFromKey(key, NULL),
                             .down = down } };
    REQUIRE(SDL_PushEvent(&e));
    return true;
}

/* Queues a key press and release for the window, as the platform would. */
static bool press_chord(SDL_Keycode key, SDL_Keymod mod)
{
    REQUIRE(push_key(key, mod, true));
    REQUIRE(push_key(key, mod, false));
    return true;
}

static bool press_key(SDL_Keycode key)
{
    return press_chord(key, SDL_KMOD_NONE);
}

/* Queues `text` as the platform's text input for the window. */
static bool type_text(const char *text)
{
    SDL_WindowID window = window_id();
    REQUIRE(window);
    SDL_Event e = {
        .text = { .type = SDL_EVENT_TEXT_INPUT, .windowID = window, .text = text }
    };
    REQUIRE(SDL_PushEvent(&e));
    return true;
}

/* Counter's source editor, whether or not its text currently reads. */
static asset_code *counter_code(studio *s)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        if (strstr(asset_code_path(s->code.assets[i]), "Counter.chibi")) {
            return s->code.assets[i];
        }
    }
    return NULL;
}

static bool same_without_space(const char *a, const char *b)
{
    for (;; a++, b++) {
        a += strspn(a, " \n");
        b += strspn(b, " \n");
        if (*a != *b) {
            return false;
        }
        if (!*a) {
            return true;
        }
    }
}

static const char commented_bytes[] = "behavior Counter {\n"
                                      "    // state count: int = 1\n"
                                      "    on Tick {\n"
                                      "        count = count + 1\n"
                                      "    }\n"
                                      "}\n";

/* The open scene's source editor. */
static asset_code *scene_code(studio *s)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        if (strstr(asset_code_path(s->code.assets[i]), "Start.kdl")) {
            return s->code.assets[i];
        }
    }
    return NULL;
}

/* The scene's KDL in the Text Editor settles once typed in, so Save writes
   it. */
static bool scene_saves(studio *s)
{
    asset_code *code = scene_code(s);
    REQUIRE(code && asset_code_dirty(code) && !asset_code_pending(code));
    studio_code_save(s);
    REQUIRE(!asset_code_dirty(code));
    char *disk = file_read_text(asset_code_path(code));
    REQUIRE(disk);
    bool saved = !strcmp(disk, asset_code_text(code));
    free(disk);
    REQUIRE(saved);
    return true;
}

/* The tab of `code`'s source. */
static int code_tab(studio *s, asset_code *code)
{
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        const char *path = s->scene->tabs[i].path;
        if (path && !strcmp(path, asset_code_path(code))) {
            return i;
        }
    }
    return -1;
}

/* The caret `code`'s tab keeps for a restored workspace. */
static int kept_cursor(studio *s, asset_code *code)
{
    int tab = code_tab(s, code);
    REQUIRE(tab >= 0 && s->scene->tabs[tab].source_state);
    return av_get_int(av_obj_get(s->scene->tabs[tab].source_state, "cursor"));
}

/* Whether the find field has the keyboard. */
static bool find_focused(studio *s)
{
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    return focus && !strcmp(rml_element_id(focus), "code-find-query");
}

/* The text selects `length` bytes from `start`. */
static bool selects(asset_code *code, int start, int length)
{
    int from, to;
    REQUIRE(kit_code_selection(asset_code_editor(code), &from, &to));
    REQUIRE(from == start && to == start + length);
    return true;
}

/* Enter on an empty line types newlines; it never accepts a completion the
   user did not ask for, and one undo takes back the burst. Mod+/ comments
   the caret's line as an undo step of its own, and uncomments it. F3 and
   Shift+F3 from the find field step through the counted matches, which do
   not overlap. A moved caret is kept in its own tab when another shows. */
static bool code_keys_frame(void *state)
{
    static int spaces, caret, other_tab;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= KEY_FRAMES);
    asset_code *code = counter_code(s);
    REQUIRE(code);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_CODE);
    } else if (frames == 4) {
        s->code.find.refocus = true;
        const char *close = strstr(asset_code_text(code), "    }\n}");
        REQUIRE(close);
        kit_code_set_cursor(asset_code_editor(code), (int)(close - asset_code_text(code)) + 5);
    } else if (frames == 6 || frames == 9) {
        REQUIRE(press_key(SDLK_RETURN));
        keys++;
    } else if (frames == 12) {
        const char *text = asset_code_text(code);
        REQUIRE(strlen(text) > strlen(counter_bytes) + 1);
        REQUIRE(same_without_space(text, counter_bytes));
        REQUIRE(asset_code_replay(code, false, false));
        REQUIRE(!strcmp(asset_code_text(code), counter_bytes));
        const char *state_line = strstr(asset_code_text(code), "state");
        REQUIRE(state_line);
        kit_code_set_cursor(asset_code_editor(code), (int)(state_line - asset_code_text(code)) + 2);
    } else if (frames == 14 || frames == 18) {
        REQUIRE(press_chord(SDLK_SLASH, SDL_KMOD_CTRL));
        keys++;
    } else if (frames == 17) {
        REQUIRE(!strcmp(asset_code_text(code), commented_bytes));
    } else if (frames == 21) {
        REQUIRE(!strcmp(asset_code_text(code), counter_bytes));
        REQUIRE(asset_code_replay(code, false, false));
        REQUIRE(!strcmp(asset_code_text(code), commented_bytes));
        /* Find seeds "  " from the first indent, where it overlaps. */
        spaces = (int)(strstr(commented_bytes, "    //") - commented_bytes);
        kit_code_select(asset_code_editor(code), spaces, spaces + 2);
        REQUIRE(press_chord(SDLK_F, SDL_KMOD_CTRL));
        keys++;
    } else if (frames == 24) {
        REQUIRE(s->code.find.open && !strcmp(s->code.find.query, "  "));
        REQUIRE(find_focused(s));
        REQUIRE(press_key(SDLK_F3));
        keys++;
    } else if (frames == 26) {
        REQUIRE(selects(code, spaces + 2, 2) && s->code.find.index == 1);
        REQUIRE(press_chord(SDLK_F3, SDL_KMOD_SHIFT));
        keys++;
    } else if (frames == 28) {
        REQUIRE(selects(code, spaces, 2) && s->code.find.index == 0);
        REQUIRE(press_key(SDLK_ESCAPE));
        keys++;
    } else if (frames == 30) {
        REQUIRE(!s->code.find.open);
        REQUIRE(press_key(SDLK_DOWN));
        keys++;
    } else if (frames == 32) {
        caret = kit_code_cursor(asset_code_editor(code));
        REQUIRE(s->code.capture_due && kept_cursor(s, code) != caret);
        other_tab = code_tab(s, code) ? 0 : 1;
        REQUIRE(other_tab < acount(s->scene->tabs));
        REQUIRE(scene_tabs_activate(s->scene, other_tab) == 0);
        /* The caret's rest ends as the other tab shows. */
        s->code.capture_due = s->time;
    } else if (frames == 34) {
        REQUIRE(s->scene->active_tab == other_tab && !s->code.capture_due);
        REQUIRE(kept_cursor(s, code) == caret);
        REQUIRE(scene_tabs_activate(s->scene, code_tab(s, code)) == 0);
        s->code.find.refocus = true;
    } else if (frames == 36) {
        /* F10 from the text steps a frame unless the debugger pauses. */
        REQUIRE(!studio_session(s) && !s->pause.active);
        REQUIRE(press_key(SDLK_F10));
        keys++;
    } else if (frames == KEY_FRAMES) {
        REQUIRE(
            studio_session(s) && !app_scene_authoring_runtime_state(s->scene).playing
        );
    }
    return true;
}

/* After Save, an Enter and a paste are undo steps of their own: Save ends
   the typing burst and a paste never joins one. */
static bool undoes_to_save(asset_code *code)
{
    char *disk = file_read_text(asset_code_path(code));
    REQUIRE(disk);
    size_t saved = strlen(disk);
    REQUIRE(strlen(asset_code_text(code)) == saved + 2);
    REQUIRE(asset_code_replay(code, false, false));
    bool enter = strlen(asset_code_text(code)) == saved + 1;
    REQUIRE(asset_code_replay(code, false, false));
    bool undone = !strcmp(asset_code_text(code), disk);
    free(disk);
    REQUIRE(enter && undone);
    return true;
}

/* Enter at the end of the open scene's KDL settles without waiting on a
   graph, so Save writes it. */
static bool kdl_keys_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    frames++;
    REQUIRE(frames <= KDL_FRAMES);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_CODE);
        return true;
    }
    asset_code *scene = scene_code(s);
    REQUIRE(scene);
    if (frames == 4) {
        s->code.find.refocus = true;
        kit_code_set_cursor(asset_code_editor(scene), (int)strlen(asset_code_text(scene)));
    } else if (frames == 6 || frames == 10) {
        REQUIRE(press_key(SDLK_RETURN));
        keys++;
    } else if (frames == 8) {
        REQUIRE(scene_saves(s));
    } else if (frames == 12) {
        kit_code_paste(asset_code_editor(scene), "x");
    } else if (frames == KDL_FRAMES) {
        REQUIRE(undoes_to_save(scene));
    }
    return true;
}

#define UNIT_FRAMES 4
static const char unit_mismatch[] = "behavior Counter {\n"
                                    "    state count: int = 1\n"
                                    "    on Tick {\n"
                                    "        count = count + 1\n"
                                    "        self.GameCollider.radius = "
                                    "self.GameMovement.speed\n"
                                    "    }\n"
                                    "}\n";
static bool unit_warned;

/* A unit mismatch underlines its statement as a warning, not an error. */
static bool unit_warnings_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= UNIT_FRAMES);
    asset_code *code = counter_code(s);
    REQUIRE(code);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_CODE);
    } else if (frames == 2) {
        const char *path = asset_code_path(code);
        REQUIRE(studio_code_replace(s, path, asset_code_text(code), unit_mismatch, NULL));
    } else {
        const char *statement = strstr(unit_mismatch, "self.GameCollider");
        const kit_code_marker *markers = asset_code_markers(code);
        for (int i = 0; i < acount(markers); i++) {
            const kit_code_marker *m = &markers[i];
            REQUIRE(m->severity == KIT_CODE_WARNING);
            REQUIRE(!strcmp(m->message, "Units differ: u = u/s") && m->line == 5);
            REQUIRE(m->start == statement - unit_mismatch);
            unit_warned = true;
        }
        REQUIRE(!asset_code_error(code)[0]);
    }
    return true;
}

static bool tilemap_viewport(studio *s)
{
    app_scene_authoring *scene = s->scene;
    ecs_world *world = scene->world;
    ecs_entity map = ecs_world_create_entity(world);
    ecs_entity foreground = ecs_world_create_entity(world);
    REQUIRE(GAME_GameTransform_add(world, map, 0) == ECS_OK);
    REQUIRE(GAME_GameOrientation_add(world, map, 0) == ECS_OK);
    REQUIRE(GAME_GameTilemap_add(world, map, 0) == ECS_OK);
    REQUIRE(GAME_GameTransform_add(world, foreground, 0) == ECS_OK);
    REQUIRE(GAME_GameAppearance_add(world, foreground, 0) == ECS_OK);
    REQUIRE(GAME_GameRenderState_add(world, foreground, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(world) == ECS_OK);
    *GAME_GameTransform_get(world, map) = (GameTransform){ 10, 20 };
    *GAME_GameOrientation_get(world, map) = (GameOrientation){ 1.5707963267948966f };
    *GAME_GameTilemap_get(world, map) = (GameTilemap){ .cells = "1 -1",
                                                       .columns = 2,
                                                       .rows = 1,
                                                       .frame_width = 8,
                                                       .frame_height = 4,
                                                       .layer = -50 };
    *GAME_GameTransform_get(world, foreground) = (GameTransform){ 8, 24 };
    *GAME_GameAppearance_get(world, foreground) = (GameAppearance){ .shape = GAME_SHAPE_BOX,
                                                                    .width = 2,
                                                                    .height = 2,
                                                                    .layer = 10 };
    int actor_count = acount(scene->actors);
    apush(scene->actors, ((scene_actor){ .entity = map }));
    apush(scene->actors, ((scene_actor){ .entity = foreground }));
    REQUIRE(s->viewport);
    engine_viewport_view before, fitted;
    engine_viewport_get_view(s->viewport, &before);
    bool fit = fit_scene_view(s->viewport, world, &map, 1);
    engine_viewport_get_view(s->viewport, &fitted);
    ecs_entity top = pick_scene_entity(scene, 8, -24, (render_skip){ 0 });
    GAME_GameRenderState_get(world, foreground)->hidden = true;
    ecs_entity layer = pick_scene_entity(scene, 8, -24, (render_skip){ 0 });
    ecs_entity empty_cell = pick_scene_entity(scene, 8, -32, (render_skip){ 0 });
    ecs_entity outside = pick_scene_entity(scene, 10.5f, -24, (render_skip){ 0 });
    engine_viewport_set_view(s->viewport, &before);
    asetlen(scene->actors, actor_count);
    REQUIRE(ecs_world_destroy_entity(world, foreground) == ECS_OK);
    REQUIRE(ecs_world_destroy_entity(world, map) == ECS_OK);
    REQUIRE(fit);
    REQUIRE(fabsf(fitted.center_x - 8) < 0.001f);
    REQUIRE(fabsf(fitted.center_y + 28) < 0.001f);
    REQUIRE(fitted.width >= 4 && fitted.height >= 16);
    REQUIRE(ecs_entity_to_u64(top) == ecs_entity_to_u64(foreground));
    REQUIRE(ecs_entity_to_u64(layer) == ecs_entity_to_u64(map));
    REQUIRE(ecs_entity_to_u64(empty_cell) == ecs_entity_to_u64(map));
    REQUIRE(ecs_entity_to_u64(outside) != ecs_entity_to_u64(map));
    return true;
}

/* Hidden graphs do no UI work. Both views follow one document, without
   losing the scene source draft selected between visits to the graph. */
static bool graph_visibility_frame(void *state)
{
    static rml_backend_stats hidden;
    static int scene_tab = -1;
    static char *draft;
    static const authoring_sources *sources;
    static const authoring_source_presentation *projection;
    static unsigned index_generation, index_revision;
    static rml_element *focus_document;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 10);
    if (frames == 1) {
        REQUIRE(opens_on_handler(s));
        studio_behavior_show(s, 2);
    } else if (frames == 2) {
        REQUIRE(tilemap_viewport(s));
    } else if (frames == 3) {
        REQUIRE(s->graph.backend);
        hidden = rml_backend_get_stats(s->graph.backend);
        REQUIRE(hidden.context_updates && hidden.render_captures);
        sources = game_preview_sources_working(s->scene->live);
        projection = s->scene->definition->working_presentation;
        index_generation = s->scene->asset_index_generation;
        index_revision = s->scene->asset_index_revision;
        rml_context *context = rml_backend_context(s->graph.backend);
        focus_document = rml_context_create_document(context, "acceptance-focus");
        REQUIRE(focus_document);
        rml_element_set_inner_rml(focus_document, "<input type='text' />");
        rml_document_show(focus_document, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
        rml_document_update(focus_document);
        rml_element *input = rml_element_query_selector(focus_document, "input");
        REQUIRE(input && rml_element_focus(input, true));
        REQUIRE(studio_graph_wants_keyboard(s) && studio_graph_accepts_text(s));
        studio_behavior_show(s, 1);
    } else if (frames == 4 || frames == 5 || frames == 10) {
        rml_backend_stats now = rml_backend_get_stats(s->graph.backend);
        REQUIRE(now.context_updates == hidden.context_updates);
        REQUIRE(now.render_captures == hidden.render_captures);
        REQUIRE(s->scene->debug_hidden);
        REQUIRE(!studio_graph_wants_keyboard(s) && !studio_graph_accepts_text(s));
        if (frames < 10) {
            REQUIRE(game_preview_sources_working(s->scene->live) == sources);
            REQUIRE(game_preview_sources_applied(s->scene->live) == sources);
            REQUIRE(s->scene->definition->working_presentation == projection);
            REQUIRE(s->scene->asset_index_generation == index_generation);
            REQUIRE(s->scene->asset_index_revision == index_revision);
        }
        if (frames == 5) {
            rml_document_close(focus_document);
            studio_behavior_show(s, 2);
        }
    } else if (frames == 7) {
        /* Frame 6 lets the retained workspace resolve the Both layout. */
        rml_backend_stats now = rml_backend_get_stats(s->graph.backend);
        REQUIRE(now.context_updates > hidden.context_updates);
        REQUIRE(now.render_captures > hidden.render_captures);
        for (int i = 0; i < acount(s->scene->tabs); i++) {
            if (s->scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE) {
                scene_tab = i;
                break;
            }
        }
        REQUIRE(scene_tab >= 0);
        REQUIRE(scene_tabs_activate(s->scene, scene_tab) == 0);
    } else if (frames == 8) {
        const char *path = s->scene->tabs[scene_tab].path;
        asset_code *code = NULL;
        for (int i = 0; i < acount(s->code.assets); i++) {
            if (!strcmp(asset_code_path(s->code.assets[i]), path)) {
                code = s->code.assets[i];
                break;
            }
        }
        REQUIRE(code);
        const char *before = asset_code_text(code);
        draft = xmalloc(strlen(before) + 2);
        snprintf(draft, strlen(before) + 2, "%s\n", before);
        REQUIRE(studio_code_replace(s, path, before, draft, NULL));
        REQUIRE(asset_code_dirty(code));
        REQUIRE(asset_code_can_replay(code, false));
        /* The accepted collaboration echo performs document and view changes
           together; an editor guest must not lose the requested Both view. */
        int counter = -1;
        for (int i = 0; i < acount(s->scene->tabs); i++) {
            if (s->scene->tabs[i].kind == APP_EDITOR_DOCUMENT_BEHAVIOR) {
                counter = i;
                break;
            }
        }
        REQUIRE(counter >= 0);
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "family", "command");
        yyjson_mut_obj_add_str(doc, root, "id", "behavior.view");
        yyjson_mut_obj_add_strcpy(doc, root, "tab", s->scene->tabs[counter].key);
        char *action = yyjson_mut_write(doc, 0, NULL);
        yyjson_mut_doc_free(doc);
        char error[256] = "";
        collab_begin_replay();
        bool applied = studio_collab_apply(s, action, error, sizeof error);
        collab_end_replay();
        free(action);
        REQUIRE(applied);
        REQUIRE(studio_behavior_view(s) == 2);
        REQUIRE(s->scene->active_tab != scene_tab);
        REQUIRE(scene_tabs_activate(s->scene, scene_tab) == 0);
        studio_behavior_show(s, 2);
        REQUIRE(s->scene->active_tab == counter);
        REQUIRE(studio_behavior_view(s) == 2);
    } else if (frames == 9) {
        REQUIRE(check_counter(s, 1, false, NULL));
        REQUIRE(s->code.editor_path[0]);
        REQUIRE(strstr(s->code.editor_path, "Counter.chibi"));
        REQUIRE(scene_tabs_activate(s->scene, scene_tab) == 0);
        const char *text = av_get_str(
            av_obj_get(s->scene->tabs[scene_tab].source_state, "text")
        );
        REQUIRE(text && !strcmp(text, draft));
        REQUIRE(s->scene->tabs[scene_tab].source_dirty);
        asset_code *code = NULL;
        for (int i = 0; i < acount(s->code.assets); i++) {
            if (!strcmp(
                    asset_code_path(s->code.assets[i]),
                    s->scene->tabs[scene_tab].path
                )) {
                code = s->code.assets[i];
                break;
            }
        }
        REQUIRE(code && asset_code_can_replay(code, false));
        REQUIRE(asset_code_replay(code, false, false));
        REQUIRE(strlen(asset_code_text(code)) + 1 == strlen(draft));
        REQUIRE(!strncmp(asset_code_text(code), draft, strlen(draft) - 1));
        REQUIRE(asset_code_can_replay(code, true));
        REQUIRE(asset_code_replay(code, true, false));
        REQUIRE(!strcmp(asset_code_text(code), draft));
        free(draft);
        draft = NULL;
        REQUIRE(s->graph.area >= 0);
        studio_active_layout(s)->editors[s->graph.area] = STUDIO_EDITOR_SCENE;
        s->ui_schedule.wake = true;
        hidden = rml_backend_get_stats(s->graph.backend);
    }
    return true;
}

#define GRAPH_KEY_FRAMES 25

/* A behavior whose first handler graph is an unnamed completion handler,
   whose entry is an Event node without a name, ahead of its Tick. */
static const char
    late_bytes[] = "behavior Late {\n"
                   "    macro Advance(elapsed: ref float, dt: double) {\n"
                   "        elapsed = cast<float>(cast<double>(elapsed) + dt)\n"
                   "    }\n"
                   "    state elapsed: float = 0.125\n"
                   "    action advance = Advance(elapsed, delta)\n"
                   "    after advance {\n"
                   "        elapsed = min(elapsed, 1.0)\n"
                   "    }\n"
                   "    on Tick {\n"
                   "        advance()\n"
                   "    }\n"
                   "}\n";

static bool write_late()
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Late.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fputs(late_bytes, file) >= 0;
    REQUIRE(fclose(file) == 0 && written);
    return true;
}

/* Late opens past its unnamed completion handler, on its Tick. */
static bool opens_past_unnamed_event(studio *s)
{
    REQUIRE(app_scene_authoring_open_behavior(s->scene, "Late") == 0);
    const graph *shown = editor_active_graph(s->scene->graph_editor);
    REQUIRE(shown && shown->name && !strcmp(shown->name, "Tick"));
    return true;
}

static bool selected_only(editor *e, uint64_t id)
{
    REQUIRE(acount(e->selected) == 1 && e->selected[0]->id == id);
    return true;
}

/* One node is selected, and it feeds the node with this id. */
static bool selected_feeds(editor *e, const graph *gr, uint64_t id)
{
    REQUIRE(acount(e->selected) == 1);
    for (int i = 0; i < acount(gr->edges); i++) {
        if (gr->edges[i].from_id == e->selected[0]->id && gr->edges[i].to_id == id) {
            return true;
        }
    }
    return false;
}

enum
{
    NAMED_SOURCE,
    NAMED_USE,
    VECTOR_USE,
    OTHER_GRAPH,
    DECLARED_COUNT
};

/* Opens a declarative graph, as a material's, of three nodes. The first
   feeds the second through a value named as Name Value does, so the second
   reads the name instead of drawing a wire; code can't name values this
   way. The third's vector input holds a literal a source can replace. A
   function graph beside it is another graph to switch to. */
static bool open_declared(editor *e, uint64_t *ids)
{
    behavior_asset *asset = behavior_asset_new("Declared");
    graph *gr = asset->graph;
    node *a = graph_add_node(gr, &NODE_Not), *b = graph_add_node(gr, &NODE_Not);
    node *vector = graph_add_node(gr, &NODE_Vector_Length);
    b->x = a->x + 400;
    vector->y = a->y + 300;
    REQUIRE(!graph_connect_data(gr, a, 0, b, 0));
    REQUIRE(graph_create_local(gr, a, 0, "named"));
    ids[NAMED_SOURCE] = a->id;
    ids[NAMED_USE] = b->id;
    ids[VECTOR_USE] = vector->id;
    ids[OTHER_GRAPH] = behavior_asset_add_graph(asset, GRAPH_FUNCTION, "Other")->graph_id;
    REQUIRE(!editor_declarative_graph(e, asset));
    behavior_asset_free(asset);
    return true;
}

/* The first element `selector` finds in Studio's documents, the kit's layer
   among them, or NULL. */
static rml_element *studio_element(studio *s, const char *selector)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_query_selector(rml_context_document(context, i), selector);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* Properties shows Bind to Source for the selected node's input, disabled
   exactly when the graph is read-only. */
static bool bind_disabled(studio *s, bool disabled)
{
    rml_element *bind = studio_element(s, ".graph-row button[data-tip='Bind to Source']");
    REQUIRE(bind);
    REQUIRE(rml_element_has_attribute(bind, "data-disabled") == disabled);
    return true;
}

/* Every output label a full node shows has room for its text: a label
   without a width once collapsed to nothing. */
static bool output_labels_show(studio *s, graph *gr)
{
    int shown = 0;
    for (int i = 0; i < acount(gr->nodes); i++) {
        node *n = gr->nodes[i];
        const node_layout *l = sg_node_visible(gr, n) ? sg_layout_of(s, n) : NULL;
        for (int p = 0; l && l->form == FORM_FULL && p < n->output_count; p++) {
            pin_parts parts;
            sg_pin_parts_of(s, n, ED_PIN_OUTPUT, p, &parts);
            if (!sg_pin_in_rows(gr, n, l, ED_PIN_OUTPUT, p) || !parts.show_label) {
                continue;
            }
            char row[16] = "", path[96];
            if (sg_pin_exec(n, ED_PIN_OUTPUT, p)) {
                snprintf(row, sizeof row, "/e%d", sg_pin_order(gr, n, l, ED_PIN_OUTPUT, p));
            }
            snprintf(
                path,
                sizeof path,
                "world/stage/n%llu/b%s/o%d/l",
                (unsigned long long)n->id,
                row,
                p
            );
            kit_rect r;
            REQUIRE(kit_find(s->graph.kit, path, &r) && r.w > 0);
            shown++;
        }
    }
    REQUIRE(shown > 0);
    return true;
}

/* The node chosen last leads node search under Recent, and stays there
   through saved preferences. */
static bool leads_search(studio *s, const char *key)
{
    studio_palette_entry *entries = NULL;
    studio_graph_palette_entries(s, "", &entries);
    REQUIRE(acount(entries) && !strcmp(entries[0].key, key));
    REQUIRE(!strcmp(entries[0].group, "Recent"));
    afree(entries);
    char *json = studio_prefs_write(&s->prefs, NULL);
    studio_prefs saved = studio_prefs_read(json);
    free(json);
    REQUIRE(!strcmp(saved.recent_nodes[0].key, key));
    REQUIRE(studio_prefs_equal(&saved, &s->prefs));
    return true;
}

/* Graph keys, through SDL as the platform sends them: Right walks the
   selection along a wire to the node it feeds and Left to the nearest node
   feeding that one, Escape at rest clears the selection, and M asks the
   source to disable the selection, which refuses and says why; with two
   nodes selected, M refuses before editing either. Escape closing the node
   menu keeps the selection. Right doesn't follow a named value's uses,
   which draw no wire, Properties disables Bind to Source while the graph
   is read-only, and Bind to Source on the canvas ends with its graph.
   Tab opens node search, and the node Enter adds there takes M at once. */
static bool graph_keys_frame(void *state)
{
    static uint64_t from, to;
    static uint64_t declared[DECLARED_COUNT], added;
    static char chosen[96];
    static studio_recent commands[STUDIO_RECENT_COUNT];
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= GRAPH_KEY_FRAMES);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
        return true;
    }
    if (frames == 2) {
        return true;
    }
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    REQUIRE(e && gr && s->graph.area >= 0);
    s->focus_area = s->graph.area;
    if (frames == 3) {
        for (int i = 0; i < acount(gr->edges) && !to; i++) {
            node *a = sg_node_by_id(s, gr->edges[i].from_id);
            node *b = sg_node_by_id(s, gr->edges[i].to_id);
            if (sg_node_visible(gr, a) && sg_node_visible(gr, b)) {
                from = a->id;
                to = b->id;
            }
        }
        REQUIRE(to);
        REQUIRE(output_labels_show(s, gr));
        sg_select_only(e, sg_node_by_id(s, from));
        REQUIRE(press_key(SDLK_RIGHT));
    } else if (frames == 4) {
        REQUIRE(selected_only(e, to));
        REQUIRE(press_key(SDLK_LEFT));
    } else if (frames == 5) {
        REQUIRE(selected_feeds(e, gr, to));
        REQUIRE(press_key(SDLK_ESCAPE));
    } else if (frames == 6) {
        REQUIRE(!acount(e->selected));
        sg_select_only(e, sg_node_by_id(s, to));
        ed_clear_connect_error(e);
        REQUIRE(press_key(SDLK_M));
    } else if (frames == 7) {
        /* Counter's statements aren't declarations a source can disable. */
        REQUIRE(!sg_node_by_id(s, to)->disabled && e->connect_error[0]);
        sg_select_only(e, sg_node_by_id(s, from));
        ed_add_selected(e, sg_node_by_id(s, to));
        REQUIRE(press_key(SDLK_M));
    } else if (frames == 8) {
        REQUIRE(!sg_node_by_id(s, from)->disabled && !sg_node_by_id(s, to)->disabled);
        REQUIRE(strstr(alast(s->records).text, "one node at a time"));
        sg_select_only(e, sg_node_by_id(s, to));
        s->graph.menu_node = to;
        s->graph.menu_edge = -1;
        s->graph.menu_x = s->graph.menu_y = 100;
        s->graph.menu_request = true;
        s->ui_schedule.wake = true;
    } else if (frames == 9) {
        REQUIRE(s->graph.menu_open);
        REQUIRE(press_key(SDLK_ESCAPE));
    } else if (frames == 10) {
        REQUIRE(!s->graph.menu_open && selected_only(e, to));
        /* A named value draws no wire, so Right has nowhere to go. */
        REQUIRE(open_declared(e, declared));
    } else if (frames == 12) {
        /* Frame 11 laid the graph out. */
        sg_select_only(e, sg_node_by_id(s, declared[NAMED_SOURCE]));
        REQUIRE(press_key(SDLK_RIGHT));
    } else if (frames == 13) {
        REQUIRE(selected_only(e, declared[NAMED_SOURCE]));
        sg_select_only(e, sg_node_by_id(s, declared[VECTOR_USE]));
        s->ui_schedule.wake = true;
    } else if (frames == 14) {
        /* A paused graph is read-only: Bind to Source can't act. */
        REQUIRE(bind_disabled(s, false));
        graph_flow_pause(gr);
        s->ui_schedule.wake = true;
    } else if (frames == 15) {
        REQUIRE(bind_disabled(s, true));
        graph_flow_resume(gr);
        /* Bind to Source on the canvas node, as its button does. */
        node *vector = sg_node_by_id(s, declared[VECTOR_USE]);
        s->graph.bind_pin = vector->input_pins[0].id;
        s->graph.bind_kit = s->graph.kit;
        s->graph.bind_focus = true;
    } else if (frames == 16) {
        /* The node's source field took the focus. Another graph, which may
           hold a pin of that id, leaves Bind to Source behind. */
        REQUIRE(s->graph.bind_pin && !s->graph.bind_focus);
        REQUIRE(!editor_open_graph(e, declared[OTHER_GRAPH]));
    } else if (frames == 17) {
        REQUIRE(gr->graph_id == declared[OTHER_GRAPH]);
        REQUIRE(!s->graph.bind_pin && !s->graph.bind_kit);
    } else if (frames == 18) {
        /* Frame 17 let go of the keys the field had. */
        REQUIRE(press_key(SDLK_TAB));
    } else if (frames == 19) {
        REQUIRE(s->palette.client == STUDIO_PALETTE_NODES && s->graph.spawn_pending);
    } else if (frames == 20) {
        /* Frame 19 gave the picker's search field the focus. */
        REQUIRE(type_text("Not"));
    } else if (frames == 21) {
        REQUIRE(!strcmp(s->palette.query, "Not"));
        memcpy(commands, s->prefs.recent, sizeof commands);
        REQUIRE(press_key(SDLK_RETURN));
    } else if (frames == 22) {
        /* Enter added the node and closed the picker, whose field let go of
           the keys: M at once disables the new node. */
        REQUIRE(!s->palette.client && acount(e->selected) == 1);
        REQUIRE(!memcmp(commands, s->prefs.recent, sizeof commands));
        added = e->selected[0]->id;
        snprintf(chosen, sizeof chosen, "%s", s->prefs.recent_nodes[0].key);
        REQUIRE(press_key(SDLK_M));
    } else if (frames == 23) {
        REQUIRE(sg_node_by_id(s, added)->disabled);
        REQUIRE(leads_search(s, chosen));
        keys++;
    } else if (frames == 24) {
        REQUIRE(opens_past_unnamed_event(s));
    }
    return true;
}

static bool element_says(studio *s, const char *selector, const char *text)
{
    rml_element *element = studio_element(s, selector);
    REQUIRE(element);
    char markup[512];
    rml_element_inner_rml(element, markup, sizeof markup);
    if (!strstr(markup, text)) {
        fprintf(stderr, "Studio shell acceptance: %s reads %s\n", selector, markup);
    }
    REQUIRE(strstr(markup, text));
    return true;
}

/* The unsaved-changes prompt asks `title`, with Save as its action and
   Discard as its cancel. */
static bool prompt_shows(studio *s, const char *title)
{
    REQUIRE(element_says(s, ".toast[data-front] .title", title));
    REQUIRE(element_says(s, ".toast[data-front] .toast-action", "Save"));
    REQUIRE(element_says(s, ".toast[data-front] .toast-cancel", "Discard"));
    return true;
}

/* Clicks the element `selector` finds, as a person would: the pointer moves
   over it, presses and releases. */
static bool click(studio *s, const char *selector)
{
    rml_element *button = studio_element(s, selector);
    REQUIRE(button);
    rml_vec2 at = rml_element_absolute_offset(button, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(button, RML_BOX_BORDER);
    REQUIRE(size.x > 0 && size.y > 0);
    SDL_WindowID id = window_id();
    REQUIRE(id);
    /* Layout is in dp; the window's pointer is in points, ui_scale to a dp. */
    float scale = s->prefs.ui_scale;
    float x = (at.x + size.x / 2) * scale, y = (at.y + size.y / 2) * scale;
    SDL_Event move = {
        .motion = { .type = SDL_EVENT_MOUSE_MOTION, .windowID = id, .x = x, .y = y }
    };
    REQUIRE(SDL_PushEvent(&move));
    for (int down = 1; down >= 0; down--) {
        SDL_Event e = { .button = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                 : SDL_EVENT_MOUSE_BUTTON_UP,
                                    .windowID = id,
                                    .button = SDL_BUTTON_LEFT,
                                    .down = down,
                                    .clicks = 1,
                                    .x = x,
                                    .y = y } };
        REQUIRE(SDL_PushEvent(&e));
    }
    return true;
}

static int counter_tab(studio *s)
{
    for (int i = 0; i < acount(s->scene->tabs); i++) {
        if (s->scene->tabs[i].kind == APP_EDITOR_DOCUMENT_BEHAVIOR &&
            strstr(s->scene->tabs[i].key, "Counter")) {
            return i;
        }
    }
    return -1;
}

/* Sets Counter's saved count literal, `was`, in its source to `value` from
   its default graph, leaving it unsaved. */
static bool edit_counter(studio *s, int was, const char *value)
{
    const node *literal = NULL;
    REQUIRE(show_state_graph(s) && check_counter(s, was, false, &literal));
    char error[256];
    bool changed = studio_code_expression_replace(s, literal, value, error, sizeof error);
    if (!changed) {
        fprintf(stderr, "Studio prompt acceptance: %s\n", error);
    }
    REQUIRE(changed && studio_unsaved(s));
    return true;
}

#define PLAY_KEY_FRAMES 6

/* F5 plays, Space pauses, F10 steps while paused, F5 resumes and Shift+F5
   stops, each a key the platform queues. */
static bool play_keys_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= PLAY_KEY_FRAMES);
    app_runtime_state run = app_scene_authoring_runtime_state(s->scene);
    switch (frames) {
        case 1:
            REQUIRE(!studio_session(s));
            REQUIRE(!strcmp(studio_command_label(STUDIO_COMMAND_REVERT), "Revert File"));
            REQUIRE(press_key(SDLK_F5));
            break;
        case 2:
            REQUIRE(studio_session(s) && run.playing);
            REQUIRE(!studio_command_enabled(s, STUDIO_COMMAND_RESUME));
            REQUIRE(press_key(SDLK_SPACE));
            break;
        case 3:
            REQUIRE(studio_session(s) && !run.playing);
            REQUIRE(!strcmp(studio_command_describe(s, STUDIO_COMMAND_RESUME).label, "Resume"));
            REQUIRE(studio_command_enabled(s, STUDIO_COMMAND_STEP));
            REQUIRE(press_key(SDLK_F10));
            break;
        case 4:
            REQUIRE(studio_session(s) && !run.playing);
            REQUIRE(press_key(SDLK_F5));
            break;
        case 5:
            REQUIRE(studio_session(s) && run.playing);
            REQUIRE(press_chord(SDLK_F5, SDL_KMOD_LSHIFT));
            break;
        default: REQUIRE(!studio_session(s)); keys = frames;
    }
    return true;
}

#define PROMPT_FRAMES 18
static bool quit_answered;

/* Adds an object to the open scene, leaving it unsaved. Its prototype
   exists only in this run, so the scene's file must never take it. */
static bool add_probe(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "name \"Probe\"\ncomponents {\n    GameTransform x=0 y=0\n}\n",
            NULL
        ) == 0
    );
    REQUIRE(app_scene_authoring_add_instance(scene, "Probe") == 0);
    REQUIRE(scene->scene_dirty);
    return true;
}

/* Closing Counter's clean tab just closes it. Closing it with an edit asks,
   and its Discard closes the tab without writing; asked again, a Save by
   hand dismisses the prompt and keeps the tab. With the scene running,
   quitting with an edit to Counter and to the scene asks; its Save writes
   Counter, which applies live, and asks again about the scene the running
   scene keeps from saving, whose Discard lets Studio quit without writing
   it. Both answers are clicks on the toast's buttons, which the prompt
   answers two frames after the click. Reduced motion, on from the first
   frame, settles each toast where layout puts it, so the clicks land on its
   buttons. */
static bool prompts_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= PROMPT_FRAMES);
    if (s->unsaved_prompt.quit) {
        /* Discard went through: Quit goes ahead with the scene unsaved. */
        REQUIRE(frames > 15 && studio_unsaved(s));
        quit_answered = true;
        return true;
    }
    switch (frames) {
        case 1:
            s->prefs.reduced_motion = true;
            /* A clean tab closes without asking. */
            studio_close_tab(s, counter_tab(s));
            REQUIRE(counter_tab(s) < 0 && !s->unsaved_prompt.toast);
            REQUIRE(app_scene_authoring_open_behavior(s->scene, "Counter") == 0);
            break;
        case 2:
            REQUIRE(!studio_command_enabled(s, STUDIO_COMMAND_SAVE_ALL));
            REQUIRE(edit_counter(s, 1, "3"));
            REQUIRE(studio_command_enabled(s, STUDIO_COMMAND_SAVE_ALL));
            studio_close_tab(s, counter_tab(s));
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_CLOSE_TAB);
            break;
        case 3:
            REQUIRE(prompt_shows(s, "Save Counter before closing?"));
            REQUIRE(click(s, ".toast[data-front] .toast-cancel"));
            break;
        case 6:
            REQUIRE(counter_tab(s) < 0 && !studio_unsaved(s));
            REQUIRE(unchanged_source());
            REQUIRE(app_scene_authoring_open_behavior(s->scene, "Counter") == 0);
            break;
        case 7:
            REQUIRE(edit_counter(s, 1, "5"));
            studio_close_tab(s, counter_tab(s));
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_CLOSE_TAB);
            break;
        case 8:
            /* Saving by hand while the prompt asks leaves nothing to ask. */
            REQUIRE(prompt_shows(s, "Save Counter before closing?"));
            REQUIRE(s->scene->active_tab == counter_tab(s));
            studio_command_run(s, STUDIO_COMMAND_SAVE);
            REQUIRE(!studio_unsaved(s));
            break;
        case 9:
            REQUIRE(!s->unsaved_prompt.toast);
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_NONE);
            REQUIRE(counter_tab(s) >= 0 && counter_says("count: int = 5"));
            REQUIRE(add_probe(s));
            studio_command_run(s, STUDIO_COMMAND_PLAY);
            break;
        case 10:
            REQUIRE(studio_session(s) && studio_save_held(s));
            REQUIRE(scene_tabs_activate(s->scene, counter_tab(s)) == 0);
            REQUIRE(edit_counter(s, 5, "4"));
            REQUIRE(!studio_quit(s, true));
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_QUIT);
            break;
        case 11:
            REQUIRE(prompt_shows(s, "and Counter before quitting?"));
            REQUIRE(click(s, ".toast[data-front] .toast-action"));
            break;
        case 13:
            /* Counter saved while the scene runs; the scene could not, so
               the prompt asks again about it instead of quitting. */
            REQUIRE(counter_says("count: int = 4"));
            REQUIRE(studio_session(s) && studio_unsaved(s));
            REQUIRE(s->unsaved_prompt.toast);
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_QUIT);
            break;
        case 14:
            REQUIRE(prompt_shows(s, "before quitting?"));
            REQUIRE(element_says(s, ".toast[data-front] .description", "Stop the scene"));
            REQUIRE(click(s, ".toast[data-front] .toast-cancel"));
            break;
        default: REQUIRE(frames < 17);
    }
    return true;
}

#define EXPORTED "Exported game: "

/* Frame 1 runs Export Game; frame 2 waits for the exporter to exit; frame 3's
 * poll, in the host's own frame, reports it. */
static bool export_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 3);
    if (frames == 1) {
        REQUIRE(studio_command_enabled(s, STUDIO_COMMAND_EXPORT_GAME));
        studio_command_run(s, STUDIO_COMMAND_EXPORT_GAME);
        if (!s->export_game.process) {
            fprintf(stderr, "Studio export acceptance: %s\n", alast(s->records).text);
        }
        REQUIRE(s->export_game.process);
        REQUIRE(!studio_command_enabled(s, STUDIO_COMMAND_EXPORT_GAME));
    } else if (frames == 2) {
        REQUIRE(SDL_WaitProcess(s->export_game.process, true, NULL));
    } else {
        REQUIRE(!s->export_game.process);
        const char *last = alast(s->records).text;
        if (strncmp(last, EXPORTED, strlen(EXPORTED))) {
            fprintf(stderr, "Studio export acceptance: %s\n", last);
        }
        REQUIRE(!strncmp(last, EXPORTED, strlen(EXPORTED)));
        snprintf(exported, sizeof exported, "%s", last + strlen(EXPORTED));
        REQUIRE(studio_command_enabled(s, STUDIO_COMMAND_EXPORT_GAME));
    }
    return true;
}

/* A ship with a turret, and a root that shares the turret's name. */
static const char
    nested_scene[] = "version 1\nname \"Scene\"\n(array)entities {\n"
                     "    - name=\"Ship\" prototype=\"Hull\" { (array)children "
                     "{ - name=\"Turret\" prototype=\"Hull\"; }; }\n"
                     "    - name=\"Turret\" prototype=\"Hull\"\n"
                     "}\n";

/* A turned ship whose turret sits ten units ahead of it. */
static const char turned_scene[] =
    "version 1\nname \"Scene\"\n(array)entities {\n"
    "    - name=\"Ship\" prototype=\"Hull\" { components { GameTransform "
    "x=100.0 y=0.0; "
    "GameOrientation radians=1.5707964; }; (array)children { - name=\"Turret\" "
    "prototype=\"Hull\" { components { GameTransform x=10.0 y=0.0; }; }; }; }\n"
    "}\n";

static bool child_of(app_scene_authoring *scene, int child, int parent)
{
    return ecs_entity_to_u64(ecs_parent(scene->world, scene->actors[child].entity)) ==
           ecs_entity_to_u64(scene->actors[parent].entity);
}

/* The scene's text, as its document would save it. */
static bool saves_as(app_scene_authoring *scene, const char *expected)
{
    char error[256];
    game_scene_document *document = game_scene_document_load(expected, error, sizeof error);
    char *canonical = document
                        ? game_scene_document_save(document, (ecs_component_catalog){ 0 })
                        : NULL;
    char *text = scene_authoring_scene_text(scene);
    bool same = canonical && text && !strcmp(canonical, text);
    if (!same) {
        fprintf(stderr, "Studio hierarchy acceptance saved:\n%s\n", text ? text : "(nothing)");
    }
    free(canonical);
    free(text);
    game_scene_document_free(document);
    return same;
}

/* Children load under their parent and keep it through save, undo and
   duplication; a parent waits for its children to go first. */
static bool scene_hierarchy_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 2) {
        return true;
    }
    app_scene_authoring *scene = s->scene;
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "name \"Hull\"\ncomponents {\n    GameTransform x=0 y=0\n    "
            "GameOrientation "
            "radians=0\n    GameAppearance shape=\"Box\" width=4.0 "
            "height=4.0\n}\n",
            NULL
        ) == 0
    );
    REQUIRE(scene_authoring_load_text(scene, nested_scene, NULL) == 0);
    REQUIRE(acount(scene->actors) == 3 && child_of(scene, 1, 0));
    REQUIRE(!ecs_parent(scene->world, scene->actors[2].entity).slot);
    REQUIRE(saves_as(scene, nested_scene));
    /* Changing an object's components replaces it, which a parent waits for
       too; a child keeps its parent through the change and its undo. */
    scene->selected = 0;
    REQUIRE(app_scene_authoring_add_instance_component(scene, "GameVelocity") != 0);
    REQUIRE(acount(scene->actors) == 3 && child_of(scene, 1, 0));
    scene->selected = 1;
    REQUIRE(app_scene_authoring_add_instance_component(scene, "GameVelocity") == 0);
    REQUIRE(acount(scene->actors) == 3 && child_of(scene, 1, 0));
    REQUIRE(replay_scene_edit(scene, false) == 0);
    REQUIRE(acount(scene->actors) == 3 && child_of(scene, 1, 0) && saves_as(scene, nested_scene));
    scene->selected = 0;
    REQUIRE(app_scene_authoring_delete_instance(scene) != 0 && acount(scene->actors) == 3);
    scene->selected = 1;
    REQUIRE(app_scene_authoring_delete_instance(scene) == 0 && acount(scene->actors) == 2);
    REQUIRE(replay_scene_edit(scene, false) == 0);
    REQUIRE(acount(scene->actors) == 3 && child_of(scene, 1, 0) && saves_as(scene, nested_scene));
    scene->selected = 1;
    REQUIRE(app_scene_authoring_duplicate_instance(scene) == 0);
    REQUIRE(acount(scene->actors) == 4 && scene->selected == 2 && child_of(scene, 2, 0));
    char expected[512];
    snprintf(
        expected,
        sizeof expected,
        "version 1\nname \"Scene\"\n(array)entities {\n"
        "    - name=\"Ship\" prototype=\"Hull\" { (array)children { - "
        "name=\"Turret\" prototype=\"Hull\"; "
        "- name=\"%s\" prototype=\"Hull\"; }; }\n"
        "    - name=\"Turret\" prototype=\"Hull\"\n}\n",
        scene->actors[2].name
    );
    REQUIRE(saves_as(scene, expected));
    /* The turned ship carries its turret to (100, 10), which the canvas, its
       y pointing down, shows at (100, -10). */
    REQUIRE(scene_authoring_load_text(scene, turned_scene, NULL) == 0);
    ecs_entity turret = scene->actors[1].entity;
    REQUIRE(
        ecs_entity_to_u64(pick_scene_entity(scene, 100, -10, (render_skip){ 0 })) ==
        ecs_entity_to_u64(turret)
    );
    /* Nothing shows where the turret would sit without its ship. */
    REQUIRE(!pick_scene_entity(scene, 10, 0, (render_skip){ 0 }).slot);
    /* A copy moved ten units along the world's x moves along the ship's -y. */
    scene->selected = 1;
    REQUIRE(scene_duplicate_selection(scene, (GameTransform){ 10, 0 }) == 0);
    const GameTransform *
        moved = GAME_GameTransform_get_const(scene->world, selected_actor(scene)->entity);
    REQUIRE(child_of(scene, scene->selected, 0));
    REQUIRE(fabsf(moved->x - 10) < 1e-3f && fabsf(moved->y + 10) < 1e-3f);
    edits++;
    return true;
}

/* Three unused prototypes, the only ones; Assets deletes declarative assets
   (executable sources go through their own refactors). */
static const char *const spares[] = { "SpareA", "SpareB", "SpareC" };

static bool spare_path(const char *name, char *path, size_t size)
{
    int length = snprintf(path, size, "%s/content/prototypes/%s.kdl", project, name);
    REQUIRE(length > 0 && (size_t)length < size);
    return true;
}

static bool write_spare(const char *name)
{
    char path[1024], text[128];
    REQUIRE(spare_path(name, path, sizeof path));
    snprintf(text, sizeof text, "name \"%s\"\ncomponents {\n    (object)GAME.GameTransform\n}\n", name);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fputs(text, file) >= 0;
    REQUIRE(fclose(file) == 0 && written);
    return true;
}

static bool write_spares()
{
    char folder[1024];
    int length = snprintf(folder, sizeof folder, "%s/content/prototypes", project);
    REQUIRE(length > 0 && (size_t)length < sizeof folder);
    REQUIRE(mkdir(folder, 0777) == 0);
    for (int i = 0; i < 3; i++) {
        REQUIRE(write_spare(spares[i]));
    }
    return true;
}

static bool spare_exists(const char *name)
{
    char path[1024];
    struct stat info;
    return spare_path(name, path, sizeof path) && stat(path, &info) == 0;
}

/* A click on the prototype's row, with modifier keys. */
static bool click_asset(studio *s, const char *name, unsigned modifiers)
{
    int id = reference_index_find_name(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_PROTOTYPE, name);
    REQUIRE(id >= 0);
    ui_activation a = { .argc = 1, .modifiers = modifiers };
    snprintf(a.args[0], sizeof a.args[0], "%d", id);
    studio_assets_action(s, "click", &a);
    return true;
}

/* The primary selection, and the assets marked with it in order. */
static bool selection_is(studio *s, const char *selected, const char *marked)
{
    char names[256] = "";
    for (int i = 0; i < acount(s->assets_marked); i++) {
        size_t used = strlen(names);
        snprintf(
            names + used,
            sizeof names - used,
            "%s%s",
            i ? " " : "",
            s->assets_marked[i].name
        );
    }
    bool same = !strcmp(s->assets_selected, selected) && !strcmp(names, marked);
    if (!same) {
        fprintf(stderr, "Studio assets selected %s with \"%s\"\n", s->assets_selected, names);
    }
    return same;
}

/* The toast's Undo of the last change. */
static bool undo_change(studio *s)
{
    app_refactor *undo = s->refactor.undo, *redo = NULL;
    REQUIRE(undo && s->refactor.undo_toast);
    s->refactor.undo = NULL;
    kit_toast_dismiss(s->kit, s->refactor.undo_toast);
    s->refactor.undo_toast = 0;
    char error[256] = "";
    REQUIRE(app_scene_authoring_apply_refactor(s->scene, undo, &redo, error, sizeof error) == 0);
    app_refactor_free(undo);
    app_refactor_free(redo);
    return true;
}

/* A file that sorts before the selection appears while the index waits to
   be rebuilt, as it does while a pointer is down or an editor has the keys.
   Deleting rebuilds it, renumbering the assets, yet removes the selected
   ones. */
static bool delete_after_index_change(studio *s)
{
    unsigned toggle = s->mac ? RML_KM_META : RML_KM_CTRL;
    REQUIRE(click_asset(s, "SpareA", 0) && click_asset(s, "SpareC", toggle));
    REQUIRE(write_spare("Spare0"));
    app_scene_authoring_refresh_content(s->scene);
    REQUIRE(studio_assets_key(s, SDLK_DELETE, 0));
    REQUIRE(spare_exists("Spare0") && spare_exists("SpareB"));
    REQUIRE(!spare_exists("SpareA") && !spare_exists("SpareC") && undo_change(s));
    char path[1024];
    REQUIRE(spare_path("Spare0", path, sizeof path) && remove(path) == 0);
    app_scene_authoring_refresh_content(s->scene);
    REQUIRE(spare_exists("SpareA") && spare_exists("SpareC"));
    return true;
}

/* Clicks and the keys select several assets, which Delete removes as one
   change that one Undo restores. */
static bool select_and_delete(studio *s)
{
    unsigned toggle = s->mac ? RML_KM_META : RML_KM_CTRL;
    REQUIRE(click_asset(s, "SpareA", 0) && selection_is(s, "SpareA", ""));
    REQUIRE(click_asset(s, "SpareC", RML_KM_SHIFT) && selection_is(s, "SpareC", "SpareA SpareB"));
    REQUIRE(click_asset(s, "SpareB", toggle) && selection_is(s, "SpareC", "SpareA"));
    /* Shift and the arrows extend from where the plain click started. */
    REQUIRE(studio_assets_key(s, SDLK_UP, SDL_KMOD_SHIFT));
    REQUIRE(selection_is(s, "SpareB", "SpareA"));
    REQUIRE(studio_assets_key(s, SDLK_UP, SDL_KMOD_SHIFT));
    REQUIRE(selection_is(s, "SpareA", ""));
    REQUIRE(studio_assets_key(s, SDLK_UP, SDL_KMOD_SHIFT));
    REQUIRE(selection_is(s, "SpareA", ""));
    REQUIRE(studio_assets_key(s, SDLK_DOWN, SDL_KMOD_SHIFT));
    REQUIRE(studio_assets_key(s, SDLK_DOWN, SDL_KMOD_SHIFT));
    REQUIRE(selection_is(s, "SpareC", "SpareA SpareB"));
    REQUIRE(click_asset(s, "SpareB", toggle) && selection_is(s, "SpareC", "SpareA"));
    REQUIRE(studio_assets_key(s, SDLK_DELETE, 0));
    REQUIRE(!spare_exists("SpareA") && spare_exists("SpareB") && !spare_exists("SpareC"));
    REQUIRE(unchanged_source());
    /* The toast's Undo, which restores both files. */
    REQUIRE(undo_change(s));
    REQUIRE(spare_exists("SpareA") && spare_exists("SpareB") && spare_exists("SpareC"));
    edits++;
    return true;
}

/* A box, and inside it a spot with nothing to draw, which a marker shows. */
static const char marked_scene
    [] = "version 1\nname \"Scene\"\n(array)entities {\n"
         "    - name=\"Box\" prototype=\"Box\"\n"
         "    - name=\"Spot\" prototype=\"Spot\" { components { GameTransform "
         "x=5.0 y=5.0; }; }\n"
         "}\n";
/* More undrawn objects than any fixed count of markers the Scene kept, laid
   out inside the box's 40 by 40, so a framed view shows them all. */
#define CROWD_COLUMNS 20
#define CROWD_ROWS 15
#define CROWD (CROWD_COLUMNS * CROWD_ROWS)
#define CROWD_LINE 128 /* room for each spot's line of the scene */
#define VIEWPORT_FRAMES 19
/* The narrow Scene the hint must fit: under the hint box's 320 dp. */
#define NARROW_SCENE_DP 240.0f

/* Queues a mouse event at the Scene surface's point (x, y), in points. */
static bool push_mouse(Uint32 type, float x, float y, float wheel_x, float wheel_y)
{
    SDL_WindowID window = window_id();
    REQUIRE(window);
    SDL_Event e = { .type = type };
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion = (SDL_MouseMotionEvent){ .type = type, .windowID = window, .x = x, .y = y };
    } else if (type == SDL_EVENT_MOUSE_WHEEL) {
        e.wheel = (SDL_MouseWheelEvent){ .type = type,
                                         .windowID = window,
                                         .x = wheel_x,
                                         .y = wheel_y,
                                         .mouse_x = x,
                                         .mouse_y = y };
    } else {
        e.button = (SDL_MouseButtonEvent){ .type = type,
                                           .windowID = window,
                                           .button = SDL_BUTTON_LEFT,
                                           .down = type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                                           .clicks = 1,
                                           .x = x,
                                           .y = y };
    }
    REQUIRE(SDL_PushEvent(&e));
    return true;
}

/* The gizmo part a pointer at canvas point (x, y) rests on, at a tenth of a
   unit per pixel. */
static int part_under(app_scene_authoring *scene, float x, float y)
{
    engine_viewport_pointer pointer = { .world_x = x, .world_y = y, .world_per_pixel = 0.1f };
    scene_gizmo_pointer(scene, scene->render_viewport, &pointer);
    return scene->gizmo.hover;
}

/* A marker picks above the box it sits in and is listed for the host; a
   handle and the ring take a press a few pixels past them, and a resize
   shows its size by the pointer. */
static bool marker_and_gizmo(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(acount(scene->actors) == 2);
    ecs_entity box = scene->actors[0].entity, spot = scene->actors[1].entity;
    app_scene_marker markers[4];
    REQUIRE(app_scene_authoring_markers(scene, markers, 4) == 1 && markers[0].actor == 1);
    REQUIRE(
        ecs_entity_to_u64(pick_scene_entity(scene, 5, -5, (render_skip){ 0 })) ==
        ecs_entity_to_u64(spot)
    );
    REQUIRE(
        ecs_entity_to_u64(pick_scene_entity(scene, -15, 15, (render_skip){ 0 })) ==
        ecs_entity_to_u64(box)
    );
    /* A thumbnail fits only what draws, so the spot alone fits nothing;
       the Scene frames it by its marker, at its place. */
    REQUIRE(!fit_scene_view(s->viewport, scene->world, &spot, 1));
    REQUIRE(frame_scene_view(s->viewport, scene->world, &spot, 1));
    engine_viewport_view framed;
    engine_viewport_get_view(s->viewport, &framed);
    REQUIRE(framed.center_x == 5 && framed.center_y == -5);
    REQUIRE(app_scene_authoring_select_entity(scene, box) == 0);
    /* The right edge's handle sits at (20, 0); the ring 64 pixels out. */
    scene->gizmo.in.tool = SCENE_TOOL_SCALE;
    REQUIRE(part_under(scene, 20 + 1.1f, 0) >= SCENE_GIZMO_HANDLE);
    REQUIRE(part_under(scene, 20 + 1.4f, 0) == SCENE_GIZMO_NONE);
    scene->gizmo.in.tool = SCENE_TOOL_ROTATE;
    REQUIRE(part_under(scene, 6.4f + 0.9f, 0) == SCENE_GIZMO_RING);
    REQUIRE(part_under(scene, 6.4f - 0.9f, 0) == SCENE_GIZMO_RING);
    REQUIRE(part_under(scene, 6.4f + 1.2f, 0) == SCENE_GIZMO_NONE);
    scene->gizmo.in.tool = SCENE_TOOL_SCALE;
    scene->gizmo.in.snap = false;
    engine_viewport_pointer pointer = { .world_x = 20,
                                        .world_per_pixel = 0.1f,
                                        .pressed = true,
                                        .down = true };
    REQUIRE(scene_gizmo_pointer(scene, scene->render_viewport, &pointer));
    pointer = (engine_viewport_pointer){ .world_x = 30,
                                         .world_per_pixel = 0.1f,
                                         .down = true,
                                         .dragging = true };
    REQUIRE(scene_gizmo_pointer(scene, scene->render_viewport, &pointer));
    REQUIRE(scene->gizmo.active && !strcmp(scene->gizmo.delta, "50 \xc3\x97 40"));
    REQUIRE(!strcmp(scene->gizmo.status, "Size 50 \xc3\x97 40"));
    scene->gizmo.in.cancel = true;
    REQUIRE(scene_gizmo_pointer(scene, scene->render_viewport, &pointer));
    REQUIRE(
        !scene->gizmo.active &&
        GAME_GameAppearance_get_const(scene->world, box)->width == 40
    );
    return true;
}

/* While a previewed prototype plays, the wheel, a trackpad's fraction
   included, and Space are its game's, as the scene's are while it plays.
   The state is the one a Preview target's Play sets; this project's source
   behavior keeps a prototype preview from launching here. */
static bool previewed_prototype_plays(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(!scene->runtime_playing && scene->active_runtime_target == APP_RUNTIME_SCENE);
    scene->active_runtime_target = APP_RUNTIME_PREVIEW;
    scene->preview_playing = true;
    engine_viewport_view before, after;
    engine_viewport_get_view(s->viewport, &before);
    float scrolled = s->scroll_y;
    s->scroll_y = 0.5f;
    engine_viewport_host_input input = { .wheel = 0.5f, .width = s->surface.rect.w };
    studio_scene_view(s, &input);
    s->scroll_y = scrolled;
    engine_viewport_get_view(s->viewport, &after);
    bool space = studio_scene_space(s, (studio_key){ .key = SDLK_SPACE });
    scene->preview_playing = false;
    scene->active_runtime_target = APP_RUNTIME_SCENE;
    REQUIRE(input.wheel == 0.5f && after.center_y == before.center_y);
    REQUIRE(!space);
    return true;
}

/* While the game plays, the wheel, a trackpad's included, scrolls its UI,
   and an undrawn object, which shows no marker, picks nothing. */
static bool played_scene(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(studio_session(s) && scene->runtime_playing);
    engine_viewport_host_input input = { .wheel = 0.5f, .width = s->surface.rect.w };
    studio_scene_view(s, &input);
    REQUIRE(input.wheel == 0.5f);
    REQUIRE(
        ecs_entity_to_u64(pick_scene_entity(scene, 5, -5, (render_skip){ 0 })) ==
        ecs_entity_to_u64(scene->actors[0].entity)
    );
    edits++;
    return true;
}

/* The grid's arrows move among the tiles it shows: a filter the list took
   while the grid was hidden leaves none of its old lines behind. */
static bool grid_after_list_filter(studio *s)
{
    REQUIRE(click_asset(s, "SpareC", 0) && selection_is(s, "SpareC", ""));
    ui_activation grid = { .argc = 1 };
    snprintf(grid.args[0], sizeof grid.args[0], "0");
    studio_assets_action(s, "view", &grid);
    REQUIRE(studio_assets_key(s, SDLK_RIGHT, 0) && selection_is(s, "SpareC", ""));
    s->assets_filter[0] = '\0';
    return true;
}

static bool asset_selection_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 6);
    switch (frames) {
        case 1:
            /* A file saved before the view setting kept list view as the
               smallest tile size. */
            REQUIRE(studio_prefs_read("{\"assets\":{\"tileSize\":40}}").assets_list);
            REQUIRE(!studio_prefs_read("{\"assets\":{\"tileSize\":40,\"view\":\"grid\"}}")
                         .assets_list);
            REQUIRE(!studio_prefs_read("{\"assets\":{\"tileSize\":96}}").assets_list);
            studio_show_editor(s, STUDIO_EDITOR_ASSETS);
            s->prefs.assets_list = true;
            s->assets_kind = REFERENCE_ASSET_PROTOTYPE;
            return true;
        case 2: return select_and_delete(s);
        case 3:
            /* After a frame has drawn the restored assets. */
            REQUIRE(delete_after_index_change(s));
            s->prefs.assets_list = false;
            return true;
        case 4:
            s->prefs.assets_list = true;
            snprintf(s->assets_filter, sizeof s->assets_filter, "SpareC");
            return true;
        case 5: return grid_after_list_filter(s);
        default:
            /* The grid lays every tile out again. */
            REQUIRE(studio_assets_key(s, SDLK_LEFT, 0) && selection_is(s, "SpareB", ""));
            return true;
    }
}

/* The box and CROWD undrawn spots inside it, the last selected. */
static bool load_crowd(studio *s)
{
    size_t size = (size_t)(CROWD + 3) * CROWD_LINE;
    char *text = xmalloc(size);
    int at = snprintf(text, size, "version 1\nname \"Scene\"\n(array)entities {\n    - name=\"Box\" prototype=\"Box\"\n");
    for (int i = 0; i < CROWD; i++) {
        int x = 2 * (i % CROWD_COLUMNS) - CROWD_COLUMNS + 1;
        int y = 2 * (i / CROWD_COLUMNS) - CROWD_ROWS + 1;
        at += snprintf(
            text + at,
            size - (size_t)at,
            "    - name=\"Spot%d\" prototype=\"Spot\" { components { "
            "GameTransform x=%d.0 y=%d.0; }; }\n",
            i,
            x,
            y
        );
    }
    snprintf(text + at, size - (size_t)at, "}\n");
    int loaded = scene_authoring_load_text(s->scene, text, NULL);
    free(text);
    REQUIRE(loaded == 0 && acount(s->scene->actors) == CROWD + 1);
    ecs_entity last = s->scene->actors[CROWD].entity;
    return app_scene_authoring_select_entity(s->scene, last) == 0;
}

/* How many markers the Scene shows, past data-for's hidden template. */
static int scene_markers(studio *s)
{
    rml_element *surface = studio_element(s, "#scene-surface");
    REQUIRE(surface);
    rml_element *document = rml_element_owner_document(surface);
    rml_element *found[CROWD + 2];
    int count = rml_element_query_selector_all(document, ".viewport > .marker", found, CROWD + 2);
    REQUIRE(count <= CROWD + 2);
    int shown = 0;
    for (int i = 0; i < count; i++) {
        shown += rml_element_is_visible(found[i], false);
    }
    return shown;
}

/* `selector`'s element in the tool's hint lies across the Scene's width,
   inside it. Layout offsets leave out the transform that places the hint. */
static bool inside_scene(studio *s, const char *selector)
{
    rml_element *surface = studio_element(s, "#scene-surface");
    rml_element *element = studio_element(s, selector);
    REQUIRE(surface && element);
    char transform[128];
    float shift;
    rml_element_property(studio_element(s, ".tool-hint"), "transform", transform, sizeof transform);
    REQUIRE(sscanf(transform, "translate(%fpx", &shift) == 1);
    float left = rml_element_absolute_offset(surface, RML_BOX_BORDER).x;
    float right = left + rml_element_box_size(surface, RML_BOX_BORDER).x;
    float at = rml_element_absolute_offset(element, RML_BOX_BORDER).x + shift;
    float width = rml_element_box_size(element, RML_BOX_BORDER).x;
    if (width <= 0 || at < left || at + width > right) {
        fprintf(
            stderr,
            "Studio scene acceptance: %s spans %g..%g in %g..%g\n",
            selector,
            at,
            at + width,
            left,
            right
        );
    }
    REQUIRE(width > 0 && at >= left && at + width <= right);
    return true;
}

/* The hint a tool no longer needs fades out (presence) as it was. Then the
   box and spot load again, nothing selected, framed. */
static bool hint_leaves(studio *s)
{
    rml_element *hint = studio_element(s, ".tool-hint");
    REQUIRE(hint && rml_element_is_visible(hint, false));
    REQUIRE(rml_element_has_attribute(hint, "data-ending-style"));
    REQUIRE(element_says(s, ".tool-hint-pill .text", "No Orientation to rotate"));
    REQUIRE(scene_authoring_load_text(s->scene, marked_scene, NULL) == 0);
    studio_command_run(s, STUDIO_COMMAND_DESELECT);
    REQUIRE(scene_selection_count(s->scene) == 0);
    studio_frame_all(s);
    return true;
}

/* A marquee drawn from empty space below the box over the spot marker's
   lower right, short of its center, takes the spot. */
static bool marquee_edge(studio *s)
{
    static float x, y, reach;
    app_scene_authoring *scene = s->scene;
    if (frames == 17) {
        app_scene_marker marker;
        REQUIRE(app_scene_authoring_markers(scene, &marker, 1) == 1 && marker.actor == 1);
        kit_rect r = s->surface.rect;
        x = (r.x + marker.x * r.w) * s->prefs.ui_scale;
        y = (r.y + marker.y * r.h) * s->prefs.ui_scale;
        reach = scene->gizmo.in.marker_radius;
        float bottom = (r.y + r.h) * s->prefs.ui_scale - 2;
        REQUIRE(bottom > y + 2 * reach);
        REQUIRE(push_mouse(SDL_EVENT_MOUSE_MOTION, x + reach, bottom, 0, 0));
        return push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x + reach, bottom, 0, 0);
    }
    if (frames == 18) {
        REQUIRE(scene->marquee);
        return push_mouse(SDL_EVENT_MOUSE_MOTION, x + 0.6f * reach, y + 0.6f * reach, 0, 0);
    }
    REQUIRE(scene->marquee && scene_instance_selected(scene, 1));
    return push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, x + 0.6f * reach, y + 0.6f * reach, 0, 0);
}

/* Every undrawn object shows its marker, however many there are, and in a
   Scene narrowed under the hint's box the rotate tool's hint for a spot
   with no orientation, and its Add, stay inside it. */
static bool crowd_and_narrow(studio *s)
{
    switch (frames) {
        case 10: {
            studio_command_run(s, STUDIO_COMMAND_STOP);
            REQUIRE(!studio_session(s) && load_crowd(s));
            studio_command_run(s, STUDIO_COMMAND_ROTATE_TOOL);
            /* The Scene beside the Text Editor, the splitter dragged to
               leave it NARROW_SCENE_DP. */
            float column = s->main_rect.w;
            REQUIRE(column > NARROW_SCENE_DP * 2);
            studio_set_workspace(s, STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
            studio_active_layout(s)->main_split = NARROW_SCENE_DP / (column - 4);
            studio_ui_resync(s, "workspace");
            return true;
        }
        case 12:
            REQUIRE(s->surface.rect.w > 0 && s->surface.rect.w < 320);
            studio_frame_all(s);
            return true;
        case 14:
            REQUIRE(scene_markers(s) == CROWD);
            REQUIRE(element_says(s, ".tool-hint-pill .text", "No Orientation to rotate"));
            REQUIRE(inside_scene(s, ".tool-hint-pill"));
            REQUIRE(inside_scene(s, ".tool-hint-pill .btn"));
            studio_command_run(s, STUDIO_COMMAND_MOVE_TOOL);
            return true;
        /* The gizmo takes the tool a frame after the command. */
        case 16: return hint_leaves(s);
        case 17:
        case 18:
        case VIEWPORT_FRAMES: return marquee_edge(s);
        default: return true;
    }
}

/* The Scene's one zoom reads 100% when framed; a mouse wheel zooms it, a
   trackpad's scroll pans it, Space with a drag pans and Space alone plays
   once it lifts. Events queue after a frame for the next one. */
static bool scene_viewport_frame(void *state)
{
    static engine_viewport_view before;
    static float x, y;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= VIEWPORT_FRAMES);
    app_scene_authoring *scene = s->scene;
    engine_viewport_view view = { 0 };
    if (s->viewport) {
        engine_viewport_get_view(s->viewport, &view);
    }
    switch (frames) {
        case 1:
            REQUIRE(
                app_scene_authoring_register_prototype(
                    scene,
                    "name \"Box\"\ncomponents {\n    GameTransform x=0 y=0\n   "
                    " "
                    "GameOrientation radians=0\n    GameAppearance "
                    "shape=\"Box\" "
                    "width=40.0 height=40.0\n}\n",
                    NULL
                ) == 0
            );
            REQUIRE(
                app_scene_authoring_register_prototype(scene, "name \"Spot\"\ncomponents {\n    GameTransform x=0 y=0\n}\n", NULL) ==
                0
            );
            REQUIRE(scene_authoring_load_text(scene, marked_scene, NULL) == 0);
            /* Framing before the Scene's first draw leaves its measure to
               that draw. */
            REQUIRE(!scene->render_viewport);
            studio_frame_all(s);
            return true;
        case 2:
            REQUIRE(s->viewport && s->surface.rect.w > 0);
            REQUIRE(marker_and_gizmo(s));
            studio_frame_all(s);
            REQUIRE(fabsf(app_scene_authoring_zoom(scene) - 1) < 1e-4f);
            engine_viewport_get_view(s->viewport, &before);
            x = (s->surface.rect.x + s->surface.rect.w * 0.5f) * s->prefs.ui_scale;
            y = (s->surface.rect.y + s->surface.rect.h * 0.5f) * s->prefs.ui_scale;
            REQUIRE(push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0));
            return push_mouse(SDL_EVENT_MOUSE_WHEEL, x, y, 0, 1);
        case 3:
            REQUIRE(s->surface.hovered);
            REQUIRE(view.width < before.width && app_scene_authoring_zoom(scene) > 1);
            before = view;
            REQUIRE(previewed_prototype_plays(s));
            /* A fraction of a notch reads as a trackpad's scroll. */
            return push_mouse(SDL_EVENT_MOUSE_WHEEL, x, y, 0, 0.5f);
        case 4:
            REQUIRE(view.width == before.width && view.center_x == before.center_x);
            REQUIRE(view.center_y > before.center_y);
            before = view;
            REQUIRE(push_key(SDLK_SPACE, SDL_KMOD_NONE, true));
            return push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 0, 0);
        case 5: return push_mouse(SDL_EVENT_MOUSE_MOTION, x + 40, y + 30, 0, 0);
        case 6:
            REQUIRE(push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, x + 40, y + 30, 0, 0));
            /* Space lifts while Settings records a chord too. */
            s->shortcut_recording = STUDIO_COMMAND_FRAME + 1;
            return push_key(SDLK_SPACE, SDL_KMOD_NONE, false);
        case 7:
            REQUIRE(!s->space_down && !s->scene_nav.space_play);
            s->shortcut_recording = 0;
            REQUIRE(!studio_session(s) && !scene->marquee);
            REQUIRE(view.width == before.width && view.center_x < before.center_x);
            REQUIRE(view.center_y > before.center_y);
            return push_key(SDLK_SPACE, SDL_KMOD_NONE, true);
        case 8:
            /* Space waits for its release, which a drag may still follow. */
            REQUIRE(s->space_down && !studio_session(s));
            return push_key(SDLK_SPACE, SDL_KMOD_NONE, false);
        case 9: return played_scene(s);
        default: return crowd_and_narrow(s);
    }
}

bool studio_acceptance_frame(void *state)
{
    switch (mode) {
        case SOURCE_EDIT: callback_ok = source_edit_frame(state); break;
        case CODE_KEYS: callback_ok = code_keys_frame(state); break;
        case KDL_KEYS: callback_ok = kdl_keys_frame(state); break;
        case EXPORT_GAME: callback_ok = export_frame(state); break;
        case GRAPH_VISIBILITY:
            callback_ok = graph_visibility_frame(state);
            break;
        case GRAPH_KEYS: callback_ok = graph_keys_frame(state); break;
        case UNIT_WARNINGS: callback_ok = unit_warnings_frame(state); break;
        case SCENE_HIERARCHY: callback_ok = scene_hierarchy_frame(state); break;
        case ASSET_SELECTION: callback_ok = asset_selection_frame(state); break;
        case SCENE_VIEWPORT: callback_ok = scene_viewport_frame(state); break;
        case PLAY_KEYS: callback_ok = play_keys_frame(state); break;
        case PROMPTS: callback_ok = prompts_frame(state); break;
    }
    return callback_ok;
}

TEST_CASE(source_graph_edit_undo_redo)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless", "--frames", "4",
                     "--fixed-dt", "0.016666666666666666", "--project-root",
                     (char *)project, "--open", "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok);
    REQUIRE(unchanged_source());
    REQUIRE(frames == 4 && edits == 1 && undos == 1 && redos == 1);
    printf("\nStudio source edit accepted: frames=%d edits=%d undo=%d redo=%d\n",
           frames, edits, undos, redos);
    return true;
}

TEST_CASE(code_enter_types_newlines)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(KEY_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok);
    REQUIRE(unchanged_source());
    REQUIRE(frames == KEY_FRAMES && keys == 10);
    printf("Studio code keys accepted: frames=%d keys=%d\n", frames, keys);
    return true;
}

TEST_CASE(kdl_enter_saves)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(KDL_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok);
    REQUIRE(unchanged_source());
    REQUIRE(frames == KDL_FRAMES && keys == 2);
    printf("Studio KDL keys accepted: frames=%d keys=%d\n", frames, keys);
    return true;
}

TEST_CASE(code_underlines_unit_mismatch)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(UNIT_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == UNIT_FRAMES && unit_warned);
    REQUIRE(unchanged_source());
    printf("Studio unit warning accepted: frames=%d\n", frames);
    return true;
}

TEST_CASE(graph_visibility_and_document_pair)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "10",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 10);
    REQUIRE(unchanged_source());
    printf("\nStudio graph visibility accepted: frames=%d\n", frames);
    return true;
}

TEST_CASE(graph_keys)
{
    REQUIRE(unchanged_source() && write_late());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(GRAPH_KEY_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == GRAPH_KEY_FRAMES && keys == 1);
    REQUIRE(unchanged_source());
    printf("Studio graph keys accepted: frames=%d\n", frames);
    return true;
}

TEST_CASE(export_game)
{
    char *args[] = { (char *)executable, "--headless", "--frames", "3",
                     "--fixed-dt", "0.016666666666666666", "--project-root",
                     (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 3);
    char artifact[1100];
#ifdef __APPLE__
    snprintf(artifact, sizeof artifact, "%s/Contents/Resources/game.chibip", exported);
#else
    snprintf(artifact, sizeof artifact, "%s/data/game.chibip", exported);
#endif
    struct stat info;
    REQUIRE(stat(artifact, &info) == 0 && info.st_size > 0);
    printf("Studio export accepted: %s\n", artifact);
    return true;
}

/* A gizmo part's cursor: the move square's, the axes', the ring's, and a
   resize handle's direction as the box turns. */
TEST_CASE(scene_gizmo_cursors)
{
    REQUIRE(!scene_gizmo_part_cursor(SCENE_GIZMO_NONE, 0));
    REQUIRE(!strcmp(scene_gizmo_part_cursor(SCENE_GIZMO_FREE, 0), "move"));
    REQUIRE(!strcmp(scene_gizmo_part_cursor(SCENE_GIZMO_AXIS_X, 0), "ew-resize"));
    REQUIRE(!strcmp(scene_gizmo_part_cursor(SCENE_GIZMO_AXIS_Y, 0), "ns-resize"));
    REQUIRE(!strcmp(scene_gizmo_part_cursor(SCENE_GIZMO_RING, 0), "pointer"));
    /* Handles run counterclockwise from the bottom left, in Y-up canvas units. */
    static const char *const upright[4] = { "nesw-resize", "ns-resize",
                                            "nwse-resize", "ew-resize" };
    static const char *const quarter[4] = { "nwse-resize", "ew-resize",
                                            "nesw-resize", "ns-resize" };
    for (int h = 0; h < 8; h++) {
        const char *turned = scene_gizmo_part_cursor(SCENE_GIZMO_HANDLE + h, CF_PI / 2);
        const char *flipped = scene_gizmo_part_cursor(SCENE_GIZMO_HANDLE + h, -CF_PI);
        REQUIRE(!strcmp(scene_gizmo_part_cursor(SCENE_GIZMO_HANDLE + h, 0), upright[h % 4]));
        REQUIRE(!strcmp(turned, quarter[h % 4]));
        REQUIRE(!strcmp(flipped, upright[h % 4]));
    }
    return true;
}

TEST_CASE(scene_hierarchy)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 2 && edits == 1);
    printf("Studio scene hierarchy accepted\n");
    return true;
}

TEST_CASE(asset_selection)
{
    REQUIRE(write_spares());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "6",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 6 && edits == 1);
    printf("Studio asset selection accepted\n");
    return true;
}

TEST_CASE(scene_viewport)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(VIEWPORT_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == VIEWPORT_FRAMES && edits == 1);
    printf("Studio scene viewport accepted\n");
    return true;
}

/* The start scene's file never took the probe object. */
static bool start_unchanged()
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/scenes/Start.kdl", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    char *text = file_read_text(path);
    REQUIRE(text);
    bool same = !strstr(text, "Probe");
    free(text);
    REQUIRE(same);
    return true;
}

/* Restores Counter's bytes over what the shell test saved. */
static bool restore_counter()
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Counter.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fputs(counter_bytes, file) >= 0;
    REQUIRE(fclose(file) == 0 && written);
    return true;
}

TEST_CASE(play_keys)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(PLAY_KEY_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && keys == PLAY_KEY_FRAMES);
    printf("\nStudio play keys accepted\n");
    return true;
}

TEST_CASE(unsaved_prompts)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(PROMPT_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && quit_answered);
    bool saved = counter_says("count: int = 4");
    bool kept = start_unchanged();
    REQUIRE(restore_counter() && saved && kept);
    printf("\nStudio unsaved prompts accepted: frames=%d\n", frames);
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--scene-cursors")) {
        RUN_TEST_CASE(scene_gizmo_cursors);
        pu_print_stats();
        return pu_test_failed() ? 1 : 0;
    }
    bool source_edit = argc == 4 && !strcmp(argv[1], "--source-edit");
    bool code_keys = argc == 4 && !strcmp(argv[1], "--code-keys");
    bool kdl_keys = argc == 4 && !strcmp(argv[1], "--kdl-keys");
    bool exporting = argc == 4 && !strcmp(argv[1], "--export-game");
    bool visibility = argc == 4 && !strcmp(argv[1], "--graph-visibility");
    bool keys_mode = argc == 4 && !strcmp(argv[1], "--graph-keys");
    bool units = argc == 4 && !strcmp(argv[1], "--unit-warnings");
    bool hierarchy = argc == 4 && !strcmp(argv[1], "--scene-hierarchy");
    bool assets = argc == 4 && !strcmp(argv[1], "--asset-selection");
    bool viewport = argc == 4 && !strcmp(argv[1], "--scene-viewport");
    bool playing = argc == 4 && !strcmp(argv[1], "--play-keys");
    bool prompting = argc == 4 && !strcmp(argv[1], "--unsaved-prompts");
    if ((!source_edit && !code_keys && !kdl_keys && !exporting && !visibility && !keys_mode &&
         !units && !hierarchy && !assets && !viewport && !playing && !prompting) ||
        strcmp(argv[2], "--project-root")) {
        fprintf(
            stderr,
            "Usage: studio-acceptance "
            "--source-edit|--code-keys|--kdl-keys|--export-game|"
            "--graph-visibility|--graph-keys|--unit-warnings|--scene-hierarchy|"
            "--asset-selection|--scene-viewport|--play-keys|--unsaved-prompts "
            "--project-root PATH | --scene-cursors\n"
        );
        return 2;
    }
    executable = argv[0];
    project = argv[3];
    char prefs[1024], cache[1024];
    int p = snprintf(prefs, sizeof prefs, "%s/.acceptance-prefs.json", project);
    int c = snprintf(cache, sizeof cache, "%s/.acceptance-thumbnails", project);
    if (p <= 0 || (size_t)p >= sizeof prefs || c <= 0 || (size_t)c >= sizeof cache ||
        setenv("CHIBI_STUDIO_PREFS_PATH", prefs, 1) ||
        setenv("CHIBI_THUMBNAIL_CACHE", cache, 1)) {
        fprintf(stderr, "Could not isolate Studio acceptance preferences/cache\n");
        return 2;
    }
    pu_display_time(true);
    mode = source_edit ? SOURCE_EDIT
         : code_keys   ? CODE_KEYS
         : kdl_keys    ? KDL_KEYS
         : exporting   ? EXPORT_GAME
         : visibility  ? GRAPH_VISIBILITY
         : keys_mode   ? GRAPH_KEYS
         : units       ? UNIT_WARNINGS
         : hierarchy   ? SCENE_HIERARCHY
         : assets      ? ASSET_SELECTION
         : viewport    ? SCENE_VIEWPORT
         : playing     ? PLAY_KEYS
                       : PROMPTS;
    switch (mode) {
        case SOURCE_EDIT: RUN_TEST_CASE(source_graph_edit_undo_redo); break;
        case CODE_KEYS: RUN_TEST_CASE(code_enter_types_newlines); break;
        case KDL_KEYS: RUN_TEST_CASE(kdl_enter_saves); break;
        case EXPORT_GAME: RUN_TEST_CASE(export_game); break;
        case GRAPH_VISIBILITY:
            RUN_TEST_CASE(graph_visibility_and_document_pair);
            break;
        case GRAPH_KEYS: RUN_TEST_CASE(graph_keys); break;
        case UNIT_WARNINGS: RUN_TEST_CASE(code_underlines_unit_mismatch); break;
        case SCENE_HIERARCHY: RUN_TEST_CASE(scene_hierarchy); break;
        case ASSET_SELECTION: RUN_TEST_CASE(asset_selection); break;
        case SCENE_VIEWPORT: RUN_TEST_CASE(scene_viewport); break;
        case PLAY_KEYS: RUN_TEST_CASE(play_keys); break;
        case PROMPTS: RUN_TEST_CASE(unsaved_prompts); break;
    }
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
