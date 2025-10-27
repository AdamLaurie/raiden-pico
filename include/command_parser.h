#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include <stdint.h>
#include <stdbool.h>

// Maximum number of command parts (command + arguments)
#define MAX_CMD_PARTS 16

// Command parsing result
typedef struct {
    char *parts[MAX_CMD_PARTS];
    uint8_t count;
} cmd_parts_t;

// Initialize the command parser
void command_parser_init(void);

// Parse a command string into parts
bool command_parser_parse(const char *cmd, cmd_parts_t *parts);

// Match abbreviated command against candidates
// Returns matched string, or NULL if ambiguous
const char* command_parser_match(const char *abbrev, const char **candidates, uint8_t count);

// Execute a parsed command
void command_parser_execute(cmd_parts_t *parts);

#endif // COMMAND_PARSER_H
