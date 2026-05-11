/*
 * Copyright (C) 2026 douxxtech
 * pty_bridge: console-socket PTY proxy between sshd and crun
 */

#include "pty_bridge.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* PTY master fd, needs to be global so the SIGWINCH handler can reach it */
static int g_master_fd = -1;

/* forward the current terminal size to the PTY master on resize */
static void on_winch(int sig) {
    (void)sig;
    if (g_master_fd < 0)
        return;
    struct winsize ws = {0};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(g_master_fd, TIOCSWINSZ, &ws);
}

int pty_bridge_init(char *sock_path, size_t sock_path_len) {
    /* build a unique socket path under /tmp */
    snprintf(sock_path, sock_path_len, "/tmp/caalsh-pty-%d.sock", (int)getpid());

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0)
        return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    /* make sure there is no leftover socket from a previous crash */
    unlink(sock_path);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sock_fd, 1) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

int pty_bridge_recv(int sock_fd) {
    /* accept crun's connection */
    int conn_fd = accept(sock_fd, NULL, NULL);
    close(sock_fd);
    if (conn_fd < 0)
        return -1;

    /*
     * crun sends the PTY master fd as ancillary data (SCM_RIGHTS).
     * We need a dummy byte buffer for recvmsg to work even though
     * we dont care about the normal payload.
     */
    char buf[1];
    char cmsg_buf[CMSG_SPACE(sizeof(int))];

    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    if (recvmsg(conn_fd, &msg, 0) < 0) {
        close(conn_fd);
        return -1;
    }
    close(conn_fd);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL                       ||
        cmsg->cmsg_level != SOL_SOCKET     ||
        cmsg->cmsg_type  != SCM_RIGHTS) {
        return -1;
    }

    int master_fd;
    memcpy(&master_fd, CMSG_DATA(cmsg), sizeof(int));
    return master_fd;
}

void pty_bridge_run(int master_fd, pid_t child) {
    g_master_fd = master_fd;

    /* push an initial window size to the container */
    struct winsize ws = {0};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(master_fd, TIOCSWINSZ, &ws);

    /* arm the resize handler */
    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);

    /* put our stdin into raw mode so keypresses go straight through */
    struct termios orig, raw;
    int is_tty = (tcgetattr(STDIN_FILENO, &orig) == 0);
    if (is_tty) {
        raw = orig;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    /*
     * Proxy loop: ferry bytes in both directions until the child exits.
     * We use select() so neither side blocks the other.
     */
    char buf[4096];
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(master_fd, &rfds);
        int nfds = master_fd + 1;

        if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue; /* SIGWINCH or other signal, just retry */
            break;
        }

        /* data from the container -> our stdout */
        if (FD_ISSET(master_fd, &rfds)) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(STDOUT_FILENO, buf, n);
        }

        /* data from our stdin -> the container */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(master_fd, buf, n);
        }
    }

    /* restore terminal before we exit */
    if (is_tty)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);

    waitpid(child, NULL, 0);
    g_master_fd = -1;
}