#include "vfs_format.h"
#include "vfs_inode.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define BITS_PER_BITMAP_BLOCK (VFS_BLOCK_SIZE * 8ULL)

static uint64_t bitmap_blocks_for(uint64_t count)
{
    return (count + BITS_PER_BITMAP_BLOCK - 1) /
           BITS_PER_BITMAP_BLOCK;
}

static uint64_t inode_table_blocks_for(void)
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
    sb->bitmap_blocks = bitmap_blocks_for(total_blocks);

    sb->inode_bitmap_start =
        sb->bitmap_start + sb->bitmap_blocks;
    sb->inode_bitmap_blocks =
        bitmap_blocks_for(VFS_INODE_COUNT);

    sb->inode_table_start =
        sb->inode_bitmap_start + sb->inode_bitmap_blocks;
    sb->inode_table_blocks = inode_table_blocks_for();
    sb->inode_count = VFS_INODE_COUNT;

    sb->data_start =
        sb->inode_table_start + sb->inode_table_blocks;
    sb->root_inode = 0;

    /*
     * The root directory needs one data block.
     */
    if (sb->data_start >= total_blocks) {
        errno = ENOSPC;
        return -1;
    }

    sb->free_blocks = total_blocks - sb->data_start - 1;

    return 0;
}

static int initialize_block_bitmap(
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

            if (block_number >= sb->total_blocks ||
                block_number <= sb->data_start) {
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

static int initialize_inode_bitmap(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb
)
{
    unsigned char buffer[VFS_BLOCK_SIZE] = {0};

    /* Reserve inode 0 for the root directory. */
    buffer[0] |= 1U;

    /* Inodes beyond the configured inode count are unavailable. */
    for (uint64_t i = sb->inode_count;
         i < sb->inode_bitmap_blocks * BITS_PER_BITMAP_BLOCK; i++) {
        buffer[i / 8] |= (unsigned char)(1U << (i % 8));
    }

    return vfs_disk_write_block(
        disk, sb->inode_bitmap_start, buffer);
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

    if (initialize_block_bitmap(disk, &sb) < 0 ||
        initialize_inode_bitmap(disk, &sb) < 0 ||
        initialize_inode_table(disk, &sb) < 0) {
        return -1;
    }

    /*
     * Initialize root inode 0. Its first direct block is reserved
     * for root-directory entries, which will be implemented later.
     */
    struct vfs_inode root = {0};
    root.type = VFS_INODE_DIRECTORY;
    root.mode = 0755;
    root.link_count = 1;
    root.blocks_count = 1;
    root.direct_blocks[0] = sb.data_start;

    if (vfs_inode_write(disk, &sb, sb.root_inode, &root) < 0) {
        return -1;
    }

    unsigned char zero_block[VFS_BLOCK_SIZE] = {0};

    if (vfs_disk_write_block(
            disk, sb.data_start, zero_block) < 0) {
        return -1;
    }

    /*
     * Persist all initialized metadata before publishing the
     * superblock as the filesystem's format marker.
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

    uint64_t expected_bitmap_blocks =
        bitmap_blocks_for(sb.total_blocks);
    uint64_t expected_inode_bitmap_blocks =
        bitmap_blocks_for(VFS_INODE_COUNT);

    uint64_t expected_inode_table_blocks =
        inode_table_blocks_for();

    if (memcmp(sb.magic, VFS_MAGIC, VFS_MAGIC_SIZE) != 0 ||
        sb.version != VFS_FORMAT_VERSION ||
        sb.block_size != VFS_BLOCK_SIZE ||
        sb.total_blocks != disk->total_blocks ||
        sb.bitmap_start != 1 ||
        sb.bitmap_blocks != expected_bitmap_blocks ||
        sb.inode_bitmap_start !=
            sb.bitmap_start + sb.bitmap_blocks ||
        sb.inode_bitmap_blocks != expected_inode_bitmap_blocks ||
        sb.inode_table_start !=
            sb.inode_bitmap_start + sb.inode_bitmap_blocks ||
        sb.inode_table_blocks != expected_inode_table_blocks ||
        sb.inode_count != VFS_INODE_COUNT ||
        sb.data_start !=
            sb.inode_table_start + sb.inode_table_blocks ||
        sb.data_start >= sb.total_blocks ||
        sb.free_blocks != sb.total_blocks - sb.data_start - 1 ||
        sb.root_inode != 0) {
        errno = EINVAL;
        return -1;
    }

    *superblock = sb;
    return 0;
}
