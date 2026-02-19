#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "exec.h"
#include "errors.h"

/*
 * run_command_basic
 * -----------------
 * Executes a single command using fork + execvp.
 *
 * argv must be NULL-terminated.
 * Parent waits for child before returning to shell prompt.
 */
int run_command_basic(char *argv[]) {

    // Create child process
    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        // ---------------------
        // CHILD PROCESS
        // ---------------------

        /*
         * Replace child process image with requested command.
         * execvp searches PATH automatically.
         * If execvp succeeds, this line is NEVER reached.
         */
        execvp(argv[0], argv);

        /*
         * If execvp returns, it means execution failed.
         * We must exit child to avoid running parent code.
         */
        fprintf(stderr, "%s\n", ERR_COMMAND_NOT_FOUND);
        exit(EXIT_FAILURE);
    }

    // ---------------------
    // PARENT PROCESS
    // ---------------------

    /*
     * Parent must wait for child to finish
     * before printing next prompt.
     */
    wait(NULL);

    return 0;
}
