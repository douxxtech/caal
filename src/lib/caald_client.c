/*
 * Copyright (C) 2026 douxxtech
 * caald_client: client library for talking to caald daemon
 */

#include "caald_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int caald_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CAALD_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* send a length-prefixed JSON message */
static bool send_msg(int fd, const char *json) {
    uint32_t len = strlen(json);
    if (write(fd, &len, sizeof(len)) != sizeof(len))
        return false;
    if (write(fd, json, len) != (ssize_t)len)
        return false;
    return true;
}

/* recv a length-prefixed JSON response */
static char *recv_msg(int fd) {
    uint32_t len;
    if (read(fd, &len, sizeof(len)) != sizeof(len))
        return NULL;
    if (len > CAALD_MAX_MSG)
        return NULL;

    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;
    if (read(fd, buf, len) != (ssize_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

bool caald_session_register(int fd, const char *username, const char *container_id, pid_t pid) {
    char msg[512];
    snprintf(msg, sizeof(msg),
             "{\"type\":%d,\"username\":\"%s\",\"container_id\":\"%s\",\"pid\":%d}",
             CAALD_SESSION_REGISTER, username, container_id, (int)pid);

    if (!send_msg(fd, msg))
        return false;

    char *resp = recv_msg(fd);
    if (!resp)
        return false;

    /* simple success check: look for "ok":true */
    bool ok = (strstr(resp, "\"ok\":true") != NULL);
    free(resp);
    return ok;
}

bool caald_session_unregister(int fd, const char *container_id) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":%d,\"container_id\":\"%s\"}",
             CAALD_SESSION_UNREGISTER, container_id);

    if (!send_msg(fd, msg))
        return false;

    char *resp = recv_msg(fd);
    if (!resp)
        return false;

    bool ok = (strstr(resp, "\"ok\":true") != NULL);
    free(resp);
    return ok;
}

int caald_session_count(int fd) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":%d}", CAALD_SESSION_COUNT);

    if (!send_msg(fd, msg))
        return -1;

    char *resp = recv_msg(fd);
    if (!resp)
        return -1;

    /* parse {"ok":true,"count":N} */
    int count = -1;
    char *p = strstr(resp, "\"count\":");
    if (p)
        sscanf(p + 8, "%d", &count);

    free(resp);
    return count;
}

int caald_session_list(int fd, caald_session_info_t *sessions, int max_sessions) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":%d}", CAALD_SESSION_LIST);

    if (!send_msg(fd, msg))
        return -1;

    char *resp = recv_msg(fd);
    if (!resp)
        return -1;

    /* parse the response manually (simple JSON parsing) */
    int count = 0;
    char *p = strstr(resp, "\"sessions\":[");
    if (p) {
        p += 12; /* skip past "sessions":[ */
        while (*p && count < max_sessions) {
            if (*p == '{') {
                caald_session_info_t *s = &sessions[count];
                memset(s, 0, sizeof(*s));

                /* extract username */
                char *u = strstr(p, "\"username\":\"");
                if (u) {
                    u += 12;
                    sscanf(u, "%63[^\"]", s->username);
                }

                /* extract container_id */
                char *c = strstr(p, "\"container_id\":\"");
                if (c) {
                    c += 16;
                    sscanf(c, "%63[^\"]", s->container_id);
                }

                /* extract pid */
                char *pid_p = strstr(p, "\"pid\":");
                if (pid_p)
                    sscanf(pid_p + 6, "%d", (int *)&s->pid);

                /* extract start_time */
                char *t = strstr(p, "\"start_time\":");
                if (t)
                    sscanf(t + 13, "%ld", (long *)&s->start_time);

                count++;
                p = strchr(p, '}');
                if (p)
                    p++;
            } else {
                p++;
            }
        }
    }

    free(resp);
    return count;
}

bool caald_session_kill(int fd, const char *container_id) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":%d,\"container_id\":\"%s\"}",
             CAALD_SESSION_KILL, container_id);

    if (!send_msg(fd, msg))
        return false;

    char *resp = recv_msg(fd);
    if (!resp)
        return false;

    bool ok = (strstr(resp, "\"ok\":true") != NULL);
    free(resp);
    return ok;
}

bool caald_session_kill_user(int fd, const char *username) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":%d,\"username\":\"%s\"}",
             CAALD_SESSION_KILL_USER, username);

    if (!send_msg(fd, msg))
        return false;

    char *resp = recv_msg(fd);
    if (!resp)
        return false;

    bool ok = (strstr(resp, "\"ok\":true") != NULL);
    free(resp);
    return ok;
}