// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"

#include <sys/time.h>

struct usfs_request_metrics
{
    uint32_t peak_outstanding_requests;           // Largest observed number of simultaneous requests.
    uint64_t num_of_accepted_requests;            // Requests admitted to the transport.
    uint64_t num_of_successful_requests;          // Requests completed successfully.
    uint64_t num_of_rejected_by_daemon_requests;  // Requests completed with a daemon error.
    uint64_t num_of_transport_failures;           // Requests aborted by transport failure.
    uint64_t num_of_timed_out_requests;           // Requests whose deadlines expired.
    uint64_t num_of_rejections_by_queue_capacity; // Requests refused by the outstanding limit.
    uint64_t num_of_interrupted_requests;         // Pending requests canceled by interruption.
    uint64_t num_of_protocol_errors;              // Rejected malformed protocol exchanges.
    struct usfs_latency_stats normal_latency;     // Latency statistics for ordinary requests.
    struct usfs_latency_stats pager_latency;      // Latency statistics for pager requests.
};

struct usfs_lifecycle_metrics
{
    uint32_t num_of_stale_mounts;         // Mounts retaining deferred resources.
    uint32_t num_of_dead_mounted_daemons; // Mounted connections that became unavailable.
    uint64_t num_of_daemon_disconnects;   // Observed daemon disconnects.
    uint64_t num_of_forced_recoveries;    // Forced recovery operations.
};

struct usfs_fault_state
{
    uint32_t fault_class;                                    // Most recently reported fault category.
    int32_t fault_errno;                                     // Error associated with the latest fault.
    int32_t fault_channel;                                   // Connection channel, or -1 when unavailable.
    uint16_t fault_opcode;                                   // Opcode associated with the latest fault.
    uint64_t fault_request_id;                               // Request identifier associated with the latest fault.
    uint64_t fault_time_sec;                                 // Latest fault time in seconds.
    uint64_t ras_time_sec[USFS_FAULT_CLEANUP_REQUIRED + 1u]; // Most recent RAS emission time per fault, in seconds.
};

struct usfs_observability_state
{
    Simple_lock lock;                        // Protects all observability state below
    struct usfs_request_metrics requests;    // Request counters and latency measurements
    struct usfs_lifecycle_metrics lifecycle; // Mount and daemon lifecycle counters
    struct usfs_fault_state last_fault;      // Latest fault and RAS rate-limit state
};

static struct usfs_observability_state g_observability;

static uint64_t saturating_add_u64 (const uint64_t left, const uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint64_t get_current_time_in_ns (void)
{
    const uint64_t ticks = lbolt < 0 ? 0u : (uint64_t)lbolt;
    const uint64_t seconds = ticks / (uint64_t)HZ;
    const uint64_t remainder = ticks % (uint64_t)HZ;

    if (seconds > UINT64_MAX / 1000000000u)
        return UINT64_MAX;

    return seconds * 1000000000u + remainder * 1000000000u / (uint64_t)HZ;
}

static int should_report_failure (const uint32_t fault, const uint64_t current_time_seconds)
{
    if (fault > USFS_FAULT_CLEANUP_REQUIRED)
        return true;

    if (g_observability.last_fault.ras_time_sec[fault] != 0 && current_time_seconds - g_observability.last_fault.ras_time_sec[fault] < 60u)
        return false;

    g_observability.last_fault.ras_time_sec[fault] = current_time_seconds;
    return true;
}

static void record_fault (
    const uint32_t fault,
    const int error,
    const struct usfs_connection * connection,
    const uint16_t opcode,
    const uint64_t request_id,
    const uint32_t mount_count
)
{
    const int channel = connection == NULL ? -1 : (int)connection->channel;
    const uint32_t mounts = connection == NULL ? mount_count : (uint32_t)connection->mounts_counter;
    const uint64_t now = (uint64_t)time;
    int should_report = false;

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_FAULT,
        fault,
        error,
        channel,
        opcode,
        request_id
    );

    // Record last fault and rate-limit RAS emission by fault class
    synchronized_with (g_observability.lock)
    {
        g_observability.last_fault.fault_class = fault;
        g_observability.last_fault.fault_errno = error;
        g_observability.last_fault.fault_channel = channel;
        g_observability.last_fault.fault_opcode = opcode;
        g_observability.last_fault.fault_request_id = request_id;
        g_observability.last_fault.fault_time_sec = now;

        should_report = should_report_failure (fault, now);
    }

    if (should_report)
        usfs_ras_report (fault, error, channel, opcode, request_id, mounts);
}

void set_up_observability_infrastructure (void)
{
    static const short metrics_lock_class = SHRT_MAX - 8;

    memset (&g_observability, 0, sizeof (g_observability));
    lock_alloc (&g_observability.lock, LOCK_ALLOC_PAGED, metrics_lock_class, -1);
    simple_lock_init (&g_observability.lock);
}

void tear_down_observability_infrastructure (void)
{
    lock_free (&g_observability.lock);
}

void on_request_processing_started (struct usfs_request * request, const enum usfs_request_class request_class, const uint32_t outstanding_requests_counter)
{
    if (request == NULL)
        return;

    request->request_class = (int)request_class;
    request->started_ns = get_current_time_in_ns ();
    request->metrics_finished = 0;

    synchronized_with (g_observability.lock)
    {
        g_observability.requests.num_of_accepted_requests = saturating_add_u64 (g_observability.requests.num_of_accepted_requests, 1u);

        if (outstanding_requests_counter > g_observability.requests.peak_outstanding_requests)
            g_observability.requests.peak_outstanding_requests = outstanding_requests_counter;
    }
}

static void update_latency_metrics (struct usfs_latency_stats * stats, const uint64_t elapsed_ns)
{
    uint64_t elapsed_ms = elapsed_ns / 1000000u;
    if (elapsed_ns % 1000000u != 0)
        elapsed_ms += 1u;

    stats->samples = saturating_add_u64 (stats->samples, 1u);
    stats->total_ns = saturating_add_u64 (stats->total_ns, elapsed_ns);

    if (elapsed_ns > stats->max_ns)
        stats->max_ns = elapsed_ns;

    uint64_t index;
    for (index = 0; index < USFS_STATUS_LATENCY_BUCKETS - 1u; ++index)
        if (elapsed_ms <= usfs_status_latency_bounds_ms[index])
            break;

    stats->buckets[index] = saturating_add_u64 (stats->buckets[index], 1u);
}

static void record_request_outcome (const enum usfs_request_outcome outcome)
{
    if (outcome == USFS_REQUEST_SUCCESS)
        g_observability.requests.num_of_successful_requests = saturating_add_u64 (g_observability.requests.num_of_successful_requests, 1u);
    else if (outcome == USFS_REQUEST_DAEMON_ERROR)
        g_observability.requests.num_of_rejected_by_daemon_requests = saturating_add_u64 (g_observability.requests.num_of_rejected_by_daemon_requests, 1u);
    else if (outcome == USFS_REQUEST_TRANSPORT_FAILURE)
        g_observability.requests.num_of_transport_failures = saturating_add_u64 (g_observability.requests.num_of_transport_failures, 1u);
    else if (outcome == USFS_REQUEST_TIMEOUT)
        g_observability.requests.num_of_timed_out_requests = saturating_add_u64 (g_observability.requests.num_of_timed_out_requests, 1u);
    else if (outcome == USFS_REQUEST_INTERRUPTED)
        g_observability.requests.num_of_interrupted_requests = saturating_add_u64 (g_observability.requests.num_of_interrupted_requests, 1u);
}

void on_request_processing_finished (struct usfs_request * request, const enum usfs_request_outcome outcome)
{
    if (request == NULL || request->metrics_finished)
        return;

    request->metrics_finished = 1;

    const uint64_t current_time_ns = get_current_time_in_ns ();
    const uint64_t elapsed_ns = current_time_ns >= request->started_ns ? current_time_ns - request->started_ns : 0;

    synchronized_with (g_observability.lock)
    {
        record_request_outcome (outcome);

        struct usfs_latency_stats * const latency = request->request_class == USFS_REQUEST_PAGER
                                                        ? &g_observability.requests.pager_latency
                                                        : &g_observability.requests.normal_latency;

        update_latency_metrics (latency, elapsed_ns);
    }
}

void on_request_rejected (void)
{
    synchronized_with (g_observability.lock)
    {
        g_observability.requests.num_of_rejections_by_queue_capacity = saturating_add_u64 (g_observability.requests.num_of_rejections_by_queue_capacity, 1u);
    }
}

void on_connection_failed (struct usfs_connection * connection, const int old_state, const int new_state)
{
    if (connection == NULL)
        return;

    if (old_state != USFS_CONN_ACTIVE)
        return;

    if (new_state == USFS_CONN_ACTIVE || new_state == USFS_CONN_CLOSED)
        return;

    if (connection->mounts_counter <= 0)
        return;

    if (connection->degraded_mount_counted)
        return;

    connection->degraded_mount_counted = 1;

    synchronized_with (g_observability.lock)
    {
        g_observability.lifecycle.num_of_dead_mounted_daemons += 1u;
    }
}

void on_mount_released (struct usfs_connection * connection)
{
    if (connection == NULL)
        return;

    synchronized_with (connection->lock)
    {
        if (connection->mounts_counter != 0 || !connection->degraded_mount_counted)
            return;

        connection->degraded_mount_counted = false;

        synchronized_with (g_observability.lock)
        {
            if (g_observability.lifecycle.num_of_dead_mounted_daemons > 0)
                g_observability.lifecycle.num_of_dead_mounted_daemons -= 1u;
        }
    }
}

void on_stale_mount_added (void)
{
    synchronized_with (g_observability.lock)
    {
        g_observability.lifecycle.num_of_stale_mounts += 1u;
    }
}

void on_stale_mount_released (void)
{
    synchronized_with (g_observability.lock)
    {
        if (g_observability.lifecycle.num_of_stale_mounts > 0)
            g_observability.lifecycle.num_of_stale_mounts -= 1u;
    }
}

void on_forced_recovery (const struct usfs_connection * connection, const int should_defer_mount_cleanup)
{
    synchronized_with (g_observability.lock)
    {
        g_observability.lifecycle.num_of_forced_recoveries = saturating_add_u64 (g_observability.lifecycle.num_of_forced_recoveries, 1u);
    }

    if (should_defer_mount_cleanup)
        on_stale_mount_added ();

    record_fault (USFS_FAULT_FORCED_RECOVERY, 0, connection, 0, 0, 0);
}

void report_protocol_error (const struct usfs_connection * connection, const int error, const uint16_t opcode, const uint64_t request_id)
{
    synchronized_with (g_observability.lock)
    {
        g_observability.requests.num_of_protocol_errors = saturating_add_u64 (g_observability.requests.num_of_protocol_errors, 1u);
    }

    record_fault (USFS_FAULT_PROTOCOL, error, connection, opcode, request_id, 0);
}

void report_timeout (const struct usfs_connection * connection, const uint16_t opcode, const uint64_t request_id)
{
    record_fault (USFS_FAULT_TIMEOUT, ETIMEDOUT, connection, opcode, request_id, 0);
}

void report_connection_lost (const struct usfs_connection * connection)
{
    if (connection == NULL || !connection->ready)
        return;

    synchronized_with (g_observability.lock)
    {
        g_observability.lifecycle.num_of_daemon_disconnects = saturating_add_u64 (g_observability.lifecycle.num_of_daemon_disconnects, 1u);
    }

    if (connection->mounts_counter <= 0)
        return;

    record_fault (USFS_FAULT_DAEMON_LOST, EIO, connection, 0, 0, 0);
}

void report_cleanup_failure (const int error, const uint32_t mounts_counter)
{
    record_fault (USFS_FAULT_CLEANUP_REQUIRED, error, NULL, 0, 0, mounts_counter);
}

static void set_state_request_counters (const struct usfs_connection * connection, struct kext_state * state)
{
    struct usfs_request * request;

    for (request = connection->pending_requests_head; request != NULL; request = request->next)
        state->pending_requests += 1u;

    for (request = connection->delivered_requests_head; request != NULL; request = request->next)
        state->delivered_requests += 1u;
}

static void set_state_connection_counters (struct kext_state * state, const chan_t channel)
{
    for (int index = 0; index < USFS_MAX_CONNECTIONS; ++index)
    {
        struct usfs_connection * connection = g_connections[index];
        if (connection == NULL || index == (int)channel)
            continue;

        synchronized_with (connection->lock)
        {
            if (!connection->ready)
                continue;

            if (connection->state == USFS_CONN_ACTIVE)
                state->active_daemons += 1u;
            else if (connection->state == USFS_CONN_UNHEALTHY)
                state->unhealthy_daemons += 1u;

            state->outstanding_requests += connection->outstanding_requests;

            set_state_request_counters (connection, state);
        }
    }
}

static void set_state_connection_data (struct kext_state * state, const chan_t channel)
{
    synchronized_with (g_connections_table_lock)
    {
        state->request_timeout_ms = g_usfs_runtime_limits.request_timeout_ms;
        state->pager_timeout_ms = g_usfs_runtime_limits.pager_timeout_ms;
        state->max_outstanding_requests = g_usfs_runtime_limits.max_outstanding_requests;

        set_state_connection_counters (state, channel);
    }
}

static void set_state_observability_data (struct kext_state * state)
{
    synchronized_with (g_observability.lock)
    {
        state->peak_outstanding_requests = g_observability.requests.peak_outstanding_requests;
        state->accepted_requests = g_observability.requests.num_of_accepted_requests;
        state->successful_requests = g_observability.requests.num_of_successful_requests;
        state->daemon_error_requests = g_observability.requests.num_of_rejected_by_daemon_requests;
        state->transport_failure_requests = g_observability.requests.num_of_transport_failures;
        state->timed_out_requests = g_observability.requests.num_of_timed_out_requests;
        state->queue_rejections = g_observability.requests.num_of_rejections_by_queue_capacity;
        state->interrupted_requests = g_observability.requests.num_of_interrupted_requests;
        state->protocol_errors = g_observability.requests.num_of_protocol_errors;
        state->normal_latency = g_observability.requests.normal_latency;
        state->pager_latency = g_observability.requests.pager_latency;

        state->stale_mounts = g_observability.lifecycle.num_of_stale_mounts;
        state->dead_mounted_daemons = g_observability.lifecycle.num_of_dead_mounted_daemons;
        state->daemon_disconnects = g_observability.lifecycle.num_of_daemon_disconnects;
        state->forced_recoveries = g_observability.lifecycle.num_of_forced_recoveries;

        state->last_fault = g_observability.last_fault.fault_class;
        state->last_fault_errno = g_observability.last_fault.fault_errno;
        state->last_fault_channel = g_observability.last_fault.fault_channel;
        state->last_fault_opcode = g_observability.last_fault.fault_opcode;
        state->last_fault_unique = g_observability.last_fault.fault_request_id;
        state->last_fault_time_sec = g_observability.last_fault.fault_time_sec;
    }
}

static void set_state_health (struct kext_state * state)
{
    // Failed mounted connections remain in the table while their daemon
    // channel is still open. Report them as unhealthy there, and as dead
    // mounted daemons only after they disappear from the table.

    if (state->dead_mounted_daemons >= state->unhealthy_daemons)
        state->dead_mounted_daemons -= state->unhealthy_daemons;
    else
        state->dead_mounted_daemons = 0;

    if (state->unhealthy_daemons > 0)
        state->health_reasons |= USFS_HEALTH_REASON_UNHEALTHY_DAEMON;

    if (state->dead_mounted_daemons > 0)
        state->health_reasons |= USFS_HEALTH_REASON_DEAD_MOUNT;

    if (state->stale_mounts > 0)
        state->health_reasons |= USFS_HEALTH_REASON_STALE_RESOURCES;

    if (state->kext_state == USFS_KEXT_CLEANUP_REQUIRED)
        state->health_reasons |= USFS_HEALTH_REASON_CLEANUP_REQUIRED;

    state->health = state->health_reasons == 0 ? USFS_HEALTH_HEALTHY : USFS_HEALTH_DEGRADED;
}

int take_kext_state_snapshot (struct kext_state * state, const chan_t channel)
{
    if (state == NULL)
        return EINVAL;

    memset (state, 0, sizeof (*state));

    state->magic = USFS_STATUS_MAGIC;
    state->abi_version = USFS_STATUS_ABI_VERSION;
    state->size = sizeof (*state);
    state->kext_state = (uint32_t)usfs_lifecycle_state_read ();

    synchronized_with (g_lifecycle_lock)
    {
        state->active_mounts = (uint32_t)g_mount_count;
    }

    set_state_connection_data (state, channel);
    set_state_observability_data (state);
    set_state_health (state);

    return 0;
}
