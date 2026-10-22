#include "authoring/image_prepare.h"
#include "authoring/engine_preview.h"
#include "graph/authoring/graph_edit.h"
/* A model document roundtrip through Studio commands and retained controls.
 * Generic graph setup supplies a reusable component; interactions change it. */
#include "assets/new_asset.h"
#include "assets/scene_document.h"
#include "assets/system_document.h"
#include "authoring/behavior_loader.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor_internal.h"
#include "editor/workspace/input.h"
#include "editor/workspace/model_preview.h"
#include "editor/workspace/scene_internal.h"
#include "editor/workspace/viewport.h"
#include "graph/modeling/model_compile.h"
#include "render/screenshot.h"
#include "render/shader_catalog.h"
#include "render/shader_images.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_document.h"
#include "support/file_io.h"
#include "support/text_file.h"
#include "test/suite_internal.h"
#include "yyjson.h"

#include <SDL3/SDL_events.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAME_SECONDS (1.0 / 60.0)
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
#define PROJECT_VERSION_TEXT STRINGIFY(CHIBI_PROJECT_VERSION)
#define MANIFEST                                                               \
    "package asset\n"                                                          \
    "\n"                                                                       \
    "import data \"game:assets\"\n"                                            \
    "\n"                                                                       \
    "pub Build :: () -> byte[] {\n"                                            \
    "    return data.Encode(\n"                                                \
    "        data.Object([\n"                                                  \
    "            data.Field.Named(\"version\", "                               \
    "data.Value.Int(" PROJECT_VERSION_TEXT ")),\n"                             \
    "            data.Field.Named(\"name\", data.Value.Text(\"Fresh\")),\n"    \
    "            data.Field.Named(\"content\", "                               \
    "data.Value.Text(\"content\")),\n"                                         \
    "            data.Field.Named(\"start_scene\", "                           \
    "data.Value.Text(\"scenes/Main.chibi\"))\n"                                \
    "        ])\n"                                                             \
    "    )\n"                                                                  \
    "}\n"

typedef struct
{
    ecs_world *world;
    game_preview_world *live;
    engine *runtime;
    editor *graph;
    engine_viewport *viewport;
    app_scene_authoring *scene;
    studio *studio;
    double time;
    char root[64], content[96];
} model_fixture;

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void join(char *out, size_t size, const char *folder, const char *name)
{
    snprintf(out, size, "%s/%s", folder, name);
}

/* A project with one scene, Main, and Studio over it. */
static model_fixture fixture_new()
{
    model_fixture f = { .world = ecs_world_new() };
    snprintf(f.root, sizeof f.root, "/tmp/chibi-model-XXXXXX");
    assert(mkdtemp(f.root));
    char path[160], error[128];
    join(path, sizeof path, f.root, CHIBI_PROJECT_MANIFEST);
    write_text(path, MANIFEST);
    join(f.content, sizeof f.content, f.root, "content");
    join(path, sizeof path, f.content, "scenes");
    assert(mkdir(f.content, 0755) == 0 && mkdir(path, 0755) == 0);
    char *json = new_asset_text(NEW_ASSET_SCENE, "Main");
    char *yaml = asset_document_to_disk(json, error, sizeof error);
    join(path, sizeof path, f.content, "scenes/Main.chibi");
    write_text(path, yaml);
    free(yaml);
    free(json);
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition *definition = game_preview_definition_new("Editing", behavior_asset_new("Editing"));
    assert(definition && game_preview_add_definition(f.live, definition) == 0);
    f.graph = editor_new(behavior_asset_new("Editing"));
    f.runtime = authoring_engine_new(f.world, f.live->runtime);
    f.viewport = engine_viewport_new(640, 360);
    f.scene = app_scene_authoring_new(
        f.world,
        f.live,
        definition,
        editor_behavior_asset(f.graph),
        f.graph,
        NULL,
        NULL,
        f.runtime
    );
    assert(app_scene_authoring_set_content_root(f.scene, f.content) == 0);
    assert(app_scene_authoring_load_content(f.scene) == 0);
    assert(app_scene_authoring_load(f.scene, path) == 0);
    f.studio = studio_new(f.scene, f.viewport, f.root);
    return f;
}

static void reference_inspection_workflow();
static bool reveal(model_fixture *, const char *);
static rml_element *control_element(model_fixture *, const char *);

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *item;
    while (directory && (item = readdir(directory))) {
        if (item->d_name[0] == '.') {
            continue;
        }
        char child[512];
        join(child, sizeof child, path, item->d_name);
        struct stat info;
        assert(lstat(child, &info) == 0);
        if (S_ISDIR(info.st_mode)) {
            remove_tree(child);
        } else {
            assert(remove(child) == 0);
        }
    }
    if (directory) {
        closedir(directory);
    }
    assert(rmdir(path) == 0);
}

static void fixture_free(model_fixture *f)
{
    studio_free(f->studio);
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
    engine_viewport_free(f->viewport);
    render_shader_catalog_clear();
    remove_tree(f->root);
}

static void frames(model_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        f->time += FRAME_SECONDS;
        cf_app_update(NULL);
        app_input_frame();
        editor_tick(f->graph, f->time);
        assert(app_scene_authoring_frame(f->scene, f->time) == FLOW_OK);
        studio_frame(f->studio, FRAME_SECONDS);
        cf_app_draw_onto_screen(true);
        /* The user's preferences stay as they are. */
        f->studio->prefs_changed_at = 0;
    }
}

static void press(SDL_Scancode key)
{
    for (int down = 1; down >= 0; down--) {
        SDL_Event event = { .type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP };
        event.key.windowID = SDL_GetWindowID(cf_app_get_window());
        event.key.scancode = key;
        event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, true);
        event.key.down = down;
        assert(SDL_PushEvent(&event));
    }
}

#define PROPS "app/workspace/properties/area/body/panel/graph-native/"

static int query_documents(rml_context *context, const char *selector, rml_element **elements, int capacity)
{
    int count = 0;
    for (int document = 0;
         document < rml_context_document_count(context) && count < capacity;
         document++) {
        count += rml_element_query_selector_all(
            rml_context_document(context, document),
            selector,
            elements + count,
            capacity - count
        );
    }
    return count;
}

static bool click(model_fixture *f, const char *path)
{
    if (!reveal(f, path)) {
        return false;
    }
    char actual_path[512];
    assert(
        kit_element_path(f->studio->kit, control_element(f, path), actual_path, sizeof actual_path)
    );
    kit_rect rect;
    if (!kit_find(f->studio->kit, actual_path, &rect)) {
        printf(
            "  missing model workflow control: %s (tab=%d, max=%d, "
            "document=%d, node=%llu)\n",
            path,
            f->studio->tab,
            f->studio->maximized,
            app_scene_authoring_document(f->scene).kind,
            (unsigned long long)studio_graph_node(f->studio)
        );
        rml_element *elements[64];
        int count = query_documents(rml_backend_context(f->studio->backend), ".panel,.prop", elements, 64);
        for (int i = 0; i < count && i < 64; i++) {
            char actual[256];
            if (kit_element_path(f->studio->kit, elements[i], actual, sizeof actual)) {
                printf("  actual model workflow path: %s\n", actual);
            }
        }
        return false;
    }
    for (int down = 1; down >= 0; down--) {
        SDL_Event event = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                         : SDL_EVENT_MOUSE_BUTTON_UP };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = (rect.x + rect.w * .5f) * f->studio->prefs.ui_scale;
        event.button.y = (rect.y + rect.h * .5f) * f->studio->prefs.ui_scale;
        assert(SDL_PushEvent(&event));
    }
    frames(f, 2);
    return true;
}

static bool text(model_fixture *f, const char *path, const char *value)
{
    if (!click(f, path)) {
        return false;
    }
    rml_element *field = rml_context_focus_element(rml_backend_context(f->studio->backend));
    if (!field || strcmp(rml_element_tag_name(field), "input")) {
        return false;
    }
    rml_element_select(field);
    rml_backend_process_input(
        f->studio->backend,
        &(rml_input){ .type = RML_INPUT_TEXT_INPUT, .text = value, .text_size = strlen(value) }
    );
    if (strstr(path, "source-edit")) {
        press(SDL_SCANCODE_RETURN);
    } else {
        rml_element_blur(field);
    }
    frames(f, 3);
    return true;
}

static rml_element *graph_input_field(model_fixture *f, int pin)
{
    char id[64];
    snprintf(id, sizeof id, "n%llu.i%d", (unsigned long long)studio_graph_node(f->studio), pin);
    rml_context *context = rml_backend_context(f->studio->backend);
    for (int document = 0; document < rml_context_document_count(context); document++) {
        rml_element *field = rml_element_by_id(rml_context_document(context, document), id);
        if (field) {
            return field;
        }
    }
    printf("  missing authored graph input: %s\n", id);
    return NULL;
}

static rml_element *graph_input_row(model_fixture *f, int pin)
{
    char id[64], selector[96];
    snprintf(id, sizeof id, "n%llu.i%d", (unsigned long long)studio_graph_node(f->studio), pin);
    snprintf(selector, sizeof selector, "label[for=\"%s\"]", id);
    rml_context *context = rml_backend_context(f->studio->backend);
    for (int document = 0; document < rml_context_document_count(context); document++) {
        rml_element *rows[128];
        int count = rml_element_query_selector_all(
            rml_context_document(context, document),
            ".graph-row",
            rows,
            128
        );
        for (int i = 0; i < count && i < 128; i++) {
            if (rml_element_query_selector(rows[i], selector)) {
                return rows[i];
            }
        }
    }
    printf("  missing authored graph input row: %s\n", id);
    return NULL;
}

static bool graph_input_choose_asset(model_fixture *f, int pin, const char *label)
{
    rml_context *context = rml_backend_context(f->studio->backend);
    rml_element *row = graph_input_row(f, pin);
    rml_element *field = row ? rml_element_query_selector(row, ".asset-field button")
                             : NULL;
    if (!field) {
        printf("  missing typed asset picker for graph input %d\n", pin);
        return false;
    }
    rml_element_scroll_into_view(field, RML_SCROLL_CENTER, RML_SCROLL_NEAREST);
    frames(f, 2);
    rml_element_click(field);
    frames(f, 2);
    rml_element *items[128];
    int count = query_documents(context, ".menu-item", items, 128);
    for (int i = 0; i < count && i < 128; i++) {
        rml_element *name = rml_element_query_selector(items[i], ".label");
        char text[128];
        if (!name || !rml_element_is_visible(items[i], true)) {
            continue;
        }
        rml_element_inner_rml(name, text, sizeof text);
        if (!strcmp(text, label)) {
            rml_element_click(items[i]);
            frames(f, 3);
            return true;
        }
    }
    return false;
}

static bool graph_input_choose(model_fixture *f, int pin, const char *query)
{
    rml_element *field = graph_input_field(f, pin);
    if (!field) {
        return false;
    }
    rml_element_scroll_into_view(field, RML_SCROLL_CENTER, RML_SCROLL_NEAREST);
    frames(f, 2);
    if (!rml_element_focus(field, true)) {
        return false;
    }
    rml_element_select(field);
    rml_backend_process_input(
        f->studio->backend,
        &(rml_input){ .type = RML_INPUT_TEXT_INPUT, .text = query, .text_size = strlen(query) }
    );
    frames(f, 2);
    press(SDL_SCANCODE_TAB);
    frames(f, 3);
    return true;
}

static char *create(model_fixture *f, studio_command command)
{
    studio_command_run(f->studio, command);
    frames(f, 2);
    char *name = strdup(f->studio->new_asset.name);
    press(SDL_SCANCODE_RETURN);
    frames(f, 3);
    bool opened = !strcmp(editor_behavior_asset(f->graph)->name, name) &&
                  editor_behavior_asset(f->graph)->domain ==
                      (command == STUDIO_COMMAND_NEW_MODEL ? BEHAVIOR_DOMAIN_MODEL
                                                           : BEHAVIOR_DOMAIN_MATERIAL);
    test_check(
        opened && !kit_popup_is_open(f->studio->kit, "new-asset-name"),
        "Studio creates a named model/material document through its New command"
    );
    assert(opened);
    return name;
}

static void clear_starter(graph *g)
{
    for (int i = acount(g->nodes) - 1; i >= 0; i--) {
        if (g->nodes[i]->desc == &NODE_ModelSphere) {
            assert(graph_remove_node(g, g->nodes[i]) == 0);
        }
    }
}

static node *output(graph *g)
{
    for (int i = 0; i < acount(g->nodes); i++) {
        if (g->nodes[i]->desc == &NODE_ModelOutput) {
            return g->nodes[i];
        }
    }
    assert(false);
    return NULL;
}

static void select_node(model_fixture *f, uint64_t id)
{
    sg_select_only(f->graph, graph_node_by_id(f->graph->active_graph, id));
    f->studio->maximized = f->studio->focus_area = STUDIO_AREA_PROPERTIES;
    f->studio->tab = STUDIO_TAB_NODE;
    frames(f, 3);
}

static void save(model_fixture *f)
{
    studio_command_run(f->studio, STUDIO_COMMAND_SAVE);
    frames(f, 3);
}

void test_studio_models()
{
    model_fixture f = fixture_new();
    frames(&f, 3);
    char *material = create(&f, STUDIO_COMMAND_NEW_MATERIAL);
    char *alternate = create(&f, STUDIO_COMMAND_NEW_MATERIAL);
    char *child = create(&f, STUDIO_COMMAND_NEW_MODEL);
    graph *g = f.graph->active_graph;
    clear_starter(g);
    node *radius = editor_spawn(f.graph, &NODE_ModelParameter, 0, 0);
    node *sphere = editor_spawn(f.graph, &NODE_ModelSphere, 0, 0);
    node *slot = editor_spawn(f.graph, &NODE_ModelMaterialSlot, 0, 0);
    node *part = editor_spawn(f.graph, &NODE_ModelPart, 0, 0);
    assert(radius && sphere && slot && part);
    uint64_t radius_id = radius->id, slot_id = slot->id, part_id = part->id;
    graph_set_node_reference(g, radius, 0, "Radius");
    graph_set_node_reference(g, slot, 0, "Shell");
    graph_set_node_reference_asset(g, slot, material);
    graph_set_node_reference(g, part, 0, "Unlabelled");
    assert(graph_connect_data(g, radius, 0, sphere, 0) == 0);
    assert(graph_connect_data(g, sphere, 0, part, 0) == 0);
    assert(graph_connect_data(g, slot, 0, part, 1) == 0);
    assert(graph_connect_data(g, part, 0, output(g), 0) == 0);
    select_node(&f, part_id);
    bool named = text(&f, PROPS "model/@body/name/@control/value", "Engine shell");
    part = graph_node_by_id(f.graph->active_graph, part_id);
    test_check(
        named && part->reference_name && !strcmp(part->reference_name, "Engine shell"),
        "a named model part is edited through its Properties text control"
    );
    save(&f);
    char *assembly = create(&f, STUDIO_COMMAND_NEW_MODEL);
    g = f.graph->active_graph;
    clear_starter(g);
    node *instance = editor_spawn(f.graph, &NODE_ModelInstance, 0, 0);
    assert(instance);
    uint64_t instance_id = instance->id;
    graph_set_node_reference_asset(g, instance, child);
    assert(graph_connect_data(g, instance, 0, output(g), 0) == 0);
    select_node(&f, instance_id);
    char path[256];
    snprintf(
        path,
        sizeof path,
        PROPS "model/@body/parameter-%llu/@control/override",
        (unsigned long long)radius_id
    );
    bool override = click(&f, path);
    instance = graph_node_by_id(f.graph->active_graph, instance_id);
    bool declared = instance->input_count == 1 &&
                    instance->input_pins[0].declaration_id == radius_id;
    bool edited = declared && graph_input_choose(&f, 0, "2");
    instance = graph_node_by_id(f.graph->active_graph, instance_id);
    bool doubled = declared && instance->input_pins[0].default_value.f == 2;
    studio_history(f.studio, false);
    frames(&f, 3);
    instance = graph_node_by_id(f.graph->active_graph, instance_id);
    bool undone = instance && instance->input_count == 1 &&
                  instance->input_pins[0].default_value.f == 1;
    studio_history(f.studio, true);
    frames(&f, 3);
    test_check(override && edited && doubled && undone, "a reused component dimension override is edited and undone through Studio history");
    select_node(&f, instance_id);
    snprintf(
        path,
        sizeof path,
        PROPS "model/@body/parameter-%llu/@control/override",
        (unsigned long long)slot_id
    );
    bool material_override = click(&f, path);
    bool picked = material_override && graph_input_choose_asset(&f, 1, alternate);
    frames(&f, 3);
    instance = graph_node_by_id(f.graph->active_graph, instance_id);
    bool assigned = instance && instance->input_count == 2 &&
                    instance->input_pins[1].declaration_id == slot_id &&
                    instance->input_pins[1].default_value.str &&
                    !strcmp(instance->input_pins[1].default_value.str, alternate);
    if (!(material_override && picked && assigned)) {
        printf(
            "  material workflow: override=%d picked=%d assigned=%d, "
            "expected=%s actual=%s\n",
            material_override,
            picked,
            assigned,
            alternate,
            instance && instance->input_count > 1 &&
                    instance->input_pins[1].default_value.str
                ? instance->input_pins[1].default_value.str
                : "<missing>"
        );
    }
    test_check(material_override && picked && assigned, "a sparse reused material override uses the typed material asset picker");
    save(&f);
    char file[256];
    snprintf(file, sizeof file, "%s/models/%s.chibi", f.content, assembly);
    scene_tabs_close(f.scene, APP_EDITOR_DOCUMENT_BEHAVIOR, assembly);
    assert(app_scene_authoring_open_behavior(f.scene, assembly) == 0);
    frames(&f, 3);
    instance = graph_node_by_id(f.graph->active_graph, instance_id);
    bool reloaded = instance && instance->input_count == 2 &&
                    instance->input_pins[0].default_value.f == 2 &&
                    !strcmp(instance->input_pins[1].default_value.str, alternate);
    char *saved = text_file_read(file);
    test_check(reloaded && saved != NULL, "saving, closing, and reopening retains declaration identities and material assignments");
    free(saved);
    select_node(&f, instance_id);
    bool preview_control = click(&f, PROPS "model-preview/@body/show/preview");
    bool preview = scene_model_preview_name(f.scene) &&
                   !strcmp(scene_model_preview_name(f.scene), assembly);
    if (preview) {
        frames(&f, 3);
        ecs_world *world = game_prototype_preview_world(f.scene->prototype_preview);
        ecs_entity entity = game_prototype_preview_entity(f.scene->prototype_preview);
        float yaw = GAME_GameVisualTransform_get_const(world, entity)->yaw;
        kit_rect surface = f.studio->surface.rect;
        float x = (surface.x + surface.w * .4f) * f.studio->prefs.ui_scale;
        float y = (surface.y + surface.h * .4f) * f.studio->prefs.ui_scale;
        SDL_Event event = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        event.button.x = x;
        event.button.y = y;
        assert(SDL_PushEvent(&event));
        frames(&f, 2);
        event = (SDL_Event){ .type = SDL_EVENT_MOUSE_MOTION };
        event.motion.windowID = SDL_GetWindowID(cf_app_get_window());
        event.motion.state = SDL_BUTTON_LMASK;
        event.motion.x = x + 60;
        event.motion.y = y + 20;
        event.motion.xrel = 60;
        event.motion.yrel = 20;
        assert(SDL_PushEvent(&event));
        frames(&f, 2);
        event = (SDL_Event){ .type = SDL_EVENT_MOUSE_BUTTON_UP };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x + 60;
        event.button.y = y + 20;
        assert(SDL_PushEvent(&event));
        frames(&f, 2);
        test_check(
            GAME_GameVisualTransform_get_const(world, entity)->yaw != yaw,
            "dragging the real Studio surface orbits the model preview"
        );
        yaw = GAME_GameVisualTransform_get_const(world, entity)->yaw;
        event = (SDL_Event){ .type = SDL_EVENT_MOUSE_BUTTON_DOWN };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        event.button.x = x;
        event.button.y = y;
        assert(SDL_PushEvent(&event));
        event = (SDL_Event){ .type = SDL_EVENT_MOUSE_MOTION };
        event.motion.windowID = SDL_GetWindowID(cf_app_get_window());
        event.motion.state = SDL_BUTTON_LMASK;
        event.motion.x = x + 60;
        event.motion.y = y + 20;
        assert(SDL_PushEvent(&event));
        event = (SDL_Event){ .type = SDL_EVENT_MOUSE_BUTTON_UP };
        event.button.windowID = SDL_GetWindowID(cf_app_get_window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x + 60;
        event.button.y = y + 20;
        assert(SDL_PushEvent(&event));
        frames(&f, 2);
        test_check(GAME_GameVisualTransform_get_const(world, entity)->yaw != yaw, "a complete drag between render frames still orbits the model preview");
        for (int direction = 0; direction < 2; direction++) {
            yaw = GAME_GameVisualTransform_get_const(world, entity)->yaw;
            float outside = (surface.x + surface.w + 8) * f.studio->prefs.ui_scale;
            float start_x = direction ? outside : x;
            float end_x = direction ? x : outside;
            event = (SDL_Event){ .type = SDL_EVENT_MOUSE_BUTTON_DOWN };
            event.button.windowID = SDL_GetWindowID(cf_app_get_window());
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = true;
            event.button.x = start_x;
            event.button.y = y;
            assert(SDL_PushEvent(&event));
            event = (SDL_Event){ .type = SDL_EVENT_MOUSE_MOTION };
            event.motion.windowID = SDL_GetWindowID(cf_app_get_window());
            event.motion.state = SDL_BUTTON_LMASK;
            event.motion.x = end_x;
            event.motion.y = y;
            assert(SDL_PushEvent(&event));
            event = (SDL_Event){ .type = SDL_EVENT_MOUSE_BUTTON_UP };
            event.button.windowID = SDL_GetWindowID(cf_app_get_window());
            event.button.button = SDL_BUTTON_LEFT;
            event.button.x = end_x;
            event.button.y = y;
            assert(SDL_PushEvent(&event));
            frames(&f, 2);
            bool changed = GAME_GameVisualTransform_get_const(world, entity)->yaw != yaw;
            test_check(changed == !direction, direction ? "a coalesced drag starting outside cannot orbit the model" : "a coalesced drag leaving the surface retains model capture");
        }
        model_parameter parameter;
        float values[4];
        if (scene_model_preview_parameter(f.scene, 0, &parameter, values)) {
            values[0] = 3;
            test_check(
                scene_model_preview_set_parameter(f.scene, 0, values),
                "a disposable preview accepts checked model parameter edits"
            );
        }
        scene_model_preview_reset_parameters(f.scene);
        scene_model_preview_frame(f.scene);
        frames(&f, 3);
    }
    test_check(preview_control && preview && scene_model_preview_name(f.scene), "a saved reusable model starts and resets through the real scene-backed preview");
    app_scene_authoring_stop_prototype_preview(f.scene);
    free(assembly);
    free(child);
    free(alternate);
    free(material);
    fixture_free(&f);
    reference_inspection_workflow();
}

static rml_element *control_element(model_fixture *f, const char *path)
{
    rml_element *elements[16384];
    int count = query_documents(rml_backend_context(f->studio->backend), "*", elements, 16384);
    for (int i = 0; i < count && i < 16384; i++) {
        char actual[512];
        if (kit_element_path(f->studio->kit, elements[i], actual, sizeof actual) &&
            !strcmp(actual, path)) {
            return elements[i];
        }
    }
    return NULL;
}

static bool reveal(model_fixture *f, const char *path)
{
    rml_element *element = control_element(f, path);
    if (!element) {
        printf("  missing inspection control: %s\n", path);
        return false;
    }
    rml_element_scroll_into_view(element, RML_SCROLL_CENTER, RML_SCROLL_NEAREST);
    frames(f, 3);
    return true;
}

static bool choose(model_fixture *f, const char *path, const char *label)
{
    if (!reveal(f, path)) {
        return false;
    }
    rml_element *field = control_element(f, path);
    rml_element *segments[16];
    int count = rml_element_query_selector_all(field, ".seg", segments, 16);
    for (int i = 0; i < count && i < 16; i++) {
        char text[128];
        rml_element_inner_rml(segments[i], text, sizeof text);
        if ((!strcmp(text, label) || (!strncmp(text, label, strlen(label)) &&
                                      text[strlen(label)] == ' '))) {
            rml_element_click(segments[i]);
            frames(f, 3);
            return true;
        }
    }
    if (!click(f, path)) {
        return false;
    }
    rml_element *items[64];
    count = query_documents(rml_backend_context(f->studio->backend), ".dropdown-results .menu-item", items, 64);
    for (int i = 0; i < count && i < 64; i++) {
        rml_element *name = rml_element_query_selector(items[i], ".label");
        char text[128];
        if (!name || !rml_element_is_visible(items[i], true)) {
            continue;
        }
        rml_element_inner_rml(name, text, sizeof text);
        if ((!strcmp(text, label) || (!strncmp(text, label, strlen(label)) &&
                                      text[strlen(label)] == ' '))) {
            rml_element_click(items[i]);
            frames(f, 3);
            return true;
        }
    }
    return false;
}

static void capture(model_fixture *f, const char *name)
{
    const char *directory = getenv("CHIBI_MODEL_CAPTURE_DIR");
    if (!directory || !directory[0]) {
        return;
    }
    frames(f, 6);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.png", directory, name);
    assert(app_save_screenshot(path) == 0);
}

static void fixture_image(model_fixture *f)
{
    char directory[160], destination[256];
    join(directory, sizeof directory, f->content, "textures");
    assert(!mkdir(directory, 0755));
    join(destination, sizeof destination, directory, "EnergyCells.png");
    FILE *source = fopen("test/fixtures/content/textures/EnergyCells.png", "rb");
    FILE *output = fopen(destination, "wb");
    assert(source && output);
    unsigned char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, source))) {
        assert(fwrite(buffer, 1, count, output) == count);
    }
    assert(!ferror(source));
    assert(!fclose(source) && !fclose(output));
    char error[256];
    assert(render_image_mount_content(f->content, error, sizeof error));
    app_scene_authoring_refresh_content(f->scene);
    frames(f, 3);
}

static void reference_inspection_workflow()
{
    model_fixture f = fixture_new();
    frames(&f, 3);
    fixture_image(&f);
    char *name = create(&f, STUDIO_COMMAND_NEW_MODEL);
    graph *g = f.graph->active_graph;
    clear_starter(g);
    node *sphere = editor_spawn(f.graph, &NODE_ModelSphere, 0, 0);
    node *left = editor_spawn(f.graph, &NODE_ModelPart, 0, 0);
    node *translate = editor_spawn(f.graph, &NODE_ModelTranslate, 0, 0);
    node *right = editor_spawn(f.graph, &NODE_ModelPart, 0, 0);
    node *combine = editor_spawn(f.graph, &NODE_ModelUnion, 0, 0);
    uint64_t left_id = left->id;
    assert(!graph_set_node_reference(g, left, 0, "Left"));
    assert(!graph_set_node_reference(g, right, 0, "Right"));
    float position[3] = { 3, 0, 0 };
    pin_value offset = { .type = PIN_STRUCT, .type_name = "ShaderVec3", .ptr = position };
    assert(!graph_set_pin_default(g, translate, false, 1, &offset));
    assert(!graph_connect_data(g, sphere, 0, left, 0));
    assert(!graph_connect_data(g, sphere, 0, translate, 0));
    assert(!graph_connect_data(g, translate, 0, right, 0));
    assert(!graph_connect_data(g, left, 0, combine, 0));
    assert(!graph_connect_data(g, right, 0, combine, 1));
    assert(!graph_connect_data(g, combine, 0, output(g), 0));
    select_node(&f, left_id);
    save(&f);
    bool selected = choose(&f, PROPS "model-preview/@body/reference-image/@control/value", "textures/EnergyCells.png");
    const behavior_asset *asset = editor_behavior_asset(f.graph);
    test_check(
        selected && asset->reference.image &&
            !strcmp(asset->reference.image, "textures/EnergyCells.png") &&
            asset->reference.locked,
        "the reference image picker authors a locked content-relative image"
    );
    bool locked = reveal(&f, PROPS "model-preview/@body/reference-2/@control/value") &&
                  !text(&f, PROPS "model-preview/@body/reference-2/@control/value", "7");
    test_check(locked && editor_behavior_asset(f.graph)->reference.span == 2, "locked reference alignment refuses numeric edits");
    bool unlocked = reveal(&f, PROPS "model-preview/@body/reference-locked") &&
                    click(&f, PROPS "model-preview/@body/reference-locked");
    bool resized = reveal(&f, PROPS "model-preview/@body/reference-2/@control/value") &&
                   text(&f, PROPS "model-preview/@body/reference-2/@control/value", "7");
    bool changed = editor_behavior_asset(f.graph)->reference.span == 7;
    studio_history(f.studio, false);
    frames(&f, 3);
    bool undone = editor_behavior_asset(f.graph)->reference.span == 2;
    studio_history(f.studio, true);
    frames(&f, 3);
    bool redone = editor_behavior_asset(f.graph)->reference.span == 7;
    test_check(
        unlocked && resized && changed && undone && redone,
        "reference alignment edits form one normal Undo and Redo gesture"
    );
    save(&f);
    scene_tabs_close(f.scene, APP_EDITOR_DOCUMENT_BEHAVIOR, name);
    assert(!app_scene_authoring_open_behavior(f.scene, name));
    select_node(&f, left_id);
    asset = editor_behavior_asset(f.graph);
    test_check(
        asset->reference.image && asset->reference.span == 7 &&
            !asset->reference.locked,
        "saved reference image alignment survives document close and reload"
    );
    f.studio->maximized = -1;
    f.studio->ui_schedule.wake = true;
    frames(&f, 6);
    save(&f);
    bool started = reveal(&f, PROPS "model-preview/@body/show/preview") &&
                   click(&f, PROPS "model-preview/@body/show/preview");
    test_check(started && scene_model_preview_name(f.scene), "inspection starts through the actual Preview Model button");
    f.studio->maximized = -1;
    f.studio->tab = STUDIO_TAB_NODE;
    f.studio->ui_schedule.wake = true;
    frames(&f, 3);
    bool front = choose(&f, PROPS "model-preview/@body/view/@control/value", "Front");
    engine_viewport_view view;
    engine_viewport_get_view(f.viewport, &view);
    test_check(
        front && scene_model_preview_view(f.scene) == MODEL_VIEW_FRONT &&
            view.focal_distance == 0,
        "Front view chooses the actual orthographic game camera"
    );
    bool side = choose(&f, PROPS "model-preview/@body/view/@control/value", "Side");
    bool top = choose(&f, PROPS "model-preview/@body/view/@control/value", "Top");
    test_check(
        side && top && scene_model_preview_view(f.scene) == MODEL_VIEW_TOP,
        "Side and Top views use the retained Studio choice control"
    );
    choose(&f, PROPS "model-preview/@body/view/@control/value", "Front");
    bool overlay = reveal(&f, PROPS "model-preview/@body/reference-visible") &&
                   click(&f, PROPS "model-preview/@body/reference-visible");
    float opacity;
    test_check(
        overlay && scene_model_preview_reference(f.scene, &opacity) && opacity == .5f,
        "Show reference overlay uses a visible default opacity"
    );
    f.studio->maximized = -1;
    frames(&f, 3);
    reveal(&f, PROPS "model-preview/@body/view/@control/value");
    capture(&f, "reference-front");
    f.studio->maximized = -1;
    frames(&f, 3);
    bool isolated = choose(&f, PROPS "model-preview/@body/isolation/@control/value", "Left");
    float dimensions[3];
    bool bounds = scene_model_preview_dimensions(f.scene, dimensions);
    test_check(
        isolated && scene_model_preview_inspection(f.scene).part >= 0 &&
            bounds && dimensions[0] == 2,
        "the actual part picker frames the selected subtree rather than the "
        "full union"
    );
    bool section = reveal(&f, PROPS "model-preview/@body/section") &&
                   click(&f, PROPS "model-preview/@body/section");
    bool axis = choose(&f, PROPS "model-preview/@body/section-axis/@control/value", "X");
    bool plane = reveal(&f, PROPS "model-preview/@body/section-offset/@control/value") &&
                 text(&f, PROPS "model-preview/@body/section-offset/@control/value", "0.25");
    model_preview_inspection inspection = scene_model_preview_inspection(f.scene);
    test_check(
        section && axis && plane && inspection.section &&
            inspection.axis == 0 && inspection.offset == .25f,
        "section checkbox, axis choice and numeric plane position control the "
        "shared renderer"
    );
    f.studio->maximized = -1;
    frames(&f, 3);
    reveal(&f, PROPS "model-preview/@body/section-offset/@control/value");
    capture(&f, "isolated-section");
    f.studio->maximized = -1;
    frames(&f, 3);
    test_check(
        control_element(&f, PROPS "resources/@body/budget-0/@control/value") != NULL &&
            control_element(&f, PROPS "resources/@body/part-help") != NULL,
        "compiler budgets and overlapping part costs appear in the real "
        "resource panel"
    );
    reveal(&f, PROPS "resources/@body/budget-0/@control/value");
    capture(&f, "compiler-resources");
    char destination[256];
    snprintf(destination, sizeof destination, "%s/baked-model", f.root);
    bool path = reveal(&f, PROPS "model-preview/@body/bake-directory/@control/value") &&
                text(&f, PROPS "model-preview/@body/bake-directory/@control/value", destination);
    bool coarse = reveal(&f, PROPS "model-preview/@body/bake-cell/@control/value") &&
                  text(&f, PROPS "model-preview/@body/bake-cell/@control/value", "0.5");
    bool limits = click(&f, PROPS "model-preview/@body/bake-limits") &&
                  text(&f, PROPS "model-preview/@body/bake-limits/@body/max-triangles/@control/value", "150000") &&
                  text(&f, PROPS "model-preview/@body/bake-limits/@body/max-samples/@control/value", "3000000") &&
                  text(&f, PROPS "model-preview/@body/bake-limits/@body/max-work/@control/value", "3000000000");
    bool baked = reveal(&f, PROPS "model-preview/@body/bake") &&
                 click(&f, PROPS "model-preview/@body/bake");
    const char *files[] = { "model.mesh.json", "model.obj", "model.mtl", "provenance.json" };
    bool complete = true;
    for (int i = 0; i < 4; i++) {
        char file[512];
        struct stat info;
        join(file, sizeof file, destination, files[i]);
        complete &= !stat(file, &info) && info.st_size > 0;
    }
    test_check(path && coarse && baked && complete, "the real Bake Mesh Bundle controls produce all four native export artifacts");
    char provenance[512];
    join(provenance, sizeof provenance, destination, "provenance.json");
    char *metadata = complete ? file_read_text(provenance) : NULL;
    yyjson_doc *doc = metadata ? yyjson_read(metadata, strlen(metadata), 0) : NULL;
    yyjson_val *options = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "options")
                              : NULL;
    test_check(
        limits && options &&
            yyjson_get_int(yyjson_obj_get(options, "max_triangles")) == 150000 &&
            yyjson_get_uint(yyjson_obj_get(options, "max_samples")) == 3000000 &&
            yyjson_get_uint(yyjson_obj_get(options, "max_work")) == UINT64_C(3000000000),
        "the real Bake Limits controls reach saved bundle provenance without "
        "narrowing wide work counts"
    );
    yyjson_doc_free(doc);
    free(metadata);
    capture(&f, "mesh-bake");
    app_scene_authoring_stop_prototype_preview(f.scene);
    free(name);
    fixture_free(&f);
}
