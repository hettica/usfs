// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_UNMOUNT_H
#define USFS_FS_OPERATIONS_VFS_UNMOUNT_H

static void release_detached_mount (struct usfs_mount_data * mount_data);

static void free_mount_nodes (struct usfs_mount_data * mount_data)
{
    struct usfs_node * node = mount_data->nodes;

    while (node != NULL)
    {
        struct usfs_node * next = node->next;

        if (node->gn->gn_seg != 0 && flush_cached_pages (node->vn, 0, 0, 0, 0) != 0)
            abort_connection (mount_data->conn);

        dispose_cache_resources (node, mount_data);
        (void)usfs_forget (mount_data->conn, node->nodeid, node->lookup_refs);

        write_synchronized_with (global_lock)
        {
            lock_free (&node->pageout_lock);
            lock_free (&node->mutation_lock);
            if (node->vn != NULL)
                _free_vnode (node->vn, node->vn_lock_occurrence);
            if (node->gn != NULL)
                _free_gnode (node->gn, node->gn_lock_occurrence);
            xmfree (node, kernel_heap);
        }

        node = next;
    }
}

static int detach_mount_data (struct vfs * file_system, struct usfs_mount_data ** reply)
{
    struct usfs_mount_data * mount_data = 0;

    *reply = NULL;

    write_synchronized_with (global_lock)
    {
        mount_data = (struct usfs_mount_data *)file_system->vfs_data;

        if (mount_data == NULL || mount_data->state != USFS_MOUNT_ACTIVE)
            return EIO;

        if (mount_data->reclaiming_nodes != 0 || mount_data->inflight_vgets != 0)
            return EBUSY;

        if (mount_data->conn != NULL && mount_data->conn->mounted_data == mount_data)
            mount_data->conn->mounted_data = NULL;

        file_system->vfs_data = NULL;
    }

    free_mount_nodes (mount_data);
    mount_data->nodes = NULL;
    mount_data->root = NULL;
    *reply = mount_data;

    return 0;
}

/* Caller holds global_lock. Claim exactly one finalizer after every user of
 * detached mount storage has finished, not merely after unlinking its nodes. */
static struct usfs_mount_data * claim_stale_mount (struct vfs * file_system)
{
    struct usfs_mount_data * mount = (struct usfs_mount_data *)file_system->vfs_data;

    if (mount == NULL || mount->state != USFS_MOUNT_STALE || mount->nodes != NULL || mount->reclaiming_nodes != 0 || mount->inflight_vgets != 0 ||
        mount->unmount_in_progress)
        return NULL;

    file_system->vfs_data = NULL;
    return mount;
}

static void finish_stale_mount (struct vfs * file_system, struct usfs_mount_data * mount)
{
    if (mount == NULL)
        return;

    const int deferred = mount->stale_accounted;
    if (deferred)
        on_stale_mount_released ();

    release_detached_mount (mount);

    /* AIX vnop_rele contract: release the VFS after its last stale vnode.
     * An immediate, empty unmount retains the ordinary LFS convention. */
    if (deferred)
        (void)vfsrele (file_system);
}

static void release_detached_mount (struct usfs_mount_data * mount_data)
{
    if (mount_data == NULL)
    {
        return;
    }

    struct usfs_connection * connection = mount_data->conn;
    lock_free (&mount_data->namespace_lock);
    xmfree (mount_data, kernel_heap);

    if (connection != NULL)
    {
        release_connection (connection);
    }
}

/*
 * A forced unmount is a transport recovery boundary.  The VFS disappears
 * from the namespace immediately, but AIX file structures and mappings may
 * still own vnodes.  Keep every such vnode, its gnode, and the mount data
 * alive until gn_rele observes the last vnode release.  Quarantining the
 * connection wakes wedged requests and prevents any stale vnode operation
 * from starting new protocol traffic.
 */
static int force_unmount (struct vfs * file_system)
{
    struct usfs_mount_data * mount_data = NULL;
    int deferred = 0;

    write_synchronized_with (global_lock)
    {
        mount_data = (struct usfs_mount_data *)file_system->vfs_data;
        if (mount_data == NULL || mount_data->state != USFS_MOUNT_ACTIVE)
            return EIO;

        mount_data->state = USFS_MOUNT_STALE;
        mount_data->unmount_in_progress = 1;

        if (mount_data->conn != NULL && mount_data->conn->mounted_data == mount_data)
            mount_data->conn->mounted_data = NULL;

        deferred = mount_data->nodes != NULL || mount_data->reclaiming_nodes != 0 || mount_data->inflight_vgets != 0;
        mount_data->stale_accounted = deferred;
    }

    if (mount_data == NULL)
        return EIO;

    /* The mount is stale while an in-flight vget retains its VFS reference.
     * This test rendezvous runs outside global_lock and cannot skip cleanup. */
    (void)usfs_checkpoint (USFS_INSTRUMENT_FORCE_UNMOUNT_STALE);

    /* A connection is the protocol isolation boundary.  If a daemon bound
     * more than one mount to this channel, all of those mounts fail fast and
     * must be reconciled by the operator. */
    if (mount_data->conn != NULL)
    {
        on_forced_recovery (mount_data->conn, deferred);
        abort_connection (mount_data->conn);
    }

    usfs_lifecycle_release_mount (mount_data->conn);

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_MOUNT,
        USFS_TRACE_ACTION_FORCE,
        mount_data->conn == NULL ? -1 : mount_data->conn->channel,
        USFS_MOUNT_STALE,
        g_mount_count,
        0
    );

    write_synchronized_with (global_lock)
    {
        mount_data->unmount_in_progress = 0;
        mount_data = claim_stale_mount (file_system);
    }

    finish_stale_mount (file_system, mount_data);

    return 0;
}

static int trace_unmount_result (int channel, int vfs_number, int rc)
{
    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_UNMOUNT, channel, vfs_number, rc, g_mount_count);
    return rc;
}

int usfs_unmount (struct vfs * file_system, int unmount_flags, struct ucred * credentials)
{
    int channel = -1;
    const int vfs_number = file_system == NULL ? 0 : file_system->vfs_number;

    ignore_parameter credentials;

    if (file_system != NULL && file_system->vfs_data != NULL)
        channel = ((struct usfs_mount_data *)file_system->vfs_data)->conn->channel;

    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_UNMOUNT, channel, vfs_number, unmount_flags, 0);

    if ((unmount_flags & UVMNT_FORCE) != 0)
        return trace_unmount_result (channel, vfs_number, force_unmount (file_system));

    struct usfs_mount_data * mount_data = NULL;
    const int rc = detach_mount_data (file_system, &mount_data);

    if (rc != 0)
        return trace_unmount_result (channel, vfs_number, rc);

    // Matches the reservation made by usfs_mount and synchronizes with
    // CFG_TERM's closed-gate user check.
    usfs_lifecycle_finish_mount (mount_data->conn);

    release_detached_mount (mount_data);
    USFS_TRACE_CONTROL5 (USFS_TRACE_MOUNT, USFS_TRACE_ACTION_UNMOUNT, channel, USFS_MOUNT_ACTIVE, g_mount_count, 0);
    return trace_unmount_result (channel, vfs_number, 0);
}

#endif
