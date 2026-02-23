#ifndef ERRORS_H
#define ERRORS_H

#define ERR_MISSING_INPUT "Input file not specified."
#define ERR_MISSING_OUTPUT "Output file not specified."
#define ERR_MISSING_ERROR_FILE "Error output file not specified."
#define ERR_MISSING_PIPE_COMMAND "Command missing after pipe."
#define ERR_EMPTY_PIPE "Empty command between pipes."
/*
 * Used only by pipeline execution path when execvp() fails
 * for one stage in a pipe sequence.
 */
#define ERR_COMMAND_IN_PIPE_SEQUENCE "Command not found in pipe sequence."
#define ERR_COMMAND_NOT_FOUND "Command not found."

#endif
