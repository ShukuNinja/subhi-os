#ifndef VFS_NAMESPACE_H
#define VFS_NAMESPACE_H

#include "vfs_dir.h"
#include "vfs_perm.h"
#include <stdio.h>

int vfs_mkdir(struct vfs_disk *disk, const struct vfs_superblock *sb,
              const char *path, uint32_t mode, uint32_t uid, uint32_t gid,
              const struct vfs_credentials *credentials);
int vfs_rmdir(struct vfs_disk *disk, const struct vfs_superblock *sb,
              const char *path, const struct vfs_credentials *credentials);
int vfs_list_directory(struct vfs_disk *disk, const struct vfs_superblock *sb,
                       const char *path, const struct vfs_credentials *credentials,
                       FILE *output);

#endif
