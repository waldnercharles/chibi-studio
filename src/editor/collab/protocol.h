#ifndef CHIBI_COLLAB_PROTOCOL_H
#define CHIBI_COLLAB_PROTOCOL_H

#include <yyjson.h>

#include <stdbool.h>
#include <stdint.h>

#define COLLAB_PROTOCOL 1
#define COLLAB_MAX_MESSAGE (32 * 1024 * 1024)
#define COLLAB_MAX_ID 96
#define COLLAB_MAX_SEQUENCE UINT64_C(9007199254740991)

typedef struct
{
    yyjson_doc *doc;
    const char *room, *client, *kind;
    uint64_t seq, revision;
    yyjson_val *payload;
} collab_message;

bool collab_message_read(const char *json, collab_message *message);
void collab_message_free(collab_message *message);
char *collab_message_write(
    const char *room,
    const char *client,
    uint64_t seq,
    uint64_t revision,
    const char *kind,
    const char *payload
);
bool collab_identifier(const char *id);

#endif
