// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_CACHE_H
#define USFS_FS_CACHE_H

static void record_writeback_error (struct usfs_node * node, const int error)
{
    if (error == 0)
        return;

    write_synchronized_with (global_lock)
    {
        if (node->writeback_error == 0)
            node->writeback_error = error;
    }
}

static int get_writeback_error (const struct usfs_node * node)
{
    int error = 0;

    read_synchronized_with (global_lock)
    {
        error = node->writeback_error;
    }

    return error;
}

static uint64_t get_cache_size (const struct usfs_node * node)
{
    uint64_t size = 0;

    read_synchronized_with (global_lock)
    {
        size = node->cache_size;
    }

    return size;
}

static int is_handle_usable_for_cache (const struct usfs_open_state * open_state, const int required_access)
{
    if ((open_state->flags & required_access) == 0)
        return false;

    if (open_state->active)
        return true;

    if (open_state->read_mappings != 0)
        return true;

    if (open_state->write_mappings != 0)
        return true;

    return open_state->cache_refs != 0;
}

/* Caller holds global_lock while selecting an existing handle. */
static struct usfs_open_state * find_usable_handle (
    const struct usfs_node * node,
    const int required_access,
    struct usfs_open_state * preferred_handle
)
{
    if (preferred_handle != NULL)
    {
        if ((preferred_handle->flags & required_access) != 0)
            return preferred_handle;
    }

    for (struct usfs_open_state * open_state = node->opens; open_state != NULL; open_state = open_state->next)
    {
        if (is_handle_usable_for_cache (open_state, required_access))
            return open_state;
    }

    return NULL;
}

/* Caller holds global_lock through selection and reference publication. */
static int select_and_retain_cache_handle (
    const struct usfs_node * node,
    struct usfs_open_state ** cache_handle,
    const int required_access,
    struct usfs_open_state * preferred_handle
)
{
    if (*cache_handle != NULL)
        return 0;

    struct usfs_open_state * open_state = find_usable_handle (node, required_access, preferred_handle);
    if (open_state == NULL)
        return EBADF;

    if (open_state->refs == UINT32_MAX)
        return EOVERFLOW;

    if (open_state->cache_refs == UINT32_MAX)
        return EOVERFLOW;

    ++open_state->refs;
    ++open_state->cache_refs;
    *cache_handle = open_state;

    return 0;
}

/* Cache references own backend handles, but do not keep an otherwise unused
 * vnode alive. Reclamation drains VMM I/O before dropping these references. */
static int ensure_cache_handle (struct usfs_node * node, const int is_writing, struct usfs_open_state * preferred_handle)
{
    struct usfs_open_state ** cache_handle = is_writing ? &node->cache_writer : &node->cache_reader;
    const int required_access = is_writing ? FWRITE : FREAD;
    int rc = 0;

    write_synchronized_with (global_lock)
    {
        rc = select_and_retain_cache_handle (node, cache_handle, required_access, preferred_handle);
    }

    return rc;
}

static int borrow_cache_handle (struct usfs_node * node, const int is_writing, struct usfs_open_state ** borrowed_handle)
{
    int rc = EBADF;

    *borrowed_handle = NULL;

    write_synchronized_with (global_lock)
    {
        struct usfs_open_state * cache_handle = is_writing ? node->cache_writer : node->cache_reader;
        if (cache_handle != NULL)
            rc = usfs_retain_operation_locked (node, cache_handle, borrowed_handle);
    }

    return rc;
}

static uint64_t page_count_for_bytes (const uint64_t byte_count)
{
    return byte_count / PAGESIZE + (byte_count % PAGESIZE != 0);
}

struct usfs_cache_flush_range
{
    uint64_t offset;     // First byte covered by the request.
    uint64_t end;        // Exclusive end byte covered by the request.
    uint64_t first_page; // First page passed to the VMM.
    uint64_t page_count; // Number of pages passed to the VMM.
};

static uint64_t writeback_end_offset (const uint64_t file_size, const int range_limited, const uint64_t range_offset, const uint64_t range_length)
{
    if (!range_limited)
        return file_size;

    if (range_length == 0)
        return file_size;

    if (range_offset > UINT64_MAX - range_length)
        return file_size;

    const uint64_t range_end = range_offset + range_length;
    if (range_end < file_size)
        return range_end;

    return file_size;
}

static struct usfs_cache_flush_range calculate_writeback_range (
    const uint64_t file_size,
    const int range_limited,
    const uint64_t range_offset,
    const uint64_t range_length
)
{
    struct usfs_cache_flush_range flush_range = { 0 };

    flush_range.end = writeback_end_offset (file_size, range_limited, range_offset, range_length);

    if (range_limited && range_length != 0)
        flush_range.offset = range_offset;

    flush_range.first_page = flush_range.offset / PAGESIZE;
    if (flush_range.end > flush_range.offset)
        flush_range.page_count = page_count_for_bytes (flush_range.end) - flush_range.first_page;

    return flush_range;
}

static struct usfs_cache_flush_range calculate_discard_range (struct usfs_cache_flush_range flush_range)
{
    flush_range.first_page = page_count_for_bytes (flush_range.offset);

    const uint64_t first_excluded_page = flush_range.end / PAGESIZE;
    flush_range.page_count = 0;
    if (first_excluded_page > flush_range.first_page)
        flush_range.page_count = first_excluded_page - flush_range.first_page;

    return flush_range;
}

static int write_cached_pages (struct usfs_node * node, const vmid_t segment, const struct usfs_cache_flush_range flush_range)
{
    int write_error = 0;

    if (flush_range.page_count != 0)
        write_error = vm_writep (segment, (vpn_t)flush_range.first_page, (vpn_t)flush_range.page_count);

    const int wait_error = vms_iowaitf (segment, V_WAITALL);
    record_writeback_error (node, write_error);
    record_writeback_error (node, wait_error);

    return get_writeback_error (node);
}

static int evict_cached_pages (struct usfs_node * node, const vmid_t segment, const struct usfs_cache_flush_range flush_range)
{
    int rc = usfs_checkpoint (USFS_INSTRUMENT_CACHE_BEFORE_EVICT);
    if (rc != 0)
        return rc;

    /* Stores can dirty these pages after the first completion wait.
     * Flush releases frames only after writing their current contents. */
    rc = vm_flushp (segment, (vpn_t)flush_range.first_page, (vpn_t)flush_range.page_count);
    const int wait_error = vms_iowaitf (segment, V_WAITALL);

    record_writeback_error (node, rc);
    record_writeback_error (node, wait_error);
    (void)usfs_checkpoint (USFS_INSTRUMENT_CACHE_EVICT_DONE);

    return get_writeback_error (node);
}

static int flush_cached_pages (
    struct vnode * file_vnode,
    const int range_limited,
    const uint64_t range_offset,
    const uint64_t range_length,
    const int discard_pages
)
{
    struct usfs_node * node = _node_of (file_vnode);
    vmid_t segment = 0;
    uint64_t file_size;
    int writeback_error = 0;

    read_synchronized_with (global_lock)
    {
        segment = file_vnode->v_gnode->gn_seg;
        file_size = node->cache_size;
        writeback_error = node->writeback_error;
    }

    if (segment == 0)
        return writeback_error;

    struct usfs_cache_flush_range flush_range = calculate_writeback_range (file_size, range_limited, range_offset, range_length);
    writeback_error = write_cached_pages (node, segment, flush_range);
    if (writeback_error != 0)
        return writeback_error;

    if (!discard_pages)
        return 0;

    if (flush_range.end <= flush_range.offset)
        return 0;

    flush_range = calculate_discard_range (flush_range);
    if (flush_range.page_count == 0)
        return 0;

    return evict_cached_pages (node, segment, flush_range);
}

static int zero_partial_page_tail (const struct vnode * file_vnode, const uint64_t first_byte, const uint64_t end_byte)
{
    static const char zero_bytes[PAGESIZE];

    if (end_byte <= first_byte)
        return 0;

    const uint64_t byte_count = end_byte - first_byte;
    struct iovec vector = { 0 };
    struct uio user_io_request = { 0 };

    vector.iov_base = (caddr_t)zero_bytes;
    vector.iov_len = (size_t)byte_count;
    user_io_request.uio_iov = &vector;
    user_io_request.uio_iovcnt = 1;
    user_io_request.uio_offset = (offset_t)first_byte;
    user_io_request.uio_resid = (int32long64_t)byte_count;
    user_io_request.uio_segflg = UIO_SYSSPACE;

    return vm_uiomove (file_vnode->v_gnode->gn_seg, (vmsize_t)byte_count, UIO_WRITE, &user_io_request);
}

static int has_cached_write_handle (const struct usfs_node * node)
{
    int has_cache_writer = false;

    read_synchronized_with (global_lock)
    {
        has_cache_writer = node->cache_writer != NULL;
    }

    return has_cache_writer;
}

static int adjust_segment_after_shrink (
    const struct vnode * file_vnode,
    const uint64_t previous_size,
    const uint64_t file_size,
    const int has_cache_writer
)
{
    const uint64_t first_removed_page = page_count_for_bytes (file_size);
    const uint64_t removed_page_count = page_count_for_bytes (previous_size) - first_removed_page;

    if (removed_page_count != 0)
    {
        const int rc = vm_invalidatep (file_vnode->v_gnode->gn_seg, (vpn_t)first_removed_page, removed_page_count);
        if (rc != 0)
            return rc;
    }

    if (file_size % PAGESIZE == 0)
        return 0;

    if (has_cache_writer)
        return zero_partial_page_tail (file_vnode, file_size, page_count_for_bytes (file_size) * PAGESIZE);

    return vm_invalidatep (file_vnode->v_gnode->gn_seg, (vpn_t)(file_size / PAGESIZE), 1);
}

static int adjust_segment_after_growth (
    const struct vnode * file_vnode,
    const uint64_t previous_size,
    const uint64_t file_size,
    const int has_cache_writer
)
{
    if (previous_size % PAGESIZE == 0)
        return 0;

    uint64_t partial_page_end = page_count_for_bytes (previous_size) * PAGESIZE;
    if (partial_page_end > file_size)
        partial_page_end = file_size;

    if (has_cache_writer)
        return zero_partial_page_tail (file_vnode, previous_size, partial_page_end);

    return vm_invalidatep (file_vnode->v_gnode->gn_seg, (vpn_t)(previous_size / PAGESIZE), 1);
}

static int adjust_segment_after_resize (
    const struct vnode * file_vnode,
    const uint64_t previous_size,
    const uint64_t file_size,
    const int has_cache_writer
)
{
    if (file_size < previous_size)
        return adjust_segment_after_shrink (file_vnode, previous_size, file_size, has_cache_writer);

    if (file_size > previous_size)
        return adjust_segment_after_growth (file_vnode, previous_size, file_size, has_cache_writer);

    return 0;
}

static int resize_cached_file (
    struct vnode * file_vnode,
    const struct usfs_setattr_in * resize_request,
    const uint64_t file_handle,
    struct ucred * credentials
)
{
    struct usfs_node * node = _node_of (file_vnode);
    const uint64_t previous_size = get_cache_size (node);
    const uint64_t file_size = resize_request->size;
    const int has_cache_writer = has_cached_write_handle (node);

    int rc = 0;

    write_synchronized_with (node->pageout_lock)
    {
        rc = _setattr_unlocked (file_vnode, resize_request, file_handle, credentials);
        if (rc != 0)
            return rc;

        write_synchronized_with (global_lock)
        {
            node->cache_size = file_size;
            if (file_size > node->cache_extent)
                node->cache_extent = file_size;
        }
    }

    rc = adjust_segment_after_resize (file_vnode, previous_size, file_size, has_cache_writer);
    if (rc == 0)
        return 0;

    record_writeback_error (node, rc);
    abort_connection (_mount_of (file_vnode)->conn);

    return rc;
}

static int is_mount_connection_active (struct vnode * file_vnode)
{
    const struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    if (mount_data == NULL)
        return false;

    if (mount_data->conn == NULL)
        return false;

    synchronized_with (mount_data->conn->lock)
    {
        return mount_data->conn->state == USFS_CONN_ACTIVE;
    }

    return false;
}

struct usfs_cache_io_context
{
    struct vnode * file_vnode;    // Vnode whose segment supplies data.
    struct usfs_node * node;      // Cache metadata for the vnode.
    uint64_t file_handle;         // Backend handle used for resizing.
    enum uio_rw direction;        // Direction of the requested transfer.
    int32long64_t flags;          // AIX operation flags.
    struct uio * user_io_request; // Mutable AIX transfer request.
    struct ucred * credentials;   // Credentials for backend operations.
};

struct usfs_cache_chunk_result
{
    uint32_t requested_size; // Bytes requested from the VMM.
    uint64_t moved_size;     // Bytes consumed by the VMM.
    int reached_eof;         // Read stopped at the cached file size.
};

static int prepare_cached_write (const struct usfs_cache_io_context * io_context)
{
    const int rc = ensure_cache_handle (io_context->node, true, NULL);
    if (rc != 0)
        return rc;

    if (((io_context->flags | io_context->user_io_request->uio_fmode) & FAPPEND) != 0)
        io_context->user_io_request->uio_offset = (offset_t)get_cache_size (io_context->node);

    return 0;
}

static int prepare_transfer_chunk (
    const struct usfs_cache_io_context * io_context,
    const uint64_t previous_size,
    const offset_t chunk_offset,
    struct usfs_cache_chunk_result * chunk_result
)
{
    chunk_result->requested_size = usfs_rdwr_chunk_size (io_context->user_io_request);

    if (!usfs_file_offset_count_valid ((int64_t)chunk_offset, chunk_result->requested_size))
        return EFBIG;

    if (io_context->direction == UIO_READ)
    {
        if ((uint64_t)chunk_offset >= previous_size)
        {
            chunk_result->reached_eof = true;

            return 0;
        }

        const uint64_t available_size = previous_size - (uint64_t)chunk_offset;
        if (chunk_result->requested_size > available_size)
            chunk_result->requested_size = (uint32_t)available_size;

        return 0;
    }

    const uint64_t requested_end = (uint64_t)chunk_offset + chunk_result->requested_size;
    if (requested_end <= previous_size)
        return 0;

    struct usfs_setattr_in growth_request;

    memset (&growth_request, 0, sizeof (growth_request));
    growth_request.valid = USFS_SET_SIZE;
    growth_request.size = requested_end;

    return resize_cached_file (io_context->file_vnode, &growth_request, io_context->file_handle, io_context->credentials);
}

static int rollback_file_growth (
    const struct usfs_cache_io_context * io_context,
    const uint64_t previous_size,
    const offset_t chunk_offset,
    const uint64_t moved_size,
    const int transfer_error
)
{
    struct usfs_setattr_in rollback_request;

    memset (&rollback_request, 0, sizeof (rollback_request));
    rollback_request.valid = USFS_SET_SIZE;
    rollback_request.size = previous_size;

    if (moved_size != 0)
    {
        if ((uint64_t)chunk_offset + moved_size > previous_size)
            rollback_request.size = (uint64_t)chunk_offset + moved_size;
    }

    const int rollback_error = resize_cached_file (io_context->file_vnode, &rollback_request, io_context->file_handle, io_context->credentials);
    if (rollback_error == 0)
        return transfer_error;

    record_writeback_error (io_context->node, rollback_error);
    abort_connection (_mount_of (io_context->file_vnode)->conn);

    return transfer_error == 0 ? rollback_error : transfer_error;
}

static int transfer_cached_chunk (const struct usfs_cache_io_context * io_context, struct usfs_cache_chunk_result * chunk_result)
{
    if (!is_mount_connection_active (io_context->file_vnode))
        return EIO;

    const uint64_t previous_size = get_cache_size (io_context->node);
    const offset_t chunk_offset = io_context->user_io_request->uio_offset;
    int rc = prepare_transfer_chunk (io_context, previous_size, chunk_offset, chunk_result);
    if (rc != 0)
        return rc;

    if (chunk_result->reached_eof)
        return 0;

    const int32long64_t previous_residual = io_context->user_io_request->uio_resid;
    rc = vm_uiomove (
        io_context->file_vnode->v_gnode->gn_seg,
        (vmsize_t)chunk_result->requested_size,
        io_context->direction,
        io_context->user_io_request
    );
    chunk_result->moved_size = (uint64_t)(previous_residual - io_context->user_io_request->uio_resid);

    if (io_context->direction != UIO_WRITE)
        return rc;

    if (chunk_result->moved_size >= chunk_result->requested_size)
        return rc;

    if ((uint64_t)chunk_offset + chunk_result->requested_size <= previous_size)
        return rc;

    return rollback_file_growth (io_context, previous_size, chunk_offset, chunk_result->moved_size, rc);
}

static int finish_cached_write (const struct usfs_cache_io_context * io_context, const int32long64_t original_residual, int rc)
{
    if (io_context->direction != UIO_WRITE)
        return rc;

    if (io_context->user_io_request->uio_resid == original_residual)
        return rc;

    const int checkpoint_error = usfs_checkpoint (USFS_INSTRUMENT_CACHE_AFTER_WRITE);

    if (rc == 0)
        rc = checkpoint_error;

    const int flush_error = flush_cached_pages (io_context->file_vnode, false, 0, 0, false);

    if (rc == 0)
        rc = flush_error;

    if (rc != 0)
        return rc;

    if (((io_context->flags | io_context->user_io_request->uio_fmode) & (FSYNC | FDATASYNC)) == 0)
        return 0;

    return _sync_node (
        io_context->file_vnode,
        io_context->file_handle,
        io_context->flags | io_context->user_io_request->uio_fmode,
        0,
        0,
        0,
        io_context->credentials
    );
}

static int transfer_cached_chunks (const struct usfs_cache_io_context * io_context)
{
    while (io_context->user_io_request->uio_resid > 0)
    {
        struct usfs_cache_chunk_result chunk_result = { 0 };

        const int rc = transfer_cached_chunk (io_context, &chunk_result);
        if (rc != 0)
            return rc;

        if (chunk_result.reached_eof)
            return 0;

        if (chunk_result.moved_size < chunk_result.requested_size)
            return 0;
    }

    return 0;
}

static int transfer_cached_data (
    struct vnode * file_vnode,
    const uint64_t file_handle,
    const enum uio_rw direction,
    const int32long64_t flags,
    struct uio * user_io_request,
    struct ucred * credentials
)
{
    const struct usfs_cache_io_context io_context = {
        .file_vnode = file_vnode,
        .node = _node_of (file_vnode),
        .file_handle = file_handle,
        .direction = direction,
        .flags = flags,
        .user_io_request = user_io_request,
        .credentials = credentials,
    };

    const int32long64_t original_residual = user_io_request->uio_resid;
    const offset_t original_offset = user_io_request->uio_offset;

    if (!is_mount_connection_active (file_vnode))
        return EIO;

    if (original_residual == 0)
        return 0;

    int rc = 0;

    if (direction == UIO_WRITE)
    {
        rc = prepare_cached_write (&io_context);
        if (rc != 0)
            return rc;
    }

    rc = transfer_cached_chunks (&io_context);
    rc = finish_cached_write (&io_context, original_residual, rc);

    if (user_io_request->uio_resid == original_residual)
        user_io_request->uio_offset = original_offset;

    return rc;
}

static void delete_cache_segment (const struct usfs_node * node, const struct usfs_mount_data * mount_data)
{
    if (node->gn->gn_seg == 0)
        return;

    const vmid_t segment = node->gn->gn_seg;
    node->gn->gn_seg = 0;

    if (vms_delete (segment) != 0)
        abort_connection (mount_data->conn);
}

static struct usfs_open_state * detach_cache_handle (struct usfs_node * node, struct usfs_open_state ** cache_handle)
{
    struct usfs_open_state * open_state = *cache_handle;
    *cache_handle = NULL;

    if (open_state == NULL)
        return NULL;

    --open_state->cache_refs;
    --open_state->refs;
    if (open_state->refs != 0)
        return NULL;

    struct usfs_open_state ** open_state_link = usfs_open_state_link_locked (node, open_state);
    *open_state_link = open_state->next;

    return open_state;
}

static void dispose_cache_handle (struct usfs_node * node, struct usfs_mount_data * mount_data, struct usfs_open_state ** cache_handle)
{
    struct usfs_open_state * released_handle = NULL;

    write_synchronized_with (global_lock)
    {
        released_handle = detach_cache_handle (node, cache_handle);
    }

    if (released_handle == NULL)
        return;

    _send_release (mount_data, node->nodeid, released_handle->fh, released_handle->flags, 0, NULL);
    usfs_open_state_discard (released_handle);
}

static void dispose_cache_resources (struct usfs_node * node, struct usfs_mount_data * mount_data)
{
    delete_cache_segment (node, mount_data);
    dispose_cache_handle (node, mount_data, &node->cache_reader);
    dispose_cache_handle (node, mount_data, &node->cache_writer);
}

static int is_next_sync_candidate (
    const struct usfs_node * node,
    const uint64_t cursor_node_id,
    const uint64_t maximum_node_id,
    const struct usfs_node * selected_node
)
{
    if (node->nodeid <= cursor_node_id)
        return false;

    if (node->nodeid > maximum_node_id)
        return false;

    if (selected_node != NULL)
    {
        if (node->nodeid >= selected_node->nodeid)
            return false;
    }

    return true;
}

static struct vnode * select_and_hold_next_sync_vnode (
    const struct usfs_connection * connection,
    const uint64_t cursor_node_id,
    const uint64_t maximum_node_id,
    uint64_t * selected_node_id
)
{
    const struct usfs_mount_data * mount_data = connection->mounted_data;
    if (mount_data == NULL)
        return NULL;

    const struct usfs_node * selected_node = NULL;

    for (struct usfs_node * node = mount_data->nodes; node != NULL; node = node->next)
    {
        if (is_next_sync_candidate (node, cursor_node_id, maximum_node_id, selected_node))
            selected_node = node;
    }

    if (selected_node == NULL)
        return NULL;

    *selected_node_id = selected_node->nodeid;

    usfs_vnode_hold_locked (selected_node->vn, USFS_INSTRUMENT_NODE_REUSE_NONE);

    return selected_node->vn;
}

static uint64_t find_maximum_node_id (const struct usfs_connection * connection)
{
    uint64_t maximum_node_id = 0;

    read_synchronized_with (global_lock)
    {
        const struct usfs_mount_data * mount_data = connection->mounted_data;

        if (mount_data != NULL)
        {
            for (const struct usfs_node * node = mount_data->nodes; node != NULL; node = node->next)
            {
                if (node->nodeid > maximum_node_id)
                    maximum_node_id = node->nodeid;
            }
        }
    }

    return maximum_node_id;
}

static struct vnode * hold_next_sync_vnode (
    const struct usfs_connection * connection,
    const uint64_t cursor_node_id,
    const uint64_t maximum_node_id,
    uint64_t * selected_node_id
)
{
    struct vnode * file_vnode = NULL;

    write_synchronized_with (global_lock)
    {
        file_vnode = select_and_hold_next_sync_vnode (connection, cursor_node_id, maximum_node_id, selected_node_id);
    }

    return file_vnode;
}

static int flush_connection_cache (const struct usfs_connection * connection)
{
    if (connection == NULL)
        return EIO;

    const uint64_t maximum_node_id = find_maximum_node_id (connection);
    uint64_t cursor_node_id = 0;
    int first_error = 0;

    while (cursor_node_id < maximum_node_id)
    {
        uint64_t selected_node_id = UINT64_MAX;
        struct vnode * file_vnode = hold_next_sync_vnode (connection, cursor_node_id, maximum_node_id, &selected_node_id);
        if (file_vnode == NULL)
            break;

        const int rc = flush_cached_pages (file_vnode, false, 0, 0, false);
        if (first_error == 0)
            first_error = rc;

        VNOP_RELE (file_vnode);
        cursor_node_id = selected_node_id;
    }

    return first_error;
}

#endif
