#include "vfs_namespace.h"
#include "vfs_alloc.h"
#include "vfs_inode.h"
#include "vfs_path.h"
#include "vfs_perm.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int split_path(const char *path, char **parent, char **name)
{
    if (!path || path[0] != '/' || !parent || !name) { errno = EINVAL; return -1; }
    size_t length = strlen(path);
    if (length <= 1 || path[length - 1] == '/') { errno = EINVAL; return -1; }
    const char *slash = strrchr(path, '/');
    const char *base = slash + 1;
    size_t n = strlen(base);
    if (n == 0 || n > VFS_NAME_MAX || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        errno = n > VFS_NAME_MAX ? ENAMETOOLONG : EINVAL; return -1;
    }
    size_t plen = slash == path ? 1U : (size_t)(slash - path);
    *parent = malloc(plen + 1U); *name = malloc(n + 1U);
    if (!*parent || !*name) { free(*parent); free(*name); *parent = NULL; *name = NULL; errno = ENOMEM; return -1; }
    if (slash == path) { (*parent)[0] = '/'; (*parent)[1] = '\0'; }
    else { memcpy(*parent, path, plen); (*parent)[plen] = '\0'; }
    memcpy(*name, base, n + 1U); return 0;
}

int vfs_mkdir(struct vfs_disk *disk, const struct vfs_superblock *sb,
              const char *path, uint32_t mode, uint32_t uid, uint32_t gid,
              const struct vfs_credentials *credentials)
{
    if (!disk || !sb || !credentials) { errno = EINVAL; return -1; }
    char *parent_path = NULL, *name = NULL;
    if (split_path(path, &parent_path, &name) < 0) return -1;
    uint64_t parent_number, child_number = 0, block_number = 0;
    int allocated_inode = 0, allocated_block = 0, entry_added = 0;
    int result = -1, saved_errno = 0;
    struct vfs_inode parent_inode, child_inode;
    if (vfs_path_resolve_as(disk, sb, parent_path, credentials, &parent_number) < 0) goto done;
    if (vfs_inode_read(disk, sb, parent_number, &parent_inode) < 0) goto done;
    if (parent_inode.type != VFS_INODE_DIRECTORY) { errno = ENOTDIR; goto done; }
    if (vfs_check_access(&parent_inode, credentials, VFS_ACCESS_WRITE | VFS_ACCESS_EXECUTE) < 0) goto done;
    uint64_t existing;
    if (vfs_dir_lookup(disk, sb, parent_number, name, &existing) == 0) { errno = EEXIST; goto done; }
    if (errno != ENOENT) goto done;
    if (vfs_inode_allocate(disk, sb, VFS_INODE_DIRECTORY, mode, uid, gid, &child_number) < 0) goto done;
    allocated_inode = 1;
    if (vfs_block_allocate(disk, &block_number) < 0) goto done;
    allocated_block = 1;
    unsigned char zero[VFS_BLOCK_SIZE] = {0};
    if (vfs_disk_write_block(disk, block_number, zero) < 0) goto done;
    if (vfs_inode_read(disk, sb, child_number, &child_inode) < 0) goto done;
    child_inode.blocks_count = 1;
    child_inode.direct_blocks[0] = block_number;
    child_inode.size = 0;
    if (vfs_inode_write(disk, sb, child_number, &child_inode) < 0) goto done;
    if (vfs_dir_add(disk, sb, parent_number, name, child_number) < 0) goto done;
    entry_added = 1;
    if (vfs_disk_flush(disk) < 0) goto done;
    result = 0;
done:
    saved_errno = errno;
    if (result < 0 && allocated_inode && !entry_added) {
        /* Remove child block pointer before releasing the block/inode. */
        if (vfs_inode_read(disk, sb, child_number, &child_inode) == 0) {
            child_inode.size = 0; child_inode.blocks_count = 0;
            memset(child_inode.direct_blocks, 0, sizeof(child_inode.direct_blocks));
            if (vfs_inode_write(disk, sb, child_number, &child_inode) == 0) {
                if (allocated_block) (void)vfs_block_release(disk, block_number);
                (void)vfs_inode_release(disk, sb, child_number);
            }
        }
    }
    free(parent_path); free(name); errno = saved_errno; return result;
}

struct list_context { struct vfs_disk *disk; const struct vfs_superblock *sb; FILE *output; };
static int print_entry(const struct vfs_dir_entry *entry, void *opaque)
{
    struct list_context *ctx = opaque;
    struct vfs_inode inode;
    if (vfs_inode_read(ctx->disk, ctx->sb, entry->inode_number, &inode) < 0) return -1;
    char type = inode.type == VFS_INODE_DIRECTORY ? 'd' : inode.type == VFS_INODE_REGULAR ? '-' : '?';
    if (fprintf(ctx->output, "%c %04o %8" PRIu64 " %s\n", type,
                inode.mode & 07777U, inode.size, entry->name) < 0) return -1;
    return 0;
}

int vfs_list_directory(struct vfs_disk *disk, const struct vfs_superblock *sb,
                       const char *path, const struct vfs_credentials *credentials,
                       FILE *output)
{
    if (!disk || !sb || !path || !credentials || !output) { errno = EINVAL; return -1; }
    uint64_t ino; struct vfs_inode inode;
    if (vfs_path_resolve_as(disk, sb, path, credentials, &ino) < 0) return -1;
    if (vfs_inode_read(disk, sb, ino, &inode) < 0) return -1;
    if (inode.type != VFS_INODE_DIRECTORY) { errno = ENOTDIR; return -1; }
    if (vfs_check_access(&inode, credentials, VFS_ACCESS_READ) < 0) return -1;
    struct list_context context = {disk, sb, output};
    return vfs_dir_iterate(disk, sb, ino, print_entry, &context);
}

struct empty_context { int found; };
static int find_any_entry(const struct vfs_dir_entry *entry, void *opaque)
{ (void)entry; ((struct empty_context *)opaque)->found = 1; return 1; }

int vfs_rmdir(struct vfs_disk *disk, const struct vfs_superblock *sb,
              const char *path, const struct vfs_credentials *credentials)
{
    if (!disk || !sb || !path || !credentials) { errno = EINVAL; return -1; }
    char *parent_path = NULL, *name = NULL;
    if (split_path(path, &parent_path, &name) < 0) return -1;
    int result = -1, saved_errno;
    uint64_t parent_ino, child_ino;
    struct vfs_inode parent, child;
    if (vfs_path_resolve_as(disk, sb, parent_path, credentials, &parent_ino) < 0) goto done;
    if (vfs_inode_read(disk, sb, parent_ino, &parent) < 0) goto done;
    if (parent.type != VFS_INODE_DIRECTORY) { errno = ENOTDIR; goto done; }
    if (vfs_check_access(&parent, credentials, VFS_ACCESS_WRITE | VFS_ACCESS_EXECUTE) < 0) goto done;
    if (vfs_dir_lookup(disk, sb, parent_ino, name, &child_ino) < 0) goto done;
    if (child_ino == sb->root_inode) { errno = EBUSY; goto done; }
    if (vfs_inode_read(disk, sb, child_ino, &child) < 0) goto done;
    if (child.type != VFS_INODE_DIRECTORY) { errno = ENOTDIR; goto done; }
    struct empty_context ec = {0};
    if (vfs_dir_iterate(disk, sb, child_ino, find_any_entry, &ec) < 0 && !ec.found) goto done;
    if (ec.found) { errno = ENOTEMPTY; goto done; }
    uint64_t blocks[VFS_DIRECT_BLOCKS]; uint32_t block_count = child.blocks_count;
    memcpy(blocks, child.direct_blocks, sizeof(blocks));
    if (vfs_dir_remove(disk, sb, parent_ino, name, NULL) < 0) goto done;
    child.size = 0; child.blocks_count = 0; memset(child.direct_blocks, 0, sizeof(child.direct_blocks));
    if (vfs_inode_write(disk, sb, child_ino, &child) < 0) goto done;
    for (uint32_t i = 0; i < block_count; i++) if (vfs_block_release(disk, blocks[i]) < 0) goto done;
    if (vfs_inode_release(disk, sb, child_ino) < 0) goto done;
    if (vfs_disk_flush(disk) < 0) goto done;
    result = 0;
done:
    saved_errno = errno; free(parent_path); free(name); errno = saved_errno; return result;
}
