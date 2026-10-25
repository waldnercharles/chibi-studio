#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define PICO_UNIT_IMPLEMENTATION
#include "vendor/pico_unit.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *studio = "c28301748699a158fec08a475ca7dde872bb4960";
static const char *old_studio = "b8cb89254976a957a3a79109e7df74523cd718cd";
static const char *tree = "9cfd1b700234e09df5f5611792e94e36e08127da";
static const char *engine = "d1154cdb8677cbfe479d4be40db7ca480d10f103";
static const char *files[] = { "chibi-studio.html",
                               "chibi-studio.js",
                               "chibi-studio.wasm",
                               "chibi-studio.data",
                               "chibi-default.js" };
static const char *fixture_hash = "e80b71cd14d3cbd65f4173abcbfcf01a545dbca32a72"
                                  "d575108b553a648cc96f";
static const char *empty_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b93"
                                "4ca495991b7852b855";
static char *script, *tools, *previous, *built, *output, *pr, *log_path;
static char temporary[128];

static char *join(const char *a, const char *b)
{
    size_t size = strlen(a) + strlen(b) + 2;
    char *path = malloc(size);
    assert(path);
    snprintf(path, size, "%s/%s", a, b);
    return path;
}

static void write_at(const char *directory, const char *name, const char *text)
{
    char *path = join(directory, name);
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
    free(path);
}

static bool has(const char *directory, const char *name, const char *text)
{
    char *path = join(directory, name);
    FILE *file = fopen(path, "rb");
    free(path);
    if (!file) {
        return false;
    }
    char buffer[4096];
    size_t length = fread(buffer, 1, sizeof buffer - 1, file);
    buffer[length] = '\0';
    fclose(file);
    return strstr(buffer, text) != NULL;
}

static bool exists(const char *directory, const char *name)
{
    char *path = join(directory, name);
    bool found = access(path, F_OK) == 0;
    free(path);
    return found;
}

static int run(char *const arguments[], bool quiet)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        if (quiet) {
            int log = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
            assert(log >= 0 && dup2(log, STDOUT_FILENO) >= 0 && dup2(log, STDERR_FILENO) >= 0);
            close(log);
        }
        execvp(arguments[0], arguments);
        _exit(127);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void site(const char *directory, const char *revision)
{
    assert(mkdir(directory, 0700) == 0);
    for (int i = 0; i < 5; i++) {
        write_at(directory, files[i], "fixture\n");
    }
    write_at(directory, "index.html", "fixture\n");
    write_at(directory, ".nojekyll", "");
    char archive[160], identity[4096];
    snprintf(archive, sizeof archive, "editor-%s-%s.tar.gz", revision, engine);
    write_at(directory, archive, "");
    int at = snprintf(
        identity,
        sizeof identity,
        "{\"format\":1,\"studio\":\"%s\",\"tree\":\"%s\",\"engine\":\"%s\","
        "\"archive\":\"%s\",\"sha256\":\"%s\",\"web_threads\":false,"
        "\"sdk_build_id\":\"%s\",\"files\":{",
        revision,
        tree,
        engine,
        archive,
        empty_hash,
        fixture_hash
    );
    for (int i = 0; i < 5; i++) {
        at += snprintf(
            identity + at,
            sizeof identity - (size_t)at,
            "%s\"%s\":\"%s\"",
            i ? "," : "",
            files[i],
            fixture_hash
        );
    }
    assert(at > 0 && (size_t)at + 4 < sizeof identity);
    strcat(identity, "}}\n");
    write_at(directory, "studio.json", identity);
}

static void pr_state(bool merged, bool open)
{
    char text[128];
    snprintf(
        text,
        sizeof text,
        "{\"number\":77,\"merged\":%s,\"state\":\"%s\"}",
        merged ? "true" : "false",
        open ? "open" : "closed"
    );
    write_at(temporary, "pr.json", text);
}

static int compose(const char *mode, const char *main)
{
    char *arguments[] = { "python3",    script,       "compose",    "--mode",
                          (char *)mode, "--previous", previous,     "--built",
                          built,        "--output",   output,       "--pr",
                          pr,           "--main",     (char *)main, NULL };
    return run(arguments, true);
}

static void setup()
{
    strcpy(temporary, "/tmp/studio-pages-tools-XXXXXX");
    assert(mkdtemp(temporary));
    previous = join(temporary, "previous");
    built = join(temporary, "built");
    output = join(temporary, "output");
    pr = join(temporary, "pr.json");
    log_path = join(temporary, "command.log");
    site(previous, old_studio);
    site(built, studio);
    char *previews = join(previous, "previews");
    char *a = join(previews, "77"), *b = join(previews, "78");
    assert(mkdir(previews, 0700) == 0 && mkdir(a, 0700) == 0 && mkdir(b, 0700) == 0);
    write_at(a, "old.txt", "old preview77\n");
    write_at(b, "keep.txt", "other preview78\n");
    free(previews);
    free(a);
    free(b);
    pr_state(false, true);
}

static void teardown()
{
    char *arguments[] = { "rm", "-rf", "--", temporary, NULL };
    assert(run(arguments, false) == 0);
    free(previous);
    free(built);
    free(output);
    free(pr);
    free(log_path);
}

TEST_CASE(preview_preserves_production_and_other_previews)
{
    REQUIRE(compose("preview77", old_studio) == 0);
    REQUIRE(has(output, "studio.json", old_studio));
    REQUIRE(has(output, "chibi-studio.js", "fixture\n"));
    REQUIRE(has(output, "previews/78/keep.txt", "other preview78\n"));
    REQUIRE(has(output, "previews/77/studio.json", studio));
    REQUIRE(!exists(output, "previews/77/old.txt"));
    REQUIRE(has(previous, "previews/77/old.txt", "old preview77\n"));
    return true;
}

TEST_CASE(production_preserves_every_preview)
{
    REQUIRE(compose("production", studio) == 0);
    REQUIRE(has(output, "studio.json", studio));
    REQUIRE(has(output, "previews/77/old.txt", "old preview77\n"));
    REQUIRE(has(output, "previews/78/keep.txt", "other preview78\n"));
    REQUIRE(has(previous, "studio.json", old_studio));
    return true;
}

TEST_CASE(merge_cleanup_changes_only_77)
{
    pr_state(true, false);
    REQUIRE(compose("cleanup77", old_studio) == 0);
    REQUIRE(!exists(output, "previews/77"));
    REQUIRE(has(output, "studio.json", old_studio));
    REQUIRE(has(output, "previews/78/keep.txt", "other preview78\n"));
    REQUIRE(has(previous, "previews/77/old.txt", "old preview77\n"));
    return true;
}

TEST_CASE(unmerged_close_cannot_cleanup)
{
    pr_state(false, false);
    REQUIRE(compose("cleanup77", old_studio) != 0);
    REQUIRE(access(output, F_OK) != 0);
    REQUIRE(has(previous, "previews/77/old.txt", "old preview77\n"));
    return true;
}

TEST_CASE(late_preview_cannot_resurrect_after_merge)
{
    pr_state(true, false);
    REQUIRE(compose("cleanup77", old_studio) == 0);
    char *first = previous;
    previous = output;
    output = join(temporary, "late");
    REQUIRE(compose("preview77", old_studio) != 0);
    REQUIRE(!exists(previous, "previews/77"));
    REQUIRE(access(output, F_OK) != 0);
    free(first);
    return true;
}

TEST_CASE(stale_production_cannot_overwrite_newer_root)
{
    REQUIRE(compose("production", old_studio) != 0);
    REQUIRE(access(output, F_OK) != 0);
    REQUIRE(has(previous, "studio.json", old_studio));
    return true;
}

TEST_CASE(corrupt_preview_is_rejected)
{
    write_at(built, "chibi-studio.wasm", "changed\n");
    REQUIRE(compose("preview77", old_studio) != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(preview_requires_the_reviewed_source_tree)
{
    char *path = join(built, "studio.json");
    FILE *file = fopen(path, "rb");
    REQUIRE(file);
    char text[4096];
    size_t length = fread(text, 1, sizeof text - 1, file);
    text[length] = '\0';
    REQUIRE(fclose(file) == 0);
    free(path);
    char *at = strstr(text, tree);
    REQUIRE(at);
    memset(at, '0', 40);
    write_at(built, "studio.json", text);
    REQUIRE(compose("preview77", old_studio) != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(arbitrary_mode_is_rejected)
{
    REQUIRE(compose("../../outside", old_studio) != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(links_cannot_escape_the_site)
{
    char *link = join(previous, "escape");
    REQUIRE(symlink("/tmp", link) == 0);
    free(link);
    REQUIRE(compose("preview77", old_studio) != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

/* Fixture generation is Python; all regression assertions remain in pico_unit. */
static int unpack_fixture(const char *kind)
{
    static char code
        [] = "import sys,io,tarfile,zipfile,hashlib; "
             "sys.path.insert(0,sys.argv[1]); import pages_site; "
             "kind=sys.argv[2]; raw=io.BytesIO(); "
             "tar=tarfile.open(fileobj=raw,mode='w'); "
             "entry=tarfile.TarInfo('../escaped.txt' if kind=='traversal' else "
             "'index.html'); "
             "entry.size=8; "
             "entry.type=tarfile.SYMTYPE if kind=='link' else tarfile.REGTYPE; "
             "entry.linkname='../previous'; "
             "tar.addfile(entry,io.BytesIO(b'fixture\\n')); "
             "tar.addfile(entry,io.BytesIO(b'fixture\\n')) if "
             "kind=='duplicate' else None; "
             "tar.close(); zipped=io.BytesIO(); z=zipfile.ZipFile(zipped,'w'); "
             "z.writestr('artifact.tar',raw.getvalue()); z.close(); "
             "data=zipped.getvalue(); "
             "digest='0'*64 if kind=='checksum' else "
             "hashlib.sha256(data).hexdigest(); "
             "pages_site.unpack(data,digest,pages_site.Path(sys.argv[3]))";
    char *arguments[] = { "python3",    "-c",   code, tools,
                          (char *)kind, output, NULL };
    return run(arguments, true);
}

TEST_CASE(valid_pages_archive_restores)
{
    REQUIRE(unpack_fixture("valid") == 0);
    REQUIRE(has(output, "index.html", "fixture\n"));
    return true;
}

TEST_CASE(archive_traversal_is_rejected)
{
    REQUIRE(unpack_fixture("traversal") != 0);
    REQUIRE(!exists(temporary, "escaped.txt"));
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(archive_links_are_rejected)
{
    REQUIRE(unpack_fixture("link") != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(archive_duplicates_are_rejected)
{
    REQUIRE(unpack_fixture("duplicate") != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(archive_checksum_is_verified)
{
    REQUIRE(unpack_fixture("checksum") != 0);
    REQUIRE(access(output, F_OK) != 0);
    return true;
}

TEST_CASE(fresh_composition_keeps_interleaved_changes)
{
    REQUIRE(compose("preview77", old_studio) == 0);
    char *initial = previous;
    previous = output;
    output = join(temporary, "production");
    REQUIRE(compose("production", studio) == 0);
    free(initial);
    initial = previous;
    previous = output;
    output = join(temporary, "cleaned");
    pr_state(true, false);
    REQUIRE(compose("cleanup77", studio) == 0);
    REQUIRE(has(output, "studio.json", studio));
    REQUIRE(!exists(output, "previews/77"));
    REQUIRE(has(output, "previews/78/keep.txt", "other preview78\n"));
    free(initial);
    return true;
}

#define RUN_PAGES_CASE(test)                                                   \
    do {                                                                       \
        setup();                                                               \
        RUN_TEST_CASE(test);                                                   \
        teardown();                                                            \
    } while (0)

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--pages-site")) {
        fprintf(stderr, "Usage: studio-tools --pages-site SOURCE\n");
        return 2;
    }
    assert(setenv("PYTHONDONTWRITEBYTECODE", "1", 1) == 0);
    tools = join(argv[2], "tools");
    script = join(tools, "pages_site.py");
    RUN_PAGES_CASE(preview_preserves_production_and_other_previews);
    RUN_PAGES_CASE(production_preserves_every_preview);
    RUN_PAGES_CASE(merge_cleanup_changes_only_77);
    RUN_PAGES_CASE(unmerged_close_cannot_cleanup);
    RUN_PAGES_CASE(late_preview_cannot_resurrect_after_merge);
    RUN_PAGES_CASE(stale_production_cannot_overwrite_newer_root);
    RUN_PAGES_CASE(corrupt_preview_is_rejected);
    RUN_PAGES_CASE(preview_requires_the_reviewed_source_tree);
    RUN_PAGES_CASE(arbitrary_mode_is_rejected);
    RUN_PAGES_CASE(links_cannot_escape_the_site);
    RUN_PAGES_CASE(valid_pages_archive_restores);
    RUN_PAGES_CASE(archive_traversal_is_rejected);
    RUN_PAGES_CASE(archive_links_are_rejected);
    RUN_PAGES_CASE(archive_duplicates_are_rejected);
    RUN_PAGES_CASE(archive_checksum_is_verified);
    RUN_PAGES_CASE(fresh_composition_keeps_interleaved_changes);
    pu_print_stats();
    free(script);
    free(tools);
    return pu_test_failed() ? 1 : 0;
}
