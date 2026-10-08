#ifndef CHIBI_COLLAB_H
#define CHIBI_COLLAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COLLAB_MAX_ACTION (60 * 1024)

typedef enum
{
    COLLAB_ROLE_NONE,
    COLLAB_ROLE_HOST,
    COLLAB_ROLE_EDITOR,
    COLLAB_ROLE_VIEWER,
} collab_role;

typedef enum
{
    COLLAB_OFFLINE,
    COLLAB_CONNECTING,
    COLLAB_LOADING,
    COLLAB_READY,
    COLLAB_RESYNCING,
    COLLAB_CLOSED,
} collab_state;

/* One room belongs to the application, not a document or a transport. All
 * callbacks run on the frame thread. JSON strings are borrowed except for
 * the owned string returned by checkpoint. */
typedef struct
{
    void *context;
    void (*send)(void *context, const char *json);
    bool (*apply)(void *context, const char *action, char *error, size_t size);
    char *(*checkpoint)(void *context);
    bool (*restore)(void *context, const char *payload, char *error, size_t size);
    void (*presence)(void *context, const char *client, const char *name, uint32_t color, const char *json);
    void (*closed)(void *context);
} collab_config;

void collab_init(collab_config config);
void collab_shutdown();
void collab_tick();
void collab_start(const char *room, const char *client, const char *role);
/* Prepare a host's temporary workspace without reloading unsaved documents.
 * Completion is reported as seed_ready in collab_inspect(). */
void collab_seed(const char *payload);
void collab_receive(const char *envelope);
void collab_disconnect(const char *reason);
void collab_leave();
void collab_control(const char *command, const char *argument);
void collab_feedback(const char *message, int error);

bool collab_active();
bool collab_rooms_available();
bool collab_is_host();
bool collab_can_edit();
collab_role collab_role_current();
collab_state collab_state_current();
const char *collab_status();
/* Status updates have their own monotonic revision, independent of edits. */
uint64_t collab_status_revision();
bool collab_status_error();
/* Browser feedback expires when a later model status arrives. */
const char *collab_feedback_text();
bool collab_feedback_error();
const char *collab_client();
const char *collab_apply_client();
uint64_t collab_revision();
bool collab_replaying();
bool collab_record_history();
void collab_begin_replay();
void collab_end_replay();

/* Publish only after a successful local mutation. Editor proposals are sent
 * before mutation; the accepted echo is their only application. */
bool collab_publish(const char *action);
bool collab_propose(const char *action);
void collab_presence(const char *presentation);
void collab_request_snapshot();
/* Owned inspection JSON for model-based browser smoke checks. */
char *collab_inspect();

#endif
