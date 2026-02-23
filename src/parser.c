#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "errors.h"

/*
 * MAX_TOKENS
 * ----------
 * Maximum number of space-separated tokens
 * allowed in a single command.
 */

#define MAX_TOKENS 100

/*
 * trim_whitespace
 * ---------------
 * Trims leading/trailing spaces and tabs in-place and
 * returns the first non-whitespace character.
 */
static char *trim_whitespace(char *s) {
    char *end;

    while (*s == ' ' || *s == '\t') {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }

    return s;
}

/*
 * free_commands
 * -------------
 * Frees argv buffers for commands [0, count).
 * Used on parser error paths.
 */
static void free_commands(Command *commands, int count) {
    int i;

    if (commands == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(commands[i].argv);
    }
}

/*
 * parse_line
 * ----------
 * Parses a command line that may include one or more
 * commands connected by pipes.
 *
 * Responsibilities:
 *   - Split input by pipe delimiter '|'
 *   - Tokenize each command by spaces/tabs
 *   - Detect redirection operators (<, >, 2>)
 *   - Store filenames for redirection
 *   - Build argv[] array for execvp()
 *   - Validate syntax errors (missing filenames)
 *
 * Returns:
 *   - Pointer to Pipeline on success
 *   - NULL if input is empty or syntax error occurs
 */
Pipeline* parse_line(char *line) {
    /*
     * cursor walks across the full input line while strsep()
     * extracts one pipe segment at a time.
     */
    char *cursor;
    /* Raw command segment between two '|' delimiters. */
    char *segment;
    /* Dynamic capacity for pipeline->commands array. */
    int capacity = 4;
    /* Number of commands parsed successfully so far. */
    int cmd_count = 0;
    Pipeline *pipeline;

    /*
     * If user pressed Enter without typing anything,
     * return NULL so the shell simply re-prompts.
     */
    if (line == NULL || strlen(line) == 0) {
        return NULL;
    }

    pipeline = malloc(sizeof(Pipeline));
    if (!pipeline) return NULL;

    pipeline->commands = malloc(sizeof(Command) * capacity);
    if (!pipeline->commands) return NULL;

    cursor = line;
    /*
     * Split the line by '|'. Each segment is parsed into one Command.
     */
    while ((segment = strsep(&cursor, "|")) != NULL) {
        Command *cmd;
        char *token;
        char *save_token = NULL;
        /* argc counts executable + arguments for this command only. */
        int argc = 0;
        char *trimmed = trim_whitespace(segment);

        /*
         * Reject empty segments:
         *   "cmd1 |"            -> missing command after pipe
         *   "cmd1 | | cmd2"     -> empty command between pipes
         */
        if (*trimmed == '\0') {
            if (cursor == NULL) {
                fprintf(stderr, "%s\n", ERR_MISSING_PIPE_COMMAND);
            } else {
                fprintf(stderr, "%s\n", ERR_EMPTY_PIPE);
            }
            free_commands(pipeline->commands, cmd_count);
            free(pipeline->commands);
            free(pipeline);
            return NULL;
        }

        if (cmd_count == capacity) {
            Command *new_commands;
            /* Grow command array geometrically for scalability. */
            capacity *= 2;
            new_commands = realloc(pipeline->commands, sizeof(Command) * capacity);
            if (new_commands == NULL) {
                free_commands(pipeline->commands, cmd_count);
                free(pipeline->commands);
                free(pipeline);
                return NULL;
            }
            pipeline->commands = new_commands;
        }

        cmd = &pipeline->commands[cmd_count];
        /*
         * Reset redirection fields for this command.
         * Each command tracks its own local redirections.
         */
        cmd->input_file = NULL;
        cmd->output_file = NULL;
        cmd->error_file = NULL;
        cmd->argv = malloc(sizeof(char*) * MAX_TOKENS);
        if (cmd->argv == NULL) {
            free_commands(pipeline->commands, cmd_count);
            free(pipeline->commands);
            free(pipeline);
            return NULL;
        }

        /*
         * Tokenize one command segment.
         * Delimiters include both spaces and tabs.
         */
        token = strtok_r(trimmed, " \t", &save_token);
        while (token != NULL) {
            if (strcmp(token, "<") == 0) {
                /* '<' requires a following filename token. */
                token = strtok_r(NULL, " \t", &save_token);
                if (token == NULL ||
                    strcmp(token, "<") == 0 ||
                    strcmp(token, ">") == 0 ||
                    strcmp(token, "2>") == 0) {

                    fprintf(stderr, "%s\n", ERR_MISSING_INPUT);
                    free_commands(pipeline->commands, cmd_count + 1);
                    free(pipeline->commands);
                    free(pipeline);
                    return NULL;
                }
                cmd->input_file = token;
            } else if (strcmp(token, ">") == 0) {
                /* '>' requires a following output filename token. */
                token = strtok_r(NULL, " \t", &save_token);
                if (token == NULL ||
                    strcmp(token, "<") == 0 ||
                    strcmp(token, ">") == 0 ||
                    strcmp(token, "2>") == 0) {

                    fprintf(stderr, "%s\n", ERR_MISSING_OUTPUT);
                    free_commands(pipeline->commands, cmd_count + 1);
                    free(pipeline->commands);
                    free(pipeline);
                    return NULL;
                }
                cmd->output_file = token;
            } else if (strcmp(token, "2>") == 0) {
                /* '2>' requires a following stderr filename token. */
                token = strtok_r(NULL, " \t", &save_token);
                if (token == NULL ||
                    strcmp(token, "<") == 0 ||
                    strcmp(token, ">") == 0 ||
                    strcmp(token, "2>") == 0) {

                    fprintf(stderr, "%s\n", ERR_MISSING_ERROR_FILE);
                    free_commands(pipeline->commands, cmd_count + 1);
                    free(pipeline->commands);
                    free(pipeline);
                    return NULL;
                }
                cmd->error_file = token;
            } else {
                /*
                 * Normal argument token.
                 * First one becomes argv[0] (program name).
                 */
                cmd->argv[argc++] = token;
            }

            token = strtok_r(NULL, " \t", &save_token);
        }

        /*
         * Reject segments that contain only redirections and no command.
         * We treat this as a pipe syntax error in the current phase rules.
         */
        if (argc == 0) {
            if (cursor == NULL) {
                fprintf(stderr, "%s\n", ERR_MISSING_PIPE_COMMAND);
            } else {
                fprintf(stderr, "%s\n", ERR_EMPTY_PIPE);
            }
            free_commands(pipeline->commands, cmd_count + 1);
            free(pipeline->commands);
            free(pipeline);
            return NULL;
        }

        cmd->argv[argc] = NULL;
        /* Command fully parsed successfully. */
        cmd_count++;
    }

    if (cmd_count == 0) {
        free(pipeline->commands);
        free(pipeline);
        return NULL;
    }

    /* Final parsed pipeline descriptor consumed by execution layer. */
    pipeline->num_commands = cmd_count;
    return pipeline;
}
