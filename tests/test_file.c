#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_file.h"
#include "vfs_dir.h"
#include "vfs_alloc.h"
#include "vfs_path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_file.img"
#define TEST_BLOCKS 1024ULL

static int fail(const char *message, struct vfs_disk *disk)
{
    fprintf(stderr, "FAIL: %s\n", message);

    if (disk != NULL && disk->fd >= 0) {
        (void)vfs_disk_close(disk);
        disk->fd = -1;
    }

    (void)unlink(TEST_IMAGE);
    return EXIT_FAILURE;
}

#define CHECK(condition, message)                  \
    do {                                            \
        if (!(condition)) {                        \
            perror(message);                       \
            return fail(message, &disk);           \
        }                                           \
    } while (0)

#define EXPECT_ERROR(expression, expected_errno, message) \
    do {                                                    \
        errno = 0;                                          \
        if ((expression) != -1 || errno != (expected_errno)) { \
            fprintf(stderr, "FAIL: %s\n", message);         \
            return fail(message, &disk);                   \
        }                                                   \
    } while (0)

int main(void)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    struct vfs_superblock sb;
    struct vfs_inode inode;

    uint64_t file_inode;
    uint64_t nested_inode;
    uint64_t resolved_inode;
    size_t bytes_read;
    unsigned char buffer[32];

    (void)unlink(TEST_IMAGE);

    printf("[1/7] Creating and formatting test filesystem\n");

    CHECK(vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) == 0,
          "vfs_disk_create");
    CHECK(vfs_format_disk(&disk) == 0, "vfs_format_disk");
    CHECK(vfs_read_superblock(&disk, &sb) == 0,
          "vfs_read_superblock");

    printf("[2/7] Creating and reading an empty root-level file\n");

    CHECK(vfs_file_create(
              &disk, &sb, "/hello.txt",
              0644, 1000, 1000, &file_inode) == 0,
          "create hello.txt");

    CHECK(vfs_inode_read(
              &disk, &sb, file_inode, &inode) == 0,
          "read file inode");

    if (inode.type != VFS_INODE_REGULAR ||
        inode.size != 0 ||
        inode.blocks_count != 0) {
        return fail("new file should have an empty regular-file inode",
                    &disk);
    }

    memset(buffer, 0xA5, sizeof(buffer));
    bytes_read = 99;

    CHECK(vfs_file_read(
              &disk, &sb, "/hello.txt",
              buffer, sizeof(buffer), &bytes_read) == 0,
          "read empty file");

    if (bytes_read != 0) {
        return fail("empty file should return zero bytes", &disk);
    }

    printf("[3/7] Rejecting duplicate names and invalid paths\n");

    EXPECT_ERROR(
        vfs_file_create(
            &disk, &sb, "/hello.txt",
            0644, 1000, 1000, NULL),
        EEXIST, "duplicate filename should return EEXIST");

    EXPECT_ERROR(
        vfs_file_create(
            &disk, &sb, "relative.txt",
            0644, 1000, 1000, NULL),
        EINVAL, "relative path should return EINVAL");

    EXPECT_ERROR(
        vfs_file_create(
            &disk, &sb, "/",
            0644, 1000, 1000, NULL),
        EINVAL, "root path cannot be a filename");

    EXPECT_ERROR(
        vfs_file_create(
            &disk, &sb, "/missing/file.txt",
            0644, 1000, 1000, NULL),
        ENOENT, "missing parent should return ENOENT");

    EXPECT_ERROR(
        vfs_file_read(
            &disk, &sb, "/missing.txt",
            buffer, sizeof(buffer), &bytes_read),
        ENOENT, "missing file should return ENOENT");

    EXPECT_ERROR(
        vfs_file_read(
            &disk, &sb, "/hello.txt",
            NULL, sizeof(buffer), &bytes_read),
        EINVAL, "NULL buffer with nonzero capacity should fail");

    printf("[4/7] Creating and reading a nested-path file\n");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_DIRECTORY,
              0755, 1000, 1000, &nested_inode) == 0,
          "allocate docs directory");

    CHECK(vfs_dir_add(
              &disk, &sb, sb.root_inode,
              "docs", nested_inode) == 0,
          "add docs directory entry");

    CHECK(vfs_file_create(
              &disk, &sb, "/docs/notes.txt",
              0600, 1000, 1000, NULL) == 0,
          "create nested file");

    CHECK(vfs_file_read(
              &disk, &sb, "/docs/notes.txt",
              buffer, sizeof(buffer), &bytes_read) == 0,
          "read nested empty file");

    if (bytes_read != 0) {
        return fail("new nested file should be empty", &disk);
    }

    printf("[5/7] Rejecting directory reads and invalid arguments\n");

    EXPECT_ERROR(
        vfs_file_read(
            &disk, &sb, "/docs",
            buffer, sizeof(buffer), &bytes_read),
        EISDIR, "reading a directory as a file should fail");

    EXPECT_ERROR(
        vfs_file_read(
            &disk, &sb, "/hello.txt",
            buffer, sizeof(buffer), NULL),
        EINVAL, "NULL bytes_read should fail");

    /*
     * A zero-capacity read is a successful no-op.
     * EXPECT_ERROR is inappropriate here because it expects -1.
     */
    bytes_read = 99;

    CHECK(
        vfs_file_read(
            &disk, &sb, "/hello.txt",
            NULL, 0, &bytes_read) == 0,
        "zero-capacity read should succeed");

    if (bytes_read != 0) {
        return fail("zero-capacity read should return zero bytes",
                    &disk);
    }

    printf("[6/7] Verifying persistence after reopening the image\n");

    CHECK(vfs_disk_flush(&disk) == 0, "flush disk");
    CHECK(vfs_disk_close(&disk) == 0, "close disk");

    disk.fd = -1;
    disk.total_blocks = 0;

    CHECK(vfs_disk_open(&disk, TEST_IMAGE) == 0, "reopen disk");
    CHECK(vfs_read_superblock(&disk, &sb) == 0,
          "read superblock after reopen");

    CHECK(vfs_file_read(
              &disk, &sb, "/hello.txt",
              buffer, sizeof(buffer), &bytes_read) == 0,
          "read persisted hello.txt");

    if (bytes_read != 0) {
        return fail("persisted empty file should remain empty", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/notes.txt",
              &resolved_inode) == 0,
          "resolve persisted nested file");

    printf("[7/7] Closing and cleaning up\n");

    CHECK(vfs_disk_close(&disk) == 0, "final close");
    disk.fd = -1;

    (void)unlink(TEST_IMAGE);

    printf("File creation and reading tests: PASS\n");
    return EXIT_SUCCESS;
}