#include "usage.h"


void usage_logo(char *user_name) {
    printf(
        " \x1b[1;31m░██████████  ░██████  ░█████████\x1b[0m   C Local Transfer (MIT License)\n"
        " \x1b[1;32m    ░██     ░██   ░██ ░██     ░██\x1b[0m  available version V3\n"
        " \x1b[1;33m    ░██    ░██        ░██     ░██\x1b[0m  built with Clang 16.0.0, GCC 13.3.0 and MinGW 15.2.0\n"
        " \x1b[1;34m    ░██    ░██        ░█████████\x1b[0m   https://github.com/Illusionna/LocalTransfer\n"
        " \x1b[1;35m    ░██    ░██        ░██\x1b[0m          Illusionna (Zolio Marling) www@orzzz.net\n"
        " \x1b[1;36m    ░██     ░██   ░██ ░██\x1b[0m          \x1b[38;2;255;165;0mHi, %s!\x1b[0m\n"
        " \x1b[1;37m    ░██      ░██████  ░██\x1b[0m          \x1b[38;2;255;165;0mRead the instructions below for app operation:\x1b[0m\n",
        user_name
    );
}


void usage_help(char *app_name, int port) {
    printf(" \x1b[1;32m*\x1b[0m \x1b[90m(server)\x1b[0m %s run [--port] [--key]\n >>> \x1b[1;32m%s run --port=%d --key=\"this is a long secret\"\x1b[0m\n", app_name, app_name, port);
    printf("\n");
    printf(" \x1b[1;32m*\x1b[0m \x1b[90m(client)\x1b[0m %s config [--host] [--proxy]\n >>> \x1b[1;32m%s config --host=8.8.8.8:%d --proxy=127.0.0.1:7890\x1b[0m\n", app_name, app_name, port);
    printf("\n");
    printf(" \x1b[1;32m*\x1b[0m \x1b[90m(client)\x1b[0m %s send <path> [--key]\n >>> \x1b[1;32m%s send \"C:\\Users\\Illusionna\\Desktop\\demo\"\x1b[0m\n", app_name, app_name);
    printf("\n");
    printf(" \x1b[1;32m*\x1b[0m \x1b[90m(client)\x1b[0m %s list <dir> [--key]\n >>> \x1b[1;32m%s list . --key=\"this is a long secret\"\x1b[0m\n", app_name, app_name);
    printf("\n");
    printf(" \x1b[1;32m*\x1b[0m \x1b[90m(client)\x1b[0m %s download <path> [--key]\n >>> \x1b[1;32m%s download ./demo\x1b[0m\n", app_name, app_name);
    printf("\n");
    printf(" \x1b[1;31m!\x1b[0m \x1b[90m(client)\x1b[0m %s remove <path> [--key]\n >>> \x1b[1;31m%s remove .\x1b[0m\n", app_name, app_name);
}
