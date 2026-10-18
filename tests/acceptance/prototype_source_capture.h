/* Test-only rendered view of the real retained-draft notice. Setup runs after
 * frame one; the real application renders and captures frame two before its
 * acceptance callback. Keep the model, input and transport alive until shutdown. */
static authority_fixture source_capture;
static char source_capture_path[1024], source_capture_directory[1024];
static const char source_capture_shared[] = "AuthoritySource :: prototype { "
                                            "Transform {x:1,y:2} }\n";
static const char source_capture_draft[] = "AuthoritySource :: prototype { "
                                           "Transform {x:9,y:2} }\n";
static const void *source_capture_step;
static uint64_t source_capture_identity;
static int source_capture_toast;
static bool source_capture_started, source_capture_created_directory;

static void prototype_source_capture_compose(studio *s)
{
    /* Preserve the actual workspace/area/document keys that own Code focus. */
    kit_begin(s->kit, &s->in);
    studio_emit(s);
    kit_end(s->kit);
}

static bool prototype_source_capture_begin(studio *s)
{
    int n = snprintf(source_capture_directory, sizeof source_capture_directory, "%s/content/prototypes", project);
    REQUIRE(n > 0 && (size_t)n < sizeof source_capture_directory);
    source_capture_created_directory = mkdir(source_capture_directory, 0700) == 0;
    REQUIRE(source_capture_created_directory || errno == EEXIST);
    n = snprintf(source_capture_path, sizeof source_capture_path, "%s/AuthoritySource.chibi", source_capture_directory);
    REQUIRE(n > 0 && (size_t)n < sizeof source_capture_path);
    REQUIRE(access(source_capture_path, F_OK) != 0 && errno == ENOENT);
    source_capture_started = true;
    REQUIRE(!write_file_atomic(source_capture_path, source_capture_shared));
    REQUIRE(
        !app_scene_authoring_register_prototype(s->scene, source_capture_shared, source_capture_path)
    );
    REQUIRE(!app_scene_authoring_open_prototype(s->scene, "AuthoritySource"));
    s->maximized = -1;
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_CODE;
    s->prefs.code_vim = false;
    s->prefs.reduced_motion = true;
    s->code.find.refocus = true;
    source_capture = (authority_fixture){ .s = s,
                                          .path = source_capture_path,
                                          .valid = true };
    REQUIRE(authority_start(&source_capture, "editor"));
    REQUIRE(!authority_raw(&source_capture, source_capture_draft));
    asset_code *code = authority_asset(&source_capture)->source;
    source_capture_step = asset_code_step(code);
    prototype_source_capture_compose(s);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *focus = rml_context_focus_element(context);
    REQUIRE(focus && !strcmp(rml_element_tag_name(focus), "kit-code"));
    REQUIRE(authority_reject(&source_capture));
    REQUIRE(authority_text(&source_capture, source_capture_shared, source_capture_draft));
    prototype_source_capture_compose(s);
    source_capture_identity = scene_source_input_conflict_identity(s->scene);
    source_capture_toast = s->source_input_toast;
    REQUIRE(source_capture_identity && source_capture_toast);
    REQUIRE(rml_context_focus_element(context) == focus);
    /* The real toast is composed; Enter must remain the code editor's. */
    kit_code_set_cursor(asset_code_editor(code), (int)strlen(source_capture_draft));
    return press_key(SDLK_RETURN);
}

static bool prototype_source_capture_verify(studio *s)
{
    REQUIRE(s == source_capture.s);
    REQUIRE(s->source_input_toast == source_capture_toast);
    REQUIRE(scene_source_input_conflict_identity(s->scene) == source_capture_identity);
    REQUIRE(
        authority_text(&source_capture, source_capture_shared, "AuthoritySource :: prototype { Transform {x:9,y:2} }\n\n")
    );
    REQUIRE(asset_code_step(authority_asset(&source_capture)->source) == source_capture_step);
    rml_element *focus = rml_context_focus_element(rml_backend_context(s->backend));
    REQUIRE(focus && !strcmp(rml_element_tag_name(focus), "kit-code"));
    REQUIRE(
        source_capture.valid && source_capture.received == 2 && source_capture.sent == 2
    );
    REQUIRE(source_capture.proposals == 1 && source_capture.snapshots == 1);
    REQUIRE(source_capture.applies == 0 && source_capture.accepted == 0);
    char *disk = file_read_text(source_capture_path);
    bool untouched = disk && !strcmp(disk, source_capture_shared);
    free(disk);
    REQUIRE(untouched);
    printf("Prototype source controls: notice_retained=1 focus_retained=1 enter_edits_draft=1\n");
    return true;
}

static bool prototype_source_capture_cleanup()
{
    /* The real host has already destroyed its model and collaboration state. */
    free(source_capture.action);
    for (int i = 0; i < acount(source_capture.owned); i++) {
        free(source_capture.owned[i]);
    }
    afree(source_capture.owned);
    bool clean = true;
    if (source_capture_started) {
        clean = !unlink(source_capture_path);
        if (source_capture_created_directory) {
            clean &= !rmdir(source_capture_directory);
        }
    }
    memset(&source_capture, 0, sizeof source_capture);
    source_capture_started = false;
    REQUIRE(clean);
    return true;
}
