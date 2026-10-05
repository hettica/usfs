// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SETXACL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SETXACL_H

int gn_setxacl (
    struct vnode * file_vnode,
    uint64_t control_flags,
    acl_type_t acl_type,
    struct uio * user_io_request,
    mode_t mode_info,
    struct ucred * credentials
)
{
    ignore_parameter file_vnode;
    ignore_parameter control_flags;
    ignore_parameter acl_type;
    ignore_parameter user_io_request;
    ignore_parameter mode_info;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
