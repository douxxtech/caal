/*
 * CaaLd - Container as a Login Shell
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

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "lib/caald_proto.h"

typedef struct {
    char username[64];
    char container_id[64];
    pid_t pid;
    time_t start_time;
    int active;
} session_t;

static session_t sessions[MAX_SERVER_SESSIONS];
static int session_count = 0;

/* logging */
static void log_event(const char *event, const char *username,
                      const char *container_id) {
    syslog(LOG_INFO, "[CaaLd] %s user=%s container=%s", event, username,
           container_id);
}

/* find a session by container_id */
static session_t *find_session(const char *container_id) {
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].container_id, container_id) == 0)
            return &sessions[i];
    }
    return NULL;
}

/* register a new session */
static void handle_register(const char *username, const char *container_id,
                            pid_t pid, char *resp) {
    if (session_count >= MAX_SERVER_SESSIONS) {
        snprintf(resp, CAALD_MAX_MSG,
                 "{\"ok\":false,\"error\":\"max sessions reached\"}");
        return;
    }

    session_t *s = &sessions[session_count++];
    strncpy(s->username, username, sizeof(s->username) - 1);
    strncpy(s->container_id, container_id, sizeof(s->container_id) - 1);
    s->pid = pid;
    s->start_time = time(NULL);
    s->active = 1;

    log_event("SESSION_START", username, container_id);
    snprintf(resp, CAALD_MAX_MSG, "{\"ok\":true}");
}

/* unregister a session */
static void handle_unregister(const char *container_id, char *resp) {
    session_t *s = find_session(container_id);
    if (!s) {
        snprintf(resp, CAALD_MAX_MSG,
                 "{\"ok\":false,\"error\":\"session not found\"}");
        return;
    }

    log_event("SESSION_END", s->username, container_id);
    s->active = 0;
    snprintf(resp, CAALD_MAX_MSG, "{\"ok\":true}");
}

/* count active sessions */
static void handle_count(char *resp) {
    int count = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active)
            count++;
    }
    snprintf(resp, CAALD_MAX_MSG, "{\"ok\":true,\"count\":%d}", count);
}

/* list all active sessions */
static void handle_list(char *resp) {
    int offset = snprintf(resp, CAALD_MAX_MSG, "{\"ok\":true,\"sessions\":[");

    int first = 1;
    for (int i = 0; i < session_count; i++) {
        if (!sessions[i].active)
            continue;

        if (!first)
            offset += snprintf(resp + offset, CAALD_MAX_MSG - offset, ",");
        first = 0;

        offset += snprintf(resp + offset, CAALD_MAX_MSG - offset,
                           "{\"username\":\"%s\",\"container_id\":\"%s\","
                           "\"pid\":%d,\"start_time\":%ld}",
                           sessions[i].username, sessions[i].container_id,
                           (int)sessions[i].pid, (long)sessions[i].start_time);
    }

    snprintf(resp + offset, CAALD_MAX_MSG - offset, "]}");
}

/* kill a session by container_id */
static void handle_kill(const char *container_id, char *resp) {
    session_t *s = find_session(container_id);
    if (!s) {
        snprintf(resp, CAALD_MAX_MSG,
                 "{\"ok\":false,\"error\":\"session not found\"}");
        return;
    }

    kill(s->pid, SIGTERM);
    log_event("SESSION_KILLED", s->username, container_id);
    s->active = 0;
    snprintf(resp, CAALD_MAX_MSG, "{\"ok\":true}");
}

/* kill all sessions for a user */
static void handle_kill_user(const char *username, char *resp) {
    int killed = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active && strcmp(sessions[i].username, username) == 0) {
            kill(sessions[i].pid, SIGTERM);
            log_event("SESSION_KILLED", username, sessions[i].container_id);
            sessions[i].active = 0;
            killed++;
        }
    }

    if (killed == 0) {
        snprintf(resp, CAALD_MAX_MSG,
                 "{\"ok\":false,\"error\":\"no sessions found\"}");
    } else {
        snprintf(resp, CAALD_MAX_MSG, "{\"ok\":true,\"killed\":%d}", killed);
    }
}

/* process a client message */
static void handle_client(int client_fd) {
    uint32_t len;
    if (read(client_fd, &len, sizeof(len)) != sizeof(len))
        return;
    if (len > CAALD_MAX_MSG)
        return;

    char *buf = malloc(len + 1);
    if (!buf)
        return;
    if (read(client_fd, buf, len) != (ssize_t)len) {
        free(buf);
        return;
    }
    buf[len] = '\0';

    /* parse message type */
    int type = -1;
    sscanf(buf, "{\"type\":%d", &type);

    char resp[CAALD_MAX_MSG];
    memset(resp, 0, sizeof(resp));

    switch (type) {
    case CAALD_SESSION_REGISTER: {
        char username[64], container_id[64];
        int pid;
        if (sscanf(buf,
                   "{\"type\":%*d,\"username\":\"%63[^\"]\",\"container_id\":"
                   "\"%63[^\"]\",\"pid\":%d}",
                   username, container_id, &pid) == 3) {
            handle_register(username, container_id, pid, resp);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"ok\":false,\"error\":\"parse error\"}");
        }
        break;
    }
    case CAALD_SESSION_UNREGISTER: {
        char container_id[64];
        if (sscanf(buf, "{\"type\":%*d,\"container_id\":\"%63[^\"]\"}",
                   container_id) == 1) {
            handle_unregister(container_id, resp);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"ok\":false,\"error\":\"parse error\"}");
        }
        break;
    }
    case CAALD_SESSION_COUNT:
        handle_count(resp);
        break;
    case CAALD_SESSION_LIST:
        handle_list(resp);
        break;
    case CAALD_SESSION_KILL: {
        char container_id[64];
        if (sscanf(buf, "{\"type\":%*d,\"container_id\":\"%63[^\"]\"}",
                   container_id) == 1) {
            handle_kill(container_id, resp);

        } else {
            snprintf(resp, sizeof(resp),
                     "{\"ok\":false,\"error\":\"parse error\"}");
        }
        break;
    }
    case CAALD_SESSION_KILL_USER: {
        char username[64];
        if (sscanf(buf, "{\"type\":%*d,\"username\":\"%63[^\"]\"}", username) ==
            1) {
            handle_kill_user(username, resp);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"ok\":false,\"error\":\"parse error\"}");
        }
        break;
    }
    default:
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"error\":\"unknown type\"}");
    }

    free(buf);

    /* send response */
    uint32_t resp_len = strlen(resp);
    write(client_fd, &resp_len, sizeof(resp_len));
    write(client_fd, resp, resp_len);
}

int main(void) {
    openlog("caald", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    /* daemonize */
    if (fork() != 0)
        exit(0);
    setsid();
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* create socket */
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CAALD_SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(CAALD_SOCK_PATH);
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        return 1;
    }

    chmod(CAALD_SOCK_PATH, 0666);

    if (listen(sock_fd, 128) < 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        return 1;
    }

    syslog(LOG_INFO, "caald started");

    /* accept loop */
    while (1) {
        int client_fd = accept(sock_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        handle_client(client_fd);
        close(client_fd);
    }

    return 0;
}