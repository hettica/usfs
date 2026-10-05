// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_FID_H
#define USFS_FS_OPERATIONS_VNODE_FID_H

static int fid_connection_is_terminal (struct usfs_connection * connection)
{
    int is_terminal = false;

    synchronized_with (connection->lock)
    {
        is_terminal = connection->state == USFS_CONN_DEAD || connection->state == USFS_CONN_CLOSED;
    }

    return is_terminal;
}

static int request_backend_file_identifier (struct usfs_mount_data * mount_data, const uint64_t node_id, struct ucred * credentials, uint64_t * token)
{
    struct usfs_request_allocation_spec request_spec = { 0 };

    request_spec.opcode = USFS_OP_FID;
    request_spec.node_id = node_id;
    request_spec.max_allowed_reply_buffer_size = sizeof (struct usfs_fid_out);
    request_spec.credentials = credentials;

    struct usfs_request * request = NULL;
    int rc = allocate_request (&request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (mount_data->conn, request);
    if (rc != 0)
    {
        if (rc == EIO && fid_connection_is_terminal (mount_data->conn))
            return ESTALE;

        return rc;
    }

    if (request->error != 0)
    {
        rc = request->error;
    }
    else if (request->reply_buffer_size != sizeof (struct usfs_fid_out) || !usfs_fid_out_valid ((const struct usfs_fid_out *)request->reply_buffer))
    {
        abort_connection (mount_data->conn);
        rc = EIO;
    }
    else
    {
        const struct usfs_fid_out * reply = (const struct usfs_fid_out *)request->reply_buffer;
        *token = reply->token;
    }

    free_request (request);
    return rc;
}

static int remember_file_identifier (struct usfs_mount_data * mount_data, struct usfs_node * node, const uint64_t token)
{
    write_synchronized_with (global_lock)
    {
        if (mount_data->state != USFS_MOUNT_ACTIVE)
            return ESTALE;

        if (node->fid_token != 0 && node->fid_token != token)
            return EIO;

        node->fid_token = token;
    }

    return 0;
}

int gn_fid (struct vnode * file_vnode, struct fileid * file_id, struct ucred * credentials)
{
    if (file_vnode == NULL || file_id == NULL)
        return EINVAL;

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL)
        return ESTALE;

    if (node == NULL || mount_data->conn == NULL)
        return ESTALE;

    if (mount_data->state != USFS_MOUNT_ACTIVE)
        return ESTALE;

    uint64_t token = 0;
    const int rc = request_backend_file_identifier (mount_data, node->nodeid, credentials, &token);
    if (rc != 0)
        return rc;

    const int remember_rc = remember_file_identifier (mount_data, node, token);
    if (remember_rc == EIO)
        abort_connection (mount_data->conn);

    if (remember_rc != 0)
        return remember_rc;

    encode_file_identifier (file_id, file_vnode->v_vfsp->vfs_number, token);
    return 0;
}

#endif
