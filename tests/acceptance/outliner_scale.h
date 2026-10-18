#include "types/copy.h"

#include <SDL3/SDL_timer.h>

/* Include after outliner_projection.h to reuse its public-SDK preparation
 * state, call cleanup, function lookup and owned-record assertions.
 * Diagnostic only: no production budget or supported scene limit changes.
 * Exactly two preparations (44 named constant evaluations) and four direct
 * Project calls: 10,000 then 1,000,000 steps per frame on each backend.
 * One fixed input has 4,096 flat actors named Actor0000 through Actor4095,
 * no prototypes/components/fields/attachments, and a 32-row viewport.
 * Empty-filter short circuit permits at most 4,293 authored starts per call:
 * Project, 4,096 ContainsFolded, Page, 32 * six row helpers, Title/LastIndex.
 * The backend counter also counts each caller re-entry after a helper returns:
 * at most 4,293 + 4,292 = 8,585 backend entries per complete direct call.
 * Canonical parents/matches adds alone copy 16,781,312 packed int slots
 * (67,125,248 bytes) per complete call. Public copy counters now require a
 * linear packed-payload bound: fewer than 4*N slots for both private append
 * chains, plus two full parents snapshots per visible Depth helper. The latter
 * permits canonical argument snapshots even without readonly borrowing. Other
 * input scalar arrays are empty; nominal row payload is counted separately.
 * This rejects either accumulator reverting to canonical prefix copies.
 * These counters cover selected copies, not total allocation or process memory.
 * Each call's storage is freed before the next call. Top-frame steps are neither
 * graph nodes nor total instructions across helpers. Timing covers the direct
 * run, excluding prepare/bind/checks.
 * No host, scene, graph display, rendering, child process or file write runs.
 */
enum
{
    OUTLINER_SCALE_ACTORS = 4096,
    OUTLINER_SCALE_ROWS = 32,
    OUTLINER_SCALE_BUDGET = 10000,
    OUTLINER_SCALE_DIAGNOSTIC_BUDGET = 1000000,
    OUTLINER_SCALE_MAX_ENTRIES = 8585,
};
static pin_value outliner_scale_arguments[10];

static void outliner_scale_cleanup()
{
    outliner_projection_cleanup();
    memset(outliner_scale_arguments, 0, sizeof outliner_scale_arguments);
}
static bool outliner_scale_inputs()
{
    arena *storage = &outliner_projection.inputs;
    static const pin_type types[] = { PIN_STRUCT, PIN_STRUCT, PIN_I32, PIN_I32, PIN_HANDLE };
    static const char *const names[] = { STUDIO_OUTLINER_ACTOR_TYPE,
                                         STUDIO_OUTLINER_PROTOTYPE_TYPE,
                                         NULL,
                                         NULL,
                                         "ecs.entity" };
    for (int i = 0; i < 5; i++) {
        pin_array *array = pin_array_snapshot_allocate(
            storage,
            types[i],
            names[i],
            i == 0 ? OUTLINER_SCALE_ACTORS : 0
        );
        REQUIRE(array);
        outliner_scale_arguments[i] = (pin_value){ .type = types[i],
                                                   .container = PIN_CONTAINER_ARRAY,
                                                   .type_name = array->type_name,
                                                   .ptr = array };
    }
    pin_array *actors = outliner_scale_arguments[0].ptr;
    for (int i = 0; i < OUTLINER_SCALE_ACTORS; i++) {
        char name[16];
        snprintf(name, sizeof name, "Actor%04d", i);
        studio_outliner_actor actor = { .actor = (uint64_t)i + 1,
                                        .name = name,
                                        .parent = -1,
                                        .prototype = "",
                                        .prototypeIndex = -1 };
        pin_value value = { .type = PIN_STRUCT,
                            .type_name = STUDIO_OUTLINER_ACTOR_TYPE,
                            .ptr = &actor };
        REQUIRE(!pin_array_snapshot_set(storage, actors, i, &value));
    }
    studio_outliner_scene scene = { .activeIndex = -1,
                                    .editable = true,
                                    .filter = "",
                                    .scenePath = "Scale.chibi" };
    pin_value value = { .type = PIN_STRUCT,
                        .type_name = STUDIO_OUTLINER_SCENE_TYPE,
                        .ptr = &scene };
    REQUIRE(!pin_value_snapshot_to_arena(storage, &outliner_scale_arguments[5], &value));
    outliner_scale_arguments[6] = (pin_value){ .type = PIN_I32, .i = 1 };
    outliner_scale_arguments[7] = (pin_value){ .type = PIN_I32,
                                               .i = OUTLINER_SCALE_ROWS };
    outliner_scale_arguments[8] = (pin_value){ .type = PIN_BOOL, .b = false };
    outliner_scale_arguments[9] = (pin_value){ .type = PIN_STR, .str = "" };
    return true;
}
static bool outliner_scale_unchanged()
{
    const arena *storage = &outliner_projection.inputs;
    for (int i = 0; i < 5; i++) {
        const pin_array *array = outliner_scale_arguments[i].ptr;
        REQUIRE(array && arena_contains(storage, array) && pin_array_valid(array));
        REQUIRE(array->count == (i == 0 ? OUTLINER_SCALE_ACTORS : 0));
        REQUIRE(array->capacity == array->count && !array->record && !array->stride);
    }
    const pin_array *actors = outliner_scale_arguments[0].ptr;
    for (int i = 0; i < OUTLINER_SCALE_ACTORS; i++) {
        pin_value value = pin_array_get(actors, i);
        REQUIRE(value.type == PIN_STRUCT && !value.record && value.ptr);
        REQUIRE(!strcmp(value.type_name, STUDIO_OUTLINER_ACTOR_TYPE));
        const studio_outliner_actor *actor = value.ptr;
        REQUIRE(arena_contains(storage, actor));
        REQUIRE(
            arena_contains(storage, actor->name) &&
            arena_contains(storage, actor->prototype)
        );
        char name[16];
        snprintf(name, sizeof name, "Actor%04d", i);
        REQUIRE(actor->actor == (uint64_t)i + 1 && !strcmp(actor->name, name));
        REQUIRE(actor->parent == -1 && actor->prototypeIndex == -1 && !actor->prototype[0]);
        REQUIRE(
            !actor->fieldFirst && !actor->fieldCount && !actor->viewFlags &&
            !actor->selected
        );
    }
    const studio_outliner_scene *scene = outliner_scale_arguments[5].ptr;
    REQUIRE(scene && arena_contains(storage, scene));
    REQUIRE(
        scene->activeIndex == -1 && !scene->rename && !scene->renameOutliner &&
        scene->editable
    );
    REQUIRE(arena_contains(storage, scene->filter) && !scene->filter[0]);
    REQUIRE(arena_contains(storage, scene->scenePath) && !strcmp(scene->scenePath, "Scale.chibi"));
    REQUIRE(outliner_scale_arguments[6].i == 1 && outliner_scale_arguments[7].i == OUTLINER_SCALE_ROWS);
    REQUIRE(!outliner_scale_arguments[8].b && !outliner_scale_arguments[9].str[0]);
    return true;
}
static bool outliner_scale_result()
{
    const outliner_projection_call *call = &outliner_projection.helper;
    const pin_value header[] = {
        { .type = PIN_STR, .str = "Scale" },
        { .type = PIN_STR, .str = "\xee\xaf\x92" },
        { .type = PIN_BOOL, .b = true },
        { .type = PIN_I32, .i = 0 },
        { .type = PIN_BOOL, .b = true },
        { .type = PIN_BOOL, .b = false },
        { .type = PIN_I32, .i = OUTLINER_SCALE_ACTORS },
        { .type = PIN_I32, .i = OUTLINER_SCALE_ACTORS + 1 },
        { .type = PIN_BOOL, .b = false },
        { .type = PIN_STR, .str = "" },
        { .type = PIN_I32, .i = -1 },
    };
    REQUIRE(outliner_projection_record(
        &call->storage,
        &call->results[0],
        "OutlinerRowPolicies.Projection",
        script_catalog_id("symbol", "libraries/OutlinerRowPolicies.chibi:1:Projection"),
        OUTLINER_PROJECTION_HEADER_FIELDS,
        header,
        11
    ));
    const pin_value *rows = &call->results[1];
    REQUIRE(rows->type == PIN_STRUCT && rows->container == PIN_CONTAINER_ARRAY);
    const pin_array *array = rows->ptr;
    REQUIRE(array && arena_contains(&call->storage, array) && pin_array_valid(array));
    REQUIRE(array->count == OUTLINER_SCALE_ROWS && array->element_type == PIN_STRUCT);
    REQUIRE(rows->record && rows->record == array->record && pin_record_valid(rows->record));
    REQUIRE(
        rows->type_name == array->type_name &&
        rows->type_name == rows->record->type.name
    );
    REQUIRE(arena_contains(
        &call->storage,
        array->stride ? array->data : (void *)array->items
    ));
    for (int i = 0; i < OUTLINER_SCALE_ROWS; i++) {
        char key[16], name[16];
        snprintf(key, sizeof key, "%d", i + 1);
        snprintf(name, sizeof name, "Actor%04d", i);
        pin_value expected[12];
        memcpy(expected, OUTLINER_PROJECTION_ROWS[0], sizeof expected);
        expected[0].str = key;
        expected[1].str = name;
        pin_value row = pin_array_get(array, i);
        REQUIRE(row.record == rows->record && row.type_name == rows->type_name);
        REQUIRE(outliner_projection_record(
            &call->storage,
            &row,
            "OutlinerRowPolicies.Row",
            script_catalog_id("symbol", "libraries/OutlinerRowPolicies.chibi:1:Row"),
            OUTLINER_PROJECTION_ROW_FIELDS,
            expected,
            12
        ));
    }
    return true;
}
static bool outliner_scale_run(behavior_backend backend, unsigned budget)
{
    outliner_projection_call *call = &outliner_projection.helper;
    outliner_projection_clear(call);
    const behavior_function *function = outliner_projection_function("Project");
    REQUIRE(function);
    int argument_count, result_count;
    behavior_function_arguments(function, &argument_count);
    const pin_value *defaults = behavior_function_results(function, &result_count);
    REQUIRE(argument_count == 10 && result_count == 2 && defaults);
    memcpy(call->results, defaults, sizeof call->results);
    behavior_function_prepare_results(function, call->results, &call->storage);
    call->context = (behavior_function_context){ .activation = &call->activation,
                                                 .arguments = outliner_scale_arguments,
                                                 .argument_count = 10,
                                                 .results = call->results,
                                                 .budget = budget };
    behavior_function_bind_storage(&call->context, &call->storage);
    REQUIRE(behavior_function_bind(function, &call->context, NULL, 0));
    behavior_function_prepare_host(function, &call->context);
    if (behavior_function_resumable(function)) {
        behavior_function_activation_init(function, &call->activation);
    }
    REQUIRE(backend == BEHAVIOR_BYTECODE || behavior_function_entry(function));
    behavior_run_scope runs = { 0 };
    pin_copy_scope copies = { 0 };
    behavior_run_scope_begin(&runs);
    pin_copy_scope_begin(&copies);
    uint64_t started = SDL_GetTicksNS();
    int status = behavior_function_run(function, &call->context, backend != BEHAVIOR_BYTECODE);
    uint64_t elapsed = SDL_GetTicksNS() - started;
    pin_copy_scope_end(&copies);
    behavior_run_scope_end(&runs);
    size_t used, capacity;
    arena_usage(&call->storage, &used, &capacity);
    printf(
        "Outliner scale: backend=%s actors=4096 viewport=32 budget=%u "
        "status=%d returned=%d top_frame_steps=%llu error_steps=%llu "
        "elapsed_ms=%.3f bytecode_entries=%llu sljit_entries=%llu "
        "packed_copy_bytes=%llu text_copy_bytes=%llu nominal_copy_bytes=%llu "
        "record_copy_bytes=%llu value_header_moves=%llu "
        "value_arena_used=%zu value_arena_capacity=%zu error=%s\n",
        behavior_backend_name(backend),
        budget,
        status,
        call->context.returned,
        (unsigned long long)call->context.steps,
        (unsigned long long)call->context.error_steps,
        (double)elapsed / 1000000.0,
        (unsigned long long)runs.counts.bytecode_entries,
        (unsigned long long)runs.counts.sljit_entries,
        (unsigned long long)copies.counts.value_buffer_bytes,
        (unsigned long long)copies.counts.text_bytes,
        (unsigned long long)copies.counts.nominal_bytes,
        (unsigned long long)copies.counts.record_bytes,
        (unsigned long long)copies.counts.value_header_moves,
        used,
        capacity,
        call->context.error_message ? call->context.error_message : ""
    );
    /* All scopes have ended before any assertion can return through cleanup. */
    REQUIRE(!copies.counts.overflow);
    const uint64_t packed_limit = (uint64_t)sizeof(int32_t) * OUTLINER_SCALE_ACTORS *
                                  (4 + 2 * OUTLINER_SCALE_ROWS);
    REQUIRE(copies.counts.value_buffer_bytes <= packed_limit);
    REQUIRE(!runs.counts.overflow && !runs.counts.inspection_bytecode_fallbacks);
    REQUIRE(!runs.counts.inspected_sljit_entries && !runs.counts.inspected_wasm_entries);
    REQUIRE(!runs.counts.wasm_entries);
    if (backend == BEHAVIOR_BYTECODE) {
        REQUIRE(runs.counts.bytecode_entries > 0 && !runs.counts.sljit_entries);
        REQUIRE(runs.counts.bytecode_entries <= OUTLINER_SCALE_MAX_ENTRIES);
    } else {
        REQUIRE(runs.counts.sljit_entries > 0 && !runs.counts.bytecode_entries);
        REQUIRE(runs.counts.sljit_entries <= OUTLINER_SCALE_MAX_ENTRIES);
    }
    if (budget == OUTLINER_SCALE_BUDGET && status == FLOW_ERR_STEP_BUDGET) {
        REQUIRE(!call->context.returned && call->context.failure == BEHAVIOR_FUNCTION_BUDGET);
        REQUIRE(call->context.steps == budget && call->context.error_steps == budget);
    } else {
        REQUIRE(status == FLOW_OK && call->context.returned);
        REQUIRE(call->context.steps <= budget);
        REQUIRE(
            copies.counts.value_buffer_bytes >=
            (uint64_t)2 * OUTLINER_SCALE_ACTORS * sizeof(int32_t)
        );
        if (budget == OUTLINER_SCALE_BUDGET) {
            printf("Outliner scale: the 10,000-step failure assumption did not hold.\n");
        }
        REQUIRE(outliner_scale_result());
    }
    REQUIRE(outliner_scale_unchanged());
    outliner_projection.invocations++;
    outliner_projection_clear(call);
    return true;
}
TEST_CASE(outliner_scale_fixed_diagnostic)
{
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
    REQUIRE(outliner_projection.native);
    REQUIRE(outliner_scale_inputs());
    REQUIRE(outliner_scale_unchanged());
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
                "Outliner scale source: %s\n",
                outliner_projection.source->diagnostics[i].message
            );
        }
        REQUIRE(outliner_projection.source->valid && outliner_projection.source->module);
        REQUIRE(outliner_projection.source->analysis->function_count == 12);
        REQUIRE(execution_module_function_count(outliner_projection.source->module) == 12);
        REQUIRE(execution_module_backend(outliner_projection.source->module) == backends[b]);
        REQUIRE(outliner_scale_run(backends[b], OUTLINER_SCALE_BUDGET));
        REQUIRE(outliner_scale_run(backends[b], OUTLINER_SCALE_DIAGNOSTIC_BUDGET));
        authoring_source_free(outliner_projection.source);
        outliner_projection.source = NULL;
    }
    REQUIRE(outliner_projection.invocations == 4);
    return true;
}
static int outliner_scale_main(const char *path)
{
    outliner_projection.path = path;
    pu_setup(NULL, outliner_scale_cleanup);
    RUN_TEST_CASE(outliner_scale_fixed_diagnostic);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
