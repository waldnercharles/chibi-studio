#include "authoring/asset_compiler_host.h"
#include "compiled_asset_history.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/protocol.h"
#include "studio_registry.h"
#include "support/resources.h"
#include "types/text.h"

/* One independent editor contract, with no actors or checkpoint restore. */
static char compiled_edit_path[1024], compiled_edit_directory[1024];
static bool compiled_edit_file, compiled_edit_directory_created;

static bool compiled_edit_x(studio *s, float expected)
{
    pin_value value;
    REQUIRE(!app_scene_authoring_read_prototype_field(
        &acceptance_field_reads,
        s->scene,
        "CompiledEdit",
        "Transform",
        "x",
        &value
    ));
    REQUIRE(value.type == PIN_F32 && value.f == expected);
    return true;
}

static bool compiled_edit_contract(studio *s)
{
    int n = snprintf(compiled_edit_directory, sizeof compiled_edit_directory, "%s/content/prototypes", project);
    REQUIRE(n > 0 && (size_t)n < sizeof compiled_edit_directory);
    compiled_edit_directory_created = !mkdir(compiled_edit_directory, 0700);
    REQUIRE(compiled_edit_directory_created || errno == EEXIST);
    n = snprintf(compiled_edit_path, sizeof compiled_edit_path, "%s/CompiledEdit.chibi", compiled_edit_directory);
    REQUIRE(n > 0 && (size_t)n < sizeof compiled_edit_path);
    REQUIRE(access(compiled_edit_path, F_OK) && errno == ENOENT);
    const char *source =
        "package asset\nimport data \"game:assets\"\n"
        "// Preserve the authored helper.\n"
        "Twice :: (value: uint64) -> uint64 { return value + value }\n"
        "pub Parameters :: () -> data.Value { return data.Object(["
        "data.Field.Named(\"amount\", data.Value.UInt(2))]) }\n"
        "pub Overrides :: () -> data.Value { return data.Object([]) }\n"
        "Make :: (parameters: data.Value) -> data.Value { return data.Object(["
        "data.Field.Named(\"name\", data.Value.Text(\"CompiledEdit\")),"
        "data.Field.Named(\"parameters\", parameters),"
        "data.Field.Named(\"components\", data.Object(["
        "data.Field.Named(\"Transform\", data.Object(["
        "data.Field.Named(\"x\", "
        "data.Value.UInt(Twice(data.AsUInt(data.Get(parameters, "
        "\"amount\")))))]))]))]) }\n"
        "pub Build :: () -> byte[] { return "
        "data.Encode(data.ApplyPrototype(Make(Parameters()), data.Object(["
        "data.Field.Named(\"components\", Overrides()), "
        "data.Field.Named(\"overrides\", Overrides())]))) }\n";
    REQUIRE(!write_file_atomic(compiled_edit_path, source));
    compiled_edit_file = true;
    int registered = app_scene_authoring_register_prototype(s->scene, source, compiled_edit_path);
    if (registered) {
        char error[256] = "";
        av_doc *diagnostic = authoring_prototype_document_read(source, &s->scene->registry, error, sizeof error);
        fprintf(stderr, "Compiled edit registration: %s\n", error);
        av_doc_free(diagnostic);
    }
    REQUIRE(!registered);
    REQUIRE(!app_scene_authoring_open_prototype(s->scene, "CompiledEdit"));
    s->tab = STUDIO_TAB_OBJECT;
    properties_subject subject;
    properties_subject_of(s, &subject);
    scene_prototype_asset *asset = subject.asset;
    REQUIRE(asset && asset->source && !subject.locked);
    REQUIRE(compiled_edit_x(s, 4));

    properties_row parameter;
    properties_parameter_row(compiled_edit_path, "amount", 0, &parameter);
    ui_activation change = { .argc = 4 };
    properties_row_id(&parameter, change.args[0]);
    snprintf(change.args[1], sizeof change.args[1], "3");
    studio_properties_action(s, "change", &change);
    REQUIRE(compiled_edit_x(s, 6));
    av_doc *parameters = authoring_prototype_source_parameters(
        asset_code_text(asset->source),
        NULL,
        0
    );
    av_value *amount = av_obj_get(av_doc_root(parameters), "amount");
    REQUIRE(av_is_uint(amount) && av_get_uint(amount) == 3);
    av_doc_free(parameters);

    const ecs_component_desc *transform = NULL;
    for (int i = 0; i < s->scene->registry.component_count; i++) {
        if (!strcmp(s->scene->registry.components[i]->name, "Transform")) {
            transform = s->scene->registry.components[i];
        }
    }
    REQUIRE(transform);
    int x = -1;
    for (int i = 0; i < transform->type->field_count; i++) {
        if (!strcmp(transform->type->fields[i].name, "x")) {
            x = i;
        }
    }
    REQUIRE(x >= 0);
    properties_row field = { .kind = PROPERTIES_PROTOTYPE,
                             .component = transform->id,
                             .field = x };
    snprintf(field.prototype, sizeof field.prototype, "CompiledEdit");
    properties_row_id(&field, change.args[0]);
    snprintf(change.args[1], sizeof change.args[1], "12");
    studio_properties_action(s, "change", &change);
    REQUIRE(compiled_edit_x(s, 12));
    authoring_prototype_field_source origin;
    REQUIRE(
        authoring_prototype_source_field(asset_code_text(asset->source), "Transform", "x", &origin)
    );
    REQUIRE(origin.computed && origin.overridden);
    char *overridden = xstrdup(asset_code_text(asset->source));
    ui_activation reset = { .argc = 1 };
    properties_row_id(&field, reset.args[0]);
    studio_properties_action(s, "resetOverride", &reset);
    REQUIRE(compiled_edit_x(s, 6));
    REQUIRE(
        authoring_prototype_source_field(asset_code_text(asset->source), "Transform", "x", &origin)
    );
    REQUIRE(origin.computed && !origin.overridden);
    char *reset_source = xstrdup(asset_code_text(asset->source));
    REQUIRE(!prototype_undo(s->scene));
    REQUIRE(compiled_edit_x(s, 12) && !strcmp(asset_code_text(asset->source), overridden));
    REQUIRE(!prototype_redo(s->scene));
    REQUIRE(compiled_edit_x(s, 6) && !strcmp(asset_code_text(asset->source), reset_source));
    free(overridden);
    free(reset_source);
    REQUIRE(strstr(asset_code_text(asset->source), "// Preserve the authored helper."));
    REQUIRE(
        strstr(asset_code_text(asset->source), "Twice(data.AsUInt(data.Get(parameters, \"amount\")))")
    );
    REQUIRE(!scene_prototype_source_save(s->scene, asset));
    char *saved = file_read_text(compiled_edit_path);
    REQUIRE(saved && !strcmp(saved, asset_code_text(asset->source)));
    REQUIRE(!scene_prototype_source_reload(s->scene, asset));
    REQUIRE(!asset_code_dirty(asset->source) && !strcmp(asset_code_text(asset->source), saved));
    REQUIRE(strstr(saved, "// Preserve the authored helper."));
    REQUIRE(strstr(saved, "Twice(data.AsUInt(data.Get(parameters, \"amount\")))"));
    free(saved);
    REQUIRE(compiled_edit_x(s, 6));
    return true;
}

static bool compiled_asset_edit_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && ++frames <= 2);
    /* Editor model actions remain usable when authored UI startup fails.
     * Require the actual Studio world and shell before exercising edits. */
    REQUIRE(s->ui);
    REQUIRE(!strcmp(studio_ui_page(s->ui), "StudioShell"));
    int panels = studio_ui_panel_count(s);
    REQUIRE(panels > 0);
    bool properties = false;
    for (int i = 0; i < panels; i++) {
        properties |= !strcmp(studio_ui_panel_name(s, i), "Properties");
    }
    REQUIRE(properties);
    REQUIRE(
        !strcmp(s->page.name, "StudioShell") && s->page.document.rml &&
        !s->page.error[0]
    );
    if (frames == 1) {
        return compiled_edit_contract(s);
    }
    REQUIRE(compiled_edit_x(s, 6));
    return true;
}

TEST_CASE(compiled_asset_edit)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    asset_compiler_cache_stats before = asset_source_cache_stats();
    int status = studio_acceptance_host_main(sizeof args / sizeof *args, args);
    asset_compiler_cache_stats after = asset_source_cache_stats();
    printf(
        "\nCompiled asset cache: attempts=%" PRIu64 " memory_hits=%" PRIu64
        " persistent_hits=%" PRIu64 "\n",
        after.compile_attempts - before.compile_attempts,
        after.memory_hits - before.memory_hits,
        after.persistent_hits - before.persistent_hits
    );
    bool clean = !compiled_edit_file || !unlink(compiled_edit_path);
    if (compiled_edit_directory_created) {
        clean &= !rmdir(compiled_edit_directory);
    }
    REQUIRE(status == 0 && clean && callback_ok && frames == 2);
    printf("\nCompiled asset edit: frames=2 files=1 actors=0 checkpoints=0 edits=3 undo=1 redo=1 save=1 reload=1\n");
    return true;
}

/* CPU model coverage deliberately skips the application/bootstrap/render loop.
   A real kit/context owns error reporting; neither is emitted or rendered. */
static move_fixture compiled_model_fixture;
static studio *compiled_model_studio;
static rml_backend *compiled_model_backend;
static bool compiled_model_registry;
static char compiled_model_root[] = "/tmp/chibi-compiled-edit-model-XXXXXX";
static editor *compiled_collab_editor;
static arena compiled_collab_source;
static char *compiled_collab_action, *compiled_collab_envelope;
static collab_message compiled_collab_message;

static void compiled_collab_cleanup(void)
{
    free(compiled_collab_action);
    compiled_collab_action = NULL;
    free(compiled_collab_envelope);
    compiled_collab_envelope = NULL;
    collab_message_free(&compiled_collab_message);
    arena_free(&compiled_collab_source);
    editor_free(compiled_collab_editor);
    compiled_collab_editor = NULL;
}

/* Use the graph constant producer and action consumer without source compilation
 * or a session handshake. The destination must outlive every wire temporary. */
static bool compiled_model_collab_bytes(void)
{
    static const char utf8[] = "caf\xc3\xa9";
    static const char nul[] = { 'A', 0, 'B' };
    static const char invalid[] = { 'A', (char)0xff, (char)0xc0, (char)0xaf };
    const char *inputs[] = { utf8, nul, invalid };
    size_t lengths[] = { sizeof utf8 - 1, sizeof nul, sizeof invalid };
    compiled_collab_editor = editor_new(behavior_asset_new("CollabBytes"));
    editor *e = compiled_collab_editor;
    node *target = graph_add_node(e->active_graph, &NODE_PrintString);
    REQUIRE(target && target->input_count == 2 && target->input_pins[1].type == PIN_STR);
    REQUIRE(target->input_pins[1].type_name);
    uint64_t id = target->id;
    for (int i = 0; i < 3; i++) {
        pin_value source;
        REQUIRE(!pin_value_text(&compiled_collab_source, &source, inputs[i], lengths[i]));
        target = graph_node_by_id(e->active_graph, id);
        source.type_name = target->input_pins[1].type_name;
        compiled_collab_action = graph_collab_action_set_input_default(e, target, 1, &source);
        REQUIRE(compiled_collab_action);
        arena_free(&compiled_collab_source);
        compiled_collab_envelope = collab_message_write(
            "byte-room",
            "producer",
            (uint64_t)i + 1,
            (uint64_t)i + 1,
            "accepted",
            compiled_collab_action
        );
        free(compiled_collab_action);
        compiled_collab_action = NULL;
        REQUIRE(compiled_collab_envelope);
        REQUIRE(collab_message_read(compiled_collab_envelope, &compiled_collab_message));
        free(compiled_collab_envelope);
        compiled_collab_envelope = NULL;
        yyjson_val *data = yyjson_obj_get(
            yyjson_obj_get(compiled_collab_message.payload, "default"),
            "data"
        );
        REQUIRE(
            i == 2 ? yyjson_is_obj(data) && yyjson_is_arr(yyjson_obj_get(data, "bytes"))
                   : yyjson_is_str(data)
        );
        compiled_collab_action = yyjson_val_write(compiled_collab_message.payload, 0, NULL);
        REQUIRE(compiled_collab_action);
        collab_message_free(&compiled_collab_message);
        char error[192];
        bool applied = collab_actions_apply(NULL, e, compiled_collab_action, error, sizeof error);
        free(compiled_collab_action);
        compiled_collab_action = NULL;
        REQUIRE(applied);
        REQUIRE(!error[0]);
        target = graph_node_by_id(e->active_graph, id);
        REQUIRE(target && target->input_pins[1].default_value.type == PIN_STR);
        pin_string actual = pin_text(target->input_pins[1].default_value);
        REQUIRE(actual.length == lengths[i]);
        REQUIRE(!memcmp(actual.bytes, inputs[i], lengths[i]));
        REQUIRE(
            target->input_pins[1].default_value.type_name &&
            !strcmp(
                target->input_pins[1].default_value.type_name,
                target->input_pins[1].type_name
            )
        );
    }
    const size_t length = 1000000;
    char *bytes = arena_alloc(&compiled_collab_source, length);
    memset(bytes, 0xff, length);
    pin_value rejected;
    REQUIRE(!pin_value_text(&compiled_collab_source, &rejected, bytes, length));
    rejected.type_name = target->input_pins[1].type_name;
    pin_value previous = target->input_pins[1].default_value;
    compiled_collab_action = graph_collab_action_for_command(
        e,
        (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                          .data.set_pin_default = { id, 0, 1, &rejected, false } }
    );
    arena_free(&compiled_collab_source);
    REQUIRE(!compiled_collab_action);
    REQUIRE(
        target->input_pins[1].default_value.type == PIN_STR &&
        target->input_pins[1].default_value.text == previous.text
    );
    pin_string actual = pin_text(target->input_pins[1].default_value);
    REQUIRE(actual.length == sizeof invalid);
    REQUIRE(!memcmp(actual.bytes, invalid, sizeof invalid));
    compiled_collab_cleanup();
    return true;
}

static void compiled_model_cleanup(void)
{
    compiled_collab_cleanup();
    compiled_history_cleanup();
    generic_asset_source_cleanup();
    if (compiled_model_fixture.scene) {
        move_fixture_free(&compiled_model_fixture);
        compiled_model_fixture = (move_fixture){ 0 };
    }
    if (compiled_model_studio) {
        kit_destroy(compiled_model_studio->kit);
        afree(compiled_model_studio->records);
        free(compiled_model_studio);
        compiled_model_studio = NULL;
    }
    rml_backend_destroy(compiled_model_backend);
    compiled_model_backend = NULL;
    if (compiled_model_registry) {
        studio_registry_free();
    }
    compiled_model_registry = false;
    if (compiled_edit_file) {
        unlink(compiled_edit_path);
    }
    if (compiled_edit_directory_created) {
        rmdir(compiled_edit_directory);
    }
    compiled_edit_file = compiled_edit_directory_created = false;
    char content[1024];
    snprintf(content, sizeof content, "%s/content", compiled_model_root);
    rmdir(content);
    rmdir(compiled_model_root);
}

/* Runtime text can contain bytes that the C-string controls cannot edit. This
 * fixture uses the existing model world/kit; no asset compilation is involved. */
typedef struct
{
    const pin_string *label, *asset;
} compiled_nul_fields;

static const type_field compiled_nul_type_fields[] = {
    { .name = "label",
      .type = PIN_STR,
      .offset = offsetof(compiled_nul_fields, label),
      .size = sizeof(const pin_string *),
      .text_descriptor = true },
    { .name = "asset",
      .type = PIN_STR,
      .offset = offsetof(compiled_nul_fields, asset),
      .size = sizeof(const pin_string *),
      .text_descriptor = true },
};
static const type_desc compiled_nul_type = {
    /* Exercise the ordinary named-system asset field as well as plain text. */
    .name = "EngineSystem",
    .size = sizeof(compiled_nul_fields),
    .fields = compiled_nul_type_fields,
    .field_count = 2,
};
static const ecs_component_desc compiled_nul_component = {
    .id = UINT64_C(0x6e756c74657874),
    .key = "acceptance.nul-text",
    .name = "NulText",
    .element_size = sizeof(compiled_nul_fields),
    .type = &compiled_nul_type,
};

static bool compiled_model_nul_controls(studio *s)
{
    app_scene_authoring *scene = s->scene;
    prototype_registry registry = scene->registry;
    app_editor_document_context document = scene->document;
    int selected = scene->selected, actors = acount(scene->actors);
    studio_tab tab = s->tab;
    char added[sizeof s->components.added];
    memcpy(added, s->components.added, sizeof added);
    const ecs_component_desc **components = xmalloc(
        (size_t)(registry.component_count + 1) * sizeof *components
    );
    memcpy(components, registry.components, (size_t)registry.component_count * sizeof *components);
    components[registry.component_count] = &compiled_nul_component;
    ecs_entity entity = { 0 };
    yyjson_mut_doc *doc = NULL;
    rml_data_model *model = NULL;
    arena reads = { 0 };
    bool ready = false, pushed = false, cleaned = true, no_history = false;
    bool label[2] = { 0 }, readonly[2] = { 0 }, diagnostic[2] = { 0 };
    bool escaped[2] = { 0 }, unchanged[2] = { 0 };
    const pin_string text = { .bytes = "a\0b", .length = 3 };
    compiled_nul_fields initial = { &text, &text };
    if (actors ||
        ecs_world_register_component(scene->world, &compiled_nul_component, NULL) !=
            ECS_OK) {
        goto cleanup;
    }
    entity = ecs_world_create_entity(scene->world);
    if (!entity.slot ||
        ecs_world_record_add(scene->world, entity, compiled_nul_component.id, &initial, 0) !=
            ECS_OK ||
        ecs_world_apply_commands(scene->world) != ECS_OK) {
        goto cleanup;
    }
    scene->registry.components = components;
    scene->registry.component_count++;
    apush(
        scene->actors,
        ((scene_actor){ .identity = 1, .entity = entity, .name = "NulText", .prototype_name = "NulText" })
    );
    pushed = true;
    scene->selected = actors;
    scene->document = (app_editor_document_context){ APP_EDITOR_DOCUMENT_SCENE, NULL };
    s->tab = STUDIO_TAB_OBJECT;
    snprintf(s->components.added, sizeof s->components.added, "NulText");
    doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));
    studio_properties_add(doc);
    model = rml_data_model_create(rml_backend_context(compiled_model_backend), "nul_controls", doc, NULL, NULL);
    if (!model) {
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "properties");
    studio_properties_sync(
        s,
        &(studio_panel_sync){ .doc = doc, .variable = root, .model = model, .preview = true }
    );
    yyjson_mut_val *panels = yyjson_mut_obj_get(root, "panels");
    yyjson_mut_val *panel = yyjson_mut_arr_get(panels, 0);
    yyjson_mut_val *rows = yyjson_mut_obj_get(panel, "rows");
    ready = yyjson_mut_arr_size(panels) == 1 && yyjson_mut_arr_size(rows) == 2 &&
            properties_named(
                &compiled_nul_component,
                &compiled_nul_type_fields[1],
                &(pin_value){ .type = PIN_STR, .text = &text }
            ) == REFERENCE_ASSET_SYSTEM;
    for (int i = 0; ready && i < 2; i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(rows, (size_t)i);
        const char *control = yyjson_mut_get_str(yyjson_mut_obj_get(row, "control"));
        const char *problem = yyjson_mut_get_str(yyjson_mut_obj_get(row, "problem"));
        const char *shown = yyjson_mut_get_str(yyjson_mut_obj_get(row, "text"));
        label[i] = control && !strcmp(control, "label");
        readonly[i] = yyjson_mut_get_bool(yyjson_mut_obj_get(row, "readonly")) &&
                      yyjson_mut_get_bool(yyjson_mut_obj_get(row, "invalid"));
        diagnostic[i] = problem && strstr(problem, "embedded NUL");
        escaped[i] = shown && !strcmp(shown, "a\\x00b");
        ui_activation activation = { .argc = 2 };
        properties_row_id(
            &(properties_row){ .kind = PROPERTIES_OBJECT,
                               .entity = ecs_entity_to_u64(entity),
                               .component = compiled_nul_component.id,
                               .field = i },
            activation.args[0]
        );
        /* Both checkbox states must be refused for a non-bool field. */
        studio_properties_action(s, "check", &activation);
        snprintf(activation.args[1], sizeof activation.args[1], "1");
        studio_properties_action(s, "check", &activation);
        pin_value value = { 0 };
        unchanged[i] = ecs_component_read_field(
                           &reads,
                           scene->world,
                           entity,
                           compiled_nul_component.id,
                           i,
                           &value
                       ) == ECS_OK &&
                       value.type == PIN_STR && pin_text(value).length == 3 &&
                       !memcmp(pin_text(value).bytes, "a\0b", 3);
    }
    no_history = !scene->property_actor.slot && !scene->scene_dirty;
cleanup:
    if (model) {
        rml_data_model_destroy(model);
    }
    yyjson_mut_doc_free(doc);
    if (pushed) {
        (void)apop(scene->actors);
    }
    scene->registry = registry;
    scene->document = document;
    scene->selected = selected;
    s->tab = tab;
    memcpy(s->components.added, added, sizeof added);
    ecs_world_clear_commands(scene->world);
    if (entity.slot) {
        cleaned = ecs_world_destroy_entity(scene->world, entity) == ECS_OK;
    }
    free(components);
    arena_free(&reads);
    REQUIRE(ready);
    for (int i = 0; i < 2; i++) {
        REQUIRE(label[i]);
        REQUIRE(readonly[i]);
        REQUIRE(diagnostic[i]);
        REQUIRE(escaped[i]);
        REQUIRE(unchanged[i]);
    }
    REQUIRE(no_history);
    REQUIRE(cleaned && acount(scene->actors) == actors);
    return true;
}

TEST_CASE(compiled_asset_edit_model)
{
    REQUIRE(mkdtemp(compiled_model_root));
    project = compiled_model_root;
    char content[1024], ui[4096], studio_resources[4096];
    snprintf(content, sizeof content, "%s/content", project);
    REQUIRE(!mkdir(content, 0700));
    const char *installed = resource_install_root();
    REQUIRE(installed);
    snprintf(ui, sizeof ui, "%s/ui", installed);
    snprintf(studio_resources, sizeof studio_resources, "%s/studio", installed);
    resource_configure(installed, ui, studio_resources);
    studio_registry_init();
    compiled_model_registry = true;
    asset_compiler_cache_stats before = asset_source_cache_stats();
    compiled_model_fixture = move_fixture_new_with_root(content);
    compiled_model_studio = xcalloc(1, sizeof *compiled_model_studio);
    studio *s = compiled_model_studio;
    s->scene = compiled_model_fixture.scene;
    s->key_area = s->graph.area = -1;
    s->focus_area = STUDIO_AREA_PROPERTIES;
    s->prefs.code_apply_live = true;
    compiled_model_backend = rml_backend_create(64, 64, 1);
    REQUIRE(compiled_model_backend);
    s->kit = kit_create(rml_backend_context(compiled_model_backend), "studio.rml");
    REQUIRE(s->kit && s->scene && studio_editable(s));
    REQUIRE(compiled_edit_contract(s));
    REQUIRE(compiled_model_nul_controls(s));
    REQUIRE(compiled_model_collab_bytes());
    REQUIRE(compiled_model_history());
    REQUIRE(generic_asset_source_refresh(project));
    REQUIRE(acount(s->records) == 0);
    asset_compiler_cache_stats after = asset_source_cache_stats();
    printf(
        "\nCompiled asset model: frames=0 renders=0 files=2 actors=0 "
        "checkpoints=10 checkpoint_restores=4 properties_edits=3 "
        "nul_view_actors=1 nul_rows=2 "
        "stale_checks=4 collab_byte_roundtrips=3 collab_budget_refusals=1 "
        "history_byte_cases=3 "
        "history_rejection_cases=3 history_worlds=6 history_undo_attempts=6 "
        "history_redo=3 source_undo=2 source_redo=2 saved=2 "
        "save_rejected=1 reload=1 disk_refreshes=2 attempts=%" PRIu64
        " memory_hits=%" PRIu64 " persistent_hits=%" PRIu64 "\n",
        after.compile_attempts - before.compile_attempts,
        after.memory_hits - before.memory_hits,
        after.persistent_hits - before.persistent_hits
    );
    return true;
}
