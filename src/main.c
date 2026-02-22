#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "exec.h"

/*
 * MAX_LINE
 * --------
 * Maximum number of characters allowed in a single
 * command line input.
 */
#define MAX_LINE 1024

/*
 * main
 * ----
 * Entry point of the shell program.
 *
 * Responsibilities:
 *   1. Display shell prompt
 *   2. Read user input safely
 *   3. Handle built-in "exit" command
 *   4. Pass input to parser
 *   5. Execute parsed command
 *   6. Free allocated memory
 *
 * This loop continues until the user types "exit"
 * or EOF is encountered.
 */
int main() {

    /*
     * Buffer to store raw input line.
     * Stored on stack with fixed maximum size.
     */
    char line[MAX_LINE];


    /*
     * Infinite loop to continuously prompt user.
     * The loop only exits when "exit" is entered
     * or input stream closes.
     */
    while (1) {

        /*
         * Display shell prompt.
         * fflush ensures prompt appears immediately
         * (important when output is buffered).
         */
        printf("$ ");
        fflush(stdout);

        /*
         * Read user input safely using fgets.
         * fgets prevents buffer overflow by limiting
         * number of characters read.
         *
         * If fgets returns NULL:
         *   - EOF (Ctrl+D)
         *   - Input error
         * In either case, exit the shell.
         */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        /*
         * Remove trailing newline character.
         * fgets includes '\n' if space permits.
         * strcspn finds index of newline and replaces
         * it with null terminator.
         */
        line[strcspn(line, "\n")] = 0;


        /*
         * Built-in command: exit
         * If user types "exit", terminate shell.
         *
         * This is handled directly in main
         * (not using execvp), since exit is
         * a shell built-in behavior.
         */
        if (strcmp(line, "exit") == 0) {
            break;
        }

        /*
         * Parse user input into structured form.
         * parse_line returns:
         *   - Pointer to Pipeline if valid
         *   - NULL if empty input or syntax error
         */
        Pipeline *pipeline = parse_line(line);

        /*
         * If parser returned NULL:
         *   - User pressed Enter
         *   - Syntax error detected
         *
         * In both cases, simply re-prompt.
         */
        if (pipeline == NULL) {
            continue;
        }

        // Execute the parsed command
        run_command_basic(&pipeline->commands[0]);

        // Free Free dynamically allocated memory to prevent memory leaks.
        free(pipeline->commands[0].argv);
        free(pipeline->commands);
        free(pipeline);
    }

    return 0;
}
