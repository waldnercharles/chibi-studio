/* New Project's native folder chooser, without a renderer, SDK or compiler. */
#include "editor/studio/project_location.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char project_location_root[128];
static bool project_location_ready;

static void project_location_setup()
{
    snprintf(project_location_root, sizeof project_location_root, "/tmp/studio-project-location-XXXXXX");
    project_location_ready = mkdtemp(project_location_root) != NULL;
}

static void project_location_cleanup()
{
    if (!project_location_ready) {
        return;
    }
    const char *names[] = { "my-project", "my-project-2", "my-project-3", "my-project-4" };
    for (int i = 0; i < 4; i++) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", project_location_root, names[i]);
        unlink(path);
        rmdir(path);
    }
    rmdir(project_location_root);
    project_location_ready = false;
}

TEST_CASE(project_location_unused)
{
    REQUIRE(project_location_ready);
    char path[256], expected[256];
    snprintf(expected, sizeof expected, "%s/my-project", project_location_root);
    REQUIRE(studio_project_location(project_location_root, "My Project", path, sizeof path));
    REQUIRE(!strcmp(path, expected));
    struct stat info;
    REQUIRE(lstat(path, &info) != 0 && errno == ENOENT);
    return true;
}

TEST_CASE(project_location_occupied)
{
    REQUIRE(project_location_ready);
    char path[256], file_path[256], expected[256];
    snprintf(path, sizeof path, "%s/my-project", project_location_root);
    REQUIRE(mkdir(path, 0700) == 0);
    snprintf(file_path, sizeof file_path, "%s/my-project-2", project_location_root);
    FILE *file = fopen(file_path, "wb");
    REQUIRE(file);
    bool written = fputs("keep", file) >= 0;
    REQUIRE(fclose(file) == 0 && written);
    snprintf(path, sizeof path, "%s/my-project-3", project_location_root);
    REQUIRE(symlink("my-project", path) == 0);
    snprintf(expected, sizeof expected, "%s/my-project-4", project_location_root);
    REQUIRE(studio_project_location(project_location_root, "My Project", path, sizeof path));
    REQUIRE(!strcmp(path, expected));
    file = fopen(file_path, "rb");
    REQUIRE(file);
    char bytes[5] = "";
    bool read = fread(bytes, 1, 4, file) == 4;
    REQUIRE(fclose(file) == 0 && read);
    REQUIRE(!strcmp(bytes, "keep"));
    return true;
}

TEST_CASE(project_location_dangling_link)
{
    REQUIRE(project_location_ready);
    char link[256], path[256], expected[256];
    snprintf(link, sizeof link, "%s/my-project", project_location_root);
    REQUIRE(symlink("missing-target", link) == 0);
    snprintf(expected, sizeof expected, "%s/my-project-2", project_location_root);
    REQUIRE(studio_project_location(project_location_root, "My Project", path, sizeof path));
    REQUIRE(!strcmp(path, expected));
    REQUIRE(mkdir(path, 0700) == 0);
    char target[64] = "";
    REQUIRE(readlink(link, target, sizeof target - 1) == (ssize_t)strlen("missing-target"));
    REQUIRE(!strcmp(target, "missing-target"));
    return true;
}

TEST_CASE(project_location_looping_link)
{
    REQUIRE(project_location_ready);
    char link[256], path[256], expected[256];
    snprintf(link, sizeof link, "%s/my-project", project_location_root);
    REQUIRE(symlink("my-project", link) == 0);
    snprintf(expected, sizeof expected, "%s/my-project-2", project_location_root);
    REQUIRE(studio_project_location(project_location_root, "My Project", path, sizeof path));
    REQUIRE(!strcmp(path, expected));
    return true;
}

TEST_CASE(project_location_errors)
{
    REQUIRE(project_location_ready);
    char path[4];
    REQUIRE(!studio_project_location(project_location_root, "My Project", path, sizeof path));
    REQUIRE(errno == ENAMETOOLONG);
    char parent[256], full[256];
    snprintf(parent, sizeof parent, "%s/my-project", project_location_root);
    FILE *file = fopen(parent, "wb");
    REQUIRE(file);
    REQUIRE(fclose(file) == 0);
    REQUIRE(!studio_project_location(parent, "Child", full, sizeof full));
    REQUIRE(errno == ENOTDIR);
    return true;
}

static void project_location_tests()
{
    pu_setup(project_location_setup, project_location_cleanup);
    RUN_TEST_CASE(project_location_unused);
    RUN_TEST_CASE(project_location_occupied);
    RUN_TEST_CASE(project_location_dangling_link);
    RUN_TEST_CASE(project_location_looping_link);
    RUN_TEST_CASE(project_location_errors);
    pu_clear_setup();
}
