// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SELECT_H
#define USFS_FS_OPERATIONS_NOT_IMPLEMENTED_VNODE_SELECT_H

int gn_select (
    struct vnode * file_vnode,
    int32long64_t correlation_id,
    ushort event,
    ushort * ready_events,
    void (*notify_function) (),
    caddr_t vnode_info,
    struct ucred * credentials
)
{
    ignore_parameter file_vnode;
    ignore_parameter correlation_id;
    ignore_parameter event;
    ignore_parameter ready_events;
    ignore_parameter notify_function;
    ignore_parameter vnode_info;
    ignore_parameter credentials;

    return ENOSYS;
}

#endif
