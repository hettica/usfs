// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_STATFSVP_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_STATFSVP_H

int usfs_statfsvp (struct vfs * file_system, struct vnode * file_vnode, struct statfs * file_system_stats, struct ucred * credentials)
{
    ignore_parameter file_system;
    ignore_parameter file_vnode;
    ignore_parameter file_system_stats;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
