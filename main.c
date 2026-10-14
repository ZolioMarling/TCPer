#include "./utils/app.h"

typedef struct {
    char host_ipv4[32];
    int host_port;
    char proxy_ipv4[32];
    int proxy_port;
} ClientEndpoint;

typedef enum {
    CLIENT_SEND,
    CLIENT_LIST,
    CLIENT_DOWNLOAD,
    CLIENT_REMOVE
} ClientOperation;

static void __thread_lock_callback__(int lock, void *ctx) {
    Mutex *mutex = (Mutex *)ctx;
    if (lock) mutex_lock(mutex);
    else mutex_unlock(mutex);
}

static const char *__environment_value__(char *envs[], const char *name) {
    usize name_length = strlen(name);
    for (int i = 0; envs && envs[i]; i++) {
        if (strncmp(envs[i], name, name_length) == 0 && envs[i][name_length] == '=')
            return envs[i] + name_length + 1;
    }
    return NULL;
}

static bool __resolve_key__(const char *argument, char *envs[], const char **key) {
    const char *value = __environment_value__(envs, "TCPER_KEY");
    if (argument) {
        static const char prefix[] = "--key=";
        if (strncmp(argument, prefix, sizeof(prefix) - 1) != 0) return False;
        value = argument + sizeof(prefix) - 1;
        if (!value[0]) return False;
    }
    if (!value || !value[0]) value = NULL;
    if (!protocol_key_valid(value)) {
        fprintf(stderr,
                "\x1b[1;37;41m[Error]\x1b[0m key must contain 16 to %d printable bytes.\n",
                PROTOCOL_KEY_MAX_LENGTH);
        return False;
    }
    *key = value;
    return True;
}

static int __run_server__(int port, const char *key) {
    usage_logo();
    printf(" * \x1b[1;32mINFO:\x1b[0m transport mode: %s.\n",
           key ? "authenticated encryption" : "plaintext (no key configured)");
    Mutex mutex;
    if (mutex_create(&mutex, 1) != 0) {
        fprintf(stderr, "\x1b[1;37;41m[Error]\x1b[0m failed to initialize logging lock.\n");
        return 1;
    }
    async_log_config_thread_lock(__thread_lock_callback__, &mutex);
    async_log_init(8, 32);

    FILE *log_file = fopen("app.log", "a");
    if (log_file) {
        async_log_config_write(log_file);
        async_log_setting(1);
    } else {
        async_log_setting(0);
        fprintf(stderr, "\x1b[1;37;43m[Warning]\x1b[0m failed to open app.log; using stderr.\n");
    }

    bool service_ok = app_run_service(port, key);
    async_log_exit(1);
    mutex_destroy(&mutex);
    if (log_file) fclose(log_file);
    app_quit_tips();
    return service_ok ? 0 : 1;
}

static bool __load_client_endpoint__(const char *config_path, ClientEndpoint *endpoint) {
    memset(endpoint, 0, sizeof(*endpoint));
    if (os_access((char *)config_path) == 1) {
        if (parse_config_file((char *)config_path, endpoint->host_ipv4,
                              &endpoint->host_port, endpoint->proxy_ipv4,
                              &endpoint->proxy_port) != 0) {
            fprintf(stderr, "\x1b[1;37;41m[Error]\x1b[0m invalid configuration file %s.\n",
                    config_path);
            return False;
        }
    } else {
        socket_ipv4(endpoint->host_ipv4, sizeof(endpoint->host_ipv4));
        if (!socket_valid_ipv4(endpoint->host_ipv4))
            snprintf(endpoint->host_ipv4, sizeof(endpoint->host_ipv4), "127.0.0.1");
        endpoint->host_port = DEFAULT_SERVICE_PORT;
        printf("\x1b[1;37;43m[Warning]\x1b[0m no client configuration; using %s:%d.\n",
               endpoint->host_ipv4, endpoint->host_port);
    }

    if (!socket_valid_ipv4(endpoint->host_ipv4) || endpoint->host_port <= 0 ||
        endpoint->host_port > 65535) return False;
    if (endpoint->proxy_ipv4[0] &&
        (!socket_valid_ipv4(endpoint->proxy_ipv4) || endpoint->proxy_port <= 0 ||
         endpoint->proxy_port > 65535)) return False;
    return True;
}

static int __run_client__(ClientOperation operation, char *path, const char *config_path,
                          const char *key) {
    ClientEndpoint endpoint;
    if (!__load_client_endpoint__(config_path, &endpoint)) return 1;
    char *proxy = endpoint.proxy_ipv4[0] ? endpoint.proxy_ipv4 : NULL;
    bool success = False;
    switch (operation) {
        case CLIENT_SEND:
            if (os_access(path) != 1) {
                fprintf(stderr, "\x1b[1;37;41m[Error]\x1b[0m no such file or directory: %s\n",
                        path);
                return 1;
            }
            success = app_client_send(path, endpoint.host_ipv4, endpoint.host_port,
                                      proxy, endpoint.proxy_port, key);
            break;
        case CLIENT_LIST:
            success = app_client_list(path, endpoint.host_ipv4, endpoint.host_port,
                                      proxy, endpoint.proxy_port, key);
            break;
        case CLIENT_DOWNLOAD:
            success = app_client_download(path, ".", endpoint.host_ipv4,
                                          endpoint.host_port, proxy,
                                          endpoint.proxy_port, key);
            break;
        case CLIENT_REMOVE:
            success = app_client_remove(path, endpoint.host_ipv4, endpoint.host_port,
                                        proxy, endpoint.proxy_port, key);
            break;
    }
    return success ? 0 : 1;
}

static int __configure_client__(int argc, char *argv[], const char *config_path,
                                const char *app_dir) {
    char host_ipv4[32];
    char proxy_ipv4[32];
    int host_port;
    int proxy_port;
    if (argc == 2) {
        if (parse_config_file((char *)config_path, host_ipv4, &host_port, proxy_ipv4,
                              &proxy_port) != 0) {
            fprintf(stderr, "\x1b[1;37;41m[Error]\x1b[0m no valid client configuration.\n");
            return 1;
        }
        printf("host ipv4 = %s\nhost port = %d\nproxy ipv4 = %s\nproxy port = %d\n",
               host_ipv4, host_port, proxy_ipv4, proxy_port);
        return 0;
    }
    if ((argc != 3 && argc != 4) ||
        parse_config_cmd(argv[2], argc == 4 ? argv[3] : NULL, host_ipv4, &host_port,
                         proxy_ipv4, &proxy_port) != 0 ||
        parse_config_write((char *)config_path, host_ipv4, host_port, proxy_ipv4,
                           proxy_port) != 0) {
        fprintf(stderr, "\x1b[1;37;41m[Error]\x1b[0m invalid client configuration.\n");
        return 1;
    }
    printf("\x1b[1;37;42m[Success]\x1b[0m configuration written to %s.\n", app_dir);
    return 0;
}

int main(int argc, char *argv[], char *envs[]) {
    char username[256];
    char app_dir[PATH_LENGTH_RESTRICTION];
    char config_path[PATH_LENGTH_RESTRICTION + sizeof("/.tcp-server-config")];
    parse_username(username, envs);

    os_getexec(app_dir, sizeof(app_dir));
    snprintf(config_path, sizeof(config_path), "%s/.tcp-server-config", app_dir);


    if (argc < 2) {
        usage_logo();
        usage_help(username, argv[0], DEFAULT_SERVICE_PORT);
        return 1;
    }

    if (strcmp(argv[1], "run") == 0) {
        int port = DEFAULT_SERVICE_PORT;
        const char *key_argument = NULL;
        if (argc >= 3 && strncmp(argv[2], "--key=", strlen("--key=")) != 0) {
            port = parse_port(argv[2]);
            if (!port) return 1;
            if (argc == 4) key_argument = argv[3];
            else if (argc > 4) return 1;
        } else if (argc == 3) key_argument = argv[2];
        else if (argc > 3) return 1;
        const char *key;
        if (!__resolve_key__(key_argument, envs, &key)) return 1;
        return __run_server__(port, key);
    }

    if (strcmp(argv[1], "config") == 0)
        return __configure_client__(argc, argv, config_path, app_dir);

    ClientOperation operation;
    if (strcmp(argv[1], "send") == 0) operation = CLIENT_SEND;
    else if (strcmp(argv[1], "list") == 0) operation = CLIENT_LIST;
    else if (strcmp(argv[1], "download") == 0) operation = CLIENT_DOWNLOAD;
    else if (strcmp(argv[1], "remove") == 0) operation = CLIENT_REMOVE;
    else {
        usage_logo();
        usage_help(username, argv[0], DEFAULT_SERVICE_PORT);
        return 1;
    }

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s %s <path> [--key=KEY]\n", argv[0], argv[1]);
        return 1;
    }
    const char *key;
    if (!__resolve_key__(argc == 4 ? argv[3] : NULL, envs, &key)) return 1;
    return __run_client__(operation, argv[2], config_path, key);
}
