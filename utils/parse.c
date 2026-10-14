#include "parse.h"

#include "../stdc/os.h"

#include <ctype.h>

static int __parse_decimal_port__(const char *text, int allow_zero, int *port) {
    if (!text || !text[0] || !port) return 1;
    unsigned long value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!isdigit(*p)) return 1;
        value = value * 10 + (*p - '0');
        if (value > 65535) return 1;
    }
    if ((!allow_zero && value == 0) || value > 65535) return 1;
    *port = (int)value;
    return 0;
}

static int __parse_endpoint__(const char *text, char *ipv4, int *port) {
    if (!text || !ipv4 || !port) return 1;
    const char *colon = strrchr(text, ':');
    if (!colon || colon == text || strchr(text, ':') != colon) return 1;

    usize length = (usize)(colon - text);
    if (length > 15 || __parse_decimal_port__(colon + 1, 0, port) != 0) return 1;
    char parsed_ipv4[16];
    memcpy(parsed_ipv4, text, length);
    parsed_ipv4[length] = '\0';
    if (!socket_valid_ipv4(parsed_ipv4)) return 1;
    memcpy(ipv4, parsed_ipv4, length + 1);
    return 0;
}

void parse_username(char *buffer, char **envs) {
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

int parse_port(char *buffer) {
    int port;
    return __parse_decimal_port__(buffer, 0, &port) == 0 ? port : 0;
}

int parse_config_file(char *path, char *host_ipv4, int *host_port, char *proxy_ipv4, int *proxy_port) {
    if (!path || !host_ipv4 || !host_port || !proxy_ipv4 || !proxy_port) return 1;
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;

    char parsed_host[16] = {0};
    char parsed_proxy[16] = {0};
    int parsed_host_port = 0;
    int parsed_proxy_port = 0;
    int seen_host = 0;
    int seen_host_port = 0;
    int seen_proxy = 0;
    int seen_proxy_port = 0;
    int valid = 1;

    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        usize length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && !feof(fp)) {
            valid = 0;
            break;
        }
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';

        const char *value;
        if (strncmp(line, "host ipv4 = ", strlen("host ipv4 = ")) == 0) {
            value = line + strlen("host ipv4 = ");
            usize value_length = strlen(value);
            if (seen_host || value_length >= sizeof(parsed_host) ||
                !socket_valid_ipv4((char *)value)) { valid = 0; break; }
            memcpy(parsed_host, value, value_length + 1);
            seen_host = 1;
        } else if (strncmp(line, "host port = ", strlen("host port = ")) == 0) {
            value = line + strlen("host port = ");
            if (seen_host_port || __parse_decimal_port__(value, 0, &parsed_host_port) != 0) { valid = 0; break; }
            seen_host_port = 1;
        } else if (strncmp(line, "proxy ipv4 = ", strlen("proxy ipv4 = ")) == 0) {
            value = line + strlen("proxy ipv4 = ");
            usize value_length = strlen(value);
            if (seen_proxy || value_length >= sizeof(parsed_proxy) ||
                (value[0] && !socket_valid_ipv4((char *)value))) { valid = 0; break; }
            memcpy(parsed_proxy, value, value_length + 1);
            seen_proxy = 1;
        } else if (strncmp(line, "proxy port = ", strlen("proxy port = ")) == 0) {
            value = line + strlen("proxy port = ");
            if (seen_proxy_port || __parse_decimal_port__(value, 1, &parsed_proxy_port) != 0) { valid = 0; break; }
            seen_proxy_port = 1;
        } else {
            valid = 0;
            break;
        }
    }
    if (ferror(fp) || fclose(fp) != 0) valid = 0;
    if (!valid || !seen_host || !seen_host_port || !seen_proxy || !seen_proxy_port ||
        (parsed_proxy[0] == '\0' ? parsed_proxy_port != 0 : parsed_proxy_port == 0)) return 1;

    memcpy(host_ipv4, parsed_host, strlen(parsed_host) + 1);
    memcpy(proxy_ipv4, parsed_proxy, strlen(parsed_proxy) + 1);
    *host_port = parsed_host_port;
    *proxy_port = parsed_proxy_port;
    return 0;
}

int parse_config_cmd(char *arg1, char *arg2, char *host_ipv4, int *host_port, char *proxy_ipv4, int *proxy_port) {
    if (!host_ipv4 || !host_port || !proxy_ipv4 || !proxy_port) return 1;
    host_ipv4[0] = '\0';
    proxy_ipv4[0] = '\0';
    *host_port = 0;
    *proxy_port = 0;

    int seen_host = 0;
    int seen_proxy = 0;
    char *args[2] = {arg1, arg2};
    for (int i = 0; i < 2; i++) {
        if (!args[i]) continue;
        if (strncmp(args[i], "--host=", strlen("--host=")) == 0) {
            if (seen_host || __parse_endpoint__(args[i] + strlen("--host="), host_ipv4, host_port) != 0) return 1;
            seen_host = 1;
        } else if (strncmp(args[i], "--proxy=", strlen("--proxy=")) == 0) {
            if (seen_proxy || __parse_endpoint__(args[i] + strlen("--proxy="), proxy_ipv4, proxy_port) != 0) return 1;
            seen_proxy = 1;
        } else return 1;
    }
    return seen_host ? 0 : 1;
}

int parse_config_write(char *path, char *host_ipv4, int host_port, char *proxy_ipv4, int proxy_port) {
    if (!path || !host_ipv4 || !proxy_ipv4 || !socket_valid_ipv4(host_ipv4) ||
        host_port <= 0 || host_port > 65535 ||
        (proxy_ipv4[0] ? (!socket_valid_ipv4(proxy_ipv4) || proxy_port <= 0 || proxy_port > 65535) : proxy_port != 0)) return 1;
    FILE *fp = os_fopen_private_write(path);
    if (!fp) return 1;
    int status = fprintf(fp, "host ipv4 = %s\nhost port = %d\nproxy ipv4 = %s\nproxy port = %d\n", host_ipv4, host_port, proxy_ipv4, proxy_port) < 0;
    if (fclose(fp) != 0) status = 1;
    return status;
}

void parse_unit_convert(uint64 x, char *buffer, int size) {
    if (x < 1024ULL) snprintf(buffer, size, "%llu B", x);
    else if (x < 1048576ULL) snprintf(buffer, size, "%.3lf KB", x / 1024.0);
    else if (x < 1073741824ULL) snprintf(buffer, size, "%.3lf MB", x / 1024.0 / 1024.0);
    else if (x < 1099511627776ULL) snprintf(buffer, size, "%.3lf GB", x / 1024.0 / 1024.0 / 1024.0);
    else if (x < 1125899906842624ULL) snprintf(buffer, size, "%.3lf TB", x / 1024.0 / 1024.0 / 1024.0 / 1024.0);
    else snprintf(buffer, size, "%.3lf PB", x / 1024.0 / 1024.0 / 1024.0 / 1024.0 / 1024.0);
}
