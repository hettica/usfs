// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_RDWR_H
#define USFS_FS_OPERATIONS_VNODE_RDWR_H

struct rdwr_context
{
    struct usfs_mount_data * mount_data; // Mounted instance used for protocol traffic.
    struct usfs_node * node;             // File node being transferred.
    uint64_t file_handle;                // Daemon handle retained for the transfer.
    int32long64_t open_flags;            // Effective AIX file flags for the transfer.
    struct uio * user_io_request;        // AIX buffer, residual count, and current offset.
    struct ucred * credentials;          // Credentials attached to daemon requests.
};

struct write_chunk_result
{
    uint32_t written; // Number of bytes committed by the daemon.
    int daemon_error; // Whether failure came from the daemon reply.
};

static int allocate_write_chunk_request (
    const struct rdwr_context * context,
    const uint32_t append,
    const uint32_t user_data_size,
    struct usfs_write_in * request_body,
    struct usfs_request ** request
)
{
    memset (request_body, 0, sizeof (*request_body));
    request_body->fh = context->file_handle;
    request_body->offset = append ? 0 : (uint64_t)context->user_io_request->uio_offset;
    request_body->size = user_data_size;
    request_body->flags = append;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_WRITE,
                                                               .node_id = context->node->nodeid,
                                                               .opcode_specific_body = { .bytes = request_body, .length = sizeof (*request_body) },
                                                               .opcode_specific_payload = { .length = user_data_size },
                                                               .max_allowed_reply_buffer_size = (uint32_t)sizeof (struct usfs_write_out),
                                                               .credentials = context->credentials };

    return allocate_request (&request_spec, request);
}

/* uiomove mutates both the uio and its iovecs. Copy one private vector at a
 * time so a copy fault or rejected WRITE cannot consume the caller's data. */
static int copy_write_payload (const struct uio * user_io_request, caddr_t payload, const uint32_t byte_count)
{
    uint32_t remaining = byte_count;

    for (int32long64_t index = 0; index < user_io_request->uio_iovcnt && remaining != 0; ++index)
    {
        struct iovec vector = user_io_request->uio_iov[index];
        if (vector.iov_len == 0)
            continue;

        const uint32_t copy_size = vector.iov_len < remaining ? (uint32_t)vector.iov_len : remaining;
        struct uio copy_request = *user_io_request;
        copy_request.uio_iov = &vector;
        copy_request.uio_iovcnt = 1;
        copy_request.uio_iovdcnt = 0;
        copy_request.uio_offset = 0;
        copy_request.uio_resid = copy_size;
        if (copy_request.uio_xmem != NULL)
            copy_request.uio_xmem += index;

        const int rc = usfs_kuiomove (USFS_UIOMOVE_WRITE_DATA, payload, copy_size, UIO_WRITE, &copy_request);
        if (rc != 0)
            return rc;

        if (copy_request.uio_resid != 0)
            return EIO;

        payload += copy_size;
        remaining -= copy_size;
    }

    return remaining == 0 ? 0 : EINVAL;
}

/* The payload copy has already validated this range. Advance only the bytes
 * acknowledged by the daemon, retaining matching iovec and xmem cursors. */
static void commit_write_payload (struct uio * user_io_request, const uint32_t byte_count)
{
    uint32_t remaining = byte_count;

    while (remaining != 0)
    {
        struct iovec * vector = user_io_request->uio_iov;
        const uint32_t consumed = vector->iov_len < remaining ? (uint32_t)vector->iov_len : remaining;

        if (consumed != 0)
        {
            vector->iov_base += consumed;
            vector->iov_len -= consumed;
            user_io_request->uio_resid -= consumed;
            remaining -= consumed;
        }

        if (vector->iov_len == 0)
        {
            user_io_request->uio_iov++;
            user_io_request->uio_iovcnt--;
            user_io_request->uio_iovdcnt++;
            if (user_io_request->uio_xmem != NULL)
                user_io_request->uio_xmem++;
        }
    }
}

static int is_write_chunk_reply_valid (
    const struct usfs_write_out * reply,
    const uint32_t append,
    const uint32_t user_data_size,
    const offset_t original_offset
)
{
    if (!usfs_write_out_valid (reply, user_data_size))
        return false;

    if (append)
        return true;

    return reply->offset == (uint64_t)original_offset;
}

static int write_data_chunk (
    const struct rdwr_context * context,
    const uint32_t append,
    const uint32_t user_data_size,
    struct write_chunk_result * result
)
{
    struct uio * user_io_request = context->user_io_request;
    const offset_t original_offset = user_io_request->uio_offset;
    struct usfs_write_in request_body;
    struct usfs_request * request = NULL;

    result->written = 0;
    result->daemon_error = false;

    int rc = allocate_write_chunk_request (context, append, user_data_size, &request_body, &request);
    if (rc != 0)
        return rc;

    rc = copy_write_payload (user_io_request, usfs_msg_data (request, sizeof (request_body), 0), user_data_size);

    if (rc != 0)
    {
        free_request (request);
        return rc;
    }

    rc = usfs_call (context->mount_data->conn, request);

    if (rc != 0)
        return rc;

    if (request->error != 0)
    {
        const int daemon_error = request->error;

        free_request (request);
        result->daemon_error = true;
        return daemon_error;
    }

    if (request->reply_buffer_size < sizeof (struct usfs_write_out))
    {
        free_request (request);
        return EIO;
    }

    struct usfs_write_out reply;

    memcpy (&reply, request->reply_buffer, sizeof (reply));
    free_request (request);

    if (!is_write_chunk_reply_valid (&reply, append, user_data_size, original_offset))
        return EIO;

    commit_write_payload (user_io_request, reply.written);

    user_io_request->uio_offset = reply.written == 0 ? original_offset : (offset_t)(reply.offset + reply.written);
    result->written = reply.written;
    return 0;
}

static int write_data_chunks (const struct rdwr_context * context)
{
    const struct uio * user_io_request = context->user_io_request;
    const int32long64_t open_flags = context->open_flags;
    const long fmode = (long)open_flags | user_io_request->uio_fmode;
    uint32_t append = ((fmode & FAPPEND) != 0) ? USFS_WRITE_APPEND : 0;
    uint64_t total = 0;

    while (user_io_request->uio_resid > 0)
    {
        const uint32_t chunk = usfs_rdwr_chunk_size (user_io_request);
        struct write_chunk_result chunk_result;

        if (!append && !usfs_file_offset_count_valid ((int64_t)user_io_request->uio_offset, chunk))
        {
            return EFBIG;
        }

        const int rc = write_data_chunk (context, append, chunk, &chunk_result);
        if (rc != 0)
        {
            if (chunk_result.daemon_error && total > 0)
                return 0;

            return rc;
        }

        total += chunk_result.written;
        /* Only the first chunk chooses EOF. Subsequent chunks continue from
           the actual committed position while syscall ownership is retained. */
        append = 0;
        if (chunk_result.written < chunk)
            return (total > 0) ? 0 : ENOSPC;
    }

    return 0;
}

static int write_uncached_file_data (const struct rdwr_context * context)
{
    const int rc = write_data_chunks (context);
    if (rc != 0)
        return rc;

    const int32long64_t effective_open_flags = context->open_flags | context->user_io_request->uio_fmode;
    if ((effective_open_flags & (FSYNC | FDATASYNC)) == 0)
        return 0;

    return _sync_node (context->node->vn, context->file_handle, effective_open_flags, 0, 0, 0, context->credentials);
}

static int write_file_data_locked (const struct rdwr_context * context)
{
    if (context->user_io_request->uio_resid > 0)
    {
        const int rc = usfs_strip_write_privileges (context->node->vn, context->file_handle, context->credentials);
        if (rc != 0)
            return rc;
    }

    if (context->node->gn->gn_seg != 0)
    {
        return transfer_cached_data (
            context->node->vn,
            context->file_handle,
            UIO_WRITE,
            context->open_flags,
            context->user_io_request,
            context->credentials
        );
    }

    return write_uncached_file_data (context);
}

static int write_file_data (const struct rdwr_context * context)
{
    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_read_synchronized_acquire (&context->mount_data->namespace_lock);

    write_synchronized_with (context->node->mutation_lock)
    {
        return write_file_data_locked (context);
    }

    return EIO;
}

static int allocate_read_chunk_request (
    const struct rdwr_context * context,
    const uint32_t user_data_size,
    struct usfs_read_in * request_body,
    struct usfs_request ** request
)
{
    memset (request_body, 0, sizeof (*request_body));
    request_body->fh = context->file_handle;
    request_body->offset = (uint64_t)context->user_io_request->uio_offset;
    request_body->size = user_data_size;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_READ,
                                                               .node_id = context->node->nodeid,
                                                               .opcode_specific_body = { .bytes = request_body, .length = sizeof (*request_body) },
                                                               .max_allowed_reply_buffer_size = user_data_size,
                                                               .credentials = context->credentials };

    return allocate_request (&request_spec, request);
}

static int copy_read_chunk_reply (const struct usfs_request * request, struct uio * user_io_request, uint32_t * bytes_read)
{
    *bytes_read = request->reply_buffer_size;
    if (*bytes_read == 0)
        return 0;

    return uiomove (request->reply_buffer, (long)*bytes_read, UIO_READ, user_io_request);
}

static int read_data_chunk (const struct rdwr_context * context, const uint32_t user_data_size, uint32_t * bytes_read)
{
    struct usfs_read_in request_body;
    struct usfs_request * request = NULL;

    *bytes_read = 0;

    int rc = allocate_read_chunk_request (context, user_data_size, &request_body, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (context->mount_data->conn, request);
    if (rc != 0)
        return rc;

    if (request->error != 0)
    {
        const int daemon_error = request->error;
        free_request (request);
        return daemon_error;
    }

    rc = copy_read_chunk_reply (request, context->user_io_request, bytes_read);
    free_request (request);

    return rc;
}

static int read_data_chunks (const struct rdwr_context * context)
{
    const struct uio * user_io_request = context->user_io_request;

    while (user_io_request->uio_resid > 0)
    {
        const uint32_t chunk = usfs_rdwr_chunk_size (user_io_request);
        uint32_t bytes_read;

        if (!usfs_file_offset_count_valid ((int64_t)user_io_request->uio_offset, chunk))
        {
            return EFBIG;
        }

        const int rc = read_data_chunk (context, chunk, &bytes_read);
        if (rc != 0)
            return rc;

        if (bytes_read < chunk)
            return 0;
    }

    return 0;
}

static int read_file_data (const struct rdwr_context * context)
{
    struct usfs_node * node = context->node;

    write_synchronized_with (node->mutation_lock)
    {
        if (node->gn->gn_seg != 0)
            return transfer_cached_data (node->vn, context->file_handle, UIO_READ, 0, context->user_io_request, context->credentials);

        return read_data_chunks (context);
    }

    return EIO;
}

static void refresh_file_attributes (struct vnode * file_vnode, struct vattr * attributes, struct ucred * credentials)
{
    if (attributes != NULL)
    {
        (void)gn_getattr (file_vnode, attributes, credentials);
    }
}

int gn_rdwr (
    struct vnode * file_vnode,
    enum uio_rw operation,
    int32long64_t open_flags,
    struct uio * user_io_request,
    ext_t extension,
    caddr_t file_info,
    struct vattr * attributes,
    struct ucred * credentials
)
{
    ignore_parameter extension;

    int rc = usfs_validate_rdwr_arguments (file_vnode, operation, user_io_request);
    if (rc != 0)
        return rc;

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL || node == NULL)
        return EIO;

    struct usfs_open_state * open_state = NULL;
    rc = usfs_borrow_description (node, file_info, &open_state);
    if (rc != 0)
        return rc;

    if (operation != UIO_READ)
    {
        if (!mount_data->writable)
        {
            usfs_put_open_reference (file_vnode, open_state, 1, credentials);
            return EROFS;
        }

        const struct rdwr_context context = { .mount_data = mount_data,
                                              .node = node,
                                              .file_handle = open_state->fh,
                                              .open_flags = open_flags,
                                              .user_io_request = user_io_request,
                                              .credentials = credentials };

        rc = write_file_data (&context);
        if (rc == 0)
            refresh_file_attributes (file_vnode, attributes, credentials);


        usfs_put_open_reference (file_vnode, open_state, 1, credentials);
        return rc;
    }

    const struct rdwr_context context = { .mount_data = mount_data,
                                          .node = node,
                                          .file_handle = open_state->fh,
                                          .open_flags = open_flags,
                                          .user_io_request = user_io_request,
                                          .credentials = credentials };

    rc = read_file_data (&context);
    if (rc == 0)
        refresh_file_attributes (file_vnode, attributes, credentials);

    usfs_put_open_reference (file_vnode, open_state, 1, credentials);

    return rc;
}

#endif
