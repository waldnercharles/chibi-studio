#include "support/asset_document.h"
#define PICO_UNIT_IMPLEMENTATION
#include "assets/input_map.h"
#include "authoring/preview_source.h"
#include "authoring/source_asset.h"
#include "authoring/source_presentation.h"
#include "debugger.h"
#include "editor/collab/collab.h"
#include "editor/documents/asset_code.h"
#include "editor/graph/search.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_members_internal.h"
#include "editor/studio/studio_properties_internal.h"
#include "editor/studio/studio_thumbnails.h"
#include "editor/workspace/asset_refactor.h"
#include "editor/workspace/scene_internal.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/local_edit.h"
#include "graph/model/local.h"
#include "graph/model/registry.h"
#include "graph/nodes/core_nodes.h"
#include "graph/nodes/vector_nodes.h"
#include "runtime/scenes/physics3d.h"
#include "support/arena.h"
#include "support/array.h"
#include "support/file_io.h"
#include "support/text_overlay.h"
#include "support/xalloc.h"
#include "vendor/pico_unit.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Own native-string projections until the selected acceptance command exits. */
static arena acceptance_field_reads;
static void acceptance_field_reads_free()
{
    arena_free(&acceptance_field_reads);
}

#include "scene_read.h"

studio *studio_acceptance_studio(void *);
int studio_acceptance_host_main(int, char **);

static const char *project;
static const char *executable;
static int frames, edits, undos, redos, keys;
static bool callback_ok = true;
static enum {
    SOURCE_EDIT,
    CODE_KEYS,
    ASSET_KEYS,
    EXPORT_GAME,
    GRAPH_VISIBILITY,
    GRAPH_KEYS,
    GRAPH_POINTER,
    DEBUGGER_SOURCE_HISTORY,
    ASSET_POINTER,
    GRAPH_LABELS,
    UNIT_WARNINGS,
    SCENE_HIERARCHY,
    SCENE_MOVE,
    PROTOTYPE_SOURCE,
    PROTOTYPE_SOURCE_AUTHORITY,
    TYPED_PROTOTYPE_PREVIEW,
    COMPILED_ASSET_EDIT,
    OUTLINER_MOVE,
    OUTLINER_ROWS,
    SCENE_PHYSICS,
    ASSET_SELECTION,
    SCENE_VIEWPORT,
    SCENE_CAMERA,
    SCENE_GIZMO3D,
    SCENE_COLLIDER3D,
    PLAY_KEYS,
    PROMPTS,
    CODE_FOLLOWUPS,
    CODE_GLYPHS,
    PANEL_IDLE
} mode;
static char exported[1024]; /* the export Studio reported */
static const char counter_bytes[] = "Counter :: behavior {\n"
                                    "    count: retain int = 1\n"
                                    "    on Tick :: () {\n"
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
        *applied = preview_source_find(s->scene->live, asset_code_path(code), true);
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
    fputs(
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"Jump\", data.Object([\n"
        "                data.Field.Named(\"sources\", data.List([\n"
        "                    data.Value.Text(\"Space\")\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        file
    );
    fclose(file);
    app_disk_result result = app_scene_authoring_reload_path(s->scene, path);
    remove(path);
    free(path);
    REQUIRE(result == APP_DISK_RELOADED);
    const authoring_sources *sources = preview_sources_working(s->scene->live);
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

static node *source_node(asset_code *code, editor *e, const char *text)
{
    const char *source = asset_code_text(code), *at = strstr(source, text);
    uint64_t graph = 0, id = 0;
    if (!at || !asset_code_node_at(code, (int)(at - source), &graph, &id) ||
        editor_open_graph(e, graph) != 0) {
        return NULL;
    }
    node *n = graph_node_by_id(e->active_graph, id);
    return n ? graph_inline_owner(e->active_graph, n) : NULL;
}

static bool flow_port(node *n, bool output, const char *name, ed_pin_view *port)
{
    const pin_spec *pins = output ? n->output_pins : n->input_pins;
    int count = output ? n->output_count : n->input_count;
    for (int i = 0; i < count; i++) {
        if (pins[i].kind == PIN_EXEC && !strcmp(pins[i].name, name)) {
            *port = (ed_pin_view){ n, output ? ED_PIN_OUTPUT : ED_PIN_INPUT, i };
            return true;
        }
    }
    return false;
}

static bool source_catalog_current(studio *s)
{
    editor *e = sg_graph_editor(s);
    REQUIRE(acount(e->asset_catalog) == preview_definition_count(s->scene->live));
    for (int i = 0; i < acount(e->asset_catalog); i++) {
        REQUIRE(e->asset_catalog[i] == preview_definition_at(s->scene->live, i)->working);
    }
    return true;
}

/* Mount and activate the authored button through its real Rml binding. The
   panel consumes its queued activation in the second composition pass; neither
   pass adds a host frame or renders the GPU. */
static bool source_member_add_button(studio *s)
{
    studio_ui_resync(s, "members");
    studio_ui_frame(s);
    kit_begin(s->kit, &s->in);
    kit_root(s->kit, "app");
    rml_element *host = studio_ui_panel(s, "acceptance-members", "Members");
    kit_end(s->kit);
    REQUIRE(host);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *list = rml_element_query_selector(host, "#members-list");
    REQUIRE(list);
    rml_element_set_property(list, "height", "64dp");
    rml_element_set_property(list, "max-height", "64dp");
    rml_context_update(context);
    rml_element *button = rml_element_query_selector(host, "#members-list button[data-tip='New Variable']");
    REQUIRE(button && !rml_element_has_attribute(button, "data-disabled"));
    rml_element_dispatch_event(button, "click", &(rml_event){ .button = 0 });
    kit_begin(s->kit, &s->in);
    kit_root(s->kit, "app");
    studio_ui_panel(s, "acceptance-members", "Members");
    kit_end(s->kit);
    return true;
}

/* Layout the expanded list, then let its normal next composition retry the
   reveal against the new scroll range. The viewport deliberately overflows. */
static bool source_member_row_visible(studio *s)
{
    rml_element *host = NULL;
    for (int i = 0; i < 2; i++) {
        studio_ui_resync(s, "members");
        studio_ui_frame(s);
        kit_begin(s->kit, &s->in);
        kit_root(s->kit, "app");
        host = studio_ui_panel(s, "acceptance-members", "Members");
        kit_end(s->kit);
        REQUIRE(host);
        rml_context_update(rml_backend_context(s->backend));
    }
    rml_element *list = rml_element_query_selector(host, "#members-list");
    REQUIRE(list);
    rml_element *selected = rml_element_query_selector(list, ".tree-row[data-selected]");
    REQUIRE(selected);
    float view = rml_element_client_height(list);
    REQUIRE(view > 0 && rml_element_scroll_height(list) > view);
    float top = rml_element_absolute_offset(list, RML_BOX_PADDING).y;
    float selected_top = rml_element_absolute_offset(selected, RML_BOX_BORDER).y;
    float selected_bottom = selected_top +
                            rml_element_box_size(selected, RML_BOX_BORDER).y;
    REQUIRE(selected_top >= top - 1 && selected_bottom <= top + view + 1);
    return true;
}

static bool source_member_edits(studio *s, const char *probe)
{
    asset_code *code = counter_source(s);
    editor *e = sg_graph_editor(s);
    uint64_t id = 0;
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation);
         i++) {
        const source_presentation_origin
            *o = authoring_source_presentation_origin_at(e->presentation, i);
        if (o->kind == SOURCE_PRESENTATION_SYMBOL && !o->pin &&
            o->function < 0 && o->operation == SCRIPT_STATE) {
            node *n = graph_node_by_id(behavior_asset_find_graph(e->asset, o->graph), o->node);
            if (!strcmp(n->desc->name, "count")) {
                id = n->reference_id;
                break;
            }
        }
    }
    REQUIRE(id && sm_source_variable(s, id));
    ui_activation member = { .argc = 1 };
    snprintf(member.args[0], sizeof member.args[0], "v%llu", (unsigned long long)id);
    studio_members_action(s, "click", &member);
    REQUIRE(s->members.kind == STUDIO_MEMBER_VARIABLE && s->members.id == id);
    const source_presentation_origin *o = sm_source_variable(s, id);
    REQUIRE(o && e->active_graph->graph_id == o->graph);
    REQUIRE(acount(e->selected) == 1 && e->selected[0]->id == o->node);
    int tab = s->tab;
    s->tab = STUDIO_TAB_MEMBER;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    studio_graph_details_add(doc);
    yyjson_mut_val *details = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "graph_details");
    studio_panel_sync sync = { .doc = doc, .variable = details };
    studio_graph_details_sync(s, &sync);
    yyjson_mut_val *panels = yyjson_mut_obj_get(details, "panels");
    bool initial = false;
    for (size_t i = 0; i < yyjson_mut_arr_size(panels); i++) {
        yyjson_mut_val *rows = yyjson_mut_obj_get(yyjson_mut_arr_get(panels, i), "rows");
        for (size_t j = 0; j < yyjson_mut_arr_size(rows); j++) {
            yyjson_mut_val *row = yyjson_mut_arr_get(rows, j);
            const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(row, "label"));
            initial |= label && !strcmp(label, "Initial");
        }
    }
    REQUIRE(initial);
    yyjson_mut_doc_free(doc);
    s->tab = tab;
    s->graph.details.key = 0;
    studio_members_action(s, "rename", &member);
    snprintf(member.args[0], sizeof member.args[0], "total");
    studio_members_action(s, "renamed", &member);
    REQUIRE(strstr(asset_code_text(code), "total: retain int = 1"));
    REQUIRE(!strstr(asset_code_text(code), "count"));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!strcmp(asset_code_text(code), probe));
    s->members.kind = STUDIO_MEMBER_NONE;
    snprintf(s->members.filter, sizeof s->members.filter, "count");
    REQUIRE(source_member_add_button(s));
    REQUIRE(strstr(asset_code_text(code), "NewVariable: bool = false;"));
    REQUIRE(asset_code_sync_graph(code, e, false));
    id = 0;
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation);
         i++) {
        const source_presentation_origin
            *added = authoring_source_presentation_origin_at(e->presentation, i);
        if (added->kind == SOURCE_PRESENTATION_SYMBOL && !added->pin &&
            added->function < 0 && added->operation == SCRIPT_STATE) {
            node *n = sm_source_variable_node(s, added);
            if (!strcmp(n->desc->name, "NewVariable")) {
                id = n->reference_id;
                break;
            }
        }
    }
    REQUIRE(id);
    REQUIRE(studio_member_shown(s));
    REQUIRE(s->members.kind == STUDIO_MEMBER_VARIABLE && s->members.id == id);
    const source_presentation_origin *new_variable = sm_source_variable(s, id);
    REQUIRE(new_variable && e->active_graph->graph_id == new_variable->graph);
    REQUIRE(acount(e->selected) == 1 && e->selected[0]->id == new_variable->node);
    REQUIRE(s->graph.reveal == new_variable->node && s->graph.reveal_frame);
    REQUIRE(s->members.reveal);
    REQUIRE(!s->members.filter[0]);
    REQUIRE(source_member_row_visible(s));
    member.argc = 2;
    snprintf(member.args[0], sizeof member.args[0], "v%llu.n", (unsigned long long)id);
    snprintf(member.args[1], sizeof member.args[1], "Level");
    studio_member_details_action(s, "change", &member);
    REQUIRE(strstr(asset_code_text(code), "Level: bool = false;"));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(studio_member_shown(s));
    REQUIRE(sm_source_variable(s, s->members.id));
    s->members.pick.id = s->members.id;
    s->members.pick.pin_id = 0;
    studio_palette_entry *types = NULL;
    studio_member_type_entries(s, &types);
    const studio_palette_entry *entity = NULL;
    for (int i = 0; i < acount(types); i++) {
        if (!strcmp(types[i].label, "Entity")) {
            entity = &types[i];
            break;
        }
    }
    REQUIRE(entity);
    studio_member_type_choose(s, entity);
    afree(types);
    REQUIRE(strstr(asset_code_text(code), "Level: entity"));
    REQUIRE(!strstr(asset_code_text(code), "Level: entity = false"));
    REQUIRE(asset_code_sync_graph(code, e, false));
    const source_presentation_origin *level = sm_source_variable(s, s->members.id);
    REQUIRE(level && sm_source_variable_node(s, level)->input_count == 0);
    snprintf(s->members.filter, sizeof s->members.filter, "Level");
    REQUIRE(source_member_add_button(s));
    REQUIRE(strstr(asset_code_text(code), "NewVariable: entity"));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(studio_member_shown(s));
    const source_presentation_origin
        *added_entity = sm_source_variable(s, s->members.id);
    REQUIRE(added_entity);
    node *entity_variable = sm_source_variable_node(s, added_entity);
    REQUIRE(!strcmp(entity_variable->desc->name, "NewVariable"));
    REQUIRE(entity_variable->input_count == 0 && !s->members.filter[0]);
    REQUIRE(e->active_graph->graph_id == added_entity->graph);
    REQUIRE(acount(e->selected) == 1 && e->selected[0]->id == added_entity->node);
    REQUIRE(s->graph.reveal == added_entity->node && s->graph.reveal_frame);
    REQUIRE(s->members.reveal);
    REQUIRE(source_member_row_visible(s));
    s->tab = STUDIO_TAB_MEMBER;
    s->shown_node = s->members.shown_node = 0;
    properties_subject subject;
    properties_subject_of(s, &subject);
    bool available[STUDIO_TAB_COUNT] = { 0 };
    properties_follow_tabs(s, &subject, available);
    REQUIRE(s->tab == STUDIO_TAB_MEMBER && available[STUDIO_TAB_MEMBER]);
    node *other_variable = NULL;
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation);
         i++) {
        const source_presentation_origin
            *other = authoring_source_presentation_origin_at(e->presentation, i);
        if (other->kind == SOURCE_PRESENTATION_SYMBOL && !other->pin &&
            other->function < 0 && other->operation == SCRIPT_STATE) {
            node *n = sm_source_variable_node(s, other);
            if (!strcmp(n->desc->name, "count")) {
                other_variable = n;
                break;
            }
        }
    }
    REQUIRE(other_variable);
    sg_select_only(e, other_variable);
    properties_follow_tabs(s, &subject, available);
    REQUIRE(s->tab == STUDIO_TAB_NODE && available[STUDIO_TAB_NODE]);
    s->tab = tab;
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!strstr(asset_code_text(code), "NewVariable"));
    for (int i = 0; i < 3; i++) {
        REQUIRE(asset_code_replay(code, false, false));
        REQUIRE(asset_code_sync_graph(code, e, false));
    }
    REQUIRE(!strcmp(asset_code_text(code), probe));
    int restored_variables = 0;
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation);
         i++) {
        const source_presentation_origin
            *restored = authoring_source_presentation_origin_at(e->presentation, i);
        if (restored->kind == SOURCE_PRESENTATION_SYMBOL && !restored->pin &&
            restored->function < 0 && restored->operation == SCRIPT_STATE) {
            node *n = sm_source_variable_node(s, restored);
            REQUIRE(!strcmp(n->desc->name, "count"));
            REQUIRE(n->output_count == 1 && n->output_pins[0].type == PIN_I32);
            restored_variables++;
        }
    }
    REQUIRE(restored_variables == 1);
    return true;
}

/* Flow gestures on fused calls use statement ownership, and a branch port
   still names its body. All edits return through the document's undo history. */
static bool fused_call_flow_edits(studio *s)
{
    asset_code *code = counter_source(s);
    editor *e = s->scene->graph_editor;
    REQUIRE(code);
    const char probe[] = "// Authored behavior with a leading comment.\n"
                         "Counter :: behavior {\n"
                         "    count: retain int = 1\n"
                         "    Add :: (amount:int) { count += amount }\n"
                         "    Double :: (amount:int)->(total:int) { return "
                         "amount * 2 }\n"
                         "    on Tick :: () {\n"
                         "        Add(1)\n"
                         "        Add(2)\n"
                         "        if true { count = Double(3) }\n"
                         "    }\n"
                         "}\n";
    bool live = s->prefs.code_apply_live;
    s->prefs.code_apply_live = false;
    REQUIRE(studio_code_replace(s, asset_code_path(code), asset_code_text(code), probe));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!asset_code_error(code)[0]);
    node *first = source_node(code, e, "Add(1)");
    node *second = source_node(code, e, "Add(2)");
    REQUIRE(first && second);
    REQUIRE(sg_layout_of(s, first)->code_lines == 0);
    REQUIRE(sg_layout_of(s, second)->code_lines == 0);
    node *set = source_node(code, e, "count = Double(3)");
    REQUIRE(set);
    int value = -1;
    for (int i = 0; i < set->input_count; i++) {
        REQUIRE(strcmp(set->input_pins[i].name, "Target"));
        if (!strcmp(set->input_pins[i].name, "Value")) {
            pin_parts parts;
            sg_pin_parts_of(s, set, ED_PIN_INPUT, i, &parts);
            REQUIRE(parts.source_select && parts.inline_source);
            REQUIRE(parts.value == VALUE_CHIP && !strcmp(parts.chip, "Double(3)"));
            value = i;
        }
    }
    REQUIRE(value >= 0);
    const source_presentation_origin *called = NULL;
    int call_start = (int)(strstr(asset_code_text(code), "Double(3)") -
                           asset_code_text(code));
    for (int i = 0; i < authoring_source_presentation_origin_count(e->presentation);
         i++) {
        const source_presentation_origin
            *origin = authoring_source_presentation_origin_at(e->presentation, i);
        if (origin->kind == SOURCE_PRESENTATION_EXPRESSION && !origin->pin &&
            origin->range.start == call_start && origin->range.end == call_start + 9) {
            called = origin;
            break;
        }
    }
    REQUIRE(called);
    node *call = graph_node_by_id(e->active_graph, called->node);
    REQUIRE(call && graph_node_is_inline(e->active_graph, call));
    const graph *definition = editor_graph_call_target(e, call);
    REQUIRE(definition && !strcmp(definition->name, "Double"));
    uint64_t caller = e->active_graph->graph_id, call_id = call->id;
    s->graph.menu_open = true;
    s->graph.menu_node = set->id;
    s->graph.menu_pin = set->input_pins[value].id;
    ui_activation menu = { .argc = 1 };
    snprintf(menu.args[0], sizeof menu.args[0], "open-function");
    studio_graph_menu_action(s, "run", &menu);
    REQUIRE(!strcmp(e->active_graph->name, "Double"));
    REQUIRE(editor_graph_back(e) == 0 && e->active_graph->graph_id == caller);
    REQUIRE(acount(e->selected) == 1 && e->selected[0]->id == call_id);
    authoring_edit_request inline_call = {
        .kind = AUTHORING_EDIT_INLINE_CALL,
        .generation = authoring_source_edit_generation(e->presentation),
        .origin = called->identity
    };
    char error[256];
    bool accepted_inline = studio_code_check_edit(s, &inline_call, error, sizeof error);
    if (!accepted_inline) {
        fprintf(stderr, "Studio inline acceptance: %s\n", error);
    }
    REQUIRE(accepted_inline);
    REQUIRE(!strcmp(asset_code_text(code), probe));
    snprintf(menu.args[0], sizeof menu.args[0], "inline-function");
    studio_graph_menu_action(s, "run", &menu);
    REQUIRE(source_catalog_current(s));
    s->graph.menu_open = false;
    REQUIRE(!strstr(asset_code_text(code), "Double(3)"));
    const char *inlined = strstr(asset_code_text(code), "on Tick");
    REQUIRE(inlined && strstr(inlined, "* 2"));
    REQUIRE(!strstr(inlined, "goto "));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!strcmp(asset_code_text(code), probe));
    REQUIRE(acount(e->asset->variables) == 0);
    REQUIRE(source_catalog_current(s));
    const char *accesses[] = { "Get count", "Set count" };
    for (int access = 0; access < 2; access++) {
        ed_search_match *matches = ed_search_matches(e, accesses[access]);
        const ed_search_match *match = NULL;
        for (int i = 0; i < acount(matches); i++) {
            if (matches[i].source_variable &&
                matches[i].desc == (access ? &NODE_SetVariable : &NODE_GetVariable)) {
                match = &matches[i];
                break;
            }
        }
        REQUIRE(match && ed_source_spawn_match(e, match));
        afree(matches);
        const authoring_source *spawned = asset_code_source(code);
        REQUIRE(spawned && spawned->valid);
        const script_analysis *analysis = spawned->analysis;
        int retained = -1;
        for (int i = 0; i < analysis->symbol_count; i++) {
            if (analysis->symbols[i].kind == SCRIPT_STATE &&
                !strcmp(analysis->symbols[i].name, "count")) {
                retained = i;
            }
        }
        REQUIRE(retained >= 0);
        bool resolved_access = false;
        if (!access) {
            for (int i = 0; i < analysis->symbol_count; i++) {
                const script_symbol *local = &analysis->symbols[i];
                if (local->kind != SCRIPT_LOCAL || local->initializer < 0) {
                    continue;
                }
                const script_expression *read = &analysis->expressions[local->initializer];
                resolved_access |= read->kind == SCRIPT_EXPR_SYMBOL &&
                                   read->symbol == retained;
            }
        } else {
            for (int i = 0; i < analysis->statement_count; i++) {
                const script_statement *statement = &analysis->statements[i];
                if (statement->kind != SCRIPT_STMT_ASSIGN ||
                    statement->expression < 0 || statement->second < 0) {
                    continue;
                }
                const script_expression
                    *target = &analysis->expressions[statement->expression];
                const script_expression *value = &analysis
                                                      ->expressions[statement->second];
                resolved_access |= target->kind == SCRIPT_EXPR_SYMBOL &&
                                   target->symbol == retained &&
                                   value->kind == SCRIPT_EXPR_CONSTANT &&
                                   value->constant.type == PIN_I32 &&
                                   value->constant.i == 0;
            }
        }
        if (!resolved_access) {
            fprintf(stderr, "Studio variable %s source:\n%s\n", accesses[access], asset_code_text(code));
        }
        REQUIRE(resolved_access);
        REQUIRE(acount(e->asset->variables) == 0);
        REQUIRE(asset_code_replay(code, false, false));
        REQUIRE(asset_code_sync_graph(code, e, false));
        REQUIRE(!strcmp(asset_code_text(code), probe));
    }
    REQUIRE(source_member_edits(s, probe));
    first = source_node(code, e, "Add(1)");
    second = source_node(code, e, "Add(2)");
    REQUIRE(first && second);
    ed_pin_view out, in;
    REQUIRE(flow_port(second, true, "Then", &out));
    REQUIRE(flow_port(first, false, "Exec", &in));
    REQUIRE(ed_connect(e, &out, &in));
    REQUIRE(asset_code_sync_graph(code, e, false));
    const char *text = asset_code_text(code);
    const char *one = strstr(text, "Add(1)"), *two = strstr(text, "Add(2)");
    REQUIRE(one && two && two < one);
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!strcmp(asset_code_text(code), probe));

    first = source_node(code, e, "Add(1)");
    node *branch = source_node(code, e, "if true");
    REQUIRE(first && branch);
    REQUIRE(flow_port(branch, true, "True", &out));
    REQUIRE(flow_port(first, false, "Exec", &in));
    REQUIRE(ed_connect(e, &out, &in));
    REQUIRE(asset_code_sync_graph(code, e, false));
    text = asset_code_text(code);
    const char *body = strstr(text, "if true {");
    const char *moved = strstr(text, "Add(1)");
    REQUIRE(body && moved && body < moved);
    const char *end = strchr(body, '}');
    REQUIRE(end && moved < end);
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!strcmp(asset_code_text(code), probe));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    s->prefs.code_apply_live = live;
    REQUIRE(show_state_graph(s));
    return check_counter(s, 1, false, NULL);
}

static bool frame_probe_drawn(studio *s, bool expected)
{
    REQUIRE(s->graph.backend);
    rml_context *context = rml_backend_context(s->graph.backend);
    bool found = false;
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *cards[32];
        int count = rml_element_query_selector_all(rml_context_document(context, i), ".gnode", cards, 32);
        REQUIRE(count <= 32);
        for (int j = 0; j < count; j++) {
            char markup[4096];
            rml_element_inner_rml(cards[j], markup, sizeof markup);
            found |= strstr(markup, "FrameProbe") || strstr(markup, "Frame Probe");
        }
    }
    REQUIRE(found == expected);
    return true;
}

/* Two tiny static buffers exercise model history/save in the existing first
 * frame, without loading executable source or adding rendered frames. */
static bool static_relationship_refresh(studio *);
static bool cached_editor_registry(void);
static bool static_prototype_refresh(void);
static bool large_source_read_limits(void);
static bool static_source_dispatch_history(studio *);

/* A generic asset's source-only edits must survive disk refresh even when
   every version evaluates to the same value. Use a fixed disposable root. */
static asset_code *generic_refresh_code;
static char generic_refresh_path[1024];

static void generic_asset_source_cleanup(void)
{
    asset_code_free(generic_refresh_code);
    generic_refresh_code = NULL;
    if (generic_refresh_path[0]) {
        unlink(generic_refresh_path);
    }
    generic_refresh_path[0] = 0;
}

static bool generic_asset_source_refresh(const char *root)
{
    char path[1024], error[256];
    snprintf(path, sizeof path, "%s/AssetMerge.chibi", root);
    snprintf(generic_refresh_path, sizeof generic_refresh_path, "%s", path);
    const char *base = "package asset\nimport data \"game:assets\"\n"
                       "// local baseline\n"
                       "Helper :: () -> int { return 1 }\n"
                       "pub Build :: () -> byte[] { return "
                       "data.Encode(data.Object([])) }\n";
    const char *local = "package asset\nimport data \"game:assets\"\n"
                        "// local draft\n"
                        "Helper :: () -> int { return 1 }\n"
                        "pub Build :: () -> byte[] { return "
                        "data.Encode(data.Object([])) }\n";
    const char *incoming = "package asset\nimport data \"game:assets\"\n"
                           "// local baseline\n"
                           "Helper :: () -> int { return 1 + 0 }\n"
                           "pub Build :: () -> byte[] { return "
                           "data.Encode(data.Object([])) }\n";
    REQUIRE(!write_file_atomic(path, base));
    asset_code *code = asset_code_open(path, error, sizeof error);
    generic_refresh_code = code;
    REQUIRE(code && !asset_code_static(code) && !asset_code_error(code)[0]);
    REQUIRE(asset_code_replace(code, base, local));
    REQUIRE(!write_file_atomic(path, incoming));
    asset_code_refresh(code);
    REQUIRE(!asset_code_error(code)[0]);
    REQUIRE(strstr(asset_code_text(code), "// local draft"));
    REQUIRE(strstr(asset_code_text(code), "return 1 + 0"));
    char *merged = xstrdup(asset_code_text(code));
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(!strcmp(asset_code_text(code), local));
    REQUIRE(asset_code_replay(code, true, false));
    REQUIRE(!strcmp(asset_code_text(code), merged));
    REQUIRE(asset_code_save(code, error, sizeof error));
    char *saved = file_read_text(path);
    REQUIRE(saved && !strcmp(saved, merged));
    free(saved);
    free(merged);
    /* Overlapping source-only changes retain the draft and block Save. */
    const char *conflicting = "package asset\nimport data \"game:assets\"\n"
                              "// local draft\n"
                              "Helper :: () -> int { return 3 }\n"
                              "pub Build :: () -> byte[] { return "
                              "data.Encode(data.Object([])) }\n";
    REQUIRE(asset_code_replace(code, asset_code_text(code), local));
    REQUIRE(!write_file_atomic(path, conflicting));
    asset_code_refresh(code);
    REQUIRE(asset_code_error(code)[0] && !strcmp(asset_code_text(code), local));
    REQUIRE(!asset_code_save(code, error, sizeof error));
    saved = file_read_text(path);
    REQUIRE(saved && !strcmp(saved, conflicting));
    free(saved);
    asset_code_free(code);
    generic_refresh_code = NULL;
    REQUIRE(!unlink(path));
    generic_refresh_path[0] = 0;
    return true;
}

static bool static_source_history(studio *s)
{
    REQUIRE(large_source_read_limits());
    REQUIRE(static_source_dispatch_history(s));
    static const type_field current = { .name = "current",
                                        .type = PIN_I32,
                                        .size = sizeof(int32_t) };
    static const type_desc health = { .name = "StaticHealth",
                                      .size = sizeof(int32_t),
                                      .fields = &current,
                                      .field_count = 1 };
    static const ecs_component_desc component = { .id = 99001,
                                                  .key = "test.static_health",
                                                  .name = "StaticHealth",
                                                  .element_size = sizeof(int32_t),
                                                  .type = &health };
    const ecs_component_desc *components[] = { &component };
    const prototype_registry typed_registry = { .components = components,
                                                .component_count = 1 };
    char root[] = "/tmp/chibi-studio-static-XXXXXX", path[256], directory[256],
         error[256];
    REQUIRE(mkdtemp(root));
    const char *folders[] = { "relationships", "prototypes" };
    const char *names[] = { "Targets", "Enemy" };
    const char *original[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"Targets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.Bool(true))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Enemy\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    const char *changed[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"Targets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.Bool(false))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Enemy\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"StaticHealth\", data.Object([\n"
        "                    data.Field.Named(\"current\", "
        "data.Value.UInt(2))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    const char *invalid[] = {
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", "
        "data.Value.Text(\"Targets\")),\n"
        "            data.Field.Named(\"exclusive\", data.Value.UInt(1))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Enemy\")),\n"
        "            data.Field.Named(\"includes\", data.List([])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"StaticHealth\", data.Object([\n"
        "                    data.Field.Named(\"current\", "
        "data.Value.Text(\"bad\"))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    };
    for (int i = 0; i < 2; i++) {
        snprintf(directory, sizeof directory, "%s/%s", root, folders[i]);
        REQUIRE(!mkdir(directory, 0700));
        snprintf(path, sizeof path, "%s/%s.chibi", directory, names[i]);
        REQUIRE(!write_file_atomic(path, original[i]));
        asset_code *code = asset_code_open(path, error, sizeof error);
        REQUIRE(code && asset_code_static(code));
        asset_code_set_registry(code, i ? &typed_registry : &s->scene->registry);
        REQUIRE(!asset_code_error(code)[0] && !asset_code_pending(code));
        REQUIRE(!asset_code_source(code) && !asset_code_presentation(code));
        int highlight_count = 0;
        authoring_highlight *highlights = authoring_source_syntax_highlights(
            asset_code_syntax(code),
            &highlight_count
        );
        REQUIRE(highlights && highlight_count > 0);
        free(highlights);
        authoring_completions suggestions;
        int offset = i ? (int)(strchr(original[i], '}') - original[i])
                       : (int)(strstr(original[i], "exclusive") - original[i]) + 2;
        authoring_asset_source_complete(asset_code_syntax(code), NULL, offset, &suggestions);
        bool offered = false;
        for (int j = 0; j < suggestions.count; j++) {
            offered |= !strcmp(suggestions.items[j].label, i ? "children" : "exclusive");
        }
        REQUIRE(offered);
        authoring_completions_free(&suggestions);
        REQUIRE(asset_code_replace(code, original[i], changed[i]));
        REQUIRE(asset_code_validate(code, s->scene->graph_editor));
        REQUIRE(asset_code_replay(code, false, true));
        REQUIRE(!strcmp(asset_code_text(code), original[i]));
        REQUIRE(asset_code_replay(code, true, true));
        REQUIRE(!strcmp(text_overlay_text(path), changed[i]));
        REQUIRE(asset_code_replace(code, changed[i], invalid[i]));
        REQUIRE(!asset_code_syntax(code)->diagnostic_count);
        REQUIRE(!asset_code_validate(code, s->scene->graph_editor));
        REQUIRE(asset_code_error(code)[0] && !strcmp(text_overlay_text(path), changed[i]));
        REQUIRE(!asset_code_save(code, error, sizeof error));
        char *disk = file_read_text(path);
        REQUIRE(disk && !strcmp(disk, original[i]));
        free(disk);
        REQUIRE(asset_code_replay(code, false, false));
        REQUIRE(asset_code_save(code, error, sizeof error));
        disk = file_read_text(path);
        REQUIRE(disk && !strcmp(disk, asset_code_text(code)));
        free(disk);
        av_value *checkpoint = asset_code_capture(code);
        asset_code *restored = asset_code_restore(
            checkpoint,
            i ? &typed_registry : &s->scene->registry
        );
        REQUIRE(restored && asset_code_static(restored));
        REQUIRE(!strcmp(asset_code_text(restored), asset_code_text(code)));
        REQUIRE(!asset_code_source(restored) && !asset_code_presentation(restored));
        asset_code_free(restored);
        av_value_free(checkpoint);
        asset_code_free(code);
        REQUIRE(!unlink(path));
        REQUIRE(!rmdir(directory));
    }
    REQUIRE(generic_asset_source_refresh(root));
    REQUIRE(!rmdir(root));
    REQUIRE(static_prototype_refresh());
    return true;
}
#include "source_discard.h"

static bool source_edit_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 4);
    static uint64_t before_undo_texture;
    if (frames > 1) {
        REQUIRE(counter_applied(s));
    }
    if (frames == 1) {
        REQUIRE(static_source_history(s));
        REQUIRE(projects_open(s));
        REQUIRE(reports_warn(s));
        REQUIRE(opens_on_handler(s));
        REQUIRE(fused_call_flow_edits(s));
        REQUIRE(executable_source_discard(s));
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
        asset_code *code = counter_source(s);
        editor *e = sg_graph_editor(s);
        REQUIRE(asset_code_sync_graph(code, e, true));
        authoring_edit_request append = {
            .kind = AUTHORING_EDIT_APPEND_DECLARATIONS,
            .generation = authoring_source_edit_generation(editor_presentation(e)),
            .fragment = "FrameProbe:int=0;"
        };
        REQUIRE(studio_code_edit_source(s, &append, false, error, sizeof error));
        REQUIRE(asset_code_sync_graph(code, e, true));
        REQUIRE(strstr(asset_code_text(code), "FrameProbe:int=0;"));
        bool selected_probe = false;
        for (int i = 0;
             i < authoring_source_presentation_origin_count(e->presentation);
             i++) {
            const source_presentation_origin
                *o = authoring_source_presentation_origin_at(e->presentation, i);
            if (o->kind == SOURCE_PRESENTATION_SYMBOL && !o->pin &&
                o->function < 0 && o->operation == SCRIPT_STATE &&
                !strcmp(sm_source_variable_node(s, o)->desc->name, "FrameProbe")) {
                studio_reveal_node(s, o->graph, o->node);
                selected_probe = true;
                break;
            }
        }
        REQUIRE(selected_probe);
        studio_behavior_show(s, 2);
    } else if (frames == 2) {
        REQUIRE(check_counter(s, 2, true, NULL));
    } else if (frames == 3) {
        REQUIRE(frame_probe_drawn(s, true));
        before_undo_texture = rml_backend_texture_revision(s->graph.backend);
        sg_select_only(sg_graph_editor(s), NULL);
        REQUIRE(studio_code_history(s, false, true));
        REQUIRE(!strstr(asset_code_text(counter_source(s)), "FrameProbe"));
        REQUIRE(check_counter(s, 2, true, NULL));
        REQUIRE(studio_code_history(s, false, true));
        undos++;
        REQUIRE(check_counter(s, 1, false, NULL));
        /* Nothing unsaved: quitting needs no confirmation. */
        REQUIRE(studio_quit(s, true));
        REQUIRE(studio_code_history(s, true, true));
        redos++;
        REQUIRE(check_counter(s, 2, true, NULL));
    } else {
        REQUIRE(frame_probe_drawn(s, false));
        REQUIRE(rml_backend_texture_revision(s->graph.backend) != before_undo_texture);
        REQUIRE(check_counter(s, 2, true, NULL));
        REQUIRE(studio_editor_shown(s, STUDIO_EDITOR_BEHAVIOR));
        REQUIRE(studio_editor_shown(s, STUDIO_EDITOR_CODE));
        REQUIRE(input_map_reloads(s));
        REQUIRE(quit_asks(s));
        REQUIRE(static_relationship_refresh(s));
        REQUIRE(cached_editor_registry());
    }
    return true;
}

/* The Text Editor settles before the keys, which leave frames between them
   for the completion list to open, then comments a line, finds, rests the
   caret in another tab and steps a frame. */
#define KEY_FRAMES 38
#define ASSET_KEY_FRAMES 14
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

/* The modifier Studio's Mod chords use: Command on macOS, Control elsewhere. */
static SDL_Keymod primary_mod(const studio *s)
{
    return s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
}

/* The key that holds that modifier. */
static SDL_Keycode primary_key(const studio *s)
{
    return s->mac ? SDLK_LGUI : SDLK_LCTRL;
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

static const char commented_bytes[] = "Counter :: behavior {\n"
                                      "    // count: retain int = 1\n"
                                      "    on Tick :: () {\n"
                                      "        count = count + 1\n"
                                      "    }\n"
                                      "}\n";

/* The open scene's source editor. */
static asset_code *scene_code(studio *s)
{
    for (int i = 0; i < acount(s->code.assets); i++) {
        if (strstr(asset_code_path(s->code.assets[i]), "Start.chibi")) {
            return s->code.assets[i];
        }
    }
    return NULL;
}

/* The scene source in the Text Editor settles once typed in, so Save writes
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
        const char *count_line = strstr(asset_code_text(code), "count:");
        REQUIRE(count_line);
        kit_code_set_cursor(asset_code_editor(code), (int)(count_line - asset_code_text(code)) + 2);
    } else if (frames == 14 || frames == 18) {
        REQUIRE(press_chord(SDLK_SLASH, primary_mod(s)));
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
        REQUIRE(press_chord(SDLK_F, primary_mod(s)));
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

/* Enter at the end of the open scene source settles without waiting on a
   graph, so Save writes it. */
static bool asset_keys_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    frames++;
    REQUIRE(frames <= ASSET_KEY_FRAMES);
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
    } else if (frames == ASSET_KEY_FRAMES) {
        REQUIRE(undoes_to_save(scene));
    }
    return true;
}

#define UNIT_FRAMES 4
static const char unit_mismatch[] = "Counter :: behavior {\n"
                                    "    count: retain int = 1\n"
                                    "    on Tick :: () {\n"
                                    "        count = count + 1\n"
                                    "        self.Collider.radius = "
                                    "self.Movement.speed\n"
                                    "    }\n"
                                    "}\n";
static bool unit_marked;

/* A proven unit mismatch underlines its statement as a compile error. */
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
        REQUIRE(studio_code_replace(s, path, asset_code_text(code), unit_mismatch));
    } else {
        const char *statement = strstr(unit_mismatch, "self.Collider");
        const kit_code_marker *markers = asset_code_markers(code);
        for (int i = 0; i < acount(markers); i++) {
            const kit_code_marker *m = &markers[i];
            REQUIRE(m->severity == KIT_CODE_ERROR);
            REQUIRE(!strcmp(m->message, "Units differ: u = u/s") && m->line == 5);
            REQUIRE(m->start == statement - unit_mismatch);
            unit_marked = true;
        }
        REQUIRE(!strcmp(asset_code_error(code), unit_marked ? "Units differ: u = u/s" : ""));
    }
    return true;
}

static bool tilemap_viewport(studio *s)
{
    app_scene_authoring *scene = s->scene;
    ecs_world *world = scene->world;
    ecs_entity map = ecs_world_create_entity(world);
    ecs_entity foreground = ecs_world_create_entity(world);
    REQUIRE(Transform_add(world, map, 0) == ECS_OK);
    REQUIRE(Orientation_add(world, map, 0) == ECS_OK);
    REQUIRE(Tilemap_add(world, map, 0) == ECS_OK);
    REQUIRE(Transform_add(world, foreground, 0) == ECS_OK);
    REQUIRE(Appearance_add(world, foreground, 0) == ECS_OK);
    REQUIRE(RenderState_add(world, foreground, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(world) == ECS_OK);
    *Transform_get(world, map) = (Transform){ 10, 20 };
    *Orientation_get(world, map) = (Orientation){ 1.5707963267948966f };
    *Tilemap_get(world, map) = (Tilemap){ .cells = "1 -1",
                                          .columns = 2,
                                          .rows = 1,
                                          .frame_width = 8,
                                          .frame_height = 4,
                                          .layer = -50 };
    *Transform_get(world, foreground) = (Transform){ 8, 24 };
    *Appearance_get(world, foreground) = (Appearance){ .shape = SHAPE_BOX,
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
    RenderState_get(world, foreground)->hidden = true;
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

/* A source graph's menu groups the user's entry points ahead of helpers.
   The labels describe the source even before the scene runs. */
static bool behavior_graph_choices(studio *s)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    studio_graph_menus_add(doc);
    yyjson_mut_val *menus = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "graph_menus");
    studio_panel_sync sync = { .doc = doc, .variable = menus };
    studio_graph_menus_sync(s, &sync);
    yyjson_mut_val *groups = yyjson_mut_obj_get(menus, "graphs");
    REQUIRE(yyjson_mut_arr_size(groups) == 3);
    yyjson_mut_val *events = yyjson_mut_arr_get(groups, 0);
    yyjson_mut_val *variables = yyjson_mut_arr_get(groups, 1);
    yyjson_mut_val *functions = yyjson_mut_arr_get(groups, 2);
    REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(events, "label")), "Events"));
    REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(variables, "label")), "Variables"));
    REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(functions, "label")), "Functions"));
    yyjson_mut_val *options = yyjson_mut_obj_get(events, "options");
    REQUIRE(yyjson_mut_arr_size(options) == 2);
    REQUIRE(
        !strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(options, 0), "label")), "Tick")
    );
    REQUIRE(
        !strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(options, 1), "label")), "Begin")
    );
    options = yyjson_mut_obj_get(variables, "options");
    REQUIRE(yyjson_mut_arr_size(options) == 1);
    REQUIRE(
        !strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(options, 0), "label")), "Variables")
    );
    options = yyjson_mut_obj_get(functions, "options");
    REQUIRE(yyjson_mut_arr_size(options) == 1);
    REQUIRE(
        !strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(options, 0), "label")), "Increment")
    );
    yyjson_mut_doc_free(doc);
    /* A temporary sync document must not become a retained cache key. */
    s->graph.menus_key = 0;
    return true;
}

/* Both follows graph navigation and node selection while graph fields keep
   keyboard focus; following source never turns navigation into a text edit. */
static bool behavior_graph_source(studio *s, rml_context *context, rml_element *input)
{
    asset_code *code = counter_source(s);
    REQUIRE(code);
    kit_code_editor *text = asset_code_editor(code);
    REQUIRE(text);
    editor *e = s->scene->graph_editor;
    const graph *handler = behavior_asset_find_graph_by_name(editor_behavior_asset(e), "Begin");
    REQUIRE(handler);
    kit_code_set_cursor(text, 0);
    ui_activation pick = { .argc = 1 };
    snprintf(
        pick.args[0],
        sizeof pick.args[0],
        "%llu",
        (unsigned long long)handler->graph_id
    );
    studio_graph_header_action(s, "graph", &pick);
    sg_nav_track(s);
    REQUIRE(editor_active_graph(e) == handler);
    const char *source = asset_code_text(code);
    const char *declaration = strstr(source, "on Begin");
    REQUIRE(declaration && kit_code_cursor(text) == declaration - source);
    REQUIRE(rml_context_focus_element(context) == input);
    const source_presentation_origin *statement = NULL;
    const authoring_source_presentation *view = editor_presentation(e);
    for (int i = 0; i < authoring_source_presentation_origin_count(view); i++) {
        const source_presentation_origin *origin = authoring_source_presentation_origin_at(view, i);
        if (origin->graph == handler->graph_id && !origin->pin &&
            origin->kind == SOURCE_PRESENTATION_STATEMENT) {
            statement = origin;
            break;
        }
    }
    REQUIRE(statement);
    node *entry = sg_node_by_id(s, handler->entry_id);
    node *distant = sg_node_by_id(s, statement->node);
    REQUIRE(entry && distant && entry != distant);
    float old_x = distant->x, old_y = distant->y;
    distant->x = entry->x - 10000;
    distant->y = entry->y - 10000;
    sg_frame_open(s);
    float entry_box[4];
    REQUIRE(sg_node_box(s, entry, entry_box));
    float right = s->graph.x + s->graph.surface.rect.w / sg_zoom(s);
    float bottom = s->graph.y + s->graph.surface.rect.h / sg_zoom(s);
    bool entry_visible = entry_box[0] >= s->graph.x && entry_box[1] >= s->graph.y &&
                         entry_box[0] + entry_box[2] <= right &&
                         entry_box[1] + entry_box[3] <= bottom;
    distant->x = old_x;
    distant->y = old_y;
    sg_frame_open(s);
    REQUIRE(entry_visible);
    studio_node_ref selected = { .graph_id = handler->graph_id,
                                 .node_id = statement->node };
    snprintf(selected.asset, sizeof selected.asset, "%s", s->scene->graph_document_id);
    int start, end;
    REQUIRE(asset_code_location(code, selected.graph_id, selected.node_id, &start, &end));
    kit_code_set_cursor(text, 0);
    studio_reveal_node(s, selected.graph_id, selected.node_id);
    REQUIRE(acount(e->selected) == 1 && e->selected[0]->id == selected.node_id);
    REQUIRE(kit_code_cursor(text) == start);
    /* Repeated source/keyboard navigation reveals its target immediately,
       including while a pointer-initiated camera transition is in flight. */
    bool reduced_motion = s->prefs.reduced_motion;
    s->prefs.reduced_motion = false;
    s->graph.x -= 10000;
    s->graph.y -= 10000;
    sg_frame_nodes(s, false, true);
    REQUIRE(s->graph.camera.active);
    sg_show_node(s, e->selected[0], false);
    REQUIRE(!s->graph.camera.active);
    float selected_box[4];
    REQUIRE(sg_node_box(s, e->selected[0], selected_box));
    REQUIRE(selected_box[0] >= s->graph.x && selected_box[1] >= s->graph.y);
    REQUIRE(
        selected_box[0] + selected_box[2] <=
        s->graph.x + s->graph.surface.rect.w / sg_zoom(s)
    );
    REQUIRE(
        selected_box[1] + selected_box[3] <=
        s->graph.y + s->graph.surface.rect.h / sg_zoom(s)
    );
    sg_frame_nodes(s, false, true);
    REQUIRE(s->graph.camera.active);
    sg_show_node(s, e->selected[0], false);
    REQUIRE(!s->graph.camera.active);
    sg_frame_again(s);
    REQUIRE(!s->graph.camera.active);
    s->prefs.reduced_motion = true;
    sg_frame_nodes(s, false, true);
    REQUIRE(!s->graph.camera.active);
    s->prefs.reduced_motion = reduced_motion;
    REQUIRE(rml_context_focus_element(context) == input);
    REQUIRE(!asset_code_dirty(code));
    return show_state_graph(s);
}

/* A behavior without state still has a Variables choice. Selecting that
   empty root must stop displaying the previously selected function. */
static bool behavior_empty_variables(studio *s)
{
    studio_behavior_show(s, 2);
    asset_code *code = counter_source(s);
    REQUIRE(code);
    editor *e = s->scene->graph_editor;
    const char empty[] = "Counter :: behavior {\n"
                         "    Helper :: () -> int { return 1 }\n"
                         "    on Tick :: () {}\n"
                         "}\n";
    REQUIRE(studio_code_replace(s, asset_code_path(code), asset_code_text(code), empty));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(!asset_code_error(code)[0]);
    const graph *function = behavior_asset_find_graph_by_name(editor_behavior_asset(e), "Helper");
    REQUIRE(function && editor_open_graph(e, function->graph_id) == 0);
    studio_code_reveal_graph(s);
    REQUIRE(kit_code_cursor(asset_code_editor(code)) > 0);
    const graph *root = behavior_asset_find_graph(editor_behavior_asset(e), 0);
    REQUIRE(root && !acount(root->nodes));
    ui_activation pick = { .argc = 1 };
    snprintf(pick.args[0], sizeof pick.args[0], "%llu", (unsigned long long)root->graph_id);
    studio_graph_header_action(s, "graph", &pick);
    sg_nav_track(s);
    REQUIRE(editor_active_graph(e) == root);
    REQUIRE(kit_code_cursor(asset_code_editor(code)) == 0);
    REQUIRE(asset_code_replay(code, false, false));
    REQUIRE(asset_code_sync_graph(code, e, false));
    REQUIRE(show_state_graph(s));
    return check_counter(s, 1, false, NULL);
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
    static uint64_t source_graph, source_node;
    static int source_cursor;
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
        sources = preview_sources_working(s->scene->live);
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
        REQUIRE(behavior_graph_choices(s));
        REQUIRE(behavior_graph_source(s, context, input));
        studio_behavior_show(s, 1);
    } else if (frames == 4 || frames == 5 || frames == 10) {
        rml_backend_stats now = rml_backend_get_stats(s->graph.backend);
        REQUIRE(now.context_updates == hidden.context_updates);
        REQUIRE(now.render_captures == hidden.render_captures);
        REQUIRE(s->scene->debug_hidden);
        REQUIRE(!studio_graph_wants_keyboard(s) && !studio_graph_accepts_text(s));
        if (frames < 10) {
            REQUIRE(preview_sources_working(s->scene->live) == sources);
            REQUIRE(preview_sources_applied(s->scene->live) == sources);
            REQUIRE(s->scene->definition->working_presentation == projection);
            REQUIRE(s->scene->asset_index_generation == index_generation);
            REQUIRE(s->scene->asset_index_revision == index_revision);
        }
        if (frames == 5) {
            rml_document_close(focus_document);
            studio_behavior_show(s, 2);
            s->code.find.refocus = true;
        }
    } else if (frames == 6) {
        asset_code *code = counter_source(s);
        REQUIRE(code);
        const char *text = asset_code_text(code);
        const char *statement = strstr(text, "on Begin :: () {\n        count");
        REQUIRE(statement);
        source_cursor = (int)(statement - text) + (int)strlen("on Begin :: () {\n        ");
        REQUIRE(asset_code_node_at(code, source_cursor, &source_graph, &source_node));
        kit_code_set_cursor(asset_code_editor(code), source_cursor);
        kit_code_post(asset_code_editor(code), KIT_CODE_CURSOR);
    } else if (frames == 7) {
        /* A focused source cursor follows its semantic graph/node, without
           graph history moving the caret back to the handler declaration. */
        editor *e = s->scene->graph_editor;
        REQUIRE(editor_active_graph(e)->graph_id == source_graph);
        node *semantic = graph_node_by_id(editor_active_graph(e), source_node);
        REQUIRE(semantic);
        node *shown = graph_inline_owner(editor_active_graph(e), semantic);
        REQUIRE(acount(e->selected) == 1 && e->selected[0] == shown);
        REQUIRE(kit_code_cursor(asset_code_editor(counter_source(s))) == source_cursor);
        rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
        REQUIRE(focus && !strcmp(rml_element_tag_name(focus), "kit-code"));
        REQUIRE(show_state_graph(s));
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
        REQUIRE(studio_code_replace(s, path, before, draft));
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
        REQUIRE(behavior_empty_variables(s));
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
    late_bytes[] = "Late :: behavior {\n"
                   "    macro Advance(elapsed: ref float, dt: double) {\n"
                   "        elapsed = cast<float>(cast<double>(elapsed) + dt)\n"
                   "    }\n"
                   "    elapsed: retain float = 0.125\n"
                   "    action advance = Advance(elapsed, delta)\n"
                   "    after advance {\n"
                   "        elapsed = min(elapsed, 1.0)\n"
                   "    }\n"
                   "    on Tick :: () {\n"
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

#define LABEL_FRAMES 5
/* Wider than the room beside a number field on a node held to NODE_MAX, but
   not than the node's whole interior. */
#define LONG_NAME "distance_falloff_toward_the_nearest_light_source"
/* A number field takes 64 dp beside its label (graph.rcss .grow.out .field). */
static const float NUMBER_FIELD = 64;

/* Opens a declarative graph, as a material's, of two constants whose
   values are named as Name Value does, one shortly and one at length. */
static bool open_named_constants(editor *e, uint64_t ids[2])
{
    const node_desc *constant = registry_node_by_name("Shader Constant");
    REQUIRE(constant);
    behavior_asset *asset = behavior_asset_new("Labels");
    graph *gr = asset->graph;
    node *a = graph_add_node(gr, constant), *b = graph_add_node(gr, constant);
    b->y = a->y + 200;
    REQUIRE(graph_declare_local(gr, a, 0, "n"));
    REQUIRE(graph_declare_local(gr, b, 0, LONG_NAME));
    ids[0] = a->id;
    ids[1] = b->id;
    REQUIRE(!editor_declarative_graph(e, asset));
    behavior_asset_free(asset);
    return true;
}

/* The label the graph drew for node `id`'s first output, or NULL. */
static rml_element *output_label(studio *s, uint64_t id)
{
    char path[96];
    snprintf(path, sizeof path, "world/stage/n%llu/b/o0/l", (unsigned long long)id);
    rml_context *context = rml_backend_context(s->graph.backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *labels[32];
        int count = rml_element_query_selector_all(rml_context_document(context, i), ".gpin-label", labels, 32);
        for (int j = 0; j < count && j < 32; j++) {
            char at[128];
            if (kit_element_path(s->graph.kit, labels[j], at, sizeof at) &&
                !strcmp(at, path)) {
                return labels[j];
            }
        }
    }
    return NULL;
}

/* A label draws past its box only when its text fits beside its row's
   number field. */
static bool label_fits(studio *s, uint64_t id, bool fits)
{
    node *n = sg_node_by_id(s, id);
    REQUIRE(n && sg_value_kind_of(s, n, 1, 0) == VALUE_NUMBER);
    pin_parts parts;
    sg_pin_parts_of(s, n, ED_PIN_OUTPUT, 0, &parts);
    const node_layout *l = sg_layout_of(s, n);
    float text = ceilf(sg_text_width(s, parts.label, 12, 400));
    float room = l->w - 2 * EDGE - 2 * SOCKET_PAD;
    if (!fits && (text <= room - PART_GAP - NUMBER_FIELD || text > room)) {
        fprintf(
            stderr,
            "Studio graph label acceptance: %s is %.0f dp in %.0f dp\n",
            parts.label,
            text,
            room
        );
        return false;
    }
    rml_element *label = output_label(s, id);
    REQUIRE(label && rml_element_has_attribute(label, "data-fits") == fits);
    return true;
}

/* A long output label on a node held to NODE_MAX shares its row with the
   value's field: it clips there rather than painting past the socket. */
static bool graph_labels_frame(void *state)
{
    static uint64_t ids[2];
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= LABEL_FRAMES);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
    } else if (frames == 3) {
        REQUIRE(open_named_constants(sg_graph_editor(s), ids));
    } else if (frames == 5) {
        /* Frame 4 laid the graph out. */
        REQUIRE(label_fits(s, ids[0], true));
        REQUIRE(label_fits(s, ids[1], false));
        keys = frames;
    }
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

/* Queue a complete pointer click in window points. */
static bool click_at(float x, float y)
{
    SDL_WindowID id = window_id();
    REQUIRE(id);
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

/* RmlUi boxes are physical pixels; SDL pointer coordinates are window points. */
static bool click(studio *s, const char *selector)
{
    rml_element *element = NULL;
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context) && !element; i++) {
        rml_element *matches[32];
        int count = rml_element_query_selector_all(rml_context_document(context, i), selector, matches, 32);
        REQUIRE(count <= 32);
        for (int j = 0; j < count; j++) {
            if (rml_element_is_visible(matches[j], true)) {
                element = matches[j];
                break;
            }
        }
    }
    REQUIRE(element);
    rml_vec2 at = rml_element_absolute_offset(element, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(element, RML_BOX_BORDER);
    REQUIRE(size.x > 0 && size.y > 0);
    float to_points = s->prefs.ui_scale / studio_pixel_ratio(s);
    return click_at((at.x + size.x / 2) * to_points, (at.y + size.y / 2) * to_points);
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

#define PROMPT_FRAMES 19
static bool quit_answered;

/* Adds an object to the open scene, leaving it unsaved. Its prototype
   exists only in this run, so the scene's file must never take it. */
static bool add_probe(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Probe\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"Transform\", data.Object([\n"
            "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
            "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
            "                ]))\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
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
   closing Counter with an edit warns only that the edit would be lost, as
   Counter still saves; quitting with the scene also edited says to stop
   the scene to save them. Its Save writes Counter, which applies live, and
   asks again about the scene the running scene keeps from saving, whose
   Discard lets Studio quit without writing it. Both answers are clicks on
   the toast's buttons, which the prompt answers two frames after the click.
   Reduced motion, on from the first frame, settles each toast where layout
   puts it, so the clicks land on its buttons. */
static bool prompts_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= PROMPT_FRAMES);
    if (s->unsaved_prompt.quit) {
        /* Discard went through: Quit goes ahead with the scene unsaved. */
        REQUIRE(frames > 16 && studio_unsaved(s));
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
            REQUIRE(counter_tab(s) >= 0 && counter_says("count: retain int = 5"));
            REQUIRE(add_probe(s));
            studio_command_run(s, STUDIO_COMMAND_PLAY);
            break;
        case 10:
            REQUIRE(studio_session(s) && studio_save_held(s));
            REQUIRE(scene_tabs_activate(s->scene, counter_tab(s)) == 0);
            REQUIRE(edit_counter(s, 5, "4"));
            studio_close_tab(s, counter_tab(s));
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_CLOSE_TAB);
            break;
        case 11:
            /* Counter applies live, so the running scene does not keep it
               from saving. Quitting asks in its place about the scene too,
               which the running scene does keep. */
            REQUIRE(prompt_shows(s, "Save Counter before closing?"));
            REQUIRE(element_says(s, ".toast[data-front] .description", "will be lost"));
            REQUIRE(!studio_quit(s, true));
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_QUIT);
            break;
        case 12:
            REQUIRE(prompt_shows(s, "and Counter before quitting?"));
            REQUIRE(element_says(s, ".toast[data-front] .description", "Stop the scene"));
            REQUIRE(click(s, ".toast[data-front] .toast-action"));
            break;
        case 14:
            /* Counter saved while the scene runs; the scene could not, so
               the prompt asks again about it instead of quitting. */
            REQUIRE(counter_says("count: retain int = 4"));
            REQUIRE(studio_session(s) && studio_unsaved(s));
            REQUIRE(s->unsaved_prompt.toast);
            REQUIRE(s->unsaved_prompt.action == STUDIO_UNSAVED_QUIT);
            break;
        case 15:
            REQUIRE(prompt_shows(s, "before quitting?"));
            REQUIRE(element_says(s, ".toast[data-front] .description", "Stop the scene"));
            REQUIRE(click(s, ".toast[data-front] .toast-cancel"));
            break;
        default: REQUIRE(frames < 18);
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

/* Writes `text` to `file` in the content folder `folder`, which it makes if
   needed. */
static bool write_content(const char *folder, const char *file, const char *text)
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/%s", project, folder);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    REQUIRE(mkdir(path, 0777) == 0 || errno == EEXIST);
    length = snprintf(path, sizeof path, "%s/content/%s/%s", project, folder, file);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    FILE *out = fopen(path, "wb");
    REQUIRE(out);
    bool written = fputs(text, out) >= 0;
    REQUIRE(fclose(out) == 0 && written);
    return true;
}

/* A component naming another object, which the game sets as it runs. */
static const char
    link_component[] = "Link :: component {\n    target: entity\n}\n";
static const char linked_scene[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
    "            data.Field.Named(\"entities\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"A\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Linker\"))\n"
    "                ]),\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"B\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Linker\"))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

/* A names the ship's turret by its path. */
static const char path_scene
    [] = "package asset\n"
         "\n"
         "import data \"game:assets\"\n"
         "\n"
         "pub Build :: () -> byte[] {\n"
         "    return data.Encode(\n"
         "        data.Object([\n"
         "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
         "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
         "            data.Field.Named(\"entities\", data.List([\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Ship\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Hull\")),\n"
         "                    data.Field.Named(\"children\", data.List([\n"
         "                        data.Object([\n"
         "                            data.Field.Named(\"name\", "
         "data.Value.Text(\"Turret\")),\n"
         "                            data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Hull\"))\n"
         "                        ])\n"
         "                    ]))\n"
         "                ]),\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"A\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Linker\")),\n"
         "                    data.Field.Named(\"components\", data.Object([\n"
         "                        data.Field.Named(\"Link\", data.Object([\n"
         "                            data.Field.Named(\"target\", "
         "data.Value.Text(\"Ship.Turret\"))\n"
         "                        ]))\n"
         "                    ]))\n"
         "                ])\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";

/* A names the turret, and B a gun the ship does not have. */
static const char renamed_scene[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
    "            data.Field.Named(\"entities\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", "
    "data.Value.Text(\"Ship\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Hull\")),\n"
    "                    data.Field.Named(\"children\", data.List([\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"name\", "
    "data.Value.Text(\"Turret\")),\n"
    "                            data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Hull\"))\n"
    "                        ])\n"
    "                    ]))\n"
    "                ]),\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"A\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Linker\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"Link\", data.Object([\n"
    "                            data.Field.Named(\"target\", "
    "data.Value.Text(\"Ship.Turret\"))\n"
    "                        ]))\n"
    "                    ]))\n"
    "                ]),\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"B\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Linker\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"Link\", data.Object([\n"
    "                            data.Field.Named(\"target\", "
    "data.Value.Text(\"Ship.Gun\"))\n"
    "                        ]))\n"
    "                    ]))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

/* A and B name paths no object has. */
static const char unresolved_scene[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
    "            data.Field.Named(\"entities\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"A\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Linker\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"Link\", data.Object([\n"
    "                            data.Field.Named(\"target\", "
    "data.Value.Text(\"Nowhere\"))\n"
    "                        ]))\n"
    "                    ]))\n"
    "                ]),\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"B\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Linker\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"Link\", data.Object([\n"
    "                            data.Field.Named(\"target\", "
    "data.Value.Text(\"Elsewhere\"))\n"
    "                        ]))\n"
    "                    ]))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";

/* A ship with a turret, and a root that shares the turret's name. */
static const char nested_scene
    [] = "package asset\n"
         "\n"
         "import data \"game:assets\"\n"
         "\n"
         "pub Build :: () -> byte[] {\n"
         "    return data.Encode(\n"
         "        data.Object([\n"
         "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
         "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
         "            data.Field.Named(\"entities\", data.List([\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Ship\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Hull\")),\n"
         "                    data.Field.Named(\"children\", data.List([\n"
         "                        data.Object([\n"
         "                            data.Field.Named(\"name\", "
         "data.Value.Text(\"Turret\")),\n"
         "                            data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Hull\"))\n"
         "                        ])\n"
         "                    ]))\n"
         "                ]),\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Turret\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Hull\"))\n"
         "                ])\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";

/* A turned ship whose turret sits ten units ahead of it. */
static const char turned_scene[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
    "            data.Field.Named(\"entities\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"name\", "
    "data.Value.Text(\"Ship\")),\n"
    "                    data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Hull\")),\n"
    "                    data.Field.Named(\"components\", data.Object([\n"
    "                        data.Field.Named(\"Transform\", data.Object([\n"
    "                            data.Field.Named(\"x\", "
    "data.Value.Real(100.0)),\n"
    "                            data.Field.Named(\"y\", "
    "data.Value.Real(0.0))\n"
    "                        ])),\n"
    "                        data.Field.Named(\"Orientation\", data.Object([\n"
    "                            data.Field.Named(\"radians\", "
    "data.Value.Real(1.5707964))\n"
    "                        ]))\n"
    "                    ])),\n"
    "                    data.Field.Named(\"children\", data.List([\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"name\", "
    "data.Value.Text(\"Turret\")),\n"
    "                            data.Field.Named(\"prototype\", "
    "data.Value.Text(\"Hull\")),\n"
    "                            data.Field.Named(\"components\", "
    "data.Object([\n"
    "                                data.Field.Named(\"Transform\", "
    "data.Object([\n"
    "                                    data.Field.Named(\"x\", "
    "data.Value.Real(10.0)),\n"
    "                                    data.Field.Named(\"y\", "
    "data.Value.Real(0.0))\n"
    "                                ]))\n"
    "                            ]))\n"
    "                        ])\n"
    "                    ]))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
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
    scene_document *document = scene_document_load(expected, error, sizeof error);
    char *canonical = document
                        ? scene_document_save(document, (ecs_component_catalog){ 0 })
                        : NULL;
    char *text = scene_authoring_scene_text(scene);
    bool same = canonical && text && !strcmp(canonical, text);
    if (!same) {
        fprintf(stderr, "Studio hierarchy acceptance saved:\n%s\n", text ? text : "(nothing)");
    }
    free(canonical);
    free(text);
    scene_document_free(document);
    return same;
}

/* The Link component the project declares, or 0. */
static ecs_component_id link_id(app_scene_authoring *scene)
{
    for (int i = 0; i < ecs_world_component_count(scene->world); i++) {
        const ecs_component_desc *desc = ecs_world_component_at(scene->world, i);
        if (desc && !strcmp(desc->name, "Link")) {
            return desc->id;
        }
    }
    return 0;
}

/* Sets `actor`'s Link to name `target`, as the game would. */
static bool link_to(app_scene_authoring *scene, int actor, int target)
{
    pin_value value = { .type = PIN_HANDLE,
                        .type_name = "ecs.entity",
                        .u64 = ecs_entity_to_u64(scene->actors[target].entity) };
    REQUIRE(
        ecs_component_write_field(scene->world, scene->actors[actor].entity, link_id(scene), 0, value) == ECS_OK
    );
    return true;
}

/* Whether `actor`'s Link names `target`, or nothing when it is negative. */
static bool links(app_scene_authoring *scene, int actor, int target)
{
    pin_value value = { 0 };
    REQUIRE(
        ecs_component_read_field(
            &acceptance_field_reads,
            scene->world,
            scene->actors[actor].entity,
            link_id(scene),
            0,
            &value
        ) == ECS_OK
    );
    return value.u64 ==
           (target < 0 ? 0 : ecs_entity_to_u64(scene->actors[target].entity));
}

/* Whether `actor`'s Link names `path`. */
static bool targets(app_scene_authoring *scene, int actor, const char *path)
{
    const char *named = scene_link_path(scene->actors[actor].paths, link_id(scene), 0);
    return named && !strcmp(named, path);
}

/* Renaming an object moves the paths that name it or an object below it,
   and Undo and Redo, also after a checkpoint, move back only those. */
static bool renames_paths(app_scene_authoring *scene)
{
    REQUIRE(scene_authoring_load_text(scene, renamed_scene, NULL) == 0);
    REQUIRE(links(scene, 2, 1) && links(scene, 3, -1));
    REQUIRE(app_scene_authoring_rename_instance(scene, scene->actors[1].entity, "Gun") == 0);
    REQUIRE(targets(scene, 2, "Ship.Gun") && targets(scene, 3, "Ship.Gun"));
    REQUIRE(links(scene, 2, 1) && links(scene, 3, 1));
    REQUIRE(app_scene_authoring_rename_instance(scene, scene->actors[0].entity, "Hull") == 0);
    REQUIRE(
        targets(scene, 2, "Hull.Gun") && targets(scene, 3, "Hull.Gun") &&
        links(scene, 2, 1)
    );
    REQUIRE(replay_scene_edit(scene, false) == 0);
    REQUIRE(targets(scene, 2, "Ship.Gun") && targets(scene, 3, "Ship.Gun"));
    char *checkpoint = app_scene_authoring_checkpoint(scene);
    REQUIRE(checkpoint);
    bool restored = app_scene_authoring_restore_checkpoint(scene, checkpoint) == 0;
    free(checkpoint);
    REQUIRE(restored);
    /* B named the gun before the turret took its name, so it keeps it. */
    REQUIRE(replay_scene_edit(scene, false) == 0);
    REQUIRE(targets(scene, 2, "Ship.Turret") && targets(scene, 3, "Ship.Gun"));
    REQUIRE(links(scene, 2, 1) && links(scene, 3, -1));
    REQUIRE(replay_scene_edit(scene, true) == 0);
    REQUIRE(
        targets(scene, 2, "Ship.Gun") && targets(scene, 3, "Ship.Gun") &&
        links(scene, 2, 1)
    );
    char *text = scene_authoring_scene_text(scene);
    bool saved = text && !strstr(text, "Ship.Turret");
    free(text);
    REQUIRE(saved);
    return true;
}

/* A Link the scene names by path holds the turret, and the path saves
   through undo, a duplicate, a rename, a checkpoint and a run of the scene. */
static bool names_by_path(app_scene_authoring *scene)
{
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Linker\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"Link\", data.Object([])),\n"
            "                data.Field.Named(\"Transform\", data.Object([\n"
            "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
            "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
            "                ]))\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            NULL
        ) == 0
    );
    REQUIRE(scene_authoring_load_text(scene, path_scene, NULL) == 0);
    REQUIRE(acount(scene->actors) == 3 && links(scene, 2, 1) && saves_as(scene, path_scene));
    scene->selected = 2;
    REQUIRE(app_scene_authoring_delete_instance(scene) == 0 && acount(scene->actors) == 2);
    REQUIRE(replay_scene_edit(scene, false) == 0);
    REQUIRE(acount(scene->actors) == 3 && links(scene, 2, 1) && saves_as(scene, path_scene));
    REQUIRE(app_scene_authoring_rename_instance(scene, scene->actors[1].entity, "Gun") == 0);
    REQUIRE(links(scene, 2, 1) && targets(scene, 2, "Ship.Gun"));
    REQUIRE(replay_scene_edit(scene, false) == 0 && links(scene, 2, 1));
    REQUIRE(saves_as(scene, path_scene));
    /* Deleting the turret leaves the path naming nothing, as does redoing it. */
    scene->selected = 1;
    REQUIRE(app_scene_authoring_delete_instance(scene) == 0 && links(scene, 1, -1));
    REQUIRE(replay_scene_edit(scene, false) == 0 && links(scene, 2, 1));
    REQUIRE(replay_scene_edit(scene, true) == 0 && links(scene, 1, -1));
    REQUIRE(replay_scene_edit(scene, false) == 0 && links(scene, 2, 1));
    char *checkpoint = app_scene_authoring_checkpoint(scene);
    REQUIRE(checkpoint);
    bool restored = app_scene_authoring_restore_checkpoint(scene, checkpoint) == 0;
    free(checkpoint);
    REQUIRE(restored && links(scene, 2, 1) && saves_as(scene, path_scene));
    /* Editing the Linker rewrites A's fields from it, and A names the turret again. */
    pin_value moved = { .type = PIN_F32, .f = 5 };
    REQUIRE(
        write_prototype_field(scene, find_prototype(scene, "Linker"), "Transform", "x", moved) == 0
    );
    REQUIRE(links(scene, 2, 1));
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    /* A run that removes A's Link and takes the turret off the ship still
       saves the authored path and hierarchy, and Stop restores them. */
    REQUIRE(
        ecs_world_record_remove(scene->world, scene->actors[2].entity, link_id(scene), 0) == ECS_OK
    );
    REQUIRE(
        ecs_world_record_remove_pair(
            scene->world,
            scene->actors[1].entity,
            ECS_CHILD_OF,
            scene->actors[0].entity,
            0
        ) == ECS_OK
    );
    REQUIRE(ecs_world_apply_commands(scene->world) == ECS_OK && !child_of(scene, 1, 0));
    REQUIRE(saves_as(scene, path_scene));
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    REQUIRE(child_of(scene, 1, 0) && links(scene, 2, 1) && saves_as(scene, path_scene));
    scene->selected = 2;
    REQUIRE(app_scene_authoring_duplicate_instance(scene) == 0 && links(scene, 3, 1));
    char *text = scene_authoring_scene_text(scene);
    const char *first = text ? strstr(text, "target=\"Ship.Turret\"") : NULL;
    bool both = first && strstr(first + 1, "target=\"Ship.Turret\"");
    free(text);
    REQUIRE(both);
    return true;
}

/* A and B, both selected, naming each other, and Properties showing
   their Link's Target. */
static bool select_crossed_links(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(names_by_path(scene) && renames_paths(scene));
    /* Objects naming different paths read as mixed, though neither resolves. */
    REQUIRE(scene_authoring_load_text(scene, unresolved_scene, NULL) == 0);
    REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[0].entity) == 0);
    scene->actors[1].marked = true;
    pin_value none = { .type = PIN_HANDLE, .type_name = "ecs.entity" };
    REQUIRE(properties_mixed(s, &scene->actors[0], link_id(scene), 0, &none));
    REQUIRE(scene_authoring_load_text(scene, linked_scene, NULL) == 0);
    REQUIRE(acount(scene->actors) == 2 && link_id(scene));
    REQUIRE(link_to(scene, 0, 1) && link_to(scene, 1, 0));
    REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[0].entity) == 0);
    scene->actors[1].marked = true;
    REQUIRE(scene_selection_count(scene) == 2);
    /* A search opens the panels holding what it finds. */
    snprintf(s->property_search, sizeof s->property_search, "Target");
    return true;
}

/* A's Link row in Properties reads as the selection's: a dash that selects
   nothing while A and B name different objects; B once both name it. */
static bool mixed_link(studio *s)
{
    app_scene_authoring *scene = s->scene;
    rml_element *field = studio_element(s, ".entity-field");
    REQUIRE(field);
    char text[64] = "";
    rml_element_inner_rml(studio_element(s, ".entity-field .value"), text, sizeof text);
    REQUIRE(!strcmp(text, "\xe2\x80\x94"));
    REQUIRE(rml_element_has_attribute(field, "data-mixed"));
    REQUIRE(rml_element_has_attribute(field, "data-readonly"));
    ui_activation reveal = { .argc = 1 };
    properties_row_id(
        &(properties_row){ .kind = PROPERTIES_OBJECT,
                           .entity = ecs_entity_to_u64(scene->actors[0].entity),
                           .component = link_id(scene),
                           .field = 0 },
        reveal.args[0]
    );
    studio_properties_action(s, "reveal", &reveal);
    REQUIRE(scene->selected == 0 && scene_selection_count(scene) == 2);
    REQUIRE(link_to(scene, 1, 1));
    studio_properties_action(s, "reveal", &reveal);
    REQUIRE(scene->selected == 1 && scene_selection_count(scene) == 1);
    /* A alone, naming by path a turret that was deleted. */
    REQUIRE(scene_authoring_load_text(scene, path_scene, NULL) == 0);
    scene->selected = 1;
    REQUIRE(app_scene_authoring_delete_instance(scene) == 0 && acount(scene->actors) == 2);
    REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[1].entity) == 0);
    return true;
}

/* A's Link row shows the path that names no object, as a problem. */
static bool dangling_path(studio *s)
{
    rml_element *field = studio_element(s, ".entity-field");
    REQUIRE(field && rml_element_has_attribute(field, "data-invalid"));
    char text[64] = "", tip[128] = "";
    rml_element_inner_rml(studio_element(s, ".entity-field .value"), text, sizeof text);
    rml_element_attribute(field, "data-tip", tip, sizeof tip);
    REQUIRE(!strcmp(text, "Ship.Turret") && !strcmp(tip, "No object has the path Ship.Turret"));
    s->property_search[0] = '\0';
    return true;
}

/* Children load under their parent and keep it through save, undo and
   duplication; a parent waits for its children to go first. */
static bool scene_hierarchy_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 4);
    if (frames == 2) {
        return true;
    }
    if (frames == 3) {
        return mixed_link(s);
    }
    if (frames == 4) {
        return dangling_path(s);
    }
    app_scene_authoring *scene = s->scene;
    REQUIRE(
        app_scene_authoring_register_prototype(
            scene,
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"name\", "
            "data.Value.Text(\"Hull\")),\n"
            "            data.Field.Named(\"components\", data.Object([\n"
            "                data.Field.Named(\"Transform\", data.Object([\n"
            "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
            "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
            "                ])),\n"
            "                data.Field.Named(\"Orientation\", data.Object([\n"
            "                    data.Field.Named(\"radians\", "
            "data.Value.UInt(0))\n"
            "                ])),\n"
            "                data.Field.Named(\"Appearance\", data.Object([\n"
            "                    data.Field.Named(\"shape\", "
            "data.Value.Text(\"Box\")),\n"
            "                    data.Field.Named(\"width\", "
            "data.Value.Real(4.0)),\n"
            "                    data.Field.Named(\"height\", "
            "data.Value.Real(4.0))\n"
            "                ]))\n"
            "            ]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
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
    REQUIRE(app_scene_authoring_add_instance_component(scene, "Velocity") != 0);
    REQUIRE(acount(scene->actors) == 3 && child_of(scene, 1, 0));
    scene->selected = 1;
    REQUIRE(app_scene_authoring_add_instance_component(scene, "Velocity") == 0);
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
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
        "            data.Field.Named(\"entities\", data.List([\n"
        "                data.Object([\n"
        "                    data.Field.Named(\"name\", "
        "data.Value.Text(\"Ship\")),\n"
        "                    data.Field.Named(\"prototype\", "
        "data.Value.Text(\"Hull\")),\n"
        "                    data.Field.Named(\"children\", data.List([\n"
        "                        data.Object([\n"
        "                            data.Field.Named(\"name\", "
        "data.Value.Text(\"Turret\")),\n"
        "                            data.Field.Named(\"prototype\", "
        "data.Value.Text(\"Hull\"))\n"
        "                        ]),\n"
        "                        data.Object([\n"
        "                            data.Field.Named(\"name\", "
        "data.Value.Text(\"%s\")),\n"
        "                            data.Field.Named(\"prototype\", "
        "data.Value.Text(\"Hull\"))\n"
        "                        ])\n"
        "                    ]))\n"
        "                ]),\n"
        "                data.Object([\n"
        "                    data.Field.Named(\"name\", "
        "data.Value.Text(\"Turret\")),\n"
        "                    data.Field.Named(\"prototype\", "
        "data.Value.Text(\"Hull\"))\n"
        "                ])\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
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
    REQUIRE(scene_duplicate_selection(scene, (Transform){ 10, 0 }) == 0);
    const Transform
        *moved = Transform_get_const(scene->world, selected_actor(scene)->entity);
    REQUIRE(child_of(scene, scene->selected, 0));
    REQUIRE(fabsf(moved->x - 10) < 1e-3f && fabsf(moved->y + 10) < 1e-3f);
    edits++;
    return select_crossed_links(s);
}

static const char physics_crate[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Crate3D\")),\n"
    "            data.Field.Named(\"components\", data.Object([\n"
    "                data.Field.Named(\"Body3D\", data.Object([\n"
    "                    data.Field.Named(\"type\", "
    "data.Value.Text(\"Dynamic\"))\n"
    "                ])),\n"
    "                data.Field.Named(\"Collider3D\", data.Object([\n"
    "                    data.Field.Named(\"category\", data.Value.UInt(1)),\n"
    "                    data.Field.Named(\"mask\", data.Value.UInt(1)),\n"
    "                    data.Field.Named(\"size\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.Real(32.0)),\n"
    "                        data.Field.Named(\"y\", data.Value.Real(32.0)),\n"
    "                        data.Field.Named(\"z\", data.Value.Real(32.0))\n"
    "                    ]))\n"
    "                ])),\n"
    "                data.Field.Named(\"Transform3D\", data.Object([\n"
    "                    data.Field.Named(\"scale\", data.Object([\n"
    "                        data.Field.Named(\"x\", data.Value.Real(1.0)),\n"
    "                        data.Field.Named(\"y\", data.Value.Real(1.0)),\n"
    "                        data.Field.Named(\"z\", data.Value.Real(1.0))\n"
    "                    ]))\n"
    "                ]))\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
static const char physics_scene
    [] = "package asset\n"
         "\n"
         "import data \"game:assets\"\n"
         "\n"
         "pub Build :: () -> byte[] {\n"
         "    return data.Encode(\n"
         "        data.Object([\n"
         "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
         "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
         "            data.Field.Named(\"entities\", data.List([\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Crate\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Crate3D\"))\n"
         "                ])\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";

/* Whether 3D physics holds a moving body for the scene's first object. */
static bool moving_3d(app_scene_authoring *scene)
{
    CF_Quat rotation;
    CF_V3 translation;
    return physics3d_interpolation(
        scene->live->runtime->physics3d,
        scene->actors[0].entity,
        1,
        &rotation,
        &translation
    );
}

/* Changing the scene's physics settings, and undoing it, rebuild the 3D world
   as well as the 2D one. */
static bool scene_physics_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 2) {
        return true;
    }
    app_scene_authoring *scene = s->scene;
    REQUIRE(app_scene_authoring_register_prototype(scene, physics_crate, NULL) == 0);
    REQUIRE(scene_authoring_load_text(scene, physics_scene, NULL) == 0);
    physics3d *physics = scene->live->runtime->physics3d;
    REQUIRE(physics3d_tick(physics, 1.0f / 60) == 0 && moving_3d(scene));
    physics_settings settings = physics_configuration(scene->live->runtime->physics);
    settings.sleep = !settings.sleep;
    REQUIRE(app_scene_authoring_set_physics(scene, settings) == 0 && !moving_3d(scene));
    REQUIRE(physics3d_tick(physics, 1.0f / 60) == 0 && moving_3d(scene));
    REQUIRE(replay_scene_edit(scene, false) == 0 && !moving_3d(scene));
    edits++;
    return true;
}

/* Three unused prototypes, the only ones; Assets deletes declarative assets
   (executable sources go through their own refactors). */
static const char *const spares[] = { "SpareA", "SpareB", "SpareC" };

static bool spare_path(const char *name, char *path, size_t size)
{
    int length = snprintf(path, size, "%s/content/prototypes/%s.chibi", project, name);
    REQUIRE(length > 0 && (size_t)length < size);
    return true;
}

static bool write_spare(const char *name)
{
    char path[1024];
    REQUIRE(spare_path(name, path, sizeof path));
    av_doc *document = av_doc_new();
    av_value *root = av_obj(document);
    av_doc_set_root(document, root);
    av_obj_add_str(document, root, "name", name);
    av_value *components = av_obj_add_obj(document, root, "components");
    av_obj_add_obj(document, components, "Transform");
    char *text = asset_document_write(root);
    av_doc_free(document);
    REQUIRE(text);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fputs(text, file) >= 0;
    free(text);
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
    [] = "package asset\n"
         "\n"
         "import data \"game:assets\"\n"
         "\n"
         "pub Build :: () -> byte[] {\n"
         "    return data.Encode(\n"
         "        data.Object([\n"
         "            data.Field.Named(\"version\", data.Value.UInt(1)),\n"
         "            data.Field.Named(\"name\", data.Value.Text(\"Scene\")),\n"
         "            data.Field.Named(\"entities\", data.List([\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Box\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Box\"))\n"
         "                ]),\n"
         "                data.Object([\n"
         "                    data.Field.Named(\"name\", "
         "data.Value.Text(\"Spot\")),\n"
         "                    data.Field.Named(\"prototype\", "
         "data.Value.Text(\"Spot\")),\n"
         "                    data.Field.Named(\"components\", data.Object([\n"
         "                        data.Field.Named(\"Transform\", "
         "data.Object([\n"
         "                            data.Field.Named(\"x\", "
         "data.Value.Real(5.0)),\n"
         "                            data.Field.Named(\"y\", "
         "data.Value.Real(5.0))\n"
         "                        ]))\n"
         "                    ]))\n"
         "                ])\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";
/* More undrawn objects than any fixed count of markers the Scene kept, laid
   out inside the box's 40 by 40, so a framed view shows them all. */
#define CROWD_COLUMNS 20
#define CROWD_ROWS 15
#define CROWD (CROWD_COLUMNS * CROWD_ROWS)
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
    engine_viewport_pointer pointer = { .world_x = x,
                                        .world_y = y,
                                        .world_per_pixel = 0.1f,
                                        .grounded = true };
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
                                        .grounded = true,
                                        .pressed = true,
                                        .down = true };
    REQUIRE(scene_gizmo_pointer(scene, scene->render_viewport, &pointer));
    pointer = (engine_viewport_pointer){ .world_x = 30,
                                         .world_per_pixel = 0.1f,
                                         .grounded = true,
                                         .down = true,
                                         .dragging = true };
    REQUIRE(scene_gizmo_pointer(scene, scene->render_viewport, &pointer));
    REQUIRE(scene->gizmo.active && !strcmp(scene->gizmo.delta, "50 \xc3\x97 40"));
    REQUIRE(!strcmp(scene->gizmo.status, "Size 50 \xc3\x97 40"));
    scene->gizmo.in.cancel = true;
    REQUIRE(scene_gizmo_pointer(scene, scene->render_viewport, &pointer));
    REQUIRE(!scene->gizmo.active && Appearance_get_const(scene->world, box)->width == 40);
    return true;
}

/* While a previewed prototype plays, the wheel, a trackpad's fraction
   included, and Space are its game's, as the scene's are while it plays.
   The preview launches beside this project's source behavior, which its
   sources already bring to the preview's world (previewed_source_runs). */
static bool previewed_prototype_plays(studio *s)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(!scene->runtime_playing && scene->active_runtime_target == APP_RUNTIME_SCENE);
    int previewed = app_scene_authoring_preview_prototype(scene, "Box");
    if (previewed != 0) {
        fprintf(stderr, "Studio viewport acceptance: %s\n", scene->prototype_message);
    }
    REQUIRE(previewed == 0 && !strcmp(scene->prototype_message, "Previewing Box."));
    REQUIRE(scene->active_runtime_target == APP_RUNTIME_PREVIEW && scene->preview_playing);
    engine_viewport_view before, after;
    engine_viewport_get_view(s->viewport, &before);
    float scrolled = s->scroll_y;
    s->scroll_y = 0.5f;
    engine_viewport_host_input input = { .wheel = 0.5f, .width = s->surface.rect.w };
    studio_scene_view(s, &input);
    s->scroll_y = scrolled;
    engine_viewport_get_view(s->viewport, &after);
    bool space = studio_scene_space(s, (studio_key){ .key = SDLK_SPACE });
    app_scene_authoring_stop_prototype_preview(scene);
    REQUIRE(scene->active_runtime_target == APP_RUNTIME_SCENE);
    REQUIRE(input.wheel == 0.5f && after.center_y == before.center_y);
    REQUIRE(!space);
    return true;
}

/* A prototype that runs Counter, this project's source behavior. */
static const char ticker_prototype
    [] = "package asset\n"
         "\n"
         "import data \"game:assets\"\n"
         "\n"
         "pub Build :: () -> byte[] {\n"
         "    return data.Encode(\n"
         "        data.Object([\n"
         "            data.Field.Named(\"name\", "
         "data.Value.Text(\"Ticker\")),\n"
         "            data.Field.Named(\"components\", data.Object([\n"
         "                data.Field.Named(\"Transform\", data.Object([\n"
         "                    data.Field.Named(\"x\", data.Value.UInt(0)),\n"
         "                    data.Field.Named(\"y\", data.Value.UInt(0))\n"
         "                ]))\n"
         "            ])),\n"
         "            data.Field.Named(\"behaviors\", data.List([\n"
         "                data.Value.Text(\"Counter\")\n"
         "            ]))\n"
         "        ])\n"
         "    )\n"
         "}\n";

/* Counter's `count` state as the preview's world published it. */
static uint64_t previewed_count_state(app_scene_authoring *scene)
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Counter.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    const authoring_source *source = preview_source_find(
        prototype_preview_live(scene->prototype_preview),
        path,
        true
    );
    REQUIRE(source && source->valid);
    for (int i = 0; i < source->analysis->symbol_count; i++) {
        const script_symbol *symbol = &source->analysis->symbols[i];
        if (symbol->kind == SCRIPT_STATE && !strcmp(symbol->name, "count")) {
            return symbol->id;
        }
    }
    return 0;
}

/* Previewing a prototype attaches its source behavior, which its sources
   published to the preview's world rather than the launch copying it, and
   the behavior runs as the preview ticks. */
static bool previewed_source_runs(studio *s)
{
    app_scene_authoring *scene = s->scene;
    int previewed = app_scene_authoring_preview_prototype(scene, "Ticker");
    if (previewed != 0) {
        fprintf(stderr, "Studio viewport acceptance: %s\n", scene->prototype_message);
    }
    REQUIRE(previewed == 0);
    execution_instance *counter = prototype_preview_instance(scene->prototype_preview, "Counter");
    uint64_t count = previewed_count_state(scene);
    REQUIRE(counter && count);
    REQUIRE(execution_instance_state(counter, count)->i == 1);
    REQUIRE(prototype_preview_tick(scene->prototype_preview, s->time) >= 0);
    REQUIRE(
        prototype_preview_tick(scene->prototype_preview, s->time + 2 * ENGINE_FIXED_SECONDS) >= 0
    );
    REQUIRE(execution_instance_state(counter, count)->i > 1);
    app_scene_authoring_stop_prototype_preview(scene);
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

/* A field of one value everywhere, whose thumbnail is one flat color. */
static const char flat_field[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.UInt(3)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Flat\")),\n"
    "            data.Field.Named(\"domain\", data.Value.Text(\"field\")),\n"
    "            data.Field.Named(\"id_counter\", data.Value.UInt(10)),\n"
    "            data.Field.Named(\"graphs\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"kind\", "
    "data.Value.Text(\"event\")),\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"\")),\n"
    "                    data.Field.Named(\"nodes\", data.List([\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"id\", "
    "data.Value.UInt(1)),\n"
    "                            data.Field.Named(\"type\", "
    "data.Value.Text(\"Field Constant\")),\n"
    "                            data.Field.Named(\"inputs\", data.Object([\n"
    "                                data.Field.Named(\"Value\", "
    "data.Value.Real(-1.0))\n"
    "                            ]))\n"
    "                        ]),\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"id\", "
    "data.Value.UInt(4)),\n"
    "                            data.Field.Named(\"type\", "
    "data.Value.Text(\"Field Output\"))\n"
    "                        ])\n"
    "                    ])),\n"
    "                    data.Field.Named(\"edges\", data.List([\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"from\", "
    "data.Value.Text(\"1.Value\")),\n"
    "                            data.Field.Named(\"to\", "
    "data.Value.Text(\"4.Value\"))\n"
    "                        ])\n"
    "                    ]))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
/* A shape whose outline lies far outside its frame, which covers nothing,
   and a UI document whose one box paints nothing. */
static const char blank_shape[] =
    "package asset\n"
    "\n"
    "import data \"game:assets\"\n"
    "\n"
    "pub Build :: () -> byte[] {\n"
    "    return data.Encode(\n"
    "        data.Object([\n"
    "            data.Field.Named(\"version\", data.Value.UInt(3)),\n"
    "            data.Field.Named(\"name\", data.Value.Text(\"Blank\")),\n"
    "            data.Field.Named(\"domain\", data.Value.Text(\"shape\")),\n"
    "            data.Field.Named(\"bounds_x\", data.Value.Real(16.0)),\n"
    "            data.Field.Named(\"bounds_y\", data.Value.Real(16.0)),\n"
    "            data.Field.Named(\"id_counter\", data.Value.UInt(11)),\n"
    "            data.Field.Named(\"graphs\", data.List([\n"
    "                data.Object([\n"
    "                    data.Field.Named(\"kind\", "
    "data.Value.Text(\"event\")),\n"
    "                    data.Field.Named(\"name\", data.Value.Text(\"\")),\n"
    "                    data.Field.Named(\"nodes\", data.List([\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"id\", "
    "data.Value.UInt(1)),\n"
    "                            data.Field.Named(\"type\", "
    "data.Value.Text(\"Shader Shape Output\"))\n"
    "                        ]),\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"id\", "
    "data.Value.UInt(3)),\n"
    "                            data.Field.Named(\"type\", "
    "data.Value.Text(\"Shader Vector2\")),\n"
    "                            data.Field.Named(\"inputs\", data.Object([\n"
    "                                data.Field.Named(\"X\", "
    "data.Value.Real(1000.0))\n"
    "                            ]))\n"
    "                        ]),\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"id\", "
    "data.Value.UInt(7)),\n"
    "                            data.Field.Named(\"type\", "
    "data.Value.Text(\"Shader Circle\")),\n"
    "                            data.Field.Named(\"inputs\", data.Object([\n"
    "                                data.Field.Named(\"Radius\", "
    "data.Value.Real(1.0))\n"
    "                            ]))\n"
    "                        ])\n"
    "                    ])),\n"
    "                    data.Field.Named(\"edges\", data.List([\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"from\", "
    "data.Value.Text(\"3.Value\")),\n"
    "                            data.Field.Named(\"to\", "
    "data.Value.Text(\"7.Position\"))\n"
    "                        ]),\n"
    "                        data.Object([\n"
    "                            data.Field.Named(\"from\", "
    "data.Value.Text(\"7.Value\")),\n"
    "                            data.Field.Named(\"to\", "
    "data.Value.Text(\"1.Distance\"))\n"
    "                        ])\n"
    "                    ]))\n"
    "                ])\n"
    "            ]))\n"
    "        ])\n"
    "    )\n"
    "}\n";
static const char blank_rml[] = "<div style=\"display: block; width: 64px; "
                                "height: 64px;\"></div>\n";
#define ASSET_FRAMES 20
#define BLANK_FRAME 13
static bool flat_shown, blank_shown;
static int blank_before;
static studio_thumb_stats last_thumb_stats;

/* The grid shows fields; the flat field's thumbnail shows its picture
   rather than the kind's icon. */
static bool flat_field_shows(studio *s)
{
    s->prefs.assets_list = false;
    s->assets_kind = REFERENCE_ASSET_FIELD;
    int flat = reference_index_find_name(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_FIELD, "Flat");
    REQUIRE(flat >= 0);
    flat_shown |= studio_thumb_cached(s, flat) != NULL;
    return true;
}

/* The tile of asset `name` of `kind` shows a picture. */
static bool tile_shown(studio *s, reference_asset_kind kind, const char *name)
{
    int asset = reference_index_find_name(app_scene_authoring_asset_index(s->scene), kind, name);
    return asset >= 0 && studio_thumb_cached(s, asset) != NULL;
}

/* The grid shows the blank shape and UI document, whose flat pictures
   show nothing, so their tiles keep their kinds' icons. */
static bool blank_tiles_fail(studio *s)
{
    if (frames == BLANK_FRAME) {
        s->assets_kind = -1;
        snprintf(s->assets_filter, sizeof s->assets_filter, "Blank");
        blank_before = studio_thumbs_stats(s).blank;
    }
    blank_shown |= tile_shown(s, REFERENCE_ASSET_SHAPE, "Blank") ||
                   tile_shown(s, REFERENCE_ASSET_UI, "Blank");
    last_thumb_stats = studio_thumbs_stats(s);
    return true;
}

static bool asset_selection_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= ASSET_FRAMES);
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
        case 6:
            /* The grid lays every tile out again. */
            REQUIRE(studio_assets_key(s, SDLK_LEFT, 0) && selection_is(s, "SpareB", ""));
            return true;
        default:
            return frames < BLANK_FRAME ? flat_field_shows(s) : blank_tiles_fail(s);
    }
}

#define ASSET_POINTER_FRAMES 20
#define GRAPH_POINTER_FRAMES 19

/* Find the drawn row by identity, then send input through SDL and RmlUi. */
static bool point_asset(studio *s, const char *name, SDL_Keymod mod)
{
    int id = reference_index_find_name(app_scene_authoring_asset_index(s->scene), REFERENCE_ASSET_PROTOTYPE, name);
    REQUIRE(id >= 0);
    char selector[96];
    snprintf(
        selector,
        sizeof selector,
        "#assets-list [data-asset='%d'] %s",
        id,
        s->prefs.assets_list ? ".tree-row" : ".tile"
    );
    SDL_Keycode key = mod & SDL_KMOD_SHIFT ? SDLK_LSHIFT
                    : mod & SDL_KMOD_GUI   ? SDLK_LGUI
                                           : SDLK_LCTRL;
    if (mod) {
        REQUIRE(push_key(key, mod, true));
    }
    REQUIRE(click(s, selector));
    return !mod || push_key(key, SDL_KMOD_NONE, false);
}

/* Only these prototype identities have visible rows or tiles after filtering. */
static bool visible_spares(studio *s, unsigned expected)
{
    rml_element *list = studio_element(s, "#assets-list");
    REQUIRE(list);
    rml_element *items[16];
    int count = rml_element_query_selector_all(list, "[data-asset]", items, 16);
    REQUIRE(count <= 16);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    unsigned shown = 0;
    for (int i = 0; i < count; i++) {
        if (!rml_element_is_visible(items[i], true)) {
            continue;
        }
        char value[32];
        REQUIRE(rml_element_attribute(items[i], "data-asset", value, sizeof value) > 0);
        int id = atoi(value);
        if (id < 0) {
            continue; /* kind heading */
        }
        bool found = false;
        for (int j = 0; j < 3; j++) {
            if (id ==
                reference_index_find_name(index, REFERENCE_ASSET_PROTOTYPE, spares[j])) {
                REQUIRE(!(shown & (1u << j)));
                shown |= 1u << j;
                found = true;
            }
        }
        REQUIRE(found);
    }
    REQUIRE(shown == expected);
    return true;
}

static bool asset_pointer_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= ASSET_POINTER_FRAMES);
    SDL_Keymod toggle = primary_mod(s);
    switch (frames) {
        case 1:
            studio_show_editor(s, STUDIO_EDITOR_ASSETS);
            s->prefs.assets_list = true;
            s->assets_kind = REFERENCE_ASSET_PROTOTYPE;
            return true;
        case 3: return click(s, "#assets-filter");
        case 4:
            REQUIRE(
                rml_context_focus_element(rml_backend_context(s->backend)) ==
                studio_element(s, "#assets-filter")
            );
            return type_text("Spare");
        case 6:
            REQUIRE(!strcmp(s->assets_filter, "Spare") && visible_spares(s, 7));
            return point_asset(s, "SpareA", 0);
        case 7:
            REQUIRE(selection_is(s, "SpareA", ""));
            return point_asset(s, "SpareC", SDL_KMOD_SHIFT);
        case 8:
            REQUIRE(selection_is(s, "SpareC", "SpareA SpareB"));
            return point_asset(s, "SpareB", toggle);
        case 9:
            REQUIRE(selection_is(s, "SpareC", "SpareA"));
            return click(s, ".assets-view [data-tip='Grid']");
        case 11:
            REQUIRE(!s->prefs.assets_list && !strcmp(s->assets_filter, "Spare"));
            REQUIRE(visible_spares(s, 7));
            return point_asset(s, "SpareB", 0);
        case 12:
            REQUIRE(selection_is(s, "SpareB", ""));
            return point_asset(s, "SpareC", toggle);
        case 13:
            REQUIRE(selection_is(s, "SpareC", "SpareB"));
            return click(s, "#assets-filter");
        case 14: return type_text("B");
        case 16:
            REQUIRE(!strcmp(s->assets_filter, "SpareB") && visible_spares(s, 2));
            return point_asset(s, "SpareB", 0);
        case 17:
            REQUIRE(selection_is(s, "SpareB", ""));
            return click(s, ".assets-view [data-tip='List']");
        case 19:
            REQUIRE(s->prefs.assets_list && visible_spares(s, 2));
            return point_asset(s, "SpareB", 0);
        case 20:
            REQUIRE(selection_is(s, "SpareB", ""));
            REQUIRE(spare_exists("SpareA") && spare_exists("SpareB") && spare_exists("SpareC"));
            return unchanged_source();
        default: return true;
    }
}

/* The first element `selector` finds in any of Studio's documents. */
static rml_element *find_element(studio *s, const char *selector)
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

/* Click the visible node title, away from sockets and editable fields.
   The surface's element places the canvas as this frame laid it out; the
   graph's own rectangle follows a frame later. */
static bool point_node(studio *s, node *n)
{
    const node_layout *layout = sg_layout_of(s, n);
    REQUIRE(layout && layout->w > 0 && layout->top > 0);
    rml_element *surface = find_element(s, ".graph-surface");
    REQUIRE(surface);
    rml_vec2 at = rml_element_absolute_offset(surface, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(surface, RML_BOX_BORDER);
    float to_points = s->prefs.ui_scale / studio_pixel_ratio(s),
          scale = sg_zoom(s) * s->prefs.ui_scale;
    float dx = (n->x + layout->w / 2 - s->graph.x) * scale,
          dy = (n->y + layout->top / 2 - s->graph.y) * scale;
    REQUIRE(dx > 0 && dx < size.x * to_points && dy > 0 && dy < size.y * to_points);
    return click_at(at.x * to_points + dx, at.y * to_points + dy);
}

static bool graph_pointer_frame(void *state)
{
    static uint64_t from, to;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= GRAPH_POINTER_FRAMES);
    editor *e = s->scene->graph_editor;
    switch (frames) {
        case 1:
            studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
            s->prefs.reduced_motion = true;
            return true;
        case 3: {
            graph *gr = sg_active_graph(s);
            REQUIRE(gr);
            for (int i = 0; i < acount(gr->edges) && !to; i++) {
                node *a = sg_node_by_id(s, gr->edges[i].from_id);
                node *b = sg_node_by_id(s, gr->edges[i].to_id);
                if (sg_node_visible(gr, a) && sg_node_visible(gr, b)) {
                    from = a->id;
                    to = b->id;
                }
            }
            REQUIRE(from && to);
            return point_node(s, sg_node_by_id(s, from));
        }
        case 4:
            REQUIRE(selected_only(e, from) && s->focus_area == s->graph.area);
            return press_key(SDLK_RIGHT);
        case 5:
            REQUIRE(selected_only(e, to));
            return press_chord(SDLK_F, primary_mod(s));
        case 7: REQUIRE(s->graph.find.open); return type_text("count");
        case 9:
            REQUIRE(
                !strcmp(s->graph.find.query, "count") && acount(s->graph.find.hits) > 1
            );
            return press_key(SDLK_DOWN);
        case 10: {
            REQUIRE(s->graph.find.current == 0);
            studio_find_hit hit = s->graph.find.hits[0];
            REQUIRE(sg_active_graph(s)->graph_id == hit.graph_id);
            REQUIRE(selected_only(e, hit.node_id));
            return press_key(SDLK_UP);
        }
        case 11: {
            int last = acount(s->graph.find.hits) - 1;
            REQUIRE(s->graph.find.current == last);
            studio_find_hit hit = s->graph.find.hits[last];
            REQUIRE(sg_active_graph(s)->graph_id == hit.graph_id);
            REQUIRE(selected_only(e, hit.node_id));
            return click(s, "#graph-find-list .tree-row");
        }
        case 13:
            REQUIRE(s->graph.find.current == 0);
            REQUIRE(selected_only(e, s->graph.find.hits[0].node_id));
            REQUIRE(
                rml_context_focus_element(rml_backend_context(s->backend)) ==
                studio_element(s, "#graph-find-query")
            );
            return press_key(SDLK_ESCAPE);
        case 15: REQUIRE(!s->graph.find.open); return press_key(SDLK_ESCAPE);
        case 16:
            REQUIRE(!acount(e->selected));
            s->graph.x = s->graph.y = 10000;
            return press_key(SDLK_HOME);
        case 19: {
            graph *gr = sg_active_graph(s);
            float width = s->graph.surface.rect.w / sg_zoom(s);
            float height = s->graph.surface.rect.h / sg_zoom(s);
            int shown = 0;
            for (int i = 0; i < acount(gr->nodes); i++) {
                node *n = gr->nodes[i];
                if (!sg_node_visible(gr, n)) {
                    continue;
                }
                const node_layout *l = sg_layout_of(s, n);
                REQUIRE(n->x >= s->graph.x && n->y >= s->graph.y);
                REQUIRE(n->x + l->w <= s->graph.x + width);
                REQUIRE(n->y + l->h <= s->graph.y + height);
                shown++;
            }
            REQUIRE(shown > 0 && !asset_code_dirty(counter_source(s)));
            return unchanged_source();
        }
        default: return true;
    }
}

/* The box and CROWD undrawn spots inside it, the last selected. */
static bool load_crowd(studio *s)
{
    av_doc *document = av_doc_new();
    av_value *root = av_obj(document);
    av_doc_set_root(document, root);
    av_obj_add_int(document, root, "version", 1);
    av_obj_add_str(document, root, "name", "Scene");
    av_value *entities = av_obj_add_arr(document, root, "entities");
    av_value *box = av_arr_add_obj(document, entities);
    av_obj_add_str(document, box, "name", "Box");
    av_obj_add_str(document, box, "prototype", "Box");
    for (int i = 0; i < CROWD; i++) {
        char name[32];
        snprintf(name, sizeof name, "Spot%d", i);
        av_value *entity = av_arr_add_obj(document, entities);
        av_obj_add_str(document, entity, "name", name);
        av_obj_add_str(document, entity, "prototype", "Spot");
        av_value *components = av_obj_add_obj(document, entity, "components");
        av_value *transform = av_obj_add_obj(document, components, "Transform");
        av_obj_add_float(document, transform, "x", 2 * (i % CROWD_COLUMNS) - CROWD_COLUMNS + 1);
        av_obj_add_float(document, transform, "y", 2 * (i / CROWD_COLUMNS) - CROWD_ROWS + 1);
    }
    char *text = asset_document_write(root);
    av_doc_free(document);
    REQUIRE(text);
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
                    "package asset\n"
                    "\n"
                    "import data \"game:assets\"\n"
                    "\n"
                    "pub Build :: () -> byte[] {\n"
                    "    return data.Encode(\n"
                    "        data.Object([\n"
                    "            data.Field.Named(\"name\", "
                    "data.Value.Text(\"Box\")),\n"
                    "            data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                    data.Field.Named(\"x\", "
                    "data.Value.UInt(0)),\n"
                    "                    data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                ])),\n"
                    "                data.Field.Named(\"Orientation\", "
                    "data.Object([\n"
                    "                    data.Field.Named(\"radians\", "
                    "data.Value.UInt(0))\n"
                    "                ])),\n"
                    "                data.Field.Named(\"Appearance\", "
                    "data.Object([\n"
                    "                    data.Field.Named(\"shape\", "
                    "data.Value.Text(\"Box\")),\n"
                    "                    data.Field.Named(\"width\", "
                    "data.Value.Real(40.0)),\n"
                    "                    data.Field.Named(\"height\", "
                    "data.Value.Real(40.0))\n"
                    "                ]))\n"
                    "            ]))\n"
                    "        ])\n"
                    "    )\n"
                    "}\n",
                    NULL
                ) == 0
            );
            REQUIRE(
                app_scene_authoring_register_prototype(
                    scene,
                    "package asset\n"
                    "\n"
                    "import data \"game:assets\"\n"
                    "\n"
                    "pub Build :: () -> byte[] {\n"
                    "    return data.Encode(\n"
                    "        data.Object([\n"
                    "            data.Field.Named(\"name\", "
                    "data.Value.Text(\"Spot\")),\n"
                    "            data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                    data.Field.Named(\"x\", "
                    "data.Value.UInt(0)),\n"
                    "                    data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                ]))\n"
                    "            ]))\n"
                    "        ])\n"
                    "    )\n"
                    "}\n",
                    NULL
                ) == 0
            );
            REQUIRE(app_scene_authoring_register_prototype(scene, ticker_prototype, NULL) == 0);
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
            REQUIRE(previewed_prototype_plays(s) && previewed_source_runs(s));
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

/* More of the Text Editor's keys than the code-keys selector, which is
   near the time limit, has room for: a command rebound to a chord the
   editor takes, Mod+/ under Vim, Find Again ending a typing burst, the
   context menu's keys and F8 through problems on one byte. */
#define FOLLOWUP_FRAMES 37
/* Where the text's glyphs are: the completion list under a word that is
   not ASCII, and the Mod+hover link of a UI symbol. */
#define GLYPH_FRAMES 30
/* Two problems on one byte: the name is unknown, and so not writable. */
static const char shared_problem[] = "Counter :: behavior {\n"
                                     "    count: retain int = 1\n"
                                     "    on Tick :: () {\n"
                                     "        missing = 1\n"
                                     "    }\n"
                                     "}\n";
static const char crepe_lines[] = "ProbeA: string : \"Cr\xc3\xaa\";\nProbeB: "
                                  "string : \"Cre\";\n";
/* Class names long enough to fill the text's first row, where the pointer
   rests on them. */
#define LONG_CLASS                                                             \
    "-abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrs" \
    "tuvwxyz"
static const char probe_rml[] = "<div class=\"lonely" LONG_CLASS "\"></div>\n";

/* A prototype whose name is not ASCII, which completes in the scene's
   strings, and a UI document with a class its styles define and one they
   do not. */
static bool write_glyph_content()
{
    REQUIRE(write_content(
        "prototypes",
        "Cr\xc3\xaapes.chibi",
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Crêpes\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"Transform\", data.Object([]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    ));
    REQUIRE(write_content("ui", "Probe.rml", probe_rml));
    REQUIRE(write_content("ui", "Probe.rcss", ".known" LONG_CLASS " { color: red; }\n"));
    return true;
}

/* Moves the pointer into the text, `across` its width along its first
   row; *x and *y are the point, in window points. */
static bool point_into_text(studio *s, float across, float *x, float *y)
{
    rml_element *el = find_element(s, "kit-code");
    REQUIRE(el);
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_CONTENT);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_CONTENT);
    float points = s->prefs.ui_scale / studio_pixel_ratio(s);
    *x = (at.x + size.x * across) * points;
    *y = at.y * points + 6 * s->prefs.ui_scale;
    return push_mouse(SDL_EVENT_MOUSE_MOTION, *x, *y, 0, 0);
}

static bool push_right_click(float x, float y)
{
    SDL_WindowID window = window_id();
    REQUIRE(window);
    for (int down = 1; down >= 0; down--) {
        SDL_Event e = { .button = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                 : SDL_EVENT_MOUSE_BUTTON_UP,
                                    .windowID = window,
                                    .button = SDL_BUTTON_RIGHT,
                                    .down = down,
                                    .clicks = 1,
                                    .x = x,
                                    .y = y } };
        REQUIRE(SDL_PushEvent(&e));
    }
    return true;
}

/* Where the completion list's left edge is, in context pixels. */
static bool completion_left(studio *s, float *left)
{
    rml_element *list = find_element(s, ".code-completions");
    REQUIRE(list);
    *left = rml_element_absolute_offset(list, RML_BOX_BORDER).x;
    return true;
}

/* The caret after `after`'s first occurrence in the editor's text. */
static bool caret_after(kit_code_editor *e, const char *after)
{
    const char *text = kit_code_text(e, NULL);
    const char *at = strstr(text, after);
    REQUIRE(at);
    kit_code_set_cursor(e, (int)(at - text + strlen(after)));
    return true;
}

/* Whether the text underlines a symbol as a link. */
static bool linked(studio *s)
{
    rml_element *el = find_element(s, "kit-code");
    REQUIRE(el);
    return rml_element_has_attribute(el, "data-link");
}

static bool code_followups_frame(void *state)
{
    static int caret;
    float x, y;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= FOLLOWUP_FRAMES);
    asset_code *code = counter_code(s);
    REQUIRE(code);
    kit_code_editor *e = asset_code_editor(code);
    switch (frames) {
        case 1: studio_show_editor(s, STUDIO_EDITOR_CODE); return true;
        case 3:
            /* Toggle Line Comment rebound to the chord that copies. */
            snprintf(s->prefs.shortcuts[STUDIO_COMMAND_TOGGLE_LINE_COMMENT], STUDIO_CHORD_SIZE, "Mod+C");
            s->code.find.refocus = true;
            return caret_after(e, "cou");
        case 5: keys++; return press_chord(SDLK_C, primary_mod(s));
        case 6:
            /* The editor takes preferences as it draws, after the frame's
               keys. */
            s->prefs.shortcuts[STUDIO_COMMAND_TOGGLE_LINE_COMMENT][0] = '\0';
            s->prefs.code_vim = true;
            return true;
        case 8:
            REQUIRE(!strcmp(asset_code_text(code), commented_bytes));
            keys++;
            return press_chord(SDLK_SLASH, primary_mod(s));
        case 11:
            REQUIRE(!strcmp(asset_code_text(code), counter_bytes));
            s->prefs.code_vim = false;
            kit_code_set_cursor(e, 0);
            snprintf(s->code.find.query, sizeof s->code.find.query, "count");
            return true;
        case 12: return type_text("x");
        case 14: keys++; return press_key(SDLK_F3);
        case 15: return type_text("y");
        case 18: {
            /* Find Again ended the burst, though the key came in the frame
               that reports its caret move: one undo takes back only the
               typing at the match. */
            REQUIRE(strstr(asset_code_text(code), "y"));
            REQUIRE(asset_code_replay(code, false, false));
            const char *text = asset_code_text(code);
            REQUIRE(text[0] == 'x' && !strcmp(text + 1, counter_bytes));
            REQUIRE(asset_code_replay(code, false, false));
            REQUIRE(!strcmp(asset_code_text(code), counter_bytes));
            REQUIRE(caret_after(e, "cou"));
            return true;
        }
        case 19:
            REQUIRE(point_into_text(s, 0.5f, &x, &y));
            return push_right_click(x, y);
        case 21:
            REQUIRE(s->code.menu.open);
            caret = kit_code_cursor(e);
            keys++;
            /* A letter the menu's typeahead reads arrives as text too. */
            REQUIRE(push_key(SDLK_G, SDL_KMOD_NONE, true) && type_text("g"));
            return push_key(SDLK_G, SDL_KMOD_NONE, false);
        case 23:
            /* The menu takes the keys: typeahead highlights an item, and
               neither the caret nor the text changes. */
            REQUIRE(find_element(s, "#code-menu [data-highlighted]"));
            REQUIRE(kit_code_cursor(e) == caret);
            REQUIRE(!strcmp(asset_code_text(code), counter_bytes));
            keys++;
            return press_key(SDLK_ESCAPE);
        case 25: {
            /* Escape gave the focus back to the text. */
            REQUIRE(!s->code.menu.open);
            rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
            REQUIRE(focus && !strcmp(rml_element_tag_name(focus), "kit-code"));
            return studio_code_replace(s, asset_code_path(code), asset_code_text(code), shared_problem);
        }
        case 27: {
            const kit_code_marker *m = asset_code_markers(code);
            REQUIRE(acount(m) == 2 && m[0].start == m[1].start);
            caret = m[0].start;
            kit_code_set_cursor(e, 0);
            keys++;
            return press_key(SDLK_F8);
        }
        case 29:
        case 31:
        case 33:
            /* F8 visits both problems on the byte, and Shift+F8 goes back. */
            REQUIRE(kit_code_cursor(e) == caret);
            REQUIRE(s->code.problem.index == (frames == 31));
            if (frames == 33) {
                /* Toggle Line Comment rebound to a chord on the secondary
                   modifier, Meta off a Mac. */
                snprintf(s->prefs.shortcuts[STUDIO_COMMAND_TOGGLE_LINE_COMMENT], STUDIO_CHORD_SIZE, "Meta+K");
                return true;
            }
            keys++;
            return press_chord(SDLK_F8, frames == 31 ? SDL_KMOD_SHIFT : 0);
        case 35: keys++; return press_chord(SDLK_K, SDL_KMOD_LGUI);
        case 37:
            REQUIRE(strstr(asset_code_text(code), "// missing = 1"));
            s->prefs.shortcuts[STUDIO_COMMAND_TOGGLE_LINE_COMMENT][0] = '\0';
            return true;
        default: return true;
    }
}

static bool code_glyphs_frame(void *state)
{
    static float left;
    static double scanned;
    float x, y;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= GLYPH_FRAMES);
    if (frames == 1) {
        studio_show_editor(s, STUDIO_EDITOR_CODE);
        return true;
    }
    asset_code *scene = scene_code(s);
    REQUIRE(scene);
    kit_code_editor *e = asset_code_editor(scene);
    switch (frames) {
        case 3:
            kit_code_set_cursor(e, (int)strlen(kit_code_text(e, NULL)));
            kit_code_paste(e, crepe_lines);
            s->code.find.refocus = true;
            return true;
        case 5:
        case 10:
            REQUIRE(caret_after(e, frames == 5 ? "\"Cr\xc3\xaa" : "\"Cre"));
            keys++;
            return press_chord(SDLK_SPACE, SDL_KMOD_CTRL);
        case 8:
            REQUIRE(completion_left(s, &left));
            return press_key(SDLK_ESCAPE);
        case 13: {
            /* The list starts under the word, a cell a character. */
            float ascii;
            REQUIRE(completion_left(s, &ascii));
            REQUIRE(fabsf(ascii - left) < 0.5f);
            char path[1024];
            int length = snprintf(path, sizeof path, "%s/content/ui/Probe.rml", project);
            REQUIRE(length > 0 && (size_t)length < sizeof path);
            REQUIRE(press_key(SDLK_ESCAPE));
            return studio_open_ui_source(s, path, 1);
        }
        case 18:
            s->code.find.refocus = true;
            return point_into_text(s, 0.3f, &x, &y);
        case 19: return push_key(primary_key(s), primary_mod(s), true);
        case 22: {
            /* The pointer rests on the class, which has no other
               definition, so it is no link. */
            const char *text = kit_code_text(s->code.editor, NULL);
            const char *name = strstr(text, "lonely");
            int under = kit_code_link(s->code.editor);
            REQUIRE(
                name && under > name - text &&
                under < (int)(name - text + strlen("lonely" LONG_CLASS))
            );
            REQUIRE(!linked(s));
            /* Another stylesheet comes to define it, unseen until the
               project's scan goes stale, as two seconds on would make it. */
            REQUIRE(write_content("ui", "Other.rcss", ".lonely" LONG_CLASS " { color: red; }\n"));
            s->code.project_time = s->time - 100;
            return true;
        }
        case 24:
            /* The rescan found the other file's definition. Empty the scan
               while it stays fresh: a lookup now would find nothing. */
            REQUIRE(linked(s));
            REQUIRE(s->code.project_time > s->time - 1);
            scanned = s->code.project_time;
            ui_source_project_free(&s->code.project);
            return true;
        case 26: {
            /* Held still, the class was neither scanned nor looked up
               again. */
            REQUIRE(s->code.project_time == scanned && linked(s));
            /* A class of the same length takes its place: the text's new
               revision looks it up again, against the emptied scan. */
            const char *text = kit_code_text(s->code.editor, NULL);
            const char *name = strstr(text, "lonely");
            REQUIRE(name);
            kit_code_set_selection(
                s->code.editor,
                (int)(name - text),
                (int)(name - text + strlen("lonely"))
            );
            kit_code_paste(s->code.editor, "nolink");
            return true;
        }
        case 28:
            REQUIRE(strstr(kit_code_text(s->code.editor, NULL), "nolink" LONG_CLASS));
            REQUIRE(!linked(s));
            return push_key(primary_key(s), SDL_KMOD_NONE, false);
        default: return true;
    }
}

#include "outliner_move.h"
#include "outliner_row_policies.h"
#include "outliner_rows.h"
#include "panel_idle.h"
#include "project_open.h"
#include "scene_camera.h"
#include "scene_collider3d.h"
#include "scene_gizmo3d.h"
#include "scene_move.h"

/* Uses the fixture defined by scene_move.h. */
#include "compiled_asset_edit.h"
#include "prototype_source.h"
#include "prototype_source_authority.h"
#include "prototype_source_capture.h"
#include "source_action_outcomes.h"
#include "typed_prototype_preview.h"

static bool prototype_source_authority_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s);
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 1) {
        REQUIRE(source_action_outcomes_contract());
        REQUIRE(prototype_source_authority_contract(s));
        if (getenv("CHIBI_STUDIO_CONTROLS_CAPTURE")) {
            REQUIRE(prototype_source_capture_begin(s));
        }
    } else if (getenv("CHIBI_STUDIO_CONTROLS_CAPTURE")) {
        REQUIRE(prototype_source_capture_verify(s));
    }
    return true;
}

TEST_CASE(prototype_source_authority)
{
    const char *capture = getenv("CHIBI_STUDIO_CONTROLS_CAPTURE");
    char *args[] = { (char *)executable,
                     "--headless",
                     "--frames",
                     "2",
                     "--fixed-dt",
                     "0.016666666666666666",
                     "--project-root",
                     (char *)project,
                     capture ? "--screenshot" : "--open",
                     capture ? (char *)capture : "Counter" };
    int status = studio_acceptance_host_main(sizeof args / sizeof *args, args);
    bool clean = prototype_source_capture_cleanup();
    REQUIRE(status == 0 && clean);
    REQUIRE(callback_ok && frames == 2);
    printf(
        "\nStudio prototype source authority accepted: frames=2 prototypes=%d "
        "actors=0 checkpoints=1 incoming=%d outgoing=%d\n",
        capture ? 2 : 1,
        capture ? 70 : 68,
        capture ? 98 : 96
    );
    return true;
}

#include "scene_registry_context.h"

bool studio_acceptance_frame(void *state)
{
    switch (mode) {
        case SOURCE_EDIT: callback_ok = source_edit_frame(state); break;
        case CODE_KEYS: callback_ok = code_keys_frame(state); break;
        case ASSET_KEYS: callback_ok = asset_keys_frame(state); break;
        case EXPORT_GAME: callback_ok = export_frame(state); break;
        case GRAPH_VISIBILITY:
            callback_ok = graph_visibility_frame(state);
            break;
        case GRAPH_KEYS: callback_ok = graph_keys_frame(state); break;
        case GRAPH_POINTER: callback_ok = graph_pointer_frame(state); break;
        case DEBUGGER_SOURCE_HISTORY:
            callback_ok = acceptance_debugger_frame(studio_acceptance_studio(state), ++frames);
            break;
        case ASSET_POINTER: callback_ok = asset_pointer_frame(state); break;
        case GRAPH_LABELS: callback_ok = graph_labels_frame(state); break;
        case UNIT_WARNINGS: callback_ok = unit_warnings_frame(state); break;
        case SCENE_HIERARCHY: callback_ok = scene_hierarchy_frame(state); break;
        case SCENE_MOVE: callback_ok = scene_move_frame(state); break;
        case PROTOTYPE_SOURCE:
            callback_ok = prototype_source_frame(state);
            break;
        case PROTOTYPE_SOURCE_AUTHORITY:
            callback_ok = prototype_source_authority_frame(state);
            break;
        case COMPILED_ASSET_EDIT:
            callback_ok = compiled_asset_edit_frame(state);
            break;
        case TYPED_PROTOTYPE_PREVIEW:
            callback_ok = typed_prototype_preview_frame(state);
            break;
        case OUTLINER_MOVE: callback_ok = outliner_move_frame(state); break;
        case OUTLINER_ROWS: callback_ok = outliner_rows_frame(state); break;
        case SCENE_PHYSICS: callback_ok = scene_physics_frame(state); break;
        case ASSET_SELECTION: callback_ok = asset_selection_frame(state); break;
        case SCENE_VIEWPORT: callback_ok = scene_viewport_frame(state); break;
        case SCENE_CAMERA: callback_ok = scene_camera_frame(state); break;
        case SCENE_GIZMO3D: callback_ok = scene_gizmo3d_frame(state); break;
        case SCENE_COLLIDER3D:
            callback_ok = scene_collider3d_frame(state);
            break;
        case PLAY_KEYS: callback_ok = play_keys_frame(state); break;
        case PROMPTS: callback_ok = prompts_frame(state); break;
        case CODE_FOLLOWUPS: callback_ok = code_followups_frame(state); break;
        case CODE_GLYPHS: callback_ok = code_glyphs_frame(state); break;
        case PANEL_IDLE: callback_ok = panel_idle_frame(state); break;
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

TEST_CASE(asset_enter_saves)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(ASSET_KEY_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok);
    REQUIRE(unchanged_source());
    REQUIRE(frames == ASSET_KEY_FRAMES && keys == 2);
    printf("Studio asset keys accepted: frames=%d keys=%d\n", frames, keys);
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
    REQUIRE(callback_ok && frames == UNIT_FRAMES && unit_marked);
    REQUIRE(unchanged_source());
    printf("Studio unit mismatch accepted: frames=%d\n", frames);
    return true;
}

/* This fixture deliberately puts a helper before the event declarations. */
static bool write_graph_counter(const char *bytes)
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/content/behaviors/Counter.chibi", project);
    REQUIRE(length > 0 && (size_t)length < sizeof path);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fputs(bytes, file) >= 0;
    REQUIRE(fclose(file) == 0 && written);
    return true;
}

TEST_CASE(graph_visibility_and_document_pair)
{
    REQUIRE(unchanged_source());
    REQUIRE(write_graph_counter(
        "Counter :: behavior {\n"
        "    count: retain int = 1\n"
        "    Increment :: () -> int {\n"
        "        return count + 1\n"
        "    }\n"
        "    on Tick :: () {\n"
        "        count = count + 1\n"
        "    }\n"
        "    on Begin :: () {\n"
        "        count = 1\n"
        "    }\n"
        "}\n"
    ));
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "10",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 10);
    REQUIRE(write_graph_counter(counter_bytes));
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

TEST_CASE(graph_labels)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(LABEL_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == LABEL_FRAMES && keys == LABEL_FRAMES);
    printf("Studio graph labels accepted: frames=%d\n", frames);
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
    REQUIRE(write_content("components", "Link.chibi", link_component));
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "4",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 4 && edits == 1);
    printf("Studio scene hierarchy accepted\n");
    return true;
}

TEST_CASE(scene_physics_settings)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 2 && edits == 1);
    printf("Studio scene physics accepted\n");
    return true;
}

TEST_CASE(asset_selection)
{
    REQUIRE(write_spares());
    REQUIRE(write_content("fields", "Flat.chibi", flat_field));
    REQUIRE(write_content("shapes", "Blank.chibi", blank_shape));
    REQUIRE(write_content("ui", "Blank.rml", blank_rml));
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(ASSET_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == ASSET_FRAMES && edits == 1 && flat_shown);
    /* Both rendered, and failed as their pictures were flat. */
    const studio_thumb_stats *stats = &last_thumb_stats;
    REQUIRE(stats->count[REFERENCE_ASSET_SHAPE] >= 1 && stats->count[REFERENCE_ASSET_UI] >= 1);
    REQUIRE(stats->blank - blank_before >= 2 && !blank_shown);
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
    int length = snprintf(path, sizeof path, "%s/content/scenes/Start.chibi", project);
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
    bool saved = counter_says("count: retain int = 4");
    bool kept = start_unchanged();
    REQUIRE(restore_counter() && saved && kept);
    printf("\nStudio unsaved prompts accepted: frames=%d\n", frames);
    return true;
}

TEST_CASE(code_followups)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(FOLLOWUP_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == FOLLOWUP_FRAMES && keys == 9);
    REQUIRE(unchanged_source());
    printf("Studio code follow-ups accepted: frames=%d keys=%d\n", frames, keys);
    return true;
}

TEST_CASE(code_glyphs)
{
    REQUIRE(write_glyph_content());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(GLYPH_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == GLYPH_FRAMES && keys == 2);
    printf("Studio code glyphs accepted: frames=%d keys=%d\n", frames, keys);
    return true;
}

TEST_CASE(graph_pointer)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(GRAPH_POINTER_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == GRAPH_POINTER_FRAMES);
    REQUIRE(unchanged_source());
    printf("\nStudio graph pointer accepted: frames=%d\n", frames);
    return true;
}

TEST_CASE(asset_pointer)
{
    REQUIRE(write_spares());
    REQUIRE(write_spare("Other"));
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(ASSET_POINTER_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == ASSET_POINTER_FRAMES);
    printf("\nStudio asset pointer accepted: frames=%d\n", frames);
    return true;
}

TEST_CASE(debugger_source_history)
{
    REQUIRE(unchanged_source());
    char *args[] = {
        (char *)executable, "--headless",
        "--frames",         FRAMES_TEXT(ACCEPTANCE_DEBUGGER_FRAMES),
        "--fixed-dt",       "0.016666666666666666",
        "--project-root",   (char *)project,
        "--open",           "Counter"
    };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == ACCEPTANCE_DEBUGGER_FRAMES);
    REQUIRE(unchanged_source());
    printf("\nStudio debugger source history accepted: frames=%d\n", frames);
    return true;
}

int main(int argc, char **argv)
{
    atexit(acceptance_field_reads_free);
    if (argc == 2 && !strcmp(argv[1], "--compiled-asset-edit-model")) {
        pu_setup(NULL, compiled_model_cleanup);
        RUN_TEST_CASE(compiled_asset_edit_model);
        pu_clear_setup();
        pu_print_stats();
        return pu_test_failed() ? 1 : 0;
    }

    if (argc == 2 && !strcmp(argv[1], "--scene-read")) {
        studio_scene_read_tests();
        pu_print_stats();
        return pu_test_failed() ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--outliner-row-policies") &&
        !strcmp(argv[2], "--source")) {
        return outliner_row_policy_main(argv[3]);
    }
    if (argc == 2 && !strcmp(argv[1], "--project-open")) {
        pu_setup(NULL, project_open_cleanup);
        RUN_TEST_CASE(project_open_errors_stop_before_ui);
        pu_clear_setup();
        pu_print_stats();
        return pu_test_failed() ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--scene-cursors")) {
        RUN_TEST_CASE(scene_gizmo_cursors);
        pu_print_stats();
        return pu_test_failed() ? 1 : 0;
    }
    bool typed_preview = argc == 6 && !strcmp(argv[1], "--typed-prototype-preview") &&
                         !strcmp(argv[4], "--screenshot") && argv[5][0];
    bool compiled_edit_mode = argc == 4 && !strcmp(argv[1], "--compiled-asset-edit");
    bool source_edit = argc == 4 && !strcmp(argv[1], "--source-edit");
    bool code_keys = argc == 4 && !strcmp(argv[1], "--code-keys");
    bool asset_keys = argc == 4 && !strcmp(argv[1], "--asset-keys");
    bool exporting = argc == 4 && !strcmp(argv[1], "--export-game");
    bool visibility = argc == 4 && !strcmp(argv[1], "--graph-visibility");
    bool keys_mode = argc == 4 && !strcmp(argv[1], "--graph-keys");
    bool graph_pointer_mode = argc == 4 && !strcmp(argv[1], "--graph-pointer");
    bool asset_pointer_mode = argc == 4 && !strcmp(argv[1], "--asset-pointer");
    bool debugging = argc == 4 && !strcmp(argv[1], "--debugger-source-history");
    bool labels = argc == 4 && !strcmp(argv[1], "--graph-labels");
    bool units = argc == 4 && !strcmp(argv[1], "--unit-warnings");
    bool hierarchy = argc == 4 && !strcmp(argv[1], "--scene-hierarchy");
    bool prototype_source_mode = argc == 4 && !strcmp(argv[1], "--prototype-source");
    bool prototype_source_authority_mode = argc == 4 &&
                                           !strcmp(argv[1], "--prototype-source-authority");
    bool scene_move_mode = argc == 4 && !strcmp(argv[1], "--scene-move");
    bool outliner_move_mode = argc == 4 && !strcmp(argv[1], "--outliner-move");
    bool outliner_rows_mode = argc == 4 && !strcmp(argv[1], "--outliner-rows");
    bool physics = argc == 4 && !strcmp(argv[1], "--scene-physics");
    bool assets = argc == 4 && !strcmp(argv[1], "--asset-selection");
    bool viewport = argc == 4 && !strcmp(argv[1], "--scene-viewport");
    bool camera = argc == 4 && !strcmp(argv[1], "--scene-camera");
    bool gizmo = argc == 4 && !strcmp(argv[1], "--scene-gizmo3d");
    bool colliders = argc == 4 && !strcmp(argv[1], "--scene-collider3d");
    bool playing = argc == 4 && !strcmp(argv[1], "--play-keys");
    bool prompting = argc == 4 && !strcmp(argv[1], "--unsaved-prompts");
    bool followups = argc == 4 && !strcmp(argv[1], "--code-followups");
    bool idle = argc == 4 && !strcmp(argv[1], "--panel-idle");
    bool glyphs = argc == 4 && !strcmp(argv[1], "--code-glyphs");
    if ((!source_edit && !code_keys && !asset_keys && !exporting && !visibility &&
         !keys_mode && !labels && !units && !hierarchy && !scene_move_mode &&
         !prototype_source_mode && !prototype_source_authority_mode &&
         !compiled_edit_mode && !typed_preview && !outliner_move_mode &&
         !outliner_rows_mode && !physics && !assets && !viewport && !camera && !playing &&
         !prompting && !followups && !glyphs && !idle && !graph_pointer_mode &&
         !asset_pointer_mode && !debugging && !gizmo && !colliders) ||
        strcmp(argv[2], "--project-root")) {
        fprintf(
            stderr,
            "Usage: studio-acceptance "
            "--compiled-asset-edit|--source-edit|--code-keys|--asset-keys|--"
            "export-game|"
            "--graph-visibility|--graph-keys|--graph-pointer|--asset-pointer|--"
            "graph-labels|--unit-warnings|"
            "--scene-hierarchy|--scene-move|--prototype-source|--prototype-"
            "source-authority|--outliner-move|"
            "--outliner-rows|--"
            "scene-physics|"
            "--asset-selection|--scene-viewport|--scene-camera|--scene-gizmo3d|"
            "--scene-collider3d|"
            "--play-keys|--unsaved-prompts|"
            "--code-followups|--code-glyphs|--panel-idle|--debugger-source-"
            "history "
            "--project-root PATH | --scene-cursors | --scene-read | "
            "--project-open | "
            "--outliner-row-policies --source PATH | "
            "--typed-prototype-preview --project-root PATH --screenshot PATH\n"
        );
        return 2;
    }
    executable = argv[0];
    /* Studio keeps real paths, so the project's paths compare equal only
       once a symlinked root (macOS /tmp) is resolved. */
    char *root = realpath(argv[3], NULL);
    if (!root) {
        fprintf(stderr, "Could not resolve the project root %s\n", argv[3]);
        return 2;
    }
    project = root;
    char prefs[1024], cache[1024];
    int p = snprintf(prefs, sizeof prefs, "%s/.acceptance-prefs.json", project);
    int c = snprintf(cache, sizeof cache, "%s/.acceptance-thumbnails", project);
    if (p <= 0 || (size_t)p >= sizeof prefs || c <= 0 || (size_t)c >= sizeof cache ||
        setenv("CHIBI_STUDIO_PREFS_PATH", prefs, 1) ||
        setenv("CHIBI_THUMBNAIL_CACHE", cache, 1)) {
        fprintf(stderr, "Could not isolate Studio acceptance preferences/cache\n");
        free(root);
        return 2;
    }
    pu_display_time(true);
    typed_preview_capture = typed_preview ? argv[5] : NULL;
    mode = compiled_edit_mode              ? COMPILED_ASSET_EDIT
         : typed_preview                   ? TYPED_PROTOTYPE_PREVIEW
         : source_edit                     ? SOURCE_EDIT
         : code_keys                       ? CODE_KEYS
         : asset_keys                      ? ASSET_KEYS
         : exporting                       ? EXPORT_GAME
         : visibility                      ? GRAPH_VISIBILITY
         : keys_mode                       ? GRAPH_KEYS
         : debugging                       ? DEBUGGER_SOURCE_HISTORY
         : graph_pointer_mode              ? GRAPH_POINTER
         : asset_pointer_mode              ? ASSET_POINTER
         : labels                          ? GRAPH_LABELS
         : units                           ? UNIT_WARNINGS
         : prototype_source_authority_mode ? PROTOTYPE_SOURCE_AUTHORITY
         : prototype_source_mode           ? PROTOTYPE_SOURCE
         : scene_move_mode                 ? SCENE_MOVE
         : outliner_move_mode              ? OUTLINER_MOVE
         : outliner_rows_mode              ? OUTLINER_ROWS
         : hierarchy                       ? SCENE_HIERARCHY
         : physics                         ? SCENE_PHYSICS
         : assets                          ? ASSET_SELECTION
         : viewport                        ? SCENE_VIEWPORT
         : camera                          ? SCENE_CAMERA
         : gizmo                           ? SCENE_GIZMO3D
         : colliders                       ? SCENE_COLLIDER3D
         : playing                         ? PLAY_KEYS
         : prompting                       ? PROMPTS
         : followups                       ? CODE_FOLLOWUPS
         : idle                            ? PANEL_IDLE
                                           : CODE_GLYPHS;
    switch (mode) {
        case SOURCE_EDIT: RUN_TEST_CASE(source_graph_edit_undo_redo); break;
        case CODE_KEYS: RUN_TEST_CASE(code_enter_types_newlines); break;
        case ASSET_KEYS: RUN_TEST_CASE(asset_enter_saves); break;
        case EXPORT_GAME: RUN_TEST_CASE(export_game); break;
        case GRAPH_VISIBILITY:
            RUN_TEST_CASE(graph_visibility_and_document_pair);
            break;
        case GRAPH_KEYS: RUN_TEST_CASE(graph_keys); break;
        case GRAPH_POINTER: RUN_TEST_CASE(graph_pointer); break;
        case DEBUGGER_SOURCE_HISTORY:
            RUN_TEST_CASE(debugger_source_history);
            break;
        case ASSET_POINTER: RUN_TEST_CASE(asset_pointer); break;
        case GRAPH_LABELS: RUN_TEST_CASE(graph_labels); break;
        case UNIT_WARNINGS: RUN_TEST_CASE(code_underlines_unit_mismatch); break;
        case SCENE_HIERARCHY: RUN_TEST_CASE(scene_hierarchy); break;
        case SCENE_MOVE: RUN_TEST_CASE(scene_move_contracts); break;
        case PROTOTYPE_SOURCE: RUN_TEST_CASE(prototype_source_ownership); break;
        case COMPILED_ASSET_EDIT: RUN_TEST_CASE(compiled_asset_edit); break;
        case TYPED_PROTOTYPE_PREVIEW:
            RUN_TEST_CASE(typed_prototype_preview);
            break;
        case PROTOTYPE_SOURCE_AUTHORITY:
            RUN_TEST_CASE(prototype_source_authority);
            break;
        case OUTLINER_MOVE: RUN_TEST_CASE(outliner_move); break;
        case OUTLINER_ROWS: RUN_TEST_CASE(outliner_rows); break;
        case SCENE_PHYSICS: RUN_TEST_CASE(scene_physics_settings); break;
        case ASSET_SELECTION: RUN_TEST_CASE(asset_selection); break;
        case SCENE_VIEWPORT: RUN_TEST_CASE(scene_viewport); break;
        case SCENE_CAMERA: RUN_TEST_CASE(camera_navigation); break;
        case SCENE_GIZMO3D: RUN_TEST_CASE(gizmo_3d); break;
        case SCENE_COLLIDER3D: RUN_TEST_CASE(collider_3d); break;
        case PLAY_KEYS: RUN_TEST_CASE(play_keys); break;
        case PROMPTS: RUN_TEST_CASE(unsaved_prompts); break;
        case CODE_FOLLOWUPS: RUN_TEST_CASE(code_followups); break;
        case CODE_GLYPHS: RUN_TEST_CASE(code_glyphs); break;
        case PANEL_IDLE: RUN_TEST_CASE(panel_idle); break;
    }
    pu_print_stats();
    free(root);
    return pu_test_failed() ? 1 : 0;
}
