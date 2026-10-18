#include "editor/studio/studio_outliner_work.h"

#include "support/xalloc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *const
    invalid = "Outliner captured workload dimensions are invalid.";
static const char *const
    overflow = "Outliner workload exceeds the representable request allowance.";
static const char *const
    text_range = "Outliner captured text exceeds the authored byte range.";

static bool add(uintptr_t a, uintptr_t b, uintptr_t *out)
{
    if (a > UINTPTR_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool multiply(uintptr_t a, uintptr_t b, uintptr_t *out)
{
    if (b && a > UINTPTR_MAX / b) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool dimension(int value, uintptr_t *out)
{
    if (value < 0 || (uintmax_t)value > UINTPTR_MAX) {
        return false;
    }
    *out = (uintptr_t)value;
    return true;
}

static const char *text_length(const char *text, uintptr_t *out)
{
    if (!text) {
        return invalid;
    }
    size_t length = strlen(text);
    if (length > INT_MAX || length > UINTPTR_MAX) {
        return text_range;
    }
    *out = (uintptr_t)length;
    return NULL;
}

static bool matching(studio_outliner_work *work, uintptr_t length)
{
    if (!add(work->actor_text_bytes, length, &work->actor_text_bytes)) {
        return false;
    }
    uintptr_t query = work->filter_bytes;
    if (!query || query > length) {
        return true;
    }
    uintptr_t starts, pairs;
    return add(length - query, 1, &starts) && multiply(starts, query, &pairs) &&
           add(work->match_starts, starts, &work->match_starts) &&
           add(work->match_pairs, pairs, &work->match_pairs);
}

static bool span(int first, int count, uintptr_t total, uintptr_t *cursor, uintptr_t *maximum)
{
    uintptr_t begin, amount;
    if (!dimension(first, &begin) || !dimension(count, &amount) ||
        begin != *cursor || begin > total || amount > total - begin) {
        return false;
    }
    if (!add(begin, amount, cursor)) {
        return false;
    }
    if (amount > *maximum) {
        *maximum = amount;
    }
    return true;
}

const char *studio_outliner_work_measure(
    const studio_scene_read_facts *facts,
    int first,
    int count,
    bool collapsed,
    const char *reveal,
    studio_outliner_work *output
)
{
    studio_outliner_work work = { 0 };
    if (!dimension(facts->actor_count, &work.actors) ||
        !dimension(facts->prototype_count, &work.prototypes) ||
        !dimension(facts->component_count, &work.components) ||
        !dimension(facts->field_count, &work.fields) ||
        !dimension(facts->attachment_count, &work.attachments) ||
        (work.actors && (!facts->nodes || !facts->actors)) ||
        (work.prototypes && !facts->prototypes) ||
        (work.components && !facts->components) || (work.fields && !facts->fields) ||
        (work.attachments && !facts->attachments)) {
        return invalid;
    }
    if (facts->actor_count == INT_MAX) {
        return "Outliner actor count leaves no space for its virtual root.";
    }
    const char *error = text_length(facts->filter, &work.filter_bytes);
    if (!error) {
        error = text_length(facts->scene_path, &work.path_bytes);
    }
    if (!error) {
        error = text_length(reveal, &work.reveal_bytes);
    }
    if (error) {
        return error;
    }
    /* Page's existing root-exclusion arithmetic: First <= 0 reserves one
     * requested item; First == 1 starts at actor zero; First >= 2 starts at
     * First - 1. Clamp before subtraction, including signed extrema. */
    uintptr_t start = 0, amount = 0;
    if ((first > 0 && !dimension(first - 1, &start)) ||
        (count > 0 && !dimension(count, &amount))) {
        return overflow;
    }
    if (start > work.actors) {
        start = work.actors;
    }
    if (count > 0 && first < 1) {
        amount--;
    }
    uintptr_t available = work.actors - start;
    work.visible = collapsed ? 0 : amount < available ? amount : available;

    uintptr_t components = 0;
    for (int i = 0; i < facts->prototype_count; i++) {
        const studio_scene_read_prototype *prototype = &facts->prototypes[i];
        if (!span(
                prototype->component_first,
                prototype->component_count,
                work.components,
                &components,
                &work.max_components
            )) {
            return invalid;
        }
    }
    if (components != work.components) {
        return invalid;
    }
    if (work.actors > SIZE_MAX / sizeof(uintptr_t)) {
        return overflow;
    }
    uintptr_t *depths = work.actors ? xmalloc((size_t)work.actors * sizeof *depths)
                                    : NULL;
    uintptr_t fields = 0;
    for (int i = 0; i < facts->actor_count; i++) {
        const studio_scene_read_actor *actor = &facts->actors[i];
        int parent = facts->nodes[i].parent;
        if (parent < -1 || parent >= i || actor->prototype < -1 ||
            actor->prototype >= facts->prototype_count ||
            !span(actor->field_first, actor->field_count, work.fields, &fields, &work.max_fields)) {
            error = invalid;
            break;
        }
        depths[i] = 0;
        if (parent >= 0 && !add(depths[parent], 1, &depths[i])) {
            error = overflow;
            break;
        }
        if (depths[i] > work.max_depth) {
            work.max_depth = depths[i];
        }
        uintptr_t name, prototype;
        error = text_length(facts->nodes[i].name, &name);
        if (!error) {
            error = text_length(actor->prototype_name, &prototype);
        }
        if (error) {
            break;
        }
        if (!matching(&work, name) || !matching(&work, prototype)) {
            error = overflow;
            break;
        }
    }
    free(depths);
    if (error) {
        return error;
    }
    if (fields != work.fields) {
        return invalid;
    }
    *output = work;
    return NULL;
}

static bool term(uintptr_t *sum, uintptr_t coefficient, uintptr_t count)
{
    uintptr_t product;
    return multiply(coefficient, count, &product) && add(*sum, product, sum);
}

bool studio_outliner_work_limit(const studio_outliner_work *work, uintptr_t canonical_weight, uintptr_t *output)
{
    if (!canonical_weight) {
        return false;
    }
    uintptr_t components, fields, depth, attachments;
    if (!multiply(work->visible, work->max_components, &components) ||
        !multiply(work->visible, work->max_fields, &fields) ||
        !multiply(work->visible, work->max_depth, &depth) ||
        !multiply(work->visible, work->attachments, &attachments)) {
        return false;
    }
    if (fields > work->fields) {
        fields = work->fields;
    }
    uintptr_t title = work->path_bytes < 127 ? work->path_bytes : 127;
    uintptr_t visits = 8, weighted, total;
    /* W = 8 + 3N + 8V + T + 3R + D + 2Cv + 2Fv + Av + l + min(l,127).
     * The two FoldByte invocations per attempted pair account for 3R.
     * Snapshot text/copying is native work and is not charged as authored loops. */
    if (!term(&visits, 3, work->actors) || !term(&visits, 8, work->visible) ||
        !add(visits, work->match_starts, &visits) ||
        !term(&visits, 3, work->match_pairs) || !add(visits, depth, &visits) ||
        !term(&visits, 2, components) || !term(&visits, 2, fields) ||
        !add(visits, attachments, &visits) ||
        !add(visits, work->path_bytes, &visits) || !add(visits, title, &visits) ||
        !multiply(canonical_weight, visits, &weighted) ||
        !add(STUDIO_OUTLINER_WORK_BOOTSTRAP, weighted, &total)) {
        return false;
    }
    *output = total;
    return true;
}
