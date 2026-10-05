// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_RDWR_ATTR_H
#define USFS_FS_OPERATIONS_VNODE_RDWR_ATTR_H

int gn_rdwr_attr (
    struct vnode * file_vnode,
    const enum uio_rw operation,
    const int32long64_t open_flags,
    struct uio * user_io_request,
    const ext_t extension,
    caddr_t file_info,
    struct vattr * pre_operation_attributes,
    struct vattr * post_operation_attributes,
    struct ucred * credentials
)
{
    if (pre_operation_attributes != NULL)
    {
        const int rc = gn_getattr (file_vnode, pre_operation_attributes, credentials);
        if (rc != 0)
            return rc;
    }

    return gn_rdwr (file_vnode, operation, open_flags, user_io_request, extension, file_info, post_operation_attributes, credentials);
}

#endif
