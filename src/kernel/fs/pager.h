// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_PAGER_H
#define USFS_FS_PAGER_H

#define USFS_PAGER_BUFFER_COUNT    64
#define USFS_PAGER_RESERVED_WRITES 16

enum usfs_pager_direction
{
    USFS_PAGER_READ_DIRECTION = 0,
    USFS_PAGER_WRITE_DIRECTION = 1
};

enum usfs_pager_trace_channel
{
    USFS_PAGER_NO_CONNECTION_CHANNEL = -1
};

enum usfs_pager_trace_layout
{
    USFS_PAGER_TRACE_DIRECTION_SHIFT = 32
};

struct pager_transfer
{
    const struct usfs_mount_data * mount_data; // Mount supplying the backend connection.
    struct usfs_node * node;                   // Node whose cached pages are transferred.
    caddr_t page_data;                         // Attached page buffer, owned by the caller.
    uint32_t byte_count;                       // Number of bytes requested from the backend.
    uint64_t offset;                           // File offset in bytes.
};

static uint32_t get_connection_trace_channel (const struct usfs_connection * connection)
{
    if (connection == NULL)
        return (uint32_t)USFS_PAGER_NO_CONNECTION_CHANNEL;

    return (uint32_t)connection->channel;
}

static uint32_t get_mount_trace_channel (const struct usfs_mount_data * mount_data)
{
    if (mount_data == NULL)
        return (uint32_t)USFS_PAGER_NO_CONNECTION_CHANNEL;

    return get_connection_trace_channel (mount_data->conn);
}

static void trace_transfer_entry (const enum usfs_pager_direction direction, const struct pager_transfer * transfer)
{
    USFS_TRACE_FS5 (
        USFS_TRACE_PAGER_ENTRY,
        ((uint64_t)(uint32_t)direction << USFS_PAGER_TRACE_DIRECTION_SHIFT) | get_mount_trace_channel (transfer->mount_data),
        transfer->node == NULL ? 0 : transfer->node->nodeid,
        transfer->offset,
        transfer->byte_count,
        0
    );
}

static int trace_transfer_result (const enum usfs_pager_direction direction, const struct pager_transfer * transfer, const int rc)
{
    USFS_TRACE_FS5 (
        USFS_TRACE_PAGER_RESULT,
        ((uint64_t)(uint32_t)direction << USFS_PAGER_TRACE_DIRECTION_SHIFT) | get_connection_trace_channel (transfer->mount_data->conn),
        transfer->node->nodeid,
        transfer->offset,
        transfer->byte_count,
        rc
    );

    return rc;
}

static int copy_read_reply_data (const caddr_t page_data, const uint32_t byte_count, const struct usfs_request * request)
{
    if (request->reply_buffer_size == 0)
        return EFAULT;

    memcpy (page_data, request->reply_buffer, request->reply_buffer_size);

    if (request->reply_buffer_size < byte_count)
        memset (page_data + request->reply_buffer_size, 0, byte_count - request->reply_buffer_size);

    return 0;
}

static int consume_write_reply (struct usfs_request * request, const uint32_t byte_count, const uint64_t offset)
{
    struct usfs_write_out reply_body;

    if (request->reply_buffer_size < sizeof (reply_body))
    {
        free_request (request);
        return EIO;
    }

    memcpy (&reply_body, request->reply_buffer, sizeof (reply_body));
    free_request (request);

    if (!usfs_write_out_valid (&reply_body, byte_count))
        return EIO;

    if (reply_body.written != byte_count)
        return EIO;

    if (reply_body.offset != offset)
        return EIO;

    return 0;
}

/* On success the caller owns the reply; every failure disposes of it. */
static int exchange_pager_request (
    struct usfs_connection * connection,
    const struct usfs_request_allocation_spec * request_spec,
    struct usfs_request ** reply
)
{
    struct usfs_request * request = NULL;

    int rc = allocate_request (request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call_with_specific_request_class (connection, request, USFS_REQUEST_PAGER);
    if (rc != 0)
        return rc;

    if (request->error != 0)
    {
        rc = request->error;
        free_request (request);

        return rc;
    }

    *reply = request;

    return 0;
}

static int request_pager_read (const struct pager_transfer * transfer, const uint64_t file_handle)
{
    if (!usfs_pager_count_valid (transfer->page_data, transfer->byte_count))
        return EINVAL;

    struct usfs_read_in request_body;

    memset (&request_body, 0, sizeof (request_body));
    request_body.fh = file_handle;
    request_body.offset = transfer->offset;
    request_body.size = transfer->byte_count;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_READ,
                                                               .node_id = transfer->node->nodeid,
                                                               .opcode_specific_body = { .bytes = &request_body, .length = sizeof (request_body) },
                                                               .max_allowed_reply_buffer_size = transfer->byte_count };
    struct usfs_request * request = NULL;

    int rc = exchange_pager_request (transfer->mount_data->conn, &request_spec, &request);
    if (rc != 0)
        return rc;

    rc = copy_read_reply_data (transfer->page_data, transfer->byte_count, request);
    free_request (request);

    return rc;
}

static int request_pager_write (const struct pager_transfer * transfer, const uint64_t file_handle)
{
    if (!transfer->mount_data->writable)
        return EROFS;

    if (!usfs_pager_count_valid (transfer->page_data, transfer->byte_count))
        return EINVAL;

    struct usfs_write_in request_body;

    memset (&request_body, 0, sizeof (request_body));
    request_body.fh = file_handle;
    request_body.offset = transfer->offset;
    request_body.size = transfer->byte_count;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_WRITE,
                                                               .node_id = transfer->node->nodeid,
                                                               .opcode_specific_body = { .bytes = &request_body, .length = sizeof (request_body) },
                                                               .opcode_specific_payload = { .bytes = transfer->page_data,
                                                                                            .length = transfer->byte_count },
                                                               .max_allowed_reply_buffer_size = (uint32_t)sizeof (struct usfs_write_out) };

    struct usfs_request * request = NULL;

    const int rc = exchange_pager_request (transfer->mount_data->conn, &request_spec, &request);
    if (rc != 0)
        return rc;

    return consume_write_reply (request, transfer->byte_count, transfer->offset);
}

static int read_pager_data (const struct pager_transfer * transfer, const uint64_t file_handle)
{
    trace_transfer_entry (USFS_PAGER_READ_DIRECTION, transfer);

    const int rc = request_pager_read (transfer, file_handle);

    return trace_transfer_result (USFS_PAGER_READ_DIRECTION, transfer, rc);
}

static int write_pager_data (const struct pager_transfer * transfer, const uint64_t file_handle)
{
    trace_transfer_entry (USFS_PAGER_WRITE_DIRECTION, transfer);

    const int rc = request_pager_write (transfer, file_handle);

    return trace_transfer_result (USFS_PAGER_WRITE_DIRECTION, transfer, rc);
}

struct usfs_pager_reference
{
    struct vnode * file_vnode;           // Vnode that owns the retained open state.
    struct usfs_open_state * open_state; // Open state retained until every submitted buffer completes.
};

static uint32_t calculate_transfer_count (const uint64_t file_size, const uint64_t offset, const uint32_t requested_count)
{
    const uint64_t remaining = file_size - offset;

    return requested_count > remaining ? (uint32_t)remaining : requested_count;
}

static int read_pager_buffer (struct pager_transfer transfer, struct usfs_open_state ** retained_state)
{
    const uint64_t file_size = get_cache_size (transfer.node);
    if (transfer.offset >= file_size)
        return EFAULT;

    const uint32_t requested_count = transfer.byte_count;

    transfer.byte_count = calculate_transfer_count (file_size, transfer.offset, requested_count);

    int rc = borrow_cache_handle (transfer.node, false, retained_state);
    if (rc != 0)
        return rc;

    rc = read_pager_data (&transfer, (*retained_state)->fh);
    if (rc != 0)
        return rc;

    if (transfer.byte_count < requested_count)
        memset (transfer.page_data + transfer.byte_count, 0, requested_count - transfer.byte_count);

    return 0;
}

static int write_buffer_to_backend (struct pager_transfer transfer, struct usfs_open_state ** retained_state)
{
    const uint64_t file_size = get_cache_size (transfer.node);
    if (transfer.offset >= file_size)
        return 0;

    transfer.byte_count = calculate_transfer_count (file_size, transfer.offset, transfer.byte_count);

    const int rc = borrow_cache_handle (transfer.node, true, retained_state);
    if (rc != 0)
        return rc;

    return write_pager_data (&transfer, (*retained_state)->fh);
}

static int write_pager_buffer (const struct pager_transfer * transfer, struct usfs_open_state ** retained_state)
{
    int rc = 0;

    write_synchronized_with (transfer->node->pageout_lock)
    {
        rc = write_buffer_to_backend (*transfer, retained_state);
    }

    record_writeback_error (transfer->node, rc);

    return rc;
}

static int resolve_buffer_transfer_target (
    const struct buf * buffer,
    struct vnode ** file_vnode,
    struct usfs_mount_data ** mount_data,
    struct usfs_node ** node
)
{
    if (buffer == NULL)
        return EIO;

    if (buffer->b_vp == NULL)
        return EIO;

    struct gnode * gnode = (struct gnode *)buffer->b_vp;
    struct vnode * resolved_vnode = gnode->gn_vnode;
    if (resolved_vnode == NULL)
        return EIO;

    if (resolved_vnode->v_gnode != gnode)
        return EIO;

    struct usfs_mount_data * resolved_mount_data = _mount_of (resolved_vnode);
    struct usfs_node * resolved_node = _node_of (resolved_vnode);
    if (resolved_mount_data == NULL)
        return EIO;

    if (resolved_node == NULL)
        return EIO;

    *file_vnode = resolved_vnode;
    *mount_data = resolved_mount_data;
    *node = resolved_node;

    return 0;
}

static int validate_buffer_transfer_request (const struct buf * buffer, uint64_t * offset, uint32_t * byte_count)
{
    const int rc = usfs_pager_offset (buffer, offset);
    if (rc != 0)
        return rc;

    if (buffer->b_bcount == 0)
        return EINVAL;

    if (buffer->b_bcount > USFS_MAX_DATA)
        return EINVAL;

    *byte_count = (uint32_t)buffer->b_bcount;
    if (!usfs_file_offset_count_valid ((int64_t)*offset, *byte_count))
        return EFBIG;

    return 0;
}

static int transfer_pager_buffer (const struct buf * buffer, struct usfs_pager_reference * reference)
{
    struct vnode * file_vnode;
    struct usfs_mount_data * mount_data;
    struct usfs_node * node;
    memset (reference, 0, sizeof (*reference));

    int rc = resolve_buffer_transfer_target (buffer, &file_vnode, &mount_data, &node);
    if (rc != 0)
        return rc;

    uint64_t offset;
    uint32_t byte_count;

    rc = validate_buffer_transfer_request (buffer, &offset, &byte_count);
    if (rc != 0)
        return rc;

    struct usfs_open_state * open_state = NULL;
    const struct pager_transfer transfer = { .mount_data = mount_data,
                                             .node = node,
                                             .page_data = vm_att (buffer->b_xmemd.subspace_id, buffer->b_baddr),
                                             .byte_count = byte_count,
                                             .offset = offset };

    if ((buffer->b_flags & B_READ) != 0)
        rc = read_pager_buffer (transfer, &open_state);
    else
        rc = write_pager_buffer (&transfer, &open_state);

    vm_det (transfer.page_data);
    reference->file_vnode = file_vnode;
    reference->open_state = open_state;

    return rc;
}

static int transfer_pager_buffer_in_thread_context (const struct buf * buffer, struct usfs_pager_reference * reference)
{
    ut_pgio_context_t context = { 0 };

    vm_thrpgio_push (&context);

    const int rc = transfer_pager_buffer (buffer, reference);
    vm_thrpgio_pop (&context);

    return rc;
}

static void complete_pager_buffer (struct buf * buffer, const int rc)
{
    buffer->b_resid = rc == 0 ? 0 : buffer->b_bcount;

    if (rc != 0)
    {
        buffer->b_flags |= B_ERROR;
        buffer->b_error = (char)rc;
    }
    else
    {
        buffer->b_flags &= ~B_ERROR;
        buffer->b_error = 0;
    }

    iodone (buffer);
}

static void release_retained_references (const struct usfs_pager_reference * references, const unsigned reference_count)
{
    if (reference_count == 0)
        return;

    ut_pgio_context_t context = { 0 };

    vm_thrpgio_push (&context);

    for (unsigned reference_index = 0; reference_index < reference_count; ++reference_index)
    {
        if (references[reference_index].open_state == NULL)
            continue;

        usfs_put_open_reference (references[reference_index].file_vnode, references[reference_index].open_state, true, NULL);
    }

    vm_thrpgio_pop (&context);
}

void usfs_pager_strategy (struct buf * head, const int flags, const int path)
{
    struct buf * buffer = head;

    struct usfs_pager_reference references[USFS_PAGER_BUFFER_COUNT];
    unsigned retained_reference_count = 0;

    (void)flags;
    (void)path;

    while (buffer != NULL)
    {
        struct buf * next_buffer = buffer->av_forw;
        int rc;

        if ((buffer->b_flags & B_PFPROT) != 0)
        {
            rc = 0;
        }
        else if (retained_reference_count == USFS_PAGER_BUFFER_COUNT)
        {
            rc = EIO;
        }
        else
        {
            rc = transfer_pager_buffer_in_thread_context (buffer, &references[retained_reference_count]);

            ++retained_reference_count;
        }

        complete_pager_buffer (buffer, rc);
        buffer = next_buffer;
    }

    release_retained_references (references, retained_reference_count);
}

static void initialize_pager_info (struct thrpginfo * info)
{
    memset (info, 0, sizeof (*info));
    info->mysize = sizeof (*info);
    info->bufstsz = sizeof (bufthrio_t);
    info->numbufs = USFS_PAGER_BUFFER_COUNT;
    info->numwresvd = USFS_PAGER_RESERVED_WRITES;
}

int usfs_pager_register (void)
{
    struct thrpginfo info;

    initialize_pager_info (&info);

    return vm_mounte (D_REMOTE | D_THRPGIO | D_ENHANCEDIO, (dev_t)usfs_pager_strategy, &info);
}

int usfs_pager_unregister (void)
{
    return vm_umount (D_REMOTE, (dev_t)usfs_pager_strategy);
}

#endif
