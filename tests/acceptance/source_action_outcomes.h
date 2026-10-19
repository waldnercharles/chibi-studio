#include "editor/collab/protocol.h"

/* Fixed in-process transport workload: 28 incoming envelopes, 32 sends,
 * four apply callbacks (one failing), eight restores, two closes. No socket,
 * source mutation, rendering, sleeps, or production test hooks. */
static struct
{
    int received, sent, proposals, accepted, acks, requests, snapshots, leaves;
    int applies, restores, closes;
    bool valid, fail_apply;
    char client[COLLAB_MAX_ID + 1];
} source_outcome_test;

static void source_outcome_send(void *, const char *json)
{
    collab_message message;
    if (!collab_message_read(json, &message)) {
        source_outcome_test.valid = false;
        return;
    }
    source_outcome_test.sent++;
    if (!strcmp(message.kind, "proposal")) {
        source_outcome_test.proposals++;
    } else if (!strcmp(message.kind, "accepted")) {
        source_outcome_test.accepted++;
    } else if (!strcmp(message.kind, "ack")) {
        source_outcome_test.acks++;
    } else if (!strcmp(message.kind, "snapshot_request")) {
        source_outcome_test.requests++;
    } else if (!strcmp(message.kind, "snapshot_ack")) {
        source_outcome_test.snapshots++;
    } else if (!strcmp(message.kind, "leave")) {
        source_outcome_test.leaves++;
    } else {
        source_outcome_test.valid = false;
    }
    collab_message_free(&message);
}

static bool source_outcome_apply(void *, const char *action, char *error, size_t size)
{
    source_outcome_test.applies++;
    source_outcome_test.valid &= collab_replaying() && !strcmp(action, "{}");
    snprintf(source_outcome_test.client, sizeof source_outcome_test.client, "%s", collab_apply_client());
    if (source_outcome_test.fail_apply) {
        snprintf(error, size, "Replay failed");
        return false;
    }
    return true;
}

static bool source_outcome_restore(void *, const char *, char *, size_t)
{
    source_outcome_test.restores++;
    source_outcome_test.valid &= collab_replaying();
    return true;
}

static void source_outcome_closed(void *)
{
    source_outcome_test.closes++;
}

static bool source_outcome_receive(
    const char *room,
    const char *client,
    uint64_t seq,
    uint64_t revision,
    const char *kind,
    const char *payload
)
{
    char *json = collab_message_write(room, client, seq, revision, kind, payload);
    REQUIRE(json);
    source_outcome_test.received++;
    collab_receive(json);
    free(json);
    collab_tick();
    REQUIRE(source_outcome_test.valid);
    return true;
}

static bool source_outcome_snapshot(const char *room, uint64_t revision)
{
    return source_outcome_receive(room, "local", 0, revision, "snapshot", "{\"checkpoint_version\":1,\"workspace\":\"\"}");
}

static bool source_outcome_ready(const char *room, const char *role)
{
    collab_start(room, "local", role);
    char payload[128];
    snprintf(payload, sizeof payload, "{\"role\":\"%s\",\"next_seq\":1,\"snapshot_revision\":0}", role);
    REQUIRE(source_outcome_receive(room, "local", 0, 0, "joined", payload));
    REQUIRE(source_outcome_snapshot(room, 0));
    REQUIRE(collab_state_current() == COLLAB_READY && collab_can_edit());
    return true;
}

static bool source_outcome_matches(
    collab_action_outcome *out,
    const collab_action_identity *action,
    collab_action_kind kind,
    collab_action_interruption interruption,
    uint64_t revision
)
{
    REQUIRE(collab_latest_outcome(out));
    REQUIRE(out->action.id == action->id && out->action.seq == action->seq);
    REQUIRE(!strcmp(out->action.room, action->room));
    REQUIRE(!strcmp(out->action.client, action->client));
    REQUIRE(out->kind == kind && out->interruption == interruption && out->revision == revision);
    return true;
}

static bool source_action_outcomes_check()
{
    collab_action_identity action, pending;
    collab_action_outcome before, out, accepted;
    collab_latest_outcome(&before);
    REQUIRE(source_outcome_ready("source-room", "editor"));
    REQUIRE(!collab_pending_action(&pending) && !pending.id);
    REQUIRE(collab_propose("{}") && collab_pending_action(&action));
    REQUIRE(action.id && action.seq == 1);
    REQUIRE(!strcmp(action.room, "source-room") && !strcmp(action.client, "local"));
    REQUIRE(!collab_propose("{}") && !collab_can_edit());
    REQUIRE(source_outcome_receive("source-room", "peer", action.seq, 0, "rejected", "{}"));
    REQUIRE(source_outcome_receive("source-room", "local", action.seq + 1, 0, "rejected", "{}"));
    REQUIRE(source_outcome_receive("other-room", "local", action.seq, 0, "rejected", "{}"));
    collab_latest_outcome(&out);
    REQUIRE(out.serial == before.serial);
    REQUIRE(source_outcome_receive("source-room", "peer", 40, 1, "accepted", "{}"));
    REQUIRE(source_outcome_test.applies == 1 && !strcmp(source_outcome_test.client, "peer"));
    REQUIRE(collab_pending_action(&pending) && pending.id == action.id);
    collab_latest_outcome(&out);
    REQUIRE(out.serial == before.serial);
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 2, "accepted", "{}"));
    REQUIRE(
        source_outcome_matches(&accepted, &action, COLLAB_ACTION_ACCEPTED, COLLAB_ACTION_UNINTERRUPTED, 2)
    );
    REQUIRE(accepted.serial > before.serial && !accepted.message[0]);
    REQUIRE(!collab_pending_action(&pending) && !pending.id && collab_can_edit());
    REQUIRE(source_outcome_test.applies == 2 && !strcmp(source_outcome_test.client, "local"));
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 2, "accepted", "{}"));
    collab_latest_outcome(&out);
    REQUIRE(out.serial == accepted.serial && source_outcome_test.applies == 2);

    REQUIRE(collab_propose("{}") && collab_pending_action(&action));
    REQUIRE(action.id > accepted.action.id && action.seq == 2);
    char reason[401], rejection[448];
    memset(reason, 'x', sizeof reason - 1);
    reason[sizeof reason - 1] = 0;
    snprintf(rejection, sizeof rejection, "{\"reason\":\"%s\"}", reason);
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 2, "rejected", rejection));
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_REJECTED, COLLAB_ACTION_UNINTERRUPTED, 0)
    );
    REQUIRE(strlen(out.message) == sizeof out.message - 1 && out.message[254] == 'x');
    REQUIRE(!collab_pending_action(&pending) && source_outcome_test.applies == 2);
    REQUIRE(accepted.kind == COLLAB_ACTION_ACCEPTED && accepted.revision == 2);

    REQUIRE(collab_propose("{}") && collab_pending_action(&action));
    collab_disconnect("Connection interrupted");
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_DISCONNECTED, 0)
    );
    REQUIRE(!strcmp(out.message, "Connection interrupted"));
    REQUIRE(collab_pending_action(&pending) && pending.id == action.id && !collab_can_edit());
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 3, "accepted", "{}"));
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESYNC, 0)
    );
    REQUIRE(source_outcome_snapshot("source-room", 3));
    REQUIRE(!collab_pending_action(&pending));
    REQUIRE(
        source_outcome_matches(&before, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESYNC, 0)
    );
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 3, "accepted", "{}"));
    collab_latest_outcome(&out);
    REQUIRE(out.serial == before.serial && source_outcome_test.applies == 2);

    REQUIRE(collab_propose("{}") && collab_pending_action(&action));
    collab_request_snapshot();
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESYNC, 0)
    );
    REQUIRE(collab_pending_action(&pending) && pending.id == action.id);
    REQUIRE(source_outcome_snapshot("source-room", 3));
    REQUIRE(!collab_pending_action(&pending));

    REQUIRE(collab_propose("{}") && collab_pending_action(&action));
    source_outcome_test.fail_apply = true;
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 4, "accepted", "{}"));
    source_outcome_test.fail_apply = false;
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESYNC, 0)
    );
    REQUIRE(!strcmp(out.message, "Replay failed") && collab_revision() == 3);
    REQUIRE(source_outcome_snapshot("source-room", 4));
    REQUIRE(
        source_outcome_matches(&before, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESYNC, 0)
    );
    REQUIRE(source_outcome_receive("source-room", "local", action.seq, 4, "accepted", "{}"));
    collab_latest_outcome(&out);
    REQUIRE(out.serial == before.serial && source_outcome_test.applies == 3);

    REQUIRE(collab_propose("{}") && collab_pending_action(&action));
    collab_start("invalid!", "local", "editor");
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESET, 0)
    );
    REQUIRE(collab_state_current() == COLLAB_OFFLINE && collab_pending_action(&pending));
    REQUIRE(pending.id == action.id);
    collab_start("source-room", "local", "invalid");
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESET, 0)
    );
    REQUIRE(collab_state_current() == COLLAB_OFFLINE && collab_pending_action(&pending));
    REQUIRE(pending.id == action.id);
    collab_start("next-room", "local", "editor");
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESET, 0)
    );
    REQUIRE(!collab_pending_action(&pending));
    REQUIRE(source_outcome_ready("next-room", "editor"));
    REQUIRE(collab_propose("{}") && collab_pending_action(&pending));
    REQUIRE(pending.id > action.id && pending.seq == 1 && !strcmp(pending.room, "next-room"));
    action = pending;
    collab_shutdown();
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_RESET, 0)
    );
    REQUIRE(!collab_pending_action(&pending));
    collab_init((collab_config){ .send = source_outcome_send,
                                 .apply = source_outcome_apply,
                                 .restore = source_outcome_restore,
                                 .closed = source_outcome_closed });
    REQUIRE(source_outcome_ready("next-room", "editor"));
    REQUIRE(collab_propose("{}") && collab_pending_action(&pending));
    REQUIRE(pending.id > action.id && pending.seq == 1);
    action = pending;
    collab_leave();
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_CLOSED, 0)
    );
    collab_tick();
    REQUIRE(!collab_active() && !collab_pending_action(&pending));

    REQUIRE(source_outcome_ready("host-room", "host"));
    REQUIRE(collab_publish("{}") && collab_pending_action(&action));
    REQUIRE(source_outcome_receive("host-room", "local", action.seq, 1, "accepted", "{}"));
    REQUIRE(
        source_outcome_matches(&accepted, &action, COLLAB_ACTION_ACCEPTED, COLLAB_ACTION_UNINTERRUPTED, 1)
    );
    REQUIRE(source_outcome_test.applies == 3 && !collab_pending_action(&pending));
    REQUIRE(source_outcome_receive("host-room", "peer", 10, 1, "proposal", "{}"));
    REQUIRE(source_outcome_test.applies == 4 && !strcmp(source_outcome_test.client, "peer"));
    REQUIRE(!collab_pending_action(&pending) && !collab_can_edit());
    collab_latest_outcome(&out);
    REQUIRE(out.serial == accepted.serial);
    REQUIRE(source_outcome_receive("host-room", "peer", 10, 2, "accepted", "{}"));
    collab_latest_outcome(&out);
    REQUIRE(
        out.serial == accepted.serial && source_outcome_test.applies == 4 &&
        collab_can_edit()
    );
    REQUIRE(collab_publish("{}") && collab_pending_action(&action));
    REQUIRE(
        source_outcome_receive("host-room", "local", action.seq, 2, "rejected", "{\"reason\":\"Host rejected\"}")
    );
    REQUIRE(
        source_outcome_matches(&before, &action, COLLAB_ACTION_REJECTED, COLLAB_ACTION_UNINTERRUPTED, 0)
    );
    REQUIRE(!strcmp(before.message, "Host rejected") && collab_state_current() == COLLAB_RESYNCING);
    REQUIRE(source_outcome_snapshot("host-room", 2));
    collab_latest_outcome(&out);
    REQUIRE(out.serial == before.serial);
    REQUIRE(collab_publish("{}") && collab_pending_action(&action));
    REQUIRE(source_outcome_receive("host-room", "server", 0, 2, "room_closed", "{}"));
    REQUIRE(
        source_outcome_matches(&out, &action, COLLAB_ACTION_INTERRUPTED, COLLAB_ACTION_CLOSED, 0)
    );
    REQUIRE(!collab_active() && !collab_pending_action(&pending));

    REQUIRE(source_outcome_test.received == 28 && source_outcome_test.sent == 32);
    REQUIRE(source_outcome_test.proposals == 8 && source_outcome_test.accepted == 4);
    REQUIRE(source_outcome_test.acks == 7 && source_outcome_test.requests == 4);
    REQUIRE(source_outcome_test.snapshots == 8 && source_outcome_test.leaves == 1);
    REQUIRE(
        source_outcome_test.applies == 4 && source_outcome_test.restores == 8 &&
        source_outcome_test.closes == 2
    );
    return true;
}

/* Call only from the dedicated acceptance selector with no live room. The
 * fixture releases its transport configuration even after a failed REQUIRE. */
static bool source_action_outcomes_contract()
{
    REQUIRE(!collab_active());
    collab_shutdown();
    memset(&source_outcome_test, 0, sizeof source_outcome_test);
    source_outcome_test.valid = true;
    collab_init((collab_config){ .send = source_outcome_send,
                                 .apply = source_outcome_apply,
                                 .restore = source_outcome_restore,
                                 .closed = source_outcome_closed });
    bool ok = source_action_outcomes_check();
    collab_shutdown();
    return ok;
}
