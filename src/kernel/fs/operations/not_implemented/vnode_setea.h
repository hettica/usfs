// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SETEA_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SETEA_H

int gn_setea (struct vnode * file_vnode, const char * attribute_name, struct uio * user_io_request, int flags, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter attribute_name;
    ignore_parameter user_io_request;
    ignore_parameter flags;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
