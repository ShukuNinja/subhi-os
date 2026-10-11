
#include "vfs_file.h"

#include "vfs_alloc.h"
#include "vfs_dir.h"
#include "vfs_path.h"
#include "vfs_perm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    const struct vfs_credentials *credentials,
    uint64_t *result_inode_number
)
{
    if (disk == NULL || sb == NULL || path == NULL || credentials == NULL) {
        errno = EINVAL;
        return -1;
    }

    char *parent_path = NULL;
    char *name = NULL;

    if (split_path(path, &parent_path, &name) < 0) {
        return -1;
    }

    uint64_t parent_inode_number;

    if (vfs_path_resolve_as(
            disk, sb, parent_path, credentials,
            &parent_inode_number) < 0) {
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

    if (vfs_check_access(
            &parent_inode, credentials,
            VFS_ACCESS_WRITE | VFS_ACCESS_EXECUTE) < 0) {
        int saved_errno = errno;
        free(name);
        errno = saved_errno;
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
    size_t *bytes_read,
    const struct vfs_credentials *credentials
)
{
    if (disk == NULL || sb == NULL || path == NULL ||
        credentials == NULL || bytes_read == NULL ||
        (buffer == NULL && buffer_capacity != 0U)) {
        errno = EINVAL;
        return -1;
    }

    *bytes_read = 0;

    uint64_t inode_number;

    if (vfs_path_resolve_as(
            disk, sb, path, credentials, &inode_number) < 0) {
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

    if (vfs_check_access(&inode, credentials, VFS_ACCESS_READ) < 0) {
        return -1;
    }

    if (inode.blocks_count > VFS_DIRECT_BLOCKS ||
        inode.size > VFS_MAX_FILE_SIZE) {
        errno = EIO;
        return -1;
    }

    uint64_t expected_blocks = inode.size / VFS_BLOCK_SIZE;

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

        int allocated = vfs_block_is_allocated(disk, block_number);

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

int vfs_file_write(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    const void *buffer,
    size_t size,
    const struct vfs_credentials *credentials
)
{
    if (disk == NULL || sb == NULL || path == NULL ||
        credentials == NULL ||
        (buffer == NULL && size != 0U)) {
        errno = EINVAL;
        return -1;
    }

    if ((uint64_t)size > VFS_MAX_FILE_SIZE) {
        errno = EFBIG;
        return -1;
    }

    uint64_t inode_number;

    if (vfs_path_resolve_as(
            disk, sb, path, credentials, &inode_number) < 0) {
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

    if (vfs_check_access(&inode, credentials, VFS_ACCESS_WRITE) < 0) {
        return -1;
    }

    if (inode.blocks_count > VFS_DIRECT_BLOCKS ||
        inode.size > VFS_MAX_FILE_SIZE) {
        errno = EIO;
        return -1;
    }

    uint64_t expected_old_blocks = inode.size / VFS_BLOCK_SIZE;

    if (inode.size % VFS_BLOCK_SIZE != 0U) {
        expected_old_blocks++;
    }

    if (inode.blocks_count != expected_old_blocks) {
        errno = EIO;
        return -1;
    }

    /*
     * Validate the current blocks before changing anything.
     */
    for (uint32_t i = 0; i < inode.blocks_count; i++) {
        uint64_t block = inode.direct_blocks[i];

        if (block < sb->data_start ||
            block >= sb->total_blocks) {
            errno = EIO;
            return -1;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (inode.direct_blocks[j] == block) {
                errno = EIO;
                return -1;
            }
        }

        int allocated = vfs_block_is_allocated(disk, block);

        if (allocated < 0) {
            return -1;
        }

        if (allocated == 0) {
            errno = EIO;
            return -1;
        }
    }

    uint64_t old_blocks = inode.blocks_count;
    uint64_t new_blocks = size / VFS_BLOCK_SIZE;

    if (size % VFS_BLOCK_SIZE != 0U) {
        new_blocks++;
    }

    uint64_t new_pointers[VFS_DIRECT_BLOCKS] = {0};
    size_t allocated_count = 0;

    /*
     * Allocate a separate set of blocks. Keeping the old blocks
     * untouched until the new inode is committed protects existing
     * contents if allocation or data writing fails.
     */
    for (uint64_t i = 0; i < new_blocks; i++) {
        if (vfs_block_allocate(disk, &new_pointers[i]) < 0) {
            int saved_errno = errno;

            for (size_t j = 0; j < allocated_count; j++) {
                (void)vfs_block_release(disk, new_pointers[j]);
            }

            errno = saved_errno;
            return -1;
        }

        allocated_count++;
    }

    const unsigned char *input = buffer;
    size_t offset = 0;
    unsigned char block_buffer[VFS_BLOCK_SIZE];

    for (uint64_t i = 0; i < new_blocks; i++) {
        memset(block_buffer, 0, sizeof(block_buffer));

        size_t amount = size - offset;

        if (amount > VFS_BLOCK_SIZE) {
            amount = VFS_BLOCK_SIZE;
        }

        if (amount > 0U) {
            memcpy(block_buffer, input + offset, amount);
        }

        if (vfs_disk_write_block(
                disk, new_pointers[i], block_buffer) < 0) {
            int saved_errno = errno;

            for (size_t j = 0; j < allocated_count; j++) {
                (void)vfs_block_release(disk, new_pointers[j]);
            }

            errno = saved_errno;
            return -1;
        }

        offset += amount;
    }

    time_t now = time(NULL);

    if (now < 0) {
        /*
         * New blocks are not referenced by the old inode, so they
         * can be reclaimed while the old inode remains unchanged.
         */
        for (size_t j = 0; j < allocated_count; j++) {
            (void)vfs_block_release(disk, new_pointers[j]);
        }

        errno = EIO;
        return -1;
    }

    struct vfs_inode updated_inode = inode;

    updated_inode.size = size;
    updated_inode.blocks_count = (uint32_t)new_blocks;
    updated_inode.mtime = (uint64_t)now;
    updated_inode.ctime = (uint64_t)now;

    memset(updated_inode.direct_blocks, 0,
           sizeof(updated_inode.direct_blocks));

    for (uint64_t i = 0; i < new_blocks; i++) {
        updated_inode.direct_blocks[i] = new_pointers[i];
    }

    /*
     * Commit the new inode before releasing the old data blocks.
     * If this write fails, retain the newly allocated blocks because
     * the inode write may have partially succeeded.
     */
    if (vfs_inode_write(
            disk, sb, inode_number, &updated_inode) < 0) {
        return -1;
    }

    /*
     * The committed inode no longer references the old blocks.
     */
    for (uint64_t i = 0; i < old_blocks; i++) {
        if (vfs_block_release(
                disk, inode.direct_blocks[i]) < 0) {
            return -1;
        }
    }

    return 0;
}


int vfs_file_unlink(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    const struct vfs_credentials *credentials
)
{
    if (disk == NULL || sb == NULL || path == NULL ||
        credentials == NULL) {
        errno = EINVAL;
        return -1;
    }

    char *parent_path = NULL;
    char *name = NULL;

    if (split_path(path, &parent_path, &name) < 0) {
        return -1;
    }

    uint64_t parent_inode_number;
    uint64_t target_inode_number;
    struct vfs_inode inode;
    uint64_t old_blocks[VFS_DIRECT_BLOCKS];

    int result = -1;

    if (vfs_path_resolve_as(
            disk, sb, parent_path, credentials,
            &parent_inode_number) < 0) {
        goto cleanup;
    }

    struct vfs_inode parent_inode;

    if (vfs_inode_read(
            disk, sb, parent_inode_number, &parent_inode) < 0) {
        goto cleanup;
    }

    if (parent_inode.type != VFS_INODE_DIRECTORY) {
        errno = ENOTDIR;
        goto cleanup;
    }

    if (vfs_check_access(
            &parent_inode, credentials,
            VFS_ACCESS_WRITE | VFS_ACCESS_EXECUTE) < 0) {
        goto cleanup;
    }

    if (vfs_dir_lookup(
            disk, sb, parent_inode_number,
            name, &target_inode_number) < 0) {
        goto cleanup;
    }

    if (target_inode_number >= sb->inode_count ||
        target_inode_number == sb->root_inode) {
        errno = EINVAL;
        goto cleanup;
    }

    int allocated = vfs_inode_is_allocated(
        disk, sb, target_inode_number);

    if (allocated < 0) {
        goto cleanup;
    }

    if (allocated == 0) {
        errno = EIO;
        goto cleanup;
    }

    if (vfs_inode_read(
            disk, sb, target_inode_number, &inode) < 0) {
        goto cleanup;
    }

    if (inode.type == VFS_INODE_DIRECTORY) {
        errno = EISDIR;
        goto cleanup;
    }

    if (inode.type != VFS_INODE_REGULAR) {
        errno = EIO;
        goto cleanup;
    }

    if (inode.size > VFS_MAX_FILE_SIZE) {
        errno = EIO;
        goto cleanup;
    }

    uint64_t expected_blocks =
        inode.size / VFS_BLOCK_SIZE;

    if (inode.size % VFS_BLOCK_SIZE != 0) {
        expected_blocks++;
    }

    if (inode.blocks_count != expected_blocks ||
        inode.blocks_count > VFS_DIRECT_BLOCKS) {
        errno = EIO;
        goto cleanup;
    }

    /*
     * Validate every owned block before changing directory metadata.
     * Also reject duplicate pointers and unexpected nonzero pointers.
     */
    for (size_t i = 0; i < VFS_DIRECT_BLOCKS; i++) {
        old_blocks[i] = inode.direct_blocks[i];

        if (i >= inode.blocks_count) {
            if (old_blocks[i] != 0) {
                errno = EIO;
                goto cleanup;
            }

            continue;
        }

        uint64_t block_number = old_blocks[i];

        if (block_number < sb->data_start ||
            block_number >= sb->total_blocks) {
            errno = EIO;
            goto cleanup;
        }

        int block_allocated =
            vfs_block_is_allocated(disk, block_number);

        if (block_allocated < 0) {
            goto cleanup;
        }

        if (block_allocated == 0) {
            errno = EIO;
            goto cleanup;
        }

        for (size_t j = 0; j < i; j++) {
            if (old_blocks[j] == block_number) {
                errno = EIO;
                goto cleanup;
            }
        }
    }

    /*
     * Remove the directory entry first. The remaining reclamation
     * steps can still fail, so this is not a transactional unlink.
     */
    uint64_t removed_inode_number;

    if (vfs_dir_remove(
            disk, sb, parent_inode_number, name,
            &removed_inode_number) < 0) {
        goto cleanup;
    }

    if (removed_inode_number != target_inode_number) {
        errno = EIO;
        goto cleanup;
    }

    /*
     * Clear inode references before releasing blocks: the allocator
     * refuses to release blocks referenced by live inodes.
     */
    memset(inode.direct_blocks, 0, sizeof(inode.direct_blocks));
    inode.size = 0;
    inode.blocks_count = 0;
    inode.mtime = (uint64_t)time(NULL);
    inode.ctime = inode.mtime;

    if (vfs_inode_write(
            disk, sb, target_inode_number, &inode) < 0) {
        goto cleanup;
    }

    for (size_t i = 0; i < VFS_DIRECT_BLOCKS; i++) {
        if (old_blocks[i] == 0) {
            continue;
        }

        if (vfs_block_release(disk, old_blocks[i]) < 0) {
            goto cleanup;
        }
    }

    if (vfs_inode_release(
            disk, sb, target_inode_number) < 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    free(parent_path);
    free(name);
    return result;
}