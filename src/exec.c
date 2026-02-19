#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "exec.h"

int run_command_basic(char *argv[]) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("Fork failed");
        return -1;
    }

    if (pid == 0) {
        // Child process
        execvp(argv[0], argv);
        perror("execvp failed");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        wait(NULL);
    }

    return 0;
}
