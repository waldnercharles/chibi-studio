#include "editor/collab/collab.h"

#include "editor/collab/protocol.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define COLLAB_EXPORT EMSCRIPTEN_KEEPALIVE
EM_JS(void, web_send, (const char *json), {
    if (Module.chibiCollab) {
        Module.chibiCollab.send(UTF8ToString(json));
    }
})
EM_JS(void, web_control, (const char *command, const char *argument), {
    if (Module.chibiCollab) {
        Module.chibiCollab.control(UTF8ToString(command), UTF8ToString(argument));
    }
})
EM_JS(void, web_close, (), {
    if (Module.chibiCollab) {
        Module.chibiCollab.close();
    }
})
#else
#define COLLAB_EXPORT
static void web_send(const char *) {}
static void web_close() {}
#endif

#define COLLAB_QUEUE_LIMIT 256
#define COLLAB_CHECKPOINT_INTERVAL 16

static struct
{
    collab_config config;
    collab_state state;
    collab_role role;
    char room[COLLAB_MAX_ID + 1], client[COLLAB_MAX_ID + 1];
    char status[256];
    bool status_error;
    char feedback[256];
    bool feedback_error;
    uint64_t feedback_revision;
    uint64_t revision, seq, pending_seq, checkpoint_revision, presence_seq;
    uint64_t recovery_revision;
    char pending_client[COLLAB_MAX_ID + 1];
    const char *apply_client;
    char *seed_payload;
    bool seed_ready;
    bool pending_applied, snapshot_requested, snapshot_inflight, closing;
    int replay_depth;
    int recovery_failures;
    char **inbound;
    size_t inbound_bytes;
} session;

/* Monotonic across session resets so browser feedback can compare freshness. */
static uint64_t status_revision;

static void status(collab_state state, const char *text)
{
    session.state = state;
    session.status_error = false;
    status_revision++;
    snprintf(session.status, sizeof session.status, "%s", text);
}

static void status_error(collab_state state, const char *text)
{
    status(state, text);
    session.status_error = true;
}

static void send_at(const char *kind, const char *client, uint64_t seq, uint64_t revision, const char *payload)
{
    char *json = collab_message_write(session.room, client, seq, revision, kind, payload);
    if (!json) {
        return;
    }
    if (session.config.send) {
        session.config.send(session.config.context, json);
    } else {
        web_send(json);
    }
    free(json);
}

static void send_as(const char *kind, const char *client, uint64_t seq, const char *payload)
{
    send_at(kind, client, seq, session.revision, payload);
}

static void send_control(const char *kind, const char *payload)
{
    send_as(kind, session.client, 0, payload);
}

static void clear_pending()
{
    session.pending_seq = 0;
    session.pending_client[0] = 0;
    session.pending_applied = false;
}

static void request_resync(const char *reason)
{
    bool first = session.state != COLLAB_RESYNCING;
    status_error(COLLAB_RESYNCING, reason);
    if (first) {
        char payload[96];
        snprintf(
            payload,
            sizeof payload,
            "{\"last_revision\":%llu}",
            (unsigned long long)(collab_is_host() ? 0
                                 : session.recovery_revision > session.revision
                                     ? session.recovery_revision
                                     : session.revision)
        );
        send_control("snapshot_request", payload);
    }
}

static char *value_json(yyjson_val *value)
{
    return yyjson_val_write(value, 0, NULL);
}

void collab_init(collab_config config)
{
    session.config = config;
}

void collab_shutdown()
{
    web_close();
    free(session.seed_payload);
    for (int i = 0; i < acount(session.inbound); i++) {
        free(session.inbound[i]);
    }
    afree(session.inbound);
    memset(&session, 0, sizeof session);
    status(COLLAB_OFFLINE, "Offline");
}

COLLAB_EXPORT void collab_start(const char *room, const char *client, const char *role)
{
    if (!collab_identifier(room) || !collab_identifier(client) || !role) {
        status_error(COLLAB_OFFLINE, "Invalid room identity");
        return;
    }
    collab_role next_role = !strcmp(role, "host")   ? COLLAB_ROLE_HOST
                          : !strcmp(role, "editor") ? COLLAB_ROLE_EDITOR
                          : !strcmp(role, "viewer") ? COLLAB_ROLE_VIEWER
                                                    : COLLAB_ROLE_NONE;
    if (next_role == COLLAB_ROLE_NONE) {
        status_error(COLLAB_OFFLINE, "Invalid room role");
        return;
    }
    bool same = !strcmp(session.room, room) && !strcmp(session.client, client);
    if (!same) {
        session.revision = session.seq = session.checkpoint_revision = 0;
        session.recovery_revision = 0;
        session.seed_ready = false;
        free(session.seed_payload);
        session.seed_payload = NULL;
        clear_pending();
    }
    snprintf(session.room, sizeof session.room, "%s", room);
    snprintf(session.client, sizeof session.client, "%s", client);
    session.role = next_role;
    session.snapshot_requested = session.snapshot_inflight = false;
    session.recovery_revision = 0;
    session.recovery_failures = 0;
    session.closing = false;
    status(COLLAB_CONNECTING, "Connecting to room");
}

COLLAB_EXPORT void collab_seed(const char *payload)
{
    if (!collab_is_host() || session.state != COLLAB_CONNECTING || !payload ||
        strlen(payload) > COLLAB_MAX_MESSAGE) {
        return;
    }
    free(session.seed_payload);
    session.seed_payload = xstrdup(payload);
    session.seed_ready = false;
}

COLLAB_EXPORT void collab_receive(const char *envelope)
{
    if (!envelope) {
        return;
    }
    size_t length = strlen(envelope);
    if (length > COLLAB_MAX_MESSAGE || acount(session.inbound) >= COLLAB_QUEUE_LIMIT ||
        session.inbound_bytes + length > 2 * COLLAB_MAX_MESSAGE) {
        request_resync("Room receive queue overflow; recovering");
        return;
    }
    apush(session.inbound, xstrdup(envelope));
    session.inbound_bytes += length;
}

COLLAB_EXPORT void collab_disconnect(const char *reason)
{
    if (collab_active()) {
        status_error(COLLAB_CONNECTING, reason ? reason : "Room connection lost; reconnecting");
    }
}

COLLAB_EXPORT void collab_leave()
{
    free(session.seed_payload);
    session.seed_payload = NULL;
    if (session.role != COLLAB_ROLE_NONE) {
        send_control("leave", "{}");
    }
    web_close();
    session.closing = true;
    status(COLLAB_CLOSED, "Left room");
}

COLLAB_EXPORT void collab_control(const char *command, const char *argument)
{
    if (!strcmp(command, "leave")) {
        collab_leave();
        return;
    }
#ifdef __EMSCRIPTEN__
    web_control(command, argument ? argument : "");
#else
    (void)argument;
    status(session.state, "Rooms are available in the web editor");
#endif
}

COLLAB_EXPORT void collab_feedback(const char *message, int error)
{
    snprintf(session.feedback, sizeof session.feedback, "%s", message ? message : "");
    session.feedback_error = error != 0;
    session.feedback_revision = status_revision;
}

static bool feedback_current()
{
    return session.feedback[0] && session.feedback_revision == status_revision;
}

const char *collab_feedback_text()
{
    return feedback_current() ? session.feedback : "";
}

bool collab_feedback_error()
{
    return feedback_current() ? session.feedback_error : collab_status_error();
}

bool collab_rooms_available()
{
#ifdef __EMSCRIPTEN__
    return true;
#else
    return false;
#endif
}

bool collab_active()
{
    return session.role != COLLAB_ROLE_NONE;
}
bool collab_is_host()
{
    return session.role == COLLAB_ROLE_HOST;
}
bool collab_can_edit()
{
    return collab_replaying() || !collab_active() ||
           (session.state == COLLAB_READY && !session.pending_seq &&
            !session.snapshot_inflight && session.role != COLLAB_ROLE_VIEWER);
}
collab_role collab_role_current()
{
    return session.role;
}
collab_state collab_state_current()
{
    return session.state;
}
const char *collab_status()
{
    return *session.status ? session.status : "Offline";
}
uint64_t collab_status_revision()
{
    return status_revision;
}
bool collab_status_error()
{
    return session.status_error;
}
const char *collab_client()
{
    return session.client;
}
const char *collab_apply_client()
{
    return session.apply_client ? session.apply_client : "";
}
uint64_t collab_revision()
{
    return session.revision;
}
bool collab_replaying()
{
    return session.replay_depth > 0;
}
bool collab_record_history()
{
    return !collab_active() || !collab_replaying() || collab_is_host();
}
void collab_begin_replay()
{
    session.replay_depth++;
}
void collab_end_replay()
{
    assert(session.replay_depth > 0);
    session.replay_depth--;
}

static bool action_send(const char *action, bool applied)
{
    if (collab_replaying() || !collab_active()) {
        return true;
    }
    if (!collab_can_edit() || session.seq == COLLAB_MAX_SEQUENCE ||
        (applied && !collab_is_host())) {
        return false;
    }
    if (!action || strlen(action) > COLLAB_MAX_ACTION) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(action, strlen(action), 0);
    if (!yyjson_is_obj(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_doc_free(doc);
    session.pending_seq = ++session.seq;
    snprintf(
        session.pending_client,
        sizeof session.pending_client,
        "%s",
        session.client
    );
    session.pending_applied = applied;
    send_as(applied ? "accepted" : "proposal", session.client, session.seq, action);
    return true;
}

bool collab_publish(const char *action)
{
    return action_send(action, true);
}
bool collab_propose(const char *action)
{
    return action_send(action, false);
}

void collab_presence(const char *presentation)
{
    if (session.state == COLLAB_READY && !collab_replaying()) {
        yyjson_doc *data = yyjson_read(presentation, strlen(presentation), 0);
        if (!yyjson_is_obj(yyjson_doc_get_root(data))) {
            yyjson_doc_free(data);
            return;
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *body = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, body);
        yyjson_mut_obj_add_uint(doc, body, "presence_seq", ++session.presence_seq);
        yyjson_mut_obj_add_val(doc, body, "data", yyjson_val_mut_copy(doc, yyjson_doc_get_root(data)));
        char *json = yyjson_mut_write(doc, 0, NULL);
        send_control("presence", json);
        free(json);
        yyjson_mut_doc_free(doc);
        yyjson_doc_free(data);
    }
}

COLLAB_EXPORT void collab_request_snapshot()
{
    if (collab_is_host()) {
        session.snapshot_requested = true;
    } else if (collab_active()) {
        if (session.state == COLLAB_RESYNCING) {
            session.state = COLLAB_LOADING;
        }
        request_resync("Recovering room workspace");
    }
}

static bool apply(const char *client, const char *action, char *error, size_t size)
{
    if (!session.config.apply) {
        snprintf(error, size, "Editor replay is unavailable");
        return false;
    }
    collab_begin_replay();
    session.apply_client = client;
    bool ok = session.config.apply(session.config.context, action, error, size);
    session.apply_client = NULL;
    collab_end_replay();
    return ok;
}

static void reject(const collab_message *message, const char *reason)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, body);
    yyjson_mut_obj_add_str(doc, body, "reason", strlen(reason) <= 128 ? reason : "Editor rejected invalid or stale arguments");
    char *json = yyjson_mut_write(doc, 0, NULL);
    send_at("rejected", message->client, message->seq, message->revision, json);
    free(json);
    yyjson_mut_doc_free(doc);
}

static void proposal(const collab_message *message)
{
    if (!collab_is_host()) {
        return;
    }
    if (session.state != COLLAB_READY || session.pending_seq || session.snapshot_inflight ||
        message->revision != session.revision || !message->seq) {
        reject(message, "Host is busy or proposal revision is stale");
        return;
    }
    char error[256] = "Invalid editor action";
    char *json = value_json(message->payload);
    bool ok = apply(message->client, json, error, sizeof error);
    if (!ok) {
        reject(message, error);
        free(json);
        return;
    }
    session.pending_seq = message->seq;
    snprintf(
        session.pending_client,
        sizeof session.pending_client,
        "%s",
        message->client
    );
    session.pending_applied = true;
    send_as("accepted", message->client, message->seq, json);
    free(json);
}

static void accepted(const collab_message *message)
{
    if (message->revision <= session.revision) {
        send_control("ack", "{}");
        return;
    }
    if (session.state != COLLAB_READY || message->revision != session.revision + 1) {
        if (!collab_is_host() && message->revision > session.recovery_revision) {
            session.recovery_revision = message->revision;
        }
        request_resync("Room action sequence gap; recovering");
        return;
    }
    bool pending = message->seq == session.pending_seq &&
                   !strcmp(message->client, session.pending_client);
    if (!(pending && session.pending_applied)) {
        char error[256] = "Room action could not be replayed";
        char *json = value_json(message->payload);
        bool ok = apply(message->client, json, error, sizeof error);
        free(json);
        if (!ok) {
            if (!collab_is_host()) {
                session.recovery_revision = message->revision;
            }
            request_resync(error);
            return;
        }
    }
    session.revision = message->revision;
    if (pending) {
        clear_pending();
    }
    send_control("ack", "{}");
    if (collab_is_host() && session.revision - session.checkpoint_revision >=
                                COLLAB_CHECKPOINT_INTERVAL) {
        session.snapshot_requested = true;
    }
}

static void retry_snapshot(const char *reason)
{
    if (++session.recovery_failures >= 3) {
        collab_leave();
        status_error(COLLAB_CLOSED, "Could not restore room after three attempts; left room");
        return;
    }
    session.state = COLLAB_LOADING;
    request_resync(reason);
}

static void snapshot(const collab_message *message)
{
    if (message->revision < session.recovery_revision) {
        return;
    }
    if (yyjson_get_int(yyjson_obj_get(message->payload, "checkpoint_version")) != 1 ||
        !yyjson_is_str(yyjson_obj_get(message->payload, "workspace")) ||
        !session.config.restore) {
        retry_snapshot("Unsupported room checkpoint");
        return;
    }
    char error[256] = "Room checkpoint could not be restored";
    char *json = value_json(message->payload);
    collab_begin_replay();
    bool ok = session.config.restore(session.config.context, json, error, sizeof error);
    collab_end_replay();
    free(json);
    if (!ok) {
        retry_snapshot(error);
        return;
    }
    session.revision = session.checkpoint_revision = message->revision;
    session.recovery_revision = 0;
    session.recovery_failures = 0;
    clear_pending();
    session.snapshot_inflight = false;
    status(COLLAB_READY, "Room connected");
    const char *hash = yyjson_get_str(yyjson_obj_get(message->payload, "sha256"));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, body);
    yyjson_mut_obj_add_uint(doc, body, "revision", session.revision);
    yyjson_mut_obj_add_str(doc, body, "sha256", hash ? hash : "");
    json = yyjson_mut_write(doc, 0, NULL);
    send_control("snapshot_ack", json);
    free(json);
    yyjson_mut_doc_free(doc);
}

static void joined(const collab_message *message)
{
    const char *role = yyjson_get_str(yyjson_obj_get(message->payload, "role"));
    if (!role || (strcmp(role, "host") && strcmp(role, "editor") &&
                  strcmp(role, "viewer"))) {
        request_resync("Invalid room membership");
        return;
    }
    collab_role assigned = !strcmp(role, "host")   ? COLLAB_ROLE_HOST
                         : !strcmp(role, "editor") ? COLLAB_ROLE_EDITOR
                                                   : COLLAB_ROLE_VIEWER;
    if (assigned != session.role || strcmp(message->client, session.client)) {
        status_error(COLLAB_CLOSED, "Room membership does not match the invitation");
        web_close();
        session.closing = true;
        return;
    }
    yyjson_val *next = yyjson_obj_get(message->payload, "next_seq");
    if (yyjson_is_uint(next) && yyjson_get_uint(next) > 0) {
        session.seq = yyjson_get_uint(next) - 1;
    }
    status(COLLAB_LOADING, "Loading room checkpoint");
    /* Only a brand new host may seed a room. A reconnect loads the last
     * server checkpoint and log, including an accepted edit whose ack was lost. */
    if (collab_is_host() && message->revision == 0 && !session.pending_seq &&
        !yyjson_is_uint(yyjson_obj_get(message->payload, "snapshot_revision"))) {
        session.revision = 0;
        session.snapshot_requested = true;
    }
}

static void dispatch(const collab_message *message)
{
    if (strcmp(message->room, session.room)) {
        return;
    }
    const char *kind = message->kind;
    if (!strcmp(kind, "joined")) {
        joined(message);
    } else if (!strcmp(kind, "accepted")) {
        accepted(message);
    } else if (!strcmp(kind, "proposal")) {
        proposal(message);
    } else if (!strcmp(kind, "snapshot")) {
        snapshot(message);
    } else if (!strcmp(kind, "snapshot_request")) {
        if (collab_is_host()) {
            session.snapshot_requested = true;
        }
    } else if (!strcmp(kind, "snapshot_ack")) {
        if (collab_is_host() && session.snapshot_inflight &&
            message->revision == session.revision) {
            session.snapshot_inflight = false;
            session.checkpoint_revision = session.revision;
            status(COLLAB_READY, "Room connected");
        }
    } else if (!strcmp(kind, "resync_required")) {
        request_resync("Room requires a fresh checkpoint");
    } else if (!strcmp(kind, "rejected")) {
        if (message->seq == session.pending_seq &&
            !strcmp(message->client, session.pending_client)) {
            bool applied = session.pending_applied;
            clear_pending();
            const char *reason = yyjson_get_str(yyjson_obj_get(message->payload, "reason"));
            if (applied) {
                request_resync(reason ? reason : "Host action rejected; recovering");
            } else {
                status_error(session.state, reason ? reason : "Edit rejected");
            }
        }
    } else if (!strcmp(kind, "presence") && session.config.presence) {
        const char *name = yyjson_get_str(yyjson_obj_get(message->payload, "name"));
        uint32_t color = (uint32_t)yyjson_get_uint(yyjson_obj_get(message->payload, "color"));
        yyjson_val *data = yyjson_obj_get(message->payload, "data");
        char *json = value_json(yyjson_is_obj(data) ? data : message->payload);
        session.config.presence(
            session.config.context,
            message->client,
            name ? name : message->client,
            color,
            json
        );
        free(json);
    } else if (!strcmp(kind, "leave") && session.config.presence) {
        session.config.presence(session.config.context, message->client, message->client, 0, NULL);
    } else if (!strcmp(kind, "room_closed")) {
        status_error(COLLAB_CLOSED, "Room closed: host departed");
        web_close();
        session.closing = true;
    } else if (!strcmp(kind, "host_changed")) {
        request_resync("Room authority changed; recovering");
    } else if (!strcmp(kind, "error")) {
        const char *reason = yyjson_get_str(yyjson_obj_get(message->payload, "reason"));
        status_error(session.state, reason ? reason : "Room service error");
    }
}

static void checkpoint()
{
    if (!session.snapshot_requested || session.snapshot_inflight ||
        session.pending_seq || !collab_is_host() || !session.config.checkpoint ||
        (session.state != COLLAB_READY && session.state != COLLAB_LOADING)) {
        return;
    }
    char *workspace = session.config.checkpoint(session.config.context);
    if (!workspace) {
        status_error(session.state, "Could not capture room checkpoint");
        return;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, body);
    yyjson_mut_obj_add_int(doc, body, "checkpoint_version", 1);
    yyjson_mut_obj_add_str(doc, body, "workspace", workspace);
    char *json = yyjson_mut_write(doc, 0, NULL);
    session.snapshot_requested = false;
    session.snapshot_inflight = true;
    send_control("snapshot", json);
    free(workspace);
    free(json);
    yyjson_mut_doc_free(doc);
}

void collab_tick()
{
    if (session.seed_payload && !session.closing) {
        char *payload = session.seed_payload;
        session.seed_payload = NULL;
        char error[256] = "Could not prepare room workspace";
        collab_begin_replay();
        session.seed_ready = session.config.restore &&
                             session.config.restore(session.config.context, payload, error, sizeof error);
        collab_end_replay();
        free(payload);
        if (session.seed_ready) {
            status(COLLAB_CONNECTING, "Room workspace prepared");
        } else {
            status_error(COLLAB_CONNECTING, error);
        }
    }
    char **messages = session.inbound;
    session.inbound = NULL;
    session.inbound_bytes = 0;
    for (int i = 0; i < acount(messages); i++) {
        collab_message message;
        if (!session.closing && collab_active()) {
            if (collab_message_read(messages[i], &message)) {
                dispatch(&message);
                collab_message_free(&message);
            } else {
                request_resync("Malformed room message; recovering");
            }
        }
        free(messages[i]);
    }
    afree(messages);
    if (session.closing) {
        session.closing = false;
        session.role = COLLAB_ROLE_NONE;
        clear_pending();
        if (session.config.closed) {
            session.config.closed(session.config.context);
        }
        return;
    }
    checkpoint();
}

COLLAB_EXPORT char *collab_inspect()
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "state", session.state);
    yyjson_mut_obj_add_int(doc, root, "role", session.role);
    yyjson_mut_obj_add_uint(doc, root, "revision", session.revision);
    yyjson_mut_obj_add_uint(doc, root, "pending", session.pending_seq);
    yyjson_mut_obj_add_bool(doc, root, "seed_ready", session.seed_ready);
    yyjson_mut_obj_add_str(doc, root, "status", collab_status());
    char *workspace = session.config.checkpoint
                        ? session.config.checkpoint(session.config.context)
                        : NULL;
    if (workspace) {
        yyjson_mut_obj_add_str(doc, root, "workspace", workspace);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    free(workspace);
    yyjson_mut_doc_free(doc);
    return json;
}
