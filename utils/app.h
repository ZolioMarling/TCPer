#ifndef _APP_H_
#define _APP_H_

#include <signal.h>

#include "../stdc/socket.h"
#include "../stdc/threadpool.h"
#include "../stdc/async_log.h"
#include "../stdc/os.h"

#include "parse.h"
#include "protocol.h"
#include "usage.h"

#define DEFAULT_SERVICE_PORT 31415
#define BUFFER_CAPACITY 65536
#define HANDSHAKE_TIMEOUT_SECONDS 15.0
#define TRANSFER_IDLE_TIMEOUT_SECONDS 300.0
#define CLIENT_REQUEST_TIMEOUT_SECONDS 300.0
#ifndef HASH_PROGRESS_INTERVAL_SECONDS
    #define HASH_PROGRESS_INTERVAL_SECONDS 1.0
#endif
#ifndef UPLOAD_CHECKPOINT_MAX_FILES
    #define UPLOAD_CHECKPOINT_MAX_FILES 256ULL
#endif
#ifndef UPLOAD_CHECKPOINT_MAX_BYTES
    #define UPLOAD_CHECKPOINT_MAX_BYTES (10ULL * 1024ULL * 1024ULL * 1024ULL)
#endif
#ifndef DIRECTORY_UPLOAD_MAX_FILES
    #define DIRECTORY_UPLOAD_MAX_FILES 10000ULL
#endif
#ifndef DIRECTORY_UPLOAD_MAX_DIRECTORIES
    #define DIRECTORY_UPLOAD_MAX_DIRECTORIES 10000ULL
#endif
#ifndef DIRECTORY_UPLOAD_MAX_BYTES
    #define DIRECTORY_UPLOAD_MAX_BYTES (10ULL * 1024ULL * 1024ULL * 1024ULL)
#endif
#ifndef STORAGE_MAX_FILES
    #define STORAGE_MAX_FILES 100000ULL
#endif
#ifndef STORAGE_MAX_BYTES
    #define STORAGE_MAX_BYTES (100ULL * 1024ULL * 1024ULL * 1024ULL)
#endif

static inline void app_hex_encode(const byte *input, usize length, char *output) {
    static const char digits[] = "0123456789abcdef";
    for (usize i = 0; i < length; i++) {
        output[i * 2] = digits[input[i] >> 4];
        output[i * 2 + 1] = digits[input[i] & 0x0f];
    }
    output[length * 2] = '\0';
}

static inline void app_start_tips(char *ipv4, int port) {
    printf(" * \x1b[1;32mINFO:\x1b[0m started server process ID [\x1b[1;36m%d\x1b[0m]\n", os_getpid());
    printf(" * \x1b[1;32mINFO:\x1b[0m waiting for application startup.\n");
    printf(" * \x1b[1;32mINFO:\x1b[0m application startup complete.\n");
    printf(" * \x1b[1;32mINFO:\x1b[0m C socket TCP service is running on \x1b[1;36m%s:%d\x1b[0m (press \x1b[1;33mCTRL+C\x1b[0m to quit)\n", ipv4, port);
}

static inline void app_quit_tips(void) {
    printf(" * \x1b[1;32mINFO:\x1b[0m shutting down.\n");
    printf(" * \x1b[1;32mINFO:\x1b[0m waiting for application shutdown.\n");
    printf(" * \x1b[1;32mINFO:\x1b[0m application shutdown complete.\n");
    printf(" * \x1b[1;32mINFO:\x1b[0m finished server process ID [\x1b[1;36m%d\x1b[0m]\n", os_getpid());
}

void app_interrupt(int semaphore);

bool app_handshake(ProtocolConnection *connection, TransportProtocolHeader *meta,
                   const char *key, double deadline);

bool app_run_service(int port, const char *key);

void app_thread_task(void *args);

void app_server_send(ProtocolConnection *connection, TransportProtocolHeader *meta);

void app_server_list(ProtocolConnection *connection, TransportProtocolHeader *meta);

void app_server_download(ProtocolConnection *connection, TransportProtocolHeader *meta);

void app_server_remove(ProtocolConnection *connection, TransportProtocolHeader *meta);

void app_server_mkdir(ProtocolConnection *connection, TransportProtocolHeader *meta);

void app_server_send_directory(ProtocolConnection *connection, TransportProtocolHeader *meta);

void app_server_default(ProtocolConnection *connection);

bool app_client_send(char *path, char *host_ipv4, int host_port, char *proxy_ipv4,
                     int proxy_port, const char *key);

bool app_client_list(char *dir, char *host_ipv4, int host_port, char *proxy_ipv4,
                     int proxy_port, const char *key);

bool app_client_download(char *remote_file, char *save_path, char *host_ipv4, int host_port,
                         char *proxy_ipv4, int proxy_port, const char *key);

bool app_client_remove(char *path, char *host_ipv4, int host_port, char *proxy_ipv4,
                       int proxy_port, const char *key);

void __app_server_list__(char *dir, char *name, bool folder, uint64 size, void *args);

#endif
