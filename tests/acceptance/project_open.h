#include "editor/studio/studio_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Two rejected projects exercise only project loading, before UI resources exist. */
static const char *const project_open_bytes[] = {
    "version \"invalid\"\n",
    "version 1000000\n"
    "name \"Newer project\"\n"
    "content \"content\"\n"
    "start_scene \"scenes/Start.kdl\"\n",
    "behavior Broken {\n"
};
static char project_open_directory[128];
static char project_open_roots[2][256], project_open_content[256];
static char project_open_files[3][384];
static bool project_open_owns_directory;
static studio_ui *project_open_results[2];

static bool project_open_path(char *path, size_t size, const char *root, const char *name)
{
    int length = snprintf(path, size, "%s/%s", root, name);
    REQUIRE(length > 0 && (size_t)length < size);
    return true;
}

static void project_open_cleanup()
{
    for (int i = 0; i < 2; i++) {
        studio_ui_free(project_open_results[i]);
        project_open_results[i] = NULL;
    }
    if (!project_open_owns_directory) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (!project_open_files[i][0]) {
            continue;
        }
        unlink(project_open_files[i]);
    }
    rmdir(project_open_content);
    for (int i = 0; i < 2; i++) {
        rmdir(project_open_roots[i]);
    }
    rmdir(project_open_directory);
    project_open_owns_directory = false;
}

static bool project_open_prepare()
{
    strcpy(project_open_directory, "/tmp/chibi-studio-project-open-XXXXXX");
    REQUIRE(mkdtemp(project_open_directory));
    project_open_owns_directory = true;
    REQUIRE(
        project_open_path(project_open_roots[0], sizeof project_open_roots[0], project_open_directory, "manifest")
    );
    REQUIRE(
        project_open_path(project_open_roots[1], sizeof project_open_roots[1], project_open_directory, "newer")
    );
    REQUIRE(
        project_open_path(project_open_content, sizeof project_open_content, project_open_roots[1], "content")
    );
    for (int i = 0; i < 2; i++) {
        REQUIRE(mkdir(project_open_roots[i], 0700) == 0);
        REQUIRE(project_open_path(
            project_open_files[i],
            sizeof project_open_files[i],
            project_open_roots[i],
            "project.kdl"
        ));
    }
    REQUIRE(mkdir(project_open_content, 0700) == 0);
    REQUIRE(
        project_open_path(project_open_files[2], sizeof project_open_files[2], project_open_content, "Broken.chibi")
    );
    for (int i = 0; i < 3; i++) {
        FILE *file = fopen(project_open_files[i], "wb");
        REQUIRE(file);
        size_t size = strlen(project_open_bytes[i]);
        bool written = fwrite(project_open_bytes[i], 1, size, file) == size;
        int closed = fclose(file);
        REQUIRE(written && closed == 0);
    }
    return true;
}

static bool project_open_unchanged()
{
    for (int i = 0; i < 3; i++) {
        FILE *file = fopen(project_open_files[i], "rb");
        REQUIRE(file);
        char actual[256];
        size_t length = fread(actual, 1, sizeof actual, file);
        bool complete = feof(file) && !ferror(file);
        int closed = fclose(file);
        REQUIRE(complete && closed == 0);
        REQUIRE(length == strlen(project_open_bytes[i]));
        REQUIRE(!memcmp(actual, project_open_bytes[i], length));
    }
    return true;
}

TEST_CASE(project_open_errors_stop_before_ui)
{
    REQUIRE(project_open_prepare());
    char *manifest = realpath(project_open_files[0], NULL);
    REQUIRE(manifest);
    char expected[512];
    int length = snprintf(expected, sizeof expected, "Project manifest needs an integer version: %s", manifest);
    free(manifest);
    REQUIRE(length > 0 && (size_t)length < sizeof expected);

    studio s = { 0 };
    char error[512] = { 0 };
    /* The red control can abort before teardown; identify its exact inputs. */
    fprintf(stderr, "Studio project-open fixture: %s\n", project_open_directory);
    fflush(stderr);
    /* A NULL backend makes reaching rml_backend_context an immediate failure. */
    project_open_results[0] = studio_ui_new(&s, project_open_roots[0], error, sizeof error);
    REQUIRE(!project_open_results[0]);
    REQUIRE(!strcmp(error, expected));

    manifest = realpath(project_open_files[1], NULL);
    REQUIRE(manifest);
    length = snprintf(
        expected,
        sizeof expected,
        "Project version 1000000 needs a newer build; this build opens version "
        "%d: %s",
        CHIBI_PROJECT_VERSION,
        manifest
    );
    free(manifest);
    REQUIRE(length > 0 && (size_t)length < sizeof expected);
    project_open_results[1] = studio_ui_new(&s, project_open_roots[1], error, sizeof error);
    REQUIRE(!project_open_results[1]);
    REQUIRE(!strcmp(error, expected));
    REQUIRE(project_open_unchanged());
    return true;
}
