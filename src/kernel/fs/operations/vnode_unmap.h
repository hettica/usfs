// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_UNMAP_H
#define USFS_FS_OPERATIONS_VNODE_UNMAP_H

int gn_unmap (struct vnode * file_vnode, const int32long64_t mapping_flags, struct ucred * credentials)
{
    if (file_vnode == NULL || file_vnode->v_gnode == NULL || credentials == NULL)
        return EINVAL;

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    const struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL || node == NULL)
        return EIO;

    const int flush_rc = flush_cached_pages (file_vnode, 0, 0, 0, 0);
    if (flush_rc != 0)
        abort_connection (mount_data->conn);

    const int release_rc = usfs_release_mapping_state (file_vnode, mapping_flags);
    if (release_rc != 0)
        return release_rc;

    const int vnode_release_rc = VNOP_RELE (file_vnode);

    return flush_rc != 0 ? flush_rc : vnode_release_rc;
}

#endif
