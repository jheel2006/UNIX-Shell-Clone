#ifndef SHELL_CORE_H
#define SHELL_CORE_H

/*
 * Shared shell entry points reused by the local shell now and
 * by the remote server implementation in Phase 2.
 */
int shell_run_interactive(void);
int shell_execute_line(char *line);

#endif
