#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_inode.img"
#define TEST_BLOCKS 1024ULL

static int fail(
    const char *message,
    struct vfs_disk *disk
)
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
    struct vfs_inode inode;

    uint64_t allocated[VFS_INODE_COUNT - 1];
    size_t allocated_count = 0;
    uint64_t first_inode = 0;

    (void)unlink(TEST_IMAGE);

    printf("[1/5] Formatting filesystem and checking root inode\n");

    if (vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) < 0 ||
        vfs_format_disk(&disk) < 0 ||
        vfs_read_superblock(&disk, &sb) < 0) {
        perror("filesystem setup");
        return fail("filesystem setup failed", &disk);
    }

    if (vfs_inode_is_allocated(&disk, &sb, 0) != 1) {
        return fail("root inode is not allocated", &disk);
    }

    errno = 0;

    if (vfs_inode_release(&disk, &sb, 0) != -1 ||
        errno != EINVAL) {
        return fail("root inode release was not rejected", &disk);
    }

    printf("[2/5] Allocating and reading a regular-file inode\n");

    if (vfs_inode_allocate(
            &disk, &sb, VFS_INODE_REGULAR,
            0644, 1000, 1000, &first_inode) < 0) {
        perror("vfs_inode_allocate");
        return fail("inode allocation failed", &disk);
    }

    if (first_inode != 1 ||
        vfs_inode_read(&disk, &sb, first_inode, &inode) < 0) {
        return fail("allocated inode could not be read", &disk);
    }

    if (inode.type != VFS_INODE_REGULAR ||
        inode.mode != 0644 ||
        inode.uid != 1000 ||
        inode.gid != 1000 ||
        inode.size != 0 ||
        inode.blocks_count != 0) {
        return fail("allocated inode metadata is incorrect", &disk);
    }

    printf("[3/5] Preventing release of an inode with blocks\n");

    inode.blocks_count = 1;

    if (vfs_inode_write(&disk, &sb, first_inode, &inode) < 0) {
        perror("vfs_inode_write");
        return fail("could not prepare release test", &disk);
    }

    errno = 0;

    if (vfs_inode_release(&disk, &sb, first_inode) != -1 ||
        errno != EBUSY) {
        return fail("non-empty inode release was not rejected", &disk);
    }

    inode.blocks_count = 0;

    if (vfs_inode_write(&disk, &sb, first_inode, &inode) < 0 ||
        vfs_inode_release(&disk, &sb, first_inode) < 0) {
        perror("inode reset/release");
        return fail("empty inode release failed", &disk);
    }

    if (vfs_inode_is_allocated(&disk, &sb, first_inode) != 0) {
        return fail("released inode remains allocated", &disk);
    }

    printf("[4/5] Reusing a released inode\n");

    uint64_t reused_inode = 0;

    if (vfs_inode_allocate(
            &disk, &sb, VFS_INODE_DIRECTORY,
            0755, 1000, 1000, &reused_inode) < 0) {
        perror("vfs_inode_allocate");
        return fail("inode reuse allocation failed", &disk);
    }

    if (reused_inode != first_inode) {
        return fail("allocator did not reuse the lowest free inode",
                    &disk);
    }

    if (vfs_inode_release(&disk, &sb, reused_inode) < 0) {
        perror("vfs_inode_release");
        return fail("reused inode release failed", &disk);
    }

    printf("[5/5] Detecting inode-table exhaustion\n");

    for (size_t i = 0; i < VFS_INODE_COUNT - 1; i++) {
        if (vfs_inode_allocate(
                &disk, &sb, VFS_INODE_REGULAR,
                0600, 1000, 1000, &allocated[i]) < 0) {
            perror("vfs_inode_allocate");
            return fail("unexpected inode allocation failure", &disk);
        }

        allocated_count++;
    }

    uint64_t extra_inode;

    errno = 0;

    if (vfs_inode_allocate(
            &disk, &sb, VFS_INODE_REGULAR,
            0600, 1000, 1000, &extra_inode) != -1 ||
        errno != ENOSPC) {
        return fail("inode exhaustion was not reported", &disk);
    }

    for (size_t i = 0; i < allocated_count; i++) {
        if (vfs_inode_release(&disk, &sb, allocated[i]) < 0) {
            perror("vfs_inode_release");
            return fail("could not clean up allocated inode", &disk);
        }
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("disk close failed", NULL);
    }

    if (unlink(TEST_IMAGE) < 0) {
        perror("unlink");
        return fail("test image cleanup failed", NULL);
    }

    printf("All inode tests passed.\n");
    return 0;
}
