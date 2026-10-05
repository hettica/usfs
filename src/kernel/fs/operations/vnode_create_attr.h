// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_CREATE_ATTR_H
#define USFS_FS_OPERATIONS_VNODE_CREATE_ATTR_H

struct create_attributes_context
{
    struct vnode * directory_vnode;          // Directory receiving the new entry.
    struct vnode ** result_vnode;            // Published vnode returned to AIX.
    caddr_t * file_info;                     // Open state returned to AIX.
    const char * entry_name;                 // Name of the new directory entry.
    const struct vattr * attributes;         // Initial attributes requested by AIX.
    struct ucred * credentials;              // Credentials authorizing the operation.
    struct usfs_mount_data * mount_data;     // Mounted filesystem instance.
    const struct usfs_node * directory_node; // Backing node for the parent directory.
    struct usfs_create_attr_in request_body; // Serialized daemon request body.
    struct usfs_create_out reply;            // Validated daemon response.
    struct usfs_node * prepared_node;        // Unpublished node owned by this operation.
    struct usfs_node * created_node;         // Published node returned by the daemon.
    struct usfs_open_state * open_state;     // Unattached open state owned by this operation.
    int32long64_t open_flags;                // AIX open flags.
    int32long64_t creation_action;           // AIX VC_* creation action.
};

static unsigned long calculate_supported_create_attribute_mask (void)
{
    const unsigned long identity_attributes = AT_UID | AT_GID;
    const unsigned long time_attributes = AT_ATIME | AT_MTIME | AT_CTIME;

    return AT_TYPE | AT_MODE | AT_SIZE | AT_EXT | identity_attributes | time_attributes;
}

static int reject_unsupported_create_attributes (const struct create_attributes_context * context)
{
    const unsigned long supported_attributes = calculate_supported_create_attribute_mask ();

    USFS_TRACE_FS5 (
        USFS_TRACE_CREATE_ATTR_REJECTED,
        context->attributes->va_mask,
        context->open_flags,
        (uint64_t)context->open_flags >> 32,
        context->creation_action,
        context->attributes->va_mask & ~supported_attributes
    );

    return EOPNOTSUPP;
}

static int validate_create_attribute_inputs (const struct create_attributes_context * context)
{
    const unsigned long supported_attributes = calculate_supported_create_attribute_mask ();
    const unsigned long unsupported_attributes = context->attributes->va_mask & ~supported_attributes;

    if (unsupported_attributes != 0)
        return reject_unsupported_create_attributes (context);

    if ((uint64_t)context->open_flags > UINT32_MAX)
        return reject_unsupported_create_attributes (context);

    if ((context->attributes->va_mask & AT_EXT) == 0)
        return 0;

    if (context->attributes->va_ext == 0)
        return 0;

    return reject_unsupported_create_attributes (context);
}

static uint32_t create_activation_value (const int32long64_t creation_action)
{
    if (creation_action == VC_OPEN)
        return USFS_CREATE_OPEN;

    if (creation_action == VC_LOOKUP)
        return USFS_CREATE_LOOKUP;

    return USFS_CREATE_DEFAULT;
}

static void copy_create_identity_attributes (const struct vattr * attributes, struct usfs_create_attr_in * request_body)
{
    if ((attributes->va_mask & AT_UID) != 0)
    {
        request_body->attr.valid |= USFS_SET_UID;
        request_body->attr.uid = (uint32_t)attributes->va_uid;
    }

    if ((attributes->va_mask & AT_GID) != 0)
    {
        request_body->attr.valid |= USFS_SET_GID;
        request_body->attr.gid = (uint32_t)attributes->va_gid;
    }
}

static void copy_create_size_attribute (const struct vattr * attributes, struct usfs_create_attr_in * request_body)
{
    if ((attributes->va_mask & AT_SIZE) == 0)
        return;

    request_body->attr.valid |= USFS_SET_SIZE;
    request_body->attr.size = (uint64_t)attributes->va_size;
}

static void copy_create_time_attributes (const struct vattr * attributes, struct usfs_create_attr_in * request_body)
{
    if ((attributes->va_mask & AT_ATIME) != 0)
    {
        request_body->attr.valid |= USFS_SET_ATIME;
        request_body->attr.atime = attributes->va_atime.tv_sec;
        request_body->attr.atimensec = (uint32_t)attributes->va_atime.tv_nsec;
    }

    if ((attributes->va_mask & AT_MTIME) != 0)
    {
        request_body->attr.valid |= USFS_SET_MTIME;
        request_body->attr.mtime = attributes->va_mtime.tv_sec;
        request_body->attr.mtimensec = (uint32_t)attributes->va_mtime.tv_nsec;
    }

    if ((attributes->va_mask & AT_CTIME) != 0)
    {
        request_body->attr.valid |= USFS_SET_CTIME;
        request_body->attr.ctime = attributes->va_ctime.tv_sec;
        request_body->attr.ctimensec = (uint32_t)attributes->va_ctime.tv_nsec;
    }
}

static int prepare_create_attributes_request (struct create_attributes_context * context)
{
    const int rc = validate_create_attribute_inputs (context);
    if (rc != 0)
        return rc;

    memset (&context->request_body, 0, sizeof (context->request_body));
    context->request_body.flags = (uint32_t)context->open_flags;
    context->request_body.activation = create_activation_value (context->creation_action);
    context->request_body.attr.valid = USFS_SET_MODE;
    context->request_body.attr.mode = (uint32_t)(context->attributes->va_mode & ~S_IFMT);

    copy_create_identity_attributes (context->attributes, &context->request_body);
    copy_create_size_attribute (context->attributes, &context->request_body);
    copy_create_time_attributes (context->attributes, &context->request_body);

    if (!usfs_create_attr_in_valid (&context->request_body))
        return EINVAL;

    return 0;
}

static void initialize_create_results (const struct create_attributes_context * context)
{
    if (context->result_vnode != NULL)
        *context->result_vnode = NULL;

    if (context->file_info != NULL)
        *context->file_info = NULL;
}

static int validate_create_arguments (const struct create_attributes_context * context)
{
    const int arguments_are_valid =
        usfs_create_attr_arguments_valid (context->result_vnode, context->attributes, context->creation_action, context->credentials);

    if (!arguments_are_valid)
        return EINVAL;

    if (context->creation_action == VC_OPEN)
    {
        if (context->file_info == NULL)
            return EINVAL;
    }

    if (context->entry_name == NULL)
        return EINVAL;

    if (context->entry_name[0] == '\0')
        return EINVAL;

    return 0;
}

static int requested_owner_requires_privilege (const struct create_attributes_context * context)
{
    const struct vattr * attributes = context->attributes;

    if ((attributes->va_mask & AT_UID) == 0)
        return false;

    return attributes->va_uid != context->credentials->cr_uid;
}

static int requested_group_requires_privilege (const struct create_attributes_context * context)
{
    const struct vattr * attributes = context->attributes;

    if ((attributes->va_mask & AT_GID) == 0)
        return false;

    return !groupmember_cr (attributes->va_gid, context->credentials);
}

static int create_identity_requires_privilege (const struct create_attributes_context * context)
{
    if (requested_owner_requires_privilege (context))
        return true;

    return requested_group_requires_privilege (context);
}

static int authorize_create_identity (const struct create_attributes_context * context)
{
    if (!create_identity_requires_privilege (context))
        return 0;

    if (privcheck_cr (SET_OBJ_DAC, context->credentials) != 0)
        return EPERM;

    return 0;
}

static int resolve_create_parent (struct create_attributes_context * context)
{
    context->mount_data = _mount_of (context->directory_vnode);
    context->directory_node = _node_of (context->directory_vnode);

    if (context->mount_data == NULL)
        return EIO;

    if (context->directory_node == NULL)
        return EIO;

    return 0;
}

static int prepare_create_resources (struct create_attributes_context * context)
{
    int rc = 0;

    if (context->creation_action == VC_OPEN)
    {
        rc = allocate_open_state (context->open_flags, &context->open_state);
        if (rc != 0)
            return rc;
    }

    if (context->creation_action == VC_DEFAULT)
        return 0;

    write_synchronized_with (global_lock)
    {
        rc = _allocate_node (context->directory_vnode->v_vfsp, VREG, 0, &context->prepared_node);
    }

    return rc;
}

static int allocate_create_attributes_request (const struct create_attributes_context * context, struct usfs_request ** request)
{
    struct usfs_request_allocation_spec allocation = { 0 };

    allocation.opcode = USFS_OP_CREATE_ATTR;
    allocation.node_id = context->directory_node->nodeid;
    allocation.opcode_specific_body.bytes = &context->request_body;
    allocation.opcode_specific_body.length = sizeof (context->request_body);
    allocation.opcode_specific_argument = context->entry_name;
    allocation.max_allowed_reply_buffer_size = sizeof (context->reply);
    allocation.credentials = context->credentials;

    return allocate_request (&allocation, request);
}

static int validate_create_attributes_reply (struct create_attributes_context * context, const struct usfs_request * request)
{
    if (request->error != 0)
        return request->error;

    if (request->reply_buffer_size != sizeof (context->reply))
    {
        abort_connection (context->mount_data->conn);
        return EIO;
    }

    memcpy (&context->reply, request->reply_buffer, sizeof (context->reply));

    if (usfs_create_attr_out_valid (&context->reply, context->request_body.activation))
        return 0;

    abort_connection (context->mount_data->conn);
    return EIO;
}

static int send_create_attributes_request (struct create_attributes_context * context)
{
    struct usfs_request * request = NULL;
    int rc = allocate_create_attributes_request (context, &request);
    if (rc != 0)
        return rc;

    rc = usfs_call (context->mount_data->conn, request);
    if (rc != 0)
        return rc;

    rc = validate_create_attributes_reply (context, request);
    free_request (request);

    return rc;
}

static void release_failed_create_reply (const struct create_attributes_context * context)
{
    if (context->creation_action == VC_OPEN)
    {
        _send_release (context->mount_data, context->reply.nodeid, context->reply.fh, context->open_flags, 0, context->credentials);
    }

    (void)usfs_forget (context->mount_data->conn, context->reply.nodeid, 1);
}

static int publish_created_node (struct create_attributes_context * context)
{
    if (context->creation_action == VC_DEFAULT)
        return 0;

    const int rc =
        commit_created_node (context->mount_data, context->directory_node, &context->reply, context->prepared_node, &context->created_node);

    if (rc != 0)
    {
        release_failed_create_reply (context);
        return rc;
    }

    context->prepared_node = NULL;
    *context->result_vnode = context->created_node->vn;
    return 0;
}

static void attach_create_open_state (struct create_attributes_context * context)
{
    if (context->open_state == NULL)
        return;

    context->open_state->fh = context->reply.fh;
    attach_open_state (context->created_node, context->open_state);
    *context->file_info = (caddr_t)context->open_state;
    context->open_state = NULL;
}

static void discard_create_resources (struct create_attributes_context * context)
{
    write_synchronized_with (global_lock)
    {
        _discard_unpublished_node (context->prepared_node);
    }

    usfs_open_state_discard (context->open_state);
    context->prepared_node = NULL;
    context->open_state = NULL;
}

static int perform_create_attributes (struct create_attributes_context * context)
{
    int rc = prepare_create_resources (context);
    if (rc != 0)
        return rc;

    rc = send_create_attributes_request (context);
    if (rc != 0)
        return rc;

    rc = publish_created_node (context);
    if (rc != 0)
        return rc;

    attach_create_open_state (context);
    return 0;
}

static int create_attributes_entry (struct create_attributes_context * context)
{
    const int rc = perform_create_attributes (context);
    if (rc != 0)
        discard_create_resources (context);

    return rc;
}

static int create_attributes_entry_locked (struct create_attributes_context * context)
{
    if (!context->mount_data->writable)
        return EROFS;

    const int rc = gn_access (context->directory_vnode, W_ACC | X_ACC, ACC_SELF, context->credentials);
    if (rc != 0)
        return rc;

    return create_attributes_entry (context);
}

int gn_create_attr (
    struct vnode * directory_vnode,
    struct vnode ** result_vnode,
    int32long64_t open_flags,
    char * entry_name,
    struct vattr * attributes,
    int32long64_t creation_action,
    caddr_t * file_info,
    struct ucred * credentials
)
{
    struct create_attributes_context context = { 0 };

    context.directory_vnode = directory_vnode;
    context.result_vnode = result_vnode;
    context.file_info = file_info;
    context.entry_name = entry_name;
    context.attributes = attributes;
    context.credentials = credentials;
    context.open_flags = open_flags;
    context.creation_action = creation_action;

    initialize_create_results (&context);

    int rc = validate_create_arguments (&context);
    if (rc != 0)
        return rc;

    rc = prepare_create_attributes_request (&context);
    if (rc != 0)
        return rc;

    rc = authorize_create_identity (&context);
    if (rc != 0)
        return rc;

    rc = resolve_create_parent (&context);
    if (rc != 0)
        return rc;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&context.mount_data->namespace_lock);

    return create_attributes_entry_locked (&context);
}

#endif
