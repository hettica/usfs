// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_CLOSE_H
#define USFS_DEVICE_CLOSE_H

#include "definitions.h"

static int usfs_dev_close (const dev_t device_number, const chan_t channel)
{
    ignore_parameter device_number; //! todo: validate this parameter, reject close for not owning devices

    struct usfs_connection * connection = get_connection_by_channel (channel);

    if (connection == NULL)
        return 0;

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_CONNECTION,
        USFS_TRACE_ACTION_CLOSE,
        channel,
        connection->state,
        connection->outstanding_requests,
        0
    );

    int orderly = 0;

    synchronized_with (connection->lock)
    {
        orderly = connection->state == USFS_CONN_CLOSED;
    }

    if (orderly)
        return 0;

    report_connection_lost (connection);
    abort_connection (connection);

    return 0;
}

#endif
