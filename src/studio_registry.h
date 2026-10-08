#ifndef CHIBI_STUDIO_REGISTRY_H
#define CHIBI_STUDIO_REGISTRY_H
/* Explicit application composition, before any authoring/type catalog access. */
void studio_registry_init();
/* After all authoring graphs and application state have retired. */
void studio_registry_free();
#endif
