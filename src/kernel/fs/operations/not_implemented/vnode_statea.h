// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_STATEA_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_STATEA_H

int gn_statea (struct vnode * file_vnode, const char * attribute_name, struct vattr * attribute_status, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter attribute_name;
    ignore_parameter attribute_status;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
