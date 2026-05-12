/*
 * CaaLd - Container as a Login Shell Daemon
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
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "lib/caald_proto.h"
#include "lib/tomlc17.h"

typedef struct {
    char username[CAALD_MAX_STR];
    char container_id[CAALD_MAX_STR];
    pid_t pid;
    time_t start_time;
    int active;
} session_t;

static session_t *sessions = NULL;
static int session_count = 0;
static int max_sessions = 0;

/* logging */
static void log_event(const char *event, const char *username,
                      const char *container_id) {
    syslog(LOG_INFO, "[CaaLd] %s user=%s container=%s", event, username,
           container_id);
}

/* send exactly n bytes, retrying on partial writes */
static bool write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return false;
        p += w;
        n -= w;
    }
    return true;
}

/* read exactly n bytes, retrying on partial reads */
static bool read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
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
static void handle_register(const caald_request_t *req,
                            caald_response_t *resp) {
    /* reuse an inactive slot before appending */
    session_t *s = NULL;
    for (int i = 0; i < session_count; i++) {
        if (!sessions[i].active) {
            s = &sessions[i];
            break;
        }
    }

    if (!s) {
        if (session_count >= max_sessions) {
            resp->ok = 0;
            strncpy(resp->error, "max sessions reached",
                    sizeof(resp->error) - 1);
            return;
        }
        s = &sessions[session_count++];
    }

    snprintf(s->username, sizeof(s->username), "%s", req->username);
    snprintf(s->container_id, sizeof(s->container_id), "%s", req->container_id);
    s->pid = (pid_t)req->pid;
    s->start_time = time(NULL);
    s->active = 1;

    log_event("SESSION_START", s->username, s->container_id);
    resp->ok = 1;
}

/* unregister a session */
static void handle_unregister(const caald_request_t *req,
                              caald_response_t *resp) {
    session_t *s = find_session(req->container_id);
    if (!s) {
        resp->ok = 0;
        strncpy(resp->error, "session not found", sizeof(resp->error) - 1);
        return;
    }

    log_event("SESSION_END", s->username, s->container_id);
    s->active = 0;
    resp->ok = 1;
}

/* count active sessions */
static void handle_count(caald_response_t *resp) {
    int count = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active)
            count++;
    }
    resp->ok = 1;
    resp->count = count;
}

/* list all active sessions */
static void handle_list(int client_fd, caald_response_t *resp) {
    /* count first so we can fill resp->count before sending */
    int count = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active)
            count++;
    }

    resp->ok = 1;
    resp->count = count;

    /* send the response header first */
    write_all(client_fd, resp, sizeof(*resp));

    /* then stream the session structs */
    for (int i = 0; i < session_count; i++) {
        if (!sessions[i].active)
            continue;

        caald_session_info_t info = {0};
        snprintf(info.username, sizeof(info.username), "%s",
                 sessions[i].username);
        snprintf(info.container_id, sizeof(info.container_id), "%s",
                 sessions[i].container_id);
        info.pid = (int32_t)sessions[i].pid;
        info.start_time = (int64_t)sessions[i].start_time;

        write_all(client_fd, &info, sizeof(info));
    }

    /* signal to the caller that we already sent the response */
    resp->ok = 0;
}

/* 
 * kill a session by container_id
 * right now, it is optimistic. Maybe add verification another time
 */
static void handle_kill(const caald_request_t *req, caald_response_t *resp) {
    session_t *s = find_session(req->container_id);
    if (!s) {
        resp->ok = 0;
        strncpy(resp->error, "session not found", sizeof(resp->error) - 1);
        return;
    }

    kill(s->pid, SIGTERM);
    log_event("SESSION_KILLED", s->username, s->container_id);
    s->active = 0;
    resp->ok = 1;
}

/* kill all sessions for a user */
static void handle_kill_user(const caald_request_t *req,
                             caald_response_t *resp) {
    int killed = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].username, req->username) == 0) {
            kill(sessions[i].pid, SIGTERM);
            log_event("SESSION_KILLED", req->username,
                      sessions[i].container_id);
            sessions[i].active = 0;
            killed++;
        }
    }

    if (killed == 0) {
        resp->ok = 0;
        strncpy(resp->error, "no sessions found", sizeof(resp->error) - 1);
    } else {
        resp->ok = 1;
        resp->count = killed;
    }
}

/* process a client message */
static void handle_client(int client_fd) {
    caald_request_t req = {0};
    if (!read_all(client_fd, &req, sizeof(req)))
        return;

    caald_response_t resp = {0};
    bool already_sent = false;

    switch ((caald_msg_type_t)req.type) {
    case CAALD_SESSION_REGISTER:
        handle_register(&req, &resp);
        break;
    case CAALD_SESSION_UNREGISTER:
        handle_unregister(&req, &resp);
        break;
    case CAALD_SESSION_COUNT:
        handle_count(&resp);
        break;
    case CAALD_SESSION_LIST:
        handle_list(client_fd, &resp);
        already_sent = true; /* handle_list sends its own response */
        break;
    case CAALD_SESSION_KILL:
        handle_kill(&req, &resp);
        break;
    case CAALD_SESSION_KILL_USER:
        handle_kill_user(&req, &resp);
        break;
    default:
        resp.ok = 0;
        strncpy(resp.error, "unknown type", sizeof(resp.error) - 1);
    }

    if (!already_sent)
        write_all(client_fd, &resp, sizeof(resp));
}

int main(void) {
    openlog("caald", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    /* ensure only one instance runs at a time */
    int pid_fd = open(CAALD_PID_PATH, O_RDWR | O_CREAT, 0644);
    if (pid_fd < 0) {
        syslog(LOG_ERR, "could not open pid file");
        return 1;
    }
    if (flock(pid_fd, LOCK_EX | LOCK_NB) < 0) {
        syslog(LOG_ERR, "caald already running, exiting");
        close(pid_fd);
        return 0;
    }

    /* daemonize */
    if (fork() != 0)
        exit(0);
    setsid();
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* write our PID now that we've forked */
    char pidbuf[32];
    ftruncate(pid_fd, 0);
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
    write(pid_fd, pidbuf, strlen(pidbuf));

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

    chmod(CAALD_SOCK_PATH, 0600);

    if (listen(sock_fd, 128) < 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        return 1;
    }

    syslog(LOG_INFO, "caald started");

    /* parse config to get max_sessions */
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (fp == NULL) {
        syslog(LOG_ERR, "could not open config");
        return 1;
    }

    toml_result_t config = toml_parse_file(fp);
    fclose(fp);
    if (!config.ok) {
        syslog(LOG_ERR, "could not parse config");
        return 1;
    }

    toml_datum_t max_sess_datum = toml_seek(config.toptab, "max_sessions");
    if (max_sess_datum.type == TOML_INT64 && max_sess_datum.u.int64 > 0)
        max_sessions = (int)max_sess_datum.u.int64;
    else
        max_sessions = 1024;

    sessions = calloc(max_sessions, sizeof(session_t));
    if (!sessions) {
        syslog(LOG_ERR, "failed to allocate session table");
        return 1;
    }

    toml_free(config);

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