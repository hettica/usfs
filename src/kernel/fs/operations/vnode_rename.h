// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_RENAME_H
#define USFS_FS_OPERATIONS_VNODE_RENAME_H

static int check_rename_access (struct vnode * source_directory_vnode, struct vnode * destination_directory_vnode, struct ucred * credentials)
{
    const int rc = gn_access (source_directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
    if (rc != 0 || destination_directory_vnode == source_directory_vnode)
        return rc;

    return gn_access (destination_directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
}

int gn_rename (
    struct vnode * source_vnode,
    struct vnode * source_directory_vnode,
    caddr_t old_name,
    struct vnode * destination_vnode,
    struct vnode * destination_directory_vnode,
    caddr_t new_name,
    struct ucred * credentials
)
{
    ignore_parameter source_vnode;
    ignore_parameter destination_vnode;

    struct usfs_mount_data * mount = _mount_of (source_directory_vnode);
    if (mount == NULL)
        return EIO;

    struct usfs_complex_lock_guard guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&mount->namespace_lock);

    const int access_rc = check_rename_access (source_directory_vnode, destination_directory_vnode, credentials);
    if (access_rc != 0)
        return access_rc;

    int sticky_rc = usfs_check_sticky_entry (source_directory_vnode, old_name, credentials, 0);
    if (sticky_rc == 0)
        sticky_rc = usfs_check_sticky_entry (destination_directory_vnode, new_name, credentials, 1);

    if (sticky_rc != 0)
        return sticky_rc;


    const struct usfs_node * destdir = _node_of (destination_directory_vnode);
    if (destdir == NULL)
        return EIO;

    if (old_name == NULL || old_name[0] == '\0' || new_name == NULL || new_name[0] == '\0')
        return EINVAL;

    const uint32_t old_name_len = calculate_bounded_opcode_specific_argument_length (old_name, USFS_MAX_NAME, USFS_MAX_NAME);
    const uint32_t new_name_len = calculate_bounded_opcode_specific_argument_length (new_name, USFS_MAX_NAME, USFS_MAX_NAME);

    if (old_name_len == 0 || new_name_len == 0)
        return ENAMETOOLONG;

    struct usfs_rename_in request_body = { 0 };
    request_body.newparent = destdir->nodeid;
    request_body.oldnamelen = old_name_len;

    // The old name travels in the name slot and the new one in the payload,
    // which lays them reply back to back exactly as usfs_rename_in describes.
    // Replacing an existing destination is the daemon's job; destination_vnode is only
    // the logical file system telling us it found one.
    return _mutate_entry (
        source_directory_vnode,
        USFS_OP_RENAME,
        &request_body,
        sizeof (request_body),
        old_name,
        new_name,
        new_name_len,
        credentials
    );
}

#endif
