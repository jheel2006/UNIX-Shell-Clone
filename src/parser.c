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
 * parse_line
 * ----------
 * Parses a single command line (no pipes yet)
 * and constructs a Pipeline structure containing
 * exactly one Command.
 *
 * Responsibilities:
 *   - Tokenize input by spaces
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
     * If user pressed Enter without typing anything,
     * return NULL so the shell simply re-prompts.
     */
    if (line == NULL || strlen(line) == 0) {
        return NULL;
    }

    /*
     * Allocate memory for Pipeline structure.
     * This represents the entire command line.
     */
    Pipeline *pipeline = malloc(sizeof(Pipeline));
    if (!pipeline) return NULL;

    pipeline->num_commands = 1;

    /*
     * Allocate memory for exactly one Command.
     */
    pipeline->commands = malloc(sizeof(Command));
    if (!pipeline->commands) return NULL;

    Command *cmd = &pipeline->commands[0];

    /*
     * Initialize redirection pointers to NULL.
     * If no redirection is provided,
     * executor will skip those sections.
     */
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->error_file = NULL;

    /*
     * Allocate memory for argument vector (argv).
     * execvp() requires argv to be NULL-terminated.
     */
    cmd->argv = malloc(sizeof(char*) * MAX_TOKENS);
    if (!cmd->argv) return NULL;

    int argc = 0;

     /*
     * Tokenize input using space delimiter.
     * strtok modifies the original string.
     */
    char *token = strtok(line, " ");

    while (token != NULL) {

        /*
         * ==========================
         * INPUT REDIRECTION (<)
         * ==========================
         *
         * If token is "<", the next token
         * must be a valid filename.
         */
        if (strcmp(token, "<") == 0) {

            token = strtok(NULL, " ");

            /*
             * Syntax validation:
             * - No filename provided
             * - Next token is another operator
             */
            if (token == NULL ||
                strcmp(token, "<") == 0 ||
                strcmp(token, ">") == 0 ||
                strcmp(token, "2>") == 0) {

                fprintf(stderr, "%s\n", ERR_MISSING_INPUT);
                free(cmd->argv);
                free(pipeline->commands);
                free(pipeline);
                return NULL;
            }

            cmd->input_file = token;
        }

        /*
         * ==========================
         * OUTPUT REDIRECTION (>)
         * ==========================
         */
        else if (strcmp(token, ">") == 0) {

            token = strtok(NULL, " ");

            if (token == NULL ||
                strcmp(token, "<") == 0 ||
                strcmp(token, ">") == 0 ||
                strcmp(token, "2>") == 0) {

                fprintf(stderr, "%s\n", ERR_MISSING_OUTPUT);
                free(cmd->argv);
                free(pipeline->commands);
                free(pipeline);
                return NULL;
            }

            cmd->output_file = token;
        }


        /*
         * ==========================
         * ERROR REDIRECTION (2>)
         * ==========================
         */
        else if (strcmp(token, "2>") == 0) {

            token = strtok(NULL, " ");

            if (token == NULL ||
                strcmp(token, "<") == 0 ||
                strcmp(token, ">") == 0 ||
                strcmp(token, "2>") == 0) {

                fprintf(stderr, "%s\n", ERR_MISSING_ERROR_FILE);
                free(cmd->argv);
                free(pipeline->commands);
                free(pipeline);
                return NULL;
            }

            cmd->error_file = token;
        }


        /*
         * ==========================
         * NORMAL ARGUMENT
         * ==========================
         *
         * Any token that is not a redirection
         * operator is considered part of argv.
         *
         * These will be passed directly to execvp().
         */
        else {
            cmd->argv[argc++] = token;
        }

        token = strtok(NULL, " ");
    }

    /*
     * argv must always be NULL-terminated
     * for execvp() to function correctly.
     */
    cmd->argv[argc] = NULL;


    /*
     * If no executable was provided (e.g. only "< file"),
     * this is invalid. Do not execute.
     */
    if (argc == 0) {
        free(cmd->argv);
        free(pipeline->commands);
        free(pipeline);
        return NULL;
    }

    return pipeline;
}
