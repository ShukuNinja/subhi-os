
#ifndef VFS_DIR_H
#define VFS_DIR_H

#include "vfs_inode.h"

#include <stdint.h>

#define VFS_NAME_MAX 55U

struct vfs_dir_entry {
    uint64_t inode_number;
    char name[VFS_NAME_MAX + 1U];
};

_Static_assert(
    sizeof(struct vfs_dir_entry) == 64U,
    "directory entry must be exactly 64 bytes"
);

/* Look up a name and return its inode number. */
int vfs_dir_lookup(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    const char *name,
    uint64_t *result_inode_number
);

/* Add a name-to-inode mapping to a directory. */
int vfs_dir_add(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    const char *name,
    uint64_t target_inode_number
);

/* Remove a directory entry without releasing its target inode. */
int vfs_dir_remove(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    uint64_t directory_inode_number,
    const char *name,
    uint64_t *removed_inode_number
);

#endif
