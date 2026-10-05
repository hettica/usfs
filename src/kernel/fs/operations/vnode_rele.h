// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_RELE_H
#define USFS_FS_OPERATIONS_VNODE_RELE_H

static void reclaim_vnode (const struct vnode * file_vnode)
{
    const struct usfs_node * node = (struct usfs_node *)file_vnode->v_gnode->gn_data;
    struct usfs_mount_data * mount_data = (struct usfs_mount_data *)file_vnode->v_vfsp->vfs_data;

    if (node == NULL || mount_data == NULL)
        return;

    ++mount_data->reclaiming_nodes;
    struct usfs_node ** link = &mount_data->nodes;

    while (*link != NULL && *link != node)
    {
        link = &(*link)->next;
    }

    const int linked = (*link == node);

    if (linked)
    {
        *link = node->next;
    }

    if (mount_data->root == node)
    {
        mount_data->root = NULL;
    }

    usfs_instrumentation_node_reclaimed (node->nodeid, linked);
}

struct vnode_release_state
{
    struct vfs * file_system;                   // VFS needed after the vnode storage is freed.
    struct usfs_node * reclaimed_node;          // Node detached by the final reference release.
    struct usfs_mount_data * reclaimed_mount;   // Mount protected by reclaiming_nodes during disposal.
    struct usfs_connection * forget_connection; // Connection retained for the deferred FORGET.
    uint64_t forgotten_node_id;                 // Protocol node whose lookup references are released.
    uint64_t forgotten_lookup_count;            // Lookup references transferred to the deferred FORGET.
    int cache_drained;                          // Whether the final cached pages were already flushed.
};

static int should_drain_vnode_cache_locked (const struct vnode * file_vnode, const struct vnode_release_state * release_state)
{
    if (release_state->cache_drained)
        return false;

    if (file_vnode->v_count != 1)
        return false;

    return file_vnode->v_gnode->gn_seg != 0;
}

static void prepare_lookup_forget_locked (
    const struct usfs_node * node,
    const struct usfs_mount_data * mount_data,
    struct vnode_release_state * release_state
)
{
    if (node == NULL)
        return;

    if (mount_data == NULL)
        return;

    if (node->lookup_refs == 0)
        return;

    release_state->forgotten_node_id = node->nodeid;
    release_state->forgotten_lookup_count = node->lookup_refs;
    release_state->forget_connection = mount_data->conn;

    if (release_state->forget_connection != NULL)
        acquire_connection (release_state->forget_connection);
}

static int release_vnode_reference_locked (struct vnode * file_vnode, struct vnode_release_state * release_state)
{
    const int should_drain = should_drain_vnode_cache_locked (file_vnode, release_state);

    if (should_drain)
        usfs_vnode_hold_locked (file_vnode, USFS_INSTRUMENT_NODE_REUSE_NONE);

    usfs_vnode_release_locked (file_vnode);

    if (file_vnode->v_count != 0)
        return should_drain;

    struct usfs_node * node = (struct usfs_node *)file_vnode->v_gnode->gn_data;
    struct usfs_mount_data * mount_data = (struct usfs_mount_data *)release_state->file_system->vfs_data;

    prepare_lookup_forget_locked (node, mount_data, release_state);
    release_state->reclaimed_node = node;
    release_state->reclaimed_mount = mount_data;
    reclaim_vnode (file_vnode);

    return should_drain;
}

static void release_vnode_reference (struct vnode * file_vnode, struct vnode_release_state * release_state)
{
    for (;;)
    {
        int should_drain = false;

        write_synchronized_with (global_lock)
        {
            should_drain = release_vnode_reference_locked (file_vnode, release_state);
        }

        if (!should_drain)
            return;

        /* This internal hold is published atomically with the final decrement.
         * Pager holds and new lookups can proceed while global_lock is free. */
        if (flush_cached_pages (file_vnode, 0, 0, 0, 0) != 0)
            abort_connection (((struct usfs_mount_data *)release_state->file_system->vfs_data)->conn);

        release_state->cache_drained = true;
    }
}

static void dispose_reclaimed_node (const struct vnode_release_state * release_state)
{
    struct usfs_node * reclaimed_node = release_state->reclaimed_node;

    if (reclaimed_node == NULL)
        return;

    dispose_cache_resources (reclaimed_node, release_state->reclaimed_mount);

    write_synchronized_with (global_lock)
    {
        lock_free (&reclaimed_node->pageout_lock);
        lock_free (&reclaimed_node->mutation_lock);
        _free_vnode (reclaimed_node->vn, reclaimed_node->vn_lock_occurrence);
        _free_gnode (reclaimed_node->gn, reclaimed_node->gn_lock_occurrence);
        xmfree (reclaimed_node, kernel_heap);
    }
}

static void forget_reclaimed_lookup_references (const struct vnode_release_state * release_state)
{
    if (release_state->forget_connection == NULL)
        return;

    (void)usfs_forget (release_state->forget_connection, release_state->forgotten_node_id, release_state->forgotten_lookup_count);
    release_connection (release_state->forget_connection);
}

static void finish_reclaimed_mount (const struct vnode_release_state * release_state)
{
    struct usfs_mount_data * stale_mount = NULL;

    if (release_state->reclaimed_mount != NULL)
    {
        write_synchronized_with (global_lock)
        {
            --release_state->reclaimed_mount->reclaiming_nodes;
            stale_mount = claim_stale_mount (release_state->file_system);
        }
    }

    finish_stale_mount (release_state->file_system, stale_mount);
}

int gn_rele (struct vnode * file_vnode)
{
    struct vnode_release_state release_state = { .file_system = file_vnode->v_vfsp };

    release_vnode_reference (file_vnode, &release_state);
    dispose_reclaimed_node (&release_state);
    forget_reclaimed_lookup_references (&release_state);
    finish_reclaimed_mount (&release_state);

    return 0;
}

#endif
