#include "graph/authoring/graph_edit.h"
#include "graph/nodes/core_nodes.h"
/* Debugging: jumping to a node, the open behavior's compile problems, the
 * playing scene's runtime errors (Unreal's PIE message log) and a run paused
 * at a breakpoint (Unreal's Blueprint debugger): stepping, the paused node
 * and its call stack. */
#include "authoring/preview_runtime.h"
#include "authoring/preview_source.h"
#include "authoring/source_presentation.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/ui/labels.h"
#include "execution/status.h"
#include "graph/document/library.h"
#include "support/array.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Jumping to a node ---------- */

static int behavior_area(const studio_layout *layout)
{
    int area = -1;
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        area = layout->editors[i] == STUDIO_EDITOR_BEHAVIOR ? i : area;
    }
    return area;
}

/* A Behavior area shows the graph. If the active split has none, the bottom
   area becomes one; a different maximized area gives way to it. */
static void show_behavior_area(studio *s)
{
    int area = behavior_area(studio_active_layout(s));
    if (area < 0) {
        area = STUDIO_AREA_BOTTOM;
        studio_active_layout(s)->editors[area] = STUDIO_EDITOR_BEHAVIOR;
    }
    if (s->maximized >= 0 && s->maximized != area) {
        s->maximized = -1;
    }
}

/* The graph editor holds `asset`: a behavior, or a system's graphs. */
static bool behavior_open(const studio *s, const char *asset)
{
    const char *open = s->scene->graph_document_id;
    return open && !strcmp(open, asset);
}

/* Opens the behavior and graph `at` names in a Behavior area. */
static bool open_graph(studio *s, const studio_node_ref *at)
{
    if ((!behavior_open(s, at->asset) &&
         app_scene_authoring_open_behavior(s->scene, at->asset) != 0) ||
        editor_open_graph(sg_graph_editor(s), at->graph_id) != 0) {
        return false;
    }
    show_behavior_area(s);
    return true;
}

/* The node `at` names in the open graph, or the node it is folded into. */
static node *shown_node(const studio *s, const studio_node_ref *at)
{
    graph *gr = sg_active_graph(s);
    node *n = gr && at->node_id ? graph_node_by_id(gr, at->node_id) : NULL;
    return n ? graph_inline_owner(gr, n) : NULL;
}

void studio_reveal(studio *s, const studio_node_ref *at)
{
    if (!open_graph(s, at)) {
        char message[192];
        snprintf(message, sizeof message, "Could not open %s", at->asset);
        studio_report(s, message);
        return;
    }
    node *n = shown_node(s, at);
    if (n) {
        sg_select_only(sg_graph_editor(s), n);
        s->graph.reveal = n->id;
        s->graph.reveal_frame = true;
    }
}

void studio_node_place(const studio *s, const studio_node_ref *at, char *out, size_t size)
{
    const editor *e = sg_graph_editor(s);
    graph *g = behavior_open(s, at->asset)
                 ? behavior_asset_find_graph(e->asset, at->graph_id)
                 : NULL;
    node *n = g && at->node_id ? graph_node_by_id(g, at->node_id) : NULL;
    char name[256] = "Event Graph";
    if (g) {
        studio_code_graph_label(s, g, name, sizeof name);
    }
    if (!n) {
        snprintf(out, size, "%s", g ? name : at->asset);
        return;
    }
    char title[128];
    ed_node_title(e, graph_inline_owner(g, n), title, sizeof title);
    snprintf(out, size, "%s \xe2\x80\xba %s", name, title);
}

/* ---------- Where running nodes were authored ---------- */

/* A running behavior's graphs include copies of the libraries it calls. */
static studio_node_ref authored(const char *behavior, const graph *g, uint64_t node_id)
{
    graph_node_source source = graph_node_authored(g, node_id);
    studio_node_ref at = { .graph_id = source.graph_id, .node_id = source.node_id };
    snprintf(at.asset, sizeof at.asset, "%s", source.asset ? source.asset : behavior);
    return at;
}

/* A behavior compiles linked: with copies of the libraries it calls and of
   its macros. */
studio_node_ref studio_problem_at(const studio *s, const compile_diagnostic *d)
{
    const editor *e = sg_graph_editor(s);
    const char *behavior = s->scene->graph_document_id ? s->scene->graph_document_id
                                                       : "";
    if (!e->compiled) {
        studio_node_ref at = { .graph_id = d->graph_id, .node_id = d->node_id };
        snprintf(at.asset, sizeof at.asset, "%s", behavior);
        return at;
    }
    const graph *g = behavior_asset_find_graph(e->compiled, d->graph_id);
    assert(g);
    return authored(behavior, g, d->node_id);
}

/* ---------- Runtime errors ---------- */

static source_registry *running_world(const studio *s)
{
    app_scene_authoring *scene = s->scene;
    return scene->active_runtime_target == APP_RUNTIME_PREVIEW
             ? prototype_preview_sources(scene->prototype_preview)
             : scene->live->runtime->sources;
}

static bool same_error(const studio_runtime_error *a, const studio_runtime_error *b)
{
    return !strcmp(a->behavior, b->behavior) && !strcmp(a->at.asset, b->at.asset) &&
           a->at.graph_id == b->at.graph_id && a->at.node_id == b->at.node_id &&
           !strcmp(a->path, b->path) && a->start == b->start &&
           a->end == b->end && !strcmp(a->message, b->message);
}

static void add_error(studio *s, const studio_runtime_error *error)
{
    for (int i = 0; i < acount(s->runtime_errors); i++) {
        if (same_error(&s->runtime_errors[i], error)) {
            s->runtime_errors[i].count++;
            return;
        }
    }
    apush(s->runtime_errors, *error);
}

static void collect_source_errors(studio *s, source_registry *sources_registry)
{
    const authoring_sources *sources = preview_sources_applied(s->scene->live);
    for (int i = 0; sources && i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        execution_program
            *program = source_program(sources_registry, source->analysis->name);
        const execution_program_failure *failure = program
                                                     ? execution_program_error(program)
                                                     : NULL;
        if (!failure) {
            continue;
        }
        studio_runtime_error error = { .count = 1,
                                       .start = failure->start,
                                       .end = failure->end,
                                       .line = failure->line };
        const char *name = failure->asset_name[0] ? failure->asset_name
                                                  : source->analysis->name;
        snprintf(error.behavior, sizeof error.behavior, "%s", source->analysis->name);
        snprintf(error.at.asset, sizeof error.at.asset, "%s", name);
        snprintf(error.message, sizeof error.message, "%s", failure->message);
        const authoring_source *origin_source = NULL;
        for (int j = 0; j < authoring_sources_count(sources); j++) {
            const authoring_source *candidate = authoring_sources_at(sources, j);
            if (candidate->module &&
                execution_module_id(candidate->module) == failure->module_id) {
                origin_source = candidate;
                break;
            }
        }
        if (origin_source) {
            snprintf(
                error.at.asset,
                sizeof error.at.asset,
                "%s",
                origin_source->analysis->name
            );
            snprintf(error.path, sizeof error.path, "%s", origin_source->path);
        }
        snprintf(error.node, sizeof error.node, "line %d", failure->line);
        preview_definition *definition = origin_source
                                           ? preview_find_definition(
                                                 s->scene->live,
                                                 origin_source->analysis->name
                                             )
                                           : NULL;
        const authoring_source_presentation *presentation = definition
                                                              ? definition->applied_presentation
                                                              : NULL;
        const source_presentation_origin *chosen = NULL;
        for (int j = 0; presentation &&
                        j < authoring_source_presentation_origin_count(presentation);
             j++) {
            const source_presentation_origin
                *origin = authoring_source_presentation_origin_at(presentation, j);
            if (failure->end <= failure->start || origin->pin ||
                origin->range.start > failure->start ||
                origin->range.end < failure->end) {
                continue;
            }
            if (!chosen || origin->range.end - origin->range.start <
                               chosen->range.end - chosen->range.start) {
                chosen = origin;
            }
        }
        if (chosen) {
            error.at.graph_id = chosen->graph;
            error.at.node_id = chosen->node;
        }
        add_error(s, &error);
        execution_program_clear_error(program);
    }
}

/* Copy log text into editor-owned rows before a publication retires its source
 * generation. Program failures retain their own authored source coordinates. */
static void collect_errors(studio *s, preview_runtime *runtime)
{
    editor *log = s->scene->graph_editor;
    for (int i = 0; log && i < preview_runtime_log_count(runtime); i++) {
        const preview_log *entry = preview_runtime_log_at(runtime, i);
        editor_log_print(log, entry->program, entry->text);
    }
    preview_runtime_clear_logs(runtime);
    collect_source_errors(s, runtime->sources);
}

/* ---------- Compile problems ---------- */

static uint64_t revision_key(const studio *s)
{
    const editor *e = sg_graph_editor(s);
    uint64_t key = 14695981039346656037ull;
    for (const char *c = s->scene->graph_document_id; *c; c++) {
        key = (key ^ (unsigned char)*c) * 1099511628211ull;
    }
    key = (key ^ editor_edit_revision(e)) * 1099511628211ull;
    for (int i = 0; i < acount(e->asset->graphs); i++) {
        key = (key ^ graph_topology_revision(e->asset->graphs[i])) * 1099511628211ull;
    }
    return key;
}

/* The open behavior compiles again whenever its graphs change, so Problems
   and node badges stay current without a Compile button. */
static void compile_on_change(studio *s)
{
    if (s->scene->graph_document_kind != APP_EDITOR_DOCUMENT_BEHAVIOR ||
        !studio_graph_active(s) || editor_presentation(sg_graph_editor(s)) ||
        revision_key(s) == s->compiled_key) {
        return;
    }
    editor_command_apply(sg_graph_editor(s), (editor_command){ .kind = EDITOR_COMMAND_COMPILE });
    s->compiled_key = revision_key(s);
}

/* ---------- The paused run ---------- */

/* Owns the returned handle; enumeration does not borrow an activation. */
static execution_inspector *paused_run(const studio *s, scene_debug_attachment *info)
{
    source_registry *registry = running_world(s);
    const authoring_sources *sources = preview_sources_applied(s->scene->live);
    for (int i = 0; sources && i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        execution_program *program = source_program(registry, source->analysis->name);
        if (!program) {
            continue;
        }
        execution_inspector *inspector = execution_program_inspection(program);
        if (!inspector) {
            continue;
        }
        execution_debug_cursor cursor = execution_inspector_cursor(inspector);
        if (!cursor.retired && cursor.status == FLOW_PAUSED_RESULT) {
            *info = (scene_debug_attachment){
                .entity = ecs_entity_from_u64(cursor.owner),
                .asset_name = source->analysis->name,
                .module = source->module,
                .program = program,
                .instance = execution_program_instance(program, cursor.owner)
            };
            return execution_inspector_retain(inspector);
        }
    }
    return NULL;
}

static const authoring_source *point_source(const studio *s, uint64_t module, bool retired)
{
    const authoring_sources *sources = retired
                                         ? s->scene->debug.sources
                                         : preview_sources_applied(s->scene->live);
    for (int i = 0; sources && i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        if (source->module && execution_module_id(source->module) == module) {
            return source;
        }
    }
    return NULL;
}

static bool point_source_current(const studio *s, const authoring_source *source)
{
    if (!source) {
        return false;
    }
    const authoring_source *current = preview_source_find(s->scene->live, source->path, false);
    return current && !strcmp(current->syntax->text, source->syntax->text);
}

static studio_node_ref point_node(const studio *s, const authoring_source *source, bool retired, behavior_source_point point)
{
    studio_node_ref at = { 0 };
    if (!point_source_current(s, source)) {
        return at;
    }
    snprintf(at.asset, sizeof at.asset, "%s", source->analysis->name);
    preview_definition *definition = preview_find_definition(s->scene->live, at.asset);
    const authoring_source_presentation *view = definition ? definition->applied_presentation
                                                           : NULL;
    const authoring_sources *generation = retired
                                            ? s->scene->debug.sources
                                            : preview_sources_applied(s->scene->live);
    if (view &&
        !authoring_source_presentation_matches(view, source->analysis, authoring_sources_catalog(generation))) {
        return at;
    }
    for (int i = 0; view && i < authoring_source_presentation_origin_count(view); i++) {
        const source_presentation_origin *origin = authoring_source_presentation_origin_at(view, i);
        if (!origin->pin && origin->occurrence == point.occurrence) {
            at.graph_id = origin->graph;
            at.node_id = origin->node;
            break;
        }
    }
    return at;
}

static void open_source_point(studio *s, const char *path, int offset, int line)
{
    ui_code_place place = { .offset = offset, .line = line };
    snprintf(place.path, sizeof place.path, "%s", path);
    studio_code_open_place(s, &place);
}

static void follow_pause(studio *s)
{
    app_scene_authoring *scene = s->scene;
    scene_debug_attachment info;
    execution_inspector *inspector = paused_run(s, &info);
    s->pause.active = inspector != NULL;
    if (!inspector) {
        if (s->pause.holding) {
            s->pause.holding = false;
            app_scene_authoring_set_debug_pinned(scene, s->pause.pinned);
        }
        s->pause.generation = s->pause.run = s->pause.occurrence = 0;
        return;
    }
    execution_debug_cursor cursor = execution_inspector_cursor(inspector);
    const authoring_source *source = point_source(s, cursor.module, cursor.retired);
    s->pause.at = point_node(s, source, cursor.retired, cursor.point);
    if (cursor.generation == s->pause.generation && cursor.run == s->pause.run &&
        cursor.point.occurrence == s->pause.occurrence &&
        cursor.step == s->pause.steps) {
        execution_inspector_free(inspector);
        return;
    }
    s->pause.generation = cursor.generation;
    s->pause.run = cursor.run;
    s->pause.occurrence = cursor.point.occurrence;
    s->pause.steps = cursor.step;
    if (!s->pause.holding) {
        s->pause.holding = true;
        s->pause.pinned = scene->debug_pinned;
        if (scene->active_runtime_target == APP_RUNTIME_SCENE) {
            app_scene_authoring_select_entity(scene, info.entity);
        }
    }
    if (s->pause.at.node_id && open_graph(s, &s->pause.at)) {
        node *n = shown_node(s, &s->pause.at);
        s->graph.reveal = n ? n->id : 0;
        s->graph.reveal_frame = false;
        studio_code_reveal_node(s, &s->pause.at);
    } else if (point_source_current(s, source)) {
        open_source_point(s, source->path, cursor.point.start, cursor.point.line);
    }
    app_scene_authoring_set_debug_target(scene, scene->active_runtime_target, info.entity, info.asset_name);
    execution_inspector_free(inspector);
}

bool studio_debug(studio *s, studio_debug_command command)
{
    scene_debug_attachment info;
    execution_inspector *inspector = paused_run(s, &info);
    if (!inspector) {
        return false;
    }
    execution_debug_control control = EXECUTION_DEBUG_RESUME;
    switch (command) {
        case STUDIO_RESUME: break;
        case STUDIO_STEP_OVER: control = EXECUTION_DEBUG_OVER; break;
        case STUDIO_STEP_INTO: control = EXECUTION_DEBUG_INTO; break;
        case STUDIO_STEP_OUT: control = EXECUTION_DEBUG_OUT; break;
    }
    int status = execution_inspector_control(inspector, control);
    execution_inspector_free(inspector);
    return status >= FLOW_OK;
}

void studio_debug_frame(studio *s)
{
    app_scene_authoring *scene = s->scene;
    bool session = runtime_session(scene);
    if (session && !s->session) {
        asetlen(s->runtime_errors, 0);
    }
    s->session = session;
    collect_errors(s, scene->live->runtime);
    collect_errors(s, prototype_preview_live(scene->prototype_preview)->runtime);
    compile_on_change(s);
    follow_pause(s);
}

/* ---------- Break when spawned ---------- */

void studio_toggle_spawn_break(studio *s, const char *prototype)
{
    bool on = !app_scene_authoring_spawn_break(s->scene, prototype);
    app_scene_authoring_set_spawn_break(s->scene, prototype, on);
    char report[160];
    snprintf(report, sizeof report, on ? "Play pauses when a %s spawns" : "A %s spawning no longer pauses Play", prototype);
    studio_report(s, report);
}

void studio_spawn_break_item(studio *s, const char *prototype)
{
    kit_menu_item_desc d = {
        .label = "Break When Spawned",
        .checked = app_scene_authoring_spawn_break(s->scene, prototype),
    };
    if (kit_menu_item(s->kit, "spawn-break", &d)) {
        studio_toggle_spawn_break(s, prototype);
    }
}

/* ---------- Playback speed ---------- */

/* Unity's Time.timeScale and Unreal's slomo as steps, as Chrome DevTools
   offers its animations' speeds. */
const double studio_speeds[] = { STUDIO_SPEED_MIN, 0.25, 0.5, 1, STUDIO_SPEED_MAX };
const int studio_speed_count = sizeof studio_speeds / sizeof *studio_speeds;

void studio_speed_label(double speed, char *buf, size_t size)
{
    snprintf(buf, size, "%g\xc3\x97", speed);
}

void studio_speed_text(const studio *s, char *buf, size_t size)
{
    double speed = app_scene_authoring_time_scale(s->scene);
    buf[0] = '\0';
    if (speed != 1) {
        studio_speed_label(speed, buf, size);
    }
}

void studio_speed_step(studio *s, int direction)
{
    double speed = app_scene_authoring_time_scale(s->scene),
           next = direction ? speed : 1;
    /* The last match is the nearest step: up from the top, down from the bottom. */
    for (int i = 0; i < studio_speed_count; i++) {
        double up = studio_speeds[studio_speed_count - 1 - i],
               down = studio_speeds[i];
        if (direction > 0 && up > speed) {
            next = up;
        }
        if (direction < 0 && down < speed) {
            next = down;
        }
    }
    app_scene_authoring_set_time_scale(s->scene, next);
}

/* ---------- Call stack ---------- */

typedef struct
{
    studio_node_ref at;
    const char *path;
    int offset, line;
    char label[128], graph[96];
} stack_row;

void studio_debug_call_stack(studio *s)
{
    scene_debug_attachment info = { .asset_name = s->scene->debug.asset_name };
    execution_inspector *inspector = s->pause.active ? paused_run(s, &info) : NULL;
    if (!inspector && s->scene->debug.inspector &&
        execution_inspector_owner_frame_count(
            s->scene->debug.inspector,
            ecs_entity_to_u64(s->scene->debug.entity)
        )) {
        inspector = execution_inspector_retain(s->scene->debug.inspector);
    }
    if (!inspector) {
        return;
    }
    kit *k = s->kit;
    stack_row *rows = NULL;
    execution_debug_cursor cursor = execution_inspector_cursor(inspector);
    uint64_t owner = s->pause.active ? cursor.owner
                                     : ecs_entity_to_u64(s->scene->debug.entity);
    bool recorded = execution_inspector_owner_cursor(inspector, owner, &cursor);
    for (int i = 0;
         recorded && i < execution_inspector_owner_frame_count(inspector, owner);
         i++) {
        execution_debug_frame frame = execution_inspector_owner_frame_at(inspector, owner, i);
        const authoring_source *source = point_source(s, frame.module, cursor.retired);
        stack_row row = { .at = point_node(s, source, cursor.retired, frame.point),
                          .path = point_source_current(s, source) ? source->path : NULL,
                          .offset = frame.point.start,
                          .line = frame.point.line };
        snprintf(row.label, sizeof row.label, "Line %d", frame.point.line);
        snprintf(row.graph, sizeof row.graph, "%s", source ? source->analysis->name : "External source");
        if (source) {
            for (int j = 0; j < source->analysis->function_count; j++) {
                const script_function *function = &source->analysis->functions[j];
                if (source->analysis->symbols[function->symbol].id == frame.function) {
                    snprintf(
                        row.graph,
                        sizeof row.graph,
                        "%s",
                        source->analysis->symbols[function->symbol].name
                    );
                    break;
                }
            }
        }
        apush(rows, row);
    }
    kit_push(k, "stack", NULL, "call-stack");
    kit_presence(k);
    kit_push(k, "head", NULL, "call-stack-head");
    kit_label(k, "title", NULL, "Call Stack");
    kit_label(k, "owner", "dim", info.asset_name ? info.asset_name : "Retired target");
    kit_pop(k);
    int top = acount(rows) - 1;
    for (int i = top; i >= 0; i--) {
        char key[16];
        snprintf(key, sizeof key, "f%d", i);
        kit_row_desc d = { .label = rows[i].label,
                           .icon = i == top ? KIT_ICON_FRAME_TOP : KIT_ICON_FRAME_CALL,
                           .twisty = -1,
                           .meta = rows[i].graph,
                           .flags = i == top ? KIT_ON : 0 };
        if (kit_row_begin(k, key, &d) & KIT_ROW_CLICK) {
            if (rows[i].at.node_id) {
                studio_reveal(s, &rows[i].at);
            } else if (rows[i].path) {
                open_source_point(s, rows[i].path, rows[i].offset, rows[i].line);
            }
        }
        kit_row_end(k);
    }
    kit_pop(k);
    afree(rows);
    execution_inspector_free(inspector);
}
