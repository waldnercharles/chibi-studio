#include "authoring/shader_prepare.h"
#include "editor/studio/studio_nodes.h"
#include "render/shader_catalog.h"
#include "render/shader_values.h"
#include "studio_registry.h"

bool studio_acceptance_publish_ui_sources(preview_world *, const char *, char *, size_t);

static ecs_world *shader_entities;
static preview_world *shader_ui;
static studio_node_service shader_nodes;

static void shader_ui_cleanup()
{
    preview_world_free(shader_ui);
    ecs_world_free(shader_entities);
    shader_ui = NULL;
    shader_entities = NULL;
    authoring_shader_catalog_clear();
}

TEST_CASE(editor_ui_publication_preserves_project_particle_shaders)
{
    shader_prepared_asset particle = {
        .name = "Project.Particle",
        .kind = SHADER_ASSET_PARTICLE_TEXT,
        .text = { .path = "shaders/Project.glsl", .entry = "particle", .code = "ChibiParticle particle(ChibiParticleInput p, ChibiParameters u) { return chibi_particle(); }" },
        .names = { "Index", "Count", "Seed" },
        .widths = { 1, 1, 1 },
    };
    char error[2048] = { 0 };
    REQUIRE(authoring_shader_sources_publish(&particle, 1, true, error, sizeof error));
    int id = render_shader_find("Project.Particle");
    REQUIRE(render_shader_is_particle(id));
    uint64_t generation = render_shader_generation(id);
    shader_entities = ecs_world_new();
    shader_ui = preview_world_new(shader_entities);
    REQUIRE(shader_ui);
    shader_nodes = (studio_node_service){ NULL, studio_node_run };
    REQUIRE(source_bind_service(shader_ui->runtime->sources, "studio", &shader_nodes) == 0);
    for (int i = 0; i < 2; i++) {
        bool published = studio_acceptance_publish_ui_sources(shader_ui, project, error, sizeof error);
        if (!published) {
            fprintf(stderr, "Studio UI source publication: %s\n", error);
        }
        REQUIRE(published);
        REQUIRE(render_shader_find("Project.Particle") == id);
        REQUIRE(render_shader_is_particle(id));
        REQUIRE(render_shader_generation(id) == generation);
        REQUIRE(authoring_sources_count(preview_sources_applied(shader_ui)) > 0);
        execution_module_event_info event;
        REQUIRE(source_event(shader_ui->runtime->sources, "RowClicked", &event));
        REQUIRE(event.field_count == 3);
    }
    return true;
}
