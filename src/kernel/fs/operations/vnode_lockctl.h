// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_LOCKCTL_H
#define USFS_FS_OPERATIONS_VNODE_LOCKCTL_H

int gn_lockctl (
    struct vnode * file_vnode,
    offset_t file_offset,
    struct eflock * lock_data,
    int32long64_t command,
    int (*retry_function) (),
    ulong * retry_id,
    struct ucred * credentials
)
{
    if (file_vnode == NULL || file_vnode->v_gnode == NULL || lock_data == NULL)
        return EINVAL;

    if (file_vnode->v_gnode->gn_type != VREG)
        return EINVAL;

    /* USFS does not hold an inode lock here. Never pass INOFLCK without the
     * release/reacquire callbacks required when a blocking lock sleeps. */
    if ((command & ~(SETFLCK | SLPFLCK | TSTFLCK)) != 0 || ((command & SLPFLCK) != 0 && (command & SETFLCK) == 0))
        return EINVAL;

    offset_t file_size = 0;
    if (lock_data->l_whence == SEEK_END)
    {
        struct vattr attributes;
        const int rc = gn_getattr (file_vnode, &attributes, credentials);
        if (rc != 0)
            return rc;
        file_size = attributes.va_size;
    }

    return common_reclock (
        file_vnode->v_gnode, file_size, file_offset, lock_data, (int)command, retry_function, retry_id, NULL, NULL
    );
}

#endif
