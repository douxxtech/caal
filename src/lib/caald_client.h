/*
 * Copyright (C) 2026 douxxtech
 * caald_client: client library for talking to caald daemon
 */

#ifndef CAALD_CLIENT_H
#define CAALD_CLIENT_H

#include "caald_proto.h"
#include <stdbool.h>

/* connect to the daemon socket, returns fd or -1 on failure */
int caald_connect(void);

/* register a new session */
bool caald_session_register(int fd, const char *username, const char *container_id, pid_t pid);

/* unregister a session */
bool caald_session_unregister(int fd, const char *container_id);

/* get the current session count, returns count or -1 on error */
int caald_session_count(int fd);

/* list all active sessions, returns number of sessions or -1 on error */
int caald_session_list(int fd, caald_session_info_t *sessions, int max_sessions);

/* kill a specific session by container_id */
bool caald_session_kill(int fd, const char *container_id);

/* kill all sessions for a user */
bool caald_session_kill_user(int fd, const char *username);

#endif /* CAALD_CLIENT_H */