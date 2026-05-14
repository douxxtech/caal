/*
 * CaaLsh - Container as a Login Shell
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

#ifndef CONTAINER_H
#define CONTAINER_H

#include <stdint.h>
#include <sys/types.h>

/*
 * All state for one container session lives here.
 *
 * Callers zero-initialise this struct, pass it to container_start(), then
 * hand it to container_stop() when done. Do not touch the fields directly
 * after container_start() returns.
 */
typedef struct {
    /* filled in by container_start() */
    char container_id[64]; /* unique crun identifier */
    char session_dir[128]; /* per-session directory under SESSION_DIR */
    char image_path[128];  /* loop image path */
    char rootfs[128];      /* overlay mount point (bundle rootfs) */
    char sock_path[108];   /* console socket path */

    pid_t child_pid; /* crun process */
} container_t;

/*
 * Set up the overlay, launch crun, wire the PTY.
 * Returns 0 on success. On failure all partial state is cleaned up and a
 * diagnostic has been printed to stderr.
 */
int container_start(container_t *ct, const char *bundle_path,
                    int64_t disk_size_mb);

/*
 * Tear down everything owned by this session.
 *
 * Safe to call after a failed container_start() as long as *ct was
 * zero-initialised before the call.
 */
void container_stop(container_t *ct);

#endif /* CONTAINER_H */