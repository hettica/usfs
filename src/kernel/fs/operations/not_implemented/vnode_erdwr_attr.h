// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_ERDWR_ATTR_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_ERDWR_ATTR_H

int gn_erdwr_attr (
    struct vnode * file_vnode,
    enum uio_rw operation,
    int32long64_t open_flags,
    struct uio * user_io_request,
    ext_t extension,
    caddr_t vnode_info,
    struct vattr * pre_operation_attributes,
    struct vattr * post_operation_attributes,
    struct ucred * credentials,
    struct file_secattr * security_attributes
)
{
    ignore_parameter file_vnode;
    ignore_parameter operation;
    ignore_parameter open_flags;
    ignore_parameter user_io_request;
    ignore_parameter extension;
    ignore_parameter vnode_info;
    ignore_parameter pre_operation_attributes;
    ignore_parameter post_operation_attributes;
    ignore_parameter credentials;
    ignore_parameter security_attributes;

    return ENOSYS;
}

#endif
