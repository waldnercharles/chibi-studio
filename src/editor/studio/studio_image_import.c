/* Transport for the authored TextureImport form. The behavior owns its phase,
 * options and confirmation; this adapter owns OS selection and SDK plans. */
#include "authoring/image_import_commit.h"
#include "authoring/visual_prepare.h"
#include "editor/studio/studio_internal.h"
#include "platform/cute.h"
#include "support/xalloc.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    SDL_AtomicInt refs;
    SDL_Mutex *mutex;
    bool done;
    char source[4096], error[512];
} image_selection;

struct studio_image_import
{
    image_selection *selection;
    studio_image_import_chooser choose;
    void *choose_context;
    char source[4096], status[512], review[1536];
    image_import_file_plan plan;
    bool ready, ok;
};

static void release_selection(image_selection *selection)
{
    if (selection && SDL_AddAtomicInt(&selection->refs, -1) == 1) {
        SDL_DestroyMutex(selection->mutex);
        free(selection);
    }
}

static void selected(void *context, const char *path, const char *error)
{
    image_selection *selection = context;
    SDL_LockMutex(selection->mutex);
    if (path && strlen(path) >= sizeof selection->source) {
        error = "Selected source path is too long";
        path = NULL;
    }
    snprintf(selection->source, sizeof selection->source, "%s", path ? path : "");
    snprintf(selection->error, sizeof selection->error, "%s", error ? error : "");
    selection->done = true;
    SDL_UnlockMutex(selection->mutex);
    release_selection(selection);
}

#ifndef __EMSCRIPTEN__
static void SDLCALL dialog_selected(void *context, const char *const *files, int filter)
{
    (void)filter;
    selected(context, files ? files[0] : NULL, files ? NULL : SDL_GetError());
}
#endif

static void choose_native(void *context, studio_image_import_completion completion, void *result)
{
    (void)context;
#ifdef __EMSCRIPTEN__
    completion(result, NULL, "Native texture import is unavailable in this browser build");
#else
    (void)completion;
    static const SDL_DialogFileFilter filters[] = { { "PNG and JPEG images",
                                                      "png;jpg;jpeg" } };
    SDL_ShowOpenFileDialog(dialog_selected, result, cf_app_get_window(), filters, 1, NULL, false);
#endif
}

static studio_image_import *state(studio *s)
{
    if (!s->image_import) {
        s->image_import = xcalloc(1, sizeof *s->image_import);
        s->image_import->choose = choose_native;
    }
    return s->image_import;
}

void studio_image_import_set_chooser(studio *s, studio_image_import_chooser chooser, void *context)
{
    studio_image_import *import = state(s);
    import->choose = chooser ? chooser : choose_native;
    import->choose_context = context;
}

void studio_image_import_free(studio_image_import *import)
{
    if (import) {
        release_selection(import->selection);
        image_import_file_plan_free(&import->plan);
        free(import);
    }
}

/* All resident visual catalogs are included. Studio's authoring scene documents
 * are not runtime scene snapshots; their parameter records are excluded here.
 * Full saved-project compilation/export remains the package admission gate. */
static bool measure(
    void *context,
    const image_import_proposal *plans,
    int count,
    authoring_visual_budget *budget,
    char *error,
    size_t size
)
{
    (void)context;
    return authoring_visual_measure(NULL, 0, plans, count, budget, error, size);
}

static void poll(studio_image_import *import)
{
    image_selection *selection = import->selection;
    if (!selection) { return; }
    SDL_LockMutex(selection->mutex);
    bool done = selection->done;
    if (done) {
        snprintf(import->source, sizeof import->source, "%s", selection->source);
        import->ok = selection->source[0] && !selection->error[0];
        snprintf(
            import->status,
            sizeof import->status,
            "%s",
            selection->error[0] ? selection->error
            : import->ok        ? ""
                                : "Selection cancelled."
        );
    }
    SDL_UnlockMutex(selection->mutex);
    if (done) {
        import->selection = NULL;
        release_selection(selection);
    }
}

static pin_value text(arena *storage, const char *value)
{
    size_t length = strlen(value) + 1;
    char *copy = arena_alloc(storage, length);
    memcpy(copy, value, length);
    return (pin_value){ .type = PIN_STR, .str = copy };
}

const char *studio_image_import_run(studio *s, const pin_value *in, arena *storage, pin_value *out)
{
    studio_image_import *import = state(s);
    const char *operation = in[0].str ? in[0].str : "";
    const char *resolution = in[1].str ? in[1].str : "";
    const char *semantic = in[2].str ? in[2].str : "";
    const char *destination = in[3].str ? in[3].str : "";
    poll(import);
    image_import_budget_host host = { .context = s, .measure = measure };
    image_import_file_options options = { 0 };
    bool valid = true;
    if (!strcmp(resolution, "1k")) {
        options.max_side = 1024;
    } else if (!strcmp(resolution, "2k")) {
        options.max_side = 2048;
    } else if (strcmp(resolution, "source")) {
        valid = false;
    }
    if (!strcmp(semantic, "color")) {
        options.semantic = IMAGE_IMPORT_COLOR;
    } else if (!strcmp(semantic, "scalar")) {
        options.semantic = IMAGE_IMPORT_SCALAR;
    } else if (!strcmp(semantic, "normal")) {
        options.semantic = IMAGE_IMPORT_NORMAL;
    } else {
        valid = false;
    }
    if (!strcmp(operation, "cancel")) {
        release_selection(import->selection);
        import->selection = NULL;
        image_import_file_plan_free(&import->plan);
        import->ready = import->ok = false;
        import->source[0] = import->review[0] = import->status[0] = '\0';
    } else if (!strcmp(operation, "choose") && !import->selection) {
        image_import_file_plan_free(&import->plan);
        import->ready = import->ok = false;
        import->review[0] = import->source[0] = '\0';
        if (!studio_editable(s)) {
            snprintf(import->status, sizeof import->status, "Stop the active session before importing.");
        } else {
            image_selection *selection = xcalloc(1, sizeof *selection);
            selection->mutex = SDL_CreateMutex();
            if (!selection->mutex) {
                free(selection);
                snprintf(
                    import->status,
                    sizeof import->status,
                    "Could not create file selection: %s",
                    SDL_GetError()
                );
            } else {
                SDL_SetAtomicInt(&selection->refs, 2);
                import->selection = selection;
                snprintf(import->status, sizeof import->status, "Waiting for an image…");
                import->choose(import->choose_context, selected, selection);
            }
        }
    } else if (
        (!strcmp(operation, "review") || !strcmp(operation, "commit")) &&
        !import->selection
    ) {
        import->ok = false;
        if (!strcmp(operation, "review")) { import->ready = false; }
        if (!valid || !studio_editable(s)) {
            snprintf(import->status, sizeof import->status, "Invalid import options or active runtime session.");
        } else if (!strcmp(operation, "review")) {
            image_import_file_plan_free(&import->plan);
            import->ready = false;
            import->review[0] = '\0';
            if (image_import_file_prepare(
                    s->scene->content_root,
                    import->source,
                    destination,
                    options,
                    host,
                    &import->plan,
                    import->status,
                    sizeof import->status
                )) {
                image_import_plan *image = &import->plan.image;
                authoring_visual_budget *budget = &import->plan.budget;
                import->ready = import->ok = budget->fits;
                snprintf(
                    import->review,
                    sizeof import->review,
                    "%d × %d · %.2f MiB PNG\n"
                    "Visual assets: %.1f / %.0f MiB",
                    image->width,
                    image->height,
                    image->png_bytes / 1048576.0,
                    budget->proposed_bytes / 1048576.0,
                    budget->limit_bytes / 1048576.0
                );
                snprintf(
                    import->status,
                    sizeof import->status,
                    "%s",
                    budget->fits ? "" : "Over the 64 MiB visual budget. Choose a smaller size or remove unused assets."
                );
            }
        } else if (
            !import->ready || !import->plan.destination ||
            strcmp(destination, import->plan.destination) ||
            options.max_side != import->plan.options.max_side ||
            options.semantic != import->plan.options.semantic
        ) {
            snprintf(import->status, sizeof import->status, "Options changed. Review the import again.");
        } else if (
            image_import_file_commit(
                s->scene->content_root,
                &import->plan,
                host,
                import->status,
                sizeof import->status
            )
        ) {
            import->ready = false;
            import->ok = true;
            import->review[0] = '\0';
            app_scene_authoring_refresh_content(s->scene);
            snprintf(import->status, sizeof import->status, "Imported %s", destination);
        } else {
            import->ready = false;
        }
    } else if (strcmp(operation, "poll") && strcmp(operation, "choose")) {
        import->ok = false;
        snprintf(import->status, sizeof import->status, "Unknown image import operation.");
    }
    out[0] = (pin_value){ .type = PIN_BOOL, .b = import->selection != NULL };
    out[1] = (pin_value){ .type = PIN_BOOL, .b = import->ok };
    /* The chooser owns the full path; the form only needs its display name. */
    const char *filename = strrchr(import->source, '/');
    out[2] = text(storage, filename ? filename + 1 : import->source);
    out[3] = text(storage, import->status);
    out[4] = text(storage, import->review);
    return NULL;
}
