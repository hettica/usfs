// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_MKNOD_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_MKNOD_H

int gn_mknod (struct vnode * directory_vnode, caddr_t file_name, int32long64_t file_mode, dev_t device_number, struct ucred * credentials)
{
    ignore_parameter directory_vnode;
    ignore_parameter file_name;
    ignore_parameter file_mode;
    ignore_parameter device_number;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
