// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_OPEN_H
#define USFS_DEVICE_OPEN_H

#include "definitions.h"

static int usfs_dev_open (const dev_t device_number, const ulong open_flags, const chan_t channel, const int extension)
{
    if (!usfs_device_number_valid (device_number))
        return ENXIO;
    /* USFS has no openx extension or kernel-buffer transport. Access and
       nonblocking flags remain governed by LFS and the read entry point. */
    if (extension != 0 || (open_flags & (DKERNEL | DAPPEND)) != 0)
        return EINVAL;

    if (channel == USFS_STATUS_CHANNEL)
        return 0;

    const struct usfs_connection * connection = get_connection_by_channel (channel);

    if (connection == NULL || connection->state != USFS_CONN_ACTIVE)
        return ENXIO;

    return 0;
}

#endif
