#ifndef TCPER_PROTOCOL_H
#define TCPER_PROTOCOL_H

#include "../stdc/sha256.h"
#include "../stdc/socket.h"

enum {
    APP_COMMAND_SEND = 0x01,
    APP_COMMAND_LIST = 0x02,
    APP_COMMAND_DOWNLOAD = 0x03,
    APP_COMMAND_REMOVE = 0x04,
    APP_COMMAND_MKDIR = 0x08,
    APP_COMMAND_SEND_DIRECTORY = 0x09,
    HANDSHAKE_OK = 0x05,
    HANDSHAKE_ERROR_TOKEN = 0x06,
    HANDSHAKE_ERROR_PATH = 0x07,
    HANDSHAKE_ERROR_MODE = 0x0A,
    HANDSHAKE_SECURE = 0x0B,
    HANDSHAKE_PLAINTEXT = 0x0C,
    HANDSHAKE_AUTHENTICATED = 0x0D,
    APP_RESPONSE_OK = 0x10,
    APP_RESPONSE_ERROR = 0x11,
    APP_LIST_ENTRY = 0x20,
    APP_LIST_COMPLETE = 0x21,
    APP_LIST_NOT_FOUND = 0x22,
    APP_LIST_INTERRUPTED = 0x23,
    APP_DIRECTORY_END = 0x30,
    APP_DIRECTORY_ENTRY = 0x31,
    APP_DIRECTORY_FILE = 0x32
};

#define PATH_LENGTH_RESTRICTION 1024
#define DIRECTORY_DOWNLOAD_MARKER (UINT64_MAX - 1ULL)
#define TRANSFER_BUSY_MARKER UINT64_MAX
#define TRANSFER_QUOTA_MARKER (UINT64_MAX - 2ULL)
#define TRANSFER_HASHING_MARKER (UINT64_MAX - 3ULL)
#define TRANSFER_ERROR_MARKER (UINT64_MAX - 4ULL)
#define DOWNLOAD_HASHING 0xA3
#define DOWNLOAD_PAYLOAD_READY 0xA4
#define DOWNLOAD_CHECKSUM_READY 0xA5

#define PROTOCOL_PLAINTEXT_MAGIC "TCPer-plain-v3"
#define PROTOCOL_SECURE_MAGIC "TCPer-secure-v3"
#define PROTOCOL_KEY_MIN_LENGTH 16
#define PROTOCOL_KEY_MAX_LENGTH 1024
#define PROTOCOL_NONCE_SIZE 32
#define PROTOCOL_RECORD_MAX_SIZE 65536
#define PROTOCOL_FILE_HASH_LENGTH SHA256_HEX_LENGTH

#if defined(__OS_UNIX__)
    typedef struct __attribute__((packed)) TransportProtocolHeader {
        byte command;
        uint64 filesize;
        char sha256[PROTOCOL_FILE_HASH_LENGTH + 1];
        char mode[65];
        char filename[PATH_LENGTH_RESTRICTION];
    } TransportProtocolHeader;
#elif defined(__OS_WINDOWS__)
    #pragma pack(push, 1)
    typedef struct TransportProtocolHeader {
        byte command;
        uint64 filesize;
        char sha256[PROTOCOL_FILE_HASH_LENGTH + 1];
        char mode[65];
        char filename[PATH_LENGTH_RESTRICTION];
    } TransportProtocolHeader;
    #pragma pack(pop)
#endif

typedef struct {
    Socket socket;
    bool secure;
    byte send_encryption_key[SHA256_DIGEST_SIZE];
    byte send_mac_key[SHA256_DIGEST_SIZE];
    byte receive_encryption_key[SHA256_DIGEST_SIZE];
    byte receive_mac_key[SHA256_DIGEST_SIZE];
    uint64 send_sequence;
    uint64 receive_sequence;
    byte receive_buffer[PROTOCOL_RECORD_MAX_SIZE];
    usize receive_offset;
    usize receive_length;
} ProtocolConnection;

void protocol_connection_init(ProtocolConnection *connection, Socket socket);

void protocol_connection_clear(ProtocolConnection *connection);

bool protocol_key_valid(const char *key);

bool protocol_client_authenticate(ProtocolConnection *connection,
                                  TransportProtocolHeader *header, const char *key);

bool protocol_server_authenticate(ProtocolConnection *connection,
                                  TransportProtocolHeader *header, const char *key,
                                  const char *server_ipv4, int server_port,
                                  double deadline);

int protocol_send_all(ProtocolConnection *connection, const void *buffer, usize length);

int protocol_recv_all(ProtocolConnection *connection, void *buffer, usize length);

long protocol_recv(ProtocolConnection *connection, void *buffer, usize length);

int protocol_shutdown(ProtocolConnection *connection);

#endif
