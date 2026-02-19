#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "exec.h"

#define MAX_LINE 1024

int main() {
    char line[MAX_LINE];

    while (1) {

        printf("$ ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        // Remove newline
        line[strcspn(line, "\n")] = 0;

        // Exit command
        if (strcmp(line, "exit") == 0) {
            break;
        }

        // Parse input
        Pipeline *pipeline = parse_line(line);

        // If empty input → just re-prompt
        if (pipeline == NULL) {
            continue;
        }

        // Execute single command
        run_command_basic(pipeline->commands[0].argv);

        // Free allocated memory
        free(pipeline->commands[0].argv);
        free(pipeline->commands);
        free(pipeline);
    }

    return 0;
}
