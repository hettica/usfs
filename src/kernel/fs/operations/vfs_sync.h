// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_SYNC_H
#define USFS_FS_OPERATIONS_VFS_SYNC_H

int usfs_sync (struct gfs * global_file_system)
{
    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_SYNC, -1, 0, 0, 0);

    if (global_file_system == NULL)
    {
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNC, -1, 0, EINVAL, 0);

        return EINVAL;
    }

    const int rc = _sync_all_mounts (USFS_SYNCFS_TRY, NULL);

    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_SYNC, -1, 0, rc, 0);

    return rc;
}

#endif
