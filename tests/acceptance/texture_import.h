#include "authoring/image_import_commit.h"
#include "authoring/image_prepare.h"
#include "platform/cute.h"
#include "runtime/images/catalog.h"
#include "runtime/shaders/program.h"

#include <inttypes.h>

/* Two processes: 46 interaction frames and eight fresh-open frames. Only the OS
 * chooser is replaced. Form, choice, drag, history and save input is SDL. */
#define TEXTURE_IMPORT_FRAMES 46
#define TEXTURE_REOPEN_FRAMES 8
#define TEXTURE_ASSET_SELECTOR                                                 \
    "[data-drag='texture'][data-drag-payload='content/materials/"              \
    "ImportedGrey.png']"
static bool texture_reopen;
static char texture_source[4096];
static char texture_original[4096];
static uint64_t texture_source_hash;
static uint64_t texture_before_pixels, texture_saved_material;
static float texture_drop_x, texture_drop_y;
static const char texture_material[] =
    "version 3\nname \"GreyRockUi\"\ndomain \"material\"\nid_counter 200\n"
    "(array)graphs {\n - {\n kind \"event\"\n name \"\"\n (array)nodes {\n"
    " - {\n id 1\n type \"Shader Surface Output\"\n inputs Unlit=#true\n }\n"
    " - {\n id 100\n type \"Shader Image Texture\"\n x -350\n y 0\n inputs {\n"
    " Image \"content/images/ExistingGrey.png\"\n }\n }\n }\n"
    " (array)edges {\n - from=\"100.Color\" to=\"1.Base Color\"\n }\n }\n}\n";

static bool texture_bytes(const char *path, unsigned char **bytes, size_t *length)
{
    *bytes = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    REQUIRE(file);
    REQUIRE(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    REQUIRE(size > 0 && size <= 8 * 1024 * 1024);
    rewind(file);
    unsigned char *data = malloc((size_t)size);
    REQUIRE(data);
    bool read = fread(data, 1, (size_t)size, file) == (size_t)size;
    REQUIRE(!fclose(file) && read);
    *bytes = data;
    *length = (size_t)size;
    return true;
}

static uint64_t texture_hash(const unsigned char *bytes, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static bool texture_write(const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fwrite(bytes, 1, length, file) == length;
    REQUIRE(!fclose(file) && written);
    return true;
}

static bool texture_small_image(const unsigned char *bytes, size_t length)
{
    shader_image_view view = { 0 };
    char error[256];
    REQUIRE(image_prepare_decode(bytes, length, &view, error, sizeof error));
    bool bounded = view.width > 0 && view.width <= 1024 && view.height > 0 &&
                   view.height <= 1024;
    free((void *)view.pixels);
    REQUIRE(bounded);
    return true;
}

static bool texture_value_is(studio *s, const char *expected);

static bool texture_metadata_is(image_import_semantic semantic)
{
    char content[4096], error[256];
    snprintf(content, sizeof content, "%s/content", project);
    image_import_texture texture = { 0 };
    bool read = image_import_texture_read(
        content,
        "content/materials/ImportedGrey.png",
        &texture,
        error,
        sizeof error
    );
    if (!read) { fprintf(stderr, "Texture metadata: %s\n", error); }
    REQUIRE(read);
    bool correct = texture.source && !strcmp(texture.source, "texture-source.png") &&
                   texture.options.max_side == 1024 &&
                   texture.options.semantic == semantic && texture.hash != 0;
    image_import_texture_free(&texture);
    REQUIRE(correct);
    return true;
}

static bool texture_setting_is(studio *s, const char *semantic)
{
    rml_element *field = studio_element(s, "#texture-asset-semantic");
    REQUIRE(field && rml_element_is_visible(field, true));
    char value[32];
    rml_element_value(field, value, sizeof value);
    REQUIRE(!strcmp(value, semantic));
    return true;
}

static bool texture_preview(studio *s, const char *semantic)
{
    bool source_valid = element_says(s, "#texture-asset-source", "texture-source.png");
    if (!source_valid) {
        fprintf(
            stderr,
            "Texture source mismatch frame=%d active=%d key=%s selected=%s "
            "node=%" PRIu64 " shown=%" PRIu64 " actor=%d shownactor=%d\n",
            frames,
            studio_texture_active(s),
            studio_texture_key(s) ? studio_texture_key(s) : "",
            s->assets_selected,
            studio_graph_node(s),
            s->shown_node,
            s->scene->selected,
            s->shown_actor
        );
        (void)texture_capture("failure");
    }
    REQUIRE(source_valid);
    rml_element *preview = studio_element(s, "#texture-asset-preview");
    char source[4096] = "";
    if (preview) {
        rml_element_attribute(preview, "src", source, sizeof source);
    }
    if (!preview || !rml_element_is_visible(preview, true)) {
        fprintf(
            stderr,
            "Texture preview missing at frame=%d active=%d key=%s source=%s\n",
            frames,
            studio_texture_active(s),
            studio_texture_key(s) ? studio_texture_key(s) : "",
            source
        );
        (void)texture_capture("failure");
    }
    REQUIRE(preview && rml_element_is_visible(preview, true));
    REQUIRE(source[0]);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int asset = reference_index_find_name(index, REFERENCE_ASSET_TEXTURE, "content/materials/ImportedGrey.png");
    REQUIRE(asset >= 0);
    bool loading = false;
    const char *shown = studio_thumb(s, asset, STUDIO_THUMB_LARGE, &loading);
    REQUIRE(shown && !loading && !strcmp(source, shown));
    REQUIRE(!strncmp(source, "kit:thumb:", strlen("kit:thumb:")));
    rml_vec2 dimensions = rml_element_box_size(preview, RML_BOX_BORDER);
    REQUIRE(dimensions.x > 0 && dimensions.y > 0);
    rml_element *resolution = studio_element(s, "#texture-asset-resolution");
    REQUIRE(resolution);
    char value[32];
    rml_element_value(resolution, value, sizeof value);
    REQUIRE(!strcmp(value, "1k"));
    return texture_setting_is(s, semantic);
}

static bool texture_drag_start(studio *s)
{
    rml_element *asset = studio_element(s, TEXTURE_ASSET_SELECTOR);
    REQUIRE(asset && rml_element_is_visible(asset, true));
    rml_vec2 at = rml_element_absolute_offset(asset, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(asset, RML_BOX_BORDER);
    REQUIRE(size.x > 0 && size.y > 0);
    float to_points = s->prefs.ui_scale / studio_pixel_ratio(s);
    kit_rect target = s->graph.surface.rect;
    REQUIRE(target.w > 0 && target.h > 0);
    texture_drop_x = (target.x + target.w * 0.75f) * s->prefs.ui_scale;
    texture_drop_y = (target.y + target.h * 0.7f) * s->prefs.ui_scale;
    return push_mouse(
        SDL_EVENT_MOUSE_BUTTON_DOWN,
        (at.x + size.x / 2) * to_points,
        (at.y + size.y / 2) * to_points,
        0,
        0
    );
}

static bool texture_pixels(const char *path, uint64_t *hash)
{
    unsigned char *bytes = NULL;
    size_t length = 0;
    REQUIRE(texture_bytes(path, &bytes, &length));
    shader_image_view image = { 0 };
    char error[256];
    bool decoded = image_prepare_decode(bytes, length, &image, error, sizeof error) !=
                   NULL;
    free(bytes);
    REQUIRE(decoded);
    *hash = texture_hash(image.pixels, (size_t)image.width * image.height * 4);
    free((void *)image.pixels);
    return true;
}

static bool texture_edit_source()
{
    char path[4096], error[256];
    snprintf(path, sizeof path, "%s/content/materials/ImportedGrey.png", project);
    REQUIRE(texture_pixels(path, &texture_before_pixels));
    unsigned char *bytes = NULL;
    size_t length = 0;
    REQUIRE(texture_bytes(texture_source, &bytes, &length));
    shader_image_view image = { 0 };
    bool decoded = image_prepare_decode(bytes, length, &image, error, sizeof error) !=
                   NULL;
    free(bytes);
    REQUIRE(decoded);
    /* An external image edit, performed only on the disposable fixture copy. */
    unsigned char *pixels = (unsigned char *)image.pixels;
    pixels[0] ^= 0xff;
    unsigned char *png = NULL;
    size_t size = 0;
    bool encoded = image_prepare_png_encode(&image, &png, &size, error, sizeof error);
    free(pixels);
    REQUIRE(encoded);
    bool written = texture_write(texture_source, png, size);
    free(png);
    REQUIRE(written);
    return true;
}

static bool texture_material_saved(bool unchanged)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/content/materials/GreyRockUi.kdl", project);
    char *saved = file_read_text(path);
    REQUIRE(saved);
    bool correct = strstr(saved, "content/materials/ImportedGrey.png") &&
                   !strstr(saved, project);
    uint64_t hash = texture_hash((const unsigned char *)saved, strlen(saved));
    free(saved);
    REQUIRE(correct);
    if (unchanged) {
        REQUIRE(hash == texture_saved_material);
    } else {
        texture_saved_material = hash;
    }
    return true;
}

static bool texture_image_nodes(studio *s, int count)
{
    graph *g = sg_active_graph(s);
    REQUIRE(g);
    int found = 0;
    for (int i = 0; i < acount(g->nodes); i++) {
        node *n = g->nodes[i];
        if (strcmp(n->desc->name, "Shader Image Texture")) { continue; }
        found++;
        if (n->id != 100) {
            const char *value = n->input_pins[SHADER_IMAGE_INPUT_IMAGE]
                                    .default_value.str;
            REQUIRE(value && !strcmp(value, "content/materials/ImportedGrey.png"));
            REQUIRE(
                n->input_pins[SHADER_IMAGE_INPUT_COLOR_SPACE].default_value.e == SHADER_IMAGE_LINEAR
            );
        }
    }
    REQUIRE(found == count);
    return true;
}

static bool texture_resident(studio *s)
{
    REQUIRE(texture_value_is(s, "content/materials/ImportedGrey.png"));
    int width = 0, height = 0;
    REQUIRE(render_image_info("content/materials/ImportedGrey.png", &width, &height));
    shader_image_source source = {
        .key = "content/materials/ImportedGrey.png"
    };
    shader_image_view actual = { 0 };
    REQUIRE(render_image_resolve(NULL, &source, &actual));
    REQUIRE(actual.width == width && actual.height == height && width > 0 && height > 0);
    char path[4096], error[256];
    snprintf(path, sizeof path, "%s/content/materials/ImportedGrey.png", project);
    unsigned char *bytes = NULL;
    size_t length = 0;
    REQUIRE(texture_bytes(path, &bytes, &length));
    shader_image_view expected = { 0 };
    bool decoded = image_prepare_decode(bytes, length, &expected, error, sizeof error) !=
                   NULL;
    free(bytes);
    REQUIRE(decoded);
    bool same = width == expected.width && height == expected.height &&
                texture_hash(actual.pixels, (size_t)width * height * 4) ==
                    texture_hash(expected.pixels, (size_t)width * height * 4);
    free((void *)expected.pixels);
    REQUIRE(same);
    printf(
        "Resident imported image verified: %dx%d rgba8_fnv1a64=%016" PRIx64 "\n",
        width,
        height,
        texture_hash(actual.pixels, (size_t)width * height * 4)
    );
    return true;
}

static bool texture_fixture()
{
    char path[4096];
    snprintf(path, sizeof path, "%s/content/images", project);
    REQUIRE(!mkdir(path, 0700) || errno == EEXIST);
    const char *source = getenv("CHIBI_STUDIO_TEXTURE_SOURCE");
    const char *second = getenv("CHIBI_STUDIO_TEXTURE_SECOND_SOURCE");
    if (source && source[0]) {
        REQUIRE(strlen(source) < sizeof texture_original && source[0] == '/');
        snprintf(texture_original, sizeof texture_original, "%s", source);
    } else {
        CF_Pixel pixels[4] = { { .r = 100, .g = 80, .b = 60, .a = 255 },
                               { .r = 140, .g = 110, .b = 80, .a = 255 },
                               { .r = 80, .g = 90, .b = 100, .a = 255 },
                               { .r = 180, .g = 160, .b = 120, .a = 255 } };
        CF_Image image = { 2, 2, pixels };
        void *png = NULL;
        int size = 0;
        REQUIRE(!cf_is_error(cf_image_save_png_to_memory(&image, &png, &size)));
        snprintf(texture_original, sizeof texture_original, "%s/texture-original.png", project);
        REQUIRE(texture_write(texture_original, png, (size_t)size));
        cf_free(png);
    }
    unsigned char *bytes = NULL;
    size_t length = 0;
    REQUIRE(texture_bytes(texture_original, &bytes, &length));
    REQUIRE(texture_small_image(bytes, length));
    texture_source_hash = texture_hash(bytes, length);
    /* Simulated external edits must never touch a caller's original image. */
    snprintf(texture_source, sizeof texture_source, "%s/texture-source.png", project);
    REQUIRE(texture_write(texture_source, bytes, length));
    free(bytes);
    snprintf(path, sizeof path, "%s/content/images/ExistingGrey.png", project);
    if (second && second[0]) {
        REQUIRE(texture_bytes(second, &bytes, &length));
        REQUIRE(texture_small_image(bytes, length));
        REQUIRE(texture_write(path, bytes, length));
        free(bytes);
    } else {
        CF_Pixel pixels[4] = { { .r = 220, .g = 220, .b = 220, .a = 255 },
                               { .r = 120, .g = 120, .b = 120, .a = 255 },
                               { .r = 160, .g = 160, .b = 160, .a = 255 },
                               { .r = 200, .g = 200, .b = 200, .a = 255 } };
        CF_Image image = { 2, 2, pixels };
        void *png = NULL;
        int size = 0;
        REQUIRE(!cf_is_error(cf_image_save_png_to_memory(&image, &png, &size)));
        REQUIRE(texture_write(path, png, (size_t)size));
        cf_free(png);
    }
    REQUIRE(write_content("materials", "GreyRockUi.kdl", texture_material));
    REQUIRE(write_content(
        "prototypes",
        "GreyRockUiMesh.kdl",
        "name \"GreyRockUiMesh\"\ncomponents {\n"
        "Geometry3D depth=20.0 elevation=10.0 height=80.0 width=80.0\n"
        "MeshSurface2D enabled=#true\nMaterial absorption=1.0 "
        "albedo=0xffffffff scene=0xffffffff\n"
        "Shader material=\"GreyRockUi\"\nTransform x=0.0 y=0.0\n}\n"
    ));
    REQUIRE(write_content(
        "scenes",
        "Start.kdl",
        "version 1\nname \"Start\"\n(array)entities {\n"
        "- name=\"GreyRock\" prototype=\"GreyRockUiMesh\"\n}\n"
    ));
    return true;
}

static void texture_choose_fixture(void *context, studio_image_import_completion complete, void *result)
{
    (void)context;
    complete(result, texture_source, NULL);
}

static bool texture_value_is(studio *s, const char *expected)
{
    node *image = sg_node_by_id(s, 100);
    REQUIRE(image && !strcmp(image->desc->name, "Shader Image Texture"));
    const char *value = image->input_pins[SHADER_IMAGE_INPUT_IMAGE]
                            .default_value.str;
    REQUIRE(value && !strcmp(value, expected));
    return true;
}

/* Release the chord modifier before the next pointer gesture, as the user
 * does; otherwise a subsequent Assets click toggles its selection. */
static bool texture_chord(SDL_Keycode key, SDL_Keymod mod)
{
    return press_chord(key, mod) &&
           push_key(mod & SDL_KMOD_GUI ? SDLK_LGUI : SDLK_LCTRL, SDL_KMOD_NONE, false);
}

static bool texture_import_frame(void *context)
{
    studio *s = studio_acceptance_studio(context);
    REQUIRE(s && s->ui);
    frames++;
    SDL_Keymod mod = s->mac ? SDL_KMOD_GUI : SDL_KMOD_CTRL;
    if (texture_reopen) {
        REQUIRE(frames <= TEXTURE_REOPEN_FRAMES);
        if (frames == 1) {
            REQUIRE(texture_metadata_is(IMAGE_IMPORT_SCALAR));
            REQUIRE(texture_image_nodes(s, 2));
            REQUIRE(texture_resident(s));
            studio_show_editor(s, STUDIO_EDITOR_ASSETS);
            studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
            s->assets_kind = REFERENCE_ASSET_TEXTURE;
            s->assets_filter[0] = '\0';
        }
        if (frames == 2) { return click(s, TEXTURE_ASSET_SELECTOR); }
        if (frames == 8) { REQUIRE(texture_preview(s, "scalar")); }
        return true;
    }
    REQUIRE(frames <= TEXTURE_IMPORT_FRAMES);
    switch (frames) {
        case 1:
            studio_image_import_set_chooser(s, texture_choose_fixture, NULL);
            studio_show_editor(s, STUDIO_EDITOR_ASSETS);
            s->assets_kind = REFERENCE_ASSET_TEXTURE;
            s->assets_filter[0] = '\0';
            return true;
        case 3: return click(s, "#texture-import-open");
        case 4: return click(s, "#texture-import-choose");
        case 6:
            REQUIRE(
                element_says(s, "#texture-import-choose", strrchr(texture_source, '/') + 1)
            );
            return click(s, "#texture-import-destination");
        case 7:
            REQUIRE(texture_chord(SDLK_A, mod));
            return type_text("content/materials/ImportedGrey.png");
        case 9: return click(s, "#texture-import-prepare");
        case 11:
            REQUIRE(texture_capture("review"));
            REQUIRE(element_says(s, "#texture-import-review", "Visual assets:"));
            REQUIRE(element_says(s, "#texture-import-review", "/ 64 MiB"));
            return click(s, "#texture-import-confirm");
        case 13:
            REQUIRE(texture_metadata_is(IMAGE_IMPORT_COLOR));
            REQUIRE(
                element_says(s, "#texture-import-status", "Imported content/materials/ImportedGrey.png")
            );
            REQUIRE(element_says(s, "#texture-import-done", "Done"));
            return click(s, "#texture-import-done");
        case 14:
            studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
            return click(s, TEXTURE_ASSET_SELECTOR);
        case 16:
            REQUIRE(element_says(s, "#texture-asset-source", "texture-source.png"));
            REQUIRE(texture_setting_is(s, "color"));
            return click(s, "#texture-asset-semantic");
        case 18: return press_key(SDLK_DOWN) && press_key(SDLK_RETURN);
        case 20:
            REQUIRE(texture_preview(s, "scalar"));
            REQUIRE(studio_texture_dirty(s));
            REQUIRE(texture_metadata_is(IMAGE_IMPORT_COLOR));
            return texture_chord(SDLK_Z, mod);
        case 22:
            REQUIRE(texture_setting_is(s, "color"));
            REQUIRE(!studio_texture_dirty(s));
            return texture_chord(SDLK_Z, mod | SDL_KMOD_SHIFT);
        case 24:
            REQUIRE(texture_setting_is(s, "scalar"));
            REQUIRE(studio_texture_dirty(s));
            return click(s, "#texture-asset-save");
        case 25:
            REQUIRE(texture_metadata_is(IMAGE_IMPORT_SCALAR));
            REQUIRE(!studio_texture_dirty(s));
            REQUIRE(!app_scene_authoring_open_behavior(s->scene, "GreyRockUi"));
            studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
            studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
            REQUIRE(texture_image_nodes(s, 1));
            return true;
        case 26: return texture_drag_start(s);
        case 27:
            return push_mouse(SDL_EVENT_MOUSE_MOTION, texture_drop_x, texture_drop_y, 0, 0);
        case 28:
            return push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, texture_drop_x, texture_drop_y, 0, 0);
        case 30:
            REQUIRE(texture_image_nodes(s, 2));
            s->focus_area = s->graph.area;
            return texture_chord(SDLK_Z, mod);
        case 32:
            REQUIRE(texture_image_nodes(s, 1));
            return texture_chord(SDLK_Z, mod | SDL_KMOD_SHIFT);
        case 33:
            REQUIRE(texture_image_nodes(s, 2));
            sg_select_only(sg_graph_editor(s), sg_node_by_id(s, 100));
            s->tab = STUDIO_TAB_NODE;
            s->focus_area = s->graph.area;
            return true;
        case 34:
            REQUIRE(texture_value_is(s, "content/images/ExistingGrey.png"));
            return click(s, ".graph-field [aria-label='Image'] button.field.select");
        case 36:
            return click(s, ".menu-item[data-label='materials/ImportedGrey.png']");
        case 38:
            REQUIRE(texture_value_is(s, "content/materials/ImportedGrey.png"));
            s->focus_area = s->graph.area;
            return texture_chord(SDLK_S, mod);
        case 40:
            REQUIRE(texture_material_saved(false));
            REQUIRE(texture_edit_source());
            return click(s, TEXTURE_ASSET_SELECTOR);
        case 42:
            REQUIRE(texture_preview(s, "scalar"));
            return click(s, "#texture-asset-reimport");
        case 46: {
            bool reimported = element_says(s, "#texture-asset-status", "Reimported content/materials/ImportedGrey.png");
            if (!reimported) { (void)texture_capture("failure"); }
            REQUIRE(reimported);
            REQUIRE(texture_metadata_is(IMAGE_IMPORT_SCALAR));
            REQUIRE(texture_material_saved(true));
            REQUIRE(texture_resident(s));
            char path[4096];
            snprintf(path, sizeof path, "%s/content/materials/ImportedGrey.png", project);
            uint64_t after, source;
            REQUIRE(texture_pixels(path, &after));
            REQUIRE(texture_pixels(texture_source, &source));
            REQUIRE(after != texture_before_pixels && after == source);
            unsigned char *bytes = NULL;
            size_t length = 0;
            REQUIRE(texture_bytes(texture_original, &bytes, &length));
            uint64_t hash = texture_hash(bytes, length);
            free(bytes);
            REQUIRE(hash == texture_source_hash);
            return true;
        }
        default: return true;
    }
}

TEST_CASE(texture_import_workflow)
{
    REQUIRE(texture_fixture());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "46",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "GreyRockUi" };
    REQUIRE(!studio_acceptance_host_main(sizeof args / sizeof *args, args));
    REQUIRE(callback_ok && frames == TEXTURE_IMPORT_FRAMES);
    const char *child[] = { executable,
                            "--texture-import-reopen",
                            "--project-root",
                            project,
                            NULL };
    SDL_Process *process = SDL_CreateProcess(child, false);
    REQUIRE(process);
    int status = -1;
    bool waited = SDL_WaitProcess(process, true, &status);
    SDL_DestroyProcess(process);
    REQUIRE(waited && status == 0);
    printf("\nStudio texture import accepted: frames=54\n");
    printf(
        "Texture import details: host_lifetimes=2 source_fnv1a64=%016" PRIx64 " import/assets/preview/settings/drag/picker/undo/redo/save/reimport/reopen/resident-pixels virtual-reference original-preserved\n",
        texture_source_hash
    );
    return true;
}

TEST_CASE(texture_import_reopen)
{
    texture_reopen = true;
    const char *capture = getenv("CHIBI_STUDIO_TEXTURE_OUTPUT");
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "8",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "GreyRockUi",
                     "--screenshot",     (char *)capture };
    int argc = sizeof args / sizeof *args;
    REQUIRE(!studio_acceptance_host_main(capture && capture[0] ? argc : argc - 2, args));
    REQUIRE(callback_ok && frames == TEXTURE_REOPEN_FRAMES);
    return true;
}
