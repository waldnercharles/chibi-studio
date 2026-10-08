#ifndef TEST_PHYSICS_EXECUTOR_H
#define TEST_PHYSICS_EXECUTOR_H

#include "runtime/tasks.h"

typedef struct
{
    game_task_system tasks;
    void *library;
    void (*dispose)(void *);
} test_physics_executor;

/* Optional native experiment selected by CBLUEPRINT_BENCH_EXECUTOR and
 * CBLUEPRINT_BENCH_WORKERS. Dispose only after all borrowing worlds are freed. */
test_physics_executor test_physics_executor_open();
void test_physics_executor_close(test_physics_executor *executor);

#endif
