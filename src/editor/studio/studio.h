#ifndef CHIBI_EDITOR_STUDIO_H
#define CHIBI_EDITOR_STUDIO_H

#include "editor/workspace/viewport.h"
#include "editor/workspace/scene_authoring.h"

#include <stdint.h>

/* The RmlUi Studio workspace: a full-window retained document. It reads and
 * edits the scene authoring model directly and shows the Scene render target
 * inside its viewport area. */
typedef struct studio studio;

/* `project_root` is the open project's directory; the caller keeps it alive. */
studio *studio_new(app_scene_authoring *scene, engine_viewport *viewport, const char *project_root);
void studio_free(studio *s);
/* Process input, emit the UI, draw the viewport and queue the composited frame.
 * Call after the scene frame; the host presents with cf_app_draw_onto_screen. */
void studio_frame(studio *s, double delta_seconds);
/* Seconds Studio can wait for input before its next frame: none just after
 * input, while a scene runs or while its UI changes from frame to frame.
 * Call once per frame, after studio_frame. */
double studio_idle_time(studio *s);
/* The project root the user chose to open, owned by the caller, or NULL. */
char *studio_take_project(studio *s);
/* Call once a frame, `requested` when the user asked to close Studio. True
 * once Studio may close: at once without unsaved edits, else after a toast
 * naming them is confirmed. */
bool studio_quit(studio *s, bool requested);
#ifndef __EMSCRIPTEN__
/* The project roots Studio opened before, most recent first, owned
 * (support/array.h). */
char **studio_recent_projects();
void studio_recent_projects_free(char **roots);
/* Puts `root` first among them; only projects a person opens belong there. */
void studio_remember_project(const char *root);
#endif
/* Shows an error that stays until dismissed. */
void studio_error(studio *s, const char *title, const char *message);
/* Fills the workspace with the graph canvas without changing saved preferences. */
void studio_maximize_graph(studio *s);

/* Collaboration checkpoint and ephemeral presence. The JSON returned by
 * capture is owned by the caller. Passing a NULL presence removes that peer. */
char *studio_collab_capture(studio *s);
bool studio_collab_restore(studio *s, const char *json);
void studio_collab_receive(studio *s, const char *client, const char *name, uint32_t color, const char *json);
bool studio_collab_apply(studio *s, const char *json, char *error, size_t size);

#endif
