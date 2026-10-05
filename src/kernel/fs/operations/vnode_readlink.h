// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_READLINK_H
#define USFS_FS_OPERATIONS_VNODE_READLINK_H

static int request_link_target (
    const struct usfs_mount_data * mount_data,
    const struct usfs_node * node,
    struct ucred * credentials,
    struct usfs_request ** result_request
)
{
    struct usfs_request * request = NULL;
    const struct usfs_request_allocation_spec request_spec = {
        .opcode = (uint16_t)USFS_OP_READLINK,
        .node_id = node->nodeid,
        .max_allowed_reply_buffer_size = USFS_MAX_LINK,
        .credentials = credentials
    };

    int rc = allocate_request (&request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (mount_data->conn, request);
    if (rc != 0)
        return rc;

    if (request->error != 0)
    {
        rc = request->error;
        free_request (request);
        return rc;
    }

    *result_request = request;
    return 0;
}

int gn_readlink (struct vnode * file_vnode, struct uio * user_io_request, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL || node == NULL)
        return EIO;

    if (file_vnode->v_gnode->gn_type != VLNK)
        return EINVAL;

    if (user_io_request->uio_resid <= 0)
        return 0;

    struct usfs_request * request = NULL;
    int rc = request_link_target (mount_data, node, credentials, &request);
    if (rc != 0)
        return rc;

    // The reply carries the target without a terminating NUL; the caller
    // learns its length from how much of the uio this consumes.
    const uint32_t len = request->reply_buffer_size;
    const uint64_t off = (uint64_t)user_io_request->uio_offset;

    if (off >= (uint64_t)len)
    {
        free_request (request);
        return 0;
    }

    uint32_t avail = len - (uint32_t)off;

    if ((uint64_t)user_io_request->uio_resid < (uint64_t)avail)
    {
        avail = (uint32_t)user_io_request->uio_resid;
    }

    rc = uiomove (request->reply_buffer + off, (long)avail, UIO_READ, user_io_request);

    free_request (request);

    return rc;
}

#endif
