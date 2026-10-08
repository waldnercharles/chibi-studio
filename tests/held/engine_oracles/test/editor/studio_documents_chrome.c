#include "graph/authoring/asset_edit.h"
/* The editor chrome (workstream C2) in Studio: the Behavior header
 * (BehaviorHeader.rml) with its crumbs, graph picker, rename in place, Back
 * and Forward and debug target picker; the graph's find bar (GraphFind.rml);
 * the Text Editor's header and find bar (CodeHeader.rml, CodeFind.rml); and
 * Performance (Performance.rml) over <line-chart>s, all over C sources that
 * keep the state and history. */

#include "editor/studio/studio_graph_internal.h"
#include "authoring/ui_check.h"
#include "support/array.h"
#include "support/text_file.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/popover.h"

#include <SDL3/SDL_keyboard.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Vault with Teller open in the Behavior area below the Scene, Teller
   holding the function Aim besides its event graph. */
static studio_fixture chrome_open()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    behavior_asset_add_function(f.graph->asset, "Aim", 0);
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_SCENE;
    layout->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_BEHAVIOR;
    frames(&f, 6);
    return f;
}

static rml_element *in_panel(studio *s, const char *panel, const char *selector)
{
    char query[192];
    snprintf(query, sizeof query, "[data-ui=%s] %s", panel, selector);
    return studio_element(s, query);
}

static rml_element *header(studio *s, const char *selector)
{
    return in_panel(s, "BehaviorHeader", selector);
}

static bool shows_text(rml_element *el, const char *expected)
{
    char text[256] = "";
    if (el) {
        rml_element_inner_rml(el, text, sizeof text);
    }
    return el && !strcmp(text, expected);
}

/* The item of `menu` whose label is `label`, or NULL. */
static rml_element *item_labeled(rml_element *menu, const char *label)
{
    rml_element *items[64];
    int count = menu ? rml_element_query_selector_all(menu, ".menu-item", items, 64)
                     : 0;
    for (int i = 0; i < count && i < 64; i++) {
        if (shows_text(rml_element_query_selector(items[i], ".label"), label)) {
            return items[i];
        }
    }
    return NULL;
}

static rml_element *focused(studio *s)
{
    return rml_context_focus_element(rml_backend_context(s->backend));
}

static const char *graph_name(studio *s)
{
    const graph *gr = sg_active_graph(s);
    return gr && gr->name && gr->name[0] ? gr->name : "Event Graph";
}

/* The crumbs name the behavior and the open graph, whose crumb picks
   another graph; Back and Forward step between the graphs visited. */
static void crumbs_pick_and_step()
{
    studio_fixture f = chrome_open();
    studio *s = f.studio;
    bool crumbs = shows_text(header(s, ".crumb .label"), "Teller") &&
                  shows_text(header(s, ".crumb.current .label"), "Event Graph");
    rml_element *crumb = header(s, ".crumb.current");
    if (crumb) {
        press(s, crumb);
    }
    frames(&f, 3);
    rml_element *picker = header(s, "#graph-picker");
    bool open = picker && popover_open(picker) &&
                item_labeled(picker, "Event Graph") &&
                item_labeled(picker, "New Function");
    rml_element *aim = item_labeled(picker, "Aim");
    if (aim) {
        press(s, aim);
    }
    frames(&f, 3);
    bool picked = !strcmp(graph_name(s), "Aim") && !popover_open(picker) &&
                  shows_text(header(s, ".crumb.current .label"), "Aim");
    rml_element *back = header(s, "[data-tip=Back]");
    bool can_back = back && !rml_element_has_attribute(back, "data-disabled");
    if (back) {
        press(s, back);
    }
    frames(&f, 3);
    bool returned = !strcmp(graph_name(s), "Event Graph");
    rml_element *forward = header(s, "[data-tip=Forward]");
    if (forward) {
        press(s, forward);
    }
    frames(&f, 3);
    test_check(
        crumbs && open && picked,
        "the Behavior header's crumbs name the behavior and the open graph, "
        "whose crumb opens the graph picker, and an item opens its graph"
    );
    test_check(
        can_back && returned && !strcmp(graph_name(s), "Aim"),
        "the Behavior header's Back and Forward step between the graphs "
        "visited"
    );
    fixture_free(&f);
}

/* The picker's Rename puts a focused name field in the crumb's place:
   Enter renames as one Undo step and Escape keeps the name. */
static void rename_in_place()
{
    studio_fixture f = chrome_open();
    studio *s = f.studio;
    behavior_asset *asset = f.graph->asset;
    editor_open_graph(f.graph, behavior_asset_find_graph_by_name(asset, "Aim")->graph_id);
    frames(&f, 3);
    int steps = acount(f.graph->undo);
    for (int round = 0; round < 2; round++) {
        rml_element *crumb = header(s, ".crumb.current");
        if (crumb) {
            press(s, crumb);
        }
        frames(&f, 3);
        rml_element *rename = item_labeled(header(s, "#graph-picker"), "Rename");
        if (rename) {
            press(s, rename);
        }
        frames(&f, 3);
        rml_element *field = header(s, ".rename input");
        bool focus = field && focused(s) == field && s->graph.renaming;
        text_event(round ? "Nope" : "Steer");
        tap(round ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_RETURN);
        frames(&f, 3);
        bool done = !header(s, ".rename input") && !s->graph.renaming &&
                    !strcmp(graph_name(s), "Steer") &&
                    acount(f.graph->undo) == steps + 1;
        test_check(
            focus && done,
            round ? "Escape in the graph's name field keeps its name"
                  : "the graph picker's Rename names the graph in place, "
                    "Enter renaming it as one Undo step"
        );
    }
    fixture_free(&f);
}

/* Find shows the open behavior's matches under its field: Down steps to
   one, Enter goes to it and closes, giving the keys back to the graph, and
   Escape closes. */
static void graph_find_bar()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 4);
    rml_element *button = header(s, "[data-tip=Find]");
    if (button) {
        press(s, button);
    }
    frames(&f, 4);
    rml_element *field = studio_element(s, "#graph-find-query");
    bool focus = field && focused(s) == field && s->graph.find.open &&
                 rml_element_has_attribute(button, "data-state");
    text_event("Return");
    frames(&f, 4);
    int hits = acount(s->graph.find.hits);
    char first[256];
    int rows = list_rows(s, "#graph-find-list .list-spacer", first, sizeof first);
    bool listed = hits >= 1 && rows == hits &&
                  in_panel(s, "GraphFind", ".find-widget-count");
    tap(SDL_SCANCODE_DOWN);
    frames(&f, 3);
    bool stepped = s->graph.find.current == 0 &&
                   shows_text(in_panel(s, "GraphFind", ".find-widget-count"), "1/1");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    const editor *e = sg_graph_editor(s);
    bool went = !s->graph.find.open && acount(e->selected) == 1 &&
                e->selected[0]->id == s->graph.find.hits[0].node_id &&
                s->focus_area == s->graph.area &&
                !studio_element(s, "#graph-find-query");
    studio_graph_find(s);
    frames(&f, 3);
    bool reopened = studio_element(s, "#graph-find-query") &&
                    focused(s) == studio_element(s, "#graph-find-query");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    test_check(focus && listed, "Find opens the graph's find bar focused, listing the matches of what is typed");
    test_check(stepped && went, "Down steps to a match and Enter goes to it, closing the bar and giving the keys back to the graph");
    test_check(
        reopened && !s->graph.find.open && !studio_element(s, "#graph-find-query"),
        "Escape closes the graph's find bar"
    );
    fixture_free(&f);
}

/* The picker's option whose value starts with `prefix`, or NULL. */
static rml_element *option_valued(rml_element *picker, const char *prefix)
{
    rml_element *options[32];
    int count = picker
                  ? rml_element_query_selector_all(picker, "[role=option]", options, 32)
                  : 0;
    for (int i = 0; i < count && i < 32; i++) {
        char value[64] = "";
        rml_element_attribute(options[i], "value", value, sizeof value);
        if (!strncmp(value, prefix, strlen(prefix))) {
            return options[i];
        }
    }
    return NULL;
}

/* While the scene runs, the header shows the debug target, whose picker
   lists No target, then Follow newest and Break when spawned for each
   prototype that attaches the behavior, then its running instances; a
   choice follows the newest, toggles the break or untargets. */
static void debug_target_picker()
{
    studio_fixture f = chrome_open();
    studio *s = f.studio;
    bool hidden = !rml_element_is_visible(header(s, ".debug-target"), false);
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 8);
    rml_element *button = header(s, ".debug-target");
    rml_element *picker = header(s, "#debug-targets");
    bool shown = button && rml_element_is_visible(button, false);
    bool listed = true, chose[3] = { false };
    static const char *const CHOICES[] = { "newest:Vault", "break:Vault", "none" };
    for (int i = 0; i < 3; i++) {
        if (button) {
            press(s, button);
        }
        frames(&f, 3);
        rml_element *option = option_valued(picker, CHOICES[i]);
        listed &= picker && popover_open(picker) && option &&
                  option_valued(picker, "none");
        if (option) {
            press(s, option);
        }
        frames(&f, 3);
        const char *newest = s->scene->debug_newest;
        chose[i] = !popover_open(picker) &&
                   (i == 0 ? newest && !strcmp(newest, "Vault")
                    : i == 1 ? app_scene_authoring_spawn_break(s->scene, "Vault")
                             : !newest && s->scene->debug_status != APP_DEBUG_AVAILABLE);
    }
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 3);
    test_check(
        hidden && shown && listed,
        "the Behavior header shows the debug target while the scene runs, "
        "its picker listing No target, Follow newest and Break when spawned"
    );
    test_check(
        chose[0] && chose[1] && chose[2],
        "the debug target picker follows the newest of a prototype, breaks "
        "when one spawns, and untargets with No target"
    );
    fixture_free(&f);
}

/* The Text Editor's header switches the part and the apply mode, and its
   find bar steps through the part's matches with Enter and Shift+Enter,
   Escape returning to the text. */
static void text_editor_chrome()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char path[PATH_MAX];
    bool opened = realpath(BANK, path) && studio_open_ui_source(s, path, 1);
    frames(&f, 4);
    bool named = shows_text(in_panel(s, "CodeHeader", ".code-file .name"), "Bank.rml");
    rml_element *rcss = in_panel(s, "CodeHeader", ".code-parts .seg:last-child");
    if (rcss) {
        press(s, rcss);
    }
    frames(&f, 3);
    bool part = studio_ui_buffer(s)->part == UI_PART_RCSS &&
                shows_text(in_panel(s, "CodeHeader", ".code-file .name"), "Bank.rcss");
    rml_element *rml = in_panel(s, "CodeHeader", ".code-parts .seg");
    if (rml) {
        press(s, rml);
    }
    bool live = s->prefs.code_apply_live;
    rml_element *mode = in_panel(s, "CodeHeader", ".code-apply .seg:last-child");
    if (mode) {
        press(s, mode);
    }
    frames(&f, 3);
    bool applied = s->prefs.code_apply_live == false &&
                   studio_ui_buffer(s)->part == UI_PART_RML;
    s->prefs.code_apply_live = live;
    studio_code_find(s);
    frames(&f, 4);
    rml_element *field = studio_element(s, "#code-find-query");
    bool focus = field && focused(s) == field;
    text_event("extract");
    frames(&f, 4);
    int first = s->code.find.index;
    bool counted = s->code.find.count == 3 && first >= 0;
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    int next = s->code.find.index;
    key_event_mod(SDL_SCANCODE_RETURN, true, SDL_KMOD_SHIFT, false);
    key_event_mod(SDL_SCANCODE_RETURN, false, SDL_KMOD_SHIFT, false);
    frames(&f, 3);
    char count[16];
    snprintf(count, sizeof count, "%d/3", first + 1);
    bool stepped = next == (first + 1) % 3 && s->code.find.index == first &&
                   shows_text(in_panel(s, "CodeFind", ".find-widget-count"), count);
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    rml_element *now = focused(s);
    bool back = !s->code.find.open && !studio_element(s, "#code-find-query") &&
                now && !strcmp(rml_element_tag_name(now), "kit-code");
    test_check(opened && named && part && applied, "the Text Editor's header names the shown part's file and switches the part and the apply mode");
    test_check(focus && counted && stepped, "the Text Editor's find bar counts the part's matches and steps through them with Enter and Shift+Enter");
    test_check(back, "Escape closes the Text Editor's find bar and returns to the text");
    fixture_free(&f);
}

/* An asset's code source in the Text Editor: its header names the asset,
   with Save and the apply mode. */
static void asset_source_header()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    bool opened = app_scene_authoring_open_prototype(f.scene, "Vault") == 0;
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_CODE;
    frames(&f, 4);
    bool named = shows_text(in_panel(s, "CodeHeader", "span.code-file"), "Vault") &&
                 in_panel(s, "CodeHeader", "[data-tip='Save source']");
    bool live = s->prefs.code_apply_live;
    rml_element *mode = in_panel(s, "CodeHeader", "[data-tip='Apply edits'] .seg:last-child");
    if (mode) {
        press(s, mode);
    }
    frames(&f, 3);
    bool applied = !s->prefs.code_apply_live;
    s->prefs.code_apply_live = live;
    test_check(opened && named && applied, "an asset's source in the Text Editor names the asset, with Save and the apply mode");
    fixture_free(&f);
}

/* Performance charts its samples while the scene runs: the frame and the
   simulation's phases with a legend, its readout above. */
static void performance_charts()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_PERFORMANCE;
    frames(&f, 4);
    rml_element *charts[4];
    int idle = panel_elements(s, "Performance", "line-chart", charts);
    bool not_running = rml_element_is_visible(in_panel(s, "Performance", ".perf-idle"), false);
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 30);
    rml_element *simulation = NULL;
    rml_element *charted[4];
    for (int i = 0;
         i < panel_elements(s, "Performance", "line-chart", charted) && i < 4;
         i++) {
        char series[128] = "";
        rml_element_attribute(charted[i], "series", series, sizeof series);
        simulation = strstr(series, "perf:behavior") ? charted[i] : simulation;
    }
    rml_element *badge = in_panel(s, "Performance", "line-chart .chart-badge");
    rml_element *legend = in_panel(s, "Performance", ".perf-legend");
    rml_element *readout = in_panel(s, "PerformanceHeader", ".perf-readout");
    char value[96] = "";
    if (readout) {
        rml_element_attribute(readout, "value", value, sizeof value);
    }
    bool running = simulation && rml_element_is_visible(simulation, false) &&
                   legend && rml_element_is_visible(legend, false) && badge &&
                   rml_element_is_visible(badge, false) && strstr(value, "fps");
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 3);
    test_check(idle >= 2 && not_running, "Performance shows its frame chart, and Not running until the scene runs");
    test_check(running, "while the scene runs, Performance charts the frame and the simulation's phases with a legend, and reads out the frame rate");
    fixture_free(&f);
}

/* The chrome's documents read without a warning from RmlUi. */
static void documents_check_clean()
{
    static const char *const NAMES[] = { "BehaviorHeader", "GraphFind",
                                         "CodeHeader",     "CodeFind",
                                         "Performance",    "PerformanceHeader" };
    int clean = 0, count = (int)(sizeof NAMES / sizeof *NAMES);
    for (int i = 0; i < count; i++) {
        char path[256], error[200] = "";
        snprintf(path, sizeof path, "projects/studio/content/ui/%s.rml", NAMES[i]);
        char *rml = text_file_read(path);
        snprintf(path, sizeof path, "projects/studio/content/ui/%s.rcss", NAMES[i]);
        char *rcss = text_file_read(path);
        ui_check_message *messages = NULL;
        bool ok = rml &&
                  ui_check("projects/studio/content", NAMES[i], rml, rcss, &messages, error, sizeof error);
        for (int m = 0; m < acount(messages); m++) {
            fprintf(
                stderr,
                "  %s:%d %s\n",
                NAMES[i],
                messages[m].line,
                messages[m].text
            );
        }
        clean += ok && !acount(messages);
        ui_check_messages_free(messages);
        free(rml);
        free(rcss);
    }
    test_check(clean == count, "the Behavior header, the find bars, the Text Editor's header and Performance check clean");
}

void studio_documents_chrome_checks()
{
    documents_check_clean();
    crumbs_pick_and_step();
    rename_in_place();
    graph_find_bar();
    debug_target_picker();
    text_editor_chrome();
    asset_source_header();
    performance_charts();
}
