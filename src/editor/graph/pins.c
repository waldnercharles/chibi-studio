#include "graph/nodes/core_nodes.h"
#include "graph/authoring/graph_edit.h"
#include "editor/ui/labels.h"
#include "editor/workspace/editor_internal.h"
#include "graph/authoring/layout.h"
#include "graph/model/local.h"
#include "graph/model/pin_conv.h"
#include "graph/nodes/component_nodes.h"
#include "graph/shader/shader_graph.h"

#include <stdio.h>
#include <string.h>

static const editor_choice ramp_interpolation_items[] = {
    { .value = 0, .label = "Linear" },
    { .value = 1, .label = "Constant" },
    { .value = 2, .label = "Smoothstep" },
};
static const editor_choices ramp_interpolation_choices = {
    .items = ramp_interpolation_items,
    .count = sizeof ramp_interpolation_items / sizeof *ramp_interpolation_items,
};
static const editor_choice image_color_space_items[] = {
    { .value = SHADER_IMAGE_SRGB, .label = "sRGB" },
    { .value = SHADER_IMAGE_LINEAR, .label = "Linear" },
};
static const editor_choices image_color_space_choices = {
    .items = image_color_space_items,
    .count = sizeof image_color_space_items / sizeof *image_color_space_items,
};
static const editor_choice image_filter_items[] = {
    { .value = SHADER_IMAGE_NEAREST, .label = "Nearest" },
    { .value = SHADER_IMAGE_BILINEAR, .label = "Bilinear" },
};
static const editor_choices image_filter_choices = {
    .items = image_filter_items,
    .count = sizeof image_filter_items / sizeof *image_filter_items,
};
static const editor_choice image_wrap_items[] = {
    { .value = SHADER_IMAGE_REPEAT, .label = "Repeat" },
    { .value = SHADER_IMAGE_CLAMP, .label = "Clamp" },
};
static const editor_choices image_wrap_choices = {
    .items = image_wrap_items,
    .count = sizeof image_wrap_items / sizeof *image_wrap_items,
};

const char *ed_pin_type_name(const pin_spec *spec)
{
    if (spec->type_name && spec->type_name[0]) {
        return spec->type_name;
    }
    if (spec->kind == PIN_EXEC) {
        return "exec";
    }
    const char *name = pin_type_name(spec->type);
    return name ? name : "unknown";
}

bool ed_pin_visible(const graph *g, const node *n, int kind, int pin)
{
    return graph_pin_visible(g, n, kind == ED_PIN_OUTPUT, pin);
}

static bool editable_output(const node *n)
{
    if (n->desc->category && !strncmp(n->desc->category, "Shader", 6)) {
        return !strcmp(n->desc->name, "Shader Constant") ||
               !strcmp(n->desc->name, "Shader Parameter");
    }
    return n->desc->pure_kind == NODE_PURE_CONST &&
           n->desc->display_class == NODE_DISPLAY_LITERAL;
}

int ed_pin_editable(const graph *g, const node *n, int kind, int pin)
{
    if (g->math_expression || !ed_pin_visible(g, n, kind, pin)) {
        return 0;
    }
    const pin_spec *spec = kind == ED_PIN_INPUT ? &n->input_pins[pin]
                                                : &n->output_pins[pin];
    node *source = NULL;
    int source_pin = -1;
    if (kind == ED_PIN_INPUT &&
        (spec->binding || graph_inline_source(g, n, pin, &source, &source_pin))) {
        return 1;
    }
    if (spec->kind != PIN_DATA || spec->field_source ||
        (spec->type == PIN_HANDLE && spec->type_name &&
         (!strcmp(spec->type_name, "Geometry3D") || !strcmp(spec->type_name, "Profile2D") ||
          !strcmp(spec->type_name, "ModelMaterialSlot") ||
          !strcmp(spec->type_name, "Anchor3D")))) {
        return 0;
    }
    return kind == ED_PIN_INPUT ? !graph_input_source(g, n, pin, &source, &source_pin)
                                : editable_output(n);
}

bool ed_node_has_advanced_inputs(const node *n)
{
    for (int p = 0; p < n->input_count; p++) {
        if (n->input_pins[p].flags & PIN_FLAG_ADVANCED) {
            return true;
        }
    }
    return false;
}

void ed_pin_label(const node *n, const pin_spec *pin, char *buf, size_t cap)
{
    if (pin->display_name && pin->display_name[0]) {
        snprintf(buf, cap, "%s", pin->display_name);
        return;
    }
    /* A generated field setter reads like Set Component showing that field. */
    if (n->desc->component_kind == NODE_COMPONENT_SET_FIELD && n->input_count == 3 &&
        (pin == &n->input_pins[1] || pin == &n->input_pins[2])) {
        const type_desc *type = n->desc->component->type;
        const char *field = type->fields[n->desc->component_field].name;
        editor_identifier_label(pin == &n->input_pins[1] ? field : "Entity", buf, cap);
        return;
    }
    /* An expression's inputs are the names its source text uses. */
    if (n->desc == &NODE_BehaviorExpression && pin->kind == PIN_DATA && pin->name) {
        snprintf(buf, cap, "%s", pin->display_name && pin->display_name[0] ? pin->display_name : pin->name);
        return;
    }
    editor_identifier_label(pin->name ? pin->name : "Pin", buf, cap);
    if (!pin->field_source) {
        return;
    }
    int aggregates = 0;
    const pin_spec *source = NULL;
    for (int i = 0; i < n->output_count; i++) {
        aggregates += graph_output_struct(n, i) != NULL;
        if (n->output_pins[i].id == pin->field_source) {
            source = &n->output_pins[i];
        }
    }
    if (aggregates > 1 && source) {
        char name[128], field[128];
        editor_identifier_label(source->name, name, sizeof name);
        editor_identifier_label(pin->name, field, sizeof field);
        snprintf(buf, cap, "%s %s", name, field);
    }
}

const editor_choices *ed_pin_choices(editor *e, node *n, int output, int index)
{
    if (output) {
        return NULL;
    }
    const pin_spec *spec = &n->input_pins[index];
    if ((spec->type != PIN_I32 && spec->type != PIN_ENUM && spec->type != PIN_NAME) ||
        spec->container != PIN_CONTAINER_SINGLE) {
        return NULL;
    }
    /* Set UI Data's Name offers what its documents bind under self. */
    if (e->ui_data_choices.count && index == 2 &&
        n->desc == registry_node_by_name("Set UI Data")) {
        return &e->ui_data_choices;
    }
    const editor_choices *choices = e->choices_resolver
                                      ? e->choices_resolver(n, output, index, e->choices_user)
                                      : NULL;
    if (choices) {
        return choices;
    }
    if (n->desc == registry_node_by_name("Shader Color Ramp") && index == 2) {
        return &ramp_interpolation_choices;
    }
    if (n->desc == registry_node_by_name("Shader Image Texture")) {
        switch (index) {
            case SHADER_IMAGE_INPUT_COLOR_SPACE:
                return &image_color_space_choices;
            case SHADER_IMAGE_INPUT_FILTER: return &image_filter_choices;
            case SHADER_IMAGE_INPUT_WRAP: return &image_wrap_choices;
            default: break;
        }
    }
    return spec->type == PIN_ENUM ? editor_choices_for_enum(spec->type_name) : NULL;
}
