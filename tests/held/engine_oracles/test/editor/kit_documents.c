/* Documents the kit shows over the boxes it places (kit_document_begin,
 * ui/iframe.h): each lays out apart, follows its box, takes the kit's
 * styles and body classes, closes with its box, and shares the context's
 * focus, keys and pointer with the rest; the layer stays above them all. */
#include "editor/ui/kit.h"
#include "test/suite_internal.h"
#include "ui/iframe_profile.h"
#include "ui/popover.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBED 8

/* Each document's layouts, counted by a probe element inside it: a
   document lays out all of its elements at once. */
static rml_element *probed[PROBED];
static int layouts[PROBED];
static bool profile_enabled;
static int profile_frame;
static int probe_updates[PROBED][IFRAME_PROFILE_PHASES];
static int probe_layouts[PROBED][IFRAME_PROFILE_PHASES];

static int probe_index(rml_element *element)
{
    rml_element *document = rml_element_owner_document(element);
    for (int i = 0; i < PROBED; i++) {
        if (!probed[i] || probed[i] == document) {
            probed[i] = document;
            return i;
        }
    }
    assert(!"too many probed documents");
    return 0;
}

static void count_update(rml_element *element, void *)
{
    if (profile_enabled) {
        probe_updates[probe_index(element)][iframe_profile_current()]++;
    }
}

static void count_layout(rml_element *element, void *)
{
    int i = probe_index(element);
    layouts[i]++;
    if (profile_enabled) {
        probe_layouts[i][iframe_profile_current()]++;
    }
}

static const rml_element_hooks PROBE = { .update = count_update, .layout = count_layout };

static void print_profile()
{
    static const char *names[] = { "outside", "pre", "context", "post" };
    for (int p = 0; p < IFRAME_PROFILE_PHASES; p++) {
        iframe_profile_sample s = iframe_profile_get(p);
        printf(
            "IFRAME_PROFILE {\"frame\":%d,\"phase\":\"%s\",\"calls\":%d,"
            "\"started\":%.9f,\"seconds\":%.9f,\"holder_updates\":%d,"
            "\"holder_seconds\":%.9f,\"boxes\":%d,\"heights\":%d,"
            "\"passes\":%d,\"probes\":[",
            profile_frame,
            names[p],
            s.calls,
            s.started,
            s.seconds,
            s.holder_updates,
            s.holder_seconds,
            s.boxes,
            s.heights,
            s.passes
        );
        for (int i = 0; i < PROBED && probed[i]; i++) {
            printf(
                "%s{\"document\":%d,\"updates\":%d,\"layouts\":%d}",
                i ? "," : "",
                i,
                probe_updates[i][p],
                probe_layouts[i][p]
            );
        }
        printf("]");
        if (s.context_calls) {
            double *t = s.context_seconds;
            printf(
                ",\"context_calls\":%d,\"model_update_seconds\":%.9f,\"root_"
                "update_"
                "seconds\":%.9f,"
                "\"layout_position_seconds\":%.9f,\"release_documents_"
                "seconds\":%.9f,\"other_"
                "context_seconds\":%.9f",
                s.context_calls,
                t[RML_UPDATE_MODELS],
                t[RML_UPDATE_ROOT],
                t[RML_UPDATE_LAYOUT],
                t[RML_UPDATE_RELEASE],
                s.seconds - t[RML_UPDATE_MODELS] - t[RML_UPDATE_ROOT] -
                    t[RML_UPDATE_LAYOUT] - t[RML_UPDATE_RELEASE]
            );
        }
        puts("}");
    }
}

static int layouts_of(rml_element *document)
{
    for (int i = 0; i < PROBED; i++) {
        if (probed[i] == document) {
            return layouts[i];
        }
    }
    return 0;
}

typedef struct documents_fixture
{
    rml_backend *backend;
    kit *k;
    kit_frame_input in;
    const char *text_a, *text_b;
    bool open_menu, third, nested;
    char field[64];
    kit_rect place_a, place_bar;
} documents_fixture;

static void probe(kit *k, const char *key)
{
    kit_leaf(k, key, "doc-probe", NULL);
    kit_style(k, "position", "absolute");
}

/* An authored popover and its trigger, which a kit menu holds. */
static void nested_popover(rml_element *host)
{
    if (rml_element_child_count(host)) {
        return;
    }
    rml_element_set_inner_rml(
        host,
        "<button id=\"inner-trigger\" popovertarget=\"inner\" "
        "style=\"display: block; width: 60dp; height: 20dp;\">Inner</button>"
        "<div id=\"inner\" popover class=\"popup\" style=\"width: 80dp; "
        "height: 40dp;\">Inside</div>"
    );
}

/* Two areas side by side, A holding a field, a menu trigger at its right
   edge and a label, B a label; under them a bar that fits its content. */
static void frame(documents_fixture *f)
{
    if (profile_enabled) {
        iframe_profile_enable(true);
        memset(probe_updates, 0, sizeof probe_updates);
        memset(probe_layouts, 0, sizeof probe_layouts);
        profile_frame++;
    }
    kit *k = f->k;
    kit_begin(k, &f->in);
    kit_root(k, "app");
    probe(k, "probe");
    kit_push(k, "row", NULL, NULL);
    kit_style(k, "display", "flex");
    kit_style(k, "height", "300dp");
    kit_document_begin(k, "a", NULL, false);
    kit_style(k, "width", "200dp");
    f->place_a = kit_item_rect(k);
    kit_push(k, "a", NULL, NULL);
    kit_style(k, "position", "absolute");
    kit_style(k, "left", "0dp");
    kit_style(k, "top", "0dp");
    kit_style(k, "right", "0dp");
    kit_style(k, "bottom", "0dp");
    kit_style(k, "overflow", "hidden");
    probe(k, "probe");
    kit_text_field(k, "field", f->field, sizeof f->field, NULL, 0);
    kit_label(k, "text", NULL, f->text_a);
    kit_leaf(k, "trigger", "button", NULL);
    kit_style(k, "position", "absolute");
    kit_style(k, "left", "180dp");
    kit_style(k, "top", "100dp");
    kit_style(k, "width", "20dp");
    kit_style(k, "height", "20dp");
    if (f->open_menu) {
        kit_popup_open(k, "menu");
        f->open_menu = false;
    }
    if (kit_popup_begin(k, "menu", "menu")) {
        kit_menu_item(k, "one", &(kit_menu_item_desc){ .label = "One", .checked = -1 });
        kit_menu_item(k, "two", &(kit_menu_item_desc){ .label = "Two", .checked = -1, .disabled = true });
        if (f->nested) {
            nested_popover(kit_host(k, "authored", NULL));
        }
        kit_popup_end(k);
    }
    kit_pop(k);
    kit_document_end(k);
    kit_document_begin(k, "b", NULL, false);
    kit_style(k, "width", "200dp");
    probe(k, "probe-b");
    kit_label(k, "text-b", NULL, f->text_b);
    kit_document_end(k);
    if (f->third) {
        kit_document_begin(k, "c", NULL, false);
        kit_style(k, "width", "100dp");
        kit_document_end(k);
    }
    kit_pop(k);
    kit_document_begin(k, "bar", NULL, true);
    f->place_bar = kit_item_rect(k);
    probe(k, "probe-bar");
    kit_leaf(k, "content", NULL, NULL);
    kit_style(k, "height", "30dp");
    kit_document_end(k);
    kit_end(k);
    rml_backend_update(f->backend, 1.0 / 60.0);
    if (profile_enabled) {
        print_profile();
    }
    f->in.time += 1.0 / 60.0;
}

static void frames(documents_fixture *f, int count)
{
    for (int i = 0; i < count; i++) {
        frame(f);
    }
}

static rml_element *element_at(documents_fixture *f, float x, float y)
{
    return rml_context_element_at_point(rml_backend_context(f->backend), (rml_vec2){ x, y }, NULL);
}

static rml_element *document_at(documents_fixture *f, float x, float y)
{
    rml_element *hit = element_at(f, x, y);
    return hit ? rml_element_owner_document(hit) : NULL;
}

static void pointer(documents_fixture *f, rml_input_type type, float x, float y)
{
    rml_backend_process_input(f->backend, &(rml_input){ .type = type, .x = x, .y = y });
}

static void click_at(documents_fixture *f, float x, float y)
{
    pointer(f, RML_INPUT_POINTER_MOVE, x, y);
    pointer(f, RML_INPUT_POINTER_DOWN, x, y);
    pointer(f, RML_INPUT_POINTER_UP, x, y);
    frame(f);
}

static bool same_rect(kit_rect a, kit_rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* A change inside one document lays out that document only. */
static void layout_apart(documents_fixture *f)
{
    rml_element *a = document_at(f, 100, 150), *b = document_at(f, 300, 150);
    rml_element *page = rml_context_document(rml_backend_context(f->backend), 0);
    int before[3] = { layouts_of(a), layouts_of(b), layouts_of(page) };
    f->text_a = "A, edited to wrap differently";
    frame(f);
    int after[3] = { layouts_of(a), layouts_of(b), layouts_of(page) };
    f->text_b = "B, edited";
    frame(f);
    test_check(
        a && b && a != b && a != page && after[0] > before[0] &&
            after[1] == before[1] && after[2] == before[2] &&
            layouts_of(a) == after[0] && layouts_of(b) > after[1] &&
            layouts_of(page) == after[2],
        "a text change inside one kit document lays out that document only"
    );
}

/* A document takes its box, and a fitting box its document's height. */
static void placement(documents_fixture *f)
{
    kit_rect a, bar;
    bool found = kit_find(f->k, "app/row/a", &a) &&
                 kit_find(f->k, "app/content", &bar);
    test_check(
        found && same_rect(a, f->place_a) && a.w == 200 && a.h == 300 &&
            f->place_bar.y == 300 && f->place_bar.h == 30 && bar.y == 300,
        "a kit document lies over its box, and a fitting box takes the "
        "height of its document's content"
    );
    rml_backend_resize(f->backend, 700, 480, 1);
    f->in.width = 700;
    frame(f);
    test_check(
        kit_find(f->k, "app/content", &bar) && bar.x == 0 && bar.w == 700,
        "a kit document follows its box in the frame the window resizes"
    );
}

/* A menu opened from the right edge of A reaches over B, unclipped, and
   stays above B after B takes focus. */
static void popup_above(documents_fixture *f)
{
    f->open_menu = true;
    frames(f, 3);
    kit_rect menu;
    bool open = kit_find(f->k, "layer/@menu", &menu);
    float x = 260, y = menu.y + menu.h * 0.5f;
    rml_element *over = element_at(f, x, y);
    rml_element *layer = rml_element_owner_document(over);
    bool above = open && menu.x + menu.w > 260 && layer &&
                 !strcmp(rml_element_id(layer), "layer");
    rml_element *b = document_at(f, 300, 280);
    rml_element_focus(b, false);
    frame(f);
    test_check(
        above && document_at(f, x, y) == layer,
        "a popup over another kit document hit-tests above it, unclipped by "
        "its own, even once that document takes focus"
    );
    pointer(f, RML_INPUT_POINTER_DOWN, 300, 280);
    pointer(f, RML_INPUT_POINTER_UP, 300, 280);
    frames(f, 2);
}

static void escape(documents_fixture *f)
{
    rml_backend_process_input(
        f->backend,
        &(rml_input){ .type = RML_INPUT_KEY_PRESS, .key = RML_INPUT_KEY_ESCAPE }
    );
    rml_backend_process_input(
        f->backend,
        &(rml_input){ .type = RML_INPUT_KEY_RELEASE, .key = RML_INPUT_KEY_ESCAPE }
    );
    frames(f, 2);
}

/* A kit menu's disabled row never takes the highlight under the pointer,
   which leaves none highlighted, as popover.c runs the menu. */
static void disabled_row(documents_fixture *f)
{
    rml_context *context = rml_backend_context(f->backend);
    f->open_menu = true;
    frames(f, 3);
    kit_rect one, two;
    bool rows = kit_find(f->k, "layer/@menu/one", &one) &&
                kit_find(f->k, "layer/@menu/two", &two);
    pointer(f, RML_INPUT_POINTER_MOVE, one.x + 4, one.y + one.h * 0.5f);
    frames(f, 1);
    bool first = test_query(context, ".popup.menu .menu-item[data-highlighted]") != NULL;
    pointer(f, RML_INPUT_POINTER_MOVE, two.x + 4, two.y + two.h * 0.5f);
    frames(f, 1);
    test_check(
        rows && first && !test_query(context, ".popup.menu .menu-item[data-highlighted]"),
        "a kit menu's disabled row takes no highlight under the pointer"
    );
    escape(f);
}

/* Kit popups and authored popovers share one stack (ui/popover.h): an
   authored popover a kit menu holds stacks above it, so Escape closes the
   popover first, then the menu, and a press outside both closes both. */
static void one_stack(documents_fixture *f)
{
    disabled_row(f);
    rml_context *context = rml_backend_context(f->backend);
    f->nested = true;
    f->open_menu = true;
    frames(f, 3);
    rml_element *trigger = test_query(context, "#inner-trigger");
    kit_rect at = { 0 };
    if (trigger) {
        rml_vec2 offset = rml_element_absolute_offset(trigger, RML_BOX_BORDER);
        at = (kit_rect){ offset.x + 4, offset.y + 4 };
        click_at(f, at.x, at.y);
        frames(f, 2);
    }
    rml_element *inner = test_query(context, "#inner");
    bool stacked = inner && popover_open(inner) &&
                   kit_popup_is_open(f->k, "menu") && popover_top(context) == inner;
    escape(f);
    bool first = inner && !popover_open(inner) && kit_popup_is_open(f->k, "menu");
    escape(f);
    bool second = !kit_popup_is_open(f->k, "menu");
    test_check(
        stacked && first && second,
        "an authored popover a kit menu holds stacks above it: Escape closes "
        "the popover, then the menu"
    );
    f->open_menu = true;
    frames(f, 3);
    click_at(f, at.x, at.y);
    frames(f, 2);
    /* The first menu's popover may still be fading out beside this one. */
    inner = popover_top(context);
    bool both = inner && !strcmp(rml_element_id(inner), "inner") &&
                kit_popup_is_open(f->k, "menu");
    click_at(f, 300, 280);
    frames(f, 2);
    test_check(
        both && !popover_open(inner) && !kit_popup_is_open(f->k, "menu") &&
            !popover_top(context),
        "a press outside a kit menu and the authored popover it holds closes "
        "both"
    );
    f->nested = false;
    frames(f, 2);
}

/* Typing reaches a field in one document; a press in another takes the
   focus there, which commits the field, and the page under the areas never
   covers them. */
static void focus_across(documents_fixture *f)
{
    kit_rect field;
    assert(kit_find(f->k, "app/row/a/field", &field));
    click_at(f, field.x + 4, field.y + field.h * 0.5f);
    bool keyboard = kit_wants_keyboard(f->k);
    rml_backend_process_input(
        f->backend,
        &(rml_input){ .type = RML_INPUT_TEXT_INPUT, .text = "hi", .text_size = 2 }
    );
    frame(f);
    click_at(f, 300, 200);
    frame(f);
    rml_element *focus = rml_context_focus_element(rml_backend_context(f->backend));
    bool moved = !kit_wants_keyboard(f->k) &&
                 focus == document_at(f, 300, 200) && !strcmp(f->field, "hi");
    click_at(f, 500, 150);
    test_check(
        keyboard && moved && document_at(f, 100, 150) != document_at(f, 500, 150),
        "a field in one kit document takes typing, a press in another moves "
        "the focus there and commits it, and focusing the page never raises "
        "it over them"
    );
}

/* The UI Inspector picks a document's element by point and names it by
   its kit path, as the kit's own. */
static void inspect(documents_fixture *f)
{
    kit_rect text;
    assert(kit_find(f->k, "app/row/text-b", &text));
    rml_element *picked = element_at(f, text.x + 2, text.y + text.h * 0.5f);
    char path[256] = "";
    bool native = picked && kit_element_path(f->k, picked, path, sizeof path);
    test_check(
        native && !strncmp(path, "app/row/text-b", 14),
        "the UI Inspector picks an element in a kit document and names it by "
        "its kit path"
    );
}

/* A document opened later takes the body classes set before; one no
   longer emitted closes. */
static void lifetime(documents_fixture *f)
{
    rml_context *context = rml_backend_context(f->backend);
    int count = rml_context_document_count(context);
    kit_set_body_class(f->k, "comfortable", true);
    f->third = true;
    frames(f, 2);
    rml_element *c = document_at(f, 450, 150);
    bool opened = rml_context_document_count(context) == count + 1 && c &&
                  rml_element_is_class_set(c, "comfortable");
    f->third = false;
    frames(f, 2);
    test_check(
        opened && rml_context_document_count(context) == count,
        "a kit document opened later takes the body classes, and closes once "
        "no longer emitted"
    );
}

void test_kit_documents()
{
    const char *profile = getenv("CHIBI_IFRAME_PROFILE");
    profile_enabled = profile && !strcmp(profile, "1");
    profile_frame = 0;
    documents_fixture f = { .text_a = "A", .text_b = "B" };
    f.backend = rml_backend_create(640, 480, 1);
    /* After the backend starts RmlUi, whose factory holds instancers. */
    rml_instancer *instancer = rml_instancer_create(&PROBE);
    rml_register_element_instancer("doc-probe", instancer);
    f.k = test_kit_create(rml_backend_context(f.backend), "studio.rml");
    f.in = (kit_frame_input){ .dp_ratio = 1, .width = 640, .height = 480 };
    frames(&f, 3);
    layout_apart(&f);
    placement(&f);
    popup_above(&f);
    one_stack(&f);
    focus_across(&f);
    inspect(&f);
    lifetime(&f);
    if (profile_enabled) {
        rml_backend_stats stats = rml_backend_get_stats(f.backend);
        printf(
            "IFRAME_BACKEND {\"frames\":%d,\"context_updates\":%llu,"
            "\"render_captures\":%llu,\"render_passes\":%llu,"
            "\"geometry_compiles\":%llu}\n",
            profile_frame,
            (unsigned long long)stats.context_updates,
            (unsigned long long)stats.render_captures,
            (unsigned long long)stats.render_passes,
            (unsigned long long)stats.geometry_compiles
        );
    }
    kit_destroy(f.k);
    rml_backend_destroy(f.backend);
    rml_instancer_destroy(instancer);
    memset(probed, 0, sizeof probed);
    memset(layouts, 0, sizeof layouts);
    iframe_profile_enable(false);
    profile_enabled = false;
}
