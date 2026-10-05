// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"

static int is_user_space_reply_timeout_valid (const uint32_t value)
{
    if (value < USFS_MIN_TIMEOUT_MS)
        return false;

    return value <= USFS_MAX_TIMEOUT_MS;
}

static int has_runtime_config_field (const struct kext_runtime_config * config, const uint32_t field)
{
    return (config->set_mask & field) != 0;
}

static int is_reserved_area_zeroed (const struct kext_runtime_config * config)
{
    for (unsigned index = 0; index < sizeof (config->reserved) / sizeof (config->reserved[0]); index++)
    {
        if (config->reserved[index] != 0)
            return false;
    }

    return true;
}

static int is_max_outstanding_requests_number_valid (const uint32_t max_outstanding_requests)
{
    if (max_outstanding_requests < USFS_MIN_OUTSTANDING || max_outstanding_requests > USFS_MAX_OUTSTANDING)
        return false;

    return true;
}

static int validate_kext_runtime_config (const struct kext_runtime_config * config)
{
    if (config == NULL)
        return EINVAL;

    if (config->magic != USFS_RUNTIME_CONFIG_MAGIC)
        return EINVAL;

    if (config->abi_version != USFS_RUNTIME_CONFIG_ABI_VERSION)
        return EINVAL;

    if (config->size != sizeof (*config))
        return EINVAL;

    if (config->set_mask == 0)
        return EINVAL;

    if ((config->set_mask & ~USFS_RUNTIME_SET_ALL) != 0)
        return EINVAL;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_REQUEST_TIMEOUT) && !is_user_space_reply_timeout_valid (config->request_timeout_ms))
        return EINVAL;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_PAGER_TIMEOUT) && !is_user_space_reply_timeout_valid (config->pager_timeout_ms))
        return EINVAL;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_MAX_OUTSTANDING) && !is_max_outstanding_requests_number_valid (config->max_outstanding_requests))
        return EINVAL;

    if (!is_reserved_area_zeroed (config))
        return EINVAL;

    return 0;
}

static void take_connections_runtime_config_snapshot (struct kext_runtime_config * config)
{
    for (unsigned index = 0; index < USFS_MAX_CONNECTIONS; index++)
    {
        struct usfs_connection * connection = g_connections[index];

        if (connection == NULL)
            continue;

        synchronized_with (connection->lock)
        {
            if (connection->state == USFS_CONN_ACTIVE)
                config->active_connections += 1;
            else if (connection->state == USFS_CONN_UNHEALTHY)
                config->unhealthy_connections += 1;

            config->outstanding_requests += connection->outstanding_requests;
        }
    }
}

int take_kext_runtime_config_snapshot (struct kext_runtime_config * config)
{
    if (config == NULL)
        return EINVAL;

    // Initialize configuration snapshot
    memset (config, 0, sizeof (*config));
    config->magic = USFS_RUNTIME_CONFIG_MAGIC;
    config->abi_version = USFS_RUNTIME_CONFIG_ABI_VERSION;
    config->size = sizeof (*config);

    // Snapshot limits and active connection state
    synchronized_with (g_connections_table_lock)
    {
        config->request_timeout_ms = g_usfs_runtime_limits.request_timeout_ms;
        config->pager_timeout_ms = g_usfs_runtime_limits.pager_timeout_ms;
        config->max_outstanding_requests = g_usfs_runtime_limits.max_outstanding_requests;

        take_connections_runtime_config_snapshot (config);
    }

    return 0;
}

static void apply_runtime_config_to_connection (struct usfs_connection * connection, const struct kext_runtime_config * config)
{
    if (has_runtime_config_field (config, USFS_RUNTIME_SET_REQUEST_TIMEOUT))
        connection->request_timeout_ms = config->request_timeout_ms;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_PAGER_TIMEOUT))
        connection->pager_timeout_ms = config->pager_timeout_ms;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_MAX_OUTSTANDING))
        connection->max_outstanding_requests = config->max_outstanding_requests;
}

static void apply_runtime_config (const struct kext_runtime_config * config)
{
    if (has_runtime_config_field (config, USFS_RUNTIME_SET_REQUEST_TIMEOUT))
        g_usfs_runtime_limits.request_timeout_ms = config->request_timeout_ms;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_PAGER_TIMEOUT))
        g_usfs_runtime_limits.pager_timeout_ms = config->pager_timeout_ms;

    if (has_runtime_config_field (config, USFS_RUNTIME_SET_MAX_OUTSTANDING))
        g_usfs_runtime_limits.max_outstanding_requests = config->max_outstanding_requests;

    for (unsigned connection_index = 0; connection_index < USFS_MAX_CONNECTIONS; connection_index++)
    {
        struct usfs_connection * connection = g_connections[connection_index];
        if (connection == NULL)
            continue;

        synchronized_with (connection->lock)
        {
            apply_runtime_config_to_connection (connection, config);
        }
    }
}

int set_kext_runtime_config (const struct kext_runtime_config * config)
{
    const int rc = validate_kext_runtime_config (config);

    if (rc != 0)
    {
        USFS_TRACE_CONTROL5 (
            USFS_TRACE_RUNTIME_CONFIG,
            0,
            0,
            0,
            0,
            rc
        );
        return rc;
    }

    // Apply requested limits to defaults and existing connections
    synchronized_with (g_connections_table_lock)
    {
        apply_runtime_config (config);
    }

    // Report effective global limits
    USFS_TRACE_CONTROL5 (
        USFS_TRACE_RUNTIME_CONFIG,
        g_usfs_runtime_limits.request_timeout_ms,
        g_usfs_runtime_limits.pager_timeout_ms,
        g_usfs_runtime_limits.max_outstanding_requests,
        config->set_mask,
        0
    );

    return 0;
}
