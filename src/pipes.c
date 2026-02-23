#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "pipes.h"
#include "errors.h"

/*
 * execute_pipeline
 * ----------------
 * Runs a pipeline of commands connected with '|'.
 *
 * For each command:
 *   - Create a pipe (except for the last command)
 *   - Fork a child
 *   - Redirect stdin/stdout using dup2() as needed
 *   - Execute command using execvp()
 *
 * Parent process:
 *   - Closes unused pipe file descriptors promptly
 *   - Waits for all children to prevent zombies
 */
int execute_pipeline(Pipeline *pipeline) {
    int i;
    /*
     * prev_read_fd keeps the read end of the previous pipe.
     * It becomes stdin of the current command (except command 0).
     */
    int prev_read_fd = -1;
    /*
     * pipe_fd stores the current stage's pipe:
     *   pipe_fd[0] -> read end
     *   pipe_fd[1] -> write end
     */
    int pipe_fd[2] = {-1, -1};

    if (pipeline == NULL || pipeline->num_commands <= 0) {
        return -1;
    }

    pid_t *pids = malloc(sizeof(pid_t) * pipeline->num_commands);
    if (pids == NULL) {
        perror("malloc");
        return -1;
    }

    for (i = 0; i < pipeline->num_commands; i++) {
        Command *cmd = &pipeline->commands[i];

        /*
         * Every command except the last needs a new outgoing pipe.
         * Last command writes to terminal/stdout as usual.
         */
        if (i < pipeline->num_commands - 1) {
            if (pipe(pipe_fd) < 0) {
                perror("pipe");
                free(pids);
                return -1;
            }
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            free(pids);
            return -1;
        }

        if (pids[i] == 0) {
            /*
             * CHILD PROCESS
             * -------------
             * 1) If there is input from a previous command,
             *    map prev_read_fd -> STDIN.
             * 2) If this is not the last command,
             *    map current pipe write end -> STDOUT.
             */
            if (prev_read_fd != -1) {
                dup2(prev_read_fd, STDIN_FILENO);
            }

            if (i < pipeline->num_commands - 1) {
                dup2(pipe_fd[1], STDOUT_FILENO);
            }

            /*
             * Child closes all inherited pipe fds it no longer needs.
             * After dup2(), the original descriptors are redundant.
             */
            if (prev_read_fd != -1) {
                close(prev_read_fd);
            }
            if (i < pipeline->num_commands - 1) {
                close(pipe_fd[0]);
                close(pipe_fd[1]);
            }

            /*
             * Replace child image with target command.
             * On success this never returns.
             */
            execvp(cmd->argv[0], cmd->argv);
            fprintf(stderr, "%s\n", ERR_COMMAND_NOT_FOUND);
            _exit(EXIT_FAILURE);
        }

        /*
         * PARENT PROCESS
         * --------------
         * Parent should close descriptors as soon as possible to avoid
         * descriptor leaks and to allow downstream readers to observe EOF.
         */
        if (prev_read_fd != -1) {
            close(prev_read_fd);
        }

        if (i < pipeline->num_commands - 1) {
            /*
             * Parent never writes to this stage's pipe.
             * Keep only the read end for the next iteration.
             */
            close(pipe_fd[1]);
            prev_read_fd = pipe_fd[0];
        } else {
            prev_read_fd = -1;
        }
    }

    /*
     * Wait for all children so the shell prompt appears
     * only after pipeline completion.
     */
    for (i = 0; i < pipeline->num_commands; i++) {
        waitpid(pids[i], NULL, 0);
    }

    free(pids);
    return 0;
}
