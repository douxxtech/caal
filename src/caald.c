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

static volatile sig_atomic_t g_reload_config = 0;
static session_t *sessions = NULL;
static int session_count = 0;
static int max_sessions = 0;

/*
 * Write a structured session event to syslog.
 */
static void log_event(const char *event, const char *username,
                      const char *container_id) {
    syslog(LOG_INFO, "[CaaLd] %s user=%s container=%s", event, username,
           container_id);
}

/*
 * Write exactly n bytes to fd, retrying on partial writes.
 * Returns true on success, false if the write fails or the fd closes early.
 */
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

/*
 * Read exactly n bytes from fd, retrying on partial reads.
 * Returns true on success, false if the read fails or the fd closes early.
 */
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

/* reload config on SIGHUP */
static void on_sighup(int sig) {
    (void)sig;
    g_reload_config = 1;
}

/*
 * Re-read max_sessions from the config file.
 * Does not shrink the session table, only grows it if the new value
 * is higher than what was allocated, so active sessions are never lost.
 */
static void reload_config(void) {
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (fp == NULL) {
        syslog(LOG_ERR, "[reload] could not open config");
        return;
    }

    toml_result_t config = toml_parse_file(fp);
    fclose(fp);
    if (!config.ok) {
        syslog(LOG_ERR,
               "[reload] could not parse config, keeping current values");
        return;
    }

    toml_datum_t datum = toml_seek(config.toptab, "max_sessions");
    int new_max = (datum.type == TOML_INT64 && datum.u.int64 > 0)
                      ? (int)datum.u.int64
                      : 1024;

    toml_free(config);

    if (new_max == max_sessions) {
        syslog(LOG_INFO, "[reload] max_sessions unchanged (%d)", max_sessions);
        return;
    }

    if (new_max < max_sessions) {
        /* don't shrink since active sessions could be in the slots we'd free */
        syslog(LOG_WARNING,
               "[reload] new max_sessions (%d) is lower than current (%d), "
               "ignoring shrink",
               new_max, max_sessions);
        return;
    }

    session_t *new_table = realloc(sessions, new_max * sizeof(session_t));
    if (!new_table) {
        syslog(LOG_ERR, "[reload] realloc failed, keeping current table");
        return;
    }

    /* zero out the newly added slots */
    memset(new_table + max_sessions, 0,
           (new_max - max_sessions) * sizeof(session_t));

    sessions = new_table;
    max_sessions = new_max;
    syslog(LOG_INFO, "[reload] max_sessions updated to %d", max_sessions);
}

/*
 * Look up an active session by container ID.
 * Returns a pointer into the sessions table, or NULL if not found.
 */
static session_t *find_session(const char *container_id) {
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].container_id, container_id) == 0)
            return &sessions[i];
    }
    return NULL;
}

/*
 * Register a new session in the table.
 *
 * Reuses the first inactive slot before appending a new one. Fails if the
 * session table is already at capacity.
 */
static void handle_register(const caald_request_t *req,
                            caald_response_t *resp) {
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

/*
 * Mark a session as inactive.
 * Fails if no active session matches the given container ID.
 */
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

/*
 * Return the number of currently active sessions.
 */
static void handle_count(caald_response_t *resp) {
    int count = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active)
            count++;
    }
    resp->ok = 1;
    resp->count = count;
}

/*
 * Stream all active sessions to the client.
 *
 * Sends the response header first with the session count, then streams each
 * session struct individually. Sets resp->ok to 0 before returning to signal
 * to the caller that the response has already been sent.
 */
static void handle_list(int client_fd, caald_response_t *resp) {
    int count = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active)
            count++;
    }

    resp->ok = 1;
    resp->count = count;

    write_all(client_fd, resp, sizeof(*resp));

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
 * Send SIGTERM to a session process and mark it inactive.
 *
 * The kill is optimistic: we signal the PID and mark the slot inactive without
 * waiting for the process to actually exit.
 * TODO: add verification that the process has exited before marking inactive.
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

/*
 * Send SIGTERM to all active sessions owned by a user.
 * Fails if the user has no active sessions. Sets resp->count to the number
 * of sessions that were killed.
 */
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

/*
 * Dispatch a single client request and send the response.
 *
 * Reads one caald_request_t from the client fd, routes it to the appropriate
 * handler, then writes back a caald_response_t. handle_list() sends its own
 * response and sets resp->ok = 0 to suppress the second write here.
 */
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
        already_sent = true;
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

    /* flock on the pid file ensures only one instance runs at a time */
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

    /* write our PID now that we have forked into the daemon process */
    char pidbuf[32];
    ftruncate(pid_fd, 0);
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
    write(pid_fd, pidbuf, strlen(pidbuf));

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

    reload_config();
    if (!sessions) {
        syslog(LOG_ERR, "failed to allocate session table");
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_sighup;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);

    /* accept loop: handle one client at a time */
    while (1) {
        if (g_reload_config) {
            g_reload_config = 0;
            reload_config();
        }

        int client_fd = accept(sock_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        /* drop any client that stalls for more than 5 seconds */
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handle_client(client_fd);
        close(client_fd);
    }

    return 0;
}