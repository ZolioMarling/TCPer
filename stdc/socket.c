#include "socket.h"


Socket socket_init(void) {
    #if defined(__OS_WINDOWS__)
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return SOCKET_INVALID;
    #elif defined(__OS_UNIX__)
        signal(SIGPIPE, SIG_IGN);
    #endif
    return 0;
}


Socket socket_create(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}


void socket_close(Socket s) {
    #if defined(__OS_WINDOWS__)
        if (s == SOCKET_INVALID) return;
        else closesocket(s);
    #elif defined(__OS_UNIX__)
        if (s == SOCKET_INVALID) return;
        else close(s);
    #endif
}


void socket_destroy(void) {
    #if defined(__OS_WINDOWS__)
        WSACleanup();
    #endif
}


void socket_config(struct sockaddr_in *server, int domain, char *ip, int port) {
    #if defined(__OS_UNIX__)
        server->sin_family = domain;
        server->sin_addr.s_addr = inet_addr(ip);
        server->sin_port = socket_htons(port);
    #elif defined(__OS_WINDOWS__)
        server->sin_family = domain;
        server->sin_addr.S_un.S_addr = inet_addr(ip);
        server->sin_port = socket_htons(port);
    #endif
}


Socket socket_connect(Socket s, struct sockaddr_in *server, int size) {
    int status = connect(s, (struct sockaddr *)server, size);
    return status < 0 ? SOCKET_INVALID : 0;
}


long socket_send(Socket s, char *buffer, int length, int flag) {
    return send(s, buffer, length, flag);
}


long socket_sendto(Socket s, void *buffer, int length, int flag, struct sockaddr_in *to, int size) {
    return sendto(s, buffer, length, flag, (struct sockaddr *)to, size);
}


long socket_recv(Socket s, char *buffer, int length, int flag) {
    return recv(s, buffer, length, flag);
}


bool socket_send_all(Socket s, void *buffer, usize length) {
    const char *p = (const char *)buffer;
    while (length > 0) {
        int chunk = length > 0x7fffffffU ? 0x7fffffff : (int)length;
        long n = socket_send(s, (char *)p, chunk, 0);
        if (n < 0) {
            #if defined(__OS_WINDOWS__)
                if (WSAGetLastError() == WSAEINTR) continue;
            #else
                if (errno == EINTR) continue;
            #endif
            return False;
        }
        if (n == 0) return False;
        p = p + n;
        length = length - (usize)n;
    }
    return True;
}


bool socket_recv_all(Socket s, void *buffer, usize length) {
    char *p = (char *)buffer;
    while (length > 0) {
        int chunk = length > 0x7fffffffU ? 0x7fffffff : (int)length;
        long n = socket_recv(s, p, chunk, 0);
        if (n < 0) {
            #if defined(__OS_WINDOWS__)
                if (WSAGetLastError() == WSAEINTR) continue;
            #else
                if (errno == EINTR) continue;
            #endif
            return False;
        }
        if (n == 0) return False;
        p = p + n;
        length = length - (usize)n;
    }
    return True;
}


bool socket_recv_all_deadline(Socket s, void *buffer, usize length, double deadline) {
    char *p = (char *)buffer;
    while (length > 0) {
        double remaining = deadline - os_time();
        if (remaining <= 0 || socket_setopt_timeout(s, 1, remaining) == SOCKET_INVALID) return False;
        int chunk = length > 0x7fffffffU ? 0x7fffffff : (int)length;
        long n = socket_recv(s, p, chunk, 0);
        if (n < 0) {
            #if defined(__OS_WINDOWS__)
                if (WSAGetLastError() == WSAEINTR) continue;
            #else
                if (errno == EINTR) continue;
            #endif
            return False;
        }
        if (n == 0) return False;
        p = p + n;
        length = length - (usize)n;
    }
    return True;
}


long socket_recvfrom(Socket s, void *buffer, int length, int flag, struct sockaddr_in *from, int *size) {
    #if defined(__OS_UNIX__)
        return recvfrom(s, buffer, length, flag, (struct sockaddr *)from, (socklen_t *)size);
    #elif defined(__OS_WINDOWS__)
        return recvfrom(s, buffer, length, flag, (struct sockaddr *)from, size);
    #endif
}


Socket socket_bind(Socket s, struct sockaddr_in *address_name, int size) {
    int status = bind(s, (struct sockaddr *)address_name, size);
    return status < 0 ? SOCKET_INVALID : 0;
}


Socket socket_listen(Socket s, int backlog) {
    int status = listen(s, backlog);
    return status < 0 ? SOCKET_INVALID : 0;
}


Socket socket_accept(Socket s, struct sockaddr_in *address, int *size_pointer) {
    #if defined(__OS_UNIX__)
        return accept(s, (struct sockaddr *)address, (socklen_t *)size_pointer);
    #elif defined(__OS_WINDOWS__)
        return accept(s, (struct sockaddr *)address, size_pointer);
    #endif
}


Socket socket_setopt(Socket s, int level, int optname, void *ctx, int size) {
    int status = 0;
    if (ctx == NULL && size == 0) {
        int opt = 1;
        #if defined(__OS_UNIX__)
            status = setsockopt(s, level, optname, (void *)&opt, sizeof(opt));
        #elif defined(__OS_WINDOWS__)
            status = setsockopt(s, level, optname, (char *)&opt, sizeof(opt));
        #endif
    } else {
        #if defined(__OS_UNIX__)
            status = setsockopt(s, level, optname, ctx, size);
        #elif defined(__OS_WINDOWS__)
            status = setsockopt(s, level, optname, (char *)ctx, size);
        #endif
    }
    return status < 0 ? SOCKET_INVALID : 0;
}


unsigned int socket_ntohl(unsigned int value) {
    return ntohl(value);
}


unsigned int socket_htonl(unsigned int value) {
    return htonl(value);
}


unsigned short socket_ntohs(unsigned short value) {
    return ntohs(value);
}


unsigned short socket_htons(unsigned short value) {
    return htons(value);
}


void socket_ipv4(char *buffer, int size) {
    if (!buffer || size <= 0) return;
    buffer[0] = '\0';

    #if defined(__OS_UNIX__)
        struct ifaddrs *ifa;
        struct ifaddrs *ifaddr;
        if (getifaddrs(&ifaddr) == -1) return;
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), buffer, size, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                buffer[0] = '\0';
                continue;
            }
            freeifaddrs(ifaddr);
            return;
        }
        freeifaddrs(ifaddr);
    #elif defined(__OS_WINDOWS__)
        WSADATA wsaData;
        char hostname[256];
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        struct addrinfo *ptr = NULL;

        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

        if (gethostname(hostname, sizeof(hostname)) != 0) {
            WSACleanup();
            return;
        }

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
            WSACleanup();
            return;
        }

        for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
            if (buffer && size > 0) buffer[0] = '\0';
            if (ptr->ai_family != AF_INET) continue;
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)ptr->ai_addr;
            char *ip = inet_ntoa(ipv4->sin_addr);
            if (ip == NULL) continue;
            if (strcmp(ip, "127.0.0.1") == 0) continue;
            strncpy(buffer, ip, size);
            buffer[size - 1] = '\0';
            break;
        }

        freeaddrinfo(res);
        WSACleanup();
    #endif
}


Socket socket_setopt_timeout(Socket c, int type, double second) {
    if (second < 0.0 || second > (double)(INT_MAX / 1000)) return SOCKET_INVALID;
    int s = (int)second;
    int ms = (int)((second - s) * 1000 + 0.5);
    if (ms >= 1000) {
        s++;
        ms = ms - 1000;
    }
    #if defined(__OS_UNIX__)
        struct timeval timeout = { .tv_sec = s, .tv_usec = ms * 1000 };
    #elif defined(__OS_WINDOWS__)
        int timeout = s * 1000 + ms;
    #endif
    if (type == 0) return socket_setopt(c, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    else return socket_setopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}


int socket_valid_ipv4(char *buffer) {
    if (!buffer) return 0;
    struct in_addr address;
    return inet_pton(AF_INET, buffer, &address) == 1;
}


Socket socket_connect_timeout(Socket s, struct sockaddr_in *server, int size, double second) {
    // Set to non-blocking mode.
    #if defined(__OS_UNIX__)
        int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0) return SOCKET_INVALID;
    #elif defined(__OS_WINDOWS__)
        unsigned long mode = 1;
        if (ioctlsocket(s, FIONBIO, &mode) != 0) return SOCKET_INVALID;
    #endif

    // Initiate standard C connection.
    int condition = connect(s, (struct sockaddr *)server, size);
    Socket status = 0;

    if (condition < 0) {
        // It must be verified if socket that responded actually connected or just failed instantly.
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(s, &writefds);

        int ms = (int)((second - (int)second) * 1000 + 0.5);
        struct timeval tv = { .tv_sec = (int)second, .tv_usec = ms * 1000 };

        condition = select((int)(s + 1), NULL, &writefds, NULL, &tv);

        if (condition > 0) {
            int error = 0;
            int len = sizeof(error);
            #if defined(__OS_UNIX__)
                int result = getsockopt(s, SOL_SOCKET, SO_ERROR, (void *)&error, (socklen_t *)&len);
            #elif defined(__OS_WINDOWS__)
                int result = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
            #endif
            status = (result != 0 || error != 0) ? SOCKET_INVALID : 0;
        } else {
            // `status = 0` for timeout; `status < 0` for `select()` error.
            status = SOCKET_INVALID;
        }
    }

    // Restore socket to standard blocking mode.
    #if defined(__OS_UNIX__)
        if (fcntl(s, F_SETFL, flags) != 0) status = SOCKET_INVALID;
    #elif defined(__OS_WINDOWS__)
        mode = 0;
        if (ioctlsocket(s, FIONBIO, &mode) != 0) status = SOCKET_INVALID;
    #endif

    return status;
}


long socket_send_nowait(Socket s, char *buffer, int length) {
    #if defined(__OS_UNIX__)
        return send(s, buffer, length, MSG_DONTWAIT);
    #elif defined(__OS_WINDOWS__)
        u_long mode = 1;    // `1` for non-blocking.
        if (ioctlsocket(s, FIONBIO, &mode) != 0) return send(s, buffer, length, 0);
        long result = send(s, buffer, length, 0);
        mode = 0;   // `0` for blocking.
        ioctlsocket(s, FIONBIO, &mode);
        return result;
    #endif
}


long long socket_sendfile(Socket s, FILE *f, long long offset, long long size) {
    if (s == SOCKET_INVALID || !f || offset < 0 || size < 0) return -1;
    #if defined(__linux__)
        int fd = fileno(f);
        long long sent = 0;
        off_t off = (off_t)offset;
        while (sent < size) {
            long n = sendfile(s, fd, &off, (unsigned long)(size - sent));
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return -1;
            }
            if (n == 0) break;
            sent = sent + n;
        }
        return sent;
    #elif defined(__APPLE__) || defined(__MACH__)
        int fd = fileno(f);
        long long sent = 0;
        off_t off = (off_t)offset;
        while (sent < size) {
            off_t length = (off_t)(size - sent);
            int n = sendfile(fd, s, off, &length, NULL, 0);
            if (length > 0) {
                off = off + length;
                sent = sent + length;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return -1;
            }
            if (length == 0) break;
        }
        return sent;
    #else
        char buffer[65536];
        long long sent = 0;
        if (size < 0 || socket_fseek(f, offset, SEEK_SET) != 0) return -1;
        while (sent < size) {
            usize want = (size - sent) > (long long)sizeof(buffer) ? sizeof(buffer) : (usize)(size - sent);
            usize length = fread(buffer, 1, want, f);
            if (length == 0) {
                if (ferror(f)) return -1;
                break;
            }
            char *p = buffer;
            usize left = length;
            while (left > 0) {
                int chunk = left > 0x7fffffffU ? 0x7fffffff : (int)left;
                long n = socket_send(s, p, chunk, 0);
                if (n < 0) {
                    #if defined(__OS_WINDOWS__)
                        if (WSAGetLastError() == WSAEINTR) continue;
                    #else
                        if (errno == EINTR) continue;
                    #endif
                    return -1;
                }
                if (n == 0) return -1;
                p = p + n;
                left = left - (usize)n;
                sent = sent + n;
            }
        }
        return sent;
    #endif
}
