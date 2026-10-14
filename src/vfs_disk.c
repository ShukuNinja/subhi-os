#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include <stddef.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#define VFS_BLOCK_CACHE_ENTRIES 16U
struct block_cache_entry {
    int valid;
    int fd;
    uint64_t block;
    uint64_t age;
    unsigned char data[VFS_BLOCK_SIZE];
};
static struct block_cache_entry block_cache[VFS_BLOCK_CACHE_ENTRIES];
static uint64_t cache_clock;
static struct vfs_io_metrics io_metrics;

#define VFS_JOURNAL_MAGIC "SUBHIJ3!"
#define VFS_JOURNAL_MAGIC_SIZE 8U

struct vfs_journal_header {
    char magic[VFS_JOURNAL_MAGIC_SIZE];
    uint32_t active;
    uint32_t reserved;
    uint64_t target_block;
    uint64_t image_hash;
    uint64_t header_hash;
};

static int transfer_block(struct vfs_disk *disk, uint64_t block_number, void *buffer, int writing);
static void invalidate_fd_cache(int fd);

static uint64_t hash_bytes(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int raw_write_block(struct vfs_disk *disk, uint64_t block, const void *buffer)
{
    return transfer_block(disk, block, (void *)buffer, 1);
}

static int recover_journal(struct vfs_disk *disk)
{
    unsigned char header_block[VFS_BLOCK_SIZE];
    unsigned char image[VFS_BLOCK_SIZE];
    struct vfs_journal_header header;

    if (transfer_block(disk, VFS_JOURNAL_START, header_block, 0) < 0)
        return -1;
    memcpy(&header, header_block, sizeof(header));
    if (memcmp(header.magic, VFS_JOURNAL_MAGIC, VFS_JOURNAL_MAGIC_SIZE) != 0 ||
        header.active != 1U)
        return 0;
    if (header.header_hash != hash_bytes(&header, offsetof(struct vfs_journal_header, header_hash)) ||
        header.target_block >= disk->total_blocks ||
        header.target_block < VFS_JOURNAL_START + VFS_JOURNAL_BLOCKS) {
        errno = EILSEQ;
        return -1;
    }
    if (transfer_block(disk, VFS_JOURNAL_START + 1U, image, 0) < 0)
        return -1;
    if (header.image_hash != hash_bytes(image, sizeof(image))) {
        errno = EILSEQ;
        return -1;
    }
    if (raw_write_block(disk, header.target_block, image) < 0 || fdatasync(disk->fd) < 0)
        return -1;
    memset(header_block, 0, sizeof(header_block));
    if (raw_write_block(disk, VFS_JOURNAL_START, header_block) < 0 || fdatasync(disk->fd) < 0)
        return -1;
    invalidate_fd_cache(disk->fd);
    return 0;
}

static int journaled_write_block(struct vfs_disk *disk, uint64_t target, const void *buffer)
{
    unsigned char old_image[VFS_BLOCK_SIZE];
    unsigned char header_block[VFS_BLOCK_SIZE] = {0};
    struct vfs_journal_header header = {0};

    if (transfer_block(disk, target, old_image, 0) < 0)
        return -1;
    if (raw_write_block(disk, VFS_JOURNAL_START + 1U, old_image) < 0 || fdatasync(disk->fd) < 0)
        return -1;

    memcpy(header.magic, VFS_JOURNAL_MAGIC, VFS_JOURNAL_MAGIC_SIZE);
    header.active = 1U;
    header.target_block = target;
    header.image_hash = hash_bytes(old_image, sizeof(old_image));
    header.header_hash = hash_bytes(&header, offsetof(struct vfs_journal_header, header_hash));
    memcpy(header_block, &header, sizeof(header));
    if (raw_write_block(disk, VFS_JOURNAL_START, header_block) < 0 || fdatasync(disk->fd) < 0)
        return -1;

    if (raw_write_block(disk, target, buffer) < 0 || fdatasync(disk->fd) < 0)
        return -1;
    memset(header_block, 0, sizeof(header_block));
    if (raw_write_block(disk, VFS_JOURNAL_START, header_block) < 0 || fdatasync(disk->fd) < 0)
        return -1;
    return 0;
}

void vfs_metrics_reset(void) { memset(&io_metrics, 0, sizeof(io_metrics)); }
void vfs_metrics_get(struct vfs_io_metrics *metrics)
{ if (metrics != NULL) *metrics = io_metrics; }

static void invalidate_fd_cache(int fd)
{
    for (size_t i = 0; i < VFS_BLOCK_CACHE_ENTRIES; i++)
        if (block_cache[i].valid && block_cache[i].fd == fd) block_cache[i].valid = 0;
}

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
    disk->journal_enabled = 0;

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

    invalidate_fd_cache(fd);
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
    disk->journal_enabled = 0;

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

    invalidate_fd_cache(fd);
    disk->fd = fd;
    disk->total_blocks = (uint64_t)st.st_size / VFS_BLOCK_SIZE;

    /* Recovery is safe to attempt before validating the superblock: an inactive
       journal is ignored, while a valid pending record restores one old block. */
    if (recover_journal(disk) < 0) {
        int saved_errno = errno;
        close(fd);
        disk->fd = -1;
        disk->total_blocks = 0;
        errno = saved_errno;
        return -1;
    }
    unsigned char superblock_block[VFS_BLOCK_SIZE];
    if (transfer_block(disk, 0, superblock_block, 0) == 0) {
        struct vfs_superblock sb;
        memcpy(&sb, superblock_block, sizeof(sb));
        if (memcmp(sb.magic, VFS_MAGIC, VFS_MAGIC_SIZE) == 0 &&
            sb.version == VFS_FORMAT_VERSION &&
            sb.block_size == VFS_BLOCK_SIZE)
            disk->journal_enabled = 1;
    }
    return 0;
}

int vfs_disk_read_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    void *buffer
)
{
    if (!valid_disk(disk) || buffer == NULL || block_number >= disk->total_blocks) {
        errno = EINVAL; return -1;
    }
    io_metrics.read_requests++;
    for (size_t i = 0; i < VFS_BLOCK_CACHE_ENTRIES; i++) {
        if (block_cache[i].valid && block_cache[i].fd == disk->fd &&
            block_cache[i].block == block_number) {
            memcpy(buffer, block_cache[i].data, VFS_BLOCK_SIZE);
            block_cache[i].age = ++cache_clock;
            io_metrics.cache_hits++;
            return 0;
        }
    }
    io_metrics.cache_misses++;
    if (transfer_block(disk, block_number, buffer, 0) < 0) return -1;
    io_metrics.physical_reads++;
    size_t slot = 0;
    for (size_t i = 0; i < VFS_BLOCK_CACHE_ENTRIES; i++) {
        if (!block_cache[i].valid) { slot = i; break; }
        if (block_cache[i].age < block_cache[slot].age) slot = i;
    }
    block_cache[slot].valid = 1; block_cache[slot].fd = disk->fd;
    block_cache[slot].block = block_number; block_cache[slot].age = ++cache_clock;
    memcpy(block_cache[slot].data, buffer, VFS_BLOCK_SIZE);
    return 0;
}

int vfs_disk_write_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    const void *buffer
)
{
    if (!valid_disk(disk) || buffer == NULL || block_number >= disk->total_blocks) {
        errno = EINVAL; return -1;
    }
    io_metrics.write_requests++;
    if (disk->journal_enabled &&
        block_number >= VFS_JOURNAL_START + VFS_JOURNAL_BLOCKS) {
        if (journaled_write_block(disk, block_number, buffer) < 0) return -1;
        io_metrics.physical_writes += 4;
    } else {
        if (transfer_block(disk, block_number, (void *)buffer, 1) < 0) return -1;
        io_metrics.physical_writes++;
    }
    size_t slot = VFS_BLOCK_CACHE_ENTRIES;
    for (size_t i = 0; i < VFS_BLOCK_CACHE_ENTRIES; i++) {
        if (block_cache[i].valid && block_cache[i].fd == disk->fd &&
            block_cache[i].block == block_number) { slot = i; break; }
    }
    if (slot == VFS_BLOCK_CACHE_ENTRIES) {
        slot = 0;
        for (size_t i = 0; i < VFS_BLOCK_CACHE_ENTRIES; i++) {
            if (!block_cache[i].valid) { slot = i; break; }
            if (block_cache[i].age < block_cache[slot].age) slot = i;
        }
    }
    block_cache[slot].valid = 1; block_cache[slot].fd = disk->fd;
    block_cache[slot].block = block_number; block_cache[slot].age = ++cache_clock;
    memcpy(block_cache[slot].data, buffer, VFS_BLOCK_SIZE);
    return 0;
}

int vfs_disk_enable_journal(struct vfs_disk *disk)
{
    if (!valid_disk(disk) || disk->total_blocks <= VFS_JOURNAL_START + VFS_JOURNAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }
    disk->journal_enabled = 1;
    return 0;
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

    invalidate_fd_cache(fd);
    disk->fd = -1;
    disk->total_blocks = 0;
    disk->journal_enabled = 0;

    return close(fd);
}
