// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_SETATTR_H
#define USFS_FS_OPERATIONS_VNODE_SETATTR_H

#include "vnode_getattr.h"

struct attribute_change_context
{
    struct vnode * file_vnode;           // Vnode whose attributes will change.
    struct usfs_mount_data * mount_data; // Mounted instance used for protocol traffic.
    struct usfs_node * node;             // USFS node corresponding to file_vnode.
    struct ucred * credentials;          // Credentials used for authorization.
    struct vattr current_attributes;     // Attributes used for authorization decisions.
    struct usfs_setattr_in request_body; // Validated mutation sent to the daemon.
    int32long64_t command;               // AIX setattr command selector.
    int32long64_t argument1;             // First command-specific AIX argument.
    int32long64_t argument2;             // Second command-specific AIX argument.
    int32long64_t argument3;             // Third command-specific AIX argument.
    int is_owner;                        // Whether credentials identify the file owner.
    int can_set_dac;                     // Whether credentials may bypass DAC ownership.
};

static int has_owner_or_dac_authority (const struct attribute_change_context * context)
{
    return context->is_owner || context->can_set_dac;
}

static int can_assign_group (const struct attribute_change_context * context, const gid_t requested_group_id)
{
    if (requested_group_id == context->current_attributes.va_gid)
        return true;

    if (context->can_set_dac)
        return true;

    if (!context->is_owner)
        return false;

    return groupmember_cr (requested_group_id, context->credentials);
}

static void remove_setid_bits_after_ownership_change (struct attribute_change_context * context)
{
    if (context->can_set_dac)
        return;

    if (context->request_body.valid == 0)
        return;

    if ((context->current_attributes.va_mode & (S_ISUID | S_ISGID)) == 0)
        return;

    context->request_body.valid |= USFS_SET_MODE;
    context->request_body.mode = (uint32_t)(context->current_attributes.va_mode & ~(S_ISUID | S_ISGID));
}

static int prepare_user_id_change (struct attribute_change_context * context)
{
    if ((context->argument1 & T_OWNER_AS_IS) != 0)
        return 0;

    const uid_t requested_user_id = (uid_t)context->argument2;

    if (requested_user_id != context->current_attributes.va_uid)
    {
        if (!context->can_set_dac)
            return EPERM;
    }

    context->request_body.valid |= USFS_SET_UID;
    context->request_body.uid = (uint32_t)requested_user_id;

    return 0;
}

static int prepare_group_id_change (struct attribute_change_context * context)
{
    if ((context->argument1 & T_GROUP_AS_IS) != 0)
        return 0;

    const gid_t requested_group_id = (gid_t)context->argument3;

    if (!can_assign_group (context, requested_group_id))
        return EPERM;

    context->request_body.valid |= USFS_SET_GID;
    context->request_body.gid = (uint32_t)requested_group_id;

    return 0;
}

static int prepare_owner_change (struct attribute_change_context * context)
{
    const int32long64_t unchanged_ownership_flags = T_OWNER_AS_IS | T_GROUP_AS_IS;

    if ((context->argument1 & unchanged_ownership_flags) == unchanged_ownership_flags)
        return 0;

    if (!has_owner_or_dac_authority (context))
        return EPERM;

    const int user_id_rc = prepare_user_id_change (context);
    if (user_id_rc != 0)
        return user_id_rc;

    const int group_id_rc = prepare_group_id_change (context);
    if (group_id_rc != 0)
        return group_id_rc;

    remove_setid_bits_after_ownership_change (context);

    return 0;
}

static void copy_attribute_time (struct usfs_setattr_in * request_body, const uint32_t time_attribute_flag, const struct timestruc_t * timestamp)
{
    if (timestamp == NULL)
        return;

    request_body->valid |= time_attribute_flag;

    if (time_attribute_flag == USFS_SET_ATIME)
    {
        request_body->atime = (int64_t)timestamp->tv_sec;
        request_body->atimensec = (uint32_t)timestamp->tv_nsec;

        return;
    }

    if (time_attribute_flag == USFS_SET_MTIME)
    {
        request_body->mtime = (int64_t)timestamp->tv_sec;
        request_body->mtimensec = (uint32_t)timestamp->tv_nsec;

        return;
    }

    request_body->ctime = (int64_t)timestamp->tv_sec;
    request_body->ctimensec = (uint32_t)timestamp->tv_nsec;
}

static int requests_current_file_times (const struct attribute_change_context * context)
{
    if (context->command != V_UTIME)
        return false;

    return (context->argument1 & T_SETTIME) != 0;
}

static int prepare_current_time_change (struct attribute_change_context * context)
{
    if (!has_owner_or_dac_authority (context))
    {
        const int rc = access_for_subject (&context->current_attributes, W_ACC, ACC_SELF, context->credentials);
        if (rc != 0)
            return rc;
    }

    context->request_body.valid = USFS_SET_TIMES_NOW | USFS_SET_ATIME | USFS_SET_MTIME;

    return 0;
}

static int prepare_explicit_time_change (struct attribute_change_context * context)
{
    if (context->command == V_UTIME)
    {
        copy_attribute_time (&context->request_body, USFS_SET_ATIME, (const struct timestruc_t *)context->argument2);
        copy_attribute_time (&context->request_body, USFS_SET_MTIME, (const struct timestruc_t *)context->argument3);

        return 0;
    }

    if (context->command == V_STIME)
    {
        if (context->argument3 != 0)
            return EOPNOTSUPP;
    }

    copy_attribute_time (&context->request_body, USFS_SET_ATIME, (const struct timestruc_t *)context->argument1);
    copy_attribute_time (&context->request_body, USFS_SET_MTIME, (const struct timestruc_t *)context->argument2);

    return 0;
}

static int prepare_time_change (struct attribute_change_context * context)
{
    if (requests_current_file_times (context))
        return prepare_current_time_change (context);

    if (!has_owner_or_dac_authority (context))
        return EPERM;

    return prepare_explicit_time_change (context);
}

static int prepare_mode_change (struct attribute_change_context * context)
{
    if (!has_owner_or_dac_authority (context))
        return EPERM;

    context->request_body.valid = USFS_SET_MODE;
    context->request_body.mode = (uint32_t)context->argument1;

    return 0;
}

static int prepare_attribute_change (struct attribute_change_context * context)
{
    switch (context->command)
    {
        case V_MODE:
            return prepare_mode_change (context);

        case V_OWN:
            return prepare_owner_change (context);

        case V_UTIME:
        case V_STIME:
            return prepare_time_change (context);

        default:
            return EINVAL;
    }
}

static int read_current_attributes (struct attribute_change_context * context)
{
    return gn_getattr (context->file_vnode, &context->current_attributes, context->credentials);
}

static int prepare_attribute_change_request (struct attribute_change_context * context)
{
    const int rc = read_current_attributes (context);
    if (rc != 0)
        return rc;

    context->is_owner = context->credentials->cr_uid == context->current_attributes.va_uid;
    context->can_set_dac = privcheck_cr (SET_OBJ_DAC, context->credentials) == 0;

    memset (&context->request_body, 0, sizeof (context->request_body));

    return prepare_attribute_change (context);
}

static int should_reject_setgid_change (const struct attribute_change_context * context)
{
    if (context->command != V_MODE)
        return false;

    if (context->can_set_dac)
        return false;

    if ((context->request_body.mode & S_ISGID) == 0)
        return false;

    return !groupmember_cr (context->current_attributes.va_gid, context->credentials);
}

static int mode_change_conflicts_with_shared_mapping (const struct attribute_change_context * context)
{
    /* namespace_lock excludes mapping admission. Keep this restriction until
     * vnode reclamation: unmap callbacks and pageout are not a notification
     * for each memory store, and cached writes can outlive a descriptor.
     * AIX 7.2 vm_qmodify (p. 591) is only a retrospective query; vm_protectp
     * (p. 590) changes resident pages only, so neither closes the store race. */
    if ((context->request_body.valid & USFS_SET_MODE) == 0)
        return false;

    if (!context->node->cache_shared_writable)
        return false;

    if ((context->request_body.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
        return false;

    return (context->request_body.mode & (S_ISUID | S_ISGID)) != 0;
}

static int apply_prepared_attribute_change (struct attribute_change_context * context)
{
    if (context->request_body.valid == 0)
        return 0;

    const int setgid_rejected = should_reject_setgid_change (context);
    if (setgid_rejected)
        context->request_body.mode &= ~S_ISGID;

    if (mode_change_conflicts_with_shared_mapping (context))
        return EBUSY;

    const int rc = _setattr (context->file_vnode, &context->request_body, 0, context->credentials);
    if (rc != 0)
        return rc;

    if (setgid_rejected)
        return EPERM;

    return 0;
}

static int process_attribute_change_locked (struct attribute_change_context * context)
{
    const int rc = prepare_attribute_change_request (context);
    if (rc != 0)
        return rc;

    return apply_prepared_attribute_change (context);
}

static int set_attributes_locked (struct attribute_change_context * context)
{
    context->node = _node_of (context->file_vnode);
    if (context->node == NULL)
        return EIO;

    return process_attribute_change_locked (context);
}

int gn_setattr (
    struct vnode * file_vnode,
    const int32long64_t command,
    const int32long64_t argument1,
    const int32long64_t argument2,
    const int32long64_t argument3,
    struct ucred * credentials
)
{
    struct attribute_change_context context = {
        .file_vnode = file_vnode,
        .mount_data = _mount_of (file_vnode),
        .credentials = credentials,
        .command = command,
        .argument1 = argument1,
        .argument2 = argument2,
        .argument3 = argument3
    };

    if (context.mount_data == NULL)
        return EIO;

    if (context.credentials == NULL)
        return EIO;

    if (!context.mount_data->writable)
        return EROFS;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&context.mount_data->namespace_lock);

    return set_attributes_locked (&context);
}

#endif
