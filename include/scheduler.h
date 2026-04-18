#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <pthread.h>
#include <stddef.h>

/*
 * TaskType
 * --------
 * Distinguishes between shell commands, which should finish in one round,
 * and program tasks, which may be preempted and re-queued.
 */
typedef enum {
    TASK_TYPE_SHELL = 0,
    TASK_TYPE_PROGRAM = 1
} TaskType;

/*
 * TaskState
 * ---------
 * Tracks where a task currently lives in the scheduler lifecycle.
 */
typedef enum {
    TASK_STATE_WAITING = 0,
    TASK_STATE_RUNNING = 1,
    TASK_STATE_FINISHED = 2,
    TASK_STATE_CANCELLED = 3
} TaskState;

/*
 * TaskRunResult
 * -------------
 * Result reported by the execution layer after one scheduled round.
 */
typedef enum {
    TASK_RUN_COMPLETE = 0,
    TASK_RUN_REQUEUE = 1,
    TASK_RUN_CANCELLED = 2,
    TASK_RUN_FAILED = 3
} TaskRunResult;

/*
 * Task
 * ----
 * Scheduler-owned task record. The execution layer may update
 * remaining_burst during a round so the scheduler can re-queue the task
 * from the point it stopped rather than restarting it.
 */
typedef struct Task {
    int task_id;
    int client_number;
    TaskType type;
    TaskState state;
    int predicted_burst;
    int remaining_burst;
    unsigned int rounds_completed;
    unsigned long arrival_order;
    int preempt_requested;
    int cancel_requested;
    char *command_text;
    void *user_data;
    struct Task *next;
} Task;

/*
 * SchedulerConfig
 * ---------------
 * Tunable policy values for the combined RR + SJRF scheduler.
 */
typedef struct {
    int first_round_quantum;
    int later_round_quantum;
    int default_program_burst;
} SchedulerConfig;

typedef TaskRunResult (*TaskExecutor)(Task *task, int quantum, void *context);
typedef void (*TaskCleanup)(Task *task, void *context);
typedef void (*SchedulerLogger)(const char *event,
                                const Task *task,
                                int quantum,
                                void *context);

/*
 * Scheduler
 * ---------
 * Shared server-side scheduler state. One scheduler thread removes tasks
 * from the waiting queue and executes them on the simulated single CPU.
 */
typedef struct {
    pthread_t thread_id;
    pthread_mutex_t mutex;
    pthread_cond_t queue_ready;
    Task *waiting_head;
    Task *waiting_tail;
    Task *current_task;
    int running;
    int stop_requested;
    int active_task_count;
    unsigned int queued_signal_count;
    int next_task_id;
    int last_selected_task_id;
    unsigned long next_arrival_order;
    SchedulerConfig config;
    TaskExecutor executor;
    TaskCleanup cleanup;
    SchedulerLogger logger;
    void *callback_context;
} Scheduler;

int scheduler_init(Scheduler *scheduler,
                   const SchedulerConfig *config,
                   TaskExecutor executor,
                   TaskCleanup cleanup,
                   SchedulerLogger logger,
                   void *callback_context);
int scheduler_start(Scheduler *scheduler);
void scheduler_request_stop(Scheduler *scheduler);
int scheduler_join(Scheduler *scheduler);
void scheduler_destroy(Scheduler *scheduler);

Task *scheduler_create_task(TaskType type,
                            int client_number,
                            const char *command_text,
                            int predicted_burst,
                            void *user_data);
void scheduler_destroy_task(Task *task);

int scheduler_submit_task(Scheduler *scheduler, Task *task);
int scheduler_cancel_client_tasks(Scheduler *scheduler, int client_number);

int scheduler_task_should_preempt(Scheduler *scheduler, const Task *task);
int scheduler_task_should_cancel(Scheduler *scheduler, const Task *task);
size_t scheduler_waiting_count(Scheduler *scheduler);
int scheduler_has_active_work(Scheduler *scheduler);

#endif
