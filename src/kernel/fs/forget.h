// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_FORGET_H
#define USFS_FS_FORGET_H

static int usfs_forget (struct usfs_connection * connection, const uint64_t node_id, const uint64_t lookup_count)
{
    if (connection == NULL)
        return 0;

    if (lookup_count == 0)
        return 0;

    int is_active = false;

    synchronized_with (connection->lock)
    {
        is_active = connection->state == USFS_CONN_ACTIVE;
    }

    if (!is_active)
        return 0;

    struct usfs_forget_in body;

    body.count = lookup_count;

    const int rc = _call_conn_no_reply (connection, USFS_OP_FORGET, node_id, &body, sizeof (body), NULL);
    if (rc != 0)
        abort_connection (connection);

    return rc;
}

#endif
