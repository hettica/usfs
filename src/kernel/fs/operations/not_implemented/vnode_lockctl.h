// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_LOCKCTL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_LOCKCTL_H

int gn_lockctl (
    struct vnode * file_vnode,
    offset_t file_offset,
    struct eflock * lock_data,
    int32long64_t command,
    int (*retry_function) (),
    ulong * retry_id,
    struct ucred * credentials
)
{
    ignore_parameter file_vnode;
    ignore_parameter file_offset;
    ignore_parameter lock_data;
    ignore_parameter command;
    ignore_parameter retry_function;
    ignore_parameter retry_id;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
