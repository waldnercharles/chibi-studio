#include "studio_registry.h"

#include "authoring/application_catalog.h"
#include "authoring/graph_catalog.h"
#include "authoring/studio_schema.h"
#include "runtime/component_catalog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

extern const registry_pack STUDIO_PACK;
static authoring_graph_catalog *catalog;

static bool same_types(type_catalog_pack a, type_catalog_pack b)
{
    if (a.type_count != b.type_count || a.enum_count != b.enum_count) {
        return false;
    }
    for (int i = 0; i < a.type_count; i++) {
        if (a.types[i] != b.types[i]) {
            return false;
        }
    }
    for (int i = 0; i < a.enum_count; i++) {
        if (a.enums[i] != b.enums[i]) {
            return false;
        }
    }
    return true;
}
static bool graph_types(type_catalog_pack pack)
{
    return (pack.type_count || pack.enum_count) &&
           !same_types(pack, TYPE_PACK) && !same_types(pack, STUDIO_TYPE_PACK);
}
int type_catalog_pack_count()
{
    int count = 2;
    for (int i = 0; catalog && i < authoring_graph_catalog_type_pack_count(catalog); i++) {
        count += graph_types(authoring_graph_catalog_type_pack_at(catalog, i));
    }
    return count;
}

type_catalog_pack type_catalog_pack_at(int index)
{
    assert(index >= 0);
    if (index == 0) {
        return TYPE_PACK;
    }
    if (index == 1) {
        return STUDIO_TYPE_PACK;
    }
    index -= 2;
    for (int i = 0; catalog && i < authoring_graph_catalog_type_pack_count(catalog); i++) {
        type_catalog_pack pack = authoring_graph_catalog_type_pack_at(catalog, i);
        if (graph_types(pack) && index-- == 0) {
            return pack;
        }
    }
    assert(false);
    return (type_catalog_pack){ 0 };
}

void studio_registry_init()
{
    assert(!catalog);
    char error[512] = { 0 };
    native_capabilities *native = authoring_application_catalog_new(true, error, sizeof error);
    const registry_pack *extra[] = { &STUDIO_PACK };
    if (native) {
        catalog = authoring_graph_catalog_new(
            (authoring_graph_catalog_config){
                .native = native,
                .types = TYPE_PACK,
                .components = { REGISTRY_COMPONENTS, REGISTRY_COMPONENT_COUNT },
                .extra_packs = extra,
                .extra_count = 1 },
            error,
            sizeof error
        );
        native_capabilities_free(native);
    }
    if (!catalog) {
        fprintf(stderr, "Studio catalog: %s\n", error);
        exit(1);
    }
    int count;
    const registry_pack *const *packs = authoring_graph_catalog_packs(catalog, &count);
    registry_set_packs(packs, count);
}

void studio_registry_free()
{
    registry_set_packs(NULL, 0);
    authoring_graph_catalog_free(catalog);
    catalog = NULL;
}
