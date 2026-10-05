// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_READDIR_H
#define USFS_FS_OPERATIONS_VNODE_READDIR_H

struct directory_entry_source
{
    const char * bytes;       // Start of the next serialized directory entry.
    uint32_t remaining_bytes; // Serialized bytes available from bytes.
    uint64_t entry_index;     // Logical directory index assigned to the native entry.
    uint64_t snapshot_id;     // Snapshot identity carried in native directory offsets.
};

struct prepared_directory_entry
{
    struct dirent entry;           // Native AIX directory entry ready for uiomove.
    uint32_t output_record_length; // Aligned number of native bytes to copy.
    uint32_t source_record_length; // Number of serialized bytes consumed.
};

struct readdir_request_context
{
    struct usfs_mount_data * mount_data; // Mounted instance used for protocol traffic.
    struct usfs_node * node;             // Directory node being enumerated.
    uint64_t file_handle;                // Daemon directory handle, or zero when absent.
    uint64_t cursor;                     // Opaque directory cursor requested.
    uint32_t maximum_payload_size;       // Maximum serialized entry bytes accepted.
    struct ucred * credentials;          // Credentials attached to the request.
};

static int prepare_directory_entry (const struct directory_entry_source * source, struct prepared_directory_entry * prepared_entry)
{
    const uint32_t dirent_header = (uint32_t)((size_t) & (((struct dirent *)0)->d_name[0]));
    struct usfs_dirent entry;

    if (source->remaining_bytes < sizeof (entry))
        return 0;

    memcpy (&entry, source->bytes, sizeof (entry));
    if (entry.reclen < sizeof (entry) + entry.namelen + 1 || entry.reclen > source->remaining_bytes)
        return 0;

    memset (&prepared_entry->entry, 0, sizeof (prepared_entry->entry));

    uint32_t name_length = entry.namelen;
    if (name_length > sizeof (prepared_entry->entry.d_name) - 1)
        name_length = (uint32_t)sizeof (prepared_entry->entry.d_name) - 1;

    memcpy (prepared_entry->entry.d_name, source->bytes + sizeof (entry), name_length);
    prepared_entry->entry.d_name[name_length] = '\0';
    prepared_entry->entry.d_ino = (ino_t)entry.ino;
    prepared_entry->entry.d_namlen = (ushort)name_length;
    prepared_entry->entry.d_offset = (offset_t)((source->snapshot_id << USFS_DIRECTORY_CURSOR_INDEX_BITS) | (source->entry_index + 1));
    prepared_entry->output_record_length = (dirent_header + name_length + 1u + 7u) & ~7u;

    if (prepared_entry->output_record_length > sizeof (prepared_entry->entry))
        prepared_entry->output_record_length = (uint32_t)sizeof (prepared_entry->entry);

    prepared_entry->entry.d_reclen = (ushort)prepared_entry->output_record_length;
    prepared_entry->source_record_length = entry.reclen;

    return 1;
}

static int request_directory_entries (const struct readdir_request_context * context, struct usfs_request ** result_request)
{
    struct usfs_readdir_in request_body;
    struct usfs_request * request = NULL;

    memset (&request_body, 0, sizeof (request_body));
    request_body.fh = context->file_handle;
    request_body.cookie = context->cursor;
    request_body.size = context->maximum_payload_size;

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = (uint16_t)USFS_OP_READDIR,
        .node_id = context->node->nodeid,
        .opcode_specific_body = { .bytes = &request_body, .length = sizeof (request_body) },
        .max_allowed_reply_buffer_size = (uint32_t)sizeof (struct usfs_readdir_out) + USFS_MAX_DATA,
        .credentials = context->credentials
    };

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

    if (!usfs_readdir_reply_valid (request->reply_buffer, request->reply_buffer_size))
    {
        free_request (request);
        return EIO;
    }

    *result_request = request;

    return 0;
}

static int copy_directory_entries (
    const struct usfs_request * request,
    const uint32_t count,
    const uint64_t start_index,
    const uint64_t snapshot_id,
    struct uio * user_io_request
)
{
    const char * src = request->reply_buffer + sizeof (struct usfs_readdir_out);
    uint32_t remaining = request->reply_buffer_size - (uint32_t)sizeof (struct usfs_readdir_out);
    uint64_t index = start_index;
    int rc = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        const struct directory_entry_source source = {
            .bytes = src,
            .remaining_bytes = remaining,
            .entry_index = index,
            .snapshot_id = snapshot_id
        };
        struct prepared_directory_entry prepared_entry;

        if (!prepare_directory_entry (&source, &prepared_entry))
            break;

        if ((uint64_t)user_io_request->uio_resid < (uint64_t)prepared_entry.output_record_length)
        {
            if (index == start_index)
                rc = EINVAL;

            break;
        }

        rc = uiomove ((caddr_t)&prepared_entry.entry, (long)prepared_entry.output_record_length, UIO_READ, user_io_request);
        if (rc != 0)
            break;

        index += 1;
        src += prepared_entry.source_record_length;
        remaining -= prepared_entry.source_record_length;
    }

    user_io_request->uio_offset = (offset_t)((snapshot_id << USFS_DIRECTORY_CURSOR_INDEX_BITS) | index);
    return rc;
}

static int borrow_readdir_state (struct usfs_node * node, const struct uio * user_io_request, struct usfs_open_state ** open_state)
{
    int rc = EBADF;

    if (user_io_request->uio_iovcnt > 1 && user_io_request->uio_iov[1].iov_len == 0 && user_io_request->uio_iov[1].iov_base != NULL)
    {
        /* AIX levels expose this zero-length side channel either as f_vinfo
         * itself or as the address of the file field that contains it. */
        caddr_t file_info = (caddr_t)user_io_request->uio_iov[1].iov_base;
        rc = usfs_borrow_description (node, file_info, open_state);

        if (rc == EBADF || rc == EINVAL)
        {
            file_info = *(caddr_t *)user_io_request->uio_iov[1].iov_base;
            rc = usfs_borrow_description (node, file_info, open_state);
        }
    }

    if (rc == EBADF || rc == EINVAL)
    {
        /* Kernel-internal directory walks may not have a file description. */
        rc = usfs_borrow_node_handle (node, 0, open_state);
    }
    return rc;
}

static int read_directory (struct vnode * file_vnode, struct uio * user_io_request, int * end_of_directory, struct ucred * credentials)
{
    if (end_of_directory != NULL)
        *end_of_directory = 0;

    const int access_rc = gn_access (file_vnode, R_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL || node == NULL)
        return EIO;

    if (user_io_request->uio_resid <= 0)
        return EINVAL;

    if (user_io_request->uio_offset < 0)
        return EINVAL;

    const uint64_t directory_cursor = (uint64_t)user_io_request->uio_offset;
    const uint64_t requested_snapshot_id = directory_cursor >> USFS_DIRECTORY_CURSOR_INDEX_BITS;
    const uint64_t start_index = directory_cursor & USFS_DIRECTORY_CURSOR_INDEX_MASK;

    if (directory_cursor != 0 && requested_snapshot_id == 0)
        return EINVAL;

    struct usfs_open_state * open_state = NULL;
    const int handle_rc = borrow_readdir_state (node, user_io_request, &open_state);
    if (handle_rc != 0 && handle_rc != EBADF)
        return handle_rc;

    /* Wire and native records have different sizes. Fetch a bounded window
     * independently of the native budget; copy only complete native entries. */
    const uint32_t max_payload = USFS_MAX_DATA;

    const struct readdir_request_context request_context = {
        .mount_data = mount_data,
        .node = node,
        .file_handle = open_state == NULL ? 0 : open_state->fh,
        .cursor = directory_cursor,
        .maximum_payload_size = max_payload,
        .credentials = credentials
    };

    struct usfs_request * request = NULL;
    int rc = request_directory_entries (&request_context, &request);

    if (rc != 0)
    {
        usfs_put_open_reference (file_vnode, open_state, 1, credentials);
        return rc;
    }

    struct usfs_readdir_out readdir_reply;
    memcpy (&readdir_reply, request->reply_buffer, sizeof (readdir_reply));

    if ((directory_cursor != 0 && readdir_reply.snapshot_id != requested_snapshot_id) ||
        (uint64_t)readdir_reply.count > USFS_DIRECTORY_CURSOR_INDEX_MASK - start_index)
    {
        free_request (request);
        usfs_put_open_reference (file_vnode, open_state, 1, credentials);
        return EIO;
    }

    if (end_of_directory != NULL)
    {
        *end_of_directory = readdir_reply.count == 0;
    }

    rc = copy_directory_entries (request, readdir_reply.count, start_index, readdir_reply.snapshot_id, user_io_request);
    free_request (request);
    usfs_put_open_reference (file_vnode, open_state, 1, credentials);

    return rc;
}

int gn_readdir (struct vnode * file_vnode, struct uio * user_io_request, struct ucred * credentials)
{
    return read_directory (file_vnode, user_io_request, NULL, credentials);
}

#endif
