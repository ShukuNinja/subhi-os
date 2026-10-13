#define _POSIX_C_SOURCE 200809L
#include "vfs_check.h"
#include "vfs_format.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int main(void)
{
    char path[] = "/tmp/subhi-check-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(unlink(path) == 0);

    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    assert(vfs_disk_create(&disk, path, 1024) == 0);
    assert(vfs_format_disk(&disk) == 0);
    FILE *report = tmpfile();
    assert(report != NULL);
    assert(vfs_check_filesystem(&disk, report) == 0);

    struct vfs_superblock sb;
    assert(vfs_read_superblock(&disk, &sb) == 0);
    unsigned char before[VFS_BLOCK_SIZE];
    unsigned char after[VFS_BLOCK_SIZE];
    assert(vfs_disk_read_block(&disk, 0, before) == 0);
    sb.free_blocks--;
    assert(vfs_write_superblock(&disk, &sb) == 0);
    assert(vfs_check_filesystem(&disk, report) == 1);
    assert(vfs_disk_read_block(&disk, 0, after) == 0);
    /* The checker itself must not mutate the already-corrupted image. */
    unsigned char corrupted[VFS_BLOCK_SIZE];
    memcpy(corrupted, after, sizeof(corrupted));
    assert(vfs_check_filesystem(&disk, report) == 1);
    assert(vfs_disk_read_block(&disk, 0, after) == 0);
    assert(memcmp(corrupted, after, sizeof(after)) == 0);

    fclose(report);
    assert(vfs_disk_close(&disk) == 0);
    assert(unlink(path) == 0);
    puts("Filesystem checker tests passed.");
    return 0;
}
