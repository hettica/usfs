/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "usfs_gcov.h"
#include "usfs_config.h"
#include "usfs_proto.h"
#include "usfs_test.h"

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE "/dev/usfs0"

static void * bad_user_address (void)
{
    /*
     * AIX keeps a readable low page in 64-bit processes, so address 1 is a
     * reliable copyout failure but can let copyin read garbage and produce
     * EINVAL.  The all-ones address is outside the user address range for
     * both transfer directions.
     */
    volatile uintptr_t address = UINTPTR_MAX;

    return (void *)address;
}

static int ioctl_fails_with_efault (const int device_descriptor, const int command, void * argument)
{
    errno = 0;
    const int rc = ioctl (device_descriptor, command, argument);
    const int saved_errno = errno;
    if (rc != -1 || saved_errno != EFAULT)
        tap_diag ("ioctl %#x invalid address: rc=%d errno=%d", command, rc, saved_errno);
    if (rc != -1)
        return false;

    return saved_errno == EFAULT;
}

static void check_connection_controls (struct tap_state * tap, const int device_descriptor, void * invalid_user_address)
{
    struct usfs_dev_info device_info;

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_IOC_CONNECTION_REQUEST, invalid_user_address), "device GETINFO rejects an invalid output address with EFAULT");
    memset (&device_info, 0, sizeof (device_info));
    tap_ok (tap, ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info) == 0 && device_info.protocol_version == USFS_PROTOCOL_VERSION && device_info.cookie != 0, "device channel remains usable after GETINFO copyout failure");
}

static void check_runtime_config_controls (struct tap_state * tap, const int device_descriptor, void * invalid_user_address)
{
    struct kext_runtime_config runtime_config;

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_IOC_GET_RUNTIME_CONFIG, invalid_user_address), "runtime GET rejects an invalid output address with EFAULT");
    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_IOC_SET_RUNTIME_CONFIG, invalid_user_address), "runtime SET rejects an invalid input address with EFAULT");
    memset (&runtime_config, 0, sizeof (runtime_config));
    tap_ok (tap, ioctl (device_descriptor, USFS_IOC_GET_RUNTIME_CONFIG, &runtime_config) == 0 && runtime_config.magic == USFS_RUNTIME_CONFIG_MAGIC && runtime_config.abi_version == USFS_RUNTIME_CONFIG_ABI_VERSION, "runtime configuration remains readable after copy failures");
    runtime_config.set_mask = USFS_RUNTIME_SET_REQUEST_TIMEOUT;
    runtime_config.request_timeout_ms = USFS_MIN_TIMEOUT_MS - 1u;
    errno = 0;
    tap_ok (tap, ioctl (device_descriptor, USFS_IOC_SET_RUNTIME_CONFIG, &runtime_config) == -1 && errno == EINVAL, "runtime SET rejects out-of-range policy values");
}

static void check_status_controls (struct tap_state * tap, const int device_descriptor, void * invalid_user_address)
{
    struct kext_state status_before;
    struct kext_state status_after;

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_IOC_GET_KEXT_STATE, invalid_user_address), "status GET rejects an invalid output address with EFAULT");
    memset (&status_before, 0, sizeof (status_before));
    memset (&status_after, 0, sizeof (status_after));
    tap_ok (tap, ioctl (device_descriptor, USFS_IOC_GET_KEXT_STATE, &status_before) == 0 && ioctl (device_descriptor, USFS_IOC_GET_KEXT_STATE, &status_after) == 0 && usfs_status_validate (&status_before) == 0 && memcmp (&status_before, &status_after, sizeof (status_before)) == 0 && status_before.active_daemons == 0, "status snapshots are non-destructive and exclude their collector");
}

static void check_test_controls (struct tap_state * tap, const int device_descriptor, void * invalid_user_address)
{
    struct usfs_test_info test_info;
    struct usfs_test_node_status node_status;
    struct usfs_test_status test_status;

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_TEST_IOC_INFO, invalid_user_address), "test INFO rejects an invalid output address with EFAULT");
    memset (&test_info, 0, sizeof (test_info));
    tap_ok (tap, ioctl (device_descriptor, USFS_TEST_IOC_INFO, &test_info) == 0 && test_info.abi_version == USFS_TEST_ABI_VERSION, "test INFO remains usable after copyout failure");

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_TEST_IOC_STATUS, invalid_user_address), "test STATUS rejects an invalid output address with EFAULT");
    memset (&test_status, 0, sizeof (test_status));
    tap_ok (tap, ioctl (device_descriptor, USFS_TEST_IOC_STATUS, &test_status) == 0 && test_status.abi_version == USFS_TEST_ABI_VERSION, "test STATUS releases quiescence after copyout failure");

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_TEST_IOC_ARM, invalid_user_address), "test ARM rejects an invalid input address with EFAULT");
    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_TEST_IOC_SCHEDULE_ARM, invalid_user_address), "test schedule ARM rejects an invalid input address with EFAULT");
    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_TEST_IOC_SCHEDULE_STATUS, invalid_user_address), "test schedule STATUS rejects an invalid output address with EFAULT");
    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_TEST_IOC_NODE_STATUS, invalid_user_address), "test node STATUS rejects an invalid output address with EFAULT");
    memset (&node_status, 0, sizeof (node_status));
    tap_ok (tap, ioctl (device_descriptor, USFS_TEST_IOC_NODE_STATUS, &node_status) == 0 && node_status.abi_version == USFS_TEST_ABI_VERSION, "test node STATUS remains usable after copyout failure");
    tap_ok (tap, ioctl (device_descriptor, USFS_TEST_IOC_RESET, NULL) == 0, "test RESET acquires quiescence after prior copy failures");
    memset (&test_info, 0, sizeof (test_info));
    tap_ok (tap, ioctl (device_descriptor, USFS_TEST_IOC_INFO, &test_info) == 0, "test controls remain usable after reset");
}

static void check_coverage_controls (struct tap_state * tap, const int device_descriptor, void * invalid_user_address, const int coverage_active, struct usfs_gcov_info * gcov_info)
{
    if (!coverage_active)
    {
        errno = 0;
        tap_ok (tap, ioctl (device_descriptor, USFS_GCOV_IOC_INFO, gcov_info) == -1 && errno == EINVAL, "coverage controls are absent from a non-coverage build");
        return;
    }

    struct usfs_gcov_snapshot_request snapshot;

    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_GCOV_IOC_INFO, invalid_user_address), "coverage INFO rejects an invalid output address with EFAULT");
    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_GCOV_IOC_SNAPSHOT, invalid_user_address), "coverage SNAPSHOT rejects an invalid request address with EFAULT");
    tap_ok (tap, ioctl_fails_with_efault (device_descriptor, USFS_GCOV_IOC_RESET, invalid_user_address), "coverage RESET rejects an invalid input address with EFAULT");

    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = USFS_GCOV_ABI_VERSION;
    snapshot.capacity = gcov_info->snapshot_size;
    snapshot.user_buffer = (uint64_t)(uintptr_t)invalid_user_address;
    errno = 0;
    tap_ok (tap, ioctl (device_descriptor, USFS_GCOV_IOC_SNAPSHOT, &snapshot) == -1 && errno == EFAULT && snapshot.size == gcov_info->snapshot_size, "coverage SNAPSHOT rejects an invalid nested output address");

    memset (gcov_info, 0, sizeof (*gcov_info));
    tap_ok (tap, ioctl (device_descriptor, USFS_GCOV_IOC_INFO, gcov_info) == 0 && gcov_info->abi_version == USFS_GCOV_ABI_VERSION, "coverage controls release quiescence after copy failures");
}

static int run_controls (void)
{
    struct usfs_gcov_info gcov_info;
    struct tap_state tap;
    void * invalid_user_address = bad_user_address ();
    int coverage_active;
    int device_descriptor;

    device_descriptor = open (DEVICE, O_RDWR);
    if (device_descriptor < 0)
    {
        perror ("open " DEVICE);
        return 1;
    }

    memset (&gcov_info, 0, sizeof (gcov_info));
    coverage_active = ioctl (device_descriptor, USFS_GCOV_IOC_INFO, &gcov_info) == 0;
    tap_plan (&tap, coverage_active ? 24u : 20u);

    check_connection_controls (&tap, device_descriptor, invalid_user_address);
    check_runtime_config_controls (&tap, device_descriptor, invalid_user_address);
    check_status_controls (&tap, device_descriptor, invalid_user_address);
    check_test_controls (&tap, device_descriptor, invalid_user_address);
    check_coverage_controls (&tap, device_descriptor, invalid_user_address, coverage_active, &gcov_info);

    close (device_descriptor);
    return tap_finish (&tap);
}

static int check_io_transfer (const int device_descriptor, const int flags, const char * operation, char * invalid_user_address)
{
    ssize_t result;

    errno = 0;
    if (flags == O_RDONLY)
        result = read (device_descriptor, invalid_user_address, 1);
    else
        result = write (device_descriptor, invalid_user_address, 1);
    if (result != -1 || errno != EFAULT)
    {
        fprintf (stderr, "%s invalid buffer: result=%ld errno=%d\n", operation, (long)result, errno);
        return 1;
    }

    return 0;
}

static int run_io (const char * operation, const char * path)
{
    char * invalid_user_address = bad_user_address ();
    int device_descriptor;
    int flags;

    if (strcmp (operation, "read") == 0)
        flags = O_RDONLY;
    else if (strcmp (operation, "write") == 0)
        flags = O_WRONLY;
    else
    {
        fprintf (stderr, "unknown I/O operation: %s\n", operation);
        return 2;
    }

    device_descriptor = open (path, flags);
    if (device_descriptor < 0)
    {
        perror (path);
        return 1;
    }

    if (check_io_transfer (device_descriptor, flags, operation, invalid_user_address) != 0)
    {
        close (device_descriptor);
        return 1;
    }

    if (close (device_descriptor) != 0)
    {
        perror ("close");
        return 1;
    }
    return 0;
}

int main (const int argc, char ** argv)
{
    if (argc == 2 && strcmp (argv[1], "controls") == 0)
        return run_controls ();
    if (argc == 3)
        return run_io (argv[1], argv[2]);

    fprintf (stderr, "usage: usfs_usercopy_test controls|read PATH|write PATH"
                     "|parent PATH EXPECTED_NODEID\n");
    return 2;
}
