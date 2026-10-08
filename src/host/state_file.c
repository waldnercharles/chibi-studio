#include "state_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *chibi_read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)length + 1);
    if (!text) {
        abort();
    }
    bool ok = fread(text, 1, (size_t)length, file) == (size_t)length;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        free(text);
        return NULL;
    }
    text[length] = 0;
    return text;
}

bool chibi_write_file(const char *path, const char *text)
{
    size_t size = strlen(path) + sizeof ".XXXXXX";
    char *temporary = malloc(size);
    if (!temporary) {
        abort();
    }
    snprintf(temporary, size, "%s.XXXXXX", path);
    int fd = mkstemp(temporary);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
    bool ok = false;
    if (file) {
        size_t length = strlen(text);
        ok = fwrite(text, 1, length, file) == length;
        ok = fflush(file) == 0 && ok;
        ok = fsync(fd) == 0 && ok;
        ok = fclose(file) == 0 && ok;
        if (ok) {
            ok = rename(temporary, path) == 0;
        }
    } else if (fd >= 0) {
        close(fd);
    }
    if (!ok) {
        unlink(temporary);
    }
    free(temporary);
    return ok;
}
