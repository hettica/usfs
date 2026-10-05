// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_CONNECTION_REFS_H
#define USFS_CONNECTION_REFS_H

#include "definitions.h"

void acquire_connection (struct usfs_connection * conn)
{
    synchronized_with (g_connections_table_lock)
    {
        conn->refs_counter += 1;
    }
}

void release_connection (struct usfs_connection * connection)
{
    int is_last_reference = false;

    synchronized_with (g_connections_table_lock)
    {
        connection->refs_counter -= 1;
        is_last_reference = connection->refs_counter == 0;
    }

    if (is_last_reference)
    {
        lock_free (&connection->lock);
        xmfree (connection, kernel_heap);
    }
}

#endif
