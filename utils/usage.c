#include "usage.h"

void usage_logo(void) {
    printf(
        " _________    ______  _______   \n"
        "|  _   _  | .' ___  ||_   __ \\     easy and quick way to transmit your file\n"
        "|_/ | | \\_|/ .'   \\_|  | |__) |    available version 2.0\n"
        "    | |    | |         |  ___/     https://github.com/Illusionna/TCPer\n"
        "   _| |_   \\ `.___.'\\ _| |_        Illusionna (Zolio Marling) www@orzzz.net\n"
        "  |_____|   `.____ .'|_____|     --------------------------------------------\n\n"
        "\x1b[1;37m   _\x1b[0m\n"
        "\x1b[1;31m  ( \\                ..-----..__\x1b[0m\n"
        "\x1b[1;31m   \\.'.        _.--'`  [   '  ' ```'-._\x1b[0m\n"
        "\x1b[1;32m    `. `'-..-'' `    '  ' '   .  ;   ; `-'''-.,__/|/_\x1b[0m\n"
        "\x1b[1;32m      `'-.;..-''`|'  `.  '.    ;     '  `    '   `'  `,\x1b[0m\n"
        "\x1b[1;33m                 \\ '   .    ' .     '   ;   .`   . ' 7 \\\x1b[0m\n"
        "\x1b[1;33m                  '.' . '- . \\    .`   .`  .   .\\     `Y\x1b[0m\n"
        "\x1b[1;34m                    '-.' .   ].  '   ,    '    /'`\"\"';:'\x1b[0m\n"
        "\x1b[1;34m                      /Y   '.] '-._ /    ' _.-'\x1b[0m\n"
        "\x1b[1;35m                      \\'\\_   ; (`'.'.'  .\"/\"\x1b[0m\n"
        "\x1b[1;35m                       ' )` /  `.'   .-'.'\x1b[0m\n"
        "\x1b[1;36m                        '\\  \\).'  .-'--\"\x1b[0m\n"
        "\x1b[1;36m                          `. `,_'`\x1b[0m\n"
        "\x1b[1;37m                            `.__)\x1b[0m\n"
    );
}

void usage_help(char *user_name, char *app_name, int port) {
    printf("\nHi %s, welcome! Before you explore TCPer app.\nI suggest reading through the usage help guide below:\n\n", user_name);
    printf(" * \x1b[90m(server)\x1b[0m %s run [port] [--key=KEY]\n\t>>> \x1b[1;32mTCPER_KEY='a long secret' %s run %d\x1b[0m\n", app_name, app_name, port);
    printf(" * \x1b[90m(client)\x1b[0m %s config [host] [proxy]\n\t>>> \x1b[1;32m%s config --host=8.8.8.8:%d --proxy=127.0.0.1:7890\x1b[0m\n", app_name, app_name, port);
    printf(" * \x1b[90m(client)\x1b[0m %s send <path> [--key=KEY]\n", app_name);
    printf(" * \x1b[90m(client)\x1b[0m %s list <dir> [--key=KEY]\n", app_name);
    printf(" * \x1b[90m(client)\x1b[0m %s download <path> [--key=KEY]\n", app_name);
    printf(" ! \x1b[90m(client)\x1b[0m %s remove <path> [--key=KEY]\n", app_name);
    printf("\n   Set the same TCPER_KEY on both peers for authenticated encryption.\n"
           "   Without a key, TCPer intentionally uses plaintext mode.\n");
}

void usage_progress_bar(int64 current, int64 total, int step, char *description) {
    if (step <= 0 || (current != 0 && current % (int64)step != 0 && current != total)) return;
    int width = 50;
    double percentage = (total > 0) ? (double)current / total : 0.0;
    int filled = percentage * width;
    printf("\r[");
    for (int i = 0; i < width; i++) {
        if (i < filled) printf("\x1b[32m=\x1b[0m");
        else if (i == filled) printf(">");
        else printf(" ");
    }
    printf("] %7.2lf%% (%lld/%lld)", percentage * 100, current, total);
    if (description != NULL && description[0] != '\0') printf(" \x1b[33m%s\x1b[0m\x1b[K", description);
    else printf("\x1b[K");
    fflush(stdout);
    if (current >= total) printf("\n");
}
