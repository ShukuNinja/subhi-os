#include "vfs_inode.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#define INODES_PER_BLOCK (VFS_BLOCK_SIZE / VFS_INODE_SIZE)

static int valid_layout(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb
)
{
    return disk != NULL && disk->fd >= 0 &&
           sb != NULL &&
           sb->inode_count > 0 &&
           sb->inode_bitmap_blocks > 0 &&
           sb->inode_table_blocks > 0 &&
           sb->inode_table_start + sb->inode_table_blocks
               <= sb->total_blocks;
}

static int inode_bitmap_update(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number,
    int allocated
)
{
    if (!valid_layout(disk, sb) ||
        inode_number >= sb->inode_count) {
        errno = EINVAL;
        return -1;
    }

    uint64_t bits_per_block = VFS_BLOCK_SIZE * 8ULL;
    uint64_t bitmap_block = inode_number / bits_per_block;
    uint64_t bit_index = inode_number % bits_per_block;

    if (bitmap_block >= sb->inode_bitmap_blocks) {
        errno = EINVAL;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk, sb->inode_bitmap_start + bitmap_block,
            buffer) < 0) {
        return -1;
    }

    unsigned char mask =
        (unsigned char)(1U << (bit_index % 8));

    if (allocated) {
        buffer[bit_index / 8] |= mask;
    } else {
        buffer[bit_index / 8] &= (unsigned char)~mask;
    }

    return vfs_disk_write_block(
        disk, sb->inode_bitmap_start + bitmap_block, buffer);
}

int vfs_inode_is_allocated(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number
)
{
    if (!valid_layout(disk, sb) ||
        inode_number >= sb->inode_count) {
        errno = EINVAL;
        return -1;
    }

    uint64_t bits_per_block = VFS_BLOCK_SIZE * 8ULL;
    uint64_t bitmap_block = inode_number / bits_per_block;
    uint64_t bit_index = inode_number % bits_per_block;

    if (bitmap_block >= sb->inode_bitmap_blocks) {
        errno = EINVAL;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(
            disk, sb->inode_bitmap_start + bitmap_block,
            buffer) < 0) {
        return -1;
    }

    unsigned char mask =
        (unsigned char)(1U << (bit_index % 8));

    return (buffer[bit_index / 8] & mask) != 0;
}

int vfs_inode_read(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number,
    struct vfs_inode *inode
)
{
    if (!valid_layout(disk, sb) || inode == NULL ||
        inode_number >= sb->inode_count) {
        errno = EINVAL;
        return -1;
    }

    int allocated = vfs_inode_is_allocated(
        disk, sb, inode_number);

    if (allocated != 1) {
        if (allocated == 0) {
            errno = ENOENT;
        }
        return -1;
    }

    uint64_t table_block =
        sb->inode_table_start + inode_number / INODES_PER_BLOCK;

    uint64_t offset =
        (inode_number % INODES_PER_BLOCK) * VFS_INODE_SIZE;

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(disk, table_block, buffer) < 0) {
        return -1;
    }

    memcpy(inode, buffer + offset, sizeof(*inode));
    return 0;
}

int vfs_inode_write(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number,
    const struct vfs_inode *inode
)
{
    if (!valid_layout(disk, sb) || inode == NULL ||
        inode_number >= sb->inode_count ||
        inode->type == VFS_INODE_FREE ||
        inode->type > VFS_INODE_DIRECTORY) {
        errno = EINVAL;
        return -1;
    }

    int allocated = vfs_inode_is_allocated(
        disk, sb, inode_number);

    if (allocated != 1) {
        if (allocated == 0) {
            errno = ENOENT;
        }
        return -1;
    }

    uint64_t table_block =
        sb->inode_table_start + inode_number / INODES_PER_BLOCK;

    uint64_t offset =
        (inode_number % INODES_PER_BLOCK) * VFS_INODE_SIZE;

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(disk, table_block, buffer) < 0) {
        return -1;
    }

    memcpy(buffer + offset, inode, sizeof(*inode));

    return vfs_disk_write_block(disk, table_block, buffer);
}

int vfs_inode_allocate(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    enum vfs_inode_type type,
    uint32_t mode,
    uint32_t uid,
    uint32_t gid,
    uint64_t *inode_number
)
{
    if (!valid_layout(disk, sb) || inode_number == NULL ||
        (type != VFS_INODE_REGULAR &&
         type != VFS_INODE_DIRECTORY)) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Inode 0 is reserved for the root directory.
     */
    for (uint64_t i = 1; i < sb->inode_count; i++) {
        int allocated = vfs_inode_is_allocated(disk, sb, i);

        if (allocated < 0) {
            return -1;
        }

        if (allocated != 0) {
            continue;
        }

        struct vfs_inode inode = {0};
        inode.type = (uint32_t)type;
        inode.mode = mode;
        inode.uid = uid;
        inode.gid = gid;
        inode.link_count = 1;

        time_t now = time(NULL);
        if (now < 0) {
            errno = EIO;
            return -1;
        }

        inode.atime = (uint64_t)now;
        inode.mtime = (uint64_t)now;
        inode.ctime = (uint64_t)now;

        /*
         * Write the inode record first. An unallocated inode record
         * can safely contain stale data if bitmap update fails.
         */
        uint64_t table_block =
            sb->inode_table_start + i / INODES_PER_BLOCK;

        uint64_t offset =
            (i % INODES_PER_BLOCK) * VFS_INODE_SIZE;

        unsigned char buffer[VFS_BLOCK_SIZE];

        if (vfs_disk_read_block(
                disk, table_block, buffer) < 0) {
            return -1;
        }

        memcpy(buffer + offset, &inode, sizeof(inode));

        if (vfs_disk_write_block(
                disk, table_block, buffer) < 0) {
            return -1;
        }

        if (inode_bitmap_update(disk, sb, i, 1) < 0) {
            return -1;
        }

        *inode_number = i;
        return 0;
    }

    errno = ENOSPC;
    return -1;
}

int vfs_inode_release(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number
)
{
    if (!valid_layout(disk, sb) ||
        inode_number >= sb->inode_count ||
        inode_number == sb->root_inode) {
        errno = EINVAL;
        return -1;
    }

    struct vfs_inode inode;

    if (vfs_inode_read(disk, sb, inode_number, &inode) < 0) {
        return -1;
    }

    /*
     * Block reclamation belongs to the storage-allocation layer.
     * Never free an inode while it still owns data blocks.
     */
    if (inode.size != 0 || inode.blocks_count != 0) {
        errno = EBUSY;
        return -1;
    }

    for (size_t i = 0; i < VFS_DIRECT_BLOCKS; i++) {
        if (inode.direct_blocks[i] != 0) {
            errno = EBUSY;
            return -1;
        }
    }

    /*
     * Clear the allocation bit first. If clearing the inode record
     * subsequently fails, its stale bytes are still unallocated.
     */
    if (inode_bitmap_update(disk, sb, inode_number, 0) < 0) {
        return -1;
    }

    memset(&inode, 0, sizeof(inode));

    uint64_t table_block =
        sb->inode_table_start + inode_number / INODES_PER_BLOCK;

    uint64_t offset =
        (inode_number % INODES_PER_BLOCK) * VFS_INODE_SIZE;

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(disk, table_block, buffer) < 0) {
        return -1;
    }

    memset(buffer + offset, 0, sizeof(inode));

    return vfs_disk_write_block(disk, table_block, buffer);
}
