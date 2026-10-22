/* Frame one prepares a real source asset. Normal production emission renders
 * it in frame two, and the host performs its ordinary screenshot readback. */
static const char *typed_preview_capture;
static char typed_preview_path[1024], typed_preview_directory[1024];
static bool typed_preview_created_file, typed_preview_created_directory;

static bool typed_preview_begin(studio *s)
{
    int n = snprintf(typed_preview_directory, sizeof typed_preview_directory, "%s/content/prototypes", project);
    REQUIRE(n > 0 && (size_t)n < sizeof typed_preview_directory);
    typed_preview_created_directory = mkdir(typed_preview_directory, 0700) == 0;
    REQUIRE(typed_preview_created_directory || errno == EEXIST);
    n = snprintf(typed_preview_path, sizeof typed_preview_path, "%s/TypedAssetPreview.chibi", typed_preview_directory);
    REQUIRE(n > 0 && (size_t)n < sizeof typed_preview_path);
    REQUIRE(access(typed_preview_path, F_OK) != 0 && errno == ENOENT);
    const char *source =
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Parameters :: () -> data.Value {\n"
        "    return data.Object([data.Field.Named(\"amount\", "
        "data.Value.Real(2.0))])\n"
        "}\n"
        "\n"
        "pub Overrides :: () -> data.Value {\n"
        "    return data.Object([data.Field.Named(\"Transform\", "
        "data.Object([data.Field.Named(\"x\", data.Value.UInt(12))]))])\n"
        "}\n"
        "\n"
        "Twice :: (value: number) -> number {\n"
        "    return value * 2.0\n"
        "}\n"
        "\n"
        "MakeEnemy :: (parameters: data.Value) -> data.Value {\n"
        "    amount := data.AsReal(data.Get(parameters, \"amount\"))\n"
        "    return data.Object([\n"
        "        data.Field.Named(\"name\", "
        "data.Value.Text(\"TypedAssetPreview\")),\n"
        "        data.Field.Named(\"parameters\", parameters),\n"
        "        data.Field.Named(\"components\", data.Object([\n"
        "            data.Field.Named(\"Transform\", data.Object([\n"
        "                data.Field.Named(\"x\", "
        "data.Value.Real(Twice(amount))),\n"
        "                data.Field.Named(\"y\", data.Value.Real(amount))\n"
        "            ]))\n"
        "        ]))\n"
        "    ])\n"
        "}\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(data.ApplyPrototype(MakeEnemy(Parameters()), "
        "data.Object([\n"
        "        data.Field.Named(\"components\", Overrides()),\n"
        "        data.Field.Named(\"overrides\", Overrides())\n"
        "    ])))\n"
        "}\n";
    REQUIRE(!write_file_atomic(typed_preview_path, source));
    typed_preview_created_file = true;
    REQUIRE(!app_scene_authoring_register_prototype(s->scene, source, typed_preview_path));
    REQUIRE(!app_scene_authoring_open_prototype(s->scene, "TypedAssetPreview"));
    s->maximized = -1;
    studio_layout *layout = studio_active_layout(s);
    layout->side_open = true;
    layout->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_CODE;
    layout->editors[STUDIO_AREA_PROPERTIES] = STUDIO_EDITOR_PROPERTIES;
    s->tab = STUDIO_TAB_OBJECT;
    s->property_search[0] = 0;
    s->prefs.reduced_motion = true;
    kit_set_theme(s->kit, "reduced-motion", true);
    studio_report(s, "Typed prototype inspector review");
    return true;
}

static bool typed_preview_visible(rml_element *element)
{
    if (!element || !rml_element_is_visible(element, true)) { return false; }
    rml_vec2 size = rml_element_box_size(element, RML_BOX_BORDER);
    return size.x > 0 && size.y > 0;
}

static bool typed_preview_verify(studio *s)
{
    rml_element *parameters = studio_element(s, ".panel[data-key='parameters']");
    REQUIRE(typed_preview_visible(parameters));
    REQUIRE(typed_preview_visible(rml_element_query_selector(parameters, "number-field")));
    rml_context *context = rml_backend_context(s->backend);
    bool computed = false, overridden = false, reset_visible = false;
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *resets[64];
        int reset_count = rml_element_query_selector_all(
            rml_context_document(context, i),
            "button[data-tip='Reset override']",
            resets,
            64
        );
        REQUIRE(reset_count <= 64);
        for (int j = 0; j < reset_count; j++) {
            reset_visible |= typed_preview_visible(resets[j]);
        }
        rml_element *origins[64];
        int count = rml_element_query_selector_all(
            rml_context_document(context, i),
            ".source-origin",
            origins,
            64
        );
        REQUIRE(count <= 64);
        for (int j = 0; j < count; j++) {
            char text[32];
            rml_element_inner_rml(origins[j], text, sizeof text);
            computed |= typed_preview_visible(origins[j]) && !strcmp(text, "Computed");
            overridden |= typed_preview_visible(origins[j]) &&
                          !strcmp(text, "Override");
        }
    }
    REQUIRE(computed && overridden && reset_visible);
    REQUIRE(bridge_field(s->scene, "TypedAssetPreview", "x", 12));
    REQUIRE(bridge_field(s->scene, "TypedAssetPreview", "y", 2));
    return true;
}

static bool typed_prototype_preview_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s);
    frames++;
    REQUIRE(frames <= 2);
    return frames == 1 ? typed_preview_begin(s) : typed_preview_verify(s);
}

TEST_CASE(typed_prototype_preview)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--screenshot",     (char *)typed_preview_capture };
    int status = studio_acceptance_host_main(sizeof args / sizeof *args, args);
    /* The host and source overlays are gone before fixture files are removed. */
    bool clean = !typed_preview_created_file || !unlink(typed_preview_path);
    if (typed_preview_created_directory) {
        clean &= !rmdir(typed_preview_directory);
    }
    typed_preview_created_file = typed_preview_created_directory = false;
    REQUIRE(status == 0 && clean && callback_ok && frames == 2);
    struct stat image;
    REQUIRE(!stat(typed_preview_capture, &image) && image.st_size > 0);
    printf("\nTyped prototype rendered review: frames=2 files=1 actors=0 checkpoints=0 screenshots=1\n");
    return true;
}
