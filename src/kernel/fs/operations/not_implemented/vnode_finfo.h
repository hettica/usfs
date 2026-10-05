// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_FINFO_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_FINFO_H

int gn_finfo (struct vnode * file_vnode, int32long64_t command, void * information_buffer, size_t buffer_length, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter command;
    ignore_parameter information_buffer;
    ignore_parameter buffer_length;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
