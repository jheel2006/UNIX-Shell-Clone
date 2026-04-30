#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "scheduler.h"
#include "shell_core.h"

#define PORT 9002
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define SERVER_CONTINUE 0U
#define SERVER_EXIT 1U
#define DEMO_DEFAULT_BURST 5

typedef struct {
    int socket_fd;
    int client_number;
    int thread_number;
    struct sockaddr_in address;
} ClientContext;

typedef struct TaskContext {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int refcount;
    int done;
    int response_status;
    char *output;
    size_t output_size;
    char **argv;
    pid_t child_pid;
    int child_running;
    int pipe_fd;
} TaskContext;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static Scheduler scheduler;
static int scheduler_started = 0;
static int next_client_number = 1;
static int next_thread_number = 1;

/* Timeline tracking for summary */
typedef struct {
    int client_id;
    int duration;  /* cumulative end timestamp */
} TimelineSlice;

#define MAX_TIMELINE_SLICES 100
static TimelineSlice timeline[MAX_TIMELINE_SLICES];
static int timeline_count = 0;
static int had_preemption = 0;
static int current_running_client = -1;
static int current_slice_start_time = 0;

/* Forward declaration for summary printing */
static void print_summary_if_needed(void);

static int send_all(int socket_fd, const void *buffer, size_t length) {
    const char *cursor = buffer;
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent_now = send(socket_fd, cursor + total_sent, length - total_sent, 0);
        if (sent_now <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total_sent += (size_t) sent_now;
    }

    return 0;
}

static ssize_t recv_command(int socket_fd, char *buffer, size_t buffer_size) {
    size_t used = 0;

    if (buffer_size == 0) {
        return -1;
    }

    while (used + 1 < buffer_size) {
        ssize_t received = recv(socket_fd, buffer + used, 1, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return 0;
        }
        if (buffer[used] == '\0') {
            return (ssize_t) used;
        }
        used += 1;
    }

    buffer[buffer_size - 1] = '\0';
    return (ssize_t) used;
}

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

static void free_string_array(char **items) {
    int i;

    if (items == NULL) {
        return;
    }

    for (i = 0; items[i] != NULL; i++) {
        free(items[i]);
    }
    free(items);
}

static void log_line(const char *fmt, ...) {
    va_list args;

    pthread_mutex_lock(&log_mutex);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

static void log_connection_open(const ClientContext *client) {
    char ip_address[INET_ADDRSTRLEN];

    inet_ntop(AF_INET,
              &client->address.sin_addr,
              ip_address,
              sizeof(ip_address));

    log_line("[%d]<<< client connected\n",
             client->client_number,
             ip_address,
             ntohs(client->address.sin_port),
             client->thread_number);
}

static void log_disconnect(const ClientContext *client, const char *reason) {
    log_line("[%d]<<< %s\n", client->client_number, reason);
}

static void log_received(const ClientContext *client, const char *command) {
    log_line("[%d]>>> %s\n", client->client_number, command);
}

static void log_sent(const ClientContext *client, size_t bytes_sent) {
    log_line("[%d]<<< %zu bytes sent\n", client->client_number, bytes_sent);
}

static void log_task_state(const Task *task, const char *state, int value) {
    log_line("(%d)--- %s (%d)\n", task->client_number, state, value);
}

static void append_timeline_slice(int client_id, int end_timestamp) {
    pthread_mutex_lock(&counter_mutex);
    if (timeline_count < MAX_TIMELINE_SLICES) {
        timeline[timeline_count].client_id = client_id;
        timeline[timeline_count].duration = end_timestamp;
        timeline_count++;
    }
    pthread_mutex_unlock(&counter_mutex);
}

static int is_demo_command(const char *command) {
    return command != NULL &&
           (strncmp(command, "./demo", 6) == 0 || strncmp(command, "demo", 4) == 0) &&
           (command[4] == '\0' || command[4] == ' ' || command[4] == '\t' ||
            command[5] == '\0' || command[5] == ' ' || command[5] == '\t' ||
            command[6] == '\0' || command[6] == ' ' || command[6] == '\t');
}

static char **build_demo_argv(const char *command, int *iterations_out) {
    char *copy;
    char *saveptr = NULL;
    char *token;
    char **argv;
    int argc = 0;
    int capacity = 4;
    int iterations = DEMO_DEFAULT_BURST;

    if (iterations_out == NULL) {
        return NULL;
    }

    *iterations_out = DEMO_DEFAULT_BURST;

    copy = duplicate_text(command);
    if (copy == NULL) {
        return NULL;
    }

    token = strtok_r(copy, " \t", &saveptr);
    if (token == NULL || !is_demo_command(token)) {
        free(copy);
        return NULL;
    }

    argv = calloc((size_t) capacity, sizeof(char *));
    if (argv == NULL) {
        free(copy);
        return NULL;
    }

    while (token != NULL) {
        char **resized;

        if (argc + 1 >= capacity) {
            capacity *= 2;
            resized = realloc(argv, (size_t) capacity * sizeof(char *));
            if (resized == NULL) {
                free_string_array(argv);
                free(copy);
                return NULL;
            }
            argv = resized;
            memset(&argv[argc], 0, (size_t) (capacity - argc) * sizeof(char *));
        }

        if (argc == 0 && strcmp(token, "demo") == 0) {
            argv[argc] = duplicate_text("./demo");
        } else {
            argv[argc] = duplicate_text(token);
        }
        if (argv[argc] == NULL) {
            free_string_array(argv);
            free(copy);
            return NULL;
        }

        argc++;
        token = strtok_r(NULL, " \t", &saveptr);
    }

    if (argc >= 2) {
        char *end = NULL;
        long parsed = strtol(argv[1], &end, 10);

        if (end != argv[1] && *end == '\0' && parsed > 0) {
            iterations = (int) parsed;
        } else {
            free(argv[1]);
            argv[1] = duplicate_text("5");
            if (argv[1] == NULL) {
                free_string_array(argv);
                free(copy);
                return NULL;
            }
        }
    } else {
        argv[argc] = duplicate_text("5");
        if (argv[argc] == NULL) {
            free_string_array(argv);
            free(copy);
            return NULL;
        }
        argc++;
    }

    argv[argc] = NULL;
    *iterations_out = iterations;
    free(copy);
    return argv;
}

static TaskContext *task_context_create(void) {
    TaskContext *context = calloc(1, sizeof(*context));

    if (context == NULL) {
        return NULL;
    }

    context->refcount = 2;
    context->response_status = SERVER_CONTINUE;
    context->pipe_fd = -1;

    if (pthread_mutex_init(&context->mutex, NULL) != 0) {
        free(context);
        return NULL;
    }

    if (pthread_cond_init(&context->ready, NULL) != 0) {
        pthread_mutex_destroy(&context->mutex);
        free(context);
        return NULL;
    }

    return context;
}

static void task_context_release(TaskContext *context) {
    int free_now = 0;

    if (context == NULL) {
        return;
    }

    pthread_mutex_lock(&context->mutex);
    context->refcount--;
    if (context->refcount == 0) {
        free_now = 1;
    }
    pthread_mutex_unlock(&context->mutex);

    if (!free_now) {
        return;
    }

    if (context->pipe_fd >= 0) {
        close(context->pipe_fd);
    }

    if (context->child_pid > 0) {
        kill(context->child_pid, SIGKILL);
        waitpid(context->child_pid, NULL, 0);
    }

    free_string_array(context->argv);
    free(context->output);
    pthread_cond_destroy(&context->ready);
    pthread_mutex_destroy(&context->mutex);
    free(context);
}

static void task_context_set_done(TaskContext *context,
                                  int status,
                                  char *output,
                                  size_t output_size) {
    pthread_mutex_lock(&context->mutex);
    context->response_status = status;
    context->output = output;
    context->output_size = output_size;
    context->done = 1;
    pthread_cond_broadcast(&context->ready);
    pthread_mutex_unlock(&context->mutex);
}

static int task_context_append(TaskContext *context, const char *data, size_t size) {
    size_t required = context->output_size + size + 1;
    char *resized;

    if (size == 0) {
        return 0;
    }

    if (context->output == NULL) {
        context->output = malloc(required < 256 ? 256 : required);
        if (context->output == NULL) {
            return -1;
        }
    } else if (required > context->output_size + 1) {
        resized = realloc(context->output, required < 256 ? 256 : required);
        if (resized == NULL) {
            return -1;
        }
        context->output = resized;
    }

    memcpy(context->output + context->output_size, data, size);
    context->output_size += size;
    context->output[context->output_size] = '\0';
    return 0;
}

static int task_context_drain_pipe(TaskContext *context) {
    char buffer[512];

    if (context->pipe_fd < 0) {
        return 0;
    }

    while (1) {
        ssize_t n = read(context->pipe_fd, buffer, sizeof(buffer));
        if (n > 0) {
            if (task_context_append(context, buffer, (size_t) n) != 0) {
                return -1;
            }
            continue;
        }

        if (n == 0) {
            close(context->pipe_fd);
            context->pipe_fd = -1;
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        }

        return -1;
    }

    return 0;
}

static void task_context_stop_child(TaskContext *context) {
    if (context->child_pid > 0 && context->child_running) {
        kill(context->child_pid, SIGSTOP);
        context->child_running = 0;
    }
}

static void task_context_terminate_child(TaskContext *context) {
    if (context->child_pid > 0) {
        kill(context->child_pid, SIGTERM);
        usleep(100000);
        kill(context->child_pid, SIGKILL);
        waitpid(context->child_pid, NULL, 0);
    }

    context->child_pid = -1;
    context->child_running = 0;

    if (context->pipe_fd >= 0) {
        close(context->pipe_fd);
        context->pipe_fd = -1;
    }
}

static int task_context_start_child(TaskContext *context) {
    int pipe_fd[2];
    pid_t pid;

    if (pipe(pipe_fd) < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }

    if (pid == 0) {
        if (dup2(pipe_fd[1], STDOUT_FILENO) < 0 || dup2(pipe_fd[1], STDERR_FILENO) < 0) {
            _exit(EXIT_FAILURE);
        }
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        execvp(context->argv[0], context->argv);
        perror("execvp");
        _exit(EXIT_FAILURE);
    }

    close(pipe_fd[1]);
    if (fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipe_fd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    context->child_pid = pid;
    context->child_running = 1;
    context->pipe_fd = pipe_fd[0];
    return 0;
}

static int task_context_resume_child(TaskContext *context) {
    if (context->child_pid > 0 && !context->child_running) {
        if (kill(context->child_pid, SIGCONT) < 0 && errno != ESRCH) {
            return -1;
        }
        context->child_running = 1;
    }
    return 0;
}

static int task_context_socket_closed(int socket_fd) {
    char peek;
    ssize_t n = recv(socket_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);

    if (n == 0) {
        return 1;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return 1;
    }

    return 0;
}

static int send_response(int socket_fd, uint32_t status, const char *payload, size_t payload_size) {
    uint32_t net_status = htonl(status);
    uint32_t net_size = htonl((uint32_t) payload_size);

    if (send_all(socket_fd, &net_status, sizeof(net_status)) != 0) {
        return -1;
    }

    if (send_all(socket_fd, &net_size, sizeof(net_size)) != 0) {
        return -1;
    }

    if (payload_size > 0 && send_all(socket_fd, payload, payload_size) != 0) {
        return -1;
    }

    return 0;
}

static void scheduler_logger(const char *event,
                             const Task *task,
                             int quantum,
                             void *context) {
    (void) context;
    (void) quantum;

    if (task == NULL) {
        return;
    }

    if (strcmp(event, "enqueue") == 0) {
        log_task_state(task, "created", task->remaining_burst);
    } else if (strcmp(event, "dispatch") == 0) {
        /* Only log "started" for first round, otherwise just dispatch happens */
        if (task->rounds_completed == 0) {
            log_task_state(task, "started", task->remaining_burst);
        }
    } else if (strcmp(event, "requeue") == 0 || strcmp(event, "preempt-request") == 0) {
        log_task_state(task, "waiting", task->remaining_burst);
    } else if (strcmp(event, "finish") == 0) {
            int ended_val = task->type == TASK_TYPE_SHELL ? -1 : task->remaining_burst;
            log_task_state(task, "ended", ended_val);
    } else if (strcmp(event, "cancel") == 0) {
            int ended_val = task->type == TASK_TYPE_SHELL ? -1 : task->remaining_burst;
            log_task_state(task, "ended", ended_val);
    }
}

static TaskRunResult execute_shell_task(Task *task, TaskContext *context) {
    char *line_copy = duplicate_text(task->command_text);
    char *captured_output = NULL;
    size_t captured_size = 0;
    int exit_requested;

    if (line_copy == NULL) {
        char *msg = duplicate_text("Failed to allocate shell command buffer.\n");
        if (msg != NULL) {
            task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
        }
        return TASK_RUN_FAILED;
    }

    exit_requested = shell_execute_line_capture(line_copy, &captured_output, &captured_size);
    free(line_copy);

    if (captured_output == NULL) {
        captured_output = duplicate_text("Failed to capture shell output.\n");
        if (captured_output == NULL) {
            return TASK_RUN_FAILED;
        }
        captured_size = strlen(captured_output);
    }

    task_context_set_done(context,
                          exit_requested ? SERVER_EXIT : SERVER_CONTINUE,
                          captured_output,
                          captured_size);
    return TASK_RUN_COMPLETE;
}

static TaskRunResult execute_program_task(Task *task, int quantum, TaskContext *context) {
    int slice;
    int slices_run = 0;
    int slice_end_timestamp = 0;

    if (context->argv == NULL) {
        char *msg = duplicate_text("Invalid program task.\n");
        if (msg != NULL) {
            task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
        }
        return TASK_RUN_FAILED;
    }

    if (context->child_pid <= 0) {
        if (task_context_start_child(context) != 0) {
            char *msg = duplicate_text("Failed to start demo program.\n");
            if (msg != NULL) {
                task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
            }
            return TASK_RUN_FAILED;
        }
    } else if (task_context_resume_child(context) != 0) {
        char *msg = duplicate_text("Failed to resume demo program.\n");
        if (msg != NULL) {
            task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
        }
        return TASK_RUN_FAILED;
    }

    for (slice = 0; slice < quantum && task->remaining_burst > 0; slice++) {
        int child_status;

        if (scheduler_task_should_cancel(&scheduler, task) || task->cancel_requested) {
            task_context_terminate_child(context);
            /* Update timeline with actual slices run */
            pthread_mutex_lock(&counter_mutex);
            current_slice_start_time += slices_run;
            slice_end_timestamp = current_slice_start_time;
            pthread_mutex_unlock(&counter_mutex);
            if (slices_run > 0) {
                append_timeline_slice(task->client_number, slice_end_timestamp);
            }
            return TASK_RUN_CANCELLED;
        }

        log_task_state(task, "running", task->remaining_burst);
        sleep(1);
        slices_run++;

        if (task_context_drain_pipe(context) != 0) {
            task_context_terminate_child(context);
            char *msg = duplicate_text("Failed while capturing program output.\n");
            if (msg != NULL) {
                task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
            }
            /* Update timeline */
            pthread_mutex_lock(&counter_mutex);
            current_slice_start_time += slices_run;
            slice_end_timestamp = current_slice_start_time;
            pthread_mutex_unlock(&counter_mutex);
            if (slices_run > 0) {
                append_timeline_slice(task->client_number, slice_end_timestamp);
            }
            return TASK_RUN_FAILED;
        }

        if (task->remaining_burst > 0) {
            task->remaining_burst--;
        }

        if (context->child_pid > 0 && waitpid(context->child_pid, &child_status, WNOHANG) == context->child_pid) {
            (void) child_status;
            context->child_pid = -1;
            context->child_running = 0;
            task->remaining_burst = 0;

            if (task_context_drain_pipe(context) != 0) {
                char *msg = duplicate_text("Failed while capturing program output.\n");
                if (msg != NULL) {
                    task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
                }
                /* Update timeline */
                pthread_mutex_lock(&counter_mutex);
                current_slice_start_time += slices_run;
                pthread_mutex_unlock(&counter_mutex);
                return TASK_RUN_FAILED;
            }

            /* Update timeline */
            pthread_mutex_lock(&counter_mutex);
            current_slice_start_time += slices_run;
            slice_end_timestamp = current_slice_start_time;
            pthread_mutex_unlock(&counter_mutex);
            if (slices_run > 0) {
                append_timeline_slice(task->client_number, slice_end_timestamp);
            }
            task_context_set_done(context, SERVER_CONTINUE, context->output, context->output_size);
            return TASK_RUN_COMPLETE;
        }

        if (scheduler_task_should_preempt(&scheduler, task)) {
            task_context_stop_child(context);
            if (task_context_drain_pipe(context) != 0) {
                char *msg = duplicate_text("Failed while capturing program output.\n");
                if (msg != NULL) {
                    task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
                }
                /* Update timeline */
                pthread_mutex_lock(&counter_mutex);
                current_slice_start_time += slices_run;
                slice_end_timestamp = current_slice_start_time;
                pthread_mutex_unlock(&counter_mutex);
                if (slices_run > 0) {
                    append_timeline_slice(task->client_number, slice_end_timestamp);
                }
                return TASK_RUN_FAILED;
            }
            /* Update timeline */
            pthread_mutex_lock(&counter_mutex);
            current_slice_start_time += slices_run;
            slice_end_timestamp = current_slice_start_time;
            had_preemption = 1;
            pthread_mutex_unlock(&counter_mutex);
            if (slices_run > 0) {
                append_timeline_slice(task->client_number, slice_end_timestamp);
            }
            return TASK_RUN_REQUEUE;
        }
    }

    if (task->remaining_burst > 0) {
        task_context_stop_child(context);
        if (task_context_drain_pipe(context) != 0) {
            char *msg = duplicate_text("Failed while capturing program output.\n");
            if (msg != NULL) {
                task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
            }
            /* Update timeline */
            pthread_mutex_lock(&counter_mutex);
            current_slice_start_time += slices_run;
            slice_end_timestamp = current_slice_start_time;
            pthread_mutex_unlock(&counter_mutex);
            if (slices_run > 0) {
                append_timeline_slice(task->client_number, slice_end_timestamp);
            }
            return TASK_RUN_FAILED;
        }
        /* Update timeline */
        pthread_mutex_lock(&counter_mutex);
        current_slice_start_time += slices_run;
        slice_end_timestamp = current_slice_start_time;
        had_preemption = 1;
        pthread_mutex_unlock(&counter_mutex);
        if (slices_run > 0) {
            append_timeline_slice(task->client_number, slice_end_timestamp);
        }
        return TASK_RUN_REQUEUE;
    }

    if (context->child_pid > 0) {
        waitpid(context->child_pid, NULL, 0);
        context->child_pid = -1;
        context->child_running = 0;
    }

    if (task_context_drain_pipe(context) != 0) {
        char *msg = duplicate_text("Failed while capturing program output.\n");
        if (msg != NULL) {
            task_context_set_done(context, SERVER_CONTINUE, msg, strlen(msg));
        }
        /* Update timeline */
        pthread_mutex_lock(&counter_mutex);
        current_slice_start_time += slices_run;
        slice_end_timestamp = current_slice_start_time;
        pthread_mutex_unlock(&counter_mutex);
        if (slices_run > 0) {
            append_timeline_slice(task->client_number, slice_end_timestamp);
        }
        return TASK_RUN_FAILED;
    }

    /* Update timeline */
    pthread_mutex_lock(&counter_mutex);
    current_slice_start_time += slices_run;
    slice_end_timestamp = current_slice_start_time;
    pthread_mutex_unlock(&counter_mutex);
    if (slices_run > 0) {
        append_timeline_slice(task->client_number, slice_end_timestamp);
    }
    task_context_set_done(context, SERVER_CONTINUE, context->output, context->output_size);
    return TASK_RUN_COMPLETE;
}

static void print_summary_if_needed(void) {
    pthread_mutex_lock(&counter_mutex);
    
    /* Only print if preemption occurred and we have timeline data */
    if (had_preemption && timeline_count > 0) {
        /* Build the timeline string like: (0)-P6-(3)-P7-(6)-P6-(13)-P7-(2) */
        char timeline_str[1024];
        timeline_str[0] = '\0';
        strlcat(timeline_str, "(0)", sizeof(timeline_str));
        int i;
        for (i = 0; i < timeline_count; i++) {
            char segment[64];
            snprintf(segment, sizeof(segment), "P%d-(%d)",
                     timeline[i].client_id, timeline[i].duration);
            strlcat(timeline_str, "-", sizeof(timeline_str));
            strlcat(timeline_str, segment, sizeof(timeline_str));
        }

        /* Print single-line summary */
        log_line("%s\n", timeline_str);

        /* Reset for next batch */
        had_preemption = 0;
        timeline_count = 0;
        current_running_client = -1;
        current_slice_start_time = 0;
    }
    
    pthread_mutex_unlock(&counter_mutex);
}

static TaskRunResult server_task_executor(Task *task, int quantum, void *callback_context) {
    TaskContext *context = task != NULL ? task->user_data : NULL;

    (void) callback_context;

    if (task == NULL || context == NULL) {
        return TASK_RUN_FAILED;
    }

    if (task->type == TASK_TYPE_SHELL) {
        return execute_shell_task(task, context);
    }

    return execute_program_task(task, quantum, context);
}

static void server_task_cleanup(Task *task, void *callback_context) {
    TaskContext *context = task != NULL ? task->user_data : NULL;

    (void) callback_context;

    if (context != NULL) {
        task_context_release(context);
    }

    scheduler_destroy_task(task);
}

static Task *create_shell_task(ClientContext *client, const char *command, TaskContext **context_out) {
    TaskContext *context = task_context_create();
    Task *task;

    if (context == NULL) {
        return NULL;
    }

    task = scheduler_create_task(TASK_TYPE_SHELL,
                                 client->client_number,
                                 command,
                                 -1,
                                 context);
    if (task == NULL) {
        task_context_release(context);
        return NULL;
    }

    *context_out = context;
    return task;
}

static Task *create_program_task(ClientContext *client,
                                 const char *command,
                                 TaskContext **context_out) {
    int iterations;
    char **argv;
    TaskContext *context;
    Task *task;

    argv = build_demo_argv(command, &iterations);
    if (argv == NULL) {
        return NULL;
    }

    context = task_context_create();
    if (context == NULL) {
        free_string_array(argv);
        return NULL;
    }

    context->argv = argv;

    task = scheduler_create_task(TASK_TYPE_PROGRAM,
                                 client->client_number,
                                 command,
                                 iterations,
                                 context);
    if (task == NULL) {
        task_context_release(context);
        return NULL;
    }

    *context_out = context;
    return task;
}

static int wait_for_task(TaskContext *context, ClientContext *client) {
    struct timeval tv;
    struct timespec ts;

    while (1) {
        pthread_mutex_lock(&context->mutex);
        if (context->done) {
            pthread_mutex_unlock(&context->mutex);
            return 0;
        }

        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = (tv.tv_usec + 100000) * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        (void) pthread_cond_timedwait(&context->ready, &context->mutex, &ts);
        pthread_mutex_unlock(&context->mutex);

        if (task_context_socket_closed(client->socket_fd)) {
            scheduler_cancel_client_tasks(&scheduler, client->client_number);
            return -1;
        }
    }
}

static int handle_task_response(ClientContext *client, TaskContext *context) {
    int status;
    char *payload;
    size_t payload_size;

    pthread_mutex_lock(&context->mutex);
    status = context->response_status;
    payload = context->output;
    payload_size = context->output_size;
    pthread_mutex_unlock(&context->mutex);

    if (send_response(client->socket_fd, (uint32_t) status, payload, payload_size) != 0) {
        return -1;
    }

    log_sent(client, payload_size);
    return 0;
}

static void maybe_print_summary_when_idle(void) {
    if (!scheduler_has_active_work(&scheduler)) {
        print_summary_if_needed();
    }
}

static void *handle_client(void *arg) {
    ClientContext *client = arg;

    while (1) {
        char command[BUFFER_SIZE];
        ssize_t recv_result;

        memset(command, 0, sizeof(command));
        recv_result = recv_command(client->socket_fd, command, sizeof(command));
        if (recv_result <= 0) {
            scheduler_cancel_client_tasks(&scheduler, client->client_number);
            log_disconnect(client, "client disconnected");
            break;
        }

        log_received(client, command);

        if (strcmp(command, "exit") == 0) {
            send_response(client->socket_fd, SERVER_EXIT, NULL, 0);
            log_sent(client, 0);
            log_disconnect(client, "client requested disconnect");
            break;
        }

        if (is_demo_command(command)) {
            TaskContext *context = NULL;
            Task *task = create_program_task(client, command, &context);

            if (task != NULL && context != NULL) {
                if (scheduler_submit_task(&scheduler, task) < 0) {
                    task_context_release(context);
                    break;
                }

                if (wait_for_task(context, client) < 0) {
                    task_context_release(context);
                    break;
                }

                if (handle_task_response(client, context) != 0) {
                    task_context_release(context);
                    break;
                }

                maybe_print_summary_when_idle();

                task_context_release(context);
                continue;
            }
        }

        {
            TaskContext *context = NULL;
            Task *task = create_shell_task(client, command, &context);

            if (task == NULL || context == NULL) {
                break;
            }

            if (scheduler_submit_task(&scheduler, task) < 0) {
                task_context_release(context);
                break;
            }

            if (wait_for_task(context, client) < 0) {
                task_context_release(context);
                break;
            }

            if (handle_task_response(client, context) != 0) {
                task_context_release(context);
                break;
            }

            maybe_print_summary_when_idle();

            task_context_release(context);
        }
    }

    close(client->socket_fd);
    free(client);
    return NULL;
}

int main(void) {
    int opt = 1;
    int server_socket;
    struct sockaddr_in server_address;
    SchedulerConfig config = {3, 7, DEMO_DEFAULT_BURST};

    if (scheduler_init(&scheduler,
                       &config,
                       server_task_executor,
                       server_task_cleanup,
                       scheduler_logger,
                       &scheduler) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize scheduler.\n");
        return EXIT_FAILURE;
    }

    if (scheduler_start(&scheduler) != 0) {
        fprintf(stderr, "[ERROR] Failed to start scheduler thread.\n");
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    scheduler_started = 1;

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        scheduler_request_stop(&scheduler);
        scheduler_join(&scheduler);
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_socket);
        scheduler_request_stop(&scheduler);
        scheduler_join(&scheduler);
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        perror("bind");
        close(server_socket);
        scheduler_request_stop(&scheduler);
        scheduler_join(&scheduler);
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        scheduler_request_stop(&scheduler);
        scheduler_join(&scheduler);
        scheduler_destroy(&scheduler);
        return EXIT_FAILURE;
    }

    log_line("------------------------------\n");
    log_line("| Hello, Server Started |\n");
    log_line("------------------------------\n");

    while (1) {
        ClientContext *client = malloc(sizeof(*client));
        int client_socket;
        socklen_t client_length;
        pthread_t thread_id;

        if (client == NULL) {
            perror("malloc");
            break;
        }

        client_length = sizeof(client->address);
        client_socket = accept(server_socket,
                               (struct sockaddr *) &client->address,
                               &client_length);
        if (client_socket < 0) {
            free(client);
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&counter_mutex);
        client->socket_fd = client_socket;
        client->client_number = next_client_number++;
        client->thread_number = next_thread_number++;
        pthread_mutex_unlock(&counter_mutex);

        log_connection_open(client);

        if (pthread_create(&thread_id, NULL, handle_client, client) != 0) {
            perror("pthread_create");
            close(client_socket);
            free(client);
            continue;
        }

        if (pthread_detach(thread_id) != 0) {
            perror("pthread_detach");
            close(client_socket);
            free(client);
            continue;
        }
    }

    close(server_socket);

    if (scheduler_started) {
        scheduler_request_stop(&scheduler);
        scheduler_join(&scheduler);
        scheduler_destroy(&scheduler);
    }

    return EXIT_SUCCESS;
}
