#ifndef VFS_DISK_H
#define VFS_DISK_H

#include <stdint.h>

#define VFS_BLOCK_SIZE 4096U

struct vfs_io_metrics {
    uint64_t read_requests;
    uint64_t write_requests;
    uint64_t physical_reads;
    uint64_t physical_writes;
    uint64_t cache_hits;
    uint64_t cache_misses;
};

struct vfs_disk {
    int fd;
    uint64_t total_blocks;
    int journal_enabled;
};

/* Process-local write-through block cache and I/O counters. */
void vfs_metrics_reset(void);
void vfs_metrics_get(struct vfs_io_metrics *metrics);

/* Create a new disk image. Refuses to overwrite an existing file. */
int vfs_disk_create(
    struct vfs_disk *disk,
    const char *path,
    uint64_t total_blocks
);

/* Open and validate an existing disk image. */
int vfs_disk_open(struct vfs_disk *disk, const char *path);

/* Read or write exactly one block. */
int vfs_disk_read_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    void *buffer
);

int vfs_disk_write_block(
    struct vfs_disk *disk,
    uint64_t block_number,
    const void *buffer
);

/* Enable write-ahead undo logging after a valid V3 filesystem is formatted/opened. */
int vfs_disk_enable_journal(struct vfs_disk *disk);

/* Flush pending file data to persistent storage. */
int vfs_disk_flush(struct vfs_disk *disk);

/* Close the disk image. */
int vfs_disk_close(struct vfs_disk *disk);

#endif
