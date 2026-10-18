#include "authoring/native_bindings.h"
#include "authoring/preview_source.h"
#include "authoring/source_collection.h"
#include "authoring/studio_outliner_definition.h"
#include "authoring/ui_events_adapter.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_outliner_projection.h"
#include "execution/inspection.h"
#include "execution/status.h"
#include "host/native_catalog.h"
#include "runtime/arrays.h"
#include "runtime/save.h"
#include "runtime/text_format.h"
#include "types/catalog.h"

/* Twelve sequential scenarios, five bytecode collection preparations (one
 * empty; four with two sources, one of which reuses both source generations).
 * Six newly prepared modules; two controllers and two one-actor edited worlds.
 * Two real mounts, four syncs, one RmlUi context, one document show and one
 * layout refresh; zero explicit context updates, render passes or app frames.
 * Backend startup installs its 16 embedded fonts. Two attachments execute two
 * empty Begin roots. Twenty-seven adapter sends include seven stale stamped
 * events and one hidden native reveal; one later send drains that deferred
 * reveal. Two source pumps and one explicit debugger resume are fixed.
 * Three library generations evaluate 66 named constants under the compiler's
 * existing 100,000-step per-frame budget, separately from runtime execution.
 * All source and markup overlays are finite in-memory literals; no file writes,
 * project loading, timers, sleeps, generated cases or fallback backend.
 * Ordinary controls use the production 10,000-step per-frame budget;
 * Refresh/Reveal use the shared request allowance granted from captured facts.
 * Exact send/pump totals are asserted separately from source preparation.
 * Debugger pause is the public suspension mechanism; no authority is forged.
 */
static const char OUTLINER_LIVE_ROOT[] = "/__chibi_outliner_live_fixture__";
static const char OUTLINER_LIVE_MARKUP[] = "<input id='old-input' type='text' "
                                           "data-event-blur=\"emit('"
                                           "RevealObject', 'old-payload')\"/>"
                                           "<div id='outliner-list'></div>";
static const char *outliner_live_content;
static struct
{
    studio *s;
    app_scene_authoring *scenes[2];
    ecs_world *world, *edited[2];
    source_registry *sources;
    preview_runtime runtimes[2];
    preview_world lives[2];
    preview_runtime ui_runtime;
    preview_world ui_live;
    ui *model;
    studio_outliner_projection *projection;
    native_capabilities *native;
    authoring_sources *prepared[3];
    execution_module *modules[3];
    execution_inspector *inspector;
    ecs_entity controllers[2];
    rml_backend *backend;
    rml_element *document, *roots[2];
    rml_listener *listener;
    ui_data data;
    ui_mount *mounts[2];
    int markup[2], preparations, sends, pumps, scenarios;
    behavior_backend execution_backend;
    bool backend_ok, catalog_set;
    uint64_t invocations;
    char actor_key[32], error[256];
    char *library, *behavior;
} outliner_live;

static void outliner_live_scene_free(int index)
{
    app_scene_authoring *scene = outliner_live.scenes[index];
    if (!scene) {
        return;
    }
    source_registry_free(outliner_live.runtimes[index].sources);
    if (outliner_live.runtimes[index].entities) {
        entities_free(outliner_live.runtimes[index].entities);
    }
    ecs_world_free(outliner_live.edited[index]);
    outliner_live.edited[index] = NULL;
    for (int i = 0; i < acount(scene->actors); i++) {
        free(scene->actors[i].name);
        free(scene->actors[i].prototype_name);
    }
    afree(scene->actors);
    for (int i = 0; i < acount(scene->view); i++) {
        free(scene->view[i].name);
    }
    afree(scene->view);
    free(scene->view_path);
    free(scene->scene_path);
    if (scene->view_store) {
        save_free(scene->view_store);
    }
    free(scene);
    outliner_live.scenes[index] = NULL;
}

static void outliner_live_source_views_free()
{
    for (int i = acount(outliner_live.ui_live.definitions) - 1; i >= 0; i--) {
        int status = preview_remove_projection(
            &outliner_live.ui_live,
            outliner_live.ui_live.definitions[i]->name
        );
        assert(status == 0);
        (void)status;
    }
    afree(outliner_live.ui_live.definitions);
    preview_sources_free(&outliner_live.ui_live);
}

static void outliner_live_cleanup()
{
    studio_outliner_projection_retire(outliner_live.projection);
    if (outliner_live.sources) {
        source_registry_free(outliner_live.sources);
    }
    outliner_live.sources = NULL;
    studio_outliner_projection_free(outliner_live.projection);
    outliner_live.projection = NULL;
    outliner_live_source_views_free();
    execution_inspector_free(outliner_live.inspector);
    for (int i = 0; i < 2; i++) {
        ui_mount_free(outliner_live.mounts[i]);
        outliner_live.mounts[i] = NULL;
    }
    if (outliner_live.document) {
        rml_document_close(outliner_live.document);
    }
    if (outliner_live.data.model) {
        ui_data_free(&outliner_live.data);
    }
    /* Closed documents detach their listeners when the context is released. */
    rml_backend_destroy(outliner_live.backend);
    rml_listener_destroy(outliner_live.listener);
    ui_free(outliner_live.model);
    for (int i = 0; i < 2; i++) {
        outliner_live_scene_free(i);
    }
    for (int i = 0; i < 3; i++) {
        authoring_sources_free(outliner_live.prepared[i]);
    }
    native_capabilities_free(outliner_live.native);
    if (outliner_live.catalog_set) {
        type_catalog_set_project(NULL);
    }
    if (outliner_live.ui_runtime.entities) {
        entities_free(outliner_live.ui_runtime.entities);
    }
    if (outliner_live.world) {
        ecs_world_free(outliner_live.world);
    }
    if (outliner_live.s) {
        component_presentation_free(&outliner_live.s->presentation.hints);
    }
    free(outliner_live.s);
    free(outliner_live.library);
    free(outliner_live.behavior);
    memset(&outliner_live, 0, sizeof outliner_live);
}

static bool outliner_live_prepare(int revision)
{
    static const char *const paths[] = {
        "libraries/OutlinerRowPolicies.chibi",
        "behaviors/OutlinerRows.chibi",
        "events/ShowRows.kdl",
        "events/Refresh.kdl",
        "events/RootClicked.kdl",
        "events/RevealObject.kdl",
        "ui/Outliner.rml",
    };
    const char *texts[] = {
        outliner_live.library,
        outliner_live.behavior,
        "name \"ShowRows\"\n(array)variables { - name=\"First\" type=\"int\"; "
        "- name=\"Count\" type=\"int\"; }\n",
        "name \"Refresh\"\n",
        "name \"RootClicked\"\n(array)variables { - name=\"Open\" "
        "type=\"bool\"; }\n",
        "name \"RevealObject\"\n(array)variables { - name=\"Object\" "
        "type=\"const char *\"; }\n",
        "<div data-event-click=\"emit('FoldRoot'); emit('UnfoldRoot'); "
        "emit('ToggleRoot')\"></div>",
    };
    char full[7][256];
    source_catalog_edit edits[7] = { 0 };
    for (int i = 0; i < 7; i++) {
        snprintf(full[i], sizeof full[i], "%s/%s", OUTLINER_LIVE_ROOT, paths[i]);
        edits[i] = (source_catalog_edit){ .path = full[i], .text = texts[i] };
    }
    int status = preview_sources_replace(
        &outliner_live.ui_live,
        edits,
        7,
        outliner_live.error,
        sizeof outliner_live.error
    );
    authoring_sources_free(outliner_live.prepared[revision]);
    outliner_live.prepared[revision] = authoring_sources_retain(
        preview_sources_working(&outliner_live.ui_live)
    );
    outliner_live.preparations++;
    const authoring_sources *sources = outliner_live.prepared[revision];
    for (int i = 0; i < authoring_sources_catalog_diagnostic_count(sources); i++) {
        fprintf(
            stderr,
            "Outliner live catalog: %s\n",
            authoring_sources_catalog_diagnostic_at(sources, i)->message
        );
    }
    for (int i = 0; i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        for (int j = 0; j < source->diagnostic_count; j++) {
            const script_diagnostic *diagnostic = &source->diagnostics[j];
            fprintf(
                stderr,
                "Outliner live source %s:%d:%d [%d,%d): %s\n",
                source->path,
                diagnostic->range.line,
                diagnostic->range.column,
                diagnostic->range.start,
                diagnostic->range.end,
                diagnostic->message
            );
        }
    }
    for (int i = 0; i < authoring_sources_count(sources); i++) {
        const authoring_source *source = authoring_sources_at(sources, i);
        if (source->analysis && source->analysis->name &&
            !strcmp(source->analysis->name, "OutlinerRows")) {
            outliner_live.modules[revision] = source->module;
        }
        REQUIRE(
            source->module && execution_module_backend(source->module) ==
                                  outliner_live.execution_backend
        );
    }
    REQUIRE(authoring_sources_valid(sources));
    REQUIRE(status == 0);
    REQUIRE(authoring_sources_count(sources) == 2 && outliner_live.modules[revision]);
    return true;
}

typedef struct
{
    int mount, seen;
} outliner_live_loader;
static void *outliner_live_open(const char *root, const char *name, bool page)
{
    (void)root;
    assert(!page && (!strcmp(name, "Left") || !strcmp(name, "Right")));
    outliner_live_loader *loader = xcalloc(1, sizeof *loader);
    loader->mount = !strcmp(name, "Right");
    loader->seen = -1;
    return loader;
}
static int outliner_live_read(void *user, double time, ui_document *out, char *error, size_t size)
{
    (void)time;
    (void)error;
    (void)size;
    outliner_live_loader *loader = user;
    if (loader->seen == outliner_live.markup[loader->mount]) {
        return 0;
    }
    loader->seen = outliner_live.markup[loader->mount];
    out->rml = xstrdup(loader->seen ? "<div id='new-child'></div>" : OUTLINER_LIVE_MARKUP);
    out->rcss = xstrdup("");
    return 1;
}
static void outliner_live_emit(
    void *user,
    const rml_event *event,
    const char *name,
    int argc,
    const char *const *args
)
{
    (void)user;
    for (int i = 0; i < 2; i++) {
        if (outliner_live.mounts[i] &&
            ui_mount_holds(outliner_live.mounts[i], event->current)) {
            ui_mount_emit(outliner_live.mounts[i], event, name, argc, args);
            return;
        }
    }
}
static void outliner_live_listen(void *user, const rml_event *event)
{
    (void)user;
    for (int i = 0; i < 2; i++) {
        if (outliner_live.mounts[i] &&
            ui_mount_holds(outliner_live.mounts[i], event->current)) {
            ui_mount_listen(outliner_live.mounts[i], event);
        }
    }
}
static void outliner_live_count_runs(const behavior_run_scope *runs)
{
    bool bytecode = outliner_live.execution_backend == BEHAVIOR_BYTECODE;
    outliner_live.invocations += bytecode ? runs->counts.bytecode_entries
                                          : runs->counts.sljit_entries;
    outliner_live.backend_ok &= !runs->counts.overflow &&
                                !runs->counts.inspection_bytecode_fallbacks &&
                                !(bytecode ? runs->counts.sljit_entries
                                           : runs->counts.bytecode_entries) &&
                                !runs->counts.wasm_entries &&
                                !runs->counts.inspected_sljit_entries &&
                                !runs->counts.inspected_wasm_entries;
}
static bool outliner_live_send(int mount, int controller, const ui_activation *activation, bool internal)
{
    const render_ui_host host = authoring_ui_events(&outliner_live.ui_live);
    outliner_live.sends++;
    behavior_run_scope runs = { 0 };
    behavior_run_scope_begin(&runs);
    bool carried = studio_outliner_projection_send(
        outliner_live.projection,
        &host,
        outliner_live.controllers[controller],
        activation,
        outliner_live.mounts[mount],
        internal,
        0,
        0,
        0
    );
    behavior_run_scope_end(&runs);
    outliner_live_count_runs(&runs);
    return carried;
}
static void outliner_live_enqueue(int mount, const char *name, int argc, const char *const *args)
{
    rml_event event = { .current = outliner_live.roots[mount],
                        .target = outliner_live.roots[mount],
                        .button = 2,
                        .mouse_x = 13,
                        .mouse_y = 17,
                        .modifiers = RML_KM_SHIFT };
    ui_mount_emit(outliner_live.mounts[mount], &event, name, argc, args);
}
static bool outliner_live_deliver(int mount, int controller, const char *event, bool accepted)
{
    ui_activation activation;
    REQUIRE(ui_mount_take(outliner_live.mounts[mount], &activation));
    REQUIRE(!strcmp(activation.event, event));
    REQUIRE(outliner_live_send(mount, controller, &activation, false) == accepted);
    if (!accepted) {
        execution_program_clear_error(source_program(outliner_live.sources, "OutlinerRows"));
    }
    return true;
}
static bool outliner_live_event(int mount, int controller, const char *name, int argc, const char *const *args)
{
    outliner_live_enqueue(mount, name, argc, args);
    return outliner_live_deliver(mount, controller, name, true);
}
static bool outliner_live_pump(int expected)
{
    outliner_live.pumps++;
    behavior_run_scope runs = { 0 };
    behavior_run_scope_begin(&runs);
    int status = source_pump(outliner_live.sources, 0, 0);
    behavior_run_scope_end(&runs);
    outliner_live_count_runs(&runs);
    studio_outliner_projection_boundary(outliner_live.projection);
    REQUIRE(status == expected);
    execution_program_clear_error(source_program(outliner_live.sources, "OutlinerRows"));
    return true;
}
static const av_value *outliner_live_value(int controller, const char *name)
{
    uint64_t owner = ecs_entity_to_u64(outliner_live.controllers[controller]);
    for (int i = 0; i < ui_data_count(outliner_live.model); i++) {
        const ui_datum *datum = ui_data_at(outliner_live.model, i);
        if (datum->entity == owner && !strcmp(datum->name, name)) {
            return datum->value;
        }
    }
    return NULL;
}
static bool outliner_live_expect_publication()
{
    av_doc *document = av_doc_new();
    bool set = ui_set_data(
        outliner_live.model,
        ecs_entity_to_u64(outliner_live.controllers[0]),
        "notice",
        av_str(document, "publication pending")
    );
    av_doc_free(document);
    REQUIRE(set);
    return true;
}
static bool outliner_live_collapsed(int controller, bool expected)
{
    execution_program *program = source_program(outliner_live.sources, "OutlinerRows");
    execution_instance *instance = execution_program_instance(
        program,
        ecs_entity_to_u64(outliner_live.controllers[controller])
    );
    const pin_value *value = execution_instance_state(
        instance,
        script_catalog_id("symbol", "behaviors/OutlinerRows.chibi:1:Collapsed")
    );
    REQUIRE(value && value->type == PIN_BOOL && value->b == expected);
    return true;
}
static bool outliner_live_rows(const av_value *rows, const char *label)
{
    REQUIRE(rows && av_is_arr(rows) && av_arr_size(rows) == 1);
    const av_value *row = av_arr_get(rows, 0);
    REQUIRE(av_is_obj(row) && av_obj_size(row) == 12);
    REQUIRE(!strcmp(av_get_str(av_obj_get(row, "key")), outliner_live.actor_key));
    REQUIRE(!strcmp(av_get_str(av_obj_get(row, "label")), label));
    REQUIRE(!strcmp(av_get_str(av_obj_get(row, "icon")), "\xf3\xb0\x86\xa7"));
    REQUIRE(!strcmp(av_get_str(av_obj_get(row, "prototype")), ""));
    REQUIRE(!strcmp(av_get_str(av_obj_get(row, "drop")), "object behavior"));
    REQUIRE(av_get_int(av_obj_get(row, "depth")) == 1);
    REQUIRE(av_get_int(av_obj_get(row, "behaviors")) == 0);
    static const char *const flags[] = { "active", "marked", "hidden", "locked", "renaming" };
    for (int i = 0; i < 5; i++) {
        REQUIRE(
            av_is_bool(av_obj_get(row, flags[i])) &&
            !av_get_bool(av_obj_get(row, flags[i]))
        );
    }
    return true;
}
static bool outliner_live_projection(int controller, const char *label)
{
    REQUIRE(outliner_live_rows(outliner_live_value(controller, "rows"), label));
    REQUIRE(!strcmp(av_get_str(outliner_live_value(controller, "root.label")), "Live"));
    REQUIRE(!strcmp(av_get_str(outliner_live_value(controller, "root.icon")), "\xee\xaf\x92"));
    REQUIRE(av_get_bool(outliner_live_value(controller, "root.parent")));
    REQUIRE(av_get_int(outliner_live_value(controller, "root.depth")) == 0);
    REQUIRE(av_get_bool(outliner_live_value(controller, "root.open")));
    REQUIRE(av_get_bool(outliner_live_value(controller, "rootShown")));
    REQUIRE(av_get_int(outliner_live_value(controller, "count")) == 1);
    REQUIRE(av_get_int(outliner_live_value(controller, "total")) == 2);
    REQUIRE(!av_get_bool(outliner_live_value(controller, "filtered")));
    REQUIRE(!strcmp(av_get_str(outliner_live_value(controller, "notice")), ""));
    return true;
}
static bool outliner_live_setup_backend(behavior_backend backend)
{
    REQUIRE(backend == BEHAVIOR_BYTECODE || backend == BEHAVIOR_SLJIT);
    REQUIRE(behavior_backend_available(backend));
    outliner_live.execution_backend = backend;
    outliner_live.backend_ok = true;
    char path[1024];
    snprintf(path, sizeof path, "%s/libraries/OutlinerRowPolicies.chibi", outliner_live_content);
    outliner_live.library = file_read_text(path);
    snprintf(path, sizeof path, "%s/behaviors/OutlinerRows.chibi", outliner_live_content);
    outliner_live.behavior = file_read_text(path);
    REQUIRE(outliner_live.library && outliner_live.behavior);
    type_catalog_set_project(&STUDIO_OUTLINER_TYPE_PACK);
    outliner_live.catalog_set = true;
    native_type_catalog *types = native_type_catalog_new(
        STUDIO_OUTLINER_TYPE_DEFINITIONS,
        3,
        outliner_live.error,
        sizeof outliner_live.error
    );
    REQUIRE(types);
    uint64_t entity_id = script_catalog_id("type", "ecs.entity");
    script_catalog_type entity = { .id = entity_id,
                                   .name = "ecs.entity",
                                   .type = { .storage = PIN_HANDLE, .nominal = entity_id },
                                   .size = sizeof(uint64_t) };
    script_catalog base = { .types = &entity, .type_count = 1, .entity_type = entity_id };
    native_capability_pack extra[] = { native_type_catalog_pack(types),
                                       { .catalog = &base } };
    const native_definition_pack packs[] = { STUDIO_OUTLINER_DEFINITION_PACK,
                                             NATIVE_ARRAY_DEFINITION_PACK,
                                             NATIVE_TEXT_DEFINITION_PACK };
    outliner_live.native = host_native_catalog_new(
        extra,
        2,
        packs,
        3,
        outliner_live.error,
        sizeof outliner_live.error
    );
    native_type_catalog_free(types);
    REQUIRE(outliner_live.native);
    outliner_live.s = xcalloc(1, sizeof *outliner_live.s);
    outliner_live.world = ecs_world_new();
    outliner_live.sources = source_registry_new(outliner_live.world);
    outliner_live.ui_runtime.ecs = outliner_live.world;
    outliner_live.ui_runtime.sources = outliner_live.sources;
    outliner_live.ui_runtime.entities = entities_new(outliner_live.world, NULL);
    outliner_live.ui_live.runtime = &outliner_live.ui_runtime;
    outliner_live.model = ui_new();
    for (int i = 0; i < 2; i++) {
        app_scene_authoring *scene = xcalloc(1, sizeof *scene);
        outliner_live.scenes[i] = scene;
        scene->world = outliner_live.edited[i] = ecs_world_new();
        preview_runtime *runtime = &outliner_live.runtimes[i];
        runtime->ecs = scene->world;
        runtime->sources = source_registry_new(scene->world);
        REQUIRE(studio_outliner_guard_install(
            runtime->sources,
            outliner_live.error,
            sizeof outliner_live.error
        ));
        runtime->entities = entities_new(scene->world, NULL);
        outliner_live.lives[i].runtime = runtime;
        scene->live = &outliner_live.lives[i];
        scene->scene_path = xstrdup("scenes/Live.chibi");
        scene->view_store = save_new();
        scene->selected = -1;
        scene->active_runtime_target = APP_RUNTIME_SCENE;
        ecs_entity actor = ecs_world_create_entity(scene->world);
        apush(
            scene->actors,
            ((scene_actor){ .identity = 1, .entity = actor, .name = xstrdup(i ? "Beta" : "Alpha") })
        );
        if (!i) {
            snprintf(
                outliner_live.actor_key,
                sizeof outliner_live.actor_key,
                "%llu",
                (unsigned long long)ecs_entity_to_u64(actor)
            );
        }
    }
    REQUIRE(
        ecs_entity_to_u64(outliner_live.scenes[0]->actors[0].entity) ==
        ecs_entity_to_u64(outliner_live.scenes[1]->actors[0].entity)
    );
    outliner_live.s->scene = outliner_live.scenes[0];
    outliner_live.projection = studio_outliner_projection_new(
        outliner_live.s,
        outliner_live.sources,
        outliner_live.model,
        outliner_live.error,
        sizeof outliner_live.error
    );
    REQUIRE(outliner_live.projection);
    REQUIRE(
        preview_sources_prepare_with_catalog(
            &outliner_live.ui_live,
            OUTLINER_LIVE_ROOT,
            backend,
            outliner_live.native,
            outliner_live.error,
            sizeof outliner_live.error
        ) == 0
    );
    outliner_live.preparations++;
    REQUIRE(outliner_live_prepare(0));
    REQUIRE(
        preview_sources_publish(
            &outliner_live.ui_live,
            NULL,
            outliner_live.error,
            sizeof outliner_live.error
        ) == 0
    );
    for (int i = 0; i < 2; i++) {
        REQUIRE(
            source_register(
                outliner_live.runtimes[i].sources,
                "OutlinerRows",
                outliner_live.modules[0],
                NULL
            ) == FLOW_OK
        );
    }
    outliner_live.backend = rml_backend_create(320, 200, 1);
    REQUIRE(outliner_live.backend);
    rml_context *context = rml_backend_context(outliner_live.backend);
    ui_data_init(&outliner_live.data, context, NULL, outliner_live_emit, NULL);
    outliner_live.document = rml_context_create_document(context, "outliner-live");
    REQUIRE(outliner_live.document);
    outliner_live.listener = rml_listener_create(outliner_live_listen, NULL);
    REQUIRE(outliner_live.listener);
    const render_ui_loader loader = { .open = outliner_live_open,
                                      .read = outliner_live_read,
                                      .close = free };
    for (int i = 0; i < 2; i++) {
        outliner_live.controllers[i] = ecs_world_create_entity(outliner_live.world);
        REQUIRE(
            source_attach(outliner_live.sources, outliner_live.controllers[i], "OutlinerRows", false, 0, 0) ==
            FLOW_OK
        );
        outliner_live.roots[i] = rml_element_create_child(outliner_live.document, "div", NULL);
        outliner_live.mounts[i] = ui_mount_new(OUTLINER_LIVE_ROOT, i ? "Right" : "Left");
        ui_mount_set_loader(outliner_live.mounts[i], &loader);
        studio_outliner_projection_bind(
            outliner_live.projection,
            outliner_live.mounts[i],
            ecs_entity_to_u64(outliner_live.controllers[i])
        );
        ui_mount_sync(
            outliner_live.mounts[i],
            outliner_live.roots[i],
            outliner_live.listener,
            0
        );
        REQUIRE(!ui_mount_error(outliner_live.mounts[i])[0]);
        ui_activation_origin origin = ui_mount_origin(outliner_live.mounts[i]);
        REQUIRE(ui_mount_origin_current(outliner_live.mounts[i], &origin));
    }
    return true;
}

static bool outliner_live_setup()
{
    return outliner_live_setup_backend(BEHAVIOR_BYTECODE);
}

TEST_CASE(outliner_live_twelve_finite_scenarios)
{
    REQUIRE(outliner_live_setup());
    const char *window[] = { "0", "3" };
    const char *reveal[] = { outliner_live.actor_key };
    execution_program *program = source_program(outliner_live.sources, "OutlinerRows");

    /* 1. A current request publishes the complete production projection. */
    REQUIRE(outliner_live_event(0, 0, "ShowRows", 2, window));
    REQUIRE(outliner_live_event(0, 0, "Refresh", 0, NULL));
    REQUIRE(outliner_live_projection(0, "Alpha"));
    outliner_live.scenarios++;

    /* 2. A separate mount/controller publishes and owns its reveal command. */
    REQUIRE(outliner_live_event(1, 1, "ShowRows", 2, window));
    REQUIRE(outliner_live_event(1, 1, "RevealObject", 1, reveal));
    REQUIRE(outliner_live_projection(1, "Alpha"));
    REQUIRE(
        ui_data_count(outliner_live.model) == 22 &&
        ui_element_count(outliner_live.model) == 1
    );
    REQUIRE(ui_element_at(outliner_live.model, 0)->reveal == 1);
    outliner_live.scenarios++;

    /* 3. No-op publication retains the actual module and queued proof. */
    REQUIRE(outliner_live_expect_publication());
    outliner_live_enqueue(0, "Refresh", 0, NULL);
    REQUIRE(outliner_live_prepare(2));
    REQUIRE(authoring_sources_reused(outliner_live.prepared[2]) == 2);
    REQUIRE(
        preview_sources_publish(
            &outliner_live.ui_live,
            NULL,
            outliner_live.error,
            sizeof outliner_live.error
        ) == 0
    );
    REQUIRE(execution_program_module(program) == outliner_live.modules[0]);
    REQUIRE(outliner_live_deliver(0, 0, "Refresh", true));
    REQUIRE(outliner_live_projection(0, "Alpha"));
    outliner_live.scenarios++;

    /* 4. Production source editing prepares a changed library, then reverts
     * before publication. The distinct working module must not replace live. */
    REQUIRE(outliner_live_expect_publication());
    outliner_live_enqueue(0, "Refresh", 0, NULL);
    char *fallback = strstr(outliner_live.library, "return \"Scene\"");
    REQUIRE(fallback && !strstr(fallback + 1, "return \"Scene\""));
    memcpy(fallback + strlen("return \""), "World", 5);
    REQUIRE(outliner_live_prepare(1));
    memcpy(fallback + strlen("return \""), "Scene", 5);
    REQUIRE(outliner_live_prepare(2));
    REQUIRE(outliner_live.modules[2] != outliner_live.modules[0]);
    REQUIRE(
        preview_sources_publish(
            &outliner_live.ui_live,
            NULL,
            outliner_live.error,
            sizeof outliner_live.error
        ) == 0
    );
    REQUIRE(execution_program_module(program) == outliner_live.modules[0]);
    REQUIRE(outliner_live_deliver(0, 0, "Refresh", true));
    REQUIRE(outliner_live_projection(0, "Alpha"));
    outliner_live.scenarios++;

    /* 5. Real reload rejects an old queued origin without any model write. */
    REQUIRE(outliner_live_event(0, 0, "FoldRoot", 0, NULL));
    REQUIRE(outliner_live_collapsed(0, true));
    outliner_live_enqueue(0, "RevealObject", 1, reveal);
    uint64_t revision = ui_revision(outliner_live.model);
    source_publication reload[2];
    REQUIRE(authoring_sources_count(outliner_live.prepared[1]) == 2);
    for (int i = 0; i < 2; i++) {
        const authoring_source *source = authoring_sources_at(outliner_live.prepared[1], i);
        reload[i] = (source_publication){ .name = source->analysis->name,
                                          .previous_name = source->analysis->name,
                                          .module = source->module };
    }
    REQUIRE(
        source_publish(
            outliner_live.sources,
            reload,
            2,
            NULL,
            outliner_live.error,
            sizeof outliner_live.error
        ) == FLOW_OK
    );
    REQUIRE(execution_program_module(program) == outliner_live.modules[1]);
    REQUIRE(outliner_live_deliver(0, 0, "RevealObject", true));
    REQUIRE(outliner_live_collapsed(0, true));
    REQUIRE(
        ui_revision(outliner_live.model) == revision &&
        ui_element_at(outliner_live.model, 0)->reveal == 1
    );
    for (int i = 0; i < 2; i++) {
        studio_outliner_projection_bind(
            outliner_live.projection,
            outliner_live.mounts[i],
            ecs_entity_to_u64(outliner_live.controllers[i])
        );
    }
    REQUIRE(outliner_live_event(0, 0, "UnfoldRoot", 0, NULL));
    REQUIRE(outliner_live_event(0, 0, "Refresh", 0, NULL));
    outliner_live.scenarios++;

    /* 6. Removing a focused old input emits a real Blur during SetInnerRML.
     * Both older queued payloads and that teardown event remain FIFO and stale. */
    rml_document_show(outliner_live.document, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    rml_document_refresh_layout(outliner_live.document);
    rml_element *input = rml_element_query_selector(outliner_live.roots[0], "#old-input");
    REQUIRE(input && rml_element_focus(input, false));
    outliner_live_enqueue(0, "ShowRows", 2, window);
    outliner_live_enqueue(0, "Refresh", 0, NULL);
    outliner_live.markup[0]++;
    ui_mount_sync(outliner_live.mounts[0], outliner_live.roots[0], outliner_live.listener, 0);
    REQUIRE(!ui_mount_error(outliner_live.mounts[0])[0]);
    ui_activation activation;
    REQUIRE(ui_mount_take(outliner_live.mounts[0], &activation));
    REQUIRE(!strcmp(activation.event, "ShowRows") && activation.argc == 2);
    REQUIRE(!strcmp(activation.args[0], "0") && !strcmp(activation.args[1], "3"));
    REQUIRE(
        activation.button == 2 && activation.x == 13 && activation.y == 17 &&
        activation.modifiers == RML_KM_SHIFT
    );
    REQUIRE(!ui_mount_origin_current(outliner_live.mounts[0], &activation.origin));
    REQUIRE(outliner_live_send(0, 0, &activation, false));
    revision = ui_revision(outliner_live.model);
    REQUIRE(outliner_live_deliver(0, 0, "Refresh", true));
    REQUIRE(ui_mount_take(outliner_live.mounts[0], &activation));
    REQUIRE(
        !strcmp(activation.event, "RevealObject") && activation.argc == 1 &&
        !strcmp(activation.args[0], "old-payload")
    );
    REQUIRE(
        !activation.origin.generation && activation.origin.binding &&
        activation.origin.request
    );
    REQUIRE(!ui_mount_origin_current(outliner_live.mounts[0], &activation.origin));
    REQUIRE(outliner_live_send(0, 0, &activation, false));
    execution_program_clear_error(program);
    REQUIRE(
        !ui_mount_pending(outliner_live.mounts[0]) &&
        ui_revision(outliner_live.model) == revision
    );
    outliner_live.scenarios++;

    /* 7. Detach invalidates queued requests, while remount permits new ones. */
    outliner_live_enqueue(0, "Refresh", 0, NULL);
    studio_outliner_projection_unmount(outliner_live.projection, outliner_live.mounts[0]);
    ui_mount_detach(outliner_live.mounts[0]);
    REQUIRE(outliner_live_deliver(0, 0, "Refresh", true));
    REQUIRE(ui_revision(outliner_live.model) == revision);
    studio_outliner_projection_unmount(outliner_live.projection, outliner_live.mounts[1]);
    const render_ui_host host = authoring_ui_events(&outliner_live.ui_live);
    activation = (ui_activation){ .event = "RevealObject", .argc = 1 };
    snprintf(activation.args[0], sizeof activation.args[0], "%s", outliner_live.actor_key);
    outliner_live.sends++;
    REQUIRE(studio_outliner_projection_send(
        outliner_live.projection,
        &host,
        (ecs_entity){ 0 },
        &activation,
        NULL,
        false,
        0,
        0,
        0
    ));
    REQUIRE(ui_revision(outliner_live.model) == revision);
    snprintf(activation.args[0], sizeof activation.args[0], "retired-payload");
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[0],
        ecs_entity_to_u64(outliner_live.controllers[0])
    );
    ui_mount_sync(outliner_live.mounts[0], outliner_live.roots[0], outliner_live.listener, 0);
    REQUIRE(outliner_live_event(0, 0, "Refresh", 0, NULL));
    REQUIRE(
        ui_element_count(outliner_live.model) == 2 &&
        ui_element_at(outliner_live.model, 1)->reveal == 1
    );
    revision = ui_revision(outliner_live.model);
    outliner_live.scenarios++;

    /* 8. A controller change cannot repurpose an older mount origin. */
    outliner_live_enqueue(0, "Refresh", 0, NULL);
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[0],
        ecs_entity_to_u64(outliner_live.controllers[1])
    );
    REQUIRE(outliner_live_deliver(0, 1, "Refresh", true));
    REQUIRE(ui_revision(outliner_live.model) == revision);
    REQUIRE(outliner_live_event(0, 1, "Refresh", 0, NULL));
    outliner_live.scenarios++;

    /* 9. The edited world is replaced, with the same numeric actor identity.
     * Old captured rows survive their raw-read scope and original scene. */
    const av_value *retained = outliner_live_value(0, "rows");
    outliner_live_enqueue(0, "Refresh", 0, NULL);
    studio_outliner_projection_invalidate(outliner_live.projection);
    outliner_live.s->scene = outliner_live.scenes[1];
    outliner_live_scene_free(0);
    REQUIRE(outliner_live_rows(retained, "Alpha"));
    REQUIRE(outliner_live_deliver(0, 1, "Refresh", true));
    REQUIRE(ui_revision(outliner_live.model) == revision);
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[0],
        ecs_entity_to_u64(outliner_live.controllers[1])
    );
    REQUIRE(outliner_live_event(0, 1, "Refresh", 0, NULL));
    REQUIRE(outliner_live_projection(1, "Beta") && outliner_live_rows(retained, "Alpha"));
    outliner_live.scenarios++;

    /* 10. Unmount cancels only the suspended internal refresh. User controls
     * queued before cancellation still deliver in their original FIFO order. */
    outliner_live.inspector = execution_program_inspector(program);
    REQUIRE(
        outliner_live.inspector && execution_inspector_pause(
                                       outliner_live.inspector,
                                       ecs_entity_to_u64(outliner_live.controllers[1])
                                   )
    );
    activation = (ui_activation){ .event = "Refresh" };
    REQUIRE(outliner_live_send(0, 1, &activation, true));
    REQUIRE(execution_program_paused(program));
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[1],
        ecs_entity_to_u64(outliner_live.controllers[1])
    );
    outliner_live_enqueue(1, "FoldRoot", 0, NULL);
    outliner_live_enqueue(1, "UnfoldRoot", 0, NULL);
    outliner_live_enqueue(1, "ToggleRoot", 0, NULL);
    studio_outliner_projection_unmount(outliner_live.projection, outliner_live.mounts[0]);
    REQUIRE(outliner_live_pump(FLOW_OK));
    REQUIRE(!execution_program_paused(program));
    REQUIRE(outliner_live_deliver(1, 1, "FoldRoot", true));
    REQUIRE(outliner_live_deliver(1, 1, "UnfoldRoot", true));
    REQUIRE(outliner_live_deliver(1, 1, "ToggleRoot", true));
    execution_instance *instance = execution_program_instance(
        program,
        ecs_entity_to_u64(outliner_live.controllers[1])
    );
    const pin_value *collapsed = execution_instance_state(
        instance,
        script_catalog_id("symbol", "behaviors/OutlinerRows.chibi:1:Collapsed")
    );
    REQUIRE(
        collapsed && collapsed->type == PIN_BOOL && collapsed->b &&
        !execution_instance_pending(instance)
    );
    outliner_live.scenarios++;

    /* 11. A suspended user request is retained on unmount, but resumption
     * cannot recover the first uninterrupted event's publication authority. */
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[0],
        ecs_entity_to_u64(outliner_live.controllers[1])
    );
    REQUIRE(execution_inspector_pause(
        outliner_live.inspector,
        ecs_entity_to_u64(outliner_live.controllers[1])
    ));
    REQUIRE(outliner_live_event(0, 1, "Refresh", 0, NULL));
    REQUIRE(execution_program_paused(program));
    revision = ui_revision(outliner_live.model);
    studio_outliner_projection_unmount(outliner_live.projection, outliner_live.mounts[0]);
    REQUIRE(execution_program_paused(program) && execution_instance_pending(instance) == 1);
    behavior_run_scope resumed = { 0 };
    behavior_run_scope_begin(&resumed);
    int resumed_status = execution_inspector_control(outliner_live.inspector, EXECUTION_DEBUG_RESUME);
    behavior_run_scope_end(&resumed);
    outliner_live_count_runs(&resumed);
    REQUIRE(resumed_status == FLOW_ERR_INVALID_GRAPH);
    REQUIRE(outliner_live_pump(FLOW_ERR_INVALID_GRAPH));
    REQUIRE(!execution_program_paused(program) && !execution_instance_pending(instance));
    REQUIRE(ui_revision(outliner_live.model) == revision);
    outliner_live.scenarios++;

    /* 12. Fresh publication works after rejection. UI-owned rows and strings
     * remain valid after input scenes, source collections and producer retire. */
    studio_outliner_projection_bind(
        outliner_live.projection,
        outliner_live.mounts[0],
        ecs_entity_to_u64(outliner_live.controllers[1])
    );
    REQUIRE(outliner_live_event(0, 1, "UnfoldRoot", 0, NULL));
    REQUIRE(outliner_live_event(0, 1, "Refresh", 0, NULL));
    REQUIRE(outliner_live_projection(1, "Beta"));
    studio_outliner_projection_retire(outliner_live.projection);
    source_registry_free(outliner_live.sources);
    outliner_live.sources = NULL;
    studio_outliner_projection_free(outliner_live.projection);
    outliner_live.projection = NULL;
    outliner_live_source_views_free();
    execution_inspector_free(outliner_live.inspector);
    outliner_live.inspector = NULL;
    for (int i = 0; i < 3; i++) {
        authoring_sources_free(outliner_live.prepared[i]);
        outliner_live.prepared[i] = NULL;
    }
    outliner_live_scene_free(1);
    outliner_live.s->scene = NULL;
    REQUIRE(outliner_live_rows(retained, "Alpha") && outliner_live_projection(1, "Beta"));
    outliner_live.scenarios++;
    REQUIRE(outliner_live.scenarios == 12 && outliner_live.preparations == 5);
    REQUIRE(outliner_live.sends == 27 && outliner_live.pumps == 2);
    REQUIRE(
        outliner_live.backend_ok && outliner_live.invocations > 0 &&
        outliner_live.invocations + 2 < 1000
    );
    return true;
}

static int outliner_live_main(const char *content_root)
{
    outliner_live_content = content_root;
    pu_setup(NULL, outliner_live_cleanup);
    RUN_TEST_CASE(outliner_live_twelve_finite_scenarios);
    pu_clear_setup();
    pu_print_stats();
    return pu_test_failed() ? 1 : 0;
}
