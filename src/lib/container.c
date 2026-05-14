/*
 * Copyright (C) 2026 douxxtech
 * container: general container manager
 */

#include "container.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "lib/caald_client.h"
#include "lib/pty_bridge.h"
#include "lib/session_disk.h"

/*
 * Synchronously force-delete a container.
 *
 * Forks a child so we can waitpid() cleanly without interfering with the
 * caller's child tracking.
 */
static void crun_delete(const char *container_id) {
    pid_t p = fork();
    if (p == 0) {
        char *const argv[] = {"crun", "delete", "--force", (char *)container_id,
                              NULL};
        execvp("crun", argv);
        _exit(1);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

int container_start(container_t *ct, const char *bundle_path,
                    int64_t disk_size_mb) {
    /* unique container ID */
    snprintf(ct->container_id, sizeof(ct->container_id), "caalsh-%d-%ld",
             (int)getpid(), (long)time(NULL));

    /* path derivations */
    snprintf(ct->session_dir, sizeof(ct->session_dir), SESSION_DIR "/%s",
             ct->container_id);
    snprintf(ct->image_path, sizeof(ct->image_path), SESSION_DIR "/%s.img",
             ct->container_id);
    snprintf(ct->rootfs, sizeof(ct->rootfs), "%s/rootfs", bundle_path);

    char upper[160], work[160];
    snprintf(upper, sizeof(upper), SESSION_DIR "/%s/upper", ct->container_id);
    snprintf(work, sizeof(work), SESSION_DIR "/%s/work", ct->container_id);

    /* overlay filesystem */
    if (session_disk_setup(ct->session_dir, ct->image_path, disk_size_mb) !=
        0) {
        fprintf(stderr, "[CaaLsh] session disk setup failed\n");
        return -1;
    }

    char overlay_opts[512];
    snprintf(overlay_opts, sizeof(overlay_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s", ct->rootfs, upper, work);

    if (mount("overlay", ct->rootfs, "overlay", 0, overlay_opts) != 0) {
        fprintf(stderr, "[CaaLsh] overlay mount failed\n");
        session_disk_cleanup(ct->session_dir, ct->image_path);
        return -1;
    }

    /* console socket */
    int sock_fd = pty_bridge_init(ct->sock_path, sizeof(ct->sock_path));
    if (sock_fd < 0) {
        fprintf(stderr, "[CaaLsh] pty_bridge_init failed\n");
        goto err_mounted;
    }

    /* launch crun */
    ct->child_pid = fork();
    if (ct->child_pid == 0) {
        char *const argv[] = {"crun",
                              "run",
                              "--bundle",
                              (char *)bundle_path,
                              "--console-socket",
                              ct->sock_path,
                              ct->container_id,
                              NULL};
        execvp("crun", argv);
        fprintf(stderr, "[CaaLsh] exec failed\n");
        _exit(1);
    }
    if (ct->child_pid < 0) {
        fprintf(stderr, "[CaaLsh] fork failed\n");
        goto err_sock;
    }

    /* wire PTY */
    return sock_fd;

    /* errors, I'm not a fan of gotos but meh */
err_sock:
    unlink(ct->sock_path);
err_mounted:
    umount2(ct->rootfs, MNT_DETACH);
    session_disk_cleanup(ct->session_dir, ct->image_path);
    return -1;
}

void container_stop(container_t *ct) {
    /* kill the watchdog first so it cannot race us */

    if (ct->sock_path[0])
        unlink(ct->sock_path);

    if (ct->rootfs[0])
        umount2(ct->rootfs, MNT_DETACH);

    if (ct->session_dir[0] && ct->image_path[0])
        session_disk_cleanup(ct->session_dir, ct->image_path);

    if (ct->container_id[0])
        crun_delete(ct->container_id);

    /* zero the struct so a double-stop is a no-op */
    memset(ct, 0, sizeof(*ct));
}