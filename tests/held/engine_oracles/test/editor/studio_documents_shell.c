#include "editor/workspace/viewport.h"
#include "editor/documents/asset_code.h"
/* Studio's shell: the authored top bar, status bar and area headers,
 * layouts, the command palette, keys, the wheel and the Performance
 * editor. */

#include "assets/ui_events.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/input.h"
#include "graph/document/serialize.h"
#include "graph/nodes/component_nodes.h"
#include "platform/cute.h"
#include "render/scene_renderer.h"
#include "render/screenshot.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/text_file.h"
#include "support/text_overlay.h"
#include "test/editor/studio_documents.h"
#include "test/editor/ui_bench.h"
#include "test/suite_internal.h"
#include "ui/input_feel.h"
#include "ui/popover.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void host_ui_refresh_keeps_input_immediate()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 90);
    s->active_until = 0;
    uint64_t before = s->ui_schedule.frames;
    double time = f.time;
    frames(&f, 60);
    uint64_t idle = s->ui_schedule.frames - before;
    test_check(idle >= 3 && idle < 10 && f.time > time + 0.99, "settled idle UI refresh is bounded while simulation frames keep advancing");

    s->active_until = 0;
    s->ui_schedule.next = s->time + 1;
    before = s->ui_schedule.frames;
    int toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){
            .title = "Async host wake",
            .duration = -1,
        }
    );
    frame(&f, false);
    char toast_text[128] = "";
    rml_element *toast_element = studio_element(s, ".toast .title");
    if (toast_element) {
        rml_element_inner_rml(toast_element, toast_text, sizeof toast_text);
    }
    test_check(
        s->ui_schedule.frames == before + 1 && strstr(toast_text, "Async host wake"),
        "a new toast wakes and renders the idle host without SDL input"
    );
    kit_toast_dismiss(s->kit, toast);
    frames(&f, 90);
    s->active_until = 0;
    s->ui_schedule.next = s->time + 1;
    before = s->ui_schedule.frames;
    kit_style_sheet(s->kit, "host-wake-probe", ".statusbar { opacity: 0.999; }");
    frame(&f, false);
    char opacity[32] = "";
    rml_element *bar = studio_element(s, ".statusbar");
    if (bar) {
        rml_element_property(bar, "opacity", opacity, sizeof opacity);
    }
    test_check(s->ui_schedule.frames == before + 1 && fabs(atof(opacity) - 0.999) < 0.0001, "an asynchronous restyle wakes and updates the idle host without SDL input");
    kit_style_sheet(s->kit, "host-wake-probe", "");
    frames(&f, 3);

    before = s->ui_schedule.frames;
    s->prefs.short_bindings = !s->prefs.short_bindings;
    frame(&f, false);
    bool prefs = s->ui_schedule.frames == before + 1;
    /* Hold the passive deadline ahead to make each immediate wake observable. */
    s->ui_schedule.next = s->time + 0.25;
    before = s->ui_schedule.frames;
    ed_select_node(f.graph, graph_node_by_id(sg_active_graph(s), 2), false);
    frame(&f, false);
    bool selection = s->ui_schedule.frames == before + 1;

    graph *selected_graph = sg_active_graph(s);
    node *first = graph_node_by_id(selected_graph, 2);
    node *second = graph_node_by_id(selected_graph, SETTER);
    node *third = graph_node_by_id(selected_graph, 6);
    assert(first && second && third);
    ed_add_selected(f.graph, second);
    frame(&f, false);
    s->active_until = 0;
    s->ui_schedule.next = s->time + 0.25;
    before = s->ui_schedule.frames;
    ed_remove_selected(f.graph, second);
    ed_add_selected(f.graph, third);
    frame(&f, false);
    test_check(s->ui_schedule.frames == before + 1, "replacing a secondary selected node wakes host emission with the first node and count unchanged");

    assert(acount(selected_graph->edges) >= 2);
    ed_link_ref first_link, second_link;
    sg_link_ref_of(&selected_graph->edges[0], &first_link);
    sg_link_ref_of(&selected_graph->edges[1], &second_link);
    ed_add_selected_link(f.graph, &first_link);
    frame(&f, false);
    s->active_until = 0;
    s->ui_schedule.next = s->time + 0.25;
    before = s->ui_schedule.frames;
    asetlen(f.graph->selected_links, 0);
    ed_add_selected_link(f.graph, &second_link);
    frame(&f, false);
    test_check(s->ui_schedule.frames == before + 1, "replacing a selected link wakes host emission with its selection count unchanged");

    s->ui_schedule.next = s->time + 0.25;
    before = s->ui_schedule.frames;
    graph *gr = sg_active_graph(s);
    assert(
        graph_set_node_presentation(gr, graph_node_by_id(gr, SETTER), NODE_PRESENTATION_EXPANDED) == 0
    );
    frame(&f, false);
    bool model = s->ui_schedule.frames == before + 1;
    s->ui_schedule.next = s->time + 0.25;
    before = s->ui_schedule.frames;
    text_event("a");
    frame(&f, false);
    bool input = s->ui_schedule.frames == before + 1;
    test_check(prefs && selection && model && input, "preferences, selection, direct graph edits and SDL text wake the host UI on the next frame");

    s->active_until = 0;
    s->in.down[0] = true;
    before = s->ui_schedule.frames;
    frames(&f, 3);
    bool held = s->ui_schedule.frames == before + 3;
    s->in.down[0] = false;
    test_check(held, "held pointer interaction retains one UI emission per simulation frame");

    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 4);
    s->active_until = 0;
    before = s->ui_schedule.frames;
    time = f.time;
    frames(&f, 60);
    uint64_t playing = s->ui_schedule.frames - before;
    app_runtime_state runtime = app_scene_authoring_runtime_state(f.scene);
    if (!runtime.playing || playing < 15 || playing >= 30 || f.time <= time + 0.99) {
        printf(
            "  playing refresh: running=%d target=%d emissions=%llu "
            "elapsed=%.6f kit_idle=%.6f rml_next=%.6f active_for=%.6f "
            "next_in=%.6f\n",
            runtime.playing,
            runtime.target,
            (unsigned long long)playing,
            f.time - time,
            kit_idle_time(s->kit),
            rml_context_next_update_delay(rml_backend_context(s->backend)),
            s->active_until - s->time,
            s->ui_schedule.next - s->time
        );
    }
    test_check(runtime.playing && playing >= 15 && playing < 30 && f.time > time + 0.99, "playing inspection refreshes at a bounded rate while runtime frames continue");
    /* A real input burst keeps the application active, while settled model
       inspection can continue at its passive cadence. Empty canvas does not
       start tooltip or control motion. */
    mouse_move(
        s,
        s->graph.surface.rect.x + s->graph.surface.rect.w - 12,
        s->graph.surface.rect.y + s->graph.surface.rect.h - 12
    );
    frame(&f, false);
    bool active = s->active_until > s->time + 0.9;
    before = s->ui_schedule.frames;
    time = f.time;
    frames(&f, 60);
    uint64_t after_input = s->ui_schedule.frames - before;
    test_check(
        active && after_input >= 15 && after_input < 30 &&
            app_scene_authoring_runtime_state(f.scene).playing && f.time > time + 0.99,
        "a real input activity window keeps runtime clocks advancing without "
        "forcing full-rate host model emissions"
    );
    studio_runtime(s, APP_RUNTIME_STOP);
    fixture_free(&f);
}

static void performance_pointer_button(studio *s, float x, float y, bool down)
{
    SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                     : SDL_EVENT_MOUSE_BUTTON_UP };
    event.button.windowID = SDL_GetWindowID(cf_app_get_window());
    event.button.button = SDL_BUTTON_MIDDLE;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x * s->prefs.ui_scale;
    event.button.y = y * s->prefs.ui_scale;
    assert(SDL_PushEvent(&event));
}

static bool performance_pan_frames(studio_fixture *f, float x, float y)
{
    studio *s = f->studio;
    bool deadline = true;
    for (int i = 0; i < 60; i++) {
        double next = s->perf_publish_next;
        uint64_t publications = s->perf_publications;
        float camera_x = s->graph.x, camera_y = s->graph.y;
        rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
        mouse_move(s, x + (float)(i % 20), y + (float)(i % 10));
        frame(f, false);
        deadline &= (!i || s->graph.x != camera_x || s->graph.y != camera_y) &&
                    rml_context_focus_element(rml_backend_context(s->backend)) == focus;
        if (s->time < next) {
            deadline &= s->perf_publications == publications &&
                        s->perf_publish_next == next;
        }
        deadline &= s->graph.gesture == STUDIO_GRAPH_PAN;
    }
    return deadline;
}

static void performance_snapshot_retains_intermediate_frames()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 90);
    float x = s->graph.surface.rect.x + s->graph.surface.rect.w / 2;
    float y = s->graph.surface.rect.y + s->graph.surface.rect.h / 2;
    mouse_move(s, x, y);
    performance_pointer_button(s, x, y, true);
    frame(&f, false);
    int recorded = acount(s->perf);
    uint64_t published = s->perf_publications, emitted = s->ui_schedule.frames;
    bool idle_deadline = performance_pan_frames(&f, x, y);
    uint64_t idle_publications = s->perf_publications - published;
    test_check(
        idle_deadline && acount(s->perf) == recorded + 60 && idle_publications >= 3 &&
            idle_publications <= 5 && s->ui_schedule.frames >= emitted + 3 &&
            s->ui_schedule.frames <= emitted + 10,
        "real idle panning advances the camera and recorder every frame while "
        "host inspection and telemetry stay bounded without losing focus or "
        "rebasing deadlines"
    );
    emitted = s->ui_schedule.frames;
    mouse_move(s, s->graph.surface.rect.x - 4, y);
    frame(&f, false);
    bool boundary = s->ui_schedule.frames == emitted + 1 &&
                    !s->graph.surface.hovered && s->graph.gesture == STUDIO_GRAPH_PAN;
    emitted = s->ui_schedule.frames;
    mouse_move(s, x, y);
    frame(&f, false);
    test_check(
        boundary && s->ui_schedule.frames == emitted + 1 &&
            s->graph.surface.hovered && s->graph.gesture == STUDIO_GRAPH_PAN,
        "a captured pan still routes leaving and reentering the graph surface "
        "through the host"
    );

    emitted = s->ui_schedule.frames;
    studio_inspect_toggle(s);
    mouse_move(s, x + 1, y + 1);
    frame(&f, false);
    test_check(studio_inspect_picking(s) && s->ui_schedule.frames == emitted + 1, "Inspect Element picking keeps host motion delivery during an established graph pan");
    studio_inspect_toggle(s);
    frame(&f, false);

    studio_palette_open(s, STUDIO_PALETTE_SEARCH, "Search");
    emitted = s->ui_schedule.frames;
    mouse_move(s, x + 2, y + 2);
    frame(&f, false);
    rml_element *input = studio_element(s, ".cmd-input input");
    test_check(
        studio_palette_is_open(s) && input &&
            rml_context_focus_element(rml_backend_context(s->backend)) == input &&
            s->ui_schedule.frames == emitted + 1,
        "a command overlay immediately takes host focus and input during graph "
        "panning"
    );
    key_event(SDL_SCANCODE_ESCAPE, true);
    key_event(SDL_SCANCODE_ESCAPE, false);
    frame(&f, false);
    performance_pointer_button(s, x, y, false);
    emitted = s->ui_schedule.frames;
    frame(&f, false);
    test_check(
        s->ui_schedule.frames == emitted + 1 &&
            s->graph.gesture == STUDIO_GRAPH_IDLE && !s->graph.captured &&
            !s->in.down[2] && !s->graph.in.down[2] && !studio_palette_is_open(s),
        "Escape and the captured pan release keep normal host and graph input "
        "lifecycles"
    );
    frames(&f, 90);

    published = s->perf_publications;
    studio_runtime(s, APP_RUNTIME_PLAY);
    frame(&f, false);
    test_check(s->perf_publish_playing && s->perf_publications == published + 1, "entering Play immediately publishes telemetry instead of waiting for the idle deadline");
    published = s->perf_publications;
    mouse_move(s, x + 1, y + 1);
    frame_delta(&f, false, 0.025);
    float spike_time = alast(s->perf).time;
    bool intermediate = s->perf_publications == published &&
                        fabsf(alast(s->perf).frame_ms - 25) < 0.001f;
    frames(&f, 2);
    bool retained = false;
    for (int i = 0; i < acount(s->perf_published); i++) {
        const studio_perf_sample *sample = &s->perf_published[i];
        retained |= sample->time == spike_time && fabsf(sample->frame_ms - 25) < 0.001f;
    }
    test_check(intermediate && retained && s->perf_publications > published, "a longer real frame between telemetry polls survives in the next complete published history");

    performance_pointer_button(s, x, y, true);
    frame(&f, false);
    recorded = acount(s->perf);
    published = s->perf_publications;
    emitted = s->ui_schedule.frames;
    bool playing_deadline = performance_pan_frames(&f, x, y);
    uint64_t playing_publications = s->perf_publications - published;
    test_check(
        playing_deadline && acount(s->perf) == recorded + 60 &&
            playing_publications >= 15 && playing_publications <= 21 &&
            s->ui_schedule.frames >= emitted + 15 &&
            s->ui_schedule.frames <= emitted + 29,
        "real playing panning retains full-rate camera input and recording "
        "while host inspection and telemetry publish at bounded rates"
    );
    performance_pointer_button(s, x, y, false);
    frame(&f, false);
    published = s->perf_publications;
    studio_runtime(s, APP_RUNTIME_STOP);
    frame(&f, false);
    test_check(
        !s->perf_publish_playing && s->perf_publications == published + 1,
        "stopping Play immediately publishes the idle telemetry mode"
    );
    fixture_free(&f);
}

/* The text of an element's .label, or "". */
static void label_text(rml_element *el, char *out, size_t size)
{
    rml_element *label = el ? rml_element_query_selector(el, ".label") : NULL;
    rml_element *node = label ? rml_element_first_child(label) : NULL;
    out[0] = '\0';
    if (node && !strcmp(rml_element_tag_name(node), "#text")) {
        rml_element_text(node, out, size);
    }
}

/* The shown element in `menu` matching `selector` labeled `name`, or NULL. */
static rml_element *labeled(rml_element *menu, const char *selector, const char *name)
{
    rml_element *items[64];
    int count = menu ? rml_element_query_selector_all(menu, selector, items, 64) : 0;
    for (int i = 0; i < count && i < 64; i++) {
        char label[64];
        label_text(items[i], label, sizeof label);
        if (!strcmp(label, name) && rml_element_is_visible(items[i], true)) {
            return items[i];
        }
    }
    return NULL;
}

static rml_element *status_bar(studio *s)
{
    return test_query(rml_backend_context(s->backend), "[data-ui=StatusBar]");
}

/* The shown labels of the status bar's key hints, joined by '|'. */
static void status_hints(studio *s, char *out, size_t size)
{
    rml_element *labels[8];
    int count = rml_element_query_selector_all(status_bar(s), "[data-ui=KeyHint] .label", labels, 8);
    out[0] = '\0';
    for (int i = 0; i < count && i < 8; i++) {
        char text[64] = "";
        rml_element_inner_rml(labels[i], text, sizeof text);
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s%s", used ? "|" : "", text);
    }
}

static bool toggle_on(studio *s)
{
    char state[8] = "";
    rml_element *assets = rml_element_query_selector(status_bar(s), ".toggle-drawer");
    rml_element_attribute(assets, "data-state", state, sizeof state);
    return !strcmp(state, "on");
}

/* Studio shows the authored status bar over its own state: the hovered
   area's hints, the last report, a drawer toggle that runs its command, and
   UI Scale native in its slot. */
static void status_bar_follows_studio()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    frames(&f, 3);
    kit_rect area = s->areas[STUDIO_AREA_TOP].rect;
    s->in.mouse_x = area.x + area.w / 2;
    s->in.mouse_y = area.y + area.h / 2;
    frames(&f, 2);
    char scene[256], elsewhere[256];
    status_hints(s, scene, sizeof scene);
    s->in.mouse_x = s->in.mouse_y = 1;
    frames(&f, 2);
    status_hints(s, elsewhere, sizeof elsewhere);
    bool hints = !strcmp(scene, "Select|Toggle|Grab|Snap|Frame") &&
                 !strcmp(elsewhere, "Select|Search|Shortcuts");

    studio_report(s, "Saved");
    frames(&f, 2);
    rml_element *report = rml_element_query_selector(status_bar(s), ".report");
    char said[32] = "";
    rml_element_attribute(rml_element_query_selector(report, "morph-text"), "aria-label", said, sizeof said);
    bool reported = rml_element_is_visible(report, false) && !strcmp(said, "Saved");
    s->status_time = s->time - 4;
    frames(&f, 2);
    reported &= rml_element_has_attribute(report, "data-ending-style") ||
                !rml_element_is_visible(report, false);

    int before = studio_command_checked(s, STUDIO_COMMAND_TOGGLE_ASSETS);
    bool shown_before = toggle_on(s) == (before == 1);
    rml_element_click(rml_element_query_selector(status_bar(s), ".toggle-drawer"));
    frames(&f, 2);
    int after = studio_command_checked(s, STUDIO_COMMAND_TOGGLE_ASSETS);
    bool toggled = shown_before && after != before && toggle_on(s) == (after == 1);

    rml_element *scale = rml_element_query_selector(status_bar(s), ".ui-scale");
    char percent[16] = "";
    label_text(scale, percent, sizeof percent);
    press(s, scale);
    frames(&f, 3);
    rml_element *menu = studio_element(s, "#scale-menu");
    rml_element *preset = labeled(menu, ".menu-item", "150%");
    bool listed = !strcmp(percent, "100%") && popover_open(menu) && preset &&
                  labeled(menu, ".menu-item[data-checked]", "100%");
    press(s, preset);
    frames(&f, 3);
    bool chosen = s->prefs.ui_scale == 1.5f && !popover_open(menu);
    s->prefs.ui_scale = 1;
    frames(&f, 2);
    if (!(hints && reported && toggled && listed && chosen)) {
        printf("  %d %d %d %d %d: %s / %s\n", hints, reported, toggled, listed, chosen, scene, elsewhere);
    }
    test_check(
        hints && reported && toggled && listed && chosen,
        "Studio's authored status bar follows the hovered area and reports, "
        "runs its drawer toggles' commands, and its UI Scale menu checks the "
        "current scale and sets another"
    );
    fixture_free(&f);
}

/* The palette finds settings: a switch flips where it is found, and any
   other setting opens Settings searching for it. */
static void palette_finds_settings()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 2);
    bool vim = s->prefs.code_vim;
    studio_command_run(s, STUDIO_COMMAND_SEARCH);
    frames(&f, 2);
    text_event("vim");
    frames(&f, 2);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 2);
    bool flipped = s->prefs.code_vim == !vim && !studio_palette_is_open(s) &&
                   studio_element(s, ".toast");
    studio_command_run(s, STUDIO_COMMAND_SEARCH);
    frames(&f, 2);
    text_event("wire style");
    frames(&f, 2);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    rml_element *search = studio_element(s, ".search-input");
    char query[64] = "";
    if (search) {
        rml_element_value(search, query, sizeof query);
    }
    bool searched = !strcmp(query, "Wire Style");
    if (!(flipped && searched)) {
        printf("  %d %d '%s'\n", flipped, searched, query);
    }
    test_check(
        flipped && searched,
        "the palette flips a switch setting where it "
        "finds it and opens Settings at any other"
    );
    fixture_free(&f);
}

/* The palette's first row's label, without its highlighting, and the group
   it heads, or "". */
static rml_element *first_shown(studio *s, const char *selector)
{
    rml_element *found[64];
    rml_element *search = studio_element(s, "#palette-search");
    int count = search
                  ? rml_element_query_selector_all(rml_element_owner_document(search), selector, found, 64)
                  : 0;
    for (int i = 0; i < count; i++) {
        if (rml_element_is_visible(found[i], true)) {
            return found[i];
        }
    }
    return NULL;
}

static void first_row(studio *s, char *label, char *group, size_t size)
{
    label[0] = group[0] = '\0';
    char rml[256] = "";
    rml_element *row = first_shown(s, "[role=option][aria-posinset=\"1\"] .label");
    if (row) {
        rml_element_inner_rml(row, rml, sizeof rml);
    }
    size_t n = 0;
    bool tag = false;
    for (const char *c = rml; *c && n + 1 < size; c++) {
        tag = *c == '<' || (tag && *c != '>');
        if (!tag && *c != '>') {
            label[n++] = *c;
        }
    }
    label[n] = '\0';
    rml_element *heading = first_shown(s, ".cmd-group");
    if (heading) {
        rml_element_inner_rml(heading, group, size);
    }
}

static void search_for(studio_fixture *f, const char *query)
{
    /* The last palette finishes leaving first. */
    frames(f, 20);
    studio_command_run(f->studio, STUDIO_COMMAND_SEARCH);
    frames(f, 2);
    if (query[0]) {
        text_event(query);
    }
    frames(f, 2);
}

/* The palette puts what was chosen lately first, alone while nothing is
   typed and above equal matches after; '>' keeps commands and settings, '@'
   the open behavior's graphs and nodes, and '?' lists the prefixes, one of
   which types itself. */
static void palette_recent_and_prefixes()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    studio_prefs saved = s->prefs;
    memset(s->prefs.recent, 0, sizeof s->prefs.recent);
    frames(&f, 2);
    char label[128], group[64];
    search_for(&f, "toggle");
    first_row(s, label, group, sizeof label);
    bool unranked = !strcmp(label, "Toggle Assets");
    tap(SDL_SCANCODE_DOWN);
    tap(SDL_SCANCODE_DOWN);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 2);
    bool remembered = !strcmp(s->prefs.recent[0].key, "command:togglePerformance") &&
                      s->prefs.recent[0].uses == 1;
    search_for(&f, "toggle");
    first_row(s, label, group, sizeof label);
    bool boosted = !strcmp(label, "Toggle Performance");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    search_for(&f, "");
    first_row(s, label, group, sizeof label);
    bool recent_first = !strcmp(label, "Toggle Performance") &&
                        !strcmp(group, "Recent");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);

    search_for(&f, ">weapons");
    first_row(s, label, group, sizeof label);
    bool commands_only = strcmp(group, "Assets") != 0;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    search_for(&f, "@");
    first_row(s, label, group, sizeof label);
    bool symbols = !strcmp(group, "In This Behavior") && label[0];
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    search_for(&f, "?");
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool prefix = studio_palette_is_open(s) && !strcmp(studio_palette_query(s), ">");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    if (!(unranked && remembered && boosted && recent_first && commands_only &&
          symbols && prefix)) {
        printf(
            "  %d %d %d %d %d %d %d '%s' '%s'\n",
            unranked,
            remembered,
            boosted,
            recent_first,
            commands_only,
            symbols,
            prefix,
            label,
            group
        );
    }
    test_check(
        unranked && remembered && boosted && recent_first,
        "the palette ranks what was chosen lately first, and leads with "
        "it while nothing is typed"
    );
    test_check(
        commands_only && symbols && prefix,
        "'>' keeps commands and settings, '@' the open behavior's "
        "symbols, and '?' types a prefix"
    );
    s->prefs = saved;
    s->applied = saved;
    fixture_free(&f);
}

/* Turns the wheel `notches` at (x, y) in dp, as SDL reports a notch. */
static void wheel_at(const studio *s, float x, float y, float notches)
{
    SDL_Event wheel = { .type = SDL_EVENT_MOUSE_WHEEL };
    wheel.wheel.windowID = SDL_GetWindowID(cf_app_get_window());
    wheel.wheel.mouse_x = x * s->prefs.ui_scale;
    wheel.wheel.mouse_y = y * s->prefs.ui_scale;
    wheel.wheel.y = notches;
    assert(SDL_PushEvent(&wheel));
}

static void button_at(const studio *s, float x, float y, Uint8 button, bool down)
{
    SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                     : SDL_EVENT_MOUSE_BUTTON_UP };
    event.button.windowID = SDL_GetWindowID(cf_app_get_window());
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x * s->prefs.ui_scale;
    event.button.y = y * s->prefs.ui_scale;
    assert(SDL_PushEvent(&event));
}

static bool near_ratio(float ratio, float expected)
{
    return fabsf(ratio / expected - 1) < 0.002f;
}

static float camera_span(const studio_fixture *f)
{
    engine_viewport_camera camera;
    engine_viewport_get_camera(f->viewport, &camera);
    return camera.span_width;
}

/* A notch scrolls a list FEEL_SCROLL_POINTS window points, and a trackpad's
   two half notches in one frame scroll as far. */
static void wheel_scrolls_lists()
{
    studio_fixture f = studio_project_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_PANEL;
    snprintf(layout->panels[STUDIO_AREA_TOP], sizeof layout->panels[0], "Widgets");
    frames(&f, 12);
    rml_element *view = studio_element(s, ".vlist");
    assert(view);
    /* The list sits low in the Widgets panel, which scrolls it into view. */
    rml_element_scroll_into_view(view, RML_SCROLL_CENTER, RML_SCROLL_NEAREST);
    frames(&f, 4);
    rml_vec2 at = rml_element_absolute_offset(view, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(view, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    float x = (at.x + size.x / 2) / ratio, y = (at.y + size.y / 2) / ratio;
    float notch = FEEL_SCROLL_POINTS * cf_app_get_pixel_scale();
    mouse_move(s, x, y);
    frames(&f, 2);
    float top = rml_element_scroll_top(view);
    wheel_at(s, x, y, -1);
    frames(&f, 30);
    float one = rml_element_scroll_top(view);
    wheel_at(s, x, y, -0.5f);
    wheel_at(s, x, y, -0.5f);
    frames(&f, 30);
    float two = rml_element_scroll_top(view);
    if (fabsf(one - top - notch) > 1 || fabsf(two - one - notch) > 1) {
        printf("  scrolled %.1f then %.1f px, a notch %.1f\n", one - top, two - one, notch);
    }
    test_check(
        fabsf(one - top - notch) <= 1 && fabsf(two - one - notch) <= 1,
        "a wheel notch scrolls a list 100 points, and a trackpad's two half "
        "notches in one frame as far"
    );
    fixture_free(&f);
}

/* A notch zooms the Scene's camera and the graph by FEEL_ZOOM_RATE once,
   however many frames draw after it; half notches zoom by half; one event
   never more than halves the zoom. */
static void wheel_zooms_canvases()
{
    studio_fixture scene = vault_open();
    studio *s = scene.studio;
    frames(&scene, 10);
    kit_rect r = s->surface.rect;
    float x = r.x + r.w / 2, y = r.y + r.h / 2;
    mouse_move(s, x, y);
    frames(&scene, 3);
    float before = camera_span(&scene);
    wheel_at(s, x, y, 1);
    frames(&scene, 8);
    float one = camera_span(&scene);
    wheel_at(s, x, y, 0.5f);
    wheel_at(s, x, y, 0.5f);
    frames(&scene, 8);
    float two = camera_span(&scene);
    wheel_at(s, x, y, -10);
    frames(&scene, 8);
    float most = camera_span(&scene);
    float notch = feel_zoom_scale(1);
    if (!near_ratio(before / one, notch) || !near_ratio(one / two, notch) ||
        !near_ratio(most / two, 2)) {
        printf("  Scene zoomed %.4f, %.4f and %.4f\n", before / one, one / two, most / two);
    }
    test_check(
        near_ratio(before / one, notch) && near_ratio(one / two, notch) &&
            near_ratio(most / two, 2),
        "a wheel notch zooms the Scene camera about 15% once, two half notches "
        "as much, and one event at most halves it"
    );
    /* A right press while the left is held reaches the Scene, whose tools
       cancel a drag with it, and only in its own frame. */
    button_at(s, x, y, SDL_BUTTON_LEFT, true);
    frames(&scene, 2);
    mouse_move(s, x + 40, y + 20);
    button_at(s, x + 40, y + 20, SDL_BUTTON_RIGHT, true);
    frame(&scene, false);
    bool heard = s->surface.pressed[1] && s->surface.down[0];
    frame(&scene, false);
    bool once = !s->surface.pressed[1];
    button_at(s, x + 40, y + 20, SDL_BUTTON_RIGHT, false);
    button_at(s, x + 40, y + 20, SDL_BUTTON_LEFT, false);
    frames(&scene, 2);
    if (kit_has_popup(s->kit)) {
        tap(SDL_SCANCODE_ESCAPE);
        frames(&scene, 2);
    }
    test_check(heard && once, "a right press during a Scene drag reaches its tools once");
    fixture_free(&scene);

    studio_fixture f = fixture_new();
    s = f.studio;
    frames(&f, 3);
    studio_graph *g = &s->graph;
    g->camera.active = false;
    g->framed = true;
    g->zoom = 1;
    frames(&f, 2);
    r = g->surface.rect;
    x = r.x + r.w / 2;
    y = r.y + r.h / 2;
    mouse_move(s, x, y);
    frames(&f, 2);
    wheel_at(s, x, y, 1);
    frames(&f, 3);
    float zoom = g->zoom;
    wheel_at(s, x, y, -0.5f);
    wheel_at(s, x, y, -0.5f);
    frames(&f, 3);
    test_check(
        near_ratio(zoom, notch) && near_ratio(g->zoom, 1),
        "a wheel notch zooms the graph about 15%, and two half notches back"
    );
    fixture_free(&f);
}

/* Runs one key press as Studio's shortcuts do, over area `hover`. */
static void press_over(studio *s, int hover, SDL_Keycode key, SDL_Keymod mod)
{
    s->hover_area = hover;
    asetlen(s->keys, 0);
    apush(s->keys, ((studio_key){ key, mod, false }));
    studio_shortcuts(s, false);
    asetlen(s->keys, 0);
}

/* Keys act on the area under the pointer (Blender): Undo there undoes the
   graph although another area was clicked last, Find opens the graph's,
   and a menu's Undo, with no key, still follows the area clicked last. */
static void keys_follow_pointer()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    int graph = s->graph.area;
    assert(graph >= 0);
    editor *e = f.graph;
    int count = acount(editor_active_graph(e)->nodes);
    assert(count > 0);
    /* Entry/Return own the function declaration; duplicate an executable
       operation so this check measures shortcut routing and graph history. */
    const struct graph *owner = editor_active_graph(e);
    node *operation = NULL;
    for (int i = 0; i < acount(owner->nodes); i++) {
        node *n = owner->nodes[i];
        if (n->id != owner->entry_id && n->id != owner->return_id) {
            operation = n;
            break;
        }
    }
    assert(operation);
    ed_clear_selection(e);
    ed_add_selected(e, operation);
    ed_duplicate_selected(e);
    bool duplicated = acount(editor_active_graph(e)->nodes) > count;
    int other = graph == STUDIO_AREA_OUTLINER ? STUDIO_AREA_PROPERTIES
                                              : STUDIO_AREA_OUTLINER;
    s->focus_area = other;
    bool menu_follows_click = studio_command_area(s) == other;
    SDL_Keymod primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    press_over(s, graph, SDLK_Z, primary);
    bool undone = acount(editor_active_graph(e)->nodes) == count;
    press_over(s, graph, SDLK_F, primary);
    bool found = s->graph.find.open;
    if (!duplicated || !menu_follows_click || !undone || !found || s->key_area >= 0) {
        printf(
            "  Pointer history: duplicated=%d menu=%d undone=%d found=%d "
            "key_area=%d count=%d/%d undo=%d redo=%d\n",
            duplicated,
            menu_follows_click,
            undone,
            found,
            s->key_area,
            acount(editor_active_graph(e)->nodes),
            count,
            editor_can_undo(e),
            editor_can_redo(e)
        );
        printf(
            "  Graph identity: working=%s document=%s active=%d "
            "source_sessions=%d\n",
            editor_behavior_asset(e)->name,
            s->scene->graph_document_id ? s->scene->graph_document_id : "(none)",
            s->scene->document.kind,
            acount(s->code.assets)
        );
        for (int i = 0; i < acount(s->code.assets); i++) {
            asset_code *code = s->code.assets[i];
            printf(
                "  Source history: %s undo=%d redo=%d pending=%d error=%s\n",
                asset_code_path(code),
                asset_code_can_replay(code, false),
                asset_code_can_replay(code, true),
                asset_code_pending(code),
                asset_code_error(code)
            );
        }
    }
    test_check(
        duplicated && menu_follows_click && undone && found && s->key_area < 0,
        "Undo and Find from the keyboard act on the area under the pointer, "
        "not the one clicked last"
    );
    fixture_free(&f);
}

/* Repeats remain limited to commands that are useful while held. */
static void shortcut_repeat_policy()
{
    studio_fixture zoom = fixture_new();
    studio *s = zoom.studio;
    SDL_Keymod primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    snprintf(s->prefs.shortcuts[STUDIO_COMMAND_ZOOM_IN], STUDIO_CHORD_SIZE, "Mod+F12");
    s->prefs.ui_scale = 1;
    apush(s->keys, ((studio_key){ SDLK_F12, primary, true }));
    studio_shortcuts(s, false);
    float held_scale = s->prefs.ui_scale;
    asetlen(s->keys, 0);
    apush(s->keys, ((studio_key){ SDLK_F12, primary, false }));
    studio_shortcuts(s, false);
    bool zoom_repeat_runs = held_scale > 1;
    bool zoom_press_runs = s->prefs.ui_scale > held_scale;
    fixture_free(&zoom);

    studio_fixture search = fixture_new();
    s = search.studio;
    primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    apush(s->keys, ((studio_key){ SDLK_K, primary, true }));
    studio_shortcuts(s, false);
    bool toggle_repeat_ignored = !studio_palette_is_open(s);
    asetlen(s->keys, 0);
    apush(s->keys, ((studio_key){ SDLK_K, primary, false }));
    studio_shortcuts(s, false);
    bool toggle_press_runs = studio_palette_is_open(s);
    test_check(
        zoom_repeat_runs && zoom_press_runs && toggle_repeat_ignored && toggle_press_runs,
        "repeatable zoom runs on a held shortcut while the Search toggle "
        "waits for a fresh press"
    );
    fixture_free(&search);
}

/* Whether the Scene's last draw framed the camera it holds now, as the
   pointer it measured shows. */
static bool drew_camera(const studio_fixture *f)
{
    engine_viewport_camera camera;
    engine_viewport_pointer pointer;
    engine_viewport_get_camera(f->viewport, &camera);
    engine_viewport_get_pointer(f->viewport, &pointer);
    float width = f->studio->surface.rect.w * f->studio->prefs.ui_scale;
    return fabsf(pointer.world_per_pixel * width - camera.span_width) <
           0.01f * camera.span_width;
}

/* The Scene draws the camera its first draw fits to the Vault, which the
   reset camera does not frame, and its zoom readout shows nothing before
   then and that camera's zoom after, so it never rolls from the reset
   camera's. */
static void readout_follows_camera()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    bool shown = false, current = true, drawn = true;
    for (int i = 0; i < 6; i++) {
        frames(&f, 1);
        drawn &= f.scene->camera_target < 0 || drew_camera(&f);
        /* The readout data-when built, not its template. */
        rml_element *readout = studio_element(s, ".viewport .readout:not([data-when])");
        if (!readout) {
            continue;
        }
        engine_viewport_view view;
        engine_viewport_get_view(f.viewport, &view);
        char value[32], expected[32];
        rml_element_attribute(readout, "aria-label", value, sizeof value);
        snprintf(expected, sizeof expected, "%d%%", (int)roundf(s->surface.rect.w / view.width * 100));
        current &= !strcmp(value, expected);
        shown = true;
    }
    test_check(
        shown && current && drawn,
        "the Scene draws the camera it fits from its first frame, and its "
        "zoom readout shows that camera"
    );
    fixture_free(&f);
}

/* The top bar's menubar trigger labeled `label`. */
static rml_element *menubar_trigger(studio *s, const char *label)
{
    rml_element *found[16];
    int count = test_query_all(rml_backend_context(s->backend), ".topbar .trigger", found, 16);
    for (int i = 0; i < count; i++) {
        char text[64] = "";
        rml_element_inner_rml(found[i], text, sizeof text);
        if (strstr(text, label)) {
            return found[i];
        }
    }
    return NULL;
}

/* Studio shows its authored top bar and area headers over its own state:
   the menubar, the play target, each area's editor type menu and its
   controls' native slot, the document tabs, whose faces and close buttons
   run their commands with the tab's index, the transport's commands, and
   each header's Maximize. */
static void chrome_follows_studio()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char path[PATH_MAX];
    /* The Text Editor shows the document, so the Scene gives up the workspace. */
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 3);
    kit_rect r;
    rml_element *heads[8], *tabs[8], *play[8];
    int shown = panel_elements(s, "AreaHeader", ".area-head", heads);
    bool slots = menubar_trigger(s, "File") &&
                 studio_element(s, "#play-target .target") && heads[0] &&
                 rml_element_query_selector(heads[0], ".editor-type") &&
                 kit_find(s->kit, "app/workspace/top/area/head/controls", &r) &&
                 s->maximized < 0 && shown >= STUDIO_AREA_COUNT - 1;
    int count = panel_elements(s, "TopBar", ".doc-tab", tabs) - 1;
    int bank = tab_named(f.scene, "Bank"), scene = bank == 0 ? 1 : 0;
    rml_element *close = bank >= 0
                           ? rml_element_query_selector(tabs[bank], ".doc-close")
                           : NULL;
    bool listed = count == acount(f.scene->tabs) && f.scene->active_tab == bank &&
                  close && rml_element_is_visible(close, true) &&
                  rml_element_has_attribute(
                      rml_element_query_selector(tabs[scene], ".doc-close"),
                      "data-disabled"
                  );
    /* The press takes the focus from the Text Editor, whose focused text
       would make its document active again, and passes the keyboard on. */
    press(s, rml_element_query_selector(tabs[scene], ".face"));
    frames(&f, 2);
    bool selected = f.scene->active_tab == scene &&
                    !kit_wants_keyboard(s->kit) && studio_ui_shortcut_focus(s);
    /* This narrow window clips the last tab, so the click goes to it. */
    rml_element_click(close);
    frames(&f, 2);
    bool closed = tab_named(f.scene, "Bank") < 0 &&
                  panel_elements(s, "TopBar", ".doc-tab", tabs) - 1 ==
                      acount(f.scene->tabs);

    panel_elements(s, "TopBar", ".group > .btn", play);
    rml_element_click(play[0]);
    frames(&f, 2);
    bool playing = app_scene_authoring_runtime_state(f.scene).playing;
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 1);

    rml_element_click(rml_element_query_selector(heads[0], ".maximize"));
    frames(&f, 2);
    bool maximized = s->maximized == STUDIO_AREA_TOP &&
                     panel_elements(s, "AreaHeader", ".area-head", heads) == 1;
    rml_element_click(rml_element_query_selector(heads[0], ".maximize"));
    frames(&f, 2);
    bool restored = maximized && s->maximized < 0 &&
                    panel_elements(s, "AreaHeader", ".area-head", heads) == shown;
    if (!(slots && listed && selected && closed && playing && restored)) {
        printf("  %d %d %d %d %d %d\n", slots, listed, selected, closed, playing, restored);
    }
    test_check(
        slots && listed && selected && closed && playing && restored,
        "Studio's authored top bar and area headers show the menubar, the "
        "play target and the controls' slots, and run tab, transport and "
        "area commands"
    );
    fixture_free(&f);
}

static rml_element *document_tab_control(studio *s, int index, const char *selector)
{
    rml_element *tabs[32];
    int count = test_query_all(rml_backend_context(s->backend), ".doc-tab", tabs, 32);
    int row = 0;
    for (int i = 0; i < count; i++) {
        if (rml_element_has_attribute(tabs[i], "data-for")) {
            continue;
        }
        if (row++ == index) {
            if (!selector) {
                return tabs[i];
            }
            rml_element *controls[8];
            int matches = rml_element_query_selector_all(tabs[i], selector, controls, 8);
            for (int j = 0; j < matches; j++) {
                if (rml_element_is_visible(controls[j], true)) {
                    return controls[j];
                }
            }
            return NULL;
        }
    }
    return NULL;
}

static void document_tab_key(studio_fixture *f, SDL_Scancode key, SDL_Keymod mod)
{
    key_event_mod(key, true, mod, false);
    key_event_mod(key, false, mod, false);
    frames(f, 3);
}

static void document_tab_press(studio_fixture *f, rml_element *control)
{
    rml_vec2 at = rml_element_absolute_offset(control, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(control, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(f->studio);
    mouse_move(f->studio, (at.x + size.x / 2) / ratio, (at.y + size.y / 2) / ratio);
    frames(f, 2);
    press(f->studio, control);
}

static void document_tab_shot(const char *state)
{
    const char *dir = getenv("CHIBI_TAB_CAPTURE_DIR");
    if (!dir || !dir[0]) {
        return;
    }
    char shot[PATH_MAX];
    assert(mkdir(dir, 0755) == 0 || access(dir, F_OK) == 0);
    snprintf(shot, sizeof shot, "%s/native-document-tabs-%s.png", dir, state);
    assert(app_save_screenshot(shot) == 0);
}

static void document_tab_pin_menu(studio_fixture *f, int index)
{
    studio *s = f->studio;
    rml_element *face = document_tab_control(s, index, ".face");
    assert(face);
    rml_vec2 at = rml_element_absolute_offset(face, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(face, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    float x = (at.x + size.x / 2) / ratio;
    float y = (at.y + size.y / 2) / ratio;
    mouse_move(s, x, y);
    button_at(s, x, y, SDL_BUTTON_RIGHT, true);
    frames(f, 1);
    button_at(s, x, y, SDL_BUTTON_RIGHT, false);
    frames(f, 3);
    rml_element *menu = studio_element(s, "#tab-menu");
    assert(menu && popover_open(menu));
    rml_element *items[16];
    int count = rml_element_query_selector_all(menu, ".menu-item", items, 16);
    rml_element *pin = NULL;
    for (int i = 0; i < count; i++) {
        char text[256];
        rml_element *label = rml_element_query_selector(items[i], ".label");
        rml_element_inner_rml(label ? label : items[i], text, sizeof text);
        if (strstr(text, "Pin Tab") && rml_element_is_visible(items[i], true)) {
            pin = items[i];
            break;
        }
    }
    if (!pin) {
        char markup[4096];
        rml_element_inner_rml(menu, markup, sizeof markup);
        printf("  tab pin menu: index=%d items=%d pointer=(%.2f,%.2f)\n%s\n", index, count, x, y, markup);
        for (int i = 0; i < count; i++) {
            char text[512];
            rml_element_inner_rml(items[i], text, sizeof text);
            printf("  item %d visible=%d: %s\n", i, rml_element_is_visible(items[i], true), text);
        }
        const char *dir = getenv("CHIBI_TAB_CAPTURE_DIR");
        if (dir && dir[0]) {
            char shot[PATH_MAX];
            assert(mkdir(dir, 0755) == 0 || access(dir, F_OK) == 0);
            snprintf(shot, sizeof shot, "%s/native-document-tab-menu.png", dir);
            assert(app_save_screenshot(shot) == 0);
        }
        fflush(stdout);
    }
    assert(pin);
    press(s, pin);
    frames(f, 3);
}

/* Document controls preserve the selected document when pinning reorders the
   strip, protect unsaved work, and keep overflow reachable without a scrollbar. */
static void document_tabs()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char path[PATH_MAX];
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 4);
    int bank = tab_named(f.scene, "Bank");
    assert(bank >= 0);
    rml_element *code = studio_element(s, "kit-code");
    assert(code && s->code.editor && rml_element_focus(code, true));
    frames(&f, 1);
    char *source = strdup(kit_code_text(s->code.editor, NULL));
    assert(source);
    int initial_count = acount(f.scene->tabs);
    document_tab_key(&f, SDL_SCANCODE_TAB, SDL_KMOD_CTRL);
    bool code_next = f.scene->active_tab == (bank + 1) % initial_count;
    document_tab_key(&f, SDL_SCANCODE_TAB, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    test_check(
        code_next && f.scene->active_tab == bank && s->code.editor &&
            !strcmp(source, kit_code_text(s->code.editor, NULL)),
        "document cycling works from the Text Editor without inserting text"
    );
    free(source);
    /* Keep another tab after Bank to catch hover shifting its neighbor. */
    assert(app_scene_authoring_open_behavior(f.scene, "Weapons") == 0);
    frames(&f, 3);
    press(s, document_tab_control(s, bank, ".face"));
    frames(&f, 3);
    code = studio_element(s, "kit-code");
    assert(code && rml_element_focus(code, true));
    mouse_move(s, 8, 100);
    frames(&f, 15);
    rml_element *close = document_tab_control(s, bank, ".doc-close");
    assert(close);
    char opacity[32];
    rml_element_property(close, "opacity", opacity, sizeof opacity);
    float rest_opacity = atof(opacity);
    bool resting = atof(opacity) < 0.01 && !document_tab_control(s, bank, ".doc-pin");
    rml_element *row = document_tab_control(s, bank, NULL);
    rml_vec2 row_at = rml_element_absolute_offset(row, RML_BOX_BORDER);
    rml_vec2 row_size = rml_element_box_size(row, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    rml_element *tab_face = document_tab_control(s, bank, ".face");
    rml_vec2 face_rest = rml_element_box_size(tab_face, RML_BOX_BORDER);
    rml_element *following = document_tab_control(s, bank + 1, NULL);
    assert(following);
    float following_x = rml_element_absolute_offset(following, RML_BOX_BORDER).x;
    bool uniform = fabsf(row_size.x - 180 * ratio) < 0.5f &&
                   fabsf(rml_element_box_size(following, RML_BOX_BORDER).x - 180 * ratio) <
                       0.5f;
    document_tab_shot("resting");
    mouse_move(s, (row_at.x + row_size.x / 2) / ratio, (row_at.y + row_size.y / 2) / ratio);
    frames(&f, 15);
    bool hover_stable = fabsf(
                            rml_element_box_size(row, RML_BOX_BORDER).x -
                            row_size.x
                        ) < 0.5f &&
                        fabsf(
                            rml_element_absolute_offset(following, RML_BOX_BORDER)
                                .x -
                            following_x
                        ) < 0.5f;
    document_tab_shot("hover");
    rml_element_property(close, "opacity", opacity, sizeof opacity);
    if (!resting || atof(opacity) <= 0.99) {
        printf(
            "  tab hover: resting=%d opacity=%.3f hover=%.3f pin=%p "
            "row=(%.2f,%.2f,%.2f,%.2f) pointer=(%.2f,%.2f) ratio=%.3f "
            "row_hover=%d close_hover=%d\n",
            resting,
            rest_opacity,
            atof(opacity),
            (void *)document_tab_control(s, bank, ".doc-pin"),
            row_at.x,
            row_at.y,
            row_size.x,
            row_size.y,
            (row_at.x + row_size.x / 2) / ratio,
            (row_at.y + row_size.y / 2) / ratio,
            ratio,
            rml_element_is_pseudo_class_set(row, "hover"),
            rml_element_is_pseudo_class_set(close, "hover")
        );
    }
    test_check(resting && atof(opacity) > 0.99, "unpinned tabs show no pin icon and reveal close only on hover");
    mouse_move(s, 8, 100);
    frames(&f, 15);
    bool leave_stable = fabsf(
                            rml_element_box_size(row, RML_BOX_BORDER).x -
                            row_size.x
                        ) < 0.5f &&
                        fabsf(
                            rml_element_absolute_offset(following, RML_BOX_BORDER)
                                .x -
                            following_x
                        ) < 0.5f;
    if (!uniform || !hover_stable || !leave_stable) {
        printf(
            "  tab geometry: uniform=%d hover_stable=%d leave_stable=%d "
            "row=%.2f face=%.2f ratio=%.2f following=%.2f\n",
            uniform,
            hover_stable,
            leave_stable,
            row_size.x,
            face_rest.x,
            ratio,
            following_x
        );
    }
    test_check(uniform && hover_stable && leave_stable, "tabs keep a uniform 180dp width and stable neighboring positions through hover");
    char *saved_label = f.scene->tabs[bank].label;
    f.scene->tabs[bank].label = strdup("CustomerAccessPermissionsAndNotificationSettings.rml");
    assert(f.scene->tabs[bank].label);
    frames(&f, 15);
    row = document_tab_control(s, bank, NULL);
    rml_element *long_label = document_tab_control(s, bank, ".label");
    assert(row && long_label);
    rml_vec2 long_at = rml_element_absolute_offset(long_label, RML_BOX_BORDER);
    rml_vec2 long_size = rml_element_box_size(long_label, RML_BOX_BORDER);
    rml_vec2 long_row_at = rml_element_absolute_offset(row, RML_BOX_BORDER);
    rml_vec2 long_row_size = rml_element_box_size(row, RML_BOX_BORDER);
    rml_element *title_fade = document_tab_control(s, bank, ".doc-title-fade");
    assert(title_fade);
    float fade_width = rml_element_box_size(title_fade, RML_BOX_BORDER).x / ratio;
    test_check(
        fabsf(long_row_size.x - 180 * ratio) < 0.5f && long_size.x > 0 &&
            long_at.x >= long_row_at.x &&
            long_at.x + long_size.x <= long_row_at.x + long_row_size.x + 0.5f &&
            fade_width >= 12 && fade_width <= 16.1f &&
            fabsf(rml_element_absolute_offset(following, RML_BOX_BORDER).x - following_x) <
                0.5f,
        "a long document title stays inside its 180dp tab with a 12 to 16dp "
        "fade and keeps its neighbor stable"
    );
    document_tab_shot("long-label");
    free(f.scene->tabs[bank].label);
    f.scene->tabs[bank].label = saved_label;
    frames(&f, 3);
    close = document_tab_control(s, bank, ".doc-close");
    assert(close);
    assert(rml_element_focus(close, true));
    frames(&f, 15);
    rml_element_property(close, "opacity", opacity, sizeof opacity);
    test_check(atof(opacity) > 0.99, "keyboard focus reveals the close action without pointer hover");
    code = studio_element(s, "kit-code");
    assert(code && rml_element_focus(code, true));
    frames(&f, 2);
    document_tab_pin_menu(&f, bank);
    bank = tab_named(f.scene, "Bank");
    row = document_tab_control(s, bank, NULL);
    assert(row);
    test_check(
        bank == 0 && f.scene->tabs[bank].pinned && f.scene->active_tab == bank &&
            !document_tab_control(s, bank, ".doc-close") &&
            fabsf(rml_element_box_size(row, RML_BOX_BORDER).x - 180 * ratio) < 0.5f,
        "pinning a document moves it first, preserves the active document, and "
        "replaces close with unpin"
    );
    rml_element *pin = document_tab_control(s, bank, ".doc-pin");
    assert(pin);
    press(s, pin);
    frames(&f, 3);
    bank = tab_named(f.scene, "Bank");
    test_check(
        bank >= 0 && !f.scene->tabs[bank].pinned && f.scene->active_tab == bank &&
            document_tab_control(s, bank, ".doc-close"),
        "unpin restores the close control and preserves the active document"
    );

    assert(app_scene_authoring_open_behavior(f.scene, "Teller") == 0);
    frames(&f, 4);
    int count = acount(f.scene->tabs);
    int selected = f.scene->active_tab;
    assert(count >= 3);
    rml_element *face = document_tab_control(s, selected, ".face");
    assert(face && rml_element_focus(face, true));
    document_tab_key(&f, SDL_SCANCODE_TAB, SDL_KMOD_CTRL);
    bool next = f.scene->active_tab == (selected + 1) % count;
    document_tab_key(&f, SDL_SCANCODE_TAB, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    test_check(next && f.scene->active_tab == selected, "Control Tab cycles open documents and Shift reverses the cycle");
    face = document_tab_control(s, f.scene->active_tab, ".face");
    assert(face && rml_element_focus(face, true));
    document_tab_key(&f, SDL_SCANCODE_HOME, SDL_KMOD_NONE);
    bool home = f.scene->active_tab == 0;
    document_tab_key(&f, SDL_SCANCODE_RIGHT, SDL_KMOD_NONE);
    bool right = f.scene->active_tab == 1;
    document_tab_key(&f, SDL_SCANCODE_END, SDL_KMOD_NONE);
    test_check(home && right && f.scene->active_tab == count - 1, "tab focus supports Home, Right Arrow and End navigation");

    /* Constrain the strip so the menu must reveal a document outside its viewport. */
    kit_style_sheet(s->kit, "document-tabs-probe", ".doc-tab-list { width: 220dp; max-width: 220dp; flex-grow: 0; flex-shrink: 0; }");
    frames(&f, 4);
    rml_element *trigger = studio_element(s, ".open-tabs");
    assert(trigger && rml_element_is_visible(trigger, true));
    press(s, trigger);
    frames(&f, 3);
    rml_element *menu = studio_element(s, "#open-tabs-menu");
    rml_element *items[32];
    int menu_count = menu
                       ? rml_element_query_selector_all(menu, ".menu-item", items, 32)
                       : 0;
    rml_element *bank_item = NULL;
    for (int i = 0; i < menu_count; i++) {
        char text[256];
        rml_element_inner_rml(rml_element_query_selector(items[i], ".label"), text, sizeof text);
        if (strstr(text, "Bank") && rml_element_is_visible(items[i], true)) {
            bank_item = items[i];
            break;
        }
    }
    bool menu_open = menu && popover_open(menu);
    assert(bank_item);
    press(s, bank_item);
    frames(&f, 4);
    bank = tab_named(f.scene, "Bank");
    face = document_tab_control(s, bank, ".face");
    rml_element *strip = studio_element(s, ".doc-tab-list");
    assert(face && strip);
    rml_vec2 strip_at = rml_element_absolute_offset(strip, RML_BOX_CONTENT);
    rml_vec2 strip_size = rml_element_box_size(strip, RML_BOX_CONTENT);
    rml_vec2 face_at = rml_element_absolute_offset(face, RML_BOX_BORDER);
    rml_vec2 face_size = rml_element_box_size(face, RML_BOX_BORDER);
    bool revealed = face_at.x >= strip_at.x - 1 &&
                    face_at.x + face_size.x <= strip_at.x + strip_size.x + 1;
    test_check(menu_open && f.scene->active_tab == bank && !popover_open(menu) && revealed, "the open documents menu selects and reveals a tab in a constrained strip");

    const char *dir = getenv("CHIBI_TAB_CAPTURE_DIR");
    if (dir && dir[0]) {
        char shot[PATH_MAX];
        assert(mkdir(dir, 0755) == 0 || access(dir, F_OK) == 0);
        frames(&f, 30);
        snprintf(shot, sizeof shot, "%s/native-document-tabs.png", dir);
        assert(app_save_screenshot(shot) == 0);
    }
    kit_style_sheet(s->kit, "document-tabs-probe", "");
    frames(&f, 3);

    f.scene->tabs[bank].source_dirty = true;
    close = document_tab_control(s, bank, ".doc-close");
    assert(close);
    document_tab_press(&f, close);
    frames(&f, 3);
    char title[256] = "";
    rml_element *toast = studio_element(s, ".toast .title");
    if (toast) {
        rml_element_inner_rml(toast, title, sizeof title);
    }
    test_check(
        tab_named(f.scene, "Bank") >= 0 && acount(f.scene->tabs) == count &&
            strstr(title, "without saving"),
        "closing an unsaved tab retains its document and asks before "
        "discarding edits"
    );
    document_tab_pin_menu(&f, bank);
    bank = tab_named(f.scene, "Bank");
    rml_element *action = studio_element(s, ".toast-action");
    assert(action);
    press(s, action);
    frames(&f, 3);
    bank = tab_named(f.scene, "Bank");
    test_check(bank >= 0 && f.scene->tabs[bank].pinned && acount(f.scene->tabs) == count, "pinning a tab while discard confirmation is pending protects it from the old close action");
    pin = document_tab_control(s, bank, ".doc-pin");
    assert(pin);
    press(s, pin);
    frames(&f, 3);
    bank = tab_named(f.scene, "Bank");
    f.scene->tabs[bank].source_dirty = false;
    kit_toast_dismiss(s->kit, s->code.close_toast);
    int adjacent = bank + 1 < count ? bank + 1 : bank - 1;
    char adjacent_key[PATH_MAX];
    snprintf(adjacent_key, sizeof adjacent_key, "%s", f.scene->tabs[adjacent].key);
    SDL_Keymod primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    code = studio_element(s, "kit-code");
    assert(code && rml_element_focus(code, true));
    document_tab_key(&f, SDL_SCANCODE_W, primary);
    test_check(
        tab_named(f.scene, "Bank") < 0 && acount(f.scene->tabs) == count - 1 &&
            !strcmp(f.scene->tabs[f.scene->active_tab].key, adjacent_key),
        "the close shortcut removes a clean active document and selects its "
        "adjacent tab"
    );
    int scene = -1;
    for (int i = 0; i < acount(f.scene->tabs); i++) {
        if (f.scene->tabs[i].kind == APP_EDITOR_DOCUMENT_SCENE) {
            scene = i;
        }
    }
    assert(scene >= 0);
    char root_key[PATH_MAX];
    snprintf(root_key, sizeof root_key, "%s", f.scene->tabs[scene].key);
    assert(realpath(PROJECT "/content/scenes/Main.chibi", path));
    assert(scene_tabs_open_scene(f.scene, path) == 0);
    frames(&f, 4);
    int second_scene = f.scene->active_tab;
    assert(second_scene != scene && f.scene->tabs[second_scene].kind == APP_EDITOR_DOCUMENT_SCENE);
    close = document_tab_control(s, second_scene, ".doc-close");
    assert(close && !rml_element_has_attribute(close, "data-disabled"));
    f.scene->scene_dirty = true;
    document_tab_press(&f, close);
    frames(&f, 3);
    test_check(
        acount(f.scene->tabs) == count && f.scene->active_tab == second_scene &&
            s->code.close_toast != 0,
        "closing an unsaved scene retains its tab until discard is confirmed"
    );
    f.scene->scene_dirty = false;
    kit_toast_dismiss(s->kit, s->code.close_toast);
    close = document_tab_control(s, second_scene, ".doc-close");
    assert(close);
    document_tab_press(&f, close);
    frames(&f, 4);
    scene = f.scene->active_tab;
    test_check(
        acount(f.scene->tabs) == count - 1 &&
            f.scene->tabs[scene].kind == APP_EDITOR_DOCUMENT_SCENE &&
            !strcmp(f.scene->tabs[scene].key, root_key),
        "the scene close button removes a clean second scene and restores the "
        "remaining scene"
    );
    close = document_tab_control(s, scene, ".doc-close");
    assert(close);
    bool disabled = rml_element_has_attribute(close, "data-disabled");
    mouse_move(s, 8, 100);
    frames(&f, 3);
    rml_element_property(close, "opacity", opacity, sizeof opacity);
    bool root_close_hidden = atof(opacity) < 0.01;
    rml_element_property(rml_element_query_selector(close, ".icon"), "opacity", opacity, sizeof opacity);
    root_close_hidden = root_close_hidden && atof(opacity) < 0.01;
    rml_element_click(close);
    frames(&f, 3);
    test_check(disabled && root_close_hidden && acount(f.scene->tabs) == count - 1, "the final scene tab keeps its disabled close action hidden at rest and stays open");
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 4);
    bank = tab_named(f.scene, "Bank");
    int teller = tab_named(f.scene, "Teller");
    assert(bank >= 0 && teller >= 0 && f.scene->active_tab == bank);
    assert(!scene_tabs_dirty(f.scene, &f.scene->tabs[teller]));
    char bank_key[PATH_MAX];
    snprintf(bank_key, sizeof bank_key, "%s", f.scene->tabs[bank].key);
    int background_count = acount(f.scene->tabs);
    close = document_tab_control(s, teller, ".doc-close");
    assert(close);
    document_tab_press(&f, close);
    frames(&f, 4);
    test_check(
        tab_named(f.scene, "Teller") < 0 &&
            acount(f.scene->tabs) == background_count - 1 &&
            !strcmp(f.scene->tabs[f.scene->active_tab].key, bank_key),
        "closing an inactive graph tab preserves the active Text Editor "
        "document"
    );
    fixture_free(&f);
}

/* Global shortcuts remain available after authored toolbar controls take Rml
   focus, while an authored text input keeps its keys. */
static void toolbar_shortcuts_keep_text_focus()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    char path[PATH_MAX];
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 4);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *button = studio_element(s, ".side-pane");
    assert(context && button);
    press(s, button);
    frames(&f, 1);
    studio_command_run(s, STUDIO_COMMAND_TOGGLE_SIDE_PANE);
    frames(&f, 1);
    /* Toolbar actions can remount the authored header; focus the button
       from the reconciled document before sending the shortcut. */
    button = studio_element(s, ".side-pane");
    assert(button);
    rml_element_focus(button, true);
    frames(&f, 1);
    bool button_allowed = studio_ui_shortcut_focus(s);
    bool button_keyboard = kit_wants_keyboard(s->kit);
    /* A focused chrome button passes the keyboard on, as a page's does. */
    bool button_focus = button_allowed && !button_keyboard;

    SDL_Keymod primary = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    key_event_mod(SDL_SCANCODE_K, true, primary, false);
    key_event_mod(SDL_SCANCODE_K, false, primary, false);
    frames(&f, 2);
    bool button_shortcut = studio_palette_is_open(s);
    key_event(SDL_SCANCODE_ESCAPE, true);
    key_event(SDL_SCANCODE_ESCAPE, false);
    frames(&f, 2);
    bool dismissed = !studio_palette_is_open(s);

    rml_element *tab = studio_element(s, ".doc-tab .face");
    assert(tab);
    press(s, tab);
    frames(&f, 1);
    tab = studio_element(s, ".doc-tab .face");
    assert(tab && rml_element_focus(tab, true));
    frames(&f, 1);
    bool tab_focus = rml_context_focus_element(context) == tab &&
                     studio_ui_shortcut_focus(s) && !kit_wants_keyboard(s->kit);
    key_event_mod(SDL_SCANCODE_K, true, primary, false);
    key_event_mod(SDL_SCANCODE_K, false, primary, false);
    frames(&f, 2);
    bool tab_shortcut = studio_palette_is_open(s);
    key_event(SDL_SCANCODE_ESCAPE, true);
    key_event(SDL_SCANCODE_ESCAPE, false);
    frames(&f, 2);

    rml_element *input = studio_element(s, "input[placeholder=Filter]");
    assert(input);
    bool focused_input = rml_element_focus(input, true);
    frames(&f, 1);
    bool input_keyboard = kit_wants_keyboard(s->kit);
    bool input_allowed = studio_ui_shortcut_focus(s);
    bool input_focus = focused_input && rml_context_focus_element(context) == input &&
                       input_keyboard && !input_allowed;
    /* A plain key stays in the field: Space types, not Play. */
    key_event(SDL_SCANCODE_SPACE, true);
    key_event(SDL_SCANCODE_SPACE, false);
    frames(&f, 2);
    rml_element *input_focus_after = rml_context_focus_element(context);
    bool input_protected = !app_scene_authoring_runtime_state(s->scene).playing &&
                           input_focus_after == input && kit_wants_keyboard(s->kit);
    /* A chord the field edits no text with still reaches Studio. */
    key_event_mod(SDL_SCANCODE_K, true, primary, false);
    key_event_mod(SDL_SCANCODE_K, false, primary, false);
    frames(&f, 2);
    bool input_palette = studio_palette_is_open(s);
    input_protected &= input_palette;

    if (!(button_focus && button_shortcut && dismissed && tab_focus &&
          tab_shortcut && input_focus && input_protected)) {
        printf("  button allowed %d, keyboard %d\n", button_allowed, button_keyboard);
        printf("  tab focus %d, shortcut %d\n", tab_focus, tab_shortcut);
        printf(
            "  %d %d %d %d %d (open %d, focus %s, input %d, allowed %d, "
            "keyboard %d, attempted %d, typing %d)\n",
            button_focus,
            button_shortcut,
            dismissed,
            input_focus,
            input_protected,
            input_palette,
            input_focus_after ? rml_element_tag_name(input_focus_after) : "none",
            input_focus_after == input,
            input_allowed,
            input_keyboard,
            focused_input,
            s->typing
        );
    }
    test_check(
        button_focus && button_shortcut && dismissed && tab_focus &&
            tab_shortcut && input_focus && input_protected,
        "focused authored toolbar buttons and document tabs allow global "
        "shortcuts, and text inputs keep their keys but pass Studio's other "
        "chords"
    );
    fixture_free(&f);
}

static bool beside(const studio *s)
{
    kit_rect top = s->areas[STUDIO_AREA_TOP].rect,
             bottom = s->areas[STUDIO_AREA_BOTTOM].rect;
    return bottom.w > 0 && fabsf(bottom.y - top.y) < 1 && bottom.x >= top.x + top.w;
}

static bool stacked(const studio *s)
{
    kit_rect top = s->areas[STUDIO_AREA_TOP].rect,
             bottom = s->areas[STUDIO_AREA_BOTTOM].rect;
    return bottom.w > 0 && fabsf(bottom.x - top.x) < 1 && bottom.y >= top.y + top.h;
}

static bool side_shown(const studio *s)
{
    return s->areas[STUDIO_AREA_OUTLINER].rect.w > 0 &&
           s->areas[STUDIO_AREA_PROPERTIES].rect.w > 0;
}

static void select_tab(studio_fixture *f, int tab)
{
    studio_command_run_with(f->studio, STUDIO_COMMAND_SELECT_TAB, tab);
    frames(f, 3);
}

/* Opening documents and changing tabs keep the selected split mode. Each mode
   remembers its arrangement, and switching modes preserves authoring state. */
static void layout_modes_are_explicit()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_layout *vertical = &s->prefs.layouts[STUDIO_WORKSPACE_VERTICAL_SPLIT];
    studio_layout *horizontal = &s->prefs.layouts[STUDIO_WORKSPACE_HORIZONTAL_SPLIT];
    vertical->side_width = 416;
    vertical->main_split = 0.66f;
    s->prefs.code_apply_live = false;
    s->applied = s->prefs;
    frames(&f, 2);
    select_instance(f.scene, 0);
    int actor = f.scene->selected;

    char path[PATH_MAX];
    assert(realpath(BANK, path) && studio_open_ui_source(s, path, 1));
    frames(&f, 4);
    bool opened_kept_mode = s->prefs.workspace == STUDIO_WORKSPACE_VERTICAL_SPLIT;
    int bank = tab_named(f.scene, "Bank"), scene = bank == 0 ? 1 : 0;
    studio_command_run(s, STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT);
    frames(&f, 3);
    kit_rect r;
    bool horizontal_shown = s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT &&
                            beside(s) &&
                            kit_find(s->kit, "app/workspace/bottom/area/body/code/text", &r) &&
                            !horizontal->drawer_open && vertical->drawer_open &&
                            vertical->editors[STUDIO_AREA_DRAWER] == STUDIO_EDITOR_CODE;
    ui_buffer *buffer = studio_ui_buffer(s);
    studio_designer *designer = designer_state(s, &buffer);
    assert(buffer && designer && acount(designer->rml->elements) > 0);
    designer_select(s, 0);
    int length = (int)strlen(ui_buffer_text(buffer, UI_PART_RML));
    text_edit edit = { length, length, "<!-- workspace -->\n" };
    ui_buffer_edit(buffer, UI_PART_RML, &edit, 1);
    frames(&f, 2);
    kit_code_editor *code = s->code.editor;
    int cursor = kit_code_cursor(code);
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(&f, 2);

    select_tab(&f, scene);
    bool tab_kept_horizontal = s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT &&
                               beside(s);
    studio_command_run(s, STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT);
    frames(&f, 3);
    bool vertical_shown = s->prefs.workspace == STUDIO_WORKSPACE_VERTICAL_SPLIT &&
                          stacked(s) && vertical->side_width == 416 &&
                          vertical->main_split == 0.66f && vertical->drawer_open;
    select_tab(&f, bank);
    bool tab_kept_vertical = s->prefs.workspace == STUDIO_WORKSPACE_VERTICAL_SPLIT &&
                             stacked(s);
    studio_command_run(s, STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT);
    frames(&f, 3);
    bool returned = s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT &&
                    beside(s) && studio_ui_buffer(s) == buffer &&
                    s->code.editor == code && kit_code_cursor(code) == cursor &&
                    designer->selected == 0 && f.scene->selected == actor &&
                    app_scene_authoring_runtime_state(f.scene).playing;

    rml_element *toggles[2];
    assert(panel_elements(s, "TopBar", ".side-pane", toggles) == 1);
    press(s, rml_element_query_selector(toggles[0], ".icon"));
    frames(&f, 30);
    bool side_hidden = !horizontal->side_open && !side_shown(s) && vertical->side_open;
    rml_element *area_heads[8];
    bool side_headers_hidden = panel_elements(s, "AreaHeader", ".area-head", area_heads) ==
                               2;
    studio_command_run(s, STUDIO_COMMAND_WORKSPACE_VERTICAL_SPLIT);
    frames(&f, 30);
    bool side_kept = vertical->side_open && side_shown(s);
    studio_command_run(s, STUDIO_COMMAND_WORKSPACE_HORIZONTAL_SPLIT);
    frames(&f, 30);
    bool side_remembered = !horizontal->side_open && !side_shown(s);
    studio_show_editor(s, STUDIO_EDITOR_OUTLINER);
    frames(&f, 30);
    bool side_reopened = horizontal->side_open && side_shown(s);
    int headers = panel_elements(s, "AreaHeader", ".area-head", area_heads);
    char area_name[32], alias[32];
    snprintf(area_name, sizeof area_name, "editor.areas[%d]", STUDIO_AREA_OUTLINER);
    bool top_header_restored = false;
    for (int i = 0; i < headers; i++) {
        rml_element *host = rml_element_parent(area_heads[i]);
        if (rml_element_attribute(host, "data-alias-area", alias, sizeof alias) > 0 &&
            !strcmp(alias, area_name)) {
            rml_vec2 size = rml_element_box_size(area_heads[i], RML_BOX_BORDER);
            top_header_restored = rml_element_is_visible(area_heads[i], true) &&
                                  size.y > 0;
            break;
        }
    }
    bool side_header_restored = headers == 4 && top_header_restored;

    horizontal->main_split = 0.75f;
    studio_command_run(s, STUDIO_COMMAND_RESET_LAYOUT);
    frames(&f, 2);
    bool reset_one = horizontal->main_split == 0.5f &&
                     vertical->main_split == 0.66f && vertical->side_width == 416;
    studio_history(s, false);
    bool undone = !strstr(ui_buffer_text(buffer, UI_PART_RML), "<!-- workspace -->");
    studio_history(s, true);
    bool redone = strstr(ui_buffer_text(buffer, UI_PART_RML), "<!-- workspace -->") !=
                  NULL;
    bool playing = app_scene_authoring_runtime_state(f.scene).playing;

    select_tab(&f, scene);
    bool tab_kept_mode = s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(&f, 1);
    if (!(opened_kept_mode && horizontal_shown && tab_kept_horizontal && vertical_shown &&
          tab_kept_vertical && returned && side_hidden && side_headers_hidden &&
          side_kept && side_remembered && side_reopened && side_header_restored &&
          reset_one && undone && redone && playing && tab_kept_mode)) {
        printf(
            "  %d %d %d %d %d %d | %d %d %d %d %d %d | %d %d %d %d %d\n",
            opened_kept_mode,
            horizontal_shown,
            tab_kept_horizontal,
            vertical_shown,
            tab_kept_vertical,
            returned,
            side_hidden,
            side_headers_hidden,
            side_kept,
            side_remembered,
            side_reopened,
            side_header_restored,
            reset_one,
            undone,
            redone,
            playing,
            tab_kept_mode
        );
    }
    test_check(
        opened_kept_mode && horizontal_shown && tab_kept_horizontal &&
            vertical_shown && tab_kept_vertical && returned,
        "opening documents and changing tabs keep the selected split mode"
    );
    test_check(
        side_hidden && side_headers_hidden && side_kept && side_remembered &&
            side_reopened && side_header_restored,
        "the top bar hides the side pane per mode and restores its editor "
        "headers"
    );
    test_check(
        reset_one && undone && redone && playing && tab_kept_mode,
        "Reset Layout resets the active mode only; switching keeps "
        "history and playback"
    );
    fixture_free(&f);
}

/* Each area is a document of its own (workstream F5): text rewritten in the
   Outliner lays out none of the others. The guest Studio, run in the Scene
   area, places its own areas over theirs the same way, and stops. */
static void area_documents()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    frames(&f, 6);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *name = test_query(context, "#outliner-list .tree-row .name");
    rml_element *outliner = name ? rml_element_owner_document(name) : NULL;
    int documents = rml_context_document_count(context);
    for (int i = 0; outliner && i < documents; i++) {
        rml_element *document = rml_context_document(context, i);
        if (document != outliner) {
            bench_probe(document);
        }
    }
    frames(&f, 30);
    uint64_t before = bench_layouts();
    rml_element_set_inner_rml(name, "Renamed in place");
    rml_backend_update(s->backend, 0);
    test_check(
        outliner && documents >= 8 && bench_layouts() == before,
        "text rewritten in the Outliner lays out the Outliner's document "
        "and no other area's, bar's or the layer's"
    );
    studio_guest_start(s);
    frames(&f, 8);
    kit_rect top = { 0 };
    bool guest = s->guest &&
                 kit_find(s->guest->kit, "app/workspace/top/area", &top) &&
                 top.w > 0 && top.h > 0 &&
                 rml_context_document_count(rml_backend_context(s->guest->backend)) >= 8;
    studio_guest_stop(s);
    frames(&f, 2);
    test_check(
        guest && !s->guest,
        "the guest Studio lays its areas over their places in documents of "
        "its own, and stops"
    );
    fixture_free(&f);
}

/* A point inside `el` outside `document`'s box: where it draws over another
   document. False when it has none. */
static bool beyond(rml_element *el, rml_element *document, rml_vec2 *point)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    rml_vec2 box_at = rml_element_absolute_offset(document, RML_BOX_BORDER);
    rml_vec2 box = rml_element_box_size(document, RML_BOX_BORDER);
    for (float y = at.y + 4; y < at.y + size.y - 4; y += 4) {
        if (y < box_at.y || y > box_at.y + box.y) {
            *point = (rml_vec2){ at.x + size.x / 2, y };
            return true;
        }
    }
    return false;
}

static bool inside(rml_element *el, const rml_element *ancestor)
{
    for (; el; el = rml_element_parent(el)) {
        if (el == ancestor) {
            return true;
        }
    }
    return false;
}

/* Each area header's editor type menu is an authored Menu over
   choices.editors: it draws over the areas beside its own, marks the area's
   editor, and a chosen item shows that editor there. The Behavior header's
   View menu toggles a view preference and stays open; Escape closes it. */
static void area_menus()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    frames(&f, 6);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *heads[8];
    int shown = panel_elements(s, "AreaHeader", ".area-head", heads);
    bool over = false, current = true;
    for (int i = 0; i < shown; i++) {
        press(s, rml_element_query_selector(heads[i], ".editor-type"));
        frames(&f, 3);
        rml_element *menu = rml_element_query_selector(heads[i], "#editor-types");
        char label[64];
        label_text(rml_element_query_selector(menu, "[data-current]"), label, sizeof label);
        current &= popover_open(menu) && label[0];
        rml_vec2 point;
        over |= beyond(menu, rml_element_owner_document(menu), &point) &&
                inside(rml_context_element_at_point(context, point, NULL), menu);
        press(s, rml_element_query_selector(heads[i], ".editor-type"));
        frames(&f, 2);
    }
    studio_layout *layout = studio_active_layout(s);
    studio_editor before[STUDIO_AREA_COUNT];
    memcpy(before, layout->editors, sizeof before);
    press(s, rml_element_query_selector(heads[0], ".editor-type"));
    frames(&f, 3);
    rml_element *members = labeled(
        rml_element_query_selector(heads[0], "#editor-types"),
        "[role=menuitem]",
        "Members"
    );
    if (members) {
        press(s, members);
        frames(&f, 3);
    }
    int shows = 0, changed = 0;
    for (int a = 0; a < STUDIO_AREA_COUNT; a++) {
        shows += layout->editors[a] == STUDIO_EDITOR_MEMBERS;
        changed += layout->editors[a] != before[a];
    }
    test_check(
        shown >= 2 && current && over && shows == 1 && changed == 1 &&
            !popover_top(context),
        "an area header's editor type menu marks its editor, draws over the "
        "next area, and its item shows that editor in the area"
    );
    memcpy(layout->editors, before, sizeof before);
    frames(&f, 4);
    rml_element *view = studio_element(s, "[data-ui=GraphView] button");
    bool compact = s->prefs.compact_nodes;
    if (view) {
        press(s, view);
        frames(&f, 3);
    }
    rml_element *menu = studio_element(s, "#graph-view");
    rml_element *toggle = labeled(menu, ".menu-check", "Compact Nodes");
    if (toggle) {
        press(s, toggle);
        frames(&f, 3);
    }
    bool toggled = toggle && s->prefs.compact_nodes != compact && popover_open(menu);
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    test_check(
        view && toggled && !popover_open(menu),
        "the Behavior header's View menu toggles a view preference, stays "
        "open, and closes on Escape"
    );
    s->prefs.compact_nodes = compact;
    fixture_free(&f);
}

/* ---------- Shots (CHIBI_SHELL_CAPTURE_DIR) ---------- */

static void shell_shot(studio_fixture *f, const char *dir, const char *variant, const char *state)
{
    char path[PATH_MAX];
    frames(f, 30);
    snprintf(path, sizeof path, "%s/native-%s-%s.png", dir, variant, state);
    assert(app_save_screenshot(path) == 0);
}

static void shot_open(
    studio_fixture *f,
    rml_element *trigger,
    const char *dir,
    const char *variant,
    const char *state
)
{
    if (!trigger) {
        fprintf(stderr, "shell shots: no trigger for %s\n", state);
        return;
    }
    press(f->studio, trigger);
    shell_shot(f, dir, variant, state);
    tap(SDL_SCANCODE_ESCAPE);
    tap(SDL_SCANCODE_ESCAPE);
    frames(f, 6);
}

/* Native shots of the shell: the window at rest, each top bar menu and
   the status bar's UI Scale open, the drawer open, an area maximized, the
   side pane closed and the horizontal split. */
static void capture_shell(const char *dir)
{
    static const struct
    {
        const char *name;
        studio_theme theme;
        bool comfortable;
        float scale;
    } VARIANTS[] = { { "dark-compact", STUDIO_THEME_DARK, false, 1 },
                     { "dark-comfortable", STUDIO_THEME_DARK, true, 1 },
                     { "light-compact", STUDIO_THEME_LIGHT, false, 1 },
                     { "light-comfortable", STUDIO_THEME_LIGHT, true, 1 },
                     { "dark-compact-150", STUDIO_THEME_DARK, false, 1.5f },
                     { "light-comfortable-150", STUDIO_THEME_LIGHT, true, 1.5f } };
    for (size_t v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++) {
        const char *name = VARIANTS[v].name;
        studio_fixture f = scene_open(PROJECT, PROJECT "/content/scenes/Main.chibi", "Weapons");
        studio *s = f.studio;
        s->maximized = -1;
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        s->prefs.ui_scale = VARIANTS[v].scale;
        mouse_move(s, 600, 790);
        frames(&f, 30);
        shell_shot(&f, dir, name, "base");
        shot_open(&f, menubar_trigger(s, "File"), dir, name, "file");
        shot_open(&f, menubar_trigger(s, "View"), dir, name, "view");
        press(s, menubar_trigger(s, "Object"));
        frames(&f, 4);
        tap(SDL_SCANCODE_DOWN);
        frames(&f, 2);
        tap(SDL_SCANCODE_RIGHT);
        shell_shot(&f, dir, name, "object-add");
        tap(SDL_SCANCODE_ESCAPE);
        tap(SDL_SCANCODE_ESCAPE);
        frames(&f, 6);
        shot_open(&f, studio_element(s, ".target"), dir, name, "target");
        shot_open(&f, studio_element(s, "#layout .btn"), dir, name, "layout");
        shot_open(&f, studio_element(s, ".speed"), dir, name, "speed");
        shot_open(&f, studio_element(s, ".ui-scale"), dir, name, "scale");
        studio_command_run(s, STUDIO_COMMAND_TOGGLE_OUTPUT);
        shell_shot(&f, dir, name, "drawer");
        studio_command_run(s, STUDIO_COMMAND_TOGGLE_OUTPUT);
        studio_toggle_maximized(s, STUDIO_AREA_TOP);
        shell_shot(&f, dir, name, "maximized");
        studio_toggle_maximized(s, STUDIO_AREA_TOP);
        studio_command_run(s, STUDIO_COMMAND_TOGGLE_SIDE_PANE);
        shell_shot(&f, dir, name, "side-closed");
        studio_command_run(s, STUDIO_COMMAND_TOGGLE_SIDE_PANE);
        studio_set_workspace(s, STUDIO_WORKSPACE_HORIZONTAL_SPLIT);
        shell_shot(&f, dir, name, "across");
        fixture_free(&f);
    }
}

/* The pointer over `el`, as a press starts. */
static void hover(studio *s, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    rml_backend_process_input(
        s->backend,
        &(rml_input){ .type = RML_INPUT_POINTER_MOVE,
                      .x = (at.x + size.x * 0.5f) / ratio,
                      .y = (at.y + size.y * 0.5f) / ratio }
    );
}

static bool menu_open(studio *s, const char *id)
{
    rml_element *menu = studio_element(s, id);
    return menu && popover_open(menu);
}

/* The top bar's menubar (workstream C3): a press opens a menu and marks its
   trigger, hovering another trigger or Right switches menus, Escape closes
   them, a MenuCommand runs its command and File's and Object's lists come
   from the `menubar` source. */
static void menubar_runs_commands()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    frames(&f, 4);
    rml_element *file = menubar_trigger(s, "File");
    press(s, file);
    frames(&f, 3);
    bool opened = menu_open(s, "#menu-file") &&
                  rml_element_has_attribute(file, "data-popup-open");
    hover(s, menubar_trigger(s, "Edit"));
    frames(&f, 3);
    bool hovered = menu_open(s, "#menu-edit") && !menu_open(s, "#menu-file") &&
                   !rml_element_has_attribute(file, "data-popup-open");
    tap(SDL_SCANCODE_RIGHT);
    frames(&f, 3);
    bool stepped = menu_open(s, "#menu-view") && !menu_open(s, "#menu-edit");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    bool closed = !menu_open(s, "#menu-view");

    studio_layout *layout = studio_active_layout(s);
    layout->drawer_open = false;
    frames(&f, 2);
    press(s, menubar_trigger(s, "View"));
    frames(&f, 3);
    rml_element *output = labeled(studio_element(s, "#menu-view"), ".menu-item", "Output");
    press(s, output);
    frames(&f, 3);
    bool ran = output && layout->drawer_open &&
               layout->editors[STUDIO_AREA_DRAWER] == STUDIO_EDITOR_OUTPUT &&
               !menu_open(s, "#menu-view");

    int named = 0;
    for (int i = 0; i < acount(f.scene->prototypes); i++) {
        named += f.scene->prototypes[i].prototype &&
                 f.scene->prototypes[i].prototype->name;
    }
    int actors = acount(f.scene->actors);
    press(s, menubar_trigger(s, "Object"));
    frames(&f, 3);
    tap(SDL_SCANCODE_DOWN);
    tap(SDL_SCANCODE_RIGHT);
    frames(&f, 6);
    rml_element *rows[32];
    rml_element *add = studio_element(s, "#menu-object .submenu .popup");
    int listed = add ? rml_element_query_selector_all(add, ".menu-item", rows, 32) : 0;
    int shown = 0;
    rml_element *last = NULL;
    for (int i = 0; i < listed && i < 32; i++) {
        bool visible = rml_element_is_visible(rows[i], true);
        shown += visible;
        last = visible ? rows[i] : last;
    }
    if (last) {
        press(s, last);
        frames(&f, 3);
    }
    bool added = named > 0 && shown == named && acount(f.scene->actors) == actors + 1;
    studio_history(s, false);
    frames(&f, 2);
    if (!(opened && hovered && stepped && closed && ran && added)) {
        printf("  %d %d %d %d %d %d (%d of %d)\n", opened, hovered, stepped, closed, ran, added, shown, named);
    }
    test_check(
        opened && hovered && stepped && closed,
        "the menubar opens a menu on press and marks its trigger, switches "
        "on hover and with Right, and Escape closes it"
    );
    test_check(
        ran && added,
        "a menubar MenuCommand runs its command, and Object > Add lists the "
        "scene's prototypes and adds the one chosen"
    );
    fixture_free(&f);
}

/* Closed menus mount nothing (workstream F3): every popover Studio's
   documents mark data-template holds no elements until it opens, builds
   them when it does and holds none again once it has faded out. */
static void menus_mount_when_open()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    /* A fade ends at once whatever the user's preference. */
    s->prefs.reduced_motion = true;
    frames(&f, 6);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *menus[64];
    rml_element *file = studio_element(s, "#menu-file");
    rml_element *bar = rml_element_owner_document(file);
    int count = test_query_all(context, "[popover][data-template]", menus, 64);
    int built = 0;
    for (int i = 0; i < count && i < 64; i++) {
        built += rml_element_child_count(menus[i]);
    }
    int closed = rml_element_query_selector_all(bar, "*", NULL, 0);
    press(s, menubar_trigger(s, "File"));
    frames(&f, 3);
    int opened = rml_element_query_selector_all(bar, "*", NULL, 0);
    bool listed = labeled(file, ".menu-item", "Save") != NULL;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 4);
    bool gone = rml_element_child_count(file) == 0 &&
                rml_element_query_selector_all(bar, "*", NULL, 0) == closed;
    if (!(count >= 8 && !built && opened > closed + 20 && listed && gone)) {
        printf("  %d menus, %d children, %d -> %d elements, %d %d\n", count, built, closed, opened, listed, gone);
    }
    test_check(count >= 8 && !built, "every menu in Studio's documents holds no element while closed");
    test_check(opened > closed + 20 && listed && gone, "a menu builds its items when it opens and removes them once closed");
    fixture_free(&f);
}

/* The play target, playback speed, layout and debugger steps (workstream
   C3): menus over the `shell` source and commands. */
static void transport_menus()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    frames(&f, 4);
    press(s, studio_element(s, "#play-target .target"));
    frames(&f, 3);
    rml_element *menu = studio_element(s, "#target-menu");
    rml_element *items[4];
    int count = menu ? rml_element_query_selector_all(menu, ".menu-item", items, 4)
                     : 0;
    char label[64] = "";
    rml_element_inner_rml(studio_element(s, "#play-target .target .label"), label, sizeof label);
    bool target = popover_open(menu) && count == 2 &&
                  rml_element_has_attribute(items[0], "data-checked") &&
                  !strncmp(label, "Scene", 5);
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);

    press(s, studio_element(s, "#speed .speed"));
    frames(&f, 3);
    rml_element *half = labeled(studio_element(s, "#speed-menu"), ".menu-item", "0.5\xc3\x97");
    press(s, half);
    frames(&f, 3);
    rml_element *speed = studio_element(s, "#speed .speed");
    char state[8] = "";
    rml_element_attribute(speed, "data-state", state, sizeof state);
    bool slowed = half && app_scene_authoring_time_scale(f.scene) == 0.5 &&
                  !strcmp(state, "on");
    app_scene_authoring_set_time_scale(f.scene, 1);

    studio_workspace before = s->prefs.workspace;
    press(s, studio_element(s, "#layout .btn"));
    frames(&f, 3);
    rml_element *layouts = studio_element(s, "#layout-menu");
    rml_element *checked = labeled(layouts, ".menu-item[data-checked]", "Vertical Split");
    rml_element *across = labeled(layouts, ".menu-item", "Horizontal Split");
    press(s, across);
    frames(&f, 3);
    bool laid_out = checked && across &&
                    s->prefs.workspace == STUDIO_WORKSPACE_HORIZONTAL_SPLIT;
    studio_set_workspace(s, before);
    s->maximized = -1;
    frames(&f, 2);

    if (!(target && slowed && laid_out)) {
        printf("  %d %d %d (%d items, label %s)\n", target, slowed, laid_out, count, label);
    }
    test_check(
        target && slowed && laid_out,
        "the play target's menu checks the scene, the speed's preset slows "
        "play and marks the button, and the layout menu switches modes"
    );
    fixture_free(&f);
}

/* The Workspace (workstream C3): areas over their places, a splitter that
   resizes its areas from its input in the same frame and lays out only
   their documents, and a drawer that eases open and shut. */
static void workspace_layout()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->drawer_open = false;
    layout->side_open = true;
    frames(&f, 30);
    rml_element *split = studio_element(s, "[data-ui=Workspace] .editors > .split");
    rml_element *top = studio_element(s, "#area-top");
    kit_rect area = s->areas[STUDIO_AREA_TOP].rect;
    rml_vec2 place = rml_element_box_size(top, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    bool placed = split && area.h > 0 && fabsf(area.h - place.y / ratio) < 1;
    float hit = split ? rml_element_box_size(split, RML_BOX_BORDER).y / ratio : 0;

    /* A drag by 40 dp. */
    rml_vec2 at = rml_element_absolute_offset(split, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(split, RML_BOX_BORDER);
    float x = (at.x + size.x / 2) / ratio, y = (at.y + size.y / 2) / ratio;
    rml_input input = { .type = RML_INPUT_POINTER_MOVE, .x = x, .y = y };
    rml_backend_process_input(s->backend, &input);
    input.type = RML_INPUT_POINTER_DOWN;
    rml_backend_process_input(s->backend, &input);
    float split_before = layout->main_split, height = place.y / ratio;
    rml_element *hit_element = rml_context_element_at_point(
        rml_backend_context(s->backend),
        (rml_vec2){ at.x + size.x / 2, at.y + 1 },
        NULL
    );
    bool followed = hit_element == split;
    for (int i = 1; i <= 4; i++) {
        input = (rml_input){ .type = RML_INPUT_POINTER_MOVE, .x = x, .y = y + 10 * i };
        rml_backend_process_input(s->backend, &input);
        frames(&f, 1);
        float now = rml_element_box_size(top, RML_BOX_BORDER).y / ratio;
        followed &= fabsf(now - (height + 10 * i)) <= 1;
    }
    followed &= layout->main_split > split_before;
    input.type = RML_INPUT_POINTER_UP;
    rml_backend_process_input(s->backend, &input);
    frames(&f, 2);
    layout->main_split = split_before;

    /* The drawer's height eases: it opens from nothing, and its area stays
       while it shuts. */
    rml_element *drawer = studio_element(s, "[data-ui=Workspace] .drawer");
    studio_command_run(s, STUDIO_COMMAND_TOGGLE_OUTPUT);
    float heights[20];
    for (int i = 0; i < 20; i++) {
        frames(&f, 1);
        heights[i] = rml_element_box_size(drawer, RML_BOX_BORDER).y / ratio;
    }
    float open = s->areas[STUDIO_AREA_DRAWER].rect.h;
    bool between = false;
    for (int i = 0; i < 20; i++) {
        between |= heights[i] > 1 && heights[i] < open;
    }
    studio_command_run(s, STUDIO_COMMAND_TOGGLE_OUTPUT);
    frames(&f, 1);
    bool closing = s->areas[STUDIO_AREA_DRAWER].rect.h > 0;
    frames(&f, 30);
    bool eased = between && open > 80 && heights[19] == open + 6 && closing &&
                 s->areas[STUDIO_AREA_DRAWER].rect.h == 0 &&
                 rml_element_box_size(drawer, RML_BOX_BORDER).y == 0;
    if (!(placed && hit >= 8 && followed && eased)) {
        printf("  %d %.1f %d %d (drawer %.1f)\n", placed, hit, followed, eased, open);
    }
    test_check(
        placed && hit >= 8,
        "each area lays out over its place in the Workspace, between "
        "splitters that take the pointer at least 8 dp wide"
    );
    test_check(followed, "a splitter's drag resizes its areas in the frame its input arrives");
    test_check(eased, "the drawer eases open and shut, then leaves its place");
    fixture_free(&f);
}

void studio_documents_shell_checks()
{
    const char *dir = getenv("CHIBI_SHELL_CAPTURE_DIR");
    if (dir && dir[0]) {
        capture_shell(dir);
        return;
    }
    studio_shell_cases(NULL, true);
}

bool studio_shell_cases(const char *only, bool run)
{
    bool matched = false;
    STUDIO_CASE("host_ui_refresh_keeps_input_immediate", host_ui_refresh_keeps_input_immediate);
    STUDIO_CASE("performance_snapshot_retains_intermediate_frames", performance_snapshot_retains_intermediate_frames);
    STUDIO_CASE("status_bar_follows_studio", status_bar_follows_studio);
    STUDIO_CASE("palette_finds_settings", palette_finds_settings);
    STUDIO_CASE("palette_recent_and_prefixes", palette_recent_and_prefixes);
    STUDIO_CASE("wheel_scrolls_lists", wheel_scrolls_lists);
    STUDIO_CASE("wheel_zooms_canvases", wheel_zooms_canvases);
    STUDIO_CASE("shortcut_repeat_policy", shortcut_repeat_policy);
    STUDIO_CASE("keys_follow_pointer", keys_follow_pointer);
    STUDIO_CASE("readout_follows_camera", readout_follows_camera);
    STUDIO_CASE("chrome_follows_studio", chrome_follows_studio);
    STUDIO_CASE("document-tabs", document_tabs);
    STUDIO_CASE("layout_modes_are_explicit", layout_modes_are_explicit);
    STUDIO_CASE("toolbar_shortcuts_keep_text_focus", toolbar_shortcuts_keep_text_focus);
    STUDIO_CASE("area_documents", area_documents);
    STUDIO_CASE("area_menus", area_menus);
    STUDIO_CASE("menubar_runs_commands", menubar_runs_commands);
    STUDIO_CASE("menus_mount_when_open", menus_mount_when_open);
    STUDIO_CASE("transport_menus", transport_menus);
    STUDIO_CASE("workspace_layout", workspace_layout);
    return matched;
}
