#ifndef CHIBI_STUDIO_IMAGE_IMPORT_H
#define CHIBI_STUDIO_IMAGE_IMPORT_H
#include "authoring/studio_schema.h"
struct studio;
typedef struct studio_image_import studio_image_import;
/* OS selection boundary. Completion may arrive on another thread, including
 * after Studio closes. The host callback owns its completion context. */
typedef void (*studio_image_import_completion)(void *, const char *, const char *);
typedef void (*studio_image_import_chooser)(void *, studio_image_import_completion, void *);
void studio_image_import_set_chooser(struct studio *, studio_image_import_chooser, void *);
void studio_image_import_free(studio_image_import *);
const char *studio_image_import_run(struct studio *, const pin_value *, arena *, pin_value *);
#endif
