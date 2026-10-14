#ifndef _SOCKET_H_
#define _SOCKET_H_


#if !defined(__OS_WINDOWS__) && !defined(__OS_UNIX__)
    #if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
        #define __OS_WINDOWS__
    #elif defined(__linux__) || defined(__APPLE__) || defined(__MACH__)
        #define __OS_UNIX__
        #ifndef _GNU_SOURCE
            #define _GNU_SOURCE
        #endif
    #else
        #error "Unsupported platforms."
    #endif
#endif


#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>


#include "os.h"


#if defined(__OS_WINDOWS__)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#elif defined(__OS_UNIX__)
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <netdb.h>
    #if defined(__linux__)
        #include <sys/sendfile.h>
    #endif
#endif


#if defined(__OS_WINDOWS__)
    typedef SOCKET Socket;
    #define SOCKET_INVALID INVALID_SOCKET
    /**
     * @brief Send `FIN` to inform the destination that all data has been sent.
     * @param s The socket descriptor.
     * @return `0` for success, `-1` for failure.
    **/
    #define socket_shutdown(s) shutdown(s, SD_SEND)
    #define socket_abort(s) shutdown(s, SD_BOTH)
    #define socket_fseek _fseeki64
#elif defined(__OS_UNIX__)
    typedef int Socket;
    #define SOCKET_INVALID -1
    /**
     * @brief Send `FIN` to inform the destination that all data has been sent.
     * @param s The socket descriptor.
     * @return `0` for success, `-1` for failure.
    **/
    #define socket_shutdown(s) shutdown(s, SHUT_WR)
    #define socket_abort(s) shutdown(s, SHUT_RDWR)
    #define socket_fseek fseeko
#endif


/**
 * @brief Network to host long long.
 * @param x network.
 * @return host.
**/
#define socket_ntohll(x) ((ntohl(1) == 1) ? (x) : (((unsigned long long)ntohl((x) & 0xffffffffUL)) << 32 | ntohl((unsigned int)((x) >> 32))))


/**
 * @brief Host to network long long.
 * @param x host.
 * @return network.
**/
#define socket_htonll(x) ((htonl(1) == 1) ? (x) : (((unsigned long long)htonl((x) & 0xffffffffUL)) << 32 | htonl((unsigned int)((x) >> 32))))


/**
 * @brief Initialize a socket.
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_init(void);


/**
 * @brief Create a socket.
 * @param domain The communication domain (e.g. `AF_INET` for IPv4).
 * @param type The socket type (e.g. `SOCK_STREAM` for TCP, `SOCK_DGRAM` for UDP).
 * @param protocol The protocol to be used (e.g. `0` for default).
 * @return A socket descriptor (e.g. `SOCKET_INVALID` for failure).
**/
Socket socket_create(int domain, int type, int protocol);


/**
 * @brief Close a socket.
 * @param s The socket descriptor.
**/
void socket_close(Socket s);


/**
 * @brief Destroy the socket.
**/
void socket_destroy(void);


/**
 * @brief Configure a socket address structure.
 * @param server Pointer to the `sockaddr_in` structure.
 * @param domain The communication domain (e.g. `AF_INET` for IPv4).
 * @param ip The IP address (e.g. `"127.0.0.1"`).
 * @param port The host port (e.g. `8080`).
**/
void socket_config(struct sockaddr_in *server, int domain, char *ip, int port);


/**
 * @brief Connect to a server using socket.
 * @param s The socket descriptor.
 * @param server Pointer to the `sockaddr_in` structure.
 * @param size The `sizeof(server)` of the `sockaddr_in` structure.
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_connect(Socket s, struct sockaddr_in *server, int size);


/**
 * @brief Send data through socket TCP tunnel.
 * @param s The socket descriptor.
 * @param buffer The data buffer.
 * @param length The length of data buffer.
 * @param flag The flag for sending data (e.g. `0` for default).
 * @return `> 0` for actual bytes sent, `= 0` for connection closed, `< 0` for failure in sending.
**/
long socket_send(Socket s, char *buffer, int length, int flag);


/**
 * @brief Send data to destination with UDP.
 * @param s The socket description.
 * @param buffer The head address of data.
 * @param length The length of data.
 * @param flag The flag for sending data (e.g. `0` for default).
 * @param to The storage of destination host and port.
 * @param size Use `sizeof(struct sockaddr)` to get the size of `to`.
 * @return `> 0` for actual bytes sent, `= 0` for connection closed, `< 0` for failure in sending.
**/
long socket_sendto(Socket s, void *buffer, int length, int flag, struct sockaddr_in *to, int size);


/**
 * @brief Receive the response from the server with blocking.
 * @param s The socket descriptor.
 * @param buffer The response data buffer.
 * @param length The length of response data buffer.
 * @param flag The flag for receiving data (e.g. `0` for default).
 * @return `> 0` for actual bytes received, `= 0` for connection closed, `< 0` for failure in receiving.
**/
long socket_recv(Socket s, char *buffer, int length, int flag);


/**
 * @brief Send an entire buffer through a TCP socket.
 * @param s The socket descriptor.
 * @param buffer The data buffer.
 * @param length The number of bytes to send.
 * @return `True` if every byte was sent, otherwise `False`.
**/
bool socket_send_all(Socket s, void *buffer, usize length);


/**
 * @brief Receive an entire buffer from a TCP socket.
 * @param s The socket descriptor.
 * @param buffer The destination data buffer.
 * @param length The number of bytes to receive.
 * @return `True` if every byte was received, otherwise `False`.
**/
bool socket_recv_all(Socket s, void *buffer, usize length);


/**
 * @brief Receive an entire buffer before a monotonic-time deadline.
 * @param s The socket descriptor.
 * @param buffer The destination data buffer.
 * @param length The number of bytes to receive.
 * @param deadline The absolute monotonic-time deadline.
 * @return `True` if every byte was received before the deadline, otherwise `False`.
**/
bool socket_recv_all_deadline(Socket s, void *buffer, usize length, double deadline);


/**
 * @brief Receive data from destination with UDP.
 * @param s The socket description.
 * @param buffer The head address of data.
 * @param length The length of data.
 * @param flag The flag for receiving data (e.g. `0` for default).
 * @param from The storage of source host and port.
 * @param size Use `sizeof(struct sockaddr)` to get the size of `from`.
 * @return `> 0` for actual bytes received, `= 0` for connection closed, `< 0` for failure in receiving.
**/
long socket_recvfrom(Socket s, void *buffer, int length, int flag, struct sockaddr_in *from, int *size);


/**
 * @brief Bind socket to a certain address.
 * @param s The socket descriptor.
 * @param address_name Pointer to the `sockaddr_in` structure.
 * @param size The `sizeof(address_name)` of the `sockaddr_in` structure.
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_bind(Socket s, struct sockaddr_in *address_name, int size);


/**
 * @brief Listen the socket.
 * @param s The socket descriptor.
 * @param backlog The maximum queue length waiting for connection (e.g. `backlog = 5`, five connection requests will be queued).
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_listen(Socket s, int backlog);


/**
 * @brief Accept the socket.
 * @param s The socket descriptor.
 * @param address Pointer to the `sockaddr_in` structure.
 * @param size_pointer The `sizeof(address)` of the `sockaddr_in` structure pointer.
 * @return A new socket object (`SOCKET_INVALID` for failure).
**/
Socket socket_accept(Socket s, struct sockaddr_in *address, int *size_pointer);


/**
 * @brief Set the option of socket.
 * @param s The socket descriptor.
 * @param level `SOL_SOCKET` for default.
 * @param optname `SO_REUSEADDR` for default (port reuse).
 * @param ctx The context data (you can use `NULL`).
 * @param size The size of `ctx` (you can use `0`).
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_setopt(Socket s, int level, int optname, void *ctx, int size);


/**
 * @brief Network to host long (e.g. `0x12345678` to `0x78563412`).
 * @param value network.
 * @return host.
**/
unsigned int socket_ntohl(unsigned int value);


/**
 * @brief Host to network long (e.g. `0x78563412` to `0x12345678`).
 * @param value host.
 * @return network.
**/
unsigned int socket_htonl(unsigned int value);


/**
 * @brief Network to host short (e.g. `0x1234` to `0x3412`).
 * @param value network.
 * @return host.
**/
unsigned short socket_ntohs(unsigned short value);


/**
 * @brief Host to network short (e.g. `0x3412` to `0x1234`).
 * @param value host.
 * @return network.
**/
unsigned short socket_htons(unsigned short value);


/**
 * @brief Get the host IPv4.
 * @param buffer Store the result.
 * @param size The size of buffer.
**/
void socket_ipv4(char *buffer, int size);


/**
 * @brief Set the option of socket time out.
 * @param c The socket descriptor.
 * @param type `0` for sender, `1` for receiver.
 * @param second You can set `3s` or `0.02s`.
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_setopt_timeout(Socket c, int type, double second);


/**
 * @brief Check if `buffer` is a valid IPv4 address.
 * @param buffer The input string.
 * @return `1` for yes, `0` for no.
**/
int socket_valid_ipv4(char *buffer);


/**
 * @brief Connect to a server using socket with timeout in non-blocking.
 * @param s The socket descriptor.
 * @param server Pointer to the `sockaddr_in` structure.
 * @param size The `sizeof(server)` of the `sockaddr_in` structure.
 * @param second You can set `3s` or `0.02s`.
 * @return `SOCKET_INVALID` for failure.
**/
Socket socket_connect_timeout(Socket s, struct sockaddr_in *server, int size, double second);


/**
 * @brief Send data through socket TCP tunnel in non-blocking.
 * @param s The socket descriptor.
 * @param buffer The data buffer.
 * @param length The length of data buffer.
 * @return `> 0` for actual bytes sent, `= 0` for connection closed, `< 0` for failure in sending.
**/
long socket_send_nowait(Socket s, char *buffer, int length);


/**
 * @brief Send a file using socket. `Linux` and `macOS` use `sendfile`; `Windows` uses `fread` + `send`.
 * @param s Destination socket to send data to.
 * @param f Source file pointer opened in binary read mode `"rb"`.
 * @param offset Byte offset within the file to start sending from.
 * @param size Number of bytes to send starting from offset (typically `total_size - offset`).
 * @return Total bytes sent on success, `-1` on a non-recoverable error.
**/
long long socket_sendfile(Socket s, FILE *f, long long offset, long long size);


#endif
