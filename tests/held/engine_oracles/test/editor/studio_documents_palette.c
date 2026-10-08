/* The palette and pickers (workstream C4): Palette.rml over the `palette`
 * source, its keys, ranking, highlighting and window; an asset field's
 * Picker.rml under its field; and the refactor popover. */

#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor.h"
#include "platform/cute.h"
#include "support/array.h"
#include "test/editor/studio_documents.h"
#include "test/suite_internal.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The shown elements `selector` matches in Studio's documents, up to
   `max`: data-for and data-if keep templates and hidden rows. */
static int shown(studio *s, const char *selector, rml_element **out, int max)
{
    rml_context *context = rml_backend_context(s->backend);
    int count = 0;
    for (int d = 0; d < rml_context_document_count(context); d++) {
        rml_element *found[256];
        int n = rml_element_query_selector_all(rml_context_document(context, d), selector, found, 256);
        for (int i = 0; i < n && count < max; i++) {
            if (rml_element_is_visible(found[i], true)) {
                out[count++] = found[i];
            }
        }
    }
    return count;
}

static rml_element *shown_one(studio *s, const char *selector)
{
    rml_element *found = NULL;
    return shown(s, selector, &found, 1) ? found : NULL;
}

/* An element's text with its markup's tags taken out. */
static void plain_text(rml_element *el, char *out, size_t size)
{
    char rml[512] = "";
    if (el) {
        rml_element_inner_rml(el, rml, sizeof rml);
    }
    size_t n = 0;
    bool tag = false;
    for (const char *c = rml; *c && n + 1 < size; c++) {
        tag = *c == '<' || (tag && *c != '>');
        if (!tag && *c != '>') {
            out[n++] = *c;
        }
    }
    out[n] = '\0';
    static const struct
    {
        const char *entity, *text;
    } ENTITIES[] = { { "&nbsp;", " " },
                     { "&quot;", "\"" },
                     { "&amp;", "&" },
                     { "&lt;", "<" },
                     { "&gt;", ">" } };
    for (size_t e = 0; e < sizeof ENTITIES / sizeof *ENTITIES; e++) {
        size_t length = strlen(ENTITIES[e].entity);
        for (char *at; (at = strstr(out, ENTITIES[e].entity));) {
            *at = ENTITIES[e].text[0];
            memmove(at + 1, at + length, strlen(at + length) + 1);
        }
    }
}

/* The result at `place` (from 0), when the window shows it. */
static rml_element *result_at(studio *s, int place)
{
    char selector[64];
    snprintf(selector, sizeof selector, "[role=option][aria-posinset=\"%d\"]", place + 1);
    return shown_one(s, selector);
}

static void search_for(studio_fixture *f, const char *query)
{
    studio_command_run(f->studio, STUDIO_COMMAND_SEARCH);
    frames(f, 2);
    if (query[0]) {
        text_event(query);
    }
    frames(f, 2);
}

static void tap_mod(SDL_Scancode key, SDL_Keymod mod)
{
    key_event_mod(key, true, mod, false);
    key_event_mod(key, false, mod, false);
}

/* Whether `el` shows whole inside the palette's List. */
static bool in_view(studio *s, rml_element *el)
{
    rml_element *list = studio_element(s, "#palette-list");
    if (!el || !list) {
        return false;
    }
    float top = rml_element_absolute_offset(el, RML_BOX_BORDER).y,
          bottom = top + rml_element_box_size(el, RML_BOX_BORDER).y,
          view = rml_element_absolute_offset(list, RML_BOX_PADDING).y;
    return top >= view - 0.5f &&
           bottom <= view + rml_element_client_height(list) + 0.5f;
}

/* ⌘K opens the palette with its field focused; its List shows a window of
   every result, which keys walk: Down, Up, Ctrl+N and Ctrl+P one at a time,
   PageDown a view, End and Home to either end, each scrolled into view;
   Tab keeps the focus in it and Escape closes it. */
static void palette_keys_and_window()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    memset(s->prefs.recent, 0, sizeof s->prefs.recent);
    frames(&f, 2);
    search_for(&f, "");
    rml_element *search = studio_element(s, "#palette-search");
    rml_context *context = rml_backend_context(s->backend);
    bool focused = search && rml_context_focus_element(context) == search &&
                   studio_palette_is_open(s);
    rml_element *rows[256];
    int results = acount(s->palette.places),
        window = shown(s, ".palette-row", rows, 256);
    bool windowed = results > 100 && window > 5 && window < 40;
    tap(SDL_SCANCODE_DOWN);
    frames(&f, 2);
    bool down = s->palette.highlight == 1 && result_at(s, 1) &&
                rml_element_has_attribute(result_at(s, 1), "data-highlighted");
    tap_mod(SDL_SCANCODE_N, SDL_KMOD_CTRL);
    frames(&f, 2);
    bool next = s->palette.highlight == 2;
    tap_mod(SDL_SCANCODE_P, SDL_KMOD_CTRL);
    tap(SDL_SCANCODE_UP);
    frames(&f, 2);
    bool up = s->palette.highlight == 0;
    tap(SDL_SCANCODE_PAGEDOWN);
    frames(&f, 2);
    int page = s->palette.highlight;
    bool paged = page > 3 && in_view(s, result_at(s, page));
    tap(SDL_SCANCODE_END);
    frames(&f, 3);
    bool end = s->palette.highlight == results - 1 &&
               in_view(s, result_at(s, results - 1)) &&
               rml_element_has_attribute(result_at(s, results - 1), "data-highlighted");
    window = shown(s, ".palette-row", rows, 256);
    windowed &= window < 40;
    tap(SDL_SCANCODE_HOME);
    frames(&f, 3);
    bool home = s->palette.highlight == 0 && in_view(s, result_at(s, 0));
    tap(SDL_SCANCODE_TAB);
    frames(&f, 2);
    bool trapped = studio_palette_is_open(s) &&
                   rml_context_focus_element(context) ==
                       studio_element(s, "#palette-search");
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    bool closed = !studio_palette_is_open(s) && !studio_element(s, "#palette-search");
    if (!(focused && windowed && down && next && up && paged && end && home &&
          trapped && closed)) {
        printf(
            "  focused %d windowed %d (%d of %d) down %d next %d up %d paged "
            "%d (%d) end %d home %d trapped %d closed %d\n",
            focused,
            windowed,
            window,
            results,
            down,
            next,
            up,
            paged,
            page,
            end,
            home,
            trapped,
            closed
        );
    }
    test_check(focused && windowed, "⌘K opens the palette focused, its List windowing every result");
    test_check(down && next && up && paged && end && home, "Up, Down, Ctrl+N, Ctrl+P, PageDown, End and Home walk the results, each scrolled into view");
    test_check(trapped && closed, "Tab keeps the focus in the palette and Escape closes it");
    fixture_free(&f);
}

/* Typing ranks the results under their groups' headings with the matched
   letters in bold, spaces kept; a described result takes a second line; a
   query nothing matches says so; rows a new query brings under a still
   pointer leave the highlight on the best match (cmdk); a press outside
   closes it. */
static void palette_ranks_and_marks()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    memset(s->prefs.recent, 0, sizeof s->prefs.recent);
    frames(&f, 2);
    search_for(&f, "wire sty");
    char label[128] = "", heading[64] = "";
    rml_element *first = result_at(s, 0);
    plain_text(first ? rml_element_query_selector(first, ".label") : NULL, label, sizeof label);
    plain_text(shown_one(s, ".cmd-group"), heading, sizeof heading);
    char rml[256] = "";
    if (first) {
        rml_element_inner_rml(rml_element_query_selector(first, ".label"), rml, sizeof rml);
    }
    bool ranked = !strcmp(label, "Wire Style") &&
                  !strcmp(heading, "Settings") && strstr(rml, "<b>");
    bool described = first && rml_element_has_attribute(first, "data-described");
    float tall = first ? rml_element_box_size(first, RML_BOX_BORDER).y : 0;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);

    search_for(&f, "toggle");
    rml_element *plain = result_at(s, 0);
    float short_row = plain ? rml_element_box_size(plain, RML_BOX_BORDER).y : 0;
    described &= plain && !rml_element_has_attribute(plain, "data-described") &&
                 tall > short_row;
    /* The pointer rests on the second result; typing brings others under it. */
    rml_element *second = result_at(s, 1);
    float ratio = studio_pixel_ratio(s);
    if (second) {
        rml_vec2 at = rml_element_absolute_offset(second, RML_BOX_BORDER);
        mouse_move(s, (at.x + 20) / ratio, (at.y + 8) / ratio);
    }
    frames(&f, 2);
    bool hovered = s->palette.highlight == 1;
    text_event(" p");
    frames(&f, 2);
    bool still = s->palette.highlight == 0;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);

    search_for(&f, "zzqqzz");
    char empty[128] = "";
    plain_text(shown_one(s, ".cmd-empty"), empty, sizeof empty);
    bool none = !strcmp(empty, "No results for \"zzqqzz\"") &&
                !acount(s->palette.places);
    rml_element *backdrop = shown_one(s, ".command-backdrop");
    if (backdrop) {
        mouse_move(s, 8, 600);
        frames(&f, 1);
        graph_drag_button(s, 8, 600, true);
        graph_drag_button(s, 8, 600, false);
    }
    frames(&f, 2);
    bool outside = !studio_palette_is_open(s);
    if (!(ranked && described && hovered && still && none && outside)) {
        printf(
            "  ranked %d '%s' '%s' described %d (%g, %g) hovered %d still %d "
            "none %d '%s' outside %d\n",
            ranked,
            label,
            heading,
            described,
            tall,
            short_row,
            hovered,
            still,
            none,
            empty,
            outside
        );
    }
    test_check(ranked, "the palette ranks a query's results under their group, the matched letters bold and spaces kept");
    test_check(described, "a described result takes a second line");
    test_check(hovered && still, "the pointer highlights a result, and rows a query brings under a still pointer leave the best match highlighted");
    test_check(none && outside, "a query nothing matches says so, and a press outside closes the palette");
    fixture_free(&f);
}

/* The Vault's ui document, as its GameUi names it. */
static const char *vault_document(studio_fixture *f)
{
    return GAME_GameUi_get_const(f->scene->world, f->scene->actors[0].entity)->document;
}

static rml_element *document_field(studio *s)
{
    return shown_one(s, "[data-ui=Properties] .asset-field");
}

static void open_picker(studio_fixture *f)
{
    rml_element *field = document_field(f->studio);
    if (field) {
        press(f->studio, field);
    }
    frames(f, 3);
}

/* An asset field opens a picker under it, its current asset highlighted;
   typing ranks the choices and Enter assigns one as one Undo step; a
   name no asset has is offered as Use "name"; the field again, or Escape,
   closes it. */
static void asset_field_picks()
{
    studio_fixture f = vault_open();
    studio *s = f.studio;
    select_instance(f.scene, 0);
    studio_show_editor(s, STUDIO_EDITOR_PROPERTIES);
    s->shown_actor = s->scene->selected;
    s->tab = STUDIO_TAB_OBJECT;
    snprintf(s->property_search, sizeof s->property_search, "Document");
    scene_history_clear(f.scene->history);
    frames(&f, 6);
    char was[128];
    snprintf(was, sizeof was, "%s", vault_document(&f));
    open_picker(&f);
    rml_element *field = document_field(s),
                *popup = shown_one(s, "#picker[data-open]");
    rml_element *lit = shown_one(s, "#picker [data-highlighted]");
    char current[128] = "";
    plain_text(lit ? rml_element_query_selector(lit, ".label") : NULL, current, sizeof current);
    float field_bottom = field
                           ? rml_element_absolute_offset(field, RML_BOX_BORDER).y +
                                 rml_element_box_size(field, RML_BOX_BORDER).y
                           : 0;
    bool opened = popup && s->palette.anchored && !strcmp(current, was) &&
                  rml_element_absolute_offset(popup, RML_BOX_BORDER).y >=
                      field_bottom - 0.5f;
    text_event("non");
    frames(&f, 2);
    char first[128] = "";
    plain_text(
        result_at(s, 0) ? rml_element_query_selector(result_at(s, 0), ".label") : NULL,
        first,
        sizeof first
    );
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool assigned = !strcmp(first, "None") && !strcmp(vault_document(&f), "") &&
                    scene_history_can_undo(f.scene->history) && !s->palette.client;
    studio_history(s, false);
    frames(&f, 2);
    bool undone = !strcmp(vault_document(&f), was);

    open_picker(&f);
    text_event("Brandnew");
    frames(&f, 2);
    char typed[128] = "";
    plain_text(
        result_at(s, 0) ? rml_element_query_selector(result_at(s, 0), ".label") : NULL,
        typed,
        sizeof typed
    );
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool named = !strcmp(typed, "Use \"Brandnew\"") &&
                 !strcmp(vault_document(&f), "Brandnew");

    open_picker(&f);
    bool reopened = s->palette.client == STUDIO_PALETTE_ASSETS;
    open_picker(&f);
    bool toggled = reopened && !s->palette.client &&
                   !shown_one(s, "#picker[data-open]");
    open_picker(&f);
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 3);
    bool escaped = !s->palette.client;
    if (!(opened && assigned && undone && named && toggled && escaped)) {
        printf(
            "  opened %d ('%s' of '%s') assigned %d ('%s', %s) undone %d named "
            "%d ('%s') toggled %d escaped %d\n",
            opened,
            current,
            was,
            assigned,
            first,
            vault_document(&f),
            undone,
            named,
            typed,
            toggled,
            escaped
        );
    }
    test_check(opened, "an asset field opens its picker under it with its current asset highlighted");
    test_check(assigned && undone, "typing ranks a picker's choices and Enter assigns one as one Undo step");
    test_check(named, "a picker offers a name no asset has yet as Use \"name\"");
    test_check(toggled && escaped, "the field again or Escape closes its picker");
    fixture_free(&f);
}

/* Add Component lists the components the prototype lacks under the
   Properties tab each shows in, the current tab's first, and ranks them as
   typed. */
static void add_component_groups()
{
    studio_fixture f = side_open();
    studio *s = f.studio;
    assert(app_scene_authoring_open_prototype(f.scene, "Vault") == 0);
    frames(&f, 3);
    s->tab = STUDIO_TAB_OBJECT;
    studio_add_component_open(s);
    frames(&f, 3);
    char heading[64] = "";
    plain_text(shown_one(s, ".cmd-group"), heading, sizeof heading);
    rml_element *search = studio_element(s, "#palette-search");
    char placeholder[64] = "";
    if (search) {
        rml_element_attribute(search, "placeholder", placeholder, sizeof placeholder);
    }
    bool listed = s->palette.client == STUDIO_PALETTE_COMPONENTS &&
                  acount(s->palette.places) > 3 && !strcmp(heading, "Object") &&
                  !strcmp(placeholder, "Search components");
    text_event("health");
    frames(&f, 2);
    char first[64] = "";
    plain_text(
        result_at(s, 0) ? rml_element_query_selector(result_at(s, 0), ".label") : NULL,
        first,
        sizeof first
    );
    bool ranked = strstr(first, "Health") != NULL;
    tap(SDL_SCANCODE_ESCAPE);
    frames(&f, 2);
    if (!(listed && ranked)) {
        printf(
            "  listed %d ('%s', '%s', %d) ranked %d ('%s')\n",
            listed,
            heading,
            placeholder,
            acount(s->palette.places),
            ranked,
            first
        );
    }
    test_check(listed && ranked, "Add Component groups what the prototype lacks by tab, the current tab's first, and ranks it as typed");
    fixture_free(&f);
}

/* A copy of the test project, which renames rewrite. */
static void copy_project(char *root, size_t size)
{
    snprintf(root, size, "/tmp/chibi-refactor-XXXXXX");
    assert(mkdtemp(root));
    char command[512];
    snprintf(command, sizeof command, "cp -R %s/. %s", PROJECT, root);
    assert(system(command) == 0);
}

static void remove_project(const char *root)
{
    char command[512];
    snprintf(command, sizeof command, "rm -rf %s", root);
    assert(system(command) == 0);
}

static int asset_named(studio *s, int kind, const char *name)
{
    app_scene_authoring_refresh_asset_index(s->scene);
    return reference_index_find_name(app_scene_authoring_asset_index(s->scene), kind, name);
}

/* The refactor popover: renaming an asset shows it at the pointer with the
   name selected; Enter on a new name lists each asset it rewrites, Enter
   again applies it with a toast whose Undo puts every file back; deleting
   an asset still used lists its uses and Delete deletes it. */
static void refactor_renames_and_deletes()
{
    char root[64], scene[128];
    copy_project(root, sizeof root);
    snprintf(scene, sizeof scene, "%s/content/scenes/Vault.kdl", root);
    studio_fixture f = scene_open(root, scene, "Teller");
    studio *s = f.studio;
    frames(&f, 3);
    mouse_move(s, 300, 200);
    frames(&f, 1);
    studio_refactor_rename_asset(s, asset_named(s, REFERENCE_ASSET_UI, "Bank"));
    frames(&f, 3);
    rml_element *popover = shown_one(s, "#refactor[data-open]");
    rml_element *field = studio_element(s, "#refactor-name");
    rml_context *context = rml_backend_context(s->backend);
    char title[64] = "";
    plain_text(popover ? rml_element_query_selector(popover, ".menu-group") : NULL, title, sizeof title);
    bool opened = popover && field && rml_context_focus_element(context) == field &&
                  !strcmp(title, "Bank");
    rml_element *apply = popover ? rml_element_query_selector(popover, ".menu-item")
                                 : NULL;
    bool waits = apply && rml_element_has_attribute(apply, "data-disabled");
    text_event("Lobby");
    frames(&f, 2);
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    rml_element *rows[16];
    int listed = shown(s, "#refactor .refactor-row", rows, 16);
    char row[256] = "";
    plain_text(listed ? rows[0] : NULL, row, sizeof row);
    apply = popover ? rml_element_query_selector(popover, ".menu-item") : NULL;
    bool planned = listed > 0 && apply &&
                   !rml_element_has_attribute(apply, "data-disabled") &&
                   asset_named(s, REFERENCE_ASSET_UI, "Bank") >= 0;
    tap(SDL_SCANCODE_RETURN);
    frames(&f, 3);
    bool renamed = asset_named(s, REFERENCE_ASSET_UI, "Lobby") >= 0 &&
                   asset_named(s, REFERENCE_ASSET_UI, "Bank") < 0 &&
                   !s->refactor.mode && s->refactor.undo_toast &&
                   !shown_one(s, "#refactor[data-open]");
    rml_element *undo = studio_element(s, ".toast-action");
    if (undo) {
        rml_element_click(undo);
    }
    frames(&f, 3);
    bool restored = asset_named(s, REFERENCE_ASSET_UI, "Bank") >= 0 &&
                    asset_named(s, REFERENCE_ASSET_UI, "Lobby") < 0;

    studio_refactor_delete_asset(s, asset_named(s, REFERENCE_ASSET_UI, "Bank"));
    frames(&f, 3);
    listed = shown(s, "#refactor .refactor-row", rows, 16);
    plain_text(listed ? rows[0] : NULL, row, sizeof row);
    apply = shown_one(s, "#refactor .menu-item");
    char label[32] = "";
    plain_text(apply ? rml_element_query_selector(apply, ".label") : NULL, label, sizeof label);
    bool asked = listed > 0 && strstr(row, "Vault") && !strcmp(label, "Delete") &&
                 asset_named(s, REFERENCE_ASSET_UI, "Bank") >= 0;
    if (apply) {
        press(s, apply);
    }
    frames(&f, 3);
    /* Vault still names Bank, which the index keeps without a file. */
    int bank = asset_named(s, REFERENCE_ASSET_UI, "Bank");
    const reference_asset
        *gone = bank >= 0
                  ? reference_index_asset_at(app_scene_authoring_asset_index(s->scene), bank)
                  : NULL;
    bool deleted = (!gone || gone->unresolved) && !s->refactor.mode;
    if (!(opened && waits && planned && renamed && restored && asked && deleted)) {
        printf(
            "  opened %d ('%s') waits %d planned %d (%d rows, '%s') renamed %d "
            "restored %d asked %d deleted %d\n",
            opened,
            title,
            waits,
            planned,
            listed,
            row,
            renamed,
            restored,
            asked,
            deleted
        );
    }
    test_check(opened && waits, "renaming an asset opens the refactor popover with its name focused and Rename waiting");
    test_check(planned && renamed && restored, "Enter lists what a new name rewrites, Enter again renames across uses, and the toast's Undo puts it back");
    test_check(asked && deleted, "deleting a used asset lists its uses and Delete deletes it");
    fixture_free(&f);
    remove_project(root);
}

/* A Studio project without the palette's document, as a web workspace's
   copy of it older than the build is, closes the palette and records why
   instead of leaving it open, unseen and holding the keys. The frame runs
   once without the Studio UI: the project has no such panel to mount. */
static void palette_without_document()
{
    studio_fixture f = fixture_new();
    studio *s = f.studio;
    frames(&f, 2);
    int records = acount(s->records);
    studio_command_run(s, STUDIO_COMMAND_SEARCH);
    bool opened = studio_palette_is_open(s);
    studio_ui *ui = s->ui;
    s->ui = NULL;
    studio_palette(s);
    s->ui = ui;
    bool recorded = acount(s->records) == records + 1 &&
                    strstr(s->records[records].text, "no Palette document") &&
                    s->records[records].kind == KIT_TOAST_ERROR;
    test_check(opened && !studio_palette_is_open(s) && recorded, "a palette whose document the Studio project lacks closes with an error recorded");
    studio_command_run(s, STUDIO_COMMAND_SEARCH);
    frames(&f, 2);
    test_check(
        studio_palette_is_open(s) && studio_element(s, "#palette-search"),
        "the palette opens again once the document is there"
    );
    fixture_free(&f);
}

void studio_documents_palette_checks()
{
    palette_without_document();
    palette_keys_and_window();
    palette_ranks_and_marks();
    asset_field_picks();
    add_component_groups();
    refactor_renames_and_deletes();
}
