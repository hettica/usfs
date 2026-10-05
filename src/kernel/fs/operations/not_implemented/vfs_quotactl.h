// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_QUOTACTL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_QUOTACTL_H

int usfs_quotactl (struct vfs * file_system, int command, uid_t quota_user_id, caddr_t quota_data, struct ucred * credentials)
{
    ignore_parameter file_system;
    ignore_parameter command;
    ignore_parameter quota_user_id;
    ignore_parameter quota_data;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
