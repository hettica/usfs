// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_GETATTR_H
#define USFS_FS_OPERATIONS_VNODE_GETATTR_H

static int _request_node_attributes_with_handle (
    const struct usfs_mount_data * mount_data,
    const struct usfs_node * node,
    struct ucred * credentials,
    struct usfs_attr_out * attributes_reply,
    const uint64_t file_handle
)
{
    struct usfs_getattr_in request_body;
    struct usfs_request * request = NULL;

    memset (&request_body, 0, sizeof (request_body));
    request_body.fh = file_handle;

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = (uint16_t)USFS_OP_GETATTR,
        .node_id = node->nodeid,
        .opcode_specific_body = { .bytes = &request_body, .length = sizeof (request_body) },
        .max_allowed_reply_buffer_size = (uint32_t)sizeof (*attributes_reply),
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

    if (request->reply_buffer_size < sizeof (*attributes_reply))
    {
        free_request (request);
        return EIO;
    }

    memcpy (attributes_reply, request->reply_buffer, sizeof (*attributes_reply));
    free_request (request);
    return usfs_attr_out_valid (attributes_reply) ? 0 : EIO;
}

static int request_node_attributes (
    const struct usfs_mount_data * mount_data,
    struct usfs_node * node,
    struct ucred * credentials,
    struct usfs_attr_out * attributes_reply
)
{
    struct usfs_open_state * open_state = NULL;
    int rc = usfs_borrow_node_handle (node, 0, &open_state);
    if (rc != 0 && rc != EBADF)
        return rc;

    rc = _request_node_attributes_with_handle (mount_data, node, credentials, attributes_reply, open_state == NULL ? 0 : open_state->fh);
    usfs_put_open_reference (node->vn, open_state, 1, credentials);
    return rc;
}

int gn_getattr (struct vnode * file_vnode, struct vattr * attributes, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL || node == NULL)
        return EIO;

    // Root permissions are authoritative backend data too. Failed metadata
    // must fail authorization and stat; forced recovery uses mount identity
    // independently of these ordinary attribute requests.
    // Send the handle when the file is open, so a file that was unlinked while
    // open can still be stat'ed: its name is gone, and the handle is then the
    // only way for the daemon to find the object.
    struct usfs_attr_out attributes_reply;
    const int rc = request_node_attributes (mount_data, node, credentials, &attributes_reply);

    if (rc == 0)
    {
        usfs_attr_to_vattr (&attributes_reply.attr, attributes);

        read_synchronized_with (global_lock)
        {
            if (file_vnode->v_gnode != NULL && file_vnode->v_gnode->gn_seg != 0)
                attributes->va_size = (offset_t)node->cache_size;
        }

        return 0;
    }

    return rc;
}

#endif
