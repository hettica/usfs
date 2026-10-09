// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPEN_REFERENCE_H
#define USFS_FS_OPEN_REFERENCE_H

/* Caller owns a vnode reference until after any terminal backend RELEASE.
 * State selection and reference changes use global_lock; RPCs never do. */
static void usfs_put_open_reference (
    struct vnode * file_vnode,
    struct usfs_open_state * open_state,
    const int is_operation_reference,
    struct ucred * credentials
)
{
    struct usfs_open_state * released_handle = NULL;
    struct usfs_node * node = _node_of (file_vnode);

    if (open_state == NULL)
        return;

    if (usfs_release_open_reference (node, open_state, is_operation_reference, &released_handle) != 0)
        return;

    if (released_handle != NULL)
    {
        _send_release (
            _mount_of (file_vnode),
            node->nodeid,
            released_handle->fh,
            released_handle->flags,
            file_vnode->v_gnode->gn_type == VDIR,
            credentials
        );
        usfs_open_state_discard (released_handle);
    }

    VNOP_RELE (file_vnode);
}

#endif
