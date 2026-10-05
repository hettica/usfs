// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_CREATE_H
#define USFS_FS_OPERATIONS_VNODE_CREATE_H

static struct usfs_node * find_created_node (struct usfs_mount_data * mount_data, uint64_t node_id)
{
    struct usfs_node * node = mount_data->nodes;

    while (node != NULL && node->nodeid != node_id)
    {
        node = node->next;
    }

    return node;
}

static void hold_created_node (struct usfs_node * node)
{
    usfs_vnode_hold_locked (node->vn, USFS_INSTRUMENT_NODE_REUSE_CREATE);
}

static int commit_created_node (
    struct usfs_mount_data * mount_data,
    const struct usfs_node * directory_node,
    const struct usfs_create_out * reply,
    struct usfs_node * prepared,
    struct usfs_node ** result_node
)
{
    struct usfs_node * node = NULL;

    write_synchronized_with (global_lock)
    {
        node = find_created_node (mount_data, reply->nodeid);

        if (node != NULL)
        {
            if (node->lookup_refs == UINT64_MAX)
                return EOVERFLOW;

            hold_created_node (node);
        }
        else
        {
            prepared->gn->gn_type = usfs_vtype_from_mode (reply->attr.mode);
            _publish_node (mount_data, prepared, reply->nodeid, directory_node->nodeid);
            node = prepared;
            prepared = NULL;
        }

        _discard_unpublished_node (prepared);
        node->lookup_refs++;
    }

    *result_node = node;
    return 0;
}

int gn_create (
    struct vnode * directory_vnode,
    struct vnode ** result_vnode,
    int32long64_t open_flags,
    caddr_t entry_name,
    int32long64_t file_mode,
    caddr_t * file_info,
    struct ucred * credentials
)
{
    struct vattr attributes = { 0 };

    attributes.va_mask = AT_TYPE | AT_MODE;
    attributes.va_type = VREG;
    attributes.va_mode = (mode_t)file_mode;

    return gn_create_attr (directory_vnode, result_vnode, open_flags, (char *)entry_name, &attributes, VC_OPEN, file_info, credentials);
}

#endif
