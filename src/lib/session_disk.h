#ifndef SESSION_DISK_H
#define SESSION_DISK_H

#include <stdint.h>

/*
 * Set up a per-session ext4 loop-mounted disk image.
 * Creates session_dir, formats a sparse image at image_path sized size_gb,
 * and mounts it at session_dir with MS_NODEV|MS_NOSUID.
 * Returns 0 on success, -1 on error.
 */
int session_disk_setup(const char *session_dir, const char *image_path,
                       int64_t size_gb);

/*
 * Tear down a session disk. Unmounts session_dir, removes the image file,
 * and recursively removes session_dir.
 * Continues even if individual steps fail.
 */
void session_disk_cleanup(const char *session_dir, const char *image_path);

#endif /* SESSION_DISK_H */