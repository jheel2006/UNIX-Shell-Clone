#ifndef SHELL_CORE_H
#define SHELL_CORE_H

#include <stddef.h>

/*
 * Shared shell entry points reused by the local shell now and
 * by the remote server implementation in Phase 4.
 */
int shell_run_interactive(void);

/*
 * Executes one command line using the shared Phase 4 parser and
 * executor pipeline. Returns 1 only when the caller should treat
 * the line as an "exit" request; otherwise returns 0.
 */
int shell_execute_line(char *line);

/*
 * Executes one command line through the same shared Phase 4 shell path
 * while capturing both stdout and stderr into a heap buffer.
 *
 * On success:
 *   - *captured_output points to a malloc-allocated buffer
 *   - *captured_size stores the exact byte count in that buffer
 *
 * The caller owns the returned buffer and must free it.
 * The return value follows shell_execute_line():
 *   - 1 means "exit" was requested
 *   - 0 means normal command processing completed
 */
int shell_execute_line_capture(char *line,
                               char **captured_output,
                               size_t *captured_size);

#endif
