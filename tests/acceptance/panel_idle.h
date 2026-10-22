#include "ui/popover.h"

#define PANEL_IDLE_FRAMES 10
#define PANEL_IDLE_REPEATS 8

static const char *const panel_idle_sources[] = { "properties",
                                                  "assets",
                                                  "inspector",
                                                  "graph_header",
                                                  "graph_targets" };
static const char *const panel_idle_values[] = {
    "properties",     "propertyValues", "assets",       "inspector",
    "inspector_tree", "inspector_ui",   "graph_header", "graph_targets"
};

static bool panel_idle_mixed_axes(ui_data *data, bool x, bool y)
{
    yyjson_mut_val *properties = yyjson_mut_obj_get(yyjson_mut_doc_get_root(data->doc), "properties");
    yyjson_mut_val *panels = yyjson_mut_obj_get(properties, "panels");
    for (size_t p = 0; p < yyjson_mut_arr_size(panels); p++) {
        yyjson_mut_val *panel = yyjson_mut_arr_get(panels, p);
        if (!yyjson_mut_get_bool(yyjson_mut_obj_get(panel, "shown"))) {
            continue;
        }
        yyjson_mut_val *rows = yyjson_mut_obj_get(panel, "rows");
        for (size_t r = 0; r < yyjson_mut_arr_size(rows); r++) {
            yyjson_mut_val *row = yyjson_mut_arr_get(rows, r);
            const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(row, "label"));
            if (!label || strcmp(label, "Position")) {
                continue;
            }
            yyjson_mut_val *axes = yyjson_mut_obj_get(row, "axes");
            REQUIRE(yyjson_mut_arr_size(axes) == 2);
            yyjson_mut_val *axis_x = yyjson_mut_arr_get(axes, 0);
            yyjson_mut_val *axis_y = yyjson_mut_arr_get(axes, 1);
            REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(axis_x, "key")), "x"));
            REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(axis_y, "key")), "y"));
            REQUIRE(yyjson_mut_get_bool(yyjson_mut_obj_get(axis_x, "mixed")) == x);
            REQUIRE(yyjson_mut_get_bool(yyjson_mut_obj_get(axis_y, "mixed")) == y);
            return true;
        }
    }
    REQUIRE(false);
    return false;
}

/* A separate data model runs the production synchronizers beside the real
   rendered panels. It owns no editor state and receives no pointer input. */
static bool panel_idle_measure(studio *s, studio_sources *sources, ui_data *data, const char *stage)
{
    for (size_t i = 0; i < sizeof panel_idle_sources / sizeof *panel_idle_sources; i++) {
        studio_sources_resync(s, sources, data, panel_idle_sources[i]);
    }
    if (!strcmp(stage, "editing")) {
        REQUIRE(panel_idle_mixed_axes(data, true, false));
    }
    char *before[sizeof panel_idle_values / sizeof *panel_idle_values];
    yyjson_mut_val *root = yyjson_mut_doc_get_root(data->doc);
    for (size_t i = 0; i < sizeof before / sizeof *before; i++) {
        before[i] = yyjson_mut_val_write(yyjson_mut_obj_get(root, panel_idle_values[i]), 0, NULL);
        REQUIRE(before[i]);
    }
    uint64_t fields = s->panel_work.properties_fields;
    uint64_t overrides = s->panel_work.property_overrides;
    uint64_t choices = s->panel_work.property_choices;
    uint64_t mixed = s->panel_work.properties_mixed_lists;
    uint64_t assets = s->panel_work.asset_rows;
    uint64_t inspector = s->panel_work.inspector_reads;
    uint64_t targets = s->panel_work.target_choices;
    uint64_t nanos[sizeof panel_idle_sources / sizeof *panel_idle_sources] = { 0 };
    for (int repeat = 0; repeat < PANEL_IDLE_REPEATS; repeat++) {
        for (size_t i = 0; i < sizeof panel_idle_sources / sizeof *panel_idle_sources;
             i++) {
            uint64_t started = SDL_GetTicksNS();
            studio_sources_resync(s, sources, data, panel_idle_sources[i]);
            nanos[i] += SDL_GetTicksNS() - started;
        }
    }
    printf(
        "Panel idle %s: repeats=%d fields=%llu overrides=%llu choices=%llu "
        "mixed=%llu assets=%llu inspector=%llu targets=%llu\n",
        stage,
        PANEL_IDLE_REPEATS,
        (unsigned long long)(s->panel_work.properties_fields - fields),
        (unsigned long long)(s->panel_work.property_overrides - overrides),
        (unsigned long long)(s->panel_work.property_choices - choices),
        (unsigned long long)(s->panel_work.properties_mixed_lists - mixed),
        (unsigned long long)(s->panel_work.asset_rows - assets),
        (unsigned long long)(s->panel_work.inspector_reads - inspector),
        (unsigned long long)(s->panel_work.target_choices - targets)
    );
    for (size_t i = 0; i < sizeof nanos / sizeof *nanos; i++) {
        printf("Panel time %s %s: %.3f ms\n", stage, panel_idle_sources[i], (double)nanos[i] / 1e6);
    }
    bool same = true;
    for (size_t i = 0; i < sizeof before / sizeof *before; i++) {
        char *after = yyjson_mut_val_write(yyjson_mut_obj_get(root, panel_idle_values[i]), 0, NULL);
        if (!after || strcmp(before[i], after)) {
            fprintf(stderr, "Panel idle %s changed %s\n", stage, panel_idle_values[i]);
            same = false;
        }
        free(before[i]);
        free(after);
    }
    REQUIRE(same);
    REQUIRE(s->panel_work.properties_mixed_lists == mixed);
    rml_element *popup = popover_top(rml_backend_context(s->backend));
    bool field_menu = popup && !strcmp(rml_element_id(popup), "field-menu");
    if (!field_menu) {
        REQUIRE(s->panel_work.property_overrides == overrides);
    }
    bool target_menu = popup && !strcmp(rml_element_id(popup), "debug-targets");
    if (studio_session(s) && target_menu) {
        REQUIRE(s->panel_work.target_choices - targets >= PANEL_IDLE_REPEATS);
    } else {
        REQUIRE(s->panel_work.target_choices == targets);
    }
    return true;
}

static char *panel_idle_json(ui_data *data, const char *name)
{
    return yyjson_mut_val_write(yyjson_mut_obj_get(yyjson_mut_doc_get_root(data->doc), name), 0, NULL);
}

static bool panel_idle_refresh(studio *s, studio_sources *sources, ui_data *data)
{
    app_scene_authoring *scene = s->scene;
    REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[0].entity) == 0);
    studio_sources_resync(s, sources, data, "properties");
    REQUIRE(panel_idle_mixed_axes(data, false, false));
    char *before = panel_idle_json(data, "properties");
    REQUIRE(before);
    REQUIRE(strstr(before, "First"));
    REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[1].entity) == 0);
    studio_sources_resync(s, sources, data, "properties");
    char *after = panel_idle_json(data, "properties");
    REQUIRE(after && strstr(after, "Second") && strcmp(before, after));
    free(before);
    free(after);
    before = panel_idle_json(data, "propertyValues");
    properties_row row = { .kind = PROPERTIES_OBJECT,
                           .entity = ecs_entity_to_u64(scene->actors[1].entity),
                           .component = COMPONENT_Transform.id,
                           .field = 0 };
    ui_activation change = { .argc = 4 };
    properties_row_id(&row, change.args[0]);
    snprintf(change.args[1], sizeof change.args[1], "7");
    snprintf(change.args[2], sizeof change.args[2], "0");
    snprintf(change.args[3], sizeof change.args[3], "x");
    studio_properties_action(s, "change", &change);
    REQUIRE(Transform_get(scene->world, scene->actors[1].entity)->x == 7);
    studio_sources_resync(s, sources, data, "properties");
    after = panel_idle_json(data, "propertyValues");
    REQUIRE(before && after && strcmp(before, after));
    free(before);
    free(after);
    snprintf(change.args[1], sizeof change.args[1], "3");
    studio_properties_action(s, "change", &change);
    REQUIRE(Transform_get(scene->world, scene->actors[1].entity)->x == 3);
    REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[0].entity) == 0);
    scene->actors[1].marked = true;
    studio_sources_resync(s, sources, data, "properties");
    REQUIRE(panel_idle_mixed_axes(data, true, false));
    rml_element *menu = studio_element(s, "#field-menu");
    REQUIRE(menu);
    popover_anchor anchor = { .side = POPOVER_POINT, .box = { 100, 100, 0, 0 } };
    popover_show(menu, &anchor);
    uint64_t overrides = s->panel_work.property_overrides;
    studio_sources_resync(s, sources, data, "properties");
    REQUIRE(s->panel_work.property_overrides > overrides);
    yyjson_mut_val *properties = yyjson_mut_obj_get(yyjson_mut_doc_get_root(data->doc), "properties");
    yyjson_mut_val *reset = yyjson_mut_obj_get(yyjson_mut_obj_get(properties, "fieldMenu"), "reset");
    REQUIRE(!yyjson_mut_get_bool(yyjson_mut_obj_get(reset, "disabled")));
    popover_hide(menu);
    popover_show(menu, &anchor);
    studio_sources_resync(s, sources, data, "properties");
    REQUIRE(!yyjson_mut_get_bool(yyjson_mut_obj_get(reset, "disabled")));
    popover_hide(menu);

    snprintf(s->assets_filter, sizeof s->assets_filter, "no-such-idle-asset");
    studio_sources_resync(s, sources, data, "assets");
    yyjson_mut_val *assets = yyjson_mut_obj_get(yyjson_mut_doc_get_root(data->doc), "assets");
    REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(assets, "empty")), "No assets match"));
    ui_activation clear = { 0 };
    studio_assets_action(s, "clearFilter", &clear);
    studio_sources_resync(s, sources, data, "assets");
    REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(assets, "empty")), ""));
    rml_element *list = studio_element(s, "#assets-list");
    REQUIRE(list);
    rml_element_set_scroll_top(list, 100);
    studio_assets_action(s, "scroll", &clear);
    studio_sources_resync(s, sources, data, "assets");
    REQUIRE(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(assets, "empty")), ""));
    rml_element_set_scroll_top(list, 0);

    return true;
}

static bool panel_idle_inspector(studio *s, studio_sources *sources, ui_data *data)
{
    rml_element *element = studio_element(s, "#assets-filter");
    REQUIRE(element);
    studio_inspect_select(s->inspect, element);
    studio_sources_resync(s, sources, data, "inspector");
    const studio_inspect_model *inspect = studio_inspect_shown(s);
    REQUIRE(inspect && inspect->selection && !inspect->detached);
    REQUIRE(acount(inspect->styles) && acount(inspect->attributes));
    REQUIRE(inspect->size[0]);
    rml_element_set_attribute(element, "data-idle-probe", "changed");
    REQUIRE(rml_element_set_property(element, "opacity", "0.5"));
    studio_sources_resync(s, sources, data, "inspector");
    inspect = studio_inspect_shown(s);
    bool attribute = false, style = false;
    for (int i = 0; i < acount(inspect->attributes); i++) {
        attribute |= !strcmp(inspect->attributes[i].name, "data-idle-probe") &&
                     !strcmp(inspect->attributes[i].value, "changed");
    }
    for (int i = 0; i < acount(inspect->styles); i++) {
        style |= !strcmp(inspect->styles[i].name, "opacity") &&
                 strtod(inspect->styles[i].value, NULL) == 0.5;
    }
    REQUIRE(attribute && style);
    rml_element_remove_attribute(element, "data-idle-probe");
    rml_element_remove_property(element, "opacity");
    return true;
}

static int panel_idle_target_count(ui_data *data)
{
    yyjson_mut_val *targets = yyjson_mut_obj_get(yyjson_mut_doc_get_root(data->doc), "graph_targets");
    yyjson_mut_val *groups = yyjson_mut_obj_get(targets, "groups");
    int count = 0;
    for (size_t i = 0; i < yyjson_mut_arr_size(groups); i++) {
        yyjson_mut_val *options = yyjson_mut_obj_get(yyjson_mut_arr_get(groups, i), "options");
        for (size_t j = 0; j < yyjson_mut_arr_size(options); j++) {
            const char *id = yyjson_mut_get_str(
                yyjson_mut_obj_get(yyjson_mut_arr_get(options, j), "id")
            );
            count += id && !strncmp(id, "entity:", 7);
        }
    }
    return count;
}

/* Resolve the painted trigger rather than the first matching retained
   document. A center hit must belong to the trigger before SDL gets it. */
static rml_element *panel_idle_target_trigger(studio *s)
{
    rml_context *context = rml_backend_context(s->backend);
    rml_element *trigger = NULL;
    for (int d = 0; d < rml_context_document_count(context) && !trigger; d++) {
        rml_element *candidates[4];
        int count = rml_element_query_selector_all(
            rml_context_document(context, d),
            "button[popovertarget='debug-targets']",
            candidates,
            4
        );
        for (int i = 0; i < count && i < 4; i++) {
            rml_element *candidate = candidates[i];
            rml_vec2 at = rml_element_absolute_offset(candidate, RML_BOX_BORDER);
            rml_vec2 size = rml_element_box_size(candidate, RML_BOX_BORDER);
            bool visible = rml_element_is_visible(candidate, true);
            rml_vec2 center = { at.x + size.x / 2, at.y + size.y / 2 };
            rml_element *hit = rml_context_element_at_point(context, center, NULL);
            bool owns = false;
            for (rml_element *parent = hit; parent;
                 parent = rml_element_parent(parent)) {
                owns |= parent == candidate;
            }
            if (visible && size.x > 0 && size.y > 0 && owns) {
                trigger = candidate;
                break;
            }
        }
    }
    if (!trigger) {
        fprintf(
            stderr,
            "Panel target trigger has no visible center hit, ratio=%.2f\n",
            studio_pixel_ratio(s)
        );
    }
    return trigger;
}

static bool panel_idle_click_target(studio *s, rml_element *trigger)
{
    REQUIRE(trigger);
    rml_vec2 at = rml_element_absolute_offset(trigger, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(trigger, RML_BOX_BORDER);
    rml_vec2 point = { at.x + size.x / 2, at.y + size.y / 2 };
    SDL_WindowID id = window_id();
    REQUIRE(id);
    /* RmlUi boxes are device pixels; SDL reports window points. The host
       divides by UI scale and the backend multiplies by pixel ratio. */
    float points = s->prefs.ui_scale / studio_pixel_ratio(s);
    float x = point.x * points, y = point.y * points;
    SDL_Event move = {
        .motion = { .type = SDL_EVENT_MOUSE_MOTION, .windowID = id, .x = x, .y = y }
    };
    REQUIRE(SDL_PushEvent(&move));
    for (int down = 1; down >= 0; down--) {
        SDL_Event event = { .button = { .type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                     : SDL_EVENT_MOUSE_BUTTON_UP,
                                        .windowID = id,
                                        .button = SDL_BUTTON_LEFT,
                                        .down = down,
                                        .clicks = 1,
                                        .x = x,
                                        .y = y } };
        REQUIRE(SDL_PushEvent(&event));
    }
    return true;
}

static bool panel_idle_frame(void *state)
{
    static studio_sources sources;
    static ui_data data;
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    frames++;
    REQUIRE(frames <= PANEL_IDLE_FRAMES);
    switch (frames) {
        case 1: {
            s->prefs.reduced_motion = true;
            app_scene_authoring *scene = s->scene;
            REQUIRE(
                app_scene_authoring_register_prototype(
                    scene,
                    "package asset\n"
                    "\n"
                    "import data \"game:assets\"\n"
                    "\n"
                    "pub Build :: () -> byte[] {\n"
                    "    return data.Encode(\n"
                    "        data.Object([\n"
                    "            data.Field.Named(\"name\", "
                    "data.Value.Text(\"IdleProbe\")),\n"
                    "            data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                    data.Field.Named(\"x\", "
                    "data.Value.UInt(0)),\n"
                    "                    data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                ])),\n"
                    "                data.Field.Named(\"Velocity\", "
                    "data.Object([]))\n"
                    "            ])),\n"
                    "            data.Field.Named(\"behaviors\", data.List([\n"
                    "                data.Value.Text(\"Counter\")\n"
                    "            ]))\n"
                    "        ])\n"
                    "    )\n"
                    "}\n",
                    NULL
                ) == 0
            );
            REQUIRE(
                scene_authoring_load_text(
                    scene,
                    "package asset\n"
                    "\n"
                    "import data \"game:assets\"\n"
                    "\n"
                    "pub Build :: () -> byte[] {\n"
                    "    return data.Encode(\n"
                    "        data.Object([\n"
                    "            data.Field.Named(\"version\", "
                    "data.Value.UInt(1)),\n"
                    "            data.Field.Named(\"name\", "
                    "data.Value.Text(\"Start\")),\n"
                    "            data.Field.Named(\"entities\", data.List([\n"
                    "                data.Object([\n"
                    "                    data.Field.Named(\"name\", "
                    "data.Value.Text(\"First\")),\n"
                    "                    data.Field.Named(\"prototype\", "
                    "data.Value.Text(\"IdleProbe\"))\n"
                    "                ]),\n"
                    "                data.Object([\n"
                    "                    data.Field.Named(\"name\", "
                    "data.Value.Text(\"Second\")),\n"
                    "                    data.Field.Named(\"prototype\", "
                    "data.Value.Text(\"IdleProbe\")),\n"
                    "                    data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                        data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                            data.Field.Named(\"x\", "
                    "data.Value.UInt(3)),\n"
                    "                            data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                        ]))\n"
                    "                    ]))\n"
                    "                ])\n"
                    "            ]))\n"
                    "        ])\n"
                    "    )\n"
                    "}\n",
                    NULL
                ) == 0
            );
            REQUIRE(acount(scene->actors) == 2);
            REQUIRE(app_scene_authoring_select_entity(scene, scene->actors[0].entity) == 0);
            scene->actors[1].marked = true;
            REQUIRE(scene_selection_count(scene) == 2);
            s->context.entity = ecs_entity_to_u64(scene->actors[0].entity);
            s->context.component = COMPONENT_Transform.id;
            s->context.field = 0;
            snprintf(s->property_search, sizeof s->property_search, "Position");
            studio_behavior_show(s, 2);
            studio_layout *layout = studio_active_layout(s);
            layout->side_open = true;
            layout->editors[STUDIO_AREA_OUTLINER] = STUDIO_EDITOR_ASSETS;
            layout->editors[STUDIO_AREA_PROPERTIES] = STUDIO_EDITOR_PROPERTIES;
            studio_inspect_toggle(s);
            studio_inspect_toggle(s);
            data.doc = studio_sources_new();
            data.model = rml_data_model_create(
                rml_backend_context(s->backend),
                "panel_idle_acceptance",
                data.doc,
                NULL,
                NULL
            );
            REQUIRE(data.model);
            break;
        }
        case 2: break;
        case 3:
            REQUIRE(studio_element(s, "#assets-list"));
            REQUIRE(studio_element(s, "#inspector-list"));
            REQUIRE(panel_idle_measure(s, &sources, &data, "editing"));
            REQUIRE(panel_idle_refresh(s, &sources, &data));
            REQUIRE(panel_idle_inspector(s, &sources, &data));
            studio_command_run(s, STUDIO_COMMAND_PLAY);
            break;
        case 4: REQUIRE(studio_session(s)); break;
        case 5: {
            REQUIRE(studio_session(s));
            REQUIRE(panel_idle_measure(s, &sources, &data, "running-closed"));
            rml_element *trigger = panel_idle_target_trigger(s);
            REQUIRE(trigger);
            rml_element *popup = rml_element_by_id(rml_element_owner_document(trigger), "debug-targets");
            REQUIRE(popup && !popover_open(popup));
            REQUIRE(!rml_element_query_selector(popup, "[role='option']"));
            REQUIRE(panel_idle_click_target(s, trigger));
            break;
        }
        case 6: {
            rml_element *popup = popover_top(rml_backend_context(s->backend));
            REQUIRE(popup && !strcmp(rml_element_id(popup), "debug-targets"));
            REQUIRE(popover_open(popup));
            rml_element *owners[4];
            REQUIRE(
                rml_element_query_selector_all(popup, "[role='option'][value^='entity:']", owners, 4) == 2
            );
            rml_element *search = rml_element_query_selector(popup, "input[type='text']");
            REQUIRE(
                search && rml_context_focus_element(rml_backend_context(s->backend)) == search
            );
            REQUIRE(press_key(SDLK_ESCAPE));
            break;
        }
        case 7: {
            rml_element *trigger = panel_idle_target_trigger(s);
            REQUIRE(trigger);
            rml_element *popup = rml_element_by_id(rml_element_owner_document(trigger), "debug-targets");
            REQUIRE(popup && !popover_open(popup));
            REQUIRE(rml_element_focus(trigger, true));
            REQUIRE(press_key(SDLK_RETURN));
            break;
        }
        case 8: {
            REQUIRE(studio_session(s));
            rml_element *popup = popover_top(rml_backend_context(s->backend));
            REQUIRE(popup && !strcmp(rml_element_id(popup), "debug-targets"));
            REQUIRE(popover_open(popup));
            rml_element *owners[4];
            REQUIRE(
                rml_element_query_selector_all(popup, "[role='option'][value^='entity:']", owners, 4) == 2
            );
            rml_element *search = rml_element_query_selector(popup, "input[type='text']");
            REQUIRE(
                search && rml_context_focus_element(rml_backend_context(s->backend)) == search
            );
            popover_anchor anchor = { .side = POPOVER_POINT,
                                      .box = { 100, 100, 0, 0 } };
            REQUIRE(panel_idle_measure(s, &sources, &data, "running-open"));
            yyjson_mut_val *targets = yyjson_mut_obj_get(yyjson_mut_doc_get_root(data.doc), "graph_targets");
            REQUIRE(yyjson_mut_arr_size(yyjson_mut_obj_get(targets, "groups")) > 0);
            int attached = panel_idle_target_count(&data);
            REQUIRE(attached == 2);
            popover_hide(popup);
            source_registry *registry = s->scene->live->runtime->sources;
            ecs_entity entity = s->scene->actors[0].entity;
            REQUIRE(source_detach(registry, entity, "Counter") == 1);
            popover_show(popup, &anchor);
            studio_sources_resync(s, &sources, &data, "graph_targets");
            REQUIRE(panel_idle_target_count(&data) == attached - 1);
            popover_hide(popup);
            REQUIRE(
                source_attach(
                    registry,
                    entity,
                    "Counter",
                    false,
                    s->scene->live->runtime->now,
                    s->scene->live->runtime->delta
                ) == 0
            );
            popover_show(popup, &anchor);
            studio_sources_resync(s, &sources, &data, "graph_targets");
            REQUIRE(panel_idle_target_count(&data) == attached);
            REQUIRE(popover_open(popup));
            REQUIRE(panel_idle_measure(s, &sources, &data, "running-reopened"));
            popover_hide(popup);
            studio_command_run(s, STUDIO_COMMAND_STOP);
            break;
        }
        case 9: REQUIRE(!studio_session(s)); break;
        case 10:
            REQUIRE(!studio_session(s));
            REQUIRE(panel_idle_measure(s, &sources, &data, "stopped"));
            studio_sources_free(&sources);
            ui_data_free(&data);
            break;
    }
    return true;
}

TEST_CASE(panel_idle)
{
    REQUIRE(unchanged_source());
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "10",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project,
                     "--open",           "Counter" };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == PANEL_IDLE_FRAMES);
    REQUIRE(unchanged_source());
    return true;
}
