/* Pointer and keyboard coverage of the authored Outliner. Included by the
   acceptance dispatcher after its SDL and element helpers. */
#include "ui/drag.h"

#define OUTLINER_MOVE_FRAMES 21

static float outliner_x, outliner_y;
static uint64_t outliner_ids[3];

static int outliner_actor(studio *s, const char *name)
{
    for (int i = 0; i < acount(s->scene->actors); i++) {
        if (!strcmp(s->scene->actors[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static rml_element *outliner_row(studio *s, const char *name)
{
    if (!name) {
        return studio_element(s, ".outliner-root");
    }
    int index = outliner_actor(s, name);
    if (index < 0) {
        return NULL;
    }
    char selector[96];
    snprintf(
        selector,
        sizeof selector,
        "[data-object='%llu']",
        (unsigned long long)ecs_entity_to_u64(s->scene->actors[index].entity)
    );
    return studio_element(s, selector);
}

static bool outliner_point(studio *s, const char *name, float zone)
{
    rml_element *row = outliner_row(s, name);
    if (!row) {
        rml_element *list = studio_element(s, "#outliner-list");
        char markup[2048] = "";
        if (list) {
            rml_element_inner_rml(list, markup, sizeof markup);
        }
        fprintf(
            stderr,
            "Outliner frame %d, missing row %s; list=%s\n",
            frames,
            name ? name : "root",
            list ? markup : "absent"
        );
    }
    REQUIRE(row);
    rml_vec2 at = rml_element_absolute_offset(row, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(row, RML_BOX_BORDER);
    REQUIRE(size.x > 0 && size.y > 0);
    /* RmlUi measures pixels; SDL mouse events carry window points. */
    float points = s->prefs.ui_scale / studio_pixel_ratio(s);
    outliner_x = (at.x + size.x * 0.4f) * points;
    outliner_y = (at.y + size.y * zone) * points;
    return push_mouse(SDL_EVENT_MOUSE_MOTION, outliner_x, outliner_y, 0, 0);
}

static bool outliner_press(studio *s, const char *name)
{
    REQUIRE(outliner_point(s, name, 0.5f));
    return push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, outliner_x, outliner_y, 0, 0);
}

static bool outliner_release(studio *s, const char *name, const char *zone, bool rejected)
{
    ui_drag_item item = { 0 };
    rml_element *row = outliner_row(s, name);
    bool over = row && ui_drag_over(row, &item);
    if (!over) {
        rml_context *context = rml_backend_context(s->backend);
        ui_drag_item active = { 0 };
        bool dragging = ui_drag_active(context, &active);
        rml_element *hover = rml_context_hover_element(context);
        char object[24] = "", source[32] = "", target[160] = "";
        for (rml_element *at = hover; at; at = rml_element_parent(at)) {
            if (!object[0]) {
                rml_element_attribute(at, "data-object", object, sizeof object);
            }
            if (!source[0]) {
                rml_element_attribute(at, "data-drag", source, sizeof source);
            }
            if (!target[0]) {
                rml_element_attribute(at, "data-drop", target, sizeof target);
            }
        }
        fprintf(
            stderr,
            "Outliner frame %d, drop on %s: active=%d type=%s payload=%s "
            "zone=%s "
            "pointer=(%g,%g) down=%d hover=%s object=%s source=%s target=%s\n",
            frames,
            name ? name : "root",
            dragging,
            active.type ? active.type : "",
            active.payload ? active.payload : "",
            active.zone ? active.zone : "",
            s->in.mouse_x,
            s->in.mouse_y,
            s->in.down[0],
            hover ? rml_element_tag_name(hover) : "none",
            object,
            source,
            target
        );
    }
    REQUIRE(over);
    REQUIRE(!strcmp(item.type, "object") && !strcmp(item.zone, zone));
    REQUIRE((studio_element(s, "[data-drop-rejected]") != NULL) == rejected);
    return push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, outliner_x, outliner_y, 0, 0);
}

static bool outliner_order(studio *s, const char *names)
{
    REQUIRE(acount(s->scene->actors) == 3);
    for (int i = 0; i < 3; i++) {
        const scene_actor *actor = &s->scene->actors[i];
        REQUIRE(actor->name[0] == names[i] && !actor->name[1]);
        REQUIRE(actor->identity == outliner_ids[names[i] - 'A']);
    }
    return true;
}

static bool outliner_select(studio *s, bool group)
{
    int b = outliner_actor(s, "B"), c = outliner_actor(s, "C");
    REQUIRE(b >= 0 && c >= 0);
    select_instance(s->scene, b);
    if (group) {
        scene_toggle_instance(s->scene, c);
    }
    return true;
}

static bool outliner_key(studio *s, SDL_Keycode key)
{
    rml_element *list = studio_element(s, "#outliner-list");
    REQUIRE(list && rml_element_focus(list, true));
    s->focus_area = STUDIO_AREA_OUTLINER;
    return press_chord(key, SDL_KMOD_ALT);
}

/* The shared target also takes asset assignments. Use native context events
   to check that the same top edge stays a whole-row target for that type;
   cancel before any authored asset-assignment handler can run. */
static bool outliner_asset_zone(studio *s)
{
    rml_context *context = rml_backend_context(s->backend);
    rml_element *source = outliner_row(s, "B");
    rml_element *target = outliner_row(s, "A");
    REQUIRE(source && target);
    rml_vec2 from = rml_element_absolute_offset(source, RML_BOX_BORDER);
    rml_vec2 size = rml_element_box_size(source, RML_BOX_BORDER);
    rml_vec2 to = rml_element_absolute_offset(target, RML_BOX_BORDER);
    rml_vec2 target_size = rml_element_box_size(target, RML_BOX_BORDER);
    rml_element_set_attribute(source, "data-drag", "behavior");
    rml_context_process_mouse_move(context, from.x + size.x * 0.4f, from.y + size.y * 0.5f, 0);
    rml_context_process_mouse_button_down(context, 0, 0);
    rml_context_process_mouse_move(
        context,
        to.x + target_size.x * 0.4f,
        to.y + target_size.y * 0.1f,
        0
    );
    ui_drag_item item;
    REQUIRE(ui_drag_over(target, &item) && !strcmp(item.type, "behavior"));
    REQUIRE(!item.zone[0] && !rml_element_has_attribute(target, "data-drop-zone"));
    ui_drag_cancel(context);
    rml_context_process_mouse_button_up(context, 0, 0);
    rml_element_set_attribute(source, "data-drag", "object");
    REQUIRE(!ui_drag_active(context, NULL));
    return true;
}

static bool outliner_move_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->ui);
    app_scene_authoring *scene = s->scene;
    frames++;
    switch (frames) {
        case 1:
            s->maximized = STUDIO_AREA_OUTLINER;
            s->prefs.reduced_motion = true;
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
                    "data.Value.Text(\"MoveBox\")),\n"
                    "            data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                    data.Field.Named(\"x\", "
                    "data.Value.UInt(0)),\n"
                    "                    data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                ]))\n"
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
                    "data.Value.Text(\"Scene\")),\n"
                    "            data.Field.Named(\"entities\", data.List([\n"
                    "                data.Object([\n"
                    "                    data.Field.Named(\"name\", "
                    "data.Value.Text(\"A\")),\n"
                    "                    data.Field.Named(\"prototype\", "
                    "data.Value.Text(\"MoveBox\")),\n"
                    "                    data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                        data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                            data.Field.Named(\"x\", "
                    "data.Value.UInt(100)),\n"
                    "                            data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                        ]))\n"
                    "                    ]))\n"
                    "                ]),\n"
                    "                data.Object([\n"
                    "                    data.Field.Named(\"name\", "
                    "data.Value.Text(\"B\")),\n"
                    "                    data.Field.Named(\"prototype\", "
                    "data.Value.Text(\"MoveBox\")),\n"
                    "                    data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                        data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                            data.Field.Named(\"x\", "
                    "data.Value.UInt(20)),\n"
                    "                            data.Field.Named(\"y\", "
                    "data.Value.UInt(0))\n"
                    "                        ]))\n"
                    "                    ]))\n"
                    "                ]),\n"
                    "                data.Object([\n"
                    "                    data.Field.Named(\"name\", "
                    "data.Value.Text(\"C\")),\n"
                    "                    data.Field.Named(\"prototype\", "
                    "data.Value.Text(\"MoveBox\")),\n"
                    "                    data.Field.Named(\"components\", "
                    "data.Object([\n"
                    "                        data.Field.Named(\"Transform\", "
                    "data.Object([\n"
                    "                            data.Field.Named(\"x\", "
                    "data.Value.UInt(30)),\n"
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
            for (int i = 0; i < 3; i++) {
                outliner_ids[i] = scene->actors[i].identity;
            }
            return outliner_select(s, true);
        /* A List publishes its new window after the changed scene and
           maximized layout settle. The next frame emits those rows. */
        case 2: return true;
        case 3: return outliner_press(s, "B");
        case 4: return outliner_point(s, "A", 0.5f);
        case 5: return outliner_release(s, "A", "inside", false);
        case 6:
            REQUIRE(outliner_order(s, "ABC"));
            REQUIRE(child_of(scene, 1, 0) && child_of(scene, 2, 0));
            REQUIRE(scene_instance_selected(scene, 1) && scene_instance_selected(scene, 2));
            REQUIRE(
                fabsf(Transform_get_const(scene->world, scene->actors[1].entity)->x + 80) < 1e-4f
            );
            REQUIRE(
                fabsf(Transform_get_const(scene->world, scene->actors[2].entity)->x + 70) < 1e-4f
            );
            select_instance(scene, 0);
            return outliner_press(s, "A");
        case 7: return outliner_point(s, "B", 0.5f);
        case 8: return outliner_release(s, "B", "inside", true);
        case 9: {
            REQUIRE(outliner_order(s, "ABC"));
            REQUIRE(child_of(scene, 1, 0) && child_of(scene, 2, 0));
            REQUIRE(scene->message[0]);
            /* The first reparent has completed its authored-row layout. */
            rml_element *a = rml_element_query_selector(outliner_row(s, "A"), ".name");
            rml_element *b = rml_element_query_selector(outliner_row(s, "B"), ".name");
            REQUIRE(a && b);
            REQUIRE(
                rml_element_absolute_offset(b, RML_BOX_BORDER).x >
                rml_element_absolute_offset(a, RML_BOX_BORDER).x + 10
            );
            REQUIRE(outliner_select(s, true));
            return outliner_key(s, SDLK_LEFT);
        }
        case 10:
            REQUIRE(outliner_order(s, "ABC"));
            REQUIRE(!ecs_parent(scene->world, scene->actors[1].entity).slot);
            REQUIRE(!ecs_parent(scene->world, scene->actors[2].entity).slot);
            return outliner_key(s, SDLK_UP);
        case 11:
            REQUIRE(outliner_order(s, "BCA"));
            select_instance(scene, 2);
            return outliner_press(s, "A");
        case 12: return outliner_point(s, "B", 0.1f);
        case 13: return outliner_release(s, "B", "before", false);
        case 14:
            REQUIRE(outliner_order(s, "ABC"));
            return outliner_press(s, "A");
        case 15: return outliner_point(s, "C", 0.9f);
        case 16: return outliner_release(s, "C", "after", false);
        case 17:
            REQUIRE(outliner_order(s, "BCA"));
            return outliner_press(s, "B");
        case 18: return outliner_point(s, NULL, 0.5f);
        case 19: return outliner_release(s, NULL, "", false);
        case 20:
            REQUIRE(outliner_order(s, "CAB"));
            REQUIRE(scene->selected == 2 && scene_instance_selected(scene, 2));
            return outliner_key(s, SDLK_RIGHT);
        case 21: {
            REQUIRE(outliner_order(s, "CAB"));
            REQUIRE(child_of(scene, 2, 1));
            REQUIRE(
                fabsf(Transform_get_const(scene->world, scene->actors[2].entity)->x + 80) < 1e-4f
            );
            REQUIRE(outliner_asset_zone(s));
            edits++;
            return true;
        }
        default: return false;
    }
}

TEST_CASE(outliner_move)
{
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         FRAMES_TEXT(OUTLINER_MOVE_FRAMES),
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == OUTLINER_MOVE_FRAMES && edits == 1);
    printf("\nStudio outliner move accepted: frames=%d\n", frames);
    return true;
}
