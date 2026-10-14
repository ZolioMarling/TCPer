#ifndef _PARSE_H_
#define _PARSE_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../stdc/type.h"
#include "../stdc/socket.h"

void parse_username(char *buffer, char **envs);

int parse_port(char *buffer);

int parse_config_file(char *path, char *host_ipv4, int *host_port, char *proxy_ipv4, int *proxy_port);

int parse_config_cmd(char *arg1, char *arg2, char *host_ipv4, int *host_port, char *proxy_ipv4, int *proxy_port);

int parse_config_write(char *path, char *host_ipv4, int host_port, char *proxy_ipv4, int proxy_port);

void parse_unit_convert(uint64 x, char *buffer, int size);
#endif
