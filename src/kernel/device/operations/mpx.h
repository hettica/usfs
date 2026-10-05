// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_MPX_H
#define USFS_DEVICE_MPX_H

#include "definitions.h"
#include "device/protocol/request_state.h"

static int deallocate_channel (const chan_t channel)
{
    struct usfs_connection * connection = NULL;

    if (channel == USFS_STATUS_CHANNEL)
    {
        synchronized_with (g_lifecycle_lock)
        {
            g_status_channel_present = false;
        }

        return 0;
    }

    if (!is_channel_valid (channel))
        return EINVAL;

    synchronized_with (g_connections_table_lock)
    {
        connection = g_connections[channel];
        g_connections[channel] = NULL;
    }

    if (connection == NULL)
        return EINVAL;

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_CONNECTION,
        USFS_TRACE_ACTION_DEALLOCATE,
        channel,
        connection->state,
        connection->outstanding_requests,
        0
    );

    const int checkpoint_rc = usfs_checkpoint (USFS_INSTRUMENT_CHANNEL_UNPUBLISHED);

    release_connection (connection);

    return checkpoint_rc;
}

static struct usfs_connection * create_connection (void)
{
    static const int alignment = 4;
    static const short connection_lock_class = SHRT_MAX - 4;
    static uint64_t cookie_state = 0x9E3779B97F4A7C15ull;

    struct usfs_connection * connection = usfs_kmalloc (USFS_ALLOC_CONNECTION, sizeof (*connection), alignment, kernel_heap);
    if (connection == NULL)
        return NULL;

    memset (connection, 0, sizeof (*connection));
    lock_alloc (&connection->lock, LOCK_ALLOC_PAGED, connection_lock_class, -1);
    simple_lock_init (&connection->lock);

    connection->state = USFS_CONN_ACTIVE;
    connection->refs_counter = 1;
    connection->next_request_number = 1;
    connection->read_queue_event = EVENT_NULL;

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_CONNECTION,
        USFS_TRACE_ACTION_ALLOCATE,
        -1,
        connection->state,
        0,
        0
    );

    // Create a mount cookie (simple pseudo-random number generator, is not cryptographically secure)
    cookie_state = cookie_state * 6364136223846793005ull + 1442695040888963407ull;
    connection->cookie = cookie_state ^ (unsigned long)connection;
    return connection;
}

static void free_connection (struct usfs_connection * connection)
{
    lock_free (&connection->lock);
    xmfree (connection, kernel_heap);
}

static int admit_connection (struct usfs_connection * connection, chan_t * output_channel)
{
    synchronized_with (g_lifecycle_lock)
    {
        // Admit only while the lifecycle gate remains open
        if (!g_gate_is_open || g_kext_is_running_control_operation || usfs_lifecycle_state_read () != USFS_KEXT_ACTIVE)
            return EBUSY;

        synchronized_with (g_connections_table_lock)
        {
            for (int i = 0; i < USFS_MAX_CONNECTIONS; i++)
            {
                if (g_connections[i] != NULL)
                    continue;

                // Initialize connection from the current runtime defaults
                connection->channel = i;
                connection->request_timeout_ms = g_usfs_runtime_limits.request_timeout_ms;
                connection->pager_timeout_ms = g_usfs_runtime_limits.pager_timeout_ms;
                connection->max_outstanding_requests = g_usfs_runtime_limits.max_outstanding_requests;
                g_connections[i] = connection;
                *output_channel = i;
                return 0;
            }
        }
    }

    return EBUSY;
}

static int handle_admission_failure (const int admission_rc, struct usfs_connection * connection)
{
    if (admission_rc == 0)
        return 0;

    if (admission_rc == EBUSY)
    {
        const int checkpoint_rc = usfs_checkpoint (USFS_INSTRUMENT_CHANNEL_REJECTED);
        if (checkpoint_rc != 0)
        {
            free_connection (connection);
            return checkpoint_rc;
        }
    }

    free_connection (connection);
    return admission_rc;
}

static int process_mpx (const dev_t device_number, chan_t * output_value, const char * channel_name)
{
    ignore_parameter device_number; //! todo: reject not owning devices

    if (output_value == NULL)
        return EINVAL;

    // Per kernel contract, passing NULL means deallocate existing channel
    if (channel_name == NULL)
        return deallocate_channel (*output_value);

    // The status channel has no backend connection and does not consume a daemon slot.
    if (strcmp (channel_name, "status") == 0)
    {
        synchronized_with (g_lifecycle_lock)
        {
            if (!g_gate_is_open || g_kext_is_running_control_operation || usfs_lifecycle_state_read () != USFS_KEXT_ACTIVE)
                return EBUSY;

            g_status_channel_present = true;
            *output_value = USFS_STATUS_CHANNEL;
            return 0;
        }
    }

    // Backend channels retain strict one-open-one-connection semantics.
    if (channel_name[0] != '\0')
        return EINVAL;

    const chan_t invalid_channel = -1;
    *output_value = invalid_channel;

    struct usfs_connection * const connection = create_connection ();
    if (connection == NULL)
        return ENOMEM;
    connection->device_number = device_number;

    // Run admission hook before publishing the connection
    {
        const int checkpoint_rc = usfs_checkpoint (USFS_INSTRUMENT_CHANNEL_PRE_ADMISSION);
        if (checkpoint_rc != 0)
        {
            free_connection (connection);
            return checkpoint_rc;
        }
    }

    chan_t channel = invalid_channel;
    const int admission_rc = admit_connection (connection, &channel);

    if (admission_rc != 0)
        return handle_admission_failure (admission_rc, connection);

    *output_value = channel;

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_CONNECTION,
        USFS_TRACE_ACTION_ADMIT,
        connection->channel,
        connection->state,
        connection->max_outstanding_requests,
        0
    );

    return 0;
}

static int usfs_dev_mpx (const dev_t device_number, chan_t * output_value, const char * channel_name)
{
    if (output_value == NULL)
        return EINVAL;

    const int is_deallocation = channel_name == NULL;
    const int admission_rc = usfs_lifecycle_mpx_enter (is_deallocation);
    if (admission_rc != 0)
        return admission_rc;

    const int rc = process_mpx (device_number, output_value, channel_name);

    usfs_lifecycle_mpx_leave ();

    return rc;
}

#endif
