
#ifndef VFS_PERM_H
#define VFS_PERM_H

#include "vfs_inode.h"

#include <stdint.h>

struct vfs_credentials {
    uint32_t uid;
    uint32_t gid;
};

/* Access bits correspond to Unix read, write, and execute permissions. */
#define VFS_ACCESS_EXECUTE 1U
#define VFS_ACCESS_WRITE   2U
#define VFS_ACCESS_READ    4U

/*
 * Check requested access against inode owner/group/other mode bits.
 *
 * Returns 0 if access is granted, or -1 with errno set.
 * Returns EACCES when the requested access is denied.
 */
int vfs_check_access(
    const struct vfs_inode *inode,
    const struct vfs_credentials *credentials,
    uint32_t requested_access
);

#endif
