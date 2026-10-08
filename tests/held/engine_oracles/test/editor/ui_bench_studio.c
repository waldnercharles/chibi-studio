#include "authoring/engine_preview.h"
#include "graph/authoring/graph_edit.h"
/* The Studio cases of the S0 UI bench (ui_bench.h), native in a hidden
 * window: Studio's documents over the Sandbox at 1440x900, its
 * Properties showing the Player (authored since P1) against the S0 spike,
 * and whether a popover escapes an area's clipping. */
#include "editor/workspace/viewport.h"
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_thumbnails.h"
#include "editor/ui/kit.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/input.h"
#include "editor/workspace/scene_internal.h"
#include "graph/model/registry.h"
#include "render/scene_renderer.h"
#include "render/screenshot.h"
#include "support/text_edit.h"
#include "test/editor/ui_bench.h"
#include "test/suite_internal.h"
#include "ui/drag.h"
#include "ui/popover.h"
#include "ui/rml.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA "projects/arena2d"
#define WIDTH 1440
#define HEIGHT 900
#define FRAME (1.0 / 60.0)
#define RUNS 31
#define WARMUP 3
#define ARMS 2

typedef struct studio_bench
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    engine_viewport *viewport;
    app_scene_authoring *scene;
    studio *studio;
    double time;
} studio_bench;

/* Studio over the Sandbox, as studio_documents.c's fixture opens one. */
static studio_bench bench_open()
{
    studio_bench b = { .world = ecs_world_new() };
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        int registered = ecs_world_register_component(b.world, GAME_REGISTRY_COMPONENTS[i], NULL);
        assert(registered == ECS_OK);
    }
    b.live = game_preview_world_new(b.world);
    game_preview_definition *definition = game_preview_definition_new("Editing", behavior_asset_new("Editing"));
    int added = game_preview_add_definition(b.live, definition);
    assert(definition && added == 0);
    b.graph = editor_new(behavior_asset_new("Editing"));
    b.runtime = authoring_engine_new(b.world, b.live->runtime);
    b.viewport = engine_viewport_new(640, 360);
    b.scene = app_scene_authoring_new(
        b.world,
        b.live,
        definition,
        editor_behavior_asset(b.graph),
        b.graph,
        NULL,
        NULL,
        b.runtime
    );
    int loaded = app_scene_authoring_set_content_root(b.scene, ARENA "/content") |
                 app_scene_authoring_load_content(b.scene) |
                 app_scene_authoring_load(b.scene, ARENA "/content/scenes/Sandbox.kdl") |
                 app_scene_authoring_open_behavior(b.scene, "Movement");
    assert(loaded == 0);
    b.studio = studio_new(b.scene, b.viewport, ARENA);
    return b;
}

static void bench_close(studio_bench *b)
{
    studio_free(b->studio);
    app_scene_authoring_free(b->scene);
    engine_free(b->runtime);
    game_preview_world_free(b->live);
    editor_free(b->graph);
    ecs_world_free(b->world);
    engine_viewport_free(b->viewport);
}

/* RmlUi warnings one frame may log before the bench stops: a defect that
   repeats them every frame floods the log by gigabytes a minute. */
#define WARNING_FLOOD 100

static void stop_on_flood(int before)
{
    int logged = rml_backend_warning_count() - before;
    if (logged > WARNING_FLOOD) {
        fprintf(stderr, "ui bench: RmlUi logged %d warnings in one frame; stopping\n", logged);
        exit(1);
    }
}

static void frames(studio_bench *b, int count)
{
    for (int i = 0; i < count; i++) {
        int warnings = rml_backend_warning_count();
        b->time += FRAME;
        cf_app_update(NULL);
        app_input_frame();
        editor_tick(b->graph, b->time);
        bool ran = app_scene_authoring_frame(b->scene, b->time) == FLOW_OK;
        assert(ran);
        studio_frame(b->studio, FRAME);
        cf_app_draw_onto_screen(true);
        b->studio->prefs_changed_at = 0;
        stop_on_flood(warnings);
    }
}

static rml_element *studio_query(studio *s, const char *selector)
{
    rml_context *context = rml_backend_context(s->backend);
    rml_element *found = NULL;
    for (int i = 0; !found && i < rml_context_document_count(context); i++) {
        found = rml_element_query_selector(rml_context_document(context, i), selector);
    }
    return found;
}

static int studio_count(studio *s, const char *selector)
{
    rml_context *context = rml_backend_context(s->backend);
    int count = 0;
    for (int i = 0; i < rml_context_document_count(context); i++) {
        count += rml_element_query_selector_all(rml_context_document(context, i), selector, NULL, 0);
    }
    return count;
}

/* ---------- Native UI work ---------- */

typedef enum
{
    PHASE_SYNC,   /* authored panels' data (studio_ui_frame) */
    PHASE_EMIT,   /* the kit re-emits the editor */
    PHASE_LAYOUT, /* RmlUi's update: data views, styles, layout */
    PHASE_CAPTURE,
    PHASE_COUNT,
} phase;

/* One frame's UI work, as studio_host.c's run does it; returns the total
 * and fills each phase's time. */
static double native_ui(studio *s, double *phases)
{
    int warnings = rml_backend_warning_count();
    double t[PHASE_COUNT + 1];
    t[0] = bench_now();
    studio_ui_frame(s);
    t[1] = bench_now();
    kit_begin(s->kit, &s->in);
    if (s->canvas) {
        s->canvas->frame = NULL;
    }
    studio_emit(s);
    studio_inspect_emit(s);
    kit_end(s->kit);
    t[2] = bench_now();
    rml_backend_update(s->backend, 0);
    t[3] = bench_now();
    rml_backend_render(s->backend);
    t[4] = bench_now();
    for (int p = 0; p < PHASE_COUNT; p++) {
        phases[p] = t[p + 1] - t[p];
    }
    stop_on_flood(warnings);
    return t[PHASE_COUNT] - t[0];
}

/* Layouts, and what the authored panels' data views read. */
static bench_counts studio_mark(studio *s)
{
    rml_data_reads reads = studio_ui_data_reads(s);
    return (bench_counts){ .values = reads.values,
                           .lookups = reads.lookups,
                           .layouts = bench_layouts() };
}

static bench_counts studio_counts(studio *s, bench_counts before)
{
    bench_counts now = studio_mark(s);
    return (bench_counts){ .values = now.values - before.values,
                           .lookups = now.lookups - before.lookups,
                           .layouts = now.layouts - before.layouts,
                           .elements = studio_count(s, "*") };
}

static void phase_row(const char *name, double phases[PHASE_COUNT][RUNS])
{
    bench_stat stat[PHASE_COUNT];
    for (int p = 0; p < PHASE_COUNT; p++) {
        stat[p] = bench_stat_of(phases[p], RUNS);
    }
    printf(
        "|  | %s: sync %.3f, emit %.3f, update %.3f, capture %.3f | | | | | | "
        "|\n",
        name,
        stat[PHASE_SYNC].median,
        stat[PHASE_EMIT].median,
        stat[PHASE_LAYOUT].median,
        stat[PHASE_CAPTURE].median
    );
}

/* ---------- 1: full layout ---------- */

static void probe_documents(studio *s)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        bench_probe(rml_context_document(context, i));
    }
}

/* Lays the whole document out by resizing it a pixel, as a window resize
 * or any layout change does. */
static void full_layout(studio_bench *b, bool comfortable)
{
    studio *s = b->studio;
    s->prefs.comfortable = comfortable;
    frames(b, 4);
    float scale = cf_app_get_pixel_scale(), ratio = studio_pixel_ratio(s);
    int width = (int)(WIDTH * scale), height = (int)(HEIGHT * scale);
    double ms[RUNS];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        rml_backend_resize_pixels(s->backend, width + (run & 1), height, ratio);
        bench_counts before = studio_mark(s);
        double t = bench_now();
        rml_backend_update(s->backend, 0);
        t = bench_now() - t;
        counts = studio_counts(s, before);
        if (run >= 0) {
            ms[run] = t;
        }
    }
    rml_backend_resize_pixels(s->backend, width, height, ratio);
    frames(b, 2);
    char name[128];
    snprintf(
        name,
        sizeof name,
        "full layout of Studio, %s, %d documents",
        comfortable ? "comfortable" : "compact",
        rml_context_document_count(rml_backend_context(s->backend))
    );
    bench_row("1", name, bench_stat_of(ms, RUNS), counts);
}

/* ---------- 13: a text change inside one area ---------- */

/* Rewrites one Outliner row's name, as a rename does, and updates: RmlUi
 * lays out the document that holds the row. Counts the documents laid out
 * and the elements in the row's document. */
static void area_text_change(studio_bench *b)
{
    studio *s = b->studio;
    frames(b, 2);
    rml_element *name = studio_query(s, "#outliner-list .tree-row .name");
    assert(name);
    char text[256];
    size_t length = rml_element_inner_rml(name, text, sizeof text);
    assert(length < sizeof text);
    double ms[RUNS];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        rml_element_set_inner_rml(name, run & 1 ? "Renamed" : "Renamed again");
        bench_counts before = studio_mark(s);
        double t = bench_now();
        rml_backend_update(s->backend, 0);
        t = bench_now() - t;
        counts = (bench_counts){
            .layouts = bench_layouts() - before.layouts,
            .elements = rml_element_query_selector_all(rml_element_owner_document(name), "*", NULL, 0)
        };
        if (run >= 0) {
            ms[run] = t;
        }
    }
    rml_element_set_inner_rml(name, text);
    frames(b, 2);
    bench_row(
        "13",
        "a text change in the Outliner: documents laid out, elements in its "
        "document",
        bench_stat_of(ms, RUNS),
        counts
    );
}

/* ---------- 5: Properties, native and authored ---------- */

static int actor_named(const app_scene_authoring *scene, const char *name)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        if (scene->actors[i].name && !strcmp(scene->actors[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* Another object with components of its own to show: a Swarmer when the
 * scene places one. */
static int other_actor(const app_scene_authoring *scene, int player)
{
    for (int i = 0; i < acount(scene->actors); i++) {
        const char *prototype = scene->actors[i].prototype_name;
        if (i != player && prototype && !strcmp(prototype, "Swarmer")) {
            return i;
        }
    }
    return player ? 0 : 1;
}

/* Opens every component panel, as a user expanding them does. */
static void open_panels(studio_bench *b)
{
    rml_element *panels[64];
    rml_element *area = studio_query(b->studio, "[data-ui=Properties]");
    int count = rml_element_query_selector_all(area, "section.panel", panels, 64);
    for (int i = 0; i < count; i++) {
        rml_element *fold = rml_element_query_selector(panels[i], ".panel-fold");
        /* data-for keeps its template hidden and empty. */
        if (fold && rml_element_is_visible(panels[i], true) &&
            !rml_element_has_attribute(panels[i], "data-open")) {
            rml_element_click(fold);
            frames(b, 1);
        }
    }
    frames(b, 2);
}

typedef struct properties_case
{
    studio_bench *b;
    bench_spike *spike;
    int player, other;
} properties_case;

static void select_actor(properties_case *c, int actor, bool authored)
{
    app_scene_authoring *scene = c->b->scene;
    if (authored) {
        bench_spike_show(
            c->spike,
            scene->world,
            scene->actors[actor].entity,
            scene->actors[actor].name,
            &c->b->studio->presentation.hints
        );
        return;
    }
    select_instance(scene, actor);
}

/* The spike's UI work after its data changed: RmlUi's update, then the
 * capture; fills those phases. */
static void authored_ui(bench_spike *spike, double *phases)
{
    double t[3] = { bench_now() };
    rml_backend_update(bench_spike_backend(spike), 0);
    t[1] = bench_now();
    rml_backend_render(bench_spike_backend(spike));
    t[2] = bench_now();
    phases[PHASE_EMIT] = 0;
    phases[PHASE_LAYOUT] = t[1] - t[0];
    phases[PHASE_CAPTURE] = t[2] - t[1];
}

/* Arm 0's (native) or arm 1's (authored) UI work for `step`: 0 selects
 * `actor`, 1 and 2 show values changed. Returns its time and fills its
 * phases; selecting and the spike's refresh count as sync. */
static double properties_ui(properties_case *c, int step, int arm, int actor, double *phases)
{
    double start = bench_now();
    if (step == 0) {
        select_actor(c, actor, arm);
    } else if (arm) {
        bench_spike_refresh(c->spike);
    }
    double synced = bench_now() - start;
    if (arm) {
        authored_ui(c->spike, phases);
        phases[PHASE_SYNC] = synced;
    } else {
        native_ui(c->b->studio, phases);
        phases[PHASE_SYNC] += synced;
    }
    return bench_now() - start;
}

/* Step 1 edits one shown field and step 2 moves every float, as Play does.
 * Values only grow: a frame between two changes need not refresh, so going
 * back and forth would show the same value to every other one. */
static void change_player(properties_case *c, int step)
{
    app_scene_authoring *scene = c->b->scene;
    ecs_entity player = scene->actors[c->player].entity;
    if (step == 1) {
        bench_edit_float(scene->world, player, "GameAppearance", "width", 1);
    } else if (step == 2) {
        bench_move_floats(scene->world, player, 0.25f);
    }
}

/* Times arm 0 (native) against arm 1 (authored) for `step`, each in the
 * same run after the same change. */
static void properties_step(properties_case *c, int step, const char *name)
{
    double ms[ARMS][RUNS], phases[ARMS][PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts[ARMS];
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            change_player(c, step);
            cf_app_update(NULL);
            bench_counts before = arm ? bench_spike_counts(c->spike)
                                      : studio_mark(c->b->studio);
            double t = properties_ui(c, step, arm, run & 1 ? c->other : c->player, phase);
            cf_app_draw_onto_screen(true);
            bench_counts after = arm ? bench_spike_counts(c->spike)
                                     : studio_counts(c->b->studio, before);
            counts[arm] = arm ? (bench_counts){ after.values - before.values,
                                                after.lookups - before.lookups,
                                                after.layouts - before.layouts,
                                                after.elements }
                              : after;
            frames(c->b, 1);
            if (run < 0) {
                continue;
            }
            ms[arm][run] = t;
            for (int p = 0; p < PHASE_COUNT; p++) {
                phases[arm][p][run] = phase[p];
            }
        }
    }
    char label[128];
    bench_stat native = bench_stat_of(ms[0], RUNS),
               authored = bench_stat_of(ms[1], RUNS);
    snprintf(label, sizeof label, "Studio: %s", name);
    bench_row("5", label, native, counts[0]);
    phase_row("Studio phases", phases[0]);
    snprintf(label, sizeof label, "authored spike: %s", name);
    bench_row("5", label, authored, counts[1]);
    phase_row("authored phases", phases[1]);
    snprintf(label, sizeof label, "spike over Studio: %s", name);
    bench_ratio("5", label, native, authored);
    /* The Player stays shown. */
    select_actor(c, c->player, false);
    select_actor(c, c->player, true);
    frames(c->b, 2);
}

static void properties(studio_bench *b)
{
    studio *s = b->studio;
    app_scene_authoring *scene = b->scene;
    properties_case c = { .b = b, .player = actor_named(scene, "Player") };
    assert(c.player >= 0);
    c.other = other_actor(scene, c.player);
    select_instance(scene, c.player);
    studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
    s->tab = STUDIO_TAB_OBJECT;
    frames(b, 4);
    open_panels(b);
    rml_element *area = studio_query(s, "[data-ui=Properties]");
    assert(area);
    rml_vec2 size = rml_element_box_size(area, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    c.spike = bench_spike_new((int)(size.x / ratio), (int)(size.y / ratio));
    rml_backend_resize(bench_spike_backend(c.spike), (int)(size.x / ratio), (int)(size.y / ratio), ratio);
    select_actor(&c, c.player, true);
    /* Captures retain unchanged paint, as Studio's do. */
    cf_app_update(NULL);
    rml_backend_frame_retained(bench_spike_backend(c.spike), 0, true);
    cf_app_draw_onto_screen(true);
    printf(
        "|  | Properties area %.0fx%.0f dp; native shows %d rows, the spike "
        "%d; the other "
        "object is %s | | | | | | |\n",
        size.x / ratio,
        size.y / ratio,
        studio_count(s, "[data-ui=Properties] .prop"),
        bench_spike_rows(c.spike),
        scene->actors[c.other].name
    );
    double idle[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        frames(b, 1);
        if (run >= 0) {
            idle[run] = t;
            for (int p = 0; p < PHASE_COUNT; p++) {
                phases[p][run] = phase[p];
            }
        }
    }
    bench_row("5", "Studio: a UI frame with nothing changed", bench_stat_of(idle, RUNS), counts);
    phase_row("Studio phases", phases);
    properties_step(&c, 0, "select the other object or the Player");
    properties_step(&c, 1, "edit one field of the Player");
    properties_step(&c, 2, "Play refresh, every float of the Player changed");
    bench_spike_free(c.spike);
}

/* ---------- 14: Assets over 500 assets ---------- */

#define ASSETS_ADDED 480
/* Where the scroll cases scroll from, in lines or rows, past the overscan
   that keeps the window still for small scrolls near the top. */
#define ASSETS_SCROLLED 10
/* Opening takes this many UI frames to show every tile in view. */
#define OPEN_FRAMES 3

typedef enum
{
    ASSETS_OPEN,   /* show Assets in an area that showed the Outliner */
    ASSETS_SCROLL, /* scroll the grid a line */
    ASSETS_FILTER, /* type in the filter */
    ASSETS_SELECT, /* select another tile */
    ASSETS_IDLE,   /* a UI frame with nothing changed */
    ASSETS_LIST,   /* scroll the list a row */
} assets_step;

/* The Assets view's scroller. */
static rml_element *assets_scroller(studio *s)
{
    return studio_query(s, "#assets-list");
}

/* Scrolls the view to `lines` of `height` dp, and half a line more for an
   odd run. */
static void assets_scroll(studio *s, float height, int run)
{
    float lines = ASSETS_SCROLLED + (run & 1 ? 1.0f : 0.0f);
    rml_element_set_scroll_top(assets_scroller(s), lines * height * studio_pixel_ratio(s));
}

static void assets_change(studio_bench *b, assets_step step, int run)
{
    studio *s = b->studio;
    switch (step) {
        case ASSETS_OPEN:
            studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_OUTLINER;
            frames(b, 2);
            studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
            break;
        case ASSETS_SCROLL:
            assets_scroll(s, (float)s->prefs.assets_tile + 2 * 4 + 18 + 4, run);
            break;
        case ASSETS_FILTER:
            snprintf(s->assets_filter, sizeof s->assets_filter, "%s", run & 1 ? "QaBench1" : "QaBench");
            break;
        case ASSETS_SELECT:
            s->assets_selected_kind = REFERENCE_ASSET_BEHAVIOR;
            snprintf(s->assets_selected, sizeof s->assets_selected, "%s", run & 1 ? "QaBench001" : "QaBench003");
            break;
        case ASSETS_LIST: assets_scroll(s, 26, run); break;
        case ASSETS_IDLE: break;
    }
}

/* Times one step's UI work: three UI frames to open (a mount, then its
   window once the List has a width), one otherwise; counts the views
   evaluated, the documents laid out and the elements in the area's
   document. */
static void assets_case(studio_bench *b, assets_step step, const char *name)
{
    studio *s = b->studio;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT], more[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        assets_change(b, step, run);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        for (int f = 1; step == ASSETS_OPEN && f < OPEN_FRAMES; f++) {
            t += native_ui(s, more);
            for (int p = 0; p < PHASE_COUNT; p++) {
                phase[p] += more[p];
            }
        }
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        rml_element *document = rml_element_owner_document(assets_scroller(s));
        counts.elements = rml_element_query_selector_all(document, "*", NULL, 0);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    bench_row("14", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* Assets maximized over the Sandbox's assets and 480 more, which have no
   files, so their tiles keep their kinds' icons: open, scroll and filter
   the grid, an idle frame, and a scroll of the list. */
static void assets(studio_bench *b)
{
    studio *s = b->studio;
    for (int i = 0; i < ASSETS_ADDED; i++) {
        char name[32];
        snprintf(name, sizeof name, "QaBench%03d", i);
        reference_asset_kind kind = i & 1 ? REFERENCE_ASSET_BEHAVIOR
                                          : REFERENCE_ASSET_MATERIAL;
        reference_index_add_asset(b->scene->asset_index, kind, name, NULL, name);
    }
    /* Inserted directly: start the cached model afresh, as a rebuild would. */
    studio_assets_free(s);
    s->maximized = STUDIO_AREA_TOP;
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
    s->prefs.assets_tile = STUDIO_ASSETS_TILE;
    frames(b, 4);
    for (int i = 0; i < 600 && studio_thumbs_pending(s); i++) {
        frames(b, 1);
    }
    /* Maximized, the area is a document of its own. */
    bench_probe(rml_element_owner_document(assets_scroller(s)));
    printf(
        "|  | Assets: %d assets, %d tiles shown | | | | | | |\n",
        reference_index_asset_count(b->scene->asset_index),
        studio_count(s, "#assets-list [data-ui=Tile]")
    );
    assets_case(b, ASSETS_OPEN, "Assets grid: open, 3 UI frames");
    assets_case(b, ASSETS_SCROLL, "Assets grid: scroll a line");
    assets_case(b, ASSETS_FILTER, "Assets grid: filter keystroke");
    /* The first QaBench behaviors in view. */
    rml_element_set_scroll_top(assets_scroller(s), 0);
    frames(b, 2);
    assets_case(b, ASSETS_SELECT, "Assets grid: select another tile");
    assets_case(b, ASSETS_IDLE, "Assets grid: UI frame, nothing changed");
    s->prefs.assets_tile = STUDIO_ASSETS_LIST;
    frames(b, 4);
    assets_case(b, ASSETS_LIST, "Assets list: scroll a row");
    s->prefs.assets_tile = STUDIO_ASSETS_TILE;
    s->maximized = -1;
    frames(b, 2);
}

/* ---------- 11: Output following a log ---------- */

/* Gives documents opened since the bench began their layout probes. */
static void probe_new_documents(studio *s)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *document = rml_context_document(context, i);
        if (!rml_element_query_selector(document, "bench-probe")) {
            bench_probe(document);
        }
    }
}

/* Prints `count` lines, as a behavior printing every frame does. */
static void print_lines(editor *graph, int count)
{
    static int printed;
    for (int i = 0; i < count; i++, printed++) {
        char text[192];
        snprintf(
            text,
            sizeof text,
            "warning: Player hit \"Wall %d\" at (%d.5, -%d.25) for %d damage "
            "in content/behaviors/Weapon.chibi",
            printed % 97,
            printed,
            printed % 13,
            printed % 9
        );
        editor_log_print(graph, "Weapon", text);
    }
}

/* One UI frame after `lines` new lines with Output's Log following them:
   its document's layouts and elements, and the views Studio's authored
   panels evaluated. */
static void log_step(studio_bench *b, int lines, bool full, rml_element *document)
{
    studio *s = b->studio;
    editor_clear_log(b->graph);
    print_lines(b->graph, full ? ED_LOG_MAX : 200);
    frames(b, 2);
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        print_lines(b->graph, lines);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        counts.elements = rml_element_query_selector_all(document, "*", NULL, 0);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    char name[128];
    snprintf(
        name,
        sizeof name,
        "Output's Log following %d new line%s a frame, the log %s",
        lines,
        lines == 1 ? "" : "s",
        full ? "full (the oldest drop)" : "growing"
    );
    bench_row("11", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

static void output_log(studio_bench *b)
{
    studio *s = b->studio;
    studio_show_editor(s, STUDIO_EDITOR_OUTPUT);
    s->output_view = 0;
    frames(b, 4);
    probe_new_documents(s);
    rml_element *list = studio_query(s, ".output-list");
    assert(list);
    rml_element *document = rml_element_owner_document(list);
    log_step(b, 1, false, document);
    log_step(b, 8, false, document);
    log_step(b, 1, true, document);
    log_step(b, 8, true, document);
    editor_clear_log(b->graph);
    frames(b, 2);
}

/* ---------- 15: dragging a splitter (workstream C3) ---------- */

/* One UI frame of the chrome at rest, or after a selection change, which
   the top bar's play target and the menus' commands follow. */
static void chrome_step(studio_bench *b, bool select, const char *name)
{
    studio *s = b->studio;
    app_scene_authoring *scene = b->scene;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        if (select) {
            select_instance(scene, run & 1 ? 0 : acount(scene->actors) - 1);
        }
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    bench_row("15", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* The splitter between the top and bottom areas: the kit's in the shell
   document, or the Workspace's. */
static rml_element *main_splitter(studio *s)
{
    return studio_query(s, ".main-editors > splitter, .editors > splitter");
}

static void pointer(studio *s, rml_input_type type, float x, float y)
{
    rml_backend_process_input(s->backend, &(rml_input){ .type = type, .x = x, .y = y });
}

/* The two UI frames after the pointer dragged the main splitter 3 dp, as
   the kit's splitter resized its areas in the frame after the drag and the
   Workspace's does in the same one: their time, the documents laid out and
   the views Studio's authored panels evaluated. */
static void splitter_drag(studio_bench *b)
{
    studio *s = b->studio;
    s->maximized = -1;
    frames(b, 4);
    probe_new_documents(s);
    rml_element *split = main_splitter(s);
    assert(split);
    float ratio = studio_pixel_ratio(s);
    rml_vec2 at = rml_element_absolute_offset(split, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(split, RML_BOX_BORDER);
    float x = (at.x + size.x / 2) / ratio, y = (at.y + size.y / 2) / ratio;
    pointer(s, RML_INPUT_POINTER_MOVE, x, y);
    pointer(s, RML_INPUT_POINTER_DOWN, x, y);
    for (int i = 1; i <= 4; i++) {
        pointer(s, RML_INPUT_POINTER_MOVE, x, y + i);
        frames(b, 1);
    }
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        cf_app_update(NULL);
        pointer(s, RML_INPUT_POINTER_MOVE, x, y + (run & 1 ? 1 : 4));
        bench_counts before = studio_mark(s);
        double t = 0, sum[PHASE_COUNT] = { 0 };
        for (int frame = 0; frame < 2; frame++) {
            if (frame) {
                cf_app_update(NULL);
            }
            t += native_ui(s, phase);
            cf_app_draw_onto_screen(true);
            for (int p = 0; p < PHASE_COUNT; p++) {
                sum[p] += phase[p];
            }
        }
        counts = studio_counts(s, before);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = sum[p];
        }
    }
    pointer(s, RML_INPUT_POINTER_UP, x, y);
    frames(b, 2);
    bench_row(
        "15",
        "dragging the main splitter 3 dp, the two UI frames after: documents "
        "laid out",
        bench_stat_of(ms, RUNS),
        counts
    );
    phase_row("phases", phases);
    chrome_step(b, false, "the chrome: a UI frame with nothing changed");
    chrome_step(b, true, "the chrome: select another object");
}

/* ---------- 17: Performance while the scene runs ---------- */

/* The UI frame after Performance takes a new sample, as each 20 Hz
   publication in Play does: the scene steps a frame, the sample publishes,
   and Studio's UI updates. Counts the layouts of every document and the
   elements in Performance's. */
static void performance_step(studio_bench *b, bool sampled, rml_element *document)
{
    studio *s = b->studio;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    uint64_t layouts = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        b->time += FRAME;
        cf_app_update(NULL);
        app_input_frame();
        editor_tick(b->graph, b->time);
        bool ran = app_scene_authoring_frame(b->scene, b->time) == FLOW_OK;
        assert(ran);
        s->time += FRAME;
        if (sampled) {
            s->perf_publish_next = 0;
            s->perf_readout_time = 0;
        }
        studio_performance_sample(s, FRAME);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        counts.elements = rml_element_query_selector_all(document, "*", NULL, 0);
        if (run < 0) {
            continue;
        }
        layouts += counts.layouts;
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    char name[160];
    snprintf(
        name,
        sizeof name,
        "Performance in Play, %s (%llu layouts in %d runs)",
        sampled ? "a new sample and readout" : "no new sample",
        (unsigned long long)layouts,
        RUNS
    );
    bench_row("17", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

static void performance_play(studio_bench *b)
{
    studio *s = b->studio;
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_PERFORMANCE;
    studio_runtime(s, APP_RUNTIME_PLAY);
    frames(b, 90);
    probe_new_documents(s);
    rml_element *chart = studio_query(s, ".perf-chart");
    assert(chart);
    rml_element *document = rml_element_owner_document(chart);
    performance_step(b, true, document);
    performance_step(b, false, document);
    studio_runtime(s, APP_RUNTIME_STOP);
    frames(b, 2);
}

/* ---------- C2's native shots ---------- */

/* A press and release on `el`, as a click (studio_documents.c's press). */
static void press_element(studio *s, rml_element *el)
{
    assert(el);
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    rml_input input = { .x = (at.x + size.x * 0.5f) / ratio,
                        .y = (at.y + size.y * 0.5f) / ratio };
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        input.type = types[i];
        rml_backend_process_input(s->backend, &input);
    }
}

static void shot(studio_bench *b, const char *dir, const char *variant, const char *state)
{
    char path[1024];
    frames(b, 10);
    snprintf(path, sizeof path, "%s/native-%s-%s.png", dir, state, variant);
    int saved = app_save_screenshot(path);
    assert(saved == 0);
}

/* ---------- 19: opening a menu (workstream F3) ---------- */

#define MENU_FRAMES_MAX 8

/* Whether the menu has been placed and shows, which is its first paint. */
static bool menu_painted(rml_element *menu)
{
    return popover_open(menu) && !rml_element_has_attribute(menu, "data-unplaced");
}

/* The top bar's File menu opened by a press on its trigger, then closed:
   the time from the press to the UI frame that first paints the menu (the
   press's own work, which builds the menu's items, and those frames), the
   frames that took, the views Studio's authored panels evaluated and the
   elements in the top bar's document open (the line after the row gives
   them closed). */
static void menu_open(studio_bench *b)
{
    studio *s = b->studio;
    s->maximized = -1;
    /* Fades end at once, so each run starts with the menu closed. */
    s->prefs.reduced_motion = true;
    frames(b, 4);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *trigger = studio_query(s, "[popovertarget=menu-file]");
    rml_element *menu = studio_query(s, "#menu-file");
    assert(trigger && menu);
    rml_element *document = rml_element_owner_document(menu);
    probe_documents(s);
    int closed = rml_element_query_selector_all(document, "*", NULL, 0);
    double ms[RUNS];
    bench_counts counts = { 0 };
    int painted_in = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        popover_hide_all(context);
        frames(b, 3);
        bench_counts before = studio_mark(s);
        double t = bench_now(), phase[PHASE_COUNT];
        press_element(s, trigger);
        t = bench_now() - t;
        int frame = 0;
        do {
            cf_app_update(NULL);
            t += native_ui(s, phase);
            cf_app_draw_onto_screen(true);
            frame++;
        } while (frame < MENU_FRAMES_MAX && !menu_painted(menu));
        counts = studio_counts(s, before);
        counts.elements = rml_element_query_selector_all(document, "*", NULL, 0);
        painted_in = frame;
        if (run >= 0) {
            ms[run] = t;
        }
    }
    popover_hide_all(context);
    frames(b, 3);
    bench_row("19", "opening the File menu: press to first paint", bench_stat_of(ms, RUNS), counts);
    printf(
        "|  | first painted in %d UI frame(s); the top bar's document holds %d "
        "elements closed | | | | | | |\n",
        painted_in,
        closed
    );
}

/* With UI_BENCH_CAPTURE set to a directory, native shots of the Behavior
   header, its graph picker and Graph menu, the graph's find bar and
   Performance in Play, in dark and light, compact and comfortable, with
   reduced motion, which the kit build and the authored one both take. */
static void capture_chrome(studio_bench *b, const char *dir)
{
    static const struct
    {
        const char *name;
        studio_theme theme;
        bool comfortable;
    } VARIANTS[] = { { "dark-compact", STUDIO_THEME_DARK, false },
                     { "dark-comfortable", STUDIO_THEME_DARK, true },
                     { "light-compact", STUDIO_THEME_LIGHT, false },
                     { "light-comfortable", STUDIO_THEME_LIGHT, true } };
    studio *s = b->studio;
    studio_layout *layout = studio_active_layout(s);
    s->maximized = -1;
    /* Fades settle at once, so a shot never catches one midway. */
    s->prefs.reduced_motion = true;
    for (size_t v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++) {
        const char *name = VARIANTS[v].name;
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        layout->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_BEHAVIOR;
        rml_context *context = rml_backend_context(s->backend);
        shot(b, dir, name, "header");
        press_element(s, studio_query(s, ".crumb.current"));
        shot(b, dir, name, "graphs");
        popover_hide_all(context);
        frames(b, 10);
        press_element(s, studio_query(s, "[data-tip=Graph]"));
        shot(b, dir, name, "more");
        popover_hide_all(context);
        frames(b, 10);
        snprintf(s->graph.find.query, sizeof s->graph.find.query, "speed");
        studio_graph_find(s);
        shot(b, dir, name, "find");
        s->graph.find.open = false;
        layout->editors[STUDIO_AREA_BOTTOM] = STUDIO_EDITOR_PERFORMANCE;
        studio_runtime(s, APP_RUNTIME_PLAY);
        frames(b, 90);
        shot(b, dir, name, "perfplay");
        studio_runtime(s, APP_RUNTIME_STOP);
        frames(b, 4);
    }
}

/* ---------- 12: the Outliner over ten thousand objects ---------- */

#define OUTLINER_OBJECTS 10000
#define SCROLL_ROWS 3
/* Rows a document query collects at most; a window holds fewer. */
#define MAX_ROWS 256

/* The Outliner's scroller, the authored List (workstream P2). Before it,
   the kit's list was ".vlist.outliner". */
static rml_element *outliner_list(studio *s)
{
    return studio_query(s, "#outliner-list");
}

/* The rows shown (templates left out) that lack the mark, which a step
   instanced; with `mark`, marks them all. `shown` counts them. */
static int fresh_rows(rml_element *list, bool mark, int *shown)
{
    rml_element *rows[MAX_ROWS];
    int count = rml_element_query_selector_all(list, ".tree-row", rows, MAX_ROWS);
    assert(count < MAX_ROWS);
    int fresh = 0;
    *shown = 0;
    for (int i = 0; i < count; i++) {
        if (!rml_element_is_visible(rows[i], false)) {
            continue;
        }
        ++*shown;
        fresh += !rml_element_has_attribute(rows[i], "data-bench");
        if (mark) {
            rml_element_set_attribute(rows[i], "data-bench", "");
        }
    }
    return fresh;
}

/* What a run did: data views read, layouts, and elements in the
   Outliner's document. */
static bench_counts outliner_counts(studio *s, rml_element *list, bench_counts before)
{
    bench_counts now = studio_mark(s);
    return (bench_counts){
        .values = now.values - before.values,
        .lookups = now.lookups - before.lookups,
        .layouts = now.layouts - before.layouts,
        .elements = rml_element_query_selector_all(rml_element_owner_document(list), "*", NULL, 0)
    };
}

/* Scrolls the Outliner three rows down per run, from the middle of ten
   thousand objects, and times the UI frame that follows: rows instanced,
   layouts and elements in the Outliner's document; then selects one of two
   objects in view in turn. */
static void outliner_scroll(studio_bench *b)
{
    studio *s = b->studio;
    for (int i = 0; i < OUTLINER_OBJECTS; i++) {
        char name[32];
        snprintf(name, sizeof name, "Marker %d", i);
        int added = add_actor(b->scene, name, "DriftingMarker");
        assert(added == 0);
    }
    select_instance(b->scene, 0);
    studio_show_editor(s, STUDIO_EDITOR_OUTLINER);
    frames(b, 6);
    rml_element *list = outliner_list(s);
    assert(list);
    probe_documents(s);
    char value[32] = "";
    rml_element_property(list, "--row", value, sizeof value);
    float row = (float)atof(value) * studio_pixel_ratio(s);
    assert(row > 0);
    float top = row * OUTLINER_OBJECTS / 2;
    rml_element_set_scroll_top(list, top);
    frames(b, 4);
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    int instanced = 0, shown = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        fresh_rows(list, true, &shown);
        top += SCROLL_ROWS * row;
        rml_element_set_scroll_top(list, top);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = outliner_counts(s, list, before);
        instanced = fresh_rows(list, false, &shown);
        if (run >= 0) {
            ms[run] = t;
            for (int p = 0; p < PHASE_COUNT; p++) {
                phases[p][run] = phase[p];
            }
        }
    }
    char name[160];
    snprintf(
        name,
        sizeof name,
        "Outliner over %d objects, %.0f dp tall, a %d-row scroll step: %d rows "
        "shown, %d instanced",
        acount(b->scene->actors),
        rml_element_client_height(list) / studio_pixel_ratio(s),
        SCROLL_ROWS,
        shown,
        instanced
    );
    bench_row("12", name, bench_stat_of(ms, RUNS), counts);
    phase_row("its phases", phases);
    /* Two objects in view, selected in turn, as clicks on their rows. */
    int first = (int)(top / row);
    for (int run = -WARMUP; run < RUNS; run++) {
        select_instance(b->scene, first + 4 + (run & 1));
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = outliner_counts(s, list, before);
        if (run >= 0) {
            ms[run] = t;
            for (int p = 0; p < PHASE_COUNT; p++) {
                phases[p][run] = phase[p];
            }
        }
    }
    bench_row(
        "12",
        "Outliner over 10k objects: select another object in view",
        bench_stat_of(ms, RUNS),
        counts
    );
    phase_row("its phases", phases);
}

/* ---------- 23: the Outliner's interactions, an A/B of builds ---------- */

typedef enum
{
    OUTLINER_IDLE,
    OUTLINER_SCROLL,
    OUTLINER_CLICK,
    OUTLINER_FILTER,
    OUTLINER_RENAME,
} outliner_step;

/* Object `actor`'s row in the Outliner: data-object since its behaviors
   (the spike), data-id before. */
static rml_element *outliner_row(studio *s, int actor)
{
    char selector[96];
    unsigned long long key = ecs_entity_to_u64(s->scene->actors[actor].entity);
    snprintf(selector, sizeof selector, "#outliner-list [data-object=\"%llu\"]", key);
    rml_element *row = studio_query(s, selector);
    snprintf(selector, sizeof selector, "#outliner-list [data-id=\"%llu\"]", key);
    return row ? row : studio_query(s, selector);
}

static void key_press(studio *s, rml_input_key key)
{
    rml_input input = { .type = RML_INPUT_KEY_PRESS, .key = key };
    rml_backend_process_input(s->backend, &input);
    input.type = RML_INPUT_KEY_RELEASE;
    rml_backend_process_input(s->backend, &input);
}

static void type_text(studio *s, const char *text)
{
    rml_input input = { .type = RML_INPUT_TEXT_INPUT,
                        .text = text,
                        .text_size = strlen(text) };
    rml_backend_process_input(s->backend, &input);
}

/* What one run changes before its UI frames: nothing, a scroll of three
   rows, a click on one of two rows in view, a keystroke in the filter
   ("Marker 5" and "Marker 51" in turn), or a rename typed in place. */
static void outliner_change(studio_bench *b, outliner_step step, int run, float row, int middle)
{
    studio *s = b->studio;
    rml_element *list = outliner_list(s);
    switch (step) {
        case OUTLINER_IDLE: break;
        case OUTLINER_SCROLL:
            rml_element_set_scroll_top(list, rml_element_scroll_top(list) + SCROLL_ROWS * row);
            break;
        case OUTLINER_CLICK:
            press_element(s, outliner_row(s, middle + 4 + (run & 1)));
            break;
        case OUTLINER_FILTER:
            if (run & 1) {
                key_press(s, RML_INPUT_KEY_BACKSPACE);
            } else {
                type_text(s, "1");
            }
            break;
        case OUTLINER_RENAME: {
            char name[32];
            snprintf(name, sizeof name, "Renamed %d", run + WARMUP);
            type_text(s, name);
            key_press(s, RML_INPUT_KEY_ENTER);
            break;
        }
    }
}

#define SHOW_FRAMES_MAX 4

/* Whether the step shows: a clicked row as the active one, a renamed row
   by its new name; the other steps in their first frame. */
static bool outliner_shown(studio *s, outliner_step step, int actor, const char *renamed)
{
    rml_element *row = outliner_row(s, actor);
    if (step == OUTLINER_CLICK) {
        return row && rml_element_query_selector(row, ".tree-row[data-active]");
    }
    if (step != OUTLINER_RENAME) {
        return true;
    }
    rml_element *label = row ? rml_element_query_selector(row, ".name") : NULL;
    char text[64] = "";
    if (label) {
        rml_element_inner_rml(label, text, sizeof text);
    }
    return !strcmp(text, renamed);
}

/* The behavior runs (dispatches) in the frame so far. */
static uint64_t behavior_runs(const game_behavior_world *behaviors)
{
    const game_behavior_profile_frame *profile = game_behavior_profile(behaviors);
    uint64_t runs = profile->overflow.dispatches;
    for (int i = 0; i < profile->count; i++) {
        runs += profile->rows[i].dispatches;
    }
    return runs;
}

/* Times the UI frames a step takes until it shows, with the behaviors'
   time and runs in them (game_behavior_profile) and what their views
   read. */
static void outliner_step_row(studio_bench *b, outliner_step step, const char *name, float row)
{
    studio *s = b->studio;
    rml_element *list = outliner_list(s);
    /* The objects a click or a rename picks are in view. */
    int middle = (int)(rml_element_scroll_top(list) / row);
    game_behavior_world *behaviors = studio_ui_behaviors(s);
    game_behavior_set_profiling(behaviors, true);
    double ms[RUNS], behavior_ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    int took = 0;
    uint64_t runs = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        int actor = middle + 4 + (run & 1);
        if (step == OUTLINER_RENAME) {
            select_instance(s->scene, actor);
            studio_rename_object(s);
            frames(b, 3);
        }
        outliner_change(b, step, run, row, middle);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = 0, behavior = 0;
        uint64_t frame_runs = 0;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phase[p] = 0;
        }
        /* A rename shows once its row names the new name. */
        char renamed[32];
        snprintf(renamed, sizeof renamed, "Renamed %d", run + WARMUP);
        int frame = 0;
        bool shown = false;
        while (!shown) {
            double frame_phases[PHASE_COUNT];
            if (frame) {
                cf_app_update(NULL);
            }
            game_behavior_profile_begin(behaviors);
            t += native_ui(s, frame_phases);
            cf_app_draw_onto_screen(true);
            behavior += game_behavior_profile(behaviors)->inclusive_cpu_ms;
            frame_runs += behavior_runs(behaviors);
            for (int p = 0; p < PHASE_COUNT; p++) {
                phase[p] += frame_phases[p];
            }
            frame++;
            shown = outliner_shown(s, step, actor, renamed) || frame == SHOW_FRAMES_MAX;
        }
        took = frame;
        runs = frame_runs;
        counts = outliner_counts(s, list, before);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        behavior_ms[run] = behavior;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    game_behavior_set_profiling(behaviors, false);
    bench_stat behavior = bench_stat_of(behavior_ms, RUNS);
    char label[200];
    snprintf(
        label,
        sizeof label,
        "%s (%d UI frame%s; behaviors %.3f ms, %llu runs)",
        name,
        took,
        took == 1 ? "" : "s",
        behavior.median,
        (unsigned long long)runs
    );
    bench_row("23", label, bench_stat_of(ms, RUNS), counts);
    phase_row("its phases", phases);
}

/* The Outliner over ten thousand objects, scrolled to the middle: a UI
   frame with nothing changed, a scroll step, a click on a row, a filter
   keystroke and a rename, each through the UI as a hand would. Run each
   build's binary in turn for an A/B (UI_BENCH_OUTLINER=1). */
static void outliner_ab(studio_bench *b)
{
    studio *s = b->studio;
    for (int i = 0; i < OUTLINER_OBJECTS; i++) {
        char name[32];
        snprintf(name, sizeof name, "Marker %d", i);
        int added = add_actor(b->scene, name, "DriftingMarker");
        assert(added == 0);
    }
    select_instance(b->scene, 0);
    studio_show_editor(s, STUDIO_EDITOR_OUTLINER);
    frames(b, 6);
    rml_element *list = outliner_list(s);
    assert(list);
    probe_documents(s);
    char value[32] = "";
    rml_element_property(list, "--row", value, sizeof value);
    float row = (float)atof(value) * studio_pixel_ratio(s);
    assert(row > 0);
    rml_element_set_scroll_top(list, row * OUTLINER_OBJECTS / 2);
    frames(b, 4);
    outliner_step_row(b, OUTLINER_IDLE, "Outliner: a UI frame with nothing changed", row);
    outliner_step_row(b, OUTLINER_SCROLL, "Outliner: a scroll step of 3 rows", row);
    outliner_step_row(b, OUTLINER_CLICK, "Outliner: click a row in view", row);
    outliner_step_row(b, OUTLINER_RENAME, "Outliner: rename in place, Enter to shown", row);
    rml_element *filter = studio_query(s, "#outliner-filter");
    assert(filter && rml_element_focus(filter, true));
    type_text(s, "Marker 5");
    frames(b, 4);
    outliner_step_row(b, OUTLINER_FILTER, "Outliner: a filter keystroke, \"Marker 5\" to \"Marker 51\"", row);
}

/* With UI_BENCH_OUTLINER_CAPTURE set to a directory, native shots of the
   Outliner (rows, selection, a hidden and a locked object, the row menu,
   rename in place) in both themes and densities. */
static void capture_outliner(studio_bench *b, const char *dir)
{
    static const struct
    {
        const char *name;
        studio_theme theme;
        bool comfortable;
    } VARIANTS[] = { { "dark-compact", STUDIO_THEME_DARK, false },
                     { "dark-comfortable", STUDIO_THEME_DARK, true },
                     { "light-compact", STUDIO_THEME_LIGHT, false },
                     { "light-comfortable", STUDIO_THEME_LIGHT, true } };
    studio *s = b->studio;
    s->maximized = -1;
    s->prefs.reduced_motion = true;
    studio_show_editor(s, STUDIO_EDITOR_OUTLINER);
    app_scene_authoring_set_actor_view(s->scene, 1, APP_SCENE_VIEW_HIDDEN);
    app_scene_authoring_set_actor_view(s->scene, 2, APP_SCENE_VIEW_LOCKED);
    select_instance(s->scene, 3);
    scene_toggle_instance(s->scene, 4);
    frames(b, 10);
    for (size_t v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++) {
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        frames(b, 6);
        rml_element *row = outliner_row(s, 5);
        assert(row);
        rml_vec2 at = rml_element_absolute_offset(row, RML_BOX_BORDER);
        float ratio = studio_pixel_ratio(s);
        pointer(s, RML_INPUT_POINTER_MOVE, at.x / ratio + 40, at.y / ratio + 6);
        shot(b, dir, VARIANTS[v].name, "outliner-rows");
        rml_input press = { .type = RML_INPUT_POINTER_DOWN,
                            .x = at.x / ratio + 40,
                            .y = at.y / ratio + 6,
                            .button = 1 };
        rml_backend_process_input(s->backend, &press);
        press.type = RML_INPUT_POINTER_UP;
        rml_backend_process_input(s->backend, &press);
        shot(b, dir, VARIANTS[v].name, "outliner-menu");
        key_press(s, RML_INPUT_KEY_ESCAPE);
        frames(b, 4);
        select_instance(s->scene, 3);
        scene_toggle_instance(s->scene, 4);
        studio_rename_object(s);
        shot(b, dir, VARIANTS[v].name, "outliner-rename");
        key_press(s, RML_INPUT_KEY_ESCAPE);
        frames(b, 4);
        pointer(s, RML_INPUT_POINTER_MOVE, 5, 5);
    }
}

/* ---------- 16: the Scene's overlays while its camera moves ---------- */

#define PAN_STEP 8.0f   /* world units a run pans, back and forth */
#define ZOOM_STEP 1.15f /* about a wheel notch */

/* Moves the Scene's camera as a held drag or a wheel notch does, `pan`
   across or by `zoom`, draws it, as the frame of the move does after its
   UI, and times the UI frame that follows, which shows the drawn camera. */
static double scene_step(studio_bench *b, float pan, float zoom, double *phases)
{
    engine_viewport_view view;
    engine_viewport_get_view(b->viewport, &view);
    view.center_x += pan;
    view.center_y += pan;
    view.width *= zoom;
    view.height *= zoom;
    engine_viewport_set_view(b->viewport, &view);
    frames(b, 1);
    cf_app_update(NULL);
    double t = native_ui(b->studio, phases);
    cf_app_draw_onto_screen(true);
    return t;
}

/* Layouts of every document, data views and elements in the Scene's. */
static void scene_case(studio_bench *b, float pan, float zoom, const char *name)
{
    studio *s = b->studio;
    rml_element *viewport = studio_query(s, ".viewport");
    assert(viewport);
    rml_element *document = rml_element_owner_document(viewport);
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        bool back = run & 1;
        bench_counts before = studio_mark(s);
        double t = scene_step(b, back ? -pan : pan, back ? 1 / zoom : zoom, phase);
        counts = studio_counts(s, before);
        counts.elements = rml_element_query_selector_all(document, "*", NULL, 0);
        if (run >= 0) {
            ms[run] = t;
            for (int p = 0; p < PHASE_COUNT; p++) {
                phases[p][run] = phase[p];
            }
        }
    }
    char row[160];
    snprintf(row, sizeof row, "%s, %d camera labels", name, studio_count(s, ".viewport .camera-label"));
    bench_row("16", row, bench_stat_of(ms, RUNS), counts);
    phase_row("its phases", phases);
}

/* A pan moves each camera frame's label; a zoom also rolls the zoom
   readout. Both should repaint without laying out the Scene's document. */
static void scene_overlays(studio_bench *b)
{
    studio *s = b->studio;
    studio_show_editor(s, STUDIO_EDITOR_SCENE);
    s->prefs.scene_cameras = true;
    frames(b, 6);
    probe_documents(s);
    scene_case(b, PAN_STEP, 1, "Scene camera panned");
    scene_case(b, 0, ZOOM_STEP, "Scene camera zoomed a notch");
    frames(b, 2);
}

/* ---------- 7: popovers and area clipping ---------- */

#define POP_SIZE 200

/* Two areas side by side like Studio's (relative, rounded, overflow
 * hidden, a scrolling panel inside); the first holds a fixed popover that
 * reaches over the second and below both. */
static const char
    POPOVER_DOC[] = "<rml><head><style>"
                    "body { width: 100%%; height: 100%%; margin: 0px; "
                    "background-color: #000000; }"
                    ".area { position: absolute; top: 0px; width: 100px; "
                    "height: 100px; overflow: hidden; "
                    "border-radius: 12px; }"
                    ".panel { position: absolute; left: 0px; top: 0px; right: "
                    "0px; bottom: 0px; overflow-y: auto; }"
                    "#a { left: 0px; background-color: #202020; %s }"
                    "#b { left: 100px; background-color: #00ff00; %s }"
                    ".pop { position: fixed; left: 60px; top: 60px; width: "
                    "100px; height: 60px; "
                    "background-color: #ff00ff; z-index: 10; %s }"
                    "</style></head><body>"
                    "<div class=\"area\" id=\"a\"><div class=\"panel\"><div "
                    "class=\"pop\"/></div></div>"
                    "<div class=\"area\" id=\"b\"/>"
                    "</body></rml>";

static CF_Pixel pixel_at(const CF_Pixel *pixels, int x, int y)
{
    return pixels[y * POP_SIZE + x];
}

static bool magenta(CF_Pixel p)
{
    return p.colors.r > 200 && p.colors.g < 60 && p.colors.b > 200;
}

static const char *seen(CF_Pixel p)
{
    return magenta(p)       ? "popover"
         : p.colors.g > 200 ? "neighbor area"
         : p.colors.r > 20  ? "own area"
                            : "nothing (clipped)";
}

static void popover_case(rml_backend *ui, const char *name, const char *a, const char *b, const char *pop)
{
    char text[2048];
    snprintf(text, sizeof text, POPOVER_DOC, a, b, pop);
    rml_element *doc = rml_context_load_document_from_memory(rml_backend_context(ui), text, "popover");
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(POP_SIZE, POP_SIZE));
    static CF_Pixel pixels[POP_SIZE * POP_SIZE];
    for (int frame = 0; frame < 2; frame++) {
        cf_app_update(NULL);
        rml_backend_update(ui, FRAME);
        rml_backend_render(ui);
        cf_draw_push();
        rml_backend_present(ui);
        cf_render_to(target, true);
        cf_draw_pop();
        cf_app_draw_onto_screen(true);
    }
    CF_Readback readback = cf_canvas_readback(target);
    uint64_t start = cf_get_ticks();
    while (!cf_readback_ready(readback)) {
        assert(cf_get_ticks() - start < cf_get_tick_frequency() * 10);
        cf_sleep(1);
    }
    int read = cf_readback_data(readback, pixels, (int)sizeof pixels);
    assert(read == (int)sizeof pixels);
    cf_destroy_readback(readback);
    printf(
        "| 7 | %s | inside its area: %s; over the next area: %s; outside both: "
        "%s | | | | | |\n",
        name,
        seen(pixel_at(pixels, 80, 80)),
        seen(pixel_at(pixels, 130, 80)),
        seen(pixel_at(pixels, 130, 110))
    );
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_update(ui, 0);
}

static void popover_clipping()
{
    rml_backend *ui = rml_backend_create(POP_SIZE, POP_SIZE, 1);
    popover_case(ui, "fixed popover, clip: none", "", "", "clip: none;");
    popover_case(ui, "fixed popover without clip: none", "", "", "");
    popover_case(ui, "clip: none, the next area has z-index: 1", "", "z-index: 1;", "clip: none;");
    popover_case(ui, "clip: none, its area has z-index: 0", "z-index: 0;", "", "clip: none;");
    popover_case(ui, "clip: none, its area has a transform", "transform: translateX(0px);", "", "clip: none;");
    popover_case(ui, "clip: none, its area has a filter", "filter: opacity(1);", "", "clip: none;");
    rml_backend_destroy(ui);
}

/* ---------- 10: Members over ~200 members ---------- */

#define MEMBER_VARIABLES 192
#define MEMBER_CATEGORY 16 /* variables per category */

/* Members' scroller, native (the kit's list) or authored (the List). */
static rml_element *members_list(studio *s)
{
    rml_element *authored = studio_query(s, "#members-list");
    return authored ? authored : studio_query(s, ".members-list");
}

/* What one run of a Members case changes before its UI work. */
typedef enum
{
    MEMBERS_IDLE,
    MEMBERS_SELECT,
    MEMBERS_SCROLL,
    MEMBERS_FILTER,
} members_step;

static void members_change(studio *s, members_step step, int run, const uint64_t *ids)
{
    rml_element *list = members_list(s);
    float row = 26 * studio_pixel_ratio(s);
    switch (step) {
        case MEMBERS_IDLE: break;
        case MEMBERS_SELECT:
            s->members.kind = STUDIO_MEMBER_VARIABLE;
            s->members.id = ids[run & 1];
            break;
        case MEMBERS_SCROLL:
            rml_element_set_scroll_top(list, (run & 1 ? 40 : 43) * row);
            break;
        case MEMBERS_FILTER:
            snprintf(s->members.filter, sizeof s->members.filter, "%s", run & 1 ? "Value 1" : "");
            break;
    }
}

static void members_step_row(studio_bench *b, members_step step, const char *name, const uint64_t *ids)
{
    studio *s = b->studio;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        members_change(s, step, run, ids);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    bench_row("10", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* The open behavior with MEMBER_VARIABLES more variables in categories,
   shown by Members in the Properties area: a UI frame with nothing
   changed, a selection change, a scroll step of 3 rows and a filter
   keystroke. Run each build's binary in turn for an A/B. */
static void members(studio_bench *b)
{
    studio *s = b->studio;
    behavior_asset *asset = b->graph->asset;
    for (int i = 0; i < MEMBER_VARIABLES; i++) {
        char name[32], category[32];
        snprintf(name, sizeof name, "Value %d", i);
        snprintf(category, sizeof category, "Group %d", i / MEMBER_CATEGORY);
        behavior_variable *v = behavior_asset_add_variable(
            asset,
            name,
            PIN_F32,
            "float",
            PIN_CONTAINER_SINGLE,
            NULL,
            category
        );
        assert(v);
    }
    uint64_t ids[2] = {
        asset->variables[acount(asset->variables) - MEMBER_VARIABLES].id,
        asset->variables[acount(asset->variables) - MEMBER_VARIABLES + 2].id
    };
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_PROPERTIES] = STUDIO_EDITOR_MEMBERS;
    frames(b, 6);
    rml_element *list = members_list(s);
    assert(list);
    probe_documents(s);
    float ratio = studio_pixel_ratio(s);
    rml_vec2 size = rml_element_box_size(list, RML_BOX_BORDER);
    rml_element *rows[256];
    int count = rml_element_query_selector_all(list, ".tree-row", rows, 256),
        shown = 0;
    for (int i = 0; i < count && i < 256; i++) {
        shown += rml_element_is_visible(rows[i], true);
    }
    printf(
        "|  | Members %.0fx%.0f dp over %d variables; %d rows in its window | "
        "| | | | | |\n",
        size.x / ratio,
        size.y / ratio,
        acount(asset->variables),
        shown
    );
    members_step_row(b, MEMBERS_IDLE, "Members: a UI frame with nothing changed", ids);
    members_step_row(b, MEMBERS_SELECT, "Members: select another variable", ids);
    members_step_row(b, MEMBERS_SCROLL, "Members: scroll 3 rows", ids);
    members_step_row(b, MEMBERS_FILTER, "Members: filter by a keystroke", ids);
}

/* ---------- 18: typing in the palette ---------- */

/* Assets added to the Sandbox's for the palette to rank, with commands,
   settings and objects: thousands of results with nothing typed. */
#define PALETTE_ADDED 2000

typedef enum
{
    PALETTE_OPEN,      /* ⌘K: the palette shows, 2 UI frames */
    PALETTE_FILTERED,  /* "qa" to "qab" and back: few results a group */
    PALETTE_EMPTIED,   /* "" to "b" and back: every result, then a few */
    PALETTE_HIGHLIGHT, /* Down and Up */
    PALETTE_IDLE,      /* a UI frame with nothing changed */
} palette_step;

static void palette_query(studio *s, const char *query)
{
    ui_activation a = { .argc = 1 };
    snprintf(a.args[0], sizeof a.args[0], "%s", query);
    studio_palette_action(s, "query", &a);
}

static void palette_change(studio_bench *b, palette_step step, int run)
{
    studio *s = b->studio;
    switch (step) {
        case PALETTE_OPEN:
            studio_palette_close(s);
            frames(b, 2);
            studio_palette_open(s, STUDIO_PALETTE_SEARCH, STUDIO_PALETTE_PLACEHOLDER);
            break;
        case PALETTE_FILTERED: palette_query(s, run & 1 ? "qab" : "qa"); break;
        case PALETTE_EMPTIED: palette_query(s, run & 1 ? "b" : ""); break;
        case PALETTE_HIGHLIGHT: {
            ui_activation a = { .argc = 1, .has_value = true, .value = (float)(run & 1) };
            snprintf(a.args[0], sizeof a.args[0], "%d", run & 1);
            studio_palette_action(s, "highlight", &a);
            break;
        }
        case PALETTE_IDLE: break;
    }
}

/* Times one step's UI work (two UI frames to open), counting the views
   evaluated, the documents laid out and the elements of the overlay
   layer's document, which holds the palette. */
static void palette_case(studio_bench *b, palette_step step, const char *name)
{
    studio *s = b->studio;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT], more[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        palette_change(b, step, run);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        if (step == PALETTE_OPEN) {
            t += native_ui(s, more);
            for (int p = 0; p < PHASE_COUNT; p++) {
                phase[p] += more[p];
            }
        }
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        rml_element *search = studio_query(s, "#palette-search");
        counts.elements = search ? rml_element_query_selector_all(
                                       rml_element_owner_document(search),
                                       "*",
                                       NULL,
                                       0
                                   )
                                 : 0;
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    bench_row("18", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* The palette over the Sandbox's commands, settings, objects and assets
   and 2,000 more assets: open it, type a keystroke that keeps few results
   a group, one that empties the query and back, move the highlight, and an
   idle frame. */
static void palette(studio_bench *b)
{
    studio *s = b->studio;
    for (int i = 0; i < PALETTE_ADDED; i++) {
        char name[32];
        snprintf(name, sizeof name, "QaPalette%04d", i);
        reference_index_add_asset(b->scene->asset_index, REFERENCE_ASSET_MATERIAL, name, NULL, name);
    }
    b->scene->asset_index_revision++;
    s->maximized = -1;
    frames(b, 2);
    studio_palette_open(s, STUDIO_PALETTE_SEARCH, STUDIO_PALETTE_PLACEHOLDER);
    frames(b, 4);
    probe_new_documents(s);
    printf(
        "|  | Palette: %d results with nothing typed, %d rows in its window | "
        "| | | | | |\n",
        acount(s->palette.places),
        studio_count(s, ".palette-row")
    );
    palette_case(b, PALETTE_OPEN, "Palette: open, 2 UI frames");
    palette_case(b, PALETTE_FILTERED, "Palette: a keystroke, \"qa\" to \"qab\"");
    palette_case(b, PALETTE_EMPTIED, "Palette: a keystroke, \"\" to \"b\"");
    palette_query(s, "");
    frames(b, 2);
    palette_case(b, PALETTE_HIGHLIGHT, "Palette: move the highlight");
    palette_case(b, PALETTE_IDLE, "Palette: a UI frame with nothing changed");
    studio_palette_close(s);
    frames(b, 2);
}

/* ---------- 20: the Inspector over ten thousand elements ---------- */

#define INSPECTOR_ITEMS 10000
#define INSPECTOR_ROWS_MAX 1100

/* The Inspector's tree, the authored List (workstream I1); before it, the
   panel's own scroller ".inspect-tree". */
static rml_element *inspector_tree(studio *s)
{
    rml_element *list = studio_query(s, "#inspector-list");
    return list ? list : studio_query(s, ".inspect-tree");
}

/* The tree rows shown (templates left out), and how many lack the mark a
   step leaves, which it instanced. */
static int inspector_rows(rml_element *tree, bool mark, int *shown)
{
    static rml_element *rows[INSPECTOR_ROWS_MAX];
    int count = rml_element_query_selector_all(tree, ".tree-row", rows, INSPECTOR_ROWS_MAX);
    assert(count < INSPECTOR_ROWS_MAX);
    int fresh = 0;
    *shown = 0;
    for (int i = 0; i < count; i++) {
        if (!rml_element_is_visible(rows[i], false)) {
            continue;
        }
        ++*shown;
        fresh += !rml_element_has_attribute(rows[i], "data-bench");
        if (mark) {
            rml_element_set_attribute(rows[i], "data-bench", "");
        }
    }
    return fresh;
}

typedef enum
{
    INSPECTOR_IDLE,
    INSPECTOR_SELECT,
    INSPECTOR_SCROLL,
} inspector_step;

/* One of the first items is selected in turn, as clicks on their rows; a
   scroll step moves three rows. */
static void inspector_change(studio *s, inspector_step step, int run, rml_element *tree, rml_element **items)
{
    char value[32] = "";
    rml_element_property(tree, "--row", value, sizeof value);
    float row = (float)atof(value) * studio_pixel_ratio(s);
    switch (step) {
        case INSPECTOR_IDLE: break;
        case INSPECTOR_SELECT:
            studio_inspect_select(s->inspect, items[100 + (run & 1)]);
            s->ui_schedule.wake = true;
            break;
        case INSPECTOR_SCROLL:
            rml_element_set_scroll_top(tree, (200 + 3 * (run + WARMUP)) * row);
            break;
    }
}

static void inspector_step_row(studio_bench *b, inspector_step step, const char *name, rml_element **items)
{
    studio *s = b->studio;
    rml_element *tree = inspector_tree(s);
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    int instanced = 0, shown = 0;
    inspector_rows(tree, true, &shown);
    for (int run = -WARMUP; run < RUNS; run++) {
        inspector_change(s, step, run, tree, items);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        bench_counts now = studio_mark(s);
        counts = (bench_counts){
            .values = now.values - before.values,
            .lookups = now.lookups - before.lookups,
            .layouts = now.layouts - before.layouts,
            .elements = rml_element_query_selector_all(rml_element_owner_document(tree), "*", NULL, 0)
        };
        instanced = inspector_rows(tree, true, &shown);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    char label[200];
    snprintf(label, sizeof label, "%s: %d rows shown, %d instanced", name, shown, instanced);
    bench_row("20", label, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* The Inspector in the Properties area over a document of ten thousand
   elements, shown but not displayed, whose items are open in the tree: a UI
   frame with nothing changed, a selection change and a scroll step of
   three rows. Run each build's binary in turn for an A/B. */
static void inspector_tree_case(studio_bench *b)
{
    studio *s = b->studio;
    size_t size = INSPECTOR_ITEMS * 40 + 256, n = 0;
    char *rml = malloc(size);
    assert(rml);
    n += (size_t)snprintf(rml + n, size - n, "<rml><body style=\"display: none; font-family: Geist;\"><div id=\"items\">");
    for (int i = 0; i < INSPECTOR_ITEMS; i++) {
        n += (size_t)snprintf(rml + n, size - n, "<div class=\"item\">Item %d</div>", i);
    }
    snprintf(rml + n, size - n, "</div></body></rml>");
    rml_element *doc = rml_context_load_document_from_memory(rml_backend_context(s->backend), rml, "[bench]");
    free(rml);
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    rml_element *container = rml_element_by_id(doc, "items");
    static rml_element *items[INSPECTOR_ITEMS];
    for (int i = 0; i < INSPECTOR_ITEMS; i++) {
        items[i] = rml_element_child(container, i);
    }
    s->maximized = -1;
    studio_area_show(s, STUDIO_AREA_PROPERTIES, STUDIO_EDITOR_PANEL, "Inspector");
    frames(b, 6);
    assert(s->inspect);
    studio_inspect_select(s->inspect, items[100]);
    s->ui_schedule.wake = true;
    frames(b, 6);
    rml_element *tree = inspector_tree(s);
    assert(tree);
    probe_documents(s);
    float ratio = studio_pixel_ratio(s);
    rml_vec2 size_dp = rml_element_box_size(tree, RML_BOX_BORDER);
    int shown = 0;
    inspector_rows(tree, false, &shown);
    printf(
        "|  | Inspector tree %.0fx%.0f dp over a document of %d elements; %d "
        "rows in its "
        "document | | | | | |\n",
        size_dp.x / ratio,
        size_dp.y / ratio,
        INSPECTOR_ITEMS,
        shown
    );
    inspector_step_row(b, INSPECTOR_IDLE, "Inspector: a UI frame with nothing changed", items);
    inspector_step_row(b, INSPECTOR_SELECT, "Inspector: select another element", items);
    inspector_step_row(b, INSPECTOR_SCROLL, "Inspector: scroll 3 rows", items);
}

/* With UI_BENCH_INSPECTOR_CAPTURE set to a directory, native shots of the
   Inspector in the Properties area, with an element selected and with the
   pointer over a row, in dark and light, compact and comfortable, with
   reduced motion. The kit build and the authored one both take them. */
static void capture_inspector(studio_bench *b, const char *dir)
{
    static const struct
    {
        const char *name;
        studio_theme theme;
        bool comfortable;
    } VARIANTS[] = { { "dark-compact", STUDIO_THEME_DARK, false },
                     { "dark-comfortable", STUDIO_THEME_DARK, true },
                     { "light-compact", STUDIO_THEME_LIGHT, false },
                     { "light-comfortable", STUDIO_THEME_LIGHT, true } };
    studio *s = b->studio;
    s->maximized = -1;
    s->prefs.reduced_motion = true;
    studio_area_show(s, STUDIO_AREA_PROPERTIES, STUDIO_EDITOR_PANEL, "Inspector");
    frames(b, 10);
    for (size_t v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++) {
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        frames(b, 6);
        rml_element *target = studio_query(s, "[data-ui=Inspector] .toolbar .pick .label");
        assert(target && s->inspect);
        studio_inspect_select(s->inspect, target);
        s->ui_schedule.wake = true;
        pointer(s, RML_INPUT_POINTER_MOVE, 5, 5);
        shot(b, dir, VARIANTS[v].name, "inspector-selected");
        rml_element *rows[32], *row = NULL;
        rml_element *tree = inspector_tree(s);
        int count = rml_element_query_selector_all(tree, ".tree-row", rows, 32),
            seen = 0;
        for (int i = 0; i < count && !row; i++) {
            row = rml_element_is_visible(rows[i], false) && ++seen == 7 ? rows[i]
                                                                        : NULL;
        }
        assert(row);
        rml_vec2 at = rml_element_absolute_offset(row, RML_BOX_BORDER);
        float ratio = studio_pixel_ratio(s);
        pointer(s, RML_INPUT_POINTER_MOVE, at.x / ratio + 60, at.y / ratio + 6);
        shot(b, dir, VARIANTS[v].name, "inspector-hover");
        pointer(s, RML_INPUT_POINTER_MOVE, 5, 5);
    }
}

/* ---------- Run ---------- */

/* ---------- 21: Properties' Node tab (workstream G1) ---------- */

typedef enum
{
    NODE_SELECT, /* select the other of two nodes: the tab rebinds */
    NODE_EDIT,   /* a vector literal's axis grows, as a typed value does */
    NODE_IDLE,   /* a UI frame with nothing changed */
} node_step;

typedef struct node_case
{
    studio_bench *b;
    uint64_t nodes[2]; /* a Direction To and an Atan2 */
    int target;        /* Direction To's Target */
} node_case;

static void node_change(node_case *c, node_step step, int run)
{
    studio *s = c->b->studio;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    node *n = graph_node_by_id(gr, c->nodes[run & 1]);
    if (step == NODE_SELECT) {
        sg_select_only(e, n);
        return;
    }
    if (step == NODE_EDIT) {
        node *direction = graph_node_by_id(gr, c->nodes[0]);
        const pin_spec *pin = &direction->input_pins[c->target];
        const float *was = pin->default_value.ptr;
        float vector[2] = { was ? was[0] + 1 : 1, was ? was[1] : 0 };
        pin_value value = pin->default_value;
        value.ptr = vector;
        int set = graph_set_pin_default(gr, direction, 0, c->target, &value);
        assert(set == 0);
    }
}

/* Times one step's UI frame, counting the views evaluated, the documents
   laid out and the elements of Properties' document. */
static void node_case_step(node_case *c, node_step step, const char *name)
{
    studio *s = c->b->studio;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        node_change(c, step, run);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        rml_element *area = studio_query(s, "[data-ui=Properties]");
        counts.elements = area ? rml_element_query_selector_all(
                                     rml_element_owner_document(area),
                                     "*",
                                     NULL,
                                     0
                                 )
                               : 0;
        frames(c->b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    bench_row("21", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* Properties' Node tab beside the Behavior area over Movement: a node
   selection change between a Direction To and an Atan2, a literal edit of
   the Direction To's Target, and an idle frame. */
static void node_tab(studio_bench *b)
{
    studio *s = b->studio;
    s->maximized = -1;
    studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
    studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
    frames(b, 4);
    editor *e = sg_graph_editor(s);
    node *direction = editor_spawn(e, registry_node_by_name("Direction To"), 40, 40);
    node *atan = editor_spawn(e, registry_node_by_name("Atan2"), 40, 240);
    node_case c = { .b = b, .nodes = { direction->id, atan->id }, .target = -1 };
    for (int p = 0; p < direction->input_count; p++) {
        c.target = !strcmp(direction->input_pins[p].name, "Target") ? p : c.target;
    }
    assert(c.target >= 0);
    sg_select_only(e, direction);
    frames(b, 6);
    probe_documents(s);
    printf("|  | Node tab: tab %d | | | | | | |\n", s->tab);
    node_case_step(&c, NODE_SELECT, "Node tab: select the other node");
    sg_select_only(e, graph_node_by_id(sg_active_graph(s), c.nodes[0]));
    frames(b, 4);
    node_case_step(&c, NODE_EDIT, "Node tab: a vector literal's axis grows");
    node_case_step(&c, NODE_IDLE, "Node tab: a UI frame with nothing changed");
}

/* ---------- 22: the visual designer ---------- */

#define DESIGNER_ITEMS 40

/* The elements of designer_document, in outline order. */
enum
{
    DESIGNER_ROOT,
    DESIGNER_TITLE,
    DESIGNER_ROW,
    DESIGNER_OK,
    DESIGNER_CANCEL,
    DESIGNER_CHECK,
    DESIGNER_IMAGE,
    DESIGNER_USE,
    DESIGNER_GROUP,
};

static const char DESIGNER_RCSS
    [] = ".panel { display: block; width: 240dp; color: #ffffff; "
         "border-radius: 6dp; font-size: 13dp; }\n"
         ".row button { color: #ff0000; flex-grow: 1; z-index: 2; }\n"
         ".title { font-weight: bold; text-align: center; }\n"
         "#root:hover { opacity: 1; }\n"
         ".big { font-size: 16dp; }\n";

/* A document with every kind of Details value, and `items` spans in its
   group, as the Hierarchy's rows. */
static void designer_document(char *out, size_t size, int items)
{
    size_t length = (size_t)snprintf(
        out,
        size,
        "<div id=\"root\" class=\"panel\" style=\"width: 280dp; height: 220dp; "
        "opacity: 0.85; background-color: #22334488; box-sizing: "
        "border-box\">\n"
        "  <span class=\"title big\" data-if=\"shown\">Title</span>\n"
        "  <div class=\"row\" style=\"display: flex; padding: 6dp; margin: 2dp "
        "4dp; "
        "justify-content: space-between\">\n"
        "    <button id=\"ok\" data-event-click=\"emit('Ok')\">OK</button>\n"
        "    <button id=\"cancel\" class=\"secondary\">Cancel</button>\n"
        "    <input type=\"checkbox\"/>\n"
        "  </div>\n"
        "  <img src=\"icon.png\"/>\n"
        "  <use ui=\"Meter\" id=\"meter\"/>\n"
        "  <div class=\"group\">\n"
    );
    for (int i = 0; i < items && length + 64 < size; i++) {
        length += (size_t)snprintf(out + length, size - length, "    <span>Item %d</span>\n", i);
    }
    snprintf(out + length, size - length, "  </div>\n</div>\n");
}

/* HUD.rml of the arena open as a UI document, its text replaced in memory
   with the designer document, with Hierarchy, Details and Library beside the
   Canvas. Nothing is saved. */
static void designer_open(studio_bench *b, int items)
{
    studio *s = b->studio;
    s->maximized = -1;
    s->prefs.code_apply_live = false;
    s->prefs.reduced_motion = true;
    char path[PATH_MAX];
    bool real = realpath(ARENA "/content/ui/HUD.rml", path) != NULL;
    assert(real && studio_open_ui_source(s, path, 1));
    frames(b, 4);
    ui_buffer *buffer = studio_ui_buffer(s);
    static char markup[16384];
    designer_document(markup, sizeof markup, items);
    text_edit rml = { 0, (int)strlen(ui_buffer_text(buffer, UI_PART_RML)), markup };
    ui_buffer_edit(buffer, UI_PART_RML, &rml, 1);
    text_edit rcss = { 0, (int)strlen(ui_buffer_text(buffer, UI_PART_RCSS)), DESIGNER_RCSS };
    ui_buffer_edit(buffer, UI_PART_RCSS, &rcss, 1);
    studio_show_editor(s, STUDIO_EDITOR_ASSETS);
    frames(b, 6);
}

/* The Hierarchy's rows, native or authored, are the Outliner area's
   tree rows. */
static int designer_rows(studio *s, rml_element **rows, int max)
{
    rml_element *all[MAX_ROWS];
    int found = 0;
    rml_context *context = rml_backend_context(s->backend);
    float ratio = studio_pixel_ratio(s);
    kit_rect area = s->areas[STUDIO_AREA_OUTLINER].rect;
    for (int d = 0; d < rml_context_document_count(context); d++) {
        int count = rml_element_query_selector_all(rml_context_document(context, d), ".tree-row", all, MAX_ROWS);
        for (int i = 0; i < count && i < MAX_ROWS && found < max; i++) {
            rml_vec2 at = rml_element_absolute_offset(all[i], RML_BOX_BORDER);
            float x = at.x / ratio, y = at.y / ratio;
            if (x >= area.x && x < area.x + area.w && y >= area.y &&
                y < area.y + area.h && rml_element_is_visible(all[i], true)) {
                rows[found++] = all[i];
            }
        }
    }
    return found;
}

/* The row whose meta starts with `meta` ("#ok"), or the document's row
   for NULL. */
static rml_element *designer_row(studio *s, const char *meta)
{
    rml_element *rows[MAX_ROWS];
    int count = designer_rows(s, rows, MAX_ROWS);
    for (int i = 0; i < count; i++) {
        rml_element *held = rml_element_query_selector(rows[i], ".meta");
        char text[96] = "";
        if (held) {
            rml_element_inner_rml(held, text, sizeof text);
        }
        if (meta ? !strncmp(text, meta, strlen(meta)) : i == 0) {
            return rows[i];
        }
    }
    return NULL;
}

typedef enum
{
    DESIGNER_IDLE,
    DESIGNER_SELECT,
    DESIGNER_SELECT_LIKE,
    DESIGNER_EDIT,
} designer_case;

/* What one run of a designer case changes before its UI work: the selection
   alternates between two elements, or the root's inline width is edited as
   a scrub's commit writes it. */
static void designer_change(studio *s, designer_case step, int run)
{
    static int serial;
    switch (step) {
        case DESIGNER_IDLE: break;
        case DESIGNER_SELECT:
            designer_select(s, run & 1 ? DESIGNER_OK : DESIGNER_TITLE);
            break;
        case DESIGNER_SELECT_LIKE:
            designer_select(s, run & 1 ? DESIGNER_OK : DESIGNER_CANCEL);
            break;
        case DESIGNER_EDIT: {
            ui_buffer *buffer;
            studio_designer *d = designer_state(s, &buffer);
            char value[24], error[192] = "";
            snprintf(value, sizeof value, "%ddp", 300 + serial++);
            bool ok = ui_design_set_style(buffer, d->selected, "width", value, false, error, sizeof error);
            assert(ok);
            break;
        }
    }
}

static void designer_case_row(studio_bench *b, designer_case step, const char *name)
{
    studio *s = b->studio;
    double ms[RUNS], phases[PHASE_COUNT][RUNS], phase[PHASE_COUNT];
    bench_counts counts = { 0 };
    for (int run = -WARMUP; run < RUNS; run++) {
        designer_change(s, step, run);
        cf_app_update(NULL);
        bench_counts before = studio_mark(s);
        double t = native_ui(s, phase);
        cf_app_draw_onto_screen(true);
        counts = studio_counts(s, before);
        frames(b, 1);
        if (run < 0) {
            continue;
        }
        ms[run] = t;
        for (int p = 0; p < PHASE_COUNT; p++) {
            phases[p][run] = phase[p];
        }
    }
    bench_row("22", name, bench_stat_of(ms, RUNS), counts);
    phase_row("phases", phases);
}

/* The designer over DESIGNER_ITEMS spans: a UI frame with nothing changed,
   a selection change, and an inline width written to the selected root,
   which re-parses the document and rewrites Details. Run each build's
   binary in turn for an A/B. */
static void designer(studio_bench *b)
{
    studio *s = b->studio;
    designer_open(b, DESIGNER_ITEMS);
    probe_documents(s);
    rml_element *rows[MAX_ROWS];
    printf(
        "|  | Designer over %d elements; %d Hierarchy rows shown | | | | | | "
        "|\n",
        DESIGNER_GROUP + 1 + DESIGNER_ITEMS,
        designer_rows(s, rows, MAX_ROWS)
    );
    designer_case_row(b, DESIGNER_IDLE, "Designer: a UI frame with nothing changed");
    designer_case_row(b, DESIGNER_SELECT, "Designer: select another element");
    designer_case_row(b, DESIGNER_SELECT_LIKE, "Designer: select another button");
    designer_select(s, DESIGNER_ROOT);
    frames(b, 4);
    designer_case_row(b, DESIGNER_EDIT, "Designer: write the root's inline width");
}

/* The input helpers of studio_documents.c, whose header would clash with
   this file's frames. */
void mouse_move(const studio *s, float x, float y);
void graph_drag_button(const studio *s, float x, float y, bool down);

/* The pointer to (x, y) in dp for the kit, which reads SDL's, and for RmlUi. */
static void move_to(studio *s, float x, float y)
{
    mouse_move(s, x, y);
    pointer(s, RML_INPUT_POINTER_MOVE, x, y);
}

static void center_of(studio *s, rml_element *el, float *x, float *y)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    *x = (at.x + size.x / 2) / ratio;
    *y = (at.y + size.y / 2) / ratio;
}

/* A press and release on `el` with the pointer there. */
static void designer_press(studio_bench *b, rml_element *el, int button)
{
    assert(el);
    studio *s = b->studio;
    float x, y;
    center_of(s, el, &x, &y);
    move_to(s, x, y);
    frames(b, 1);
    rml_input input = { .x = x, .y = y, .button = button };
    input.type = RML_INPUT_POINTER_DOWN;
    rml_backend_process_input(s->backend, &input);
    input.type = RML_INPUT_POINTER_UP;
    rml_backend_process_input(s->backend, &input);
}

/* The first element `selector` matches inside area `area`. */
static rml_element *area_query(studio *s, int area, const char *selector)
{
    rml_element *found[MAX_ROWS];
    rml_context *context = rml_backend_context(s->backend);
    float ratio = studio_pixel_ratio(s);
    kit_rect rect = s->areas[area].rect;
    for (int d = 0; d < rml_context_document_count(context); d++) {
        int count = rml_element_query_selector_all(rml_context_document(context, d), selector, found, MAX_ROWS);
        for (int i = 0; i < count && i < MAX_ROWS; i++) {
            rml_vec2 at = rml_element_absolute_offset(found[i], RML_BOX_BORDER);
            float x = at.x / ratio, y = at.y / ratio;
            if (x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
                y < rect.y + rect.h && rml_element_is_visible(found[i], true)) {
                return found[i];
            }
        }
    }
    return NULL;
}

/* Presses `from`, carries it past the drag threshold to `x`, `y` (dp) and
   holds it there. */
static void designer_carry(studio_bench *b, rml_element *from, float x, float y)
{
    studio *s = b->studio;
    float ratio = studio_pixel_ratio(s);
    rml_vec2 start = rml_element_absolute_offset(from, RML_BOX_BORDER);
    float sx = start.x / ratio + 30, sy = start.y / ratio + 6;
    move_to(s, sx, sy);
    frames(b, 1);
    graph_drag_button(s, sx, sy, true);
    frames(b, 1);
    move_to(s, sx - 12, sy);
    frames(b, 2);
    move_to(s, x, y);
    frames(b, 4);
}

static void designer_release(studio_bench *b)
{
    studio *s = b->studio;
    ui_drag_cancel(rml_backend_context(s->backend));
    frames(b, 3);
    graph_drag_button(s, 1, 1, false);
    frames(b, 2);
}

/* Selects an element and asks for a refresh with a pointer move, which the
   builds before the authored panels needed to show it before their idle
   refresh. */
static void select_element(studio_bench *b, int element)
{
    static int flip;
    designer_select(b->studio, element);
    move_to(b->studio, 600 + (flip++ & 1), 300);
}

/* With UI_BENCH_DESIGNER_CAPTURE set to a directory, native shots of the
   Hierarchy, Details and Library in dark and light, compact and
   comfortable: the rows with a selection, the row menu, the Add menu, a
   row dragged over the middle and top of another, Details of the root
   with an Add Class popover and a color picker, of a row with a keyword
   open, of a button with a binding, and of the document, and the Library
   with a tile hovered. The pointer rests over the Canvas between shots. */
static void capture_designer(studio_bench *b, const char *dir)
{
    static const struct
    {
        const char *name;
        studio_theme theme;
        bool comfortable;
    } VARIANTS[] = { { "dark-compact", STUDIO_THEME_DARK, false },
                     { "dark-comfortable", STUDIO_THEME_DARK, true },
                     { "light-compact", STUDIO_THEME_LIGHT, false },
                     { "light-comfortable", STUDIO_THEME_LIGHT, true } };
    studio *s = b->studio;
    designer_open(b, 3);
    float ratio = studio_pixel_ratio(s);
    rml_context *context = rml_backend_context(s->backend);
    for (size_t v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++) {
        const char *name = VARIANTS[v].name;
        s->prefs.theme = VARIANTS[v].theme;
        s->prefs.comfortable = VARIANTS[v].comfortable;
        move_to(s, 600, 300);
        select_element(b, DESIGNER_OK);
        shot(b, dir, name, "rows");
        designer_press(b, designer_row(s, "#ok"), 1);
        shot(b, dir, name, "menu");
        popover_hide_all(context);
        move_to(s, 600, 300);
        frames(b, 6);
        designer_press(b, studio_query(s, "[data-tip=Add]"), 0);
        shot(b, dir, name, "add");
        popover_hide_all(context);
        move_to(s, 600, 300);
        frames(b, 6);
        rml_element *target = designer_row(s, ".row");
        rml_vec2 at = rml_element_absolute_offset(target, RML_BOX_BORDER);
        rml_vec2 size = rml_element_box_size(target, RML_BOX_BORDER);
        designer_carry(
            b,
            designer_row(s, "#cancel"),
            (at.x + size.x / 2) / ratio,
            (at.y + size.y * 0.5f) / ratio
        );
        shot(b, dir, name, "drag-inside");
        move_to(s, (at.x + size.x / 2) / ratio, (at.y + size.y * 0.1f) / ratio);
        shot(b, dir, name, "drag-before");
        designer_release(b);
        move_to(s, 600, 300);
        select_element(b, DESIGNER_ROOT);
        shot(b, dir, name, "details");
        designer_press(b, area_query(s, STUDIO_AREA_PROPERTIES, ".designer-add"), 0);
        shot(b, dir, name, "details-add");
        popover_hide_all(context);
        move_to(s, 600, 300);
        frames(b, 6);
        designer_press(b, area_query(s, STUDIO_AREA_PROPERTIES, "color-field"), 0);
        shot(b, dir, name, "details-color");
        popover_hide_all(context);
        move_to(s, 600, 300);
        frames(b, 6);
        select_element(b, DESIGNER_ROW);
        move_to(s, 600, 300);
        shot(b, dir, name, "details-row");
        designer_press(b, area_query(s, STUDIO_AREA_PROPERTIES, ".field.select"), 0);
        shot(b, dir, name, "details-keyword");
        popover_hide_all(context);
        move_to(s, 600, 300);
        frames(b, 6);
        select_element(b, DESIGNER_TITLE);
        shot(b, dir, name, "details-text");
        select_element(b, -1);
        shot(b, dir, name, "document");
        rml_element *tile = studio_query(s, ".designer-tile");
        assert(tile);
        float x, y;
        center_of(s, tile, &x, &y);
        move_to(s, x, y);
        shot(b, dir, name, "library");
    }
}

int ui_bench_studio(const char *program, bool members_only)
{
    CF_Result result = cf_make_app(
        "UI bench",
        0,
        0,
        0,
        WIDTH,
        HEIGHT,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT,
        program
    );
    if (cf_is_error(result)) {
        fprintf(stderr, "UI bench app: %s\n", result.details);
        return 1;
    }
    app_scene_renderer_initialize();
    app_input_initialize();
    studio_bench b = bench_open();
    frames(&b, 4);
    probe_documents(b.studio);
    bench_header("Studio on the Sandbox at 1440x900, native");
    printf("|  | pixel scale %.1f | | | | | | |\n", cf_app_get_pixel_scale());
    /* UI_BENCH_CAPTURE takes C2's native shots alone. */
    if (getenv("UI_BENCH_CAPTURE")) {
        capture_chrome(&b, getenv("UI_BENCH_CAPTURE"));
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_NODE runs case 21 alone. */
    if (getenv("UI_BENCH_NODE")) {
        node_tab(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_DESIGNER_CAPTURE takes D1's native shots alone, and
       UI_BENCH_DESIGNER runs case 22 alone. */
    if (getenv("UI_BENCH_DESIGNER_CAPTURE")) {
        capture_designer(&b, getenv("UI_BENCH_DESIGNER_CAPTURE"));
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    if (getenv("UI_BENCH_DESIGNER")) {
        designer(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_OUTLINER runs case 23 alone; UI_BENCH_OUTLINER_CAPTURE
       takes the Outliner's native shots. */
    if (getenv("UI_BENCH_OUTLINER") || getenv("UI_BENCH_OUTLINER_CAPTURE")) {
        if (getenv("UI_BENCH_OUTLINER")) {
            outliner_ab(&b);
        } else {
            capture_outliner(&b, getenv("UI_BENCH_OUTLINER_CAPTURE"));
        }
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_PALETTE runs case 18 alone. */
    if (getenv("UI_BENCH_PALETTE")) {
        palette(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_INSPECTOR runs case 20 alone. */
    if (getenv("UI_BENCH_INSPECTOR")) {
        inspector_tree_case(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_INSPECTOR_CAPTURE takes the Inspector's native shots alone. */
    if (getenv("UI_BENCH_INSPECTOR_CAPTURE")) {
        capture_inspector(&b, getenv("UI_BENCH_INSPECTOR_CAPTURE"));
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_PERFORMANCE runs case 17 alone. */
    if (getenv("UI_BENCH_PERFORMANCE")) {
        performance_play(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_MENUS runs case 19 alone. */
    if (getenv("UI_BENCH_MENUS")) {
        menu_open(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_SCENE runs case 16 alone. */
    if (getenv("UI_BENCH_SCENE")) {
        scene_overlays(&b);
        bench_close(&b);
        app_input_shutdown();
        cf_destroy_app();
        return 0;
    }
    /* UI_BENCH_ASSETS runs case 14 alone, UI_BENCH_SPLITTER case 15. */
    bool assets_only = getenv("UI_BENCH_ASSETS") != NULL;
    bool splitter_only = getenv("UI_BENCH_SPLITTER") != NULL;
    bool all = !members_only && !assets_only && !splitter_only;
    if (all) {
        full_layout(&b, false);
        full_layout(&b, true);
        b.studio->prefs.comfortable = false;
        area_text_change(&b);
        properties(&b);
        output_log(&b);
        scene_overlays(&b);
        performance_play(&b);
        menu_open(&b);
    }
    if (all || splitter_only) {
        splitter_drag(&b);
    }
    if (!assets_only && !splitter_only) {
        members(&b);
    }
    if (!members_only && !splitter_only) {
        assets(&b);
    }
    if (all) {
        inspector_tree_case(&b);
    }
    /* Last: it leaves ten thousand objects in the scene. */
    if (all) {
        palette(&b);
        outliner_scroll(&b);
    }
    bench_close(&b);
    if (all) {
        popover_clipping();
    }
    app_input_shutdown();
    cf_destroy_app();
    return 0;
}
