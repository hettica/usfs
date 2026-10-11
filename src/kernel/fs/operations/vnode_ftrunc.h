// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_FTRUNC_H
#define USFS_FS_OPERATIONS_VNODE_FTRUNC_H

int gn_ftrunc (struct vnode * file_vnode, const int32long64_t open_flags, const offset_t length, caddr_t file_info, struct ucred * credentials)
{
    ignore_parameter open_flags;
    ignore_parameter file_info;

    if (length < 0)
        return EINVAL;

    struct usfs_setattr_in request_body = { 0 };

    request_body.valid = USFS_SET_SIZE;
    request_body.size = (uint64_t)length;

    return _setattr (file_vnode, &request_body, 0, credentials);
}

#endif
