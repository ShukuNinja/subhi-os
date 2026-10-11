#include "vfs_alloc.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include <stddef.h>
#include <errno.h>
#include <stdint.h>

#define BITS_PER_BLOCK (VFS_BLOCK_SIZE * 8ULL)

static int read_valid_superblock(
    struct vfs_disk *disk,
    struct vfs_superblock *sb
)
{
    if (disk == NULL || disk->fd < 0 || sb == NULL) {
        errno = EINVAL;
        return -1;
    }

    return vfs_read_superblock(disk, sb);
}

static int read_bitmap_block(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t block_number,
    unsigned char *buffer
)
{
    uint64_t bitmap_index = block_number / BITS_PER_BLOCK;

    if (bitmap_index >= sb->bitmap_blocks) {
        errno = EINVAL;
        return -1;
    }

    return vfs_disk_read_block(
        disk, sb->bitmap_start + bitmap_index, buffer);
}

int vfs_block_is_allocated(
    struct vfs_disk *disk,
    uint64_t block_number
)
{
    struct vfs_superblock sb;

    if (read_valid_superblock(disk, &sb) < 0) {
        return -1;
    }

    if (block_number >= sb.total_blocks) {
        errno = EINVAL;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (read_bitmap_block(disk, &sb, block_number, buffer) < 0) {
        return -1;
    }

    uint64_t bit_index = block_number % BITS_PER_BLOCK;
    unsigned char mask =
        (unsigned char)(1U << (bit_index % 8));

    return (buffer[bit_index / 8] & mask) != 0;
}

/*
 * Check whether a live inode references a block through a direct
 * pointer. The root directory is included in this check.
 */
static int block_is_referenced(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t block_number
)
{
    for (uint64_t i = 0; i < sb->inode_count; i++) {
        int allocated = vfs_inode_is_allocated(disk, sb, i);

        if (allocated < 0) {
            return -1;
        }

        if (allocated == 0) {
            continue;
        }

        struct vfs_inode inode;

        if (vfs_inode_read(disk, sb, i, &inode) < 0) {
            return -1;
        }

        for (uint64_t j = 0; j < VFS_DIRECT_BLOCKS; j++) {
            if (inode.direct_blocks[j] == block_number) {
                return 1;
            }
        }
    }

    return 0;
}

int vfs_block_allocate(
    struct vfs_disk *disk,
    uint64_t *block_number
)
{
    if (block_number == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct vfs_superblock sb;

    if (read_valid_superblock(disk, &sb) < 0) {
        return -1;
    }

    if (sb.free_blocks == 0) {
        errno = ENOSPC;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    for (uint64_t block = sb.data_start;
         block < sb.total_blocks; block++) {
        if (read_bitmap_block(disk, &sb, block, buffer) < 0) {
            return -1;
        }

        uint64_t bit_index = block % BITS_PER_BLOCK;
        unsigned char mask =
            (unsigned char)(1U << (bit_index % 8));

        if ((buffer[bit_index / 8] & mask) != 0) {
            continue;
        }

        /* Mark the selected block allocated. */
        buffer[bit_index / 8] |= mask;

        uint64_t bitmap_index = block / BITS_PER_BLOCK;

        if (vfs_disk_write_block(
                disk, sb.bitmap_start + bitmap_index,
                buffer) < 0) {
            return -1;
        }

        sb.free_blocks--;

        if (vfs_write_superblock(disk, &sb) < 0) {
            int saved_errno = errno;

            /* Best-effort rollback of the bitmap update. */
            if (vfs_disk_read_block(
                    disk, sb.bitmap_start + bitmap_index,
                    buffer) == 0) {
                buffer[bit_index / 8] &= (unsigned char)~mask;
                (void)vfs_disk_write_block(
                    disk, sb.bitmap_start + bitmap_index, buffer);
            }

            errno = saved_errno;
            return -1;
        }

        *block_number = block;
        return 0;
    }

    errno = ENOSPC;
    return -1;
}

int vfs_block_release(
    struct vfs_disk *disk,
    uint64_t block_number
)
{
    struct vfs_superblock sb;

    if (read_valid_superblock(disk, &sb) < 0) {
        return -1;
    }

    if (block_number < sb.data_start ||
        block_number >= sb.total_blocks) {
        errno = EINVAL;
        return -1;
    }

    int allocated = vfs_block_is_allocated(disk, block_number);

    if (allocated < 0) {
        return -1;
    }

    if (allocated == 0) {
        errno = EALREADY;
        return -1;
    }

    int referenced = block_is_referenced(disk, &sb, block_number);

    if (referenced < 0) {
        return -1;
    }

    if (referenced != 0) {
        errno = EBUSY;
        return -1;
    }

    if (sb.free_blocks >= sb.total_blocks - sb.data_start) {
        errno = EINVAL;
        return -1;
    }

    uint64_t bitmap_index = block_number / BITS_PER_BLOCK;
    uint64_t bit_index = block_number % BITS_PER_BLOCK;
    unsigned char mask =
        (unsigned char)(1U << (bit_index % 8));

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk, sb.bitmap_start + bitmap_index, buffer) < 0) {
        return -1;
    }

    buffer[bit_index / 8] &= (unsigned char)~mask;

    if (vfs_disk_write_block(
            disk, sb.bitmap_start + bitmap_index, buffer) < 0) {
        return -1;
    }

    sb.free_blocks++;

    if (vfs_write_superblock(disk, &sb) < 0) {
        int saved_errno = errno;

        /* Best-effort rollback: mark the block allocated again. */
        if (vfs_disk_read_block(
                disk, sb.bitmap_start + bitmap_index,
                buffer) == 0) {
            buffer[bit_index / 8] |= mask;
            (void)vfs_disk_write_block(
                disk, sb.bitmap_start + bitmap_index, buffer);
        }

        errno = saved_errno;
        return -1;
    }

    return 0;
}
