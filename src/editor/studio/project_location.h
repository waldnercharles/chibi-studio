#ifndef CHIBI_STUDIO_PROJECT_LOCATION_H
#define CHIBI_STUDIO_PROJECT_LOCATION_H

#include <stdbool.h>
#include <stddef.h>

/* Choose an unused project folder without creating it. On failure, set errno. */
bool studio_project_location(const char *folder, const char *name, char *path, size_t size);

#endif
