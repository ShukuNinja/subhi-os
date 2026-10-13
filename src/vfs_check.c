#define _POSIX_C_SOURCE 200809L
#include "vfs_check.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_dir.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BITS_PER_BLOCK ((uint64_t)VFS_BLOCK_SIZE * 8U)
#define DIR_ENTRIES_PER_BLOCK (VFS_BLOCK_SIZE / sizeof(struct vfs_dir_entry))

struct checker {
    struct vfs_disk *disk;
    const struct vfs_superblock *sb;
    FILE *report;
    uint64_t errors;
    uint64_t warnings;
    uint32_t *references;
    unsigned char *block_bitmap;
    unsigned char *inode_bitmap;
};

static void problem(struct checker *c, const char *message,
                    uint64_t object, uint64_t detail)
{
    c->errors++;
    if (c->report != NULL) {
        fprintf(c->report, "ERROR: %s (%llu, %llu)\n", message,
                (unsigned long long)object, (unsigned long long)detail);
    }
}

static int bit_get(const unsigned char *bits, uint64_t index)
{
    return (bits[index / 8U] & (unsigned char)(1U << (index % 8U))) != 0;
}

static int read_bitmaps(struct checker *c)
{
    uint64_t total = c->sb->total_blocks;
    uint64_t inode_count = c->sb->inode_count;
    if (total > SIZE_MAX / sizeof(*c->references) ||
        total > SIZE_MAX || inode_count > SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    c->references = calloc((size_t)total, sizeof(*c->references));
    c->block_bitmap = calloc((size_t)((total + 7U) / 8U), 1);
    c->inode_bitmap = calloc((size_t)((inode_count + 7U) / 8U), 1);
    if (c->references == NULL || c->block_bitmap == NULL ||
        c->inode_bitmap == NULL) return -1;

    unsigned char buffer[VFS_BLOCK_SIZE];
    for (uint64_t i = 0; i < c->sb->bitmap_blocks; i++) {
        if (vfs_disk_read_block(c->disk, c->sb->bitmap_start + i, buffer) < 0)
            return -1;
        uint64_t first = i * BITS_PER_BLOCK;
        uint64_t count = total - first;
        if (count > BITS_PER_BLOCK) count = BITS_PER_BLOCK;
        for (uint64_t j = 0; j < count; j++) {
            if (bit_get(buffer, j)) {
                uint64_t b = first + j;
                c->block_bitmap[b / 8U] |= (unsigned char)(1U << (b % 8U));
            }
        }
        for (uint64_t j = count; j < BITS_PER_BLOCK; j++) {
            if (!bit_get(buffer, j)) {
                problem(c, "bitmap marks out-of-range block as free", first + j, 0);
                break;
            }
        }
    }
    for (uint64_t i = 0; i < c->sb->inode_bitmap_blocks; i++) {
        if (vfs_disk_read_block(c->disk, c->sb->inode_bitmap_start + i, buffer) < 0)
            return -1;
        uint64_t first = i * BITS_PER_BLOCK;
        uint64_t count = inode_count - first;
        if (count > BITS_PER_BLOCK) count = BITS_PER_BLOCK;
        for (uint64_t j = 0; j < count; j++) {
            if (bit_get(buffer, j)) {
                uint64_t ino = first + j;
                c->inode_bitmap[ino / 8U] |= (unsigned char)(1U << (ino % 8U));
            }
        }
        for (uint64_t j = count; j < BITS_PER_BLOCK; j++) {
            if (!bit_get(buffer, j)) {
                problem(c, "inode bitmap marks out-of-range inode as free", first + j, 0);
                break;
            }
        }
    }
    return 0;
}

static int block_allocated(const struct checker *c, uint64_t b)
{
    return b < c->sb->total_blocks && bit_get(c->block_bitmap, b);
}

static int inode_allocated(const struct checker *c, uint64_t i)
{
    return i < c->sb->inode_count && bit_get(c->inode_bitmap, i);
}

static void check_directory(struct checker *c, uint64_t ino,
                           const struct vfs_inode *inode)
{
    uint64_t entries_seen = 0;
    char names[VFS_DIRECT_BLOCKS * DIR_ENTRIES_PER_BLOCK][VFS_NAME_MAX + 1U];
    size_t name_count = 0;
    unsigned char buffer[VFS_BLOCK_SIZE];

    for (uint32_t b = 0; b < inode->blocks_count; b++) {
        if (vfs_disk_read_block(c->disk, inode->direct_blocks[b], buffer) < 0) {
            problem(c, "cannot read directory data block", ino, inode->direct_blocks[b]);
            continue;
        }
        struct vfs_dir_entry entries[DIR_ENTRIES_PER_BLOCK];
        memcpy(entries, buffer, sizeof(entries));
        for (size_t e = 0; e < DIR_ENTRIES_PER_BLOCK; e++) {
            if (entries[e].name[0] == '\0') continue;
            entries_seen++;
            if (memchr(entries[e].name, '\0', sizeof(entries[e].name)) == NULL) {
                problem(c, "directory entry name is not terminated", ino, e);
                continue;
            }
            size_t length = strnlen(entries[e].name, sizeof(entries[e].name));
            if (length == 0 || memchr(entries[e].name, '/', length) != NULL) {
                problem(c, "invalid directory entry name", ino, e);
            }
            if (!inode_allocated(c, entries[e].inode_number)) {
                problem(c, "directory entry references free/invalid inode", ino,
                        entries[e].inode_number);
            }
            for (size_t n = 0; n < name_count; n++) {
                if (strcmp(names[n], entries[e].name) == 0) {
                    problem(c, "duplicate directory entry name", ino, e);
                    break;
                }
            }
            if (name_count < sizeof(names) / sizeof(names[0])) {
                memcpy(names[name_count], entries[e].name, length + 1U);
                name_count++;
            }
        }
    }
    if (inode->size != entries_seen * sizeof(struct vfs_dir_entry))
        problem(c, "directory size disagrees with active entries", ino, entries_seen);
}

int vfs_check_filesystem(struct vfs_disk *disk, FILE *report)
{
    if (disk == NULL || disk->fd < 0) { errno = EINVAL; return -1; }
    struct vfs_superblock sb;
    if (vfs_read_superblock(disk, &sb) < 0) return -1;

    struct checker c = { .disk = disk, .sb = &sb, .report = report };
    if (read_bitmaps(&c) < 0) {
        int saved = errno;
        free(c.references); free(c.block_bitmap); free(c.inode_bitmap);
        errno = saved;
        return -1;
    }

    for (uint64_t b = 0; b <= sb.data_start; b++) {
        if (!block_allocated(&c, b)) problem(&c, "reserved metadata/root block is free", b, 0);
    }
    if (!inode_allocated(&c, sb.root_inode))
        problem(&c, "root inode is not allocated", sb.root_inode, 0);

    uint64_t free_blocks = 0;
    for (uint64_t b = sb.data_start; b < sb.total_blocks; b++) {
        if (!block_allocated(&c, b)) free_blocks++;
    }
    if (free_blocks != sb.free_blocks)
        problem(&c, "superblock free-block count disagrees with bitmap", sb.free_blocks, free_blocks);

    for (uint64_t ino = 0; ino < sb.inode_count; ino++) {
        if (!inode_allocated(&c, ino)) continue;
        struct vfs_inode inode;
        if (vfs_inode_read(disk, &sb, ino, &inode) < 0) {
            problem(&c, "cannot read allocated inode", ino, 0);
            continue;
        }
        if (inode.type != VFS_INODE_REGULAR && inode.type != VFS_INODE_DIRECTORY) {
            problem(&c, "allocated inode has invalid type", ino, inode.type);
            continue;
        }
        if (inode.blocks_count > VFS_DIRECT_BLOCKS) {
            problem(&c, "inode block count exceeds direct-pointer capacity", ino, inode.blocks_count);
            continue;
        }
        if (ino == sb.root_inode && inode.type != VFS_INODE_DIRECTORY)
            problem(&c, "root inode is not a directory", ino, inode.type);
        uint64_t needed = inode.size / VFS_BLOCK_SIZE +
                         (inode.size % VFS_BLOCK_SIZE != 0U);
        if (inode.type == VFS_INODE_REGULAR && needed > inode.blocks_count)
            problem(&c, "file size exceeds allocated block capacity", ino, inode.size);
        if (inode.type == VFS_INODE_DIRECTORY && inode.blocks_count == 0)
            problem(&c, "directory has no data blocks", ino, 0);

        for (uint32_t p = 0; p < VFS_DIRECT_BLOCKS; p++) {
            uint64_t b = inode.direct_blocks[p];
            if (p >= inode.blocks_count) {
                if (b != 0) problem(&c, "unused direct pointer is nonzero", ino, b);
                continue;
            }
            if (b < sb.data_start || b >= sb.total_blocks) {
                problem(&c, "inode direct pointer is outside data region", ino, b);
                continue;
            }
            if (!block_allocated(&c, b))
                problem(&c, "inode references a free data block", ino, b);
            c.references[b]++;
            if (c.references[b] > 1U)
                problem(&c, "data block is referenced more than once", b, ino);
        }
        if (inode.type == VFS_INODE_DIRECTORY) check_directory(&c, ino, &inode);
    }

    for (uint64_t b = sb.data_start; b < sb.total_blocks; b++) {
        if (block_allocated(&c, b) && c.references[b] == 0)
            problem(&c, "allocated data block has no inode owner", b, 0);
    }

    if (report != NULL) {
        if (c.errors == 0) fprintf(report, "Filesystem check passed: %llu blocks, %llu inodes examined.\n",
                                  (unsigned long long)sb.total_blocks,
                                  (unsigned long long)sb.inode_count);
        else fprintf(report, "Filesystem check failed: %llu inconsistency(s) found.\n",
                     (unsigned long long)c.errors);
    }
    int result = c.errors == 0 ? 0 : 1;
    free(c.references); free(c.block_bitmap); free(c.inode_bitmap);
    return result;
}
