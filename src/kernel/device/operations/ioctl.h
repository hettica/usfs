// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_IOCTL_H
#define USFS_DEVICE_IOCTL_H

#include "definitions.h"

static int copy_to_user_space (const void * kernel_source, void * user_space_destination, const size_t size)
{
    return copyout ((caddr_t)kernel_source, user_space_destination, size) == 0 ? 0 : EFAULT;
}

static int accept_new_connection (void * user_space_buffer, const chan_t channel)
{
    struct usfs_connection * const connection = get_connection_by_channel (channel);

    if (connection == NULL)
        return ENXIO;

    struct usfs_dev_info info = { 0 };

    info.protocol_version = USFS_PROTOCOL_VERSION;
    info.fs_type = gfs.gfs_type;
    info.channel = (int32_t)channel;
    info.cookie = connection->cookie;

    const int rc = copy_to_user_space (&info, user_space_buffer, sizeof (info));
    if (rc != 0)
        return rc;

    synchronized_with (connection->lock)
    {
        connection->ready = true;
        USFS_TRACE_CONTROL5 (
            USFS_TRACE_CONNECTION,
            USFS_TRACE_ACTION_READY,
            connection->channel,
            connection->state,
            connection->outstanding_requests,
            0
        );
    }

    return 0;
}

static int get_kext_state (void * user_space_buffer, const chan_t channel)
{
    struct kext_state state = { 0 };

    const int rc = take_kext_state_snapshot (&state, channel);
    if (rc != 0)
        return rc;

    return copy_to_user_space (&state, user_space_buffer, sizeof (state));
}

static int get_runtime_config (void * user_space_buffer, const chan_t channel)
{
    struct kext_runtime_config config = { 0 };

    const int rc = take_kext_runtime_config_snapshot (&config);
    if (rc != 0)
        return rc;

    struct usfs_connection * const collector = get_connection_by_channel (channel);

    if (collector != NULL) //! todo: properly handle this with corresponding RC; should not be possible tho
    {
        synchronized_with (collector->lock)
        {
            if (collector->state == USFS_CONN_ACTIVE && config.active_connections > 0)
                config.active_connections -= 1;
            else if (collector->state == USFS_CONN_UNHEALTHY && config.unhealthy_connections > 0)
                config.unhealthy_connections -= 1;

            if (config.outstanding_requests >= collector->outstanding_requests)
                config.outstanding_requests -= collector->outstanding_requests;
        }
    }

    return copy_to_user_space (&config, user_space_buffer, sizeof (config));
}

static int set_runtime_config (void * user_space_buffer)
{
    struct kext_runtime_config config = { 0 };

    if (privcheck (DEV_CONFIG) != 0)
        return EPERM;

    if (copyin (user_space_buffer, (caddr_t)&config, sizeof (config)) != 0)
        return EFAULT;

    return set_kext_runtime_config (&config);
}

static int process_public_dev_ctl_command (
    const dev_t device_number,
    const int command,
    void * user_space_buffer,
    const ulong ioctl_flags,
    const chan_t channel,
    const int is_called_from_kernel_extension
)
{
    ignore_parameter device_number;                   //! todo: properly validate this parameter, reject not owning devices
    ignore_parameter ioctl_flags;                     //! todo: properly validate this parameter, reject unsupported flags
    ignore_parameter is_called_from_kernel_extension; //! todo: properly validate this parameter, reject unsupported flags

    if (command == USFS_IOC_CONNECTION_REQUEST)
        return accept_new_connection (user_space_buffer, channel);

    if (command == USFS_IOC_GET_RUNTIME_CONFIG)
        return get_runtime_config (user_space_buffer, channel);

    if (command == USFS_IOC_SET_RUNTIME_CONFIG)
        return set_runtime_config (user_space_buffer);

    if (command == USFS_IOC_GET_KEXT_STATE)
        return get_kext_state (user_space_buffer, channel);

    return EINVAL;
}

static int is_public_dev_ctl_command (const int command)
{
    switch (command)
    {
        case USFS_IOC_CONNECTION_REQUEST:
        case USFS_IOC_GET_RUNTIME_CONFIG:
        case USFS_IOC_SET_RUNTIME_CONFIG:
        case USFS_IOC_GET_KEXT_STATE:
            return true;

        default:
            return false;
    }
}

// Declaration is used only to exclude the ioctl dispatcher from profile instrumentation. It routes coverage-control
// commands, so instrumenting it would let coverage collection and reset modify measured counters.
static int usfs_dev_ioctl (
    dev_t device_number,
    int command,
    void * user_space_buffer,
    ulong ioctl_flags,
    chan_t channel,
    int is_called_from_kernel_extension
) __attribute__ ((no_profile_instrument_function));

static int usfs_dev_ioctl (
    const dev_t device_number,
    const int command,
    void * user_space_buffer,
    const ulong ioctl_flags,
    const chan_t channel,
    const int is_called_from_kernel_extension
)
{
    if (channel == USFS_STATUS_CHANNEL)
    {
        if (command != USFS_IOC_GET_KEXT_STATE)
            return EINVAL;

        return get_kext_state (user_space_buffer, channel);
    }

    if (is_instrumentation_dev_ctl_command (command))
        return process_instrumentation_dev_ctl_command (command, user_space_buffer, channel);

    if (is_public_dev_ctl_command (command))
        return process_public_dev_ctl_command (device_number, command, user_space_buffer, ioctl_flags, channel, is_called_from_kernel_extension);

    return EINVAL;
}

#endif
