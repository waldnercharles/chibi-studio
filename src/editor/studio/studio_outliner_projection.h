#ifndef CHIBI_STUDIO_OUTLINER_PROJECTION_H
#define CHIBI_STUDIO_OUTLINER_PROJECTION_H

#include "render/ui.h"
#include "runtime/behaviors/source.h"

typedef struct studio studio;
typedef struct studio_outliner_projection studio_outliner_projection;

/* The same final-module contract applies to edited worlds without a live panel.
 * An initially absent producer is valid; an applied producer cannot disappear.
 * These worlds receive a stable service that denies capture/publication. */
bool studio_outliner_guard_install(source_registry *, char *error, size_t size);

/* Install on a fresh UI registry before preparing/publishing its sources.
 * Retire before destroying mounts/worlds; free after the registry is freed. */
studio_outliner_projection *studio_outliner_projection_new(
    studio *,
    source_registry *,
    ui *,
    char *error,
    size_t size
);
void studio_outliner_projection_retire(studio_outliner_projection *);
void studio_outliner_projection_free(studio_outliner_projection *);
void studio_outliner_projection_invalidate(studio_outliner_projection *);
/* Call before an edited scene/world is destroyed or replaced, including when
 * its allocation might later be reused at the same address. */
void studio_ui_outliner_invalidate(studio *);

void studio_outliner_projection_bind(studio_outliner_projection *, ui_mount *, uint64_t controller);
void studio_outliner_projection_unmount(studio_outliner_projection *, ui_mount *);
/* Close raw-read permission before callbacks/reentry and reap completed requests. */
void studio_outliner_projection_boundary(studio_outliner_projection *);

/* Keep the existing payload converter. Only dispatch is tagged; every other
 * host callback still receives its original context. Internal marks host-created
 * Refresh only, never a similarly named authored/user event. */
bool studio_outliner_projection_send(
    studio_outliner_projection *,
    const render_ui_host *,
    ecs_entity,
    const ui_activation *,
    ui_mount *,
    bool internal,
    unsigned primary,
    double now,
    double delta
);
/* A deferred host reveal's failure, preserved across the following ordinary
 * dispatch. Borrowed until the next send; report after send closes its scope. */
const char *studio_outliner_projection_replay_error(const studio_outliner_projection *);

#endif
