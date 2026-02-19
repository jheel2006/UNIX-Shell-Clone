#ifndef PARSER_H
#define PARSER_H

// Structure representing a single command
typedef struct {
    char **argv;          // Argument vector (NULL terminated)
    char *input_file;     // Input redirection file
    char *output_file;    // Output redirection file
    char *error_file;     // Error redirection file
} Command;

// Structure representing a pipeline of commands
typedef struct {
    Command *commands;    // Array of commands
    int num_commands;     // Number of commands in pipeline
} Pipeline;

// Function to parse a line into a Pipeline structure
Pipeline* parse_line(char *line);

#endif
