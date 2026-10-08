#include "editor/ui/labels.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void editor_identifier_label(const char *identifier, char *buf, size_t cap)
{
    assert(identifier && buf && cap);
    const unsigned char *name = (const unsigned char *)identifier;
    size_t used = 0;
    bool word_start = true;
    for (size_t i = 0; name[i] && used + 1 < cap; i++) {
        unsigned char c = name[i];
        if (c == '_' || isspace(c)) {
            word_start = true;
            continue;
        }
        bool camel_boundary = i && isupper(c) &&
                              (islower(name[i - 1]) ||
                               (isupper(name[i - 1]) && islower(name[i + 1])));
        if (used && (word_start || camel_boundary)) {
            if (used + 2 >= cap) {
                break;
            }
            buf[used++] = ' ';
        }
        buf[used++] = word_start ? (char)toupper(c) : (char)c;
        word_start = false;
    }
    buf[used] = '\0';
}

void editor_component_label(const char *name, char *buf, size_t cap)
{
    assert(name);
    if (!cap) {
        return;
    }
    if (!strcmp(name, "GameGeometry3D")) {
        snprintf(buf, cap, "3D Geometry");
        return;
    }
    if (!strcmp(name, "GameAppearance")) {
        snprintf(buf, cap, "2D Appearance");
        return;
    }
    if (strncmp(name, "Game", 4) == 0 && isupper((unsigned char)name[4])) {
        name += 4;
    }
    editor_identifier_label(name, buf, cap);
    for (char *word = buf; *word;) {
        char *end = strchr(word, ' ');
        size_t length = end ? (size_t)(end - word) : strlen(word);
        if (length == 3 && strncmp(word, "Sdf", 3) == 0) {
            word[1] = 'D';
            word[2] = 'F';
        }
        if (!end) {
            break;
        }
        word = end + 1;
    }
}

void editor_node_label(const node_desc *desc, char *buf, size_t cap)
{
    assert(desc);
    const char *display_name = desc->name;
    if (desc->category && !strcmp(desc->category, "Shader") &&
        !strncmp(display_name, "Shader ", 7)) {
        display_name += 7;
    }
    const char *type = desc->type ? desc->type->name : NULL;
    if (type && strncmp(desc->name, "Set_", 4) == 0) {
        size_t length = strlen(type);
        const char *name = desc->name + 4;
        if (strncmp(name, type, length) == 0 && name[length] == '_' &&
            name[length + 1]) {
            char component[128], field[128];
            editor_component_label(type, component, sizeof component);
            editor_identifier_label(name + length + 1, field, sizeof field);
            snprintf(buf, cap, "Set %s %s", component, field);
            return;
        }
    }
    const char *operations[] = { "Get",       "Set",  "GetEntity",
                                 "SetEntity", "Make", "Break",
                                 "Unwrap",    "Has",  "HasEntity" };
    if (type && strncmp(type, "Game", 4) == 0 && isupper((unsigned char)type[4])) {
        for (int i = 0; i < (int)(sizeof operations / sizeof operations[0]); i++) {
            size_t length = strlen(operations[i]);
            if (strncmp(desc->name, operations[i], length) != 0 ||
                desc->name[length] != '_' ||
                strcmp(desc->name + length + 1, type) != 0) {
                continue;
            }
            editor_identifier_label(operations[i], buf, cap);
            size_t used = strlen(buf);
            if (used + 2 < cap) {
                buf[used++] = ' ';
                editor_identifier_label(type + 4, buf + used, cap - used);
            }
            return;
        }
    }
    editor_identifier_label(display_name, buf, cap);
}
