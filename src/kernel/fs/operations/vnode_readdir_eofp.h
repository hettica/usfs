// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_READDIR_EOFP_H
#define USFS_FS_OPERATIONS_VNODE_READDIR_EOFP_H

int gn_readdir_eofp (struct vnode * directory_vnode, struct uio * user_io_request, int * end_of_directory, struct ucred * credentials)
{
    if (end_of_directory == NULL)
        return EINVAL;

    return read_directory (directory_vnode, user_io_request, end_of_directory, credentials);
}

#endif
