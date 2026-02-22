#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "exec.h"
#include "errors.h"


/*
 * run_command_basic
 * -----------------
 * Executes a single command (no pipes) with optional
 * input (<), output (>), and error (2>) redirection.
 *
 * This function:
 *   1. Creates a child process using fork()
 *   2. Performs redirection in the child (if requested)
 *   3. Replaces the child process image using execvp()
 *   4. Ensures the parent waits before showing the prompt again
 *
 * Important:
 * Redirection MUST happen in the child process only.
 * The parent shell must not modify its own file descriptors,
 * otherwise future commands would inherit redirected streams.
 */
int run_command_basic(Command *cmd) {

    pid_t pid = fork();

    /*
     * fork() creates a new process.
     * - Parent receives child's PID (> 0)
     * - Child receives 0
     * - On failure, returns -1
     */
    if (pid < 0) {
        // Fork failed → no child created
        perror("fork");
        return -1;
    }

    /*
     * Child Process
     * --------------
     * All redirection logic and exec() must occur here.
     * If execvp() succeeds, this code never returns.
     */
    if (pid == 0) {

        /*
         * ==========================
         * INPUT REDIRECTION (<)
         * ==========================
         *
         * If user provided an input file:
         *   - Open file in read-only mode
         *   - Replace STDIN (fd 0) with file descriptor
         *   - Close original fd (no longer needed)
         */
        if (cmd->input_file != NULL) {

            int fd = open(cmd->input_file, O_RDONLY);

            /*
            * open() failed (file does not exist,
            * permission denied, etc.)
            * Child must exit safely.
            */
            if (fd < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }

            /*
             * dup2 replaces STDIN_FILENO (0) with fd.
             * After this call, any read from stdin
             * will read from the specified file.
             */
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        /*
         * ==========================
         * OUTPUT REDIRECTION (>)
         * ==========================
         *
         * Flags:
         *   O_WRONLY → write only
         *   O_CREAT  → create file if it doesn't exist
         *   O_TRUNC  → overwrite existing file
         *
         * Mode 0644:
         *   Owner: read/write
         *   Group/Others: read
         */
        if (cmd->output_file != NULL) {

            int fd = open(cmd->output_file,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          0644);

            if (fd < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }

            /*
             * Redirect standard output (fd 1)
             * so printf()/write() output goes to file.
             */
            dup2(fd, STDOUT_FILENO);

            close(fd);
        }

        /*
         * ==========================
         * ERROR REDIRECTION (2>)
         * ==========================
         *
         * Same logic as output redirection,
         * but applied to STDERR (fd 2).
         */
        if (cmd->error_file != NULL) {

            int fd = open(cmd->error_file,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          0644);

            if (fd < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }
            /*
             * Redirect standard error (fd 2)
             * so error messages are written to file.
             */
            dup2(fd, STDERR_FILENO);

            close(fd);
        }

        /*
         * ==========================
         * EXECUTION
         * ==========================
         *
         * execvp replaces the current process image
         * with the program specified in argv[0].
         *
         * execvp searches for the program using PATH.
         *
         * If execvp succeeds:
         *   → The child process memory is replaced
         *   → Code below will NEVER execute
         *
         * If execvp fails:
         *   → It returns -1
         */
        execvp(cmd->argv[0], cmd->argv);

        /*
         * If we reach this line,
         * execvp failed (invalid command).
         */
        fprintf(stderr, "%s\n", ERR_COMMAND_NOT_FOUND);

        exit(EXIT_FAILURE);
    }

    /*
     * Parent Process
     * --------------
     * The shell must wait for the child to finish
     * before printing the next prompt.
     *
     * This prevents overlapping prompts and ensures
     * sequential command execution.
     */
    wait(NULL);
    return 0;
}
