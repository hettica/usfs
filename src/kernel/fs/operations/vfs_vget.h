// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VFS_VGET_H
#define USFS_FS_OPERATIONS_VFS_VGET_H

static int begin_vget (struct vfs * file_system, struct usfs_mount_data ** retained_mount)
{
    write_synchronized_with (global_lock)
    {
        struct usfs_mount_data * mount_data = (struct usfs_mount_data *)file_system->vfs_data;

        if (mount_data == NULL || mount_data->state != USFS_MOUNT_ACTIVE || mount_data->conn == NULL)
            return ESTALE;

        if (mount_data->inflight_vgets == UINT32_MAX)
            return EOVERFLOW;

        mount_data->inflight_vgets++;
        vfs_hold (file_system);
        *retained_mount = mount_data;
    }

    return 0;
}

static void end_vget (struct vfs * file_system, struct usfs_mount_data * mount_data)
{
    struct usfs_mount_data * stale_mount = NULL;

    write_synchronized_with (global_lock)
    {
        mount_data->inflight_vgets--;
        stale_mount = claim_stale_mount (file_system);
    }

    (void)vfs_unhold (file_system);
    finish_stale_mount (file_system, stale_mount);
}

static int vget_connection_is_terminal (struct usfs_connection * connection)
{
    int is_terminal = false;

    synchronized_with (connection->lock)
    {
        is_terminal = connection->state == USFS_CONN_DEAD || connection->state == USFS_CONN_CLOSED;
    }

    return is_terminal;
}

static int request_vget_entry (struct usfs_mount_data * mount_data, const uint64_t token, struct ucred * credentials, struct usfs_entry_out * entry)
{
    const struct usfs_vget_in body = { .token = token };
    struct usfs_request_allocation_spec request_spec = { 0 };

    request_spec.opcode = USFS_OP_VGET;
    request_spec.node_id = USFS_ROOT_ID;
    request_spec.opcode_specific_body.bytes = &body;
    request_spec.opcode_specific_body.length = sizeof (body);
    request_spec.max_allowed_reply_buffer_size = sizeof (struct usfs_vget_out);
    request_spec.credentials = credentials;

    struct usfs_request * request = NULL;
    int rc = allocate_request (&request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (mount_data->conn, request);
    if (rc != 0)
    {
        if (rc == EIO && vget_connection_is_terminal (mount_data->conn))
            return ESTALE;

        return rc;
    }

    if (request->error != 0)
    {
        rc = request->error == ENOENT ? ESTALE : request->error;
    }
    else if (request->reply_buffer_size != sizeof (struct usfs_vget_out) || !usfs_vget_out_valid ((const struct usfs_vget_out *)request->reply_buffer, token))
    {
        abort_connection (mount_data->conn);
        rc = EIO;
    }
    else
    {
        const struct usfs_vget_out * reply = (const struct usfs_vget_out *)request->reply_buffer;
        *entry = reply->entry;
    }

    free_request (request);
    return rc;
}

static struct usfs_node * find_vget_node (struct usfs_mount_data * mount_data, const uint64_t node_id)
{
    for (struct usfs_node * node = mount_data->nodes; node != NULL; node = node->next)
    {
        if (node->nodeid == node_id)
            return node;
    }

    return NULL;
}

static int attach_vget_entry (
    struct vfs * file_system,
    struct usfs_mount_data * mount_data,
    const struct usfs_entry_out * entry,
    const uint64_t token,
    struct vnode ** result_vnode
)
{
    const int vnode_type = usfs_vtype_from_mode (entry->attr.mode);

    write_synchronized_with (global_lock)
    {
        if (mount_data->state != USFS_MOUNT_ACTIVE)
            return ESTALE;

        struct usfs_node * node = find_vget_node (mount_data, entry->nodeid);

        if (node != NULL)
        {
            if ((int)node->vn->v_vntype != vnode_type)
                return EIO;

            if (node->fid_token != 0 && node->fid_token != token)
                return EIO;

            if (node->lookup_refs == UINT64_MAX)
                return EOVERFLOW;

            usfs_vnode_hold_locked (node->vn, USFS_INSTRUMENT_NODE_REUSE_LOOKUP);
        }
        else
        {
            const int is_root = entry->nodeid == USFS_ROOT_ID;
            const uint64_t parent_node_id = is_root ? USFS_ROOT_ID : 0;
            const int rc = _create_node (file_system, mount_data, entry->nodeid, parent_node_id, vnode_type, is_root, &node);
            if (rc != 0)
                return rc;

            if (is_root)
                mount_data->root = node;
        }

        node->fid_token = token;
        node->lookup_refs++;
        *result_vnode = node->vn;
    }

    return 0;
}

static int resolve_file_identifier (
    struct vfs * file_system,
    struct usfs_mount_data * mount_data,
    const uint64_t token,
    struct vnode ** result_vnode,
    struct ucred * credentials
)
{
    struct usfs_entry_out entry = { 0 };

    int rc = request_vget_entry (mount_data, token, credentials, &entry);
    if (rc != 0)
        return rc;

    rc = attach_vget_entry (file_system, mount_data, &entry, token, result_vnode);
    if (rc == EIO)
        abort_connection (mount_data->conn);

    if (rc != 0)
        (void)usfs_forget (mount_data->conn, entry.nodeid, 1);

    return rc;
}

int usfs_vget (struct vfs * file_system, struct vnode ** result_vnode, struct fileid * file_id, struct ucred * credentials)
{
    if (result_vnode == NULL)
        return EINVAL;

    *result_vnode = NULL;

    if (file_system == NULL || file_id == NULL)
        return EINVAL;

    const uint32_t mount_serial = file_system->vfs_number;

    USFS_TRACE_FS5 (USFS_TRACE_VFS_ENTRY, USFS_TRACE_VFS_VGET, -1, mount_serial, 0, 0);

    uint64_t token = 0;
    int rc = decode_file_identifier (file_id, mount_serial, &token);
    if (rc != 0)
        return rc;

    struct usfs_mount_data * mount_data = NULL;
    rc = begin_vget (file_system, &mount_data);
    if (rc != 0)
        return rc;

    rc = usfs_checkpoint (USFS_INSTRUMENT_VGET_RESERVED);
    if (rc == 0)
    {
        read_synchronized_with (global_lock)
        {
            if (mount_data->state != USFS_MOUNT_ACTIVE)
                rc = ESTALE;
        }
    }

    if (rc == 0)
        rc = resolve_file_identifier (file_system, mount_data, token, result_vnode, credentials);
    end_vget (file_system, mount_data);

    USFS_TRACE_FS5 (USFS_TRACE_VFS_RESULT, USFS_TRACE_VFS_VGET, -1, mount_serial, rc, rc == 0 ? token : 0);

    return rc;
}

#endif
