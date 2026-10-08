/* Thumbnails kept between sessions (studio_thumbnails.h), as Unity's Library
 * and Unreal's Derived Data Cache keep derived data: one PNG a picture, named
 * by the 64-bit key of the content it shows, in one directory for every
 * project, so a copied project finds its pictures by content. The directory
 * is read once; past the cap the least recently used pictures go first,
 * their use being the file's modification time, which loading renews. In the
 * browser the directory is an IndexedDB mount (tools/web_cache.js), written
 * back a moment after each change. */
#include "editor/studio/studio_thumbnails.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, disk_persist_browser, (), {
    if (Module['chibiPersistCache']) {
        Module['chibiPersistCache']();
    }
})
#endif

#define EXTENSION ".png"
#define KEY_DIGITS 16

typedef struct
{
    uint64_t key;
    size_t size;
    int64_t used; /* nanoseconds since the epoch */
} disk_entry;

struct studio_thumb_disk
{
    char *directory;
    size_t cap, total;
    disk_entry *entries; /* support/array.h, sorted by key */
};

static int64_t nanoseconds(struct timespec time)
{
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int64_t now()
{
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return nanoseconds(time);
}

static int64_t modified(const struct stat *info)
{
#ifdef __APPLE__
    return nanoseconds(info->st_mtimespec);
#else
    return nanoseconds(info->st_mtim);
#endif
}

static int compare_keys(const void *a, const void *b)
{
    uint64_t x = ((const disk_entry *)a)->key, y = ((const disk_entry *)b)->key;
    return (x > y) - (x < y);
}

static void entry_path(const studio_thumb_disk *d, uint64_t key, char *out, size_t size)
{
    snprintf(out, size, "%s/%016" PRIx64 EXTENSION, d->directory, key);
}

/* A file named as the cache names them, and its key. */
static bool parse_key(const char *name, uint64_t *key)
{
    if (strlen(name) != KEY_DIGITS + strlen(EXTENSION) ||
        strcmp(name + KEY_DIGITS, EXTENSION)) {
        return false;
    }
    char *end;
    *key = strtoull(name, &end, 16);
    return end == name + KEY_DIGITS;
}

static disk_entry *find(const studio_thumb_disk *d, uint64_t key)
{
    disk_entry probe = { .key = key };
    return acount(d->entries)
             ? bsearch(&probe, d->entries, (size_t)acount(d->entries), sizeof probe, compare_keys)
             : NULL;
}

static void scan(studio_thumb_disk *d)
{
    DIR *dir = opendir(d->directory);
    struct dirent *item;
    while (dir && (item = readdir(dir))) {
        uint64_t key;
        char path[1024];
        struct stat info;
        snprintf(path, sizeof path, "%s/%s", d->directory, item->d_name);
        size_t length = strlen(item->d_name);
        if (length > 5 && !strcmp(item->d_name + length - 5, ".part")) {
            remove(path); /* a store a crash cut short */
            continue;
        }
        if (parse_key(item->d_name, &key) && stat(path, &info) == 0 &&
            S_ISREG(info.st_mode)) {
            apush(d->entries, ((disk_entry){ key, (size_t)info.st_size, modified(&info) }));
            d->total += (size_t)info.st_size;
        }
    }
    if (dir) {
        closedir(dir);
    }
    if (acount(d->entries)) {
        qsort(d->entries, (size_t)acount(d->entries), sizeof *d->entries, compare_keys);
    }
}

static void forget(studio_thumb_disk *d, disk_entry *e)
{
    char path[1024];
    entry_path(d, e->key, path, sizeof path);
    remove(path);
    d->total -= e->size;
    int at = (int)(e - d->entries), after = acount(d->entries) - at - 1;
    memmove(e, e + 1, (size_t)after * sizeof *e);
    asetlen(d->entries, acount(d->entries) - 1);
}

/* The least recently used go until the rest fit the cap. */
static void evict(studio_thumb_disk *d)
{
    while (d->total > d->cap && acount(d->entries)) {
        disk_entry *oldest = d->entries;
        for (int i = 1; i < acount(d->entries); i++) {
            oldest = d->entries[i].used < oldest->used ? &d->entries[i] : oldest;
        }
        forget(d, oldest);
    }
}

/* Makes each missing folder of `path`. */
static bool make_directory(const char *path)
{
    char partial[1024];
    snprintf(partial, sizeof partial, "%s", path);
    for (char *at = partial + 1; *at; at++) {
        if (*at == '/') {
            *at = '\0';
            mkdir(partial, 0755);
            *at = '/';
        }
    }
    return mkdir(partial, 0755) == 0 || errno == EEXIST;
}

static void persist()
{
#ifdef __EMSCRIPTEN__
    disk_persist_browser();
#endif
}

studio_thumb_disk *studio_thumb_disk_open(const char *directory, size_t cap)
{
    if (!make_directory(directory)) {
        return NULL;
    }
    studio_thumb_disk *d = xcalloc(1, sizeof *d);
    d->directory = xstrdup(directory);
    size_t length = strlen(d->directory);
    if (length > 1 && d->directory[length - 1] == '/') {
        d->directory[length - 1] = '\0';
    }
    d->cap = cap;
    scan(d);
    evict(d);
    return d;
}

void studio_thumb_disk_close(studio_thumb_disk *d)
{
    if (!d) {
        return;
    }
    free(d->directory);
    afree(d->entries);
    free(d);
}

bool studio_thumb_disk_has(const studio_thumb_disk *d, uint64_t key)
{
    return find(d, key) != NULL;
}

/* The file's bytes; NULL when it cannot be read. */
static void *read_bytes(const char *path, int *size)
{
    FILE *file = fopen(path, "rb");
    long length = file && fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    void *bytes = length > 0 && fseek(file, 0, SEEK_SET) == 0
                    ? xmalloc((size_t)length)
                    : NULL;
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    if (file) {
        fclose(file);
    }
    *size = (int)length;
    return bytes;
}

bool studio_thumb_disk_load(studio_thumb_disk *d, uint64_t key, CF_Canvas canvas, int side)
{
    disk_entry *e = find(d, key);
    if (!e) {
        return false;
    }
    char path[1024];
    entry_path(d, key, path, sizeof path);
    int size;
    void *bytes = read_bytes(path, &size);
    CF_Image image = { 0 };
    bool decoded = bytes &&
                   !cf_is_error(cf_image_load_png_from_memory(bytes, size, &image));
    free(bytes);
    bool fits = decoded && image.w == side && image.h == side;
    if (fits) {
        cf_texture_update(
            cf_canvas_get_target(canvas),
            image.pix,
            side * side * (int)sizeof *image.pix
        );
        e->used = now();
        utimensat(AT_FDCWD, path, NULL, 0);
    }
    if (decoded) {
        cf_image_free(&image);
    }
    if (!fits) {
        /* Unreadable or of another size: it renders again. */
        forget(d, e);
        persist();
    }
    return fits;
}

void studio_thumb_disk_store(studio_thumb_disk *d, uint64_t key, const CF_Pixel *pixels, int side)
{
    CF_Image image = { side, side, (CF_Pixel *)pixels };
    void *png = NULL;
    int size = 0;
    if (cf_is_error(cf_image_save_png_to_memory(&image, &png, &size))) {
        return;
    }
    char path[1024], partial[1040];
    entry_path(d, key, path, sizeof path);
    snprintf(partial, sizeof partial, "%s.part", path);
    FILE *file = fopen(partial, "wb");
    bool written = file && fwrite(png, 1, (size_t)size, file) == (size_t)size;
    written = file && fclose(file) == 0 && written;
    cf_free(png);
    /* Written whole before it takes its name, so a crash never leaves half
       a picture under a key. */
    if (!written || rename(partial, path) != 0) {
        remove(partial);
        return;
    }
    disk_entry *e = find(d, key);
    if (e) {
        d->total -= e->size;
        e->size = (size_t)size;
        e->used = now();
    } else {
        apush(d->entries, ((disk_entry){ key, (size_t)size, now() }));
        qsort(d->entries, (size_t)acount(d->entries), sizeof *d->entries, compare_keys);
    }
    d->total += (size_t)size;
    evict(d);
    persist();
}

size_t studio_thumb_disk_size(const studio_thumb_disk *d)
{
    return d->total;
}
