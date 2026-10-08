#include "test/physics_executor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <dlfcn.h>
#endif

test_physics_executor test_physics_executor_open()
{
    test_physics_executor executor = { 0 };
#ifndef __EMSCRIPTEN__
    const char *path = getenv("CBLUEPRINT_BENCH_EXECUTOR");
    if (!path || !*path) {
        return executor;
    }
    const char *workers = getenv("CBLUEPRINT_BENCH_WORKERS");
    assert(workers);
    executor.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!executor.library) {
        fprintf(stderr, "Physics executor: %s\n", dlerror());
    }
    assert(executor.library);
    void (*create)(game_task_system *, int);
    void *symbol = dlsym(executor.library, "physics_executor_create");
    static_assert(sizeof symbol == sizeof create);
    memcpy(&create, &symbol, sizeof symbol);
    symbol = dlsym(executor.library, "physics_executor_free");
    memcpy(&executor.dispose, &symbol, sizeof symbol);
    assert(create && executor.dispose);
    create(&executor.tasks, atoi(workers));
#endif
    return executor;
}

void test_physics_executor_close(test_physics_executor *executor)
{
#ifndef __EMSCRIPTEN__
    if (executor->library) {
        executor->dispose(executor->tasks.user);
        dlclose(executor->library);
    }
#endif
    *executor = (test_physics_executor){ 0 };
}
