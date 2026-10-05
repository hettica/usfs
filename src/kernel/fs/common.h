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

static void _free_vnode (struct vnode * file_vnode, const short occurrence)
{
    lock_free (&file_vnode->v_lock);

    usfs_free_lock_occurrence (vnode_lock_occurrences, occurrence);

    vn_free (file_vnode);
}

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

static void _publish_node (struct usfs_mount_data * mount_data, struct usfs_node * node, const uint64_t nodeid, const uint64_t parent)
{
    node->nodeid = nodeid;
    node->parent = parent;
    node->next = mount_data->nodes;
    mount_data->nodes = node;
    usfs_instrumentation_node_created (mount_data, nodeid);
}

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

static struct usfs_mount_data * _mount_of (struct vnode * file_vnode)
{
    return (struct usfs_mount_data *)file_vnode->v_vfsp->vfs_data;
}

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

static struct usfs_node * _node_of (struct vnode * file_vnode)
{
    return (struct usfs_node *)file_vnode->v_gnode->gn_data;
}

#include "open_state.h"
#include "namespace_security.h"

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

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = (uint16_t)USFS_OP_RELEASE,
        .node_id = nodeid,
        .opcode_specific_body = { .bytes = &body, .length = sizeof (body) },
        .credentials = credentials
    };

    const int rc = exchange_status_request (mount_data->conn, &request_spec);
    if (rc != 0)
        abort_connection (mount_data->conn);
}

#include "open_reference.h"

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

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = opcode,
        .node_id = nodeid,
        .opcode_specific_body = { .bytes = body, .length = body_length },
        .credentials = credentials
    };

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

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = (uint16_t)USFS_OP_SETATTR,
        .node_id = node->nodeid,
        .opcode_specific_body = { .bytes = &attribute_request,
                                  .length = sizeof (attribute_request) },
        .max_allowed_reply_buffer_size = (uint32_t)sizeof (struct usfs_attr_out),
        .credentials = credentials
    };

    return exchange_status_request (mount_data->conn, &request_spec);
}

static int _request_node_attributes_with_handle (
    const struct usfs_mount_data *,
    const struct usfs_node *,
    struct ucred *,
    struct usfs_attr_out *,
    uint64_t
);

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

    const struct usfs_request_allocation_spec request_spec = {
        .opcode = opcode,
        .node_id = directory_node->nodeid,
        .opcode_specific_body = { .bytes = body, .length = body_length },
        .opcode_specific_argument = name,
        .opcode_specific_payload = { .bytes = data, .length = data_length },
        .credentials = credentials
    };

    return exchange_status_request (mount_data->conn, &request_spec);
}

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

void init_global_virtual_inode_operations ()
{
    union
    {
        thrpgio_t pager;                                             // Descriptor as invoked by the VMM.
        int (*vnode) (struct vnode *, struct buf *, struct ucred *); // Same descriptor in the vnode operations table.
    } strategy_entry;

    memset (&gn_ops, 0, sizeof (gn_ops));

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
    gfs.gfs_flags = GFS_VERSION4 | GFS_VERSION421 | GFS_VERSION53 | GFS_OFLAGS64 | GFS_REMOTE | GFS_SYNCVFS | GFS_FUMNT;
}

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
