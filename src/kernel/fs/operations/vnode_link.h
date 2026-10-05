// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_LINK_H
#define USFS_FS_OPERATIONS_VNODE_LINK_H

int gn_link (struct vnode * existing_vnode, struct vnode * directory_vnode, char * entry_name, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (directory_vnode);
    if (mount_data == NULL)
        return EIO;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&mount_data->namespace_lock);

    const int access_rc = gn_access (directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    struct usfs_node * directory_node = _node_of (directory_vnode);
    if (directory_node == NULL)
        return EIO;

    struct usfs_link_in request_body = { 0 };

    request_body.newparent = directory_node->nodeid;

    return _mutate_entry (existing_vnode, (uint16_t)USFS_OP_LINK, &request_body, sizeof (request_body), entry_name, NULL, 0, credentials);
}

#endif
