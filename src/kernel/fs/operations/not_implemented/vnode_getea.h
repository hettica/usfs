// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_GETEA_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_GETEA_H

int gn_getea (struct vnode * file_vnode, const char * attribute_name, struct uio * user_io_request, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter attribute_name;
    ignore_parameter user_io_request;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
