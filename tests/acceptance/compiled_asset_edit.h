#include "authoring/asset_compiler_host.h"
#include "studio_registry.h"
#include "support/resources.h"

/* One independent editor contract, with no actors or checkpoint restore. */
static char compiled_edit_path[1024], compiled_edit_directory[1024];
static bool compiled_edit_file, compiled_edit_directory_created;

static bool compiled_edit_x(studio *s, float expected)
{
    pin_value value;
    REQUIRE(
        !app_scene_authoring_read_prototype_field(s->scene, "CompiledEdit", "Transform", "x", &value)
    );
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

static void compiled_model_cleanup(void)
{
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
    REQUIRE(generic_asset_source_refresh(project));
    REQUIRE(acount(s->records) == 0);
    asset_compiler_cache_stats after = asset_source_cache_stats();
    printf(
        "\nCompiled asset model: frames=0 renders=0 files=2 actors=0 "
        "checkpoints=0 properties_edits=3 undo=2 redo=2 saved=2 "
        "save_rejected=1 reload=1 disk_refreshes=2 attempts=%" PRIu64
        " memory_hits=%" PRIu64 " persistent_hits=%" PRIu64 "\n",
        after.compile_attempts - before.compile_attempts,
        after.memory_hits - before.memory_hits,
        after.persistent_hits - before.persistent_hits
    );
    return true;
}
