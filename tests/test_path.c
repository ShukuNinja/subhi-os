
#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_dir.h"
#include "vfs_path.h"
#include "vfs_perm.h"

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

#define CHECK(condition, message)              \
    do {                                        \
        if (!(condition)) {                     \
            perror(message);                    \
            return fail(message, &disk);        \
        }                                       \
    } while (0)

int main(void)
{
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    struct vfs_superblock sb;
    struct vfs_credentials user = {
        .uid = 1000U,
        .gid = 1000U
    };
    struct vfs_inode saved_inode;
    uint64_t docs_inode;
    uint64_t nested_inode;
    uint64_t file_inode;
    uint64_t resolved_inode;
    char long_path[VFS_NAME_MAX + 3U];

    (void)unlink(TEST_IMAGE);

    CHECK(vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) == 0,
          "vfs_disk_create");
    CHECK(vfs_format_disk(&disk) == 0, "vfs_format_disk");
    CHECK(vfs_read_superblock(&disk, &sb) == 0,
          "vfs_read_superblock");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_DIRECTORY,
              0755U, 1000U, 1000U, &docs_inode) == 0,
          "allocate docs inode");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_DIRECTORY,
              0755U, 1000U, 1000U, &nested_inode) == 0,
          "allocate nested inode");

    CHECK(vfs_inode_allocate(
              &disk, &sb, VFS_INODE_REGULAR,
              0644U, 1000U, 1000U, &file_inode) == 0,
          "allocate file inode");

    CHECK(vfs_dir_add(
              &disk, &sb, sb.root_inode, "docs", docs_inode) == 0,
          "add docs entry");

    CHECK(vfs_dir_add(
              &disk, &sb, docs_inode, "nested", nested_inode) == 0,
          "add nested entry");

    CHECK(vfs_dir_add(
              &disk, &sb, nested_inode, "notes.txt", file_inode) == 0,
          "add notes entry");

    /*
     * Existing path-resolution regression tests.
     */
    CHECK(vfs_path_resolve(
              &disk, &sb, "/", &resolved_inode) == 0,
          "resolve root");
    if (resolved_inode != sb.root_inode) {
        return fail("incorrect root inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs", &resolved_inode) == 0,
          "resolve docs");
    if (resolved_inode != docs_inode) {
        return fail("incorrect docs inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/nested", &resolved_inode) == 0,
          "resolve nested");
    if (resolved_inode != nested_inode) {
        return fail("incorrect nested inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/nested/notes.txt",
              &resolved_inode) == 0,
          "resolve notes.txt");
    if (resolved_inode != file_inode) {
        return fail("incorrect file inode", &disk);
    }

    CHECK(vfs_path_resolve(
              &disk, &sb, "//docs///nested/notes.txt/",
              &resolved_inode) == 0,
          "resolve repeated separators");
    if (resolved_inode != file_inode) {
        return fail("incorrect inode for repeated separators", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "docs/nested/notes.txt",
            &resolved_inode) != -1 || errno != EINVAL) {
        return fail("relative path should return EINVAL", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "", &resolved_inode) != -1 || errno != EINVAL) {
        return fail("empty path should return EINVAL", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "/docs/missing",
            &resolved_inode) != -1 || errno != ENOENT) {
        return fail("missing entry should return ENOENT", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "/docs/nested/notes.txt/child",
            &resolved_inode) != -1 || errno != ENOTDIR) {
        return fail("traversal through file should return ENOTDIR", &disk);
    }

    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1U] = '\0';

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, long_path, &resolved_inode) != -1 ||
        errno != ENAMETOOLONG) {
        return fail("overlong component should return ENAMETOOLONG", &disk);
    }

    errno = 0;
    if (vfs_path_resolve(
            &disk, &sb, "/", NULL) != -1 || errno != EINVAL) {
        return fail("NULL output pointer should return EINVAL", &disk);
    }

    /*
     * Permission-aware path resolution.
     * Keep the disk open and the image present throughout these tests.
     */
    CHECK(vfs_path_resolve_as(
              &disk, &sb, "/docs/nested/notes.txt",
              &user, &resolved_inode) == 0,
          "resolve path with execute permissions");

    if (resolved_inode != file_inode) {
        return fail("incorrect permission-aware resolution", &disk);
    }

    /*
     * Save docs inode metadata before changing its permissions.
     */
    CHECK(vfs_inode_read(
              &disk, &sb, docs_inode, &saved_inode) == 0,
          "read docs inode before permission test");

    /*
     * Remove execute permission from docs.
     * The caller owns docs, so mode 0644 grants read/write but not execute.
     */
    {
        struct vfs_inode modified_inode = saved_inode;
        modified_inode.mode = 0644U;

        CHECK(vfs_inode_write(
                  &disk, &sb, docs_inode, &modified_inode) == 0,
              "remove docs execute permission");
    }

    errno = 0;
    if (vfs_path_resolve_as(
            &disk, &sb, "/docs/nested",
            &user, &resolved_inode) != -1 || errno != EACCES) {
        return fail(
            "traversal without execute permission must return EACCES",
            &disk);
    }

    /*
     * The legacy resolver deliberately remains permission-unaware.
     */
    CHECK(vfs_path_resolve(
              &disk, &sb, "/docs/nested",
              &resolved_inode) == 0,
          "legacy resolver compatibility");

    /*
     * Restore docs metadata before continuing.
     */
    CHECK(vfs_inode_write(
              &disk, &sb, docs_inode, &saved_inode) == 0,
          "restore docs inode permissions");

    errno = 0;
    if (vfs_path_resolve_as(
            &disk, &sb, "/docs/nested",
            NULL, &resolved_inode) != -1 || errno != EINVAL) {
        return fail("NULL credentials must return EINVAL", &disk);
    }

    CHECK(vfs_disk_flush(&disk) == 0, "vfs_disk_flush");
    CHECK(vfs_disk_close(&disk) == 0, "vfs_disk_close");

    (void)unlink(TEST_IMAGE);

    printf("Path resolution tests: PASS\n");
    printf("Permission-aware path resolution tests: PASS\n");

    return EXIT_SUCCESS;
}