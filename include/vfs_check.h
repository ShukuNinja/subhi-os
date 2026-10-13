#ifndef VFS_CHECK_H
#define VFS_CHECK_H

#include "vfs_disk.h"
#include <stdio.h>

/* Returns 0 if consistent, 1 if inconsistencies are found, -1 on I/O/setup error. */
int vfs_check_filesystem(struct vfs_disk *disk, FILE *report);

#endif
