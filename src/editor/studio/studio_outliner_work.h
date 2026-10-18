#ifndef CHIBI_EDITOR_STUDIO_OUTLINER_WORK_H
#define CHIBI_EDITOR_STUDIO_OUTLINER_WORK_H

#include "editor/studio/studio_scene_read.h"

#include <stdint.h>

#define STUDIO_OUTLINER_WORK_BOOTSTRAP ((uintptr_t)10000)

/* Native workload metadata from one owned capture, not row policy or a bound
 * on native callback time. Text lengths use the authored byte-operation domain.
 * Only measure fills this in production; arithmetic fixtures may use synthetic
 * dimensions to exercise overflow without allocating a corresponding scene. */
typedef struct
{
    uintptr_t actors, prototypes, components, fields, attachments;
    uintptr_t filter_bytes, path_bytes, reveal_bytes, actor_text_bytes;
    uintptr_t visible, max_depth, max_components, max_fields;
    uintptr_t match_starts, match_pairs;
} studio_outliner_work;

/* NULL on success. Failure leaves output untouched and has no model effects. */
const char *studio_outliner_work_measure(
    const studio_scene_read_facts *,
    int first,
    int count,
    bool collapsed,
    const char *reveal,
    studio_outliner_work *output
);

/* Absolute total 10000 + canonical_weight * W; never remaining/extra work.
 * canonical_weight must come from the reviewed frozen canonical BF_CHECK
 * census. Zero and every unrepresentable intermediate reject without changing
 * output. This arithmetic does not grant or renew a runtime account. */
bool studio_outliner_work_limit(const studio_outliner_work *, uintptr_t canonical_weight, uintptr_t *output);

#endif
