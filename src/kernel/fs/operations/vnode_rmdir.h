// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_RMDIR_H
#define USFS_FS_OPERATIONS_VNODE_RMDIR_H

int gn_rmdir (struct vnode * removed_vnode, struct vnode * directory_vnode, char * entry_name, struct ucred * credentials)
{
    if (directory_vnode == NULL || removed_vnode == NULL)
        return ESTALE;

    struct usfs_mount_data * mount_data = _mount_of (directory_vnode);
    struct usfs_node * target_node = _node_of (removed_vnode);
    if (mount_data == NULL || target_node == NULL || target_node->nodeid <= USFS_ROOT_ID)
        return ESTALE;

    if (_mount_of (removed_vnode) != mount_data)
        return EXDEV;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&mount_data->namespace_lock);

    const int access_rc = gn_access (directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    const int sticky_rc = usfs_check_sticky_entry (directory_vnode, entry_name, credentials, 0);
    if (sticky_rc != 0)
        return sticky_rc;

    const struct usfs_remove_in request_body = { .target = target_node->nodeid };

    return _mutate_entry (directory_vnode, &(const struct entry_mutation){
        .opcode = (uint16_t)USFS_OP_RMDIR,
        .body = &request_body,
        .body_length = sizeof (request_body),
        .name = entry_name,
        .data = NULL,
        .data_length = 0,
        .credentials = credentials
    });
}

#endif
