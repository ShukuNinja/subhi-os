#ifndef VFS_FILE_H
#define VFS_FILE_H

#include "vfs_inode.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Create an empty regular file at an absolute path.
 *
 * mode, uid, and gid are stored in the new inode.
 * result_inode_number may be NULL if the inode number is not needed.
 *
 * Returns 0 on success, -1 on failure with errno set.
 */
int vfs_file_create(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    uint32_t mode,
    uint32_t uid,
    uint32_t gid,
    uint64_t *result_inode_number
);

/*
 * Read from the beginning of a regular file.
 *
 * Reads at most buffer_capacity bytes and reports the number
 * of bytes copied through bytes_read.
 *
 * A NULL buffer is permitted only when buffer_capacity is zero.
 * bytes_read must not be NULL.
 *
 * Returns 0 on success, -1 on failure with errno set.
 */
int vfs_file_read(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    void *buffer,
    size_t buffer_capacity,
    size_t *bytes_read
);

/*
 * Replace the entire contents of a regular file.
 *
 * A NULL buffer is permitted only when size is zero.
 * Maximum supported file size is 32 KiB.
 *
 * Returns 0 on success, -1 on failure with errno set.
 */
int vfs_file_write(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    const void *buffer,
    size_t size
);

/*
 * Remove a regular file and reclaim its data blocks and inode.
 *
 * Directories cannot be removed through this operation.
 * The path must be absolute.
 *
 * Returns 0 on success, -1 on failure with errno set.
 */
int vfs_file_unlink(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path
);

#endif
