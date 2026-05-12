/*
 * Copyright (C) 2026 douxxtech
 * caald_proto: shared protocol definitions for caald daemon
 */

#ifndef CAALD_PROTO_H
#define CAALD_PROTO_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define CAALD_MAX_STR 64

/* message types */
typedef enum {
    CAALD_SESSION_REGISTER = 1,
    CAALD_SESSION_UNREGISTER,
    CAALD_SESSION_COUNT,
    CAALD_SESSION_LIST,
    CAALD_SESSION_KILL,
    CAALD_SESSION_KILL_USER,
} caald_msg_type_t;

/*
 * Single fixed-size request struct for all message types.
 * Unused fields are zeroed by the sender and ignored by the receiver.
 */
typedef struct {
    uint8_t type;
    char username[CAALD_MAX_STR];
    char container_id[CAALD_MAX_STR];
    int32_t pid;
} __attribute__((packed)) caald_request_t;

/*
 * Fixed-size response for everything except LIST.
 * - ok:    1 on success, 0 on failure
 * - count: filled for COUNT responses
 * - error: null-terminated message on failure
 */
typedef struct {
    uint8_t ok;
    int32_t count;
    char error[CAALD_MAX_STR];
} __attribute__((packed)) caald_response_t;

/*
 * One entry in a LIST response.
 * The daemon sends a caald_response_t first (with count = N),
 * then N of these structs back-to-back.
 */
typedef struct {
    char username[CAALD_MAX_STR];
    char container_id[CAALD_MAX_STR];
    int32_t pid;
    int64_t start_time;
} __attribute__((packed)) caald_session_info_t;

#endif /* CAALD_PROTO_H */