#include "editor/collab/protocol.h"

#include <string.h>

bool collab_identifier(const char *id)
{
    if (!id || !*id || strlen(id) > COLLAB_MAX_ID) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            return false;
        }
    }
    return true;
}

static bool kind_valid(const char *kind)
{
    static const char *kinds[] = {
        "hello",
        "join",
        "joined",
        "leave",
        "presence",
        "error",
        "snapshot_request",
        "snapshot",
        "snapshot_ack",
        "resync_required",
        "proposal",
        "accepted",
        "rejected",
        "ack",
        "host_changed",
        "room_closed",
    };
    for (size_t i = 0; kind && i < sizeof kinds / sizeof *kinds; i++) {
        if (!strcmp(kind, kinds[i])) {
            return true;
        }
    }
    return false;
}

static bool sequence(yyjson_val *value)
{
    return yyjson_is_uint(value) && yyjson_get_uint(value) <= COLLAB_MAX_SEQUENCE;
}

bool collab_message_read(const char *json, collab_message *message)
{
    *message = (collab_message){ 0 };
    if (!json || strlen(json) > COLLAB_MAX_MESSAGE) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *room = yyjson_get_str(yyjson_obj_get(root, "room"));
    const char *client = yyjson_get_str(yyjson_obj_get(root, "client"));
    const char *kind = yyjson_get_str(yyjson_obj_get(root, "kind"));
    yyjson_val *seq = yyjson_obj_get(root, "seq");
    yyjson_val *revision = yyjson_obj_get(root, "revision");
    yyjson_val *payload = yyjson_obj_get(root, "payload");
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 7 ||
        !yyjson_is_uint(yyjson_obj_get(root, "protocol")) ||
        yyjson_get_uint(yyjson_obj_get(root, "protocol")) != COLLAB_PROTOCOL ||
        !collab_identifier(room) || !collab_identifier(client) || !kind_valid(kind) ||
        !sequence(seq) || !sequence(revision) || !yyjson_is_obj(payload)) {
        yyjson_doc_free(doc);
        return false;
    }
    /* Exactly seven unique known keys: duplicates cannot shadow validation. */
    size_t index, count;
    yyjson_val *key, *value;
    unsigned seen = 0;
    const char *keys[] = { "protocol", "room", "client", "seq",
                           "revision", "kind", "payload" };
    yyjson_obj_foreach(root, index, count, key, value)
    {
        unsigned bit = 0;
        for (unsigned i = 0; i < 7; i++) {
            if (yyjson_equals_str(key, keys[i])) {
                bit = 1u << i;
            }
        }
        if (!bit || (seen & bit)) {
            yyjson_doc_free(doc);
            return false;
        }
        seen |= bit;
    }
    *message = (collab_message){ doc,
                                 room,
                                 client,
                                 kind,
                                 yyjson_get_uint(seq),
                                 yyjson_get_uint(revision),
                                 payload };
    return true;
}

void collab_message_free(collab_message *message)
{
    yyjson_doc_free(message->doc);
    *message = (collab_message){ 0 };
}

char *collab_message_write(
    const char *room,
    const char *client,
    uint64_t seq,
    uint64_t revision,
    const char *kind,
    const char *payload
)
{
    if (!collab_identifier(room) || !collab_identifier(client) || !kind_valid(kind) ||
        seq > COLLAB_MAX_SEQUENCE || revision > COLLAB_MAX_SEQUENCE || !payload) {
        return NULL;
    }
    yyjson_doc *body = yyjson_read(payload, strlen(payload), 0);
    if (!yyjson_is_obj(yyjson_doc_get_root(body))) {
        yyjson_doc_free(body);
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "protocol", COLLAB_PROTOCOL);
    yyjson_mut_obj_add_str(doc, root, "room", room);
    yyjson_mut_obj_add_str(doc, root, "client", client);
    yyjson_mut_obj_add_uint(doc, root, "seq", seq);
    yyjson_mut_obj_add_uint(doc, root, "revision", revision);
    yyjson_mut_obj_add_str(doc, root, "kind", kind);
    yyjson_mut_obj_add_val(doc, root, "payload", yyjson_val_mut_copy(doc, yyjson_doc_get_root(body)));
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(body);
    return json;
}
