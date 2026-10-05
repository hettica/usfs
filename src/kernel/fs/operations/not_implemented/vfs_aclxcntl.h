// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_ACLXCNTL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_ACLXCNTL_H

int usfs_aclxcntl (
    struct vfs * file_system,
    struct vnode * file_vnode,
    int command,
    struct uio * user_io_request,
    size_t * user_data_size,
    struct ucred * credentials
)
{
    ignore_parameter file_system;
    ignore_parameter file_vnode;
    ignore_parameter command;
    ignore_parameter user_io_request;
    ignore_parameter user_data_size;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
