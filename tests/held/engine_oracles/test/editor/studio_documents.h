/* Studio over a project on disk, in a hidden window: the fixture and input
 * helpers the surface checks share (studio_documents.c), and each surface's
 * checks, one file per surface (studio_documents_<surface>.c). */
#ifndef CHIBI_TEST_STUDIO_DOCUMENTS_H
#define CHIBI_TEST_STUDIO_DOCUMENTS_H

#include "editor/workspace/viewport.h"
#include "editor/studio/studio_internal.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>

#define FRAME_SECONDS (1.0 / 60.0)
#define PROJECT "test/fixtures/studio"
/* Weapons' Ignore Shooter function and its automatic component setter. The
   setter's file entry, {id: 10247, type: Set_GameCollider_ignore_entity},
   stays one line wherever the canvas arranges it. */
#define FUNCTION_GRAPH 1
#define SETTER 10247
#define VAULT PROJECT "/content/scenes/Vault.chibi"
#define BANK PROJECT "/content/ui/Bank.rml"

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    engine_viewport *viewport;
    app_scene_authoring *scene;
    studio *studio;
    double time;
} studio_fixture;

/* Opt-in wall times and frame work, including nested shell cases. */
typedef struct
{
    double wall, setup, teardown, pump, scene, studio, present, batches;
    int fixtures, frames, batch_frames;
} studio_test_sample;

studio_test_sample studio_test_begin(const char *name);
void studio_test_end(const char *name, studio_test_sample before);
bool studio_shell_cases(const char *only, bool run);
int studio_documents_select(const char *program, const char *only);
/* CPU-only name validation; list prints the available names. */
int studio_documents_validate(const char *only);
int studio_shell_validate(const char *only);
int studio_shell_select(const char *program, const char *only);

/* The same ordered calls serve listing, validation, focused and full runs.
   No fixture or graphics work happens while listing/validating names. */
#define STUDIO_CASE(name, function)                                            \
    do {                                                                       \
        if (only && !strcmp(only, "list")) {                                   \
            puts(name);                                                        \
            matched = true;                                                    \
        } else if (!only || !strcmp(only, name)) {                             \
            matched = true;                                                    \
            if (run) {                                                         \
                studio_test_sample before = studio_test_begin(name);           \
                function();                                                    \
                studio_test_end(name, before);                                 \
            }                                                                  \
        }                                                                      \
    } while (0)

/* ---------- Fixtures ---------- */

studio_fixture fixture_open(const char *project, const char *scene, const char *behavior);
studio_fixture fixture_new();
studio_fixture scene_open(const char *project, const char *scene, const char *behavior);
studio_fixture vault_open();
studio_fixture studio_project_open();
studio_fixture side_open();
void fixture_free(studio_fixture *f);

/* ---------- Frames ---------- */

void frame_delta(studio_fixture *f, bool reload, double delta_seconds);
void frame(studio_fixture *f, bool reload);
void frames(studio_fixture *f, int count);

/* ---------- Input ---------- */

void key_event_mod(SDL_Scancode key, bool down, SDL_Keymod mod, bool repeat);
void key_event(SDL_Scancode key, bool down);
void tap(SDL_Scancode key);
void text_event(const char *text);
void mouse_move(const studio *s, float x, float y);
void graph_drag_button(const studio *s, float x, float y, bool down);
void press(studio *s, rml_element *el);

/* ---------- Elements ---------- */

rml_element *studio_element(studio *s, const char *selector);
int panel_elements(studio *s, const char *panel, const char *selector, rml_element **found);
int list_rows(studio *s, const char *selector, char *first, size_t size);
int tab_named(const app_scene_authoring *scene, const char *label);

/* ---------- Surfaces ---------- */

void studio_documents_graph_checks();
void studio_documents_expression_geometry_checks();
void studio_documents_shell_checks();
void studio_documents_panel_checks();
void studio_documents_outliner_checks();
/* The fields alone, for --studio-properties. */
void studio_documents_property_field_checks();
void studio_documents_properties_checks();
void studio_documents_assets_checks();
/* Lists and drag (workstream F4): authored and native lists together. */
void studio_documents_list_checks();
/* Output's views, log, places and Calls tree (workstream P4). */
void studio_documents_output_checks();
/* Members (workstream P5). */
void studio_documents_members_checks();
/* The Scene area's header and overlays (workstream C1). */
void studio_documents_scene_checks();
/* The Behavior header, find bars, Text Editor header and Performance
   (workstream C2). */
void studio_documents_chrome_checks();
/* The UI Inspector's tree (workstream I1). */
void studio_documents_inspector_checks();
/* The visual designer's Hierarchy, Details and Library (workstream D1). */
void studio_documents_designer_checks();
/* The palette, pickers and refactor popover (workstream C4). */
void studio_documents_palette_checks();
/* The graph's menus and Properties' Node and Member tabs (workstream G1). */
void studio_documents_graph_chrome_checks();

#endif
