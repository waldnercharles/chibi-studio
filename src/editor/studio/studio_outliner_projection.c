#include "editor/studio/studio_outliner_projection.h"

#include "authoring/studio_outliner_definition.h"
#include "capabilities/catalog.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_outliner_work.h"
#include "editor/studio/studio_scene_read.h"
#include "execution/authority.h"
#include "execution/status.h"
#include "runtime/behaviors/source_authority.h"
#include "runtime/behaviors/source_guard.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "types/record.h"
#include "types/value_document.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frozen shipped closure: docs/evidence/outliner-request-work/census.md.
 * 14 reachable canonical bodies, 249 BF_CHECK sites; census.log SHA-256:
 * 530ceb49ea65144694a986b57b26829d430725c24dab3f7663617388f80ecc70
 * Authored source, lowerer and context/prepared-ABI hashes are pinned there.
 * Requalify after changes to those inputs or checkpoint lowering/accounting.
 * These hashes document the bound; producer validation grants authority. */
#define STUDIO_OUTLINER_CANONICAL_CHECKPOINT_WEIGHT ((uintptr_t)249)

typedef struct
{
    uint64_t id, generation, controller, scene_session;
    ui_mount *mount;
    const app_scene_authoring *scene;
    const ecs_world *scene_world;
} outliner_binding;

typedef struct
{
    execution_dispatch *token;
    ui_activation_origin origin;
    uint64_t generation, controller, event, scene_session;
    const app_scene_authoring *scene;
    const ecs_world *scene_world;
    bool internal, published;
    arena arguments;
    pin_value *payload;
    int payload_count;
    execution_authority authority;
    studio_scene_read *read;
    pin_value state[4];
} outliner_request;

typedef struct
{
    arena arguments;
    pin_value *payload;
    int count;
    uint64_t generation, scene_session;
    const app_scene_authoring *scene;
    const ecs_world *scene_world;
} outliner_reveal;

struct studio_outliner_projection
{
    studio *studio;
    source_registry *sources;
    ecs_world *world;
    ui *model;
    const execution_module *module;
    uint64_t generation;
    studio_outliner_service service;
    outliner_binding *bindings;
    outliner_request **requests;
    outliner_reveal reveal;
    studio_scene_read_scope scope;
    outliner_request *reading;
    bool retired;
    char error[256];
    char replay_error[256];
};

static uint64_t identity;
static uint64_t next_identity()
{
    if (identity == UINT64_MAX) {
        abort();
    }
    return ++identity;
}

static const execution_module *producer(const source_module_view *modules, int count)
{
    for (int i = 0; i < count; i++) {
        if (!strcmp(modules[i].name, "OutlinerRows")) {
            return modules[i].module;
        }
    }
    return NULL;
}

static bool validate(
    void *,
    const source_module_view *current,
    int current_count,
    const source_module_view *candidate,
    int candidate_count,
    char *error,
    size_t size
)
{
    const execution_module *module = producer(candidate, candidate_count);
    if (!module && producer(current, current_count)) {
        snprintf(error, size, "The applied OutlinerRows producer cannot be removed or renamed.");
        return false;
    }
    return !module || studio_outliner_module_valid(module, error, size);
}

static const char *denied_capture(const behavior_function_context *, void *, pin_value *)
{
    return "Outliner capture is available only to the mounted Studio UI "
           "producer.";
}

static const char *denied_publish(
    const behavior_function_context *,
    void *,
    const pin_value *,
    const pin_value *,
    bool *accepted
)
{
    *accepted = false;
    return NULL;
}

bool studio_outliner_guard_install(source_registry *sources, char *error, size_t size)
{
    static studio_outliner_service denied = { .capture = denied_capture,
                                              .publish = denied_publish };
    if (!source_set_publication_guard(sources, (source_publication_guard){ .validate = validate }, error, size)) {
        return false;
    }
    if (source_bind_service(sources, STUDIO_OUTLINER_SERVICE, &denied)) {
        snprintf(error, size, "Could not bind the nonpublishing Outliner service.");
        return false;
    }
    return true;
}

static void clear_reveal(studio_outliner_projection *projection)
{
    arena_free(&projection->reveal.arguments);
    projection->reveal = (outliner_reveal){ 0 };
}

static void end_read(studio_outliner_projection *projection)
{
    if (projection->scope.entered) {
        studio_scene_read_end(&projection->scope);
    }
    projection->reading = NULL;
}

static void published(void *user, const source_module_view *modules, int count)
{
    studio_outliner_projection *projection = user;
    const execution_module *module = producer(modules, count);
    if (module == projection->module) {
        return;
    }
    /* The final applied generation, including a no-op/revert's retained module,
     * is the only source identity. Old pointers are never dereferenced later. */
    end_read(projection);
    clear_reveal(projection);
    if (module) {
        execution_module_retain((execution_module *)module);
    }
    if (projection->module) {
        execution_module_free((execution_module *)projection->module);
    }
    projection->module = module;
    projection->generation = next_identity();
}

static void request_free(outliner_request *request)
{
    if (request->read) {
        studio_scene_read_free(request->read);
    }
    arena_free(&request->arguments);
    execution_dispatch_free(request->token);
    free(request);
}

void studio_outliner_projection_boundary(studio_outliner_projection *projection)
{
    if (!projection) {
        return;
    }
    execution_authority_invalidate();
    end_read(projection);
    for (int i = acount(projection->requests) - 1; i >= 0; i--) {
        outliner_request *request = projection->requests[i];
        if (!execution_dispatch_pending(request->token)) {
            request_free(request);
            adel(projection->requests, i);
        }
    }
}

static outliner_binding *binding(studio_outliner_projection *projection, uint64_t id)
{
    for (int i = 0; i < acount(projection->bindings); i++) {
        if (projection->bindings[i].id == id) {
            return &projection->bindings[i];
        }
    }
    return NULL;
}

static bool binding_current(studio_outliner_projection *projection, const outliner_binding *bound)
{
    return bound && !projection->retired && bound->mount &&
           bound->generation == projection->generation &&
           bound->scene == projection->studio->scene &&
           bound->scene_world == projection->studio->scene->world &&
           bound->scene_session ==
               projection->studio->scene->debug_sessions[APP_RUNTIME_SCENE];
}

void studio_outliner_projection_bind(studio_outliner_projection *projection, ui_mount *mount, uint64_t controller)
{
    if (!projection || projection->retired) {
        return;
    }
    execution_program *program = source_program(projection->sources, "OutlinerRows");
    if (!program || !execution_program_instance(program, controller)) {
        ui_mount_bind_origin(mount, 0);
        return;
    }
    for (int i = 0; i < acount(projection->bindings); i++) {
        outliner_binding *bound = &projection->bindings[i];
        if (bound->mount != mount) {
            continue;
        }
        if (binding_current(projection, bound) && bound->controller == controller) {
            ui_mount_bind_origin(mount, bound->id);
            return;
        }
        adel(projection->bindings, i);
        break;
    }
    outliner_binding bound = {
        .id = next_identity(),
        .generation = projection->generation,
        .controller = controller,
        .mount = mount,
        .scene = projection->studio->scene,
        .scene_world = projection->studio->scene->world,
        .scene_session = projection->studio->scene->debug_sessions[APP_RUNTIME_SCENE],
    };
    apush(projection->bindings, bound);
    ui_mount_bind_origin(mount, bound.id);
}

void studio_outliner_projection_unmount(studio_outliner_projection *projection, ui_mount *mount)
{
    if (!projection || !mount) {
        return;
    }
    execution_authority_invalidate();
    end_read(projection);
    ui_activation_origin origin = ui_mount_origin(mount);
    for (int i = 0; i < acount(projection->requests); i++) {
        outliner_request *request = projection->requests[i];
        if (request->internal && request->origin.mount == origin.mount) {
            execution_dispatch_cancel(request->token);
        }
    }
    for (int i = acount(projection->bindings) - 1; i >= 0; i--) {
        if (projection->bindings[i].mount == mount) {
            adel(projection->bindings, i);
        }
    }
    ui_mount_bind_origin(mount, 0);
}

static outliner_request *request_find(studio_outliner_projection *projection, uint64_t id)
{
    for (int i = 0; i < acount(projection->requests); i++) {
        if (execution_dispatch_id(projection->requests[i]->token) == id) {
            return projection->requests[i];
        }
    }
    return NULL;
}

static bool request_current(
    studio_outliner_projection *projection,
    const outliner_request *request,
    const execution_authority *authority
)
{
    outliner_binding *bound = binding(projection, request->origin.binding);
    execution_program *program = source_program(projection->sources, "OutlinerRows");
    return binding_current(projection, bound) &&
           ui_mount_origin_current(bound->mount, &request->origin) &&
           request->generation == projection->generation &&
           request->controller == bound->controller &&
           request->scene == projection->studio->scene &&
           request->scene_world == projection->studio->scene->world &&
           request->scene_session ==
               projection->studio->scene->debug_sessions[APP_RUNTIME_SCENE] &&
           source_world(projection->sources) == projection->world &&
           ecs_entity_is_alive(projection->world, ecs_entity_from_u64(request->controller)) &&
           !execution_dispatch_cancelled(request->token) &&
           authority->request == execution_dispatch_id(request->token) &&
           authority->owner == request->controller &&
           authority->event == request->event && program &&
           authority->program == program && authority->module == projection->module &&
           execution_program_module(program) == projection->module &&
           (authority->event == execution_module_event(projection->module, "Refresh") ||
            authority->event ==
                execution_module_event(projection->module, "RevealObject"));
}

static bool protected_state(const behavior_function_context *context, pin_value values[4])
{
    static const char *const identities[] = {
        "behaviors/OutlinerRows.chibi:1:First",
        "behaviors/OutlinerRows.chibi:1:Count",
        "behaviors/OutlinerRows.chibi:1:Collapsed",
        "behaviors/OutlinerRows.chibi:1:ShownOpen",
    };
    for (int i = 0; i < 4; i++) {
        if (!execution_context_state(context, script_catalog_id("symbol", identities[i]), &values[i]) ||
            values[i].container != PIN_CONTAINER_SINGLE ||
            values[i].type != (i < 2 ? PIN_I32 : PIN_BOOL)) {
            return false;
        }
    }
    return true;
}

static bool same_state(const behavior_function_context *context, const outliner_request *request)
{
    pin_value values[4];
    return protected_state(context, values) && values[0].i == request->state[0].i &&
           values[1].i == request->state[1].i &&
           values[2].b == request->state[2].b &&
           values[3].b == request->state[3].b;
}

static const char *capture(const behavior_function_context *context, void *user, pin_value *outputs)
{
    studio_outliner_projection *projection = user;
    execution_authority authority;
    if (!execution_context_authority(context, &authority)) {
        return "Outliner capture requires the original uninterrupted request.";
    }
    outliner_request *request = request_find(projection, authority.request);
    if (!request || !request_current(projection, request, &authority)) {
        return "Outliner capture request is no longer current.";
    }
    if (request->read) {
        return "An Outliner request may capture only once.";
    }
    if (!protected_state(context, request->state)) {
        return "Outliner protected controller state is unavailable.";
    }
    end_read(projection);
    if (!studio_scene_read_begin(&projection->scope, projection->studio)) {
        return "Outliner scene scope is unavailable.";
    }
    request->authority = authority;
    request->read = studio_scene_read_capture(
        &projection->scope,
        projection->studio,
        projection->error,
        sizeof projection->error
    );
    if (!request->read) {
        end_read(projection);
        return projection->error;
    }
    projection->reading = request;
    const studio_scene_read_facts *facts = studio_scene_read_get(request->read);
    if (facts->actor_count == INT_MAX) {
        return "Outliner actor count leaves no space for its virtual root.";
    }
    const char *reveal = "";
    if (request->event == execution_module_event(projection->module, "RevealObject")) {
        if (request->payload_count != 1 ||
            request->payload[0].container != PIN_CONTAINER_SINGLE ||
            request->payload[0].type != PIN_STR || !request->payload[0].str) {
            end_read(projection);
            return "Outliner reveal workload requires its original text "
                   "payload.";
        }
        reveal = request->payload[0].str;
    }
    studio_outliner_work work;
    const char *work_error = studio_outliner_work_measure(
        facts,
        request->state[0].i,
        request->state[1].i,
        request->state[2].b,
        reveal,
        &work
    );
    uintptr_t work_limit;
    if (work_error ||
        !studio_outliner_work_limit(&work, STUDIO_OUTLINER_CANONICAL_CHECKPOINT_WEIGHT, &work_limit)) {
        end_read(projection);
        return work_error ? work_error : "Outliner workload exceeds the representable request allowance.";
    }
    arena *storage = context->value_storage;
    const int counts[] = { facts->actor_count,
                           facts->prototype_count,
                           facts->component_count,
                           facts->field_count,
                           facts->attachment_count };
    for (int port = 0; port < STUDIO_OUTLINER_SCENE; port++) {
        pin_array *array = pin_array_snapshot_allocate(
            storage,
            outputs[port].type,
            outputs[port].type_name,
            counts[port]
        );
        if (!array) {
            return "Could not allocate captured Outliner facts.";
        }
        for (int i = 0; i < counts[port]; i++) {
            studio_outliner_actor actor;
            studio_outliner_prototype prototype;
            pin_value value = { .type = outputs[port].type,
                                .type_name = outputs[port].type_name };
            switch (port) {
                case STUDIO_OUTLINER_ACTORS:
                    actor = (studio_outliner_actor){
                        .actor = ecs_entity_to_u64(facts->nodes[i].entity),
                        .name = facts->nodes[i].name,
                        .parent = facts->nodes[i].parent,
                        .prototype = facts->actors[i].prototype_name,
                        .prototypeIndex = facts->actors[i].prototype,
                        .fieldFirst = facts->actors[i].field_first,
                        .fieldCount = facts->actors[i].field_count,
                        .viewFlags = (int)facts->actors[i].view_flags,
                        .selected = facts->actors[i].selected,
                    };
                    value.ptr = &actor;
                    break;
                case STUDIO_OUTLINER_PROTOTYPES:
                    prototype = (studio_outliner_prototype){
                        .componentFirst = facts->prototypes[i].component_first,
                        .componentCount = facts->prototypes[i].component_count,
                    };
                    value.ptr = &prototype;
                    break;
                case STUDIO_OUTLINER_COMPONENT_ICONS:
                    value.i = facts->components[i].icon;
                    break;
                case STUDIO_OUTLINER_FIELD_KINDS:
                    value.i = facts->fields[i].kind;
                    break;
                case STUDIO_OUTLINER_ATTACHMENT_ENTITIES:
                    value.u64 = ecs_entity_to_u64(facts->attachments[i].entity);
                    break;
                default: abort();
            }
            if (pin_array_snapshot_set(storage, array, i, &value)) {
                return "Could not copy captured Outliner facts.";
            }
        }
        outputs[port].ptr = array;
    }
    studio_outliner_scene *scene = outputs[STUDIO_OUTLINER_SCENE].ptr;
    if (!scene) {
        return "Outliner capture scene output storage is missing.";
    }
    /* The result buffer belongs to the host bank. Only its managed fields
     * belong to activation storage; the runtime traces them through the bank. */
    pin_value filter = { .type = PIN_STR, .str = facts->filter };
    pin_value scene_path = { .type = PIN_STR, .str = facts->scene_path };
    if (pin_value_snapshot_to_arena(storage, &filter, &filter) ||
        pin_value_snapshot_to_arena(storage, &scene_path, &scene_path)) {
        return "Could not copy captured Outliner scene text.";
    }
    *scene = (studio_outliner_scene){
        .activeIndex = facts->active,
        .rename = ecs_entity_to_u64(facts->rename),
        .renameOutliner = facts->rename_outliner,
        .editable = facts->editable,
        .filter = filter.str,
        .scenePath = scene_path.str,
    };
    /* The account belongs to the exact retained request. Grant an absolute
     * total only after its successful owned Capture; retain all entry debit.
     * A duplicate Capture is already rejected by request->read above. */
    if (!execution_authority_current(context, &request->authority) ||
        !request_current(projection, request, &request->authority) ||
        !same_state(context, request) ||
        !studio_scene_read_current(
            &projection->scope,
            projection->studio,
            request->read
        ) ||
        !execution_context_grant_work(context, request->token, work_limit)) {
        end_read(projection);
        return "Outliner request workload grant is no longer valid.";
    }
    return NULL;
}

static bool record_values(const pin_value *value)
{
    if (!value->ptr) {
        return false;
    }
    for (int i = 0; i < value->record->type.field_count; i++) {
        const type_field *field = &value->record->type.fields[i];
        if (field->type == PIN_BOOL) {
            bool off = false, on = true;
            const void *bytes = (const char *)value->ptr + field->offset;
            if (memcmp(bytes, &off, sizeof off) && memcmp(bytes, &on, sizeof on)) {
                return false;
            }
        }
        pin_value item;
        pin_read_field(value->ptr, field, &item);
        if (field->type == PIN_STR && !item.str) {
            return false;
        }
        if (field->type == PIN_I32 &&
            item.i < (!strcmp(field->name, "revealRow") ? -1 : 0)) {
            return false;
        }
    }
    return true;
}

static bool row_values(const pin_value *rows)
{
    const pin_array *array = rows->ptr;
    if (!array) {
        return true;
    }
    if (!pin_array_valid(array) || !pin_record_same(array->record, rows->record)) {
        return false;
    }
    for (int i = 0; i < array->count; i++) {
        pin_value row = pin_array_get(array, i);
        if (row.type != PIN_STRUCT || row.container != PIN_CONTAINER_SINGLE ||
            !pin_record_same(row.record, rows->record) || !record_values(&row)) {
            return false;
        }
    }
    return true;
}

static const char *publish(
    const behavior_function_context *context,
    void *user,
    const pin_value *header,
    const pin_value *rows,
    bool *accepted
)
{
    studio_outliner_projection *projection = user;
    *accepted = false;
    execution_authority authority;
    if (!execution_context_authority(context, &authority)) {
        return NULL;
    }
    outliner_request *request = request_find(projection, authority.request);
    if (!request || !request->read || request->published ||
        projection->reading != request ||
        !request_current(projection, request, &authority) ||
        !execution_authority_current(context, &request->authority) ||
        !same_state(context, request) ||
        !studio_scene_read_current(
            &projection->scope,
            projection->studio,
            request->read
        )) {
        return NULL;
    }
    if (!studio_outliner_projection_shape(header) || !studio_outliner_rows_shape(rows) ||
        !record_values(header) || !row_values(rows)) {
        return "Outliner publication has an invalid complete projection.";
    }
    av_doc *document = av_doc_new();
    av_value *fields = pin_value_to_document(document, header);
    av_value *items = pin_value_to_document(document, rows);
    if (!fields || !av_is_obj(fields) || !items || !av_is_arr(items)) {
        av_doc_free(document);
        return "Could not prepare the complete Outliner publication.";
    }
    static const char *const names[] = {
        "rootLabel", "rootIcon", "rootParent", "rootDepth", "rootOpen",
        "rootShown", "count",    "total",      "filtered",  "notice",
    };
    static const char *const paths[] = {
        "root.label", "root.icon", "root.parent", "root.depth", "root.open",
        "rootShown",  "count",     "total",       "filtered",   "notice",
    };
    av_value *values[10];
    for (int i = 0; i < 10; i++) {
        values[i] = av_obj_get(fields, names[i]);
        if (!values[i]) {
            av_doc_free(document);
            return "Outliner publication is missing a required field.";
        }
    }
    int reveal = av_get_int(av_obj_get(fields, "revealRow"));
    if (reveal < -1) {
        av_doc_free(document);
        return "Outliner reveal row is invalid.";
    }
    /* Conversion above is callback-free. Recheck immediately before the fixed
     * nonrejecting copy sequence; no model observer runs until this returns. */
    if (!request_current(projection, request, &authority) ||
        !execution_authority_current(context, &request->authority) ||
        !same_state(context, request) ||
        !studio_scene_read_current(
            &projection->scope,
            projection->studio,
            request->read
        )) {
        av_doc_free(document);
        return NULL;
    }
    for (int i = 0; i < 10; i++) {
        if (!ui_set_data(projection->model, request->controller, paths[i], values[i])) {
            abort();
        }
    }
    if (!ui_set_data(projection->model, request->controller, "rows", items)) {
        abort();
    }
    if (reveal >= 0 &&
        !ui_reveal_row(projection->model, request->controller, "#outliner-list", reveal)) {
        abort();
    }
    request->published = true;
    end_read(projection);
    *accepted = true;
    av_doc_free(document);
    return NULL;
}

studio_outliner_projection *studio_outliner_projection_new(
    studio *studio,
    source_registry *sources,
    ui *model,
    char *error,
    size_t size
)
{
    studio_outliner_projection *projection = xcalloc(1, sizeof *projection);
    projection->studio = studio;
    projection->sources = sources;
    projection->world = source_world(sources);
    projection->model = model;
    projection->generation = next_identity();
    projection->service = (studio_outliner_service){ .user = projection,
                                                     .capture = capture,
                                                     .publish = publish };
    if (source_bind_service(sources, STUDIO_OUTLINER_SERVICE, &projection->service)) {
        snprintf(error, size, "Could not bind the guarded Outliner service.");
        free(projection);
        return NULL;
    }
    if (!source_set_publication_guard(
            sources,
            (source_publication_guard){ .user = projection,
                                        .validate = validate,
                                        .published = published },
            error,
            size
        )) {
        if (source_bind_service(sources, STUDIO_OUTLINER_SERVICE, NULL)) {
            abort();
        }
        free(projection);
        return NULL;
    }
    return projection;
}

void studio_outliner_projection_invalidate(studio_outliner_projection *projection)
{
    if (!projection) {
        return;
    }
    execution_authority_invalidate();
    end_read(projection);
    clear_reveal(projection);
    projection->generation = next_identity();
    for (int i = 0; i < acount(projection->requests); i++) {
        if (projection->requests[i]->internal) {
            execution_dispatch_cancel(projection->requests[i]->token);
        }
    }
    aclear(projection->bindings);
}

void studio_outliner_projection_retire(studio_outliner_projection *projection)
{
    studio_outliner_projection_invalidate(projection);
    if (projection) {
        projection->retired = true;
    }
}

void studio_outliner_projection_free(studio_outliner_projection *projection)
{
    if (!projection) {
        return;
    }
    end_read(projection);
    clear_reveal(projection);
    for (int i = 0; i < acount(projection->requests); i++) {
        request_free(projection->requests[i]);
    }
    afree(projection->requests);
    afree(projection->bindings);
    if (projection->module) {
        execution_module_free((execution_module *)projection->module);
    }
    free(projection);
}

typedef struct
{
    studio_outliner_projection *projection;
    const render_ui_host *host;
    ui_activation_origin origin;
    bool internal, host_origin;
    double now, delta;
} outliner_dispatch;

static int dispatch_payload(void *user, const char *event, render_ui_event_field *fields, int capacity)
{
    outliner_dispatch *dispatch = user;
    return dispatch->host->payload
             ? dispatch->host->payload(dispatch->host->context, event, fields, capacity)
             : 0;
}

static bool dispatch_initializing(void *user)
{
    outliner_dispatch *dispatch = user;
    return dispatch->host->initializing &&
           dispatch->host->initializing(dispatch->host->context);
}

static const char *dispatch_failure(void *user)
{
    outliner_dispatch *dispatch = user;
    return dispatch->host->failure ? dispatch->host->failure(dispatch->host->context)
                                   : "";
}

static int dispatch_event(void *user, uint64_t owner, const char *event, execution_arguments arguments)
{
    outliner_dispatch *dispatch = user;
    studio_outliner_projection *projection = dispatch->projection;
    if (!dispatch->origin.binding) {
        if (!dispatch->host_origin || strcmp(event, "RevealObject")) {
            return dispatch->host->dispatch(dispatch->host->context, owner, event, arguments);
        }
        /* A hidden panel has no publication authority. Retain the latest native
         * intent, then acquire a live mount origin when that view next runs. */
        clear_reveal(projection);
        if (!projection->module) {
            return FLOW_OK;
        }
        outliner_reveal *reveal = &projection->reveal;
        reveal->payload = arena_alloc(
            &reveal->arguments,
            (size_t)arguments.count * sizeof *reveal->payload
        );
        for (int i = 0; i < arguments.count; i++) {
            if (pin_value_snapshot_to_arena(
                    &reveal->arguments,
                    &reveal->payload[i],
                    &arguments.values[i]
                )) {
                clear_reveal(projection);
                return FLOW_ERR_ARGUMENT;
            }
        }
        reveal->count = arguments.count;
        reveal->generation = projection->generation;
        reveal->scene = projection->studio->scene;
        reveal->scene_world = projection->studio->scene->world;
        reveal->scene_session = projection->studio->scene->debug_sessions[APP_RUNTIME_SCENE];
        return FLOW_OK;
    }
    outliner_binding *bound = binding(projection, dispatch->origin.binding);
    execution_module_event_info schema;
    if (!binding_current(projection, bound) || (owner && owner != bound->controller) ||
        !ui_mount_origin_current(bound->mount, &dispatch->origin) ||
        !source_event(projection->sources, event, &schema)) {
        /* A stamped event belongs to its original Outliner binding. Never
         * reinterpret an obsolete event against a replacement controller. */
        return FLOW_OK;
    }
    outliner_request *request = xcalloc(1, sizeof *request);
    bool projects_rows = !strcmp(event, "Refresh") || !strcmp(event, "RevealObject");
    request->token = projects_rows
                       ? execution_dispatch_new_work(STUDIO_OUTLINER_WORK_BOOTSTRAP)
                       : execution_dispatch_new();
    request->origin = dispatch->origin;
    request->generation = bound->generation;
    request->controller = bound->controller;
    request->event = schema.id;
    request->scene = bound->scene;
    request->scene_world = bound->scene_world;
    request->scene_session = bound->scene_session;
    request->internal = dispatch->internal && !strcmp(event, "Refresh");
    request->payload_count = arguments.count;
    request->payload = arena_alloc(
        &request->arguments,
        (size_t)arguments.count * sizeof *request->payload
    );
    for (int i = 0; i < arguments.count; i++) {
        if (pin_value_snapshot_to_arena(
                &request->arguments,
                &request->payload[i],
                &arguments.values[i]
            )) {
            request_free(request);
            return FLOW_ERR_ARGUMENT;
        }
    }
    if (request->internal) {
        for (int i = 0; i < acount(projection->requests); i++) {
            outliner_request *prior = projection->requests[i];
            if (prior->internal && prior->controller == request->controller) {
                execution_dispatch_cancel(prior->token);
            }
        }
    }
    apush(projection->requests, request);
    return owner ? source_dispatch_tagged(
                       projection->sources,
                       request->token,
                       ecs_entity_from_u64(owner),
                       event,
                       request->payload,
                       request->payload_count,
                       dispatch->now,
                       dispatch->delta
                   )
                 : source_dispatch_all_tagged(
                       projection->sources,
                       request->token,
                       event,
                       request->payload,
                       request->payload_count,
                       dispatch->now,
                       dispatch->delta
                   );
}

bool studio_outliner_projection_send(
    studio_outliner_projection *projection,
    const render_ui_host *host,
    ecs_entity entity,
    const ui_activation *activation,
    ui_mount *mount,
    bool internal,
    unsigned primary,
    double now,
    double delta
)
{
    if (!projection) {
        return ui_activation_send(host, entity, activation, primary);
    }
    projection->replay_error[0] = '\0';
    if (projection->retired) {
        return activation->origin.binding || !strcmp(activation->event, "RevealObject")
                 ? true
                 : ui_activation_send(host, entity, activation, primary);
    }
    studio_outliner_projection_boundary(projection);
    char replay_error[sizeof projection->replay_error] = "";
    outliner_dispatch dispatch = {
        .projection = projection,
        .host = host,
        .origin = activation->origin,
        .internal = internal,
        .now = now,
        .delta = delta,
        .host_origin = !mount && !entity.slot && !activation->origin.binding,
    };
    if (internal && mount) {
        dispatch.origin = ui_mount_origin(mount);
    } else if (dispatch.host_origin && !strcmp(activation->event, "RevealObject")) {
        /* Studio's native RevealObject broadcast originates now. Choose one
         * currently mounted view of the shared controller before conversion. */
        for (int i = 0; i < acount(projection->bindings); i++) {
            outliner_binding *bound = &projection->bindings[i];
            if (binding_current(projection, bound)) {
                ui_activation_origin origin = ui_mount_origin(bound->mount);
                if (ui_mount_origin_current(bound->mount, &origin)) {
                    dispatch.origin = origin;
                    break;
                }
            }
        }
    }
    outliner_binding *bound = binding(projection, dispatch.origin.binding);
    if (projection->reveal.generation && binding_current(projection, bound) &&
        ui_mount_origin_current(bound->mount, &dispatch.origin)) {
        /* Remove the pending intent before dispatch: callbacks may reenter,
         * and a new native reveal must remain a separate request. */
        outliner_reveal reveal = projection->reveal;
        projection->reveal = (outliner_reveal){ 0 };
        if (reveal.generation == projection->generation &&
            reveal.scene == projection->studio->scene &&
            reveal.scene_world == projection->studio->scene->world &&
            reveal.scene_session ==
                projection->studio->scene->debug_sessions[APP_RUNTIME_SCENE]) {
            outliner_dispatch deferred = dispatch;
            deferred.internal = false;
            deferred.host_origin = true;
            int status = dispatch_event(
                &deferred,
                bound->controller,
                "RevealObject",
                (execution_arguments){ .values = reveal.payload, .count = reveal.count }
            );
            if (status < 0) {
                const char *failure = source_error(projection->sources);
                snprintf(replay_error, sizeof replay_error, "%s", failure[0] ? failure : "Deferred reveal dispatch failed.");
            }
        }
        arena_free(&reveal.arguments);
    }
    render_ui_host tagged = {
        .context = &dispatch,
        .payload = dispatch_payload,
        .dispatch = dispatch_event,
        .initializing = dispatch_initializing,
        .failure = dispatch_failure,
    };
    bool carried = ui_activation_send(&tagged, entity, activation, primary);
    studio_outliner_projection_boundary(projection);
    snprintf(projection->replay_error, sizeof projection->replay_error, "%s", replay_error);
    return carried;
}

const char *studio_outliner_projection_replay_error(const studio_outliner_projection *projection)
{
    return projection ? projection->replay_error : "";
}
