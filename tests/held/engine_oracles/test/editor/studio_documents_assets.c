/* The Assets area. */

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
#include "support/asset_text.h"
#include "support/text_file.h"
#include "support/text_overlay.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"
#include "ui/input_feel.h"

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

/* The index is rebuilt in place even when no dirty-generation change preceded
 * an explicit refresh. Numeric row caches must follow completed rebuilds. */
static void assets_cache_follows_explicit_refresh()
{
    studio_fixture f = studio_project_open();
    studio *s = f.studio;
    s->maximized = STUDIO_AREA_TOP;
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
    s->prefs.assets_tile = STUDIO_ASSETS_LIST;
    s->applied = s->prefs;
    frames(&f, 6);
    reference_index_add_asset(f.scene->asset_index, REFERENCE_ASSET_BEHAVIOR, "QaRefreshCache", NULL, "qa-cache-probe");
    /* The fixture inserts directly into the index; start its cached model
     * afresh, as a completed rebuild would. */
    studio_assets_free(s);
    snprintf(s->assets_filter, sizeof s->assets_filter, "QaRefreshCache");
    s->ui_schedule.wake = true;
    frames(&f, 3);
    char first[4096];
    int rows = list_rows(s, "#assets-list > .list-spacer", first, sizeof first);
    bool found = rows == 2 && strstr(first, "Behaviors");
    /* Nothing is selected, so neither the group's header nor its row is. */
    test_check(found && !s->assets_selected[0] && !studio_element(s, "#assets-list [data-selected]"), "Assets marks no row selected while no asset is, its group headers included");
    unsigned generation = f.scene->asset_index_generation;
    uint64_t revision = f.scene->asset_index_revision;
    s->ui_schedule.next = s->time + 1;
    s->active_until = 0;
    uint64_t emitted = s->ui_schedule.frames;
    assert(app_scene_authoring_refresh_asset_index(f.scene) == 0);
    frame(&f, false);
    rows = list_rows(s, "#assets-list > .list-spacer", first, sizeof first);
    if (!found || rows || generation != f.scene->asset_index_generation ||
        f.scene->asset_index_revision != revision + 1 ||
        s->ui_schedule.frames != emitted + 1) {
        printf(
            "  Assets cache: found=%d rows=%d generation=%u/%u "
            "revision=%llu/%llu emissions=%llu/%llu first='%s'\n",
            found,
            rows,
            generation,
            f.scene->asset_index_generation,
            (unsigned long long)revision,
            (unsigned long long)f.scene->asset_index_revision,
            (unsigned long long)emitted,
            (unsigned long long)s->ui_schedule.frames,
            first
        );
    }
    test_check(
        found && !rows && generation == f.scene->asset_index_generation &&
            f.scene->asset_index_revision == revision + 1 &&
            s->ui_schedule.frames == emitted + 1,
        "Assets discards cached rows after an explicit index rebuild with "
        "unchanged dirty generation"
    );
    fixture_free(&f);
}

/* Tiles of the grid's List that lie in its box: what it shows. */
static int tiles_in_view(studio *s, rml_element *list)
{
    rml_vec2 at = rml_element_absolute_offset(list, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(list, RML_BOX_BORDER);
    rml_element *tiles[512];
    int count = test_query_all(rml_backend_context(s->backend), "#assets-list [data-ui=Tile]", tiles, 512);
    int shown = 0;
    for (int i = 0; i < count && i < 512; i++) {
        rml_vec2 tile = rml_element_absolute_offset(tiles[i], RML_BOX_BORDER);
        shown += rml_element_is_visible(tiles[i], true) && tile.y >= at.y &&
                 tile.y < at.y + size.y;
    }
    return shown;
}

/* Turns the wheel `notches` over `el`, as SDL reports a notch. */
static void wheel_over(studio *s, rml_element *el, float notches)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float ratio = studio_pixel_ratio(s);
    mouse_move(s, (at.x + size.x / 2) / ratio, (at.y + size.y / 2) / ratio);
    SDL_Event wheel = { .type = SDL_EVENT_MOUSE_WHEEL };
    wheel.wheel.windowID = SDL_GetWindowID(cf_app_get_window());
    wheel.wheel.mouse_x = (at.x + size.x / 2) / ratio * s->prefs.ui_scale;
    wheel.wheel.mouse_y = (at.y + size.y / 2) / ratio * s->prefs.ui_scale;
    wheel.wheel.y = -notches;
    assert(SDL_PushEvent(&wheel));
}

/* Scrolled by the wheel, typed into and cleared, the grid still shows
   tiles over its whole view, the lines it keeps in their slots among
   them. */
static void scrolled_grid_shows_tiles()
{
    studio_fixture f = scene_open("test/fixtures/studio-workspace", "test/fixtures/studio-workspace/content/scenes/Sandbox.kdl", "Movement");
    studio *s = f.studio;
    /* The index rebuilds as Studio first opens: add to it afterwards. */
    frames(&f, 4);
    for (int i = 0; i < 480; i++) {
        char name[32];
        snprintf(name, sizeof name, "QaScroll%03d", i);
        reference_index_add_asset(f.scene->asset_index, REFERENCE_ASSET_BEHAVIOR, name, NULL, name);
    }
    /* Inserted directly: start the cached model afresh, as a rebuild would. */
    studio_assets_free(s);
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
    s->prefs.assets_tile = STUDIO_ASSETS_TILE;
    s->applied = s->prefs;
    frames(&f, 6);
    rml_element *list = studio_element(s, "#assets-list");
    int top = list ? tiles_in_view(s, list) : 0;
    if (list) {
        wheel_over(s, list, 3);
    }
    frames(&f, 40);
    float scroll = list ? rml_element_scroll_top(list) : 0;
    int scrolled = list ? tiles_in_view(s, list) : 0;
    snprintf(s->assets_filter, sizeof s->assets_filter, "e");
    frames(&f, 3);
    int typed = list ? tiles_in_view(s, list) : 0;
    s->assets_filter[0] = '\0';
    frames(&f, 3);
    int cleared = list ? tiles_in_view(s, list) : 0;
    bool shows = top > 20 && scroll > 0 && scrolled > 20 && typed > 20 && cleared > 20;
    if (!shows) {
        printf("  grid tiles in view: %d, scrolled %.0f px %d, typed %d, cleared %d\n", top, scroll, scrolled, typed, cleared);
    }
    test_check(shows, "a grid scrolled by the wheel, filtered and cleared shows tiles over its whole view");
    fixture_free(&f);
}

void studio_documents_assets_checks()
{
    assets_cache_follows_explicit_refresh();
    scrolled_grid_shows_tiles();
}
