/*
 * CaaLctl - Container as a Login Shell Control
 * Copyright (C) 2026 douxxtech
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "lib/caald_client.h"

/*
 * Print the command summary to stderr.
 */
static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <command> [args]\n"
            "\n"
            "Commands:\n"
            "  list              list all active sessions\n"
            "  count             print the number of active sessions\n"
            "  kill <id>         kill a session by container ID\n"
            "  killuser <user>   kill all sessions for a user\n",
            prog);
}

/*
 * Fetch and print all active sessions from the daemon.
 * Columns: username, container ID, PID, start time.
 */
static int cmd_list(int fd) {
    caald_session_info_t sessions[1024];

    int n = caald_session_list(fd, sessions, 1024);
    if (n < 0) {
        fprintf(stderr, "[CaaLctl] failed to list sessions\n");
        return 1;
    }

    if (n == 0) {
        printf("no active sessions\n");
        return 0;
    }

    printf("%-20s %-30s %-8s %s\n", "USERNAME", "CONTAINER ID", "PID",
           "STARTED");
    printf("%-20s %-30s %-8s %s\n", "--------", "------------", "---",
           "-------");

    for (int i = 0; i < n; i++) {
        char timebuf[32];

        time_t start_time = (time_t)sessions[i].start_time;
        struct tm *tm = localtime(&start_time);

        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

        printf("%-20s %-30s %-8d %s\n", sessions[i].username,
               sessions[i].container_id, (int)sessions[i].pid, timebuf);
    }

    return 0;
}

/*
 * Print the number of active sessions to stdout.
 */
static int cmd_count(int fd) {
    int n = caald_session_count(fd);
    if (n < 0) {
        fprintf(stderr, "[CaaLctl] failed to get session count\n");
        return 1;
    }
    printf("%d\n", n);
    return 0;
}

/*
 * Kill a single session by container ID.
 */
static int cmd_kill(int fd, const char *container_id) {
    if (!caald_session_kill(fd, container_id)) {
        fprintf(stderr, "[CaaLctl] failed to kill session '%s'\n",
                container_id);
        return 1;
    }
    printf("killed session %s\n", container_id);
    return 0;
}

/*
 * Kill all active sessions owned by a user.
 */
static int cmd_killuser(int fd, const char *username) {
    if (!caald_session_kill_user(fd, username)) {
        fprintf(stderr, "[CaaLctl] failed to kill sessions for user '%s'\n",
                username);
        return 1;
    }
    printf("killed all sessions for user '%s'\n", username);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    int fd = caald_connect();
    if (fd < 0) {
        fprintf(stderr, "[CaaLctl] Cannot connect to caald (is it running?)\n");
        fprintf(stderr, "[CaaLctl] You may want to run this as root (sudo) if "
                        "you aren't already\n");
        return 1;
    }

    int ret = 0;

    if (strcmp(argv[1], "list") == 0) {
        ret = cmd_list(fd);
    } else if (strcmp(argv[1], "count") == 0) {
        ret = cmd_count(fd);
    } else if (strcmp(argv[1], "kill") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[CaaLctl] kill requires a container ID\n");
            ret = 1;
        } else {
            ret = cmd_kill(fd, argv[2]);
        }
    } else if (strcmp(argv[1], "killuser") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[CaaLctl] killuser requires a username\n");
            ret = 1;
        } else {
            ret = cmd_killuser(fd, argv[2]);
        }
    } else {
        fprintf(stderr, "[CaaLctl] unknown command '%s'\n", argv[1]);
        usage(argv[0]);
        ret = 1;
    }

    close(fd);
    return ret;
}