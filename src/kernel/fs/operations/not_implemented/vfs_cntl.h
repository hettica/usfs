// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_CNTL_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VFS_CNTL_H

int usfs_cntl (struct vfs * file_system, int command, caddr_t command_argument, size_t command_argument_size, struct ucred * credentials)
{
    ignore_parameter file_system;
    ignore_parameter command;
    ignore_parameter command_argument;
    ignore_parameter command_argument_size;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
