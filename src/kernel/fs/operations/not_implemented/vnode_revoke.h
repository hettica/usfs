// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_REVOKE_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_REVOKE_H

int gn_revoke (struct vnode * file_vnode, int32long64_t open_state, int32long64_t open_flags, struct vattr * vnode_info, struct ucred * credentials)
{
    ignore_parameter file_vnode;
    ignore_parameter open_state;
    ignore_parameter open_flags;
    ignore_parameter vnode_info;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
