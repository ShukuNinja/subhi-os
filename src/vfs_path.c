
#include "vfs_path.h"

#include "vfs_dir.h"
#include "vfs_inode.h"

#include <errno.h>
#include <string.h>

int vfs_path_resolve(
    struct vfs_disk *disk,
    const struct vfs_superblock *sb,
    const char *path,
    uint64_t *result_inode_number
)
{
    uint64_t current_inode_number;
    const char *component;
    const char *separator;
    size_t component_length;

    if (disk == NULL || sb == NULL || path == NULL ||
        result_inode_number == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    current_inode_number = sb->root_inode;
    component = path;

    while (*component != '\0') {
        while (*component == '/') {
            component++;
        }

        if (*component == '\0') {
            break;
        }

        separator = strchr(component, '/');

        if (separator != NULL) {
            component_length = (size_t)(separator - component);
        } else {
            component_length = strlen(component);
        }

        if (component_length > VFS_NAME_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }

        if (component_length == 0U) {
            errno = EINVAL;
            return -1;
        }

        {
            char name[VFS_NAME_MAX + 1U];
            struct vfs_inode inode;
            uint64_t next_inode_number;

            memcpy(name, component, component_length);
            name[component_length] = '\0';

            if (vfs_inode_read(
                    disk, sb, current_inode_number, &inode) == -1) {
                return -1;
            }

            if (inode.type != VFS_INODE_DIRECTORY) {
                errno = ENOTDIR;
                return -1;
            }

            if (vfs_dir_lookup(
                    disk, sb, current_inode_number,
                    name, &next_inode_number) == -1) {
                return -1;
            }

            current_inode_number = next_inode_number;
        }

        component += component_length;
    }

    *result_inode_number = current_inode_number;
    return 0;
}
