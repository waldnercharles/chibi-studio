#ifndef CHIBI_STUDIO_TEXTURE_ASSET_H
#define CHIBI_STUDIO_TEXTURE_ASSET_H

#include "authoring/studio_schema.h"

struct studio;
typedef struct studio_texture_asset studio_texture_asset;
/* Selection is separate from the retained per-asset settings documents. */
bool studio_texture_open(struct studio *, const char *key);
void studio_texture_close(struct studio *);
bool studio_texture_active(const struct studio *);
/* Counter for the selected settings document and its action status. */
uint64_t studio_texture_revision(const struct studio *);
const char *studio_texture_key(const struct studio *);
bool studio_texture_dirty(const struct studio *);
bool studio_texture_unsaved(const struct studio *);
bool studio_texture_history(struct studio *, bool redo, bool apply);
bool studio_texture_save(struct studio *);
bool studio_texture_save_all(struct studio *);
void studio_texture_asset_free(studio_texture_asset *);
/* Operation: read, edit, undo, redo, save, reimport. Resolution and
 * Semantic are the authored form choices. Outputs: Active, Image, Source,
 * Resolution, Semantic, Dirty, Status, Info. Strings belong to storage. */
const char *studio_texture_asset_run(struct studio *, const pin_value *, arena *, pin_value *);

#endif
