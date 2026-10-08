#ifndef CHIBI_HOST_STATE_FILE_H
#define CHIBI_HOST_STATE_FILE_H
#include <stdbool.h>
char *chibi_read_file(const char *path);
bool chibi_write_file(const char *path, const char *text);
#endif
