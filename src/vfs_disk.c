#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int valid_disk(const struct vfs_disk *disk)
{
    return disk != NULL && disk->fd >= 0 && disk->total_blocks > 0;
}

static int transfer_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    void *buffer,
    int writing
)
{
    if (!valid_disk(disk) || buffer == NULL ||
        block_number >= disk->total_blocks) {
        errno = EINVAL;
        return -1;
    }

    off_t offset = (off_t)(block_number * (uint64_t)VFS_BLOCK_SIZE);
    size_t transferred = 0;

    while (transferred < VFS_BLOCK_SIZE) {
        ssize_t result;

        if (writing) {
            result = pwrite(
                disk->fd,
                (const char *)buffer + transferred,
                VFS_BLOCK_SIZE - transferred,
                offset + (off_t)transferred
            );
        } else {
            result = pread(
                disk->fd,
                (char *)buffer + transferred,
                VFS_BLOCK_SIZE - transferred,
                offset + (off_t)transferred
            );
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (result == 0) {
            errno = EIO;
            return -1;
        }

        transferred += (size_t)result;
    }

    return 0;
}

int vfs_disk_create(
    struct vfs_disk *disk,
    const char *path,
    uint64_t total_blocks
)
{
    if (disk == NULL || path == NULL || path[0] == '\0' ||
        total_blocks == 0 ||
        total_blocks > (uint64_t)INT64_MAX / VFS_BLOCK_SIZE) {
        errno = EINVAL;
        return -1;
    }

    disk->fd = -1;
    disk->total_blocks = 0;

    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return -1;
    }

    off_t size = (off_t)(total_blocks * (uint64_t)VFS_BLOCK_SIZE);

    if (ftruncate(fd, size) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        return -1;
    }

    disk->fd = fd;
    disk->total_blocks = total_blocks;

    return 0;
}

int vfs_disk_open(struct vfs_disk *disk, const char *path)
{
    if (disk == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    disk->fd = -1;
    disk->total_blocks = 0;

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    struct stat st;

    if (fstat(fd, &st) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size % VFS_BLOCK_SIZE != 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    disk->fd = fd;
    disk->total_blocks = (uint64_t)st.st_size / VFS_BLOCK_SIZE;

    return 0;
}

int vfs_disk_read_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    void *buffer
)
{
    return transfer_block(disk, block_number, buffer, 0);
}

int vfs_disk_write_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    const void *buffer
)
{
    return transfer_block(
        disk,
        block_number,
        (void *)buffer,
        1
    );
}

int vfs_disk_flush(struct vfs_disk *disk)
{
    if (!valid_disk(disk)) {
        errno = EINVAL;
        return -1;
    }

    return fdatasync(disk->fd);
}

int vfs_disk_close(struct vfs_disk *disk)
{
    if (!valid_disk(disk)) {
        errno = EINVAL;
        return -1;
    }

    int fd = disk->fd;

    disk->fd = -1;
    disk->total_blocks = 0;

    return close(fd);
}
