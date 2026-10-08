#ifndef CHIBI_TREE_WATCH_H
#define CHIBI_TREE_WATCH_H

/* Polls a directory tree for files that appeared, changed or disappeared, by
 * modification time and size. Zero-initialize; free with tree_watch_free. */
typedef struct tree_watch_file
{
    char *path;
    long long modified; /* nanoseconds */
    long long size;
} tree_watch_file;

typedef struct tree_watch
{
    char *root;             /* NULL until the first poll */
    tree_watch_file *files; /* sorted by path */
    double due;
} tree_watch;

void tree_watch_free(tree_watch *w);

/* Paths of files ending in `extension` under `root` that changed since the
 * last poll, as an owned array of owned strings; NULL when none did. Scans at
 * most once a second; the first poll of a root only records it. */
char **tree_watch_poll(tree_watch *w, const char *root, const char *extension, double now);
void tree_watch_changes_free(char **changes);

/* Records `path` as it is now, as after the program wrote it itself. */
void tree_watch_note(tree_watch *w, const char *path);

#endif
