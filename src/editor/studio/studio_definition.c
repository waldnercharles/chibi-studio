/* Go to Definition (Unreal's Alt+G) from a graph node: a call opens its
 * graph, a Get or Set selects its variable in Members, an event node opens
 * its event definition, Spawn Prototype opens its prototype, and a component
 * node peeks its fields (VS Code's Peek Definition), each with its uses. */
#include "editor/studio/studio_graph_internal.h"
#include "graph/document/events.h"
#include "graph/nodes/component_nodes.h"

#include <stdio.h>

bool studio_node_definable(const studio *s, const node *n)
{
    return ed_is_graph_call(n) || studio_node_referable(s, n);
}

static void report(studio *s, const char *format, const char *name)
{
    char message[256];
    snprintf(message, sizeof message, format, name);
    studio_report(s, message);
}

static void open_asset(studio *s, int asset, const char *name)
{
    const reference_asset *
        found = reference_index_asset_at(app_scene_authoring_asset_index(s->scene), asset);
    if (!found || found->unresolved) {
        report(s, "No definition for %s", name);
        return;
    }
    finish_transform_edit(s->scene, false);
    if (app_scene_authoring_open_asset(s->scene, asset) != 0) {
        report(s, "Could not open %s", name);
    }
}

static void open_event(studio *s, reference_symbol event, const char *name)
{
    const reference *declared = reference_index_definition(
        app_scene_authoring_asset_index(s->scene),
        event
    );
    if (declared) {
        open_asset(s, declared->source, name);
        return;
    }
    report(s, game_host_event_reserved(name) ? "%s is a host event" : "No definition for %s", name);
}

static void peek(studio *s, const char *component)
{
    snprintf(s->peek.component, sizeof s->peek.component, "%s", component);
    s->peek.request = true;
    s->peek.x = s->in.mouse_x;
    s->peek.y = s->in.mouse_y;
}

void studio_go_to_definition(studio *s, node *n)
{
    if (ed_is_graph_call(n)) {
        editor_open_graph_call(sg_graph_editor(s), n->id);
        return;
    }
    const char *title;
    reference_symbol symbol = studio_node_symbol(s, n, &title);
    switch (symbol.kind) {
        case REFERENCE_SYMBOL_VARIABLE:
            studio_members_reveal(s, STUDIO_MEMBER_VARIABLE, symbol.id);
            return;
        case REFERENCE_SYMBOL_EVENT: open_event(s, symbol, title); return;
        case REFERENCE_SYMBOL_ASSET: open_asset(s, symbol.asset, title); return;
        case REFERENCE_SYMBOL_COMPONENT: peek(s, title); return;
        default: return;
    }
}

void studio_definition_peek(studio *s)
{
    kit *k = s->kit;
    if (s->peek.request) {
        kit_popup_open_at(k, "peek", s->peek.x, s->peek.y);
        s->peek.request = false;
    }
    if (!kit_popup_begin(k, "peek", "menu")) {
        return;
    }
    const ecs_component_desc *component = component_by_key(s->peek.component);
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    kit_menu_group(k, "title", s->peek.component);
    for (int i = 0; component && i < component->type->field_count; i++) {
        reference_symbol field = { .kind = REFERENCE_SYMBOL_FIELD,
                                   .asset = -1,
                                   .name = component->name,
                                   .field = component->type->fields[i].name };
        char key[16], uses[16];
        snprintf(key, sizeof key, "f%d", i);
        snprintf(uses, sizeof uses, "%d", reference_index_count_uses(index, field, REFERENCE_USES));
        kit_menu_item_desc d = { .label = field.field,
                                 .icon = KIT_ICON_FIELD,
                                 .shortcut = uses,
                                 .checked = -1 };
        if (kit_menu_item(k, key, &d)) {
            char title[256];
            snprintf(title, sizeof title, "%s.%s", component->name, field.field);
            studio_find_references(s, title, field);
        }
    }
    kit_popup_end(k);
}
