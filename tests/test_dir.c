
#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_inode.h"
#include "vfs_dir.h"
#include "vfs_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_IMAGE "build/test_dir.img"
#define TEST_BLOCKS 1024ULL
#define TEST_ENTRIES 65U

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
    struct vfs_disk disk = {
        .fd = -1,
        .total_blocks = 0
    };

    struct vfs_superblock sb;
    uint64_t inodes[TEST_ENTRIES];

    (void)unlink(TEST_IMAGE);

    printf("[1/7] Formatting filesystem and allocating target inodes\n");

    if (vfs_disk_create(&disk, TEST_IMAGE, TEST_BLOCKS) < 0 ||
        vfs_format_disk(&disk) < 0 ||
        vfs_read_superblock(&disk, &sb) < 0) {
        perror("filesystem setup");
        return fail("filesystem setup failed", &disk);
    }

    for (size_t i = 0; i < TEST_ENTRIES; i++) {
        if (vfs_inode_allocate(
                &disk, &sb, VFS_INODE_REGULAR,
                0644, 1000, 1000, &inodes[i]) < 0) {
            perror("vfs_inode_allocate");
            return fail("target inode allocation failed", &disk);
        }
    }

    printf("[2/7] Adding and looking up directory entries\n");

    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "alpha.txt", inodes[0]) < 0) {
        perror("vfs_dir_add");
        return fail("directory entry insertion failed", &disk);
    }

    uint64_t found_inode = 0;

    if (vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "alpha.txt", &found_inode) < 0 ||
        found_inode != inodes[0]) {
        return fail("directory lookup returned the wrong inode", &disk);
    }

    printf("[3/7] Validating duplicate names and invalid lookups\n");

    errno = 0;

    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "alpha.txt", inodes[1]) != -1 ||
        errno != EEXIST) {
        return fail("duplicate directory name was not rejected", &disk);
    }

    errno = 0;

    if (vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "missing.txt", &found_inode) != -1 ||
        errno != ENOENT) {
        return fail("missing directory entry was not reported", &disk);
    }

    errno = 0;

    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "", inodes[1]) != -1 ||
        errno != EINVAL) {
        return fail("empty filename was not rejected", &disk);
    }

    errno = 0;

    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "bad/name", inodes[1]) != -1 ||
        errno != EINVAL) {
        return fail("filename containing slash was not rejected", &disk);
    }

    char long_name[VFS_NAME_MAX + 2U];
    memset(long_name, 'x', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';

    errno = 0;

    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            long_name, inodes[1]) != -1 ||
        errno != ENAMETOOLONG) {
        return fail("overlong filename was not rejected", &disk);
    }

    printf("[4/7] Supporting inode zero and removing entries\n");

    /*
     * Inode zero is the root inode, so it must be a valid target.
     */
    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "root-link", sb.root_inode) < 0 ||
        vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "root-link", &found_inode) < 0 ||
        found_inode != sb.root_inode) {
        return fail("directory entry could not reference inode zero", &disk);
    }

    uint64_t removed_inode = 0;

    if (vfs_dir_remove(
            &disk, &sb, sb.root_inode,
            "alpha.txt", &removed_inode) < 0 ||
        removed_inode != inodes[0]) {
        perror("vfs_dir_remove");
        return fail("directory entry removal failed", &disk);
    }

    if (vfs_inode_is_allocated(&disk, &sb, inodes[0]) != 1) {
        return fail("removing entry incorrectly released target inode", &disk);
    }

    errno = 0;

    if (vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "alpha.txt", &found_inode) != -1 ||
        errno != ENOENT) {
        return fail("removed directory entry is still visible", &disk);
    }

    if (vfs_dir_remove(
            &disk, &sb, sb.root_inode,
            "root-link", NULL) < 0) {
        return fail("could not remove root-link entry", &disk);
    }

    printf("[5/7] Filling all 64 slots and reusing an empty slot\n");

    char name[32];

    for (size_t i = 0; i < 64U; i++) {
        (void)snprintf(name, sizeof(name), "file-%02zu", i);

        if (vfs_dir_add(
                &disk, &sb, sb.root_inode,
                name, inodes[i]) < 0) {
            perror("vfs_dir_add");
            return fail("failed to fill first directory block", &disk);
        }
    }

    struct vfs_inode root;

    if (vfs_inode_read(&disk, &sb, sb.root_inode, &root) < 0 ||
        root.blocks_count != 1) {
        return fail("directory grew before its first block was full", &disk);
    }

    if (vfs_dir_remove(
            &disk, &sb, sb.root_inode,
            "file-10", NULL) < 0 ||
        vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "replacement.txt", inodes[64]) < 0) {
        perror("remove/reinsert directory entry");
        return fail("directory slot reuse failed", &disk);
    }

    if (vfs_inode_read(&disk, &sb, sb.root_inode, &root) < 0 ||
        root.blocks_count != 1) {
        return fail("slot reuse unexpectedly allocated another block", &disk);
    }

    printf("[6/7] Growing directory to a second data block\n");

    if (vfs_dir_add(
            &disk, &sb, sb.root_inode,
            "overflow.txt", inodes[64]) < 0) {
        perror("vfs_dir_add growth");
        return fail("directory growth failed", &disk);
    }

    if (vfs_inode_read(&disk, &sb, sb.root_inode, &root) < 0 ||
        root.blocks_count != 2 ||
        root.direct_blocks[1] < sb.data_start ||
        root.direct_blocks[1] >= sb.total_blocks) {
        return fail("second directory block was not attached correctly", &disk);
    }

    if (vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "overflow.txt", &found_inode) < 0 ||
        found_inode != inodes[64]) {
        return fail("entry in second directory block was not found", &disk);
    }

    printf("[7/7] Verifying directory persistence after reopen\n");

    if (vfs_disk_flush(&disk) < 0 ||
        vfs_disk_close(&disk) < 0) {
        perror("flush/close");
        return fail("could not close test image", &disk);
    }

    disk.fd = -1;

    if (vfs_disk_open(&disk, TEST_IMAGE) < 0 ||
        vfs_read_superblock(&disk, &sb) < 0) {
        perror("disk reopen");
        return fail("could not reopen filesystem", &disk);
    }

    if (vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "replacement.txt", &found_inode) < 0 ||
        found_inode != inodes[64]) {
        return fail("first-block entry did not persist", &disk);
    }

    if (vfs_dir_lookup(
            &disk, &sb, sb.root_inode,
            "overflow.txt", &found_inode) < 0 ||
        found_inode != inodes[64]) {
        return fail("second-block entry did not persist", &disk);
    }

    if (vfs_disk_close(&disk) < 0) {
        perror("vfs_disk_close");
        return fail("final disk close failed", NULL);
    }

    if (unlink(TEST_IMAGE) < 0) {
        perror("unlink");
        return fail("test image cleanup failed", NULL);
    }

    printf("All directory tests passed.\n");
    return 0;
}
