#ifndef VFS_ALLOC_H
#define VFS_ALLOC_H

#include "vfs_disk.h"

#include <stdint.h>

/* Allocate one free data block. Returns 0 on success, -1 on error. */
int vfs_block_allocate(
    struct vfs_disk *disk,
    uint64_t *block_number
);

/* Release an allocated data block if no live inode references it. */
int vfs_block_release(
    struct vfs_disk *disk,
    uint64_t block_number
);

/* Returns 1 if allocated, 0 if free, and -1 on error. */
int vfs_block_is_allocated(
    struct vfs_disk *disk,
    uint64_t block_number
);

#endif
