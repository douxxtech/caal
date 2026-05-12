/*
 * Copyright (C) 2026 douxxtech
 * caald_client: client library for talking to caald daemon
 */

#include "caald_client.h"

#include <errno.h>
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

static bool send_request(int fd, const caald_request_t *req) {
    return write_all(fd, req, sizeof(*req));
}

static bool recv_response(int fd, caald_response_t *resp) {
    return read_all(fd, resp, sizeof(*resp));
}

bool caald_session_register(int fd, const char *username,
                            const char *container_id, pid_t pid) {
    caald_request_t req = {0};
    req.type = CAALD_SESSION_REGISTER;
    strncpy(req.username, username, sizeof(req.username) - 1);
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    req.pid = (int32_t)pid;

    if (!send_request(fd, &req))
        return false;

    caald_response_t resp = {0};
    if (!recv_response(fd, &resp))
        return false;

    return resp.ok;
}

bool caald_session_unregister(int fd, const char *container_id) {
    caald_request_t req = {0};
    req.type = CAALD_SESSION_UNREGISTER;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (!send_request(fd, &req))
        return false;

    caald_response_t resp = {0};
    if (!recv_response(fd, &resp))
        return false;

    return resp.ok;
}

int caald_session_count(int fd) {
    caald_request_t req = {0};
    req.type = CAALD_SESSION_COUNT;

    if (!send_request(fd, &req))
        return -1;

    caald_response_t resp = {0};
    if (!recv_response(fd, &resp))
        return -1;

    return resp.ok ? (int)resp.count : -1;
}

int caald_session_list(int fd, caald_session_info_t *sessions,
                       int max_sessions) {
    caald_request_t req = {0};
    req.type = CAALD_SESSION_LIST;

    if (!send_request(fd, &req))
        return -1;

    /* first read the header to get the count */
    caald_response_t resp = {0};
    if (!recv_response(fd, &resp) || !resp.ok)
        return -1;

    int count = (int)resp.count;
    if (count > max_sessions)
        count = max_sessions;

    /* then read count session structs back-to-back */
    for (int i = 0; i < count; i++) {
        if (!read_all(fd, &sessions[i], sizeof(caald_session_info_t)))
            return -1;
    }

    return count;
}

bool caald_session_kill(int fd, const char *container_id) {
    caald_request_t req = {0};
    req.type = CAALD_SESSION_KILL;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (!send_request(fd, &req))
        return false;

    caald_response_t resp = {0};
    if (!recv_response(fd, &resp))
        return false;

    return resp.ok;
}

bool caald_session_kill_user(int fd, const char *username) {
    caald_request_t req = {0};
    req.type = CAALD_SESSION_KILL_USER;
    strncpy(req.username, username, sizeof(req.username) - 1);

    if (!send_request(fd, &req))
        return false;

    caald_response_t resp = {0};
    if (!recv_response(fd, &resp))
        return false;

    return resp.ok;
}