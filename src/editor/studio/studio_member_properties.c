#include "graph/authoring/asset_edit.h"
/* The Member tab in Properties: the details of the member selected in
 * Members, as Unreal's Details shows a My Blueprint selection, as rows of
 * graph_details (studio_graph_details.c). A variable has its type,
 * container, category and default; a function or macro its inputs and
 * outputs, with their containers and the inputs' defaults, reordered by
 * dragging their grips, and a function its purity; a named value its name and
 * source. Types are chosen from a search over basic types, structures and
 * enums, and Exec for a macro (Unreal's pin type picker), in the palette's
 * picker under the type. A row's id names what it edits: v<variable>,
 * g<graph> or l<local>, then a part letter and, for a pin, p<pin id>.
 * Capabilities edit the behavior over members, pins and values; intents
 * only read the rows' events into them. */
#include "editor/studio/studio_members_internal.h"
#include "editor/ui/choices.h"
#include "graph/model/pin_conv.h"
#include "graph/model/registry.h"
#include "support/arena.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "types/text.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char *label;
    pin_type type;
    const char *type_name;
} type_choice;

static const type_choice TYPES[] = {
    { "Boolean", PIN_BOOL, "bool" },   { "Integer", PIN_I32, "int" },
    { "Float", PIN_F32, "float" },     { "Double", PIN_F64, "double" },
    { "Name", PIN_NAME, "name" },      { "String", PIN_STR, "const char *" },
    { "Vector", PIN_STRUCT, "CF_V2" }, { "Entity", PIN_HANDLE, "ecs.entity" },
};
enum
{
    TYPE_COUNT = sizeof TYPES / sizeof *TYPES,
};
/* A macro's tunnels also carry execution (Unreal's Exec type). */
static const type_choice EXEC = { "Exec", PIN_FLOW, NULL };

/* A basic type matches by type; a named one also by its name. */
static bool same_type(pin_type type, const char *type_name, const type_choice *choice)
{
    bool named = type == PIN_STRUCT || type == PIN_HANDLE || type == PIN_ENUM;
    return type == choice->type &&
           (!named || (type_name && !strcmp(type_name, choice->type_name)));
}

const char *sm_type_label(const pin_spec *pin, char *out, size_t size)
{
    static const char *const SUFFIXES[] = { "", " Array", " Set", " Map" };
    if (pin->kind == PIN_EXEC) {
        snprintf(out, size, "%s", EXEC.label);
        return out;
    }
    const char *label = pin->type_name && pin->type_name[0]
                          ? pin->type_name
                          : pin_type_name(pin->type);
    for (int i = 0; i < TYPE_COUNT; i++) {
        label = same_type(pin->type, pin->type_name, &TYPES[i]) ? TYPES[i].label : label;
    }
    snprintf(out, size, "%s%s", label ? label : "?", SUFFIXES[pin->container]);
    return out;
}

/* ---------- Shared parts ---------- */

static editor *member_editor(const studio *s)
{ return sg_graph_editor(s); }

static bool locked(const studio *s)
{ return !behavior_asset_mutable(member_editor(s)->asset); }

static void row_id(char out[GD_ROW_ID], char owner, uint64_t id, const char *part)
{ snprintf(out, GD_ROW_ID, "%c%" PRIu64 ".%s", owner, id, part); }

static void text_row(
    gd_rows *r,
    const char *id,
    const char *label,
    const char *text,
    const char *placeholder,
    bool readonly
)
{
    yyjson_mut_val *row = gd_next_row(
        r,
        &(gd_row){ .id = id, .label = label, .control = "text", .readonly = readonly }
    );
    gd_put_text(r, row, "text", text);
    gd_put_text(r, row, "placeholder", placeholder);
}

static void label_row(gd_rows *r, const char *id, const char *label, const char *text)
{
    gd_put_text(r, gd_next_row(r, &(gd_row){ .id = id, .label = label, .control = "label" }), "text", text);
}

/* The type as a swatch and a button that opens the type picker. The
   container has its own control, so the button names the value type alone
   (Unreal). */
static void put_type(gd_rows *r, yyjson_mut_val *row, const pin_spec *pin)
{
    char label[96];
    pin_spec value = *pin;
    value.container = PIN_CONTAINER_SINGLE;
    gd_put_type(r, row, pin, sm_type_label(&value, label, sizeof label));
}

/* Single values of the types a default edits. */
static bool value_editable(const pin_spec *pin)
{
    bool enumeration = pin->type == PIN_ENUM && type_catalog_enum_find(pin->type_name);
    return pin->kind == PIN_DATA && pin->container == PIN_CONTAINER_SINGLE &&
           (enumeration || pin->type == PIN_BOOL || pin_vector_width(pin) > 0 ||
            pin->type == PIN_STR || pin->type == PIN_NAME ||
            (pin->type >= PIN_I8 && pin->type <= PIN_F64));
}

/* The control a default of `pin`'s type edits with, as the canvas edits pin
   values: numbers and vectors scrub. */
static const char *default_control(const pin_spec *pin)
{
    if (pin->type == PIN_ENUM) { return "choice"; }
    if (pin->type == PIN_BOOL) { return "bool"; }
    if (pin_vector_width(pin) > 0) { return "vector"; }
    return pin->type == PIN_STR || pin->type == PIN_NAME ? "text" : "number";
}

/* Writes a default's value into `row`, for its default_control. */
static void put_default(gd_rows *r, yyjson_mut_val *row, const pin_spec *pin, const pin_value *value)
{
    if (pin->type == PIN_ENUM) {
        const editor_choices *choices = editor_choices_for_enum(pin->type_name);
        int current = -1;
        for (int i = 0; choices && i < editor_choices_count(choices); i++) {
            current = editor_choice_at(choices, i).value == value->e ? i : current;
        }
        gd_put_choices(r, row, choices, current);
        return;
    }
    if (pin->type == PIN_BOOL) {
        gd_put_flag(r, row, "checked", value->b);
        return;
    }
    if (pin_vector_width(pin) > 0) {
        double axes[4] = { 0 };
        const float *v = value->ptr;
        for (int a = 0; v && a < pin_vector_width(pin); a++) { axes[a] = v[a]; }
        gd_number_shape shape = { .step = 0.1,
                                  .precision = 2,
                                  .single = true,
                                  .min = -INFINITY,
                                  .max = INFINITY };
        gd_put_axes(r, row, axes, pin_vector_width(pin), &shape);
        return;
    }
    if (pin->type == PIN_STR || pin->type == PIN_NAME) {
        arena scratch = { 0 };
        const char *text = NULL;
        const char *error = pin_text_cstring(&scratch, *value, &text, NULL);
        gd_put_text(r, row, "text", error ? "" : text);
        gd_put_text(r, row, "placeholder", error ? error : "");
        if (error) {
            gd_put_flag(r, row, "readonly", true);
            gd_put_flag(r, row, "invalid", true);
        }
        arena_free(&scratch);
        return;
    }
    bool integer = pin->type < PIN_F32;
    gd_number_shape shape = { .integer = integer,
                              .single = pin->type == PIN_F32,
                              .precision = integer ? 0 : 2,
                              .step = integer ? 1 : 0.1,
                              .min = -INFINITY,
                              .max = INFINITY };
    gd_put_number(r, row, sg_literal_number(value), &shape);
}

/* ---------- Variables ---------- */

static void source_variable_details(gd_view *v, studio *s, const source_presentation_origin *source)
{
    node *n = sm_source_variable_node(s, source);
    uint64_t id = n->reference_id;
    char row[GD_ROW_ID];
    gd_identity(
        v,
        KIT_ICON_VARIABLE,
        n->desc->name,
        source->operation == SCRIPT_SHARED_STATE ? "Shared variable" : "Variable"
    );
    gd_rows r = gd_panel(v, "variable", "Variable");
    if (r.open) {
        row_id(row, 'v', id, "n");
        text_row(&r, row, "Name", n->desc->name, "", locked(s));
        row_id(row, 'v', id, "t");
        char type[96];
        gd_put_type(
            &r,
            gd_next_row(&r, &(gd_row){ .id = row, .label = "Type", .control = "type", .readonly = locked(s) }),
            &n->output_pins[0],
            sm_type_label(&n->output_pins[0], type, sizeof type)
        );
    }
    gd_panel_end(&r);
    if (n->input_count) {
        gd_rows defaults = gd_panel(v, "default", "Default Value");
        for (int p = 0; defaults.open && p < n->input_count; p++) {
            sg_input_row(&defaults, s, n, p);
        }
        gd_panel_end(&defaults);
    }
}

static void variable_details(gd_view *v, studio *s, const behavior_variable *var)
{
    bool fixed = locked(s);
    pin_spec pin = sm_variable_pin(var);
    char id[GD_ROW_ID], type[96];
    gd_identity(v, KIT_ICON_VARIABLE, var->name, var->category && var->category[0] ? var->category : "Variable");
    gd_rows r = gd_panel(v, "variable", "Variable");
    if (r.open) {
        row_id(id, 'v', var->id, "n");
        text_row(&r, id, "Name", var->name, "", fixed);
        row_id(id, 'v', var->id, "t");
        put_type(
            &r,
            gd_next_row(&r, &(gd_row){ .id = id, .label = "Type", .control = "type", .readonly = fixed }),
            &pin
        );
        row_id(id, 'v', var->id, "c");
        yyjson_mut_val *row = gd_next_row(
            &r,
            &(gd_row){ .id = id, .label = "Container", .control = "container", .readonly = fixed }
        );
        gd_put_int(&r, row, "container", pin.container);
        row_id(id, 'v', var->id, "g");
        text_row(&r, id, "Category", var->category ? var->category : "", "Default", fixed);
    }
    gd_panel_end(&r);
    gd_rows d = gd_panel(v, "default", "Default Value");
    if (d.open) {
        row_id(id, 'v', var->id, "d");
        if (value_editable(&pin)) {
            gd_row row = { .id = id,
                           .label = "Value",
                           .control = default_control(&pin),
                           .readonly = fixed };
            put_default(&d, gd_next_row(&d, &row), &pin, &var->default_value);
        } else {
            label_row(&d, id, "Value", sm_type_label(&pin, type, sizeof type));
        }
    }
    gd_panel_end(&d);
}

/* ---------- Functions and macros ---------- */

static node *signature_node(const graph *g, bool result)
{ return graph_node_by_id(g, result ? g->return_id : g->entry_id); }

bool sm_move_signature_pin(editor *e, graph *g, bool result, uint64_t dragged, int to)
{
    int output, from = -1;
    graph_pin_by_id(signature_node(g, result), dragged, &output, &from);
    if (from < 0 || from == to) { return false; }
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, graph_signature_move(e->asset, g, result, from, to) == 0);
}

/* One input or output: a grip that drags it onto another row of its side
   (dnd-kit), its name, a remove button, its type and container, and an
   input's default below. */
static void signature_row(gd_rows *r, studio *s, graph *g, bool result, const pin_spec *pin)
{
    bool fixed = locked(s);
    char id[GD_ROW_ID], part[32];
    snprintf(part, sizeof part, "p%" PRIu64, pin->id);
    row_id(id, 'g', g->graph_id, part);
    bool defaults = !result && value_editable(pin);
    gd_row desc = { .id = id,
                    .control = "param",
                    .def = defaults ? default_control(pin) : "",
                    .readonly = fixed,
                    .fixed = pin->kind == PIN_EXEC };
    yyjson_mut_val *row = gd_next_row(r, &desc);
    gd_put_text(r, row, "name", pin->name ? pin->name : "");
    gd_put_text(r, row, "drop", fixed ? "" : result ? "result" : "parameter");
    put_type(r, row, pin);
    gd_put_int(r, row, "container", pin->container);
    if (defaults) { put_default(r, row, pin, &pin->default_value); }
}

static void signature_panel(gd_view *v, studio *s, graph *g, bool result)
{
    gd_rows r = gd_panel(v, result ? "outputs" : "inputs", result ? "Outputs" : "Inputs");
    if (r.open) {
        node *owner = signature_node(g, result);
        int count = result ? owner->input_count : owner->output_count;
        for (int p = 0; p < count; p++) {
            const pin_spec *pin = result ? &owner->input_pins[p]
                                         : &owner->output_pins[p];
            if (graph_signature_declared(g, pin)) {
                signature_row(&r, s, g, result, pin);
            }
        }
        char id[GD_ROW_ID];
        row_id(id, 'g', g->graph_id, result ? "outputs" : "inputs");
        gd_panel_add(&r, id, result ? "Add Output" : "Add Input", locked(s));
    }
    gd_panel_end(&r);
}

static void graph_details(gd_view *v, studio *s, graph *g)
{
    editor *e = member_editor(s);
    bool function = g->kind == GRAPH_FUNCTION;
    bool pure = function && graph_signature_pure(g);
    char id[GD_ROW_ID];
    gd_identity(
        v,
        function ? KIT_ICON_FUNCTION : KIT_ICON_EXPRESSION,
        g->name,
        function ? (pure ? "Pure Function" : "Function") : "Macro"
    );
    gd_rows r = gd_panel(v, "graph", function ? "Function" : "Macro");
    if (r.open) {
        row_id(id, 'g', g->graph_id, "n");
        text_row(&r, id, "Name", g->name, "", locked(s));
        if (function) {
            /* Shader and field functions compute values only. */
            bool fixed = locked(s) || behavior_domain_pure(e->asset->domain);
            row_id(id, 'g', g->graph_id, "u");
            yyjson_mut_val *row = gd_next_row(
                &r,
                &(gd_row){ .id = id, .label = "Pure", .control = "bool", .readonly = fixed }
            );
            gd_put_flag(&r, row, "checked", pure);
            gd_put_text(&r, row, "check", "");
        }
    }
    gd_panel_end(&r);
    signature_panel(v, s, g, false);
    signature_panel(v, s, g, true);
}

/* ---------- Named values ---------- */

static int local_uses(const graph *g, uint64_t id)
{
    int uses = 0;
    for (int i = 0; i < acount(g->nodes); i++) {
        for (int p = 0; p < g->nodes[i]->input_count; p++) {
            const pin_binding *b = g->nodes[i]->input_pins[p].binding;
            uses += b && b->kind == PIN_BINDING_LOCAL && b->local_id == id;
        }
    }
    return uses;
}

static void local_details(gd_view *v, studio *s, const graph_local *local)
{
    editor *e = member_editor(s);
    graph *gr = sg_active_graph(s);
    node *source = graph_node_by_id(gr, local->source_id);
    int output = 0, index = -1;
    const pin_spec *pin = graph_pin_by_id(source, local->source_pin_id, &output, &index);
    char id[GD_ROW_ID], text[320], title[160], pin_label[128];
    gd_identity(v, KIT_ICON_LOCAL, local->name, "Named Value");
    gd_rows r = gd_panel(v, "value", "Named Value");
    if (r.open) {
        row_id(id, 'l', local->id, "n");
        text_row(&r, id, "Name", local->name, "", sg_graph_readonly(s));
        studio_code_pin_label(s, source, pin, pin_label, sizeof pin_label);
        snprintf(text, sizeof text, "%s \xe2\x80\xba %s", ed_node_title(e, source, title, sizeof title), pin_label);
        row_id(id, 'l', local->id, "s");
        label_row(&r, id, "Source", text);
        row_id(id, 'l', local->id, "t");
        label_row(&r, id, "Type", sm_type_label(pin, text, sizeof text));
        snprintf(text, sizeof text, "%d", local_uses(gr, local->id));
        row_id(id, 'l', local->id, "u");
        label_row(&r, id, "Uses", text);
    }
    gd_panel_end(&r);
}

void studio_member_details(studio *s, gd_view *v)
{
    if (s->members.kind == STUDIO_MEMBER_VARIABLE &&
        sm_source_variable(s, s->members.id)) {
        source_variable_details(v, s, sm_source_variable(s, s->members.id));
        return;
    }
    const behavior_variable *var = sm_variable(s);
    graph *g = sm_graph(s);
    const graph_local *local = sm_local(s);
    if (var) {
        variable_details(v, s, var);
    } else if (g) {
        graph_details(v, s, g);
    } else if (local) {
        local_details(v, s, local);
    } else {
        gd_empty(v, "No member selected");
    }
}

/* ---------- Capabilities ----------
 * What the tab's edits do to the behavior, over members, pins and values:
 * no row ids, events or JSON. Each is one undo step; false: refused. */

/* A function's or macro's pin, and its side. */
static const pin_spec *signature_pin(graph *g, uint64_t pin_id, bool *result, int *index)
{
    for (int side = 0; g && side < 2; side++) {
        int output = 0;
        const pin_spec *pin = graph_pin_by_id(signature_node(g, side), pin_id, &output, index);
        if (pin) {
            *result = side;
            return pin;
        }
    }
    return NULL;
}

static bool retype_variable(studio *s, uint64_t id, pin_type type, const char *type_name)
{
    editor *e = member_editor(s);
    const source_presentation_origin *source = sm_source_variable(s, id);
    if (source) {
        const char *name = type == PIN_HANDLE ? "entity"
                         : type == PIN_STRUCT || type == PIN_ENUM
                             ? type_name
                             : script_scalar_name(type);
        const char *format;
        switch (source->type.container) {
            case PIN_CONTAINER_SINGLE: format = "%s"; break;
            case PIN_CONTAINER_ARRAY: format = "%s[]"; break;
            case PIN_CONTAINER_SET: format = "set<%s>"; break;
            case PIN_CONTAINER_MAP: format = "map<%s>"; break;
            default: assert(false); return false;
        }
        size_t bytes = strlen(name) + 6;
        char *signature = xmalloc(bytes);
        snprintf(signature, bytes, format, name);
        authoring_edit_request request = {
            .kind = AUTHORING_EDIT_RETYPE_VARIABLE,
            .generation = authoring_source_edit_generation(editor_presentation(e)),
            .origin = source->identity,
            .signature = signature,
        };
        bool changed = ed_source_edit(e, &request);
        free(signature);
        return changed;
    }
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, behavior_asset_retype_variable(e->asset, id, type, type_name) == 0);
}

static bool set_variable_container(editor *e, uint64_t id, pin_container container)
{
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, behavior_asset_set_variable_container(e->asset, id, container) == 0);
}

static bool set_variable_category(editor *e, uint64_t id, const char *category)
{
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, behavior_asset_set_variable_category(e->asset, id, category) == 0);
}

/* Sets variable `variable`'s default or, with `g`, its input `index`'s: a
   scrub (live, then its commit) is one undo step. */
static void set_default(studio *s, graph *g, int index, uint64_t variable, const pin_value *value, sg_step step)
{
    studio_members *m = &s->members;
    editor *e = member_editor(s);
    if (step == SG_STEP_ONCE || !m->value_editing) { ed_snapshot_before(e); }
    m->value_editing = step == SG_STEP_LIVE;
    if (g) {
        graph_signature_set_default(e->asset, g, index, value);
        return;
    }
    behavior_asset_set_variable_default(e->asset, variable, value);
}

/* Adds a Float input or output, named past the names in use. */
static bool add_signature_pin(editor *e, graph *g, bool result)
{
    node *owner = signature_node(g, result);
    const char *base = result ? "Result" : "Value";
    char name[64];
    snprintf(name, sizeof name, "%s", base);
    for (int i = 2;; i++) {
        bool taken = false;
        int count = result ? owner->input_count : owner->output_count;
        const pin_spec *pins = result ? owner->input_pins : owner->output_pins;
        for (int p = 0; p < count && !taken; p++) {
            taken = pins[p].name && !strcmp(pins[p].name, name);
        }
        if (!taken) { break; }
        snprintf(name, sizeof name, "%s %d", base, i);
    }
    pin_spec pin = { .name = name, .kind = PIN_DATA, .type = PIN_F32, .type_name = "float" };
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, graph_signature_add(e->asset, g, result, &pin) == 0);
}

/* Renames an input or output, and what calls in other assets name it; an
   empty or unchanged name changes nothing. */
static bool rename_signature_pin(studio *s, graph *g, uint64_t pin_id, const char *name)
{
    bool result = false;
    int index = -1;
    const pin_spec *pin = signature_pin(g, pin_id, &result, &index);
    if (!pin || !name[0] || !strcmp(name, pin->name)) { return true; }
    reference_symbol parameter = { .kind = REFERENCE_SYMBOL_PARAMETER,
                                   .asset = studio_refresh_document_asset(s),
                                   .graph_id = g->graph_id,
                                   .id = pin_id };
    if (studio_refactor_member_rename(s, parameter, pin->name, name)) {
        return true;
    }
    editor *e = member_editor(s);
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, graph_signature_rename(e->asset, g, result, index, name) == 0);
}

/* Gives an input or output `with`'s kind, type and container. */
static bool retype_signature_pin(editor *e, graph *g, uint64_t pin_id, const pin_spec *with)
{
    bool result = false;
    int index = -1;
    if (!signature_pin(g, pin_id, &result, &index)) { return false; }
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, graph_signature_retype(e->asset, g, result, index, with) == 0);
}

static bool remove_signature_pin(editor *e, graph *g, uint64_t pin_id)
{
    bool result = false;
    int index = -1;
    if (!signature_pin(g, pin_id, &result, &index)) { return false; }
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, graph_signature_remove(e->asset, g, result, index) == 0);
}

static bool set_pure(editor *e, graph *g, bool pure)
{
    if (pure == graph_signature_pure(g)) { return true; }
    behavior_asset *before = ed_snapshot(e);
    return sm_record(e, before, graph_signature_set_pure(e->asset, g, pure) == 0);
}

/* ---------- The type picker ---------- */

/* Exec for a macro's pins, basic types, then registered structures and
   enums. */
static type_choice *type_choices(bool exec)
{
    type_choice *choices = NULL;
    if (exec) { apush(choices, EXEC); }
    for (int i = 0; i < TYPE_COUNT; i++) { apush(choices, TYPES[i]); }
    for (int i = 0; i < type_catalog_count(); i++) {
        const type_desc *type = type_catalog_at(i);
        apush(choices, ((type_choice){ type->name, PIN_STRUCT, type->name }));
    }
    for (int i = 0; i < type_catalog_enum_count(); i++) {
        const enum_desc *type = type_catalog_enum_at(i);
        apush(choices, ((type_choice){ type->name, PIN_ENUM, type->name }));
    }
    return choices;
}

/* The graph whose pin the picker types, or NULL for a variable. */
static graph *picked_graph(studio *s)
{
    studio_members *m = &s->members;
    return m->pick.pin_id
             ? behavior_asset_find_graph(member_editor(s)->asset, m->pick.id)
             : NULL;
}

void studio_member_type_entries(studio *s, studio_palette_entry **entries)
{
    studio_members *m = &s->members;
    graph *g = picked_graph(s);
    bool exec = g && g->kind == GRAPH_MACRO, result = false;
    int index = -1;
    const pin_spec *pin = g ? signature_pin(g, m->pick.pin_id, &result, &index) : NULL;
    const source_presentation_origin *source = !m->pick.pin_id
                                                 ? sm_source_variable(s, m->pick.id)
                                                 : NULL;
    if (source) { pin = &sm_source_variable_node(s, source)->output_pins[0]; }
    const behavior_variable *variable = m->pick.pin_id
                                          ? NULL
                                          : behavior_asset_find_variable_by_id(
                                                member_editor(s)->asset,
                                                m->pick.id
                                            );
    type_choice *choices = type_choices(exec);
    for (int i = 0; i < acount(choices); i++) {
        const type_choice *choice = &choices[i];
        const char *group = i < TYPE_COUNT + exec      ? "Types"
                          : choice->type == PIN_STRUCT ? "Structures"
                                                       : "Enums";
        studio_palette_entry *entry = studio_palette_add(entries, group, choice->label);
        entry->icon = choice->type == PIN_FLOW   ? KIT_ICON_PLAY
                    : choice->type == PIN_STRUCT ? KIT_ICON_COMPONENT
                    : choice->type == PIN_ENUM   ? KIT_ICON_FIELD
                                                 : KIT_ICON_VARIABLE;
        entry->current = pin ? same_type(pin->type, pin->type_name, choice)
                             : variable &&
                                   same_type(variable->type, variable->type_name, choice);
        entry->kind = i;
    }
    afree(choices);
}

void studio_member_type_choose(studio *s, const studio_palette_entry *entry)
{
    editor *e = member_editor(s);
    studio_members *m = &s->members;
    graph *g = picked_graph(s);
    type_choice *choices = type_choices(g && g->kind == GRAPH_MACRO);
    if (entry->kind < 0 || entry->kind >= acount(choices) || !studio_graph_active(s)) {
        afree(choices);
        return;
    }
    const type_choice *choice = &choices[entry->kind];
    bool result = false;
    int index = -1;
    const pin_spec *pin = signature_pin(g, m->pick.pin_id, &result, &index);
    if (!m->pick.pin_id) {
        retype_variable(s, m->pick.id, choice->type, choice->type_name);
    } else if (pin) {
        pin_spec type = { .kind = choice->type == PIN_FLOW ? PIN_EXEC : PIN_DATA,
                          .type = choice->type,
                          .type_name = choice->type_name,
                          .container = pin->container };
        retype_signature_pin(e, g, m->pick.pin_id, &type);
    }
    afree(choices);
    gd_changed(s);
}

/* The type button opens the picker under itself, or closes it, as a select
   does. */
static void pick_type(studio *s, uint64_t id, uint64_t pin_id, const ui_activation *a)
{
    rml_element *button = a->anchor ? rml_element_ref_get(a->anchor) : NULL;
    if (!button || locked(s)) { return; }
    if (s->palette.client && studio_palette_anchor(s) == button) {
        studio_palette_close(s);
        return;
    }
    s->members.pick.id = id;
    s->members.pick.pin_id = pin_id;
    studio_palette_open_at(s, STUDIO_PALETTE_TYPES, "Search types", button);
}

/* ---------- Intents ----------
 * The rows' events read as the capabilities above. */

/* A refused edit's field shows the member's text again. */
static void refused(studio *s, bool ok, const char *title)
{
    if (!ok) {
        sm_refused(s, title);
        s->graph.details.redraw = true;
    }
}

/* The value a default's control sent, as `pin`'s type reads it. */
static bool sent_value(
    const pin_spec *pin,
    const pin_value *was,
    const ui_activation *a,
    arena *scratch,
    pin_value *out
)
{
    const char *text = a->argc > 1 ? a->args[1] : "";
    *out = *was;
    if (pin->type == PIN_ENUM) {
        const editor_choices *choices = editor_choices_for_enum(pin->type_name);
        int index = atoi(text);
        if (!choices || index < 0 || index >= editor_choices_count(choices)) {
            return false;
        }
        out->e = editor_choice_at(choices, index).value;
        return true;
    }
    if (pin->type == PIN_BOOL) {
        /* RmlUi sends change as a binding sets the box too. */
        out->b = text[0] != '\0';
        return out->b != was->b;
    }
    if (pin_vector_width(pin) > 0) {
        const char *axis = a->argc > 3 ? a->args[3] : "";
        const char *key = axis[0] ? strchr("xyzw", axis[0]) : NULL;
        int at = key ? (int)(key - "xyzw") : -1;
        if (at < 0 || at >= pin_vector_width(pin)) { return false; }
        float *vector = arena_alloc(scratch, 4 * sizeof *vector);
        memset(vector, 0, 4 * sizeof *vector);
        if (was->ptr) {
            memcpy(vector, was->ptr, (size_t)pin_vector_width(pin) * sizeof *vector);
        }
        vector[at] = strtof(text, NULL);
        out->ptr = vector;
        return true;
    }
    if (pin->type == PIN_STR || pin->type == PIN_NAME) {
        if (pin->type == PIN_STR) {
            if (pin_value_text(scratch, out, text, strlen(text))) {
                return false;
            }
            out->type_name = was->type_name;
            return true;
        }
        size_t length = strlen(text) + 1;
        char *copy = arena_alloc(scratch, length);
        memcpy(copy, text, length);
        out->str = copy;
        return true;
    }
    char *end;
    double number = strtod(text, &end);
    if (end == text) { return false; }
    sg_set_literal_number(out, number);
    return true;
}

/* A default's edit: a variable's or an input's. Numbers and vectors scrub,
   so input runs a gesture and change ends it. */
static void edit_default(
    studio *s,
    const pin_spec *pin,
    const pin_value *was,
    const char *action,
    const ui_activation *a,
    graph *g,
    int index,
    uint64_t variable
)
{
    arena scratch = { 0 };
    pin_value value;
    if (sent_value(pin, was, a, &scratch, &value)) {
        bool scrubs = (pin->type >= PIN_I8 && pin->type <= PIN_F64) ||
                      pin_vector_width(pin) > 0;
        sg_step step = !scrubs                  ? SG_STEP_ONCE
                     : !strcmp(action, "input") ? SG_STEP_LIVE
                                                : SG_STEP_COMMIT;
        set_default(s, g, index, variable, &value, step);
    }
    arena_free(&scratch);
}

static void variable_edit(
    studio *s,
    behavior_variable *var,
    const char *part,
    const char *action,
    const ui_activation *a
)
{
    editor *e = member_editor(s);
    const char *text = a->argc > 1 ? a->args[1] : "";
    bool change = !strcmp(action, "change");
    if (!strcmp(part, "n") && change) {
        refused(s, sm_rename(s, STUDIO_MEMBER_VARIABLE, var->id, text), sm_name_taken(STUDIO_MEMBER_VARIABLE));
    } else if (!strcmp(part, "t") && !strcmp(action, "type")) {
        pick_type(s, var->id, 0, a);
    } else if (!strcmp(part, "c") && change) {
        set_variable_container(e, var->id, (pin_container)atoi(text));
    } else if (!strcmp(part, "g") && change) {
        set_variable_category(e, var->id, text);
    } else if (!strcmp(part, "d")) {
        pin_spec pin = sm_variable_pin(var);
        edit_default(s, &pin, &var->default_value, action, a, NULL, 0, var->id);
    }
}

static void signature_edit(studio *s, graph *g, uint64_t pin_id, const char *action, const ui_activation *a)
{
    editor *e = member_editor(s);
    bool result = false;
    int index = -1;
    const pin_spec *pin = signature_pin(g, pin_id, &result, &index);
    if (!pin) { return; }
    const char *text = a->argc > 1 ? a->args[1] : "";
    if (!strcmp(action, "type")) {
        pick_type(s, g->graph_id, pin_id, a);
    } else if (!strcmp(action, "reorder")) {
        /* The dragged row's id names its pin. */
        const char *dragged = strstr(text, ".p");
        if (dragged) {
            sm_move_signature_pin(e, g, result, strtoull(dragged + 2, NULL, 10), index);
        }
    } else if (!strcmp(action, "rename")) {
        refused(s, rename_signature_pin(s, g, pin_id, text), "A pin has that name");
    } else if (!strcmp(action, "container")) {
        pin_spec type = { .kind = PIN_DATA,
                          .type = pin->type,
                          .type_name = pin->type_name,
                          .container = (pin_container)atoi(text) };
        retype_signature_pin(e, g, pin_id, &type);
    } else if (!strcmp(action, "remove")) {
        remove_signature_pin(e, g, pin_id);
    } else if (!result) {
        edit_default(s, pin, &pin->default_value, action, a, g, index, 0);
    }
}

static void graph_edit(studio *s, graph *g, const char *part, const char *action, const ui_activation *a)
{
    const char *text = a->argc > 1 ? a->args[1] : "";
    if (part[0] == 'p') {
        signature_edit(s, g, strtoull(part + 1, NULL, 10), action, a);
    } else if (!strcmp(part, "n") && !strcmp(action, "change")) {
        refused(s, sm_rename(s, STUDIO_MEMBER_GRAPH, g->graph_id, text), sm_name_taken(STUDIO_MEMBER_GRAPH));
    } else if (!strcmp(part, "u") && !strcmp(action, "check")) {
        refused(s, set_pure(member_editor(s), g, text[0] != '\0'), "The function runs nodes in order");
    }
}

void studio_member_details_action(studio *s, const char *action, const ui_activation *a)
{
    const char *id = a->args[0];
    char *dot;
    uint64_t member = strtoull(id + 1, &dot, 10);
    if (*dot != '.' || locked(s)) { return; }
    const char *part = dot + 1;
    if (id[0] == 'v' && s->members.kind == STUDIO_MEMBER_VARIABLE &&
        s->members.id == member && sm_source_variable(s, member)) {
        if (!strcmp(part, "n") && !strcmp(action, "change") && a->argc > 1) {
            refused(
                s,
                sm_rename(s, STUDIO_MEMBER_VARIABLE, member, a->args[1]),
                sm_name_taken(STUDIO_MEMBER_VARIABLE)
            );
        } else if (!strcmp(part, "t") && !strcmp(action, "type")) {
            pick_type(s, member, 0, a);
        }
        return;
    }
    if (!strcmp(action, "addPin")) {
        graph *g = sm_graph(s);
        if (g && g->graph_id == member) {
            add_signature_pin(member_editor(s), g, !strcmp(part, "outputs"));
        }
        return;
    }
    behavior_variable *var = sm_variable(s);
    graph *g = sm_graph(s);
    const graph_local *local = sm_local(s);
    const char *text = a->argc > 1 ? a->args[1] : "";
    if (id[0] == 'v' && var && var->id == member) {
        variable_edit(s, var, part, action, a);
    } else if (id[0] == 'g' && g && g->graph_id == member) {
        graph_edit(s, g, part, action, a);
    } else if (id[0] == 'l' && local && local->id == member && !strcmp(part, "n") && !strcmp(action, "change")) {
        refused(s, sm_rename(s, STUDIO_MEMBER_LOCAL, member, text), sm_name_taken(STUDIO_MEMBER_LOCAL));
    }
}
