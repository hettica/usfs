// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT
/* Private instrumentation boundary for the independent resident proxy. */
#ifdef USFS_TESTING
static uint32_t barrier_point;
static uint32_t barrier_reached;
static tid_t barrier_event = EVENT_NULL;
static void checkpoint(uint32_t point)
{
    simple_lock(&dispatch_lock);
    while (barrier_point == point)
    {
        barrier_reached = point;
        e_assert_wait(&barrier_event, 0);
        simple_unlock(&dispatch_lock);
        e_block_thread();
        simple_lock(&dispatch_lock);
    }
    simple_unlock(&dispatch_lock);
}
#else
#define checkpoint(point) ((void)0)
#endif

#ifdef USFS_TESTING
static int proxy_test_busy(void) { return barrier_point != 0; }
static int proxy_test_command(struct uio * io)
{
    struct usfs_proxy_test_control request;
    if (!io || io->uio_resid != sizeof(request)) return EINVAL;
    int rc = uiomove((caddr_t)&request, sizeof(request), UIO_WRITE, io);
    if (rc) return rc;
    if (request.reserved || request.operation > 2 || request.point > 4) return EINVAL;
    int wake = 0;
    simple_lock(&dispatch_lock);
    if (request.operation == 1 && barrier_point)
        rc = EBUSY;
    else if (request.operation == 1)
    { barrier_reached = 0; barrier_point = request.point; }
    else if (request.operation == 2)
    { wake = barrier_reached != 0; barrier_reached = 0; barrier_point = 0; }
    else
        rc = (int)barrier_reached; /* query encoded as errno for test helper */
    simple_unlock(&dispatch_lock);
    /* Only signal an admitted checkpoint; no waiter exists after arm alone. */
    if (wake) e_wakeup(&barrier_event);
    return rc;
}
#else
static int proxy_test_busy(void) { return 0; }
#endif

#ifdef USFS_COVERAGE
#include "instrumentation/coverage/runtime.h"
static int proxy_export_counters(struct uio *io)
{
    struct usfs_config_coverage_export request;
    if (!io || io->uio_resid != sizeof(request)) return EINVAL;
    int rc = uiomove((caddr_t)&request, sizeof(request), UIO_WRITE, io);
    if (rc) return rc;
    if (request.magic != USFS_CONFIG_DDS_MAGIC || request.abi_version != USFS_CONFIG_ABI_VERSION ||
        request.size != sizeof(request) || !request.user_buffer || request.reserved[0] || request.reserved[1])
        return EINVAL;
    rc = usfs_gcov_runtime_prepare_export();
    if (rc) return rc;
    void *snapshot = NULL;
    unsigned size = 0;
    rc = usfs_gcov_snapshot_create(&snapshot, &size);
    if (rc) return rc;
    if (request.capacity < size) rc = ENOSPC;
    else if (copyout(snapshot, (void *)(unsigned long)request.user_buffer, size)) rc = EFAULT;
    usfs_gcov_snapshot_destroy(snapshot);
    if (!rc) rc = usfs_gcov_reset_counters();
    return rc;
}
#endif

static int proxy_instrument_command(int command, struct uio *io, int *handled)
{
    (void)io; /* Production has no instrumentation command payload. */
    *handled = 1;
    if (command == USFS_PROXY_TEST)
    {
#ifdef USFS_TESTING
        return proxy_test_command(io);
#else
        return ENOTTY;
#endif
    }
    if (command == USFS_PROXY_EXPORT)
    {
#ifdef USFS_COVERAGE
        simple_lock(&dispatch_lock);
        if (control_busy || (implementation.config &&
            status.state == USFS_PROXY_CLOSED &&
            status.lifecycle == USFS_KEXT_STATE_DOWN))
        { simple_unlock(&dispatch_lock); return EBUSY; }
        control_busy = 1;
        simple_unlock(&dispatch_lock);
        const int rc = proxy_export_counters(io);
        simple_lock(&dispatch_lock);
        control_busy = 0;
        simple_unlock(&dispatch_lock);
        return rc;
#else
        return ENOTTY;
#endif
    }
    *handled = 0;
    return 0;
}
