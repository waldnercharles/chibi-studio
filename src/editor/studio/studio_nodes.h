#ifndef CHIBI_STUDIO_NODES_H
#define CHIBI_STUDIO_NODES_H

#include "authoring/studio_schema.h"

/* Studio's implementation of every node (studio_node_calls.c). */
const char *studio_node_run(
    struct studio *s,
    studio_node_op op,
    const pin_value *in,
    arena *storage,
    pin_value *out
);

/* A number that differs whenever something the nodes read changed, and is
   the same while nothing did (studio_node_calls.c). */
uint64_t studio_nodes_stamp(const struct studio *s);

#endif
