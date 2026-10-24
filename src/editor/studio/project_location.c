#include "editor/studio/project_location.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

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

bool studio_project_location(const char *folder, const char *name, char *path, size_t size)
{
    char base[128];
    project_slug(name, base, sizeof base);
    struct stat info;
    for (int index = 1;; index++) {
        int length = index == 1
                       ? snprintf(path, size, "%s/%s", folder, base)
                       : snprintf(path, size, "%s/%s-%d", folder, base, index);
        if (length < 0 || (size_t)length >= size) {
            errno = ENAMETOOLONG;
            return false;
        }
        /* An occupied name stays occupied even when its symlink target is gone. */
        if (lstat(path, &info) == 0) {
            continue;
        }
        if (errno == ENOENT) {
            return true;
        }
        return false;
    }
}
