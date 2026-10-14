#ifndef _PARSER_H_
#define _PARSER_H_


#include "../stdc/os.h"
#include "../stdc/type.h"


#include "protocol.h"


enum {
    PARSER_OPTION_PORT = 1 << 0,
    PARSER_OPTION_KEY = 1 << 1,
    PARSER_OPTION_HOST = 1 << 2,
    PARSER_OPTION_PROXY = 1 << 3
};


typedef enum ParserCommand {
    PARSER_COMMAND_RUN,
    PARSER_COMMAND_CONFIG,
    PARSER_COMMAND_SEND,
    PARSER_COMMAND_LIST,
    PARSER_COMMAND_DOWNLOAD,
    PARSER_COMMAND_REMOVE
} ParserCommand;


typedef struct ParserArguments {
    ParserCommand command;
    char *path;
    char *key;
    char *host;
    char *proxy;
    int port;
} ParserArguments;


void parser_username(char *buffer, char **envs);


char *parser_command_name(ParserCommand command);


int parser_flags(int argc, char **argv, ParserArguments *args, char *error, usize error_size);


void __parser_error__(char *error, usize error_size, char *message);


#endif
