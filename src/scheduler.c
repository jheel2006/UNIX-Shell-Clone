#include <stdlib.h>
#include <string.h>

#include "scheduler.h"

/*
 * DEFAULT_* values
 * ----------------
 * These defaults match the suggested Phase 4 policy:
 * short first response time, then a longer quantum for later rounds.
 */
#define DEFAULT_FIRST_ROUND_QUANTUM 3
#define DEFAULT_LATER_ROUND_QUANTUM 7
#define DEFAULT_PROGRAM_BURST 5

/*
 * duplicate_text
 * --------------
 * Small helper used when creating scheduler task records.
 */
static char *duplicate_text(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }

    length = strlen(text);
    copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1);
    return copy;
}

/*
 * log_scheduler_event
 * -------------------
 * Keeps the scheduler core independent from the server logging format.
 * The server may register a callback to print detailed task transitions.
 */
static void log_scheduler_event(Scheduler *scheduler,
                                const char *event,
                                const Task *task,
                                int quantum) {
    if (scheduler->logger != NULL) {
        scheduler->logger(event,
                          task,
                          quantum,
                          scheduler->callback_context);
    }
}

/*
 * enqueue_task_locked
 * ------------------
 * Appends one task to the waiting queue. The caller must already hold
 * scheduler->mutex.
 */
static void enqueue_task_locked(Scheduler *scheduler, Task *task) {
    task->next = NULL;

    if (scheduler->waiting_tail == NULL) {
        scheduler->waiting_head = task;
        scheduler->waiting_tail = task;
        return;
    }

    scheduler->waiting_tail->next = task;
    scheduler->waiting_tail = task;
}

/*
 * choose_shortest_program_locked
 * ------------------------------
 * Selects the shortest remaining program task.
 * This is the SJRF part of the Phase 4 scheduler. We keep both the best
 * task and a backup task, because the assignment says the same process
 * should not be chosen twice in a row unless it is the only one waiting.
 *
 * The caller must already hold scheduler->mutex.
 */
static Task *choose_shortest_program_locked(Scheduler *scheduler) {
    Task *cursor = scheduler->waiting_head;
    Task *best = NULL;
    Task *best_prev = NULL;
    Task *best_alt = NULL;
    Task *best_alt_prev = NULL;
    Task *previous = NULL;

    while (cursor != NULL) {
        if (cursor->type == TASK_TYPE_PROGRAM) {
            if (best == NULL ||
                cursor->remaining_burst < best->remaining_burst ||
                (cursor->remaining_burst == best->remaining_burst &&
                 cursor->arrival_order < best->arrival_order)) {
                best_alt = best;
                best_alt_prev = best_prev;
                best = cursor;
                best_prev = previous;
            } else if (best_alt == NULL ||
                       cursor->remaining_burst < best_alt->remaining_burst ||
                       (cursor->remaining_burst == best_alt->remaining_burst &&
                        cursor->arrival_order < best_alt->arrival_order)) {
                best_alt = cursor;
                best_alt_prev = previous;
            }
        }

        previous = cursor;
        cursor = cursor->next;
    }

    /*
     * Fairness rule: avoid selecting the exact same program twice in a
     * row when another one is available, even if the first task is still
     * technically the shortest.
     */
    if (best != NULL &&
        best->task_id == scheduler->last_selected_task_id &&
        best_alt != NULL) {
        best = best_alt;
        best_prev = best_alt_prev;
    }

    if (best == NULL) {
        return NULL;
    }

    if (best_prev == NULL) {
        scheduler->waiting_head = best->next;
    } else {
        best_prev->next = best->next;
    }

    if (scheduler->waiting_tail == best) {
        scheduler->waiting_tail = best_prev;
    }

    best->next = NULL;
    return best;
}

/*
 * dequeue_next_task_locked
 * ------------------------
 * Implements the combined policy:
 *   1. shell commands always win immediately
 *   2. otherwise choose the shortest remaining program
 *   3. avoid selecting the same program twice in a row when possible
 * This keeps simple shell commands responsive while still letting demo
 * processes share the simualted CPU.
 *
 * The caller must already hold scheduler->mutex.
 */
static Task *dequeue_next_task_locked(Scheduler *scheduler) {
    Task *cursor = scheduler->waiting_head;
    Task *previous = NULL;

    while (cursor != NULL) {
        if (cursor->type == TASK_TYPE_SHELL) {
            if (previous == NULL) {
                scheduler->waiting_head = cursor->next;
            } else {
                previous->next = cursor->next;
            }

            if (scheduler->waiting_tail == cursor) {
                scheduler->waiting_tail = previous;
            }

            cursor->next = NULL;
            return cursor;
        }

        previous = cursor;
        cursor = cursor->next;
    }

    return choose_shortest_program_locked(scheduler);
}

/*
 * should_preempt_current_locked
 * -----------------------------
 * New shell tasks always preempt a running program. New programs may
 * preempt only when their remaining burst time is shorter.
 * The executor checks this flag between one-second ticks, so preemption
 * happens at clean points instead of in the middle of captured output.
 *
 * The caller must already hold scheduler->mutex.
 */
static int should_preempt_current_locked(const Scheduler *scheduler,
                                         const Task *incoming) {
    const Task *current = scheduler->current_task;

    if (current == NULL || current->type != TASK_TYPE_PROGRAM) {
        return 0;
    }

    if (incoming->type == TASK_TYPE_SHELL) {
        return 1;
    }

    if (incoming->type == TASK_TYPE_PROGRAM &&
        incoming->remaining_burst < current->remaining_burst) {
        return 1;
    }

    return 0;
}

/*
 * finalize_task
 * -------------
 * Applies the scheduler-side accounting for a completed/cancelled task.
 */
static void finalize_task(Scheduler *scheduler,
                          Task *task,
                          TaskState state,
                          const char *event) {
    task->state = state;
    scheduler->active_task_count--;
    log_scheduler_event(scheduler, event, task, 0);

    if (scheduler->cleanup != NULL) {
        scheduler->cleanup(task, scheduler->callback_context);
    } else {
        scheduler_destroy_task(task);
    }
}

/*
 * scheduler_main
 * --------------
 * Dedicated scheduler thread that simulates the one CPU available to
 * the server. Only this thread dispatches actual task execution.
 */
static void *scheduler_main(void *arg) {
    Scheduler *scheduler = arg;

    while (1) {
        Task *task;
        TaskRunResult result;
        int quantum;

        pthread_mutex_lock(&scheduler->mutex);

        while (scheduler->queued_signal_count == 0 &&
               !(scheduler->stop_requested &&
                 scheduler->active_task_count == 0)) {
            pthread_cond_wait(&scheduler->queue_ready, &scheduler->mutex);
        }

        if (scheduler->queued_signal_count > 0) {
            scheduler->queued_signal_count--;
        }

        if (scheduler->stop_requested && scheduler->active_task_count == 0) {
            pthread_mutex_unlock(&scheduler->mutex);
            break;
        }

        task = dequeue_next_task_locked(scheduler);
        if (task == NULL) {
            pthread_mutex_unlock(&scheduler->mutex);
            continue;
        }

        scheduler->current_task = task;
        scheduler->last_selected_task_id = task->task_id;
        task->state = TASK_STATE_RUNNING;
        task->preempt_requested = 0;

        quantum = task->rounds_completed == 0
            ? scheduler->config.first_round_quantum
            : scheduler->config.later_round_quantum;

        log_scheduler_event(scheduler, "dispatch", task, quantum);
        pthread_mutex_unlock(&scheduler->mutex);

        result = scheduler->executor(task,
                                     quantum,
                                     scheduler->callback_context);

        pthread_mutex_lock(&scheduler->mutex);
        scheduler->current_task = NULL;

        if (result == TASK_RUN_REQUEUE &&
            !task->cancel_requested &&
            task->remaining_burst > 0) {
            task->rounds_completed++;
            task->state = TASK_STATE_WAITING;
            task->preempt_requested = 0;
            log_scheduler_event(scheduler, "waiting", task, quantum);
            enqueue_task_locked(scheduler, task);
            scheduler->queued_signal_count++;
            pthread_cond_signal(&scheduler->queue_ready);
            pthread_mutex_unlock(&scheduler->mutex);
            continue;
        }

        if (result == TASK_RUN_CANCELLED || task->cancel_requested) {
            finalize_task(scheduler, task, TASK_STATE_CANCELLED, "cancel");
        } else if (task->remaining_burst <= 0 || result == TASK_RUN_COMPLETE) {
            finalize_task(scheduler, task, TASK_STATE_FINISHED, "finish");
        } else {
            finalize_task(scheduler, task, TASK_STATE_CANCELLED, "fail");
        }

        pthread_mutex_unlock(&scheduler->mutex);
    }

    return NULL;
}

/*
 * scheduler_init
 * --------------
 * Prepares the scheduler data structures and stores the execution hooks
 * that the server will provide later.
 */
int scheduler_init(Scheduler *scheduler,
                   const SchedulerConfig *config,
                   TaskExecutor executor,
                   TaskCleanup cleanup,
                   SchedulerLogger logger,
                   void *callback_context) {
    if (scheduler == NULL || executor == NULL) {
        return -1;
    }

    memset(scheduler, 0, sizeof(*scheduler));

    if (pthread_mutex_init(&scheduler->mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&scheduler->queue_ready, NULL) != 0) {
        pthread_mutex_destroy(&scheduler->mutex);
        return -1;
    }

    scheduler->config.first_round_quantum =
        config != NULL && config->first_round_quantum > 0
            ? config->first_round_quantum
            : DEFAULT_FIRST_ROUND_QUANTUM;
    scheduler->config.later_round_quantum =
        config != NULL && config->later_round_quantum > 0
            ? config->later_round_quantum
            : DEFAULT_LATER_ROUND_QUANTUM;
    scheduler->config.default_program_burst =
        config != NULL && config->default_program_burst > 0
            ? config->default_program_burst
            : DEFAULT_PROGRAM_BURST;

    scheduler->executor = executor;
    scheduler->cleanup = cleanup;
    scheduler->logger = logger;
    scheduler->callback_context = callback_context;
    scheduler->next_task_id = 1;
    scheduler->last_selected_task_id = -1;
    scheduler->next_arrival_order = 1UL;

    return 0;
}

/*
 * scheduler_start
 * ---------------
 * Launches the dedicated scheduler thread.
 */
int scheduler_start(Scheduler *scheduler) {
    if (scheduler == NULL) {
        return -1;
    }

    scheduler->running = 1;
    return pthread_create(&scheduler->thread_id,
                          NULL,
                          scheduler_main,
                          scheduler);
}

/*
 * scheduler_request_stop
 * ----------------------
 * Signals the scheduler thread to stop once there is no active work left.
 */
void scheduler_request_stop(Scheduler *scheduler) {
    if (scheduler == NULL) {
        return;
    }

    pthread_mutex_lock(&scheduler->mutex);
    scheduler->stop_requested = 1;
    pthread_cond_broadcast(&scheduler->queue_ready);
    pthread_mutex_unlock(&scheduler->mutex);
}

/*
 * scheduler_join
 * --------------
 * Waits for the scheduler thread to finish after stop was requested.
 */
int scheduler_join(Scheduler *scheduler) {
    if (scheduler == NULL || !scheduler->running) {
        return -1;
    }

    scheduler->running = 0;
    return pthread_join(scheduler->thread_id, NULL);
}

/*
 * scheduler_destroy
 * -----------------
 * Releases synchronization primitives and any remaining queued tasks.
 */
void scheduler_destroy(Scheduler *scheduler) {
    Task *cursor;

    if (scheduler == NULL) {
        return;
    }

    cursor = scheduler->waiting_head;
    while (cursor != NULL) {
        Task *next = cursor->next;
        scheduler_destroy_task(cursor);
        cursor = next;
    }

    pthread_cond_destroy(&scheduler->queue_ready);
    pthread_mutex_destroy(&scheduler->mutex);
}

/*
 * scheduler_create_task
 * ---------------------
 * Allocates one task record. The scheduler assigns ids and arrival order
 * when the task is actually submitted.
 */
Task *scheduler_create_task(TaskType type,
                            int client_number,
                            const char *command_text,
                            int predicted_burst,
                            void *user_data) {
    Task *task = calloc(1, sizeof(*task));

    if (task == NULL) {
        return NULL;
    }

    task->type = type;
    task->client_number = client_number;
    task->state = TASK_STATE_WAITING;
    task->predicted_burst = predicted_burst;
    task->remaining_burst = predicted_burst;
    task->command_text = duplicate_text(command_text);
    task->user_data = user_data;

    if (command_text != NULL && task->command_text == NULL) {
        free(task);
        return NULL;
    }

    return task;
}

/*
 * scheduler_destroy_task
 * ----------------------
 * Releases the heap storage owned by one task record.
 */
void scheduler_destroy_task(Task *task) {
    if (task == NULL) {
        return;
    }

    free(task->command_text);
    free(task);
}

/*
 * scheduler_submit_task
 * ---------------------
 * Adds one task to the waiting queue and requests preemption when the
 * new arrival should interrupt the currently running program.
 */
int scheduler_submit_task(Scheduler *scheduler, Task *task) {
    if (scheduler == NULL || task == NULL) {
        return -1;
    }

    pthread_mutex_lock(&scheduler->mutex);

    task->task_id = scheduler->next_task_id++;
    task->arrival_order = scheduler->next_arrival_order++;
    task->state = TASK_STATE_WAITING;
    task->next = NULL;

    if (task->type == TASK_TYPE_SHELL) {
        task->predicted_burst = -1;
        task->remaining_burst = -1;
    } else if (task->predicted_burst <= 0) {
        task->predicted_burst = scheduler->config.default_program_burst;
        task->remaining_burst = task->predicted_burst;
    }

    enqueue_task_locked(scheduler, task);
    scheduler->active_task_count++;
    scheduler->queued_signal_count++;
    log_scheduler_event(scheduler, "enqueue", task, 0);

    if (should_preempt_current_locked(scheduler, task)) {
        scheduler->current_task->preempt_requested = 1;
        log_scheduler_event(scheduler, "preempt-request", scheduler->current_task, 0);
    }

    pthread_cond_signal(&scheduler->queue_ready);
    pthread_mutex_unlock(&scheduler->mutex);
    return task->task_id;
}

/*
 * scheduler_cancel_client_tasks
 * -----------------------------
 * Removes all waiting tasks owned by one client and marks the currently
 * running task for cancellation if it belongs to the same client.
 */
int scheduler_cancel_client_tasks(Scheduler *scheduler, int client_number) {
    Task *cursor;
    Task *previous;
    Task *to_free = NULL;
    int cancelled = 0;

    if (scheduler == NULL) {
        return -1;
    }

    pthread_mutex_lock(&scheduler->mutex);

    previous = NULL;
    cursor = scheduler->waiting_head;
    while (cursor != NULL) {
        if (cursor->client_number == client_number) {
            Task *removed = cursor;

            if (previous == NULL) {
                scheduler->waiting_head = cursor->next;
            } else {
                previous->next = cursor->next;
            }

            if (scheduler->waiting_tail == removed) {
                scheduler->waiting_tail = previous;
            }

            cursor = cursor->next;
            removed->next = to_free;
            to_free = removed;
            scheduler->active_task_count--;
            cancelled++;
            continue;
        }

        previous = cursor;
        cursor = cursor->next;
    }

    if (scheduler->current_task != NULL &&
        scheduler->current_task->client_number == client_number) {
        scheduler->current_task->cancel_requested = 1;
        scheduler->current_task->preempt_requested = 1;
        cancelled++;
    }

    pthread_mutex_unlock(&scheduler->mutex);

    while (to_free != NULL) {
        Task *next = to_free->next;

        if (scheduler->cleanup != NULL) {
            to_free->state = TASK_STATE_CANCELLED;
            scheduler->cleanup(to_free, scheduler->callback_context);
        } else {
            scheduler_destroy_task(to_free);
        }

        to_free = next;
    }

    return cancelled;
}

/*
 * scheduler_task_should_preempt
 * -----------------------------
 * Polled by the execution layer between work units.
 */
int scheduler_task_should_preempt(Scheduler *scheduler, const Task *task) {
    int should_preempt;

    if (scheduler == NULL || task == NULL) {
        return 0;
    }

    pthread_mutex_lock(&scheduler->mutex);
    should_preempt = scheduler->current_task == task &&
                     task->preempt_requested;
    pthread_mutex_unlock(&scheduler->mutex);

    return should_preempt;
}

/*
 * scheduler_task_should_cancel
 * ----------------------------
 * Polled by the execution layer when a client disconnects while its task
 * is currently running.
 */
int scheduler_task_should_cancel(Scheduler *scheduler, const Task *task) {
    int should_cancel;

    if (scheduler == NULL || task == NULL) {
        return 0;
    }

    pthread_mutex_lock(&scheduler->mutex);
    should_cancel = scheduler->current_task == task &&
                    task->cancel_requested;
    pthread_mutex_unlock(&scheduler->mutex);

    return should_cancel;
}

/*
 * scheduler_waiting_count
 * -----------------------
 * Convenience helper for diagnostics and tests.
 */
size_t scheduler_waiting_count(Scheduler *scheduler) {
    Task *cursor;
    size_t count = 0;

    if (scheduler == NULL) {
        return 0;
    }

    pthread_mutex_lock(&scheduler->mutex);
    cursor = scheduler->waiting_head;
    while (cursor != NULL) {
        count++;
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&scheduler->mutex);

    return count;
}

/*
 * scheduler_has_active_work
 * -------------------------
 * Reports whether the scheduler still owns any queued or running task.
 */
int scheduler_has_active_work(Scheduler *scheduler) {
    int active;

    if (scheduler == NULL) {
        return 0;
    }

    pthread_mutex_lock(&scheduler->mutex);
    active = scheduler->active_task_count > 0;
    pthread_mutex_unlock(&scheduler->mutex);

    return active;
}
