
#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_dir.h"
#include "vfs_path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_path.img"
#define TEST_BLOCKS 1024ULL

static int fail(const char *message, struct vfs_disk *disk)
{
    fprintf(stderr, "FAIL: %s\n", message);

    if (disk != NULL && disk->fd >= 0) {
        (void)vfs_disk_close(disk);
    }

    (void)unlink(TEST_IMAGE);
    return EXIT_FAILURE;
}

#define CHECK(condition, message)                 \
    do {                                           \
        if (!(condition)) {                       \
            perror(message);                      \
            return fail(message, &disk);          \
        }                                          \
    } while (0)

int main(void)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    struct vfs_superblock sb;

    uint64_t docs_inode;
    uint64_t nested_inode;
    uint64_t file_inode;
    uint64_t resolved_inode;

    char long_path[VFS_NAME_MAX + 3U];

    (void)unlink(TEST_IMAGE);

    printf("[1/5] Creating and formatting filesystem\n");

    CHECK(vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) == 0,
          "vfs_disk_create");
    CHECK(vfs_format_disk(&disk) == 0, "vfs_format_disk");
    CHECK(vfs_read_superblock(&disk, &sb) == 0, "vfs_read_superblock");

    printf("[2/5] Creating nested directory structure\n");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_DIRECTORY,
              0755, 1000, 1000, &docs_inode) == 0,
          "allocate docs inode");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_DIRECTORY,
              0755, 1000, 1000, &nested_inode) == 0,
          "allocate nested inode");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_REGULAR,
              0644, 1000, 1000, &file_inode) == 0,
          "allocate file inode");

    CHECK(vfs_dir_add(
              &disk, &sb, sb.root_inode,
              "docs", docs_inode) == 0,
          "add docs entry");

    CHECK(vfs_dir_add(
              &disk, &sb, docs_inode,
              "nested", nested_inode) == 0,
          "add nested entry");

    CHECK(vfs_dir_add(
              &disk, &sb, nested_inode,
              "notes.txt", file_inode) == 0,
          "add notes entry");

    printf("[3/5] Testing successful path resolution\n");

    CHECK(vfs_path_resolve(
              &disk, &sb, "/", &resolved_inode) == 0,
          "resolve root");
    if (resolved_inode != sb.root_inode) {
        return fail("root path resolved to wrong inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs", &resolved_inode) == 0,
          "resolve docs");
    if (resolved_inode != docs_inode) {
        return fail("docs path resolved to wrong inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/nested", &resolved_inode) == 0,
          "resolve nested");
    if (resolved_inode != nested_inode) {
        return fail("nested path resolved to wrong inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/nested/notes.txt",
              &resolved_inode) == 0,
          "resolve notes.txt");
    if (resolved_inode != file_inode) {
        return fail("file path resolved to wrong inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "//docs///nested/notes.txt/",
              &resolved_inode) == 0,
          "resolve repeated separators");
    if (resolved_inode != file_inode) {
        return fail("repeated-separator path resolved incorrectly", &disk);
    }

    printf("[4/5] Testing invalid paths and traversal errors\n");

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "docs/nested/notes.txt",
            &resolved_inode) != -1 || errno != EINVAL) {
        return fail("relative path was not rejected with EINVAL", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "", &resolved_inode) != -1 || errno != EINVAL) {
        return fail("empty path was not rejected with EINVAL", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "/docs/missing.txt",
            &resolved_inode) != -1 || errno != ENOENT) {
        return fail("missing path did not return ENOENT", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "/docs/nested/notes.txt/child",
            &resolved_inode) != -1 || errno != ENOTDIR) {
        return fail("file traversal did not return ENOTDIR", &disk);
    }

    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1U] = '\0';

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, long_path, &resolved_inode) != -1 ||
        errno != ENAMETOOLONG) {
        return fail("overlong path component was not rejected", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "/", NULL) != -1 || errno != EINVAL) {
        return fail("NULL output pointer was not rejected", &disk);
    }

    printf("[5/5] Flushing and closing filesystem\n");

    CHECK(vfs_disk_flush(&disk) == 0, "vfs_disk_flush");
    CHECK(vfs_disk_close(&disk) == 0, "vfs_disk_close");

    (void)unlink(TEST_IMAGE);

    printf("Path resolution tests: PASS\n");
    return EXIT_SUCCESS;
}
