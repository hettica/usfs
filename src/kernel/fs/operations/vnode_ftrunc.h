// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_FTRUNC_H
#define USFS_FS_OPERATIONS_VNODE_FTRUNC_H

int gn_ftrunc (struct vnode * file_vnode, const int32long64_t open_flags, const offset_t length, caddr_t file_info, struct ucred * credentials)
{
    ignore_parameter open_flags;

    if (length < 0)
        return EINVAL;

    struct usfs_setattr_in request_body = { 0 };

    request_body.valid = USFS_SET_SIZE;
    request_body.size = (uint64_t)length;

    struct usfs_open_state * open_state = NULL;
    const int handle_rc = usfs_borrow_description (_node_of (file_vnode), file_info, &open_state);
    if (handle_rc != 0)
        return handle_rc;

    const int rc = _setattr (file_vnode, &request_body, open_state->fh, credentials);

    usfs_put_open_reference (file_vnode, open_state, 1, credentials);

    return rc;
}

#endif
