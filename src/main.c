#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE 1024

int main() {
    char line[MAX_LINE];

    while (1) {
        printf("$ ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        // Remove newline character
        line[strcspn(line, "\n")] = 0;

        // Exit condition
        if (strcmp(line, "exit") == 0) {
            break;
        }

        // For Day 1 we are not executing yet
        // Just confirm shell loop works
        if (strlen(line) > 0) {
            printf("You entered: %s\n", line);
        }
    }

    return 0;
}
