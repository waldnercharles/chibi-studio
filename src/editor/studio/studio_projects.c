/* The projects Studio opens: those opened recently, most recent first, then
 * the workspace's own and those in its projects directory, and any folder
 * File ▸ Open Project… picks. Opening one rebuilds the editor, so the
 * application takes the chosen root after the frame. */
#include "assets/project.h"
#include "editor/studio/studio_internal.h"
#include "platform/cute.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <yyjson.h>
#ifndef __EMSCRIPTEN__
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* In the browser without a local server the page keeps projects in IndexedDB
   and lists them with the samples not yet copied; opening one reloads onto it. */
EM_JS(char *, web_project_list, (), {
    return stringToNewUTF8(Module['chibiProjects']());
})
EM_JS(void, web_open_project, (const char *root), {
    Module['chibiOpenProject'](UTF8ToString(root));
})

EM_JS(void, web_open_created_project, (const char *root), {
    Module['chibiOpenProject'](UTF8ToString(root), false);
})

EM_JS(char *, web_project_open_status, (), {
    return stringToNewUTF8(Module['chibiProjectOpenStatus']());
})

EM_JS(bool, web_project_create_available, (), {
    return typeof Module['chibiCreateProject'] == 'function';
})

EM_JS(char *, web_create_project, (const char *name, int version), {
    const result = Module['chibiCreateProject'](UTF8ToString(name), version);
    return stringToNewUTF8(typeof result == 'string' ? result : JSON.stringify(result));
})

/* The page asks before it unloads while this is set (web_shell.html). */
EM_JS(void, web_set_unsaved, (bool unsaved), {
    Module['chibiUnsaved'] = unsaved;
})

EM_JS(void, web_remember_project, (const char *root), {
    if (typeof Module['chibiRememberProject'] == 'function') {
        Module['chibiRememberProject'](UTF8ToString(root));
    }
})

static void add_web_projects(studio *s)
{
    char *json = web_project_list();
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    free(json);
    size_t i, count;
    yyjson_val *project;
    yyjson_arr_foreach(yyjson_doc_get_root(doc), i, count, project)
    {
        apush(
            s->projects,
            ((studio_project){
                .root = xstrdup(yyjson_get_str(yyjson_obj_get(project, "root"))),
                .name = xstrdup(yyjson_get_str(yyjson_obj_get(project, "name"))) })
        );
    }
    yyjson_doc_free(doc);
}
#endif

static bool create_project(studio *s);

static int project_index(const studio *s, const char *root)
{
    for (int i = 0; i < acount(s->projects); i++) {
        if (!strcmp(s->projects[i].root, root)) {
            return i;
        }
    }
    return -1;
}

static void add_project(studio *s, const char *manifest)
{
    chibi_project project = { 0 };
    char error[256];
    if (chibi_project_open(&project, manifest, error, sizeof error) == 0 &&
        project_index(s, project.root) < 0) {
        apush(
            s->projects,
            ((studio_project){ .root = xstrdup(project.root),
                               .name = xstrdup(project.name) })
        );
    }
    chibi_project_free(&project);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(((const studio_project *)a)->name, ((const studio_project *)b)->name);
}

void studio_projects_load(studio *s)
{
#ifdef __EMSCRIPTEN__
    if (web_projects_available()) {
        add_web_projects(s);
        qsort(s->projects, acount(s->projects), sizeof *s->projects, compare_names);
        return;
    }
#else
    char **recent = studio_recent_projects();
    for (int i = 0; i < acount(recent); i++) {
        add_project(s, recent[i]);
    }
    studio_recent_projects_free(recent);
#endif
    int recents = acount(s->projects);
    const char *workspace = resource_workspace();
    if (!workspace) {
        char manifest[4096];
        snprintf(manifest, sizeof manifest, "%s/" CHIBI_PROJECT_MANIFEST, s->project_root);
        add_project(s, manifest);
        return;
    }
    char manifest[4096];
    snprintf(manifest, sizeof manifest, "%s/" CHIBI_PROJECT_MANIFEST, workspace);
    add_project(s, manifest);
    char projects[4096];
    snprintf(projects, sizeof projects, "%s/projects", workspace);
    DIR *dir = opendir(projects);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        snprintf(
            manifest,
            sizeof manifest,
            "%s/projects/%s/" CHIBI_PROJECT_MANIFEST,
            workspace,
            entry->d_name
        );
        add_project(s, manifest);
    }
    if (dir) {
        closedir(dir);
    }
    qsort(s->projects + recents, acount(s->projects) - recents, sizeof *s->projects, compare_names);
}

void studio_projects_free(studio *s)
{
    for (int i = 0; i < acount(s->projects); i++) {
        free(s->projects[i].root);
        free(s->projects[i].name);
    }
    afree(s->projects);
    free(s->open_project);
}

bool studio_project_current(const studio *s, int index)
{
    return !strcmp(s->projects[index].root, s->project_root);
}

static void request(studio *s, int index)
{
#ifdef __EMSCRIPTEN__
    if (web_projects_available()) {
        web_open_project(s->projects[index].root);
        return;
    }
#endif
    free(s->open_project);
    s->open_project = xstrdup(s->projects[index].root);
}

/* The unsaved documents by name, as "Level", "Level and Player" or "Level,
   Player and 2 more", so a confirmation says what it would lose. */
static void unsaved_names(studio *s, char *out, size_t size)
{
    app_scene_authoring *scene = s->scene;
    const char *names[2] = { 0 };
    int count = 0;
    for (int i = 0; i < acount(scene->tabs); i++) {
        if (scene_tabs_dirty(scene, &scene->tabs[i])) {
            if (count < 2) {
                names[count] = scene->tabs[i].label;
            }
            count++;
        }
    }
    char title[128];
    if (!count) {
        names[count++] = studio_scene_title(scene, title, sizeof title);
    }
    if (count == 1) {
        snprintf(out, size, "%s", names[0]);
    } else if (count == 2) {
        snprintf(out, size, "%s and %s", names[0], names[1]);
    } else {
        snprintf(out, size, "%s, %s and %d more", names[0], names[1], count - 2);
    }
}

/* ---------- Unsaved changes ---------- */

#define UNSAVED_LOST "Your changes will be lost if you don't save them."
#define UNSAVED_HELD "Stop the scene to save them, or discard them."

/* The tab the pending action closes, else -1 for every document. */
static int pending_tab(studio *s)
{
    studio_unsaved_prompt *prompt = &s->unsaved_prompt;
    return prompt->action == STUDIO_UNSAVED_CLOSE_TAB
             ? studio_tab_index(s, prompt->tab)
             : -1;
}

/* Shows the prompt's question about `names`: "Save Level before quitting?"
   with Save first and Discard second, and the toast's close button
   cancelling. While a running scene keeps a pending document from saving,
   it says how to save it. */
static void show_unsaved(studio *s, const char *names)
{
    studio_unsaved_prompt *prompt = &s->unsaved_prompt;
    char title[448];
    snprintf(title, sizeof title, "Save %s before %s?", names, prompt->before);
    prompt->toast = kit_toast(
        s->kit,
        prompt->toast,
        &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                           .title = title,
                           .description = studio_unsaved_held(s, pending_tab(s))
                                            ? UNSAVED_HELD
                                            : UNSAVED_LOST,
                           .action = "Save",
                           .cancel = "Discard",
                           .duration = -1 }
    );
}

/* What every action that would lose unsaved edits asks, as Save / Don't Save
   / Cancel does on every desktop. One prompt shows at a time; a new question
   updates it in place. */
static void ask_unsaved(studio *s, studio_unsaved_action action, const char *names, const char *before)
{
    studio_unsaved_prompt *prompt = &s->unsaved_prompt;
    prompt->action = action;
    snprintf(prompt->before, sizeof prompt->before, "%s", before);
    show_unsaved(s, names);
}

void studio_open_project(studio *s, int index)
{
    if (!studio_unsaved(s)) {
        request(s, index);
        return;
    }
    char names[256], before[192];
    unsaved_names(s, names, sizeof names);
    snprintf(before, sizeof before, "opening %s", s->projects[index].name);
    s->unsaved_prompt.project = index;
    ask_unsaved(s, STUDIO_UNSAVED_OPEN_PROJECT, names, before);
}

void studio_close_tab(studio *s, int index)
{
    if (!scene_tabs_can_close(s->scene, index)) {
        return;
    }
    scene_document_tab *tab = &s->scene->tabs[index];
    if (!scene_tabs_dirty(s->scene, tab)) {
        studio_discard_tab(s, index);
        return;
    }
    snprintf(s->unsaved_prompt.tab, sizeof s->unsaved_prompt.tab, "%s", tab->key);
    ask_unsaved(s, STUDIO_UNSAVED_CLOSE_TAB, tab->label, "closing");
}

/* Saves what the pending action would lose: the tab it closes, else every
   document. True when that left nothing unsaved; a failure's toast says why. */
static bool save_pending(studio *s, int tab)
{
    if (s->unsaved_prompt.action == STUDIO_UNSAVED_CLOSE_TAB) {
        return tab >= 0 && studio_save_tab(s, tab);
    }
    return studio_save_all(s);
}

/* Goes ahead with the action the prompt held. */
static void proceed(studio *s, int tab)
{
    switch (s->unsaved_prompt.action) {
        case STUDIO_UNSAVED_OPEN_PROJECT:
            request(s, s->unsaved_prompt.project);
            break;
        case STUDIO_UNSAVED_CLOSE_TAB:
            if (tab >= 0) {
                studio_discard_tab(s, tab);
            }
            break;
        case STUDIO_UNSAVED_QUIT: s->unsaved_prompt.quit = true; break;
        case STUDIO_UNSAVED_NONE: break;
    }
}

/* True while the pending action would still lose unsaved edits: the tab it
   closes, else any document. */
static bool pending_unsaved(studio *s, int tab)
{
    if (s->unsaved_prompt.action == STUDIO_UNSAVED_CLOSE_TAB) {
        return tab >= 0 && scene_tabs_dirty(s->scene, &s->scene->tabs[tab]);
    }
    return studio_unsaved(s);
}

/* Asks again about what Save left unsaved, such as a scene the running
   scene kept from saving, so the action never goes ahead as if saved: Save
   once the scene stops, or Discard, still does. */
static void ask_again(studio *s, int tab)
{
    char names[256];
    if (tab >= 0) {
        snprintf(names, sizeof names, "%s", s->scene->tabs[tab].label);
    } else {
        unsaved_names(s, names, sizeof names);
    }
    show_unsaved(s, names);
}

/* Takes the prompt's answer. Edits saved some other way while it asks
   leave nothing to ask about, so it goes away unanswered. */
static void answer_unsaved(studio *s)
{
    studio_unsaved_prompt *prompt = &s->unsaved_prompt;
    if (!prompt->toast) {
        return;
    }
    int tab = pending_tab(s);
    bool save = kit_toast_action(s->kit, prompt->toast);
    bool discard = !save && kit_toast_cancel(s->kit, prompt->toast);
    if (!save && !discard && pending_unsaved(s, tab)) {
        return;
    }
    kit_toast_dismiss(s->kit, prompt->toast);
    prompt->toast = 0;
    if (save && !save_pending(s, tab) && pending_unsaved(s, tab)) {
        ask_again(s, tab);
        return;
    }
    if (save || discard) {
        proceed(s, tab);
    }
    prompt->action = STUDIO_UNSAVED_NONE;
}

#ifndef __EMSCRIPTEN__
/* What a folder is picked for: a project to open, or where New Project
   creates one. */
enum
{
    PICK_PROJECT,
    PICK_LOCATION,
    PICK_COUNT,
};

/* The folder dialog answers on a thread of its own; the frame takes what it
   left, each owned. */
static void *picked_folder[PICK_COUNT], *picker_error;

static void SDLCALL folder_picked(void *purpose, const char *const *folders, int)
{
    if (!folders) {
        free(SDL_SetAtomicPointer(&picker_error, xstrdup(SDL_GetError())));
        return;
    }
    if (folders[0]) {
        free(SDL_SetAtomicPointer(&picked_folder[(intptr_t)purpose], xstrdup(folders[0])));
    }
}

static void pick_folder(int purpose, const char *start)
{
    SDL_ShowOpenFolderDialog(folder_picked, (void *)(intptr_t)purpose, cf_app_get_window(), start, false);
}

void studio_pick_project()
{
    pick_folder(PICK_PROJECT, NULL);
}

static void fail(studio *s, const char *title, const char *error)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = title, .description = error });
}

static bool open_folder(chibi_project *project, const char *folder, char *error, size_t size)
{
    char manifest[4096];
    snprintf(manifest, sizeof manifest, "%s/" CHIBI_PROJECT_MANIFEST, folder);
    struct stat info;
    if (stat(manifest, &info) != 0) {
        const char *slash = strrchr(folder, '/');
        snprintf(
            error,
            size,
            "%s has no " CHIBI_PROJECT_MANIFEST ".",
            slash && slash[1] ? slash + 1 : folder
        );
        return false;
    }
    return chibi_project_open(project, manifest, error, size) == 0;
}

void studio_open_project_folder(studio *s, const char *folder)
{
    chibi_project project = { 0 };
    char error[512];
    if (!open_folder(&project, folder, error, sizeof error)) {
        fail(s, "Could not open the project", error);
        return;
    }
    int index = project_index(s, project.root);
    if (index < 0) {
        apush(
            s->projects,
            ((studio_project){ .root = xstrdup(project.root),
                               .name = xstrdup(project.name) })
        );
        index = acount(s->projects) - 1;
    }
    chibi_project_free(&project);
    if (!studio_project_current(s, index)) {
        studio_open_project(s, index);
    }
}

static void take_picked(studio *s)
{
    char *error = SDL_SetAtomicPointer(&picker_error, NULL);
    if (error) {
        fail(s, "Could not show the folder picker", error);
        free(error);
    }
    char *folder = SDL_SetAtomicPointer(&picked_folder[PICK_PROJECT], NULL);
    if (folder) {
        studio_open_project_folder(s, folder);
        free(folder);
    }
    char *location = SDL_SetAtomicPointer(&picked_folder[PICK_LOCATION], NULL);
    if (location) {
        studio_new_project *dialog = &s->new_project;
        snprintf(dialog->folder, sizeof dialog->folder, "%s", location);
        dialog->error[0] = '\0';
        free(location);
    }
}
#endif

void studio_projects_frame(studio *s)
{
#ifdef __EMSCRIPTEN__
    web_set_unsaved(studio_unsaved(s));
#else
    /* The window's own Studio opens projects, not a guest's. */
    if (!s->hosted) {
        take_picked(s);
    }
#endif
    answer_unsaved(s);
}

bool studio_quit(studio *s, bool requested)
{
    if (s->unsaved_prompt.quit) {
        return true;
    }
    if (!requested) {
        return false;
    }
    if (!studio_unsaved(s)) {
        return true;
    }
    char names[256];
    unsaved_names(s, names, sizeof names);
    ask_unsaved(s, STUDIO_UNSAVED_QUIT, names, "quitting");
    return false;
}

/* Asks inline whether to save the unsaved documents before creating. */
static void confirm_unsaved(studio *s, studio_new_project *dialog)
{
    char names[256];
    unsaved_names(s, names, sizeof names);
    snprintf(
        dialog->confirm,
        sizeof dialog->confirm,
        "Save %s before creating %s?",
        names,
        dialog->name
    );
    dialog->confirm_save = true;
}

static bool named(const char *name)
{
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        if (*c > ' ') {
            return true;
        }
    }
    return false;
}

static void trim(char *text)
{
    char *first = text;
    while (isspace((unsigned char)*first)) {
        first++;
    }
    if (first != text) {
        memmove(text, first, strlen(first) + 1);
    }
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
}

#ifndef __EMSCRIPTEN__
static void project_slug(const char *name, char *out, size_t size)
{
    size_t at = 0;
    bool separator = false;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        bool alphanumeric = (*c >= 'a' && *c <= 'z') ||
                            (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
        if (alphanumeric) {
            if (separator && at && at + 1 < size) {
                out[at++] = '-';
            }
            if (at + 1 >= size) {
                break;
            }
            out[at++] = (char)tolower(*c);
            separator = false;
        } else {
            separator = true;
        }
    }
    if (!at) {
        snprintf(out, size, "project");
        return;
    }
    out[at] = '\0';
}

/* Where New Project creates unless another folder is chosen: the workspace's
   projects, else beside the external project an installed editor opened. */
static bool default_location(const studio *s, char *folder, size_t size)
{
    const char *workspace = resource_workspace();
    if (workspace) {
        int length = snprintf(folder, size, "%s/projects", workspace);
        return length >= 0 && (size_t)length < size;
    }
    size_t length = strlen(s->project_root);
    while (length > 1 && s->project_root[length - 1] == '/') {
        length--;
    }
    while (length && s->project_root[length - 1] != '/') {
        length--;
    }
    if (length >= size) {
        return false;
    }
    if (!length) {
        snprintf(folder, size, ".");
        return true;
    }
    if (length > 1) {
        length--;
    }
    memcpy(folder, s->project_root, length);
    folder[length] = '\0';
    return true;
}

static char *native_project_root(const studio *s, const char *name)
{
    char base[128];
    project_slug(name, base, sizeof base);
    char folder[4096];
    if (s->new_project.folder[0]) {
        snprintf(folder, sizeof folder, "%s", s->new_project.folder);
    } else if (!default_location(s, folder, sizeof folder)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char path[4096];
    struct stat info;
    for (int index = 1;; index++) {
        int length = index == 1
                       ? snprintf(path, sizeof path, "%s/%s", folder, base)
                       : snprintf(path, sizeof path, "%s/%s-%d", folder, base, index);
        if (length < 0 || (size_t)length >= sizeof path) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (stat(path, &info) == 0) {
            continue;
        }
        if (errno == ENOENT) {
            return xstrdup(path);
        }
        return NULL;
    }
}
#endif

#ifdef __EMSCRIPTEN__
static char *browser_project_root(const char *name, char *error, size_t error_size)
{
    if (!web_project_create_available()) {
        snprintf(error, error_size, "Project creation is unavailable in this browser");
        return NULL;
    }
    char *json = web_create_project(name, CHIBI_PROJECT_VERSION);
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    free(json);
    yyjson_val *result = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *root = result ? yyjson_get_str(yyjson_obj_get(result, "root")) : NULL;
    const char *message = result ? yyjson_get_str(yyjson_obj_get(result, "error"))
                                 : NULL;
    char *copy = root ? xstrdup(root) : NULL;
    if (!copy) {
        snprintf(error, error_size, "%s", message ? message : "Could not create project");
    }
    if (doc) {
        yyjson_doc_free(doc);
    }
    return copy;
}
#endif

void studio_new_project_open(studio *s)
{
    s->new_project = (studio_new_project){ .open = true, .focus = true };
#ifndef __EMSCRIPTEN__
    if (!default_location(s, s->new_project.folder, sizeof s->new_project.folder)) {
        s->new_project.folder[0] = '\0';
    }
#endif
}

/* The longest location the dialog shows before it drops leading folders. */
#define LOCATION_SHOWN 48

void studio_new_project_location(const studio *s, char *out, size_t size)
{
    const char *folder = s->new_project.folder;
    const char *home = getenv("HOME");
    size_t length = home ? strlen(home) : 0;
    char full[sizeof s->new_project.folder + 1];
    bool homed = length > 1 && !strncmp(folder, home, length) &&
                 (folder[length] == '/' || !folder[length]);
    snprintf(full, sizeof full, "%s%s", homed ? "~" : "", homed ? folder + length : folder);
    /* The folders nearest the project tell it apart; drop the leading ones. */
    const char *tail = full;
    while (strlen(tail) > LOCATION_SHOWN && strchr(tail + 1, '/')) {
        tail = strchr(tail + 1, '/');
    }
    snprintf(out, size, "%s%s", tail == full ? "" : "\xe2\x80\xa6", tail);
}

static void created_project(studio *s, const char *root)
{
#ifdef __EMSCRIPTEN__
    if (web_projects_available()) {
        web_open_project(root);
        return;
    }
    web_remember_project(root);
#endif
    free(s->open_project);
    s->open_project = xstrdup(root);
}

static bool create_project(studio *s)
{
    studio_new_project *dialog = &s->new_project;
    if (dialog->created_root[0]) {
#ifdef __EMSCRIPTEN__
        if (web_projects_available()) {
            web_open_created_project(dialog->created_root);
            dialog->waiting = true;
            dialog->error[0] = '\0';
            return false;
        }
#endif
    }
    trim(dialog->name);
    if (!named(dialog->name)) {
        snprintf(dialog->error, sizeof dialog->error, "Enter a project name");
        return false;
    }
    char error[192] = "";
    char *root;
#ifdef __EMSCRIPTEN__
    root = browser_project_root(dialog->name, error, sizeof error);
    if (!root) {
        snprintf(dialog->error, sizeof dialog->error, "%s", error);
        return false;
    }
    snprintf(dialog->created_root, sizeof dialog->created_root, "%s", root);
    if (web_projects_available()) {
        web_open_created_project(root);
        dialog->waiting = true;
        free(root);
        return false;
    }
#else
    root = native_project_root(s, dialog->name);
    if (!root) {
        snprintf(error, sizeof error, "Cannot inspect project folder: %s", strerror(errno));
        snprintf(dialog->error, sizeof dialog->error, "%s", error);
        return false;
    }
    if (chibi_project_create(root, dialog->name, error, sizeof error) != 0) {
        snprintf(dialog->error, sizeof dialog->error, "%s", error);
        free(root);
        return false;
    }
#endif
    created_project(s, root);
    free(root);
    return true;
}

void studio_new_project_action(studio *s, const char *action)
{
    studio_new_project *dialog = &s->new_project;
    if (!dialog->open || dialog->waiting) {
        return;
    }
    if (!strcmp(action, "cancel")) {
        dialog->confirm_save = false;
        dialog->open = false;
        return;
    }
#ifndef __EMSCRIPTEN__
    if (!strcmp(action, "location")) {
        pick_folder(PICK_LOCATION, dialog->folder[0] ? dialog->folder : NULL);
        return;
    }
#endif
    bool create = !strcmp(action, "create");
    bool answer = !strcmp(action, "save") || !strcmp(action, "discard");
    if (!create && !(answer && dialog->confirm_save)) {
        return;
    }
    trim(dialog->name);
    if (!named(dialog->name)) {
        snprintf(dialog->error, sizeof dialog->error, "Enter a project name");
        return;
    }
    /* The dialog asks inline what the unsaved prompt asks elsewhere, and
       asks again about what Save left unsaved. */
    if ((create && studio_unsaved(s)) ||
        (!strcmp(action, "save") && !studio_save_all(s))) {
        confirm_unsaved(s, dialog);
        return;
    }
    dialog->confirm_save = false;
    if (create_project(s)) {
        dialog->open = false;
    }
}

void studio_new_project_frame(studio *s)
{
    studio_new_project *dialog = &s->new_project;
#ifdef __EMSCRIPTEN__
    if (dialog->waiting) {
        char *status = web_project_open_status();
        if (!strcmp(status, "done")) {
            dialog->waiting = false;
            dialog->open = false;
        } else if (!strncmp(status, "error:", 6)) {
            dialog->waiting = false;
            snprintf(dialog->error, sizeof dialog->error, "%s", status + 6);
        }
        free(status);
    }
#endif
    studio_ui_new_project(s);
}

char *studio_take_project(studio *s)
{
    char *root = s->open_project;
    s->open_project = NULL;
    return root;
}
