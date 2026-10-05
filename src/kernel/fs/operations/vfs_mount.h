// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_MOUNT_H
#define USFS_FS_OPERATIONS_VFS_MOUNT_H

struct usfs_mount_options
{
    uint64_t channel;
    uint64_t cookie;
    int writable;
};

static int trace_mount_result (struct vfs * file_system, int channel, int rc)
{
    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_MOUNT, channel, file_system == NULL ? 0 : file_system->vfs_number, rc, g_mount_count);
    USFS_TRACE_CONTROL5 (USFS_TRACE_MOUNT, USFS_TRACE_ACTION_PUBLISH, channel, USFS_MOUNT_ACTIVE, g_mount_count, rc);
    return rc;
}

static int parse_mount_options (const struct vmount * virtual_mount, struct usfs_mount_options * options)
{
    const char * info = NULL;
    uint32_t info_length = 0;
    uint64_t writable = 0;

    if (usfs_vmt_text_field (virtual_mount, VMT_INFO, &info, &info_length) != 0)
        return EINVAL;

    /* Every peer supplies an explicit access policy. */
    int field_rc = usfs_info_field_u64 (info, info_length, "rw", 2u, 10u, &writable);
    if (field_rc != 1 || writable > 1u)
        return EINVAL;

    if (usfs_info_field_u64 (info, info_length, "chan", 4u, 10u, &options->channel) != 1 ||
        usfs_info_field_u64 (info, info_length, "cookie", 6u, 16u, &options->cookie) != 1)
    {
        return EINVAL;
    }

    options->writable = (writable != 0) && !(virtual_mount->vmt_flags & MNT_READONLY);
    return 0;
}

static void release_mount_reservation (struct usfs_connection * connection)
{
    usfs_lifecycle_release_mount (connection);
    release_connection (connection);
}

static void free_mount_data (struct usfs_mount_data * mount_data)
{
    struct usfs_connection * connection = mount_data->conn;

    lock_free (&mount_data->namespace_lock);
    xmfree (mount_data, kernel_heap);
    release_mount_reservation (connection);
}

static int mount_reserved_schedule_point (void)
{
    return usfs_checkpoint (USFS_INSTRUMENT_MOUNT_RESERVED);
}

static struct usfs_mount_data * create_mount_data (struct usfs_connection * connection, const struct usfs_mount_options * options)
{
    static const int align = 4;
    struct usfs_mount_data * mount_data = xmalloc (sizeof (*mount_data), align, kernel_heap);

    if (mount_data == NULL)
        return NULL;

    memset (mount_data, 0, sizeof (*mount_data));
    lock_alloc (&mount_data->namespace_lock, LOCK_ALLOC_PAGED, SHRT_MAX - 10, connection->channel);
    lock_init (&mount_data->namespace_lock, TRUE);
    mount_data->conn = connection;
    mount_data->state = USFS_MOUNT_ACTIVE;
    mount_data->writable = options->writable;

    return mount_data;
}

static int publish_mount_data (struct vfs * file_system, struct usfs_mount_data * mount_data)
{
    if (!mount_data->writable)
        file_system->vfs_flag |= VFS_READONLY;

    write_synchronized_with (global_lock)
    {
        file_system->vfs_data = (caddr_t)mount_data;
        mount_data->conn->mounted_data = mount_data;
    }

    return 0;
}

static int validate_mount_request (struct vfs * file_system, struct ucred * credentials, struct usfs_mount_options * options)
{
    if (file_system == NULL)
        return EINVAL;

    if (credentials == NULL || privcheck_cr (PV_FS_MOUNT, credentials) != 0)
        return EPERM;

    if (file_system->vfs_mdata == NULL)
        return EINVAL;

    return parse_mount_options (file_system->vfs_mdata, options);
}

int usfs_mount (struct vfs * file_system, struct ucred * credentials)
{
    struct usfs_mount_options options;
    struct usfs_connection * connection = NULL;

    memset (&options, 0, sizeof (options));
    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_MOUNT, -1, file_system == NULL ? 0 : file_system->vfs_number, 0, 0);

    int rc = validate_mount_request (file_system, credentials, &options);
    if (rc != 0)
        return trace_mount_result (file_system, -1, rc);

    // Atomically validate the connection, take its mount reference, and
    // reserve the mount count while the lifecycle gate is held. CFG_TERM
    // therefore cannot race between connection validation and accounting.
    rc = usfs_lifecycle_reserve_mount (options.channel, options.cookie, &connection);
    USFS_TRACE_CONTROL5 (USFS_TRACE_MOUNT, USFS_TRACE_ACTION_RESERVE, options.channel, USFS_MOUNT_ACTIVE, g_mount_count, rc);
    if (rc != 0)
        return trace_mount_result (file_system, (int)options.channel, rc);

    rc = mount_reserved_schedule_point ();
    if (rc != 0)
    {
        release_mount_reservation (connection);
        return trace_mount_result (file_system, (int)options.channel, rc);
    }

    struct usfs_mount_data * mount_data = create_mount_data (connection, &options);
    if (mount_data == NULL)
    {
        release_mount_reservation (connection);
        return trace_mount_result (file_system, (int)options.channel, ENOMEM);
    }

    // The gfs descriptor's gfs_hold field is deliberately not touched: the
    // kernel maintains it itself (it already reads as 1 for this mount before
    // the file system sees it), so adjusting it here would double-count and
    // leave the file system type permanently unloadable.
    // The root node is not built here. It is created on demand by vfs_root
    // and released again once nothing references it, exactly like any other
    // node of this file system: every live v-node holds the vfs, and a vfs
    // use count above 1 makes the logical file system refuse to unmount. A
    // root kept alive for the lifetime of the mount would therefore pin the
    // file system permanently.
    rc = publish_mount_data (file_system, mount_data);
    if (rc != 0)
    {
        free_mount_data (mount_data);
        return trace_mount_result (file_system, (int)options.channel, rc);
    }

    return trace_mount_result (file_system, (int)options.channel, 0);
}

#endif
