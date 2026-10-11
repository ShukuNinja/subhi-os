
#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_file.h"
#include "vfs_dir.h"
#include "vfs_alloc.h"
#include "vfs_path.h"
#include "vfs_perm.h"

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
    struct vfs_superblock sb_check;
    struct vfs_inode inode;
    const struct vfs_credentials test_credentials = {
        .uid = 1000U, .gid = 1000U
    };
    const struct vfs_credentials other_credentials = {
        .uid = 2000U, .gid = 2000U
    };

    uint64_t file_inode;
    uint64_t nested_inode;
    uint64_t resolved_inode;
    uint64_t delete_inode;
    uint64_t free_blocks_before;
    uint64_t free_blocks_after;

    size_t bytes_read;
    unsigned char buffer[32];

    const char delete_contents[] = "delete me";

    (void)unlink(TEST_IMAGE);

    printf("[1/8] Creating and formatting test filesystem\n");

    CHECK(vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) == 0,
          "vfs_disk_create");

    CHECK(vfs_format_disk(&disk) == 0,
          "vfs_format_disk");

    CHECK(vfs_read_superblock(&disk, &sb) == 0,
          "vfs_read_superblock");

    CHECK(vfs_inode_read(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "read root inode before configuring test permissions");
    inode.uid = test_credentials.uid;
    inode.gid = test_credentials.gid;
    inode.mode = 0755U;
    CHECK(vfs_inode_write(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "configure root directory permissions");

    printf("[2/8] Creating and reading an empty root-level file\n");

    CHECK(vfs_file_create(&disk, &sb, "/hello.txt", 0644, 1000, 1000, &test_credentials, &file_inode) == 0,
          "create hello.txt");

    CHECK(vfs_inode_read(
              &disk, &sb, file_inode, &inode) == 0,
          "read file inode");

    if (inode.type != VFS_INODE_REGULAR ||
        inode.size != 0 ||
        inode.blocks_count != 0) {
        return fail(
            "new file should have an empty regular-file inode",
            &disk);
    }

    memset(buffer, 0xA5, sizeof(buffer));
    bytes_read = 99;

    CHECK(vfs_file_read(&disk, &sb, "/hello.txt", buffer, sizeof(buffer), &bytes_read, &test_credentials) == 0,
          "read empty file");

    if (bytes_read != 0) {
        return fail("empty file should return zero bytes", &disk);
    }

    CHECK(vfs_inode_read(
              &disk, &sb, file_inode, &inode) == 0,
          "read hello.txt inode before permission tests");
    inode.mode = 0640U;
    CHECK(vfs_inode_write(
              &disk, &sb, file_inode, &inode) == 0,
          "set hello.txt restrictive permissions");
    EXPECT_ERROR(
        vfs_file_read(
            &disk, &sb, "/hello.txt",
            buffer, sizeof(buffer), &bytes_read, &other_credentials),
        EACCES,
        "reading without file read permission should return EACCES");
    EXPECT_ERROR(
        vfs_file_write(
            &disk, &sb, "/hello.txt", "x", 1U, &other_credentials),
        EACCES,
        "writing without file write permission should return EACCES");
    CHECK(vfs_file_read(
              &disk, &sb, "/hello.txt",
              buffer, sizeof(buffer), &bytes_read, &test_credentials) == 0,
          "owner should retain read permission");
    inode.mode = 0644U;
    CHECK(vfs_inode_write(
              &disk, &sb, file_inode, &inode) == 0,
          "restore hello.txt permissions");

    printf("[3/8] Rejecting duplicate names and invalid paths\n");

    EXPECT_ERROR(
        vfs_file_create(&disk, &sb, "/hello.txt", 0644, 1000, 1000, &test_credentials, NULL),
        EEXIST,
        "duplicate filename should return EEXIST");

    EXPECT_ERROR(
        vfs_file_create(&disk, &sb, "relative.txt", 0644, 1000, 1000, &test_credentials, NULL),
        EINVAL,
        "relative path should return EINVAL");

    EXPECT_ERROR(
        vfs_file_create(&disk, &sb, "/", 0644, 1000, 1000, &test_credentials, NULL),
        EINVAL,
        "root path cannot be a filename");

    EXPECT_ERROR(
        vfs_file_create(&disk, &sb, "/missing/file.txt", 0644, 1000, 1000, &test_credentials, NULL),
        ENOENT,
        "missing parent should return ENOENT");

    EXPECT_ERROR(
        vfs_file_read(&disk, &sb, "/missing.txt", buffer, sizeof(buffer), &bytes_read, &test_credentials),
        ENOENT,
        "missing file should return ENOENT");

    EXPECT_ERROR(
        vfs_file_read(&disk, &sb, "/hello.txt", NULL, sizeof(buffer), &bytes_read, &test_credentials),
        EINVAL,
        "NULL buffer with nonzero capacity should fail");

    CHECK(vfs_inode_read(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "read root inode before create-denial test");
    inode.mode = 0555U;
    CHECK(vfs_inode_write(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "remove root write permission temporarily");
    EXPECT_ERROR(
        vfs_file_create(
            &disk, &sb, "/denied.txt",
            0644U, 1000U, 1000U, &test_credentials, NULL),
        EACCES,
        "create without parent write permission should return EACCES");
    inode.mode = 0755U;
    CHECK(vfs_inode_write(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "restore root directory permissions");

    printf("[4/8] Creating and reading a nested-path file\n");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_DIRECTORY,
              0755, 1000, 1000, &nested_inode) == 0,
          "allocate docs directory");

    CHECK(vfs_dir_add(
              &disk, &sb, sb.root_inode,
              "docs", nested_inode) == 0,
          "add docs directory entry");

    CHECK(vfs_file_create(&disk, &sb, "/docs/notes.txt", 0600, 1000, 1000, &test_credentials, NULL) == 0,
          "create nested file");

    CHECK(vfs_file_read(&disk, &sb, "/docs/notes.txt", buffer, sizeof(buffer), &bytes_read, &test_credentials) == 0,
          "read nested empty file");

    if (bytes_read != 0) {
        return fail("new nested file should be empty", &disk);
    }

    printf("[5/8] Rejecting directory reads and invalid arguments\n");

    EXPECT_ERROR(
        vfs_file_read(&disk, &sb, "/docs", buffer, sizeof(buffer), &bytes_read, &test_credentials),
        EISDIR,
        "reading a directory as a file should fail");

    EXPECT_ERROR(
        vfs_file_read(&disk, &sb, "/hello.txt", buffer, sizeof(buffer), NULL, &test_credentials),
        EINVAL,
        "NULL bytes_read should fail");

    /*
     * A zero-capacity read is a successful no-op.
     */
    bytes_read = 99;

    CHECK(
        vfs_file_read(&disk, &sb, "/hello.txt", NULL, 0, &bytes_read, &test_credentials) == 0,
        "zero-capacity read should succeed");

    if (bytes_read != 0) {
        return fail(
            "zero-capacity read should return zero bytes",
            &disk);
    }

    printf("[6/8] Testing file deletion and resource reclamation\n");

    /*
     * Create a separate file so the original persistence tests
     * for /hello.txt and /docs/notes.txt remain unchanged.
     */
    CHECK(vfs_file_create(&disk, &sb, "/delete-me.bin", 0644, 1000, 1000, &test_credentials, &delete_inode) == 0,
          "create file for deletion");

    CHECK(vfs_inode_is_allocated(
              &disk, &sb, delete_inode) == 1,
          "deletion-test inode should initially be allocated");

    CHECK(vfs_read_superblock(
              &disk, &sb_check) == 0,
          "read superblock before writing deletion-test file");

    free_blocks_before = sb_check.free_blocks;

    CHECK(vfs_file_write(&disk, &sb, "/delete-me.bin", delete_contents, sizeof(delete_contents) - 1U, &test_credentials) == 0,
          "write contents before deletion");

    CHECK(vfs_inode_read(
              &disk, &sb, delete_inode, &inode) == 0,
          "read populated inode before deletion");

    if (inode.type != VFS_INODE_REGULAR ||
        inode.size != sizeof(delete_contents) - 1U ||
        inode.blocks_count != 1U) {
        return fail(
            "populated file should own exactly one data block",
            &disk);
    }

    uint64_t deleted_data_block = inode.direct_blocks[0];

    CHECK(vfs_block_is_allocated(
              &disk, deleted_data_block) == 1,
          "data block should be allocated before deletion");

    CHECK(vfs_read_superblock(
              &disk, &sb_check) == 0,
          "read superblock after writing deletion-test file");

    if (sb_check.free_blocks + 1U != free_blocks_before) {
        return fail(
            "writing file should consume exactly one data block",
            &disk);
    }

    CHECK(vfs_inode_read(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "read root inode before unlink-denial test");
    inode.mode = 0555U;
    CHECK(vfs_inode_write(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "remove root write permission temporarily");
    EXPECT_ERROR(
        vfs_file_unlink(
            &disk, &sb, "/delete-me.bin", &test_credentials),
        EACCES,
        "unlink without parent write permission should return EACCES");
    inode.mode = 0755U;
    CHECK(vfs_inode_write(
              &disk, &sb, sb.root_inode, &inode) == 0,
          "restore root directory permissions");

    CHECK(vfs_file_unlink(&disk, &sb, "/delete-me.bin", &test_credentials) == 0,
          "unlink populated file");

    /* The pathname must no longer resolve. */
    EXPECT_ERROR(
        vfs_path_resolve(
            &disk, &sb, "/delete-me.bin", &resolved_inode),
        ENOENT,
        "deleted file should no longer exist");

    /* The inode allocation bit must be cleared. */
    CHECK(vfs_inode_is_allocated(
              &disk, &sb, delete_inode) == 0,
          "deleted file inode should be released");

    /* Its former data block must now be free. */
    CHECK(vfs_block_is_allocated(
              &disk, deleted_data_block) == 0,
          "deleted file data block should be released");

    CHECK(vfs_read_superblock(
              &disk, &sb_check) == 0,
          "read superblock after deletion");

    free_blocks_after = sb_check.free_blocks;

    if (free_blocks_after != free_blocks_before) {
        return fail(
            "unlink should restore the original free-block count",
            &disk);
    }

    /* Repeated deletion must fail without corrupting the filesystem. */
    EXPECT_ERROR(
        vfs_file_unlink(&disk, &sb, "/delete-me.bin", &test_credentials),
        ENOENT,
        "unlinking a nonexistent file should return ENOENT");

    /* Directories must not be removed through the file API. */
    EXPECT_ERROR(
        vfs_file_unlink(&disk, &sb, "/docs", &test_credentials),
        EISDIR,
        "unlinking a directory should return EISDIR");

    EXPECT_ERROR(
        vfs_file_unlink(&disk, &sb, "/missing.txt", &test_credentials),
        ENOENT,
        "unlinking a missing path should return ENOENT");

    EXPECT_ERROR(
        vfs_file_unlink(&disk, &sb, NULL, &test_credentials),
        EINVAL,
        "unlinking with a NULL path should return EINVAL");

    EXPECT_ERROR(
        vfs_file_unlink(&disk, &sb, "/", &test_credentials),
        EINVAL,
        "unlinking the root path should return EINVAL");

    printf("File deletion and reclamation tests: PASS\n");

    printf("[7/8] Verifying persistence after reopening the image\n");

    CHECK(vfs_disk_flush(&disk) == 0,
          "flush disk");

    CHECK(vfs_disk_close(&disk) == 0,
          "close disk");

    disk.fd = -1;
    disk.total_blocks = 0;

    CHECK(vfs_disk_open(&disk, TEST_IMAGE) == 0,
          "reopen disk");

    CHECK(vfs_read_superblock(&disk, &sb) == 0,
          "read superblock after reopen");

    CHECK(vfs_file_read(&disk, &sb, "/hello.txt", buffer, sizeof(buffer), &bytes_read, &test_credentials) == 0,
          "read persisted hello.txt");

    if (bytes_read != 0) {
        return fail(
            "persisted empty file should remain empty",
            &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/notes.txt",
              &resolved_inode) == 0,
          "resolve persisted nested file");

    EXPECT_ERROR(
        vfs_path_resolve(
            &disk, &sb, "/delete-me.bin", &resolved_inode),
        ENOENT,
        "deleted file should remain absent after reopening");

    CHECK(vfs_inode_is_allocated(
              &disk, &sb, delete_inode) == 0,
          "deleted inode should remain free after reopening");

    CHECK(vfs_block_is_allocated(
              &disk, deleted_data_block) == 0,
          "deleted data block should remain free after reopening");

    printf("[8/8] Closing and cleaning up\n");

    CHECK(vfs_disk_close(&disk) == 0,
          "final close");

    disk.fd = -1;

    (void)unlink(TEST_IMAGE);

    printf("File operation tests: PASS\n");
    return EXIT_SUCCESS;
}