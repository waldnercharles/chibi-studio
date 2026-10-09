#include "editor/workspace/tree_watch.h"

#include "support/array.h"
#include "support/xalloc.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static long long modified_ns(const struct stat *info)
{
#if defined(_WIN32)
    /* The Windows runtime stats whole seconds. */
    return (long long)info->st_mtime * 1000000000LL;
#elif defined(__APPLE__)
    return (long long)info->st_mtimespec.tv_sec * 1000000000LL +
           info->st_mtimespec.tv_nsec;
#else
    return (long long)info->st_mtim.tv_sec * 1000000000LL + info->st_mtim.tv_nsec;
#endif
}

static bool has_extension(const char *name, const char *extension)
{
    size_t n = strlen(name), e = strlen(extension);
    return n > e && !strcmp(name + n - e, extension);
}

/* Symbolic links are not followed, so a linked directory cannot loop. */
static void scan(tree_watch_file **files, const char *directory, const char *extension)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        size_t size = strlen(directory) + strlen(entry->d_name) + 2;
        char *path = xmalloc(size);
        snprintf(path, size, "%s/%s", directory, entry->d_name);
        struct stat info;
        bool known = lstat(path, &info) == 0;
        if (known && S_ISDIR(info.st_mode)) {
            scan(files, path, extension);
        }
        if (!known || !S_ISREG(info.st_mode) || !has_extension(path, extension)) {
            free(path);
            continue;
        }
        apush(
            *files,
            ((tree_watch_file){ path, modified_ns(&info), (long long)info.st_size })
        );
    }
    if (dir) {
        closedir(dir);
    }
}

static int compare_paths(const void *a, const void *b)
{
    return strcmp(
        ((const tree_watch_file *)a)->path,
        ((const tree_watch_file *)b)->path
    );
}

static void free_files(tree_watch_file *files)
{
    for (int i = 0; i < acount(files); i++) {
        free(files[i].path);
    }
    afree(files);
}

void tree_watch_free(tree_watch *w)
{
    free(w->root);
    free_files(w->files);
    *w = (tree_watch){ 0 };
}

/* Both lists are sorted, so one walk finds removals, additions and edits. */
static char **differences(const tree_watch_file *before, const tree_watch_file *after)
{
    char **changes = NULL;
    int i = 0, j = 0;
    while (i < acount(before) || j < acount(after)) {
        int order = i == acount(before) ? 1
                  : j == acount(after)  ? -1
                                        : strcmp(before[i].path, after[j].path);
        if (order < 0) {
            apush(changes, xstrdup(before[i++].path));
            continue;
        }
        if (order > 0) {
            apush(changes, xstrdup(after[j++].path));
            continue;
        }
        if (before[i].modified != after[j].modified ||
            before[i].size != after[j].size) {
            apush(changes, xstrdup(after[j].path));
        }
        i++;
        j++;
    }
    return changes;
}

char **tree_watch_poll(tree_watch *w, const char *root, const char *extension, double now)
{
    bool same = w->root && !strcmp(w->root, root);
    if (same && now < w->due) {
        return NULL;
    }
    w->due = now + 1;
    tree_watch_file *files = NULL;
    scan(&files, root, extension);
    if (acount(files) > 1) {
        qsort(files, (size_t)acount(files), sizeof *files, compare_paths);
    }
    char **changes = same ? differences(w->files, files) : NULL;
    if (!same) {
        free(w->root);
        w->root = xstrdup(root);
    }
    free_files(w->files);
    w->files = files;
    return changes;
}

void tree_watch_changes_free(char **changes)
{
    for (int i = 0; i < acount(changes); i++) {
        free(changes[i]);
    }
    afree(changes);
}

void tree_watch_note(tree_watch *w, const char *path)
{
    int at = 0;
    while (at < acount(w->files) && strcmp(w->files[at].path, path) < 0) {
        at++;
    }
    bool listed = at < acount(w->files) && !strcmp(w->files[at].path, path);
    struct stat info;
    if (stat(path, &info) != 0) {
        if (listed) {
            free(w->files[at].path);
            memmove(
                &w->files[at],
                &w->files[at + 1],
                (size_t)(acount(w->files) - at - 1) * sizeof *w->files
            );
            asetlen(w->files, acount(w->files) - 1);
        }
        return;
    }
    tree_watch_file file = { listed ? w->files[at].path : xstrdup(path),
                             modified_ns(&info),
                             (long long)info.st_size };
    if (!listed) {
        apush(w->files, file);
        memmove(
            &w->files[at + 1],
            &w->files[at],
            (size_t)(acount(w->files) - at - 1) * sizeof *w->files
        );
    }
    w->files[at] = file;
}
