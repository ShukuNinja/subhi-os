
#include "vfs_dir.h"
#include "vfs_alloc.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define ENTRIES_PER_BLOCK \
    (VFS_BLOCK_SIZE / sizeof(struct vfs_dir_entry))

/*
 * Validate a filename without reading beyond the permitted
 * filename length.
 */
static int valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return 0;
    }

    for (size_t i = 0; i <= VFS_NAME_MAX; i++) {
        if (name[i] == '\0') {
            return 1;
        }

        if (name[i] == '/') {
            errno = EINVAL;
            return 0;
        }
    }

    errno = ENAMETOOLONG;
    return 0;
}

/*
 * Load a directory inode and verify that it is a directory.
 */
static int read_directory(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t inode_number,
    struct vfs_inode *directory
)
{
    if (disk == NULL || sb == NULL || directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (vfs_inode_read(
            disk, sb, inode_number, directory) < 0) {
        return -1;
    }

    if (directory->type != VFS_INODE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }

    if (directory->blocks_count > VFS_DIRECT_BLOCKS) {
        errno = EIO;
        return -1;
    }

    for (uint32_t i = 0; i < directory->blocks_count; i++) {
        if (directory->direct_blocks[i] < sb->data_start ||
            directory->direct_blocks[i] >= sb->total_blocks) {
            errno = EIO;
            return -1;
        }
    }

    return 0;
}

/*
 * Search every allocated directory block.
 * Returns 1 when found, 0 when absent, and -1 on error.
 */
static int find_entry(
    struct vfs_disk *disk,
    const struct vfs_inode *directory,
    const char *name,
    uint64_t *found_block,
    size_t *found_slot,
    uint64_t *found_inode
)
{
    unsigned char buffer[VFS_BLOCK_SIZE];

    for (uint32_t i = 0; i < directory->blocks_count; i++) {
        uint64_t block_number = directory->direct_blocks[i];

        if (vfs_disk_read_block(
                disk, block_number, buffer) < 0) {
            return -1;
        }

        struct vfs_dir_entry *entries =
            (struct vfs_dir_entry *)buffer;

        for (size_t j = 0; j < ENTRIES_PER_BLOCK; j++) {
            if (entries[j].name[0] == '\0') {
                continue;
            }

            /*
             * Treat an on-disk name without a terminator
             * as corrupt rather than reading past the field.
             */
            if (memchr(
                    entries[j].name, '\0',
                    sizeof(entries[j].name)) == NULL) {
                errno = EIO;
                return -1;
            }

            if (strcmp(entries[j].name, name) == 0) {
                if (found_block != NULL) {
                    *found_block = block_number;
                }

                if (found_slot != NULL) {
                    *found_slot = j;
                }

                if (found_inode != NULL) {
                    *found_inode = entries[j].inode_number;
                }

                return 1;
            }
        }
    }

    return 0;
}

int vfs_dir_lookup(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    const char *name,
    uint64_t *result_inode_number
)
{
    if (!valid_name(name) || result_inode_number == NULL) {
        if (result_inode_number == NULL) {
            errno = EINVAL;
        }

        return -1;
    }

    struct vfs_inode directory;

    if (read_directory(
            disk, sb, directory_inode_number, &directory) < 0) {
        return -1;
    }

    uint64_t target_inode;

    int result = find_entry(
        disk, &directory, name,
        NULL, NULL, &target_inode);

    if (result < 0) {
        return -1;
    }

    if (result == 0) {
        errno = ENOENT;
        return -1;
    }

    int allocated = vfs_inode_is_allocated(
        disk, sb, target_inode);

    if (allocated < 0) {
        return -1;
    }

    if (allocated == 0) {
        errno = EIO;
        return -1;
    }

    *result_inode_number = target_inode;
    return 0;
}

int vfs_dir_add(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    const char *name,
    uint64_t target_inode_number
)
{
    if (!valid_name(name)) {
        return -1;
    }

    if (disk == NULL || sb == NULL ||
        target_inode_number >= sb->inode_count) {
        errno = EINVAL;
        return -1;
    }

    struct vfs_inode target;

    if (vfs_inode_read(
            disk, sb, target_inode_number, &target) < 0) {
        return -1;
    }

    struct vfs_inode directory;

    if (read_directory(
            disk, sb, directory_inode_number, &directory) < 0) {
        return -1;
    }

    int existing = find_entry(
        disk, &directory, name, NULL, NULL, NULL);

    if (existing < 0) {
        return -1;
    }

    if (existing != 0) {
        errno = EEXIST;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    /*
     * Reuse the first empty slot in an existing block.
     */
    for (uint32_t i = 0; i < directory.blocks_count; i++) {
        uint64_t block_number = directory.direct_blocks[i];

        if (vfs_disk_read_block(
                disk, block_number, buffer) < 0) {
            return -1;
        }

        struct vfs_dir_entry *entries =
            (struct vfs_dir_entry *)buffer;

        for (size_t j = 0; j < ENTRIES_PER_BLOCK; j++) {
            if (entries[j].name[0] != '\0') {
                continue;
            }

            entries[j].inode_number = target_inode_number;
            memcpy(entries[j].name, name, strlen(name) + 1);

            if (vfs_disk_write_block(
                    disk, block_number, buffer) < 0) {
                return -1;
            }

            directory.size += sizeof(struct vfs_dir_entry);

            if (vfs_inode_write(
                    disk, sb, directory_inode_number,
                    &directory) < 0) {
                return -1;
            }

            return 0;
        }
    }

    /*
     * No free slot remains. Grow the directory using a direct
     * block pointer, if capacity remains.
     */
    if (directory.blocks_count >= VFS_DIRECT_BLOCKS) {
        errno = ENOSPC;
        return -1;
    }

    uint64_t new_block;

    if (vfs_block_allocate(disk, &new_block) < 0) {
        return -1;
    }

    memset(buffer, 0, sizeof(buffer));

    struct vfs_dir_entry *entries =
        (struct vfs_dir_entry *)buffer;

    entries[0].inode_number = target_inode_number;
    memcpy(entries[0].name, name, strlen(name) + 1);

    if (vfs_disk_write_block(disk, new_block, buffer) < 0) {
        int saved_errno = errno;
        (void)vfs_block_release(disk, new_block);
        errno = saved_errno;
        return -1;
    }

    directory.direct_blocks[directory.blocks_count] = new_block;
    directory.blocks_count++;
    directory.size += sizeof(struct vfs_dir_entry);

    if (vfs_inode_write(
            disk, sb, directory_inode_number, &directory) < 0) {
        /*
         * The new block may already contain data. Do not release
         * it here: the inode write might have partially succeeded.
         */
        return -1;
    }

    return 0;
}

int vfs_dir_remove(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    const char *name,
    uint64_t *removed_inode_number
)
{
    if (!valid_name(name)) {
        return -1;
    }

    struct vfs_inode directory;

    if (read_directory(
            disk, sb, directory_inode_number, &directory) < 0) {
        return -1;
    }

    uint64_t block_number;
    size_t slot;
    uint64_t target_inode;

    int result = find_entry(
        disk, &directory, name,
        &block_number, &slot, &target_inode);

    if (result < 0) {
        return -1;
    }

    if (result == 0) {
        errno = ENOENT;
        return -1;
    }

    unsigned char buffer[VFS_BLOCK_SIZE];

    if (vfs_disk_read_block(disk, block_number, buffer) < 0) {
        return -1;
    }

    struct vfs_dir_entry *entries =
        (struct vfs_dir_entry *)buffer;

    memset(&entries[slot], 0, sizeof(entries[slot]));

    if (vfs_disk_write_block(disk, block_number, buffer) < 0) {
        return -1;
    }

    if (directory.size >= sizeof(struct vfs_dir_entry)) {
        directory.size -= sizeof(struct vfs_dir_entry);
    } else {
        directory.size = 0;
    }

    if (vfs_inode_write(
            disk, sb, directory_inode_number, &directory) < 0) {
        return -1;
    }

    if (removed_inode_number != NULL) {
        *removed_inode_number = target_inode;
    }

    return 0;
}
