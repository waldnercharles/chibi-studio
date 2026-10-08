#include "editor/collab/protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_collab_protocol()
{
    collab_message message;
    char *json = collab_message_write("room", "peer", 3, 9, "accepted", "{\"family\":\"graph\"}");
    assert(json && collab_message_read(json, &message));
    assert(message.seq == 3 && message.revision == 9 && !strcmp(message.kind, "accepted"));
    collab_message_free(&message);
    /* Every truncated prefix fails instead of producing a partial action. */
    for (size_t i = 0; i < strlen(json); i++) {
        char saved = json[i];
        json[i] = 0;
        assert(!collab_message_read(json, &message));
        json[i] = saved;
    }
    free(json);
    const char *invalid[] = {
        "null",
        "[]",
        "{}",
        "{\"protocol\":2,\"room\":\"r\",\"client\":\"c\",\"seq\":1,"
        "\"revision\":0,\"kind\":\"accepted\",\"payload\":{}}",
        "{\"protocol\":1,\"room\":\"../"
        "r\",\"client\":\"c\",\"seq\":1,\"revision\":0,\"kind\":\"accepted\","
        "\"payload\":{}}",
        "{\"protocol\":1,\"room\":\"r\",\"client\":\"c\",\"seq\":-1,"
        "\"revision\":0,\"kind\":\"accepted\",\"payload\":{}}",
        "{\"protocol\":1,\"room\":\"r\",\"client\":\"c\",\"seq\":1.5,"
        "\"revision\":0,\"kind\":\"accepted\",\"payload\":{}}",
        "{\"protocol\":1,\"room\":\"r\",\"client\":\"c\",\"seq\":"
        "9007199254740992,\"revision\":0,\"kind\":\"accepted\",\"payload\":{}}",
        "{\"protocol\":1,\"room\":\"r\",\"client\":\"c\",\"seq\":1,"
        "\"revision\":0,\"kind\":\"unknown\",\"payload\":{}}",
        "{\"protocol\":1,\"room\":\"r\",\"client\":\"c\",\"seq\":1,"
        "\"revision\":0,\"kind\":\"accepted\",\"payload\":[]}",
        "{\"protocol\":1,\"room\":\"r\",\"client\":\"c\",\"seq\":1,"
        "\"revision\":0,\"kind\":\"accepted\",\"payload\":{},\"seq\":2}",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        assert(!collab_message_read(invalid[i], &message));
    }
    assert(!collab_message_write("r", "c", 0, 0, "unknown", "{}"));
    assert(!collab_message_write("r", "c", 0, 0, "accepted", "[]"));
    puts("collaboration protocol PASS: envelopes, truncation, version, identity, sequence bounds");
}
