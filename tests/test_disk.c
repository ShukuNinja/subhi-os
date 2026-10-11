#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_disk.img"
#define TEST_BLOCKS 8

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    unlink(TEST_IMAGE);
    return 1;
}

int main(void)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};

    unsigned char written[VFS_BLOCK_SIZE];
    unsigned char read_back[VFS_BLOCK_SIZE];

    unlink(TEST_IMAGE);

    printf("[1/5] Creating an 8-block disk image\n");

    if (vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) < 0) {
        perror("vfs_disk_create");
        return fail("disk creation failed");
    }

    if (disk.total_blocks != TEST_BLOCKS) {
        vfs_disk_close(&disk);
        return fail("incorrect block count");
    }

    printf("[2/5] Writing block 3\n");

    for (size_t i = 0; i < sizeof(written); i++) {
        written[i] = (unsigned char)(i % 251);
    }

    if (vfs_disk_write_block(&disk, 3, written) < 0) {
        perror("vfs_disk_write_block");
        vfs_disk_close(&disk);
        return fail("block write failed");
    }

    if (vfs_disk_flush(&disk) < 0) {
        perror("vfs_disk_flush");
        vfs_disk_close(&disk);
        return fail("disk flush failed");
    }

    printf("[3/5] Closing and reopening the image\n");

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("disk close failed");
    }

    if (vfs_disk_open(&disk, TEST_IMAGE) < 0) {
        perror("vfs_disk_open");
        return fail("disk reopen failed");
    }

    printf("[4/5] Verifying persisted block contents\n");

    memset(read_back, 0, sizeof(read_back));

    if (vfs_disk_read_block(&disk, 3, read_back) < 0) {
        perror("vfs_disk_read_block");
        vfs_disk_close(&disk);
        return fail("block read failed");
    }

    if (memcmp(written, read_back, sizeof(written)) != 0) {
        vfs_disk_close(&disk);
        return fail("data mismatch after reopening");
    }

    printf("[5/5] Rejecting an out-of-range block\n");

    errno = 0;

    if (vfs_disk_read_block(&disk, TEST_BLOCKS, read_back) != -1 ||
        errno != EINVAL) {
        vfs_disk_close(&disk);
        return fail("out-of-range read was not rejected");
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("final disk close failed");
    }

    if (unlink(TEST_IMAGE) < 0) {
        perror("unlink");
        return fail("test image cleanup failed");
    }

    printf("All virtual disk tests passed.\n");
    return 0;
}
