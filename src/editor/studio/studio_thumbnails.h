#ifndef CHIBI_EDITOR_STUDIO_THUMBNAILS_H
#define CHIBI_EDITOR_STUDIO_THUMBNAILS_H

/* Asset thumbnails, as Unity's Project window and Unreal's Content Browser
 * show them: each asset drawn as it looks, into a small offscreen canvas
 * bound as a still kit image ("kit:thumb:<slot>"). Thumbnails render lazily,
 * only for what an emission asked for, within a frame budget, cheapest kinds
 * first. They follow the live asset, as Unreal's do: an open document's
 * unsaved edits show once they rest, never mid-gesture, and Undo and Redo
 * show too; otherwise a thumbnail shows its file and renders again when the
 * file or a file it uses changes. Pictures of saved content persist between
 * sessions on disk, keyed by the content they show. Rendering reads the
 * authored state and never changes it, the open scene or its runtime. */

#include "authoring/reference_index.h"
#include "graph/model/graph.h"
#include "platform/cute.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct studio studio;
typedef struct studio_thumbs studio_thumbs;

typedef enum
{
    STUDIO_THUMB_TILE,  /* a grid tile's, 128 dp */
    STUDIO_THUMB_LARGE, /* a hovered tile's preview, 256 dp */
    STUDIO_THUMB_SIZE_COUNT,
} studio_thumb_size;

/* The "kit:thumb:<slot>" source showing asset `asset` of the asset index, or
 * NULL while it renders or when its kind has no picture. `loading` tells
 * that more of the picture is still coming, as a scene's streamed terrain.
 * Asking keeps it cached and queues it; call while emitting. */
const char *studio_thumb(studio *s, int asset, studio_thumb_size size, bool *loading);
/* The tile thumbnail's source when one is cached, else NULL, keeping it
 * cached without queueing anything: the palette's results show what Assets
 * already rendered. */
const char *studio_thumb_cached(studio *s, int asset);
/* Renders queued thumbnails within the frame budget, after emission and
 * before the UI draws; wakes the UI when one is ready. */
void studio_thumbs_frame(studio *s);
/* Whether thumbnails wait to render, so the editor keeps its frames coming. */
bool studio_thumbs_pending(const studio *s);
void studio_thumbs_free(studio *s);

/* What rendering cost so far, by asset kind, for Performance and tests:
 * thumbnails finished, their milliseconds in all, and the longest one frame
 * spent on one. */
typedef struct
{
    int count[REFERENCE_ASSET_KIND_COUNT];
    double total_ms[REFERENCE_ASSET_KIND_COUNT];
    double max_ms[REFERENCE_ASSET_KIND_COUNT];
    int blank;       /* finished without a picture, as a camera's prototype */
    int bound;       /* textures bound now */
    int evicted;     /* thumbnails dropped to stay within the cap */
    int loaded;      /* pictures read from the disk cache */
    int stored;      /* pictures written to it */
    int partial;     /* pictures shown before all they show had loaded */
    double frame_ms; /* the last frame's thumbnail work, checks included */
} studio_thumb_stats;
studio_thumb_stats studio_thumbs_stats(const studio *s);

/* ---------- Renderers (studio_thumbnail_kinds.c) ---------- */

/* The worlds, renderers and documents rendering keeps between thumbnails,
 * made as kinds need them. */
typedef struct studio_thumb_renderers studio_thumb_renderers;

typedef struct
{
    reference_asset_kind kind;
    studio_thumb_size size;
    const char *name;
    const char *path;
    int side;   /* the canvas's pixels */
    bool light; /* Studio's theme, which ui documents show in */
} studio_thumb_job;

/* The canvas side a kind renders at for a thumbnail of `side` pixels: fields
 * bake on the CPU at half of it. */
int studio_thumb_side(reference_asset_kind kind, int side);
/* The order kinds render in, cheapest first. */
int studio_thumb_cost(reference_asset_kind kind);
typedef enum
{
    STUDIO_THUMB_DRAWN,
    STUDIO_THUMB_BLANK, /* no picture, as an empty graph has none */
    /* Call again with the job next frame: a scene takes a step a call, and
       the world scenes and prototypes run in copies a share a call. */
    STUDIO_THUMB_BUSY,
    /* Drawn with what has loaded so far; call again, and it draws again
       once the rest has. */
    STUDIO_THUMB_PARTIAL,
} studio_thumb_step;

/* Draws the job's asset into `canvas`, or takes a step toward it. */
studio_thumb_step studio_thumb_render(
    studio *s,
    studio_thumb_renderers **r,
    const studio_thumb_job *job,
    CF_Canvas canvas
);
/* Kinds that render in the world a busy scene holds, and wait for it. */
bool studio_thumb_needs_world(reference_asset_kind kind);
/* Drops a job left busy. */
void studio_thumb_cancel(studio_thumb_renderers *r);
void studio_thumb_renderers_free(studio_thumb_renderers *r);
/* The text saving the asset's open document would write, while it holds
 * unsaved edits; NULL when the asset shows its file. Owned. A ui document's
 * edits stand in for its files (support/text_overlay.h), so it has none. */
char *studio_thumb_unsaved(studio *s, const reference_asset *a);
/* The prototype that prototypes and materials stand on, which their
 * pictures depend on beside what they reference; NULL for none. */
const char *studio_thumb_stage(studio *s);

/* ---------- Disk cache (studio_thumbnail_cache.c) ---------- */

/* Square RGBA pictures in `directory`, by key, trimmed to `cap` bytes, least
 * recently used first; NULL when the directory cannot be made. */
typedef struct studio_thumb_disk studio_thumb_disk;
studio_thumb_disk *studio_thumb_disk_open(const char *directory, size_t cap);
void studio_thumb_disk_close(studio_thumb_disk *d);
bool studio_thumb_disk_has(const studio_thumb_disk *d, uint64_t key);
/* Fills the canvas's `side` pixels square with the picture of `key`; false,
 * forgetting it, when it is missing, unreadable or of another size. */
bool studio_thumb_disk_load(studio_thumb_disk *d, uint64_t key, CF_Canvas canvas, int side);
void studio_thumb_disk_store(studio_thumb_disk *d, uint64_t key, const CF_Pixel *pixels, int side);
/* The bytes it holds. */
size_t studio_thumb_disk_size(const studio_thumb_disk *d);

/* ---------- Graph mini-maps (studio_thumbnail_graph.c) ---------- */

/* A graph as Blender's and Unreal's node-group thumbnails draw it: its node
 * boxes, in their category colors, and its wires, fitted into a square of
 * `side` pixels with a margin, y down. */
typedef struct
{
    float x, y, w, h;
    uint32_t color; /* 0xRRGGBBAA, the node's category */
} studio_minimap_box;

typedef struct
{
    float x0, y0, x1, y1;
    bool flow; /* an execution wire */
} studio_minimap_wire;

typedef struct
{
    studio_minimap_box *boxes; /* support/array.h */
    studio_minimap_wire *wires;
} studio_minimap;

/* Arranges `g`'s automatic nodes in place, as the editor would, then fits its
 * boxes and wires into the square; empty when it has no boxes. */
void studio_minimap_build(graph *g, float side, studio_minimap *out);
void studio_minimap_free(studio_minimap *m);
/* Draws the mini-map with Cute's draw calls into the current target, whose
 * projection maps the square with y up. */
void studio_minimap_draw(const studio_minimap *m, float side);

#endif
