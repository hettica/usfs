// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SETACL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SETACL_H

int gn_setacl (struct vnode * file_vnode, struct uio * user_io_request, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter user_io_request;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
