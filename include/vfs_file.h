
#ifndef VFS_FILE_H
#define VFS_FILE_H

#include "vfs_inode.h"
#include "vfs_perm.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Create an empty regular file at an absolute path.
 * mode, uid, and gid describe the new file.
 * credentials describe the caller.
 * result_inode_number may be NULL.
 */
int vfs_file_create(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    uint32_t mode,
    uint32_t uid,
    uint32_t gid,
    const struct vfs_credentials *credentials,
    uint64_t *result_inode_number
);

/*
 * Read from the beginning of a regular file.
 * Requires read permission on the file.
 */
int vfs_file_read(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    void *buffer,
    size_t buffer_capacity,
    size_t *bytes_read,
    const struct vfs_credentials *credentials
);

/*
 * Replace the entire contents of a regular file.
 * Requires write permission on the file.
 * Maximum supported file size is 32 KiB.
 */
int vfs_file_write(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    const void *buffer,
    size_t size,
    const struct vfs_credentials *credentials
);

/*
 * Remove a regular file and reclaim its resources.
 * Requires write and execute permission on the parent directory.
 */
int vfs_file_unlink(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    const struct vfs_credentials *credentials
);

#endif