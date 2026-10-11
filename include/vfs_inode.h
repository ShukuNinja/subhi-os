#ifndef VFS_INODE_H
#define VFS_INODE_H

#include "vfs_format.h"

#include <stdint.h>

#define VFS_DIRECT_BLOCKS 8U

enum vfs_inode_type {
    VFS_INODE_FREE = 0,
    VFS_INODE_REGULAR = 1,
    VFS_INODE_DIRECTORY = 2
};

struct vfs_inode {
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;

    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;

    uint32_t link_count;
    uint32_t blocks_count;

    uint64_t direct_blocks[VFS_DIRECT_BLOCKS];
    uint64_t reserved;
};

_Static_assert(sizeof(struct vfs_inode) == VFS_INODE_SIZE,
               "inode structure must match on-disk inode size");

int vfs_inode_read(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number,
    struct vfs_inode *inode
);

int vfs_inode_write(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number,
    const struct vfs_inode *inode
);

/* Returns 1 if allocated, 0 if free, and -1 on error. */
int vfs_inode_is_allocated(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number
);

int vfs_inode_allocate(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    enum vfs_inode_type type,
    uint32_t mode,
    uint32_t uid,
    uint32_t gid,
    uint64_t *inode_number
);

/* Only empty, non-root inodes can be released. */
int vfs_inode_release(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number
);

#endif
