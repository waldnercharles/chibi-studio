/* What a Designer value is (workstream D1): the control a CSS property's
 * value calls for, the text a control's value becomes in the document, and
 * the value a new property starts with. Plain C over strings, so a panel, a
 * behavior node or a test can ask without a panel (studio_designer_edit.h). */
#include "editor/studio/studio_designer_edit.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Property types ---------- */

#define KEYWORDS(list) list, (int)(sizeof list / sizeof *list)

static const char *const DISPLAY[] = { "none",         "block", "inline",
                                       "inline-block", "flex",  "inline-flex",
                                       "table" };
static const char *const POSITION[] = { "static", "relative", "absolute", "fixed" };
static const char *const FLEX_DIRECTION[] = { "row", "row-reverse", "column", "column-reverse" };
static const char *const FLEX_WRAP[] = { "nowrap", "wrap", "wrap-reverse" };
static const char *const JUSTIFY[] = { "flex-start",   "flex-end",
                                       "center",       "space-between",
                                       "space-around", "space-evenly" };
static const char *const ALIGN_ITEMS[] = { "flex-start", "flex-end", "center", "baseline", "stretch" };
static const char *const ALIGN_SELF[] = { "auto",   "flex-start", "flex-end",
                                          "center", "baseline",   "stretch" };
static const char *const TEXT_ALIGN[] = { "left", "center", "right", "justify" };
static const char *const OVERFLOW_MODES[] = { "visible", "hidden", "auto", "scroll" };
static const char *const VISIBILITY[] = { "visible", "hidden" };
static const char *const WHITE_SPACE[] = { "normal", "pre", "nowrap", "pre-wrap", "pre-line" };
static const char *const FONT_STYLE[] = { "normal", "italic" };
static const char *const FONT_WEIGHT[] = { "normal", "bold", "100", "200",
                                           "300",    "400",  "500", "600",
                                           "700",    "800",  "900" };
static const char *const TEXT_TRANSFORM[] = { "none", "capitalize", "uppercase", "lowercase" };
static const char *const TEXT_DECORATION[] = { "none", "underline", "overline", "line-through" };
static const char *const TEXT_OVERFLOW[] = { "clip", "ellipsis" };
static const char *const POINTER_EVENTS[] = { "auto", "none" };
static const char *const BOX_SIZING[] = { "content-box", "border-box" };

/* RCSS properties by the control their values take (RmlUi's property
 * reference); others edit as text. */
static const designer_property_type TYPES[] = {
    { "width", DESIGNER_VALUE_LENGTH },
    { "height", DESIGNER_VALUE_LENGTH },
    { "min-width", DESIGNER_VALUE_LENGTH },
    { "max-width", DESIGNER_VALUE_LENGTH },
    { "min-height", DESIGNER_VALUE_LENGTH },
    { "max-height", DESIGNER_VALUE_LENGTH },
    { "left", DESIGNER_VALUE_LENGTH },
    { "top", DESIGNER_VALUE_LENGTH },
    { "right", DESIGNER_VALUE_LENGTH },
    { "bottom", DESIGNER_VALUE_LENGTH },
    { "margin", DESIGNER_VALUE_LENGTH },
    { "margin-top", DESIGNER_VALUE_LENGTH },
    { "margin-right", DESIGNER_VALUE_LENGTH },
    { "margin-bottom", DESIGNER_VALUE_LENGTH },
    { "margin-left", DESIGNER_VALUE_LENGTH },
    { "padding", DESIGNER_VALUE_LENGTH },
    { "padding-top", DESIGNER_VALUE_LENGTH },
    { "padding-right", DESIGNER_VALUE_LENGTH },
    { "padding-bottom", DESIGNER_VALUE_LENGTH },
    { "padding-left", DESIGNER_VALUE_LENGTH },
    { "border-width", DESIGNER_VALUE_LENGTH },
    { "border-top-width", DESIGNER_VALUE_LENGTH },
    { "border-right-width", DESIGNER_VALUE_LENGTH },
    { "border-bottom-width", DESIGNER_VALUE_LENGTH },
    { "border-left-width", DESIGNER_VALUE_LENGTH },
    { "border-radius", DESIGNER_VALUE_LENGTH },
    { "font-size", DESIGNER_VALUE_LENGTH },
    { "line-height", DESIGNER_VALUE_LENGTH },
    { "gap", DESIGNER_VALUE_LENGTH },
    { "row-gap", DESIGNER_VALUE_LENGTH },
    { "column-gap", DESIGNER_VALUE_LENGTH },
    { "letter-spacing", DESIGNER_VALUE_LENGTH },
    { "flex-basis", DESIGNER_VALUE_LENGTH },
    { "opacity", DESIGNER_VALUE_NUMBER },
    { "flex-grow", DESIGNER_VALUE_NUMBER },
    { "flex-shrink", DESIGNER_VALUE_NUMBER },
    { "z-index", DESIGNER_VALUE_NUMBER },
    { "color", DESIGNER_VALUE_COLOR },
    { "background-color", DESIGNER_VALUE_COLOR },
    { "border-color", DESIGNER_VALUE_COLOR },
    { "border-top-color", DESIGNER_VALUE_COLOR },
    { "border-right-color", DESIGNER_VALUE_COLOR },
    { "border-bottom-color", DESIGNER_VALUE_COLOR },
    { "border-left-color", DESIGNER_VALUE_COLOR },
    { "image-color", DESIGNER_VALUE_COLOR },
    { "display", DESIGNER_VALUE_KEYWORD, KEYWORDS(DISPLAY) },
    { "position", DESIGNER_VALUE_KEYWORD, KEYWORDS(POSITION) },
    { "flex-direction", DESIGNER_VALUE_KEYWORD, KEYWORDS(FLEX_DIRECTION) },
    { "flex-wrap", DESIGNER_VALUE_KEYWORD, KEYWORDS(FLEX_WRAP) },
    { "justify-content", DESIGNER_VALUE_KEYWORD, KEYWORDS(JUSTIFY) },
    { "align-content", DESIGNER_VALUE_KEYWORD, KEYWORDS(JUSTIFY) },
    { "align-items", DESIGNER_VALUE_KEYWORD, KEYWORDS(ALIGN_ITEMS) },
    { "align-self", DESIGNER_VALUE_KEYWORD, KEYWORDS(ALIGN_SELF) },
    { "text-align", DESIGNER_VALUE_KEYWORD, KEYWORDS(TEXT_ALIGN) },
    { "overflow", DESIGNER_VALUE_KEYWORD, KEYWORDS(OVERFLOW_MODES) },
    { "overflow-x", DESIGNER_VALUE_KEYWORD, KEYWORDS(OVERFLOW_MODES) },
    { "overflow-y", DESIGNER_VALUE_KEYWORD, KEYWORDS(OVERFLOW_MODES) },
    { "visibility", DESIGNER_VALUE_KEYWORD, KEYWORDS(VISIBILITY) },
    { "white-space", DESIGNER_VALUE_KEYWORD, KEYWORDS(WHITE_SPACE) },
    { "font-style", DESIGNER_VALUE_KEYWORD, KEYWORDS(FONT_STYLE) },
    { "font-weight", DESIGNER_VALUE_KEYWORD, KEYWORDS(FONT_WEIGHT) },
    { "text-transform", DESIGNER_VALUE_KEYWORD, KEYWORDS(TEXT_TRANSFORM) },
    { "text-decoration", DESIGNER_VALUE_KEYWORD, KEYWORDS(TEXT_DECORATION) },
    { "text-overflow", DESIGNER_VALUE_KEYWORD, KEYWORDS(TEXT_OVERFLOW) },
    { "pointer-events", DESIGNER_VALUE_KEYWORD, KEYWORDS(POINTER_EVENTS) },
    { "box-sizing", DESIGNER_VALUE_KEYWORD, KEYWORDS(BOX_SIZING) },
};

static const designer_property_type *type_of(const char *property)
{
    for (size_t i = 0; i < sizeof TYPES / sizeof *TYPES; i++) {
        if (!strcmp(TYPES[i].name, property)) {
            return &TYPES[i];
        }
    }
    return NULL;
}

/* ---------- Values ---------- */

/* A number and its unit alone, as "12dp", "50%" or "0.5". */
static bool parse_number(const char *value, double *number, char *unit, size_t size)
{
    char *end;
    *number = strtod(value, &end);
    const char *start = end;
    while (isalpha((unsigned char)*end) || *end == '%') {
        end++;
    }
    size_t length = (size_t)(end - start);
    if (start == value || *end || length >= size || !isfinite(*number)) {
        return false;
    }
    snprintf(unit, size, "%.*s", (int)length, start);
    return true;
}

static void format_value(char *out, size_t size, double value, int precision, const char *unit)
{
    char number[48];
    snprintf(number, sizeof number, "%.*f", precision, value);
    size_t length = strlen(number);
    while (strchr(number, '.') &&
           (number[length - 1] == '0' || number[length - 1] == '.')) {
        number[--length] = '\0';
    }
    snprintf(out, size, "%s%s", strcmp(number, "-0") ? number : "0", unit);
}

static int hex_digit(char c)
{
    return isdigit((unsigned char)c)  ? c - '0'
         : isxdigit((unsigned char)c) ? tolower((unsigned char)c) - 'a' + 10
                                      : -1;
}

/* #rgb, #rgba, #rrggbb or #rrggbbaa as 0xRRGGBBAA. */
static bool parse_color(const char *value, uint32_t *rgba)
{
    size_t digits = value[0] == '#' ? strlen(value + 1) : 0;
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) {
        return false;
    }
    bool shorthand = digits <= 4;
    uint32_t color = 0;
    for (size_t i = 1; i <= digits; i++) {
        int v = hex_digit(value[i]);
        if (v < 0) {
            return false;
        }
        color = shorthand ? color << 8 | (uint32_t)(v * 17)
                          : color << 4 | (uint32_t)v;
    }
    *rgba = digits == 3 || digits == 6 ? color << 8 | 0xffu : color;
    return true;
}

static void format_color(char *out, size_t size, uint32_t rgba)
{
    if ((rgba & 0xffu) == 0xffu) {
        snprintf(out, size, "#%06x", (unsigned)(rgba >> 8));
    } else {
        snprintf(out, size, "#%08x", (unsigned)rgba);
    }
}

/* A property's control: its type's, or for a custom property its value's. */
static designer_value_kind kind_of(const char *property, const char *value, const designer_property_type **type)
{
    *type = type_of(property);
    uint32_t color;
    double number;
    char unit[8];
    bool custom = !strncmp(property, "--", 2);
    return *type                                ? (*type)->kind
         : custom && parse_color(value, &color) ? DESIGNER_VALUE_COLOR
         : custom && parse_number(value, &number, unit, sizeof unit)
             ? DESIGNER_VALUE_LENGTH
             : DESIGNER_VALUE_TEXT;
}

const char *designer_default_value(const char *property)
{
    const designer_property_type *type = type_of(property);
    switch (type ? type->kind : DESIGNER_VALUE_TEXT) {
        case DESIGNER_VALUE_LENGTH: return "0dp";
        case DESIGNER_VALUE_NUMBER:
            return strcmp(property, "opacity") ? "0" : "1";
        case DESIGNER_VALUE_COLOR: return "#ffffff";
        case DESIGNER_VALUE_KEYWORD: return type->keywords[0];
        case DESIGNER_VALUE_TEXT: break;
    }
    return "none";
}

/* ---------- A value's control ---------- */

static bool fine_unit(const char *unit)
{
    return !strcmp(unit, "em") || !strcmp(unit, "rem") || !strcmp(unit, "s");
}

/* A length scrubs with its unit, dp and percentages by ones, ems and
 * seconds by tenths, opacity as a slider. An unwritten unit reads as dp. A
 * value its type does not call a control for, as var(--x), is text. */
designer_value_shape designer_value_shape_of(const char *property, const char *value)
{
    designer_value_shape v = { .control = "text", .min = -INFINITY, .max = INFINITY };
    const designer_property_type *type;
    designer_value_kind kind = kind_of(property, value, &type);
    double number;
    char unit[8];
    uint32_t rgba;
    if ((kind == DESIGNER_VALUE_LENGTH || kind == DESIGNER_VALUE_NUMBER) &&
        parse_number(value, &number, unit, sizeof unit)) {
        const char *shown_unit = unit[0] || kind != DESIGNER_VALUE_LENGTH ? unit : "dp";
        v.control = "number";
        snprintf(v.number, sizeof v.number, "%.17g", number);
        snprintf(v.unit, sizeof v.unit, "%s", shown_unit);
        v.step = fine_unit(shown_unit) ? 0.1 : 1;
        v.precision = 2;
        v.integer = !strcmp(property, "z-index");
        if (!strcmp(property, "opacity")) {
            v.min = 0;
            v.max = 1;
            v.step = 0.01;
            v.slider = true;
        }
    } else if (kind == DESIGNER_VALUE_COLOR && parse_color(value, &rgba)) {
        v.control = "color";
        snprintf(v.color, sizeof v.color, "#%08x", (unsigned)rgba);
    } else if (kind == DESIGNER_VALUE_KEYWORD) {
        for (int i = 0; i < type->count; i++) {
            if (!strcmp(type->keywords[i], value)) {
                v.control = type->count <= 3 ? "toggle" : "choice";
                v.type = type;
                v.keyword = i;
            }
        }
    }
    return v;
}

/* What a control sends back, as the text the document gets, for the value
 * `current`; false when it is not a value that control takes. */
bool designer_value_text(const char *property, const char *current, const char *argument, char *out, size_t size)
{
    designer_value_shape v = designer_value_shape_of(property, current);
    if (!strcmp(v.control, "number")) {
        char *end;
        double number = strtod(argument, &end);
        if (end == argument || !isfinite(number)) {
            return false;
        }
        format_value(out, size, number, v.precision, v.unit);
        return true;
    }
    uint32_t rgba;
    if (!strcmp(v.control, "color")) {
        if (!parse_color(argument, &rgba)) {
            return false;
        }
        format_color(out, size, rgba);
        return true;
    }
    for (int i = 0; v.type && i < v.type->count; i++) {
        if (!strcmp(v.type->keywords[i], argument)) {
            snprintf(out, size, "%s", argument);
            return true;
        }
    }
    return false;
}
