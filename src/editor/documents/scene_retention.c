#include "editor/documents/scene_retention.h"

#include "assets/prototype.h"
#include "support/array.h"
#include "support/asset_value.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static scene_retained_component *scene_retention_find(
    scene_retained_component *components,
    const char *name
)
{
    for (int i = 0; i < acount(components); i++) {
        if (!strcmp(components[i].name, name)) {
            return &components[i];
        }
    }
    return NULL;
}

static void free_component(scene_retained_component *component)
{
    free(component->name);
    av_value_free(component->values);
    av_value_free(component->defaults);
}

void scene_retention_remove(scene_retained_component **components, const char *name)
{
    scene_retained_component *found = scene_retention_find(*components, name);
    if (!found) {
        return;
    }
    int index = (int)(found - *components);
    free_component(found);
    adel(*components, index);
}

void scene_retention_free(scene_retained_component *components)
{
    for (int i = 0; i < acount(components); i++) {
        free_component(&components[i]);
    }
    afree(components);
}

av_value *scene_retention_save(const scene_retained_component *components)
{
    av_doc *doc = av_doc_new();
    av_value *root = av_obj(doc);
    av_doc_set_root(doc, root);
    for (int i = 0; i < acount(components); i++) {
        const scene_retained_component *component = &components[i];
        av_value *entry = av_obj(doc);
        av_obj_add_val(doc, entry, "values", av_clone(doc, component->values));
        av_obj_add_val(doc, entry, "defaults", av_clone(doc, component->defaults));
        av_obj_add_bool(doc, entry, "automatic", component->automatic);
        av_obj_add_val(doc, root, component->name, entry);
    }
    av_value *value = av_value_copy(root);
    av_doc_free(doc);
    return value;
}

int scene_retention_load(av_value *root, scene_retained_component **components)
{
    scene_retained_component *loaded = NULL;
    if (root && !av_is_obj(root)) {
        return -1;
    }
    av_obj_iter iterator = av_obj_iter_with(root);
    av_value *key;
    while ((key = av_obj_iter_next(&iterator))) {
        const char *name = av_get_str(key);
        av_value *entry = av_obj_iter_get_val(key);
        av_value *values = av_obj_get(entry, "values");
        av_value *defaults = av_obj_get(entry, "defaults");
        av_value *automatic = av_obj_get(entry, "automatic");
        if (!name[0] || scene_retention_find(loaded, name) || !av_is_obj(values) ||
            !av_is_obj(defaults) || !av_is_bool(automatic)) {
            scene_retention_free(loaded);
            return -1;
        }
        scene_retained_component component = {
            .name = xstrdup(name),
            .values = av_value_copy(values),
            .defaults = av_value_copy(defaults),
            .automatic = av_get_bool(automatic),
        };
        av_doc *empty = av_doc_new();
        av_value *check = game_prototype_rebase_component_value(
            component.values,
            component.defaults,
            av_obj(empty)
        );
        av_doc_free(empty);
        if (!check) {
            free_component(&component);
            scene_retention_free(loaded);
            return -1;
        }
        av_value_free(check);
        apush(loaded, component);
    }
    scene_retention_free(*components);
    *components = loaded;
    return 0;
}

bool scene_retention_validate(const scene_retained_component *components, const game_prototype_registry *registry)
{
    for (int i = 0; i < acount(components); i++) {
        av_value *snapshots[] = { components[i].values, components[i].defaults };
        for (int j = 0; j < 2; j++) {
            av_doc *doc = av_doc_new();
            av_value *root = av_obj(doc);
            av_obj_add_str(doc, root, "name", "Retained");
            av_obj_add_val(doc, root, "components", av_clone(doc, snapshots[j]));
            char error[256];
            game_prototype *parsed = game_prototype_load_value(root, registry, error, sizeof error);
            av_doc_free(doc);
            bool valid = parsed && ((j == 1 && !acount(parsed->components)) ||
                                    (acount(parsed->components) == 1 &&
                                     !strcmp(
                                         parsed->components[0].desc->name,
                                         components[i].name
                                     )));
            game_prototype_free(parsed);
            if (!valid) {
                return false;
            }
        }
    }
    return true;
}
