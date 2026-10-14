#include "app.h"

static bool __client_connect__(Socket s, char *host_ipv4, int host_port, char *proxy_ipv4, int proxy_port) {
    const char *connect_ipv4 = proxy_ipv4 ? proxy_ipv4 : host_ipv4;
    int connect_port = proxy_ipv4 ? proxy_port : host_port;
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    socket_config(&server, AF_INET, (char *)connect_ipv4, connect_port);
    if (socket_connect_timeout(s, &server, sizeof(server), 3.7) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m is the %s \"%s:%d\" running?\n", proxy_ipv4 ? "proxy" : "server", connect_ipv4, connect_port);
        return False;
    }
    if (!proxy_ipv4) return True;

    const byte greeting[] = {0x05, 0x01, 0x00};
    byte response[260];
    if (!socket_send_all(s, (void *)greeting, sizeof(greeting)) ||
        !socket_recv_all(s, response, 2) || response[0] != 0x05 || response[1] != 0x00) return False;

    byte request[10] = {0x05, 0x01, 0x00, 0x01};
    struct in_addr remote_ipv4;
    if (inet_pton(AF_INET, host_ipv4, &remote_ipv4) != 1) return False;
    memcpy(&request[4], &remote_ipv4.s_addr, 4);
    unsigned short remote_port = socket_htons(host_port);
    memcpy(&request[8], &remote_port, 2);
    if (!socket_send_all(s, request, sizeof(request)) || !socket_recv_all(s, response, 4) ||
        response[0] != 0x05 || response[1] != 0x00 || response[2] != 0x00) return False;

    usize remaining;
    switch (response[3]) {
        case 0x01:
            remaining = 6;
            break;
        case 0x04:
            remaining = 18;
            break;
        case 0x03:
            if (!socket_recv_all(s, response, 1) || response[0] == 0) return False;
            remaining = (usize)response[0] + 2;
            break;
        default:
            return False;
    }
    return socket_recv_all(s, response, remaining);
}

static Socket __client_open__(ProtocolConnection *connection, char *host_ipv4,
                              int host_port, char *proxy_ipv4, int proxy_port) {
    if (socket_init() == SOCKET_INVALID) return SOCKET_INVALID;
    Socket socket = socket_create(AF_INET, SOCK_STREAM, 0);
    if (socket == SOCKET_INVALID) {
        socket_destroy();
        return SOCKET_INVALID;
    }

    int buffer_size = BUFFER_CAPACITY;
    bool ready = socket_setopt(socket, SOL_SOCKET, SO_SNDBUF, &buffer_size,
                               sizeof(buffer_size)) != SOCKET_INVALID &&
        socket_setopt(socket, SOL_SOCKET, SO_RCVBUF, &buffer_size,
                      sizeof(buffer_size)) != SOCKET_INVALID &&
        socket_setopt_timeout(socket, 0, CLIENT_REQUEST_TIMEOUT_SECONDS) != SOCKET_INVALID &&
        socket_setopt_timeout(socket, 1, CLIENT_REQUEST_TIMEOUT_SECONDS) != SOCKET_INVALID &&
        socket_setopt(socket, IPPROTO_TCP, TCP_NODELAY, NULL, 0) != SOCKET_INVALID &&
        __client_connect__(socket, host_ipv4, host_port, proxy_ipv4, proxy_port);
    if (!ready) {
        socket_close(socket);
        socket_destroy();
        return SOCKET_INVALID;
    }
    protocol_connection_init(connection, socket);
    if (proxy_ipv4)
        printf("\x1b[1;37;42m[INFO]\x1b[0m SOCKS5 tunnel %s:%d -> %s:%d established.\n",
               proxy_ipv4, proxy_port, host_ipv4, host_port);
    return socket;
}

static void __client_close__(ProtocolConnection *connection) {
    if (!connection || connection->socket == SOCKET_INVALID) return;
    Socket socket = connection->socket;
    protocol_connection_clear(connection);
    socket_close(socket);
    socket_destroy();
    connection->socket = SOCKET_INVALID;
}

typedef struct {
    char description[32];
    char speed[16];
    uint64 last_bytes;
    double last_time;
} _TransferProgress;

static void __client_show_progress__(_TransferProgress *progress, uint64 current,
                                     uint64 total) {
    double now = os_time();
    double interval = now - progress->last_time;
    if (interval >= 0.2) {
        parse_unit_convert((uint64)((current - progress->last_bytes) / interval),
                           progress->speed, sizeof(progress->speed));
        snprintf(progress->description, sizeof(progress->description), "%s/s", progress->speed);
        progress->last_time = now;
        progress->last_bytes = current;
    } else if (current == total && interval > 0) {
        parse_unit_convert((uint64)((current - progress->last_bytes) / interval),
                           progress->speed, sizeof(progress->speed));
        snprintf(progress->description, sizeof(progress->description), "%s/s", progress->speed);
    }
    if (interval >= 0.2 || current == total)
        usage_progress_bar((int64)current, (int64)total, 1, progress->description);
}

static bool __client_receive_response__(ProtocolConnection *connection) {
    byte status;
    if (protocol_recv_all(connection, &status, sizeof(status)) != 0) {
        printf("\x1b[1;37;43m[Warning]\x1b[0m receiving response status failed.\n");
        return False;
    }

    char output[4096];
    bool completed = False;
    while (!completed) {
        long n = protocol_recv(connection, output, sizeof(output));
        if (n <= 0) break;
        char *end = memchr(output, 0, (usize)n);
        usize printable = end ? (usize)(end - output) : (usize)n;
        if (printable > 0) fwrite(output, 1, printable, stdout);
        completed = end != NULL;
    }
    if (!completed) {
        printf("\x1b[1;37;43m[Warning]\x1b[0m receiving response text failed.\n");
        return False;
    }
    putchar('\n');
    return status == APP_RESPONSE_OK;
}

static bool __client_set_request_path__(TransportProtocolHeader *header, const char *path) {
    if (!header || !path || !path[0] || strlen(path) >= sizeof(header->filename)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m remote path is empty or exceeds %d bytes.\n",
               PATH_LENGTH_RESTRICTION - 1);
        return False;
    }
    memcpy(header->filename, path, strlen(path) + 1);
    return True;
}

static bool __client_prepare_source__(char *path, FILE **source, char hash[PROTOCOL_FILE_HASH_LENGTH + 1], uint64 *total_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("\x1b[1;37;41m[Error]\x1b[0m file \"%s\" open failed.\n", path);
        return False;
    }

    int64 source_size = os_filesize(path);
    if (source_size < 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m get the size of \"%s\" failed.\n", path);
        fclose(fp);
        return False;
    }

    sha256_file(fp, hash);
    if (ferror(fp) || os_fseek(fp, 0, SEEK_SET) != 0 || os_filesize(path) != source_size) {
        printf("\x1b[1;37;41m[Error]\x1b[0m source file changed or could not be read.\n");
        fclose(fp);
        return False;
    }

    *source = fp;
    *total_size = (uint64)source_size;
    return True;
}

static bool __client_send_item__(char *path, const char *remote_path, bool directory,
                                 char *host_ipv4, int host_port, char *proxy_ipv4,
                                 int proxy_port, const char *key) {
    FILE *fp = NULL;
    char hash[PROTOCOL_FILE_HASH_LENGTH + 1] = {0};
    uint64 total_size = 0;
    if (!directory && !__client_prepare_source__(path, &fp, hash, &total_size)) return False;

    ProtocolConnection connection;
    Socket s = __client_open__(&connection, host_ipv4, host_port, proxy_ipv4, proxy_port);
    if (s == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m failed to connect to the remote service.\n");
        if (fp) fclose(fp);
        return False;
    }

    bool success = False;

    TransportProtocolHeader meta;
    memset(&meta, 0, sizeof(meta));
    meta.command = directory ? APP_COMMAND_MKDIR : APP_COMMAND_SEND;
    snprintf(meta.sha256, sizeof(meta.sha256), "%s", hash);
    meta.sha256[sizeof(meta.sha256) - 1] = '\0';
    if (!__client_set_request_path__(&meta, remote_path)) goto release_client;

    meta.filesize = socket_htonll(total_size);

    if (!protocol_client_authenticate(&connection, &meta, key)) goto release_client;
    if (socket_setopt_timeout(s, 0, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID ||
        socket_setopt_timeout(s, 1, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket transfer timeout setting failed.\n");
        goto release_client;
    }
    if (directory) goto receive_response;

    uint64 offset;
    while (1) {
        uint64 network_offset;
        if (protocol_recv_all(&connection, &network_offset, sizeof(network_offset)) != 0) {
            printf("\x1b[1;37;41m[Error]\x1b[0m receiving file meta header offset failed.\n");
            goto release_client;
        }
        offset = socket_ntohll(network_offset);
        if (offset != TRANSFER_HASHING_MARKER) break;
    }
    if (offset == TRANSFER_BUSY_MARKER) {
        printf("\x1b[1;37;43m[Warning]\x1b[0m another upload is already writing \"%s\".\n", os_basename(path));
        goto release_client;
    }
    if (offset == TRANSFER_QUOTA_MARKER) {
        printf("\x1b[1;37;41m[Error]\x1b[0m upload checkpoint quota would be exceeded.\n");
        goto release_client;
    }
    if (offset == TRANSFER_ERROR_MARKER) {
        __client_receive_response__(&connection);
        goto release_client;
    }
    if (offset > total_size) {
        printf("\x1b[1;37;41m[Error]\x1b[0m server returned an invalid upload offset.\n");
        goto release_client;
    }
    if (socket_setopt_timeout(s, 1, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket receiver timeout setting failed.\n");
        goto release_client;
    }
    if (total_size > 0 && offset >= total_size) {
        printf("\x1b[1;37;42m[Success]\x1b[0m file \"%s\" already exists.\n", os_basename(path));
        success = True;
        goto release_client;
    }
    uint64 transmission_size = total_size - offset;
    if (offset > 0) {
        printf("\x1b[1;37;42m[INFO]\x1b[0m resumable transmission from \x1b[1;32m[%llu]\x1b[0m to \x1b[1;32m[%llu]\x1b[0m bytes.\n", offset, total_size);
        if (os_fseek(fp, (int64)offset, SEEK_SET) != 0) {
            printf("\x1b[1;37;41m[Error]\x1b[0m seeking source file for resume failed.\n");
            goto release_client;
        }
    }

    printf("\x1b[1;37;42m[INFO]\x1b[0m awaiting...\n");

    char buffer[BUFFER_CAPACITY];
    uint64 sent_bytes = 0;
    usize length;
    _TransferProgress progress = { .description = "1.000 GB/s", .last_time = os_time() };

    while (sent_bytes < transmission_size) {
        usize want = transmission_size - sent_bytes > sizeof(buffer) ? sizeof(buffer) : (usize)(transmission_size - sent_bytes);
        length = fread(buffer, 1, want, fp);
        if (length != want) {
            printf("\n\x1b[1;37;41m[Error]\x1b[0m source file changed or read failed during upload.\n");
            socket_abort(s);
            goto release_client;
        }
        if (protocol_send_all(&connection, buffer, length) != 0) {
            printf("\x1b[1;37;41m[Error]\x1b[0m remaining data sending failed.\n");
            goto release_client;
        }
        sent_bytes += (uint64)length;
        __client_show_progress__(&progress, sent_bytes, transmission_size);
    }

    if (fgetc(fp) != EOF || ferror(fp) || os_filesize(path) != (int64)total_size) {
        printf("\n\x1b[1;37;41m[Error]\x1b[0m source file changed or read failed during upload.\n");
        socket_abort(s);
        goto release_client;
    }
    receive_response:
        success = __client_receive_response__(&connection);

    release_client:
        if (fp) fclose(fp);
        __client_close__(&connection);
        return success;
}

typedef struct {
    ProtocolConnection *connection;
    const char *root;
    usize root_length;
    uint64 files;
    uint64 directories;
    uint64 bytes;
    bool failed;
} _DirectorySendContext;

static bool __client_send_directory_header__(_DirectorySendContext *ctx, byte type,
                                               const char *relative, uint64 size) {
    usize length = strlen(relative);
    if (length == 0 || length >= PATH_LENGTH_RESTRICTION || length > UINT16_MAX) return False;

    uint16 network_length = socket_htons((uint16)length);
    uint64 network_size = socket_htonll(size);
    return protocol_send_all(ctx->connection, &type, sizeof(type)) == 0 &&
        protocol_send_all(ctx->connection, &network_length, sizeof(network_length)) == 0 &&
        protocol_send_all(ctx->connection, &network_size, sizeof(network_size)) == 0 &&
        protocol_send_all(ctx->connection, relative, length) == 0;
}

static void __client_send_directory_entry__(char *dir, char *name, bool folder, uint64 listed_size, void *args) {
    (void)listed_size;
    _DirectorySendContext *ctx = (_DirectorySendContext *)args;
    if (ctx->failed) return;

    char local_path[PATH_LENGTH_RESTRICTION];
    int n = snprintf(local_path, sizeof(local_path), "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof(local_path) || (usize)n <= ctx->root_length + 1) {
        ctx->failed = True;
        return;
    }
    const char *relative = local_path + ctx->root_length + 1;

    if (folder) {
        if (!__client_send_directory_header__(ctx, APP_DIRECTORY_ENTRY, relative, 0)) {
            ctx->failed = True;
            return;
        }
        ctx->directories++;
        if (os_listdir(local_path, __client_send_directory_entry__, ctx) != 0) ctx->failed = True;
        return;
    }

    FILE *fp = fopen(local_path, "rb");
    if (!fp) {
        ctx->failed = True;
        return;
    }
    int64 source_size = os_file_size(fp);
    if (source_size < 0 || !__client_send_directory_header__(ctx, APP_DIRECTORY_FILE,
                                                             relative, (uint64)source_size)) {
        fclose(fp);
        ctx->failed = True;
        return;
    }

    SHA256Context sha256;
    sha256_init(&sha256);
    char buffer[BUFFER_CAPACITY];
    uint64 remaining = (uint64)source_size;
    while (remaining > 0) {
        usize want = remaining > sizeof(buffer) ? sizeof(buffer) : (usize)remaining;
        usize length = fread(buffer, 1, want, fp);
        if (length != want) {
            ctx->failed = True;
            break;
        }
        sha256_update(&sha256, (unsigned char *)buffer, (unsigned int)length);
        if (protocol_send_all(ctx->connection, buffer, length) != 0) {
            ctx->failed = True;
            break;
        }
        remaining -= length;
    }

    if (!ctx->failed && (fgetc(fp) != EOF || ferror(fp) || os_file_size(fp) != source_size))
        ctx->failed = True;

    byte digest[SHA256_DIGEST_SIZE];
    char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
    if (!ctx->failed) {
        sha256_final(&sha256, digest);
        app_hex_encode(digest, sizeof(digest), hash);
        if (protocol_send_all(ctx->connection, hash, PROTOCOL_FILE_HASH_LENGTH) != 0) ctx->failed = True;
    }
    if (fclose(fp) != 0) ctx->failed = True;
    if (!ctx->failed) {
        if (UINT64_MAX - ctx->bytes < (uint64)source_size) ctx->failed = True;
        else {
            ctx->files++;
            ctx->bytes += (uint64)source_size;
        }
    }
}

static bool __client_send_directory_stream__(char *local_dir, const char *remote_dir,
                                               char *host_ipv4, int host_port,
                                               char *proxy_ipv4, int proxy_port,
                                               const char *key) {
    ProtocolConnection connection;
    Socket s = __client_open__(&connection, host_ipv4, host_port, proxy_ipv4, proxy_port);
    if (s == SOCKET_INVALID) return False;

    bool success = False;

    TransportProtocolHeader meta;
    memset(&meta, 0, sizeof(meta));
    meta.command = APP_COMMAND_SEND_DIRECTORY;
    if (!__client_set_request_path__(&meta, remote_dir)) goto release_directory_client;
    if (!protocol_client_authenticate(&connection, &meta, key)) goto release_directory_client;
    if (socket_setopt_timeout(s, 0, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID ||
        socket_setopt_timeout(s, 1, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID)
        goto release_directory_client;

    _DirectorySendContext ctx = {
        .connection = &connection,
        .root = local_dir,
        .root_length = strlen(local_dir),
        .files = 0,
        .directories = 0,
        .bytes = 0,
        .failed = False
    };
    if (os_listdir(local_dir, __client_send_directory_entry__, &ctx) != 0) ctx.failed = True;
    if (ctx.failed) {
        socket_abort(s);
        printf("\n\x1b[1;37;41m[Error]\x1b[0m folder traversal or streaming failed after %llu file(s).\n", ctx.files);
        goto release_directory_client;
    }

    byte end = APP_DIRECTORY_END;
    if (protocol_send_all(&connection, &end, sizeof(end)) != 0) goto release_directory_client;
    success = __client_receive_response__(&connection);
    if (success) {
        printf("\x1b[1;37;42m[Success]\x1b[0m folder uploaded: %llu file(s), %llu directory(s), %llu bytes.\n",
               ctx.files, ctx.directories, ctx.bytes);
    }

    release_directory_client:
        __client_close__(&connection);
        return success;
}

bool app_client_send(char *path, char *host_ipv4, int host_port, char *proxy_ipv4,
                     int proxy_port, const char *key) {
    if (!os_isdir(path)) {
        if (!os_isfile(path)) {
            printf("\x1b[1;37;41m[Error]\x1b[0m \"%s\" is not a regular file or directory.\n", path);
            return False;
        }
        char *name = os_basename(path);
        if (!name) {
            printf("\x1b[1;37;41m[Error]\x1b[0m invalid file path \"%s\".\n", path);
            return False;
        }
        return __client_send_item__(path, name, False, host_ipv4, host_port, proxy_ipv4,
                                    proxy_port, key);
    }

    char normalized[PATH_LENGTH_RESTRICTION];
    int n = snprintf(normalized, sizeof(normalized), "%s", path);
    if (n < 0 || n >= (int)sizeof(normalized)) return False;
    while (n > 1 && (normalized[n - 1] == '/' || normalized[n - 1] == '\\')) normalized[--n] = 0;
    char *root = os_basename(normalized);
    if (!root || !root[0] || strcmp(root, ".") == 0 || strcmp(root, "..") == 0) root = "folder";
    return __client_send_directory_stream__(normalized, root, host_ipv4, host_port,
                                             proxy_ipv4, proxy_port, key);
}

bool app_client_list(char *dir, char *host_ipv4, int host_port, char *proxy_ipv4,
                     int proxy_port, const char *key) {
    ProtocolConnection connection;
    Socket s = __client_open__(&connection, host_ipv4, host_port, proxy_ipv4, proxy_port);
    if (s == SOCKET_INVALID) return False;

    bool success = False;

    TransportProtocolHeader meta;
    memset(&meta, 0, sizeof(meta));
    meta.command = APP_COMMAND_LIST;
    if (!__client_set_request_path__(&meta, dir)) goto release_client_list;
    if (!protocol_client_authenticate(&connection, &meta, key)) goto release_client_list;

    while (1) {
        byte type;
        if (protocol_recv_all(&connection, &type, sizeof(type)) != 0) {
            printf("\x1b[1;37;43m[Warning]\x1b[0m directory listing connection was interrupted.\n");
            break;
        }
        if (type == APP_LIST_COMPLETE) {
            success = True;
            break;
        }
        if (type == APP_LIST_NOT_FOUND) {
            printf("No such file or directory.\n");
            break;
        }
        if (type == APP_LIST_INTERRUPTED) {
            printf("Directory listing interrupted.\n");
            break;
        }
        if (type != APP_LIST_ENTRY) {
            printf("\x1b[1;37;41m[Error]\x1b[0m invalid directory listing record.\n");
            break;
        }

        uint16 network_length;
        if (protocol_recv_all(&connection, &network_length, sizeof(network_length)) != 0) break;
        usize length = socket_ntohs(network_length);
        if (length == 0 || length >= PATH_LENGTH_RESTRICTION) break;
        char response[PATH_LENGTH_RESTRICTION];
        if (protocol_recv_all(&connection, response, length) != 0) break;
        fwrite(response, 1, length, stdout);
    }

    release_client_list:
        __client_close__(&connection);
        return success;
}

static bool __client_safe_relative_path__(const char *path) {
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\') return False;
    const char *segment = path;
    for (const char *p = path; ; p++) {
        if ((unsigned char)*p < 32 && *p != '\0') return False;
        if (*p == '/' || *p == '\\' || *p == '\0') {
            usize length = (usize)(p - segment);
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.')) return False;
            if (*p == '\0') return True;
            segment = p + 1;
        }
    }
}

static bool __client_checked_directory__(char *path) {
    if (os_mkdir(path) != 0) return False;
    #if defined(__OS_UNIX__)
        struct stat st;
        return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    #elif defined(__OS_WINDOWS__)
        DWORD attrs = GetFileAttributesA(path);
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
    #endif
}

static bool __client_make_directory__(char *path) {
    char work[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(work, sizeof(work), "%s", path);
    if (n < 0 || n >= (int)sizeof(work)) return False;
    for (char *p = work + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char separator = *p;
        *p = '\0';
        if (work[0] && !__client_checked_directory__(work)) return False;
        *p = separator;
    }
    return __client_checked_directory__(work);
}

static bool __client_make_parent_directory__(char *path) {
    char parent[PATH_LENGTH_RESTRICTION * 2];
    int n = snprintf(parent, sizeof(parent), "%s", path);
    if (n < 0 || n >= (int)sizeof(parent)) return False;
    char *slash = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) return True;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    return __client_make_directory__(parent);
}

static bool __client_acquire_transfer_lock__(PathLock *lock, const char *target_path) {
    char canonical[PATH_LENGTH_RESTRICTION * 2];
    if (os_canonical_path(canonical, sizeof(canonical), target_path) != 0) return False;

    char hash[PROTOCOL_FILE_HASH_LENGTH + 1];
    sha256_string(canonical, hash);
    char lock_path[PATH_LENGTH_RESTRICTION];
    if (os_transfer_lock_path(lock_path, sizeof(lock_path), hash) != 0) return False;
    return os_path_lock(lock, lock_path) == 0;
}

static bool __client_download_directory_stream__(ProtocolConnection *connection,
                                                 char *remote_file, char *save_path) {
    char remote_copy[PATH_LENGTH_RESTRICTION];
    int remote_length = snprintf(remote_copy, sizeof(remote_copy), "%s", remote_file);
    if (remote_length < 0 || remote_length >= (int)sizeof(remote_copy)) return False;
    while (remote_length > 1 && (remote_copy[remote_length - 1] == '/' || remote_copy[remote_length - 1] == '\\')) remote_copy[--remote_length] = '\0';
    char *base = os_basename(remote_copy);
    if (!base) return False;

    char destination[PATH_LENGTH_RESTRICTION];
    int n;
    usize save_length = strlen(save_path);
    if (strcmp(save_path, ".") == 0 || save_path[save_length - 1] == '/' || save_path[save_length - 1] == '\\') {
        n = snprintf(destination, sizeof(destination), "%s/%s", strcmp(save_path, ".") == 0 ? "." : save_path, base);
    } else n = snprintf(destination, sizeof(destination), "%s", save_path);
    if (n < 0 || n >= (int)sizeof(destination) ||
        !__client_make_parent_directory__(destination)) return False;

    PathLock destination_lock = {0};
    if (!__client_acquire_transfer_lock__(&destination_lock, destination)) return False;

    char staging[PATH_LENGTH_RESTRICTION * 2] = {0};
    char backup[PATH_LENGTH_RESTRICTION * 2] = {0};
    int staging_length = snprintf(staging, sizeof(staging), "%s.tcper-directory-downloading",
                                  destination);
    int backup_length = snprintf(backup, sizeof(backup), "%s.tcper-directory-backup",
                                 destination);
    if (staging_length < 0 || staging_length >= (int)sizeof(staging) ||
        backup_length < 0 || backup_length >= (int)sizeof(backup))
        goto release_directory_lock;

    int destination_exists = os_access(destination);
    int backup_exists = os_access(backup);
    if (destination_exists < 0 || backup_exists < 0) goto release_directory_lock;
    if (destination_exists == 0 && backup_exists == 1) {
        if (os_rename_replace(backup, destination) != 0) goto release_directory_lock;
        destination_exists = 1;
        backup_exists = 0;
    }
    if (destination_exists == 1 && backup_exists == 1 && os_remove(backup) != 0)
        goto release_directory_lock;
    if (os_access(staging) == 1 && os_remove(staging) != 0) goto release_directory_lock;
    if (!__client_make_directory__(staging)) goto release_directory_lock;

    uint64 files = 0;
    uint64 directories = 0;
    uint64 bytes = 0;
    while (1) {
        byte type;
        if (protocol_recv_all(connection, &type, sizeof(type)) != 0) goto release_directory_lock;
        if (type == 0) break;
        if (type != 1 && type != 2) goto release_directory_lock;

        uint16 network_length;
        uint64 network_size;
        if (protocol_recv_all(connection, &network_length, sizeof(network_length)) != 0 ||
            protocol_recv_all(connection, &network_size, sizeof(network_size)) != 0)
            goto release_directory_lock;
        usize length = socket_ntohs(network_length);
        uint64 size = socket_ntohll(network_size);
        if (length == 0 || length >= PATH_LENGTH_RESTRICTION) goto release_directory_lock;

        char relative[PATH_LENGTH_RESTRICTION];
        if (protocol_recv_all(connection, relative, length) != 0) goto release_directory_lock;
        relative[length] = '\0';
        if (!__client_safe_relative_path__(relative)) goto release_directory_lock;

        char local_path[PATH_LENGTH_RESTRICTION * 2];
        n = snprintf(local_path, sizeof(local_path), "%s/%s", staging, relative);
        if (n < 0 || n >= (int)sizeof(local_path)) goto release_directory_lock;
        if (type == 1) {
            if (!__client_make_directory__(local_path)) goto release_directory_lock;
            directories++;
            continue;
        }

        if (!__client_make_parent_directory__(local_path)) goto release_directory_lock;
        char temporary_path[PATH_LENGTH_RESTRICTION * 2 + 32];
        int temporary_length = snprintf(temporary_path, sizeof(temporary_path), "%s.tcper-downloading", local_path);
        if (temporary_length < 0 || temporary_length >= (int)sizeof(temporary_path)) goto release_directory_lock;

        remove(temporary_path);
        FILE *fp = fopen(temporary_path, "wb");
        if (!fp) goto release_directory_lock;

        SHA256Context sha256;
        sha256_init(&sha256);
        char buffer[BUFFER_CAPACITY];
        uint64 received = 0;
        bool ok = True;
        while (received < size) {
            usize want = size - received > sizeof(buffer) ? sizeof(buffer) : (usize)(size - received);
            if (protocol_recv_all(connection, buffer, want) != 0 ||
                fwrite(buffer, 1, want, fp) != want) {
                ok = False;
                break;
            }
            sha256_update(&sha256, (unsigned char *)buffer, (unsigned int)want);
            received += want;
        }
        int sync_status = os_file_sync(fp);
        int close_status = fclose(fp);
        if (sync_status != 0 || close_status != 0) ok = False;
        byte digest[SHA256_DIGEST_SIZE];
        char actual_hash[PROTOCOL_FILE_HASH_LENGTH + 1];
        char expected_hash[PROTOCOL_FILE_HASH_LENGTH + 1] = {0};
        if (ok && protocol_recv_all(connection, expected_hash, PROTOCOL_FILE_HASH_LENGTH) != 0) ok = False;
        sha256_final(&sha256, digest);
        app_hex_encode(digest, sizeof(digest), actual_hash);
        if (!ok || strcmp(actual_hash, expected_hash) != 0 || os_rename_replace(temporary_path, local_path) != 0) {
            remove(temporary_path);
            goto release_directory_lock;
        }
        files++;
        bytes += size;
    }
    destination_exists = os_access(destination);
    if (destination_exists < 0) goto release_directory_lock;
    if (destination_exists == 1 && os_rename_replace(destination, backup) != 0)
        goto release_directory_lock;
    if (os_rename_replace(staging, destination) != 0) {
        if (destination_exists == 1) os_rename_replace(backup, destination);
        goto release_directory_lock;
    }
    if (destination_exists == 1 && os_remove(backup) != 0)
        printf("\x1b[1;37;43m[Warning]\x1b[0m downloaded folder is complete, but its old backup could not be removed.\n");
    printf("\x1b[1;37;42m[Success]\x1b[0m downloaded folder \"%s\": %llu file(s), %llu directory(s), %llu bytes.\n", destination, files, directories, bytes);
    os_path_unlock(&destination_lock);
    return True;

    release_directory_lock:
        if (staging[0] && os_access(staging) == 1) os_remove(staging);
        os_path_unlock(&destination_lock);
        return False;
}

bool app_client_download(char *remote_file, char *save_path, char *host_ipv4, int host_port,
                         char *proxy_ipv4, int proxy_port, const char *key) {
    ProtocolConnection connection;
    Socket s = __client_open__(&connection, host_ipv4, host_port, proxy_ipv4, proxy_port);
    if (s == SOCKET_INVALID) return False;

    FILE *fp = NULL;
    PathLock download_lock = {0};
    bool success = False;

    TransportProtocolHeader meta;
    memset(&meta, 0, sizeof(meta));
    meta.command = APP_COMMAND_DOWNLOAD;
    if (!__client_set_request_path__(&meta, remote_file)) goto release_client;

    if (!protocol_client_authenticate(&connection, &meta, key)) goto release_client;

    uint64 network_total_size;
    if (protocol_recv_all(&connection, &network_total_size, sizeof(network_total_size)) != 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m receiving file size failed.\n");
        goto release_client;
    }

    uint64 total_size = socket_ntohll(network_total_size);
    if (total_size == UINT64_MAX) {
        printf("\x1b[1;37;41m[Error]\x1b[0m remote file \"%s\" not found or inaccessible.\n", remote_file);
        goto release_client;
    }
    if (total_size == DIRECTORY_DOWNLOAD_MARKER) {
        if (!save_path || !save_path[0]) {
            printf("\x1b[1;37;41m[Error]\x1b[0m empty save path.\n");
            goto release_client;
        }
        if (socket_setopt_timeout(s, 1, TRANSFER_IDLE_TIMEOUT_SECONDS) != SOCKET_INVALID) {
            success = __client_download_directory_stream__(&connection, remote_file, save_path);
        }
        if (!success) {
            printf("\x1b[1;37;41m[Error]\x1b[0m folder download failed.\n");
        }
        goto release_client;
    }

    if (!save_path || save_path[0] == '\0') {
        printf("\x1b[1;37;41m[Error]\x1b[0m empty save path.\n");
        goto release_client;
    }

    char actual_save_path[PATH_LENGTH_RESTRICTION];
    usize sp_len = strlen(save_path);
    int save_path_length;
    if (strcmp(save_path, ".") == 0 || save_path[sp_len - 1] == '/' || save_path[sp_len - 1] == '\\') {
        save_path_length = snprintf(actual_save_path, sizeof(actual_save_path), "%s/%s",
            strcmp(save_path, ".") == 0 ? "." : save_path, os_basename(remote_file));
    } else save_path_length = snprintf(actual_save_path, sizeof(actual_save_path), "%s", save_path);
    if (save_path_length < 0 || save_path_length >= (int)sizeof(actual_save_path)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m download destination path is too long.\n");
        goto release_client;
    }

    char temp_file[PATH_LENGTH_RESTRICTION * 2];
    int temp_file_length = snprintf(temp_file, sizeof(temp_file), "%s.tcper-downloading", actual_save_path);
    if (temp_file_length < 0 || temp_file_length >= (int)sizeof(temp_file)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m temporary download path is too long.\n");
        goto release_client;
    }

    if (!__client_acquire_transfer_lock__(&download_lock, actual_save_path)) {
        printf("\x1b[1;37;41m[Error]\x1b[0m another download is already writing \"%s\".\n", actual_save_path);
        goto release_client;
    }

    int64 temp_size = os_filesize(temp_file);

    if (temp_size > 0 && (uint64)temp_size > total_size) {
        remove(temp_file);
        temp_size = -1;
    }
    uint64 offset = (temp_size != -1) ? (uint64)temp_size : 0;
    uint64 network_offset = socket_htonll(offset);

    if (socket_setopt_timeout(s, 1, TRANSFER_IDLE_TIMEOUT_SECONDS) == SOCKET_INVALID) {
        printf("\x1b[1;37;41m[Error]\x1b[0m socket receiver timeout setting failed.\n");
        goto release_client;
    }

    if (protocol_send_all(&connection, &network_offset, sizeof(network_offset)) != 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m sending offset failed.\n");
        goto release_client;
    }

    if (offset > 0) {
        while (1) {
            byte status;
            if (protocol_recv_all(&connection, &status, sizeof(status)) != 0) {
                printf("\x1b[1;37;41m[Error]\x1b[0m waiting for download payload failed.\n");
                goto release_client;
            }
            if (status == DOWNLOAD_PAYLOAD_READY) break;
            if (status != DOWNLOAD_HASHING) {
                printf("\x1b[1;37;41m[Error]\x1b[0m invalid download preparation status.\n");
                goto release_client;
            }
        }
    }

    if (offset > 0 && offset < total_size) {
        printf("\x1b[1;37;42m[INFO]\x1b[0m resumable download from \x1b[1;32m[%llu]\x1b[0m to \x1b[1;32m[%llu]\x1b[0m bytes.\n", offset, total_size);
    } else {
        printf("\x1b[1;37;42m[INFO]\x1b[0m awaiting data...\n");
    }

    fp = fopen(temp_file, (os_access(temp_file) == 1) ? "r+b" : "wb");
    if (!fp) {
        printf("\x1b[1;37;41m[Error]\x1b[0m cannot open temporary file \"%s\" for writing.\n", temp_file);
        goto release_client;
    }
    if (os_fseek(fp, (int64)offset, SEEK_SET) != 0) {
        printf("\x1b[1;37;41m[Error]\x1b[0m seeking temporary download file failed.\n");
        goto release_client;
    }

    uint64 transmission_size = total_size - offset;
    uint64 received_bytes = 0;
    char buffer[BUFFER_CAPACITY];
    bool transfer_ok = True;

    _TransferProgress progress = { .description = "0.000 B/s", .last_time = os_time() };

    while (received_bytes < transmission_size) {
        usize want = (transmission_size - received_bytes > sizeof(buffer)) ? sizeof(buffer) : (usize)(transmission_size - received_bytes);
        if (protocol_recv_all(&connection, buffer, want) != 0) {
            printf("\n\x1b[1;37;41m[Error]\x1b[0m network disconnected or timeout during data streaming.\n");
            transfer_ok = False;
            break;
        }

        if (fwrite(buffer, 1, want, fp) != want) {
            printf("\n\x1b[1;37;41m[Error]\x1b[0m disk write failed. Out of space?\n");
            transfer_ok = False;
            break;
        }

        received_bytes += want;
        __client_show_progress__(&progress, received_bytes, transmission_size);
    }
    
    int flush_status = os_file_sync(fp);
    int close_status = fclose(fp);
    fp = NULL;
    if (flush_status != 0 || close_status != 0) {
        printf("\n\x1b[1;37;41m[Error]\x1b[0m failed to finalize the temporary file.\n");
        transfer_ok = False;
    }

    if (transfer_ok && (offset + received_bytes >= total_size)) {
        byte checksum_ready;
        if (protocol_recv_all(&connection, &checksum_ready, sizeof(checksum_ready)) != 0 ||
            checksum_ready != DOWNLOAD_CHECKSUM_READY) {
            printf("\n\x1b[1;37;41m[Error]\x1b[0m receiving checksum readiness failed.\n");
            goto release_client;
        }
        char hash[PROTOCOL_FILE_HASH_LENGTH + 1] = {0};
        if (protocol_recv_all(&connection, hash, PROTOCOL_FILE_HASH_LENGTH) != 0) {
            printf("\n\x1b[1;37;41m[Error]\x1b[0m receiving file SHA-256 failed.\n");
            goto release_client;
        }
        printf("\n\x1b[1;37;42m[INFO]\x1b[0m verifying SHA-256 checksum...\n");
        
        FILE *vfp = fopen(temp_file, "rb");
        if (vfp) {
            char local_hash[PROTOCOL_FILE_HASH_LENGTH + 1];
            sha256_file(vfp, local_hash);
            fclose(vfp);

            if (strcmp(local_hash, hash) == 0) {
                if (os_rename_replace(temp_file, actual_save_path) == 0) {
                    printf("\x1b[1;37;42m[Success]\x1b[0m downloaded successfully and saved to \"%s\".\n", actual_save_path);
                    success = True;
                } else {
                    printf("\x1b[1;37;41m[Error]\x1b[0m failed to rename temporary file to \"%s\".\n", actual_save_path);
                }
            } else {
                printf("\x1b[1;37;41m[Error]\x1b[0m SHA-256 mismatch! The file is corrupted.\n");
                printf(" - Expected: %s\n - Actual:   %s\n", hash, local_hash);
                remove(temp_file); 
            }
        } else printf("\x1b[1;37;41m[Error]\x1b[0m failed to reopen the temporary file for verification.\n");
    }

    release_client:
        if (fp) fclose(fp);
        os_path_unlock(&download_lock);
        __client_close__(&connection);
        return success;
}

bool app_client_remove(char *path, char *host_ipv4, int host_port, char *proxy_ipv4,
                       int proxy_port, const char *key) {
    ProtocolConnection connection;
    Socket s = __client_open__(&connection, host_ipv4, host_port, proxy_ipv4, proxy_port);
    if (s == SOCKET_INVALID) return False;

    bool success = False;

    TransportProtocolHeader meta;
    memset(&meta, 0, sizeof(meta));
    meta.command = APP_COMMAND_REMOVE;
    if (!__client_set_request_path__(&meta, path)) goto release_client_remove;

    if (!protocol_client_authenticate(&connection, &meta, key)) goto release_client_remove;

    success = __client_receive_response__(&connection);

    release_client_remove:
        __client_close__(&connection);
        return success;
}
