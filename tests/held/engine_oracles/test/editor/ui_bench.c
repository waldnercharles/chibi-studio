#include "authoring/ui_loader.h"
/* The headless cases of the S0 UI bench (ui_bench.h). Each case runs RUNS
 * times after WARMUP runs, and the two variants of a case alternate run by
 * run, so a change in the machine's load reaches both alike. Documents use
 * Studio's theme (the StudioShell page) at 1440x900, dp ratio 1. */
#include "test/editor/ui_bench.h"

#include "assets/prototype.h"
#include "editor/ui/highlight.h"
#include "editor/ui/labels.h"
#include "graph/model/registry.h"
#include "types/type.h"
#include "render/ui.h"
#include "runtime/component_catalog.h"
#include "support/array.h"
#include "support/text_file.h"
#include "support/xalloc.h"
#include "ui/json_items.h"
#include "ui/list.h"
#include "ui/rml.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STUDIO_CONTENT "projects/studio/content"
#define ARENA_CONTENT "projects/arena2d/content/"
#define RUNS 41
#define WARMUP 3
#define WIDTH 1440
#define HEIGHT 900
/* About Properties' area in Studio's default layout at 1440x900. */
#define AREA_WIDTH 330
#define AREA_HEIGHT 820
#define ARMS 2

/* ---------- Timing and counts ---------- */

double bench_now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

bench_stat bench_stat_of(double *ms, int count)
{
    qsort(ms, (size_t)count, sizeof *ms, by_value);
    return (bench_stat){ ms[count / 2], ms[count / 4], ms[count * 3 / 4] };
}

void bench_header(const char *title)
{
    printf(
        "\n%s\n\n"
        "| # | Case | Median ms | p25-p75 ms | Values | Lookups | Layouts | "
        "Elements |\n"
        "|---|---|---:|---:|---:|---:|---:|---:|\n",
        title
    );
}

void bench_row(const char *id, const char *name, bench_stat t, bench_counts c)
{
    printf(
        "| %s | %s | %.3f | %.3f-%.3f | %llu | %llu | %llu | %d |\n",
        id,
        name,
        t.median,
        t.low,
        t.high,
        (unsigned long long)c.values,
        (unsigned long long)c.lookups,
        (unsigned long long)c.layouts,
        c.elements
    );
    fflush(stdout);
}

void bench_ratio(const char *id, const char *name, bench_stat a, bench_stat b)
{
    printf("| %s | %s | %.2fx | %.3f / %.3f | | | | |\n", id, name, b.median / a.median, b.median, a.median);
    fflush(stdout);
}

static uint64_t layouts;
static rml_instancer *probe_instancer;

static void probe_layout(rml_element *, void *)
{
    layouts++;
}

static const rml_element_hooks PROBE = { .layout = probe_layout };

rml_element *bench_probe(rml_element *document)
{
    if (!probe_instancer) {
        probe_instancer = rml_instancer_create(&PROBE);
    }
    /* RmlUi forgets instancers when it shuts down. */
    rml_register_element_instancer("bench-probe", probe_instancer);
    rml_element *probe = rml_element_create_child(document, "bench-probe", NULL);
    rml_element_set_property(probe, "position", "absolute");
    rml_element_set_property(probe, "width", "0dp");
    rml_element_set_property(probe, "height", "0dp");
    return probe;
}

uint64_t bench_layouts()
{
    return layouts;
}

static int elements(rml_element *document)
{
    return rml_element_query_selector_all(document, "*", NULL, 0);
}

/* ---------- Documents ---------- */

/* A document in a context of its own, styled as Studio, over the variables
 * of `json`'s root. */
typedef struct bench_doc
{
    rml_backend *backend;
    ui_data data;
    rml_element *doc;
} bench_doc;

static ui_page theme;

static void on_emit(void *, const rml_event *, const char *, int, const char *const *)
{
}

static void on_key(void *, const rml_event *) {}

/* What Studio's settings add to its theme at the default compact density
 * (studio_settings.c). */
static const char DENSITY
    [] = "body, body.dark { "
         "--studio-standard-font-family: Geist; "
         "--studio-code-font-family: JetBrains Mono; "
         "--studio-numeric-font-family: GeistNumeric; "
         "font-size: 13dp; line-height: 18dp; --studio-property-label-max: "
         "160dp; "
         "--h: 26dp; --text-line: 24dp; --h-sm: 22dp; --input-line: 16dp; "
         "--input-pad: 4dp; --row: 26dp; --gap: 2dp; --header-h: 32dp; "
         "--dropdown-list-h: 182dp; --dropdown-box-h: 192dp; --topbar-h: 40dp; "
         "--status-h: 28dp; --panel-h: 30dp; }\n";

/* Opens an empty document; doc_show styles it and adds `body`. `bound`
 * binds the body to the model; a mounted asset binds its host instead. */
static void doc_open(bench_doc *d, yyjson_mut_doc *json, bool bound, int width, int height)
{
    if (!theme.name[0]) {
        authoring_ui_page_sync(&theme, STUDIO_CONTENT, "StudioShell", 0);
        assert(theme.document.rcss);
    }
    d->backend = rml_backend_create(width, height, 1);
    rml_context *context = rml_backend_context(d->backend);
    ui_data_init(&d->data, context, json, on_emit, NULL);
    d->doc = rml_context_load_document_from_memory(
        context,
        bound ? "<rml><head/><body class=\"studio dark\" "
                "data-model=\"" UI_DATA_MODEL "\"/></rml>"
              : "<rml><head/><body class=\"studio dark\"/></rml>",
        "ui-bench"
    );
    assert(d->doc);
    rml_document_show(d->doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
}

/* Styles the document with Studio's theme and `rcss`, then shows `body`. */
static void doc_show(bench_doc *d, const char *rcss, const char *body)
{
    size_t size = strlen(theme.document.rcss) + strlen(DENSITY) + strlen(rcss) + 2;
    char *sheet = xmalloc(size);
    snprintf(sheet, size, "%s\n%s%s", theme.document.rcss, DENSITY, rcss);
    bool styled = rml_document_set_named_style_sheet(d->doc, "ui-bench", sheet);
    assert(styled);
    free(sheet);
    if (body) {
        rml_element_set_inner_rml(d->doc, body);
        bench_probe(d->doc);
    }
    rml_backend_update(d->backend, 0);
}

static void doc_close(bench_doc *d)
{
    rml_document_close(d->doc);
    rml_backend_update(d->backend, 0);
    ui_data_free(&d->data);
    rml_backend_destroy(d->backend);
}

/* What the document's views and layouts did since `before`. */
static bench_counts doc_counts(const bench_doc *d, bench_counts before)
{
    rml_data_reads reads = rml_data_model_reads(d->data.model);
    return (bench_counts){ .values = reads.values - before.values,
                           .lookups = reads.lookups - before.lookups,
                           .layouts = layouts - before.layouts,
                           .elements = elements(d->doc) };
}

static bench_counts doc_mark(const bench_doc *d)
{
    rml_data_reads reads = rml_data_model_reads(d->data.model);
    return (bench_counts){ .values = reads.values,
                           .lookups = reads.lookups,
                           .layouts = layouts };
}

static yyjson_mut_doc *json_new(yyjson_mut_val **root)
{
    yyjson_mut_doc *json = yyjson_mut_doc_new(NULL);
    *root = yyjson_mut_obj(json);
    yyjson_mut_doc_set_root(json, *root);
    return json;
}

/* Dirties `variable` and updates: views, styles and any layout. */
static double update_time(bench_doc *d, const char *variable)
{
    double t = bench_now();
    rml_data_model_dirty(d->data.model, variable);
    rml_backend_update(d->backend, 0);
    return bench_now() - t;
}

/* ---------- 2: one variable's views ---------- */

static const char TEXT_ROWS[] = "<div class=\"v\" data-for=\"row : "
                                "big.rows\">{{ row.v }}</div>";
static const char ATTRIBUTE_ROWS[] = "<div class=\"v\" data-for=\"row : "
                                     "big.rows\" data-attr-data-v=\"row.v\">"
                                     "<span/></div>";

/* `views` rows each showing one value of one variable, as text, which lays
 * out when it changes, or as an attribute, which does not (as a
 * <number-field> repaints without layout); one value changed or all. */
static void dirty_cost(int views, bool attribute)
{
    yyjson_mut_val *root;
    yyjson_mut_doc *json = json_new(&root);
    yyjson_mut_val *rows = yyjson_mut_obj_add_arr(json, yyjson_mut_obj_add_obj(json, root, "big"), "rows");
    yyjson_mut_val **values = NULL;
    for (int i = 0; i < views; i++) {
        yyjson_mut_val *v = yyjson_mut_int(json, i);
        yyjson_mut_obj_add_val(json, yyjson_mut_arr_add_obj(json, rows), "v", v);
        apush(values, v);
    }
    bench_doc d;
    doc_open(&d, json, true, WIDTH, HEIGHT);
    doc_show(&d, ".v { display: block; height: 14dp; }", attribute ? ATTRIBUTE_ROWS : TEXT_ROWS);
    double ms[ARMS][RUNS];
    bench_counts counts[ARMS];
    int next = views;
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            for (int i = arm ? 0 : views / 2; i < (arm ? views : views / 2 + 1); i++) {
                yyjson_mut_set_int(values[i], next++);
            }
            bench_counts before = doc_mark(&d);
            double t = update_time(&d, "big");
            counts[arm] = doc_counts(&d, before);
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    const char *kind = attribute ? "attribute" : "text";
    char name[96];
    snprintf(name, sizeof name, "%d %s views, one value changed", views, kind);
    bench_row("2", name, bench_stat_of(ms[0], RUNS), counts[0]);
    snprintf(name, sizeof name, "%d %s views, all values changed", views, kind);
    bench_row("2", name, bench_stat_of(ms[1], RUNS), counts[1]);
    doc_close(&d);
    afree(values);
}

/* ---------- The Properties spike ---------- */

typedef enum
{
    KIND_NUMBER,
    KIND_VECTOR,
    KIND_BOOL,
    KIND_CHOICE,
    KIND_COLOR,
    KIND_TEXT,
    KIND_COUNT,
} row_kind;

static const char *const KIND_NAMES[KIND_COUNT] = { "number", "vector", "bool",
                                                    "choice", "color",  "text" };
static const char *const AXES[] = { "x", "y", "z", "w" },
                         *const AXIS_LABELS[] = { "X", "Y", "Z", "W" };

/* A shown value the spike refreshes from its field. */
typedef struct spike_value
{
    const ecs_component_desc *desc;
    int field;
    int axis;              /* a float of a vector field, or -1 */
    bool number;           /* a <number-field>'s value */
    yyjson_mut_val *shown; /* its number or text, or a check's bool */
} spike_value;

struct bench_spike
{
    bench_doc doc;
    ui_mount *mount;
    rml_listener *listener;
    yyjson_mut_val *properties;
    spike_value *values; /* array.h */
    const ecs_world *world;
    ecs_entity entity;
    int rows;
};

static const char SPIKE_HOST[] = "<div id=\"host\" style=\"position: absolute; "
                                 "left: 0dp; top: 0dp; "
                                 "right: 0dp; bottom: 0dp;\"/>";

bench_spike *bench_spike_new(int width, int height)
{
    bench_spike *s = xcalloc(1, sizeof *s);
    yyjson_mut_val *root;
    yyjson_mut_doc *json = json_new(&root);
    s->properties = yyjson_mut_obj_add_obj(json, root, "properties");
    yyjson_mut_val *tabs = yyjson_mut_obj_add_arr(json, s->properties, "tabs");
    static const char *const TABS[][2] = {
        { "object", "\xee\xaa\xb2" },
        { "prototype", "\xee\xab\x84" },
        { "scene", "\xee\xab\x83" },
        { "behaviors", "\xee\xab\x86" },
    };
    for (int i = 0; i < 4; i++) {
        yyjson_mut_val *tab = yyjson_mut_arr_add_obj(json, tabs);
        yyjson_mut_obj_add_str(json, tab, "id", TABS[i][0]);
        yyjson_mut_obj_add_str(json, tab, "icon", TABS[i][1]);
        yyjson_mut_obj_add_str(json, tab, "tip", TABS[i][0]);
        yyjson_mut_obj_add_bool(json, tab, "on", i == 0);
    }
    yyjson_mut_val *identity = yyjson_mut_obj_add_obj(json, s->properties, "identity");
    yyjson_mut_obj_add_str(json, identity, "name", "");
    yyjson_mut_obj_add_str(json, identity, "prototype", "");
    yyjson_mut_obj_add_arr(json, s->properties, "panels");
    doc_open(&s->doc, json, false, width, height);
    doc_show(&s->doc, "", SPIKE_HOST);
    s->mount = authoring_ui_mount_new(STUDIO_CONTENT, "spike/PropertiesSpike");
    s->listener = rml_listener_create(on_key, NULL);
    ui_mount_sync(s->mount, rml_element_by_id(s->doc.doc, "host"), s->listener, 0);
    assert(!ui_mount_error(s->mount)[0]);
    doc_show(&s->doc, ui_mount_style(s->mount), NULL);
    return s;
}

void bench_spike_free(bench_spike *s)
{
    ui_mount_free(s->mount);
    doc_close(&s->doc);
    rml_listener_destroy(s->listener);
    afree(s->values);
    free(s);
}

rml_backend *bench_spike_backend(bench_spike *s)
{
    return s->doc.backend;
}

bench_counts bench_spike_counts(bench_spike *s)
{
    rml_data_reads reads = rml_data_model_reads(s->doc.data.model);
    return (bench_counts){ .values = reads.values,
                           .lookups = reads.lookups,
                           .layouts = layouts,
                           .elements = elements(s->doc.doc) };
}

int bench_spike_rows(const bench_spike *s)
{
    return s->rows;
}

static int vector_axes(const char *type_name)
{
    if (!type_name) {
        return 0;
    }
    return !strcmp(type_name, "CF_V2") ? 2
         : !strcmp(type_name, "CF_V3") ? 3
         : !strcmp(type_name, "CF_V4") ? 4
                                       : 0;
}

static row_kind kind_of(const type_field *f, const pin_value *v)
{
    switch (v->type) {
        case PIN_BOOL: return KIND_BOOL;
        case PIN_ENUM: return KIND_CHOICE;
        case PIN_U32: return f->color ? KIND_COLOR : KIND_NUMBER;
        case PIN_I8:
        case PIN_U8:
        case PIN_I16:
        case PIN_U16:
        case PIN_I32:
        case PIN_I64:
        case PIN_U64:
        case PIN_F32:
        case PIN_F64: return KIND_NUMBER;
        case PIN_STRUCT:
            return vector_axes(f->type_name) ? KIND_VECTOR : KIND_TEXT;
        default: return KIND_TEXT;
    }
}

static long long integer_of(const pin_value *v)
{
    switch (v->type) {
        case PIN_I8: return v->i8;
        case PIN_U8: return v->u8;
        case PIN_I16: return v->i16;
        case PIN_U16: return v->u16;
        case PIN_I32: return v->i;
        case PIN_U32: return v->u;
        case PIN_I64: return v->i64;
        case PIN_U64: return (long long)v->u64;
        default: return v->e;
    }
}

static const char *choice_label(const type_field *f, int value)
{
    const enum_desc *e = type_catalog_enum_find(f->type_name);
    for (int i = 0; e && i < e->value_count; i++) {
        if (e->values[i].value == value) {
            return e->values[i].label ? e->values[i].label : e->values[i].name;
        }
    }
    return NULL;
}

/* The text a field shows, as the native field formats it. */
static void field_text(const type_field *f, const pin_value *v, int axis, char *out, size_t size)
{
    const char *label = v->type == PIN_ENUM ? choice_label(f, v->e) : NULL;
    switch (v->type) {
        case PIN_F32: snprintf(out, size, "%.2f", v->f); return;
        case PIN_F64: snprintf(out, size, "%.2f", v->d); return;
        case PIN_ENUM:
            if (label) {
                snprintf(out, size, "%s", label);
                return;
            }
            break;
        case PIN_STR:
        case PIN_NAME: snprintf(out, size, "%s", v->str ? v->str : ""); return;
        case PIN_STRUCT:
            if (axis >= 0) {
                snprintf(out, size, "%.2f", ((const float *)v->ptr)[axis]);
                return;
            }
            snprintf(out, size, "%s", f->type_name ? f->type_name : "");
            return;
        case PIN_HANDLE: snprintf(out, size, "Entity"); return;
        default: break;
    }
    if (f->color) {
        snprintf(out, size, "#%08x", v->u);
        return;
    }
    snprintf(out, size, "%lld", integer_of(v));
}

/* The number a <number-field> shows. */
static double number_of(const pin_value *v, int axis)
{
    switch (v->type) {
        case PIN_F32: return v->f;
        case PIN_F64: return v->d;
        case PIN_STRUCT: return ((const float *)v->ptr)[axis];
        default: return (double)integer_of(v);
    }
}

static bool read_value(const bench_spike *s, const spike_value *value, pin_value *out)
{
    return ecs_component_read_field(s->world, s->entity, value->desc->id, value->field, out) ==
           ECS_OK;
}

/* Adds a text to `item` that refreshes follow. */
static void add_shown(bench_spike *s, yyjson_mut_val *item, const char *key, spike_value value)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    pin_value v;
    bool read = read_value(s, &value, &v);
    assert(read);
    const type_field *f = &value.desc->type->fields[value.field];
    if (value.number) {
        value.shown = yyjson_mut_real(json, number_of(&v, value.axis));
    } else if (v.type == PIN_BOOL) {
        value.shown = yyjson_mut_bool(json, v.b);
    } else {
        char text[256];
        field_text(f, &v, value.axis, text, sizeof text);
        value.shown = yyjson_mut_strcpy(json, text);
    }
    yyjson_mut_obj_add_val(json, item, key, value.shown);
    apush(s->values, value);
}

/* A vector axis's fields other than its value. */
static void spike_axis(yyjson_mut_doc *json, yyjson_mut_val *axis, int a)
{
    yyjson_mut_obj_add_str(json, axis, "key", AXES[a]);
    yyjson_mut_obj_add_str(json, axis, "label", AXIS_LABELS[a]);
    yyjson_mut_obj_add_real(json, axis, "step", 0.1);
    yyjson_mut_obj_add_int(json, axis, "precision", 2);
}

/* A row with an empty array for each control kind, which `kinds` gets. */
static void new_row(
    bench_spike *s,
    yyjson_mut_val *rows,
    const char *id,
    const char *label,
    const char *about,
    yyjson_mut_val **kinds
)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    yyjson_mut_val *row = yyjson_mut_arr_add_obj(json, rows);
    yyjson_mut_obj_add_strcpy(json, row, "id", id);
    yyjson_mut_obj_add_strcpy(json, row, "label", label);
    yyjson_mut_obj_add_strcpy(json, row, "about", about);
    yyjson_mut_obj_add_bool(json, row, "overridden", false);
    yyjson_mut_obj_add_bool(json, row, "mixed", false);
    yyjson_mut_obj_add_bool(json, row, "readonly", false);
    yyjson_mut_obj_add_bool(json, row, "invalid", false);
    for (int k = 0; k < KIND_COUNT; k++) {
        kinds[k] = yyjson_mut_obj_add_arr(json, row, KIND_NAMES[k]);
    }
    s->rows++;
}

static void add_field_row(bench_spike *s, yyjson_mut_val *rows, const ecs_component_desc *desc, int field)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    const type_field *f = &desc->type->fields[field];
    pin_value v;
    if (ecs_component_read_field(s->world, s->entity, desc->id, field, &v) != ECS_OK) {
        return;
    }
    char id[192], label[128];
    snprintf(
        id,
        sizeof id,
        "%llu/%s.%s",
        (unsigned long long)ecs_entity_to_u64(s->entity),
        desc->name,
        f->name
    );
    editor_identifier_label(f->name, label, sizeof label);
    yyjson_mut_val *kinds[KIND_COUNT];
    new_row(s, rows, id, label, f->description ? f->description : "", kinds);
    row_kind kind = kind_of(f, &v);
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(json, kinds[kind]);
    spike_value value = { .desc = desc, .field = field, .axis = -1 };
    bool real = v.type == PIN_F32 || v.type == PIN_F64;
    switch (kind) {
        case KIND_NUMBER:
            value.number = true;
            add_shown(s, item, "value", value);
            yyjson_mut_obj_add_real(json, item, "step", real ? 0.1 : 1);
            yyjson_mut_obj_add_int(json, item, "precision", real ? 2 : 0);
            yyjson_mut_obj_add_bool(json, item, "integer", !real);
            yyjson_mut_obj_add_str(json, item, "unit", "");
            return;
        case KIND_VECTOR: {
            yyjson_mut_val *axes = yyjson_mut_obj_add_arr(json, item, "axes");
            value.number = true;
            for (int a = 0; a < vector_axes(f->type_name); a++) {
                yyjson_mut_val *axis = yyjson_mut_arr_add_obj(json, axes);
                spike_axis(json, axis, a);
                value.axis = a;
                add_shown(s, axis, "value", value);
            }
            return;
        }
        case KIND_BOOL: add_shown(s, item, "value", value); return;
        case KIND_CHOICE: add_shown(s, item, "label", value); return;
        case KIND_COLOR: add_shown(s, item, "css", value); return;
        default: add_shown(s, item, "value", value); return;
    }
}

/* GameTransform's x and y as one Position row, as native Properties joins
 * them. */
static void add_position_row(bench_spike *s, yyjson_mut_val *rows, const ecs_component_desc *desc)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    char id[192];
    snprintf(id, sizeof id, "%llu/%s.position", (unsigned long long)ecs_entity_to_u64(s->entity), desc->name);
    yyjson_mut_val *kinds[KIND_COUNT];
    new_row(s, rows, id, "Position", "", kinds);
    yyjson_mut_val *axes = yyjson_mut_obj_add_arr(
        json,
        yyjson_mut_arr_add_obj(json, kinds[KIND_VECTOR]),
        "axes"
    );
    for (int a = 0; a < 2; a++) {
        yyjson_mut_val *axis = yyjson_mut_arr_add_obj(json, axes);
        spike_axis(json, axis, a);
        add_shown(s, axis, "value", (spike_value){ .desc = desc, .field = a, .axis = -1, .number = true });
    }
}

static void add_panel(bench_spike *s, yyjson_mut_val *panels, const ecs_component_desc *desc)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    yyjson_mut_val *panel = yyjson_mut_arr_add_obj(json, panels);
    char title[128];
    editor_component_label(desc->name, title, sizeof title);
    yyjson_mut_obj_add_strcpy(json, panel, "key", desc->name);
    yyjson_mut_obj_add_strcpy(json, panel, "title", title);
    yyjson_mut_obj_add_str(json, panel, "badge", "");
    yyjson_mut_obj_add_bool(json, panel, "open", true);
    yyjson_mut_val *rows = yyjson_mut_obj_add_arr(json, panel, "rows");
    if (desc->id == GAME_COMPONENT_GameTransform.id) {
        add_position_row(s, rows, desc);
        return;
    }
    for (int i = 0; desc->type && i < desc->type->field_count; i++) {
        add_field_row(s, rows, desc, i);
    }
}

void bench_spike_show(
    bench_spike *s,
    const ecs_world *world,
    ecs_entity entity,
    const char *name,
    const component_presentation *tabs
)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    s->world = world;
    s->entity = entity;
    s->rows = 0;
    asetlen(s->values, 0);
    yyjson_mut_val *identity = yyjson_mut_obj_get(s->properties, "identity");
    ui_data_set_text(json, yyjson_mut_obj_get(identity, "name"), name);
    yyjson_mut_val *panels = yyjson_mut_obj_get(s->properties, "panels");
    yyjson_mut_arr_clear(panels);
    for (int i = 0; i < ecs_world_component_count(world); i++) {
        const ecs_component_desc *desc = ecs_world_component_at(world, i);
        if (desc && ecs_component_has(world, entity, desc->id) &&
            component_presentation_tab(tabs, desc) == COMPONENT_TAB_OBJECT) {
            add_panel(s, panels, desc);
        }
    }
    rml_data_model_dirty(s->doc.data.model, "properties");
}

int bench_spike_refresh(bench_spike *s)
{
    int changed = 0;
    for (int i = 0; i < acount(s->values); i++) {
        const spike_value *value = &s->values[i];
        pin_value v;
        if (!read_value(s, value, &v)) {
            continue;
        }
        if (value->number) {
            double number = number_of(&v, value->axis);
            changed += yyjson_mut_get_real(value->shown) != number;
            yyjson_mut_set_real(value->shown, number);
            continue;
        }
        if (v.type == PIN_BOOL) {
            changed += yyjson_mut_get_bool(value->shown) != v.b;
            yyjson_mut_set_bool(value->shown, v.b);
            continue;
        }
        char text[256];
        field_text(&value->desc->type->fields[value->field], &v, value->axis, text, sizeof text);
        if (strcmp(yyjson_mut_get_str(value->shown), text)) {
            ui_data_set_text(s->doc.data.doc, value->shown, text);
            changed++;
        }
    }
    if (changed) {
        rml_data_model_dirty(s->doc.data.model, "properties");
    }
    return changed;
}

/* One open panel of `count` number rows, for instancing. */
static void spike_fill(bench_spike *s, int count)
{
    yyjson_mut_doc *json = s->doc.data.doc;
    s->rows = 0;
    asetlen(s->values, 0);
    yyjson_mut_val *panels = yyjson_mut_obj_get(s->properties, "panels");
    yyjson_mut_arr_clear(panels);
    yyjson_mut_val *panel = yyjson_mut_arr_add_obj(json, panels);
    yyjson_mut_obj_add_str(json, panel, "key", "bench");
    yyjson_mut_obj_add_str(json, panel, "title", "Bench");
    yyjson_mut_obj_add_str(json, panel, "badge", "");
    yyjson_mut_obj_add_bool(json, panel, "open", true);
    yyjson_mut_val *rows = yyjson_mut_obj_add_arr(json, panel, "rows");
    for (int i = 0; i < count; i++) {
        char id[32], label[32];
        snprintf(id, sizeof id, "bench.%d", i);
        snprintf(label, sizeof label, "Field %d", i);
        yyjson_mut_val *kinds[KIND_COUNT];
        new_row(s, rows, id, label, "", kinds);
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(json, kinds[KIND_NUMBER]);
        yyjson_mut_obj_add_real(json, item, "value", i * 1.5);
        yyjson_mut_obj_add_real(json, item, "step", 0.1);
        yyjson_mut_obj_add_int(json, item, "precision", 2);
        yyjson_mut_obj_add_bool(json, item, "integer", false);
        yyjson_mut_obj_add_str(json, item, "unit", "");
    }
}

/* ---------- Entities ---------- */

static game_prototype_registry registry()
{
    return (game_prototype_registry){ .components = GAME_REGISTRY_COMPONENTS,
                                      .component_count = GAME_REGISTRY_COMPONENT_COUNT };
}

static const ecs_component_desc *component_named(const ecs_world *world, const char *name)
{
    for (int i = 0; i < ecs_world_component_count(world); i++) {
        const ecs_component_desc *desc = ecs_world_component_at(world, i);
        const char *dot = desc ? strrchr(desc->name, '.') : NULL;
        if (desc && !strcmp(dot ? dot + 1 : desc->name, name)) {
            return desc;
        }
    }
    return NULL;
}

static int move_field(char *data, const type_field *f, float delta)
{
    int floats = f->type == PIN_F32    ? 1
               : f->type == PIN_STRUCT ? vector_axes(f->type_name)
                                       : 0;
    float *at = (float *)(data + f->offset);
    for (int i = 0; i < floats; i++) {
        at[i] += delta;
    }
    return floats > 0;
}

int bench_move_floats(ecs_world *world, ecs_entity entity, float delta)
{
    int moved = 0;
    for (int i = 0; i < ecs_world_component_count(world); i++) {
        const ecs_component_desc *desc = ecs_world_component_at(world, i);
        if (!desc || !desc->type || !ecs_component_has(world, entity, desc->id)) {
            continue;
        }
        char *data = ecs_component_data(world, entity, desc->id);
        for (int f = 0; f < desc->type->field_count; f++) {
            moved += move_field(data, &desc->type->fields[f], delta);
        }
    }
    return moved;
}

void bench_edit_float(ecs_world *world, ecs_entity entity, const char *component, const char *field, float delta)
{
    const ecs_component_desc *desc = component_named(world, component);
    assert(desc && desc->type);
    const type_field *f = pin_type_field_path(desc->type, field);
    assert(f && f->type == PIN_F32);
    char *data = ecs_component_data(world, entity, desc->id);
    *(float *)(data + f->offset) += delta;
}

static ecs_entity spawn(ecs_world *world, const char *prototype)
{
    char path[256], error[256];
    snprintf(path, sizeof path, ARENA_CONTENT "prototypes/%s.kdl", prototype);
    char *text = text_file_read(path);
    assert(text);
    game_prototype_registry components = registry();
    game_prototype *loaded = game_prototype_load(text, &components, error, sizeof error);
    assert(loaded);
    ecs_entity entity;
    int spawned = game_prototype_spawn_components(loaded, world, &entity, error, sizeof error);
    assert(spawned == 0);
    game_prototype_free(loaded);
    free(text);
    return entity;
}

/* ---------- 3: instancing rows ---------- */

static void instancing()
{
    bench_spike *s = bench_spike_new(AREA_WIDTH, AREA_HEIGHT);
    double ms[ARMS][RUNS];
    bench_counts counts[ARMS];
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            spike_fill(s, arm ? 0 : 50);
            bench_counts before = doc_mark(&s->doc);
            double t = update_time(&s->doc, "properties");
            counts[arm] = doc_counts(&s->doc, before);
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    bench_row("3", "50 property rows instanced (0 to 50)", bench_stat_of(ms[0], RUNS), counts[0]);
    bench_row("3", "50 property rows removed (50 to 0)", bench_stat_of(ms[1], RUNS), counts[1]);
    printf(
        "|  | (each row: %d elements) | | | | | | |\n",
        (counts[0].elements - counts[1].elements) / 50
    );
    bench_spike_free(s);
}

/* ---------- 4: a list window ---------- */

#define LIST_ROWS 10000
#define WINDOW 60
#define ROW_DP 22

static const char OUTLINER[] =
    "<div style=\"position: absolute; left: 0dp; top: 0dp; right: 0dp; bottom: "
    "0dp; "
    "overflow-y: auto;\">"
    "<div style=\"position: relative;\" data-style-height=\"outliner.height\">"
    "<div style=\"position: absolute; left: 0dp; right: 0dp;\" "
    "data-style-top=\"outliner.top\">"
    "<div class=\"tree-row\" data-for=\"row : outliner.rows\" "
    "data-class-selected=\"row.selected\" "
    "data-style-padding-left=\"row.indent\">"
    "<span class=\"icon\">{{ row.icon }}</span><span class=\"label\">{{ "
    "row.label }}</span>"
    "<span class=\"icon eye\" data-class-off=\"row.hidden\">&#xea70;</span>"
    "<span class=\"icon lock\" data-class-on=\"row.locked\">&#xea75;</span>"
    "</div></div></div></div>";

typedef struct window_row
{
    yyjson_mut_val *label, *icon, *indent, *selected, *hidden, *locked;
} window_row;

/* Shows rows `first` on in the window, changing values in place. */
static void show_window(yyjson_mut_doc *json, yyjson_mut_val *top, window_row *rows, int first)
{
    static const char *const ICONS[] = { "\xee\xaa\xbc", "\xee\xac\x96", "\xee\xa9\xbb" };
    char text[64];
    for (int i = 0; i < WINDOW; i++) {
        int row = first + i;
        snprintf(text, sizeof text, "Swarmer %05d", row);
        ui_data_set_text(json, rows[i].label, text);
        ui_data_set_text(json, rows[i].icon, ICONS[row % 3]);
        snprintf(text, sizeof text, "%ddp", 4 + 12 * (row % 4));
        ui_data_set_text(json, rows[i].indent, text);
        yyjson_mut_set_bool(rows[i].selected, row % 17 == 0);
        yyjson_mut_set_bool(rows[i].hidden, row % 5 == 0);
        yyjson_mut_set_bool(rows[i].locked, row % 7 == 0);
    }
    snprintf(text, sizeof text, "%ddp", first * ROW_DP);
    ui_data_set_text(json, top, text);
}

static void scroll_step()
{
    yyjson_mut_val *root;
    yyjson_mut_doc *json = json_new(&root);
    yyjson_mut_val *outliner = yyjson_mut_obj_add_obj(json, root, "outliner");
    char height[32];
    snprintf(height, sizeof height, "%ddp", LIST_ROWS * ROW_DP);
    yyjson_mut_obj_add_strcpy(json, outliner, "height", height);
    yyjson_mut_val *top = yyjson_mut_strcpy(json, "0dp");
    yyjson_mut_obj_add_val(json, outliner, "top", top);
    yyjson_mut_val *items = yyjson_mut_obj_add_arr(json, outliner, "rows");
    window_row rows[WINDOW];
    for (int i = 0; i < WINDOW; i++) {
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(json, items);
        rows[i] = (window_row){
            yyjson_mut_strcpy(json, ""),  yyjson_mut_strcpy(json, ""),
            yyjson_mut_strcpy(json, ""),  yyjson_mut_bool(json, false),
            yyjson_mut_bool(json, false), yyjson_mut_bool(json, false)
        };
        yyjson_mut_obj_add_val(json, item, "label", rows[i].label);
        yyjson_mut_obj_add_val(json, item, "icon", rows[i].icon);
        yyjson_mut_obj_add_val(json, item, "indent", rows[i].indent);
        yyjson_mut_obj_add_val(json, item, "selected", rows[i].selected);
        yyjson_mut_obj_add_val(json, item, "hidden", rows[i].hidden);
        yyjson_mut_obj_add_val(json, item, "locked", rows[i].locked);
    }
    show_window(json, top, rows, 0);
    bench_doc d;
    doc_open(&d, json, true, AREA_WIDTH, AREA_HEIGHT);
    doc_show(&d, "", OUTLINER);
    double ms[RUNS];
    bench_counts counts = { 0 };
    int first = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        first = (first + 3) % (LIST_ROWS - WINDOW);
        bench_counts before = doc_mark(&d);
        double t = bench_now();
        show_window(json, top, rows, first);
        rml_data_model_dirty(d.data.model, "outliner");
        rml_backend_update(d.backend, 0);
        t = bench_now() - t;
        counts = doc_counts(&d, before);
        if (run >= 0) {
            ms[run] = t;
        }
    }
    bench_row("4", "scroll step: 60-row window over 10k rows", bench_stat_of(ms, RUNS), counts);
    doc_close(&d);
}

/* ---------- 5: the Properties Object tab, authored ---------- */

/* The spike over the Player and a Swarmer spawned from their prototypes:
 * a selection change rebuilds the rows; an edit changes one value and a
 * Play refresh every float, and both refresh every shown value. */
static void properties_headless()
{
    ecs_world *world = ecs_world_new();
    for (int i = 0; i < GAME_REGISTRY_COMPONENT_COUNT; i++) {
        int registered = ecs_world_register_component(world, GAME_REGISTRY_COMPONENTS[i], NULL);
        assert(registered == ECS_OK);
    }
    ecs_entity player = spawn(world, "Player"), swarmer = spawn(world, "Swarmer");
    char *text = text_file_read(ARENA_CONTENT COMPONENT_PRESENTATION_PATH),
         error[256];
    component_presentation tabs = { 0 };
    game_prototype_registry components = registry();
    bool presented = text && component_presentation_read(
                                 text,
                                 (ecs_component_catalog){ components.components,
                                                          components.component_count },
                                 &tabs,
                                 error,
                                 sizeof error
                             );
    assert(presented);
    free(text);
    bench_spike *s = bench_spike_new(AREA_WIDTH, AREA_HEIGHT);
    double ms[ARMS][RUNS];
    bench_counts counts[ARMS];
    int rows[ARMS];
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            bench_counts before = bench_spike_counts(s);
            double t = bench_now();
            bench_spike_show(s, world, arm ? player : swarmer, arm ? "Player" : "Swarmer", &tabs);
            rml_backend_update(s->doc.backend, 0);
            t = bench_now() - t;
            counts[arm] = doc_counts(&s->doc, before);
            rows[arm] = s->rows;
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    char name[96];
    snprintf(name, sizeof name, "authored: select the Swarmer (%d rows)", rows[0]);
    bench_row("5", name, bench_stat_of(ms[0], RUNS), counts[0]);
    snprintf(name, sizeof name, "authored: select the Player (%d rows)", rows[1]);
    bench_row("5", name, bench_stat_of(ms[1], RUNS), counts[1]);
    int moved = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            if (arm) {
                moved = bench_move_floats(world, player, run % 2 ? 0.25f : -0.25f);
            } else {
                bench_edit_float(world, player, "GameAppearance", "width", (run & 1) ? 1 : -1);
            }
            bench_counts before = bench_spike_counts(s);
            double t = bench_now();
            bench_spike_refresh(s);
            rml_backend_update(s->doc.backend, 0);
            t = bench_now() - t;
            counts[arm] = doc_counts(&s->doc, before);
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    bench_row("5", "authored: edit one field of the Player", bench_stat_of(ms[0], RUNS), counts[0]);
    snprintf(name, sizeof name, "authored: Play refresh, %d float fields changed", moved);
    bench_row("5", name, bench_stat_of(ms[1], RUNS), counts[1]);
    bench_spike_free(s);
    component_presentation_free(&tabs);
    ecs_world_free(world);
}

/* ---------- 6: highlighted Output rows ---------- */

#define OUTPUT_ROWS 40

static const char
    OUTPUT_MARKUP[] = "<div class=\"output-list\" style=\"position: absolute; "
                      "left: 0dp; top: 0dp; right: "
                      "0dp; bottom: 0dp; overflow: hidden;\">"
                      "<div class=\"output-row\" data-for=\"row : "
                      "output.rows\"><span class=\"output-line\" "
                      "data-rml=\"row.markup\"/></div></div>";
static const char OUTPUT_TEXT
    [] = "<div class=\"output-list\" style=\"position: absolute; left: 0dp; "
         "top: 0dp; right: "
         "0dp; bottom: 0dp; overflow: hidden;\">"
         "<div class=\"output-row\" data-for=\"row : output.rows\"><span "
         "class=\"output-line\">{{ row.text }}</span></div></div>";

/* Output following a log: every row shows the next line, as markup that
 * data-rml parses, or as plain text for comparison. */
static void output_rows()
{
    yyjson_mut_val *root;
    yyjson_mut_doc *json[ARMS];
    yyjson_mut_val *shown[ARMS][OUTPUT_ROWS];
    bench_doc d[ARMS];
    for (int arm = 0; arm < ARMS; arm++) {
        json[arm] = json_new(&root);
        yyjson_mut_val *rows = yyjson_mut_obj_add_arr(
            json[arm],
            yyjson_mut_obj_add_obj(json[arm], root, "output"),
            "rows"
        );
        for (int i = 0; i < OUTPUT_ROWS; i++) {
            shown[arm][i] = yyjson_mut_strcpy(json[arm], "");
            yyjson_mut_obj_add_val(
                json[arm],
                yyjson_mut_arr_add_obj(json[arm], rows),
                arm ? "text" : "markup",
                shown[arm][i]
            );
        }
        doc_open(&d[arm], json[arm], true, WIDTH, HEIGHT / 2);
        doc_show(&d[arm], "", arm ? OUTPUT_TEXT : OUTPUT_MARKUP);
    }
    double ms[ARMS][RUNS];
    bench_counts counts[ARMS];
    int line = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        line++;
        for (int arm = 0; arm < ARMS; arm++) {
            bench_counts before = doc_mark(&d[arm]);
            double t = bench_now();
            for (int i = 0; i < OUTPUT_ROWS; i++) {
                char text[256], markup[1024];
                snprintf(
                    text,
                    sizeof text,
                    "[%06d] warning: Player hit \"Wall %d\" at (%d.5, -%d.25) "
                    "for %d damage in "
                    "content/behaviors/Weapon.chibi",
                    line + i,
                    i,
                    line,
                    i,
                    i % 9
                );
                highlight_markup(HIGHLIGHT_LOG, text, markup, sizeof markup);
                ui_data_set_text(json[arm], shown[arm][i], arm ? text : markup);
            }
            rml_data_model_dirty(d[arm].data.model, "output");
            rml_backend_update(d[arm].backend, 0);
            t = bench_now() - t;
            counts[arm] = doc_counts(&d[arm], before);
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    bench_stat markup = bench_stat_of(ms[0], RUNS), text = bench_stat_of(ms[1], RUNS);
    bench_row("6", "40 Output rows as highlighted data-rml, all new", markup, counts[0]);
    bench_row("6", "40 Output rows as plain text, all new", text, counts[1]);
    bench_ratio("6", "data-rml over plain text", text, markup);
    for (int arm = 0; arm < ARMS; arm++) {
        doc_close(&d[arm]);
    }
}

/* ---------- 8: array item lookups ---------- */

/* Every item of an n-item array by index, as a dirty list's views look
 * them up: yyjson's walk against the listed items. */
static void item_lookups(int count)
{
    yyjson_mut_doc *json = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *array = yyjson_mut_arr(json);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_int(json, array, i);
    }
    json_items *items = json_items_new();
    double ms[ARMS][RUNS];
    int64_t sum = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            json_items_clear(items);
            double t = bench_now();
            for (int i = 0; i < count; i++) {
                yyjson_mut_val *item = arm ? json_items_get(items, array, (size_t)i)
                                           : yyjson_mut_arr_get(array, (size_t)i);
                sum += yyjson_mut_get_int(item);
            }
            t = bench_now() - t;
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    assert(sum == (int64_t)(RUNS + WARMUP) * ARMS * count * (count - 1) / 2);
    char name[96];
    bench_stat walk = bench_stat_of(ms[0], RUNS), listed = bench_stat_of(ms[1], RUNS);
    snprintf(name, sizeof name, "look up %d items: yyjson walk", count);
    bench_row("8", name, walk, (bench_counts){ .lookups = (uint64_t)count });
    snprintf(name, sizeof name, "look up %d items: json_items (after a clear)", count);
    bench_row("8", name, listed, (bench_counts){ .lookups = (uint64_t)count });
    snprintf(name, sizeof name, "json_items over walk, %d items", count);
    bench_ratio("8", name, walk, listed);
    json_items_free(items);
    yyjson_mut_doc_free(json);
}

/* ---------- 9: the List widget (workstream F4) ---------- */

/* projects/studio's List of TreeRows over ten thousand rows, which
 * ui_list_window windows as a panel source does (spike/ListSpike). */
#define TREE_ROWS 10000

typedef struct list_spike
{
    bench_doc doc;
    ui_mount *mount;
    rml_listener *listener;
    rml_element *list;
    yyjson_mut_val *rows;
    ui_list_state state;
} list_spike;

static yyjson_mut_val *tree_item(yyjson_mut_doc *json)
{
    static const char *const TEXTS[] = { "label", "icon", "meta", "drag", "name" };
    static const char *const FLAGS[] = { "parent", "open",   "selected",
                                         "active", "marked", "hidden" };
    yyjson_mut_val *item = yyjson_mut_obj(json);
    for (size_t i = 0; i < sizeof TEXTS / sizeof *TEXTS; i++) {
        yyjson_mut_obj_add_strcpy(json, item, TEXTS[i], "");
    }
    for (size_t i = 0; i < sizeof FLAGS / sizeof *FLAGS; i++) {
        yyjson_mut_obj_add_bool(json, item, FLAGS[i], false);
    }
    yyjson_mut_obj_add_int(json, item, "depth", 0);
    return item;
}

/* Shows row `row` in `item`, changing its values in place. */
static void tree_show(yyjson_mut_doc *json, yyjson_mut_val *item, int row)
{
    static const char *const ICONS[] = { "\xee\xaa\xbc", "\xee\xac\x96", "\xee\xa9\xbb" };
    char text[64];
    snprintf(text, sizeof text, "Swarmer %05d", row);
    ui_data_set_text(json, yyjson_mut_obj_get(item, "label"), text);
    ui_data_set_text(json, yyjson_mut_obj_get(item, "name"), text);
    ui_data_set_text(json, yyjson_mut_obj_get(item, "icon"), ICONS[row % 3]);
    ui_data_set_text(json, yyjson_mut_obj_get(item, "meta"), row % 4 ? "" : "3");
    ui_data_set_text(json, yyjson_mut_obj_get(item, "drag"), "prototype");
    yyjson_mut_set_int(yyjson_mut_obj_get(item, "depth"), row % 4);
    yyjson_mut_set_bool(yyjson_mut_obj_get(item, "parent"), row % 4 == 0);
    yyjson_mut_set_bool(yyjson_mut_obj_get(item, "selected"), row % 17 == 0);
    yyjson_mut_set_bool(yyjson_mut_obj_get(item, "hidden"), row % 5 == 0);
}

/* Rows `first` on, `length` of them: the array grows and shrinks at its
 * end, and the rows it keeps change their values. */
static void tree_window(list_spike *l, int first, int length)
{
    yyjson_mut_doc *json = l->doc.data.doc;
    while ((int)yyjson_mut_arr_size(l->rows) < length) {
        yyjson_mut_arr_append(l->rows, tree_item(json));
    }
    while ((int)yyjson_mut_arr_size(l->rows) > length) {
        yyjson_mut_arr_remove_last(l->rows);
    }
    yyjson_mut_val *item;
    yyjson_mut_arr_iter it = yyjson_mut_arr_iter_with(l->rows);
    for (int row = first; (item = yyjson_mut_arr_iter_next(&it)); row++) {
        tree_show(json, item, row);
    }
}

static void list_spike_open(list_spike *l)
{
    yyjson_mut_val *root;
    yyjson_mut_doc *json = json_new(&root);
    l->rows = yyjson_mut_obj_add_arr(json, yyjson_mut_obj_add_obj(json, root, "bench"), "rows");
    doc_open(&l->doc, json, false, AREA_WIDTH, AREA_HEIGHT);
    doc_show(&l->doc, "", SPIKE_HOST);
    l->mount = authoring_ui_mount_new(STUDIO_CONTENT, "spike/ListSpike");
    l->listener = rml_listener_create(on_key, NULL);
    ui_mount_sync(l->mount, rml_element_by_id(l->doc.doc, "host"), l->listener, 0);
    assert(!ui_mount_error(l->mount)[0]);
    doc_show(&l->doc, ui_mount_style(l->mount), NULL);
    l->list = rml_element_by_id(l->doc.doc, "bench-list");
    assert(l->list);
    l->state = (ui_list_state)UI_LIST_STATE_INIT;
}

static void list_spike_close(list_spike *l)
{
    ui_mount_free(l->mount);
    doc_close(&l->doc);
    rml_listener_destroy(l->listener);
}

/* 50 TreeRows made and removed, as case 3 makes PropertyRows. */
static void tree_instancing(list_spike *l)
{
    double ms[ARMS][RUNS];
    bench_counts counts[ARMS];
    for (int run = -WARMUP; run < RUNS; run++) {
        for (int arm = 0; arm < ARMS; arm++) {
            tree_window(l, 0, arm ? 0 : 50);
            bench_counts before = doc_mark(&l->doc);
            double t = update_time(&l->doc, "bench");
            counts[arm] = doc_counts(&l->doc, before);
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    bench_row("9", "50 TreeRows instanced (0 to 50)", bench_stat_of(ms[0], RUNS), counts[0]);
    bench_row("9", "50 TreeRows removed (50 to 0)", bench_stat_of(ms[1], RUNS), counts[1]);
    printf(
        "|  | (each row: %d elements) | | | | | | |\n",
        (counts[0].elements - counts[1].elements) / 50
    );
}

/* Shows the window, dirtying and updating once, or emptying it first when
 * `anew`, as a list that keeps no rows does. */
static void tree_step(list_spike *l, int first, int length, bool anew)
{
    if (anew) {
        tree_window(l, first, 0);
        rml_data_model_dirty(l->doc.data.model, "bench");
        rml_backend_update(l->doc.backend, 0);
    }
    tree_window(l, first, length);
    rml_data_model_dirty(l->doc.data.model, "bench");
    rml_backend_update(l->doc.backend, 0);
}

/* A scroll step of three rows, arms alternated run by run, each in a
 * document of its own: the window ui_list_window gives, whose rows keep
 * their elements, against the same window made anew each step. */
static void tree_scroll(list_spike arms[ARMS])
{
    double ms[ARMS][RUNS];
    bench_counts counts[ARMS];
    ui_list desc = { .count = TREE_ROWS, .reveal = -1 };
    int first = 0;
    float row = 0;
    for (int arm = 0; arm < ARMS; arm++) {
        ui_list_window(arms[arm].list, &desc, &arms[arm].state, &first);
        rml_backend_update(arms[arm].doc.backend, 0);
        char value[32];
        rml_element_property(arms[arm].list, "--row", value, sizeof value);
        row = strtof(value, NULL);
    }
    int step = 0;
    for (int run = -WARMUP; run < RUNS; run++) {
        step = (step + 3) % (TREE_ROWS - 100);
        for (int arm = 0; arm < ARMS; arm++) {
            list_spike *l = &arms[arm];
            bench_counts before = doc_mark(&l->doc);
            double t = bench_now();
            rml_element_set_scroll_top(l->list, step * row);
            int length = ui_list_window(l->list, &desc, &l->state, &first);
            tree_step(l, first, length, arm == 1);
            t = bench_now() - t;
            counts[arm] = doc_counts(&l->doc, before);
            if (run >= 0) {
                ms[arm][run] = t;
            }
        }
    }
    bench_stat kept = bench_stat_of(ms[0], RUNS), anew = bench_stat_of(ms[1], RUNS);
    bench_row("9", "scroll step, List window of TreeRows over 10k rows", kept, counts[0]);
    bench_row("9", "the same window made anew each step", anew, counts[1]);
    bench_ratio("9", "made anew over kept rows", kept, anew);
}

static void tree_list()
{
    list_spike arms[ARMS];
    for (int arm = 0; arm < ARMS; arm++) {
        list_spike_open(&arms[arm]);
    }
    tree_instancing(&arms[0]);
    tree_step(&arms[0], 0, 0, false);
    tree_scroll(arms);
    for (int arm = 0; arm < ARMS; arm++) {
        list_spike_close(&arms[arm]);
    }
}

/* ---------- Run ---------- */

static void print_load()
{
#ifdef __EMSCRIPTEN__
    printf("Load: see the host's uptime\n");
#else
    double load[3] = { 0 };
    getloadavg(load, 3);
    printf("Load averages: %.2f %.2f %.2f\n", load[0], load[1], load[2]);
#endif
}

int ui_bench_headless()
{
    /* Keeps RmlUi and its fonts loaded while cases open and close contexts. */
    rml_backend *keeper = rml_backend_create(64, 64, 1);
    print_load();
    bench_header("Headless: RmlUi and the JSON bridge, Studio theme, dp ratio 1");
    dirty_cost(300, false);
    dirty_cost(3000, false);
    dirty_cost(300, true);
    dirty_cost(3000, true);
    instancing();
    scroll_step();
    properties_headless();
    output_rows();
    item_lookups(300);
    item_lookups(3000);
    tree_list();
    print_load();
    ui_page_free(&theme);
    rml_backend_destroy(keeper);
    rml_instancer_destroy(probe_instancer);
    probe_instancer = NULL;
    return 0;
}
