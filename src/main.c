#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_check.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUBHI_VERSION "0.1.0"

static void print_usage(const char *program)
{
    printf("Subhi-OS v%s\n", SUBHI_VERSION);
    printf("A Linux-based virtual filesystem and storage engine.\n\n");

    printf("Usage:\n");
    printf("  %s --help\n", program);
    printf("  %s --version\n", program);
    printf("  %s format <image> <blocks>\n", program);
    printf("  %s info <image>\n", program);
    printf("  %s check <image>\n", program);
}

static int parse_block_count(const char *text, uint64_t *blocks)
{
    if (text == NULL || text[0] == '\0' ||
        text[0] == '-' || blocks == NULL) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno == ERANGE || end == text || *end != '\0' ||
        value == 0) {
        errno = EINVAL;
        return -1;
    }

    *blocks = (uint64_t)value;
    return 0;
}

static int command_format(const char *path, const char *block_text)
{
    uint64_t blocks;

    if (parse_block_count(block_text, &blocks) < 0) {
        fprintf(stderr, "Error: invalid block count '%s'\n", block_text);
        return 2;
    }

    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};

    if (vfs_disk_create(&disk, path, blocks) < 0) {
        perror("Error creating disk image");
        return 1;
    }

    if (vfs_format_disk(&disk) < 0) {
        int saved_errno = errno;
        perror("Error formatting filesystem");
        (void)vfs_disk_close(&disk);
        (void)unlink(path);
        errno = saved_errno;
        return 1;
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("Error closing disk image");
        return 1;
    }

    printf("Filesystem formatted successfully.\n");
    printf("Image: %s\n", path);
    printf("Block size: %u bytes\n", VFS_BLOCK_SIZE);
    printf("Total blocks: %" PRIu64 "\n", blocks);

    return 0;
}

static int command_check(const char *path)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    if (vfs_disk_open(&disk, path) < 0) {
        perror("Error opening disk image");
        return 1;
    }

    int result = vfs_check_filesystem(&disk, stdout);
    int saved_errno = errno;
    if (result < 0) {
        errno = saved_errno;
        perror("Error checking filesystem");
    }
    if (vfs_disk_close(&disk) < 0) {
        perror("Error closing disk image");
        return 1;
    }
    return result < 0 ? 1 : result;
}

static int command_info(const char *path)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    struct vfs_superblock sb;

    if (vfs_disk_open(&disk, path) < 0) {
        perror("Error opening disk image");
        return 1;
    }

    if (vfs_read_superblock(&disk, &sb) < 0) {
        perror("Error: invalid or unsupported Subhi-OS filesystem");
        (void)vfs_disk_close(&disk);
        return 1;
    }

    printf("Subhi-OS filesystem information\n");
    printf("Version: %" PRIu32 "\n", sb.version);
    printf("Block size: %" PRIu32 " bytes\n", sb.block_size);
    printf("Total blocks: %" PRIu64 "\n", sb.total_blocks);
    printf("Block bitmap start: %" PRIu64 "\n", sb.bitmap_start);
    printf("Block bitmap blocks: %" PRIu64 "\n", sb.bitmap_blocks);
    printf("Inode bitmap start: %" PRIu64 "\n", sb.inode_bitmap_start);
    printf("Inode bitmap blocks: %" PRIu64 "\n", sb.inode_bitmap_blocks);
    printf("Inode table start: %" PRIu64 "\n", sb.inode_table_start);
    printf("Inode table blocks: %" PRIu64 "\n", sb.inode_table_blocks);
    printf("Inode count: %" PRIu64 "\n", sb.inode_count);
    printf("Data region starts: %" PRIu64 "\n", sb.data_start);
    printf("Free data blocks: %" PRIu64 "\n", sb.free_blocks);
    printf("Root inode: %" PRIu64 "\n", sb.root_inode);

    if (vfs_disk_close(&disk) < 0) {
        perror("Error closing disk image");
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("Subhi-OS v%s\n", SUBHI_VERSION);
        return 0;
    }

    if (argc == 4 && strcmp(argv[1], "format") == 0) {
        return command_format(argv[2], argv[3]);
    }

    if (argc == 3 && strcmp(argv[1], "info") == 0) {
        return command_info(argv[2]);
    }

    if (argc == 3 && strcmp(argv[1], "check") == 0) {
        return command_check(argv[2]);
    }

    print_usage(argv[0]);
    return 2;
}
