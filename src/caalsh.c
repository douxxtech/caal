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

/* child pids, needs to be global so the signal handlers can reach them */
static pid_t child_pid = -1;
static pid_t timer_pid = -1;

/*
 * Cleanup. Unmount the overlay, tear down the session disk image, then
 * delete the container state via crun. Fork a child for crun delete so
 * we can wait on it and return cleanly.
 */
static void cleanup(const char *rootfs, const char *session_dir,
                    const char *image_path, const char *sock_path,
                    const char *container_id) {
    unlink(sock_path);
    umount2(rootfs, MNT_DETACH);
    session_disk_cleanup(session_dir, image_path);

    pid_t p = fork();
    if (p == 0) {
        char *const argv[] = {CRUN_PATH, "delete", "--force",
                              (char *)container_id, NULL};
        execv(CRUN_PATH, argv);
        _exit(1);
    }
    if (p > 0)
        waitpid(p, NULL, 0);

    int daemon_fd = caald_connect();
    if (daemon_fd >= 0) {
        caald_session_unregister(daemon_fd, container_id);
        close(daemon_fd);
    }
}

/* try to get session count from daemon, fall back to dir counting if it fails
 */
static int count_active_sessions(void) {
    int fd = caald_connect();
    if (fd >= 0) {
        int count = caald_session_count(fd);
        close(fd);
        if (count >= 0)
            return count;
        /* daemon responded but gave error, warn and fall back */
        fprintf(stderr, "[CaaLsh] daemon query failed, falling back\n");
    }

    /* daemon not available or failed, count dirs */
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
    /* Figure out who we are */
    struct passwd *pw = getpwuid(getuid());
    if (pw == NULL) {
        fprintf(stderr, "[CaaLsh] could not resolve user\n");
        return 1;
    }
    const char *username = pw->pw_name;

    /* sanity cap before we use username in any snprintf below */
    if (strlen(username) > 32) {
        fprintf(stderr, "[CaaLsh] username too long\n");
        return 1;
    }

    /*
     * Open and parse the main config file;
     * Every user that is allowed to log in must have a section in there.
     * No section = No access
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

    /*
     * Read global max_sessions.
     * zero or missing means unlimited
     */
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
     * Look up [username].bundle using dot path syntax.
     * This is the OCI bundle directory crun will be pointed at
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

    /* relative paths are a footgun, require absolute only */
    if (bundle_path[0] != '/') {
        fprintf(stderr, "[CaaLsh] bundle path must be absolute\n");
        toml_free(config);
        return 1;
    }

    /* Check the enabled flag. If it is explicitly set to false we bail out */
    char enabled_path[128];
    snprintf(enabled_path, sizeof(enabled_path), "%s.enabled", username);
    toml_datum_t enabled_datum = toml_seek(config.toptab, enabled_path);
    if (enabled_datum.type == TOML_BOOLEAN && !enabled_datum.u.boolean) {
        fprintf(stderr, "[CaaLsh] user disabled, access denied\n");
        toml_free(config);
        return 1;
    }

    /*
     * Read the session timeout in seconds.
     * Zero or missing means no timeout
     */
    char timeout_path[128];
    snprintf(timeout_path, sizeof(timeout_path), "%s.timeout", username);
    toml_datum_t timeout_datum = toml_seek(config.toptab, timeout_path);
    int64_t timeout = 0;
    if (timeout_datum.type == TOML_INT64)
        timeout = timeout_datum.u.int64;

    /*
     * Read the session disk size in gigabytes.
     * Defaults to 1GB if not set or zero.
     */
    char disk_size_path[128];
    snprintf(disk_size_path, sizeof(disk_size_path), "%s.disk", username);
    toml_datum_t disk_size_datum = toml_seek(config.toptab, disk_size_path);
    int64_t disk_size_mb = 1024; /* default to 1GB */
    if (disk_size_datum.type == TOML_INT64 && disk_size_datum.u.int64 > 0)
        disk_size_mb = disk_size_datum.u.int64;

    /*
     * Nuke the entire environment before we launch crun.
     * We dont want any SSH injected vars or sshd state leaking into
     * the container.
     */
    if (clearenv() != 0) {
        fprintf(stderr, "[CaaLsh] clearenv failed\n");
        toml_free(config);
        return 1;
    }

    /*
     * Give crun the minimum it needs to run.
     * Nothing from the original env survives past this point.
     */
    if (setenv("PATH",
               "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
               1)) {
        fprintf(stderr, "[CaaLsh] setenv failed\n");
        toml_free(config);
        return 1;
    }

    /* Build a unique container ID using our PID and current timestamp */
    char container_id[64];
    snprintf(container_id, sizeof(container_id), "caalsh-%d-%ld", (int)getpid(),
             (long)time(NULL));

    /*
     * Set up the per-session overlay filesystem.
     *
     * We create a loop-mounted ext4 image under SESSION_DIR and use it as
     * the overlay scratch space. upper gets all the writes, work is required
     * by overlayfs. When the session ends the image is unmounted and deleted,
     * so the container rootfs is always clean for the next login.
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

    /*
     * Mount the overlay. The loop-mounted image provides upper and work,
     * the bundle rootfs is the read-only lower layer. All writes go to the
     * loop image and are discarded when the session ends.
     */
    char overlay_opts[512];
    snprintf(overlay_opts, sizeof(overlay_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s", rootfs, upper, work);

    if (mount("overlay", rootfs, "overlay", 0, overlay_opts) != 0) {
        fprintf(stderr, "[CaaLsh] overlay mount failed\n");
        session_disk_cleanup(session_dir, image_path);
        toml_free(config);
        return 1;
    }

    /* set up the console socket so crun can hand us the PTY master */
    char sock_path[108];
    int sock_fd = pty_bridge_init(sock_path, sizeof(sock_path));
    if (sock_fd < 0) {
        fprintf(stderr, "[CaaLsh] pty_bridge_init failed\n");
        toml_free(config);
        return 1;
    }

    /*
     * Fork here. The child execs into crun and becomes the container process.
     * The parent waits for it to finish, then handles cleanup (unmounts,
     * session disk removal, crun delete).
     *
     * We do it this way instead of just execv so we can always run cleanup,
     * regardless of how the session ends.
     */
    pid_t pid = fork();

    if (pid == 0) {
        /* child: hand off to crun, we are done here */
        char *const argv[] = {
            CRUN_PATH,          "run",     "--bundle",   (char *)bundle_path,
            "--console-socket", sock_path, container_id, NULL};
        execv(CRUN_PATH, argv);
        fprintf(stderr, "[CaaLsh] exec failed\n");
        _exit(1);
    } else if (pid < 0) {
        fprintf(stderr, "[CaaLsh] fork failed\n");
        cleanup(rootfs, session_dir, image_path, sock_path, container_id);
        toml_free(config);
        return 1;
    }

    /*
     * Parent: arm the timeout process if one was configured, then block on the
     * child.
     */
    child_pid = pid;

    int daemon_fd = caald_connect();
    if (daemon_fd >= 0) {
        if (!caald_session_register(daemon_fd, username, container_id,
                                    getpid())) {
            fprintf(stderr, "[CaaLsh] daemon register failed\n");
        }
        close(daemon_fd);
    }

    if (timeout > 0) {
        timer_pid = fork();
        if (timer_pid == 0) {
            sleep((unsigned int)timeout);
            kill(child_pid, SIGTERM);
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