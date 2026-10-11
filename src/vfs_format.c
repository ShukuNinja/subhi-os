#include "vfs_format.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define BITS_PER_BITMAP_BLOCK (VFS_BLOCK_SIZE * 8ULL)

static uint64_t calculate_bitmap_blocks(uint64_t total_blocks)
{
    return (total_blocks + BITS_PER_BITMAP_BLOCK - 1) /
           BITS_PER_BITMAP_BLOCK;
}

static uint64_t calculate_inode_table_blocks(void)
{
    uint64_t bytes = (uint64_t)VFS_INODE_COUNT * VFS_INODE_SIZE;

    return (bytes + VFS_BLOCK_SIZE - 1) / VFS_BLOCK_SIZE;
}

static int calculate_layout(
    uint64_t total_blocks,
    struct vfs_superblock *sb
)
{
    if (sb == NULL || total_blocks == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(sb, 0, sizeof(*sb));

    memcpy(sb->magic, VFS_MAGIC, VFS_MAGIC_SIZE);
    sb->version = VFS_FORMAT_VERSION;
    sb->block_size = VFS_BLOCK_SIZE;
    sb->total_blocks = total_blocks;

    sb->bitmap_start = 1;
    sb->bitmap_blocks = calculate_bitmap_blocks(total_blocks);

    sb->inode_table_start = sb->bitmap_start + sb->bitmap_blocks;
    sb->inode_table_blocks = calculate_inode_table_blocks();
    sb->inode_count = VFS_INODE_COUNT;

    sb->data_start =
        sb->inode_table_start + sb->inode_table_blocks;

    sb->root_inode = 0;

    if (sb->data_start >= total_blocks) {
        errno = ENOSPC;
        return -1;
    }

    sb->free_blocks = total_blocks - sb->data_start;

    return 0;
}

static int initialize_bitmap(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb
)
{
    unsigned char buffer[VFS_BLOCK_SIZE];

    for (uint64_t i = 0; i < sb->bitmap_blocks; i++) {
        memset(buffer, 0, sizeof(buffer));

        uint64_t first_block = i * BITS_PER_BITMAP_BLOCK;

        for (uint64_t bit = 0; bit < BITS_PER_BITMAP_BLOCK; bit++) {
            uint64_t block_number = first_block + bit;

            if (block_number >= sb->total_blocks) {
                /*
                 * Bits beyond the end of the disk are unavailable.
                 * Mark them allocated so the allocator cannot use them.
                 */
                buffer[bit / 8] |=
                    (unsigned char)(1U << (bit % 8));
            } else if (block_number < sb->data_start) {
                /* Reserve all filesystem metadata blocks. */
                buffer[bit / 8] |=
                    (unsigned char)(1U << (bit % 8));
            }
        }

        if (vfs_disk_write_block(
                disk, sb->bitmap_start + i, buffer) < 0) {
            return -1;
        }
    }

    return 0;
}

static int initialize_inode_table(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb
)
{
    unsigned char zero_block[VFS_BLOCK_SIZE] = {0};

    for (uint64_t i = 0; i < sb->inode_table_blocks; i++) {
        if (vfs_disk_write_block(
                disk, sb->inode_table_start + i,
                zero_block) < 0) {
            return -1;
        }
    }

    return 0;
}

int vfs_format_disk(struct vfs_disk *disk)
{
    if (disk == NULL || disk->fd < 0 ||
        disk->total_blocks == 0) {
        errno = EINVAL;
        return -1;
    }

    struct vfs_superblock sb;

    if (calculate_layout(disk->total_blocks, &sb) < 0) {
        return -1;
    }

    /*
     * Initialize metadata first. Write the superblock last so a
     * partially completed format is not mistaken for a valid one.
     */
    if (initialize_bitmap(disk, &sb) < 0) {
        return -1;
    }

    if (initialize_inode_table(disk, &sb) < 0) {
        return -1;
    }

    /*
     * Make initialized metadata durable before publishing the
     * superblock that identifies this image as formatted.
     */
    if (vfs_disk_flush(disk) < 0) {
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE] = {0};

    memcpy(buffer, &sb, sizeof(sb));

    if (vfs_disk_write_block(disk, 0, buffer) < 0) {
        return -1;
    }

    return vfs_disk_flush(disk);
}

int vfs_read_superblock(
    struct vfs_disk *disk,
    struct vfs_superblock *superblock
)
{
    if (disk == NULL || disk->fd < 0 || superblock == NULL) {
        errno = EINVAL;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(disk, 0, buffer) < 0) {
        return -1;
    }

    struct vfs_superblock sb;
    memcpy(&sb, buffer, sizeof(sb));

    if (memcmp(sb.magic, VFS_MAGIC, VFS_MAGIC_SIZE) != 0 ||
        sb.version != VFS_FORMAT_VERSION ||
        sb.block_size != VFS_BLOCK_SIZE ||
        sb.total_blocks != disk->total_blocks ||
        sb.bitmap_start != 1 ||
        sb.bitmap_blocks != calculate_bitmap_blocks(sb.total_blocks) ||
        sb.inode_table_start != sb.bitmap_start + sb.bitmap_blocks ||
        sb.inode_table_blocks != calculate_inode_table_blocks() ||
        sb.inode_count != VFS_INODE_COUNT ||
        sb.data_start != sb.inode_table_start + sb.inode_table_blocks ||
        sb.data_start >= sb.total_blocks ||
        sb.free_blocks != sb.total_blocks - sb.data_start ||
        sb.root_inode >= sb.inode_count) {
        errno = EINVAL;
        return -1;
    }

    *superblock = sb;
    return 0;
}
