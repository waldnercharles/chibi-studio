#include "authoring/ui_loader.h"
/* A ui asset mounted in a Studio kit element: the same mount a game uses,
 * with its RCSS scoped inside the kit's document. */
#include "editor/ui/kit.h"
#include "render/ui.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
    rml_backend *backend;
    kit *k;
    kit_frame_input in;
    rml_listener *listener;
    ui_mount *mount;
    const char *host_key;
    rml_element *host;
    ui_data data;
} host_fixture;

/* A kit styled with the Studio scene's root document, as the editor styles
 * its own. */
kit *test_kit_create(void *context, const char *document)
{
    kit *k = kit_create(context, document);
    ui_page page = { 0 };
    assert(
        authoring_ui_page_sync(&page, "projects/studio/content", "StudioShell", 0) &&
        page.document.rcss
    );
    kit_theme(k, page.document.rcss);
    ui_page_free(&page);
    return k;
}

rml_element *test_query(rml_context *context, const char *selector)
{
    rml_element *found = NULL;
    for (int i = 0; !found && i < rml_context_document_count(context); i++) {
        found = rml_element_query_selector(rml_context_document(context, i), selector);
    }
    return found;
}

int test_query_all(rml_context *context, const char *selector, rml_element **matches, int max)
{
    int count = 0;
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *document = rml_context_document(context, i);
        int room = max > count ? max - count : 0;
        count += rml_element_query_selector_all(document, selector, room ? matches + count : NULL, room);
    }
    return count;
}

static void on_event(void *user, const rml_event *event)
{
    ui_mount_listen(user, event);
}

static void on_emit(void *user, const rml_event *event, const char *name, int argc, const char *const *args)
{
    ui_mount_emit(user, event, name, argc, args);
}

/* A Studio kit whose host shows the ui asset `name`, bound to its own data. */
static void open_fixture(host_fixture *f, const char *root, const char *name)
{
    f->backend = rml_backend_create(640, 480, 1);
    f->k = test_kit_create(rml_backend_context(f->backend), "studio.rml");
    f->in = (kit_frame_input){ .dp_ratio = 1, .width = 640, .height = 480 };
    f->mount = authoring_ui_mount_new(root, name);
    f->listener = rml_listener_create(on_event, f->mount);
    ui_data_init(&f->data, rml_backend_context(f->backend), NULL, on_emit, f->mount);
}

static void close_fixture(host_fixture *f)
{
    kit_destroy(f->k);
    ui_data_free(&f->data);
    rml_backend_destroy(f->backend);
    ui_mount_free(f->mount);
    rml_listener_destroy(f->listener);
}

static void frame(host_fixture *f)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    kit_leaf(k, "own", NULL, "button");
    kit_style(k, "display", "block");
    kit_style(k, "height", "20dp");
    f->host = kit_host(k, f->host_key, NULL);
    kit_style(k, "display", "block");
    kit_style(k, "height", "200dp");
    if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
        kit_style_sheet(k, "Panel", ui_mount_style(f->mount));
    }
    kit_end(k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    f->in.time += 1.0 / 60.0;
}

static float height_of(rml_element *root, const char *selector)
{
    rml_element *el = rml_element_query_selector(root, selector);
    return el ? rml_element_box_size(el, RML_BOX_BORDER).y : -1;
}

static void click(host_fixture *f, rml_element *el)
{
    rml_vec2 at = rml_element_absolute_offset(el, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(el, RML_BOX_BORDER);
    float x = at.x + size.x * 0.5f, y = at.y + size.y * 0.5f;
    rml_input_type types[] = { RML_INPUT_POINTER_MOVE,
                               RML_INPUT_POINTER_DOWN,
                               RML_INPUT_POINTER_UP };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        rml_backend_process_input(f->backend, &(rml_input){ .type = types[i], .x = x, .y = y });
    }
}

/* A hosted menu in a kit popup: its autofocus row takes focus, its data-keys
 * bindings get the arrow keys and Enter, and Move UI Focus steps the focus. */
static void popup_frame(host_fixture *f, bool open)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    kit_leaf(k, "trigger", NULL, NULL);
    if (open) {
        kit_popup_open(k, "menu");
    }
    if (kit_popup_begin(k, "menu", "menu")) {
        f->host = kit_host(k, f->host_key, NULL);
        if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
            kit_style_sheet(k, "Menu", ui_mount_style(f->mount));
        }
        kit_popup_end(k);
    }
    kit_end(k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    f->in.time += 1.0 / 60.0;
}

static const char *focused_id(host_fixture *f)
{
    rml_element *focus = rml_context_focus_element(rml_backend_context(f->backend));
    return focus ? rml_element_id(focus) : "";
}

static void press(host_fixture *f, rml_input_key key)
{
    rml_backend_process_input(f->backend, &(rml_input){ .type = RML_INPUT_KEY_PRESS, .key = key });
    rml_backend_process_input(f->backend, &(rml_input){ .type = RML_INPUT_KEY_RELEASE, .key = key });
}

static void popup_navigation(const char *root)
{
    char folder[128];
    snprintf(folder, sizeof folder, "%s/ui", root);
    test_write_ui(
        folder,
        "Menu",
        "<div data-keys=\"Enter:Choose Down:Next\"><div class=\"row\" id=\"a\" "
        "autofocus>A</div><div class=\"row\" id=\"b\"><input type=\"radio\" "
        "id=\"pick\"/>B</div></div>\n",
        ".row { display: block; height: 20dp; tab-index: auto; }\n"
    );
    host_fixture f = { .host_key = "menu-host" };
    open_fixture(&f, root, "Menu");
    popup_frame(&f, true);
    for (int i = 0; i < 4; i++) {
        popup_frame(&f, false);
    }
    bool first = !strcmp(focused_id(&f), "a");
    ui_activation a;
    press(&f, RML_INPUT_KEY_DOWN);
    popup_frame(&f, false);
    bool next = ui_mount_take(f.mount, &a) && !strcmp(a.event, "Next");
    game_ui *model = game_ui_new();
    game_ui_move_focus(model, 0, ".row", 1);
    ui_mount_apply(f.mount, model, 0);
    bool moved = !strcmp(focused_id(&f), "b");
    game_ui_move_focus(model, 0, ".row", 1);
    ui_mount_apply(f.mount, model, 0);
    bool wrapped = !strcmp(focused_id(&f), "a");
    press(&f, RML_INPUT_KEY_ENTER);
    popup_frame(&f, false);
    bool chose = ui_mount_take(f.mount, &a) && !strcmp(a.event, "Choose");
    game_ui_move_focus(model, 0, ".row", 1);
    game_ui_click(model, 0, ".row:focus input");
    ui_mount_apply(f.mount, model, 0);
    popup_frame(&f, false);
    rml_element *pick = rml_element_query_selector(f.host, "#pick");
    test_check(
        first && next && moved && wrapped && chose &&
            rml_element_has_attribute(pick, "checked"),
        "a hosted menu in a popup takes focus, binds its keys, steps focus and "
        "clicks the focused row"
    );
    game_ui_free(model);
    close_fixture(&f);
    test_remove_ui(folder, "Menu");
}

/* As in HTML, radios without a name form no group. */
static void option_groups(const char *root)
{
    char folder[128];
    snprintf(folder, sizeof folder, "%s/ui", root);
    test_write_ui(
        folder,
        "Groups",
        "<input type=\"radio\" id=\"x\" data-checked=\"save.first\" "
        "value=\"x\"/><input "
        "type=\"radio\" id=\"y\" data-checked=\"save.second\" value=\"y\"/>\n",
        NULL
    );
    host_fixture f = { .host_key = "groups" };
    open_fixture(&f, root, "Groups");
    game_save *save = game_save_new();
    game_save_set(save, "first", "x");
    game_save_set(save, "second", "y");
    for (int i = 0; i < 3; i++) {
        ui_data_sync(&f.data, save);
        frame(&f);
    }
    test_check(
        rml_element_has_attribute(rml_element_query_selector(f.host, "#x"), "checked") &&
            rml_element_has_attribute(rml_element_query_selector(f.host, "#y"), "checked"),
        "unnamed radios bound to different keys keep their own checks"
    );
    close_fixture(&f);
    game_save_free(save);
    test_remove_ui(folder, "Groups");
}

/* A field of the editor's, then a modal dialog hosting the asset. */
static void modal_frame(host_fixture *f)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    kit_leaf(k, "field", "input", NULL);
    f->host = kit_modal_host(k, f->host_key);
    if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
        kit_style_sheet(k, "Dialog", ui_mount_style(f->mount));
    }
    kit_end(k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    f->in.time += 1.0 / 60.0;
}

static void press_tab(host_fixture *f, bool shift)
{
    rml_input tab = { .type = RML_INPUT_KEY_PRESS,
                      .key = RML_INPUT_KEY_TAB,
                      .modifiers = shift ? RML_INPUT_MOD_SHIFT : 0 };
    rml_backend_process_input(f->backend, &tab);
    tab.type = RML_INPUT_KEY_RELEASE;
    rml_backend_process_input(f->backend, &tab);
    modal_frame(f);
}

/* Tab and Shift+Tab cycle through a modal's fields and never reach the
 * editor's behind it; from the editor, Tab enters the modal. */
static void modal_focus(const char *root)
{
    char folder[128];
    snprintf(folder, sizeof folder, "%s/ui", root);
    test_write_ui(folder, "Dialog", "<input type=\"text\" id=\"a\" autofocus/><input type=\"text\" id=\"b\"/>\n", NULL);
    host_fixture f = { .host_key = "dialog" };
    open_fixture(&f, root, "Dialog");
    for (int i = 0; i < 3; i++) {
        modal_frame(&f);
    }
    bool first = !strcmp(focused_id(&f), "a");
    press_tab(&f, false);
    bool next = !strcmp(focused_id(&f), "b");
    press_tab(&f, false);
    bool wrapped = !strcmp(focused_id(&f), "a");
    press_tab(&f, true);
    bool back = !strcmp(focused_id(&f), "b");
    /* The modal is in the layer, a document over the kit's own. */
    rml_element *page = rml_context_document(rml_backend_context(f.backend), 0);
    rml_element *field = rml_element_query_selector(page, "#app input");
    bool left = field && rml_element_focus(field, true);
    press_tab(&f, false);
    test_check(
        first && next && wrapped && back && left && !strcmp(focused_id(&f), "a"),
        "Tab and Shift+Tab keep the focus inside a modal host, wrapping, and "
        "Tab from the editor enters it"
    );
    close_fixture(&f);
    test_remove_ui(folder, "Dialog");
}

/* A host in the kit's own document, or in a document of its own over a place
 * in it, as each Studio area is. */
static void class_frame(host_fixture *f, bool in_document)
{
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    if (in_document) {
        kit_document_begin(k, "area", NULL, false);
    }
    f->host = kit_host(k, f->host_key, NULL);
    kit_style(k, "display", "block");
    kit_style(k, "height", "200dp");
    if (ui_mount_sync(f->mount, f->host, f->listener, f->in.time)) {
        kit_style_sheet(k, "Probe", ui_mount_style(f->mount));
    }
    if (in_document) {
        kit_document_end(k);
    }
    kit_end(k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    f->in.time += 1.0 / 60.0;
}

static void class_frames(host_fixture *f, bool in_document)
{
    for (int i = 0; i < 3; i++) {
        class_frame(f, in_document);
    }
}

/* Scoped RCSS names its host `body`, so the host stands for the page body:
 * a rule under `body.comfortable`, `body:not(.dark)` or a theme's class sees
 * the classes the kit set on its documents, now and as they change. Reduced
 * motion is the context's theme, which a media query reads. */
static void body_classes(const char *root, bool in_document)
{
    char folder[128];
    snprintf(folder, sizeof folder, "%s/ui", root);
    test_write_ui(
        folder,
        "Probe",
        "<div class=\"row\" id=\"density\"></div><div class=\"row\" "
        "id=\"light\"></div>"
        "<div class=\"row\" id=\"theme\"></div><div class=\"row\" "
        "id=\"motion\"></div>\n",
        ".row { display: block; height: 20dp; }\n"
        "body.comfortable #density { height: 30dp; }\n"
        "body:not(.dark) #light { height: 40dp; }\n"
        "body.theme-dracula.dark #theme { height: 50dp; }\n"
        "@media (theme: reduced-motion) { #motion { height: 60dp; } }\n"
    );
    host_fixture f = { .host_key = "probe" };
    open_fixture(&f, root, "Probe");
    class_frames(&f, in_document);
    bool plain = height_of(f.host, "#density") == 20 &&
                 height_of(f.host, "#light") == 40 &&
                 height_of(f.host, "#theme") == 20 &&
                 height_of(f.host, "#motion") == 20;
    kit_set_body_class(f.k, "comfortable", true);
    kit_set_body_class(f.k, "dark", true);
    kit_set_body_class(f.k, "theme-dracula", true);
    class_frames(&f, in_document);
    bool set = height_of(f.host, "#density") == 30 &&
               height_of(f.host, "#light") == 20 &&
               height_of(f.host, "#theme") == 50;
    kit_set_theme(f.k, "reduced-motion", true);
    class_frames(&f, in_document);
    bool motion = height_of(f.host, "#motion") == 60;
    kit_set_theme(f.k, "reduced-motion", false);
    kit_set_body_class(f.k, "comfortable", false);
    kit_set_body_class(f.k, "dark", false);
    kit_set_body_class(f.k, "theme-dracula", false);
    class_frames(&f, in_document);
    bool cleared = height_of(f.host, "#density") == 20 &&
                   height_of(f.host, "#light") == 40 &&
                   height_of(f.host, "#theme") == 20 &&
                   height_of(f.host, "#motion") == 20;
    test_check(
        !ui_mount_error(f.mount)[0] && plain && set && motion && cleared,
        in_document ? "a host in an area's document sees the kit's body "
                      "classes and theme in "
                      "its RCSS, as they change"
                    : "a host in the kit's document sees the kit's body "
                      "classes and theme in "
                      "its RCSS, as they change"
    );
    close_fixture(&f);
    test_remove_ui(folder, "Probe");
}

void test_kit_host()
{
    char root[] = "/tmp/chibi-kit-host-XXXXXX", folder[64];
    assert(mkdtemp(root));
    snprintf(folder, sizeof folder, "%s/ui", root);
    assert(mkdir(folder, 0700) == 0);
    test_write_ui(folder, "Panel", "<div class=\"button\" id=\"go\" data-event-click=\"emit('Go')\">Go</div>\n", ".button { display: block; height: 50dp; }\n");

    host_fixture f = { .host_key = "panel" };
    open_fixture(&f, root, "Panel");
    frame(&f);
    test_check(height_of(f.host, "#go") == 50, "a kit host's content is styled in the frame it first shows");
    for (int i = 0; i < 2; i++) {
        frame(&f);
    }
    kit_rect own;
    test_check(
        !ui_mount_error(f.mount)[0] && height_of(f.host, "#go") == 50 &&
            kit_find(f.k, "app/own", &own) && own.h == 20,
        "a kit host shows a ui asset whose RCSS applies only inside it"
    );

    click(&f, rml_element_query_selector(f.host, "#go"));
    frame(&f);
    ui_activation a;
    bool taken = ui_mount_take(f.mount, &a) && !strcmp(a.event, "Go") &&
                 !strcmp(a.id, "go");
    test_check(taken, "an event hosted content emits reaches its mount");

    f.host_key = "moved";
    for (int i = 0; i < 3; i++) {
        frame(&f);
    }
    test_check(height_of(f.host, "#go") == 50, "a mount follows its host to a new element");

    close_fixture(&f);
    test_remove_ui(folder, "Panel");
    popup_navigation(root);
    option_groups(root);
    modal_focus(root);
    body_classes(root, false);
    body_classes(root, true);
    rmdir(folder);
    rmdir(root);
}
