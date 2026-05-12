/*
 * Copyright (C) 2026 douxxtech
 * session_disk: per-session ext4 loop image setup and teardown
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/falloc.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "session_disk.h"

int session_disk_setup(const char *session_dir, const char *image_path,
                       int64_t size_mb) {
    if (mkdir(session_dir, 0700) != 0)
        return -1;

    /* create and fully allocate the image; fallocate ensures real disk blocks
     * are reserved upfront so the loop device has a hard size cap */
    int fd = open(image_path, O_CREAT | O_WRONLY | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    if (fallocate(fd, 0, 0, size_mb * 1024 * 1024) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    /* format it */
    pid_t p = fork();
    if (p == 0) {
        char *const argv[] = {"mkfs.ext4", "-q", (char *)image_path, NULL};
        execvp("mkfs.ext4", argv);
        _exit(1);
    }
    if (p < 0)
        return -1;
    int status;
    waitpid(p, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    /* mount loopback via mount command since the kernel needs the loop device
     * set up first */
    pid_t m = fork();
    if (m == 0) {
        setuid(0); // otherwise mount will fail
        setgid(0);
        char *const argv[] = {"mount",
                              "-o",
                              "loop,nodev,nosuid",
                              (char *)image_path,
                              (char *)session_dir,
                              NULL};
        execvp("mount", argv);
        _exit(1);
    }
    if (m < 0)
        return -1;
    waitpid(m, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    /* create upper and work inside the loop image, after it's mounted */
    char upper[256], work[256];
    snprintf(upper, sizeof(upper), "%s/upper", session_dir);
    snprintf(work, sizeof(work), "%s/work", session_dir);
    if (mkdir(upper, 0700) != 0 || mkdir(work, 0700) != 0)
        return -1;

    return 0;
}

void session_disk_cleanup(const char *session_dir, const char *image_path) {
    umount2(session_dir, MNT_DETACH);
    unlink(image_path);

    pid_t p = fork();
    if (p == 0) {
        char *const argv[] = {"/bin/rm", "-rf", (char *)session_dir, NULL};
        execvp("rm", argv);
        _exit(1);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}