/* Graph navigation in the Behavior header (Unreal): Back and Forward through
 * the graphs visited and their breadcrumbs, across behaviors, with Alt+Left
 * and Alt+Right or mouse buttons 4 and 5; and a breadcrumb of the behavior
 * and the calls entered to reach the open graph, each of which returns to its
 * graph. The Text Editor's Go to Definition joins the same history, as VS
 * Code's Go Back crosses editors: it records the place it leaves and the
 * one it lands on (workstream Q). */
#include "editor/studio/studio_graph_internal.h"
#include "support/array.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum
{
    VISIT_LIMIT = 64,
};

static studio_visit here(const studio *s)
{
    const app_scene_authoring *scene = s->scene;
    studio_visit visit = { .kind = scene->graph_document_kind,
                           .graph_id = sg_active_graph(s)->graph_id };
    snprintf(visit.document, sizeof visit.document, "%s", scene->graph_document_id);
    if (visit.kind == APP_EDITOR_DOCUMENT_SYSTEM && scene->system_path) {
        snprintf(visit.path, sizeof visit.path, "%s", scene->system_path);
    }
    return visit;
}

static bool same_visit(const studio_visit *a, const studio_visit *b)
{
    if (a->kind == APP_EDITOR_DOCUMENT_UI || b->kind == APP_EDITOR_DOCUMENT_UI) {
        return a->kind == b->kind && !strcmp(a->place.path, b->place.path) &&
               a->place.part == b->place.part && a->place.offset == b->place.offset;
    }
    return a->kind == b->kind && a->graph_id == b->graph_id &&
           !strcmp(a->document, b->document);
}

/* Makes `visit` the current one unless it is, dropping the visits ahead of
   it (browser history). */
static void push(studio_graph *g, const studio_visit *visit)
{
    if (acount(g->visits) && same_visit(&g->visits[g->visit], visit)) {
        return;
    }
    asetlen(g->visits, acount(g->visits) ? g->visit + 1 : 0);
    apush(g->visits, *visit);
    if (acount(g->visits) > VISIT_LIMIT) {
        adel(g->visits, 0);
    }
    g->visit = acount(g->visits) - 1;
}

static void see(studio_graph *g, const studio_visit *visit)
{
    asetlen(g->shown, 0);
    apush(g->shown, *visit);
}

/* Records a changed graph and its breadcrumb. Source visits synchronize
   the displayed graph separately without adding a graph history entry. */
void sg_nav_track(studio *s)
{
    studio_graph *g = &s->graph;
    studio_visit visit = here(s);
    if (acount(g->shown) && same_visit(&g->shown[0], &visit)) {
        return;
    }
    const editor *e = sg_graph_editor(s);
    visit.caller_count = acount(e->graph_callers);
    assert(visit.caller_count <= ED_CALLER_LIMIT);
    if (visit.caller_count) {
        memcpy(
            visit.callers,
            e->graph_callers,
            (size_t)visit.caller_count * sizeof *visit.callers
        );
    }
    see(g, &visit);
    push(g, &visit);
}

static bool open_visit(studio *s, const studio_visit *visit)
{
    if (visit->kind == APP_EDITOR_DOCUMENT_UI) {
        if (!studio_code_open_place(s, &visit->place)) {
            return false;
        }
        /* A source visit can also change the graph shown in Both. Record
           that view without making a new visit that discards Forward. */
        if (studio_graph_active(s)) {
            studio_visit graph = here(s);
            see(&s->graph, &graph);
        }
        return true;
    }
    app_scene_authoring *scene = s->scene;
    bool open = scene->graph_document_id &&
                scene->graph_document_kind == (app_editor_document_kind)visit->kind &&
                !strcmp(scene->graph_document_id, visit->document);
    if (!open) {
        finish_transform_edit(scene, false);
        int opened = visit->kind == APP_EDITOR_DOCUMENT_SYSTEM
                       ? app_scene_authoring_open_system(scene, visit->path)
                       : app_scene_authoring_open_behavior(scene, visit->document);
        if (opened != 0) {
            return false;
        }
    }
    editor *e = sg_graph_editor(s);
    if (!studio_graph_active(s) || editor_open_graph(e, visit->graph_id) != 0) {
        return false;
    }
    ed_restore_callers(e, visit->callers, visit->caller_count);
    see(&s->graph, visit);
    return true;
}

static bool can_step(const studio *s, int direction)
{
    int next = s->graph.visit + direction;
    return next >= 0 && next < acount(s->graph.visits);
}

/* Visits that can no longer open, such as a deleted function, drop out. */
static void step(studio *s, int direction)
{
    studio_graph *g = &s->graph;
    if (studio_graph_active(s)) {
        sg_nav_track(s);
    }
    int at = g->visit + direction;
    while (at >= 0 && at < acount(g->visits)) {
        if (open_visit(s, &g->visits[at])) {
            g->visit = at;
            return;
        }
        adel(g->visits, at);
        if (at < g->visit) {
            g->visit--;
        }
        at += direction < 0 ? -1 : 0;
    }
}

/* ---------- The Text Editor's places (workstream Q) ---------- */

void studio_nav_jump(studio *s, const ui_code_place *from, const ui_code_place *to)
{
    studio_graph *g = &s->graph;
    if (studio_graph_active(s)) {
        sg_nav_track(s);
    }
    studio_visit visits[2] = { { .kind = APP_EDITOR_DOCUMENT_UI, .place = *from },
                               { .kind = APP_EDITOR_DOCUMENT_UI, .place = *to } };
    push(g, &visits[0]);
    push(g, &visits[1]);
}

bool studio_nav_can_step(const studio *s, int direction)
{
    return can_step(s, direction);
}

void studio_nav_step(studio *s, int direction)
{
    step(s, direction);
}

/* Alt+Left and Alt+Right, or mouse buttons 4 and 5, over the Behavior area. */
void sg_nav_keys(studio *s)
{
    studio_graph *g = &s->graph;
    bool over = s->hover_area == g->area ||
                (s->hover_area < 0 && s->focus_area == g->area);
    if (!over || kit_wants_keyboard(s->kit) || kit_wants_keyboard(g->kit)) {
        s->nav_button = 0;
        return;
    }
    if (s->nav_button) {
        step(s, s->nav_button);
        s->nav_button = 0;
    }
    for (int i = 0; i < acount(s->keys); i++) {
        SDL_Keycode key = s->keys[i].key;
        if ((s->keys[i].mod & SDL_KMOD_ALT) && (key == SDLK_LEFT || key == SDLK_RIGHT)) {
            step(s, key == SDLK_RIGHT ? 1 : -1);
        }
    }
}
