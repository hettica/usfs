// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_REMOVEEA_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_REMOVEEA_H

int gn_removeea (struct vnode * file_vnode, const char * attribute_name, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter attribute_name;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
