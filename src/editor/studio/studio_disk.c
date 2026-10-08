/* Content another program changes reloads as it changes. A file that cannot
 * load raises an error; one whose document has unsaved edits offers Reload,
 * which discards them. */
#include "editor/studio/studio_internal.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <stdlib.h>
#include <string.h>

static void forget(studio *s, int index)
{
    free(s->disk_toasts[index].path);
    s->disk_toasts[index] = s->disk_toasts[acount(s->disk_toasts) - 1];
    asetlen(s->disk_toasts, acount(s->disk_toasts) - 1);
}

static void offer_reload(studio *s, const char *path, const char *title)
{
    for (int i = 0; i < acount(s->disk_toasts); i++) {
        if (!strcmp(s->disk_toasts[i].path, path)) {
            kit_toast_dismiss(s->kit, s->disk_toasts[i].toast);
            forget(s, i);
            break;
        }
    }
    int toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .type = KIT_TOAST_WARNING,
                           .title = title,
                           .description = content_basename(path),
                           .action = "Reload",
                           .duration = -1 }
    );
    apush(s->disk_toasts, ((studio_disk_toast){ toast, xstrdup(path) }));
}

static void report(studio *s, app_disk_result result, const char *path, const char *message)
{
    if (result == APP_DISK_FAILED) {
        studio_error(s, content_basename(path), message);
    }
    if (result == APP_DISK_UNSAVED) {
        offer_reload(s, path, "Changed on disk");
    }
}

static void reload(studio *s, const char *path)
{
    report(s, studio_code_reload_path(s, path), path, app_scene_authoring_message(s->scene));
    s->problems_checked = false;
}

void studio_reload(studio *s)
{
    app_scene_authoring *scene = s->scene;
    if (scene->active_tab < 0 || scene->active_tab >= acount(scene->tabs)) {
        return;
    }
    scene_document_tab *tab = &scene->tabs[scene->active_tab];
    if (!tab->path) {
        studio_error(s, "Could not reload", "Save the document first");
        return;
    }
    if (scene_tabs_dirty(scene, tab)) {
        offer_reload(s, tab->path, "Reload without saving?");
        return;
    }
    char *path = xstrdup(tab->path);
    reload(s, path);
    free(path);
}

void studio_disk_frame(studio *s)
{
    app_disk_change *changes = app_scene_authoring_sync_disk(s->scene, s->time);
    /* What changed on disk can change the project's problems. */
    s->problems_checked = s->problems_checked && !acount(changes);
    for (int i = 0; i < acount(changes); i++) {
        report(s, changes[i].result, changes[i].path, changes[i].message);
    }
    app_disk_changes_free(changes);
    for (int i = acount(s->disk_toasts) - 1; i >= 0; i--) {
        if (!kit_toast_action(s->kit, s->disk_toasts[i].toast)) {
            continue;
        }
        char *path = s->disk_toasts[i].path;
        s->disk_toasts[i].path = NULL;
        forget(s, i);
        reload(s, path);
        free(path);
    }
}

void studio_disk_free(studio *s)
{
    for (int i = 0; i < acount(s->disk_toasts); i++) {
        free(s->disk_toasts[i].path);
    }
    afree(s->disk_toasts);
}
