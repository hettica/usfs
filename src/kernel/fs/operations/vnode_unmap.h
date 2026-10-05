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

    struct usfs_open_state * mapping_state_to_release = NULL;
    const int release_rc = usfs_release_mapping_state (file_vnode, mapping_flags, &mapping_state_to_release);
    if (release_rc != 0)
        return release_rc;

    if (mapping_state_to_release != NULL)
    {
        _send_release (mount_data, node->nodeid, mapping_state_to_release->fh, mapping_state_to_release->flags, 0, credentials);
        usfs_open_state_discard (mapping_state_to_release);
    }

    VNOP_RELE (file_vnode);

    return flush_rc;
}

#endif
