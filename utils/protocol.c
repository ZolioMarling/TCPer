#include "protocol.h"

#include "../stdc/async_log.h"

#include <string.h>

#define PROTOCOL_RECORD_HEADER_SIZE 12

static void __write_uint32__(byte output[4], uint32 value) {
    output[0] = (byte)(value >> 24);
    output[1] = (byte)(value >> 16);
    output[2] = (byte)(value >> 8);
    output[3] = (byte)value;
}

static uint32 __read_uint32__(const byte input[4]) {
    return ((uint32)input[0] << 24) | ((uint32)input[1] << 16) |
        ((uint32)input[2] << 8) | (uint32)input[3];
}

static void __write_uint64__(byte output[8], uint64 value) {
    for (unsigned int i = 0; i < 8; i++) output[7 - i] = (byte)(value >> (i * 8));
}

static uint64 __read_uint64__(const byte input[8]) {
    uint64 value = 0;
    for (unsigned int i = 0; i < 8; i++) value = (value << 8) | input[i];
    return value;
}

static void __proof__(const char *key, const char *label,
                      const TransportProtocolHeader *header,
                      const byte server_nonce[PROTOCOL_NONCE_SIZE],
                      const byte client_nonce[PROTOCOL_NONCE_SIZE], byte output[32]) {
    HMAC256Context context;
    hmac_sha256_init(&context, (void *)key, strlen(key));
    hmac_sha256_update(&context, (void *)label, strlen(label));
    hmac_sha256_update(&context, (void *)header, sizeof(*header));
    hmac_sha256_update(&context, (void *)server_nonce, PROTOCOL_NONCE_SIZE);
    hmac_sha256_update(&context, (void *)client_nonce, PROTOCOL_NONCE_SIZE);
    hmac_sha256_final(&context, output);
}

static void __transcript_hash__(const TransportProtocolHeader *header,
                                const byte server_nonce[PROTOCOL_NONCE_SIZE],
                                const byte client_nonce[PROTOCOL_NONCE_SIZE], byte output[32]) {
    SHA256Context context;
    sha256_init(&context);
    sha256_update(&context, "TCPer-session-v3", strlen("TCPer-session-v3"));
    sha256_update(&context, (void *)header, sizeof(*header));
    sha256_update(&context, (void *)server_nonce, PROTOCOL_NONCE_SIZE);
    sha256_update(&context, (void *)client_nonce, PROTOCOL_NONCE_SIZE);
    sha256_final(&context, output);
}

static void __derive_key__(const char *key, const char *label,
                           const byte transcript_hash[32], byte output[32]) {
    HMAC256Context context;
    hmac_sha256_init(&context, (void *)key, strlen(key));
    hmac_sha256_update(&context, "TCPer-KDF-v3", strlen("TCPer-KDF-v3"));
    hmac_sha256_update(&context, (void *)label, strlen(label));
    hmac_sha256_update(&context, (void *)transcript_hash, 32);
    hmac_sha256_final(&context, output);
}

static void __initialize_secure_connection__(ProtocolConnection *connection, const char *key,
                                             const TransportProtocolHeader *header,
                                             const byte server_nonce[PROTOCOL_NONCE_SIZE],
                                             const byte client_nonce[PROTOCOL_NONCE_SIZE],
                                             bool client) {
    byte transcript_hash[32];
    byte client_encryption[32];
    byte client_mac[32];
    byte server_encryption[32];
    byte server_mac[32];
    __transcript_hash__(header, server_nonce, client_nonce, transcript_hash);
    __derive_key__(key, "client encryption", transcript_hash, client_encryption);
    __derive_key__(key, "client authentication", transcript_hash, client_mac);
    __derive_key__(key, "server encryption", transcript_hash, server_encryption);
    __derive_key__(key, "server authentication", transcript_hash, server_mac);

    if (client) {
        memcpy(connection->send_encryption_key, client_encryption, 32);
        memcpy(connection->send_mac_key, client_mac, 32);
        memcpy(connection->receive_encryption_key, server_encryption, 32);
        memcpy(connection->receive_mac_key, server_mac, 32);
    } else {
        memcpy(connection->send_encryption_key, server_encryption, 32);
        memcpy(connection->send_mac_key, server_mac, 32);
        memcpy(connection->receive_encryption_key, client_encryption, 32);
        memcpy(connection->receive_mac_key, client_mac, 32);
    }
    connection->secure = True;
    secure_memzero(transcript_hash, sizeof(transcript_hash));
    secure_memzero(client_encryption, sizeof(client_encryption));
    secure_memzero(client_mac, sizeof(client_mac));
    secure_memzero(server_encryption, sizeof(server_encryption));
    secure_memzero(server_mac, sizeof(server_mac));
}

static void __xor_record__(const byte key[32], uint64 sequence, byte *data, usize length) {
    byte input[sizeof("TCPer-record-v3") - 1 + 16];
    memcpy(input, "TCPer-record-v3", sizeof("TCPer-record-v3") - 1);
    __write_uint64__(input + sizeof("TCPer-record-v3") - 1, sequence);
    uint64 block = 0;
    usize offset = 0;
    while (offset < length) {
        __write_uint64__(input + sizeof("TCPer-record-v3") - 1 + 8, block++);
        byte stream[32];
        hmac_sha256((void *)key, 32, input, sizeof(input), stream);
        usize count = length - offset > sizeof(stream) ? sizeof(stream) : length - offset;
        for (usize i = 0; i < count; i++) data[offset + i] ^= stream[i];
        secure_memzero(stream, sizeof(stream));
        offset += count;
    }
    secure_memzero(input, sizeof(input));
}

static void __record_tag__(const byte key[32], const byte header[PROTOCOL_RECORD_HEADER_SIZE],
                           const byte *ciphertext, usize length, byte output[32]) {
    HMAC256Context context;
    hmac_sha256_init(&context, (void *)key, 32);
    hmac_sha256_update(&context, "TCPer-record-tag-v3", strlen("TCPer-record-tag-v3"));
    hmac_sha256_update(&context, (void *)header, PROTOCOL_RECORD_HEADER_SIZE);
    hmac_sha256_update(&context, (void *)ciphertext, length);
    hmac_sha256_final(&context, output);
}

static bool __receive_record__(ProtocolConnection *connection) {
    if (connection->receive_sequence == UINT64_MAX) return False;
    byte header[PROTOCOL_RECORD_HEADER_SIZE];
    if (!socket_recv_all(connection->socket, header, sizeof(header))) return False;
    uint32 length = __read_uint32__(header);
    uint64 sequence = __read_uint64__(header + 4);
    if (length == 0 || length > PROTOCOL_RECORD_MAX_SIZE ||
        sequence != connection->receive_sequence) return False;
    if (!socket_recv_all(connection->socket, connection->receive_buffer, length))
        return False;
    byte received_tag[32];
    byte expected_tag[32];
    if (!socket_recv_all(connection->socket, received_tag, sizeof(received_tag)))
        return False;
    __record_tag__(connection->receive_mac_key, header, connection->receive_buffer,
                   length, expected_tag);
    bool valid = secure_memequal(received_tag, expected_tag, sizeof(expected_tag));
    secure_memzero(received_tag, sizeof(received_tag));
    secure_memzero(expected_tag, sizeof(expected_tag));
    if (!valid) return False;
    __xor_record__(connection->receive_encryption_key, sequence,
                   connection->receive_buffer, length);
    connection->receive_sequence++;
    connection->receive_offset = 0;
    connection->receive_length = length;
    return True;
}

static void __send_http_response__(Socket socket, bool head, const char *server_ipv4,
                                   int server_port) {
    char body[1280];
    int body_length = snprintf(
        body, sizeof(body),
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>TCPer</title></head><body><h1>TCPer Service</h1><p>The service is running.</p><p>Socket: <strong>%s:%d</strong></p></body></html>",
        server_ipv4, server_port);
    if (body_length < 0 || body_length >= (int)sizeof(body)) return;
    char response[512];
    int response_length = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nServer: TCPer/2.0\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        body_length);
    if (response_length < 0 || response_length >= (int)sizeof(response)) return;
    if (socket_send_all(socket, response, (usize)response_length) && !head)
        socket_send_all(socket, body, (usize)body_length);
    socket_shutdown(socket);
}

static bool __receive_http_headers__(Socket socket, const byte prefix[5], double deadline) {
    byte tail[4] = {prefix[1], prefix[2], prefix[3], prefix[4]};
    for (usize length = 5; length < 8192; length++) {
        if (memcmp(tail, "\r\n\r\n", sizeof(tail)) == 0) return True;
        byte next;
        if (!socket_recv_all_deadline(socket, &next, sizeof(next), deadline)) return False;
        memmove(tail, tail + 1, sizeof(tail) - 1);
        tail[sizeof(tail) - 1] = next;
    }
    return memcmp(tail, "\r\n\r\n", sizeof(tail)) == 0;
}

static bool __receive_protocol_header__(Socket socket, TransportProtocolHeader *header,
                                        const char *server_ipv4, int server_port,
                                        double deadline) {
    byte prefix[5];
    if (!socket_recv_all_deadline(socket, prefix, sizeof(prefix), deadline)) return False;
    if (memcmp(prefix, "GET ", 4) == 0) {
        if (__receive_http_headers__(socket, prefix, deadline))
            __send_http_response__(socket, False, server_ipv4, server_port);
        return False;
    }
    if (memcmp(prefix, "HEAD ", 5) == 0) {
        if (__receive_http_headers__(socket, prefix, deadline))
            __send_http_response__(socket, True, server_ipv4, server_port);
        return False;
    }
    memcpy(header, prefix, sizeof(prefix));
    return socket_recv_all_deadline(socket, (byte *)header + sizeof(prefix),
                                    sizeof(*header) - sizeof(prefix), deadline);
}

static void __print_handshake_error__(byte status) {
    if (status == HANDSHAKE_ERROR_TOKEN)
        printf("\x1b[1;37;41m[Error]\x1b[0m invalid authentication key.\n");
    else if (status == HANDSHAKE_ERROR_MODE)
        printf("\x1b[1;37;41m[Error]\x1b[0m client/server encryption modes do not match.\n");
    else if (status == HANDSHAKE_ERROR_PATH)
        printf("\x1b[1;37;41m[Error]\x1b[0m unsafe remote path.\n");
    else printf("\x1b[1;37;41m[Error]\x1b[0m handshake failed (0x%02x).\n", status);
}

void protocol_connection_init(ProtocolConnection *connection, Socket socket) {
    memset(connection, 0, sizeof(*connection));
    connection->socket = socket;
}

void protocol_connection_clear(ProtocolConnection *connection) {
    if (!connection) return;
    Socket socket = connection->socket;
    secure_memzero(connection, sizeof(*connection));
    connection->socket = socket;
}

bool protocol_key_valid(const char *key) {
    if (!key || !key[0]) return True;
    usize length = strlen(key);
    if (length < PROTOCOL_KEY_MIN_LENGTH || length > PROTOCOL_KEY_MAX_LENGTH) return False;
    for (usize i = 0; i < length; i++) {
        unsigned char character = (unsigned char)key[i];
        if (character < 32 || character == 127) return False;
    }
    return True;
}

bool protocol_client_authenticate(ProtocolConnection *connection,
                                  TransportProtocolHeader *header, const char *key) {
    bool secure = key && key[0];
    snprintf(header->mode, sizeof(header->mode), "%s",
             secure ? PROTOCOL_SECURE_MAGIC : PROTOCOL_PLAINTEXT_MAGIC);
    if (!socket_send_all(connection->socket, header, sizeof(*header))) return False;

    byte status;
    if (!socket_recv_all(connection->socket, &status, sizeof(status))) return False;
    if (!secure) {
        if (status != HANDSHAKE_PLAINTEXT) {
            __print_handshake_error__(status);
            return False;
        }
    } else {
        if (status != HANDSHAKE_SECURE) {
            __print_handshake_error__(status);
            return False;
        }
        byte server_nonce[PROTOCOL_NONCE_SIZE] = {0};
        byte client_nonce[PROTOCOL_NONCE_SIZE] = {0};
        byte client_proof[32] = {0};
        byte expected_server_proof[32] = {0};
        byte server_proof[32] = {0};
        bool valid = False;
        if (!socket_recv_all(connection->socket, server_nonce, sizeof(server_nonce)) ||
            os_random_bytes(client_nonce, sizeof(client_nonce)) != 0) goto clear_client_auth;
        __proof__(key, "TCPer-client-proof-v3", header, server_nonce, client_nonce,
                  client_proof);
        if (!socket_send_all(connection->socket, client_nonce, sizeof(client_nonce)) ||
            !socket_send_all(connection->socket, client_proof, sizeof(client_proof)) ||
            !socket_recv_all(connection->socket, &status, sizeof(status)))
            goto clear_client_auth;
        if (status != HANDSHAKE_AUTHENTICATED) {
            __print_handshake_error__(status);
            goto clear_client_auth;
        }
        if (!socket_recv_all(connection->socket, server_proof, sizeof(server_proof)))
            goto clear_client_auth;
        __proof__(key, "TCPer-server-proof-v3", header, server_nonce, client_nonce,
                  expected_server_proof);
        valid = secure_memequal(server_proof, expected_server_proof, 32);
        if (valid) __initialize_secure_connection__(connection, key, header, server_nonce,
                                                    client_nonce, True);
        clear_client_auth:
        secure_memzero(server_nonce, sizeof(server_nonce));
        secure_memzero(client_nonce, sizeof(client_nonce));
        secure_memzero(client_proof, sizeof(client_proof));
        secure_memzero(server_proof, sizeof(server_proof));
        secure_memzero(expected_server_proof, sizeof(expected_server_proof));
        if (!valid) {
            if (status == HANDSHAKE_AUTHENTICATED)
                printf("\x1b[1;37;41m[Error]\x1b[0m server identity verification failed.\n");
            return False;
        }
    }

    if (protocol_recv_all(connection, &status, sizeof(status)) != 0) return False;
    if (status == HANDSHAKE_OK) return True;
    __print_handshake_error__(status);
    return False;
}

bool protocol_server_authenticate(ProtocolConnection *connection,
                                  TransportProtocolHeader *header, const char *key,
                                  const char *server_ipv4, int server_port,
                                  double deadline) {
    if (!__receive_protocol_header__(connection->socket, header, server_ipv4, server_port,
                                    deadline))
        return False;
    header->sha256[sizeof(header->sha256) - 1] = 0;
    header->mode[sizeof(header->mode) - 1] = 0;
    header->filename[sizeof(header->filename) - 1] = 0;
    bool client_secure = strcmp(header->mode, PROTOCOL_SECURE_MAGIC) == 0;
    bool client_plaintext = strcmp(header->mode, PROTOCOL_PLAINTEXT_MAGIC) == 0;
    bool server_secure = key && key[0];
    if ((!client_secure && !client_plaintext) || client_secure != server_secure) {
        byte status = HANDSHAKE_ERROR_MODE;
        socket_send_all(connection->socket, &status, sizeof(status));
        socket_shutdown(connection->socket);
        return False;
    }

    if (!server_secure) {
        byte status = HANDSHAKE_PLAINTEXT;
        return socket_send_all(connection->socket, &status, sizeof(status));
    }

    byte status = HANDSHAKE_SECURE;
    byte server_nonce[PROTOCOL_NONCE_SIZE] = {0};
    byte client_nonce[PROTOCOL_NONCE_SIZE] = {0};
    byte received_proof[32] = {0};
    byte expected_proof[32] = {0};
    byte server_proof[32] = {0};
    bool valid = False;
    if (os_random_bytes(server_nonce, sizeof(server_nonce)) != 0 ||
        !socket_send_all(connection->socket, &status, sizeof(status)) ||
        !socket_send_all(connection->socket, server_nonce, sizeof(server_nonce)) ||
        !socket_recv_all_deadline(connection->socket, client_nonce, sizeof(client_nonce), deadline) ||
        !socket_recv_all_deadline(connection->socket, received_proof, sizeof(received_proof), deadline))
        goto clear_server_auth;
    __proof__(key, "TCPer-client-proof-v3", header, server_nonce, client_nonce,
              expected_proof);
    valid = secure_memequal(received_proof, expected_proof, 32);
    if (!valid) {
        status = HANDSHAKE_ERROR_TOKEN;
        socket_send_all(connection->socket, &status, sizeof(status));
        socket_shutdown(connection->socket);
    } else {
        status = HANDSHAKE_AUTHENTICATED;
        __proof__(key, "TCPer-server-proof-v3", header, server_nonce, client_nonce,
                  server_proof);
        valid = socket_send_all(connection->socket, &status, sizeof(status)) &&
            socket_send_all(connection->socket, server_proof, sizeof(server_proof));
        if (valid) __initialize_secure_connection__(connection, key, header, server_nonce,
                                                    client_nonce, False);
    }
    clear_server_auth:
    secure_memzero(server_nonce, sizeof(server_nonce));
    secure_memzero(client_nonce, sizeof(client_nonce));
    secure_memzero(received_proof, sizeof(received_proof));
    secure_memzero(expected_proof, sizeof(expected_proof));
    secure_memzero(server_proof, sizeof(server_proof));
    return valid;
}

int protocol_send_all(ProtocolConnection *connection, const void *buffer, usize length) {
    if (!connection || (!buffer && length > 0)) return -1;
    if (!connection->secure)
        return socket_send_all(connection->socket, (void *)buffer, length) ? 0 : -1;
    const byte *input = (const byte *)buffer;
    while (length > 0) {
        if (connection->send_sequence == UINT64_MAX) return -1;
        usize count = length > PROTOCOL_RECORD_MAX_SIZE ? PROTOCOL_RECORD_MAX_SIZE : length;
        byte header[PROTOCOL_RECORD_HEADER_SIZE];
        byte ciphertext[PROTOCOL_RECORD_MAX_SIZE];
        byte tag[32];
        __write_uint32__(header, (uint32)count);
        __write_uint64__(header + 4, connection->send_sequence);
        memcpy(ciphertext, input, count);
        __xor_record__(connection->send_encryption_key, connection->send_sequence,
                       ciphertext, count);
        __record_tag__(connection->send_mac_key, header, ciphertext, count, tag);
        if (!socket_send_all(connection->socket, header, sizeof(header)) ||
            !socket_send_all(connection->socket, ciphertext, count) ||
            !socket_send_all(connection->socket, tag, sizeof(tag))) {
            secure_memzero(ciphertext, count);
            secure_memzero(tag, sizeof(tag));
            return -1;
        }
        secure_memzero(ciphertext, count);
        secure_memzero(tag, sizeof(tag));
        connection->send_sequence++;
        input += count;
        length -= count;
    }
    return 0;
}

long protocol_recv(ProtocolConnection *connection, void *buffer, usize length) {
    if (!connection || !buffer || length == 0) return -1;
    if (!connection->secure) {
        int count = length > 0x7fffffffU ? 0x7fffffff : (int)length;
        return socket_recv(connection->socket, buffer, count, 0);
    }
    if (connection->receive_offset == connection->receive_length &&
        !__receive_record__(connection)) return -1;
    usize available = connection->receive_length - connection->receive_offset;
    usize count = length < available ? length : available;
    memcpy(buffer, connection->receive_buffer + connection->receive_offset, count);
    connection->receive_offset += count;
    return (long)count;
}

int protocol_recv_all(ProtocolConnection *connection, void *buffer, usize length) {
    byte *output = (byte *)buffer;
    while (length > 0) {
        long count = protocol_recv(connection, output, length);
        if (count <= 0) return -1;
        output += count;
        length -= (usize)count;
    }
    return 0;
}

int protocol_shutdown(ProtocolConnection *connection) {
    return connection ? socket_shutdown(connection->socket) : -1;
}
