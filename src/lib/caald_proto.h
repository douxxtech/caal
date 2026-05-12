/*
 * Copyright (C) 2026 douxxtech
 * caald_proto: shared protocol definitions for caald daemon
 */

#ifndef CAALD_PROTO_H
#define CAALD_PROTO_H

#include <stdint.h>
#include <time.h>

#define CAALD_SOCK_PATH "/run/caald.sock"
#define CAALD_MAX_MSG 8192

/* message types */
typedef enum {
    CAALD_SESSION_REGISTER = 1,
    CAALD_SESSION_UNREGISTER,
    CAALD_SESSION_COUNT,
    CAALD_SESSION_LIST,
    CAALD_SESSION_KILL,
    CAALD_SESSION_KILL_USER,
} caald_msg_type_t;

/* session info structure used in list responses */
typedef struct {
    char username[64];
    char container_id[64];
    pid_t pid;
    time_t start_time;
} caald_session_info_t;

#endif