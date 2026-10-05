// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_SELECT_H
#define USFS_DEVICE_SELECT_H

#include "definitions.h"

static int usfs_dev_select (const dev_t device_number, const int promoted_events, ushort * returned, const int channel)
{
    const ushort events = (ushort)promoted_events;
    struct usfs_connection * connection = get_connection_by_channel (channel);
    if (connection == NULL || connection->device_number != device_number)
        return ENXIO;

    if ((events & ~(POLLIN | POLLOUT | POLLPRI | POLLHUP | POLLERR | POLLSYNC)) != 0)
        return EINVAL;

    synchronized_with (connection->lock)
    {
        if ((events & POLLIN) && connection->pending_requests_head != NULL)
            *returned |= POLLIN;
        if (connection->state == USFS_CONN_CLOSED)
            *returned |= events & (POLLIN | POLLHUP);
        else if (connection->state != USFS_CONN_ACTIVE)
            *returned |= events & (POLLIN | POLLPRI | POLLHUP | POLLERR);
        if (events & POLLOUT)
            *returned |= POLLOUT;
        if (*returned == 0 && !(events & POLLSYNC))
            connection->select_events |= events & (POLLIN | POLLPRI | POLLHUP | POLLERR);
    }

    return 0;
}

#endif
