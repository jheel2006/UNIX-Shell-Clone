#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

#define MAX_TOKENS 100

/*
 * parse_line
 * ----------
 * Parses a single command line with NO pipes.
 * Splits by spaces and builds argv.
 * Returns a Pipeline containing exactly 1 command.
 */
Pipeline* parse_line(char *line) {

    // Detect empty input (just Enter)
    if (line == NULL || strlen(line) == 0) {
        return NULL;
    }

    // Allocate Pipeline
    Pipeline *pipeline = malloc(sizeof(Pipeline));
    if (!pipeline) return NULL;

    pipeline->num_commands = 1;
    pipeline->commands = malloc(sizeof(Command));
    if (!pipeline->commands) return NULL;

    Command *cmd = &pipeline->commands[0];

    // No redirection yet
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->error_file = NULL;

    // Allocate argv array
    cmd->argv = malloc(sizeof(char*) * MAX_TOKENS);
    if (!cmd->argv) return NULL;

    int argc = 0;

    // Tokenize by spaces
    char *token = strtok(line, " ");
    while (token != NULL && argc < MAX_TOKENS - 1) {
        cmd->argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    cmd->argv[argc] = NULL; // IMPORTANT: NULL terminate

    // If user just typed spaces
    if (argc == 0) {
        free(cmd->argv);
        free(pipeline->commands);
        free(pipeline);
        return NULL;
    }

    return pipeline;
}
