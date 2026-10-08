#include "assets/ui_document.h"
#include "authoring/engine_preview.h"
/* Assets ▸ New and Settings ▸ Project over a project on disk: every kind New
 * makes is written as its model saves it, joins the asset index at once and
 * opens as Assets opens it; the Project rows rewrite project.kdl. */
#include "assets/new_asset.h"
#include "assets/scene_document.h"
#include "authoring/behavior_loader.h"
#include "authoring/preview_source.h"
#include "authoring/source_format.h"
#include "authoring/source_presentation.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/input.h"
#include "editor/workspace/scene_internal.h"
#include "editor/workspace/viewport.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/text_file.h"
#include "test/suite_internal.h"
#include "ui/popover.h"

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
    "version " PROJECT_VERSION_TEXT "\n"                                       \
    "name \"Fresh\"\n"                                                         \
    "content \"content\"\n"                                                    \
    "start_scene \"scenes/Main.kdl\"\n"

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
} new_fixture;

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
static new_fixture fixture_new()
{
    new_fixture f = { .world = ecs_world_new() };
    snprintf(f.root, sizeof f.root, "/tmp/chibi-new-XXXXXX");
    assert(mkdtemp(f.root));
    char path[160], error[128];
    join(path, sizeof path, f.root, CHIBI_PROJECT_MANIFEST);
    write_text(path, MANIFEST);
    join(f.content, sizeof f.content, f.root, "content");
    join(path, sizeof path, f.content, "scenes");
    assert(mkdir(f.content, 0755) == 0 && mkdir(path, 0755) == 0);
    char *json = new_asset_text(NEW_ASSET_SCENE, "Main");
    char *yaml = asset_text_to_disk(json, error, sizeof error);
    join(path, sizeof path, f.content, "scenes/Main.kdl");
    write_text(path, yaml);
    free(yaml);
    free(json);
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        assert(ecs_world_register_component(f.world, GAME_REGISTRY_COMPONENTS[i], NULL) == ECS_OK);
    }
    f.live = game_preview_world_new(f.world);
    game_preview_definition
        *definition = game_preview_definition_new("Editing", behavior_asset_new("Editing"));
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

static void fixture_free(new_fixture *f)
{
    studio_free(f->studio);
    app_scene_authoring_free(f->scene);
    engine_free(f->runtime);
    game_preview_world_free(f->live);
    editor_free(f->graph);
    ecs_world_free(f->world);
    engine_viewport_free(f->viewport);
    remove_tree(f->root);
}

static void frames(new_fixture *f, int count)
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

/* The file's text as its model loads and saves it, in the form --format
   writes. */
static char *canonical(new_asset_kind kind, const char *text)
{
    char error[256];
    char *json = NULL;
    if (kind == NEW_ASSET_BEHAVIOR || kind == NEW_ASSET_LIBRARY ||
        kind == NEW_ASSET_SYSTEM) {
        script_syntax *source = script_parse(text);
        json = authoring_source_format(source, error, sizeof error);
        script_syntax_free(source);
    } else if (kind == NEW_ASSET_SCENE) {
        game_scene_document *scene = game_scene_document_load(text, error, sizeof error);
        json = scene ? game_scene_document_save(
                           scene,
                           (ecs_component_catalog){ GAME_REGISTRY_COMPONENTS,
                                                    GAME_REGISTRY_COMPONENT_COUNT }
                       )
                     : NULL;
        game_scene_document_free(scene);
    } else if (kind == NEW_ASSET_PROTOTYPE) {
        game_prototype_registry registry = { .components = GAME_REGISTRY_COMPONENTS,
                                             .component_count = GAME_REGISTRY_COMPONENT_COUNT };
        game_prototype *prototype = game_prototype_load(text, &registry, error, sizeof error);
        json = prototype ? game_prototype_save(prototype) : NULL;
        game_prototype_free(prototype);
    } else {
        behavior_asset *asset = behavior_asset_load(text);
        json = asset ? behavior_asset_save(asset) : NULL;
        behavior_asset_free(asset);
    }
    return json;
}

/* Whether no node of a graph asset's text holds a position, so the editor
   arranges them all. */
static bool automatic(new_asset_kind kind, const char *text)
{
    if (kind == NEW_ASSET_SCENE || kind == NEW_ASSET_PROTOTYPE) {
        return true;
    }
    if (kind == NEW_ASSET_BEHAVIOR || kind == NEW_ASSET_LIBRARY ||
        kind == NEW_ASSET_SYSTEM) {
        const script_catalog catalog = { 0 };
        script_syntax *syntax = script_parse(text);
        script_analysis *analysis = script_analyze(syntax, &catalog);
        authoring_source_presentation
            *view = analysis->valid
                      ? authoring_source_presentation_new(analysis, &catalog, NULL)
                      : NULL;
        const behavior_asset *asset = view ? authoring_source_presentation_view(view) : NULL;
        bool placed = !asset;
        for (int i = 0; asset && i < acount(asset->graph->nodes); i++) {
            placed |= asset->graph->nodes[i]->placed;
        }
        authoring_source_presentation_free(view);
        script_analysis_free(analysis);
        script_syntax_free(syntax);
        return !placed;
    }
    behavior_asset *asset = behavior_asset_load(text);
    bool placed = !asset;
    for (int i = 0; asset && i < acount(asset->graph->nodes); i++) {
        placed |= asset->graph->nodes[i]->placed;
    }
    behavior_asset_free(asset);
    return !placed;
}

/* The asset `kind` makes is open as Assets opens it. */
static bool opened(const app_scene_authoring *scene, int kind, const char *name, const char *path)
{
    app_editor_document_context document = app_scene_authoring_document(scene);
    switch (kind) {
        case NEW_ASSET_SCENE:
            return document.kind == APP_EDITOR_DOCUMENT_SCENE &&
                   scene_same_file(scene->scene_path, path);
        case NEW_ASSET_PROTOTYPE:
            return document.kind == APP_EDITOR_DOCUMENT_PROTOTYPE &&
                   !strcmp(document.id, name);
        case NEW_ASSET_SYSTEM:
            return document.kind == APP_EDITOR_DOCUMENT_SYSTEM &&
                   !strcmp(document.id, name);
        default:
            if (kind >= NEW_ASSET_KIND_COUNT) {
                return document.kind == APP_EDITOR_DOCUMENT_UI &&
                       scene_same_file(document.id, path);
            }
            return document.kind == APP_EDITOR_DOCUMENT_BEHAVIOR &&
                   !strcmp(document.id, name);
    }
}

static const reference_asset_kind INDEXED[STUDIO_NEW_KIND_COUNT] = {
    REFERENCE_ASSET_SCENE,    REFERENCE_ASSET_PROTOTYPE,
    REFERENCE_ASSET_BEHAVIOR, REFERENCE_ASSET_EVENT,
    REFERENCE_ASSET_SYSTEM,   REFERENCE_ASSET_LIBRARY,
    REFERENCE_ASSET_MATERIAL, REFERENCE_ASSET_SHAPE,
    REFERENCE_ASSET_FIELD,    REFERENCE_ASSET_LIBRARY,
    REFERENCE_ASSET_SHAPE,    REFERENCE_ASSET_UI,
    REFERENCE_ASSET_UI,       REFERENCE_ASSET_UI,
};

/* Whether NewAsset's name popover is open. */
static bool asking(studio *s)
{
    rml_element *popover = test_query(rml_backend_context(s->backend), "#new-asset-name");
    return popover && popover_open(popover);
}

/* Each New … command asks for a name, Enter makes the asset, and Studio
   opens it. */
static void creates_every_kind()
{
    new_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    for (int kind = 0; kind < STUDIO_NEW_KIND_COUNT; kind++) {
        studio_command_run(s, (studio_command)(STUDIO_COMMAND_NEW_SCENE + kind));
        frames(&f, 2);
        bool asked = asking(s);
        char name[64];
        snprintf(name, sizeof name, "%s", s->new_asset.name);
        press(SDL_SCANCODE_RETURN);
        frames(&f, 2);
        char file[288];
        const char *extension = kind < NEW_ASSET_KIND_COUNT
                                  ? new_asset_extension((new_asset_kind)kind)
                              : kind == STUDIO_NEW_KIND_COUNT - 1
                                  ? UI_STYLE_EXTENSION
                                  : UI_MARKUP_EXTENSION;
        const char *folder = kind < NEW_ASSET_KIND_COUNT
                               ? new_asset_folder((new_asset_kind)kind)
                               : "ui";
        snprintf(file, sizeof file, "%s/%s/%s%s", f.content, folder, name, extension);
        char *text = text_file_read(file);
        char *again = text && kind < NEW_ASSET_KIND_COUNT
                        ? canonical((new_asset_kind)kind, text)
                        : NULL;
        bool canonical_bytes = kind >= NEW_ASSET_KIND_COUNT ||
                               (again && !strcmp(again, text));
        bool automatic_positions = kind >= NEW_ASSET_KIND_COUNT ||
                                   (text && automatic((new_asset_kind)kind, text));
        bool canonical_text = canonical_bytes && automatic_positions;
        if (kind == NEW_ASSET_BEHAVIOR || kind == NEW_ASSET_LIBRARY ||
            kind == NEW_ASSET_SYSTEM) {
            const script_catalog catalog = { 0 };
            script_syntax *syntax = text ? script_parse(text) : NULL;
            script_analysis *source = syntax ? script_analyze(syntax, &catalog) : NULL;
            test_check(source && source->valid && source->name && !strcmp(source->name, name), "creating an executable asset writes readable source that reconstructs its named model");
            script_analysis_free(source);
            script_syntax_free(syntax);
        } else if (kind < NEW_ASSET_KIND_COUNT) {
            av_doc *document = text ? av_doc_read(text, NULL, 0) : NULL;
            test_check(document != NULL, "creating a declarative asset preserves KDL");
            av_doc_free(document);
        }
        const reference_index *index = app_scene_authoring_asset_index(f.scene);
        bool indexed = reference_index_find_asset(index, INDEXED[kind], file, NULL) >= 0;
        bool open = opened(f.scene, kind, name, file);
        bool closed = !asking(s);
        if (!(asked && text && canonical_text && indexed && open && closed)) {
            printf(
                "  %s: asked %d, written %d, canonical %d, automatic %d, "
                "indexed %d, open "
                "%d, closed %d: %s\n",
                name,
                asked,
                text != NULL,
                canonical_bytes,
                automatic_positions,
                indexed,
                open,
                closed,
                app_scene_authoring_message(f.scene)
            );
        }
        test_check(
            asked && text && canonical_text && indexed && open && closed,
            "a New command names the asset, writes it canonical, indexes it "
            "and opens it"
        );
        free(again);
        free(text);
        /* A UI document takes the focus once it reveals its first line,
           which would close a popover opened meanwhile. */
        frames(&f, 4);
    }
    studio_new_asset_start(s, NEW_ASSET_BEHAVIOR);
    bool numbered = !strcmp(s->new_asset.name, "NewBehavior2");
    snprintf(s->new_asset.name, sizeof s->new_asset.name, "NewScene");
    frames(&f, 2);
    press(SDL_SCANCODE_RETURN);
    frames(&f, 2);
    bool refused = strstr(s->new_asset.error, "NewScene already exists") != NULL &&
                   asking(s);
    test_check(numbered && refused, "a new asset's default name is free, and a taken name asks again");
    fixture_free(&f);
}

/* With the Assets filter on a kind, New lists that kind first. */
static void menu_puts_filter_first()
{
    new_fixture f = fixture_new();
    studio *s = f.studio;
    studio_active_layout(s)->editors[STUDIO_AREA_TOP] = STUDIO_EDITOR_ASSETS;
    s->maximized = STUDIO_AREA_TOP;
    s->assets_kind = REFERENCE_ASSET_BEHAVIOR;
    frames(&f, 3);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *button = test_query(context, "#asset-new-button");
    if (button) {
        rml_element_click(button);
    }
    frames(&f, 2);
    rml_element *labels[64];
    int count = test_query_all(context, "#asset-new .menu-item .label", labels, 64);
    int behavior = -1, scene = -1, shown = 0;
    rml_element *item = NULL;
    for (int i = 0; i < count && i < 64; i++) {
        char text[64] = "";
        if (!rml_element_is_visible(labels[i], true)) {
            continue;
        }
        rml_element_inner_rml(labels[i], text, sizeof text);
        if (behavior < 0 && !strcmp(text, "Behavior")) {
            behavior = shown;
            item = rml_element_parent(rml_element_parent(labels[i]));
        }
        scene = scene < 0 && !strcmp(text, "Scene") ? shown : scene;
        shown++;
    }
    if (!(behavior >= 0 && behavior < scene)) {
        printf(
            "  New menu: button %d, %d items shown, Behavior %d, Scene %d\n",
            button != NULL,
            shown,
            behavior,
            scene
        );
    }
    test_check(behavior >= 0 && behavior < scene, "the New menu lists the Assets filter's kind first");
    /* Choosing a kind asks for its name under the button, selected. */
    if (item) {
        rml_element_click(item);
    }
    frames(&f, 4);
    rml_element *field = test_query(context, "#new-asset-name input");
    int start = -1, end = -1;
    if (field) {
        rml_element_selection_range(field, &start, &end);
    }
    rml_element *popover = test_query(context, "#new-asset-name");
    bool below = popover && button &&
                 rml_element_absolute_offset(popover, RML_BOX_BORDER).y >=
                     rml_element_absolute_offset(button, RML_BOX_BORDER).y +
                         rml_element_box_size(button, RML_BOX_BORDER).y;
    bool selected = field && rml_context_focus_element(context) == field &&
                    start == 0 && end == (int)strlen(s->new_asset.name);
    if (!(asking(s) && below && selected)) {
        printf(
            "  New name: asking %d, below %d, selection %d..%d of '%s'\n",
            asking(s),
            below,
            start,
            end,
            s->new_asset.name
        );
    }
    test_check(asking(s) && below && selected, "a kind from the New menu asks for its name under the button, the offered name selected");
    fixture_free(&f);
}

static rml_element *settings_element(studio *s, const char *selector)
{
    rml_element *matches[16];
    int count = test_query_all(rml_backend_context(s->backend), selector, matches, 16);
    for (int i = 0; i < count && i < 16; i++) {
        if (rml_element_is_visible(matches[i], true)) {
            return matches[i];
        }
    }
    return NULL;
}

/* The authored modal keeps validation and discard decisions inside its trap. */
static void project_creation_modal()
{
    new_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 3);
    studio_new_project_open(s);
    frames(&f, 3);
    rml_context *context = rml_backend_context(s->backend);
    rml_element *name = settings_element(s, "#new-project-name");
    bool focused = name && rml_context_focus_element(context) == name;
    /* Reordering keyed modal hosts detaches and reattaches their data model. */
    rml_element *host = settings_element(s, "[data-ui=NewProject]");
    if (host && name) {
        rml_element_move_child(rml_element_parent(host), host, NULL);
        rml_element_focus(name, true);
    }
    press(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    rml_element *error = settings_element(s, "#new-project-error");
    rml_element *field = settings_element(s, ".project-name .field");
    char error_text[128] = "";
    if (error) {
        rml_element_inner_rml(error, error_text, sizeof error_text);
    }
    bool invalid = s->new_project.open &&
                   !strcmp(s->new_project.error, "Enter a project name") &&
                   error && !strcmp(error_text, "Enter a project name") &&
                   field && rml_element_has_attribute(field, "data-invalid") &&
                   !s->open_project;
    if (name) {
        rml_element_focus(name, true);
        rml_backend_process_input(
            s->backend,
            &(rml_input){ .type = RML_INPUT_TEXT_INPUT, .text = "Fresh Project", .text_size = 13 }
        );
    }
    frames(&f, 2);
    bool corrected = field && !s->new_project.error[0] &&
                     !settings_element(s, "#new-project-error") &&
                     !rml_element_has_attribute(field, "data-invalid");
    f.scene->scene_dirty = true;
    press(SDL_SCANCODE_RETURN);
    frames(&f, 4);
    rml_element *cancel = settings_element(s, "#new-project-cancel");
    rml_element *discard = settings_element(s, ".project-actions .discard");
    bool confirmation = s->new_project.open && s->new_project.confirm_discard &&
                        settings_element(s, ".project-confirmation") && cancel &&
                        discard && !s->discard_toast && !s->open_project;
    bool safe_focus = cancel && rml_context_focus_element(context) == cancel;
    press(SDL_SCANCODE_TAB);
    frames(&f, 2);
    bool reachable = discard && rml_context_focus_element(context) == discard;
    press(SDL_SCANCODE_TAB);
    frames(&f, 2);
    bool wrapped = cancel && rml_context_focus_element(context) == cancel;
    press(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool canceled = !s->new_project.open && !s->new_project.confirm_discard &&
                    !s->open_project && f.scene->scene_dirty;
    test_check(
        focused && invalid && corrected && confirmation && safe_focus &&
            reachable && wrapped && canceled,
        "New Project autofocuses its labeled input, validates an empty name, "
        "and keeps discard confirmation keyboard reachable and cancelable "
        "without losing edits"
    );
    fixture_free(&f);
}

/* Settings ▸ Project renames the project and picks its start scene, each
   written to project.kdl in its place. */
static void project_settings_write()
{
    new_fixture f = fixture_new();
    studio *s = f.studio;
    char path[160];
    join(path, sizeof path, f.content, "scenes/Boss.kdl");
    char *json = new_asset_text(NEW_ASSET_SCENE, "Boss"), error[128];
    char *yaml = asset_text_to_disk(json, error, sizeof error);
    write_text(path, yaml);
    free(yaml);
    free(json);
    content_refresh(f.scene);
    studio_command_run(s, STUDIO_COMMAND_SETTINGS);
    frames(&f, 3);
    rml_element *scope = settings_element(s, "[data-ui=Settings] .scope input[value=project]");
    if (scope) {
        rml_element_click(scope);
    }
    frames(&f, 3);
    rml_element *name = settings_element(s, "[data-ui=Settings] .text-control input");
    rml_element *start = settings_element(s, "[data-ui=Settings] select");
    char shown[64] = "", selected[128] = "";
    if (name && start) {
        rml_element_value(name, shown, sizeof shown);
        rml_element_set_value(start, "scenes/Boss.kdl");
        frames(&f, 2);
        rml_element_focus(name, false);
        rml_element_select(name);
        rml_backend_process_input(
            s->backend,
            &(rml_input){ .type = RML_INPUT_TEXT_INPUT, .text = "Renamed", .text_size = 7 }
        );
        rml_element_blur(name);
    }
    frames(&f, 3);
    if (start) {
        rml_element_value(start, selected, sizeof selected);
    }
    join(path, sizeof path, f.root, CHIBI_PROJECT_MANIFEST);
    char *manifest = text_file_read(path);
    const char *expected = "version " PROJECT_VERSION_TEXT "\n"
                           "name \"Renamed\"\n"
                           "content \"content\"\n"
                           "start_scene \"scenes/Boss.kdl\"\n";
    bool written = manifest && !strcmp(manifest, expected);
    bool current_scene = s->project.start_scene &&
                         !strcmp(s->project.start_scene, "scenes/Boss.kdl");
    if (!(scope && name && start && !strcmp(shown, "Fresh") && written && current_scene)) {
        printf(
            "  scope=%p name=%p start=%p shown='%s' selected='%s' model='%s' "
            "written=%d\n",
            (void *)scope,
            (void *)name,
            (void *)start,
            shown,
            selected,
            s->project.start_scene ? s->project.start_scene : "(none)",
            written
        );
        printf("  manifest:\n%s\n", manifest ? manifest : "(none)");
    }
    test_check(
        scope && name && start && !strcmp(shown, "Fresh") && written && current_scene,
        "Settings ▸ Project renames the project and sets its start scene in "
        "project.kdl"
    );
    free(manifest);
    fixture_free(&f);
}

/* Replay startup's content/scene/preview chain after adding the first UI asset. */
static void project_creation_reload()
{
    new_fixture f = fixture_new();
    char root[256], path[512], error[256];
    join(root, sizeof root, f.root, "empty-project");
    assert(chibi_project_create(root, "Empty Reload", error, sizeof error) == 0);
    chibi_project project = { 0 };
    assert(chibi_project_open(&project, root, error, sizeof error) == 0);
    char *start = chibi_project_scene_path(&project, NULL);
    assert(app_scene_authoring_set_content_root(f.scene, project.content) == 0);
    bool empty = app_scene_authoring_load_content(f.scene) == 0 &&
                 app_scene_authoring_load(f.scene, start) == 0 &&
                 app_scene_authoring_apply_pending(f.scene) == 0;
    join(path, sizeof path, project.content, "ui");
    assert(mkdir(path, 0755) == 0);
    join(path, sizeof path, project.content, "ui/NewDocument.rml");
    write_text(path, "<div class=\"panel\">\n  <div class=\"title\">Title</div>\n</div>\n");
    join(path, sizeof path, project.content, "ui/NewDocument.rcss");
    write_text(path, "body { font-family: Inter; font-size: 16dp; line-height: 24dp; color: white; }\n.panel { display: block; padding: 12dp; }\n.title { font-size: 16dp; font-weight: 600; }\n");
    bool reloaded = app_scene_authoring_load_content(f.scene) == 0 &&
                    app_scene_authoring_load(f.scene, start) == 0 &&
                    app_scene_authoring_apply_pending(f.scene) == 0;
    char *json = scene_authoring_scene_text(f.scene);
    game_scene_document *scene = json ? game_scene_document_load(json, error, sizeof error)
                                      : NULL;
    free(json);
    test_check(empty && reloaded && scene && !acount(scene->entities), "a created empty project reloads after its first authored UI document without adding entities");
    game_scene_document_free(scene);
    join(path, sizeof path, project.content, "behaviors");
    assert(mkdir(path, 0755) == 0);
    join(path, sizeof path, project.content, "behaviors/Broken.chibi");
    write_text(path, "invalid: [\n");
    bool rejected_behavior = app_scene_authoring_load_content(f.scene) != 0;
    assert(unlink(path) == 0);
    join(path, sizeof path, project.content, "prototypes");
    assert(mkdir(path, 0755) == 0);
    join(path, sizeof path, project.content, "prototypes/Broken.kdl");
    write_text(path, "invalid: [\n");
    bool rejected_prototype = app_scene_authoring_load_content(f.scene) != 0;
    test_check(rejected_behavior && rejected_prototype, "UI-only content does not hide invalid required behavior or prototype assets");
    assert(unlink(path) == 0);
    join(path, sizeof path, project.content, "systems");
    assert(mkdir(path, 0755) == 0);
    join(path, sizeof path, project.content, "systems/OnlySystem.chibi");
    char *system = new_asset_text(NEW_ASSET_SYSTEM, "OnlySystem");
    assert(system);
    write_text(path, system);
    free(system);
    int loaded = app_behavior_fixtures_load(
        f.live,
        f.scene->prototype_preview,
        project.content
    );
    bool system_reload = app_scene_authoring_load_content(f.scene) == 0 &&
                         app_scene_authoring_load(f.scene, start) == 0 &&
                         app_scene_authoring_apply_pending(f.scene) == 0;
    test_check(loaded == 1 && game_preview_source_find(f.live, path, true) && system_reload, "a system-only project counts its registered system and reloads successfully");
    free(start);
    chibi_project_free(&project);
    fixture_free(&f);
}

void test_studio_new_assets()
{
    creates_every_kind();
    menu_puts_filter_first();
    project_settings_write();
    project_creation_modal();
    project_creation_reload();
}
