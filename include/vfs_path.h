#ifndef VFS_PATH_H
#define VFS_PATH_H

#include "vfs_format.h"

#include <stdint.h>

/*
 * Resolve an absolute filesystem path to an inode number.
 *
 * Returns 0 on success, or -1 on failure with errno set.
 * The root path "/" resolves to sb->root_inode.
 */
int vfs_path_resolve(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    uint64_t *result_inode_number
);

#endif