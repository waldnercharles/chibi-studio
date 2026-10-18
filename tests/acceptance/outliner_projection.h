#include "authoring/native_bindings.h"
#include "authoring/source_prepare.h"
#include "authoring/studio_outliner_definition.h"
#include "execution/function_builder.h"
#include "execution/module.h"
#include "execution/status.h"
#include "runtime/arrays.h"
#include "runtime/text_format.h"
#include "types/array.h"
#include "types/catalog.h"
#include "types/record.h"

#include <limits.h>

/* Exact production source, two preparations, 40 direct calls per backend:
 * 20 Project, five ContainsFolded, five LastIndex and ten Title. Each call has
 * a 20,000-step budget per authored frame, with fewer than 1,000 authored
 * function invocations per backend. The two preparations also evaluate
 * 44 named constants; the frame budget is not a whole-fixture instruction cap.
 * Fixed inputs: at most four actors, two prototypes, seven icons, eleven field
 * kinds, seven attachments and 134 text bytes; 23 output rows per backend.
 * No app, project, rendering,
 * child process, file writes, generated cases or fallback backend is involved.
 */
enum
{
    OUTLINER_PROJECTION_CASES = 20,
    OUTLINER_PROJECTION_BUDGET = 20000
};

#define OP_TEXT(V) { .type = PIN_STR, .str = V }
#define OP_INT(V) { .type = PIN_I32, .i = V }
#define OP_BOOL(V) { .type = PIN_BOOL, .b = V }
#define OP_ACTOR0 UINT64_C(0x8000000100000007)
#define OP_ACTOR1 UINT64_C(0x8000000200000007)
#define OP_ACTOR2 UINT64_C(0xfedcba9800000009)
#define OP_ACTOR3 UINT64_C(0x0020000000000001)

static const studio_outliner_actor OUTLINER_PROJECTION_ACTORS[] = {
    { OP_ACTOR0, "Alpha", -1, "", -1, 0, 0, 0, false },
    { OP_ACTOR1, "Bolt", 0, "CameraRig", 0, 0, 7, 1, true },
    { OP_ACTOR2, "Élan", 1, "Box", 1, 7, 3, 2, true },
    { OP_ACTOR3, "Delta", -1, "Unresolved", -1, 10, 1, 3, true },
};
static const studio_outliner_prototype OUTLINER_PROJECTION_PROTOTYPES[] = { { 0, 4 },
                                                                            { 4, 3 } };
static const int OUTLINER_PROJECTION_ICONS[] = { -1, 5, 0, 1, 4, 2, -1 };
static const int OUTLINER_PROJECTION_KINDS[] = { 8,   4, 5, 9, 6, 4,
                                                 123, 6, 5, 5, 9 };
static const uint64_t OUTLINER_PROJECTION_ATTACHMENTS[] = {
    OP_ACTOR1, OP_ACTOR2, OP_ACTOR3, OP_ACTOR1, OP_ACTOR3, OP_ACTOR3, 42
};
static const char *const OUTLINER_PROJECTION_ROW_FIELDS[] = {
    "key",       "label",  "icon",   "prototype", "drop",   "depth",
    "behaviors", "active", "marked", "hidden",    "locked", "renaming"
};
static const pin_value OUTLINER_PROJECTION_ROWS[4][12] = {
    { OP_TEXT("9223372041149743111"),
      OP_TEXT("Alpha"),
      OP_TEXT("\xf3\xb0\x86\xa7"),
      OP_TEXT(""),
      OP_TEXT("object behavior"),
      OP_INT(1),
      OP_INT(0),
      OP_BOOL(false),
      OP_BOOL(false),
      OP_BOOL(false),
      OP_BOOL(false),
      OP_BOOL(false) },
    { OP_TEXT("9223372045444710407"),
      OP_TEXT("Bolt"),
      OP_TEXT("\xf3\xb0\xb5\x9d"),
      OP_TEXT("CameraRig"),
      OP_TEXT("object behavior material shape ui field system"),
      OP_INT(2),
      OP_INT(2),
      OP_BOOL(true),
      OP_BOOL(false),
      OP_BOOL(true),
      OP_BOOL(false),
      OP_BOOL(false) },
    { OP_TEXT("18364758542507835401"),
      OP_TEXT("Élan"),
      OP_TEXT("\xf3\xb0\x8c\xb6"),
      OP_TEXT("Box"),
      OP_TEXT("object behavior shape field"),
      OP_INT(3),
      OP_INT(1),
      OP_BOOL(false),
      OP_BOOL(true),
      OP_BOOL(false),
      OP_BOOL(true),
      OP_BOOL(true) },
    { OP_TEXT("9007199254740993"),
      OP_TEXT("Delta"),
      OP_TEXT("\xf3\xb0\x86\xa7"),
      OP_TEXT("Unresolved"),
      OP_TEXT("object behavior ui"),
      OP_INT(1),
      OP_INT(3),
      OP_BOOL(false),
      OP_BOOL(true),
      OP_BOOL(true),
      OP_BOOL(true),
      OP_BOOL(false) },
};
static const char *const OUTLINER_PROJECTION_HEADER_FIELDS[] = {
    "rootLabel", "rootIcon", "rootParent", "rootDepth", "rootOpen", "rootShown",
    "count",     "total",    "filtered",   "notice",    "revealRow"
};
#define OP_HEADER(OPEN, SHOWN, COUNT, TOTAL, FILTERED, NOTICE, REVEAL)         \
    { OP_TEXT("Stage.v2"), OP_TEXT("\xee\xaf\x92"), OP_BOOL(true),             \
      OP_INT(0),           OP_BOOL(OPEN),           OP_BOOL(SHOWN),            \
      OP_INT(COUNT),       OP_INT(TOTAL),           OP_BOOL(FILTERED),         \
      OP_TEXT(NOTICE),     OP_INT(REVEAL) }
typedef struct
{
    int actors, first, count;
    bool collapsed, editable;
    const char *filter, *reveal;
    int row_count, rows[4];
    pin_value header[11];
} outliner_projection_case;
static const outliner_projection_case OUTLINER_PROJECTION_REQUESTS[] = {
    { 0, 0, 5, false, true, "", "", 0, { 0 }, OP_HEADER(true, true, 0, 1, false, "No objects. Add a prototype to start.", -1) },
    { 4, 0, 5, false, true, "", "", 4, { 0, 1, 2, 3 }, OP_HEADER(true, true, 4, 5, false, "", -1) },
    { 4, 0, 1, false, true, "", "", 0, { 0 }, OP_HEADER(true, true, 4, 5, false, "", -1) },
    { 4, 1, 2, false, true, "", "", 2, { 0, 1 }, OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4, 2, 2, false, true, "", "", 2, { 1, 2 }, OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4, 4, 9, false, true, "", "", 1, { 3 }, OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4, 99, 4, false, true, "", "", 0, { 0 }, OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4, -2, 3, false, true, "", "", 2, { 0, 1 }, OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4,
      INT_MIN,
      INT_MAX,
      false,
      true,
      "",
      "",
      4,
      { 0, 1, 2, 3 },
      OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4, INT_MAX, INT_MAX, false, true, "", "", 0, { 0 }, OP_HEADER(true, false, 4, 5, false, "", -1) },
    { 4, 0, INT_MIN, false, true, "", "", 0, { 0 }, OP_HEADER(true, true, 4, 5, false, "", -1) },
    { 4, 0, 5, true, true, "", "", 0, { 0 }, OP_HEADER(false, true, 4, 1, false, "", -1) },
    { 4, 0, 5, false, true, "ALP", "", 1, { 0 }, OP_HEADER(true, true, 1, 2, false, "", -1) },
    { 4,
      0,
      5,
      false,
      true,
      "camera",
      "9223372045444710407",
      1,
      { 1 },
      OP_HEADER(true, true, 1, 2, false, "", 1) },
    { 4, 0, 5, false, true, "É", "", 1, { 2 }, OP_HEADER(true, true, 1, 2, false, "", -1) },
    { 4, 0, 5, false, true, "é", "", 0, { 0 }, OP_HEADER(true, true, 0, 1, true, "No objects match “é”", -1) },
    { 4, 0, 5, false, true, "absent", "", 0, { 0 }, OP_HEADER(true, true, 0, 1, true, "No objects match “absent”", -1) },
    { 4, 0, 5, false, false, "", "", 4, { 0, 1, 2, 3 }, OP_HEADER(true, true, 4, 5, false, "", -1) },
    { 4, 0, 1, false, true, "", "18364758542507835401", 0, { 0 }, OP_HEADER(true, true, 4, 5, false, "", 3) },
    { 4,
      0,
      5,
      false,
      true,
      "camera",
      "18364758542507835401",
      1,
      { 1 },
      OP_HEADER(true, true, 1, 2, false, "", -1) },
};
#undef OP_HEADER

typedef struct
{
    arena storage;
    behavior_function_context context;
    behavior_function_activation activation;
    pin_value results[2];
} outliner_projection_call;
static struct
{
    const char *path;
    char *text;
    native_capabilities *native;
    authoring_source *source;
    arena inputs;
    pin_value actors, empty, prototypes, icons, kinds, attachments;
    outliner_projection_call calls[OUTLINER_PROJECTION_CASES], helper;
    int invocations, materialized;
    bool catalog_set;
} outliner_projection;

static void outliner_projection_release(outliner_projection_call *call)
{
    behavior_function_release_host(&call->context);
    behavior_function_activation_free(&call->activation);
    call->context = (behavior_function_context){ 0 };
    call->activation = (behavior_function_activation){ 0 };
}
static void outliner_projection_clear(outliner_projection_call *call)
{
    outliner_projection_release(call);
    arena_free(&call->storage);
    *call = (outliner_projection_call){ 0 };
}
static void outliner_projection_cleanup()
{
    for (int i = 0; i < OUTLINER_PROJECTION_CASES; i++) {
        outliner_projection_clear(&outliner_projection.calls[i]);
    }
    outliner_projection_clear(&outliner_projection.helper);
    authoring_source_free(outliner_projection.source);
    native_capabilities_free(outliner_projection.native);
    arena_free(&outliner_projection.inputs);
    free(outliner_projection.text);
    if (outliner_projection.catalog_set) {
        type_catalog_set_project(NULL);
    }
    memset(&outliner_projection, 0, sizeof outliner_projection);
}
static const behavior_function *outliner_projection_function(const char *name)
{
    const script_analysis *analysis = outliner_projection.source->analysis;
    uint64_t id = 0;
    for (int i = 0; i < analysis->function_count; i++) {
        const script_symbol *symbol = &analysis->symbols[analysis->functions[i].symbol];
        if (!strcmp(symbol->name, name)) {
            id = symbol->id;
        }
    }
    execution_module *module = outliner_projection.source->module;
    for (int i = 0; i < execution_module_function_count(module); i++) {
        execution_module_function_info function = execution_module_function_at(module, i);
        if (function.id == id) {
            return function.body;
        }
    }
    return NULL;
}
static bool outliner_projection_invoke(
    const char *name,
    const pin_value *arguments,
    int count,
    int results,
    outliner_projection_call *call,
    behavior_backend backend
)
{
    outliner_projection_clear(call);
    const behavior_function *function = outliner_projection_function(name);
    REQUIRE(function);
    int argument_count, result_count;
    behavior_function_arguments(function, &argument_count);
    const pin_value *defaults = behavior_function_results(function, &result_count);
    REQUIRE(argument_count == count && result_count == results && defaults);
    REQUIRE(results >= 1 && results <= 2);
    memcpy(call->results, defaults, (size_t)results * sizeof *defaults);
    behavior_function_prepare_results(function, call->results, &call->storage);
    call->context = (behavior_function_context){ .activation = &call->activation,
                                                 .arguments = arguments,
                                                 .argument_count = count,
                                                 .results = call->results,
                                                 .budget = OUTLINER_PROJECTION_BUDGET };
    behavior_function_bind_storage(&call->context, &call->storage);
    REQUIRE(behavior_function_bind(function, &call->context, NULL, 0));
    behavior_function_prepare_host(function, &call->context);
    if (behavior_function_resumable(function)) {
        behavior_function_activation_init(function, &call->activation);
    }
    REQUIRE(backend == BEHAVIOR_BYTECODE || behavior_function_entry(function));
    behavior_run_scope runs = { 0 };
    behavior_run_scope_begin(&runs);
    int status = behavior_function_run(function, &call->context, backend != BEHAVIOR_BYTECODE);
    behavior_run_scope_end(&runs);
    if (status != FLOW_OK) {
        fprintf(
            stderr,
            "Outliner projection %s: %d %s\n",
            name,
            status,
            call->context.error_message ? call->context.error_message : ""
        );
    }
    REQUIRE(status == FLOW_OK && call->context.returned);
    REQUIRE(!runs.counts.overflow && !runs.counts.inspection_bytecode_fallbacks);
    REQUIRE(!runs.counts.inspected_sljit_entries && !runs.counts.inspected_wasm_entries);
    REQUIRE(!runs.counts.wasm_entries);
    if (backend == BEHAVIOR_BYTECODE) {
        REQUIRE(runs.counts.bytecode_entries > 0 && !runs.counts.sljit_entries);
    } else {
        REQUIRE(runs.counts.sljit_entries > 0 && !runs.counts.bytecode_entries);
    }
    outliner_projection.invocations++;
    outliner_projection_release(call);
    return true;
}
static bool outliner_projection_record(
    const arena *storage,
    const pin_value *value,
    const char *name,
    uint64_t id,
    const char *const *names,
    const pin_value *expected,
    int count
)
{
    REQUIRE(value->type == PIN_STRUCT && value->container == PIN_CONTAINER_SINGLE);
    REQUIRE(value->record && pin_record_valid(value->record));
    const type_desc *type = &value->record->type;
    REQUIRE(arena_contains(storage, value->record));
    REQUIRE(value->record->id == id && type->field_count == count);
    REQUIRE(value->type_name == type->name && !strcmp(type->name, name));
    REQUIRE(arena_contains(storage, type->name) && arena_contains(storage, type->fields));
    REQUIRE(value->ptr && arena_contains(storage, value->ptr));
    for (int i = 0; i < count; i++) {
        const type_field *field = &type->fields[i];
        REQUIRE(arena_contains(storage, field->name) && !strcmp(field->name, names[i]));
        REQUIRE(field->type == expected[i].type);
        if (field->type_name) {
            REQUIRE(arena_contains(storage, field->type_name));
        }
        pin_value member;
        pin_read_field(value->ptr, field, &member);
        REQUIRE(member.type == expected[i].type && member.container == PIN_CONTAINER_SINGLE);
        if (member.type == PIN_STR) {
            REQUIRE(member.str && arena_contains(storage, member.str));
            REQUIRE(!strcmp(member.str, expected[i].str));
        } else if (member.type == PIN_BOOL) {
            REQUIRE(member.b == expected[i].b);
        } else {
            REQUIRE(member.type == PIN_I32 && member.i == expected[i].i);
        }
    }
    return true;
}
static bool outliner_projection_check(int index)
{
    const outliner_projection_case *expected = &OUTLINER_PROJECTION_REQUESTS[index];
    const outliner_projection_call *call = &outliner_projection.calls[index];
    const arena *storage = &call->storage;
    REQUIRE(outliner_projection_record(
        storage,
        &call->results[0],
        "OutlinerRowPolicies.Projection",
        script_catalog_id("symbol", "libraries/OutlinerRowPolicies.chibi:1:Projection"),
        OUTLINER_PROJECTION_HEADER_FIELDS,
        expected->header,
        11
    ));
    const pin_value *rows = &call->results[1];
    REQUIRE(rows->type == PIN_STRUCT && rows->container == PIN_CONTAINER_ARRAY);
    const pin_array *array = rows->ptr;
    REQUIRE(array && arena_contains(storage, array) && pin_array_valid(array));
    REQUIRE(array->element_type == PIN_STRUCT && array->count == expected->row_count);
    REQUIRE(rows->record && rows->record == array->record && pin_record_valid(rows->record));
    REQUIRE(arena_contains(storage, rows->record));
    REQUIRE(
        rows->type_name == array->type_name &&
        rows->type_name == rows->record->type.name
    );
    REQUIRE(arena_contains(storage, rows->type_name));
    REQUIRE(!strcmp(rows->type_name, "OutlinerRowPolicies.Row"));
    REQUIRE(
        rows->record->id ==
        script_catalog_id("symbol", "libraries/OutlinerRowPolicies.chibi:1:Row")
    );
    REQUIRE(rows->record->type.field_count == 12);
    REQUIRE(arena_contains(storage, rows->record->type.fields));
    for (int i = 0; i < 12; i++) {
        const type_field *field = &rows->record->type.fields[i];
        REQUIRE(arena_contains(storage, field->name));
        REQUIRE(!strcmp(field->name, OUTLINER_PROJECTION_ROW_FIELDS[i]));
        REQUIRE(field->type == OUTLINER_PROJECTION_ROWS[0][i].type);
        if (field->type_name) {
            REQUIRE(arena_contains(storage, field->type_name));
        }
    }
    if (array->count) {
        REQUIRE(
            arena_contains(storage, array->stride ? array->data : (void *)array->items)
        );
    }
    for (int i = 0; i < array->count; i++) {
        pin_value row = pin_array_get(array, i);
        REQUIRE(row.record == rows->record && row.type_name == rows->type_name);
        pin_value fields[12];
        memcpy(fields, OUTLINER_PROJECTION_ROWS[expected->rows[i]], sizeof fields);
        if (!expected->editable) {
            fields[4].str = "";
        }
        REQUIRE(outliner_projection_record(
            storage,
            &row,
            "OutlinerRowPolicies.Row",
            rows->record->id,
            OUTLINER_PROJECTION_ROW_FIELDS,
            fields,
            12
        ));
    }
    return true;
}
static bool outliner_projection_inputs()
{
    arena *storage = &outliner_projection.inputs;
    pin_array *actors = pin_array_snapshot_allocate(storage, PIN_STRUCT, STUDIO_OUTLINER_ACTOR_TYPE, 4);
    pin_array *empty = pin_array_snapshot_allocate(storage, PIN_STRUCT, STUDIO_OUTLINER_ACTOR_TYPE, 0);
    pin_array *prototypes = pin_array_snapshot_allocate(storage, PIN_STRUCT, STUDIO_OUTLINER_PROTOTYPE_TYPE, 2);
    pin_array *icons = pin_array_snapshot_allocate(storage, PIN_I32, NULL, 7);
    pin_array *kinds = pin_array_snapshot_allocate(storage, PIN_I32, NULL, 11);
    pin_array *attachments = pin_array_snapshot_allocate(storage, PIN_HANDLE, "ecs.entity", 7);
    for (int i = 0; i < 4; i++) {
        pin_value value = { .type = PIN_STRUCT,
                            .type_name = STUDIO_OUTLINER_ACTOR_TYPE,
                            .ptr = (void *)&OUTLINER_PROJECTION_ACTORS[i] };
        REQUIRE(!pin_array_snapshot_set(storage, actors, i, &value));
    }
    for (int i = 0; i < 2; i++) {
        pin_value value = { .type = PIN_STRUCT,
                            .type_name = STUDIO_OUTLINER_PROTOTYPE_TYPE,
                            .ptr = (void *)&OUTLINER_PROJECTION_PROTOTYPES[i] };
        REQUIRE(!pin_array_snapshot_set(storage, prototypes, i, &value));
    }
    for (int i = 0; i < 11; i++) {
        pin_value value = OP_INT(OUTLINER_PROJECTION_KINDS[i]);
        REQUIRE(!pin_array_snapshot_set(storage, kinds, i, &value));
    }
    for (int i = 0; i < 7; i++) {
        pin_value icon = OP_INT(OUTLINER_PROJECTION_ICONS[i]);
        pin_value entity = { .type = PIN_HANDLE,
                             .type_name = "ecs.entity",
                             .u64 = OUTLINER_PROJECTION_ATTACHMENTS[i] };
        REQUIRE(!pin_array_snapshot_set(storage, icons, i, &icon));
        REQUIRE(!pin_array_snapshot_set(storage, attachments, i, &entity));
    }
    outliner_projection.actors = (pin_value){ .type = PIN_STRUCT,
                                              .container = PIN_CONTAINER_ARRAY,
                                              .type_name = actors->type_name,
                                              .ptr = actors };
    outliner_projection.empty = (pin_value){ .type = PIN_STRUCT,
                                             .container = PIN_CONTAINER_ARRAY,
                                             .type_name = empty->type_name,
                                             .ptr = empty };
    outliner_projection.prototypes = (pin_value){ .type = PIN_STRUCT,
                                                  .container = PIN_CONTAINER_ARRAY,
                                                  .type_name = prototypes->type_name,
                                                  .ptr = prototypes };
    outliner_projection.icons = (pin_value){ .type = PIN_I32,
                                             .container = PIN_CONTAINER_ARRAY,
                                             .ptr = icons };
    outliner_projection.kinds = (pin_value){ .type = PIN_I32,
                                             .container = PIN_CONTAINER_ARRAY,
                                             .ptr = kinds };
    outliner_projection.attachments = (pin_value){ .type = PIN_HANDLE,
                                                   .container = PIN_CONTAINER_ARRAY,
                                                   .type_name = attachments->type_name,
                                                   .ptr = attachments };
    return true;
}
static bool outliner_projection_unchanged()
{
    const pin_array *actors = outliner_projection.actors.ptr;
    const pin_array *empty = outliner_projection.empty.ptr;
    const pin_array *prototypes = outliner_projection.prototypes.ptr;
    const pin_array *icons = outliner_projection.icons.ptr;
    const pin_array *kinds = outliner_projection.kinds.ptr;
    const pin_array *attachments = outliner_projection.attachments.ptr;
    const pin_array *arrays[] = { actors, empty, prototypes,
                                  icons,  kinds, attachments };
    static const int counts[] = { 4, 0, 2, 7, 11, 7 };
    static const pin_type types[] = { PIN_STRUCT, PIN_STRUCT, PIN_STRUCT,
                                      PIN_I32,    PIN_I32,    PIN_HANDLE };
    static const char *const names[] = { STUDIO_OUTLINER_ACTOR_TYPE,
                                         STUDIO_OUTLINER_ACTOR_TYPE,
                                         STUDIO_OUTLINER_PROTOTYPE_TYPE,
                                         NULL,
                                         NULL,
                                         "ecs.entity" };
    for (int i = 0; i < 6; i++) {
        const pin_array *array = arrays[i];
        REQUIRE(pin_array_valid(array) && arena_contains(&outliner_projection.inputs, array));
        REQUIRE(array->count == counts[i] && array->capacity == counts[i]);
        REQUIRE(array->element_type == types[i] && !array->record && !array->stride);
        REQUIRE(!counts[i] || arena_contains(&outliner_projection.inputs, array->items));
        if (names[i]) {
            REQUIRE(array->type_name && !strcmp(array->type_name, names[i]));
            REQUIRE(arena_contains(&outliner_projection.inputs, array->type_name));
        } else {
            REQUIRE(!array->type_name);
        }
    }
    REQUIRE(actors->count == 4 && empty->count == 0 && prototypes->count == 2);
    REQUIRE(icons->count == 7 && kinds->count == 11 && attachments->count == 7);
    for (int i = 0; i < 4; i++) {
        pin_value value = pin_array_get(actors, i);
        REQUIRE(
            value.type == PIN_STRUCT && value.type_name &&
            !strcmp(value.type_name, STUDIO_OUTLINER_ACTOR_TYPE)
        );
        const studio_outliner_actor *a = value.ptr,
                                    *b = &OUTLINER_PROJECTION_ACTORS[i];
        REQUIRE(a->actor == b->actor && a->parent == b->parent);
        REQUIRE(!strcmp(a->name, b->name) && !strcmp(a->prototype, b->prototype));
        REQUIRE(a->prototypeIndex == b->prototypeIndex && a->fieldFirst == b->fieldFirst);
        REQUIRE(
            a->fieldCount == b->fieldCount && a->viewFlags == b->viewFlags &&
            a->selected == b->selected
        );
    }
    for (int i = 0; i < 2; i++) {
        pin_value value = pin_array_get(prototypes, i);
        REQUIRE(
            value.type == PIN_STRUCT && value.type_name &&
            !strcmp(value.type_name, STUDIO_OUTLINER_PROTOTYPE_TYPE)
        );
        const studio_outliner_prototype *p = value.ptr;
        REQUIRE(p->componentFirst == OUTLINER_PROJECTION_PROTOTYPES[i].componentFirst);
        REQUIRE(p->componentCount == OUTLINER_PROJECTION_PROTOTYPES[i].componentCount);
    }
    for (int i = 0; i < 11; i++) {
        pin_value value = pin_array_get(kinds, i);
        REQUIRE(value.type == PIN_I32 && value.i == OUTLINER_PROJECTION_KINDS[i]);
    }
    for (int i = 0; i < 7; i++) {
        pin_value icon = pin_array_get(icons, i),
                  entity = pin_array_get(attachments, i);
        REQUIRE(icon.type == PIN_I32 && icon.i == OUTLINER_PROJECTION_ICONS[i]);
        REQUIRE(entity.type == PIN_HANDLE && entity.u64 == OUTLINER_PROJECTION_ATTACHMENTS[i]);
        REQUIRE(entity.type_name && !strcmp(entity.type_name, "ecs.entity"));
    }
    return true;
}
static bool outliner_projection_project(int index, behavior_backend backend)
{
    const outliner_projection_case *request = &OUTLINER_PROJECTION_REQUESTS[index];
    studio_outliner_scene scene = { .activeIndex = request->actors ? 1 : -1,
                                    .rename = OP_ACTOR2,
                                    .renameOutliner = true,
                                    .editable = request->editable,
                                    .filter = request->filter,
                                    .scenePath = "folder/Stage.v2.chibi" };
    pin_value borrowed = { .type = PIN_STRUCT,
                           .type_name = STUDIO_OUTLINER_SCENE_TYPE,
                           .ptr = &scene };
    pin_value snapshot;
    REQUIRE(!pin_value_snapshot_to_arena(&outliner_projection.inputs, &snapshot, &borrowed));
    pin_value arguments[] = { request->actors ? outliner_projection.actors
                                              : outliner_projection.empty,
                              outliner_projection.prototypes,
                              outliner_projection.icons,
                              outliner_projection.kinds,
                              outliner_projection.attachments,
                              snapshot,
                              OP_INT(request->first),
                              OP_INT(request->count),
                              OP_BOOL(request->collapsed),
                              OP_TEXT(request->reveal) };
    REQUIRE(outliner_projection_invoke(
        "Project",
        arguments,
        10,
        2,
        &outliner_projection.calls[index],
        backend
    ));
    REQUIRE(outliner_projection_check(index));
    outliner_projection.materialized += ((const pin_array *)outliner_projection
                                             .calls[index]
                                             .results[1]
                                             .ptr)
                                            ->count;
    REQUIRE(outliner_projection_unchanged());
    const studio_outliner_scene *unchanged = snapshot.ptr;
    REQUIRE(
        unchanged->activeIndex == scene.activeIndex &&
        unchanged->rename == scene.rename
    );
    REQUIRE(unchanged->renameOutliner && unchanged->editable == scene.editable);
    REQUIRE(
        !strcmp(unchanged->filter, scene.filter) &&
        !strcmp(unchanged->scenePath, scene.scenePath)
    );
    return true;
}
static bool outliner_projection_helpers(behavior_backend backend)
{
    static const struct
    {
        const char *query, *text;
        bool result;
    } contains[] = { { "", "", true },
                     { "ALP", "xxAlpha", true },
                     { "É", "Élan", true },
                     { "é", "Élan", false },
                     { "longer", "lo", false } };
    static const struct
    {
        const char *text, *needle;
        int result;
    } last[] = { { "aaaa", "aa", 2 },
                 { "abc", "x", -1 },
                 { "", "", 0 },
                 { "abc", "", 3 },
                 { "a", "aa", -1 } };
    static const struct
    {
        const char *path, *result;
    } titles[] = { { "", "Scene" },
                   { "dir/scene.chibi", "scene" },
                   { "dir/.scene", ".scene" },
                   { "dir/a.b.chibi", "a.b" },
                   { "dir/", "" },
                   { "dir\\scene.chibi", "dir\\scene" },
                   { ".x.chibi", ".x" },
                   { "a.", "a" },
                   { "a."
                     "bbbbbbbbbbbbbbbbbbbbbbbbb"
                     "bbbbbbbbbbbbbbbbbbbbbbbbb"
                     "bbbbbbbbbbbbbbbbbbbbbbbbb"
                     "bbbbbbbbbbbbbbbbbbbbbbbbb"
                     "bbbbbbbbbbbbbbbbbbbbbbbbb"
                     ".chibi",
                     "a" },
                   { "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "a\xc3\x89.chibi",
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaa"
                     "a\xc3" } };
    outliner_projection_call *call = &outliner_projection.helper;
    for (int i = 0; i < 5; i++) {
        pin_value arguments[] = { OP_TEXT(contains[i].query),
                                  OP_TEXT(contains[i].text) };
        REQUIRE(outliner_projection_invoke("ContainsFolded", arguments, 2, 1, call, backend));
        REQUIRE(
            call->results[0].type == PIN_BOOL &&
            call->results[0].b == contains[i].result
        );
    }
    for (int i = 0; i < 5; i++) {
        pin_value arguments[] = { OP_TEXT(last[i].text), OP_TEXT(last[i].needle) };
        REQUIRE(outliner_projection_invoke("LastIndex", arguments, 2, 1, call, backend));
        REQUIRE(
            call->results[0].type == PIN_I32 && call->results[0].i == last[i].result
        );
    }
    for (int i = 0; i < 10; i++) {
        REQUIRE(strlen(titles[i].path) <= 134);
        pin_value argument = OP_TEXT(titles[i].path);
        REQUIRE(outliner_projection_invoke("Title", &argument, 1, 1, call, backend));
        REQUIRE(call->results[0].type == PIN_STR && call->results[0].str);
        REQUIRE(!strcmp(call->results[0].str, titles[i].result));
    }
    REQUIRE(arena_contains(&call->storage, call->results[0].str));
    return true;
}
TEST_CASE(outliner_projection_contract)
{
    REQUIRE(
        sizeof OUTLINER_PROJECTION_REQUESTS / sizeof *OUTLINER_PROJECTION_REQUESTS == OUTLINER_PROJECTION_CASES
    );
    outliner_projection.text = file_read_text(outliner_projection.path);
    REQUIRE(outliner_projection.text);
    type_catalog_set_project(&STUDIO_OUTLINER_TYPE_PACK);
    outliner_projection.catalog_set = true;
    char error[256];
    native_type_catalog *
        types = native_type_catalog_new(STUDIO_OUTLINER_TYPE_DEFINITIONS, 3, error, sizeof error);
    REQUIRE(types);
    uint64_t entity_id = script_catalog_id("type", "ecs.entity");
    script_catalog_type entity = { .id = entity_id,
                                   .name = "ecs.entity",
                                   .type = { .storage = PIN_HANDLE, .nominal = entity_id },
                                   .size = sizeof(uint64_t) };
    script_catalog base = { .types = &entity, .type_count = 1, .entity_type = entity_id };
    native_capability_pack extra[] = { native_type_catalog_pack(types),
                                       { .catalog = &base } };
    const native_definition_pack packs[] = { NATIVE_ARRAY_DEFINITION_PACK,
                                             NATIVE_TEXT_DEFINITION_PACK };
    outliner_projection
        .native = native_capabilities_from_definition_packs(extra, 2, packs, 2, error, sizeof error);
    native_type_catalog_free(types);
    if (!outliner_projection.native) {
        fprintf(stderr, "Outliner projection catalog: %s\n", error);
    }
    REQUIRE(outliner_projection.native);
    static const behavior_backend backends[] = { BEHAVIOR_BYTECODE, BEHAVIOR_SLJIT };
    for (int b = 0; b < 2; b++) {
        REQUIRE(behavior_backend_available(backends[b]));
        script_lower_bindings bindings = authoring_native_bindings(
            outliner_projection.native
        );
        outliner_projection.source = authoring_source_prepare_document(
            outliner_projection.path,
            "libraries/OutlinerRowPolicies.chibi",
            outliner_projection.text,
            native_capabilities_catalog(outliner_projection.native),
            &bindings,
            backends[b]
        );
        REQUIRE(outliner_projection.source);
        for (int i = 0; i < outliner_projection.source->diagnostic_count; i++) {
            fprintf(
                stderr,
                "Outliner projection: %s\n",
                outliner_projection.source->diagnostics[i].message
            );
        }
        REQUIRE(outliner_projection.source->valid);
        REQUIRE(outliner_projection.source->analysis->function_count == 12);
        REQUIRE(execution_module_function_count(outliner_projection.source->module) == 12);
        REQUIRE(execution_module_backend(outliner_projection.source->module) == backends[b]);
        REQUIRE(outliner_projection_inputs());
        outliner_projection.invocations = 0;
        outliner_projection.materialized = 0;
        for (int i = 0; i < OUTLINER_PROJECTION_CASES; i++) {
            REQUIRE(outliner_projection_project(i, backends[b]));
        }
        REQUIRE(outliner_projection_helpers(backends[b]));
        REQUIRE(outliner_projection.invocations == 40);
        REQUIRE(outliner_projection.materialized == 23);
        authoring_source_free(outliner_projection.source);
        outliner_projection.source = NULL;
        arena_free(&outliner_projection.inputs);
        /* Re-read original results; no test-side snapshot may repair ownership. */
        for (int i = 0; i < OUTLINER_PROJECTION_CASES; i++) {
            REQUIRE(outliner_projection_check(i));
        }
        REQUIRE(arena_contains(
            &outliner_projection.helper.storage,
            outliner_projection.helper.results[0].str
        ));
        REQUIRE(!strcmp(
            outliner_projection.helper.results[0].str,
            "aaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaa"
            "a\xc3"
        ));
        printf(
            "Outliner projection accepted: backend=%s preparations=1 calls=40 "
            "budget=20000\n",
            behavior_backend_name(backends[b])
        );
    }
    return true;
}
static int outliner_projection_main(const char *path)
{
    outliner_projection.path = path;
    pu_setup(NULL, outliner_projection_cleanup);
    RUN_TEST_CASE(outliner_projection_contract);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}

#undef OP_TEXT
#undef OP_INT
#undef OP_BOOL
#undef OP_ACTOR0
#undef OP_ACTOR1
#undef OP_ACTOR2
#undef OP_ACTOR3
