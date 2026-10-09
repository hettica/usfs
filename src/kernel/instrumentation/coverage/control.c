/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "definitions.h"
#include "control.h"
#include "runtime.h"

/* Kernel coverage control interface. */

static int copy_coverage_info (void * user_buffer)
{
    struct usfs_gcov_info info;

    memset (&info, 0, sizeof (info));
    info.abi_version = USFS_GCOV_ABI_VERSION;
    info.gcov_version = USFS_GCOV_VERSION;
    info.unit_count = usfs_gcov_unit_count ();
    info.flags = USFS_GCOV_FLAG_ARCS;

    const int rc = usfs_gcov_snapshot_size (&info.snapshot_size);
    if (rc != 0)
        return rc;

    return copyout ((caddr_t)&info, (caddr_t)user_buffer, sizeof (info)) == 0 ? 0 : EFAULT;
}

static int copy_snapshot_payload (const void * snapshot, const unsigned snapshot_size, const uint64_t capacity, const uint64_t buffer_address)
{
    if (capacity < snapshot_size)
        return ENOSPC;

    if (copyout ((caddr_t)snapshot, (caddr_t)(unsigned long)buffer_address, snapshot_size) != 0)
        return EFAULT;

    return 0;
}

/* The caller retains lifecycle admission through snapshot disposal. */
static int collect_coverage_snapshot (struct usfs_gcov_snapshot_request * request, void * user_buffer)
{
    void * snapshot = NULL;
    unsigned snapshot_size = 0;

    int rc = usfs_gcov_snapshot_create (&snapshot, &snapshot_size);
    if (rc == 0)
        rc = copy_snapshot_payload (snapshot, snapshot_size, request->capacity, request->user_buffer);

    request->size = snapshot_size;
    if (copyout ((caddr_t)request, (caddr_t)user_buffer, sizeof (*request)) != 0)
    {
        if (rc == 0)
            rc = EFAULT;
    }

    usfs_gcov_snapshot_destroy (snapshot);

    return rc;
}

static int copy_coverage_snapshot (void * user_buffer, const chan_t collector_channel)
{
    struct usfs_gcov_snapshot_request request;

    if (copyin ((caddr_t)user_buffer, (caddr_t)&request, sizeof (request)) != 0)
        return EFAULT;

    if (request.abi_version != USFS_GCOV_ABI_VERSION)
        return EINVAL;

    if (request.reserved != 0)
        return EINVAL;

    if (request.user_buffer == 0)
        return EINVAL;

    int rc = usfs_lifecycle_control_enter (collector_channel, USFS_CONTROL_EXACTLY_ONE_CONNECTION);
    if (rc != 0)
        return rc;

    rc = collect_coverage_snapshot (&request, user_buffer);
    usfs_lifecycle_control_leave ();

    return rc;
}

static int reset_coverage_counters (void * user_buffer, const chan_t collector_channel)
{
    if (user_buffer == NULL)
        return EFAULT;

    uint32_t abi_version;

    if (copyin ((caddr_t)user_buffer, (caddr_t)&abi_version, sizeof (abi_version)) != 0)
        return EFAULT;

    if (abi_version != USFS_GCOV_ABI_VERSION)
        return EINVAL;

    int rc = usfs_lifecycle_control_enter (collector_channel, USFS_CONTROL_EXACTLY_ONE_CONNECTION);
    if (rc != 0)
        return rc;

    rc = usfs_gcov_reset_counters ();
    usfs_lifecycle_control_leave ();

    return rc;
}

int usfs_gcov_control_ioctl (const int command, void * user_buffer, const chan_t collector_channel)
{
    if (command == USFS_GCOV_IOC_INFO)
        return copy_coverage_info (user_buffer);

    if (command == USFS_GCOV_IOC_SNAPSHOT)
        return copy_coverage_snapshot (user_buffer, collector_channel);

    if (command == USFS_GCOV_IOC_RESET)
        return reset_coverage_counters (user_buffer, collector_channel);

    return EINVAL;
}

static int validate_coverage_export (const struct usfs_config_coverage_export * request, const uint32_t expected_generation)
{
    if (request->magic != USFS_CONFIG_DDS_MAGIC)
        return EINVAL;

    if (request->abi_version != USFS_CONFIG_ABI_VERSION)
        return EINVAL;

    if (request->size != sizeof (*request))
        return EINVAL;

    if (request->generation == 0)
        return EINVAL;

    if (request->generation != expected_generation)
        return EINVAL;

    if (request->capacity > USFS_GCOV_MAX_SNAPSHOT)
        return EINVAL;

    if (request->user_buffer == 0)
        return EINVAL;

    if (request->reserved[0] != 0)
        return EINVAL;

    if (request->reserved[1] != 0)
        return EINVAL;

    return 0;
}

static int export_coverage_snapshot (const struct usfs_config_coverage_export * request, const int reset_after)
{
    void * snapshot = NULL;
    unsigned snapshot_size = 0;

    int rc = usfs_gcov_snapshot_create (&snapshot, &snapshot_size);
    if (rc == 0)
        rc = copy_snapshot_payload (snapshot, snapshot_size, request->capacity, request->user_buffer);

    if (rc == 0)
    {
        if (reset_after)
            rc = usfs_gcov_reset_counters ();
    }

    usfs_gcov_snapshot_destroy (snapshot);

    return rc;
}

/*
 * Export a final generation snapshot through SYS_CFGDD. This path is used
 * only after runtime entry points are quiescent or have been removed, so it
 * deliberately does not take the ordinary device-control locks.
 */
int usfs_gcov_config_export (struct uio * user_io_request, const int reset_after, const uint32_t expected_generation)
{
    if (user_io_request == NULL)
        return EINVAL;

    struct usfs_config_coverage_export request;

    if ((uint64_t)user_io_request->uio_resid != (uint64_t)sizeof (request))
        return EINVAL;

    memset (&request, 0, sizeof (request));

    int rc = uiomove ((caddr_t)&request, (long)sizeof (request), UIO_WRITE, user_io_request);
    if (rc != 0)
        return rc;

    rc = validate_coverage_export (&request, expected_generation);
    if (rc != 0)
        return rc;

    rc = usfs_gcov_runtime_prepare_export ();
    if (rc != 0)
        return rc;

    return export_coverage_snapshot (&request, reset_after);
}
