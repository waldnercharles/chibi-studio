static const char
    included_world[] = "name \"World\"\n(array)includes "
                       "\"ReactionQueue\"\ncomponents { Transform y=2.0; }\n";
static const char
    edited_world[] = "name \"World\"\n(array)includes "
                     "\"ReactionQueue\"\ncomponents { Transform y=12.0; }\n";
static const char reaction_slot[] = "name \"ReactionSlot\"\ncomponents { "
                                    "Transform x=3.0; }\n";

static bool write_queue(int x)
{
    char text[1024];
    snprintf(
        text,
        sizeof text,
        "name \"ReactionQueue\"\ncomponents { Transform x=%d.0; Link "
        "target=\"Reaction0\"; }\n"
        "(array)children {\n"
        " - name=\"Reaction0\" prototype=\"ReactionSlot\" { components { Link "
        "target=\"Reaction1\"; }; }\n"
        " - name=\"Reaction1\" prototype=\"ReactionSlot\" { components { "
        "Transform x=4.0; Link target=\"Reaction0\"; }; }\n"
        "}\n",
        x
    );
    return write_content("prototypes", "Queue.kdl", text);
}

static bool world_source_is(const char *expected)
{
    char *path = content_path_join(project, "content/prototypes/World.kdl");
    char *text = file_read_text(path);
    REQUIRE(text);
    bool same = !strcmp(text, expected);
    free(text);
    free(path);
    REQUIRE(same);
    return true;
}

static bool structured_save_preserves_source(app_scene_authoring *scene, const char *name)
{
    scene_prototype_asset *asset = find_prototype(scene, name);
    REQUIRE(asset && asset->path);
    char *before = read_scene_file(asset->path);
    REQUIRE(before);
    REQUIRE(app_scene_authoring_save_prototype(scene, name, asset->path) != 0);
    REQUIRE(strstr(scene->prototype_message, "Text Editor"));
    char *after = read_scene_file(asset->path);
    REQUIRE(after);
    bool same = !strcmp(before, after);
    free(before);
    free(after);
    REQUIRE(same);
    return true;
}

static ecs_entity linked_entity(app_scene_authoring *scene, ecs_entity from)
{
    pin_value link = { 0 };
    int result = ecs_component_read_field(scene->world, from, link_id(scene), 0, &link);
    assert(result == ECS_OK);
    return ecs_entity_from_u64(link.u64);
}

static bool queue_ring_intact(app_scene_authoring *scene, ecs_entity world)
{
    REQUIRE(ecs_pair_source_count(scene->world, ECS_CHILD_OF, world) == 2);
    ecs_entity first = linked_entity(scene, world);
    REQUIRE(ecs_entity_is_alive(scene->world, first));
    ecs_entity second = linked_entity(scene, first);
    REQUIRE(ecs_entity_is_alive(scene->world, second));
    REQUIRE(ecs_entity_to_u64(first) != ecs_entity_to_u64(second));
    REQUIRE(ecs_entity_to_u64(linked_entity(scene, second)) == ecs_entity_to_u64(first));
    REQUIRE(ecs_entity_to_u64(ecs_parent(scene->world, first)) == ecs_entity_to_u64(world));
    REQUIRE(ecs_entity_to_u64(ecs_parent(scene->world, second)) == ecs_entity_to_u64(world));
    return true;
}

static bool included_runtime_restores(app_scene_authoring *scene)
{
    ecs_entity
        disabled = linked_entity(scene, linked_entity(scene, scene->actors[0].entity));
    REQUIRE(ecs_world_record_add(scene->world, disabled, ECS_DISABLED, NULL, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(scene->world) == ECS_OK);
    for (int run = 0; run < 2; run++) {
        ecs_entity before = scene->actors[0].entity;
        ecs_entity old_child = linked_entity(scene, before);
        REQUIRE(
            app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0
        );
        Transform_get(scene->world, before)->y = 50;
        Transform_get(scene->world, old_child)->x = 60;
        REQUIRE(
            app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, run ? APP_RUNTIME_RESTART : APP_RUNTIME_STOP) ==
            0
        );
        ecs_entity restored = scene->actors[0].entity;
        REQUIRE(queue_ring_intact(scene, restored));
        ecs_entity child = linked_entity(scene, restored);
        REQUIRE(!ecs_entity_is_alive(scene->world, before));
        REQUIRE(!ecs_entity_is_alive(scene->world, old_child));
        REQUIRE(Transform_get_const(scene->world, restored)->y == 2.0f);
        REQUIRE(Transform_get_const(scene->world, child)->x == 3.0f);
        REQUIRE(ecs_component_has(scene->world, linked_entity(scene, child), ECS_DISABLED));
        if (run) {
            REQUIRE(
                app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0
            );
            restored = scene->actors[0].entity;
            REQUIRE(queue_ring_intact(scene, restored));
            REQUIRE(!ecs_entity_is_alive(scene->world, child));
            child = linked_entity(scene, restored);
            REQUIRE(Transform_get_const(scene->world, child)->x == 3.0f);
            REQUIRE(ecs_component_has(scene->world, linked_entity(scene, child), ECS_DISABLED));
        }
    }
    disabled = linked_entity(scene, linked_entity(scene, scene->actors[0].entity));
    REQUIRE(ecs_world_record_remove(scene->world, disabled, ECS_DISABLED, 0) == ECS_OK);
    REQUIRE(ecs_world_apply_commands(scene->world) == ECS_OK);
    return true;
}

static bool prototype_includes_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->scene);
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 2) {
        return true;
    }
    app_scene_authoring *scene = s->scene;
    scene_prototype_asset *asset = find_prototype(scene, "World");
    REQUIRE(asset && acount(asset->prototype->includes) == 1);
    REQUIRE(acount(asset->prototype->child_prototypes) == 2);
    REQUIRE(acount(scene->actors) == 1);
    REQUIRE(queue_ring_intact(scene, scene->actors[0].entity));
    REQUIRE(structured_save_preserves_source(scene, "ReactionQueue"));
    REQUIRE(structured_save_preserves_source(scene, "ReactionSlot"));
    REQUIRE(included_runtime_restores(scene));
    ecs_entity world = scene->actors[0].entity;
    ecs_entity child = linked_entity(scene, world);
    REQUIRE(Transform_get_const(scene->world, child)->x == 3.0f);
    REQUIRE(Transform_get_const(scene->world, world)->y == 2.0f);

    prototype_history_before(scene, asset->prototype);
    REQUIRE(
        prototype_write_field(asset->prototype, "Transform", "y", (pin_value){ .type = PIN_F32, .f = 8 }) == 0
    );
    REQUIRE(app_scene_authoring_save_prototype(scene, "World", asset->path) != 0);
    REQUIRE(strstr(scene->prototype_message, "Text Editor"));
    REQUIRE(world_source_is(included_world));
    scene->selected_prototype = (int)(asset - scene->prototypes);
    REQUIRE(prototype_undo(scene) == 0);
    REQUIRE(app_scene_authoring_save_prototype(scene, "World", asset->path) != 0);
    REQUIRE(world_source_is(included_world));

    Transform_get(scene->world, child)->x = 9;
    REQUIRE(write_content("prototypes", "World.kdl", edited_world));
    REQUIRE(app_scene_authoring_reload_path(scene, asset->path) == APP_DISK_RELOADED);
    REQUIRE(world_source_is(edited_world));
    REQUIRE(acount(asset->prototype->includes) == 1);
    REQUIRE(queue_ring_intact(scene, world));
    REQUIRE(ecs_entity_is_alive(scene->world, child));
    REQUIRE(ecs_entity_to_u64(linked_entity(scene, world)) == ecs_entity_to_u64(child));
    REQUIRE(Transform_get_const(scene->world, child)->x == 9.0f);
    REQUIRE(Transform_get_const(scene->world, world)->y == 12.0f);

    REQUIRE(write_queue(5));
    scene_prototype_asset *queue = find_prototype(scene, "ReactionQueue");
    REQUIRE(queue);
    asset->dirty = 1;
    REQUIRE(app_scene_authoring_reload_path(scene, queue->path) == APP_DISK_FAILED);
    REQUIRE(strstr(scene->message, "Save or reload World"));
    REQUIRE(Transform_get_const(scene->world, world)->x == 1.0f);
    asset->dirty = 0;
    REQUIRE(app_scene_authoring_reload_path(scene, queue->path) == APP_DISK_RELOADED);
    REQUIRE(Transform_get_const(scene->world, world)->x == 5.0f);
    REQUIRE(queue_ring_intact(scene, world));
    REQUIRE(ecs_entity_is_alive(scene->world, child));
    REQUIRE(Transform_get_const(scene->world, child)->x == 9.0f);

    REQUIRE(
        write_content("prototypes", "World.kdl", "name \"World\"\n(array)includes \"ReactionQueue\"\ncomponents { Transform y=12.0; Link target=\"Reaction1\"; }\n")
    );
    REQUIRE(app_scene_authoring_reload_path(scene, asset->path) == APP_DISK_FAILED);
    REQUIRE(strstr(scene->message, "Reopen the project"));
    REQUIRE(queue_ring_intact(scene, world));
    REQUIRE(ecs_entity_to_u64(linked_entity(scene, world)) == ecs_entity_to_u64(child));
    REQUIRE(write_content("prototypes", "World.kdl", edited_world));
    REQUIRE(app_scene_authoring_reload_path(scene, asset->path) == APP_DISK_RELOADED);

    REQUIRE(
        write_content("prototypes", "Slot.kdl", "name \"ReactionSlot\"\ncomponents { Transform x=7.0; }\n")
    );
    scene_prototype_asset *slot = find_prototype(scene, "ReactionSlot");
    REQUIRE(slot);
    REQUIRE(app_scene_authoring_reload_path(scene, slot->path) == APP_DISK_FAILED);
    REQUIRE(strstr(scene->message, "Reopen the project"));
    REQUIRE(Transform_get_const(scene->world, child)->x == 9.0f);
    REQUIRE(queue_ring_intact(scene, world));
    REQUIRE(write_content("prototypes", "Slot.kdl", reaction_slot));
    REQUIRE(app_scene_authoring_reload_path(scene, slot->path) == APP_DISK_RELOADED);

    REQUIRE(
        write_content("prototypes", "Queue.kdl", "name \"ReactionQueue\"\n(array)includes \"World\"\n")
    );
    REQUIRE(app_scene_authoring_reload_path(scene, queue->path) == APP_DISK_FAILED);
    REQUIRE(strstr(scene->message, "cycle"));
    REQUIRE(ecs_entity_is_alive(scene->world, child));
    REQUIRE(Transform_get_const(scene->world, world)->y == 12.0f);
    REQUIRE(queue_ring_intact(scene, world));
    REQUIRE(write_queue(5));
    REQUIRE(app_scene_authoring_reload_path(scene, queue->path) == APP_DISK_RELOADED);
    return true;
}

TEST_CASE(prototype_includes)
{
    REQUIRE(write_content("components", "Link.chibi", link_component));
    REQUIRE(write_content("prototypes", "Slot.kdl", reaction_slot));
    REQUIRE(write_queue(1));
    REQUIRE(write_content("prototypes", "World.kdl", included_world));
    REQUIRE(
        write_content("scenes", "Start.kdl", "version 1\nname \"Start\"\n(array)entities { - name=\"World\" prototype=\"World\"; }\n")
    );
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 2);
    REQUIRE(world_source_is(edited_world));
    return true;
}
