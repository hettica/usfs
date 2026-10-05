// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_OPEN_H
#define USFS_FS_OPERATIONS_VNODE_OPEN_H

struct open_request_context
{
    struct vnode * file_vnode;           // Vnode being opened.
    struct usfs_mount_data * mount_data; // Mounted instance used for protocol traffic.
    struct usfs_node * node;             // USFS node corresponding to file_vnode.
    int32long64_t open_flags;            // AIX open flags supplied by the caller.
    struct ucred * credentials;          // Credentials attached to daemon requests.
};

static int request_open_handle (const struct open_request_context * context, uint64_t * file_handle)
{
    struct usfs_open_in request_body;
    struct usfs_open_out open_reply;
    struct usfs_request * request = NULL;

    if ((uint64_t)context->open_flags > UINT32_MAX)
        return EOPNOTSUPP;

    memset (&request_body, 0, sizeof (request_body));
    request_body.flags = (uint32_t)context->open_flags;
    request_body.isdir = context->file_vnode->v_gnode->gn_type == VDIR ? 1 : 0;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_OPEN,
                                                               .node_id = context->node->nodeid,
                                                               .opcode_specific_body = { .bytes = &request_body, .length = sizeof (request_body) },
                                                               .max_allowed_reply_buffer_size = (uint32_t)sizeof (open_reply),
                                                               .credentials = context->credentials };

    int rc = allocate_request (&request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (context->mount_data->conn, request);
    if (rc != 0)
        return rc;

    if (request->error != 0)
    {
        rc = request->error;
        free_request (request);
        return rc;
    }

    if (request->reply_buffer_size != sizeof (open_reply))
    {
        abort_connection (context->mount_data->conn);
        free_request (request);
        return EIO;
    }

    memcpy (&open_reply, request->reply_buffer, sizeof (open_reply));
    free_request (request);

    if (!usfs_open_out_valid (&open_reply))
    {
        abort_connection (context->mount_data->conn);
        return EIO;
    }

    *file_handle = open_reply.fh;
    return 0;
}

static int truncate_open_handle (const struct open_request_context * context, const uint64_t file_handle)
{
    struct usfs_setattr_in trunc;

    if ((context->open_flags & FTRUNC) == 0 || context->file_vnode->v_gnode->gn_type == VDIR)
        return 0;

    memset (&trunc, 0, sizeof (trunc));
    trunc.valid = USFS_SET_SIZE;
    trunc.size = 0;

    const int rc = _setattr (context->file_vnode, &trunc, file_handle, context->credentials);
    if (rc != 0)
        _send_release (context->mount_data, context->node->nodeid, file_handle, context->open_flags, 0, context->credentials);

    return rc;
}

static int open_access_mode (int32long64_t open_flags)
{
    int mode = 0;

    if ((open_flags & FEXEC) != 0)
    {
        mode |= X_ACC;
    }

    if ((open_flags & FREAD) != 0)
    {
        mode |= R_ACC;
    }

    if ((open_flags & (FWRITE | FTRUNC)) != 0)
    {
        mode |= W_ACC;
    }

    return mode;
}

int gn_open (struct vnode * file_vnode, int32long64_t open_flags, ext_t extension, caddr_t * file_info, struct ucred * credentials)
{
    ignore_parameter extension;

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);
    struct usfs_open_state * open_state = NULL;
    uint64_t file_handle = 0;

    if (mount_data == NULL || node == NULL || file_info == NULL)
        return (file_info == NULL) ? EINVAL : EIO;

    *file_info = NULL;

    const int access_rc = gn_access (file_vnode, open_access_mode (open_flags), ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    int rc = allocate_open_state (open_flags, &open_state);
    if (rc != 0)
        return rc;

    const struct open_request_context request_context = { .file_vnode = file_vnode,
                                                          .mount_data = mount_data,
                                                          .node = node,
                                                          .open_flags = open_flags,
                                                          .credentials = credentials };

    rc = request_open_handle (&request_context, &file_handle);

    // Emptying the file for FTRUNC belongs here: the logical file system only
    // checks that a truncating open is not aimed at a read-only file system,
    // and leaves the truncation itself to the file system implementation. It
    // does not follow up with a vnop_ftrunc, so an open(O_TRUNC) of an existing
    // file would otherwise keep every byte past what the caller then writes.
    //
    // Done before the open is committed below, so that a failure leaves no
    // reference behind: the logical file system issues no vnop_close for a
    // vnop_open that returned an error, and the handle the daemon has already
    // created is handed straight back for the same reason.
    if (rc == 0)
    {
        rc = truncate_open_handle (&request_context, file_handle);
    }

    if (rc != 0)
    {
        usfs_open_state_discard (open_state);
        return rc;
    }

    open_state->fh = file_handle;
    attach_open_state (node, open_state);
    *file_info = (caddr_t)open_state;

    return 0;
}

#endif
