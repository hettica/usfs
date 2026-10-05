// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_HOLD_H
#define USFS_FS_OPERATIONS_VNODE_HOLD_H

int gn_hold (struct vnode * file_vnode)
{
    write_synchronized_with (global_lock)
    {
        usfs_vnode_hold_locked (file_vnode, USFS_INSTRUMENT_NODE_REUSE_NONE);
    }

    return 0;
}

#endif
