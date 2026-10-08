/* Editor view state of the open scene (scene_authoring.h): the objects its
 * edit view hides or will not pick. A game_save holds every scene's, keyed by
 * the scene's path, as {"<object>": flags}; Studio binds the store to a user
 * file, as Unity keeps Scene visibility in its Library, so the scene file and
 * its Undo never see it. Objects are keyed by name: a scene keeps names unique
 * and saves them, while actor identities are numbered anew by each load. */
#include "editor/workspace/scene_internal.h"
#include "runtime/save.h"
#include "support/array.h"
#include "support/xalloc.h"
#include <yyjson.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_FLAGS (APP_SCENE_VIEW_HIDDEN | APP_SCENE_VIEW_LOCKED)

static int compare_entries(const void *a, const void *b)
{
    return strcmp(
        ((const scene_view_entry *)a)->name,
        ((const scene_view_entry *)b)->name
    );
}

static int find_entry(const app_scene_authoring *scene, const char *name)
{
    if (!acount(scene->view)) {
        return -1;
    }
    scene_view_entry key = { .name = (char *)name };
    const scene_view_entry
        *found = bsearch(&key, scene->view, (size_t)acount(scene->view), sizeof key, compare_entries);
    return found ? (int)(found - scene->view) : -1;
}

static void sort_entries(app_scene_authoring *scene)
{
    qsort(scene->view, (size_t)acount(scene->view), sizeof *scene->view, compare_entries);
}

/* Gives `name` its flags; none forgets it. */
static void put_entry(app_scene_authoring *scene, const char *name, unsigned flags)
{
    int at = find_entry(scene, name);
    if (at >= 0 && flags) {
        scene->view[at].flags = flags;
        return;
    }
    if (at >= 0) {
        free(scene->view[at].name);
        adel(scene->view, at);
        sort_entries(scene);
        return;
    }
    if (flags) {
        apush(scene->view, ((scene_view_entry){ xstrdup(name), flags }));
        sort_entries(scene);
    }
}

static void clear_entries(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->view); i++) {
        free(scene->view[i].name);
    }
    aclear(scene->view);
}

/* Reads the open scene's entries again when the scene or the store changed.
   An unsaved scene's stay in memory. */
static void sync_view(app_scene_authoring *scene)
{
    const char *path = scene->scene_path ? scene->scene_path : "";
    uint64_t revision = game_save_revision(scene->view_store);
    if (scene->view_path && !strcmp(scene->view_path, path) &&
        scene->view_revision == revision) {
        return;
    }
    free(scene->view_path);
    scene->view_path = xstrdup(path);
    scene->view_revision = revision;
    clear_entries(scene);
    const char *text = path[0] ? game_save_get(scene->view_store, path) : NULL;
    yyjson_doc *doc = text ? yyjson_read(text, strlen(text), 0) : NULL;
    if (!doc) {
        return;
    }
    size_t i, max;
    yyjson_val *name, *flags;
    yyjson_obj_foreach(yyjson_doc_get_root(doc), i, max, name, flags)
    {
        unsigned value = (unsigned)yyjson_get_uint(flags) & VIEW_FLAGS;
        if (value) {
            apush(scene->view, ((scene_view_entry){ xstrdup(yyjson_get_str(name)), value }));
        }
    }
    yyjson_doc_free(doc);
    sort_entries(scene);
}

static void write_view(app_scene_authoring *scene)
{
    if (!scene->view_path[0]) {
        return;
    }
    if (!acount(scene->view)) {
        game_save_remove(scene->view_store, scene->view_path);
        scene->view_revision = game_save_revision(scene->view_store);
        return;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < acount(scene->view); i++) {
        yyjson_mut_obj_add_uint(doc, root, scene->view[i].name, scene->view[i].flags);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    assert(json);
    /* A path the store refuses keeps its state for this session only. */
    game_save_set(scene->view_store, scene->view_path, json);
    free(json);
    yyjson_mut_doc_free(doc);
    scene->view_revision = game_save_revision(scene->view_store);
}

unsigned app_scene_authoring_actor_view(app_scene_authoring *scene, int actor)
{
    assert(actor >= 0 && actor < acount(scene->actors));
    sync_view(scene);
    int at = find_entry(scene, scene->actors[actor].name);
    return at < 0 ? 0 : scene->view[at].flags;
}

void app_scene_authoring_set_actor_view(app_scene_authoring *scene, int actor, unsigned flags)
{
    assert(actor >= 0 && actor < acount(scene->actors));
    assert(!(flags & ~VIEW_FLAGS));
    if (app_scene_authoring_actor_view(scene, actor) == flags) {
        return;
    }
    put_entry(scene, scene->actors[actor].name, flags);
    write_view(scene);
}

game_save *app_scene_authoring_view_store(app_scene_authoring *scene)
{
    return scene->view_store;
}

void scene_view_rename(app_scene_authoring *scene, const char *from, const char *to)
{
    sync_view(scene);
    int at = find_entry(scene, from);
    if (at < 0) {
        return;
    }
    unsigned flags = scene->view[at].flags;
    put_entry(scene, from, 0);
    if (to) {
        put_entry(scene, to, flags);
    }
    write_view(scene);
}

void scene_view_save_as(app_scene_authoring *scene, const char *path)
{
    sync_view(scene);
    if (!strcmp(scene->view_path, path)) {
        return;
    }
    free(scene->view_path);
    scene->view_path = xstrdup(path);
    write_view(scene);
}

render_skip scene_view_skip(app_scene_authoring *scene, unsigned flags, uint64_t **buffer)
{
    aclear(*buffer);
    bool edited = scene->active_runtime_target == APP_RUNTIME_SCENE &&
                  !scene->runtime_snapshot_valid;
    for (int i = 0; edited && i < acount(scene->actors); i++) {
        if (app_scene_authoring_actor_view(scene, i) & flags) {
            apush(*buffer, ecs_entity_to_u64(scene->actors[i].entity));
        }
    }
    int count = acount(*buffer);
    if (count > 1) {
        qsort(*buffer, (size_t)count, sizeof **buffer, render_skip_compare);
    }
    return (render_skip){ *buffer, count };
}

void scene_view_init(app_scene_authoring *scene)
{
    scene->view_store = game_save_new();
}

void scene_view_free(app_scene_authoring *scene)
{
    clear_entries(scene);
    afree(scene->view);
    afree(scene->view_hidden);
    afree(scene->view_unpickable);
    free(scene->view_path);
    game_save_free(scene->view_store);
}
