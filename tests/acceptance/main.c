#define PICO_UNIT_IMPLEMENTATION
#include "assets/input_map.h"
#include "authoring/preview_source.h"
#include "authoring/source_presentation.h"
#include "editor/collab/collab.h"
#include "editor/documents/asset_code.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/scene_internal.h"
#include "support/array.h"
#include "support/file_io.h"
#include "support/xalloc.h"
#include "vendor/pico_unit.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_video.h>
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
    EXPORT_GAME,
    GRAPH_VISIBILITY,
    UNIT_WARNINGS
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
    REQUIRE(!studio_quit(s, false) && !s->quit_toast);
    REQUIRE(!studio_quit(s, true) && s->quit_toast);
    int toast = s->quit_toast;
    REQUIRE(!studio_quit(s, true) && s->quit_toast == toast);
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
    REQUIRE(!s->open_project && !s->discard_toast);
    return true;
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
   for the completion list to open. */
#define KEY_FRAMES 12
#define TEXT(x) #x
#define FRAMES_TEXT(x) TEXT(x)

/* Queues a key press and release for the window, as the platform would. */
static bool press_key(SDL_Keycode key)
{
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_WindowID window = count ? SDL_GetWindowID(windows[0]) : 0;
    SDL_free(windows);
    REQUIRE(window);
    for (int down = 1; down >= 0; down--) {
        SDL_Event e = { .key = { .type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP,
                                 .windowID = window,
                                 .key = key,
                                 .scancode = SDL_GetScancodeFromKey(key, NULL),
                                 .down = down } };
        REQUIRE(SDL_PushEvent(&e));
    }
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

/* Enter on an empty line types newlines; it never accepts a completion the
   user did not ask for. */
static bool code_keys_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= KEY_FRAMES);
    asset_code *code = counter_code(s);
    REQUIRE(code);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_CODE);
    } else if (frames == KEY_FRAMES - 8) {
        s->code.find.refocus = true;
        const char *close = strstr(asset_code_text(code), "    }\n}");
        REQUIRE(close);
        kit_code_set_cursor(asset_code_editor(code), (int)(close - asset_code_text(code)) + 5);
    } else if (frames == KEY_FRAMES - 6 || frames == KEY_FRAMES - 3) {
        REQUIRE(press_key(SDLK_RETURN));
        keys++;
    } else if (frames == KEY_FRAMES) {
        const char *text = asset_code_text(code);
        REQUIRE(strlen(text) > strlen(counter_bytes) + 1);
        REQUIRE(same_without_space(text, counter_bytes));
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
        studio_behavior_show(s, 2);
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

bool studio_acceptance_frame(void *state)
{
    switch (mode) {
        case SOURCE_EDIT: callback_ok = source_edit_frame(state); break;
        case CODE_KEYS: callback_ok = code_keys_frame(state); break;
        case EXPORT_GAME: callback_ok = export_frame(state); break;
        case GRAPH_VISIBILITY:
            callback_ok = graph_visibility_frame(state);
            break;
        case UNIT_WARNINGS: callback_ok = unit_warnings_frame(state); break;
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
    REQUIRE(frames == KEY_FRAMES && keys == 2);
    printf("Studio code keys accepted: frames=%d keys=%d\n", frames, keys);
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

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--scene-cursors")) {
        RUN_TEST_CASE(scene_gizmo_cursors);
        pu_print_stats();
        return pu_test_failed() ? 1 : 0;
    }
    bool source_edit = argc == 4 && !strcmp(argv[1], "--source-edit");
    bool code_keys = argc == 4 && !strcmp(argv[1], "--code-keys");
    bool exporting = argc == 4 && !strcmp(argv[1], "--export-game");
    bool visibility = argc == 4 && !strcmp(argv[1], "--graph-visibility");
    bool units = argc == 4 && !strcmp(argv[1], "--unit-warnings");
    if ((!source_edit && !code_keys && !exporting && !visibility && !units) ||
        strcmp(argv[2], "--project-root")) {
        fprintf(
            stderr,
            "Usage: studio-acceptance "
            "--source-edit|--code-keys|--export-game|--graph-visibility|"
            "--unit-warnings "
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
         : visibility  ? GRAPH_VISIBILITY
         : units       ? UNIT_WARNINGS
                       : EXPORT_GAME;
    switch (mode) {
        case SOURCE_EDIT: RUN_TEST_CASE(source_graph_edit_undo_redo); break;
        case CODE_KEYS: RUN_TEST_CASE(code_enter_types_newlines); break;
        case EXPORT_GAME: RUN_TEST_CASE(export_game); break;
        case GRAPH_VISIBILITY:
            RUN_TEST_CASE(graph_visibility_and_document_pair);
            break;
        case UNIT_WARNINGS: RUN_TEST_CASE(code_underlines_unit_mismatch); break;
    }
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
