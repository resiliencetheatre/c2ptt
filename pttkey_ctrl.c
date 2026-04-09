/*
 * pttkey_ctrl - send ptt_down / ptt_up to pttkey_rec over Unix socket
 *
 * Usage:
 *   pttkey_ctrl ptt_down
 *   pttkey_ctrl ptt_up
 *   pttkey_ctrl -s /tmp/pttkey.sock ptt_down
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-s socket_path] [ptt_down|ptt_up]\n", prog);
}

int main(int argc, char *argv[])
{
    const char *socket_path = "/tmp/pttkey.sock";
    const char *cmd = NULL;
    int fd;
    struct sockaddr_un addr;
    int opt;

    while ((opt = getopt(argc, argv, "s:h")) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    cmd = argv[optind];

    if (strcmp(cmd, "ptt_down") != 0 && strcmp(cmd, "ptt_up") != 0) {
        fprintf(stderr, "Invalid command: %s\n", cmd);
        usage(argv[0]);
        return 1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", socket_path);
        close(fd);
        return 1;
    }

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n", socket_path, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) < 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
