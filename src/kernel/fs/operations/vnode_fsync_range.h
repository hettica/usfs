// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_FSYNC_RANGE_H
#define USFS_FS_OPERATIONS_VNODE_FSYNC_RANGE_H

int gn_fsync_range (
    struct vnode * file_vnode,
    const int32long64_t open_flags,
    const int32long64_t file_info,
    const offset_t offset,
    const offset_t length,
    struct ucred * credentials
)
{
    ignore_parameter file_info;

    if (file_vnode == NULL || file_vnode->v_gnode == NULL)
        return EINVAL;

    struct usfs_open_state * open_state = NULL;
    int rc = usfs_borrow_node_handle (_node_of (file_vnode), open_flags, &open_state);
    if (rc != 0)
        return rc;

    rc = _sync_node (file_vnode, open_state->fh, open_flags, 1, offset, length, credentials);
    usfs_put_open_reference (file_vnode, open_state, 1, credentials);

    return rc;
}

#endif
