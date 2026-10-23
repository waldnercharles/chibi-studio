#include "assets/ui_events.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/graph/search.h"
#include "editor/workspace/editor_internal.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/local_edit.h"
#include "graph/document/events.h"
#include "graph/document/serialize.h"
#include "graph/model/local.h"
#include "graph/model/pin_conv.h"
#include "graph/model/registry.h"
#include "graph/model/value_copy.h"
#include "support/arena.h"
#include "support/asset_json.h"
#include "types/array.h"
#include "types/text.h"
#include "types/type.h"
#include "types/value_document.h"
#include "yyjson.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t next_asset_id(const editor *e)
{
    uint64_t id = e->asset->graph->id_counter;
    return id ? id : 1;
}

static const char *document_key(const editor *e)
{
    return e->document_key && e->document_key[0] ? e->document_key : e->asset->name;
}

static yyjson_mut_doc *action_start(const editor *e, const char *op, uint64_t graph_id)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "version", 1);
    yyjson_mut_obj_add_strcpy(doc, root, "family", "graph");
    yyjson_mut_obj_add_strcpy(doc, root, "op", op);
    yyjson_mut_obj_add_strcpy(doc, root, "document", document_key(e));
    yyjson_mut_obj_add_uint(doc, root, "graph", graph_id);
    return doc;
}

static char *action_finish(yyjson_mut_doc *doc)
{
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static yyjson_mut_val *action_root(yyjson_mut_doc *doc)
{
    return yyjson_mut_doc_get_root(doc);
}

/* Authored plain values cross the wire by value. Runtime handles never do. */
static bool action_value_valid(const pin_value *value, int depth)
{
    if (!value || depth > 16 || value->type <= PIN_FLOW || value->type > PIN_ENUM) {
        return false;
    }
    if (value->container != PIN_CONTAINER_SINGLE) {
        if (value->container != PIN_CONTAINER_ARRAY &&
            value->container != PIN_CONTAINER_SET) {
            return false;
        }
        const pin_array *array = value->ptr;
        if (!array) {
            return true;
        }
        if (!pin_array_valid(array) || array->element_type != value->type ||
            ((value->type_name || array->type_name) &&
             (!value->type_name || !array->type_name ||
              strcmp(value->type_name, array->type_name)))) {
            return false;
        }
        for (int i = 0; i < array->count; i++) {
            pin_value item = pin_array_get(array, i);
            if (item.container != PIN_CONTAINER_SINGLE || item.type != value->type ||
                (value->type == PIN_STRUCT &&
                 (!item.type_name || strcmp(item.type_name, value->type_name))) ||
                !action_value_valid(&item, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    if (value->type == PIN_STRUCT) {
        const type_desc *type = type_catalog_find(value->type_name);
        if (!type || !type_is_plain(type)) {
            return false;
        }
        if (!value->ptr) {
            return true; /* Authored null structs represent their typed zero default. */
        }
        for (int i = 0; i < type->field_count; i++) {
            arena storage = { 0 };
            pin_value field;
            bool valid = !pin_read_field(&storage, value->ptr, &type->fields[i], &field) &&
                         action_value_valid(&field, depth + 1);
            arena_free(&storage);
            if (!valid) {
                return false;
            }
        }
        return true;
    }
    if (value->type == PIN_F32) {
        return isfinite(value->f);
    }
    if (value->type == PIN_F64) {
        return isfinite(value->d);
    }
    if (value->type == PIN_HANDLE) {
        return !value->u64;
    }
    return value->type != PIN_NAME || value->str;
}

static bool write_pin_value(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key, const pin_value *value)
{
    if (!action_value_valid(value, 0)) {
        return false;
    }
    yyjson_mut_val *item = yyjson_mut_obj_add_obj(doc, root, key);
    yyjson_mut_obj_add_int(doc, item, "type", value->type);
    if (value->type_name) {
        yyjson_mut_obj_add_strcpy(doc, item, "type_name", value->type_name);
    }
    if (value->container != PIN_CONTAINER_SINGLE || value->type == PIN_STRUCT ||
        value->type == PIN_STR) {
        yyjson_mut_obj_add_int(doc, item, "container", value->container);
        av_doc *native = av_doc_new();
        av_value *data = av_obj(native);
        pin_value_write_document(native, data, "data", value->type, value->type_name, value);
        yyjson_mut_val *payload = av_to_json(doc, av_obj_get(data, "data"));
        bool valid = payload && yyjson_mut_obj_add_val(doc, item, "data", payload);
        av_doc_free(native);
        return valid;
    }
    switch (value->type) {
        case PIN_BOOL:
            yyjson_mut_obj_add_bool(doc, item, "data", value->b);
            break;
        case PIN_I8:
            yyjson_mut_obj_add_int(doc, item, "data", value->i8);
            break;
        case PIN_U8:
            yyjson_mut_obj_add_uint(doc, item, "data", value->u8);
            break;
        case PIN_I16:
            yyjson_mut_obj_add_int(doc, item, "data", value->i16);
            break;
        case PIN_U16:
            yyjson_mut_obj_add_uint(doc, item, "data", value->u16);
            break;
        case PIN_I32:
            yyjson_mut_obj_add_int(doc, item, "data", value->i);
            break;
        case PIN_U32:
            yyjson_mut_obj_add_uint(doc, item, "data", value->u);
            break;
        case PIN_I64:
            yyjson_mut_obj_add_sint(doc, item, "data", value->i64);
            break;
        case PIN_U64:
            yyjson_mut_obj_add_uint(doc, item, "data", value->u64);
            break;
        case PIN_F32:
            if (!isfinite(value->f)) {
                return false;
            }
            yyjson_mut_obj_add_real(doc, item, "data", value->f);
            break;
        case PIN_F64:
            if (!isfinite(value->d)) {
                return false;
            }
            yyjson_mut_obj_add_real(doc, item, "data", value->d);
            break;
        case PIN_NAME:
            if (!value->str) {
                return false;
            }
            yyjson_mut_obj_add_strcpy(doc, item, "data", value->str);
            break;
        case PIN_ENUM:
            yyjson_mut_obj_add_int(doc, item, "data", value->e);
            break;
        case PIN_HANDLE:
            if (value->u64) {
                return false;
            }
            yyjson_mut_obj_add_uint(doc, item, "data", 0);
            break;
        case PIN_FLOW:
        case PIN_STR:
        case PIN_STRUCT: return false;
    }
    return true;
}

static bool read_pin_value(yyjson_val *item, pin_value *out, graph *storage)
{
    yyjson_val *type = yyjson_obj_get(item, "type");
    yyjson_val *data = yyjson_obj_get(item, "data");
    const char *type_name = yyjson_get_str(yyjson_obj_get(item, "type_name"));
    if (!yyjson_is_obj(item) || !yyjson_is_int(type) || !data) {
        return false;
    }
    yyjson_val *container_json = yyjson_obj_get(item, "container");
    if (container_json && (!yyjson_is_int(container_json) ||
                           yyjson_get_sint(container_json) < PIN_CONTAINER_SINGLE ||
                           yyjson_get_sint(container_json) > PIN_CONTAINER_SET)) {
        return false;
    }
    pin_value value = { .type = yyjson_get_int(type),
                        .type_name = type_name,
                        .container = container_json ? yyjson_get_int(container_json)
                                                    : PIN_CONTAINER_SINGLE };
    if (value.type <= PIN_FLOW || value.type > PIN_ENUM) {
        return false;
    }
    if (value.container != PIN_CONTAINER_SINGLE || value.type == PIN_STRUCT ||
        value.type == PIN_STR) {
        if (value.type == PIN_STRUCT && !type_is_plain(type_catalog_find(type_name))) {
            return false;
        }
        av_doc *native = av_doc_new();
        av_value *native_data = av_from_json(native, data);
        arena owned = { 0 };
        pin_value decoded;
        char error[192];
        int result = pin_value_read_document(&owned, value, native_data, &decoded, error, sizeof error);
        if (!result) {
            result = pin_value_copy_to_graph(storage, &value, &decoded);
        }
        arena_free(&owned);
        av_doc_free(native);
        value.container = container_json ? yyjson_get_int(container_json)
                                         : PIN_CONTAINER_SINGLE;
        if (result || !action_value_valid(&value, 0)) {
            return false;
        }
        *out = value;
        return true;
    }
    switch (value.type) {
        case PIN_BOOL:
            if (!yyjson_is_bool(data)) {
                return false;
            }
            value.b = yyjson_get_bool(data);
            break;
        case PIN_I8:
            if (!yyjson_is_int(data) || yyjson_get_sint(data) < INT8_MIN ||
                yyjson_get_sint(data) > INT8_MAX) {
                return false;
            }
            value.i8 = (int8_t)yyjson_get_sint(data);
            break;
        case PIN_U8:
            if (!yyjson_is_uint(data) || yyjson_get_uint(data) > UINT8_MAX) {
                return false;
            }
            value.u8 = (uint8_t)yyjson_get_uint(data);
            break;
        case PIN_I16:
            if (!yyjson_is_int(data) || yyjson_get_sint(data) < INT16_MIN ||
                yyjson_get_sint(data) > INT16_MAX) {
                return false;
            }
            value.i16 = (int16_t)yyjson_get_sint(data);
            break;
        case PIN_U16:
            if (!yyjson_is_uint(data) || yyjson_get_uint(data) > UINT16_MAX) {
                return false;
            }
            value.u16 = (uint16_t)yyjson_get_uint(data);
            break;
        case PIN_I32:
            if (!yyjson_is_int(data) || yyjson_get_sint(data) < INT32_MIN ||
                yyjson_get_sint(data) > INT32_MAX) {
                return false;
            }
            value.i = (int32_t)yyjson_get_sint(data);
            break;
        case PIN_U32:
            if (!yyjson_is_uint(data) || yyjson_get_uint(data) > UINT32_MAX) {
                return false;
            }
            value.u = (uint32_t)yyjson_get_uint(data);
            break;
        case PIN_I64:
            if (!yyjson_is_int(data)) {
                return false;
            }
            value.i64 = yyjson_get_sint(data);
            break;
        case PIN_U64:
            if (!yyjson_is_uint(data)) {
                return false;
            }
            value.u64 = yyjson_get_uint(data);
            break;
        case PIN_F32:
            if (!yyjson_is_num(data) || !isfinite(yyjson_get_real(data))) {
                return false;
            }
            value.f = (float)yyjson_get_real(data);
            if (!isfinite(value.f)) {
                return false;
            }
            break;
        case PIN_F64:
            if (!yyjson_is_num(data) || !isfinite(yyjson_get_real(data))) {
                return false;
            }
            value.d = yyjson_get_real(data);
            break;
        case PIN_NAME:
            if (!yyjson_is_str(data) ||
                yyjson_get_len(data) != strlen(yyjson_get_str(data))) {
                return false;
            }
            value.str = yyjson_get_str(data);
            break;
        case PIN_ENUM:
            if (!yyjson_is_int(data) || yyjson_get_sint(data) < INT32_MIN ||
                yyjson_get_sint(data) > INT32_MAX) {
                return false;
            }
            value.e = (int32_t)yyjson_get_sint(data);
            break;
        case PIN_HANDLE:
            if (!yyjson_is_uint(data) || yyjson_get_uint(data)) {
                return false;
            }
            value.u64 = 0;
            break;
        case PIN_FLOW:
        case PIN_STR:
        case PIN_STRUCT: return false;
    }
    *out = value;
    return true;
}

static bool action_value_matches(const pin_spec *spec, const pin_value *value)
{
    return spec && value && spec->type == value->type &&
           spec->container == value->container &&
           (value->type != PIN_STRUCT || (spec->type_name && value->type_name &&
                                          !strcmp(spec->type_name, value->type_name)));
}

char *graph_collab_action_for_command(const editor *e, editor_command command)
{
    if (!e || !e->asset || !e->active_graph) {
        return NULL;
    }
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = NULL;
    switch (command.kind) {
        case EDITOR_COMMAND_CREATE_GRAPH:
            doc = action_start(e, "create", next_asset_id(e));
            root = action_root(doc);
            yyjson_mut_obj_add_int(doc, root, "kind", command.data.create_graph.graph_kind);
            if (command.data.create_graph.name) {
                yyjson_mut_obj_add_strcpy(
                    doc,
                    root,
                    "name",
                    command.data.create_graph.name
                );
            }
            break;
        case EDITOR_COMMAND_RENAME_GRAPH:
            doc = action_start(e, "rename", command.data.rename_graph.graph_id);
            root = action_root(doc);
            if (command.data.rename_graph.name) {
                yyjson_mut_obj_add_strcpy(
                    doc,
                    root,
                    "name",
                    command.data.rename_graph.name
                );
            }
            break;
        case EDITOR_COMMAND_SET_PIN_DEFAULT: {
            node *target = graph_node_by_id(
                e->active_graph,
                command.data.set_pin_default.node_id
            );
            int side = command.data.set_pin_default.output,
                index = command.data.set_pin_default.pin_index;
            const pin_spec *spec = target && (side == 0 || side == 1) && index >= 0 &&
                                           index < (side ? target->output_count
                                                         : target->input_count)
                                     ? &(side ? target->output_pins
                                              : target->input_pins)[index]
                                     : NULL;
            if (!action_value_matches(spec, command.data.set_pin_default.value)) {
                return NULL;
            }
            doc = action_start(e, "set_default", e->active_graph->graph_id);
            root = action_root(doc);
            yyjson_mut_obj_add_uint(
                doc,
                root,
                "node",
                command.data.set_pin_default.node_id
            );
            yyjson_mut_obj_add_int(
                doc,
                root,
                "output",
                command.data.set_pin_default.output
            );
            yyjson_mut_obj_add_int(
                doc,
                root,
                "pin",
                command.data.set_pin_default.pin_index
            );
            if (!write_pin_value(
                    doc,
                    root,
                    "value",
                    command.data.set_pin_default.value
                )) {
                yyjson_mut_doc_free(doc);
                return NULL;
            }
            break;
        }
        case EDITOR_COMMAND_CONNECT_DATA:
        case EDITOR_COMMAND_CONNECT_EXEC:
            doc = action_start(e, "connect", e->active_graph->graph_id);
            root = action_root(doc);
            yyjson_mut_obj_add_int(
                doc,
                root,
                "kind",
                command.kind == EDITOR_COMMAND_CONNECT_DATA ? PIN_DATA : PIN_EXEC
            );
            yyjson_mut_obj_add_uint(doc, root, "from_node", command.data.connect.from_node);
            yyjson_mut_obj_add_int(doc, root, "from_pin", command.data.connect.from_pin);
            yyjson_mut_obj_add_uint(doc, root, "to_node", command.data.connect.to_node);
            yyjson_mut_obj_add_int(doc, root, "to_pin", command.data.connect.to_pin);
            break;
        default: return NULL;
    }
    return action_finish(doc);
}

char *graph_collab_action_open(const editor *e, uint64_t graph_id)
{
    if (!e || !e->asset) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "open", graph_id);
    return action_finish(doc);
}

char *graph_collab_action_spawn(const editor *e, const node_desc *desc, float x, float y)
{
    if (!e || !e->asset || !e->active_graph || !desc || !desc->name ||
        registry_node_by_name(desc->name) != desc || !isfinite(x) || !isfinite(y)) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "spawn", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_strcpy(doc, root, "descriptor", desc->name);
    yyjson_mut_obj_add_uint(doc, root, "node", next_asset_id(e));
    yyjson_mut_obj_add_real(doc, root, "x", x);
    yyjson_mut_obj_add_real(doc, root, "y", y);
    return action_finish(doc);
}

char *graph_collab_action_spawn_match(const editor *e, const ed_search_match *match, float x, float y)
{
    if (!e || !e->asset || !e->active_graph || !match || !match->desc ||
        !match->desc->name || !isfinite(x) || !isfinite(y)) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "spawn_match", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_strcpy(doc, root, "descriptor", match->desc->name);
    yyjson_mut_obj_add_uint(doc, root, "node", next_asset_id(e));
    yyjson_mut_obj_add_real(doc, root, "x", x);
    yyjson_mut_obj_add_real(doc, root, "y", y);
    if (match->function) {
        yyjson_mut_obj_add_uint(doc, root, "target_graph", match->function->graph_id);
        if (match->function->owner && match->function->owner != e->asset) {
            yyjson_mut_obj_add_strcpy(
                doc,
                root,
                "target_asset",
                match->function->owner->name
            );
        }
    }
    if (match->variable) {
        yyjson_mut_obj_add_uint(doc, root, "variable", match->variable->id);
    }
    if (match->prototype) {
        yyjson_mut_obj_add_strcpy(doc, root, "prototype", match->prototype);
    }
    if (match->event) {
        yyjson_mut_obj_add_strcpy(doc, root, "event", match->event->name);
    }
    if (match->host_event) {
        if (match->host_event > e->event_catalog_count || !e->event_catalog) {
            yyjson_mut_doc_free(doc);
            return NULL;
        }
        yyjson_mut_obj_add_strcpy(
            doc,
            root,
            "host_event",
            e->event_catalog[match->host_event - 1].name
        );
    }
    if (match->ui_event) {
        yyjson_mut_obj_add_strcpy(doc, root, "ui_event", match->ui_event->name);
    }
    if (match->setting) {
        yyjson_mut_obj_add_strcpy(doc, root, "setting", match->setting);
    }
    if (match->field) {
        yyjson_mut_obj_add_strcpy(doc, root, "field", match->field);
    }
    if (e->spawn_link_pending) {
        yyjson_mut_val *link = yyjson_mut_obj_add_obj(doc, root, "link");
        yyjson_mut_obj_add_uint(doc, link, "node", e->spawn_link_pin.n->id);
        yyjson_mut_obj_add_int(doc, link, "kind", e->spawn_link_pin.kind);
        yyjson_mut_obj_add_int(doc, link, "pin", e->spawn_link_pin.index);
    }
    return action_finish(doc);
}

char *graph_collab_action_connect(const editor *e, node *a, int a_kind, int a_pin, node *b, int b_kind, int b_pin)
{
    if (!e || !e->active_graph || !a || !b || a_kind == b_kind) {
        return NULL;
    }
    node *from = a_kind == ED_PIN_OUTPUT ? a : b;
    int from_pin = a_kind == ED_PIN_OUTPUT ? a_pin : b_pin;
    node *to = a_kind == ED_PIN_INPUT ? a : b;
    int to_pin = a_kind == ED_PIN_INPUT ? a_pin : b_pin;
    if (from_pin < 0 || from_pin >= from->output_count || to_pin < 0 ||
        to_pin >= to->input_count) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "connect", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_int(doc, root, "kind", from->output_pins[from_pin].kind);
    yyjson_mut_obj_add_uint(doc, root, "from_node", from->id);
    yyjson_mut_obj_add_int(doc, root, "from_pin", from_pin);
    yyjson_mut_obj_add_uint(doc, root, "to_node", to->id);
    yyjson_mut_obj_add_int(doc, root, "to_pin", to_pin);
    return action_finish(doc);
}

static bool reconnect_valid(
    const editor *e,
    node *from,
    int from_pin,
    node *old_to,
    int old_pin,
    node *new_to,
    int new_pin
)
{
    if (!e || !e->asset || !e->active_graph || !behavior_asset_mutable(e->asset) ||
        !from || !old_to || graph_node_by_id(e->active_graph, from->id) != from ||
        graph_node_by_id(e->active_graph, old_to->id) != old_to ||
        from_pin < 0 || from_pin >= from->output_count || old_pin < 0 ||
        old_pin >= old_to->input_count) {
        return false;
    }
    bool found = false;
    for (int i = 0; i < acount(e->active_graph->edges); i++) {
        const edge *wire = &e->active_graph->edges[i];
        if (wire->from_id == from->id && wire->to_id == old_to->id &&
            wire->from_pin_id == from->output_pins[from_pin].id &&
            wire->to_pin_id == old_to->input_pins[old_pin].id) {
            found = true;
            break;
        }
    }
    return found && (!new_to || (graph_node_by_id(e->active_graph, new_to->id) == new_to &&
                                 new_pin >= 0 && new_pin < new_to->input_count &&
                                 ed_connection_specs_valid(
                                     &from->output_pins[from_pin],
                                     &new_to->input_pins[new_pin],
                                     from->output_pins[from_pin].kind
                                 )));
}

char *graph_collab_action_reconnect(
    const editor *e,
    node *from,
    int from_pin,
    node *old_to,
    int old_pin,
    node *new_to,
    int new_pin
)
{
    if (!reconnect_valid(e, from, from_pin, old_to, old_pin, new_to, new_pin)) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "reconnect", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_uint(doc, root, "from_node", from->id);
    yyjson_mut_obj_add_int(doc, root, "from_pin", from_pin);
    yyjson_mut_obj_add_uint(doc, root, "old_node", old_to->id);
    yyjson_mut_obj_add_int(doc, root, "old_pin", old_pin);
    if (new_to) {
        yyjson_mut_obj_add_uint(doc, root, "to_node", new_to->id);
        yyjson_mut_obj_add_int(doc, root, "to_pin", new_pin);
    }
    return action_finish(doc);
}

bool graph_collab_reconnect_apply(
    editor *e,
    node *from,
    int from_pin,
    node *old_to,
    int old_pin,
    node *new_to,
    int new_pin
)
{
    if (e->presentation) {
        if (!from || !old_to || from_pin < 0 || from_pin >= from->output_count ||
            old_pin < 0 || old_pin >= old_to->input_count ||
            (new_to && (new_pin < 0 || new_pin >= new_to->input_count))) {
            ed_show_error(e, "Reconnect has stale source ports");
            return false;
        }
        if (from->output_pins[from_pin].kind == PIN_EXEC) {
            if (!new_to) {
                ed_show_error(e, "Edit the structured source body to remove this flow");
                return false;
            }
            return ed_connect(
                e, &(ed_pin_view){ from, ED_PIN_OUTPUT, from_pin },
                &(ed_pin_view){ new_to, ED_PIN_INPUT, new_pin }
            );
        }
        uint64_t graph = e->active_graph->graph_id;
        const source_presentation_origin *producer = authoring_source_presentation_origin(
            e->presentation, graph, from->id, from->output_pins[from_pin].id
        );
        const source_presentation_origin *previous = authoring_source_presentation_origin(
            e->presentation, graph, old_to->id, old_to->input_pins[old_pin].id
        );
        const source_presentation_origin *target = new_to
            ? authoring_source_presentation_origin(
                  e->presentation, graph, new_to->id, new_to->input_pins[new_pin].id
              ) : previous;
        if (!producer || !previous || !target) {
            ed_show_error(e, "Reconnect has no current source input");
            return false;
        }
        authoring_edit_request request = {
            .kind = new_to ? AUTHORING_EDIT_RECONNECT : AUTHORING_EDIT_DISCONNECT,
            .generation = authoring_source_edit_generation(e->presentation),
            .origin = target->identity,
            .previous = previous->identity,
            .producer = producer->identity,
            .producer_result = producer->result_index
        };
        return ed_source_edit(e, &request);
    }
    if (!reconnect_valid(e, from, from_pin, old_to, old_pin, new_to, new_pin)) {
        return false;
    }
    if (new_to == old_to && new_pin == old_pin) {
        return true;
    }
    behavior_asset *snapshot = ed_snapshot(e);
    int pending = e->edit_pending;
    uint64_t from_id = from->id, old_id = old_to->id;
    bool moved_exec = new_to && from->output_pins[from_pin].kind == PIN_EXEC;
    /* Exec outputs reject a second destination. Release the validated edge
       within the snapshot; a refused new connection restores it below. */
    if (moved_exec &&
        graph_remove_edge(e->active_graph, from, from_pin, old_to, old_pin) != 0) {
        behavior_asset_free(snapshot);
        return false;
    }
    bool connected = !new_to || ed_connect(
                                    e,
                                    &(ed_pin_view){ from, ED_PIN_OUTPUT, from_pin },
                                    &(ed_pin_view){ new_to, ED_PIN_INPUT, new_pin }
                                );
    if (connected) {
        from = graph_node_by_id(e->active_graph, from_id);
        old_to = graph_node_by_id(e->active_graph, old_id);
        if (!moved_exec && from && old_to) {
            (void)graph_remove_edge(e->active_graph, from, from_pin, old_to, old_pin);
        }
        e->edit_pending = 1;
        if (collab_record_history()) {
            if (snapshot) {
                ed_push_asset(&e->undo, snapshot);
            }
            ed_free_asset_stack(&e->redo);
        } else {
            behavior_asset_free(snapshot);
        }
        return true;
    }
    behavior_asset *current = ed_snapshot(e);
    if (snapshot && current && !behavior_asset_equal(current, snapshot)) {
        if (ed_adopt_behavior_asset(e, snapshot) != 0) {
            abort();
        }
        av_value_free(e->edit_revision_snapshot);
        e->edit_revision_snapshot = ed_value_snapshot(e->asset);
        e->edit_revision_asset = e->asset;
        e->edit_revision_initialized = 1;
    }
    behavior_asset_free(current);
    behavior_asset_free(snapshot);
    e->edit_pending = pending;
    return false;
}

char *graph_collab_action_reset_pin(const editor *e, const node *n, int pin_index, const pin_value *value)
{
    if (!e || !e->active_graph || !n || pin_index < 0 ||
        pin_index >= n->input_count || !value) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "reset_pin", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_uint(doc, root, "node", n->id);
    yyjson_mut_obj_add_int(doc, root, "pin", pin_index);
    if (!write_pin_value(doc, root, "value", value)) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return action_finish(doc);
}

static bool write_pin_binding(yyjson_mut_doc *doc, yyjson_mut_val *root, const pin_binding *binding)
{
    if (!binding) {
        yyjson_mut_obj_add_null(doc, root, "binding");
        return true;
    }
    yyjson_mut_val *value = yyjson_mut_obj_add_obj(doc, root, "binding");
    yyjson_mut_obj_add_int(doc, value, "kind", binding->kind);
    switch (binding->kind) {
        case PIN_BINDING_VARIABLE:
            if (!binding->variable_id) {
                return false;
            }
            yyjson_mut_obj_add_uint(doc, value, "variable", binding->variable_id);
            if (binding->source_asset) {
                yyjson_mut_obj_add_strcpy(doc, value, "source_asset", binding->source_asset);
            }
            return true;
        case PIN_BINDING_COMPONENT_FIELD:
            if (!binding->component_key || !binding->component_key[0] ||
                !binding->field_name || !binding->field_name[0]) {
                return false;
            }
            yyjson_mut_obj_add_strcpy(doc, value, "component", binding->component_key);
            yyjson_mut_obj_add_strcpy(doc, value, "field", binding->field_name);
            return true;
        case PIN_BINDING_COMPONENT:
            if (!binding->component_key || !binding->component_key[0] ||
                binding->field_name) {
                return false;
            }
            yyjson_mut_obj_add_strcpy(doc, value, "component", binding->component_key);
            return true;
        case PIN_BINDING_LOCAL:
            if (!binding->local_id) {
                return false;
            }
            yyjson_mut_obj_add_uint(doc, value, "local", binding->local_id);
            return true;
        case PIN_BINDING_EVENT_OUTPUT:
            if (!binding->event_node_id || !binding->event_pin_id) {
                return false;
            }
            yyjson_mut_obj_add_uint(doc, value, "event_node", binding->event_node_id);
            yyjson_mut_obj_add_uint(doc, value, "event_pin", binding->event_pin_id);
            return true;
    }
    return false;
}

static bool graph_binding_allowed(const editor *e, const pin_binding *binding)
{
    if (!binding || binding->kind == PIN_BINDING_LOCAL || !e || !e->asset) {
        return true;
    }
    switch (e->asset->domain) {
        case BEHAVIOR_DOMAIN_LIBRARY:
        case BEHAVIOR_DOMAIN_MATERIAL:
        case BEHAVIOR_DOMAIN_SHAPE:
        case BEHAVIOR_DOMAIN_MODEL:
        case BEHAVIOR_DOMAIN_EVENT_DEFINITION:
        case BEHAVIOR_DOMAIN_SHADER_LIBRARY: return false;
        case BEHAVIOR_DOMAIN_EVENT:
        case BEHAVIOR_DOMAIN_FIELD: return true;
    }
    return false;
}

char *graph_collab_action_set_binding(
    const editor *e,
    const node *target,
    int input_index,
    const pin_binding *binding
)
{
    if (!e || !e->asset || !e->active_graph || !target ||
        graph_node_by_id(e->active_graph, target->id) != target ||
        input_index < 0 || input_index >= target->input_count ||
        target->input_pins[input_index].kind != PIN_DATA ||
        (target->input_pins[input_index].flags & PIN_FLAG_SETTING)) {
        return NULL;
    }
    if (!graph_binding_allowed(e, binding) ||
        (binding && binding->kind == PIN_BINDING_LOCAL &&
         !graph_local_by_id(e->active_graph, binding->local_id))) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "set_binding", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_uint(doc, root, "node", target->id);
    yyjson_mut_obj_add_uint(doc, root, "pin_id", target->input_pins[input_index].id);
    if (!write_pin_binding(doc, root, binding)) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return action_finish(doc);
}

static bool input_default_matches(const pin_spec *spec, const pin_value *value)
{
    if (!spec || !value || spec->kind != PIN_DATA ||
        (spec->flags & PIN_FLAG_SETTING) || spec->container != PIN_CONTAINER_SINGLE ||
        value->container != PIN_CONTAINER_SINGLE ||
        !action_value_matches(spec, value)) {
        return false;
    }
    return value->type != PIN_ENUM ||
           type_catalog_enum_label(spec->type_name, value->e);
}

char *graph_collab_action_set_input_default(
    const editor *e,
    const node *target,
    int input_index,
    const pin_value *value
)
{
    if (!e || !e->asset || !e->active_graph || !target || !value ||
        !behavior_asset_mutable(e->asset) || e->active_graph->flow_pause_count ||
        graph_node_by_id(e->active_graph, target->id) != target ||
        input_index < 0 || input_index >= target->input_count ||
        !input_default_matches(&target->input_pins[input_index], value)) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "set_binding", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_uint(doc, root, "node", target->id);
    yyjson_mut_obj_add_uint(doc, root, "pin_id", target->input_pins[input_index].id);
    yyjson_mut_obj_add_null(doc, root, "binding");
    if (!write_pin_value(doc, root, "default", value)) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    return action_finish(doc);
}

char *graph_collab_action_expand_inline(const editor *e, const node *target, int input_index)
{
    if (!e || !e->asset || !e->active_graph || !target ||
        graph_node_by_id(e->active_graph, target->id) != target ||
        input_index < 0 || input_index >= target->input_count ||
        target->input_pins[input_index].kind != PIN_DATA) {
        return NULL;
    }
    node *source = NULL;
    int source_pin = -1;
    if (!graph_input_source(e->active_graph, target, input_index, &source, &source_pin) ||
        !graph_node_is_inline(e->active_graph, source)) {
        return NULL;
    }
    yyjson_mut_doc *doc = action_start(e, "expand_inline", e->active_graph->graph_id);
    yyjson_mut_val *root = action_root(doc);
    yyjson_mut_obj_add_uint(doc, root, "node", target->id);
    yyjson_mut_obj_add_uint(doc, root, "pin_id", target->input_pins[input_index].id);
    return action_finish(doc);
}

static bool string_matches(yyjson_val *root, const char *key, const char *expected)
{
    yyjson_val *value = yyjson_obj_get(root, key);
    const char *actual = yyjson_get_str(value);
    return expected ? actual && !strcmp(actual, expected) : !value;
}

static const behavior_asset *find_asset(const editor *e, const char *name)
{
    if (!name || !name[0] || !strcmp(name, document_key(e)) ||
        !strcmp(name, e->asset->name)) {
        return e->asset;
    }
    const behavior_asset *found = NULL;
    for (int i = 0; i < acount(e->asset_catalog); i++) {
        const behavior_asset *asset = e->asset_catalog[i];
        if (!strcmp(asset->name, name)) {
            if (found) {
                return NULL;
            }
            found = asset;
        }
    }
    return found;
}

static bool match_is_action(const editor *e, const ed_search_match *match, yyjson_val *root)
{
    if (!match->desc || !string_matches(root, "descriptor", match->desc->name)) {
        return false;
    }
    yyjson_val *target_graph = yyjson_obj_get(root, "target_graph");
    const char *target_asset = yyjson_get_str(yyjson_obj_get(root, "target_asset"));
    yyjson_val *variable = yyjson_obj_get(root, "variable");
    const char *prototype = yyjson_get_str(yyjson_obj_get(root, "prototype"));
    const char *event = yyjson_get_str(yyjson_obj_get(root, "event"));
    const char *host_name = yyjson_get_str(yyjson_obj_get(root, "host_event"));
    const char *ui_name = yyjson_get_str(yyjson_obj_get(root, "ui_event"));
    if (target_graph) {
        const behavior_asset *asset = find_asset(e, target_asset);
        if (!asset || !match->function ||
            match->function->graph_id != yyjson_get_uint(target_graph) ||
            match->function->owner != asset) {
            return false;
        }
    } else if (match->function) {
        return false;
    }
    if (variable) {
        if (!match->variable || match->variable->id != yyjson_get_uint(variable)) {
            return false;
        }
    } else if (match->variable) {
        return false;
    }
    if (!string_matches(root, "prototype", match->prototype) ||
        !string_matches(root, "event", match->event ? match->event->name : NULL) ||
        !string_matches(root, "setting", match->setting) ||
        !string_matches(root, "field", match->field)) {
        return false;
    }
    if (host_name) {
        if (!match->host_event || match->host_event > e->event_catalog_count ||
            strcmp(host_name, e->event_catalog[match->host_event - 1].name)) {
            return false;
        }
    } else if (match->host_event) {
        return false;
    }
    if (ui_name) {
        if (!match->ui_event || strcmp(ui_name, match->ui_event->name)) {
            return false;
        }
    } else if (match->ui_event) {
        return false;
    }
    return true;
}

static bool replay_spawn_match(editor *e, yyjson_val *root, char *error, size_t error_size)
{
    const char *filter = "";
    ed_search_match *matches = ed_search_matches(e, filter);
    ed_search_match *match = NULL;
    for (int i = 0; i < acount(matches); i++) {
        if (match_is_action(e, &matches[i], root)) {
            match = &matches[i];
            break;
        }
    }
    if (!match) {
        afree(matches);
        snprintf(error, error_size, "Graph node descriptor or reference is unavailable");
        return false;
    }
    int pending_link = e->spawn_link_pending;
    ed_pin_view saved_link = e->spawn_link_pin;
    e->spawn_link_pending = 0;
    yyjson_val *link = yyjson_obj_get(root, "link");
    if (link) {
        yyjson_val *node_id = yyjson_obj_get(link, "node");
        yyjson_val *kind = yyjson_obj_get(link, "kind");
        yyjson_val *pin = yyjson_obj_get(link, "pin");
        node *source = yyjson_is_uint(node_id)
                         ? graph_node_by_id(e->active_graph, yyjson_get_uint(node_id))
                         : NULL;
        if (!source || !yyjson_is_int(kind) || !yyjson_is_int(pin)) {
            e->spawn_link_pending = pending_link;
            e->spawn_link_pin = saved_link;
            afree(matches);
            snprintf(error, error_size, "Graph spawn link endpoint is stale");
            return false;
        }
        int side = yyjson_get_int(kind), index = yyjson_get_int(pin);
        int count = side == ED_PIN_OUTPUT ? source->output_count : source->input_count;
        if ((side != ED_PIN_OUTPUT && side != ED_PIN_INPUT) || index < 0 ||
            index >= count) {
            e->spawn_link_pending = pending_link;
            e->spawn_link_pin = saved_link;
            afree(matches);
            snprintf(error, error_size, "Graph spawn link pin is invalid");
            return false;
        }
        e->spawn_link_pending = 1;
        e->spawn_link_pin = (ed_pin_view){ source, side, index };
    }
    float x = (float)yyjson_get_real(yyjson_obj_get(root, "x"));
    float y = (float)yyjson_get_real(yyjson_obj_get(root, "y"));
    yyjson_val *expected = yyjson_obj_get(root, "node");
    uint64_t created_id = 0;
    node *made = yyjson_is_uint(expected)
                   ? ed_spawn_match_at_with_id(e, match, x, y, &created_id)
                   : NULL;
    uint64_t expected_id = yyjson_get_uint(expected);
    e->spawn_link_pending = pending_link;
    e->spawn_link_pin = saved_link;
    afree(matches);
    if (!created_id || created_id != expected_id || (made && made->id != expected_id)) {
        snprintf(error, error_size, "Graph node creation diverged from its expected ID");
        return false;
    }
    return true;
}

static bool read_pin_binding(yyjson_val *value, pin_binding *out)
{
    yyjson_val *kind = yyjson_obj_get(value, "kind");
    if (!yyjson_is_obj(value) || !yyjson_is_int(kind)) {
        return false;
    }
    pin_binding binding = { .kind = yyjson_get_int(kind) };
    yyjson_val *field = NULL;
    switch (binding.kind) {
        case PIN_BINDING_VARIABLE: {
            yyjson_val *id = yyjson_obj_get(value, "variable");
            yyjson_val *asset = yyjson_obj_get(value, "source_asset");
            if (!yyjson_is_uint(id) || !yyjson_get_uint(id) ||
                (asset && !yyjson_is_str(asset))) {
                return false;
            }
            binding.variable_id = yyjson_get_uint(id);
            binding.source_asset = yyjson_get_str(asset);
            break;
        }
        case PIN_BINDING_COMPONENT_FIELD:
            binding.component_key = yyjson_get_str(yyjson_obj_get(value, "component"));
            binding.field_name = yyjson_get_str(yyjson_obj_get(value, "field"));
            if (!binding.component_key || !binding.component_key[0] ||
                !binding.field_name || !binding.field_name[0]) {
                return false;
            }
            break;
        case PIN_BINDING_COMPONENT:
            binding.component_key = yyjson_get_str(yyjson_obj_get(value, "component"));
            field = yyjson_obj_get(value, "field");
            if (!binding.component_key || !binding.component_key[0] || field) {
                return false;
            }
            break;
        case PIN_BINDING_LOCAL: {
            yyjson_val *id = yyjson_obj_get(value, "local");
            if (!yyjson_is_uint(id) || !yyjson_get_uint(id)) {
                return false;
            }
            binding.local_id = yyjson_get_uint(id);
            break;
        }
        case PIN_BINDING_EVENT_OUTPUT: {
            yyjson_val *node_id = yyjson_obj_get(value, "event_node");
            yyjson_val *pin_id = yyjson_obj_get(value, "event_pin");
            if (!yyjson_is_uint(node_id) || !yyjson_get_uint(node_id) ||
                !yyjson_is_uint(pin_id) || !yyjson_get_uint(pin_id)) {
                return false;
            }
            binding.event_node_id = yyjson_get_uint(node_id);
            binding.event_pin_id = yyjson_get_uint(pin_id);
            break;
        }
        default: return false;
    }
    *out = binding;
    return true;
}

static void forget_removed_node(editor *e, node *n, uint64_t id)
{
    ed_remove_selected(e, n);
    if (e->front_node == n) {
        e->front_node = NULL;
    }
    if (e->spawn_link_pin.n == n) {
        e->spawn_link_pin = (ed_pin_view){ 0 };
    }
    for (int i = acount(e->selected_links) - 1; i >= 0; i--) {
        ed_link_ref link = e->selected_links[i];
        if (link.from_id == id || link.to_id == id) {
            adel(e->selected_links, i);
        }
    }
}

static int remove_inline_source(editor *e, node *source)
{
    graph *gr = e->active_graph;
    uint64_t *ids = NULL;
    graph_inline_tree(gr, source, &ids);
    for (int i = 0; i < acount(ids); i++) {
        node *part = graph_node_by_id(gr, ids[i]);
        assert(part);
        forget_removed_node(e, part, ids[i]);
    }
    int result = graph_remove_node_tree(gr, source);
    afree(ids);
    return result;
}

static bool local_binding_valid(graph *gr, node *target, int pin, const pin_binding *binding)
{
    const graph_local *local = graph_local_by_id(gr, binding->local_id);
    node *source = local ? graph_node_by_id(gr, local->source_id) : NULL;
    int output = 0, source_pin = -1;
    const pin_spec *source_spec = source
                                    ? graph_pin_by_id(source, local->source_pin_id, &output, &source_pin)
                                    : NULL;
    const pin_spec *target_spec = &target->input_pins[pin];
    if (!source || source == target || !source_spec || !output ||
        source_spec->kind != PIN_DATA ||
        !ed_connection_specs_valid(source_spec, target_spec, PIN_DATA)) {
        return false;
    }
    node *old_source = NULL;
    int old_source_pin = -1;
    if (graph_input_source(gr, target, pin, &old_source, &old_source_pin) &&
        graph_node_is_inline(gr, old_source) && source != old_source) {
        uint64_t *inline_ids = NULL;
        graph_inline_tree(gr, old_source, &inline_ids);
        bool source_is_inline = false;
        for (int i = 0; i < acount(inline_ids); i++) {
            source_is_inline |= inline_ids[i] == source->id;
        }
        afree(inline_ids);
        if (source_is_inline) {
            return false;
        }
    }
    bool wildcard = (source_spec->flags | target_spec->flags) & PIN_FLAG_WILDCARD;
    if (!pin_compatible(source_spec, target_spec) && !wildcard) {
        return false;
    }
    if (target_spec->flags & PIN_FLAG_REFERENCE) {
        pin_binding reference = { 0 };
        return graph_resolve_reference(gr, source, source_pin, &reference) == 0;
    }
    return true;
}

static bool replace_input_binding(editor *e, node *target, int pin, const pin_binding *binding)
{
    graph *gr = e->active_graph;
    const pin_binding *old_binding = graph_pin_binding(gr, target, 0, pin);
    node *old_source = NULL;
    int old_source_pin = -1;
    bool has_source = graph_input_source(gr, target, pin, &old_source, &old_source_pin);
    bool old_inline = has_source && graph_node_is_inline(gr, old_source);

    if (binding && binding->kind == PIN_BINDING_LOCAL) {
        if (!local_binding_valid(gr, target, pin, binding)) {
            return false;
        }
        if (old_binding && graph_set_pin_binding(gr, target, 0, pin, NULL) != 0) {
            return false;
        }
        if (graph_use_local(gr, binding->local_id, target, pin) != 0) {
            return false;
        }
        const graph_local *local = graph_local_by_id(gr, binding->local_id);
        node *local_source = graph_node_by_id(gr, local->source_id);
        if (old_inline && old_source != local_source &&
            remove_inline_source(e, old_source) != 0) {
            return false;
        }
        graph_prune_locals(gr);
        return true;
    }

    if (has_source) {
        int removed = old_inline
                        ? remove_inline_source(e, old_source)
                        : graph_remove_edge(gr, old_source, old_source_pin, target, pin);
        if (removed != 0) {
            return false;
        }
    }
    if (graph_pin_binding(gr, target, 0, pin) &&
        graph_set_pin_binding(gr, target, 0, pin, NULL) != 0) {
        return false;
    }
    return !binding || graph_set_pin_binding(gr, target, 0, pin, binding) == 0;
}

static bool apply_input_binding(
    editor *e,
    node *target,
    int pin,
    const pin_binding *binding,
    const pin_value *default_value
)
{
    if (e->presentation) {
        const source_presentation_origin *origin = authoring_source_presentation_origin(
            e->presentation, e->active_graph->graph_id, target->id, target->input_pins[pin].id
        );
        if (!origin) {
            ed_show_error(e, "Input has no current source origin");
            return false;
        }
        if (binding) {
            ed_show_error(e, "Choose the source value through the checked source editor");
            return false;
        }
        authoring_edit_request request = {
            .kind = default_value ? AUTHORING_EDIT_VALUE : AUTHORING_EDIT_DISCONNECT,
            .generation = authoring_source_edit_generation(e->presentation),
            .origin = origin->identity,
            .value = default_value
        };
        return ed_source_edit(e, &request);
    }
    if (!behavior_asset_mutable(e->asset) ||
        e->active_graph->flow_pause_count || e->pending_snapshot ||
        (default_value &&
         (binding || !input_default_matches(&target->input_pins[pin], default_value)))) {
        return false;
    }
    behavior_asset *before = ed_snapshot(e);
    if (!before) {
        return false;
    }
    bool edit_pending = e->edit_pending;
    bool arrange_pending = e->arrange_pending;
    e->pending_snapshot = before;
    bool success = replace_input_binding(e, target, pin, binding);
    if (success && default_value) {
        success = graph_set_pin_default(e->active_graph, target, 0, pin, default_value) ==
                  0;
    }
    if (success) {
        ed_commit_snapshot(e);
        return true;
    }
    int restored = ed_adopt_behavior_asset(e, before);
    assert(restored == 0);
    (void)restored;
    ed_cancel_snapshot(e);
    e->edit_pending = edit_pending;
    e->arrange_pending = arrange_pending;
    return false;
}

static bool expand_inline_input(editor *e, node *target, int pin)
{
    graph *gr = e->active_graph;
    node *source = NULL;
    int source_pin = -1;
    if (!graph_input_source(gr, target, pin, &source, &source_pin) ||
        !graph_node_is_inline(gr, source)) {
        return false;
    }
    uint64_t *ids = NULL;
    graph_inline_tree(gr, source, &ids);
    for (int i = 0; i < acount(ids); i++) {
        if (!graph_node_by_id(gr, ids[i])) {
            afree(ids);
            return false;
        }
    }
    ed_snapshot_before(e);
    for (int i = 0; i < acount(ids); i++) {
        node *part = graph_node_by_id(gr, ids[i]);
        int result = graph_set_node_presentation(gr, part, NODE_PRESENTATION_EXPANDED);
        assert(result == 0);
        (void)result;
        part->placed = false;
    }
    afree(ids);
    e->arrange_pending = true;
    return true;
}

bool graph_collab_apply_action(editor *e, const char *json, char *error, size_t error_size)
{
    if (error && error_size) {
        error[0] = 0;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *op = yyjson_get_str(yyjson_obj_get(root, "op"));
    const char *key = yyjson_get_str(yyjson_obj_get(root, "document"));
    yyjson_val *graph_value = yyjson_obj_get(root, "graph");
    if (!e || !e->asset || !key || strcmp(key, document_key(e)) || !op ||
        !yyjson_is_uint(graph_value)) {
        yyjson_doc_free(doc);
        if (error && error_size) {
            snprintf(error, error_size, "Graph action does not match the active document");
        }
        return false;
    }
    uint64_t graph_id = yyjson_get_uint(graph_value);
    graph storage;
    graph_init(&storage);
    int result = -1;
    if (!strcmp(op, "open")) {
        result = editor_open_graph(e, graph_id);
    } else if (!strcmp(op, "create")) {
        yyjson_val *kind = yyjson_obj_get(root, "kind"), *id = graph_value;
        const char *name = yyjson_get_str(yyjson_obj_get(root, "name"));
        if (yyjson_is_int(kind) && yyjson_get_int(kind) >= GRAPH_EVENT &&
            yyjson_get_int(kind) <= GRAPH_MACRO && name) {
            result = editor_command_apply(
                e,
                (editor_command){
                    .kind = EDITOR_COMMAND_CREATE_GRAPH,
                    .data.create_graph = { (graph_kind)yyjson_get_int(kind), name } }
            );
            graph *created = behavior_asset_find_graph(e->asset, yyjson_get_uint(id));
            if (result == 0 && (!created || strcmp(created->name, name) ||
                                created->kind != (graph_kind)yyjson_get_int(kind))) {
                result = -1;
            }
        }
    } else if (!strcmp(op, "rename")) {
        const char *name = yyjson_get_str(yyjson_obj_get(root, "name"));
        if (name) {
            result = editor_command_apply(
                e,
                (editor_command){ .kind = EDITOR_COMMAND_RENAME_GRAPH,
                                  .data.rename_graph = { graph_id, name } }
            );
        }
    } else if (!strcmp(op, "spawn")) {
        const char *descriptor = yyjson_get_str(yyjson_obj_get(root, "descriptor"));
        yyjson_val *id = yyjson_obj_get(root, "node");
        const node_desc *desc = descriptor ? registry_node_by_name(descriptor) : NULL;
        float x = (float)yyjson_get_real(yyjson_obj_get(root, "x"));
        float y = (float)yyjson_get_real(yyjson_obj_get(root, "y"));
        node *made = desc && yyjson_is_uint(id) && e->active_graph->graph_id == graph_id
                       ? editor_spawn(e, desc, x, y)
                       : NULL;
        result = made && made->id == yyjson_get_uint(id) ? 0 : -1;
    } else if (!strcmp(op, "spawn_match")) {
        if (e->active_graph->graph_id == graph_id) {
            result = replay_spawn_match(e, root, error, error_size) ? 0 : -1;
        }
    } else if (!strcmp(op, "set_default")) {
        yyjson_val *node_id = yyjson_obj_get(root, "node"),
                   *output = yyjson_obj_get(root, "output"),
                   *pin = yyjson_obj_get(root, "pin");
        pin_value value = { 0 };
        if (e->active_graph->graph_id == graph_id && yyjson_is_uint(node_id) &&
            yyjson_is_int(output) && yyjson_is_int(pin) &&
            read_pin_value(yyjson_obj_get(root, "value"), &value, &storage)) {
            node *target = graph_node_by_id(e->active_graph, yyjson_get_uint(node_id));
            int side = yyjson_get_int(output), index = yyjson_get_int(pin);
            const pin_spec *spec = target && (side == 0 || side == 1) && index >= 0 &&
                                           index < (side ? target->output_count
                                                         : target->input_count)
                                     ? &(side ? target->output_pins
                                              : target->input_pins)[index]
                                     : NULL;
            bool same_shape = action_value_matches(spec, &value);
            if (!same_shape) {
                graph_free(&storage);
                yyjson_doc_free(doc);
                if (error && error_size) {
                    snprintf(error, error_size, "Graph default does not match the target pin shape");
                }
                return false;
            }
            result = editor_command_apply(
                e,
                (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                                  .data.set_pin_default = { yyjson_get_uint(node_id),
                                                            yyjson_get_int(output),
                                                            yyjson_get_int(pin),
                                                            &value,
                                                            false } }
            );
        }
    } else if (!strcmp(op, "reset_pin")) {
        yyjson_val *node_id = yyjson_obj_get(root, "node"),
                   *pin = yyjson_obj_get(root, "pin");
        pin_value value = { 0 };
        node *n = yyjson_is_uint(node_id)
                    ? graph_node_by_id(e->active_graph, yyjson_get_uint(node_id))
                    : NULL;
        if (e->active_graph->graph_id == graph_id && n && yyjson_is_int(pin) &&
            read_pin_value(yyjson_obj_get(root, "value"), &value, &storage) &&
            yyjson_get_int(pin) >= 0 && yyjson_get_int(pin) < n->input_count &&
            action_value_matches(&n->input_pins[yyjson_get_int(pin)], &value)) {
            if (e->presentation) {
                result = editor_command_apply(e, (editor_command){
                    .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                    .data.set_pin_default = { n->id, 0, yyjson_get_int(pin), &value, false }
                });
            } else {
                ed_snapshot_before(e);
                result = graph_set_pin_binding(e->active_graph, n, 0, yyjson_get_int(pin), NULL);
                if (result == 0) {
                    result = graph_set_pin_default(e->active_graph, n, 0, yyjson_get_int(pin), &value);
                }
            }
        }
    } else if (!strcmp(op, "set_binding")) {
        yyjson_val *node_id = yyjson_obj_get(root, "node"),
                   *pin_id = yyjson_obj_get(root, "pin_id"),
                   *value = yyjson_obj_get(root, "binding"),
                   *default_json = yyjson_obj_get(root, "default");
        node *n = yyjson_is_uint(node_id)
                    ? graph_node_by_id(e->active_graph, yyjson_get_uint(node_id))
                    : NULL;
        int output = 0, pin = -1;
        const pin_spec *spec = n && yyjson_is_uint(pin_id)
                                 ? graph_pin_by_id(n, yyjson_get_uint(pin_id), &output, &pin)
                                 : NULL;
        pin_binding binding = { 0 };
        pin_value default_value = { 0 };
        bool clear = yyjson_is_null(value);
        bool has_default = default_json != NULL;
        bool valid_binding = clear || read_pin_binding(value, &binding);
        bool binding_allowed = clear || graph_binding_allowed(e, &binding);
        bool valid_default = !has_default ||
                             read_pin_value(default_json, &default_value, &storage);
        bool default_allowed = !has_default ||
                               (clear && input_default_matches(spec, &default_value));
        if (e->active_graph->graph_id == graph_id && spec && !output &&
            spec->kind == PIN_DATA && !(spec->flags & PIN_FLAG_SETTING) &&
            valid_binding && binding_allowed && valid_default && default_allowed &&
            (clear || binding.kind != PIN_BINDING_LOCAL ||
             local_binding_valid(e->active_graph, n, pin, &binding))) {
            result = apply_input_binding(e, n, pin, clear ? NULL : &binding, has_default ? &default_value : NULL)
                       ? 0
                       : -1;
        }
    } else if (!strcmp(op, "expand_inline")) {
        yyjson_val *node_id = yyjson_obj_get(root, "node"),
                   *pin_id = yyjson_obj_get(root, "pin_id");
        node *n = yyjson_is_uint(node_id)
                    ? graph_node_by_id(e->active_graph, yyjson_get_uint(node_id))
                    : NULL;
        int output = 0, pin = -1;
        const pin_spec *spec = n && yyjson_is_uint(pin_id)
                                 ? graph_pin_by_id(n, yyjson_get_uint(pin_id), &output, &pin)
                                 : NULL;
        if (e->active_graph->graph_id == graph_id && spec && !output &&
            spec->kind == PIN_DATA) {
            result = expand_inline_input(e, n, pin) ? 0 : -1;
        }
    } else if (!strcmp(op, "reconnect")) {
        yyjson_val *from_id = yyjson_obj_get(root, "from_node"),
                   *from_pin = yyjson_obj_get(root, "from_pin"),
                   *old_id = yyjson_obj_get(root, "old_node"),
                   *old_pin = yyjson_obj_get(root, "old_pin"),
                   *to_id = yyjson_obj_get(root, "to_node"),
                   *to_pin = yyjson_obj_get(root, "to_pin");
        if (e->active_graph->graph_id == graph_id && yyjson_is_uint(from_id) &&
            yyjson_is_int(from_pin) && yyjson_is_uint(old_id) &&
            yyjson_is_int(old_pin) &&
            ((!to_id && !to_pin) || (yyjson_is_uint(to_id) && yyjson_is_int(to_pin)))) {
            node *from = graph_node_by_id(e->active_graph, yyjson_get_uint(from_id));
            node *old_to = graph_node_by_id(e->active_graph, yyjson_get_uint(old_id));
            node *new_to = to_id
                             ? graph_node_by_id(e->active_graph, yyjson_get_uint(to_id))
                             : NULL;
            if (!to_id || new_to) {
                result = graph_collab_reconnect_apply(
                             e,
                             from,
                             yyjson_get_int(from_pin),
                             old_to,
                             yyjson_get_int(old_pin),
                             new_to,
                             yyjson_get_int(to_pin)
                         )
                           ? 0
                           : -1;
            }
        }
    } else if (!strcmp(op, "connect")) {
        yyjson_val *kind = yyjson_obj_get(root, "kind"),
                   *from_node = yyjson_obj_get(root, "from_node"),
                   *from_pin = yyjson_obj_get(root, "from_pin"),
                   *to_node = yyjson_obj_get(root, "to_node"),
                   *to_pin = yyjson_obj_get(root, "to_pin");
        if (e->active_graph->graph_id == graph_id && yyjson_is_int(kind) &&
            yyjson_is_uint(from_node) && yyjson_is_int(from_pin) &&
            yyjson_is_uint(to_node) && yyjson_is_int(to_pin) &&
            (yyjson_get_int(kind) == PIN_DATA || yyjson_get_int(kind) == PIN_EXEC)) {
            result = editor_command_apply(
                e,
                (editor_command){ .kind = yyjson_get_int(kind) == PIN_DATA
                                            ? EDITOR_COMMAND_CONNECT_DATA
                                            : EDITOR_COMMAND_CONNECT_EXEC,
                                  .data.connect = { yyjson_get_uint(from_node),
                                                    yyjson_get_int(from_pin),
                                                    yyjson_get_uint(to_node),
                                                    yyjson_get_int(to_pin) } }
            );
        }
    }
    graph_free(&storage);
    yyjson_doc_free(doc);
    if (result != 0) {
        if (error && error_size && !error[0]) {
            snprintf(error, error_size, "Graph action was refused by the editor");
        }
        return false;
    }
    return true;
}
