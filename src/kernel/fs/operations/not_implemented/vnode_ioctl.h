// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_IOCTL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_IOCTL_H

int gn_ioctl (
    struct vnode * file_vnode,
    int32long64_t command,
    caddr_t command_argument,
    size_t open_flags,
    ext_t extension,
    struct ucred * credentials
)
{
    ignore_parameter file_vnode;
    ignore_parameter command;
    ignore_parameter command_argument;
    ignore_parameter open_flags;
    ignore_parameter extension;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
