#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * Reuses the Phase 1 parser and executors for a single command line.
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
 *
 * Using a temporary file keeps this capture path simple and avoids
 * pipe-buffer deadlocks when a command prints more than one small chunk
 * of output before the parent finishes waiting for child processes.
 */
int shell_execute_line_capture(char *line,
                               char **captured_output,
                               size_t *captured_size) {
    FILE *capture_file;
    int saved_stdout;
    int saved_stderr;
    int capture_fd;
    int exit_requested;
    long file_size;
    char *buffer;

    if (captured_output == NULL || captured_size == NULL) {
        return 1;
    }

    *captured_output = NULL;
    *captured_size = 0;

    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0) {
        if (saved_stdout >= 0) close(saved_stdout);
        if (saved_stderr >= 0) close(saved_stderr);
        *captured_output = duplicate_text("Failed to duplicate output streams.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return 0;
    }

    capture_file = tmpfile();
    if (capture_file == NULL) {
        close(saved_stdout);
        close(saved_stderr);
        *captured_output = duplicate_text("Failed to create temporary capture file.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return 0;
    }

    capture_fd = fileno(capture_file);

    /*
     * Flush any pending text first so only the target command's output
     * is redirected into the temporary capture file.
     */
    fflush(stdout);
    fflush(stderr);

    if (dup2(capture_fd, STDOUT_FILENO) < 0 ||
        dup2(capture_fd, STDERR_FILENO) < 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdout);
        close(saved_stderr);
        fclose(capture_file);
        *captured_output = duplicate_text("Failed to redirect output streams.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return 0;
    }

    exit_requested = shell_execute_line(line);

    /*
     * Flush the redirected streams before restoring the original terminal
     * file descriptors so every byte written by the command is persisted.
     */
    fflush(stdout);
    fflush(stderr);

    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);

    if (fseek(capture_file, 0L, SEEK_END) != 0) {
        fclose(capture_file);
        *captured_output = duplicate_text("Failed to size captured output.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return exit_requested;
    }

    file_size = ftell(capture_file);
    if (file_size < 0) {
        fclose(capture_file);
        *captured_output = duplicate_text("Failed to read captured output size.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return exit_requested;
    }

    if (fseek(capture_file, 0L, SEEK_SET) != 0) {
        fclose(capture_file);
        *captured_output = duplicate_text("Failed to rewind captured output.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return exit_requested;
    }

    buffer = malloc((size_t) file_size + 1);
    if (buffer == NULL) {
        fclose(capture_file);
        *captured_output = duplicate_text("Failed to allocate captured output buffer.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return exit_requested;
    }

    if (file_size > 0 &&
        fread(buffer, 1, (size_t) file_size, capture_file) != (size_t) file_size) {
        free(buffer);
        fclose(capture_file);
        *captured_output = duplicate_text("Failed to read captured output bytes.\n");
        if (*captured_output != NULL) {
            *captured_size = strlen(*captured_output);
        }
        return exit_requested;
    }

    buffer[file_size] = '\0';
    *captured_output = buffer;
    *captured_size = (size_t) file_size;

    fclose(capture_file);
    return exit_requested;
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
