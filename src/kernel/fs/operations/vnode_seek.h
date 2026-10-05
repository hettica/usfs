// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_SEEK_H
#define USFS_FS_OPERATIONS_VNODE_SEEK_H

int gn_seek (struct vnode * file_vnode, offset_t * offset, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter offset;
    ignore_parameter credentials;

    // Accept every offset so lseek can use the full 64-bit range.
    return 0;
}

#endif
