// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"

#include <limits.h>

/* Shared kernel extension lifecycle state. */

struct usfs_connection * g_connections[USFS_MAX_CONNECTIONS];
Simple_lock g_connections_table_lock;
Simple_lock g_lifecycle_lock;
int g_status_channel_present = false;
int g_mount_count = 0;
static int g_kext_state = USFS_KEXT_DOWN;
int g_gate_is_open = false;
int g_kext_is_running_control_operation = false;

enum usfs_mpx_guard_state
{
    USFS_MPX_GUARD_OPEN = 0,
    USFS_MPX_GUARD_CLOSING = 1,
    USFS_MPX_GUARD_SEALED = 2,
    USFS_MPX_GUARD_STATE_MASK = 3,
    USFS_MPX_GUARD_REFERENCE = 4
};

/* Keep the callback guard available until the extension itself is unpinned. */
static int g_mpx_guard = USFS_MPX_GUARD_SEALED;
struct usfs_runtime_limits g_usfs_runtime_limits = { USFS_DEFAULT_REQUEST_TIMEOUT_MS, USFS_DEFAULT_PAGER_TIMEOUT_MS, USFS_DEFAULT_MAX_OUTSTANDING };

struct vnodeops gn_ops;
struct vfsops vfsops;
struct gfs gfs;

Complex_lock global_lock;

struct usfs_private_data usfs_private_data;

int usfs_lifecycle_state_read (void)
{
    int observed_state = USFS_KEXT_DOWN;

    while (!compare_and_swap (&g_kext_state, &observed_state, observed_state))
    {
    }

    return observed_state;
}

int usfs_lifecycle_state_replace (const int desired_state)
{
    int observed_state = usfs_lifecycle_state_read ();

    while (!compare_and_swap (&g_kext_state, &observed_state, desired_state))
    {
    }

    return observed_state;
}

int usfs_lifecycle_state_claim (const int expected_state, const int desired_state)
{
    int observed_state = expected_state;

    return compare_and_swap (&g_kext_state, &observed_state, desired_state);
}

static int read_mpx_guard (void)
{
    int observed_guard = 0;

    while (!compare_and_swap (&g_mpx_guard, &observed_guard, observed_guard))
    {
    }

    return observed_guard;
}

int usfs_lifecycle_mpx_enter (const int is_deallocation)
{
    for (;;)
    {
        int observed_guard = read_mpx_guard ();
        const int guard_state = observed_guard & USFS_MPX_GUARD_STATE_MASK;

        if (guard_state == USFS_MPX_GUARD_SEALED)
            return EBUSY;

        if (!is_deallocation && guard_state != USFS_MPX_GUARD_OPEN)
            return EBUSY;

        if (observed_guard > INT_MAX - USFS_MPX_GUARD_REFERENCE)
            return EBUSY;

        const int desired_guard = observed_guard + USFS_MPX_GUARD_REFERENCE;

        if (compare_and_swap (&g_mpx_guard, &observed_guard, desired_guard))
            return 0;
    }
}

void usfs_lifecycle_mpx_leave (void)
{
    for (;;)
    {
        int observed_guard = read_mpx_guard ();

        if (observed_guard < USFS_MPX_GUARD_REFERENCE)
            return;

        const int desired_guard = observed_guard - USFS_MPX_GUARD_REFERENCE;

        if (compare_and_swap (&g_mpx_guard, &observed_guard, desired_guard))
            return;
    }
}

int usfs_lifecycle_mpx_begin_close (void)
{
    for (;;)
    {
        int observed_guard = read_mpx_guard ();
        const int guard_state = observed_guard & USFS_MPX_GUARD_STATE_MASK;

        if (guard_state == USFS_MPX_GUARD_SEALED)
            return 0;

        if (guard_state != USFS_MPX_GUARD_OPEN)
            return EBUSY;

        const int desired_guard = (observed_guard & ~USFS_MPX_GUARD_STATE_MASK) | USFS_MPX_GUARD_CLOSING;

        if (compare_and_swap (&g_mpx_guard, &observed_guard, desired_guard))
            return 0;
    }
}

int usfs_lifecycle_mpx_has_users (void)
{
    return (read_mpx_guard () & ~USFS_MPX_GUARD_STATE_MASK) != 0;
}

int usfs_lifecycle_mpx_seal (void)
{
    int expected_guard = USFS_MPX_GUARD_CLOSING;

    if (compare_and_swap (&g_mpx_guard, &expected_guard, USFS_MPX_GUARD_SEALED))
        return true;

    return expected_guard == USFS_MPX_GUARD_SEALED;
}

void usfs_lifecycle_mpx_reopen (void)
{
    for (;;)
    {
        int observed_guard = read_mpx_guard ();
        const int desired_guard = observed_guard & ~USFS_MPX_GUARD_STATE_MASK;

        if (compare_and_swap (&g_mpx_guard, &observed_guard, desired_guard))
            return;
    }
}

int usfs_lifecycle_mpx_activate (void)
{
    int expected_guard = USFS_MPX_GUARD_SEALED;

    return compare_and_swap (&g_mpx_guard, &expected_guard, USFS_MPX_GUARD_OPEN) ? 0 : EBUSY;
}

int has_active_connections (void)
{
    for (int connection_index = 0; connection_index < USFS_MAX_CONNECTIONS; connection_index++)
    {
        if (g_connections[connection_index] != NULL)
            return true;
    }

    return false;
}

static int has_collector_connection_locked (const chan_t collector_channel)
{
    if (collector_channel < 0)
        return false;

    if (collector_channel >= USFS_MAX_CONNECTIONS)
        return false;

    return g_connections[(int)collector_channel] != NULL;
}

static int validate_control_connections_locked (const chan_t collector_channel, const enum usfs_control_policy policy)
{
    if (!has_collector_connection_locked (collector_channel))
        return policy == USFS_CONTROL_ONLY_COLLECTOR_CONNECTION ? ENXIO : EBUSY;

    int connection_count = 0;

    for (int connection_index = 0; connection_index < USFS_MAX_CONNECTIONS; connection_index++)
    {
        if (g_connections[connection_index] == NULL)
            continue;

        if (policy == USFS_CONTROL_EXACTLY_ONE_CONNECTION)
        {
            connection_count += 1;
            continue;
        }

        if (connection_index != (int)collector_channel)
            return EBUSY;
    }

    if (policy != USFS_CONTROL_EXACTLY_ONE_CONNECTION)
        return 0;

    if (connection_count != 1)
        return EBUSY;

    return 0;
}

static int can_admit_lifecycle_operation_locked (void)
{
    if (!g_gate_is_open)
        return false;

    if (g_kext_is_running_control_operation)
        return false;

    return usfs_lifecycle_state_read () == USFS_KEXT_ACTIVE;
}

static int enter_control_operation_locked (const chan_t collector_channel, const enum usfs_control_policy policy)
{
    if (!can_admit_lifecycle_operation_locked ())
        return EBUSY;

    if (g_mount_count != 0)
        return EBUSY;

    int rc = 0;

    synchronized_with (g_connections_table_lock)
    {
        rc = validate_control_connections_locked (collector_channel, policy);
    }

    if (rc != 0)
        return rc;

    g_kext_is_running_control_operation = true;

    return 0;
}

int usfs_lifecycle_control_enter (const chan_t collector_channel, const enum usfs_control_policy policy)
{
    int rc = 0;

    synchronized_with (g_lifecycle_lock)
    {
        rc = enter_control_operation_locked (collector_channel, policy);
    }

    return rc;
}

void usfs_lifecycle_control_leave (void)
{
    synchronized_with (g_lifecycle_lock)
    {
        g_kext_is_running_control_operation = false;
    }
}

static struct usfs_connection * reserve_connection_mount_locked (const uint64_t channel, const uint64_t cookie, int * rc)
{
    struct usfs_connection * connection = NULL;

    if (channel < USFS_MAX_CONNECTIONS)
        connection = g_connections[(unsigned)channel];

    if (connection == NULL)
        return NULL;

    if (connection->cookie != cookie)
        return NULL;

    if (connection->state != USFS_CONN_ACTIVE)
        return NULL;

    if (connection->mounts_counter != 0)
    {
        *rc = EBUSY;
        return NULL;
    }

    connection->refs_counter += 1;
    connection->mounts_counter += 1;
    g_mount_count += 1;

    return connection;
}

static int reserve_mount_locked (const uint64_t channel, const uint64_t cookie, struct usfs_connection ** connection_out)
{
    if (!can_admit_lifecycle_operation_locked ())
        return EBUSY;

    int rc = 0;

    synchronized_with (g_connections_table_lock)
    {
        *connection_out = reserve_connection_mount_locked (channel, cookie, &rc);
    }

    if (rc != 0)
        return rc;

    if (*connection_out == NULL)
        return EINVAL;

    return 0;
}

int usfs_lifecycle_reserve_mount (const uint64_t channel, const uint64_t cookie, struct usfs_connection ** connection_out)
{
    struct usfs_connection * connection = NULL;
    int rc = 0;

    synchronized_with (g_lifecycle_lock)
    {
        rc = reserve_mount_locked (channel, cookie, &connection);
    }

    *connection_out = connection;

    return rc;
}

static void release_mount_locked (struct usfs_connection * connection, const int close_connection)
{
    if (connection != NULL)
    {
        if (connection->mounts_counter > 0)
            connection->mounts_counter -= 1;
    }

    if (g_mount_count > 0)
        g_mount_count -= 1;

    if (!close_connection)
        return;

    if (connection == NULL)
        return;

    synchronized_with (connection->lock)
    {
        if (connection->state == USFS_CONN_ACTIVE)
            connection->state = USFS_CONN_CLOSED;
    }
}

static void release_mount (struct usfs_connection * connection, const int close_connection)
{
    synchronized_with (g_lifecycle_lock)
    {
        synchronized_with (g_connections_table_lock)
        {
            release_mount_locked (connection, close_connection);
        }
    }

    on_mount_released (connection);

    if (!close_connection)
        return;

    if (connection == NULL)
        return;

    wake_closed_connection (connection);
}

void usfs_lifecycle_release_mount (struct usfs_connection * connection)
{
    release_mount (connection, false);
}

void usfs_lifecycle_finish_mount (struct usfs_connection * connection)
{
    release_mount (connection, true);
}
