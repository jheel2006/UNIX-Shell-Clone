#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "scheduler.h"

/*
 * TestContext
 * -----------
 * Keeps together the scheduler pointer so the fake executor can poll the
 * preemption and cancellation flags while it simulates one time unit.
 */
typedef struct {
    Scheduler *scheduler;
} TestContext;

/*
 * test_logger
 * -----------
 * Prints a compact trace of the scheduling decisions so Phase 4 queue
 * ordering and preemption behavior can be verified without the socket
 * layer being involved yet.
 */
static void test_logger(const char *event,
                        const Task *task,
                        int quantum,
                        void *context) {
    (void) context;

    if (task == NULL) {
        return;
    }

    printf("[scheduler] event=%s task=%d client=%d type=%s remaining=%d rounds=%u quantum=%d command=\"%s\"\n",
           event,
           task->task_id,
           task->client_number,
           task->type == TASK_TYPE_SHELL ? "shell" : "program",
           task->remaining_burst,
           task->rounds_completed,
           quantum,
           task->command_text != NULL ? task->command_text : "");
    fflush(stdout);
}

/*
 * fake_executor
 * -------------
 * Simulates a scheduled run one time unit at a time. Program tasks sleep
 * briefly to make the scheduling trace visible, while shell tasks finish
 * immediately in one round.
 */
static TaskRunResult fake_executor(Task *task, int quantum, void *context) {
    TestContext *test_context = context;
    int slice;

    if (task->type == TASK_TYPE_SHELL) {
        printf("[executor] shell task %d runs immediately: %s\n",
               task->task_id,
               task->command_text);
        fflush(stdout);
        return TASK_RUN_COMPLETE;
    }

    for (slice = 0; slice < quantum && task->remaining_burst > 0; slice++) {
        if (scheduler_task_should_cancel(test_context->scheduler, task)) {
            return TASK_RUN_CANCELLED;
        }

        printf("[executor] task %d tick %d remaining(before)=%d\n",
               task->task_id,
               slice + 1,
               task->remaining_burst);
        fflush(stdout);

        usleep(200000);
        task->remaining_burst--;

        if (task->remaining_burst <= 0) {
            return TASK_RUN_COMPLETE;
        }

        if (scheduler_task_should_preempt(test_context->scheduler, task)) {
            return TASK_RUN_REQUEUE;
        }
    }

    return task->remaining_burst > 0 ? TASK_RUN_REQUEUE : TASK_RUN_COMPLETE;
}

/*
 * main
 * ----
 * Simple scheduler-core verification harness:
 *   1. submit one longer program
 *   2. submit a shorter program so preemption is requested
 *   3. submit a shell command which should run immediately next
 */
int main(void) {
    Scheduler scheduler;
    SchedulerConfig config = {3, 7, 5};
    TestContext context;
    Task *task_a;
    Task *task_b;
    Task *task_c;

    context.scheduler = &scheduler;

    if (scheduler_init(&scheduler,
                       &config,
                       fake_executor,
                       NULL,
                       test_logger,
                       &context) != 0) {
        fprintf(stderr, "scheduler_init failed\n");
        return EXIT_FAILURE;
    }

    if (scheduler_start(&scheduler) != 0) {
        fprintf(stderr, "scheduler_start failed\n");
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    task_a = scheduler_create_task(TASK_TYPE_PROGRAM, 1, "demo 8", 8, NULL);
    task_b = scheduler_create_task(TASK_TYPE_PROGRAM, 2, "demo 2", 2, NULL);
    task_c = scheduler_create_task(TASK_TYPE_SHELL, 3, "pwd", -1, NULL);

    if (task_a == NULL || task_b == NULL || task_c == NULL) {
        fprintf(stderr, "scheduler_create_task failed\n");
        scheduler_request_stop(&scheduler);
        scheduler_join(&scheduler);
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    scheduler_submit_task(&scheduler, task_a);
    usleep(250000);
    scheduler_submit_task(&scheduler, task_b);
    usleep(150000);
    scheduler_submit_task(&scheduler, task_c);

    while (scheduler_has_active_work(&scheduler)) {
        usleep(100000);
    }

    scheduler_request_stop(&scheduler);
    scheduler_join(&scheduler);
    scheduler_destroy(&scheduler);
    return EXIT_SUCCESS;
}
