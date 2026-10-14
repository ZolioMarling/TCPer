#include "parser.h"


void parser_username(char *buffer, char **envs) {
    for (int i = 0; envs[i] != NULL; i++) {
        #if defined(__OS_UNIX__)
            if (strncmp(envs[i], "USER=", 5) == 0) {
                snprintf(buffer, 256, "%s", envs[i] + 5);
                return;
            }
        #elif defined(__OS_WINDOWS__)
            if (strncmp(envs[i], "USERNAME=", 9) == 0) {
                snprintf(buffer, 256, "%s", envs[i] + 9);
                return;
            }
        #endif
    }
    strcpy(buffer, "UNKNOWN");
}


char *parser_command_name(ParserCommand command) {
    switch (command) {
        case PARSER_COMMAND_RUN: return "run";
        case PARSER_COMMAND_CONFIG: return "config";
        case PARSER_COMMAND_SEND: return "send";
        case PARSER_COMMAND_LIST: return "list";
        case PARSER_COMMAND_DOWNLOAD: return "download";
        case PARSER_COMMAND_REMOVE: return "remove";
    }
    return NULL;
}


// static void __parser_error__(char *error, usize error_size, const char *message) {
//     if (error != NULL && error_size > 0) {
//         snprintf(error, error_size, "%s", message);
//     }
// }


void ____parser_error____(char *error, usize error_size, char *message) {
    if (error != NULL && error_size > 0) snprintf(error, error_size, "%s", message);
}


static int parser_set_port(ParserArguments *arguments, char *value, char *error, usize error_size) {
    char *end = NULL;
    long port;

    if (value[0] == '\0') {
        ____parser_error____(error, error_size, "--port requires a value");
        // if (error != NULL && error_size > 0) snprintf(error, error_size, "%s", "--port requires a value");
        return 1;
    }

    errno = 0;
    port = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0' || port < 1 || port > 65535) {
        __parser_error__(error, error_size, "--port must be an integer between 1 and 65535");
        return 1;
    }

    arguments->port = (int)port;
    return 0;
}





static int parser_parse_command(char *command, ParserCommand *parsed_command) {
    if (strcmp(command, "run") == 0) {
        *parsed_command = PARSER_COMMAND_RUN;
    } else if (strcmp(command, "config") == 0) {
        *parsed_command = PARSER_COMMAND_CONFIG;
    } else if (strcmp(command, "send") == 0) {
        *parsed_command = PARSER_COMMAND_SEND;
    } else if (strcmp(command, "list") == 0) {
        *parsed_command = PARSER_COMMAND_LIST;
    } else if (strcmp(command, "download") == 0) {
        *parsed_command = PARSER_COMMAND_DOWNLOAD;
    } else if (strcmp(command, "remove") == 0) {
        *parsed_command = PARSER_COMMAND_REMOVE;
    } else {
        return 1;
    }
    return 0;
}


static int parser_option_allowed(ParserCommand command, int option) {
    if (command == PARSER_COMMAND_RUN) {
        return option == PARSER_OPTION_PORT || option == PARSER_OPTION_KEY;
    }
    if (command == PARSER_COMMAND_CONFIG) {
        return option == PARSER_OPTION_HOST || option == PARSER_OPTION_PROXY;
    }
    return option == PARSER_OPTION_KEY;
}


int parser_flags(int argc, char **argv, ParserArguments *args, char *error, usize error_size) {

}


int parser_flags_(int argc, char *argv[], ParserArguments *arguments, char *error, usize error_size) {
    int option_mask = 0;
    int positional_count = 0;
    int options_ended = 0;

    if (arguments == NULL || argc < 2 || argv == NULL) {
        __parser_error__(error, error_size, "a command is required");
        return 1;
    }

    memset(arguments, 0, sizeof(*arguments));
    arguments->port = DEFAULT_SERVICE_PORT;
    if (parser_parse_command(argv[1], &arguments->command) != 0) {
        __parser_error__(error, error_size, "unknown command");
        return 1;
    }

    for (int index = 2; index < argc; index++) {
        char *argument = argv[index];
        char *value = NULL;
        int option = 0;

        if (!options_ended && strcmp(argument, "--") == 0) {
            options_ended = 1;
            continue;
        }

        if (!options_ended && strncmp(argument, "--", 2) == 0) {
            char *equals = strchr(argument, '=');
            usize name_length = equals == NULL ? strlen(argument) : (usize)(equals - argument);

            if (name_length == strlen("--port") && strncmp(argument, "--port", name_length) == 0) {
                option = PARSER_OPTION_PORT;
            } else if (name_length == strlen("--key") && strncmp(argument, "--key", name_length) == 0) {
                option = PARSER_OPTION_KEY;
            } else if (name_length == strlen("--host") && strncmp(argument, "--host", name_length) == 0) {
                option = PARSER_OPTION_HOST;
            } else if (name_length == strlen("--proxy") && strncmp(argument, "--proxy", name_length) == 0) {
                option = PARSER_OPTION_PROXY;
            } else {
                __parser_error__(error, error_size, "unknown option");
                return 1;
            }

            if (!parser_option_allowed(arguments->command, option)) {
                __parser_error__(error, error_size, "option is not supported by this command");
                return 1;
            }
            if ((option_mask & option) != 0) {
                __parser_error__(error, error_size, "option specified more than once");
                return 1;
            }

            if (equals != NULL) {
                value = equals + 1;
            } else if (++index < argc && strcmp(argv[index], "--") != 0) {
                value = argv[index];
            }
            if (value == NULL || value[0] == '\0') {
                __parser_error__(error, error_size, "option requires a non-empty value");
                return 1;
            }

            option_mask |= option;
            if (option == PARSER_OPTION_PORT) {
                if (parser_set_port(arguments, value, error, error_size) != 0) {
                    return 1;
                }
            } else if (option == PARSER_OPTION_KEY) {
                arguments->key = value;
            } else if (option == PARSER_OPTION_HOST) {
                arguments->host = value;
            } else {
                arguments->proxy = value;
            }
            continue;
        }

        if (arguments->command == PARSER_COMMAND_RUN) {
            if ((option_mask & PARSER_OPTION_PORT) != 0 || positional_count++ != 0) {
                __parser_error__(error, error_size, "run accepts exactly one port");
                return 1;
            }
            if (parser_set_port(arguments, argument, error, error_size) != 0) {
                return 1;
            }
            continue;
        }
        if (arguments->command == PARSER_COMMAND_CONFIG) {
            __parser_error__(error, error_size, "this command does not accept a path");
            return 1;
        }
        if (positional_count++ != 0) {
            __parser_error__(error, error_size, "this command accepts exactly one path");
            return 1;
        }
        arguments->path = argument;
    }

    if (arguments->command != PARSER_COMMAND_RUN && arguments->command != PARSER_COMMAND_CONFIG && positional_count == 0) {
        __parser_error__(error, error_size, "this command requires a path");
        return 1;
    }
    return 0;
}



