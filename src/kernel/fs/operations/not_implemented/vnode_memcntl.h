// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_MEMCNTL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_MEMCNTL_H

int gn_memcntl (struct vnode * file_vnode, int command, void * attachment_data, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter command;
    ignore_parameter attachment_data;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
