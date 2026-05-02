#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "exec.h"
#include "parser.h"
#include "pipes.h"
#include "shell_core.h"

/*
 * MAX_LINE
 * --------
 * Maximum number of characters allowed in one command line.
 * Shared here so all front-ends can rely on the same limit.
 */
#define MAX_LINE 1024

/*
 * free_pipeline
 * -------------
 * Releases parser-owned storage after one command line finishes.
 */
static void free_pipeline(Pipeline *pipeline) {
    int i;
    int j;

    if (pipeline == NULL) {
        return;
    }

    for (i = 0; i < pipeline->num_commands; i++) {
        if (pipeline->commands[i].argv != NULL) {
            for (j = 0; pipeline->commands[i].argv[j] != NULL; j++) {
                free(pipeline->commands[i].argv[j]);
            }
            free(pipeline->commands[i].argv);
        }

        free(pipeline->commands[i].input_file);
        free(pipeline->commands[i].output_file);
        free(pipeline->commands[i].error_file);
    }

    free(pipeline->commands);
    free(pipeline);
}

/*
 * duplicate_text
 * --------------
 * Small helper used on capture error paths so callers still receive
 * a printable message buffer that follows the same ownership rules
 * as successful command output capture.
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
 * shell_execute_line
 * ------------------
 * Reuses the Phase 4 parser and executors for a single command line.
 * This function is intentionally front-end agnostic so both the local
 * shell and the remote server can drive the exact same execution path.
 *
 * Returns 0 after handling the line and 1 only when the caller should
 * interpret the input as an "exit" request.
 */
int shell_execute_line(char *line) {
    Pipeline *pipeline;

    if (line == NULL) {
        return 1;
    }

    line[strcspn(line, "\n")] = '\0';

    if (strcmp(line, "exit") == 0) {
        return 1;
    }

    pipeline = parse_line(line);
    if (pipeline == NULL) {
        return 0;
    }

    if (pipeline->num_commands == 1) {
        run_command_basic(&pipeline->commands[0]);
    } else {
        execute_pipeline(pipeline);
    }

    free_pipeline(pipeline);
    return 0;
}

/*
 * shell_execute_line_capture
 * --------------------------
 * Runs the same shared shell execution path as shell_execute_line(),
 * but temporarily redirects stdout and stderr to a temporary file so
 * the server can send the resulting output back to the remote client.
 * This is used for shell commands, not demo scheduling, because a shell
 * command is expected to finish in one schedulable round.
 *
 * Using a temporary file keeps this capture path simple and avoids
 * pipe-buffer deadlocks when a command prints more than one small chunk
 * of output before the parent finishes waiting for child processes.
 */
int shell_execute_line_capture(char *line,
                               char **captured_output,
                               size_t *captured_size) {
    int pipe_fd[2];
    pid_t child_pid;
    int child_status;
    size_t total_size = 0;
    size_t capacity = 0;
    ssize_t bytes_read;
    char chunk[4096];
    char *buffer = NULL;

    if (captured_output == NULL || captured_size == NULL) {
        return 1;
    }

    *captured_output = NULL;
    *captured_size = 0;

    if (pipe(pipe_fd) < 0) {
        *captured_output = duplicate_text("Failed to create capture pipe.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return 0;
    }

    child_pid = fork();
    if (child_pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        *captured_output = duplicate_text("Failed to fork capture process.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return 0;
    }

    if (child_pid == 0) {
        int exit_requested;

        close(pipe_fd[0]);

        if (dup2(pipe_fd[1], STDOUT_FILENO) < 0 ||
            dup2(pipe_fd[1], STDERR_FILENO) < 0) {
            _exit(EXIT_FAILURE);
        }

        close(pipe_fd[1]);

        exit_requested = shell_execute_line(line);
        fflush(stdout);
        fflush(stderr);
        _exit(exit_requested ? 1 : 0);
    }

    close(pipe_fd[1]);

    while ((bytes_read = read(pipe_fd[0], chunk, sizeof(chunk))) > 0) {
        char *resized_buffer;

        if (total_size + (size_t) bytes_read + 1 > capacity) {
            size_t new_capacity = capacity == 0 ? 4096 : capacity;

            while (new_capacity < total_size + (size_t) bytes_read + 1) {
                new_capacity *= 2;
            }

            resized_buffer = realloc(buffer, new_capacity);
            if (resized_buffer == NULL) {
                free(buffer);
                buffer = duplicate_text("Failed to allocate captured output buffer.\n");
                if (buffer != NULL) {
                    total_size = strlen(buffer);
                } else {
                    total_size = 0;
                }
                break;
            }

            buffer = resized_buffer;
            capacity = new_capacity;
        }

        memcpy(buffer + total_size, chunk, (size_t) bytes_read);
        total_size += (size_t) bytes_read;
    }

    close(pipe_fd[0]);

    if (waitpid(child_pid, &child_status, 0) < 0) {
        free(buffer);
        *captured_output = duplicate_text("Failed to wait for capture process.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return 0;
    }

    if (bytes_read < 0) {
        free(buffer);
        *captured_output = duplicate_text("Failed to read captured output bytes.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 1;
    }

    if (buffer == NULL) {
        buffer = malloc(1);
        if (buffer == NULL) {
            *captured_output = duplicate_text("Failed to allocate captured output buffer.\n");
            if (*captured_output != NULL) {
                *captured_size = strlen(*captured_output);
            }
            return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 1;
        }
    }

    buffer[total_size] = '\0';
    *captured_output = buffer;
    *captured_size = total_size;

    return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 1;
}

/*
 * shell_run_interactive
 * ---------------------
 * Local interactive REPL used by myshell.
 */
int shell_run_interactive(void) {
    char line[MAX_LINE];

    while (1) {
        printf("$ ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        if (shell_execute_line(line)) {
            break;
        }
    }

    return 0;
}
