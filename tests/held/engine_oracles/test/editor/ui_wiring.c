#include "graph/nodes/core_nodes.h"
#include "test/asset_fixture.h"
/* Wiring a UI widget end to end: the assets an asset picker offers, the
 * events GameUi documents send the behaviors they reach, and the Add menu's
 * Event nodes for them. */
#include "assets/prototype.h"
#include "authoring/reference_index.h"
#include "assets/reference_adapters.h"
#include "assets/ui_events.h"
#include "editor/graph/search.h"
#include "editor/studio/studio_internal.h"
#include "editor/workspace/editor_internal.h"
#include "runtime/component_catalog.h"
#include "test/suite_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool names_are(const char **names, const char *const *expected, int count)
{
    bool same = acount(names) == count;
    for (int i = 0; same && i < count; i++) {
        same = !strcmp(names[i], expected[i]);
    }
    afree(names);
    return same;
}

/* A picker lists the assets of its kind by name, not the names only a
 * reference makes up. */
static void picker_choices()
{
    reference_index *index = reference_index_new();
    reference_index_add_asset(index, REFERENCE_ASSET_UI, "Settings", "content/ui/Settings.rml", NULL);
    reference_index_add_asset(index, REFERENCE_ASSET_PROTOTYPE, "Vault", "content/prototypes/Vault.chibi", NULL);
    reference_index_add_asset(index, REFERENCE_ASSET_UI, "bank", "content/ui/bank.rml", NULL);
    int widgets = reference_index_add_asset(index, REFERENCE_ASSET_UI, "Widgets", "content/ui/Widgets.rml", NULL);
    reference_index_add_ui(index, widgets, "<use ui=\"Missing\"/>");
    reference_index_add_asset(index, REFERENCE_ASSET_UI, "base.rcss", "content/ui/theme/base.rcss", NULL);
    static const char *const UI[] = { "bank", "Settings", "Widgets" };
    static const char *const PROTOTYPES[] = { "Vault" };
    bool ui = names_are(studio_asset_names(index, REFERENCE_ASSET_UI), UI, 3);
    bool prototypes = names_are(studio_asset_names(index, REFERENCE_ASSET_PROTOTYPE), PROTOTYPES, 1);
    bool none = names_are(studio_asset_names(index, REFERENCE_ASSET_MATERIAL), NULL, 0);
    test_check(ui && prototypes && none, "an asset picker offers the project's assets of its kind, sorted, without names nothing defines or stylesheets");
    reference_index_free(index);
    bool kinds = reference_named_field_kind("GameUi", "document") == REFERENCE_ASSET_UI &&
                 reference_named_field_kind("GameSpawnRequest", "prototype") ==
                     REFERENCE_ASSET_PROTOTYPE &&
                 reference_named_field_kind("GameActionBinding", "action") ==
                     REFERENCE_ASSET_EVENT &&
                 reference_named_field_kind("GameUi", "order") ==
                     REFERENCE_ASSET_KIND_COUNT;
    test_check(kinds, "component fields that name assets pick from assets of their kind");
}

static ui_event *events_of(const char *rml, const char *document)
{
    ui_document_event *found = NULL;
    ui_document_events(rml, &found);
    ui_event *events = NULL;
    for (int i = 0; i < acount(found); i++) {
        ui_events_add(&events, &found[i], document);
    }
    ui_document_events_free(found);
    return events;
}

static const ui_event *event_named(const ui_event *events, const char *name)
{
    for (int i = 0; i < acount(events); i++) {
        if (!strcmp(events[i].name, name)) {
            return &events[i];
        }
    }
    return NULL;
}

/* Each event reads as what the player does to send it, every way in one
 * sentence. */
static void event_sentences()
{
    ui_event *events = events_of(
        "<div id=\"extract\" data-event-click=\"emit('Extract')\">Go</div>"
        "<div id=\"menu\" data-keys=\"Escape:Close Enter:Extract\"></div>"
        "<input id=\"amount\" type=\"range\" "
        "data-event-change=\"emit('Amount')\"/>",
        "Bank"
    );
    const ui_event *extract = event_named(events, "Extract");
    const ui_event *close = event_named(events, "Close");
    const ui_event *amount = event_named(events, "Amount");
    bool ok = acount(events) == 3 && extract && close && amount &&
              !strcmp(extract->description, "Sent when the player clicks #extract in Bank, or presses Enter inside #menu in Bank") &&
              !strcmp(close->description, "Sent when the player presses Escape inside #menu in Bank") &&
              !strcmp(amount->description, "Sent when the player changes #amount in Bank");
    if (!ok) {
        for (int i = 0; i < acount(events); i++) {
            printf("  %s: %s\n", events[i].name, events[i].description);
        }
    }
    test_check(ok, "a UI event describes what the player does to send it");
    ui_events_free(events);
}

/* A behavior hears the documents on whatever attaches it: a prototype's
 * GameUi, and a scene object's, inherited or overridden. */
static void attached_documents()
{
    game_prototype_registry registry = {
        .components = GAME_REGISTRY_COMPONENTS,
        .component_count = GAME_REGISTRY_COMPONENT_COUNT,
    };
    char error[256];
    game_prototype *vault = game_prototype_load(
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Vault\")),\n"
        "            data.Field.Named(\"behaviors\", data.List([\n"
        "                data.Value.Text(\"Teller\")\n"
        "            ])),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"GameUi\", data.Object([\n"
        "                    data.Field.Named(\"document\", "
        "data.Value.Text(\"Bank\"))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        &registry,
        error,
        sizeof error
    );
    game_prototype *sign = game_prototype_load(
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"name\", data.Value.Text(\"Sign\")),\n"
        "            data.Field.Named(\"components\", data.Object([\n"
        "                data.Field.Named(\"GameUi\", data.Object([\n"
        "                    data.Field.Named(\"document\", "
        "data.Value.Text(\"Notice\"))\n"
        "                ]))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n",
        &registry,
        error,
        sizeof error
    );
    assert(vault && sign);
    reference_index *index = reference_index_new();
    int bank = reference_index_add_asset(index, REFERENCE_ASSET_UI, "Bank", "content/ui/Bank.rml", NULL);
    int desk = reference_index_add_asset(index, REFERENCE_ASSET_UI, "Desk", "content/ui/Desk.rml", NULL);
    reference_index_add_asset(index, REFERENCE_ASSET_UI, "Notice", "content/ui/Notice.rml", NULL);
    int teller = reference_index_add_asset(index, REFERENCE_ASSET_BEHAVIOR, "Teller", "content/behaviors/Teller.chibi", NULL);
    int prototype = reference_index_add_asset(
        index,
        REFERENCE_ASSET_PROTOTYPE,
        "Vault",
        "content/prototypes/Vault.chibi",
        NULL
    );
    int other = reference_index_add_asset(index, REFERENCE_ASSET_PROTOTYPE, "Sign", "content/prototypes/Sign.chibi", NULL);
    int scene = reference_index_add_asset(index, REFERENCE_ASSET_SCENE, "Hall", "content/scenes/Hall.chibi", NULL);
    reference_index_add_prototype(index, prototype, vault);
    reference_index_add_prototype(index, other, sign);
    const char *behaviors[] = { "Teller" };
    av_value *overrides = test_read_asset_value(
        "package asset\n"
        "\n"
        "import data \"game:assets\"\n"
        "\n"
        "pub Build :: () -> byte[] {\n"
        "    return data.Encode(\n"
        "        data.Object([\n"
        "            data.Field.Named(\"GAME.GameUi\", data.Object([\n"
        "                data.Field.Named(\"document\", "
        "data.Value.Text(\"Desk\"))\n"
        "            ]))\n"
        "        ])\n"
        "    )\n"
        "}\n"
    );
    reference_object clerk = { .name = "clerk",
                               .prototype = "Sign",
                               .base = sign,
                               .overrides = overrides,
                               .behaviors = behaviors,
                               .behavior_count = 1 };
    reference_object post = { .name = "post", .prototype = "Sign", .base = sign };
    reference_index_add_object(index, scene, &clerk);
    av_value_free(overrides);
    reference_index_add_object(index, scene, &post);
    int *documents = NULL;
    reference_index_attached_documents(index, teller, &documents);
    bool ok = acount(documents) == 2 && documents[0] == bank && documents[1] == desk;
    test_check(ok, "a behavior hears the GameUi documents of the prototypes and objects that attach it");
    afree(documents);
    reference_index_free(index);
    game_prototype_free(vault);
    game_prototype_free(sign);
}

static const ed_search_match *ui_match(const ed_search_match *matches, const char *name)
{
    for (int i = 0; i < acount(matches); i++) {
        if (matches[i].ui_event && !strcmp(matches[i].name, name)) {
            return &matches[i];
        }
    }
    return NULL;
}

/* The Add menu offers a UI event as an Event node, found by what the player
 * does, until the behavior handles it. */
static void add_menu_events()
{
    editor *e = editor_new(behavior_asset_new("Teller"));
    ui_event *events = events_of("<div id=\"extract\" data-event-click=\"emit('Extract')\"/>", "Bank");
    editor_set_ui_event_catalog(e, events, acount(events));
    ed_search_match *by_deed = ed_search_matches(e, "clicks extract");
    ed_search_match *matches = ed_search_matches(e, "Extract");
    const ed_search_match *found = ui_match(matches, "On Extract");
    bool offered = ui_match(by_deed, "On Extract") && found &&
                   !strcmp(found->category, "UI Events");
    node *n = found ? ed_spawn_match_at(e, found, 0, 0) : NULL;
    bool named = n && n->desc == &NODE_Event && n->reference_name &&
                 !strcmp(n->reference_name, "Extract");
    afree(matches);
    matches = ed_search_matches(e, "Extract");
    bool handled = !ui_match(matches, "On Extract");
    test_check(offered && named && handled, "the Add menu offers a UI event as an Event node until the behavior handles it");
    afree(by_deed);
    afree(matches);
    editor_set_ui_event_catalog(e, NULL, 0);
    ui_events_free(events);
    editor_free(e);
}

/* The Add menu offers each value a document binds under self as a Set UI
   Data that names it, and Name's choices are those values, each once with
   the documents that bind it. */
static void add_menu_data()
{
    editor *e = editor_new(behavior_asset_new("Teller"));
    ui_document_binding *found = NULL;
    ui_document_bindings("<div data-for=\"coin : self.coins\">{{ coin.value }}</div>{{ self.total }}", &found);
    ui_data_binding *bindings = NULL;
    for (int i = 0; i < acount(found); i++) {
        ui_data_binding bank = { .binding = found[i] },
                        till = { .binding = found[i] };
        snprintf(bank.document, sizeof bank.document, "Bank");
        snprintf(till.document, sizeof till.document, "Till");
        apush(bindings, bank);
        if (i) {
            apush(bindings, till);
        }
    }
    afree(found);
    editor_set_ui_data_catalog(e, bindings, acount(bindings));
    ed_search_match *matches = ed_search_matches(e, "coins value");
    const ed_search_match *coins = NULL;
    for (int i = 0; i < acount(matches); i++) {
        coins = matches[i].ui_data ? &matches[i] : coins;
    }
    bool offered = coins && !strcmp(coins->name, "Set coins (Bank)") &&
                   !strcmp(coins->category, "UI Data");
    node *n = coins ? ed_spawn_match_at(e, coins, 0, 0) : NULL;
    bool named = n && n->input_pins[2].default_value.str &&
                 !strcmp(n->input_pins[2].default_value.str, "coins");
    const editor_choices *choices = n ? ed_pin_choices(e, n, 0, 2) : NULL;
    bool chosen = choices && choices->count == 2 &&
                  !strcmp(choices->items[1].label, "total (Bank, Till)") &&
                  !strcmp(choices->items[0].name, "coins");
    test_check(offered && named && chosen, "the Add menu offers Set UI Data for each value a document binds, and Name chooses among them");
    afree(matches);
    editor_set_ui_data_catalog(e, NULL, 0);
    afree(bindings);
    editor_free(e);
}

void test_ui_wiring()
{
    picker_choices();
    event_sentences();
    attached_documents();
    add_menu_events();
    add_menu_data();
}
