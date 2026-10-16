#include "graph/authoring/graph_edit.h"
/* Typed literal controls for behavior and shader graph pins. */
#include "authoring/reference_index.h"
#include "editor/collab/collab.h"
#include "editor/studio/studio_graph_internal.h"
#include "editor/ui/choices.h"
#include "editor/ui/labels.h"
#include "graph/model/local.h"
#include "graph/model/pin_conv.h"
#include "graph/model/registry.h"
#include "types/type.h"
#include "types/array.h"
#include "graph/shader/shader_graph.h"
#include "support/arena.h"
#include "support/array.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Streaming edits must reach the preview on the next tick. */
static void touched(editor *e)
{
    e->edit_pending = 1;
}

typedef struct
{
    bool changed;
    bool streaming;
    bool committed;
    bool active;
} literal_edit;

typedef struct
{
    double min, max;
    bool valid;
} literal_range;

static void merge_edit(literal_edit *into, literal_edit from)
{
    into->changed |= from.changed;
    into->streaming |= from.streaming;
    into->committed |= from.committed;
    into->active |= from.active;
}

static bool value_edit_matches(const studio_graph *g, const node *n, int output, uint64_t pin_id, const kit *k)
{
    return g->value_editing && g->value_edit_node_id == n->id &&
           g->value_edit_pin_id == pin_id && g->value_edit_output == output &&
           g->value_edit_kit == k;
}

static void value_edit_set(studio_graph *g, const node *n, int output, uint64_t pin_id, kit *k, bool active)
{
    g->value_editing = active;
    g->value_edit_node_id = active ? n->id : 0;
    g->value_edit_pin_id = active ? pin_id : 0;
    g->value_edit_output = active ? output : 0;
    g->value_edit_kit = active ? k : NULL;
}

static bool pin_string(pin_type type)
{
    return type == PIN_STR || type == PIN_NAME;
}

/* ---------- The canvas's pickers ---------- */

/* The last control's box in Studio's pixels, where its picker hangs: the
   graph's own context draws into the canvas surface. */
static void canvas_box(const studio *s, kit *k, float box[4])
{
    kit_rect r = kit_item_rect(k);
    kit_rect surface = s->graph.surface.rect;
    float scale = studio_pixel_ratio(s), ratio = studio_graph_pixel_ratio(s);
    box[0] = surface.x * scale + r.x * ratio;
    box[1] = surface.y * scale + r.y * ratio;
    box[2] = r.w * ratio;
    box[3] = r.h * ratio;
}

/* Whether a click on canvas control `key` opens its picker: a click while
   it is open, or on the press that just closed it from outside the picker,
   shuts it, as a select does. */
static bool picker_opens(studio *s, const char *key, bool clicked)
{
    studio_graph *g = &s->graph;
    bool mine = !strcmp(g->picker, key);
    bool open = mine && s->palette.client && s->palette.boxed;
    if (!clicked) {
        if (mine && !open) {
            g->picker[0] = '\0';
        }
        return false;
    }
    if (mine) {
        if (open) {
            studio_palette_close(s);
        }
        g->picker[0] = '\0';
        return false;
    }
    snprintf(g->picker, sizeof g->picker, "%s", key);
    return true;
}

static bool picker_open(const studio *s, const char *key)
{
    return !strcmp(s->graph.picker, key) && s->palette.client && s->palette.boxed;
}

static const char *graph_asset_field(
    studio *s,
    kit *k,
    const char *key,
    const char *value,
    reference_asset_kind kind,
    unsigned flags
)
{
    const reference_index *index = app_scene_authoring_asset_index(s->scene);
    bool named = value && value[0] && index && kind != REFERENCE_ASSET_EVENT;
    const reference_asset *found = named
                                     ? reference_index_asset_at(
                                           index,
                                           reference_index_find_name(index, kind, value)
                                       )
                                     : NULL;
    bool unresolved = named && (!found || found->unresolved);
    char tip[192] = "", pick[192];
    if (unresolved) {
        snprintf(tip, sizeof tip, "No %s asset is named %s", studio_asset_kind_name(kind), value);
    }
    kit_picker_desc picker = { .value = value,
                               .placeholder = "None",
                               .icon = studio_asset_kind_icon(kind),
                               .tip = tip[0] ? tip : NULL,
                               .flags = flags | (unresolved ? KIT_INVALID : 0) };
    snprintf(pick, sizeof pick, "graph:%s-assets", key);
    bool clicked = kit_picker(k, "asset", &picker) && !(flags & KIT_READONLY);
    kit_flag(k, "data-open", picker_open(s, pick));
    if (picker_opens(s, pick, clicked)) {
        float box[4];
        canvas_box(s, k, box);
        studio_pick_open_box(
            s,
            pick,
            value,
            &(studio_pick_desc){ .kind = kind, .none = "None", .text = true },
            box
        );
    }
    if (!s->pick.chosen || strcmp(s->pick.key, pick)) {
        return NULL;
    }
    s->pick.chosen = false;
    return s->pick.value;
}

/* A choice's picker lists every option, its value's current. */
void studio_graph_choice_entries(studio *s, studio_palette_entry **entries)
{
    const editor_choices *choices = s->graph.choice.choices;
    for (int i = 0; choices && i < editor_choices_count(choices); i++) {
        editor_choice choice = editor_choice_at(choices, i);
        studio_palette_entry *entry = studio_palette_add(
            entries,
            choice.group ? choice.group : "",
            choice.label ? choice.label : choice.name
        );
        snprintf(
            entry->keywords,
            sizeof entry->keywords,
            "%s",
            choice.name ? choice.name : ""
        );
        entry->icon = choice.icon;
        entry->current = i == s->graph.choice.current;
        entry->kind = i;
    }
}

void studio_graph_choice_choose(studio *s, const studio_palette_entry *entry)
{
    s->graph.choice.chosen = entry->kind;
    s->graph.choice.taken = true;
}

/* The place of the option chosen for canvas control `key` this frame, or
   -1; a click on the control opens its picker under it. */
static int choice_pick(
    studio *s,
    kit *k,
    const char *key,
    bool clicked,
    const editor_choices *choices,
    int current,
    const char *placeholder
)
{
    studio_graph *g = &s->graph;
    if (picker_opens(s, key, clicked)) {
        snprintf(g->choice.key, sizeof g->choice.key, "%s", key);
        g->choice.choices = choices;
        g->choice.current = current;
        g->choice.taken = false;
        float box[4];
        canvas_box(s, k, box);
        studio_palette_open_box(s, STUDIO_PALETTE_CHOICES, placeholder, box);
    }
    if (!g->choice.taken || strcmp(g->choice.key, key)) {
        return -1;
    }
    g->choice.taken = false;
    return g->choice.chosen;
}

double sg_literal_number(const pin_value *value)
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

void sg_set_literal_number(pin_value *value, double number)
{
    switch (value->type) {
        case PIN_I8: value->i8 = (int8_t)number; break;
        case PIN_U8: value->u8 = (uint8_t)number; break;
        case PIN_I16: value->i16 = (int16_t)number; break;
        case PIN_U16: value->u16 = (uint16_t)number; break;
        case PIN_I32: value->i = (int32_t)number; break;
        case PIN_U32: value->u = (uint32_t)number; break;
        case PIN_I64: value->i64 = (int64_t)number; break;
        case PIN_U64: value->u64 = (uint64_t)number; break;
        case PIN_F32: value->f = (float)number; break;
        case PIN_F64: value->d = number; break;
        default: break;
    }
}

static bool literal_integer(pin_type type)
{
    return type >= PIN_I8 && type <= PIN_U64;
}

static bool literal_integer_bounds(pin_type type, double *min, double *max)
{
    switch (type) {
        case PIN_I8:
            *min = INT8_MIN;
            *max = INT8_MAX;
            return true;
        case PIN_U8:
            *min = 0;
            *max = UINT8_MAX;
            return true;
        case PIN_I16:
            *min = INT16_MIN;
            *max = INT16_MAX;
            return true;
        case PIN_U16:
            *min = 0;
            *max = UINT16_MAX;
            return true;
        case PIN_I32:
            *min = INT32_MIN;
            *max = INT32_MAX;
            return true;
        case PIN_U32:
            *min = 0;
            *max = UINT32_MAX;
            return true;
        default: return false;
    }
}

void sg_literal_bounds(pin_type type, double *min, double *max)
{
    if (literal_integer_bounds(type, min, max)) {
        return;
    }
    if (type == PIN_F32) {
        *min = -FLT_MAX;
        *max = FLT_MAX;
    }
}

static bool parse_i64(const char *text, int64_t *value)
{
    if (!text || !text[0]) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    intmax_t parsed = strtoimax(text, &end, 10);
    if (errno == ERANGE || end == text || *end || parsed < INT64_MIN ||
        parsed > INT64_MAX) {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    if (!text || !text[0]) {
        return false;
    }
    const char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (*start == '-') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || *end || parsed > UINT64_MAX) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool edit_integer64(
    kit *k,
    const char *key,
    pin_value *value,
    unsigned flags,
    bool *streaming,
    bool *active,
    bool *committed
)
{
    char text[32];
    if (value->type == PIN_I64) {
        snprintf(text, sizeof text, "%" PRId64, value->i64);
    } else {
        assert(value->type == PIN_U64);
        snprintf(text, sizeof text, "%" PRIu64, value->u64);
    }
    bool entered = kit_text_field(k, key, text, sizeof text, NULL, flags);
    *committed = kit_item_committed(k);
    *active = kit_item_active(k) && !*committed;
    *streaming = entered || *active;
    if (!entered) {
        return false;
    }
    if (value->type == PIN_I64) {
        int64_t parsed;
        if (!parse_i64(text, &parsed) || parsed == value->i64) {
            return false;
        }
        value->i64 = parsed;
        return true;
    }
    uint64_t parsed;
    if (!parse_u64(text, &parsed) || parsed == value->u64) {
        return false;
    }
    value->u64 = parsed;
    return true;
}

static uint32_t color_channel(const pin_value *value)
{
    double channel = sg_literal_number(value);
    if (value->type == PIN_F32 || value->type == PIN_F64) {
        channel *= 255;
    }
    return (uint32_t)lround(fmin(fmax(channel, 0), 255));
}

uint32_t sg_literal_color(const pin_spec *spec, const pin_value *value)
{
    if (value->type == PIN_U32) {
        return value->u;
    }
    if (value->type == PIN_I32) {
        return (uint32_t)value->i;
    }
    const type_desc *type = type_catalog_find(spec->type_name);
    if (!type || !value->ptr || (type->field_count != 3 && type->field_count != 4)) {
        return 0x000000ffu;
    }
    uint32_t channels[4] = { 0, 0, 0, 255 };
    for (int i = 0; i < type->field_count; i++) {
        pin_value component;
        pin_read_field(value->ptr, &type->fields[i], &component);
        channels[i] = color_channel(&component);
    }
    return (channels[0] << 24) | (channels[1] << 16) | (channels[2] << 8) |
           channels[3];
}

bool sg_write_literal_color(const pin_spec *spec, pin_value *value, arena *scratch, uint32_t rgba)
{
    if (value->type == PIN_U32) {
        value->u = rgba;
        return true;
    }
    if (value->type == PIN_I32) {
        value->i = (int32_t)rgba;
        return true;
    }
    const type_desc *type = type_catalog_find(spec->type_name);
    if (!type || (type->field_count != 3 && type->field_count != 4)) {
        return false;
    }
    if (!value->ptr) {
        value->ptr = arena_alloc(scratch, type->size);
        memset(value->ptr, 0, type->size);
        value->type_name = type->name;
        if (type->field_count == 4) {
            pin_value alpha;
            pin_read_field(value->ptr, &type->fields[3], &alpha);
            sg_set_literal_number(&alpha, alpha.type == PIN_F32 || alpha.type == PIN_F64 ? 1 : 255);
            pin_write_field(value->ptr, &type->fields[3], alpha);
        }
    }
    uint32_t channels[] = { rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255 };
    for (int i = 0; i < 3; i++) {
        const type_field *field = &type->fields[i];
        pin_value previous, component;
        pin_read_field(value->ptr, field, &previous);
        double channel = channels[i];
        if (previous.type == PIN_F32 || previous.type == PIN_F64) {
            channel /= 255;
        }
        component = previous;
        sg_set_literal_number(&component, channel);
        pin_write_field(value->ptr, field, component);
    }
    return true;
}

double sg_literal_alpha(const pin_spec *spec, const pin_value *value)
{
    const type_desc *type = type_catalog_find(spec->type_name);
    if (!type || type->field_count != 4 || !value->ptr) {
        return 1;
    }
    pin_value alpha;
    pin_read_field(value->ptr, &type->fields[3], &alpha);
    double number = sg_literal_number(&alpha);
    if (alpha.type != PIN_F32 && alpha.type != PIN_F64) {
        number /= 255;
    }
    return fmin(fmax(number, 0), 1);
}

void sg_write_literal_alpha(const pin_spec *spec, pin_value *value, arena *scratch, double alpha)
{
    const type_desc *type = type_catalog_find(spec->type_name);
    assert(type && type->field_count == 4);
    if (!value->ptr) {
        value->ptr = arena_alloc(scratch, type->size);
        memset(value->ptr, 0, type->size);
        value->type_name = type->name;
    }
    const type_field *field = &type->fields[3];
    pin_value previous, component;
    pin_read_field(value->ptr, field, &previous);
    if (previous.type != PIN_F32 && previous.type != PIN_F64) {
        alpha *= 255;
    }
    component = previous;
    sg_set_literal_number(&component, alpha);
    pin_write_field(value->ptr, field, component);
}

static bool enum_control(studio *s, kit *k, const char *key, const pin_spec *shape, pin_value *value, unsigned flags)
{
    const editor_choices *choices = editor_choices_for_enum(shape->type_name);
    int count = choices ? editor_choices_count(choices) : 0;
    if (count <= 0) {
        kit_label(k, key, "muted", "Missing enum values");
        return false;
    }
    char pick[192], current[128];
    snprintf(pick, sizeof pick, "graph:%s-options", key);
    const char *label = type_catalog_enum_label(shape->type_name, value->e);
    snprintf(current, sizeof current, "%s", label ? label : "Choose value");
    kit_button_desc button = { .label = current,
                               .variant = KIT_GHOST,
                               .size = KIT_SM,
                               .flags = flags };
    bool clicked = kit_button(k, "enum", &button) && !(flags & KIT_READONLY);
    int at = -1;
    for (int i = 0; i < count; i++) {
        at = editor_choice_at(choices, i).value == value->e ? i : at;
    }
    int selected = choice_pick(s, k, pick, clicked, choices, at, "Choose value");
    if (selected < 0) {
        return false;
    }
    int next = editor_choice_at(choices, selected).value;
    if (value->e == next) {
        return false;
    }
    value->e = next;
    return true;
}

static bool edit_literal_value(
    studio *s,
    kit *k,
    const char *key,
    pin_value *value,
    const pin_spec *shape,
    const type_desc *owner,
    const type_field *field,
    arena *scratch,
    unsigned flags,
    literal_edit *state,
    int depth,
    const literal_range *range
);

static bool edit_struct_fields(
    studio *s,
    kit *k,
    const char *path,
    pin_value *value,
    const type_desc *type,
    arena *scratch,
    unsigned flags,
    literal_edit *state,
    int depth,
    const literal_range *position_range
)
{
    if (depth >= 8 || !type || !type->size) {
        kit_label(k, "unsupported", "muted", "No editable fields");
        return false;
    }
    if (!value->ptr) {
        value->ptr = arena_alloc(scratch, type->size);
        memset(value->ptr, 0, type->size);
        value->type_name = type->name;
    }
    for (int i = 0; i < type->field_count; i++) {
        const type_field *field = &type->fields[i];
        char key[32], control[192], label[128];
        snprintf(key, sizeof key, "field-%d", i);
        snprintf(control, sizeof control, "%s-field-%d", path, i);
        editor_identifier_label(field->name, label, sizeof label);
        pin_value member;
        pin_read_field(value->ptr, field, &member);
        if (!member.type_name) {
            member.type_name = field->type_name;
        }
        pin_spec shape = { .type = field->type,
                           .type_name = field->type_name,
                           .kind = PIN_DATA,
                           .container = PIN_CONTAINER_SINGLE,
                           .flags = !strcmp(type->name, "ShaderRampStop") &&
                                            !strcmp(field->name, "Color")
                                      ? PIN_FLAG_COLOR
                                      : 0 };
        bool ramp_position = !strcmp(type->name, "ShaderRampStop") &&
                             !strcmp(field->name, "Position");
        const literal_range *range = ramp_position ? position_range : NULL;
        unsigned field_flags = flags | (range && !range->valid ? KIT_READONLY : 0);
        kit_prop_begin(k, key, label);
        literal_edit child = { 0 };
        bool changed = edit_literal_value(
            s,
            k,
            control,
            &member,
            &shape,
            type,
            field,
            scratch,
            field_flags,
            &child,
            depth + 1,
            range && range->valid ? range : NULL
        );
        kit_prop_end(k);
        merge_edit(state, child);
        if (changed) {
            pin_write_field(value->ptr, field, member);
            state->changed = true;
        }
    }
    return state->changed;
}

static bool edit_struct_control(
    studio *s,
    kit *k,
    const char *key,
    pin_value *value,
    const pin_spec *shape,
    arena *scratch,
    unsigned flags,
    literal_edit *state,
    int depth,
    const literal_range *position_range
)
{
    const type_desc *type = type_catalog_find(shape->type_name);
    if (!type) {
        kit_label(k, "unsupported", "muted", shape->type_name ? shape->type_name : "Struct");
        return false;
    }
    char label[128];
    editor_identifier_label(type->name, label, sizeof label);
    char popup[192];
    snprintf(popup, sizeof popup, "%s-fields", key);
    kit_button_desc button = { .label = "Edit",
                               .icon = KIT_ICON_COMPONENT,
                               .variant = KIT_GHOST,
                               .size = KIT_SM,
                               .tip = label,
                               .flags = flags };
    if (kit_button(k, "edit", &button) && !(flags & KIT_READONLY)) {
        kit_popup_open(k, popup);
    }
    if (!kit_popup_begin(k, popup, "popover menu struct-editor")) {
        return false;
    }
    kit_label(k, "title", "muted", label);
    literal_edit fields = { 0 };
    edit_struct_fields(s, k, key, value, type, scratch, flags, &fields, depth, position_range);
    merge_edit(state, fields);
    kit_popup_end(k);
    return fields.changed;
}

bool sg_ramp_stops(const pin_spec *shape)
{
    return shape->type == PIN_STRUCT && shape->container == PIN_CONTAINER_ARRAY &&
           shape->type_name && !strcmp(shape->type_name, "ShaderRampStop");
}

static bool ramp_values(const pin_array *array, shader_ramp_stop stops[SHADER_RAMP_MAX])
{
    if (!array || !pin_array_valid(array) || array->element_type != PIN_STRUCT ||
        !array->type_name || strcmp(array->type_name, "ShaderRampStop") ||
        array->count > SHADER_RAMP_MAX) {
        return false;
    }
    for (int i = 0; i < array->count; i++) {
        pin_value item = pin_array_get(array, i);
        if (item.type != PIN_STRUCT || !item.ptr || !item.type_name ||
            strcmp(item.type_name, "ShaderRampStop")) {
            return false;
        }
        memcpy(&stops[i], item.ptr, sizeof stops[i]);
    }
    return shader_ramp_valid(stops, array->count);
}

/* A ramp stop's Position keeps it between its neighbours. */
static void ramp_position_range(const pin_array *array, int index, literal_range *range)
{
    *range = (literal_range){ .min = 0, .max = 1, .valid = true };
    shader_ramp_stop stops[SHADER_RAMP_MAX];
    if (!ramp_values(array, stops) || index < 0 || index >= array->count) {
        return;
    }
    float min = index ? nextafterf(stops[index - 1].position, INFINITY) : 0;
    float max = index + 1 < array->count
                  ? nextafterf(stops[index + 1].position, -INFINITY)
                  : 1;
    range->min = min;
    range->max = max;
    range->valid = min <= max;
}

static bool ramp_insert_value(const pin_array *array, int *at, shader_ramp_stop *stop)
{
    shader_ramp_stop stops[SHADER_RAMP_MAX];
    if (!ramp_values(array, stops) || array->count >= SHADER_RAMP_MAX) {
        return false;
    }
    float largest_gap = 0;
    int insert = -1;
    for (int i = 0; i + 1 < array->count; i++) {
        shader_ramp_stop left = stops[i], right = stops[i + 1];
        float gap = right.position - left.position;
        if (!isfinite(gap) || gap <= largest_gap) {
            continue;
        }
        float position = left.position + gap * 0.5f;
        if (position <= left.position) {
            position = nextafterf(left.position, right.position);
        }
        if (position >= right.position) {
            continue;
        }
        float t = (position - left.position) / gap;
        *stop = (shader_ramp_stop){ .position = position };
        for (int channel = 0; channel < 4; channel++) {
            stop->color[channel] = left.color[channel] +
                                   (right.color[channel] - left.color[channel]) * t;
        }
        largest_gap = gap;
        insert = i + 1;
    }
    if (insert < 0) {
        return false;
    }
    *at = insert;
    return true;
}

bool sg_ramp_range(const pin_array *array, int index, double *min, double *max)
{
    literal_range range;
    ramp_position_range(array, index, &range);
    *min = range.min;
    *max = range.max;
    return range.valid;
}

static pin_array *array_with_count(
    arena *scratch,
    const pin_array *source,
    int removed,
    bool append,
    pin_type element_type,
    const char *type_name,
    int insert_at,
    const pin_value *added
)
{
    int old_count = source ? source->count : 0;
    int count = old_count - (removed >= 0) + append;
    pin_array *copy = pin_array_allocate(scratch, element_type, type_name, count);
    int at = 0;
    for (int i = 0; i < old_count; i++) {
        if (append && insert_at == i) {
            if (pin_value_snapshot_to_arena(scratch, &copy->items[at++], added)) {
                return NULL;
            }
        }
        if (i == removed) {
            continue;
        }
        pin_value item = pin_array_get(source, i);
        if (pin_value_snapshot_to_arena(scratch, &copy->items[at++], &item)) {
            return NULL;
        }
    }
    if (append && insert_at == old_count) {
        if (pin_value_snapshot_to_arena(scratch, &copy->items[at], added)) {
            return NULL;
        }
    }
    return copy;
}

/* Whether an item may be added: a ramp holds SHADER_RAMP_MAX stops at
   most, each between two others. */
bool sg_array_can_add(const pin_value *value, const pin_spec *shape)
{
    if (shape->container != PIN_CONTAINER_ARRAY && shape->container != PIN_CONTAINER_SET) {
        return false;
    }
    const pin_array *array = value->ptr;
    if (!sg_ramp_stops(shape)) {
        return true;
    }
    shader_ramp_stop stop;
    int at;
    return array && array->count < SHADER_RAMP_MAX &&
           ramp_insert_value(array, &at, &stop);
}

pin_array *sg_array_edit(arena *scratch, const pin_value *value, const pin_spec *shape, int remove, bool append)
{
    if (shape->container != PIN_CONTAINER_ARRAY && shape->container != PIN_CONTAINER_SET) {
        return NULL;
    }
    pin_array *array = value->ptr
                         ? value->ptr
                         : pin_array_allocate(scratch, shape->type, shape->type_name, 0);
    bool ramp = sg_ramp_stops(shape);
    shader_ramp_stop stop = { 0 };
    int at = array->count;
    if (append && ramp && !ramp_insert_value(array, &at, &stop)) {
        return NULL;
    }
    pin_value added = { .type = array->element_type,
                        .type_name = array->type_name,
                        .container = PIN_CONTAINER_SINGLE };
    if (append && ramp) {
        added.ptr = &stop;
    } else if (append && added.type == PIN_STRUCT) {
        const type_desc *type = type_catalog_find(added.type_name);
        if (type) {
            added.ptr = arena_alloc(scratch, type->size);
            memset(added.ptr, 0, type->size);
        }
    } else if (append && pin_string(added.type)) {
        added.str = "";
    }
    return array_with_count(scratch, array, remove, append, added.type, added.type_name, at, &added);
}

static bool edit_array_control(
    studio *s,
    kit *k,
    const char *key,
    pin_value *value,
    const pin_spec *shape,
    arena *scratch,
    unsigned flags,
    literal_edit *state,
    int depth
)
{
    pin_array *array = value->ptr;
    int count = array ? array->count : 0;
    char label[64], popup[192];
    snprintf(label, sizeof label, "%d item%s", count, count == 1 ? "" : "s");
    snprintf(popup, sizeof popup, "%s-items", key);
    kit_button_desc button = { .label = label,
                               .icon = KIT_ICON_OUTLINER,
                               .variant = KIT_GHOST,
                               .size = KIT_SM,
                               .flags = flags };
    if (kit_button(k, "edit", &button) && !(flags & KIT_READONLY)) {
        kit_popup_open(k, popup);
    }
    if (!kit_popup_begin(k, popup, "popover menu")) {
        return false;
    }
    if (!array) {
        array = pin_array_allocate(scratch, shape->type, shape->type_name, 0);
    }
    bool ramp = sg_ramp_stops(shape);
    bool can_add = sg_array_can_add(&(pin_value){ .ptr = array }, shape);
    int remove = -1;
    pin_spec item_shape = { .type = array->element_type,
                            .type_name = array->type_name,
                            .kind = PIN_DATA,
                            .container = PIN_CONTAINER_SINGLE };
    for (int i = 0; i < array->count; i++) {
        char item_key[32], item_control[192], item_label[32];
        snprintf(item_key, sizeof item_key, "item-%d", i);
        snprintf(item_control, sizeof item_control, "%s-item-%d", key, i);
        snprintf(item_label, sizeof item_label, "Item %d", i + 1);
        literal_range item_range;
        const literal_range *position_range = NULL;
        if (ramp) {
            ramp_position_range(array, i, &item_range);
            position_range = &item_range;
        }
        kit_prop_begin(k, item_key, item_label);
        literal_edit child = { 0 };
        pin_value item = pin_array_get(array, i);
        bool changed = edit_literal_value(
            s,
            k,
            item_control,
            &item,
            &item_shape,
            NULL,
            NULL,
            scratch,
            flags,
            &child,
            depth + 1,
            position_range
        );
        if (changed) {
            int error = pin_array_set(array, i, &item);
            assert(!error);
            (void)error;
        }
        kit_button_desc remove_button = {
            .icon = KIT_ICON_TRASH,
            .variant = KIT_GHOST,
            .size = KIT_SM,
            .tip = "Remove item",
            .flags = flags | (ramp && count <= 2 ? KIT_DISABLED : 0)
        };
        if (kit_button(k, "remove", &remove_button) && !(flags & KIT_READONLY)) {
            remove = i;
        }
        kit_prop_end(k);
        merge_edit(state, child);
        state->changed |= changed;
    }
    bool append = false;
    kit_button_desc add = { .label = "Add item",
                            .icon = KIT_ICON_ADD,
                            .variant = KIT_SUBTLE,
                            .size = KIT_SM,
                            .flags = flags | (can_add ? 0 : KIT_DISABLED) };
    append = kit_button(k, "add", &add) && !(flags & KIT_READONLY) && can_add;
    if (remove >= 0 && append) {
        append = false;
    }
    if (remove >= 0 || append) {
        pin_array *updated = sg_array_edit(scratch, &(pin_value){ .ptr = array }, shape, remove, append);
        if (updated) {
            array = updated;
            state->changed = true;
        } else {
            kit_label(k, "copy-error", "muted", "Collection could not be updated");
        }
    }
    value->ptr = array;
    kit_popup_end(k);
    return state->changed;
}

static bool edit_literal_value(
    studio *s,
    kit *k,
    const char *key,
    pin_value *value,
    const pin_spec *shape,
    const type_desc *owner,
    const type_field *field,
    arena *scratch,
    unsigned flags,
    literal_edit *state,
    int depth,
    const literal_range *range
)
{
    if (shape->container == PIN_CONTAINER_MAP) {
        kit_label(k, key, "muted", "Map values are not editable yet");
        return false;
    }
    if (shape->container >= PIN_CONTAINER_ARRAY) {
        return edit_array_control(s, k, key, value, shape, scratch, flags, state, depth);
    }
    if ((shape->flags & PIN_FLAG_COLOR) &&
        (shape->type == PIN_U32 || shape->type == PIN_I32 || shape->type == PIN_STRUCT)) {
        const type_desc *type = shape->type == PIN_STRUCT
                                  ? type_catalog_find(shape->type_name)
                                  : NULL;
        bool has_alpha = type && type->field_count == 4;
        if (has_alpha) {
            kit_push(k, "rgba", NULL, "color-alpha-row");
        }
        uint32_t color = sg_literal_color(shape, value);
        bool color_changed = kit_color(k, has_alpha ? "swatch" : key, &color, flags);
        bool changed = color_changed;
        bool committed = kit_item_committed(k);
        bool active = kit_item_active(k) && !committed;
        merge_edit(
            state,
            (literal_edit){ .changed = color_changed,
                            .streaming = color_changed || active,
                            .committed = committed,
                            .active = active }
        );
        if (has_alpha) {
            double alpha = sg_literal_alpha(shape, value);
            kit_number_desc desc = { .min = 0,
                                     .max = 1,
                                     .step = 0.01,
                                     .precision = 3,
                                     .addon = "Alpha",
                                     .flags = flags };
            bool alpha_changed = kit_number(k, "alpha", &alpha, &desc);
            committed = kit_item_committed(k);
            active = kit_item_active(k) && !committed;
            merge_edit(
                state,
                (literal_edit){ .changed = alpha_changed,
                                .streaming = alpha_changed || active,
                                .committed = committed,
                                .active = active }
            );
            if (alpha_changed) {
                changed = true;
                sg_write_literal_alpha(shape, value, scratch, alpha);
            }
            kit_pop(k);
        }
        if (color_changed) {
            sg_write_literal_color(shape, value, scratch, color);
        }
        return changed;
    }
    if (shape->type == PIN_STRUCT) {
        int width = pin_vector_width(shape);
        if (width > 0) {
            const type_desc *type = type_catalog_find(shape->type_name);
            if (!type || !type->size) {
                kit_label(k, key, "muted", shape->type_name);
                return false;
            }
            if (!value->ptr) {
                value->ptr = arena_alloc(scratch, type->size);
                memset(value->ptr, 0, type->size);
                value->type_name = type->name;
            }
            static const char *const axes[] = { "X", "Y", "Z", "W" };
            bool changed = false;
            kit_push(k, "vector", NULL, "joined");
            for (int i = 0; i < width; i++) {
                pin_value component;
                pin_read_field(value->ptr, &type->fields[i], &component);
                double number = sg_literal_number(&component);
                double min = 0, max = 0;
                sg_literal_bounds(component.type, &min, &max);
                kit_number_desc desc = { .precision = 2,
                                         .single_precision = component.type == PIN_F32,
                                         .min = min,
                                         .max = max,
                                         .step = 0.1,
                                         .addon = axes[i],
                                         .addon_cls = i == 0 ? "x"
                                                    : i == 1 ? "y"
                                                             : NULL,
                                         .flags = flags };
                bool edited = kit_number(k, axes[i], &number, &desc);
                bool committed = kit_item_committed(k);
                bool active = kit_item_active(k) && !committed;
                state->streaming |= edited || active;
                state->active |= active;
                state->committed |= committed;
                if (edited) {
                    sg_set_literal_number(&component, number);
                    pin_write_field(value->ptr, &type->fields[i], component);
                    changed = true;
                }
            }
            kit_pop(k);
            state->changed |= changed;
            return changed;
        }
        return edit_struct_control(s, k, key, value, shape, scratch, flags, state, depth, range);
    }
    if (shape->type == PIN_ENUM) {
        bool changed = enum_control(s, k, key, shape, value, flags);
        state->changed |= changed;
        return changed;
    }
    if (shape->type == PIN_BOOL) {
        bool boolean = value->b;
        bool changed = kit_checkbox(k, key, &boolean, NULL, flags);
        if (changed) {
            value->b = boolean;
        }
        state->changed |= changed;
        return changed;
    }
    if (pin_string(shape->type)) {
        reference_asset_kind
            asset = owner && field
                      ? reference_named_field_kind(owner->name, field->name)
                      : REFERENCE_ASSET_KIND_COUNT;
        if (asset != REFERENCE_ASSET_KIND_COUNT) {
            const char *chosen = graph_asset_field(s, k, key, value->str, asset, flags);
            if (!chosen) {
                return false;
            }
            size_t length = strlen(chosen) + 1;
            char *copy = arena_alloc(scratch, length);
            memcpy(copy, chosen, length);
            value->str = copy;
            state->changed = true;
            return true;
        }
        char text[256];
        snprintf(text, sizeof text, "%s", value->str ? value->str : "");
        bool changed = kit_text_field(k, key, text, sizeof text, NULL, flags);
        bool committed = kit_item_committed(k);
        bool active = kit_item_active(k) && !committed;
        state->changed |= changed;
        state->streaming |= changed || active;
        state->active |= active;
        if (changed) {
            size_t length = strlen(text) + 1;
            char *copy = arena_alloc(scratch, length);
            memcpy(copy, text, length);
            value->str = copy;
        }
        state->committed |= committed;
        return changed;
    }
    if (shape->type >= PIN_I8 && shape->type <= PIN_F64) {
        if (shape->type == PIN_I64 || shape->type == PIN_U64) {
            bool streaming, active, committed;
            bool changed = edit_integer64(k, key, value, flags, &streaming, &active, &committed);
            state->changed |= changed;
            state->streaming |= streaming;
            state->active |= active;
            state->committed |= committed;
            return changed;
        }
        double number = sg_literal_number(value);
        bool integer = literal_integer(shape->type);
        double min = 0, max = 0;
        sg_literal_bounds(shape->type, &min, &max);
        if (range) {
            min = fmax(min, range->min);
            max = fmin(max, range->max);
        }
        kit_number_desc desc = { .integer = integer,
                                 .single_precision = shape->type == PIN_F32,
                                 .min = min,
                                 .max = max,
                                 .precision = integer ? 0 : 2,
                                 .step = integer ? 1 : 0.1,
                                 .flags = flags };
        bool changed = kit_number(k, key, &number, &desc);
        bool committed = kit_item_committed(k);
        bool active = kit_item_active(k) && !committed;
        state->streaming |= changed || active;
        state->active |= active;
        state->committed |= committed;
        if (changed) {
            sg_set_literal_number(value, number);
        }
        state->changed |= changed;
        return changed;
    }
    if (shape->type == PIN_HANDLE) {
        kit_label(
            k,
            key,
            "muted",
            value->u64
                ? runtime_entity_label(s->scene, ecs_entity_from_u64(value->u64))
                : "None"
        );
        return false;
    }
    kit_label(k, key, "muted", ed_pin_type_name(shape));
    return false;
}

/* Writes the pin's default, through collaboration while it runs: a new Undo
   step, or with `coalesce` the gesture's open one. 0 on success. */
static int write_literal(studio *s, node *n, int output, int p, const pin_value *value, bool coalesce)
{
    editor *e = sg_graph_editor(s);
    if (editor_presentation(e) || (collab_active() && !collab_replaying())) {
        return editor_command_apply(
            e,
            (editor_command){ .kind = EDITOR_COMMAND_SET_PIN_DEFAULT,
                              .data.set_pin_default = {
                                  .node_id = n->id,
                                  .output = output,
                                  .pin_index = p,
                                  .value = (pin_value *)value,
                                  .coalesce_history = coalesce &&
                                                      (editor_presentation(e) || collab_is_host()) } }
        );
    }
    /* A scrub is one undo step; discrete edits, and a step or typed value
       that commits as it changes, are one each. */
    if (!coalesce) {
        ed_snapshot_before(e);
    }
    return graph_set_pin_default(sg_active_graph(s), n, output, p, value);
}

bool sg_set_literal(studio *s, node *n, int output, int p, const pin_value *value, sg_step step)
{
    studio_graph *g = &s->graph;
    const pin_spec *spec = output ? &n->output_pins[p] : &n->input_pins[p];
    bool same = value_edit_matches(g, n, output, spec->id, NULL);
    int edited = write_literal(s, n, output, p, value, step != SG_STEP_ONCE && same);
    value_edit_set(g, n, output, spec->id, NULL, edited == 0 && step == SG_STEP_LIVE);
    if (edited == 0) {
        touched(sg_graph_editor(s));
    }
    return edited == 0;
}

static void apply_literal_edit(
    studio *s,
    kit *k,
    node *n,
    int output,
    int p,
    pin_value *value,
    literal_edit state,
    unsigned flags
)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    const pin_spec *spec = output ? &n->output_pins[p] : &n->input_pins[p];
    bool same_edit = value_edit_matches(g, n, output, spec->id, k);
    bool readonly = sg_graph_readonly(s) || (flags & KIT_READONLY);
    if (!state.changed || readonly) {
        if (same_edit && (readonly || state.committed || !state.active)) {
            value_edit_set(g, n, output, spec->id, k, false);
        }
        return;
    }
    int edited = write_literal(s, n, output, p, value, state.streaming && same_edit);
    value_edit_set(
        g,
        n,
        output,
        spec->id,
        k,
        edited == 0 && state.streaming && state.active && !state.committed
    );
    if (edited == 0) {
        touched(e);
    }
}

static bool edit_compound(studio *s, kit *k, node *n, int output, int p, const pin_spec *spec, unsigned flags)
{
    arena scratch = { 0 };
    pin_value value = spec->default_value;
    if (pin_value_snapshot_to_arena(&scratch, &value, &spec->default_value)) {
        arena_free(&scratch);
        return false;
    }
    literal_edit state = { 0 };
    pin_spec shape = *spec;
    if (sg_value_kind_of(s, n, output, p) == VALUE_COLOR) {
        shape.flags |= PIN_FLAG_COLOR;
    }
    char key[32];
    snprintf(key, sizeof key, "pin-%llu", (unsigned long long)spec->id);
    edit_literal_value(s, k, key, &value, &shape, NULL, NULL, &scratch, flags, &state, 0, NULL);
    apply_literal_edit(s, k, n, output, p, &value, state, flags);
    arena_free(&scratch);
    return state.changed;
}

/* A pin's choice as a select whose picker lists the choices. */
static bool choice_control(
    studio *s,
    kit *k,
    int output,
    const pin_spec *spec,
    pin_value *value,
    const editor_choices *choices,
    unsigned flags
)
{
    int count = editor_choices_count(choices), current = -1;
    for (int i = 0; i < count; i++) {
        editor_choice choice = editor_choice_at(choices, i);
        bool selected = spec->type == PIN_NAME ? value->str && choice.name &&
                                                     !strcmp(value->str, choice.name)
                      : spec->type == PIN_ENUM
                          ? choice.value == value->e
                          : choice.value == sg_literal_number(value);
        if (selected) {
            current = i;
            break;
        }
    }
    char pick[64];
    snprintf(pick, sizeof pick, "graph:pin-%" PRIu64 "-%d", spec->id, output);
    kit_picker_desc picker = { .value = current >= 0
                                          ? editor_choice_at(choices, current).label
                                          : "-",
                               .flags = flags };
    bool clicked = kit_picker(k, "v", &picker) && !(flags & KIT_READONLY);
    kit_flag(k, "data-open", picker_open(s, pick));
    int index = choice_pick(s, k, pick, clicked, choices, current, "Search options");
    if (index < 0 || index >= count || index == current) {
        return false;
    }
    editor_choice selected = editor_choice_at(choices, index);
    if (spec->type == PIN_NAME) {
        value->str = selected.name;
    } else if (spec->type == PIN_ENUM) {
        value->e = selected.value;
    } else {
        sg_set_literal_number(value, selected.value);
    }
    return true;
}

/* A pin's value editor, in the canvas or in Properties. */
const char *sg_material_picker(studio *s, kit *k, const char *value, unsigned flags)
{
    editor *e = sg_graph_editor(s);
    kit_command_item *items = NULL;
    const behavior_asset **materials = NULL;
    for (int i = 0; e && i < acount(e->asset_catalog); i++) {
        const behavior_asset *asset = e->asset_catalog[i];
        if (asset->domain != BEHAVIOR_DOMAIN_MATERIAL) {
            continue;
        }
        apush(materials, asset);
        apush(
            items,
            ((kit_command_item){ .label = asset->name,
                                 .current = value && !strcmp(value, asset->name) })
        );
    }
    kit_autocomplete_result
        choice = kit_autocomplete(k, "material", value, items, acount(items), "Choose material", flags);
    int picked = choice.selected;
    const char *result = picked >= 0 && picked < acount(materials)
                           ? materials[picked]->name
                           : NULL;
    afree(items);
    afree(materials);
    return result;
}

void sg_emit_value_in(studio *s, kit *k, node *n, int output, int p)
{
    studio_graph *g = &s->graph;
    editor *e = sg_graph_editor(s);
    graph *gr = sg_active_graph(s);
    const pin_spec *spec = output ? &n->output_pins[p] : &n->input_pins[p];
    bool readonly = sg_graph_readonly(s) ||
                    (output && !ed_pin_editable(gr, n, ED_PIN_OUTPUT, p));
    unsigned flags = readonly ? KIT_READONLY : 0;
    int width = pin_vector_width(spec);
    bool color = sg_value_kind_of(s, n, output, p) == VALUE_COLOR;
    if (spec->container != PIN_CONTAINER_SINGLE ||
        (spec->type == PIN_STRUCT && (width == 0 || color)) || color) {
        edit_compound(s, k, n, output, p, spec, flags);
        return;
    }
    pin_value value = spec->default_value;
    bool changed = false, streaming = false, committed = false;
    bool active = false;
    const editor_choices *choices = ed_pin_choices(e, n, output, p);
    double number = 0;
    float vector[4] = { 0 };
    char text[256];
    int count = choices ? editor_choices_count(choices) : 0;
    if (count > 0) {
        changed = choice_control(s, k, output, spec, &value, choices, flags);
    } else if (spec->type == PIN_BOOL) {
        bool b = value.b;
        if (kit_checkbox(k, "v", &b, NULL, flags)) {
            value.b = b;
            changed = true;
        }
    } else if (width > 0 && !(spec->flags & PIN_FLAG_COLOR)) {
        if (spec->default_value.ptr) {
            memcpy(vector, spec->default_value.ptr, (size_t)width * sizeof(float));
        }
        static const char *const axes[] = { "X", "Y", "Z", "W" };
        for (int a = 0; a < width; a++) {
            char key[8];
            snprintf(key, sizeof key, "v%d", a);
            double axis = vector[a];
            kit_number_desc desc = { .single_precision = true,
                                     .min = -FLT_MAX,
                                     .max = FLT_MAX,
                                     .precision = 2,
                                     .step = 0.1,
                                     .addon = axes[a],
                                     .addon_cls = a == 0 ? "x"
                                                : a == 1 ? "y"
                                                         : NULL,
                                     .flags = flags };
            bool edited = kit_number(k, key, &axis, &desc);
            bool item_committed = kit_item_committed(k);
            bool item_active = kit_item_active(k) && !item_committed;
            streaming |= edited || item_active;
            active |= item_active;
            committed |= item_committed;
            if (edited) {
                vector[a] = (float)axis;
                changed = true;
            }
        }
        if (changed) {
            value.ptr = vector;
        }
    } else if (spec->type == PIN_STR || spec->type == PIN_NAME) {
        if (spec->type_name && !strcmp(spec->type_name, "MaterialAsset")) {
            const char *picked = sg_material_picker(s, k, value.str, flags);
            changed = picked && (!value.str || strcmp(picked, value.str));
            committed = changed;
            if (changed) {
                value.str = picked;
            }
        } else {
            snprintf(text, sizeof text, "%s", value.str ? value.str : "");
            changed = kit_text_field(k, "v", text, sizeof text, NULL, flags);
            committed = kit_item_committed(k);
            active = kit_item_active(k) && !committed;
            streaming = changed || active;
            if (changed) {
                value.str = text;
            }
        }
    } else if (spec->type == PIN_I64 || spec->type == PIN_U64) {
        changed = edit_integer64(k, "v", &value, flags, &streaming, &active, &committed);
    } else if (spec->type >= PIN_I8 && spec->type <= PIN_F64) {
        bool integer = spec->type < PIN_F32;
        switch (spec->type) {
            case PIN_I8: number = value.i8; break;
            case PIN_U8: number = value.u8; break;
            case PIN_I16: number = value.i16; break;
            case PIN_U16: number = value.u16; break;
            case PIN_I32: number = value.i; break;
            case PIN_U32: number = value.u; break;
            case PIN_I64: number = (double)value.i64; break;
            case PIN_U64: number = (double)value.u64; break;
            case PIN_F32: number = value.f; break;
            default: number = value.d; break;
        }
        double min = 0, max = 0;
        sg_literal_bounds(spec->type, &min, &max);
        kit_number_desc desc = { .single_precision = spec->type == PIN_F32,
                                 .min = min,
                                 .max = max,
                                 .integer = integer,
                                 .precision = integer ? 0 : 2,
                                 .step = integer ? 1 : 0.1,
                                 .flags = flags };
        changed = kit_number(k, "v", &number, &desc);
        committed = kit_item_committed(k);
        active = kit_item_active(k) && !committed;
        streaming = changed || active;
        if (changed) {
            switch (spec->type) {
                case PIN_I8: value.i8 = (int8_t)number; break;
                case PIN_U8: value.u8 = (uint8_t)number; break;
                case PIN_I16: value.i16 = (int16_t)number; break;
                case PIN_U16: value.u16 = (uint16_t)number; break;
                case PIN_I32: value.i = (int32_t)number; break;
                case PIN_U32: value.u = (uint32_t)number; break;
                case PIN_I64: value.i64 = (int64_t)number; break;
                case PIN_U64: value.u64 = (uint64_t)number; break;
                case PIN_F32: value.f = (float)number; break;
                default: value.d = number; break;
            }
        }
    }
    bool same_edit = value_edit_matches(g, n, output, spec->id, k);
    if (!changed || readonly) {
        if (same_edit && (readonly || committed || !active)) {
            value_edit_set(g, n, output, spec->id, k, false);
        }
        return;
    }
    int edited = write_literal(s, n, output, p, &value, streaming && same_edit);
    value_edit_set(g, n, output, spec->id, k, edited == 0 && streaming && active && !committed);
    if (edited == 0) {
        touched(e);
    }
}
