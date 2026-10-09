// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_COMMON_H
#define USFS_FS_COMMON_H

static int flush_connection_cache (const struct usfs_connection *);
static int flush_cached_pages (struct vnode *, int, uint64_t, uint64_t, int);
static int resize_cached_file (struct vnode *, const struct usfs_setattr_in *, uint64_t, struct ucred *);
static void dispose_cache_resources (struct usfs_node *, struct usfs_mount_data *);


static uint32_t gnode_lock_occurrences[USFS_LOCK_OCCURRENCE_WORDS];
static uint32_t vnode_lock_occurrences[USFS_LOCK_OCCURRENCE_WORDS];

/**
 * Allocates and initializes a gnode structure.
 *
 * Parameters:
 *   allocated_gnode - Points to storage that receives the address of the newly allocated and
 *            initialized gnode on success. Left unmodified if the function fails.
 *
 * Description:
 *   Allocates a struct gnode from the kernel heap and zero-fills it. Sets the gnode's gn_ops
 *   field to the driver's global gn_ops file operations table. Assigns the gnode's reclaim
 *   lock (gn_reclk_lock) a free occurrence identifier from the gnode bitmap, allocates the
 *   lock with lock_alloc, and initializes it with simple_lock_init. If the kernel heap cannot
 *   satisfy the allocation, the function fails with ENOMEM. If every occurrence identifier
 *   is in use, the function fails with EAGAIN without allocating a lock; the caller may retry
 *   later. On success, the addresses and occurrence identifier are returned to the caller.
 *
 * Return values:
 *   0 - Indicates success.
 *   Nonzero return values are returned from the /usr/include/sys/errno.h file to indicate failure.
 */
static int allocate_gnode (struct gnode ** allocated_gnode, short * occurrence_out)
{
    static const short gn_reclk_lock_class = SHRT_MAX - 1;
    static const int allocation_alignment = 4;

    struct gnode * gnode = usfs_kmalloc (USFS_ALLOC_GNODE, sizeof (struct gnode), allocation_alignment, kernel_heap);

    if (gnode == NULL)
        return ENOMEM;

    memset (gnode, 0, sizeof (struct gnode));

    // Initialize file operations
    gnode->gn_ops = &gn_ops;

    short occurrence = -1;
    if (usfs_allocate_lock_occurrence (gnode_lock_occurrences, &occurrence) != 0)
    {
        xmfree (gnode, kernel_heap);
        return EAGAIN;
    }

    lock_alloc (&gnode->gn_reclk_lock, LOCK_ALLOC_PAGED, gn_reclk_lock_class, occurrence);
    simple_lock_init (&gnode->gn_reclk_lock);

    *allocated_gnode = gnode;
    *occurrence_out = occurrence;

    return 0;
}

/**
 * Releases a gnode structure and its associated reclaim lock.
 *
 * Parameters:
 *   gnode - Points to the gnode to release. Must have been allocated by allocate_gnode;
 *        the caller is responsible for ensuring nothing else still references it.
 *
 * Description:
 *   Frees the gnode's reclaim lock (gn_reclk_lock) with lock_free, returns its occurrence
 *   identifier to the gnode bitmap, and releases the gnode's storage back to the kernel heap
 *   with xmfree. If
 *   xmfree reports a nonzero return code, that indicates a logical error in the caller
 *   (e.g. freeing storage that was not allocated as expected); the current
 *   implementation does not act on that condition.
 */
static void _free_gnode (struct gnode * gnode, const short occurrence)
{
    lock_free (&gnode->gn_reclk_lock);

    usfs_free_lock_occurrence (gnode_lock_occurrences, occurrence);

    const int rc = xmfree (gnode, kernel_heap);
    if (rc != 0)
    {
        // should never happen; indicates logical error in the code
        // brkpoint(0);
    }
}

/**
 * Allocates and initializes a vnode structure for a given gnode.
 *
 * Parameters:
 *   filesystem   - Points to the vfs structure the new vnode is to be associated with.
 *   gnode     - Points to the gnode the new vnode represents.
 *   allocated_vnode - Points to storage that receives the address of the newly allocated and
 *            initialized vnode on success. Left unmodified if the function fails.
 *
 * Description:
 *   Obtains a struct vnode for the given vfs/gnode pair by calling vn_get. If vn_get
 *   fails, its return code is propagated to the caller and no lock is allocated. On
 *   success, assigns the vnode's lock (v_lock) a free occurrence identifier from the vnode
 *   bitmap, allocates the lock with lock_alloc, and initializes it with simple_lock_init. If
 *   every occurrence identifier is in use, the function fails with EAGAIN without allocating
 *   a lock; the caller may retry later. On success, the vnode and occurrence identifier are
 *   returned to the caller.
 *
 * Return values:
 *   0 - Indicates success.
 *   Nonzero return values are returned from the /usr/include/sys/errno.h file to indicate failure.
 */
static int allocate_vnode (struct vfs * filesystem, struct gnode * gnode, struct vnode ** allocated_vnode, short * occurrence_out)
{
    static const short vn_lock_class = SHRT_MAX - 2;

    struct vnode * file_vnode = NULL;
    const int rc = vn_get (filesystem, gnode, &file_vnode);
    if (rc != 0)
        return rc;

    short occurrence = -1;
    if (usfs_allocate_lock_occurrence (vnode_lock_occurrences, &occurrence) != 0)
    {
        vn_free (file_vnode);
        return EAGAIN;
    }

    lock_alloc (&file_vnode->v_lock, LOCK_ALLOC_PAGED, vn_lock_class, occurrence);
    simple_lock_init (&file_vnode->v_lock);

    *allocated_vnode = file_vnode;
    *occurrence_out = occurrence;

    return 0;
}

/**
 * Releases a vnode structure and its associated lock.
 *
 * Parameters:
 *   file_vnode - Points to the vnode to release. Must have been allocated by allocate_vnode;
 *        the caller is responsible for ensuring nothing else still references it.
 *
 * Description:
 *   Frees the vnode's lock (v_lock) with lock_free, returns its occurrence identifier to the
 *   vnode bitmap, and releases the vnode itself back to the kernel with vn_free.
 */
static void _free_vnode (struct vnode * file_vnode, const short occurrence)
{
    lock_free (&file_vnode->v_lock);

    usfs_free_lock_occurrence (vnode_lock_occurrences, occurrence);

    vn_free (file_vnode);
}

/* The caller owns node and releases it if either allocation fails. */
static int allocate_node_vnodes (struct vfs * filesystem, const int vnode_type, const int is_root, struct usfs_node * node)
{
    struct gnode * gnode = NULL;
    int rc = allocate_gnode (&gnode, &node->gn_lock_occurrence);
    if (rc != 0)
        return rc;

    gnode->gn_type = vnode_type;
    gnode->gn_data = (caddr_t)node;

    struct vnode * file_vnode = NULL;

    rc = allocate_vnode (filesystem, gnode, &file_vnode, &node->vn_lock_occurrence);
    if (rc != 0)
    {
        _free_gnode (gnode, node->gn_lock_occurrence);

        return rc;
    }

    if (is_root)
        file_vnode->v_flag |= V_ROOT;

    node->gn = gnode;
    node->vn = file_vnode;

    return 0;
}

static void initialize_node_locks (struct usfs_node * node)
{
    static const short mutation_lock_class = SHRT_MAX - 9;
    static const short pageout_lock_class = SHRT_MAX - 11;

    /* Separate families, same unique occurrence as this node's gnode. AIX
       lock_alloc/lock_init permit pageable storage in process context. */
    lock_alloc (&node->mutation_lock, LOCK_ALLOC_PAGED, mutation_lock_class, node->gn_lock_occurrence);
    lock_init (&node->mutation_lock, FALSE);
    lock_alloc (&node->pageout_lock, LOCK_ALLOC_PAGED, pageout_lock_class, node->gn_lock_occurrence);
    lock_init (&node->pageout_lock, FALSE);
}

/**
 * Allocates a usfs_node together with its gnode/vnode pair without publishing it.
 *
 * Parameters:
 *   filesystem     - Points to the vfs the node belongs to.
 *   vnode_type    - The vnode type (VDIR, VREG, ...) stored in the gnode's gn_type field.
 *   is_root  - Nonzero to mark the vnode with V_ROOT.
 *   node_out - Receives the new node on success.
 *
 * Description:
 *   Allocates the private node structure, a gnode (via allocate_gnode, which wires the
 *   driver's vnode operations) and a vnode (via allocate_vnode / vn_get, which links the
 *   vnode into the vfs and sets its use count to 1). The gnode's gn_data points back at the
 *   node so vnode operations can recover it. The caller must hold global_lock: allocation
 *   may sleep, which the complex lock permits, and lock-occurrence bitmaps are shared.
 *   On any failure everything allocated so far is rolled back and nothing is linked.
 *
 * Return values:
 *   0 - Indicates success.
 *   Nonzero return values are returned from the /usr/include/sys/errno.h file to indicate failure.
 */
static int _allocate_node (struct vfs * filesystem, const int vnode_type, const int is_root, struct usfs_node ** node_out)
{
    static const int memory_alignment_in_bytes = 4;

    struct usfs_node * node = usfs_kmalloc (USFS_ALLOC_NODE, sizeof (struct usfs_node), memory_alignment_in_bytes, kernel_heap);
    if (node == NULL)
        return ENOMEM;

    memset (node, 0, sizeof (struct usfs_node));
    node->gn_lock_occurrence = -1;
    node->vn_lock_occurrence = -1;

    const int rc = allocate_node_vnodes (filesystem, vnode_type, is_root, node);
    if (rc != 0)
    {
        xmfree (node, kernel_heap);

        return rc;
    }

    initialize_node_locks (node);
    *node_out = node;

    return 0;
}

/* Discards a node that has not been linked into a mount. */
static void _discard_unpublished_node (struct usfs_node * node)
{
    if (node == NULL)
        return;

    lock_free (&node->pageout_lock);
    lock_free (&node->mutation_lock);
    _free_vnode (node->vn, node->vn_lock_occurrence);
    _free_gnode (node->gn, node->gn_lock_occurrence);
    xmfree (node, kernel_heap);
}

/* Publishes a completely allocated node. No operation in this commit step can
 * fail, so callers may safely perform remote mutation only after allocation. */
static void _publish_node (struct usfs_mount_data * mount_data, struct usfs_node * node, const uint64_t nodeid, const uint64_t parent)
{
    node->nodeid = nodeid;
    node->parent = parent;
    node->next = mount_data->nodes;
    mount_data->nodes = node;
    usfs_instrumentation_node_created (mount_data, nodeid);
}

/** Allocates and publishes a node for lookup/root paths. */
static int _create_node (
    struct vfs * filesystem,
    struct usfs_mount_data * mount_data,
    const uint64_t nodeid,
    const uint64_t parent,
    const int vnode_type,
    const int is_root,
    struct usfs_node ** node_out
)
{
    struct usfs_node * node = NULL;
    const int rc = _allocate_node (filesystem, vnode_type, is_root, &node);
    if (rc != 0)
        return rc;

    _publish_node (mount_data, node, nodeid, parent);
    *node_out = node;

    return 0;
}

/**
 * Returns the mount's private data for a vnode, or NULL if the vnode's vfs carries none.
 */
static struct usfs_mount_data * _mount_of (struct vnode * file_vnode)
{
    return (struct usfs_mount_data *)file_vnode->v_vfsp->vfs_data;
}

/* Callers hold global_lock while changing vnode reference ownership. */
static void usfs_vnode_hold_locked (struct vnode * file_vnode, const enum usfs_instrumentation_node_reuse source)
{
    if (source != USFS_INSTRUMENT_NODE_REUSE_NONE)
        usfs_instrumentation_node_reused (source);
    usfs_instrumentation_vnode_hold ((uint64_t)file_vnode->v_count);
    file_vnode->v_count += 1;
}

static void usfs_vnode_release_locked (struct vnode * file_vnode)
{
    usfs_instrumentation_vnode_release ((uint64_t)file_vnode->v_count);
    file_vnode->v_count -= 1;
}

/**
 * Returns the per-file private data of a vnode, or NULL if its gnode carries none.
 */
static struct usfs_node * _node_of (struct vnode * file_vnode)
{
    return (struct usfs_node *)file_vnode->v_gnode->gn_data;
}

#include "open_state.h"

#include "namespace_security.h"

/* usfs_call consumes a failed exchange; successful replies remain ours. */
static int exchange_status_request (struct usfs_connection * connection, const struct usfs_request_allocation_spec * request_spec)
{
    struct usfs_request * request = NULL;

    int rc = allocate_request (request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (connection, request);
    if (rc != 0)
        return rc;

    rc = request->error;
    free_request (request);

    return rc;
}

/**
 * Tells the daemon that a file handle is finished with.
 *
 * Parameters:
 *   mount_data    - Points to the mount the file belongs to.
 *   nodeid - The node id the handle belongs to.
 *   file_handle     - The handle from usfs_open_out, or from usfs_create_out.
 *   flags  - The open flags the handle was created with.
 *   is_directory  - Non-zero if the object is a directory, which the high-level FUSE API closes
 *            through a separate callback.
 *   credentials    - Points to the cred structure.
 *
 * Description:
 *   Best effort: the handle counts as gone whatever the daemon says, including when it has
 *   died and the call fails immediately. Used by vnop_close, and when vnop_open or vnop_create
 *   fails after the daemon has already issued a handle -- no vnop_close follows either failed
 *   operation, so the handle would otherwise be stranded in the daemon for the life of the mount.
 */
static void _send_release (
    struct usfs_mount_data * mount_data,
    const uint64_t nodeid,
    const uint64_t file_handle,
    const int32long64_t flags,
    const uint32_t is_directory,
    struct ucred * credentials
)
{
    struct usfs_release_in body;
    memset (&body, 0, sizeof (body));
    body.fh = file_handle;
    body.flags = (uint32_t)flags;
    body.isdir = is_directory;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_RELEASE,
                                                               .node_id = nodeid,
                                                               .opcode_specific_body = { .bytes = &body, .length = sizeof (body) },
                                                               .credentials = credentials };

    /* The final owner is gone: no ACTIVE channel may retain an unreachable
     * handle after a failed release exchange. */
    const int rc = exchange_status_request (mount_data->conn, &request_spec);
    if (rc != 0)
        abort_connection (mount_data->conn);
}

#include "open_reference.h"

/** Sends an operation with a fixed body and no reply body. */
static int _call_conn_no_reply (
    struct usfs_connection * connection,
    const uint16_t opcode,
    const uint64_t nodeid,
    const void * body,
    const uint32_t body_length,
    struct ucred * credentials
)
{
    if (connection == NULL)
        return EIO;

    const struct usfs_request_allocation_spec request_spec = { .opcode = opcode,
                                                               .node_id = nodeid,
                                                               .opcode_specific_body = { .bytes = body, .length = body_length },
                                                               .credentials = credentials };

    return exchange_status_request (connection, &request_spec);
}

#include "forget.h"

static int _call_no_reply (
    struct usfs_mount_data * mount_data,
    const uint16_t opcode,
    const uint64_t nodeid,
    const void * body,
    const uint32_t body_length,
    struct ucred * credentials
)
{
    return _call_conn_no_reply (mount_data != NULL ? mount_data->conn : NULL, opcode, nodeid, body, body_length, credentials);
}

static int _sync_connection (struct usfs_connection * connection, const uint32_t mode, struct ucred * credentials)
{
    struct usfs_syncfs_in body;

    memset (&body, 0, sizeof (body));
    body.mode = mode;
    if (!usfs_syncfs_in_valid (&body))
        return EINVAL;

    const int cache_rc = flush_connection_cache (connection);
    if (cache_rc != 0)
        return cache_rc;

    return _call_conn_no_reply (connection, (uint16_t)USFS_OP_SYNCFS, USFS_ROOT_ID, &body, (uint32_t)sizeof (body), credentials);
}

static unsigned retain_mounted_connections (struct usfs_connection ** connections)
{
    unsigned connection_count = 0;

    synchronized_with (g_connections_table_lock)
    {
        for (unsigned connection_index = 0; connection_index < USFS_MAX_CONNECTIONS; ++connection_index)
        {
            struct usfs_connection * connection = g_connections[connection_index];

            if (connection == NULL)
                continue;

            if (connection->mounts_counter <= 0)
                continue;

            connection->refs_counter += 1;
            connections[connection_count++] = connection;
        }
    }

    return connection_count;
}

/* Snapshot under the table lock, then perform daemon I/O without it. Retained
 * connections stay alive if an unmount races the synchronization pass. */
static int _sync_all_mounts (const uint32_t mode, struct ucred * credentials)
{
    struct usfs_connection * connections[USFS_MAX_CONNECTIONS];
    const unsigned connection_count = retain_mounted_connections (connections);
    int first_error = 0;

    for (unsigned connection_index = 0; connection_index < connection_count; ++connection_index)
    {
        const int sync_rc = _sync_connection (connections[connection_index], mode, credentials);
        if (first_error == 0)
            first_error = sync_rc;

        release_connection (connections[connection_index]);
    }

    return first_error;
}

/** Flushes userspace buffers associated with one open file description. */
static int _flush_handle (
    struct usfs_mount_data * mount_data,
    const uint64_t nodeid,
    const uint64_t file_handle,
    const int32long64_t flags,
    struct ucred * credentials
)
{
    struct usfs_flush_in body;

    memset (&body, 0, sizeof (body));
    body.fh = file_handle;
    body.flags = (uint32_t)flags;
    return _call_no_reply (mount_data, (uint16_t)USFS_OP_FLUSH, nodeid, &body, (uint32_t)sizeof (body), credentials);
}

static int prepare_sync_request (
    struct usfs_fsync_in * body,
    const struct vnode * file_vnode,
    const int32long64_t flags,
    const int use_range,
    const offset_t offset,
    const offset_t length
)
{
    if (use_range)
    {
        if (offset < 0)
            return EINVAL;

        if (length < 0)
            return EINVAL;
    }

    if ((flags & FDATASYNC) != 0)
        body->flags |= USFS_FSYNC_DATASYNC;

    if (use_range)
    {
        body->flags |= USFS_FSYNC_RANGE;
        body->offset = (uint64_t)offset;
        body->length = (uint64_t)length;
    }

    if (file_vnode->v_gnode->gn_type == VDIR)
        body->flags |= USFS_FSYNC_DIRECTORY;

    if (!usfs_fsync_in_valid (body))
        return EINVAL;

    return 0;
}

/** Persists a file or directory, optionally using data-only or range semantics. */
static int _sync_node (
    struct vnode * file_vnode,
    const uint64_t file_handle,
    const int32long64_t flags,
    const int use_range,
    const offset_t offset,
    const offset_t length,
    struct ucred * credentials
)
{
    if (file_vnode == NULL)
        return EINVAL;

    if (file_vnode->v_gnode == NULL)
        return EINVAL;

    if (file_vnode->v_vfsp == NULL)
        return EINVAL;

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    const struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL)
        return EIO;

    if (node == NULL)
        return EIO;

    struct usfs_fsync_in body;

    memset (&body, 0, sizeof (body));
    body.fh = file_handle;

    int rc = prepare_sync_request (&body, file_vnode, flags, use_range, offset, length);
    if (rc != 0)
        return rc;

    rc = flush_cached_pages (file_vnode, use_range, (uint64_t)offset, (uint64_t)length, (flags & FNOCACHE) != 0);
    if (rc != 0)
        return rc;

    return _call_no_reply (mount_data, (uint16_t)USFS_OP_FSYNC, node->nodeid, &body, (uint32_t)sizeof (body), credentials);
}

/**
 * Sends a USFS_OP_SETATTR request for an object.
 *
 * Parameters:
 *   file_vnode   - Points to the v-node of the object whose attributes are being changed.
 *   body - The marshalled change, with valid naming the fields that carry meaning.
 *   credentials  - Points to the cred structure.
 *
 * Description:
 *   Shared by vnop_setattr and vnop_ftrunc, which differ only in how they arrive at the
 *   change: truncation is "set the size", and folding it here keeps ftrunc, an open with
 *   FTRUNC and a future truncate() on one path. The reply's attributes are not used; the
 *   next getattr fetches them, and nothing here caches them. No lock is held across the
 *   message.
 *
 * Return values:
 *   0 - Indicates success.
 *   Nonzero return values are returned from the /usr/include/sys/errno.h file to indicate failure.
 */
static int _setattr_unlocked (struct vnode * file_vnode, const struct usfs_setattr_in * body, const uint64_t file_handle, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    struct usfs_node * node = _node_of (file_vnode);

    if (mount_data == NULL)
        return EIO;

    if (node == NULL)
        return EIO;

    if (!mount_data->writable)
        return EROFS;

    struct usfs_setattr_in attribute_request = *body;
    attribute_request.fh = file_handle;

    const struct usfs_request_allocation_spec request_spec = { .opcode = (uint16_t)USFS_OP_SETATTR,
                                                               .node_id = node->nodeid,
                                                               .opcode_specific_body = { .bytes = &attribute_request,
                                                                                         .length = sizeof (attribute_request) },
                                                               .max_allowed_reply_buffer_size = (uint32_t)sizeof (struct usfs_attr_out),
                                                               .credentials = credentials };

    return exchange_status_request (mount_data->conn, &request_spec);
}

static int _request_node_attributes_with_handle (
    const struct usfs_mount_data *,
    const struct usfs_node *,
    struct ucred *,
    struct usfs_attr_out *,
    uint64_t
);

/* Serialize this check with chmod/chown using the mount namespace lock.
 * AIX preserves these bits on non-executable files and privileged writes. */
static int usfs_strip_write_privileges (struct vnode * file_vnode, const uint64_t file_handle, struct ucred * credentials)
{
    if (credentials == NULL)
        return EINVAL;

    if (privcheck_cr (SET_OBJ_DAC, credentials) == 0)
        return 0;

    struct usfs_attr_out attributes;

    const int rc = _request_node_attributes_with_handle (_mount_of (file_vnode), _node_of (file_vnode), credentials, &attributes, file_handle);
    if (rc != 0)
        return rc;

    if ((attributes.attr.mode & S_IFMT) != S_IFREG)
        return 0;

    if ((attributes.attr.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
        return 0;

    if ((attributes.attr.mode & (S_ISUID | S_ISGID)) == 0)
        return 0;

    struct usfs_setattr_in change;

    memset (&change, 0, sizeof (change));
    change.valid = USFS_SET_MODE;
    change.mode = attributes.attr.mode & ~(S_ISUID | S_ISGID);

    return _setattr_unlocked (file_vnode, &change, file_handle, credentials);
}

static int _setattr (struct vnode * file_vnode, const struct usfs_setattr_in * body, const uint64_t file_handle, struct ucred * credentials)
{
    struct usfs_node * node = _node_of (file_vnode);
    if (node == NULL)
        return EIO;

    if ((body->valid & USFS_SET_SIZE) == 0)
        return _setattr_unlocked (file_vnode, body, file_handle, credentials);

    struct usfs_mount_data * mount_data = _mount_of (file_vnode);
    if (mount_data == NULL)
        return EIO;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_read_synchronized_acquire (&mount_data->namespace_lock);

    write_synchronized_with (node->mutation_lock)
    {
        const int rc = usfs_strip_write_privileges (file_vnode, file_handle, credentials);
        if (rc != 0)
            return rc;

        if (file_vnode->v_gnode->gn_seg != 0)
            return resize_cached_file (file_vnode, body, file_handle, credentials);

        return _setattr_unlocked (file_vnode, body, file_handle, credentials);
    }

    return EIO;
}

/**
 * Sends a mutating request naming an entry, expecting no reply body.
 *
 * Parameters:
 *   directory_vnode     - Points to the v-node the request is addressed to, which becomes the request's
 *             node id. That is the containing directory for most operations, but the existing
 *             file for USFS_OP_LINK, whose body carries the directory instead.
 *   opcode  - The USFS_OP_* operation code.
 *   body    - Per-operation request body, or NULL.
 *   body_length - Size of body in bytes.
 *   name    - The entry name. Operations naming a second object pass it through data.
 *   data    - Bytes appended after the name, or NULL.
 *   data_length - Size of data in bytes.
 *   credentials     - Points to the cred structure.
 *
 * Description:
 *   Shared by the directory-modifying entry points. A read-only file system is refused here
 *   without a message to the daemon, which could only refuse it too. No lock is held across
 *   the message.
 *
 * Return values:
 *   0 - Indicates success.
 *   Nonzero return values are returned from the /usr/include/sys/errno.h file to indicate failure.
 */
static int _mutate_entry (
    struct vnode * directory_vnode,
    const uint16_t opcode,
    const void * body,
    const uint32_t body_length,
    const char * name,
    const void * data,
    const uint32_t data_length,
    struct ucred * credentials
)
{
    struct usfs_mount_data * mount_data = _mount_of (directory_vnode);
    struct usfs_node * directory_node = _node_of (directory_vnode);

    if (mount_data == NULL)
        return EIO;

    if (directory_node == NULL)
        return EIO;

    if (!mount_data->writable)
        return EROFS;

    if (name == NULL)
        return EINVAL;

    if (name[0] == '\0')
        return EINVAL;

    const struct usfs_request_allocation_spec request_spec = { .opcode = opcode,
                                                               .node_id = directory_node->nodeid,
                                                               .opcode_specific_body = { .bytes = body, .length = body_length },
                                                               .opcode_specific_argument = name,
                                                               .opcode_specific_payload = { .bytes = data, .length = data_length },
                                                               .credentials = credentials };

    return exchange_status_request (mount_data->conn, &request_spec);
}

/**
 * Populates the module-global vfsops structure with USFS's VFS operation entry points.
 *
 * Description:
 *   Zeroes the module-global vfsops structure (declared near the top of this file) with
 *   memset, then assigns each of USFS's implemented VFS operations to the matching vfsops
 *   field: vfs_cntl, vfs_mount, vfs_root, vfs_statfs, vfs_sync, vfs_unmount, vfs_vget, and
 *   vfs_quotactl. Fields not assigned here are left zeroed, so the kernel treats the
 *   corresponding VFS operations as unsupported. The populated structure is later attached to
 *   the file system's gfs descriptor (see init_global_gfs_descriptor, which sets gfs.gfs_ops
 *   to &vfsops) so the kernel can dispatch VFS calls to USFS. Called once from
 *   usfs_kext_initialize, before the file system implementation is registered with the kernel.
 */
void init_global_virtual_fs_operations ()
{
    memset (&vfsops, 0, sizeof (vfsops));

    vfsops.vfs_cntl = usfs_cntl;
    vfsops.vfs_mount = usfs_mount;
    vfsops.vfs_root = usfs_root;
    vfsops.vfs_statfs = usfs_statfs;
    vfsops.vfs_sync = usfs_sync;
    vfsops.vfs_syncvfs = usfs_syncvfs;
    vfsops.vfs_unmount = usfs_unmount;
    vfsops.vfs_vget = usfs_vget;
    vfsops.vfs_quotactl = usfs_quotactl;
}

/**
 * Populates the module-global vnodeops structure with USFS's VNODE operation entry points.
 *
 * Description:
 *   Zeroes the module-global gn_ops structure (declared near the top of this file, and referred
 *   to as "vnodeops" in this driver's comments) with memset, then assigns each of USFS's
 *   implemented VNODE operations to the matching gn_ops field: vn_link, vn_mkdir, vn_mknod,
 *   vn_remove, vn_rename, vn_rmdir, vn_lookup, vn_fid, vn_open, vn_create, vn_hold, vn_rele,
 *   vn_close, vn_map, vn_unmap, vn_access, vn_getattr, vn_setattr, vn_fclear, vn_fsync,
 *   vn_ftrunc, vn_rdwr, vn_lockctl, vn_ioctl, vn_readlink, vn_select, vn_symlink, vn_readdir,
 *   vn_revoke, vn_getacl, vn_setacl, vn_getpcl, vn_setpcl, vn_seek, vn_fsync_range,
 *   vn_create_attr, vn_finfo, vn_map_lloff, vn_readdir_eofp, vn_rdwr_attr, vn_memcntl, vn_getea,
 *   vn_setea, vn_listea, vn_removeea, vn_statea, vn_getxacl, vn_setxacl, vn_erdwr_attr, and the
 *   thread page-I/O strategy required by client segments.
 *   Fields not assigned here are left zeroed, so the kernel treats the corresponding VNODE
 *   operations as unsupported. Every gnode allocated by this driver has its gn_ops field
 *   pointed at this structure (see allocate_gnode, which sets gnode->gn_ops = &gn_ops), so the
 *   kernel dispatches all per-file/per-directory VNODE calls into USFS through it. Called once
 *   from usfs_kext_initialize, before the file system implementation is registered with the
 *   kernel.
 */
void init_global_virtual_inode_operations ()
{
    union
    {
        thrpgio_t pager;                                             // Descriptor as invoked by the VMM.
        int (*vnode) (struct vnode *, struct buf *, struct ucred *); // Same descriptor in the vnode operations table.
    } strategy_entry;

    memset (&gn_ops, 0, sizeof (gn_ops));

    /*
     * VMM finds the remote PDT entry through gn_ops.vn_strategy, but invokes
     * the same descriptor with the thrpgio_t ABI. Preserve the descriptor
     * bits through a union so both identities remain exactly equal.
     */
    strategy_entry.pager = usfs_pager_strategy;

    gn_ops.vn_link = gn_link;
    gn_ops.vn_mkdir = gn_mkdir;
    gn_ops.vn_mknod = gn_mknod;
    gn_ops.vn_remove = gn_remove;
    gn_ops.vn_rename = gn_rename;
    gn_ops.vn_rmdir = gn_rmdir;
    gn_ops.vn_lookup = gn_lookup;
    gn_ops.vn_fid = gn_fid;
    gn_ops.vn_open = gn_open;
    gn_ops.vn_create = gn_create;
    gn_ops.vn_hold = gn_hold;
    gn_ops.vn_rele = gn_rele;
    gn_ops.vn_close = gn_close;
    gn_ops.vn_map = gn_map;
    gn_ops.vn_unmap = gn_unmap;
    gn_ops.vn_access = gn_access;
    gn_ops.vn_getattr = gn_getattr;
    gn_ops.vn_setattr = gn_setattr;
    gn_ops.vn_fclear = gn_fclear;
    gn_ops.vn_fsync = gn_fsync;
    gn_ops.vn_ftrunc = gn_ftrunc;
    gn_ops.vn_rdwr = gn_rdwr;
    gn_ops.vn_lockctl = gn_lockctl;
    gn_ops.vn_ioctl = gn_ioctl;
    gn_ops.vn_readlink = gn_readlink;
    gn_ops.vn_select = gn_select;
    gn_ops.vn_symlink = gn_symlink;
    gn_ops.vn_readdir = gn_readdir;
    gn_ops.vn_strategy = strategy_entry.vnode;
    gn_ops.vn_revoke = gn_revoke;
    gn_ops.vn_getacl = gn_getacl;
    gn_ops.vn_setacl = gn_setacl;
    gn_ops.vn_getpcl = gn_getpcl;
    gn_ops.vn_setpcl = gn_setpcl;
    gn_ops.vn_seek = gn_seek;
    gn_ops.vn_fsync_range = gn_fsync_range;
    gn_ops.vn_create_attr = gn_create_attr;
    gn_ops.vn_finfo = gn_finfo;
    gn_ops.vn_map_lloff = gn_map_lloff;
    gn_ops.vn_readdir_eofp = gn_readdir_eofp;
    gn_ops.vn_rdwr_attr = gn_rdwr_attr;
    gn_ops.vn_memcntl = gn_memcntl;
    gn_ops.vn_getea = gn_getea;
    gn_ops.vn_setea = gn_setea;
    gn_ops.vn_listea = gn_listea;
    gn_ops.vn_removeea = gn_removeea;
    gn_ops.vn_statea = gn_statea;
    gn_ops.vn_getxacl = gn_getxacl;
    gn_ops.vn_setxacl = gn_setxacl;
    gn_ops.vn_erdwr_attr = gn_erdwr_attr;
}

/**
 * Populates the module-global gfs structure describing USFS's file system implementation.
 *
 * Description:
 *   Zeroes the module-global gfs structure (declared near the top of this file) with memset,
 *   then fills in the fields the kernel's gfsadd kernel service needs to register USFS as a
 *   file system implementation: gfs_name is set to "usfs"; gfs_type is left 0 here (it is
 *   updated later, at FS registration time); gn_ops points at the module-global vnodeops
 *   structure populated by init_global_virtual_inode_operations; gfs_ops points at the
 *   module-global vfsops structure populated by init_global_virtual_fs_operations; both
 *   gfs_init and gfs_rinit are set to usfs_init; gfs_data points at the module-global
 *   usfs_private_data structure; and gfs_hold is left 0. gfs_flags is set to GFS_VERSION4
 *   (required, so the vnode.h structures this driver relies on are supported) combined with
 *   GFS_VERSION421, GFS_OFLAGS64, GFS_REMOTE, and GFS_FUMNT; the latter is
 *   backed by deferred stale-vnode reclamation. The remaining GFS_* flags are
 *   left commented out and unset. Called once from
 *   usfs_kext_initialize, after init_global_virtual_fs_operations and
 *   init_global_virtual_inode_operations and before the file system implementation is
 *   registered with the kernel.
 */
void init_global_gfs_descriptor ()
{
    memset (&gfs, 0, sizeof (gfs));

    strcpy (gfs.gfs_name, "usfs");

    gfs.gfs_type = 0; // will be updated later at FS registration time
    gfs.gn_ops = &gn_ops;
    gfs.gfs_ops = &vfsops;
    gfs.gfs_init = usfs_init;
    gfs.gfs_data = (caddr_t)&usfs_private_data;
    gfs.gfs_hold = 0;

    /* Enable the vnode ABI, large offsets, expanded VFS operations, and stale
     * vnode retention until the final vnop_rele. */
    gfs.gfs_flags = GFS_VERSION4 | GFS_VERSION421 | GFS_VERSION53 | GFS_OFLAGS64 | GFS_REMOTE | GFS_SYNCVFS | GFS_FUMNT;
}

/**
 * Initializes the module-global file operation lock.
 *
 * Description:
 *   Allocates and initializes global_lock, the module-global complex_lock_t used to
 *   synchronize mount/node lifecycle across the module. A complex (sleepable) lock is used
 *   instead of a simple lock so that operations which may block can still be performed
 *   while the lock is held. The lock is allocated with lock_alloc, using paged-memory
 *   allocation and a fixed lock class/instance index pair, then initialized as
 *   non-recursive with lock_init. Also allocates and initializes g_conn_table_lock, the
 *   simple lock guarding the connection table and connection reference counts, and zeroes
 *   the g_conns[] table itself. Logs a message once initialization completes.
 */
void init_global_lock ()
{
    static const short global_lock_class = SHRT_MAX - 3;
    static const short global_lock_instance_index = -1;
    static const short conn_table_lock_class = SHRT_MAX - 5;
    static const short conn_table_lock_instance_index = -1;
    static const short lifecycle_lock_class = SHRT_MAX - 6;
    static const short lifecycle_lock_instance_index = -1;

    // Initialize complex lock (sleepable) instead of simple lock
    // This allows blocking operations while holding the lock
    lock_alloc (&global_lock, LOCK_ALLOC_PAGED, global_lock_class, global_lock_instance_index);
    lock_init (&global_lock, FALSE); // FALSE = not recursive

    // Connection table lock (simple; always taken alone) and the table itself
    lock_alloc (&g_connections_table_lock, LOCK_ALLOC_PAGED, conn_table_lock_class, conn_table_lock_instance_index);
    simple_lock_init (&g_connections_table_lock);
    memset (g_connections, 0, sizeof (g_connections));

    lock_alloc (&g_lifecycle_lock, LOCK_ALLOC_PAGED, lifecycle_lock_class, lifecycle_lock_instance_index);
    simple_lock_init (&g_lifecycle_lock);
    g_mount_count = 0;
    g_gate_is_open = 0;
    g_kext_is_running_control_operation = 0;
    set_up_observability_infrastructure ();
}

void destroy_global_locks (void)
{
    tear_down_observability_infrastructure ();
    lock_free (&g_lifecycle_lock);
    lock_free (&g_connections_table_lock);
    lock_free (&global_lock);
}

/**
 * Registers the module's file system implementation with the kernel.
 *
 * Description:
 *   Calls the gfsadd kernel service to register the driver's global gfs descriptor (see
 *   init_global_gfs_descriptor) as a new file system type. AIX limits the number of
 *   user-defined file system types and does not expose which type numbers other kernel
 *   extensions have already taken, so this function probes each candidate number from
 *   MNT_USRVFS up to MNT_USRLAST, setting gfs.gfs_type to the candidate and calling gfsadd,
 *   retrying with the next number whenever gfsadd fails with EBUSY (the number is already in
 *   use). The loop stops as soon as gfsadd returns anything other than EBUSY: success (0),
 *   EINVAL, or another error. On success, gfs.gfs_type is left set to the accepted FS number,
 *   which should be used in /etc/vfs to register the new FS type in the system. Progress and
 *   each gfsadd EBUSY/EINVAL failure are logged in debug builds.
 *
 * Return values:
 *   0 - The file system implementation was registered successfully.
 *   1 - Registration failed; gfsadd returned an error other than EBUSY, or every number from
 *       MNT_USRVFS through MNT_USRLAST was already in use.
 */
int register_fs_implementation ()
{
    // In AIX the number of custom user-defined file systems is limited.
    // When a new FS is being registered in the system, it should have a number (type code) assigned.
    // The FS number can be one starting from MNT_USRVFS and up to MNT_USRLAST.
    // We don't know in advance what numbers are already taken by other kernel extensions, so we try all
    // of them to find a free one.

    int rc = EBUSY;
    int filesystem_type = MNT_USRVFS - 1;

    do
    {
        filesystem_type += 1;
        gfs.gfs_type = filesystem_type;

        rc = gfsadd (filesystem_type, &gfs);
    }
    while (rc == EBUSY && filesystem_type <= MNT_USRLAST);

    enum
    {
        FS_REGISTRATION_SUCCEEDED = 0,
        FS_REGISTRATION_FAILED = 1
    };

    if (rc != 0)
        return FS_REGISTRATION_FAILED;

    return FS_REGISTRATION_SUCCEEDED;
}

#endif
