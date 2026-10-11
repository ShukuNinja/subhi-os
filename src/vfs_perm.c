
#include "vfs_perm.h"

#include <errno.h>
#include <stdlib.h>

int vfs_check_access(
    const struct vfs_inode *inode,
    const struct vfs_credentials *credentials,
    uint32_t requested_access
)
{
    if (inode == NULL || credentials == NULL ||
        requested_access == 0U ||
        (requested_access & ~7U) != 0U) {
        errno = EINVAL;
        return -1;
    }

    uint32_t granted_bits;

    if (credentials->uid == inode->uid) {
        granted_bits = (inode->mode >> 6U) & 7U;
    } else if (credentials->gid == inode->gid) {
        granted_bits = (inode->mode >> 3U) & 7U;
    } else {
        granted_bits = inode->mode & 7U;
    }

    if ((granted_bits & requested_access) != requested_access) {
        errno = EACCES;
        return -1;
    }

    return 0;
}