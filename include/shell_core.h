#ifndef SHELL_CORE_H
#define SHELL_CORE_H

/*
 * Shared shell entry points reused by the local shell now and
 * by the remote server implementation in Phase 2.
 */
int shell_run_interactive(void);

/*
 * Executes one command line using the shared Phase 1 parser and
 * executor pipeline. Returns 1 only when the caller should treat
 * the line as an "exit" request; otherwise returns 0.
 */
int shell_execute_line(char *line);

#endif
