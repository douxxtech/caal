#ifndef PTY_BRIDGE_H
#define PTY_BRIDGE_H

#include <sys/types.h>

/*
 * Create a Unix socket crun can connect to and hand the PTY master over.
 * sock_path is filled with the socket path, sock_path_len is its size.
 * Returns the listening socket fd on success, -1 on error.
 */
int pty_bridge_init(char *sock_path, size_t sock_path_len);

/*
 * Accept crun's connection on sock_fd and receive the PTY master fd
 * via SCM_RIGHTS. Closes sock_fd when done.
 * Returns the master fd on success, -1 on error.
 */
int pty_bridge_recv(int sock_fd);

/*
 * Put stdin into raw mode, then proxy bytes between stdin/stdout and
 * master_fd until child exits. Forwards SIGWINCH as TIOCSWINSZ on the
 * master so terminal resizes work correctly inside the container.
 * Restores the original termios on exit.
 */
void pty_bridge_run(int master_fd, pid_t child);

#endif /* PTY_BRIDGE_H */