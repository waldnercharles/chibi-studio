/* Text that changes in place: kit_number_flow and kit_morph emit the native
 * <number-flow> and <morph-text> elements authored UI uses (ui/motion.h),
 * which animate themselves. */
#include "editor/rmlui/kit_internal.h"

void kit_number_flow(kit *k, const char *key, const char *cls, double value, const kit_number_flow_desc *d)
{
    const kit_class list[] = { { cls, cls != NULL } };
    node *n = emit(k, key, KIND_BOX, "number-flow", kit_classes(k, "number-flow", list, 1));
    set_attr(n, "value", frame_format(k, "%.17g", value));
    set_attr(n, "precision", frame_format(k, "%d", d->precision));
    set_flag(n, "grouping", d->grouping);
    set_attr(n, "prefix", d->prefix ? d->prefix : "");
    set_attr(n, "suffix", d->suffix ? d->suffix : "");
}

void kit_morph(kit *k, const char *key, const char *cls, const char *text)
{
    const kit_class list[] = { { cls, cls != NULL } };
    node *n = emit(k, key, KIND_BOX, "morph-text", kit_classes(k, "morph", list, 1));
    morph_text_set(n->el, text ? text : "");
}
