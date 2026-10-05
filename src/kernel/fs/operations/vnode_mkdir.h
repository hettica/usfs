// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_MKDIR_H
#define USFS_FS_OPERATIONS_VNODE_MKDIR_H

int gn_mkdir (struct vnode * directory_vnode, char * entry_name, const int32long64_t mode, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (directory_vnode);
    if (mount_data == NULL)
        return EIO;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&mount_data->namespace_lock);

    const int access_rc = gn_access (directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    struct usfs_mkdir_in request_body = { 0 };

    request_body.mode = (uint32_t)mode;

    // The logical file system looks up the new directory if it needs a vnode.
    return _mutate_entry (directory_vnode, (uint16_t)USFS_OP_MKDIR, &request_body, sizeof (request_body), entry_name, NULL, 0, credentials);
}

#endif
