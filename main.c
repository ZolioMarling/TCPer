#include "./app/usage.h"
#include "./app/parser.h"
#include "./app/protocol.h"



int main(int argc, char *argv[], char *envs[]) {
    char username[256];
    parser_username(username, envs);

    char app_dir[PATH_LENGTH_RESTRICTION];
    os_getexec(app_dir, sizeof(app_dir));

    char config_path[PATH_LENGTH_RESTRICTION + sizeof("/.tcp_server_config")];
    snprintf(config_path, sizeof(config_path), "%s/.tcp_server_config", app_dir);

    if (argc < 2) {
        printf("\n");
        usage_logo(username);
        printf("\n");
        usage_help(argv[0], DEFAULT_SERVICE_PORT);
        return 1;
    }

    ParserArguments arguments;
    char parse_error[256];
    if (parser_flags(argc, argv, &arguments, parse_error, sizeof(parse_error)) != 0) {
        fprintf(stderr, "Invalid arguments: %s\n\n", parse_error);
        usage_help(argv[0], DEFAULT_SERVICE_PORT);
        return 1;
    }

    printf("command: %s\n", parser_command_name(arguments.command));
    printf("path: %s\n", arguments.path == NULL ? "(null)" : arguments.path);
    printf("port: %d\n", arguments.port);
    printf("key: %s\n", arguments.key == NULL ? "(null)" : arguments.key);
    printf("host: %s\n", arguments.host == NULL ? "(null)" : arguments.host);
    printf("proxy: %s\n", arguments.proxy == NULL ? "(null)" : arguments.proxy);

    return 0;
}
