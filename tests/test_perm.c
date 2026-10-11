
#include "vfs_perm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define EXPECT_ERROR(expression, expected_errno) do { \
    errno = 0; \
    CHECK((expression) == -1); \
    CHECK(errno == (expected_errno)); \
} while (0)

int main(void)
{
    struct vfs_inode inode = {0};
    struct vfs_credentials owner = {.uid = 1000U, .gid = 100U};
    struct vfs_credentials group_user = {.uid = 2000U, .gid = 100U};
    struct vfs_credentials other_user = {.uid = 3000U, .gid = 300U};

    printf("[1/5] Owner permissions\n");
    inode.uid = 1000U;
    inode.gid = 100U;
    inode.mode = 0640U;

    CHECK(vfs_check_access(&inode, &owner, VFS_ACCESS_READ) == 0);
    CHECK(vfs_check_access(&inode, &owner, VFS_ACCESS_WRITE) == 0);
    EXPECT_ERROR(
        vfs_check_access(&inode, &owner, VFS_ACCESS_EXECUTE),
        EACCES
    );

    printf("[2/5] Group permissions\n");
    CHECK(vfs_check_access(&inode, &group_user, VFS_ACCESS_READ) == 0);
    EXPECT_ERROR(
        vfs_check_access(&inode, &group_user, VFS_ACCESS_WRITE),
        EACCES
    );

    printf("[3/5] Other-user permissions\n");
    EXPECT_ERROR(
        vfs_check_access(&inode, &other_user, VFS_ACCESS_READ),
        EACCES
    );
    EXPECT_ERROR(
        vfs_check_access(&inode, &other_user, VFS_ACCESS_WRITE),
        EACCES
    );

    printf("[4/5] Combined access requirements\n");
    inode.mode = 0660U;
    CHECK(vfs_check_access(
        &inode, &owner, VFS_ACCESS_READ | VFS_ACCESS_WRITE) == 0);
    CHECK(vfs_check_access(
        &inode, &group_user, VFS_ACCESS_READ | VFS_ACCESS_WRITE) == 0);
    EXPECT_ERROR(
        vfs_check_access(
            &inode, &other_user, VFS_ACCESS_READ | VFS_ACCESS_WRITE),
        EACCES
    );

    printf("[5/5] Invalid arguments\n");
    EXPECT_ERROR(vfs_check_access(NULL, &owner, VFS_ACCESS_READ), EINVAL);
    EXPECT_ERROR(vfs_check_access(&inode, NULL, VFS_ACCESS_READ), EINVAL);
    EXPECT_ERROR(vfs_check_access(&inode, &owner, 0U), EINVAL);
    EXPECT_ERROR(vfs_check_access(&inode, &owner, 8U), EINVAL);

    puts("PASS: all permission tests");
    return EXIT_SUCCESS;
}
