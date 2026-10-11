
#ifndef VFS_PATH_H
#define VFS_PATH_H

#include "vfs_format.h"
#include "vfs_perm.h"

#include <stdint.h>

/*
 * Resolve an absolute filesystem path to an inode number.
 *
 * This legacy variant does not enforce permissions.
 * Returns 0 on success, or -1 on failure with errno set.
 */
int vfs_path_resolve(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    uint64_t *result_inode_number
);

/*
 * Resolve an absolute path while enforcing execute permission
 * on every directory traversed.
 *
 * The credentials contain the caller's UID and primary GID.
 * Returns 0 on success, or -1 on failure with errno set.
 */
int vfs_path_resolve_as(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    const struct vfs_credentials *credentials,
    uint64_t *result_inode_number
);

#endif