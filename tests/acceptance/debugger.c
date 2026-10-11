#include "debugger.h"

#include "authoring/preview_source.h"
#include "editor/documents/asset_code.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor_internal.h"
#include "execution/inspection.h"
#include "execution/status.h"
#include "support/array.h"
#include "vendor/pico_unit.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <string.h>

static const char counter[] = "behavior Counter {\n"
                              "    state count: int = 1\n"
                              "    on Tick {\n"
                              "        count = count + 1\n"
                              "    }\n"
                              "}\n";
static const char invalid_counter[] = "behavior Counter {\n"
                                      "    state count: int = 1\n"
                                      "    on Tick {\n"
                                      "        count = count + @\n"
                                      "    }\n"
                                      "}\n";
static execution_debug_cursor paused;
static uint64_t count_state;
static uint64_t breakpoint_graph, breakpoint_node;
static uint64_t last_step;
static bool assignment_complete;

static SDL_WindowID window_id()
{
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_WindowID id = count ? SDL_GetWindowID(windows[0]) : 0;
    SDL_free(windows);
    return id;
}

static bool press(SDL_Keycode key, SDL_Keymod mod)
{
    SDL_WindowID id = window_id();
    REQUIRE(id);
    for (int down = 1; down >= 0; down--) {
        SDL_Event event = { .key = { .type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP,
                                     .windowID = id,
                                     .key = key,
                                     .mod = mod,
                                     .scancode = SDL_GetScancodeFromKey(key, NULL),
                                     .down = down } };
        REQUIRE(SDL_PushEvent(&event));
    }
    return true;
}

static bool type_invalid()
{
    SDL_WindowID id = window_id();
    REQUIRE(id);
    SDL_Event event = {
        .text = { .type = SDL_EVENT_TEXT_INPUT, .windowID = id, .text = "@" }
    };
    REQUIRE(SDL_PushEvent(&event));
    return true;
}

static asset_code *counter_code(studio *s)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        asset_code *code = s->code.assets[i];
        if (strstr(asset_code_path(code), "/Counter.chibi")) {
            return code;
        }
    }
    return NULL;
}

static bool code_focused(studio *s, asset_code *code)
{
    REQUIRE(s->code.editor == asset_code_editor(code));
    REQUIRE(!strcmp(s->code.editor_path, asset_code_path(code)));
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    REQUIRE(focus && !strcmp(rml_element_tag_name(focus), "kit-code"));
    REQUIRE(rml_element_is_visible(focus, true));
    return true;
}

/* Focus setup must finish before the queued key arrives; an idle UI frame
   need not consume the code panel's pending refocus request. */
static bool focus_code(studio *s, asset_code *code)
{
    rml_context *context = rml_backend_context(s->backend);
    rml_element *text = NULL;
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *candidate = rml_element_query_selector(rml_context_document(context, i), "kit-code");
        if (candidate && rml_element_is_visible(candidate, true)) {
            text = candidate;
            break;
        }
    }
    REQUIRE(text && rml_element_focus(text, true));
    return code_focused(s, code);
}

static bool has_breakpoint(studio *s)
{
    const graph *g = behavior_asset_find_graph(s->scene->graph_editor->asset, breakpoint_graph);
    const node *n = g ? graph_node_by_id(g, breakpoint_node) : NULL;
    REQUIRE(n && n->breakpoint);
    return true;
}

/* Inspect the live program again after each frame; source history may publish
   a collection, so neither program instances nor source rows are cached. */
static bool paused_counter(studio *s, asset_code *code, int count, bool stepped)
{
    execution_program *program = source_program(s->scene->live->runtime->sources, "Counter");
    REQUIRE(program && execution_program_paused(program));
    execution_inspector *inspector = execution_program_inspection(program);
    REQUIRE(inspector);
    execution_debug_cursor cursor = execution_inspector_cursor(inspector);
    REQUIRE(cursor.status == FLOW_PAUSED_RESULT && !cursor.retired);
    REQUIRE(cursor.generation == paused.generation && cursor.run == paused.run);
    REQUIRE(cursor.module == paused.module && cursor.owner == paused.owner);
    REQUIRE(stepped ? cursor.step > paused.step : cursor.step == paused.step);
    if (!stepped) {
        REQUIRE(cursor.point.occurrence == paused.point.occurrence);
    }
    execution_instance *instance = execution_program_instance(program, paused.owner);
    REQUIRE(instance);
    const pin_value *value = execution_instance_state(instance, count_state);
    REQUIRE(value && value->type == PIN_I32 && value->i == count);
    const authoring_source
        *applied = preview_source_find(s->scene->live, asset_code_path(code), true);
    REQUIRE(applied && applied->valid && applied->module);
    REQUIRE(!strcmp(applied->syntax->text, counter));
    REQUIRE(execution_module_id(applied->module) == paused.module);
    REQUIRE(s->pause.active && studio_session(s));
    REQUIRE(studio_command_enabled(s, STUDIO_COMMAND_STEP_OVER));
    return true;
}

static bool draft(studio *s, asset_code *code, bool valid)
{
    REQUIRE(!strcmp(asset_code_text(code), valid ? counter : invalid_counter));
    REQUIRE(valid ? !asset_code_error(code)[0] : asset_code_error(code)[0]);
    REQUIRE(asset_code_can_replay(code, false));
    int start, end;
    bool location = asset_code_execution_location(code, &start, &end);
    REQUIRE(location == valid);
    if (valid) {
        REQUIRE(start == paused.point.start && end == paused.point.end);
        REQUIRE(has_breakpoint(s));
    }
    return paused_counter(s, code, 1, false);
}

static bool step_counter(studio *s, asset_code *code, int frame)
{
    execution_program *program = source_program(s->scene->live->runtime->sources, "Counter");
    REQUIRE(program);
    execution_inspector *inspector = execution_program_inspection(program);
    REQUIRE(inspector);
    execution_debug_cursor cursor = execution_inspector_cursor(inspector);
    REQUIRE(cursor.step > last_step);
    REQUIRE(paused_counter(s, code, cursor.point.completion ? 2 : 1, true));
    REQUIRE(!strcmp(asset_code_text(code), counter));
    REQUIRE(asset_code_can_replay(code, true));
    last_step = cursor.step;
    if (cursor.point.completion) {
        assignment_complete = true;
        return press(SDLK_F5, SDL_KMOD_SHIFT);
    }
    /* At most ten real F10 presses, including the first on frame 25. */
    REQUIRE(frame < ACCEPTANCE_DEBUGGER_FRAMES - 2);
    return press(SDLK_F10, SDL_KMOD_NONE);
}

/* F9 establishes a real breakpoint; an invalid typed draft and source history
   keep that paused runtime. F10 steps through the assignment exactly once. */
bool acceptance_debugger_frame(studio *s, int frame)
{
    REQUIRE(s && s->ui && frame > 0 && frame <= ACCEPTANCE_DEBUGGER_FRAMES);
    if (frame == 1) {
        paused = (execution_debug_cursor){ 0 };
        count_state = breakpoint_graph = breakpoint_node = 0;
        last_step = 0;
        assignment_complete = false;
        s->prefs.code_apply_live = true;
        s->prefs.code_vim = false;
        s->prefs.reduced_motion = true;
        REQUIRE(
            app_scene_authoring_register_prototype(
                s->scene,
                "name \"Ticker\"\ncomponents {\n"
                "    Transform x=0 y=0\n}\n"
                "(array)behaviors \"Counter\"\n",
                NULL
            ) == 0
        );
        REQUIRE(app_scene_authoring_add_instance(s->scene, "Ticker") == 0);
        REQUIRE(app_scene_authoring_open_behavior(s->scene, "Counter") == 0);
        studio_show_editor(s, STUDIO_EDITOR_CODE);
        return true;
    }
    asset_code *code = counter_code(s);
    REQUIRE(code);
    if (frame >= 27 && frame <= ACCEPTANCE_DEBUGGER_FRAMES - 2 && frame % 2 &&
        !assignment_complete) {
        return step_counter(s, code, frame);
    }
    SDL_Keymod primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    switch (frame) {
        case 3: {
            REQUIRE(!strcmp(asset_code_text(code), counter));
            const char *statement = strstr(counter, "count = count + 1");
            REQUIRE(statement);
            int offset = (int)(statement - counter);
            REQUIRE(asset_code_breakpoint_at(code, offset, &breakpoint_graph, &breakpoint_node));
            kit_code_set_cursor(asset_code_editor(code), offset);
            REQUIRE(focus_code(s, code));
            break;
        }
        case 5:
            REQUIRE(code_focused(s, code));
            return press(SDLK_F9, SDL_KMOD_NONE);
        case 7:
            REQUIRE(code_focused(s, code));
            REQUIRE(has_breakpoint(s));
            REQUIRE(!strcmp(asset_code_text(code), counter));
            return press(SDLK_F5, SDL_KMOD_NONE);
        /* Play resets the engine clock; its first frame seeds elapsed time. */
        case 10: {
            execution_program *program = source_program(s->scene->live->runtime->sources, "Counter");
            REQUIRE(program && execution_program_paused(program));
            execution_inspector *inspector = execution_program_inspection(program);
            REQUIRE(inspector);
            paused = execution_inspector_cursor(inspector);
            REQUIRE(paused.generation && paused.run && paused.step);
            REQUIRE(
                !paused.point.completion && paused.point.start < paused.point.end
            );
            const authoring_source *
                source = preview_source_find(s->scene->live, asset_code_path(code), true);
            REQUIRE(source && source->valid);
            for (int i = 0; i < source->analysis->symbol_count; i++) {
                const script_symbol *symbol = &source->analysis->symbols[i];
                if (symbol->kind == SCRIPT_STATE && !strcmp(symbol->name, "count")) {
                    count_state = symbol->id;
                }
            }
            REQUIRE(count_state && draft(s, code, true));
            const char *expression = strstr(counter, "count + 1");
            REQUIRE(expression);
            int offset = (int)(expression - counter) + (int)strlen("count + ");
            kit_code_select(asset_code_editor(code), offset, offset + 1);
            REQUIRE(focus_code(s, code));
            break;
        }
        case 11: REQUIRE(code_focused(s, code)); return type_invalid();
        case 13:
            REQUIRE(code_focused(s, code));
            REQUIRE(draft(s, code, false));
            return press(SDLK_Z, primary);
        case 15:
            REQUIRE(code_focused(s, code));
            REQUIRE(draft(s, code, true));
            REQUIRE(asset_code_can_replay(code, true));
            return press(SDLK_Z, primary | SDL_KMOD_SHIFT);
        case 17:
            REQUIRE(code_focused(s, code));
            REQUIRE(draft(s, code, false));
            return press(SDLK_Z, primary);
        case 25:
            REQUIRE(code_focused(s, code));
            REQUIRE(!asset_code_pending(code));
            REQUIRE(draft(s, code, true));
            REQUIRE(asset_code_can_replay(code, true));
            last_step = paused.step;
            return press(SDLK_F10, SDL_KMOD_NONE);
        case ACCEPTANCE_DEBUGGER_FRAMES:
            REQUIRE(assignment_complete);
            REQUIRE(!studio_session(s) && !s->pause.active);
            REQUIRE(!strcmp(asset_code_text(code), counter));
            REQUIRE(!studio_command_enabled(s, STUDIO_COMMAND_STEP_OVER));
            break;
    }
    return true;
}
