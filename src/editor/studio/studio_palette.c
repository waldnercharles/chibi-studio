#include "graph/authoring/graph_edit.h"
/* The ⌘K search (cmdk; VS Code's Quick Open), a client of the palette
 * (studio_palette_view.c): commands, settings, the scene's objects, assets,
 * projects and the active document's symbols, ranked by
 * the shared support/text_rank.h, with items chosen recently first. A
 * prefix narrows it: '>' to commands and settings, '@' to the active
 * document's symbols, and '?' lists the prefixes. */
#include "editor/studio/studio_designer_internal.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/workspace/asset_index.h"
#include "support/array.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What choosing a result does: an entry's kind. */
typedef enum
{
    PICK_COMMAND,
    PICK_SETTING,
    PICK_OBJECT,
    PICK_COMPONENT,
    PICK_ASSET,
    PICK_PROJECT,
    PICK_SCENARIO,
    PICK_GRAPH,
    PICK_NODE,
    PICK_ELEMENT,
    PICK_RULE,
    PICK_PREFIX,
} pick_kind;

typedef enum
{
    SCOPE_ALL,
    SCOPE_COMMANDS, /* '>' */
    SCOPE_SYMBOLS,  /* '@' */
    SCOPE_HELP,     /* '?' */
} scope;

static const struct
{
    char prefix;
    const char *label;
    scope scope;
} PREFIXES[] = {
    { '>', "Commands and settings", SCOPE_COMMANDS },
    { '@', "Symbols in the active document", SCOPE_SYMBOLS },
    { '?', "Help", SCOPE_HELP },
};

#define PREFIX_COUNT (int)(sizeof PREFIXES / sizeof *PREFIXES)

static studio_palette_entry *add(
    studio_palette_entry **picks,
    pick_kind kind,
    int index,
    const char *group,
    const char *label
)
{
    studio_palette_entry *p = studio_palette_add(picks, group, label);
    p->kind = kind;
    p->index = index;
    return p;
}

/* ---------- Commands and settings ---------- */

static void add_commands(studio *s, studio_palette_entry **picks)
{
    for (int c = STUDIO_COMMAND_SEARCH + 1; c < STUDIO_COMMAND_COUNT; c++) {
        studio_command_info info = studio_command_describe(s, (studio_command)c);
        /* A command that needs a tab has none here. */
        if (info.indexed) {
            continue;
        }
        studio_palette_entry *p = add(picks, PICK_COMMAND, c, "Commands", info.label);
        snprintf(p->keywords, sizeof p->keywords, "%s", info.keywords ? info.keywords : "");
        snprintf(p->key, sizeof p->key, "command:%s", info.id);
        snprintf(p->shortcut, sizeof p->shortcut, "%s", info.shortcut ? info.shortcut : "");
        p->icon = info.icon;
    }
}

/* A setting's value as the palette shows it beside its name. */
static void setting_value(const studio *s, const studio_setting *setting, char *out, size_t size)
{
    float value = studio_setting_get(&s->prefs, setting);
    if (setting->type == STUDIO_SETTING_BOOL) {
        snprintf(out, size, "%s", value ? "On" : "Off");
        return;
    }
    for (int i = 0; i < setting->choice_count; i++) {
        bool shown = setting->type == STUDIO_SETTING_CHOICE
                       ? i == (int)value
                       : strtof(setting->choices[i].value, NULL) == value;
        if (shown) {
            snprintf(out, size, "%s", setting->choices[i].label);
            return;
        }
    }
    snprintf(out, size, "%g", value);
}

/* A command of the same name already runs it, as Reduce Motion. */
static bool commanded(const studio_palette_entry *picks, const char *label)
{
    for (int i = 0; i < acount(picks); i++) {
        if (picks[i].kind == PICK_COMMAND && !strcmp(picks[i].label, label)) {
            return true;
        }
    }
    return false;
}

static void add_settings(studio *s, studio_palette_entry **picks)
{
    for (int i = 0; i < studio_setting_count; i++) {
        const studio_setting *setting = &studio_settings[i];
        if (commanded(*picks, setting->label)) {
            continue;
        }
        const studio_settings_category_info
            *category = &studio_settings_categories[setting->category];
        studio_palette_entry
            *p = add(picks, PICK_SETTING, i, "Settings", setting->label);
        setting_value(s, setting, p->meta, sizeof p->meta);
        snprintf(
            p->keywords,
            sizeof p->keywords,
            "%s %s setting preference",
            setting->keywords ? setting->keywords : "",
            category->label
        );
        snprintf(p->key, sizeof p->key, "setting:%s", setting->key);
        snprintf(p->about, sizeof p->about, "%s", setting->description ? setting->description : "");
        p->icon = category->icon;
    }
}

/* A switch flips from the palette, as in JetBrains' Find Action; any other
   setting opens Settings searching for it. */
static void run_setting(studio *s, const studio_setting *setting)
{
    if (setting->type != STUDIO_SETTING_BOOL) {
        studio_ui_show_settings(s, setting->label);
        return;
    }
    bool on = !studio_setting_get(&s->prefs, setting);
    studio_setting_set(&s->prefs, setting, on);
    char title[96];
    snprintf(title, sizeof title, "%s turned %s", setting->label, on ? "on" : "off");
    kit_toast(s->kit, 0, &(kit_toast_desc){ .title = title });
}

/* ---------- Objects, assets and projects ---------- */

static void add_objects(studio *s, studio_palette_entry **picks)
{
    const app_scene_authoring *scene = s->scene;
    for (int i = 0; i < acount(scene->actors); i++) {
        const scene_actor *actor = &scene->actors[i];
        studio_palette_entry *p = add(picks, PICK_OBJECT, i, "Objects", actor->name);
        snprintf(p->meta, sizeof p->meta, "%s", actor->prototype_name);
        snprintf(p->keywords, sizeof p->keywords, "%s", actor->prototype_name);
        snprintf(p->key, sizeof p->key, "object:%s", actor->name);
        p->icon = studio_prototype_icon(s, actor->prototype_name);
    }
}

/* An asset shows the thumbnail Assets made of it, once one is cached. */
static void add_assets(studio *s, studio_palette_entry **picks)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    int count = index ? reference_index_asset_count(index) : 0;
    for (int i = 0; i < count; i++) {
        const reference_asset *asset = reference_index_asset_at(index, i);
        if (!asset->name || asset->kind < 0 || asset->kind >= REFERENCE_ASSET_KIND_COUNT) {
            continue;
        }
        const char *kind = studio_asset_kind_name(asset->kind);
        if (asset->kind == REFERENCE_ASSET_SHAPE) {
            preview_definition
                *definition = preview_find_definition(s->scene->live, asset->name);
            if (definition && definition->working->domain == BEHAVIOR_DOMAIN_MODEL) {
                kind = "model";
            }
        }
        studio_palette_entry *p = add(picks, PICK_ASSET, i, "Assets", asset->name);
        snprintf(p->meta, sizeof p->meta, "%s", kind);
        snprintf(p->keywords, sizeof p->keywords, "%s", kind);
        snprintf(p->key, sizeof p->key, "asset:%s:%s", kind, asset->name);
        p->icon = studio_asset_kind_icon(asset->kind);
        p->asset = i;
    }
}

/* Each scenario replays on a loop, until Stop. */
static void add_scenarios(studio *s, studio_palette_entry **picks)
{
    char **names = app_scenario_names(s->scene);
    for (int i = 0; i < acount(names); i++) {
        studio_palette_entry *p = add(picks, PICK_SCENARIO, i, "Scenarios", names[i]);
        snprintf(p->meta, sizeof p->meta, "Replay");
        snprintf(p->keywords, sizeof p->keywords, "replay scenario gameplay profile");
        snprintf(p->key, sizeof p->key, "scenario:%s", names[i]);
        p->icon = KIT_ICON_REPLAY;
        free(names[i]);
    }
    afree(names);
}

static void add_projects(studio *s, studio_palette_entry **picks)
{
    for (int i = 0; i < acount(s->projects); i++) {
        if (studio_project_current(s, i)) {
            continue;
        }
        studio_palette_entry
            *p = add(picks, PICK_PROJECT, i, "Projects", s->projects[i].name);
        snprintf(p->keywords, sizeof p->keywords, "open switch");
        snprintf(p->key, sizeof p->key, "project:%s", s->projects[i].name);
        p->icon = KIT_ICON_FOLDER;
    }
}

/* ---------- The active document's symbols ---------- */

static const char *graph_name(const graph *gr)
{
    return gr->name && gr->name[0] ? gr->name : "Event Graph";
}

/* The open behavior's graphs and their nodes, each once: a node drawn inside
   another is found through it. */
static void add_graph_symbols(studio *s, studio_palette_entry **picks, const char *group)
{
    editor *e = sg_graph_editor(s);
    for (int g = 0; g < acount(e->asset->graphs); g++) {
        graph *gr = e->asset->graphs[g];
        if (gr->math_expression) {
            continue;
        }
        studio_palette_entry *p = add(picks, PICK_GRAPH, g, group, graph_name(gr));
        snprintf(p->meta, sizeof p->meta, "graph");
        p->graph = gr->graph_id;
        p->icon = KIT_ICON_GRAPH;
        for (int i = 0; i < acount(gr->nodes); i++) {
            node *n = gr->nodes[i];
            if (!n->desc || graph_inline_owner(gr, n) != n) {
                continue;
            }
            char title[96];
            p = add(picks, PICK_NODE, i, group, ed_node_title(e, n, title, sizeof title));
            snprintf(p->meta, sizeof p->meta, "%s", graph_name(gr));
            snprintf(p->keywords, sizeof p->keywords, "%s", n->desc->name);
            p->graph = gr->graph_id;
            p->node = n->id;
            uint32_t icon = sg_node_icon(s, n);
            p->icon = icon ? icon : KIT_ICON_HEXAGON;
        }
    }
}

/* The UI document's elements, as the Hierarchy names them, and its rules. */
static void add_ui_symbols(studio *s, studio_palette_entry **picks, const char *group)
{
    ui_buffer *b = NULL;
    studio_designer *d = designer_state(s, &b);
    if (!d) {
        return;
    }
    for (int i = 0; i < acount(d->rml->elements); i++) {
        const ui_source_element *el = &d->rml->elements[i];
        char name[96];
        designer_element_name(d->rml, i, name, sizeof name);
        studio_palette_entry *p = add(picks, PICK_ELEMENT, i, group, name);
        snprintf(
            p->meta,
            sizeof p->meta,
            "%s%s%s%s",
            el->id ? "#" : "",
            el->id ? el->id : "",
            el->classes ? " ." : "",
            el->classes ? el->classes : ""
        );
        snprintf(p->keywords, sizeof p->keywords, "%s", p->meta);
        p->icon = designer_element_icon(d->rml, i);
    }
    for (int i = 0; d->rcss && i < acount(d->rcss->rules); i++) {
        studio_palette_entry
            *p = add(picks, PICK_RULE, i, group, d->rcss->rules[i].selector);
        snprintf(p->meta, sizeof p->meta, "line %d", d->rcss->rules[i].line);
        p->icon = KIT_ICON_MATERIAL;
    }
}

/* Each object's components, found by the component's name. */
static void add_components(studio *s, studio_palette_entry **picks, const char *group)
{
    app_scene_authoring *scene = s->scene;
    for (int i = 0; i < acount(scene->actors); i++) {
        const scene_actor *actor = &scene->actors[i];
        const scene_prototype_asset *asset = find_prototype(scene, actor->prototype_name);
        const prototype *prototype = asset ? asset->prototype : NULL;
        for (int c = 0; prototype && c < acount(prototype->components); c++) {
            const char *component = prototype->components[c].desc->name;
            studio_palette_entry *p = add(picks, PICK_COMPONENT, i, group, component);
            snprintf(p->meta, sizeof p->meta, "%s", actor->name);
            snprintf(p->keywords, sizeof p->keywords, "component %s", actor->name);
            p->icon = KIT_ICON_PROTOTYPE;
        }
    }
}

static void add_symbols(studio *s, studio_palette_entry **picks)
{
    ui_buffer *b = studio_ui_buffer(s);
    if (b) {
        add_ui_symbols(s, picks, "In This Document");
        return;
    }
    if (studio_graph_active(s)) {
        add_graph_symbols(s, picks, "In This Behavior");
    }
    if (s->scene->document.kind == APP_EDITOR_DOCUMENT_SCENE) {
        add_components(s, picks, "Components");
    }
}

static void add_prefixes(studio_palette_entry **picks)
{
    for (int i = 0; i < PREFIX_COUNT; i++) {
        studio_palette_entry
            *p = add(picks, PICK_PREFIX, i, "Prefixes", PREFIXES[i].label);
        snprintf(p->meta, sizeof p->meta, "%c", PREFIXES[i].prefix);
    }
}

/* ---------- Recent use ---------- */

/* Where `key` is among the recent items, or -1. */
static int recent_index(const studio_recent *recent, const char *key)
{
    for (int i = 0; key[0] && i < STUDIO_RECENT_COUNT && recent[i].key[0]; i++) {
        if (!strcmp(recent[i].key, key)) {
            return i;
        }
    }
    return -1;
}

/* Frecency, as Raycast and Firefox rank: newer and more often chosen items
   gain up to 300, never enough to lift a scattered-letter match above a
   word it names. */
static int boost(const studio_prefs *prefs, const char *key)
{
    int i = recent_index(prefs->recent, key);
    if (i < 0) {
        return 0;
    }
    int uses = prefs->recent[i].uses < 5 ? prefs->recent[i].uses : 5;
    return 40 * uses + 100 * (STUDIO_RECENT_COUNT - i) / STUDIO_RECENT_COUNT;
}

void studio_palette_remember(studio_recent *recent, const char *key)
{
    if (!key[0]) {
        return;
    }
    int i = recent_index(recent, key);
    studio_recent used = { .uses = i >= 0 ? recent[i].uses + 1 : 1 };
    snprintf(used.key, sizeof used.key, "%s", key);
    int last = i >= 0 ? i : STUDIO_RECENT_COUNT - 1;
    memmove(&recent[1], &recent[0], (size_t)last * sizeof *recent);
    recent[0] = used;
}

void studio_palette_recent_first(const studio_recent *recent, int limit, studio_palette_entry **picks)
{
    studio_palette_entry *ordered = NULL;
    bool *taken = xcalloc((size_t)acount(*picks) + 1, sizeof *taken);
    for (int r = 0;
         r < STUDIO_RECENT_COUNT && recent[r].key[0] && acount(ordered) < limit;
         r++) {
        for (int i = 0; i < acount(*picks); i++) {
            if (!taken[i] && !strcmp((*picks)[i].key, recent[r].key)) {
                apush(ordered, (*picks)[i]);
                snprintf(alast(ordered).group, sizeof alast(ordered).group, "Recent");
                taken[i] = true;
                break;
            }
        }
    }
    for (int i = 0; i < acount(*picks); i++) {
        if (!taken[i]) {
            apush(ordered, (*picks)[i]);
        }
    }
    free(taken);
    aclear(*picks);
    for (int i = 0; i < acount(ordered); i++) {
        apush(*picks, ordered[i]);
    }
    afree(ordered);
}

/* ---------- Choosing ---------- */

void studio_search_choose(studio *s, const studio_palette_entry *p)
{
    app_scene_authoring *scene = s->scene;
    studio_palette_remember(s->prefs.recent, p->key);
    switch ((pick_kind)p->kind) {
        case PICK_COMMAND:
            studio_command_run(s, (studio_command)p->index);
            break;
        case PICK_SETTING: run_setting(s, &studio_settings[p->index]); break;
        case PICK_OBJECT:
        case PICK_COMPONENT:
            finish_transform_edit(scene, false);
            select_instance(scene, p->index);
            studio_frame_selection(s);
            break;
        case PICK_ASSET: app_scene_authoring_open_asset(scene, p->index); break;
        case PICK_PROJECT: studio_open_project(s, p->index); break;
        case PICK_SCENARIO: studio_replay_scenario(s, p->label); break;
        case PICK_GRAPH:
            studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
            editor_open_graph(sg_graph_editor(s), p->graph);
            break;
        case PICK_NODE:
            studio_show_editor(s, STUDIO_EDITOR_BEHAVIOR);
            studio_reveal_node(s, p->graph, p->node);
            break;
        case PICK_ELEMENT: designer_select(s, p->index); break;
        case PICK_RULE: {
            ui_buffer *b = NULL;
            studio_designer *d = designer_state(s, &b);
            studio_open_ui_part(
                s,
                d->path,
                UI_PART_RCSS,
                d->rcss->rules[p->index].line
            );
            break;
        }
        case PICK_PREFIX: {
            char prefix[2] = { PREFIXES[p->index].prefix, 0 };
            studio_palette_open(s, STUDIO_PALETTE_SEARCH, STUDIO_PALETTE_PLACEHOLDER);
            studio_palette_set_query(s, prefix);
            break;
        }
    }
}

const char *studio_search_verb(const studio_palette_entry *p)
{
    switch ((pick_kind)p->kind) {
        case PICK_COMMAND: return "Run";
        case PICK_SETTING:
            return studio_settings[p->index].type == STUDIO_SETTING_BOOL
                     ? "Toggle"
                     : "Open";
        case PICK_OBJECT:
        case PICK_COMPONENT:
        case PICK_ELEMENT: return "Select";
        case PICK_SCENARIO: return "Replay";
        case PICK_PREFIX: return "Filter";
        case PICK_ASSET:
        case PICK_PROJECT:
        case PICK_GRAPH:
        case PICK_NODE:
        case PICK_RULE: return "Open";
    }
    return "Open";
}

/* ---------- The search's results ---------- */

static scope scope_of(const char *query, const char **rest)
{
    for (int i = 0; i < PREFIX_COUNT; i++) {
        if (query[0] == PREFIXES[i].prefix) {
            *rest = query + 1 + strspn(query + 1, " ");
            return PREFIXES[i].scope;
        }
    }
    *rest = query;
    return SCOPE_ALL;
}

const char *studio_search_query(const char *query)
{
    const char *rest;
    scope_of(query, &rest);
    return rest;
}

bool studio_search_lists_anew(const char *was, const char *query)
{
    const char *before, *now;
    scope a = scope_of(was, &before), b = scope_of(query, &now);
    return a != b || !before[0] != !now[0];
}

/* Symbols wait for a query or '@', so the list opens short. */
void studio_search_entries(studio *s, const char *query, studio_palette_entry **picks)
{
    const char *rest;
    scope scope = scope_of(query, &rest);
    if (scope == SCOPE_HELP) {
        add_prefixes(picks);
        return;
    }
    if (scope != SCOPE_SYMBOLS) {
        add_commands(s, picks);
    }
    if (scope == SCOPE_ALL) {
        add_objects(s, picks);
        add_assets(s, picks);
        add_projects(s, picks);
        add_scenarios(s, picks);
    }
    if (scope != SCOPE_SYMBOLS) {
        add_settings(s, picks);
    }
    if (scope == SCOPE_SYMBOLS || (scope == SCOPE_ALL && rest[0])) {
        add_symbols(s, picks);
    }
    for (int i = 0; i < acount(*picks); i++) {
        (*picks)[i].boost = boost(&s->prefs, (*picks)[i].key);
    }
    if (scope == SCOPE_ALL && !rest[0]) {
        studio_palette_recent_first(s->prefs.recent, STUDIO_RECENT_COUNT, picks);
    }
}
