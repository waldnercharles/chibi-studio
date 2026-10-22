/* Assets dropped on scene objects (Studio's typed drops): the field of an
 * object that names an asset of a kind, and a preview of the asset in it, as
 * Unity previews a material dragged over an object. A preview lasts one
 * draw: the draw writes it, renders and puts the authored value back, so no
 * save, history or runtime ever reads it. */
#include "authoring/reference_index.h"
#include "editor/workspace/scene_internal.h"
#include "support/arena.h"
#include "support/array.h"
#include "support/xalloc.h"
#include "types/text.h"

#include <stdlib.h>
#include <string.h>

int scene_asset_field(
    const app_scene_authoring *scene,
    ecs_entity entity,
    reference_asset_kind kind,
    ecs_component_id *component
)
{
    for (int i = 0; i < scene->registry.component_count; i++) {
        const ecs_component_desc *desc = scene->registry.components[i];
        if (!ecs_component_has(scene->world, entity, desc->id)) { continue; }
        for (int f = 0; f < desc->type->field_count; f++) {
            const type_field *field = &desc->type->fields[f];
            if ((field->type == PIN_STR || field->type == PIN_NAME) &&
                reference_named_field_kind(desc->type->name, field->name) == kind) {
                *component = desc->id;
                return f;
            }
        }
    }
    return -1;
}

void scene_preview_field(
    app_scene_authoring *scene,
    ecs_entity entity,
    ecs_component_id component,
    int field,
    const char *text
)
{
    for (int i = 0; i < acount(scene->field_previews); i++) {
        scene_field_preview *p = &scene->field_previews[i];
        if (ecs_entity_to_u64(p->entity) == ecs_entity_to_u64(entity) &&
            p->component == component && p->field == field) {
            free(p->text);
            p->text = xstrdup(text);
            return;
        }
    }
    scene_field_preview preview = { .entity = entity,
                                    .component = component,
                                    .field = field,
                                    .text = xstrdup(text) };
    apush(scene->field_previews, preview);
}

void scene_field_previews_apply(app_scene_authoring *scene)
{
    arena storage = { 0 };
    for (int i = 0; i < acount(scene->field_previews); i++) {
        arena_reset(&storage);
        scene_field_preview *p = &scene->field_previews[i];
        pin_value value = { 0 };
        if (!ecs_entity_is_alive(scene->world, p->entity) ||
            ecs_component_read_field(&storage, scene->world, p->entity, p->component, p->field, &value) !=
                ECS_OK ||
            !scene_property_value_copy(&p->before, value)) {
            continue;
        }
        if (value.type == PIN_STR) {
            pin_value preview = { 0 };
            if (pin_value_text(&storage, &preview, p->text, strlen(p->text))) {
                scene_property_value_free(&p->before);
                continue;
            }
            preview.type_name = value.type_name;
            value = preview;
        } else {
            value.str = p->text;
        }
        p->shown = ecs_component_write_field(scene->world, p->entity, p->component, p->field, value) ==
                   ECS_OK;
        if (!p->shown) { scene_property_value_free(&p->before); }
    }
    arena_free(&storage);
}

void scene_field_previews_clear(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->field_previews); i++) {
        scene_field_preview *p = &scene->field_previews[i];
        if (p->shown) {
            ecs_component_write_field(scene->world, p->entity, p->component, p->field, p->before);
            scene_property_value_free(&p->before);
        }
        free(p->text);
    }
    afree(scene->field_previews);
    scene->field_previews = NULL;
}
