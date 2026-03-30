#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    if (pipeline == NULL) {
        return;
    }

    for (i = 0; i < pipeline->num_commands; i++) {
        free(pipeline->commands[i].argv);
    }

    free(pipeline->commands);
    free(pipeline);
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
