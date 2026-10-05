// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_REMOVE_H
#define USFS_FS_OPERATIONS_VNODE_REMOVE_H

int gn_remove (struct vnode * removed_vnode, struct vnode * directory_vnode, char * entry_name, struct ucred * credentials)
{
    ignore_parameter removed_vnode;

    struct usfs_mount_data * mount_data = _mount_of (directory_vnode);
    if (mount_data == NULL)
        return EIO;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&mount_data->namespace_lock);

    const int access_rc = gn_access (directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    const int sticky_rc = usfs_check_sticky_entry (directory_vnode, entry_name, credentials, 0);
    if (sticky_rc != 0)
        return sticky_rc;

    // AIX releases both lookup holds after this callback. Releasing removed_vnode here
    // would free it before that final logical-file-system release.
    return _mutate_entry (directory_vnode, (uint16_t)USFS_OP_UNLINK, NULL, 0, entry_name, NULL, 0, credentials);
}

#endif
