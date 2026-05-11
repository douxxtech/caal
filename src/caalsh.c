#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "lib/tomlc17.h"

/* pid of the crun child, used by the signal handler */
static pid_t child_pid = -1;

static void on_alarm(int sig) {
    (void)sig;
    if (child_pid > 0)
        kill(child_pid, SIGKILL);
}

int main(void) {
    /*
     * Resolve who is logging in
     * getpwuid(getuid()) reads directly from /etc/passwd
     */
    struct passwd *pw = getpwuid(getuid());
    if (pw == NULL) {
        write(STDERR_FILENO, "[CaaLsh] could not resolve user\n", 30);
        return 1;
    }
    const char *username = pw->pw_name;

    /* guard against long usernames before we use it in snprintf */
    if (strlen(username) > 32) {
        write(STDERR_FILENO, "[CaaLsh] username too long\n", 25);
        return 1;
    }

    /*
     * Parse /etc/caal/caal.toml and look up [<username>].
     * If the user has no entry, we refuse the login entirely.
     */
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (fp == NULL) {
        write(STDERR_FILENO, "[CaaLsh] could not open config\n", 29);
        return 1;
    }

    toml_result_t config = toml_parse_file(fp);
    fclose(fp);
    if (!config.ok) {
        write(STDERR_FILENO, "[CaaLsh] could not parse config\n", 30);
        return 1;
    }

    /* look up [<username>].bundle via dot-path */
    char seek_path[128];
    snprintf(seek_path, sizeof(seek_path), "%s.bundle", username);

    toml_datum_t bundle_datum = toml_seek(config.toptab, seek_path);
    if (bundle_datum.type == TOML_UNKNOWN) {
        write(STDERR_FILENO, "[CaaLsh] user not configured, access denied\n",
              42);
        toml_free(config);
        return 1;
    }
    if (bundle_datum.type != TOML_STRING) {
        write(STDERR_FILENO, "[CaaLsh] bundle is not a string\n", 30);
        toml_free(config);
        return 1;
    }

    const char *bundle_path = bundle_datum.u.s;

    /* reject relative paths */
    if (bundle_path[0] != '/') {
        write(STDERR_FILENO, "[CaaLsh] bundle path must be absolute\n", 36);
        toml_free(config);
        return 1;
    }

    /* check enabled flag  */
    char enabled_path[128];
    snprintf(enabled_path, sizeof(enabled_path), "%s.enabled", username);
    toml_datum_t enabled_datum = toml_seek(config.toptab, enabled_path);
    if (enabled_datum.type == TOML_BOOLEAN && !enabled_datum.u.boolean) {
        write(STDERR_FILENO, "[CaaLsh] user disabled, access denied\n", 36);
        toml_free(config);
        return 1;
    }

    /* check timeout */
    char timeout_path[128];
    snprintf(timeout_path, sizeof(timeout_path), "%s.timeout", username);
    toml_datum_t timeout_datum = toml_seek(config.toptab, timeout_path);
    int64_t timeout = 0;
    if (timeout_datum.type == TOML_INT64)
        timeout = timeout_datum.u.int64;

    /*
     * Nuke the entire environment
     * We don't want anything the SSH client or sshd injected to survive into
     * crun
     */
    if (clearenv() != 0) {
        write(STDERR_FILENO, "[CaaLsh] clearenv failed\n", 23);
        toml_free(config);
        return 1;
    }

    /*
     * Give crun the bare minimum env it needs.
     * Do NOT forward anything from the original env.
     */
    if (setenv("PATH",
               "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
               1)) {
        write(STDERR_FILENO, "[CaaLsh] setenv failed\n", 21);
        toml_free(config);
        return 1;
    }

    /*
     * Build a unique container ID
     * PID + timestamp to avoid collisions
     */
    char container_id[64];
    snprintf(container_id, sizeof(container_id), "caalsh-%d-%ld", (int)getpid(),
             (long)time(NULL));

    /*
     * Set up a tmpfs for this session's overlay scratch space
     * upper: writes go here, work: required dir for overlayfs
     * The tmpfs is unmounted after the session ends, wiping everything away
     */
    char tmp_dir[96], upper[128], work[128], rootfs[128], overlay_opts[512];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/%s", container_id);
    snprintf(upper, sizeof(upper), "/tmp/%s/upper", container_id);
    snprintf(work, sizeof(work), "/tmp/%s/work", container_id);
    snprintf(rootfs, sizeof(rootfs), "%s/rootfs", bundle_path);

    mkdir(tmp_dir, 0700);
    mount("tmpfs", tmp_dir, "tmpfs", 0, "size=100m");
    mkdir(upper, 0700);
    mkdir(work, 0700);

    snprintf(overlay_opts, sizeof(overlay_opts),
             "lowerdir=%s,upperdir=%s,workdir=%s", rootfs, upper, work);

    if (mount("overlay", rootfs, "overlay", 0, overlay_opts) != 0) {
        write(STDERR_FILENO, "[CaaLsh] overlay mount failed\n", 28);
        toml_free(config);
        return 1;
    }

    /*
     * Fork so the parent can clean up the container after the session ends.
     * The child execs into crun and becomes the container process.
     * The parent waits, then runs `crun delete --force` to wipe the state.
     */
    pid_t pid = fork();

    if (pid == 0) {
        /* becomes crun */
        char *const argv[] = {CRUN_PATH,           "run",        "--bundle",
                              (char *)bundle_path, container_id, NULL};
        execv(CRUN_PATH, argv);
        write(STDERR_FILENO, "[CaaLsh] exec failed\n", 19);
        _exit(1);
    } else if (pid < 0) {
        write(STDERR_FILENO, "[CaaLsh] fork failed\n", 19);
        toml_free(config);
        return 1;
    }

    /* arm the timeout if configured, then wait */
    child_pid = pid;
    if (timeout > 0) {
        signal(SIGALRM, on_alarm);
        alarm((unsigned int)timeout);
    }

    waitpid(pid, NULL, 0);
    alarm(0); /* cancel any pending alarm if session ended naturally */

    /* unmount overlay, then tmpfs */
    umount(rootfs);
    umount(tmp_dir);
    rmdir(tmp_dir);

    /* delete the container */
    char *const del_argv[] = {CRUN_PATH, "delete", "--force", container_id,
                              NULL};
    execv(CRUN_PATH, del_argv);

    write(STDERR_FILENO, "[CaaLsh] delete exec failed\n", 26);
    return 1;
}