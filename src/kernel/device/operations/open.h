// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_OPEN_H
#define USFS_DEVICE_OPEN_H

#include "definitions.h"

static int usfs_dev_open (const dev_t device_number, const ulong open_flags, const chan_t channel, const int is_called_from_kernel_extension)
{
    ignore_parameter device_number;                   //! todo: validate this parameter, reject open for not owning devices
    ignore_parameter open_flags;                      //! todo: reject unsupported open flags
    ignore_parameter is_called_from_kernel_extension; //! todo: research what this affects, decide if we should handle this parameter

    if (channel == USFS_STATUS_CHANNEL)
        return 0;

    const struct usfs_connection * connection = get_connection_by_channel (channel);

    if (connection == NULL || connection->state != USFS_CONN_ACTIVE)
        return ENXIO;

    return 0;
}

#endif
