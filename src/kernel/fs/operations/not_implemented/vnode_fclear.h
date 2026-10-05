// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_FCLEAR_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_FCLEAR_H

int gn_fclear (
    struct vnode * file_vnode,
    int32long64_t open_flags,
    offset_t start_offset,
    offset_t clear_length,
    caddr_t vnode_info,
    struct ucred * credentials
)
{
    ignore_parameter file_vnode;
    ignore_parameter open_flags;
    ignore_parameter start_offset;
    ignore_parameter clear_length;
    ignore_parameter vnode_info;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
