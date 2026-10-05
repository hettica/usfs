// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_SYNCVFS_H
#define USFS_FS_OPERATIONS_VFS_SYNCVFS_H

int usfs_syncvfs (struct gfs * global_file_system, struct vfs * file_system, int sync_flags, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data;
    struct usfs_syncfs_in request_body;
    int rc;
    int channel = -1;

    if (file_system != NULL && file_system->vfs_data != NULL)
    {
        mount_data = (struct usfs_mount_data *)file_system->vfs_data;
        if (mount_data->conn != NULL)
            channel = mount_data->conn->channel;
    }

    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_SYNCVFS, channel, file_system != NULL ? file_system->vfs_number : 0, sync_flags, 0);

    if (global_file_system == NULL)
    {
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNCVFS, channel, file_system != NULL ? file_system->vfs_number : 0, EINVAL, 0);
        return EINVAL;
    }

    const int synchronization_level = sync_flags & 0x03;
    memset (&request_body, 0, sizeof (request_body));

    if (synchronization_level == FS_SYNCVFS_TRY)
    {
        request_body.mode = USFS_SYNCFS_TRY;
    }
    else if (synchronization_level == FS_SYNCVFS_FORCE)
    {
        request_body.mode = USFS_SYNCFS_FORCE;
    }
    else if (synchronization_level == FS_SYNCVFS_QUIESCE)
    {
        request_body.mode = USFS_SYNCFS_QUIESCE;
    }
    else
    {
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNCVFS, channel, file_system != NULL ? file_system->vfs_number : 0, EINVAL, 0);
        return EINVAL;
    }

    if (file_system == NULL)
    {
        rc = _sync_all_mounts (request_body.mode, credentials);
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNCVFS, -1, 0, rc, request_body.mode);
        return rc;
    }

    mount_data = (struct usfs_mount_data *)file_system->vfs_data;
    
    if (mount_data == NULL)
    {
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNCVFS, channel, file_system->vfs_number, EIO, request_body.mode);
        return EIO;
    }

    rc = _sync_connection (mount_data->conn, request_body.mode, credentials);
    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNCVFS, channel, file_system->vfs_number, rc, request_body.mode);
    return rc;
}

#endif
