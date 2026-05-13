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

#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "lib/caald_client.h"
#include "lib/pty_bridge.h"
#include "lib/session_disk.h"
#include "lib/tomlc17.h"

/* child PIDs must be global so signal handlers can reach them */
static pid_t child_pid = -1;
static pid_t timer_pid = -1;

/*
 * Tear down everything owned by this session.
 *
 * Unmounts the overlay filesystem, removes the session disk image,
 * force-deletes the container state via crun, then unregisters the
 * session from the daemon. Safe to call regardless of how the session ended.
 */
static void cleanup(const char *rootfs, const char *session_dir,
                    const char *image_path, const char *sock_path,
                    const char *container_id) {
    unlink(sock_path);
    umount2(rootfs, MNT_DETACH);
    session_disk_cleanup(session_dir, image_path);

    /* fork a child for crun delete so we can wait on it cleanly */
    pid_t p = fork();
    if (p == 0) {
        char *const argv[] = {"crun", "delete", "--force",
                              (char *)container_id, NULL};
        execvp("crun", argv);
        _exit(1);
    }
    if (p > 0)
        waitpid(p, NULL, 0);

    int daemon_fd = caald_connect();
    if (daemon_fd >= 0) {
        caald_session_unregister(daemon_fd, container_id);
        close(daemon_fd);
    }

    fprintf(stderr, "\n[CaaLsh] session cleaned up\n");
}

/*
 * Return the number of currently active sessions.
 *
 * Queries the daemon first. Falls back to counting session directories under
 * SESSION_DIR if the daemon is unavailable or returns an error.
 */
static int count_active_sessions(void) {
    int fd = caald_connect();
    if (fd >= 0) {
        int count = caald_session_count(fd);
        close(fd);
        if (count >= 0)
            return count;
        fprintf(stderr, "[CaaLsh] daemon query failed, falling back\n");
    }

    /* daemon not available; count caalsh-* directories as a best effort */
    DIR *d = opendir(SESSION_DIR);
    if (d == NULL)
        return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "caalsh-", 7) == 0 && ent->d_type == DT_DIR)
            count++;
    }
    closedir(d);
    return count;
}

int main(void) {
    /* resolve the uid to a username */
    struct passwd *pw = getpwuid(getuid());
    if (pw == NULL) {
        fprintf(stderr, "[CaaLsh] could not resolve user\n");
        return 1;
    }
    const char *username = pw->pw_name;

    /* sanity cap before username is used in any snprintf below */
    if (strlen(username) > 32) {
        fprintf(stderr, "[CaaLsh] username too long\n");
        return 1;
    }

    /*
     * Parse the main config file. Every user that is allowed to log in must
     * have a section in there. No section means no access.
     */
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (fp == NULL) {
        fprintf(stderr, "[CaaLsh] could not open config\n");
        return 1;
    }

    toml_result_t config = toml_parse_file(fp);
    fclose(fp);
    if (!config.ok) {
        fprintf(stderr, "[CaaLsh] could not parse config\n");
        return 1;
    }

    /* zero or missing max_sessions means unlimited */
    toml_datum_t max_sess_datum = toml_seek(config.toptab, "max_sessions");
    int64_t max_sessions = 0;
    if (max_sess_datum.type == TOML_INT64)
        max_sessions = max_sess_datum.u.int64;

    if (max_sessions > 0 && count_active_sessions() >= max_sessions) {
        fprintf(stderr, "[CaaLsh] max sessions reached, try again later\n");
        toml_free(config);
        return 1;
    }

    /*
     * Look up [username].bundle. This is the OCI bundle directory crun will
     * be pointed at. The path must be absolute.
     */
    char seek_path[128];
    snprintf(seek_path, sizeof(seek_path), "%s.bundle", username);

    toml_datum_t bundle_datum = toml_seek(config.toptab, seek_path);
    if (bundle_datum.type == TOML_UNKNOWN) {
        fprintf(stderr, "[CaaLsh] user not configured, access denied\n");
        toml_free(config);
        return 1;
    }
    if (bundle_datum.type != TOML_STRING) {
        fprintf(stderr, "[CaaLsh] bundle is not a string\n");
        toml_free(config);
        return 1;
    }

    const char *bundle_path = bundle_datum.u.s;

    if (bundle_path[0] != '/') {
        fprintf(stderr, "[CaaLsh] bundle path must be absolute\n");
        toml_free(config);
        return 1;
    }

    /* explicitly disabled users are rejected before anything else runs */
    char enabled_path[128];
    snprintf(enabled_path, sizeof(enabled_path), "%s.enabled", username);
    toml_datum_t enabled_datum = toml_seek(config.toptab, enabled_path);
    if (enabled_datum.type == TOML_BOOLEAN && !enabled_datum.u.boolean) {
        fprintf(stderr, "[CaaLsh] user disabled, access denied\n");
        toml_free(config);
        return 1;
    }

    /* zero or missing timeout means no limit */
    char timeout_path[128];
    snprintf(timeout_path, sizeof(timeout_path), "%s.timeout", username);
    toml_datum_t timeout_datum = toml_seek(config.toptab, timeout_path);
    int64_t timeout = 0;
    if (timeout_datum.type == TOML_INT64)
        timeout = timeout_datum.u.int64;

    /* disk size in megabytes; defaults to 1 GB if not set or zero */
    char disk_size_path[128];
    snprintf(disk_size_path, sizeof(disk_size_path), "%s.disk", username);
    toml_datum_t disk_size_datum = toml_seek(config.toptab, disk_size_path);
    int64_t disk_size_mb = 1024;
    if (disk_size_datum.type == TOML_INT64 && disk_size_datum.u.int64 > 0)
        disk_size_mb = disk_size_datum.u.int64;

    /*
     * Wipe the entire environment before launching crun. SSH-injected vars
     * and sshd state must not leak into the container.
     */
    if (clearenv() != 0) {
        fprintf(stderr, "[CaaLsh] clearenv failed\n");
        toml_free(config);
        return 1;
    }

    /* give crun the bare minimum PATH it needs; nothing else survives */
    if (setenv("PATH",
               "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
               1)) {
        fprintf(stderr, "[CaaLsh] setenv failed\n");
        toml_free(config);
        return 1;
    }

    /* unique container ID scoped to this PID and timestamp */
    char container_id[64];
    snprintf(container_id, sizeof(container_id), "caalsh-%d-%ld", (int)getpid(),
             (long)time(NULL));

    /*
     * Set up the per-session overlay filesystem.
     *
     * A loop-mounted ext4 image under SESSION_DIR provides the upper and work
     * directories. The bundle rootfs is the read-only lower layer. All writes
     * go to the loop image and are discarded when the session ends, leaving
     * the rootfs clean for the next login.
     */
    char session_dir[128], image_path[128], upper[160], work[160], rootfs[128];
    snprintf(session_dir, sizeof(session_dir), SESSION_DIR "/%s", container_id);
    snprintf(image_path, sizeof(image_path), SESSION_DIR "/%s.img",
             container_id);
    snprintf(upper, sizeof(upper), SESSION_DIR "/%s/upper", container_id);
    snprintf(work, sizeof(work), SESSION_DIR "/%s/work", container_id);
    snprintf(rootfs, sizeof(rootfs), "%s/rootfs", bundle_path);

    if (session_disk_setup(session_dir, image_path, disk_size_mb) != 0) {
        fprintf(stderr, "[CaaLsh] session disk setup failed\n");
        toml_free(config);
        return 1;
    }

    char overlay_opts[512];
    snprintf(overlay_opts, sizeof(overlay_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s", rootfs, upper, work);

    if (mount("overlay", rootfs, "overlay", 0, overlay_opts) != 0) {
        fprintf(stderr, "[CaaLsh] overlay mount failed\n");
        session_disk_cleanup(session_dir, image_path);
        toml_free(config);
        return 1;
    }

    /* console socket lets crun hand us the PTY master fd */
    char sock_path[108];
    int sock_fd = pty_bridge_init(sock_path, sizeof(sock_path));
    if (sock_fd < 0) {
        fprintf(stderr, "[CaaLsh] pty_bridge_init failed\n");
        toml_free(config);
        return 1;
    }

    /*
     * Fork. The child execs into crun and becomes the container process.
     * The parent waits for it to finish, then runs cleanup unconditionally,
     * regardless of how the session ended.
     */
    pid_t pid = fork();

    if (pid == 0) {
        /* child: exec into crun, replacing this process image */
        char *const argv[] = {
            "crun",          "run",     "--bundle",   (char *)bundle_path,
            "--console-socket", sock_path, container_id, NULL};
        execvp("crun", argv);
        fprintf(stderr, "[CaaLsh] exec failed\n");
        _exit(1);
    } else if (pid < 0) {
        fprintf(stderr, "[CaaLsh] fork failed\n");
        cleanup(rootfs, session_dir, image_path, sock_path, container_id);
        toml_free(config);
        return 1;
    }

    child_pid = pid;

    /* register the session with the daemon if it is available */
    int daemon_fd = caald_connect();
    if (daemon_fd >= 0) {
        if (!caald_session_register(daemon_fd, username, container_id,
                                    getpid())) {
            fprintf(stderr,
                    "[CaaLsh] daemon register failed (is it running?)\n");
        }
        close(daemon_fd);
    }

    /*
     * Arm the timeout process if one was configured. When the timer expires
     * it sends SIGTERM to the parent (us), which forces the session to end.
     */
    if (timeout > 0) {
        pid_t ppid = getpid();
        timer_pid = fork();
        if (timer_pid == 0) {
            sleep((unsigned int)timeout);
            if (!kill(ppid, 0))
                kill(ppid, SIGTERM);
            _exit(0);
        }
    }

    int master_fd = pty_bridge_recv(sock_fd);
    if (master_fd < 0) {
        fprintf(stderr, "[CaaLsh] pty_bridge_recv failed\n");
        cleanup(rootfs, session_dir, image_path, sock_path, container_id);
        return 1;
    }
    pty_bridge_run(master_fd, pid);

    if (timer_pid > 0)
        kill(timer_pid, SIGKILL);

    cleanup(rootfs, session_dir, image_path, sock_path, container_id);
    return 0;
}