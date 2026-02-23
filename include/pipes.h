#ifndef PIPES_H
#define PIPES_H

#include "parser.h"

/*
 * Execute a parsed pipeline with one or more commands.
 * Returns 0 on success, -1 on failure.
 */
int execute_pipeline(Pipeline *pipeline);

#endif
