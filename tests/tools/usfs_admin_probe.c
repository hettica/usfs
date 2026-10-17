/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Public-ABI-only administrative safety probe for long-running campaigns.
 */

#include "usfs_config.h"
#include "usfs_proto.h"

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE               "/dev/usfs0"
#define UNKNOWN_IOCTL        0x55537fffu
#define RETIRED_GETLOG_IOCTL 0x55530002u

static void * invalid_address (void)
{
    volatile uintptr_t address = UINTPTR_MAX;
    return (void *)address;
}

static int expect_failure (const int device_descriptor, const int command, void * argument, const int first_error, const int second_error)
{
    errno = 0;
    const int rc = ioctl (device_descriptor, command, argument);
    const int saved_errno = errno;
    if (rc == -1 && (saved_errno == first_error || saved_errno == second_error))
        return 0;
    fprintf (stderr, "admin probe: ioctl %#x rc=%d errno=%d\n", command, rc, saved_errno);
    return -1;
}

static int open_churn (void)
{
    unsigned iteration_index;

    for (iteration_index = 0; iteration_index < 32; ++iteration_index)
    {
        struct usfs_dev_info device_info;
        const int device_descriptor = open (DEVICE, O_RDWR | O_NONBLOCK);

        if (device_descriptor < 0)
        {
            perror ("admin probe: churn open");
            return -1;
        }
        memset (&device_info, 0, sizeof (device_info));
        if (ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info) != 0 ||
            device_info.protocol_version != USFS_PROTOCOL_VERSION || device_info.cookie == 0 ||
            close (device_descriptor) != 0)
        {
            fprintf (stderr, "admin probe: churn handshake %u failed errno=%d\n", iteration_index, errno);
            return -1;
        }
    }
    return 0;
}

static int check_runtime_control (const int device_descriptor, const int unprivileged)
{
    struct kext_runtime_config runtime_configuration;

    memset (&runtime_configuration, 0, sizeof (runtime_configuration));
    if (ioctl (device_descriptor, USFS_IOC_GET_RUNTIME_CONFIG, &runtime_configuration) != 0 ||
        runtime_configuration.magic != USFS_RUNTIME_CONFIG_MAGIC ||
        runtime_configuration.abi_version != USFS_RUNTIME_CONFIG_ABI_VERSION)
    {
        fprintf (stderr, "admin probe: valid runtime GET failed errno=%d\n", errno);
        return 1;
    }
    runtime_configuration.magic = USFS_RUNTIME_CONFIG_MAGIC;
    runtime_configuration.abi_version = USFS_RUNTIME_CONFIG_ABI_VERSION;
    runtime_configuration.size = sizeof (runtime_configuration);
    runtime_configuration.set_mask = unprivileged ? USFS_RUNTIME_SET_REQUEST_TIMEOUT : 0;
    runtime_configuration.active_connections = 0;
    runtime_configuration.unhealthy_connections = 0;
    runtime_configuration.outstanding_requests = 0;
    memset (runtime_configuration.reserved, 0, sizeof (runtime_configuration.reserved));
    if (unprivileged)
    {
        runtime_configuration.request_timeout_ms = USFS_DEFAULT_REQUEST_TIMEOUT_MS;
        if (expect_failure (device_descriptor, USFS_IOC_SET_RUNTIME_CONFIG, &runtime_configuration, EPERM, EACCES) != 0)
        {
            return 1;
        }
    }
    return 0;
}

static int check_control_paths (const int device_descriptor, void * invalid_user_address, const int unprivileged)
{
    struct usfs_dev_info device_info;

    memset (&device_info, 0, sizeof (device_info));
    if (ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info) != 0 ||
        device_info.protocol_version != USFS_PROTOCOL_VERSION || device_info.cookie == 0)
    {
        fprintf (stderr, "admin probe: valid GETINFO failed errno=%d\n", errno);
        return 1;
    }
    if (expect_failure (device_descriptor, USFS_IOC_CONNECTION_REQUEST, invalid_user_address, EFAULT, EFAULT) != 0 ||
        expect_failure (device_descriptor, USFS_IOC_GET_RUNTIME_CONFIG, invalid_user_address, EFAULT, EFAULT) != 0 ||
        expect_failure (device_descriptor, USFS_IOC_SET_RUNTIME_CONFIG, invalid_user_address, EFAULT, EFAULT) != 0 ||
        expect_failure (device_descriptor, RETIRED_GETLOG_IOCTL, NULL, EINVAL, ENOTTY) != 0 ||
        expect_failure (device_descriptor, UNKNOWN_IOCTL, NULL, EINVAL, ENOTTY) != 0)
    {
        return 1;
    }
    return check_runtime_control (device_descriptor, unprivileged);
}

static int safe_probe (const int unprivileged)
{
    void * invalid_user_address = invalid_address ();
    const int device_descriptor = open (DEVICE, O_RDWR | O_NONBLOCK);

    if (device_descriptor < 0)
    {
        if (unprivileged && (errno == EPERM || errno == EACCES))
        {
            printf ("admin probe: unprivileged device open denied\n");
            return 0;
        }
        perror ("admin probe: open " DEVICE);
        return 1;
    }
    if (check_control_paths (device_descriptor, invalid_user_address, unprivileged) != 0)
    {
        close (device_descriptor);
        return 1;
    }

    if (close (device_descriptor) != 0)
    {
        perror ("admin probe: close");
        return 1;
    }
    if (open_churn () != 0)
        return 1;
    printf ("admin probe: public control paths healthy%s\n", unprivileged ? "; mutation denied" : "");
    return 0;
}

int main (const int argc, char ** argv)
{
    if (argc == 1 || (argc == 2 && strcmp (argv[1], "--safe") == 0))
        return safe_probe (0);
    if (argc == 2 && strcmp (argv[1], "--unprivileged") == 0)
        return safe_probe (1);
    fprintf (stderr, "usage: %s [--safe|--unprivileged]\n", argv[0]);
    return 2;
}
