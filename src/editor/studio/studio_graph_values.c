#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "graph/authoring/local_edit.h"
/* Typed value and source editors shared by graph nodes and Properties. */
#include "ecs/ecs.h"
#include "editor/collab/actions_internal.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/studio/studio_members_internal.h"
#include "editor/ui/choices.h"
#include "editor/ui/labels.h"
#include "graph/model/local.h"
#include "graph/model/pin_conv.h"
#include "graph/model/registry.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/nodes/conv_nodes.h"
#include "runtime/declared_components.h"
#include "support/array.h"
#include "support/number_expr.h"
#include "types/value_format.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum
{
    SOURCE_BINDING,
    SOURCE_OUTPUT,
    SOURCE_TYPED_LITERAL,
    SOURCE_EDIT_EXPRESSION,
    SOURCE_EDIT_INLINE_INPUTS,
    SOURCE_EXPAND_INLINE,
    SOURCE_TYPED_EXPRESSION, /* Properties: a folded expression's new source */
} source_kind;

static void inline_popup_key(char *out, size_t size, const node *target, uint64_t input_pin);

typedef struct studio_graph_source
{
    source_kind kind;
    pin_binding binding; /* IDs and kind only; strings live in the arrays below. */
    uint64_t node_id;
    int pin;
    pin_value literal;
    bool typed_literal;
    uint32_t icon;
    int boost;
    bool current;
    char component_key[128];
    char field_name[256];
    char source_asset[128];
    char label[192];
    char group[64];
    char keywords[192];
    char meta[80];
    char description[80];
    char literal_text[256];
} source_choice;

static bool compatible(const pin_spec *source, const pin_spec *target)
{
    return pin_compatible(source, target) || conversion_for_pins(source, target);
}

static bool binding_equal(const pin_binding *a, const pin_binding *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case PIN_BINDING_LOCAL: return a->local_id == b->local_id;
        case PIN_BINDING_VARIABLE:
            return a->variable_id == b->variable_id &&
                   ((!a->source_asset && !b->source_asset) ||
                    (a->source_asset && b->source_asset &&
                     !strcmp(a->source_asset, b->source_asset)));
        case PIN_BINDING_COMPONENT:
        case PIN_BINDING_COMPONENT_FIELD:
            return a->component_key && b->component_key &&
                   !strcmp(a->component_key, b->component_key) &&
                   ((!a->field_name && !b->field_name) ||
                    (a->field_name && b->field_name &&
                     !strcmp(a->field_name, b->field_name)));
        case PIN_BINDING_EVENT_OUTPUT:
            return a->event_node_id == b->event_node_id &&
                   a->event_pin_id == b->event_pin_id;
    }
    return false;
}

static void add_choice(
    source_choice **choices,
    source_kind kind,
    const char *label,
    const char *group,
    const char *keywords,
    const char *meta,
    const char *description,
    uint32_t icon,
    int boost,
    bool current,
    const pin_binding *binding,
    uint64_t node_id,
    int pin
)
{
    source_choice choice = { .kind = kind,
                             .node_id = node_id,
                             .pin = pin,
                             .icon = icon,
                             .boost = boost,
                             .current = current };
    snprintf(choice.label, sizeof choice.label, "%s", label);
    snprintf(choice.group, sizeof choice.group, "%s", group);
    snprintf(choice.keywords, sizeof choice.keywords, "%s", keywords ? keywords : "");
    snprintf(choice.meta, sizeof choice.meta, "%s", meta ? meta : "");
    snprintf(choice.description, sizeof choice.description, "%s", description ? description : "");
    if (binding) {
        choice.binding = *binding;
        choice.binding.component_key = NULL;
        choice.binding.field_name = NULL;
        choice.binding.source_asset = NULL;
        snprintf(
            choice.component_key,
            sizeof choice.component_key,
            "%s",
            binding->component_key ? binding->component_key : ""
        );
        snprintf(
            choice.field_name,
            sizeof choice.field_name,
            "%s",
            binding->field_name ? binding->field_name : ""
        );
        snprintf(
            choice.source_asset,
            sizeof choice.source_asset,
            "%s",
            binding->source_asset ? binding->source_asset : ""
        );
    }
    apush(*choices, choice);
}

static pin_binding choice_binding(const source_choice *choice)
{
    pin_binding binding = choice->binding;
    binding.component_key = choice->component_key[0] ? choice->component_key : NULL;
    binding.field_name = choice->field_name[0] ? choice->field_name : NULL;
    binding.source_asset = choice->source_asset[0] ? choice->source_asset : NULL;
    return binding;
}

static pin_value choice_literal(const source_choice *choice)
{
    pin_value value = choice->literal;
    if (value.type == PIN_STR || value.type == PIN_NAME) {
        value.str = choice->literal_text;
    }
    return value;
}

static void add_literal_choice(
    source_choice **choices,
    const char *label,
    const char *group,
    const char *keywords,
    const char *meta,
    uint32_t icon,
    int boost,
    bool current,
    const pin_value *value
)
{
    add_choice(
        choices,
        SOURCE_TYPED_LITERAL,
        label,
        group,
        keywords,
        meta,
        "Use this value directly instead of a source",
        icon,
        boost,
        current,
        NULL,
        0,
        -1
    );
    source_choice *choice = &(*choices)[acount(*choices) - 1];
    choice->typed_literal = true;
    choice->literal = *value;
    if (value->type == PIN_STR || value->type == PIN_NAME) {
        snprintf(
            choice->literal_text,
            sizeof choice->literal_text,
            "%s",
            value->str ? value->str : ""
        );
        choice->literal.str = NULL;
    }
}

static double numeric_value(const pin_value *value)
{
    switch (value->type) {
        case PIN_I8: return value->i8;
        case PIN_U8: return value->u8;
        case PIN_I16: return value->i16;
        case PIN_U16: return value->u16;
        case PIN_I32: return value->i;
        case PIN_U32: return value->u;
        case PIN_I64: return (double)value->i64;
        case PIN_U64: return (double)value->u64;
        case PIN_F32: return value->f;
        case PIN_F64: return value->d;
        default: return 0;
    }
}

static bool trailing_space(const char **text)
{
    while (isspace((unsigned char)**text)) {
        (*text)++;
    }
    return !**text;
}

static bool only_space(const char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    return !*text;
}

static bool parse_integer_text(const char *text, bool is_unsigned, intmax_t *signed_value, uintmax_t *unsigned_value)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (is_unsigned) {
        if (*text == '-') {
            return false;
        }
        errno = 0;
        char *end = NULL;
        uintmax_t value = strtoumax(text, &end, 10);
        if (end == text || errno == ERANGE || !only_space(end)) {
            return false;
        }
        *unsigned_value = value;
        return true;
    }
    errno = 0;
    char *end = NULL;
    intmax_t value = strtoimax(text, &end, 10);
    if (end == text || errno == ERANGE || !only_space(end)) {
        return false;
    }
    *signed_value = value;
    return true;
}

static bool quoted_text(const char *text, char *out, size_t size)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (*text++ != '"' || size < 1) {
        return false;
    }
    size_t used = 0;
    bool closed = false;
    while (*text) {
        char c = *text++;
        if (c == '"') {
            closed = true;
            break;
        }
        if (c == '\\') {
            c = *text++;
            switch (c) {
                case '"':
                case '\\':
                case '/': break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return false;
            }
        }
        if (!c || used + 1 >= size) {
            return false;
        }
        out[used++] = c;
    }
    out[used] = '\0';
    return closed && trailing_space(&text);
}

static bool parse_input_literal(const pin_spec *spec, const char *text, pin_value *out)
{
    if (spec->container != PIN_CONTAINER_SINGLE) {
        return false;
    }
    pin_value value = { .type = spec->type,
                        .type_name = spec->type_name,
                        .container = spec->container };
    if (spec->type >= PIN_I8 && spec->type <= PIN_U64) {
        bool is_unsigned = spec->type == PIN_U8 || spec->type == PIN_U16 ||
                           spec->type == PIN_U32 || spec->type == PIN_U64;
        intmax_t signed_value = 0;
        uintmax_t unsigned_value = 0;
        bool exact = parse_integer_text(text, is_unsigned, &signed_value, &unsigned_value);
        double number = 0;
        if (!exact &&
            (!number_expr_eval(text, numeric_value(&spec->default_value), &number) ||
             trunc(number) != number)) {
            return false;
        }
        if (!exact && (spec->type == PIN_I64 || spec->type == PIN_U64) &&
            fabs(number) >= 0x1p53) {
            return false;
        }
        if (!exact) {
            if (is_unsigned) {
                if (number < 0 || number >= 0x1p64) {
                    return false;
                }
                unsigned_value = (uintmax_t)number;
            } else {
                if (number < -0x1p63 || number >= 0x1p63) {
                    return false;
                }
                signed_value = (intmax_t)number;
            }
        }
        switch (spec->type) {
            case PIN_I8:
                if (signed_value < INT8_MIN || signed_value > INT8_MAX) {
                    return false;
                }
                value.i8 = (int8_t)signed_value;
                break;
            case PIN_U8:
                if (unsigned_value > UINT8_MAX) {
                    return false;
                }
                value.u8 = (uint8_t)unsigned_value;
                break;
            case PIN_I16:
                if (signed_value < INT16_MIN || signed_value > INT16_MAX) {
                    return false;
                }
                value.i16 = (int16_t)signed_value;
                break;
            case PIN_U16:
                if (unsigned_value > UINT16_MAX) {
                    return false;
                }
                value.u16 = (uint16_t)unsigned_value;
                break;
            case PIN_I32:
                if (signed_value < INT32_MIN || signed_value > INT32_MAX) {
                    return false;
                }
                value.i = (int32_t)signed_value;
                break;
            case PIN_U32:
                if (unsigned_value > UINT32_MAX) {
                    return false;
                }
                value.u = (uint32_t)unsigned_value;
                break;
            case PIN_I64:
                if (signed_value < INT64_MIN || signed_value > INT64_MAX) {
                    return false;
                }
                value.i64 = (int64_t)signed_value;
                break;
            case PIN_U64:
                if (unsigned_value > UINT64_MAX) {
                    return false;
                }
                value.u64 = (uint64_t)unsigned_value;
                break;
            default: assert(0); return false;
        }
        *out = value;
        return true;
    }
    if (spec->type == PIN_F32 || spec->type == PIN_F64) {
        double number;
        if (!number_expr_eval(text, numeric_value(&spec->default_value), &number)) {
            return false;
        }
        if (spec->type == PIN_F32) {
            float rounded = (float)number;
            if (!isfinite(rounded)) {
                return false;
            }
            value.f = rounded;
        } else {
            value.d = number;
        }
        *out = value;
        return true;
    }
    if (spec->type == PIN_BOOL) {
        while (isspace((unsigned char)*text)) {
            text++;
        }
        if (!strcasecmp(text, "true")) {
            value.b = true;
        } else if (!strcasecmp(text, "false")) {
            value.b = false;
        } else {
            return false;
        }
        *out = value;
        return true;
    }
    if (spec->type == PIN_STR || spec->type == PIN_NAME) {
        static char decoded[256];
        if (!quoted_text(text, decoded, sizeof decoded)) {
            return false;
        }
        value.str = decoded;
        *out = value;
        return true;
    }
    if (spec->type == PIN_HANDLE) {
        while (isspace((unsigned char)*text)) {
            text++;
        }
        if (strcasecmp(text, "none")) {
            return false;
        }
        value.u64 = 0;
        *out = value;
        return true;
    }
    return false;
}

static void component_label(const ecs_component_desc *component, char *out, size_t size)
{
    editor_component_label(component->name, out, size);
}

static bool component_query_separator(
    const ecs_component_desc *component,
    const char *query,
    const char **separator_out
)
{
    const char *separator = query ? strchr(query, '.') : NULL;
    if (!separator || separator == query) {
        return false;
    }
    char scope[96], label[96];
    size_t scope_length = (size_t)(separator - query);
    if (scope_length >= sizeof scope) {
        return false;
    }
    snprintf(scope, sizeof scope, "%.*s", (int)scope_length, query);
    component_label(component, label, sizeof label);
    if (strcasecmp(scope, label)) {
        return false;
    }

    int visible_matches = 0;
    ecs_component_catalog project = project_components();
    for (int i = 0; i < project.component_count; i++) {
        const ecs_component_desc *candidate = project.components[i];
        if (!candidate || !candidate->name) {
            continue;
        }
        char candidate_label[96];
        component_label(candidate, candidate_label, sizeof candidate_label);
        visible_matches += !strcasecmp(scope, candidate_label);
    }
    if (visible_matches != 1) {
        return false;
    }

    *separator_out = separator;
    return true;
}

static bool field_path_prefix(const char *path, const char *prefix, size_t length)
{
    return strlen(path) > length && !strncasecmp(path, prefix, length) &&
           path[length] == '.';
}

static const char *component_scope_path(
    const ecs_component_desc *component,
    const char *path,
    const char *query
)
{
    const char *separator = NULL;
    if (!component_query_separator(component, query, &separator)) {
        return NULL;
    }
    const char *last_dot = strrchr(query, '.');
    size_t field_scope_length = last_dot == separator
                                  ? 0
                                  : (size_t)(last_dot - separator - 1);
    const char *field_prefix = last_dot + 1;
    const char *remaining = path;
    if (field_scope_length) {
        if (!field_path_prefix(path, separator + 1, field_scope_length)) {
            return NULL;
        }
        remaining = path + field_scope_length + 1;
    } else if (last_dot != separator) {
        return NULL;
    }
    return strncasecmp(remaining, field_prefix, strlen(field_prefix)) ? NULL : remaining;
}

static void component_field_label(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    size_t used = 0;
    for (const char *at = path; at && *at && used < size;) {
        const char *end = strchr(at, '.');
        int length = end ? (int)(end - at) : (int)strlen(at);
        char segment[96], label[96];
        snprintf(segment, sizeof segment, "%.*s", length, at);
        editor_identifier_label(segment, label, sizeof label);
        used += (size_t)
            snprintf(out + used, size - used, "%s%s", used ? " \xE2\x80\xBA " : "", label);
        at = end ? end + 1 : NULL;
    }
}

static bool component_path_label(
    const ecs_component_desc *component,
    const char *path,
    const char *query,
    char *out,
    size_t size
)
{
    const char *scoped_path = component_scope_path(component, path, query);
    if (scoped_path) {
        component_field_label(scoped_path, out, size);
        return true;
    }
    char component_name[96], field_path[192];
    component_label(component, component_name, sizeof component_name);
    component_field_label(path, field_path, sizeof field_path);
    snprintf(out, size, "%s \xE2\x80\xBA %s", component_name, field_path);
    return false;
}

static void add_component_fields(
    source_choice **choices,
    const ecs_component_desc *component,
    const type_desc *type,
    const pin_spec *target,
    const char *query,
    const char *prefix,
    int depth,
    const pin_binding *current
)
{
    if (!type || depth >= 6) {
        return;
    }
    char group[96];
    component_label(component, group, sizeof group);
    for (int i = 0; i < type->field_count; i++) {
        const type_field *field = &type->fields[i];
        char path[256], label[192], keywords[192];
        int written = snprintf(
            path,
            sizeof path,
            "%s%s%s",
            prefix,
            prefix[0] ? "." : "",
            field->name
        );
        if (written < 0 || (size_t)written >= sizeof path) {
            continue;
        }
        pin_spec source_spec = { .type = field->type,
                                 .type_name = field->type_name,
                                 .kind = PIN_DATA,
                                 .container = PIN_CONTAINER_SINGLE };
        if (compatible(&source_spec, target)) {
            pin_binding binding = { .kind = PIN_BINDING_COMPONENT_FIELD,
                                    .component_key = component->key,
                                    .field_name = path };
            bool scoped = component_path_label(component, path, query, label, sizeof label);
            snprintf(
                keywords,
                sizeof keywords,
                "%s %s %s %s.%s",
                component->key,
                component->name,
                path,
                group,
                path
            );
            bool same_component = current && current->component_key &&
                                  !strcmp(current->component_key, component->key);
            add_choice(
                choices,
                SOURCE_BINDING,
                label,
                scoped ? "" : (same_component ? group : "Components"),
                keywords,
                ed_pin_type_name(&source_spec),
                NULL,
                KIT_ICON_FIELD,
                same_component ? 80 : 0,
                current && binding_equal(&binding, current),
                &binding,
                0,
                -1
            );
        }
        if (field->type != PIN_STRUCT || !field->type_name) {
            continue;
        }
        const type_desc *nested = type_catalog_find(field->type_name);
        add_component_fields(choices, component, nested, target, query, path, depth + 1, current);
    }
}

static bool id_in(const uint64_t *ids, uint64_t id)
{
    for (int i = 0; i < acount(ids); i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

static node *input_source(graph *gr, node *target, int pin, int *source_pin)
{
    node *source = NULL;
    *source_pin = -1;
    return graph_input_source(gr, target, pin, &source, source_pin) ? source : NULL;
}

static const pin_binding *current_binding(
    graph *gr,
    node *target,
    int pin,
    node *source,
    int source_pin,
    pin_binding *resolved
)
{
    const pin_binding *binding = graph_pin_binding(gr, target, 0, pin);
    if (binding && binding->kind == PIN_BINDING_LOCAL) {
        return binding;
    }
    if (source && source_pin >= 0) {
        return graph_resolve_reference(gr, source, source_pin, resolved) == 0
                 ? resolved
                 : NULL;
    }
    return binding;
}

static void add_sources(
    studio *s,
    node *target,
    int pin,
    const char *query,
    source_choice **choices,
    node *source,
    int source_pin,
    const pin_binding *current
)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    const pin_spec *target_spec = &target->input_pins[pin];
    uint64_t *inline_ids = NULL;
    if (source && graph_node_is_inline(gr, source)) {
        graph_inline_tree(gr, source, &inline_ids);
    }

    if (ed_source_scope_available(e) && e->asset->domain == BEHAVIOR_DOMAIN_EVENT) {
        for (int i = 0; i < acount(e->asset->variables); i++) {
            const behavior_variable *variable = &e->asset->variables[i];
            pin_spec source_spec = { .type = variable->type,
                                     .type_name = variable->type_name,
                                     .kind = PIN_DATA,
                                     .container = variable->container };
            if (!compatible(&source_spec, target_spec)) {
                continue;
            }
            char label[192], keywords[192];
            snprintf(label, sizeof label, "Variable \xE2\x80\xBA %s", variable->name);
            snprintf(
                keywords,
                sizeof keywords,
                "%s %s",
                variable->name,
                variable->category ? variable->category : ""
            );
            pin_binding binding = { .kind = PIN_BINDING_VARIABLE,
                                    .variable_id = variable->id,
                                    .source_asset = e->asset->name };
            add_choice(
                choices,
                SOURCE_BINDING,
                label,
                variable->category && variable->category[0] ? variable->category
                                                            : "Variables",
                keywords,
                ed_pin_type_name(&source_spec),
                NULL,
                KIT_ICON_VARIABLE,
                0,
                current && binding_equal(&binding, current),
                &binding,
                0,
                -1
            );
        }

        if (target_spec->container == PIN_CONTAINER_SINGLE) {
            ecs_component_catalog project = project_components();
            for (int i = 0; i < project.component_count; i++) {
                const ecs_component_desc *component = project.components[i];
                if (!component || !component->type || !component->key) {
                    continue;
                }
                bool same_component = current && current->component_key &&
                                      !strcmp(current->component_key, component->key);
                add_component_fields(choices, component, component->type, target_spec, query, "", 0, current);
                pin_spec component_spec = { .type = PIN_STRUCT,
                                            .type_name = component->type->name,
                                            .kind = PIN_DATA,
                                            .container = PIN_CONTAINER_SINGLE };
                if (compatible(&component_spec, target_spec)) {
                    char label[96];
                    component_label(component, label, sizeof label);
                    pin_binding binding = { .kind = PIN_BINDING_COMPONENT,
                                            .component_key = component->key };
                    add_choice(
                        choices,
                        SOURCE_BINDING,
                        label,
                        same_component ? label : "Components",
                        component->key,
                        ed_pin_type_name(&component_spec),
                        NULL,
                        KIT_ICON_COMPONENT,
                        same_component ? 80 : 0,
                        current && binding_equal(&binding, current),
                        &binding,
                        0,
                        -1
                    );
                }
            }
        }
    }

    for (int i = 0; i < acount(gr->locals); i++) {
        const graph_local *local = &gr->locals[i];
        node *local_source = graph_node_by_id(gr, local->source_id);
        if (local_source && id_in(inline_ids, local_source->id) &&
            !(current && current->kind == PIN_BINDING_LOCAL &&
              current->local_id == local->id)) {
            continue;
        }
        int out = -1, is_output = 0;
        const pin_spec *spec = local_source
                                 ? graph_pin_by_id(local_source, local->source_pin_id, &is_output, &out)
                                 : NULL;
        if (!spec || !is_output || !pin_compatible(spec, target_spec)) {
            continue;
        }
        pin_binding binding = { .kind = PIN_BINDING_LOCAL, .local_id = local->id };
        add_choice(
            choices,
            SOURCE_BINDING,
            local->name,
            "Locals",
            local->name,
            ed_pin_type_name(spec),
            NULL,
            KIT_ICON_LOCAL,
            0,
            current && binding_equal(&binding, current),
            &binding,
            0,
            -1
        );
    }

    for (int i = 0; i < acount(gr->nodes); i++) {
        node *candidate = gr->nodes[i];
        if (candidate == target || id_in(inline_ids, candidate->id)) {
            continue;
        }
        uint32_t icon = sg_node_icon(s, candidate);
        if (!icon) {
            icon = candidate->desc->display_class == NODE_DISPLAY_LITERAL
                     ? KIT_ICON_CONSTANT
                     : KIT_ICON_FUNCTION;
        }
        for (int out = 0; out < candidate->output_count; out++) {
            const pin_spec *spec = &candidate->output_pins[out];
            if (spec->kind != PIN_DATA || spec->container != PIN_CONTAINER_SINGLE ||
                target_spec->container != PIN_CONTAINER_SINGLE ||
                !ed_connection_specs_valid(spec, target_spec, PIN_DATA)) {
                continue;
            }
            char title[128], pin_label[96], label[256];
            ed_node_title(e, candidate, title, sizeof title);
            studio_code_pin_label(s, candidate, spec, pin_label, sizeof pin_label);
            snprintf(label, sizeof label, "%s \xE2\x80\xBA %s", title, pin_label);
            add_choice(
                choices,
                SOURCE_OUTPUT,
                label,
                candidate->desc->category ? candidate->desc->category : "Graph outputs",
                candidate->desc->name,
                ed_pin_type_name(spec),
                NULL,
                icon,
                0,
                source == candidate && source_pin == out,
                NULL,
                candidate->id,
                out
            );
        }
    }

    if (source && graph_node_is_inline(gr, source)) {
        bool expression = false;
        for (int i = 0; i < acount(inline_ids); i++) {
            node *part = graph_node_by_id(gr, inline_ids[i]);
            expression |= part && part->desc == &NODE_BehaviorExpression;
        }
        if (expression) {
            add_choice(
                choices,
                SOURCE_EDIT_EXPRESSION,
                "Edit Expression",
                "Inline source",
                "math formula expression code",
                NULL,
                "Edit this folded expression in place",
                KIT_ICON_EXPRESSION,
                25,
                false,
                NULL,
                source->id,
                source_pin
            );
        }
        add_choice(
            choices,
            SOURCE_EDIT_INLINE_INPUTS,
            "Edit Inline Inputs",
            "Inline source",
            "edit fields values inline",
            NULL,
            "Edit this folded source without expanding its nodes",
            KIT_ICON_PROPERTIES,
            30,
            false,
            NULL,
            source->id,
            source_pin
        );
        add_choice(
            choices,
            SOURCE_EXPAND_INLINE,
            "Expand Inline Inputs",
            "Inline source",
            "show reveal fields node inputs",
            NULL,
            "Show the source nodes and their editable inputs",
            KIT_ICON_MAXIMIZE,
            10,
            false,
            NULL,
            source->id,
            source_pin
        );
    }
    afree(inline_ids);
}

static pin_value typed_editor_choice(const pin_spec *spec, const editor_choice *choice)
{
    pin_value value = { .type = spec->type,
                        .type_name = spec->type_name,
                        .container = spec->container };
    if (spec->type == PIN_ENUM) {
        value.e = choice->value;
    } else if (spec->type == PIN_NAME) {
        value.str = choice->name ? choice->name : choice->label;
    } else {
        switch (spec->type) {
            case PIN_I8: value.i8 = (int8_t)choice->value; break;
            case PIN_U8: value.u8 = (uint8_t)choice->value; break;
            case PIN_I16: value.i16 = (int16_t)choice->value; break;
            case PIN_U16: value.u16 = (uint16_t)choice->value; break;
            case PIN_I32: value.i = choice->value; break;
            case PIN_U32: value.u = (uint32_t)choice->value; break;
            case PIN_I64: value.i64 = choice->value; break;
            case PIN_U64: value.u64 = (uint32_t)choice->value; break;
            default: assert(0); break;
        }
    }
    return value;
}

static void add_typed_choices(
    editor *e,
    node *target,
    int pin,
    const char *query,
    bool has_source,
    source_choice **choices
)
{
    const pin_spec *spec = &target->input_pins[pin];
    const editor_choices *catalog = ed_pin_choices(e, target, 0, pin);
    int count = catalog ? editor_choices_count(catalog) : 0;
    if (count) {
        for (int i = 0; i < count; i++) {
            editor_choice item = editor_choice_at(catalog, i);
            pin_value value = typed_editor_choice(spec, &item);
            char keywords[256];
            snprintf(
                keywords,
                sizeof keywords,
                "%s %s",
                item.name ? item.name : "",
                item.label ? item.label : ""
            );
            add_literal_choice(
                choices,
                item.label ? item.label : item.name,
                item.group ? item.group : "Values",
                keywords,
                ed_pin_type_name(spec),
                item.icon,
                100,
                !has_source && pin_value_equal(&value, &spec->default_value),
                &value
            );
        }
        return;
    }
    if (spec->type == PIN_BOOL) {
        pin_value value = { .type = PIN_BOOL, .container = spec->container };
        value.b = true;
        add_literal_choice(
            choices,
            "true",
            "Values",
            "boolean true",
            ed_pin_type_name(spec),
            KIT_ICON_CHECK,
            0,
            !has_source && spec->default_value.b,
            &value
        );
        value.b = false;
        add_literal_choice(
            choices,
            "false",
            "Values",
            "boolean false",
            ed_pin_type_name(spec),
            KIT_ICON_CLOSE,
            0,
            !has_source && !spec->default_value.b,
            &value
        );
        return;
    }
    if (spec->type == PIN_HANDLE) {
        pin_value none = { .type = PIN_HANDLE,
                           .type_name = spec->type_name,
                           .container = spec->container };
        add_literal_choice(
            choices,
            "None",
            "Values",
            "no handle empty",
            ed_pin_type_name(spec),
            KIT_ICON_CLOSE,
            0,
            !has_source && !spec->default_value.u64,
            &none
        );
        return;
    }
    if (!query || !query[0]) {
        return;
    }
    pin_value value;
    if (!parse_input_literal(spec, query, &value)) {
        return;
    }
    add_literal_choice(choices, query, "", query, ed_pin_type_name(spec), KIT_ICON_CONSTANT, 10000, false, &value);
}

static kit_command_item *autocomplete_items(const source_choice *choices)
{
    kit_command_item *items = NULL;
    for (int i = 0; i < acount(choices); i++) {
        const source_choice *choice = &choices[i];
        apush(
            items,
            ((kit_command_item){ .label = choice->label,
                                 .group = choice->group,
                                 .meta = choice->meta,
                                 .keywords = choice->keywords,
                                 .icon = choice->icon,
                                 .description = choice->description,
                                 .boost = choice->boost,
                                 .current = choice->current })
        );
    }
    return items;
}

static void forget_inline_node(editor *e, node *part, uint64_t id)
{
    ed_remove_selected(e, part);
    if (e->front_node == part) {
        e->front_node = NULL;
    }
    if (e->spawn_link_pin.n == part) {
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
        forget_inline_node(e, part, ids[i]);
    }
    int result = graph_remove_node_tree(gr, source);
    afree(ids);
    return result;
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
        const graph_local *local = graph_local_by_id(gr, binding->local_id);
        node *local_source = local ? graph_node_by_id(gr, local->source_id) : NULL;
        int is_output = 0;
        const pin_spec *local_spec = local_source
                                       ? graph_pin_by_id(local_source, local->source_pin_id, &is_output, NULL)
                                       : NULL;
        /* The picker only offers exact compatible locals; validate again
           before mutating in case the graph changed while it was open. */
        if (!local_spec || !is_output ||
            !pin_compatible(local_spec, &target->input_pins[pin])) {
            return false;
        }
        if (old_binding && graph_set_pin_binding(gr, target, 0, pin, NULL) != 0) {
            return false;
        }
        if (graph_use_local(gr, binding->local_id, target, pin) != 0) {
            return false;
        }
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

static bool apply_binding_choice(studio *s, node *target, int pin, const pin_binding *binding)
{
    editor *e = sg_graph_editor(s);
    char *action = graph_collab_action_set_binding(e, target, pin, binding);
    if (!action && collab_active() && !collab_replaying()) {
        return false;
    }
    collab_action_gate gate = action ? collab_actions_begin(action) : COLLAB_ACTION_APPLY;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return false;
    }

    behavior_asset *before = ed_snapshot(e);
    bool success = replace_input_binding(e, target, pin, binding);
    if (success) {
        e->pending_snapshot = before;
        ed_commit_snapshot(e);
    } else {
        (void)ed_adopt_behavior_asset(e, before);
        behavior_asset_free(before);
    }
    if (action) {
        success = collab_actions_finish(action, success) && success;
        free(action);
    }
    return success;
}

static bool apply_input_constant(studio *s, node *target, int pin, const pin_value *value)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    char *action = graph_collab_action_set_input_default(e, target, pin, value);
    if (!action && collab_active() && !collab_replaying()) {
        return false;
    }
    collab_action_gate gate = action ? collab_actions_begin(action) : COLLAB_ACTION_APPLY;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return false;
    }
    behavior_asset *before = ed_snapshot(e);
    bool success = replace_input_binding(e, target, pin, NULL) &&
                   graph_set_pin_default(gr, target, 0, pin, value) == 0;
    if (success) {
        e->pending_snapshot = before;
        ed_commit_snapshot(e);
        e->edit_pending = 1;
    } else {
        (void)ed_adopt_behavior_asset(e, before);
        behavior_asset_free(before);
    }
    if (action) {
        success = collab_actions_finish(action, success) && success;
        free(action);
    }
    return success;
}

static bool apply_expand_inline(studio *s, node *target, int pin)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    char *action = graph_collab_action_expand_inline(e, target, pin);
    if (!action && collab_active() && !collab_replaying()) {
        return false;
    }
    collab_action_gate gate = action ? collab_actions_begin(action) : COLLAB_ACTION_APPLY;
    if (gate != COLLAB_ACTION_APPLY) {
        free(action);
        return false;
    }
    int source_pin = -1;
    node *source = input_source(gr, target, pin, &source_pin);
    if (!source || !graph_node_is_inline(gr, source)) {
        if (action) {
            (void)collab_actions_finish(action, false);
            free(action);
        }
        return false;
    }
    uint64_t *ids = NULL;
    graph_inline_tree(gr, source, &ids);
    ed_snapshot_before(e);
    for (int i = 0; i < acount(ids); i++) {
        node *part = graph_node_by_id(gr, ids[i]);
        assert(part);
        graph_set_node_presentation(gr, part, NODE_PRESENTATION_EXPANDED);
        part->placed = false;
    }
    afree(ids);
    e->arrange_pending = true;
    if (action) {
        bool published = collab_actions_finish(action, true);
        free(action);
        return published;
    }
    return true;
}

static node *inline_expression(graph *gr, node *target, int pin)
{
    node *source = NULL;
    int source_pin = -1;
    if (!graph_input_source(gr, target, pin, &source, &source_pin) ||
        !graph_node_is_inline(gr, source)) {
        return NULL;
    }
    uint64_t *ids = NULL;
    graph_inline_tree(gr, source, &ids);
    node *expression = NULL;
    for (int i = 0; i < acount(ids) && !expression; i++) {
        node *part = graph_node_by_id(gr, ids[i]);
        if (part && part->desc == &NODE_BehaviorExpression) {
            expression = part;
        }
    }
    afree(ids);
    return expression;
}

static bool apply_choice(studio *s, kit *k, node *target, int pin, const source_choice *choice)
{
    graph *gr = sg_active_graph(s);
    if (choice->current) {
        return false;
    }
    studio_graph *state = &s->graph;
    if (choice->typed_literal) {
        pin_value value = choice_literal(choice);
        return apply_input_constant(s, target, pin, &value);
    }
    if (choice->kind == SOURCE_EDIT_EXPRESSION) {
        node *expression = inline_expression(gr, target, pin);
        if (!expression) {
            return false;
        }
        sg_begin_expression_edit(s, k, expression, target->input_pins[pin].id);
        return true;
    }
    if (choice->kind == SOURCE_EDIT_INLINE_INPUTS) {
        state->inline_edit_node = choice->node_id;
        state->inline_edit_pin = target->input_pins[pin].id;
        state->inline_edit_kit = k;
        state->inline_popup_pending = true;
        return true;
    }
    if (choice->kind == SOURCE_EXPAND_INLINE) {
        return apply_expand_inline(s, target, pin);
    }

    if (choice->kind == SOURCE_OUTPUT) {
        node *source = graph_node_by_id(gr, choice->node_id);
        if (!source || choice->pin < 0 || choice->pin >= source->output_count) {
            return false;
        }
        return sg_connect_pins(s, source, ED_PIN_OUTPUT, choice->pin, target, ED_PIN_INPUT, pin);
    }

    pin_binding binding = { 0 };
    const pin_binding *selected = NULL;
    if (choice->kind == SOURCE_BINDING) {
        binding = choice_binding(choice);
        selected = &binding;
    }
    return apply_binding_choice(s, target, pin, selected);
}

static void save_inline_anchor(studio *s, kit *k, const node *target, uint64_t pin_id)
{
    studio_graph *state = &s->graph;
    state->inline_anchor = kit_item_rect(k);
    state->inline_anchor_node = target->id;
    state->inline_anchor_pin = pin_id;
    state->inline_anchor_kit = k;
}

void sg_begin_expression_edit(studio *s, kit *k, node *expression, uint64_t owner_pin)
{
    studio_graph *state = &s->graph;
    char *source = studio_code_expression(s, expression);
    state->input_edit_node = expression->id;
    state->input_edit_pin = owner_pin;
    state->input_edit_kit = k;
    snprintf(state->input_edit_source, sizeof state->input_edit_source, "%s", source ? source : "");
    snprintf(state->input_edit, sizeof state->input_edit, "%s", source ? source : "");
    state->input_edit_focus = true;
    free(source);
}

bool sg_expression_editing(const studio *s, const kit *k, const node *expression, uint64_t owner_pin)
{
    const studio_graph *state = &s->graph;
    return expression && state->input_edit_kit == k &&
           state->input_edit_node == expression->id &&
           state->input_edit_pin == owner_pin;
}

void sg_emit_expression_editor(studio *s, kit *k, node *expression, uint64_t owner_pin)
{
    studio_graph *state = &s->graph;
    if (state->input_edit_kit != k || state->input_edit_pin != owner_pin) {
        return;
    }
    if (!expression || expression->desc != &NODE_BehaviorExpression ||
        !sg_expression_editing(s, k, expression, owner_pin) ||
        graph_node_by_id(sg_active_graph(s), expression->id) != expression) {
        state->input_edit_pin = state->input_edit_node = 0;
        state->input_edit_kit = NULL;
        state->input_edit_focus = false;
        return;
    }
    char *source = studio_code_expression(s, expression);
    if (strncmp(state->input_edit_source, source ? source : "", sizeof state->input_edit_source)) {
        snprintf(state->input_edit_source, sizeof state->input_edit_source, "%s", source ? source : "");
        snprintf(state->input_edit, sizeof state->input_edit, "%s", source ? source : "");
    }
    free(source);
    kit_push(k, "expression-editor", NULL, KIT_CX(k, "graph-expression-editor", { "standalone", !owner_pin }));
    char width[32];
    float room = owner_pin ? 260 : sg_layout_of(s, expression)->w - 16;
    snprintf(
        width,
        sizeof width,
        "%.0fdp",
        fminf(room, fminf(260, fmaxf(128, sg_text_width(s, state->input_edit, 11, 400) + 24)))
    );
    kit_style(k, "width", width);
    if (!owner_pin) {
        snprintf(width, sizeof width, "%.0fdp", sg_code_height(sg_layout_of(s, expression)) - 4);
        kit_style(k, "height", width);
    }
    if (kit_text_field(
            k,
            "expression",
            state->input_edit,
            sizeof state->input_edit,
            "a + b",
            sg_graph_readonly(s) ? KIT_READONLY : 0
        )) {
        if (sg_set_expression_source(s, expression, state->input_edit) == 0) {
            state->input_edit_pin = state->input_edit_node = 0;
            state->input_edit_kit = NULL;
            state->input_edit_focus = false;
        } else if (state->expression_error[0]) {
            sg_show_error(s, "Can't use this expression", state->expression_error);
        }
    }
    if (state->input_edit_focus) {
        kit_focus(k);
        state->input_edit_focus = false;
    }
    kit_pop(k);
}

static void inline_popup_key(char *out, size_t size, const node *target, uint64_t input_pin)
{
    snprintf(out, size, "inline-values-%llu-%llu", (unsigned long long)target->id, (unsigned long long)input_pin);
}

static void open_pending_inline_popup(studio *s, kit *k, node *target, uint64_t pin_id)
{
    studio_graph *state = &s->graph;
    if (!state->inline_popup_pending || state->inline_edit_kit != k ||
        state->inline_edit_pin != pin_id || state->inline_anchor_kit != k ||
        state->inline_anchor_node != target->id || state->inline_anchor_pin != pin_id) {
        return;
    }
    char key[64];
    inline_popup_key(key, sizeof key, target, pin_id);
    kit_popup_open_at(
        k,
        key,
        state->inline_anchor.x,
        state->inline_anchor.y + state->inline_anchor.h
    );
    state->inline_popup_pending = false;
}

static void emit_inline_values_popup(studio *s, kit *k, node *target, int pin)
{
    studio_graph *state = &s->graph;
    if (state->inline_edit_kit != k ||
        state->inline_edit_pin != target->input_pins[pin].id ||
        state->inline_edit_node == 0) {
        return;
    }
    graph *gr = sg_active_graph(s);
    int source_pin = -1;
    node *source = input_source(gr, target, pin, &source_pin);
    if (!source || source->id != state->inline_edit_node ||
        !graph_node_is_inline(gr, source)) {
        char popup_key[64];
        inline_popup_key(
            popup_key,
            sizeof popup_key,
            target,
            target->input_pins[pin].id
        );
        kit_popup_close(k, popup_key);
        state->inline_edit_pin = state->inline_edit_node = 0;
        state->inline_edit_kit = NULL;
        return;
    }
    char popup_key[64];
    inline_popup_key(
        popup_key,
        sizeof popup_key,
        target,
        target->input_pins[pin].id
    );
    if (!kit_popup_is_open(k, popup_key) ||
        !kit_popup_begin(k, popup_key, "popover graph-inline-values")) {
        return;
    }

    editor *e = sg_graph_editor(s);
    kit_label(k, "heading", "graph-inline-heading", "Inline inputs");
    uint64_t *ids = NULL;
    graph_inline_tree(gr, source, &ids);
    for (int i = 0; i < acount(ids); i++) {
        node *part = graph_node_by_id(gr, ids[i]);
        /* Editing an input can remove a nested inline source that appears
           later in this captured tree. Skip it after the mutation. */
        if (!part) {
            continue;
        }
        char title[128], section[48];
        ed_node_title(e, part, title, sizeof title);
        snprintf(section, sizeof section, "node-%llu", (unsigned long long)part->id);
        kit_push(k, section, NULL, "graph-inline-node");
        kit_label(k, "title", "graph-inline-title", title);
        for (int input = 0; input < part->input_count; input++) {
            const pin_spec *spec = &part->input_pins[input];
            if (spec->kind != PIN_DATA ||
                !ed_pin_visible(gr, part, ED_PIN_INPUT, input)) {
                continue;
            }
            char row[24], label[128];
            snprintf(row, sizeof row, "input-%d", input);
            studio_code_pin_label(s, part, spec, label, sizeof label);
            kit_push(k, row, NULL, "graph-inline-row");
            kit_label(k, "label", "graph-inline-label", label);
            sg_emit_graph_input(s, k, part, input);
            kit_pop(k);
        }
        for (int output = 0; output < part->output_count; output++) {
            const pin_spec *spec = &part->output_pins[output];
            if (spec->kind != PIN_DATA ||
                !ed_pin_editable(gr, part, ED_PIN_OUTPUT, output)) {
                continue;
            }
            char row[24], label[128];
            snprintf(row, sizeof row, "output-%d", output);
            studio_code_pin_label(s, part, spec, label, sizeof label);
            kit_push(k, row, NULL, "graph-inline-row");
            kit_label(k, "label", "graph-inline-label", label);
            sg_emit_value_in(s, k, part, 1, output);
            kit_pop(k);
        }
        kit_pop(k);
    }
    afree(ids);
    kit_popup_end(k);
}

static bool private_expression_source(
    const behavior_asset *asset,
    const graph *gr,
    const node *source,
    const node *target,
    int pin
)
{
    if (!source || source->desc != &NODE_BehaviorExpression) {
        return false;
    }
    for (int i = 0; i < acount(asset->graphs); i++) {
        const graph *caller = asset->graphs[i];
        for (int j = 0; j < acount(caller->nodes); j++) {
            const node *call = caller->nodes[j];
            if (call != source && !call->reference_asset &&
                call->reference_id == source->reference_id) {
                return false;
            }
        }
    }
    const edge *only = NULL;
    for (int i = 0; i < acount(gr->edges); i++) {
        const edge *wire = &gr->edges[i];
        if (wire->kind != GRAPH_EDGE_DATA || wire->from_id != source->id) {
            continue;
        }
        if (only) {
            return false;
        }
        only = wire;
    }
    return only && only->to_id == target->id &&
           only->to_pin_id == target->input_pins[pin].id;
}

static void graph_input_display(
    studio *s,
    const pin_spec *spec,
    const pin_parts *parts,
    node *source,
    int source_pin,
    bool expression_source,
    const pin_binding *binding,
    char *out,
    size_t size
)
{
    editor *e = sg_graph_editor(s);
    if (binding && (binding->kind == PIN_BINDING_COMPONENT ||
                    binding->kind == PIN_BINDING_COMPONENT_FIELD)) {
        const ecs_component_desc *component = ed_component_by_key(binding->component_key);
        char name[96];
        if (component) {
            component_label(component, name, sizeof name);
        } else {
            snprintf(name, sizeof name, "%s", binding->component_key);
        }
        snprintf(
            out,
            size,
            "%s%s%s",
            name,
            binding->field_name ? "." : "",
            binding->field_name ? binding->field_name : ""
        );
        return;
    }
    if (binding) {
        char label[256];
        snprintf(out, size, "%s", ed_binding_label(e, binding, label, sizeof label));
        return;
    }
    if (expression_source) {
        char *readable = studio_code_expression(s, source);
        snprintf(out, size, "%s", readable ? readable : "");
        free(readable);
        return;
    }
    if (parts->value == VALUE_CHIP && parts->chip[0] &&
        (!source || graph_node_is_inline(e->active_graph, source))) {
        snprintf(out, size, "%s", parts->chip);
        return;
    }
    if (source) {
        char title[128], pin_label[96];
        ed_node_title(e, source, title, sizeof title);
        ed_pin_label(source, &source->output_pins[source_pin], pin_label, sizeof pin_label);
        snprintf(out, size, "%s.%s", title, pin_label);
        return;
    }
    if (spec->type == PIN_HANDLE && !spec->default_value.u64) {
        snprintf(out, size, "None");
        return;
    }
    snprintf(out, size, "%s", pin_value_to_string(&spec->default_value));
}

/* A vector, color or compound input keeps its value editor, and Bind to
   Source beside it swaps in the input's source field, as typing a path in
   a scalar input's field binds it. */
static void bind_button(studio *s, kit *k, uint64_t pin)
{
    studio_graph *g = &s->graph;
    kit_button_desc bind = { .icon = KIT_ICON_BINDING,
                             .size = KIT_SM,
                             .cls = "gbind",
                             .tip = "Bind to Source",
                             .description = "Type a property path, or choose a "
                                            "value for this input to read." };
    if (kit_button(k, "bind", &bind)) {
        g->bind_pin = pin;
        g->bind_kit = k;
        g->bind_focus = true;
        g->details.bind[0] = '\0';
    }
}

/* The field Bind to Source swapped in takes the focus, which opens its list;
   the value editor returns once the list closes, or if it never opened. */
static void bind_follow(studio *s, kit *k, const char *query)
{
    studio_graph *g = &s->graph;
    if (g->bind_focus) {
        kit_focus(k);
        g->bind_focus = false;
        return;
    }
    if (!query) {
        g->bind_pin = 0;
        g->bind_kit = NULL;
    }
}

/* Enter on text that is neither a value of the input's type nor a source
   keeps the field open, marked invalid with a tip that says what it takes,
   until the text changes (Postel). */
static void refuse_text(studio *s, const pin_spec *spec, const char *text)
{
    studio_graph *g = &s->graph;
    g->refused_pin = spec->id;
    snprintf(g->refused, sizeof g->refused, "%s", text);
}

static void refusal_mark(studio *s, kit *k, const pin_spec *spec, const char *query)
{
    studio_graph *g = &s->graph;
    bool refused = query && g->refused_pin == spec->id && !strcmp(g->refused, query);
    char type[96], title[128];
    sm_type_label(spec, type, sizeof type);
    snprintf(title, sizeof title, "Not a %s or a source", type);
    kit_flag(k, "data-invalid", refused);
    kit_tip(k, refused ? title : NULL, NULL, refused ? "Type a value, or choose a source from the list." : NULL);
}

void sg_emit_graph_input(studio *s, kit *k, node *n, int p)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    if (!e || !gr || !n || p < 0 || p >= n->input_count) {
        return;
    }
    uint64_t node_id = n->id;
    const pin_spec *spec = &n->input_pins[p];
    if (spec->kind != PIN_DATA) {
        kit_label(k, "value-kind", "dim", "Execution");
        return;
    }
    pin_parts parts;
    sg_pin_parts_of(s, n, ED_PIN_INPUT, p, &parts);
    bool material_asset = spec->type == PIN_STR && spec->type_name &&
                          !strcmp(spec->type_name, "MaterialAsset");
    if (material_asset) {
        sg_emit_value_in(s, k, n, 0, p);
        return;
    }
    bool can_choose_source = parts.source_select && !sg_graph_readonly(s) &&
                             !(spec->flags & PIN_FLAG_SETTING);
    int source_pin = -1;
    node *source = input_source(gr, n, p, &source_pin);
    pin_binding resolved_binding = { 0 };
    const pin_binding *binding = current_binding(gr, n, p, source, source_pin, &resolved_binding);
    bool expression_source = private_expression_source(e->asset, gr, source, n, p);
    bool compound_literal = !source && !binding &&
                            (parts.value == VALUE_VECTOR || parts.value == VALUE_COLOR ||
                             parts.value == VALUE_COMPOUND);
    bool bind = s->graph.bind_pin == spec->id && s->graph.bind_kit == k;
    bool autocomplete = can_choose_source && (!compound_literal || bind);

    if (s->graph.input_edit_pin == spec->id && s->graph.input_edit_kit == k) {
        node *expression = graph_node_by_id(gr, s->graph.input_edit_node);
        sg_emit_expression_editor(s, k, expression, spec->id);
    } else if (autocomplete) {
        /* A compound value's field starts empty: it takes a source. */
        char displayed[256] = "";
        if (!bind) {
            graph_input_display(s, spec, &parts, source, source_pin, expression_source, binding, displayed, sizeof displayed);
        }
        const char *query = kit_autocomplete_query(k, "source-edit");
        source_choice *choices = NULL;
        kit_command_item *items = NULL;
        if (query) {
            add_sources(s, n, p, query, &choices, source, source_pin, binding);
            add_typed_choices(e, n, p, query, source || binding, &choices);
            items = autocomplete_items(choices);
        }
        kit_autocomplete_result result = kit_autocomplete(
            k,
            "source-edit",
            displayed,
            items,
            acount(items),
            "Value or source",
            sg_graph_readonly(s) ? KIT_READONLY : 0
        );
        refusal_mark(s, k, spec, query);
        if (result.selected >= 0 && result.selected < acount(choices)) {
            apply_choice(s, k, n, p, &choices[result.selected]);
            kit_autocomplete_close(k, "source-edit");
        } else if (result.submitted && query) {
            pin_value literal;
            if (parse_input_literal(spec, query, &literal)) {
                apply_input_constant(s, n, p, &literal);
                kit_autocomplete_close(k, "source-edit");
            } else if (expression_source) {
                if (sg_set_expression_source(s, source, query) == 0) {
                    kit_autocomplete_close(k, "source-edit");
                } else if (s->graph.expression_error[0]) {
                    sg_show_error(s, "Can't use this expression", s->graph.expression_error);
                }
            } else {
                refuse_text(s, spec, query);
            }
        }
        if (bind) {
            bind_follow(s, k, query);
        }
        afree(items);
        afree(choices);
    } else if (parts.value == VALUE_CHIP) {
        kit_label(k, "source-context", "chip graph-source-context", parts.chip);
    } else if (parts.value != VALUE_NONE) {
        uint64_t pin = spec->id; /* an edit may move the node's pins */
        sg_emit_value_in(s, k, n, 0, p);
        if (compound_literal && can_choose_source) {
            bind_button(s, k, pin);
        }
    } else if (source) {
        char label[256];
        graph_input_display(s, spec, &parts, source, source_pin, expression_source, binding, label, sizeof label);
        kit_label(k, "source-context", "chip graph-source-context", label);
    } else if (ed_pin_editable(gr, n, ED_PIN_INPUT, p) && !(spec->flags & PIN_FLAG_SETTING)) {
        sg_emit_value_in(s, k, n, 0, p);
    }

    /* Expression edits publish a replacement asset. Rebind the consuming
     * node before emitting anchors and popups that follow the field. */
    n = graph_node_by_id(sg_active_graph(s), node_id);
    assert(n && p < n->input_count);
    spec = &n->input_pins[p];

    if (parts.inline_source && autocomplete) {
        save_inline_anchor(s, k, n, spec->id);
    }
    if (parts.inline_source && autocomplete) {
        open_pending_inline_popup(s, k, n, spec->id);
    }
    emit_inline_values_popup(s, k, n, p);
}

/* ---------- Properties' inputs (workstream G1) ---------- */

bool sg_input_bindable(studio *s, node *n, int p)
{
    const pin_spec *spec = &n->input_pins[p];
    pin_parts parts;
    sg_pin_parts_of(s, n, ED_PIN_INPUT, p, &parts);
    return parts.source_select && !(spec->flags & PIN_FLAG_SETTING) &&
           (parts.value == VALUE_VECTOR || parts.value == VALUE_COLOR ||
            parts.value == VALUE_COMPOUND);
}

sg_input_kind sg_input_view(studio *s, node *n, int p, char *text, size_t size)
{
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    const pin_spec *spec = &n->input_pins[p];
    text[0] = '\0';
    if (spec->kind != PIN_DATA) {
        return SG_INPUT_EXEC;
    }
    if (spec->type == PIN_STR && spec->type_name &&
        !strcmp(spec->type_name, "MaterialAsset")) {
        return SG_INPUT_VALUE;
    }
    pin_parts parts;
    sg_pin_parts_of(s, n, ED_PIN_INPUT, p, &parts);
    bool can_choose_source = parts.source_select && !sg_graph_readonly(s) &&
                             !(spec->flags & PIN_FLAG_SETTING);
    int source_pin = -1;
    node *source = input_source(gr, n, p, &source_pin);
    pin_binding resolved = { 0 };
    const pin_binding *binding = current_binding(gr, n, p, source, source_pin, &resolved);
    bool expression = private_expression_source(e->asset, gr, source, n, p);
    bool compound_literal = !source && !binding &&
                            (parts.value == VALUE_VECTOR || parts.value == VALUE_COLOR ||
                             parts.value == VALUE_COMPOUND);
    bool bind = s->graph.bind_pin == spec->id && !s->graph.bind_kit;
    if (can_choose_source && (!compound_literal || bind)) {
        if (!bind) {
            graph_input_display(s, spec, &parts, source, source_pin, expression, binding, text, size);
        }
        return SG_INPUT_SOURCE;
    }
    if (parts.value == VALUE_CHIP) {
        snprintf(text, size, "%s", parts.chip);
        return SG_INPUT_CHIP;
    }
    if (parts.value != VALUE_NONE) {
        return SG_INPUT_VALUE;
    }
    if (source) {
        graph_input_display(s, spec, &parts, source, source_pin, expression, binding, text, size);
        return SG_INPUT_CHIP;
    }
    bool editable = ed_pin_editable(gr, n, ED_PIN_INPUT, p) &&
                    !(spec->flags & PIN_FLAG_SETTING);
    return editable ? SG_INPUT_VALUE : SG_INPUT_NONE;
}

/* The input whose source list is open, or NULL. */
static node *listed_input(studio *s, int *p)
{
    studio_graph *g = &s->graph;
    node *n = sg_node_by_id(s, g->details.source_node);
    *p = g->details.source_pin;
    return n && *p >= 0 && *p < n->input_count ? n : NULL;
}

void sg_source_open(studio *s, node *n, int p, const char *row, rml_element *field)
{
    studio_graph *g = &s->graph;
    g->details.source_node = n->id;
    g->details.source_pin = p;
    g->details.source_shown = false;
    snprintf(g->details.source, sizeof g->details.source, "%s", row);
    studio_palette_open_field(s, STUDIO_PALETTE_SOURCES, field);
}

/* What the source list offers for its query, as the canvas's field offers:
   values of the input's type, bindings and outputs that fit it, and, for a
   folded expression, the source typed. */
void studio_graph_source_entries(studio *s, const char *query, studio_palette_entry **entries)
{
    studio_graph *g = &s->graph;
    afree(g->details.source_choices);
    g->details.source_choices = NULL;
    int p;
    node *n = listed_input(s, &p);
    if (!n) {
        return;
    }
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    source_choice **choices = &g->details.source_choices;
    int source_pin = -1;
    node *source = input_source(gr, n, p, &source_pin);
    pin_binding resolved = { 0 };
    const pin_binding *binding = current_binding(gr, n, p, source, source_pin, &resolved);
    add_sources(s, n, p, query, choices, source, source_pin, binding);
    add_typed_choices(e, n, p, query, source || binding, choices);
    pin_value literal;
    if (private_expression_source(e->asset, gr, source, n, p) && query[0] &&
        !parse_input_literal(&n->input_pins[p], query, &literal)) {
        add_choice(
            choices,
            SOURCE_TYPED_EXPRESSION,
            query,
            "",
            query,
            NULL,
            "Use this expression",
            KIT_ICON_EXPRESSION,
            10000,
            false,
            NULL,
            source->id,
            source_pin
        );
    }
    for (int i = 0; i < acount(*choices); i++) {
        const source_choice *choice = &(*choices)[i];
        studio_palette_entry
            *entry = studio_palette_add(entries, choice->group, choice->label);
        snprintf(entry->keywords, sizeof entry->keywords, "%s", choice->keywords);
        snprintf(entry->meta, sizeof entry->meta, "%s", choice->meta);
        /* A described result's tooltip names it, then says what it does. */
        snprintf(entry->about, sizeof entry->about, "%s", choice->description);
        snprintf(entry->tip, sizeof entry->tip, "%s", choice->label);
        entry->icon = choice->icon;
        entry->boost = choice->boost;
        entry->current = choice->current;
        entry->kind = i;
    }
}

void studio_graph_source_choose(studio *s, const studio_palette_entry *entry)
{
    studio_graph *g = &s->graph;
    int p;
    node *n = listed_input(s, &p);
    if (!n || entry->kind < 0 || entry->kind >= acount(g->details.source_choices)) {
        return;
    }
    source_choice choice = g->details.source_choices[entry->kind];
    if (choice.kind == SOURCE_TYPED_EXPRESSION) {
        node *expression = sg_node_by_id(s, choice.node_id);
        if (expression && sg_set_expression_source(s, expression, choice.label) != 0 &&
            g->expression_error[0]) {
            sg_show_error(s, "Can't use this expression", g->expression_error);
        }
        return;
    }
    apply_choice(s, NULL, n, p, &choice);
    if (choice.kind == SOURCE_EDIT_INLINE_INPUTS) {
        snprintf(g->details.popup, sizeof g->details.popup, "%s", g->details.source);
    }
}

void sg_source_free(studio *s)
{
    afree(s->graph.details.source_choices);
    s->graph.details.source_choices = NULL;
}
