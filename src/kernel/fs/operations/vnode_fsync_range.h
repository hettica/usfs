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

    return _sync_node (file_vnode, 0, open_flags, 1, offset, length, credentials);
}

#endif
