// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_CLOSE_H
#define USFS_FS_OPERATIONS_VNODE_CLOSE_H

int gn_close (struct vnode * file_vnode, const int32long64_t open_flags, caddr_t file_info, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);
    struct usfs_open_state * open_state = (struct usfs_open_state *)file_info;

    if (mount_data == NULL || node == NULL)
        return EIO;

    const int close_rc = usfs_close_open_state (node, file_info);
    if (close_rc != 0)
        return close_rc;

    const int cache_rc = file_vnode->v_gnode->gn_type == VDIR ? 0 : flush_cached_pages (file_vnode, 0, 0, 0, 0);
    const int flush_rc = file_vnode->v_gnode->gn_type == VDIR ? 0 : _flush_handle (mount_data, node->nodeid, open_state->fh, open_flags, credentials);

    usfs_put_open_reference (file_vnode, open_state, 0, credentials);

    return cache_rc != 0 ? cache_rc : flush_rc;
}

#endif
