// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_SYMLINK_H
#define USFS_FS_OPERATIONS_VNODE_SYMLINK_H

int gn_symlink (struct vnode * directory_vnode, char * link_name, char * link_target, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (directory_vnode);
    if (mount_data == NULL)
        return EIO;

    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&mount_data->namespace_lock);

    const int access_rc = gn_access (directory_vnode, W_ACC | X_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    if (link_name == NULL || link_name[0] == '\0' || link_target == NULL)
        return EINVAL;

    const uint32_t link_name_length = calculate_bounded_opcode_specific_argument_length (link_name, USFS_MAX_NAME, USFS_MAX_NAME);
    const uint32_t link_target_length = calculate_bounded_opcode_specific_argument_length (link_target, USFS_MAX_LINK, USFS_MAX_LINK);

    if (link_name_length == 0 || link_target_length == 0)
        return ENAMETOOLONG;

    struct usfs_symlink_in request_body = { 0 };
    request_body.namelen = link_name_length;

    return _mutate_entry (
        directory_vnode,
        USFS_OP_SYMLINK,
        &request_body,
        sizeof (request_body),
        link_name,
        link_target,
        link_target_length,
        credentials
    );
}

#endif
