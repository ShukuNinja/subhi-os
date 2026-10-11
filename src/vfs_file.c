#include "vfs_file.h"

#include "vfs_alloc.h"
#include "vfs_dir.h"
#include "vfs_path.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define VFS_MAX_FILE_SIZE \
    ((uint64_t)VFS_DIRECT_BLOCKS * VFS_BLOCK_SIZE)

static int split_path(
    const char *path,
    char **parent_path,
    char **name
)
{
    if (path == NULL || parent_path == NULL || name == NULL ||
        path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    size_t length = strlen(path);

    if (length <= 1U || path[length - 1U] == '/') {
        errno = EINVAL;
        return -1;
    }

    const char *separator = strrchr(path, '/');
    const char *base = separator + 1;
    size_t name_length = strlen(base);

    if (name_length > VFS_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (name_length == 0U ||
        strcmp(base, ".") == 0 ||
        strcmp(base, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t parent_length = (size_t)(separator - path);

    if (parent_length == 0U) {
        parent_length = 1U;
    }

    char *parent = malloc(parent_length + 1U);
    char *filename = malloc(name_length + 1U);

    if (parent == NULL || filename == NULL) {
        free(parent);
        free(filename);
        errno = ENOMEM;
        return -1;
    }

    if (separator == path) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        memcpy(parent, path, parent_length);
        parent[parent_length] = '\0';
    }

    memcpy(filename, base, name_length + 1U);

    *parent_path = parent;
    *name = filename;
    return 0;
}

int vfs_file_create(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    uint32_t mode,
    uint32_t uid,
    uint32_t gid,
    uint64_t *result_inode_number
)
{
    if (disk == NULL || sb == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    char *parent_path = NULL;
    char *name = NULL;

    if (split_path(path, &parent_path, &name) < 0) {
        return -1;
    }

    uint64_t parent_inode_number;

    if (vfs_path_resolve(
            disk, sb, parent_path, &parent_inode_number) < 0) {
        int saved_errno = errno;
        free(parent_path);
        free(name);
        errno = saved_errno;
        return -1;
    }

    free(parent_path);

    struct vfs_inode parent_inode;

    if (vfs_inode_read(
            disk, sb, parent_inode_number, &parent_inode) < 0) {
        int saved_errno = errno;
        free(name);
        errno = saved_errno;
        return -1;
    }

    if (parent_inode.type != VFS_INODE_DIRECTORY) {
        free(name);
        errno = ENOTDIR;
        return -1;
    }

    uint64_t inode_number;

    if (vfs_inode_allocate(
            disk, sb, VFS_INODE_REGULAR,
            mode, uid, gid, &inode_number) < 0) {
        int saved_errno = errno;
        free(name);
        errno = saved_errno;
        return -1;
    }

    if (vfs_dir_add(
            disk, sb, parent_inode_number, name, inode_number) < 0) {
        int saved_errno = errno;

        /*
         * The new inode is empty. It can be released if inserting
         * the directory entry fails.
         */
        (void)vfs_inode_release(disk, sb, inode_number);

        free(name);
        errno = saved_errno;
        return -1;
    }

    free(name);

    if (result_inode_number != NULL) {
        *result_inode_number = inode_number;
    }

    return 0;
}

int vfs_file_read(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    void *buffer,
    size_t buffer_capacity,
    size_t *bytes_read
)
{
    if (disk == NULL || sb == NULL || path == NULL ||
        bytes_read == NULL ||
        (buffer == NULL && buffer_capacity != 0U)) {
        errno = EINVAL;
        return -1;
    }

    *bytes_read = 0;

    uint64_t inode_number;

    if (vfs_path_resolve(
            disk, sb, path, &inode_number) < 0) {
        return -1;
    }

    struct vfs_inode inode;

    if (vfs_inode_read(
            disk, sb, inode_number, &inode) < 0) {
        return -1;
    }

    if (inode.type != VFS_INODE_REGULAR) {
        errno = EISDIR;
        return -1;
    }

    if (inode.blocks_count > VFS_DIRECT_BLOCKS ||
        inode.size > VFS_MAX_FILE_SIZE) {
        errno = EIO;
        return -1;
    }

    uint64_t expected_blocks =
        inode.size / VFS_BLOCK_SIZE;

    if (inode.size % VFS_BLOCK_SIZE != 0U) {
        expected_blocks++;
    }

    if (inode.blocks_count != expected_blocks) {
        errno = EIO;
        return -1;
    }

    size_t remaining = buffer_capacity;

    if ((uint64_t)remaining > inode.size) {
        remaining = (size_t)inode.size;
    }

    unsigned char block_buffer[VFS_BLOCK_SIZE];
    unsigned char *output = buffer;
    size_t total_read = 0;

    while (remaining > 0U) {
        uint64_t block_index = total_read / VFS_BLOCK_SIZE;
        size_t block_offset = total_read % VFS_BLOCK_SIZE;

        if (block_index >= inode.blocks_count) {
            errno = EIO;
            return -1;
        }

        uint64_t block_number = inode.direct_blocks[block_index];

        if (block_number < sb->data_start ||
            block_number >= sb->total_blocks) {
            errno = EIO;
            return -1;
        }

        int allocated = vfs_block_is_allocated(
            disk, block_number);

        if (allocated < 0) {
            return -1;
        }

        if (allocated == 0) {
            errno = EIO;
            return -1;
        }

        if (vfs_disk_read_block(
                disk, block_number, block_buffer) < 0) {
            return -1;
        }

        size_t amount = VFS_BLOCK_SIZE - block_offset;

        if (amount > remaining) {
            amount = remaining;
        }

        memcpy(output + total_read,
               block_buffer + block_offset, amount);

        total_read += amount;
        remaining -= amount;
    }

    *bytes_read = total_read;
    return 0;
}
