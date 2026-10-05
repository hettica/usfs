// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_LOOKUP_H
#define USFS_FS_OPERATIONS_VNODE_LOOKUP_H

struct lookup_context
{
    struct vnode * directory_vnode;          // Directory vnode searched by AIX.
    struct vnode ** result_vnode;            // Resolved vnode returned to AIX.
    const char * entry_name;                 // Directory entry requested by AIX.
    struct vattr * attributes;               // Optional resolved attributes returned to AIX.
    struct ucred * credentials;              // Credentials authorizing the lookup.
    struct usfs_mount_data * mount_data;     // Mounted filesystem instance.
    const struct usfs_node * directory_node; // Backing node for the searched directory.
    struct usfs_entry_out entry;             // Validated daemon lookup reply.
    struct usfs_node * resolved_node;        // Node retained for a successful lookup.
    int is_parent;                           // Whether entry_name requests the parent.
};

static struct usfs_node * find_lookup_node (const struct usfs_mount_data * mount_data, const uint64_t node_id)
{
    struct usfs_node * node = mount_data->nodes;

    while (node != NULL)
    {
        if (node->nodeid == node_id)
            return node;

        node = node->next;
    }

    return NULL;
}

static enum usfs_instrumentation_node_reuse lookup_reuse_reason (const int is_parent)
{
    if (is_parent)
        return USFS_INSTRUMENT_NODE_REUSE_PARENT;

    return USFS_INSTRUMENT_NODE_REUSE_LOOKUP;
}

static void hold_lookup_node (struct usfs_node * node, const int is_parent)
{
    usfs_vnode_hold_locked (node->vn, lookup_reuse_reason (is_parent));
}

static int retain_lookup_node_locked (struct usfs_node * node, const int is_parent)
{
    if (node->lookup_refs == UINT64_MAX)
        return EOVERFLOW;

    hold_lookup_node (node, is_parent);
    return 0;
}

static int create_lookup_node_locked (struct lookup_context * context, struct usfs_node ** result_node)
{
    if (context->is_parent)
        usfs_instrumentation_parent_rebuilt ();

    const uint64_t parent_node_id = context->is_parent ? 0 : context->directory_node->nodeid;
    const int vnode_type = usfs_vtype_from_mode (context->entry.attr.mode);
    const int is_root = context->entry.nodeid == USFS_ROOT_ID;

    const int rc =
        _create_node (context->directory_vnode->v_vfsp, context->mount_data, context->entry.nodeid, parent_node_id, vnode_type, is_root, result_node);
    if (rc != 0)
        return rc;

    if (is_root)
        context->mount_data->root = *result_node;

    return 0;
}

static int acquire_lookup_node (struct lookup_context * context)
{
    write_synchronized_with (global_lock)
    {
        struct usfs_node * node = find_lookup_node (context->mount_data, context->entry.nodeid);
        int rc = 0;

        if (node != NULL)
            rc = retain_lookup_node_locked (node, context->is_parent);
        else
            rc = create_lookup_node_locked (context, &node);

        if (rc != 0)
            return rc;

        node->lookup_refs++;
        context->resolved_node = node;
    }

    return 0;
}

static int copy_lookup_reply (struct lookup_context * context, const struct usfs_request * request)
{
    if (request->error != 0)
        return request->error;

    if (request->reply_buffer_size != sizeof (context->entry))
    {
        abort_connection (context->mount_data->conn);
        return EIO;
    }

    memcpy (&context->entry, request->reply_buffer, sizeof (context->entry));
    return 0;
}

static int request_lookup_entry (struct lookup_context * context)
{
    struct usfs_request_allocation_spec request_spec = { 0 };

    request_spec.opcode = (uint16_t)USFS_OP_LOOKUP;
    request_spec.node_id = context->directory_node->nodeid;
    request_spec.opcode_specific_argument = context->entry_name;
    request_spec.max_allowed_reply_buffer_size = (uint32_t)sizeof (context->entry);
    request_spec.credentials = context->credentials;

    struct usfs_request * request = NULL;
    int rc = allocate_request (&request_spec, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (context->mount_data->conn, request);
    if (rc != 0)
        return rc;

    rc = copy_lookup_reply (context, request);
    free_request (request);
    if (rc != 0)
        return rc;

    if (!usfs_entry_out_valid (&context->entry))
    {
        abort_connection (context->mount_data->conn);
        return EIO;
    }

    return 0;
}

static int lookup_is_self (const struct lookup_context * context)
{
    if (context->entry_name[0] == '\0')
        return true;

    if (strcmp (context->entry_name, ".") == 0)
        return true;

    if (strcmp (context->entry_name, "..") != 0)
        return false;

    return context->directory_node->nodeid == USFS_ROOT_ID;
}

static int hold_lookup_self (const struct lookup_context * context)
{
    /* A held or removed directory retains its own stable identity. */
    if (context->attributes != NULL)
    {
        const int rc = gn_getattr (context->directory_vnode, context->attributes, context->credentials);
        if (rc != 0)
            return rc;
    }

    VNOP_HOLD (context->directory_vnode);
    *context->result_vnode = context->directory_vnode;

    return 0;
}

static int validate_lookup_arguments (const struct lookup_context * context)
{
    if (context->result_vnode == NULL)
        return EINVAL;

    if (context->entry_name == NULL)
        return EINVAL;

    *context->result_vnode = NULL;
    return 0;
}

static int validate_lookup_directory (const struct lookup_context * context)
{
    if (context->mount_data == NULL)
        return EIO;

    if (context->directory_node == NULL)
        return EIO;

    return 0;
}

static int authorize_lookup_directory (const struct lookup_context * context)
{
    return gn_access (context->directory_vnode, X_ACC, ACC_SELF, context->credentials);
}

static int validate_lookup_name (const struct lookup_context * context)
{
    const uint32_t name_length = calculate_bounded_opcode_specific_argument_length (context->entry_name, USFS_MAX_NAME, USFS_MAX_NAME);

    if (name_length == 0)
        return ENAMETOOLONG;

    return 0;
}

static int discard_unattached_lookup_reply (const struct lookup_context * context, const int error)
{
    (void)usfs_forget (context->mount_data->conn, context->entry.nodeid, 1);
    return error;
}

static int prepare_remote_lookup (struct lookup_context * context)
{
    int rc = request_lookup_entry (context);
    if (rc != 0)
        return rc;

    rc = usfs_checkpoint (USFS_INSTRUMENT_LOOKUP_POST_REPLY);
    if (rc != 0)
        return discard_unattached_lookup_reply (context, rc);

    rc = acquire_lookup_node (context);
    if (rc != 0)
        return discard_unattached_lookup_reply (context, rc);

    return 0;
}

static int publish_remote_lookup (const struct lookup_context * context)
{
    const int rc = usfs_checkpoint (USFS_INSTRUMENT_LOOKUP_RESOLVED);

    if (rc != 0)
    {
        VNOP_RELE (context->resolved_node->vn);
        return rc;
    }

    if (context->attributes != NULL)
        usfs_attr_to_vattr (&context->entry.attr, context->attributes);

    *context->result_vnode = context->resolved_node->vn;
    return 0;
}

static int lookup_remote_entry (struct lookup_context * context)
{
    const int rc = prepare_remote_lookup (context);
    if (rc != 0)
        return rc;

    return publish_remote_lookup (context);
}

int gn_lookup (
    struct vnode * directory_vnode,
    struct vnode ** result_vnode,
    char * entry_name,
    int32long64_t flags,
    struct vattr * attributes,
    struct ucred * credentials
)
{
    ignore_parameter flags;

    struct lookup_context context = { 0 };

    context.directory_vnode = directory_vnode;
    context.result_vnode = result_vnode;
    context.entry_name = entry_name;
    context.attributes = attributes;
    context.credentials = credentials;
    context.mount_data = _mount_of (directory_vnode);
    context.directory_node = _node_of (directory_vnode);

    int rc = validate_lookup_arguments (&context);
    if (rc != 0)
        return rc;

    rc = validate_lookup_directory (&context);
    if (rc != 0)
        return rc;

    rc = authorize_lookup_directory (&context);
    if (rc != 0)
        return rc;

    rc = validate_lookup_name (&context);
    if (rc != 0)
        return rc;

    if (lookup_is_self (&context))
        return hold_lookup_self (&context);

    context.is_parent = strcmp (context.entry_name, "..") == 0;
    return lookup_remote_entry (&context);
}

#endif
