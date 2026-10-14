#define _POSIX_C_SOURCE 200809L

#include "vfs_disk.h"
#include "vfs_format.h"
#include "vfs_check.h"
#include "vfs_inode.h"
#include "vfs_alloc.h"
#include "vfs_file.h"
#include "vfs_namespace.h"
#include "vfs_dir.h"
#include "vfs_perm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SUBHI_VERSION "0.3.0"
#define VFS_MAX_FILE_SIZE ((size_t)VFS_DIRECT_BLOCKS * VFS_BLOCK_SIZE)

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
    printf("  %s mkdir <image> <absolute-path>\n", program);
    printf("  %s rmdir <image> <absolute-path>\n", program);
    printf("  %s ls <image> <absolute-directory-path>\n", program);
    printf("  %s create <image> <absolute-file-path>\n", program);
    printf("  %s write <image> <absolute-file-path> <text>\n", program);
    printf("  %s read <image> <absolute-file-path>\n", program);
    printf("  %s rm <image> <absolute-file-path>\n", program);
    printf("  %s stats <image>\n", program);
    printf("\nFile contents are limited to %zu bytes. Paths must be absolute.\n", VFS_MAX_FILE_SIZE);
    printf("This CLI uses virtual filesystem administrator credentials (UID/GID 0).\n");
}

static int parse_block_count(const char *text, uint64_t *blocks)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-' || blocks == NULL) {
        errno = EINVAL; return -1;
    }
    errno = 0; char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value == 0) {
        errno = EINVAL; return -1;
    }
    *blocks = (uint64_t)value; return 0;
}

static int command_format(const char *path, const char *block_text)
{
    uint64_t blocks;
    if (parse_block_count(block_text, &blocks) < 0) {
        fprintf(stderr, "Error: invalid block count '%s'\n", block_text); return 2;
    }
    struct vfs_disk disk = {.fd = -1, .total_blocks = 0};
    if (vfs_disk_create(&disk, path, blocks) < 0) { perror("Error creating disk image"); return 1; }
    if (vfs_format_disk(&disk) < 0) {
        int saved = errno; perror("Error formatting filesystem");
        (void)vfs_disk_close(&disk); (void)unlink(path); errno = saved; return 1;
    }
    if (vfs_disk_close(&disk) < 0) { perror("Error closing disk image"); return 1; }
    printf("Filesystem formatted successfully.\nImage: %s\nBlock size: %u bytes\nTotal blocks: %" PRIu64 "\n", path, VFS_BLOCK_SIZE, blocks);
    return 0;
}

static int open_fs(const char *path, struct vfs_disk *disk, struct vfs_superblock *sb)
{
    disk->fd = -1; disk->total_blocks = 0;
    if (vfs_disk_open(disk, path) < 0) { perror("Error opening disk image"); return -1; }
    if (vfs_read_superblock(disk, sb) < 0) {
        int saved = errno; perror("Error: invalid or unsupported Subhi-OS filesystem");
        (void)vfs_disk_close(disk); errno = saved; return -1;
    }
    return 0;
}

static int close_fs(struct vfs_disk *disk, int operation_result, int write_operation)
{
    int saved = errno;
    if (write_operation && vfs_disk_flush(disk) < 0 && operation_result == 0) {
        perror("Error flushing filesystem image"); operation_result = 1;
    }
    if (vfs_disk_close(disk) < 0 && operation_result == 0) {
        perror("Error closing disk image"); operation_result = 1;
    }
    errno = saved;
    return operation_result;
}

static int command_info(const char *path)
{
    struct vfs_disk disk; struct vfs_superblock sb;
    if (open_fs(path, &disk, &sb) < 0) return 1;
    printf("Subhi-OS filesystem information\nVersion: %" PRIu32 "\nBlock size: %" PRIu32 " bytes\nTotal blocks: %" PRIu64 "\nBlock bitmap start: %" PRIu64 "\nBlock bitmap blocks: %" PRIu64 "\nInode bitmap start: %" PRIu64 "\nInode bitmap blocks: %" PRIu64 "\nInode table start: %" PRIu64 "\nInode table blocks: %" PRIu64 "\nInode count: %" PRIu64 "\nData region starts: %" PRIu64 "\nFree data blocks: %" PRIu64 "\nRoot inode: %" PRIu64 "\n",
           sb.version, sb.block_size, sb.total_blocks, sb.bitmap_start, sb.bitmap_blocks,
           sb.inode_bitmap_start, sb.inode_bitmap_blocks, sb.inode_table_start,
           sb.inode_table_blocks, sb.inode_count, sb.data_start, sb.free_blocks, sb.root_inode);
    return close_fs(&disk, 0, 0);
}

static int command_check(const char *path)
{
    struct vfs_disk disk; struct vfs_superblock sb;
    if (open_fs(path, &disk, &sb) < 0) return 1;
    int result = vfs_check_filesystem(&disk, stdout);
    int saved = errno;
    if (result < 0) { errno = saved; perror("Error checking filesystem"); }
    return close_fs(&disk, result < 0 ? 1 : result, 0);
}

static struct vfs_credentials admin_credentials(void)
{
    const struct vfs_credentials credentials = {.uid = 0, .gid = 0};
    return credentials;
}

static int command_file(const char *verb, const char *image, const char *path, const char *text)
{
    struct vfs_disk disk; struct vfs_superblock sb;
    if (open_fs(image, &disk, &sb) < 0) return 1;
    struct vfs_credentials credentials = admin_credentials();
    int result = -1; int writing = 1;
    if (strcmp(verb, "create") == 0) {
        result = vfs_file_create(&disk, &sb, path, 0644, credentials.uid, credentials.gid, &credentials, NULL);
        if (result == 0) printf("Created file: %s\n", path);
    } else if (strcmp(verb, "write") == 0) {
        size_t size = strlen(text);
        result = vfs_file_write(&disk, &sb, path, text, size, &credentials);
        if (result == 0) printf("Wrote %zu bytes to %s\n", size, path);
    } else if (strcmp(verb, "read") == 0) {
        unsigned char buffer[VFS_MAX_FILE_SIZE]; size_t bytes_read = 0;
        writing = 0;
        result = vfs_file_read(&disk, &sb, path, buffer, sizeof(buffer), &bytes_read, &credentials);
        if (result == 0 && bytes_read > 0 && fwrite(buffer, 1, bytes_read, stdout) != bytes_read) { errno = EIO; result = -1; }
        if (result == 0 && (bytes_read == 0 || buffer[bytes_read - 1] != '\n')) putchar('\n');
    } else if (strcmp(verb, "rm") == 0) {
        result = vfs_file_unlink(&disk, &sb, path, &credentials);
        if (result == 0) printf("Deleted file: %s\n", path);
    }
    int saved = errno;
    if (result < 0) { errno = saved; perror("Subhi-OS file operation failed"); }
    int status = close_fs(&disk, result == 0 ? 0 : 1, writing && result == 0);
    return status;
}

static int command_directory(const char *verb, const char *image, const char *path)
{
    struct vfs_disk disk; struct vfs_superblock sb;
    if (open_fs(image, &disk, &sb) < 0) return 1;
    struct vfs_credentials credentials = admin_credentials();
    int result;
    if (strcmp(verb, "mkdir") == 0) {
        result = vfs_mkdir(&disk, &sb, path, 0755, credentials.uid, credentials.gid, &credentials);
        if (result == 0) printf("Created directory: %s\n", path);
    } else if (strcmp(verb, "rmdir") == 0) {
        result = vfs_rmdir(&disk, &sb, path, &credentials);
        if (result == 0) printf("Removed directory: %s\n", path);
    } else {
        result = vfs_list_directory(&disk, &sb, path, &credentials, stdout);
    }
    int saved = errno;
    if (result < 0) { errno = saved; perror("Subhi-OS directory operation failed"); }
    return close_fs(&disk, result == 0 ? 0 : 1, strcmp(verb, "ls") != 0 && result == 0);
}

struct stats_dir_entries {
    struct vfs_dir_entry entries[VFS_DIRECT_BLOCKS * (VFS_BLOCK_SIZE / sizeof(struct vfs_dir_entry))];
    size_t count;
};

static int collect_stats_entry(const struct vfs_dir_entry *entry, void *context)
{
    struct stats_dir_entries *entries = context;
    if (entries->count >= sizeof(entries->entries) / sizeof(entries->entries[0])) {
        errno = EOVERFLOW;
        return -1;
    }
    entries->entries[entries->count++] = *entry;
    return 0;
}

static int command_stats(const char *image)
{
    struct vfs_disk disk; struct vfs_superblock sb;
    if (open_fs(image, &disk, &sb) < 0) return 1;
    vfs_metrics_reset();
    struct timespec start, end, dir_start, dir_end;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t allocated_blocks = 0, allocated_inodes = 0;
    uint64_t allocated_inode_blocks = 0, minimum_required_blocks = 0;
    for (uint64_t b = sb.data_start; b < sb.total_blocks; b++) {
        int allocated = vfs_block_is_allocated(&disk, b);
        if (allocated < 0) { perror("Error scanning block bitmap"); return close_fs(&disk, 1, 0); }
        allocated_blocks += (uint64_t)allocated;
    }
    for (uint64_t i = 0; i < sb.inode_count; i++) {
        int allocated = vfs_inode_is_allocated(&disk, &sb, i);
        if (allocated < 0) { perror("Error scanning inode bitmap"); return close_fs(&disk, 1, 0); }
        if (!allocated) continue;
        allocated_inodes++;
        struct vfs_inode inode;
        if (vfs_inode_read(&disk, &sb, i, &inode) < 0) {
            perror("Error reading inode during statistics scan"); return close_fs(&disk, 1, 0);
        }
        allocated_inode_blocks += inode.blocks_count;
        uint64_t required = 0;
        if (inode.type == VFS_INODE_REGULAR) {
            required = inode.size / VFS_BLOCK_SIZE + (inode.size % VFS_BLOCK_SIZE != 0U);
        } else if (inode.type == VFS_INODE_DIRECTORY) {
            required = inode.size / VFS_BLOCK_SIZE + (inode.size % VFS_BLOCK_SIZE != 0U);
            if (required == 0) required = 1; /* directories reserve a block even when empty */
        }
        minimum_required_blocks += required;
    }

    struct stats_dir_entries root_entries = {0};
    if (vfs_dir_iterate(&disk, &sb, sb.root_inode, collect_stats_entry, &root_entries) < 0) {
        perror("Error scanning root directory for access metrics"); return close_fs(&disk, 1, 0);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &dir_start);
    uint64_t successful_lookups = 0;
    for (size_t i = 0; i < root_entries.count; i++) {
        uint64_t found = UINT64_MAX;
        if (vfs_dir_lookup(&disk, &sb, sb.root_inode, root_entries.entries[i].name, &found) < 0 ||
            found != root_entries.entries[i].inode_number) {
            fprintf(stderr, "Error: root directory index lookup mismatch\n");
            return close_fs(&disk, 1, 0);
        }
        successful_lookups++;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &dir_end);
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
    double dir_elapsed = (double)(dir_end.tv_sec - dir_start.tv_sec) + (double)(dir_end.tv_nsec - dir_start.tv_nsec) / 1000000000.0;
    if (elapsed <= 0.0) elapsed = 0.000000001;
    if (dir_elapsed < 0.0) dir_elapsed = 0.0;
    struct vfs_io_metrics metrics; vfs_metrics_get(&metrics);
    uint64_t data_blocks = sb.total_blocks - sb.data_start;
    uint64_t io_bytes = (metrics.physical_reads + metrics.physical_writes) * (uint64_t)VFS_BLOCK_SIZE;
    double allocation_efficiency = allocated_inode_blocks == 0 ? 100.0 :
        100.0 * (double)minimum_required_blocks / (double)allocated_inode_blocks;
    if (allocation_efficiency > 100.0) allocation_efficiency = 100.0;
    printf("Subhi-OS storage statistics (measured for this invocation)\n");
    printf("Data blocks:           %" PRIu64 "\n", data_blocks);
    printf("Allocated data blocks: %" PRIu64 " (%.2f%%)\n", allocated_blocks, data_blocks ? 100.0 * (double)allocated_blocks / (double)data_blocks : 0.0);
    printf("Free data blocks:      %" PRIu64 "\n", data_blocks - allocated_blocks);
    printf("Allocated inodes:      %" PRIu64 " / %" PRIu64 "\n", allocated_inodes, sb.inode_count);
    printf("Allocation efficiency: %.2f%% (minimum blocks implied by inode sizes / inode-owned blocks)\n", allocation_efficiency);
    printf("Block requests:        reads=%" PRIu64 ", writes=%" PRIu64 "\n", metrics.read_requests, metrics.write_requests);
    printf("Physical block I/O:    reads=%" PRIu64 ", writes=%" PRIu64 "\n", metrics.physical_reads, metrics.physical_writes);
    printf("Block cache:           hits=%" PRIu64 ", misses=%" PRIu64 "\n", metrics.cache_hits, metrics.cache_misses);
    printf("Scan elapsed:          %.6f seconds\n", elapsed);
    printf("Effective I/O rate:    %.2f KiB/s (physical block bytes / scan time)\n", ((double)io_bytes / 1024.0) / elapsed);
    printf("Root directory index:  %zu verified lookups in %.6f seconds (%.2f lookups/s)\n",
           (size_t)successful_lookups, dir_elapsed,
           dir_elapsed > 0.0 ? (double)successful_lookups / dir_elapsed : 0.0);
    return close_fs(&disk, 0, 0);
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) { print_usage(argv[0]); return 0; }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { printf("Subhi-OS v%s\n", SUBHI_VERSION); return 0; }
    if (argc == 4 && strcmp(argv[1], "format") == 0) return command_format(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "info") == 0) return command_info(argv[2]);
    if (argc == 3 && strcmp(argv[1], "check") == 0) return command_check(argv[2]);
    if (argc == 4 && (strcmp(argv[1], "mkdir") == 0 || strcmp(argv[1], "rmdir") == 0 || strcmp(argv[1], "ls") == 0)) return command_directory(argv[1], argv[2], argv[3]);
    if (argc == 4 && (strcmp(argv[1], "create") == 0 || strcmp(argv[1], "read") == 0 || strcmp(argv[1], "rm") == 0)) return command_file(argv[1], argv[2], argv[3], NULL);
    if (argc == 5 && strcmp(argv[1], "write") == 0) return command_file(argv[1], argv[2], argv[3], argv[4]);
    if (argc == 3 && strcmp(argv[1], "stats") == 0) return command_stats(argv[2]);
    print_usage(argv[0]); return 2;
}
