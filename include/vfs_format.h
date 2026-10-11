#ifndef VFS_FORMAT_H
#define VFS_FORMAT_H

#include "vfs_disk.h"
#include <stddef.h>
#include <stdint.h>

#define VFS_MAGIC "SUBHIFS1"
#define VFS_MAGIC_SIZE 8U
#define VFS_FORMAT_VERSION 2U

#define VFS_INODE_SIZE 128U
#define VFS_INODE_COUNT 128U

struct vfs_superblock {
    char magic[VFS_MAGIC_SIZE];

    uint32_t version;
    uint32_t block_size;

    uint64_t total_blocks;

    uint64_t bitmap_start;
    uint64_t bitmap_blocks;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;

    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t inode_count;

    uint64_t data_start;
    uint64_t free_blocks;
    uint64_t root_inode;
};

int vfs_format_disk(struct vfs_disk *disk);

int vfs_read_superblock(
    struct vfs_disk *disk,
    struct vfs_superblock *superblock
);

int vfs_write_superblock(
    struct vfs_disk *disk,
    const struct vfs_superblock *superblock
);

#endif
