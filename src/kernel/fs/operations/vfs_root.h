// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_ROOT_H
#define USFS_FS_OPERATIONS_VFS_ROOT_H

static int acquire_root (struct vfs * file_system, struct usfs_mount_data * mount_data, struct vnode ** result_vnode)
{
    int rc = 0;

    *result_vnode = NULL;

    write_synchronized_with (global_lock)
    {
        if (mount_data->root != NULL)
        {
            /* Capture the cached root and acquire the caller's reference as
             * one atomic operation with respect to gn_rele.  Otherwise the
             * last release can reclaim mount_data->root after this check but before
             * a later VNOP_HOLD, leaving pathname lookup with a stale vnode. */
            usfs_vnode_hold_locked (mount_data->root->vn, USFS_INSTRUMENT_NODE_REUSE_NONE);
            *result_vnode = mount_data->root->vn;
            return 0;
        }

        struct usfs_node * root = NULL;
        rc = _create_node (file_system, mount_data, USFS_ROOT_ID, USFS_ROOT_ID, VDIR, 1, &root);
        if (rc != 0)
            return rc;

        mount_data->root = root;
        /* vn_get created this vnode with v_count == 1.  That initial
         * reference is transferred directly to this caller. */
        *result_vnode = root->vn;
    }

    return rc;
}

int usfs_root (struct vfs * file_system, struct vnode ** result_vnode, struct ucred * credentials)
{
    ignore_parameter credentials;

    struct usfs_mount_data * mount_data = (struct usfs_mount_data *)file_system->vfs_data;
    int channel = mount_data != NULL && mount_data->conn != NULL ? mount_data->conn->channel : -1;

    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_ROOT, channel, file_system->vfs_number, 0, 0);

    if (mount_data == NULL || mount_data->state != USFS_MOUNT_ACTIVE)
    {
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_ROOT, channel, file_system->vfs_number, EIO, 0);
        return EIO;
    }

    const int rc = acquire_root (file_system, mount_data, result_vnode);
    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_ROOT, channel, file_system->vfs_number, rc, rc == 0 ? USFS_ROOT_ID : 0);
    return rc;
}

#endif
