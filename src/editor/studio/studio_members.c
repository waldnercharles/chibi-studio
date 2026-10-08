#include "graph/authoring/asset_edit.h"
/* Members: the open behavior's graphs, the events it handles, its functions,
 * macros and variables, and the open graph's named values, after Unreal's My
 * Blueprint. A click opens a graph or selects a member for Properties;
 * double-click or F2 renames. A variable dragged onto the canvas places its
 * Get, or its Set with Alt (Unreal), and a function or macro its call. Each
 * member counts its uses across the project, and the count lists them in
 * Find. The panel is authored (Members.rml, its header's filter
 * MembersHeader.rml); this is its `members` source, which windows the rows
 * into its List and takes what the rows, the name field and the menu send
 * (docs/studio-rml-migration.md, P5). */
#include "editor/studio/studio_members_internal.h"
#include "support/array.h"
#include "ui/list.h"
#include "ui/popover.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    SECTION_GRAPHS,
    SECTION_EVENTS,
    SECTION_FUNCTIONS,
    SECTION_MACROS,
    SECTION_VARIABLES,
    SECTION_VALUES,
    SECTION_COUNT,
} section;

static const char *const SECTION_LABELS[SECTION_COUNT] = {
    "Graphs", "Events", "Functions", "Macros", "Variables", "Named Values",
};

typedef enum
{
    ROW_SECTION,
    ROW_CATEGORY,
    ROW_GRAPH,
    ROW_VARIABLE,
    ROW_LOCAL,
    ROW_EVENT,
} row_kind;

typedef struct
{
    row_kind kind;
    /* section, graph, variable or local; a category's first variable; an
       event's graph, or -1 for an event definition's own event */
    int index;
    int depth;
    int count; /* a section's or category's members */
    int node;  /* an event's Event node in its graph */
} member_row;

/* A member's uses by role, counted once per build of the asset index. */
struct studio_member_uses
{
    reference_symbol_kind kind;
    uint64_t graph_id, id;
    char event[128];
    int roles[REFERENCE_ROLE_COUNT];
};

/* ---------- Selection ---------- */

behavior_variable *sm_variable(const studio *s)
{
    return s->members.kind == STUDIO_MEMBER_VARIABLE
             ? behavior_asset_find_variable_by_id(
                   sg_graph_editor(s)->asset,
                   s->members.id
               )
             : NULL;
}

graph *sm_graph(const studio *s)
{
    return s->members.kind == STUDIO_MEMBER_GRAPH && s->members.id
             ? behavior_asset_find_graph(sg_graph_editor(s)->asset, s->members.id)
             : NULL;
}

const graph_local *sm_local(const studio *s)
{
    return s->members.kind == STUDIO_MEMBER_LOCAL
             ? graph_local_by_id(sg_active_graph(s), s->members.id)
             : NULL;
}

/* A member that no longer exists, as after Undo, is deselected, and so is any
   member once a graph node is selected: Details shows one or the other. */
static void follow(studio *s)
{
    studio_members *m = &s->members;
    uint64_t node = studio_graph_node(s);
    bool exists = studio_graph_active(s) &&
                  (sm_variable(s) || sm_graph(s) || sm_local(s));
    if (!exists || (node && node != m->shown_node)) {
        m->kind = STUDIO_MEMBER_NONE;
    }
    m->shown_node = node;
}

uint64_t studio_member_shown(studio *s)
{
    follow(s);
    return s->members.kind ? (uint64_t)s->members.kind << 56 ^ s->members.id : 0;
}

static void select_member(studio *s, studio_member_kind kind, uint64_t id)
{
    s->members.kind = kind;
    s->members.id = id;
    ed_clear_selection(sg_graph_editor(s));
    s->members.shown_node = 0;
}

/* A named value selects and frames the node that gives it. */
static void select_local(studio *s, const graph_local *local)
{
    editor *e = sg_graph_editor(s);
    node *source = graph_node_by_id(sg_active_graph(s), local->source_id);
    select_member(s, STUDIO_MEMBER_LOCAL, local->id);
    sg_select_only(e, source);
    s->members.shown_node = source->id;
    sg_frame_nodes(s, true, true);
}

/* ---------- Edits ---------- */

bool sm_record(editor *e, behavior_asset *before, bool ok)
{
    behavior_asset *pending = e->pending_snapshot;
    e->pending_snapshot = before;
    if (ok) {
        ed_commit_snapshot(e);
    } else {
        ed_cancel_snapshot(e);
    }
    e->pending_snapshot = pending;
    return ok;
}

void sm_refused(studio *s, const char *title)
{
    kit_toast(s->kit, 0, &(kit_toast_desc){ .type = KIT_TOAST_WARNING, .title = title });
}

static bool locked(const studio *s)
{
    return !behavior_asset_mutable(sg_graph_editor(s)->asset);
}

static void start_rename(studio *s, studio_member_kind kind, uint64_t id)
{
    studio_members *m = &s->members;
    if (kind == STUDIO_MEMBER_NONE || locked(s)) {
        return;
    }
    m->rename_kind = kind;
    m->rename_id = id;
    m->rename_focus = true;
}

/* Names a new member after `base`, numbering it past the names in use. */
static void unique_graph_name(const behavior_asset *asset, const char *base, char *out, size_t size)
{
    snprintf(out, size, "%s", base);
    for (int i = 2; behavior_asset_find_graph_by_name(asset, out); i++) {
        snprintf(out, size, "%s %d", base, i);
    }
}

static void unique_variable_name(const behavior_asset *asset, const char *base, char *out, size_t size)
{
    snprintf(out, size, "%s", base);
    for (int i = 2; behavior_asset_find_variable_by_name(asset, out); i++) {
        snprintf(out, size, "%s %d", base, i);
    }
}

/* A new function or macro opens and takes its name at once (Unreal). */
static void add_graph(studio *s, graph_kind kind, const char *base)
{
    editor *e = sg_graph_editor(s);
    char name[128];
    unique_graph_name(e->asset, base, name, sizeof name);
    editor_command create = { .kind = EDITOR_COMMAND_CREATE_GRAPH,
                              .data.create_graph = { kind, name } };
    graph *made = editor_command_apply(e, create) == 0
                    ? behavior_asset_find_graph_by_name(e->asset, name)
                    : NULL;
    if (!made) {
        return;
    }
    editor_open_graph(e, made->graph_id);
    select_member(s, STUDIO_MEMBER_GRAPH, made->graph_id);
    start_rename(s, STUDIO_MEMBER_GRAPH, made->graph_id);
}

/* A new variable takes the selected variable's type, else Boolean (Unreal). */
static void add_variable(studio *s)
{
    editor *e = sg_graph_editor(s);
    const behavior_variable *like = sm_variable(s);
    pin_spec type = like ? sm_variable_pin(like)
                         : (pin_spec){ .type = PIN_BOOL, .type_name = "bool" };
    char name[128];
    unique_variable_name(e->asset, "New Variable", name, sizeof name);
    behavior_asset *before = ed_snapshot(e);
    behavior_variable *v = behavior_asset_add_variable(
        e->asset,
        name,
        type.type,
        type.type_name,
        type.container,
        NULL,
        NULL
    );
    if (sm_record(e, before, v != NULL)) {
        select_member(s, STUDIO_MEMBER_VARIABLE, v->id);
        start_rename(s, STUDIO_MEMBER_VARIABLE, v->id);
    }
}

static void duplicate_variable(studio *s)
{
    editor *e = sg_graph_editor(s);
    const behavior_variable *v = sm_variable(s);
    if (!v || locked(s)) {
        return;
    }
    char name[128];
    unique_variable_name(e->asset, v->name, name, sizeof name);
    behavior_asset *before = ed_snapshot(e);
    behavior_variable *copy = behavior_asset_duplicate_variable(e->asset, v->id, name);
    if (sm_record(e, before, copy != NULL)) {
        select_member(s, STUDIO_MEMBER_VARIABLE, copy->id);
    }
}

static const char *graph_label(const graph *g)
{
    return g->name && g->name[0] ? g->name : "Event Graph";
}

/* The selected member's name, for its toast. */
static const char *member_name(const studio *s)
{
    switch (s->members.kind) {
        case STUDIO_MEMBER_GRAPH: return graph_label(sm_graph(s));
        case STUDIO_MEMBER_VARIABLE: return sm_variable(s)->name;
        case STUDIO_MEMBER_LOCAL: return sm_local(s)->name;
        default: return "";
    }
}

/* The graph editor's latest undo step, which a delete's toast undoes. */
static const behavior_asset *latest_step(const editor *e)
{
    return e && acount(e->undo) ? alast(e->undo) : NULL;
}

/* A toast offers Undo while the delete is still the latest edit, as scene
   deletes do. */
static void offer_undo(studio *s, const char *name)
{
    studio_members *m = &s->members;
    char title[160];
    snprintf(title, sizeof title, "Deleted %s", name);
    kit_toast_dismiss(s->kit, m->undo.toast);
    m->undo.toast = kit_toast(
        s->kit,
        0,
        &(kit_toast_desc){ .title = title, .action = "Undo", .duration = 6 }
    );
    m->undo.step = latest_step(sg_graph_editor(s));
    studio_record(s, KIT_TOAST_DEFAULT, title);
}

void studio_members_toast(studio *s)
{
    studio_members *m = &s->members;
    if (!m->undo.toast) {
        return;
    }
    bool current = latest_step(sg_graph_editor(s)) == m->undo.step;
    if (kit_toast_action(s->kit, m->undo.toast)) {
        m->undo.toast = 0;
        if (current && studio_graph_can_replay(s, false)) {
            studio_graph_history(s, false);
            studio_report(s, "Undo");
        }
        return;
    }
    if (!current) {
        kit_toast_dismiss(s->kit, m->undo.toast);
        m->undo.toast = 0;
    }
}

/* The open document's member as a symbol of the asset index. */
static reference_symbol member_symbol(studio *s, studio_member_kind kind, uint64_t id)
{
    reference_symbol symbol = { .asset = studio_refresh_document_asset(s), .id = id };
    symbol.kind = kind == STUDIO_MEMBER_GRAPH ? REFERENCE_SYMBOL_GRAPH
                                              : REFERENCE_SYMBOL_VARIABLE;
    return symbol;
}

/* Deleting a variable deletes its Get and Set nodes (Unreal); deleting a
   named value draws its wires again. */
static void delete_member_now(studio *s)
{
    editor *e = sg_graph_editor(s);
    studio_members *m = &s->members;
    if (locked(s) || m->kind == STUDIO_MEMBER_NONE) {
        return;
    }
    char name[128];
    snprintf(name, sizeof name, "%s", member_name(s));
    behavior_asset *before = NULL;
    bool deleted = false;
    switch (m->kind) {
        case STUDIO_MEMBER_GRAPH:
            deleted = editor_delete_graph(e, m->id) == 0;
            break;
        case STUDIO_MEMBER_VARIABLE:
            before = ed_snapshot(e);
            behavior_asset_remove_variable_uses(e->asset, m->id);
            deleted = sm_record(e, before, behavior_asset_delete_variable(e->asset, m->id) == 0);
            break;
        case STUDIO_MEMBER_LOCAL:
            before = ed_snapshot(e);
            deleted = sm_record(e, before, graph_expand_local(sg_active_graph(s), m->id) == 0);
            break;
        default: return;
    }
    m->kind = STUDIO_MEMBER_NONE;
    if (deleted) {
        offer_undo(s, name);
    }
}

void studio_members_delete_now(studio *s, studio_member_kind kind, uint64_t id)
{
    select_member(s, kind, id);
    delete_member_now(s);
}

/* A function or variable something still uses asks first, listing its uses. */
static void delete_member(studio *s)
{
    studio_members *m = &s->members;
    bool asks = (m->kind == STUDIO_MEMBER_GRAPH || m->kind == STUDIO_MEMBER_VARIABLE) &&
                !locked(s) &&
                studio_refactor_confirm_delete(
                    s,
                    member_symbol(s, m->kind, m->id),
                    member_name(s),
                    m->kind,
                    m->id
                );
    if (!asks) {
        delete_member_now(s);
    }
}

bool studio_members_rename_across(
    studio *s,
    studio_member_kind kind,
    uint64_t id,
    const char *name,
    const char *new_name
)
{
    return name && strcmp(name, new_name) &&
           studio_refactor_member_rename(s, member_symbol(s, kind, id), name, new_name);
}

bool sm_rename(studio *s, studio_member_kind kind, uint64_t id, const char *name)
{
    editor *e = sg_graph_editor(s);
    graph *g = behavior_asset_find_graph(e->asset, id);
    const behavior_variable *v = behavior_asset_find_variable_by_id(e->asset, id);
    const graph_local *local = graph_local_by_id(sg_active_graph(s), id);
    const char *old = kind == STUDIO_MEMBER_GRAPH    ? (g ? g->name : NULL)
                    : kind == STUDIO_MEMBER_VARIABLE ? (v ? v->name : NULL)
                    : kind == STUDIO_MEMBER_LOCAL ? (local ? local->name : NULL)
                                                  : NULL;
    if (!old || !name[0] || !strcmp(old, name)) {
        return true;
    }
    /* A library's functions and an event's payload are named elsewhere too. */
    if (kind != STUDIO_MEMBER_LOCAL &&
        studio_members_rename_across(s, kind, id, old, name)) {
        return true;
    }
    if (kind == STUDIO_MEMBER_GRAPH) {
        editor_command rename = { .kind = EDITOR_COMMAND_RENAME_GRAPH,
                                  .data.rename_graph = { id, name } };
        return editor_command_apply(e, rename) == 0;
    }
    behavior_asset *before = ed_snapshot(e);
    int status = kind == STUDIO_MEMBER_VARIABLE
                   ? behavior_asset_rename_variable(e->asset, id, name)
                   : graph_rename_local(sg_active_graph(s), id, name);
    return sm_record(e, before, status == 0);
}

const char *sm_name_taken(studio_member_kind kind)
{
    switch (kind) {
        case STUDIO_MEMBER_GRAPH: return "A graph has that name";
        case STUDIO_MEMBER_VARIABLE: return "A variable has that name";
        default: return "A value has that name";
    }
}

static void rename_member(studio *s, const char *name)
{
    studio_members *m = &s->members;
    if (!sm_rename(s, m->rename_kind, m->rename_id, name)) {
        sm_refused(s, sm_name_taken(m->rename_kind));
    }
}

/* ---------- Rows ---------- */

static section graph_section(const graph *g)
{
    return g->kind == GRAPH_FUNCTION ? SECTION_FUNCTIONS
         : g->kind == GRAPH_MACRO    ? SECTION_MACROS
                                     : SECTION_GRAPHS;
}

/* Libraries hold functions only; variables are a behavior's, or a field's
   and an event's parameters. */
static bool shows(section sec, behavior_domain domain)
{
    switch (sec) {
        case SECTION_GRAPHS:
            return domain != BEHAVIOR_DOMAIN_LIBRARY &&
                   domain != BEHAVIOR_DOMAIN_SHADER_LIBRARY &&
                   domain != BEHAVIOR_DOMAIN_EVENT_DEFINITION;
        case SECTION_FUNCTIONS:
            return domain != BEHAVIOR_DOMAIN_EVENT_DEFINITION;
        case SECTION_MACROS: return domain == BEHAVIOR_DOMAIN_EVENT;
        case SECTION_EVENTS:
            return domain == BEHAVIOR_DOMAIN_EVENT ||
                   domain == BEHAVIOR_DOMAIN_EVENT_DEFINITION;
        case SECTION_VARIABLES:
            return domain == BEHAVIOR_DOMAIN_EVENT ||
                   domain == BEHAVIOR_DOMAIN_FIELD ||
                   domain == BEHAVIOR_DOMAIN_EVENT_DEFINITION;
        default: return true;
    }
}

static const char *section_label(const studio *s, section sec)
{
    bool parameters = sec == SECTION_VARIABLES &&
                      sg_graph_editor(s)->asset->domain != BEHAVIOR_DOMAIN_EVENT;
    return parameters ? "Parameters" : SECTION_LABELS[sec];
}

/* "Default" is Unreal's uncategorized category. */
static const char *category_of(const behavior_variable *v)
{
    return v->category && v->category[0] && strcmp(v->category, "Default") ? v->category
                                                                           : NULL;
}

static bool same_category(const behavior_variable *a, const behavior_variable *b)
{
    const char *x = category_of(a), *y = category_of(b);
    return x == y || (x && y && !strcmp(x, y));
}

static bool variable_matches(const behavior_variable *v, const char *filter)
{
    return studio_matches(filter, v->name) ||
           (category_of(v) && studio_matches(filter, category_of(v)));
}

/* Uncategorized variables first, then each category in order of first use
   with its variables. Rows rebuild at every refresh Members shows, so a
   category is found among the categories seen, not every variable before. */
static int variable_rows(const behavior_asset *asset, const char *filter, member_row **rows)
{
    const behavior_variable *vars = asset->variables;
    int total = 0, *firsts = NULL; /* each category's first variable */
    for (int i = 0; i < acount(vars); i++) {
        bool seen = !category_of(&vars[i]);
        for (int c = 0; !seen && c < acount(firsts); c++) {
            seen = same_category(&vars[i], &vars[firsts[c]]);
        }
        if (!seen) {
            apush(firsts, i);
        }
        if (!category_of(&vars[i]) && variable_matches(&vars[i], filter)) {
            apush(*rows, ((member_row){ ROW_VARIABLE, i, 1, 0 }));
            total++;
        }
    }
    for (int c = 0; c < acount(firsts); c++) {
        int i = firsts[c], header = acount(*rows);
        apush(*rows, ((member_row){ ROW_CATEGORY, i, 1, 0 }));
        for (int j = i; j < acount(vars); j++) {
            if (same_category(&vars[i], &vars[j]) &&
                variable_matches(&vars[j], filter)) {
                apush(*rows, ((member_row){ ROW_VARIABLE, j, 2, 0 }));
                (*rows)[header].count++;
            }
        }
        total += (*rows)[header].count;
        if (!(*rows)[header].count) {
            asetlen(*rows, header);
        }
    }
    afree(firsts);
    return total;
}

static bool event_node(const node *n)
{
    return n && n->desc && n->desc->display_class == NODE_DISPLAY_EVENT &&
           n->reference_name && n->reference_name[0];
}

/* Whether an Event node for this event comes before node `index` of graph
   `at`; each event lists once. */
static bool event_listed(const behavior_asset *asset, int at, int index, const char *name)
{
    for (int g = 0; g <= at; g++) {
        const graph *gr = asset->graphs[g];
        for (int i = 0; i < (g == at ? index : acount(gr->nodes)); i++) {
            if (event_node(gr->nodes[i]) &&
                !strcmp(gr->nodes[i]->reference_name, name)) {
                return true;
            }
        }
    }
    return false;
}

/* The events a behavior's event graphs handle, or an event definition's own. */
static int event_rows(const studio *s, member_row **rows)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    const char *filter = s->members.filter;
    if (asset->domain == BEHAVIOR_DOMAIN_EVENT_DEFINITION) {
        bool shown = studio_matches(filter, s->scene->graph_document_id);
        if (shown) {
            apush(*rows, ((member_row){ ROW_EVENT, -1, 1, 0, 0 }));
        }
        return shown;
    }
    int count = 0;
    for (int g = 0; g < acount(asset->graphs); g++) {
        const graph *gr = asset->graphs[g];
        for (int i = 0; gr->kind == GRAPH_EVENT && i < acount(gr->nodes); i++) {
            const node *n = gr->nodes[i];
            const char *name = event_node(n) ? n->reference_name : NULL;
            if (name && studio_matches(filter, name) &&
                !event_listed(asset, g, i, name)) {
                apush(*rows, ((member_row){ ROW_EVENT, g, 1, 0, i }));
                count++;
            }
        }
    }
    return count;
}

static int section_rows(const studio *s, section sec, member_row **rows)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    const graph *active = sg_active_graph(s);
    const char *filter = s->members.filter;
    int count = 0;
    if (sec == SECTION_VARIABLES) {
        return variable_rows(asset, filter, rows);
    }
    if (sec == SECTION_EVENTS) {
        return event_rows(s, rows);
    }
    if (sec == SECTION_VALUES) {
        for (int i = 0; i < acount(active->locals); i++) {
            if (studio_matches(filter, active->locals[i].name)) {
                apush(*rows, ((member_row){ ROW_LOCAL, i, 1, 0 }));
                count++;
            }
        }
        return count;
    }
    for (int i = 0; i < acount(asset->graphs); i++) {
        const graph *g = asset->graphs[i];
        /* An expression's generated function belongs to its node. */
        if (!g->math_expression && graph_section(g) == sec &&
            studio_matches(filter, graph_label(g))) {
            apush(*rows, ((member_row){ ROW_GRAPH, i, 1, 0 }));
            count++;
        }
    }
    return count;
}

/* Sections with their members; an empty section shows while it can take a
   new member and nothing is filtered. */
static member_row *member_rows(const studio *s)
{
    const studio_members *m = &s->members;
    behavior_domain domain = sg_graph_editor(s)->asset->domain;
    member_row *rows = NULL, *items = NULL;
    for (int sec = 0; sec < SECTION_COUNT; sec++) {
        asetlen(items, 0);
        int count = shows((section)sec, domain)
                      ? section_rows(s, (section)sec, &items)
                      : 0;
        bool addable = sec == SECTION_FUNCTIONS || sec == SECTION_MACROS ||
                       sec == SECTION_VARIABLES;
        if (!shows((section)sec, domain) || (!count && (m->filter[0] || !addable))) {
            continue;
        }
        apush(rows, ((member_row){ ROW_SECTION, sec, 0, count }));
        if (m->filter[0] || !(m->collapsed & 1u << sec)) {
            for (int i = 0; i < acount(items); i++) {
                apush(rows, items[i]);
            }
        }
    }
    afree(items);
    return rows;
}

pin_spec sm_variable_pin(const behavior_variable *v)
{
    return (pin_spec){ .name = v->name,
                       .type = v->type,
                       .type_name = v->type_name,
                       .kind = PIN_DATA,
                       .container = v->container,
                       .default_value = v->default_value };
}

/* ---------- Uses ---------- */

static const char *const ROLE_WORDS[REFERENCE_ROLE_COUNT] = {
    [REFERENCE_USE] = "use",           [REFERENCE_CALL] = "call",
    [REFERENCE_ARGUMENT] = "argument", [REFERENCE_READ] = "read",
    [REFERENCE_WRITE] = "write",       [REFERENCE_SEND] = "sender",
    [REFERENCE_HANDLE] = "handler",    [REFERENCE_SPAWN] = "spawn",
    [REFERENCE_ATTACH] = "attachment", [REFERENCE_BIND] = "binding",
};

static bool same_member(const struct studio_member_uses *uses, const reference_symbol *symbol)
{
    if (uses->kind != symbol->kind) {
        return false;
    }
    return symbol->kind == REFERENCE_SYMBOL_EVENT
             ? !strcmp(uses->event, symbol->name)
             : uses->graph_id == symbol->graph_id && uses->id == symbol->id;
}

static struct studio_member_uses *uses_of(studio_members *m, const reference_symbol *symbol)
{
    for (int i = 0; i < acount(m->uses.items); i++) {
        if (same_member(&m->uses.items[i], symbol)) {
            return &m->uses.items[i];
        }
    }
    return NULL;
}

/* Tallies by role the uses of the document's graphs, variables and locals,
   and of every event. */
static void count_uses(studio_members *m, const reference_index *index, int asset)
{
    asetlen(m->uses.items, 0);
    for (int i = 0; asset >= 0 && i < reference_index_count(index); i++) {
        const reference *r = reference_index_at(index, i);
        const reference_symbol *symbol = &r->symbol;
        bool event = symbol->kind == REFERENCE_SYMBOL_EVENT;
        bool member = symbol->asset == asset &&
                      (symbol->kind == REFERENCE_SYMBOL_GRAPH ||
                       symbol->kind == REFERENCE_SYMBOL_VARIABLE ||
                       symbol->kind == REFERENCE_SYMBOL_LOCAL);
        if (r->role == REFERENCE_DEFINE || (!event && !member)) {
            continue;
        }
        struct studio_member_uses *uses = uses_of(m, symbol);
        if (!uses) {
            struct studio_member_uses added = { .kind = symbol->kind,
                                                .graph_id = symbol->graph_id,
                                                .id = symbol->id };
            snprintf(added.event, sizeof added.event, "%s", event ? symbol->name : "");
            apush(m->uses.items, added);
            uses = &alast(m->uses.items);
        }
        uses->roles[r->role]++;
    }
}

/* Counts follow the index's builds and the open document. */
static const struct studio_member_uses *member_uses(studio *s, const reference_symbol *symbol)
{
    studio_members *m = &s->members;
    unsigned built = s->scene->asset_index_built_generation;
    int asset = studio_document_asset(s);
    if (!m->uses.counted || m->uses.generation != built || m->uses.asset != asset) {
        count_uses(m, app_scene_authoring_asset_index(s->scene), asset);
        m->uses.counted = true;
        m->uses.generation = built;
        m->uses.asset = asset;
    }
    return uses_of(m, symbol);
}

/* A graph, variable or local of the open document. */
static reference_symbol own_symbol(const studio *s, reference_symbol_kind kind, uint64_t graph_id, uint64_t id)
{
    return (reference_symbol){ .kind = kind,
                               .asset = studio_document_asset(s),
                               .graph_id = graph_id,
                               .id = id };
}

/* An event row's event: the one its Event node handles, or an event
   definition's own. */
static const char *event_name(const studio *s, const member_row *row)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    return row->index >= 0 ? asset->graphs[row->index]->nodes[row->node]->reference_name
                           : s->scene->graph_document_id;
}

/* The symbol whose uses a row counts, and its title; false for a row that
   counts none. Event and field parameters are read by name, not through
   Get nodes, and an event graph is the behavior itself. */
static bool row_symbol(const studio *s, const member_row *row, reference_symbol *symbol, const char **title)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    const graph *active = sg_active_graph(s);
    switch (row->kind) {
        case ROW_GRAPH: {
            const graph *g = asset->graphs[row->index];
            *symbol = own_symbol(s, REFERENCE_SYMBOL_GRAPH, 0, g->graph_id);
            *title = graph_label(g);
            return g->kind != GRAPH_EVENT;
        }
        case ROW_VARIABLE: {
            const behavior_variable *v = &asset->variables[row->index];
            *symbol = own_symbol(s, REFERENCE_SYMBOL_VARIABLE, 0, v->id);
            *title = v->name;
            return asset->domain == BEHAVIOR_DOMAIN_EVENT;
        }
        case ROW_LOCAL: {
            const graph_local *local = &active->locals[row->index];
            *symbol = own_symbol(
                s,
                REFERENCE_SYMBOL_LOCAL,
                active->graph_id,
                local->id
            );
            *title = local->name;
            return true;
        }
        case ROW_EVENT:
            *title = event_name(s, row);
            *symbol = (reference_symbol){ .kind = REFERENCE_SYMBOL_EVENT,
                                          .asset = -1,
                                          .name = *title };
            return true;
        default: return false;
    }
}

/* A member's use count and the tip that splits it by role. Events count
   every sender and handler. */
static void uses_text(studio *s, const reference_symbol *symbol, char label[16], char tip[160])
{
    const struct studio_member_uses *uses = member_uses(s, symbol);
    int total = 0;
    tip[0] = '\0';
    for (int role = 0; uses && role < REFERENCE_ROLE_COUNT; role++) {
        int count = uses->roles[role];
        size_t used = strlen(tip);
        if (count) {
            snprintf(
                tip + used,
                160 - used,
                "%s%d %s%s",
                total ? ", " : "",
                count,
                ROLE_WORDS[role],
                count == 1 ? "" : "s"
            );
        }
        total += count;
    }
    snprintf(label, 16, "%d", total);
    if (!total) {
        snprintf(tip, 160, "No uses");
    }
}

/* ---------- Rows ---------- */

static bool renaming(const studio *s, studio_member_kind kind, uint64_t id)
{
    return s->members.rename_kind == kind && s->members.rename_id == id;
}

/* The member a row shows; NONE for sections, categories and events. */
static studio_member_kind row_member(const studio *s, const member_row *row, uint64_t *id)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    switch (row->kind) {
        case ROW_GRAPH:
            *id = asset->graphs[row->index]->graph_id;
            return asset->graphs[row->index]->kind == GRAPH_EVENT
                     ? STUDIO_MEMBER_NONE
                     : STUDIO_MEMBER_GRAPH;
        case ROW_VARIABLE:
            *id = asset->variables[row->index].id;
            return STUDIO_MEMBER_VARIABLE;
        case ROW_LOCAL:
            *id = sg_active_graph(s)->locals[row->index].id;
            return STUDIO_MEMBER_LOCAL;
        default: return STUDIO_MEMBER_NONE;
    }
}

/* A row's name for the actions it sends: its kind's letter and what it
   shows, as "v12" for variable 12 or "s4" for the Variables section. */
static void row_key(const studio *s, const member_row *row, char *out, size_t size)
{
    uint64_t id = 0;
    row_member(s, row, &id);
    switch (row->kind) {
        case ROW_SECTION: snprintf(out, size, "s%d", row->index); return;
        case ROW_CATEGORY: snprintf(out, size, "c%d", row->index); return;
        case ROW_GRAPH:
            snprintf(out, size, "g%llu", (unsigned long long)id);
            return;
        case ROW_VARIABLE:
            snprintf(out, size, "v%llu", (unsigned long long)id);
            return;
        case ROW_LOCAL:
            snprintf(out, size, "l%llu", (unsigned long long)id);
            return;
        case ROW_EVENT:
            snprintf(out, size, "e%d.%d", row->index, row->node);
            return;
    }
}

/* The row named `key` among those Members shows now. */
static bool row_named(const studio *s, const char *key, member_row *out)
{
    member_row *rows = member_rows(s);
    bool found = false;
    for (int i = 0; !found && i < acount(rows); i++) {
        char named[40];
        row_key(s, &rows[i], named, sizeof named);
        found = !strcmp(named, key);
        if (found) {
            *out = rows[i];
        }
    }
    afree(rows);
    return found;
}

/* What each item of a list holds: its texts, its flags and, for a row,
   its depth. Every row has them all, so data-for reuses its elements as
   kinds change under them. */
typedef struct
{
    const char *const *texts, *const *flags; /* NULL-terminated */
    bool depth;
} item_shape;

static const char *const ROW_TEXTS[] = { "key",    "label", "icon", "meta",
                                         "swatch", "color", "uses", "usesTip",
                                         "add",    "drag",  NULL };
static const char *const ROW_FLAGS[] = { "parent", "open", "active", "unused", NULL };
static const char *const ITEM_TEXTS[] = { "id", "icon", "label", "shortcut", NULL };
static const char *const ITEM_FLAGS[] = { "checked", "disabled", NULL };
static const item_shape ROW_SHAPE = { ROW_TEXTS, ROW_FLAGS, true };
static const item_shape ITEM_SHAPE = { ITEM_TEXTS, ITEM_FLAGS, false };

static yyjson_mut_val *item_new(yyjson_mut_doc *doc, const item_shape *shape)
{
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    for (const char *const *text = shape->texts; *text; text++) {
        yyjson_mut_obj_add_strcpy(doc, item, *text, "");
    }
    for (const char *const *flag = shape->flags; *flag; flag++) {
        yyjson_mut_obj_add_bool(doc, item, *flag, false);
    }
    if (shape->depth) {
        yyjson_mut_obj_add_int(doc, item, "depth", 0);
    }
    return item;
}

void studio_members_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *m = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "members");
    yyjson_mut_obj_add_bool(doc, m, "shown", false);
    yyjson_mut_obj_add_bool(doc, m, "locked", false);
    yyjson_mut_obj_add_strcpy(doc, m, "filter", "");
    yyjson_mut_obj_add_arr(doc, m, "rows");
}

void studio_members_ui_add(yyjson_mut_doc *doc)
{
    yyjson_mut_val *m = yyjson_mut_obj_add_obj(doc, yyjson_mut_doc_get_root(doc), "members_ui");
    yyjson_mut_obj_add_strcpy(doc, m, "selected", "");
    yyjson_mut_obj_add_strcpy(doc, m, "active", "");
    yyjson_mut_obj_add_strcpy(doc, m, "renaming", "");
    yyjson_mut_obj_add_arr(doc, m, "menu");
    yyjson_mut_obj_add_val(doc, m, "remove", item_new(doc, &ITEM_SHAPE));
}

/* ---------- Values, written in place ---------- */

static bool put_glyph(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *name, uint32_t icon)
{
    char glyph[5] = "";
    if (icon) {
        studio_glyph(glyph, icon);
    }
    return ui_data_put_text(doc, object, name, glyph);
}

/* Gives `array` `length` items, adding and removing at its end, so items
   already shown keep their elements; true when its length changed. */
static bool fit(yyjson_mut_doc *doc, yyjson_mut_val *array, int length, const item_shape *shape)
{
    int had = (int)yyjson_mut_arr_size(array);
    for (int i = had; i < length; i++) {
        yyjson_mut_arr_append(array, item_new(doc, shape));
    }
    for (int i = had; i > length; i--) {
        yyjson_mut_arr_remove_last(array);
    }
    return had != length;
}

/* ---------- One row ---------- */

/* What a row shows besides its key and uses. */
typedef struct
{
    const char *label, *meta, *add;
    uint32_t icon;
    const pin_spec *pin; /* its type, for a swatch */
    bool open, draggable;
} row_look;

static row_look section_look(const studio *s, const member_row *row, char *meta, size_t size)
{
    static const char *const ADD_TIPS[SECTION_COUNT] = {
        [SECTION_FUNCTIONS] = "New Function",
        [SECTION_MACROS] = "New Macro",
        [SECTION_VARIABLES] = "New Variable",
    };
    snprintf(meta, size, "%d", row->count);
    return (row_look){ .label = section_label(s, (section)row->index),
                       .meta = meta,
                       .add = ADD_TIPS[row->index],
                       .open = s->members.filter[0] ||
                               !(s->members.collapsed & 1u << row->index) };
}

/* A function or macro drags onto the canvas as its call. */
static row_look graph_look(const studio *s, const member_row *row)
{
    const editor *e = sg_graph_editor(s);
    const graph *g = e->asset->graphs[row->index];
    return (row_look){
        .label = graph_label(g),
        .icon = g->kind == GRAPH_FUNCTION ? KIT_ICON_FUNCTION
              : g->kind == GRAPH_MACRO    ? KIT_ICON_EXPRESSION
                                          : KIT_ICON_EVENT,
        .meta = g->kind == GRAPH_FUNCTION && graph_signature_pure(g) ? "pure" : "",
        .draggable = g->kind != GRAPH_EVENT,
    };
}

/* Get and Set nodes read a behavior's own variables, not an event's or a
   field's parameters. */
static row_look variable_look(const studio *s, const member_row *row, pin_spec *pin, char *meta, size_t size)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    *pin = sm_variable_pin(&asset->variables[row->index]);
    return (row_look){ .label = pin->name,
                       .icon = KIT_ICON_VARIABLE,
                       .meta = sm_type_label(pin, meta, size),
                       .pin = pin,
                       .draggable = asset->domain == BEHAVIOR_DOMAIN_EVENT };
}

static row_look local_look(const studio *s, const member_row *row, char *meta, size_t size)
{
    graph *gr = sg_active_graph(s);
    const graph_local *local = &gr->locals[row->index];
    int output = 0, index = -1;
    const pin_spec *pin = graph_pin_by_id(
        graph_node_by_id(gr, local->source_id),
        local->source_pin_id,
        &output,
        &index
    );
    return (row_look){ .label = local->name,
                       .icon = KIT_ICON_LOCAL,
                       .meta = pin ? sm_type_label(pin, meta, size) : "",
                       .pin = pin };
}

static row_look row_look_of(const studio *s, const member_row *row, pin_spec *pin, char *meta, size_t size)
{
    const behavior_asset *asset = sg_graph_editor(s)->asset;
    switch (row->kind) {
        case ROW_SECTION: return section_look(s, row, meta, size);
        case ROW_CATEGORY:
            snprintf(meta, size, "%d", row->count);
            return (row_look){ .label = category_of(&asset->variables[row->index]),
                               .icon = KIT_ICON_FOLDER,
                               .meta = meta };
        case ROW_GRAPH: return graph_look(s, row);
        case ROW_VARIABLE: return variable_look(s, row, pin, meta, size);
        case ROW_LOCAL: return local_look(s, row, meta, size);
        case ROW_EVENT:
            if (row->index >= 0) {
                const graph *g = asset->graphs[row->index];
                return (row_look){ .label = g->nodes[row->node]->reference_name,
                                   .icon = KIT_ICON_EVENT };
            }
            return (row_look){ .label = event_name(s, row), .icon = KIT_ICON_EVENT };
    }
    return (row_look){ 0 };
}

/* A type swatch colored as the canvas colors its sockets. */
static bool show_swatch(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *item, const pin_spec *pin)
{
    char color[16] = "";
    const char *shape = "";
    if (pin) {
        snprintf(color, sizeof color, "#%08x", sg_pin_color(s, pin));
        shape = pin->kind == PIN_EXEC                  ? "arrow"
              : pin->container == PIN_CONTAINER_SINGLE ? "circle"
                                                       : "square";
    }
    bool changed = ui_data_put_text(doc, item, "swatch", shape);
    return ui_data_put_text(doc, item, "color", color) || changed;
}

static bool show_uses(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *item, const member_row *row)
{
    reference_symbol symbol;
    const char *title;
    char label[16] = "", tip[160] = "";
    if (row_symbol(s, row, &symbol, &title)) {
        uses_text(s, &symbol, label, tip);
    }
    bool changed = ui_data_put_text(doc, item, "uses", label);
    changed |= ui_data_put_text(doc, item, "usesTip", tip);
    return ui_data_put_bool(item, "unused", !strcmp(label, "0")) || changed;
}

/* Whether `row` is the open graph's. */
static bool open_graph(const studio *s, const member_row *row)
{
    return row->kind == ROW_GRAPH &&
           sg_graph_editor(s)->asset->graphs[row->index] == sg_active_graph(s);
}

/* A row: what it shows, but whether it is selected, active or being
   named, which members_ui holds by key, so a selection changes no row;
   the open graph's row is active by itself, as opening a graph changes
   the rows anyway. A drag's payload is its key. */
static bool show_row(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *item, const member_row *row)
{
    pin_spec pin;
    char meta[512], key[40];
    row_look look = row_look_of(s, row, &pin, meta, sizeof meta);
    row_key(s, row, key, sizeof key);
    bool changed = ui_data_put_text(doc, item, "key", key);
    changed |= ui_data_put_text(doc, item, "label", look.label);
    changed |= put_glyph(doc, item, "icon", look.icon);
    changed |= ui_data_put_text(doc, item, "meta", look.meta ? look.meta : "");
    changed |= ui_data_put_text(doc, item, "add", look.add ? look.add : "");
    changed |= ui_data_put_text(doc, item, "drag", look.draggable ? "member" : "");
    changed |= ui_data_put_int(item, "depth", row->depth);
    changed |= ui_data_put_bool(item, "parent", row->kind == ROW_SECTION);
    changed |= ui_data_put_bool(item, "open", look.open);
    changed |= ui_data_put_bool(item, "active", open_graph(s, row));
    changed |= show_swatch(s, doc, item, look.pin);
    return show_uses(s, doc, item, row) || changed;
}

/* ---------- The panel ---------- */

/* The List's id in Members.rml. */
#define MEMBERS_LIST "members-list"
/* Rows shown before the List is laid out. */
#define UNLAID_ROWS 40

/* The element with `id` in Studio's documents, or NULL. */
static rml_element *studio_by_id(studio *s, const char *id)
{
    rml_context *context = rml_backend_context(s->backend);
    for (int i = 0; i < rml_context_document_count(context); i++) {
        rml_element *found = rml_element_by_id(rml_context_document(context, i), id);
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* The List, found by id once while it lives: the search walks every
   document, and the source syncs at every refresh. */
static rml_element *members_list(studio *s)
{
    studio_members *m = &s->members;
    rml_element *list = m->list_element ? rml_element_ref_get(m->list_element) : NULL;
    if (!list && (list = studio_by_id(s, MEMBERS_LIST))) {
        rml_element_ref_destroy(m->list_element);
        m->list_element = rml_element_ref_create(list);
    }
    return list;
}

/* The row to scroll into view: a member being named as it is added, or
   one Go to Definition selected; -1 for none. */
static int reveal_row(studio *s, const member_row *rows)
{
    studio_members *m = &s->members;
    int reveal = -1;
    for (int r = 0; r < acount(rows); r++) {
        uint64_t id = 0;
        studio_member_kind kind = row_member(s, &rows[r], &id);
        bool named = m->rename_focus && kind && renaming(s, kind, id);
        bool selected = m->reveal && kind && kind == m->kind && id == m->id;
        reveal = named || selected ? r : reveal;
    }
    m->rename_focus = false;
    m->reveal = false;
    return reveal;
}

/* The rows of the List in view, as ui_list_window places them. */
static bool show_rows(studio *s, const studio_panel_sync *at, yyjson_mut_val *array)
{
    member_row *rows = member_rows(s);
    int count = acount(rows), reveal = reveal_row(s, rows);
    rml_element *list = at->preview ? NULL : members_list(s);
    int first = 0, length = count < UNLAID_ROWS ? count : UNLAID_ROWS;
    if (list) {
        ui_list window = { .count = count, .reveal = reveal };
        length = ui_list_window(list, &window, &s->members.list, &first);
    }
    bool changed = fit(at->doc, array, length, &ROW_SHAPE);
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(array);
    for (int i = first; (item = yyjson_mut_arr_iter_next(&it)); i++) {
        changed |= show_row(s, at->doc, item, &rows[i]);
    }
    afree(rows);
    return changed;
}

/* A menu item: what it runs (members.<id>), and how it shows. */
typedef struct
{
    const char *id, *label, *shortcut;
    uint32_t icon;
    bool shown, disabled;
} menu_item;

static bool show_item(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *item, const menu_item *shown)
{
    bool changed = ui_data_put_text(doc, item, "id", shown->id);
    changed |= ui_data_put_text(doc, item, "label", shown->label);
    changed |= put_glyph(doc, item, "icon", shown->icon);
    changed |= ui_data_put_text(
        doc,
        item,
        "shortcut",
        shown->shortcut ? studio_chord(s, shown->shortcut) : ""
    );
    return ui_data_put_bool(item, "disabled", shown->disabled) || changed;
}

/* The selected member's menu: what it can do, and Delete apart. */
static bool show_menu(studio *s, yyjson_mut_doc *doc, yyjson_mut_val *m)
{
    graph *g = sm_graph(s);
    bool readonly = locked(s), variable = s->members.kind == STUDIO_MEMBER_VARIABLE;
    bool callable = g && (g->kind == GRAPH_FUNCTION || g->kind == GRAPH_MACRO);
    const menu_item items[] = {
        { "open", "Open", NULL, KIT_ICON_EXTERNAL, g != NULL, false },
        { "rename", "Rename", "F2", KIT_ICON_EDIT, true, readonly },
        { "duplicate", "Duplicate", "Shift+D", KIT_ICON_COPY, variable, readonly },
        { "refs", "Find References", NULL, KIT_ICON_REFERENCES, variable || callable, false },
        { "calls", "Call Hierarchy", NULL, KIT_ICON_CALLERS, callable, false },
    };
    const menu_item remove = { "delete",       "Delete", "X",
                               KIT_ICON_TRASH, true,     readonly };
    int length = 0;
    for (size_t i = 0; i < sizeof items / sizeof *items; i++) {
        length += items[i].shown;
    }
    yyjson_mut_val *menu = yyjson_mut_obj_get(m, "menu");
    bool changed = fit(doc, menu, length, &ITEM_SHAPE);
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(menu);
    for (size_t i = 0; i < sizeof items / sizeof *items; i++) {
        if (items[i].shown) {
            changed |= show_item(s, doc, yyjson_mut_arr_iter_next(&it), &items[i]);
        }
    }
    return show_item(s, doc, yyjson_mut_obj_get(m, "remove"), &remove) || changed;
}

/* The filter is view state the header's field binds; C keeps it too, so
   it filters rows and outlasts the field. */
static bool sync_filter(studio *s, const studio_panel_sync *at)
{
    studio_members *m = &s->members;
    yyjson_mut_val *filter = yyjson_mut_obj_get(at->variable, "filter");
    if (at->wrote && strcmp(yyjson_mut_get_str(filter), m->filter)) {
        snprintf(m->filter, sizeof m->filter, "%s", yyjson_mut_get_str(filter));
    }
    /* Find (Mod+F) focuses the field. */
    rml_element *field = s->find_request == STUDIO_EDITOR_MEMBERS + 1 && !at->preview
                           ? studio_by_id(s, "members-filter")
                           : NULL;
    if (field && rml_element_focus(field, true)) {
        rml_element_select(field);
        s->find_request = 0;
    }
    return ui_data_put_text(at->doc, at->variable, "filter", m->filter);
}

bool studio_members_sync(studio *s, const studio_panel_sync *at)
{
    yyjson_mut_val *m = at->variable;
    bool shown = studio_graph_active(s);
    bool changed = sync_filter(s, at);
    changed |= ui_data_put_bool(m, "shown", shown);
    if (!shown) {
        s->members.rename_kind = STUDIO_MEMBER_NONE;
        return fit(at->doc, yyjson_mut_obj_get(m, "rows"), 0, &ROW_SHAPE) || changed;
    }
    follow(s);
    changed |= ui_data_put_bool(m, "locked", locked(s));
    return show_rows(s, at, yyjson_mut_obj_get(m, "rows")) || changed;
}

/* A member's row key, as row_key names it; "" for none. */
static void member_key(studio_member_kind kind, uint64_t id, char *out, size_t size)
{
    static const char LETTERS[] = { [STUDIO_MEMBER_GRAPH] = 'g',
                                    [STUDIO_MEMBER_VARIABLE] = 'v',
                                    [STUDIO_MEMBER_LOCAL] = 'l' };
    out[0] = '\0';
    if (kind) {
        snprintf(out, size, "%c%llu", LETTERS[kind], (unsigned long long)id);
    }
}

/* What changes as the user selects and names: the selected member, the
   active one (the selection unless it is a graph, whose row is active
   while it is open) and the member being named by row key, and the
   selected member's menu. Rows bind them by key (Members.rml), so a
   selection re-evaluates these views alone, not every row's. */
bool studio_members_ui_sync(studio *s, const studio_panel_sync *at)
{
    studio_members *m = &s->members;
    yyjson_mut_val *ui = at->variable;
    char selected[40] = "", named[40] = "";
    if (studio_graph_active(s)) {
        follow(s);
        member_key(m->kind, m->id, selected, sizeof selected);
        member_key(m->rename_kind, m->rename_id, named, sizeof named);
    }
    bool changed = ui_data_put_text(at->doc, ui, "selected", selected);
    bool graph = m->kind == STUDIO_MEMBER_GRAPH;
    changed |= ui_data_put_text(at->doc, ui, "active", graph ? "" : selected);
    changed |= ui_data_put_text(at->doc, ui, "renaming", named);
    return show_menu(s, at->doc, ui) || changed;
}

/* ---------- Actions ---------- */

/* A press on a row: a section opens or closes, a graph opens, a member
   is selected for Properties, and an event reveals its node. */
static void press_row(studio *s, const member_row *row)
{
    editor *e = sg_graph_editor(s);
    uint64_t id = 0;
    studio_member_kind kind = row_member(s, row, &id);
    switch (row->kind) {
        case ROW_SECTION: s->members.collapsed ^= 1u << row->index; return;
        case ROW_GRAPH:
            if (e->asset->graphs[row->index] != e->active_graph) {
                editor_open_graph(e, id);
            }
            select_member(s, kind, id);
            return;
        case ROW_VARIABLE: select_member(s, kind, id); return;
        case ROW_LOCAL:
            select_local(s, &sg_active_graph(s)->locals[row->index]);
            return;
        case ROW_EVENT:
            if (row->index >= 0) {
                studio_reveal_node(
                    s,
                    e->asset->graphs[row->index]->graph_id,
                    e->asset->graphs[row->index]->nodes[row->node]->id
                );
            }
            return;
        case ROW_CATEGORY: return;
    }
}

/* A right press on a member selects it and opens its menu at the pointer. */
static void open_menu(studio *s, const member_row *row, const ui_activation *a)
{
    uint64_t id = 0;
    bool member = row_member(s, row, &id) != STUDIO_MEMBER_NONE;
    if (row->kind == ROW_GRAPH || member) {
        press_row(s, row);
    }
    rml_element *anchor = rml_element_ref_get(a->anchor);
    rml_element *menu = member && anchor
                          ? rml_element_query_selector(rml_element_owner_document(anchor), "#member-menu")
                          : NULL;
    if (menu) {
        popover_show(menu, &(popover_anchor){ .side = POPOVER_POINT, .box = { a->x, a->y, 0, 0 } });
    }
}

static void add_to(studio *s, const member_row *row)
{
    if (row->kind != ROW_SECTION || locked(s)) {
        return;
    }
    switch ((section)row->index) {
        case SECTION_VARIABLES: add_variable(s); return;
        case SECTION_FUNCTIONS:
            add_graph(s, GRAPH_FUNCTION, "New Function");
            return;
        case SECTION_MACROS: add_graph(s, GRAPH_MACRO, "New Macro"); return;
        default: return;
    }
}

static void list_uses(studio *s, const member_row *row)
{
    reference_symbol symbol;
    const char *title;
    if (row_symbol(s, row, &symbol, &title)) {
        studio_find_references(s, title, symbol);
    }
}

/* What a row's activation names: its press, double click, right press,
   use count or add button. */
static void row_action(studio *s, const char *action, const ui_activation *a)
{
    member_row row;
    if (a->argc < 1 || !studio_graph_active(s) || !row_named(s, a->args[0], &row)) {
        return;
    }
    uint64_t id = 0;
    studio_member_kind kind = row_member(s, &row, &id);
    if (!strcmp(action, "click")) {
        press_row(s, &row);
    } else if (!strcmp(action, "edit")) {
        start_rename(s, kind, id);
    } else if (!strcmp(action, "menu")) {
        open_menu(s, &row, a);
    } else if (!strcmp(action, "uses")) {
        list_uses(s, &row);
    } else if (!strcmp(action, "add")) {
        add_to(s, &row);
    }
}

/* The selected member's menu items. */
static bool menu_action(studio *s, const char *action)
{
    studio_members *m = &s->members;
    graph *g = sm_graph(s);
    if (!strcmp(action, "open") && g) {
        editor_open_graph(sg_graph_editor(s), g->graph_id);
    } else if (!strcmp(action, "rename")) {
        start_rename(s, m->kind, m->id);
    } else if (!strcmp(action, "duplicate")) {
        duplicate_variable(s);
    } else if (!strcmp(action, "refs") && g) {
        studio_find_graph_references(s, g);
    } else if (!strcmp(action, "refs") && m->kind == STUDIO_MEMBER_VARIABLE) {
        studio_find_variable_references(s, m->id);
    } else if (!strcmp(action, "calls") && g) {
        studio_calls_graph(s, g);
    } else if (!strcmp(action, "delete")) {
        delete_member(s);
    } else {
        return false;
    }
    return true;
}

void studio_members_action(studio *s, const char *action, const ui_activation *a)
{
    studio_members *m = &s->members;
    /* A scroll only wakes the source, which reads where the List is. */
    if (!studio_graph_active(s) || !strcmp(action, "scroll")) {
        return;
    }
    follow(s);
    /* The name field commits with Enter or by leaving it, and Escape keeps
       the name; it sends one or the other first. */
    if (!strcmp(action, "renamed") || !strcmp(action, "cancel")) {
        if (!strcmp(action, "renamed") && m->rename_kind && a->argc && a->args[0][0]) {
            rename_member(s, a->args[0]);
        }
        m->rename_kind = STUDIO_MEMBER_NONE;
        return;
    }
    if (!menu_action(s, action)) {
        row_action(s, action, a);
    }
}

/* ---------- Editor ---------- */

/* The header's filter field (MembersHeader.rml). */
void studio_members_header(studio *s, int area)
{
    studio_ui_area_controls(s, area, "filter", "MembersHeader");
}

void studio_members_body(studio *s, int area)
{
    studio_ui_area(s, area, "Members");
}

bool studio_members_key(studio *s, SDL_Keycode key, SDL_Keymod mod)
{
    if (!studio_graph_active(s) || (mod & SDL_KMOD_ALT)) {
        return false;
    }
    /* Duplicate is Shift+D or Mod+D, as for objects. */
    bool duplicate = key == SDLK_D &&
                     ((mod & SDL_KMOD_SHIFT) || studio_primary(s, mod));
    if (studio_primary(s, mod) && !duplicate) {
        return false;
    }
    follow(s);
    if (key == SDLK_F2) {
        start_rename(s, s->members.kind, s->members.id);
        return true;
    }
    if (key == SDLK_X || key == SDLK_DELETE || key == SDLK_BACKSPACE) {
        delete_member(s);
        return true;
    }
    if (duplicate) {
        duplicate_variable(s);
        return true;
    }
    return false;
}

/* ---------- Canvas drop ---------- */

static node *drop_node(studio *s, char kind, uint64_t id)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (kind == 'v') {
        bool set = s->mods & SDL_KMOD_ALT;
        return set ? behavior_asset_add_set_node_to_graph(e->asset, gr, id)
                   : behavior_asset_add_get_node_to_graph(e->asset, gr, id);
    }
    /* A function may call itself; a macro can't expand into itself. */
    graph *target = behavior_asset_find_graph(e->asset, id);
    if (!target || (target == gr && target->kind == GRAPH_MACRO)) {
        return NULL;
    }
    return target->kind == GRAPH_MACRO
             ? behavior_asset_add_macro_call_node_to_graph(e->asset, gr, target)
             : behavior_asset_add_function_call_node_to_graph(e->asset, gr, target);
}

/* What a drop does, under the drag's label: a variable places its Get, or
   its Set while Alt is held, and a function or macro its call. */
static const char *drop_hint(const studio *s, const char *payload)
{
    if (payload[0] != 'v') {
        return "Call";
    }
    return s->mods & SDL_KMOD_ALT ? "Set" : "Get (Alt: Set)";
}

void studio_members_drop(studio *s)
{
    char type[16], payload[32];
    bool readonly = sg_graph_readonly(s);
    if (!readonly &&
        kit_drop_hover(s->kit, type, sizeof type, payload, sizeof payload) &&
        !strcmp(type, "member")) {
        kit_drag_hint(s->kit, drop_hint(s, payload));
    }
    if (!kit_drop_target(s->kit, "member", payload, sizeof payload) || readonly) {
        return;
    }
    editor *e = sg_graph_editor(s);
    behavior_asset *before = ed_snapshot(e);
    node *n = drop_node(s, payload[0], strtoull(payload + 1, NULL, 10));
    if (!sm_record(e, before, n != NULL)) {
        return;
    }
    sg_to_world(s, s->in.mouse_x, s->in.mouse_y, &n->x, &n->y);
    n->placed = true;
    sg_select_only(e, n);
    s->focus_area = s->graph.area;
}

/* Its section opens, and Members shows in an area or the drawer. */
void studio_members_reveal(studio *s, studio_member_kind kind, uint64_t id)
{
    select_member(s, kind, id);
    graph *g = sm_graph(s);
    section sec = kind == STUDIO_MEMBER_VARIABLE ? SECTION_VARIABLES
                : kind == STUDIO_MEMBER_LOCAL    ? SECTION_VALUES
                : g                              ? graph_section(g)
                                                 : SECTION_GRAPHS;
    s->members.reveal = true;
    s->members.collapsed &= ~(1u << sec);
    studio_show_editor(s, STUDIO_EDITOR_MEMBERS);
}
