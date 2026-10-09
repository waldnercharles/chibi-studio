#include "editor/ui/choices.h"

#include "editor/ui/kit_icons.h"
#include "graph/model/registry.h"
#include "runtime/components.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int editor_choices_count(const editor_choices *choices)
{
    if (!choices->enumeration) {
        return choices->count;
    }
    int count = choices->enumeration->value_count;
    if (!strcmp(choices->enumeration->name, "GameControl") && count > 0 &&
        choices->enumeration->values[count - 1].value == GAME_CONTROL_COUNT) {
        return count - 1;
    }
    return count;
}

editor_choice editor_choice_at(const editor_choices *choices, int index)
{
    if (!choices->enumeration) {
        return choices->items[index];
    }
    const enum_value_desc *value = &choices->enumeration->values[index];
    /* Enum values are numeric. Search their displayed labels, without common
       implementation prefixes such as GAME_CONTROL matching every row. */
    editor_choice choice = { .value = value->value, .label = value->label };
    /* Game controls are reused as enum choices by the inspector and graph
       editors, so keep their input categories and glyphs in this catalog. */
    if (!strcmp(choices->enumeration->name, "GameControl")) {
        if (value->value == GAME_CONTROL_NONE) {
            choice.icon = KIT_ICON_CLOSE;
        } else if (value->value < GAME_CONTROL_SPACE) {
          choice.group = "Mouse";
          choice.icon = KIT_ICON_MOUSE;
        } else if (value->value < GAME_CONTROL_PAD_A) {
          choice.group = "Keyboard";
          choice.icon = KIT_ICON_KEYBOARD;
        } else {
          choice.group = "Controller";
          choice.icon = KIT_ICON_GAMEPAD;
        }
    }
    return choice;
}

const editor_choices *editor_choices_for_enum(const char *type_name)
{
    static editor_choices choices;
    const enum_desc *enumeration = type_catalog_enum_find(type_name);
    if (!enumeration) {
        return NULL;
    }
    choices = (editor_choices){ .enumeration = enumeration };
    return &choices;
}
