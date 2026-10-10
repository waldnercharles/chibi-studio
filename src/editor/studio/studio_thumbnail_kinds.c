#include "assets/ui_document.h"
#include "authoring/ui_loader.h"
#include "assets/field_parameters.h"
/* What each kind of asset looks like in its thumbnail:
 * - a scene, its start state a few frames in, framed as the Scene view
 *   frames a scene it opens, its objects fitted, widened to what its
 *   GameCamera sees, as Godot's scene previews fit their content; its
 *   streamed terrain fills the frame first, and it is lit at the Scene
 *   view's scale, then shrinks;
 * - a prototype, one instance fitted on the project's stage (its lighting
 *   and material worlds), as Unity's prefabs;
 * - a material, a disc of it on that stage, a sphere in perspective scenes;
 * - a shape, filled, as previewShader draws it;
 * - a field, its values and iso-contour, as previewField bakes them;
 * - a ui document, itself on Storybook's backdrop, as the Canvas shows it;
 * - a stylesheet, the colors it declares;
 * - a graph asset, a mini-map of its nodes and wires.
 * Each shows its open document's unsaved edits: graphs and scenes draw
 * their working copies, prototypes are working copies, and ui documents
 * read their text overlays. Scenes and prototypes run in a world of their
 * own that copies the authored behaviors and prototypes, so nothing
 * authored or running changes; it copies them a few at a time and catches
 * up with edits before each picture. Materials and shapes draw as the
 * shader catalog holds them. */
#include "assets/scene_document.h"
#include "authoring/preview_source.h"
#include "authoring/prototype_preview.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_thumbnails.h"
#include "editor/workspace/viewport.h"
#include "graph/document/serialize.h"
#include "render/field_image.h"
#include "render/scene_renderer.h"
#include "render/shader_preview.h"
#include "render/ui.h"
#include "runtime/camera.h"
#include "runtime/component_catalog.h"
#include "runtime/declared_components.h"
#include "runtime/material_field.h"
#include "runtime/materials.h"
#include "support/array.h"
#include "support/text_file.h"
#include "support/xalloc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stage pictures stand on, as Unreal's thumbnails' dark backdrop. */
#define STAGE 0x202126ffu
/* Storybook's light and dark backgrounds, as the Canvas uses. */
#define BACKDROP_LIGHT 0xf4f4f5ffu
#define BACKDROP_DARK 0x1c1c1fffu
/* A field's world extent across its thumbnail. */
#define FIELD_EXTENT 768.0f
/* Frames a scene or prototype runs before its picture, so generated
   content appears. */
#define SCENE_FRAMES 2
/* What a step may spend copying the world or streaming terrain. */
#define STEP_SECONDS 0.003
/* A scene whose terrain streams longer than this shows what it has, then
   draws again once the rest has streamed. */
#define STREAM_CAP_SECONDS 2.0
/* Draws of a scene's picture, the last kept: HRC feeds each frame's light
   back into the next, so bounced light needs a few. */
#define LIGHT_PASSES 3
/* The least a prototype's picture spans, in world units, so a small one
   stays small rather than filling it. */
#define PROTOTYPE_MIN_SPAN 48.0f
/* The unlit scene color a prototype's stage adds, so surfaces that no
   light reaches still show. */
#define PROTOTYPE_AMBIENT 0.35f
/* HRC lights each pixel, so lighting looks as the Scene view shows it only
   where a pixel covers as much world as the Scene view's do: a scene draws
   where a pixel covers at most this many world units, then shrinks to the
   thumbnail. At a tile's own 128 pixels a pixel of Sandbox holds 25 units
   and its lit tunnels go black. */
#define SCENE_UNITS_PER_PIXEL 3.0f
/* The largest side a scene's picture draws at before shrinking. */
#define SCENE_MAX_SIDE 1024
#define FRAME_SECONDS (1.0 / 60.0)
#define SWATCH_MAX 12
/* A widget's thumbnail frames its content to fill this much of it. */
#define DOCUMENT_FILL 0.86f
/* The dp a ui document's square screen spans, and the least a widget's
   does. */
#define DOCUMENT_SCREEN 720.0f
#define DOCUMENT_MIN_SCREEN 48.0f

/* A world copying the authored behaviors, systems and prototypes, made a
   few at a time: behaviors by stage, then systems and prototypes. */
typedef struct
{
    game_prototype_preview *preview;
    game_prototype_registry registry;
    int stage, next; /* what it copies next; WORLD_STAGES once it has all */
    uint64_t edits;  /* the asset index generation whose edits it holds */
    bool failed;
    char error[256];
} thumb_world;

/* A scene renderer and the viewport framing it, at one canvas side. A
   scene's view also keeps its picture at that side and the picture's halves
   on the way down to a thumbnail's. */
typedef struct
{
    app_scene_renderer *renderer;
    engine_viewport *viewport;
    int side;
    CF_Canvas picture;
    CF_Canvas *halves; /* support/array.h: side / 2, side / 4, ... */
} thumb_view;

/* A scene running toward its picture: its terrain streams for a share of
   each step, then it runs a frame a step and draws. */
typedef struct
{
    game_scene_document *scene; /* NULL while none runs */
    game_prototype_preview_scene_entity *entities;
    engine_viewport_view view; /* what it frames */
    int side;                  /* its picture's, before it shrinks */
    double started;            /* Studio's time when it launched */
    int frame;    /* the next to run; -1 while terrain streams first */
    int pass;     /* the light pass it draws next */
    bool partial; /* it drew before its terrain had streamed */
} scene_run;

struct studio_thumb_renderers
{
    thumb_world world;
    thumb_view views[STUDIO_THUMB_SIZE_COUNT]; /* tiles and large previews keep their own */
    thumb_view scene; /* scenes draw at their own side */
    scene_run run;
    render_ui *ui;
    ecs_world *ui_world;
    studio_sources sources; /* Studio's own ui assets bind its data, as in the Canvas */
};

int studio_thumb_side(reference_asset_kind kind, int side)
{
    return kind == REFERENCE_ASSET_FIELD ? side / 2 : side;
}

int studio_thumb_cost(reference_asset_kind kind)
{
    switch (kind) {
        case REFERENCE_ASSET_BEHAVIOR:
        case REFERENCE_ASSET_EVENT:
        case REFERENCE_ASSET_LIBRARY:
        case REFERENCE_ASSET_SYSTEM: return 0;
        case REFERENCE_ASSET_UI: return 1;
        case REFERENCE_ASSET_FIELD: return 2;
        case REFERENCE_ASSET_MATERIAL:
        case REFERENCE_ASSET_SHAPE: return 3;
        case REFERENCE_ASSET_PROTOTYPE: return 4;
        default: return 5;
    }
}

static CF_Color color_of(uint32_t rgba)
{
    return cf_make_color_rgba(rgba >> 24, rgba >> 16 & 255, rgba >> 8 & 255, rgba & 255);
}

static bool has_suffix(const char *text, const char *suffix)
{
    size_t length = strlen(text), size = strlen(suffix);
    return length >= size && !strcmp(text + length - size, suffix);
}

/* ---------- Graph assets ---------- */

static graph *largest_graph(const behavior_asset *asset)
{
    graph *largest = asset->graph;
    for (int i = 0; i < acount(asset->graphs); i++) {
        if (acount(asset->graphs[i]->nodes) > acount(largest->nodes)) {
            largest = asset->graphs[i];
        }
    }
    return largest;
}

/* The working copy of a graph asset with unsaved edits, or NULL. */
static const behavior_asset *unsaved_graph(studio *s, const char *name)
{
    game_preview_definition *d = game_preview_find_definition(s->scene->live, name);
    return d && d->unsaved ? d->working : NULL;
}

/* A copy of the graph asset's working copy, or its file's. */
static behavior_asset *graph_asset(studio *s, const studio_thumb_job *job)
{
    game_preview_definition *definition = game_preview_find_definition(s->scene->live, job->name);
    if (definition && definition->working_presentation) {
        return authoring_source_presentation_take_view(
            authoring_source_presentation_clone(definition->working_presentation)
        );
    }
    const behavior_asset *working = unsaved_graph(s, job->name);
    if (working) {
        return behavior_asset_clone(working);
    }
    char *text = text_file_read(job->path);
    char error[256];
    behavior_asset *asset = scene_graph_source_path(s->scene, text, job->path, error, sizeof error);
    free(text);
    return asset;
}

static bool render_graph(studio *s, const studio_thumb_job *job, CF_Canvas canvas)
{
    /* A copy: building the mini-map arranges its automatic nodes. */
    behavior_asset *asset = graph_asset(s, job);
    if (!asset) {
        return false;
    }
    studio_minimap map;
    float side = (float)job->side;
    studio_minimap_build(largest_graph(asset), side, &map);
    behavior_asset_free(asset);
    bool drawn = acount(map.boxes) > 0;
    if (drawn) {
        cf_draw_push();
        cf_draw_projection(cf_ortho_2d(side / 2, side / 2, side, side));
        studio_minimap_draw(&map, side);
        cf_render_to(canvas, true);
        cf_draw_pop();
    }
    studio_minimap_free(&map);
    return drawn;
}

/* ---------- Stylesheets ---------- */

static int hex_digit(char c)
{
    return isdigit((unsigned char)c)  ? c - '0'
         : isxdigit((unsigned char)c) ? tolower((unsigned char)c) - 'a' + 10
                                      : -1;
}

/* A #rgb, #rgba, #rrggbb or #rrggbbaa color at `at`, as 0xRRGGBBAA; false
 * for anything else, as an id selector. */
static bool hex_color(const char *at, uint32_t *out, int *length)
{
    int n = 0;
    while (hex_digit(at[n]) >= 0) {
        n++;
    }
    if ((n != 3 && n != 4 && n != 6 && n != 8) ||
        isalnum((unsigned char)at[n]) || at[n] == '-' || at[n] == '_') {
        return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < n; i++) {
        int digit = hex_digit(at[i]);
        value = n <= 4 ? value << 8 | (uint32_t)(digit * 17)
                       : value << 4 | (uint32_t)digit;
    }
    bool alpha = n == 4 || n == 8;
    *out = alpha ? value : value << 8 | 0xffu;
    *length = n;
    return true;
}

/* The distinct colors a stylesheet's declarations name, in order. */
static int sheet_colors(const char *text, uint32_t *colors, int max)
{
    int count = 0;
    bool in_value = false;
    for (const char *p = text; *p && count < max; p++) {
        if (*p == ':' || *p == ';' || *p == '{' || *p == '}') {
            in_value = *p == ':';
            continue;
        }
        uint32_t color;
        int length;
        if (*p != '#' || !in_value || !hex_color(p + 1, &color, &length)) {
            continue;
        }
        p += length;
        bool seen = false;
        for (int i = 0; i < count; i++) {
            seen |= colors[i] == color;
        }
        if (!seen) {
            colors[count++] = color;
        }
    }
    return count;
}

/* Up to four swatches a row, centered on the Canvas's backdrop, as ui
   documents show. */
static bool render_stylesheet(const studio_thumb_job *job, CF_Canvas canvas)
{
    char *text = text_file_read(job->path);
    uint32_t colors[SWATCH_MAX];
    int count = text ? sheet_colors(text, colors, SWATCH_MAX) : 0;
    free(text);
    if (!count) {
        return false;
    }
    float side = (float)job->side;
    cf_canvas_set_clear_color(canvas, color_of(job->light ? BACKDROP_LIGHT : BACKDROP_DARK));
    int columns = count < 4 ? count : 4, rows = (count + columns - 1) / columns;
    float cell = side * 0.8f / 4, gap = cell * 0.12f;
    float left = (side - columns * cell) / 2, top = (side + rows * cell) / 2;
    cf_draw_push();
    cf_draw_projection(cf_ortho_2d(side / 2, side / 2, side, side));
    for (int i = 0; i < count; i++) {
        float x = left + (float)(i % columns) * cell,
              y = top - (float)(i / columns + 1) * cell;
        CF_Aabb box = { { x + gap, y + gap }, { x + cell - gap, y + cell - gap } };
        cf_draw_push_color(color_of(0x3a3c44ffu));
        cf_draw_box_rounded_fill(
            (CF_Aabb){ { box.min.x - 1, box.min.y - 1 },
                       { box.max.x + 1, box.max.y + 1 } },
            gap
        );
        cf_draw_pop_color();
        cf_draw_push_color(color_of(colors[i]));
        cf_draw_box_rounded_fill(box, gap);
        cf_draw_pop_color();
    }
    cf_render_to(canvas, true);
    cf_draw_pop();
    return true;
}

/* ---------- UI documents ---------- */

/* A square centered on the content, which fills DOCUMENT_FILL of it, and
   never smaller than the canvas. */
static void frame_content(const float bounds[4], float region[4])
{
    float span = fmaxf(bounds[2] - bounds[0], bounds[3] - bounds[1]);
    float half = fmaxf(span / DOCUMENT_FILL, 1) / 2;
    float x = (bounds[0] + bounds[2]) / 2, y = (bounds[1] + bounds[3]) / 2;
    float frame[4] = { x - half, y - half, x + half, y + half };
    memcpy(region, frame, sizeof frame);
}

/* The document on the Canvas's backdrop, as laid out on a 720 dp square
   screen; a smaller one lays out again on a screen of its size, so a widget
   shows large and sharp, and the square centers on its content. */
static bool render_document(studio *s, studio_thumb_renderers *r, const studio_thumb_job *job, CF_Canvas canvas)
{
    if (!r->ui) {
        r->ui = authoring_ui_new(s->scene->content_root, s->own_project ? studio_sources_new() : NULL);
        r->ui_world = ecs_world_new();
    }
    if (s->own_project) {
        studio_sources_show(s, &r->sources, render_ui_data(r->ui));
    }
    ui_world world = { .ecs = r->ui_world, .page = app_scene_authoring_page(s->scene) };
    render_ui_set_overlay(r->ui, job->name, job->light ? "light" : "dark");
    render_ui_set_reference_height(r->ui, 0);
    render_ui_update(r->ui, &world, job->side, job->side, 0);
    float bounds[4], region[4];
    bool shows = render_ui_overlay_bounds(r->ui, bounds) &&
                 !render_ui_error(r->ui)[0];
    float screen = shows ? fmaxf(bounds[2] - bounds[0], bounds[3] - bounds[1]) *
                               DOCUMENT_SCREEN / DOCUMENT_FILL
                         : DOCUMENT_SCREEN;
    if (shows && screen < DOCUMENT_SCREEN * 0.9f) {
        render_ui_set_reference_height(r->ui, fmaxf(screen, DOCUMENT_MIN_SCREEN));
        render_ui_update(r->ui, &world, job->side, job->side, 0);
        shows = render_ui_overlay_bounds(r->ui, bounds);
    }
    if (shows) {
        frame_content(bounds, region);
        cf_canvas_set_clear_color(canvas, color_of(job->light ? BACKDROP_LIGHT : BACKDROP_DARK));
        cf_render_to(canvas, true);
        render_ui_composite_region(r->ui, canvas, region);
    }
    render_ui_set_overlay(r->ui, job->name, NULL);
    return shows;
}

/* ---------- Fields ---------- */

static bool render_field(studio *s, const studio_thumb_job *job, CF_Canvas canvas)
{
    int side = job->side;
    const material_fields *fields = game_materials_fields(
        s->scene->live->runtime->materials
    );
    float *values = xmalloc((size_t)side * side * sizeof *values);
    char error[256];
    field_grid grid = { side, side, FIELD_EXTENT / (float)side, 0, 0 };
    bool baked = material_field_bake_text(fields, job->name, NULL, 0, grid, values, NULL, error, sizeof error);
    if (baked) {
        CF_Pixel *pixels = xmalloc((size_t)side * side * sizeof *pixels);
        render_field_image(values, side, side, grid.cell_size, pixels);
        cf_texture_update(cf_canvas_get_target(canvas), pixels, side * side * (int)sizeof *pixels);
        free(pixels);
    }
    free(values);
    return baked;
}

/* ---------- Shapes ---------- */

static bool render_shape(const studio_thumb_job *job, CF_Canvas canvas)
{
    shader_error error;
    render_shader_preview
        *preview = render_shader_preview_find(job->name, job->side, job->side, &error);
    if (!preview) {
        return false;
    }
    float side = (float)job->side;
    cf_draw_push();
    cf_draw_projection(cf_ortho_2d(0, 0, side, side));
    bool drawn = render_shader_preview_draw(preview, 0, NULL);
    cf_render_to(canvas, true);
    cf_draw_pop();
    render_shader_preview_free(preview);
    return drawn;
}

/* ---------- Worlds ---------- */

static bool shader_domain(behavior_domain domain)
{
    return domain == BEHAVIOR_DOMAIN_MATERIAL || domain == BEHAVIOR_DOMAIN_SHAPE ||
           domain == BEHAVIOR_DOMAIN_SHADER_LIBRARY;
}

/* Libraries before the materials and fields that call them, and those
   before behaviors, as content loads (assets/behavior_loader.c). */
static int stage_of(behavior_domain domain)
{
    return domain == BEHAVIOR_DOMAIN_EVENT ? 2
         : domain == BEHAVIOR_DOMAIN_FIELD ? 1
                                           : 0;
}

/* The stages behaviors copy in (stage_of), then one for the systems and
   prototypes. */
#define WORLD_STAGES 4

static double now_seconds()
{
    return (double)cf_get_ticks() / (double)cf_get_tick_frequency();
}

static void world_free(thumb_world *w)
{
    game_prototype_preview_free(w->preview);
    *w = (thumb_world){ 0 };
}

/* Canonical modules and declaration snapshots are shared, while the thumbnail
 * world owns its services, instances and simulation state. Invalid drafts use
 * the last applied generation. */
static bool copy_sources(studio *s, thumb_world *w)
{
    game_preview_world *live = s->scene->live;
    const authoring_sources *working = game_preview_sources_working(live);
    const authoring_sources *applied = game_preview_sources_applied(live);
    if (!working && !applied) {
        return true;
    }
    bool use_applied = !working || !authoring_sources_valid(working);
    int result = game_preview_sources_mirror(
        game_prototype_preview_live(w->preview),
        live,
        use_applied,
        w->error,
        sizeof w->error
    );
    w->failed = result != 0;
    return !w->failed;
}

/* Registering a prototype replaces a copy that differs and keeps an equal
   one. */
static void copy_prototypes(studio *s, thumb_world *w)
{
    for (int i = 0; i < acount(s->scene->prototypes); i++) {
        game_prototype_preview_register_prototype(
            w->preview,
            s->scene->prototypes[i].prototype
        );
    }
}

/* Copies what the world lacks, behaviors by stage, until `deadline`; true
   once it holds everything. Shaders stay in the catalog the scene draws
   with, untouched. */
static bool world_fill(studio *s, thumb_world *w, double deadline)
{
    game_preview_world *live = s->scene->live;
    while (w->stage < WORLD_STAGES - 1) {
        if (w->next >= game_preview_definition_count(live)) {
            w->stage++;
            w->next = 0;
            continue;
        }
        game_preview_definition *d = game_preview_definition_at(live, w->next++);
        behavior_domain domain = d->working->domain;
        if (d->working_presentation || shader_domain(domain) ||
            stage_of(domain) != w->stage) {
            continue;
        }
        game_prototype_preview_register_behavior(w->preview, d->name, d->working);
        if (now_seconds() >= deadline) {
            return false;
        }
    }
    if (w->stage == WORLD_STAGES - 1) {
        if (!copy_sources(s, w)) {
            return false;
        }
        copy_prototypes(s, w);
        w->stage++;
    }
    return true;
}

/* Brings the copies up to the authored edits: a changed behavior compiles
   again, and a changed system or prototype replaces its copy. */
static bool world_refresh(studio *s, thumb_world *w)
{
    game_preview_world *live = s->scene->live;
    for (int stage = 0; stage < WORLD_STAGES - 1; stage++) {
        for (int i = 0; i < game_preview_definition_count(live); i++) {
            game_preview_definition *d = game_preview_definition_at(live, i);
            behavior_domain domain = d->working->domain;
            char message[256];
            if (!d->working_presentation && !shader_domain(domain) &&
                stage_of(domain) == stage) {
                game_prototype_preview_update_behavior(w->preview, d->name, d->working, true, message, sizeof message);
            }
        }
    }
    if (!copy_sources(s, w)) {
        return false;
    }
    copy_prototypes(s, w);
    return true;
}

/* The world once it holds the authored state, or NULL while it copies a
   step's share of it. */
static game_prototype_preview *world_ready(studio *s, studio_thumb_renderers *r)
{
    thumb_world *w = &r->world;
    uint64_t edits = s->scene->asset_index_generation;
    if (!w->preview) {
        w->registry = app_component_registry();
        w->preview = game_prototype_preview_new(&w->registry);
        w->edits = edits;
    }
    if (w->failed && w->edits == edits) {
        return NULL;
    }
    w->failed = false;
    if (w->stage < WORLD_STAGES && !world_fill(s, w, now_seconds() + STEP_SECONDS)) {
        if (w->failed) {
            w->edits = edits;
        }
        return NULL;
    }
    if (w->edits != edits) {
        bool refreshed = world_refresh(s, w);
        w->edits = edits;
        if (!refreshed) {
            return NULL;
        }
    }
    return w->preview;
}

static bool run_frames(game_prototype_preview *preview)
{
    for (int frame = 0; frame <= SCENE_FRAMES; frame++) {
        if (game_prototype_preview_tick(preview, frame * FRAME_SECONDS) != 0) {
            return false;
        }
    }
    return true;
}

/* Settles the world's lighting for one still frame: HRC gathers radiance
   over frames, which one picture lacks, and adaptive resolution judges a
   running frame rate. Unlit surfaces show at least `ambient`. */
static void still_lighting(ecs_world *world, float ambient)
{
    ecs_component_id component = GAME_COMPONENT_GameHRC2D.id;
    ecs_query query;
    if (!ecs_world_component(world, component) ||
        ecs_query_begin(world, &query, &component, 1) != ECS_OK) {
        return;
    }
    ecs_entity entity;
    while (ecs_query_next(&query, &entity)) {
        GameHRC2D *lighting = GAME_GameHRC2D_get(world, entity);
        lighting->radiance_hysteresis = lighting->bounce_hysteresis = 0;
        lighting->adaptive = false;
        lighting->ambient = fmaxf(lighting->ambient, ambient);
    }
    ecs_query_end(&query);
}

static void view_free(thumb_view *view)
{
    engine_viewport_free(view->viewport);
    app_scene_renderer_free(view->renderer);
    if (view->picture.id) {
        cf_destroy_canvas(view->picture);
    }
    for (int i = 0; i < acount(view->halves); i++) {
        cf_destroy_canvas(view->halves[i]);
    }
    afree(view->halves);
    *view = (thumb_view){ 0 };
}

/* The view at `side`, made again when its side changed. */
static thumb_view *view_at(thumb_view *view, int side)
{
    if (view->viewport && view->side != side) {
        view_free(view);
    }
    if (!view->viewport) {
        *view = (thumb_view){ app_scene_renderer_new(),
                              engine_viewport_new(side, side),
                              side };
    }
    engine_viewport_reset_camera(view->viewport);
    return view;
}

static thumb_view *view_for(studio_thumb_renderers *r, const studio_thumb_job *job)
{
    return view_at(&r->views[job->size], job->side);
}

/* Widens the view to at least `span` world units. */
static void widen_view(engine_viewport *viewport, float span)
{
    engine_viewport_view view;
    engine_viewport_get_view(viewport, &view);
    view.width = fmaxf(view.width, span);
    view.height = fmaxf(view.height, span);
    engine_viewport_set_view(viewport, &view);
}

static bool draw_world(thumb_view *view, game_prototype_preview *preview, CF_Canvas canvas)
{
    ecs_world *world = game_prototype_preview_world(preview);
    const game_particles *particles = game_prototype_preview_particles(preview);
    const engine *runtime = game_prototype_preview_runtime(preview);
    engine_viewport_camera camera;
    engine_viewport_get_camera(view->viewport, &camera);
    cf_draw_push();
    cf_draw_projection(
        cf_ortho_2d(camera.center_x, camera.center_y, camera.span_width, camera.span_height)
    );
    bool drawn;
    if (engine_viewport_perspective(view->viewport)) {
        render_scene3d_camera camera3d;
        engine_viewport_get_camera3d(view->viewport, &camera3d);
        drawn = app_scene_renderer_draw3d(
            view->renderer,
            world,
            particles,
            &camera3d,
            view->viewport,
            runtime,
            (render_skip){ 0 }
        );
    } else {
        drawn = app_scene_renderer_draw(
            view->renderer,
            world,
            particles,
            view->viewport,
            runtime,
            (render_skip){ 0 }
        );
    }
    cf_render_to(canvas, true);
    cf_draw_pop();
    return drawn;
}

/* ---------- Prototypes and materials ---------- */

/* The project's lighting and material worlds: the first prototype holding
   GameHRC2D, as Neon Swarm's HRC, or NULL. */
static const game_prototype *stage_prototype(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->prototypes); i++) {
        const game_prototype *p = scene->prototypes[i].prototype;
        if (prototype_has_component_id(p, GAME_COMPONENT_GameHRC2D.id)) {
            return p;
        }
    }
    return NULL;
}

const char *studio_thumb_stage(studio *s)
{
    const game_prototype *stage = stage_prototype(s->scene);
    return stage ? stage->name : NULL;
}

static game_prototype_preview_scene_entity entity_of(const game_prototype *prototype)
{
    return (game_prototype_preview_scene_entity){ .prototype = prototype,
                                                  .parent = -1 };
}

/* One instance on the project's stage, fitted to the square, which spans at
   least PROTOTYPE_MIN_SPAN; false when nothing of it draws, as for a camera
   or a world's settings. */
static bool render_instance(
    studio *s,
    studio_thumb_renderers *r,
    const game_prototype *prototype,
    const studio_thumb_job *job,
    CF_Canvas canvas
)
{
    game_prototype_preview *preview = r->world.preview;
    const game_prototype *stage = stage_prototype(s->scene);
    game_prototype_preview_scene_entity entities[2] = { entity_of(prototype) };
    int count = 1;
    if (stage && stage != prototype) {
        entities[count++] = entity_of(stage);
    }
    char error[256];
    game_prototype_preview_stop(preview);
    bool drawn = game_prototype_preview_launch_scene(preview, entities, count, error, sizeof error) ==
                     0 &&
                 run_frames(preview);
    ecs_world *world = game_prototype_preview_world(preview);
    ecs_entity entity = game_prototype_preview_entity(preview);
    thumb_view *view = view_for(r, job);
    engine_viewport_set_perspective(view->viewport, drawn && GAME_GameGeometry3D_has(world, entity));
    drawn = drawn && fit_scene_view(view->viewport, world, &entity, 1);
    if (drawn) {
        widen_view(view->viewport, PROTOTYPE_MIN_SPAN);
        still_lighting(world, PROTOTYPE_AMBIENT);
        drawn = draw_world(view, preview, canvas);
    }
    game_prototype_preview_stop(preview);
    return drawn;
}

static bool render_prototype(studio *s, studio_thumb_renderers *r, const studio_thumb_job *job, CF_Canvas canvas)
{
    scene_prototype_asset *asset = find_prototype(s->scene, job->name);
    return asset && render_instance(s, r, asset->prototype, job, canvas);
}

/* A material's swatch: a disc of it, or in a perspective scene a sphere, as
   the scene renderer draws it. */
static bool render_material(studio *s, studio_thumb_renderers *r, const studio_thumb_job *job, CF_Canvas canvas)
{
    char text[512], error[256];
    if (s->scene->perspective) {
        snprintf(
            text,
            sizeof text,
            "name \"Swatch\"\ncomponents {\n    GAME.GameGeometry3D "
            "depth=100.0 height=100.0 primitive=%d width=100.0\n    "
            "GAME.GameShader material=\"%s\" parameters=\"\" shape=\"\"\n    "
            "(object)GAME.GameTransform\n}\n",
            (int)GAME_PRIMITIVE_SPHERE,
            job->name
        );
    } else {
        snprintf(
            text,
            sizeof text,
            "name \"Swatch\"\ncomponents {\n    GAME.GameAppearance "
            "color=0xffffffff height=100.0 shape=\"Circle\" width=100.0\n    "
            "GAME.GameMaterial scene=0xffffffff\n    GAME.GameShader "
            "material=\"%s\" parameters=\"\" shape=\"\"\n    "
            "(object)GAME.GameTransform\n}\n",
            job->name
        );
    }
    game_prototype *swatch = game_prototype_load(text, &r->world.registry, error, sizeof error);
    bool drawn = swatch && render_instance(s, r, swatch, job, canvas);
    game_prototype_free(swatch);
    return drawn;
}

/* ---------- Scenes ---------- */

/* The scene's objects as its file lists them, with the authored prototypes. */
static game_prototype_preview_scene_entity *scene_entities(studio *s, const game_scene_document *scene)
{
    game_prototype_preview_scene_entity *entities = NULL;
    for (int i = 0; i < acount(scene->entities); i++) {
        const game_scene_entity *e = &scene->entities[i];
        scene_prototype_asset *asset = find_prototype(s->scene, e->prototype);
        if (!asset) {
            afree(entities);
            return NULL;
        }
        apush(
            entities,
            ((game_prototype_preview_scene_entity){
                .prototype = asset->prototype,
                .parent = e->parent,
                .components = e->components,
                .behaviors = (const char *const *)e->behaviors,
                .behavior_count = acount(e->behaviors),
                .own_behaviors = e->own_behaviors })
        );
    }
    return entities;
}

static void run_end(studio_thumb_renderers *r)
{
    if (r->run.scene) {
        game_prototype_preview_stop(r->world.preview);
    }
    afree(r->run.entities);
    game_scene_document_free(r->run.scene);
    r->run = (scene_run){ 0 };
}

/* The scene at `path` as its open document holds it with unsaved edits, or
   NULL. */
static game_scene_document *unsaved_scene(studio *s, const char *path)
{
    app_scene_authoring *scene = s->scene;
    for (int i = 0; i < acount(scene->tabs); i++) {
        const scene_document_tab *tab = &scene->tabs[i];
        if (tab->kind != APP_EDITOR_DOCUMENT_SCENE || !tab->path ||
            strcmp(tab->path, path)) {
            continue;
        }
        bool current = scene->current_scene_tab_key && tab->key &&
                       !strcmp(scene->current_scene_tab_key, tab->key);
        if (current) {
            return scene->scene_dirty ? scene_authoring_document(scene) : NULL;
        }
        char error[256];
        return tab->scene_dirty && tab->scene_value
                 ? game_scene_document_load_value(tab->scene_value, error, sizeof error)
                 : NULL;
    }
    return NULL;
}

/* Widens the view to take in `other` too. */
static void widen_to(engine_viewport *viewport, const engine_viewport_view *other)
{
    engine_viewport_view v;
    engine_viewport_get_view(viewport, &v);
    float left = fminf(v.center_x - v.width / 2, other->center_x - other->width / 2);
    float right = fmaxf(v.center_x + v.width / 2, other->center_x + other->width / 2);
    float bottom = fminf(v.center_y - v.height / 2, other->center_y - other->height / 2);
    float top = fmaxf(v.center_y + v.height / 2, other->center_y + other->height / 2);
    v.center_x = (left + right) / 2;
    v.center_y = (bottom + top) / 2;
    v.width = right - left;
    v.height = top - bottom;
    engine_viewport_set_view(viewport, &v);
}

/* The side a scene spanning `span` world units draws at: the thumbnail's,
   doubled while a pixel would hold more than SCENE_UNITS_PER_PIXEL. */
static int scene_side(float span, int side)
{
    while (side * 2 <= SCENE_MAX_SIDE && span / (float)side > SCENE_UNITS_PER_PIXEL) {
        side *= 2;
    }
    return side;
}

/* What the scene shows, as the Scene view frames a scene it opens: its
   objects fitted, widened to what its camera sees; a perspective scene's
   camera frames it alone. Terrain streams into that, the cameras' own tiles
   under the stream budget too, so no step prepares them all at once. */
static void frame_scene(studio_thumb_renderers *r, const studio_thumb_job *job)
{
    game_prototype_preview *preview = r->world.preview;
    ecs_world *world = game_prototype_preview_world(preview);
    thumb_view *view = view_for(r, job);
    bool perspective = r->run.scene->perspective;
    engine_viewport_set_perspective(view->viewport, perspective);
    int count = game_prototype_preview_entity_count(preview);
    ecs_entity *objects = xmalloc((size_t)count * sizeof *objects);
    for (int i = 0; i < count; i++) {
        objects[i] = game_prototype_preview_entity_at(preview, i);
    }
    bool fitted = fit_scene_view(view->viewport, world, objects, count);
    free(objects);
    engine_viewport_view camera;
    if (game_camera_view(world, game_prototype_preview_runtime(preview), &camera) > 0) {
        if (fitted && !perspective) {
            widen_to(view->viewport, &camera);
        } else {
            engine_viewport_set_view(view->viewport, &camera);
        }
    }
    engine_viewport_get_view(view->viewport, &r->run.view);
    engine_viewport_camera seen;
    engine_viewport_get_camera(view->viewport, &seen);
    r->run.side = scene_side(fmaxf(seen.span_width, seen.span_height), job->side);
    material_stream_view stream = { seen.center_x,
                                    -seen.center_y,
                                    0.5f * fmaxf(seen.span_width, seen.span_height),
                                    0 };
    game_materials *materials = game_prototype_preview_live(preview)->runtime->materials;
    game_materials_set_stream_views(materials, &stream, 1);
    game_materials_set_stream_deferred(materials, true);
}

/* Loads the scene, its unsaved edits or its file, and launches it. */
static studio_thumb_step scene_start(studio *s, studio_thumb_renderers *r, const studio_thumb_job *job)
{
    game_prototype_preview *preview = world_ready(s, r);
    if (!preview) {
        return r->world.failed ? STUDIO_THUMB_BLANK : STUDIO_THUMB_BUSY;
    }
    char error[256];
    game_scene_document *scene = unsaved_scene(s, job->path);
    if (!scene) {
        char *text = text_file_read(job->path);
        scene = text ? game_scene_document_load(text, error, sizeof error) : NULL;
        free(text);
    }
    game_prototype_preview_scene_entity *entities = scene && acount(scene->entities)
                                                      ? scene_entities(s, scene)
                                                      : NULL;
    r->run = (scene_run){ .scene = scene,
                          .entities = entities,
                          .started = s->time,
                          .frame = -1 };
    game_prototype_preview_stop(preview);
    bool running = entities &&
                   game_prototype_preview_configure_physics(preview, scene->physics) == 0 &&
                   game_prototype_preview_launch_scene(preview, entities, acount(entities), error, sizeof error) ==
                       0;
    if (!running) {
        run_end(r);
        return STUDIO_THUMB_BLANK;
    }
    frame_scene(r, job);
    return STUDIO_THUMB_BUSY;
}

/* Streams terrain for a step's share; true once none waits, or when it
   cannot stream. */
static bool stream(game_prototype_preview *preview)
{
    game_materials *materials = game_prototype_preview_live(preview)->runtime->materials;
    double deadline = now_seconds() + STEP_SECONDS;
    const char *error = NULL;
    do {
        error = game_materials_prepare(materials);
    } while (!error && game_materials_streaming(materials) && now_seconds() < deadline);
    return error || !game_materials_streaming(materials);
}

/* Copies `source` over all of `destination`. */
static void blit(CF_Canvas source, CF_Canvas destination)
{
    cf_draw_canvas(source, (CF_V2){ 0 }, (CF_V2){ 2, 2 });
    cf_render_to(destination, true);
}

/* Shrinks the view's picture into the thumbnail by halves: a bilinear
   blit at half size averages each 2x2 pixels, so nothing aliases. */
static void shrink(thumb_view *view, CF_Canvas canvas, int side)
{
    cf_draw_push();
    cf_draw_TSR_absolute((CF_V2){ 0 }, (CF_V2){ 1, 1 }, 0);
    cf_draw_projection(cf_ortho_2d(0, 0, 2, 2));
    cf_draw_push_filter(CF_DRAW_FILTER_LINEAR);
    CF_Canvas source = view->picture;
    int i = 0;
    for (int half = view->side / 2; half > side; half /= 2, i++) {
        if (i == acount(view->halves)) {
            apush(view->halves, cf_make_canvas(cf_canvas_defaults(half, half)));
        }
        blit(source, view->halves[i]);
        source = view->halves[i];
    }
    blit(source, canvas);
    cf_draw_pop_filter();
    cf_draw_pop();
}

/* One of the scene's light passes, a step each, at its picture's side:
   light that bounces reaches surfaces through the passes before, so only
   the last shrinks into the thumbnail and no half lit picture shows. */
static bool draw_pass(studio_thumb_renderers *r, const studio_thumb_job *job, CF_Canvas canvas)
{
    game_prototype_preview *preview = r->world.preview;
    thumb_view *view = view_at(&r->scene, r->run.side);
    if (!view->picture.id) {
        view->picture = cf_make_canvas(cf_canvas_defaults(view->side, view->side));
        cf_canvas_set_clear_color(view->picture, color_of(STAGE));
    }
    engine_viewport_set_perspective(view->viewport, r->run.scene->perspective);
    engine_viewport_set_view(view->viewport, &r->run.view);
    still_lighting(game_prototype_preview_world(preview), 0);
    bool drawn = draw_world(view, preview, view->picture);
    if (drawn && r->run.pass == LIGHT_PASSES - 1) {
        shrink(view, canvas, job->side);
    }
    return drawn;
}

/* A scene streams the terrain it frames, a share a step, then runs a frame
   a step and draws a light pass a step. Terrain still streaming at
   STREAM_CAP_SECONDS draws as far as it has come, and again once it has all
   streamed. */
static studio_thumb_step render_scene(
    studio *s,
    studio_thumb_renderers *r,
    const studio_thumb_job *job,
    CF_Canvas canvas
)
{
    if (!r->run.scene) {
        return scene_start(s, r, job);
    }
    game_prototype_preview *preview = r->world.preview;
    if (r->run.frame < 0) {
        bool capped = s->time - r->run.started >= STREAM_CAP_SECONDS;
        r->run.frame = stream(preview) || capped ? 0 : -1;
        return STUDIO_THUMB_BUSY;
    }
    if (r->run.frame <= SCENE_FRAMES) {
        bool ran = game_prototype_preview_tick(preview, r->run.frame++ * FRAME_SECONDS) ==
                   0;
        if (!ran) {
            run_end(r);
        }
        return ran ? STUDIO_THUMB_BUSY : STUDIO_THUMB_BLANK;
    }
    bool waiting = game_materials_streaming(
        game_prototype_preview_live(preview)->runtime->materials
    );
    if (waiting && r->run.partial && !stream(preview)) {
        return STUDIO_THUMB_BUSY;
    }
    waiting &= !r->run.partial;
    if (!draw_pass(r, job, canvas)) {
        run_end(r);
        return STUDIO_THUMB_BLANK;
    }
    if (++r->run.pass < LIGHT_PASSES) {
        return STUDIO_THUMB_BUSY;
    }
    r->run.pass = 0;
    if (waiting) {
        r->run.partial = true;
        return STUDIO_THUMB_PARTIAL;
    }
    run_end(r);
    return STUDIO_THUMB_DRAWN;
}

/* ---------- Dispatch ---------- */

static bool render_still(studio *s, studio_thumb_renderers *r, const studio_thumb_job *job, CF_Canvas canvas)
{
    switch (job->kind) {
        case REFERENCE_ASSET_PROTOTYPE:
            return render_prototype(s, r, job, canvas);
        case REFERENCE_ASSET_MATERIAL:
            return render_material(s, r, job, canvas);
        case REFERENCE_ASSET_SHAPE: return render_shape(job, canvas);
        case REFERENCE_ASSET_FIELD: return render_field(s, job, canvas);
        case REFERENCE_ASSET_UI:
            return has_suffix(job->path, UI_STYLE_EXTENSION)
                     ? render_stylesheet(job, canvas)
                     : render_document(s, r, job, canvas);
        default: return render_graph(s, job, canvas);
    }
}

bool studio_thumb_needs_world(reference_asset_kind kind)
{
    return kind == REFERENCE_ASSET_SCENE || kind == REFERENCE_ASSET_PROTOTYPE ||
           kind == REFERENCE_ASSET_MATERIAL;
}

studio_thumb_step studio_thumb_render(
    studio *s,
    studio_thumb_renderers **renderers,
    const studio_thumb_job *job,
    CF_Canvas canvas
)
{
    if (!*renderers) {
        *renderers = xcalloc(1, sizeof **renderers);
    }
    studio_thumb_renderers *r = *renderers;
    cf_canvas_set_clear_color(canvas, color_of(STAGE));
    if (job->kind == REFERENCE_ASSET_SCENE) {
        return render_scene(s, r, job, canvas);
    }
    if (studio_thumb_needs_world(job->kind) && !world_ready(s, r)) {
        return r->world.failed ? STUDIO_THUMB_BLANK : STUDIO_THUMB_BUSY;
    }
    return render_still(s, r, job, canvas) ? STUDIO_THUMB_DRAWN : STUDIO_THUMB_BLANK;
}

/* ---------- Unsaved edits ---------- */

static char *unsaved_scene_text(studio *s, const char *path)
{
    game_scene_document *scene = unsaved_scene(s, path);
    char *text = scene ? game_scene_document_save(
                             scene,
                             project_components()
                         )
                       : NULL;
    game_scene_document_free(scene);
    return text;
}

static char *unsaved_prototype_text(studio *s, const char *name)
{
    const scene_prototype_asset *asset = find_prototype(s->scene, name);
    return asset && asset->dirty ? game_prototype_save(asset->prototype) : NULL;
}

static char *unsaved_graph_text(studio *s, const char *name)
{
    const behavior_asset *working = unsaved_graph(s, name);
    return working ? behavior_asset_save(working) : NULL;
}

char *studio_thumb_unsaved(studio *s, const reference_asset *a)
{
    switch (a->kind) {
        case REFERENCE_ASSET_SCENE: return unsaved_scene_text(s, a->path);
        case REFERENCE_ASSET_PROTOTYPE:
            return unsaved_prototype_text(s, a->name);
        case REFERENCE_ASSET_SYSTEM:
        case REFERENCE_ASSET_UI: return NULL;
        default: return unsaved_graph_text(s, a->name);
    }
}

void studio_thumb_cancel(studio_thumb_renderers *r)
{
    run_end(r);
}

void studio_thumb_renderers_free(studio_thumb_renderers *r)
{
    if (!r) {
        return;
    }
    run_end(r);
    world_free(&r->world);
    for (int i = 0; i < STUDIO_THUMB_SIZE_COUNT; i++) {
        view_free(&r->views[i]);
    }
    view_free(&r->scene);
    render_ui_free(r->ui);
    studio_sources_free(&r->sources);
    ecs_world_free(r->ui_world);
    free(r);
}
