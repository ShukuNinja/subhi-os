#define _POSIX_C_SOURCE 200809L

#include "vfs_alloc.h"
#include "vfs_disk.h"
#include "vfs_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_alloc.img"
#define TEST_BLOCKS 16ULL

static int fail(const char *message, struct vfs_disk *disk)
{
    fprintf(stderr, "FAIL: %s\n", message);

    if (disk != NULL && disk->fd >= 0) {
        (void)vfs_disk_close(disk);
    }

    (void)unlink(TEST_IMAGE);
    return 1;
}

int main(void)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    struct vfs_superblock sb;

    uint64_t allocated[TEST_BLOCKS];
    size_t allocated_count = 0;

    (void)unlink(TEST_IMAGE);

    printf("[1/6] Formatting a small filesystem\n");

    if (vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) < 0 ||
        vfs_format_disk(&disk) < 0 ||
        vfs_read_superblock(&disk, &sb) < 0) {
        perror("filesystem setup");
        return fail("filesystem setup failed", &disk);
    }

    if (sb.data_start != 7 || sb.free_blocks != 8) {
        return fail("unexpected initial free-space accounting", &disk);
    }

    printf("[2/6] Allocating data blocks in first-fit order\n");

    for (uint64_t expected = 8; expected < TEST_BLOCKS; expected++) {
        uint64_t actual = UINT64_MAX;

        if (vfs_block_allocate(&disk, &actual) < 0) {
            perror("vfs_block_allocate");
            return fail("data block allocation failed", &disk);
        }

        if (actual != expected) {
            fprintf(stderr,
                    "Expected block %" PRIu64 ", got %" PRIu64 "\n",
                    expected, actual);
            return fail("allocator returned an unexpected block", &disk);
        }

        allocated[allocated_count++] = actual;
    }

    if (vfs_read_superblock(&disk, &sb) < 0 ||
        sb.free_blocks != 0) {
        return fail("free-block count did not reach zero", &disk);
    }

    printf("[3/6] Detecting storage exhaustion\n");

    uint64_t extra;

    errno = 0;

    if (vfs_block_allocate(&disk, &extra) != -1 ||
        errno != ENOSPC) {
        return fail("disk-full condition was not reported", &disk);
    }

    printf("[4/6] Rejecting unsafe block releases\n");

    errno = 0;

    if (vfs_block_release(&disk, 0) != -1 ||
        errno != EINVAL) {
        return fail("superblock release was not rejected", &disk);
    }

    errno = 0;

    if (vfs_block_release(&disk, sb.data_start) != -1 ||
        errno != EBUSY) {
        return fail("root-directory block release was not rejected",
                    &disk);
    }

    errno = 0;

    if (vfs_block_release(&disk, TEST_BLOCKS) != -1 ||
        errno != EINVAL) {
        return fail("out-of-range release was not rejected", &disk);
    }

    printf("[5/6] Releasing and reusing a data block\n");

    uint64_t released = allocated[3];

    if (vfs_block_release(&disk, released) < 0) {
        perror("vfs_block_release");
        return fail("block release failed", &disk);
    }

    if (vfs_block_is_allocated(&disk, released) != 0) {
        return fail("released block remains allocated", &disk);
    }

    if (vfs_read_superblock(&disk, &sb) < 0 ||
        sb.free_blocks != 1) {
        return fail("free-block count did not increase", &disk);
    }

    uint64_t reused;

    if (vfs_block_allocate(&disk, &reused) < 0) {
        perror("vfs_block_allocate");
        return fail("block reuse failed", &disk);
    }

    if (reused != released) {
        return fail("allocator did not reuse the lowest free block",
                    &disk);
    }

    if (vfs_block_release(&disk, reused) < 0) {
        perror("vfs_block_release");
        return fail("final release failed", &disk);
    }

    errno = 0;

    if (vfs_block_release(&disk, reused) != -1 ||
        errno != EALREADY) {
        return fail("double release was not rejected", &disk);
    }

    printf("[6/6] Verifying persisted allocation metadata\n");

    if (vfs_disk_flush(&disk) < 0 ||
        vfs_disk_close(&disk) < 0) {
        perror("flush/close");
        return fail("could not close test image", &disk);
    }

    if (vfs_disk_open(&disk, TEST_IMAGE) < 0 ||
        vfs_read_superblock(&disk, &sb) < 0) {
        perror("reopen/validate");
        return fail("could not reopen filesystem", &disk);
    }

    if (sb.free_blocks != 1 ||
        vfs_block_is_allocated(&disk, reused) != 0) {
        return fail("allocation state did not persist", &disk);
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("disk close failed", NULL);
    }

    if (unlink(TEST_IMAGE) < 0) {
        perror("unlink");
        return fail("test image cleanup failed", NULL);
    }

    printf("All block allocation tests passed.\n");
    return 0;
}
