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
    int i, j;

    if (commands == NULL) return;

    for (i = 0; i < count; i++) {

        if (commands[i].argv != NULL) {
            for (j = 0; commands[i].argv[j] != NULL; j++) {
                free(commands[i].argv[j]);
            }
            free(commands[i].argv);
        }

        if (commands[i].input_file)
            free(commands[i].input_file);

        if (commands[i].output_file)
            free(commands[i].output_file);

        if (commands[i].error_file)
            free(commands[i].error_file);
    }
}


/*
 * next_token
 * ----------
 * Extracts the next token from *input.
 * Supports grouping by single and double quotes.
 * Modifies input pointer to advance position.
 */
static char *next_token(char **input) {
    static char buffer[4096];
    int i = 0;

    char *p = *input;

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p == '\0') {
        *input = p;
        return NULL;
    }

    enum { OUTSIDE, IN_SINGLE, IN_DOUBLE } state = OUTSIDE;

    while (*p) {

        /* OUTSIDE QUOTES */
        if (state == OUTSIDE) {

            if (*p == ' ' || *p == '\t') {
                break;
            }

            else if (*p == '\'') {
                state = IN_SINGLE;
                p++;
            }

            else if (*p == '"') {
                state = IN_DOUBLE;
                p++;
            }

            else if (*p == '\\') {
                /* Escape next character */
                p++;
                if (*p) {
                    buffer[i++] = *p++;
                }
            }

            else {
                buffer[i++] = *p++;
            }
        }

        /* INSIDE SINGLE QUOTES */
        else if (state == IN_SINGLE) {

            if (*p == '\'') {
                state = OUTSIDE;
                p++;
            } else {
                buffer[i++] = *p++;
            }
        }

        /* INSIDE DOUBLE QUOTES */
        else if (state == IN_DOUBLE) {

            if (*p == '"') {
                state = OUTSIDE;
                p++;
            }

            else if (*p == '\\') {
                p++;
                if (*p == 'n') {
                    buffer[i++] = '\n';
                    p++;
                }
                else if (*p == '"' || *p == '\\') {
                    buffer[i++] = *p++;
                }
                else {
                    /* Unknown escape, keep literal */
                    buffer[i++] = '\\';
                }
            }

            else {
                buffer[i++] = *p++;
            }
        }
    }

    buffer[i] = '\0';

    if (state != OUTSIDE) {
        fprintf(stderr, "Unmatched quote");
        return NULL;
    }

    *input = p;

    return buffer;
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
    /*
     * Tracks segments that contained only redirections and no executable.
     * We defer reporting this so later segments can surface more specific
     * errors (e.g., missing output target after '>').
     */
    int deferred_empty_segment = 0;
    Pipeline *pipeline;

    /*
     * If user pressed Enter without typing anything or only types spaces,
     * return NULL so the shell simply re-prompts.
     */
    if (line == NULL) return NULL;

    char *trimmed_line = trim_whitespace(line);

    if (*trimmed_line == '\0') return NULL;
    

    pipeline = malloc(sizeof(Pipeline));
    if (!pipeline) return NULL;

    pipeline->commands = calloc(capacity, sizeof(Command));
    if (!pipeline->commands) {
        free(pipeline);
        return NULL;
    }

    cursor = line;
    /*
     * Split the line by '|'. Each segment is parsed into one Command.
     */
    while ((segment = strsep(&cursor, "|")) != NULL) {
        Command *cmd;
        char *token;
        // char *save_token = NULL;
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
            int old_capacity = capacity;
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
            memset(&pipeline->commands[old_capacity], 0,
                   sizeof(Command) * (capacity - old_capacity));
        }

        cmd = &pipeline->commands[cmd_count];
        /*
         * Reset redirection fields for this command.
         * Each command tracks its own local redirections.
         */
        cmd->input_file = NULL;
        cmd->output_file = NULL;
        cmd->error_file = NULL;
        cmd->argv = calloc(MAX_TOKENS, sizeof(char *));
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
        // token = strtok_r(trimmed, " \t", &save_token);
        char *scan = trimmed;
        while ((token = next_token(&scan)) != NULL) {
            if (strcmp(token, "<") == 0) {
                /* '<' requires a following filename token. */
                token = next_token(&scan);
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
                cmd->input_file = strdup(token);
            } else if (strcmp(token, ">") == 0) {
                /* '>' requires a following output filename token. */
                token = next_token(&scan);
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
                cmd->output_file = strdup(token);
            } else if (strcmp(token, "2>") == 0) {
                /* '2>' requires a following stderr filename token. */
                token = next_token(&scan);
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
                cmd->error_file = strdup(token);
            } else {
                /*
                 * Normal argument token.
                 * First one becomes argv[0] (program name).
                 */
                cmd->argv[argc++] = strdup(token);
            }

            // token = strtok_r(NULL, " \t", &save_token);
        }

        /*
         * Reject segments that contain only redirections and no command.
         * We treat this as a pipe syntax error in the current phase rules.
         */
        if (argc == 0) {
            /*
             * Special handling:
             * If this segment had only redirections and there are more
             * segments to parse, defer the "empty segment" error for now.
             * This lets inputs like:
             *   < input.txt | command1 >
             * report the more specific:
             *   Output file not specified.
             * from the next segment, as required by the assignment.
             */
            if (cursor != NULL &&
                (cmd->input_file != NULL ||
                 cmd->output_file != NULL ||
                 cmd->error_file != NULL)) {
                deferred_empty_segment = 1;
                free(cmd->argv);
                continue;
            }

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

    /*
     * If we deferred an empty redirection-only segment but no later
     * specific parser error occurred, report it as an empty pipe segment.
     */
    if (deferred_empty_segment) {
        fprintf(stderr, "%s\n", ERR_EMPTY_PIPE);
        free_commands(pipeline->commands, cmd_count);
        free(pipeline->commands);
        free(pipeline);
        return NULL;
    }

    /* Final parsed pipeline descriptor consumed by execution layer. */
    pipeline->num_commands = cmd_count;
    return pipeline;
}
