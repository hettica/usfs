// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#include "usfs_common.h"
#include <assert.h>
#include <stdbool.h>

enum fixture_fault
{
    NO_FAULT, QUERY_LOAD_FAILURE, QUERY_STATUS_FAILURE, UNINITIALIZED_PROXY,
    WRONG_ABI, WRONG_SIZE, UNTERMINATED_PROXY_BUILD, UNTERMINATED_IMPLEMENTATION_BUILD,
    UNTERMINATED_VARIANT, LOAD_FAILURE, INITIALIZE_FAILURE, LOCK_FAILURE,
    UNLOCK_FAILURE, TERMINATE_FAILURE, UNLOAD_FAILURE, STILL_LOADED,
    VERIFY_QUERY_FAILURE, ALLOCATION_FAILURE, MAJOR_FAILURE, MINOR_QUERY_FAILURE,
    EXTRA_MINOR, NO_MINOR, RELEASE_DEVICE_FAILURE, RELEASE_MAJOR_FAILURE
};

static enum fixture_fault fault;
static struct usfs_proxy_status resident_status;
static int resident;
static int implementation_state;
static unsigned query_count;
static unsigned load_count;
static unsigned initialize_count;
static unsigned terminate_count;
static unsigned unload_count;
static unsigned release_count;
static unsigned unlock_count;
static int lock_held;
static int minor_number;
const char * USFS_DRIVER_NAME = "usfs";

static int fake_sysconfig (const int command, void * data, const int length)
{
    errno = EIO;

    if (command == SYS_QUERYLOAD)
    {
        struct cfg_load * query = data;
        assert (length == sizeof (*query));
        query_count++;

        if (fault == QUERY_LOAD_FAILURE || (fault == VERIFY_QUERY_FAILURE && query_count > 1))
            return -1;

        query->kmid = resident ? 123 : 0;
        return 0;
    }

    if (command == SYS_KLOAD)
    {
        load_count++;
        return fault == LOAD_FAILURE ? -1 : 0;
    }

    if (command == SYS_KULOAD)
    {
        const struct cfg_load * unload = data;
        assert (lock_held && unload->kmid == 123 && release_count == 0);
        unload_count++;

        if (fault == UNLOAD_FAILURE)
            return -1;

        resident = fault == STILL_LOADED;
        return 0;
    }

    assert (command == SYS_CFGDD);
    const struct cfg_dd * request = data;
    assert (length == sizeof (*request) && request->kmid == 123);

    if (request->cmd == USFS_PROXY_QUERY)
    {
        assert (request->ddslen == sizeof (resident_status));

        if (fault == QUERY_STATUS_FAILURE)
            return -1;

        if (fault == UNINITIALIZED_PROXY)
        {
            errno = ENODEV;
            return -1;
        }

        memcpy (request->ddsptr, &resident_status, sizeof (resident_status));
        return 0;
    }

    assert (request->ddsptr == NULL && request->ddslen == 0);

    if (request->cmd == CFG_INIT)
    {
        assert (request->devno == 41);
        initialize_count++;
        return fault == INITIALIZE_FAILURE ? -1 : 0;
    }

    assert (request->cmd == USFS_PROXY_UNSAFE_TERM && lock_held);
    assert (request->devno == resident_status.device && unload_count == 0);
    terminate_count++;
    return fault == TERMINATE_FAILURE ? -1 : 0;
}

int usfs_kext_is_loaded (void)
{
    return implementation_state;
}

int usfs_config_lock_acquire (void)
{
    if (fault == LOCK_FAILURE)
        return -1;

    lock_held = true;
    return 7;
}

int usfs_config_lock_release (const int descriptor)
{
    assert (descriptor == 7 && lock_held);
    unlock_count++;
    lock_held = false;
    return fault == UNLOCK_FAILURE ? -1 : 0;
}

int genmajor (char * driver_name)
{
    assert (strcmp (driver_name, USFS_DRIVER_NAME) == 0);
    assert (lock_held && !resident && query_count == 2);
    return fault == MAJOR_FAILURE ? -1 : 41;
}

int * getminor (int major_number, int * count, char * driver_name)
{
    assert (major_number == 41 && strcmp (driver_name, USFS_DRIVER_NAME) == 0);

    if (fault == MINOR_QUERY_FAILURE)
        return NULL;

    if (fault == NO_MINOR || fault == RELEASE_MAJOR_FAILURE)
    {
        *count = 0;
        return NULL;
    }

    *count = fault == EXTRA_MINOR ? 2 : 1;
    return &minor_number;
}

int reldevno (char * driver_name, int release_major)
{
    assert (lock_held && !resident && release_major == 1);
    assert (strcmp (driver_name, USFS_DRIVER_NAME) == 0);
    release_count++;
    return fault == RELEASE_DEVICE_FAILURE ? -1 : 0;
}

int relmajor (char * driver_name)
{
    assert (lock_held && !resident && strcmp (driver_name, USFS_DRIVER_NAME) == 0);
    release_count++;
    return fault == RELEASE_MAJOR_FAILURE ? -1 : 0;
}

static char * duplicate_driver_name (const char * name)
{
    if (fault == ALLOCATION_FAILURE)
        return NULL;

    return strdup (name);
}

// Exercise production control code without loading modules or modifying ODM.
#define sysconfig fake_sysconfig
#define strdup duplicate_driver_name
#include "../../../src/common/proxy_control.c"
#undef strdup
#undef sysconfig

static void reset_fixture (const enum fixture_fault requested_fault)
{
    fault = requested_fault;
    memset (&resident_status, 0, sizeof (resident_status));
    resident_status.abi = USFS_PROXY_ABI;
    resident_status.size = sizeof (resident_status);
    resident_status.device = 41;
    resident = true;
    implementation_state = USFS_QUERY_ABSENT;
    query_count = 0;
    load_count = 0;
    initialize_count = 0;
    terminate_count = 0;
    unload_count = 0;
    release_count = 0;
    unlock_count = 0;
    lock_held = false;

    switch (fault)
    {
        case WRONG_ABI: resident_status.abi++; break;
        case WRONG_SIZE: resident_status.size--; break;
        case UNTERMINATED_PROXY_BUILD:
            memset (resident_status.proxy_build, 'x', sizeof (resident_status.proxy_build));
            break;
        case UNTERMINATED_IMPLEMENTATION_BUILD:
            memset (resident_status.implementation_build, 'x', sizeof (resident_status.implementation_build));
            break;
        case UNTERMINATED_VARIANT:
            memset (resident_status.variant, 'x', sizeof (resident_status.variant));
            break;
        default: break;
    }
}

static void verify_queries (struct tap_state * tap)
{
    for (enum fixture_fault query_fault = QUERY_LOAD_FAILURE; query_fault <= UNTERMINATED_VARIANT; query_fault++)
    {
        reset_fixture (query_fault);
        mid_t module = 999;
        struct usfs_proxy_status status = { 0 };
        const int expected = query_fault == UNINITIALIZED_PROXY ? USFS_QUERY_PRESENT : USFS_QUERY_ERROR;
        const int rc = usfs_proxy_query_user (&module, &status);

        tap_ok (tap, rc == expected && module == (query_fault == QUERY_LOAD_FAILURE ? 0 : 123),
                "query validates transport, initialization, ABI, size, and each bounded string");
    }

    reset_fixture (NO_FAULT);
    tap_ok (tap, usfs_proxy_ensure_loaded () == USFS_SUCCESS && load_count == 0, "resident proxy is reused");
    resident = false;
    tap_ok (tap, usfs_proxy_ensure_loaded () == USFS_SUCCESS && load_count == 1, "absent proxy is loaded once");
    fault = LOAD_FAILURE;
    tap_ok (tap, usfs_proxy_ensure_loaded () == USFS_FAILURE, "load failure is returned");
    reset_fixture (WRONG_ABI);
    tap_ok (tap, usfs_proxy_ensure_loaded () == USFS_FAILURE && load_count == 0, "incompatible proxy is never replaced implicitly");

    reset_fixture (NO_FAULT);
    tap_ok (tap, usfs_proxy_prepare_device (41) == USFS_SUCCESS && initialize_count == 1, "unregistered proxy initializes the requested device");
    fault = INITIALIZE_FAILURE;
    tap_ok (tap, usfs_proxy_prepare_device (41) == USFS_FAILURE, "device initialization failure is returned");
    resident_status.registered = 1;
    resident_status.pager_registered = 1;
    tap_ok (tap, usfs_proxy_prepare_device (41) == USFS_SUCCESS && initialize_count == 2, "matching registered device needs no initialization");
    tap_ok (tap, usfs_proxy_prepare_device (42) == USFS_FAILURE, "a foreign device number is rejected");
    resident_status.pager_registered = 0;
    fault = NO_FAULT;
    tap_ok (tap, usfs_proxy_prepare_device (41) == USFS_SUCCESS && initialize_count == 3, "incomplete pager registration is retried");
    resident = false;
    tap_ok (tap, usfs_proxy_prepare_device (41) == USFS_FAILURE, "absent proxy cannot prepare a device");
}

static void verify_routing (struct tap_state * tap)
{
    reset_fixture (NO_FAULT);
    tap_ok (tap, usfs_proxy_detached () && usfs_proxy_control_module (456) == 456, "empty proxy delegates control to implementation");
    resident_status.calls = 1;
    tap_ok (tap, !usfs_proxy_detached (), "outstanding dispatch prevents detached status");
    resident_status.calls = 0;
    resident_status.state = USFS_PROXY_EMPTY + 1;
    tap_ok (tap, !usfs_proxy_detached () && usfs_proxy_control_module (456) == 123, "attached proxy receives control");
    resident = false;
    tap_ok (tap, usfs_proxy_detached () && usfs_proxy_control_module (456) == 456, "absent proxy uses implementation control");
    fault = QUERY_LOAD_FAILURE;
    tap_ok (tap, !usfs_proxy_detached () && usfs_proxy_control_module (456) == 0, "query errors never claim safe detachment or route control");
}

static void verify_teardown (struct tap_state * tap)
{
    for (enum fixture_fault teardown_fault = LOCK_FAILURE; teardown_fault <= RELEASE_MAJOR_FAILURE; teardown_fault++)
    {
        reset_fixture (teardown_fault);
        const int expected = teardown_fault == NO_MINOR ? USFS_SUCCESS : USFS_FAILURE;
        const int rc = usfs_proxy_unsafe_unload ();

        tap_ok (tap, rc == expected && !lock_held && unlock_count == (teardown_fault != LOCK_FAILURE),
                "teardown failures preserve ordering and release the configuration lock");
    }

    reset_fixture (NO_FAULT);
    tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_SUCCESS && terminate_count == 1 && unload_count == 1 && release_count == 1,
            "device numbers are released only after verified module removal");
    reset_fixture (UNINITIALIZED_PROXY);
    tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_SUCCESS && terminate_count == 0 && release_count == 1,
            "uninitialized proxy can be removed without sending termination");
    reset_fixture (NO_FAULT);
    resident = false;
    tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_SUCCESS && unload_count == 0 && release_count == 0, "absent proxy removal is idempotent");

    const int implementation_states[] = { USFS_QUERY_PRESENT, USFS_QUERY_ERROR };
    for (unsigned state_index = 0; state_index < sizeof (implementation_states) / sizeof (implementation_states[0]); state_index++)
    {
        reset_fixture (NO_FAULT);
        implementation_state = implementation_states[state_index];
        tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_FAILURE && terminate_count == 0 && unlock_count == 1,
                "loaded or unqueryable implementation prevents proxy removal");
    }

    reset_fixture (QUERY_STATUS_FAILURE);
    tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_FAILURE && unload_count == 0, "unqueryable proxy is not unloaded");
    reset_fixture (NO_FAULT);
    resident_status.state = USFS_PROXY_EMPTY + 1;
    tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_FAILURE && terminate_count == 0, "attached proxy is not terminated");
    resident_status.state = USFS_PROXY_EMPTY;
    resident_status.calls = 1;
    tap_ok (tap, usfs_proxy_unsafe_unload () == USFS_FAILURE && terminate_count == 0, "active dispatch prevents termination");
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, 44);
    verify_queries (&tap);
    verify_routing (&tap);
    verify_teardown (&tap);

    return tap_finish (&tap);
}
