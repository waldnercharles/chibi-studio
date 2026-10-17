#include "authoring/component_presentation.h"
#include "authoring/native_bindings.h"
#include "authoring/reference_index.h"
#include "authoring/source_format.h"
#include "authoring/source_prepare.h"
#include "execution/function_builder.h"
#include "execution/module.h"
#include "execution/status.h"
#include "runtime/arrays.h"
#include "runtime/text_format.h"
#include "types/record.h"

/* Renderer-free exact-source check. One source preparation and 41 direct
 * helper invocations per backend: four depths, 19 icons, ten drop strings and
 * eight flag records. No scene, host child, file mutation or UI is involved.
 * Arrays contain at most six integers; calls have at most seven arguments.
 * Bytecode and actual SLJIT must agree.
 */
typedef struct
{
    arena storage;
    behavior_function_context context;
    behavior_function_activation activation;
    pin_value result;
} outliner_policy_call;

static struct
{
    const char *path;
    char *text, *formatted, *again;
    script_syntax *formatted_syntax;
    native_capabilities *native;
    authoring_source *source;
    arena inputs;
    outliner_policy_call calls[3];
    int invocations;
} outliner_policy;

static void outliner_policy_release(outliner_policy_call *call)
{
    behavior_function_release_host(&call->context);
    behavior_function_activation_free(&call->activation);
    call->context = (behavior_function_context){ 0 };
    call->activation = (behavior_function_activation){ 0 };
}

static void outliner_policy_clear(outliner_policy_call *call)
{
    outliner_policy_release(call);
    arena_free(&call->storage);
    *call = (outliner_policy_call){ 0 };
}

static void outliner_policy_cleanup()
{
    for (int i = 0; i < 3; i++) {
        outliner_policy_clear(&outliner_policy.calls[i]);
    }
    authoring_source_free(outliner_policy.source);
    native_capabilities_free(outliner_policy.native);
    script_syntax_free(outliner_policy.formatted_syntax);
    arena_free(&outliner_policy.inputs);
    free(outliner_policy.text);
    free(outliner_policy.formatted);
    free(outliner_policy.again);
    memset(&outliner_policy, 0, sizeof outliner_policy);
}

static const behavior_function *outliner_policy_function(const char *name)
{
    const script_analysis *analysis = outliner_policy.source->analysis;
    uint64_t id = 0;
    for (int i = 0; i < analysis->function_count; i++) {
        const script_symbol *symbol = &analysis->symbols[analysis->functions[i].symbol];
        if (!strcmp(symbol->name, name)) {
            id = symbol->id;
        }
    }
    execution_module *module = outliner_policy.source->module;
    for (int i = 0; i < execution_module_function_count(module); i++) {
        execution_module_function_info function = execution_module_function_at(module, i);
        if (function.id == id) {
            return function.body;
        }
    }
    return NULL;
}

static bool outliner_policy_prepare(behavior_backend backend)
{
    const script_catalog *catalog = native_capabilities_catalog(outliner_policy.native);
    script_lower_bindings bindings = authoring_native_bindings(outliner_policy.native);
    outliner_policy.source = authoring_source_prepare(
        outliner_policy.path,
        outliner_policy.text,
        catalog,
        &bindings,
        backend
    );
    REQUIRE(outliner_policy.source);
    for (int i = 0; i < outliner_policy.source->diagnostic_count; i++) {
        fprintf(
            stderr,
            "Outliner row policy: %s\n",
            outliner_policy.source->diagnostics[i].message
        );
    }
    REQUIRE(outliner_policy.source->valid);
    REQUIRE(outliner_policy.source->analysis->function_count == 4);
    REQUIRE(execution_module_function_count(outliner_policy.source->module) == 4);
    REQUIRE(execution_module_backend(outliner_policy.source->module) == backend);
    outliner_policy.invocations = 0;
    return true;
}

static bool outliner_policy_format()
{
    char error[256];
    outliner_policy.formatted = authoring_source_format(outliner_policy.source->syntax, error, sizeof error);
    if (!outliner_policy.formatted) {
        fprintf(stderr, "Outliner row policy format: %s\n", error);
    }
    REQUIRE(outliner_policy.formatted);
    REQUIRE(!strcmp(outliner_policy.text, outliner_policy.formatted));
    outliner_policy.formatted_syntax = script_parse(outliner_policy.formatted);
    REQUIRE(!outliner_policy.formatted_syntax->diagnostic_count);
    outliner_policy.again = authoring_source_format(outliner_policy.formatted_syntax, error, sizeof error);
    REQUIRE(outliner_policy.again);
    REQUIRE(!strcmp(outliner_policy.formatted, outliner_policy.again));
    return true;
}

static bool outliner_policy_invoke(
    const char *name,
    const pin_value *arguments,
    int count,
    outliner_policy_call *call,
    behavior_backend backend
)
{
    outliner_policy_clear(call);
    const behavior_function *function = outliner_policy_function(name);
    REQUIRE(function);
    int argument_count, result_count;
    behavior_function_arguments(function, &argument_count);
    const pin_value *defaults = behavior_function_results(function, &result_count);
    REQUIRE(argument_count == count && result_count == 1 && defaults);
    call->result = defaults[0];
    behavior_function_prepare_results(function, &call->result, &call->storage);
    call->context = (behavior_function_context){
        .activation = &call->activation,
        .arguments = arguments,
        .argument_count = count,
        .results = &call->result,
        .budget = 2000,
    };
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
            "Outliner row policy %s: %d %s\n",
            name,
            status,
            call->context.error_message ? call->context.error_message : ""
        );
    }
    REQUIRE(status == FLOW_OK && call->context.returned);
    REQUIRE(!runs.counts.overflow && !runs.counts.inspection_bytecode_fallbacks);
    REQUIRE(!runs.counts.inspected_sljit_entries && !runs.counts.inspected_wasm_entries);
    REQUIRE(runs.counts.bytecode_entries == (backend == BEHAVIOR_BYTECODE ? 1 : 0));
    REQUIRE(runs.counts.sljit_entries == (backend == BEHAVIOR_SLJIT ? 1 : 0));
    REQUIRE(!runs.counts.wasm_entries);
    outliner_policy.invocations++;
    outliner_policy_release(call);
    return true;
}

static pin_value outliner_policy_array(const int *values, int count)
{
    arena_free(&outliner_policy.inputs);
    pin_array *array = pin_array_snapshot_allocate(&outliner_policy.inputs, PIN_I32, NULL, count);
    for (int i = 0; i < count; i++) {
        array->items[i] = (pin_value){ .type = PIN_I32, .i = values[i] };
    }
    return (pin_value){ .type = PIN_I32, .container = PIN_CONTAINER_ARRAY, .ptr = array };
}

static bool outliner_policy_unchanged(pin_value value, const int *expected, int count)
{
    const pin_array *array = value.ptr;
    REQUIRE(array && array->count == count && array->element_type == PIN_I32);
    REQUIRE(arena_contains(&outliner_policy.inputs, array));
    REQUIRE(!count || arena_contains(&outliner_policy.inputs, array->items));
    for (int i = 0; i < count; i++) {
        REQUIRE(array->items[i].type == PIN_I32 && array->items[i].i == expected[i]);
    }
    return true;
}

static bool outliner_policy_depths(behavior_backend backend)
{
    static const int parents[] = { -1, 0, 1, -1 };
    static const int expected[] = { 1, 2, 3, 1 };
    for (int i = 0; i < 4; i++) {
        pin_value arguments[] = {
            outliner_policy_array(parents, 4),
            { .type = PIN_I32, .i = i },
        };
        outliner_policy_call *call = &outliner_policy.calls[0];
        REQUIRE(outliner_policy_invoke("Depth", arguments, 2, call, backend));
        REQUIRE(call->result.type == PIN_I32 && call->result.i == expected[i]);
        REQUIRE(outliner_policy_unchanged(arguments[0], parents, 4));
    }
    return true;
}

static bool outliner_policy_text(
    const char *name,
    const int *values,
    int count,
    bool editable,
    const char *expected,
    outliner_policy_call *call,
    behavior_backend backend
)
{
    pin_value arguments[] = {
        outliner_policy_array(values, count),
        { .type = PIN_BOOL, .b = editable },
    };
    REQUIRE(outliner_policy_invoke(name, arguments, !strcmp(name, "Drop") ? 2 : 1, call, backend));
    REQUIRE(call->result.type == PIN_STR && call->result.str);
    /* Scalar literal returns borrow the module; publication into a row record
     * owns its strings. Inspect these direct returns before module retirement. */
    REQUIRE(!strcmp(call->result.str, expected));
    REQUIRE(outliner_policy_unchanged(arguments[0], values, count));
    return true;
}

static bool outliner_policy_icons(behavior_backend backend)
{
    static const int icons[] = {
        COMPONENT_ICON_CAMERA,     COMPONENT_ICON_SYSTEM,
        COMPONENT_ICON_LIGHT,      COMPONENT_ICON_EMITTER,
        COMPONENT_ICON_WATER,      COMPONENT_ICON_TERRAIN,
        COMPONENT_ICON_GAMEPAD,    COMPONENT_ICON_KEYBOARD,
        COMPONENT_ICON_PROJECTILE, COMPONENT_ICON_HEALTH,
        COMPONENT_ICON_DIAMOND,    COMPONENT_ICON_SHAPE,
    };
    /* Independent UTF-8 bytes for Studio's existing kit_icons.h glyphs. */
    static const char *const glyphs[] = {
        "\xf3\xb0\xb5\x9d", "\xee\xae\xa2",     "\xf3\xb0\x8c\xb6",
        "\xf3\xb0\x99\xb4", "\xf3\xb0\xb8\x8a", "\xf3\xb0\x96\x8a",
        "\xf3\xb0\x8a\x97", "\xf3\xb0\xa5\xbb", "\xf3\xb0\xb3\x9a",
        "\xf3\xb0\x8b\x95", "\xf3\xb0\xae\x8b", "\xf3\xb0\xa0\xb2",
    };
    static const int none[] = { COMPONENT_ICON_NONE };
    static const int mixed[] = { COMPONENT_ICON_SHAPE,
                                 COMPONENT_ICON_LIGHT,
                                 COMPONENT_ICON_NONE,
                                 COMPONENT_ICON_CAMERA,
                                 COMPONENT_ICON_CAMERA };
    static const int repeated[] = { COMPONENT_ICON_HEALTH,
                                    COMPONENT_ICON_HEALTH,
                                    COMPONENT_ICON_NONE };
    static const int unknown_negative[] = { -2, COMPONENT_ICON_CAMERA };
    static const int unknown_high[] = { COMPONENT_ICON_COUNT, COMPONENT_ICON_COUNT + 1 };
    static const int mixed_unknown_high[] = {
        COMPONENT_ICON_COUNT + 1, COMPONENT_ICON_SHAPE, COMPONENT_ICON_NONE
    };
    outliner_policy_call *call = &outliner_policy.calls[0];
    REQUIRE(COMPONENT_ICON_COUNT == 12);
    for (int i = 0; i < 12; i++) {
        REQUIRE(outliner_policy_text("Icon", &icons[i], 1, false, glyphs[i], call, backend));
    }
    REQUIRE(outliner_policy_text("Icon", NULL, 0, false, "\xf3\xb0\x86\xa7", call, backend));
    REQUIRE(outliner_policy_text("Icon", none, 1, false, "\xf3\xb0\x86\xa7", call, backend));
    REQUIRE(outliner_policy_text("Icon", mixed, 5, false, glyphs[0], call, backend));
    REQUIRE(outliner_policy_text("Icon", repeated, 3, false, glyphs[9], call, backend));
    REQUIRE(
        outliner_policy_text("Icon", unknown_negative, 2, false, "\xf3\xb0\x86\xa7", call, backend)
    );
    REQUIRE(
        outliner_policy_text("Icon", unknown_high, 2, false, "\xf3\xb0\x86\xa7", call, backend)
    );
    REQUIRE(
        outliner_policy_text("Icon", mixed_unknown_high, 3, false, glyphs[11], call, backend)
    );
    return true;
}

static bool outliner_policy_drops(behavior_backend backend)
{
    static const int kinds[] = {
        REFERENCE_ASSET_MATERIAL, REFERENCE_ASSET_SHAPE,  REFERENCE_ASSET_UI,
        REFERENCE_ASSET_FIELD,    REFERENCE_ASSET_SYSTEM,
    };
    static const char *const expected[] = {
        "object behavior material", "object behavior shape",
        "object behavior ui",       "object behavior field",
        "object behavior system",
    };
    static const int all[] = {
        REFERENCE_ASSET_SYSTEM, REFERENCE_ASSET_UI,    REFERENCE_ASSET_MATERIAL,
        REFERENCE_ASSET_FIELD,  REFERENCE_ASSET_SHAPE, REFERENCE_ASSET_MATERIAL,
    };
    static const int ignored[] = {
        REFERENCE_ASSET_SCENE,    REFERENCE_ASSET_PROTOTYPE,
        REFERENCE_ASSET_BEHAVIOR, REFERENCE_ASSET_EVENT,
        REFERENCE_ASSET_LIBRARY,
    };
    static const int mixed[] = { REFERENCE_ASSET_FIELD, REFERENCE_ASSET_SHAPE };
    outliner_policy_call *call = &outliner_policy.calls[1];
    REQUIRE(outliner_policy_text("Drop", NULL, 0, true, "object behavior", call, backend));
    for (int i = 0; i < 5; i++) {
        REQUIRE(outliner_policy_text("Drop", &kinds[i], 1, true, expected[i], call, backend));
    }
    REQUIRE(
        outliner_policy_text("Drop", all, 6, true, "object behavior material shape ui field system", call, backend)
    );
    REQUIRE(outliner_policy_text("Drop", ignored, 5, true, "object behavior", call, backend));
    REQUIRE(outliner_policy_text("Drop", all, 6, false, "", call, backend));
    REQUIRE(
        outliner_policy_text("Drop", mixed, 2, true, "object behavior shape field", call, backend)
    );
    return true;
}

static bool outliner_policy_flags(outliner_policy_call *call, const bool expected[5])
{
    static const char *const names[] = { "active", "marked", "hidden", "locked", "renaming" };
    const pin_value *value = &call->result;
    REQUIRE(value->type == PIN_STRUCT && value->container == PIN_CONTAINER_SINGLE);
    REQUIRE(value->record && pin_record_valid(value->record));
    REQUIRE(arena_contains(&call->storage, value->record));
    REQUIRE(value->type_name == value->record->type.name);
    REQUIRE(arena_contains(&call->storage, value->type_name));
    REQUIRE(value->ptr && arena_contains(&call->storage, value->ptr));
    REQUIRE(value->record->type.field_count == 5);
    for (int i = 0; i < 5; i++) {
        const type_field *field = &value->record->type.fields[i];
        REQUIRE(!strcmp(field->name, names[i]));
        pin_value member;
        pin_read_field(value->ptr, field, &member);
        REQUIRE(member.type == PIN_BOOL && member.b == expected[i]);
    }
    return true;
}

static bool outliner_policy_states(behavior_backend backend)
{
    static const struct
    {
        int index, active;
        bool selected;
        int view;
        uint64_t actor, rename;
        bool outliner;
        bool expected[5];
    } cases[] = {
        { 0, 2, false, 0, 7, 0, false, { false, false, false, false, false } },
        { 2,
          2,
          true,
          1,
          UINT64_C(0x8000000100000007),
          UINT64_C(0x8000000200000007),
          true,
          { true, false, true, false, false } },
        { 1,
          2,
          true,
          2,
          UINT64_C(0x8000000200000007),
          UINT64_C(0x8000000200000007),
          true,
          { false, true, false, true, true } },
        { 1,
          2,
          true,
          3,
          UINT64_C(0x8000000200000007),
          UINT64_C(0x8000000200000007),
          false,
          { false, true, true, true, false } },
        { 3,
          -1,
          false,
          128,
          UINT64_C(0xfedcba9800000009),
          UINT64_C(0xfedcba9800000009),
          true,
          { false, false, false, false, true } },
        { 3,
          3,
          false,
          131,
          UINT64_C(0xfedcba9800000009),
          UINT64_C(0x0020000000000001),
          true,
          { true, false, true, true, false } },
        { 1,
          2,
          false,
          2,
          UINT64_C(0x0020000000000001),
          UINT64_C(0x0020000000000000),
          true,
          { false, false, false, true, false } },
        { 0,
          0,
          true,
          0,
          UINT64_C(0x0020000000000001),
          UINT64_C(0x0020000000000001),
          true,
          { true, false, false, false, true } },
    };
    REQUIRE(APP_SCENE_VIEW_HIDDEN == 1 && APP_SCENE_VIEW_LOCKED == 2);
    for (int i = 0; i < 8; i++) {
        pin_value arguments[] = {
            { .type = PIN_I32, .i = cases[i].index },
            { .type = PIN_I32, .i = cases[i].active },
            { .type = PIN_BOOL, .b = cases[i].selected },
            { .type = PIN_I32, .i = cases[i].view },
            { .type = PIN_HANDLE, .type_name = "ecs.entity", .u64 = cases[i].actor },
            { .type = PIN_HANDLE, .type_name = "ecs.entity", .u64 = cases[i].rename },
            { .type = PIN_BOOL, .b = cases[i].outliner },
        };
        outliner_policy_call *call = &outliner_policy.calls[2];
        REQUIRE(outliner_policy_invoke("SelectState", arguments, 7, call, backend));
        REQUIRE(outliner_policy_flags(call, cases[i].expected));
    }
    return true;
}

TEST_CASE(outliner_row_policy_contract)
{
    outliner_policy.text = file_read_text(outliner_policy.path);
    REQUIRE(outliner_policy.text);
    native_definition_pack packs[] = { NATIVE_ARRAY_DEFINITION_PACK,
                                       NATIVE_TEXT_DEFINITION_PACK };
    char error[256];
    outliner_policy
        .native = native_capabilities_from_definition_packs(NULL, 0, packs, 2, error, sizeof error);
    if (!outliner_policy.native) {
        fprintf(stderr, "Outliner row policy catalog: %s\n", error);
    }
    REQUIRE(outliner_policy.native);
    static const behavior_backend backends[] = { BEHAVIOR_BYTECODE, BEHAVIOR_SLJIT };
    for (int i = 0; i < 2; i++) {
        REQUIRE(behavior_backend_available(backends[i]));
        REQUIRE(outliner_policy_prepare(backends[i]));
        if (i == 0) {
            REQUIRE(outliner_policy_format());
        }
        REQUIRE(outliner_policy_depths(backends[i]));
        REQUIRE(outliner_policy_icons(backends[i]));
        REQUIRE(outliner_policy_drops(backends[i]));
        REQUIRE(outliner_policy_states(backends[i]));
        REQUIRE(outliner_policy.invocations == 41);
        authoring_source_free(outliner_policy.source);
        outliner_policy.source = NULL;
        arena_free(&outliner_policy.inputs);
        /* Inspect the original output closures after producer retirement,
         * without a test-side snapshot that could conceal a borrowed result. */
        static const bool retained_flags[] = { true, false, false, false, true };
        REQUIRE(outliner_policy_flags(&outliner_policy.calls[2], retained_flags));
        REQUIRE(arena_contains(
            &outliner_policy.calls[1].storage,
            outliner_policy.calls[1].result.str
        ));
        REQUIRE(!strcmp(outliner_policy.calls[1].result.str, "object behavior shape field"));
        printf(
            "Outliner row policies accepted: backend=%s calls=41\n",
            behavior_backend_name(backends[i])
        );
    }
    return true;
}

static int outliner_row_policy_main(const char *path)
{
    outliner_policy.path = path;
    pu_setup(NULL, outliner_policy_cleanup);
    RUN_TEST_CASE(outliner_row_policy_contract);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
