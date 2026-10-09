/* Export Game runs the SDK's `chibi export` on the open project, as Unity's
 * Build and Godot's export run their exporter. A loading toast follows it, as
 * Sonner's promise toast, and turns into the result; a finished export's toast
 * offers to show its folder. What the exporter prints goes to a log beside the
 * exports and into Output. Native only: the browser Studio has no SDK to run. */
#include "editor/studio/studio_internal.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_process.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define LAUNCHER "chibi.exe"
#else
#define LAUNCHER "chibi"
#endif
/* Where exports and their log go, inside the project's own build folder. */
#define EXPORT_FOLDER ".chibi/export"
#define EXPORT_LOG "export.log"
#define EXPORTED_PREFIX "Exported "
#define FAILED_PREFIX "Export failed: "
#define EXPORT_NAME_MAX 256
#define EXPORT_URL_MAX (3 * STUDIO_EXPORT_PATH_MAX + sizeof "file:///")

#define FORMAT(buffer, ...)                                                    \
    do {                                                                       \
        int length = snprintf(buffer, sizeof buffer, __VA_ARGS__);             \
        assert(length >= 0 && (size_t)length < sizeof buffer);                 \
    } while (0)

bool studio_export_available(const studio *s)
{
#ifdef __EMSCRIPTEN__
    (void)s;
    return false;
#else
    return !s->hosted && !s->guest && !s->own_project && s->project_root &&
           !s->export_game.process;
#endif
}

/* The launcher beside Studio, as a development build copies it, else the one
 * on PATH. */
static void launcher_path(char *path, size_t size)
{
    const char *base = SDL_GetBasePath();
    assert(base);
    int length = snprintf(path, size, "%s" LAUNCHER, base);
    assert(length >= 0 && (size_t)length < size);
    if (!SDL_GetPathInfo(path, NULL)) {
        snprintf(path, size, LAUNCHER);
    }
}

/* The export's folder name: the project's name, without path separators. */
static void export_name(const studio *s, char *name, size_t size)
{
    const char *project = s->project.name;
    bool plain = project && project[0] && strcmp(project, ".") &&
                 strcmp(project, "..");
    snprintf(name, size, "%s", plain ? project : "Game");
    for (char *c = name; *c; c++) {
        if (*c == '/' || *c == '\\' || *c == ':') {
            *c = '_';
        }
    }
}

static void show(studio *s, kit_toast_type type, const char *title, const char *description, const char *action)
{
    char text[sizeof s->records->text];
    snprintf(text, sizeof text, "%s: %s", title, description);
    studio_record(s, type, text);
    s->export_game.toast = kit_toast(
        s->kit,
        s->export_game.toast,
        &(kit_toast_desc){ .type = type,
                           .title = title,
                           .description = description,
                           .action = action,
                           .duration = type == KIT_TOAST_ERROR ? -1 : 0 }
    );
}

void studio_export_game(studio *s)
{
    if (!studio_export_available(s)) {
        return;
    }
    s->export_game.toast = 0;
    if (studio_unsaved(s)) {
        show(s, KIT_TOAST_WARNING, "Save before exporting", "The export builds the saved project.", NULL);
        return;
    }
    char launcher[STUDIO_EXPORT_PATH_MAX], name[EXPORT_NAME_MAX];
    char output[STUDIO_EXPORT_PATH_MAX], log[STUDIO_EXPORT_PATH_MAX];
    launcher_path(launcher, sizeof launcher);
    export_name(s, name, sizeof name);
    FORMAT(s->export_game.folder, "%s/" EXPORT_FOLDER, s->project_root);
    FORMAT(output, "%s/%s", s->export_game.folder, name);
    FORMAT(log, "%s/" EXPORT_LOG, s->export_game.folder);
    SDL_IOStream *file = SDL_CreateDirectory(s->export_game.folder)
                           ? SDL_IOFromFile(log, "w")
                           : NULL;
    if (!file) {
        show(s, KIT_TOAST_ERROR, "Could not export the game", SDL_GetError(), NULL);
        return;
    }
    const char *args[] = { launcher,   "export", s->project_root,
                           "--output", output,   NULL };
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, file);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    s->export_game.process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_CloseIO(file); /* the exporter writes through its own copy */
    if (!s->export_game.process) {
        char description[sizeof s->records->text];
        snprintf(description, sizeof description, "Cannot run %s: %s", launcher, SDL_GetError());
        show(s, KIT_TOAST_ERROR, "Could not export the game", description, NULL);
        return;
    }
    s->export_game.toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_LOADING,
                           .title = "Exporting game\xe2\x80\xa6",
                           .description = name }
    );
}

/* Copies each line of the exporter's log to Output and returns the text after
 * `prefix` on the last line that starts with it, or NULL. */
static const char *record_log(studio *s, char *log, const char *prefix)
{
    const char *found = NULL;
    for (char *line = log, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = 0;
        }
        if (!line[0]) {
            continue;
        }
        studio_record(s, KIT_TOAST_DEFAULT, line);
        if (!strncmp(line, prefix, strlen(prefix))) {
            found = line + strlen(prefix);
        }
    }
    return found;
}

static void finish(studio *s, int code)
{
    char path[STUDIO_EXPORT_PATH_MAX];
    FORMAT(path, "%s/" EXPORT_LOG, s->export_game.folder);
    char *log = SDL_LoadFile(path, NULL);
    const char *line = log ? record_log(s, log, code ? FAILED_PREFIX : EXPORTED_PREFIX)
                           : NULL;
    if (code == 0) {
        show(s, KIT_TOAST_SUCCESS, "Exported game", line ? line : s->export_game.folder, "Show in Folder");
    } else {
        show(
            s,
            KIT_TOAST_ERROR,
            "Could not export the game",
            line ? line : "See " EXPORT_LOG " in " EXPORT_FOLDER,
            NULL
        );
    }
    SDL_free(log);
    SDL_DestroyProcess(s->export_game.process);
    s->export_game.process = NULL;
}

/* Opens the folder holding the exports, percent-encoded as a file URL. */
static void show_folder(studio *s)
{
    static const char HEX[] = "0123456789ABCDEF";
#ifdef _WIN32
    char url[EXPORT_URL_MAX] = "file:///"; /* then the drive, as C: */
#else
    char url[EXPORT_URL_MAX] = "file://";
#endif
    char *out = url + strlen(url);
    for (const unsigned char *c = (const unsigned char *)s->export_game.folder; *c;
         c++) {
        if (strchr("-._~/:", *c) || (*c >= '0' && *c <= '9') ||
            ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'z')) {
            *out++ = (char)*c;
            continue;
        }
        *out++ = '%';
        *out++ = HEX[*c >> 4];
        *out++ = HEX[*c & 15];
    }
    *out = 0;
    if (!SDL_OpenURL(url)) {
        studio_error(s, "Could not show the export", SDL_GetError());
    }
}

void studio_export_poll(studio *s)
{
    if (s->export_game.toast && kit_toast_action(s->kit, s->export_game.toast)) {
        show_folder(s);
    }
    int code;
    if (s->export_game.process &&
        SDL_WaitProcess(s->export_game.process, false, &code)) {
        finish(s, code);
    }
}

/* A running export finishes on its own, writing its log. */
void studio_export_free(studio *s)
{
    SDL_DestroyProcess(s->export_game.process);
}
