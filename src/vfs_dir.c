
#include "vfs_dir.h"
#include "vfs_alloc.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ENTRIES_PER_BLOCK \
    (VFS_BLOCK_SIZE / sizeof(struct vfs_dir_entry))

/*
 * Small process-local directory index. The index is rebuilt from the
 * authoritative on-disk directory blocks and invalidated after mutations.
 * It does not alter the version-2 on-disk format.
 */
#define DIR_INDEX_SLOTS 4U
#define DIR_INDEX_ENTRIES (VFS_DIRECT_BLOCKS * ENTRIES_PER_BLOCK)
struct directory_index {
    int valid;
    int fd;
    uint64_t inode_number;
    dev_t device;
    ino_t image_inode;
    size_t count;
    struct vfs_dir_entry entries[DIR_INDEX_ENTRIES];
    uint64_t age;
};
static struct directory_index directory_indexes[DIR_INDEX_SLOTS];
static uint64_t directory_index_clock;

static int compare_entries(const void *left, const void *right)
{
    const struct vfs_dir_entry *a = left;
    const struct vfs_dir_entry *b = right;
    return strcmp(a->name, b->name);
}

static void invalidate_directory_index(int fd, uint64_t inode_number)
{
    for (size_t i = 0; i < DIR_INDEX_SLOTS; i++) {
        if (directory_indexes[i].valid && directory_indexes[i].fd == fd &&
            directory_indexes[i].inode_number == inode_number) {
            directory_indexes[i].valid = 0;
        }
    }
}

static struct directory_index *get_directory_index(
    struct vfs_disk *disk, uint64_t inode_number,
    const struct vfs_inode *directory)
{
    struct stat image_stat;
    if (fstat(disk->fd, &image_stat) < 0) return NULL;
    struct directory_index *slot = NULL;
    for (size_t i = 0; i < DIR_INDEX_SLOTS; i++) {
        if (directory_indexes[i].valid && directory_indexes[i].fd == disk->fd &&
            directory_indexes[i].device == image_stat.st_dev &&
            directory_indexes[i].image_inode == image_stat.st_ino &&
            directory_indexes[i].inode_number == inode_number) {
            directory_indexes[i].age = ++directory_index_clock;
            return &directory_indexes[i];
        }
        if (!directory_indexes[i].valid) slot = &directory_indexes[i];
    }
    if (slot == NULL) {
        slot = &directory_indexes[0];
        for (size_t i = 1; i < DIR_INDEX_SLOTS; i++)
            if (directory_indexes[i].age < slot->age) slot = &directory_indexes[i];
    }
    slot->valid = 0;
    slot->fd = disk->fd;
    slot->device = image_stat.st_dev;
    slot->image_inode = image_stat.st_ino;
    slot->inode_number = inode_number;
    slot->count = 0;
    unsigned char buffer[VFS_BLOCK_SIZE];
    for (uint32_t b = 0; b < directory->blocks_count; b++) {
        if (vfs_disk_read_block(disk, directory->direct_blocks[b], buffer) < 0)
            return NULL;
        struct vfs_dir_entry entries[ENTRIES_PER_BLOCK];
        memcpy(entries, buffer, sizeof(entries));
        for (size_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].name[0] == '\0') continue;
            if (memchr(entries[e].name, '\0', sizeof(entries[e].name)) == NULL) {
                errno = EIO;
                return NULL;
            }
            if (slot->count >= DIR_INDEX_ENTRIES) { errno = EIO; return NULL; }
            slot->entries[slot->count++] = entries[e];
        }
    }
    qsort(slot->entries, slot->count, sizeof(slot->entries[0]), compare_entries);
    slot->age = ++directory_index_clock;
    slot->valid = 1;
    return slot;
}

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

int vfs_dir_iterate(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    vfs_dir_visit_fn visitor,
    void *context
)
{
    if (disk == NULL || sb == NULL || visitor == NULL) { errno = EINVAL; return -1; }
    struct vfs_inode directory;
    if (read_directory(disk, sb, directory_inode_number, &directory) < 0) return -1;
    unsigned char buffer[VFS_BLOCK_SIZE];
    for (uint32_t b = 0; b < directory.blocks_count; b++) {
        if (vfs_disk_read_block(disk, directory.direct_blocks[b], buffer) < 0) return -1;
        struct vfs_dir_entry entries[ENTRIES_PER_BLOCK];
        memcpy(entries, buffer, sizeof(entries));
        for (size_t e = 0; e < ENTRIES_PER_BLOCK; e++) {
            if (entries[e].name[0] == '\0') continue;
            if (memchr(entries[e].name, '\0', sizeof(entries[e].name)) == NULL ||
                memchr(entries[e].name, '/', strlen(entries[e].name)) != NULL) {
                errno = EIO; return -1;
            }
            int allocated = vfs_inode_is_allocated(disk, sb, entries[e].inode_number);
            if (allocated < 0) return -1;
            if (!allocated) { errno = EIO; return -1; }
            if (visitor(&entries[e], context) != 0) return -1;
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

    struct directory_index *index = get_directory_index(
        disk, directory_inode_number, &directory);
    if (index == NULL) return -1;
    struct vfs_dir_entry key = {0};
    size_t name_length = strlen(name);
    if (name_length > VFS_NAME_MAX) { errno = ENAMETOOLONG; return -1; }
    memcpy(key.name, name, name_length + 1U);
    struct vfs_dir_entry *found = bsearch(
        &key, index->entries, index->count, sizeof(index->entries[0]), compare_entries);
    if (found == NULL) { errno = ENOENT; return -1; }
    uint64_t target_inode = found->inode_number;

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
            invalidate_directory_index(disk->fd, directory_inode_number);
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
        /* The inode write may have partially succeeded. */
        return -1;
    }
    invalidate_directory_index(disk->fd, directory_inode_number);
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

    invalidate_directory_index(disk->fd, directory_inode_number);
    if (removed_inode_number != NULL) {
        *removed_inode_number = target_inode;
    }

    return 0;
}
