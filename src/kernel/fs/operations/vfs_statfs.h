// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_STATFS_H
#define USFS_FS_OPERATIONS_VFS_STATFS_H

static void initialize_file_system_stats (const struct vfs * file_system, struct statfs * file_system_stats)
{
    memset (file_system_stats, 0, sizeof (*file_system_stats));
    file_system_stats->f_version = 0;
    file_system_stats->f_type = 0;
    file_system_stats->f_fsid = file_system->vfs_mdata->vmt_fsid;
    file_system_stats->f_vfstype = gfs.gfs_type;
    file_system_stats->f_vfsnumber = file_system->vfs_number;
    file_system_stats->f_vfsoff = sizeof (*file_system_stats);
    file_system_stats->f_vfslen = 0;
    file_system_stats->f_vfsvers = 0;

    strncpy (file_system_stats->f_fname, "usfs", sizeof (file_system_stats->f_fname) - 1);
    file_system_stats->f_fname[sizeof (file_system_stats->f_fname) - 1] = '\0';
    strncpy (file_system_stats->f_fpack, "usfs", sizeof (file_system_stats->f_fpack) - 1);
    file_system_stats->f_fpack[sizeof (file_system_stats->f_fpack) - 1] = '\0';
}

static void set_file_system_mount_name (const struct vmount * virtual_mount, struct statfs * file_system_stats)
{
    const char * stub = NULL;
    uint32_t stub_length = 0;

    if (usfs_vmt_text_field (virtual_mount, VMT_STUB, &stub, &stub_length) != 0 || stub_length == 0)
    {
        return;
    }

    uint32_t copy_length = stub_length - 1u;

    if (copy_length >= (uint32_t)sizeof (file_system_stats->f_fname))
    {
        copy_length = (uint32_t)sizeof (file_system_stats->f_fname) - 1u;
    }

    memcpy (file_system_stats->f_fname, stub, copy_length);
    file_system_stats->f_fname[copy_length] = '\0';
}

static int apply_file_system_stats_reply (const struct usfs_request * request, struct statfs * file_system_stats)
{
    struct usfs_statfs_out reply;

    if (request->error != 0)
        return request->error;

    if (request->reply_buffer_size < sizeof (reply))
    {
        return EIO;
    }

    memcpy (&reply, request->reply_buffer, sizeof (reply));

    if (!usfs_statfs_out_valid (&reply))
    {
        return EIO;
    }

    file_system_stats->f_bsize = (ulong)reply.bsize;
    file_system_stats->f_fsize = (ulong)reply.bsize;
    file_system_stats->f_blocks = (fsblkcnt_t)reply.blocks;
    file_system_stats->f_bfree = (fsblkcnt_t)reply.bfree;
    file_system_stats->f_bavail = (fsblkcnt_t)reply.bavail;
    file_system_stats->f_files = (fsfilcnt_t)reply.files;
    file_system_stats->f_ffree = (fsfilcnt_t)reply.ffree;
    file_system_stats->f_name_max = (long)reply.namemax;
    return 0;
}

static int refresh_file_system_stats (struct usfs_mount_data * mount_data, struct ucred * credentials, struct statfs * file_system_stats)
{
    struct usfs_request * request = NULL;

    if (mount_data == NULL || mount_data->conn == NULL)
    {
        return EIO;
    }

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = (uint16_t)USFS_OP_STATFS,
        .node_id = USFS_ROOT_ID,
        .max_allowed_reply_buffer_size = (uint32_t)sizeof (struct usfs_statfs_out),
        .credentials = credentials
    };

    int rc = allocate_request (&request_spec, &request);
    if (rc != 0)
    {
        return rc;
    }

    rc = usfs_call (mount_data->conn, request);
    if (rc != 0)
    {
        /* usfs_call disposes failed requests. */
        return rc;
    }

    rc = apply_file_system_stats_reply (request, file_system_stats);
    free_request (request);
    return rc;
}

int usfs_statfs (struct vfs * file_system, struct statfs * file_system_stats, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data;
    struct statfs result;
    int channel = -1;

    if (file_system != NULL && file_system->vfs_data != NULL)
    {
        mount_data = (struct usfs_mount_data *)file_system->vfs_data;
        if (mount_data->conn != NULL)
            channel = mount_data->conn->channel;
    }

    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_STATFS, channel, file_system != NULL ? file_system->vfs_number : 0, 0, 0);

    if (file_system == NULL || file_system_stats == NULL || file_system->vfs_mdata == NULL)
    {
        USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_STATFS, channel, file_system != NULL ? file_system->vfs_number : 0, EINVAL, 0);
        return EINVAL;
    }

    initialize_file_system_stats (file_system, &result);
    mount_data = (struct usfs_mount_data *)file_system->vfs_data;

    const int rc = refresh_file_system_stats (mount_data, credentials, &result);
    if (rc == 0)
    {
        set_file_system_mount_name (file_system->vfs_mdata, &result);
        *file_system_stats = result;
    }

    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_STATFS, channel, file_system->vfs_number, rc, rc == 0 ? file_system_stats->f_bsize : 0);
    return rc;
}

#endif
