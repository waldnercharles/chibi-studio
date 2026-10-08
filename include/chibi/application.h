#ifndef CHIBI_APPLICATION_H
#define CHIBI_APPLICATION_H

#include <stddef.h>

/* Checkpoints contain UTF-8 JSON. The caller owns returned bytes and frees them. */
void *chibi_application_create(int argc, char **argv, const char *checkpoint, char *error, size_t size);
char *chibi_application_checkpoint(void *state);
void chibi_application_activate(void *state);
/* Zero continues, positive exits normally, negative reports failure. */
int chibi_application_frame(void *state);
void chibi_application_destroy(void *state);

#endif
