/* Number fields: the kit's face of the native <number-field>
 * (ui/controls/controls.h), which scrubs, steps, takes typed expressions and
 * repaints its value without laying out the document. The kit writes the
 * desc as attributes and takes what the field's gestures set. */
#include "editor/rmlui/kit_internal.h"
#include "ui/controls/controls.h"

#include <stdio.h>

/* Writes a number attribute so it reads back exactly. */
static void number_attr(node *n, const char *name, double value)
{
    char text[32];
    snprintf(text, sizeof text, "%.17g", value);
    set_attr(n, name, text);
}

static void text_attr(node *n, const char *name, const char *text)
{
    if (text && text[0]) {
        set_attr(n, name, text);
        return;
    }
    set_flag(n, name, false);
}

static void bounds(node *n, const kit_number_desc *d)
{
    bool bounded = d->min != 0 || d->max != 0;
    if (bounded) {
        number_attr(n, "min", d->min);
        number_attr(n, "max", d->max);
        return;
    }
    set_flag(n, "min", false);
    set_flag(n, "max", false);
}

bool kit_number(kit *k, const char *key, double *value, const kit_number_desc *d)
{
    node *n = emit(k, key, KIND_NUMBER, "number-field", "field number");
    widget *w = &n->w;
    w->desc = *d;
    str_set(&w->addon, d->addon);
    double taken = *value;
    bool committed;
    bool changed = number_field_take(n->el, &taken, &committed);
    if (changed) {
        *value = taken;
    }
    w->committed = committed;
    number_attr(n, "value", d->single_precision ? (double)(float)*value : *value);
    bounds(n, d);
    number_attr(n, "step", d->step);
    number_attr(n, "precision", d->precision);
    set_flag(n, "integer", d->integer);
    set_flag(n, "float", d->single_precision);
    set_flag(n, "slider", d->slider);
    text_attr(n, "unit", d->unit);
    text_attr(n, "addon", d->addon);
    text_attr(n, "data-axis", d->addon_cls);
    /* The caller writes a changed value everywhere this frame. */
    flag_element(n->el, "mixed", (d->flags & KIT_MIXED) && !changed);
    set_flag(n, "readonly", d->flags & KIT_READONLY);
    set_flag(n, "disabled", d->flags & KIT_DISABLED);
    set_flag(n, "invalid", d->flags & KIT_INVALID);
    set_flag(n, "data-overridden", d->flags & KIT_OVERRIDDEN);
    return changed;
}
