#ifndef CHIBI_EDITOR_LABELS_H
#define CHIBI_EDITOR_LABELS_H

#include "graph/model/node.h"

/* Presentation only; registry and serialized names remain unchanged. */
void editor_node_label(const node_desc *desc, char *buf, size_t cap);
/* Zero capacity is a no-op and permits a NULL buffer. */
void editor_component_label(const char *name, char *buf, size_t cap);
void editor_identifier_label(const char *name, char *buf, size_t cap);

#endif
