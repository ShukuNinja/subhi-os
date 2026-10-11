#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_format.img"
#define TEST_BLOCKS 1024ULL

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    unlink(TEST_IMAGE);
    return 1;
}

int main(void)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    struct vfs_superblock sb;

    unsigned char block[VFS_BLOCK_SIZE];

    unlink(TEST_IMAGE);

    printf("[1/5] Creating and formatting a disk image\n");

    if (vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) < 0) {
        perror("vfs_disk_create");
        return fail("disk creation failed");
    }

    if (vfs_format_disk(&disk) < 0) {
        perror("vfs_format_disk");
        vfs_disk_close(&disk);
        return fail("formatting failed");
    }

    printf("[2/5] Validating superblock layout\n");

    if (vfs_read_superblock(&disk, &sb) < 0) {
        perror("vfs_read_superblock");
        vfs_disk_close(&disk);
        return fail("superblock validation failed");
    }

    if (sb.total_blocks != TEST_BLOCKS ||
        sb.bitmap_start != 1 ||
        sb.bitmap_blocks != 1 ||
        sb.inode_table_start != 2 ||
        sb.inode_table_blocks != 4 ||
        sb.data_start != 6 ||
        sb.free_blocks != TEST_BLOCKS - 6 ||
        sb.root_inode != 0) {
        vfs_disk_close(&disk);
        return fail("unexpected filesystem layout");
    }

    printf("[3/5] Checking allocation bitmap\n");

    if (vfs_disk_read_block(&disk, sb.bitmap_start, block) < 0) {
        perror("vfs_disk_read_block");
        vfs_disk_close(&disk);
        return fail("bitmap read failed");
    }

    for (uint64_t i = 0; i < sb.data_start; i++) {
        unsigned char mask = (unsigned char)(1U << (i % 8));

        if ((block[i / 8] & mask) == 0) {
            vfs_disk_close(&disk);
            return fail("metadata block was not marked allocated");
        }
    }

    if ((block[sb.data_start / 8] &
         (unsigned char)(1U << (sb.data_start % 8))) != 0) {
        vfs_disk_close(&disk);
        return fail("first data block should be free");
    }

    printf("[4/5] Checking inode table initialization\n");

    for (uint64_t i = 0; i < sb.inode_table_blocks; i++) {
        if (vfs_disk_read_block(
                &disk, sb.inode_table_start + i, block) < 0) {
            perror("vfs_disk_read_block");
            vfs_disk_close(&disk);
            return fail("inode table read failed");
        }

        for (size_t j = 0; j < sizeof(block); j++) {
            if (block[j] != 0) {
                vfs_disk_close(&disk);
                return fail("inode table was not zero-initialized");
            }
        }
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("disk close failed");
    }

    printf("[5/5] Rejecting an invalid superblock\n");

    if (vfs_disk_open(&disk, TEST_IMAGE) < 0) {
        perror("vfs_disk_open");
        return fail("disk reopen failed");
    }

    memset(block, 0, sizeof(block));

    if (vfs_disk_write_block(&disk, 0, block) < 0 ||
        vfs_disk_flush(&disk) < 0) {
        perror("corrupt test superblock");
        vfs_disk_close(&disk);
        return fail("could not prepare corruption test");
    }

    if (vfs_read_superblock(&disk, &sb) == 0) {
        vfs_disk_close(&disk);
        return fail("invalid superblock was accepted");
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("final disk close failed");
    }

    if (unlink(TEST_IMAGE) < 0) {
        perror("unlink");
        return fail("test image cleanup failed");
    }

    printf("All filesystem formatting tests passed.\n");
    return 0;
}
