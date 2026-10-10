#include "authoring/ui_loader.h"
#include "authoring/ui_events_adapter.h"
/* The editor's authored UI: the Studio project (STUDIO_PROJECT) run as a
 * small world whose GameUi entities are panels. A panel's document mounts in
 * a kit host the way a game mounts it over its canvas, so its widgets,
 * behaviors and data bindings work as they do in games. Studio's preferences
 * are that world's saved keys. */
#include "assets/project.h"
#include "assets/scene_document.h"
#include "authoring/behavior_loader.h"
#include "authoring/application_catalog.h"
#include "authoring/preview.h"
#include "authoring/preview_source.h"
#include "authoring/prototype_preview.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_internal.h"
#include "editor/studio/studio_nodes.h"
#include "graph/document/serialize.h"
#include "render/ui.h"
#include "runtime/component_catalog.h"
#include "runtime/declared_components.h"
#include "support/array.h"
#include "support/asset_text.h"
#include "support/text_file.h"
#include "editor/workspace/tree_watch.h"
#include "support/xalloc.h"
#include "ui/drag.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <yyjson.h>

/* A scene entity with a GameUi document. */
typedef struct panel
{
    char *name;
    int index; /* in the launched scene */
    ui_mount *mount;
    uint64_t refreshed; /* the revision its behaviors last refreshed at; 0 before */
    char error[256]; /* the last error reported */
} panel;

struct studio_ui
{
    char *content_root;
    char *default_page; /* the Studio project's default root document, or NULL */
    char *page;         /* the Studio scene's root document, or NULL */
    game_prototype_registry registry;
    game_prototype_preview *world;
    game_prototype **prototypes;
    panel *panels;
    tree_watch files, code_files,
        metadata_files; /* declarative and executable project assets */
    rml_listener *listener;
    ui_data data; /* what the kit's hosted panels bind */
    studio_sources sources;
    ui_mount *areas[STUDIO_AREA_COUNT]; /* each area's panel, when it shows one */
    ui_mount *headers[STUDIO_AREA_COUNT];  /* each area's header */
    ui_mount *controls[STUDIO_AREA_COUNT]; /* an authored panel in each header */
    studio_node_service nodes; /* the "studio" service its behaviors call */
    bool wake; /* authored activations waiting for a host emission */
    /* What the panels' behaviors read (studio_nodes_stamp) as of the last
       tick; `revision` counts its changes, and the panels refresh when it
       moved past theirs. */
    uint64_t stamp, revision;
    uint64_t ui_seen, save_seen, panels_seen;
    uint64_t prefs_seen; /* the settings revision Studio's preferences match */
    char **errors;       /* those reported, until the sources change */
};

/* The mount rooted at `root`, or NULL. */
static ui_mount *mount_at(studio_ui *ui, rml_element *root)
{
    for (int i = 0; i < acount(ui->panels); i++) {
        if (ui->panels[i].mount && ui_mount_root(ui->panels[i].mount) == root) {
            return ui->panels[i].mount;
        }
    }
    ui_mount *const *kinds[] = { ui->areas, ui->headers, ui->controls };
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
            if (kinds[k][i] && ui_mount_root(kinds[k][i]) == root) {
                return kinds[k][i];
            }
        }
    }
    return NULL;
}

/* The innermost panel or area mount holding `element`, or NULL: a header's
 * controls are a mount inside the AreaHeader's, and their events are the
 * controls' panel's. */
static ui_mount *mount_holding(studio_ui *ui, rml_element *element)
{
    for (rml_element *at = element; at; at = rml_element_parent(at)) {
        ui_mount *m = mount_at(ui, at);
        if (m) {
            return m;
        }
    }
    return NULL;
}

static void on_event(void *user, const rml_event *event)
{
    studio_ui *ui = user;
    ui->wake = true;
    ui_mount *m = mount_holding(ui, event->target);
    if (m) {
        ui_mount_listen(m, event);
    }
}

static void on_emit(void *user, const rml_event *event, const char *name, int argc, const char *const *args)
{
    studio_ui *ui = user;
    ui->wake = true;
    ui_mount *m = mount_holding(ui, event->current);
    if (m) {
        ui_mount_emit(m, event, name, argc, args);
    }
}

static game_prototype *load_prototype(const studio_ui *ui, const char *name, char *error, size_t size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/prototypes/%s.kdl", ui->content_root, name);
    char *text = text_file_read(path);
    game_prototype *prototype = text
                                  ? game_prototype_load(text, &ui->registry, error, size)
                                  : NULL;
    if (!text) {
        snprintf(error, size, "prototypes/%s.kdl is missing", name);
    }
    free(text);
    return prototype;
}

/* Launches the Studio scene; each entity becomes a panel by name. */
static bool launch(studio_ui *ui, char *error, size_t size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/scenes/Studio.kdl", ui->content_root);
    char *text = text_file_read(path);
    snprintf(error, size, "scenes/Studio.kdl is missing");
    game_scene_document *scene = text ? game_scene_document_load(text, error, size)
                                      : NULL;
    free(text);
    if (!scene) {
        return false;
    }
    free(ui->page);
    const char *page = scene->ui ? scene->ui : ui->default_page;
    ui->page = page ? xstrdup(page) : NULL;
    game_prototype_preview_scene_entity *entities = NULL;
    bool ok = true;
    for (int i = 0; ok && i < acount(scene->entities); i++) {
        game_scene_entity *e = &scene->entities[i];
        game_prototype *prototype = load_prototype(ui, e->prototype, error, size);
        ok = prototype != NULL;
        if (!ok) {
            break;
        }
        apush(ui->prototypes, prototype);
        game_prototype_preview_register_prototype(ui->world, prototype);
        apush(
            entities,
            ((game_prototype_preview_scene_entity){
                .prototype = prototype,
                .parent = e->parent,
                .components = e->components,
                .behaviors = (const char *const *)e->behaviors,
                .behavior_count = acount(e->behaviors),
                .own_behaviors = e->own_behaviors })
        );
        apush(ui->panels, ((panel){ .name = xstrdup(e->name), .index = i }));
    }
    ok = ok &&
         game_prototype_preview_launch_scene(ui->world, entities, acount(entities), error, size) ==
             0;
    afree(entities);
    game_scene_document_free(scene);
    return ok;
}

static void forget_errors(studio_ui *ui)
{
    for (int i = 0; i < acount(ui->errors); i++) {
        free(ui->errors[i]);
    }
    afree(ui->errors);
}

/* Reports each error once until the sources change, as a panel's (report). */
static void report_error(studio *s, const char *path, const char *message)
{
    char error[1024];
    snprintf(error, sizeof error, "%s %s", path, message);
    for (int i = 0; i < acount(s->ui->errors); i++) {
        if (!strcmp(s->ui->errors[i], error)) {
            return;
        }
    }
    apush(s->ui->errors, xstrdup(error));
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = "Studio UI", .description = error });
    studio_record(s, KIT_TOAST_ERROR, error);
}

/* Every panel's behaviors refresh in the next UI frame. */
static void refresh_all(studio_ui *ui)
{
    ui->revision++;
    ui->wake = true;
}

/* Studio owns its optional native capabilities independently of resource paths. */
static int prepare_sources(game_preview_world *world, const char *root, char *error, size_t size)
{
    if (game_preview_sources_working(world)) {
        return 0;
    }
    native_capabilities *catalog = authoring_application_catalog_new(true, error, size);
    if (!catalog) {
        return -1;
    }
    int status = game_preview_sources_prepare_with_catalog(
        world, root, behavior_default_backend(), catalog, error, size
    );
    native_capabilities_free(catalog);
    return status;
}

/* The canonical source collection prepares and publishes the entire edit batch.
 * A failed candidate keeps the last applied panel behavior generation. */
static void reload_sources(studio *s, const source_catalog_edit *edits, int count)
{
    game_preview_world *live = game_prototype_preview_live(s->ui->world);
    char message[256];
    if (prepare_sources(live, s->ui->content_root, message, sizeof message)) {
        report_error(s, s->ui->content_root, message);
        return;
    }
    if (game_preview_sources_edit(live, edits, count, message, sizeof message) ||
        game_preview_sources_publish(live, NULL, message, sizeof message)) {
        report_error(s, s->ui->content_root, message);
        return;
    }
    forget_errors(s->ui);
    refresh_all(s->ui);
}

static void free_world(studio_ui *ui)
{
    forget_errors(ui);
    for (int i = 0; i < acount(ui->panels); i++) {
        free(ui->panels[i].name);
        ui_mount_free(ui->panels[i].mount);
    }
    for (int i = 0; i < STUDIO_AREA_COUNT; i++) {
        ui_mount_free(ui->areas[i]);
        ui_mount_free(ui->headers[i]);
        ui_mount_free(ui->controls[i]);
        ui->areas[i] = ui->headers[i] = ui->controls[i] = NULL;
    }
    afree(ui->panels);
    game_prototype_preview_free(ui->world);
    for (int i = 0; i < acount(ui->prototypes); i++) {
        game_prototype_free(ui->prototypes[i]);
    }
    afree(ui->prototypes);
    ui->panels = NULL;
    ui->prototypes = NULL;
}

/* A new world for the Studio scene, whose behaviors reach Studio through
 * the editor nodes' service; panels mount again in their hosts. */
static bool open_world(studio *s, studio_ui *ui, char *error, size_t size)
{
    ui->world = game_prototype_preview_new(&ui->registry);
    if (!ui->world) {
        snprintf(error, size, "Could not initialize Studio UI world");
        return false;
    }
    ui->nodes = (studio_node_service){ s, studio_node_run };
    game_preview_world *live = game_prototype_preview_live(ui->world);
    int bound = game_source_bind_service(live->runtime->sources, "studio", &ui->nodes);
    assert(!bound);
    (void)bound;
    if (prepare_sources(live, ui->content_root, error, size)) {
        return false;
    }
    if (app_behavior_fixtures_load(live, NULL, ui->content_root) < 0) {
        snprintf(error, size, "Could not publish Studio UI sources");
        return false;
    }
    return launch(ui, error, size);
}

static bool in_folder(const studio_ui *ui, const char *path, const char *folder)
{
    size_t root = strlen(ui->content_root), length = strlen(folder);
    return !strncmp(path + root, folder, length) && path[root + length] == '/';
}

/* Behaviors reload in place; a scene or prototype edit relaunches the world.
 * UI documents follow their files as they mount. */
static void follow_disk(studio *s)
{
    studio_ui *ui = s->ui;
    char **changed = tree_watch_poll(&ui->files, ui->content_root, DECLARATIVE_ASSET_EXTENSION, s->time);
    char **code = tree_watch_poll(&ui->code_files, ui->content_root, CODE_ASSET_EXTENSION, s->time);
    for (int i = 0; i < acount(code); i++) {
        apush(changed, code[i]);
    }
    afree(code);
    char **metadata = tree_watch_poll(
        &ui->metadata_files,
        ui->content_root,
        CODE_ASSET_METADATA_EXTENSION,
        s->time
    );
    for (int i = 0; i < acount(metadata); i++) {
        char *path = metadata[i];
        path[strlen(path) - strlen(".meta")] = '\0';
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        bool known = false;
        for (int j = 0; j < acount(changed); j++) {
            known |= !strcmp(changed[j], path);
        }
        if (!known) {
            apush(changed, xstrdup(path));
        }
    }
    tree_watch_changes_free(metadata);
    bool relaunch = false, readable = true;
    source_catalog_edit *edits = NULL;
    ui->wake |= acount(changed) != 0;
    for (int i = 0; i < acount(changed); i++) {
        if (source_catalog_source_path(ui->content_root, changed[i]) ||
            in_folder(ui, changed[i], "/events")) {
            char *text = text_file_read(changed[i]);
            struct stat info;
            if (!text && (stat(changed[i], &info) == 0 || errno != ENOENT)) {
                report_error(s, changed[i], "Could not read the changed source");
                readable = false;
                continue;
            }
            apush(edits, ((source_catalog_edit){ .path = changed[i], .text = text }));
        }
        relaunch |= in_folder(ui, changed[i], "/scenes") ||
                    in_folder(ui, changed[i], "/prototypes");
    }
    if (readable && !relaunch && acount(edits)) {
        reload_sources(s, edits, acount(edits));
    }
    for (int i = 0; i < acount(edits); i++) {
        free((char *)edits[i].text);
    }
    afree(edits);
    tree_watch_changes_free(changed);
    if (!relaunch) {
        return;
    }
    free_world(ui);
    char error[256];
    if (!open_world(s, ui, error, sizeof error)) {
        report_error(s, "scenes/Studio.kdl", error);
    }
}

studio_ui *studio_ui_new(studio *s, const char *project_root, char *error, size_t size)
{
    rml_context *context = rml_backend_context(s->backend);
    studio_ui *ui = xcalloc(1, sizeof *ui);
    chibi_project project = { 0 };
    char manifest_error[256];
    if (chibi_project_open(&project, project_root, manifest_error, sizeof manifest_error) == 0 &&
        project.ui) {
        ui->default_page = xstrdup(project.ui);
    }
    chibi_project_free(&project);
    ui->revision = 1; /* a panel is due before it first refreshes */
    ui_data_init(&ui->data, context, studio_sources_new(), on_emit, ui);
    size_t length = strlen(project_root) + sizeof "/content";
    ui->content_root = xmalloc(length);
    snprintf(ui->content_root, length, "%s/content", project_root);
    ecs_component_catalog components = project_components();
    ui->registry = (game_prototype_registry){ .components = components.components,
                                              .component_count = components.component_count };
    ui->listener = rml_listener_create(on_event, ui);
    if (!open_world(s, ui, error, size)) {
        studio_ui_free(ui);
        return NULL;
    }
    tree_watch_poll(&ui->files, ui->content_root, DECLARATIVE_ASSET_EXTENSION, 0);
    tree_watch_poll(&ui->code_files, ui->content_root, CODE_ASSET_EXTENSION, 0);
    tree_watch_poll(&ui->metadata_files, ui->content_root, CODE_ASSET_METADATA_EXTENSION, 0);
    return ui;
}

/* Frees the world; kit hosts that showed panels must be gone, since they
 * hold the listener. */
void studio_ui_free(studio_ui *ui)
{
    if (!ui) {
        return;
    }
    free_world(ui);
    free(ui->page);
    free(ui->default_page);
    studio_sources_free(&ui->sources);
    ui_data_free(&ui->data);
    tree_watch_free(&ui->files);
    tree_watch_free(&ui->code_files);
    tree_watch_free(&ui->metadata_files);
    rml_listener_destroy(ui->listener);
    free(ui->content_root);
    free(ui);
}

static game_save *settings(studio_ui *ui)
{
    return game_prototype_preview_live(ui->world)->runtime->save;
}

static const GameUi *panel_ui(studio_ui *ui, const panel *p);

/* GameUi component setters drive visibility and the document to mount; those
 * changes are distinct from the runtime UI selector/data revision. */
static uint64_t panel_state(studio_ui *ui)
{
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < acount(ui->panels); i++) {
        const GameUi *value = panel_ui(ui, &ui->panels[i]);
        key = (key ^ (value != NULL)) * 1099511628211ull;
        if (!value) {
            continue;
        }
        key = (key ^ value->hidden) * 1099511628211ull;
        key = (key ^ (uint64_t)value->order) * 1099511628211ull;
        for (const char *p = value->document; p && *p; p++) {
            key = (key ^ (unsigned char)*p) * 1099511628211ull;
        }
    }
    return key;
}

/* Authored behaviors keep their host clock even when unchanged native panels
 * have no work. Actual UI/save revisions wake the next model emission, as
 * does a change in what the panels' behaviors read. */
bool studio_ui_tick(studio *s)
{
    if (!s->ui) {
        return false;
    }
    studio_ui *ui = s->ui;
    follow_disk(s);
    game_prototype_preview_tick(ui->world, s->time);
    uint64_t stamp = studio_nodes_stamp(s);
    if (stamp != ui->stamp) {
        ui->stamp = stamp;
        refresh_all(ui);
    }
    uint64_t model = game_ui_revision(engine_ui(game_prototype_preview_runtime(ui->world)));
    uint64_t save = game_save_revision(settings(ui));
    uint64_t panels = panel_state(ui);
    bool changed = ui->wake || model != ui->ui_seen || save != ui->save_seen ||
                   panels != ui->panels_seen;
    ui->wake = false;
    ui->ui_seen = model;
    ui->save_seen = save;
    ui->panels_seen = panels;
    return changed;
}

void studio_ui_frame(studio *s)
{
    if (!s->ui) {
        return;
    }
    ui_data_sync(&s->ui->data, settings(s->ui));
    ui_data_show_entities(
        &s->ui->data,
        engine_ui(game_prototype_preview_runtime(s->ui->world))
    );
    studio_sources_sync(s, &s->ui->sources, &s->ui->data);
    studio_prefs_share(&s->prefs, settings(s->ui), &s->ui->prefs_seen);
}

void studio_ui_resync(studio *s, const char *name)
{
    studio_sources_resync(s, &s->ui->sources, &s->ui->data, name);
}

static panel *find_panel(studio_ui *ui, const char *name)
{
    for (int i = 0; i < acount(ui->panels); i++) {
        if (!strcmp(ui->panels[i].name, name)) {
            return &ui->panels[i];
        }
    }
    return NULL;
}

/* Runs `a` on `entity`'s behaviors now, or on every panel's when `entity`
 * is none, so what they set shows in the frame that sent it. */
static void dispatch(studio *s, ecs_entity entity, const ui_activation *a)
{
    game_preview_world *live = game_prototype_preview_live(s->ui->world);
    render_ui_host host = authoring_ui_events(live);
    if (!ui_activation_send(&host, entity, a, s->mac ? RML_KM_META : RML_KM_CTRL)) {
        report_error(s, a->event, "declares a payload a UI event cannot carry");
        return;
    }
    const char *failure = host.failure ? host.failure(host.context) : "";
    if (failure[0]) {
        char message[512];
        snprintf(message, sizeof message, "failed: %s", failure);
        report_error(s, a->event, message);
    }
}

/* Runs the events behaviors sent each other meanwhile (Send Event), so a
 * chain settles before the panel shows. */
static void settle(studio_ui *ui)
{
    game_entities *entities = game_prototype_preview_entities(ui->world);
    for (int round = 0; round < 8 && game_entities_pending_events(entities); round++) {
        game_entities_dispatch_events(entities);
    }
}

/* Sends a panel's events to its entity's behaviors now, an input's value
 * first, unless they are Studio's (studio_panel_route). New Project's
 * dialog takes its own, its backdrop's Close among them. True when it sent
 * any. */
static bool send_activations(studio *s, ui_mount *mount, ecs_entity entity)
{
    studio_ui *ui = s->ui;
    ui_activation a;
    bool sent = false;
    while (ui_mount_take(mount, &a)) {
        sent = true;
        ui->wake = true;
        if (!strcmp(ui_mount_name(mount), "NewProject") &&
            (!strncmp(a.event, "newProject.", 11) || !strcmp(a.event, "Close"))) {
            bool confirmation = s->new_project.confirm_save;
            studio_new_project_action(s, !strcmp(a.event, "Close") ? "cancel" : a.event + 11);
            if (!confirmation && s->new_project.confirm_save) {
                game_entities_request_event(
                    game_prototype_preview_entities(ui->world),
                    entity,
                    "ConfirmDiscard",
                    (execution_arguments){ 0 }
                );
            } else if (s->new_project.error[0]) {
                game_entities_request_event(
                    game_prototype_preview_entities(ui->world),
                    entity,
                    "InvalidName",
                    (execution_arguments){ 0 }
                );
            }
            continue;
        }
        if (studio_panel_route(s, &a)) {
            studio_sources_acted(s, &ui->sources, &ui->data, a.event);
            continue;
        }
        if (a.has_value && a.id[0]) {
            char selector[160];
            snprintf(selector, sizeof selector, "#%s", a.id);
            game_ui_set_value(
                engine_ui(game_prototype_preview_runtime(ui->world)),
                ecs_entity_to_u64(entity),
                selector,
                a.value
            );
        }
        dispatch(s, entity, &a);
    }
    return sent;
}

void studio_ui_send(studio *s, const char *name, const char *argument)
{
    if (!s->ui) {
        return;
    }
    ui_activation a = { .argc = 1 };
    snprintf(a.event, sizeof a.event, "%s", name);
    snprintf(a.args[0], sizeof a.args[0], "%s", argument);
    dispatch(s, (ecs_entity){ 0 }, &a);
    settle(s->ui);
    refresh_all(s->ui);
}

/* While an asset is dragged over an object's element, data-object="<key>"
   (an Outliner row), the object shows it in the Scene and the overlay
   says what a drop does. */
static void preview_drop(studio *s, ui_mount *mount, rml_element *host)
{
    if (!ui_drag_active(rml_backend_context(s->backend), NULL)) {
        return;
    }
    rml_element *over = rml_element_query_selector(host, "[data-drop-over][data-object]");
    ui_drag_item item;
    if (!over || !ui_mount_holds(mount, over) || !ui_drag_over(over, &item)) {
        return;
    }
    char key[24] = "";
    rml_element_attribute(over, "data-object", key, sizeof key);
    int actor = studio_object_index(s, key);
    if (actor >= 0) {
        studio_drop_preview(s, actor, item.type, item.payload);
    }
}

/* Reports a panel's asset error once when it appears. */
static void report(studio *s, panel *p, ui_mount *mount)
{
    const char *error = ui_mount_error(mount);
    if (!strcmp(error, p->error)) {
        return;
    }
    snprintf(p->error, sizeof p->error, "%s", error);
    if (error[0]) {
        kit_toast(
            s->kit,
            0,
            &(kit_toast_desc){ .type = KIT_TOAST_ERROR, .title = p->name, .description = error }
        );
        studio_record(s, KIT_TOAST_ERROR, error);
    }
}

/* The panel's GameUi, or NULL when it has none. */
static const GameUi *panel_ui(studio_ui *ui, const panel *p)
{
    ecs_world *world = game_prototype_preview_world(ui->world);
    ecs_entity entity = game_prototype_preview_entity_at(ui->world, p->index);
    return GAME_GameUi_has(world, entity) ? GAME_GameUi_get_const(world, entity) : NULL;
}

/* The panels shown as modal dialogs. */
static const char *const MODALS[] = { "Settings", "Collaboration", "Shortcuts" };

bool studio_ui_modal_open(studio *s)
{
    if (s->new_project.open) {
        return true;
    }
    for (size_t i = 0; s->ui && i < sizeof MODALS / sizeof *MODALS; i++) {
        panel *p = find_panel(s->ui, MODALS[i]);
        const GameUi *component = p ? panel_ui(s->ui, p) : NULL;
        if (component && !component->hidden) {
            return true;
        }
    }
    return false;
}

/* Whether `el` edits text, and so keeps every key: a text input or a
 * textarea. A checkbox, a choice, a button or a number field not being typed
 * in (it types in an input of its own) keeps only the keys it handles, as
 * the kit's controls do. */
static bool edits_text(rml_element *el)
{
    const char *tag = rml_element_tag_name(el);
    char type[16] = "text";
    rml_element_attribute(el, "type", type, sizeof type);
    return !strcmp(tag, "textarea") ||
           (!strcmp(tag, "input") && strcmp(type, "checkbox") && strcmp(type, "radio"));
}

/* The area whose authored editor, or its header's controls, hold `focus` on
 * an element that edits no text, or -1: Studio's keys and the area's reach
 * it, as they reach a native area. A Studio UI panel shown in an area keeps
 * its keys. */
static int focus_area(studio *s, rml_element *focus)
{
    const studio_layout *layout = studio_active_layout(s);
    for (int i = 0; focus && !edits_text(focus) && i < STUDIO_AREA_COUNT; i++) {
        bool holds = (s->ui->areas[i] && ui_mount_holds(s->ui->areas[i], focus)) ||
                     (s->ui->controls[i] && ui_mount_holds(s->ui->controls[i], focus));
        if (holds && layout->editors[i] != STUDIO_EDITOR_PANEL) {
            return i;
        }
    }
    return -1;
}

void studio_ui_data_dirty_all(studio *s)
{
    rml_data_model_dirty_all(s->ui->data.model);
}

game_source_registry *studio_ui_sources(studio *s)
{
    return game_prototype_preview_live(s->ui->world)->runtime->sources;
}

rml_data_reads studio_ui_data_reads(const studio *s)
{
    return s->ui ? rml_data_model_reads(s->ui->data.model) : (rml_data_reads){ 0 };
}

bool studio_ui_shortcut_focus(studio *s)
{
    if (!s || !s->ui || !s->backend || !s->kit || kit_has_popup(s->kit) ||
        studio_palette_is_open(s) || studio_ui_modal_open(s)) {
        return false;
    }
    studio_ui *ui = s->ui;
    rml_context *context = rml_backend_context(s->backend);
    rml_element *focus = context ? rml_context_focus_element(context) : NULL;
    /* An editor area keeps only its text fields' keys, as a native one
       does, so Studio's shortcuts and the area's own (F2, X) reach it. */
    if (focus_area(s, focus) >= 0) {
        return true;
    }
    if (kit_accepts_text(s->kit)) {
        return false;
    }
    rml_element *button = focus;
    while (button && strcmp(rml_element_tag_name(button), "button") &&
           !rml_element_is_class_set(button, "btn")) {
        button = rml_element_parent(button);
    }
    if (!button) {
        return false;
    }
    /* A button in the chrome, a header's controls among it. */
    for (rml_element *at = focus; at; at = rml_element_parent(at)) {
        ui_mount *mount = mount_at(ui, at);
        const char *name = mount ? ui_mount_name(mount) : "";
        if (!strcmp(name, "TopBar") || !strcmp(name, "AreaHeader") ||
            !strcmp(name, "StatusBar")) {
            return true;
        }
    }
    return false;
}

int studio_ui_keyboard_area(studio *s)
{
    rml_context *context = s->ui && s->backend ? rml_backend_context(s->backend) : NULL;
    rml_element *focus = context ? rml_context_focus_element(context) : NULL;
    if (!focus || !rml_element_is_pseudo_class_set(focus, "focus-visible")) {
        return -1;
    }
    return focus_area(s, focus);
}

/* Rounds of events a panel's own effects may send in one frame. */
#define EFFECT_ROUNDS 4

/* Mounts the panel's document in `host` and runs its events. */
static void show_panel(studio *s, panel *p, ui_mount **mount, rml_element *host, const char *document)
{
    studio_ui *ui = s->ui;
    ecs_entity entity = game_prototype_preview_entity_at(ui->world, p->index);
    if (*mount && strcmp(ui_mount_name(*mount), document) != 0) {
        ui_mount_free(*mount);
        *mount = NULL;
    }
    /* Its behaviors refresh when a document is new to them, when what they
       read changed since they last did, and after the panel's own events. */
    bool due = !*mount || p->refreshed != ui->revision;
    if (!*mount) {
        *mount = authoring_ui_mount_new(ui->content_root, document);
    }
    studio_sources_mounted(s, &ui->sources, &ui->data, p->name);
    uint64_t id = ecs_entity_to_u64(entity);
    ui_data_bind_self(&ui->data, host, id);
    if (ui_mount_sync(*mount, host, ui->listener, s->time)) {
        kit_style_sheet(s->kit, document, ui_mount_style(*mount));
    }
    game_ui *model = engine_ui(game_prototype_preview_runtime(ui->world));
    /* What behaviors asked for since (a list's reveal) applies before the
       lists window, whose `window` events join the panel's activations;
       then the behaviors refresh when due, as a panel source syncs, and
       what they set shows in this frame. An effect can send events of its
       own (a focus moved blurs a field, which commits it): they run in the
       same frame too, a few rounds at most. */
    uint64_t revision = ui->revision;
    ui_mount_apply(*mount, model, id);
    ui_mount_measure(*mount, model, id);
    for (int round = 0;
         round == 0 || (round < EFFECT_ROUNDS && ui_mount_pending(*mount));
         round++) {
        due |= send_activations(s, *mount, entity);
        if (due) {
            dispatch(s, entity, &(ui_activation){ .event = "Refresh" });
            due = false;
        }
        settle(ui);
        ui_data_show_entities(&ui->data, model);
        ui_mount_apply(*mount, model, id);
    }
    p->refreshed = revision;
    preview_drop(s, *mount, host);
    report(s, p, *mount);
}

/* The document panel `name` shows, with the panel in `out`; NULL when it
 * has none. */
static const char *panel_document(studio *s, const char *name, panel **out)
{
    panel *p = s->ui ? find_panel(s->ui, name) : NULL;
    const GameUi *component = p ? panel_ui(s->ui, p) : NULL;
    *out = p;
    return component && component->document && component->document[0]
             ? component->document
             : NULL;
}

rml_element *studio_ui_panel(studio *s, const char *key, const char *name)
{
    panel *p;
    const char *document = panel_document(s, name, &p);
    if (!document) {
        return NULL;
    }
    rml_element *host = kit_host(s->kit, key, "panel-host");
    show_panel(s, p, &p->mount, host, document);
    return host;
}

rml_element *studio_ui_dialog(studio *s, const char *key, const char *name)
{
    panel *p;
    const char *document = panel_document(s, name, &p);
    if (!document) {
        return NULL;
    }
    rml_element *host = kit_modal_host(s->kit, key);
    show_panel(s, p, &p->mount, host, document);
    return host;
}

void studio_ui_panel_detach(studio *s, const char *name)
{
    panel *p = s->ui ? find_panel(s->ui, name) : NULL;
    if (p) {
        ui_mount_detach(p->mount);
    }
}

/* The alias takes when the mount binds the host to the data model, as a
 * widget's data-alias-* attributes do. */
rml_element *studio_ui_area_header(studio *s, int area, const char *name)
{
    panel *p;
    const char *document = panel_document(s, name, &p);
    if (!document) {
        return NULL;
    }
    rml_element *host = kit_host(s->kit, "head", "panel-host");
    char alias[32];
    snprintf(alias, sizeof alias, "editor.areas[%d]", area);
    kit_attr(s->kit, "data-alias-area", alias);
    show_panel(s, p, &s->ui->headers[area], host, document);
    return host;
}

/* A mount per area, so each header showing the panel keeps its own. */
rml_element *studio_ui_area_controls(studio *s, int area, const char *key, const char *name)
{
    panel *p;
    const char *document = panel_document(s, name, &p);
    if (!document) {
        return NULL;
    }
    rml_element *host = kit_host(s->kit, key, "panel-host");
    show_panel(s, p, &s->ui->controls[area], host, document);
    return host;
}

void studio_ui_area_detach(studio *s, int area)
{
    if (!s->ui) {
        return;
    }
    ui_mount_detach(s->ui->headers[area]);
    ui_mount_detach(s->ui->areas[area]);
    ui_mount_detach(s->ui->controls[area]);
}

const char *studio_ui_page(const studio_ui *ui)
{
    return ui->page ? ui->page : "";
}

int studio_ui_panel_count(const studio *s)
{
    return s->ui ? acount(s->ui->panels) : 0;
}

const char *studio_ui_panel_name(const studio *s, int index)
{
    return s->ui->panels[index].name;
}

rml_element *studio_ui_area(studio *s, int area, const char *name)
{
    panel *p;
    const char *document = panel_document(s, name, &p);
    if (!document) {
        return NULL;
    }
    rml_element *host = kit_host(s->kit, "panel", "area-panel");
    show_panel(s, p, &s->ui->areas[area], host, document);
    return host;
}

rml_element *studio_ui_area_view(studio *s, int area, const char *name, const char *views)
{
    panel *p;
    const char *document = panel_document(s, name, &p);
    if (!document) {
        return NULL;
    }
    rml_element *host = kit_host(s->kit, "panel", "area-panel");
    char alias[64];
    snprintf(alias, sizeof alias, "%s[%d]", views, area);
    kit_attr(s->kit, "data-alias-view", alias);
    show_panel(s, p, &s->ui->areas[area], host, document);
    return host;
}

/* Data-keys names for the keys an area's panel can bind: letters, digits
   and the named keys; NULL for others. */
static const char *key_label(SDL_Keycode key, char letter[2])
{
    switch (key) {
        case SDLK_LEFT: return "Left";
        case SDLK_RIGHT: return "Right";
        case SDLK_UP: return "Up";
        case SDLK_DOWN: return "Down";
        case SDLK_HOME: return "Home";
        case SDLK_END: return "End";
        case SDLK_PAGEUP: return "PageUp";
        case SDLK_PAGEDOWN: return "PageDown";
        case SDLK_RETURN: return "Enter";
        case SDLK_ESCAPE: return "Escape";
        case SDLK_SPACE: return "Space";
        case SDLK_TAB: return "Tab";
        case SDLK_BACKSPACE: return "Backspace";
        case SDLK_DELETE: return "Delete";
        default: break;
    }
    if ((key >= SDLK_A && key <= SDLK_Z) || (key >= SDLK_0 && key <= SDLK_9)) {
        letter[0] = (char)toupper((int)key);
        letter[1] = '\0';
        return letter;
    }
    return NULL;
}

bool studio_ui_area_key(studio *s, int area, SDL_Keycode key, SDL_Keymod mod)
{
    char letter[2];
    const char *name = key_label(key, letter);
    ui_mount *mount = s->ui && area >= 0 ? s->ui->areas[area] : NULL;
    if (!name || !mount ||
        (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT | SDL_KMOD_SHIFT)) ||
        !ui_mount_area_key(mount, name)) {
        return false;
    }
    s->ui_schedule.wake = true;
    return true;
}

static void show_modal(studio *s, const char *name)
{
    panel *p = s->ui ? find_panel(s->ui, name) : NULL;
    const GameUi *component = p ? panel_ui(s->ui, p) : NULL;
    if (component && !component->hidden && component->document &&
        component->document[0]) {
        show_panel(s, p, &p->mount, kit_modal_host(s->kit, name), component->document);
        return;
    }
    if (p) {
        ui_mount_detach(p->mount);
    }
    if (!strcmp(name, "Settings")) {
        s->shortcut_recording = 0;
    }
}

void studio_ui_modal(studio *s, const char *name)
{
    if (!s->ui) {
        return;
    }
    show_modal(s, name);
    /* The top bar's modal pass also hosts the on-demand Collaboration and
       Keyboard Shortcuts panels. */
    if (!strcmp(name, "Settings")) {
        show_modal(s, "Collaboration");
        show_modal(s, "Shortcuts");
    }
}

void studio_ui_show(studio *s, const char *name)
{
    s->ui_schedule.wake = true;
    panel *p = s->ui ? find_panel(s->ui, name) : NULL;
    if (p && panel_ui(s->ui, p)) {
        ecs_entity entity = game_prototype_preview_entity_at(s->ui->world, p->index);
        GAME_GameUi_get(game_prototype_preview_world(s->ui->world), entity)->hidden = false;
    }
}

/* Project operations own the dialog's state (`new_project`); the panel owns
 * its controls. */
void studio_ui_new_project(studio *s)
{
    panel *p;
    const char *document = panel_document(s, "NewProject", &p);
    if (!document) {
        return;
    }
    if (!s->new_project.open) {
        ui_mount_detach(p->mount);
        return;
    }
    show_panel(s, p, &p->mount, kit_modal_host(s->kit, "new-project"), document);
    s->new_project.focus = false;
}

void studio_ui_collaboration_action(studio *s, studio_command command)
{
    if (!s->ui) {
        return;
    }
    if ((command == STUDIO_COMMAND_COLLABORATION_HOST ||
         command == STUDIO_COMMAND_COLLABORATION_JOIN) &&
        studio_collab_busy(s)) {
        return;
    }
    const char *action = NULL;
    const char *field = NULL;
    const char *progress = NULL;
    switch (command) {
        case STUDIO_COMMAND_COLLABORATION_HOST:
            action = "host";
            field = "token";
            progress = "Creating room...";
            break;
        case STUDIO_COMMAND_COLLABORATION_JOIN:
            action = "join";
            field = "invite";
            progress = "Joining room...";
            break;
        case STUDIO_COMMAND_COLLABORATION_LEAVE:
            action = "leave";
            progress = "Leaving room...";
            break;
        case STUDIO_COMMAND_COLLABORATION_INVITE_VIEWER:
            action = "invite_viewer";
            progress = "Creating viewer invite...";
            break;
        case STUDIO_COMMAND_COLLABORATION_INVITE_EDITOR:
            action = "invite_editor";
            progress = "Creating editor invite...";
            break;
        default: return;
    }

    ui_data *data = &s->ui->data;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(data->doc);
    yyjson_mut_val *collaboration = yyjson_mut_obj_get(root, "collaboration");
    yyjson_mut_val *value = field ? yyjson_mut_obj_get(collaboration, field) : NULL;
    const char *argument = value && yyjson_mut_is_str(value)
                             ? yyjson_mut_get_str(value)
                             : "";
    char invite[4096];
    if (command == STUDIO_COMMAND_COLLABORATION_JOIN) {
        while (*argument == ' ' || *argument == '\t' || *argument == '\r' ||
               *argument == '\n') {
            argument++;
        }
        size_t length = strlen(argument);
        while (length &&
               (argument[length - 1] == ' ' || argument[length - 1] == '\t' ||
                argument[length - 1] == '\r' || argument[length - 1] == '\n')) {
            length--;
        }
        if (length >= sizeof invite) {
            length = sizeof invite - 1;
        }
        memcpy(invite, argument, length);
        invite[length] = '\0';
        argument = invite;
    }
    if (command == STUDIO_COMMAND_COLLABORATION_HOST ||
        command == STUDIO_COMMAND_COLLABORATION_JOIN) {
        s->collaboration.busy = true;
    }
    if (command == STUDIO_COMMAND_COLLABORATION_HOST) {
        s->collaboration.admin_token_expanded = false;
    }
    if (command == STUDIO_COMMAND_COLLABORATION_LEAVE) {
        studio_collab_unfollow(s);
    }
    studio_collab_web_status(progress, 0);
    studio_collab_control(action, argument);
    if (value) {
        ui_data_set_text(data->doc, value, "");
        rml_data_model_dirty(data->model, "collaboration");
    }
}

void studio_ui_show_settings(studio *s, const char *query)
{
    if (!s->ui) {
        return;
    }
    studio_sources_search_settings(&s->ui->data, query);
    studio_ui_show(s, "Settings");
}
