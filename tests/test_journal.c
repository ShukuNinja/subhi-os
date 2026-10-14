#define _POSIX_C_SOURCE 200809L
#include "vfs_disk.h"
#include "vfs_format.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define IMAGE "build/test_journal.img"
#define JOURNAL_MAGIC "SUBHIJ3!"

struct journal_header_test {
    char magic[8];
    uint32_t active;
    uint32_t reserved;
    uint64_t target_block;
    uint64_t image_hash;
    uint64_t header_hash;
};

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

static int write_at(int fd, const void *buffer, size_t length, off_t offset)
{
    const unsigned char *p = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = pwrite(fd, p + done, length - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(void)
{
    struct vfs_disk disk;
    unsigned char before[VFS_BLOCK_SIZE];
    unsigned char changed[VFS_BLOCK_SIZE];
    unsigned char actual[VFS_BLOCK_SIZE];
    unsigned char journal_block[VFS_BLOCK_SIZE] = {0};
    struct journal_header_test header = {0};

    unlink(IMAGE);
    if (vfs_disk_create(&disk, IMAGE, 64) < 0 || vfs_format_disk(&disk) < 0) {
        perror("create/format");
        return 1;
    }
    memset(before, 0x35, sizeof(before));
    memset(changed, 0xA7, sizeof(changed));
    if (vfs_disk_write_block(&disk, 20, before) < 0 || vfs_disk_flush(&disk) < 0 ||
        vfs_disk_close(&disk) < 0) {
        perror("prepare image");
        return 1;
    }

    /* Emulate power loss after the target write but before journal clearing. */
    int fd = open(IMAGE, O_RDWR);
    if (fd < 0) { perror("open raw image"); return 1; }
    memcpy(header.magic, JOURNAL_MAGIC, sizeof(header.magic));
    header.active = 1;
    header.target_block = 20;
    header.image_hash = hash_bytes(before, sizeof(before));
    header.header_hash = hash_bytes(&header, offsetof(struct journal_header_test, header_hash));
    memcpy(journal_block, &header, sizeof(header));
    if (write_at(fd, before, sizeof(before), (off_t)(2U * VFS_BLOCK_SIZE)) < 0 ||
        fdatasync(fd) < 0 ||
        write_at(fd, journal_block, sizeof(journal_block), (off_t)VFS_BLOCK_SIZE) < 0 ||
        fdatasync(fd) < 0 ||
        write_at(fd, changed, sizeof(changed), (off_t)(20U * VFS_BLOCK_SIZE)) < 0 ||
        fdatasync(fd) < 0) {
        perror("simulate interrupted write"); close(fd); return 1;
    }
    close(fd);

    if (vfs_disk_open(&disk, IMAGE) < 0) {
        perror("open/recover image");
        return 1;
    }
    if (vfs_disk_read_block(&disk, 20, actual) < 0 ||
        memcmp(actual, before, sizeof(before)) != 0) {
        fprintf(stderr, "FAIL: pending journal was not recovered\n");
        vfs_disk_close(&disk);
        return 1;
    }
    if (vfs_disk_close(&disk) < 0) { perror("close recovered image"); return 1; }
    unlink(IMAGE);
    puts("Journal recovery test passed.");
    return 0;
}
