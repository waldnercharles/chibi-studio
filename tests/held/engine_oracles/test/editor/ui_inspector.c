/* The UI Inspector: the binding calls it reads documents with, its kit
 * overlay and element paths, and the model it lists for a small document. */
#include "editor/studio/studio_internal.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <math.h>
#include <string.h>

kit *test_kit_create(void *context, const char *document);

static const char
    DOC[] = "<rml><head><style>\n"
            "body { font-family: Inter; font-size: 12dp; color: #102030; }\n"
            ".card { display: block; width: 100dp; height: 40dp; margin: 4dp; "
            "padding: 2dp; }\n"
            "#title.card { color: #ff0000; }\n"
            "</style></head><body>"
            "<div id=\"title\" class=\"card big\" data-ui=\"Card\" "
            "data-src=\"Card:3\">Hello <span>world</span></div>"
            "<p style=\"opacity: 0.5;\">Label</p>"
            "</body></rml>";

typedef struct found
{
    char name[64], value[64], rule[64];
    int line, specificity, count;
} found;

static void find_color(void *user, const rml_property_info *p)
{
    found *f = user;
    f->count++;
    if (!strcmp(p->name, f->name)) {
        snprintf(f->value, sizeof f->value, "%s", p->value);
        snprintf(f->rule, sizeof f->rule, "%s", p->rule);
        f->line = p->line;
        f->specificity = p->specificity;
    }
}

static void find_attribute(void *user, const char *name, const char *value)
{
    found *f = user;
    f->count++;
    if (!strcmp(name, f->name)) {
        snprintf(f->value, sizeof f->value, "%s", value);
    }
}

static bool has_word(const char *words, const char *word)
{
    size_t n = strlen(word);
    for (const char *at = strstr(words, word); at; at = strstr(at + 1, word)) {
        if ((at == words || at[-1] == ' ') && (at[n] == ' ' || !at[n])) {
            return true;
        }
    }
    return false;
}

/* Attributes, classes, pseudo classes, declared properties with their rules,
 * inherited ones, computed values, text and the element at a point. */
static bool reads_elements(rml_context *context, rml_element *doc)
{
    rml_element *title = rml_element_by_id(doc, "title");
    rml_element *body = rml_element_owner_document(title);
    bool listed = false;
    for (int i = 0; i < rml_context_document_count(context); i++) {
        listed |= rml_context_document(context, i) == doc;
    }
    char classes[64];
    rml_element_class_names(title, classes, sizeof classes);
    found ui = { .name = "data-ui" };
    rml_element_attributes(title, find_attribute, &ui);
    rml_element_set_pseudo_class(title, "hover", true);
    char pseudo[64];
    rml_element_pseudo_classes(title, pseudo, sizeof pseudo);
    bool forced = has_word(pseudo, "hover") &&
                  rml_element_is_pseudo_class_set(title, "hover");
    rml_element_set_pseudo_class(title, "hover", false);
    found color = { .name = "color" }, width = { .name = "width" },
          inherited = { .name = "font-size" };
    rml_element_local_properties(title, NULL, find_color, &color);
    rml_element_local_properties(title, NULL, find_color, &width);
    rml_element_local_properties(body, title, find_color, &inherited);
    char value[64], unknown[8] = "x";
    rml_element_property(title, "width", value, sizeof value);
    size_t missing = rml_element_property(title, "no-such-property", unknown, sizeof unknown);
    char text[64];
    rml_element_text(rml_element_child(title, 0), text, sizeof text);
    rml_vec2 at = rml_element_absolute_offset(title, RML_BOX_CONTENT);
    rml_element *hit = rml_context_element_at_point(context, (rml_vec2){ at.x + 80, at.y + 30 }, NULL);
    rml_element *ignored = rml_context_element_at_point(
        context,
        (rml_vec2){ at.x + 80, at.y + 30 },
        title
    );
    bool ok = listed && !strcmp(classes, "card big") &&
              !strcmp(ui.value, "Card") && ui.count >= 3 && forced &&
              !strcmp(color.rule, "#title.card") && !strcmp(width.rule, ".card") &&
              color.specificity > width.specificity && color.line > width.line &&
              width.line > 0 && !strcmp(inherited.value, "12dp") &&
              !strcmp(value, "100dp") && !missing && !unknown[0] &&
              !strncmp(text, "Hello", 5) && hit == title && ignored != title;
    if (!ok) {
        printf(
            "  classes '%s' ui '%s' pseudo '%s' color %s@%d/%d width %s@%d/%d "
            "inherited '%s' "
            "value '%s' text '%s' hit %d\n",
            classes,
            ui.value,
            pseudo,
            color.rule,
            color.line,
            color.specificity,
            width.rule,
            width.line,
            width.specificity,
            inherited.value,
            value,
            text,
            hit == title
        );
    }
    return ok;
}

static const studio_inspect_row *row_tagged(const studio_inspect_model *m, const char *tag, bool node)
{
    for (int i = 0; i < acount(m->rows); i++) {
        if (m->rows[i].node == node &&
            !strcmp(node ? m->rows[i].text : m->rows[i].tag, tag)) {
            return &m->rows[i];
        }
    }
    return NULL;
}

/* The tree reveals the selection; text nodes list by their text, an only
 * text child joins its element's row. */
static bool lists_tree(studio_inspect *in, rml_context *context, const kit *k, rml_element *doc)
{
    const studio_inspect_model *m = studio_inspect_read(in, context, k);
    bool closed = acount(m->rows) == 1 && m->rows[0].parent &&
                  !m->rows[0].open && !m->selection;
    rml_element *title = rml_element_by_id(doc, "title");
    studio_inspect_select(in, rml_element_child(title, 1));
    m = studio_inspect_read(in, context, k);
    const studio_inspect_row *div = row_tagged(m, "div", false);
    const studio_inspect_row *hello = row_tagged(m, "Hello", true);
    const studio_inspect_row *span = row_tagged(m, "span", false);
    const studio_inspect_row *p = row_tagged(m, "p", false);
    bool ok = closed && acount(m->rows) == 5 && m->rows[0].open && div &&
              div->open && div->depth == 1 && !strcmp(div->id, "#title") &&
              !strcmp(div->classes, ".card.big") &&
              !strcmp(div->widget, "Card") && hello && hello->depth == 2 &&
              span && m->selected_row == span - m->rows && !span->parent &&
              !strcmp(span->text, "world") && p && !p->parent &&
              !strcmp(p->text, "Label") && !strcmp(m->title, "span") &&
              m->selection && !m->detached;
    if (!ok) {
        printf("  rows %d title '%s'\n", acount(m->rows), m->title);
    }
    return ok;
}

static const studio_inspect_style *style_named(const studio_inspect_model *m, const char *name, bool inherited)
{
    for (int i = 0; i < acount(m->styles); i++) {
        const studio_inspect_style *s = &m->styles[i];
        if (!s->heading && s->inherited == inherited && !strcmp(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

/* Rules in cascade order with their declarations, inherited ones under the
 * ancestor they come from; the box, source and attributes. */
static bool describes_selection(studio_inspect *in, rml_context *context, const kit *k, rml_element *doc)
{
    studio_inspect_select(in, rml_element_by_id(doc, "title"));
    const studio_inspect_model *m = studio_inspect_read(in, context, k);
    const studio_inspect_style *first = acount(m->styles) ? &m->styles[0] : NULL;
    bool ancestor = false;
    for (int i = 0; i < acount(m->styles); i++) {
        ancestor |= m->styles[i].ancestor &&
                    !strcmp(m->styles[i].selector, "Inherited from body");
    }
    /* RmlUi lists what an ancestor gives that the element does not set. */
    bool styles = first && first->heading &&
                  !strcmp(first->selector, "#title.card") &&
                  !strcmp(first->origin, "line 4") &&
                  style_named(m, "width", false) &&
                  style_named(m, "margin-top", false) && ancestor &&
                  style_named(m, "font-size", true) &&
                  !style_named(m, "color", true);
    bool attributes = acount(m->attributes) == 4 &&
                      !strcmp(m->attributes[0].name, "id") &&
                      !strcmp(m->attributes[1].value, "card big") &&
                      !strcmp(m->attributes[2].name, "data-src") &&
                      !strcmp(m->attributes[3].name, "data-ui");
    bool box = !strcmp(m->box[0][0], "4") && !strcmp(m->box[1][0], "\xe2\x80\x93") &&
               !strcmp(m->box[2][3], "2") && !strcmp(m->size, "100 \xc3\x97 40");
    bool source = !strcmp(m->source, "Card:3") && m->editable && !m->native &&
                  !strcmp(m->title, "div#title.card.big");
    bool computed = false;
    for (int i = 0; i < acount(m->computed); i++) {
        computed |= !strcmp(m->computed[i].name, "width") &&
                    !strcmp(m->computed[i].value, "100dp");
    }
    /* The span inherits the nearer ancestor's color, not the body's. */
    studio_inspect_select(in, rml_element_child(rml_element_by_id(doc, "title"), 1));
    m = studio_inspect_read(in, context, k);
    int colors = 0, from_title = -1, from_body = -1;
    for (int i = 0; i < acount(m->styles); i++) {
        const studio_inspect_style *s = &m->styles[i];
        colors += !s->heading && !strcmp(s->name, "color");
        from_title = !strcmp(s->selector, "Inherited from div#title.card.big")
                       ? i
                       : from_title;
        from_body = !strcmp(s->selector, "Inherited from body") ? i : from_body;
    }
    bool overrides = colors == 1 && from_title >= 0 && from_body > from_title;
    studio_inspect_select(in, rml_element_child(doc, 1));
    m = studio_inspect_read(in, context, k);
    bool inline_style = acount(m->styles) >= 2 &&
                        !strcmp(m->styles[0].selector, "style") &&
                        !strcmp(m->styles[1].name, "opacity");
    bool ok = styles && overrides && attributes && box && source && computed &&
              inline_style;
    if (!ok) {
        printf(
            "  styles %d overrides %d attributes %d box %d source %d computed "
            "%d inline %d\n",
            styles,
            overrides,
            attributes,
            box,
            source,
            computed,
            inline_style
        );
    }
    return ok;
}

/* A selection whose element leaves the document keeps its description. */
static bool detaches(studio_inspect *in, rml_context *context, const kit *k, rml_element *doc)
{
    rml_element *title = rml_element_by_id(doc, "title");
    studio_inspect_select(in, title);
    studio_inspect_read(in, context, k);
    rml_element_remove_child(doc, title);
    rml_context_update(context);
    const studio_inspect_model *m = studio_inspect_read(in, context, k);
    return m->detached && !strcmp(m->title, "div#title.card.big") &&
           !row_tagged(m, "div", false);
}

/* Kit paths name emitted nodes by key and hosted content by its steps; the
 * highlight places its bands at the boxes it is given. */
static bool marks_kit_elements(rml_context *context)
{
    kit *k = test_kit_create(context, "studio.rml");
    kit_frame_input input = { .dp_ratio = 1, .width = 640, .height = 480 };
    kit_begin(k, &input);
    kit_root(k, "app");
    rml_element *host = kit_host(k, "host", NULL);
    rml_element *child = rml_element_create_child(host, "div", NULL);
    rml_element_set_class_names(child, "mounted");
    kit_highlight_desc d = { .margin = { 10, 20, 100, 50 },
                             .border = { 12, 22, 96, 46 },
                             .padding = { 13, 23, 94, 44 },
                             .content = { 15, 25, 90, 40 },
                             .label = "div 96 \xc3\x97 46" };
    rml_element *cover = kit_inspect_begin(k, "inspect", true);
    kit_highlight(k, "hover", &d, "hovered");
    kit_inspect_end(k);
    kit_end(k);
    rml_context_update(context);
    char host_path[128], child_path[128];
    bool native = kit_element_path(k, host, host_path, sizeof host_path);
    bool hosted = kit_element_path(k, child, child_path, sizeof child_path);
    kit_rect content;
    bool placed = kit_find(k, "layer/inspect/hover/content", &content) &&
                  fabsf(content.x - 15) < 0.5f && fabsf(content.y - 25) < 0.5f &&
                  fabsf(content.w - 90) < 0.5f && fabsf(content.h - 40) < 0.5f;
    bool ok = cover && native && !strcmp(host_path, "app/host") && !hosted &&
              !strcmp(child_path, "app/host \xe2\x80\xba div.mounted") && placed;
    if (!ok) {
        printf("  host '%s' %d child '%s' %d placed %d\n", host_path, native, child_path, hosted, placed);
    }
    kit_destroy(k);
    return ok;
}

void test_ui_inspector()
{
    rml_backend *backend = rml_backend_create(640, 480, 1);
    rml_context *context = rml_backend_context(backend);
    rml_element *doc = rml_context_load_document_from_memory(context, DOC, "[inspector]");
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_NONE);
    rml_context_update(context);
    test_check(reads_elements(context, doc), "the RmlUi binding reads attributes, pseudo classes, declared, inherited and computed properties, text and the element at a point");
    rml_backend *studio_backend = rml_backend_create(640, 480, 1);
    rml_context *studio_context = rml_backend_context(studio_backend);
    test_check(marks_kit_elements(studio_context), "the kit names its elements by key path and places the Inspector's box-model highlight");
    kit *k = test_kit_create(studio_context, "studio.rml");
    studio_inspect *in = studio_inspect_new();
    test_check(lists_tree(in, context, k, doc), "the Inspector's tree reveals the selection and lists text nodes by their text");
    test_check(describes_selection(in, context, k, doc), "the Inspector groups styles by rule in cascade order, lists inherited ones under their ancestor and reads the box, source and attributes");
    test_check(detaches(in, context, k, doc), "the Inspector keeps describing a selection whose element left its document");
    studio_inspect_free(in);
    kit_destroy(k);
    rml_backend_destroy(studio_backend);
    rml_document_close(doc);
    rml_backend_destroy(backend);
}
